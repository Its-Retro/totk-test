#ifndef TOTK_HOST_SDK_HID_H
#define TOTK_HOST_SDK_HID_H
/* nn::hid natively, as one family (switch "hid"; included by host_sdk.h). With the switch on, no hid
 * session, shared memory or library applet is ever created: every hid entry the game imports that
 * reaches IPC or the hid shared memory runs here, reading the host input directly.
 *
 *   npad      InitializeNpad, SetSupportedNpadStyleSet, SetSupportedNpadIdType, SetNpadJoyHoldType,
 *             DisconnectNpad, GetNpadStyleSet, GetNpadStates (FullKey, Handheld, JoyDual, JoyLeft, JoyRight)
 *   vibration detail::SendVibrationValue / SendVibrationValues (the per-frame IPC behind
 *             VibrationNode::Update -> FlushEnqueuedVibrationValues), detail::InitializeVibrationDevice
 *             -> totk_input_sdl_vibrate (host_input_sdl.h)
 *   six-axis  StartSixAxisSensor, GetSixAxisSensorStates (SDL gyro/accel; at-rest default when not started)
 *   keyboard / mouse  Initialize*, GetKeyboardState, GetMouseStates: no host source (as today: empty LIFO)
 *   applet    ShowControllerSupport: 1 player, the connected npad, success
 * Pure SDK computation stays guest code (GetSixAxisSensorHandles, GetVibrationDeviceHandles, the
 * VibrationPlayer/Node/Mixer classes, ControllerSupportArg::SetDefault).
 *
 * Behaviour matches the runtime's npad backend (boot_hid_npad_tick): once the npad is initialized and
 * the supported style set has FullKey (or else Handheld), one wired Pro Controller is connected as npad
 * No1 (Handheld: npad Handheld) with the same keyboard map + SDL gamepad, the same attributes and stick
 * direction bits; TOTK_NO_CONTROLLER keeps it disconnected. States of other styles and other npads are
 * empty (count 0), as their never-written LIFOs. A new sample is taken when guest time advanced by at
 * least 1 ms since the previous one (sampling numbers +1, contiguous); up to 16 are kept and returned
 * newest first, as the SDK's LIFO read.
 * Invalid npad ids decline so the SDK aborts exactly as before. Host state (static, per process):
 * configuration, sample rings, started six-axis handles. */
#ifdef TOTK_DRAW_PATH
#include "host_input_sdl.h"
#endif

#define SDK_HID_STEPS 32
#define SDK_HID_RING  17
typedef struct SdkHidPad { uint64_t sampling, buttons; int32_t lx, ly, rx, ry; } SdkHidPad;
typedef struct SdkHidMotion { uint64_t sampling, delta_ns; float accel[3], gyro[3], rotation[3], orientation[9]; } SdkHidMotion;
typedef struct SdkHid {
    SRWLOCK lock;
    int initialized, disconnected; uint32_t supported_styles, hold_type, supported_ids;
    uint64_t pad_ms, motion_ms, motion_ns; int pad_have, motion_have;
    SdkHidPad pad[SDK_HID_RING]; unsigned pad_tail, pad_count;
    SdkHidMotion motion[SDK_HID_RING]; unsigned motion_tail, motion_count;
    volatile LONG sixaxis_started;
} SdkHid;
static inline SdkHid *sdk_hid(void) { static SdkHid h; return &h; }   /* zero = SRWLOCK_INIT */

/* npad ids the SDK accepts: No1..No8 (0..7), Other (0x10), Handheld (0x20). */
static inline int sdk_hid_valid_id(uint32_t id) { return id < 8 || id == 0x10 || id == 0x20; }
/* Connected style (1 FullKey, 2 Handheld, 0 none) and its npad id. */
static inline uint32_t sdk_hid_style(const SdkHid *h, uint32_t *id) {
    static int no_controller = -1;
    if (no_controller < 0) no_controller = getenv("TOTK_NO_CONTROLLER") != NULL;
    uint32_t style = !h->initialized || h->disconnected || no_controller ? 0 : (h->supported_styles & 1) ? 1 : (h->supported_styles & 2) ? 2 : 0;
    if (id) *id = style == 2 ? 0x20 : 0;
    return style;
}

