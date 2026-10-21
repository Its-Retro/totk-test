#ifndef TOTK_BOOT_SCHEDULER_H
#define TOTK_BOOT_SCHEDULER_H

/* Runtime adapter. Prepare and choose on a private queue copy, committing only
 * after every affinity, queue and selection check succeeds. Unused slots do
 * not participate. exclude models a thread about to block or exit. */
enum { BOOT_SCHEDULE_CHECK=0, BOOT_SCHEDULE_YIELD=1, BOOT_SCHEDULE_MIGRATE=2 };
static inline int boot_scheduler_prepare(const BootContext *c,
                                         CoreScheduleThread *queue, unsigned exclude) {
    if (!c->thread_count || c->thread_count>BOOT_THREAD_CAPACITY ||
        c->current_thread>=c->thread_count) return 0;
    uint64_t last=0;
    for (unsigned i=0;i<c->thread_count;++i)
        if (c->threads[i].schedule.order>last) last=c->threads[i].schedule.order;
    for (unsigned i=0;i<c->thread_count;++i) {
        const BootThread *t=&c->threads[i];
        queue[i]=t->schedule;
        if (!t->state) { queue[i].runnable=0; continue; }
        if (t->state>8 || t->priority>=64) return 0;
        if (!queue[i].affinity) queue[i].active=t->core;
        if (!core_schedule_affinity(&queue[i],t->core_mask,t->core,c->layout_info[0])) return 0;
        unsigned runnable=t->state==2 && i!=exclude;
        if (runnable && (!queue[i].runnable || queue[i].priority!=t->priority)) {
            if (last==UINT64_MAX) return 0;
            queue[i].order=++last;
        }
        queue[i].priority=t->priority;
        queue[i].runnable=runnable;
    }
    return 1;
}

/* Select a core's highest-priority runnable thread. At cooperative scheduling
 * points other cores also need execution: their priorities are independent.
 * Prefer the least recently executed front when rotating between cores. */
static inline int boot_scheduler_select(BootContext *c, unsigned mode,
                                        unsigned exclude, unsigned *chosen) {
    CoreScheduleThread queue[BOOT_THREAD_CAPACITY];
    if (mode>BOOT_SCHEDULE_MIGRATE || !boot_scheduler_prepare(c,queue,exclude)) return 0;
    unsigned current=c->current_thread,core=queue[current].active;
    if (core>=64) return 0;
    if (mode && queue[current].runnable) {
        unsigned ignored;
        if (!core_schedule_yield(queue,c->thread_count,current,mode==BOOT_SCHEDULE_MIGRATE,&ignored)) return 0;
    }
    for (unsigned k=0;k<64;++k) {
        if (!(c->layout_info[0]&(UINT64_C(1)<<k))) continue;
        if (core_schedule_front(queue,c->thread_count,k)==CORE_SCHEDULE_NONE) {
            unsigned ignored;
            if (!core_schedule_fill_idle(queue,c->thread_count,k,&ignored)) return 0;
        }
    }
    core=queue[current].active;
    unsigned selected=core_schedule_front(queue,c->thread_count,core);
    if (!mode) {
        /* The cores run concurrently on hardware: each core's front (highest-priority) thread
         * makes progress regardless of other cores' priorities. One host thread emulates that by
         * giving the turn to the core front that has waited longest, so priority orders threads
         * within a core only. (Choosing the best priority across all cores starved lower-priority
         * cores; their loading workers did not run for tens of billions of instructions.) The
         * running thread counts as scheduled now. */
        uint64_t best=UINT64_MAX;
        for (unsigned k=0;k<64;++k) {
            unsigned front=core_schedule_front(queue,c->thread_count,k);
            if (front==CORE_SCHEDULE_NONE) continue;
            uint64_t last=front==current?boot_order_now(c):queue[front].last_scheduled;
            if (selected==CORE_SCHEDULE_NONE || last<best) { selected=front; best=last; }
        }
    }
    if (mode || selected==CORE_SCHEDULE_NONE) {
        unsigned other=CORE_SCHEDULE_NONE;
        for (unsigned k=0;k<64;++k) {
            if (k==core) continue;
            unsigned front=core_schedule_front(queue,c->thread_count,k);
            if (front!=CORE_SCHEDULE_NONE && (other==CORE_SCHEDULE_NONE ||
                queue[front].last_scheduled<queue[other].last_scheduled)) other=front;
        }
        if (other!=CORE_SCHEDULE_NONE) selected=other;
    }
    if (selected==CORE_SCHEDULE_NONE) return 0;
    for (unsigned i=0;i<c->thread_count;++i) c->threads[i].schedule=queue[i];
    *chosen=selected;
    return 1;
}

