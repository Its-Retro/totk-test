/* Multi-core execution state (TOTK_CORES, see mt-work/DESIGN.md). Included by boot_runtime.h right
 * after BootContext. With TOTK_CORES unset or 1 nothing here is ever activated: boot_mt.active stays 0,
 * no context carries BOOT_TLB_CORE, and every hook below is a not-taken branch.
 *
 * With TOTK_CORES>1 (requires fastmem mode 1 or 2, host_fastmem.h; off by default) each guest core
 * runs on its own host worker thread with its own BootContext copy (the "core context", tlb_epoch ==
 * BOOT_TLB_CORE) holding the registers of the guest thread it runs. The host-allocated BootContext
 * stays the process record ("master"): kernel work (every SVC, CNTPCT) takes the kernel lock, copies
 * the core's CPU state into the master, runs the unchanged single-core handler on the master, and
 * copies the result back. The scheduler then picks for the calling guest core only. */
#ifndef TOTK_BOOT_MT_EARLY_H
#define TOTK_BOOT_MT_EARLY_H
/* Workers: one per guest core (TOTK_CORES), or one per guest thread (TOTK_THREADS=host). */
#define BOOT_MT_MAX (BOOT_THREAD_CAPACITY+8)
#define BOOT_MT_IDLE (BOOT_THREAD_CAPACITY+1u)          /* "no thread": the core idles */
#define BOOT_MT_IDLE_PC UINT64_C(0xfffffffffffff000)    /* target of a switch to idle (not a code page) */
#define BOOT_TLB_CORE 0xffffffffu                       /* tlb_epoch marking a core context (never a TLB epoch) */
/* Per-core hot fields right after the core context (BootCore): the fastmem pc hint and page-frame
 * mark of that host thread (single-core execution keeps using boot_fm.pc / boot_fm.page_rsp). */
typedef struct BootCoreHot { volatile uint64_t fm_pc; uint64_t fm_page_rsp; } BootCoreHot;
#define BOOT_CORE_HOT(c) ((BootCoreHot*)(void*)((uint8_t*)(c)+sizeof(BootContext)))
static inline int boot_mt_core_ctx(const BootContext*c){return c->tlb_epoch==BOOT_TLB_CORE;}
struct BootCore;
typedef struct BootMt {
    volatile LONG active;                 /* workers are running */
    unsigned configured,requested;        /* TOTK_CORES parsed / value */
    unsigned workers,kernel_core;         /* kernel_core: worker inside the kernel (UINT32_MAX none) */
    unsigned vcore[BOOT_MT_MAX];          /* guest core served by each worker */
    unsigned running[BOOT_THREAD_CAPACITY];/* worker+1 currently executing thread i, 0 none */
    SRWLOCK klock;                        /* kernel lock: exclusive for kernel work, shared for TLB fills */
    struct BootCore*core[BOOT_MT_MAX];
    BootContext*master;
    uint32_t (*execute)(BootContext*,uint64_t);
    /* hooks reached from header inline code (set while active; keep page units free of MT bodies) */
    uint8_t*(*access_slow)(BootContext*,uint64_t,unsigned,unsigned,uint64_t);
    int (*system_tick)(BootContext*,uint64_t,uint64_t*);
    DWORD core_tid[BOOT_MT_MAX];          /* host thread id of each worker (fault handler lookup) */
    /* stop protocol */
    volatile LONG stop,stop_set,stop_status;uint64_t stop_pc;unsigned stop_worker;
    /* switch from single-core execution once threads exist */
    unsigned hijacked;uint64_t saved_limit;
    /* instruction budget: shared pool, per-core chunks */
    uint64_t base_steps;volatile LONG64 pool;
    uint64_t starts,svcs,services,kicks,slow_paths,idle_waits;
    /* TOTK_THREADS=host: each guest thread runs on its own host worker (bound[w] = its thread index,
     * worker_of[thread] = w+1); the OS schedules them. alive = workers whose host thread has not ended. */
    unsigned host_threads;unsigned bound[BOOT_MT_MAX];unsigned worker_of[BOOT_THREAD_CAPACITY];volatile LONG alive;
} BootMt;
__declspec(selectany) BootMt boot_mt={.kernel_core=UINT32_MAX};
/* SetCpuBoostMode with TOTK_CORES>1: priority of every worker thread (boot_mt_late.h). */
static inline void boot_mt_boost_workers(unsigned mode);
static inline int boot_mt_in_kernel(void){return boot_mt.active && boot_mt.kernel_core!=UINT32_MAX;}
/* Worker index of the calling host thread, or -1 (fastmem fault handler). */
static inline int boot_mt_worker_of_thread(DWORD tid){
    if(!boot_mt.active)return -1;
    for(unsigned w=0;w<boot_mt.workers;++w)if(boot_mt.core_tid[w]==tid)return (int)w;
    return -1;
}
#endif
