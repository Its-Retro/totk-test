#ifndef TOTK_HOST_PRESENT_TRACE_H
#define TOTK_HOST_PRESENT_TRACE_H
/* Diagnostics only (70). Initialized by the UI thread before window-ready, default off.
 * Producers do no allocation, file I/O, waits, worker startup or pacing: one successful-lock QPC sample,
 * counters and TryAcquire on a fixed ring. Contention/full loss is explicit. A separate
 * worker drains/serializes at 200 ms intervals. No GPU/display completion is inferred.
 * Abrupt process exit can lose the still-queued tail; retained checkpoints expose the
 * last observation time and loss counters. There is deliberately no loader-lock join. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#ifndef BOOT_PRESENT_TRACE_CAPACITY
#define BOOT_PRESENT_TRACE_CAPACITY 4096
#endif
#define BOOT_PRESENT_TRACE_BATCH 256
enum { BOOT_PT_NVN_READY=1, BOOT_PT_INTERVAL_DONE=2, BOOT_PT_HOST_READY=3,
       BOOT_PT_PRESENT_ENTER=4, BOOT_PT_PRESENT_EXIT=5 };
typedef struct BootPresentTraceEvent {
    uint64_t seq,frame,qpc,swapchain,window;
    uint32_t pid,tid,stage,index,hr,reserved;
} BootPresentTraceEvent;
typedef struct BootPresentTrace {
    SRWLOCK lock;
    volatile LONG initialized,enabled,stop,io_error;
    volatile LONG64 frames,attempted,full,contended,stopping,clock_lost,clock_errors;
    uint64_t accepted,written,frequency,qpc0,filetime0,qpc0_low,qpc0_high,ordered_qpc;
    uint32_t head,count,pid;
    HANDLE thread;
    BootPresentTraceEvent ring[BOOT_PRESENT_TRACE_CAPACITY];
    BootPresentTraceEvent batch[BOOT_PRESENT_TRACE_BATCH];
    char csv[65536],path[260];
} BootPresentTrace;
typedef struct BootPresentTraceSnapshot {
    uint64_t qpc,watermark_qpc,accepted,attempted,full,contended,stopping,clock_lost,clock_errors;
    uint32_t n,queued; LONG stop;
} BootPresentTraceSnapshot;
static BootPresentTrace boot_present_trace;
static _Thread_local uint64_t boot_present_trace_frame;

static inline int boot_present_trace_on(void){
    return boot_present_trace.enabled && !boot_present_trace.io_error && !boot_present_trace.stop;
}
static inline void boot_present_trace_event(unsigned stage,uint64_t frame,uint64_t swapchain,
                                            uint64_t window,uint32_t index,uint32_t hr){
    BootPresentTrace*q=&boot_present_trace;
    if(!boot_present_trace_on())return;
    InterlockedIncrement64(&q->attempted);
    if(!TryAcquireSRWLockExclusive(&q->lock)){InterlockedIncrement64(&q->contended);return;}
    if(q->stop){InterlockedIncrement64(&q->stopping);ReleaseSRWLockExclusive(&q->lock);return;}
    if(q->count==BOOT_PRESENT_TRACE_CAPACITY){InterlockedIncrement64(&q->full);ReleaseSRWLockExclusive(&q->lock);return;}
    /* No timestamp exists before successful publication ownership. A paused producer
     * therefore cannot later publish an event older than a drained watermark. */
    LARGE_INTEGER now;
    if(!QueryPerformanceCounter(&now) || now.QuadPart<0 || (uint64_t)now.QuadPart<q->ordered_qpc){
        InterlockedIncrement64(&q->clock_lost);ReleaseSRWLockExclusive(&q->lock);return;
    }
    q->ordered_qpc=(uint64_t)now.QuadPart;
    BootPresentTraceEvent*e=&q->ring[(q->head+q->count)%BOOT_PRESENT_TRACE_CAPACITY];
    *e=(BootPresentTraceEvent){++q->accepted,frame,(uint64_t)now.QuadPart,swapchain,window,
        q->pid,GetCurrentThreadId(),stage,index,hr,0};
    ++q->count;ReleaseSRWLockExclusive(&q->lock);
}
static inline uint64_t boot_present_trace_begin(uint64_t window,uint32_t index){
    if(!boot_present_trace_on())return 0;
    uint64_t frame=(uint64_t)InterlockedIncrement64(&boot_present_trace.frames);
    boot_present_trace_frame=frame;
    boot_present_trace_event(BOOT_PT_NVN_READY,frame,0,window,index,0);return frame;
}
static inline uint64_t boot_present_trace_host_ready(uint64_t swapchain){
    if(!boot_present_trace_on())return 0;
    uint64_t frame=boot_present_trace_frame;
    if(!frame)frame=(uint64_t)InterlockedIncrement64(&boot_present_trace.frames);
    boot_present_trace_event(BOOT_PT_HOST_READY,frame,swapchain,0,0,0);return frame;
}
/* Worker-only I/O and formatting below. */
static inline int boot_present_trace_write(HANDLE file,const char*data,DWORD bytes){
    while(bytes){DWORD done=0;if(!WriteFile(file,data,bytes,&done,NULL) || !done)return 0;data+=done;bytes-=done;}
    return 1;
}
/* Worker takes a bounded batch and samples its empty-ring boundary under the
 * same lock as accepted timestamps. No watermark exists for a partial drain. */
