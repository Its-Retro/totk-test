#ifndef TOTK_HOST_SDK_TIME_H
#define TOTK_HOST_SDK_TIME_H
/* nn::time (switch "time") and nn::oe getters (switch "oe") natively; included by host_sdk.h.
 *
 * time, as one family: with the switch on, nn::time::Initialize opens no time:u session and maps no
 * time shared memory; every time entry the game imports that needs either runs here:
 *   Initialize, IsInitialized, Finalize, StandardSteadyClock::now, StandardUserSystemClock::GetCurrentTime,
 *   StandardNetworkSystemClock::GetCurrentTime, ClockSnapshot::CreateWithStandardSystemClock, ToCalendarTime.
 * Values are exactly the runtime's time service (boot_runtime.h time:u handlers): the guest clock
 * (boot_guest_steady_unix, guest_time.h), steady clock source id "TOTK-Win-Time-01", the UTC device rule,
 * the 0xd0-byte ClockSnapshot layout. Pure SDK computation stays guest code (ClockSnapshot ctor,
 * CalculateSpanBetween, CalculateStandardUserSystemClockDifferenceByUser, ToCalendarTimeInUtc,
 * ToPosixTimeFromUtc). Before Initialize the functions return what the SDK returns uninitialized
 * (now() = 0, NotInitialized 0x74).
 *
 * oe: nn::oe::Initialize stays guest code (it opens the appletOE session that library applets and the
 * applet resource user id need), and so do Pop/TryPopNotificationMessage (their message event belongs to
 * the window message queue). Native: GetOperationMode (Handheld, as ICommonStateGetter 5 today),
 * GetPerformanceMode (Normal), GetCurrentFocusState and the Set{OperationMode,PerformanceMode}Changed /
 * Resume notification switches (the host window's own state, the same calls the IPC handlers make),
 * GetDesiredLanguage ("en-US", the pinned startup profile). Without a host window they decline.
 *
 * nn::account: nothing per call to replace: TryOpenPreselectedUser reads the state account::Initialize
 * stored (no IPC); Initialize itself stays guest code (startup only). */

#define SDK_TIME_STEPS 32
#define SDK_TIME_NOT_INITIALIZED UINT32_C(0x74)
static inline volatile LONG *sdk_time_initialized(void) { static volatile LONG v; return &v; }
static const uint8_t sdk_time_source_id[16] = {0x54,0x4f,0x54,0x4b,0x2d,0x57,0x69,0x6e,0x2d,0x54,0x69,0x6d,0x65,0x2d,0x30,0x31};

/* UTC civil date of a posix time: CalendarTime (8 bytes) and CalendarAdditionalInfo (0x18 bytes). */
static inline int sdk_time_calendar(int64_t t, uint8_t cal[8], uint8_t info[0x18]) {
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400), secs = t - days * 86400;
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9; y += m <= 2;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int start[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int64_t yday = start[m - 1] + d - 1 + (leap && m > 2), wday = ((days % 7) + 11) % 7;   /* 1970-01-01 was a Thursday */
    if (y < INT16_MIN || y > INT16_MAX) return 0;
    int16_t year = (int16_t)y; memset(cal, 0, 8); memcpy(cal, &year, 2);
    cal[2] = (uint8_t)m; cal[3] = (uint8_t)d; cal[4] = (uint8_t)(secs / 3600); cal[5] = (uint8_t)(secs / 60 % 60); cal[6] = (uint8_t)(secs % 60);
    uint32_t w = (uint32_t)wday, yd = (uint32_t)yday;
    memset(info, 0, 0x18); memcpy(info, &w, 4); memcpy(info + 4, &yd, 4); memcpy(info + 8, "UTC", 3);
    return 1;
}
/* The ClockSnapshot GetClockSnapshot(type) fills (time:u 400 handler): user and network contexts,
 * times, calendars, steady time point, location "UTC", automatic correction off, type. */
