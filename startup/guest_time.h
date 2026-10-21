#ifndef TOTK_GUEST_TIME_H
#define TOTK_GUEST_TIME_H
/* BootContext glue for the guest clock (guest_clock.h): progress measure, busy virtual cores,
 * guest-timed host events (vsync, audio completion), idle fast-forward and call-site statistics.
 * Included by boot_runtime.h after BootContext and before boot_scheduler.h. */
#include "guest_clock.h"
#include "host_present_hold.h"

static inline int boot_poll_wait(BootContext*c,BootThread*t,uint64_t pc);
static inline int boot_render_trylock(void);static inline void boot_render_unlock(int held);   /* boot_mt_late.h */
static inline void boot_finish_wait(BootThread*t,uint32_t result,int index);
/* HID npad sampling (boot_runtime.h): runs from every guest clock update, at most once per ms. */
static inline void boot_hid_npad_tick(BootContext*c,uint64_t now_ms);

/* Guest progress: instructions executed by all guest threads (one host thread runs them in turn).
 * A multi-host-thread runtime must return the sum over its execution contexts here.
 * Play build (TOTK_PLAY, no instruction counting): instruction-equivalents of host time, i.e. host
 * ticks x TOTK_GUEST_IPS_PER_CORE x host threads running guest code, so the dilation sees exactly the
 * host time the guest had (one host thread: guest work = real time / busy virtual cores, as the
 * counted model gave for an even instruction rate). The frame lock and idle fast-forward are
 * unchanged; a host fast enough for real time converges to the wall clock. */
