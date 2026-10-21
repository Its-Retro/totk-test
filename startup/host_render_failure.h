#ifndef TOTK_HOST_RENDER_FAILURE_H
#define TOTK_HOST_RENDER_FAILURE_H
/*98: original failed-result CPU context only. No GPU/guest/clock operation,
 * wait/retry, allocation, changed result or success-path event emission. */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
enum { BOOT_RF98_OPEN_CLOSE=1,BOOT_RF98_BIND_WAIT,BOOT_RF98_ALLOC_CREATE,
       BOOT_RF98_WAIT_FIRST,BOOT_RF98_WAIT_END,BOOT_RF98_BEGIN_WAIT,
       BOOT_RF98_ALLOC_RESET,BOOT_RF98_LIST_RESET,BOOT_RF98_KICK_CLOSE,
       BOOT_RF98_KICK_SIGNAL,BOOT_RF98_RT_CREATE,BOOT_RF98_TARGET_NULL,BOOT_RF98_GPU_WAIT,
       BOOT_RF98_LIMIT };
enum { BOOT_RF98_HR=1,BOOT_RF98_FIRST=2,BOOT_RF98_FINAL=4,
       BOOT_RF98_WAIT=8,BOOT_RF98_LAST_ERROR=16,BOOT_RF98_TARGET=32,BOOT_RF98_REQUEST=64 };
enum { BOOT_RF98_T_FORMAT=1,BOOT_RF98_T_SHAPE,BOOT_RF98_T_PITCH,
       BOOT_RF98_T_GOB,BOOT_RF98_T_CACHED_PITCH,BOOT_RF98_T_OVERLAP,
       BOOT_RF98_T_STALE,BOOT_RF98_T_SLOT,BOOT_RF98_T_CREATE,
       BOOT_RF98_T_NEW_PITCH,BOOT_RF98_T_NEW_REFRESH,BOOT_RF98_T_SPARE_BEGIN };
typedef struct BootRenderFailure98 {
    uint64_t sequence,thread,sub,list,submitted,target,first,final,ring_used,ring_size,address;
    uintptr_t renderer,gpu,device,queue,cl,allocator,fence;
    unsigned stage,detail,known,hr,wait,last_error,line,segment,whole,recording,cbv_used;
    unsigned format,dxgi,width,height,layers,pitch,array_pitch,slot,create_called;
    uintptr_t spare;
} BootRenderFailure98;
typedef struct BootRenderFailure98Slot {
    volatile LONG ready; _Alignas(8) volatile LONG64 count; BootRenderFailure98 row;
} BootRenderFailure98Slot;
static BootRenderFailure98Slot boot_rf98_slots[BOOT_RF98_LIMIT];
static _Alignas(8) volatile LONG64 boot_rf98_sequence;
static volatile LONG boot_rf98_dump_claim;
/* These macros form CPU metadata only, exclusively inside failed branches.
 * Their identifiers are resolved at each renderer call site, after definitions. */
#define BOOT_RF98_GPU(g) .gpu=(uintptr_t)(g),.device=(uintptr_t)(g)->device,.queue=(uintptr_t)(g)->queue,.fence=(uintptr_t)(g)->fence,.submitted=(g)->submitted
#define BOOT_RF98_R(r) BOOT_RF98_GPU((r)->gpu),.renderer=(uintptr_t)(r),.cl=(uintptr_t)(r)->cl,.sub=boot_render_exec_id,.list=boot_render_list_id,.recording=(unsigned)(r)->recording,.ring_used=(r)->ring_used,.ring_size=(r)->ring_size,.cbv_used=(r)->cbv_used
/* First successfully claimed immutable record per stage, not a global earliest
 * GPU event. Counters/claim can race other CPU failures. Never wait on ready1. */
