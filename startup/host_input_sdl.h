#ifndef TOTK_HOST_INPUT_SDL_H
#define TOTK_HOST_INPUT_SDL_H
/* Host gamepad input (SDL 3, startup/third_party/build_sdl.py) for the npad backend in boot_runtime.h.
 *
 * Split in two so the runtime never sees SDL headers:
 *   host_input_sdl.c  (compiled by build_sdl.py into sdl-build/libtotk_input.a, linked with libSDL3.a)
 *                     owns SDL: a host thread initialises the gamepad subsystem on first use (no window,
 *                     SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1, SDL_HINT_TIMER_RESOLUTION=0 so the process
 *                     timer resolution is untouched), pumps events every 2 ms, opens the first gamepad (hot
 *                     plug: the next one when it goes away), and publishes a snapshot already in Switch Pro
 *                     Controller terms (TotkPadState). Rumble requests are applied on that thread.
 *   this header       plain C: the snapshot type, the shim API (referenced weakly by the runtime, as libopus:
 *                     a DLL linked without the shim keeps the keyboard), and the shared-memory helpers the
 *                     npad tick uses (six-axis LIFO, vibration value decoding). No BootContext dependency.
 *
 * boot_runtime.h includes this header only under TOTK_DRAW_PATH (a runtime_support-only define), so the
 * generated page header (split_page_header.py -> boot_page.h) is unchanged.
 *
 * Mapping (by position, Switch layout; see startup/input-work/README.md for the full table):
 *   south (Xbox A / PS Cross / Nintendo B) = B, east = A, west = Y, north = X, LB/L1 = L, RB/R1 = R,
 *   LT/L2 = ZL, RT/R2 = ZR (pressed above 30 % travel), Start/Options = Plus, Back/Share(PS4)/Create = Minus,
 *   Guide = Home, Misc1 (Switch Capture, Xbox Share, PS5 Mic) or touchpad click = Capture, stick clicks,
 *   d-pad; sticks scaled to +-32767 with +y up and a radial dead zone (TOTK_INPUT_DEADZONE percent, default 8).
 *   Home/Capture are system buttons: on hardware the application never receives them in its npad state, so
 *   they are reported in TotkPadState.system_buttons only.
 * Six-axis (nn::hid::SixAxisSensorState, suyu input_common/drivers/sdl_driver.cpp conventions):
 *   accel in G, gyro in revolutions/s, Switch axes from SDL's (x right, y up, z toward the player):
 *   accel = (-ax, az, -ay)/9.80665, gyro = (gx, -gz, gy)/(2 pi); at rest flat accel = (0,0,-1).
 *   rotation = integrated gyro (revolutions), orientation = rows of the device-to-world rotation (Mahony
 *   filter with accelerometer tilt correction, identity yaw at connect).
 * Environment: TOTK_INPUT=sdl (default) | keyboard; TOTK_NO_CONTROLLER (runtime) still disconnects the pad. */
#include <stdint.h>
#include <string.h>

#define TOTK_PAD_SYSTEM_HOME    1u
#define TOTK_PAD_SYSTEM_CAPTURE 2u

typedef struct TotkPadState {
    uint32_t connected;          /* a gamepad is open */
    uint32_t motion;             /* accel and gyro are live (otherwise the fields hold the at-rest default) */
    uint64_t buttons;            /* nn::hid::NpadButton bits 0..15 (stick direction bits are left to the runtime) */
    int32_t lx,ly,rx,ry;         /* Switch stick range, +-32767, +y up, dead zone applied */
    uint32_t system_buttons;     /* TOTK_PAD_SYSTEM_* */
    uint32_t reserved;
    float accel[3];              /* G */
    float gyro[3];               /* revolutions per second */
    float rotation[3];           /* revolutions */
    float orientation[9];        /* 3 direction vectors (x, y, z), row-major */
    uint64_t sequence;           /* bumps on every published snapshot */
    uint64_t connect_count;      /* gamepads opened so far (hot-plug diagnostics) */
    char name[64];               /* SDL gamepad name, NUL-terminated */
} TotkPadState;