#if TOTK_PLAY
static inline uint64_t boot_guest_progress(const BootContext*c){(void)c;return boot_guest_clock.play_progress;}
/* Lock held. Advance play_progress to host time `host` (19.2 MHz ticks). */
static inline void boot_guest_play_advance(uint64_t host){
    BootGuestClock*g=&boot_guest_clock;
    if(!g->play_host || host<=g->play_host){if(!g->play_host)g->play_host=host;return;}
    uint64_t dt=host-g->play_host;g->play_host=host;
    /* runners UINT32_MAX (TOTK_THREADS=host): every runnable guest core has a host thread: real time */
    uint64_t runners=g->play_runners==UINT32_MAX?(g->busy?g->busy:1):(g->play_runners?g->play_runners:1);
    __uint128_t num=(__uint128_t)dt*g->d.ips*runners+g->play_rem;
    g->play_progress+=(uint64_t)(num/BOOT_GUEST_HZ);g->play_rem=(uint64_t)(num%BOOT_GUEST_HZ);
}
/* Ordering stamp for the scheduler (least recently scheduled front, condition-variable FIFO). */
static inline uint64_t boot_order_now(const BootContext*c){(void)c;LARGE_INTEGER q;QueryPerformanceCounter(&q);return (uint64_t)q.QuadPart;}
#else
static inline uint64_t boot_guest_progress(const BootContext*c){return c->steps;}
static inline uint64_t boot_order_now(const BootContext*c){return c->steps;}
#endif
static inline int boot_guest_timed_state(uint32_t state){return state==4 || state==6 || state==7 || state==8;}
/* Virtual cores that currently have a runnable (or running) thread, 1..64. */
static inline unsigned boot_guest_busy_cores(const BootContext*c){
    if(!c->thread_count || c->thread_count>BOOT_THREAD_CAPACITY)return 1;
    uint64_t mask=0;
    for(unsigned i=0;i<c->thread_count;++i){const BootThread*t=&c->threads[i];
        if(t->state!=2)continue;
        unsigned core=t->schedule.affinity?t->schedule.active:t->core;
        mask|=core<64?UINT64_C(1)<<core:1;
    }
    unsigned n=(unsigned)__builtin_popcountll(mask);
    return n?n:1;
}
static inline void boot_guest_clock_init_locked(BootContext*c,uint64_t host){
    uint64_t ips=boot_guest_env_u64("TOTK_GUEST_IPS_PER_CORE",UINT64_C(1000000000));
    uint64_t slack_ms=boot_guest_env_u64("TOTK_GUEST_CLOCK_SLACK_MS",250);
    if(slack_ms>UINT64_C(100000000))slack_ms=UINT64_C(100000000);
    /* Continue a clock carried by the context (checkpoint, DLL hand-over); a fresh or legacy
     * context starts at the legacy QPC tick, so deadlines it already holds stay meaningful. */
    uint64_t start=c->clock_test_ticks?c->clock_test_ticks:host;
    boot_dilation_init(&boot_guest_clock.d,start,host,boot_guest_progress(c),ips,slack_ms*(BOOT_GUEST_HZ/1000));
    boot_guest_clock.published=start;boot_guest_clock.busy=boot_guest_busy_cores(c);
    boot_guest_clock.busy_progress=boot_guest_progress(c);boot_guest_clock.initialized=1;
}
static inline int boot_guest_vsync_slots(const BootContext*c){
    for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==200 && c->event_native_handles[i])return 1;
    return 0;
}
/* Fire guest-timed host events due at `now` (lock held, dilated mode). */
static inline void boot_guest_service_locked(BootContext*c,uint64_t now){
    boot_hid_npad_tick(c,now/(BOOT_GUEST_HZ/1000));   /* controller input: guest ms, not IPC-driven */
    if(boot_guest_clock.timer_count)boot_guest_timers_fire_locked(now);
    if(!boot_guest_vsync_slots(c)){boot_guest_clock.vsync_next=0;return;}
    if(!boot_guest_clock.vsync_next)boot_guest_clock.vsync_next=now+BOOT_GUEST_VSYNC_TICKS;
    if(now<boot_guest_clock.vsync_next)return;
    for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==200 && c->event_native_handles[i])
        SetEvent((HANDLE)(uintptr_t)c->event_native_handles[i]);
    ++boot_guest_clock.vsync_fired;
    /* Missed periods coalesce into one signal (manual-reset event), like the DWM source did. */
    boot_guest_clock.vsync_next+=((now-boot_guest_clock.vsync_next)/BOOT_GUEST_VSYNC_TICKS+1)*BOOT_GUEST_VSYNC_TICKS;
}
static inline void boot_guest_stats_dump(const BootContext*c){
    unsigned states[10]={0};
    for(unsigned i=0;i<c->thread_count && i<BOOT_THREAD_CAPACITY;++i)++states[c->threads[i].state<10?c->threads[i].state:9];
    boot_time_stats_dump(boot_guest_progress(c),states);
}
static inline void boot_guest_stats_poll(const BootContext*c){
    if(!boot_guest_clock.stats || (++boot_guest_clock.updates&1023))return;
    ULONGLONG ms=GetTickCount64();
    if(!boot_time_stats.start_ms)boot_time_stats.start_ms=ms;
    if(ms-boot_time_stats.dump_ms<30000)return;
    boot_time_stats.dump_ms=ms;boot_guest_stats_dump(c);
}
/* Update the clock for a guest that is running (or, idle!=0, for an all-blocked guest heading for
 * `deadline`, 0 = none). Returns the guest tick in *out; *reached reports deadline reached. */
