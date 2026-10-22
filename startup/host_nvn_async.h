/* host_nvn_async.h: asynchronous NVN translation (TOTK_ASYNC_XLATE): stage 1 "join" (=1) and stage 2 "async" (=2; 3 runs as 2 until the
 * early frame fence exists). Design: play-work/async-xlate-work/DESIGN.md (8.2) and PRECHECKS.md; what each stage changed, the counters
 * and the boot recipes: stage1/README.md, stage2/README.md. runtime_support only (TOTK_DRAW_PATH): included by host_nvn.h after
 * host_nvn_play.h, so the generated page header does not change.
 *
 * Why: t2 runs the game's render code and then our NVN -> D3D12 translation serially (~20 ms a frame each); the frame only gets shorter
 * once t2's guest code overlaps the translation. Stage 1 moved the translation to a private host thread X and kept every NVN semantic (t2
 * waits for X inside nvnQueueSubmitCommands; X runs inside t2's renderer-lock section). Stage 2 lets the submit return right after the
 * capture: X works through a FIFO of captured submissions and ops in call order while t2 records the next frame, and everything that
 * observes GPU completion goes through the same FIFO, so the game sees the order it saw before. The frame fence stays full (t0's wait on
 * frame N returns only once N is translated and its GPU work is done): the early return is stage 3.
 *
 *  - Locks (DESIGN 5.7). With TOTK_ASYNC_XLATE=2 at boot the queue / sync / window functions register without the renderer lock
 *    (boot_ax_own; the wrapper takes the lock for every other own function, and for these too once the live mode is below 2). X takes the
 *    renderer lock itself for each item, never across items, so graphics IPC, unmaps and the observed registry calls still exclude it. No
 *    thread waits for X holding the renderer lock (every level is released first), w->lock, the kernel, walk or store locks, and every wait
 *    leaves guest code (boot_ax_wait_done), so X's pack can park every core.
 *  - Virtual fences (DESIGN 5.1-5.2). nvnQueueFenceSync queues a FENCE op (X kicks the open list, as boot_nvn_exec_signal did) and the sync
 *    gets the op's sequence number tagged with BOOT_AX_VTAG. A tagged value is complete once X ran that item and the D3D12 fence passed the
 *    value X recorded after it (fence_of). A wait on one that is not complete queues an IDLE op (boot_nvn_exec_wait's complete wait: queue
 *    idle, deferred compute stores, pitch-linear targets to guest memory, texture trim) and waits for it, so what the game reads after the
 *    wait is in guest memory as before; nvnQueueFinish queues FINISH (the same). Infinite waits never give up (a watchdog line every 2 s),
 *    finite ones return TIMEOUT_EXPIRED, nothing is skipped or merged. Tagged values stay resolvable after the live mode narrows.
 *  - Present: t2 keeps the game's pacing and resolves the window texture through its own context; the PRESENT op runs the present body on
 *    X in order. nvnDeviceSetDepthMode / SetWindowOriginMode are ops too. nvnMemoryPoolFinalize drains first (DESIGN 5.5: a submission
 *    never translates over a pool unmapped after its capture); physical unmaps need no drain: boot_physical_memory refuses an unmap that
 *    overlaps a GPU mapping (boot_runtime.h), so GPU-mapped memory only goes with its pool.
 *  - Back-pressure (DESIGN 6): at most TOTK_ASYNC_XLATE_DEPTH (default 2) captured submissions not yet translated; t2 waits for room outside
 *    guest code. Degrade to join (5.8, counted): the walk did not cover the submission, a handle could not be read, the sentinel samples it.
 *    The kill file frames\async-xlate.request steps 2 -> 1 -> 0 live (X drains first).
 *  - The capture (both stages; stage 2 Part A): at the submit call t2 copies, through its own context and a snapshot of the GPU mappings
 *    (never renderer state), every uniform-buffer range the submission can bind (its summaries, the bindings it inherits from the previous
 *    submissions, X's published end state plus the summaries of submissions X has not finished) at exactly the size boot_render_cbv_ex
 *    copies (boot_render_cb_view_bytes: stage 1 copied the bound size, so every binding that was not a multiple of 256 bytes missed), and
 *    flags the ranges GPU work not yet idle may write (GPU order decides those: X reads them when it records, as before); in stage 2 also
 *    the learnt dynamic chunks the call-start copy takes (X publishes their list, X's boot_ds_capture then reads them from the capture). X's
 *    own reads of shared registries go through epochs: texture / sampler registrations and programs keep the entries they replace
 *    (host_gpu_render.h boot_ax_hist, host_nvn.h boot_nvn_prog_hist), texel writes after the capture wait for the next submission. What X
 *    still reads live is counted by class (the async-xlate line): GPU-ordered constant buffers, compute and program constants, shader code,
 *    texel data of texture builds and the re-hash, NVN objects not captured at record time. */
#ifndef TOTK_HOST_NVN_ASYNC_H
#define TOTK_HOST_NVN_ASYNC_H

_Static_assert(BOOT_NVN_AXR_CB == BOOT_AX_KIND_CB, "the constant-buffer capture selects uniform-buffer summaries by kind");
_Static_assert(sizeof(BootNvnAxr) == 2 * sizeof(uint64_t), "summaries go to the renderer as (address, bound) pairs");
_Static_assert(BOOT_DS_CHUNK_SHIFT == 16, "boot_ax_dyn_page and the chunk capture use 64 KiB chunks of 16 pages");
#define BOOT_AX_CORE_INDEX BOOT_MT_MAX   /* X's core index: one past every worker (boot_mt_pack_seen has the slot) */
#define BOOT_AX_VTAG (UINT64_C(1) << 62) /* a virtual fence value (a FIFO sequence number); D3D12 fence values never get there */
/* TOTK_ASYNC_XLATE_CHECK=1: the sentinel (below) on every TOTK_ASYNC_XLATE_CHECK_EVERY-th submission (default 16; in stage 2 those join) */
static inline int boot_ax_check_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_ASYNC_XLATE_CHECK", s, sizeof s); v = k && k < sizeof s && s[0] == '1'; }
    return v;
}
static inline uint32_t boot_ax_check_every(void) {
    static uint32_t v; if (!v) { char s[16]; DWORD k = GetEnvironmentVariableA("TOTK_ASYNC_XLATE_CHECK_EVERY", s, sizeof s); unsigned long n = k && k < sizeof s ? strtoul(s, NULL, 10) : 16;
                                 v = n < 1 ? 1 : n > 100000 ? 100000 : (uint32_t)n; }
    return v;
}
/* TOTK_ASYNC_XLATE_CB=0 (A/B): no constant-buffer capture; X reads every constant buffer at translation (each binding data-snapshot takes then
 * counts as "not summarized"). */
static inline int boot_ax_cb_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_ASYNC_XLATE_CB", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0'); }
    return v;
}
/* TOTK_ASYNC_XLATE_CAPTURE_MB (default 48, 1-1024): one submission's capture arena (constant buffers and call-start chunks), reserved once per
 * submission object and committed on demand in 1 MiB steps; a range past it is read at translation, counted "over budget" */
static inline uint64_t boot_ax_capture_bytes(void) {
    static uint64_t v; if (!v) { char s[16]; DWORD k = GetEnvironmentVariableA("TOTK_ASYNC_XLATE_CAPTURE_MB", s, sizeof s); unsigned long mb = k && k < sizeof s ? strtoul(s, NULL, 10) : 48;
                                 if (mb < 1) mb = 1; if (mb > 1024) mb = 1024; v = (uint64_t)mb << 20; }
    return v;
}
/* TOTK_ASYNC_XLATE_DEPTH (stage 2, default 2, 1-4): captured submissions not yet translated before the submit waits (DESIGN 6) */
static inline uint32_t boot_ax_depth(void) {
    static uint32_t v; if (!v) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_ASYNC_XLATE_DEPTH", s, sizeof s); unsigned long n = k && k < sizeof s ? strtoul(s, NULL, 10) : 2;
                                 v = n < 1 ? 1 : n > 4 ? 4 : (uint32_t)n; }
    return v;
}

/* ---- the FIFO: submissions and ops in call order. Pushers get sequence numbers 1, 2, ...; X runs the head item, records its D3D12 value
 * (fence_of) and publishes it done; a waiter sleeps on `progress`. Flags: DELEGATED (stage 1 join: the pusher holds the renderer lock and
 * waits, X runs inside its section and never takes the lock), JOIN (the pusher waits and takes the submission back), XDIAG (X runs the
 * call's data-snapshot / suspect-diag begin and end: stage 2). */
#define BOOT_AX_FIFO 64
#define BOOT_AX_PAY 96
enum { BOOT_AX_OP_SUBMIT = 1, BOOT_AX_OP_PRESENT, BOOT_AX_OP_FINISH, BOOT_AX_OP_FLUSH_PITCH, BOOT_AX_OP_DEPTH_MODE, BOOT_AX_OP_ORIGIN_MODE,
       BOOT_AX_OP_HOUSEKEEP, BOOT_AX_OP_DRAIN, BOOT_AX_OP_FENCE, BOOT_AX_OP_IDLE, BOOT_AX_OPS };   /* DESIGN 5.2; FLUSH_PITCH / HOUSEKEEP: IDLE does both */
enum { BOOT_AX_IT_DELEGATED = 1, BOOT_AX_IT_JOIN = 2, BOOT_AX_IT_XDIAG = 4 };
typedef struct BootAxItem { uint32_t op, arg, flags, npay; uint64_t seq; void *sub; uint8_t pay[BOOT_AX_PAY]; } BootAxItem;
typedef struct BootAxFifo {
    SRWLOCK lock; CONDITION_VARIABLE work, progress;   /* zero = SRWLOCK_INIT / CONDITION_VARIABLE_INIT */
    BootAxItem q[BOOT_AX_FIFO]; uint64_t head, tail;   /* under lock: q[head..tail) queued, q[head] the one X runs */
    uint64_t seq, done;                                /* under lock (done also read with acquire): the last sequence number given out / finished */
} BootAxFifo;
/* Queues an item: its sequence number, 0 when the ring is full (or, need_empty, when anything is queued: a delegated join must be the only
 * item, else X would wait for the renderer lock its pusher holds). */
static inline uint64_t boot_ax_fifo_push_ex(BootAxFifo *f, uint32_t op, uint32_t arg, uint32_t flags, void *sub, const void *pay, uint32_t npay, int need_empty) {
    uint64_t s = 0;
    AcquireSRWLockExclusive(&f->lock);
    if (f->tail - f->head < BOOT_AX_FIFO && (!need_empty || f->head == f->tail)) {
        s = ++f->seq; BootAxItem *it = &f->q[f->tail++ % BOOT_AX_FIFO]; it->op = op; it->arg = arg; it->flags = flags; it->seq = s; it->sub = sub;
        it->npay = pay && npay <= BOOT_AX_PAY ? npay : 0; if (it->npay) memcpy(it->pay, pay, npay);
    }
    ReleaseSRWLockExclusive(&f->lock);
    if (s) WakeConditionVariable(&f->work);
    return s;
}
static inline uint64_t boot_ax_fifo_push(BootAxFifo *f, uint32_t op, uint32_t arg, void *sub) { return boot_ax_fifo_push_ex(f, op, arg, 0, sub, NULL, 0, 0); }
/* X: the item at the head (waits for one); it stays queued until boot_ax_fifo_done. */
static inline BootAxItem boot_ax_fifo_next(BootAxFifo *f) {
    AcquireSRWLockExclusive(&f->lock);
    while (f->head == f->tail) SleepConditionVariableSRW(&f->work, &f->lock, INFINITE, 0);
    const BootAxItem it = f->q[f->head % BOOT_AX_FIFO];
    ReleaseSRWLockExclusive(&f->lock);
    return it;
}
/* X: the head item is finished; its sequence number is published (release: everything X wrote for it, fence_of included, is visible to a
 * waiter that sees it) and every waiter wakes. */
static inline void boot_ax_fifo_done(BootAxFifo *f, uint64_t seq) {
    AcquireSRWLockExclusive(&f->lock); ++f->head; __atomic_store_n(&f->done, seq, __ATOMIC_RELEASE); __atomic_store_n(&boot_ax_fifo_done_seq, (LONG64)seq, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&f->lock);
    WakeAllConditionVariable(&f->progress);
}
static inline uint64_t boot_ax_fifo_done_now(const BootAxFifo *f) { return __atomic_load_n(&f->done, __ATOMIC_ACQUIRE); }
/* Waits until item seq is finished or the QPC deadline (0: none) passes; 1 when finished. The caller holds no lock X needs and is outside
 * guest code (DESIGN 5.7.4). Every tick_ms without progress, tick(arg, ms waited) runs outside the FIFO lock (the watchdog: infinite waits
 * never give up, they log). */
static inline int boot_ax_fifo_wait_until(BootAxFifo *f, uint64_t seq, uint64_t deadline, DWORD tick_ms, void (*tick)(void *, uint64_t), void *arg, uint64_t *ticks) {
    uint64_t t = 0; int ok;
    AcquireSRWLockExclusive(&f->lock);
    for (;;) {
        if (f->done >= seq) { ok = 1; break; }
        DWORD ms = tick_ms;
        if (deadline) {
            const uint64_t now = boot_nvn_qpc();
            if (now >= deadline) { ok = 0; break; }
            const uint64_t left = (deadline - now) * 1000 / boot_nvn_qpf() + 1; if (left < ms) ms = (DWORD)left;
        }
        if (!SleepConditionVariableSRW(&f->progress, &f->lock, ms, 0) && GetLastError() == ERROR_TIMEOUT && f->done < seq && ms == tick_ms) {
            ++t;
            if (tick) { ReleaseSRWLockExclusive(&f->lock); tick(arg, t * tick_ms); AcquireSRWLockExclusive(&f->lock); }
        }
    }
    ReleaseSRWLockExclusive(&f->lock);
    if (ticks) *ticks += t;
    return ok;
}
static inline uint64_t boot_ax_fifo_wait(BootAxFifo *f, uint64_t seq, DWORD tick_ms, void (*tick)(void *, uint64_t), void *arg) {
    uint64_t t = 0; boot_ax_fifo_wait_until(f, seq, 0, tick_ms, tick, arg, &t); return t;
}

/* ---- the capture map: an immutable GPU VA -> guest CPU VA index of the GPU mappings, by the executor's rule (the first address space,
 * mappings with CPU backing; a later table entry wins where two overlap, as boot_render_map writes them in table order). t2 keeps a copy of
 * the mapping table and compares it with the live one at each capture without the window lock (a writer changes an entry field by field
 * under w->lock exclusive, so a compare that finds every entry equal saw the table as it was before any change still in flight); only a
 * difference takes w->lock shared, copies the table and rebuilds the index (rare: pool creation, loading). */
