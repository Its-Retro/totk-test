#ifndef TOTK_HOST_GPU_DRED_TRACE_H
#define TOTK_HOST_GPU_DRED_TRACE_H
/* Diagnostic82: CPU metadata only, no D3D/guest/heap/file operations in hooks.
 * Resets and Execute calls are different serials. DRED list order is not an ID.
 * Matching a retained work-type profile supplies candidates, never GPU proof. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BOOT_DRED_STATE_CAP 16u
#define BOOT_DRED_RESET_CAP 256u
#define BOOT_DRED_EXEC_CAP 256u
#define BOOT_DRED_WORK_CAP 4096u
#define BOOT_DRED_HASH0 UINT64_C(1469598103934665603)
enum { BOOT_DRED_DRAW=3, BOOT_DRED_INDEXED=4, BOOT_DRED_DISPATCH=6 };
enum { BOOT_DRED_MAIN=1, BOOT_DRED_FEEDBACK=2, BOOT_DRED_SO=3, BOOT_DRED_CLEAR=4, BOOT_DRED_TILE=5, BOOT_DRED_COMPUTE=6 };
typedef struct BootDredReset {
    const void *cl,*queue,*device; uint64_t life,reset,sub,hash,work_count,first_work,last_work; unsigned site,flags,executes;
} BootDredReset;
typedef struct BootDredExec { BootDredReset recording; uint64_t execute; unsigned site; } BootDredExec;
typedef struct BootDredState { const void *cl; uint64_t life,reset; unsigned unknown; } BootDredState;
typedef struct BootDredWork {
    const void *cl; uint64_t life,reset,serial,ordinal,sub,target; uint32_t event,op,kind; uint8_t vs[8],ps[8];
} BootDredWork;
static BootDredState boot_dred_states[BOOT_DRED_STATE_CAP];
/*89 optional startup-published observers. CPU metadata only, no waits/IO or
 * additional lock in these callbacks. They never change the original ledger. */
static void (*boot_dred_work_observer)(const BootDredWork *);
static void (*boot_dred_execute_attempt_observer)(const void *,const void *);
static void (*boot_dred_execute_observer)(const BootDredExec *);
static BootDredReset boot_dred_resets[BOOT_DRED_RESET_CAP];
static BootDredExec boot_dred_execs[BOOT_DRED_EXEC_CAP];
static BootDredWork boot_dred_works[BOOT_DRED_WORK_CAP];
static uint64_t boot_dred_life_n,boot_dred_reset_n,boot_dred_exec_n,boot_dred_work_n;
static unsigned boot_dred_state_next;
static SRWLOCK boot_dred_lock=SRWLOCK_INIT;
_Alignas(8) static volatile LONG64 boot_dred_lost_lock,boot_dred_missing_state,boot_dred_state_evictions;
static inline uint64_t boot_dred_hash(uint64_t h,uint32_t op) { return (h^op)*UINT64_C(1099511628211); }
static inline BootDredState *boot_dred_state(const void *cl,int create) {
    for(unsigned i=0;i<BOOT_DRED_STATE_CAP;++i) if(boot_dred_states[i].cl==cl) return &boot_dred_states[i];
    if(!create) return NULL;
    unsigned i; for(i=0;i<BOOT_DRED_STATE_CAP && boot_dred_states[i].cl;++i) {}
    if(i==BOOT_DRED_STATE_CAP) { i=boot_dred_state_next++%BOOT_DRED_STATE_CAP; InterlockedIncrement64(&boot_dred_state_evictions); }
    BootDredState *s=&boot_dred_states[i]; memset(s,0,sizeof *s); s->cl=cl; s->life=++boot_dred_life_n; s->unknown=1; return s;
}
/* Opt-in (diagnostics rule): TOTK_DRED_LEDGER=1, or implicitly while a trace (tile89 etc.) has registered
 * an observer. Off, every hook is one cached test instead of a lock + scan + memset per draw. */
