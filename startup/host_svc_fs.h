#ifndef TOTK_HOST_SVC_FS_H
#define TOTK_HOST_SVC_FS_H
/* Native file system: host functions for the game's nn::fs calls on its RomFS (design:
 * startup/svc-work/DESIGN.md, stage 1). The functions are registered with the shared host-function
 * registry (host_fn.h, owned by the NVN work) as guest-entry hooks (boot_host_fn_hook) on the sdk
 * functions below; a hook that declines runs the original translated SDK code. No IPC message is
 * built or parsed.
 *
 * 1a (fs.storage): nn::fs::detail::StorageServiceObjectAdapter<nn::fssrv::sf::IStorage>::Read and
 *    ::GetSize (sdk+0xabc64 / +0xabce4) read the RomFS image (c->storage_native) directly.
 *    Invariant: the runtime creates fssrv IStorage objects only for OpenDataStorageByCurrentProcess
 *    (fsp-srv 200, the RomFS image), so every such adapter is the RomFS storage. If another
 *    IStorage-creating command is ever implemented, this check must learn to tell them apart.
 * 1b (fs.api): nn::fs::OpenFile / ReadFile (4 overloads) / GetFileSize / CloseFile / GetFileOpenMode /
 *    GetEntryType for paths on a RomFS mount. The mount is recognised statelessly from the guest's
 *    own mount table (nn::fs::detail::MountTable at sdk+0xd1c8a0): an accessor of that name whose
 *    IFileSystem is an nn::fs::RomFsFileSystem over a StorageServiceObjectAdapter. The guest keeps
 *    its MountRom (one fsp request; its storage reads are native through 1a), so every case not
 *    handled here falls through to the original guest code and gives the exact SDK result.
 *    A native FileHandle is 0xF5 << 56 | 1 << 52 | <file entry offset in the RomFS file table>:
 *    it carries all its state, survives checkpoints and DLL swaps, and can never equal a guest
 *    FileAccessor pointer (39-bit addresses). Every other exported sdk function that takes a
 *    FileHandle has a guard: native handle -> fixed result or a clean stop, guest handle -> through.
 * Shadow mode (TOTK_NATIVE_MODE=shadow): the native answer is computed, the original SDK path
 * runs (boot_host_return_to -> continuation svc.fs.shadow.ret), and the two are compared on return.
 * Mismatches, a summary every 1024 comparisons, native error results and guard stops go to
 * native-svc.log (process working directory).
 *
 * Switches (environment, read once per process): TOTK_NATIVE_SERVICES=all|fs|fs.storage|fs.api
 * (comma list; default none: every function falls through), TOTK_NATIVE_MODE=native|shadow,
 * TOTK_SVC_FS_LOOSE=1 accepts a RomFS mount whose base storage is not the adapter.
 * Offsets are for the pinned sdk build (the runtime checks its SHA-256 at load). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define SVC_FS_SDK_MOUNT_TABLE   UINT64_C(0xd1c8a0)  /* nn::fs::detail::MountTable (Find: sdk+0xa1610) */
#define SVC_FS_SDK_VT_ROMFS      UINT64_C(0xcb7588)  /* _ZTVN2nn2fs15RomFsFileSystemE */
#define SVC_FS_SDK_VT_ADAPTER    UINT64_C(0xcb46c8)  /* _ZTVN2nn2fs6detail27StorageServiceObjectAdapterINS_5fssrv2sf8IStorageEEE */
#define SVC_FS_ACCESSOR_NAME     0x18                /* FileSystemAccessor: char name[16] (ctor sdk+0x995f0) */
#define SVC_FS_ACCESSOR_FS       0x28                /* FileSystemAccessor: unique_ptr<fsa::IFileSystem> */
#define SVC_FS_ROMFS_BASE        0x118               /* RomFsFileSystem::GetBaseStorage (sdk+0xe3b80) */
#define SVC_FS_RESULT_OUT_OF_RANGE        UINT32_C(0x177a02)  /* fs 3005 (nn::fs::ReadFile short read, sdk+0x9c5e0) */
#define SVC_FS_RESULT_NULLPTR_ARGUMENT    UINT32_C(0x2f5e02)  /* fs 6063 (MountTable::Find, sdk+0x9ae3c) */
#define SVC_FS_RESULT_WRITE_NOT_PERMITTED UINT32_C(0x307602)  /* fs 6203 */
#define SVC_FS_HANDLE_TAG        UINT64_C(0xF5)
#define SVC_FS_HANDLE_FILE       UINT64_C(1)
#define SVC_FS_STORAGE_LIMIT     (UINT64_C(64) << 20)  /* the IPC IStorage::Read limit: larger reads keep the old path */
#define SVC_FS_PATH_MAX          0x301                 /* entry name length max 0x300 + NUL */
#define SVC_FS_STEPS             64                    /* nominal guest steps charged per native call */

/* ---------------------------------------------------------------- configuration */
typedef struct SvcFsConfig { int storage, api, shadow, loose; } SvcFsConfig;
static inline int svc_fs_token(const char *list, const char *token) {
    size_t n = strlen(token);
    for (const char *p = list; p && *p;) {
        while (*p == ',' || *p == ' ') ++p;
        const char *e = p; while (*e && *e != ',' && *e != ' ') ++e;
        if ((size_t)(e - p) == n && !_strnicmp(p, token, n)) return 1;
        p = e;
    }
    return 0;
}
static inline SvcFsConfig *svc_fs_config_slot(void) { static SvcFsConfig cfg; return &cfg; }
static inline void svc_fs_config_load(void) {
    SvcFsConfig *cfg = svc_fs_config_slot();
    const char *s = getenv("TOTK_NATIVE_SERVICES"), *m = getenv("TOTK_NATIVE_MODE"), *l = getenv("TOTK_SVC_FS_LOOSE");
#if defined(TOTK_PLAY) && TOTK_PLAY
    if (!s) s = "all";   /* play build: native services on unless the variable says otherwise */
#endif
    int all = svc_fs_token(s, "all") || svc_fs_token(s, "fs");
    cfg->storage = all || svc_fs_token(s, "fs.storage");
    cfg->api = all || svc_fs_token(s, "fs.api");
    cfg->shadow = m && !_stricmp(m, "shadow");
    cfg->loose = l && *l == '1';
}
static inline const SvcFsConfig *svc_fs_config(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        svc_fs_config_load(); InitOnceComplete(&once, 0, NULL);
    }
    return svc_fs_config_slot();
}