typedef struct BootAxMapEnt { uint64_t gpu, end, cpu; uint32_t order, pad; } BootAxMapEnt;
typedef struct BootAxMap { BootGpuMapping *copy; BootAxMapEnt *e; uint32_t n, cap; uint64_t ver; } BootAxMap;
static inline int boot_ax_map_order(const void *a, const void *b) {
    const BootAxMapEnt *x = (const BootAxMapEnt *)a, *y = (const BootAxMapEnt *)b; return x->gpu < y->gpu ? -1 : x->gpu > y->gpu ? 1 : (x->order > y->order) - (x->order < y->order);
}
/* Rebuilds the index from m->copy. 0: out of memory. */
static inline int boot_ax_map_index(BootAxMap *m) {
    uint32_t as_fd = UINT32_MAX, n = 0;
    if (!m->e) { m->e = (BootAxMapEnt *)malloc((size_t)BOOT_GPU_MAPPING_CAPACITY * sizeof *m->e); if (!m->e) return 0; m->cap = BOOT_GPU_MAPPING_CAPACITY; }
    for (uint32_t i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *g = &m->copy[i];
        if (!g->size) continue;
        if (as_fd == UINT32_MAX) as_fd = g->as_fd;
        const uint64_t b = boot_gpu_mapping_backing_size(g);
        if (g->as_fd != as_fd || !g->cpu || !b || g->gpu > UINT64_MAX - b) continue;
        m->e[n].gpu = g->gpu; m->e[n].end = g->gpu + b; m->e[n].cpu = g->cpu; m->e[n].order = i; m->e[n].pad = 0; ++n;
    }
    qsort(m->e, n, sizeof *m->e, boot_ax_map_order); m->n = n; ++m->ver;
    return 1;
}
/* The mapping that backs GPU VA va (the highest table order among those that contain it, looking back up to 8 entries), NULL if none. */
static inline const BootAxMapEnt *boot_ax_map_find(const BootAxMap *m, uint64_t va) {
    uint32_t lo = 0, hi = m->n;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (m->e[mid].gpu <= va) lo = mid + 1; else hi = mid; }
    const BootAxMapEnt *hit = NULL;
    for (uint32_t k = lo; k-- > 0 && lo - k <= 8;) if (va < m->e[k].end && (!hit || m->e[k].order > hit->order)) hit = &m->e[k];
    return hit;
}
/* t2 at the capture: the index current with the live mapping table (c: the caller's context, outside guest code while it waits for the
 * window lock, which X holds through a translation). *rebuilt: it was rebuilt now. 0: no index (out of memory). */
static inline int boot_ax_map_sync(BootAxMap *m, BootWindow *w, BootContext *c, int *rebuilt) {
    const size_t bytes = sizeof(BootGpuMappings); int changed = 0;
    *rebuilt = 0;
    if (!m->copy) { m->copy = (BootGpuMapping *)calloc(1, bytes); if (!m->copy) return 0; changed = 1; }
    else changed = memcmp(m->copy, w->gpu_mappings.entries, bytes) != 0;
    if (!changed && m->e) return 1;
    BootCore *k = boot_ov_leave_guest(c);
    AcquireSRWLockShared(&w->lock); memcpy(m->copy, w->gpu_mappings.entries, bytes); ReleaseSRWLockShared(&w->lock);
    boot_ov_enter_guest(k);
    *rebuilt = 1;
    return boot_ax_map_index(m);
}
/* Reads GPU VA [va, va + n) through the map and this thread's context into dst, page by page: a page without CPU backing (or that the guest
 * cannot read) reads zero, as boot_render_cbv_ex's page path does. 0 when the first byte has no backing (cbv_ex fails such a binding); *zero:
 * some page read zero. */
static inline int boot_ax_read_gpu(BootContext *c, uint64_t self, const BootAxMap *m, uint64_t va, uint8_t *dst, uint64_t n, int *zero) {
    *zero = 0;
    const BootAxMapEnt *e = boot_ax_map_find(m, va);
    if (!e) return 0;
    if (n <= e->end - va && boot_nvn_peek(c, self, e->cpu + (va - e->gpu), dst, (size_t)n) == n) return 1;   /* the usual case: one mapping */
    int first = 1;
    for (uint64_t at = 0; at < n;) {
        uint64_t piece = 4096 - ((va + at) & 4095); if (piece > n - at) piece = n - at;
        const BootAxMapEnt *p = boot_ax_map_find(m, va + at); size_t got = 0;
        if (p && piece <= p->end - (va + at)) got = boot_nvn_peek(c, self, p->cpu + (va + at - p->gpu), dst + at, (size_t)piece);
        if (first && !got) return 0;
        if (got < piece) { memset(dst + at + got, 0, (size_t)(piece - got)); *zero = 1; }
        first = 0; at += piece;
    }
    return 1;
}

/* ---- a captured submission */
typedef struct BootAxObj { uint64_t *key, *val; uint32_t hcap, n; } BootAxObj;   /* NVN object words: guest CPU address -> 8 bytes at the call */
typedef struct BootAxSub {
    uint64_t queue, self, window; DWORD tid; int check, ds_cap;   /* window: the submitting context's window_native; tid: its host thread */
    uint64_t *h; uint32_t nh, hcap;                               /* the handles as the call read them (a prefix when a read failed) */
    BootNvnPinSet pins;                                           /* the call's pins while they are out of t2's thread-local set */
    BootNvnDsnap ds;                                              /* stage 2: the call's walk table (freeze, deferred binds), X reads it */
    int own_ds, cap_ok, dyn_ok;                                   /* own_ds: X translates through ds (else t2's table: stage 1) */
    BootAxCap cap;                                                /* constant-buffer bytes and call-start chunks (host_gpu_render.h) */
    BootAxDyn dyn;                                                /* stage 2: the call-start chunks in cap's arena */
    BootAxObj obj;
    uint64_t *cbr; uint32_t ncbr, cbr_cap;                        /* its uniform-buffer summaries (address, bound size): later captures carry them */
    uint64_t reg_epoch, ds_cap_bytes, seq;                        /* registry epoch at the capture; call-start copy bytes taken by t2; FIFO item */
    uint32_t flags; volatile LONG in_use;                         /* item flags; taken by t2 until X (async) or t2 (join) gives it back */
    volatile LONG k;                                              /* X: the handle being translated (watchdog) */
    uint32_t done, fail_k; int failed;                            /* X: handles translated; the first that failed */
    uint32_t status; uint64_t fault_pc, fault_address, fault_width;   /* X's context after the submission (a failed guest read) */
    uint64_t q_cap0, q_enq, q_x0, q_x1, q_ret, pins_in, pins_out;
} BootAxSub;
#define BOOT_AX_SUBS 5   /* submission objects: up to TOTK_ASYNC_XLATE_DEPTH (4 at most) queued plus one joining */
/* why a submission was translated on the submitting thread (today's loop) */
enum { BOOT_AX_WHY_SINGLE, BOOT_AX_WHY_THREAD, BOOT_AX_WHY_NODSNAP, BOOT_AX_WHY_REQUEST, BOOT_AX_WHY_WINDOW, BOOT_AX_WHY_COUNT, BOOT_AX_WHY_MEMORY, BOOT_AX_WHYS };
static const char *const boot_ax_why_name[BOOT_AX_WHYS] = {"single host thread", "X not started", "no walk", "live request 0", "no window", "handle count", "out of memory"};
/* stage 2: why a submission joined instead of returning at once (DESIGN 5.8) */
enum { BOOT_AX_DEG_COVER, BOOT_AX_DEG_READ, BOOT_AX_DEG_SENTINEL, BOOT_AX_DEGS };
static const char *const boot_ax_deg_name[BOOT_AX_DEGS] = {"walk not covered", "handle read failed", "sentinel sample"};
/* waits for X, by kind (virtual fences and the rest) */
enum { BOOT_AX_W_SYNC, BOOT_AX_W_SYNC_TIMEOUT, BOOT_AX_W_FINISH, BOOT_AX_W_JOIN, BOOT_AX_W_ROOM, BOOT_AX_W_RING, BOOT_AX_W_POOL,
       BOOT_AX_W_MODE, BOOT_AX_W_CORE, BOOT_AX_W_FALLBACK, BOOT_AX_W_GPU, BOOT_AX_WS };
static const char *const boot_ax_w_name[BOOT_AX_WS] = {"sync wait", "sync wait timed out", "finish", "join", "back-pressure (queue full)", "FIFO ring full",
    "drain at pool finalize", "drain at a mode step", "drain for another core", "drain before today's loop", "D3D12 fence after X"};
/* the capture: ranges, duplicates, flagged GPU-ordered, first page unmapped, over budget, some pages zero, ranges carried from earlier submissions
 * (X's published bindings / submissions X has not finished) */
enum { BOOT_AX_CAPS_RANGES, BOOT_AX_CAPS_DUPS, BOOT_AX_CAPS_GPU, BOOT_AX_CAPS_UNMAPPED, BOOT_AX_CAPS_OVER, BOOT_AX_CAPS_ZERO, BOOT_AX_CAPS_CARRY, BOOT_AX_CAPS_INFLIGHT, BOOT_AX_CAPS_N };
typedef struct BootAxCount {   /* cumulative; the 300-present line prints the differences */
    uint64_t join, async, sync[BOOT_AX_WHYS], degrade[BOOT_AX_DEGS], translated, fail, read_fail, order_alarms, sticky, watchdog, ops[BOOT_AX_OPS], ops_done[BOOT_AX_OPS];
    uint64_t q_cap, q_x, q_x_async, q_wait, q_handoff, q_call_async, cb_bytes, ds_cap_t2, ds_cap_x, ds_cap_bytes, caps[BOOT_AX_CAPS_N], axr[BOOT_NVN_AXR_N];
    uint64_t obj_words, obj_unread, pins_in, pins_added, reg_calls, reg_during, reg_queued, reg_subs;
    uint64_t depth_hist[5], w_n[BOOT_AX_WS], w_qpc[BOOT_AX_WS], w_max[BOOT_AX_WS], sync_ready, sync_poll, sync_d3d12;
    uint64_t map_rebuilds, map_qpc, wr_over, dyn_chunks, dyn_bytes, dyn_pages_missed, steps;
    uint64_t chk_subs, chk_recs, chk_rec_bad, chk_handles, chk_handle_bad, chk_unfrozen, chk_cbs, chk_cb_same, chk_cb_saved, chk_cb_raced, chk_cb_gpu, chk_cb_unread;
    uint64_t chk_draws, chk_draw_eq, chk_draw_rec, chk_draw_saved, chk_draw_raced, chk_draw_gpu, chk_lost;
} BootAxCount;
#define BOOT_AX_FENCES 4096u   /* fence_of ring (items) */
#define BOOT_AX_WR 16384u      /* write ranges of submissions not yet idle */
#define BOOT_AX_CHUNKS 1024u   /* the published call-start chunk list */
typedef struct BootAxFenceOf { uint64_t fence; const BootGpu *gpu; } BootAxFenceOf;
typedef struct BootAxWr { uint64_t seq, va, end; } BootAxWr;
static struct {
    BootAxFifo f;
    volatile LONG started, busy; HANDLE thread; DWORD tid; INIT_ONCE start_once;
    volatile LONG64 cur_seq; volatile LONG cur_op;
    uint64_t x_last;                              /* X: the previous item's sequence number (order check) */
    volatile LONG live; uint32_t req_tick; INIT_ONCE live_once; const char *live_why;   /* the live mode: 2 async, 1 join, 0 today's loop */
    BootAxSub sub[BOOT_AX_SUBS];                  /* in_use under the FIFO lock */
    uint32_t inflight;                            /* under the FIFO lock: async submissions captured and not finished */
    BootCore core; const BootCore *core_from;     /* X's private context and the core it was copied from */
    BootAxFenceOf fence_of[BOOT_AX_FENCES];       /* X: the D3D12 value after each item (before it publishes the item done) */
    SRWLOCK vlock; volatile LONG64 vdone;         /* items done by X whose D3D12 value completed: every one up to vdone */
    const BootGpu *volatile gpu;                  /* X: the renderer's GPU after its last item */
    /* published by X under the FIFO lock (t2 reads them at the capture) */
    uint64_t carry[96][2], carry_seq; uint32_t ncarry;   /* the uniform-buffer bindings after the newest translated submission */
    uint64_t chunks[BOOT_AX_CHUNKS]; uint32_t nchunks;  /* the chunks its next call-start copy takes */
    /* t2: the capture's map, the write ranges of submissions not yet idle, scratch */
    BootAxMap map; BootAxWr wr[BOOT_AX_WR]; uint64_t wr_head, wr_tail, wr_over_seq; uint64_t (*wr_iv)[2]; uint32_t nwr_iv, wr_iv_cap;
    uint64_t *ranges; uint32_t nranges, ranges_cap;
    /* a translation that failed on X after its call returned: the next NVN call stops the game (DESIGN 5.7.8) */
    volatile LONG sticky_on; uint64_t sticky_handle; uint32_t sticky_status; uint64_t sticky_pc, sticky_address, sticky_width;
    BootAxCount c; uint64_t handoff_max;
} boot_ax;   /* zero: SRWLOCK_INIT, CONDITION_VARIABLE_INIT, INIT_ONCE_STATIC_INIT */
_Static_assert(sizeof(BootAxPresentPay) <= BOOT_AX_PAY, "a PRESENT op's payload fits the FIFO item");
static const BootAxObj *boot_ax_obj_cur;    /* X: the object words of that submission */
static const BootAxCap boot_ax_cap_none;    /* X: the capture of a submission without one (no constant-buffer summaries): every taken binding counts as missed */
static BootAxChkLog boot_ax_chk;            /* the sentinel log (host_gpu_render.h boot_ax_chk_log points here for a sampled submission) */
static const char *const boot_ax_live_name[BOOT_AX_L_N] = {"deferred binds", "NVN objects", "object words", "shader control sections", "diagnostic compares",
                                                           "shader code", "texture builds from guest memory", "texture re-hash"};

