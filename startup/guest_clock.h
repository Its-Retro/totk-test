#ifndef TOTK_GUEST_CLOCK_H
#define TOTK_GUEST_CLOCK_H
/* One guest clock for every guest-visible time source (CNTPCT_EL0 / system tick, svc timeouts and
 * sleeps, audio renderer completion, display vsync, HID sampling, time services, GPU report
 * timestamps). Units: Switch system ticks, 19.2 MHz.
 *
 * Modes (env TOTK_GUEST_CLOCK): "wall" = legacy, the tick is host QPC scaled to 19.2 MHz and the
 * host-side timers keep their real-time behaviour; anything else (default) = DILATED:
 *
 *   guest time advances by min(real time, guest work) where guest work is the number of guest
 *   instructions executed (all guest threads; one host thread runs them in turn) divided by
 *   (TOTK_GUEST_IPS_PER_CORE, default 1e9) x (virtual cores with a runnable thread, 1..3).
 *
 * A slow host therefore looks like a slow-motion Switch (each guest thread gets about as much work
 * done per guest second as on hardware), and a host that is fast enough runs in real time. Real time
 * the guest did not use is kept as credit, capped at TOTK_GUEST_CLOCK_SLACK_MS (default 250 ms); a
 * running guest may spend credit (a short catch-up burst after a slow phase), and when every guest
 * thread is blocked the clock jumps to the next guest deadline (timed wait, sleep, guest-timed host
 * event) as far as the credit allows, otherwise it passes at real rate. The clock never runs
 * backwards and never gets ahead of real time by more than the slack.
 *
 * FRAME LOCK (dilated mode with present hold, default on; TOTK_FRAME_LOCK=0 disables): when the host
 * is slower than real time, every presented frame advances guest time by the same whole number of
 * vsync periods (the game's frame period F, learned from its own release deadlines), so animation
 * steps are even. At each QueueBuffer (boot_guest_present_deadline) the pacer
 *   - sets a LIMIT: while guest threads run, guest time may not pass the release deadline of the
 *     buffer just queued plus (F - interval) periods: the next frame is then queued in time for its
 *     own release one frame period later (no slip), and with two display buffers it starts exactly
 *     on that release (frame starts stay F periods apart even for frames heavier than the budget).
 *     Work beyond the limit is dropped. All-blocked idle waits may cross it (the game waits on
 *     purpose, e.g. for a vsync: a deliberate slip); a running guest that keeps working at the
 *     limit for TOTK_FRAME_STRETCH (default 4) frame periods worth of work lapses it (no deadlock
 *     if the game needs time to pass, e.g. a timed spin, or presents rarely while loading).
 *   - sets a SCALE <= 1 for guest time per unit of work: the recent frames' work (fast up, slow
 *     decay) is mapped onto 3/4 of a frame period, so a heavy frame's sub-frame events (audio
 *     frames, timers, HID samples) spread over the frame instead of bunching at the limit. A host
 *     whose frames fit keeps scale 1 (never faster than 1 GIPS per busy core).
 * Light frames still finish early and wait (idle fast-forward) for their release deadline, so frame
 * starts stay on the vsync grid. Real time still bounds everything: a fast host converges to wall
 * clock pacing (the credit, not the limit, decides).
 *
 * Host-side real time (audio output devices, host window vsync/present pacing, wall-clock
 * diagnostics, process watchdogs) is not affected: only guest-visible values come from here.
 *
 * Everything here is process-global (selectany: one instance per DLL, like boot_tlb) and outside
 * BootContext, so no ABI or checkpoint layout changes. The current guest tick is mirrored into
 * BootContext.clock_test_ticks while clock_mode==1 (unused in that mode before), so checkpoints and
 * DLL hand-overs carry the guest clock forward (guest_time.h). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_GUEST_HZ UINT64_C(19200000)
#define BOOT_GUEST_VSYNC_TICKS UINT64_C(320000)    /* 60 Hz */
#define BOOT_GUEST_AUDIO_TICKS UINT64_C(96000)     /* 5 ms audio renderer frame */
#define BOOT_GUEST_TIMER_CAPACITY 64
enum { BOOT_GUEST_CLOCK_UNSET=0, BOOT_GUEST_CLOCK_WALL=1, BOOT_GUEST_CLOCK_DILATED=2 };

