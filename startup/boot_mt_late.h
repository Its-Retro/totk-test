/* Multi-core execution (TOTK_CORES>1): workers, kernel entry, scheduling glue. Guest memory goes
 * through fastmem (process-wide mapping state; no per-core TLB).
 * Included at the end of boot_runtime.h. See boot_mt_early.h and mt-work/DESIGN.md. */
#ifndef TOTK_BOOT_MT_LATE_H
#define TOTK_BOOT_MT_LATE_H
#include <stddef.h>

typedef struct BootCore {
    BootContext ctx;                /* must be first: translated code's c */
    BootCoreHot hot;                /* must follow ctx: fastmem pc hint / page mark (BOOT_CORE_HOT) */
    uint8_t bounce[64];             /* gathered straddling reads (per core) */
    unsigned index,vcore,thread;    /* thread: index into the master's table or BOOT_MT_IDLE */
    uint64_t pc,quota_end;          /* resume pc of the running thread; step quota end */
    volatile LONG interrupt,in_guest,idle;
    HANDLE doorbell,host,timer;
} BootCore;
_Static_assert(offsetof(BootCore,hot)==sizeof(BootContext),"hot fields must follow the core context");
/* TOTK_THREADS=host, per worker: wait slot + 1 whose handle ended the park (0 none). Outside BootCore so the
 * page header does not change. Counters: parks ended by a wait handle / by the doorbell. */
static unsigned boot_mt_woke[BOOT_MT_MAX];
static volatile LONG64 boot_mt_handle_wakes,boot_mt_doorbell_wakes;
static volatile LONG64 boot_mt_stale_resched;   /* overlap-work P2: stale RESCHED kicks cleared in boot_mt_leave */
/* TOTK_OVERLAP=0 (guardfix): the serial behaviour before overlap-work step 2 (play-84). Stale RESCHED kicks are kept
 * (boot_mt_leave), the window-closed check takes the window lock (host_window.h boot_window_is_closed) and the frame
 * guard is off unless TOTK_FRAME_GUARD=1 (host_nvn_overlap.h). Default (unset or anything else): overlap on. */
static inline int boot_mt_overlap_on(void){
    static volatile LONG v=-1;
    if(v<0){char e[8];DWORD n=GetEnvironmentVariableA("TOTK_OVERLAP",e,sizeof e);v=!(n&&n<sizeof e&&e[0]=='0');}
    return (int)v;
}
/* Quiesce: the kernel (lock held) parks every other core outside guest code, for host work that moves live
 * guest memory (boot_gpu_pack_regions). A core re-enters guest code (and host functions that touch guest
 * memory) only through boot_mt_guest_enter: in_guest=1, then check the flag (Dekker with
 * boot_mt_quiesce_begin, both seq_cst), so once the kernel has seen in_guest==0 that core cannot come back
 * until boot_mt_quiesce_end. */
__declspec(selectany) volatile LONG boot_mt_quiesce=0;
static inline void boot_mt_guest_enter(BootCore*k){
    for(;;){
        __atomic_store_n(&k->in_guest,1,__ATOMIC_SEQ_CST);
        if(!__atomic_load_n(&boot_mt_quiesce,__ATOMIC_SEQ_CST))return;
        __atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);
        while(__atomic_load_n(&boot_mt_quiesce,__ATOMIC_SEQ_CST))YieldProcessor();
    }
}
#include "boot_mt_crash.h"
/* Non-faulting guest pointer for host functions running on a core context (TOTK_CORES>1). A core
 * context's region table is the snapshot taken when the workers started, so never scan it: use the
 * process-wide fastmem page table, else the master's region table under the kernel lock. NULL when no
 * region holds [a, a+n) entirely (the caller treats that as "not ours"). */
static inline const uint8_t*boot_mt_peek_ptr(const BootContext*c,uint64_t a,uint64_t n){
    (void)c;
#if TOTK_FASTMEM
    if(boot_fm.table_ctx && n && n<=4096 && (a&4095)+n<=4096){const uint8_t*h=boot_fm_table_ptr(c,a,(unsigned)n,READ);if(h)return h;}
#endif
    const BootContext*M=boot_mt.master;const uint8_t*result=NULL;
    if(!M)return NULL;
    BootCore*k=boot_mt_core_ctx(c)?(BootCore*)(void*)c:NULL;   /* a quiescing kernel holding the lock must not wait for us */
    if(k)__atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);
    AcquireSRWLockShared(&boot_mt.klock);
    for(unsigned i=0;i<M->region_count && i<BOOT_REGION_CAPACITY;++i){const Region*r=&M->regions[i];
        if(r->data && a>=r->base && a-r->base<r->size && n<=r->size-(a-r->base)){result=r->data+(size_t)(a-r->base);break;}}
    ReleaseSRWLockShared(&boot_mt.klock);
    if(k)boot_mt_guest_enter(k);
    return result;
}
#if TOTK_FASTMEM && TOTK_FM_FIXED
/* Apply a pending table change to the guest view now (fixed view: no gate re-syncs it lazily). Takes the
 * kernel lock unless the caller already holds it (inside the kernel). */
static inline void boot_fm_sync_pending(void){
    if(!__atomic_load_n(&boot_fm.pending,__ATOMIC_ACQUIRE))return;
    /* play (48): skip the lock only when THIS thread is the core inside the kernel (boot_mt_in_kernel() is
     * "any core": a lock-free host function exiting while another core ran an SVC synced concurrently) */
    int held=boot_mt.active && boot_mt.kernel_core!=UINT32_MAX && (int)boot_mt.kernel_core==boot_mt_worker_of_thread(GetCurrentThreadId());
    int lock=boot_mt.active && !held;
    if(lock)AcquireSRWLockExclusive(&boot_mt.klock);
    if(boot_fm.pending)boot_fm_sync(boot_fm.pending);
    if(lock)ReleaseSRWLockExclusive(&boot_mt.klock);
}
#endif
static inline void boot_mt_boost_workers(unsigned mode){
    for(unsigned w=0;w<boot_mt.workers && w<BOOT_MT_MAX;++w)if(boot_mt.core[w] && boot_mt.core[w]->host)
        SetThreadPriority(boot_mt.core[w]->host,mode?THREAD_PRIORITY_ABOVE_NORMAL:THREAD_PRIORITY_NORMAL);
}
enum { BOOT_MT_INT_RESCHED=1, BOOT_MT_INT_STOP=2 };
#define BOOT_MT_CHUNK UINT64_C(1000000)

static inline uint64_t boot_mt_total_steps(void){
    uint64_t n=boot_mt.base_steps;
    for(unsigned w=0;w<boot_mt.workers;++w)n+=__atomic_load_n(&boot_mt.core[w]->ctx.steps,__ATOMIC_RELAXED);
    return n;
}
/* Interrupt worker w: it leaves translated code at its next block check (limit hijack) or wakes. */
static inline void boot_mt_kick(unsigned w,LONG bits){
    BootCore*k=boot_mt.core[w];
    _InterlockedOr(&k->interrupt,bits);
    __atomic_store_n(&k->ctx.limit,0,__ATOMIC_SEQ_CST);
    if(__atomic_load_n(&k->idle,__ATOMIC_SEQ_CST))SetEvent(k->doorbell);
    ++boot_mt.kicks;
}
/* chain P1 (play-work/chain-work): doorbells rung after the kernel lock is released. kick_check runs at the end of
 * every kernel exit with the lock held; SetEvent there (a syscall, 1-3 us, several per fan-out) lengthened the hold
 * every other core convoys on, and the woken worker's first act (boot_mt_service) is to take that same lock. The
 * interrupt bit and the limit hijack are still set under the lock; only the doorbell moves out. Dekker pair as
 * before: kicker interrupt (locked OR) then idle load; worker idle store then interrupt load (both seq_cst), so either
 * the worker sees the interrupt or the releaser sees idle=1 and rings (an auto-reset doorbell rung early stays set).
 * Workers' doorbells live until boot_mt_execute has joined every worker. TOTK_MT_DEFER_KICK=0 restores ringing
 * under the lock. Only the TOTK_THREADS=host branch of kick_check defers; quiesce/stop kicks ring at once. */
static unsigned boot_mt_bell[BOOT_MT_MAX],boot_mt_bells;   /* kernel lock held */
static inline int boot_mt_defer_kicks(void){static int v=-1;if(v<0){const char*e=getenv("TOTK_MT_DEFER_KICK");v=!(e && !strcmp(e,"0"));}return v;}
static inline void boot_mt_kick_deferred(unsigned w,LONG bits){
    BootCore*k=boot_mt.core[w];
    _InterlockedOr(&k->interrupt,bits);
    __atomic_store_n(&k->ctx.limit,0,__ATOMIC_SEQ_CST);
    ++boot_mt.kicks;
    for(unsigned i=0;i<boot_mt_bells;++i)if(boot_mt_bell[i]==w)return;
    if(boot_mt_bells<BOOT_MT_MAX)boot_mt_bell[boot_mt_bells++]=w;
    else if(__atomic_load_n(&k->idle,__ATOMIC_SEQ_CST))SetEvent(k->doorbell);   /* cannot happen (one slot per worker) */
}
/* chain P7: bounded spin on the kernel lock before the blocking acquire. Holds are ~1.5-3 us (wait graph: 0x34/0x35
 * hold 1.6-2.2 us) while a blocked SRW acquire costs an OS sleep and wake (15-50 us measured); a short TryAcquire spin
 * turns most convoy waits into a few microseconds. TOTK_MT_KLOCK_SPIN=<pauses> (default 64: ~4.5 us on a P-core at
 * ~70 ns per PAUSE, ~2.2 us on an E-core), 0 = plain AcquireSRWLockExclusive. The in_guest/quiesce protocol is
 * unchanged: callers clear in_guest before calling, exactly as before the plain acquire. */
static inline unsigned boot_mt_klock_spin(void){static int v=-1;if(v<0){char e[16];DWORD n=GetEnvironmentVariableA("TOTK_MT_KLOCK_SPIN",e,sizeof e);long x=n && n<sizeof e?strtol(e,NULL,10):64;v=x<0?0:x>4096?4096:(int)x;}return (unsigned)v;}
static inline void boot_mt_klock_acquire(void){
    for(unsigned s=boot_mt_klock_spin();s;--s){if(TryAcquireSRWLockExclusive(&boot_mt.klock))return;YieldProcessor();}
    AcquireSRWLockExclusive(&boot_mt.klock);
}
/* Release the kernel lock, then ring the doorbells P1 collected (outside the lock). */
static inline void boot_mt_klock_release(void){
    unsigned n=boot_mt_bells,bell[BOOT_MT_MAX];
    for(unsigned i=0;i<n;++i)bell[i]=boot_mt_bell[i];
    boot_mt_bells=0;
    ReleaseSRWLockExclusive(&boot_mt.klock);
    for(unsigned i=0;i<n;++i){BootCore*k=boot_mt.core[bell[i]];if(__atomic_load_n(&k->idle,__ATOMIC_SEQ_CST))SetEvent(k->doorbell);}
}
/* CPU state of the running guest thread: core context <-> master. */
static inline void boot_mt_copy_cpu(BootContext*d,const BootContext*s){
    memcpy(d->x,s->x,sizeof(d->x));memcpy(d->q,s->q,sizeof(d->q));
    d->sp=s->sp;d->tpidrro=s->tpidrro;d->tpidr=s->tpidr;d->n=s->n;d->z=s->z;d->carry=s->carry;d->v=s->v;
    d->fpcr=s->fpcr;d->fpsr=s->fpsr;d->exclusive_valid=s->exclusive_valid;d->exclusive_address=s->exclusive_address;
    d->exclusive_value=s->exclusive_value;d->exclusive_value_hi=s->exclusive_value_hi;d->exclusive_width=s->exclusive_width;
    d->svc_id=s->svc_id;
}
/* TOTK_THREADS=host identity check: a thread's TLS address (tpidrro) is fixed at creation, so a core context
 * whose tpidrro is not that of the thread it claims to run carries another thread's CPU state. One stderr line
 * per event (at most 16): where it was seen, the worker, its bound thread and whose TLS it has. */
static inline void boot_mt_identity_log(const BootCore*k,const char*where,unsigned thread){
    static volatile LONG lines;if(_InterlockedIncrement(&lines)>16)return;
    const BootContext*M=boot_mt.master;unsigned owner=BOOT_MT_IDLE;
    for(unsigned i=0;i<M->thread_count;++i)if(M->threads[i].tpidrro==k->ctx.tpidrro)owner=i;
    fprintf(stderr,"host threads identity (%s): worker %u bound %u thread %u, core tpidrro %#llx is thread %u's, svc %#x pc %#llx\n",
            where,k->index,boot_mt.bound[k->index],thread,(unsigned long long)k->ctx.tpidrro,owner,k->ctx.svc_id,(unsigned long long)k->pc);
}
/* play (51): idle spin (pauses) before a worker parks, adapted per worker; how idle waits ended */
static volatile LONG64 boot_mt_spin_wakes, boot_mt_park_wakes; static uint8_t boot_mt_spin_score[BOOT_MT_MAX+1];
static inline unsigned boot_mt_spin_env(const char*name,long dflt){
    char e[16];DWORD n=GetEnvironmentVariableA(name,e,sizeof e);long x=n && n<sizeof e?strtol(e,NULL,10):dflt;return x<0?0u:x>65536?65536u:(unsigned)x;
}
static inline unsigned boot_mt_spin_limit(void){static int v=-1;if(v<0)v=(int)boot_mt_spin_env("TOTK_MT_SPIN",4096);return (unsigned)v;}
static inline unsigned boot_mt_spin_floor(void){static int v=-1;if(v<0)v=(int)boot_mt_spin_env("TOTK_MT_SPIN_MIN",256);return (unsigned)v;}
/* score 0..8: +2 per idle wait that ended while spinning, -1 per park; the spin scales from floor to limit */
static inline unsigned boot_mt_spin_for(unsigned w){unsigned lo=boot_mt_spin_floor(),hi=boot_mt_spin_limit();if(lo>hi)lo=hi;
    unsigned s=boot_mt_spin_score[w<=BOOT_MT_MAX?w:BOOT_MT_MAX];return lo+(unsigned)((uint64_t)(hi-lo)*s/8);}
/* play (52): kernel lock wait/hold per SVC number (0x00-0x7f; 0x80 = higher numbers, 0x81 = core slow path) */
static volatile LONG64 boot_klock_n[0x84], boot_klock_wait[0x84], boot_klock_hold[0x84];   /* chain P0: 0x82 service, 0x83 timer tick */
static volatile LONG64 boot_mt_fast_yields;   /* play (60) */
static volatile LONG64 boot_mt_fast_signals;  /* play (61) */
static inline uint64_t boot_klock_now(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return (uint64_t)t.QuadPart;}
/* ---- TOTK_THREADS=host wait graph (host-wait-graph.log; TOTK_WAIT_GRAPH=0 turns it off) ----
 * Every block of a guest thread (state 4 mutex, 5 handles, 6 address arbiter, 7 condvar, 8 sleep) is timed from
 * the kernel exit that parked it to the service that runs it again, with the object it waited on (address or
 * first handle), the thread that made it runnable (kernel wakeups: the thread in the kernel at that moment;
 * "timer" = deadline or 1 ms poll; "event" = its own wait handle fired) and the latency from that wakeup to
 * running. Once a second the top 10 edges by blocked time and the threads' run/blocked shares are appended.
 * All updates happen under the kernel lock. */