/* ---------------------------------------------------------------- host input */
#ifndef SDK_HID_NOW_MS
/* svc-S3: the wall clock is QPC. GetTickCount64 advances in 15.6 ms steps, so a new pad/motion sample was taken only
 * every ~16 ms whatever the frame rate, and the motion delta_ns was 15 or 16 ms; nxport samples on every
 * GetNpadStates call (MIT, crates/engine/src/horizon/hle.rs:1617-1670). TOTK_HID_CLOCK=tick (or 0): GetTickCount64,
 * as before. Dilated guest time keeps its 1 ms steps. */
static inline int sdk_hid_qpc(void) {
    static int on = -1;
    if (on < 0) { char v[16]; DWORD n = GetEnvironmentVariableA("TOTK_HID_CLOCK", v, sizeof v); on = !(n && n < sizeof v && (!_stricmp(v, "tick") || !strcmp(v, "0"))); }
    return on;
}
static inline uint64_t sdk_hid_now_ns(BootContext *c) {   /* nanoseconds of the clock sdk_hid_now_ms counts */
    uint64_t t;
    if (boot_guest_clock_mode() == BOOT_GUEST_CLOCK_DILATED && boot_guest_clock_now(c, &t)) return t / (BOOT_GUEST_HZ / 1000) * 1000000u;
    if (!sdk_hid_qpc()) return GetTickCount64() * 1000000u;
    static LARGE_INTEGER f; LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (uint64_t)(q.QuadPart / f.QuadPart) * 1000000000u + (uint64_t)(q.QuadPart % f.QuadPart) * 1000000000u / (uint64_t)f.QuadPart;
}
static inline uint64_t sdk_hid_now_ms(BootContext *c) { return sdk_hid_now_ns(c) / 1000000u; }
#define SDK_HID_NOW_MS(c) sdk_hid_now_ms(c)
#define SDK_HID_NOW_NS(c) sdk_hid_now_ns(c)
#endif
#ifndef SDK_HID_NOW_NS
#define SDK_HID_NOW_NS(c) (SDK_HID_NOW_MS(c) * 1000000u)
#endif
#ifndef SDK_HID_SAMPLE
/* The runtime npad backend's mapping (boot_hid_npad_tick): keyboard when the game window has focus,
 * merged with the SDL gamepad (buttons OR'ed, a non-zero keyboard stick wins), then stick direction bits. */
static inline void sdk_hid_sample(SdkHidPad *p, SdkHidMotion *m) {
    uint64_t buttons = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    HWND fg = GetForegroundWindow(); DWORD fpid = 0; if (fg) GetWindowThreadProcessId(fg, &fpid);
    if (fpid == GetCurrentProcessId()) {
        static const struct { int key; unsigned bit; } map[] = {{'K',0},{'J',1},{'I',2},{'U',3},{'Q',6},{'E',7},{'1',8},{'3',9},
            {VK_RETURN,10},{VK_TAB,11},{VK_LEFT,12},{VK_UP,13},{VK_RIGHT,14},{VK_DOWN,15}};
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i) if (GetAsyncKeyState(map[i].key) & 0x8000) buttons |= UINT64_C(1) << map[i].bit;
        if (GetAsyncKeyState('A') & 0x8000) lx -= 32767;
        if (GetAsyncKeyState('D') & 0x8000) lx += 32767;
        if (GetAsyncKeyState('W') & 0x8000) ly += 32767;
        if (GetAsyncKeyState('S') & 0x8000) ly -= 32767;
        if (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) rx -= 32767;
        if (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) rx += 32767;
        if (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) ry += 32767;
        if (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) ry -= 32767;
    }
    static const float rest_accel[3] = {0.0f, 0.0f, -1.0f}, identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memset(m->accel, 0, sizeof m->accel + sizeof m->gyro + sizeof m->rotation); memcpy(m->accel, rest_accel, sizeof rest_accel);
    memcpy(m->orientation, identity, sizeof identity);
