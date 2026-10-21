#ifndef TOTK_HOST_SDK_OS_H
#define TOTK_HOST_SDK_OS_H
/* nn::os locks natively (switch "os"; included by host_sdk.h).
 *
 * The SDK's lock primitive is nn::os::detail::InternalCriticalSectionImplByHorizon: one u32 word in
 * guest memory holding the owner's thread handle (ThreadType+0x1b0; the current ThreadType is at
 * TLS+0x1f8), bit 30 = "kernel waiters". Uncontended Enter/Leave are user-space atomics; contention
 * goes to the kernel through the SDK's svc wrappers (ArbitrateLock sdk+0x15841c, ArbitrateUnlock
 * +0x158424). Condition variables are one u32 word: Signal/Broadcast only enter the kernel
 * (SignalProcessWideKey +0x158434) when the word is non-zero; Wait always does (+0x15842c).
 *
 * The host functions here do the user-space part in C on the same words with host atomics
 * (InterlockedCompareExchange: atomic against other host threads and against the translated LDAXR/STLXR,
 * which the runtime implements as a compare-exchange against the loaded value), and DECLINE whenever
 * the kernel is needed (contention, waiters bit, recursion errors that abort). The declined call is
 * the original translated function from its first instruction, so it takes the kernel path through the
 * runtime's svc layer: whatever the thread model (cooperative scheduler today, host threads in the play
 * build) implements ArbitrateLock & co. keeps working unchanged. Nothing here blocks or switches threads,
 * so no hook is a kernel entry (no BOOT_HOST_FN_KERNEL).
 *
 * Object layouts (from the sdk disassembly, pinned build):
 *   MutexType (nn::os::LockMutex sdk+0x127e20): +0 u8 state, +1 u8 recursive, +4 lock level,
 *     +8 u32 nest count, +0x10 owner ThreadType*, +0x18 critical-section word.
 *   SdkMutexType: +0 critical-section word.  SdkRecursiveMutexType: +0 word, +4 u32 count.
 *   ConditionVariableType: +4 condition word (InternalConditionVariable).                         */
#define SDK_OS_TLS_THREAD      0x1f8         /* TLS: current nn::os::ThreadType* */
#define SDK_OS_THREAD_HANDLE   0x1b0         /* ThreadType: u32 kernel thread handle (critical-section tag) */
#define SDK_OS_WAITERS         UINT32_C(0x40000000)
#define SDK_OS_STEPS           12            /* nominal guest steps per native call */

/* Current thread: ThreadType* and its handle tag. 0 when unreadable (c->status set: STOP) or tag 0. */
static inline int sdk_os_thread(BootContext *c, uint64_t self, uint64_t *thread, uint32_t *tag) {
    uint64_t t = 0; uint32_t h = 0;
    if (!boot_host_read(c, self, c->tpidrro + SDK_OS_TLS_THREAD, &t, 8) || !t) return 0;
    if (!boot_host_read(c, self, t + SDK_OS_THREAD_HANDLE, &h, 4) || !h) return 0;
    *thread = t; *tag = h; return 1;
}
/* Host pointer to an aligned guest u32 for atomics; NULL = decline (misaligned: the guest code faults
 * exactly as before) or stop (c->status set by boot_access). */
