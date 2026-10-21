#ifndef TOTK_HOST_TILE_BINDING_TRACE_H
#define TOTK_HOST_TILE_BINDING_TRACE_H
/*89: selected tile-family CPU issue metadata. No new GPU command/query or guest
 * read. Direct UBO payload is UNKNOWN. Writer/arming run outside the renderer. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "host_gpu_cbv_tap.h"
#include "host_tile_binding_trace_core.h"
#include "host_tile_failure_tail.h"
enum { BOOT_TILE89_WAIT_ARM=0,BOOT_TILE89_ACTIVE=1,BOOT_TILE89_CLOSED=2 };
typedef struct BootTile89Pending {
    uint64_t token; uintptr_t draw; unsigned alive,ready,bindings,tables;
    BootTile89Record row;
} BootTile89Pending;
typedef struct BootTile89Tls {
    BootTile89Pending pending[BOOT_CBV_TAP_DEPTH]; BootTile89Pending *issue;
    uintptr_t native_executor,native_draw,barrier_executor;
    uint64_t native_values[12],barrier_flags,barrier_address,barrier_sequence,consumed_sequence;
    BootDredExec execute; unsigned execute_known,need_signal,barrier_overflow;
    uintptr_t signal_cl;
} BootTile89Tls;
typedef struct BootTile89Trace {
    int enabled; volatile LONG state; SRWLOCK lock; BootTile89Config config;
    DWORD pid; uint64_t creation,start,frequency,arm,end,last_qpc,stop_qpc;
    unsigned stop_reason,armed_consumed,arm_bytes;
    uint64_t accepted,draws,signals,written; unsigned head,count;
    BootTile89Record ring[BOOT_TILE89_RING],batch[32];
    char io_buffer[65536]; unsigned io_used; HANDLE file;
    char path[MAX_PATH],receipt[MAX_PATH],temporary[MAX_PATH],request[MAX_PATH];
    _Alignas(8) volatile LONG64 lock_loss,full_loss,cap_loss,clock_loss,late_loss,unissued,tap_fault,ticket_missing,io_error;
} BootTile89Trace;
static BootTile89Trace boot_tile89={.lock=SRWLOCK_INIT,.file=INVALID_HANDLE_VALUE};
static _Thread_local BootTile89Tls boot_tile89_tls;
/*97: separate producer tail and single-use immutable failure snapshot. Neither
 * aliases the worker ring/batch/text/file. No reset/reuse during this process. */
