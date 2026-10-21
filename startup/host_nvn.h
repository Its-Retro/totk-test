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
typedef struct BootNvnDref { uint32_t off, count; uint64_t ptr; } BootNvnDref;   /* count UINT32_MAX: ptr is a CallCommands handle */
typedef struct BootNvnSlot {
    SRWLOCK lock; uint8_t *data; uint32_t len, cap, gen, next_free, pins, dead, closed; uint64_t token;
    BootNvnDref *dref; uint32_t dref_n, dref_cap, snap, rec_thread; uint64_t end_qpc;
} BootNvnSlot;
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
typedef struct BootNvnRlStats { _Alignas(64) uint64_t n[BOOT_NVN_RL_N]; } BootNvnRlStats;
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
    boot_nvn_rl_lock(&s->lock, BOOT_NVN_RL_STORE);
    if (s->free_head != UINT32_MAX) { i = s->free_head; s->free_head = boot_nvn_slot_at(s, i)->next_free; }
    else {
        if (s->count == s->cap) {   /* a new chunk (zeroed: unlocked locks, no buffers, generation 0) */
            uint32_t k = s->cap >> BOOT_NVN_SLOT_CHUNK_BITS;
            BootNvnSlot *n = k < BOOT_NVN_SLOT_CHUNKS ? (BootNvnSlot *)calloc((size_t)1 << BOOT_NVN_SLOT_CHUNK_BITS, sizeof *n) : NULL;
            if (!n) { ReleaseSRWLockExclusive(&s->lock); return 0; }
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
    *gen = t->gen;
    ReleaseSRWLockExclusive(&t->lock);
    t->pins = t->dead = 0;
    if (++s->live >= 65536 && s->live >= 2 * s->live_logged) { s->live_logged = s->live; logged = 1; live = s->live; }
    ReleaseSRWLockExclusive(&s->lock);
    if (logged) { boot_nvn_log("live command recordings", "shadow store", live);   /* growth here would be a leak */
                  boot_nvn_log("recordings released by command memory reuse so far", "shadow store", boot_nvn_reuse_released); }
    *shadow = BOOT_NVN_SHADOW | (uint64_t)i << 32;
    return 1;
}
static inline int boot_nvn_slot_new(uint64_t *shadow) { uint32_t gen; return boot_nvn_slot_new_gen(shadow, &gen); }
/* Store lock held. 1: a submission still reads it (only marked dead; freed by its unpin), 0: released. */
static inline int boot_nvn_slot_free_locked(BootNvnShadowStore *s, uint32_t i) {
    BootNvnSlot *t = boot_nvn_slot_at(s, i);
    if (t->pins) { t->dead = 1; return 1; }   /* a submission is still reading it: freed by its unpin */
    t->dead = 0;
    AcquireSRWLockExclusive(&t->lock);
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
    return 0;
}
/* 1 when a submission was reading the recording (batch-a: re-recorded under a submit call), 0 otherwise. */
static inline int boot_nvn_slot_free(uint64_t shadow) {
    BootNvnShadowStore *s = boot_nvn_shadow(); uint32_t i = boot_nvn_slot_of(shadow); int pinned = 0;
    boot_nvn_rl_lock(&s->lock, BOOT_NVN_RL_STORE);
    if (i < s->count && boot_nvn_slot_at(s, i)->next_free == UINT32_MAX && !boot_nvn_slot_at(s, i)->dead) pinned = boot_nvn_slot_free_locked(s, i);
    ReleaseSRWLockExclusive(&s->lock);
    return pinned;
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
typedef struct BootNvnDrefAdd { uint32_t count, n; const uint64_t *ptr; } BootNvnDrefAdd;   /* n entries {record offset, count, ptr[k]} */
static inline void boot_nvn_dsnap_lost(void);
static inline void boot_nvn_slot_dref_locked(BootNvnSlot *t, uint32_t off, const BootNvnDrefAdd *d) {   /* slot lock held */
    if (t->dref_n + (uint64_t)d->n > t->dref_cap) {
        uint64_t cap = t->dref_cap ? t->dref_cap : 8; while (cap < t->dref_n + (uint64_t)d->n) cap *= 2;
        BootNvnDref *x = cap <= 0x10000000u ? (BootNvnDref *)realloc(t->dref, (size_t)cap * sizeof *x) : NULL;
        if (x) { t->dref = x; t->dref_cap = (uint32_t)cap; }
    }
    if (t->dref_n + (uint64_t)d->n <= t->dref_cap)
        for (uint32_t k = 0; k < d->n; ++k) { t->dref[t->dref_n].off = off; t->dref[t->dref_n].count = d->count; t->dref[t->dref_n].ptr = d->ptr[k]; ++t->dref_n; }
    else boot_nvn_dsnap_lost();
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
    static int v = -1; if (v < 0) { char s[8]; DWORD k = GetEnvironmentVariableA("TOTK_SUBMIT_FREEZE", s, sizeof s); v = k && k < sizeof s ? s[0] != '0' : boot_mt_overlap_on(); }
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
static struct {
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
    /* batch-a cumulative: handles resolved at the call, of them not in the map then, translation lookups answered from
     * the freeze, of them where the live map had moved on (re-recorded since the call), handles the translation reached
     * that the call did not (live map), freeze entries lost (out of memory) */
    uint64_t fz_frozen, fz_missing, fz_reads, fz_moved, fz_unknown, fz_lost;
    uint64_t ft_latest_end; uint32_t ft_latest_thread, ft_recent;   /* frame trace: newest EndRecording among the reached recordings */
} boot_nvn_dsnap;
static inline void boot_nvn_dsnap_lost(void) { InterlockedIncrement64((volatile LONG64 *)&boot_nvn_dsnap.index_lost); }
/* p grown to hold `need` elements (*cap updated); NULL when out of memory (p unchanged). */
static inline void *boot_nvn_dsnap_grow(void *p, uint32_t *cap, uint64_t need, size_t elem) {
    if (need <= *cap) return p;
    if (need > 0x10000000u) return NULL;
    uint32_t c = *cap ? *cap : 256; while (c < need) c *= 2;
    void *q = realloc(p, (size_t)c * elem); if (q) *cap = c;
    return q;
}
/* Record time: the index entries of record r (its deferred bind, or the handles of a CallCommands: the ones the
 * executor follows, 64 at most), for the record's append (boot_nvn_slot_append). 0 entries: none. */
static inline unsigned boot_nvn_dsnap_dref(int op, const BootNvnRecord *r, uint64_t h[64], BootNvnDrefAdd *d) {
    static int deferred = -2, call = -2;
    if (deferred == -2) deferred = boot_nvn_name_index("nvnCommandBufferBindTexturesDeferred");
    if (call == -2) call = boot_nvn_name_index("nvnCommandBufferCallCommands");
    d->count = 0; d->n = 0; d->ptr = h;
    if (op == deferred) { h[0] = r->x[4]; d->count = (uint32_t)r->x[3]; d->n = 1; }
    else if (op == call && r->blocks && r->block[0].arg == 2 && r->block[0].bytes >= 8) {
        uint32_t n = r->block[0].bytes / 8 < 64 ? r->block[0].bytes / 8 : 64;
        memcpy(h, r->block[0].data, (size_t)n * 8); d->count = UINT32_MAX; d->n = n;
    }
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
static _Thread_local struct { BootNvnPin *pin; uint32_t *hash; uint32_t n, cap, hcap, last; } boot_nvn_pins;
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
static inline uint64_t boot_nvn_handle_pin(uint64_t handle) {
    BootNvnShadowStore *s = boot_nvn_shadow(); const int fast = boot_nvn_reclock_on();
    for (unsigned tries = 0; tries < 64; ++tries) {
        uint64_t shadow = boot_nvn_hmap_get(boot_nvn_handles(), handle); if (!shadow) return 0;
        uint32_t i = boot_nvn_slot_of(shadow); int ok = 0, again = 0;
        if (fast && boot_nvn_pin_find(i)) { ++boot_nvn_rl()->n[BOOT_NVN_RL_PIN_REPEATS]; return shadow; }   /* already pinned by this submission */
        boot_nvn_rl_lock(&s->lock, BOOT_NVN_RL_STORE);
        if (boot_nvn_hmap_get(boot_nvn_handles(), handle) != shadow) again = 1;   /* replaced meanwhile: take the newer one */
        else if (i < s->count && boot_nvn_slot_at(s, i)->next_free == UINT32_MAX && !boot_nvn_slot_at(s, i)->dead) {
            BootNvnSlot *t = boot_nvn_slot_at(s, i);
            if (!fast && boot_nvn_pin_find(i)) ok = 1;   /* TOTK_RECLOCK=0: already pinned, asked under the lock as before */
            else {
                AcquireSRWLockShared(&t->lock); const uint8_t *data = t->data; uint32_t len = t->len; ReleaseSRWLockShared(&t->lock);
                if (boot_nvn_pin_add(i, data, len)) { ++t->pins; ok = 1; ++boot_nvn_rl()->n[BOOT_NVN_RL_PINS]; }
            }
        }
        ReleaseSRWLockExclusive(&s->lock);
        if (ok) return shadow;
        if (!again) return 0;
    }
    return 0;
}
static inline void boot_nvn_unpin_all(void) {
    if (!boot_nvn_pins.n) return;
    BootNvnShadowStore *s = boot_nvn_shadow();
    boot_nvn_rl_lock(&s->lock, BOOT_NVN_RL_STORE);
    for (uint32_t k = 0; k < boot_nvn_pins.n; ++k) {
        uint32_t i = boot_nvn_pins.pin[k].slot; BootNvnSlot *t = boot_nvn_slot_at(s, i);
        if (t->pins && !--t->pins && t->dead) boot_nvn_slot_free_locked(s, i);
        boot_nvn_pins.hash[boot_nvn_pins.pin[k].hpos] = 0;
    }
    boot_nvn_pins.n = 0; boot_nvn_pins.last = 0;
    ReleaseSRWLockExclusive(&s->lock);
}
/* reclock counters, appended to the periodic "submit cpu ms total" line (host_nvn_play.h). */
static inline void boot_nvn_rl_format(char *out, size_t cap) {
    uint64_t n[BOOT_NVN_RL_N] = {0}; double ms[BOOT_NVN_RL_LOCKS]; LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (unsigned t = 0; t < 65; ++t) for (unsigned k = 0; k < BOOT_NVN_RL_N; ++k) n[k] += boot_nvn_rl_slots[t].n[k];
    for (unsigned k = 0; k < BOOT_NVN_RL_LOCKS; ++k) ms[k] = f.QuadPart ? (double)boot_nvn_rl_wait_qpc[k] * 1000.0 / (double)f.QuadPart : 0.0;
    BootNvnShadowStore *s = boot_nvn_shadow();
    snprintf(out, cap, "; record store (reclock %s): records %llu (cached slot %llu), buffer grows %llu, kept buffers reused %llu (%llu KiB kept, %llu KiB in all),"
             " padded blocks %llu, records read %llu (locked fetches %llu), pins %llu (repeats without lock %llu);"
             " lock waits: store %llu (%.2f ms), recording %llu (%.2f ms), maps %llu (%.2f ms)", boot_nvn_reclock_on() ? "on" : "off",
             (unsigned long long)n[BOOT_NVN_RL_RECORDS], (unsigned long long)n[BOOT_NVN_RL_CACHED], (unsigned long long)n[BOOT_NVN_RL_GROWS],
             (unsigned long long)n[BOOT_NVN_RL_REUSED], (unsigned long long)(s->kept >> 10), (unsigned long long)((uint64_t)s->bytes >> 10),
             (unsigned long long)n[BOOT_NVN_RL_UNALIGNED], (unsigned long long)n[BOOT_NVN_RL_READS], (unsigned long long)n[BOOT_NVN_RL_LOCKED_READS],
             (unsigned long long)n[BOOT_NVN_RL_PINS], (unsigned long long)n[BOOT_NVN_RL_PIN_REPEATS],
             (unsigned long long)boot_nvn_rl_waits[BOOT_NVN_RL_STORE], ms[BOOT_NVN_RL_STORE], (unsigned long long)boot_nvn_rl_waits[BOOT_NVN_RL_SLOT], ms[BOOT_NVN_RL_SLOT],
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
    if (!boot_nvn_dsnap.active || !boot_nvn_dsnap.fz_on || boot_nvn_dsnap.tid != GetCurrentThreadId()) return 0;
    const BootNvnFzEnt *f = boot_nvn_freeze_find(h);
    if (!f) { ++boot_nvn_dsnap.fz_unknown; return 0; }
    if (!f->shadow) return 0;   /* not in the map at the call (counted as missing there) */
    ++boot_nvn_dsnap.fz_reads;
    if (boot_nvn_hmap_get(boot_nvn_handles(), h) != f->shadow) ++boot_nvn_dsnap.fz_moved;   /* recorded again since the call */
    return f->shadow;
}
/* nvnQueueSubmitCommands(queue, count, handles), first thing (renderer lock held): every deferred bind the submitted
 * recordings reach, directly or through CallCommands (8 levels, as the executor), copied now. Each recording reached
 * is pinned (boot_nvn_handle_pin) until boot_nvn_dsnap_end, so the record addresses stay what the translation reads.
 * batch-a: with the freeze the handle resolutions and the submitted handle values are kept too (see above); the index
 * and stamp of a slot are read under its own lock (reclock), not the store lock. */
static inline void boot_nvn_dsnap_take(BootContext *c, uint64_t self, uint64_t handles, uint64_t count) {
    boot_nvn_dsnap.active = 0; boot_nvn_dsnap.fz_on = 0; boot_nvn_dsnap.top_n = 0;
    boot_nvn_dsnap.ft_latest_end = 0; boot_nvn_dsnap.ft_latest_thread = UINT32_MAX; boot_nvn_dsnap.ft_recent = 0;
    if (!count || !boot_nvn_dsnap_wanted()) return;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    if (!++boot_nvn_dsnap.stamp) boot_nvn_dsnap.stamp = 1;
    boot_nvn_dsnap.n = boot_nvn_dsnap.hn = boot_nvn_dsnap.wn = 0; boot_nvn_dsnap.fz_n = 0; boot_nvn_dsnap.active = 1; ++boot_nvn_dsnap.submits;
    boot_nvn_dsnap.fz_on = boot_nvn_freeze_on(); boot_nvn_dsnap.tid = GetCurrentThreadId();
    const int trace = boot_nvn_ftrace_on(); int top_ok = 1; uint64_t recent = 0;
    if (trace) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); recent = (uint64_t)f.QuadPart / 1000; }   /* frame trace: 1 ms */
    BootNvnShadowStore *s = boot_nvn_shadow();
    for (uint64_t k = 0; k < count && k < 4096; ++k) {
        uint64_t h = 0, sh;
        if (boot_nvn_peek(c, self, handles + 8 * k, &h, 8) != 8) { top_ok = 0; continue; }
        if (top_ok) {   /* batch-a: the handle values as the call read them (a prefix: a failed read ends it) */
            uint64_t *top = (uint64_t *)boot_nvn_dsnap_grow(boot_nvn_dsnap.top, &boot_nvn_dsnap.top_cap, (uint64_t)boot_nvn_dsnap.top_n + 1, 8);
            if (top) { boot_nvn_dsnap.top = top; top[boot_nvn_dsnap.top_n++] = h; } else top_ok = 0;
        }
        if (h && (sh = boot_nvn_dsnap_resolve(h)) != 0) boot_nvn_dsnap_push(sh, 0);
    }
    while (boot_nvn_dsnap.wn) {
        BootNvnDsnapWork w = boot_nvn_dsnap.work[--boot_nvn_dsnap.wn];
        uint32_t i = boot_nvn_slot_of(w.shadow), nr = 0; uint64_t base = BOOT_NVN_SHADOW | (uint64_t)i << 32;
        BootNvnSlot *t = boot_nvn_slot_peek(s, i);
        if (!t) continue;
        AcquireSRWLockExclusive(&t->lock);
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
            if (d->count != UINT32_MAX) { boot_nvn_dsnap_add(c, self, base + d->off, d->ptr, d->count); continue; }
            uint64_t sh;
            if (w.depth < 8 && d->ptr && (sh = boot_nvn_dsnap_resolve(d->ptr)) != 0) boot_nvn_dsnap_push(sh, w.depth + 1);
        }
    }
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
static inline void boot_nvn_sub_begin(BootContext *c) {
    unsigned th = boot_nvn_guest_thread(c);
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    if (!(boot_nvn_sub.seq & 1)) ++boot_nvn_sub.seq;
    boot_nvn_sub.tid = GetCurrentThreadId(); boot_nvn_sub.thread = th;
    boot_nvn_sub.prev_call_qpc = boot_nvn_sub.call_qpc; boot_nvn_sub.call_qpc = boot_nvn_qpc(); boot_nvn_sub.ev_n = boot_nvn_sub.ev_lost = 0;
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    if (boot_nvn_ftrace_on()) { __atomic_store_n(&boot_mt_ft.thread, (LONG)th, __ATOMIC_RELAXED); __atomic_store_n(&boot_mt_ft.on, 1, __ATOMIC_RELEASE); }
}
static inline void boot_nvn_sub_end(void) {
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    if (boot_nvn_sub.seq & 1) ++boot_nvn_sub.seq;
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    WakeAllConditionVariable(&boot_nvn_sub.cv);
}
/* batch-a: EndRecording returned handle `handle` again while a submission had its older recording `old` pinned (the
 * old slot is only marked dead, so the call still reads it; with the freeze its translation keeps reading it). Noted
 * for the trace, logged (first 16), and with TOTK_RERECORD_WAIT the recording thread waits for the call to return. */
static inline void boot_nvn_rerecorded(BootContext *c, uint64_t handle, uint64_t old) {
    unsigned me = boot_nvn_guest_thread(c); uint64_t now = boot_nvn_qpc(), f = boot_nvn_qpf();
    InterlockedIncrement64(&boot_nvn_sub.rerecords);
    AcquireSRWLockExclusive(&boot_nvn_sub.lock);
    LONG64 seq = boot_nvn_sub.seq; int inside = (seq & 1) && boot_nvn_sub.tid != GetCurrentThreadId();
    uint64_t call = boot_nvn_sub.call_qpc; unsigned submitter = boot_nvn_sub.thread;
    if (inside) {
        if (boot_nvn_sub.ev_n < BOOT_NVN_SUB_EVENTS) { BootNvnSubEvent *e = &boot_nvn_sub.ev[boot_nvn_sub.ev_n++]; e->handle = handle; e->qpc = now; e->thread = me; e->pad = 0; }
        else ++boot_nvn_sub.ev_lost;
    }
    ReleaseSRWLockExclusive(&boot_nvn_sub.lock);
    if (!inside) { InterlockedIncrement64(&boot_nvn_sub.outside); return; }   /* the call had just ended (its pins were going) */
    const int wait = boot_nvn_rerecord_wait_on();
#ifdef TOTK_DRAW_PATH
    if (InterlockedIncrement64(&boot_nvn_sub.logged) <= 16) {
        char t[200]; snprintf(t, sizeof t, " (batch-a): handle %llx by t%u, %.2f ms after the submit call of t%u began (older recording slot %u)%s", (unsigned long long)handle, me,
                              (double)(now - call) * 1000.0 / (double)f, submitter, boot_nvn_slot_of(old), wait ? ", the thread waits for the call" : "");
        boot_draw_log("nvn native: recording replaced while a submission reads it%s (event %llu of the first 16, %llu so far)", t, (uint64_t)boot_nvn_sub.logged, (uint64_t)boot_nvn_sub.rerecords);
    }
#else
    (void)submitter; (void)old; (void)call;
#endif
    if (!wait) return;
    if (boot_mt.active) { CRITICAL_SECTION *cs = boot_render_cs(); if ((DWORD)(uintptr_t)cs->OwningThread == GetCurrentThreadId()) return; }   /* never under the renderer lock */
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
enum { BOOT_NVN_CMD_PASS = 0, BOOT_NVN_CMD_RECORD = 1, BOOT_NVN_CMD_BEGIN = 2, BOOT_NVN_CMD_END = 3, BOOT_NVN_CMD_RESET = 4, BOOT_NVN_CMD_ADD_MEMORY = 5 };
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
        {   /* play (62): (cmd, pool, offset, size): recordings in that memory are dead */
            BootNvnPool p; uint64_t off = c->x[2], size = c->x[3];
            if (boot_nvn_peek(c, self, c->x[1], &p, sizeof p) == sizeof p && p.magic == BOOT_NVN_MAGIC_POOL && off < p.size) {
                if (size > p.size - off) size = p.size - off;
                if (p.storage) boot_nvn_handles_release_range(p.storage + off, size);
                if (p.va) boot_nvn_handles_release_range(p.va + off, size);
            } else { static LONG logged; if (!InterlockedExchange(&logged, 1)) boot_nvn_log("command memory reuse: pool not recognised (recordings kept)", "nvnCommandBufferAddCommandMemory", c->x[1]); }
        }
        break;
    case BOOT_NVN_CMD_RESET: {
        uint64_t old = boot_nvn_hmap_put(slots, cmd, 0); if (old) boot_nvn_slot_free(old);
        if (boot_nvn_rec.cmd == cmd) boot_nvn_rec.cmd = 0;
        break; }
    case BOOT_NVN_CMD_BEGIN: {
        boot_nvn_inline_off(c, self);
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
        int ok = boot_nvn_record_write(slot, gen, &r, &now, dref);
        if (ok < 0 && gen) {   /* ended or released by another thread meanwhile: the map decides, as without the cache */
            boot_nvn_rec.cmd = 0;
            if (!(slot = boot_nvn_rec_open(c, self, op, cmd))) return BOOT_HOST_STOP;
            ok = boot_nvn_record_write(slot, 0, &r, &now, dref);
        } else if (ok > 0 && gen) ++boot_nvn_rl()->n[BOOT_NVN_RL_CACHED];
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
    (void)cmd;
    if (!c->x[0]) { boot_nvn_slot_free(slot); return BOOT_HOST_DONE; }
    if (boot_nvn_ftrace_on()) boot_nvn_slot_mark_end(c, slot);   /* batch-a frame trace: who ended it, when */
    uint64_t old = boot_nvn_hmap_put(boot_nvn_handles(), c->x[0], slot);
    if (old == UINT64_MAX) { boot_nvn_slot_free(slot); return boot_nvn_fail(c, self, "out of host memory for records", c->x[0]); }
    if (old && old != slot && boot_nvn_slot_free(old)) boot_nvn_rerecorded(c, c->x[0], old);   /* batch-a: a submission was reading the older one */
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
        if (!size) return 0;
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
#ifdef TOTK_DRAW_PATH   /* overlap-work P3: every return of the submit ends its in-submit stretch */
#define BOOT_NVN_FF_END() boot_ff_submit_end(c)
#else
#define BOOT_NVN_FF_END() ((void)0)
#endif
BOOT_NVN_FN(boot_nvn_queue_submit) {   /* (queue, count, handles) */
    BOOT_NVN_UNUSED; BootNvnQueue q; uint64_t address = c->x[0];
    if (!boot_nvn_queue_load(c, self, address, &q)) return BOOT_HOST_STOP;
    boot_nvn_sub_begin(c);   /* batch-a: a submit call runs (re-record wait, frame trace) */
    /* flashfix: the deferred-bind handles as the driver reads them, at the call, before anything below can let other
     * guest threads run; the recordings it reaches stay pinned until the end of the call (boot_nvn_dsnap_end) */
    boot_nvn_dsnap_take(c, self, c->x[2], c->x[1]);
    const int dsnap_held = boot_nvn_dsnap.active, frozen = dsnap_held && boot_nvn_dsnap.fz_on;
#ifdef TOTK_DRAW_PATH
    boot_ff_submit(c);   /* overlap-work P3: this thread is inside a submit (repeat waits on its frame sync: no give-up) */
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
    if (boot_nvn_ftrace_on()) boot_nvn_ftrace_write(c, c->x[1], 1);   /* batch-a: before the pins go */
    boot_nvn_dsnap_end();
    boot_nvn_sub_end();
    BOOT_NVN_FF_END();
    BOOT_NVN_PUT(q); return BOOT_HOST_DONE;
}
BOOT_NVN_FN(boot_nvn_queue_finish) {   /* Finish waits; Flush (user 1) only executes, which submit already did */
    (void)target; BootNvnQueue q; uint64_t address = c->x[0];
    if (!boot_nvn_queue_load(c, self, address, &q)) return BOOT_HOST_STOP;
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
    s.queue = c->x[0]; s.value = be->signal(be->ctx, c->x[0]);
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
    if (!be->present(be->ctx, c, c->x[0], c->x[1], (uint32_t)c->x[2])) return boot_nvn_fail(c, self, "present", c->x[1]);
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
static inline const BootNvnProgram *boot_nvn_program_find(uint64_t program) {
    BootNvnPrograms *t = boot_nvn_programs(); if (!t || !program) return NULL;
    const BootNvnProgram *found = NULL;
    AcquireSRWLockShared(&t->lock);
    for (uint32_t i = (uint32_t)((program >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 48) & (BOOT_NVN_PROGRAMS - 1), n = 0; n < BOOT_NVN_PROGRAMS; ++n, i = (i + 1) & (BOOT_NVN_PROGRAMS - 1)) {
        if (!t->p[i].program) break;
        if (t->p[i].program == program) { found = &t->p[i]; break; }
    }
    ReleaseSRWLockShared(&t->lock);
    return found;
}
static inline void boot_nvn_program_put(const BootNvnProgram *p) {
    BootNvnPrograms *t = boot_nvn_programs(); if (!t) return;
    AcquireSRWLockExclusive(&t->lock);
    for (uint32_t i = (uint32_t)((p->program >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 48) & (BOOT_NVN_PROGRAMS - 1), n = 0; n < BOOT_NVN_PROGRAMS; ++n, i = (i + 1) & (BOOT_NVN_PROGRAMS - 1))
        if (!t->p[i].program || t->p[i].program == p->program) { t->p[i] = *p; break; }
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
BOOT_NVN_FN(boot_nvn_window_set) {   /* user: 0 crop(x, y, w, h), 1 present interval, 2 active textures, 3 finalize */
    (void)target; uint64_t address = c->x[0]; BootNvnWindow wn; BOOT_NVN_GET(wn);
    switch ((int)(intptr_t)user) {
        case 0: for (unsigned k = 0; k < 4; ++k) wn.crop[k] = (int32_t)c->x[1 + k]; break;
        case 1: wn.interval = (uint32_t)c->x[1]; break; case 2: wn.active = (uint32_t)c->x[1]; break; default: wn.magic = 0;
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
#endif

/* ---------------------------------------------------------------- registration */
BOOT_NVN_FN(boot_nvn_unimplemented) {
    (void)target; boot_nvn_log("unimplemented", boot_nvn_names[(intptr_t)user].name, (uint64_t)self);
    boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
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
        if (boot_nvn_tic_from_texture(o, v, &t)) boot_render_host_register(0, pool, id, &t, NULL);
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
#endif
/* Observed driver functions: record, then continue in the driver function (tail call). */
BOOT_NVN_FN(boot_nvn_hybrid_observe) {
    int index = (int)(intptr_t)user; const char *n = boot_nvn_names[index].name;
    uint64_t logical = boot_nvn_trace()->procs[index], real = logical ? boot_address(c, logical) : 0;
    if (!real || real == UINT64_MAX) return boot_nvn_fail(c, self, "driver function unknown", (uint64_t)index);
    if (!strcmp(n, "nvnProgramSetShaders")) boot_nvn_observe_program(c, self, 0);
    else if (!strcmp(n, "nvnProgramSetShadersExt")) boot_nvn_observe_program(c, self, 1);
#ifdef TOTK_DRAW_PATH
    else if (!strcmp(n, "nvnDeviceSetDepthMode")) { BootNvnExec *e = boot_nvn_exec(); if (e) e->depth_mode = (uint32_t)c->x[1]; }
    else if (!strcmp(n, "nvnDeviceSetWindowOriginMode")) { BootNvnExec *e = boot_nvn_exec(); if (e) e->origin_upper_left = (uint32_t)c->x[1]; }
    else if (!strcmp(n, "nvnTexturePoolRegisterTexture")) boot_nvn_observe_register(c, self, 0);
    else if (!strcmp(n, "nvnSamplerPoolRegisterSampler")) boot_nvn_observe_register(c, self, 1);
    else if (!strcmp(n, "nvnTextureWriteTexels") || !strcmp(n, "nvnTextureWriteTexelsStrided")) boot_nvn_observe_write_texels(c, self, !strcmp(n, "nvnTextureWriteTexelsStrided"));   /* render-32 */
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
            boot_nvn_cmd_kinds()[i] = kind;
            snprintf(name, sizeof name, "nvno:%s", n); boot_host_fn_register(name, boot_nvn_cmd_observe, (void *)(intptr_t)i, BOOT_HOST_FN_NOLOCK, 0); continue;   /* records: shadow store and maps are locked */
        }
        if (policy == BOOT_NVN_OBSERVE) { snprintf(name, sizeof name, "nvno:%s", n); boot_host_fn_register(name, boot_nvn_hybrid_observe, (void *)(intptr_t)i, 0, 0); continue; }
        if (policy != BOOT_NVN_OWN) continue;
        BootHostFunction fn = boot_nvn_unimplemented; intptr_t user = i;
        for (size_t k = 0; k < sizeof impl / sizeof impl[0]; ++k) if (!strcmp(impl[k].name, n)) { fn = impl[k].fn; user = impl[k].user; }
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