static inline volatile LONG *sdk_os_word(BootContext *c, uint64_t self, uint64_t a) {
    if (a & 3) return NULL;
    uint8_t *p = boot_access(c, a, 4, READ | WRITE, self);
    if (!p || ((uintptr_t)p & 3)) return NULL;
    return (volatile LONG *)p;
}
static inline int sdk_os_fail(const BootContext *c) { return c->status ? BOOT_HOST_STOP : BOOT_HOST_DECLINE; }
/* Uncontended acquire / release of a critical-section word. */
static inline int sdk_os_cs_try(volatile LONG *w, uint32_t tag) { return InterlockedCompareExchange(w, (LONG)tag, 0) == 0; }
static inline int sdk_os_cs_release(volatile LONG *w, uint32_t tag) { return (uint32_t)InterlockedCompareExchange(w, 0, (LONG)tag) == tag; }
static inline int sdk_os_cs_mine(volatile LONG *w, uint32_t tag) { return ((uint32_t)*w & ~SDK_OS_WAITERS) == tag; }
static inline uint32_t sdk_os_ld32(const volatile LONG *p) { return (uint32_t)*p; }
/* Blocking acquire (Enter / LockMutex / SdkMutex::Lock / SdkRecursiveMutex::Lock). With one host thread per
 * guest thread (TOTK_THREADS=host) the owner runs concurrently, so a short critical section usually ends
 * within microseconds: spin briefly (bounded, ~2-5 us) on the word before declining to the kernel wait.
 * No spin while the waiters bit is set (kernel waiters keep their FIFO hand-off) or on the cooperative
 * scheduler (the owner cannot run while we spin). */
#define SDK_OS_SPIN 96
static inline int sdk_os_cs_acquire(volatile LONG *w, uint32_t tag) {
    if (sdk_os_cs_try(w, tag)) return 1;
    if (!boot_mt.host_threads) return 0;
    for (unsigned i = 0; i < SDK_OS_SPIN; ++i) {
        uint32_t v = (uint32_t)*w;
        if (v & SDK_OS_WAITERS) return 0;
        if (!v && sdk_os_cs_try(w, tag)) return 1;
        YieldProcessor();
    }
    return 0;
}

/* ---------------------------------------------------------------- critical section / condition variable */
/* void InternalCriticalSectionImplByHorizon::Enter(this x0) */
static inline int sdk_os_cs_enter(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t thread; uint32_t tag; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !(w = sdk_os_word(c, self, c->x[0]))) return sdk_os_fail(c);
    if (!sdk_os_cs_acquire(w, tag)) return BOOT_HOST_DECLINE;   /* held: the guest code waits in the kernel */
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}
/* bool InternalCriticalSectionImplByHorizon::TryEnter(this x0): never enters the kernel. */
static inline int sdk_os_cs_try_enter(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t thread; uint32_t tag; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !(w = sdk_os_word(c, self, c->x[0]))) return sdk_os_fail(c);
    c->x[0] = (uint64_t)sdk_os_cs_try(w, tag);
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}
/* void InternalCriticalSectionImplByHorizon::Leave(this x0) */
static inline int sdk_os_cs_leave(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t thread; uint32_t tag; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !(w = sdk_os_word(c, self, c->x[0]))) return sdk_os_fail(c);
    if (!sdk_os_cs_release(w, tag)) return BOOT_HOST_DECLINE;   /* waiters bit (kernel hand-off) or not ours */
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}
/* void InternalConditionVariableImplByHorizon::Signal / Broadcast(this x0): no waiters -> nothing to do. */
static inline int sdk_os_cv_signal(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!(w = sdk_os_word(c, self, c->x[0]))) return sdk_os_fail(c);
    if (sdk_os_ld32(w)) return BOOT_HOST_DECLINE;              /* waiters: SignalProcessWideKey in the guest code */
    return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- nn::os::MutexType */
