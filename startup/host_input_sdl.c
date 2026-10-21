/* SDL 3 gamepad backend of the runtime's npad state (see host_input_sdl.h for the contract and mapping).
 * Compiled by startup/third_party/build_sdl.py into sdl-build/libtotk_input.a with SDL's public headers;
 * the runtime itself never includes SDL. All SDL calls happen on one host thread started on first use. */
#define TOTK_HOST_INPUT_SDL_IMPL 1
#include "host_input_sdl.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

enum {SW_A=0,SW_B=1,SW_X=2,SW_Y=3,SW_LSTICK=4,SW_RSTICK=5,SW_L=6,SW_R=7,SW_ZL=8,SW_ZR=9,SW_PLUS=10,SW_MINUS=11,
      SW_LEFT=12,SW_UP=13,SW_RIGHT=14,SW_DOWN=15};
#define TOTK_TRIGGER_THRESHOLD 9830   /* 30 % of SDL's 0..32767 trigger range */
#define TOTK_GRAVITY 9.80665f
#define TOTK_TAU 6.28318530718f

static SRWLOCK totk_lock=SRWLOCK_INIT;
static TotkPadState totk_snapshot;
static struct {float value[2][4];uint64_t update_ms;uint32_t generation;} totk_vibration;
static volatile LONG totk_start_state;           /* 0 never, 1 starting/running, 2 disabled */
static volatile LONG totk_stop_request;
static volatile LONG totk_ready;                 /* 0 pending, 1 ready, -1 SDL_Init failed */
static HANDLE totk_thread,totk_ready_event;

/* ---- input thread state (only touched by the input thread) ------------------------------------------- */
static SDL_Gamepad*pad;
static SDL_JoystickID pad_id;
static int pad_has_accel,pad_has_gyro,pad_rumble_ok;
static float deadzone=0.08f;
static uint64_t connect_count;
static struct {
    int have_accel,have_gyro;
    float accel[3],gyro[3],rotation[3];
    float q[4];                    /* device-to-world rotation, w x y z */
    int q_valid;
    uint64_t last_gyro_ns;
} motion;
static uint32_t rumble_generation;
static uint64_t rumble_sent_ms;
static uint16_t rumble_low,rumble_high;

static void motion_reset(void){
    memset(&motion,0,sizeof motion);motion.q[0]=1.0f;
}

static void quat_to_rows(const float q[4],float m[9]){
    float w=q[0],x=q[1],y=q[2],z=q[3];
    m[0]=1-2*(y*y+z*z);m[1]=2*(x*y-w*z);  m[2]=2*(x*z+w*y);
    m[3]=2*(x*y+w*z);  m[4]=1-2*(x*x+z*z);m[5]=2*(y*z-w*x);
    m[6]=2*(x*z-w*y);  m[7]=2*(y*z+w*x);  m[8]=1-2*(x*x+y*y);
}
static void quat_normalize(float q[4]){
    float n=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if(n<1e-6f){q[0]=1;q[1]=q[2]=q[3]=0;return;}
    for(int i=0;i<4;++i)q[i]/=n;
}
/* Rotation taking the measured gravity direction d (device frame, unit) to world down (0,0,-1). */
static void quat_from_gravity(const float d[3],float q[4]){
    const float w[3]={0,0,-1};
    float c[3]={d[1]*w[2]-d[2]*w[1],d[2]*w[0]-d[0]*w[2],d[0]*w[1]-d[1]*w[0]};
    float dot=d[0]*w[0]+d[1]*w[1]+d[2]*w[2];
    if(dot<-0.9999f){q[0]=0;q[1]=1;q[2]=0;q[3]=0;return;}   /* upside down: 180 degrees about x */
    q[0]=1+dot;q[1]=c[0];q[2]=c[1];q[3]=c[2];quat_normalize(q);
}

