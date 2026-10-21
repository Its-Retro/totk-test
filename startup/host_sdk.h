#ifndef TOTK_HOST_SDK_H
#define TOTK_HOST_SDK_H
/* Native SDK layer: host C functions in place of Nintendo SDK (sdk module) functions, registered with
 * the shared host-function registry (host_fn.h) as guest-entry hooks. Families:
 *   host_svc_fs.h   nn::fs on the RomFS mount: files (svc-work stage 1, switch "fs")
 *   host_sdk_fs.h   nn::fs on the RomFS mount: directories (switch "fs.dir")
 *   host_sdk_save.h nn::fs save data as host-directory mounts (switch "fs.save")
 *   host_sdk_hid.h  nn::hid: npad, six-axis, vibration, ShowControllerSupport from host input (switch "hid")
 *   host_sdk_time.h nn::time (switch "time") and nn::oe getters / notification switches (switch "oe")
 *   host_sdk_os.h   nn::os mutexes / critical sections / condition-variable signals (switch "os")
 *   host_sdk_nvdrv.h NvOsDrvIoctl nvmap / nvhost-as mapping requests on the runtime's GPU mapping state (switch "nvdrv")
 * Notes: startup/play-work/SDK.md.
 *
 * Rules shared by every family:
 *  - A hook implements the SDK function's documented, game-visible behaviour with host facilities and
 *    keeps all state in the SDK's own guest objects (same layouts and protocols), so a call that misses
 *    the hook (a direct branch inside one page) or that the hook declines runs the translated SDK code
 *    on the same state and stays consistent.
 *  - Anything not fully handled declines BEFORE changing guest-visible state (the translated SDK code
 *    then runs: contended locks reach the kernel through the SDK's own svc wrappers).
 *  - No IPC message is built or parsed.
 *
 * Switch: TOTK_NATIVE_SERVICES (comma list, shared with host_svc_fs.h): all | os | fs | fs.dir | fs.save | hid | time | oe | nvdrv ;
 * TOTK_PLAY builds default to "all" when the variable is unset; other builds default to none.
 * Runtime_support only (included under TOTK_DRAW_PATH), so the generated page header is unchanged. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#if defined(TOTK_PLAY) && TOTK_PLAY
#define SDK_DEFAULT_SERVICES "all"
#else
#define SDK_DEFAULT_SERVICES ""
#endif

typedef struct SdkConfig { int os, fs_dir, fs_save, hid, time, oe, nvdrv; } SdkConfig;
static inline SdkConfig *sdk_config_slot(void) { static SdkConfig cfg; return &cfg; }
static inline int sdk_token(const char *list, const char *token) {
    size_t n = strlen(token);
    for (const char *p = list; p && *p;) {
        while (*p == ',' || *p == ' ') ++p;
        const char *e = p; while (*e && *e != ',' && *e != ' ') ++e;
        if ((size_t)(e - p) == n && !_strnicmp(p, token, n)) return 1;
        p = e;
    }
    return 0;
}
static inline void sdk_config_load(void) {
    SdkConfig *cfg = sdk_config_slot();
    const char *s = getenv("TOTK_NATIVE_SERVICES");
    if (!s) s = SDK_DEFAULT_SERVICES;
    int all = sdk_token(s, "all");
    cfg->os = all || sdk_token(s, "os");
    cfg->fs_dir = all || sdk_token(s, "fs") || sdk_token(s, "fs.dir");
    cfg->fs_save = all || sdk_token(s, "fs") || sdk_token(s, "fs.save");
    cfg->hid = all || sdk_token(s, "hid");
    cfg->time = all || sdk_token(s, "time");
    cfg->oe = all || sdk_token(s, "oe");
    cfg->nvdrv = all || sdk_token(s, "nvdrv");
}
static inline const SdkConfig *sdk_config(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) { sdk_config_load(); InitOnceComplete(&once, 0, NULL); }
    return sdk_config_slot();
}

/* One hook: sdk module offset, registry name, function, user word. Tables are checked against the sdk's
 * dynamic symbols by play-work/sdk/tests/test_sdk.py. */
typedef struct SdkEntry { uint32_t sdk_offset; const char *name; BootHostFunction fn; uintptr_t user; uint32_t steps; } SdkEntry;
static inline unsigned sdk_register_table(const SdkEntry *t, unsigned n) {
    unsigned failed = 0;
    for (unsigned i = 0; i < n; ++i)
        /* os/save/fs/hid/time hooks never touch the renderer and lock their own state: no renderer lock
         * (host threads; file reads ran under it and serialized every loader thread with the GPU submit). */
        if (!boot_host_fn_hook(3, t[i].sdk_offset, t[i].name, t[i].fn, (void *)t[i].user, BOOT_HOST_FN_NOLOCK, t[i].steps)) ++failed;
    return failed;
}

#include "host_sdk_os.h"
#include "host_sdk_save.h"
#include "host_sdk_fs.h"
#include "host_sdk_hid.h"
#include "host_sdk_time.h"
#include "host_sdk_nvdrv.h"

/* Registers every SDK family (boot_host_fn_subsystems). Returns the number of hooks that failed. */
static inline unsigned boot_sdk_register(void) {
    return sdk_register_table(sdk_os_entries, (unsigned)(sizeof sdk_os_entries / sizeof sdk_os_entries[0])) +
           sdk_register_table(sdk_fs_entries, (unsigned)(sizeof sdk_fs_entries / sizeof sdk_fs_entries[0])) +
           sdk_register_table(sdk_save_entries, (unsigned)(sizeof sdk_save_entries / sizeof sdk_save_entries[0])) +
           sdk_register_table(sdk_hid_entries, (unsigned)(sizeof sdk_hid_entries / sizeof sdk_hid_entries[0])) +
           sdk_register_table(sdk_time_entries, (unsigned)(sizeof sdk_time_entries / sizeof sdk_time_entries[0])) +
           sdk_nvdrv_register() +
           sdk_save_register_chain();   /* after boot_svc_fs_register: wraps its OpenFile/ReadFile/... hooks */
}
#endif