static inline BootPresentTraceSnapshot boot_present_trace_take(void){
    BootPresentTrace*q=&boot_present_trace;BootPresentTraceSnapshot s={0};
    AcquireSRWLockExclusive(&q->lock);
    s.n=q->count<BOOT_PRESENT_TRACE_BATCH?q->count:BOOT_PRESENT_TRACE_BATCH;
    for(unsigned i=0;i<s.n;++i)q->batch[i]=q->ring[(q->head+i)%BOOT_PRESENT_TRACE_CAPACITY];
    q->head=(q->head+s.n)%BOOT_PRESENT_TRACE_CAPACITY;q->count-=s.n;s.queued=q->count;s.accepted=q->accepted;
    LARGE_INTEGER now;
    if(QueryPerformanceCounter(&now) && now.QuadPart>=0 && (uint64_t)now.QuadPart>=q->ordered_qpc){
        s.qpc=(uint64_t)now.QuadPart;q->ordered_qpc=s.qpc;if(!s.queued)s.watermark_qpc=s.qpc;
    }else InterlockedIncrement64(&q->clock_errors);
    s.attempted=(uint64_t)InterlockedCompareExchange64(&q->attempted,0,0);
    s.full=(uint64_t)InterlockedCompareExchange64(&q->full,0,0);
    s.contended=(uint64_t)InterlockedCompareExchange64(&q->contended,0,0);
    s.stopping=(uint64_t)InterlockedCompareExchange64(&q->stopping,0,0);
    s.clock_lost=(uint64_t)InterlockedCompareExchange64(&q->clock_lost,0,0);
    s.clock_errors=(uint64_t)InterlockedCompareExchange64(&q->clock_errors,0,0);s.stop=q->stop;
    ReleaseSRWLockExclusive(&q->lock);return s;
}
/* A checkpoint is emitted only AFTER every row it certifies was successfully
 * written. WriteFile completion is OS file visibility, not power-loss durability. */