#define BOOT_WG_EDGES 2048
#define BOOT_WG_TIMER 0xfffeu
#define BOOT_WG_EVENT 0xfffdu
typedef struct { uint64_t key,obj,blocked,latency,count;unsigned thread,state,waker; } BootWgEdge;
typedef struct {
    int on;uint64_t freq,last_dump;
    uint64_t block_at[BOOT_THREAD_CAPACITY],wake_at[BOOT_THREAD_CAPACITY],run_at[BOOT_THREAD_CAPACITY],obj[BOOT_THREAD_CAPACITY];
    uint64_t ran[BOOT_THREAD_CAPACITY],blocked[BOOT_THREAD_CAPACITY];
    unsigned state[BOOT_THREAD_CAPACITY],waker[BOOT_THREAD_CAPACITY];
    BootWgEdge edge[BOOT_WG_EDGES];unsigned edges;
} BootWg;
static BootWg boot_wg;
/* Time spent waiting for the renderer lock, per worker (index BOOT_MT_MAX: not a worker). */
static volatile LONG64 boot_render_wait[BOOT_MT_MAX+1];
static inline void boot_ct_init(void);   /* chain P0: defined below, used by boot_wg_on */
static inline uint64_t boot_wg_now(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return (uint64_t)q.QuadPart;}
static inline int boot_wg_on(void){
    if(!boot_wg.on){const char*v=getenv("TOTK_WAIT_GRAPH");boot_wg.on=v && !strcmp(v,"0")?-1:1;boot_ct_init();   /* chain P0 */
        LARGE_INTEGER f;QueryPerformanceFrequency(&f);boot_wg.freq=(uint64_t)f.QuadPart;boot_wg.last_dump=boot_wg_now();}
    return boot_wg.on>0 && boot_mt.host_threads;
}
/* play (62): kernel-lock statistics. The per-SVC counters are only read by boot_wg_dump, so with TOTK_WAIT_GRAPH=0
 * the three QueryPerformanceCounter calls per kernel entry are skipped. Every update happens with the kernel lock held
 * (exclusive), so plain read-modify-write replaces the three LOCK-prefixed operations per kernel entry that bounced
 * their cache lines between all 60 workers; the dump's InterlockedExchange64 reset can at worst drop one count. */
static inline void boot_klock_add(unsigned i,uint64_t wait,uint64_t hold){
    boot_klock_n[i]=boot_klock_n[i]+1;boot_klock_wait[i]=boot_klock_wait[i]+(LONG64)wait;boot_klock_hold[i]=boot_klock_hold[i]+(LONG64)hold;
}
/* batch-a (TOTK_FRAME_TRACE=1, host_nvn.h boot_nvn_ftrace_write): the guest waits of the NVN submitting thread (t2 in
 * TOTK) and the timed-out waits of every guest thread, for the per-submission frame trace. A wait is noted at its SVC
 * (0x0b SleepThread, 0x18 WaitSynchronization, 0x1a ArbitrateLock, 0x1c WaitProcessWideKeyAtomic, 0x34 WaitForAddress)
 * and finished either at that kernel exit (it did not block: its x0) or when the thread's worker loads it again
 * (boot_mt_service: the x0 its waker or its deadline left, 0xea01 = timed out; "early" = timed out in less than 90% of
 * the timeout it asked for). Off until host_nvn's first traced submit: one predictable branch per SVC. Written with the
 * kernel lock held; boot_mt_ft_copy reads under the shared kernel lock. */
#define BOOT_MT_FT_WAITS 64u
typedef struct BootMtFtWait { uint64_t obj,timeout_ns,qpc,waited; uint32_t svc,result,parked,pad; } BootMtFtWait;
typedef struct BootMtFt {
    volatile LONG on,thread; uint32_t head;
    BootMtFtWait ring[BOOT_MT_FT_WAITS];
    uint64_t pend_qpc[BOOT_THREAD_CAPACITY],pend_obj[BOOT_THREAD_CAPACITY],pend_to[BOOT_THREAD_CAPACITY]; uint32_t pend_svc[BOOT_THREAD_CAPACITY];
    uint32_t timeouts[BOOT_THREAD_CAPACITY],early[BOOT_THREAD_CAPACITY];
} BootMtFt;
static BootMtFt boot_mt_ft;
static inline int boot_mt_ft_svc(unsigned svc){return svc==0x0b || svc==0x18 || svc==0x1a || svc==0x1c || svc==0x34;}
static inline void boot_mt_ft_begin(unsigned i,unsigned svc,const BootContext*c){   /* kernel lock held */
    if(i>=BOOT_THREAD_CAPACITY)return;
    boot_mt_ft.pend_qpc[i]=boot_klock_now();boot_mt_ft.pend_svc[i]=svc;
    boot_mt_ft.pend_obj[i]=svc==0x34?c->x[0]:svc==0x0b?0:c->x[1];
    boot_mt_ft.pend_to[i]=svc==0x0b?c->x[0]:svc==0x1a?UINT64_MAX:c->x[3];
}
static inline void boot_mt_ft_end(unsigned i,uint32_t result,unsigned parked){   /* kernel lock held */
    if(i>=BOOT_THREAD_CAPACITY || !boot_mt_ft.pend_qpc[i])return;
    static uint64_t freq;if(!freq){LARGE_INTEGER f;QueryPerformanceFrequency(&f);freq=f.QuadPart>0?(uint64_t)f.QuadPart:1;}
    uint64_t now=boot_klock_now(),waited=now-boot_mt_ft.pend_qpc[i],to=boot_mt_ft.pend_to[i];
    if(result==UINT32_C(0xea01) && boot_mt_ft.pend_svc[i]!=0x0b && (int64_t)to>0){
        ++boot_mt_ft.timeouts[i];
        if((unsigned __int128)waited*1000000000u*10u<(unsigned __int128)to*freq*9u)++boot_mt_ft.early[i];
    }
    if((LONG)i==boot_mt_ft.thread){BootMtFtWait*w=&boot_mt_ft.ring[boot_mt_ft.head++%BOOT_MT_FT_WAITS];
        w->obj=boot_mt_ft.pend_obj[i];w->timeout_ns=to;w->qpc=boot_mt_ft.pend_qpc[i];w->waited=waited;
        w->svc=boot_mt_ft.pend_svc[i];w->result=result;w->parked=parked;w->pad=0;}
    boot_mt_ft.pend_qpc[i]=0;
}
/* host_nvn's frame trace (outside the kernel): the traced thread's waits since *seen (at most cap, the newest kept) and
 * every thread's cumulative timeout counts. */
static inline unsigned boot_mt_ft_copy(BootContext*c,uint32_t*seen,BootMtFtWait*out,unsigned cap,uint32_t*timeouts,uint32_t*early){
    if(!boot_mt.active)return 0;
    BootCore*k=boot_mt_core_ctx(c)?(BootCore*)(void*)c:NULL;unsigned n=0;
    if(k)__atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);   /* a quiescing kernel holding the lock must not wait for us */
    AcquireSRWLockShared(&boot_mt.klock);
    uint32_t head=boot_mt_ft.head,avail=head-*seen;   /* overwritten entries are skipped */
    if(avail>BOOT_MT_FT_WAITS)avail=BOOT_MT_FT_WAITS;
    if(avail>cap)avail=cap;
    for(uint32_t q=head-avail;q!=head;++q)out[n++]=boot_mt_ft.ring[q%BOOT_MT_FT_WAITS];
    *seen=head;
    memcpy(timeouts,boot_mt_ft.timeouts,sizeof boot_mt_ft.timeouts);memcpy(early,boot_mt_ft.early,sizeof boot_mt_ft.early);
    ReleaseSRWLockShared(&boot_mt.klock);
    if(k)boot_mt_guest_enter(k);
    return n;
}
/* batch-a: TOTK_FAST_SIGNAL_LOCK (on unless =0). The play (61) type-0 SignalToAddress shortcut decided "nobody queued"
 * from the thread table without the kernel lock. A waiter in svc 0x34 reads the word and parks (state 6) inside one
 * exclusive kernel section, so a signal that lands between the two saw no waiter and returned: a lost wakeup (the
 * waiter sleeps until its timeout or the next signal). The shortcut now reads the table under a shared try-lock and
 * takes the kernel path whenever the kernel is busy. */
static inline int boot_mt_fast_signal_lock_on(void){static int v=-1;if(v<0){char e[8];DWORD n=GetEnvironmentVariableA("TOTK_FAST_SIGNAL_LOCK",e,sizeof e);v=!(n && n<sizeof e && e[0]=='0');}return v;}
/* logic-batch (play-work/logic-batch): kernel-lock-free outcomes for the open-world game phase. Runtime only.
 *   TOTK_MT_ACCESS_FAST (on unless =0), L1: boot_mt_access_slow serves an access that crosses a 4 KiB page, when both
 *     pages are mapped with the permission, without the kernel lock (boot_mt_access_cross).
 *   TOTK_ARB_FAST (on unless =0; TOTK_ARBITER_KERNEL=1 turns it off too), L2: WaitForAddress whose comparison fails,
 *     SignalToAddress types 1/2 with no waiter queued, ArbitrateLock whose word no longer names the owner with the
 *     waiters bit, ArbitrateUnlock / SignalProcessWideKey with no waiter queued: the same result as the kernel path,
 *     without the exclusive kernel lock (boot_mt_arb_fast).
 *   TOTK_MT_LOGIC_STATS (on unless =0): every 10 s one stderr line "logic-batch:" with the per-frame counts (calls by
 *     SVC and type, kernel vs fast, why boot_mt_access_slow ran) and the CPU-set placements (boot_lb_report).
 * Counters: one row per worker, written only by that worker (plain stores, no LOCK prefix); the report sums racily. */
enum {
    BOOT_LB_W=0,           /* 0x34 WaitForAddress, by type 0..2: +0 kernel, +3 fast (comparison failed: 0xfa01) */
    BOOT_LB_S=6,           /* 0x35 SignalToAddress, by type 0..2: +0 kernel, +3 fast (no waiter), +6 of those: compare-exchange failed */
    BOOT_LB_S_QUEUED=15,BOOT_LB_S_BUSY=16,   /* 0x35 sent to the kernel: a waiter is queued / the kernel lock was held */
    BOOT_LB_LOCK_K=17,BOOT_LB_LOCK_F=18,BOOT_LB_UNLOCK_K=19,BOOT_LB_UNLOCK_F=20,   /* 0x1a / 0x1b */
    BOOT_LB_CVWAIT_K=21,BOOT_LB_CVSIG_K=22,BOOT_LB_CVSIG_F=23,                    /* 0x1c / 0x1d */
    BOOT_LB_AS_STATUS=24,BOOT_LB_AS_DETACHED=25,BOOT_LB_AS_SAME=26,BOOT_LB_AS_VIEW=27,BOOT_LB_AS_CROSS=28,
    BOOT_LB_AS_INPAGE=29,BOOT_LB_AS_OTHER=30,BOOT_LB_AS_OFF=31,   /* boot_mt_access_slow reasons (see boot_mt_access_cross) */
    BOOT_LB_N=32
};
static uint64_t boot_lb_n[BOOT_MT_MAX+1][BOOT_LB_N] __attribute__((aligned(64)));   /* 256 bytes: whole cache lines per worker */
static inline void boot_lb_add(unsigned w,unsigned i){uint64_t*p=&boot_lb_n[w<BOOT_MT_MAX?w:BOOT_MT_MAX][i];__atomic_store_n(p,__atomic_load_n(p,__ATOMIC_RELAXED)+1,__ATOMIC_RELAXED);}
static inline int boot_lb_env_on(const char*name){char e[8];DWORD n=GetEnvironmentVariableA(name,e,sizeof e);return !(n && n<sizeof e && e[0]=='0');}
static inline int boot_mt_access_fast_on(void){static int v=-1;if(v<0)v=boot_lb_env_on("TOTK_MT_ACCESS_FAST");return v;}
static inline int boot_mt_arb_fast_on(void){static int v=-1;if(v<0){const char*k=getenv("TOTK_ARBITER_KERNEL");v=boot_lb_env_on("TOTK_ARB_FAST") && !(k && !strcmp(k,"1"));}return v;}
static inline int boot_mt_lb_stats_on(void){static int v=-1;if(v<0)v=boot_lb_env_on("TOTK_MT_LOGIC_STATS");return v;}
static volatile LONG64 boot_mt_cpuset_p,boot_mt_cpuset_e,boot_mt_cpuset_fail;   /* L3: SetThreadSelectedCpuSets calls by target / failed */
/* chain P0 (play-work/chain-work): TOTK_CHAIN_TRACE=1 (with the wait graph on) records every block / wake / run of a
 * guest thread with its QPC time, object, waker and the host CPU it happened on into a ring; the 1 s dump copies the
 * new records out (kernel lock held) and boot_wg_flush appends them to chain-trace.bin outside the lock. With the
 * QPC times of present-qpc-*.csv, chain_critical_path.py walks each frame's critical path back from its present:
 * guest-thread run segments (compute, incl. in-SVC time), wake->run gaps (our wake latency) and true waits.
 * Off (default): one predictable branch in the three wait-graph hooks. Also counted whenever the wait graph is on:
 * runs on E-cores per thread (CPU set efficiency class 0 on a hybrid CPU) and SwitchToThread yields per thread. */