/* ---- Pure dilation core: no OS calls, unit tested directly (tests/test_guest_clock.c). ---- */
typedef struct BootDilation {
    uint64_t ticks;          /* guest ticks */
    uint64_t host_last;      /* host time of the last update, 19.2 MHz ticks */
    uint64_t progress_last;  /* guest instructions at the last update */
    uint64_t remainder;      /* fractional work, in instruction*Hz units below ips*busy */
    uint64_t credit;         /* unused real time, <= slack */
    uint64_t slack,ips;      /* configuration */
    /* statistics (ticks) */
    uint64_t real_total,work_advance,idle_advance,skip_advance,dropped_work;
    /* frame lock (BootFramePacer): 0 limit = none; scale Q16 (65536 = 1.0, 0 is read as 1.0) */
    uint64_t limit,scale,stretch,pinned,frame_work,pinned_total,lapses;
} BootDilation;
#define BOOT_DILATION_ONE UINT64_C(65536)

static inline void boot_dilation_init(BootDilation*d,uint64_t ticks,uint64_t host_now,uint64_t progress,uint64_t ips,uint64_t slack){
    memset(d,0,sizeof(*d));
    d->ticks=ticks;d->host_last=host_now;d->progress_last=progress;d->ips=ips?ips:1;d->slack=slack;
}
/* Accrue real time since the last update as credit; returns the real interval. */
static inline uint64_t boot_dilation_real(BootDilation*d,uint64_t host_now){
    uint64_t dreal=host_now>d->host_last?host_now-d->host_last:0;
    if(host_now>d->host_last)d->host_last=host_now;
    d->real_total+=dreal;
    uint64_t room=d->credit<d->slack?d->slack-d->credit:0;
    d->credit=dreal>=room?d->slack:d->credit+dreal;
    return dreal;
}
/* A running guest: advance by the work done since the last update, limited by the credit. Work that
 * real time cannot cover (the host is faster than the Switch) is dropped, not banked. */
static inline uint64_t boot_dilation_run(BootDilation*d,uint64_t host_now,uint64_t progress,unsigned busy){
    uint64_t dreal=boot_dilation_real(d,host_now);
    if(progress<d->progress_last){d->progress_last=progress;d->remainder=0;}   /* new context */
    uint64_t steps=progress-d->progress_last;d->progress_last=progress;
    uint64_t scale=d->scale && d->scale<BOOT_DILATION_ONE?d->scale:BOOT_DILATION_ONE;
    __uint128_t per=(__uint128_t)d->ips*(busy?busy:1);
    __uint128_t raw=(__uint128_t)steps*BOOT_GUEST_HZ;
    __uint128_t unscaled=raw/per;
    d->frame_work+=unscaled>UINT64_MAX/4?UINT64_MAX/4:(uint64_t)unscaled;
    __uint128_t num=raw*scale+d->remainder,den=per*BOOT_DILATION_ONE;
    __uint128_t work=num/den;d->remainder=(uint64_t)(num%den);
    uint64_t advance=work>d->credit?d->credit:(uint64_t)work;
    if(work>advance){__uint128_t lost=work-advance;d->dropped_work+=lost>UINT64_MAX/2?UINT64_MAX/2:(uint64_t)lost;d->remainder=0;}
    if(d->limit && advance){   /* frame lock: a running guest stops at the limit */
        uint64_t room=d->limit>d->ticks?d->limit-d->ticks:0;
        if(advance>room){
            uint64_t over=advance-room;advance=room;d->pinned_total+=over;d->remainder=0;
            d->pinned+=(uint64_t)(((__uint128_t)over*BOOT_DILATION_ONE)/scale);   /* in work (unscaled) ticks */
            if(d->stretch && d->pinned>d->stretch){d->limit=0;++d->lapses;}   /* lapse: free run until the next frame */
        }
    }
    if(advance>UINT64_MAX-d->ticks)advance=UINT64_MAX-d->ticks;
    d->ticks+=advance;d->credit-=advance;d->work_advance+=advance;
    return dreal;
}
/* Every guest thread is blocked. With a deadline (nonzero), jump towards it as far as the credit
 * allows and return 1 once it is reached. Without one, guest time passes at the real rate of this
 * interval. */
