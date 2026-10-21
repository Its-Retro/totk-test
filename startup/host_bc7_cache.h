#ifndef TOTK_HOST_BC7_CACHE_H
#define TOTK_HOST_BC7_CACHE_H
/* bc7-work: disk cache of transcoded textures, so each texture is encoded once ever (host_bc7.h; TOTK_BC7_CACHE=0: off).
 *
 * Key: SHA-256 over "TOTKBC7K", BOOT_BC7_KEY_VERSION, the source descriptor (BootBc7Source: TIC format, sRGB, view
 * dimension, extent, layers, levels, block footprint, block-linear GOB layout, pitch, layer stride, output DXGI format)
 * and the SHA-256 of the source bytes exactly as the guest holds them (the tiled ASTC blocks of every level and layer).
 * The key names the file: <dir>\<64 hex>.bc7. Content addressed: guest writes change the bytes, so a rebuilt texture
 * looks up a new key and never sees a stale entry. Neither the producer nor the encoder settings are in the key: any
 * producer's entry is a valid BC7 image of that source (stage 1 CPU decode + GPU encode, stage 2 GPU decode + GPU
 * encode, stage 3 direct transcoding all write the same kind of file and read each other's); the header records which
 * one wrote it (BootBc7FileHeader.producer, .encoder) for logs and for a later policy that re-encodes weaker entries.
 *
 * File: BootBc7FileHeader (version tag BOOT_BC7_CACHE_VERSION: other versions read as misses), then the payload: for
 * each subresource in D3D12 order (level-major within a layer: sub = level + layer * levels), ceil(h/4) rows of
 * ceil(w/4) * 16 bytes, tightly packed. The header carries a 64-bit checksum of the payload; a file that fails any check
 * (short, torn, other version or shape, checksum) is a miss and is deleted.
 * Writes are crash safe: a background thread writes <name>.tmp<pid>.<n> and renames it over the final name
 * (MoveFileEx REPLACE_EXISTING); a crash leaves at most a temp file, which the next start-up scan removes.
 * LRU: a hit touches the file's last-write time; when the directory exceeds its budget (TOTK_BC7_CACHE_MB, default
 * BOOT_BC7_CACHE_MB_DEFAULT) the writer deletes the least recently written/used files down to 7/8 of it. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_BC7_CACHE_VERSION 1u
#define BOOT_BC7_KEY_VERSION 1u
#define BOOT_BC7_CACHE_MB_DEFAULT 4096u
#define BOOT_BC7_CACHE_PENDING_MAX (UINT64_C(64) << 20)   /* bytes queued for the writer before writes are dropped */

/* Who produced a BC7 image (cache header, logs). Stages 2-4 add their producers here. */
enum {
    BOOT_BC7_PRODUCER_NONE = 0,
    BOOT_BC7_PRODUCER_CPU_DECODE_GPU_ENCODE = 1,   /* stage 1: CPU ASTC decode (host_astc.h) -> RGBA8 texture -> GPU BC7 encode */
    BOOT_BC7_PRODUCER_GPU_DECODE_GPU_ENCODE = 2,   /* stage 2: raw ASTC -> GPU ASTC decode into the encoder input -> GPU BC7 encode */
    BOOT_BC7_PRODUCER_DIRECT = 3,                  /* stage 3: ASTC 4x4 blocks transcoded straight to BC7 blocks */
    BOOT_BC7_PRODUCER_DIRECT_MIXED = 4,            /* stage 3: direct blocks + full decode/encode fallback for the rest */
    BOOT_BC7_PRODUCERS
};
static const char *const boot_bc7_producer_name[BOOT_BC7_PRODUCERS] = {"none", "cpu-decode+gpu-encode", "gpu-decode+gpu-encode", "direct", "direct+fallback"};

typedef struct BootBc7Key { uint8_t b[32]; } BootBc7Key;
/* Everything besides the bytes that decides the image. Zero-initialise, then fill (padding stays zero). */
typedef struct BootBc7Source {
    uint32_t tic_format, srgb, dim, width, height, depth, layers, levels;
    uint32_t block_w, block_h, log_gob_h, log_gob_d, pitch, out_format, reserved[2];
    uint64_t layer_stride, bytes;
} BootBc7Source;
typedef void (*BootBc7Sha256)(const void *data, size_t n, uint8_t out[32]);
static inline void boot_bc7_key(BootBc7Sha256 sha, const BootBc7Source *s, const void *bytes, size_t n, BootBc7Key *k) {
    struct { char tag[8]; uint32_t version, size; BootBc7Source src; uint8_t content[32]; } m;
    memset(&m, 0, sizeof m); memcpy(m.tag, "TOTKBC7K", 8); m.version = BOOT_BC7_KEY_VERSION; m.size = (uint32_t)sizeof m.src; m.src = *s;
    sha(bytes, n, m.content);
    sha(&m, sizeof m, k->b);
}
static inline void boot_bc7_key_hex(const BootBc7Key *k, char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { out[2 * i] = hx[k->b[i] >> 4]; out[2 * i + 1] = hx[k->b[i] & 15]; }
    out[64] = 0;
}