typedef struct { uint64_t qpc,obj; uint16_t thread,other; uint8_t kind,state,cpu,eff; } BootCtRec;   /* 24 bytes */
#define BOOT_CT_RING 65536u
static BootCtRec boot_ct_ring[BOOT_CT_RING],boot_ct_out[BOOT_CT_RING];static uint32_t boot_ct_head,boot_ct_tail,boot_ct_out_n,boot_ct_lost;
static uint8_t boot_ct_eff[256];static int boot_ct_state;   /* 0 unknown, 1 on, -1 off */
static uint64_t boot_wg_eruns[BOOT_THREAD_CAPACITY],boot_wg_runs[BOOT_THREAD_CAPACITY];static volatile LONG64 boot_mt_thread_yields[BOOT_THREAD_CAPACITY];
static int boot_ct_hybrid;
static inline void boot_ct_init(void){
    const char*v=getenv("TOTK_CHAIN_TRACE");boot_ct_state=v && !strcmp(v,"1")?1:-1;
    for(unsigned i=0;i<256;++i)boot_ct_eff[i]=0xff;
    ULONG len=0;GetSystemCpuSetInformation(NULL,0,&len,GetCurrentProcess(),0);
    static uint8_t buf[16384];unsigned lo=255,hi=0;
    if(len && len<=sizeof buf && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)(void*)buf,len,&len,GetCurrentProcess(),0))
        for(ULONG off=0;off<len;){const SYSTEM_CPU_SET_INFORMATION*c=(const SYSTEM_CPU_SET_INFORMATION*)(const void*)(buf+off);
            if(!c->Size)break;
            if(c->Type==CpuSetInformation && c->CpuSet.Group==0){
                boot_ct_eff[c->CpuSet.LogicalProcessorIndex]=c->CpuSet.EfficiencyClass;
                if(c->CpuSet.EfficiencyClass<lo)lo=c->CpuSet.EfficiencyClass;
                if(c->CpuSet.EfficiencyClass>hi)hi=c->CpuSet.EfficiencyClass;}
            off+=c->Size;}
    boot_ct_hybrid=lo<hi;
}
static inline uint8_t boot_ct_cpu(void){PROCESSOR_NUMBER n;GetCurrentProcessorNumberEx(&n);return (uint8_t)(n.Group?255:n.Number);}
static inline void boot_ct_add(uint64_t qpc,unsigned kind,unsigned thread,unsigned other,unsigned state,uint64_t obj,uint8_t cpu){   /* kernel lock held */
    if(boot_ct_state<=0)return;
    if(boot_ct_head-boot_ct_tail>=BOOT_CT_RING){++boot_ct_lost;return;}
    BootCtRec*r=&boot_ct_ring[boot_ct_head++%BOOT_CT_RING];
    r->qpc=qpc;r->obj=obj;r->thread=(uint16_t)thread;r->other=(uint16_t)other;r->kind=(uint8_t)kind;r->state=(uint8_t)state;r->cpu=cpu;r->eff=boot_ct_eff[cpu];
}
/* fence-read trace (TOTK_FENCE_READ_TRACE=1; host_nvn_fence_read.h, play-work/fence-read-trace; observation only): when the
 * frame-fence waiter (TOTK's main thread, t0) hands the next frame to the NVN presenting thread (t2). host_nvn sets `watch`
 * (t2, at its first traced present) and `sig_thread` (t0, at its first frame-fence return); t2 drains both rings right after
 * each of its presents.
 *  - Parks of the watched thread and its first wake after each (boot_wg_block / boot_wg_wake below, both run with the kernel
 *    lock held: one writer at a time, plain stores for the open park): the park's state (4 lock, 5 handles, 6 arbiter, 7
 *    condvar, 8 sleep) and objects (wait_address: the arbiter word or the mutex; key: the condvar key or the first handle; the
 *    kernel clears both at the wake), then the waker (the guest thread whose kernel exit made it runnable, boot_mt_kick_check,
 *    as the wait graph has it; 0xfffe timer), the wait result and the time parked.
 *  - The waiter's signalling SVCs (0x35 SignalToAddress, 0x1d SignalProcessWideKey, 0x1b ArbitrateUnlock, 0x11 SignalEvent) at
 *    boot_mt_svc's entry, before the lock-free shortcuts (a signal for a word nobody is parked on yet never enters the kernel), at most
 *    `armed` per frame (host_nvn re-arms it on that same thread at each frame-fence return: one writer).
 * Off (default): watch and sig_thread stay BOOT_FRT_NONE, never a guest thread index: one compare per hook, nothing stored. */
#define BOOT_FRT_NONE 0xffffffffu
#define BOOT_FRT_KRING 1024u
#define BOOT_FRT_SRING 256u
#define BOOT_FRT_SIG_ARM 64u
typedef struct BootFrtK { uint64_t qpc,obj,key,parked; uint32_t thread,other,result; uint16_t state,kind; } BootFrtK;   /* other: waker ('W'), svc ('S') */
static struct { volatile uint32_t watch,sig_thread; } boot_frt_k __attribute__((aligned(64))) = {BOOT_FRT_NONE,BOOT_FRT_NONE};   /* read by every hook */
static struct { uint32_t open,state;uint64_t qpc,obj,key; volatile uint32_t head,tail;uint32_t lost;BootFrtK ring[BOOT_FRT_KRING]; } boot_frt_kr __attribute__((aligned(64)));
static struct { uint32_t armed; volatile uint32_t head,tail;uint32_t lost;BootFrtK ring[BOOT_FRT_SRING]; } boot_frt_sr __attribute__((aligned(64)));
static inline void boot_frt_k_put(unsigned kind,unsigned i,unsigned other,uint64_t qpc,uint64_t parked,uint32_t result){   /* kernel lock held */
    const uint32_t h=boot_frt_kr.head;
    if(h-__atomic_load_n(&boot_frt_kr.tail,__ATOMIC_ACQUIRE)>=BOOT_FRT_KRING){++boot_frt_kr.lost;return;}
    BootFrtK*e=&boot_frt_kr.ring[h%BOOT_FRT_KRING];
    e->qpc=qpc;e->obj=boot_frt_kr.obj;e->key=boot_frt_kr.key;e->parked=parked;e->thread=i;e->other=other;e->result=result;
    e->state=(uint16_t)boot_frt_kr.state;e->kind=(uint16_t)kind;
    __atomic_store_n(&boot_frt_kr.head,h+1,__ATOMIC_RELEASE);
}
static inline void boot_frt_k_park(const BootContext*M,unsigned i){   /* the watched thread just parked (kernel lock held) */
    if(i>=M->thread_count)return;
    const BootThread*t=&M->threads[i];
    if(t->state<4 || t->state>8)return;
    boot_frt_kr.open=1;boot_frt_kr.state=t->state;boot_frt_kr.qpc=boot_klock_now();
    boot_frt_kr.obj=t->state==5 || t->state==8?0:t->wait_address;boot_frt_kr.key=t->state==5 || t->state==7?t->wait_native[0]:0;
    boot_frt_k_put('B',i,0,boot_frt_kr.qpc,0,0);
}
static inline void boot_frt_k_wake(unsigned i,unsigned waker){   /* runnable again (kernel lock held; called at every kernel exit while it is: the first counts) */
    if(!boot_frt_kr.open)return;
    boot_frt_kr.open=0;
    const BootContext*M=boot_mt.master;const uint64_t now=boot_klock_now();
    boot_frt_k_put('W',i,waker,now,now-boot_frt_kr.qpc,M && i<M->thread_count?(uint32_t)M->threads[i].x[0]:0);
}
static inline void boot_frt_k_svc(const BootContext*c,unsigned thread,unsigned svc){   /* the waiter's own thread, at its SVC entry */
    if(!boot_frt_sr.armed || (svc!=0x35 && svc!=0x1d && svc!=0x1b && svc!=0x11))return;
    --boot_frt_sr.armed;
    const uint32_t h=boot_frt_sr.head;
    if(h-__atomic_load_n(&boot_frt_sr.tail,__ATOMIC_ACQUIRE)>=BOOT_FRT_SRING){++boot_frt_sr.lost;return;}
    BootFrtK*e=&boot_frt_sr.ring[h%BOOT_FRT_SRING];
    e->qpc=boot_klock_now();e->obj=c->x[0];e->key=c->x[1];e->parked=c->x[2];e->thread=thread;e->other=svc;e->result=(uint32_t)c->x[3];e->state=0;e->kind='S';
    __atomic_store_n(&boot_frt_sr.head,h+1,__ATOMIC_RELEASE);
}
/* thread i just parked (its worker went idle with the thread in a wait state) */
static inline void boot_wg_block(const BootContext*M,unsigned i){
    if(i==boot_frt_k.watch)boot_frt_k_park(M,i);   /* fence-read trace (off: never a thread) */
    if(!boot_wg_on() || i>=M->thread_count)return;
    const BootThread*t=&M->threads[i];
    if(t->state==3){boot_wg.run_at[i]=0;boot_wg.block_at[i]=0;return;}   /* exited: no longer running */
    if(t->state<4 || t->state>8)return;
    uint64_t now=boot_wg_now();
    if(boot_wg.run_at[i])boot_wg.ran[i]+=now-boot_wg.run_at[i];
    boot_wg.run_at[i]=0;boot_wg.block_at[i]=now;boot_wg.wake_at[i]=0;boot_wg.waker[i]=BOOT_WG_TIMER;boot_wg.state[i]=t->state;
    boot_wg.obj[i]=t->state==5?t->wait_native[0]:t->state==8?0:t->wait_address;
    if(boot_ct_state>0)boot_ct_add(now,'B',i,0,t->state,boot_wg.obj[i],boot_ct_cpu());   /* chain P0 */
}
/* thread i became runnable; waker = guest thread index, BOOT_WG_TIMER or BOOT_WG_EVENT */
static inline void boot_wg_wake(unsigned i,unsigned waker){
    if(i==boot_frt_k.watch)boot_frt_k_wake(i,waker);   /* fence-read trace (off: never a thread) */
    if(!boot_wg_on() || i>=BOOT_THREAD_CAPACITY || !boot_wg.block_at[i] || boot_wg.wake_at[i])return;
    boot_wg.wake_at[i]=boot_wg_now();boot_wg.waker[i]=waker;
    if(boot_ct_state>0)boot_ct_add(boot_wg.wake_at[i],'W',i,waker,boot_wg.state[i],boot_wg.obj[i],boot_ct_cpu());   /* chain P0 */
}
/* thread i runs again */
static inline void boot_wg_run(unsigned i){
    if(!boot_wg_on() || i>=BOOT_THREAD_CAPACITY)return;
    uint64_t now=boot_wg_now();
    {uint8_t cpu=boot_ct_cpu();++boot_wg_runs[i];if(boot_ct_hybrid && boot_ct_eff[cpu]==0)++boot_wg_eruns[i];   /* chain P0: the worker's CPU at run */
     if(boot_ct_state>0)boot_ct_add(now,'R',i,boot_wg.block_at[i]?boot_wg.waker[i]:0xffffu,boot_wg.state[i],0,cpu);}
    if(boot_wg.block_at[i]){
        uint64_t blocked=now-boot_wg.block_at[i],latency=boot_wg.wake_at[i]?now-boot_wg.wake_at[i]:0;
        boot_wg.blocked[i]+=blocked;
        uint64_t key=((uint64_t)i<<56)^((uint64_t)boot_wg.state[i]<<48)^((uint64_t)boot_wg.waker[i]<<32)^(boot_wg.obj[i]*UINT64_C(0x9e3779b97f4a7c15));
        unsigned h=(unsigned)(key>>40)%BOOT_WG_EDGES,n=0;
        while(n<BOOT_WG_EDGES && boot_wg.edge[h].count && boot_wg.edge[h].key!=key){h=(h+1)%BOOT_WG_EDGES;++n;}
        if(n<BOOT_WG_EDGES){BootWgEdge*e=&boot_wg.edge[h];
            if(!e->count){e->key=key;e->thread=i;e->state=boot_wg.state[i];e->waker=boot_wg.waker[i];e->obj=boot_wg.obj[i];++boot_wg.edges;}
            e->blocked+=blocked;e->latency+=latency;++e->count;}
        boot_wg.block_at[i]=0;boot_wg.wake_at[i]=0;
    }
    boot_wg.run_at[i]=now;
}
/* play (62): the dump runs on the timer thread with the kernel lock held. Its text is built in memory there and
 * appended to host-wait-graph.log after the lock is released (boot_wg_flush): opening, writing and closing the
 * file (hundreds of microseconds, milliseconds with a virus scanner on a log this size) no longer stops every
 * guest SVC once a second. Only the timer thread calls either function. */