/* Condition wait deadlines share the guest system counter's 19.2MHz clock (guest_time.h: dilated
 * guest time by default, legacy wall clock with TOTK_GUEST_CLOCK=wall).
 * Slot 2 is metadata, never a native handle; zero means no deadline. */
static inline int boot_condition_clock(BootContext*c,uint64_t*ticks){
    if(c->clock_mode==2){*ticks=c->clock_test_ticks;return 1;}
    if(c->clock_mode!=1)return 0;
    return boot_guest_clock_now(c,ticks);
}
static inline int boot_condition_expire(BootContext*c){
    unsigned active=0;
    for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
        active+=(t->state==7 || t->state==4 || t->state==6 || t->state==8) && t->wait_native[2]!=0;
    }
    uint64_t now=0;
    if(boot_guest_clock.stats && c->clock_mode==1){
        if(!boot_condition_clock(c,&now))return 0;
        boot_time_observe(c,now);
    }
    if(!active)return 1;
    if(!now && !boot_condition_clock(c,&now))return 0;
    for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
        if((t->state!=7 && t->state!=4 && t->state!=6 && t->state!=8) || !t->wait_native[2] || now<t->wait_native[2])continue;
        boot_time_expired(c,i,now);
        if(t->state==8){t->state=2;t->wait_native[2]=0;continue;}
        /* Kernel timeout removes queue ownership. The caller relocks in
         * userland; neither the mutex word nor its key flag is rewritten. */
        t->state=2;t->x[0]=UINT32_C(0xea01);t->wait_address=t->wait_owner=0;
        t->wait_native[0]=t->wait_native[1]=t->wait_native[2]=0;
    }
    return 1;
}
/* Scheduling plans run in place on the context. Before a plan, BootSchedUndo records every field
 * the plan may change (per-thread scheduling/wait fields of threads [0,thread_count), optionally
 * one thread's whole record and the CPU state a thread switch rewrites, the guest test clock and
 * the fault state); a rejected plan restores them exactly. This replaces copying the whole
 * BootContext (616 KB: malloc + memcpy + free per yield/condvar/arbiter wait whenever any
 * thread had a deadline). Fields a plan writes: state, x[0], x[1], wait_address, wait_owner,
 * wait_native[0..2], wait_count, wait_cancel, schedule (expiry, selection, condvar/arbiter queue
 * changes); boot_save_thread/boot_load_thread on the current thread and the CPU state. */
