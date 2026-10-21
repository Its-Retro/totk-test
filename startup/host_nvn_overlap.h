#ifndef TOTK_HOST_NVN_OVERLAP_H
#define TOTK_HOST_NVN_OVERLAP_H
/* overlap-work (play-work/overlap-work/DIAGNOSIS.md): what lets the game's next frame run while t2 translates and the
 * GPU executes this one. Included from host_nvn.h under TOTK_DRAW_PATH only (runtime_support): the generated page
 * header does not change. Everything here is inactive unless guest threads run on host threads (TOTK_THREADS=host).
 *
 * P3, frame-in-flight guard (on with host threads; off with TOTK_FRAME_GUARD=0, and with TOTK_OVERLAP=0 unless
 * TOTK_FRAME_GUARD=1). Each frame TOTK's GPU thread (t2) runs nvnQueueSubmitCommands, nvnQueueFenceSync on one sync
 * (the frame fence), Flush, Present and AcquireTexture; the main thread (t0) waits once per frame with nvnSyncWait on
 * that sync, then hands t2 the next frame (nvn-trace.bin, 2628 frames: only t2 submits and fences, only t0 waits). The
 * wait is meant for the frame t2 was handed last. On the Switch the submit is a cheap GPFIFO push, so t2's FenceSync
 * always comes first; here t2 translates the frame inside the submit (25-40 ms) and sleeps in the present pacing and
 * hold, and once job threads no longer stop on the window lock (host_window.h boot_window_is_closed) t0 can reach the
 * wait before that FenceSync, even before the submit begins. The sync then still holds the fence t0 already waited
 * for, the wait returned at once and the simulation ran a whole frame ahead into memory the untranslated frame still
 * reads (play-85: environment and UI flashing, a pink fill on the menu).
 * The guard keys on the sync and on the fence each waiter has consumed, not on who is inside a submit. A frame sync is
 * one fenced at least twice, its newest fence right after a submit by the fencing thread (the fencer, re-learnt at
 * every FenceSync). A wait by another thread on a frame sync whose newest fence that thread has already waited for (a
 * repeat wait) blocks, without the renderer lock and outside guest code, until the next FenceSync on the sync, then
 * reads it again. A wait that finds a fence it has not waited for yet, the fencer's own waits and every other sync are
 * unchanged. Give-up (counted; never a hang): none while the fencer is inside nvnQueueSubmitCommands (it holds the
 * renderer lock and never waits on guest threads, so it reaches its FenceSync; giving up inside a multi-second
 * PSO/texture hitch would let the simulation run into the frame); BOOT_FF_CAP_MS after its submit returned without a
 * FenceSync; BOOT_FF_CAP_MS after the first repeat wait of that fence while the fencer has not begun a submit (it may
 * itself be waiting for the waiter). After a give-up the later waits on that fence pass; three give-ups in a row with
 * no submit at all switch the before-submit stage off for that waiter for 32, 64, ... fences (logged). A poll
 * (timeout 0) that is a repeat wait answers TIMEOUT_EXPIRED until the next FenceSync or the give-up time; a caller's
 * finite timeout is honoured anywhere (TIMEOUT_EXPIRED, never a stale ALREADY_SIGNALED).
 * Why the first version (a frame in flight from the submitter's submit to its FenceSync, owner locked to the first
 * submitter) never fired in play-85: every host function without BOOT_HOST_FN_NOLOCK runs under the renderer lock
 * (host_fn.h boot_render_lock), which t2 holds for its whole submit, so t0's nvnSyncWait entered only after the submit
 * had returned, and t2 takes the lock again for its FenceSync a few instructions later (a critical section has no
 * hand-off): the stretch it watched was never observable. The waits that ran ahead start outside t2's submit.
 * Diagnostics (draw-path.log): the first 64 guard events ("frame guard decision"), the first 16 blocked waits ("frame
 * guard hit"), the first 8 give-ups; every 100 submissions the "frame overlap" line names the frame submitter and
 * waiter and a "frame guard:" line follows with the frame sync state and the decision counters.
 *
 * P4, early fence (opt-in, TOTK_EARLY_FENCE=1; nxport crates/engine/src/metal/mod.rs:4505-4596 --early-fence and
 * fence_guard.rs, MIT). The frame waiter's nvnSyncWait on the newest frame fence returns as soon as t2 has captured
 * the frame (its FenceSync kicked every list of it: all CPU-side reads of guest memory for the frame are done) and
 * the previous early-returned frame has completed on the GPU (at most one frame overlaps). What the GPU work of the
 * frame still touches in guest memory is then write-guarded (see boot_efg_*): pages it reads zero-copy become
 * PAGE_READONLY and pages it writes PAGE_NOACCESS in the fixed guest view (BOOT_FM_FIXED_BASE + guest address; the
 * GPU and the renderer use the separate section views D3D12 opened, so neither is affected). The first guest access
 * that would conflict faults; boot_efg_veh blocks that thread until the frame's fence has completed, restores the
 * fastmem protection of every guarded page (boot_fm_reprotect, from the page states) and retries the access.
 * Recorded per submission (t2, renderer lock held, GPU VA -> guest VA through the GPU mappings): vertex, uniform and
 * index buffers, indirect arguments and conditional-render counters (read), storage buffers, uniform-buffer updates,
 * buffer clears and reports (written), plus at the early return the renderer's pending GPU writes (boot_render_gw,
 * render-target write-backs boot_render_gt_pending) and the learnt store ranges of deferred compute dispatches. The
 * frame takes the ordinary full wait instead when the guard cannot cover it: pitch-linear targets waiting for their
 * CPU copy, GPU-write table overflow, a recording overflow or unbounded range, a failed protection, and every
 * TOTK_EARLY_FENCE_IDLE_EVERY-th frame (default 16, 0 = never) so the queue-idle housekeeping (upload release,
 * staging reset, texture trim) still runs. Other threads keep full waits.
 * NOT covered (documented risks): guest writes through region data pointers (kernel/host code using c->regions data
 * instead of the fixed view) and through a second guest alias of the same memory; buffer textures and shader program
 * data read zero-copy (not bound per draw: not recorded); stores of deferred compute outside its learnt ranges; GPU
 * VA ranges with no CPU mapping in w->gpu_mappings; fixed-view pages whose view is not mapped yet (skipped, counted);
 * a syscall (not a user-mode copy) touching a guarded page fails instead of faulting (none found: fs reads copy
 * through a host buffer, svc_fs_copy). The guest sees GPU results of the frame only after the fault wait, so reads
 * are never stale. TOTK_EARLY_FENCE_GUARD=0 turns the guard off (unguarded overlap, as nxport's A/B; unsafe). */