static char boot_wg_text[24576];static size_t boot_wg_text_n;
__attribute__((format(printf,1,2))) static inline void boot_wg_cat(const char*fmt,...){
    size_t room=sizeof boot_wg_text-boot_wg_text_n;if(room<2)return;
    __builtin_va_list ap;__builtin_va_start(ap,fmt);int n=vsnprintf(boot_wg_text+boot_wg_text_n,room,fmt,ap);__builtin_va_end(ap);   /* no <stdarg.h>: includes reach the page header */
    if(n>0)boot_wg_text_n+=(size_t)n<room?(size_t)n:room-1;
}
static inline void boot_wg_flush(void){
    if(boot_ct_out_n){   /* chain P0: header once (magic, QPC frequency, efficiency class per CPU), then records */
        static int header;FILE*t=fopen("chain-trace.bin",header?"ab":"wb");
        if(t){if(!header){uint64_t h[2]={UINT64_C(0x3145434152544843),boot_wg.freq};fwrite(h,8,2,t);fwrite(boot_ct_eff,1,256,t);header=1;}
            fwrite(boot_ct_out,sizeof(BootCtRec),boot_ct_out_n,t);fclose(t);}
        boot_ct_out_n=0;
    }
    if(!boot_wg_text_n)return;
    FILE*f=fopen("host-wait-graph.log","a");
    if(f){fwrite(boot_wg_text,1,boot_wg_text_n,f);fclose(f);}
    boot_wg_text_n=0;
}
/* once a second (timer tick, kernel lock held) */
static inline void boot_wg_dump(const BootContext*M){
    if(!boot_wg_on())return;
    uint64_t now=boot_wg_now();if(now-boot_wg.last_dump<boot_wg.freq)return;
    double span=(double)(now-boot_wg.last_dump)/(double)boot_wg.freq;boot_wg.last_dump=now;
    boot_wg_text_n=0;
    {
        static const char*names[9]={"?","?","run","exit","mutex","handles","arbiter","condvar","sleep"};
        SYSTEMTIME st;GetLocalTime(&st);
        boot_wg_cat("%02u:%02u:%02u.%03u span %.2fs presents %u edges %u; top blocking edges (waiter <- waker):\n",st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,span,
                (unsigned)boot_guest_clock.pacer.period_count,boot_wg.edges);
        static uint8_t used[BOOT_WG_EDGES];memset(used,0,sizeof used);
        for(unsigned r=0;r<10;++r){unsigned best=BOOT_WG_EDGES;
            for(unsigned h=0;h<BOOT_WG_EDGES;++h)if(boot_wg.edge[h].count && !used[h] && (best==BOOT_WG_EDGES || boot_wg.edge[h].blocked>boot_wg.edge[best].blocked))best=h;
            if(best==BOOT_WG_EDGES)break;
            BootWgEdge*e=&boot_wg.edge[best];const BootThread*t=e->thread<M->thread_count?&M->threads[e->thread]:NULL;
            char who[32];if(e->waker==BOOT_WG_TIMER)snprintf(who,sizeof who,"timer");else if(e->waker==BOOT_WG_EVENT)snprintf(who,sizeof who,"event");
            else snprintf(who,sizeof who,"t%u h=%x",e->waker,e->waker<M->thread_count?M->threads[e->waker].handle:0);
            boot_wg_cat("  t%-3u h=%-6x prio %-2u %-7s obj %-12llx <- %-14s n %-6llu blocked %8.1f ms (%5.1f%%) avg %7.1f us  wake->run avg %6.1f us\n",
                    e->thread,t?t->handle:0,t?t->priority:0,e->state<9?names[e->state]:"?",(unsigned long long)e->obj,who,(unsigned long long)e->count,
                    (double)e->blocked*1e3/(double)boot_wg.freq,(double)e->blocked*100.0/((double)boot_wg.freq*span),
                    (double)e->blocked*1e6/(double)boot_wg.freq/(double)e->count,(double)e->latency*1e6/(double)boot_wg.freq/(double)e->count);
            used[best]=1;   /* printed: drop from this ranking (the table is cleared below) */
        }
        /* The threads that ran the most (the frame's critical path): each one's own top 3 blocking edges. */
        unsigned shown[6];unsigned nshown=0;
        for(unsigned r=0;r<6;++r){unsigned best=BOOT_THREAD_CAPACITY;uint64_t bran=0;
            for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY;++i){int dup=0;for(unsigned s2=0;s2<nshown;++s2)dup|=shown[s2]==i;
                uint64_t ran=boot_wg.ran[i]+(boot_wg.run_at[i]?now-boot_wg.run_at[i]:0);
                if(!dup && ran>bran){bran=ran;best=i;}}
            if(best==BOOT_THREAD_CAPACITY)break;shown[nshown++]=best;
            boot_wg_cat("  busy t%u h=%x prio %u ran %.0f ms, blocked %.0f ms:",best,M->threads[best].handle,M->threads[best].priority,
                    (double)bran*1e3/(double)boot_wg.freq,(double)boot_wg.blocked[best]*1e3/(double)boot_wg.freq);
            for(unsigned q=0;q<3;++q){unsigned be=BOOT_WG_EDGES;
                for(unsigned h=0;h<BOOT_WG_EDGES;++h)if(boot_wg.edge[h].count && boot_wg.edge[h].thread==best && (be==BOOT_WG_EDGES || boot_wg.edge[h].blocked>boot_wg.edge[be].blocked))be=h;
                if(be==BOOT_WG_EDGES)break;BootWgEdge*e=&boot_wg.edge[be];
                boot_wg_cat(" [%s %llx <- %s%u n %llu %.1f ms]",e->state<9?names[e->state]:"?",(unsigned long long)e->obj,
                        e->waker==BOOT_WG_TIMER?"timer":e->waker==BOOT_WG_EVENT?"event":"t",e->waker<BOOT_THREAD_CAPACITY?e->waker:0u,
                        (unsigned long long)e->count,(double)e->blocked*1e3/(double)boot_wg.freq);
                e->count=0;}
            boot_wg_cat("\n");
        }
        boot_wg_cat("  idle wakes: spinning %lld, parked %lld (spin %u..%u)\n",(long long)_InterlockedExchange64(&boot_mt_spin_wakes,0),
                (long long)_InterlockedExchange64(&boot_mt_park_wakes,0),boot_mt_spin_floor(),boot_mt_spin_limit());
        {   /* play (52) */
            unsigned idx[0x84];LONG64 n[0x84],wt[0x84],hd[0x84];unsigned m=0;
            for(unsigned i=0;i<0x84;++i){n[i]=_InterlockedExchange64(&boot_klock_n[i],0);wt[i]=_InterlockedExchange64(&boot_klock_wait[i],0);hd[i]=_InterlockedExchange64(&boot_klock_hold[i],0);if(n[i])idx[m++]=i;}
            for(unsigned a=1;a<m;++a){unsigned v=idx[a],b=a;while(b && wt[idx[b-1]]<wt[v]){idx[b]=idx[b-1];--b;}idx[b]=v;}
            boot_wg_cat("  yields without the kernel: %lld, type-0 signals without the kernel: %lld\n",(long long)_InterlockedExchange64(&boot_mt_fast_yields,0),
                (long long)_InterlockedExchange64(&boot_mt_fast_signals,0));
        boot_wg_cat("  kernel lock by svc (n / wait ms / hold ms):");
            for(unsigned a=0;a<m && a<12;++a){unsigned i=idx[a];
                if(i==0x83)boot_wg_cat(" timer");else if(i==0x82)boot_wg_cat(" service");   /* chain P0 */
                else if(i==0x81)boot_wg_cat(" slow-path");else if(i==0x80)boot_wg_cat(" svc>=0x80");else boot_wg_cat(" %#x",i);
                boot_wg_cat(" %lld/%.1f/%.1f",(long long)n[i],(double)wt[i]*1e3/(double)boot_wg.freq,(double)hd[i]*1e3/(double)boot_wg.freq);}
            boot_wg_cat("\n");
        }
        boot_wg_cat("  renderer lock waits (ms):");
        for(unsigned w=0;w<=BOOT_MT_MAX && w<=boot_mt.workers;++w){LONG64 q=_InterlockedExchange64(&boot_render_wait[w<boot_mt.workers?w:BOOT_MT_MAX],0);
            if(q)boot_wg_cat(" t%u %.0f",w<boot_mt.workers?boot_mt.bound[w]:9999u,(double)q*1e3/(double)boot_wg.freq);}
        boot_wg_cat("\n");
        {   /* chain P0 */
            boot_wg_cat("  E-core runs / runs (hybrid %d):",boot_ct_hybrid);
            for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY;++i)if(boot_wg_runs[i])boot_wg_cat(" t%u %llu/%llu",i,(unsigned long long)boot_wg_eruns[i],(unsigned long long)boot_wg_runs[i]);
            boot_wg_cat("\n  yields by thread:");
            for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY;++i){LONG64 y=_InterlockedExchange64(&boot_mt_thread_yields[i],0);if(y)boot_wg_cat(" t%u %lld",i,(long long)y);}
            boot_wg_cat("\n");memset(boot_wg_eruns,0,sizeof boot_wg_eruns);memset(boot_wg_runs,0,sizeof boot_wg_runs);
            if(boot_ct_state>0){uint32_t n=boot_ct_head-boot_ct_tail;
                for(uint32_t q=0;q<n;++q)boot_ct_out[q]=boot_ct_ring[(boot_ct_tail+q)%BOOT_CT_RING];
                boot_ct_out_n=n;boot_ct_tail=boot_ct_head;
                if(boot_ct_lost){boot_wg_cat("  chain trace: %u records lost (ring full)\n",boot_ct_lost);boot_ct_lost=0;}}
        }
        boot_wg_cat("  threads (ran / blocked ms):");
        for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY;++i){
            uint64_t ran=boot_wg.ran[i]+(boot_wg.run_at[i]?now-boot_wg.run_at[i]:0);
            if(ran || boot_wg.blocked[i])boot_wg_cat(" t%u %.0f/%.0f",i,(double)ran*1e3/(double)boot_wg.freq,(double)boot_wg.blocked[i]*1e3/(double)boot_wg.freq);
        }
        boot_wg_cat("\n");
    }
    memset(boot_wg.edge,0,sizeof(boot_wg.edge));boot_wg.edges=0;
    memset(boot_wg.ran,0,sizeof(boot_wg.ran));memset(boot_wg.blocked,0,sizeof(boot_wg.blocked));
    for(unsigned i=0;i<BOOT_THREAD_CAPACITY;++i)if(boot_wg.run_at[i])boot_wg.run_at[i]=now;
}
/* Enter/leave the kernel on behalf of worker k (k's thread becomes the master's current thread). */
static inline void boot_mt_enter(BootCore*k){
    __atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);
    boot_mt_klock_acquire();   /* chain P7 */
    BootContext*M=boot_mt.master;
    boot_mt.kernel_core=k->index;
#if TOTK_PLAY
    /* play (62): guest time and scheduling order come from the host clock (guest_time.h) and translated code counts no
     * instructions, so M->steps is only printed by diagnostics. The 1 ms timer tick (boot_mt_timer_tick) refreshes it;
     * the sum over every worker no longer runs at each of the thousands of kernel entries per second, under the lock. */
    if(!boot_mt.host_threads)
#endif
    M->steps=boot_mt_total_steps();
    if(k->thread<BOOT_THREAD_CAPACITY){boot_mt_copy_cpu(M,&k->ctx);M->current_thread=k->thread;
        if(boot_mt.host_threads && (k->thread!=boot_mt.bound[k->index] || k->ctx.tpidrro!=M->threads[k->thread].tpidrro))boot_mt_identity_log(k,"enter",k->thread);}
    else M->current_thread=BOOT_MT_IDLE;
    M->status=0;M->fault_pc=M->fault_address=M->fault_width=0;
}
/* TOTK_THREADS=host: a host worker for guest thread `thread` (kernel lock held, or before the workers
 * start). Created when the thread is first runnable; it ends when the thread has exited (state 3). The
 * guest priority maps onto three host priority bands (lower guest value = more urgent). */
static DWORD WINAPI boot_mt_worker(void*arg);
static inline int boot_mt_host_priority(uint32_t p){return p<=16?THREAD_PRIORITY_ABOVE_NORMAL:p<48?THREAD_PRIORITY_NORMAL:THREAD_PRIORITY_BELOW_NORMAL;}
static inline int boot_mt_spawn(unsigned thread){
    BootContext*M=boot_mt.master;unsigned w=boot_mt.workers;
    if(!M || w>=BOOT_MT_MAX || thread>=M->thread_count)return 0;
    BootCore*k=boot_mt.core[w];
    if(!k){k=_aligned_malloc(sizeof(BootCore),64);if(!k)return 0;boot_mt.core[w]=k;}
    memset(k,0,sizeof(*k));memcpy(&k->ctx,M,sizeof(BootContext));
    /* A core context never schedules by itself: translated WFE/YIELD hints (translate_startup.py) pick another
     * runnable thread from c->threads and load it when c->current_thread<c->thread_count. On a core context that
     * table is the snapshot taken at worker start, so the hint loaded another thread's stale CPU state (its TLS,
     * registers, pc) into this worker: two host threads ran the same guest thread. current_thread stays
     * BOOT_MT_IDLE in core contexts (the kernel keeps the real one in the master), so the hint only pauses. */
    k->ctx.tlb_epoch=BOOT_TLB_CORE;k->ctx.status=0;k->ctx.steps=0;k->ctx.limit=0;k->ctx.trace_count=0;k->ctx.current_thread=BOOT_MT_IDLE;
    k->index=w;k->vcore=M->threads[thread].core;k->thread=BOOT_MT_IDLE;k->quota_end=0;
    k->doorbell=CreateEventW(NULL,FALSE,FALSE,NULL);
    boot_mt.vcore[w]=k->vcore;boot_mt.bound[w]=thread;boot_mt.worker_of[thread]=w+1;
    k->host=CreateThread(NULL,8u<<20,boot_mt_worker,k,CREATE_SUSPENDED,NULL);
    if(!k->host){boot_mt.worker_of[thread]=0;CloseHandle(k->doorbell);k->doorbell=NULL;return 0;}
    SetThreadPriority(k->host,boot_mt_host_priority(M->threads[thread].priority));
    _InterlockedIncrement(&boot_mt.alive);
    __atomic_store_n(&boot_mt.workers,w+1,__ATOMIC_SEQ_CST);
    ResumeThread(k->host);
    return 1;
}
/* Who should run where: interrupt a worker whose guest core has a better runnable thread than the
 * one it runs, or that idles while a thread for it (or a migratable one) is runnable. */
static inline void boot_mt_kick_check(unsigned self){
    BootContext*M=boot_mt.master;
    if(boot_mt.host_threads){   /* wake the worker of every runnable thread not running; spawn new ones */
        for(unsigned i=0;i<M->thread_count;++i){
            if(M->threads[i].state!=2 || boot_mt.running[i])continue;
            unsigned w=boot_mt.worker_of[i];
            if(!w){boot_mt_spawn(i);continue;}
            boot_wg_wake(i,self<boot_mt.workers?boot_mt.bound[self]:BOOT_WG_TIMER);
            if(w-1!=self && boot_mt.core[w-1]->thread>=BOOT_THREAD_CAPACITY){
                if(boot_mt_defer_kicks())boot_mt_kick_deferred(w-1,BOOT_MT_INT_RESCHED);   /* chain P1: rung after release */
                else boot_mt_kick(w-1,BOOT_MT_INT_RESCHED);
            }
        }
        return;
    }
    for(unsigned w=0;w<boot_mt.workers;++w){
        if(w==self)continue;
        BootCore*k=boot_mt.core[w];
        unsigned current=k->thread;uint32_t limit=current<M->thread_count?M->threads[current].priority:64u;
        int idle=current>=BOOT_THREAD_CAPACITY,want=0;
        for(unsigned i=0;i<M->thread_count && !want;++i){const BootThread*t=&M->threads[i];
            if(t->state!=2 || boot_mt.running[i] || t->priority>=limit)continue;
            unsigned active=t->schedule.affinity?t->schedule.active:t->core;
            if(active==k->vcore)want=1;
            else if(idle && t->priority>=2 && (t->core_mask&(UINT64_C(1)<<k->vcore)))want=1;
        }
        if(want)boot_mt_kick(w,BOOT_MT_INT_RESCHED);
    }
}
static inline void boot_mt_leave(BootCore*k){
    BootContext*M=boot_mt.master;
    unsigned previous=k->thread;
    /* Host threads: a worker only ever runs its own guest thread. A handler that switched the kernel to
     * another thread (its saved record was loaded unchanged) is logged and undone: the worker idles, the
     * other thread stays with its own worker, and this worker's thread (saved by the handler) runs again
     * from its record when it is runnable. */
    if(boot_mt.host_threads && M->current_thread<M->thread_count && M->current_thread!=boot_mt.bound[k->index]){
        boot_mt_identity_log(k,"foreign switch",M->current_thread);M->current_thread=BOOT_MT_IDLE;
    }
    if(M->current_thread<M->thread_count){boot_mt_copy_cpu(&k->ctx,M);k->thread=M->current_thread;}
    else k->thread=BOOT_MT_IDLE;
    if(previous<BOOT_THREAD_CAPACITY && k->thread>=BOOT_THREAD_CAPACITY)boot_wg_block(M,previous);
    if(previous<BOOT_THREAD_CAPACITY && boot_mt.running[previous]==k->index+1)boot_mt.running[previous]=0;
    if(k->thread<BOOT_THREAD_CAPACITY)boot_mt.running[k->thread]=k->index+1;
    /* overlap-work P2: with host threads every RESCHED bit is set under the kernel lock, which this core holds: by
     * kick_check, only to a worker with no thread loaded (its bound thread runnable, not running), or by
     * quiesce_begin, whose holder ends the quiesce before it releases the lock. Once this worker has its bound thread
     * loaded, every RESCHED bit already set is satisfied; a later request is posted after this core releases the
     * lock, so after the clear, and is kept. STOP is never cleared. Without this, a worker kicked again while it woke
     * (a barrier releasing several threads: several kernel exits in a row) went into guest code with limit 0, stopped
     * at once in boot_play_stop and ran a redundant service. */
    if(boot_mt.host_threads && boot_mt_overlap_on() && k->thread<BOOT_THREAD_CAPACITY && k->thread==boot_mt.bound[k->index]){
        LONG was=_InterlockedAnd(&k->interrupt,~(LONG)BOOT_MT_INT_RESCHED);
        if(was&BOOT_MT_INT_RESCHED){
            _InterlockedIncrement64(&boot_mt_stale_resched);
            if(!(was&~(LONG)BOOT_MT_INT_RESCHED)){   /* nothing else pending: run token back; Dekker with boot_mt_kick (OR, then limit 0) */
                __atomic_store_n(&k->ctx.limit,k->quota_end,__ATOMIC_SEQ_CST);
                if(__atomic_load_n(&k->interrupt,__ATOMIC_SEQ_CST))__atomic_store_n(&k->ctx.limit,0,__ATOMIC_SEQ_CST);
            }
        }
    }
    if(M->status && !k->ctx.status){k->ctx.status=M->status;k->ctx.fault_pc=M->fault_pc;k->ctx.fault_address=M->fault_address;k->ctx.fault_width=M->fault_width;}
    M->status=0;M->fault_pc=M->fault_address=M->fault_width=0;
    boot_mt_kick_check(k->index);
    boot_mt.kernel_core=UINT32_MAX;
    boot_mt_klock_release();   /* chain P1: doorbells after the release */
    boot_mt_guest_enter(k);
}
/* SVC from a core: the single-core handler runs on the master under the kernel lock. */
/* Renderer lock (TOTK_THREADS=host, TOTK_CORES>1). The D3D12 renderer (BootRender, BootGpu, command lists,
 * allocators, descriptor heaps, PSO and texture caches, swap chain) was written for one guest-executing host
 * thread. With guest threads on several host threads its entries can run concurrently: native NVN host
 * functions (any guest thread), the kernel's IPC paths (nvdrv submit, vi/binder present), the physical memory
 * unmap path (boot_render_forget) and the present-hold sweep from guest clock reads. All of them now hold this
 * one recursive lock. Order: renderer lock, then kernel lock, then window lock. Inactive (no cost) while a
 * single host thread runs guest code. */