static inline int boot_dilation_idle(BootDilation*d,uint64_t host_now,uint64_t progress,unsigned busy,uint64_t deadline){
    uint64_t dreal=boot_dilation_run(d,host_now,progress,busy);
    uint64_t want=deadline?(deadline>d->ticks?deadline-d->ticks:0):dreal;
    uint64_t advance=want>d->credit?d->credit:want;
    if(advance>UINT64_MAX-d->ticks)advance=UINT64_MAX-d->ticks;
    d->ticks+=advance;d->credit-=advance;
    if(advance>dreal){d->skip_advance+=advance-dreal;d->idle_advance+=dreal;}else d->idle_advance+=advance;
    /* An all-blocked guest may wait past the frame limit (a deliberate slip): the limit lapses. */
    if(d->limit && d->ticks>d->limit){d->limit=0;++d->lapses;}
    return deadline && d->ticks>=deadline;
}

/* ---- Frame pacer (pure; tests/test_guest_clock.c) ----
 * Fed at every QueueBuffer with the buffer's release deadline (the chain: one buffer per swap
 * interval). Learns the game's frame period F (vsync periods between consecutive releases: the most
 * frequent of the last 16, ties to the shorter, never below the swap interval) and re-arms the
 * dilation's limit, scale and stretch for the next frame. */
#define BOOT_PACER_HISTORY 16
typedef struct BootFramePacer {
    uint64_t last_release,work_avg;
    uint64_t frames,discontinuities;
    uint32_t hist[BOOT_PACER_HISTORY],hist_count,hist_pos,period_count;
    uint32_t stretch_frames,enabled;
} BootFramePacer;
static inline unsigned boot_pacer_period(const BootFramePacer*p,unsigned interval){
    unsigned counts[9]={0},best=0;
    for(unsigned i=0;i<p->hist_count;++i)if(p->hist[i]>=1 && p->hist[i]<=8)++counts[p->hist[i]];
    for(unsigned f=1;f<=8;++f)if(counts[f]>counts[best])best=f;
    if(!best)best=interval?interval:1;
    return best<interval?interval:best;
}
/* Returns the frame's work (unscaled guest ticks) for statistics. */
static inline uint64_t boot_pacer_frame(BootFramePacer*p,BootDilation*d,uint64_t release,unsigned interval,uint64_t period){
    if(!interval)interval=1;
    uint64_t work=d->frame_work;d->frame_work=0;++p->frames;
    if(p->last_release && release>p->last_release){
        uint64_t inc=(release-p->last_release+period/2)/period;
        if(inc>=1 && inc<=8){p->hist[p->hist_pos]=(uint32_t)inc;p->hist_pos=(p->hist_pos+1)%BOOT_PACER_HISTORY;
            if(p->hist_count<BOOT_PACER_HISTORY)++p->hist_count;}
        else {++p->discontinuities;work=0;p->work_avg=0;}   /* a long gap (loading): not a frame */
    }
    p->last_release=release;
    unsigned f=boot_pacer_period(p,interval);p->period_count=f;
    if(!p->enabled){d->limit=0;d->scale=BOOT_DILATION_ONE;d->stretch=0;d->pinned=0;return work;}
    /* work estimate: follows a heavier frame at once, decays by 1/8 per lighter frame */
    p->work_avg=work>p->work_avg?work:p->work_avg-(p->work_avg-work)/8;
    uint64_t frame=(uint64_t)f*period,budget=frame-frame/4;
    uint64_t scale=BOOT_DILATION_ONE;
    if(p->work_avg>budget){scale=(uint64_t)(((__uint128_t)budget*BOOT_DILATION_ONE)/p->work_avg);if(scale<BOOT_DILATION_ONE/256)scale=BOOT_DILATION_ONE/256;}
    d->scale=scale;
    d->limit=release+(uint64_t)(f-interval)*period;
    d->stretch=(uint64_t)(p->stretch_frames?p->stretch_frames:4)*frame;d->pinned=0;
    return work;
}