typedef struct BootBc7FileHeader {
    char magic[8];              /* "TOTKBC7\0" */
    uint32_t version;           /* BOOT_BC7_CACHE_VERSION */
    uint32_t header_bytes;      /* sizeof(BootBc7FileHeader) */
    uint32_t producer;          /* BOOT_BC7_PRODUCER_* */
    uint32_t encoder;           /* producer detail; stage 1: quality level | encoder flags << 8 */
    uint32_t encoder_param;     /* stage 1: refinement error gate */
    uint32_t dxgi_format;       /* DXGI_FORMAT_BC7_UNORM (98) or _SRGB (99) */
    uint32_t dim, width, height, layers, levels, subresources;
    uint64_t payload_bytes;
    uint64_t payload_check;     /* boot_bc7_check64 of the payload */
    BootBc7Key key;
    uint32_t reserved[8];
} BootBc7FileHeader;

/* Fast 64-bit checksum (integrity, not security): four multiply-xor lanes over 8-byte words. */
static inline uint64_t boot_bc7_check64(const uint8_t *p, uint64_t n) {
    uint64_t h[4] = {UINT64_C(0x9e3779b97f4a7c15), UINT64_C(0xc2b2ae3d27d4eb4f), UINT64_C(0x165667b19e3779f9), UINT64_C(0x27d4eb2f165667c5)}, i = 0;
    for (; i + 32 <= n; i += 32)
        for (unsigned l = 0; l < 4; ++l) { uint64_t w; memcpy(&w, p + i + 8 * l, 8); h[l] = (h[l] ^ w) * UINT64_C(0x100000001b3); h[l] ^= h[l] >> 29; }
    uint64_t t = n;
    for (; i < n; ++i) t = (t ^ p[i]) * UINT64_C(0x100000001b3);
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ (t * 11);
}
/* Tightly packed payload bytes of a 2D / array / cube image of `levels` levels and `layers` layers. */
static inline uint64_t boot_bc7_payload_bytes(uint32_t w, uint32_t h, uint32_t layers, uint32_t levels) {
    uint64_t n = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        n += (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
    }
    return n * layers;
}

/* ---- the cache directory and its writer thread ---- */
typedef struct BootBc7WriteItem { struct BootBc7WriteItem *next; char path[MAX_PATH]; uint64_t bytes; uint8_t *data; } BootBc7WriteItem;
static struct {
    SRWLOCK lock; CONDITION_VARIABLE cv; int started, ok; HANDLE thread;
    char dir[MAX_PATH];
    uint64_t budget, total, pending;                  /* bytes: directory budget, directory total (writer's view), queued */
    BootBc7WriteItem *head, *tail;
    volatile LONG64 writes, write_fails, dropped, trims, trimmed_files, bad_files, hits, misses, read_ms_x1000;
} boot_bc7_disk;   /* zero-initialised: SRWLOCK_INIT and CONDITION_VARIABLE_INIT are all-zero */

typedef struct { char path[MAX_PATH]; uint64_t size; uint64_t time; } BootBc7DirEntry;
static int boot_bc7_dir_cmp(const void *a, const void *b) {
    uint64_t x = ((const BootBc7DirEntry *)a)->time, y = ((const BootBc7DirEntry *)b)->time; return x < y ? -1 : x > y;
}
/* Scans the directory: removes temp files left by a crash, sums the entries and, when `trim`, deletes the oldest
 * (last write = last use) down to 7/8 of the budget. Writer thread only. Returns the bytes kept. */