#define SDK_OS_MUTEX_RECURSIVE 0x01
#define SDK_OS_MUTEX_COUNT     0x08
#define SDK_OS_MUTEX_OWNER     0x10
#define SDK_OS_MUTEX_CS        0x18
/* void nn::os::LockMutex(MutexType* x0) (user 0) / bool TryLockMutex (user 1) */
static inline int sdk_os_lock_mutex(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    int try_only = user != NULL;
    uint64_t m = c->x[0], thread, owner = 0; uint32_t tag, count = 0; uint8_t recursive = 0; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !boot_host_read(c, self, m + SDK_OS_MUTEX_RECURSIVE, &recursive, 1) ||
        !boot_host_read(c, self, m + SDK_OS_MUTEX_OWNER, &owner, 8) || !boot_host_read(c, self, m + SDK_OS_MUTEX_COUNT, &count, 4) ||
        !(w = sdk_os_word(c, self, m + SDK_OS_MUTEX_CS))) return sdk_os_fail(c);
    if (!(recursive && owner == thread)) {
        if (try_only ? !sdk_os_cs_try(w, tag) : !sdk_os_cs_acquire(w, tag)) {
            if (!try_only) return BOOT_HOST_DECLINE;            /* contended: kernel wait in the guest code */
            c->x[0] = 0; return BOOT_HOST_DONE;                 /* TryLockMutex: false, as the SDK */
        }
        if (!boot_host_read(c, self, m + SDK_OS_MUTEX_COUNT, &count, 4)) return BOOT_HOST_STOP;   /* now ours */
    }
    ++count;
    if (!boot_host_write(c, self, m + SDK_OS_MUTEX_OWNER, &thread, 8) || !boot_host_write(c, self, m + SDK_OS_MUTEX_COUNT, &count, 4))
        return BOOT_HOST_STOP;
    if (try_only) c->x[0] = 1;
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}
/* void nn::os::UnlockMutex(MutexType* x0) */
static inline int sdk_os_unlock_mutex(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t m = c->x[0], thread, zero = 0; uint32_t tag, count = 0; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !boot_host_read(c, self, m + SDK_OS_MUTEX_COUNT, &count, 4) ||
        !(w = sdk_os_word(c, self, m + SDK_OS_MUTEX_CS))) return sdk_os_fail(c);
    if (!count) return BOOT_HOST_DECLINE;                       /* not locked: the SDK's own behaviour */
    if (count > 1) {
        --count;
        if (!boot_host_write(c, self, m + SDK_OS_MUTEX_COUNT, &count, 4)) return BOOT_HOST_STOP;
        c->exclusive_valid = 0; return BOOT_HOST_DONE;
    }
    if (!sdk_os_cs_mine(w, tag) || (sdk_os_ld32(w) & SDK_OS_WAITERS)) return BOOT_HOST_DECLINE;
    /* SDK order: count = 0, owner = 0, then release. If a waiter arrived in between, undo and decline. */
    uint32_t zero32 = 0;
    if (!boot_host_write(c, self, m + SDK_OS_MUTEX_COUNT, &zero32, 4) || !boot_host_write(c, self, m + SDK_OS_MUTEX_OWNER, &zero, 8))
        return BOOT_HOST_STOP;
    if (!sdk_os_cs_release(w, tag)) {
        if (!boot_host_write(c, self, m + SDK_OS_MUTEX_COUNT, &count, 4) || !boot_host_write(c, self, m + SDK_OS_MUTEX_OWNER, &thread, 8))
            return BOOT_HOST_STOP;
        return BOOT_HOST_DECLINE;
    }
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}

/* ---------------------------------------------------------------- nn::os::SdkMutexType / SdkRecursiveMutexType */
/* user: 0 SdkMutex::Lock, 1 SdkMutex::TryLock, 2 SdkMutex::Unlock (self-owned checks abort in the SDK:
 * those cases decline so the guest code aborts exactly as before). */
