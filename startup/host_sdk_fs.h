#ifndef TOTK_HOST_SDK_FS_H
#define TOTK_HOST_SDK_FS_H
/* nn::fs directories on the RomFS mount (switch "fs.dir"; included by host_sdk.h). Completes the RomFS
 * side of nn::fs started in host_svc_fs.h (files): OpenDirectory / ReadDirectory /
 * GetDirectoryEntryCount / CloseDirectory for normalized "<rommount>:/dir" paths, served from the same
 * native RomFS index (svc_romfs_*) and mount recognition (svc_fs_rom_mount). Paths that are not on a
 * RomFS mount, missing paths and invalid modes decline: the guest SDK code produces the exact result.
 *
 * Native DirectoryHandle: 0xF5 << 56 | 2 << 52 | generation << 40 | slot << 32 | directory entry offset.
 * The read cursor lives in a host slot (256 slots); a handle whose slot was lost (DLL swap, process
 * restart) stops cleanly at the call instead of reading a wrong directory.
 * Order and counts follow fssystem::RomFsDirectory: child directories (sibling chain) then files;
 * GetDirectoryEntryCount counts the whole directory for the open mode, independent of the cursor. */
#include "host_svc_fs.h"

#define SDK_FS_HANDLE_DIR      UINT64_C(2)
#define SDK_FS_DIR_SLOTS       256
#define SDK_FS_ENTRY_SIZE      0x310         /* nn::fs::DirectoryEntry: name[0x301], type s8 at 0x304, size s64 at 0x308 */
#define SDK_FS_MODE_DIRECTORY  1u
#define SDK_FS_MODE_FILE       2u
#define SDK_FS_MODE_NO_SIZE    UINT32_C(0x80000000)
#define SDK_FS_NONE            UINT32_MAX

typedef struct SdkDirSlot { volatile LONG used; uint32_t generation, dir, mode, next_dir, next_file; } SdkDirSlot;
static inline SdkDirSlot *sdk_fs_dirs(void) { static SdkDirSlot s[SDK_FS_DIR_SLOTS]; return s; }

static inline uint64_t sdk_fs_dir_handle(unsigned slot, uint32_t generation, uint32_t dir) {
    return SVC_FS_HANDLE_TAG << 56 | SDK_FS_HANDLE_DIR << 52 | (uint64_t)(generation & 0xff) << 40 | (uint64_t)slot << 32 | dir;
}
/* The slot of a live native directory handle, or NULL. */
static inline SdkDirSlot *sdk_fs_dir_slot(uint64_t h) {
    if ((h >> 56) != SVC_FS_HANDLE_TAG || ((h >> 52) & 15) != SDK_FS_HANDLE_DIR || (h & UINT64_C(0x000f000000000000))) return NULL;
    SdkDirSlot *s = &sdk_fs_dirs()[(h >> 32) & 0xff];
    if (!s->used || (s->generation & 0xff) != ((h >> 40) & 0xff) || s->dir != (uint32_t)h) return NULL;
    return s;
}
static inline int sdk_fs_is_dir_handle(uint64_t h) { return (h >> 56) == SVC_FS_HANDLE_TAG && ((h >> 52) & 15) == SDK_FS_HANDLE_DIR; }
static inline int sdk_fs_native_stop(BootContext *c, uint64_t self, const char *what, uint64_t h) {
    svc_fs_log("STOP %s: stale native directory handle %#llx", what, (unsigned long long)h);
    boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
}

