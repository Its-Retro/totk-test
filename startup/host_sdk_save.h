#ifndef TOTK_HOST_SDK_SAVE_H
#define TOTK_HOST_SDK_SAVE_H
/* nn::fs save data as native host-directory mounts (switch "fs.save"; included by host_sdk.h).
 *
 * The game's save mounts never reach fsp: nn::fs::MountSaveData / MountSaveDataReadOnly /
 * MountTemporaryStorage / MountCacheStorage add a mount to a host table (name -> host directory), and
 * every nn::fs function the game imports that takes a path or a handle checks that table first:
 *   paths   OpenFile, CreateFile, DeleteFile, RenameFile, GetEntryType, CreateDirectory,
 *           DeleteDirectoryRecursively, OpenDirectory, Commit, Unmount
 *   handles ReadFile x4, WriteFile, FlushFile, SetFileSize, GetFileSize, GetFileOpenMode, CloseFile,
 *           ReadDirectory, GetDirectoryEntryCount, CloseDirectory
 *   cache   CreateCacheStorage, DeleteCacheStorage, Open/Read/CloseCacheStorageList, IsSaveDataExisting
 * Paths on other mounts (RomFS, anything the guest mounted) go to the RomFS hooks (host_svc_fs.h,
 * host_sdk_fs.h) or the original SDK code. Hooks shared with host_svc_fs.h are chained: the save hook
 * runs first and hands everything else to the svc.fs function registered under the same name.
 *
 * Host layout (the same directories the fsp IPC handlers in boot_runtime.h use, root = TOTK_SAVE_ROOT,
 * default "startup\save-data" relative to the working directory):
 *   MountSaveData                 <root>\0100F2C0115B6000\common      (created on first mount)
 *   MountTemporaryStorage         <root>\0100F2C0115B6000\temporary   (created on first mount)
 *   MountCacheStorage(index)      <root>\cache\0100F2C0115B6000\<index> (CreateCacheStorage makes it)
 *   MountSaveDataReadOnly(app)    <root>\<APPID>, read-only; TargetNotFound when absent (as the IPC path)
 *   DeleteCacheStorage            moves the volume to <root>\deleted\<tick>-cache-<index> (recoverable)
 * Writes go straight to the host files (Commit flushes); there is no journal.
 *
 * Native handles (top byte 0xF5, kind in bits 52..55, generation 40..47, slot 32..39): 3 save file,
 * 4 save directory, 5 cache-storage list. The open host handles live in host slots, so a handle does
 * not survive a DLL swap (the play build has none); a stale handle stops cleanly at the call.
 * Results are the fs results the SDK returns for these cases (PathNotFound 0x202, PathAlreadyExists
 * 0x402, TargetLocked 0xe02, TargetNotFound 0x7d402, OutOfRange 0x177a02, ReadNotPermitted 0x307402,
 * WriteNotPermitted 0x307602, FileExtensionWithoutOpenModeAllowAppend 0x307202). */
#include "host_svc_fs.h"

#define SDK_SAVE_APP              UINT64_C(0x0100F2C0115B6000)
#define SDK_SAVE_R_PATH_NOT_FOUND UINT32_C(0x202)
#define SDK_SAVE_R_ALREADY_EXISTS UINT32_C(0x402)
#define SDK_SAVE_R_NO_SPACE       UINT32_C(0x3c02)
#define SDK_SAVE_R_TARGET_LOCKED  UINT32_C(0xe02)
#define SDK_SAVE_R_TARGET_MISSING UINT32_C(0x7d402)
#define SDK_SAVE_R_INVALID_ARG    UINT32_C(0x2ee202)
#define SDK_SAVE_R_READ_DENIED    UINT32_C(0x307402)
#define SDK_SAVE_R_APPEND_DENIED  UINT32_C(0x307202)
#define SDK_SAVE_KIND_FILE        UINT64_C(3)
#define SDK_SAVE_KIND_DIR         UINT64_C(4)
#define SDK_SAVE_KIND_LIST        UINT64_C(5)
#define SDK_SAVE_MOUNTS           16
#define SDK_SAVE_SLOTS            256
#define SDK_SAVE_STEPS            64
#define SDK_SAVE_WPATH            1100
#define SDK_FS_ENTRY_SIZE         0x310         /* nn::fs::DirectoryEntry (also host_sdk_fs.h) */