static inline int boot_guest_clock_update(BootContext*c,int idle,uint64_t deadline,uint64_t*out,int*reached){
    uint32_t mode=boot_guest_clock_mode();uint64_t host;
    if(reached)*reached=0;
    if(!boot_guest_host_ticks(&host))return 0;
    if(mode!=BOOT_GUEST_CLOCK_DILATED){
        boot_guest_clock.published=host;*out=host;boot_guest_stats_poll(c);
        boot_hid_npad_tick(c,GetTickCount64());   /* wall mode: controller input on wall ms, as before */
        return 1;
    }
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    if(!boot_guest_clock.initialized)boot_guest_clock_init_locked(c,host);
#if TOTK_PLAY
    boot_guest_play_advance(host);
#endif
    uint64_t progress=boot_guest_progress(c);
    if(idle || progress-boot_guest_clock.busy_progress>=4096 || progress<boot_guest_clock.busy_progress){
        boot_guest_clock.busy=boot_guest_busy_cores(c);boot_guest_clock.busy_progress=progress;
    }
    int hit=0;
    if(idle)hit=boot_dilation_idle(&boot_guest_clock.d,host,progress,boot_guest_clock.busy,deadline);
    else boot_dilation_run(&boot_guest_clock.d,host,progress,boot_guest_clock.busy);
    uint64_t now=boot_guest_clock.d.ticks;
    boot_guest_clock.published=now;c->clock_test_ticks=now;
    boot_guest_service_locked(c,now);
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    if(boot_present_hold.sweep){int render=boot_render_trylock();if(render>=0){boot_present_sweep(c);boot_render_unlock(render);}}   /* held buffers of another DLL (host_present_hold.h) */
    boot_guest_stats_poll(c);
    if(reached)*reached=hit;
    *out=now;return 1;
}
/* Guest tick for a running guest (system tick reads, timeout deadlines). */
static inline int boot_guest_clock_now(BootContext*c,uint64_t*out){return boot_guest_clock_update(c,0,0,out,NULL);}
/* Earliest guest deadline: timed thread waits, guest timers, vsync. 0 = none. */
static inline uint64_t boot_guest_next_deadline(const BootContext*c){
    uint64_t next=0;
    for(unsigned i=0;i<c->thread_count && i<BOOT_THREAD_CAPACITY;++i){const BootThread*t=&c->threads[i];
        if(boot_guest_timed_state(t->state) && t->wait_native[2] && (!next || t->wait_native[2]<next))next=t->wait_native[2];
    }
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    uint64_t timer=boot_guest_timers_next_locked(),vsync=boot_guest_vsync_slots(c)?boot_guest_clock.vsync_next:0;
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    if(timer && (!next || timer<next))next=timer;
    if(vsync && (!next || vsync<next))next=vsync;
    return next;
}
/* Every guest thread is blocked. Dilated mode: advance the guest clock towards the next deadline
 * (idle fast-forward within the credit) and fire due guest-timed events; returns 1 when the caller
 * should re-check its waits immediately instead of sleeping. Wall mode: 0 (sleep). */
static inline int boot_guest_clock_idle(BootContext*c){
    if(!boot_guest_dilated())return 0;
    uint64_t now,deadline=boot_guest_next_deadline(c);int reached=0;
    if(!boot_guest_clock_update(c,1,deadline,&now,&reached))return 0;
    return reached;
}
/* Guest steady clock seconds and calendar (unix) seconds. Dilated: calendar = unix time at the first
 * query minus guest seconds then, plus guest seconds now (a slow-motion calendar consistent with
 * the tick, which the time client uses to extrapolate). Wall: legacy QPC seconds and host UTC. */
static inline int boot_guest_steady_unix(BootContext*c,uint64_t*steady,int64_t*unix_seconds){
    FILETIME filetime;GetSystemTimeAsFileTime(&filetime);
    uint64_t file_ticks=((uint64_t)filetime.dwHighDateTime<<32)|filetime.dwLowDateTime;
    int64_t real_unix=(int64_t)(file_ticks/UINT64_C(10000000))-INT64_C(11644473600);
    if(!boot_guest_dilated()){
        LARGE_INTEGER counter,frequency;
        if(!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || counter.QuadPart<0 || frequency.QuadPart<=0)return 0;
        *steady=(uint64_t)counter.QuadPart/(uint64_t)frequency.QuadPart;*unix_seconds=real_unix;return 1;
    }
    uint64_t now;if(!boot_guest_clock_now(c,&now))return 0;
    uint64_t seconds=now/BOOT_GUEST_HZ;
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    if(!boot_guest_clock.unix_valid){boot_guest_clock.unix_offset=real_unix-(int64_t)seconds;boot_guest_clock.unix_valid=1;}
    int64_t offset=boot_guest_clock.unix_offset;
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    *steady=seconds;*unix_seconds=offset+(int64_t)seconds;return 1;
}

/* ---- Statistics hooks ---- */
/* Non-faulting guest read for frame walks; `slot` selects a remembered region (one per guest
 * thread: its stack), so the region table is scanned only when a stack is first seen. */