/* Shim API (host_input_sdl.c). Weak in the runtime; strong in tests. */
#ifndef TOTK_HOST_INPUT_SDL_IMPL
#define TOTK_HOST_INPUT_WEAK __attribute__((weak))
#else
#define TOTK_HOST_INPUT_WEAK
#endif
/* Starts the input thread once (later calls return the first result): 1 started, 0 disabled/failed. */
extern TOTK_HOST_INPUT_WEAK int totk_input_sdl_start(void);
/* Copies the latest snapshot; returns out->connected. Never blocks on SDL (SRW lock around a memcpy). */
extern TOTK_HOST_INPUT_WEAK int totk_input_sdl_read(TotkPadState*out);
/* nn::hid::VibrationValue for device 0 (left) / 1 (right): amp_low, freq_low, amp_high, freq_high. */
extern TOTK_HOST_INPUT_WEAK void totk_input_sdl_vibrate(unsigned device,const float value[4]);
/* Waits until the thread has initialised SDL (tests). 1 ready, 0 timeout or failure. */
extern TOTK_HOST_INPUT_WEAK int totk_input_sdl_wait_ready(unsigned timeout_ms);
/* Stops the thread and shuts SDL down (tests; the runtime never calls it). */
extern TOTK_HOST_INPUT_WEAK void totk_input_sdl_stop(void);

/* ---- shared-memory helpers for the npad tick (runtime side; not compiled into the shim) --------------- */
#ifndef TOTK_HOST_INPUT_SDL_IMPL
static inline void totk_input_le_write(uint8_t*p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=(uint8_t)(v>>(8*i));}
static inline uint64_t totk_input_le_read(const uint8_t*p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static inline void totk_input_put_f32(uint8_t*p,float f){uint32_t u;memcpy(&u,&f,4);totk_input_le_write(p,u,4);}

/* NpadInternalState six-axis LIFOs: Lifo<SixAxisSensorState,17> (0x708 bytes) x6 at entry+0x1758
 * (fullkey, handheld, dual left, dual right, left, right). Entry = u64 sampling number + 0x60-byte state:
 * delta_time ns, sampling_number, accel[3], gyro[3], rotation[3], orientation[3][3], attribute, reserved. */
#define TOTK_SIXAXIS_LIFO(entry,index) ((entry)+0x1758+(index)*0x708)
static inline void totk_input_sixaxis_write(uint8_t*lifo,uint64_t now_ms,uint64_t delta_ns,const TotkPadState*s,int live){
    static const float rest_accel[3]={0.0f,0.0f,-1.0f},zero[3]={0},identity[9]={1,0,0,0,1,0,0,0,1};
    uint64_t tail=totk_input_le_read(lifo+16,8),count=totk_input_le_read(lifo+24,8);
    if(tail>16)tail=0;
    uint64_t sampling=totk_input_le_read(lifo+32+tail*0x68+8+8,8)+1;
    if(count<16)totk_input_le_write(lifo+24,count+1,8);
    tail=(tail+1)%17;uint8_t*e=lifo+32+tail*0x68,*st=e+8;
    const float*accel=live?s->accel:rest_accel,*gyro=live?s->gyro:zero,*rot=live?s->rotation:zero,*orient=live?s->orientation:identity;
    totk_input_le_write(e,sampling,8);
    totk_input_le_write(st,delta_ns,8);totk_input_le_write(st+8,sampling,8);
    for(unsigned i=0;i<3;++i){totk_input_put_f32(st+16+4*i,accel[i]);totk_input_put_f32(st+28+4*i,gyro[i]);totk_input_put_f32(st+40+4*i,rot[i]);}
    for(unsigned i=0;i<9;++i)totk_input_put_f32(st+52+4*i,orient[i]);
    totk_input_le_write(st+88,1,4);totk_input_le_write(st+92,0,4);   /* attribute: is_connected */
    totk_input_le_write(lifo+16,tail,8);totk_input_le_write(lifo,now_ms,8);
}

/* StartSixAxisSensor (hid 66) / StopSixAxisSensor (67): one bit per SixAxisSensorHandle (style index x device
 * index). The game reads real motion only while a handle is started; otherwise the at-rest default (as suyu). */
static volatile long totk_input_sixaxis_started;
static inline void totk_input_sixaxis_handle(uint32_t handle,int start){
    unsigned bit=((handle&7u)<<2)|((handle>>16)&3u);
    long mask=(long)(1ul<<bit),old,next;
    do{old=totk_input_sixaxis_started;next=start?(old|mask):(old&~mask);}
    while(__sync_val_compare_and_swap(&totk_input_sixaxis_started,old,next)!=old);
}

/* VibrationDeviceHandle (u8 style index, u8 npad id, u8 device index, pad) + VibrationValue (4 x f32 LE). */
static inline void totk_input_vibration_forward(uint32_t handle,const uint8_t*value){
    if(!totk_input_sdl_vibrate)return;
    float v[4];
    for(unsigned i=0;i<4;++i){uint32_t u=(uint32_t)totk_input_le_read(value+4*i,4);memcpy(&v[i],&u,4);}
    unsigned device=(handle>>16)&0xff;
    if(device>1)return;
    totk_input_sdl_vibrate(device,v);
}
#endif /* !TOTK_HOST_INPUT_SDL_IMPL */
#endif