#define BOOT_FF_CAP_MS 250

/* ---------------------------------------------------------------- common */
static inline int boot_ov_threads(void) { return boot_mt.active && boot_mt.host_threads; }
/* TOTK_FRAME_GUARD=0 turns the frame-in-flight guard off (A/B only: the early-wait race of P3 comes back), =1 forces it
 * on; unset, it follows TOTK_OVERLAP (off with TOTK_OVERLAP=0, the serial play-84 behaviour, boot_mt_late.h). */
static inline int boot_ff_on(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_FRAME_GUARD", e, sizeof e); v = n && n < sizeof e ? e[0] != '0' : boot_mt_overlap_on(); }
    return v && boot_ov_threads();
}
static inline uint64_t boot_ov_qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
static inline uint64_t boot_ov_freq(void) {
    static uint64_t f; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart > 0 ? (uint64_t)q.QuadPart : 1; }
    return f;
}
/* Leave the recursive renderer lock completely (every level this thread holds) / take it back. */
static inline LONG boot_ov_cs_release(void) {
    CRITICAL_SECTION *cs = boot_mt.active ? boot_render_cs() : NULL; LONG depth = 0;
    if (cs && (DWORD)(uintptr_t)cs->OwningThread == GetCurrentThreadId()) { depth = cs->RecursionCount; for (LONG i = 0; i < depth; ++i) LeaveCriticalSection(cs); }
    return depth;
}
static inline void boot_ov_cs_reacquire(LONG depth) { if (depth) { CRITICAL_SECTION *cs = boot_render_cs(); for (LONG i = 0; i < depth; ++i) EnterCriticalSection(cs); } }
/* A blocked host function on a core context is outside guest code (a quiescing kernel need not wait for it); it comes
 * back through boot_mt_guest_enter, as boot_mt_peek_ptr does. */
static inline BootCore *boot_ov_leave_guest(BootContext *c) {
    BootCore *k = c && boot_mt.active && boot_mt_core_ctx(c) ? (BootCore *)(void *)c : NULL;
    if (k) __atomic_store_n(&k->in_guest, 0, __ATOMIC_SEQ_CST);
    return k;
}
static inline void boot_ov_enter_guest(BootCore *k) { if (k) boot_mt_guest_enter(k); }

/* ---------------------------------------------------------------- P3: frame-in-flight guard */
#define BOOT_FF_SYNCS 16
#define BOOT_FF_WAITERS 4
#define BOOT_FF_SUBMITTERS 8
#define BOOT_FF_LOG 64
enum { BOOT_FF_IDLE = 0, BOOT_FF_IN_SUBMIT = 1, BOOT_FF_SUBMITTED = 2 };   /* a submitter: fenced / inside a submit / submit returned */
static const char *const boot_ff_stage_name[3] = {"before its submit", "inside its submit", "after its submit, before its FenceSync"};
typedef struct BootFfWaiter { uint64_t tls, consumed, off_until; unsigned has, streak, offs; } BootFfWaiter;   /* consumed: a fence count */
typedef struct BootFfSync {
    uint64_t sync, fencer, value, fences, fence_qpc;   /* fences: FenceSyncs on the sync so far (the newest fence's number) */
    unsigned frame, suspended;                         /* frame: the newest fence came right after a submit by the fencer */
    uint64_t suspended_fence, hit_fence, hit_qpc;      /* hit_qpc: the first repeat wait on fence hit_fence */
    BootFfWaiter w[BOOT_FF_WAITERS];
} BootFfSync;
typedef struct BootFfSubmitter { uint64_t tls, begin_qpc, end_qpc; unsigned state, since_fence; } BootFfSubmitter;
typedef struct BootFrameGuard {
    SRWLOCK lock; CONDITION_VARIABLE cv;
    BootFfSync s[BOOT_FF_SYNCS]; unsigned ns;
    BootFfSubmitter sub[BOOT_FF_SUBMITTERS];
    uint64_t frame_owner, frame_sync, frame_value;   /* the newest frame fence: its fencer, sync and value (P4 reads these) */
    uint64_t waiter, log_qpc;                        /* waiter: the newest thread that waited on a frame sync (diagnostic) */
    volatile LONG64 waits, wait_qpc, gave_up, polls, expired, logged, hits_logged;
    volatile LONG64 hit[3], in_order, first, not_frame, own, after_give_up, rule_off, give_up_before, offs;
} BootFrameGuard;
static BootFrameGuard boot_ff;
/* The guest thread index of a TLS address (t0 = main, t2 = the GPU thread in TOTK), -1 unknown. */
static inline int boot_ff_tid(uint64_t tls) {
    const BootContext *M = boot_mt.master;
    if (M && tls) for (unsigned i = 0; i < M->thread_count && i < BOOT_THREAD_CAPACITY; ++i) if (M->threads[i].tpidrro == tls) return (int)i;
    return -1;
}
/* The first BOOT_FF_LOG guard events: formatted under the guard lock, written after it (boot_ff_emit). */
typedef struct BootFfNote { char text[288]; LONG64 n; } BootFfNote;
__attribute__((format(printf, 2, 3))) static inline void boot_ff_note(BootFfNote *note, const char *fmt, ...) {
    if (note->n || __atomic_load_n(&boot_ff.logged, __ATOMIC_RELAXED) >= BOOT_FF_LOG) return;
    LONG64 n = _InterlockedIncrement64(&boot_ff.logged);
    if (n > BOOT_FF_LOG) return;
    if (n == 1) boot_ff.log_qpc = boot_ov_qpc();
    va_list ap; va_start(ap, fmt); vsnprintf(note->text, sizeof note->text, fmt, ap); va_end(ap);
    note->n = n;
}
static inline void boot_ff_emit(const BootFfNote *note) {
    if (note->n) boot_draw_log("nvn native: frame guard decision%s (event %llu of the first 64, +%llu ms)", note->text, (uint64_t)note->n,
                               (boot_ov_qpc() - boot_ff.log_qpc) * 1000 / boot_ov_freq());
}
static inline BootFfSync *boot_ff_find_locked(uint64_t sync) {
    for (unsigned i = 0; i < boot_ff.ns; ++i) if (boot_ff.s[i].sync == sync) return &boot_ff.s[i];
    return NULL;
}
static inline BootFfSync *boot_ff_add_locked(uint64_t sync) {
    BootFfSync *e = boot_ff.ns < BOOT_FF_SYNCS ? &boot_ff.s[boot_ff.ns++] : NULL;
    if (!e) { e = &boot_ff.s[0]; for (unsigned i = 1; i < BOOT_FF_SYNCS; ++i) if (boot_ff.s[i].fence_qpc < e->fence_qpc) e = &boot_ff.s[i]; }   /* the longest unfenced */
    memset(e, 0, sizeof *e); e->sync = sync;
    return e;
}
static inline BootFfWaiter *boot_ff_waiter_locked(BootFfSync *e, uint64_t tls, int add) {
    BootFfWaiter *slot = NULL;
    for (unsigned i = 0; i < BOOT_FF_WAITERS; ++i) { if (e->w[i].tls == tls) return &e->w[i]; if (!e->w[i].tls && !slot) slot = &e->w[i]; }
    if (!add) return NULL;
    if (!slot) slot = &e->w[BOOT_FF_WAITERS - 1];   /* more waiters than slots: the last one is reused (it only forgets a consumed fence) */
    memset(slot, 0, sizeof *slot); slot->tls = tls;
    return slot;
}
static inline BootFfSubmitter *boot_ff_submitter_locked(uint64_t tls, int add) {
    BootFfSubmitter *pick = NULL;
    for (unsigned i = 0; i < BOOT_FF_SUBMITTERS; ++i) if (boot_ff.sub[i].tls == tls) return &boot_ff.sub[i];
    if (!add) return NULL;
    for (unsigned i = 0; i < BOOT_FF_SUBMITTERS && !pick; ++i) if (!boot_ff.sub[i].tls) pick = &boot_ff.sub[i];
    for (unsigned i = 0; i < BOOT_FF_SUBMITTERS && !pick; ++i) if (boot_ff.sub[i].state == BOOT_FF_IDLE) pick = &boot_ff.sub[i];
    if (!pick) pick = &boot_ff.sub[0];
    memset(pick, 0, sizeof *pick); pick->tls = tls;
    return pick;
}
static inline unsigned boot_ff_stage_locked(const BootFfSync *e) {
    BootFfSubmitter *s = boot_ff_submitter_locked(e->fencer, 0);
    return s ? s->state : BOOT_FF_IDLE;
}
/* The give-up of a repeat wait on e (lock held): none while the fencer is inside its submit, BOOT_FF_CAP_MS from the
 * submit's return after it, BOOT_FF_CAP_MS from the first repeat wait on this fence before it. 1 = reached; else
 * *until is when it will be (UINT64_MAX: none). */