/* ---------------------------------------------------------------- non-faulting guest peeks */
/* Object-graph reads (mount table, vtables, path strings) go straight to the region table and
 * never fault: a miss means "not ours" and the call falls through to the guest code. */
static inline const uint8_t *svc_fs_region_ptr(const BootContext *c, uint64_t a, uint64_t n) {
    if (boot_mt_core_ctx(c)) return boot_mt_peek_ptr(c, a, n);   /* TOTK_CORES>1: the core's table is a stale snapshot */
    for (unsigned i = 0; i < c->region_count && i < BOOT_REGION_CAPACITY; ++i) {
        const Region *r = &c->regions[i];
        if (r->data && a >= r->base && a - r->base < r->size && n <= r->size - (a - r->base)) return r->data + (size_t)(a - r->base);
    }
    return NULL;
}
static inline int svc_fs_peek(const BootContext *c, uint64_t a, void *out, size_t n) {
    uint8_t *d = (uint8_t *)out;
    while (n) {
        size_t chunk = 4096 - (size_t)(a & 4095); if (chunk > n) chunk = n;
        const uint8_t *p = svc_fs_region_ptr(c, a, chunk); if (!p) return 0;
        memcpy(d, p, chunk); d += chunk; a += chunk; n -= chunk;
    }
    return 1;
}
static inline int svc_fs_peek_u64(const BootContext *c, uint64_t a, uint64_t *v) { return svc_fs_peek(c, a, v, 8); }
/* NUL-terminated string of at most cap-1 bytes; 0 when unmapped or unterminated. */
static inline int svc_fs_peek_string(const BootContext *c, uint64_t a, char *buf, size_t cap) {
    for (size_t n = 0; n < cap; ++n) {
        const uint8_t *p = svc_fs_region_ptr(c, a + n, 1); if (!p) return 0;
        buf[n] = (char)*p;
        if (!*p) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- RomFS image index */
typedef struct SvcRomfs {
    const void *owner;          /* the BootStorage this index was built from */
    HANDLE file;                /* own handle (ReOpenFile): positional reads, no shared file pointer */
    uint64_t image_size, data_offset;
    uint8_t *dir_hash, *dir_meta, *file_hash, *file_meta;
    uint64_t dir_hash_size, dir_meta_size, file_hash_size, file_meta_size;
} SvcRomfs;
typedef struct SvcRomfsSlot { SRWLOCK lock; SvcRomfs *index; } SvcRomfsSlot;
static inline SvcRomfsSlot *svc_romfs_slot(void) { static SvcRomfsSlot s = {SRWLOCK_INIT, NULL}; return &s; }
static inline uint32_t svc_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t svc_le64(const uint8_t *p) { return (uint64_t)svc_le32(p) | (uint64_t)svc_le32(p + 4) << 32; }
static inline int svc_romfs_pread(HANDLE f, uint64_t offset, void *buffer, uint32_t size) {
    OVERLAPPED o; memset(&o, 0, sizeof o);
    o.Offset = (DWORD)offset; o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    return ReadFile(f, buffer, size, &got, &o) && got == size;
}
static inline void svc_romfs_free(SvcRomfs *r) {
    if (!r) return;
    if (r->file && r->file != INVALID_HANDLE_VALUE) CloseHandle(r->file);
    free(r->dir_hash); free(r->dir_meta); free(r->file_hash); free(r->file_meta); free(r);
}
static inline SvcRomfs *svc_romfs_build(const BootStorage *s) {
    SvcRomfs *r = (SvcRomfs *)calloc(1, sizeof *r); if (!r) return NULL;
    r->owner = s; r->image_size = s->size;
    r->file = ReOpenFile(s->file, GENERIC_READ, FILE_SHARE_READ, 0);
    uint8_t h[0x50];
    if (r->file == INVALID_HANDLE_VALUE || !svc_romfs_pread(r->file, 0, h, sizeof h) || svc_le64(h) != 0x50) goto bad;
    uint64_t *sizes[4] = {&r->dir_hash_size, &r->dir_meta_size, &r->file_hash_size, &r->file_meta_size};
    uint8_t **tables[4] = {&r->dir_hash, &r->dir_meta, &r->file_hash, &r->file_meta};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t off = svc_le64(h + 8 + 16 * i), len = svc_le64(h + 16 + 16 * i);
        if (off > r->image_size || len > r->image_size - off || len > (UINT64_C(1) << 31) || (len & 3)) goto bad;
        *sizes[i] = len;
        if (!(*tables[i] = (uint8_t *)malloc(len ? (size_t)len : 1))) goto bad;
        if (len && !svc_romfs_pread(r->file, off, *tables[i], (uint32_t)len)) goto bad;
    }
    r->data_offset = svc_le64(h + 0x48);
    if (r->data_offset > r->image_size || r->dir_meta_size < 0x18 || !r->dir_hash_size || !r->file_hash_size) goto bad;
    return r;
bad:
    svc_romfs_free(r); return NULL;
}
/* Shared-locked index for c's RomFS image (built on first use, rebuilt if the image changes);
 * the caller releases with svc_romfs_release. NULL when there is no usable image. */
static inline SvcRomfs *svc_romfs_acquire(const BootContext *c) {
    const BootStorage *s = (const BootStorage *)(uintptr_t)c->storage_native;
    if (!s) return NULL;
    SvcRomfsSlot *slot = svc_romfs_slot();
    AcquireSRWLockShared(&slot->lock);
    if (slot->index && slot->index->owner == s) return slot->index;
    ReleaseSRWLockShared(&slot->lock);
    AcquireSRWLockExclusive(&slot->lock);
    if (!slot->index || slot->index->owner != s) {
        SvcRomfs *built = svc_romfs_build(s);
        if (built) { svc_romfs_free(slot->index); slot->index = built; }
    }
    int ok = slot->index && slot->index->owner == s;
    ReleaseSRWLockExclusive(&slot->lock);
    if (!ok) return NULL;
    AcquireSRWLockShared(&slot->lock);
    if (slot->index && slot->index->owner == s) return slot->index;
    ReleaseSRWLockShared(&slot->lock);
    return NULL;
}
static inline void svc_romfs_release(void) { ReleaseSRWLockShared(&svc_romfs_slot()->lock); }

static inline uint32_t svc_romfs_hash(uint32_t parent, const char *name, size_t len) {
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < len; ++i) { h = (h >> 5) | (h << 27); h ^= (uint8_t)name[i]; }
    return h;
}
/* Directory entry: parent, sibling, child dir, child file, hash next, name size, name.
 * File entry: parent, sibling, u64 data offset, u64 size, hash next, name size, name. */
