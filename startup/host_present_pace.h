#ifndef TOTK_HOST_PRESENT_PACE_H
#define TOTK_HOST_PRESENT_PACE_H
/* Even host presentation (runtime_support only, included by host_gpu_present.h).
 *
 * The guest runs slower than real time, so frames become ready at uneven real intervals (host frame
 * cost varies, ~0.4-0.6 s). Showing each one as soon as it is ready makes the motion uneven even when
 * the guest time per frame is constant (guest_clock.h frame lock). When frames are slower than
 * 45 ms apart on average, a pacer thread calls IDXGISwapChain::Present at a steady cadence instead:
 * target = max(ready, previous target + cadence), cadence = 0.97 x moving average of the ready
 * intervals (EMA 1/8; a gap over 4x the average or 2 s restarts it), and a frame is never held more
 * than one cadence. The copy into the back buffer still happens on the guest thread; before it
 * touches the swapchain again it waits until the pacer has presented the previous frame, which only
 * blocks when two frames become ready within one cadence. Faster frames present at once, exactly as
 * before. Off by default (it blocked the game thread for up to a cadence per frame); TOTK_PRESENT_PACE=1 enables. Host real time only: no guest-visible effect.
 *
 * The pacer holds its own reference to the swapchain it presents, so a swapchain released or
 * recreated by the guest thread (resize, device recovery) stays valid until the pending Present. */
#include "host_present_trace.h"

/* Keep every actual Present (including the optional pacer thread) in the same trace domain.
 * This wrapper changes neither the call arguments nor the HRESULT observed by existing code. */
static inline HRESULT boot_present_trace_dxgi(IDXGISwapChain3*s,uint64_t frame){
    boot_present_trace_event(BOOT_PT_PRESENT_ENTER,frame,(uint64_t)(uintptr_t)s,0,0,0);
    HRESULT hr=IDXGISwapChain3_Present(s,1,0);
    boot_present_trace_event(BOOT_PT_PRESENT_EXIT,frame,(uint64_t)(uintptr_t)s,0,0,(uint32_t)hr);
    return hr;
}
typedef struct BootPresentPacer {
    HANDLE thread,request,idle,timer;
    IDXGISwapChain3*swapchain;          /* pending present (own reference) */
    volatile LONG pending,failed;
    int64_t target;                     /* QPC */
    int64_t last_ready,last_target,cadence,frequency;
    uint64_t presents,paced,waits,wait_qpc,delay_qpc;
    int enabled;                        /* -1 unknown, 0 off, 1 on */
    uint64_t trace_frame;               /* diagnostics only, carried to the optional pacer thread */
} BootPresentPacer;
static BootPresentPacer boot_present_pacer={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,0};

static DWORD WINAPI boot_present_pacer_thread(void*unused){
    (void)unused;BootPresentPacer*q=&boot_present_pacer;
    for(;;){
        if(WaitForSingleObject(q->request,INFINITE)!=WAIT_OBJECT_0)return 1;
        LARGE_INTEGER now;QueryPerformanceCounter(&now);
        int64_t wait=q->target-now.QuadPart;
        if(wait>0){
            LARGE_INTEGER due;due.QuadPart=-(LONGLONG)(wait*10000000/q->frequency);   /* 100 ns units, relative */
            if(due.QuadPart<0 && q->timer && SetWaitableTimer(q->timer,&due,0,NULL,NULL,FALSE))WaitForSingleObject(q->timer,INFINITE);
            else Sleep((DWORD)(wait*1000/q->frequency));
            q->delay_qpc+=(uint64_t)wait;
        }
        IDXGISwapChain3*s=q->swapchain;q->swapchain=NULL;
        uint64_t trace_frame=q->trace_frame;q->trace_frame=0;
        if(s){if(FAILED(boot_present_trace_dxgi(s,trace_frame)))InterlockedExchange(&q->failed,1);IDXGISwapChain3_Release(s);}
        ++q->presents;InterlockedExchange(&q->pending,0);SetEvent(q->idle);
    }
}
static inline int boot_present_pace_enabled(void){
    BootPresentPacer*q=&boot_present_pacer;
    if(q->enabled<0){
        const char*v=getenv("TOTK_PRESENT_PACE");q->enabled=v && !strcmp(v,"1");   /* opt-in: waiting for the paced present held the game thread back (0.9 -> 4.1 fps measured) */
        LARGE_INTEGER f;q->frequency=QueryPerformanceFrequency(&f) && f.QuadPart>0?f.QuadPart:0;
        if(!q->frequency)q->enabled=0;
        if(q->enabled){
            q->request=CreateEventW(NULL,FALSE,FALSE,NULL);q->idle=CreateEventW(NULL,TRUE,TRUE,NULL);
            q->timer=CreateWaitableTimerExW(NULL,NULL,0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,TIMER_ALL_ACCESS);
            if(!q->timer)q->timer=CreateWaitableTimerW(NULL,TRUE,NULL);
            if(q->request && q->idle)q->thread=CreateThread(NULL,0,boot_present_pacer_thread,NULL,0,NULL);
            if(!q->thread)q->enabled=0;
            else SetThreadPriority(q->thread,THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }
    return q->enabled;
}
/* Before the guest thread touches the swapchain: the previous paced present has happened.
 * Returns 0 if that Present failed. */
static inline int boot_present_pace_wait(void){
    BootPresentPacer*q=&boot_present_pacer;
    if(q->enabled<=0)return 1;
    if(InterlockedCompareExchange(&q->pending,0,0)){
        LARGE_INTEGER a,b;QueryPerformanceCounter(&a);
        WaitForSingleObject(q->idle,INFINITE);
        QueryPerformanceCounter(&b);++q->waits;q->wait_qpc+=(uint64_t)(b.QuadPart-a.QuadPart);
    }
    return !InterlockedExchange(&q->failed,0);
}
/* Schedule t (ready at now, QPC) given the history: returns the target QPC and updates it. Pure. */
static inline int64_t boot_present_pace_schedule(int64_t now,int64_t frequency,int64_t*last_ready,int64_t*last_target,int64_t*cadence){
    int64_t interval=*last_ready?now-*last_ready:0;*last_ready=now;
    if(interval<=0)return *last_target=now;
    if(!*cadence || interval>4**cadence || interval>2*frequency)*cadence=interval>2*frequency?0:interval;
    else *cadence+=(interval-*cadence)/8;
    if(!*cadence || *cadence<frequency*45/1000)return *last_target=now;   /* fast frames: present at once */
    int64_t step=*cadence*97/100,target=*last_target+step;
    if(target<now)target=now;
    if(target>now+*cadence)target=now+*cadence;
    return *last_target=target;
}
/* Present `s` (sync interval 1): now, or at its paced time on the pacer thread. */
static inline HRESULT boot_present_pace_submit(IDXGISwapChain3*s){
    BootPresentPacer*q=&boot_present_pacer;
    uint64_t trace_frame=boot_present_trace_host_ready((uint64_t)(uintptr_t)s);
    if(!boot_present_pace_enabled())return boot_present_trace_dxgi(s,trace_frame);
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    int64_t target=boot_present_pace_schedule(now.QuadPart,q->frequency,&q->last_ready,&q->last_target,&q->cadence);
    if(target<=now.QuadPart){++q->presents;return boot_present_trace_dxgi(s,trace_frame);}
    IDXGISwapChain3_AddRef(s);q->swapchain=s;q->target=target;q->trace_frame=trace_frame;++q->paced;
    InterlockedExchange(&q->pending,1);ResetEvent(q->idle);SetEvent(q->request);
    return S_OK;
}
#endif