static inline int boot_ff_capped_locked(const BootFfSync *e, uint64_t now, uint64_t cap, uint64_t *until) {
    BootFfSubmitter *s = boot_ff_submitter_locked(e->fencer, 0);
    unsigned stage = s ? s->state : BOOT_FF_IDLE;
    *until = UINT64_MAX;
    if (stage == BOOT_FF_IN_SUBMIT) return 0;
    uint64_t base = stage == BOOT_FF_SUBMITTED ? s->end_qpc : e->hit_qpc;
    if (now >= base && now - base >= cap) return 1;
    *until = base + cap;
    return 0;
}
/* nvnQueueSubmitCommands entry (renderer lock held). */
static inline void boot_ff_submit(BootContext *c) {
    if (!boot_ff_on()) return;
    BootFfNote note = {{0}, 0};
    AcquireSRWLockExclusive(&boot_ff.lock);
    BootFfSubmitter *s = boot_ff_submitter_locked(c->tpidrro, 1);
    unsigned was = s->state;
    if (was == BOOT_FF_IDLE) s->begin_qpc = boot_ov_qpc();   /* its first submit since its FenceSync */
    s->state = BOOT_FF_IN_SUBMIT; s->since_fence = 1;
    WakeAllConditionVariable(&boot_ff.cv);   /* repeat waiters: no give-up while it is inside the submit */
    boot_ff_note(&note, ": submit begins, t%d (tls %llx)%s", boot_ff_tid(c->tpidrro), (unsigned long long)c->tpidrro,
                 was == BOOT_FF_IDLE ? "" : ", another submit before its FenceSync");
    ReleaseSRWLockExclusive(&boot_ff.lock);
    boot_ff_emit(&note);
}
/* Every return of nvnQueueSubmitCommands (renderer lock held): give-ups of repeat waits now run from here. */
static inline void boot_ff_submit_end(BootContext *c) {
    if (!boot_ff_on()) return;
    BootFfNote note = {{0}, 0};
    AcquireSRWLockExclusive(&boot_ff.lock);
    BootFfSubmitter *s = boot_ff_submitter_locked(c->tpidrro, 0);
    if (s && s->state == BOOT_FF_IN_SUBMIT) {
        s->state = BOOT_FF_SUBMITTED; s->end_qpc = boot_ov_qpc();
        WakeAllConditionVariable(&boot_ff.cv);
        boot_ff_note(&note, ": submit returns, t%d, %llu ms after its first submit", boot_ff_tid(c->tpidrro),
                     (unsigned long long)((s->end_qpc - s->begin_qpc) * 1000 / boot_ov_freq()));
    }
    ReleaseSRWLockExclusive(&boot_ff.lock);
    boot_ff_emit(&note);
}
/* nvnQueueFenceSync (renderer lock held), after the sync object holds its new value: the next fence of the sync. */
static inline void boot_ff_fence(BootContext *c, uint64_t sync, uint64_t value) {
    if (!boot_ff_on()) return;
    BootFfNote note = {{0}, 0};
    AcquireSRWLockExclusive(&boot_ff.lock);
    BootFfSubmitter *s = boot_ff_submitter_locked(c->tpidrro, 0);
    unsigned frame = s && s->since_fence;
    if (s) { s->state = BOOT_FF_IDLE; s->since_fence = 0; }
    BootFfSync *e = boot_ff_find_locked(sync);
    if (!e) e = boot_ff_add_locked(sync);
    e->fencer = c->tpidrro; e->value = value; ++e->fences; e->frame = frame; e->fence_qpc = boot_ov_qpc();
    if (frame) { boot_ff.frame_owner = c->tpidrro; boot_ff.frame_sync = sync; boot_ff.frame_value = value; }
    WakeAllConditionVariable(&boot_ff.cv);
    boot_ff_note(&note, ": FenceSync by t%d (tls %llx), sync %llx value %llu, fence %llu of this sync, %s", boot_ff_tid(c->tpidrro),
                 (unsigned long long)c->tpidrro, (unsigned long long)sync, (unsigned long long)value, (unsigned long long)e->fences,
                 frame ? "a frame fence (right after its submit)" : "no submit since its previous FenceSync (not a frame fence)");
    ReleaseSRWLockExclusive(&boot_ff.lock);
    boot_ff_emit(&note);
}
/* A satisfied nvnSyncWait (renderer lock held): this thread has waited for fence `fence` of the sync (UINT64_MAX:
 * not a frame sync when it read it). */