static BootTile97Tail boot_tile97_tail;
static BootTile97Snapshot boot_tile97_failure;
static volatile LONG boot_tile97_claim;
static inline int boot_tile89_on(void) { return boot_tile89.enabled; }
static inline int boot_tile89_now(uint64_t *value) {
    LARGE_INTEGER q;if(!QueryPerformanceCounter(&q) || q.QuadPart<=0){InterlockedIncrement64(&boot_tile89.clock_loss);return 0;}
    *value=(uint64_t)q.QuadPart;return 1;
}
static inline BootTile89Pending *boot_tile89_pending(uint64_t token,uintptr_t draw) {
    unsigned depth=(unsigned)token;if(!token || !depth || depth>BOOT_CBV_TAP_DEPTH)return NULL;
    BootTile89Pending *p=&boot_tile89_tls.pending[depth-1];return p->alive && p->token==token && p->draw==draw?p:NULL;
}
static inline int boot_tile89_emit(const BootTile89Record *record) {
    if(!boot_tile89.enabled)return 0;
    if(!TryAcquireSRWLockExclusive(&boot_tile89.lock)) { InterlockedIncrement64(&boot_tile89.lock_loss);return 0; }
    if(boot_tile89.state!=BOOT_TILE89_ACTIVE) { ReleaseSRWLockExclusive(&boot_tile89.lock);InterlockedIncrement64(&boot_tile89.late_loss);return 0; }
    uint64_t qpc;if(!boot_tile89_now(&qpc)){ReleaseSRWLockExclusive(&boot_tile89.lock);return 0;}
    if(qpc<boot_tile89.last_qpc) { ReleaseSRWLockExclusive(&boot_tile89.lock);InterlockedIncrement64(&boot_tile89.clock_loss);return 0; }
    if(qpc>=boot_tile89.end || boot_tile89.accepted>=BOOT_TILE89_MAX_ROWS ||
       (record->kind==BOOT_TILE89_DRAW && boot_tile89.draws>=boot_tile89.config.max_draws)) {
        boot_tile89.state=BOOT_TILE89_CLOSED;boot_tile89.stop_reason=qpc>=boot_tile89.end?BOOT_TILE89_STOP_TIME:BOOT_TILE89_STOP_CAP;boot_tile89.stop_qpc=qpc;
        ReleaseSRWLockExclusive(&boot_tile89.lock);
        if(qpc>=boot_tile89.end)InterlockedIncrement64(&boot_tile89.late_loss);else InterlockedIncrement64(&boot_tile89.cap_loss);return 0;
    }
    if(boot_tile89.count==BOOT_TILE89_RING) { ReleaseSRWLockExclusive(&boot_tile89.lock);InterlockedIncrement64(&boot_tile89.full_loss);return 0; }
    unsigned slot=(boot_tile89.head+boot_tile89.count)%BOOT_TILE89_RING;
    boot_tile89.ring[slot]=*record;boot_tile89.ring[slot].qpc=qpc;boot_tile89.ring[slot].serial=++boot_tile89.accepted;
    ++boot_tile89.count;boot_tile89.last_qpc=qpc;
    if(record->kind==BOOT_TILE89_DRAW)++boot_tile89.draws;else ++boot_tile89.signals;
    boot_tile97_commit(&boot_tile97_tail,&boot_tile89.ring[slot]);
    ReleaseSRWLockExclusive(&boot_tile89.lock);return 1;
}
static inline unsigned boot_tile89_begin(const BootCbvTapBegin *b) {
    if(!boot_tile89.enabled)return 0;
    /* A subsequent or reentrant Draw can replace renderer binding state. Do
     * not reuse an outer/unissued record, including for an unselected Begin. */
    for(unsigned i=0;i<BOOT_CBV_TAP_DEPTH;++i)if(boot_tile89_tls.pending[i].alive){boot_tile89_tls.pending[i].alive=0;InterlockedIncrement64(&boot_tile89.unissued);}
    boot_tile89_tls.issue=NULL;
    if(InterlockedCompareExchange(&boot_tile89.state,0,0)!=BOOT_TILE89_ACTIVE || !boot_tile89_pair(b->vs,b->ps))return 0;
    unsigned depth=(unsigned)b->token;if(!depth || depth>BOOT_CBV_TAP_DEPTH)return 0;
    BootTile89Pending *p=&boot_tile89_tls.pending[depth-1];if(p->alive)InterlockedIncrement64(&boot_tile89.unissued);
    memset(p,0,sizeof *p);p->alive=1;p->token=b->token;p->draw=b->draw;p->row.kind=BOOT_TILE89_DRAW;p->row.thread=GetCurrentThreadId();
    memcpy(p->row.vs,b->vs,32);memcpy(p->row.ps,b->ps,32);uint64_t *v=p->row.v;
    v[0]=b->token;v[1]=b->sub;v[2]=b->event;v[3]=b->renderer;v[10]=b->pso;v[11]=b->vs_mask;
    v[16]=b->count;v[17]=b->instances;v[18]=b->indexed;v[19]=b->topology;v[20]=b->rt_count;v[21]=b->samples;
    for(unsigned j=0;j<b->rt_count && j<8;++j) {
        unsigned k=BOOT_TILE89_RT_BASE+j*BOOT_TILE89_RT_STRIDE;const BootCbvTapTarget *t=&b->target[j];
        v[k]=b->rt_resource[j];v[k+1]=b->rt_address[j];v[k+2]=((uint64_t)t->width<<32)|t->height;
        v[k+3]=((uint64_t)t->format<<32)|t->state;v[k+4]=((uint64_t)t->layer<<32)|t->layers;v[k+5]=((uint64_t)t->pitch<<32)|t->map;
    }
    return 1;
}
static inline void boot_tile89_binding(const BootCbvTapBinding *b) {
    BootTile89Pending *p=boot_tile89_pending(b->token,b->draw);if(!p)return;
    unsigned index=boot_tile89_bind_index(b->stage,b->slot);if(index==UINT32_MAX)return;
    unsigned bit=1u<<index,k=BOOT_TILE89_BIND_BASE+index*BOOT_TILE89_BIND_STRIDE;uint64_t *v=p->row.v;
    if(p->bindings&bit || b->renderer!=v[3] || b->group!=(b->stage?4u:0u))v[56]|=BOOT_TILE89_BAD_BINDING;
    p->bindings|=bit;
    v[k]=b->requested_va;v[k+1]=b->requested_size;v[k+2]=b->valid;v[k+3]=b->location;v[k+4]=b->size;v[k+5]=b->resource;v[k+6]=b->offset;
    v[k+7]=b->resource_bytes;v[k+8]=b->mode;v[k+9]=b->region;v[k+10]=b->scope;v[k+11]=b->list;v[k+12]=b->map_generation;v[k+13]=b->flags;
    /* All copies are tiny allowlists from an already-proven upload-ring span. */
    if(b->slot==0) {
        size_t at=b->stage?0x550u:0x150u;unsigned raw=b->stage?4u:0u,known=b->stage?BOOT_TILE89_PS_C0:BOOT_TILE89_VS_C0;
        if(boot_cbv_tap_copy_owned(b,at,16,&p->row.raw[raw]))v[62]|=known;
    } else if(b->slot==4) {
        unsigned raw=b->stage?16u:8u,known=b->stage?BOOT_TILE89_PS_C4:BOOT_TILE89_VS_C4;
        if(boot_cbv_tap_copy_owned(b,0x920u,32,&p->row.raw[raw]))v[62]|=known;
    } else if(b->slot==18) {
        uint32_t observed[8];if(boot_cbv_tap_copy_owned(b,0,32,observed)) {
            if(!b->stage) { memcpy(&p->row.raw[24],observed,32);v[62]|=BOOT_TILE89_HOST_VS; }
            else if(v[62]&BOOT_TILE89_HOST_VS) { if(!memcmp(&p->row.raw[24],observed,32))v[62]|=BOOT_TILE89_HOST_PS;else v[56]|=BOOT_TILE89_BAD_BINDING; }
        }
    }
}
static inline void boot_tile89_table(const BootCbvTapTable *t,const BootCbvTapCv actual[19]) {
    BootTile89Pending *p=boot_tile89_pending(t->token,t->draw);if(!p || t->stage>1)return;
    unsigned stage=t->stage,bit=1u<<stage;uint64_t *v=p->row.v;unsigned ok=1;
    if(p->tables&bit || t->renderer!=v[3] || t->group!=(stage?4u:0u))ok=0;
    for(unsigned j=0;j<3;++j) {
        unsigned slot=j==0?0:j==1?4:18,k=BOOT_TILE89_BIND_BASE+(stage*3+j)*BOOT_TILE89_BIND_STRIDE;
        if(!(p->bindings&(1u<<(stage*3+j))) || actual[slot].location!=v[k+3] || actual[slot].size!=v[k+4])ok=0;
    }
    p->tables|=bit;v[63]=p->tables;v[196+stage]=t->base;v[198+stage]=t->reused;v[200+stage]=ok;
    v[202+stage]=t->scope;v[204+stage]=t->list;v[206+stage]=t->map_generation;
    if(!ok)v[56]|=BOOT_TILE89_BAD_TABLE;
}
static inline void boot_tile89_end(uint64_t token,uintptr_t draw,unsigned aborted) {
    BootTile89Pending *p=boot_tile89_pending(token,draw);if(!p)return;
    if(aborted) { p->alive=0;p->row.v[56]|=BOOT_TILE89_BAD_ABORT;InterlockedIncrement64(&boot_tile89.unissued);if(boot_tile89_tls.issue==p)boot_tile89_tls.issue=NULL;return; }
    p->ready=1;if(p->bindings!=63 || p->tables!=3)p->row.v[56]|=BOOT_TILE89_BAD_BINDING;
}
static inline void boot_tile89_fault(unsigned reason) {
    (void)reason;if(!boot_tile89.enabled)return;InterlockedIncrement64(&boot_tile89.tap_fault);
    for(unsigned i=0;i<BOOT_CBV_TAP_DEPTH;++i) if(boot_tile89_tls.pending[i].alive) { boot_tile89_tls.pending[i].alive=0;InterlockedIncrement64(&boot_tile89.unissued); }
    boot_tile89_tls.issue=NULL;
}
static inline BootTile89Record *boot_tile89_issue_record(uint64_t token,uintptr_t draw) {
    if(!boot_tile89.enabled)return NULL;BootTile89Pending *p=boot_tile89_pending(token,draw);
    if(!p || !p->ready)return NULL;boot_tile89_tls.issue=p;return &p->row;
}
/* Existing82 counters are monotone and can be incremented on a failed lock.
 * A successful later hook alone cannot repair the lost generation history. */