static inline uint32_t boot_ax_obj_slot(uint64_t a, uint32_t hcap) { a ^= a >> 31; a *= UINT64_C(0x94d049bb133111eb); a ^= a >> 29; return (uint32_t)a & (hcap - 1); }
static inline int boot_ax_obj_find(const BootAxObj *o, uint64_t a, uint64_t *v) {
    if (!o || !o->hcap || !a) return 0;
    for (uint32_t m = o->hcap - 1, j = boot_ax_obj_slot(a, o->hcap), k = 0; k <= m && o->key[j]; j = (j + 1) & m, ++k)
        if (o->key[j] == a) { *v = o->val[j]; return 1; }
    return 0;
}
/* X: the object word t2 read at guest address a during the capture (1), or 0 (read it live). */
static inline int boot_ax_obj_word(uint64_t a, uint64_t *v) { return boot_ax_on_x && boot_ax_obj_find(boot_ax_obj_cur, a, v); }
static inline int boot_ax_obj_put(BootAxObj *o, uint64_t a, uint64_t v) {
    if (!a || !o->hcap || 2 * (o->n + 1) > o->hcap) return 0;
    uint32_t m = o->hcap - 1, j = boot_ax_obj_slot(a, o->hcap);
    while (o->key[j] && o->key[j] != a) j = (j + 1) & m;
    if (!o->key[j]) { o->key[j] = a; ++o->n; }
    o->val[j] = v; return 1;
}
static inline int boot_ax_obj_begin(BootAxObj *o, uint32_t words) {
    uint32_t h = 256; while (h < 2u * words + 2 && h < (1u << 22)) h *= 2;
    if (h > o->hcap) {
        uint64_t *k = (uint64_t *)realloc(o->key, (size_t)h * 8); if (k) o->key = k;
        uint64_t *v = k ? (uint64_t *)realloc(o->val, (size_t)h * 8) : NULL; if (v) o->val = v;
        if (!k || !v) { o->hcap = 0; o->n = 0; return 0; }
        o->hcap = h;
    }
    memset(o->key, 0, (size_t)o->hcap * 8); o->n = 0;
    return 1;
}

/* ---- registry epoch: the observed calls that change what the translation resolves (texture / sampler registrations, programs, texel
 * writes; boot_nvn_hybrid_observe, before the call's own effect). Stage 1: they hold the renderer lock, so none lands while X translates.
 * Stage 2: still renderer-locked, so none lands inside an item, but they land between a capture and its translation: the entries they
 * replace stay readable for the submissions captured before (boot_ax_hist, boot_nvn_prog_hist), counted. */
static inline void boot_ax_reg_note(int index) {
    static int ix[7] = {-2, -2, -2, -2, -2, -2, -2};
    static const char *const names[7] = {"nvnTexturePoolRegisterTexture", "nvnSamplerPoolRegisterSampler", "nvnProgramSetShaders", "nvnProgramSetShadersExt",
                                         "nvnTextureWriteTexels", "nvnTextureWriteTexelsStrided", "nvnTextureFlushTexels"};
    int hit = 0;
    for (unsigned i = 0; i < 7 && !hit; ++i) { if (ix[i] == -2) ix[i] = boot_nvn_name_index(names[i]); hit = index == ix[i]; }
    if (!hit) return;
    InterlockedIncrement64(&boot_ax_reg_epoch); InterlockedIncrement64((volatile LONG64 *)&boot_ax.c.reg_calls);
    if (__atomic_load_n(&boot_ax.busy, __ATOMIC_ACQUIRE) && __atomic_load_n(&boot_ax.cur_op, __ATOMIC_ACQUIRE) == BOOT_AX_OP_SUBMIT) {
        const LONG64 n = InterlockedIncrement64((volatile LONG64 *)&boot_ax.c.reg_during);
        if (n <= 8) boot_draw_log("nvn native: async-xlate: registry call %s while X translates (host thread %llu; must stay 0: X holds the renderer lock)", boot_nvn_names[index].name, GetCurrentThreadId(), 0);
    }
    if (__atomic_load_n(&boot_ax.f.seq, __ATOMIC_ACQUIRE) > boot_ax_fifo_done_now(&boot_ax.f)) InterlockedIncrement64((volatile LONG64 *)&boot_ax.c.reg_queued);
}

/* ---- the sentinel (TOTK_ASYNC_XLATE_CHECK=1). A D3D12-level comparison would translate the submission twice (ring, descriptors,
 * texture builds and GPU-ordered guest writes have side effects), so the comparison is one level up, on what decides the D3D12
 * stream: for a sampled submission X logs every record it reads (address, size, a hash of the decoded record; for a driver handle the
 * recording it resolved it to) and every constant-buffer copy (range, a hash of the bytes it put in the ring before overlays, and
 * whether they came from the capture). After the join t2 replays the log the old way: it re-reads each record through its own
 * boot_nvn_record_read (its pins, its thread identity, the call's freeze), checks each handle against the call's freeze table, and
 * re-hashes each constant-buffer range from guest memory now. Per draw (a draw record and the copies after it) the verdict is: equal;
 * a record before it differs (X read other commands: pins / freeze / thread identity, must stay 0); its constant buffers changed
 * since the capture (the bytes the capture saved: X drew the submit-time bytes, the old way would have drawn newer ones); or changed
 * since X read them live (a race the capture did not cover); a binding data-snapshot does not take that changed is GPU order's (GPU-only
 * pools, pending GPU writes: the GPU may still be writing them), counted apart. In stage 2 the sampled submissions join (degrade reason
 * "sentinel sample"), so the replay runs with X idle as in stage 1. */
#define BOOT_AX_CHK_CAP (1u << 18)   /* log entries per sampled submission (8 MiB); more are counted "log full" */
static inline uint64_t boot_ax_rec_hash(const BootNvnRecord *r) {
    uint64_t h = boot_ax_hash(&r->op, 8, BOOT_AX_H0);   /* op and words */
    h = boot_ax_hash(&r->x[1], 7 * sizeof r->x[0], h); h = boot_ax_hash(r->q, sizeof r->q, h);
    for (uint32_t b = 0; b < r->blocks && b < 2; ++b) {
        h = boot_ax_hash(&r->block[b].arg, 8, h);
        if (r->block[b].data && r->block[b].bytes) h = boot_ax_hash(r->block[b].data, r->block[b].bytes, h);
    }
    return h;
}
/* X, boot_nvn_exec_handle: the record the translation got at `at` (a driver handle: the JUMP to the recording it resolved). */
static inline void boot_ax_chk_rec(uint64_t at, const BootNvnRecord *rec, uint64_t size) {
    BootAxChkLog *L = boot_ax_chk_log;
    if (!L) return;
    if (L->n >= L->cap) { ++L->lost; return; }
    BootAxChkEnt *e = &L->e[L->n++]; e->at = at; e->va = 0; e->n = (uint32_t)size; e->kind = (uint32_t)BOOT_AX_CHK_REC << 24 | (rec->op & 0xffffu);
    e->h = (at & BOOT_NVN_SHADOW) ? boot_ax_rec_hash(rec) : rec->x[1];
}
static inline int boot_ax_is_draw(unsigned op) {
    BootNvnOps *o = boot_nvn_ops(); const int x = (int)op;
    return x == o->draw_arrays || x == o->draw_arrays_inst || x == o->draw_elements || x == o->draw_elements_bv || x == o->draw_elements_inst ||
           x == o->draw_arrays_ind || x == o->draw_elements_ind;
}
enum { BOOT_AX_D_REC = 1, BOOT_AX_D_SAVED = 2, BOOT_AX_D_RACED = 4, BOOT_AX_D_GPU = 8 };
static inline void boot_ax_chk_draw(unsigned f) {
    ++boot_ax.c.chk_draws;
    if (!f) ++boot_ax.c.chk_draw_eq; else if (f & BOOT_AX_D_REC) ++boot_ax.c.chk_draw_rec; else if (f & BOOT_AX_D_RACED) ++boot_ax.c.chk_draw_raced;
    else if (f & BOOT_AX_D_SAVED) ++boot_ax.c.chk_draw_saved; else ++boot_ax.c.chk_draw_gpu;
}
static inline void boot_ax_chk_bad(const char *what, const BootAxChkEnt *e, uint64_t got) {
    static unsigned logged; if (logged >= 16) return;
    ++logged; const unsigned op = e->kind & 0xffffu;
    char t[320]; snprintf(t, sizeof t, " (async-xlate sentinel, %s): at %llx op %s size %u, X read %llx, re-read on the submitting thread %llx (submission %llu)", what,
                          (unsigned long long)e->at, op < BOOT_NVN_NAME_COUNT ? boot_nvn_names[op].name : "jump/end", e->n, (unsigned long long)e->h, (unsigned long long)got,
                          (unsigned long long)boot_render_exec_id);
    boot_draw_log("nvn native: X read differs from the old way%s %llu %llu", t, logged, 0);
}
/* t2, after a join (the call's pins and walk table back with this thread, X idle unless another thread queued an op meanwhile: the page
 * table is read under w->lock, which every X item that touches it holds). */
static inline void boot_ax_chk_replay(BootContext *c, uint64_t self, BootWindow *w) {
    static uint8_t scratch[2 * 65536];
    BootAxChkLog *L = &boot_ax_chk; BootAxCount *k = &boot_ax.c;
    ++k->chk_subs; k->chk_lost += L->lost;
    AcquireSRWLockExclusive(&w->lock);
    BootRender *r = w->render; unsigned open = 0, df = 0, pend = 0;
    for (uint32_t i = 0; i < L->n; ++i) {
        const BootAxChkEnt *e = &L->e[i]; const unsigned type = e->kind >> 24, src = (e->kind >> 16) & 0xffu, op = e->kind & 0xffffu;
        if (type == BOOT_AX_CHK_REC) {
            unsigned bad = 0;
            if (!(e->at & BOOT_NVN_SHADOW)) {   /* a driver handle: X's resolution against the call's own */
                const BootNvnFzEnt *f = boot_nvn_freeze_find(e->at); ++k->chk_handles;
                if (!f || !f->shadow) ++k->chk_unfrozen;   /* not resolved at the call: the live map decided, on X as it would on t2 */
                else if (f->shadow != e->h) { ++k->chk_handle_bad; bad = 1; boot_ax_chk_bad("handle", e, f->shadow); }
            } else {
                BootNvnRecord rr; const uint64_t size = boot_nvn_record_read(c, self, e->at, &rr, scratch, sizeof scratch); ++k->chk_recs;
                const uint64_t h = size ? boot_ax_rec_hash(&rr) : 0;
                if (size != e->n || h != e->h) { ++k->chk_rec_bad; bad = 1; boot_ax_chk_bad("record", e, h); }
            }
            if (boot_ax_is_draw(op)) { if (open) boot_ax_chk_draw(df); open = 1; df = pend | (bad ? (unsigned)BOOT_AX_D_REC : 0u); pend = 0; }
            else if (bad) pend |= BOOT_AX_D_REC;
        } else if (type == BOOT_AX_CHK_CB) {
            ++k->chk_cbs;
            const uint8_t *g = r ? boot_ds_guest(r, e->va, e->n, NULL) : NULL;
            if (!g) { ++k->chk_cb_unread; continue; }
            if (boot_ax_hash(g, e->n, BOOT_AX_H0) == e->h) { ++k->chk_cb_same; continue; }
            if (src == 1) { ++k->chk_cb_saved; df |= BOOT_AX_D_SAVED; }   /* the capture kept the submit-time bytes */
            else if (src == 2) { ++k->chk_cb_raced; df |= BOOT_AX_D_RACED; }   /* taken by data-snapshot, read live: a race the capture missed */
            else { ++k->chk_cb_gpu; df |= BOOT_AX_D_GPU; }   /* not taken or GPU-ordered: GPU writes land here by design */
        }
    }
    if (open) boot_ax_chk_draw(df);
    ReleaseSRWLockExclusive(&w->lock);
}

/* ---- virtual fences. fence_of[seq] is written by X before the item is published done; vdone moves under vlock while the D3D12 fence has
 * passed each next item's value (an item without a renderer, or recorded on a GPU object since replaced by a device recovery, counts as
 * complete: its work is gone with it). */
static inline int boot_ax_vcomplete(uint64_t v) {
    if (v <= (uint64_t)__atomic_load_n(&boot_ax.vdone, __ATOMIC_ACQUIRE)) return 1;
    AcquireSRWLockExclusive(&boot_ax.vlock);
    uint64_t d = (uint64_t)boot_ax.vdone; const uint64_t done = boot_ax_fifo_done_now(&boot_ax.f);
    const BootGpu *g = boot_ax.gpu; const int have = g && g->fence; uint64_t got = 0; int read = 0;
    while (d < done) {
        const BootAxFenceOf *s = &boot_ax.fence_of[(d + 1) % BOOT_AX_FENCES];
        if (have && s->gpu == g) { if (!read) { got = ID3D12Fence_GetCompletedValue(g->fence); read = 1; } if (got < s->fence) break; }
        ++d;
    }
    __atomic_store_n(&boot_ax.vdone, (LONG64)d, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&boot_ax.vlock);
    return v <= d;
}
/* The D3D12 value of item v (X has run it), 0 when it needs none (no renderer, another GPU object, or older than the ring and so complete). */
static inline uint64_t boot_ax_d3d12_of(uint64_t v) {
    const uint64_t done = boot_ax_fifo_done_now(&boot_ax.f);
    if (v > done || done - v >= BOOT_AX_FENCES - 1) return 0;
    const BootAxFenceOf *s = &boot_ax.fence_of[v % BOOT_AX_FENCES];
    return s->gpu && s->gpu == boot_ax.gpu ? s->fence : 0;
}
/* Waits (outside every lock) for the D3D12 completion of item v that X has run; 1 when complete. */
static inline int boot_ax_gpu_wait(uint64_t v, DWORD ms) {
    const uint64_t f = boot_ax_d3d12_of(v); const BootGpu *g = boot_ax.gpu;
    if (!f || !g || !g->fence || ID3D12Fence_GetCompletedValue(g->fence) >= f) return 1;
    static _Thread_local HANDLE ev; if (!ev) ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev || FAILED(ID3D12Fence_SetEventOnCompletion(g->fence, f, ev))) return 0;
    return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0 || ID3D12Fence_GetCompletedValue(g->fence) >= f;
}

/* ---- X */
/* X's context: a copy of the submitting core (fastmem table access needs a core context, BOOT_FM_TABLE; boot_mt_gpu_pack and the
 * GPU-resident refresh cast it to BootCore) with its own index, no host handles and not in guest code. Made on t2 while X is idle (stage
 * 2 drains first); again only if another core submits. The copied region table is the submitting core's snapshot: X's pack slot is
 * cleared so its first pack takes the locked path and refreshes the table from the master. */