static inline void boot_ff_consumed(BootContext *c, uint64_t sync, uint64_t fence) {
    if (fence == UINT64_MAX || !boot_ff_on()) return;
    AcquireSRWLockExclusive(&boot_ff.lock);
    BootFfSync *e = boot_ff_find_locked(sync);
    if (e && e->fencer != c->tpidrro) { BootFfWaiter *w = boot_ff_waiter_locked(e, c->tpidrro, 1); w->has = 1; w->consumed = fence; }
    ReleaseSRWLockExclusive(&boot_ff.lock);
}
/* nvnSyncWait entry (renderer lock held, the sync just read from guest memory). *fence: the fence number of the sync
 * that goes with the value the caller reads (for boot_ff_consumed; UINT64_MAX: not a frame sync). 0: unchanged path;
 * 1: the next FenceSync came (read the sync again); 2: gave up (read it again, continue as before); -1: the caller's
 * timeout (0 = poll) ran out first (TIMEOUT_EXPIRED). */
static inline int boot_ff_wait(BootContext *c, uint64_t sync, uint64_t timeout_ns, uint64_t *fence) {
    *fence = UINT64_MAX;
    if (!boot_ff_on()) return 0;
    uint64_t me = c->tpidrro, f = boot_ov_freq(), t0 = boot_ov_qpc(), cap = (uint64_t)BOOT_FF_CAP_MS * f / 1000, n0 = 0, until = 0;
    BootFfNote note = {{0}, 0}; const char *why = "not a frame sync"; int hit = 0, capped = 0, fencer = -1; unsigned stage = BOOT_FF_IDLE;
    AcquireSRWLockExclusive(&boot_ff.lock);
    BootFfSync *e = boot_ff_find_locked(sync);
    if (!e || !e->frame || e->fences < 2) _InterlockedIncrement64(&boot_ff.not_frame);
    else {
        n0 = e->fences; *fence = n0; fencer = boot_ff_tid(e->fencer); stage = boot_ff_stage_locked(e);
        if (e->fencer == me) { why = "the fencer's own wait"; _InterlockedIncrement64(&boot_ff.own); }
        else {
            BootFfWaiter *w = boot_ff_waiter_locked(e, me, 1); boot_ff.waiter = me;
            if (!w->has) { why = "this thread's first wait on it"; _InterlockedIncrement64(&boot_ff.first); }
            else if (w->consumed != n0) { why = "a fence it has not waited for yet (in order)"; _InterlockedIncrement64(&boot_ff.in_order); }
            else if (e->suspended && e->suspended_fence == n0) { why = "repeat wait, already gave up on this fence"; _InterlockedIncrement64(&boot_ff.after_give_up); }
            else if (stage == BOOT_FF_IDLE && n0 < w->off_until) { why = "repeat wait before the submit, that stage is off after give-ups"; _InterlockedIncrement64(&boot_ff.rule_off); }
            else {
                hit = 1; _InterlockedIncrement64(&boot_ff.hit[stage]);
                if (e->hit_fence != n0 || !e->hit_qpc) { e->hit_fence = n0; e->hit_qpc = t0; }
                capped = boot_ff_capped_locked(e, t0, cap, &until);
                if (capped) { e->suspended = 1; e->suspended_fence = n0; why = "repeat wait, give-up time already reached: passes"; }
                else why = timeout_ns ? "HIT: repeat wait on the fence it already waited for, blocks until the next FenceSync" : "HIT: repeat poll, answers not signaled";
            }
        }
    }
    boot_ff_note(&note, ": %s by t%d (tls %llx), sync %llx fence %llu, fencer t%d %s: %s", timeout_ns ? "wait" : "poll", boot_ff_tid(me),
                 (unsigned long long)me, (unsigned long long)sync, (unsigned long long)n0, fencer, boot_ff_stage_name[stage], why);
    ReleaseSRWLockExclusive(&boot_ff.lock);
    boot_ff_emit(&note);
    if (!hit) return 0;
    if (capped) { _InterlockedIncrement64(&boot_ff.gave_up); return 0; }
    if (!timeout_ns) { _InterlockedIncrement64(&boot_ff.polls); return -1; }
    /* The caller's own deadline (none for UINT64_MAX or anything absurdly long) gives TIMEOUT_EXPIRED. */
    uint64_t deadline = UINT64_MAX;
    if (timeout_ns != UINT64_MAX) { unsigned __int128 d = (unsigned __int128)timeout_ns * f / 1000000000u; if (d < (unsigned __int128)(UINT64_MAX / 2)) deadline = t0 + (uint64_t)d; }
    LONG depth = boot_ov_cs_release(); BootCore *k = boot_ov_leave_guest(c);
    int result = 2, off = 0; unsigned last = stage; uint64_t off_for = 0;
    AcquireSRWLockExclusive(&boot_ff.lock);
    for (;;) {
        if (c->window_native) {   /* outside the guard lock: with TOTK_OVERLAP=0 the check takes w->lock, which t2 holds when it formats the periodic line */
            ReleaseSRWLockExclusive(&boot_ff.lock);
            int closed = boot_window_is_closed((BootWindow *)(uintptr_t)c->window_native);
            AcquireSRWLockExclusive(&boot_ff.lock);
            if (closed) { result = 2; break; }
        }
        BootFfSync *x = boot_ff_find_locked(sync);
        if (!x || x->fences != n0) { result = 1; break; }   /* the next FenceSync (an entry reused for another sync: fenced long ago) */
        uint64_t now = boot_ov_qpc(), wake = deadline, gap;
        if (now >= deadline) { result = -1; break; }
        last = boot_ff_stage_locked(x);
        if (boot_ff_capped_locked(x, now, cap, &gap)) { result = 2; break; }
        if (gap < wake) wake = gap;
        uint64_t left = wake == UINT64_MAX ? UINT64_MAX : (wake - now) * 1000 / f + 1;   /* inside the submit: woken by its end / FenceSync */
        SleepConditionVariableSRW(&boot_ff.cv, &boot_ff.lock, left > 100 ? 100u : (DWORD)left, 0);
    }
    BootFfSync *x = boot_ff_find_locked(sync);
    BootFfWaiter *w = x ? boot_ff_waiter_locked(x, me, 0) : NULL;
    if (result == 2 && x) { x->suspended = 1; x->suspended_fence = n0; }
    if (w && result == 1) w->streak = 0;
    if (w && result == 2 && last == BOOT_FF_IDLE) {   /* no submit came at all: the fencer may be waiting for this thread */
        _InterlockedIncrement64(&boot_ff.give_up_before);
        if (++w->streak >= 3) {
            unsigned sh = w->offs < 15 ? w->offs : 15; ++w->offs; w->streak = 0;
            off_for = (uint64_t)32 << sh; w->off_until = x->fences + off_for; off = 1; _InterlockedIncrement64(&boot_ff.offs);
        }
    }
    ReleaseSRWLockExclusive(&boot_ff.lock);
    uint64_t t1 = boot_ov_qpc();
    _InterlockedIncrement64(&boot_ff.waits); _InterlockedExchangeAdd64(&boot_ff.wait_qpc, (LONG64)(t1 - t0));
    boot_ov_cs_reacquire(depth); boot_ov_enter_guest(k);
    if (result > 0) {   /* renderer lock held again: the fence number that goes with what the caller reads now */
        AcquireSRWLockShared(&boot_ff.lock);
        BootFfSync *y = boot_ff_find_locked(sync);
        *fence = y && y->frame && y->fences >= 2 ? y->fences : UINT64_MAX;
        ReleaseSRWLockShared(&boot_ff.lock);
    }
    unsigned long long ms = (unsigned long long)((t1 - t0) * 1000 / f);
    if (_InterlockedIncrement64(&boot_ff.hits_logged) <= 16) {
        char t[320];
        snprintf(t, sizeof t, ": t%d waited %llu ms on sync %llx (fence %llu, fencer t%d %s at the start, %s at the end): %s", boot_ff_tid(me), ms,
                 (unsigned long long)sync, (unsigned long long)n0, fencer, boot_ff_stage_name[stage], boot_ff_stage_name[last],
                 result == 1 ? "the next FenceSync came" : result == 2 ? "gave up" : "the caller's timeout ran out");
        boot_draw_log("nvn native: frame guard hit%s (hit %llu of the first 16, result %llu)", t, (uint64_t)boot_ff.hits_logged, (uint64_t)(result < 0 ? 3 : result));
    }
    if (result < 0) { _InterlockedIncrement64(&boot_ff.expired); return -1; }
    if (result == 2 && _InterlockedIncrement64(&boot_ff.gave_up) <= 8)
        boot_draw_log("nvn native: frame-in-flight guard gave up%s %llu ms after the fencer's submit returned or the first repeat wait (sync %llx): no FenceSync came",
                      last == BOOT_FF_IDLE ? " (no submit began)" : "", (uint64_t)BOOT_FF_CAP_MS, sync);
    if (off) boot_draw_log("nvn native: frame guard: three give-ups in a row with no submit, repeat waits before the submit pass for%s %llu fences (sync %llx)", "", off_for, sync);
    return result;
}