static inline unsigned boot_tile89_ledger_taint(void) {
    return (InterlockedCompareExchange64(&boot_dred_lost_lock,0,0)?1u:0u)|
        (InterlockedCompareExchange64(&boot_dred_missing_state,0,0)?2u:0u)|
        (InterlockedCompareExchange64(&boot_dred_state_evictions,0,0)?4u:0u);
}
/*97: failure path only. Exactly one nonblocking89 lock attempt; no82 lock,
 * QPC, worker state/IO access, guest read, GPU query or producer allocation.
 * Foreign atomic counters are observed separately, not an intent watermark. */
static inline void boot_tile89_failure_tail(FILE *file) {
    if(!boot_tile89.enabled || !file)return;
    if(InterlockedCompareExchange(&boot_tile97_claim,1,0)) { (void)boot_tile97_unavailable(file,"already_claimed");return; }
    if(!TryAcquireSRWLockExclusive(&boot_tile89.lock)) { (void)boot_tile97_unavailable(file,"lock_busy");return; }
    BootTile97Snapshot *s=&boot_tile97_failure;
    s->pid=boot_tile89.pid;s->creation=boot_tile89.creation;s->start=boot_tile89.start;s->frequency=boot_tile89.frequency;
    s->arm=boot_tile89.arm;s->end=boot_tile89.end;s->last_qpc=boot_tile89.last_qpc;s->stop_qpc=boot_tile89.stop_qpc;
    s->duration_ms=boot_tile89.config.duration_ms;s->max_draws=boot_tile89.config.max_draws;
    s->state=(unsigned)boot_tile89.state;s->reason=boot_tile89.stop_reason;s->arm_consumed=boot_tile89.armed_consumed;s->arm_bytes=boot_tile89.arm_bytes;
    s->accepted=boot_tile89.accepted;s->draws=boot_tile89.draws;s->signals=boot_tile89.signals;
    s->loss[0]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.lock_loss,0,0);
    s->loss[1]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.full_loss,0,0);
    s->loss[2]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.cap_loss,0,0);
    s->loss[3]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.clock_loss,0,0);
    s->loss[4]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.late_loss,0,0);
    s->loss[5]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.unissued,0,0);
    s->loss[6]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.tap_fault,0,0);
    s->loss[7]=(uint64_t)InterlockedCompareExchange64(&boot_tile89.ticket_missing,0,0);
    s->ledger_taint=boot_tile89_ledger_taint();int valid=boot_tile97_copy(s,&boot_tile97_tail);
    ReleaseSRWLockExclusive(&boot_tile89.lock);
    if(!valid) { (void)boot_tile97_unavailable(file,"tail_inconsistent");return; }
    (void)boot_tile97_format(file,s);
}
static inline void boot_tile89_dred_work(const BootDredWork *work) {
    if(!boot_tile89.enabled)return;
    BootTile89Pending *p=boot_tile89_tls.issue;if(!p || !p->alive || !p->ready)return;uint64_t *v=p->row.v;
    if((uintptr_t)work->cl!=v[6] || work->sub!=v[1] || work->event!=v[2])return;
    if(work->op!=(v[18]?BOOT_DRED_INDEXED:BOOT_DRED_DRAW) || memcmp(work->vs,p->row.vs,8) || memcmp(work->ps,p->row.ps,8)) {
        v[61]=0;v[56]|=BOOT_TILE89_BAD_NEST;boot_tile89_tls.issue=NULL;return;
    }
    v[57]=work->life;v[58]=work->reset;v[59]=work->serial;v[60]=work->ordinal;
    const BootDredReset *recording=work->reset?&boot_dred_resets[(work->reset-1)%BOOT_DRED_RESET_CAP]:NULL;
    if(!boot_tile89_ledger_taint() && recording && recording->reset==work->reset && recording->cl==work->cl &&
       recording->life==work->life && !recording->flags)v[61]=1;
    else v[56]|=BOOT_TILE89_BAD_LEDGER;
}
static inline void boot_tile89_dred_execute(const BootDredExec *execution) { if(boot_tile89.enabled){boot_tile89_tls.execute=*execution;boot_tile89_tls.execute_known=!execution->recording.flags && !boot_tile89_ledger_taint();} }
static inline void boot_tile89_issued(uint64_t token,uintptr_t draw,unsigned issued) {
    BootTile89Pending *p=boot_tile89_pending(token,draw);if(!p)return;
    if(!issued || !p->ready)InterlockedIncrement64(&boot_tile89.unissued);
    else {
        if(!p->row.v[211])p->row.v[56]|=BOOT_TILE89_BAD_ISSUE;
        if(boot_tile89_ledger_taint()){p->row.v[61]=0;p->row.v[56]|=BOOT_TILE89_BAD_LEDGER;}
        if(!p->row.v[61])InterlockedIncrement64(&boot_tile89.ticket_missing);
        if(boot_tile89_emit(&p->row)) { boot_tile89_tls.need_signal=1;boot_tile89_tls.signal_cl=(uintptr_t)p->row.v[6]; }
    }
    p->alive=0;if(boot_tile89_tls.issue==p)boot_tile89_tls.issue=NULL;
}
static inline void boot_tile89_signal(const void *cl,const void *queue,const void *fence,uint64_t value,HRESULT result) {
    if(!boot_tile89.enabled || !boot_tile89_tls.need_signal || boot_tile89_tls.signal_cl!=(uintptr_t)cl)return;
    BootTile89Record row={0};row.kind=BOOT_TILE89_SIGNAL;row.thread=GetCurrentThreadId();row.v[5]=(uintptr_t)queue;row.v[6]=(uintptr_t)cl;row.v[9]=(uintptr_t)fence;
    row.v[63]=(uint32_t)result;row.v[64]=value;
    const BootDredExec *e=&boot_tile89_tls.execute;
    row.v[65]=boot_tile89_ledger_taint();
    if(boot_tile89_tls.execute_known && !row.v[65] && e->recording.cl==cl && e->recording.queue==queue && !e->recording.flags) {
        row.v[4]=(uintptr_t)e->recording.device;row.v[57]=e->recording.life;row.v[58]=e->recording.reset;row.v[59]=e->execute;
        row.v[60]=e->recording.first_work;row.v[61]=e->recording.last_work;row.v[62]=1;
    }
    if(!row.v[62])InterlockedIncrement64(&boot_tile89.ticket_missing);
    boot_tile89_emit(&row);boot_tile89_tls.need_signal=0;boot_tile89_tls.execute_known=0;
}
static inline void boot_tile89_native_barrier(const void *executor,uint64_t flags,uint64_t address) {
    if(!boot_tile89.enabled)return;boot_tile89_tls.barrier_executor=(uintptr_t)executor;boot_tile89_tls.barrier_flags=flags;boot_tile89_tls.barrier_address=address;
    if(++boot_tile89_tls.barrier_sequence==0)boot_tile89_tls.barrier_overflow=1;
}
static inline void boot_tile89_native_enter(const void *executor,const void *draw,uint64_t first,uint64_t base_vertex,uint64_t base_instance,uint64_t instances,uint64_t ordinal,unsigned expanded,unsigned guest_barrier,uint64_t vs_base,uint64_t vs_bytes,uint64_t ps_base,uint64_t ps_bytes) {
    if(!boot_tile89.enabled)return;boot_tile89_tls.native_executor=(uintptr_t)executor;boot_tile89_tls.native_draw=(uintptr_t)draw;
    uint64_t *v=boot_tile89_tls.native_values;v[0]=first;v[1]=base_vertex;v[2]=base_instance;v[3]=instances;v[4]=ordinal;v[5]=expanded;
    v[6]=guest_barrier && !boot_tile89_tls.barrier_overflow && boot_tile89_tls.barrier_executor==(uintptr_t)executor && boot_tile89_tls.barrier_sequence>boot_tile89_tls.consumed_sequence;
    v[7]=boot_tile89_tls.barrier_sequence;boot_tile89_tls.consumed_sequence=boot_tile89_tls.barrier_sequence;
    v[8]=vs_base;v[9]=vs_bytes;v[10]=ps_base;v[11]=ps_bytes;
}
static inline void boot_tile89_native_leave(void) { if(boot_tile89.enabled)boot_tile89_tls.native_draw=0; }
static inline void boot_tile89_execute_attempt(const void *cl,const void *queue) { (void)cl;(void)queue;if(boot_tile89.enabled)boot_tile89_tls.execute_known=0; }
static inline void boot_tile89_error(DWORD error) {
    if(!error)error=ERROR_WRITE_FAULT;InterlockedCompareExchange64(&boot_tile89.io_error,error,0);
}
static inline int boot_tile89_write(HANDLE file,const void *data,DWORD bytes) {
    DWORD written=0;if(!WriteFile(file,data,bytes,&written,NULL) || written!=bytes) { boot_tile89_error(GetLastError());return 0; }return 1;
}
static inline int boot_tile89_flush_buffer(void) {
    unsigned bytes=boot_tile89.io_used;boot_tile89.io_used=0;
    return !bytes || boot_tile89_write(boot_tile89.file,boot_tile89.io_buffer,bytes);
}
static inline int boot_tile89_buffer(const char *text,size_t bytes) {
    if(bytes>sizeof boot_tile89.io_buffer)return 0;
    if(bytes>sizeof boot_tile89.io_buffer-boot_tile89.io_used && !boot_tile89_flush_buffer())return 0;
    memcpy(boot_tile89.io_buffer+boot_tile89.io_used,text,bytes);boot_tile89.io_used+=(unsigned)bytes;return 1;
}
static inline int boot_tile89_row(const BootTile89Record *r) {
    char line[12288],vs[65],ps[65];for(unsigned i=0;i<32;++i){snprintf(vs+i*2,3,"%02x",r->vs[i]);snprintf(ps+i*2,3,"%02x",r->ps[i]);}
    size_t used=0;
#define BOOT_TILE89_APP(...) do { int n=snprintf(line+used,sizeof line-used,__VA_ARGS__);if(n<0 || (size_t)n>=sizeof line-used){boot_tile89_error(ERROR_INSUFFICIENT_BUFFER);return 0;}used+=(size_t)n; } while(0)
    BOOT_TILE89_APP("{\"kind\":\"%s\",\"qpc\":%llu,\"serial\":%llu,\"thread\":%llu,\"vs\":\"%s\",\"ps\":\"%s\",\"v\":[",r->kind==BOOT_TILE89_DRAW?"draw":"signal",(unsigned long long)r->qpc,(unsigned long long)r->serial,(unsigned long long)r->thread,vs,ps);
    for(unsigned i=0;i<BOOT_TILE89_VALUES;++i)BOOT_TILE89_APP("%s%llu",i?",":"",(unsigned long long)r->v[i]);
    BOOT_TILE89_APP("],\"raw\":[");for(unsigned i=0;i<BOOT_TILE89_RAW;++i)BOOT_TILE89_APP("%s%u",i?",":"",r->raw[i]);BOOT_TILE89_APP("]}\n");
#undef BOOT_TILE89_APP
    return boot_tile89_buffer(line,used);
}
static inline void boot_tile89_close_admission(unsigned reason) {
    AcquireSRWLockExclusive(&boot_tile89.lock);
    if(boot_tile89.state!=BOOT_TILE89_CLOSED) { uint64_t q=boot_tile89.last_qpc;if(!boot_tile89_now(&q))reason=BOOT_TILE89_STOP_CLOCK;else if(q<boot_tile89.last_qpc){InterlockedIncrement64(&boot_tile89.clock_loss);q=boot_tile89.last_qpc;reason=BOOT_TILE89_STOP_CLOCK;}boot_tile89.stop_qpc=q;boot_tile89.stop_reason=reason;boot_tile89.state=BOOT_TILE89_CLOSED; }
    ReleaseSRWLockExclusive(&boot_tile89.lock);
}
static inline unsigned boot_tile89_take(void) {
    AcquireSRWLockExclusive(&boot_tile89.lock);unsigned n=boot_tile89.count<32?boot_tile89.count:32;
    for(unsigned i=0;i<n;++i)boot_tile89.batch[i]=boot_tile89.ring[(boot_tile89.head+i)%BOOT_TILE89_RING];
    boot_tile89.head=(boot_tile89.head+n)%BOOT_TILE89_RING;boot_tile89.count-=n;ReleaseSRWLockExclusive(&boot_tile89.lock);return n;
}
static inline int boot_tile89_finish_receipt(uint64_t lock,uint64_t full,uint64_t cap,uint64_t clock,uint64_t late,uint64_t unissued,uint64_t faults,uint64_t missing) {
    char data[3072];DWORD io=(DWORD)InterlockedCompareExchange64(&boot_tile89.io_error,0,0);
    int n=snprintf(data,sizeof data,"{\"version\":89,\"pid\":%lu,\"creation\":%llu,\"start_qpc\":%llu,\"arm_qpc\":%llu,\"stop_qpc\":%llu,\"duration_ms\":%u,\"max_draws\":%u,\"rows\":%llu,\"draws\":%llu,\"signals\":%llu,\"written\":%llu,\"lock\":%llu,\"full\":%llu,\"cap\":%llu,\"clock\":%llu,\"late\":%llu,\"unissued\":%llu,\"tap_fault\":%llu,\"ticket_missing\":%llu,\"reason\":%u,\"arm_consumed\":%s,\"arm_bytes\":%u,\"io_error\":%lu,\"accepted_prefix_persisted\":%s,\"capture_complete_proven\":false,\"global_last_writer_proven\":false,\"hung_draw_identity_proven\":false,\"gpu_completion_observed\":false}\n",
        (unsigned long)boot_tile89.pid,(unsigned long long)boot_tile89.creation,(unsigned long long)boot_tile89.start,(unsigned long long)boot_tile89.arm,(unsigned long long)boot_tile89.stop_qpc,boot_tile89.config.duration_ms,boot_tile89.config.max_draws,
        (unsigned long long)boot_tile89.accepted,(unsigned long long)boot_tile89.draws,(unsigned long long)boot_tile89.signals,(unsigned long long)boot_tile89.written,(unsigned long long)lock,(unsigned long long)full,(unsigned long long)cap,(unsigned long long)clock,(unsigned long long)late,(unsigned long long)unissued,(unsigned long long)faults,(unsigned long long)missing,boot_tile89.stop_reason,boot_tile89.armed_consumed?"true":"false",boot_tile89.arm_bytes,(unsigned long)io,!io && boot_tile89.written==boot_tile89.accepted?"true":"false");
    if(n<0 || (size_t)n>=sizeof data){boot_tile89_error(ERROR_INSUFFICIENT_BUFFER);return 0;}
    HANDLE f=CreateFileA(boot_tile89.temporary,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE){boot_tile89_error(GetLastError());return 0;}
    int ok=boot_tile89_write(f,data,(DWORD)n);if(!FlushFileBuffers(f)){boot_tile89_error(GetLastError());ok=0;}
    if(!CloseHandle(f)){boot_tile89_error(GetLastError());ok=0;}
    if(!ok)return 0;
    if(!MoveFileExA(boot_tile89.temporary,boot_tile89.receipt,MOVEFILE_WRITE_THROUGH)){boot_tile89_error(GetLastError());return 0;}
    return 1;
}
static inline int boot_tile89_try_arm(void) {
    /* READ sharing only: a still-open request writer makes this a retry, so a
     * partial request is not consumed. Never delete/modify the request file. */
    HANDLE f=CreateFileA(boot_tile89.request,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) { DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND || error==ERROR_SHARING_VIOLATION)return 0;boot_tile89_error(error);return -1; }
    LARGE_INTEGER size;char data[513];DWORD got=0;int ok=GetFileSizeEx(f,&size) && size.QuadPart>0 && size.QuadPart<=512;
    if(ok)ok=ReadFile(f,data,(DWORD)size.QuadPart,&got,NULL) && got==(DWORD)size.QuadPart;
    if(!CloseHandle(f)){boot_tile89_error(GetLastError());ok=0;}
    if(!ok || !boot_tile89_arm(data,got,boot_tile89.pid,boot_tile89.creation,boot_tile89.start))return -1;
    AcquireSRWLockExclusive(&boot_tile89.lock);uint64_t q;
    int clock_ok=boot_tile89_now(&q);if(clock_ok && q<boot_tile89.start){InterlockedIncrement64(&boot_tile89.clock_loss);clock_ok=0;}
    if(!clock_ok) {boot_tile89.state=BOOT_TILE89_CLOSED;boot_tile89.stop_reason=BOOT_TILE89_STOP_CLOCK;boot_tile89.stop_qpc=boot_tile89.last_qpc;ReleaseSRWLockExclusive(&boot_tile89.lock);return -1;}
    uint64_t ticks=boot_tile89.frequency*boot_tile89.config.duration_ms/1000;
    boot_tile89.arm=q;
    if(ticks>UINT64_MAX-boot_tile89.arm){ReleaseSRWLockExclusive(&boot_tile89.lock);boot_tile89_error(ERROR_ARITHMETIC_OVERFLOW);return -1;}
    boot_tile89.end=boot_tile89.arm+ticks;boot_tile89.last_qpc=boot_tile89.arm;boot_tile89.armed_consumed=1;boot_tile89.arm_bytes=got;boot_tile89.state=BOOT_TILE89_ACTIVE;
    ReleaseSRWLockExclusive(&boot_tile89.lock);
    char arm[256];int n=snprintf(arm,sizeof arm,"{\"kind\":\"armed\",\"qpc\":%llu,\"end_qpc\":%llu,\"request_bytes\":%lu}\n",(unsigned long long)boot_tile89.arm,(unsigned long long)boot_tile89.end,(unsigned long)got);
    if(n<0 || (size_t)n>=sizeof arm || !boot_tile89_buffer(arm,(size_t)n)){boot_tile89_error(GetLastError());return -1;}
    return 1;
}
static DWORD WINAPI boot_tile89_worker(void *unused) {
    (void)unused;uint64_t unarmed_end=boot_tile89.start+boot_tile89.frequency*1200;
    while(InterlockedCompareExchange(&boot_tile89.state,0,0)==BOOT_TILE89_WAIT_ARM) {
        int armed=boot_tile89_try_arm();if(armed>0)break;
        if(armed<0){boot_tile89_close_admission(BOOT_TILE89_STOP_BAD_ARM);break;}
        uint64_t q;if(!boot_tile89_now(&q)){boot_tile89_close_admission(BOOT_TILE89_STOP_CLOCK);break;}if(q>=unarmed_end){boot_tile89_close_admission(BOOT_TILE89_STOP_UNARMED);break;}Sleep(250);
    }
    for(;;) {
        if(InterlockedCompareExchange64(&boot_tile89.io_error,0,0)){boot_tile89_close_admission(BOOT_TILE89_STOP_IO);break;}
        unsigned n=boot_tile89_take();for(unsigned i=0;i<n;++i) { if(!boot_tile89_row(&boot_tile89.batch[i])){boot_tile89_error(GetLastError());break;}++boot_tile89.written; }
        AcquireSRWLockExclusive(&boot_tile89.lock);uint64_t q=boot_tile89.last_qpc;int clock_ok=boot_tile89_now(&q);
        if(clock_ok && q<boot_tile89.last_qpc){InterlockedIncrement64(&boot_tile89.clock_loss);clock_ok=0;q=boot_tile89.last_qpc;}
        if(boot_tile89.state==BOOT_TILE89_ACTIVE && (!clock_ok || q>=boot_tile89.end || boot_tile89.draws>=boot_tile89.config.max_draws || boot_tile89.accepted>=BOOT_TILE89_MAX_ROWS)) {
            boot_tile89.stop_reason=!clock_ok?BOOT_TILE89_STOP_CLOCK:q>=boot_tile89.end?BOOT_TILE89_STOP_TIME:BOOT_TILE89_STOP_CAP;boot_tile89.stop_qpc=q;boot_tile89.state=BOOT_TILE89_CLOSED;
        }
        int done=boot_tile89.state==BOOT_TILE89_CLOSED && !boot_tile89.count;ReleaseSRWLockExclusive(&boot_tile89.lock);
        if(done)break;if(!n)Sleep(25);
    }
    boot_tile89_close_admission(InterlockedCompareExchange64(&boot_tile89.io_error,0,0)?BOOT_TILE89_STOP_IO:BOOT_TILE89_STOP_TIME);
    uint64_t lock=(uint64_t)InterlockedCompareExchange64(&boot_tile89.lock_loss,0,0),full=(uint64_t)InterlockedCompareExchange64(&boot_tile89.full_loss,0,0),cap=(uint64_t)InterlockedCompareExchange64(&boot_tile89.cap_loss,0,0),clock=(uint64_t)InterlockedCompareExchange64(&boot_tile89.clock_loss,0,0),late=(uint64_t)InterlockedCompareExchange64(&boot_tile89.late_loss,0,0),unissued=(uint64_t)InterlockedCompareExchange64(&boot_tile89.unissued,0,0),faults=(uint64_t)InterlockedCompareExchange64(&boot_tile89.tap_fault,0,0),missing=(uint64_t)InterlockedCompareExchange64(&boot_tile89.ticket_missing,0,0);
    char end[2048];int n=snprintf(end,sizeof end,"{\"kind\":\"end\",\"rows\":%llu,\"draws\":%llu,\"signals\":%llu,\"written\":%llu,\"lock\":%llu,\"full\":%llu,\"cap\":%llu,\"clock\":%llu,\"late\":%llu,\"unissued\":%llu,\"tap_fault\":%llu,\"ticket_missing\":%llu,\"reason\":%u,\"arm_qpc\":%llu,\"stop_qpc\":%llu,\"arm_consumed\":%s,\"arm_bytes\":%u,\"capture_complete_proven\":false}\n",
        (unsigned long long)boot_tile89.accepted,(unsigned long long)boot_tile89.draws,(unsigned long long)boot_tile89.signals,(unsigned long long)boot_tile89.written,(unsigned long long)lock,(unsigned long long)full,(unsigned long long)cap,(unsigned long long)clock,(unsigned long long)late,(unsigned long long)unissued,(unsigned long long)faults,(unsigned long long)missing,boot_tile89.stop_reason,(unsigned long long)boot_tile89.arm,(unsigned long long)boot_tile89.stop_qpc,boot_tile89.armed_consumed?"true":"false",boot_tile89.arm_bytes);
    if(n<0 || (size_t)n>=sizeof end || !boot_tile89_buffer(end,(size_t)n))boot_tile89_error(ERROR_INSUFFICIENT_BUFFER);
    if(!boot_tile89_flush_buffer())boot_tile89_error(GetLastError());
    if(!FlushFileBuffers(boot_tile89.file))boot_tile89_error(GetLastError());
    if(!CloseHandle(boot_tile89.file))boot_tile89_error(GetLastError());boot_tile89.file=INVALID_HANDLE_VALUE;
    int receipt=boot_tile89_finish_receipt(lock,full,cap,clock,late,unissued,faults,missing);
    return receipt && !InterlockedCompareExchange64(&boot_tile89.io_error,0,0)?0:1;
}
static inline int boot_tile89_absent(const char *path) {
    if(GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES)return 0;DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;
}
static BOOL CALLBACK boot_tile89_init_once(PINIT_ONCE once,PVOID parameter,PVOID *context) {
    (void)once;(void)parameter;(void)context;char setting[64];DWORD len=GetEnvironmentVariableA("TOTK_TILE_BIND_TRACE",setting,sizeof setting);
    if(!len || (len==1 && setting[0]=='0'))return TRUE;
    if(len>=sizeof setting || !boot_tile89_config(setting,&boot_tile89.config)){fprintf(stderr,"[tile89] invalid config; disabled\n");return TRUE;}
    LARGE_INTEGER q,f;FILETIME creation,exit,kernel,user;
    if(!QueryPerformanceCounter(&q) || !QueryPerformanceFrequency(&f) || f.QuadPart<=0 || q.QuadPart<=0 ||
       !GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)){fprintf(stderr,"[tile89] identity/clock unavailable; disabled\n");return TRUE;}
    boot_tile89.pid=GetCurrentProcessId();boot_tile89.creation=((uint64_t)creation.dwHighDateTime<<32)|creation.dwLowDateTime;
    boot_tile89.start=(uint64_t)q.QuadPart;boot_tile89.last_qpc=boot_tile89.start;boot_tile89.frequency=(uint64_t)f.QuadPart;
    if(boot_tile89.frequency>UINT64_MAX/120000 || boot_tile89.frequency*1200>UINT64_MAX-boot_tile89.start){fprintf(stderr,"[tile89] clock range; disabled\n");return TRUE;}
    if(!CreateDirectoryA("frames",NULL) && GetLastError()!=ERROR_ALREADY_EXISTS){fprintf(stderr,"[tile89] output directory unavailable; disabled\n");return TRUE;}
    char relative[MAX_PATH];int n=snprintf(relative,sizeof relative,"frames/tile-binding89-%lu-%llu.jsonl",(unsigned long)boot_tile89.pid,(unsigned long long)boot_tile89.start);
    if(n<0 || (size_t)n>=sizeof relative)return TRUE;DWORD absolute=GetFullPathNameA(relative,MAX_PATH,boot_tile89.path,NULL);if(!absolute || absolute>=MAX_PATH)return TRUE;
    if(snprintf(boot_tile89.receipt,MAX_PATH,"%s.receipt.json",boot_tile89.path)>=MAX_PATH || snprintf(boot_tile89.temporary,MAX_PATH,"%s.tmp",boot_tile89.receipt)>=MAX_PATH || snprintf(boot_tile89.request,MAX_PATH,"%s.arm.json",boot_tile89.path)>=MAX_PATH)return TRUE;
    if(!boot_tile89_absent(boot_tile89.receipt) || !boot_tile89_absent(boot_tile89.temporary) || !boot_tile89_absent(boot_tile89.request)){fprintf(stderr,"[tile89] preexisting control/output; disabled\n");return TRUE;}
    boot_tile89.file=CreateFileA(boot_tile89.path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(boot_tile89.file==INVALID_HANDLE_VALUE){fprintf(stderr,"[tile89] fresh output failed; disabled\n");return TRUE;}
    char request[2*MAX_PATH+1];size_t out=0;for(const unsigned char *p=(const unsigned char *)boot_tile89.request;*p;++p){if(*p=='\\' || *p=='"')request[out++]='\\';request[out++]=(char)*p;}request[out]=0;
    char header[2048];n=snprintf(header,sizeof header,"{\"kind\":\"header\",\"version\":89,\"pid\":%lu,\"creation\":%llu,\"start_qpc\":%llu,\"qpc_frequency\":%llu,\"duration_ms\":%u,\"max_draws\":%u,\"record_limit\":%u,\"ring\":%u,\"value_count\":%u,\"raw_words\":%u,\"request\":\"%s\",\"capture_complete_proven\":false,\"gpu_completion_observed\":false}\n",
        (unsigned long)boot_tile89.pid,(unsigned long long)boot_tile89.creation,(unsigned long long)boot_tile89.start,(unsigned long long)boot_tile89.frequency,boot_tile89.config.duration_ms,boot_tile89.config.max_draws,BOOT_TILE89_MAX_ROWS,BOOT_TILE89_RING,BOOT_TILE89_VALUES,BOOT_TILE89_RAW,request);
    if(n<0 || (size_t)n>=sizeof header || !boot_tile89_write(boot_tile89.file,header,(DWORD)n)){CloseHandle(boot_tile89.file);boot_tile89.file=INVALID_HANDLE_VALUE;return TRUE;}
    boot_tile89.enabled=1;
    BootCbvTapObserver observer={boot_tile89_begin,boot_tile89_binding,boot_tile89_table,boot_tile89_end,boot_tile89_issued,boot_tile89_fault};
    if(!boot_cbv_tap_register(BOOT_CBV_TAP_89,&observer)){boot_tile89.enabled=0;CloseHandle(boot_tile89.file);boot_tile89.file=INVALID_HANDLE_VALUE;fprintf(stderr,"[tile89] observer registration failed; disabled\n");return TRUE;}
    boot_dred_work_observer=boot_tile89_dred_work;boot_dred_execute_attempt_observer=boot_tile89_execute_attempt;boot_dred_execute_observer=boot_tile89_dred_execute;
    HANDLE worker=CreateThread(NULL,0,boot_tile89_worker,NULL,0,NULL);
    if(!worker){boot_tile89.enabled=0;CloseHandle(boot_tile89.file);boot_tile89.file=INVALID_HANDLE_VALUE;fprintf(stderr,"[tile89] worker failed; disabled\n");return TRUE;}
    CloseHandle(worker);fprintf(stderr,"[tile89] metadata waiting for one identity-bound arm: %s\n",boot_tile89.request);return TRUE;
}
static inline void boot_tile89_init(void) { static INIT_ONCE once=INIT_ONCE_STATIC_INIT;InitOnceExecuteOnce(&once,boot_tile89_init_once,NULL,NULL); }
#endif