/* ---- Process-global clock ---- */
typedef struct BootGuestTimer { uint64_t deadline; HANDLE event; } BootGuestTimer;
typedef struct BootGuestClock {
    SRWLOCK lock;
    uint32_t mode,initialized,stats,busy;
    uint64_t busy_progress,updates,reads,frequency;
    BootDilation d;
    volatile uint64_t published;        /* last guest tick (readable from any host thread) */
    uint64_t gpu_last;                  /* last GPU timestamp handed out (strictly increasing) */
    int64_t unix_offset;uint32_t unix_valid,timer_count;
    uint64_t vsync_next,vsync_fired,audio_fired;
    BootGuestTimer timers[BOOT_GUEST_TIMER_CAPACITY];
    /* frame lock (see the top of this file) and its statistics */
    BootFramePacer pacer;uint32_t pacer_configured;
    uint64_t frame_guest_last,frame_host_last,frame_hist[33],frame_real_sum,frame_real_sq,frame_real_n;
    /* play build (TOTK_PLAY): guest work measured by the host clock instead of instruction counts
     * (guest_time.h boot_guest_play_advance); runners = host threads executing guest code (0 = 1) */
    uint64_t play_progress,play_host,play_rem;uint32_t play_runners;
} BootGuestClock;
__declspec(selectany) BootGuestClock boot_guest_clock={0};

static inline uint64_t boot_guest_env_u64(const char*name,uint64_t fallback){
    const char*v=getenv(name);if(!v || !*v)return fallback;
    char*end=NULL;unsigned long long x=strtoull(v,&end,10);
    return end && !*end?(uint64_t)x:fallback;
}
/* Mode is read once per process (DLL); callers hold no lock. */
static inline uint32_t boot_guest_clock_mode(void){
    uint32_t mode=boot_guest_clock.mode;
    if(!mode){
        const char*v=getenv("TOTK_GUEST_CLOCK");
        mode=v && !_stricmp(v,"wall")?BOOT_GUEST_CLOCK_WALL:BOOT_GUEST_CLOCK_DILATED;
        const char*s=getenv("TOTK_TIME_STATS");
        boot_guest_clock.stats=!(s && !strcmp(s,"0"));
        boot_guest_clock.mode=mode;
    }
    return mode;
}
static inline int boot_guest_dilated(void){return boot_guest_clock_mode()==BOOT_GUEST_CLOCK_DILATED;}
/* Host time as 19.2 MHz ticks (the legacy guest tick). */
static inline int boot_guest_host_ticks(uint64_t*out){
    LARGE_INTEGER counter,frequency;
    if(!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
       counter.QuadPart<0 || frequency.QuadPart<=0)return 0;
    __uint128_t ticks=(__uint128_t)(uint64_t)counter.QuadPart*BOOT_GUEST_HZ/(uint64_t)frequency.QuadPart;
    if(ticks>UINT64_MAX)return 0;
    *out=(uint64_t)ticks;return 1;
}
/* Guest ticks <-> guest nanoseconds (Switch kernel rounding: ns->ticks rounds up). */
static inline uint64_t boot_guest_ns_to_ticks(uint64_t ns){
    __uint128_t t=((__uint128_t)ns*BOOT_GUEST_HZ+UINT64_C(999999999))/UINT64_C(1000000000);
    return t>UINT64_MAX?UINT64_MAX:(uint64_t)t;
}
static inline uint64_t boot_guest_ticks_to_ns(uint64_t ticks){
    __uint128_t ns=(__uint128_t)ticks*UINT64_C(1000000000)/BOOT_GUEST_HZ;
    return ns>UINT64_MAX?UINT64_MAX:(uint64_t)ns;
}
/* Guest-timed host events (dilated mode): the event is signalled when the guest clock reaches the
 * deadline. The timer owns `event` (a duplicate) and closes it after signalling. */