/* ---------------------------------------------------------------- P4: early fence (TOTK_EARLY_FENCE=1) */
static inline int boot_ef_on(void) {
    static int v = -1; if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_EARLY_FENCE", e, sizeof e); v = n && n < sizeof e && e[0] == '1'; }
    return v;
}
static inline int boot_ef_guard_on(void) {
    static int v = -1; if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_EARLY_FENCE_GUARD", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
static inline unsigned boot_ef_idle_every(void) {
    static long v = -2; if (v == -2) { char e[16]; DWORD n = GetEnvironmentVariableA("TOTK_EARLY_FENCE_IDLE_EVERY", e, sizeof e); v = n && n < sizeof e ? strtol(e, NULL, 10) : 16; if (v < 0) v = 0; }
    return (unsigned)v;
}
typedef struct BootEfRange { uint64_t g0, g1; } BootEfRange;   /* guest pages [g0, g1) */
typedef struct BootEfMap { uint64_t gpu, end, cpu; } BootEfMap;  /* GPU VA [gpu, end) -> guest VA cpu */
#define BOOT_EF_REC 8192
#define BOOT_EF_RUNS 8192
#define BOOT_EF_MAPS 16384
enum { BOOT_EF_FULL_PERIODIC = 1, BOOT_EF_FULL_PITCH, BOOT_EF_FULL_GW_ALL, BOOT_EF_FULL_RECORD, BOOT_EF_FULL_PROTECT, BOOT_EF_FULL_NO_GUARD_BUILD, BOOT_EF_FULL_REASONS };
/* Recording state: written by t2 inside its submit and by the frame waiter inside its sync wait, both holding the
 * renderer lock (host_fn.h), so it needs no lock of its own. */
typedef struct BootEarlyFence {
    int rec_on, rec_overflow; unsigned nrec[2]; BootEfRange rec[2][BOOT_EF_REC];   /* [0] GPU reads, [1] GPU writes */
    BootEfMap map[BOOT_EF_MAPS]; unsigned nmap;
    uint64_t waiter, prev, early, early_qpc, run;   /* prev: the last frame fence waited for; early: the last one returned early */
    uint64_t signals, full[BOOT_EF_FULL_REASONS], prev_waits, prev_wait_qpc, overlap_qpc, not_waiter;
} BootEarlyFence;
static BootEarlyFence boot_ef;
static inline int boot_ef_frame_wait(BootContext *c, uint64_t sync, uint64_t value, uint64_t timeout_ns);   /* host_nvn_play.h */
static inline int boot_ef_map_order(const void *a, const void *b) {
    const BootEfMap *x = (const BootEfMap *)a, *y = (const BootEfMap *)b; return x->gpu < y->gpu ? -1 : x->gpu > y->gpu;
}
/* GPU VA -> guest VA index of the GPU mappings the executor uses (the first address space, as boot_nvn_exec_memory).
 * Built at each submission (t2, window lock held: the mappings do not change while it is used). */
static inline void boot_ef_index(BootWindow *w) {
    boot_ef.rec_on = boot_ef_on() && boot_ef_guard_on() && boot_ov_threads();
    if (!boot_ef.rec_on || !w) { boot_ef.nmap = 0; return; }
    uint32_t as_fd = UINT32_MAX; unsigned n = 0;
    for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY && n < BOOT_EF_MAPS; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (!m->size) continue;
        if (as_fd == UINT32_MAX) as_fd = m->as_fd;
        uint64_t b = boot_gpu_mapping_backing_size(m);
        if (m->as_fd != as_fd || !m->cpu || !b || m->gpu > UINT64_MAX - b) continue;
        boot_ef.map[n].gpu = m->gpu; boot_ef.map[n].end = m->gpu + b; boot_ef.map[n].cpu = m->cpu; ++n;
    }
    qsort(boot_ef.map, n, sizeof boot_ef.map[0], boot_ef_map_order); boot_ef.nmap = n;
}
static inline void boot_ef_add(int write, uint64_t g, uint64_t n) {
    uint64_t g0 = g & ~UINT64_C(4095), g1 = (g + n + 4095) & ~UINT64_C(4095);
    unsigned *count = &boot_ef.nrec[write]; BootEfRange *r = boot_ef.rec[write];
    if (*count && g0 <= r[*count - 1].g1 && r[*count - 1].g0 <= g1) {   /* consecutive binds of one buffer collapse */
        if (g0 < r[*count - 1].g0) r[*count - 1].g0 = g0;
        if (g1 > r[*count - 1].g1) r[*count - 1].g1 = g1;
        return;
    }
    if (*count == BOOT_EF_REC) { boot_ef.rec_overflow = 1; return; }
    r[*count].g0 = g0; r[*count].g1 = g1; ++*count;
}
/* GPU work recorded now reads (write = 0) or may write (1) GPU VA [va, va + size). */
static inline void boot_ef_note(uint64_t va, uint64_t size, int write) {
    if (!boot_ef.rec_on || !va || !size) return;
    if (size > (UINT64_C(1) << 30) || va > UINT64_MAX - size) { boot_ef.rec_overflow = 1; return; }   /* unbounded: full wait */
    const BootEfMap *m = boot_ef.map; unsigned n = boot_ef.nmap;
    for (unsigned step = 0; size; ++step) {
        if (step == 16) { boot_ef.rec_overflow = 1; return; }
        unsigned lo = 0, hi = n;
        while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (m[mid].gpu <= va) lo = mid + 1; else hi = mid; }
        const BootEfMap *hit = NULL;
        for (unsigned k = lo; k-- > 0 && lo - k <= 8;) if (va < m[k].end) { hit = &m[k]; break; }   /* m[k].gpu <= va */
        if (!hit) {   /* no CPU backing here (sparse or unmapped): continue at the next mapping */
            if (lo == n || m[lo].gpu - va >= size) return;
            size -= m[lo].gpu - va; va = m[lo].gpu; continue;
        }
        uint64_t take = hit->end - va < size ? hit->end - va : size;
        boot_ef_add(write, hit->cpu + (va - hit->gpu), take);
        va += take; size -= take;
    }
}
static inline void boot_ef_reset_rec(void) { boot_ef.nrec[0] = boot_ef.nrec[1] = 0; boot_ef.rec_overflow = 0; }