static inline CRITICAL_SECTION*boot_render_cs(void){
    static CRITICAL_SECTION cs;static INIT_ONCE once=INIT_ONCE_STATIC_INIT;BOOL pending;
    if(InitOnceBeginInitialize(&once,0,&pending,NULL) && pending){InitializeCriticalSection(&cs);InitOnceComplete(&once,0,NULL);}
    return &cs;
}
static inline int boot_render_lock(void){
    if(!boot_mt.active)return 0;
    CRITICAL_SECTION*cs=boot_render_cs();
    if(TryEnterCriticalSection(cs))return 1;
    LARGE_INTEGER a,b;QueryPerformanceCounter(&a);EnterCriticalSection(cs);QueryPerformanceCounter(&b);
    int w=boot_mt_worker_of_thread(GetCurrentThreadId());
    _InterlockedExchangeAdd64(&boot_render_wait[w>=0 && w<BOOT_MT_MAX?w:BOOT_MT_MAX],b.QuadPart-a.QuadPart);
#ifdef TOTK_DRAW_PATH
    boot_nw_span(BOOT_NW_RENDER_LOCK,a.QuadPart,b.QuadPart);   /*80: reuse original clocks */
#endif
    return 1;
}
static inline void boot_render_unlock(int held){if(held)LeaveCriticalSection(boot_render_cs());}
/* Callers that may hold the kernel lock (guest clock updates from the timer tick) only try: -1 = busy, skip. */
static inline int boot_render_trylock(void){if(!boot_mt.active)return 0;return TryEnterCriticalSection(boot_render_cs())?1:-1;}
/* Kernel entries that reach the renderer: SendSyncRequest (nvdrv, vi/binder present, GPU packing), Map/Unmap
 * PhysicalMemory (boot_render_forget), kernel-mode host functions (BOOT_SVC_HOSTCALL). */
/* IPC (svc 0x21) takes the renderer lock only for sessions of the graphics services: nvdrv (FSP+4), vi:u and
 * its display, relay/binder (present) and system-display objects (FSP+5..7, FSP+9, FSP+10), and the am display
 * and window controllers. File system, audio, HID, time, account, applet and the rest run concurrently with the
 * renderer. A domain session counts as graphics if any of its objects is. Unknown handles (service manager
 * lookups, closed sessions) keep the lock. CloseHandle of a graphics session takes it too (display release). The handle tables are read without the kernel lock: a session's
 * type does not change while its handle is open. */
static inline int boot_mt_render_session_type(uint32_t t){
    return (t>=BOOT_SESSION_FSP+4 && t<=BOOT_SESSION_FSP+7) || t==BOOT_SESSION_FSP+9 || t==BOOT_SESSION_FSP+10 ||
           t==BOOT_SESSION_DISPLAY_CONTROL || t==BOOT_SESSION_WINDOW_CONTROL;
}
static inline int boot_mt_ipc_renders(uint32_t handle,int unknown){
    const BootContext*M=boot_mt.master;if(!M)return unknown;
    for(unsigned i=0;i<BOOT_SESSION_CAPACITY;++i)if(M->session_handles[i]==handle && handle){
        if(boot_mt_render_session_type(M->session_types[i]))return 1;
        uint32_t d=M->session_domains[i];
        if(d && d<=16)for(unsigned k=0;k<16;++k){uint32_t v=M->domain_types[d-1][k];if(v && boot_mt_render_session_type(v-1))return 1;}   /* stored as type+1 */
        return 0;
    }
    return unknown;
}
static inline int boot_mt_svc_renders(unsigned svc,const BootContext*c){
    if(svc==0x21)return boot_mt_ipc_renders((uint32_t)c->x[0],1);
    if(svc==0x16)return boot_mt_ipc_renders((uint32_t)c->x[0],0);   /* closing a display/driver session releases renderer state */
    return svc==0x2c || svc==0x2d || svc>=0x100;
}
/* logic-batch L2 (TOTK_ARB_FAST): SVC outcomes that need no kernel section. Each one is the result the kernel path
 * (boot_svc_arbiter, boot_mutex, boot_condition_variable on the master) computes for the same inputs; any other case,
 * an argument the kernel would reject, a word not in the attached page table, or a busy kernel takes the kernel path.
 *  - 0x34 WaitForAddress whose comparison fails: the kernel reads the word once and returns 0xfa01 (type 1 decrements
 *    only when the test passes). The word is read once here (through the fixed view, like a guest load), after a full
 *    fence (the kernel lock acquire was one): the call linearizes at that read; it queues nothing, so no wakeup can be lost.
 *  - 0x35 SignalToAddress types 1/2 with no waiter at the address: the kernel's compare-exchange value -> value+1 (both
 *    types with zero waiters) and x0 0 / 0xfa01. Done under the SHARED kernel lock, as the type-0 path below: a waiter
 *    reads the word and parks (state 6) inside one exclusive section, so it either is seen queued here (kernel path) or
 *    reads the word after this compare-exchange, exactly as if the kernel had run this signal first.
 *  - 0x1a ArbitrateLock whose word is not owner|0x40000000: x0 0, the caller retries (read as for 0x34).
 *  - 0x1b ArbitrateUnlock with no thread waiting on the address for this owner: the word becomes 0, x0 0 (shared lock).
 *  - 0x1d SignalProcessWideKey with no thread waiting on the key: the key word becomes 0 after a release fence and
 *    this thread's exclusive monitor on it is cleared; x0 is left as it was (shared lock). 0x1c always blocks.
 * Off while the batch-a frame trace runs (it records every 0x1a/0x34 wait, also the ones that do not block).
 * Returns 2 (handled: continue at *target) or 0 (kernel path). */
#if TOTK_FASTMEM && TOTK_FM_FIXED
#define BOOT_LB_WORD(w,a) ((void)(w),(const uint8_t*)(BOOT_FM_VIEW+(a)))   /* lock-free reads through the fixed view, as a guest LDR (VEH-covered) */
#else
#define BOOT_LB_WORD(w,a) ((void)(a),(w))
#endif
static inline int boot_mt_arb_fast(BootCore*k,BootContext*c,unsigned svc,uint64_t next,uint64_t*target){
#if TOTK_FASTMEM
    BootContext*M=boot_mt.master;const unsigned self=k->thread;
    if(!M || self>=BOOT_THREAD_CAPACITY || c->status || !boot_mt_arb_fast_on() || !__atomic_load_n(&boot_fm.table_ctx,__ATOMIC_ACQUIRE))return 0;
    if(boot_mt_ft.on && (svc==0x34 || svc==0x1a))return 0;
    if(self>=__atomic_load_n(&M->thread_count,__ATOMIC_ACQUIRE))return 0;
    const uint32_t handle=M->threads[self].handle;
    if(svc==0x34){
        const uint64_t a=c->x[0];const uint32_t type=(uint32_t)c->x[1],value=(uint32_t)c->x[2];
        if((a&3) || type>2)return 0;
        const uint8_t*w=boot_fm_table_ptr(c,a,4,type==1?READ|WRITE:READ);if(!w)return 0;   /* the kernel's permission check */
        atomic_thread_fence(memory_order_seq_cst);
        const uint32_t old=boot_kernel_load32(BOOT_LB_WORD(w,a));
        if(type==2?old==value:(int32_t)old<(int32_t)value)return 0;   /* the test passes: the kernel waits (or times out) */
        c->x[0]=UINT32_C(0xfa01);*target=next;boot_lb_add(k->index,BOOT_LB_W+3+type);return 2;
    }
    if(svc==0x1a){
        const uint64_t owner=c->x[0],a=c->x[1];
        if((a&3) || c->x[2]!=handle || owner==handle)return 0;
        const uint8_t*w=boot_fm_table_ptr(c,a,4,READ|WRITE);if(!w)return 0;
        atomic_thread_fence(memory_order_seq_cst);
        if((uint64_t)boot_kernel_load32(BOOT_LB_WORD(w,a))==(owner|UINT64_C(0x40000000)))return 0;   /* still owned with waiters: the kernel queues us */
        c->x[0]=0;*target=next;boot_lb_add(k->index,BOOT_LB_LOCK_F);return 2;
    }
    if(svc!=0x35 && svc!=0x1b && svc!=0x1d)return 0;
    const uint64_t a=c->x[0];const uint32_t type=(uint32_t)c->x[1],value=(uint32_t)c->x[2];
    if((a&3) || (svc==0x35 && (type<1 || type>2)))return 0;   /* type 0: the play (61) path in boot_mt_svc */
    if(!TryAcquireSRWLockShared(&boot_mt.klock)){if(svc==0x35)boot_lb_add(k->index,BOOT_LB_S_BUSY);return 0;}
    /* the page table only changes in exclusive kernel sections: the pointer stays valid while the shared lock is held */
    uint8_t*w=__atomic_load_n(&boot_fm.table_ctx,__ATOMIC_ACQUIRE)?boot_fm_table_ptr(c,a,4,READ|WRITE):NULL;
    if(!w){ReleaseSRWLockShared(&boot_mt.klock);return 0;}
    unsigned n=__atomic_load_n(&M->thread_count,__ATOMIC_ACQUIRE);if(n>BOOT_THREAD_CAPACITY)n=BOOT_THREAD_CAPACITY;   /* under the lock */
    int queued=0,ok=1;
    for(unsigned j=0;j<n && !queued;++j){const BootThread*t=&M->threads[j];const uint32_t st=__atomic_load_n(&t->state,__ATOMIC_ACQUIRE);
        queued=svc==0x35?st==6 && t->wait_address==a:svc==0x1b?st==4 && t->wait_address==a && t->wait_owner==handle:st==7 && t->wait_native[0]==a;}
    if(!queued){
        if(svc==0x35)ok=boot_kernel_cas32(w,value,value+1);
        else if(svc==0x1b)boot_kernel_store32(w,0);
        else {atomic_thread_fence(memory_order_release);boot_invalidate_exclusive(c,a,4);boot_kernel_store32(w,0);}
    }
    ReleaseSRWLockShared(&boot_mt.klock);
    if(queued){if(svc==0x35)boot_lb_add(k->index,BOOT_LB_S_QUEUED);return 0;}
    *target=next;
    if(svc==0x35){c->x[0]=ok?0:UINT32_C(0xfa01);boot_lb_add(k->index,BOOT_LB_S+3+type);if(!ok)boot_lb_add(k->index,BOOT_LB_S+6+type);}
    else if(svc==0x1b){c->x[0]=0;boot_lb_add(k->index,BOOT_LB_UNLOCK_F);}
    else boot_lb_add(k->index,BOOT_LB_CVSIG_F);
    return 2;
#else
    (void)k;(void)c;(void)svc;(void)next;(void)target;return 0;
#endif
}
static inline __attribute__((noinline)) int boot_mt_svc(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){
    BootCore*k=(BootCore*)c;
    if(k->thread==boot_frt_k.sig_thread)boot_frt_k_svc(c,k->thread,svc);   /* fence-read trace: the frame waiter's signals (off: never a thread) */
    if(svc==0xb && boot_mt.host_threads && (int64_t)c->x[0]<=0 && (int64_t)c->x[0]>=-2){   /* play (60): yield without the kernel */
        static int kernel=-1;if(kernel<0){const char*v=getenv("TOTK_YIELD_KERNEL");kernel=v && !strcmp(v,"1");}
        if(!kernel){if(boot_wg_on()){_InterlockedIncrement64(&boot_mt_fast_yields);if(k->thread<BOOT_THREAD_CAPACITY)_InterlockedIncrement64(&boot_mt_thread_yields[k->thread]);}   /* chain P0 */
            SwitchToThread();*target=next;return 2;}
    }
    if(svc==0x35 && boot_mt.host_threads && (uint32_t)c->x[1]==0 && !(c->x[0]&3)){   /* play (61): type-0 signal, nobody queued */
        static int kernel=-1;if(kernel<0){const char*v=getenv("TOTK_ARBITER_KERNEL");kernel=v && !strcmp(v,"1");}
        BootContext*M=boot_mt.master;int queued=0,held=0,busy=0;
        if(!kernel && M && boot_mt_fast_signal_lock_on()){held=TryAcquireSRWLockShared(&boot_mt.klock);if(!held)queued=busy=1;}   /* batch-a: kernel busy: it decides */
        /* logic-batch: the thread count is read after the shared lock is taken (a thread created, run and parked on this
         * address between an earlier read and the lock was outside the scan) */
        unsigned n=M && !queued?__atomic_load_n(&M->thread_count,__ATOMIC_ACQUIRE):0;
        if(n>BOOT_THREAD_CAPACITY)n=BOOT_THREAD_CAPACITY;
        for(unsigned j=0;M && j<n && !queued;++j)queued=__atomic_load_n(&M->threads[j].state,__ATOMIC_ACQUIRE)==6 && M->threads[j].wait_address==c->x[0];
        if(held)ReleaseSRWLockShared(&boot_mt.klock);
        if(!kernel && M && !queued){if(boot_wg_on())_InterlockedIncrement64(&boot_mt_fast_signals);c->x[0]=0;*target=next;boot_lb_add(k->index,BOOT_LB_S+3);return 2;}
        if(!kernel && M)boot_lb_add(k->index,busy?BOOT_LB_S_BUSY:BOOT_LB_S_QUEUED);
    }
    if(boot_mt.host_threads && (svc==0x34 || svc==0x35 || svc==0x1a || svc==0x1b || svc==0x1d)){int f=boot_mt_arb_fast(k,c,svc,next,target);if(f)return f;}   /* logic-batch L2 */
    if(svc==0x34 || svc==0x35){const uint32_t t=(uint32_t)c->x[1];if(t<=2)boot_lb_add(k->index,(svc==0x34?BOOT_LB_W:BOOT_LB_S)+t);}   /* kernel path */
    else if(svc>=0x1a && svc<=0x1d)boot_lb_add(k->index,svc==0x1a?BOOT_LB_LOCK_K:svc==0x1b?BOOT_LB_UNLOCK_K:svc==0x1c?BOOT_LB_CVWAIT_K:BOOT_LB_CVSIG_K);
    int render=boot_mt_svc_renders(svc,c)?boot_render_lock():0;
    const int kstat=boot_wg_on();   /* play (62): statistics only for the wait-graph dump */
    uint64_t kq0=kstat?boot_klock_now():0;   /* play (52) */
    boot_mt_enter(k);++boot_mt.svcs;
    uint64_t kq1=kstat?boot_klock_now():0;unsigned ks=svc<0x80?svc:0x80;
    const unsigned ft=boot_mt_ft.on && boot_mt.host_threads && boot_mt_ft_svc(svc)?k->thread:BOOT_THREAD_CAPACITY;   /* batch-a frame trace */
    if(ft<BOOT_THREAD_CAPACITY)boot_mt_ft_begin(ft,svc,&k->ctx);
    int r=boot_svc_kernel(boot_mt.master,pc,next,svc,target);
    if(ft<BOOT_THREAD_CAPACITY && boot_mt.master->current_thread==ft)boot_mt_ft_end(ft,(uint32_t)boot_mt.master->x[0],0);   /* it did not block */
#if TOTK_FASTMEM && TOTK_FM_FIXED
    if(boot_fm.pending)boot_fm_sync(boot_fm.pending);   /* under the kernel lock */
#endif
    if(kstat)boot_klock_add(ks,kq1-kq0,boot_klock_now()-kq1);   /* kernel lock still held */
    boot_mt_leave(k);
    boot_render_unlock(render);
    if(k->thread>=BOOT_THREAD_CAPACITY){*target=BOOT_MT_IDLE_PC;if(r>0)r=2;}
    return r;
}
static inline __attribute__((noinline)) int boot_mt_system_tick(BootContext*c,uint64_t pc,uint64_t*out){
    BootCore*k=(BootCore*)c;
    if(boot_mt.host_threads){
        /* TOTK_THREADS=host: CNTPCT reads do not enter the kernel. A guest thread polling the tick in a loop
         * (nn::os timer events, spin waits: millions of reads per second) took the kernel lock on every read;
         * SRW locks are not fair, so the timer thread and the workers woken to run the thread it waits for
         * starved on the lock and the game hung. The guest clock has its own lock; its update runs at most
         * every ~50 us here, other reads return the last published tick (monotonic). */
        BootContext*M=boot_mt.master;
        if(M->clock_mode==2){*out=M->clock_test_ticks;return 1;}
        if(M->clock_mode!=1){boot_fault(c,BOOT_SYSREG,pc,0,0);return 0;}
        static volatile LONG64 boot_mt_tick_last;static LONG64 boot_mt_tick_gap;
        if(!boot_mt_tick_gap){LARGE_INTEGER f;QueryPerformanceFrequency(&f);boot_mt_tick_gap=f.QuadPart/20000?f.QuadPart/20000:1;}
        LARGE_INTEGER q;QueryPerformanceCounter(&q);
        LONG64 last=__atomic_load_n(&boot_mt_tick_last,__ATOMIC_RELAXED);uint64_t published=boot_guest_clock.published;
        if(published && q.QuadPart-last<boot_mt_tick_gap){*out=published;return 1;}
        __atomic_store_n(&boot_mt_tick_last,q.QuadPart,__ATOMIC_RELAXED);
        if(!boot_guest_clock_now(M,out)){boot_fault(c,BOOT_SYSREG,pc,0,0);return 0;}
        return 1;
    }
    boot_mt_enter(k);
    int r=boot_system_tick(boot_mt.master,pc,out);
    boot_mt_leave(k);
    return r;
}
/* Slow path of a core context (fastmem fast paths missed: tables detached for a sync, a fault, a
 * straddling access). Under the kernel lock (the single map lock): re-attach the master if a sync is
 * pending, retry the page table, else run the exact lookup on the master and move its fault (or
 * bounce buffer) to the core. */