static inline int boot_guest_timer_add(HANDLE event,uint64_t delay){
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    int ok=boot_guest_clock.timer_count<BOOT_GUEST_TIMER_CAPACITY;
    if(ok){uint64_t now=boot_guest_clock.published;
        boot_guest_clock.timers[boot_guest_clock.timer_count++]=(BootGuestTimer){now>UINT64_MAX-delay?UINT64_MAX:now+delay,event};}
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    return ok;
}
/* Same with an absolute guest deadline (a deadline already passed fires at the next clock update). */
static inline int boot_guest_timer_at(HANDLE event,uint64_t deadline){
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    int ok=boot_guest_clock.timer_count<BOOT_GUEST_TIMER_CAPACITY;
    if(ok)boot_guest_clock.timers[boot_guest_clock.timer_count++]=(BootGuestTimer){deadline,event};
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    return ok;
}
/* A frame was queued with release `deadline` (lock held, dilated mode): re-arm the frame lock and
 * record its guest and real duration (guest-time-stats.txt; one line per frame in guest-frames.log
 * while statistics are on: real ms, guest ms, frame period F, work ms, scale, limit ms ahead). */
static inline void boot_guest_frame_locked(uint64_t deadline,unsigned interval){
    BootFramePacer*p=&boot_guest_clock.pacer;
    if(!boot_guest_clock.pacer_configured){
        const char*v=getenv("TOTK_FRAME_LOCK");p->enabled=!(v && !strcmp(v,"0"));
        p->stretch_frames=(uint32_t)boot_guest_env_u64("TOTK_FRAME_STRETCH",4);
        boot_guest_clock.pacer_configured=1;
    }
    uint64_t work=boot_pacer_frame(p,&boot_guest_clock.d,deadline,interval,BOOT_GUEST_VSYNC_TICKS);
    uint64_t now=boot_guest_clock.published,host=0;boot_guest_host_ticks(&host);
    uint64_t guest=boot_guest_clock.frame_guest_last && now>=boot_guest_clock.frame_guest_last?now-boot_guest_clock.frame_guest_last:0;
    uint64_t real=boot_guest_clock.frame_host_last && host>=boot_guest_clock.frame_host_last?host-boot_guest_clock.frame_host_last:0;
    if(boot_guest_clock.frame_guest_last){
        uint64_t quarter=guest/(BOOT_GUEST_VSYNC_TICKS/4);++boot_guest_clock.frame_hist[quarter<32?quarter:32];
        double r=(double)real/1.92e4;boot_guest_clock.frame_real_sum+=(uint64_t)r;boot_guest_clock.frame_real_sq+=(uint64_t)(r*r);++boot_guest_clock.frame_real_n;
    }
    boot_guest_clock.frame_guest_last=now;boot_guest_clock.frame_host_last=host;
    if(boot_guest_clock.stats){FILE*f=fopen("guest-frames.log","a");
        if(f){fprintf(f,"%.1f %.2f F%u i%u work %.2f scale %.3f limit %+.2f%s\n",(double)real/1.92e4,(double)guest/1.92e4,p->period_count,interval,
                      (double)work/1.92e4,(double)boot_guest_clock.d.scale/65536.0,
                      boot_guest_clock.d.limit?((double)boot_guest_clock.d.limit-(double)now)/1.92e4:0.0,p->enabled?"":" (lock off)");fclose(f);}}
}
/* Release tick of a buffer queued to the display now (host_present_hold.h): shown from the next
 * guest vsync for `interval` periods, and not before the previously queued buffer (*chain) has had
 * its own interval. Updates *chain and the frame lock. */