static inline uint64_t boot_bc7_dir_scan(int trim) {
    char pat[MAX_PATH + 8]; snprintf(pat, sizeof pat, "%s\\*", boot_bc7_disk.dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BootBc7DirEntry *e = NULL; size_t n = 0, cap = 0; uint64_t total = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t len = strlen(fd.cFileName); char full[MAX_PATH];
        if (snprintf(full, sizeof full, "%s\\%s", boot_bc7_disk.dir, fd.cFileName) >= (int)sizeof full) continue;
        if (strstr(fd.cFileName, ".tmp")) { DeleteFileA(full); continue; }   /* an interrupted write (only this process writes) */
        if (len != 68 || strcmp(fd.cFileName + 64, ".bc7")) continue;       /* not ours */
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        total += size;
        if (!trim) continue;
        if (n == cap) { cap = cap ? cap * 2 : 1024; BootBc7DirEntry *ne = (BootBc7DirEntry *)realloc(e, cap * sizeof *e); if (!ne) break; e = ne; }
        memcpy(e[n].path, full, sizeof full); e[n].size = size;
        e[n].time = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime; ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (trim && e && total > boot_bc7_disk.budget) {
        qsort(e, n, sizeof *e, boot_bc7_dir_cmp);
        uint64_t target = boot_bc7_disk.budget - boot_bc7_disk.budget / 8;
        for (size_t i = 0; i < n && total > target; ++i)
            if (DeleteFileA(e[i].path)) { total -= e[i].size; InterlockedIncrement64(&boot_bc7_disk.trimmed_files); }
        InterlockedIncrement64(&boot_bc7_disk.trims);
    }
    free(e);
    return total;
}
static DWORD WINAPI boot_bc7_writer(void *arg) {
    (void)arg; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint64_t total = boot_bc7_dir_scan(1);
    AcquireSRWLockExclusive(&boot_bc7_disk.lock); boot_bc7_disk.total = total; ReleaseSRWLockExclusive(&boot_bc7_disk.lock);
    static LONG serial;
    for (;;) {
        AcquireSRWLockExclusive(&boot_bc7_disk.lock);
        while (!boot_bc7_disk.head) SleepConditionVariableSRW(&boot_bc7_disk.cv, &boot_bc7_disk.lock, INFINITE, 0);
        BootBc7WriteItem *it = boot_bc7_disk.head; boot_bc7_disk.head = it->next; if (!boot_bc7_disk.head) boot_bc7_disk.tail = NULL;
        ReleaseSRWLockExclusive(&boot_bc7_disk.lock);
        char tmp[MAX_PATH + 32]; snprintf(tmp, sizeof tmp, "%s.tmp%lu.%ld", it->path, (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&serial));
        int ok = 0; HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            ok = 1;
            for (uint64_t at = 0; ok && at < it->bytes; ) {
                DWORD chunk = it->bytes - at > (UINT64_C(1) << 30) ? (DWORD)1 << 30 : (DWORD)(it->bytes - at), wrote = 0;
                ok = WriteFile(f, it->data + at, chunk, &wrote, NULL) && wrote == chunk; at += wrote;
            }
            ok = CloseHandle(f) && ok;
            if (ok) ok = MoveFileExA(tmp, it->path, MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) DeleteFileA(tmp);
        }
        InterlockedIncrement64(ok ? &boot_bc7_disk.writes : &boot_bc7_disk.write_fails);
        AcquireSRWLockExclusive(&boot_bc7_disk.lock);
        boot_bc7_disk.pending -= it->bytes; if (ok) boot_bc7_disk.total += it->bytes;
        int over = boot_bc7_disk.total > boot_bc7_disk.budget;
        ReleaseSRWLockExclusive(&boot_bc7_disk.lock);
        free(it->data); free(it);
        if (over) { uint64_t t = boot_bc7_dir_scan(1); AcquireSRWLockExclusive(&boot_bc7_disk.lock); boot_bc7_disk.total = t; ReleaseSRWLockExclusive(&boot_bc7_disk.lock); }
    }
}
/* Opens the cache at `dir` (created if missing) with a budget of `budget` bytes and starts the writer. 1 = usable. */
static inline int boot_bc7_disk_open(const char *dir, uint64_t budget) {
    if (boot_bc7_disk.started) return boot_bc7_disk.ok;
    boot_bc7_disk.started = 1;
    if (!dir || !*dir || strlen(dir) >= MAX_PATH - 80) return 0;
    snprintf(boot_bc7_disk.dir, sizeof boot_bc7_disk.dir, "%s", dir);
    size_t L = strlen(boot_bc7_disk.dir); while (L && (boot_bc7_disk.dir[L - 1] == '\\' || boot_bc7_disk.dir[L - 1] == '/')) boot_bc7_disk.dir[--L] = 0;
    CreateDirectoryA(boot_bc7_disk.dir, NULL);
    DWORD a = GetFileAttributesA(boot_bc7_disk.dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    boot_bc7_disk.budget = budget;
    boot_bc7_disk.thread = CreateThread(NULL, 0, boot_bc7_writer, NULL, 0, NULL);
    if (!boot_bc7_disk.thread) return 0;
    return boot_bc7_disk.ok = 1;
}
static inline void boot_bc7_disk_path(const BootBc7Key *k, char out[MAX_PATH]) {
    char hx[65]; boot_bc7_key_hex(k, hx); snprintf(out, MAX_PATH, "%s\\%s.bc7", boot_bc7_disk.dir, hx);
}
/* Queues header + payload (tightly packed) for writing; takes ownership of `file` (malloc'd, header first). */
static inline int boot_bc7_disk_write(const BootBc7Key *k, uint8_t *file, uint64_t bytes) {
    if (!boot_bc7_disk.ok) { free(file); return 0; }
    BootBc7WriteItem *it = (BootBc7WriteItem *)calloc(1, sizeof *it);
    if (!it) { free(file); return 0; }
    boot_bc7_disk_path(k, it->path); it->data = file; it->bytes = bytes;
    AcquireSRWLockExclusive(&boot_bc7_disk.lock);
    int room = boot_bc7_disk.pending + bytes <= BOOT_BC7_CACHE_PENDING_MAX;
    if (room) {
        boot_bc7_disk.pending += bytes;
        if (boot_bc7_disk.tail) boot_bc7_disk.tail->next = it; else boot_bc7_disk.head = it;
        boot_bc7_disk.tail = it;
    }
    ReleaseSRWLockExclusive(&boot_bc7_disk.lock);
    if (!room) { InterlockedIncrement64(&boot_bc7_disk.dropped); free(file); free(it); return 0; }
    WakeConditionVariable(&boot_bc7_disk.cv);
    return 1;
}
/* Reads the entry for key `k` describing exactly (format, dim, extent, layers, levels). Returns the payload
 * (malloc'd, *payload_bytes set, *hdr filled) or NULL (a miss; a damaged file is deleted). Touches the file
 * (last-write time = now) for the LRU. */
static inline uint8_t *boot_bc7_disk_read(const BootBc7Key *k, uint32_t fmt, uint32_t dim, uint32_t w, uint32_t h, uint32_t layers, uint32_t levels,
                                          uint64_t *payload_bytes, BootBc7FileHeader *hdr) {
    if (!boot_bc7_disk.ok) return NULL;
    char path[MAX_PATH]; boot_bc7_disk_path(k, path);
    HANDLE f = CreateFileA(path, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);   /* a rename may replace it meanwhile */
    if (f == INVALID_HANDLE_VALUE) { InterlockedIncrement64(&boot_bc7_disk.misses); return NULL; }
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
    BootBc7FileHeader x; memset(&x, 0, sizeof x); DWORD hgot = 0, got = 0; uint8_t *p = NULL; int bad = 1;
    uint64_t want = boot_bc7_payload_bytes(w, h, layers, levels);
    LARGE_INTEGER fs; fs.QuadPart = 0;
    if (ReadFile(f, &x, sizeof x, &hgot, NULL) && hgot == sizeof x && !memcmp(x.magic, "TOTKBC7", 8) && x.version == BOOT_BC7_CACHE_VERSION &&
        x.header_bytes == sizeof x && !memcmp(&x.key, k, sizeof *k) && x.dxgi_format == fmt && x.dim == dim && x.width == w && x.height == h &&
        x.layers == layers && x.levels == levels && x.subresources == layers * levels && x.payload_bytes == want &&
        GetFileSizeEx(f, &fs) && (uint64_t)fs.QuadPart == sizeof x + want && want < (UINT64_C(1) << 31) && (p = (uint8_t *)malloc((size_t)want)) != NULL) {
        bad = !(ReadFile(f, p, (DWORD)want, &got, NULL) && got == want && boot_bc7_check64(p, want) == x.payload_check);
    }
    if (!bad) { FILETIME now; GetSystemTimeAsFileTime(&now); SetFileTime(f, NULL, NULL, &now); }
    CloseHandle(f);
    if (bad) {
        free(p); p = NULL;
        /* damaged (short, torn, wrong shape or checksum): deleted. Another format version is only a miss (rewritten when re-encoded). */
        if (hgot < sizeof x || memcmp(x.magic, "TOTKBC7", 8) || x.version == BOOT_BC7_CACHE_VERSION) { DeleteFileA(path); InterlockedIncrement64(&boot_bc7_disk.bad_files); }
        InterlockedIncrement64(&boot_bc7_disk.misses);
        return NULL;
    }
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    InterlockedExchangeAdd64(&boot_bc7_disk.read_ms_x1000, (LONG64)((q1.QuadPart - q0.QuadPart) * 1000000 / qf.QuadPart));
    InterlockedIncrement64(&boot_bc7_disk.hits);
    *payload_bytes = want; *hdr = x;
    return p;
}
#endif