static inline int sdk_os_sdk_mutex(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    unsigned op = (unsigned)(uintptr_t)user;
    uint64_t thread; uint32_t tag; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !(w = sdk_os_word(c, self, c->x[0]))) return sdk_os_fail(c);
    int mine = sdk_os_cs_mine(w, tag);
    if (op == 2) {
        if (!mine || !sdk_os_cs_release(w, tag)) return BOOT_HOST_DECLINE;
    } else {
        if (mine) return BOOT_HOST_DECLINE;                     /* SDK aborts: recursive lock of SdkMutex */
        int got = op == 1 ? sdk_os_cs_try(w, tag) : sdk_os_cs_acquire(w, tag);
        if (op == 1) c->x[0] = (uint64_t)got;
        else if (!got) return BOOT_HOST_DECLINE;
    }
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}
/* user: 0 SdkRecursiveMutex::Lock, 1 TryLock, 2 Unlock (count at +4). */
static inline int sdk_os_sdk_recursive(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    unsigned op = (unsigned)(uintptr_t)user;
    uint64_t m = c->x[0], thread; uint32_t tag, count = 0; volatile LONG *w;
    if (!sdk_config()->os) return BOOT_HOST_DECLINE;
    if (!sdk_os_thread(c, self, &thread, &tag) || !(w = sdk_os_word(c, self, m)) || !boot_host_read(c, self, m + 4, &count, 4))
        return sdk_os_fail(c);
    int mine = sdk_os_cs_mine(w, tag);
    if (op == 2) {
        if (!mine || !count) return BOOT_HOST_DECLINE;          /* SDK aborts / undefined: original code */
        uint32_t next = count - 1;
        if (!boot_host_write(c, self, m + 4, &next, 4)) return BOOT_HOST_STOP;
        if (!next && !sdk_os_cs_release(w, tag)) {               /* waiters: restore, kernel hand-off in the guest */
            if (!boot_host_write(c, self, m + 4, &count, 4)) return BOOT_HOST_STOP;
            return BOOT_HOST_DECLINE;
        }
        c->exclusive_valid = 0; return BOOT_HOST_DONE;
    }
    if (mine && count == UINT32_MAX) return BOOT_HOST_DECLINE;  /* SDK aborts on overflow */
    if (!mine) {
        if (op == 0 ? !sdk_os_cs_acquire(w, tag) : !sdk_os_cs_try(w, tag)) {
            if (op == 0) return BOOT_HOST_DECLINE;
            c->x[0] = 0; return BOOT_HOST_DONE;
        }
        if (!boot_host_read(c, self, m + 4, &count, 4)) return BOOT_HOST_STOP;
        if (count == UINT32_MAX) { (void)sdk_os_cs_release(w, tag); return BOOT_HOST_DECLINE; }
    }
    ++count;
    if (!boot_host_write(c, self, m + 4, &count, 4)) return BOOT_HOST_STOP;
    if (op == 1) c->x[0] = 1;
    c->exclusive_valid = 0; return BOOT_HOST_DONE;
}

static const SdkEntry sdk_os_entries[] = {
    {0x131734, "sdk.os.CriticalSection.Enter",    sdk_os_cs_enter,     0, SDK_OS_STEPS},
    {0x1317cc, "sdk.os.CriticalSection.TryEnter", sdk_os_cs_try_enter, 0, SDK_OS_STEPS},
    {0x131804, "sdk.os.CriticalSection.Leave",    sdk_os_cs_leave,     0, SDK_OS_STEPS},
    {0x1310a4, "sdk.os.ConditionVariable.Signal",    sdk_os_cv_signal, 0, SDK_OS_STEPS},
    {0x1310b8, "sdk.os.ConditionVariable.Broadcast", sdk_os_cv_signal, 1, SDK_OS_STEPS},
    {0x127e20, "sdk.os.LockMutex",    sdk_os_lock_mutex,   0, SDK_OS_STEPS},
    {0x127e70, "sdk.os.TryLockMutex", sdk_os_lock_mutex,   1, SDK_OS_STEPS},
    {0x127ee0, "sdk.os.UnlockMutex",  sdk_os_unlock_mutex, 0, SDK_OS_STEPS},
    {0x1282f4, "sdk.os.SdkMutex.Lock",    sdk_os_sdk_mutex, 0, SDK_OS_STEPS},
    {0x128320, "sdk.os.SdkMutex.TryLock", sdk_os_sdk_mutex, 1, SDK_OS_STEPS},
    {0x12834c, "sdk.os.SdkMutex.Unlock",  sdk_os_sdk_mutex, 2, SDK_OS_STEPS},
    {0x1283a0, "sdk.os.SdkRecursiveMutex.Lock",    sdk_os_sdk_recursive, 0, SDK_OS_STEPS},
    {0x1283e0, "sdk.os.SdkRecursiveMutex.TryLock", sdk_os_sdk_recursive, 1, SDK_OS_STEPS},
    {0x128438, "sdk.os.SdkRecursiveMutex.Unlock",  sdk_os_sdk_recursive, 2, SDK_OS_STEPS},
};
#endif