static inline uint64_t boot_guest_present_deadline(uint64_t*chain,unsigned interval){
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    uint64_t now=boot_guest_clock.published,period=BOOT_GUEST_VSYNC_TICKS;
    uint64_t next=boot_guest_clock.vsync_next>now?boot_guest_clock.vsync_next:now+period;
    uint64_t deadline=next+(uint64_t)(interval?interval-1:0)*period;
    if(*chain && *chain<UINT64_MAX-(uint64_t)interval*period && *chain+(uint64_t)interval*period>deadline)deadline=*chain+(uint64_t)interval*period;
    *chain=deadline;
    if(boot_guest_clock.mode==BOOT_GUEST_CLOCK_DILATED)boot_guest_frame_locked(deadline,interval);
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    return deadline;
}
/* Fire due timers (lock held). */
static inline void boot_guest_timers_fire_locked(uint64_t now){
    for(unsigned i=0;i<boot_guest_clock.timer_count;){
        BootGuestTimer*t=&boot_guest_clock.timers[i];
        if(t->deadline>now){++i;continue;}
        SetEvent(t->event);CloseHandle(t->event);++boot_guest_clock.audio_fired;
        boot_guest_clock.timers[i]=boot_guest_clock.timers[--boot_guest_clock.timer_count];
    }
}
static inline uint64_t boot_guest_timers_next_locked(void){
    uint64_t next=0;
    for(unsigned i=0;i<boot_guest_clock.timer_count;++i)
        if(!next || boot_guest_clock.timers[i].deadline<next)next=boot_guest_clock.timers[i].deadline;
    return next;
}
/* GPU report timestamp in GPU timer ticks (614.4 MHz = 32 x 19.2 MHz) from the guest clock,
 * strictly increasing (reports inside one submission are distinct). Dilated mode only. */
static inline uint64_t boot_guest_gpu_ticks(void){
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    uint64_t v=boot_guest_clock.published*32;
    if(v<=boot_guest_clock.gpu_last)v=boot_guest_clock.gpu_last+1;
    boot_guest_clock.gpu_last=v;
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    return v;
}
static inline uint64_t boot_guest_gpu_ns(void){
    /* ns such that ns*384/625 reproduces boot_guest_gpu_ticks (callers convert with 384/625). */
    uint64_t ticks=boot_guest_gpu_ticks();
    return (uint64_t)(((__uint128_t)ticks*625+383)/384);
}

/* ---- Instrumentation (counters by call site), on unless TOTK_TIME_STATS=0 ---- */
enum { BOOT_TS_TICK_READ, BOOT_TS_SLEEP_BEGIN, BOOT_TS_SLEEP_WAKE, BOOT_TS_YIELD, BOOT_TS_TIMED_WAIT,
       BOOT_TS_TIMEOUT, BOOT_TS_SIGNALLED, BOOT_TS_INFINITE_WAIT, BOOT_TS_WAITSYNC_READY,
       BOOT_TS_WAITSYNC_POLL_MISS, BOOT_TS_WAITSYNC_BLOCK, BOOT_TS_KINDS };
static const char*const boot_ts_names[BOOT_TS_KINDS]={"tick-read","sleep-begin","sleep-wake","yield","timed-wait",
    "TIMEOUT","signalled","infinite-wait","waitsync-ready","waitsync-poll-miss","waitsync-block"};
#define BOOT_TS_SITES 4096
typedef struct BootTimeSite { uint64_t key,frames[4],count,ticks_sum,ticks_max;uint32_t kind,state; } BootTimeSite;
typedef struct BootTimeShadow { uint64_t deadline,frames[4];uint32_t state,expired; } BootTimeShadow;
typedef struct BootTimeStats {
    uint64_t totals[BOOT_TS_KINDS],last_totals[BOOT_TS_KINDS];
    uint64_t dump_ms,start_ms,last_guest,last_host,last_steps,dropped_sites;
    BootTimeSite sites[BOOT_TS_SITES];
    BootTimeShadow shadow[256];
} BootTimeStats;
__declspec(selectany) BootTimeStats boot_time_stats={0};
/* Count `weight` events of `kind` at a call site (frames) for a thread state; ticks = a duration in
 * guest ticks (requested wait, lateness, time left). */