static inline int boot_dred_ledger_on(void) {
    static int v=-1;
    if(v<0){char e[8];DWORD n=GetEnvironmentVariableA("TOTK_DRED_LEDGER",e,sizeof e);v=n&&n<sizeof e&&e[0]=='1';}
    return v || boot_dred_work_observer || boot_dred_execute_observer;
}
static inline int boot_dred_enter(void) {
    if(!boot_dred_ledger_on()) return 0;
    if(TryAcquireSRWLockExclusive(&boot_dred_lock)) return 1;
    InterlockedIncrement64(&boot_dred_lost_lock); return 0;
}
static inline void boot_dred_created(const void *cl) {
    if(!cl || !boot_dred_enter()) return;
    BootDredState *s=boot_dred_state(cl,1); s->life=++boot_dred_life_n; s->reset=0; s->unknown=0;
    ReleaseSRWLockExclusive(&boot_dred_lock);
}
static inline void boot_dred_reset(const void *cl,const void *queue,const void *device,uint64_t sub,unsigned site) {
    if(!cl || !boot_dred_enter()) return;
    BootDredState *s=boot_dred_state(cl,1); uint64_t id=++boot_dred_reset_n;
    BootDredReset *r=&boot_dred_resets[(id-1)%BOOT_DRED_RESET_CAP]; memset(r,0,sizeof *r);
    r->cl=cl; r->queue=queue; r->device=device; r->life=s->life; r->reset=id; r->sub=sub; r->site=site; r->flags=s->unknown; r->hash=BOOT_DRED_HASH0;
    s->reset=id; ReleaseSRWLockExclusive(&boot_dred_lock);
}
static inline void boot_dred_work(const void *cl,uint32_t op,unsigned kind,const uint8_t *vs,const uint8_t *ps,uint64_t target,uint64_t sub,unsigned event) {
    if(!boot_dred_enter()) return;
    BootDredState *s=boot_dred_state(cl,0);
    BootDredReset *r=s && s->reset?&boot_dred_resets[(s->reset-1)%BOOT_DRED_RESET_CAP]:NULL;
    if(!r || r->reset!=s->reset) { InterlockedIncrement64(&boot_dred_missing_state); ReleaseSRWLockExclusive(&boot_dred_lock); return; }
    uint64_t serial=++boot_dred_work_n; BootDredWork *w=&boot_dred_works[(serial-1)%BOOT_DRED_WORK_CAP]; memset(w,0,sizeof *w);
    w->cl=cl; w->life=s->life; w->reset=s->reset; w->serial=serial; w->ordinal=r->work_count++; w->sub=sub; w->event=event; w->op=op; w->kind=kind; w->target=target;
    if(vs) memcpy(w->vs,vs,8); if(ps) memcpy(w->ps,ps,8);
    if(!r->first_work) r->first_work=serial; r->last_work=serial; r->hash=boot_dred_hash(r->hash,op);
    if(boot_dred_work_observer)boot_dred_work_observer(w);
    ReleaseSRWLockExclusive(&boot_dred_lock);
}
static inline void boot_dred_execute(const void *cl,const void *queue,unsigned site) {
    /* Clear89's old TLS ticket on EVERY attempt, including a failed ledger lock. */
    if(boot_dred_execute_attempt_observer)boot_dred_execute_attempt_observer(cl,queue);
    if(!boot_dred_enter()) return;
    uint64_t id=++boot_dred_exec_n; BootDredExec *e=&boot_dred_execs[(id-1)%BOOT_DRED_EXEC_CAP]; memset(e,0,sizeof *e); e->execute=id; e->site=site;
    BootDredState *s=boot_dred_state(cl,0); BootDredReset *r=s && s->reset?&boot_dred_resets[(s->reset-1)%BOOT_DRED_RESET_CAP]:NULL;
    if(r && r->reset==s->reset) { e->recording=*r; ++r->executes; }
    else { e->recording.cl=cl; e->recording.flags=2; InterlockedIncrement64(&boot_dred_missing_state); }
    e->recording.queue=queue;
    if(boot_dred_execute_observer)boot_dred_execute_observer(e);
    ReleaseSRWLockExclusive(&boot_dred_lock);
}
static inline const char *boot_dred_kind(unsigned k) {
    return k==BOOT_DRED_MAIN?"normal_draw":k==BOOT_DRED_FEEDBACK?"feedback_draw":k==BOOT_DRED_SO?"SO_probe_draw":
           k==BOOT_DRED_CLEAR?"masked_clear_draw":k==BOOT_DRED_TILE?"tile_dispatch":k==BOOT_DRED_COMPUTE?"compute_dispatch":"unknown_hook";
}
/* Diagnostic90: failure-report-only context. Fixed retention-order selection
 * is NOT DRED node identity or GPU chronology. No producer path changes. */