/* Mahony step: gyro in rad/s (Switch axes), dt seconds; accelerometer gravity direction corrects tilt. */
static void motion_integrate(float dt){
    float g[3]={motion.gyro[0]*TOTK_TAU,motion.gyro[1]*TOTK_TAU,motion.gyro[2]*TOTK_TAU};
    for(int i=0;i<3;++i)motion.rotation[i]+=motion.gyro[i]*dt;
    float an=sqrtf(motion.accel[0]*motion.accel[0]+motion.accel[1]*motion.accel[1]+motion.accel[2]*motion.accel[2]);
    if(motion.have_accel && an>0.75f && an<1.25f){
        float d[3]={motion.accel[0]/an,motion.accel[1]/an,motion.accel[2]/an};
        if(!motion.q_valid){quat_from_gravity(d,motion.q);motion.q_valid=1;}
        /* estimated gravity direction in the device frame: R^T (0,0,-1) = -(third row of R) */
        float m[9];quat_to_rows(motion.q,m);
        float v[3]={-m[6],-m[7],-m[8]};
        float e[3]={d[1]*v[2]-d[2]*v[1],d[2]*v[0]-d[0]*v[2],d[0]*v[1]-d[1]*v[0]};
        const float kp=1.0f;
        for(int i=0;i<3;++i)g[i]+=kp*e[i];
    }
    float*q=motion.q,h=0.5f*dt;
    float w=q[0],x=q[1],y=q[2],z=q[3];
    q[0]=w+(-x*g[0]-y*g[1]-z*g[2])*h;
    q[1]=x+( w*g[0]+y*g[2]-z*g[1])*h;
    q[2]=y+( w*g[1]-x*g[2]+z*g[0])*h;
    q[3]=z+( w*g[2]+x*g[1]-y*g[0])*h;
    quat_normalize(q);
}

static void pad_close(void){
    if(pad){if(pad_rumble_ok)SDL_RumbleGamepad(pad,0,0,0);SDL_CloseGamepad(pad);}
    pad=NULL;pad_id=0;pad_has_accel=pad_has_gyro=pad_rumble_ok=0;rumble_low=rumble_high=0;motion_reset();
}

static void pad_open(SDL_JoystickID id){
    SDL_Gamepad*g=SDL_OpenGamepad(id);
    if(!g)return;
    pad=g;pad_id=id;++connect_count;motion_reset();
    pad_has_accel=SDL_GamepadHasSensor(g,SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_ACCEL,true);
    pad_has_gyro=SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_GYRO,true);
    SDL_PropertiesID props=SDL_GetGamepadProperties(g);
    pad_rumble_ok=props?SDL_GetBooleanProperty(props,SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false):0;
    SDL_SetGamepadPlayerIndex(g,0);
    rumble_generation=0;rumble_sent_ms=0;rumble_low=rumble_high=0;
    const char*name=SDL_GetGamepadName(g);
    fprintf(stderr,"[input] gamepad connected: %s (motion %s, rumble %s)\n",name?name:"?",
            pad_has_accel && pad_has_gyro?"yes":"no",pad_rumble_ok?"yes":"no");
}

static void pad_open_first(void){
    int count=0;SDL_JoystickID*ids=SDL_GetGamepads(&count);
    if(ids){if(count>0)pad_open(ids[0]);SDL_free(ids);}
}

static int32_t stick_axis(float v){long r=lroundf(v*32767.0f);return (int32_t)(r>32767?32767:r<-32767?-32767:r);}
static void map_stick(Sint16 sx,Sint16 sy,int32_t*ox,int32_t*oy){
    float x=sx/32767.0f,y=-(sy/32767.0f);   /* SDL: +y down; Switch: +y up */
    if(x<-1)x=-1;if(y<-1)y=-1;if(x>1)x=1;if(y>1)y=1;
    float m=sqrtf(x*x+y*y);
    if(m<=deadzone){*ox=*oy=0;return;}
    float scaled=(m-deadzone)/(1.0f-deadzone);if(scaled>1)scaled=1;
    float s=scaled/m;*ox=stick_axis(x*s);*oy=stick_axis(y*s);
}