#ifdef TOTK_DRAW_PATH
    TotkPadState pad; memset(&pad, 0, sizeof pad);
    if (totk_input_sdl_start && totk_input_sdl_read && totk_input_sdl_start() && totk_input_sdl_read(&pad)) {
        buttons |= pad.buttons;
        if (!lx && !ly) { lx = pad.lx; ly = pad.ly; }
        if (!rx && !ry) { rx = pad.rx; ry = pad.ry; }
        if (pad.motion) {
            memcpy(m->accel, pad.accel, sizeof m->accel); memcpy(m->gyro, pad.gyro, sizeof m->gyro);
            memcpy(m->rotation, pad.rotation, sizeof m->rotation); memcpy(m->orientation, pad.orientation, sizeof m->orientation);
        }
    }
#endif
    p->buttons = buttons; p->lx = lx; p->ly = ly; p->rx = rx; p->ry = ry;
}
#define SDK_HID_SAMPLE(p, m) sdk_hid_sample(p, m)
#endif
static inline uint64_t sdk_hid_direction_bits(const SdkHidPad *p) {
    uint64_t b = 0;
    if (p->lx <= -16384) b |= 1u << 16;
    if (p->ly >= 16384) b |= 1u << 17;
    if (p->lx >= 16384) b |= 1u << 18;
    if (p->ly <= -16384) b |= 1u << 19;
    if (p->rx <= -16384) b |= 1u << 20;
    if (p->ry >= 16384) b |= 1u << 21;
    if (p->rx >= 16384) b |= 1u << 22;
    if (p->ry <= -16384) b |= 1u << 23;
    return b;
}
/* Takes a new sample when guest time moved by >= 1 ms (caller holds the lock exclusively). */
static inline void sdk_hid_update(BootContext *c, SdkHid *h, int motion) {
    uint64_t now_ns = SDK_HID_NOW_NS(c), now = now_ns / 1000000u;
    uint64_t *last = motion ? &h->motion_ms : &h->pad_ms; int *have = motion ? &h->motion_have : &h->pad_have;
    if (*have && now <= *last) return;
    SdkHidPad p; SdkHidMotion m; memset(&p, 0, sizeof p); memset(&m, 0, sizeof m);
    SDK_HID_SAMPLE(&p, &m);
    if (!motion) {
        SdkHidPad *prev = &h->pad[h->pad_tail];
        p.sampling = h->pad_count ? prev->sampling + 1 : 1; p.buttons |= sdk_hid_direction_bits(&p);
        h->pad_tail = h->pad_count ? (h->pad_tail + 1) % SDK_HID_RING : 0; h->pad[h->pad_tail] = p;
        if (h->pad_count < 16) ++h->pad_count;
    } else {
        SdkHidMotion *prev = &h->motion[h->motion_tail];
        m.sampling = h->motion_count ? prev->sampling + 1 : 1; m.delta_ns = *have ? now_ns - h->motion_ns : 0; h->motion_ns = now_ns;
        h->motion_tail = h->motion_count ? (h->motion_tail + 1) % SDK_HID_RING : 0; h->motion[h->motion_tail] = m;
        if (h->motion_count < 16) ++h->motion_count;
    }
    *last = now; *have = 1;
}

/* ---------------------------------------------------------------- npad */
static inline int sdk_hid_on(void) { return sdk_config()->hid; }
/* user: 0 InitializeNpad(), 1 SetSupportedNpadStyleSet(w0), 2 SetSupportedNpadIdType(ids x0, count x1),
 *       3 SetNpadJoyHoldType(w0), 4 DisconnectNpad(const u32& x0), 5 InitializeKeyboard(), 6 InitializeMouse(),
 *       7 GetKeyboardState(KeyboardState* x0) (empty LIFO: untouched), 8 detail::InitializeVibrationDevice(&) -> Result 0 */