#if TOTK_FASTMEM && TOTK_FM_FIXED
/* ---------------------------------------------------------------- P4: write guard over the fixed guest view */
typedef struct BootEfGuard {
    SRWLOCK lock; volatile LONG active; PVOID veh;
    uint64_t fence; ID3D12Fence *d3d;
    unsigned n, nread; BootEfRange run[BOOT_EF_RUNS];   /* [0, nread) PAGE_READONLY, [nread, n) PAGE_NOACCESS, each sorted */
    volatile LONG64 faults, fault_qpc, timeouts, protects, protect_qpc, pages, unviewed, cycles;
} BootEfGuard;
static BootEfGuard boot_efg;
/* Waits (signal-safe enough for a VEH: an event and the fence) until the D3D12 fence reaches v; 0 after ms. */
static inline int boot_efg_gpu_wait(ID3D12Fence *f, uint64_t v, DWORD ms) {
    if (!f || ID3D12Fence_GetCompletedValue(f) >= v) return 1;
    static _Thread_local HANDLE ev; if (!ev) ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    uint64_t fq = boot_ov_freq(), t0 = boot_ov_qpc();
    while (ID3D12Fence_GetCompletedValue(f) < v) {
        uint64_t spent = (boot_ov_qpc() - t0) * 1000 / fq; if (spent >= ms) return 0;
        if (ev && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(f, v, ev))) WaitForSingleObject(ev, (DWORD)(ms - spent));
        else Sleep(1);
    }
    return 1;
}
/* Every guarded page back to the protection fastmem's page states give it (guard lock held). The run table stays,
 * so a thread that faulted on a guarded page before this still recognises it and retries. */