#define BOOT_DRED_CONTEXT_CANDIDATES 4u
#define BOOT_DRED_CONTEXT_WORK 8u
static inline void boot_dred_dump_candidate_work(FILE *f,const BootDredExec *candidate,uint64_t ordinal,int unfinished_is_work) {
    const BootDredReset *r=&candidate->recording;
    const BootDredWork *rows[BOOT_DRED_CONTEXT_WORK]={0},*focus=NULL;
    uint64_t retained=0,begin=boot_dred_work_n>BOOT_DRED_WORK_CAP?boot_dred_work_n-BOOT_DRED_WORK_CAP:0;
    unsigned shown=0;
    for(uint64_t i=begin;i<boot_dred_work_n;++i) {
        const BootDredWork *w=&boot_dred_works[i%BOOT_DRED_WORK_CAP];
        if(w->cl!=r->cl || w->life!=r->life || w->reset!=r->reset ||
           w->ordinal>=r->work_count || w->serial<r->first_work || w->serial>r->last_work) continue;
        ++retained;
        if(shown<BOOT_DRED_CONTEXT_WORK) rows[shown++]=w;
        if(unfinished_is_work && w->ordinal==ordinal) focus=w;
    }
    /* Retain the reported unfinished work ordinal in preference to the eighth
     * prefix row. This is a candidate-coordinate match, not failing-work proof. */
    if(focus) {
        unsigned found=0;
        for(unsigned i=0;i<shown;++i) if(rows[i]==focus) found=1;
        if(!found) { if(shown<BOOT_DRED_CONTEXT_WORK) rows[shown++]=focus; else rows[shown-1]=focus; }
    }
    for(unsigned i=0;i<shown;++i) {
        const BootDredWork *w=rows[i];
        fprintf(f,"      %s work-ordinal=%llu DRED-op-type=%u sub=%llu event=%u vs %02x%02x%02x%02x%02x%02x%02x%02x ps %02x%02x%02x%02x%02x%02x%02x%02x target/grid=%llx%s\n",
            boot_dred_kind(w->kind),(unsigned long long)w->ordinal,w->op,(unsigned long long)w->sub,w->event,
            w->vs[0],w->vs[1],w->vs[2],w->vs[3],w->vs[4],w->vs[5],w->vs[6],w->vs[7],w->ps[0],w->ps[1],w->ps[2],w->ps[3],w->ps[4],w->ps[5],w->ps[6],w->ps[7],
            (unsigned long long)w->target,w==focus?" (candidate at DRED work ordinal only; identity UNKNOWN)":"");
    }
    fprintf(f,"      candidate detail bounds: shown=%u retained=%llu recorded=%llu detail_limit=%u; snapshot-bounded CPU context only\n",
        shown,(unsigned long long)retained,(unsigned long long)r->work_count,BOOT_DRED_CONTEXT_WORK);
    if(!shown) fprintf(f,"      candidate work details unavailable: ring overwritten or missing hook; no fallback\n");
    if(retained<r->work_count) fprintf(f,"      candidate work details partial: retained=%llu recorded=%llu (work ring overwritten or missing details); absence is not proof\n",(unsigned long long)retained,(unsigned long long)r->work_count);
    if(retained>shown) fprintf(f,"      candidate work details omitted by bound: %llu; no absence inference\n",(unsigned long long)(retained-shown));
    if(unfinished_is_work && !focus) fprintf(f,"      unfinished candidate work details unavailable: no retained row at ordinal=%llu; no fallback\n",(unsigned long long)ordinal);
}
static inline void boot_dred_dump(FILE *f,const void *cl,const void *queue,const uint32_t *ops,unsigned count,unsigned last) {
    if(!boot_dred_enter()) { fprintf(f,"    diagnostic82 unavailable: metadata lock busy; no older-recording fallback\n"); return; }
    uint64_t work_count=0,hash=BOOT_DRED_HASH0,ordinal=0; int unfinished_is_work=0;
    int history_ok=ops && count<=65536 && last<=count;
    if(history_ok) for(unsigned i=0;i<count;++i) if(ops[i]==BOOT_DRED_DRAW || ops[i]==BOOT_DRED_INDEXED || ops[i]==BOOT_DRED_DISPATCH) {
        if(i<last) ++ordinal; if(i==last) unfinished_is_work=1; ++work_count; hash=boot_dred_hash(hash,ops[i]);
    }
    LONG64 loss_lock=InterlockedCompareExchange64(&boot_dred_lost_lock,0,0),missing=InterlockedCompareExchange64(&boot_dred_missing_state,0,0),evictions=InterlockedCompareExchange64(&boot_dred_state_evictions,0,0);
    int ledger_eligible=!loss_lock && !missing && !evictions;
    fprintf(f,"    diagnostic82: DRED linked-node order is not an execution ID; no nearest-older fallback\n");
    fprintf(f,"    metadata totals reset=%llu execute_calls=%llu work_calls=%llu loss_lock=%lld missing_state=%lld state_evictions=%lld; retained reset/execute/work caps %u/%u/%u\n",
        (unsigned long long)boot_dred_reset_n,(unsigned long long)boot_dred_exec_n,(unsigned long long)boot_dred_work_n,
        (long long)loss_lock,(long long)missing,(long long)evictions,BOOT_DRED_RESET_CAP,BOOT_DRED_EXEC_CAP,BOOT_DRED_WORK_CAP);
    if(!ledger_eligible) fprintf(f,"    candidate association suppressed: a pre-dump metadata loss/missing-state/eviction can leave recording generations untrusted; retained rows below are context only\n");
    unsigned resets=0,executions=0,candidates=0,contexts=0; const BootDredExec *context[BOOT_DRED_CONTEXT_CANDIDATES]={0};
    uint64_t begin=boot_dred_reset_n>BOOT_DRED_RESET_CAP?boot_dred_reset_n-BOOT_DRED_RESET_CAP:0;
    for(uint64_t i=begin;i<boot_dred_reset_n;++i) { const BootDredReset *r=&boot_dred_resets[i%BOOT_DRED_RESET_CAP]; if(r->cl!=cl) continue; ++resets;
        fprintf(f,"      retained reset=%llu life=%llu device=%p queue=%p site=%u sub=%llu work=%llu execute_calls=%u flags=%u%s\n",
            (unsigned long long)r->reset,(unsigned long long)r->life,r->device,r->queue,r->site,(unsigned long long)r->sub,(unsigned long long)r->work_count,r->executes,r->flags,r->executes?"":" (no retained Execute call for this Reset)");
    }
    begin=boot_dred_exec_n>BOOT_DRED_EXEC_CAP?boot_dred_exec_n-BOOT_DRED_EXEC_CAP:0;
    for(uint64_t i=begin;i<boot_dred_exec_n;++i) { const BootDredExec *e=&boot_dred_execs[i%BOOT_DRED_EXEC_CAP]; const BootDredReset *r=&e->recording; if(r->cl!=cl || r->queue!=queue) continue; ++executions;
        int match=ledger_eligible && history_ok && !r->flags && r->work_count==work_count && r->hash==hash;
        fprintf(f,"      retained Execute call=%llu reset=%llu life=%llu sub=%llu work=%llu site=%u flags=%u work_type_profile=%s\n",
            (unsigned long long)e->execute,(unsigned long long)r->reset,(unsigned long long)r->life,(unsigned long long)r->sub,(unsigned long long)r->work_count,e->site,r->flags,match?"candidate":"different/unavailable");
        if(match) { if(contexts<BOOT_DRED_CONTEXT_CANDIDATES) context[contexts++]=e; ++candidates; }
    }
    fprintf(f,"    association UNKNOWN: retained resets=%u Execute calls=%u matching-work-type candidates=%u; history=%s work=%llu next_work_ordinal=%llu unfinished_op_is_work=%d\n",
        resets,executions,candidates,history_ok?"available":"unavailable/truncated",(unsigned long long)work_count,(unsigned long long)ordinal,unfinished_is_work);
    if(candidates==1 && contexts==1) {
        fprintf(f,"    sole retained profile candidate only (not proven node identity/GPU cause): Execute call=%llu Reset=%llu\n",
            (unsigned long long)context[0]->execute,(unsigned long long)context[0]->recording.reset);
    } else fprintf(f,"    no draw attribution: %s\n",candidates?"ambiguous retained profiles":"no matching retained profile (evicted/missing hook/lifetime/history mismatch possible)");
    if(contexts) {
        fprintf(f,"    diagnostic90 candidate contexts: shown=%u total=%u candidate_limit=%u work_limit=%u; association UNKNOWN; retained-ring order is not DRED chronology\n",
            contexts,candidates,BOOT_DRED_CONTEXT_CANDIDATES,BOOT_DRED_CONTEXT_WORK);
        for(unsigned i=0;i<contexts;++i) {
            const BootDredExec *e=context[i]; const BootDredReset *r=&e->recording;
            fprintf(f,"    retained candidate context %u/%u: Execute call=%llu Reset=%llu life=%llu sub=%llu site=%u; node identity/GPU cause UNKNOWN\n",
                i+1,contexts,(unsigned long long)e->execute,(unsigned long long)r->reset,(unsigned long long)r->life,(unsigned long long)r->sub,e->site);
            boot_dred_dump_candidate_work(f,e,ordinal,unfinished_is_work);
        }
        if(candidates>contexts) fprintf(f,"    candidate contexts omitted by bound: %u; retained Execute rows above remain context only\n",candidates-contexts);
    }
    ReleaseSRWLockExclusive(&boot_dred_lock);
}
#endif