typedef struct BootSchedUndoThread {
    uint64_t x0,x1,wait_address,wait_native[3];
    uint32_t state,wait_owner,wait_count,wait_cancel;
    CoreScheduleThread schedule;
} BootSchedUndoThread;
typedef struct BootSchedUndo {
    unsigned count,current_index,has_current;
    BootThread current;
    uint64_t x[31],sp,tpidrro,tpidr,clock_test_ticks,fault_pc,fault_address,fault_width;
    uint8_t q[32][16];
    uint32_t n,z,carry,v,fpcr,fpsr,current_thread,exclusive_valid,thread_switch_count,status;
    BootSchedUndoThread t[BOOT_THREAD_CAPACITY];
} BootSchedUndo;
enum { BOOT_UNDO_THREADS=1, BOOT_UNDO_CPU=2, BOOT_UNDO_FAULT=4, BOOT_UNDO_CLOCK=8, BOOT_UNDO_ALL=15 };
static inline void boot_sched_undo_save(const BootContext*c,BootSchedUndo*u,int with_current){
    unsigned count=c->thread_count<=BOOT_THREAD_CAPACITY?c->thread_count:BOOT_THREAD_CAPACITY;
    u->count=count;
    for(unsigned i=0;i<count;++i){const BootThread*t=&c->threads[i];BootSchedUndoThread*s=&u->t[i];
        s->x0=t->x[0];s->x1=t->x[1];s->wait_address=t->wait_address;
        s->wait_native[0]=t->wait_native[0];s->wait_native[1]=t->wait_native[1];s->wait_native[2]=t->wait_native[2];
        s->state=t->state;s->wait_owner=t->wait_owner;s->wait_count=t->wait_count;s->wait_cancel=t->wait_cancel;
        s->schedule=t->schedule;
    }
    u->has_current=with_current && c->current_thread<BOOT_THREAD_CAPACITY;u->current_index=c->current_thread;
    if(u->has_current)u->current=c->threads[c->current_thread];
    memcpy(u->x,c->x,sizeof(u->x));memcpy(u->q,c->q,sizeof(u->q));
    u->sp=c->sp;u->tpidrro=c->tpidrro;u->tpidr=c->tpidr;u->n=c->n;u->z=c->z;u->carry=c->carry;u->v=c->v;
    u->fpcr=c->fpcr;u->fpsr=c->fpsr;u->current_thread=c->current_thread;u->exclusive_valid=c->exclusive_valid;
    u->thread_switch_count=c->thread_switch_count;u->clock_test_ticks=c->clock_test_ticks;
    u->status=c->status;u->fault_pc=c->fault_pc;u->fault_address=c->fault_address;u->fault_width=c->fault_width;
}
static inline void boot_sched_undo_restore(BootContext*c,const BootSchedUndo*u,unsigned what){
    if(what&BOOT_UNDO_THREADS){
        if(u->has_current)c->threads[u->current_index]=u->current;
        for(unsigned i=0;i<u->count;++i){BootThread*t=&c->threads[i];const BootSchedUndoThread*s=&u->t[i];
            t->x[0]=s->x0;t->x[1]=s->x1;t->wait_address=s->wait_address;
            t->wait_native[0]=s->wait_native[0];t->wait_native[1]=s->wait_native[1];t->wait_native[2]=s->wait_native[2];
            t->state=s->state;t->wait_owner=s->wait_owner;t->wait_count=s->wait_count;t->wait_cancel=s->wait_cancel;
            t->schedule=s->schedule;
        }
    }
    if(what&BOOT_UNDO_CPU){
        memcpy(c->x,u->x,sizeof(c->x));memcpy(c->q,u->q,sizeof(c->q));
        c->sp=u->sp;c->tpidrro=u->tpidrro;c->tpidr=u->tpidr;c->n=u->n;c->z=u->z;c->carry=u->carry;c->v=u->v;
        c->fpcr=u->fpcr;c->fpsr=u->fpsr;c->current_thread=u->current_thread;c->exclusive_valid=u->exclusive_valid;
        c->thread_switch_count=u->thread_switch_count;
    }
    if(what&BOOT_UNDO_CLOCK)c->clock_test_ticks=u->clock_test_ticks;
    if(what&BOOT_UNDO_FAULT){c->status=u->status;c->fault_pc=u->fault_pc;c->fault_address=u->fault_address;c->fault_width=u->fault_width;}
}
/* Multi-core (boot_mt_early.h): pick for the guest core of the worker inside the kernel only.
 * Threads executing on other cores are not candidates (their stored queue state is kept). Priority and
 * FIFO order as boot_scheduler_select; yield modes rotate the caller first. An empty core takes a
 * migratable thread (core_schedule_fill_idle); otherwise it idles: *chosen=BOOT_MT_IDLE (success). */