static inline void sdk_time_snapshot(uint8_t out[0xd0], uint64_t steady, int64_t unix_seconds, unsigned kind) {
    memset(out, 0, 0xd0);
    for (unsigned k = 0; k < 2; ++k) {
        uint64_t offset = (uint64_t)(unix_seconds - (int64_t)steady);
        memcpy(out + k * 0x20, &offset, 8); memcpy(out + k * 0x20 + 8, &steady, 8); memcpy(out + k * 0x20 + 16, sdk_time_source_id, 16);
        memcpy(out + 0x40 + k * 8, &unix_seconds, 8);
        (void)sdk_time_calendar(unix_seconds, out + 0x50 + k * 8, out + 0x60 + k * 0x18);
    }
    memcpy(out + 0x90, &steady, 8); memcpy(out + 0x98, sdk_time_source_id, 16);
    memcpy(out + 0xa8, "UTC", 3); out[0xcc] = 0; out[0xcd] = (uint8_t)kind;
}
#ifndef SDK_TIME_NOW
#define SDK_TIME_NOW(c, steady, unix_seconds) boot_guest_steady_unix(c, steady, unix_seconds)
#endif
/* user: 0 Initialize() -> Result, 1 IsInitialized() -> bool, 2 Finalize() -> Result, 3 StandardSteadyClock::now()
 * -> s64 seconds, 4 StandardUserSystemClock::GetCurrentTime(PosixTime* x0), 5 StandardNetworkSystemClock::
 * GetCurrentTime(PosixTime* x0), 6 ClockSnapshot::CreateWithStandardSystemClock(ClockSnapshot* x0),
 * 7 ToCalendarTime(CalendarTime* x0, CalendarAdditionalInfo* x1, const PosixTime& x2). */