static inline void boot_ax_core_from(const BootCore *from) {
    if (boot_ax.core_from == from) return;
    memcpy(&boot_ax.core, from, sizeof boot_ax.core);
    boot_ax.core.index = BOOT_AX_CORE_INDEX; boot_ax.core.in_guest = 0; boot_ax.core.interrupt = 0; boot_ax.core.idle = 0;
    boot_ax.core.doorbell = boot_ax.core.host = boot_ax.core.timer = NULL;
    boot_ax.core.ctx.status = 0; boot_ax.core.ctx.fault_pc = boot_ax.core.ctx.fault_address = boot_ax.core.ctx.fault_width = 0;
    boot_mt_pack_seen[BOOT_AX_CORE_INDEX] = 0;
    boot_ax.core_from = from;
}
static inline void boot_ax_run_sub(BootAxSub *S, uint32_t flags) {
    BootContext *xc = &boot_ax.core.ctx; BootNvnBackend *be = boot_nvn_backend();
    S->q_x0 = boot_nvn_qpc();
    xc->status = 0; xc->fault_pc = xc->fault_address = xc->fault_width = 0; xc->window_native = S->window;   /* per submission (PRECHECKS 3) */
    boot_nvn_act_tid = S->tid; boot_ax_on_x = 1;   /* the call's freeze, pins, call-bind and NVN_PAR checks see the submitting thread */
    if (S->own_ds) { boot_nvn_dsnap_tls = &S->ds; boot_ax_x_epoch1 = S->reg_epoch + 1; boot_ax_texel_cut1 = S->reg_epoch + 1; }   /* stage 2: its walk table, its registry view */
    boot_nvn_pins_swap(&S->pins);                  /* X's set is empty: it adopts the call's pins; any it adds join them */
    boot_ax_cap_view = S->cap_ok ? &S->cap : &boot_ax_cap_none; boot_ax_obj_cur = &S->obj; boot_ax_chk_log = S->check ? &boot_ax_chk : NULL;
    boot_ax_dyn_view = S->dyn_ok ? &S->dyn : NULL;
    if (S->own_ds && boot_ax_hist_on) boot_ax_hist_memo_reset(S->reg_epoch);   /* memos of entries registered after the capture: resolved again */
    if (flags & BOOT_AX_IT_XDIAG) boot_ds_call_begin();
    for (uint32_t k = 0; k < S->nh; ++k) {   /* today's loop, on X's context */
        InterlockedExchange(&S->k, (LONG)k);
        if (!be->submit(be->ctx, xc, S->self, S->queue, S->h[k])) { S->failed = 1; S->fail_k = k; break; }
        ++S->done;
    }
    if (flags & BOOT_AX_IT_XDIAG) { boot_sd_call_end(xc); boot_ds_call_end(xc); }   /* the end-of-call checks as the submitting thread ran them */
    if (S->own_ds && boot_ax_hist_on) boot_ax_hist_memo_reset(S->reg_epoch);   /* ... and nothing memoized from an older entry outlives it */
    boot_ax_chk_log = NULL; boot_ax_obj_cur = NULL; boot_ax_cap_view = NULL; boot_ax_dyn_view = NULL;
    S->pins_out = boot_nvn_pins.n;
    if (!(flags & BOOT_AX_IT_JOIN)) boot_nvn_unpin_all();   /* stage 2: the call returned long ago; its recordings are free once translated */
    boot_nvn_pins_swap(&S->pins);                  /* back to S; X's set is empty again */
    if (S->own_ds) { boot_nvn_dsnap_tls = NULL; boot_nvn_dsnap_fold(&S->ds); if (!(flags & BOOT_AX_IT_JOIN)) S->ds.active = 0; }
    boot_ax_on_x = 0; boot_nvn_act_tid = 0; boot_ax_x_epoch1 = 0; boot_ax_texel_cut1 = 0;
    S->status = xc->status; S->fault_pc = xc->fault_pc; S->fault_address = xc->fault_address; S->fault_width = xc->fault_width;
    xc->status = 0; xc->fault_pc = xc->fault_address = xc->fault_width = 0;
    if ((uint64_t)__atomic_load_n(&boot_ax_reg_epoch, __ATOMIC_ACQUIRE) != S->reg_epoch) ++boot_ax.c.reg_subs;
    S->q_x1 = boot_nvn_qpc();
}
/* X after a submission: what the next captures need (under the FIFO lock): the uniform-buffer bindings the queue state holds now (a later
 * submission inherits them) and the call-start chunks. */
static inline void boot_ax_publish(uint64_t seq) {
    BootNvnExec *e = boot_nvn_exec(); uint64_t cb[96][2]; uint32_t n = 0;
    static uint64_t ch[BOOT_AX_CHUNKS];
    const uint32_t nch = boot_ax_chunk_list(ch, BOOT_AX_CHUNKS);
    if (e) for (unsigned s = 0; s < 6; ++s) for (unsigned i = 0; i < 16; ++i) if (e->ubo[s][i] && e->ubo_size[s][i]) { cb[n][0] = e->ubo[s][i]; cb[n][1] = e->ubo_size[s][i]; ++n; }
    AcquireSRWLockExclusive(&boot_ax.f.lock);
    memcpy(boot_ax.carry, cb, sizeof cb[0] * n); boot_ax.ncarry = n; boot_ax.carry_seq = seq;
    memcpy(boot_ax.chunks, ch, sizeof ch[0] * nch); boot_ax.nchunks = nch;
    ReleaseSRWLockExclusive(&boot_ax.f.lock);
}
/* X: a stage-2 submission's translation failed after its call returned: the next NVN call stops the game with the reason (DESIGN 5.7.8). */
static inline void boot_ax_sticky_set(const BootAxSub *S) {
    if (InterlockedCompareExchange(&boot_ax.sticky_on, 1, 0)) return;
    boot_ax.sticky_handle = S->failed ? S->h[S->fail_k] : 0; boot_ax.sticky_status = S->status;
    boot_ax.sticky_pc = S->fault_pc; boot_ax.sticky_address = S->fault_address; boot_ax.sticky_width = S->fault_width;
    char t[200]; snprintf(t, sizeof t, " (async-xlate): handle %llx (%u of %u translated), guest read status %u at %llx: the next NVN call stops the game", (unsigned long long)boot_ax.sticky_handle,
                          S->done, S->nh, S->status, (unsigned long long)S->fault_address);
    boot_draw_log("nvn native: a submission failed on the translation thread X after its call returned%s %llu %llu", t, S->seq, 0);
}
static DWORD WINAPI boot_ax_main(void *arg) {
    (void)arg;
    boot_mt_cpuset_apply(1);   /* the P-core set, as the guest workers (TOTK_MT_CPUSETS) */
    { BootNvnRlStats *m = boot_nvn_rl(); m->sl_role = BOOT_NVN_SL_ROLE_RENDER; }   /* storelock-work counters: X waits as the render thread */
    for (;;) {
        const BootAxItem it = boot_ax_fifo_next(&boot_ax.f);
        if (it.seq != boot_ax.x_last + 1 && ++boot_ax.c.order_alarms <= 16)
            boot_draw_log("nvn native: async-xlate ORDER ALARM%s: item %llu after %llu", "", it.seq, boot_ax.x_last);
        boot_ax.x_last = it.seq;
        InterlockedExchange64(&boot_ax.cur_seq, (LONG64)it.seq); InterlockedExchange(&boot_ax.cur_op, (LONG)it.op); InterlockedExchange64(&boot_ax_x_item, (LONG64)it.seq);
        InterlockedExchange(&boot_ax.busy, 1);
        const int held = (it.flags & BOOT_AX_IT_DELEGATED) ? 0 : boot_render_lock();   /* stage 2: X's own renderer-lock section, one item long */
        BootAxSub *S = (BootAxSub *)it.sub; BootNvnExec *e = boot_nvn_exec();
        switch (it.op) {
            case BOOT_AX_OP_SUBMIT: boot_ax_run_sub(S, it.flags); ++boot_ax.c.translated; break;
            case BOOT_AX_OP_FENCE: (void)boot_nvn_exec_signal(NULL, 0); break;   /* the list the submit left open executes; its value covers everything before */
            case BOOT_AX_OP_PRESENT: if (it.npay == sizeof(BootAxPresentPay)) { BootAxPresentPay p; memcpy(&p, it.pay, sizeof p); boot_nvn_exec_present_x(&p); } break;
            case BOOT_AX_OP_IDLE: case BOOT_AX_OP_FINISH: (void)boot_nvn_exec_idle_x(); break;
            case BOOT_AX_OP_DEPTH_MODE: if (e) e->depth_mode = it.arg; break;
            case BOOT_AX_OP_ORIGIN_MODE: if (e) e->origin_upper_left = it.arg; break;
            default: break;   /* DRAIN: a marker */
        }
        {   BootWindow *w = e ? e->w : NULL; const BootGpu *g = w && w->render ? w->render->gpu : NULL;   /* the D3D12 value after this item */
            BootAxFenceOf *s = &boot_ax.fence_of[it.seq % BOOT_AX_FENCES]; s->fence = g ? g->submitted : 0; s->gpu = g; boot_ax.gpu = g; }
        if (it.op == BOOT_AX_OP_SUBMIT) boot_ax_publish(it.seq);
        boot_render_unlock(held);
        if (it.op < BOOT_AX_OPS) ++boot_ax.c.ops_done[it.op];
        InterlockedExchange(&boot_ax.busy, 0); InterlockedExchange64(&boot_ax_x_item, 0);
        if (it.op == BOOT_AX_OP_SUBMIT && !(it.flags & BOOT_AX_IT_JOIN)) {   /* stage 2: S goes back to t2's pool here (a join's waiter takes it) */
            if (S->failed || S->status) { ++boot_ax.c.sticky; boot_ax_sticky_set(S); }
            boot_ax.c.q_x_async += S->q_x1 - S->q_x0;
            AcquireSRWLockExclusive(&boot_ax.f.lock); S->in_use = 0; if (boot_ax.inflight) --boot_ax.inflight; ReleaseSRWLockExclusive(&boot_ax.f.lock);
        }
        boot_ax_fifo_done(&boot_ax.f, it.seq);
        (void)boot_ax_vcomplete(it.seq);   /* keeps vdone close behind (fence_of is a ring) */
    }
    return 0;
}
/* X is made by the first item (a submit on t2, or an op): 8 MiB of stack as the guest workers (the translation ran on t2), the caller's
 * priority (t2's), the name "nvn-xlate" for profilers. Once (INIT_ONCE): t0 and t2 may queue at the same time. */
static BOOL CALLBACK boot_ax_start_once(PINIT_ONCE once, PVOID p, PVOID *ctx) {
    (void)once; (void)p; (void)ctx;
    boot_ax.started = 1;
    boot_ax.thread = CreateThread(NULL, 8u << 20, boot_ax_main, NULL, 0, &boot_ax.tid);
    if (boot_ax.thread) {
        SetThreadPriority(boot_ax.thread, GetThreadPriority(GetCurrentThread()));
        typedef HRESULT (WINAPI *BootAxDesc)(HANDLE, PCWSTR); BootAxDesc d = NULL; HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC p = k32 ? GetProcAddress(k32, "SetThreadDescription") : NULL; if (p) memcpy(&d, &p, sizeof d);
        if (d) d(boot_ax.thread, L"nvn-xlate");
    }
    char t[300]; snprintf(t, sizeof t, " (TOTK_ASYNC_XLATE %d, live %ld%s%s, depth %u, sentinel %s every %u submissions, constant-buffer capture %s, arena %llu MiB per submission)",
                          boot_nvn_ax_mode(), (long)boot_ax.live, boot_ax.live_why ? ": " : "", boot_ax.live_why ? boot_ax.live_why : "", boot_ax_depth(),
                          boot_ax_check_on() ? "on" : "off", boot_ax_check_every(), boot_ax_cb_on() ? "on" : "off (TOTK_ASYNC_XLATE_CB=0)", (unsigned long long)(boot_ax_capture_bytes() >> 20));
    boot_draw_log("nvn native: async-xlate: translation thread X%s host thread %llu, started %llu", t, boot_ax.tid, boot_ax.thread != NULL);
    return TRUE;
}
static inline int boot_ax_start(void) { InitOnceExecuteOnce(&boot_ax.start_once, boot_ax_start_once, NULL, NULL); return boot_ax.thread != NULL; }
typedef struct BootAxWd { unsigned kind; uint64_t seq; } BootAxWd;
static void boot_ax_watchdog(void *arg, uint64_t ms) {
    const BootAxWd *d = (const BootAxWd *)arg; ++boot_ax.c.watchdog;
    char t[300]; snprintf(t, sizeof t, " (async-xlate watchdog): a %s has waited %llu ms for item %llu; X busy %ld on item %lld (op %ld), done %llu, queued up to %llu, submission %llu",
                          d->kind < BOOT_AX_WS ? boot_ax_w_name[d->kind] : "wait", (unsigned long long)ms, (unsigned long long)d->seq, (long)boot_ax.busy, (long long)boot_ax.cur_seq,
                          (long)boot_ax.cur_op, (unsigned long long)boot_ax_fifo_done_now(&boot_ax.f), (unsigned long long)boot_ax.f.seq, (unsigned long long)boot_render_exec_id);
    boot_draw_log("nvn native: translation thread X is late%s %llu %llu", t, boot_ax.c.watchdog, 0);
}
/* Waits until item seq is done (deadline: QPC, 0 = never gives up). Every renderer-lock level this thread holds is released and it leaves
 * guest code first (X takes the renderer lock per item; X's pack quiesces every core), then both come back. 1: done. */