static inline void boot_time_count_weighted(unsigned kind,const uint64_t frames[4],uint32_t state,uint64_t ticks,uint64_t weight){
    if(!boot_guest_clock.stats || kind>=BOOT_TS_KINDS)return;
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    boot_time_stats.totals[kind]+=weight;
    uint64_t key=(frames[0]*UINT64_C(0x9e3779b97f4a7c15))^(frames[1]*UINT64_C(0xc2b2ae3d27d4eb4f))^((uint64_t)kind<<56|state);
    unsigned slot=(unsigned)((key>>20)^key)&(BOOT_TS_SITES-1);
    for(unsigned probe=0;probe<32;++probe,slot=(slot+1)&(BOOT_TS_SITES-1)){
        BootTimeSite*s=&boot_time_stats.sites[slot];
        if(s->count && (s->key!=key || s->kind!=kind || s->state!=state || s->frames[0]!=frames[0] || s->frames[1]!=frames[1]))continue;
        if(!s->count){s->key=key;s->kind=kind;s->state=state;memcpy(s->frames,frames,sizeof(s->frames));}
        s->count+=weight;s->ticks_sum+=ticks*weight;if(ticks>s->ticks_max)s->ticks_max=ticks;
        ReleaseSRWLockExclusive(&boot_guest_clock.lock);return;
    }
    ++boot_time_stats.dropped_sites;
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
}
static inline void boot_time_count(unsigned kind,const uint64_t frames[4],uint32_t state,uint64_t ticks){
    boot_time_count_weighted(kind,frames,state,ticks,1);
}
static int boot_ts_compare(const void*a,const void*b){
    const BootTimeSite*x=*(const BootTimeSite*const*)a,*y=*(const BootTimeSite*const*)b;
    return x->count<y->count?1:x->count>y->count?-1:0;
}
/* Writes guest-time-stats.txt (cumulative, top sites per kind) and appends one summary line to
 * guest-time-stats.log. Lock not held by the caller. */
