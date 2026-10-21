#ifndef TOTK_HOST_NVN_FENCE_READ_H
#define TOTK_HOST_NVN_FENCE_READ_H
/* fence-read trace (TOTK_FENCE_READ_TRACE=1; play-work/fence-read-trace/README.md). Observation only: nothing here changes what
 * is drawn, what is copied or waited for, or which locks a thread takes. Included from host_nvn_play.h (runtime_support,
 * TOTK_DRAW_PATH): the page header does not change.
 *
 * The question (play-work/async-xlate-work PRECHECKS.md section 4, DESIGN.md 7): in 1,261 of 1,264 traced frames TOTK's main
 * thread (t0), right after its nvnSyncWait on frame N's fence, invalidates a 58,848-byte memory pool that frame N's GPU work
 * writes (bound as a storage buffer about 41 times per frame at the same offset: single-buffered) and converts two GPU
 * timestamps. An early frame fence (TOTK_ASYNC_XLATE stage 3) pays off only if t0 hands frame N+1 to the GPU thread (t2)
 * BEFORE it reads that output. The NVN trace cannot say: the hand-off is not an NVN call. Recorded here, with QPC and guest
 * thread:
 *  - nvnSyncWait returns. The frame fence is the sync of the newest FenceSync that followed a submit on the same thread (P3's
 *    frame sync, host_nvn_overlap.h), waited for by another thread; the value is the one the wait read at its entry. Other
 *    waits (the always-signalled acquire sync) and nvnQueueFinish returns are counted apart;
 *  - nvnMemoryPoolInvalidateMappedRange / FlushMappedRange (pool GPU address: the driver's pool object +0x18, as
 *    boot_nvn_pool_object_va; offset; size) and nvnBufferInvalidateMappedRange / FlushMappedRange (buffer object, offset,
 *    size): OBSERVE functions while the trace is on, registered NOLOCK, noted before the driver's own function runs (tail
 *    call); TOTK_FENCE_READ_FLUSH=0 leaves the two Flush functions to the driver (PASS) if their count costs;
 *  - nvnDeviceGetTimestampInNanoseconds (the counter's tick word and its age against the host clock). The game's reads of
 *    counters and of the pool are plain loads: the invalidate and this conversion are the only calls;
 *  - the hand-off (boot_mt_late.h boot_frt_k): t2's first wake after each park, with the guest thread whose kernel exit made it
 *    runnable (kick_check runs inside the waker's own kernel section: the wake time is the waker's signal time), and t0's
 *    signalling SVCs after each frame-fence return, taken before the lock-free shortcuts (a signal for a word t2 is not parked
 *    on yet never enters the kernel; an uncontended nn::os condvar signal is handled by host_sdk_os.h without an SVC and stays
 *    invisible, but then nobody was parked on it). Plus t2's thread state read (racily) at each of t0's events: parked (4 lock,
 *    5 handles, 6 arbiter, 7 condvar) = still waiting for the hand-off;
 *  - t2's nvnQueueSubmitCommands entry (its first NVN call of the next frame), FenceSync, present and AcquireTexture.
 * The hand-off time of a frame: t0's signal on the object t2 was parked on when the waker is t0 (program order on t0), else the
 * wake itself; with no wake (t2 was not parked yet), t0's signal on the object of the most frequent earlier hand-off.
 * Buffers: one ring per producing host thread (thread-local, one writer, lock-free), the kernel ring (its writers are
 * serialized by the kernel lock the hooks already run under) and t0's signal ring; t2 drains all of them right after each of
 * its presents (it holds the renderer lock there already; nothing here takes a lock), sorts by QPC and closes a cycle at each
 * frame-fence return (so a frame is reported one present later).
 * draw-path.log: an "on" line; the first 16 cycles and 16 more from cycle TOTK_FENCE_READ_TRACE_DETAIL_AT (default 5000, 0 =
 * none) event by event; one summary line every 300 presents; once per watched range (the first ranges t0 reads after the
 * fence), the programs that have it bound as a storage buffer and whether they store (boot_frt_draw / boot_frt_dispatch).
 * Off (default): the PASS functions stay PASS and the queue/sync/window functions keep their own registration (the wrappers
 * are installed only when on), each exec hook is one cached branch, each kernel hook one compare. */

/* ---------------------------------------------------------------- switch, events, per-thread rings */
static int boot_frt_mode = -1;   /* -1 not read yet, 0 off, 1 on */
static inline __attribute__((noinline)) void boot_frt_mode_init(void) {
    char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_FENCE_READ_TRACE", v, sizeof v);
    boot_frt_mode = k && k < sizeof v && v[0] == '1' ? 1 : 0;
}
static inline int boot_frt_on(void) {
    if (__builtin_expect(boot_frt_mode < 0, 0)) boot_frt_mode_init();
    return __builtin_expect(boot_frt_mode > 0, 0);
}
static inline uint32_t boot_frt_env_u32(const char *name, uint32_t def) {
    char v[16]; DWORD k = GetEnvironmentVariableA(name, v, sizeof v);
    return k && k < sizeof v ? (uint32_t)strtoul(v, NULL, 10) : def;
}
enum { BOOT_FRT_FW = 1, BOOT_FRT_W, BOOT_FRT_FIN, BOOT_FRT_INV, BOOT_FRT_FLU, BOOT_FRT_BINV, BOOT_FRT_BFLU, BOOT_FRT_TS,
       BOOT_FRT_SUB, BOOT_FRT_FEN, BOOT_FRT_PRE, BOOT_FRT_ACQ, BOOT_FRT_PARK, BOOT_FRT_WAKE, BOOT_FRT_SIG, BOOT_FRT_KINDS };
/* One event (48 bytes). By kind:
 *   FW, W     nvnSyncWait returned (FW: on the frame fence, by another thread than its fencer): a sync, b the value read at
 *             entry, c QPC at entry, d result (0 already signalled, 1 satisfied, 2 timeout)
 *   FIN       nvnQueueFinish returned: c QPC at entry
 *   INV, FLU  nvnMemoryPool{Invalidate,Flush}MappedRange: a pool GPU address (0: unreadable), b size, c pool object, d offset
 *   BINV, BFLU nvnBuffer{Invalidate,Flush}MappedRange: b size, c buffer object, d offset (the GPU address is the driver's)
 *   TS        nvnDeviceGetTimestampInNanoseconds: a counter address, b its tick word (+8)
 *   SUB       nvnQueueSubmitCommands entry: a handle count.  FEN nvnQueueFenceSync: a sync, b value, d 1 = right after a submit
 *   PRE       nvnQueuePresentTexture returned.  ACQ nvnWindowAcquireTexture returned: c QPC at entry
 *   PARK      (kernel) the render thread parked: a wait_address, b key.  WAKE its first wake after that park: a, b of the park,
 *             c QPC ticks parked, d its x0 (0 signalled, 0xea01 timed out), e the waker (guest thread; 0xfffe timer, 0xfffd event)
 *   SIG       (the waiter's SVC entry) e svc, a x0, b x1, c x2, d x3
 * x: PARK / WAKE the park's state; otherwise the render thread's state when the event happened (read racily: 2 runnable or
 *    running, 4 lock, 5 handles, 6 arbiter, 7 condvar, 8 sleep; 0xff unknown). */
typedef struct BootFrtEv { uint64_t qpc, a, b, c; uint32_t d, e; uint16_t thread; uint8_t kind, x; } BootFrtEv;
static const char *const boot_frt_kind_name[BOOT_FRT_KINDS] = {"?", "frame wait", "wait", "finish", "inv pool", "flush pool", "inv buffer",
    "flush buffer", "timestamp", "submit", "fence", "present", "acquire", "parks", "woken", "signal"};