static inline int boot_present_trace_emit(HANDLE file,const BootPresentTraceSnapshot*s){
    BootPresentTrace*q=&boot_present_trace;int used=0;
    for(unsigned i=0;i<s->n;++i){BootPresentTraceEvent*e=&q->batch[i];
        int k=snprintf(q->csv+used,sizeof q->csv-(size_t)used,"%llu,%llu,%llu,%u,%lu,%lu,0x%llx,0x%llx,%u,0x%08x\n",
            (unsigned long long)e->seq,(unsigned long long)e->frame,(unsigned long long)e->qpc,e->stage,
            (unsigned long)e->pid,(unsigned long)e->tid,(unsigned long long)e->swapchain,
            (unsigned long long)e->window,e->index,e->hr);
        if(k<0 || (size_t)k>=sizeof q->csv-(size_t)used){SetLastError(ERROR_INSUFFICIENT_BUFFER);return 0;}
        used+=k;
    }
    if(used && !boot_present_trace_write(file,q->csv,(DWORD)used))return 0;
    q->written+=s->n;
    used=snprintf(q->csv,sizeof q->csv,
        "#stats,qpc=%llu,watermark_qpc=%llu,watermark_seq=%llu,attempted=%llu,accepted=%llu,written=%llu,queued=%u,full=%llu,contended=%llu,stopping=%llu,clock_lost=%llu,clock_errors=%llu,io_error=0,stop=%ld\n",
        (unsigned long long)s->qpc,(unsigned long long)s->watermark_qpc,
        (unsigned long long)(s->watermark_qpc?s->accepted:0),(unsigned long long)s->attempted,
        (unsigned long long)s->accepted,(unsigned long long)q->written,s->queued,
        (unsigned long long)s->full,(unsigned long long)s->contended,
        (unsigned long long)s->stopping,(unsigned long long)s->clock_lost,(unsigned long long)s->clock_errors,(long)s->stop);
    if(used<0 || (size_t)used>=sizeof q->csv){SetLastError(ERROR_INSUFFICIENT_BUFFER);return 0;}
    return boot_present_trace_write(file,q->csv,(DWORD)used);
}
static DWORD WINAPI boot_present_trace_worker(void*unused){
    (void)unused;BootPresentTrace*q=&boot_present_trace;
    CreateDirectoryA("frames",NULL);
    HANDLE file=CreateFileA(q->path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE){
        InterlockedExchange(&q->io_error,(LONG)GetLastError());
        fprintf(stderr,"present trace70: cannot create diagnostic file (error %lu)\n",(unsigned long)q->io_error);return 1;
    }
    int used=snprintf(q->csv,sizeof q->csv,"#trace70,version=2,pid=%lu,frequency=%llu,qpc0=%llu,filetime0=%llu,qpc0_low=%llu,qpc0_high=%llu,capacity=%u,drain_ms=200\n"
        "seq,frame,qpc,stage,pid,tid,swapchain,window,index,hresult\n",
        (unsigned long)q->pid,(unsigned long long)q->frequency,(unsigned long long)q->qpc0,
        (unsigned long long)q->filetime0,(unsigned long long)q->qpc0_low,(unsigned long long)q->qpc0_high,BOOT_PRESENT_TRACE_CAPACITY);
    if(used<0 || !boot_present_trace_write(file,q->csv,(DWORD)used))goto failed;
    fprintf(stderr,"present trace70: %s (engine/API timestamps, no display timing)\n",q->path);
    for(;;){
        BootPresentTraceSnapshot s=boot_present_trace_take();
        if(!boot_present_trace_emit(file,&s))goto failed;
        if(s.stop && !s.queued)break;
        if(s.queued)continue;
        Sleep(200);   /* worker only, never the renderer/guest/pacer */
    }
    CloseHandle(file);return 0;
failed:
    {DWORD error=GetLastError();if(!error)error=ERROR_WRITE_FAULT;InterlockedExchange(&q->io_error,(LONG)error);
     fprintf(stderr,"present trace70: diagnostic write failed (error %lu); trace incomplete\n",(unsigned long)error);}
    CloseHandle(file);return 1;
}
/* UI thread only, before publishing window-ready. No lazy init in any producer. */
static inline void boot_present_trace_start(void){
    BootPresentTrace*q=&boot_present_trace;
    if(InterlockedCompareExchange(&q->initialized,1,0))return;
    char value[8];DWORD n=GetEnvironmentVariableA("TOTK_PRESENT_TRACE",value,sizeof value);
    if(n!=1 || value[0]!='1')return;
    LARGE_INTEGER frequency,a,b;FILETIME wall;
    if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0)return;
    q->frequency=(uint64_t)frequency.QuadPart;q->pid=GetCurrentProcessId();
    QueryPerformanceCounter(&a);GetSystemTimePreciseAsFileTime(&wall);QueryPerformanceCounter(&b);
    q->qpc0_low=(uint64_t)a.QuadPart;q->qpc0_high=(uint64_t)b.QuadPart;
    q->qpc0=q->qpc0_low+(q->qpc0_high-q->qpc0_low)/2;q->ordered_qpc=q->qpc0_high;
    q->filetime0=((uint64_t)wall.dwHighDateTime<<32)|wall.dwLowDateTime;
    snprintf(q->path,sizeof q->path,"frames/present-qpc-%lu-%llu.csv",(unsigned long)q->pid,(unsigned long long)q->filetime0);
    q->thread=CreateThread(NULL,0,boot_present_trace_worker,NULL,0,NULL);
    if(!q->thread){InterlockedExchange(&q->io_error,(LONG)GetLastError());fprintf(stderr,"present trace70: cannot start diagnostic worker\n");return;}
    SetThreadPriority(q->thread,THREAD_PRIORITY_BELOW_NORMAL);InterlockedExchange(&q->enabled,1);
}
#endif