static inline int boot_ax_wait_done(BootContext *c, uint64_t seq, uint64_t deadline, unsigned kind) {
    if (boot_ax_fifo_done_now(&boot_ax.f) >= seq) return 1;
    const uint64_t q0 = boot_nvn_qpc(); BootAxWd wd = {kind, seq};
    const LONG depth = boot_ov_cs_release(); BootCore *k = boot_ov_leave_guest(c);
    const int ok = boot_ax_fifo_wait_until(&boot_ax.f, seq, deadline, 2000, boot_ax_watchdog, &wd, NULL);
    boot_ov_cs_reacquire(depth); boot_ov_enter_guest(k);
    const uint64_t dq = boot_nvn_qpc() - q0;
    if (kind < BOOT_AX_WS) { ++boot_ax.c.w_n[kind]; boot_ax.c.w_qpc[kind] += dq; if (dq > boot_ax.c.w_max[kind]) boot_ax.c.w_max[kind] = dq; }
    return ok;
}
/* Every item queued so far done (X idle unless another thread queues meanwhile). */
static inline void boot_ax_drain_items(BootContext *c, unsigned kind) {
    const uint64_t s = __atomic_load_n(&boot_ax.f.seq, __ATOMIC_ACQUIRE);
    if (s && boot_ax_fifo_done_now(&boot_ax.f) < s) boot_ax_wait_done(c, s, 0, kind);
}
/* ... and their GPU work complete (DESIGN 5.5: pool finalize): nothing translated or executing reads the memory after it. */
static inline void boot_ax_drain(BootContext *c, unsigned kind) {
    const uint64_t s = __atomic_load_n(&boot_ax.f.seq, __ATOMIC_ACQUIRE);
    if (!s || boot_ax_vcomplete(s)) return;
    boot_ax_drain_items(c, kind);
    const LONG depth = boot_ov_cs_release(); BootCore *k = boot_ov_leave_guest(c);
    while (!boot_ax_gpu_wait(s, 2000) && !boot_ax_vcomplete(s)) { BootAxWd wd = {BOOT_AX_W_GPU, s}; boot_ax_watchdog(&wd, 2000); }
    boot_ov_cs_reacquire(depth); boot_ov_enter_guest(k);
    (void)boot_ax_vcomplete(s);
}
/* Queues an item, waiting (outside the renderer lock and guest code) while the ring is full. */
static inline uint64_t boot_ax_push_wait(BootContext *c, uint32_t op, uint32_t arg, uint32_t flags, void *sub, const void *pay, uint32_t npay) {
    (void)boot_ax_start();   /* an op may come before the first submit (a FenceSync at start-up) */
    for (;;) {
        const uint64_t s = boot_ax_fifo_push_ex(&boot_ax.f, op, arg, flags, sub, pay, npay, 0);
        if (s) { if (op < BOOT_AX_OPS) ++boot_ax.c.ops[op]; return s; }
        AcquireSRWLockShared(&boot_ax.f.lock); const uint64_t head = boot_ax.f.seq - (boot_ax.f.tail - boot_ax.f.head) + 1; ReleaseSRWLockShared(&boot_ax.f.lock);
        boot_ax_wait_done(c, head, 0, BOOT_AX_W_RING);
    }
}

/* ---- the live mode. TOTK_ASYNC_XLATE at boot, narrowed once: 2 needs the frame-in-flight guard, the early fence, frame overlap, the
 * re-record wait and NVN_PAR off (each reads state stage 2 moved off the call: the frame fence values, the call's walk table on helper
 * threads, the call staying open until translated). frames\async-xlate.request ("2", "1" or "0"; checked every 64 submissions) narrows
 * it live: X drains, then the next call runs the new mode. */
static BOOL CALLBACK boot_ax_live_init(PINIT_ONCE once, PVOID p, PVOID *ctx) {
    (void)once; (void)p; (void)ctx;
    int m = boot_nvn_ax_mode();
    if (m > 2) { m = 2; boot_ax.live_why = "stage 3 (early fence) is not built: running stage 2"; }
    if (m == 2) {
        char fg[8]; const DWORD fgn = GetEnvironmentVariableA("TOTK_FRAME_GUARD", fg, sizeof fg);   /* boot_ff_on's switch (it also needs host threads, maybe not up yet) */
        const char *why = boot_ff_on() || (fgn && fgn < sizeof fg && fg[0] != '0') ? "the frame-in-flight guard is on (TOTK_FRAME_GUARD / TOTK_OVERLAP)" : boot_ef_on() ? "the early fence is on (TOTK_EARLY_FENCE)" :
                          boot_mt_overlap_on() ? "frame overlap is on (TOTK_OVERLAP)" : boot_nvn_rerecord_wait_on() ? "the re-record wait is on (TOTK_RERECORD_WAIT)" :
                          boot_nvn_par_on() ? "NVN_PAR is on (helpers read the call's walk table)" : NULL;
        if (why) { m = 1; boot_ax.live_why = why; }
    }
    boot_ax.live = m;
    return TRUE;
}
static inline int boot_ax_request(BootContext *c, int m) {
    if (GetFileAttributesA("frames\\async-xlate.request") == INVALID_FILE_ATTRIBUTES) return m;
    FILE *f = fopen("frames\\async-xlate.request", "rb"); const int ch = f ? fgetc(f) : EOF; if (f) fclose(f);
    const int want = ch == '0' ? 0 : ch == '1' ? 1 : ch == '2' || ch == '3' ? 2 : m;
    if (want >= m) return m;
    if (m >= 2) boot_ax_drain_items(c, BOOT_AX_W_MODE);   /* everything captured is translated before a narrower mode runs */
    __atomic_store_n(&boot_ax.live, want, __ATOMIC_RELEASE); ++boot_ax.c.steps;
    char t[160]; snprintf(t, sizeof t, ": frames\\async-xlate.request is %c: mode %d -> %d from this call on (async %llu, join %llu so far)", ch, m, want,
                          (unsigned long long)boot_ax.c.async, (unsigned long long)boot_ax.c.join);
    boot_draw_log("nvn native: async-xlate%s %llu %llu", t, 0, 0);
    return want;
}
static inline int boot_ax_live_mode(BootContext *c, int tick) {
    InitOnceExecuteOnce(&boot_ax.live_once, boot_ax_live_init, NULL, NULL);
    int m = __atomic_load_n(&boot_ax.live, __ATOMIC_ACQUIRE);
    if (tick && m && !(++boot_ax.req_tick & 63)) m = boot_ax_request(c, m);
    return m;
}
/* TOTK_ASYNC_XLATE as it applies now (stage 1 entry: the request file narrows 1 to 0). */
static inline int boot_ax_mode_now(BootContext *c) { return boot_nvn_ax_mode() ? boot_ax_live_mode(c, 1) : 0; }

/* ---- the registration wrapper (stage 2, TOTK_ASYNC_XLATE >= 2 at boot): every own NVN function registers through it without the host
 * function's renderer lock; the wrapper takes the lock itself except for the functions with a lock-free stage-2 path while the live mode is
 * 2. A lock the function takes later (boot_ax_held: today's loop, an untagged wait) is released here. A sticky async failure stops the
 * game at the next call. */
typedef struct BootAxOwn { BootHostFunction fn; intptr_t user; int lf, submit; } BootAxOwn;
#define BOOT_AX_OWN_MAX 64
static BootAxOwn boot_ax_own_tab[BOOT_AX_OWN_MAX]; static unsigned boot_ax_own_n;
BOOT_NVN_FN(boot_ax_own) {
    const BootAxOwn *o = &boot_ax_own_tab[(intptr_t)user];
    if (__atomic_load_n(&boot_ax.sticky_on, __ATOMIC_ACQUIRE)) {
        if (boot_ax.sticky_status && !c->status) { c->status = boot_ax.sticky_status; c->fault_pc = boot_ax.sticky_pc; c->fault_address = boot_ax.sticky_address; c->fault_width = boot_ax.sticky_width; }
        return boot_nvn_fail(c, self, "submit (translated after the call returned, async-xlate)", boot_ax.sticky_handle);
    }
    const int lf = o->lf && boot_mt.active && boot_ax_live_mode(c, o->submit) >= 2;   /* the renderer lock is what excludes X: none without host threads */
    const int held = lf ? 0 : boot_render_lock();
    const int was_lf = boot_ax_lf, was_held = boot_ax_held; boot_ax_lf = lf; boot_ax_held = 0;
    const int rc = o->fn(c, self, target, (void *)o->user);
    if (boot_ax_held) boot_render_unlock(1);
    boot_ax_lf = was_lf; boot_ax_held = was_held;
    boot_render_unlock(held);
    return rc;
}
/* Registration (host_nvn.h boot_nvn_native_register): *fn / *user become the wrapper's. 0: the table is full (registered as before). */
static inline int boot_ax_own_add(const char *name, BootHostFunction *fn, intptr_t *user) {
    static const char *const lf_names[] = {"nvnQueueSubmitCommands", "nvnQueueFinish", "nvnQueueFlush", "nvnQueueFenceSync", "nvnQueueWaitSync", "nvnQueuePresentTexture",
        "nvnSyncInitialize", "nvnSyncFinalize", "nvnSyncWait", "nvnWindowSetCrop", "nvnWindowSetPresentInterval", "nvnWindowSetNumActiveTextures",
        "nvnWindowGetPresentInterval", "nvnWindowGetNumActiveTextures", "nvnWindowAcquireTexture"};
    if (boot_ax_own_n >= BOOT_AX_OWN_MAX) return 0;
    BootAxOwn *o = &boot_ax_own_tab[boot_ax_own_n]; o->fn = *fn; o->user = *user; o->lf = 0; o->submit = !strcmp(name, "nvnQueueSubmitCommands");
    for (size_t i = 0; i < sizeof lf_names / sizeof *lf_names; ++i) o->lf |= !strcmp(name, lf_names[i]);
    *fn = boot_ax_own; *user = (intptr_t)boot_ax_own_n++;
    return 1;
}
/* A lock-free call that needs today's renderer path after all: the renderer lock (the wrapper releases it). */
static inline void boot_ax_lock_now(void) { if (boot_ax_lf && !boot_ax_held) boot_ax_held = boot_render_lock(); }

/* ---- t2: the capture */
/* Write ranges (GPU-ordered writes, storage buffers) of the submissions whose work is not yet idle, as merged sorted intervals for the
 * capture's GPU-order test (boot_ax_wr_hit). An idle (boot_render_idle_cleanup -> boot_ax_idle_mark) means every item before the one X
 * runs has completed on the GPU with its deferred compute stores applied; until then the bytes the capture would copy may still change in
 * GPU order. *all: the ring overflowed for one of them (every range then counts as GPU-ordered). */
static inline int boot_ax_wr_cmp(const void *a, const void *b) { const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b; return x[0] < y[0] ? -1 : x[0] > y[0]; }
static inline void boot_ax_wr_build(int *all) {
    const uint64_t idle = (uint64_t)__atomic_load_n(&boot_ax_idle_done, __ATOMIC_ACQUIRE);
    while (boot_ax.wr_head < boot_ax.wr_tail && boot_ax.wr[boot_ax.wr_head % BOOT_AX_WR].seq <= idle) ++boot_ax.wr_head;
    *all = boot_ax.wr_over_seq > idle;
    const uint64_t n = boot_ax.wr_tail - boot_ax.wr_head;
    if (n > boot_ax.wr_iv_cap) { uint64_t (*v)[2] = (uint64_t (*)[2])realloc(boot_ax.wr_iv, (size_t)n * sizeof *v); if (!v) { *all = 1; boot_ax.nwr_iv = 0; return; } boot_ax.wr_iv = v; boot_ax.wr_iv_cap = (uint32_t)n; }
    uint32_t k = 0;
    for (uint64_t i = boot_ax.wr_head; i < boot_ax.wr_tail; ++i) { const BootAxWr *w = &boot_ax.wr[i % BOOT_AX_WR]; boot_ax.wr_iv[k][0] = w->va; boot_ax.wr_iv[k][1] = w->end; ++k; }
    qsort(boot_ax.wr_iv, k, sizeof *boot_ax.wr_iv, boot_ax_wr_cmp);
    uint32_t m = 0;
    for (uint32_t i = 0; i < k; ++i) {
        if (m && boot_ax.wr_iv[i][0] <= boot_ax.wr_iv[m - 1][1]) { if (boot_ax.wr_iv[i][1] > boot_ax.wr_iv[m - 1][1]) boot_ax.wr_iv[m - 1][1] = boot_ax.wr_iv[i][1]; }
        else { boot_ax.wr_iv[m][0] = boot_ax.wr_iv[i][0]; boot_ax.wr_iv[m][1] = boot_ax.wr_iv[i][1]; ++m; }
    }
    boot_ax.nwr_iv = m;
}
static inline int boot_ax_wr_hit(uint64_t va, uint64_t n) {
    uint32_t lo = 0, hi = boot_ax.nwr_iv;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (boot_ax.wr_iv[mid][1] <= va) lo = mid + 1; else hi = mid; }
    return lo < boot_ax.nwr_iv && boot_ax.wr_iv[lo][0] < va + n;
}
/* After a submission is queued: its write ranges (and storage buffers, which a storing draw or a dispatch may write) join the set. */
static inline void boot_ax_wr_add(uint64_t seq) {
    const uint64_t idle = (uint64_t)__atomic_load_n(&boot_ax_idle_done, __ATOMIC_ACQUIRE);
    while (boot_ax.wr_head < boot_ax.wr_tail && boot_ax.wr[boot_ax.wr_head % BOOT_AX_WR].seq <= idle) ++boot_ax.wr_head;
    for (uint32_t i = 0; i < boot_nvn_axr.n; ++i) {
        const BootNvnAxr *e = &boot_nvn_axr.e[i]; const unsigned kind = (unsigned)(e->bound >> 56); const uint64_t size = e->bound & UINT64_C(0xffffffffff);
        if (!(kind == BOOT_NVN_AXR_W_UBO || kind == BOOT_NVN_AXR_W_CLEAR || kind == BOOT_NVN_AXR_W_REPORT || kind == BOOT_NVN_AXR_SSBO) || !e->va || !size || e->va > UINT64_MAX - size) continue;
        if (boot_ax.wr_tail - boot_ax.wr_head >= BOOT_AX_WR) { boot_ax.wr_over_seq = seq; ++boot_ax.c.wr_over; continue; }
        BootAxWr *w = &boot_ax.wr[boot_ax.wr_tail++ % BOOT_AX_WR]; w->seq = seq; w->va = e->va; w->end = e->va + size;
    }
}
static inline int boot_ax_range_add(uint64_t va, uint64_t size) {
    if (boot_ax.nranges == boot_ax.ranges_cap) {
        const uint32_t cap = boot_ax.ranges_cap ? boot_ax.ranges_cap * 2 : 4096; uint64_t *r = cap < (1u << 24) ? (uint64_t *)realloc(boot_ax.ranges, (size_t)cap * 16) : NULL;
        if (!r) return 0;
        boot_ax.ranges = r; boot_ax.ranges_cap = cap;
    }
    boot_ax.ranges[2 * boot_ax.nranges] = va; boot_ax.ranges[2 * boot_ax.nranges + 1] = size; ++boot_ax.nranges;
    return 1;
}
/* The constant-buffer capture of S (t2 at the submit call, after the walk; no renderer state is read). The ranges: S's uniform-buffer
 * summaries, the bindings S inherits (X's published queue state after the newest translated submission, and the summaries of every
 * submission captured after it that X has not finished: a superset of the queue state S starts with), each at the size
 * boot_render_cbv_ex copies for it, deduplicated by start address (a longer copy replaces a shorter one). A range a write of a submission
 * not yet idle may overlap is flagged GPU-ordered and not copied (X reads it when it records the draw: GPU order, as before). */