static inline void boot_rf98_note(const BootRenderFailure98 *row) {
    if(!row->stage || row->stage>=BOOT_RF98_LIMIT)return;
    DWORD saved=GetLastError();BootRenderFailure98Slot *slot=&boot_rf98_slots[row->stage];
    InterlockedIncrement64(&slot->count);
    if(!InterlockedCompareExchange(&slot->ready,1,0)) {
        slot->row=*row;slot->row.thread=GetCurrentThreadId();
        slot->row.sequence=(uint64_t)InterlockedIncrement64(&boot_rf98_sequence);
        InterlockedExchange(&slot->ready,2);
    }
    SetLastError(saved);
}
/* Called once on existing native backend false, after replay/kick/reopen and
 * before leaving its existing window lock. No DRED or GPU query is triggered.
 * Only immutable ready2 rows are read; ready1 stays explicitly unavailable. */
static inline void boot_rf98_dump(void) {
    DWORD saved=GetLastError();
    if(InterlockedCompareExchange(&boot_rf98_dump_claim,1,0)){SetLastError(saved);return;}
    FILE *file=fopen("draw-path.log","a");
    if(!file){SetLastError(saved);return;}
    unsigned rows=0,pending=0;
    fprintf(file,"RENDER98 header version=98 pid=%lu scope=first_claimed_failure_per_stage stages=%u no_new_clock=1 gpu_status_known=0 capture_complete_proven=0\n",(unsigned long)GetCurrentProcessId(),BOOT_RF98_LIMIT-1);
    for(unsigned stage=1;stage<BOOT_RF98_LIMIT;++stage) {
        BootRenderFailure98Slot *slot=&boot_rf98_slots[stage];LONG ready=InterlockedCompareExchange(&slot->ready,0,0);
        uint64_t count=(uint64_t)InterlockedCompareExchange64(&slot->count,0,0);
        if(ready!=2){if(ready || count){++pending;fprintf(file,"RENDER98 unavailable stage=%u ready=%ld observed_failures=%llu\n",stage,(long)ready,(unsigned long long)count);}continue;}
        const BootRenderFailure98 *r=&slot->row;++rows;
        fprintf(file,"RENDER98 row stage=%u detail=%u known=%u observed_failures=%llu seq=%llu tid=%llu sub=%llu list=%llu renderer=%llx gpu=%llx device=%llx queue=%llx cl=%llx alloc=%llx fence=%llx segment=%u whole=%u recording=%u submitted=%llu target=%llu first=%llu final=%llu hr=%08x wait=%08x last_error=%u line=%u ring_used=%llu ring_size=%llu cbv_used=%u address=%llx format=%u dxgi=%u width=%u height=%u layers=%u pitch=%u array_pitch=%u slot=%u spare=%llx create_called=%u\n",
            r->stage,r->detail,r->known,(unsigned long long)count,(unsigned long long)r->sequence,(unsigned long long)r->thread,(unsigned long long)r->sub,(unsigned long long)r->list,
            (unsigned long long)r->renderer,(unsigned long long)r->gpu,(unsigned long long)r->device,(unsigned long long)r->queue,(unsigned long long)r->cl,(unsigned long long)r->allocator,(unsigned long long)r->fence,
            r->segment,r->whole,r->recording,(unsigned long long)r->submitted,(unsigned long long)r->target,(unsigned long long)r->first,(unsigned long long)r->final,r->hr,r->wait,r->last_error,r->line,
            (unsigned long long)r->ring_used,(unsigned long long)r->ring_size,r->cbv_used,(unsigned long long)r->address,r->format,r->dxgi,r->width,r->height,r->layers,r->pitch,r->array_pitch,r->slot,(unsigned long long)r->spare,r->create_called);
        if(ferror(file))break;
    }
    if(!ferror(file))fprintf(file,"RENDER98 end rows=%u pending=%u identity=CPU_context_only original_hung_operation=UNKNOWN terminal_io_known=0 capture_complete_proven=0\n",rows,pending);
    fclose(file);SetLastError(saved);
}
#endif