static inline void boot_time_stats_dump(uint64_t steps,unsigned threads_by_state[10]){
    static BootTimeSite*order[BOOT_TS_SITES];
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    BootTimeStats*s=&boot_time_stats;BootDilation*d=&boot_guest_clock.d;
    uint64_t host=0;boot_guest_host_ticks(&host);
    uint64_t guest=boot_guest_clock.published,ms=GetTickCount64();
    double dh=s->last_host && host>s->last_host?(double)(host-s->last_host)/1.92e7:0;
    double dg=s->last_guest && guest>=s->last_guest?(double)(guest-s->last_guest)/1.92e7:0;
    double di=s->last_steps && steps>=s->last_steps?(double)(steps-s->last_steps):0;
    FILE*f=fopen("guest-time-stats.txt","w");
    if(f){
        fprintf(f,"mode %s  host interval %.1f s  guest interval %.3f s  guest/real %.5f  guest instr/s(real) %.0f  instr per guest s %.0f\n",
                boot_guest_clock.mode==BOOT_GUEST_CLOCK_DILATED?"dilated":"wall",dh,dg,dh>0?dg/dh:0,dh>0?di/dh:0,dg>0?di/dg:0);
        fprintf(f,"guest ticks %llu  credit %llu  busy cores %u  ips/core %llu  slack %llu\n",(unsigned long long)guest,
                (unsigned long long)d->credit,boot_guest_clock.busy,(unsigned long long)d->ips,(unsigned long long)d->slack);
        fprintf(f,"advance(s): work %.3f idle %.3f skipped %.3f dropped-work %.3f real %.3f  vsync fired %llu  audio timers fired %llu pending %u\n",
                (double)d->work_advance/1.92e7,(double)d->idle_advance/1.92e7,(double)d->skip_advance/1.92e7,(double)d->dropped_work/1.92e7,
                (double)d->real_total/1.92e7,(unsigned long long)boot_guest_clock.vsync_fired,(unsigned long long)boot_guest_clock.audio_fired,boot_guest_clock.timer_count);
        {const BootFramePacer*p=&boot_guest_clock.pacer;double n=(double)boot_guest_clock.frame_real_n;
            double mean=n>0?(double)boot_guest_clock.frame_real_sum/n:0,var=n>0?(double)boot_guest_clock.frame_real_sq/n-mean*mean:0;
            fprintf(f,"frame lock %s: frames %llu period F=%u vsyncs  scale %.3f  work avg %.2f ms  pinned %.3f s  lapses %llu  gaps %llu  real ms/frame mean %.1f sd %.1f\n",
                    p->enabled?"on":"off",(unsigned long long)p->frames,p->period_count,(double)d->scale/65536.0,(double)p->work_avg/1.92e4,
                    (double)d->pinned_total/1.92e7,(unsigned long long)d->lapses,(unsigned long long)p->discontinuities,mean,var>0?__builtin_sqrt(var):0.0);
            fprintf(f,"guest ms per frame (quarter-vsync bins, count):");
            for(unsigned i=0;i<33;++i)if(boot_guest_clock.frame_hist[i])fprintf(f," %.1f%s:%llu",(double)i*4.1667,i==32?"+":"",(unsigned long long)boot_guest_clock.frame_hist[i]);
            fprintf(f,"\n");}
        fprintf(f,"threads by state:");for(unsigned i=0;i<10;++i)fprintf(f," %u:%u",i,threads_by_state[i]);
        fprintf(f,"   (2 run, 4 mutex, 5 handles, 6 address, 7 condvar, 8 sleep)\nkind totals (cumulative / this interval per real s):\n");
        for(unsigned k=0;k<BOOT_TS_KINDS;++k)fprintf(f,"  %-20s %12llu  %10.1f/s\n",boot_ts_names[k],(unsigned long long)s->totals[k],
                                                     dh>0?(double)(s->totals[k]-s->last_totals[k])/dh:0);
        unsigned n=0;for(unsigned i=0;i<BOOT_TS_SITES;++i)if(s->sites[i].count)order[n++]=&s->sites[i];
        qsort(order,n,sizeof(order[0]),boot_ts_compare);
        fprintf(f,"sites (%u, dropped %llu): kind state count avg-ms max-ms frames(lr, fp chain...)\n",n,(unsigned long long)s->dropped_sites);
        for(unsigned k=0;k<BOOT_TS_KINDS;++k){unsigned shown=0;
            for(unsigned i=0;i<n && shown<(k==BOOT_TS_TICK_READ?24u:16u);++i){BootTimeSite*t=order[i];if(t->kind!=k)continue;++shown;
                fprintf(f,"  %-18s s%u %10llu %9.3f %9.3f  %llx %llx %llx %llx\n",boot_ts_names[k],t->state,(unsigned long long)t->count,
                        (double)t->ticks_sum/(double)t->count/19200.0,(double)t->ticks_max/19200.0,(unsigned long long)t->frames[0],
                        (unsigned long long)t->frames[1],(unsigned long long)t->frames[2],(unsigned long long)t->frames[3]);}}
        fclose(f);
    }
    FILE*l=fopen("guest-time-stats.log","a");
    if(l){fprintf(l,"%llu ms steps %llu guest/real %.5f instr/s %.0f",(unsigned long long)(ms-s->start_ms),(unsigned long long)steps,dh>0?dg/dh:0,dh>0?di/dh:0);
        for(unsigned k=0;k<BOOT_TS_KINDS;++k)fprintf(l," %s=%llu",boot_ts_names[k],(unsigned long long)(s->totals[k]-s->last_totals[k]));
        fprintf(l,"\n");fclose(l);}
    memcpy(s->last_totals,s->totals,sizeof(s->totals));s->last_host=host;s->last_guest=guest;s->last_steps=steps;
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
}
#endif