static inline void boot_ax_capture_cb(BootContext *c, uint64_t self, BootWindow *w, BootAxSub *S) {
    BootAxCap *cap = &S->cap; uint64_t *st = boot_ax.c.caps;
    S->ncbr = 0; S->cap_ok = 0;
    cap->map_gen = (uint64_t)__atomic_load_n(&boot_render_map_gen, __ATOMIC_ACQUIRE); cap->big = (uint64_t)__atomic_load_n(&boot_render_big_writes, __ATOMIC_ACQUIRE);
    cap->epoch = (uint64_t)__atomic_load_n(&boot_render_cb_epoch, __ATOMIC_ACQUIRE);   /* X's counters, read once: a later move makes X read live (stale) */
    boot_ax.nranges = 0;
    for (uint32_t i = 0; i < boot_nvn_axr.n; ++i) {
        const BootNvnAxr *e = &boot_nvn_axr.e[i];
        if ((unsigned)(e->bound >> 56) != BOOT_NVN_AXR_CB) continue;
        const uint64_t size = e->bound & UINT64_C(0xffffffffff);
        if (S->ncbr == S->cbr_cap) { const uint32_t nc = S->cbr_cap ? S->cbr_cap * 2 : 1024; uint64_t *r = (uint64_t *)realloc(S->cbr, (size_t)nc * 16); if (!r) break; S->cbr = r; S->cbr_cap = nc; }
        S->cbr[2 * S->ncbr] = e->va; S->cbr[2 * S->ncbr + 1] = size; ++S->ncbr;
        boot_ax_range_add(e->va, size);
    }
    if (!boot_ds_on() || !boot_ds_cfg.cb || !boot_ax_cb_on()) return;   /* data-snapshot takes no constant buffer: nothing to capture */
    {   const uint32_t own = boot_ax.nranges;   /* inherited bindings */
        AcquireSRWLockShared(&boot_ax.f.lock);
        const uint64_t cs = boot_ax.carry_seq;
        for (uint32_t i = 0; i < boot_ax.ncarry; ++i) boot_ax_range_add(boot_ax.carry[i][0], boot_ax.carry[i][1]);
        st[BOOT_AX_CAPS_CARRY] += boot_ax.nranges - own;
        const uint32_t carried = boot_ax.nranges;
        for (unsigned j = 0; j < BOOT_AX_SUBS; ++j) {
            const BootAxSub *T = &boot_ax.sub[j];
            if (T != S && T->in_use && T->seq > cs) for (uint32_t i = 0; i < T->ncbr; ++i) boot_ax_range_add(T->cbr[2 * i], T->cbr[2 * i + 1]);
        }
        ReleaseSRWLockShared(&boot_ax.f.lock);
        st[BOOT_AX_CAPS_INFLIGHT] += boot_ax.nranges - carried;
    }
    int rebuilt = 0; const uint64_t m0 = boot_nvn_qpc();
    if (!boot_ax_map_sync(&boot_ax.map, w, c, &rebuilt)) return;
    if (rebuilt) { ++boot_ax.c.map_rebuilds; boot_ax.c.map_qpc += boot_nvn_qpc() - m0; }
    int all = 0; boot_ax_wr_build(&all);
    if (!boot_ax_cap_begin(cap, boot_ax.nranges, boot_ax_capture_bytes())) return;
    for (uint32_t i = 0; i < boot_ax.nranges; ++i) {
        const uint64_t va = boot_ax.ranges[2 * i], n = boot_render_cb_view_bytes(boot_ax.ranges[2 * i + 1]);
        if (!va || !n || va > UINT64_MAX - n) continue;
        ++st[BOOT_AX_CAPS_RANGES];
        const uint32_t j = boot_ax_cap_at(cap, va);
        if (j != UINT32_MAX && ((cap->flag[j] & BOOT_AX_CAPF_GPU) || cap->len[j] >= n)) { ++st[BOOT_AX_CAPS_DUPS]; continue; }
        if (all || boot_ax_wr_hit(va, n)) { (void)boot_ax_cap_put(cap, va, 0, BOOT_AX_CAPF_GPU, NULL); ++st[BOOT_AX_CAPS_GPU]; continue; }
        uint32_t slot = 0; uint8_t *dst = boot_ax_cap_put(cap, va, n, 0, &slot);
        if (!dst) { ++st[BOOT_AX_CAPS_OVER]; continue; }
        int zero = 0;
        if (!boot_ax_read_gpu(c, self, &boot_ax.map, va, dst, n, &zero)) { cap->len[slot] = 0; cap->flag[slot] = BOOT_AX_CAPF_UNMAPPED; ++st[BOOT_AX_CAPS_UNMAPPED]; continue; }
        if (zero) { cap->flag[slot] |= BOOT_AX_CAPF_ZERO; ++st[BOOT_AX_CAPS_ZERO]; }
    }
    S->cap_ok = 1;
}
static inline int boot_ax_dyn_order(const void *a, const void *b) { const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b; return x[0] < y[0] ? -1 : x[0] > y[0]; }
/* Stage 2: the call-start copy of S (DESIGN 4.5a): the chunks X published (its boot_ds_capture choice) copied page by page into S's arena;
 * X's boot_ds_capture fills the shadow from there (boot_ax_dyn_page), so the call-start bytes are the submit call's, not the translation's. */
static inline void boot_ax_capture_dyn(BootContext *c, uint64_t self, BootAxSub *S) {
    static uint64_t list[BOOT_AX_CHUNKS], pairs[BOOT_AX_CHUNKS][2]; BootAxDyn *d = &S->dyn;
    S->dyn_ok = 1; d->n = 0; d->arena = S->cap.arena;   /* always a view in stage 2: X's call-start copy takes only what this capture holds */
    AcquireSRWLockShared(&boot_ax.f.lock); const uint32_t n = boot_ax.nchunks; memcpy(list, boot_ax.chunks, sizeof list[0] * n); ReleaseSRWLockShared(&boot_ax.f.lock);
    if (!n || !S->cap_ok || !boot_ax.map.e) return;
    if (n > d->cap) {
        uint64_t *ck = (uint64_t *)realloc(d->chunk, (size_t)n * 8); if (ck) d->chunk = ck;
        uint32_t *of = ck ? (uint32_t *)realloc(d->off, (size_t)n * 4) : NULL; if (of) d->off = of;
        uint32_t *mk = of ? (uint32_t *)realloc(d->mask, (size_t)n * 4) : NULL; if (mk) d->mask = mk;
        if (!mk) return;
        d->cap = n;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *dst = boot_ax_arena_take(&S->cap, UINT64_C(1) << 16);
        if (!dst) break;
        uint32_t mask = 0;
        for (unsigned p = 0; p < 16; ++p) {
            const uint64_t va = (list[i] << 16) + (uint64_t)p * 4096; const BootAxMapEnt *e = boot_ax_map_find(&boot_ax.map, va);
            if (e && 4096 <= e->end - va && boot_nvn_peek(c, self, e->cpu + (va - e->gpu), dst + (size_t)p * 4096, 4096) == 4096) mask |= 1u << p; else ++boot_ax.c.dyn_pages_missed;
        }
        if (!mask) continue;
        pairs[k][0] = list[i]; pairs[k][1] = (uint64_t)(dst - S->cap.arena) | (uint64_t)mask << 32; ++k;
        ++boot_ax.c.dyn_chunks; boot_ax.c.dyn_bytes += UINT64_C(1) << 16;
    }
    qsort(pairs, k, sizeof pairs[0], boot_ax_dyn_order);
    for (uint32_t i = 0; i < k; ++i) { d->chunk[i] = pairs[i][0]; d->off[i] = (uint32_t)pairs[i][1]; d->mask[i] = (uint32_t)(pairs[i][1] >> 32); }
    d->n = k;
}
/* The object words (pool GPU addresses) the summaries name: CPU memory, read through this thread's context as the translation reads them. */
static inline void boot_ax_capture_obj(BootContext *c, uint64_t self, BootAxSub *S) {
    uint32_t nobj = 0;
    for (uint32_t i = 0; i < boot_nvn_axr.n; ++i) {
        const unsigned kind = (unsigned)(boot_nvn_axr.e[i].bound >> 56);
        if (kind < BOOT_NVN_AXR_N) ++boot_ax.c.axr[kind];
        nobj += kind == BOOT_NVN_AXR_OBJ;
    }
    if (boot_ax_obj_begin(&S->obj, nobj))
        for (uint32_t i = 0; i < boot_nvn_axr.n; ++i) {
            const BootNvnAxr *e = &boot_nvn_axr.e[i]; uint64_t v = 0;
            if ((unsigned)(e->bound >> 56) != BOOT_NVN_AXR_OBJ || boot_ax_obj_find(&S->obj, e->va, &v)) continue;
            if (boot_nvn_peek(c, self, e->va, &v, 8) == 8 && boot_ax_obj_put(&S->obj, e->va, v)) ++boot_ax.c.obj_words; else ++boot_ax.c.obj_unread;
        }
}
/* A free submission object (stage 2 async: also a free queue slot), waiting for X when there is none. NULL: none could be had. */
static inline BootAxSub *boot_ax_sub_take(BootContext *c, int async) {
    for (unsigned tries = 0;; ++tries) {
        BootAxSub *S = NULL;
        AcquireSRWLockExclusive(&boot_ax.f.lock);
        if (!async || boot_ax.inflight < boot_ax_depth())
            for (unsigned j = 0; j < BOOT_AX_SUBS && !S; ++j) if (!boot_ax.sub[j].in_use) S = &boot_ax.sub[j];
        if (S) { S->in_use = 1; if (async) ++boot_ax.inflight; }
        const uint64_t head = boot_ax.f.head < boot_ax.f.tail ? boot_ax.f.seq - (boot_ax.f.tail - boot_ax.f.head) + 1 : 0;
        ReleaseSRWLockExclusive(&boot_ax.f.lock);
        if (S) return S;
        if (!head || tries > 64) return NULL;   /* nothing queued to wait for: the pool is leaked (counted as out of memory) */
        boot_ax_wait_done(c, head, 0, BOOT_AX_W_ROOM);
    }
}
static inline void boot_ax_sub_give(BootAxSub *S) { AcquireSRWLockExclusive(&boot_ax.f.lock); S->in_use = 0; ReleaseSRWLockExclusive(&boot_ax.f.lock); }

/* ---- t2: the submit (boot_nvn_queue_submit, after the walk). Outcomes: BOOT_AX_SYNC: the caller runs today's loop (counted by reason; in a
 * lock-free call X was drained and the renderer lock taken first); BOOT_AX_DONE: X translated it and the call goes on as after the loop;
 * BOOT_AX_FAIL (*bad = the handle) / BOOT_AX_READ_FAIL: as the loop's failures; BOOT_AX_QUEUED (stage 2): captured and queued, the call
 * returns now. *xdiag: X ran the call's data-snapshot / suspect-diag begin and end (the caller skips them). */