static inline int sdk_time_call(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    unsigned op = (unsigned)(uintptr_t)user;
    if (!sdk_config()->time) return BOOT_HOST_DECLINE;
    volatile LONG *init = sdk_time_initialized();
    switch (op) {
    case 0: InterlockedExchange(init, 1); c->x[0] = 0; return BOOT_HOST_DONE;
    case 1: c->x[0] = *init != 0; return BOOT_HOST_DONE;
    case 2: InterlockedExchange(init, 0); c->x[0] = 0; return BOOT_HOST_DONE;
    default: break;
    }
    if (!*init) { c->x[0] = op == 3 ? 0 : SDK_TIME_NOT_INITIALIZED; return BOOT_HOST_DONE; }
    uint64_t steady; int64_t unix_seconds;
    if (op == 7) {
        int64_t t; uint8_t cal[8], info[0x18];
        if (!boot_host_read(c, self, c->x[2], &t, 8)) return BOOT_HOST_STOP;
        if (!sdk_time_calendar(t, cal, info)) return BOOT_HOST_DECLINE;      /* out of range: original path */
        if (!c->x[0] || !boot_host_write(c, self, c->x[0], cal, 8) || (c->x[1] && !boot_host_write(c, self, c->x[1], info, sizeof info)))
            return c->x[0] ? BOOT_HOST_STOP : BOOT_HOST_DECLINE;
        c->x[0] = 0; return BOOT_HOST_DONE;
    }
    if (!SDK_TIME_NOW(c, &steady, &unix_seconds)) return BOOT_HOST_DECLINE;
    if (op == 3) { c->x[0] = steady; return BOOT_HOST_DONE; }
    if (!c->x[0]) return BOOT_HOST_DECLINE;
    if (op == 6) {
        uint8_t snapshot[0xd0]; sdk_time_snapshot(snapshot, steady, unix_seconds, 1);   /* the SDK asks for type 1 */
        if (!boot_host_write(c, self, c->x[0], snapshot, sizeof snapshot)) return BOOT_HOST_STOP;
    } else if (!boot_host_write(c, self, c->x[0], &unix_seconds, 8)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- oe */
#ifndef SDK_OE_WINDOW
#define SDK_OE_WINDOW(c) ((BootWindow *)(uintptr_t)(c)->window_native)
#endif
/* user: 0 GetOperationMode -> u8 0 (Handheld), 1 GetPerformanceMode -> 0 (Normal), 2 GetCurrentFocusState -> u8,
 *       3 GetDesiredLanguage -> LanguageCode "en-US", 4 SetOperationModeChangedNotificationEnabled(bool),
 *       5 SetPerformanceModeChangedNotificationEnabled(bool), 6 SetResumeNotificationEnabled(bool). */
static inline int sdk_oe_call(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target;
    unsigned op = (unsigned)(uintptr_t)user;
    if (!sdk_config()->oe) return BOOT_HOST_DECLINE;
    BootWindow *w = SDK_OE_WINDOW(c);
    switch (op) {
    case 0: case 1: c->x[0] = 0; return BOOT_HOST_DONE;
    case 3: { uint64_t code = 0; memcpy(&code, "en-US", 5); c->x[0] = code; return BOOT_HOST_DONE; }
    default: break;
    }
    if (!w) return BOOT_HOST_DECLINE;
    int v = (int)(c->x[0] & 1);
    switch (op) {
    case 2: c->x[0] = (uint64_t)(uint8_t)boot_window_get_focus(w); return BOOT_HOST_DONE;
    case 4: case 5: if (!boot_window_set_mode_notification(w, op == 4 ? 11u : 12u, (uint8_t)v)) return BOOT_HOST_DECLINE; return BOOT_HOST_DONE;
    default: if (!boot_window_set_resume_notifications(w, (uint8_t)v)) return BOOT_HOST_DECLINE; return BOOT_HOST_DONE;
    }
}

static const SdkEntry sdk_time_entries[] = {
    {0x3cf8c0, "sdk.time.Initialize",                       sdk_time_call, 0, SDK_TIME_STEPS},
    {0x3cfbb0, "sdk.time.IsInitialized",                    sdk_time_call, 1, SDK_TIME_STEPS},
    {0x3cfbf0, "sdk.time.Finalize",                         sdk_time_call, 2, SDK_TIME_STEPS},
    {0x3d1d88, "sdk.time.StandardSteadyClock.now",          sdk_time_call, 3, SDK_TIME_STEPS},
    {0x3d20c4, "sdk.time.StandardUserSystemClock.GetCurrentTime",    sdk_time_call, 4, SDK_TIME_STEPS},
    {0x3d1aa4, "sdk.time.StandardNetworkSystemClock.GetCurrentTime", sdk_time_call, 5, SDK_TIME_STEPS},
    {0x3d1104, "sdk.time.ClockSnapshot.CreateWithStandardSystemClock", sdk_time_call, 6, SDK_TIME_STEPS},
    {0x3d23e0, "sdk.time.ToCalendarTime",                   sdk_time_call, 7, SDK_TIME_STEPS},
    {0x39bed0, "sdk.oe.GetOperationMode",                   sdk_oe_call, 0, SDK_TIME_STEPS},
    {0x39bf10, "sdk.oe.GetPerformanceMode",                 sdk_oe_call, 1, SDK_TIME_STEPS},
    {0x39bdd0, "sdk.oe.GetCurrentFocusState",               sdk_oe_call, 2, SDK_TIME_STEPS},
    {0x39c2b0, "sdk.oe.GetDesiredLanguage",                 sdk_oe_call, 3, SDK_TIME_STEPS},
    {0x39be50, "sdk.oe.SetOperationModeChangedNotificationEnabled",   sdk_oe_call, 4, SDK_TIME_STEPS},
    {0x39be90, "sdk.oe.SetPerformanceModeChangedNotificationEnabled", sdk_oe_call, 5, SDK_TIME_STEPS},
    {0x39be10, "sdk.oe.SetResumeNotificationEnabled",       sdk_oe_call, 6, SDK_TIME_STEPS},
};
#endif