/* ---------------------------------------------------------------- mounts */
typedef struct SdkSaveMount { char name[16]; wchar_t root[MAX_PATH]; int read_only; } SdkSaveMount;
typedef struct SdkSaveState {
    SRWLOCK lock; SdkSaveMount mount[SDK_SAVE_MOUNTS];
    struct { volatile LONG used; uint32_t generation, mode, cursor, count; HANDLE file; int mount; wchar_t *dir; int32_t index[64]; } slot[SDK_SAVE_SLOTS];
} SdkSaveState;
static inline SdkSaveState *sdk_save(void) { static SdkSaveState s; return &s; }   /* zero = SRWLOCK_INIT */
static inline const wchar_t *sdk_save_root(void) {
    static wchar_t root[MAX_PATH]; static volatile LONG ready;
    if (!ready) {
        DWORD n = GetEnvironmentVariableW(L"TOTK_SAVE_ROOT", root, MAX_PATH);
        if (!n || n >= MAX_PATH) wcscpy(root, L"startup\\save-data");
        MemoryBarrier(); ready = 1;
    }
    return root;
}
static inline void sdk_save_log(const char *fmt, ...) {
    static SRWLOCK lock = SRWLOCK_INIT; va_list ap; va_start(ap, fmt);
    AcquireSRWLockExclusive(&lock);
    FILE *f = fopen("native-save.log", "a");
    if (f) { vfprintf(f, fmt, ap); fputc('\n', f); fclose(f); }
    ReleaseSRWLockExclusive(&lock); va_end(ap);
}
/* Mount index of a native mount name, -1 if none (caller holds the lock or accepts a racy answer). */
static inline int sdk_save_find(const char *name) {
    SdkSaveState *s = sdk_save();
    for (int i = 0; i < SDK_SAVE_MOUNTS; ++i) if (s->mount[i].name[0] && !strncmp(s->mount[i].name, name, 16)) return i;
    return -1;
}
/* 1 when the guest's own nn::fs mount table has `name` (MountTable walk as svc_fs_rom_mount). */
static inline int sdk_save_guest_mounted(const BootContext *c, const char *name) {
    uint64_t sdk = c->code_bases[3], table = sdk + SVC_FS_SDK_MOUNT_TABLE, node = 0;
    if (!sdk || !svc_fs_peek_u64(c, table + 8, &node)) return 0;
    for (unsigned guard = 0; node && node != table && guard < 256; ++guard) {
        char n[17] = {0};
        if (!svc_fs_peek(c, node - 8 + SVC_FS_ACCESSOR_NAME, n, 16)) return 0;
        if (!strncmp(n, name, 16)) return 1;
        if (!svc_fs_peek_u64(c, node + 8, &node)) return 0;
    }
    return 0;
}
static inline int sdk_save_mkdirs(const wchar_t *path) {
    wchar_t buf[SDK_SAVE_WPATH]; size_t n = wcslen(path); if (n >= SDK_SAVE_WPATH) return 0;
    wcscpy(buf, path);
    for (size_t i = 1; i <= n; ++i)
        if (buf[i] == L'\\' || buf[i] == 0) {
            wchar_t keep = buf[i]; buf[i] = 0;
            if (!CreateDirectoryW(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { buf[i] = keep; return 0; }
            buf[i] = keep;
        }
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static inline int sdk_save_is_dir(const wchar_t *path) { DWORD a = GetFileAttributesW(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static inline uint32_t sdk_save_win32_result(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_DIRECTORY: return SDK_SAVE_R_PATH_NOT_FOUND;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return SDK_SAVE_R_ALREADY_EXISTS;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_ACCESS_DENIED: return SDK_SAVE_R_TARGET_LOCKED;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return SDK_SAVE_R_NO_SPACE;
    default: sdk_save_log("unexpected host error %lu", (unsigned long)e); return SDK_SAVE_R_PATH_NOT_FOUND;
    }
}

/* Reads the guest path; returns the mount index (>= 0) for a native mount and the host path in `out`
 * (root for "/"), -1 = not a native mount (decline / chain), -2 = native mount but invalid path. */
static inline int sdk_save_path(const BootContext *c, uint64_t guest, wchar_t *out, int *read_only) {
    char path[SVC_FS_PATH_MAX + 32], mount[16]; const char *rest;
    if (!guest || !svc_fs_peek_string(c, guest, path, sizeof path)) return -1;
    const char *colon = strchr(path, ':');
    if (!colon || colon == path || colon - path > 15) return -1;
    memcpy(mount, path, (size_t)(colon - path)); mount[colon - path] = 0;
    SdkSaveState *s = sdk_save();
    AcquireSRWLockShared(&s->lock);
    int m = sdk_save_find(mount);
    if (m >= 0) { wcscpy(out, s->mount[m].root); if (read_only) *read_only = s->mount[m].read_only; }
    ReleaseSRWLockShared(&s->lock);
    if (m < 0) return -1;
    if (!svc_fs_split(path, mount, &rest)) { sdk_save_log("invalid path %s", path); return -2; }
    size_t n = wcslen(out);
    if (rest[1]) {
        wchar_t w[SVC_FS_PATH_MAX + 8];
        int k = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rest, -1, w, SVC_FS_PATH_MAX + 8);
        if (k <= 0 || n + (size_t)k >= SDK_SAVE_WPATH) return -2;
        for (int i = 0; w[i]; ++i) if (w[i] == L'/') w[i] = L'\\';
        wcscpy(out + n, w);
    }
    return m;
}

/* ---------------------------------------------------------------- handles */
static inline uint64_t sdk_save_handle(uint64_t kind, unsigned slot, uint32_t generation) {
    return SVC_FS_HANDLE_TAG << 56 | kind << 52 | (uint64_t)(generation & 0xff) << 40 | (uint64_t)slot << 32;
}
static inline int sdk_save_kind(uint64_t h, uint64_t kind) { return (h >> 56) == SVC_FS_HANDLE_TAG && ((h >> 52) & 15) == kind; }
static inline int sdk_save_slot_of(uint64_t h, uint64_t kind) {
    if (!sdk_save_kind(h, kind) || (h & UINT64_C(0x000f0000ffffffff))) return -1;
    SdkSaveState *s = sdk_save(); unsigned i = (unsigned)(h >> 32) & 0xff;
    if (!s->slot[i].used || (s->slot[i].generation & 0xff) != ((h >> 40) & 0xff)) return -1;
    return (int)i;
}
static inline int sdk_save_claim(void) {
    SdkSaveState *s = sdk_save();
    for (int i = 0; i < SDK_SAVE_SLOTS; ++i)
        if (!InterlockedCompareExchange(&s->slot[i].used, 1, 0)) {
            ++s->slot[i].generation; s->slot[i].file = NULL; s->slot[i].dir = NULL; s->slot[i].cursor = s->slot[i].count = 0;
            return i;
        }
    return -1;
}
static inline void sdk_save_release(int i) {
    SdkSaveState *s = sdk_save();
    if (s->slot[i].file && s->slot[i].file != INVALID_HANDLE_VALUE) CloseHandle(s->slot[i].file);
    free(s->slot[i].dir); s->slot[i].file = NULL; s->slot[i].dir = NULL;
    InterlockedExchange(&s->slot[i].used, 0);
}
static inline int sdk_save_stale(BootContext *c, uint64_t self, const char *what, uint64_t h) {
    sdk_save_log("STOP %s: stale native save handle %#llx", what, (unsigned long long)h);
    fprintf(stderr, "sdk.save: stale native handle %#llx passed to %s, stopping\n", (unsigned long long)h, what);
    boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
}
static inline int sdk_save_done(BootContext *c, uint32_t result) { c->x[0] = result; return BOOT_HOST_DONE; }

/* ---------------------------------------------------------------- mounting */
static inline int sdk_save_add_mount(BootContext *c, uint64_t name_address, const wchar_t *root, int read_only) {
    char name[17];
    if (!name_address || !svc_fs_peek_string(c, name_address, name, sizeof name) || !name[0] || name[0] == '@' || strchr(name, ':') || strchr(name, '/'))
        return BOOT_HOST_DECLINE;                                 /* the SDK's own name checks and results */
    SdkSaveState *s = sdk_save(); uint32_t result = 0;
    AcquireSRWLockExclusive(&s->lock);
    if (sdk_save_find(name) >= 0 || sdk_save_guest_mounted(c, name)) result = UINT32_C(0x101802);   /* MountNameAlreadyExists */
    else {
        int free_slot = -1;
        for (int i = 0; i < SDK_SAVE_MOUNTS && free_slot < 0; ++i) if (!s->mount[i].name[0]) free_slot = i;
        if (free_slot < 0) result = UINT32_C(0x101802);
        else { wcscpy(s->mount[free_slot].root, root); s->mount[free_slot].read_only = read_only; memcpy(s->mount[free_slot].name, name, 16); }
    }
    ReleaseSRWLockExclusive(&s->lock);
    if (result) sdk_save_log("mount %s -> %#x", name, result);
    return sdk_save_done(c, result);
}
/* Result MountSaveData(const char *name x0, const account::Uid & x1)            user 0
 * Result MountTemporaryStorage(const char *name x0)                              user 1
 * Result MountCacheStorage(const char *name x0, int index w1)                    user 2
 * Result MountSaveDataReadOnly(const char *name x0, ApplicationId x1, const Uid & x2)  user 3 */
static inline int sdk_save_mount(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target;
    if (!sdk_config()->fs_save) return BOOT_HOST_DECLINE;
    wchar_t root[SDK_SAVE_WPATH]; unsigned kind = (unsigned)(uintptr_t)user;
    switch (kind) {
    case 0: case 1:
        swprintf(root, SDK_SAVE_WPATH, L"%ls\\%016llX\\%ls", sdk_save_root(), (unsigned long long)SDK_SAVE_APP, kind ? L"temporary" : L"common");
        if (!sdk_save_mkdirs(root)) { sdk_save_log("cannot create %ls", root); return sdk_save_done(c, SDK_SAVE_R_TARGET_MISSING); }
        break;
    case 2:
        if ((int32_t)c->x[1] < 0) return BOOT_HOST_DECLINE;
        swprintf(root, SDK_SAVE_WPATH, L"%ls\\cache\\%016llX\\%d", sdk_save_root(), (unsigned long long)SDK_SAVE_APP, (int)(int32_t)c->x[1]);
        if (!sdk_save_is_dir(root)) return sdk_save_done(c, SDK_SAVE_R_TARGET_MISSING);
        break;
    default:
        swprintf(root, SDK_SAVE_WPATH, L"%ls\\%016llX", sdk_save_root(), (unsigned long long)c->x[1]);
        if (!sdk_save_is_dir(root)) return sdk_save_done(c, SDK_SAVE_R_TARGET_MISSING);
        break;
    }
    return sdk_save_add_mount(c, c->x[0], root, kind == 3);
}
/* void Unmount(const char *name x0): native mounts only (open handles stay usable; the SDK would abort). */
static inline int sdk_save_unmount(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    char name[17]; SdkSaveState *s = sdk_save(); int hit = 0;
    if (!c->x[0] || !svc_fs_peek_string(c, c->x[0], name, sizeof name)) return BOOT_HOST_DECLINE;
    AcquireSRWLockExclusive(&s->lock);
    int m = sdk_save_find(name);
    if (m >= 0) {
        hit = 1; memset(s->mount[m].name, 0, sizeof s->mount[m].name);
        for (int i = 0; i < SDK_SAVE_SLOTS; ++i)
            if (s->slot[i].used && s->slot[i].mount == m && s->slot[i].file) sdk_save_log("Unmount %s with a file still open (SDK aborts here)", name);
    }
    ReleaseSRWLockExclusive(&s->lock);
    return hit ? BOOT_HOST_DONE : BOOT_HOST_DECLINE;
}
/* Result Commit(const char *name x0): flush every open file of the mount. */
static inline int sdk_save_commit(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    char name[17]; SdkSaveState *s = sdk_save();
    if (!c->x[0] || !svc_fs_peek_string(c, c->x[0], name, sizeof name)) return BOOT_HOST_DECLINE;
    AcquireSRWLockShared(&s->lock);
    int m = sdk_save_find(name);
    if (m >= 0) for (int i = 0; i < SDK_SAVE_SLOTS; ++i) if (s->slot[i].used && s->slot[i].mount == m && s->slot[i].file && (s->slot[i].mode & 2)) FlushFileBuffers(s->slot[i].file);
    ReleaseSRWLockShared(&s->lock);
    return m >= 0 ? sdk_save_done(c, 0) : BOOT_HOST_DECLINE;
}

/* ---------------------------------------------------------------- path operations */
static inline int sdk_save_remove_tree(const wchar_t *dir) {
    wchar_t pattern[SDK_SAVE_WPATH + 4], child[SDK_SAVE_WPATH + MAX_PATH]; WIN32_FIND_DATAW fd;
    swprintf(pattern, SDK_SAVE_WPATH + 4, L"%ls\\*", dir);
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            swprintf(child, SDK_SAVE_WPATH + MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!sdk_save_remove_tree(child)) { FindClose(f); return 0; } }
            else if (!DeleteFileW(child)) { FindClose(f); return 0; }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(dir);
}
/* user: 0 CreateFile(path x0, s64 size x1), 1 DeleteFile(path), 2 CreateDirectory(path),
 *       3 DeleteDirectoryRecursively(path), 4 RenameFile(old x0, new x1). */
static inline int sdk_save_path_op(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target;
    unsigned op = (unsigned)(uintptr_t)user; int ro = 0;
    wchar_t path[SDK_SAVE_WPATH], other[SDK_SAVE_WPATH];
    int m = sdk_save_path(c, c->x[0], path, &ro);
    if (m == -1) return BOOT_HOST_DECLINE;
    if (m == -2) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    if (ro) return sdk_save_done(c, SVC_FS_RESULT_WRITE_NOT_PERMITTED);
    DWORD a = GetFileAttributesW(path);
    switch (op) {
    case 0: {
        if ((int64_t)c->x[1] < 0) return sdk_save_done(c, SVC_FS_RESULT_OUT_OF_RANGE);
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return sdk_save_done(c, a != INVALID_FILE_ATTRIBUTES ? SDK_SAVE_R_ALREADY_EXISTS : sdk_save_win32_result(GetLastError()));
        LARGE_INTEGER size; size.QuadPart = (LONGLONG)c->x[1];
        int ok = SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h);
        DWORD e = GetLastError(); CloseHandle(h);
        if (!ok) { DeleteFileW(path); return sdk_save_done(c, sdk_save_win32_result(e)); }
        return sdk_save_done(c, 0);
    }
    case 1:
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
        return sdk_save_done(c, DeleteFileW(path) ? 0 : sdk_save_win32_result(GetLastError()));
    case 2:
        if (a != INVALID_FILE_ATTRIBUTES) return sdk_save_done(c, SDK_SAVE_R_ALREADY_EXISTS);
        return sdk_save_done(c, CreateDirectoryW(path, NULL) ? 0 : sdk_save_win32_result(GetLastError()));
    case 3: {
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
        SdkSaveState *s = sdk_save(); AcquireSRWLockShared(&s->lock); int is_root = !_wcsicmp(path, s->mount[m].root); ReleaseSRWLockShared(&s->lock);
        if (is_root) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);   /* the SDK refuses the root */
        return sdk_save_done(c, sdk_save_remove_tree(path) ? 0 : sdk_save_win32_result(GetLastError()));
    }
    default: {
        int m2 = sdk_save_path(c, c->x[1], other, NULL);
        if (m2 == -1) return BOOT_HOST_DECLINE;
        if (m2 != m) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);   /* across mounts: not supported by fs either */
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
        if (GetFileAttributesW(other) != INVALID_FILE_ATTRIBUTES) return sdk_save_done(c, SDK_SAVE_R_ALREADY_EXISTS);
        return sdk_save_done(c, MoveFileExW(path, other, 0) ? 0 : sdk_save_win32_result(GetLastError()));
    }
    }
}
/* Result GetEntryType(DirectoryEntryType *out x0, const char *path x1) on a save mount; -1 = not ours. */
static inline int sdk_save_entry_type(BootContext *c, uint64_t self) {
    wchar_t path[SDK_SAVE_WPATH]; int m = sdk_save_path(c, c->x[1], path, NULL);
    if (m == -1) return -1;
    if (m == -2) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    if (!c->x[0]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    uint32_t type = (a & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
    if (!boot_host_write(c, self, c->x[0], &type, 4)) return BOOT_HOST_STOP;
    return sdk_save_done(c, 0);
}
/* Result OpenFile(FileHandle *out x0, const char *path x1, int mode w2); -1 = not ours. */
static inline int sdk_save_open_file(BootContext *c, uint64_t self) {
    wchar_t path[SDK_SAVE_WPATH]; int ro = 0; uint32_t mode = (uint32_t)c->x[2];
    int m = sdk_save_path(c, c->x[1], path, &ro);
    if (m == -1) return -1;
    if (m == -2) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    if (!c->x[0]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    if (!(mode & 3) || (mode & ~7u)) return sdk_save_done(c, SDK_SAVE_R_INVALID_ARG);
    if (ro && (mode & 6)) return sdk_save_done(c, SVC_FS_RESULT_WRITE_NOT_PERMITTED);
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    HANDLE h = CreateFileW(path, GENERIC_READ | ((mode & 6) ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return sdk_save_done(c, sdk_save_win32_result(GetLastError()));
    int i = sdk_save_claim();
    if (i < 0) { CloseHandle(h); return sdk_save_done(c, SDK_SAVE_R_TARGET_LOCKED); }
    SdkSaveState *s = sdk_save(); s->slot[i].file = h; s->slot[i].mode = mode; s->slot[i].mount = m;
    uint64_t handle = sdk_save_handle(SDK_SAVE_KIND_FILE, (unsigned)i, s->slot[i].generation);
    if (!boot_host_write(c, self, c->x[0], &handle, 8)) { sdk_save_release(i); return BOOT_HOST_STOP; }
    return sdk_save_done(c, 0);
}

/* ---------------------------------------------------------------- file handles */
static inline int sdk_save_file_size(HANDLE h, uint64_t *size) { LARGE_INTEGER v; if (!GetFileSizeEx(h, &v)) return 0; *size = (uint64_t)v.QuadPart; return 1; }
static inline int sdk_save_pio(HANDLE h, int write, uint64_t offset, void *buffer, uint32_t n) {
    OVERLAPPED o; memset(&o, 0, sizeof o); o.Offset = (DWORD)offset; o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    return (write ? WriteFile(h, buffer, n, &got, &o) : ReadFile(h, buffer, n, &got, &o)) && got == n;
}
/* ReadFile variants (as svc_fs_read_file, user = variant 0..3); -1 = not a save handle. */
static inline int sdk_save_read_file(BootContext *c, uint64_t self, unsigned variant) {
    int has_out = variant >= 2;
    uint64_t out = has_out ? c->x[0] : 0, handle = c->x[has_out], offset = c->x[has_out + 1], buffer = c->x[has_out + 2], size = c->x[has_out + 3];
    if (!sdk_save_kind(handle, SDK_SAVE_KIND_FILE)) return -1;
    int i = sdk_save_slot_of(handle, SDK_SAVE_KIND_FILE); uint64_t file_size, n;
    if (i < 0) return sdk_save_stale(c, self, "ReadFile", handle);
    SdkSaveState *s = sdk_save();
    if (!(s->slot[i].mode & 1)) return sdk_save_done(c, SDK_SAVE_R_READ_DENIED);
    if (!sdk_save_file_size(s->slot[i].file, &file_size)) return sdk_save_done(c, sdk_save_win32_result(GetLastError()));
    uint32_t result = svc_fs_read_plan(offset, buffer, size, file_size, has_out, out, &n);
    if (!result && n) {
        enum { CHUNK = 1 << 20 }; uint8_t *tmp = malloc(n < CHUNK ? (size_t)n : CHUNK); if (!tmp) return sdk_save_done(c, SDK_SAVE_R_NO_SPACE);
        for (uint64_t done = 0; done < n;) {
            uint32_t k = (uint32_t)(n - done < CHUNK ? n - done : CHUNK);
            if (!sdk_save_pio(s->slot[i].file, 0, offset + done, tmp, k)) { free(tmp); return sdk_save_done(c, sdk_save_win32_result(GetLastError())); }
            if (!boot_host_write(c, self, buffer + done, tmp, k)) { free(tmp); return BOOT_HOST_STOP; }
            done += k;
        }
        free(tmp); c->exclusive_valid = 0;
    }
    if (!result && has_out && !boot_host_write(c, self, out, &n, 8)) return BOOT_HOST_STOP;
    if (!result && !has_out && n != size) result = SVC_FS_RESULT_OUT_OF_RANGE;
    return sdk_save_done(c, result);
}
/* Result WriteFile(FileHandle x0, s64 offset x1, const void *buffer x2, size_t size x3, const WriteOption & x4) */
static inline int sdk_save_write_file(BootContext *c, uint64_t self) {
    uint64_t handle = c->x[0], offset = c->x[1], buffer = c->x[2], size = c->x[3], file_size; uint32_t flags = 0;
    if (!sdk_save_kind(handle, SDK_SAVE_KIND_FILE)) return -1;
    int i = sdk_save_slot_of(handle, SDK_SAVE_KIND_FILE);
    if (i < 0) return sdk_save_stale(c, self, "WriteFile", handle);
    SdkSaveState *s = sdk_save();
    if (c->x[4] && !boot_host_read(c, self, c->x[4], &flags, 4)) return BOOT_HOST_STOP;
    if (!(s->slot[i].mode & 2)) return sdk_save_done(c, SVC_FS_RESULT_WRITE_NOT_PERMITTED);
    if (size && !buffer) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    if ((int64_t)offset < 0 || (int64_t)size < 0 || (uint64_t)INT64_MAX - offset < size) return sdk_save_done(c, SVC_FS_RESULT_OUT_OF_RANGE);
    if (!sdk_save_file_size(s->slot[i].file, &file_size)) return sdk_save_done(c, sdk_save_win32_result(GetLastError()));
    if (offset + size > file_size && !(s->slot[i].mode & 4)) return sdk_save_done(c, SDK_SAVE_R_APPEND_DENIED);
    if (size) {
        enum { CHUNK = 1 << 20 }; uint8_t *tmp = malloc(size < CHUNK ? (size_t)size : CHUNK); if (!tmp) return sdk_save_done(c, SDK_SAVE_R_NO_SPACE);
        for (uint64_t done = 0; done < size;) {
            uint32_t k = (uint32_t)(size - done < CHUNK ? size - done : CHUNK);
            if (!boot_host_read(c, self, buffer + done, tmp, k)) { free(tmp); return BOOT_HOST_STOP; }
            if (!sdk_save_pio(s->slot[i].file, 1, offset + done, tmp, k)) { free(tmp); return sdk_save_done(c, sdk_save_win32_result(GetLastError())); }
            done += k;
        }
        free(tmp);
    }
    if (flags & 1) FlushFileBuffers(s->slot[i].file);
    return sdk_save_done(c, 0);
}
/* user: 0 FlushFile(h x0), 1 SetFileSize(h x0, s64 x1), 2 GetFileSize(s64* x0, h x1), 3 GetFileOpenMode(h x0), 4 CloseFile(h x0) */
static inline int sdk_save_handle_op(BootContext *c, uint64_t self, unsigned op) {
    uint64_t handle = c->x[op == 2 ? 1 : 0];
    if (!sdk_save_kind(handle, SDK_SAVE_KIND_FILE)) return -1;
    int i = sdk_save_slot_of(handle, SDK_SAVE_KIND_FILE);
    if (i < 0) return op == 4 ? BOOT_HOST_DONE : sdk_save_stale(c, self, "file handle function", handle);
    SdkSaveState *s = sdk_save(); uint64_t size;
    switch (op) {
    case 0: if (s->slot[i].mode & 2) FlushFileBuffers(s->slot[i].file); return sdk_save_done(c, 0);
    case 1: {
        if (!(s->slot[i].mode & 2)) return sdk_save_done(c, SVC_FS_RESULT_WRITE_NOT_PERMITTED);
        if ((int64_t)c->x[1] < 0) return sdk_save_done(c, SVC_FS_RESULT_OUT_OF_RANGE);
        FILE_END_OF_FILE_INFO eof; eof.EndOfFile.QuadPart = (LONGLONG)c->x[1];
        return sdk_save_done(c, SetFileInformationByHandle(s->slot[i].file, FileEndOfFileInfo, &eof, sizeof eof) ? 0 : sdk_save_win32_result(GetLastError()));
    }
    case 2:
        if (!c->x[0]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
        if (!sdk_save_file_size(s->slot[i].file, &size)) return sdk_save_done(c, sdk_save_win32_result(GetLastError()));
        if (!boot_host_write(c, self, c->x[0], &size, 8)) return BOOT_HOST_STOP;
        return sdk_save_done(c, 0);
    case 3: return sdk_save_done(c, s->slot[i].mode);
    default: sdk_save_release(i); return BOOT_HOST_DONE;
    }
}

/* ---------------------------------------------------------------- directories */
/* Result OpenDirectory(DirectoryHandle *out x0, const char *path x1, int mode w2); -1 = not ours. */
static inline int sdk_save_open_directory(BootContext *c, uint64_t self) {
    wchar_t path[SDK_SAVE_WPATH]; uint32_t mode = (uint32_t)c->x[2];
    int m = sdk_save_path(c, c->x[1], path, NULL);
    if (m == -1) return -1;
    if (m == -2) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    if (!c->x[0]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    if (!(mode & 3) || (mode & ~(UINT32_C(0x80000000) | 3u))) return sdk_save_done(c, SDK_SAVE_R_INVALID_ARG);
    if (!sdk_save_is_dir(path)) return sdk_save_done(c, SDK_SAVE_R_PATH_NOT_FOUND);
    int i = sdk_save_claim();
    if (i < 0) return sdk_save_done(c, SDK_SAVE_R_TARGET_LOCKED);
    SdkSaveState *s = sdk_save();
    s->slot[i].dir = _wcsdup(path); s->slot[i].mode = mode; s->slot[i].mount = m;
    uint64_t handle = sdk_save_handle(SDK_SAVE_KIND_DIR, (unsigned)i, s->slot[i].generation);
    if (!s->slot[i].dir) { sdk_save_release(i); return sdk_save_done(c, SDK_SAVE_R_NO_SPACE); }
    if (!boot_host_write(c, self, c->x[0], &handle, 8)) { sdk_save_release(i); return BOOT_HOST_STOP; }
    return sdk_save_done(c, 0);
}
/* Enumerates the directory: entries number `skip`.. are written (up to max) when entries != 0.
 * Returns the number of matching entries visited after `skip` (all of them when max < 0). */
static inline int64_t sdk_save_enum(BootContext *c, uint64_t self, int slot, uint32_t skip, uint64_t entries, int64_t max, int *fault) {
    SdkSaveState *s = sdk_save(); wchar_t pattern[SDK_SAVE_WPATH + 4]; WIN32_FIND_DATAW fd; int64_t n = 0; uint32_t index = 0;
    swprintf(pattern, SDK_SAVE_WPATH + 4, L"%ls\\*", s->slot[slot].dir);
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!(s->slot[slot].mode & (dir ? 1u : 2u))) continue;
        if (index++ < skip) continue;
        if (max >= 0 && n >= max) break;
        if (entries) {
            uint8_t e[SDK_FS_ENTRY_SIZE]; memset(e, 0, sizeof e);
            int k = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, (char *)e, 0x301, NULL, NULL);
            if (k <= 0) { sdk_save_log("unrepresentable file name skipped"); continue; }
            e[0x304] = (uint8_t)(dir ? 0 : 1);
            if (!dir && !(s->slot[slot].mode & UINT32_C(0x80000000))) { uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow; memcpy(e + 0x308, &size, 8); }
            if (!boot_host_write(c, self, entries + (uint64_t)n * SDK_FS_ENTRY_SIZE, e, sizeof e)) { *fault = 1; break; }
        }
        ++n;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}
/* user 0: ReadDirectory(s64* x0, DirectoryEntry* x1, h x2, s64 x3); 1: GetDirectoryEntryCount(s64* x0, h x1); 2: CloseDirectory(h x0) */
static inline int sdk_save_directory_op(BootContext *c, uint64_t self, unsigned op) {
    uint64_t handle = c->x[op == 0 ? 2 : op == 1 ? 1 : 0];
    if (!sdk_save_kind(handle, SDK_SAVE_KIND_DIR)) return -1;
    int i = sdk_save_slot_of(handle, SDK_SAVE_KIND_DIR), fault = 0;
    if (i < 0) return op == 2 ? BOOT_HOST_DONE : sdk_save_stale(c, self, "directory handle function", handle);
    SdkSaveState *s = sdk_save();
    if (op == 2) { sdk_save_release(i); return BOOT_HOST_DONE; }
    if (!c->x[0]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    if (op == 1) {
        int64_t n = sdk_save_enum(c, self, i, 0, 0, -1, &fault);
        if (!boot_host_write(c, self, c->x[0], &n, 8)) return BOOT_HOST_STOP;
        return sdk_save_done(c, 0);
    }
    int64_t max = (int64_t)c->x[3];
    if (max < 0) return sdk_save_done(c, SVC_FS_RESULT_OUT_OF_RANGE);
    if (max > 0 && !c->x[1]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    int64_t n = max ? sdk_save_enum(c, self, i, s->slot[i].cursor, c->x[1], max, &fault) : 0;
    if (fault || !boot_host_write(c, self, c->x[0], &n, 8)) return BOOT_HOST_STOP;
    s->slot[i].cursor += (uint32_t)n; c->exclusive_valid = 0;
    return sdk_save_done(c, 0);
}

/* ---------------------------------------------------------------- cache storage */
static inline void sdk_save_cache_dir(wchar_t *out, int index) {
    if (index < 0) swprintf(out, SDK_SAVE_WPATH, L"%ls\\cache\\%016llX", sdk_save_root(), (unsigned long long)SDK_SAVE_APP);
    else swprintf(out, SDK_SAVE_WPATH, L"%ls\\cache\\%016llX\\%d", sdk_save_root(), (unsigned long long)SDK_SAVE_APP, index);
}
/* Result CreateCacheStorage(int index w0, s64 size x1, s64 journal x2) */
static inline int sdk_save_create_cache(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!sdk_config()->fs_save) return BOOT_HOST_DECLINE;
    int index = (int)(int32_t)c->x[0]; wchar_t dir[SDK_SAVE_WPATH];
    if (index < 0 || index > 0xffff || (int64_t)c->x[1] < 0 || (int64_t)c->x[2] < 0) return BOOT_HOST_DECLINE;
    sdk_save_cache_dir(dir, index);
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) return sdk_save_done(c, SDK_SAVE_R_ALREADY_EXISTS);
    return sdk_save_done(c, sdk_save_mkdirs(dir) ? 0 : SDK_SAVE_R_NO_SPACE);
}
/* Result DeleteCacheStorage(int index w0): the volume is moved to <root>\deleted (recoverable). */
static inline int sdk_save_delete_cache(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!sdk_config()->fs_save) return BOOT_HOST_DECLINE;
    int index = (int)(int32_t)c->x[0]; wchar_t dir[SDK_SAVE_WPATH], dest[SDK_SAVE_WPATH], deleted[SDK_SAVE_WPATH];
    if (index < 0 || index > 0xffff) return BOOT_HOST_DECLINE;
    sdk_save_cache_dir(dir, index);
    if (!sdk_save_is_dir(dir)) return sdk_save_done(c, SDK_SAVE_R_TARGET_MISSING);
    SdkSaveState *s = sdk_save(); int busy = 0;
    AcquireSRWLockShared(&s->lock);
    for (int m = 0; m < SDK_SAVE_MOUNTS; ++m) if (s->mount[m].name[0] && !_wcsicmp(s->mount[m].root, dir)) busy = 1;
    ReleaseSRWLockShared(&s->lock);
    if (busy) return sdk_save_done(c, SDK_SAVE_R_TARGET_LOCKED);
    swprintf(deleted, SDK_SAVE_WPATH, L"%ls\\deleted", sdk_save_root());
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    swprintf(dest, SDK_SAVE_WPATH, L"%ls\\%llu-cache-%d", deleted, (unsigned long long)t.QuadPart, index);
    if (!sdk_save_mkdirs(deleted) || !MoveFileExW(dir, dest, 0)) return sdk_save_done(c, sdk_save_win32_result(GetLastError()));
    sdk_save_log("DeleteCacheStorage %d -> %ls", index, dest);
    return sdk_save_done(c, 0);
}
/* Result OpenCacheStorageList(CacheStorageListHandle *out x0): the existing volume indices, sorted. */
static inline int sdk_save_open_cache_list(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    if (!sdk_config()->fs_save) return BOOT_HOST_DECLINE;
    if (!c->x[0]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    int i = sdk_save_claim();
    if (i < 0) return BOOT_HOST_DECLINE;
    SdkSaveState *s = sdk_save(); wchar_t dir[SDK_SAVE_WPATH], pattern[SDK_SAVE_WPATH + 4]; WIN32_FIND_DATAW fd;
    sdk_save_cache_dir(dir, -1); swprintf(pattern, SDK_SAVE_WPATH + 4, L"%ls\\*", dir);
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *end; long v = wcstol(fd.cFileName, &end, 10);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] >= L'0' && fd.cFileName[0] <= L'9' && !*end && v >= 0 && v <= 0xffff &&
                s->slot[i].count < 64) {
                uint32_t k = s->slot[i].count++;
                while (k && s->slot[i].index[k - 1] > v) { s->slot[i].index[k] = s->slot[i].index[k - 1]; --k; }
                s->slot[i].index[k] = (int32_t)v;
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    uint64_t handle = sdk_save_handle(SDK_SAVE_KIND_LIST, (unsigned)i, s->slot[i].generation);
    if (!boot_host_write(c, self, c->x[0], &handle, 8)) { sdk_save_release(i); return BOOT_HOST_STOP; }
    return sdk_save_done(c, 0);
}
/* Result ReadCacheStorageList(int *out x0, CacheStorageInfo *infos x1 (0x20 bytes, index at +0), handle x2, int max w3) */
static inline int sdk_save_read_cache_list(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    if (!sdk_save_kind(c->x[2], SDK_SAVE_KIND_LIST)) return BOOT_HOST_DECLINE;
    int i = sdk_save_slot_of(c->x[2], SDK_SAVE_KIND_LIST), max = (int)(int32_t)c->x[3];
    if (i < 0) return sdk_save_stale(c, self, "ReadCacheStorageList", c->x[2]);
    if (max < 0) return sdk_save_done(c, SDK_SAVE_R_INVALID_ARG);
    if (!c->x[0] || !c->x[1]) return sdk_save_done(c, SVC_FS_RESULT_NULLPTR_ARGUMENT);
    SdkSaveState *s = sdk_save(); int32_t n = 0;
    while (n < max && s->slot[i].cursor + (uint32_t)n < s->slot[i].count) {
        uint8_t info[0x20]; memset(info, 0, sizeof info); memcpy(info, &s->slot[i].index[s->slot[i].cursor + (uint32_t)n], 4);
        if (!boot_host_write(c, self, c->x[1] + (uint64_t)n * 0x20, info, sizeof info)) return BOOT_HOST_STOP;
        ++n;
    }
    if (!boot_host_write(c, self, c->x[0], &n, 4)) return BOOT_HOST_STOP;
    s->slot[i].cursor += (uint32_t)n;
    return sdk_save_done(c, 0);
}
/* void CloseCacheStorageList(handle x0) */
static inline int sdk_save_close_cache_list(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!sdk_save_kind(c->x[0], SDK_SAVE_KIND_LIST)) return BOOT_HOST_DECLINE;
    int i = sdk_save_slot_of(c->x[0], SDK_SAVE_KIND_LIST);
    if (i >= 0) sdk_save_release(i);
    return BOOT_HOST_DONE;
}
/* bool IsSaveDataExisting(ApplicationId x0, const account::Uid & x1) */
static inline int sdk_save_exists(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!sdk_config()->fs_save) return BOOT_HOST_DECLINE;
    wchar_t dir[SDK_SAVE_WPATH];
    if (c->x[0] == SDK_SAVE_APP) swprintf(dir, SDK_SAVE_WPATH, L"%ls\\%016llX\\common", sdk_save_root(), (unsigned long long)SDK_SAVE_APP);
    else swprintf(dir, SDK_SAVE_WPATH, L"%ls\\%016llX", sdk_save_root(), (unsigned long long)c->x[0]);
    c->x[0] = (uint64_t)sdk_save_is_dir(dir);
    return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- hooks shared with host_svc_fs.h */
/* The save hook runs first; -1 hands the call to the svc.fs function registered at the same entry. */
typedef struct SdkSaveChain { uint32_t sdk_offset; unsigned op; BootHostFunction prev; void *prev_user; } SdkSaveChain;
static inline int sdk_save_chained(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    SdkSaveChain *ch = (SdkSaveChain *)user; int r;
    switch (ch->op) {
    case 0: r = sdk_save_open_file(c, self); break;
    case 1: case 2: case 3: case 4: r = sdk_save_read_file(c, self, ch->op - 1); break;
    case 5: r = sdk_save_handle_op(c, self, 2); break;   /* GetFileSize */
    case 6: r = sdk_save_handle_op(c, self, 4); break;   /* CloseFile */
    case 7: r = sdk_save_handle_op(c, self, 3); break;   /* GetFileOpenMode */
    case 8: r = sdk_save_entry_type(c, self); break;
    case 9: r = sdk_save_write_file(c, self); break;
    case 10: r = sdk_save_handle_op(c, self, 0); break;  /* FlushFile */
    default: r = sdk_save_handle_op(c, self, 1); break;  /* SetFileSize */
    }
    if (r != -1) return r;
    return ch->prev ? ch->prev(c, self, target, ch->prev_user) : BOOT_HOST_DECLINE;
}
static SdkSaveChain sdk_save_chain[] = {
    {0x9e1c0, 0, 0, 0}, {0x9c520, 1, 0, 0}, {0x9c430, 2, 0, 0}, {0x9c6a0, 3, 0, 0}, {0x9c610, 4, 0, 0}, {0x9ca50, 5, 0, 0},
    {0x9c3a0, 6, 0, 0}, {0x9cb60, 7, 0, 0}, {0x9dd00, 8, 0, 0}, {0x9c730, 9, 0, 0}, {0x9c860, 10, 0, 0}, {0x9c950, 11, 0, 0},
};
/* Wraps the svc.fs hooks (registered before, by boot_svc_fs_register) under their own names. Returns failures. */
static inline unsigned sdk_save_register_chain(void) {
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof sdk_save_chain / sizeof sdk_save_chain[0]; ++i) {
        SdkSaveChain *ch = &sdk_save_chain[i];
        uint64_t key = BOOT_HOST_KEY_GUEST | (UINT64_C(3) << 28) | ch->sdk_offset;
        BootHostFnEntry *e = boot_host_fn_find(key);
        if (!e) { ++failed; continue; }
        if (e->fn == sdk_save_chained) continue;                 /* already wrapped */
        ch->prev = e->fn; ch->prev_user = e->user;
        if (!boot_host_fn_add(key, e->name, sdk_save_chained, ch, e->flags, e->steps)) ++failed;
    }
    return failed;
}
/* Path functions only the save layer handles (no svc.fs hook at these entries). */
static inline int sdk_save_path_hook(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    if (!sdk_config()->fs_save) return BOOT_HOST_DECLINE;
    return sdk_save_path_op(c, self, target, user);
}
static const SdkEntry sdk_save_entries[] = {
    {0x3442d0, "sdk.fs.MountSaveData",              sdk_save_mount, 0, SDK_SAVE_STEPS},
    {0xd4b30,  "sdk.fs.MountTemporaryStorage",      sdk_save_mount, 1, SDK_SAVE_STEPS},
    {0xd4db0,  "sdk.fs.MountCacheStorage",          sdk_save_mount, 2, SDK_SAVE_STEPS},
    {0x344350, "sdk.fs.MountSaveDataReadOnly",      sdk_save_mount, 3, SDK_SAVE_STEPS},
    {0x9b520,  "sdk.fs.Unmount",                    sdk_save_unmount, 0, SDK_SAVE_STEPS},
    {0x9e6d0,  "sdk.fs.Commit",                     sdk_save_commit, 0, SDK_SAVE_STEPS},
    {0x9d030,  "sdk.fs.CreateFile",                 sdk_save_path_hook, 0, SDK_SAVE_STEPS},
    {0x9d040,  "sdk.fs.DeleteFile",                 sdk_save_path_hook, 1, SDK_SAVE_STEPS},
    {0x9d1c0,  "sdk.fs.CreateDirectory",            sdk_save_path_hook, 2, SDK_SAVE_STEPS},
    {0x9d4c0,  "sdk.fs.DeleteDirectoryRecursively", sdk_save_path_hook, 3, SDK_SAVE_STEPS},
    {0x9d7c0,  "sdk.fs.RenameFile",                 sdk_save_path_hook, 4, SDK_SAVE_STEPS},
    {0x3450f0, "sdk.fs.CreateCacheStorage",         sdk_save_create_cache, 0, SDK_SAVE_STEPS},
    {0xdb1f0,  "sdk.fs.DeleteCacheStorage",         sdk_save_delete_cache, 0, SDK_SAVE_STEPS},
    {0xdb500,  "sdk.fs.OpenCacheStorageList",       sdk_save_open_cache_list, 0, SDK_SAVE_STEPS},
    {0xdb660,  "sdk.fs.ReadCacheStorageList",       sdk_save_read_cache_list, 0, SDK_SAVE_STEPS},
    {0xdb8b0,  "sdk.fs.CloseCacheStorageList",      sdk_save_close_cache_list, 0, SDK_SAVE_STEPS},
    {0x344500, "sdk.fs.IsSaveDataExisting",         sdk_save_exists, 0, SDK_SAVE_STEPS},
};
#endif