static inline int boot_ax_sync(BootContext *c, unsigned why) {
    ++boot_ax.c.sync[why];
    if (boot_ax_lf) { boot_ax_drain_items(c, BOOT_AX_W_FALLBACK); boot_ax_lock_now(); boot_ds_call_begin(); }   /* today's loop runs here: X idle, the lock held */
    return BOOT_AX_SYNC;
}
static inline int boot_ax_join(BootContext *c, uint64_t self, uint64_t queue, int frozen, uint64_t *submitted, uint64_t *bad, int *xdiag) {
    *xdiag = 0;
    const int lf = boot_ax_lf, now = lf ? 2 : boot_ax_mode_now(c), mode = lf ? 2 : now > 1 ? 1 : now;   /* a call holding the renderer lock joins (stage 1) */
    if (!mode) { if (boot_nvn_ax_mode()) ++boot_ax.c.sync[BOOT_AX_WHY_REQUEST]; return BOOT_AX_SYNC; }
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native; const uint64_t count = c->x[1];
    unsigned why = BOOT_AX_WHYS;
    if (!boot_mt.active || !boot_mt_core_ctx(c)) why = BOOT_AX_WHY_SINGLE;   /* X's context is a core copy (PRECHECKS 1); X's exclusion is the renderer lock */
    else if (!boot_nvn_dsnap.active) why = BOOT_AX_WHY_NODSNAP;
    else if (!w) why = BOOT_AX_WHY_WINDOW;
    else if (count > (1u << 20)) why = BOOT_AX_WHY_COUNT;
    if (why == BOOT_AX_WHYS && !boot_ax_start()) why = BOOT_AX_WHY_THREAD;
    if (why == BOOT_AX_WHY_THREAD) __atomic_store_n(&boot_ax.live, 0, __ATOMIC_RELEASE);   /* no translation thread: today's code for good */
    if (why < BOOT_AX_WHYS) return boot_ax_sync(c, why);
    const uint64_t q0 = boot_nvn_qpc();
    /* the handles as today's loop reads them: the values the call read (freeze), then guest memory; a failed read ends the prefix */
    unsigned deg = BOOT_AX_DEGS;
    if (mode >= 2) {
        if (!boot_nvn_dsnap.covered) deg = BOOT_AX_DEG_COVER;
        else if (boot_ax_check_on() && !((boot_ax.c.async + boot_ax.c.join + 1) % boot_ax_check_every())) deg = BOOT_AX_DEG_SENTINEL;
    } else boot_ax_drain_items(c, BOOT_AX_W_MODE);   /* stage 1: X runs inside this thread's renderer-lock section: nothing may be ahead of it */
    if (boot_ax.core_from != (const BootCore *)(const void *)c) { boot_ax_drain_items(c, BOOT_AX_W_CORE); boot_ax_core_from((const BootCore *)(const void *)c); }
    BootAxSub *S = boot_ax_sub_take(c, mode >= 2 && deg == BOOT_AX_DEGS);
    if (!S) return boot_ax_sync(c, BOOT_AX_WHY_MEMORY);
    if (count > S->hcap) {
        uint64_t *h = (uint64_t *)realloc(S->h, (size_t)count * 8);
        if (!h) { const int async = mode >= 2 && deg == BOOT_AX_DEGS; AcquireSRWLockExclusive(&boot_ax.f.lock); S->in_use = 0; if (async && boot_ax.inflight) --boot_ax.inflight; ReleaseSRWLockExclusive(&boot_ax.f.lock);
                  return boot_ax_sync(c, BOOT_AX_WHY_MEMORY); }
        S->h = h; S->hcap = (uint32_t)count;
    }
    S->q_cap0 = q0;
    int read_fail = 0; S->nh = 0;
    for (uint64_t k = 0; k < count; ++k) {
        uint64_t h;
        if (frozen && k < boot_nvn_dsnap.top_n) h = boot_nvn_dsnap.top[k];
        else if (!boot_host_read(c, self, c->x[2] + 8 * k, &h, 8)) { read_fail = 1; break; }
        S->h[S->nh++] = h;
    }
    if (read_fail && mode >= 2 && deg == BOOT_AX_DEGS) {   /* the failure must stop this call: join (the slot taken as async goes back to the join count) */
        deg = BOOT_AX_DEG_READ; AcquireSRWLockExclusive(&boot_ax.f.lock); if (boot_ax.inflight) --boot_ax.inflight; ReleaseSRWLockExclusive(&boot_ax.f.lock);
    }
    const int async = mode >= 2 && deg == BOOT_AX_DEGS;
    S->queue = queue; S->self = self; S->window = c->window_native; S->tid = GetCurrentThreadId();
    S->done = 0; S->fail_k = 0; S->failed = 0; S->status = 0; S->k = 0; S->dyn_ok = 0;
    S->flags = mode >= 2 ? BOOT_AX_IT_XDIAG | (async ? 0u : BOOT_AX_IT_JOIN) : BOOT_AX_IT_DELEGATED | BOOT_AX_IT_JOIN;
    S->pins_in = boot_nvn_pins.n; boot_nvn_pins_swap(&S->pins);   /* the call's pins go with S (this thread's set is now empty) */
    S->own_ds = mode >= 2;
    if (S->own_ds) boot_nvn_dsnap_swap(&S->ds);   /* stage 2: the walk table goes with S (this thread's next walk fills another) */
    S->reg_epoch = (uint64_t)__atomic_load_n(&boot_ax_reg_epoch, __ATOMIC_ACQUIRE);
    boot_ax_capture_obj(c, self, S);
    boot_ax_capture_cb(c, self, w, S);
    S->ds_cap = 0; S->ds_cap_bytes = 0;
    if (mode >= 2) boot_ax_capture_dyn(c, self, S);   /* X's boot_ds_capture fills the shadow from it */
    else {   /* stage 1: the data-snapshot call-start copy here (window lock; X is idle and this thread holds the renderer lock) */
        AcquireSRWLockExclusive(&w->lock);
        BootRender *r = w->render; const uint64_t cap0 = boot_ds_c.cap_bytes;
        S->ds_cap = r != NULL;
        if (r) boot_ds_capture(r);   /* X's boot_nvn_exec_submit then finds it done (capture_due); no renderer yet: X makes it */
        S->ds_cap_bytes = boot_ds_c.cap_bytes - cap0;
        ReleaseSRWLockExclusive(&w->lock);
        if (S->ds_cap) ++boot_ax.c.ds_cap_t2; else ++boot_ax.c.ds_cap_x;
    }
    boot_ax.c.ds_cap_bytes += S->ds_cap_bytes; boot_ax.c.cb_bytes += S->cap_ok ? S->cap.used : 0; boot_ax.c.pins_in += S->pins_in;
    S->check = 0;
    if (!async && boot_ax_check_on() && (mode < 2 ? !((boot_ax.f.seq + 1) % boot_ax_check_every()) : deg == BOOT_AX_DEG_SENTINEL)) {   /* the sentinel samples this one */
        if (!boot_ax_chk.e) { boot_ax_chk.e = (BootAxChkEnt *)malloc((size_t)BOOT_AX_CHK_CAP * sizeof *boot_ax_chk.e); boot_ax_chk.cap = boot_ax_chk.e ? BOOT_AX_CHK_CAP : 0; }
        boot_ax_chk.n = 0; boot_ax_chk.lost = 0; S->check = boot_ax_chk.e != NULL;
    }
    /* hand-off: stage 2 async returns now; a join waits outside guest code (X's pack can park this core, PRECHECKS 1 #4) */
    S->q_enq = boot_nvn_qpc(); boot_ax.c.q_cap += S->q_enq - S->q_cap0;
    uint64_t seq = 0;
    if (mode >= 2) seq = boot_ax_push_wait(c, BOOT_AX_OP_SUBMIT, 0, S->flags, S, NULL, 0);
    else for (;;) {   /* stage 1: the only item, else X would wait for the renderer lock this thread holds */
        if ((seq = boot_ax_fifo_push_ex(&boot_ax.f, BOOT_AX_OP_SUBMIT, 0, S->flags, S, NULL, 0, 1)) != 0) { ++boot_ax.c.ops[BOOT_AX_OP_SUBMIT]; break; }
        boot_ax_drain_items(c, BOOT_AX_W_MODE);
    }
    S->seq = seq;
    boot_ax_wr_add(seq);
    if (async) {
        AcquireSRWLockShared(&boot_ax.f.lock); const uint32_t d = boot_ax.inflight; ReleaseSRWLockShared(&boot_ax.f.lock);
        ++boot_ax.c.depth_hist[d < 5 ? d : 4]; ++boot_ax.c.async;
        S->q_ret = boot_nvn_qpc(); boot_ax.c.q_call_async += S->q_ret - S->q_cap0;
        *submitted += S->nh; *xdiag = 1;
        return BOOT_AX_QUEUED;
    }
    if (deg < BOOT_AX_DEGS) ++boot_ax.c.degrade[deg];
    {   BootCore *k = boot_ov_leave_guest(c); BootAxWd wd = {BOOT_AX_W_JOIN, seq}; const uint64_t j0 = boot_nvn_qpc();
        if (mode >= 2) { const LONG depth = boot_ov_cs_release(); boot_ax_fifo_wait(&boot_ax.f, seq, 2000, boot_ax_watchdog, &wd); boot_ov_cs_reacquire(depth); }
        else boot_ax_fifo_wait(&boot_ax.f, seq, 2000, boot_ax_watchdog, &wd);   /* stage 1: the renderer lock stays with this thread (X runs inside its section) */
        boot_ov_enter_guest(k);
        const uint64_t dq = boot_nvn_qpc() - j0; ++boot_ax.c.w_n[BOOT_AX_W_JOIN]; boot_ax.c.w_qpc[BOOT_AX_W_JOIN] += dq; if (dq > boot_ax.c.w_max[BOOT_AX_W_JOIN]) boot_ax.c.w_max[BOOT_AX_W_JOIN] = dq; }
    const uint64_t q1 = boot_nvn_qpc(), hand = S->q_x0 > S->q_enq ? S->q_x0 - S->q_enq : 0;
    boot_ax.c.q_wait += q1 - S->q_enq; boot_ax.c.q_x += S->q_x1 - S->q_x0; boot_ax.c.q_handoff += hand; if (hand > boot_ax.handoff_max) boot_ax.handoff_max = hand;
    boot_nvn_pins_swap(&S->pins);   /* back to this thread: boot_nvn_dsnap_end releases them at the end of the call, as before */
    if (S->own_ds) boot_nvn_dsnap_swap(&S->ds);   /* and the walk table (the sentinel replays against it; dsnap_end retires it) */
    boot_ax.c.pins_added += S->pins_out > S->pins_in ? S->pins_out - S->pins_in : 0;
    if (S->check) boot_ax_chk_replay(c, self, w);
    *submitted += S->done; *xdiag = (S->flags & BOOT_AX_IT_XDIAG) != 0;
    if (S->status || S->failed) ++boot_ax.c.sticky;
    if (S->status && !c->status) { c->status = S->status; c->fault_pc = S->fault_pc; c->fault_address = S->fault_address; c->fault_width = S->fault_width; }   /* stops after this call, as on t2 */
    const int failed = S->failed; const uint64_t fh = failed ? S->h[S->fail_k] : 0;
    boot_ax_sub_give(S);
    if (failed) { ++boot_ax.c.fail; *bad = fh; return BOOT_AX_FAIL; }
    if (read_fail) { ++boot_ax.c.read_fail; return BOOT_AX_READ_FAIL; }
    ++boot_ax.c.join;
    return BOOT_AX_DONE;
}

/* ---- stage 2, the other lock-free calls (boot_ax_lf set by the wrapper) */
static inline uint64_t boot_ax_tagged(uint64_t v) { return v & BOOT_AX_VTAG; }
/* nvnQueueFenceSync: a FENCE op; the sync gets its tagged sequence number (it covers every item queued before it). */
static inline uint64_t boot_ax_fence(BootContext *c) { return BOOT_AX_VTAG | boot_ax_push_wait(c, BOOT_AX_OP_FENCE, 0, 0, NULL, NULL, 0); }
/* nvnSyncWait / a tagged value: 0 ALREADY_SIGNALED (complete at entry), 1 CONDITION_SATISFIED, 2 TIMEOUT_EXPIRED; -1: a mode below 2 runs today's
 * wait with the D3D12 value (*v replaced; X drained when the mode narrowed). */
static inline int boot_ax_sync_wait(BootContext *c, uint64_t *v, uint64_t timeout_ns) {
    const uint64_t s = *v & ~BOOT_AX_VTAG;
    if (boot_ax_vcomplete(s)) { ++boot_ax.c.sync_ready; return 0; }
    if (!boot_ax_lf) { ++boot_ax.c.sync_d3d12; boot_ax_wait_done(c, s, 0, BOOT_AX_W_MODE); *v = boot_ax_d3d12_of(s); return -1; }
    if (!timeout_ns) { ++boot_ax.c.sync_poll; return 2; }
    uint64_t deadline = 0;
    if (timeout_ns != UINT64_MAX) { const unsigned __int128 d = (unsigned __int128)timeout_ns * boot_nvn_qpf() / 1000000000u; if (d < (unsigned __int128)(UINT64_MAX / 4)) deadline = boot_nvn_qpc() + (uint64_t)d; }
    const uint64_t idle = boot_ax_push_wait(c, BOOT_AX_OP_IDLE, 0, 0, NULL, NULL, 0);   /* runs after s (FIFO order); its queue idle waits for s's GPU work */
    const int ok = boot_ax_wait_done(c, idle, deadline, deadline ? BOOT_AX_W_SYNC_TIMEOUT : BOOT_AX_W_SYNC);
    return ok && boot_ax_vcomplete(s) ? 1 : 2;
}
/* nvnQueueFinish: everything queued executed, the queue idle (boot_nvn_exec_wait's complete wait), before the call returns. */
static inline void boot_ax_finish(BootContext *c) { boot_ax_wait_done(c, boot_ax_push_wait(c, BOOT_AX_OP_FINISH, 0, 0, NULL, NULL, 0), 0, BOOT_AX_W_FINISH); }
/* nvnQueuePresentTexture: the call's half (pacing, the window texture through this thread's context) here; the PRESENT op runs the body on X. */
static inline int boot_ax_present_queue(BootContext *c, uint64_t window, uint32_t index) {
    BootAxPresentPay p;
    if (boot_nvn_exec_present_call(c, window, index, &p)) boot_ax_push_wait(c, BOOT_AX_OP_PRESENT, index, 0, NULL, &p, sizeof p);
    return 1;
}
/* nvnDeviceSetDepthMode / SetWindowOriginMode (observed, renderer lock held): applied in FIFO order (after every submission queued before). */
static inline int boot_ax_mode_op(BootContext *c, int origin, uint32_t value) {
    if (boot_nvn_ax_mode() < 2 || !boot_mt.active || boot_ax_live_mode(c, 0) < 2) return 0;
    boot_ax_push_wait(c, origin ? BOOT_AX_OP_ORIGIN_MODE : BOOT_AX_OP_DEPTH_MODE, value, 0, NULL, NULL, 0);
    return 1;
}
/* nvnMemoryPoolFinalize (observed, no renderer lock), before the driver's unmap ioctls. */
static inline void boot_ax_pool_final(BootContext *c) { if (boot_nvn_ax_mode() >= 2) boot_ax_drain(c, BOOT_AX_W_POOL); }