static void publish(void){
    TotkPadState s;memset(&s,0,sizeof s);
    s.orientation[0]=s.orientation[4]=s.orientation[8]=1.0f;s.accel[2]=-1.0f;
    if(pad){
        static const struct {SDL_GamepadButton b;unsigned bit;} map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,SW_B},{SDL_GAMEPAD_BUTTON_EAST,SW_A},{SDL_GAMEPAD_BUTTON_WEST,SW_Y},
            {SDL_GAMEPAD_BUTTON_NORTH,SW_X},{SDL_GAMEPAD_BUTTON_LEFT_STICK,SW_LSTICK},{SDL_GAMEPAD_BUTTON_RIGHT_STICK,SW_RSTICK},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SW_L},{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SW_R},
            {SDL_GAMEPAD_BUTTON_START,SW_PLUS},{SDL_GAMEPAD_BUTTON_BACK,SW_MINUS},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,SW_LEFT},{SDL_GAMEPAD_BUTTON_DPAD_UP,SW_UP},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SW_RIGHT},{SDL_GAMEPAD_BUTTON_DPAD_DOWN,SW_DOWN}};
        s.connected=1;
        for(unsigned i=0;i<sizeof map/sizeof map[0];++i)if(SDL_GetGamepadButton(pad,map[i].b))s.buttons|=UINT64_C(1)<<map[i].bit;
        if(SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)>TOTK_TRIGGER_THRESHOLD)s.buttons|=UINT64_C(1)<<SW_ZL;
        if(SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)>TOTK_TRIGGER_THRESHOLD)s.buttons|=UINT64_C(1)<<SW_ZR;
        if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_GUIDE))s.system_buttons|=TOTK_PAD_SYSTEM_HOME;
        if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_MISC1) || SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_TOUCHPAD))
            s.system_buttons|=TOTK_PAD_SYSTEM_CAPTURE;
        map_stick(SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTX),SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTY),&s.lx,&s.ly);
        map_stick(SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHTX),SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHTY),&s.rx,&s.ry);
        if(motion.have_accel && motion.have_gyro){
            s.motion=1;memcpy(s.accel,motion.accel,sizeof s.accel);memcpy(s.gyro,motion.gyro,sizeof s.gyro);
            memcpy(s.rotation,motion.rotation,sizeof s.rotation);quat_to_rows(motion.q,s.orientation);
        }
        const char*name=SDL_GetGamepadName(pad);
        if(name){strncpy(s.name,name,sizeof s.name-1);}
    }
    s.connect_count=connect_count;
    AcquireSRWLockExclusive(&totk_lock);
    s.sequence=totk_snapshot.sequence+1;totk_snapshot=s;
    ReleaseSRWLockExclusive(&totk_lock);
}

static uint16_t rumble_strength(float a){
    if(!(a>0))return 0;if(a>1)a=1;
    float v=(a+powf(a,0.35f))*0.5f;           /* yuzu/suyu SDL curve for non-HD rumble motors */
    long r=lroundf(v*65535.0f);return (uint16_t)(r>65535?65535:r);
}

static void apply_rumble(void){
    if(!pad || !pad_rumble_ok)return;
    float v[2][4];uint64_t update_ms;uint32_t generation;
    AcquireSRWLockShared(&totk_lock);
    memcpy(v,totk_vibration.value,sizeof v);update_ms=totk_vibration.update_ms;generation=totk_vibration.generation;
    ReleaseSRWLockShared(&totk_lock);
    uint64_t now=GetTickCount64();
    /* low band (160 Hz nominal) drives the heavy low-frequency motor, high band (320 Hz) the light one; the
     * Pro Controller's two actuators (device 0 left, 1 right) are merged by taking the stronger of each band.
     * A value persists until the game sends another, as on hardware, but stops after 2 s without updates. */
    float lo=v[0][0]>v[1][0]?v[0][0]:v[1][0],hi=v[0][2]>v[1][2]?v[0][2]:v[1][2];
    if(update_ms && now-update_ms>2000)lo=hi=0;
    uint16_t low=rumble_strength(lo),high=rumble_strength(hi);
    int changed=generation!=rumble_generation && (low!=rumble_low || high!=rumble_high);
    int refresh=(low||high) && now-rumble_sent_ms>=100;
    int expire=!(low||high) && (rumble_low||rumble_high);
    rumble_generation=generation;
    if(!(changed||refresh||expire))return;
    SDL_RumbleGamepad(pad,low,high,low||high?250:0);
    rumble_low=low;rumble_high=high;rumble_sent_ms=now;
}