static inline int svc_romfs_find(const SvcRomfs *r, int file, uint32_t parent, const char *name, size_t len, uint32_t *out) {
    const uint8_t *hash = file ? r->file_hash : r->dir_hash, *meta = file ? r->file_meta : r->dir_meta;
    uint64_t buckets = (file ? r->file_hash_size : r->dir_hash_size) / 4, size = file ? r->file_meta_size : r->dir_meta_size;
    unsigned head = file ? 0x20 : 0x18, next_at = file ? 0x18 : 0x10;
    if (!buckets) return 0;
    uint32_t at = svc_le32(hash + 4 * (svc_romfs_hash(parent, name, len) % buckets));
    for (unsigned guard = 0; at != UINT32_MAX && guard < 1u << 20; ++guard) {
        if ((at & 3) || at + (uint64_t)head > size) return 0;
        const uint8_t *e = meta + at;
        uint32_t name_size = svc_le32(e + head - 4);
        if (at + (uint64_t)head + name_size > size) return 0;
        if (svc_le32(e) == parent && name_size == len && !memcmp(e + head, name, len)) { *out = at; return 1; }
        at = svc_le32(e + next_at);
    }
    return 0;
}
static inline int svc_romfs_file(const SvcRomfs *r, uint32_t entry, uint64_t *image_offset, uint64_t *size) {
    if ((entry & 3) || entry + UINT64_C(0x20) > r->file_meta_size) return 0;
    const uint8_t *e = r->file_meta + entry;
    uint64_t off = svc_le64(e + 8), len = svc_le64(e + 16);
    if (off > r->image_size - r->data_offset || len > r->image_size - r->data_offset - off) return 0;
    *image_offset = r->data_offset + off; *size = len; return 1;
}
/* Splits "<mount>:/<path>" (pointer into path). Only plain normalized forms are accepted: single '/'
 * separators, no empty, "." or ".." component, no trailing '/' (except the root "/"), no '\\' or
 * characters the SDK path checker rejects. Anything else -> 0 (the guest decides). */
static inline int svc_fs_split(const char *path, char *mount, const char **rest) {
    const char *colon = strchr(path, ':');
    if (!colon || colon == path || colon - path > 15 || colon[1] != '/') return 0;
    memcpy(mount, path, (size_t)(colon - path)); mount[colon - path] = 0;
    for (const char *p = path; p < colon; ++p) if ((uint8_t)*p < 0x21 || *p == '/' || *p == '\\') return 0;
    const char *p = colon + 1;
    if (strlen(p) >= SVC_FS_PATH_MAX) return 0;
    if (p[1]) {
        for (const char *q = p; *q; ) {
            if (*q != '/') return 0;
            const char *e = ++q; while (*e && *e != '/') ++e;
            size_t n = (size_t)(e - q);
            if (!n || (n == 1 && q[0] == '.') || (n == 2 && q[0] == '.' && q[1] == '.')) return 0;
            for (const char *k = q; k < e; ++k) {
                uint8_t ch = (uint8_t)*k;
                if (ch < 0x20 || ch == 0x7f || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|' || ch == '"') return 0;
            }
            q = e;
        }
    }
    *rest = p; return 1;
}
/* Resolves "/a/b/c" (or "/" = root directory). Returns 1 file, 2 directory, 0 not found. */
static inline int svc_romfs_resolve(const SvcRomfs *r, const char *p, uint32_t *entry) {
    uint32_t dir = 0;
    if (!p[1]) { *entry = 0; return 2; }
    for (const char *q = p + 1;;) {
        const char *e = strchr(q, '/');
        size_t n = e ? (size_t)(e - q) : strlen(q);
        if (!e) {
            if (svc_romfs_find(r, 1, dir, q, n, entry)) return 1;
            if (svc_romfs_find(r, 0, dir, q, n, entry)) return 2;
            return 0;
        }
        if (!svc_romfs_find(r, 0, dir, q, n, &dir)) return 0;
        q = e + 1;
    }
}

/* ---------------------------------------------------------------- the guest's mount table */
/* 1 when `mount` is a RomFS mount over the data storage in the guest's nn::fs mount table. */
static inline int svc_fs_rom_mount(const BootContext *c, const char *mount) {
    uint64_t sdk = c->code_bases[3], table = sdk + SVC_FS_SDK_MOUNT_TABLE, node = 0;
    if (!sdk || !svc_fs_peek_u64(c, table + 8, &node)) return 0;
    for (unsigned guard = 0; node && node != table && guard < 256; ++guard) {
        uint64_t accessor = node - 8; char name[17] = {0};
        if (!svc_fs_peek(c, accessor + SVC_FS_ACCESSOR_NAME, name, 16)) return 0;
        if (!strncmp(name, mount, 16)) {
            uint64_t fs = 0, vptr = 0, base = 0, base_vptr = 0;
            if (!svc_fs_peek_u64(c, accessor + SVC_FS_ACCESSOR_FS, &fs) || !fs || !svc_fs_peek_u64(c, fs, &vptr) ||
                vptr != sdk + SVC_FS_SDK_VT_ROMFS + 16) return 0;
            if (svc_fs_config()->loose) return 1;
            return svc_fs_peek_u64(c, fs + SVC_FS_ROMFS_BASE, &base) && base && svc_fs_peek_u64(c, base, &base_vptr) &&
                   base_vptr == sdk + SVC_FS_SDK_VT_ADAPTER + 16;
        }
        if (!svc_fs_peek_u64(c, node + 8, &node)) return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- handles and data */
static inline uint64_t svc_fs_handle(uint32_t entry) { return SVC_FS_HANDLE_TAG << 56 | SVC_FS_HANDLE_FILE << 52 | entry; }
static inline int svc_fs_is_native(uint64_t h) { return (h >> 56) == SVC_FS_HANDLE_TAG; }
static inline int svc_fs_handle_entry(uint64_t h, uint32_t *entry) {
    if (!svc_fs_is_native(h) || ((h >> 52) & 15) != SVC_FS_HANDLE_FILE || (h & UINT64_C(0x000fffff00000000))) return 0;
    *entry = (uint32_t)h; return 1;
}
static inline uint32_t svc_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256]; static volatile LONG ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t v = i; for (int k = 0; k < 8; ++k) v = v & 1 ? (v >> 1) ^ 0xedb88320u : v >> 1; table[i] = v; }
        MemoryBarrier(); ready = 1;
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}
/* Copies image bytes [offset, offset+size) to guest memory (or only checksums them when guest==0).
 * 1 ok; 0 host read error (nothing is reported to the guest: caller falls through or stops);
 * -1 guest fault (c->status set at self; the call is restartable: it rewrites the same bytes). */