/* ---- every 300 presents (boot_nvn_queue_present, t2): one "async-xlate" draw-path.log line, per frame = per present */
static inline void boot_ax_present(void) {
    static uint64_t presents; static BootAxCount l; static uint64_t l_walk, l_cbn[BOOT_AX_CB_N], l_cbb[BOOT_AX_CB_N], l_ln[BOOT_AX_L_N], l_lb[BOOT_AX_L_N], l_vb, l_vs, l_ib, l_is, l_cpu, l_lost;
    static uint64_t l_hist[4], l_phist[3], l_tx_held;
    if (!boot_nvn_ax_mode() || ++presents % 300) return;
    const BootAxCount n = boot_ax.c; const double f = (double)boot_nvn_qpf(), fr = 300.0, ms = 1000.0 / f / fr, mib = 1.0 / 1048576.0 / fr;
    uint64_t cpu = 0; FILETIME tc, te, tk, tu;
    if (boot_ax.thread && GetThreadTimes(boot_ax.thread, &tc, &te, &tk, &tu)) cpu = ((uint64_t)tk.dwHighDateTime << 32 | tk.dwLowDateTime) + ((uint64_t)tu.dwHighDateTime << 32 | tu.dwLowDateTime);
    const uint64_t walk = boot_nvn_dsnap.take_qpc, vb = boot_ds_c.bytes[BOOT_DS_K_VERTEX], vs = boot_ds_c.shadow_bytes[BOOT_DS_K_VERTEX];
    const uint64_t ib = boot_ds_c.bytes[BOOT_DS_K_INDEX], is = boot_ds_c.shadow_bytes[BOOT_DS_K_INDEX], lost = boot_nvn_axr.lost;
    #define D(x) ((unsigned long long)(n.x - l.x))
    #define P(x) ((double)(n.x - l.x) / fr)
    #define CN(k) ((unsigned long long)(boot_ax_cb_n[k] - l_cbn[k]))
    #define LN(k) ((unsigned long long)(boot_ax_live_n[k] - l_ln[k]))
    char t[6144]; size_t at = 0; int w;
    uint64_t sync = 0; for (unsigned i = 0; i < BOOT_AX_WHYS; ++i) sync += n.sync[i] - l.sync[i];
    uint64_t deg = 0; for (unsigned i = 0; i < BOOT_AX_DEGS; ++i) deg += n.degrade[i] - l.degrade[i];
    w = snprintf(t, sizeof t, " (TOTK_ASYNC_XLATE %d, live %ld, depth %u, sentinel %s): last 300 presents: submissions async %llu, joined %llu [stage 2 degrades %llu: ", boot_nvn_ax_mode(),
                 (long)boot_ax.live, boot_ax_depth(), boot_ax_check_on() ? "on" : "off", D(async), D(join), (unsigned long long)deg);
    if (w > 0) at += (size_t)w;
    for (unsigned i = 0; i < BOOT_AX_DEGS && at < sizeof t; ++i) { w = snprintf(t + at, sizeof t - at, "%s%s %llu", i ? ", " : "", boot_ax_deg_name[i], D(degrade[i])); if (w > 0) at += (size_t)w; }
    if (at < sizeof t) { w = snprintf(t + at, sizeof t - at, "], on the calling thread %llu [", (unsigned long long)sync); if (w > 0) at += (size_t)w; }
    for (unsigned i = 0; i < BOOT_AX_WHYS && at < sizeof t; ++i) { w = snprintf(t + at, sizeof t - at, "%s%s %llu", i ? ", " : "", boot_ax_why_name[i], D(sync[i])); if (w > 0) at += (size_t)w; }
    if (at < sizeof t) {
        w = snprintf(t + at, sizeof t - at, "], translated on X %llu, failed %llu, read failed %llu, sticky errors %llu, order alarms %llu (must stay 0), watchdog %llu, mode steps %llu;"
                     " ops queued / run: submit %llu / %llu, fence %llu / %llu, present %llu / %llu, idle %llu / %llu, finish %llu / %llu, depth/origin %llu / %llu;"
                     " queue depth at the push 1: %llu 2: %llu 3: %llu 4: %llu",
                     D(translated), D(fail), D(read_fail), D(sticky), D(order_alarms), D(watchdog), D(steps), D(ops[BOOT_AX_OP_SUBMIT]), D(ops_done[BOOT_AX_OP_SUBMIT]),
                     D(ops[BOOT_AX_OP_FENCE]), D(ops_done[BOOT_AX_OP_FENCE]), D(ops[BOOT_AX_OP_PRESENT]), D(ops_done[BOOT_AX_OP_PRESENT]), D(ops[BOOT_AX_OP_IDLE]), D(ops_done[BOOT_AX_OP_IDLE]),
                     D(ops[BOOT_AX_OP_FINISH]), D(ops_done[BOOT_AX_OP_FINISH]), (unsigned long long)(n.ops[BOOT_AX_OP_DEPTH_MODE] + n.ops[BOOT_AX_OP_ORIGIN_MODE] - l.ops[BOOT_AX_OP_DEPTH_MODE] - l.ops[BOOT_AX_OP_ORIGIN_MODE]),
                     (unsigned long long)(n.ops_done[BOOT_AX_OP_DEPTH_MODE] + n.ops_done[BOOT_AX_OP_ORIGIN_MODE] - l.ops_done[BOOT_AX_OP_DEPTH_MODE] - l.ops_done[BOOT_AX_OP_ORIGIN_MODE]),
                     D(depth_hist[1]), D(depth_hist[2]), D(depth_hist[3]), D(depth_hist[4]));
        if (w > 0) at += (size_t)w;
    }
    if (at < sizeof t) {
        const uint64_t na = n.async - l.async; const double call = na ? (double)(n.q_call_async - l.q_call_async) * 1000.0 / f / (double)na : 0.0;
        w = snprintf(t + at, sizeof t - at, "; per frame: walk %.3f ms, capture %.3f ms (eager CB %.3f MiB: ranges %.1f, duplicate %.1f, GPU-ordered %.1f, unmapped %.1f, over budget %.1f,"
                     " partly zero %.1f, carried from X's bindings %.1f, from submissions X had not finished %.1f; call-start chunks %.1f (%.3f MiB, pages unmapped %.1f) / stage 1 copy %.3f MiB;"
                     " object words %.1f, unreadable %llu; mapping snapshot rebuilds %llu, %.3f ms), X translate %.2f ms (cpu %.2f ms); async: t2 submit returns after %.3f ms per call,"
                     " X translates them in %.2f ms, t2 time saved %.2f ms per frame; join waits %.2f ms (hand-off %.3f ms, max %.3f ms); summaries per frame: cb %.0f vb %.0f ib %.0f ssbo %.0f"
                     " ind %.0f cond %.0f writes %.0f obj %.0f (lost %llu, write-set overflows %llu); pins moved %llu, added on X %llu",
                     (double)(walk - l_walk) * ms, (double)(n.q_cap - l.q_cap) * ms, (double)(n.cb_bytes - l.cb_bytes) * mib, P(caps[BOOT_AX_CAPS_RANGES]), P(caps[BOOT_AX_CAPS_DUPS]),
                     P(caps[BOOT_AX_CAPS_GPU]), P(caps[BOOT_AX_CAPS_UNMAPPED]), P(caps[BOOT_AX_CAPS_OVER]), P(caps[BOOT_AX_CAPS_ZERO]), P(caps[BOOT_AX_CAPS_CARRY]), P(caps[BOOT_AX_CAPS_INFLIGHT]),
                     P(dyn_chunks), (double)(n.dyn_bytes - l.dyn_bytes) * mib, P(dyn_pages_missed), (double)(n.ds_cap_bytes - l.ds_cap_bytes) * mib, P(obj_words), D(obj_unread),
                     D(map_rebuilds), (double)(n.map_qpc - l.map_qpc) * ms * fr,
                     (double)(n.q_x + n.q_x_async - l.q_x - l.q_x_async) * ms, l_cpu && cpu >= l_cpu ? (double)(cpu - l_cpu) / 10000.0 / fr : 0.0, call,
                     (double)(n.q_x_async - l.q_x_async) * ms, (double)((int64_t)(n.q_x_async - l.q_x_async) - (int64_t)(n.q_call_async - l.q_call_async)) * ms,
                     (double)(n.q_wait - l.q_wait) * ms, (double)(n.q_handoff - l.q_handoff) * ms, (double)boot_ax.handoff_max * 1000.0 / f,
                     P(axr[BOOT_NVN_AXR_CB]), P(axr[BOOT_NVN_AXR_VB]), P(axr[BOOT_NVN_AXR_IB]), P(axr[BOOT_NVN_AXR_SSBO]), P(axr[BOOT_NVN_AXR_IND]), P(axr[BOOT_NVN_AXR_COND]),
                     (double)(n.axr[BOOT_NVN_AXR_W_UBO] + n.axr[BOOT_NVN_AXR_W_CLEAR] + n.axr[BOOT_NVN_AXR_W_REPORT] - l.axr[BOOT_NVN_AXR_W_UBO] - l.axr[BOOT_NVN_AXR_W_CLEAR] - l.axr[BOOT_NVN_AXR_W_REPORT]) / fr,
                     P(axr[BOOT_NVN_AXR_OBJ]), (unsigned long long)(lost - l_lost), D(wr_over), D(pins_in), D(pins_added));
        if (w > 0) at += (size_t)w;
    }
    if (at < sizeof t) {   /* waits for X: virtual fences and the rest */
        w = snprintf(t + at, sizeof t - at, "; virtual-fence waits: complete at entry %llu, polls not complete %llu, after a mode step (D3D12) %llu", D(sync_ready), D(sync_poll), D(sync_d3d12));
        if (w > 0) at += (size_t)w;
        for (unsigned i = 0; i < BOOT_AX_WS && at < sizeof t; ++i) {
            if (n.w_n[i] == l.w_n[i]) continue;
            w = snprintf(t + at, sizeof t - at, ", %s %llu (%.3f ms per frame, max %.3f ms)", boot_ax_w_name[i], D(w_n[i]), (double)(n.w_qpc[i] - l.w_qpc[i]) * ms, (double)n.w_max[i] * 1000.0 / f);
            if (w > 0) at += (size_t)w;
        }
    }
    if (at < sizeof t) {   /* residual live guest reads: the first group must stay 0, the second is live by design (DESIGN 4.3, 4.6) */
        w = snprintf(t + at, sizeof t - at, "; X residual live reads in these 300 presents (must stay 0): CB not summarized %llu (%.3f MiB), shorter than the copy %llu, unmapped at the capture %llu,"
                     " over budget %llu, captured but stale %llu, vertex/index snapshot bytes from guest memory %.3f MiB, %s %llu, %s %llu, %s %llu; by design: CB GPU-ordered at the capture %llu"
                     " (%.3f MiB), CB not snapshot-taken %llu (%.3f MiB, GPU order / GPU-only / budget / compute), program constants %llu, %s %llu, %s %llu (%.3f MiB), %s %llu (%.3f MiB),"
                     " %s %.3f MiB, texel writes held for a later submission %llu, %s %llu; CB from the capture %llu (%.3f MiB); registry calls %llu (while X translates %llu, with"
                     " submissions queued %llu, submissions translated after one %llu); kept entries: textures/samplers %llu (older entries served %llu, none kept %llu, memo resets %llu),"
                     " programs %llu (older served %llu, none kept %llu)",
                     CN(BOOT_AX_CB_MISS), (double)(boot_ax_cb_bytes[BOOT_AX_CB_MISS] - l_cbb[BOOT_AX_CB_MISS]) * mib * fr, CN(BOOT_AX_CB_SHORT), CN(BOOT_AX_CB_UNMAPPED), CN(BOOT_AX_CB_OVER),
                     CN(BOOT_AX_CB_STALE), (double)((vb - vs) - (l_vb - l_vs) + (ib - is) - (l_ib - l_is)) * mib * fr,
                     boot_ax_live_name[BOOT_AX_L_DBIND], LN(BOOT_AX_L_DBIND), boot_ax_live_name[BOOT_AX_L_POOLWORD], LN(BOOT_AX_L_POOLWORD), boot_ax_live_name[BOOT_AX_L_OBJECT], LN(BOOT_AX_L_OBJECT),
                     CN(BOOT_AX_CB_GPU), (double)(boot_ax_cb_bytes[BOOT_AX_CB_GPU] - l_cbb[BOOT_AX_CB_GPU]) * mib * fr,
                     CN(BOOT_AX_CB_NOT_TAKEN), (double)(boot_ax_cb_bytes[BOOT_AX_CB_NOT_TAKEN] - l_cbb[BOOT_AX_CB_NOT_TAKEN]) * mib * fr, CN(BOOT_AX_CB_CODE),
                     boot_ax_live_name[BOOT_AX_L_CODE], LN(BOOT_AX_L_CODE),
                     boot_ax_live_name[BOOT_AX_L_SHADER], LN(BOOT_AX_L_SHADER), (double)(boot_ax_live_bytes[BOOT_AX_L_SHADER] - l_lb[BOOT_AX_L_SHADER]) * mib * fr,
                     boot_ax_live_name[BOOT_AX_L_TEXEL], LN(BOOT_AX_L_TEXEL), (double)(boot_ax_live_bytes[BOOT_AX_L_TEXEL] - l_lb[BOOT_AX_L_TEXEL]) * mib * fr,
                     boot_ax_live_name[BOOT_AX_L_REHASH], (double)(boot_ax_live_bytes[BOOT_AX_L_REHASH] - l_lb[BOOT_AX_L_REHASH]) * mib * fr,
                     (unsigned long long)(boot_ax_texel_held - l_tx_held), boot_ax_live_name[BOOT_AX_L_DIAG], LN(BOOT_AX_L_DIAG),
                     CN(BOOT_AX_CB_HIT), (double)(boot_ax_cb_bytes[BOOT_AX_CB_HIT] - l_cbb[BOOT_AX_CB_HIT]) * mib * fr,
                     D(reg_calls), D(reg_during), D(reg_queued), D(reg_subs), (unsigned long long)(boot_ax_hist_c[0] - l_hist[0]), (unsigned long long)(boot_ax_hist_c[1] - l_hist[1]),
                     (unsigned long long)(boot_ax_hist_c[2] - l_hist[2]), (unsigned long long)(boot_ax_hist_c[3] - l_hist[3]),
                     (unsigned long long)(boot_nvn_prog_hist_c[0] - l_phist[0]), (unsigned long long)(boot_nvn_prog_hist_c[1] - l_phist[1]), (unsigned long long)(boot_nvn_prog_hist_c[2] - l_phist[2]));
        if (w > 0) at += (size_t)w;
    }
    if (boot_ax_check_on() && at < sizeof t) {
        w = snprintf(t + at, sizeof t - at, "; sentinel (every %u): checked %llu, records %llu (differ %llu), handles %llu (differ %llu, not frozen %llu), CB copies %llu (same %llu,"
                     " changed since the capture %llu = saved by it, taken and changed since X read them live %llu = raced, not taken or GPU-ordered and changed %llu = GPU order, unreadable %llu),"
                     " draws %llu (equal %llu, a record differs %llu, CB raced %llu, CB saved %llu, GPU-order change %llu), log full %llu", boot_ax_check_every(), D(chk_subs), D(chk_recs), D(chk_rec_bad), D(chk_handles), D(chk_handle_bad), D(chk_unfrozen),
                     D(chk_cbs), D(chk_cb_same), D(chk_cb_saved), D(chk_cb_raced), D(chk_cb_gpu), D(chk_cb_unread), D(chk_draws), D(chk_draw_eq), D(chk_draw_rec),
                     D(chk_draw_raced), D(chk_draw_saved), D(chk_draw_gpu), D(chk_lost));
        if (w > 0) at += (size_t)w;
    }
    #undef D
    #undef P
    #undef CN
    #undef LN
    boot_draw_log("nvn native: async-xlate%s", t, 0, 0);
    l = n; l_walk = walk; l_vb = vb; l_vs = vs; l_ib = ib; l_is = is; l_cpu = cpu; l_lost = lost; boot_ax.handoff_max = 0;
    for (unsigned i = 0; i < BOOT_AX_WS; ++i) boot_ax.c.w_max[i] = 0;
    memcpy(l_cbn, boot_ax_cb_n, sizeof l_cbn); memcpy(l_cbb, boot_ax_cb_bytes, sizeof l_cbb); memcpy(l_ln, boot_ax_live_n, sizeof l_ln); memcpy(l_lb, boot_ax_live_bytes, sizeof l_lb);
    memcpy(l_hist, boot_ax_hist_c, sizeof l_hist); memcpy(l_phist, boot_nvn_prog_hist_c, sizeof l_phist); l_tx_held = boot_ax_texel_held;
}
#endif