static inline void boot_efg_restore_locked(void) {
    for (unsigned i = 0; i < boot_efg.n; ++i) boot_fm_reprotect(boot_efg.run[i].g0, boot_efg.run[i].g1);
    __atomic_store_n(&boot_efg.active, 0, __ATOMIC_RELEASE);
    if (boot_efg.d3d) { ID3D12Fence_Release(boot_efg.d3d); boot_efg.d3d = NULL; }
}
static inline int boot_efg_find(const BootEfRange *r, unsigned n, uint64_t g) {
    unsigned lo = 0, hi = n;
    while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (r[mid].g0 <= g) lo = mid + 1; else hi = mid; }
    return lo && g < r[lo - 1].g1;
}
static LONG CALLBACK boot_efg_veh(EXCEPTION_POINTERS *x) {
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || x->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t host = (uint64_t)x->ExceptionRecord->ExceptionInformation[1], kind = (uint64_t)x->ExceptionRecord->ExceptionInformation[0];
    uint64_t base = (uint64_t)(uintptr_t)BOOT_FM_VIEW;
    if (kind == 8 || host < base || host - base >= (UINT64_C(1) << BOOT_FM_SPACE_BITS) ||
        (!__atomic_load_n(&boot_efg.n, __ATOMIC_ACQUIRE) && !__atomic_load_n(&boot_efg.active, __ATOMIC_ACQUIRE))) return EXCEPTION_CONTINUE_SEARCH;
    /* n is still 0 while boot_efg_protect protects its first run with active already 1: such a fault takes the guard
     * lock below (the protector holds it) and then finds the recorded run. Falling through to boot_fm_veh instead
     * would let its reprotect silently unguard the page for the rest of the frame. */
    uint64_t g = (host - base) & ~UINT64_C(4095);
    static _Thread_local uint64_t last_rip; static _Thread_local unsigned retries;   /* stale-table retries of one instruction */
    AcquireSRWLockExclusive(&boot_efg.lock);
    int ours = boot_efg_find(boot_efg.run, boot_efg.nread, g) || boot_efg_find(boot_efg.run + boot_efg.nread, boot_efg.n - boot_efg.nread, g);
    if (ours && boot_efg.active) {   /* the first conflicting access: wait for the frame, then open every guarded page */
        uint64_t t0 = boot_ov_qpc();
        if (!boot_efg_gpu_wait(boot_efg.d3d, boot_efg.fence, 2000)) _InterlockedIncrement64(&boot_efg.timeouts);
        boot_efg_restore_locked();
        _InterlockedIncrement64(&boot_efg.faults); _InterlockedExchangeAdd64(&boot_efg.fault_qpc, (LONG64)(boot_ov_qpc() - t0));
        last_rip = 0;
    } else if (ours) {   /* guard already lifted by another thread: retry only if the page now allows the access */
        MEMORY_BASIC_INFORMATION mbi;
        DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        ours = VirtualQuery((const void *)(uintptr_t)host, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
               (kind == 1 ? (mbi.Protect & writable) != 0 : !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)));
        if (ours) {   /* never loop on one instruction: after a few stale retries the fault is fastmem's to judge */
            uint64_t rip = (uint64_t)x->ContextRecord->Rip;
            if (rip != last_rip) { last_rip = rip; retries = 0; }
            if (++retries > 4) { last_rip = 0; ours = 0; }
        }
    }
    ReleaseSRWLockExclusive(&boot_efg.lock);
    return ours ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
/* Open the guarded pages once the guarded frame has completed (the frame waiter, before guarding the next one). */
static inline void boot_efg_release_done(void) {
    if (!__atomic_load_n(&boot_efg.active, __ATOMIC_ACQUIRE)) return;
    AcquireSRWLockExclusive(&boot_efg.lock);
    if (boot_efg.active && (!boot_efg.d3d || ID3D12Fence_GetCompletedValue(boot_efg.d3d) >= boot_efg.fence)) boot_efg_restore_locked();
    ReleaseSRWLockExclusive(&boot_efg.lock);
}
static inline int boot_ef_range_order(const void *a, const void *b) {
    const BootEfRange *x = (const BootEfRange *)a, *y = (const BootEfRange *)b; return x->g0 < y->g0 ? -1 : x->g0 > y->g0;
}
static inline unsigned boot_ef_merge(BootEfRange *r, unsigned n) {
    if (n < 2) return n;
    qsort(r, n, sizeof *r, boot_ef_range_order);
    unsigned k = 0;
    for (unsigned i = 1; i < n; ++i) {
        if (r[i].g0 <= r[k].g1) { if (r[i].g1 > r[k].g1) r[k].g1 = r[i].g1; }
        else r[++k] = r[i];
    }
    return k + 1;
}
/* Protect the guest-writable pages of r[0..n) in the fixed view with prot, one VirtualProtect per run inside one
 * host region (a run never spans two section views). Guard lock held. 0 = a protection failed. */
static inline int boot_efg_protect_list(const BootEfRange *r, unsigned n, DWORD prot) {
    for (unsigned i = 0; i < n; ++i) {
        for (uint64_t p = r[i].g0; p < r[i].g1;) {
            if ((boot_fm_state(p) & (BOOT_FM_MAPPED | 3u)) != (BOOT_FM_MAPPED | 3u)) { p += 4096; continue; }   /* not guest-writable */
            uint64_t q = p + 4096;
            while (q < r[i].g1 && (boot_fm_state(q) & (BOOT_FM_MAPPED | 3u)) == (BOOT_FM_MAPPED | 3u)) q += 4096;
            while (p < q) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(BOOT_FM_VIEW + p, &mbi, sizeof mbi)) return 0;
                uint64_t end = (uint64_t)((uint8_t *)mbi.BaseAddress + mbi.RegionSize - BOOT_FM_VIEW); if (end > q) end = q;
                if (end <= p) return 0;
                if (mbi.State != MEM_COMMIT) {   /* no view yet: a guest access maps it first (boot_fm_veh), unguarded */
                    _InterlockedExchangeAdd64(&boot_efg.unviewed, (LONG64)((end - p) >> 12)); p = end; continue; }
                if (boot_efg.n == BOOT_EF_RUNS) return 0;
                DWORD old;
                if (!VirtualProtect(BOOT_FM_VIEW + p, (SIZE_T)(end - p), prot, &old)) return 0;
                boot_efg.run[boot_efg.n].g0 = p; boot_efg.run[boot_efg.n].g1 = end; ++boot_efg.n;
                _InterlockedIncrement64(&boot_efg.protects); _InterlockedExchangeAdd64(&boot_efg.pages, (LONG64)((end - p) >> 12));
                p = end;
            }
        }
    }
    return 1;
}
/* Guard what the recorded GPU work of the frame fenced at `value` still touches until `fence` reaches it. The caller
 * holds the renderer lock; the recorded ranges are consumed. 0 = not guarded (the frame takes the full wait). */
static inline int boot_efg_protect(ID3D12Fence *fence, uint64_t value) {
    uint64_t t0 = boot_ov_qpc();
    if (!boot_efg.veh) { boot_efg.veh = AddVectoredExceptionHandler(1, boot_efg_veh); if (!boot_efg.veh) return 0; }   /* first: before boot_fm_veh */
    unsigned nr = boot_ef_merge(boot_ef.rec[0], boot_ef.nrec[0]), nw = boot_ef_merge(boot_ef.rec[1], boot_ef.nrec[1]);
    AcquireSRWLockExclusive(&boot_efg.lock);
    if (boot_efg.active) {   /* an older frame: normally completed (the caller waited for it); if not, keep it, no new guard */
        if (boot_efg.d3d && ID3D12Fence_GetCompletedValue(boot_efg.d3d) < boot_efg.fence) { ReleaseSRWLockExclusive(&boot_efg.lock); boot_ef_reset_rec(); return 0; }
        boot_efg_restore_locked();
    }
    __atomic_store_n(&boot_efg.n, 0, __ATOMIC_RELEASE); boot_efg.nread = 0;
    ID3D12Fence_AddRef(fence); boot_efg.d3d = fence; boot_efg.fence = value;
    __atomic_store_n(&boot_efg.active, 1, __ATOMIC_RELEASE);   /* a fault during the protection below waits for the frame */
    int ok = boot_efg_protect_list(boot_ef.rec[0], nr, PAGE_READONLY);
    boot_efg.nread = boot_efg.n;
    ok = ok && boot_efg_protect_list(boot_ef.rec[1], nw, PAGE_NOACCESS);
    if (!ok || !boot_efg.n) boot_efg_restore_locked();   /* failed (or nothing to guard): no early return taken on a partial guard */
    ReleaseSRWLockExclusive(&boot_efg.lock);
    _InterlockedIncrement64(&boot_efg.cycles); _InterlockedExchangeAdd64(&boot_efg.protect_qpc, (LONG64)(boot_ov_qpc() - t0));
    boot_ef_reset_rec();
    return ok;
}
#else
static inline void boot_efg_release_done(void) {}
#endif