/* RomFS directory-table entry fields (see svc_romfs_find): parent, sibling, child dir, child file. */
static inline int sdk_fs_dir_field(const SvcRomfs *r, uint32_t dir, unsigned field, uint32_t *out) {
    if ((dir & 3) || dir + UINT64_C(0x18) > r->dir_meta_size) return 0;
    *out = svc_le32(r->dir_meta + dir + 4 * field); return 1;
}
/* Name and (file) size of a child: kind 0 directory, 1 file. */
static inline int sdk_fs_child(const SvcRomfs *r, int file, uint32_t at, uint32_t *sibling, const uint8_t **name, uint32_t *name_size, uint64_t *size) {
    const uint8_t *meta = file ? r->file_meta : r->dir_meta; uint64_t meta_size = file ? r->file_meta_size : r->dir_meta_size;
    unsigned head = file ? 0x20 : 0x18;
    if ((at & 3) || at + (uint64_t)head > meta_size) return 0;
    const uint8_t *e = meta + at; uint32_t n = svc_le32(e + head - 4);
    if (at + (uint64_t)head + n > meta_size || n > 0x300) return 0;
    *sibling = svc_le32(e + 4); *name = e + head; *name_size = n; *size = file ? svc_le64(e + 16) : 0;
    return 1;
}
static inline int sdk_fs_count(const SvcRomfs *r, int file, uint32_t first, uint64_t *count) {
    uint32_t at = first, sib, n; const uint8_t *name; uint64_t size;
    for (unsigned guard = 0; at != SDK_FS_NONE; ++guard) {
        if (guard > (1u << 22) || !sdk_fs_child(r, file, at, &sib, &name, &n, &size)) return 0;
        ++*count; at = sib;
    }
    return 1;
}

/* Result nn::fs::OpenDirectory(DirectoryHandle *out x0, const char *path x1, int mode w2) */
static inline int sdk_fs_open_directory(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint32_t mode = (uint32_t)c->x[2], dir, first_dir, first_file; SvcRomfs *r;
    int saved = sdk_save_open_directory(c, self); if (saved != -1) return saved;   /* save mount (host_sdk_save.h) */
    if (!sdk_config()->fs_dir || svc_fs_config()->shadow || !c->x[0] || !(mode & 3) || (mode & ~(SDK_FS_MODE_NO_SIZE | 3u)))
        return BOOT_HOST_DECLINE;
    if (svc_fs_lookup(c, c->x[1], &r, &dir) != 2) return BOOT_HOST_DECLINE;   /* not ours, missing, or a file */
    int ok = sdk_fs_dir_field(r, dir, 2, &first_dir) && sdk_fs_dir_field(r, dir, 3, &first_file);
    svc_romfs_release();
    if (!ok) return BOOT_HOST_DECLINE;
    SdkDirSlot *slots = sdk_fs_dirs();
    for (unsigned i = 0; i < SDK_FS_DIR_SLOTS; ++i) {
        if (InterlockedCompareExchange(&slots[i].used, 1, 0)) continue;
        SdkDirSlot *s = &slots[i];
        s->generation = s->generation + 1; s->dir = dir; s->mode = mode;
        s->next_dir = (mode & SDK_FS_MODE_DIRECTORY) ? first_dir : SDK_FS_NONE;
        s->next_file = (mode & SDK_FS_MODE_FILE) ? first_file : SDK_FS_NONE;
        uint64_t h = sdk_fs_dir_handle(i, s->generation, dir);
        if (!boot_host_write(c, self, c->x[0], &h, 8)) { s->used = 0; return BOOT_HOST_STOP; }
        c->x[0] = 0; return BOOT_HOST_DONE;
    }
    return BOOT_HOST_DECLINE;   /* all slots busy: guest directory */
}
/* Result nn::fs::ReadDirectory(s64 *out_count x0, DirectoryEntry *entries x1, DirectoryHandle x2, s64 count x3)
 * Restartable: the cursor advances only after every entry was written. */