static inline int svc_fs_copy(BootContext *c, uint64_t self, const SvcRomfs *r, uint64_t offset, uint64_t size, uint64_t guest, uint32_t *crc) {
    enum { CHUNK = 1 << 20 };
    if (!size) return 1;
    uint8_t *buffer = (uint8_t *)malloc(size < CHUNK ? (size_t)size : CHUNK); if (!buffer) return 0;
    int rc = 1;
    for (uint64_t done = 0; done < size;) {
        uint32_t n = (uint32_t)(size - done < CHUNK ? size - done : CHUNK);
        if (!svc_romfs_pread(r->file, offset + done, buffer, n)) { rc = 0; break; }
        if (crc) *crc = svc_crc32(*crc, buffer, n);
        if (guest && !boot_host_write(c, self, guest + done, buffer, n)) { rc = -1; break; }
        done += n;
    }
    free(buffer);
    if (guest && rc == 1) c->exclusive_valid = 0;
    return rc;
}
static inline uint32_t svc_fs_guest_crc(BootContext *c, uint64_t a, uint64_t n, int *ok) {
    uint32_t crc = 0; uint8_t buffer[4096];
    while (n) {
        size_t chunk = 4096 - (size_t)(a & 4095); if (chunk > n) chunk = (size_t)n;
        if (!svc_fs_peek(c, a, buffer, chunk)) { *ok = 0; return 0; }
        crc = svc_crc32(crc, buffer, chunk); a += chunk; n -= chunk;
    }
    return crc;
}
static inline void svc_fs_log(const char *fmt, ...) {
    static SRWLOCK lock = SRWLOCK_INIT;
    va_list ap; va_start(ap, fmt);
    AcquireSRWLockExclusive(&lock);
    FILE *f = fopen("native-svc.log", "a");
    if (f) { vfprintf(f, fmt, ap); fputc('\n', f); fclose(f); }
    ReleaseSRWLockExclusive(&lock);
    va_end(ap);
}

/* ---------------------------------------------------------------- shadow bookkeeping */
/* A pending comparison: one slot per in-flight shadowed call, identified in the guest frame by
 * (slot, generation). After a DLL swap the table is empty and the continuation just returns. */
enum { SVC_SHADOW_STORAGE_READ = 1, SVC_SHADOW_STORAGE_SIZE, SVC_SHADOW_OPEN, SVC_SHADOW_READ, SVC_SHADOW_SIZE, SVC_SHADOW_ENTRY_TYPE };
typedef struct SvcFsShadow {
    volatile LONG used; uint32_t kind, expect_result, entry;
    uint64_t out, buffer, size, expect_value; uint32_t expect_crc; int no_out;
} SvcFsShadow;
typedef struct SvcFsShadowState {
    SvcFsShadow slot[256]; uint64_t generation;
    SRWLOCK map_lock; struct { uint64_t guest; uint32_t entry; } map[1024];   /* guest FileHandle -> file entry */
    volatile LONG64 compared, mismatched;
} SvcFsShadowState;
static inline SvcFsShadowState *svc_fs_shadow_state(void) {
    static SvcFsShadowState s; static INIT_ONCE once = INIT_ONCE_STATIC_INIT; BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        s.generation = ((uint64_t)t.QuadPart ^ GetCurrentProcessId()) & UINT64_C(0xffffffffffff);
        InitializeSRWLock(&s.map_lock); InitOnceComplete(&once, 0, NULL);
    }
    return &s;
}
static inline void svc_fs_map_set(uint64_t guest, uint32_t entry, int add) {
    SvcFsShadowState *s = svc_fs_shadow_state();
    AcquireSRWLockExclusive(&s->map_lock);
    unsigned free_slot = 1024;
    for (unsigned i = 0; i < 1024; ++i) {
        if (s->map[i].guest == guest) { if (add) s->map[i].entry = entry; else s->map[i].guest = 0; free_slot = 2048; break; }
        if (!s->map[i].guest && free_slot == 1024) free_slot = i;
    }
    if (add && free_slot < 1024) { s->map[free_slot].guest = guest; s->map[free_slot].entry = entry; }
    ReleaseSRWLockExclusive(&s->map_lock);
}
static inline int svc_fs_map_get(uint64_t guest, uint32_t *entry) {
    SvcFsShadowState *s = svc_fs_shadow_state(); int hit = 0;
    if (!guest) return 0;
    AcquireSRWLockShared(&s->map_lock);
    for (unsigned i = 0; i < 1024; ++i) if (s->map[i].guest == guest) { *entry = s->map[i].entry; hit = 1; break; }
    ReleaseSRWLockShared(&s->map_lock);
    return hit;
}
static inline uint64_t svc_fs_address(const char *name) { return boot_host_fn_address(name); }
/* Pushes the comparison frame and sends the call down the original path. Returns BOOT_HOST_DECLINE
 * (or STOP on a guest stack fault, before anything changed). */