#if TOTK_FASTMEM
/* logic-batch L1 (TOTK_MT_ACCESS_FAST): why the slow path ran, and the lock-free answer for a page-crossing access.
 * boot_access_fm leaves the page table for an access that crosses a 4 KiB page (32-byte stp q,q / multi-register
 * st1 through boot_store_bytes_legacy: open world, main+0x29ce000) or whose page state lacks the permission. A
 * crossing read or write (p = READ, WRITE or both, w <= 4096: two pages) whose two pages both have the permission in
 * the attached table (no sync pending) gets, with no kernel lock:
 *  - equal addends (one region, or host-contiguous regions): a + addend, the region data pointer the exact lookup
 *    returns for one region, read the same way boot_fm_table_ptr reads one page (state, then addend);
 *  - different addends (the access spans two regions, TOTK_FM_FIXED): the fixed guest view at a. Both pages are mapped
 *    there with host protections that follow the same table, so this is every byte of the access, as the exact path's
 *    CH_ADDRESS fault plus byte-wise boot_store_split / gathered read would deliver it after its per-byte checks
 *    passed. A page unmapped concurrently faults into the view's VEH like any direct access.
 * Anything else takes the kernel-locked exact path unchanged. Reasons are counted per worker (boot_lb_report). */
static inline uint8_t*boot_mt_access_cross(BootCore*k,uint64_t a,unsigned w,unsigned p){
    unsigned reason;uint8_t*h=NULL;
    if(!w || w>4096 || !p || (p&~(unsigned)(READ|WRITE)) || (a>>BOOT_FM_SPACE_BITS) || !boot_fm.leaves)reason=BOOT_LB_AS_OTHER;
    else if(!__atomic_load_n(&boot_fm.table_ctx,__ATOMIC_ACQUIRE) || __atomic_load_n(&boot_fm.pending,__ATOMIC_ACQUIRE))reason=BOOT_LB_AS_DETACHED;
    else if((a&4095)+(uint64_t)w<=4096)reason=BOOT_LB_AS_INPAGE;
    else if(!boot_mt_access_fast_on())reason=BOOT_LB_AS_OFF;
    else if(((a|4095)+1)>>BOOT_FM_SPACE_BITS)reason=BOOT_LB_AS_OTHER;   /* second page outside the space */
    else {
        const uint64_t b=(a|4095)+1;
        const BootFmLeaf*l0=boot_fm.leaves[(a>>21)&(BOOT_FM_LEAVES-1)],*l1=boot_fm.leaves[(b>>21)&(BOOT_FM_LEAVES-1)];
        const unsigned i0=(unsigned)(a>>12)&511,i1=(unsigned)(b>>12)&511;
        const uint8_t s0=l0?__atomic_load_n(&l0->state[i0],__ATOMIC_ACQUIRE):0,s1=l1?__atomic_load_n(&l1->state[i1],__ATOMIC_ACQUIRE):0;
        reason=BOOT_LB_AS_CROSS;
        if((s0&p)==p && (s1&p)==p){
            const uint64_t d0=l0->addend[i0],d1=l1->addend[i1];
            if(d0==d1){h=(uint8_t*)(uintptr_t)(a+d0);reason=BOOT_LB_AS_SAME;}
#if TOTK_FM_FIXED
            else if(boot_fm.direct_ok){h=BOOT_FM_VIEW+a;reason=BOOT_LB_AS_VIEW;}
#endif
        }
    }
    boot_lb_add(k->index,reason);
    return h;
}
static inline __attribute__((noinline)) uint8_t*boot_mt_access_slow(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc){
    BootCore*k=(BootCore*)c;BootContext*M=boot_mt.master;
    if(c->status){boot_lb_add(k->index,BOOT_LB_AS_STATUS);return NULL;}
    {uint8_t*h=boot_mt_access_cross(k,a,w,p);if(h)return h;}   /* logic-batch L1 */
    __atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);
    const int kstat=boot_wg_on();   /* play (62) */
    uint64_t kq0=kstat?boot_klock_now():0;   /* play (52) */
    boot_mt_klock_acquire();   /* chain P7 */
    if(kstat)boot_klock_add(0x81,boot_klock_now()-kq0,0);
    ++boot_mt.slow_paths;
    if(boot_fm.pending)boot_fm_sync(boot_fm.pending);
    else if(!boot_fm.table_ctx && boot_fm.mode)boot_fm_sync(M);   /* re-attach the master */
    uint8_t*h=NULL;
    if(boot_fm.table_ctx && w && p && !(p&~7u))h=boot_fm_table_ptr(c,a,w,p);
    if(!h){
        uint32_t status=M->status;uint64_t fpc=M->fault_pc,fa=M->fault_address,fw=M->fault_width;
        M->status=0;
        h=boot_access_lookup(M,a,w,p,pc);
        if(!h && M->status)boot_fault(c,M->status,M->fault_pc,M->fault_address,M->fault_width);
        M->status=status;M->fault_pc=fpc;M->fault_address=fa;M->fault_width=fw;
        /* a gathered read lives in the lookup's static bounce buffer: copy it out under the lock */
        if(h && w<=64){
            int region=0;
            for(unsigned i=0;i<M->region_count && !region;++i){const Region*r=&M->regions[i];
                region=h>=r->data && h<r->data+r->size;}
            if(!region){memcpy(k->bounce,h,w);h=k->bounce;}
        }
    }
    ReleaseSRWLockExclusive(&boot_mt.klock);
    boot_mt_guest_enter(k);
    return h;
}
#else
static inline uint8_t*boot_mt_access_slow(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc){(void)p;boot_fault(c,CH_ADDRESS,pc,a,w);return NULL;}
#endif
/* Park every other core outside guest code (kernel lock held by the caller, boot_mt.kernel_core = the
 * caller's worker). Returns 0 (flag withdrawn) if some core did not get out within ~10 ms: a core can be
 * blocked on a host lock the caller holds (a native NVN submit waiting for the window lock), so the
 * caller must then skip its work instead of waiting. */
static inline int boot_mt_quiesce_begin(void){
    if(!boot_mt.active)return 1;
    unsigned self=boot_mt.kernel_core;
    _InterlockedExchange(&boot_mt_quiesce,1);
    for(unsigned w=0;w<boot_mt.workers;++w)if(w!=self && __atomic_load_n(&boot_mt.core[w]->in_guest,__ATOMIC_SEQ_CST))boot_mt_kick(w,BOOT_MT_INT_RESCHED);
    LARGE_INTEGER f,t0,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t0);
    for(unsigned w=0;w<boot_mt.workers;++w){if(w==self)continue;
        while(__atomic_load_n(&boot_mt.core[w]->in_guest,__ATOMIC_SEQ_CST)){
            QueryPerformanceCounter(&t);
            if((uint64_t)(t.QuadPart-t0.QuadPart)*100>(uint64_t)f.QuadPart){_InterlockedExchange(&boot_mt_quiesce,0);return 0;}
            YieldProcessor();
        }
    }
    return 1;
}
static inline void boot_mt_quiesce_end(void){_InterlockedExchange(&boot_mt_quiesce,0);}
#ifdef TOTK_DRAW_PATH
/* boot_gpu_pack_regions from a core context (native NVN submit: a non-kernel host function holding the
 * window lock). Lock order is kernel lock, then window lock (kernel IPC handlers take the window lock), so
 * drop the window lock, enter the kernel, retake it. Packs the master's regions, then refreshes the core's
 * region table (a snapshot from the start of multi-core execution), which the caller resolves its GPU
 * mappings through. */
/* t2-A: per worker, boot_gpu_pack_rgen()+1 when its region table was last refreshed from the master here (0: never).
 * Outside BootCore so the page header does not change. */
static uint64_t boot_mt_pack_seen[BOOT_MT_MAX];
static inline int boot_mt_gpu_pack(BootContext*c,BootWindow*w,uint32_t as_fd){
    BootCore*k=(BootCore*)c;BootContext*M=boot_mt.master;
    /* t2-A: steady state without the kernel lock: this core's region table was copied at the current region
     * generation and the master's last full pack had nothing to move with the same mappings (boot_gpu_pack_unchanged,
     * boot_runtime.h). Race-free: the cache, the mapping table and boot_mt_pack_seen are only read or written under
     * w->lock (held here); every change of the master's regions bumps the generation inside its kernel-locked section,
     * so a generation equal to the cached one means no change completed since the cached full pack; a change still in
     * flight is ordered after this submission, exactly as if the submission had taken the kernel lock first. */
    const int verify=boot_gpu_pack_verify();
    if(!verify && k->index<BOOT_MT_MAX){uint64_t g=boot_gpu_pack_rgen();
        if(boot_mt_pack_seen[k->index]==g+1 && boot_gpu_pack_unchanged(M,w,as_fd,g)){
            c->exclusive_valid=0;++boot_gpu_pack_stats.skipped_unlocked;return 1;}
    }
    ReleaseSRWLockExclusive(&w->lock);
    __atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);
    AcquireSRWLockExclusive(&boot_mt.klock);
    boot_mt.kernel_core=k->index;
    AcquireSRWLockExclusive(&w->lock);
    /* TOTK_PACK_VERIFY: the unlocked decision, evaluated here where nothing can change under it */
    int would_skip=0;
    if(verify && k->index<BOOT_MT_MAX){uint64_t g=boot_gpu_pack_rgen();would_skip=boot_mt_pack_seen[k->index]==g+1 && boot_gpu_pack_unchanged(M,w,as_fd,g);}
    int ok=boot_gpu_pack_regions_locked(M,w,as_fd);
    unsigned n=M->region_count<BOOT_REGION_CAPACITY?M->region_count:BOOT_REGION_CAPACITY;
    if(would_skip){
        int same=c->region_count==n && !memcmp(c->regions,M->regions,n*sizeof(Region)) && !memcmp(c->memory_types,M->memory_types,n*sizeof(uint32_t)) &&
                 c->alias_count==M->alias_count && !memcmp(c->aliases,M->aliases,sizeof(c->aliases));
        if(!same || boot_gpu_pack_last_work || !ok)
            boot_gpu_pack_mismatch("unlocked skip would have been wrong (core region table stale, full pack had work)",(uint64_t)!same,(uint64_t)(boot_gpu_pack_last_work || !ok));
    }
    memcpy(c->regions,M->regions,n*sizeof(Region));memcpy(c->memory_types,M->memory_types,n*sizeof(uint32_t));
    if(n<c->region_count){memset(&c->regions[n],0,(c->region_count-n)*sizeof(Region));memset(&c->memory_types[n],0,(c->region_count-n)*sizeof(uint32_t));}
    c->region_count=n;
    memcpy(c->aliases,M->aliases,sizeof(c->aliases));c->alias_count=M->alias_count;
    c->exclusive_valid=0;
    if(k->index<BOOT_MT_MAX)boot_mt_pack_seen[k->index]=boot_gpu_pack_rgen()+1;   /* t2-A: the copy above is of this generation */
    boot_mt.kernel_core=UINT32_MAX;
    ReleaseSRWLockExclusive(&boot_mt.klock);
    boot_mt_guest_enter(k);
    return ok;
}
#endif
/* Scheduling service of worker k (interrupt, idle tick): save its thread, expire deadlines, poll host
 * waits, pick for its guest core. Returns 0 if the kernel rejected the plan (terminal stop). */