/* The periodic log line (host_nvn_play.h, every 100 submissions). */
static inline void boot_ov_format(char *out, size_t cap) {
    uint64_t f = boot_ov_freq();
    AcquireSRWLockShared(&boot_ff.lock);   /* guardfix: who the guard thinks submits and waits, and how it decided */
    const BootFfSync *e = boot_ff.frame_sync ? boot_ff_find_locked(boot_ff.frame_sync) : NULL;
    BootFfWaiter *w = e ? boot_ff_waiter_locked((BootFfSync *)e, boot_ff.waiter, 0) : NULL;
    uint64_t fsync = boot_ff.frame_sync, fences = e ? e->fences : 0, fencer = e ? e->fencer : 0, consumed = w && w->has ? w->consumed : 0;
    unsigned stage = e ? boot_ff_stage_locked(e) : BOOT_FF_IDLE;
    uint64_t owner = boot_ff.frame_owner, waiter = boot_ff.waiter, value = boot_ff.frame_value;
    ReleaseSRWLockShared(&boot_ff.lock);
    {   char g[640];
        snprintf(g, sizeof g, ": on %d (TOTK_OVERLAP %d), frame sync %llx fence %llu value %llu, fencer t%d (tls %llx, now %s), frame owner t%d, frame waiter t%d"
                 " (tls %llx, consumed fence %llu); repeat waits held, fencer before its submit %llu, inside it %llu, after it %llu; passed: in order %llu, first %llu,"
                 " not a frame sync %llu, fencer's own %llu, after a give-up %llu, before-submit stage off %llu; give-ups with no submit %llu, stage switched off %llu;",
                 boot_ff_on(), boot_mt_overlap_on(), (unsigned long long)fsync, (unsigned long long)fences, (unsigned long long)value, boot_ff_tid(fencer),
                 (unsigned long long)fencer, boot_ff_stage_name[stage], boot_ff_tid(owner), boot_ff_tid(waiter), (unsigned long long)waiter,
                 (unsigned long long)consumed, (unsigned long long)boot_ff.hit[BOOT_FF_IDLE], (unsigned long long)boot_ff.hit[BOOT_FF_IN_SUBMIT],
                 (unsigned long long)boot_ff.hit[BOOT_FF_SUBMITTED], (unsigned long long)boot_ff.in_order, (unsigned long long)boot_ff.first,
                 (unsigned long long)boot_ff.not_frame, (unsigned long long)boot_ff.own, (unsigned long long)boot_ff.after_give_up,
                 (unsigned long long)boot_ff.rule_off, (unsigned long long)boot_ff.give_up_before, (unsigned long long)boot_ff.offs);
        boot_draw_log("nvn native: frame guard%s decisions logged %llu, blocked waits logged %llu", g, (uint64_t)boot_ff.logged, (uint64_t)boot_ff.hits_logged); }
    size_t at = (size_t)snprintf(out, cap, ": frame-in-flight guard waits %llu (%llu ms, gave up %llu, caller timeouts %llu, polls answered not signaled %llu),"
        " frame submitter t%d, frame waiter t%d;",
        (unsigned long long)boot_ff.waits, (unsigned long long)((uint64_t)boot_ff.wait_qpc * 1000 / f), (unsigned long long)boot_ff.gave_up,
        (unsigned long long)boot_ff.expired, (unsigned long long)boot_ff.polls, boot_ff_tid(owner), boot_ff_tid(waiter));
    if (at < cap) at += (size_t)snprintf(out + at, cap - at, " early fence (TOTK_EARLY_FENCE %d, guard %d): full waits periodic %llu pitch %llu gw-overflow %llu record %llu protect %llu no-guard-build %llu, other waiter %llu, previous-frame waits %llu (%llu ms), overlap %llu ms;",
        boot_ef_on(), boot_ef_guard_on(), (unsigned long long)boot_ef.full[BOOT_EF_FULL_PERIODIC], (unsigned long long)boot_ef.full[BOOT_EF_FULL_PITCH],
        (unsigned long long)boot_ef.full[BOOT_EF_FULL_GW_ALL], (unsigned long long)boot_ef.full[BOOT_EF_FULL_RECORD], (unsigned long long)boot_ef.full[BOOT_EF_FULL_PROTECT],
        (unsigned long long)boot_ef.full[BOOT_EF_FULL_NO_GUARD_BUILD], (unsigned long long)boot_ef.not_waiter, (unsigned long long)boot_ef.prev_waits,
        (unsigned long long)(boot_ef.prev_wait_qpc * 1000 / f), (unsigned long long)(boot_ef.overlap_qpc * 1000 / f));
#if TOTK_FASTMEM && TOTK_FM_FIXED
    if (at < cap) at += (size_t)snprintf(out + at, cap - at, " write guard: cycles %llu, runs %llu, pages %llu, unviewed pages %llu, protect %llu ms, faults %llu (%llu ms waiting, timeouts %llu);",
        (unsigned long long)boot_efg.cycles, (unsigned long long)boot_efg.protects, (unsigned long long)boot_efg.pages, (unsigned long long)boot_efg.unviewed,
        (unsigned long long)((uint64_t)boot_efg.protect_qpc * 1000 / f), (unsigned long long)boot_efg.faults, (unsigned long long)((uint64_t)boot_efg.fault_qpc * 1000 / f),
        (unsigned long long)boot_efg.timeouts);
#endif
    (void)at;
}
#endif