static inline int boot_time_read_u64(const BootContext*c,unsigned slot,uint64_t a,uint64_t*out){
    static unsigned hints[BOOT_THREAD_CAPACITY+1];
    unsigned*hint=&hints[slot<BOOT_THREAD_CAPACITY?slot:BOOT_THREAD_CAPACITY];
    if(*hint<c->region_count){const Region*r=&c->regions[*hint];
        if(r->data && a>=r->base && a-r->base<r->size && r->size-(a-r->base)>=8){memcpy(out,r->data+(size_t)(a-r->base),8);return 1;}}
    for(unsigned i=0;i<c->region_count && i<BOOT_REGION_CAPACITY;++i){const Region*r=&c->regions[i];
        if(r->data && a>=r->base && a-r->base<r->size && r->size-(a-r->base)>=8){*hint=i;memcpy(out,r->data+(size_t)(a-r->base),8);return 1;}}
    return 0;
}
/* frames[0] = link register, then return addresses along the frame-pointer chain. */
static inline void boot_time_frames(const BootContext*c,unsigned slot,uint64_t lr,uint64_t fp,uint64_t frames[4]){
    frames[0]=lr;frames[1]=frames[2]=frames[3]=0;
    for(unsigned k=1;k<4;++k){uint64_t next=0,ret=0;
        if(!fp || (fp&7) || !boot_time_read_u64(c,slot,fp,&next) || !boot_time_read_u64(c,slot,fp+8,&ret))break;
        frames[k]=ret;if(next<=fp)break;fp=next;
    }
}
static inline void boot_time_current(const BootContext*c,unsigned kind,uint64_t ticks){
    if(!boot_guest_clock.stats)return;
    uint64_t frames[4];boot_time_frames(c,c->current_thread,c->x[30],c->x[29],frames);
    uint32_t state=c->thread_count && c->current_thread<c->thread_count?c->threads[c->current_thread].state:0;
    boot_time_count(kind,frames,state,ticks);
}
/* Tick reads can be very frequent: one in 16 is attributed to its site (weight 16). */
static inline void boot_time_tick_read(uint64_t pc,uint64_t lr){
    static unsigned sample;
    if(!boot_guest_clock.stats || (++sample&15))return;
    uint64_t frames[4]={pc,lr,0,0};boot_time_count_weighted(BOOT_TS_TICK_READ,frames,0,0,16);
}
/* A timed wait (sleep, address, condition variable) ended by its deadline. */
static inline void boot_time_expired(const BootContext*c,unsigned index,uint64_t now){
    if(!boot_guest_clock.stats || index>=BOOT_THREAD_CAPACITY)return;
    const BootThread*t=&c->threads[index];BootTimeShadow*s=&boot_time_stats.shadow[index];
    uint64_t frames[4];
    if(s->deadline==t->wait_native[2] && s->state==t->state)memcpy(frames,s->frames,sizeof(frames));
    else boot_time_frames(c,index,t->x[30],t->x[29],frames);
    boot_time_count(t->state==8?BOOT_TS_SLEEP_WAKE:BOOT_TS_TIMEOUT,frames,t->state,now-t->wait_native[2]);
    s->expired=1;
}
/* Track wait transitions: new timed/infinite waits (with requested length), and timed waits that
 * ended before their deadline (signalled). Called where waits are expired. */
static inline void boot_time_observe(const BootContext*c,uint64_t now){
    if(!boot_guest_clock.stats)return;
    for(unsigned i=0;i<c->thread_count && i<BOOT_THREAD_CAPACITY;++i){
        const BootThread*t=&c->threads[i];BootTimeShadow*s=&boot_time_stats.shadow[i];
        /* Tracked: address (6), condition variable (7) and sleep (8) waits, and a timed condition
         * waiter requeued on its mutex (4 with the deadline it kept). */
        int waiting=t->state==6 || t->state==7 || t->state==8 || (t->state==4 && t->wait_native[2]);
        uint64_t deadline=waiting?t->wait_native[2]:0;
        if(waiting && s->state==t->state && s->deadline==deadline)continue;
        if(!waiting && !s->state)continue;
        /* The previous timed wait ended without expiring: it was signalled (a condition wait
         * moved to its mutex queue counts as signalled). Sleeps only end by expiry. */
        if(s->state && s->state!=4 && s->state!=8 && s->deadline && !s->expired)
            boot_time_count(BOOT_TS_SIGNALLED,s->frames,s->state,s->deadline>now?s->deadline-now:0);
        int requeued=waiting && t->state==4 && s->state==7 && s->deadline==deadline;
        if(waiting && !requeued){
            boot_time_frames(c,i,t->x[30],t->x[29],s->frames);
            unsigned kind=t->state==8?BOOT_TS_SLEEP_BEGIN:deadline?BOOT_TS_TIMED_WAIT:BOOT_TS_INFINITE_WAIT;
            boot_time_count(kind,s->frames,t->state,deadline>now?deadline-now:0);
        }
        s->expired=0;s->state=waiting?t->state:0;s->deadline=deadline;
    }
}
#endif
