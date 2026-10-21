#ifndef TOTK_HOST_PERFORMANCE_H
#define TOTK_HOST_PERFORMANCE_H
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct BootPerformance {
    HANDLE thread;
    DWORD thread_id;
    THREAD_POWER_THROTTLING_STATE previous;
    uint32_t configuration;
} BootPerformance;
static inline int boot_performance_close(BootPerformance*p){
    if(!p)return 1;
    DWORD exit_code=STILL_ACTIVE;
    int restored=SetThreadInformation(p->thread,ThreadPowerThrottling,&p->previous,sizeof(p->previous)) ||
        (GetExitCodeThread(p->thread,&exit_code) && exit_code!=STILL_ACTIVE);
    CloseHandle(p->thread);free(p);return restored;
}
static inline int boot_performance_set(BootPerformance**slot,uint32_t mode,uint32_t configuration){
    /* Supported handheld profile maps to Windows HighQoS, not Switch clocks. */
    if(!slot || mode!=0 || configuration!=UINT32_C(0x92220008))return 0;
    BootPerformance*p=*slot;int created=p==NULL;
    if(p && p->thread_id!=GetCurrentThreadId())return 0;
    if(created){
        p=calloc(1,sizeof(*p));if(!p)return 0;
        p->previous.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&p->thread,0,FALSE,DUPLICATE_SAME_ACCESS)){
            free(p);return 0;
        }
        if(!GetThreadInformation(p->thread,ThreadPowerThrottling,&p->previous,sizeof(p->previous))){
            CloseHandle(p->thread);free(p);return 0;
        }
        p->thread_id=GetCurrentThreadId();
    }
    THREAD_POWER_THROTTLING_STATE desired=p->previous;
    desired.ControlMask|=THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    desired.StateMask&=~THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    if(!SetThreadInformation(p->thread,ThreadPowerThrottling,&desired,sizeof(desired))){
        if(created){CloseHandle(p->thread);free(p);}return 0;
    }
    p->configuration=configuration;*slot=p;return 1;
}

/* play (62): host power/QoS tuning for the game's threads. All of it is per process / per thread (documented
 * Win32 APIs; nothing system-wide, nothing persists after exit), all of it is best effort (an older Windows
 * returns FALSE and the game runs as before), and TOTK_THREAD_QOS=0 / TOTK_PROCESS_TUNING=0 turn them off.
 *
 * boot_host_thread_high_qos: opt the calling thread out of EcoQoS ("execution speed" power throttling). On a hybrid
 * CPU (Alder Lake and later) a throttled thread is steered to the E-cores at a lower clock, and Windows 11 applies
 * it by itself to threads of a process it considers background (hidden or occluded window, efficiency mode). The
 * process-wide opt-out (totk_launcher.c timer_precision) covers threads that follow the process default; this makes
 * the decision explicit for every guest worker, the timer thread and the renderer helpers, whatever state the process
 * default is in. Same call and same masks as the SetPerformanceConfiguration handler above (boot_performance_set). */
static inline int boot_host_thread_high_qos(void){
    static int enabled=-1;
    if(enabled<0){const char*v=getenv("TOTK_THREAD_QOS");enabled=!(v && !strcmp(v,"0"));}
    if(!enabled)return 0;
    THREAD_POWER_THROTTLING_STATE s={0};   /* no <string.h>/<stdarg.h>: includes reach the page header (split_page_header.py) */
    s.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask=THREAD_POWER_THROTTLING_EXECUTION_SPEED;s.StateMask=0;   /* controlled, and not throttled */
    return SetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&s,sizeof s)!=0;
}
/* boot_host_process_tuning: what totk_launcher.c timer_precision() does (1 ms timer resolution, execution-speed
 * throttling off, timer-resolution requests honoured), for a game DLL started by some other host (probe scripts,
 * test harnesses) so that its timings compare with the launcher's. Once per process; does nothing when the
 * process is already in the launcher's state, so the normal totk.exe path is unchanged. */
static inline void boot_host_process_tuning(void){
    static volatile LONG done;
    if(InterlockedExchange(&done,1))return;
    const char*off=getenv("TOTK_PROCESS_TUNING");if(off && !strcmp(off,"0"))return;
    const ULONG both=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    PROCESS_POWER_THROTTLING_STATE s={0};s.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if(GetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&s,sizeof s) && (s.ControlMask&both)==both && !(s.StateMask&both))return;   /* the launcher did it */
    s=(PROCESS_POWER_THROTTLING_STATE){0};s.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask=both;s.StateMask=0;
    SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&s,sizeof s);
    HMODULE winmm=LoadLibraryW(L"winmm.dll");
    typedef unsigned(WINAPI*TimePeriodFn)(unsigned);TimePeriodFn begin=NULL;
    FARPROC fn=winmm?GetProcAddress(winmm,"timeBeginPeriod"):NULL;if(fn)begin=(TimePeriodFn)(void*)fn;
    if(begin)begin(1);   /* released by the OS at process exit, like the launcher's */
}
#endif