static inline int boot_mt_service(BootCore*k){
    const int kstat=boot_wg_on();uint64_t kq0=kstat?boot_klock_now():0;   /* chain P0: service lock wait/hold (0x82) */
    boot_mt_enter(k);++boot_mt.services;
    uint64_t kq1=kstat?boot_klock_now():0;
    BootContext*M=boot_mt.master;int ok=1;
    if(boot_mt_woke[k->index]){   /* a wait handle ended this worker's park: finish that wait (the thread is still blocked in it) */
        unsigned b=boot_mt.bound[k->index];BootThread*t=b<M->thread_count?&M->threads[b]:NULL;
        if(t && t->state==5 && boot_mt_woke[k->index]-1<t->wait_count && !boot_mt.running[b]){boot_wg_wake(b,BOOT_WG_EVENT);boot_finish_wait(t,0,(int)(boot_mt_woke[k->index]-1));}
        boot_mt_woke[k->index]=0;
    }
    if(k->thread<BOOT_THREAD_CAPACITY)boot_save_thread(M,k->pc);
    for(unsigned i=0;i<M->thread_count;++i){BootThread*t=&M->threads[i];
        if(t->state==5 && !boot_mt.running[i]){int ready=boot_poll_wait(M,t,k->pc);if(ready>=0)boot_finish_wait(t,0,ready);}}
    unsigned chosen=BOOT_MT_IDLE;
    if(!boot_scheduler_choose(M,BOOT_SCHEDULE_CHECK,CORE_SCHEDULE_NONE,&chosen))ok=0;
    else if(chosen<M->thread_count){uint64_t target;boot_load_thread(M,chosen,&target);k->pc=target;boot_wg_run(chosen);
        if(boot_mt_ft.on)boot_mt_ft_end(chosen,(uint32_t)M->x[0],1);}   /* batch-a frame trace: a wait that blocked ends here */
    else M->current_thread=BOOT_MT_IDLE;
    if(ok && M->current_thread>=M->thread_count && M->clock_mode==1 && boot_guest_dilated()){
        unsigned busy=0;for(unsigned w=0;w<boot_mt.workers;++w)busy+=w!=k->index && boot_mt.core[w]->thread<BOOT_THREAD_CAPACITY;
        if(!busy)boot_guest_clock_idle(M);          /* every core idles: advance guest time */
    }
    if(!ok){k->ctx.status=M->status?M->status:BOOT_SVC;k->ctx.fault_pc=k->pc;}
    if(kstat)boot_klock_add(0x82,kq1-kq0,boot_klock_now()-kq1);   /* chain P0 */
    boot_mt_leave(k);
    return ok;
}
static inline void boot_mt_terminal(BootCore*k,uint32_t status,uint64_t pc){
    if(_InterlockedCompareExchange(&boot_mt.stop_set,1,0)==0){
        boot_mt.stop_status=(LONG)status;boot_mt.stop_pc=pc;boot_mt.stop_worker=k->index;
    }else if(k->thread<BOOT_THREAD_CAPACITY){   /* a later stop: keep its thread resumable */
        boot_mt_enter(k);boot_save_thread(boot_mt.master,pc);boot_mt.running[k->thread]=0;k->thread=BOOT_MT_IDLE;
        boot_mt.master->current_thread=BOOT_MT_IDLE;boot_mt_leave(k);
    }
    _InterlockedExchange(&boot_mt.stop,1);
    for(unsigned w=0;w<boot_mt.workers;++w)if(w!=k->index)boot_mt_kick(w,BOOT_MT_INT_STOP);
}
/* TOTK_THREADS=host: park a worker until its own thread can run. Besides the doorbell (kernel wakeups: mutex,
 * condvar, arbiter, thread start/exit), a thread blocked in WaitSynchronization (state 5) parks on its own wait
 * handles, so SignalEvent (SetEvent) and host events wake it at once instead of at the next 1 ms timer poll. The
 * handles are duplicated under the kernel lock (the kernel closes the originals when the wait ends). The slot
 * that fired is remembered: boot_mt_service finishes the wait with it even if the handle was auto-reset. */
static inline void boot_mt_host_park(BootCore*k){
    HANDLE h[MAXIMUM_WAIT_OBJECTS];unsigned slot[MAXIMUM_WAIT_OBJECTS];DWORD n=0;
    h[n]=k->doorbell;slot[n++]=0;
    BootContext*M=boot_mt.master;unsigned b=boot_mt.bound[k->index];
    AcquireSRWLockShared(&boot_mt.klock);
    if(M && b<M->thread_count && M->threads[b].state==5){const BootThread*t=&M->threads[b];
        for(unsigned i=0;i<t->wait_count && i<64 && n<MAXIMUM_WAIT_OBJECTS;++i)if(!t->wait_threads[i] && t->wait_native[i]){
            HANDLE dup=NULL;
            if(DuplicateHandle(GetCurrentProcess(),(HANDLE)(uintptr_t)t->wait_native[i],GetCurrentProcess(),&dup,SYNCHRONIZE,FALSE,0)){h[n]=dup;slot[n++]=i+1;}
        }
    }
    /* A thread waiting with a deadline (timed mutex/arbiter/condvar wait, SleepThread) also parks on this worker's
     * high-resolution timer, set to the deadline (guest ticks at 19.2 MHz; in play mode the guest clock follows
     * host time): it wakes on time instead of at the 1 ms timer poll. If the guest clock is behind (frame lock),
     * the service finds the deadline not due yet and the worker parks again for the rest. */
    if(M && b<M->thread_count){const BootThread*t=&M->threads[b];
        if((t->state==4 || t->state==6 || t->state==7 || t->state==8) && t->wait_native[2] && n<MAXIMUM_WAIT_OBJECTS){
            static HANDLE timers[BOOT_MT_MAX];
            if(!timers[k->index])timers[k->index]=CreateWaitableTimerExW(NULL,NULL,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);
            uint64_t now=boot_guest_clock.published,deadline=t->wait_native[2];
            LARGE_INTEGER due;due.QuadPart=-(LONGLONG)(deadline>now?((deadline-now)*UINT64_C(10000000)+UINT64_C(19199999))/UINT64_C(19200000):1);
            if(timers[k->index] && SetWaitableTimer(timers[k->index],&due,0,NULL,NULL,FALSE)){h[n]=timers[k->index];slot[n++]=0x10000u;}
        }
    }
    ReleaseSRWLockShared(&boot_mt.klock);
    DWORD r=WaitForMultipleObjects(n,h,FALSE,INFINITE);
    if(r>WAIT_OBJECT_0 && r<WAIT_OBJECT_0+n && slot[r-WAIT_OBJECT_0]==0x10000u)_InterlockedIncrement64(&boot_mt_handle_wakes);   /* deadline: the service expires it */
    else if(r>WAIT_OBJECT_0 && r<WAIT_OBJECT_0+n){boot_mt_woke[k->index]=slot[r-WAIT_OBJECT_0];_InterlockedIncrement64(&boot_mt_handle_wakes);}
    else _InterlockedIncrement64(&boot_mt_doorbell_wakes);
    for(DWORD i=1;i<n;++i)if(slot[i]!=0x10000u)CloseHandle(h[i]);
}
/* chain P6 (opt-in, play-work/chain-work): soft CPU placement of guest workers on a hybrid CPU with CPU sets (per
 * thread, documented Win32, nothing system-wide, released at exit). TOTK_MT_CPUSETS=p: every guest worker and the
 * 1 ms timer thread select the performance cores (efficiency class > 0), leaving the E-cores to host helpers (ASTC
 * decode, driver threads); =split: guest priority < 48 (main, render, job workers t47..t51) on P-cores, the rest on
 * E-cores. Unset/0 or a non-hybrid CPU: no call (the OS decides, as today). Gated on P0's E-core run counts: only
 * worth enabling if the frame chain's threads are seen on E-cores. */
/* logic-batch L3: parked CPU sets are part of the sets. Parking is a momentary power state (a core parked while the
 * game loads is unparked under load); leaving the cores parked at the first call out of the selection shrank the
 * P-core set for the whole run, or switched the mode off (no E-core left: "not hybrid"). The mode and the sets are
 * published only once complete (the first call is the timer thread's, before any worker exists). Every call is
 * counted (boot_lb_report: performance / efficiency / failed). */
static inline void boot_mt_cpuset_apply(int performance){
    static int mode=-1;static ULONG pset[64],eset[64];static unsigned np,ne;
    if(mode<0){const char*v=getenv("TOTK_MT_CPUSETS");int m=!v || !strcmp(v,"0")?0:!strcmp(v,"split")?2:1;
        if(m){static uint8_t buf[16384];ULONG len=0;GetSystemCpuSetInformation(NULL,0,&len,GetCurrentProcess(),0);
            unsigned lo=255,hi=0,parked=0;
            if(len && len<=sizeof buf && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)(void*)buf,len,&len,GetCurrentProcess(),0)){
                for(ULONG off=0;off<len;){const SYSTEM_CPU_SET_INFORMATION*c=(const SYSTEM_CPU_SET_INFORMATION*)(const void*)(buf+off);if(!c->Size)break;
                    if(c->Type==CpuSetInformation){if(c->CpuSet.EfficiencyClass<lo)lo=c->CpuSet.EfficiencyClass;if(c->CpuSet.EfficiencyClass>hi)hi=c->CpuSet.EfficiencyClass;}
                    off+=c->Size;}
                for(ULONG off=0;off<len;){const SYSTEM_CPU_SET_INFORMATION*c=(const SYSTEM_CPU_SET_INFORMATION*)(const void*)(buf+off);if(!c->Size)break;
                    if(c->Type==CpuSetInformation){parked+=c->CpuSet.Parked!=0;
                        if(c->CpuSet.EfficiencyClass>lo){if(np<64)pset[np++]=c->CpuSet.Id;}else if(ne<64)eset[ne++]=c->CpuSet.Id;}
                    off+=c->Size;}
            }
            if(lo>=hi || !np || !ne)m=0;   /* not hybrid: nothing to steer */
            fprintf(stderr,"chain P6: TOTK_MT_CPUSETS mode %d (%u performance, %u efficiency CPU sets; %u parked at start, kept)\n",m,np,ne,parked);}
        mode=m;
    }
    if(!mode)return;
    int p=mode==1 || performance;
    if(SetThreadSelectedCpuSets(GetCurrentThread(),p?pset:eset,p?np:ne))_InterlockedIncrement64(p?&boot_mt_cpuset_p:&boot_mt_cpuset_e);
    else _InterlockedIncrement64(&boot_mt_cpuset_fail);
}
/* logic-batch: the 10 s stats line (timer thread, outside the kernel lock). Per frame = per QueueBuffer of the window
 * (boot_guest_clock.pacer.frames); with no frame in the interval, per second. */
static inline void boot_lb_report(void){
    if(!boot_mt_lb_stats_on())return;
    static uint64_t last_ms,last_frames,prev[BOOT_LB_N];
    const uint64_t now=GetTickCount64();
    if(last_ms && now-last_ms<10000)return;
    uint64_t sum[BOOT_LB_N],d[BOOT_LB_N];
    for(unsigned i=0;i<BOOT_LB_N;++i){sum[i]=0;for(unsigned w=0;w<=BOOT_MT_MAX;++w)sum[i]+=__atomic_load_n(&boot_lb_n[w][i],__ATOMIC_RELAXED);d[i]=sum[i]-prev[i];prev[i]=sum[i];}
    const uint64_t frames=boot_guest_clock.pacer.frames;
    if(!last_ms){last_ms=now;last_frames=frames;return;}
    const double secs=(double)(now-last_ms)/1000.0;const uint64_t nf=frames-last_frames;
    const double per=nf?(double)nf:secs>0?secs:1.0;
    last_ms=now;last_frames=frames;
#define BOOT_LB_F(i) ((double)d[i]/per)
    fprintf(stderr,"logic-batch: %.1f s, %llu frames, per %s: 0x34 kernel %.1f/%.1f/%.1f fast %.1f/%.1f/%.1f (type 0/1/2) | "
            "0x35 kernel %.1f/%.1f/%.1f (queued %.1f, busy %.1f) fast %.1f/%.1f/%.1f (cas failed %.1f/%.1f) | "
            "0x1a k %.1f f %.1f, 0x1b k %.1f f %.1f, 0x1c k %.1f, 0x1d k %.1f f %.1f | "
            "access_slow: cross lock-free %.1f + view %.1f, cross locked %.1f, in-page %.1f, detached %.1f, off %.1f, other %.1f, status %.1f | "
            "cpusets P %lld E %lld failed %lld\n",
            secs,(unsigned long long)nf,nf?"frame":"second",
            BOOT_LB_F(BOOT_LB_W),BOOT_LB_F(BOOT_LB_W+1),BOOT_LB_F(BOOT_LB_W+2),BOOT_LB_F(BOOT_LB_W+3),BOOT_LB_F(BOOT_LB_W+4),BOOT_LB_F(BOOT_LB_W+5),
            BOOT_LB_F(BOOT_LB_S),BOOT_LB_F(BOOT_LB_S+1),BOOT_LB_F(BOOT_LB_S+2),BOOT_LB_F(BOOT_LB_S_QUEUED),BOOT_LB_F(BOOT_LB_S_BUSY),
            BOOT_LB_F(BOOT_LB_S+3),BOOT_LB_F(BOOT_LB_S+4),BOOT_LB_F(BOOT_LB_S+5),BOOT_LB_F(BOOT_LB_S+7),BOOT_LB_F(BOOT_LB_S+8),
            BOOT_LB_F(BOOT_LB_LOCK_K),BOOT_LB_F(BOOT_LB_LOCK_F),BOOT_LB_F(BOOT_LB_UNLOCK_K),BOOT_LB_F(BOOT_LB_UNLOCK_F),
            BOOT_LB_F(BOOT_LB_CVWAIT_K),BOOT_LB_F(BOOT_LB_CVSIG_K),BOOT_LB_F(BOOT_LB_CVSIG_F),
            BOOT_LB_F(BOOT_LB_AS_SAME),BOOT_LB_F(BOOT_LB_AS_VIEW),BOOT_LB_F(BOOT_LB_AS_CROSS),BOOT_LB_F(BOOT_LB_AS_INPAGE),
            BOOT_LB_F(BOOT_LB_AS_DETACHED),BOOT_LB_F(BOOT_LB_AS_OFF),BOOT_LB_F(BOOT_LB_AS_OTHER),BOOT_LB_F(BOOT_LB_AS_STATUS),
            (long long)boot_mt_cpuset_p,(long long)boot_mt_cpuset_e,(long long)boot_mt_cpuset_fail);
#undef BOOT_LB_F
}
static DWORD WINAPI boot_mt_worker(void*arg){
    BootCore*k=arg;BootContext*K=&k->ctx;
    boot_mt.core_tid[k->index]=GetCurrentThreadId();
    boot_host_thread_high_qos();   /* play (62): no EcoQoS / E-core steering for guest workers (host_performance.h) */
    if(boot_mt.host_threads){unsigned b=boot_mt.bound[k->index];const BootContext*M=boot_mt.master;
        boot_mt_cpuset_apply(M && b<M->thread_count && M->threads[b].priority<48);}   /* chain P6 (opt-in) */
    for(;;){
        if(boot_mt.stop)break;
        if(boot_mt.host_threads && k->thread>=BOOT_THREAD_CAPACITY && boot_mt.master->threads[boot_mt.bound[k->index]].state==3)break;   /* its thread exited */
        if(k->thread>=BOOT_THREAD_CAPACITY || k->interrupt){
            __atomic_store_n(&K->limit,k->quota_end,__ATOMIC_SEQ_CST);
            _InterlockedExchange(&k->interrupt,0);
            if(boot_mt.stop)break;
            if(!boot_mt_service(k)){boot_mt_terminal(k,K->status,K->fault_pc);break;}
            if(k->thread>=BOOT_THREAD_CAPACITY){
                ++boot_mt.idle_waits;
                /* Spin briefly first: a cross-core wake through the doorbell costs tens of microseconds,
                 * a kick seen while spinning costs nanoseconds (mutex/condvar hand-offs between cores). */
                const unsigned spin_limit=boot_mt_spin_for(k->index);   /* play (51): was a fixed 4096 */
                uint8_t*score=&boot_mt_spin_score[k->index<=BOOT_MT_MAX?k->index:BOOT_MT_MAX];
                for(unsigned spin=0;spin<spin_limit && !__atomic_load_n(&k->interrupt,__ATOMIC_RELAXED) && !boot_mt.stop;++spin)YieldProcessor();
                if(__atomic_load_n(&k->interrupt,__ATOMIC_RELAXED)){_InterlockedIncrement64(&boot_mt_spin_wakes);*score=(uint8_t)(*score+2>8?8:*score+2);continue;}
                _InterlockedIncrement64(&boot_mt_park_wakes);if(*score)--*score;
                __atomic_store_n(&k->idle,1,__ATOMIC_SEQ_CST);
                if(!__atomic_load_n(&k->interrupt,__ATOMIC_SEQ_CST) && !boot_mt.stop){
                    /* idle tick: 1 ms (deadlines, host-handle waits) unless kicked earlier */
                    LARGE_INTEGER due;due.QuadPart=-10000;
                    if(boot_mt.host_threads)boot_mt_host_park(k);   /* doorbell or the thread's own wait handles; the timer thread still polls deadlines */
                    else if(k->timer && SetWaitableTimer(k->timer,&due,0,NULL,NULL,FALSE)){HANDLE h[2]={k->doorbell,k->timer};WaitForMultipleObjects(2,h,FALSE,INFINITE);}
                    else WaitForSingleObject(k->doorbell,1);
                }
                __atomic_store_n(&k->idle,0,__ATOMIC_SEQ_CST);
                continue;
            }
        }
        K->status=0;K->fault_pc=K->fault_address=K->fault_width=0;
        boot_mt_guest_enter(k);
        uint32_t status=boot_mt.execute(K,k->pc);
        __atomic_store_n(&k->in_guest,0,__ATOMIC_SEQ_CST);
        uint64_t pc=K->fault_pc;
        if(status==BOOT_TARGET && pc==BOOT_MT_IDLE_PC && k->thread>=BOOT_THREAD_CAPACITY){K->status=0;continue;}
        if(status==CH_BUDGET){
            K->status=0;K->fault_pc=0;k->pc=pc;
            if(K->steps>=k->quota_end){
                /* Chunks shrink as the pool drains, so a budget stop leaves little unused quota on the
                 * other cores (the stop is exact to within about pool/(2*workers) per core). */
                LONG64 left,take;
                do{
                    left=__atomic_load_n(&boot_mt.pool,__ATOMIC_SEQ_CST);
                    if(left<=0)break;
                    take=left/(LONG64)(2*boot_mt.workers);if(take>(LONG64)BOOT_MT_CHUNK)take=(LONG64)BOOT_MT_CHUNK;
                    if(take<4096)take=left<4096?left:4096;
                }while(!__atomic_compare_exchange_n(&boot_mt.pool,&left,left-take,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST));
                if(left<=0){boot_mt_terminal(k,CH_BUDGET,pc);break;}
                k->quota_end=K->steps+(uint64_t)take;
            }
            _InterlockedOr(&k->interrupt,0);
            if(!k->interrupt)__atomic_store_n(&K->limit,k->quota_end,__ATOMIC_SEQ_CST);
            continue;
        }
        boot_mt_terminal(k,status,pc);break;
    }
    /* park: hand the running thread back to the master's table (resumable at k->pc) */
    if(k->thread<BOOT_THREAD_CAPACITY && !(boot_mt.stop_worker==k->index && boot_mt.stop_set)){
        boot_mt_enter(k);boot_save_thread(boot_mt.master,k->pc);boot_mt.running[k->thread]=0;
        boot_mt.master->current_thread=BOOT_MT_IDLE;k->thread=BOOT_MT_IDLE;
        boot_mt.kernel_core=UINT32_MAX;ReleaseSRWLockExclusive(&boot_mt.klock);
    }
    _InterlockedDecrement(&boot_mt.alive);
    return 0;
}
/* Timer tick (the host thread waiting in boot_mt_execute, about every millisecond): expire due guest
 * deadlines (sleep, timed condvar/arbiter/mutex waits) under the kernel lock and interrupt the workers
 * whose guest core now has a better runnable thread. Cheap racy pre-check: nothing to do without a
 * pending deadline. */