static inline int boot_mt_choose(BootContext*c,unsigned mode,unsigned exclude,unsigned*chosen){
    if(boot_mt.host_threads){
        /* One host worker per guest thread: a worker only ever runs its own thread, when it is runnable
         * (the woken threads' workers are kicked by boot_mt_kick_check). Priorities are the OS's job. */
        if(mode>BOOT_SCHEDULE_MIGRATE || !c->thread_count || c->thread_count>BOOT_THREAD_CAPACITY)return 0;
        if(!boot_condition_expire(c))return 0;
        unsigned t=boot_mt.bound[boot_mt.kernel_core];
        *chosen=t<c->thread_count && t!=exclude && c->threads[t].state==2?t:BOOT_MT_IDLE;
        return 1;
    }
    if(mode>BOOT_SCHEDULE_MIGRATE || c->thread_count>BOOT_THREAD_CAPACITY || !c->thread_count)return 0;
    if(!boot_condition_expire(c))return 0;
    unsigned current=c->current_thread,fake=current>=c->thread_count;
    if(fake)c->current_thread=0;
    CoreScheduleThread queue[BOOT_THREAD_CAPACITY];unsigned char hidden[BOOT_THREAD_CAPACITY];
    int ok=boot_scheduler_prepare(c,queue,fake?CORE_SCHEDULE_NONE:exclude);
    c->current_thread=current;
    if(!ok)return 0;
    unsigned self=boot_mt.kernel_core+1,core=boot_mt.vcore[boot_mt.kernel_core];
    for(unsigned i=0;i<c->thread_count;++i){
        hidden[i]=boot_mt.running[i] && boot_mt.running[i]!=self && queue[i].runnable;
        if(hidden[i])queue[i].runnable=0;
        if(fake && i==exclude)queue[i].runnable=0;
    }
    if(!fake && mode && queue[current].runnable){
        unsigned ignored;
        if(!core_schedule_yield(queue,c->thread_count,current,mode==BOOT_SCHEDULE_MIGRATE,&ignored))return 0;
    }
    unsigned selected=core_schedule_front(queue,c->thread_count,core);
    if(selected==CORE_SCHEDULE_NONE){
        unsigned moved=CORE_SCHEDULE_NONE;
        if(core_schedule_fill_idle(queue,c->thread_count,core,&moved) && moved!=CORE_SCHEDULE_NONE)selected=moved;
    }
    for(unsigned i=0;i<c->thread_count;++i){if(hidden[i])queue[i].runnable=1;c->threads[i].schedule=queue[i];}
    *chosen=selected==CORE_SCHEDULE_NONE?BOOT_MT_IDLE:selected;
    return 1;
}
static inline int boot_scheduler_choose(BootContext*c,unsigned mode,unsigned exclude,unsigned*chosen){
    if(boot_mt_in_kernel())return boot_mt_choose(c,mode,exclude,chosen);
    if(mode>BOOT_SCHEDULE_MIGRATE || c->thread_count>BOOT_THREAD_CAPACITY)return 0;
    unsigned timers=0;
    for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
        timers+=(t->state==7 || t->state==4 || t->state==6 || t->state==8) && t->wait_native[2]!=0;
    }
    if(!timers){
        if(boot_scheduler_select(c,mode,exclude,chosen))return 1;
        /* Dilated: guest-timed host events (vsync, audio completion) can still wake a
         * thread blocked on handles, so an all-blocked process idles on guest time. */
        if(c->clock_mode!=1 || !boot_guest_dilated())return 0;
    }
    /* The plan runs in place (p==c); success commits threads[] and the guest test clock and
     * restores everything else (a failed poll's fault state), as the old private copy did. */
    BootSchedUndo undo;boot_sched_undo_save(c,&undo,0);
    BootContext*p=c;
    int woken=0;   /* finished handle waits (their duplicates are closed): keep them */
    for(;;){
        CoreScheduleThread queue[BOOT_THREAD_CAPACITY];
        if(!boot_condition_expire(p) || !boot_scheduler_prepare(p,queue,exclude))break;
        unsigned runnable=0;
        for(unsigned i=0;i<p->thread_count;++i)runnable+=queue[i].runnable!=0;
        if(runnable){
            unsigned selected;
            if(!boot_scheduler_select(p,mode,exclude,&selected))break;
            boot_sched_undo_restore(c,&undo,BOOT_UNDO_CPU|BOOT_UNDO_FAULT);
            *chosen=selected;return 1;
        }
        /* An idle process waits for guest time (wall clock in legacy mode); deterministic
         * tests must set their clock explicitly, never manufacture elapsed guest time. */
        if(p->clock_mode!=1)break;
        int dilated=boot_guest_dilated();
        unsigned pending=0,handles=0;
        for(unsigned i=0;i<p->thread_count;++i){
            pending+=(i!=exclude &&
                (p->threads[i].state==7 || p->threads[i].state==4 || p->threads[i].state==6 || p->threads[i].state==8) && p->threads[i].wait_native[2]!=0);
            handles+=i!=exclude && p->threads[i].state==5;
        }
        if(dilated && handles && (boot_guest_clock.timer_count || boot_guest_vsync_slots(p)))++pending;
        if(!pending || (p->window_native && boot_window_is_closed((BootWindow*)(uintptr_t)p->window_native)))break;
        if(dilated){
            int woke=0,failed=0;
            for(unsigned i=0;i<p->thread_count;++i)if(i!=exclude && p->threads[i].state==5){
                int index=boot_poll_wait(p,&p->threads[i],0);
                if(index==-2){failed=1;break;}
                if(index>=0){boot_finish_wait(&p->threads[i],0,index);woke=woken=1;}
            }
            if(failed)break;
            if(woke || boot_guest_clock_idle(p))continue;
        }
        Sleep(1);
    }
    /* Rejected: restore everything, except that finished handle waits keep the thread changes. */
    boot_sched_undo_restore(c,&undo,woken?BOOT_UNDO_CPU|BOOT_UNDO_FAULT|BOOT_UNDO_CLOCK:BOOT_UNDO_ALL);
    return 0;
}
#endif