static inline int svc_fs_shadow_begin(BootContext *c, uint64_t self, const SvcFsShadow *expect) {
    SvcFsShadowState *s = svc_fs_shadow_state();
    for (unsigned i = 0; i < 256; ++i) {
        if (InterlockedCompareExchange(&s->slot[i].used, 1, 0)) continue;
        SvcFsShadow copy = *expect; copy.used = 1; s->slot[i] = copy;   /* used stays 1 (claimed above) */
        if (!boot_host_return_to(c, self, svc_fs_address("svc.fs.shadow.ret"), i, s->generation)) { s->slot[i].used = 0; return BOOT_HOST_STOP; }
        return BOOT_HOST_DECLINE;
    }
    return BOOT_HOST_DECLINE;   /* table full: unshadowed */
}
static inline void svc_fs_shadow_report(int mismatch, const char *what, const SvcFsShadow *e, uint64_t got_result, uint64_t got_value, uint32_t got_crc) {
    SvcFsShadowState *s = svc_fs_shadow_state();
    LONG64 n = InterlockedIncrement64(&s->compared), bad = mismatch ? InterlockedIncrement64(&s->mismatched) : s->mismatched;
    if (mismatch && bad <= 1000)
        svc_fs_log("MISMATCH %s entry=%#x size=%#llx expect result=%#x value=%#llx crc=%08x | guest result=%#llx value=%#llx crc=%08x",
                   what, e->entry, (unsigned long long)e->size, e->expect_result, (unsigned long long)e->expect_value, e->expect_crc,
                   (unsigned long long)got_result, (unsigned long long)got_value, got_crc);
    if (!(n & 1023)) svc_fs_log("summary compared=%lld mismatched=%lld", (long long)n, (long long)bad);
}
/* svc.fs.shadow.ret: the original SDK function returned here. Compare, then continue at its caller. */
static inline int svc_fs_shadow_ret(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user;
    uint64_t index = 0, generation = 0;
    if (!boot_host_frame_pop(c, self, target, &index, &generation)) return BOOT_HOST_STOP;
    SvcFsShadowState *s = svc_fs_shadow_state();
    if (generation != s->generation || index >= 256 || !s->slot[index].used) return BOOT_HOST_DONE;
    SvcFsShadow e = s->slot[index]; s->slot[index].used = 0;
    uint32_t result = (uint32_t)c->x[0]; int ok = 1;
    switch (e.kind) {
    case SVC_SHADOW_STORAGE_READ: {
        uint32_t crc = result ? 0 : svc_fs_guest_crc(c, e.buffer, e.size, &ok);
        svc_fs_shadow_report(!ok || result || crc != e.expect_crc, "StorageAdapter.Read", &e, result, 0, crc); break; }
    case SVC_SHADOW_STORAGE_SIZE: {
        uint64_t v = 0; if (!result) ok = svc_fs_peek_u64(c, e.out, &v);
        svc_fs_shadow_report(!ok || result || v != e.expect_value, "StorageAdapter.GetSize", &e, result, v, 0); break; }
    case SVC_SHADOW_OPEN: {
        uint64_t h = 0; if (!result) ok = svc_fs_peek_u64(c, e.out, &h);
        if (!result && ok && h) svc_fs_map_set(h, e.entry, 1);
        svc_fs_shadow_report(!ok || result != e.expect_result, "OpenFile", &e, result, h, 0); break; }
    case SVC_SHADOW_READ: {
        uint64_t n = e.expect_value;
        if (!e.no_out && !result) ok = svc_fs_peek_u64(c, e.out, &n);
        uint32_t crc = (!result && ok) ? svc_fs_guest_crc(c, e.buffer, n, &ok) : 0;
        int bad = !ok || result != e.expect_result || (!result && (n != e.expect_value || crc != e.expect_crc));
        svc_fs_shadow_report(bad, "ReadFile", &e, result, n, crc); break; }
    case SVC_SHADOW_SIZE: {
        uint64_t v = 0; if (!result) ok = svc_fs_peek_u64(c, e.out, &v);
        svc_fs_shadow_report(!ok || result != e.expect_result || (!result && v != e.expect_value), "GetFileSize", &e, result, v, 0); break; }
    case SVC_SHADOW_ENTRY_TYPE: {
        uint64_t v = 0; uint32_t t = 0; if (!result) { ok = svc_fs_peek(c, e.out, &t, 4); v = t; }
        svc_fs_shadow_report(!ok || result != e.expect_result || (!result && v != e.expect_value), "GetEntryType", &e, result, v, 0); break; }
    default: break;
    }
    return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- 1a: storage adapter */
static inline int svc_fs_is_adapter(const BootContext *c, uint64_t self_object) {
    uint64_t vptr = 0;
    return c->code_bases[3] && svc_fs_peek_u64(c, self_object, &vptr) && vptr == c->code_bases[3] + SVC_FS_SDK_VT_ADAPTER + 16;
}
/* svc-S5: play builds (TOTK_PLAY) write romfs-reads.log only with TOTK_ROMFS_READ_LOG=1 (any value but 0); other
 * builds unless =0, as before. Each logged read takes the exclusive lock and a fprintf + fflush on the streaming
 * thread (romfs-reads.log, appended by every boot, had 77,763 lines); off, a read costs one unlocked test. state: 0
 * unknown, 1 log, 2 off. */
static inline void svc_fs_read_log(uint64_t offset, uint64_t size) {
    static FILE *log; static volatile LONG state; static SRWLOCK lock = SRWLOCK_INIT;
    if (state == 2) return;
    AcquireSRWLockExclusive(&lock);
    if (!state) {
        const char *e = getenv("TOTK_ROMFS_READ_LOG");
#if defined(TOTK_PLAY) && TOTK_PLAY
        if (e && *e != '0') log = fopen("romfs-reads.log", "a");
#else
        if (!e || *e != '0') log = fopen("romfs-reads.log", "a");
#endif
        InterlockedExchange(&state, log ? 1 : 2);
    }
    if (log) { fprintf(log, "%#llx %#llx\n", (unsigned long long)offset, (unsigned long long)size); fflush(log); }
    ReleaseSRWLockExclusive(&lock);
}
/* Result StorageServiceObjectAdapter<IStorage>::Read(this x0, s64 offset x1, void *buffer x2, size_t size x3) */
static inline int svc_fs_storage_read(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    const SvcFsConfig *cfg = svc_fs_config();
    uint64_t offset = c->x[1], buffer = c->x[2], size = c->x[3];
    if (!cfg->storage || !svc_fs_is_adapter(c, c->x[0])) return BOOT_HOST_DECLINE;
    SvcRomfs *r = svc_romfs_acquire(c); if (!r) return BOOT_HOST_DECLINE;
    /* Out-of-range, oversized and null-buffer requests keep the original path (the IPC handler
     * decides them exactly as before). */
    if ((int64_t)offset < 0 || offset > r->image_size || size > r->image_size - offset || size > SVC_FS_STORAGE_LIMIT || (size && !buffer)) {
        svc_romfs_release(); return BOOT_HOST_DECLINE;
    }
    if (cfg->shadow) {
        SvcFsShadow e; memset(&e, 0, sizeof e); e.kind = SVC_SHADOW_STORAGE_READ; e.buffer = buffer; e.size = size;
        int rc = svc_fs_copy(c, self, r, offset, size, 0, &e.expect_crc);
        svc_romfs_release();
        return rc == 1 ? svc_fs_shadow_begin(c, self, &e) : BOOT_HOST_DECLINE;
    }
    int rc = svc_fs_copy(c, self, r, offset, size, buffer, NULL);
    svc_romfs_release();
    if (rc < 0) return BOOT_HOST_STOP;
    if (!rc) return BOOT_HOST_DECLINE;
    svc_fs_read_log(offset, size);
    c->x[0] = 0; return BOOT_HOST_DONE;
}
/* Result StorageServiceObjectAdapter<IStorage>::GetSize(this x0, s64 *out x1) */
static inline int svc_fs_storage_size(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    const SvcFsConfig *cfg = svc_fs_config();
    const BootStorage *s = (const BootStorage *)(uintptr_t)c->storage_native;
    if (!cfg->storage || !s || !c->x[1] || !svc_fs_is_adapter(c, c->x[0])) return BOOT_HOST_DECLINE;
    uint64_t size = s->size;
    if (cfg->shadow) {
        SvcFsShadow e; memset(&e, 0, sizeof e); e.kind = SVC_SHADOW_STORAGE_SIZE; e.out = c->x[1]; e.expect_value = size;
        return svc_fs_shadow_begin(c, self, &e);
    }
    if (!boot_host_write(c, self, c->x[1], &size, 8)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- 1b: nn::fs API on the RomFS mount */
/* Resolves a guest path on a native RomFS mount: 1 file, 2 directory, 0 = not ours / not found.
 * On success *r is acquired (caller releases). */
static inline int svc_fs_lookup(BootContext *c, uint64_t path_address, SvcRomfs **r, uint32_t *entry) {
    char path[SVC_FS_PATH_MAX + 32], mount[16]; const char *rest;
    if (!path_address || !svc_fs_peek_string(c, path_address, path, sizeof path) || !svc_fs_split(path, mount, &rest) ||
        !svc_fs_rom_mount(c, mount)) return 0;
    if (!(*r = svc_romfs_acquire(c))) return 0;
    int kind = svc_romfs_resolve(*r, rest, entry);
    if (!kind) svc_romfs_release();
    return kind;
}
/* Result nn::fs::OpenFile(FileHandle *out x0, const char *path x1, int mode w2) */
static inline int svc_fs_open_file(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    const SvcFsConfig *cfg = svc_fs_config();
    SvcRomfs *r; uint32_t entry; uint64_t data, size;
    if (!cfg->api || !c->x[0] || (uint32_t)c->x[2] != 1) return BOOT_HOST_DECLINE;
    if (svc_fs_lookup(c, c->x[1], &r, &entry) != 1) return BOOT_HOST_DECLINE;   /* missing or directory: guest result */
    int valid = svc_romfs_file(r, entry, &data, &size);
    svc_romfs_release();
    if (!valid) return BOOT_HOST_DECLINE;
    if (cfg->shadow) {
        SvcFsShadow e; memset(&e, 0, sizeof e); e.kind = SVC_SHADOW_OPEN; e.out = c->x[0]; e.entry = entry;
        return svc_fs_shadow_begin(c, self, &e);
    }
    uint64_t handle = svc_fs_handle(entry);
    if (!boot_host_write(c, self, c->x[0], &handle, 8)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}
/* The four nn::fs::ReadFile overloads (user = variant):
 *   0 Result ReadFile(FileHandle x0, s64 offset x1, void *buffer x2, size_t size x3)                     sdk+0x9c520
 *   1 Result ReadFile(FileHandle x0, s64 x1, void * x2, size_t x3, const ReadOption & x4)                sdk+0x9c430
 *   2 Result ReadFile(size_t *out x0, FileHandle x1, s64 x2, void * x3, size_t x4)                       sdk+0x9c6a0
 *   3 Result ReadFile(size_t *out x0, FileHandle x1, s64 x2, void * x3, size_t x4, const ReadOption & x5) sdk+0x9c610
 * Semantics of FileAccessor::Read -> fsa::IFile::Read -> RomFsFile::DoRead: size 0 succeeds with 0;
 * then null buffer -> NullptrArgument; negative offset/size or overflow -> OutOfRange; offset beyond
 * the file -> OutOfRange; otherwise min(size, file size - offset) bytes. Variants 0/1 additionally
 * require the full size (OutOfRange after reading, as sdk+0x9c5a4). */
static inline uint32_t svc_fs_read_plan(uint64_t offset, uint64_t buffer, uint64_t size, uint64_t file_size, int has_out, uint64_t out, uint64_t *n) {
    *n = 0;
    if (has_out && !out) return SVC_FS_RESULT_NULLPTR_ARGUMENT;
    if (!size) return 0;
    if (!buffer) return SVC_FS_RESULT_NULLPTR_ARGUMENT;
    if ((int64_t)offset < 0 || (int64_t)size < 0 || (uint64_t)INT64_MAX - offset < size) return SVC_FS_RESULT_OUT_OF_RANGE;
    if (offset > file_size) return SVC_FS_RESULT_OUT_OF_RANGE;
    *n = file_size - offset < size ? file_size - offset : size;
    return 0;
}
static inline int svc_fs_read_file(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    unsigned variant = (unsigned)(uintptr_t)user;
    int has_out = variant >= 2;
    uint64_t out = has_out ? c->x[0] : 0, handle = c->x[has_out], offset = c->x[has_out + 1],
             buffer = c->x[has_out + 2], size = c->x[has_out + 3];
    uint32_t entry; uint64_t data, file_size, n;
    if (!svc_fs_is_native(handle)) {
        if (!svc_fs_config()->shadow || !svc_fs_map_get(handle, &entry)) return BOOT_HOST_DECLINE;
        SvcRomfs *r = svc_romfs_acquire(c); if (!r) return BOOT_HOST_DECLINE;
        SvcFsShadow e; memset(&e, 0, sizeof e);
        e.kind = SVC_SHADOW_READ; e.entry = entry; e.out = out; e.buffer = buffer; e.size = size; e.no_out = !has_out;
        int ok = svc_romfs_file(r, entry, &data, &file_size);
        if (ok) {
            e.expect_result = svc_fs_read_plan(offset, buffer, size, file_size, has_out, out, &n);
            if (!e.expect_result && !has_out && n != size) e.expect_result = SVC_FS_RESULT_OUT_OF_RANGE;
            e.expect_value = n;
            if (!e.expect_result) ok = svc_fs_copy(c, self, r, data + offset, n, 0, &e.expect_crc) == 1;
        }
        svc_romfs_release();
        return ok ? svc_fs_shadow_begin(c, self, &e) : BOOT_HOST_DECLINE;
    }
    SvcRomfs *r;
    if (!svc_fs_handle_entry(handle, &entry) || !(r = svc_romfs_acquire(c))) {
        boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
    }
    if (!svc_romfs_file(r, entry, &data, &file_size)) { svc_romfs_release(); boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP; }
    uint32_t result = svc_fs_read_plan(offset, buffer, size, file_size, has_out, out, &n);
    if (!result) {
        int rc = svc_fs_copy(c, self, r, data + offset, n, buffer, NULL);
        if (rc <= 0) { svc_romfs_release(); if (!rc) boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP; }
    }
    svc_romfs_release();
    if (!result && has_out && !boot_host_write(c, self, out, &n, 8)) return BOOT_HOST_STOP;
    if (!result && !has_out && n != size) result = SVC_FS_RESULT_OUT_OF_RANGE;
    if (result) svc_fs_log("native ReadFile v%u entry=%#x offset=%#llx size=%#llx -> %#x", variant, entry,
                           (unsigned long long)offset, (unsigned long long)size, result);
    c->x[0] = result; return BOOT_HOST_DONE;
}
/* Result nn::fs::GetFileSize(s64 *out x0, FileHandle x1) */
static inline int svc_fs_get_file_size(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint32_t entry; uint64_t data, size;
    if (!svc_fs_is_native(c->x[1])) {
        if (!svc_fs_config()->shadow || !c->x[0] || !svc_fs_map_get(c->x[1], &entry)) return BOOT_HOST_DECLINE;
        SvcRomfs *r = svc_romfs_acquire(c); if (!r) return BOOT_HOST_DECLINE;
        int ok = svc_romfs_file(r, entry, &data, &size); svc_romfs_release();
        if (!ok) return BOOT_HOST_DECLINE;
        SvcFsShadow e; memset(&e, 0, sizeof e); e.kind = SVC_SHADOW_SIZE; e.entry = entry; e.out = c->x[0]; e.expect_value = size;
        return svc_fs_shadow_begin(c, self, &e);
    }
    SvcRomfs *r;
    if (!svc_fs_handle_entry(c->x[1], &entry) || !(r = svc_romfs_acquire(c))) { boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP; }
    int ok = svc_romfs_file(r, entry, &data, &size); svc_romfs_release();
    if (!ok) { boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP; }
    if (!c->x[0]) { c->x[0] = SVC_FS_RESULT_NULLPTR_ARGUMENT; return BOOT_HOST_DONE; }
    if (!boot_host_write(c, self, c->x[0], &size, 8)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}
/* void nn::fs::CloseFile(FileHandle x0) */
static inline int svc_fs_close_file(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!svc_fs_is_native(c->x[0])) {
        if (svc_fs_config()->shadow) svc_fs_map_set(c->x[0], 0, 0);
        return BOOT_HOST_DECLINE;
    }
    return BOOT_HOST_DONE;
}
/* int nn::fs::GetFileOpenMode(FileHandle x0) */
static inline int svc_fs_get_open_mode(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!svc_fs_is_native(c->x[0])) return BOOT_HOST_DECLINE;
    c->x[0] = 1; return BOOT_HOST_DONE;   /* OpenMode_Read */
}
/* Result nn::fs::GetEntryType(DirectoryEntryType *out x0 (32-bit, RomFsFileSystem::DoGetEntryType), const char *path x1) */
static inline int svc_fs_get_entry_type(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    const SvcFsConfig *cfg = svc_fs_config();
    SvcRomfs *r; uint32_t entry;
    if (!cfg->api || !c->x[0]) return BOOT_HOST_DECLINE;
    int kind = svc_fs_lookup(c, c->x[1], &r, &entry);
    if (!kind) return BOOT_HOST_DECLINE;
    svc_romfs_release();
    uint32_t type = kind == 1 ? 1 : 0;   /* DirectoryEntryType_File = 1, _Directory = 0 */
    if (cfg->shadow) {
        SvcFsShadow e; memset(&e, 0, sizeof e); e.kind = SVC_SHADOW_ENTRY_TYPE; e.out = c->x[0]; e.entry = entry; e.expect_value = type;
        return svc_fs_shadow_begin(c, self, &e);
    }
    if (!boot_host_write(c, self, c->x[0], &type, 4)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}
static inline const char *svc_fs_entry_name(const BootContext *c, uint64_t self);
/* Guards: a native handle in handle register `user & 7` gets a fixed result (user >> 8: 1 success,
 * 2 WriteNotPermitted) or, for everything not meaningful on a read-only RomFS file, a clean stop
 * (restartable, nothing changed) naming the function. Guest handles go through untouched. */
static inline int svc_fs_guard(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    unsigned reg = (unsigned)(uintptr_t)user & 7, action = (unsigned)(uintptr_t)user >> 8;
    if (!svc_fs_is_native(c->x[reg])) return BOOT_HOST_DECLINE;
    if (action == 1) { c->x[0] = 0; return BOOT_HOST_DONE; }
    if (action == 2) { c->x[0] = SVC_FS_RESULT_WRITE_NOT_PERMITTED; return BOOT_HOST_DONE; }
    const char *name = svc_fs_entry_name(c, self);
    fprintf(stderr, "svc.fs: native RomFS file handle %#llx passed to %s: unsupported, stopping\n", (unsigned long long)c->x[reg], name ? name : "?");
    svc_fs_log("STOP native handle %#llx passed to %s", (unsigned long long)c->x[reg], name ? name : "?");
    boot_fault(c, BOOT_TARGET, self, self, 0);
    return BOOT_HOST_STOP;
}

/* ---------------------------------------------------------------- registration */
/* Guest-entry hooks (sdk module offsets; dynsym names checked by test_svc_fs.py). */
typedef struct SvcFsEntry { uint32_t sdk_offset; const char *name; BootHostFunction fn; uintptr_t user; } SvcFsEntry;
#define SVC_FS_GUARD(reg, action) ((uintptr_t)(reg) | (uintptr_t)(action) << 8)
static const SvcFsEntry svc_fs_entries[] = {
    {0xabc64, "svc.fs.StorageAdapter.Read",    svc_fs_storage_read,   0},
    {0xabce4, "svc.fs.StorageAdapter.GetSize", svc_fs_storage_size,   0},
    {0x9e1c0, "svc.fs.OpenFile",               svc_fs_open_file,      0},
    {0x9c520, "svc.fs.ReadFile",               svc_fs_read_file,      0},
    {0x9c430, "svc.fs.ReadFile.Option",        svc_fs_read_file,      1},
    {0x9c6a0, "svc.fs.ReadFile.Out",           svc_fs_read_file,      2},
    {0x9c610, "svc.fs.ReadFile.OutOption",     svc_fs_read_file,      3},
    {0x9ca50, "svc.fs.GetFileSize",            svc_fs_get_file_size,  0},
    {0x9c3a0, "svc.fs.CloseFile",              svc_fs_close_file,     0},
    {0x9cb60, "svc.fs.GetFileOpenMode",        svc_fs_get_open_mode,  0},
    {0x9dd00, "svc.fs.GetEntryType",           svc_fs_get_entry_type, 0},
    {0x9c730, "svc.fs.guard.WriteFile",        svc_fs_guard, SVC_FS_GUARD(0, 2)},
    {0x9c860, "svc.fs.guard.FlushFile",        svc_fs_guard, SVC_FS_GUARD(0, 1)},
    {0x9c950, "svc.fs.guard.SetFileSize",      svc_fs_guard, SVC_FS_GUARD(0, 2)},
    {0x9cbe0, "svc.fs.guard.QueryRange",       svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0x9cc80, "svc.fs.guard.InvalidateCache",  svc_fs_guard, SVC_FS_GUARD(0, 0)},
    {0x9cd20, "svc.fs.guard.QueryUnpreparedRange",       svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0x9cde0, "svc.fs.guard.QueryUnpreparedRangeDetail", svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0x9ceb0, "svc.fs.guard.QueryLazyLoadCompletionRate", svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0x9cf90, "svc.fs.guard.ReadyLazyLoadFileForciblyForDebug", svc_fs_guard, SVC_FS_GUARD(0, 0)},
    {0xd3de0, "svc.fs.guard.QueryMountRomOnFileCacheSize", svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0xd3fa0, "svc.fs.guard.MountRomOnFile",   svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0xe8864, "svc.fs.guard.FileHandleStorage", svc_fs_guard, SVC_FS_GUARD(1, 0)},
    {0xe8884, "svc.fs.guard.FileHandleStorage.Bool", svc_fs_guard, SVC_FS_GUARD(1, 0)},
};
static inline const char *svc_fs_entry_name(const BootContext *c, uint64_t self) {
    for (unsigned i = 0; i < sizeof svc_fs_entries / sizeof svc_fs_entries[0]; ++i)
        if (c->code_bases[3] && self == c->code_bases[3] + svc_fs_entries[i].sdk_offset) return svc_fs_entries[i].name;
    return "?";
}
/* Hooks every fs entry (sdk module 3) and registers the shadow continuation with the shared registry
 * (host_fn.h). Returns the number that failed (collision or full registry), 0 when all are in place.
 * Not kernel functions: they touch only guest memory and their own locked host state, so they run on
 * the calling core's context concurrently with other guest threads. */
static inline unsigned boot_svc_fs_register(void) {
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof svc_fs_entries / sizeof svc_fs_entries[0]; ++i)
        if (!boot_host_fn_hook(3, svc_fs_entries[i].sdk_offset, svc_fs_entries[i].name, svc_fs_entries[i].fn,
                               (void *)svc_fs_entries[i].user, BOOT_HOST_FN_NOLOCK, SVC_FS_STEPS)) ++failed;   /* RomFS I/O: no renderer lock */
    if (!boot_host_fn_register("svc.fs.shadow.ret", svc_fs_shadow_ret, NULL, BOOT_HOST_FN_NOLOCK, 0)) ++failed;
    return failed;
}
#endif