static void handle_event(const SDL_Event*e){
    switch(e->type){
    case SDL_EVENT_GAMEPAD_ADDED:
        if(!pad)pad_open(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if(pad && e->gdevice.which==pad_id){
            fprintf(stderr,"[input] gamepad disconnected\n");
            pad_close();pad_open_first();
        }
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if(pad && e->gsensor.which==pad_id){
            const float*d=e->gsensor.data;
            if(e->gsensor.sensor==SDL_SENSOR_ACCEL){
                motion.accel[0]=-d[0]/TOTK_GRAVITY;motion.accel[1]=d[2]/TOTK_GRAVITY;motion.accel[2]=-d[1]/TOTK_GRAVITY;
                motion.have_accel=1;
            }else if(e->gsensor.sensor==SDL_SENSOR_GYRO){
                motion.gyro[0]=d[0]/TOTK_TAU;motion.gyro[1]=-d[2]/TOTK_TAU;motion.gyro[2]=d[1]/TOTK_TAU;
                uint64_t t=e->gsensor.sensor_timestamp?e->gsensor.sensor_timestamp:e->common.timestamp;
                if(motion.have_gyro && t>motion.last_gyro_ns){
                    float dt=(float)((double)(t-motion.last_gyro_ns)*1e-9);
                    if(dt<=0.1f)motion_integrate(dt);
                }
                motion.have_gyro=1;motion.last_gyro_ns=t;
            }
        }
        break;
    default:break;
    }
}

static DWORD WINAPI input_thread(LPVOID arg){
    (void)arg;
    const char*dz=getenv("TOTK_INPUT_DEADZONE");
    if(dz && *dz){float p=(float)atof(dz);if(p>=0 && p<90)deadzone=p/100.0f;}
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");   /* no window: always receive input */
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION,"0");                   /* leave the process timer resolution alone */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED,"0");
    if(!SDL_Init(SDL_INIT_GAMEPAD)){
        fprintf(stderr,"[input] SDL_Init(GAMEPAD) failed: %s; keyboard only\n",SDL_GetError());
        InterlockedExchange(&totk_ready,-1);SetEvent(totk_ready_event);return 0;
    }
    motion_reset();
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(NULL,FALSE,NULL);
    pad_open_first();publish();
    InterlockedExchange(&totk_ready,1);SetEvent(totk_ready_event);
    while(!totk_stop_request){
        SDL_Event e;
        while(SDL_PollEvent(&e))handle_event(&e);
        if(!pad)pad_open_first();
        publish();apply_rumble();
        LARGE_INTEGER due;due.QuadPart=-20000;   /* 2 ms */
        if(timer && SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE))WaitForSingleObject(timer,10);
        else Sleep(1);
    }
    if(timer)CloseHandle(timer);
    pad_close();publish();
    SDL_Quit();
    return 0;
}

int totk_input_sdl_start(void){
    LONG state=InterlockedCompareExchange(&totk_start_state,1,0);
    if(state)return state==1;
    const char*mode=getenv("TOTK_INPUT");
    if(mode && *mode && _stricmp(mode,"sdl")!=0){InterlockedExchange(&totk_start_state,2);return 0;}
    totk_ready_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    totk_thread=totk_ready_event?CreateThread(NULL,0,input_thread,NULL,0,NULL):NULL;
    if(!totk_thread){InterlockedExchange(&totk_start_state,2);return 0;}
    SetThreadPriority(totk_thread,THREAD_PRIORITY_ABOVE_NORMAL);
    return 1;
}

int totk_input_sdl_wait_ready(unsigned timeout_ms){
    if(totk_start_state!=1 || !totk_ready_event)return 0;
    if(WaitForSingleObject(totk_ready_event,timeout_ms)!=WAIT_OBJECT_0)return 0;
    return totk_ready==1;
}

int totk_input_sdl_read(TotkPadState*out){
    AcquireSRWLockShared(&totk_lock);*out=totk_snapshot;ReleaseSRWLockShared(&totk_lock);
    return (int)out->connected;
}

void totk_input_sdl_vibrate(unsigned device,const float value[4]){
    if(device>1)return;
    float v[4];
    for(int i=0;i<4;++i)v[i]=isfinite(value[i])?value[i]:0.0f;
    AcquireSRWLockExclusive(&totk_lock);
    memcpy(totk_vibration.value[device],v,sizeof v);totk_vibration.update_ms=GetTickCount64();++totk_vibration.generation;
    if(!totk_vibration.update_ms)totk_vibration.update_ms=1;
    ReleaseSRWLockExclusive(&totk_lock);
}

void totk_input_sdl_stop(void){
    if(totk_start_state!=1 || !totk_thread)return;
    InterlockedExchange(&totk_stop_request,1);
    WaitForSingleObject(totk_thread,5000);CloseHandle(totk_thread);totk_thread=NULL;
    InterlockedExchange(&totk_stop_request,0);InterlockedExchange(&totk_start_state,0);InterlockedExchange(&totk_ready,0);
    if(totk_ready_event){CloseHandle(totk_ready_event);totk_ready_event=NULL;}
}