static inline void boot_mt_timer_tick(void){
    BootContext*M=boot_mt.master;if(!M || boot_mt.stop)return;
    if(boot_mt.host_threads){
        /* Parked workers wait without a tick: here deadlines expire, host-handle waits (state 5) are
         * polled, the guest clock idles forward when no worker runs guest code, and woken threads'
         * workers are kicked (or spawned). */
        const int kstat=boot_wg_on();uint64_t kq0=kstat?boot_klock_now():0;   /* chain P0: timer tick (0x83) */
        boot_mt_klock_acquire();   /* chain P7 */
        uint64_t kq1=kstat?boot_klock_now():0;
        M->steps=boot_mt_total_steps();
        boot_condition_expire(M);
        for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY;++i){BootThread*t=&M->threads[i];
            if(t->state==5 && !boot_mt.running[i]){int ready=boot_poll_wait(M,t,t->pc);if(ready>=0)boot_finish_wait(t,0,ready);}}
        if(M->clock_mode==1 && boot_guest_dilated()){
            unsigned busy=0;for(unsigned w=0;w<boot_mt.workers;++w)busy+=boot_mt.core[w]->thread<BOOT_THREAD_CAPACITY;
            if(!busy)boot_guest_clock_idle(M);
        }
        boot_mt_kick_check(UINT32_MAX);
        if(kstat)boot_klock_add(0x83,kq1-kq0,boot_klock_now()-kq1);   /* chain P0 (before the dump: the dump is not a tick) */
        boot_wg_dump(M);
        boot_mt_klock_release();   /* chain P1 */
        boot_wg_flush();   /* play (62): the log is written outside the kernel lock */
        boot_lb_report();   /* logic-batch: every 10 s, outside the kernel lock */
        return;
    }
    unsigned pending=0;
    for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY && !pending;++i){const BootThread*t=&M->threads[i];
        pending=(t->state==4 || t->state==6 || t->state==7 || t->state==8) && t->wait_native[2]!=0;}
    if(!pending)return;
    AcquireSRWLockExclusive(&boot_mt.klock);
    M->steps=boot_mt_total_steps();
    boot_condition_expire(M);
    boot_mt_kick_check(UINT32_MAX);
    ReleaseSRWLockExclusive(&boot_mt.klock);
}
/* Entry used by boot_run/boot_resume (partition_native.py): single-core unless TOTK_CORES>1. */
static inline uint32_t boot_mt_execute(BootContext*c,uint64_t target,uint32_t(*execute)(BootContext*,uint64_t)){
    if(!boot_mt.configured){
        boot_mt.configured=1;boot_host_process_tuning();   /* play (62): per-process timer resolution / EcoQoS opt-out when the launcher did not (host_performance.h) */
        const char*v=getenv("TOTK_CORES");
        boot_mt.requested=v?(unsigned)strtoul(v,NULL,10):1;
        const char*h=getenv("TOTK_THREADS");
        if(h && !strcmp(h,"host")){boot_mt.host_threads=1;if(boot_mt.requested<2)boot_mt.requested=2;}
    }
#if TOTK_FASTMEM
    if(boot_mt.requested>1 && !boot_fm.initialized)boot_fm_init(c);
    if(boot_mt.requested>1 && !boot_fm.mode){
        boot_fm_log("TOTK_CORES=%u needs fastmem (TOTK_FASTMEM=1 or 2): running single-core",boot_mt.requested);boot_mt.requested=1;
    }
#else
    boot_mt.requested=1;   /* multi-core needs the fastmem build */
#endif
#if TOTK_FASTMEM && TOTK_FM_FIXED
    if(!boot_fm.initialized)boot_fm_init(c);
    if(boot_fm.mode!=1 || !boot_fm.direct_ok || boot_fm.base!=(uint8_t*)(uintptr_t)BOOT_FM_FIXED_BASE){
        boot_fm_log("TOTK_FM_FIXED: guest view not at %#llx (mode %u): cannot run",(unsigned long long)BOOT_FM_FIXED_BASE,boot_fm.mode);
        boot_fault(c,CH_ADDRESS,target,BOOT_FM_FIXED_BASE,0);return c->status;
    }
    if(boot_fm.table_ctx!=c)boot_fm_enter(c);
#endif
    if(boot_mt.requested<=1)return execute(c,target);
    for(;;){
        uint64_t cores=c->layout_info[0];unsigned n=0;for(unsigned b=0;b<64;++b)n+=(cores>>b)&1;
        if(c->thread_count<2 || !n || n>BOOT_MT_MAX || c->current_thread>=c->thread_count){
            /* No thread table yet: run single-core; boot_svc hijacks the limit once threads exist. */
            uint32_t status=execute(c,target);
            if(boot_mt.hijacked){
                c->limit=boot_mt.saved_limit;boot_mt.hijacked=0;
                if(status==CH_BUDGET && c->fault_pc && c->steps<c->limit){target=c->fault_pc;c->status=0;c->fault_pc=c->fault_address=c->fault_width=0;continue;}
            }
            return status;
        }
        /* Start: the master's running thread goes back to the table at target. The master must be
         * attached to the fastmem fast paths (core contexts piggy-back on it). */
        BootContext*M=c;
#if TOTK_FASTMEM
        if(boot_fm.table_ctx!=M)boot_fm_enter(M);
#endif
        boot_save_thread(M,target);
        boot_mt.master=M;boot_mt.execute=execute;boot_mt.workers=n;boot_mt.kernel_core=UINT32_MAX;
        boot_mt.stop=0;boot_mt.stop_set=0;boot_mt.stop_status=0;boot_mt.stop_pc=0;boot_mt.stop_worker=UINT32_MAX;
        boot_mt.base_steps=M->steps;boot_mt.pool=M->limit>M->steps?(LONG64)(M->limit-M->steps>(uint64_t)INT64_MAX?INT64_MAX:M->limit-M->steps):0;
        memset(boot_mt.running,0,sizeof(boot_mt.running));
        boot_mt.access_slow=boot_mt_access_slow;boot_mt.system_tick=boot_mt_system_tick;
        unsigned w=0;
        if(boot_mt.host_threads){n=0;boot_mt.workers=0;boot_mt.alive=0;memset(boot_mt.worker_of,0,sizeof(boot_mt.worker_of));}
        for(unsigned b=0;b<64 && w<n;++b)if((cores>>b)&1){
            BootCore*k=boot_mt.core[w];
            if(!k){k=_aligned_malloc(sizeof(BootCore),64);if(!k){boot_fault(M,BOOT_SVC,target,0,0);return M->status;}boot_mt.core[w]=k;}
            memset(k,0,sizeof(*k));memcpy(&k->ctx,M,sizeof(BootContext));
            k->ctx.tlb_epoch=BOOT_TLB_CORE;
            k->ctx.status=0;k->ctx.steps=0;k->ctx.limit=0;k->ctx.trace_count=0;k->ctx.current_thread=BOOT_MT_IDLE;   /* see boot_mt_spawn */
            k->index=w;k->vcore=b;k->thread=BOOT_MT_IDLE;k->quota_end=0;
            k->doorbell=CreateEventW(NULL,FALSE,FALSE,NULL);
            k->timer=CreateWaitableTimerExW(NULL,NULL,0x00000002/*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);
            boot_mt.vcore[w]=b;++w;
        }
        ++boot_mt.starts;
        boot_mt_crash_install();
        _InterlockedExchange(&boot_mt.active,1);
#if TOTK_PLAY
        boot_guest_clock.play_runners=boot_mt.host_threads?UINT32_MAX:n;   /* guest time: host threads running guest code (guest_time.h) */
#endif
        if(boot_mt.host_threads){   /* under the kernel lock: running workers spawn too (kick check) */
            /* the waiting host thread is the 1 ms timer (deadlines, host-handle waits, wakeups): above the workers */
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
            boot_host_thread_high_qos();   /* play (62) */
            boot_mt_cpuset_apply(1);   /* chain P6 (opt-in): the timer thread with the chain */
            AcquireSRWLockExclusive(&boot_mt.klock);
            for(unsigned i=0;i<M->thread_count;++i)if(M->threads[i].state==2 && !boot_mt.worker_of[i])boot_mt_spawn(i);
            ReleaseSRWLockExclusive(&boot_mt.klock);
        }
        else{boot_mt.alive=(LONG)n;for(w=0;w<n;++w)boot_mt.core[w]->host=CreateThread(NULL,8u<<20,boot_mt_worker,boot_mt.core[w],0,NULL);}
        HANDLE handles[BOOT_MT_MAX];for(w=0;w<n;++w)handles[w]=boot_mt.core[w]->host;
        /* The waiting host thread is the timer: every millisecond it expires guest deadlines and
         * interrupts the cores that should run a woken thread (a sleeping thread must wake even
         * when every core is busy in guest code without SVCs); mt-status.txt once a second. */
        boot_mt_status_write();
        {HANDLE tick=CreateWaitableTimerExW(NULL,NULL,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);unsigned ticks=0;
         for(;;){
            LARGE_INTEGER due;due.QuadPart=-10000;
            if(boot_mt.host_threads){   /* workers come and go: wait until none is left */
                if(!__atomic_load_n(&boot_mt.alive,__ATOMIC_SEQ_CST))break;
                if(tick && SetWaitableTimer(tick,&due,0,NULL,NULL,FALSE))WaitForSingleObject(tick,INFINITE);else Sleep(1);
            }else if(tick && SetWaitableTimer(tick,&due,0,NULL,NULL,FALSE)){if(WaitForMultipleObjects(n,handles,TRUE,0)!=WAIT_TIMEOUT)break;WaitForSingleObject(tick,INFINITE);}
            else if(WaitForMultipleObjects(n,handles,TRUE,1)!=WAIT_TIMEOUT)break;
            boot_mt_timer_tick();
            if(++ticks>=1000){ticks=0;boot_mt_status_write();}
         }
         if(tick)CloseHandle(tick);}
        boot_mt_status_write();
        _InterlockedExchange(&boot_mt.active,0);
        if(boot_mt.host_threads)SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
#if TOTK_PLAY
        boot_guest_clock.play_runners=0;
#endif
        /* Compose the master as single-core execution would have left it at this stop. */
        uint32_t status=(uint32_t)boot_mt.stop_status;
        if(boot_mt.host_threads)n=boot_mt.workers;
        BootCore*s=boot_mt.stop_worker<n?boot_mt.core[boot_mt.stop_worker]:NULL;
        M->steps=boot_mt_total_steps();
        if(s && s->thread<M->thread_count){
            boot_mt_copy_cpu(M,&s->ctx);M->current_thread=s->thread;
            memcpy(M->trace,s->ctx.trace,sizeof(M->trace));M->trace_count=s->ctx.trace_count;
        }else{
            unsigned pick=0;for(unsigned i=0;i<M->thread_count;++i)if(M->threads[i].state==2){pick=i;break;}
            uint64_t ignored;boot_load_thread(M,pick,&ignored);
        }
        M->status=0;boot_fault(M,status,boot_mt.stop_pc,s?s->ctx.fault_address:0,s?s->ctx.fault_width:0);
        if(s && status==BOOT_SVC)M->svc_id=s->ctx.svc_id;
        for(w=0;w<n;++w){if(boot_mt.host_threads)WaitForSingleObject(boot_mt.core[w]->host,INFINITE);
            CloseHandle(boot_mt.core[w]->host);CloseHandle(boot_mt.core[w]->doorbell);
            if(boot_mt.core[w]->timer)CloseHandle(boot_mt.core[w]->timer);boot_mt.core[w]->host=NULL;}
        boot_mt.access_slow=NULL;boot_mt.system_tick=NULL;
        return M->status;
    }
}
#endif
