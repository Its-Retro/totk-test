#ifndef TOTK_HOST_NVN_H
#define TOTK_HOST_NVN_H
/* NVN API in host C (nvn-work/DESIGN.md). TOTK_NVN selects, per process:
 *   off (default)  nothing registered; the game's NVN driver (translated sdk code) runs as before;
 *   trace          stage 0 tracer around the real driver (host_nvn_trace.h);
 *   native         the host implementation below: the driver never runs, so no Maxwell command
 *                  buffers exist. Functions not implemented yet stop the guest with their name
 *                  ("nvn native: unimplemented <name>"), so gaps show immediately.
 *
 * Attachment (both modes): a host-function hook on the sdk export nvnBootstrapLoader (sdk+0x4900b0,
 * reached through main's PLT). Native mode answers it with fake addresses "nvn:<name>"
 * (host_fn.h, name-derived, stable across processes), so the game's proc loader fills its NVN
 * function-pointer globals with host functions.
 *
 * Native state lives in the guest objects the game allocates (opaque NVN storage) and in guest
 * memory the game hands to NVN (command memory), never only in host statics: a process resumed
 * from a checkpoint continues with the same objects. Layouts below are ours; sizes must stay
 * within the NVN object sizes (BOOT_NVN_SIZE_*, from the public NVN headers; the stage 0 trace
 * confirms them, see nvn-work/tools/nvn_trace_decode.py).
 *
 * Command buffers record our own API-level command stream (not Maxwell methods) into the command
 * memory the game provides: one record per nvnCommandBuffer* call (function index, x1-x7, q0-q3,
 * argument memory the call reads at record time). The handle returned by EndRecording is the guest
 * address of its first record. Queues hand handles to a backend (BootNvnBackend: the D3D12
 * executor is stage 2; the null backend walks the records), so recorded work, fences and present
 * go through one narrow interface that the unit tests replace. */
#include "host_nvn_trace.h"

/* ---------------------------------------------------------------- ABI (verify with the trace) */
enum {
    BOOT_NVN_SIZE_DEVICE_BUILDER = 64, BOOT_NVN_SIZE_DEVICE = 12288, BOOT_NVN_SIZE_QUEUE_BUILDER = 64, BOOT_NVN_SIZE_QUEUE = 8192,
    BOOT_NVN_SIZE_COMMAND_BUFFER = 160, BOOT_NVN_SIZE_MEMORY_POOL_BUILDER = 64, BOOT_NVN_SIZE_MEMORY_POOL = 256,
    BOOT_NVN_SIZE_BUFFER_BUILDER = 64, BOOT_NVN_SIZE_BUFFER = 48, BOOT_NVN_SIZE_SYNC = 64,
};
enum { BOOT_NVN_SYNC_ALREADY_SIGNALED = 0, BOOT_NVN_SYNC_CONDITION_SATISFIED = 1, BOOT_NVN_SYNC_TIMEOUT_EXPIRED = 2, BOOT_NVN_SYNC_FAILED = 3 };
enum { BOOT_NVN_MEMORY_EVENT_OUT_OF_COMMAND = 0, BOOT_NVN_MEMORY_EVENT_OUT_OF_CONTROL = 1 };
#define BOOT_NVN_VA_BASE   UINT64_C(0x0000000400000000)   /* GPU virtual addresses handed to the game */
#define BOOT_NVN_VA_ALIGN  UINT64_C(0x10000)
#ifndef BOOT_NVN_QUEUE_MEMORY_SIZE
#define BOOT_NVN_QUEUE_MEMORY_SIZE 0x108000u              /* nvnQueueBuilderGetQueueMemorySize: the driver's answer (stage 0 trace) */
#endif
/* nvnDeviceGetInteger answers: filled from the trace (pname -> driver value). Unknown pnames log and return 0. */
typedef struct { uint32_t pname; int32_t value; } BootNvnInteger;
#ifndef BOOT_NVN_INTEGERS
#define BOOT_NVN_INTEGERS {0xffffffffu, 0}
#endif
static const BootNvnInteger boot_nvn_integers[] = {BOOT_NVN_INTEGERS};

/* ---------------------------------------------------------------- guest object layouts (ours) */
enum { BOOT_NVN_MAGIC_DEVICE = 0x444e564eu, BOOT_NVN_MAGIC_DEVICE_BUILDER = 0x42444e56u, BOOT_NVN_MAGIC_POOL = 0x4c504e56u,
       BOOT_NVN_MAGIC_POOL_BUILDER = 0x42504e56u, BOOT_NVN_MAGIC_BUFFER = 0x46424e56u, BOOT_NVN_MAGIC_BUFFER_BUILDER = 0x42424e56u,
       BOOT_NVN_MAGIC_QUEUE = 0x55514e56u, BOOT_NVN_MAGIC_QUEUE_BUILDER = 0x42514e56u, BOOT_NVN_MAGIC_CMD = 0x44434e56u,
       BOOT_NVN_MAGIC_SYNC = 0x59534e56u };
typedef struct BootNvnDevice { uint32_t magic, flags, depth_mode, window_origin; uint64_t va_next, debug_callback, debug_data; } BootNvnDevice;
typedef struct BootNvnDeviceBuilder { uint32_t magic, flags; } BootNvnDeviceBuilder;
typedef struct BootNvnPoolBuilder { uint32_t magic, flags; uint64_t device, storage, size; } BootNvnPoolBuilder;
typedef struct BootNvnPool { uint32_t magic, flags; uint64_t device, storage, size, va; } BootNvnPool;
typedef struct BootNvnBufferBuilder { uint32_t magic, pad; uint64_t device, pool, offset, size; } BootNvnBufferBuilder;
typedef struct BootNvnBuffer { uint32_t magic, pad; uint64_t va, cpu, size; } BootNvnBuffer;
typedef struct BootNvnQueueBuilder { uint32_t magic, flags, command_size, compute_size, control_size, pad; uint64_t device, memory, memory_size; } BootNvnQueueBuilder;
typedef struct BootNvnQueue { uint32_t magic, flags; uint64_t device, submitted, fence_next, completed; } BootNvnQueue;
typedef struct BootNvnSync { uint32_t magic, pad; uint64_t queue, value; } BootNvnSync;
typedef struct BootNvnCmd {
    uint32_t magic, recording, callback_enabled, pad;
    uint64_t device;
    uint64_t cmd_cur, cmd_end, cmd_used;        /* guest CPU addresses of the current command memory chunk */
    uint64_t ctrl_cur, ctrl_end, ctrl_used;
    uint64_t callback, callback_data;
    uint64_t start;                             /* first record of the recording in progress */
} BootNvnCmd;
_Static_assert(sizeof(BootNvnDevice) <= BOOT_NVN_SIZE_DEVICE, "device");
_Static_assert(sizeof(BootNvnDeviceBuilder) <= BOOT_NVN_SIZE_DEVICE_BUILDER, "device builder");
_Static_assert(sizeof(BootNvnPoolBuilder) <= BOOT_NVN_SIZE_MEMORY_POOL_BUILDER, "pool builder");
_Static_assert(sizeof(BootNvnPool) <= BOOT_NVN_SIZE_MEMORY_POOL, "pool");
_Static_assert(sizeof(BootNvnBufferBuilder) <= BOOT_NVN_SIZE_BUFFER_BUILDER, "buffer builder");
_Static_assert(sizeof(BootNvnBuffer) <= BOOT_NVN_SIZE_BUFFER, "buffer");
_Static_assert(sizeof(BootNvnQueueBuilder) <= BOOT_NVN_SIZE_QUEUE_BUILDER, "queue builder");
_Static_assert(sizeof(BootNvnQueue) <= BOOT_NVN_SIZE_QUEUE, "queue");
_Static_assert(sizeof(BootNvnSync) <= BOOT_NVN_SIZE_SYNC, "sync");
_Static_assert(sizeof(BootNvnCmd) <= BOOT_NVN_SIZE_COMMAND_BUFFER, "command buffer");

/* ---------------------------------------------------------------- command stream */
/* Records are 4-byte aligned words in host memory (the shadow store below), not in guest command memory:
 *   u32 header = op (bits 0-15) | words incl. header (bits 16-31)
 *   op < BOOT_NVN_NAME_COUNT: a recorded call: u64 x1..x7, u32 q0..q3 (low 4 bytes), then memory
 *     blocks {u32 arg, u32 bytes, data padded to 4} captured at record time;
 *   BOOT_NVN_OP_JUMP: u64 address of the next record (command memory chunk change);
 *   BOOT_NVN_OP_END: end of the handle. */
enum { BOOT_NVN_OP_JUMP = 0xfffe, BOOT_NVN_OP_END = 0xffff };
#define BOOT_NVN_RECORD_FIXED (4 + 56 + 16)
#define BOOT_NVN_JUMP_RESERVE 12u   /* every chunk keeps room for its JUMP/END */
typedef struct BootNvnRecord {
    uint32_t op, words; uint64_t x[8]; uint32_t q[4];   /* x[0] unused (the command buffer) */
    uint32_t blocks; struct { uint32_t arg, bytes; const uint8_t *data; } block[2];
} BootNvnRecord;
/* Shadow command memory. The game sizes its command memory for the driver's methods: it reads
 * nvnCommandBufferGetCommandMemoryUsed after each recording and advances its own sub-allocator by it
 * (driver trace: a program's 0x200-byte command area at pool+0x1e00 advances 0x2c per recording, and the
 * game always hands out 0x200 bytes from there, past the end of the 0x2000-byte pool). Records are several
 * times larger than the driver's methods, so writing them into that memory ran past what the game gave out
 * and overwrote the memory behind it (native stop at 8.91B steps: the game's GPU memory allocation failed and
 * nvnBufferInitialize got a NULL pool). The records of a recording therefore live in a host slot. Since 10 the
 * command buffer itself is the driver's (see "command buffers" below): its handle maps to the slot, and
 * readers see a handle as a JUMP to the slot's shadow address (BOOT_NVN_SHADOW | slot << 32 | offset), so
 * code that follows JUMPs is unchanged. A slot is released when the driver returns its handle again for a
 * newer recording, when its command buffer is initialized or finalized while recording, or when its
 * recording is abandoned. (BOOT_NVN_TOKEN_MAGIC is kept for the diagnostic heap scan.) */
#define BOOT_NVN_SHADOW UINT64_C(0x8000000000000000)
enum { BOOT_NVN_TOKEN_MAGIC = 0x4b544e56u, BOOT_NVN_TOKEN_BYTES = 16 };
/* pins: submissions reading the slot right now (boot_nvn_handle_pin); dead: released while pinned, freed at the
 * last unpin.
 * reclock (play-86 Q5): recording threads (t0, t47, t48) used to take the store lock exclusively for every store
 * (3-5 per record, plus a shared one for the slot's end), so they serialised on each other and on t2's fetches.
 * Now each slot has its own lock: a record is appended in one step under the lock of its slot only (recorders
 * never share a slot), and the store lock is left to slot allocation, release and pinning. Slots live in chunks
 * that never move, so a slot is reached without the store lock. data/len/cap/gen/next_free/closed change under
 * the slot lock (allocation and release hold both, store lock first); pins/dead only under the store lock. A
 * finished recording (closed by its END) never changes again, so a submission that pinned it reads it with no
 * lock at all (boot_nvn_cmd_fetch). Released slots keep a small buffer for their next recording (budgeted), so
 * steady-state recording does not reallocate.
 * TOTK_RECLOCK=0 (batch-a, A/B only): the serial store of before reclock: every append also holds the store lock
 * exclusively (recorders serialise on each other and on t2's reads again), pinned recordings are read under the
 * locks, the per-thread recording cache is not used and released buffers are freed. The slot layout stays. */
/* flashfix: dref = the recording's deferred texture binds and the command buffers it calls, indexed when they are
 * recorded (boot_nvn_dsnap_dref; added by the record's own append, under the slot lock); snap = stamp of the last
 * submit-time snapshot that read it (boot_nvn_dsnap_take). Both change under the slot lock.
 * batch-a (TOTK_FRAME_TRACE=1): rec_thread / end_qpc = the guest thread that ended the recording and when its handle
 * was installed (boot_nvn_cmd_end_ret), for the per-submission frame trace. */
typedef struct BootNvnDref { uint32_t off, count; uint64_t ptr, bound; } BootNvnDref;   /* count UINT32_MAX: ptr is a CallCommands handle, bound the
                                                                                     * version bound at record time (call-bind; 0: none) */
typedef struct BootNvnSlot {
    SRWLOCK lock; uint8_t *data; uint32_t len, cap, gen, next_free, pins, dead, closed; uint64_t token;
    BootNvnDref *dref; uint32_t dref_n, dref_cap, snap, rec_thread; uint64_t end_qpc;
    uint32_t ver_bytes, ver_pad;   /* rerecord-work: bytes counted while it is a version (dead and pinned; store lock) */
    /* rec-evict (see boot_nvn_ev_dead_mode): the handle it is installed under and its control / command memory ranges (guest CPU
     * addresses; length 0 = none) as the overwrite index has them (boot_nvn_ev.lock; ev_in = indexed); ev_use / ev_reach: the
     * present at which a submit call's walk last reached it, and how (BOOT_NVN_EV_SUBMITTED at depth 0, BOOT_NVN_EV_CALLED through
     * CallCommands; walk, slot lock); ev_mark: dry-run marks (BOOT_NVN_EV_WOULD_*; set under the store lock, atomic). Reset by
     * boot_nvn_slot_new_gen; ev_in and ev_mark are dropped by boot_nvn_slot_free_locked. */
    uint64_t ev_handle, ev_ctl, ev_cmd; uint32_t ev_ctl_len, ev_cmd_len, ev_use; uint8_t ev_reach, ev_in, ev_mark, ev_pad;
    uint32_t cb_refs, cb_bytes; uint8_t cb_flags, cb_pad[3];   /* call-bind (see "call-bind" below; store lock): bindings, held bytes, BOOT_NVN_CB_* */
} BootNvnSlot;
enum { BOOT_NVN_CB_VER = 1, BOOT_NVN_CB_HELD = 2, BOOT_NVN_CB_INVALID = 4, BOOT_NVN_CB_STRIPPED = 8, BOOT_NVN_CB_INPLACE = 16 };   /* call-bind: cb_flags */
enum { BOOT_NVN_EV_SUBMITTED = 1, BOOT_NVN_EV_CALLED = 2 };                                /* rec-evict: ev_reach */
enum { BOOT_NVN_EV_WOULD_DEAD = 1, BOOT_NVN_EV_WOULD_EVICT = 2, BOOT_NVN_EV_REUSED = 4 };   /* rec-evict: ev_mark */
static volatile LONG boot_nvn_ev_now;   /* rec-evict: presents so far (boot_nvn_ev_present) */
enum { BOOT_NVN_SLOT_CHUNK_BITS = 12, BOOT_NVN_SLOT_CHUNKS = 4096 };   /* 16M slots */
#define BOOT_NVN_SLOT_FIRST_CAP 1024u
#define BOOT_NVN_SLOT_KEEP_CAP (16u << 10)          /* buffers up to this size are kept by a released slot */
#define BOOT_NVN_SLOT_KEEP_TOTAL (UINT64_C(32) << 20)   /* ... up to this many bytes in all */
typedef struct BootNvnShadowStore {
    SRWLOCK lock; BootNvnSlot *chunk[BOOT_NVN_SLOT_CHUNKS]; uint32_t count, cap, free_head, live, live_logged;
    volatile LONG64 bytes; uint64_t kept, dref_kept;   /* recfast: dref_kept = bytes of index arrays kept by released slots */
} BootNvnShadowStore;
static inline BootNvnShadowStore *boot_nvn_shadow(void) {
    static BootNvnShadowStore s = {SRWLOCK_INIT, {NULL}, 0, 0, UINT32_MAX, 0, 0, 0, 0, 0};
    return &s;
}
static inline int boot_nvn_reclock_on(void) {   /* TOTK_RECLOCK=0: the serial store (see above) */
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_RECLOCK", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0'); }
    return v;
}
/* reclock counters ("record store" in the periodic "submit cpu ms total" line, host_nvn_play.h). Per-thread
 * blocks (no shared cache line on the record path); lock waits are counted only when a try-acquire fails. */
enum { BOOT_NVN_RL_RECORDS, BOOT_NVN_RL_CACHED, BOOT_NVN_RL_GROWS, BOOT_NVN_RL_REUSED, BOOT_NVN_RL_UNALIGNED,
       BOOT_NVN_RL_READS, BOOT_NVN_RL_LOCKED_READS, BOOT_NVN_RL_PINS, BOOT_NVN_RL_PIN_REPEATS,
       BOOT_NVN_RL_POOL_HITS, BOOT_NVN_RL_POOL_NEW, BOOT_NVN_RL_HINTED, BOOT_NVN_RL_TRIMS, BOOT_NVN_RL_N };   /* recfast: the last four */
enum { BOOT_NVN_RL_STORE, BOOT_NVN_RL_SLOT, BOOT_NVN_RL_MAP, BOOT_NVN_RL_LOCKS };
/* storelock-work: the store lock's call sites (boot_nvn_sl_lock; the first five hold it shared with TOTK_STORE_LOCK_V2) and the role of
 * a thread (set by its first submit call / BeginRecording) */
enum { BOOT_NVN_SL_PIN, BOOT_NVN_SL_CBPIN, BOOT_NVN_SL_BIND, BOOT_NVN_SL_UNPIN, BOOT_NVN_SL_DROP, BOOT_NVN_SL_UNPIN_FREE, BOOT_NVN_SL_DROP_FREE,
       BOOT_NVN_SL_NEW, BOOT_NVN_SL_FREE, BOOT_NVN_SL_EVICT, BOOT_NVN_SL_OVERWRITE, BOOT_NVN_SL_UNBIND, BOOT_NVN_SL_N };
enum { BOOT_NVN_SL_ROLE_OTHER, BOOT_NVN_SL_ROLE_RECORDER, BOOT_NVN_SL_ROLE_RENDER };
typedef struct BootNvnRlStats {
    _Alignas(64) uint64_t n[BOOT_NVN_RL_N];
    uint64_t sl_acq[BOOT_NVN_SL_N], sl_wait[BOOT_NVN_SL_N], sl_wait_qpc[BOOT_NVN_SL_N], sl_hold_n[BOOT_NVN_SL_N], sl_hold_qpc[BOOT_NVN_SL_N], sl_hold_max[BOOT_NVN_SL_N];
    uint32_t sl_tick, sl_role, sl_guest, sl_pad;   /* storelock-work: per thread, see boot_nvn_sl_lock */
} BootNvnRlStats;
static BootNvnRlStats boot_nvn_rl_slots[65];   /* one per thread, the 65th shared by any further threads */
static volatile LONG boot_nvn_rl_next;
static volatile LONG64 boot_nvn_rl_waits[BOOT_NVN_RL_LOCKS], boot_nvn_rl_wait_qpc[BOOT_NVN_RL_LOCKS];
static _Thread_local BootNvnRlStats *boot_nvn_rl_mine;
static inline BootNvnRlStats *boot_nvn_rl(void) {
    if (!boot_nvn_rl_mine) { LONG k = InterlockedIncrement(&boot_nvn_rl_next) - 1; boot_nvn_rl_mine = &boot_nvn_rl_slots[k >= 0 && k < 64 ? k : 64]; }
    return boot_nvn_rl_mine;
}
static inline void boot_nvn_rl_waited(unsigned k, LARGE_INTEGER a) {
    LARGE_INTEGER b; QueryPerformanceCounter(&b);
    InterlockedIncrement64(&boot_nvn_rl_waits[k]); InterlockedExchangeAdd64(&boot_nvn_rl_wait_qpc[k], b.QuadPart - a.QuadPart);
}
static inline void boot_nvn_rl_lock(SRWLOCK *l, unsigned k) {
    if (TryAcquireSRWLockExclusive(l)) return;
    LARGE_INTEGER a; QueryPerformanceCounter(&a); AcquireSRWLockExclusive(l); boot_nvn_rl_waited(k, a);
}
static inline void boot_nvn_rl_lock_shared(SRWLOCK *l, unsigned k) {
    if (TryAcquireSRWLockShared(l)) return;
    LARGE_INTEGER a; QueryPerformanceCounter(&a); AcquireSRWLockShared(l); boot_nvn_rl_waited(k, a);
}
/* storelock-work (play-work/storelock-work/README.md): TOTK_STORE_LOCK_V2 (on unless =0; off with TOTK_RECLOCK=0). play-104 (25 min,
 * 37,890 presents): 15.1 M failed try-acquires of the store lock, 29.5 s waited (0.78 ms per present summed over the threads, ~400
 * waits of ~2 us). Per present the lock was taken ~4,700 times, all exclusively: ~1,820 by t2's submit walk (one pin per recording the
 * call reaches, boot_nvn_handle_pin; the walk took 0.71 ms per call), ~2,640 by the recording threads' CallCommands binds (one per
 * call record, a map lookup per handle inside, boot_nvn_cb_bind), once per call by t2's boot_nvn_unpin_all (all ~1,820 pins in one
 * section), and a few hundred times for slot allocation, releases and rec-evict class 1 (66 recordings, 42 class 1 releases). Pins
 * and binds only read state that changes under the exclusive lock and add one to a counter. With the switch on:
 *   - pins (boot_nvn_handle_pin, boot_nvn_cb_pin) and binds hold the lock SHARED and add with atomics (pins, cb_refs, boot_nvn_cb.refs):
 *     t2's walk and the recorders' binds no longer exclude each other. Every release, allocation and eviction still holds it
 *     exclusively, so a pin or bind is still atomic with its map lookup against every free (see boot_nvn_handle_pin);
 *   - boot_nvn_unpin_all drops its pins under the shared lock (atomic) and takes the exclusive lock only for the slots it left dead and
 *     unpinned (versions; usually none), checked again there by generation;
 *   - a released recording's CallCommands bindings (boot_nvn_cb_drop_bindings_locked: one unref per bound entry, ~2,640 per present, two
 *     cache misses each, the longest part of a caller's release) are queued per thread and dropped right after the exclusive section
 *     under the shared lock (atomic; boot_nvn_sl_flush), the versions they leave dead, unbound and unpinned freed under the exclusive
 *     lock as boot_nvn_cb_drain_locked frees them. Such a version stays held a few microseconds longer, by the thread that released
 *     its last caller; nothing can play it meanwhile (no live record names it, and the released caller was not pinned);
 *   - EndRecording's release of the handle's older recording takes one exclusive section for call-bind's in-place check and the
 *     release (boot_nvn_slot_free_ow), not two.
 * Offline model of the play-104 frame (play-work/storelock-work/test, paced as the game): waits per frame 185 -> 62, wait time summed
 * over the threads 0.73 -> 0.45 ms, most of it on the recording threads; t2's own waits 50 -> 16 per frame, ~0.13 ms either way (it
 * now waits only behind the exclusive sections, ~150 per frame). Every 300 presents the line below shows the real split.
 * =0: every site exactly as before (exclusive, unrefs inline). The counters (boot_nvn_sl_lock) run in both modes, as the A/B baseline:
 * the "lock waits: store" field split by role, and every 300 presents a "store lock" draw-path.log line by site (boot_nvn_sl_report). */
static inline int boot_nvn_slv2_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_STORE_LOCK_V2", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0') && boot_nvn_reclock_on(); }
    return v;
}
static _Thread_local struct { uint64_t *b, *r; uint32_t n, cap, rn, rcap; int flushing; } boot_nvn_sld;   /* v2: queued binding drops (versions,
                                                                                                         * slot | generation), reap candidates */
static struct { volatile LONG64 deferred, drop_frees, ev_waits, ev_wait_qpc; } boot_nvn_sl_stats;
static inline void boot_nvn_sl_flush(void);
/* The store lock at one site, counted per thread: acquisitions, failed try-acquires and their wait (also added to the totals of the
 * "lock waits: store" field, as boot_nvn_rl_lock did), and the hold time of every 64th acquisition of the thread. Returns the QPC to
 * pass to boot_nvn_sl_unlock (0: not sampled). */
static inline uint64_t boot_nvn_sl_lock(BootNvnShadowStore *s, unsigned site, int shared) {
    BootNvnRlStats *m = boot_nvn_rl(); uint64_t t0 = 0; ++m->sl_acq[site];
    if (!(shared ? TryAcquireSRWLockShared(&s->lock) : TryAcquireSRWLockExclusive(&s->lock))) {
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        if (shared) AcquireSRWLockShared(&s->lock); else AcquireSRWLockExclusive(&s->lock);
        QueryPerformanceCounter(&b); t0 = (uint64_t)b.QuadPart;
        InterlockedIncrement64(&boot_nvn_rl_waits[BOOT_NVN_RL_STORE]); InterlockedExchangeAdd64(&boot_nvn_rl_wait_qpc[BOOT_NVN_RL_STORE], b.QuadPart - a.QuadPart);
        ++m->sl_wait[site]; m->sl_wait_qpc[site] += (uint64_t)(b.QuadPart - a.QuadPart);
    }
    if (++m->sl_tick & 63) return 0;
    if (!t0) { LARGE_INTEGER q; QueryPerformanceCounter(&q); t0 = (uint64_t)q.QuadPart; }
    return t0;
}
/* After an exclusive section, the binding drops its releases queued (v2) are done here, with no store / slot / map / ev lock held. */
static inline void boot_nvn_sl_unlock(BootNvnShadowStore *s, unsigned site, int shared, uint64_t t0) {
    if (t0) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q); BootNvnRlStats *m = boot_nvn_rl(); const uint64_t d = (uint64_t)q.QuadPart - t0;
        ++m->sl_hold_n[site]; m->sl_hold_qpc[site] += d; if (d > m->sl_hold_max[site]) m->sl_hold_max[site] = d;
    }
    if (shared) ReleaseSRWLockShared(&s->lock); else ReleaseSRWLockExclusive(&s->lock);
    if (!shared && boot_nvn_sld.n && !boot_nvn_sld.flushing) boot_nvn_sl_flush();
}
/* v2, store lock held exclusively (a release): version b loses this binding right after the section. 0: drop it now (switch off, or
 * out of host memory for the queue). */
static inline int boot_nvn_sl_defer(uint64_t b) {
    if (!boot_nvn_slv2_on()) return 0;
    if (boot_nvn_sld.n == boot_nvn_sld.cap) {
        uint32_t cap = boot_nvn_sld.cap ? boot_nvn_sld.cap * 2 : 256; uint64_t *x = cap ? (uint64_t *)realloc(boot_nvn_sld.b, (size_t)cap * sizeof *x) : NULL;
        if (!x) return 0;
        boot_nvn_sld.b = x; boot_nvn_sld.cap = cap;
    }
    boot_nvn_sld.b[boot_nvn_sld.n++] = b;
    return 1;
}
/* recfast (play-work/recfast-work): play-92's line profile of the recording threads in the heavy tower view had 12-15 ms
 * of the 18-23 ms per frame that t0, t47 and t48 spend in our NVN code inside the realloc of boot_nvn_slot_append, and
 * 2-3 ms more in the free of a replaced recording's buffer (boot_nvn_slot_free_locked, under the store lock, so the other
 * recorders' boot_nvn_slot_new_gen waited behind it). A recording above BOOT_NVN_SLOT_KEEP_CAP gave its buffer back to
 * the process heap at its release, and the next recording of that command buffer grew one again from at most 16 KiB by
 * doubling: past ~0.5 MiB every step is a fresh VirtualAlloc (demand-zero page faults while the bytes are copied) and a
 * VirtualFree, all under the process heap's lock, which half of those samples were waiting for.
 * TOTK_REC_POOL_MB (default 256; 0 = off: the store exactly as before recfast; off with TOTK_RECLOCK=0, whose serial store
 * frees released buffers): released record buffers of any size go to a pool by capacity class (BOOT_NVN_SLOT_FIRST_CAP
 * << k, the only capacities the store makes), up to that many MiB in all (beyond it they are freed as before). A growth
 * takes a pooled buffer of the new capacity (or of the next class) and copies the recording's bytes into it, as realloc
 * did. TOTK_REC_HINT (on unless =0; needs the pool): at BeginRecording the slot gets a buffer of the capacity class the
 * command buffer's previous recording ended in (a table keyed by the command buffer object; a stale or colliding entry
 * only changes the capacity), so steady-state recording neither grows nor copies; a hinted recording that ended in a class
 * at least 4 times smaller than its buffer moves to a buffer of that class at its EndRecording (before its handle exists,
 * so no reader can see it), so a hint never holds more than 4 times the bytes for long. Released slots also keep their
 * deferred-bind index array (up to BOOT_NVN_DREF_KEEP_CAP entries each, BOOT_NVN_DREF_KEEP_TOTAL bytes in all) instead
 * of growing a new one from 8 entries. Records, offsets, locks and every reader are unchanged: only where a recording's
 * bytes live and which buffer a growth gets. The pool lock is a leaf (nothing is taken under it). */
enum { BOOT_NVN_RBUF_CLASSES = 22 };   /* capacities BOOT_NVN_SLOT_FIRST_CAP << k, k < 22: 1 KiB .. 2 GiB */
#define BOOT_NVN_DREF_KEEP_CAP 4096u                    /* index arrays up to this many entries are kept by a released slot */
#define BOOT_NVN_DREF_KEEP_TOTAL (UINT64_C(16) << 20)   /* ... up to this many bytes in all */
typedef struct BootNvnRbufPool { SRWLOCK lock; uint8_t *head[BOOT_NVN_RBUF_CLASSES]; uint64_t bytes, puts, drops; } BootNvnRbufPool;
static BootNvnRbufPool boot_nvn_rbuf = {SRWLOCK_INIT, {NULL}, 0, 0, 0};   /* a pooled buffer's first 8 bytes: the next one */
static inline uint64_t boot_nvn_rbuf_limit(void) {   /* bytes; 0: no pool */
    static int64_t v = -1;
    if (v < 0) {
        char s[16]; DWORD k = GetEnvironmentVariableA("TOTK_REC_POOL_MB", s, sizeof s); uint64_t mb = 256;
        if (k && k < sizeof s) { mb = 0; for (DWORD i = 0; i < k && s[i] >= '0' && s[i] <= '9' && mb < 65536; ++i) mb = mb * 10 + (uint64_t)(s[i] - '0'); }
        if (mb > 65536) mb = 65536;
        v = boot_nvn_reclock_on() ? (int64_t)(mb << 20) : 0;
    }
    return (uint64_t)v;
}
static inline int boot_nvn_rbuf_class(uint64_t cap) {   /* k with BOOT_NVN_SLOT_FIRST_CAP << k == cap; -1: not a class capacity */
    for (int k = 0; k < BOOT_NVN_RBUF_CLASSES; ++k) if (((uint64_t)BOOT_NVN_SLOT_FIRST_CAP << k) == cap) return k;
    return -1;
}
/* A pooled buffer of capacity *cap, or else of the next class (*cap updated); NULL: none pooled. */
static inline uint8_t *boot_nvn_rbuf_get(uint32_t *cap) {
    int k = boot_nvn_rbuf_class(*cap); uint8_t *p = NULL;
    if (k < 0) return NULL;
    AcquireSRWLockExclusive(&boot_nvn_rbuf.lock);
    for (int j = k; j <= k + 1 && j < BOOT_NVN_RBUF_CLASSES && !p; ++j) if ((p = boot_nvn_rbuf.head[j]) != NULL) {
        uint8_t *next; memcpy(&next, p, sizeof next); boot_nvn_rbuf.head[j] = next;
        boot_nvn_rbuf.bytes -= (uint64_t)BOOT_NVN_SLOT_FIRST_CAP << j; *cap = (uint32_t)((uint64_t)BOOT_NVN_SLOT_FIRST_CAP << j);
    }
    ReleaseSRWLockExclusive(&boot_nvn_rbuf.lock);
    return p;
}
/* 1: p (capacity cap) went to the pool; 0: the caller frees it (no pool, pool full, not a class capacity, NULL). */
static inline int boot_nvn_rbuf_put(uint8_t *p, uint64_t cap) {
    uint64_t lim = boot_nvn_rbuf_limit(); int k = boot_nvn_rbuf_class(cap), ok = 0;
    if (!p || !lim || k < 0) return 0;
    AcquireSRWLockExclusive(&boot_nvn_rbuf.lock);
    if (boot_nvn_rbuf.bytes + cap <= lim) {
        memcpy(p, &boot_nvn_rbuf.head[k], sizeof(uint8_t *)); boot_nvn_rbuf.head[k] = p; boot_nvn_rbuf.bytes += cap; ++boot_nvn_rbuf.puts; ok = 1;
    } else ++boot_nvn_rbuf.drops;
    ReleaseSRWLockExclusive(&boot_nvn_rbuf.lock);
    return ok;
}
/* The capacity hint table: 2 ways per set (entries 2*set, 2*set+1; set = the hash's top bits), entry = hash(cmd) bits 8-63 |
 * (class + 1), 0 = empty. A new key takes an empty way, or way 0 (the old way 0 moves to way 1). Races only lose a hint. */
enum { BOOT_NVN_HINT_BITS = 12 };
static int64_t boot_nvn_hint_tab[2u << BOOT_NVN_HINT_BITS];
static inline int boot_nvn_hint_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_REC_HINT", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0') && boot_nvn_rbuf_limit() != 0; }
    return v;
}
static inline uint64_t boot_nvn_hint_hash(uint64_t k) {
    k ^= k >> 33; k *= UINT64_C(0xff51afd7ed558ccd); k ^= k >> 33; k *= UINT64_C(0xc4ceb9fe1a85ec53); k ^= k >> 33; return k;
}
static inline uint64_t boot_nvn_hint_get(uint64_t cmd) {   /* bytes; 0: no hint */
    uint64_t h = boot_nvn_hint_hash(cmd); int64_t *set = &boot_nvn_hint_tab[2 * (h >> (64 - BOOT_NVN_HINT_BITS))];
    if (!cmd) return 0;
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t e = (uint64_t)__atomic_load_n(&set[w], __ATOMIC_RELAXED); unsigned k = (unsigned)(e & 0xff);
        if (k && k <= (unsigned)BOOT_NVN_RBUF_CLASSES && !((e ^ h) >> 8)) return (uint64_t)BOOT_NVN_SLOT_FIRST_CAP << (k - 1);
    }
    return 0;
}
static inline void boot_nvn_hint_set(uint64_t cmd, uint64_t len) {   /* the class a recording of len bytes ended in */
    if (!cmd || !len) return;
    uint64_t h = boot_nvn_hint_hash(cmd); int64_t *set = &boot_nvn_hint_tab[2 * (h >> (64 - BOOT_NVN_HINT_BITS))]; unsigned k = 0;
    while (k + 1 < (unsigned)BOOT_NVN_RBUF_CLASSES && ((uint64_t)BOOT_NVN_SLOT_FIRST_CAP << k) < len) ++k;
    int64_t e = (int64_t)((h & ~UINT64_C(0xff)) | (k + 1)), e0 = __atomic_load_n(&set[0], __ATOMIC_RELAXED), e1 = __atomic_load_n(&set[1], __ATOMIC_RELAXED);
    int m0 = e0 && !(((uint64_t)e0 ^ h) >> 8), m1 = e1 && !(((uint64_t)e1 ^ h) >> 8);
    if (m0 || (!m1 && !e0)) __atomic_store_n(&set[0], e, __ATOMIC_RELAXED);
    else if (m1 || !e1) __atomic_store_n(&set[1], e, __ATOMIC_RELAXED);
    else { __atomic_store_n(&set[1], e0, __ATOMIC_RELAXED); __atomic_store_n(&set[0], e, __ATOMIC_RELAXED); }
}
/* The slot with index i (NULL if none was ever made): chunks are published once and never move or go away. */
static inline BootNvnSlot *boot_nvn_slot_peek(BootNvnShadowStore *s, uint32_t i) {
    if (i >= (uint32_t)BOOT_NVN_SLOT_CHUNKS << BOOT_NVN_SLOT_CHUNK_BITS) return NULL;
    BootNvnSlot *k = __atomic_load_n(&s->chunk[i >> BOOT_NVN_SLOT_CHUNK_BITS], __ATOMIC_ACQUIRE);
    return k ? &k[i & ((1u << BOOT_NVN_SLOT_CHUNK_BITS) - 1)] : NULL;
}
static inline BootNvnSlot *boot_nvn_slot_at(BootNvnShadowStore *s, uint32_t i) {   /* i < s->count */
    return &s->chunk[i >> BOOT_NVN_SLOT_CHUNK_BITS][i & ((1u << BOOT_NVN_SLOT_CHUNK_BITS) - 1)];
}
static inline void boot_nvn_log(const char *what, const char *name, uint64_t value);
static inline uint32_t boot_nvn_slot_of(uint64_t shadow) { return (uint32_t)((shadow & ~BOOT_NVN_SHADOW) >> 32); }
static uint64_t boot_nvn_reuse_released;   /* play (62): recordings released because their command memory was reused */
/* A new empty slot (its shadow address in *shadow, its generation in *gen); 0 when out of host memory. */
static inline int boot_nvn_slot_new_gen(uint64_t *shadow, uint32_t *gen) {
    BootNvnShadowStore *s = boot_nvn_shadow(); uint32_t i; int logged = 0; uint32_t live = 0;
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_NEW, 0);
    if (s->free_head != UINT32_MAX) { i = s->free_head; s->free_head = boot_nvn_slot_at(s, i)->next_free; }
    else {
        if (s->count == s->cap) {   /* a new chunk (zeroed: unlocked locks, no buffers, generation 0) */
            uint32_t k = s->cap >> BOOT_NVN_SLOT_CHUNK_BITS;
            BootNvnSlot *n = k < BOOT_NVN_SLOT_CHUNKS ? (BootNvnSlot *)calloc((size_t)1 << BOOT_NVN_SLOT_CHUNK_BITS, sizeof *n) : NULL;
            if (!n) { boot_nvn_sl_unlock(s, BOOT_NVN_SL_NEW, 0, sl); return 0; }
            __atomic_store_n(&s->chunk[k], n, __ATOMIC_RELEASE); s->cap += 1u << BOOT_NVN_SLOT_CHUNK_BITS;
        }
        i = s->count++;
    }
    BootNvnSlot *t = boot_nvn_slot_at(s, i);
    AcquireSRWLockExclusive(&t->lock);   /* a stale recorder may still look at it (boot_nvn_slot_append) */
    if (t->cap) { s->kept -= t->cap; ++boot_nvn_rl()->n[BOOT_NVN_RL_REUSED]; }   /* the buffer kept at its release */
    if (t->dref) s->dref_kept -= (uint64_t)t->dref_cap * sizeof *t->dref;   /* recfast: the index array kept at its release */
    t->len = 0; t->token = 0; t->next_free = UINT32_MAX; t->closed = 0; ++t->gen; if (!t->gen) t->gen = 1;
    t->dref_n = 0; t->snap = 0; t->rec_thread = UINT32_MAX; t->end_qpc = 0;   /* flashfix (a freed slot's index was freed with it), batch-a */
    t->ev_handle = t->ev_ctl = t->ev_cmd = 0; t->ev_ctl_len = t->ev_cmd_len = t->ev_use = 0; t->ev_reach = t->ev_in = 0;   /* rec-evict */
    __atomic_store_n(&t->ev_mark, 0, __ATOMIC_RELAXED);
    t->cb_refs = t->cb_bytes = 0; t->cb_flags = 0;   /* call-bind (a freed slot has neither) */
    *gen = t->gen;
    ReleaseSRWLockExclusive(&t->lock);
    t->pins = t->dead = 0;
    if (++s->live >= 65536 && s->live >= 2 * s->live_logged) { s->live_logged = s->live; logged = 1; live = s->live; }
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_NEW, 0, sl);
    if (logged) { boot_nvn_log("live command recordings", "shadow store", live);   /* growth here would be a leak */
                  boot_nvn_log("recordings released by command memory reuse so far", "shadow store", boot_nvn_reuse_released); }
    *shadow = BOOT_NVN_SHADOW | (uint64_t)i << 32;
    return 1;
}
static inline int boot_nvn_slot_new(uint64_t *shadow) { uint32_t gen; return boot_nvn_slot_new_gen(shadow, &gen); }
/* rerecord-work (TOTK_RERECORD_VERSION, see boot_nvn_rerecord_version_on): the versions held, i.e. recordings released while
 * a submission reads them (dead and pinned, freed at its unpin), and the bytes each submission pinned (its reached set,
 * boot_nvn_unpin_all). Written under the store lock; n and bytes are also read without it (boot_nvn_rv_decide). */
static struct { uint64_t n, bytes, peak_n, peak_bytes, reach_bytes, reach_calls, reach_max; } boot_nvn_ver;
static inline void boot_nvn_ver_add(BootNvnSlot *t) {   /* store lock held: t becomes a version */
    AcquireSRWLockShared(&t->lock); uint32_t cap = t->cap; ReleaseSRWLockShared(&t->lock);
    t->ver_bytes = cap; t->cb_flags |= BOOT_NVN_CB_VER;   /* call-bind: dead no longer means a version (bindings keep dead ones too) */
    __atomic_store_n(&boot_nvn_ver.n, boot_nvn_ver.n + 1, __ATOMIC_RELAXED); __atomic_store_n(&boot_nvn_ver.bytes, boot_nvn_ver.bytes + cap, __ATOMIC_RELAXED);
    if (boot_nvn_ver.n > boot_nvn_ver.peak_n) boot_nvn_ver.peak_n = boot_nvn_ver.n;
    if (boot_nvn_ver.bytes > boot_nvn_ver.peak_bytes) boot_nvn_ver.peak_bytes = boot_nvn_ver.bytes;
}
static inline void boot_nvn_ev_forget_locked(BootNvnSlot *t, uint32_t i);   /* rec-evict: out of the overwrite index, dry-run marks dropped */
/* ---------------------------------------------------------------- call-bind: CallCommands bound at record time (play-work/call-bind)
 * NVN copies a callee's control entries (GPU pointers to its commands) into the caller when the caller records CallCommands
 * (sdk+0x44561c walks them), so the console plays the callee as it was then. The executor resolved each called handle at the
 * submit instead, through the handle map: a callee recorded again under the same handle between the caller's CallCommands and its
 * submit was played in its NEW version under the caller's old state (the stage-0 trace: 12,854 such reaches in 2,622 presents; a
 * per-mesh bind list re-recorded with new vertex buffers then drawn with the caller's old index count and base vertex is a candidate
 * for the island "vertex explosions"). TOTK_CALL_BIND_AT_RECORD (default 1; read once, then switchable live, see
 * boot_nvn_cb_present):
 *   1  each CallCommands record also names the version (slot | generation) its handles had when it was recorded (record block
 *      BOOT_NVN_CB_ARG, and its index entries); the version gets one binding per naming record, and a version re-recorded or released
 *      while bound stays (dead) until its last binding goes: a binding goes with the record's recording (freed, re-recorded, released
 *      by rec-evict), so callers holding old versions keep only what live callers name (TOTK_CALL_BIND_MB, default 256: over it new
 *      calls are not bound and run as before). The submit walk pins and walks a bound version that is not the handle's current one,
 *      and the translation plays it (boot_nvn_cb_target).
 *   2  watch: bound the same way, but the translation plays today's live version; every call whose bound and live versions differ is
 *      counted and logged (host_nvn_play.h boot_nvn_cb_watch: present, QPC, caller, callee, and for the first 4 per second the
 *      vertex buffers, strides and the caller's next draw of both versions).
 *   0  as before: nothing bound; bindings made in 1 / 2 are released when switching to 0, and a record's bound versions are ignored.
 * A bound version rec-evict's class 1 released (its command memory written over: invalid in NVN too) is never played: its buffer goes
 * at once (the slot stays as a marker until its last binding goes) and the call falls back to the handle, counted ("released by
 * class 1"); a version no longer there for another reason (bindings dropped by a switch to 0) falls back the same way ("gone").
 * Bookkeeping, all under the store lock: cb_refs (bindings), cb_flags (BOOT_NVN_CB_VER: counted as a re-record version, _HELD:
 * counted as held by callers, _INVALID: released by class 1, _STRIPPED: its buffer is gone), cb_bytes (bytes counted while held). */
#define BOOT_NVN_CB_ARG 0x42u   /* record block arg: the versions bound to block 0's handles (arg 2), one u64 each (0: none) */
static volatile LONG boot_nvn_cb_mode_v = -1;   /* -1: not read yet */
static struct {
    uint64_t held_n, held_bytes, held_peak_n, held_peak_bytes, tombs, refs, unref_bad, reap_lost, inplace;   /* store lock (the log reads them without it) */
    uint32_t *reap, reap_n, reap_cap; int reaping;                                                  /* store lock */
    volatile LONG64 calls, binds, unbound_rec, budget_skip, bind_lost, ex_same, ex_diverged, ex_unbound, ex_gone, ex_invalid, ex_inplace, ex_off;
    volatile LONG64 switches, released_by_switch, watch_events, watch_logged, watch_diffs, watch_over;
    volatile LONG logged_fb;
} boot_nvn_cb;
static inline int boot_nvn_cb_mode(void) {
    LONG v = boot_nvn_cb_mode_v;
    if (v < 0) {
        char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_CALL_BIND_AT_RECORD", s, sizeof s);
        InterlockedCompareExchange(&boot_nvn_cb_mode_v, k && k < sizeof s && s[0] >= '0' && s[0] <= '2' ? s[0] - '0' : 1, -1);
        v = boot_nvn_cb_mode_v;
    }
    return (int)v;
}
static inline uint64_t boot_nvn_cb_budget(void) {   /* bytes of versions held by callers beyond which new calls are not bound */
    static int64_t v = -1;
    if (v < 0) {
        char s[16]; DWORD k = GetEnvironmentVariableA("TOTK_CALL_BIND_MB", s, sizeof s); uint64_t mb = 256;
        if (k && k < sizeof s) { mb = 0; for (DWORD i = 0; i < k && s[i] >= '0' && s[i] <= '9' && mb < 65536; ++i) mb = mb * 10 + (uint64_t)(s[i] - '0'); }
        v = (int64_t)((mb > 65536 ? 65536 : mb) << 20);
    }
    return (uint64_t)v;
}
static inline void boot_nvn_cb_hold_locked(BootNvnSlot *t) {   /* dead, kept by bindings only */
    if (t->cb_flags & BOOT_NVN_CB_HELD) return;
    t->cb_flags |= BOOT_NVN_CB_HELD; t->cb_bytes = t->cap;
    ++boot_nvn_cb.held_n; boot_nvn_cb.held_bytes += t->cap;
    if (boot_nvn_cb.held_n > boot_nvn_cb.held_peak_n) boot_nvn_cb.held_peak_n = boot_nvn_cb.held_n;
    if (boot_nvn_cb.held_bytes > boot_nvn_cb.held_peak_bytes) boot_nvn_cb.held_peak_bytes = boot_nvn_cb.held_bytes;
}
static inline void boot_nvn_cb_unhold_locked(BootNvnSlot *t) {
    if (!(t->cb_flags & BOOT_NVN_CB_HELD)) return;
    t->cb_flags &= (uint8_t)~BOOT_NVN_CB_HELD; --boot_nvn_cb.held_n; boot_nvn_cb.held_bytes -= t->cb_bytes; t->cb_bytes = 0;
}
/* One binding to version b goes; a dead version without bindings or pins is queued for boot_nvn_cb_drain_locked. */
static inline void boot_nvn_cb_unref_locked(BootNvnShadowStore *s, uint64_t b) {
    const uint32_t j = boot_nvn_slot_of(b); BootNvnSlot *u = j < s->count ? boot_nvn_slot_at(s, j) : NULL;
    if (!u || u->gen != (uint32_t)b || !u->cb_refs) { ++boot_nvn_cb.unref_bad; return; }   /* must stay 0 (the harness checks) */
    --u->cb_refs; --boot_nvn_cb.refs;
    if (u->cb_refs || !u->dead || u->pins) return;
    if (boot_nvn_cb.reap_n == boot_nvn_cb.reap_cap) {
        uint32_t cap = boot_nvn_cb.reap_cap ? boot_nvn_cb.reap_cap * 2 : 256, *x = (uint32_t *)realloc(boot_nvn_cb.reap, (size_t)cap * sizeof *x);
        if (!x) { ++boot_nvn_cb.reap_lost; return; }   /* out of host memory: that version stays (dead) */
        boot_nvn_cb.reap = x; boot_nvn_cb.reap_cap = cap;
    }
    boot_nvn_cb.reap[boot_nvn_cb.reap_n++] = j;
}
static inline void boot_nvn_cb_drop_bindings_locked(BootNvnShadowStore *s, BootNvnSlot *t) {   /* t's slot lock held too: its CallCommands' bindings go */
    for (uint32_t k = 0; k < t->dref_n; ++k)   /* storelock-work: with TOTK_STORE_LOCK_V2 queued, dropped after the section (boot_nvn_sl_flush) */
        if (t->dref[k].count == UINT32_MAX && t->dref[k].bound) { if (!boot_nvn_sl_defer(t->dref[k].bound)) boot_nvn_cb_unref_locked(s, t->dref[k].bound); t->dref[k].bound = 0; }
}
static inline int boot_nvn_slot_free_locked(BootNvnShadowStore *s, uint32_t i);
static inline void boot_nvn_cb_drain_locked(BootNvnShadowStore *s) {   /* the versions whose last binding went are freed (their own bindings in turn) */
    if (boot_nvn_cb.reaping) return;
    boot_nvn_cb.reaping = 1;
    while (boot_nvn_cb.reap_n) {
        const uint32_t j = boot_nvn_cb.reap[--boot_nvn_cb.reap_n]; BootNvnSlot *u = boot_nvn_slot_at(s, j);
        if (u->dead && !u->pins && !u->cb_refs) boot_nvn_slot_free_locked(s, j);
    }
    boot_nvn_cb.reaping = 0;
}
/* Class 1 released a version callers still name: it is never played again, so its buffer and index go now; the slot stays (a marker
 * the callers' bindings name) until the last binding goes. */
static inline void boot_nvn_cb_strip_locked(BootNvnShadowStore *s, BootNvnSlot *t) {
    boot_nvn_cb_unhold_locked(t);
    AcquireSRWLockExclusive(&t->lock);
    boot_nvn_cb_drop_bindings_locked(s, t);
    InterlockedExchangeAdd64(&s->bytes, -(LONG64)t->cap); if (!boot_nvn_rbuf_put(t->data, t->cap)) free(t->data);
    t->data = NULL; t->cap = 0; t->len = 0; t->closed = 0;
    free(t->dref); t->dref = NULL; t->dref_cap = 0; t->dref_n = 0;
    ReleaseSRWLockExclusive(&t->lock);
    t->cb_flags |= BOOT_NVN_CB_STRIPPED; ++boot_nvn_cb.tombs;
}
/* Store lock held. 1: a submission still reads it (only marked dead; freed by its unpin), 0: released, or (call-bind) kept dead while
 * a live caller's CallCommands is bound to it (freed at its last binding; a version class 1 released keeps only its marker). */
static inline int boot_nvn_slot_free_locked(BootNvnShadowStore *s, uint32_t i) {
    BootNvnSlot *t = boot_nvn_slot_at(s, i);
    boot_nvn_ev_forget_locked(t, i);   /* rec-evict: every release and every marking dead passes here, after its handle is gone */
    if (t->pins) {   /* a submission is still reading it: freed by its unpin */
        if (!t->dead) boot_nvn_ver_add(t);
        t->dead = 1; if (t->cb_refs) boot_nvn_cb_hold_locked(t);
        return 1;
    }
    if (t->cb_flags & BOOT_NVN_CB_VER) {   /* rerecord-work: a version goes (its last unpin) */
        __atomic_store_n(&boot_nvn_ver.n, boot_nvn_ver.n - 1, __ATOMIC_RELAXED); __atomic_store_n(&boot_nvn_ver.bytes, boot_nvn_ver.bytes - t->ver_bytes, __ATOMIC_RELAXED); t->ver_bytes = 0;
        t->cb_flags &= (uint8_t)~BOOT_NVN_CB_VER;
    }
    if (t->cb_refs) {   /* call-bind: bound by a live caller's CallCommands: kept (dead) until its last binding goes */
        t->dead = 1; boot_nvn_cb_hold_locked(t);
        if ((t->cb_flags & (BOOT_NVN_CB_INVALID | BOOT_NVN_CB_STRIPPED)) == BOOT_NVN_CB_INVALID) { boot_nvn_cb_strip_locked(s, t); boot_nvn_cb_drain_locked(s); }
        return 0;
    }
    boot_nvn_cb_unhold_locked(t);
    if (t->cb_flags & BOOT_NVN_CB_STRIPPED) --boot_nvn_cb.tombs;
    t->cb_flags = 0;
    t->dead = 0;
    AcquireSRWLockExclusive(&t->lock);
    boot_nvn_cb_drop_bindings_locked(s, t);   /* call-bind: its own CallCommands' bindings go with it */
    if (boot_nvn_reclock_on() && t->data && t->cap <= BOOT_NVN_SLOT_KEEP_CAP && s->kept + t->cap <= BOOT_NVN_SLOT_KEEP_TOTAL) s->kept += t->cap;   /* for the next recording here */
    else { InterlockedExchangeAdd64(&s->bytes, -(LONG64)t->cap); if (!boot_nvn_rbuf_put(t->data, t->cap)) free(t->data); t->data = NULL; t->cap = 0; }   /* recfast: pooled */
    t->len = 0; t->token = 0; t->closed = 0;
    if (t->dref && boot_nvn_rbuf_limit() && t->dref_cap <= BOOT_NVN_DREF_KEEP_CAP && s->dref_kept + (uint64_t)t->dref_cap * sizeof *t->dref <= BOOT_NVN_DREF_KEEP_TOTAL)
        s->dref_kept += (uint64_t)t->dref_cap * sizeof *t->dref;   /* recfast: kept for the next recording here */
    else { free(t->dref); t->dref = NULL; t->dref_cap = 0; }   /* flashfix */
    t->dref_n = 0;
    ++t->gen; if (!t->gen) t->gen = 1;
    t->next_free = s->free_head;
    ReleaseSRWLockExclusive(&t->lock);
    s->free_head = i; --s->live;
    boot_nvn_cb_drain_locked(s);   /* call-bind: versions only it still bound */
    return 0;
}
/* 1 when a submission was reading the recording (batch-a: re-recorded under a submit call), 0 otherwise. */
static inline int boot_nvn_slot_free(uint64_t shadow) {
    BootNvnShadowStore *s = boot_nvn_shadow(); uint32_t i = boot_nvn_slot_of(shadow); int pinned = 0;
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_FREE, 0);
    if (i < s->count && boot_nvn_slot_at(s, i)->next_free == UINT32_MAX && !boot_nvn_slot_at(s, i)->dead) pinned = boot_nvn_slot_free_locked(s, i);
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_FREE, 0, sl);   /* (v2: then its bindings) */
    return pinned;
}
/* storelock-work (TOTK_STORE_LOCK_V2): the binding drops this thread's releases queued (boot_nvn_cb_drop_bindings_locked), under the
 * shared lock: one atomic decrement each (a binding keeps its version, so its generation is stable; another thread may drop or add
 * bindings of the same version at the same time). A version left dead, unbound and unpinned is freed under the exclusive lock if it is
 * still that generation and still so (t2's unpin or another thread's drop may have freed it meanwhile; a dead version is pinned again
 * only by t2's boot_nvn_cb_pin, whose unpin then frees it), as boot_nvn_cb_drain_locked frees it; those frees queue their own
 * bindings: repeated until none is left. Called by boot_nvn_sl_unlock with no store, slot, map or ev lock held (the walk lock may be). */
static inline void boot_nvn_sl_flush(void) {
    BootNvnShadowStore *s = boot_nvn_shadow();
    boot_nvn_sld.flushing = 1;
    while (boot_nvn_sld.n) {
        uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_DROP, 1), dropped = 0;
        for (uint32_t k = 0; k < boot_nvn_sld.n; ++k) {
            const uint64_t b = boot_nvn_sld.b[k]; const uint32_t j = boot_nvn_slot_of(b);
            BootNvnSlot *u = j < s->count ? boot_nvn_slot_at(s, j) : NULL;
            uint32_t r = u && u->gen == (uint32_t)b ? __atomic_load_n(&u->cb_refs, __ATOMIC_RELAXED) : 0;
            while (r && !__atomic_compare_exchange_n(&u->cb_refs, &r, r - 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            if (!r) { __atomic_add_fetch(&boot_nvn_cb.unref_bad, 1, __ATOMIC_RELAXED); continue; }   /* must stay 0, as in boot_nvn_cb_unref_locked */
            ++dropped;
            if (r > 1 || !u->dead || __atomic_load_n(&u->pins, __ATOMIC_RELAXED)) continue;
            if (boot_nvn_sld.rn == boot_nvn_sld.rcap) {
                uint32_t cap = boot_nvn_sld.rcap ? boot_nvn_sld.rcap * 2 : 64; uint64_t *x = (uint64_t *)realloc(boot_nvn_sld.r, (size_t)cap * sizeof *x);
                if (!x) { __atomic_add_fetch(&boot_nvn_cb.reap_lost, 1, __ATOMIC_RELAXED); continue; }   /* out of host memory: that version stays (dead) */
                boot_nvn_sld.r = x; boot_nvn_sld.rcap = cap;
            }
            boot_nvn_sld.r[boot_nvn_sld.rn++] = b;
        }
        __atomic_sub_fetch(&boot_nvn_cb.refs, dropped, __ATOMIC_RELAXED);
        InterlockedExchangeAdd64(&boot_nvn_sl_stats.deferred, (LONG64)boot_nvn_sld.n);
        boot_nvn_sld.n = 0;
        boot_nvn_sl_unlock(s, BOOT_NVN_SL_DROP, 1, sl);
        if (!boot_nvn_sld.rn) break;
        sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_DROP_FREE, 0);
        for (uint32_t k = 0; k < boot_nvn_sld.rn; ++k) {
            const uint64_t b = boot_nvn_sld.r[k]; const uint32_t j = boot_nvn_slot_of(b); BootNvnSlot *u = boot_nvn_slot_at(s, j);
            if (u->gen == (uint32_t)b && u->dead && !u->pins && !u->cb_refs) { boot_nvn_slot_free_locked(s, j); InterlockedIncrement64(&boot_nvn_sl_stats.drop_frees); }
        }
        boot_nvn_sld.rn = 0;
        boot_nvn_sl_unlock(s, BOOT_NVN_SL_DROP_FREE, 0, sl);   /* (flushing: the loop takes the bindings these frees queued) */
    }
    boot_nvn_sld.flushing = 0;
}
/* recfast, slot lock held, pool on: t's buffer replaced by one of capacity cap (a class capacity) or of the next class,
 * holding t's t->len record bytes, as realloc would; the old buffer goes to the pool (or is freed). 0: out of host
 * memory (t unchanged). */
static inline int boot_nvn_slot_regrow(BootNvnShadowStore *s, BootNvnSlot *t, uint32_t cap) {
    uint32_t got = cap; uint8_t *d = boot_nvn_rbuf_get(&got);
    if (d) ++boot_nvn_rl()->n[BOOT_NVN_RL_POOL_HITS];
    else { d = (uint8_t *)malloc((size_t)cap); if (!d) return 0; got = cap; ++boot_nvn_rl()->n[BOOT_NVN_RL_POOL_NEW]; }
    if (t->len) memcpy(d, t->data, t->len);
    if (t->data && !boot_nvn_rbuf_put(t->data, t->cap)) free(t->data);
    InterlockedExchangeAdd64(&s->bytes, (LONG64)got - (LONG64)t->cap);
    t->data = d; t->cap = got;
    return 1;
}
/* recfast: a buffer for `want` bytes for the new recording in slot `shadow` (generation gen, nothing appended yet). */
static inline void boot_nvn_slot_reserve(uint64_t shadow, uint32_t gen, uint64_t want) {
    BootNvnShadowStore *s = boot_nvn_shadow(); BootNvnSlot *t = boot_nvn_slot_peek(s, boot_nvn_slot_of(shadow));
    if (!t || !boot_nvn_rbuf_limit() || want > 0x80000000u) return;
    AcquireSRWLockExclusive(&t->lock);
    if (t->next_free == UINT32_MAX && t->gen == gen && !t->closed && !t->len && t->cap < want) {
        uint64_t cap = t->cap ? t->cap : BOOT_NVN_SLOT_FIRST_CAP; while (cap < want) cap *= 2;
        if (boot_nvn_slot_regrow(s, t, (uint32_t)cap)) ++boot_nvn_rl()->n[BOOT_NVN_RL_HINTED];   /* failure: the appends grow as before */
    }
    ReleaseSRWLockExclusive(&t->lock);
}
static inline uint32_t boot_nvn_slot_len(uint64_t shadow) {   /* recfast: bytes recorded in a live slot (0: none) */
    BootNvnSlot *t = boot_nvn_slot_peek(boot_nvn_shadow(), boot_nvn_slot_of(shadow)); uint32_t n = 0;
    if (!t) return 0;
    AcquireSRWLockShared(&t->lock); if (t->next_free == UINT32_MAX) n = t->len; ReleaseSRWLockShared(&t->lock);
    return n;
}
/* recfast, EndRecording (the recording is closed and not yet reachable through a handle): its length, and its buffer moved
 * to the class of that length when the buffer is at least 4 times larger (a hint bigger than this recording). */
static inline uint32_t boot_nvn_slot_fit(uint64_t shadow) {
    BootNvnShadowStore *s = boot_nvn_shadow(); BootNvnSlot *t = boot_nvn_slot_peek(s, boot_nvn_slot_of(shadow)); uint32_t n = 0;
    if (!t) return 0;
    AcquireSRWLockExclusive(&t->lock);
    if (t->next_free == UINT32_MAX) {
        n = t->len;
        if (t->closed && t->cap > BOOT_NVN_SLOT_KEEP_CAP && boot_nvn_rbuf_limit()) {
            uint64_t need = BOOT_NVN_SLOT_FIRST_CAP; while (need < n) need *= 2;
            if (need * 4 <= t->cap && boot_nvn_slot_regrow(s, t, (uint32_t)need)) ++boot_nvn_rl()->n[BOOT_NVN_RL_TRIMS];   /* failure: it keeps the buffer */
        }
    }
    ReleaseSRWLockExclusive(&t->lock);
    return n;
}
/* Record storage: one record (its parts back to back, each padded with zeros to 4 bytes, the step
 * boot_nvn_record_read takes) appended to the open recording in slot `shadow`, under that slot's lock only.
 * gen 0: any live slot (the map said so, as before reclock); otherwise only that generation and not yet closed
 * (the recorder's cache, boot_nvn_rec). close: the END record (nothing is appended after it through the cache).
 * dref: the flashfix index entries of this record (boot_nvn_dsnap_dref), added in the same step.
 * Returns 1 appended (the generation in *gen_out), 0 out of host memory (logged), -1 not that open recording. */
typedef struct BootNvnPart { const void *p; uint32_t n; } BootNvnPart;
typedef struct BootNvnDrefAdd { uint32_t count, n; const uint64_t *ptr, *bound; uint32_t *lost; } BootNvnDrefAdd;   /* n entries {record offset, count, ptr[k]
                                                                                                                * (, bound[k]: call-bind)}; *lost set when they are not stored */
static inline void boot_nvn_dsnap_lost(void);
static inline void boot_nvn_slot_dref_locked(BootNvnSlot *t, uint32_t off, const BootNvnDrefAdd *d) {   /* slot lock held */
    if (t->dref_n + (uint64_t)d->n > t->dref_cap) {
        uint64_t cap = t->dref_cap ? t->dref_cap : 8; while (cap < t->dref_n + (uint64_t)d->n) cap *= 2;
        BootNvnDref *x = cap <= 0x10000000u ? (BootNvnDref *)realloc(t->dref, (size_t)cap * sizeof *x) : NULL;
        if (x) { t->dref = x; t->dref_cap = (uint32_t)cap; }
    }
    if (t->dref_n + (uint64_t)d->n <= t->dref_cap)
        for (uint32_t k = 0; k < d->n; ++k) {
            t->dref[t->dref_n].off = off; t->dref[t->dref_n].count = d->count; t->dref[t->dref_n].ptr = d->ptr[k]; t->dref[t->dref_n].bound = d->bound ? d->bound[k] : 0; ++t->dref_n;
        }
    else { boot_nvn_dsnap_lost(); if (d->lost) *d->lost = 1; }
}
static inline int boot_nvn_slot_append(uint64_t shadow, uint32_t gen, const BootNvnPart *part, unsigned parts, int close, uint32_t *gen_out, const BootNvnDrefAdd *dref) {
    BootNvnShadowStore *s = boot_nvn_shadow(); BootNvnSlot *t = boot_nvn_slot_peek(s, boot_nvn_slot_of(shadow));
    if (!t) return -1;
    uint64_t need = 0; for (unsigned k = 0; k < parts; ++k) need += (part[k].n + 3u) & ~3u;
    int ok = -1, serial = !boot_nvn_reclock_on(); uint64_t off = 0;
    if (serial) boot_nvn_rl_lock(&s->lock, BOOT_NVN_RL_STORE);   /* TOTK_RECLOCK=0: the store lock as well (it comes first) */
    boot_nvn_rl_lock(&t->lock, BOOT_NVN_RL_SLOT);
    if (t->next_free == UINT32_MAX && (!gen || (t->gen == gen && !t->closed))) {
        off = t->len; ok = 0;
        if (off + need < 0x80000000u) {
            ok = 1;
            if (off + need > t->cap) {   /* geometric growth */
                uint64_t cap = t->cap ? t->cap : BOOT_NVN_SLOT_FIRST_CAP; while (cap < off + need) cap *= 2;
                if (boot_nvn_rbuf_limit()) { if (boot_nvn_slot_regrow(s, t, (uint32_t)cap)) ++boot_nvn_rl()->n[BOOT_NVN_RL_GROWS]; else ok = 0; }   /* recfast */
                else {
                    uint8_t *d = (uint8_t *)realloc(t->data, (size_t)cap);
                    if (d) { InterlockedExchangeAdd64(&s->bytes, (LONG64)(cap - t->cap)); t->data = d; t->cap = (uint32_t)cap; ++boot_nvn_rl()->n[BOOT_NVN_RL_GROWS]; }
                    else ok = 0;
                }
            }
            if (ok) {
                uint8_t *w = t->data + off;
                for (unsigned k = 0; k < parts; ++k) {
                    uint32_t n = part[k].n, pad = ((n + 3u) & ~3u) - n;
                    if (n) memcpy(w, part[k].p, n);
                    if (pad) { memset(w + n, 0, pad); ++boot_nvn_rl()->n[BOOT_NVN_RL_UNALIGNED]; }
                    w += n + pad;
                }
                t->len = (uint32_t)(off + need);
                if (close) t->closed = 1;
                if (gen_out) *gen_out = t->gen;
                if (dref && dref->n) boot_nvn_slot_dref_locked(t, (uint32_t)off, dref);   /* flashfix */
            }
        }
    }
    ReleaseSRWLockExclusive(&t->lock);
    if (serial) ReleaseSRWLockExclusive(&s->lock);
    if (!ok) boot_nvn_log("command record storage", "shadow store", shadow + off);
    return ok;
}
/* Reading a recording: lock-free from a slot this thread has pinned (boot_nvn_pin_find: finished, cannot be
 * freed, moved or reused until boot_nvn_unpin_all), under the locks otherwise (and always with TOTK_RECLOCK=0). */
typedef struct BootNvnPin { const uint8_t *data; uint32_t slot, len, hpos; } BootNvnPin;
static inline const BootNvnPin *boot_nvn_pin_find(uint32_t i);
static inline int boot_nvn_cmd_fetch(uint64_t at, void *dst, size_t n) {
    uint32_t i = boot_nvn_slot_of(at); uint64_t off = at & 0xffffffffu; int ok = 0;
    if (boot_nvn_reclock_on()) {
        const BootNvnPin *p = boot_nvn_pin_find(i);
        if (p) { if (off + n > p->len) return 0; memcpy(dst, p->data + off, n); return 1; }
    }
    BootNvnShadowStore *s = boot_nvn_shadow(); ++boot_nvn_rl()->n[BOOT_NVN_RL_LOCKED_READS];
    boot_nvn_rl_lock_shared(&s->lock, BOOT_NVN_RL_STORE);
    if (i < s->count) {
        BootNvnSlot *t = boot_nvn_slot_at(s, i);
        AcquireSRWLockShared(&t->lock);
        if (t->next_free == UINT32_MAX && off + n <= t->len) { memcpy(dst, t->data + off, n); ok = 1; }
        ReleaseSRWLockShared(&t->lock);
    }
    ReleaseSRWLockShared(&s->lock);
    return ok;
}
/* flashfix (play-work/flash-work/NOTES.md): deferred texture binds read at the submit call, as the driver does.
 * nvnCommandBufferBindTexturesDeferred records a pointer; the driver reads the handles behind it when
 * nvnQueueSubmitCommands is called (host_nvn_exec.h boot_nvn_exec_bind_deferred). We play the records during a
 * 30-40 ms translation, and with frame overlap the next frame's job threads run game code meanwhile and rewrite those
 * per-material arrays for their own frame, while the matching TexturePoolRegisterTexture waits for the renderer lock:
 * draws took TIC slots whose registration was still the old one (single frames dark or black, the menu mural missing,
 * magenta ASTC-error frames). Each recording keeps an index of its deferred binds (record offset, pointer, count) and
 * of the command buffers it calls (CallCommands handles), made when they are recorded; at the top of
 * nvnQueueSubmitCommands every handle array the submission reaches is copied into a per-submission table keyed by the
 * record's shadow address (one guest read per bind, no record walk), and the bind reads the copy.
 * TOTK_DEFER_SNAPSHOT=0: the handles are read at translation as before. TOTK_DEFER_LATE_CHECK=1 (diagnostic): the copy
 * is taken also with the snapshot off, and each deferred bind compares it with guest memory at translation; changed
 * handles are counted (flicker.log per-present counters, the periodic draw-path.log line) and the first 32 logged.
 * Copies (TOTK_COPY_OBJ_CAPTURE=0 disables): CopyTextureToBuffer / CopyBufferToTexture keep the texture object and the
 * view as recorded (boot_nvn_record_build), as SetRenderTargets does. */
static inline int boot_nvn_defer_snapshot_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_DEFER_SNAPSHOT", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0'); }
    return v;
}
static inline int boot_nvn_defer_late_check_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_DEFER_LATE_CHECK", s, sizeof s); v = k && k < sizeof s && s[0] == '1'; }
    return v;
}
/* async-xlate (play-work/async-xlate-work: DESIGN.md, PRECHECKS.md, stage1/README.md, stage2/README.md): TOTK_ASYNC_XLATE, read once;
 * unset/0 = today's code paths. 1 = stage 1 "join" (host_nvn_async.h: t2 captures each submission, the translation thread X translates it,
 * t2 waits for X inside the submit); 2 = stage 2 "async" (the submit returns after the capture, virtual fences, the frame fence stays full);
 * 3 (early frame fence) is not built yet and runs as 2. The launcher (no TOTK_DRAW_PATH) reads 0.
 * With it on the submit freeze is forced on (the walk's freeze table, pins and copies are what X translates from) and recordings keep
 * range summaries (boot_nvn_axr_of). */
static inline int boot_nvn_ax_mode(void) {
#ifdef TOTK_DRAW_PATH
    static int v = -1;
    if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_ASYNC_XLATE", s, sizeof s); v = k && k < sizeof s && s[0] >= '1' && s[0] <= '3' ? s[0] - '0' : 0; }
    return v;
#else
    return 0;
#endif
}
/* The host thread a submit call's translation acts for: the translation thread X sets the submitting thread's id while it translates
 * that call's submission, so the checks keyed by thread (freeze, call-bind, NVN_PAR) see the call's thread; 0 on every other thread. */
static _Thread_local DWORD boot_nvn_act_tid;
static inline DWORD boot_nvn_call_tid(void) { const DWORD t = boot_nvn_act_tid; return t ? t : GetCurrentThreadId(); }
/* batch-a (play-work/batch-a): the submit freeze. play-87's flicker.log has the cause of the remaining dark frames in its
 * own counters: in every dip present with deferred binds, a large share of the binds the translation executed were "not
 * in copy" (7, 34, 50, 82, 224 of 287; 0 in the frames around them). A bind is missing from the copy only when the
 * translation walks a recording the submit call did not reach, and every recording is reached through a driver handle:
 * the translation resolved handles (the submitted ones and those of CallCommands) through the live handle map, and
 * boot_nvn_handle_pin deliberately takes the newest recording of a handle ("replaced meanwhile: take the newer one").
 * With frame overlap other threads keep recording during the 30-40 ms translation, and a handle the game records again
 * (EndRecording returns the same command memory address) replaced the submitted recording under the translation: the
 * frame was drawn from another frame's commands (its uniform buffers, deferred binds and TIC slots not yet registered:
 * dark, black or near-empty frames, garbled objects). On the Switch the GPU fetches the submitted command memory right
 * after the call, before the game can rewrite it. TOTK_SUBMIT_FREEZE (on unless =0; default follows TOTK_OVERLAP): the
 * walk at the call (boot_nvn_dsnap_take, which already pins every recording the submission reaches) also keeps each
 * handle's resolution (handle -> pinned shadow, first resolution wins) and the submitted handle values, and the
 * translation and its read-ahead resolve handles from that table (boot_nvn_freeze_resolve). A handle the call could not
 * resolve (not recorded yet) or did not reach still takes the live map, counted. The pins keep a replaced recording
 * readable until the call ends (the old slot is only marked dead). Costs nothing when the walk runs anyway. */
static inline int boot_nvn_freeze_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_SUBMIT_FREEZE", s, sizeof s); v = k && k < sizeof s ? s[0] != '0' : boot_mt_overlap_on();
                                    if (boot_nvn_ax_mode()) v = 1; }   /* async-xlate: forced on (PRECHECKS 1 #12: X translates from the walk's table) */
    return v;
}
/* batch-a: TOTK_RERECORD_WAIT (on unless =0; default follows TOTK_OVERLAP). A thread whose EndRecording replaces a
 * recording that the submit call running on another thread is reading (the old slot is pinned) waits, outside guest
 * code and without any lock, until that call returns (at most BOOT_NVN_RERECORD_CAP_MS): on the Switch that call takes
 * microseconds, so the game's next recording into that command memory comes after it; here the thread would go on
 * filling the memory the frame being translated still reads. Only such collisions wait (counted, the first 16 logged). */
#define BOOT_NVN_RERECORD_CAP_MS 500u
static inline int boot_nvn_rerecord_wait_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_RERECORD_WAIT", s, sizeof s); v = k && k < sizeof s ? s[0] != '0' : boot_mt_overlap_on(); }
    return v;
}
/* rerecord-work (play-work/rerecord-work: ours.md, nxport.md, test/): TOTK_RERECORD_VERSION (on unless =0; =0: the wait
 * above exactly as before). The rationale above does not hold here: a re-record never writes the recording a submission
 * reads. BeginRecording always takes a fresh slot, EndRecording's return points the handle at it, and the older slot, which
 * the call pinned, is only marked dead: closed (nothing is appended to it again), on no free list, never regrown, moved or
 * freed until the call's boot_nvn_unpin_all. It stays an immutable version of the recording as the call found it: the model
 * of nxport (MIT; crates/engine/src/nvn/api.rs: an Arc<RecordedCommandList> per EndRecording, walk_command_lists clones the
 * Arcs a submit reaches) and of SMW's shared_ptr lists, neither of which waits. Which version the translation reads is
 * decided by the handle -> recording resolution, and the submit freeze (TOTK_SUBMIT_FREEZE) keeps the call's own resolution
 * for every handle its walk reached. So the wait was timing only: it held t0/t47/t48 (their per-thread command buffers are
 * re-recorded 2-4 ms into every call) for the rest of the 25-40 ms call, 17.8 ms per frame in play-96 (7091 waits, 115 s
 * of 317 s), serialising the game with the translation again.
 * With the switch on, a thread whose re-record replaced a recording the running call reads keeps the old one as a version
 * and goes on when the call vouches for it (boot_nvn_rv_decide), i.e. all of:
 *   1. the call's walk has ended and covered the whole submission: freeze on, every submitted handle read (at most 4096),
 *      none missing from the map, no freeze entry or recording index entry lost (boot_nvn_dsnap_take, boot_nvn_rv_publish);
 *   2. the old recording was reached by this call's walk: its slot carries the call's snapshot stamp (set under the slot lock
 *      by the walk), so its handle is in the freeze table and the translation resolves it to that slot;
 *   3. the translation has not met a handle the walk missed (boot_nvn_freeze_resolve: such a handle takes the live map, so
 *      from then on the call's re-records wait as before);
 *   4. the versions held (all dead, pinned recordings) stay within TOTK_RERECORD_VERSION_MB (default 64; 0: every re-record
 *      waits): over it the thread waits as before, which also stops it from adding more.
 * Otherwise it waits as before (counted by reason). Why a submission still reads only what it found at its call (play-88):
 * the translation reads records (a) at a shadow address of a slot this thread pinned (boot_nvn_pin_find: its bytes cannot
 * change until boot_nvn_unpin_all), or (b) through a handle, which the freeze resolves to the slot the walk pinned (1, 2), or
 * the live map for a handle the walk missed, after which nothing is waived (3); the NVN_PAR helpers decode frozen handles
 * only. A re-record only adds a slot and moves the map entry: it changes neither (a) nor the freeze table.
 * Memory: a version lives until the end of the call that pinned it, so the versions are part of that call's reached set
 * ("reached per call" below) and, past the budget, at most one more per recording thread before it waits.
 * TOTK_SNAP_ATOMIC (default: as TOTK_RERECORD_VERSION; =0 off): the walk (boot_nvn_dsnap_take, about 0.5 ms) holds
 * boot_nvn_walk_lock exclusively and EndRecording's return holds it shared around its map change and release
 * (boot_nvn_handle_install), so the walk sees the map at one instant, as nxport's walk under the COMMAND_LISTS lock does: a
 * recording ended during the walk is installed after it, so the call reads the version that was current when the walk began
 * (not the next frame's when the walk had not reached it yet), and that re-record is then covered (no wait for the call).
 * Every 300 presents (boot_nvn_queue_present) a "re-record versions" draw-path.log line, written with the switch off too
 * (counters only, the A/B baseline): re-records in calls, kept as versions, waits by reason and their ms, versions and bytes
 * held (now, peak), bytes each call pinned (its reached set), walk-lock waits, and alarms (a call that kept a version and
 * then met a handle its walk missed; must stay 0). */
static inline int boot_nvn_rerecord_version_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_RERECORD_VERSION", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0'); }
    return v;
}
static inline uint64_t boot_nvn_rerecord_budget(void) {   /* bytes of versions held beyond which a re-record waits again */
    static int64_t v = -1;
    if (v < 0) {
        char s[16]; DWORD k = GetEnvironmentVariableA("TOTK_RERECORD_VERSION_MB", s, sizeof s); uint64_t mb = 64;
        if (k && k < sizeof s) { mb = 0; for (DWORD i = 0; i < k && s[i] >= '0' && s[i] <= '9' && mb < 65536; ++i) mb = mb * 10 + (uint64_t)(s[i] - '0'); }
        if (mb > 65536) mb = 65536;
        v = (int64_t)(mb << 20);
    }
    return (uint64_t)v;
}
static inline int boot_nvn_snap_atomic_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_SNAP_ATOMIC", s, sizeof s); v = k && k < sizeof s ? s[0] != '0' : boot_nvn_rerecord_version_on(); }
    return v;
}
static SRWLOCK boot_nvn_walk_lock = SRWLOCK_INIT;   /* lock order: before the store, slot and map locks */
/* A re-record in a call: kept as a version (no wait), or why it still waits. */
enum { BOOT_NVN_RV_KEPT, BOOT_NVN_RV_OFF, BOOT_NVN_RV_UNCOVERED, BOOT_NVN_RV_UNREACHED, BOOT_NVN_RV_MISSED, BOOT_NVN_RV_BUDGET, BOOT_NVN_RV_N };
static struct {
    uint32_t cover;          /* under boot_nvn_sub.lock: the running call's snapshot stamp once its walk vouches for it, else 0 */
    volatile LONG missed;    /* the running call's translation met a handle its walk had not reached */
    uint64_t unknown0;       /* t2: boot_nvn_dsnap.fz_unknown when the walk ended */
    volatile LONG64 n[BOOT_NVN_RV_N], kept_call, alarms, walk_waits, walk_wait_qpc;
} boot_nvn_rv;
static inline void boot_nvn_rv_publish(void);   /* at the end of the walk, before its lock goes (boot_nvn_dsnap_take) */
/* batch-a: TOTK_FRAME_TRACE=1 (diagnostic, off by default): frame-trace.log (cwd), one block per nvnQueueSubmitCommands
 * (boot_nvn_ftrace_write): the submitted handles (recording thread, bytes, EndRecording time against the call), what the
 * call's walk reached, how the translation resolved handles (frozen, moved since the call, not reached), the deferred
 * binds, draws and clears of the call, recordings re-recorded while it ran (by whom, when), the submitting thread's guest
 * waits since its previous call (svc, object, timeout, result, time) and every thread's timed-out waits (boot_mt_ft,
 * boot_mt_late.h). */
static inline int boot_nvn_ftrace_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_FRAME_TRACE", s, sizeof s); v = k && k < sizeof s && s[0] == '1'; }
    return v;
}
static inline int boot_nvn_dsnap_wanted(void) { return boot_nvn_defer_snapshot_on() || boot_nvn_defer_late_check_on() || boot_nvn_freeze_on(); }
static inline int boot_nvn_copy_capture_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_COPY_OBJ_CAPTURE", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0'); }
    return v;
}
/* The guest thread a context runs (a core context's loaded thread with host threads). */
static inline unsigned boot_nvn_guest_thread(const BootContext *c) {
    if (boot_mt.active && boot_mt_core_ctx(c)) return ((const BootCore *)(const void *)c)->thread;
    return c->current_thread;
}
/* One deferred bind of the submission: handles h[at .. at+n) as read at the submit call (ok 0: unreadable then). */
typedef struct BootNvnDsnapEnt { uint64_t rec, ptr; uint32_t count, n, at, ok; } BootNvnDsnapEnt;
typedef struct BootNvnDsnapWork { uint64_t shadow; uint32_t depth; } BootNvnDsnapWork;
typedef struct BootNvnFzEnt { uint64_t handle, shadow; } BootNvnFzEnt;   /* batch-a: a driver handle as the call resolved it (shadow 0: not in the map then) */
typedef struct BootNvnDsnap {   /* async-xlate: named (was anonymous); stage 2 gives each captured submission its own table */
    int active; uint32_t stamp;                                   /* active: the table belongs to the submit call running now */
    BootNvnDsnapEnt *ent; uint32_t n, cap; uint64_t *h; uint32_t hn, hcap;
    uint32_t *hash, *hash_stamp, hash_cap;                        /* rec -> ent index, valid where hash_stamp == stamp */
    BootNvnDsnapWork *work; uint32_t wn, wcap; BootNvnDref *refs; uint32_t rcap;
    /* cumulative: snapshots taken, recordings read, binds copied, handles copied, binds unreadable at the call, index
     * entries lost (out of memory), binds executed, of them from the copy / not in it, late check handles and changes */
    uint64_t submits, slots, entries, words, unread, index_lost, binds, hits, misses, late_checked, late_changed, take_qpc;
    /* batch-a freeze: handle -> recording as resolved at the call (fz_hash valid where fz_hstamp == stamp), the call's
     * top-level handle values, the submitting host thread; fz_on: the freeze applies to this call */
    BootNvnFzEnt *fz; uint32_t fz_n, fz_cap; uint32_t *fz_hash, *fz_hstamp, fz_hcap; uint64_t *top; uint32_t top_n, top_cap; DWORD tid; int fz_on;
    int covered;   /* rerecord-work: this call's walk covered the whole submission (boot_nvn_dsnap_take) */
    /* batch-a cumulative: handles resolved at the call, of them not in the map then, translation lookups answered from
     * the freeze, of them where the live map had moved on (re-recorded since the call), handles the translation reached
     * that the call did not (live map), freeze entries lost (out of memory) */
    uint64_t fz_frozen, fz_missing, fz_reads, fz_moved, fz_unknown, fz_lost;
    uint64_t ft_latest_end; uint32_t ft_latest_thread, ft_recent;   /* frame trace: newest EndRecording among the reached recordings */
} BootNvnDsnap;
/* async-xlate stage 2: the walk table a thread works with. Every thread uses the submit call's table (boot_nvn_dsnap_main) except the
 * translation thread X while it translates a stage-2 submission: that submission took the call's table (boot_nvn_dsnap_swap at its
 * capture, so t2's next walk fills another) and X points boot_nvn_dsnap_tls at it; the counters X adds are folded back
 * (boot_nvn_dsnap_fold). One cached TLS load and a predicted branch per access. */
static BootNvnDsnap boot_nvn_dsnap_main;
static _Thread_local BootNvnDsnap *boot_nvn_dsnap_tls;
static inline BootNvnDsnap *boot_nvn_dsnap_cur(void) { BootNvnDsnap *p = boot_nvn_dsnap_tls; return p ? p : &boot_nvn_dsnap_main; }
#define boot_nvn_dsnap (*boot_nvn_dsnap_cur())
/* Exchanges the call's table (the per-call fields: active, deferred-bind copies, freeze table, top handles, thread, coverage) of the main
 * table with *s; s takes the call's stamp (hash entries are valid where their stamp equals it), the main table keeps counting stamps, so a
 * table coming back never matches a later call's stamp. The cumulative counters stay where they are. */
static inline void boot_nvn_dsnap_swap(BootNvnDsnap *s) {
    BootNvnDsnap *m = &boot_nvn_dsnap_main;
#define BOOT_NVN_DSNAP_X(f) do { __typeof__(m->f) t_ = m->f; m->f = s->f; s->f = t_; } while (0)
    BOOT_NVN_DSNAP_X(active); BOOT_NVN_DSNAP_X(ent); BOOT_NVN_DSNAP_X(n); BOOT_NVN_DSNAP_X(cap); BOOT_NVN_DSNAP_X(h); BOOT_NVN_DSNAP_X(hn); BOOT_NVN_DSNAP_X(hcap);
    BOOT_NVN_DSNAP_X(hash); BOOT_NVN_DSNAP_X(hash_stamp); BOOT_NVN_DSNAP_X(hash_cap); BOOT_NVN_DSNAP_X(fz); BOOT_NVN_DSNAP_X(fz_n); BOOT_NVN_DSNAP_X(fz_cap);
    BOOT_NVN_DSNAP_X(fz_hash); BOOT_NVN_DSNAP_X(fz_hstamp); BOOT_NVN_DSNAP_X(fz_hcap); BOOT_NVN_DSNAP_X(top); BOOT_NVN_DSNAP_X(top_n); BOOT_NVN_DSNAP_X(top_cap);
    BOOT_NVN_DSNAP_X(tid); BOOT_NVN_DSNAP_X(fz_on); BOOT_NVN_DSNAP_X(covered);
#undef BOOT_NVN_DSNAP_X
    s->stamp = m->stamp;
}
/* X after a stage-2 submission: the counters it added into the submission's table go to the main table (t2's walk adds others meanwhile). */
static inline void boot_nvn_dsnap_fold(BootNvnDsnap *s) {
    BootNvnDsnap *m = &boot_nvn_dsnap_main;
#define BOOT_NVN_DSNAP_F(f) do { if (s->f) { InterlockedExchangeAdd64((volatile LONG64 *)&m->f, (LONG64)s->f); s->f = 0; } } while (0)
    BOOT_NVN_DSNAP_F(binds); BOOT_NVN_DSNAP_F(hits); BOOT_NVN_DSNAP_F(misses); BOOT_NVN_DSNAP_F(late_checked); BOOT_NVN_DSNAP_F(late_changed);
    BOOT_NVN_DSNAP_F(fz_reads); BOOT_NVN_DSNAP_F(fz_moved); BOOT_NVN_DSNAP_F(fz_unknown); BOOT_NVN_DSNAP_F(index_lost); BOOT_NVN_DSNAP_F(unread);
#undef BOOT_NVN_DSNAP_F
}
static inline void boot_nvn_dsnap_lost(void) { InterlockedIncrement64((volatile LONG64 *)&boot_nvn_dsnap.index_lost); }
/* p grown to hold `need` elements (*cap updated); NULL when out of memory (p unchanged). */
static inline void *boot_nvn_dsnap_grow(void *p, uint32_t *cap, uint64_t need, size_t elem) {
    if (need <= *cap) return p;
    if (need > 0x10000000u) return NULL;
    uint32_t c = *cap ? *cap : 256; while (c < need) c *= 2;
    void *q = realloc(p, (size_t)c * elem); if (q) *cap = c;
    return q;
}
/* async-xlate: record-time range summaries (DESIGN 4.2, the nxport model: per-list summaries built where the list is recorded, unioned at
 * submit). With TOTK_ASYNC_XLATE each record that makes the GPU read or write guest memory, or makes the translation read an NVN object
 * word from CPU memory, adds index entries next to the flashfix ones (count BOOT_NVN_DREF_RANGE, ptr the address, bound size | kind << 56 |
 * stage/slot << 40). The submit walk copies each reached recording's index anyway and collects them (boot_nvn_dsnap_take ->
 * boot_nvn_axr), so the capture knows the submission's ranges without decoding it. Kinds: reads (uniform buffer, vertex buffer, index
 * range of a direct indexed draw, storage buffer, indirect arguments, conditional-render counters), GPU-ordered writes (UpdateUniformBuffer,
 * ClearBuffer, reports), NVN object words (the GPU-address word of a texture / sampler pool's memory pool and of each render target's pool,
 * guest CPU addresses). The index consumers that act on entries (call-bind, rec-evict, the walk) select theirs by count. */
#define BOOT_NVN_DREF_RANGE 0xfffffffeu
enum { BOOT_NVN_AXR_CB, BOOT_NVN_AXR_VB, BOOT_NVN_AXR_IB, BOOT_NVN_AXR_SSBO, BOOT_NVN_AXR_IND, BOOT_NVN_AXR_COND, BOOT_NVN_AXR_W_UBO, BOOT_NVN_AXR_W_CLEAR,
       BOOT_NVN_AXR_W_REPORT, BOOT_NVN_AXR_OBJ, BOOT_NVN_AXR_N };
#define BOOT_NVN_AXR_BOUND(size, kind, slot) (((uint64_t)(size) & UINT64_C(0xffffffffff)) | (uint64_t)(kind) << 56 | ((uint64_t)(slot) & 0xffffu) << 40)
typedef struct BootNvnAxr { uint64_t va, bound; } BootNvnAxr;
static struct { BootNvnAxr *e; uint32_t n, cap; uint64_t lost; } boot_nvn_axr;   /* the running submit call's summaries (t2: reset and filled by its walk) */
static inline void boot_nvn_axr_add(uint64_t va, uint64_t bound) {
    if (boot_nvn_axr.n == boot_nvn_axr.cap) {
        const uint32_t cap = boot_nvn_axr.cap ? boot_nvn_axr.cap * 2 : 4096;
        BootNvnAxr *e = cap < (1u << 26) ? (BootNvnAxr *)realloc(boot_nvn_axr.e, (size_t)cap * sizeof *e) : NULL;
        if (!e) { ++boot_nvn_axr.lost; return; }
        boot_nvn_axr.e = e; boot_nvn_axr.cap = cap;
    }
    boot_nvn_axr.e[boot_nvn_axr.n].va = va; boot_nvn_axr.e[boot_nvn_axr.n].bound = bound; ++boot_nvn_axr.n;
}
/* The summary entries of record r (ptr[k] the address, bound[k]), at most 10; 0 for any other record. Op indices once (release flag:
 * recording threads call this concurrently). */
static inline unsigned boot_nvn_axr_of(int op, const BootNvnRecord *r, uint64_t *ptr, uint64_t *bound) {
    enum { AXO_UBO, AXO_VBUF, AXO_SSBO, AXO_DE, AXO_DEBV, AXO_DEI, AXO_DAI, AXO_DEIND, AXO_DISPI, AXO_COND, AXO_UPD, AXO_CLR, AXO_RVAL, AXO_RCNT, AXO_TPOOL,
           AXO_SPOOL, AXO_RTS, AXO_N };
    static int ix[AXO_N]; static volatile LONG ready;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        static const char *const names[AXO_N] = {"BindUniformBuffer", "BindVertexBuffer", "BindStorageBuffer", "DrawElements", "DrawElementsBaseVertex",
            "DrawElementsInstanced", "DrawArraysIndirect", "DrawElementsIndirect", "DispatchComputeIndirect", "SetRenderEnableConditional", "UpdateUniformBuffer",
            "ClearBuffer", "ReportValue", "ReportCounter", "SetTexturePool", "SetSamplerPool", "SetRenderTargets"};
        char n[96];
        for (unsigned i = 0; i < AXO_N; ++i) { snprintf(n, sizeof n, "nvnCommandBuffer%s", names[i]); ix[i] = boot_nvn_name_index(n); }
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    const uint64_t *x = r->x; unsigned k = 0;
#define BOOT_NVN_AXR_PUT(a, size, kind, slot) do { if ((a) && (size)) { ptr[k] = (a); bound[k] = BOOT_NVN_AXR_BOUND(size, kind, slot); ++k; } } while (0)
    if (op == ix[AXO_UBO]) BOOT_NVN_AXR_PUT(x[3], x[4], BOOT_NVN_AXR_CB, x[1] << 4 | (x[2] & 15));   /* (stage, slot, address, size), as the executor */
    else if (op == ix[AXO_VBUF]) BOOT_NVN_AXR_PUT(x[2], x[3], BOOT_NVN_AXR_VB, x[1]);
    else if (op == ix[AXO_SSBO]) BOOT_NVN_AXR_PUT(x[3], x[4], BOOT_NVN_AXR_SSBO, x[1] << 4 | (x[2] & 15));
    else if (op == ix[AXO_DE] || op == ix[AXO_DEBV] || op == ix[AXO_DEI])   /* (mode, type, count, index buffer, ...) */
        BOOT_NVN_AXR_PUT(x[4], x[3] * (x[2] == 0 ? 1u : x[2] == 1 ? 2u : 4u), BOOT_NVN_AXR_IB, x[2]);
    else if (op == ix[AXO_DAI]) BOOT_NVN_AXR_PUT(x[2], 16, BOOT_NVN_AXR_IND, 0);
    else if (op == ix[AXO_DEIND]) BOOT_NVN_AXR_PUT(x[4], 20, BOOT_NVN_AXR_IND, 1);   /* its index buffer's size is not known here */
    else if (op == ix[AXO_DISPI]) BOOT_NVN_AXR_PUT(x[1], 12, BOOT_NVN_AXR_IND, 2);
    else if (op == ix[AXO_COND]) BOOT_NVN_AXR_PUT(x[2], 32, BOOT_NVN_AXR_COND, 0);
    else if (op == ix[AXO_UPD]) BOOT_NVN_AXR_PUT(x[1] + x[3], r->blocks ? r->block[0].bytes : 0, BOOT_NVN_AXR_W_UBO, 0);
    else if (op == ix[AXO_CLR]) BOOT_NVN_AXR_PUT(x[1], x[2], BOOT_NVN_AXR_W_CLEAR, 0);
    else if (op == ix[AXO_RVAL]) BOOT_NVN_AXR_PUT(x[2], 4, BOOT_NVN_AXR_W_REPORT, 0);
    else if (op == ix[AXO_RCNT]) BOOT_NVN_AXR_PUT(x[2], 16, BOOT_NVN_AXR_W_REPORT, 1);
    else if ((op == ix[AXO_TPOOL] || op == ix[AXO_SPOOL]) && r->blocks && r->block[0].bytes >= 20) {   /* {memory pool object, offset, count} */
        uint64_t mem; memcpy(&mem, r->block[0].data, 8);
        BOOT_NVN_AXR_PUT(mem ? mem + 0x18 : 0, 8, BOOT_NVN_AXR_OBJ, 0);
    } else if (op == ix[AXO_RTS] && r->blocks > 1) {   /* the texture objects captured at record time (views, then 0x80 per object): each pool word */
        const uint32_t n = x[1] < 8 ? (uint32_t)x[1] : 8;
        if (r->block[1].bytes >= (16u + 0x80u) * (n + 1)) {
            const uint8_t *objs = r->block[1].data + 16 * (size_t)(n + 1);
            for (uint32_t i = 0; i <= n; ++i) {   /* the colour targets, then the depth target's */
                uint64_t pool; memcpy(&pool, objs + 0x80 * (size_t)i + 0x10, 8);
                if (i < n || x[4]) BOOT_NVN_AXR_PUT(pool ? pool + 0x18 : 0, 8, BOOT_NVN_AXR_OBJ, 1);
            }
        }
    }
#undef BOOT_NVN_AXR_PUT
    return k;
}
/* Record time: the index entries of record r (its deferred bind, or the handles of a CallCommands: the ones the
 * executor follows, 64 at most), for the record's append (boot_nvn_slot_append). 0 entries: none. */
static inline unsigned boot_nvn_dsnap_dref(int op, const BootNvnRecord *r, uint64_t h[64], BootNvnDrefAdd *d) {
    static int deferred = -2, call = -2;
    if (deferred == -2) deferred = boot_nvn_name_index("nvnCommandBufferBindTexturesDeferred");
    if (call == -2) call = boot_nvn_name_index("nvnCommandBufferCallCommands");
    d->count = 0; d->n = 0; d->ptr = h; d->bound = NULL; d->lost = NULL;
    if (op == deferred) { h[0] = r->x[4]; d->count = (uint32_t)r->x[3]; d->n = 1; }
    else if (op == call && r->blocks && r->block[0].arg == 2 && r->block[0].bytes >= 8) {
        uint32_t n = r->block[0].bytes / 8 < 64 ? r->block[0].bytes / 8 : 64;
        memcpy(h, r->block[0].data, (size_t)n * 8); d->count = UINT32_MAX; d->n = n;
    }
    else if (boot_nvn_ax_mode() && (d->n = boot_nvn_axr_of(op, r, h, h + 32)) != 0) { d->count = BOOT_NVN_DREF_RANGE; d->bound = h + 32; }   /* async-xlate: summaries */
    return d->n;
}
/* Host maps (open addressing, key 0 = empty, backward-shift deletion): the command buffer object -> the slot of
 * its recording in progress, and the driver's command handle (EndRecording's result) -> the slot holding that
 * recording. A handle the driver returns again (the game reused that memory) replaces and releases the older
 * recording, as overwriting command memory would. */
typedef struct BootNvnHostMap { SRWLOCK lock; uint64_t *key, *val; uint32_t cap, used; } BootNvnHostMap;
static inline uint32_t boot_nvn_hmap_hash(uint64_t k, uint32_t cap) {
    k ^= k >> 33; k *= UINT64_C(0xff51afd7ed558ccd); k ^= k >> 33; return (uint32_t)k & (cap - 1);
}
static inline int boot_nvn_hmap_grow_locked(BootNvnHostMap *m) {
    uint32_t cap = m->cap ? m->cap * 2 : 1024;
    uint64_t *key = (uint64_t *)calloc(cap, 8), *val = (uint64_t *)calloc(cap, 8);
    if (!key || !val) { free(key); free(val); return 0; }
    for (uint32_t i = 0; i < m->cap; ++i) if (m->key[i]) {
        uint32_t j = boot_nvn_hmap_hash(m->key[i], cap); while (key[j]) j = (j + 1) & (cap - 1);
        key[j] = m->key[i]; val[j] = m->val[i];
    }
    free(m->key); free(m->val); m->key = key; m->val = val; m->cap = cap;
    return 1;
}
static inline uint64_t boot_nvn_hmap_get(BootNvnHostMap *m, uint64_t k) {
    uint64_t v = 0;
    boot_nvn_rl_lock_shared(&m->lock, BOOT_NVN_RL_MAP);
    if (k && m->cap) for (uint32_t j = boot_nvn_hmap_hash(k, m->cap); m->key[j]; j = (j + 1) & (m->cap - 1)) if (m->key[j] == k) { v = m->val[j]; break; }
    ReleaseSRWLockShared(&m->lock);
    return v;
}
/* k -> v (v == 0 removes k). Returns the previous value (0: none); UINT64_MAX when out of host memory. */
static inline uint64_t boot_nvn_hmap_put(BootNvnHostMap *m, uint64_t k, uint64_t v) {
    uint64_t old = 0; if (!k) return 0;
    boot_nvn_rl_lock(&m->lock, BOOT_NVN_RL_MAP);
    if (v && (m->used + 1) * 4 > m->cap * 3 && !boot_nvn_hmap_grow_locked(m)) { ReleaseSRWLockExclusive(&m->lock); return UINT64_MAX; }
    if (m->cap) {
        uint32_t j = boot_nvn_hmap_hash(k, m->cap);
        while (m->key[j] && m->key[j] != k) j = (j + 1) & (m->cap - 1);
        if (m->key[j]) {
            old = m->val[j];
            if (v) m->val[j] = v;
            else {   /* backward-shift deletion keeps every probe chain unbroken */
                m->key[j] = 0; m->val[j] = 0; --m->used;
                for (uint32_t i = (j + 1) & (m->cap - 1); m->key[i]; i = (i + 1) & (m->cap - 1)) {
                    uint32_t home = boot_nvn_hmap_hash(m->key[i], m->cap);
                    if (((i - home) & (m->cap - 1)) >= ((i - j) & (m->cap - 1))) {
                        m->key[j] = m->key[i]; m->val[j] = m->val[i]; m->key[i] = 0; m->val[i] = 0; j = i;
                    }
                }
            }
        } else if (v) { m->key[j] = k; m->val[j] = v; ++m->used; }
    }
    ReleaseSRWLockExclusive(&m->lock);
    return old;
}
static inline BootNvnHostMap *boot_nvn_cmd_slots(void) { static BootNvnHostMap m = {SRWLOCK_INIT, NULL, NULL, 0, 0}; return &m; }
static inline BootNvnHostMap *boot_nvn_handles(void) { static BootNvnHostMap m = {SRWLOCK_INIT, NULL, NULL, 0, 0}; return &m; }
/* Host threads: a submission walks a recording while other guest threads record. When the driver hands out a
 * handle again (cmd_end_ret), the older recording was released at once and its slot went back to the free list,
 * so a newer BeginRecording could take it (len 0) under the walker: "bad record at <shadow>" and the submit
 * fault (play-10, play-25). A submission therefore pins each recording it resolves from a handle, atomically with
 * the lookup (the map is re-read under the store lock; frees take that lock after replacing the entry), and a
 * release while pinned only marks the slot dead; boot_nvn_unpin_all (after each submitted handle) frees it.
 * Lock order: the store lock before a slot lock and before a map lock (neither is held while taking the other).
 * reclock: the pins of this thread are a table (slot -> its data and length, taken once at the pin) with a hash
 * index instead of a linear list, and a handle that resolves to a slot this thread already pinned is answered
 * without the store lock: the slot cannot have been freed or reused since (its map entry is removed before any
 * release, and a pinned slot is never freed), so this is the answer the locked path would give (TOTK_RECLOCK=0:
 * asked under the store lock, as before). */
typedef struct BootNvnPinSet { BootNvnPin *pin; uint32_t *hash; uint32_t n, cap, hcap, last; } BootNvnPinSet;   /* async-xlate: named (was anonymous) */
static _Thread_local BootNvnPinSet boot_nvn_pins;
/* async-xlate: a submit call's pins move with its captured submission (t2 -> S -> the translation thread X -> S -> t2, host_nvn_async.h):
 * exchanges this thread's set with *s. A set held outside a thread is either the call's pins or an empty one keeping its buffers (an
 * empty set's hash is all zero: boot_nvn_unpin_all clears each entry it drops), so either side can take the other's. */
static inline void boot_nvn_pins_swap(BootNvnPinSet *s) { const BootNvnPinSet t = boot_nvn_pins; boot_nvn_pins = *s; *s = t; }
static inline uint32_t boot_nvn_pin_hash(uint32_t i, uint32_t hcap) { return (i * 2654435761u) & (hcap - 1); }
static inline const BootNvnPin *boot_nvn_pin_find(uint32_t i) {
    uint32_t l = boot_nvn_pins.last;
    if (l < boot_nvn_pins.n && boot_nvn_pins.pin[l].slot == i) return &boot_nvn_pins.pin[l];   /* most reads: the slot of the previous one */
    if (!boot_nvn_pins.n) return NULL;
    for (uint32_t m = boot_nvn_pins.hcap - 1, j = boot_nvn_pin_hash(i, boot_nvn_pins.hcap), h; (h = boot_nvn_pins.hash[j]) != 0; j = (j + 1) & m)
        if (boot_nvn_pins.pin[h - 1].slot == i) { boot_nvn_pins.last = h - 1; return &boot_nvn_pins.pin[h - 1]; }
    return NULL;
}
static inline int boot_nvn_pin_add(uint32_t i, const uint8_t *data, uint32_t len) {
    if (boot_nvn_pins.n == boot_nvn_pins.cap) {
        uint32_t cap = boot_nvn_pins.cap ? boot_nvn_pins.cap * 2 : 256;
        BootNvnPin *p = (BootNvnPin *)realloc(boot_nvn_pins.pin, (size_t)cap * sizeof *p);
        if (!p) return 0;
        boot_nvn_pins.pin = p; boot_nvn_pins.cap = cap;
    }
    if (2 * (boot_nvn_pins.n + 1) > boot_nvn_pins.hcap) {   /* load <= 1/2; rebuilt from the table */
        uint32_t hcap = boot_nvn_pins.hcap ? boot_nvn_pins.hcap * 2 : 512;
        uint32_t *h = (uint32_t *)calloc(hcap, 4);
        if (!h) return 0;
        for (uint32_t k = 0; k < boot_nvn_pins.n; ++k) {
            uint32_t j = boot_nvn_pin_hash(boot_nvn_pins.pin[k].slot, hcap); while (h[j]) j = (j + 1) & (hcap - 1);
            h[j] = k + 1; boot_nvn_pins.pin[k].hpos = j;
        }
        free(boot_nvn_pins.hash); boot_nvn_pins.hash = h; boot_nvn_pins.hcap = hcap;
    }
    uint32_t j = boot_nvn_pin_hash(i, boot_nvn_pins.hcap); while (boot_nvn_pins.hash[j]) j = (j + 1) & (boot_nvn_pins.hcap - 1);
    boot_nvn_pins.hash[j] = boot_nvn_pins.n + 1;
    BootNvnPin *p = &boot_nvn_pins.pin[boot_nvn_pins.n]; p->data = data; p->slot = i; p->len = len; p->hpos = j;
    boot_nvn_pins.last = boot_nvn_pins.n++;
    return 1;
}
/* rec-evict (defined before boot_nvn_handles_release_range): the submit walk's notes (last use, released handles met) */
static inline void boot_nvn_ev_reach(BootNvnSlot *t, uint32_t i, uint32_t depth);
static inline void boot_nvn_ev_missing(uint64_t handle, uint32_t depth);
static inline uint64_t boot_nvn_handle_pin(uint64_t handle) {
    BootNvnShadowStore *s = boot_nvn_shadow(); const int fast = boot_nvn_reclock_on(), v2 = boot_nvn_slv2_on();
    for (unsigned tries = 0; tries < 64; ++tries) {
        uint64_t shadow = boot_nvn_hmap_get(boot_nvn_handles(), handle); if (!shadow) return 0;
        uint32_t i = boot_nvn_slot_of(shadow); int ok = 0, again = 0;
        if (fast && boot_nvn_pin_find(i)) { ++boot_nvn_rl()->n[BOOT_NVN_RL_PIN_REPEATS]; return shadow; }   /* already pinned by this submission */
        const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_PIN, v2);   /* storelock-work: shared with v2 (frees are exclusive), the pin atomic */
        if (boot_nvn_hmap_get(boot_nvn_handles(), handle) != shadow) again = 1;   /* replaced meanwhile: take the newer one */
        else if (i < s->count && boot_nvn_slot_at(s, i)->next_free == UINT32_MAX && !boot_nvn_slot_at(s, i)->dead) {
            BootNvnSlot *t = boot_nvn_slot_at(s, i);
            if (!fast && boot_nvn_pin_find(i)) ok = 1;   /* TOTK_RECLOCK=0: already pinned, asked under the lock as before */
            else {
                AcquireSRWLockShared(&t->lock); const uint8_t *data = t->data; uint32_t len = t->len; ReleaseSRWLockShared(&t->lock);
                if (boot_nvn_pin_add(i, data, len)) { if (v2) __atomic_add_fetch(&t->pins, 1, __ATOMIC_RELAXED); else ++t->pins; ok = 1; ++boot_nvn_rl()->n[BOOT_NVN_RL_PINS]; }
            }
        }
        boot_nvn_sl_unlock(s, BOOT_NVN_SL_PIN, v2, sl);
        if (ok) return shadow;
        if (!again) return 0;
    }
    return 0;
}
static inline void boot_nvn_unpin_all(void) {
    if (!boot_nvn_pins.n) return;
    BootNvnShadowStore *s = boot_nvn_shadow(); uint64_t reach = 0;
    if (boot_nvn_slv2_on()) {   /* storelock-work: the pins go under the shared lock; the slots left dead and unpinned are freed after it */
        BootNvnPin *pin = boot_nvn_pins.pin; uint32_t nf = 0;   /* entries 0..nf-1 reused for those: slot, generation (nf <= k: entry k is read first) */
        uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_UNPIN, 1);
        for (uint32_t k = 0; k < boot_nvn_pins.n; ++k) {
            const uint32_t i = pin[k].slot; BootNvnSlot *t = boot_nvn_slot_at(s, i);
            boot_nvn_pins.hash[pin[k].hpos] = 0; reach += pin[k].len;
            if (__atomic_load_n(&t->pins, __ATOMIC_RELAXED) && !__atomic_sub_fetch(&t->pins, 1, __ATOMIC_RELAXED) && t->dead) { pin[nf].slot = i; pin[nf].len = t->gen; ++nf; }
        }
        __atomic_add_fetch(&boot_nvn_ver.reach_bytes, reach, __ATOMIC_RELAXED); __atomic_add_fetch(&boot_nvn_ver.reach_calls, 1, __ATOMIC_RELAXED);   /* rerecord-work */
        for (uint64_t m = __atomic_load_n(&boot_nvn_ver.reach_max, __ATOMIC_RELAXED); reach > m && !__atomic_compare_exchange_n(&boot_nvn_ver.reach_max, &m, reach, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {}
        boot_nvn_sl_unlock(s, BOOT_NVN_SL_UNPIN, 1, sl);
        boot_nvn_pins.n = 0; boot_nvn_pins.last = 0;
        if (!nf) return;
        sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_UNPIN_FREE, 0);
        for (uint32_t k = 0; k < nf; ++k) {   /* still that generation, dead and unpinned (another thread's binding drop may have freed it) */
            BootNvnSlot *t = boot_nvn_slot_at(s, pin[k].slot);
            if (t->gen == pin[k].len && t->dead && !t->pins) boot_nvn_slot_free_locked(s, pin[k].slot);
        }
        boot_nvn_sl_unlock(s, BOOT_NVN_SL_UNPIN_FREE, 0, sl);
        return;
    }
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_UNPIN, 0);
    for (uint32_t k = 0; k < boot_nvn_pins.n; ++k) {
        uint32_t i = boot_nvn_pins.pin[k].slot; BootNvnSlot *t = boot_nvn_slot_at(s, i);
        if (t->pins && !--t->pins && t->dead) boot_nvn_slot_free_locked(s, i);
        boot_nvn_pins.hash[boot_nvn_pins.pin[k].hpos] = 0; reach += boot_nvn_pins.pin[k].len;
    }
    boot_nvn_pins.n = 0; boot_nvn_pins.last = 0;
    boot_nvn_ver.reach_bytes += reach; ++boot_nvn_ver.reach_calls; if (reach > boot_nvn_ver.reach_max) boot_nvn_ver.reach_max = reach;   /* rerecord-work */
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_UNPIN, 0, sl);
}
/* call-bind: the version b (slot | generation, as a CallCommands record names it) pinned for this submission like a resolved handle
 * (boot_nvn_handle_pin), dead versions included: its shadow address, 0 when it is no longer that version or class 1 released it. */
static inline uint64_t boot_nvn_cb_pin(uint64_t b) {
    BootNvnShadowStore *s = boot_nvn_shadow(); const uint32_t i = boot_nvn_slot_of(b), gen = (uint32_t)b; const uint64_t sh = b & ~UINT64_C(0xffffffff);
    const int fast = boot_nvn_reclock_on(), v2 = boot_nvn_slv2_on(); int ok = 0;
    if (!b || !(b & BOOT_NVN_SHADOW)) return 0;
    if (fast && boot_nvn_pin_find(i)) { const BootNvnSlot *t = boot_nvn_slot_peek(s, i); return t && t->gen == gen ? sh : 0; }   /* pinned: its generation cannot change */
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_CBPIN, v2);   /* storelock-work: as boot_nvn_handle_pin */
    BootNvnSlot *t = i < s->count ? boot_nvn_slot_at(s, i) : NULL;
    if (t && t->gen == gen && t->next_free == UINT32_MAX && t->closed && t->len && !(t->cb_flags & (BOOT_NVN_CB_INVALID | BOOT_NVN_CB_STRIPPED))) {
        if (!fast && boot_nvn_pin_find(i)) ok = 1;   /* TOTK_RECLOCK=0: already pinned, asked under the lock */
        else {
            AcquireSRWLockShared(&t->lock); const uint8_t *data = t->data; uint32_t len = t->len; ReleaseSRWLockShared(&t->lock);
            if (boot_nvn_pin_add(i, data, len)) { if (v2) __atomic_add_fetch(&t->pins, 1, __ATOMIC_RELAXED); else ++t->pins; ok = 1; ++boot_nvn_rl()->n[BOOT_NVN_RL_PINS]; }
        }
    }
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_CBPIN, v2, sl);
    return ok ? sh : 0;
}
/* reclock counters, appended to the periodic "submit cpu ms total" line (host_nvn_play.h). */
static inline void boot_nvn_rl_format(char *out, size_t cap) {
    uint64_t n[BOOT_NVN_RL_N] = {0}; double ms[BOOT_NVN_RL_LOCKS]; LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (unsigned t = 0; t < 65; ++t) for (unsigned k = 0; k < BOOT_NVN_RL_N; ++k) n[k] += boot_nvn_rl_slots[t].n[k];
    for (unsigned k = 0; k < BOOT_NVN_RL_LOCKS; ++k) ms[k] = f.QuadPart ? (double)boot_nvn_rl_wait_qpc[k] * 1000.0 / (double)f.QuadPart : 0.0;
    uint64_t rw[3] = {0, 0, 0}, rq[3] = {0, 0, 0};   /* storelock-work: store lock waits by thread role (render, recorders; other = the rest) */
    for (unsigned t = 0; t < 65; ++t) {
        const BootNvnRlStats *b = &boot_nvn_rl_slots[t]; const unsigned r = b->sl_role == BOOT_NVN_SL_ROLE_RENDER ? 0 : b->sl_role == BOOT_NVN_SL_ROLE_RECORDER ? 1 : 2;
        for (unsigned k = 0; k < BOOT_NVN_SL_N; ++k) { rw[r] += b->sl_wait[k]; rq[r] += b->sl_wait_qpc[k]; }
    }
    const uint64_t sw = (uint64_t)boot_nvn_rl_waits[BOOT_NVN_RL_STORE], sq = (uint64_t)boot_nvn_rl_wait_qpc[BOOT_NVN_RL_STORE];
    rw[2] = sw > rw[0] + rw[1] ? sw - rw[0] - rw[1] : 0; rq[2] = sq > rq[0] + rq[1] ? sq - rq[0] - rq[1] : 0;
    const double qms = f.QuadPart ? 1000.0 / (double)f.QuadPart : 0.0;
    BootNvnShadowStore *s = boot_nvn_shadow();
    snprintf(out, cap, "; record store (reclock %s): records %llu (cached slot %llu), buffer grows %llu, kept buffers reused %llu (%llu KiB kept, %llu KiB in all),"
             " padded blocks %llu, records read %llu (locked fetches %llu), pins %llu (repeats without lock %llu);"
             " lock waits: store %llu (%.2f ms) [render %llu (%.2f ms), recorders %llu (%.2f ms), other %llu (%.2f ms); TOTK_STORE_LOCK_V2 %s], recording %llu (%.2f ms), maps %llu (%.2f ms)",
             boot_nvn_reclock_on() ? "on" : "off",
             (unsigned long long)n[BOOT_NVN_RL_RECORDS], (unsigned long long)n[BOOT_NVN_RL_CACHED], (unsigned long long)n[BOOT_NVN_RL_GROWS],
             (unsigned long long)n[BOOT_NVN_RL_REUSED], (unsigned long long)(s->kept >> 10), (unsigned long long)((uint64_t)s->bytes >> 10),
             (unsigned long long)n[BOOT_NVN_RL_UNALIGNED], (unsigned long long)n[BOOT_NVN_RL_READS], (unsigned long long)n[BOOT_NVN_RL_LOCKED_READS],
             (unsigned long long)n[BOOT_NVN_RL_PINS], (unsigned long long)n[BOOT_NVN_RL_PIN_REPEATS],
             (unsigned long long)boot_nvn_rl_waits[BOOT_NVN_RL_STORE], ms[BOOT_NVN_RL_STORE], (unsigned long long)rw[0], (double)rq[0] * qms,
             (unsigned long long)rw[1], (double)rq[1] * qms, (unsigned long long)rw[2], (double)rq[2] * qms, boot_nvn_slv2_on() ? "on" : "off",
             (unsigned long long)boot_nvn_rl_waits[BOOT_NVN_RL_SLOT], ms[BOOT_NVN_RL_SLOT],
             (unsigned long long)boot_nvn_rl_waits[BOOT_NVN_RL_MAP], ms[BOOT_NVN_RL_MAP]);
    size_t at = strlen(out);   /* recfast */
    if (at < cap) snprintf(out + at, cap - at, "; record pool (%llu MiB, hint %s): %llu KiB pooled, taken %llu, new %llu, pooled %llu, freed (full) %llu,"
             " hinted recordings %llu (trimmed %llu), kept index arrays %llu KiB", (unsigned long long)(boot_nvn_rbuf_limit() >> 20), boot_nvn_hint_on() ? "on" : "off",
             (unsigned long long)(boot_nvn_rbuf.bytes >> 10), (unsigned long long)n[BOOT_NVN_RL_POOL_HITS], (unsigned long long)n[BOOT_NVN_RL_POOL_NEW],
             (unsigned long long)boot_nvn_rbuf.puts, (unsigned long long)boot_nvn_rbuf.drops, (unsigned long long)n[BOOT_NVN_RL_HINTED],
             (unsigned long long)n[BOOT_NVN_RL_TRIMS], (unsigned long long)(s->dref_kept >> 10));
}
/* flashfix: the per-submission table of deferred-bind handles (see boot_nvn_dsnap above). */
static inline const BootNvnDsnapEnt *boot_nvn_dsnap_find(uint64_t rec) {
    if (!boot_nvn_dsnap.active || !boot_nvn_dsnap.hash_cap) return NULL;
    uint32_t mask = boot_nvn_dsnap.hash_cap - 1;
    for (uint32_t j = boot_nvn_hmap_hash(rec, boot_nvn_dsnap.hash_cap), n = 0; n <= mask && boot_nvn_dsnap.hash_stamp[j] == boot_nvn_dsnap.stamp; j = (j + 1) & mask, ++n)
        if (boot_nvn_dsnap.ent[boot_nvn_dsnap.hash[j]].rec == rec) return &boot_nvn_dsnap.ent[boot_nvn_dsnap.hash[j]];
    return NULL;
}
static inline void boot_nvn_dsnap_hash_put(uint32_t e) {
    uint32_t mask = boot_nvn_dsnap.hash_cap - 1, j = boot_nvn_hmap_hash(boot_nvn_dsnap.ent[e].rec, boot_nvn_dsnap.hash_cap);
    while (boot_nvn_dsnap.hash_stamp[j] == boot_nvn_dsnap.stamp) j = (j + 1) & mask;
    boot_nvn_dsnap.hash_stamp[j] = boot_nvn_dsnap.stamp; boot_nvn_dsnap.hash[j] = e;
}
static inline int boot_nvn_dsnap_rehash(uint32_t cap) {
    uint32_t *h = (uint32_t *)calloc(cap, 4), *st = (uint32_t *)calloc(cap, 4);
    if (!h || !st) { free(h); free(st); return 0; }
    free(boot_nvn_dsnap.hash); free(boot_nvn_dsnap.hash_stamp);
    boot_nvn_dsnap.hash = h; boot_nvn_dsnap.hash_stamp = st; boot_nvn_dsnap.hash_cap = cap;
    for (uint32_t e = 0; e < boot_nvn_dsnap.n; ++e) boot_nvn_dsnap_hash_put(e);
    return 1;
}
/* One deferred bind: its handles now (the bind clamps to 32 - first, so 32 cover it). */
static inline void boot_nvn_dsnap_add(BootContext *c, uint64_t self, uint64_t rec, uint64_t ptr, uint32_t count) {
    if (boot_nvn_dsnap_find(rec)) return;   /* the same recording reached twice in this submission */
    if ((uint64_t)(boot_nvn_dsnap.n + 1) * 2 > boot_nvn_dsnap.hash_cap && !boot_nvn_dsnap_rehash(boot_nvn_dsnap.hash_cap ? boot_nvn_dsnap.hash_cap * 2 : 1024)) return;
    uint32_t want = count < 32 ? count : 32;
    BootNvnDsnapEnt *ents = (BootNvnDsnapEnt *)boot_nvn_dsnap_grow(boot_nvn_dsnap.ent, &boot_nvn_dsnap.cap, (uint64_t)boot_nvn_dsnap.n + 1, sizeof *ents);
    if (!ents) return;
    boot_nvn_dsnap.ent = ents;
    uint64_t *words = (uint64_t *)boot_nvn_dsnap_grow(boot_nvn_dsnap.h, &boot_nvn_dsnap.hcap, (uint64_t)boot_nvn_dsnap.hn + want, 8);
    if (!words) return;
    boot_nvn_dsnap.h = words;
    BootNvnDsnapEnt *e = &boot_nvn_dsnap.ent[boot_nvn_dsnap.n];
    e->rec = rec; e->ptr = ptr; e->count = count; e->n = 0; e->at = boot_nvn_dsnap.hn; e->ok = 0;
    if (want && ptr && boot_nvn_peek(c, self, ptr, boot_nvn_dsnap.h + boot_nvn_dsnap.hn, 8 * (size_t)want) == 8 * (size_t)want) {
        e->ok = 1; e->n = want; boot_nvn_dsnap.hn += want; boot_nvn_dsnap.words += want;
    } else ++boot_nvn_dsnap.unread;   /* the bind reads guest memory at translation (and logs if that fails too) */
    boot_nvn_dsnap_hash_put(boot_nvn_dsnap.n++); ++boot_nvn_dsnap.entries;
}
static inline void boot_nvn_dsnap_push(uint64_t shadow, uint32_t depth) {
    BootNvnDsnapWork *work = (BootNvnDsnapWork *)boot_nvn_dsnap_grow(boot_nvn_dsnap.work, &boot_nvn_dsnap.wcap, (uint64_t)boot_nvn_dsnap.wn + 1, sizeof *work);
    if (!work) { ++boot_nvn_dsnap.index_lost; return; }
    boot_nvn_dsnap.work = work;
    boot_nvn_dsnap.work[boot_nvn_dsnap.wn].shadow = shadow; boot_nvn_dsnap.work[boot_nvn_dsnap.wn].depth = depth; ++boot_nvn_dsnap.wn;
}
/* batch-a freeze table (t2 only: written by boot_nvn_dsnap_take, read by the same submit call's translation). */
static inline const BootNvnFzEnt *boot_nvn_freeze_find(uint64_t handle) {
    if (!boot_nvn_dsnap.fz_hcap) return NULL;
    uint32_t mask = boot_nvn_dsnap.fz_hcap - 1;
    for (uint32_t j = boot_nvn_hmap_hash(handle, boot_nvn_dsnap.fz_hcap), n = 0; n <= mask && boot_nvn_dsnap.fz_hstamp[j] == boot_nvn_dsnap.stamp; j = (j + 1) & mask, ++n)
        if (boot_nvn_dsnap.fz[boot_nvn_dsnap.fz_hash[j]].handle == handle) return &boot_nvn_dsnap.fz[boot_nvn_dsnap.fz_hash[j]];
    return NULL;
}
static inline void boot_nvn_freeze_hash_put(uint32_t e) {
    uint32_t mask = boot_nvn_dsnap.fz_hcap - 1, j = boot_nvn_hmap_hash(boot_nvn_dsnap.fz[e].handle, boot_nvn_dsnap.fz_hcap);
    while (boot_nvn_dsnap.fz_hstamp[j] == boot_nvn_dsnap.stamp) j = (j + 1) & mask;
    boot_nvn_dsnap.fz_hstamp[j] = boot_nvn_dsnap.stamp; boot_nvn_dsnap.fz_hash[j] = e;
}
static inline int boot_nvn_freeze_rehash(uint32_t cap) {
    uint32_t *h = (uint32_t *)calloc(cap, 4), *st = (uint32_t *)calloc(cap, 4);
    if (!h || !st) { free(h); free(st); return 0; }
    free(boot_nvn_dsnap.fz_hash); free(boot_nvn_dsnap.fz_hstamp);
    boot_nvn_dsnap.fz_hash = h; boot_nvn_dsnap.fz_hstamp = st; boot_nvn_dsnap.fz_hcap = cap;
    for (uint32_t e = 0; e < boot_nvn_dsnap.fz_n; ++e) boot_nvn_freeze_hash_put(e);
    return 1;
}
static inline void boot_nvn_freeze_put(uint64_t handle, uint64_t shadow) {
    if ((uint64_t)(boot_nvn_dsnap.fz_n + 1) * 2 > boot_nvn_dsnap.fz_hcap && !boot_nvn_freeze_rehash(boot_nvn_dsnap.fz_hcap ? boot_nvn_dsnap.fz_hcap * 2 : 1024)) { ++boot_nvn_dsnap.fz_lost; return; }
    BootNvnFzEnt *fz = (BootNvnFzEnt *)boot_nvn_dsnap_grow(boot_nvn_dsnap.fz, &boot_nvn_dsnap.fz_cap, (uint64_t)boot_nvn_dsnap.fz_n + 1, sizeof *fz);
    if (!fz) { ++boot_nvn_dsnap.fz_lost; return; }
    boot_nvn_dsnap.fz = fz;
    fz[boot_nvn_dsnap.fz_n].handle = handle; fz[boot_nvn_dsnap.fz_n].shadow = shadow;
    boot_nvn_freeze_hash_put(boot_nvn_dsnap.fz_n++); ++boot_nvn_dsnap.fz_frozen;
}
/* At the call: the recording handle h names now (pinned until the call ends); with the freeze on the answer is kept,
 * and a handle met again in this call gets the same answer. */
static inline uint64_t boot_nvn_dsnap_resolve(uint64_t h) {
    if (h & BOOT_NVN_SHADOW) return h;
    if (boot_nvn_dsnap.fz_on) { const BootNvnFzEnt *f = boot_nvn_freeze_find(h); if (f) return f->shadow; }
    uint64_t sh = boot_nvn_handle_pin(h);
    if (boot_nvn_dsnap.fz_on) { boot_nvn_freeze_put(h, sh); if (!sh) ++boot_nvn_dsnap.fz_missing; }
    return sh;
}
/* Translation (boot_nvn_record_read, the submitting thread): the recording the call resolved for handle h, 0 to use
 * the live map (freeze off, another thread, a handle the call did not reach or could not resolve). */
static inline uint64_t boot_nvn_freeze_resolve(uint64_t h) {
    if (!boot_nvn_dsnap.active || !boot_nvn_dsnap.fz_on || boot_nvn_dsnap.tid != boot_nvn_call_tid()) return 0;   /* async-xlate: X acts for the call's thread */
    const BootNvnFzEnt *f = boot_nvn_freeze_find(h);
    if (!f) { ++boot_nvn_dsnap.fz_unknown; if (!boot_nvn_rv.missed) InterlockedExchange(&boot_nvn_rv.missed, 1); return 0; }   /* rerecord-work: no more versions in this call */
    if (!f->shadow) return 0;   /* not in the map at the call (counted as missing there) */
    ++boot_nvn_dsnap.fz_reads;
    if (boot_nvn_hmap_get(boot_nvn_handles(), h) != f->shadow) ++boot_nvn_dsnap.fz_moved;   /* recorded again since the call */
    return f->shadow;
}
/* call-bind, the translation of one CallCommands entry (boot_nvn_exec_handle; the submitting thread): handle h, the version b its
 * caller bound when it recorded it (0: none). *live_sh: the recording h names for this call (the call's frozen resolution, else the
 * live map; not pinned here), *bound_sh: b pinned (DIVERGED only). */
enum { BOOT_NVN_CB_R_UNBOUND, BOOT_NVN_CB_R_OFF, BOOT_NVN_CB_R_SAME, BOOT_NVN_CB_R_DIVERGED, BOOT_NVN_CB_R_GONE, BOOT_NVN_CB_R_INVALID, BOOT_NVN_CB_R_INPLACE };
static inline void boot_nvn_cb_fallback_log(uint64_t h, uint64_t b, unsigned why);
static inline unsigned boot_nvn_cb_resolve(uint64_t h, uint64_t b, uint64_t *bound_sh, uint64_t *live_sh) {
    *bound_sh = *live_sh = 0;
    if (!b) { InterlockedIncrement64(&boot_nvn_cb.ex_unbound); return BOOT_NVN_CB_R_UNBOUND; }
    if (!boot_nvn_cb_mode()) { InterlockedIncrement64(&boot_nvn_cb.ex_off); return BOOT_NVN_CB_R_OFF; }
    const BootNvnFzEnt *f = boot_nvn_dsnap.active && boot_nvn_dsnap.fz_on && boot_nvn_dsnap.tid == boot_nvn_call_tid() ? boot_nvn_freeze_find(h) : NULL;   /* async-xlate: as above */
    const uint64_t cur = f ? f->shadow : boot_nvn_hmap_get(boot_nvn_handles(), h);
    *live_sh = cur;
    if (cur && boot_nvn_slot_of(cur) == boot_nvn_slot_of(b)) {   /* the same slot: the same version while b's binding holds it (else stale: check) */
        const BootNvnSlot *t = boot_nvn_slot_peek(boot_nvn_shadow(), boot_nvn_slot_of(cur));
        if (t && t->gen == (uint32_t)b) { InterlockedIncrement64(&boot_nvn_cb.ex_same); return BOOT_NVN_CB_R_SAME; }
    }
    const uint64_t sh = boot_nvn_cb_pin(b);
    if (!sh) {   /* not playable: class 1 released it, it was recorded again in place (its marker says so), or it is no longer there */
        const BootNvnSlot *t = boot_nvn_slot_peek(boot_nvn_shadow(), boot_nvn_slot_of(b));
        const unsigned why = !t || t->gen != (uint32_t)b || !(t->cb_flags & (BOOT_NVN_CB_INVALID | BOOT_NVN_CB_STRIPPED)) ? BOOT_NVN_CB_R_GONE :
                             (t->cb_flags & BOOT_NVN_CB_INPLACE) ? BOOT_NVN_CB_R_INPLACE : BOOT_NVN_CB_R_INVALID;
        InterlockedIncrement64(why == BOOT_NVN_CB_R_INVALID ? &boot_nvn_cb.ex_invalid : why == BOOT_NVN_CB_R_INPLACE ? &boot_nvn_cb.ex_inplace : &boot_nvn_cb.ex_gone);
        boot_nvn_cb_fallback_log(h, b, why);
        return why;
    }
    *bound_sh = sh; InterlockedIncrement64(&boot_nvn_cb.ex_diverged);
    return BOOT_NVN_CB_R_DIVERGED;
}
/* The address the translation plays for that entry: mode 1 the bound version when it is not the handle's current one, otherwise (and
 * in mode 2, which only watches) the handle, as before. */
static inline uint64_t boot_nvn_cb_target(uint64_t h, uint64_t b, unsigned *how, uint64_t *bound_sh, uint64_t *live_sh) {
    const unsigned r = boot_nvn_cb_resolve(h, b, bound_sh, live_sh); *how = r;
    return r == BOOT_NVN_CB_R_DIVERGED && boot_nvn_cb_mode() == 1 ? *bound_sh : h;
}
/* nvnQueueSubmitCommands(queue, count, handles), first thing (renderer lock held): every deferred bind the submitted
 * recordings reach, directly or through CallCommands (8 levels, as the executor), copied now. Each recording reached
 * is pinned (boot_nvn_handle_pin) until boot_nvn_dsnap_end, so the record addresses stay what the translation reads.
 * batch-a: with the freeze the handle resolutions and the submitted handle values are kept too (see above); the index
 * and stamp of a slot are read under its own lock (reclock), not the store lock. */
static inline void boot_nvn_dsnap_take(BootContext *c, uint64_t self, uint64_t handles, uint64_t count) {
    boot_nvn_dsnap.active = 0; boot_nvn_dsnap.fz_on = 0; boot_nvn_dsnap.top_n = 0; boot_nvn_dsnap.covered = 0;
    boot_nvn_dsnap.ft_latest_end = 0; boot_nvn_dsnap.ft_latest_thread = UINT32_MAX; boot_nvn_dsnap.ft_recent = 0;
    boot_nvn_axr.n = 0;   /* async-xlate: this call's range summaries (collected by the walk below) */
    if (!count || !boot_nvn_dsnap_wanted()) return;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    if (!++boot_nvn_dsnap.stamp) boot_nvn_dsnap.stamp = 1;
    boot_nvn_dsnap.n = boot_nvn_dsnap.hn = boot_nvn_dsnap.wn = 0; boot_nvn_dsnap.fz_n = 0; boot_nvn_dsnap.active = 1; ++boot_nvn_dsnap.submits;
    boot_nvn_dsnap.fz_on = boot_nvn_freeze_on(); boot_nvn_dsnap.tid = GetCurrentThreadId();
    const int trace = boot_nvn_ftrace_on(); int top_ok = 1; uint64_t recent = 0;
    if (trace) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); recent = (uint64_t)f.QuadPart / 1000; }   /* frame trace: 1 ms */
    BootNvnShadowStore *s = boot_nvn_shadow();
    /* rerecord-work: TOTK_SNAP_ATOMIC: no EndRecording changes the handle map during the walk (boot_nvn_handle_install) */
    const int atomic = boot_nvn_dsnap.fz_on && boot_nvn_snap_atomic_on(); const uint64_t lost0 = boot_nvn_dsnap.fz_lost, missing0 = boot_nvn_dsnap.fz_missing;
    if (atomic) AcquireSRWLockExclusive(&boot_nvn_walk_lock);
    for (uint64_t k = 0; k < count && k < 4096; ++k) {
        uint64_t h = 0, sh;
        if (boot_nvn_peek(c, self, handles + 8 * k, &h, 8) != 8) { top_ok = 0; continue; }
        if (top_ok) {   /* batch-a: the handle values as the call read them (a prefix: a failed read ends it) */
            uint64_t *top = (uint64_t *)boot_nvn_dsnap_grow(boot_nvn_dsnap.top, &boot_nvn_dsnap.top_cap, (uint64_t)boot_nvn_dsnap.top_n + 1, 8);
            if (top) { boot_nvn_dsnap.top = top; top[boot_nvn_dsnap.top_n++] = h; } else top_ok = 0;
        }
        if (h && (sh = boot_nvn_dsnap_resolve(h)) != 0) boot_nvn_dsnap_push(sh, 0);
        else if (h) boot_nvn_ev_missing(h, 0);   /* rec-evict: a released handle submitted again? */
    }
    while (boot_nvn_dsnap.wn) {
        BootNvnDsnapWork w = boot_nvn_dsnap.work[--boot_nvn_dsnap.wn];
        uint32_t i = boot_nvn_slot_of(w.shadow), nr = 0; uint64_t base = BOOT_NVN_SHADOW | (uint64_t)i << 32;
        BootNvnSlot *t = boot_nvn_slot_peek(s, i);
        if (!t) continue;
        AcquireSRWLockExclusive(&t->lock);
        if (t->next_free == UINT32_MAX) boot_nvn_ev_reach(t, i, w.depth);   /* rec-evict: last use, and submitted or called */
        if (t->next_free == UINT32_MAX && t->snap != boot_nvn_dsnap.stamp) {
            t->snap = boot_nvn_dsnap.stamp; ++boot_nvn_dsnap.slots;
            if (trace && t->end_qpc) {   /* frame trace: the newest recording the call reached */
                if (t->end_qpc > boot_nvn_dsnap.ft_latest_end) { boot_nvn_dsnap.ft_latest_end = t->end_qpc; boot_nvn_dsnap.ft_latest_thread = t->rec_thread; }
                if (t->end_qpc + recent >= (uint64_t)q0.QuadPart) ++boot_nvn_dsnap.ft_recent;   /* ended less than 1 ms before the call */
            }
            BootNvnDref *refs = t->dref_n ? (BootNvnDref *)boot_nvn_dsnap_grow(boot_nvn_dsnap.refs, &boot_nvn_dsnap.rcap, t->dref_n, sizeof *refs) : NULL;
            if (refs) { boot_nvn_dsnap.refs = refs; memcpy(refs, t->dref, (size_t)t->dref_n * sizeof *t->dref); nr = t->dref_n; }
            else if (t->dref_n) ++boot_nvn_dsnap.index_lost;
        }
        ReleaseSRWLockExclusive(&t->lock);
        for (uint32_t k = 0; k < nr; ++k) {   /* guest reads and pins outside the slot lock */
            const BootNvnDref *d = &boot_nvn_dsnap.refs[k];
            if (d->count == BOOT_NVN_DREF_RANGE) { boot_nvn_axr_add(d->ptr, d->bound); continue; }   /* async-xlate: a range summary (no guest read) */
            if (d->count != UINT32_MAX) { boot_nvn_dsnap_add(c, self, base + d->off, d->ptr, d->count); continue; }
            const uint64_t sh = w.depth < 8 && d->ptr ? boot_nvn_dsnap_resolve(d->ptr) : 0;
            if (sh) boot_nvn_dsnap_push(sh, w.depth + 1);
            else if (w.depth < 8 && d->ptr) boot_nvn_ev_missing(d->ptr, w.depth + 1);   /* rec-evict: a released handle called again? */
            if (w.depth < 8 && d->bound && boot_nvn_cb_mode() && (!sh || boot_nvn_slot_of(sh) != boot_nvn_slot_of(d->bound))) {
                const uint64_t bsh = boot_nvn_cb_pin(d->bound);   /* call-bind: the version the caller bound, re-recorded since: pinned and walked too */
                if (bsh) boot_nvn_dsnap_push(bsh, w.depth + 1);
            }
        }
    }
    /* rerecord-work: the walk reached all the translation can reach and the freeze holds every answer (index_lost: since boot,
     * a recording whose index lost an entry stays incomplete). Published before the walk lock goes, so a recording ended
     * during the walk (installed right after it) is covered. */
    boot_nvn_dsnap.covered = boot_nvn_dsnap.fz_on && top_ok && count <= 4096 && boot_nvn_dsnap.fz_lost == lost0 && boot_nvn_dsnap.fz_missing == missing0 && !boot_nvn_dsnap.index_lost;
    boot_nvn_rv_publish();
    if (atomic) ReleaseSRWLockExclusive(&boot_nvn_walk_lock);
    QueryPerformanceCounter(&q1); boot_nvn_dsnap.take_qpc += (uint64_t)(q1.QuadPart - q0.QuadPart);
}
/* End of the submit call: the table is no longer current; the pins taken by the snapshot and the translation go. */
static inline void boot_nvn_dsnap_end(void) {
    if (!boot_nvn_dsnap.active) return;
    boot_nvn_dsnap.active = 0; boot_nvn_dsnap.fz_on = 0; boot_nvn_unpin_all();
}
/* batch-a: the submit call in progress (seq odd inside a call; tid/thread: its host thread and guest thread), for the
 * re-record wait and the frame trace. A call that re-enters is impossible (renderer lock, one submitting thread). */
#define BOOT_NVN_SUB_EVENTS 32
typedef struct BootNvnSubEvent { uint64_t handle, qpc; uint32_t thread, pad; } BootNvnSubEvent;
static struct {
    SRWLOCK lock; CONDITION_VARIABLE cv; volatile LONG64 seq; DWORD tid; unsigned thread; uint64_t call_qpc, prev_call_qpc;
    BootNvnSubEvent ev[BOOT_NVN_SUB_EVENTS]; uint32_t ev_n, ev_lost;   /* this call's re-records (frame trace) */
    volatile LONG64 rerecords, outside, waits, wait_qpc, capped, logged;
} boot_nvn_sub;   /* zero: SRWLOCK_INIT / CONDITION_VARIABLE_INIT */
static inline uint64_t boot_nvn_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (uint64_t)q.QuadPart; }
static inline uint64_t boot_nvn_qpf(void) { static uint64_t f; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart > 0 ? (uint64_t)q.QuadPart : 1; } return f; }
/* rerecord-work: at the end of the walk (t2, inside the call, still holding the walk lock: lock order walk lock, then
 * boot_nvn_sub.lock): from now on a re-record of a recording this walk reached may be kept as a version
 * (boot_nvn_rv_decide) when the walk covered the whole submission. */
static inline void boot_nvn_rv_publish(void) {
    boot_nvn_rv.unknown0 = boot_nvn_dsnap.fz_unknown; InterlockedExchange64(&boot_nvn_rv.kept_call, 0); InterlockedExchange(&boot_nvn_rv.missed, 0);
    uint32_t cover = boot_nvn_rerecord_version_on() && boot_nvn_dsnap.active && boot_nvn_dsnap.covered ? boot_nvn_dsnap.stamp : 0;
    if (!cover) return;
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    if (boot_nvn_sub.seq & 1) boot_nvn_rv.cover = cover;
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
}
/* rerecord-work: may the re-record that made `old` a version go on without waiting? cover: read with the call's seq.
 * BOOT_NVN_RV_KEPT, or why it waits (see TOTK_RERECORD_VERSION). `old` may have been freed and reused since (the call
 * ended): then nothing reads it any more and either answer is safe. */
static inline unsigned boot_nvn_rv_decide(uint64_t old, uint32_t cover) {
    if (!boot_nvn_rerecord_version_on()) return BOOT_NVN_RV_OFF;
    if (!cover) return BOOT_NVN_RV_UNCOVERED;   /* the walk still runs, or did not cover the whole submission */
    BootNvnSlot *t = boot_nvn_slot_peek(boot_nvn_shadow(), boot_nvn_slot_of(old)); uint32_t snap = 0;
    if (t) { AcquireSRWLockShared(&t->lock); snap = t->snap; ReleaseSRWLockShared(&t->lock); }
    if (snap != cover) return BOOT_NVN_RV_UNREACHED;   /* pinned by the translation through the live map, not by the walk */
    if (__atomic_load_n(&boot_nvn_rv.missed, __ATOMIC_SEQ_CST)) return BOOT_NVN_RV_MISSED;
    if (__atomic_load_n(&boot_nvn_ver.bytes, __ATOMIC_RELAXED) > boot_nvn_rerecord_budget()) return BOOT_NVN_RV_BUDGET;
    return BOOT_NVN_RV_KEPT;
}
/* rerecord-work, end of a submit call (t2): a call that kept a version and whose translation then met a handle its walk had not
 * reached is an alarm (the released thread could have re-recorded that handle before the translation took it from the live
 * map; not expected: the walk follows what the executor follows). Counted, the first 16 logged. */
static inline void boot_nvn_rv_call_end(void) {
    LONG64 kept = InterlockedExchange64(&boot_nvn_rv.kept_call, 0);
    if (!kept || boot_nvn_dsnap.fz_unknown == boot_nvn_rv.unknown0) return;
    LONG64 n = InterlockedIncrement64(&boot_nvn_rv.alarms);
#ifdef TOTK_DRAW_PATH
    if (n <= 16) boot_draw_log("nvn native: re-record versions ALARM: a call kept %s%llu versions and then reached %llu handles its walk had not", "", (uint64_t)kept, boot_nvn_dsnap.fz_unknown - boot_nvn_rv.unknown0);
#else
    (void)n;
#endif
}
static inline void boot_nvn_sub_begin(BootContext *c) {
    unsigned th = boot_nvn_guest_thread(c);
    { BootNvnRlStats *m = boot_nvn_rl(); m->sl_role = BOOT_NVN_SL_ROLE_RENDER; m->sl_guest = th; }   /* storelock-work: the render thread's counters */
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    if (!(boot_nvn_sub.seq & 1)) ++boot_nvn_sub.seq;
    boot_nvn_sub.tid = GetCurrentThreadId(); boot_nvn_sub.thread = th;
    boot_nvn_sub.prev_call_qpc = boot_nvn_sub.call_qpc; boot_nvn_sub.call_qpc = boot_nvn_qpc(); boot_nvn_sub.ev_n = boot_nvn_sub.ev_lost = 0;
    boot_nvn_rv.cover = 0;   /* rerecord-work: no versions until this call's walk vouches (boot_nvn_rv_publish) */
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    if (boot_nvn_ftrace_on()) { __atomic_store_n(&boot_mt_ft.thread, (LONG)th, __ATOMIC_RELAXED); __atomic_store_n(&boot_mt_ft.on, 1, __ATOMIC_RELEASE); }
}
static inline void boot_nvn_sub_end(void) {
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    if (boot_nvn_sub.seq & 1) ++boot_nvn_sub.seq;
    boot_nvn_rv.cover = 0;   /* rerecord-work */
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    WakeAllConditionVariable(&boot_nvn_sub.cv);
    boot_nvn_rv_call_end();
}
/* batch-a: EndRecording returned handle `handle` again while a submission had its older recording `old` pinned (the
 * old slot is only marked dead, so the call still reads it; with the freeze its translation keeps reading it). Noted
 * for the trace, logged (first 16), and with TOTK_RERECORD_WAIT the recording thread waits for the call to return. */
static inline void boot_nvn_rerecorded(BootContext *c, uint64_t handle, uint64_t old) {
    unsigned me = boot_nvn_guest_thread(c); uint64_t now = boot_nvn_qpc(), f = boot_nvn_qpf();
    InterlockedIncrement64(&boot_nvn_sub.rerecords);
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    LONG64 seq = boot_nvn_sub.seq; int inside = (seq & 1) && boot_nvn_sub.tid != GetCurrentThreadId();
    uint64_t call = boot_nvn_sub.call_qpc; unsigned submitter = boot_nvn_sub.thread; uint32_t cover = boot_nvn_rv.cover;   /* rerecord-work: with seq */
    if (inside) {
        if (boot_nvn_sub.ev_n < BOOT_NVN_SUB_EVENTS) { BootNvnSubEvent *e = &boot_nvn_sub.ev[boot_nvn_sub.ev_n++]; e->handle = handle; e->qpc = now; e->thread = me; e->pad = 0; }
        else ++boot_nvn_sub.ev_lost;
    }
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    if (!inside) { InterlockedIncrement64(&boot_nvn_sub.outside); return; }   /* the call had just ended (its pins were going) */
    const int wait = boot_nvn_rerecord_wait_on();
    const unsigned rv = wait ? boot_nvn_rv_decide(old, cover) : BOOT_NVN_RV_N;   /* rerecord-work: BOOT_NVN_RV_KEPT: kept as a version, no wait */
#ifdef TOTK_DRAW_PATH
    if (InterlockedIncrement64(&boot_nvn_sub.logged) <= 16) {
        char t[200]; snprintf(t, sizeof t, " (batch-a): handle %llx by t%u, %.2f ms after the submit call of t%u began (older recording slot %u)%s", (unsigned long long)handle, me,
                              (double)(now - call) * 1000.0 / (double)f, submitter, boot_nvn_slot_of(old),
                              !wait ? "" : rv == BOOT_NVN_RV_KEPT ? ", kept as a version: the thread goes on" : ", the thread waits for the call");
        boot_draw_log("nvn native: recording replaced while a submission reads it%s (event %llu of the first 16, %llu so far)", t, (uint64_t)boot_nvn_sub.logged, (uint64_t)boot_nvn_sub.rerecords);
    }
#else
    (void)submitter; (void)old; (void)call;
#endif
    if (!wait) return;
    if (rv == BOOT_NVN_RV_KEPT) { InterlockedIncrement64(&boot_nvn_rv.n[BOOT_NVN_RV_KEPT]); InterlockedIncrement64(&boot_nvn_rv.kept_call); return; }   /* rerecord-work */
    if (boot_mt.active) { CRITICAL_SECTION *cs = boot_render_cs(); if ((DWORD)(uintptr_t)cs->OwningThread == GetCurrentThreadId()) return; }   /* never under the renderer lock */
    InterlockedIncrement64(&boot_nvn_rv.n[rv]);   /* rerecord-work: why it still waits */
    BootCore *k = boot_mt.active && boot_mt_core_ctx(c) ? (BootCore *)(void *)c : NULL;
    if (k) __atomic_store_n(&k->in_guest, 0, __ATOMIC_SEQ_CST);   /* outside guest code: a quiescing kernel need not wait for it */
    uint64_t t0 = boot_nvn_qpc(), cap = (uint64_t)BOOT_NVN_RERECORD_CAP_MS * f / 1000, t1 = t0; int capped = 0;
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    while (boot_nvn_sub.seq == seq) {
        t1 = boot_nvn_qpc();
        if (t1 - t0 >= cap) { capped = 1; break; }
        DWORD ms = (DWORD)((cap - (t1 - t0)) * 1000 / f) + 1;
        SleepConditionVariableSRW(&boot_nvn_sub.cv, &boot_nvn_sub.lock, ms > 100 ? 100u : ms, 0);
    }
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    t1 = boot_nvn_qpc();
    if (k) boot_mt_guest_enter(k);
    InterlockedIncrement64(&boot_nvn_sub.waits); InterlockedExchangeAdd64(&boot_nvn_sub.wait_qpc, (LONG64)(t1 - t0));
    if (capped) InterlockedIncrement64(&boot_nvn_sub.capped);
}
/* batch-a frame trace: who ended a recording and when (EndRecording, before its handle is installed). */
static inline void boot_nvn_slot_mark_end(const BootContext *c, uint64_t shadow) {
    BootNvnSlot *t = boot_nvn_slot_peek(boot_nvn_shadow(), boot_nvn_slot_of(shadow));
    if (!t) return;
    unsigned th = boot_nvn_guest_thread(c); uint64_t now = boot_nvn_qpc();
    AcquireSRWLockExclusive(&t->lock);
    if (t->next_free == UINT32_MAX) { t->rec_thread = th; t->end_qpc = now; }
    ReleaseSRWLockExclusive(&t->lock);
}
/* EndRecording's return (boot_nvn_cmd_end_ret): handle -> slot, and the older recording of that handle released (only marked
 * dead, a version, while a submission reads it: batch-a's re-record, boot_nvn_rerecorded), in that order as before.
 * rerecord-work: with TOTK_SNAP_ATOMIC both steps hold boot_nvn_walk_lock shared, so a submit call's walk sees both or
 * neither. 0: out of host memory (slot released). */
/* rec-evict: with rg (EndRecording's return, TOTK_REC_EVICT_DEAD / TOTK_REC_EVICT not both 0), inside the same walk-lock section:
 * the recordings this one was written over are released and this one is indexed (boot_nvn_ev_installed). */
typedef struct BootNvnEvRange { uint64_t ctl, cmd; uint32_t ctl_len, cmd_len; } BootNvnEvRange;   /* guest CPU ranges; length 0 = none */
static inline void boot_nvn_ev_installed(uint64_t handle, uint64_t slot, const BootNvnEvRange *rg);
/* EndRecording's return, a handle recorded again (boot_nvn_handle_install_ev, before the older recording is released): when the new
 * recording wrote over the older one's commands (a list re-recorded in place, TOTK_REC_EVICT_DEAD tracking its ranges), a caller bound
 * to the older one would, on the console, run the new bytes: that version is marked so the call plays the handle ("recorded again in
 * place", counted with the fallbacks). Overlap with either range of the new recording. */
static inline void boot_nvn_cb_overwritten_locked(BootNvnShadowStore *s, uint64_t old, const BootNvnEvRange *rg) {   /* store lock held exclusively */
    const uint32_t i = boot_nvn_slot_of(old); BootNvnSlot *t = i < s->count ? boot_nvn_slot_at(s, i) : NULL;
    if (t && t->cb_refs && t->ev_cmd_len &&
        ((rg->cmd_len && t->ev_cmd < rg->cmd + rg->cmd_len && rg->cmd < t->ev_cmd + t->ev_cmd_len) || (rg->ctl_len && t->ev_cmd < rg->ctl + rg->ctl_len && rg->ctl < t->ev_cmd + t->ev_cmd_len))) {
        t->cb_flags |= BOOT_NVN_CB_INVALID | BOOT_NVN_CB_INPLACE; ++boot_nvn_cb.inplace;
    }
}
static inline void boot_nvn_cb_overwritten(uint64_t old, const BootNvnEvRange *rg) {
    BootNvnShadowStore *s = boot_nvn_shadow();
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_OVERWRITE, 0);   /* storelock-work: exclusive (a bind's lookup and count stay on one side of it) */
    boot_nvn_cb_overwritten_locked(s, old, rg);
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_OVERWRITE, 0, sl);
}
/* storelock-work (TOTK_STORE_LOCK_V2): EndRecording's return, the older recording of a handle: boot_nvn_cb_overwritten (ow: its ranges, or
 * NULL) and boot_nvn_slot_free in ONE exclusive section (one of the orders the two sections could already run in: the older recording is
 * out of the handle map before either, so no pin or bind can reach it through the map in between). */
static inline int boot_nvn_slot_free_ow(uint64_t shadow, const BootNvnEvRange *ow) {
    BootNvnShadowStore *s = boot_nvn_shadow(); uint32_t i = boot_nvn_slot_of(shadow); int pinned = 0;
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_FREE, 0);
    if (ow) boot_nvn_cb_overwritten_locked(s, shadow, ow);
    if (i < s->count && boot_nvn_slot_at(s, i)->next_free == UINT32_MAX && !boot_nvn_slot_at(s, i)->dead) pinned = boot_nvn_slot_free_locked(s, i);
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_FREE, 0, sl);   /* (then its bindings) */
    return pinned;
}
static inline int boot_nvn_handle_install_ev(BootContext *c, uint64_t handle, uint64_t slot, const BootNvnEvRange *rg) {
    const int atomic = boot_nvn_snap_atomic_on(); int pinned = 0; BootCore *k = NULL;
    if (atomic && !TryAcquireSRWLockShared(&boot_nvn_walk_lock)) {   /* a walk runs (about 0.5 ms): outside guest code meanwhile */
        k = boot_mt.active && boot_mt_core_ctx(c) ? (BootCore *)(void *)c : NULL;
        if (k) __atomic_store_n(&k->in_guest, 0, __ATOMIC_SEQ_CST);
        uint64_t t0 = boot_nvn_qpc(); AcquireSRWLockShared(&boot_nvn_walk_lock);
        InterlockedIncrement64(&boot_nvn_rv.walk_waits); InterlockedExchangeAdd64(&boot_nvn_rv.walk_wait_qpc, (LONG64)(boot_nvn_qpc() - t0));
    }
    uint64_t old = boot_nvn_hmap_put(boot_nvn_handles(), handle, slot);
    if (old != UINT64_MAX && old && old != slot) {
        const BootNvnEvRange *ow = rg && boot_nvn_cb_mode() ? rg : NULL;   /* call-bind: recorded again over its own commands? */
        if (boot_nvn_slv2_on()) pinned = boot_nvn_slot_free_ow(old, ow);   /* storelock-work: both in one exclusive section */
        else { if (ow) boot_nvn_cb_overwritten(old, ow); pinned = boot_nvn_slot_free(old); }
    }
    if (old != UINT64_MAX && rg) boot_nvn_ev_installed(handle, slot, rg);   /* rec-evict: overwritten recordings released, this one indexed */
    if (atomic) ReleaseSRWLockShared(&boot_nvn_walk_lock);
    if (k) boot_mt_guest_enter(k);   /* after the release: never in a quiesce wait while the walk waits for us */
    if (old == UINT64_MAX) { boot_nvn_slot_free(slot); return 0; }
    if (pinned) boot_nvn_rerecorded(c, handle, old);   /* batch-a: a submission was reading the older one */
    return 1;
}
static inline int boot_nvn_handle_install(BootContext *c, uint64_t handle, uint64_t slot) { return boot_nvn_handle_install_ev(c, handle, slot, NULL); }
/* rerecord-work: every 300 presents (boot_nvn_queue_present) the "re-record versions" line (see TOTK_RERECORD_VERSION). */
static inline void boot_nvn_rv_present(void) {
#ifdef TOTK_DRAW_PATH
    static volatile LONG64 presents; static uint64_t l_re, l_out, l_n[BOOT_NVN_RV_N], l_wq, l_ww, l_wwq, l_rb, l_rc, l_al;
    uint64_t p = (uint64_t)InterlockedIncrement64(&presents);
    if (p % 300) return;
    uint64_t f = boot_nvn_qpf(), n[BOOT_NVN_RV_N], vn, vb, pn, pb, rb, rc, rm, waits = 0;
    for (unsigned k = 0; k < BOOT_NVN_RV_N; ++k) { n[k] = (uint64_t)boot_nvn_rv.n[k]; if (k != BOOT_NVN_RV_KEPT) waits += n[k] - l_n[k]; }
    BootNvnShadowStore *s = boot_nvn_shadow();
    AcquireSRWLockExclusive(&s->lock);
    vn = boot_nvn_ver.n; vb = boot_nvn_ver.bytes; pn = boot_nvn_ver.peak_n; pb = boot_nvn_ver.peak_bytes;
    rb = boot_nvn_ver.reach_bytes; rc = boot_nvn_ver.reach_calls; rm = boot_nvn_ver.reach_max;
    boot_nvn_ver.peak_n = vn; boot_nvn_ver.peak_bytes = vb; boot_nvn_ver.reach_max = 0;
    ReleaseSRWLockExclusive(&s->lock);
    uint64_t re = (uint64_t)boot_nvn_sub.rerecords, out = (uint64_t)boot_nvn_sub.outside, wq = (uint64_t)boot_nvn_sub.wait_qpc;
    uint64_t ww = (uint64_t)boot_nvn_rv.walk_waits, wwq = (uint64_t)boot_nvn_rv.walk_wait_qpc, al = (uint64_t)boot_nvn_rv.alarms;
    double ms = 1000.0 / (double)f; char line[1000];
    snprintf(line, sizeof line, " (rerecord-work; TOTK_RERECORD_VERSION %s, budget %llu MiB, TOTK_SNAP_ATOMIC %s, TOTK_RERECORD_WAIT %s) last 300 presents:"
             " recordings replaced while a call read them %llu (after its call %llu), kept as versions without a wait %llu, waits %llu (%.1f ms; why: switch off %llu,"
             " call not covered %llu, not reached by the call's walk %llu, the call met a handle its walk missed %llu, over budget %llu); versions held now %llu (%llu KiB),"
             " peak %llu (%llu KiB); pinned per call %.1f KiB (max %llu KiB, %llu calls); walk-lock waits %llu (%.2f ms); alarms %llu; totals: kept %llu, waits %llu, alarms %llu;",
             boot_nvn_rerecord_version_on() ? "on" : "off", (unsigned long long)(boot_nvn_rerecord_budget() >> 20), boot_nvn_snap_atomic_on() ? "on" : "off",
             boot_nvn_rerecord_wait_on() ? "on" : "off", (unsigned long long)(re - l_re), (unsigned long long)(out - l_out),
             (unsigned long long)(n[BOOT_NVN_RV_KEPT] - l_n[BOOT_NVN_RV_KEPT]), (unsigned long long)waits, (double)(wq - l_wq) * ms,
             (unsigned long long)(n[BOOT_NVN_RV_OFF] - l_n[BOOT_NVN_RV_OFF]), (unsigned long long)(n[BOOT_NVN_RV_UNCOVERED] - l_n[BOOT_NVN_RV_UNCOVERED]),
             (unsigned long long)(n[BOOT_NVN_RV_UNREACHED] - l_n[BOOT_NVN_RV_UNREACHED]), (unsigned long long)(n[BOOT_NVN_RV_MISSED] - l_n[BOOT_NVN_RV_MISSED]),
             (unsigned long long)(n[BOOT_NVN_RV_BUDGET] - l_n[BOOT_NVN_RV_BUDGET]), (unsigned long long)vn, (unsigned long long)(vb >> 10), (unsigned long long)pn,
             (unsigned long long)(pb >> 10), rc > l_rc ? (double)(rb - l_rb) / 1024.0 / (double)(rc - l_rc) : 0.0, (unsigned long long)(rm >> 10),
             (unsigned long long)(rc - l_rc), (unsigned long long)(ww - l_ww), (double)(wwq - l_wwq) * ms, (unsigned long long)(al - l_al),
             (unsigned long long)n[BOOT_NVN_RV_KEPT], (unsigned long long)(uint64_t)boot_nvn_sub.waits, (unsigned long long)al);
    boot_draw_log("nvn native: re-record versions%s present %llu %llu", line, p, 0);
    l_re = re; l_out = out; memcpy(l_n, n, sizeof l_n); l_wq = wq; l_ww = ww; l_wwq = wwq; l_rb = rb; l_rc = rc; l_al = al;
#endif
}
/* The periodic counters line (host_nvn_play.h, every 100 submissions). */
static inline void boot_nvn_batch_a_format(char *out, size_t cap) {
    uint64_t f = boot_nvn_qpf();
    snprintf(out, cap, " (batch-a; TOTK_SUBMIT_FREEZE %s, TOTK_RERECORD_WAIT %s, TOTK_RECLOCK %s, TOTK_FRAME_TRACE %s): handles resolved at the call %llu (not in the map then %llu),"
             " translation lookups from the freeze %llu (map moved since the call %llu), handles not reached at the call %llu, freeze entries lost %llu;"
             " recordings replaced while a submission read them %llu (after its call %llu), waits for the call %llu (%llu ms, capped %llu);",
             boot_nvn_freeze_on() ? "on" : "off", boot_nvn_rerecord_wait_on() ? "on" : "off", boot_nvn_reclock_on() ? "on" : "off", boot_nvn_ftrace_on() ? "on" : "off",
             (unsigned long long)boot_nvn_dsnap.fz_frozen, (unsigned long long)boot_nvn_dsnap.fz_missing, (unsigned long long)boot_nvn_dsnap.fz_reads,
             (unsigned long long)boot_nvn_dsnap.fz_moved, (unsigned long long)boot_nvn_dsnap.fz_unknown, (unsigned long long)boot_nvn_dsnap.fz_lost,
             (unsigned long long)boot_nvn_sub.rerecords, (unsigned long long)boot_nvn_sub.outside, (unsigned long long)boot_nvn_sub.waits,
             (unsigned long long)((uint64_t)boot_nvn_sub.wait_qpc * 1000 / f), (unsigned long long)boot_nvn_sub.capped);
}
/* batch-a frame trace (TOTK_FRAME_TRACE=1): one block per submit call, written at its end (renderer lock held, before
 * the pins go). Draw counts come from the executor (host_nvn_play.h). */
static inline void boot_nvn_ftrace_exec_counts(uint64_t v[3]);
static inline const char *boot_nvn_ftrace_svc(uint32_t svc) {
    return svc == 0x0b ? "sleep" : svc == 0x18 ? "waitsync" : svc == 0x1a ? "mutex" : svc == 0x1c ? "condvar" : svc == 0x34 ? "arbiter" : "svc";
}
static inline void boot_nvn_ftrace_write(BootContext *c, uint64_t count, int ok) {
    static FILE *fp; static int failed; static uint32_t seen; static uint64_t calls;
    static uint64_t p_slots, p_frozen, p_missing, p_reads, p_moved, p_unknown, p_binds, p_hits, p_misses, p_ex[3];
    static uint32_t p_to[BOOT_THREAD_CAPACITY], p_early[BOOT_THREAD_CAPACITY];
    if (failed) return;
    if (!fp) { fp = fopen("frame-trace.log", "a"); if (!fp) { failed = 1; return; }
               fprintf(fp, "# batch-a frame trace (TOTK_FRAME_TRACE=1): one block per nvnQueueSubmitCommands; times in ms against the call (QPC), threads are guest threads\n"); }
    uint64_t f = boot_nvn_qpf(), now = boot_nvn_qpc(), call = boot_nvn_sub.call_qpc, prev = boot_nvn_sub.prev_call_qpc;
    double ms = 1000.0 / (double)f;
    uint64_t ex[3]; boot_nvn_ftrace_exec_counts(ex);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(fp, "sub %llu %02u:%02u:%02u.%03u t%u: %llu handles, %.2f ms since the previous call, call took %.2f ms, ok %d | draws %llu clears %llu skipped %llu"
            " | reached %llu recordings (newest ended %.2f ms before the call by t%d, %u within 1 ms) | handles resolved %llu (not recorded yet %llu)"
            " | translation: frozen %llu, moved since the call %llu, not reached by the call %llu | deferred binds %llu (copy %llu, not in copy %llu)"
            " | replaced during the call %llu (+%u lost) | re-record waits so far %llu (%.1f ms)\n",
            (unsigned long long)++calls, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, boot_nvn_sub.thread, (unsigned long long)count,
            prev ? (double)(call - prev) * ms : 0.0, (double)(now - call) * ms, ok,
            (unsigned long long)(ex[0] - p_ex[0]), (unsigned long long)(ex[1] - p_ex[1]), (unsigned long long)(ex[2] - p_ex[2]),
            (unsigned long long)(boot_nvn_dsnap.slots - p_slots),
            boot_nvn_dsnap.ft_latest_end && boot_nvn_dsnap.ft_latest_end <= call ? (double)(call - boot_nvn_dsnap.ft_latest_end) * ms : boot_nvn_dsnap.ft_latest_end ? -(double)(boot_nvn_dsnap.ft_latest_end - call) * ms : 0.0,
            boot_nvn_dsnap.ft_latest_thread == UINT32_MAX ? -1 : (int)boot_nvn_dsnap.ft_latest_thread, boot_nvn_dsnap.ft_recent,
            (unsigned long long)(boot_nvn_dsnap.fz_frozen - p_frozen), (unsigned long long)(boot_nvn_dsnap.fz_missing - p_missing),
            (unsigned long long)(boot_nvn_dsnap.fz_reads - p_reads), (unsigned long long)(boot_nvn_dsnap.fz_moved - p_moved), (unsigned long long)(boot_nvn_dsnap.fz_unknown - p_unknown),
            (unsigned long long)(boot_nvn_dsnap.binds - p_binds), (unsigned long long)(boot_nvn_dsnap.hits - p_hits), (unsigned long long)(boot_nvn_dsnap.misses - p_misses),
            (unsigned long long)boot_nvn_sub.ev_n, boot_nvn_sub.ev_lost, (unsigned long long)boot_nvn_sub.waits, (double)(uint64_t)boot_nvn_sub.wait_qpc * ms);
    p_slots = boot_nvn_dsnap.slots; p_frozen = boot_nvn_dsnap.fz_frozen; p_missing = boot_nvn_dsnap.fz_missing; p_reads = boot_nvn_dsnap.fz_reads;
    p_moved = boot_nvn_dsnap.fz_moved; p_unknown = boot_nvn_dsnap.fz_unknown; p_binds = boot_nvn_dsnap.binds; p_hits = boot_nvn_dsnap.hits; p_misses = boot_nvn_dsnap.misses;
    memcpy(p_ex, ex, sizeof ex);
    /* the submitted handles: the recording each resolved to at the call */
    BootNvnShadowStore *s = boot_nvn_shadow();
    for (uint32_t k = 0; k < boot_nvn_dsnap.top_n && k < 8; ++k) {
        uint64_t h = boot_nvn_dsnap.top[k], sh = h & BOOT_NVN_SHADOW ? h : 0;
        if (!sh) { const BootNvnFzEnt *e = boot_nvn_dsnap.fz_on ? boot_nvn_freeze_find(h) : NULL; sh = e ? e->shadow : boot_nvn_hmap_get(boot_nvn_handles(), h); }
        BootNvnSlot *t = sh ? boot_nvn_slot_peek(s, boot_nvn_slot_of(sh)) : NULL; uint32_t len = 0, th = UINT32_MAX; uint64_t end = 0;
        if (t) { AcquireSRWLockShared(&t->lock); len = t->len; th = t->rec_thread; end = t->end_qpc; ReleaseSRWLockShared(&t->lock); }
        uint64_t live = h & BOOT_NVN_SHADOW ? sh : boot_nvn_hmap_get(boot_nvn_handles(), h);
        fprintf(fp, "  handle %llx -> slot %u: %u bytes, ended by t%d %.2f ms %s the call%s\n", (unsigned long long)h, sh ? boot_nvn_slot_of(sh) : UINT32_MAX, len,
                th == UINT32_MAX ? -1 : (int)th, end ? (double)(end <= call ? call - end : end - call) * ms : 0.0, end <= call ? "before" : "after",
                live == sh ? "" : live ? " (RECORDED AGAIN since: the map now names another recording)" : " (no longer in the map)");
    }
    {   BootNvnSubEvent ev[BOOT_NVN_SUB_EVENTS]; uint32_t ne;   /* written by the recording threads under the lock */
        AcquireSRWLockExclusive(&boot_nvn_sub.lock);
        ne = boot_nvn_sub.ev_n < BOOT_NVN_SUB_EVENTS ? boot_nvn_sub.ev_n : BOOT_NVN_SUB_EVENTS; memcpy(ev, boot_nvn_sub.ev, (size_t)ne * sizeof ev[0]);
        ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
        for (uint32_t k = 0; k < ne; ++k)
            fprintf(fp, "  replaced during the call: handle %llx by t%u at +%.2f ms\n", (unsigned long long)ev[k].handle, ev[k].thread,
                    ev[k].qpc >= call ? (double)(ev[k].qpc - call) * ms : 0.0); }
    /* the submitting thread's guest waits since its previous call, and every thread's timeouts */
    BootMtFtWait w[BOOT_MT_FT_WAITS]; static uint32_t to[BOOT_THREAD_CAPACITY], early[BOOT_THREAD_CAPACITY];
    unsigned n = boot_mt_ft_copy(c, &seen, w, BOOT_MT_FT_WAITS, to, early);
    if (n) {
        fprintf(fp, "  t%u waits since its previous call:", boot_nvn_sub.thread);
        for (unsigned k = 0; k < n; ++k) {
            char tos[32]; if ((int64_t)w[k].timeout_ns < 0) snprintf(tos, sizeof tos, "inf"); else snprintf(tos, sizeof tos, "%.2f", (double)w[k].timeout_ns / 1e6);
            fprintf(fp, " [%s %llx timeout %s -> %x after %.2f ms%s, %.2f ms before the call]", boot_nvn_ftrace_svc(w[k].svc), (unsigned long long)w[k].obj, tos, w[k].result,
                    (double)w[k].waited * ms, w[k].parked ? "" : " at once", w[k].qpc <= call ? (double)(call - w[k].qpc) * ms : -(double)(w[k].qpc - call) * ms);
        }
        fprintf(fp, "\n");
    }
    int any = 0;
    for (unsigned i = 0; i < BOOT_THREAD_CAPACITY; ++i) if (to[i] != p_to[i] || early[i] != p_early[i]) {
        if (!any) { fprintf(fp, "  timed-out waits since the previous call:"); any = 1; }
        fprintf(fp, " t%u %u (early %u)", i, to[i] - p_to[i], early[i] - p_early[i]);
    }
    if (any) fprintf(fp, "\n");
    memcpy(p_to, to, sizeof p_to); memcpy(p_early, early, sizeof p_early);
    fflush(fp);
}
#ifndef TOTK_DRAW_PATH
static inline void boot_nvn_ftrace_exec_counts(uint64_t v[3]) { v[0] = v[1] = v[2] = 0; }
#endif

/* ---------------------------------------------------------------- backend */
typedef struct BootNvnBackend {
    void *ctx;
    /* Execute recorded handles on a queue (in order). Returns 0 on failure (logged). */
    int (*submit)(void *ctx, BootContext *c, uint64_t self, uint64_t queue, uint64_t handle);
    /* GPU-side signal after everything submitted so far; returns the fence value. */
    uint64_t (*signal)(void *ctx, uint64_t queue);
    /* CPU wait for a fence value with timeout in ns (UINT64_MAX = infinite). 1 reached, 0 timeout. */
    int (*wait)(void *ctx, uint64_t queue, uint64_t value, uint64_t timeout_ns);
    uint64_t (*completed)(void *ctx, uint64_t queue);
    /* GPU-side wait of `queue` for `value` signalled on `other`. */
    void (*gpu_wait)(void *ctx, uint64_t queue, uint64_t other, uint64_t value);
    /* Memory pool storage mapped at a GPU virtual address (size 0: unmapped). */
    void (*pool)(void *ctx, uint64_t va, uint64_t storage, uint64_t size, uint32_t flags);
    int (*present)(void *ctx, BootContext *c, uint64_t queue, uint64_t window, uint32_t index);
} BootNvnBackend;
/* Walks one handle (and nested CallCommands) to count records: the null backend's submit. */
typedef struct BootNvnNullStats { uint64_t submits, records, by_op[BOOT_NVN_NAME_COUNT], fence; } BootNvnNullStats;
static inline int boot_nvn_walk(BootContext *c, uint64_t self, uint64_t handle, BootNvnNullStats *st, unsigned depth);
static inline int boot_nvn_null_submit(void *ctx, BootContext *c, uint64_t self, uint64_t queue, uint64_t handle) {
    (void)queue; BootNvnNullStats *st = ctx; ++st->submits; return boot_nvn_walk(c, self, handle, st, 0);
}
static inline uint64_t boot_nvn_null_signal(void *ctx, uint64_t queue) { (void)queue; return ++((BootNvnNullStats *)ctx)->fence; }
static inline int boot_nvn_null_wait(void *ctx, uint64_t queue, uint64_t value, uint64_t timeout_ns) { (void)queue; (void)timeout_ns; return value <= ((BootNvnNullStats *)ctx)->fence; }
static inline uint64_t boot_nvn_null_completed(void *ctx, uint64_t queue) { (void)queue; return ((BootNvnNullStats *)ctx)->fence; }
static inline void boot_nvn_null_gpu_wait(void *ctx, uint64_t queue, uint64_t other, uint64_t value) { (void)ctx; (void)queue; (void)other; (void)value; }
static inline void boot_nvn_null_pool(void *ctx, uint64_t va, uint64_t storage, uint64_t size, uint32_t flags) { (void)ctx; (void)va; (void)storage; (void)size; (void)flags; }
static inline int boot_nvn_null_present(void *ctx, BootContext *c, uint64_t queue, uint64_t window, uint32_t index) { (void)ctx; (void)c; (void)queue; (void)window; (void)index; return 1; }
static inline BootNvnNullStats *boot_nvn_null_stats(void) { static BootNvnNullStats s; return &s; }
static inline BootNvnBackend *boot_nvn_backend(void) {
    static BootNvnBackend b = {0};
    if (!b.submit) { b.ctx = boot_nvn_null_stats(); b.submit = boot_nvn_null_submit; b.signal = boot_nvn_null_signal; b.wait = boot_nvn_null_wait;
        b.completed = boot_nvn_null_completed; b.gpu_wait = boot_nvn_null_gpu_wait; b.pool = boot_nvn_null_pool; b.present = boot_nvn_null_present; }
    return &b;
}

/* ---------------------------------------------------------------- helpers */
static inline uint64_t boot_nvn_address(const char *name) { char n[112]; snprintf(n, sizeof n, "nvn:%s", name); return boot_host_fn_address(n); }
#define BOOT_NVN_GET(obj) do { if (!boot_host_read(c, self, address, &(obj), sizeof(obj))) return BOOT_HOST_STOP; } while (0)
#define BOOT_NVN_PUT(obj) do { if (!boot_host_write(c, self, address, &(obj), sizeof(obj))) return BOOT_HOST_STOP; } while (0)
/* Native-mode stops are logged to nvn-native.log (cwd) as well: the guest stops with BOOT_TARGET at the
 * host function's own address, which otherwise reads like untranslated code. */
static inline void boot_nvn_log(const char *what, const char *name, uint64_t value) {
    fprintf(stderr, "nvn native: %s (%s %llx)\n", what, name, (unsigned long long)value);
    FILE *f = fopen("nvn-native.log", "a");
    if (f) { fprintf(f, "nvn native: %s (%s %llx)\n", what, name, (unsigned long long)value); fclose(f); }
}
static inline int boot_nvn_fail(BootContext *c, uint64_t self, const char *what, uint64_t value) {
    boot_nvn_log(what, boot_host_fn_name(self) ? boot_host_fn_name(self) : "?", value);
    boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
}
#define BOOT_NVN_FN(name) static inline int name(BootContext *c, uint64_t self, uint64_t *target, void *user)
#define BOOT_NVN_UNUSED (void)target; (void)user; (void)self

/* ---------------------------------------------------------------- device */
BOOT_NVN_FN(boot_nvn_device_builder_set_defaults) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnDeviceBuilder b = {BOOT_NVN_MAGIC_DEVICE_BUILDER, 0}; BOOT_NVN_PUT(b); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_device_builder_set_flags) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnDeviceBuilder b; BOOT_NVN_GET(b); b.flags = (uint32_t)c->x[1]; BOOT_NVN_PUT(b); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_device_builder_get_flags) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnDeviceBuilder b; BOOT_NVN_GET(b); c->x[0] = b.flags; return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_device_initialize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[1]; BootNvnDeviceBuilder b; BOOT_NVN_GET(b);
    BootNvnDevice d = {BOOT_NVN_MAGIC_DEVICE, b.flags, 0, 0, BOOT_NVN_VA_BASE, 0, 0};
    address = c->x[0]; BOOT_NVN_PUT(d); c->x[0] = 1; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_device_finalize) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnDevice d; BOOT_NVN_GET(d); d.magic = 0; BOOT_NVN_PUT(d); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_device_get_proc_address) {
    BOOT_NVN_UNUSED; char name[112] = {0};
    if (!boot_host_read_string(c, self, c->x[1], name, sizeof name)) return BOOT_HOST_STOP;
    c->x[0] = boot_nvn_name_index(name) >= 0 ? boot_nvn_address(name) : 0; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_device_get_integer) {
    BOOT_NVN_UNUSED; uint32_t pname = (uint32_t)c->x[1]; int32_t v = 0; int known = 0;
    for (size_t i = 0; i < sizeof boot_nvn_integers / sizeof boot_nvn_integers[0]; ++i)
        if (boot_nvn_integers[i].pname == pname) { v = boot_nvn_integers[i].value; known = 1; }
    if (!known) fprintf(stderr, "nvn native: nvnDeviceGetInteger %u unknown (0)\n", pname);
    if (!boot_host_write(c, self, c->x[2], &v, 4)) return BOOT_HOST_STOP;
    return BOOT_HOST_DONE;
}
/* Same bit layout as the Maxwell bindless handle the translated shaders decode (texture | sampler << 20). */
BOOT_NVN_FN(boot_nvn_device_get_texture_handle) { BOOT_NVN_UNUSED; c->x[0] = (c->x[1] & 0xfffff) | ((c->x[2] & 0xfff) << 20); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_device_get_image_handle) { BOOT_NVN_UNUSED; c->x[0] = c->x[1] & 0xfffff; return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_device_get_timestamp_ns) {
    BOOT_NVN_UNUSED; LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    c->x[0] = (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_device_set_word) {   /* SetDepthMode (user 0) / SetWindowOriginMode (user 1) */
    (void)target; uint64_t address = c->x[0]; BootNvnDevice d; BOOT_NVN_GET(d);
    if (user) d.window_origin = (uint32_t)c->x[1]; else d.depth_mode = (uint32_t)c->x[1];
    BOOT_NVN_PUT(d); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_device_install_debug_callback) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnDevice d; BOOT_NVN_GET(d);
    d.debug_callback = c->x[1]; d.debug_data = c->x[2]; BOOT_NVN_PUT(d); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_nop) { BOOT_NVN_UNUSED; (void)c; (void)self; return BOOT_HOST_DONE; }   /* debug labels */

/* ---------------------------------------------------------------- memory pools and buffers */
BOOT_NVN_FN(boot_nvn_pool_builder_set_defaults) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnPoolBuilder b = {BOOT_NVN_MAGIC_POOL_BUILDER, 0, 0, 0, 0}; BOOT_NVN_PUT(b); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_pool_builder_set) {   /* user: 0 device, 1 flags, 2 storage */
    (void)target; uint64_t address = c->x[0]; BootNvnPoolBuilder b; BOOT_NVN_GET(b);
    switch ((int)(intptr_t)user) { case 0: b.device = c->x[1]; break; case 1: b.flags = (uint32_t)c->x[1]; break; default: b.storage = c->x[1]; b.size = c->x[2]; }
    BOOT_NVN_PUT(b); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_pool_initialize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[1]; BootNvnPoolBuilder b; BOOT_NVN_GET(b);
    if (!b.device) return boot_nvn_fail(c, self, "memory pool without device", 0);
    BootNvnDevice d; address = b.device; BOOT_NVN_GET(d);
    if (d.magic != BOOT_NVN_MAGIC_DEVICE) return boot_nvn_fail(c, self, "memory pool device", b.device);
    BootNvnPool p = {BOOT_NVN_MAGIC_POOL, b.flags, b.device, b.storage, b.size, d.va_next};
    d.va_next += (b.size + BOOT_NVN_VA_ALIGN - 1) & ~(BOOT_NVN_VA_ALIGN - 1);   /* allocator state lives in the guest device */
    BOOT_NVN_PUT(d);
    address = c->x[0]; BOOT_NVN_PUT(p);
    BootNvnBackend *be = boot_nvn_backend(); be->pool(be->ctx, p.va, p.storage, p.size, p.flags);
    c->x[0] = 1; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_pool_finalize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnPool p; BOOT_NVN_GET(p);
    BootNvnBackend *be = boot_nvn_backend(); be->pool(be->ctx, p.va, p.storage, 0, p.flags);
    p.magic = 0; BOOT_NVN_PUT(p); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_pool_get) {   /* user: 0 map (storage), 1 buffer address (va), 2 flags, 3 size */
    (void)target; uint64_t address = c->x[0]; BootNvnPool p; BOOT_NVN_GET(p);
    if (p.magic != BOOT_NVN_MAGIC_POOL) return boot_nvn_fail(c, self, "not a memory pool", address);
    switch ((int)(intptr_t)user) { case 0: c->x[0] = p.storage; break; case 1: c->x[0] = p.va; break; case 2: c->x[0] = p.flags; break; default: c->x[0] = p.size; }
    return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_pool_flush) {   /* Flush/InvalidateMappedRange: host-coherent storage; caches over the range are the backend's */
    BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnPool p; BOOT_NVN_GET(p);
    if (p.magic != BOOT_NVN_MAGIC_POOL) return boot_nvn_fail(c, self, "not a memory pool", address);
    return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_buffer_builder_set_defaults) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnBufferBuilder b = {BOOT_NVN_MAGIC_BUFFER_BUILDER, 0, 0, 0, 0, 0}; BOOT_NVN_PUT(b); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_buffer_builder_set) {   /* user: 0 device, 1 storage (pool, offset, size) */
    (void)target; uint64_t address = c->x[0]; BootNvnBufferBuilder b; BOOT_NVN_GET(b);
    if (!user) b.device = c->x[1]; else { b.pool = c->x[1]; b.offset = c->x[2]; b.size = c->x[3]; }
    BOOT_NVN_PUT(b); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_buffer_initialize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[1]; BootNvnBufferBuilder b; BOOT_NVN_GET(b);
    if (!b.pool) return boot_nvn_fail(c, self, "NULL memory pool (the game's GPU memory allocation failed)", 0);
    BootNvnPool p; address = b.pool; BOOT_NVN_GET(p);
    if (p.magic != BOOT_NVN_MAGIC_POOL || b.offset > p.size || b.size > p.size - b.offset) return boot_nvn_fail(c, self, "buffer storage", b.pool);
    BootNvnBuffer f = {BOOT_NVN_MAGIC_BUFFER, 0, p.va + b.offset, p.storage + b.offset, b.size};
    address = c->x[0]; BOOT_NVN_PUT(f); c->x[0] = 1; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_buffer_get) {   /* user: 0 address, 1 map, 2 size */
    (void)target; uint64_t address = c->x[0]; BootNvnBuffer f; BOOT_NVN_GET(f);
    if (f.magic != BOOT_NVN_MAGIC_BUFFER) return boot_nvn_fail(c, self, "not a buffer", address);
    c->x[0] = !user ? f.va : (intptr_t)user == 1 ? f.cpu : f.size; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_buffer_finalize) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnBuffer f; BOOT_NVN_GET(f); f.magic = 0; BOOT_NVN_PUT(f); return BOOT_HOST_DONE; }

/* ---------------------------------------------------------------- command buffers (the driver's, recorded) */
/* The command buffer stays the driver's: every nvnCommandBuffer* call runs in the driver after the host
 * function (tail call), so the object layout, command and control memory use, GetCommandMemoryUsed/Free and
 * the out-of-memory callback are the driver's, byte for byte. The game depends on all of them: it sizes GPU
 * memory from GetCommandMemoryUsed (main 0x711060 measures every material's command sets into [obj+0x3f4],
 * 0x70d0e8 allocates their command memory at 0x70d7bc from an exactly sized model heap; with our own 16-byte
 * answers the pool layout differed and the allocation failed: 14.4B native stop, heap
 * "Model/FldObj_OpeningField_A..."), and it inlines driver internals (BeginRecording at 7 sites; method
 * writes at 0x706394, 0x72c754, 0x7047dc, 0x73e6d0 against cmd+0x20/+0x28). The driver's methods in command
 * memory are never read or submitted: the host records each call's arguments (record_build) into a host
 * slot, keyed by the command buffer while recording and by the driver's handle after EndRecording;
 * QueueSubmitCommands and CallCommands/CopyCommands handles resolve through that map. Inline method writes
 * bypass the API, so they are switched off: the driver's nvnPrivate0 (sdk+0xdbb9a8, which the game reads
 * through its GOT 0x45586e8 before every inline path) is cleared after the driver sets it. */
#define BOOT_NVN_INLINE_FLAG ((UINT64_C(3) << 28) | UINT64_C(0xdbb9a8))
enum { BOOT_NVN_CMD_PASS = 0, BOOT_NVN_CMD_RECORD = 1, BOOT_NVN_CMD_BEGIN = 2, BOOT_NVN_CMD_END = 3, BOOT_NVN_CMD_RESET = 4, BOOT_NVN_CMD_ADD_MEMORY = 5,
       BOOT_NVN_CMD_ADD_CONTROL = 6 };   /* rec-evict: AddControlMemory (driver only, as before) */
static inline uint8_t *boot_nvn_cmd_kinds(void) { static uint8_t k[BOOT_NVN_NAME_COUNT]; return k; }
static inline void boot_nvn_inline_off(BootContext *c, uint64_t self) {
    uint64_t a = boot_address(c, BOOT_NVN_INLINE_FLAG); uint32_t v = 0, zero = 0;
    if (!a || a == UINT64_MAX || boot_nvn_peek(c, self, a, &v, 4) != 4 || !v) return;
    if (boot_host_write(c, self, a, &zero, 4)) {
        static LONG logged; if (!InterlockedExchange(&logged, 1)) boot_nvn_log("inline command writes switched off (nvnPrivate0 was set)", "sdk+0xdbb9a8", v);
    }
}
static inline uint64_t boot_nvn_driver_fn(BootContext *c, int index) {
    uint64_t logical = boot_nvn_trace()->procs[index], real = logical ? boot_address(c, logical) : 0;
    return real == UINT64_MAX ? 0 : real;
}
/* The record a CommandBuffer call makes: arguments and the argument memory the call reads now. */
/* SetRenderTargets(cmd, count, textures, views, depth, depth view): block 0 the texture pointers, block 1 the
 * contents (first 16 bytes) of every view, colour targets then depth, zero for none. The views are the
 * game's temporaries: by the submit their memory holds other data (nvn-15: a depth view read back as TIC
 * words), so they are read now. */
static inline int boot_nvn_record_render_targets(BootContext *c, uint64_t self, BootNvnRecord *r, uint8_t *scratch, size_t cap) {
    uint32_t k = c->x[1] < 8 ? (uint32_t)c->x[1] : 8; size_t used = 0;
    if (cap < 8 * 8 + (16 + 0x80) * 9) return 0;
    if (c->x[2] && k) used = boot_nvn_peek(c, self, c->x[2], scratch, 8 * (size_t)k) & ~(size_t)7;
    r->block[0].arg = 2; r->block[0].bytes = (uint32_t)used; r->block[0].data = scratch;
    uint8_t *views = scratch + 8 * 8; memset(views, 0, 16 * (k + 1));
    for (uint32_t i = 0; i < k; ++i) {
        uint64_t v = 0;
        if (c->x[3] && boot_nvn_peek(c, self, c->x[3] + 8 * i, &v, 8) == 8 && v && boot_nvn_peek(c, self, v, views + 16 * i, 16) != 16) memset(views + 16 * i, 0, 16);
    }
    if (c->x[5] && boot_nvn_peek(c, self, c->x[5], views + 16 * k, 16) != 16) memset(views + 16 * k, 0, 16);
    /* then the texture objects (0x80 bytes each, colour targets then depth): the driver programs the targets'
     * addresses now, and the game re-initializes ping-pong texture objects before the submit (white-chars/render-02) */
    uint8_t *objs = views + 16 * (k + 1); memset(objs, 0, 0x80 * (size_t)(k + 1));
    for (uint32_t i = 0; i < k && 8 * i + 8 <= used; ++i) {
        uint64_t t; memcpy(&t, scratch + 8 * i, 8);
        if (t && boot_nvn_peek(c, self, t, objs + 0x80 * i, 0x80) != 0x80) memset(objs + 0x80 * i, 0, 0x80);
    }
    if (c->x[4] && boot_nvn_peek(c, self, c->x[4], objs + 0x80 * k, 0x80) != 0x80) memset(objs + 0x80 * k, 0, 0x80);
    r->block[1].arg = 3; r->block[1].bytes = (16 + 0x80) * (k + 1); r->block[1].data = views;
    r->blocks = 2;
    return 1;
}
static inline int boot_nvn_record_build(BootContext *c, uint64_t self, int op, BootNvnRecord *r, uint8_t *scratch, size_t cap) {
    memset(r, 0, sizeof *r); r->op = (uint32_t)op;
    memcpy(r->x, c->x, 64);
    for (unsigned k = 0; k < 4; ++k) memcpy(&r->q[k], c->q[k], 4);
    static int rts = -2; if (rts == -2) rts = boot_nvn_name_index("nvnCommandBufferSetRenderTargets");
    if (op == rts) {
        if (!boot_nvn_record_render_targets(c, self, r, scratch, cap)) { boot_nvn_fail(c, self, "record argument memory too large", 0); return 0; }
        size_t bytes = BOOT_NVN_RECORD_FIXED;
        for (unsigned k = 0; k < r->blocks; ++k) bytes += 8 + ((r->block[k].bytes + 3u) & ~3u);
        r->words = (uint32_t)(bytes / 4);
        return 1;
    }
    const BootNvnTraceSpec *s = &boot_nvn_trace()->spec[op]; size_t used = 0;
    for (unsigned k = 0; k < 2; ++k) if (s->arg[k] >= 0 && c->x[s->arg[k]]) {
        uint64_t n = s->count_arg[k] >= 0 ? c->x[s->count_arg[k]] * (s->size[k] ? s->size[k] : 1) : s->size[k];
        if (n > 65536 || used + n > cap) { boot_nvn_fail(c, self, "record argument memory too large", n); return 0; }
        if (n) n = boot_nvn_peek(c, self, c->x[s->arg[k]], scratch + used, (size_t)n);   /* fixed-size object reads may end early */
        r->block[r->blocks].arg = (uint32_t)s->arg[k]; r->block[r->blocks].bytes = (uint32_t)n; r->block[r->blocks].data = scratch + used;
        ++r->blocks; used += (size_t)n;
    }
    {   /* flashfix: CopyTextureToBuffer (texture x1, view x2) and CopyBufferToTexture (texture x2, view x3): the view's first 16
         * bytes and the texture object's first 0x80 as they are now (block arg 0x80 | texture argument, after the region),
         * as SetRenderTargets does: the driver bakes the image address at record time, the views are game temporaries and the
         * game re-initializes ping-pong texture objects before the submit. Zero bytes = unreadable (read at the submit). */
        static int t2b = -2, b2t = -2;
        if (t2b == -2) t2b = boot_nvn_name_index("nvnCommandBufferCopyTextureToBuffer");
        if (b2t == -2) b2t = boot_nvn_name_index("nvnCommandBufferCopyBufferToTexture");
        unsigned ta = op == t2b ? 1u : op == b2t ? 2u : 0u;
        if (ta && r->blocks == 1 && c->x[ta] && used + 16 + 0x80 <= cap && boot_nvn_copy_capture_on()) {
            uint8_t *v = scratch + used; memset(v, 0, 16 + 0x80);
            if (c->x[ta + 1] && boot_nvn_peek(c, self, c->x[ta + 1], v, 16) != 16) memset(v, 0, 16);
            if (boot_nvn_peek(c, self, c->x[ta], v + 16, 0x80) != 0x80) memset(v + 16, 0, 0x80);
            r->block[1].arg = 0x80u | ta; r->block[1].bytes = 16 + 0x80; r->block[1].data = v; r->blocks = 2; used += 16 + 0x80;
        }
    }
    size_t bytes = BOOT_NVN_RECORD_FIXED;
    for (unsigned k = 0; k < r->blocks; ++k) bytes += 8 + ((r->block[k].bytes + 3u) & ~3u);
    r->words = (uint32_t)(bytes / 4);
    return 1;
}
/* reclock: the whole record in one append (it used to be 3-5 stores, each under the exclusive store lock). */
static inline int boot_nvn_record_write(uint64_t slot, uint32_t gen, const BootNvnRecord *r, uint32_t *gen_out, const BootNvnDrefAdd *dref) {
    uint8_t head[BOOT_NVN_RECORD_FIXED]; uint32_t h = (r->op & 0xffff) | (r->words << 16), bh[2][2];
    memcpy(head, &h, 4); memcpy(head + 4, &r->x[1], 56); memcpy(head + 60, r->q, 16);
    BootNvnPart part[1 + 2 * 2]; unsigned n = 0;
    part[n].p = head; part[n++].n = sizeof head;
    for (unsigned k = 0; k < r->blocks && k < 2; ++k) {
        bh[k][0] = r->block[k].arg; bh[k][1] = r->block[k].bytes;
        part[n].p = bh[k]; part[n++].n = 8;
        part[n].p = r->block[k].data; part[n++].n = r->block[k].bytes;
    }
    return boot_nvn_slot_append(slot, gen, part, n, 0, gen_out, dref);
}
/* ---- call-bind: record time, switches, log (see "call-bind" above) */
/* Record time (boot_nvn_cmd_observe), a CallCommands of n handles (its index entries h): each handle's recording now gets a binding
 * and is named in out[k] (slot | generation; 0: no recording yet) and in the record (block BOOT_NVN_CB_ARG). Returns n when any was
 * bound (the caller passes out to the record's index and calls boot_nvn_cb_unbind if that index is not stored), else 0 (the record
 * unchanged: resolved at the submit as before). */
static inline unsigned boot_nvn_cb_bind(BootNvnRecord *r, const uint64_t *h, uint32_t n, uint64_t *out) {
    unsigned got = 0;
    if (!boot_nvn_cb_mode() || !n || n > 64 || r->blocks != 1) return 0;
    InterlockedIncrement64(&boot_nvn_cb.calls);
    if (boot_nvn_cb.held_bytes > boot_nvn_cb_budget()) { InterlockedExchangeAdd64(&boot_nvn_cb.budget_skip, n); return 0; }
    BootNvnShadowStore *s = boot_nvn_shadow(); const int v2 = boot_nvn_slv2_on();
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_BIND, v2);   /* storelock-work: shared with v2 (other recorders bind the same callees: atomic) */
    for (uint32_t k = 0; k < n; ++k) {
        const uint64_t sh = h[k] && !(h[k] & BOOT_NVN_SHADOW) ? boot_nvn_hmap_get(boot_nvn_handles(), h[k]) : 0; const uint32_t i = boot_nvn_slot_of(sh);
        BootNvnSlot *t = sh && i < s->count ? boot_nvn_slot_at(s, i) : NULL;
        out[k] = 0;
        if (!t || t->next_free != UINT32_MAX || t->dead || !t->closed) continue;
        if (v2) __atomic_add_fetch(&t->cb_refs, 1, __ATOMIC_RELAXED); else { ++t->cb_refs; ++boot_nvn_cb.refs; }
        out[k] = sh | t->gen; ++got;
        if (!(__atomic_load_n(&t->ev_reach, __ATOMIC_RELAXED) & BOOT_NVN_EV_CALLED)) __atomic_or_fetch(&t->ev_reach, BOOT_NVN_EV_CALLED, __ATOMIC_RELAXED);   /* rec-evict: called */
    }
    if (v2 && got) __atomic_add_fetch(&boot_nvn_cb.refs, got, __ATOMIC_RELAXED);
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_BIND, v2, sl);
    InterlockedExchangeAdd64(&boot_nvn_cb.binds, got); InterlockedExchangeAdd64(&boot_nvn_cb.unbound_rec, n - got);
    if (!got) return 0;
    r->block[1].arg = BOOT_NVN_CB_ARG; r->block[1].bytes = 8 * n; r->block[1].data = (const uint8_t *)out; r->blocks = 2;
    r->words += (8 + 8 * n) / 4;
    return n;
}
static inline void boot_nvn_cb_unbind(const uint64_t *b, uint32_t n) {   /* bindings the record's index does not hold (not stored) */
    BootNvnShadowStore *s = boot_nvn_shadow();
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_UNBIND, 0);
    for (uint32_t k = 0; k < n; ++k) if (b[k]) boot_nvn_cb_unref_locked(s, b[k]);
    boot_nvn_cb_drain_locked(s);
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_UNBIND, 0, sl);
    InterlockedIncrement64(&boot_nvn_cb.bind_lost);
}
/* Switched to 0: every binding goes (the versions only bindings kept are freed). In chunks of 4096 slots under the store lock; a
 * record keeps naming its versions, but in mode 0 they are ignored and later checked by generation. Returns the bindings released. */
static inline uint64_t boot_nvn_cb_unbind_all(void) {
    BootNvnShadowStore *s = boot_nvn_shadow(); uint64_t n = 0;
    for (uint32_t base = 0;; base += 4096) {
        const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_UNBIND, 0);
        const uint32_t end = s->count - base < 4096 ? s->count : base + 4096;
        for (uint32_t i = base; i < end; ++i) {
            BootNvnSlot *t = boot_nvn_slot_at(s, i);
            if (!t->dref_n) continue;
            AcquireSRWLockExclusive(&t->lock);
            for (uint32_t k = 0; k < t->dref_n; ++k) if (t->dref[k].count == UINT32_MAX && t->dref[k].bound) { boot_nvn_cb_unref_locked(s, t->dref[k].bound); t->dref[k].bound = 0; ++n; }
            ReleaseSRWLockExclusive(&t->lock);
        }
        boot_nvn_cb_drain_locked(s);
        const int more = end < s->count;
        boot_nvn_sl_unlock(s, BOOT_NVN_SL_UNBIND, 0, sl);
        if (!more) break;
    }
    return n;
}
static inline void boot_nvn_cb_fallback_log(uint64_t h, uint64_t b, unsigned why) {
#ifdef TOTK_DRAW_PATH
    if (InterlockedIncrement(&boot_nvn_cb.logged_fb) > 16) return;
    char line[300]; snprintf(line, sizeof line, " (call-bind, TOTK_CALL_BIND_AT_RECORD %d): handle %llx, bound version slot %u gen %u %s; played through the handle (present %u)",
                             boot_nvn_cb_mode(), (unsigned long long)h, boot_nvn_slot_of(b), (uint32_t)b,
                             why == BOOT_NVN_CB_R_INVALID ? "was released by rec-evict class 1 (its command memory written over: invalid in NVN too)" :
                             why == BOOT_NVN_CB_R_INPLACE ? "was recorded again over its own commands (the console runs the new bytes)" : "is gone",
                             (unsigned)boot_nvn_ev_now);
    boot_draw_log("nvn native: call-bind fallback%s %llu %llu", line, (uint64_t)boot_nvn_cb.ex_invalid, (uint64_t)boot_nvn_cb.ex_gone);
#else
    (void)h; (void)b; (void)why;
#endif
}
/* Every present (boot_nvn_ev_present, renderer lock held, no submit running): the live switch frames\callbind-mode.request (one digit
 * 0 / 1 / 2; applied from the next submit on, the file deleted, the change logged), and every 300 presents the "call bind" line. */
static inline void boot_nvn_cb_report(uint32_t now) {
#ifdef TOTK_DRAW_PATH
    static uint64_t l_div, l_binds, l_inv, l_gone, l_ev;
    const int m = boot_nvn_cb_mode(); char line[1200];
    const uint64_t div = (uint64_t)boot_nvn_cb.ex_diverged, binds = (uint64_t)boot_nvn_cb.binds, inv = (uint64_t)boot_nvn_cb.ex_invalid, gone = (uint64_t)boot_nvn_cb.ex_gone, ev = (uint64_t)boot_nvn_cb.watch_events;
    snprintf(line, sizeof line, " (call-bind; TOTK_CALL_BIND_AT_RECORD %d: %s; budget %llu MiB): CallCommands handles bound at record %llu last 300 presents, %llu in all"
             " (not bound: no recording yet %llu, held versions over the budget %llu, index lost %llu); translated calls: same version %llu,"
             " callee re-recorded before its caller's submit %llu last 300 presents, %llu in all (%s), fallbacks: bound version released by class 1 %llu, recorded again in place %llu (marked %llu), gone %llu"
             " (last 300: %llu, %llu), unbound %llu, ignored (mode 0) %llu; versions held alive by callers now %llu (%llu MiB), peak %llu (%llu MiB), markers %llu,"
             " bindings %llu (bad releases %llu, lost %llu); watch events %llu last 300 (logged %llu, with differences %llu, over the per-second limit %llu); switches %llu"
             " (bindings released by switches %llu)",
             m, m == 1 ? "bound versions played" : m == 2 ? "watch: live versions played, differences logged" : "off", (unsigned long long)(boot_nvn_cb_budget() >> 20),
             (unsigned long long)(binds - l_binds), (unsigned long long)binds, (unsigned long long)boot_nvn_cb.unbound_rec, (unsigned long long)boot_nvn_cb.budget_skip,
             (unsigned long long)boot_nvn_cb.bind_lost, (unsigned long long)boot_nvn_cb.ex_same, (unsigned long long)(div - l_div), (unsigned long long)div,
             m == 1 ? "played as recorded" : "played live", (unsigned long long)inv, (unsigned long long)boot_nvn_cb.ex_inplace, (unsigned long long)boot_nvn_cb.inplace, (unsigned long long)gone, (unsigned long long)(inv - l_inv), (unsigned long long)(gone - l_gone),
             (unsigned long long)boot_nvn_cb.ex_unbound, (unsigned long long)boot_nvn_cb.ex_off, (unsigned long long)boot_nvn_cb.held_n,
             (unsigned long long)(boot_nvn_cb.held_bytes >> 20), (unsigned long long)boot_nvn_cb.held_peak_n, (unsigned long long)(boot_nvn_cb.held_peak_bytes >> 20),
             (unsigned long long)boot_nvn_cb.tombs, (unsigned long long)boot_nvn_cb.refs, (unsigned long long)boot_nvn_cb.unref_bad, (unsigned long long)boot_nvn_cb.reap_lost,
             (unsigned long long)(ev - l_ev), (unsigned long long)boot_nvn_cb.watch_logged, (unsigned long long)boot_nvn_cb.watch_diffs, (unsigned long long)boot_nvn_cb.watch_over,
             (unsigned long long)boot_nvn_cb.switches, (unsigned long long)boot_nvn_cb.released_by_switch);
    boot_draw_log("nvn native: call bind%s present %llu %llu", line, now, 0);
    l_div = div; l_binds = binds; l_inv = inv; l_gone = gone; l_ev = ev;
#else
    (void)now;
#endif
}
static inline void boot_nvn_cb_present(uint32_t now) {
    if (GetFileAttributesA("frames\\callbind-mode.request") != INVALID_FILE_ATTRIBUTES) {
        int want = -1; FILE *q = fopen("frames\\callbind-mode.request", "r");
        if (q) { int ch = fgetc(q); while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == 0xef || ch == 0xbb || ch == 0xbf) ch = fgetc(q); if (ch >= '0' && ch <= '2') want = ch - '0'; fclose(q); }
        DeleteFileA("frames\\callbind-mode.request");
        const int was = boot_nvn_cb_mode(); uint64_t dropped = 0;
        if (want >= 0 && want != was) {
            InterlockedExchange(&boot_nvn_cb_mode_v, want); InterlockedIncrement64(&boot_nvn_cb.switches);
            if (!want) { dropped = boot_nvn_cb_unbind_all(); InterlockedExchangeAdd64(&boot_nvn_cb.released_by_switch, (LONG64)dropped); }
        }
        char line[200]; snprintf(line, sizeof line, " (frames\\callbind-mode.request): TOTK_CALL_BIND_AT_RECORD %d -> %d at present %u%s (bindings released %llu; held versions now %llu)",
                                 was, want < 0 ? was : want, now, want < 0 ? ", not a digit 0-2: ignored" : want == was ? ", unchanged" : "",
                                 (unsigned long long)dropped, (unsigned long long)boot_nvn_cb.held_n);
#ifdef TOTK_DRAW_PATH
        boot_draw_log("nvn native: call-bind mode switch%s %llu %llu", line, now, 0);
#endif
        boot_nvn_log(line, "call-bind", now);
    }
    if (!(now % 300)) boot_nvn_cb_report(now);
}
/* ---------------------------------------------------------------- rec-evict: record store eviction (play-work/rec-evict) */
/* The host copy of every recording (the slots above) was freed only when the driver returned the same handle again. TotK records
 * most lists into memory it hands out afresh (each model's lists into its own heap, the measuring passes into one shared 4 MiB
 * block), so the store kept nearly everything ever recorded: play-96 4.7 GiB (+0.6 GiB/min exploring), the play-97 dry run 145558
 * live recordings / 4450 MiB after 8 minutes, enough to drive this 16 GB PC to its commit limit. mem-work batch A (TOTK_REC_KILL)
 * assumed a handle is its command memory address plus a constant and never proved it ("proven linear 0 / not 405137"); this
 * replaces it.
 *
 * The driver's handle, decoded from the translated driver (sdk+0x444530 nvnCommandBufferEndRecording; the CallCommands worker
 * sdk+0x44561c decodes it the same way) and checked against the stage-0 trace (nvn-trace.bin: all 140936 EndRecording returns lie
 * in the control block the game added, at the offsets below). The driver's NVNcommandBuffer keeps (guest CPU addresses; +0x58 is the
 * command block's GPU address, from the pool's +0x18; the CPU one is the pool's storage, pool+0x30):
 *   +0x00 command write pointer   +0x18 command block end - reserve   +0x30 command block start   +0x40 recording's segment start
 *   +0x20 control write pointer   +0x28 control block end - 0x10      +0x38 control block start   +0x48 recording's control start
 * BeginRecording writes a 16-byte header {0xffffffffffffff01, 0} (sdk+0xa9dd90) at the control write pointer; EndRecording appends
 * the recording's command segments as control entries (an 8-byte group header, then {GPU address, CPU address, words, type 3} per
 * segment) and an end entry (type 2), and returns ctl_start + 0x10, or, for one group of segments (count < 65536), the packed form
 * (count << 48) | (ctl_start + 0x18) | 1. A handle is an address in CONTROL memory (bits 0-47; bit 0 = packed, bits 48-63 = segment
 * count): play-97's sample "handle 2007400151291, begin 7400151278" is ctl_start 0x7400151278 with two segments, and batch A's
 * cmd+0x20 was the control write pointer.
 *
 * Class 1 (LIST_DEAD in nxport's terms; nxport, MIT, crates/engine/src/nvn/api.rs: COMMAND_LISTS 2195-2215, kill_overlapping_lists
 * 2361-2400, command_list_recorded 2420-2432): a recording whose COMMAND memory a newer recording has written (with its commands
 * or its control entries) is invalid in NVN, and so is every list that called it: CallCommands copies the callee's control
 * entries, i.e. GPU pointers into the callee's command memory, into the caller when the caller is recorded (sdk+0x44561c walks
 * them). Nothing can validly execute it again, so it is freed at once (one a submission has pinned: marked dead, freed at its
 * unpin, as a replaced recording). Not freed:
 *  - a recording whose CONTROL memory alone was written over: its handle is dead for new submits and calls, but a caller recorded
 *    before holds the copied entries and stays valid, and the executor resolves a caller's callees by handle at the submit. In the
 *    trace the per-frame list 7604339030 reached 8256 callees whose control memory had been reused between its CallCommands and
 *    its submit (a ring of per-frame control memory): freeing those would drop their draws. Counted ("control only").
 *  - AddCommandMemory / AddControlMemory over its range: nothing is written (nxport: TotK re-adds a block to re-record its first
 *    list and keeps calling the list after it in the same block).
 * Also class 1: the recordings whose commands lie in a memory pool the game finalizes (nvnMemoryPoolFinalize, observed while class 1
 * is on): that memory is no longer mapped for the GPU.
 * Replaying this rule over the trace (play-work/rec-evict/sim.py): 28909 recordings freed, none used again, no submitted list
 * reaching a freed callee.
 * Mechanics: BeginRecording (before the driver) notes the command write pointer; EndRecording's return reads the command buffer
 * and takes [that pointer, +0x00) as the command range and [ctl_start from the handle, +0x20) as the control range, each checked
 * against its block (+0x30..+0x18, +0x38..+0x28), plus the header and the end entry in guest memory. A layout surprise, an
 * AddCommandMemory / AddControlMemory while recording (the out-of-memory callback: two blocks), a range over 16 MiB or no
 * BeginRecording seen leaves the recording untracked: it frees nothing and is only freed as before. An index of pages (4 KiB for
 * ranges up to 64 KiB, 1 MiB above) finds the recordings whose command range a new one overlaps.
 *
 * Class 2 (TOTK_REC_EVICT; nxport's 512 MiB budget, LIST_SUBMITTED): while the store is over TOTK_REC_BUDGET_MB (512), recordings
 * that submit walks only ever reached as submitted handles (never through CallCommands: nxport's per-mesh lists, whose plain-LRU
 * eviction drew meshes with the previous draw's vertex buffers, as geometry streaks) and that no submission reached for
 * TOTK_REC_EVICT_AGE presents (600) are freed oldest first, down to 3/4 of the budget, at most once per 30 presents (300 after a
 * sweep that found nothing to take), on the presenting thread. Never: a pinned recording, one no walk reached yet (LIST_NEW), one called through CallCommands (with
 * TOTK_REC_EVICT=1 also marked when a CallCommands naming it is recorded).
 *
 * A released handle used again (submitted, or reached through CallCommands) cannot be played. The executor used to stop the game at
 * "bad record"; now its commands are skipped (boot_nvn_ev_skip), counted per class, and the first 16 are logged with their history
 * ("REC-EVICT released handle used again"). In a dry run nothing is released; a marked recording reached again by a submission is
 * counted and logged ("would-be-released ... reached again"): that boot shows whether switching the class on is safe.
 *
 * Switches: TOTK_REC_EVICT_DEAD 1 (default) class 1 on, 2 dry run, 0 off (no tracking at all). TOTK_REC_EVICT 2 (default) dry run,
 * 1 on, 0 off. TOTK_REC_BUDGET_MB (512), TOTK_REC_EVICT_AGE (600 presents). Both 0: the store exactly as before (but for the skip of
 * a released handle, which then never happens). Every 300 presents a draw-path.log line "record store eviction": live recordings
 * and MiB by class, freed / would free per class (last 300 presents and in all), released handles used again, tracking and index
 * counters. nvn-native.log gets the first 24 tracked recordings ("rec evict sample"), the first 8 untracked ones and the first 8
 * class 1 releases ("rec evict release"). TOTK_REC_KILL (batch A) is gone; set, it is logged once. */
static inline int boot_nvn_ev_env(const char *name, int def) {   /* 0, 1 or 2 from the first character; def when unset or other */
    char s[8]; DWORD k = GetEnvironmentVariableA(name, s, sizeof s);
    if (!k || k >= sizeof s) return def;
    return s[0] == '0' ? 0 : s[0] == '1' ? 1 : s[0] == '2' ? 2 : def;
}
static inline uint64_t boot_nvn_ev_env_u(const char *name, uint64_t def, uint64_t max) {
    char s[16]; DWORD k = GetEnvironmentVariableA(name, s, sizeof s); uint64_t v = def;
    if (k && k < sizeof s && s[0] >= '0' && s[0] <= '9') { v = 0; for (DWORD i = 0; i < k && s[i] >= '0' && s[i] <= '9' && v <= max; ++i) v = v * 10 + (uint64_t)(s[i] - '0'); }
    return v > max ? max : v;
}
static inline int boot_nvn_ev_dead_mode(void) { static int v = -1; if (v < 0) v = boot_nvn_ev_env("TOTK_REC_EVICT_DEAD", 1); return v; }
static inline int boot_nvn_ev_lru_mode(void) { static int v = -1; if (v < 0) v = boot_nvn_ev_env("TOTK_REC_EVICT", 2); return v; }
static inline int boot_nvn_ev_on(void) { return boot_nvn_ev_dead_mode() || boot_nvn_ev_lru_mode(); }
static inline uint64_t boot_nvn_ev_budget(void) { static int64_t v = -1; if (v < 0) v = (int64_t)(boot_nvn_ev_env_u("TOTK_REC_BUDGET_MB", 512, 65536) << 20); return (uint64_t)v; }
static inline uint32_t boot_nvn_ev_age(void) { static int64_t v = -1; if (v < 0) v = (int64_t)boot_nvn_ev_env_u("TOTK_REC_EVICT_AGE", 600, UINT32_C(1) << 30); return (uint32_t)v; }
#define BOOT_NVN_EV_MAX (UINT64_C(16) << 20)                      /* a longer range is not tracked */
#define BOOT_NVN_EV_SMALL (UINT64_C(64) << 10)                    /* ranges up to this are indexed by 4 KiB page, longer ones by 1 MiB page */
#define BOOT_NVN_EV_GONE_MAX (UINT32_C(1) << 19)                  /* released handles remembered (a 16 MiB map); then the memory starts over */
#define BOOT_NVN_EV_CTL_HEADER UINT64_C(0xffffffffffffff01)       /* BeginRecording's first control word (sdk+0xa9dd90) */
#define BOOT_NVN_EV_BATCH 256u                                    /* recordings released per pass */
enum { BOOT_NVN_EV_DEAD = 1, BOOT_NVN_EV_EVICT = 2 };             /* classes; the released-handle memory holds class | present << 8 */
enum { BOOT_NVN_EV_U_NO_BEGIN, BOOT_NVN_EV_U_SWITCHED, BOOT_NVN_EV_U_OBJECT, BOOT_NVN_EV_U_CONTROL, BOOT_NVN_EV_U_COMMAND, BOOT_NVN_EV_U_N };   /* why untracked */
typedef struct BootNvnEvPage { uint64_t key; uint32_t *s; uint32_t n, cap; } BootNvnEvPage;   /* key: (page << 1 | level) + 1, 0 = empty; s: slot indices */
static struct {
    SRWLOCK lock;   /* the index and every slot's ev_handle / ranges / ev_in. Order: walk lock, store lock, this, map locks */
    BootNvnEvPage *tab; uint32_t cap, used; uint64_t entries, index_bytes, index_lost;
    volatile LONG64 ended, tracked, with_cmd, untracked[BOOT_NVN_EV_U_N];
    volatile LONG64 dead_n, dead_bytes, dead_pinned, dead_skipped, ctl_only, would_dead_n, would_dead_bytes, pools, pool_lists;
    volatile LONG64 lru_n, lru_bytes, would_lru_n, would_lru_bytes, sweeps, sweep_qpc;
    volatile LONG64 virt_dead, virt_lru;   /* dry runs: bytes of marked recordings still live (taken off the budget's count) */
    volatile LONG64 reuse_walk[3], reuse_skip, reuse_unknown, would_reuse[3], would_reach, gone_clears;
    volatile LONG logged_use, logged_would, logged_skip, logged_rel, samples, samples_bad, released_any;   /* released_any: set before the first real release */
    uint32_t next_sweep;   /* present of the next sweep: +30 after one that found candidates, +300 after one that found none */
} boot_nvn_ev;   /* zero: SRWLOCK_INIT */
static inline BootNvnHostMap *boot_nvn_ev_begins(void) { static BootNvnHostMap m = {SRWLOCK_INIT, NULL, NULL, 0, 0}; return &m; }   /* cmd -> command write pointer at BeginRecording | 1 (| 2: a block added since), while it records */
static inline BootNvnHostMap *boot_nvn_ev_gone(void) { static BootNvnHostMap m = {SRWLOCK_INIT, NULL, NULL, 0, 0}; return &m; }     /* released handle -> class | present << 8 */
/* The control memory address a driver handle names (see above), and where its recording's control entries start. */
static inline uint64_t boot_nvn_ev_ctl_start(uint64_t h) { uint64_t a = h & UINT64_C(0x0000fffffffffffe); return (h & 1) ? a - 0x18 : a - 0x10; }
static inline const char *boot_nvn_ev_class_name(unsigned cls) { return cls == BOOT_NVN_EV_DEAD ? "dead: command memory overwritten" : cls == BOOT_NVN_EV_EVICT ? "budget: submitted only, unused" : "?"; }
static inline void boot_nvn_ev_note(const char *fmt, ...) {   /* one line to nvn-native.log */
    FILE *f = fopen("nvn-native.log", "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
/* ---- the overwrite index (ev lock held) */
static inline unsigned boot_nvn_ev_shift(unsigned level) { return level ? 20u : 12u; }
static inline uint64_t boot_nvn_ev_key(uint64_t page, unsigned level) { return (page << 1 | level) + 1; }
static inline BootNvnEvPage *boot_nvn_ev_page_locked(uint64_t key, int create) {
    if (boot_nvn_ev.cap) for (uint32_t j = boot_nvn_hmap_hash(key, boot_nvn_ev.cap); boot_nvn_ev.tab[j].key; j = (j + 1) & (boot_nvn_ev.cap - 1))
        if (boot_nvn_ev.tab[j].key == key) return &boot_nvn_ev.tab[j];
    if (!create) return NULL;
    if ((boot_nvn_ev.used + 1) * 4 > boot_nvn_ev.cap * 3) {   /* load <= 3/4 */
        uint32_t cap = boot_nvn_ev.cap ? boot_nvn_ev.cap * 2 : 4096;
        BootNvnEvPage *t = (BootNvnEvPage *)calloc(cap, sizeof *t);
        if (!t) return NULL;
        for (uint32_t i = 0; i < boot_nvn_ev.cap; ++i) if (boot_nvn_ev.tab[i].key) {
            uint32_t j = boot_nvn_hmap_hash(boot_nvn_ev.tab[i].key, cap); while (t[j].key) j = (j + 1) & (cap - 1);
            t[j] = boot_nvn_ev.tab[i];
        }
        boot_nvn_ev.index_bytes += (uint64_t)(cap - boot_nvn_ev.cap) * sizeof *t;
        free(boot_nvn_ev.tab); boot_nvn_ev.tab = t; boot_nvn_ev.cap = cap;
    }
    uint32_t j = boot_nvn_hmap_hash(key, boot_nvn_ev.cap); while (boot_nvn_ev.tab[j].key) j = (j + 1) & (boot_nvn_ev.cap - 1);
    BootNvnEvPage *p = &boot_nvn_ev.tab[j]; p->key = key; p->s = NULL; p->n = p->cap = 0; ++boot_nvn_ev.used;
    return p;
}
static inline void boot_nvn_ev_page_drop_locked(BootNvnEvPage *p) {   /* an empty page leaves the table (backward-shift deletion) */
    const uint32_t mask = boot_nvn_ev.cap - 1; uint32_t j = (uint32_t)(p - boot_nvn_ev.tab);
    boot_nvn_ev.index_bytes -= (uint64_t)p->cap * sizeof *p->s; free(p->s);
    p->key = 0; p->s = NULL; p->n = p->cap = 0; --boot_nvn_ev.used;
    for (uint32_t i = (j + 1) & mask; boot_nvn_ev.tab[i].key; i = (i + 1) & mask) {
        uint32_t home = boot_nvn_hmap_hash(boot_nvn_ev.tab[i].key, boot_nvn_ev.cap);
        if (((i - home) & mask) >= ((i - j) & mask)) {
            boot_nvn_ev.tab[j] = boot_nvn_ev.tab[i]; boot_nvn_ev.tab[i].key = 0; boot_nvn_ev.tab[i].s = NULL; boot_nvn_ev.tab[i].n = boot_nvn_ev.tab[i].cap = 0; j = i;
        }
    }
}
static inline int boot_nvn_ev_add_locked(uint32_t i, uint64_t a, uint64_t len) {   /* slot i on every page of [a, a+len); 0: out of host memory */
    if (!len) return 1;
    const unsigned lv = len > BOOT_NVN_EV_SMALL, sh = boot_nvn_ev_shift(lv);
    for (uint64_t pg = a >> sh, last = (a + len - 1) >> sh; pg <= last; ++pg) {
        BootNvnEvPage *p = boot_nvn_ev_page_locked(boot_nvn_ev_key(pg, lv), 1);
        if (!p) return 0;
        if (p->n == p->cap) {
            uint32_t cap = p->cap ? p->cap * 2 : 4; uint32_t *x = (uint32_t *)realloc(p->s, (size_t)cap * sizeof *x);
            if (!x) return 0;
            boot_nvn_ev.index_bytes += (uint64_t)(cap - p->cap) * sizeof *x; p->s = x; p->cap = cap;
        }
        p->s[p->n++] = i; ++boot_nvn_ev.entries;
    }
    return 1;
}
static inline void boot_nvn_ev_del_locked(uint32_t i, uint64_t a, uint64_t len) {   /* slot i off the pages of [a, a+len) (absent: nothing) */
    if (!len) return;
    const unsigned lv = len > BOOT_NVN_EV_SMALL, sh = boot_nvn_ev_shift(lv);
    for (uint64_t pg = a >> sh, last = (a + len - 1) >> sh; pg <= last; ++pg) {
        BootNvnEvPage *p = boot_nvn_ev_page_locked(boot_nvn_ev_key(pg, lv), 0);
        if (!p) continue;
        for (uint32_t k = 0; k < p->n; ) if (p->s[k] == i) { p->s[k] = p->s[--p->n]; --boot_nvn_ev.entries; } else ++k;
        if (!p->n) boot_nvn_ev_page_drop_locked(p);
    }
}
static inline void boot_nvn_ev_unindex_locked(BootNvnSlot *t, uint32_t i) {
    if (!t->ev_in) return;
    boot_nvn_ev_del_locked(i, t->ev_ctl, t->ev_ctl_len); boot_nvn_ev_del_locked(i, t->ev_cmd, t->ev_cmd_len); t->ev_in = 0;
}
/* Store lock held (boot_nvn_slot_free_locked): the slot is released or marked dead, its handle already gone. */
static inline void boot_nvn_ev_forget_locked(BootNvnSlot *t, uint32_t i) {
    uint8_t m = __atomic_exchange_n(&t->ev_mark, 0, __ATOMIC_RELAXED);
    if (m & BOOT_NVN_EV_WOULD_DEAD) InterlockedExchangeAdd64(&boot_nvn_ev.virt_dead, -(LONG64)t->cap);
    if (m & BOOT_NVN_EV_WOULD_EVICT) InterlockedExchangeAdd64(&boot_nvn_ev.virt_lru, -(LONG64)t->cap);
    if (!boot_nvn_ev_on()) return;
    if (!TryAcquireSRWLockExclusive(&boot_nvn_ev.lock)) {   /* always (ev_in is written under this lock by another thread's install) */
        const uint64_t q0 = boot_nvn_qpc(); AcquireSRWLockExclusive(&boot_nvn_ev.lock);   /* storelock-work: counted (the store lock is held meanwhile) */
        InterlockedIncrement64(&boot_nvn_sl_stats.ev_waits); InterlockedExchangeAdd64(&boot_nvn_sl_stats.ev_wait_qpc, (LONG64)(boot_nvn_qpc() - q0));
    }
    boot_nvn_ev_unindex_locked(t, i);
    __atomic_store_n(&t->ev_handle, 0, __ATOMIC_RELAXED); t->ev_reach = 0;   /* no longer installed (the census and the sweep read these) */
    ReleaseSRWLockExclusive(&boot_nvn_ev.lock);
}
static inline void boot_nvn_hmap_clear(BootNvnHostMap *m) {
    boot_nvn_rl_lock(&m->lock, BOOT_NVN_RL_MAP);
    if (m->cap) { memset(m->key, 0, (size_t)m->cap * sizeof *m->key); memset(m->val, 0, (size_t)m->cap * sizeof *m->val); }
    m->used = 0;
    ReleaseSRWLockExclusive(&m->lock);
}
/* k removed only while it still maps to v (an EndRecording on another thread may have installed a newer recording). 1: removed. */
static inline int boot_nvn_hmap_remove_if(BootNvnHostMap *m, uint64_t k, uint64_t v) {
    int done = 0; if (!k || !v) return 0;
    boot_nvn_rl_lock(&m->lock, BOOT_NVN_RL_MAP);
    if (m->cap) {
        uint32_t j = boot_nvn_hmap_hash(k, m->cap);
        while (m->key[j] && m->key[j] != k) j = (j + 1) & (m->cap - 1);
        if (m->key[j] == k && m->val[j] == v) {   /* backward-shift deletion, as boot_nvn_hmap_put */
            m->key[j] = 0; m->val[j] = 0; --m->used; done = 1;
            for (uint32_t i = (j + 1) & (m->cap - 1); m->key[i]; i = (i + 1) & (m->cap - 1)) {
                uint32_t home = boot_nvn_hmap_hash(m->key[i], m->cap);
                if (((i - home) & (m->cap - 1)) >= ((i - j) & (m->cap - 1))) {
                    m->key[j] = m->key[i]; m->val[j] = m->val[i]; m->key[i] = 0; m->val[i] = 0; j = i;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&m->lock);
    return done;
}
static inline void boot_nvn_ev_remember(uint64_t h, unsigned cls) {   /* a released handle, for "used again" */
    BootNvnHostMap *g = boot_nvn_ev_gone();
    if (g->used >= BOOT_NVN_EV_GONE_MAX) { boot_nvn_hmap_clear(g); InterlockedIncrement64(&boot_nvn_ev.gone_clears); }
    if (boot_nvn_hmap_put(g, h, cls | (uint64_t)(uint32_t)boot_nvn_ev_now << 8) == UINT64_MAX) boot_nvn_hmap_put(g, h, 0);
}
/* Release (or, dry, mark) slot i if it still is the recording of handle h with generation gen. Takes the store lock (walk lock
 * shared may be held). 1 released, 2 released while a submission reads it (freed at its unpin), 3 marked (dry run), 0 skipped
 * (no longer that recording; or pinned, for class 2). *bytes: its buffer. */
static inline int boot_nvn_ev_release(uint32_t i, uint32_t gen, uint64_t h, unsigned cls, int dry, uint64_t *bytes) {
    BootNvnShadowStore *s = boot_nvn_shadow(); const uint64_t sh = BOOT_NVN_SHADOW | (uint64_t)i << 32; int r = 0;
    *bytes = 0;
    const uint64_t sl = boot_nvn_sl_lock(s, BOOT_NVN_SL_EVICT, 0);
    BootNvnSlot *t = i < s->count ? boot_nvn_slot_at(s, i) : NULL;
    if (t && t->next_free == UINT32_MAX && !t->dead && t->gen == gen && t->closed && __atomic_load_n(&t->ev_handle, __ATOMIC_RELAXED) == h &&
        !(cls == BOOT_NVN_EV_EVICT && (t->pins || t->cb_refs))) {   /* call-bind: a version a caller is bound to is a called list */
        if (dry) {
            if (!(__atomic_load_n(&t->ev_mark, __ATOMIC_RELAXED) & (BOOT_NVN_EV_WOULD_DEAD | BOOT_NVN_EV_WOULD_EVICT)) && boot_nvn_hmap_get(boot_nvn_handles(), h) == sh) {
                *bytes = t->cap; r = 3;
                __atomic_or_fetch(&t->ev_mark, cls == BOOT_NVN_EV_DEAD ? BOOT_NVN_EV_WOULD_DEAD : BOOT_NVN_EV_WOULD_EVICT, __ATOMIC_RELAXED);
                InterlockedExchangeAdd64(cls == BOOT_NVN_EV_DEAD ? &boot_nvn_ev.virt_dead : &boot_nvn_ev.virt_lru, (LONG64)t->cap);
                if (cls == BOOT_NVN_EV_DEAD) {   /* counted once: out of the index (a would-be-evicted one stays: class 1 may still free it) */
                    AcquireSRWLockExclusive(&boot_nvn_ev.lock); boot_nvn_ev_unindex_locked(t, i); ReleaseSRWLockExclusive(&boot_nvn_ev.lock);
                }
            }
        } else {
            if (!boot_nvn_ev.released_any) InterlockedExchange(&boot_nvn_ev.released_any, 1);   /* before the first map removal (boot_nvn_ev_skip) */
            if (boot_nvn_hmap_remove_if(boot_nvn_handles(), h, sh)) {
                if (cls == BOOT_NVN_EV_DEAD) t->cb_flags |= BOOT_NVN_CB_INVALID;   /* call-bind: invalid in NVN: a caller bound to it falls back to the handle */
                *bytes = t->cap; r = boot_nvn_slot_free_locked(s, i) ? 2 : 1;
            }
        }
    }
    boot_nvn_sl_unlock(s, BOOT_NVN_SL_EVICT, 0, sl);   /* (v2: then its bindings) */
    if (r == 1 || r == 2) boot_nvn_ev_remember(h, cls);
    return r;
}
/* ---- recording hooks (boot_nvn_cmd_observe, boot_nvn_cmd_end_ret) */
static inline void boot_nvn_ev_begin(BootContext *c, uint64_t self, uint64_t cmd) {   /* BeginRecording, before the driver */
    uint64_t w = 0;
    if (!cmd) return;
    int ok = boot_nvn_peek(c, self, cmd, &w, sizeof w) == sizeof w && !(w & 3);
    if (boot_nvn_hmap_put(boot_nvn_ev_begins(), cmd, ok ? w | 1 : 0) == UINT64_MAX) boot_nvn_hmap_put(boot_nvn_ev_begins(), cmd, 0);
}
static inline void boot_nvn_ev_switch(uint64_t cmd) {   /* AddCommandMemory / AddControlMemory (before the driver): a block switch while recording? */
    if (!cmd || !boot_nvn_ev_begins()->used) return;
    uint64_t v = boot_nvn_hmap_get(boot_nvn_ev_begins(), cmd);
    if (v && !(v & 2)) boot_nvn_hmap_put(boot_nvn_ev_begins(), cmd, v | 2);
}
static inline void boot_nvn_ev_reset(uint64_t cmd) { if (cmd && boot_nvn_ev_begins()->used) boot_nvn_hmap_put(boot_nvn_ev_begins(), cmd, 0); }   /* Initialize / Finalize */
/* EndRecording's return, before the install: the recording's control and command ranges (zero lengths: untracked). */
static inline void boot_nvn_ev_ranges(BootContext *c, uint64_t self, uint64_t cmd, uint64_t h, BootNvnEvRange *rg) {
    memset(rg, 0, sizeof *rg);
    const uint64_t v = cmd && boot_nvn_ev_begins()->used ? boot_nvn_hmap_put(boot_nvn_ev_begins(), cmd, 0) : 0;   /* taken: cmd no longer records */
    if (!h || !boot_nvn_ev_dead_mode()) return;
    InterlockedIncrement64(&boot_nvn_ev.ended);
    uint64_t w[10] = {0}, hd[2] = {0, 0}; const uint64_t ctl_s = boot_nvn_ev_ctl_start(h), cmd_s = v & ~UINT64_C(3); uint64_t ctl_e = 0, cmd_e = 0; int why = -1;
    if (!v) why = BOOT_NVN_EV_U_NO_BEGIN;
    else if (v & 2) why = BOOT_NVN_EV_U_SWITCHED;
    else if (boot_nvn_peek(c, self, cmd, w, sizeof w) != sizeof w || w[9] != w[4]) why = BOOT_NVN_EV_U_OBJECT;   /* +0x48 = +0x20 after EndRecording */
    else {
        ctl_e = w[4];
        if (!(w[7] <= ctl_s && ctl_s + 0x18 <= ctl_e && ctl_e <= w[5] + 0x10 && ctl_e - ctl_s <= BOOT_NVN_EV_MAX) ||
            boot_nvn_peek(c, self, ctl_s, &hd[0], 8) != 8 || hd[0] != BOOT_NVN_EV_CTL_HEADER ||
            boot_nvn_peek(c, self, ctl_e - 8, &hd[1], 8) != 8 || (hd[1] & 0xff) != 2) why = BOOT_NVN_EV_U_CONTROL;
        else if ((cmd_e = w[0]) != cmd_s && !(cmd_s && w[6] <= cmd_s && cmd_s < cmd_e && cmd_e <= w[3] + 0x400 && cmd_e - cmd_s <= BOOT_NVN_EV_MAX))
            why = BOOT_NVN_EV_U_COMMAND;
    }
    if (why >= 0) {
        InterlockedIncrement64(&boot_nvn_ev.untracked[why]);
        if (InterlockedIncrement(&boot_nvn_ev.samples_bad) <= 8)
            boot_nvn_ev_note("nvn native: rec evict untracked (%s): cmd %llx handle %llx begin %llx object %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx control words %llx %llx",
                             why == BOOT_NVN_EV_U_NO_BEGIN ? "no BeginRecording seen" : why == BOOT_NVN_EV_U_SWITCHED ? "block added while recording" : why == BOOT_NVN_EV_U_OBJECT ? "command buffer layout" :
                             why == BOOT_NVN_EV_U_CONTROL ? "control layout" : "command range", (unsigned long long)cmd, (unsigned long long)h, (unsigned long long)v,
                             (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5],
                             (unsigned long long)w[6], (unsigned long long)w[7], (unsigned long long)w[8], (unsigned long long)w[9], (unsigned long long)hd[0], (unsigned long long)hd[1]);
        return;
    }
    rg->ctl = ctl_s; rg->ctl_len = (uint32_t)(ctl_e - ctl_s);
    if (cmd_e > cmd_s) { rg->cmd = cmd_s; rg->cmd_len = (uint32_t)(cmd_e - cmd_s); InterlockedIncrement64(&boot_nvn_ev.with_cmd); }
    InterlockedIncrement64(&boot_nvn_ev.tracked);
    if (InterlockedIncrement(&boot_nvn_ev.samples) <= 24)
        boot_nvn_ev_note("nvn native: rec evict sample (TOTK_REC_EVICT_DEAD %d): cmd %llx handle %llx (%s, %llu segments) control %llx +%x (block %llx..%llx) commands %llx +%x (block %llx..%llx)",
                         boot_nvn_ev_dead_mode(), (unsigned long long)cmd, (unsigned long long)h, (h & 1) ? "packed" : "plain", (unsigned long long)((h & 1) ? h >> 48 : 0),
                         (unsigned long long)rg->ctl, rg->ctl_len, (unsigned long long)w[7], (unsigned long long)(w[5] + 0x10), (unsigned long long)rg->cmd, rg->cmd_len,
                         (unsigned long long)w[6], (unsigned long long)w[3]);
}
/* ev lock held: the indexed recordings (not slot self) on page p whose COMMAND range overlaps [a, e), added to vi / vg / vh (*n, no
 * duplicates); *ctl_only counts entries whose control range alone overlaps. 1: the batch is full. */
static inline int boot_nvn_ev_scan_page_locked(const BootNvnEvPage *p, uint64_t a, uint64_t e, uint32_t self, uint32_t *vi, uint32_t *vg, uint64_t *vh, unsigned *n, unsigned *ctl_only) {
    BootNvnShadowStore *s = boot_nvn_shadow();
    for (uint32_t k = 0; k < p->n; ++k) {
        const uint32_t j = p->s[k]; const BootNvnSlot *u = j == self ? NULL : boot_nvn_slot_peek(s, j);
        if (!u) continue;
        if (!(u->ev_cmd_len && u->ev_cmd < e && a < u->ev_cmd + u->ev_cmd_len)) {   /* control memory only: kept (see above) */
            if (u->ev_ctl_len && u->ev_ctl < e && a < u->ev_ctl + u->ev_ctl_len) ++*ctl_only;
            continue;
        }
        unsigned d = 0; while (d < *n && vi[d] != j) ++d;
        if (d < *n) continue;
        if (*n == BOOT_NVN_EV_BATCH) return 1;
        vi[*n] = j; vg[*n] = u->gen; vh[*n] = __atomic_load_n(&u->ev_handle, __ATOMIC_RELAXED); ++*n;
    }
    return 0;
}
/* ev lock held: the recordings whose command range overlaps [a, a+len), at both page sizes (a range with more pages than the table
 * has entries, a whole memory pool, walks the table instead). 1: the batch is full (release these, then ask again). */
static inline int boot_nvn_ev_collect_locked(uint64_t a, uint64_t len, uint32_t self, uint32_t *vi, uint32_t *vg, uint64_t *vh, unsigned *n, unsigned *ctl_only) {
    if (!len) return 0;
    const uint64_t e = a + len;
    for (unsigned lv = 0; lv < 2; ++lv) {
        const unsigned sh = boot_nvn_ev_shift(lv); const uint64_t p0 = a >> sh, p1 = (e - 1) >> sh;
        if (p1 - p0 >= (uint64_t)boot_nvn_ev.cap) {
            for (uint32_t it = 0; it < boot_nvn_ev.cap; ++it) {
                const BootNvnEvPage *p = &boot_nvn_ev.tab[it]; const uint64_t key = p->key - 1;
                if (p->key && (key & 1) == lv && (key >> 1) >= p0 && (key >> 1) <= p1 && boot_nvn_ev_scan_page_locked(p, a, e, self, vi, vg, vh, n, ctl_only)) return 1;
            }
        } else for (uint64_t pg = p0; pg <= p1; ++pg) {
            const BootNvnEvPage *p = boot_nvn_ev_page_locked(boot_nvn_ev_key(pg, lv), 0);
            if (p && boot_nvn_ev_scan_page_locked(p, a, e, self, vi, vg, vh, n, ctl_only)) return 1;
        }
    }
    return 0;
}
/* Class 1 releases of a batch found by boot_nvn_ev_collect_locked (no ev lock held; the walk lock shared may be). by: the new
 * recording's handle (rg its ranges), or the finalized pool (rg NULL), for the log. */
static inline void boot_nvn_ev_release_dead(const uint32_t *vi, const uint32_t *vg, const uint64_t *vh, unsigned n, uint64_t by, const BootNvnEvRange *rg) {
    const int dry = boot_nvn_ev_dead_mode() == 2;
    for (unsigned k = 0; k < n; ++k) {
        uint64_t b; const int r = boot_nvn_ev_release(vi[k], vg[k], vh[k], BOOT_NVN_EV_DEAD, dry, &b);
        if (r == 1 || r == 2) { InterlockedIncrement64(&boot_nvn_ev.dead_n); InterlockedExchangeAdd64(&boot_nvn_ev.dead_bytes, (LONG64)b); if (r == 2) InterlockedIncrement64(&boot_nvn_ev.dead_pinned); }
        else if (r == 3) { InterlockedIncrement64(&boot_nvn_ev.would_dead_n); InterlockedExchangeAdd64(&boot_nvn_ev.would_dead_bytes, (LONG64)b); }
        else InterlockedIncrement64(&boot_nvn_ev.dead_skipped);
        if (r && InterlockedIncrement(&boot_nvn_ev.logged_rel) <= 8) {
            const char *how = r == 3 ? "dry run, would free: " : r == 2 ? "pinned, freed at its unpin: " : "";
            if (rg) boot_nvn_ev_note("nvn native: rec evict release (%s%s): handle %llx (slot %u, %llu bytes) written over by handle %llx (control %llx +%x, commands %llx +%x)",
                                     how, boot_nvn_ev_class_name(BOOT_NVN_EV_DEAD), (unsigned long long)vh[k], vi[k], (unsigned long long)b, (unsigned long long)by,
                                     (unsigned long long)rg->ctl, rg->ctl_len, (unsigned long long)rg->cmd, rg->cmd_len);
            else boot_nvn_ev_note("nvn native: rec evict release (%s%s): handle %llx (slot %u, %llu bytes), its memory pool %llx finalized",
                                  how, boot_nvn_ev_class_name(BOOT_NVN_EV_DEAD), (unsigned long long)vh[k], vi[k], (unsigned long long)b, (unsigned long long)by);
        }
    }
}
/* Inside boot_nvn_handle_install_ev (walk lock shared when TOTK_SNAP_ATOMIC): handle -> slot is installed. The recordings whose
 * command range this one's ranges overlap are released (class 1) and this one is indexed; with class 2 only, its handle is noted. */
static inline void boot_nvn_ev_installed(uint64_t handle, uint64_t slot, const BootNvnEvRange *rg) {
    BootNvnShadowStore *s = boot_nvn_shadow(); const uint32_t self = boot_nvn_slot_of(slot); BootNvnSlot *t = boot_nvn_slot_peek(s, self);
    if (!t) return;
    if (boot_nvn_ev_gone()->used) boot_nvn_hmap_put(boot_nvn_ev_gone(), handle, 0);   /* issued again: a valid handle */
    const int track = boot_nvn_ev_dead_mode() && (rg->ctl_len || rg->cmd_len);
    uint32_t vi[BOOT_NVN_EV_BATCH], vg[BOOT_NVN_EV_BATCH]; uint64_t vh[BOOT_NVN_EV_BATCH];
    for (unsigned pass = 0; pass < 64; ++pass) {
        unsigned n = 0, ctl_only = 0; int more = 0;
        AcquireSRWLockExclusive(&boot_nvn_ev.lock);
        if (boot_nvn_hmap_get(boot_nvn_handles(), handle) != slot) { ReleaseSRWLockExclusive(&boot_nvn_ev.lock); return; }   /* replaced or released meanwhile */
        if (!pass) __atomic_store_n(&t->ev_handle, handle, __ATOMIC_RELAXED);
        if (track) {
            more = boot_nvn_ev_collect_locked(rg->ctl, rg->ctl_len, self, vi, vg, vh, &n, &ctl_only) || boot_nvn_ev_collect_locked(rg->cmd, rg->cmd_len, self, vi, vg, vh, &n, &ctl_only);
            if (!more && !t->ev_in) {   /* indexed once, after the recordings it overwrote are out */
                t->ev_ctl = rg->ctl; t->ev_ctl_len = rg->ctl_len; t->ev_cmd = rg->cmd; t->ev_cmd_len = rg->cmd_len;
                if (boot_nvn_ev_add_locked(self, rg->ctl, rg->ctl_len) && boot_nvn_ev_add_locked(self, rg->cmd, rg->cmd_len)) t->ev_in = 1;
                else { boot_nvn_ev_del_locked(self, rg->ctl, rg->ctl_len); boot_nvn_ev_del_locked(self, rg->cmd, rg->cmd_len); ++boot_nvn_ev.index_lost; }
            }
        }
        ReleaseSRWLockExclusive(&boot_nvn_ev.lock);
        if (ctl_only) InterlockedExchangeAdd64(&boot_nvn_ev.ctl_only, ctl_only);
        boot_nvn_ev_release_dead(vi, vg, vh, n, handle, rg);
        if (!more) return;
    }
}
/* nvnMemoryPoolFinalize, before the driver (hybrid observe, registered NOLOCK: no renderer lock; the walk / store / index locks are
 * taken here as an EndRecording's return takes them): every recording with commands in the pool's storage
 * is dead in NVN (its commands are no longer mapped): class 1. The pool's CPU storage and size are pool+0x30 / +0x38 (stage-0 trace
 * dumps of nvnMemoryPoolInitialize: storage 760845d000 size 400000 for pool 760885d060). In the trace TotK finalized no pool that had
 * held command memory (loading only); streaming areas out may. */
static inline void boot_nvn_ev_pool_final(BootContext *c, uint64_t self, uint64_t pool) {
    uint64_t w[2] = {0, 0};
    if (!boot_nvn_ev_dead_mode() || !pool || boot_nvn_peek(c, self, pool + 0x30, w, sizeof w) != sizeof w || !w[0] || !w[1] || w[1] > (UINT64_C(16) << 30) || w[0] + w[1] < w[0]) return;
    InterlockedIncrement64(&boot_nvn_ev.pools);
    const int atomic = boot_nvn_snap_atomic_on(); const uint64_t d0 = (uint64_t)boot_nvn_ev.dead_n + (uint64_t)boot_nvn_ev.would_dead_n;
    if (atomic) AcquireSRWLockShared(&boot_nvn_walk_lock);   /* as an EndRecording's map change */
    uint32_t vi[BOOT_NVN_EV_BATCH], vg[BOOT_NVN_EV_BATCH]; uint64_t vh[BOOT_NVN_EV_BATCH];
    for (unsigned pass = 0; pass < 65536; ++pass) {
        unsigned n = 0, ctl_only = 0;
        AcquireSRWLockExclusive(&boot_nvn_ev.lock);
        const int more = boot_nvn_ev_collect_locked(w[0], w[1], UINT32_MAX, vi, vg, vh, &n, &ctl_only);
        ReleaseSRWLockExclusive(&boot_nvn_ev.lock);
        boot_nvn_ev_release_dead(vi, vg, vh, n, pool, NULL);
        if (!more) break;
    }
    if (atomic) ReleaseSRWLockShared(&boot_nvn_walk_lock);
    InterlockedExchangeAdd64(&boot_nvn_ev.pool_lists, (LONG64)((uint64_t)boot_nvn_ev.dead_n + (uint64_t)boot_nvn_ev.would_dead_n - d0));
}
/* TOTK_REC_EVICT=1: a CallCommands naming these handles was recorded: they are never budget candidates. */
static inline void boot_nvn_ev_called(const uint64_t *h, uint32_t n) {
    BootNvnShadowStore *s = boot_nvn_shadow();
    for (uint32_t k = 0; k < n; ++k) {
        uint64_t sh = h[k] ? boot_nvn_hmap_get(boot_nvn_handles(), h[k]) : 0;
        BootNvnSlot *t = sh ? boot_nvn_slot_peek(s, boot_nvn_slot_of(sh)) : NULL;   /* a slot reused meanwhile only gets the safe mark */
        if (t && !(__atomic_load_n(&t->ev_reach, __ATOMIC_RELAXED) & BOOT_NVN_EV_CALLED)) __atomic_or_fetch(&t->ev_reach, BOOT_NVN_EV_CALLED, __ATOMIC_RELAXED);
    }
}
/* ---- "used again" */
static inline void boot_nvn_ev_would_reused(BootNvnSlot *t, uint32_t i, uint32_t depth, uint8_t m) {   /* the walk (slot lock held) */
    InterlockedIncrement64(&boot_nvn_ev.would_reach);
    if (m & BOOT_NVN_EV_REUSED) return;
    __atomic_or_fetch(&t->ev_mark, BOOT_NVN_EV_REUSED, __ATOMIC_RELAXED);
    const unsigned cls = (m & BOOT_NVN_EV_WOULD_DEAD) ? BOOT_NVN_EV_DEAD : BOOT_NVN_EV_EVICT;
    InterlockedIncrement64(&boot_nvn_ev.would_reuse[cls]);
#ifdef TOTK_DRAW_PATH
    if (InterlockedIncrement(&boot_nvn_ev.logged_would) <= 16) {
        char line[400]; snprintf(line, sizeof line, " (rec-evict dry run, %s): handle %llx (slot %u, %u bytes, last reached at present %u, now %u) reached %s (depth %u);"
                                 " with the class on its commands would be skipped", boot_nvn_ev_class_name(cls), (unsigned long long)t->ev_handle, i, t->cap, t->ev_use,
                                 (unsigned)boot_nvn_ev_now, depth ? "through CallCommands" : "as a submitted handle", depth);
        boot_draw_log("nvn native: REC-EVICT would-be-released recording reached again%s %llu %llu", line, (uint64_t)boot_nvn_ev.would_reuse[cls], (uint64_t)boot_nvn_ev.would_reach);
    }
#else
    (void)i; (void)depth;
#endif
}
static inline void boot_nvn_ev_reach(BootNvnSlot *t, uint32_t i, uint32_t depth) {   /* the submit walk reached slot i (slot lock held) */
    const uint8_t r = depth ? BOOT_NVN_EV_CALLED : BOOT_NVN_EV_SUBMITTED, m = __atomic_load_n(&t->ev_mark, __ATOMIC_RELAXED);
    t->ev_use = (uint32_t)boot_nvn_ev_now;
    if ((__atomic_load_n(&t->ev_reach, __ATOMIC_RELAXED) & r) != r) __atomic_or_fetch(&t->ev_reach, r, __ATOMIC_RELAXED);
    if (m & (BOOT_NVN_EV_WOULD_DEAD | BOOT_NVN_EV_WOULD_EVICT)) boot_nvn_ev_would_reused(t, i, depth, m);
}
/* The submit walk could not resolve handle h: a handle rec-evict released? */
static inline void boot_nvn_ev_missing(uint64_t h, uint32_t depth) {
    BootNvnHostMap *g = boot_nvn_ev_gone(); uint64_t v;
    if (!g->used || !(v = boot_nvn_hmap_get(g, h))) return;
    const unsigned cls = (unsigned)(v & 0xff) == BOOT_NVN_EV_EVICT ? BOOT_NVN_EV_EVICT : BOOT_NVN_EV_DEAD; const uint32_t at = (uint32_t)(v >> 8);
    InterlockedIncrement64(&boot_nvn_ev.reuse_walk[cls]);
#ifdef TOTK_DRAW_PATH
    if (InterlockedIncrement(&boot_nvn_ev.logged_use) <= 16) {
        char line[400]; snprintf(line, sizeof line, " (rec-evict, %s): handle %llx (control %llx, %s, %llu segments), released at present %u (%u presents ago), reached %s (depth %u)"
                                 " by the submit call; its commands are skipped (the frame misses them)", boot_nvn_ev_class_name(cls), (unsigned long long)h,
                                 (unsigned long long)boot_nvn_ev_ctl_start(h), (h & 1) ? "packed" : "plain", (unsigned long long)((h & 1) ? h >> 48 : 0), at, (uint32_t)boot_nvn_ev_now - at,
                                 depth ? "through CallCommands" : "as a submitted handle", depth);
        boot_draw_log("nvn native: REC-EVICT released handle used again%s %llu %llu", line, (uint64_t)boot_nvn_ev.reuse_walk[cls], 0);
    }
#else
    (void)depth; (void)at;
#endif
}
/* The executor met handle h that no longer resolves (it would stop the game: "bad record"). 1: skip its commands (rec-evict has
 * released recordings this boot, so it is one of those, remembered or not); 0: the old behaviour. */
static inline int boot_nvn_ev_skip(uint64_t h, unsigned depth) {
    if (!__atomic_load_n(&boot_nvn_ev.released_any, __ATOMIC_SEQ_CST)) return 0;   /* nothing was ever released: not ours (set before the first map removal) */
    BootNvnHostMap *g = boot_nvn_ev_gone(); const uint64_t v = g->used ? boot_nvn_hmap_get(g, h) : 0;
    InterlockedIncrement64(v ? &boot_nvn_ev.reuse_skip : &boot_nvn_ev.reuse_unknown);
#ifdef TOTK_DRAW_PATH
    if (InterlockedIncrement(&boot_nvn_ev.logged_skip) <= 16) {
        char line[300]; snprintf(line, sizeof line, " (rec-evict, %s): handle %llx at depth %u, present %u: its commands are skipped", v ? boot_nvn_ev_class_name((unsigned)(v & 0xff)) :
                                 "not remembered (released before the memory of released handles started over, or not ours)", (unsigned long long)h, depth, (unsigned)boot_nvn_ev_now);
        boot_draw_log("nvn native: REC-EVICT unresolvable handle skipped in the translation%s %llu %llu", line, (uint64_t)boot_nvn_ev.reuse_skip, (uint64_t)boot_nvn_ev.reuse_unknown);
    }
#else
    (void)depth;
#endif
    return 1;
}
/* ---- class 2: the budget sweep (presenting thread) */
static inline int boot_nvn_ev_candidate(const BootNvnSlot *t, uint32_t now, uint32_t age, uint32_t *idle) {   /* read without locks; boot_nvn_ev_release checks again */
    if (t->next_free != UINT32_MAX || t->dead || !t->closed || t->pins || t->cb_refs || !__atomic_load_n(&t->ev_handle, __ATOMIC_RELAXED)) return 0;
    if (__atomic_load_n(&t->ev_reach, __ATOMIC_RELAXED) != BOOT_NVN_EV_SUBMITTED || (__atomic_load_n(&t->ev_mark, __ATOMIC_RELAXED) & (BOOT_NVN_EV_WOULD_DEAD | BOOT_NVN_EV_WOULD_EVICT))) return 0;
    const uint32_t a = now - t->ev_use;
    if (a < age || a > (UINT32_C(1) << 31)) return 0;
    *idle = a - age; return 1;
}
static inline unsigned boot_nvn_ev_bucket(uint32_t x) { unsigned b = 0; while (x) { ++b; x >>= 1; } return b; }   /* 0..32 */
static inline int64_t boot_nvn_ev_effective(void) { return (int64_t)boot_nvn_shadow()->bytes - boot_nvn_ev.virt_dead - boot_nvn_ev.virt_lru; }
static inline int boot_nvn_ev_sweep(uint32_t now) {   /* 1: there were candidates */
    BootNvnShadowStore *s = boot_nvn_shadow(); const uint64_t budget = boot_nvn_ev_budget(); const uint32_t age = boot_nvn_ev_age(); const int dry = boot_nvn_ev_lru_mode() == 2;
    const int64_t eff = boot_nvn_ev_effective();
    if (eff <= (int64_t)budget) return 1;
    const uint64_t need = (uint64_t)eff - budget / 4 * 3; uint64_t hist[33] = {0}, sum = 0, freed = 0; uint32_t idle; unsigned cut = 33;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    const uint32_t n = __atomic_load_n(&s->count, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; ++i) { const BootNvnSlot *t = boot_nvn_slot_peek(s, i); if (t && boot_nvn_ev_candidate(t, now, age, &idle)) hist[boot_nvn_ev_bucket(idle)] += t->cap; }
    while (cut > 0 && sum < need) sum += hist[--cut];   /* the oldest buckets that cover the excess */
    if (sum) {
        const int atomic = boot_nvn_snap_atomic_on();
        if (atomic) AcquireSRWLockShared(&boot_nvn_walk_lock);   /* as an EndRecording's map change: a submit walk sees the map at one instant */
        for (uint32_t i = 0; i < n && freed < need; ++i) {
            const BootNvnSlot *t = boot_nvn_slot_peek(s, i);
            if (!t || !boot_nvn_ev_candidate(t, now, age, &idle) || boot_nvn_ev_bucket(idle) < cut) continue;
            uint64_t b; const int r = boot_nvn_ev_release(i, t->gen, __atomic_load_n(&t->ev_handle, __ATOMIC_RELAXED), BOOT_NVN_EV_EVICT, dry, &b);
            if (r == 1 || r == 2) { InterlockedIncrement64(&boot_nvn_ev.lru_n); InterlockedExchangeAdd64(&boot_nvn_ev.lru_bytes, (LONG64)b); freed += b; }
            else if (r == 3) { InterlockedIncrement64(&boot_nvn_ev.would_lru_n); InterlockedExchangeAdd64(&boot_nvn_ev.would_lru_bytes, (LONG64)b); freed += b; }
        }
        if (atomic) ReleaseSRWLockShared(&boot_nvn_walk_lock);
    }
    QueryPerformanceCounter(&q1);
    InterlockedIncrement64(&boot_nvn_ev.sweeps); InterlockedExchangeAdd64(&boot_nvn_ev.sweep_qpc, q1.QuadPart - q0.QuadPart);
    return sum != 0;
}
/* ---- every present (boot_nvn_queue_present) */
static inline void boot_nvn_ev_report(uint32_t now) {
#ifdef TOTK_DRAW_PATH
    static uint64_t l_dn, l_db, l_wdn, l_wdb, l_ln, l_lb, l_wln, l_wlb;
    BootNvnShadowStore *s = boot_nvn_shadow(); uint64_t cn[4] = {0}, cb[4] = {0}, vn = 0, vb = 0, open = 0;   /* new, submitted only, called; versions */
    const uint32_t n = __atomic_load_n(&s->count, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; ++i) {   /* census, read without locks (approximate) */
        const BootNvnSlot *t = boot_nvn_slot_peek(s, i);
        if (!t || t->next_free != UINT32_MAX) continue;
        if (t->dead) { ++vn; vb += t->cap; continue; }
        if (!t->closed) { if (t->len) ++open; continue; }   /* a recording in progress (a free slot at the bottom of the free list also has next_free UINT32_MAX) */
        if (!__atomic_load_n(&t->ev_handle, __ATOMIC_RELAXED)) continue;
        const uint8_t r = __atomic_load_n(&t->ev_reach, __ATOMIC_RELAXED); const unsigned k = (r & BOOT_NVN_EV_CALLED) ? 2 : (r & BOOT_NVN_EV_SUBMITTED) ? 1 : 0;
        ++cn[k]; cb[k] += t->cap;
    }
    const int dm = boot_nvn_ev_dead_mode(), lm = boot_nvn_ev_lru_mode(); LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const uint64_t dn = (uint64_t)boot_nvn_ev.dead_n, db = (uint64_t)boot_nvn_ev.dead_bytes, wdn = (uint64_t)boot_nvn_ev.would_dead_n, wdb = (uint64_t)boot_nvn_ev.would_dead_bytes;
    const uint64_t ln = (uint64_t)boot_nvn_ev.lru_n, lb = (uint64_t)boot_nvn_ev.lru_bytes, wln = (uint64_t)boot_nvn_ev.would_lru_n, wlb = (uint64_t)boot_nvn_ev.would_lru_bytes;
    const uint64_t c1n = dm == 2 ? wdn : dn, c1b = dm == 2 ? wdb : db, p1n = dm == 2 ? l_wdn : l_dn, p1b = dm == 2 ? l_wdb : l_db;
    const uint64_t c2n = lm == 2 ? wln : ln, c2b = lm == 2 ? wlb : lb, p2n = lm == 2 ? l_wln : l_ln, p2b = lm == 2 ? l_wlb : l_lb;
    uint32_t pages; uint64_t entries, ibytes;
    AcquireSRWLockShared(&boot_nvn_ev.lock); pages = boot_nvn_ev.used; entries = boot_nvn_ev.entries; ibytes = boot_nvn_ev.index_bytes; ReleaseSRWLockShared(&boot_nvn_ev.lock);
    char line[1900];
    snprintf(line, sizeof line, " (rec-evict; TOTK_REC_EVICT_DEAD %d%s, TOTK_REC_EVICT %d%s, budget %llu MiB, age %u presents): live recordings %u, %llu MiB in all (effective %lld MiB);"
             " by class: new %llu (%llu MiB), submitted only %llu (%llu MiB), called %llu (%llu MiB), versions %llu (%llu MiB), open %llu;"
             " class 1 dead (command memory written over): %s %llu (%llu MiB) last 300 presents, %llu (%llu MiB) in all, pinned at release %llu, gone meanwhile %llu,"
             " control memory only written over (kept) %llu, memory pools finalized %llu (their recordings %llu); class 2 budget (submitted only, unused %u presents): %s %llu (%llu MiB) last 300, %llu (%llu MiB) in all, sweeps %llu (%.1f ms);"
             " released handles used again: dead %llu, budget %llu (submit walk), skipped in the translation %llu (not remembered %llu);"
             " dry runs, marked recordings reached again: dead %llu, budget %llu (reaches %llu);"
             " recordings ended %llu: tracked %llu (with commands %llu), untracked: no BeginRecording %llu, block added while recording %llu, object layout %llu,"
             " control layout %llu, command range %llu; index %u pages, %llu entries, %llu KiB (lost %llu); released handles remembered %u (memory started over %llu)",
             dm, dm == 2 ? " dry run" : dm ? "" : " off", lm, lm == 2 ? " dry run" : lm ? "" : " off", (unsigned long long)(boot_nvn_ev_budget() >> 20), boot_nvn_ev_age(),
             s->live, (unsigned long long)((uint64_t)s->bytes >> 20), (long long)(boot_nvn_ev_effective() >> 20),
             (unsigned long long)cn[0], (unsigned long long)(cb[0] >> 20), (unsigned long long)cn[1], (unsigned long long)(cb[1] >> 20), (unsigned long long)cn[2],
             (unsigned long long)(cb[2] >> 20), (unsigned long long)vn, (unsigned long long)(vb >> 20), (unsigned long long)open,
             dm == 2 ? "would free" : "freed", (unsigned long long)(c1n - p1n), (unsigned long long)((c1b - p1b) >> 20), (unsigned long long)c1n, (unsigned long long)(c1b >> 20),
             (unsigned long long)boot_nvn_ev.dead_pinned, (unsigned long long)boot_nvn_ev.dead_skipped, (unsigned long long)boot_nvn_ev.ctl_only,
             (unsigned long long)boot_nvn_ev.pools, (unsigned long long)boot_nvn_ev.pool_lists, boot_nvn_ev_age(),
             lm == 2 ? "would free" : "freed", (unsigned long long)(c2n - p2n), (unsigned long long)((c2b - p2b) >> 20), (unsigned long long)c2n, (unsigned long long)(c2b >> 20),
             (unsigned long long)boot_nvn_ev.sweeps, (double)boot_nvn_ev.sweep_qpc * 1000.0 / (double)(f.QuadPart ? f.QuadPart : 1),
             (unsigned long long)boot_nvn_ev.reuse_walk[BOOT_NVN_EV_DEAD], (unsigned long long)boot_nvn_ev.reuse_walk[BOOT_NVN_EV_EVICT], (unsigned long long)boot_nvn_ev.reuse_skip,
             (unsigned long long)boot_nvn_ev.reuse_unknown, (unsigned long long)boot_nvn_ev.would_reuse[BOOT_NVN_EV_DEAD], (unsigned long long)boot_nvn_ev.would_reuse[BOOT_NVN_EV_EVICT],
             (unsigned long long)boot_nvn_ev.would_reach, (unsigned long long)boot_nvn_ev.ended, (unsigned long long)boot_nvn_ev.tracked, (unsigned long long)boot_nvn_ev.with_cmd,
             (unsigned long long)boot_nvn_ev.untracked[BOOT_NVN_EV_U_NO_BEGIN], (unsigned long long)boot_nvn_ev.untracked[BOOT_NVN_EV_U_SWITCHED],
             (unsigned long long)boot_nvn_ev.untracked[BOOT_NVN_EV_U_OBJECT], (unsigned long long)boot_nvn_ev.untracked[BOOT_NVN_EV_U_CONTROL],
             (unsigned long long)boot_nvn_ev.untracked[BOOT_NVN_EV_U_COMMAND], pages, (unsigned long long)entries, (unsigned long long)(ibytes >> 10),
             (unsigned long long)boot_nvn_ev.index_lost, boot_nvn_ev_gone()->used, (unsigned long long)boot_nvn_ev.gone_clears);
    boot_draw_log("nvn native: record store eviction%s present %llu %llu", line, now, 0);
    l_dn = dn; l_db = db; l_wdn = wdn; l_wdb = wdb; l_ln = ln; l_lb = lb; l_wln = wln; l_wlb = wlb;
#else
    (void)now;
#endif
}
/* storelock-work: every 300 presents (boot_nvn_ev_present) the "store lock" line, per present over the last 300 (both modes, the A/B
 * baseline): acquisitions (those held shared), failed try-acquires and their wait by thread role (and the three recording threads that
 * waited longest, by guest thread) and by site, the sampled hold time by site (average and max of every 64th acquisition of a thread,
 * us; the max over the window), the binding drops done after the exclusive sections and the versions they freed, and the ev lock
 * waits of releases (the store lock held meanwhile: a release that waits there makes every other user of the store lock wait). */
static inline void boot_nvn_sl_report(uint32_t now) {
#ifdef TOTK_DRAW_PATH
    static const char *const name[BOOT_NVN_SL_N] = {"pin", "bound pin", "bind", "unpin", "drop", "unpin free", "drop free", "new", "free", "evict", "overwrite", "unbind"};
    static uint64_t l_acq[BOOT_NVN_SL_N], l_w[BOOT_NVN_SL_N], l_wq[BOOT_NVN_SL_N], l_hn[BOOT_NVN_SL_N], l_hq[BOOT_NVN_SL_N], l_bw[65], l_bq[65], l_def, l_df, l_ew, l_ewq;
    uint64_t acq[BOOT_NVN_SL_N] = {0}, w[BOOT_NVN_SL_N] = {0}, wq[BOOT_NVN_SL_N] = {0}, hn[BOOT_NVN_SL_N] = {0}, hq[BOOT_NVN_SL_N] = {0}, hm[BOOT_NVN_SL_N] = {0};
    uint64_t bw[65], bq[65], rw[3] = {0, 0, 0}, rq[3] = {0, 0, 0}, topq[3] = {0, 0, 0}; unsigned top[3] = {65, 65, 65};
    for (unsigned t = 0; t < 65; ++t) {
        BootNvnRlStats *b = &boot_nvn_rl_slots[t]; bw[t] = bq[t] = 0;
        for (unsigned k = 0; k < BOOT_NVN_SL_N; ++k) {
            acq[k] += b->sl_acq[k]; w[k] += b->sl_wait[k]; wq[k] += b->sl_wait_qpc[k]; hn[k] += b->sl_hold_n[k]; hq[k] += b->sl_hold_qpc[k];
            if (b->sl_hold_max[k] > hm[k]) hm[k] = b->sl_hold_max[k];
            b->sl_hold_max[k] = 0;   /* per window (a racy reset: diagnostic only) */
            bw[t] += b->sl_wait[k]; bq[t] += b->sl_wait_qpc[k];
        }
        const uint64_t dw = bw[t] - l_bw[t], dq = bq[t] - l_bq[t];
        const unsigned r = b->sl_role == BOOT_NVN_SL_ROLE_RENDER ? 0 : b->sl_role == BOOT_NVN_SL_ROLE_RECORDER ? 1 : 2;
        rw[r] += dw; rq[r] += dq;
        if (r == 1 && dw) for (unsigned j = 0; j < 3; ++j) if (top[j] == 65 || dq > topq[j]) {
            for (unsigned m = 2; m > j; --m) { top[m] = top[m - 1]; topq[m] = topq[m - 1]; }
            top[j] = t; topq[j] = dq; break;
        }
    }
    unsigned render = UINT32_MAX; for (unsigned t = 0; t < 65; ++t) if (boot_nvn_rl_slots[t].sl_role == BOOT_NVN_SL_ROLE_RENDER) { render = boot_nvn_rl_slots[t].sl_guest; break; }
    const int v2 = boot_nvn_slv2_on(); const double per = 1.0 / 300.0, ms = 1000.0 / (double)boot_nvn_qpf(), us = 1e6 / (double)boot_nvn_qpf();
    uint64_t ta = 0, tsh = 0, tw = 0, twq = 0;
    for (unsigned k = 0; k < BOOT_NVN_SL_N; ++k) { ta += acq[k] - l_acq[k]; if (v2 && k <= BOOT_NVN_SL_DROP) tsh += acq[k] - l_acq[k]; tw += w[k] - l_w[k]; twq += wq[k] - l_wq[k]; }
    const uint64_t def = (uint64_t)boot_nvn_sl_stats.deferred, df = (uint64_t)boot_nvn_sl_stats.drop_frees, ew = (uint64_t)boot_nvn_sl_stats.ev_waits, ewq = (uint64_t)boot_nvn_sl_stats.ev_wait_qpc;
    char line[2400]; size_t at = 0;
#define BOOT_NVN_SL_CAT(...) do { if (at < sizeof line) { int k_ = snprintf(line + at, sizeof line - at, __VA_ARGS__); if (k_ > 0) at += (size_t)k_; } } while (0)
    BOOT_NVN_SL_CAT(" (storelock-work; TOTK_STORE_LOCK_V2 %s%s) last 300 presents, per present: taken %.0f (%.0f shared), waits %.1f (%.3f ms); by role: render t%d %.1f (%.3f ms),"
                    " recorders %.1f (%.3f ms", v2 ? "on" : "off", v2 ? ": pins, binds, unpins and binding drops shared" : "", (double)ta * per, (double)tsh * per,
                    (double)tw * per, (double)twq * per * ms, render == UINT32_MAX ? -1 : (int)render, (double)rw[0] * per, (double)rq[0] * per * ms, (double)rw[1] * per, (double)rq[1] * per * ms);
    for (unsigned j = 0; j < 3; ++j) if (top[j] < 65)
        BOOT_NVN_SL_CAT("%s t%u %.1f (%.3f ms)", j ? "," : ":", boot_nvn_rl_slots[top[j]].sl_guest, (double)(bw[top[j]] - l_bw[top[j]]) * per, (double)topq[j] * per * ms);
    BOOT_NVN_SL_CAT("), other %.1f (%.3f ms); by site (taken, waits, wait ms per present; hold us avg/max of every 64th):", (double)rw[2] * per, (double)rq[2] * per * ms);
    for (unsigned k = 0; k < BOOT_NVN_SL_N; ++k) if (acq[k] != l_acq[k])
        BOOT_NVN_SL_CAT(" %s %.1f %.2f %.3f %.2f/%.1f,", name[k], (double)(acq[k] - l_acq[k]) * per, (double)(w[k] - l_w[k]) * per, (double)(wq[k] - l_wq[k]) * per * ms,
                        hn[k] != l_hn[k] ? (double)(hq[k] - l_hq[k]) * us / (double)(hn[k] - l_hn[k]) : 0.0, (double)hm[k] * us);
    BOOT_NVN_SL_CAT(" binding drops after the section %llu (versions they freed %llu), ev lock waits inside the store lock %llu (%.3f ms) per present %.1f (%.3f ms)",
                    (unsigned long long)(def - l_def), (unsigned long long)(df - l_df), (unsigned long long)(ew - l_ew), (double)(ewq - l_ewq) * ms,
                    (double)(ew - l_ew) * per, (double)(ewq - l_ewq) * per * ms);
#undef BOOT_NVN_SL_CAT
    boot_draw_log("nvn native: store lock%s present %llu %llu", line, now, 0);
    memcpy(l_acq, acq, sizeof l_acq); memcpy(l_w, w, sizeof l_w); memcpy(l_wq, wq, sizeof l_wq); memcpy(l_hn, hn, sizeof l_hn); memcpy(l_hq, hq, sizeof l_hq);
    memcpy(l_bw, bw, sizeof l_bw); memcpy(l_bq, bq, sizeof l_bq); l_def = def; l_df = df; l_ew = ew; l_ewq = ewq;
#else
    (void)now;
#endif
}
static inline void boot_nvn_ev_present(void) {
    const uint32_t now = (uint32_t)InterlockedIncrement(&boot_nvn_ev_now);
#ifdef TOTK_DRAW_PATH
    boot_render_present_no = (LONG)now;   /* call-bind: the vbounds diagnostic lines carry it */
#endif
    boot_nvn_cb_present(now);   /* call-bind: the live switch and its 300-present line */
    if (!(now % 300)) boot_nvn_sl_report(now);   /* storelock-work */
    if (now == 1) { char k[8]; if (GetEnvironmentVariableA("TOTK_REC_KILL", k, sizeof k)) boot_nvn_log("TOTK_REC_KILL (mem-work batch A) is gone: see TOTK_REC_EVICT_DEAD / TOTK_REC_EVICT", "rec-evict", 0); }
    if (!boot_nvn_ev_on()) return;
    if (boot_nvn_ev_lru_mode() && (int32_t)(now - boot_nvn_ev.next_sweep) >= 0 && boot_nvn_ev_effective() > (int64_t)boot_nvn_ev_budget())
        boot_nvn_ev.next_sweep = now + (boot_nvn_ev_sweep(now) ? 30u : 300u);   /* over the budget with nothing to take (all called / new): look again later */
    if (!(now % 300)) boot_nvn_ev_report(now);
}
/* play (62): recordings whose handles lie in [lo, lo+size) are dead (their command memory is being reused):
 * removed from the handle map, their slots released (pinned ones at their unpin). Returns how many. */
static uint64_t boot_nvn_reuse_released;
static inline unsigned boot_nvn_handles_release_range(uint64_t lo, uint64_t size) {
    if (!size || lo + size < lo) return 0;
    BootNvnHostMap *m = boot_nvn_handles(); unsigned n = 0;
    for (;;) {   /* collect a batch under the shared lock, remove outside it */
        uint64_t keys[256]; unsigned k = 0;
        AcquireSRWLockShared(&m->lock);
        for (uint32_t i = 0; i < m->cap && k < 256; ++i) if (m->key[i] && m->key[i] - lo < size) keys[k++] = m->key[i];
        ReleaseSRWLockShared(&m->lock);
        for (unsigned j = 0; j < k; ++j) { uint64_t old = boot_nvn_hmap_put(m, keys[j], 0); if (old && old != UINT64_MAX) { boot_nvn_slot_free(old); ++n; } }
        if (k < 256) break;
    }
    boot_nvn_reuse_released += n;
    return n;
}
/* reclock: the recording this thread is appending to (command buffer -> slot and generation), so a record needs
 * no map lookup. Set by BeginRecording and by each record, dropped at EndRecording/Reset. It is only a hint: the
 * append checks under the slot lock that the slot still holds that generation and is not closed, and otherwise
 * the map decides, as it did for every record before (always with TOTK_RECLOCK=0). */
static _Thread_local struct { uint64_t cmd, slot; uint32_t gen; } boot_nvn_rec;
/* The slot of cmd's recording in progress, from the map; a record without BeginRecording starts one (before
 * nvnPrivate0 was cleared, the recording starts there). 0 after a failure (boot_nvn_fail already called). */
static inline uint64_t boot_nvn_rec_open(BootContext *c, uint64_t self, int op, uint64_t cmd) {
    BootNvnHostMap *slots = boot_nvn_cmd_slots(); uint64_t slot = boot_nvn_hmap_get(slots, cmd);
    if (!slot) {
        static LONG logged; if (!InterlockedExchange(&logged, 1)) boot_nvn_log("command recorded without BeginRecording", boot_nvn_names[op].name, cmd);
        if (!boot_nvn_slot_new(&slot)) { boot_nvn_fail(c, self, "out of host memory for records", cmd); return 0; }
        if (boot_nvn_hmap_put(slots, cmd, slot) == UINT64_MAX) { boot_nvn_slot_free(slot); boot_nvn_fail(c, self, "out of host memory for records", cmd); return 0; }
    }
    return slot;
}
/* Every nvnCommandBuffer* function (user = name index): host bookkeeping by kind, then the driver function. */
BOOT_NVN_FN(boot_nvn_cmd_observe) {
    int op = (int)(intptr_t)user; uint64_t real = boot_nvn_driver_fn(c, op), cmd = c->x[0];
    if (!real) return boot_nvn_fail(c, self, "driver function unknown", (uint64_t)op);
    BootNvnHostMap *slots = boot_nvn_cmd_slots();
    switch (boot_nvn_cmd_kinds()[op]) {
    case BOOT_NVN_CMD_ADD_MEMORY:   /* the driver would fault on it: name the call instead */
        if (!c->x[1]) return boot_nvn_fail(c, self, "NULL memory pool (the game's GPU memory allocation failed)", 0);
        if (boot_nvn_ev_dead_mode()) boot_nvn_ev_switch(cmd);   /* rec-evict: a block added while recording leaves that recording untracked */
        {   /* play (62): (cmd, pool, offset, size): recordings in that memory are dead */
            BootNvnPool p; uint64_t off = c->x[2], size = c->x[3];
            if (boot_nvn_peek(c, self, c->x[1], &p, sizeof p) == sizeof p && p.magic == BOOT_NVN_MAGIC_POOL && off < p.size) {
                if (size > p.size - off) size = p.size - off;
                if (p.storage) boot_nvn_handles_release_range(p.storage + off, size);
                if (p.va) boot_nvn_handles_release_range(p.va + off, size);
            } else { static LONG logged; if (!InterlockedExchange(&logged, 1)) boot_nvn_log("command memory reuse: pool not recognised (recordings kept)", "nvnCommandBufferAddCommandMemory", c->x[1]); }
        }
        break;
    case BOOT_NVN_CMD_ADD_CONTROL:   /* rec-evict: AddControlMemory, passed to the driver */
        if (boot_nvn_ev_dead_mode()) boot_nvn_ev_switch(cmd);
        break;
    case BOOT_NVN_CMD_RESET: {
        if (boot_nvn_ev_dead_mode()) boot_nvn_ev_reset(cmd);   /* rec-evict */
        uint64_t old = boot_nvn_hmap_put(slots, cmd, 0); if (old) boot_nvn_slot_free(old);
        if (boot_nvn_rec.cmd == cmd) boot_nvn_rec.cmd = 0;
        break; }
    case BOOT_NVN_CMD_BEGIN: {
        boot_nvn_inline_off(c, self);
        { BootNvnRlStats *m = boot_nvn_rl(); if (m->sl_role != BOOT_NVN_SL_ROLE_RENDER) { m->sl_role = BOOT_NVN_SL_ROLE_RECORDER; m->sl_guest = boot_nvn_guest_thread(c); } }   /* storelock-work */
        if (boot_nvn_ev_dead_mode()) boot_nvn_ev_begin(c, self, cmd);   /* rec-evict: the driver's command write pointer (cmd+0x00) before it starts */
        if (boot_nvn_rec.cmd == cmd) boot_nvn_rec.cmd = 0;
        uint64_t slot; uint32_t gen; if (!boot_nvn_slot_new_gen(&slot, &gen)) return boot_nvn_fail(c, self, "out of host memory for records", cmd);
        uint64_t old = boot_nvn_hmap_put(slots, cmd, slot);
        if (old == UINT64_MAX) { boot_nvn_slot_free(slot); return boot_nvn_fail(c, self, "out of host memory for records", cmd); }
        if (old) boot_nvn_slot_free(old);   /* a recording begun again without EndRecording */
        if (cmd) { boot_nvn_rec.cmd = cmd; boot_nvn_rec.slot = slot; boot_nvn_rec.gen = gen; }
        if (cmd && boot_nvn_hint_on()) { uint64_t want = boot_nvn_hint_get(cmd); if (want) boot_nvn_slot_reserve(slot, gen, want); }   /* recfast */
        break; }
    case BOOT_NVN_CMD_RECORD: {
        uint64_t slot = 0; uint32_t gen = 0, now = 0;
        if (cmd && boot_nvn_rec.cmd == cmd && boot_nvn_reclock_on()) { slot = boot_nvn_rec.slot; gen = boot_nvn_rec.gen; }
        else if (!(slot = boot_nvn_rec_open(c, self, op, cmd))) return BOOT_HOST_STOP;
        static _Thread_local uint8_t scratch[2 * 65536]; BootNvnRecord r;
        if (!boot_nvn_record_build(c, self, op, &r, scratch, sizeof scratch)) return BOOT_HOST_STOP;
        /* flashfix: the record's deferred bind / called handles, indexed by its own append */
        uint64_t dh[64]; BootNvnDrefAdd da; const BootNvnDrefAdd *dref = boot_nvn_dsnap_wanted() && boot_nvn_dsnap_dref(op, &r, dh, &da) ? &da : NULL;
        if (dref && dref->count == UINT32_MAX && boot_nvn_ev_lru_mode() == 1) boot_nvn_ev_called(dref->ptr, dref->n);   /* rec-evict: called lists are never budget candidates */
        /* call-bind: a CallCommands binds its callees' current versions (in the record and its index) */
        uint64_t cbb[64]; uint32_t cb_lost = 0; const unsigned cbn = dref && dref->count == UINT32_MAX ? boot_nvn_cb_bind(&r, dh, da.n, cbb) : 0;
        if (cbn) { da.bound = cbb; da.lost = &cb_lost; }
        int ok = boot_nvn_record_write(slot, gen, &r, &now, dref);
        if (ok < 0 && gen) {   /* ended or released by another thread meanwhile: the map decides, as without the cache */
            boot_nvn_rec.cmd = 0;
            if (!(slot = boot_nvn_rec_open(c, self, op, cmd))) { if (cbn) boot_nvn_cb_unbind(cbb, cbn); return BOOT_HOST_STOP; }
            ok = boot_nvn_record_write(slot, 0, &r, &now, dref);
        } else if (ok > 0 && gen) ++boot_nvn_rl()->n[BOOT_NVN_RL_CACHED];
        if (cbn && (ok <= 0 || cb_lost)) boot_nvn_cb_unbind(cbb, cbn);   /* call-bind: not in a stored index: nothing would release them */
        if (ok <= 0) { boot_nvn_rec.cmd = 0; return boot_nvn_fail(c, self, "record storage", (uint64_t)op); }
        ++boot_nvn_rl()->n[BOOT_NVN_RL_RECORDS];
        if (cmd) { boot_nvn_rec.cmd = cmd; boot_nvn_rec.slot = slot; boot_nvn_rec.gen = now; }
        break; }
    case BOOT_NVN_CMD_END: {   /* close the records; the handle is known when the driver returns (nvn.cmd.end_ret) */
        uint64_t slot = boot_nvn_hmap_put(slots, cmd, 0);
        if (boot_nvn_rec.cmd == cmd) boot_nvn_rec.cmd = 0;
        if (!slot && !boot_nvn_slot_new(&slot)) return boot_nvn_fail(c, self, "out of host memory for records", cmd);
        uint32_t end = BOOT_NVN_OP_END | (1u << 16); BootNvnPart part = {&end, 4};
        if (boot_nvn_slot_append(slot, 0, &part, 1, 1, NULL, NULL) <= 0) { boot_nvn_slot_free(slot); return boot_nvn_fail(c, self, "record storage", cmd); }
        if (cmd && boot_nvn_hint_on()) boot_nvn_hint_set(cmd, boot_nvn_slot_fit(slot));   /* recfast: the next recording of cmd starts with that capacity */
        if (!boot_host_return_to(c, self, boot_host_fn_address("nvn.cmd.end_ret"), cmd, slot)) { boot_nvn_slot_free(slot); return BOOT_HOST_STOP; }
        break; }
    default: break;
    }
    *target = real;
    return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_cmd_end_ret) {   /* x0 = the driver's handle */
    (void)user; uint64_t cmd = 0, slot = 0;
    if (!boot_host_frame_pop(c, self, target, &cmd, &slot)) return BOOT_HOST_STOP;
    BootNvnEvRange rg; const int ev = boot_nvn_ev_on();   /* rec-evict: the recording's ranges, read before the install (guest reads only) */
    if (ev) boot_nvn_ev_ranges(c, self, cmd, c->x[0], &rg);
    if (!c->x[0]) { boot_nvn_slot_free(slot); return BOOT_HOST_DONE; }
    if (boot_nvn_ftrace_on()) boot_nvn_slot_mark_end(c, slot);   /* batch-a frame trace: who ended it, when */
    if (!boot_nvn_handle_install_ev(c, c->x[0], slot, ev ? &rg : NULL)) return boot_nvn_fail(c, self, "out of host memory for records", c->x[0]);   /* rerecord-work: same steps; rec-evict inside */
    return BOOT_HOST_DONE;
}
/* nvnpar-work (TOTK_RECORD_ZERO_COPY, default on; =0: copied as below). A record of a recording this thread has pinned
 * (finished, never written, moved or freed again until boot_nvn_unpin_all at the end of the submit call) is decoded
 * from the recording's own bytes: the same bounds checks as the fetches below (every fetch of the copying path fails
 * exactly where one of these fails), the same fields, and the blocks point into the recording instead of being copied
 * into `scratch` (record blocks are 4-byte aligned like scratch; every reader copies or reads bytes; the data stays
 * valid at least as long as scratch's: until the submit call ends). The scratch capacity check is kept as is. */
static inline int boot_nvn_record_zero_copy_on(void) {
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_RECORD_ZERO_COPY", s, sizeof s); v = !(k && k < sizeof s && s[0] == '0'); }
    return v;
}
static inline uint64_t boot_nvn_record_parse_pinned(const BootNvnPin *pin, uint64_t at, BootNvnRecord *r, size_t cap) {
    const uint8_t *d = pin->data; const uint64_t len = pin->len, off = at & 0xffffffffu;
    uint32_t h; if (off + 4 > len) return 0;
    memcpy(&h, d + off, 4);
    r->op = h & 0xffff; r->words = h >> 16;
    if (r->op == BOOT_NVN_OP_END) return 4;
    if (r->op == BOOT_NVN_OP_JUMP) { uint32_t a[2]; if (off + 4 + 8 > len) return 0; memcpy(a, d + off + 4, 8); r->x[1] = a[0] | ((uint64_t)a[1] << 32); return 12; }
    if (r->op >= BOOT_NVN_NAME_COUNT || r->words * 4 < BOOT_NVN_RECORD_FIXED) return 0;
    if (off + BOOT_NVN_RECORD_FIXED > len) return 0;
    memcpy(&r->x[1], d + off + 4, 56); memcpy(r->q, d + off + 60, 16);
    uint64_t p = at + BOOT_NVN_RECORD_FIXED, end = at + (uint64_t)r->words * 4; size_t used = 0;
    while (p < end && r->blocks < 2) {
        uint64_t po = p & 0xffffffffu; uint32_t bh[2];
        if (po + 8 > len) return 0;
        memcpy(bh, d + po, 8);
        if (used + bh[1] > cap || p + 8 + bh[1] > end) return 0;
        if (bh[1] && po + 8 + bh[1] > len) return 0;
        r->block[r->blocks].arg = bh[0]; r->block[r->blocks].bytes = bh[1]; r->block[r->blocks].data = d + po + 8;
        ++r->blocks; used += bh[1]; p += 8 + ((bh[1] + 3u) & ~3u);
    }
    return (uint64_t)r->words * 4;
}
static uint64_t boot_nvn_record_zero_copies;   /* records decoded in place (nvnpar-work) */
/* Decodes one record at `at` (blocks point into `scratch`). Returns its size in bytes, 0 on error. */
/* Any other address is a driver handle: it reads as a JUMP to its recording's shadow address. */
static inline uint64_t boot_nvn_record_read(BootContext *c, uint64_t self, uint64_t at, BootNvnRecord *r, uint8_t *scratch, size_t cap) {
    (void)c; (void)self; memset(r, 0, sizeof *r);
    if (!(at & BOOT_NVN_SHADOW)) {
        uint64_t shadow = boot_nvn_freeze_resolve(at);   /* batch-a: the recording the submit call resolved (pinned since) */
        if (!shadow) shadow = boot_nvn_handle_pin(at);
        if (!shadow) return 0;   /* pinned until the submit ends */
        r->op = BOOT_NVN_OP_JUMP; r->words = 3; r->x[1] = shadow; return 12;
    }
    ++boot_nvn_rl()->n[BOOT_NVN_RL_READS];
    if (boot_nvn_reclock_on() && boot_nvn_record_zero_copy_on()) {   /* nvnpar-work: a pinned recording, in place */
        const BootNvnPin *pin = boot_nvn_pin_find(boot_nvn_slot_of(at));
        if (pin) { ++boot_nvn_record_zero_copies; return boot_nvn_record_parse_pinned(pin, at, r, cap); }
    }
    uint32_t h; if (!boot_nvn_cmd_fetch(at, &h, 4)) return 0;
    r->op = h & 0xffff; r->words = h >> 16;
    if (r->op == BOOT_NVN_OP_END) return 4;
    if (r->op == BOOT_NVN_OP_JUMP) { uint32_t a[2]; if (!boot_nvn_cmd_fetch(at + 4, a, 8)) return 0; r->x[1] = a[0] | ((uint64_t)a[1] << 32); return 12; }
    if (r->op >= BOOT_NVN_NAME_COUNT || r->words * 4 < BOOT_NVN_RECORD_FIXED) return 0;
    uint8_t head[BOOT_NVN_RECORD_FIXED];
    if (!boot_nvn_cmd_fetch(at, head, sizeof head)) return 0;
    memcpy(&r->x[1], head + 4, 56); memcpy(r->q, head + 60, 16);
    uint64_t p = at + sizeof head, end = at + (uint64_t)r->words * 4; size_t used = 0;
    while (p < end && r->blocks < 2) {
        uint32_t bh[2]; if (!boot_nvn_cmd_fetch(p, bh, 8)) return 0;
        if (used + bh[1] > cap || p + 8 + bh[1] > end) return 0;
        if (bh[1] && !boot_nvn_cmd_fetch(p + 8, scratch + used, bh[1])) return 0;
        r->block[r->blocks].arg = bh[0]; r->block[r->blocks].bytes = bh[1]; r->block[r->blocks].data = scratch + used;
        ++r->blocks; used += bh[1]; p += 8 + ((bh[1] + 3u) & ~3u);
    }
    return (uint64_t)r->words * 4;
}
static inline int boot_nvn_walk(BootContext *c, uint64_t self, uint64_t handle, BootNvnNullStats *st, unsigned depth) {
    static _Thread_local uint8_t scratch[2 * 65536];
    static int call_op = -2; if (call_op == -2) call_op = boot_nvn_name_index("nvnCommandBufferCallCommands");
    if (depth > 8) return 0;
    for (uint64_t at = handle, n = 0; n < (UINT64_C(1) << 24); ++n) {
        BootNvnRecord r; uint64_t size = boot_nvn_record_read(c, self, at, &r, scratch, sizeof scratch);
        if (!size) return !n && at == handle && boot_nvn_ev_skip(handle, depth);   /* rec-evict: a released handle used again: skipped */
        if (r.op == BOOT_NVN_OP_END) return 1;
        if (r.op == BOOT_NVN_OP_JUMP) { at = r.x[1]; continue; }
        ++st->records; ++st->by_op[r.op];
        if ((int)r.op == call_op && r.blocks) {   /* nested handles: copied out (the scratch is reused below) */
            uint64_t nested[64]; uint32_t count = r.block[0].bytes / 8 < 64 ? r.block[0].bytes / 8 : 64;
            memcpy(nested, r.block[0].data, (size_t)count * 8);
            for (uint32_t k = 0; k < count; ++k) if (!boot_nvn_walk(c, self, nested[k], st, depth + 1)) return 0;
        }
        at += size;
    }
    return 0;
}

#ifdef TOTK_DRAW_PATH
#include "host_nvn_overlap.h"   /* overlap-work: frame-in-flight guard, opt-in early fence (runtime_support only) */
#endif
/* ---------------------------------------------------------------- queues and syncs */
BOOT_NVN_FN(boot_nvn_queue_builder_set_defaults) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnQueueBuilder b; memset(&b, 0, sizeof b); b.magic = BOOT_NVN_MAGIC_QUEUE_BUILDER; BOOT_NVN_PUT(b); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_queue_builder_set) {   /* user: 0 device, 1 flags, 2 command size, 3 compute size, 4 control size, 5 queue memory */
    (void)target; uint64_t address = c->x[0]; BootNvnQueueBuilder b; BOOT_NVN_GET(b);
    switch ((int)(intptr_t)user) {
        case 0: b.device = c->x[1]; break; case 1: b.flags = (uint32_t)c->x[1]; break; case 2: b.command_size = (uint32_t)c->x[1]; break;
        case 3: b.compute_size = (uint32_t)c->x[1]; break; case 4: b.control_size = (uint32_t)c->x[1]; break;
        default: b.memory = c->x[1]; b.memory_size = c->x[2];
    }
    BOOT_NVN_PUT(b); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_queue_builder_memory_size) { BOOT_NVN_UNUSED; (void)self; c->x[0] = BOOT_NVN_QUEUE_MEMORY_SIZE; return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_queue_initialize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[1]; BootNvnQueueBuilder b; BOOT_NVN_GET(b);
    BootNvnQueue q = {BOOT_NVN_MAGIC_QUEUE, b.flags, b.device, 0, 1, 0};
    address = c->x[0]; BOOT_NVN_PUT(q); c->x[0] = 1; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_queue_finalize) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnQueue q; BOOT_NVN_GET(q); q.magic = 0; BOOT_NVN_PUT(q); return BOOT_HOST_DONE; }
static inline int boot_nvn_queue_load(BootContext *c, uint64_t self, uint64_t address, BootNvnQueue *q) {
    if (!boot_host_read(c, self, address, q, sizeof *q)) return 0;
    if (q->magic != BOOT_NVN_MAGIC_QUEUE) { boot_nvn_fail(c, self, "not a queue", address); return 0; }
    return 1;
}
#ifdef TOTK_DRAW_PATH
static inline void boot_sd_call_end(BootContext *c);   /* suspect-diag (host_nvn_exec.h): TOTK_SUSPECT_DIAG end-of-call re-hash */
static inline void boot_ds_call_end(BootContext *c);   /* data-snapshot (host_nvn_exec.h): the detector's end-of-call check */
/* async-xlate (host_nvn_async.h, included after host_nvn_play.h): the submit's capture / hand-off / join, its 300-present line, the
 * registry epoch, and what the executor (host_nvn_play.h, host_nvn_exec.h) asks while it runs on the translation thread X: object words
 * captured at the call, residual live guest reads by class (host_gpu_render.h), the sentinel's record log. Stage 2: the lock-free paths of
 * the queue and sync calls (boot_ax_lf: set by the registration wrapper while the live mode is 2; boot_ax_held: a lock such a call took
 * later, released by the wrapper). */
enum { BOOT_AX_SYNC, BOOT_AX_DONE, BOOT_AX_FAIL, BOOT_AX_READ_FAIL, BOOT_AX_QUEUED };
static _Thread_local int boot_ax_lf, boot_ax_held;
static inline int boot_ax_join(BootContext *c, uint64_t self, uint64_t queue, int frozen, uint64_t *submitted, uint64_t *bad, int *xdiag);
static inline uint64_t boot_ax_tagged(uint64_t v);
static inline uint64_t boot_ax_fence(BootContext *c);
static inline int boot_ax_sync_wait(BootContext *c, uint64_t *v, uint64_t timeout_ns);
static inline void boot_ax_finish(BootContext *c);
static inline int boot_ax_present_queue(BootContext *c, uint64_t window, uint32_t index);
static inline void boot_ax_lock_now(void);
static inline void boot_ax_present(void);
static inline void boot_ax_reg_note(int index);
static inline void boot_ax_live(unsigned cls, uint64_t n);
static inline int boot_ax_obj_word(uint64_t a, uint64_t *v);
static inline void boot_ax_chk_rec(uint64_t at, const BootNvnRecord *rec, uint64_t size);
#endif
#ifdef TOTK_DRAW_PATH   /* overlap-work P3: every return of the submit ends its in-submit stretch */
#define BOOT_NVN_FF_END() boot_ff_submit_end(c)
#else
#define BOOT_NVN_FF_END() ((void)0)
#endif
BOOT_NVN_FN(boot_nvn_queue_submit) {   /* (queue, count, handles) */
    BOOT_NVN_UNUSED; BootNvnQueue q; uint64_t address = c->x[0];
    if (!boot_nvn_queue_load(c, self, address, &q)) return BOOT_HOST_STOP;
    boot_nvn_sub_begin(c);   /* batch-a: a submit call runs (re-record wait, frame trace) */
#ifdef TOTK_DRAW_PATH
    int ax_xdiag = 0;   /* async-xlate stage 2: X ran the call's data-snapshot / suspect-diag begin and end */
    if (!boot_ax_lf) boot_ds_call_begin();   /* data-snapshot (host_gpu_render.h): a new call (call-start copy due, detector records reset); stage 2: on X */
#endif
    /* flashfix: the deferred-bind handles as the driver reads them, at the call, before anything below can let other
     * guest threads run; the recordings it reaches stay pinned until the end of the call (boot_nvn_dsnap_end) */
    boot_nvn_dsnap_take(c, self, c->x[2], c->x[1]);
    const int dsnap_held = boot_nvn_dsnap.active, frozen = dsnap_held && boot_nvn_dsnap.fz_on;
#ifdef TOTK_DRAW_PATH
    boot_ff_submit(c);   /* overlap-work P3: this thread is inside a submit (repeat waits on its frame sync: no give-up) */
    {   /* async-xlate (host_nvn_async.h): stage 1 (=1) captures the submission here, X translates it and this call joins it; stage 2 (=2,
         * lock-free) returns once it is captured and queued (BOOT_AX_QUEUED: the walk table and the pins went with it), or joins it. The
         * outcomes below are the loop's own. BOOT_AX_SYNC (off, or not possible: counted): the loop (stage 2: X drained, the lock taken) */
        uint64_t bad = 0; const int ax = boot_ax_join(c, self, address, frozen, &q.submitted, &bad, &ax_xdiag);
        if (ax == BOOT_AX_READ_FAIL) { boot_nvn_dsnap_end(); boot_nvn_sub_end(); BOOT_NVN_FF_END(); return BOOT_HOST_STOP; }
        if (ax == BOOT_AX_FAIL) { if (boot_nvn_ftrace_on()) boot_nvn_ftrace_write(c, c->x[1], 0); boot_nvn_dsnap_end(); boot_nvn_sub_end(); BOOT_NVN_FF_END(); return boot_nvn_fail(c, self, "submit", bad); }
        if (ax == BOOT_AX_DONE) goto submitted;
        if (ax == BOOT_AX_QUEUED) { boot_nvn_dsnap_end(); boot_nvn_sub_end(); BOOT_NVN_FF_END(); BOOT_NVN_PUT(q); return BOOT_HOST_DONE; }   /* X translates it */
    }
#endif
    BootNvnBackend *be = boot_nvn_backend();
    for (uint64_t k = 0; k < c->x[1]; ++k) {
        uint64_t handle;
        if (frozen && k < boot_nvn_dsnap.top_n) handle = boot_nvn_dsnap.top[k];   /* batch-a: the value the call read */
        else if (!boot_host_read(c, self, c->x[2] + 8 * k, &handle, 8)) { boot_nvn_dsnap_end(); boot_nvn_sub_end(); BOOT_NVN_FF_END(); return BOOT_HOST_STOP; }
        int ok = be->submit(be->ctx, c, self, address, handle);
        if (!dsnap_held) boot_nvn_unpin_all();
        if (!ok) { if (boot_nvn_ftrace_on()) boot_nvn_ftrace_write(c, c->x[1], 0); boot_nvn_dsnap_end(); boot_nvn_sub_end(); BOOT_NVN_FF_END(); return boot_nvn_fail(c, self, "submit", handle); }
        ++q.submitted;
    }
#ifdef TOTK_DRAW_PATH
submitted:   /* async-xlate: X translated the submission */
#endif
    if (boot_nvn_ftrace_on()) boot_nvn_ftrace_write(c, c->x[1], 1);   /* batch-a: before the pins go */
#ifdef TOTK_DRAW_PATH
    if (!ax_xdiag) {   /* stage 2: X ran them */
        boot_sd_call_end(c);   /* suspect-diag: TOTK_SUSPECT_DIAG (off: one cached branch) */
        boot_ds_call_end(c);   /* data-snapshot: TOTK_DATA_SNAPSHOT (=0: one cached branch) */
    }
#endif
    boot_nvn_dsnap_end();
    boot_nvn_sub_end();
    BOOT_NVN_FF_END();
    BOOT_NVN_PUT(q); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_queue_finish) {   /* Finish waits; Flush (user 1) only executes, which submit already did */
    (void)target; BootNvnQueue q; uint64_t address = c->x[0];
    if (!boot_nvn_queue_load(c, self, address, &q)) return BOOT_HOST_STOP;
#ifdef TOTK_DRAW_PATH
    if (!user && boot_ax_lf) { boot_ax_finish(c); return BOOT_HOST_DONE; }   /* async-xlate stage 2: a FINISH op through the FIFO */
#endif
    if (!user) { BootNvnBackend *be = boot_nvn_backend(); uint64_t v = be->signal(be->ctx, address); be->wait(be->ctx, address, v, UINT64_MAX); }
    return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_queue_get_error) { BOOT_NVN_UNUSED; (void)self; c->x[0] = 0; return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_sync_initialize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnSync s = {BOOT_NVN_MAGIC_SYNC, 0, 0, 0};
    BOOT_NVN_PUT(s); c->x[0] = 1; return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_sync_finalize) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnSync s; BOOT_NVN_GET(s); s.magic = 0; BOOT_NVN_PUT(s); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_queue_fence_sync) {   /* (queue, sync, condition, flags) */
    BOOT_NVN_UNUSED; BootNvnQueue q; if (!boot_nvn_queue_load(c, self, c->x[0], &q)) return BOOT_HOST_STOP;
    uint64_t address = c->x[1]; BootNvnSync s; BOOT_NVN_GET(s);
    if (s.magic != BOOT_NVN_MAGIC_SYNC) return boot_nvn_fail(c, self, "not a sync", address);
    BootNvnBackend *be = boot_nvn_backend();
    s.queue = c->x[0];
#ifdef TOTK_DRAW_PATH
    if (boot_ax_lf) s.value = boot_ax_fence(c); else   /* async-xlate stage 2: a FENCE op; the value is its tagged sequence number */
#endif
    s.value = be->signal(be->ctx, c->x[0]);
    BOOT_NVN_PUT(s);
#ifdef TOTK_DRAW_PATH
    boot_ff_fence(c, address, s.value);   /* overlap-work P3: a new fence on this sync (after the sync holds its value) */
    boot_nw_note(BOOT_NW_FRONT_SIGNAL,BOOT_NW_A|BOOT_NW_B|BOOT_NW_C,address,s.queue,s.value,0);
#endif
    return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_sync_wait) {   /* (sync, timeout ns) -> NVNsyncWaitResult */
    BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnSync s; BOOT_NVN_GET(s);
    if (s.magic != BOOT_NVN_MAGIC_SYNC) return boot_nvn_fail(c, self, "not a sync", address);
    BootNvnBackend *be = boot_nvn_backend();
#ifdef TOTK_DRAW_PATH
    uint64_t ff_fence = UINT64_MAX;   /* guardfix: the fence count of the sync that goes with s (UINT64_MAX: not a frame sync) */
    {   /* overlap-work P3: a repeat wait on a frame fence this thread already waited for is read after the next FenceSync */
        int ff = boot_ff_wait(c, address, c->x[1], &ff_fence);
        if (ff < 0) { c->x[0] = BOOT_NVN_SYNC_TIMEOUT_EXPIRED; return BOOT_HOST_DONE; }
        if (ff > 0) { BOOT_NVN_GET(s); if (s.magic != BOOT_NVN_MAGIC_SYNC) return boot_nvn_fail(c, self, "not a sync", address); }
    }
    boot_nw_note(BOOT_NW_FRONT_ENTRY,BOOT_NW_A|BOOT_NW_B|BOOT_NW_C,address,s.queue,s.value,0);
    if (s.queue && boot_ax_tagged(s.value)) {   /* async-xlate stage 2: a virtual fence (the result as the backend gives it; -1: a mode below 2 waits below for its D3D12 value) */
        const int r = boot_ax_sync_wait(c, &s.value, c->x[1]);
        if (r >= 0) {
            c->x[0] = r == 0 ? BOOT_NVN_SYNC_ALREADY_SIGNALED : r == 1 ? BOOT_NVN_SYNC_CONDITION_SATISFIED : BOOT_NVN_SYNC_TIMEOUT_EXPIRED;
            if (r < 2) boot_ff_consumed(c, address, ff_fence);
            boot_nw_note(BOOT_NW_FRONT_RESULT,BOOT_NW_A,c->x[0],0,0,0);
            return BOOT_HOST_DONE;
        }
    }
    if (boot_ax_lf && s.queue && be->completed(be->ctx, s.queue) < s.value) boot_ax_lock_now();   /* stage 2, an untagged value: today's wait, under the renderer lock */
#endif
    if (!s.queue || be->completed(be->ctx, s.queue) >= s.value) {
#ifdef TOTK_DRAW_PATH
        boot_nw_note(BOOT_NW_FRONT_READY,BOOT_NW_A,!s.queue,0,0,0);
        boot_ff_consumed(c, address, ff_fence);
#endif
        c->x[0] = BOOT_NVN_SYNC_ALREADY_SIGNALED; return BOOT_HOST_DONE;
    }
#ifdef TOTK_DRAW_PATH
    {   /* overlap-work P4 (TOTK_EARLY_FENCE=1): the frame waiter's wait on the newest frame fence, -1 = not taken */
        int ef = boot_ef_frame_wait(c, address, s.value, c->x[1]);
        if (ef > 0) boot_ff_consumed(c, address, ff_fence);
        if (ef >= 0) { c->x[0] = ef ? BOOT_NVN_SYNC_CONDITION_SATISFIED : BOOT_NVN_SYNC_TIMEOUT_EXPIRED; return BOOT_HOST_DONE; }
    }
    boot_nw_note(BOOT_NW_FRONT_BACKEND,BOOT_NW_A,c->x[1],0,0,0);
#endif
    c->x[0] = be->wait(be->ctx, s.queue, s.value, c->x[1]) ? BOOT_NVN_SYNC_CONDITION_SATISFIED : BOOT_NVN_SYNC_TIMEOUT_EXPIRED;
#ifdef TOTK_DRAW_PATH
    if (c->x[0] == BOOT_NVN_SYNC_CONDITION_SATISFIED) boot_ff_consumed(c, address, ff_fence);
    boot_nw_note(BOOT_NW_FRONT_RESULT,BOOT_NW_A,c->x[0],0,0,0);
#endif
    return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_queue_wait_sync) {   /* (queue, sync): GPU-side wait */
    BOOT_NVN_UNUSED; uint64_t address = c->x[1]; BootNvnSync s; BOOT_NVN_GET(s);
    if (s.magic != BOOT_NVN_MAGIC_SYNC) return boot_nvn_fail(c, self, "not a sync", address);
    BootNvnBackend *be = boot_nvn_backend(); if (s.queue) be->gpu_wait(be->ctx, c->x[0], s.queue, s.value);
    c->x[0] = 1; return BOOT_HOST_DONE;
}
static inline int boot_nvn_window_presented(BootContext *c, uint64_t self, uint64_t window, uint64_t index);
BOOT_NVN_FN(boot_nvn_queue_present) {   /* (queue, window, texture index) */
    BOOT_NVN_UNUSED; BootNvnBackend *be = boot_nvn_backend();
#ifdef TOTK_DRAW_PATH
    if (boot_ax_lf) (void)boot_ax_present_queue(c, c->x[1], (uint32_t)c->x[2]); else   /* async-xlate stage 2: the call's half here, a PRESENT op for X */
#endif
    if (!be->present(be->ctx, c, c->x[0], c->x[1], (uint32_t)c->x[2])) return boot_nvn_fail(c, self, "present", c->x[1]);
    boot_nvn_rv_present();   /* rerecord-work: the 300-present line */
    boot_nvn_ev_present();   /* rec-evict: the budget sweep and its 300-present line */
#ifdef TOTK_DRAW_PATH
    boot_ax_present();   /* async-xlate: its 300-present line (TOTK_ASYNC_XLATE unset/0: one cached branch) */
#endif
    return boot_nvn_window_presented(c, self, c->x[1], c->x[2]) ? BOOT_HOST_DONE : BOOT_HOST_STOP;
}

/* ---------------------------------------------------------------- programs (observed) */
/* nvnProgramSetShaders(Ext) stays in the driver (it only prepares CPU-side data); a host pre-function
 * records each program's stages for the executor: shader data GPU address, control section, the
 * constant-section offset control[0x700] (c1) and the stage from the SPH (DESIGN.md appendix A). */
typedef struct BootNvnProgramStage { uint64_t data, control; uint32_t c1_offset, kind; } BootNvnProgramStage;   /* kind: 1 vertex, 5 pixel, 6 compute */
typedef struct BootNvnProgram { uint64_t program; uint32_t count; BootNvnProgramStage stage[6]; } BootNvnProgram;
#define BOOT_NVN_PROGRAMS 65536
typedef struct BootNvnPrograms { SRWLOCK lock; BootNvnProgram p[BOOT_NVN_PROGRAMS]; } BootNvnPrograms;
static inline BootNvnPrograms *boot_nvn_programs(void) { static BootNvnPrograms *t; if (!t) t = calloc(1, sizeof *t); return t; }
/* async-xlate stage 2 (TOTK_ASYNC_XLATE >= 2): a SetShaders that replaces a program keeps the old stages with the registry epochs it was set
 * at and replaced at (host_gpu_render.h boot_ax_hist's rule, under the table's lock): X translating a submission captured before reads them. */
#ifdef TOTK_DRAW_PATH
#define BOOT_NVN_PROG_HIST 1024u
typedef struct BootNvnProgHist { BootNvnProgram p; uint64_t ev, er; } BootNvnProgHist;
static BootNvnProgHist *boot_nvn_prog_hist; static uint64_t boot_nvn_prog_hist_n, *boot_nvn_prog_ep;
static uint64_t boot_nvn_prog_hist_c[3];   /* kept, older served, none kept (current used) */
#endif
static inline const BootNvnProgram *boot_nvn_program_find(uint64_t program) {
    BootNvnPrograms *t = boot_nvn_programs(); if (!t || !program) return NULL;
    const BootNvnProgram *found = NULL;
    AcquireSRWLockShared(&t->lock);
    for (uint32_t i = (uint32_t)((program >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 48) & (BOOT_NVN_PROGRAMS - 1), n = 0; n < BOOT_NVN_PROGRAMS; ++n, i = (i + 1) & (BOOT_NVN_PROGRAMS - 1)) {
        if (!t->p[i].program) break;
        if (t->p[i].program == program) { found = &t->p[i]; break; }
    }
#ifdef TOTK_DRAW_PATH
    if (found && boot_ax_x_epoch1 && boot_nvn_prog_ep && boot_nvn_prog_ep[found - t->p] >= boot_ax_x_epoch1) {   /* set again after the capture X translates */
        const uint64_t e = boot_ax_x_epoch1 - 1, n = boot_nvn_prog_hist_n < BOOT_NVN_PROG_HIST ? boot_nvn_prog_hist_n : BOOT_NVN_PROG_HIST; const BootNvnProgram *old = NULL;
        for (uint64_t k = 0; boot_nvn_prog_hist && k < n && !old; ++k) {
            const BootNvnProgHist *h = &boot_nvn_prog_hist[(boot_nvn_prog_hist_n - 1 - k) % BOOT_NVN_PROG_HIST];
            if (h->er <= e) break;
            if (h->p.program == program && h->ev <= e) old = &h->p;
        }
        if (old) { found = old; ++boot_nvn_prog_hist_c[1]; } else ++boot_nvn_prog_hist_c[2];
    }
#endif
    ReleaseSRWLockShared(&t->lock);
    return found;
}
static inline void boot_nvn_program_put(const BootNvnProgram *p) {
    BootNvnPrograms *t = boot_nvn_programs(); if (!t) return;
    AcquireSRWLockExclusive(&t->lock);
    for (uint32_t i = (uint32_t)((p->program >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 48) & (BOOT_NVN_PROGRAMS - 1), n = 0; n < BOOT_NVN_PROGRAMS; ++n, i = (i + 1) & (BOOT_NVN_PROGRAMS - 1))
        if (!t->p[i].program || t->p[i].program == p->program) {
#ifdef TOTK_DRAW_PATH
            if (boot_ax_hist_on) {   /* async-xlate stage 2: the stages it replaces stay readable for the submissions captured before */
                if (!boot_nvn_prog_ep) boot_nvn_prog_ep = (uint64_t *)calloc(BOOT_NVN_PROGRAMS, 8);
                if (!boot_nvn_prog_hist) boot_nvn_prog_hist = (BootNvnProgHist *)calloc(BOOT_NVN_PROG_HIST, sizeof *boot_nvn_prog_hist);
                const uint64_t er = (uint64_t)__atomic_load_n(&boot_ax_reg_epoch, __ATOMIC_ACQUIRE);
                if (boot_nvn_prog_ep && boot_nvn_prog_hist && t->p[i].program) {
                    BootNvnProgHist *h = &boot_nvn_prog_hist[boot_nvn_prog_hist_n++ % BOOT_NVN_PROG_HIST]; h->p = t->p[i]; h->ev = boot_nvn_prog_ep[i]; h->er = er; ++boot_nvn_prog_hist_c[0];
                }
                if (boot_nvn_prog_ep) boot_nvn_prog_ep[i] = er;
            }
#endif
            t->p[i] = *p; break;
        }
    ReleaseSRWLockExclusive(&t->lock);
}
/* nvnProgramSetShaders(program, count, NVNshaderData*) / Ext (32-byte entries): record, then the driver runs. */
static inline int boot_nvn_observe_program(BootContext *c, uint64_t self, int ext) {
    BootNvnProgram p; memset(&p, 0, sizeof p); p.program = c->x[0];
    uint64_t count = c->x[1] < 6 ? c->x[1] : 6, stride = ext ? 24 : 16;   /* Ext entries: {data, control, reserved} */
    for (uint64_t k = 0; k < count; ++k) {
        uint64_t entry[2] = {0, 0};
        if (boot_nvn_peek(c, self, c->x[2] + k * stride, entry, 16) != 16) return 0;
        BootNvnProgramStage *s = &p.stage[p.count++];
        s->data = entry[0]; s->control = entry[1];
        boot_nvn_peek(c, self, entry[1] + 0x700, &s->c1_offset, 4);
        uint32_t sph = 0; uint64_t cpu = boot_nvn_gpu_cpu(c, entry[0] + 0x30);
        if (cpu) boot_nvn_peek(c, self, cpu, &sph, 4);
        uint32_t sph_type = sph & 31, version = (sph >> 5) & 31, type = (sph >> 10) & 15;
        s->kind = version == 3 && sph_type == 1 && (type == 1 || type == 2) ? 1 : version == 3 && sph_type == 2 && type == 5 ? 5 :
                  version == 3 && sph_type == 1 ? 2 + type : 6;   /* 3+: tessellation/geometry (unsupported by the renderer) */
    }
    boot_nvn_program_put(&p);
    return 1;
}

/* ---------------------------------------------------------------- windows */
/* Our layouts (the driver never sees windows): builder {magic, count, interval, pad, device, native, textures*},
 * window {magic, count, interval, next, crop[4], textures[16]} (NVNwindow is 384 bytes). */
enum { BOOT_NVN_MAGIC_WINDOW = 0x4e574e56u, BOOT_NVN_MAGIC_WINDOW_BUILDER = 0x42574e56u };
typedef struct BootNvnWindowBuilder { uint32_t magic, count, interval, active; uint64_t device, native, textures; } BootNvnWindowBuilder;
typedef struct BootNvnWindow { uint32_t magic, count, interval, next, active, pad; int32_t crop[4]; uint64_t textures[16]; } BootNvnWindow;
/* Present hold state, after BootNvnWindow in the 384-byte NVNwindow: guest tick each texture is free
 * again, and the last deadline (a separate block keeps BootNvnWindow, which the page header sees, as is). */
#define BOOT_NVN_WINDOW_HOLD 0xc0u
typedef struct BootNvnWindowHold { uint64_t release[16], chain; } BootNvnWindowHold;
_Static_assert(sizeof(BootNvnWindow) <= 384, "window");
_Static_assert(sizeof(BootNvnWindowBuilder) <= 64, "window builder");
BOOT_NVN_FN(boot_nvn_window_builder_set_defaults) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnWindowBuilder b; memset(&b, 0, sizeof b); b.magic = BOOT_NVN_MAGIC_WINDOW_BUILDER; b.interval = 1; BOOT_NVN_PUT(b); return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_window_builder_set) {   /* user: 0 device, 1 native window, 2 textures (count, array), 3 present interval, 4 active textures */
    (void)target; uint64_t address = c->x[0]; BootNvnWindowBuilder b; BOOT_NVN_GET(b);
    switch ((int)(intptr_t)user) {
        case 0: b.device = c->x[1]; break; case 1: b.native = c->x[1]; break;
        case 2: b.count = (uint32_t)c->x[1]; b.textures = c->x[2]; break; case 3: b.interval = (uint32_t)c->x[1]; break; default: b.active = (uint32_t)c->x[1];
    }
    BOOT_NVN_PUT(b); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_window_initialize) {
    BOOT_NVN_UNUSED; uint64_t address = c->x[1]; BootNvnWindowBuilder b; BOOT_NVN_GET(b);
    BootNvnWindow wn; memset(&wn, 0, sizeof wn); wn.magic = BOOT_NVN_MAGIC_WINDOW; wn.count = b.count < 16 ? b.count : 16; wn.interval = b.interval;
    wn.active = b.active ? b.active : wn.count;
    if (wn.count && !boot_host_read(c, self, b.textures, wn.textures, (size_t)wn.count * 8)) return BOOT_HOST_STOP;
    BootNvnWindowHold hold; memset(&hold, 0, sizeof hold);
    if (!boot_host_write(c, self, c->x[0] + BOOT_NVN_WINDOW_HOLD, &hold, sizeof hold)) return BOOT_HOST_STOP;
    address = c->x[0]; BOOT_NVN_PUT(wn); c->x[0] = 1; return BOOT_HOST_DONE;
}
static inline uint64_t boot_nvn_window_texture(BootContext *c, uint64_t window, uint32_t index) {
    BootNvnWindow wn;
    if (!boot_nvn_peek(c, window, window, &wn, sizeof wn) || wn.magic != BOOT_NVN_MAGIC_WINDOW || index >= wn.count) return 0;
    return wn.textures[index];
}
/* N2 diagnostics (stderr): every present-interval change the game makes, with its caller and the last 10
 * present-to-present times (QPC of the newest presents, noted by boot_nvn_exec_present), and every 300 presents the
 * present-interval waits (host_nvn_play.h boot_nvn_present_note). A change to 3 while frames take under 33 ms is TotK's
 * frame pacer locking 20 fps (nxport crates/engine/src/nvn/api.rs:6365-6367). TOTK_PACE_LOG=0: silent. */
#define BOOT_NVN_PRESENT_RING 11
static volatile int64_t boot_nvn_present_qpc[BOOT_NVN_PRESENT_RING]; static volatile uint32_t boot_nvn_present_count;
static inline int boot_nvn_pace_log_on(void) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_PACE_LOG", v, sizeof v); on = !(n && n < sizeof v && v[0] == '0'); }
    return on;
}
static inline void boot_nvn_interval_changed(BootContext *c, uint32_t from, uint32_t to) {
    if (!boot_nvn_pace_log_on()) return;
    LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
    char ms[128]; size_t n = 0; ms[0] = 0; uint32_t k = boot_nvn_present_count;
    for (uint32_t i = 0; i < 10 && i + 1 < k; ++i) {
        int64_t d = boot_nvn_present_qpc[(k - 1 - i) % BOOT_NVN_PRESENT_RING] - boot_nvn_present_qpc[(k - 2 - i) % BOOT_NVN_PRESENT_RING];
        int w = snprintf(ms + n, sizeof ms - n, " %.1f", (double)d * 1000.0 / (double)f.QuadPart);
        if (w < 0 || (size_t)w >= sizeof ms - n) break;
        n += (size_t)w;
    }
    fprintf(stderr, "nvn native: present interval %u -> %u at qpc %lld, caller %llx (logical %llx), after %u presents; present-to-present ms, newest first:%s\n",
            from, to, (long long)now.QuadPart, (unsigned long long)c->x[30], (unsigned long long)boot_offset(c, c->x[30]), k, ms);
}
BOOT_NVN_FN(boot_nvn_window_set) {   /* user: 0 crop(x, y, w, h), 1 present interval, 2 active textures, 3 finalize */
    (void)target; uint64_t address = c->x[0]; BootNvnWindow wn; BOOT_NVN_GET(wn);
    switch ((int)(intptr_t)user) {
        case 0: for (unsigned k = 0; k < 4; ++k) wn.crop[k] = (int32_t)c->x[1 + k]; break;
        case 1: if (wn.interval != (uint32_t)c->x[1]) boot_nvn_interval_changed(c, wn.interval, (uint32_t)c->x[1]); wn.interval = (uint32_t)c->x[1]; break; case 2: wn.active = (uint32_t)c->x[1]; break; default: wn.magic = 0;
    }
    BOOT_NVN_PUT(wn); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_window_get) {   /* user: 0 present interval, 1 active textures */
    (void)target; uint64_t address = c->x[0]; BootNvnWindow wn; BOOT_NVN_GET(wn);
    c->x[0] = user ? wn.active : wn.interval; return BOOT_HOST_DONE;
}
/* nvnWindowAcquireTexture(window, sync, int *index): the next texture in order; the sync is signalled at once
 * (its contents were presented, and presentation copies them out). Returns NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
 * (0). Present hold (host_present_hold.h, dilated guest clock): a texture queued for display is free again only
 * after its guest vsync deadline; acquiring it earlier sleeps the calling guest thread until then (svc
 * SleepThread through the kernel entry, as the display client blocks on the producer event in the emulated
 * path), so guest time advances one display interval per frame. */
#ifdef TOTK_DRAW_PATH
/* N2: where the present-interval wait (host_nvn_play.h boot_present_interval_pace) runs. nxport (MIT, behaviour only:
 * crates/engine/src/nvn/api.rs:6365-6367, wait in 6060-6104) found that TotK's frame pacer measures its update + draw
 * time without a wait in nvnWindowAcquireTexture but with one in nvnQueuePresentTexture, so the wait at present made a
 * frame that fits interval 2 measure as 2 vblanks or more and locked the game at 20 fps. TOTK's GPU thread calls
 * Present and then AcquireTexture (host_nvn_overlap.h P3), so its frame period is unchanged; only the pacer's
 * measurement is. Default: the wait in acquire (as the display hands a texture back), unless the present hold (dilated
 * clock) paces acquire already. TOTK_PACE_AT=present (or 0): the wait at present, as before. */
static inline void boot_present_interval_pace_at(BootContext *c, uint64_t window, uint32_t interval);
static inline int boot_present_pace_at_acquire(void) {
    static int at = -1;
    if (at < 0) { char v[16]; DWORD n = GetEnvironmentVariableA("TOTK_PACE_AT", v, sizeof v); at = !(n && n < sizeof v && (!_stricmp(v, "present") || !strcmp(v, "0"))); }
    return at && !boot_present_hold_enabled();
}
#endif
BOOT_NVN_FN(boot_nvn_window_acquire) {
    (void)user; uint64_t address = c->x[0]; BootNvnWindow wn; BOOT_NVN_GET(wn);
    if (wn.magic != BOOT_NVN_MAGIC_WINDOW || !wn.active) return boot_nvn_fail(c, self, "not a window", address);
    int32_t index = (int32_t)(wn.next % wn.active); wn.next = (uint32_t)(index + 1) % wn.active;
    BOOT_NVN_PUT(wn);
    BootNvnWindowHold hold; uint64_t release = 0;
    if ((uint32_t)index < 16 && boot_host_read(c, self, address + BOOT_NVN_WINDOW_HOLD, &hold, sizeof hold)) {
        release = hold.release[index]; hold.release[index] = 0;
        if (!boot_host_write(c, self, address + BOOT_NVN_WINDOW_HOLD, &hold, sizeof hold)) return BOOT_HOST_STOP;
    }
    if (c->x[1]) { address = c->x[1]; BootNvnSync s = {BOOT_NVN_MAGIC_SYNC, 0, 0, 0}; BOOT_NVN_PUT(s); }
    if (!boot_host_write(c, self, c->x[2], &index, 4)) return BOOT_HOST_STOP;
    c->x[0] = 0;
    if (release && boot_present_hold_enabled()) {
        uint64_t now = boot_present_now();
        if (release > now) {
            c->x[0] = boot_guest_ticks_to_ns(release - now);
            int rc = boot_svc(c, self, c->x[30], 0xb, target);   /* SleepThread: x0 = 0 on return */
            if (rc < 0) return BOOT_HOST_STOP;
            if (rc == 1) *target = c->x[30];
            c->x[0] = 0;
        }
    }
#ifdef TOTK_DRAW_PATH
    if (boot_present_pace_at_acquire()) boot_present_interval_pace_at(c, 0, wn.interval);   /* N2 (x0 stays 0) */
#endif
    return BOOT_HOST_DONE;
}
/* Guest tick a texture presented now is released at (0: no hold; wall clock or TOTK_PRESENT_HOLD=0). */
static inline uint64_t boot_nvn_present_deadline(uint64_t *chain, unsigned interval) {
    if (!boot_present_hold_enabled() || !interval) return 0;
    return boot_guest_present_deadline(chain, interval > 4 ? 1 : interval);
}
/* Present hold: the presented texture stays on display until its vsync deadline (see boot_nvn_window_acquire). */
static inline int boot_nvn_window_presented(BootContext *c, uint64_t self, uint64_t window, uint64_t index) {
    BootNvnWindow wn; BootNvnWindowHold hold;
    if (index >= 16 || !boot_host_read(c, self, window, &wn, sizeof wn) || wn.magic != BOOT_NVN_MAGIC_WINDOW) return 1;
    if (!boot_host_read(c, self, window + BOOT_NVN_WINDOW_HOLD, &hold, sizeof hold)) return 0;
    hold.release[index] = boot_nvn_present_deadline(&hold.chain, wn.interval);
    return boot_host_write(c, self, window + BOOT_NVN_WINDOW_HOLD, &hold, sizeof hold);
}
/* NvRm interop of the multimedia module (subsdk0): host syncs are always complete. */
BOOT_NVN_FN(boot_nvn_sync_from_nvrm) { BOOT_NVN_UNUSED; uint64_t address = c->x[0]; BootNvnSync s = {BOOT_NVN_MAGIC_SYNC, 0, 0, 0}; BOOT_NVN_PUT(s); c->x[0] = 1; return BOOT_HOST_DONE; }
BOOT_NVN_FN(boot_nvn_sync_get_nvrm) { BOOT_NVN_UNUSED; (void)self; c->x[0] = 0; return BOOT_HOST_DONE; }

#ifdef TOTK_DRAW_PATH
#include "host_nvn_play.h"
#include "host_nvn_async.h"   /* async-xlate: TOTK_ASYNC_XLATE (play-work/async-xlate-work/stage1, stage2) */
#endif

/* ---------------------------------------------------------------- registration */
BOOT_NVN_FN(boot_nvn_unimplemented) {
    (void)target; boot_nvn_log("unimplemented", boot_nvn_names[(intptr_t)user].name, (uint64_t)self);
    boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
}
/* data-snapshot (host_gpu_render.h boot_ds_pool_fn): nvnMemoryPoolBuilderSetDefaults/SetFlags/SetStorage, nvnMemoryPoolInitialize
 * and Finalize are observed (pool flags by storage address); TOTK_DATA_SNAPSHOT=0 or TOTK_DATA_SNAPSHOT_POOLS=0: passed as before. */
static inline int boot_nvn_ds_pool(const char *n) {
#ifdef TOTK_DRAW_PATH
    return boot_ds_pool_fn(n);
#else
    (void)n; return 0;
#endif
}
/* Hybrid native mode: who implements each NVN entry point. */
enum { BOOT_NVN_PASS = 0, BOOT_NVN_OWN = 1, BOOT_NVN_OBSERVE = 2 };
static inline int boot_nvn_policy(const char *n) {
    if (!strcmp(n, "nvnDeviceGetProcAddress")) return BOOT_NVN_OWN;
    if (!strncmp(n, "nvnQueue", 8) || !strncmp(n, "nvnSync", 7) || !strncmp(n, "nvnWindow", 9)) return BOOT_NVN_OWN;
    if (!strncmp(n, "nvnCommandBuffer", 16)) return BOOT_NVN_OBSERVE;   /* the driver's, recorded (boot_nvn_cmd_observe) */
    if (!strcmp(n, "nvnProgramSetShaders") || !strcmp(n, "nvnProgramSetShadersExt") || !strcmp(n, "nvnDeviceSetDepthMode") ||
        !strcmp(n, "nvnDeviceSetWindowOriginMode") || !strcmp(n, "nvnTexturePoolRegisterTexture") || !strcmp(n, "nvnSamplerPoolRegisterSampler") ||
        !strcmp(n, "nvnTextureWriteTexels") || !strcmp(n, "nvnTextureWriteTexelsStrided"))   /* render-32 */
        return BOOT_NVN_OBSERVE;
#ifdef TOTK_DRAW_PATH
    if (!strcmp(n, "nvnTextureFlushTexels") && boot_nvn_flush_texels_on()) return BOOT_NVN_OBSERVE;   /* batch-3 N6 */
    if (boot_frt_name(n)) return BOOT_NVN_OBSERVE;   /* fence-read trace (TOTK_FENCE_READ_TRACE=1): pool/buffer invalidate and flush, timestamps */
#endif
    if (!strcmp(n, "nvnMemoryPoolFinalize") && (boot_nvn_ev_dead_mode() || boot_nvn_ax_mode() >= 2)) return BOOT_NVN_OBSERVE;   /* rec-evict: its recordings are dead; async-xlate stage 2: the drain */
    if (boot_nvn_ds_pool(n)) return BOOT_NVN_OBSERVE;   /* data-snapshot: memory pool flags */
    return BOOT_NVN_PASS;
}
static inline uint64_t boot_nvn_hybrid_address(BootContext *c, int index) {
    const char *n = boot_nvn_names[index].name; char name[112];
    switch (boot_nvn_policy(n)) {
        case BOOT_NVN_OWN: return boot_nvn_address(n);
        case BOOT_NVN_OBSERVE: snprintf(name, sizeof name, "nvno:%s", n); return boot_host_fn_address(name);
        default: { uint64_t logical = boot_nvn_trace()->procs[index]; return logical ? boot_address(c, logical) : 0; }
    }
}
/* nvnBootstrapLoader (sdk+0x4900b0, hooked): the real bootstrap runs; its result is replaced on return. */
BOOT_NVN_FN(boot_nvn_hybrid_bootstrap) {
    (void)target; (void)user;
    return boot_host_return_to(c, self, boot_host_fn_address("nvnh.bootstrap_ret"), c->x[0], 0) ? BOOT_HOST_DECLINE : BOOT_HOST_STOP;
}
static inline uint64_t boot_nvn_hybrid_wrap(BootContext *c, uint64_t self, uint64_t name_address, uint64_t real) {
    char name[112] = {0};
    size_t got = boot_nvn_peek(c, self, name_address, name, sizeof name - 1); name[got] = 0;
    int index = boot_nvn_name_index(name);
    if (index < 0) return real;
    if (real && !boot_host_fn_is_host(real)) boot_nvn_trace_learn(c, index, real);
    return boot_nvn_hybrid_address(c, index);
}
BOOT_NVN_FN(boot_nvn_hybrid_bootstrap_ret) {
    (void)user; uint64_t name = 0;
    if (!boot_host_frame_pop(c, self, target, &name, NULL)) return BOOT_HOST_STOP;
    c->x[0] = boot_nvn_hybrid_wrap(c, self, name, c->x[0]);
    boot_nvn_inline_off(c, self);
    return BOOT_HOST_DONE;
}
/* nvnDeviceGetProcAddress(device, name): owned and observed names answer at once; pass-through names need
 * the driver's pointer (asked once from the real GetProcAddress, then remembered in nvn-trace-procs.txt). */
BOOT_NVN_FN(boot_nvn_hybrid_getproc) {
    (void)user; char name[112] = {0};
    size_t got = boot_nvn_peek(c, self, c->x[1], name, sizeof name - 1); name[got] = 0;
    int index = boot_nvn_name_index(name);
    if (index < 0) { c->x[0] = 0; return BOOT_HOST_DONE; }
    BootNvnTrace *t = boot_nvn_trace();
    uint64_t a = boot_nvn_hybrid_address(c, index);
    /* observed functions continue in the driver: its pointer is needed too */
    if (a && !(boot_nvn_policy(name) == BOOT_NVN_OBSERVE && !t->procs[index])) { c->x[0] = a; return BOOT_HOST_DONE; }
    uint64_t real = t->procs[t->getproc] ? boot_address(c, t->procs[t->getproc]) : 0;
    if (!real || real == UINT64_MAX) return boot_nvn_fail(c, self, "driver GetProcAddress unknown", 0);
    return boot_host_call_guest(c, self, target, real, boot_host_fn_address("nvnh.getproc_ret"), c->x[1], 0);
}
BOOT_NVN_FN(boot_nvn_hybrid_getproc_ret) {
    (void)user; uint64_t name = 0;
    if (!boot_host_frame_pop(c, self, target, &name, NULL)) return BOOT_HOST_STOP;
    c->x[0] = boot_nvn_hybrid_wrap(c, self, name, c->x[0]);
    boot_nvn_inline_off(c, self);
    return BOOT_HOST_DONE;
}
#ifdef TOTK_DRAW_PATH
/* ---------------------------------------------------------------- host texture and sampler descriptors
 * The renderer's view of a pool entry comes from the NVN objects themselves (boot_render_host_register),
 * not from the TIC/TSC words the driver writes into pool memory. Layouts (sdk disassembly):
 *   texture: +0x1e levels, +0x1f samples, +0x20 width, +0x24 height u16, +0x26 depth u16, +0x28 flags u16 |
 *            target << 25, +0x2a format (9 bits), +0x2c swizzle (3 bits x 4) | depth-stencil mode << 12,
 *            +0x50 stride, +0x60 GPU address, +0x71 / +0x72 log2 block height / depth;
 *   view (16 bytes): u16 min layer, u16 layers, u8 base level, u8 levels, u8 target, u8 set mask (1 levels,
 *            2 layers, 4 format, 8 swizzle, 0x10 depth-stencil mode, 0x20 target), u32 format, u8 swizzle[4];
 *   sampler: builder copy at +8: +0x10/+0x14 LOD clamp, +0x18 LOD bias, +0x1c anisotropy, +0x24 border,
 *            +0x34 packed wrap s/t/r (4 bits), reduction, min filter (+0x36), mag filter (bits 20..23),
 *            compare mode (bits 24..25), compare function (bits 26..31).
 * Per NVN format, the driver's own table (sdk+0xd01614 + 48 * format, word 8) holds the texture format word
 * (components, component types, default swizzle) and byte 0x0e the sRGB flag. */
#include "generated_nvn_format_tic.h"   /* boot_nvn_format_tic[161], written by gen_game_headers.py from the dumped sdk */
static inline int boot_nvn_format_srgb(uint32_t f) {
    return f == 55 || f == 56 || (f >= 69 && f <= 72) || f == 78 || (f >= 135 && f <= 148) || f == 155 || f == 156 || f == 159 || f == 160;
}
/* Texture object (0x80 bytes) + view (16 bytes, zero for none) -> descriptor. 0 when unrepresentable. */
static inline int boot_nvn_tic_from_texture(const uint8_t o[0x80], const uint8_t v[16], BootTic *t) {
    memset(t, 0, sizeof *t);
    uint16_t flags, height, depth, fmt16, sw; uint32_t word28, width, stride; uint64_t address;
    memcpy(&word28, o + 0x28, 4); memcpy(&flags, o + 0x28, 2); memcpy(&width, o + 0x20, 4); memcpy(&height, o + 0x24, 2);
    memcpy(&depth, o + 0x26, 2); memcpy(&fmt16, o + 0x2a, 2); memcpy(&sw, o + 0x2c, 2); memcpy(&stride, o + 0x50, 4); memcpy(&address, o + 0x60, 8);
    uint32_t target = word28 >> 25, format = fmt16 & 0x1ff, levels = o[0x1e] ? o[0x1e] : 1, mask = v[7];
    uint32_t swz[4] = {sw & 7u, (sw >> 3) & 7u, (sw >> 6) & 7u, (sw >> 9) & 7u};
    uint32_t min_layer = 0, layers = 0, base_level = 0, view_levels = levels;
    if (mask & 0x20) target = v[6];
    if (mask & 4) memcpy(&format, v + 8, 4);
    if (mask & 8) for (unsigned c = 0; c < 4; ++c) swz[c] = v[12 + c];
    if (mask & 2) { uint16_t a, b; memcpy(&a, v, 2); memcpy(&b, v + 2, 2); min_layer = a; layers = b; }
    if (mask & 1) { base_level = v[4]; view_levels = v[5] ? v[5] : 1; }
    if (format == 0 || format >= 161) return 0;
    uint32_t fw = boot_nvn_format_tic[format];
    t->components = fw & 0x7f;
    for (unsigned c = 0; c < 4; ++c) t->data_type[c] = (fw >> (7 + 3 * c)) & 7;
    uint32_t fsrc[4]; for (unsigned c = 0; c < 4; ++c) fsrc[c] = (fw >> (19 + 3 * c)) & 7;
    for (unsigned c = 0; c < 4; ++c)   /* NVN swizzle: 0 zero, 1 one, 2..5 the format's R, G, B, A */
        t->source[c] = swz[c] == 0 ? 0 : swz[c] == 1 ? 7 : swz[c] <= 5 ? fsrc[swz[c] - 2] : 0;
    t->srgb = (uint32_t)boot_nvn_format_srgb(format);
    t->depth_texture = format >= 49 && format <= 54;
    t->address = address; t->normalized = target != 7;
    static const uint8_t type_of[11] = {BOOT_TEX_1D, BOOT_TEX_2D, BOOT_TEX_3D, BOOT_TEX_1D_ARRAY, BOOT_TEX_2D_ARRAY, BOOT_TEX_2D,
                                        BOOT_TEX_2D_ARRAY, BOOT_TEX_2D_NO_MIPMAP, BOOT_TEX_CUBE, BOOT_TEX_CUBE_ARRAY, BOOT_TEX_1D_BUFFER};
    if (target > 10) return 0;
    t->texture_type = type_of[target];
    if (target == 10) {   /* buffer texture: element count in the width */
        t->header_version = 0; t->width = width; t->height = t->depth = 1; return width != 0;
    }
    t->width = width ? width : 1; t->height = height ? height : 1;
    uint32_t d = depth ? depth : 1;
    if (target == 8 || target == 9) d = d >= 6 ? d / 6 : 1;   /* cubes: TIC depth counts cubes */
    if (target == 8) d = 1;
    if (mask & 2) {   /* a layer range of a layered texture */
        uint32_t per = (target == 8 || target == 9) ? 6 : 1;
        t->base_layer = min_layer / per;
        if (layers) d = layers / per ? layers / per : 1;
    }
    if (target == 2) t->depth = depth ? depth : 1; else t->depth = d;
    t->max_mip = levels - 1;
    t->view_min_mip = base_level; t->view_max_mip = base_level + view_levels - 1;
    if ((flags & 0x50) || stride) {   /* LINEAR / LINEAR_RENDER_TARGET: pitch layout */
        if (!stride) return 0;
        t->header_version = 1; t->pitch = stride;
    } else {
        t->header_version = 3; t->log_gob_h = o[0x71]; t->log_gob_d = o[0x72];
    }
    return 1;
}
/* batch-3 N5 diagnostic (TOTK_STATE_DIAG; log only): the descriptor boot_nvn_tic_from_texture builds, against the TIC
 * words the driver writes into the pool for the same registration. nvnTexturePoolRegisterTexture is observed before the
 * driver runs, so each registration is queued (a later one of the same entry replaces it) and compared at the second
 * present after it (host_nvn_exec.h boot_nvn_state_diag_present), when the driver has written it. Settles whether a view
 * swizzle replaces the texture's own swizzle (ours, above) or is composed on top of it (nxport,
 * crates/engine/src/metal/translated.rs:722-738), and checks the other fields. Logged: the first 32 source (swizzle)
 * mismatches and the first 16 other mismatches, with a field mask: 1 undecodable, 2 address, 4 components/types,
 * 8 layout class (buffer / pitch / block-linear), 0x10 width/type/srgb, 0x20 height/depth, 0x40 block height/depth or
 * pitch, 0x80 levels, 0x100 base layer, 0x200 normalized/depth texture. */
#define BOOT_NVN_TIC_DIAG 256
typedef struct BootNvnTicDiagEnt { uint64_t pool; uint32_t id, epoch, format; uint16_t swizzle; uint8_t view[16]; BootTic ours; } BootNvnTicDiagEnt;
static struct { SRWLOCK lock; uint32_t n, epoch; unsigned src_logged, other_logged; BootNvnTicDiagEnt q[BOOT_NVN_TIC_DIAG]; } boot_nvn_tic_diag;
static inline void boot_nvn_tic_diag_note(uint64_t pool, uint32_t id, const uint8_t o[0x80], const uint8_t v[16], const BootTic *t) {
    if (!boot_render_state_diag_on()) return;
    uint16_t fmt16, sw; uint32_t format; memcpy(&fmt16, o + 0x2a, 2); memcpy(&sw, o + 0x2c, 2); format = fmt16 & 0x1ffu;
    if (v[7] & 4) memcpy(&format, v + 8, 4);   /* the view's format, as boot_nvn_tic_from_texture */
    AcquireSRWLockExclusive(&boot_nvn_tic_diag.lock);
    uint32_t i = 0, n = boot_nvn_tic_diag.n;
    while (i < n && !(boot_nvn_tic_diag.q[i].pool == pool && boot_nvn_tic_diag.q[i].id == id)) ++i;
    if (i == n && n == BOOT_NVN_TIC_DIAG) ++boot_state_diag.tic_dropped;
    else {
        BootNvnTicDiagEnt *q = &boot_nvn_tic_diag.q[i];
        if (i == n) boot_nvn_tic_diag.n = n + 1;
        q->pool = pool; q->id = id; q->epoch = boot_nvn_tic_diag.epoch; q->format = format; q->swizzle = sw; memcpy(q->view, v, 16); q->ours = *t;
    }
    ReleaseSRWLockExclusive(&boot_nvn_tic_diag.lock);
}
/* The sources of the composed rule (nxport translated.rs:722-738: the texture's swizzle, identity when unset, with the
 * view's on top: a view channel selecting R/G/B/A picks the texture's already-swizzled channel). */
static inline void boot_nvn_tic_diag_compose(uint32_t format, uint16_t sw, const uint8_t v[16], uint32_t out[4]) {
    uint32_t fw = format < 161 ? boot_nvn_format_tic[format] : 0, eff[4], sel[4], fsrc[4];
    for (unsigned c = 0; c < 4; ++c) { eff[c] = (sw >> (3 * c)) & 7u; fsrc[c] = (fw >> (19 + 3 * c)) & 7u; }
    if (!(eff[0] | eff[1] | eff[2] | eff[3])) for (unsigned c = 0; c < 4; ++c) eff[c] = 2 + c;
    for (unsigned c = 0; c < 4; ++c) sel[c] = !(v[7] & 8) ? eff[c] : v[12 + c] >= 2 && v[12 + c] <= 5 ? eff[v[12 + c] - 2] : v[12 + c];
    for (unsigned c = 0; c < 4; ++c) out[c] = sel[c] == 0 ? 0 : sel[c] == 1 ? 7 : sel[c] <= 5 ? fsrc[sel[c] - 2] : 0;
}
static inline void boot_nvn_tic_diag_check(BootWindow *w) {
    if (!boot_nvn_tic_diag.n || !w || !w->render) return;   /* racy read: a registration now is checked at a later present */
    AcquireSRWLockExclusive(&w->lock);   /* the renderer's page table (boot_render_diag_guest) */
    AcquireSRWLockExclusive(&boot_nvn_tic_diag.lock);
    BootRender *r = w->render; BootStateDiag *g = &boot_state_diag;
    uint32_t keep = 0, n = boot_nvn_tic_diag.n, epoch = boot_nvn_tic_diag.epoch++;
    for (uint32_t i = 0; i < n; ++i) {
        BootNvnTicDiagEnt *q = &boot_nvn_tic_diag.q[i];
        if (q->epoch == epoch || !r) { if (keep != i) boot_nvn_tic_diag.q[keep] = *q; ++keep; continue; }   /* queued since the last present */
        const uint8_t *p = boot_render_diag_guest(r, q->pool + (uint64_t)q->id * 32, 32);
        if (!p) { ++g->tic_unreadable; continue; }
        uint32_t wd[8]; memcpy(wd, p, 32);
        BootTic d; int ok = boot_tic_decode(wd, &d); const BootTic *o = &q->ours; unsigned bad = 0;
        ++g->tic_checked;
        if (!ok) bad |= 1;
        if (o->address != d.address) bad |= 2;
        if (o->components != d.components || memcmp(o->data_type, d.data_type, sizeof d.data_type)) bad |= 4;
        if ((o->header_version == 0) != (d.header_version == 0) || (o->header_version >= 3) != (d.header_version >= 3)) bad |= 8;
        if (o->width != d.width || o->texture_type != d.texture_type || o->srgb != d.srgb) bad |= 0x10;
        if (o->header_version && d.header_version) {   /* not a buffer texture */
            if (o->height != d.height || o->depth != d.depth) bad |= 0x20;
            if (o->log_gob_h != d.log_gob_h || o->log_gob_d != d.log_gob_d || o->pitch != d.pitch) bad |= 0x40;
            if (o->max_mip != d.max_mip || o->view_min_mip != d.view_min_mip || o->view_max_mip != d.view_max_mip) bad |= 0x80;
            if (o->base_layer != d.base_layer) bad |= 0x100;
            if (o->normalized != d.normalized || o->depth_texture != d.depth_texture) bad |= 0x200;
        }
        uint32_t comp[4]; boot_nvn_tic_diag_compose(q->format, q->swizzle, q->view, comp);
        int src = memcmp(o->source, d.source, sizeof d.source) != 0, comp_ok = !memcmp(comp, d.source, sizeof comp), differ = memcmp(comp, o->source, sizeof comp) != 0;
        if (!src && !bad) ++g->tic_same;
        if (src) { ++g->tic_source_bad; if (comp_ok) ++g->tic_compose; else ++g->tic_neither; }
        else if (differ) ++g->tic_replace;   /* the driver agrees with ours where the composed rule would not */
        if (bad) ++g->tic_other_bad;
        if ((src || differ) && boot_nvn_tic_diag.src_logged < 32) {
            ++boot_nvn_tic_diag.src_logged;
            boot_gp_log("nvn native: N5 TIC source %s: pool %llx id %u format %u, texture swizzle %03x (+0x2c), view mask %02x swizzle %u %u %u %u; "
                        "ours %u%u%u%u, driver %u%u%u%u, composed %u%u%u%u", src ? (comp_ok ? "= composed rule" : "= neither rule") : "= ours (replace rule)",
                        (unsigned long long)q->pool, q->id, q->format, q->swizzle & 0xfffu, q->view[7], q->view[12], q->view[13], q->view[14], q->view[15],
                        o->source[0], o->source[1], o->source[2], o->source[3], d.source[0], d.source[1], d.source[2], d.source[3], comp[0], comp[1], comp[2], comp[3]);
        }
        if (bad && boot_nvn_tic_diag.other_logged < 16) {
            ++boot_nvn_tic_diag.other_logged;
            boot_gp_log("nvn native: N5 TIC field mismatch %x: pool %llx id %u format %u view mask %02x; driver words %08x %08x %08x %08x %08x %08x %08x %08x; "
                        "ours/driver: address %llx/%llx, %ux%ux%u / %ux%ux%u, type %u/%u, header %u/%u, levels %u %u-%u / %u %u-%u, layer %u/%u, "
                        "block %u.%u/%u.%u, pitch %u/%u, srgb %u/%u, normalized %u/%u, depth %u/%u", bad, (unsigned long long)q->pool, q->id, q->format, q->view[7],
                        wd[0], wd[1], wd[2], wd[3], wd[4], wd[5], wd[6], wd[7], (unsigned long long)o->address, (unsigned long long)d.address,
                        o->width, o->height, o->depth, d.width, d.height, d.depth, o->texture_type, d.texture_type, o->header_version, d.header_version,
                        o->max_mip, o->view_min_mip, o->view_max_mip, d.max_mip, d.view_min_mip, d.view_max_mip, o->base_layer, d.base_layer,
                        o->log_gob_h, o->log_gob_d, d.log_gob_h, d.log_gob_d, o->pitch, d.pitch, o->srgb, d.srgb, o->normalized, d.normalized,
                        o->depth_texture, d.depth_texture);
        }
    }
    boot_nvn_tic_diag.n = keep;
    ReleaseSRWLockExclusive(&boot_nvn_tic_diag.lock);
    ReleaseSRWLockExclusive(&w->lock);
}
/* Sampler object (0x40 bytes) -> sampler description. */
static inline void boot_nvn_tsc_from_sampler(const uint8_t o[0x40], BootTsc *s) {
    memset(s, 0, sizeof *s);
    uint32_t packed; float lod[2], bias, aniso; memcpy(&packed, o + 0x34, 4); memcpy(lod, o + 0x10, 8); memcpy(&bias, o + 0x18, 4); memcpy(&aniso, o + 0x1c, 4);
    static const uint8_t wrap[8] = {4, 0, 7, 5, 6, 3, 1, 2};   /* NVN CLAMP, REPEAT, MIRROR_CLAMP, MIRROR_CLAMP_TO_EDGE, MIRROR_CLAMP_TO_BORDER,
                                                                  CLAMP_TO_BORDER, MIRRORED_REPEAT, CLAMP_TO_EDGE -> sampler address modes */
    for (unsigned i = 0; i < 3; ++i) s->address[i] = wrap[(packed >> (4 * i)) & 7];
    uint32_t minf = (packed >> 16) & 0xf, magf = (packed >> 20) & 0xf, cmode = (packed >> 24) & 3, cfunc = packed >> 26;
    static const uint8_t min_of[6] = {1, 2, 1, 2, 1, 2}, mip_of[6] = {1, 1, 2, 2, 3, 3};
    s->min_filter = minf < 6 ? min_of[minf] : 2; s->mip_filter = minf < 6 ? mip_of[minf] : 3;
    s->mag_filter = magf ? 2 : 1;
    s->depth_compare = cmode == 1; s->compare_func = cfunc >= 1 && cfunc <= 8 ? cfunc - 1 : 7;
    uint32_t a = aniso >= 16 ? 7 : aniso >= 12 ? 6 : aniso >= 10 ? 5 : aniso >= 8 ? 4 : aniso >= 6 ? 3 : aniso >= 4 ? 2 : aniso >= 2 ? 1 : 0;
    s->max_aniso = a; if (a && s->min_filter == 2) s->min_filter = 3;
    s->lod_bias = bias; s->min_lod = lod[0]; s->max_lod = lod[1];
    memcpy(s->border, o + 0x24, 16);
}
/* nvnTexturePoolRegisterTexture(pool, id, texture, view) / nvnSamplerPoolRegisterSampler(pool, id, sampler):
 * the pool object holds {memory pool, offset}; its GPU address is memory pool +0x18 plus the offset. */
static inline uint64_t boot_nvn_pool_object_va(BootContext *c, uint64_t self, uint64_t pool_object) {
    uint64_t po[2] = {0, 0}, va = 0;
    if (!pool_object || boot_nvn_peek(c, self, pool_object, po, 16) != 16 || !po[0] || boot_nvn_peek(c, self, po[0] + 0x18, &va, 8) != 8) return 0;
    return va + po[1];
}
static inline void boot_nvn_observe_register(BootContext *c, uint64_t self, int sampler) {
    static uint64_t failed;
    uint64_t pool = boot_nvn_pool_object_va(c, self, c->x[0]); uint32_t id = (uint32_t)c->x[1];
    if (!pool) { if (failed++ < 8) boot_nvn_log("host descriptor: pool unreadable", sampler ? "sampler" : "texture", c->x[0]); return; }
    if (sampler) {
        uint8_t o[0x40] = {0}; BootTsc s;
        if (boot_nvn_peek(c, self, c->x[2], o, sizeof o) != sizeof o) return;
        boot_nvn_tsc_from_sampler(o, &s); boot_render_host_register(1, pool, id, NULL, &s);
    } else {
        uint8_t o[0x80] = {0}, v[16] = {0}; BootTic t;
        if (boot_nvn_peek(c, self, c->x[2], o, sizeof o) != sizeof o) return;
        if (c->x[3]) boot_nvn_peek(c, self, c->x[3], v, sizeof v);
        if (boot_nvn_tic_from_texture(o, v, &t)) { boot_render_host_register(0, pool, id, &t, NULL); boot_nvn_tic_diag_note(pool, id, o, v, &t); }   /* batch-3 N5 */
        else if (failed++ < 16) boot_nvn_log("host descriptor: texture not representable (guest words used)", "texture", c->x[2]);
    }
}
#endif
#ifdef TOTK_DRAW_PATH   /* renderer-only helpers (image trace, texture layout); the launcher builds without it */
/* render-32: nvnTextureWriteTexels(texture, view, region, data) / WriteTexelsStrided: the whole texture's guest range
 * (all levels and layers, from the driver's address at +0x60) goes to the render queue of CPU-written memory. */
/* Diagnostics71: inspect an already-established fixed guest view only. No
 * boot_access, fault conversion, mapping, synchronization or initialization. */
static inline size_t boot_nvn_image_peek_mapped(BootContext *c, uint64_t va, void *dst, size_t size) {
#if TOTK_FASTMEM && TOTK_FM_FIXED
    if (!boot_image_trace_enabled || !va || !size || size > 24 || c->status || !boot_fm.initialized ||
        boot_fm.mode != 1 || !boot_fm.direct_ok || boot_fm.pending || !BOOT_FM_TABLE(c) || !boot_fm.leaves ||
        boot_fm.base != (uint8_t *)(uintptr_t)BOOT_FM_FIXED_BASE || va >= (UINT64_C(1) << BOOT_FM_SPACE_BITS) ||
        size > (UINT64_C(1) << BOOT_FM_SPACE_BITS) - va) return 0;
    uint64_t last = (va + size - 1) >> 12;
    for (uint64_t page = va >> 12; page <= last; ++page) {
        const BootFmLeaf *leaf = boot_fm.leaves[page >> 9];
        if (!leaf || (__atomic_load_n(&leaf->state[page & 511], __ATOMIC_ACQUIRE) & (BOOT_FM_MAPPED | READ)) != (BOOT_FM_MAPPED | READ)) return 0;
    }
    DWORD previous_error = GetLastError(); SIZE_T got = 0;
    BOOL ok = ReadProcessMemory(GetCurrentProcess(), boot_fm.base + va, dst, size, &got);
    SetLastError(previous_error);
    return ok && got == size ? size : 0;
#else
    (void)c; (void)va; (void)dst; (void)size; return 0;
#endif
}
static inline void boot_nvn_observe_write_texels(BootContext *c, uint64_t self, int strided) {
    uint8_t o[0x80] = {0}, v[16] = {0}; BootTic t; BootTexFormat f;
    if (!c->x[0] || boot_nvn_peek(c, self, c->x[0], o, sizeof o) != sizeof o) {
        if (boot_image_trace_enabled) {
            BootImageTraceRecord e = {0}; e.kind = BOOT_IMAGE_API_UNRESOLVED;
            for (unsigned j = 0; j < 4; ++j) e.v[j] = c->x[j];
            e.v[8] = (unsigned)strided; e.v[20] = (uint64_t)InterlockedIncrement64(&boot_image_trace_api_epoch);
            boot_image_trace_push(&e);
        }
        return;
    }
    uint64_t va = 0, size = 0; memcpy(&va, o + 0x60, 8);
    if (boot_nvn_tic_from_texture(o, v, &t) && t.address) {
        va = t.address;
        if (boot_tex_format(t.components, t.data_type[0], &f)) {
            BootTexLevel lv[16]; unsigned levels = t.max_mip + 1 > 16 ? 16 : t.max_mip + 1;
            uint64_t stride = boot_tex_layout(&t, &f, lv, levels);
            uint64_t layers = t.texture_type == BOOT_TEX_CUBE ? 6 : t.texture_type == BOOT_TEX_CUBE_ARRAY ? 6 * (uint64_t)t.depth :
                              (t.texture_type == BOOT_TEX_2D_ARRAY || t.texture_type == BOOT_TEX_1D_ARRAY) ? t.depth : 1;
            size = stride * (layers ? layers : 1);
        }
    }
    int trace_layout_derived = size != 0;
    if (va && !size) size = 0x10000;   /* layout unknown: the first 64 KiB */
    if (boot_image_trace_enabled && (!trace_layout_derived || boot_image_trace_overlap(va, size))) {
        BootImageTraceRecord e = {0}; uint32_t region[6] = {0}, view[4] = {0};
        e.kind = trace_layout_derived ? BOOT_IMAGE_API_ENTRY : BOOT_IMAGE_API_UNRESOLVED;
        e.v[0] = c->x[0]; e.v[1] = c->x[1]; e.v[2] = c->x[2]; e.v[3] = c->x[3]; e.v[4] = va; e.v[5] = size;
        e.v[6] = strided ? c->x[4] : 0; e.v[7] = strided ? c->x[5] : 0; e.v[8] = (unsigned)strided;
        if (!c->x[1] || boot_nvn_image_peek_mapped(c, c->x[1], view, sizeof view) == sizeof view) e.v[9] |= 1;
        if (c->x[2] && boot_nvn_image_peek_mapped(c, c->x[2], region, sizeof region) == sizeof region) e.v[9] |= 2;
        for (unsigned j = 0; j < 6; ++j) e.v[10 + j] = region[j];
        for (unsigned j = 0; j < 4; ++j) e.v[16 + j] = view[j];
        e.v[20] = (uint64_t)InterlockedIncrement64(&boot_image_trace_api_epoch); e.v[21] = (unsigned)trace_layout_derived;
        boot_image_trace_push(&e);
    }
    boot_render_cpu_texels_push(va, size);
}
/* batch-3 N6: nvnTextureFlushTexels(texture, view, region): the game wrote the texture's storage with CPU stores and makes
 * them visible to the GPU. The whole texture's guest range goes to the render queue of CPU-written memory, as for
 * nvnTextureWriteTexels above (textures over it rebuild at their next use). Before, such writes were found only by the
 * texture watch or the round-robin verifier (host_gpu_render.h render-39), late. nxport (MIT,
 * crates/engine/src/nvn/api.rs:4740-4750) marks the texture dirty here for the same reason. The region is logged, not
 * used (as for WriteTexels). TOTK_FLUSH_TEXELS=0: not observed (host_nvn_exec.h boot_nvn_flush_texels_on). */
static inline void boot_nvn_observe_flush_texels(BootContext *c, uint64_t self) {
    uint8_t o[0x80] = {0}, v[16] = {0}; BootTic t; BootTexFormat f;
    ++boot_state_diag.flush_texels;
    if (!c->x[0] || boot_nvn_peek(c, self, c->x[0], o, sizeof o) != sizeof o) return;
    uint64_t va = 0, size = 0; memcpy(&va, o + 0x60, 8);
    if (boot_nvn_tic_from_texture(o, v, &t) && t.address) {   /* the range boot_nvn_observe_write_texels pushes */
        va = t.address;
        if (boot_tex_format(t.components, t.data_type[0], &f)) {
            BootTexLevel lv[16]; unsigned levels = t.max_mip + 1 > 16 ? 16 : t.max_mip + 1;
            uint64_t stride = boot_tex_layout(&t, &f, lv, levels);
            uint64_t layers = t.texture_type == BOOT_TEX_CUBE ? 6 : t.texture_type == BOOT_TEX_CUBE_ARRAY ? 6 * (uint64_t)t.depth :
                              (t.texture_type == BOOT_TEX_2D_ARRAY || t.texture_type == BOOT_TEX_1D_ARRAY) ? t.depth : 1;
            size = stride * (layers ? layers : 1);
        }
    }
    if (!va) return;
    if (!size) size = 0x10000;   /* layout unknown: the first 64 KiB */
    ++boot_state_diag.flush_pushed;
    static unsigned logged;
    if (logged < 16 && boot_render_state_diag_on()) {
        int32_t region[6] = {0}; ++logged;
        if (c->x[2]) boot_nvn_peek(c, self, c->x[2], region, sizeof region);
        boot_gp_log("nvn native: N6 nvnTextureFlushTexels texture %llx: guest %llx + %llx, %ux%ux%u components %x, region %d,%d,%d %dx%dx%d%s (call %llu)",
                    (unsigned long long)c->x[0], (unsigned long long)va, (unsigned long long)size, t.width, t.height, t.depth, t.components,
                    region[0], region[1], region[2], region[3], region[4], region[5], c->x[2] ? "" : " (none)", (unsigned long long)boot_state_diag.flush_texels);
    }
    boot_render_cpu_texels_push(va, size);
}
#endif
/* Observed driver functions: record, then continue in the driver function (tail call). */
BOOT_NVN_FN(boot_nvn_hybrid_observe) {
    int index = (int)(intptr_t)user; const char *n = boot_nvn_names[index].name;
    uint64_t logical = boot_nvn_trace()->procs[index], real = logical ? boot_address(c, logical) : 0;
    if (!real || real == UINT64_MAX) return boot_nvn_fail(c, self, "driver function unknown", (uint64_t)index);
#ifdef TOTK_DRAW_PATH
    if (boot_nvn_ax_mode()) boot_ax_reg_note(index);   /* async-xlate: the registry epoch (registrations, programs, texel writes) */
#endif
    if (!strcmp(n, "nvnProgramSetShaders")) boot_nvn_observe_program(c, self, 0);
    else if (!strcmp(n, "nvnProgramSetShadersExt")) boot_nvn_observe_program(c, self, 1);
#ifdef TOTK_DRAW_PATH
    else if (!strcmp(n, "nvnDeviceSetDepthMode")) { BootNvnExec *e = boot_nvn_exec(); if (e && !boot_ax_mode_op(c, 0, (uint32_t)c->x[1])) e->depth_mode = (uint32_t)c->x[1]; }   /* async-xlate stage 2: an op */
    else if (!strcmp(n, "nvnDeviceSetWindowOriginMode")) { BootNvnExec *e = boot_nvn_exec(); if (e && !boot_ax_mode_op(c, 1, (uint32_t)c->x[1])) e->origin_upper_left = (uint32_t)c->x[1]; }
    else if (!strcmp(n, "nvnTexturePoolRegisterTexture")) boot_nvn_observe_register(c, self, 0);
    else if (!strcmp(n, "nvnSamplerPoolRegisterSampler")) boot_nvn_observe_register(c, self, 1);
    else if (!strcmp(n, "nvnTextureFlushTexels")) boot_nvn_observe_flush_texels(c, self);   /* batch-3 N6 */
    else if (!strcmp(n, "nvnTextureWriteTexels") || !strcmp(n, "nvnTextureWriteTexelsStrided")) boot_nvn_observe_write_texels(c, self, !strcmp(n, "nvnTextureWriteTexelsStrided"));   /* render-32 */
#endif
    {   static int pool_final = -2; if (pool_final == -2) pool_final = boot_nvn_name_index("nvnMemoryPoolFinalize");
#ifdef TOTK_DRAW_PATH
        if (index == pool_final) boot_ax_pool_final(c);   /* async-xlate stage 2: every captured submission translated and executed first (no lock held) */
#endif
        if (index == pool_final) boot_nvn_ev_pool_final(c, self, c->x[0]); }   /* rec-evict: the pool's recordings are dead */
#ifdef TOTK_DRAW_PATH
    boot_ds_pool_note(n, c->x[0], c->x[1], c->x[2]);   /* data-snapshot: pool flags by storage address (own lock) */
#endif
    *target = real;
    return BOOT_HOST_DONE;
}
typedef struct { const char *name; BootHostFunction fn; intptr_t user; } BootNvnImpl;
static inline void boot_nvn_native_register(void) {
#ifdef TOTK_DRAW_PATH
    boot_light88_init();
    boot_light86_init();
    boot_light84_init();
    boot_image_trace_init();
    boot_title_init();   /* diagnostics71: initialization outside callbacks/render locks */
    boot_tile89_init();
#endif
    static const BootNvnImpl impl[] = {
        {"nvnQueueBuilderSetDefaults", boot_nvn_queue_builder_set_defaults, 0}, {"nvnQueueBuilderSetDevice", boot_nvn_queue_builder_set, 0},
        {"nvnQueueBuilderSetFlags", boot_nvn_queue_builder_set, 1}, {"nvnQueueBuilderSetCommandMemorySize", boot_nvn_queue_builder_set, 2},
        {"nvnQueueBuilderSetComputeMemorySize", boot_nvn_queue_builder_set, 3}, {"nvnQueueBuilderSetControlMemorySize", boot_nvn_queue_builder_set, 4},
        {"nvnQueueBuilderSetQueueMemory", boot_nvn_queue_builder_set, 5}, {"nvnQueueBuilderGetQueueMemorySize", boot_nvn_queue_builder_memory_size, 0},
        {"nvnQueueInitialize", boot_nvn_queue_initialize, 0}, {"nvnQueueFinalize", boot_nvn_queue_finalize, 0},
        {"nvnQueueSubmitCommands", boot_nvn_queue_submit, 0}, {"nvnQueueFinish", boot_nvn_queue_finish, 0}, {"nvnQueueFlush", boot_nvn_queue_finish, 1},
        {"nvnQueueGetError", boot_nvn_queue_get_error, 0}, {"nvnQueueFenceSync", boot_nvn_queue_fence_sync, 0}, {"nvnQueueWaitSync", boot_nvn_queue_wait_sync, 0},
        {"nvnQueuePresentTexture", boot_nvn_queue_present, 0}, {"nvnQueueSetDebugLabel", boot_nvn_nop, 0},
        {"nvnSyncInitialize", boot_nvn_sync_initialize, 0}, {"nvnSyncFinalize", boot_nvn_sync_finalize, 0}, {"nvnSyncWait", boot_nvn_sync_wait, 0},
        {"nvnSyncSetDebugLabel", boot_nvn_nop, 0}, {"nvnSyncInitializeFromNvRmSyncNVX", boot_nvn_sync_from_nvrm, 0}, {"nvnSyncGetNvRmSyncNVX", boot_nvn_sync_get_nvrm, 0},
        {"nvnWindowBuilderSetDefaults", boot_nvn_window_builder_set_defaults, 0}, {"nvnWindowBuilderSetDevice", boot_nvn_window_builder_set, 0},
        {"nvnWindowBuilderSetNativeWindow", boot_nvn_window_builder_set, 1}, {"nvnWindowBuilderSetTextures", boot_nvn_window_builder_set, 2},
        {"nvnWindowBuilderSetPresentInterval", boot_nvn_window_builder_set, 3}, {"nvnWindowBuilderSetNumActiveTextures", boot_nvn_window_builder_set, 4},
        {"nvnWindowInitialize", boot_nvn_window_initialize, 0}, {"nvnWindowFinalize", boot_nvn_window_set, 3}, {"nvnWindowSetCrop", boot_nvn_window_set, 0},
        {"nvnWindowSetPresentInterval", boot_nvn_window_set, 1}, {"nvnWindowSetNumActiveTextures", boot_nvn_window_set, 2},
        {"nvnWindowGetPresentInterval", boot_nvn_window_get, 0}, {"nvnWindowGetNumActiveTextures", boot_nvn_window_get, 1},
        {"nvnWindowAcquireTexture", boot_nvn_window_acquire, 0}, {"nvnWindowSetDebugLabel", boot_nvn_nop, 0},
        {"nvnDeviceGetProcAddress", boot_nvn_hybrid_getproc, 0},
    };
    static const char *const not_recorded[] = {"nvnCommandBufferInitialize", "nvnCommandBufferFinalize", "nvnCommandBufferAddCommandMemory",   /* driver only */
        "nvnCommandBufferAddControlMemory", "nvnCommandBufferGetCommandMemoryUsed", "nvnCommandBufferGetCommandMemoryFree",
        "nvnCommandBufferGetControlMemoryUsed", "nvnCommandBufferGetControlMemoryFree", "nvnCommandBufferSetMemoryCallback",
        "nvnCommandBufferSetMemoryCallbackData", "nvnCommandBufferSetCommandMemoryCallbackEnabled", "nvnCommandBufferBeginRecording",
        "nvnCommandBufferEndRecording", "nvnCommandBufferSetDebugLabel", "nvnCommandBufferGetMemoryCallback",
        "nvnCommandBufferGetMemoryCallbackData", "nvnCommandBufferIsRecording", "nvnCommandBufferGetCommandMemorySize",
        "nvnCommandBufferGetControlMemorySize", "nvnCommandBufferGetDebugLabel", "nvnCommandBufferIsCommandMemoryCallbackEnabled"};
    BootNvnTrace *t = boot_nvn_trace();
    boot_nvn_trace_specs(t);   /* argument memory captured by records */
    t->limit = 0;              /* hybrid mode writes no trace */
    t->getproc = boot_nvn_name_index("nvnDeviceGetProcAddress"); t->present = boot_nvn_name_index("nvnQueuePresentTexture");
    FILE *f = fopen("nvn-trace-procs.txt", "r");   /* driver pointers learnt by an earlier process */
    if (f) {
        char name[112]; unsigned long long logical;
        while (fscanf(f, "%111s %llx", name, &logical) == 2) { int i = boot_nvn_name_index(name); if (i >= 0) t->procs[i] = logical; }
        fclose(f);
    }
#ifdef TOTK_DRAW_PATH
    if (boot_nvn_ax_mode() >= 2) boot_ax_hist_on = 1;   /* async-xlate stage 2: registrations keep the entries they replace */
#endif
    for (int i = 0; i < BOOT_NVN_NAME_COUNT; ++i) {
        const char *n = boot_nvn_names[i].name; char name[112]; int policy = boot_nvn_policy(n);
        if (policy == BOOT_NVN_OBSERVE && !strncmp(n, "nvnCommandBuffer", 16)) {
            int skip = !strncmp(n, "nvnCommandBufferGet", 19);
            for (size_t k = 0; k < sizeof not_recorded / sizeof not_recorded[0]; ++k) skip |= !strcmp(not_recorded[k], n);
            uint8_t kind = skip ? BOOT_NVN_CMD_PASS : BOOT_NVN_CMD_RECORD;
            if (!strcmp(n, "nvnCommandBufferBeginRecording")) kind = BOOT_NVN_CMD_BEGIN;
            else if (!strcmp(n, "nvnCommandBufferEndRecording")) kind = BOOT_NVN_CMD_END;
            else if (!strcmp(n, "nvnCommandBufferInitialize") || !strcmp(n, "nvnCommandBufferFinalize")) kind = BOOT_NVN_CMD_RESET;
            else if (!strcmp(n, "nvnCommandBufferAddCommandMemory")) kind = BOOT_NVN_CMD_ADD_MEMORY;
            else if (!strcmp(n, "nvnCommandBufferAddControlMemory")) kind = BOOT_NVN_CMD_ADD_CONTROL;   /* rec-evict */
            boot_nvn_cmd_kinds()[i] = kind;
            snprintf(name, sizeof name, "nvno:%s", n); boot_host_fn_register(name, boot_nvn_cmd_observe, (void *)(intptr_t)i, BOOT_HOST_FN_NOLOCK, 0); continue;   /* records: shadow store and maps are locked */
        }
        if (policy == BOOT_NVN_OBSERVE) {   /* rec-evict: nvnMemoryPoolFinalize's hook never touches the renderer (no lock: no wait behind a submit) */
            snprintf(name, sizeof name, "nvno:%s", n);
#ifdef TOTK_DRAW_PATH
            if (boot_frt_name(n)) { boot_host_fn_register(name, boot_frt_observe, boot_frt_user(n, i), BOOT_HOST_FN_NOLOCK, 0); continue; }   /* fence-read trace: no lock */
#endif
            boot_host_fn_register(name, boot_nvn_hybrid_observe, (void *)(intptr_t)i, !strcmp(n, "nvnMemoryPoolFinalize") || boot_nvn_ds_pool(n) ? BOOT_HOST_FN_NOLOCK : 0, 0); continue;
        }
        if (policy != BOOT_NVN_OWN) continue;
        BootHostFunction fn = boot_nvn_unimplemented; intptr_t user = i;
        for (size_t k = 0; k < sizeof impl / sizeof impl[0]; ++k) if (!strcmp(impl[k].name, n)) { fn = impl[k].fn; user = impl[k].user; }
#ifdef TOTK_DRAW_PATH
        fn = boot_frt_own(fn);   /* fence-read trace (TOTK_FENCE_READ_TRACE=1): its observing wrapper; off: fn itself */
        snprintf(name, sizeof name, "nvn:%s", n);
        if (boot_nvn_ax_mode() >= 2 && boot_ax_own_add(n, &fn, &user)) { boot_host_fn_register(name, fn, (void *)user, BOOT_HOST_FN_NOLOCK, 0); continue; }   /* async-xlate stage 2: the wrapper locks */
#endif
        snprintf(name, sizeof name, "nvn:%s", n);
        boot_host_fn_register(name, fn, (void *)user, 0, 0);
    }
    boot_host_fn_register("nvn.cmd.end_ret", boot_nvn_cmd_end_ret, NULL, BOOT_HOST_FN_NOLOCK, 0);
    boot_host_fn_register("nvnh.bootstrap_ret", boot_nvn_hybrid_bootstrap_ret, NULL, 0, 0);
    boot_host_fn_register("nvnh.getproc_ret", boot_nvn_hybrid_getproc_ret, NULL, 0, 0);
    boot_host_fn_hook(3, 0x4900b0, "nvn.bootstrap", boot_nvn_hybrid_bootstrap, NULL, 0, 0);
#ifdef TOTK_DRAW_PATH
    boot_nvn_exec_install(boot_nvn_backend());
#endif
}
/* ---------------------------------------------------------------- the game's GPU memory allocations (diagnostic) */
/* main+0x85587c(handle, desc, count) is the game's GPU memory allocation (callers: program and command
 * memory setup 0x70d0e8, ...): desc {u64 size per element, u64 sead heap (0: the thread's current heap),
 * u32 alignment, u32 flags, ...}; it allocates size * count through 0x8559d4, which takes a pool of the
 * heap's GPU memory manager or creates one (0x856308, two allocations from the sead heap); on success
 * handle+0x10 is the allocation object, on failure it stays NULL (the 8.91B and 14.39B native stops).
 * A guest-entry hook logs every call's heap, size and result to gpu-alloc.log, so trace and native runs
 * can be compared: one line per failure (the first 1000, with the caller), and per-heap totals every
 * 16384 calls and at every failure. The hook only observes (boot_host_return_to + DECLINE): the
 * translated allocator runs unchanged. Callers inside page 0x855 branch directly and are not seen.
 * On with TOTK_NVN=trace|native unless TOTK_GPU_ALLOC_LOG=0; TOTK_GPU_ALLOC_LOG=1 turns it on in any mode. */
#define BOOT_GALLOC_OFFSET UINT64_C(0x85587c)
enum { BOOT_GALLOC_HEAPS = 64 };
typedef struct BootGallocHeap { uint64_t heap, calls, fails, bytes, fail_bytes, largest_fail; } BootGallocHeap;
typedef struct BootGalloc {
    SRWLOCK lock; BootGallocHeap h[BOOT_GALLOC_HEAPS]; uint32_t heaps;
    uint64_t calls, fails, fail_lines; FILE *f;
} BootGalloc;
static inline BootGalloc *boot_galloc(void) { static BootGalloc g = {SRWLOCK_INIT, {{0}}, 0, 0, 0, 0, NULL}; return &g; }
static inline void boot_galloc_summary_locked(BootGalloc *g) {
    fprintf(g->f, "gpu alloc: %llu calls, %llu failed\n", (unsigned long long)g->calls, (unsigned long long)g->fails);
    for (uint32_t i = 0; i < g->heaps; ++i) {
        const BootGallocHeap *x = &g->h[i];
        fprintf(g->f, "  heap %llx: %llu calls, %llu bytes ok, %llu failed (%llu bytes, largest %llx)\n", (unsigned long long)x->heap,
                (unsigned long long)x->calls, (unsigned long long)x->bytes, (unsigned long long)x->fails,
                (unsigned long long)x->fail_bytes, (unsigned long long)x->largest_fail);
    }
    fflush(g->f);
}
/* One completed allocation: heap (0: current heap), bytes requested, success, the caller's return address. */
static inline void boot_galloc_note(uint64_t heap, uint64_t bytes, int ok, uint64_t caller, uint64_t steps) {
    BootGalloc *g = boot_galloc();
    AcquireSRWLockExclusive(&g->lock);
    if (!g->f) g->f = fopen("gpu-alloc.log", "a");
    BootGallocHeap *x = NULL;
    for (uint32_t i = 0; i < g->heaps && !x; ++i) if (g->h[i].heap == heap) x = &g->h[i];
    if (!x && g->heaps < BOOT_GALLOC_HEAPS) { x = &g->h[g->heaps++]; memset(x, 0, sizeof *x); x->heap = heap; }
    if (!x) x = &g->h[BOOT_GALLOC_HEAPS - 1];   /* overflow: lumped into the last entry */
    ++g->calls; ++x->calls;
    if (ok) x->bytes += bytes;
    else { ++g->fails; ++x->fails; x->fail_bytes += bytes; if (bytes > x->largest_fail) x->largest_fail = bytes; }
    if (g->f) {
        if (!ok && g->fail_lines < 1000) {
            ++g->fail_lines;
            fprintf(g->f, "gpu alloc FAILED: heap %llx size %llx caller %llx step %llu (call %llu)\n", (unsigned long long)heap,
                    (unsigned long long)bytes, (unsigned long long)caller, (unsigned long long)steps, (unsigned long long)g->calls);
            boot_galloc_summary_locked(g);
        } else if (!(g->calls & 16383)) boot_galloc_summary_locked(g);
    }
    ReleaseSRWLockExclusive(&g->lock);
}
static inline int boot_galloc_entry(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t desc[2];
    if (!c->x[0] || !c->x[1] || boot_nvn_peek(c, self, c->x[1], desc, 16) != 16 || !desc[0]) return BOOT_HOST_DECLINE;   /* nothing allocated */
    uint64_t count = c->x[2] < 0xffff ? c->x[2] : 0xffff;
    if (!boot_host_return_to(c, self, boot_host_fn_address("game.gpu_alloc.ret"), c->x[0], (c->x[1] & ((UINT64_C(1) << 48) - 1)) | count << 48))
        return BOOT_HOST_STOP;
    return BOOT_HOST_DECLINE;   /* the translated allocator runs and returns into game.gpu_alloc.ret */
}
/* The sead heap behind desc[1] (the failing one is an expanding heap, vtable main+0x44e1b98, header at the
 * start of its own memory): +0x20 name (char *), +0x28 start, +0x30 size, +0xa8 flags, free list head at
 * +0xb0 (first node +0xb8, node offset +0xc4, block size at block+0x18): the walk sead's getFreeSize
 * (main+0x21512d8) does, read without its lock (diagnostic: a racing change gives one odd line). */
typedef struct BootGallocHeapInfo { uint64_t size, free; char name[40]; int valid; } BootGallocHeapInfo;
static inline void boot_galloc_heap_info(BootContext *c, uint64_t self, uint64_t heap, BootGallocHeapInfo *info) {
    memset(info, 0, sizeof *info); snprintf(info->name, sizeof info->name, "?");
    uint8_t hd[0xc8]; if (!heap || boot_nvn_peek(c, self, heap, hd, sizeof hd) != sizeof hd) return;
    uint64_t name, size, head = heap + 0xb0, node; int32_t off;
    memcpy(&name, hd + 0x20, 8); memcpy(&size, hd + 0x30, 8); memcpy(&node, hd + 0xb8, 8); memcpy(&off, hd + 0xc4, 4);
    char n[40] = {0};
    if (name && boot_nvn_peek(c, self, name, n, sizeof n - 1)) {
        size_t k = 0; while (k < sizeof n - 1 && n[k] >= 0x20 && n[k] < 0x7f) ++k;
        n[k] = 0; if (k) snprintf(info->name, sizeof info->name, "%s", n);
    }
    uint64_t free_bytes = 0; unsigned guard = 0;
    while (node && node != head && guard++ < 65536) {
        uint64_t blk = node - (uint64_t)(int64_t)off, bsize = 0, next = 0;
        if (!boot_nvn_peek(c, self, blk + 0x18, &bsize, 8) || !boot_nvn_peek(c, self, node + 8, &next, 8)) return;
        free_bytes += bsize; node = next;
    }
    if (node != head) return;
    info->size = size; info->free = free_bytes; info->valid = 1;
}
/* Every completed allocation, one line in gpu-alloc-calls.log: step, caller, heap (name, size, bytes in use
 * after the call), request, result. Live bytes falling between two lines are frees. With
 * TOTK_GALLOC_SUMMARY_STEPS=<steps> (e.g. 14.0e9) the per-heap totals are written once when the step count
 * passes it, so trace and native runs can be compared at the same point. */
static inline void boot_galloc_calls_log(BootContext *c, uint64_t self, uint64_t heap, uint64_t bytes, int ok, uint64_t caller) {
    static FILE *f; static SRWLOCK lock = SRWLOCK_INIT; static uint64_t summary_at = UINT64_MAX - 1; static int summarized;
    BootGallocHeapInfo info; boot_galloc_heap_info(c, self, heap, &info);
    AcquireSRWLockExclusive(&lock);
    if (summary_at == UINT64_MAX - 1) {
        char v[64]; DWORD n = GetEnvironmentVariableA("TOTK_GALLOC_SUMMARY_STEPS", v, sizeof v);
        double d = n && n < sizeof v ? strtod(v, NULL) : 0; summary_at = d > 0 ? (uint64_t)d : UINT64_MAX;
    }
    if (!f) f = fopen("gpu-alloc-calls.log", "a");
    if (f) {
        if (info.valid)
            fprintf(f, "%llu %llx heap %llx \"%s\" size %llx used %llx free %llx: alloc %llx %s\n", (unsigned long long)c->steps,
                    (unsigned long long)caller, (unsigned long long)heap, info.name, (unsigned long long)info.size,
                    (unsigned long long)(info.size - info.free), (unsigned long long)info.free, (unsigned long long)bytes, ok ? "ok" : "FAILED");
        else
            fprintf(f, "%llu %llx heap %llx: alloc %llx %s\n", (unsigned long long)c->steps, (unsigned long long)caller,
                    (unsigned long long)heap, (unsigned long long)bytes, ok ? "ok" : "FAILED");
        if (!ok) fflush(f);
    }
    int summary = !summarized && c->steps >= summary_at; if (summary) summarized = 1;
    if (f && summary) fflush(f);
    ReleaseSRWLockExclusive(&lock);
    if (summary) {
        BootGalloc *g = boot_galloc();
        AcquireSRWLockExclusive(&g->lock);
        if (!g->f) g->f = fopen("gpu-alloc.log", "a");
        if (g->f) { fprintf(g->f, "summary at step %llu (TOTK_GALLOC_SUMMARY_STEPS):\n", (unsigned long long)c->steps); boot_galloc_summary_locked(g); }
        ReleaseSRWLockExclusive(&g->lock);
    }
}
static inline int boot_galloc_return(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; uint64_t handle = 0, packed = 0;
    if (!boot_host_frame_pop(c, self, target, &handle, &packed)) return BOOT_HOST_STOP;
    uint64_t desc[2] = {0, 0}, object = 0;
    boot_nvn_peek(c, self, packed & ((UINT64_C(1) << 48) - 1), desc, 16);
    boot_nvn_peek(c, self, handle + 0x10, &object, 8);
    boot_galloc_note(desc[1], desc[0] * (packed >> 48), object != 0, *target, c->steps);
    boot_galloc_calls_log(c, self, desc[1], desc[0] * (packed >> 48), object != 0, *target);
    if (!object && desc[1]) {   /* failure: dump the heap object and look for our command tokens in its first 64 KiB */
        static unsigned dumps; FILE *f = dumps++ < 4 ? fopen("gpu-alloc-dump.log", "a") : NULL;
        if (f) {
            uint8_t h[256]; size_t got = boot_nvn_peek(c, self, desc[1], h, sizeof h);
            fprintf(f, "heap %llx (%zu bytes read) step %llu:\n", (unsigned long long)desc[1], got, (unsigned long long)c->steps);
            for (size_t i = 0; i < got; ++i) fprintf(f, "%02x%s", h[i], (i & 31) == 31 ? "\n" : (i & 7) == 7 ? " " : "");
            static uint8_t big[65536]; uint64_t base = desc[1] & ~UINT64_C(0xffff);
            size_t n = boot_nvn_peek(c, self, base, big, sizeof big);
            for (size_t i = 0; i + 16 <= n; i += 4) { uint32_t w; memcpy(&w, big + i, 4);
                if (w == BOOT_NVN_TOKEN_MAGIC) fprintf(f, "token magic at %llx\n", (unsigned long long)(base + i)); }
            fprintf(f, "scanned %zu bytes from %llx\n", n, (unsigned long long)base); fclose(f);
        }
    }
    return BOOT_HOST_DONE;
}
static inline void boot_galloc_register(unsigned module, uint64_t offset) {
    boot_host_fn_register("game.gpu_alloc.ret", boot_galloc_return, NULL, 0, 0);
    boot_host_fn_hook(module, offset, "game.gpu_alloc", boot_galloc_entry, NULL, 0, 0);
}
/* TOTK_NVN=off|trace|native (registry startup, once per process). */
static inline void boot_nvn_register(void) {
    const char *mode = getenv("TOTK_NVN"), *log = getenv("TOTK_GPU_ALLOC_LOG");
    if (mode && !strcmp(mode, "trace")) boot_nvn_trace_register();
    else if (mode && !strcmp(mode, "native")) boot_nvn_native_register();
    if (log ? !strcmp(log, "1") : (mode && (!strcmp(mode, "trace") || !strcmp(mode, "native")))) boot_galloc_register(1, BOOT_GALLOC_OFFSET);
}
#endif