static inline int sdk_fs_read_directory(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t h = c->x[2], out = c->x[0], entries = c->x[1]; int64_t max = (int64_t)c->x[3];
    int saved = sdk_save_directory_op(c, self, 0); if (saved != -1) return saved;
    if (!sdk_fs_is_dir_handle(h)) return BOOT_HOST_DECLINE;
    SdkDirSlot *s = sdk_fs_dir_slot(h); SvcRomfs *r;
    if (!s || !(r = svc_romfs_acquire(c))) return sdk_fs_native_stop(c, self, "ReadDirectory", h);
    if (!out || (max > 0 && !entries)) { svc_romfs_release(); c->x[0] = SVC_FS_RESULT_NULLPTR_ARGUMENT; return BOOT_HOST_DONE; }
    if (max < 0) { svc_romfs_release(); c->x[0] = SVC_FS_RESULT_OUT_OF_RANGE; return BOOT_HOST_DONE; }
    uint32_t next_dir = s->next_dir, next_file = s->next_file; int64_t n = 0;
    uint8_t entry[SDK_FS_ENTRY_SIZE];
    while (n < max && (next_dir != SDK_FS_NONE || next_file != SDK_FS_NONE)) {
        int file = next_dir == SDK_FS_NONE; uint32_t at = file ? next_file : next_dir, sib, len; const uint8_t *name; uint64_t size;
        if (!sdk_fs_child(r, file, at, &sib, &name, &len, &size)) { svc_romfs_release(); return sdk_fs_native_stop(c, self, "ReadDirectory(table)", h); }
        memset(entry, 0, sizeof entry); memcpy(entry, name, len);
        entry[0x304] = (uint8_t)(file ? 1 : 0);
        if (file && !(s->mode & SDK_FS_MODE_NO_SIZE)) memcpy(entry + 0x308, &size, 8);
        if (!boot_host_write(c, self, entries + (uint64_t)n * SDK_FS_ENTRY_SIZE, entry, sizeof entry)) { svc_romfs_release(); return BOOT_HOST_STOP; }
        if (file) next_file = sib; else next_dir = sib;
        ++n;
    }
    svc_romfs_release();
    if (!boot_host_write(c, self, out, &n, 8)) return BOOT_HOST_STOP;
    s->next_dir = next_dir; s->next_file = next_file;
    c->exclusive_valid = 0; c->x[0] = 0; return BOOT_HOST_DONE;
}
/* Result nn::fs::GetDirectoryEntryCount(s64 *out x0, DirectoryHandle x1) */
static inline int sdk_fs_directory_count(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t h = c->x[1], count = 0; uint32_t first_dir, first_file;
    int saved = sdk_save_directory_op(c, self, 1); if (saved != -1) return saved;
    if (!sdk_fs_is_dir_handle(h)) return BOOT_HOST_DECLINE;
    SdkDirSlot *s = sdk_fs_dir_slot(h); SvcRomfs *r;
    if (!s || !(r = svc_romfs_acquire(c))) return sdk_fs_native_stop(c, self, "GetDirectoryEntryCount", h);
    int ok = sdk_fs_dir_field(r, s->dir, 2, &first_dir) && sdk_fs_dir_field(r, s->dir, 3, &first_file) &&
             (!(s->mode & SDK_FS_MODE_DIRECTORY) || sdk_fs_count(r, 0, first_dir, &count)) &&
             (!(s->mode & SDK_FS_MODE_FILE) || sdk_fs_count(r, 1, first_file, &count));
    svc_romfs_release();
    if (!ok) return sdk_fs_native_stop(c, self, "GetDirectoryEntryCount(table)", h);
    if (!c->x[0]) { c->x[0] = SVC_FS_RESULT_NULLPTR_ARGUMENT; return BOOT_HOST_DONE; }
    if (!boot_host_write(c, self, c->x[0], &count, 8)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}
/* void nn::fs::CloseDirectory(DirectoryHandle x0) */
static inline int sdk_fs_close_directory(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    int saved = sdk_save_directory_op(c, self, 2); if (saved != -1) return saved;
    if (!sdk_fs_is_dir_handle(c->x[0])) return BOOT_HOST_DECLINE;
    SdkDirSlot *s = sdk_fs_dir_slot(c->x[0]);
    if (s) InterlockedExchange(&s->used, 0);
    return BOOT_HOST_DONE;
}

static const SdkEntry sdk_fs_entries[] = {
    {0x9e4b0, "sdk.fs.OpenDirectory",          sdk_fs_open_directory,  0, SVC_FS_STEPS},
    {0x9c170, "sdk.fs.ReadDirectory",          sdk_fs_read_directory,  0, SVC_FS_STEPS},
    {0x9c290, "sdk.fs.GetDirectoryEntryCount", sdk_fs_directory_count, 0, SVC_FS_STEPS},
    {0x9c0e0, "sdk.fs.CloseDirectory",         sdk_fs_close_directory, 0, SVC_FS_STEPS},
};
#endif