#define BOOT_FRT_BUF 2048u
typedef struct BootFrtBuf { struct BootFrtBuf *next; uint32_t head, tail, lost, pad; BootFrtEv ev[BOOT_FRT_BUF]; } BootFrtBuf;
static BootFrtBuf *boot_frt_bufs;             /* every producing thread's ring (pushed once, lock-free; never freed) */
static _Thread_local BootFrtBuf *boot_frt_mine;
static uint32_t boot_frt_nobuf;               /* events dropped because a ring could not be allocated */
static inline void boot_frt_put(const BootFrtEv *e) {
    BootFrtBuf *b = boot_frt_mine;
    if (__builtin_expect(!b, 0)) {
        b = (BootFrtBuf *)calloc(1, sizeof *b);
        if (!b) { __atomic_fetch_add(&boot_frt_nobuf, 1, __ATOMIC_RELAXED); return; }
        BootFrtBuf *old = __atomic_load_n(&boot_frt_bufs, __ATOMIC_ACQUIRE);
        do b->next = old; while (!__atomic_compare_exchange_n(&boot_frt_bufs, &old, b, 1, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
        boot_frt_mine = b;
    }
    const uint32_t h = b->head;
    if (h - __atomic_load_n(&b->tail, __ATOMIC_ACQUIRE) >= BOOT_FRT_BUF) { __atomic_store_n(&b->lost, b->lost + 1, __ATOMIC_RELAXED); return; }
    b->ev[h % BOOT_FRT_BUF] = *e;
    __atomic_store_n(&b->head, h + 1, __ATOMIC_RELEASE);
}
/* Published by t2 for the other threads: the frame sync (its newest FenceSync after a submit) and its own guest thread. */
static struct { uint64_t frame_sync; uint32_t t2, since_submit; } boot_frt_pub = {0, BOOT_FRT_NONE, 0};
static inline void boot_frt_note(const BootContext *c, unsigned kind, uint64_t qpc, uint64_t a, uint64_t b, uint64_t cc, uint32_t d, uint32_t e, unsigned x) {
    if (__atomic_load_n(&boot_frt_pub.t2, __ATOMIC_RELAXED) == BOOT_FRT_NONE) return;   /* review: before t2's first present (loading: the rings would only overflow) */
    BootFrtEv v; v.qpc = qpc; v.a = a; v.b = b; v.c = cc; v.d = d; v.e = e;
    v.thread = (uint16_t)boot_nvn_guest_thread(c); v.kind = (uint8_t)kind; v.x = (uint8_t)x;
    boot_frt_put(&v);
}
static inline unsigned boot_frt_t2_state(void) {
    const uint32_t t = __atomic_load_n(&boot_frt_pub.t2, __ATOMIC_RELAXED); const BootContext *M = boot_mt.master;
    if (t >= BOOT_THREAD_CAPACITY || !M || t >= M->thread_count) return 0xffu;
    return __atomic_load_n(&M->threads[t].state, __ATOMIC_RELAXED) & 0xffu;   /* racy on purpose: observation only */
}

/* ---------------------------------------------------------------- observed driver functions (NOLOCK, then the driver's) */
static const char *const boot_frt_fn_names[5] = {"nvnMemoryPoolInvalidateMappedRange", "nvnMemoryPoolFlushMappedRange",
    "nvnBufferInvalidateMappedRange", "nvnBufferFlushMappedRange", "nvnDeviceGetTimestampInNanoseconds"};
/* 1..5: the name's place above while the trace is on; 0: not traced (host_nvn.h boot_nvn_policy and registration). */
static inline unsigned boot_frt_fn_kind(const char *n) {
    if (!boot_frt_on()) return 0;
    static int flush = -1; if (flush < 0) flush = boot_frt_env_u32("TOTK_FENCE_READ_FLUSH", 1) != 0;
    for (unsigned k = 0; k < 5; ++k)
        if (!strcmp(n, boot_frt_fn_names[k])) return (k == 1 || k == 3) && !flush ? 0 : k + 1;
    return 0;
}
static inline int boot_frt_name(const char *n) { return boot_frt_fn_kind(n) != 0; }
static inline void *boot_frt_user(const char *n, int index) { return (void *)(((intptr_t)boot_frt_fn_kind(n) << 16) | (intptr_t)(index & 0xffff)); }
BOOT_NVN_FN(boot_frt_observe) {   /* user: kind << 16 | name index */
    const intptr_t u = (intptr_t)user; const int index = (int)(u & 0xffff); const unsigned kind = (unsigned)(u >> 16);
    uint64_t logical = boot_nvn_trace()->procs[index], real = logical ? boot_address(c, logical) : 0;
    if (!real || real == UINT64_MAX) return boot_nvn_fail(c, self, "driver function unknown", (uint64_t)index);
    const uint64_t now = boot_nvn_qpc();
    if (kind == 5) {   /* (device, const NVNcounterData *): {counter, timestamp ticks} */
        uint64_t ticks = 0; boot_nvn_peek(c, self, c->x[1] + 8, &ticks, 8);
        boot_frt_note(c, BOOT_FRT_TS, now, c->x[1], ticks, 0, 0, 0, boot_frt_t2_state());
    } else {           /* (pool or buffer, offset, size) */
        static const uint8_t kinds[4] = {BOOT_FRT_INV, BOOT_FRT_FLU, BOOT_FRT_BINV, BOOT_FRT_BFLU};
        uint64_t va = 0; if (kind <= 2 && c->x[0]) boot_nvn_peek(c, self, c->x[0] + 0x18, &va, 8);   /* the driver's pool object */
        boot_frt_note(c, kind >= 1 && kind <= 4 ? kinds[kind - 1] : BOOT_FRT_INV, now, va, c->x[2], c->x[0], (uint32_t)c->x[1], (uint32_t)(c->x[1] >> 32),
                      boot_frt_t2_state());
    }
    *target = real;
    return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- the render thread's side: state, watched ranges, producers */
#define BOOT_FRT_BATCH 16384u   /* events drained per present */
#define BOOT_FRT_WIN 1024u      /* samples kept per summary window */
#define BOOT_FRT_POOLS 32u
#define BOOT_FRT_CEV 64u        /* events kept per detailed cycle */
#define BOOT_FRT_CSIG 64u       /* waiter signals kept per cycle (= BOOT_FRT_SIG_ARM) */
#define BOOT_FRT_CEV_KIND 16u   /* detailed lines: at most this many signals, and parks + wakes, per cycle */
#define BOOT_FRT_HO 8u
#define BOOT_FRT_WATCH 4u
#define BOOT_FRT_PROD 48u       /* programs per watched range: compute ones, and at most BOOT_FRT_PROD_GFX storing graphics ones */
#define BOOT_FRT_PROD_GFX 32u
typedef struct BootFrtSamples { uint32_t n; int32_t v[BOOT_FRT_WIN]; } BootFrtSamples;
typedef struct BootFrtCycle {
    uint64_t f, wait0, sync, value; uint32_t index, present, result, detail; uint8_t t2_at_f, t2_at_read, woken_before, pad;
    uint64_t read, read_base, read_off, read_size, read_any, sub, wake, wake0, wake_obj, wake_key; uint32_t read_any_thread, wake_by, wake_state;   /* wake: the first
                                         * qualifying wake of t2 (any waker); wake0 and its park objects: the first by the waiter */
    uint32_t ts, ts_waiter, inv, nsig; struct { uint64_t qpc, obj; uint32_t svc, pad; } sig[BOOT_FRT_CSIG];
    uint32_t nev, more, nev_sig, nev_park; BootFrtEv ev[BOOT_FRT_CEV];
} BootFrtCycle;
typedef struct BootFrtPool { uint64_t va, size, obj; uint32_t kind, n, waiter, first, nthreads; uint16_t threads[4]; } BootFrtPool;
typedef struct BootFrtProd { uint64_t data; uint8_t id[8]; uint32_t stage, binds, stores, learnt, has_id; char label[112]; } BootFrtProd;
typedef struct BootFrtState {
    BootFrtEv *batch; uint64_t freq; uint32_t presents, cycles, waiter, detail_at, late;
    BootFrtCycle cur; int open; uint64_t last_present, last_wake_waiter, last_wake_used;
    /* summary window */
    uint32_t w_cycles, w_rb, w_hb, w_noread, w_nohand, w_before, w_parked_f, w_parked_read, w_sig, w_sig_learnt, w_wake, w_wake_other, w_repeat, w_waits, w_fin;
    uint64_t w_ts, w_ts_waiter;
    BootFrtSamples f2r, f2a, f2h, r2h, f2w, f2s, wait, age;
    BootFrtPool pool[BOOT_FRT_POOLS]; uint32_t npool, pool_over;
    struct { uint32_t thread, n; } waker[8]; uint32_t nwaker;
    struct { uint64_t obj, key; uint32_t state, n; } ho[BOOT_FRT_HO]; uint32_t nho;   /* hand-off objects (all windows) */
    /* watched ranges: the ranges the waiter reads first after the fence (t2-owned: set here, read by the exec hooks on t2) */
    uint32_t nwatch; uint64_t wbase[BOOT_FRT_WATCH], woff[BOOT_FRT_WATCH], wsize[BOOT_FRT_WATCH]; uint32_t wsince[BOOT_FRT_WATCH], wlogged[BOOT_FRT_WATCH];
    uint32_t nprod[BOOT_FRT_WATCH], ngfx[BOOT_FRT_WATCH], prod_over[BOOT_FRT_WATCH]; uint64_t other[BOOT_FRT_WATCH][4];
    uint64_t gfx_bound[BOOT_FRT_WATCH][2], gfx_stored[BOOT_FRT_WATCH][2];   /* draws with it bound in the vertex / fragment stage, and storing from it */
    BootFrtProd prod[BOOT_FRT_WATCH][BOOT_FRT_PROD];
} BootFrtState;
static BootFrtState *boot_frt_st;   /* allocated at t2's first traced present */
static inline int boot_frt_watch_hit(const BootFrtState *S, uint64_t va, uint64_t size) {
    for (uint32_t w = 0; w < S->nwatch; ++w) {
        const uint64_t lo = S->wbase[w] + S->woff[w];
        if (va < lo + S->wsize[w] && lo < va + size) return (int)w;
    }
    return -1;
}
static inline BootFrtProd *boot_frt_prod(BootFrtState *S, uint32_t w, uint32_t stage, uint64_t data) {
    for (uint32_t i = 0; i < S->nprod[w]; ++i) { BootFrtProd *p = &S->prod[w][i]; if (p->stage == stage && p->data == data) return p; }
    if (S->nprod[w] == BOOT_FRT_PROD || (stage != 5 && S->ngfx[w] == BOOT_FRT_PROD_GFX)) { ++S->prod_over[w]; return NULL; }   /* room kept for compute */
    if (stage != 5) ++S->ngfx[w];
    BootFrtProd *p = &S->prod[w][S->nprod[w]++]; memset(p, 0, sizeof *p); p->stage = stage; p->data = data;
    return p;
}
/* A draw that ran (host_nvn_play.h boot_nvn_exec_draw, right after boot_render_draw; t2, w->lock held): for each graphics
 * stage whose bound storage buffers (NVN stage 0 vertex, 1 fragment: e->ssbo) overlap a watched range, the draw is counted
 * (the trace-mode run binds this pool in every stage of 76 graphics programs: bound state, mostly readers), and when the
 * renderer saw the draw's shaders store (stored: boot_render_store_draws moved) from that stage (stages = boot_ds_store_stages:
 * 1 vertex, 2 pixel; 0 = not known, counted for both) it is counted against that stage's program. The program's canonical id
 * (the shader-manifest / shader-labels key) is asked once per program address (boot_render_program_id: the draw just resolved
 * it, so a cache hit). A bound buffer is not proof of a store to it: "storing" is the stage's, not the slot's. */
static inline void boot_frt_draw(BootRender *r, const BootNvnExec *e, int stored, unsigned stages) {
    BootFrtState *S = boot_frt_st; if (!S || !S->nwatch) return;
    for (unsigned st = 0; st < 2; ++st)
        for (unsigned i = 0; i < 16; ++i) {
            const uint64_t va = e->ssbo[st][i], n = e->ssbo_size[st][i];
            if (!va || !n) continue;
            const int w = boot_frt_watch_hit(S, va, n); if (w < 0) continue;
            ++S->gfx_bound[w][st];
            if (stored && (!stages || ((stages >> st) & 1u))) {
                ++S->gfx_stored[w][st];
                const BootNvnProgramStage *ps = NULL;
                for (unsigned k = 0; e->has_prog && k < e->prog.count; ++k) if (e->prog.stage[k].kind == (st ? 5u : 1u)) ps = &e->prog.stage[k];
                BootFrtProd *p = boot_frt_prod(S, (uint32_t)w, st, ps ? ps->data : 0);
                if (p && !p->binds && ps) { uint8_t id[32]; if (boot_render_program_id(r, ps->data + 0x30, id)) { memcpy(p->id, id, 8); p->has_id = 1; } }
                if (p) { ++p->binds; ++p->stores; }
            }
            break;   /* once per stage and draw */
        }
}
/* The compute program's learnt store ranges (host_gpu_compute.h), looked up without boot_compute_writes_of's insert. */
static inline const BootComputeWrites *boot_frt_cs_writes(const uint8_t id[32]) {
    unsigned h; memcpy(&h, id, 4); h &= BOOT_COMPUTE_PROGRAMS - 1;
    for (unsigned n = 0; n < BOOT_COMPUTE_PROGRAMS; ++n, h = (h + 1) & (BOOT_COMPUTE_PROGRAMS - 1)) {
        const BootComputeWrites *w = &boot_compute_writes[h];
        if (!w->used) return NULL;
        if (!memcmp(w->id, id, 32)) return w;
    }
    return NULL;
}
/* A dispatch that ran (boot_nvn_exec_dispatch, a non-empty grid: id is set): compute storage buffers (e->ssbo[5]) over a
 * watched range; "learnt" = the range is inside the stores the program was seen to make (its dirty bitmap: proof of a write). */
static inline void boot_frt_dispatch(const BootNvnExec *e, const uint8_t id[32]) {
    BootFrtState *S = boot_frt_st; if (!S || !S->nwatch) return;
    const BootComputeWrites *learnt = NULL; int looked = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const uint64_t va = e->ssbo[5][i], n = e->ssbo_size[5][i];
        if (!va || !n) continue;
        const int w = boot_frt_watch_hit(S, va, n); if (w < 0) continue;
        const BootNvnProgramStage *cs = NULL;
        for (unsigned k = 0; e->has_prog && k < e->prog.count; ++k) if (e->prog.stage[k].kind == 6) cs = &e->prog.stage[k];
        BootFrtProd *p = boot_frt_prod(S, (uint32_t)w, 5, cs ? cs->data : 0); if (!p) continue;
        if (!p->has_id) { memcpy(p->id, id, 8); p->has_id = 1; }
        ++p->binds;
        if (!looked) { looked = 1; learnt = boot_frt_cs_writes(id); }
        const uint64_t lo = S->wbase[w] + S->woff[w], hi = lo + S->wsize[w];
        for (uint32_t k = 0; learnt && learnt->seen && k < learnt->count; ++k)
            if (learnt->va[k] < hi && lo < learnt->va[k] + learnt->size[k]) { ++p->learnt; break; }
    }
}
/* Other GPU-ordered writes into a watched range (exec_handle, every record): ClearBuffer, UpdateUniformBuffer,
 * CopyTextureToBuffer (its destination address), ReportCounter / ReportValue. */
static inline void boot_frt_op(const BootNvnOps *o, int op, const uint64_t *x, uint32_t b0n, int t2b_op) {
    BootFrtState *S = boot_frt_st; if (!S || !S->nwatch) return;
    uint64_t va, n; unsigned k;
    if (op == o->clear_buffer) { va = x[1]; n = x[2]; k = 0; }
    else if (op == o->update_ubo) { va = x[1] + x[3]; n = b0n; k = 1; }
    else if (op == t2b_op) { va = x[4]; n = 1; k = 2; }
    else if (op == o->report_counter) { va = x[2]; n = 16; k = 3; }
    else if (op == o->report_value) { va = x[2]; n = 4; k = 3; }
    else return;
    const int w = boot_frt_watch_hit(S, va, n ? n : 1); if (w >= 0) ++S->other[w][k];
}

/* ---------------------------------------------------------------- the render thread's side: drain, cycles, lines */
static inline BootFrtState *boot_frt_state(void) {
    if (boot_frt_st) return boot_frt_st;
    BootFrtState *S = (BootFrtState *)calloc(1, sizeof *S);
    BootFrtEv *b = S ? (BootFrtEv *)calloc(BOOT_FRT_BATCH, sizeof *b) : NULL;
    if (!S || !b) { free(S); free(b); boot_frt_mode = 0; boot_gp_log("nvn native: fence-read trace: no memory, off"); return NULL; }
    S->batch = b; S->freq = boot_nvn_qpf(); S->waiter = BOOT_FRT_NONE; S->detail_at = boot_frt_env_u32("TOTK_FENCE_READ_TRACE_DETAIL_AT", 5000);
    boot_frt_st = S;
    return S;
}
static inline int32_t boot_frt_us(const BootFrtState *S, uint64_t from, uint64_t to) {
    const double us = (double)(int64_t)(to - from) * 1e6 / (double)S->freq;
    return us > 2e9 ? 2000000000 : us < -2e9 ? -2000000000 : (int32_t)us;
}
static inline void boot_frt_sample(BootFrtSamples *s, int32_t v) { if (s->n < BOOT_FRT_WIN) s->v[s->n++] = v; }
static inline int boot_frt_cmp_i32(const void *a, const void *b) { const int32_t x = *(const int32_t *)a, y = *(const int32_t *)b; return (x > y) - (x < y); }
static inline int boot_frt_cmp_ev(const void *a, const void *b) {
    const uint64_t x = ((const BootFrtEv *)a)->qpc, y = ((const BootFrtEv *)b)->qpc; return (x > y) - (x < y);
}
static inline const char *boot_frt_pq(BootFrtSamples *s, char out[32]) {   /* "p50/p90", or "-" without samples */
    if (!s->n) { snprintf(out, 32, "-"); return out; }
    qsort(s->v, s->n, sizeof s->v[0], boot_frt_cmp_i32);
    snprintf(out, 32, "%d/%d", (int)s->v[s->n / 2], (int)s->v[(s->n * 9u) / 10u]);
    return out;
}
__attribute__((format(printf, 4, 5))) static inline void boot_frt_cat(char *s, size_t cap, size_t *at, const char *fmt, ...) {
    if (*at + 1 >= cap) return;
    va_list ap; va_start(ap, fmt); int n = vsnprintf(s + *at, cap - *at, fmt, ap); va_end(ap);
    if (n > 0) *at += (size_t)n < cap - *at ? (size_t)n : cap - *at - 1;
}
static inline const char *boot_frt_state_name(unsigned x) {
    switch (x) { case 2: return "runnable"; case 3: return "exited"; case 4: return "parked (lock)"; case 5: return "parked (handles)";
                 case 6: return "parked (arbiter)"; case 7: return "parked (condvar)"; case 8: return "sleeping"; default: return "unknown"; }
}
static inline int boot_frt_parked(unsigned x) { return x >= 4 && x <= 7; }   /* lock, handles, arbiter, condvar (not a sleep) */
/* t0's signal that released a park on (obj, key): SignalProcessWideKey names the condvar key, SignalToAddress and ArbitrateUnlock the
 * word (arbiter word or mutex); SignalEvent names the writable handle, never the one waited on: no match (its wake still counts). */
static inline int boot_frt_sig_match(uint32_t svc, uint64_t x0, uint64_t obj, uint64_t key) { return svc == 0x1d ? key && x0 == key : svc != 0x11 && obj && x0 == obj; }
static inline void boot_frt_pool_add(BootFrtState *S, const BootFrtEv *e, int first) {
    const uint64_t va = e->a ? e->a + e->d : 0, obj = e->a ? 0 : e->c;
    uint32_t i = 0;
    while (i < S->npool && !(S->pool[i].kind == e->kind && S->pool[i].va == va && S->pool[i].size == e->b && S->pool[i].obj == obj)) ++i;
    if (i == S->npool) {
        if (S->npool == BOOT_FRT_POOLS) { ++S->pool_over; return; }
        BootFrtPool *p = &S->pool[S->npool++]; memset(p, 0, sizeof *p); p->kind = e->kind; p->va = va; p->size = e->b; p->obj = obj;
    }
    BootFrtPool *p = &S->pool[i]; ++p->n;
    if (e->thread == S->waiter) ++p->waiter;
    if (first) ++p->first;
    uint32_t k = 0; while (k < p->nthreads && p->threads[k] != e->thread) ++k;
    if (k == p->nthreads && k < 4) p->threads[p->nthreads++] = e->thread;
}
static inline void boot_frt_watch_add(BootFrtState *S, uint64_t base, uint64_t off, uint64_t size) {
    for (uint32_t w = 0; w < S->nwatch; ++w) if (S->wbase[w] + S->woff[w] == base + off && S->wsize[w] == size) return;
    if (S->nwatch == BOOT_FRT_WATCH) return;
    const uint32_t w = S->nwatch; S->wbase[w] = base; S->woff[w] = off; S->wsize[w] = size; S->wsince[w] = S->presents;
    __atomic_store_n(&S->nwatch, w + 1, __ATOMIC_RELEASE);
    boot_gp_log("nvn native: fence-read trace: watching %llx+%llx size %llu (the waiter's first read after the fence, cycle %u): its storage-buffer bindings are counted",
                (unsigned long long)base, (unsigned long long)off, (unsigned long long)size, S->cycles);
}
/* One event of the closing or current cycle, for the detailed lines. */
static inline void boot_frt_cev(BootFrtState *S, const BootFrtEv *e) {
    BootFrtCycle *y = &S->cur;
    if (!y->detail) return;
    uint32_t *kind = e->kind == BOOT_FRT_SIG ? &y->nev_sig : e->kind == BOOT_FRT_PARK || e->kind == BOOT_FRT_WAKE ? &y->nev_park : NULL;
    if (y->nev < BOOT_FRT_CEV && (!kind || *kind < BOOT_FRT_CEV_KIND)) { y->ev[y->nev++] = *e; if (kind) ++*kind; } else ++y->more;
}
static inline void boot_frt_detail(BootFrtState *S, const BootFrtCycle *y, uint64_t hand, int via) {
    static char line[8192]; size_t at = 0; const uint32_t t2 = boot_frt_pub.t2;
    boot_frt_cat(line, sizeof line, &at, "nvn native: fence-read trace frame %u (present %u): t%u's wait on sync %llx value %llu took %d us (%s), t%u %s at its return; "
                 "first read %s%d us, hand-off %s%d us (%s); after the return (us, guest thread, event):",
                 y->index, y->present, S->waiter, (unsigned long long)y->sync, (unsigned long long)y->value, (int)boot_frt_us(S, y->wait0, y->f),
                 y->result == 0 ? "already signalled" : y->result == 1 ? "satisfied" : "timeout", t2, boot_frt_state_name(y->t2_at_f),
                 y->read ? "+" : "none ", y->read ? (int)boot_frt_us(S, y->f, y->read) : 0, hand ? "+" : "none ", hand ? (int)boot_frt_us(S, y->f, hand) : 0,
                 via == 1 ? "t0 signal on the parked object" : via == 2 ? "t0 signal on the learnt object" : via == 3 ? "t0 wake" : via == 4 ? "another thread's wake" : "-");
    for (uint32_t i = 0; i < y->nev; ++i) {
        const BootFrtEv *e = &y->ev[i]; const int us = (int)boot_frt_us(S, y->f, e->qpc);
        boot_frt_cat(line, sizeof line, &at, " | +%d t%u %s", us, (unsigned)e->thread, e->kind < BOOT_FRT_KINDS ? boot_frt_kind_name[e->kind] : "?");
        switch (e->kind) {
        case BOOT_FRT_W: boot_frt_cat(line, sizeof line, &at, " sync %llx value %llu rc %u (%d us)", (unsigned long long)e->a, (unsigned long long)e->b, e->d, (int)boot_frt_us(S, e->c, e->qpc)); break;
        case BOOT_FRT_FW: boot_frt_cat(line, sizeof line, &at, " again, value %llu", (unsigned long long)e->b); break;
        case BOOT_FRT_FIN: case BOOT_FRT_ACQ: boot_frt_cat(line, sizeof line, &at, " (%d us)", (int)boot_frt_us(S, e->c, e->qpc)); break;
        case BOOT_FRT_INV: case BOOT_FRT_FLU:
            if (e->a) boot_frt_cat(line, sizeof line, &at, " %llx+%x %llu", (unsigned long long)e->a, e->d, (unsigned long long)e->b);
            else boot_frt_cat(line, sizeof line, &at, " object %llx +%x %llu", (unsigned long long)e->c, e->d, (unsigned long long)e->b);
            break;
        case BOOT_FRT_BINV: case BOOT_FRT_BFLU: boot_frt_cat(line, sizeof line, &at, " object %llx +%x %llu", (unsigned long long)e->c, e->d, (unsigned long long)e->b); break;
        case BOOT_FRT_TS: {
            const uint64_t ns = (uint64_t)((double)e->qpc * 1e9 / (double)S->freq), ts = (uint64_t)((unsigned __int128)e->b * 625 / 384);
            if (!boot_guest_dilated() && ns > ts) boot_frt_cat(line, sizeof line, &at, " %llx (%d us old)", (unsigned long long)e->a, (int)((ns - ts) / 1000));
            else boot_frt_cat(line, sizeof line, &at, " %llx", (unsigned long long)e->a);
            break; }
        case BOOT_FRT_SUB: boot_frt_cat(line, sizeof line, &at, " %llu", (unsigned long long)e->a); break;
        case BOOT_FRT_FEN: boot_frt_cat(line, sizeof line, &at, " sync %llx value %llu%s", (unsigned long long)e->a, (unsigned long long)e->b, e->d ? " (frame)" : ""); break;
        case BOOT_FRT_PARK: boot_frt_cat(line, sizeof line, &at, " st%u %llx key %llx", (unsigned)e->x, (unsigned long long)e->a, (unsigned long long)e->b); break;
        case BOOT_FRT_WAKE: boot_frt_cat(line, sizeof line, &at, " by t%u after %d us (st%u %llx key %llx, x0 %x)", e->e, (int)((double)e->c * 1e6 / (double)S->freq), (unsigned)e->x,
                                         (unsigned long long)e->a, (unsigned long long)e->b, e->d); break;
        case BOOT_FRT_SIG: boot_frt_cat(line, sizeof line, &at, " svc %x %llx %llx", e->e, (unsigned long long)e->a, (unsigned long long)e->b); break;
        default: break;
        }
    }
    if (y->more) boot_frt_cat(line, sizeof line, &at, " | %u more", y->more);
    boot_gp_log("%s", line);
}
static inline void boot_frt_close(BootFrtState *S) {
    BootFrtCycle *y = &S->cur; S->open = 0;
    uint64_t hand = 0; int via = 0;   /* 1 t0's signal on the object t2 was parked on, 2 t0's signal on the learnt object (t2 not parked),
                                       * 3 t0's wake of t2, 4 another thread's wake of t2 (no wake by t0: a relay, or t2 was busy) */
    if (y->wake0) {
        for (uint32_t i = 0; i < y->nsig && !hand; ++i)
            if (y->sig[i].qpc <= y->wake0 && boot_frt_sig_match(y->sig[i].svc, y->sig[i].obj, y->wake_obj, y->wake_key)) { hand = y->sig[i].qpc; via = 1; }
        if (!hand) { hand = y->wake0; via = 3; }
        {   /* the hand-off object */
            uint32_t i = 0; while (i < S->nho && !(S->ho[i].obj == y->wake_obj && S->ho[i].key == y->wake_key && S->ho[i].state == y->wake_state)) ++i;
            if (i == S->nho) {
                if (S->nho < BOOT_FRT_HO) ++S->nho;
                else { i = 0; for (uint32_t k = 1; k < BOOT_FRT_HO; ++k) if (S->ho[k].n < S->ho[i].n) i = k; }
                S->ho[i].obj = y->wake_obj; S->ho[i].key = y->wake_key; S->ho[i].state = y->wake_state; S->ho[i].n = 0;
            }
            ++S->ho[i].n;
        }
    } else if (S->nho) {   /* t2 never parked for it: t0's signal on the most frequent hand-off object */
        uint32_t top = 0; for (uint32_t k = 1; k < S->nho; ++k) if (S->ho[k].n > S->ho[top].n) top = k;
        for (uint32_t i = 0; i < y->nsig && !hand; ++i)
            if (boot_frt_sig_match(y->sig[i].svc, y->sig[i].obj, S->ho[top].obj, S->ho[top].key)) { hand = y->sig[i].qpc; via = 2; }
    }
    if (y->wake0 > S->last_wake_used) S->last_wake_used = y->wake0;   /* review: this cycle's wake is spent (not counted as the next one's early hand-off) */
    if (!hand && y->wake) { hand = y->wake; via = 4; }
    if (y->wake) {   /* who woke t2 first after the fence */
        uint32_t k = 0; while (k < S->nwaker && S->waker[k].thread != y->wake_by) ++k;
        if (k == S->nwaker && k < 8) { S->waker[k].thread = y->wake_by; S->waker[k].n = 0; ++S->nwaker; }
        if (k < 8) ++S->waker[k].n;
    }
    ++S->w_cycles;
    if (y->read) boot_frt_sample(&S->f2r, boot_frt_us(S, y->f, y->read));
    if (y->read_any) boot_frt_sample(&S->f2a, boot_frt_us(S, y->f, y->read_any));
    if (hand) boot_frt_sample(&S->f2h, boot_frt_us(S, y->f, hand));
    if (y->read && hand) boot_frt_sample(&S->r2h, boot_frt_us(S, y->read, hand));
    if (y->wake) boot_frt_sample(&S->f2w, boot_frt_us(S, y->f, y->wake));
    if (y->sub) boot_frt_sample(&S->f2s, boot_frt_us(S, y->f, y->sub));
    boot_frt_sample(&S->wait, boot_frt_us(S, y->wait0, y->f));
    if (!y->read) ++S->w_noread;
    if (!hand) { if (y->woken_before) ++S->w_before; else ++S->w_nohand; }
    if (y->read && hand) { if (y->read < hand) ++S->w_rb; else ++S->w_hb; }
    if (boot_frt_parked(y->t2_at_f)) ++S->w_parked_f;
    if (y->read && boot_frt_parked(y->t2_at_read)) ++S->w_parked_read;
    if (via == 1) ++S->w_sig; else if (via == 2) ++S->w_sig_learnt; else if (via == 3) ++S->w_wake; else if (via == 4) ++S->w_wake_other;
    S->w_ts += y->ts; S->w_ts_waiter += y->ts_waiter;
    if (y->read && y->read_base && y->read_size) boot_frt_watch_add(S, y->read_base, y->read_off, y->read_size);
    if (y->detail) boot_frt_detail(S, y, hand, via);
}
static inline void boot_frt_take(BootFrtState *S, const BootFrtEv *e) {
    BootFrtCycle *y = &S->cur; const uint32_t t2 = boot_frt_pub.t2;
    if (e->kind == BOOT_FRT_FW) {
        if (S->waiter == BOOT_FRT_NONE) S->waiter = e->thread;
        if (e->thread == S->waiter) {
            if (S->open && e->b == y->value) { ++S->w_repeat; boot_frt_cev(S, e); return; }
            if (S->open) boot_frt_close(S);
            memset(y, 0, sizeof *y);
            y->f = e->qpc; y->wait0 = e->c; y->sync = e->a; y->value = e->b; y->result = e->d; y->t2_at_f = e->x;
            y->index = S->cycles++; y->present = S->presents;
            y->detail = y->index < 16 || (S->detail_at && y->index >= S->detail_at && y->index < S->detail_at + 16);
            y->woken_before = S->last_present && S->last_wake_waiter > S->last_present && S->last_wake_waiter > S->last_wake_used;   /* t0 woke t2 after its present, before this return */
            S->open = 1;
            return;
        }
    }
    if (e->kind == BOOT_FRT_PRE && e->thread == t2) S->last_present = e->qpc;
    if (e->kind == BOOT_FRT_WAKE && e->e == S->waiter && boot_frt_parked(e->x) && !e->d) S->last_wake_waiter = e->qpc;
    if (!S->open) return;
    if (e->qpc < y->f) { ++S->late; return; }
    boot_frt_cev(S, e);
    switch (e->kind) {
    case BOOT_FRT_INV: case BOOT_FRT_BINV: {
        ++y->inv; const int first = !y->read_any;
        if (first) { y->read_any = e->qpc; y->read_any_thread = e->thread; }
        if (e->thread == S->waiter && !y->read) { y->read = e->qpc; y->read_base = e->a; y->read_off = e->d; y->read_size = e->b; y->t2_at_read = e->x; }
        boot_frt_pool_add(S, e, first);
        break; }
    case BOOT_FRT_FLU: case BOOT_FRT_BFLU: boot_frt_pool_add(S, e, 0); break;
    case BOOT_FRT_TS:
        ++y->ts; if (e->thread == S->waiter) ++y->ts_waiter;
        if (!boot_guest_dilated()) {
            const uint64_t ns = (uint64_t)((double)e->qpc * 1e9 / (double)S->freq), ts = (uint64_t)((unsigned __int128)e->b * 625 / 384);
            if (ns > ts && ns - ts < UINT64_C(10000000000)) boot_frt_sample(&S->age, (int32_t)((ns - ts) / 1000));
        }
        break;
    case BOOT_FRT_SUB: if (e->thread == t2 && !y->sub) y->sub = e->qpc; break;
    case BOOT_FRT_WAKE:
        if (!y->wake && e->e < BOOT_THREAD_CAPACITY && boot_frt_parked(e->x) && !e->d) {
            y->wake = e->qpc; y->wake_by = e->e; }
        if (!y->wake0 && e->e == S->waiter && boot_frt_parked(e->x) && !e->d) { y->wake0 = e->qpc; y->wake_obj = e->a; y->wake_key = e->b; y->wake_state = e->x; }
        break;
    case BOOT_FRT_SIG:
        if (e->thread == S->waiter && y->nsig < BOOT_FRT_CSIG) { y->sig[y->nsig].qpc = e->qpc; y->sig[y->nsig].obj = e->a; y->sig[y->nsig].svc = e->e; ++y->nsig; }
        break;
    case BOOT_FRT_W: ++S->w_waits; break;
    case BOOT_FRT_FIN: ++S->w_fin; break;
    default: break;
    }
}
static inline unsigned boot_frt_drain(BootFrtState *S) {
    unsigned n = 0;
    for (BootFrtBuf *b = __atomic_load_n(&boot_frt_bufs, __ATOMIC_ACQUIRE); b; b = b->next) {
        const uint32_t h = __atomic_load_n(&b->head, __ATOMIC_ACQUIRE); uint32_t t = b->tail;
        for (; t != h && n < BOOT_FRT_BATCH; ++t) S->batch[n++] = b->ev[t % BOOT_FRT_BUF];
        __atomic_store_n(&b->tail, t, __ATOMIC_RELEASE);
    }
    {   const uint32_t h = __atomic_load_n(&boot_frt_kr.head, __ATOMIC_ACQUIRE); uint32_t t = boot_frt_kr.tail;
        for (; t != h && n < BOOT_FRT_BATCH; ++t) {
            const BootFrtK *k = &boot_frt_kr.ring[t % BOOT_FRT_KRING]; BootFrtEv *e = &S->batch[n++];
            e->qpc = k->qpc; e->a = k->obj; e->b = k->key; e->c = k->parked; e->d = k->result; e->e = k->other;
            e->thread = (uint16_t)k->thread; e->kind = k->kind == 'W' ? BOOT_FRT_WAKE : BOOT_FRT_PARK; e->x = (uint8_t)k->state;
        }
        __atomic_store_n(&boot_frt_kr.tail, t, __ATOMIC_RELEASE); }
    {   const uint32_t h = __atomic_load_n(&boot_frt_sr.head, __ATOMIC_ACQUIRE); uint32_t t = boot_frt_sr.tail;
        for (; t != h && n < BOOT_FRT_BATCH; ++t) {
            const BootFrtK *k = &boot_frt_sr.ring[t % BOOT_FRT_SRING]; BootFrtEv *e = &S->batch[n++];
            e->qpc = k->qpc; e->a = k->obj; e->b = k->key; e->c = k->parked; e->d = k->result; e->e = k->other;
            e->thread = (uint16_t)k->thread; e->kind = BOOT_FRT_SIG; e->x = 0xffu;
        }
        __atomic_store_n(&boot_frt_sr.tail, t, __ATOMIC_RELEASE); }
    return n;
}
/* shader-labels.tsv (<canonical id hex>\t<kind>\t<variant>\t<stem>\t<family>\t<program index>\t...), read once, streamed: the
 * label of each producer of watched range w ("?" when the file or the id is missing). */
static inline void boot_frt_labels(BootFrtState *S, uint32_t w) {
    char hex[BOOT_FRT_PROD][17]; uint32_t n = S->nprod[w], left = 0;
    for (uint32_t i = 0; i < n; ++i) {
        BootFrtProd *p = &S->prod[w][i]; snprintf(p->label, sizeof p->label, "?");
        for (unsigned b = 0; b < 8; ++b) snprintf(hex[i] + 2 * b, 3, "%02x", p->id[b]);
        left += p->has_id;
    }
    char path[600]; DWORD k = GetEnvironmentVariableA("TOTK_SHADER_LABELS", path, sizeof path);
    const char *tries[3] = {k && k < sizeof path ? path : NULL, "startup\\shader-labels.tsv", "shader-labels.tsv"};
    FILE *f = NULL;
    for (unsigned t = 0; t < 3 && !f && left; ++t) if (tries[t]) f = fopen(tries[t], "rb");
    if (!f) return;
    char line[1024];
    while (left && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        for (uint32_t i = 0; i < n; ++i) {
            BootFrtProd *p = &S->prod[w][i];
            if (!p->has_id || p->label[0] != '?' || strncmp(line, hex[i], 16)) continue;
            char *field[7] = {line, NULL, NULL, NULL, NULL, NULL, NULL}; unsigned nf = 1;
            for (char *q = line; *q && nf < 7; ++q) if (*q == '\t') { *q = 0; field[nf++] = q + 1; }
            for (unsigned j = 1; j < nf; ++j) { char *z = strpbrk(field[j], "\t\r\n"); if (z) *z = 0; }
            if (nf >= 6) snprintf(p->label, sizeof p->label, "%s %s%s%s #%s stem %s", field[4], field[1], strcmp(field[2], "-") ? "/" : "", strcmp(field[2], "-") ? field[2] : "", field[5], field[3]);
            --left; break;
        }
    }
    fclose(f);
}
static inline void boot_frt_producers(BootFrtState *S, uint32_t w) {
    static const char *const stage_name[6] = {"vs", "ps", "?", "?", "?", "cs"};
    static char line[8192]; size_t at = 0;
    boot_frt_labels(S, w);
    boot_frt_cat(line, sizeof line, &at, "nvn native: fence-read trace: pool %llx+%llx size %llu written by:", (unsigned long long)S->wbase[w], (unsigned long long)S->woff[w],
                 (unsigned long long)S->wsize[w]);
    uint32_t order[BOOT_FRT_PROD], n = S->nprod[w];
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    for (uint32_t i = 1; i < n; ++i) for (uint32_t j = i; j > 0; --j) {   /* storing first, then by bindings */
        const BootFrtProd *a = &S->prod[w][order[j - 1]], *b = &S->prod[w][order[j]];
        if (a->stores + a->learnt > b->stores + b->learnt || (a->stores + a->learnt == b->stores + b->learnt && a->binds >= b->binds)) break;
        uint32_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const BootFrtProd *p = &S->prod[w][order[i]]; char id[17] = "?";
        if (p->has_id) for (unsigned b = 0; b < 8; ++b) snprintf(id + 2 * b, 3, "%02x", p->id[b]);
        if (p->stage == 5) boot_frt_cat(line, sizeof line, &at, " [%s %s %s dispatches %u, learnt stores over it %u]", id, p->label, stage_name[5], p->binds, p->learnt);
        else boot_frt_cat(line, sizeof line, &at, " [%s %s %s storing draws %u]", id, p->label, stage_name[p->stage < 6 ? p->stage : 2], p->stores);
    }
    if (!n) boot_frt_cat(line, sizeof line, &at, " no dispatch and no storing draw had it bound since watched");
    boot_frt_cat(line, sizeof line, &at, "; draws with it bound: vertex stage %llu (storing %llu), fragment stage %llu (storing %llu)",
                 (unsigned long long)S->gfx_bound[w][0], (unsigned long long)S->gfx_stored[w][0], (unsigned long long)S->gfx_bound[w][1], (unsigned long long)S->gfx_stored[w][1]);
    boot_frt_cat(line, sizeof line, &at, "; other GPU writes into it: clear %llu, uniform update %llu, copy-to-buffer %llu, report %llu%s (since present %u)",
                 (unsigned long long)S->other[w][0], (unsigned long long)S->other[w][1], (unsigned long long)S->other[w][2], (unsigned long long)S->other[w][3],
                 S->prod_over[w] ? ", producer table full" : "", S->wsince[w]);
    boot_gp_log("%s", line);
}
static inline void boot_frt_summary(BootFrtState *S) {
    static char line[8192]; size_t at = 0; char p[8][32];
    uint32_t lost = __atomic_load_n(&boot_frt_nobuf, __ATOMIC_RELAXED) + boot_frt_kr.lost + boot_frt_sr.lost;
    for (BootFrtBuf *b = __atomic_load_n(&boot_frt_bufs, __ATOMIC_ACQUIRE); b; b = b->next) lost += __atomic_load_n(&b->lost, __ATOMIC_RELAXED);
    boot_frt_cat(line, sizeof line, &at, "nvn native: fence-read trace (TOTK_FENCE_READ_TRACE 1): presents 300 (to %u), frames %u (repeat frame waits %u), waiter t%u, render t%u: "
                 "read-before-handoff %u, handoff-before-read %u, no read %u, no handoff %u (t0 woke t2 before the fence instead %u); "
                 "fence->read us p50/p90 %s (any thread %s), fence->handoff us p50/p90 %s, read->handoff us p50/p90 %s (positive: read first), fence->t2 woken %s, fence->t2 submit %s, fence wait %s; "
                 "t2 parked at the fence return %u, at t0's first read %u; hand-off as t0 signal %u, as t0 signal on the learnt object %u, as t0 wake %u, as another thread's wake %u",
                 S->presents, S->w_cycles, S->w_repeat, S->waiter, boot_frt_pub.t2, S->w_rb, S->w_hb, S->w_noread, S->w_nohand, S->w_before,
                 boot_frt_pq(&S->f2r, p[0]), boot_frt_pq(&S->f2a, p[7]), boot_frt_pq(&S->f2h, p[1]), boot_frt_pq(&S->r2h, p[2]), boot_frt_pq(&S->f2w, p[3]), boot_frt_pq(&S->f2s, p[4]), boot_frt_pq(&S->wait, p[5]),
                 S->w_parked_f, S->w_parked_read, S->w_sig, S->w_sig_learnt, S->w_wake, S->w_wake_other);
    boot_frt_cat(line, sizeof line, &at, "; t2 first woken after the fence by:");
    for (uint32_t k = 0; k < S->nwaker; ++k) boot_frt_cat(line, sizeof line, &at, " t%u x%u", S->waker[k].thread, S->waker[k].n);
    if (!S->nwaker) boot_frt_cat(line, sizeof line, &at, " -");
    boot_frt_cat(line, sizeof line, &at, "; hand-off objects (all windows):");
    for (uint32_t k = 0; k < S->nho; ++k) boot_frt_cat(line, sizeof line, &at, " [%llx key %llx st%u x%u]", (unsigned long long)S->ho[k].obj, (unsigned long long)S->ho[k].key, S->ho[k].state, S->ho[k].n);
    if (!S->nho) boot_frt_cat(line, sizeof line, &at, " -");
    boot_frt_cat(line, sizeof line, &at, "; pools read after the fence [kind va size count by-waiter first-after-fence threads]:");
    uint32_t order[BOOT_FRT_POOLS], n = S->npool;
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    for (uint32_t i = 1; i < n; ++i) for (uint32_t j = i; j > 0 && S->pool[order[j - 1]].n < S->pool[order[j]].n; --j) { uint32_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        const BootFrtPool *q = &S->pool[order[i]];
        boot_frt_cat(line, sizeof line, &at, " [%s ", q->kind < BOOT_FRT_KINDS ? boot_frt_kind_name[q->kind] : "?");
        if (q->va) boot_frt_cat(line, sizeof line, &at, "%llx", (unsigned long long)q->va); else boot_frt_cat(line, sizeof line, &at, "object %llx", (unsigned long long)q->obj);
        boot_frt_cat(line, sizeof line, &at, " %llu %u %u %u", (unsigned long long)q->size, q->n, q->waiter, q->first);
        for (uint32_t k = 0; k < q->nthreads; ++k) boot_frt_cat(line, sizeof line, &at, " t%u", (unsigned)q->threads[k]);
        boot_frt_cat(line, sizeof line, &at, "]");
    }
    if (n > 8 || S->pool_over) boot_frt_cat(line, sizeof line, &at, " (+%u)", n > 8 ? n - 8 + S->pool_over : S->pool_over);
    if (!n) boot_frt_cat(line, sizeof line, &at, " -");
    boot_frt_cat(line, sizeof line, &at, "; timestamps per frame %.2f (waiter %.2f), age us p50/p90 %s; other waits %u, finishes %u; late events %u, lost %u",
                 S->w_cycles ? (double)S->w_ts / S->w_cycles : 0.0, S->w_cycles ? (double)S->w_ts_waiter / S->w_cycles : 0.0, boot_frt_pq(&S->age, p[6]),
                 S->w_waits, S->w_fin, S->late, lost);
    boot_gp_log("%s", line);
    for (uint32_t w = 0; w < S->nwatch; ++w)   /* once per watched range, after 300 presents of watching */
        if (!S->wlogged[w] && S->presents >= S->wsince[w] + 300) { S->wlogged[w] = 1; boot_frt_producers(S, w); }
    S->w_cycles = S->w_rb = S->w_hb = S->w_noread = S->w_nohand = S->w_before = S->w_parked_f = S->w_parked_read = 0;
    S->w_sig = S->w_sig_learnt = S->w_wake = S->w_wake_other = S->w_repeat = S->w_waits = S->w_fin = 0; S->w_ts = S->w_ts_waiter = 0;
    S->f2r.n = S->f2a.n = S->f2h.n = S->r2h.n = S->f2w.n = S->f2s.n = S->wait.n = S->age.n = 0;
    S->npool = S->pool_over = 0; S->nwaker = 0;
}
/* After each traced present of the render thread (renderer lock held by the caller; nothing here takes a lock). */
static inline void boot_frt_present(BootContext *c) {
    BootFrtState *S = boot_frt_state(); if (!S) return;
    const unsigned me = boot_nvn_guest_thread(c);
    if (__atomic_load_n(&boot_frt_pub.t2, __ATOMIC_RELAXED) == BOOT_FRT_NONE && me < BOOT_THREAD_CAPACITY) {
        __atomic_store_n(&boot_frt_pub.t2, me, __ATOMIC_RELEASE);
        __atomic_store_n(&boot_frt_k.watch, me, __ATOMIC_RELEASE);   /* kernel park / wake hooks (boot_mt_late.h) */
        boot_gp_log("nvn native: fence-read trace on (TOTK_FENCE_READ_TRACE 1; observation only; play-work/fence-read-trace/README.md): render thread t%u; "
                    "observed NOLOCK before the driver: nvnMemoryPool/nvnBuffer InvalidateMappedRange%s, nvnDeviceGetTimestampInNanoseconds; "
                    "wrapped: SubmitCommands, FenceSync, SyncWait, Finish, PresentTexture, AcquireTexture; kernel: t%u's parks and wakes, the frame waiter's signals; "
                    "detailed frames 0-15 and %u-%u; one line per 300 presents", me, boot_frt_fn_kind("nvnMemoryPoolFlushMappedRange") ? " and FlushMappedRange" : "",
                    me, S->detail_at, S->detail_at ? S->detail_at + 15 : 0);
    }
    ++S->presents;
    const unsigned n = boot_frt_drain(S);
    qsort(S->batch, n, sizeof *S->batch, boot_frt_cmp_ev);
    for (unsigned i = 0; i < n; ++i) boot_frt_take(S, &S->batch[i]);
    if (S->presents % 300 == 0) boot_frt_summary(S);
}

/* ---------------------------------------------------------------- wrappers of the owned queue / sync / window functions */
BOOT_NVN_FN(boot_frt_queue_submit) {
    boot_frt_note(c, BOOT_FRT_SUB, boot_nvn_qpc(), c->x[1], 0, 0, 0, 0, 0xffu);
    boot_frt_pub.since_submit = 1;   /* this thread's next FenceSync is a frame fence (P3's rule) */
    return boot_nvn_queue_submit(c, self, target, user);
}
BOOT_NVN_FN(boot_frt_queue_fence_sync) {
    const uint64_t sync = c->x[1];
    const int rc = boot_nvn_queue_fence_sync(c, self, target, user);
    if (rc != BOOT_HOST_DONE) return rc;
    BootNvnSync s; memset(&s, 0, sizeof s); boot_nvn_peek(c, self, sync, &s, sizeof s);
    const uint32_t frame = boot_frt_pub.since_submit; boot_frt_pub.since_submit = 0;
    if (frame) __atomic_store_n(&boot_frt_pub.frame_sync, sync, __ATOMIC_RELEASE);
    boot_frt_note(c, BOOT_FRT_FEN, boot_nvn_qpc(), sync, s.value, 0, frame, 0, 0xffu);
    return rc;
}
BOOT_NVN_FN(boot_frt_sync_wait) {
    const uint64_t sync = c->x[0], q0 = boot_nvn_qpc();
    BootNvnSync s; memset(&s, 0, sizeof s); boot_nvn_peek(c, self, sync, &s, sizeof s);   /* the value the wait reads at its entry */
    const int rc = boot_nvn_sync_wait(c, self, target, user);
    if (rc != BOOT_HOST_DONE) return rc;
    const uint64_t q1 = boot_nvn_qpc(), fs = __atomic_load_n(&boot_frt_pub.frame_sync, __ATOMIC_ACQUIRE);
    const unsigned me = boot_nvn_guest_thread(c), t2 = __atomic_load_n(&boot_frt_pub.t2, __ATOMIC_RELAXED);
    const int frame = fs && sync == fs && s.queue && me != t2;   /* the acquire sync (queue 0) is always signalled */
    if (frame && me < BOOT_THREAD_CAPACITY) {   /* the frame waiter: its signalling SVCs from here on (boot_mt_late.h, same thread) */
        if (__atomic_load_n(&boot_frt_k.sig_thread, __ATOMIC_RELAXED) == BOOT_FRT_NONE) __atomic_store_n(&boot_frt_k.sig_thread, me, __ATOMIC_RELEASE);
        if (me == boot_frt_k.sig_thread) boot_frt_sr.armed = BOOT_FRT_SIG_ARM;
    }
    boot_frt_note(c, frame ? BOOT_FRT_FW : BOOT_FRT_W, q1, sync, s.value, q0, (uint32_t)c->x[0], 0, boot_frt_t2_state());
    return rc;
}
BOOT_NVN_FN(boot_frt_queue_finish) {   /* user 0 Finish, 1 Flush (not noted) */
    const uint64_t q0 = boot_nvn_qpc();
    const int rc = boot_nvn_queue_finish(c, self, target, user);
    if (rc == BOOT_HOST_DONE && !user) boot_frt_note(c, BOOT_FRT_FIN, boot_nvn_qpc(), 0, 0, q0, 0, 0, boot_frt_t2_state());
    return rc;
}
BOOT_NVN_FN(boot_frt_window_acquire) {
    const uint64_t q0 = boot_nvn_qpc();
    const int rc = boot_nvn_window_acquire(c, self, target, user);
    boot_frt_note(c, BOOT_FRT_ACQ, boot_nvn_qpc(), 0, 0, q0, 0, 0, 0xffu);
    return rc;
}
BOOT_NVN_FN(boot_frt_queue_present) {
    const int rc = boot_nvn_queue_present(c, self, target, user);
    if (rc == BOOT_HOST_DONE) { boot_frt_note(c, BOOT_FRT_PRE, boot_nvn_qpc(), 0, 0, 0, 0, 0, 0xffu); boot_frt_present(c); }
    return rc;
}
/* host_nvn.h boot_nvn_native_register: the registered function of an owned name (its wrapper while the trace is on). */
static inline BootHostFunction boot_frt_own(BootHostFunction fn) {
    if (!boot_frt_on()) return fn;
    if (fn == boot_nvn_queue_submit) return boot_frt_queue_submit;
    if (fn == boot_nvn_queue_fence_sync) return boot_frt_queue_fence_sync;
    if (fn == boot_nvn_sync_wait) return boot_frt_sync_wait;
    if (fn == boot_nvn_queue_finish) return boot_frt_queue_finish;
    if (fn == boot_nvn_window_acquire) return boot_frt_window_acquire;
    if (fn == boot_nvn_queue_present) return boot_frt_queue_present;
    return fn;
}
#endif