static inline int sdk_hid_config_call(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    unsigned op = (unsigned)(uintptr_t)user; SdkHid *h = sdk_hid(); uint32_t v = (uint32_t)c->x[0];
    if (op == 1 && v == UINT32_MAX) return BOOT_HOST_DECLINE;    /* the SDK aborts */
    if (op == 2) {
        uint32_t ids[16]; uint64_t n = c->x[1];
        if (n > 16 || (n && !c->x[0])) return BOOT_HOST_DECLINE;
        if (n && !boot_host_read(c, self, c->x[0], ids, (size_t)n * 4)) return BOOT_HOST_STOP;
        v = 0; for (uint64_t i = 0; i < n; ++i) { if (!sdk_hid_valid_id(ids[i])) return BOOT_HOST_DECLINE; v |= 1u << (ids[i] < 8 ? ids[i] : ids[i] == 0x10 ? 8 : 9); }
    }
    if (op == 4) {
        if (!boot_host_read(c, self, c->x[0], &v, 4)) return BOOT_HOST_STOP;
        if (!sdk_hid_valid_id(v)) return BOOT_HOST_DECLINE;
    }
    AcquireSRWLockExclusive(&h->lock);
    switch (op) {
    case 0: h->initialized = 1; break;
    case 1: h->supported_styles = v; break;
    case 2: h->supported_ids = v; break;
    case 3: h->hold_type = v; break;
    case 4: { uint32_t id; if (sdk_hid_style(h, &id) && id == v) h->disconnected = 1; break; }
    default: break;
    }
    ReleaseSRWLockExclusive(&h->lock);
    if (op == 8) c->x[0] = 0;
    return BOOT_HOST_DONE;
}
/* BitFlagSet<32, NpadStyleTag> GetNpadStyleSet(const NpadIdType & x0) */
static inline int sdk_hid_style_set(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint32_t want, id; SdkHid *h = sdk_hid();
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    if (!boot_host_read(c, self, c->x[0], &want, 4)) return BOOT_HOST_STOP;
    if (!sdk_hid_valid_id(want)) return BOOT_HOST_DECLINE;
    AcquireSRWLockShared(&h->lock); uint32_t style = sdk_hid_style(h, &id); ReleaseSRWLockShared(&h->lock);
    c->x[0] = style && id == want ? style : 0;
    return BOOT_HOST_DONE;
}
/* int GetNpadStates(Npad<Style>State *out x0, int count w1, const NpadIdType & x2); user = style bit of the
 * state type (1 FullKey, 2 Handheld, 4 JoyDual, 8 JoyLeft, 16 JoyRight). State: sampling, buttons, sticks
 * (4 x s32), attributes, reserved = 0x28 bytes. */
static inline int sdk_hid_npad_states(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    uint32_t want, id, type = (uint32_t)(uintptr_t)user; int count = (int)(int32_t)c->x[1]; SdkHid *h = sdk_hid();
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    if (!boot_host_read(c, self, c->x[2], &want, 4)) return BOOT_HOST_STOP;
    if (!sdk_hid_valid_id(want)) return BOOT_HOST_DECLINE;
    AcquireSRWLockExclusive(&h->lock);
    uint32_t style = sdk_hid_style(h, &id); int n = 0;
    uint8_t out[16 * 0x28];
    if (style && style == type && id == want && count > 0) {
        sdk_hid_update(c, h, 0);
        n = count < (int)h->pad_count ? count : (int)h->pad_count;
        uint32_t attributes = style == 1 ? 3 : 0x3f;   /* connected + wired (+ left/right connected + wired) */
        for (int i = 0; i < n; ++i) {
            const SdkHidPad *p = &h->pad[(h->pad_tail + SDK_HID_RING - (unsigned)i) % SDK_HID_RING]; uint8_t *e = out + i * 0x28;
            memset(e, 0, 0x28); memcpy(e, &p->sampling, 8); memcpy(e + 8, &p->buttons, 8);
            memcpy(e + 16, &p->lx, 4); memcpy(e + 20, &p->ly, 4); memcpy(e + 24, &p->rx, 4); memcpy(e + 28, &p->ry, 4); memcpy(e + 32, &attributes, 4);
        }
    }
    ReleaseSRWLockExclusive(&h->lock);
    if (n && !boot_host_write(c, self, c->x[0], out, (size_t)n * 0x28)) return BOOT_HOST_STOP;
    c->x[0] = (uint64_t)(uint32_t)n;
    return BOOT_HOST_DONE;
}
/* int GetMouseStates(MouseState* x0, int count w1): no host mouse (empty LIFO). */
static inline int sdk_hid_zero_count(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)self; (void)target; (void)user;
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    c->x[0] = 0; return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- six-axis */
/* void StartSixAxisSensor(const SixAxisSensorHandle & x0): handle bytes = style index, npad id, device index. */
static inline int sdk_hid_sixaxis_start(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint32_t handle; SdkHid *h = sdk_hid();
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    if (!boot_host_read(c, self, c->x[0], &handle, 4)) return BOOT_HOST_STOP;
    unsigned bit = ((handle & 7u) << 2) | ((handle >> 16) & 3u);
    InterlockedOr(&h->sixaxis_started, (LONG)(1ul << bit));
    return BOOT_HOST_DONE;
}
/* int GetSixAxisSensorStates(SixAxisSensorState *out x0, int count w1, const SixAxisSensorHandle & x2)
 * State (0x60): delta ns, sampling, accel[3], gyro[3], rotation[3], orientation[3][3], attributes, reserved. */
