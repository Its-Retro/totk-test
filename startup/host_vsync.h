#ifndef TOTK_HOST_VSYNC_H
#define TOTK_HOST_VSYNC_H
#include <windows.h>
#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef HRESULT (WINAPI *BootDwmTiming)(HWND,DWM_TIMING_INFO*);
typedef struct BootVsync {
    HMODULE library;BootDwmTiming timing;HANDLE event,stop,thread;
    uint64_t refresh;volatile LONG failed;
} BootVsync;
/* Observe compositor refresh advances. Manual reset coalesces missed ticks;
 * this is desktop-compositor timing, not an exclusive GPU scanout interrupt. */
static DWORD WINAPI boot_vsync_thread(void*parameter){
    BootVsync*v=parameter;
    while(WaitForSingleObject(v->stop,2)==WAIT_TIMEOUT){
        DWM_TIMING_INFO timing={0};timing.cbSize=sizeof(timing);
        if(FAILED(v->timing(NULL,&timing))){InterlockedExchange(&v->failed,1);return 1;}
        if(timing.cRefresh>v->refresh){v->refresh=timing.cRefresh;SetEvent(v->event);}
        else if(timing.cRefresh<v->refresh)v->refresh=timing.cRefresh;
    }
    return 0;
}
/* svc-S1: the guest's display vsync event (vi 5202, wall-clock builds) at the Switch's 60 Hz from a high-resolution
 * waitable timer on absolute QPC deadlines: tick n at start + n*freq/60, so the rate never drifts; a tick more than
 * two periods late restarts the schedule at once instead of bursting. The compositor source above fired at the host
 * refresh (144 Hz on the Intel output, play-work/FRAME-PACING.md) and its thread ended for good when
 * DwmGetCompositionTimingInfo failed (display asleep), after which the guest vsync never fired again; this one needs
 * no DWM and runs until boot_vsync_close. As nxport's 60 Hz vsync pacer thread (MIT, behaviour only:
 * crates/engine/src/horizon/hle.rs:616-655). refresh counts the ticks. TOTK_VSYNC_SOURCE=dwm (or 0): the compositor
 * source, as before. */
static inline int boot_vsync_dwm(void){
    static int dwm=-1;
    if(dwm<0){char v[16];DWORD n=GetEnvironmentVariableA("TOTK_VSYNC_SOURCE",v,sizeof v);dwm=n && n<sizeof v && (!_stricmp(v,"dwm") || !strcmp(v,"0"));}
    return dwm;
}
static DWORD WINAPI boot_vsync_timer_thread(void*parameter){
    BootVsync*v=parameter;LARGE_INTEGER frequency,now;
    if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0)frequency.QuadPart=10000000;
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,0x00000002 /* HIGH_RESOLUTION */,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(NULL,TRUE,NULL);
    const int64_t f=frequency.QuadPart,period=f/60;
    QueryPerformanceCounter(&now);int64_t start=now.QuadPart;uint64_t n=1;
    for(;;){
        int64_t due=start+(int64_t)(n*(uint64_t)f/60);
        QueryPerformanceCounter(&now);
        if(now.QuadPart-due>2*period){start=due=now.QuadPart;n=0;}   /* resync: tick now, the grid restarts here */
        DWORD r;
        if(due>now.QuadPart){
            LARGE_INTEGER rel;rel.QuadPart=-(LONGLONG)((due-now.QuadPart)*10000000/f);HANDLE wait[2]={v->stop,timer};
            if(timer && rel.QuadPart<0 && SetWaitableTimer(timer,&rel,0,NULL,NULL,FALSE))r=WaitForMultipleObjects(2,wait,FALSE,INFINITE);
            else r=WaitForSingleObject(v->stop,(DWORD)((due-now.QuadPart)*1000/f));
        }else r=WaitForSingleObject(v->stop,0);
        if(r==WAIT_OBJECT_0)break;
        if(r==WAIT_FAILED)Sleep(1);   /* never spin */
        SetEvent(v->event);++v->refresh;++n;
    }
    if(timer)CloseHandle(timer);
    return 0;
}
static inline int boot_vsync_close(BootVsync*v){
    if(!v)return 1;
    if(v->thread){SetEvent(v->stop);if(WaitForSingleObject(v->thread,5000)!=WAIT_OBJECT_0)return 0;CloseHandle(v->thread);}
    if(v->event)CloseHandle(v->event);if(v->stop)CloseHandle(v->stop);
    if(v->library)FreeLibrary(v->library);free(v);return 1;
}
static inline BootVsync*boot_vsync_open(void){
    BootVsync*v=calloc(1,sizeof(*v));if(!v)return NULL;
    if(!boot_vsync_dwm()){   /* svc-S1: the 60 Hz timer source needs no compositor timing */
        v->event=CreateEventW(NULL,TRUE,FALSE,NULL);v->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(v->event && v->stop)v->thread=CreateThread(NULL,0,boot_vsync_timer_thread,v,0,NULL);
        if(!v->thread){boot_vsync_close(v);return NULL;}return v;
    }
    v->library=LoadLibraryW(L"dwmapi.dll");
    if(v->library){FARPROC procedure=GetProcAddress(v->library,"DwmGetCompositionTimingInfo");memcpy(&v->timing,&procedure,sizeof(procedure));}
    DWM_TIMING_INFO timing={0};timing.cbSize=sizeof(timing);
    if(!v->timing || FAILED(v->timing(NULL,&timing)) || !timing.qpcRefreshPeriod){boot_vsync_close(v);return NULL;}
    v->refresh=timing.cRefresh;v->event=CreateEventW(NULL,TRUE,FALSE,NULL);v->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(v->event && v->stop)v->thread=CreateThread(NULL,0,boot_vsync_thread,v,0,NULL);
    if(!v->thread){boot_vsync_close(v);return NULL;}return v;
}
#endif