static inline int sdk_hid_sixaxis_states(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint32_t handle, id; int count = (int)(int32_t)c->x[1]; SdkHid *h = sdk_hid();
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    if (!boot_host_read(c, self, c->x[2], &handle, 4)) return BOOT_HOST_STOP;
    unsigned bit = ((handle & 7u) << 2) | ((handle >> 16) & 3u);
    static const float rest_accel[3] = {0.0f, 0.0f, -1.0f}, identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    uint8_t out[16 * 0x60]; int n = 0;
    AcquireSRWLockExclusive(&h->lock);
    if (sdk_hid_style(h, &id) && ((handle >> 8) & 0xff) == id && count > 0) {
        int live = (h->sixaxis_started >> bit) & 1;
        sdk_hid_update(c, h, 1);
        n = count < (int)h->motion_count ? count : (int)h->motion_count;
        for (int i = 0; i < n; ++i) {
            const SdkHidMotion *m = &h->motion[(h->motion_tail + SDK_HID_RING - (unsigned)i) % SDK_HID_RING]; uint8_t *e = out + i * 0x60;
            float zero[3] = {0, 0, 0}; uint32_t attributes = 1;
            memset(e, 0, 0x60); memcpy(e, &m->delta_ns, 8); memcpy(e + 8, &m->sampling, 8);
            memcpy(e + 16, live ? m->accel : rest_accel, 12); memcpy(e + 28, live ? m->gyro : zero, 12);
            memcpy(e + 40, live ? m->rotation : zero, 12); memcpy(e + 52, live ? m->orientation : identity, 36); memcpy(e + 88, &attributes, 4);
        }
    }
    ReleaseSRWLockExclusive(&h->lock);
    if (n && !boot_host_write(c, self, c->x[0], out, (size_t)n * 0x60)) return BOOT_HOST_STOP;
    c->x[0] = (uint64_t)(uint32_t)n;
    return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- vibration */
static inline void sdk_hid_vibrate(uint32_t handle, const uint8_t value[16]) {
#ifdef TOTK_DRAW_PATH
    totk_input_vibration_forward(handle, value);
#else
    (void)handle; (void)value;
#endif
}
/* Result detail::SendVibrationValue(const VibrationDeviceHandle & x0, const VibrationValue & x1)      user 0
 * Result detail::SendVibrationValues(const VibrationDeviceHandle * x0, const VibrationValue * x1, int w2) user 1 */
static inline int sdk_hid_send_vibration(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    if (!sdk_hid_on()) return BOOT_HOST_DECLINE;
    int many = user != NULL, n = many ? (int)(int32_t)c->x[2] : 1;
    if (n < 0 || n > 256) return BOOT_HOST_DECLINE;
    for (int i = 0; i < n; ++i) {
        uint32_t handle; uint8_t value[16];
        if (!boot_host_read(c, self, c->x[0] + 4 * (uint64_t)i, &handle, 4) || !boot_host_read(c, self, c->x[1] + 16 * (uint64_t)i, value, 16)) return BOOT_HOST_STOP;
        sdk_hid_vibrate(handle, value);
    }
    c->x[0] = 0; return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- applet */
/* Result ShowControllerSupport(ControllerSupportResultInfo *out x0 (s8 players, pad, u32 npad id), const Arg & x1) */
static inline int sdk_hid_controller_support(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint32_t id = 0; SdkHid *h = sdk_hid();
    if (!sdk_hid_on() || !c->x[0]) return BOOT_HOST_DECLINE;
    AcquireSRWLockShared(&h->lock); uint32_t style = sdk_hid_style(h, &id); ReleaseSRWLockShared(&h->lock);
    if (!style) id = (h->supported_styles & 1) || !(h->supported_styles & 2) ? 0 : 0x20;
    uint8_t info[8] = {1, 0, 0, 0}; memcpy(info + 4, &id, 4);
    if (!boot_host_write(c, self, c->x[0], info, 8)) return BOOT_HOST_STOP;
    c->x[0] = 0; return BOOT_HOST_DONE;
}

static const SdkEntry sdk_hid_entries[] = {
    {0x49e8b0, "sdk.hid.InitializeNpad",            sdk_hid_config_call, 0, SDK_HID_STEPS},
    {0x49e8e0, "sdk.hid.SetSupportedNpadStyleSet",  sdk_hid_config_call, 1, SDK_HID_STEPS},
    {0x49e960, "sdk.hid.SetSupportedNpadIdType",    sdk_hid_config_call, 2, SDK_HID_STEPS},
    {0x49f440, "sdk.hid.SetNpadJoyHoldType",        sdk_hid_config_call, 3, SDK_HID_STEPS},
    {0x49ea60, "sdk.hid.DisconnectNpad",            sdk_hid_config_call, 4, SDK_HID_STEPS},
    {0x49e700, "sdk.hid.InitializeKeyboard",        sdk_hid_config_call, 5, SDK_HID_STEPS},
    {0x49e7d0, "sdk.hid.InitializeMouse",           sdk_hid_config_call, 6, SDK_HID_STEPS},
    {0x49e730, "sdk.hid.GetKeyboardState",          sdk_hid_config_call, 7, SDK_HID_STEPS},
    {0x49d8c0, "sdk.hid.detail.InitializeVibrationDevice", sdk_hid_config_call, 8, SDK_HID_STEPS},
    {0x49e9f0, "sdk.hid.GetNpadStyleSet",           sdk_hid_style_set, 0, SDK_HID_STEPS},
    {0x49eed0, "sdk.hid.GetNpadStates.FullKey",     sdk_hid_npad_states, 1, SDK_HID_STEPS},
    {0x49efb0, "sdk.hid.GetNpadStates.Handheld",    sdk_hid_npad_states, 2, SDK_HID_STEPS},
    {0x49f670, "sdk.hid.GetNpadStates.JoyDual",     sdk_hid_npad_states, 4, SDK_HID_STEPS},
    {0x49f750, "sdk.hid.GetNpadStates.JoyLeft",     sdk_hid_npad_states, 8, SDK_HID_STEPS},
    {0x49f830, "sdk.hid.GetNpadStates.JoyRight",    sdk_hid_npad_states, 16, SDK_HID_STEPS},
    {0x49e830, "sdk.hid.GetMouseStates",            sdk_hid_zero_count, 0, SDK_HID_STEPS},
    {0x4a03b0, "sdk.hid.StartSixAxisSensor",        sdk_hid_sixaxis_start, 0, SDK_HID_STEPS},
    {0x4a0630, "sdk.hid.GetSixAxisSensorStates",    sdk_hid_sixaxis_states, 0, SDK_HID_STEPS},
    {0x49dab0, "sdk.hid.detail.SendVibrationValue",  sdk_hid_send_vibration, 0, SDK_HID_STEPS},
    {0x49db30, "sdk.hid.detail.SendVibrationValues", sdk_hid_send_vibration, 1, SDK_HID_STEPS},
    {0x4bf480, "sdk.hid.ShowControllerSupport",     sdk_hid_controller_support, 0, SDK_HID_STEPS},
};
#endif
