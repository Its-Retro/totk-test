#ifndef TOTK_HOST_FN_H
#define TOTK_HOST_FN_H
/* Host functions: native C implementations that guest code reaches like guest functions. One
 * mechanism, shared by the NVN API layer (host_nvn.h) and native SDK services (svc-work).
 *
 * Two kinds of entries, one registry:
 *  - Fake-address functions (boot_host_fn_register). Their guest-visible address depends only on
 *    the name:  BOOT_HOST_FN_BASE + 16 * hash24(name)  (FNV-1a 32 folded to 24 bits). The range
 *    0x7ff000000000.. is above the 39-bit guest address space and outside every module, so no page
 *    and no guest mapping can exist there. Guest code gets these addresses as function pointers
 *    (NVN proc addresses, vtable or GOT entries) and calls them with BLR/BR. Since the address is
 *    name-derived, pointers stored in guest memory stay valid across processes, checkpoints, DLL
 *    swaps and builds; a hash collision is reported at registration (rename one of the two).
 *  - Guest-entry hooks (boot_host_fn_hook): a host function runs in place of the translated guest
 *    function at (module, offset), or declines and the translated code runs. The hooked page is
 *    marked in boot_host_fn_mark.
 *
 * Dispatch. Every cross-page transfer, BR/BLR/RET and boot_execute iteration looks up the target
 * page (BOOT_PAGE_DISPATCH / boot_execute). When the target has no page, or its page is marked,
 * boot_host_fn_dispatch (runtime_support) is asked first; the hit path costs one byte load of
 * boot_host_fn_mark. Direct branches inside one page do not pass the dispatch: hook only entries
 * reached by calls from other pages, PLT stubs, vtables or pointers (svc-work found no direct
 * callers of its candidates; nvnBootstrapLoader is reached through main's PLT).
 *
 * Calling convention (guest AAPCS64): integer/pointer arguments in c->x[0..7], floating point in
 * the low bytes of c->q[0..7], indirect result pointer in x8, stack arguments at c->sp; results in
 * c->x[0] / c->q[0]. A host function
 *     int fn(BootContext *c, uint64_t self, uint64_t *target, void *user)
 * is entered with *target = c->x[30] (the return address) and `self` = the called address, and
 * returns:
 *   BOOT_HOST_DONE     continue at *target (the return, or another target: a guest function to
 *                      tail-call, a thread switch; see boot_host_call_guest);
 *   BOOT_HOST_DECLINE  run the translated guest code instead (hooks; for a fake address this is a
 *                      BOOT_TARGET fault). Must not have changed guest-visible state, except on
 *                      purpose (wrappers: boot_host_return_to, argument rewrites);
 *   BOOT_HOST_STOP     stop with c->status (boot_fault; BOOT_TARGET at self if unset). boot_resume
 *                      continues at c->fault_pc == self, so the function runs again from its
 *                      start: stops must be restartable (validate first, commit last).
 * Callee-saved registers (x19-x28, x29, sp, d8-d15) must be preserved unless the function is a
 * deliberate frame helper (boot_host_call_guest / boot_host_return_to).
 *
 * Flags: BOOT_HOST_FN_KERNEL runs the function as a kernel entry (pseudo-SVC BOOT_SVC_HOSTCALL
 * through boot_svc): with TOTK_CORES>1 boot_mt_svc takes the kernel lock and runs it on the master
 * context, so it may use every kernel structure and switch threads (return DONE with *target set
 * by boot_svc-style logic). Without the flag it runs on the calling core's context, concurrently
 * with other guest threads: it must only touch guest memory and its own thread-safe host state.
 * `steps` is charged to the guest step counter per completed call (guest time sees work done).
 *
 * Enable switches: TOTK_HOST_FN_DENY="name,name,..." registers those entries disabled (they decline
 * / fault as if absent); boot_host_fn_enable toggles at run time. Subsystems add their own modes
 * (native / shadow / count) on top; shadow mode: compute the native answer without side effects,
 * then boot_host_return_to(cont) + DECLINE, and compare in `cont` when the guest code returns.
 *
 * State. The registry is host state, rebuilt in every process by the subsystems'
 * registration functions (boot_host_fn_subsystems, run once on the first dispatch and at every
 * boot_execute entry); it is never checkpointed and adds no BootContext field. Persistent
 * subsystem state belongs in guest memory (guest objects, guest stack frames, a state page). */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define BOOT_HOST_FN_BASE  UINT64_C(0x00007ff000000000)
#define BOOT_HOST_FN_BITS  24
#define BOOT_HOST_FN_END   (BOOT_HOST_FN_BASE + (UINT64_C(16) << BOOT_HOST_FN_BITS))
#define BOOT_HOST_FN_TABLE 16384                       /* registry slots (power of two), half usable */
#define BOOT_HOST_KEY_GUEST (UINT64_C(1) << 63)        /* key of a guest-entry hook: flag | logical offset */
enum { BOOT_HOST_DONE = 0, BOOT_HOST_DECLINE = 1, BOOT_HOST_STOP = -1 };
/* BOOT_HOST_FN_NOLOCK: the function never touches the renderer (it only uses internally locked host state and
 * guest memory), so it runs without the renderer lock when guest threads run on several host threads. */
enum { BOOT_HOST_FN_KERNEL = 1, BOOT_HOST_FN_NOLOCK = 2 };
#define BOOT_HOST_FRAME_MAGIC UINT64_C(0x4d52465453544f48)   /* "HOSTFRM" */
#ifndef BOOT_SVC_HOSTCALL
#define BOOT_SVC_HOSTCALL 0x100u
#endif

typedef int (*BootHostFunction)(BootContext *c, uint64_t self, uint64_t *target, void *user);
typedef struct BootHostFnEntry {
    volatile uint64_t key;            /* 0 = empty; published last. Fake address, or KEY_GUEST | offset */
    BootHostFunction fn; void *user;
    uint32_t flags, steps;
    volatile LONG enabled;
    volatile LONG64 calls, declines;
    char name[96];
} BootHostFnEntry;
typedef struct BootHostFnRegistry {
    SRWLOCK lock; INIT_ONCE once;
    uint32_t count, collisions;
    BootHostFnEntry e[BOOT_HOST_FN_TABLE];
} BootHostFnRegistry;
/* One registry per runtime unit (function-local static, so units that only see declarations get no
 * unused objects). Only runtime_support registers and dispatches. */
static inline BootHostFnRegistry *boot_host_fns(void) {
    static BootHostFnRegistry r;   /* zero = SRWLOCK_INIT, INIT_ONCE_STATIC_INIT */
    return &r;
}

static inline uint32_t boot_host_fn_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) { h ^= *p; h *= 16777619u; }
    return (h ^ (h >> 24)) & ((1u << BOOT_HOST_FN_BITS) - 1);
}
static inline uint64_t boot_host_fn_address(const char *name) { return BOOT_HOST_FN_BASE + ((uint64_t)boot_host_fn_hash(name) << 4); }
static inline int boot_host_fn_is_host(uint64_t a) { return a >= BOOT_HOST_FN_BASE && a < BOOT_HOST_FN_END && !(a & 15); }
static inline uint32_t boot_host_fn_slot(uint64_t key) {
    uint64_t k = key * UINT64_C(0x9e3779b97f4a7c15); return (uint32_t)(k >> 50) & (BOOT_HOST_FN_TABLE - 1);
}
static inline BootHostFnEntry *boot_host_fn_find(uint64_t key) {
    BootHostFnRegistry *r = boot_host_fns();
    uint32_t i = boot_host_fn_slot(key);
    for (uint32_t n = 0; n < BOOT_HOST_FN_TABLE; ++n, i = (i + 1) & (BOOT_HOST_FN_TABLE - 1)) {
        uint64_t k = r->e[i].key; MemoryBarrier();
        if (!k) return NULL;
        if (k == key) return &r->e[i];
    }
    return NULL;
}
static inline BootHostFnEntry *boot_host_fn_by_name(const char *name) {
    BootHostFnRegistry *r = boot_host_fns();
    for (uint32_t i = 0; i < BOOT_HOST_FN_TABLE; ++i) if (r->e[i].key && !strcmp(r->e[i].name, name)) return &r->e[i];
    return NULL;
}
static inline int boot_host_fn_denied(const char *name) {
    const char *deny = getenv("TOTK_HOST_FN_DENY"); size_t n = strlen(name);
    for (const char *p = deny; p && *p; ) {
        const char *end = strchr(p, ','); size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return 1;
        p = end ? end + 1 : p + len;
    }
    return 0;
}
static inline uint64_t boot_host_fn_add(uint64_t key, const char *name, BootHostFunction fn, void *user, uint32_t flags, uint32_t steps) {
    BootHostFnRegistry *r = boot_host_fns(); uint64_t result = 0;
    if (!fn || !name || strlen(name) >= sizeof r->e[0].name) return 0;
    AcquireSRWLockExclusive(&r->lock);
    uint32_t i = boot_host_fn_slot(key);
    for (uint32_t n = 0; n < BOOT_HOST_FN_TABLE; ++n, i = (i + 1) & (BOOT_HOST_FN_TABLE - 1)) {
        BootHostFnEntry *e = &r->e[i];
        if (e->key == key) {
            if (strcmp(e->name, name)) { ++r->collisions; fprintf(stderr, "host fn: %s collides with %s\n", name, e->name); break; }
            e->user = user; e->flags = flags; e->steps = steps; MemoryBarrier(); e->fn = fn; result = key; break;   /* re-registration */
        }
        if (!e->key) {
            if (r->count * 2 >= BOOT_HOST_FN_TABLE) break;
            snprintf(e->name, sizeof e->name, "%s", name); e->fn = fn; e->user = user; e->flags = flags; e->steps = steps;
            e->calls = e->declines = 0; e->enabled = !boot_host_fn_denied(name);
            MemoryBarrier(); e->key = key; ++r->count; result = key; break;
        }
    }
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}
/* A function at its name-derived fake address. Returns the address (0: collision or full). */
static inline uint64_t boot_host_fn_register(const char *name, BootHostFunction fn, void *user, uint32_t flags, uint32_t steps) {
    return boot_host_fn_add(boot_host_fn_address(name), name, fn, user, flags, steps);
}
/* A hook on the guest entry at module (0 rtld, 1 main, 2 subsdk0, 3 sdk) + offset. Returns 1 on success. */
static inline int boot_host_fn_hook(unsigned module, uint64_t offset, const char *name, BootHostFunction fn, void *user, uint32_t flags, uint32_t steps) {
    if (module > 3 || offset >= (UINT64_C(1) << 28)) return 0;
    uint64_t logical = ((uint64_t)module << 28) | offset;
    if (!boot_host_fn_add(BOOT_HOST_KEY_GUEST | logical, name, fn, user, flags, steps)) return 0;
    boot_host_fn_mark[logical >> 12] = 1;
    return 1;
}
static inline int boot_host_fn_enable(const char *name, int on) {
    BootHostFnEntry *e = boot_host_fn_by_name(name); if (!e) return 0;
    InterlockedExchange(&e->enabled, on != 0); return 1;
}
/* Name of the host function at a fake address or a hooked guest logical offset; NULL if none. */
static inline const char *boot_host_fn_name(uint64_t key) {
    BootHostFnEntry *e = boot_host_fn_find(key); return e ? e->name : NULL;
}
/* Diagnostics: key, calls, declines, name of every entry. */
static inline void boot_host_fn_dump(FILE *f) {
    BootHostFnRegistry *r = boot_host_fns();
    for (uint32_t i = 0; i < BOOT_HOST_FN_TABLE; ++i)
        if (r->e[i].key) fprintf(f, "%016llx %10lld %10lld %s%s\n", (unsigned long long)r->e[i].key, (long long)r->e[i].calls,
                                 (long long)r->e[i].declines, r->e[i].name, r->e[i].enabled ? "" : " (disabled)");
}

/* ---------------------------------------------------------------- guest access helpers */
/* Copies spanning pages go page by page through boot_access (fastmem or TLB); faults name `self`. */
static inline int boot_host_read(BootContext *c, uint64_t self, uint64_t a, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        size_t chunk = 4096 - (size_t)(a & 4095); if (chunk > n) chunk = n;
        const uint8_t *p = boot_access(c, a, (unsigned)chunk, READ, self); if (!p) return 0;
        memcpy(d, p, chunk); d += chunk; a += chunk; n -= chunk;
    }
    return 1;
}
static inline int boot_host_write(BootContext *c, uint64_t self, uint64_t a, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        size_t chunk = 4096 - (size_t)(a & 4095); if (chunk > n) chunk = n;
        uint8_t *p = boot_access(c, a, (unsigned)chunk, WRITE, self); if (!p) return 0;
        memcpy(p, s, chunk); s += chunk; a += chunk; n -= chunk;
    }
    return 1;
}
/* NUL-terminated guest string into buf (truncated to cap-1); 0 on fault. */
static inline int boot_host_read_string(BootContext *c, uint64_t self, uint64_t a, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        const uint8_t *p = boot_access(c, a + n, 1, READ, self); if (!p) return 0;
        if (!(buf[n] = (char)*p)) return 1;
        ++n;
    }
    buf[n] = 0; return 1;
}
static inline float boot_host_arg_f32(const BootContext *c, unsigned i) { float f; memcpy(&f, c->q[i], 4); return f; }
static inline double boot_host_arg_f64(const BootContext *c, unsigned i) { double f; memcpy(&f, c->q[i], 8); return f; }
static inline void boot_host_ret_f32(BootContext *c, float f) { memset(c->q[0], 0, 16); memcpy(c->q[0], &f, 4); }

/* ---------------------------------------------------------------- frames on the guest stack */
/* A 32-byte frame {return address, s0, s1, magic} below sp lets a host function regain control
 * after guest code returns, with all state in guest memory (checkpoint-safe):
 *  - boot_host_call_guest: call guest function `fn` (arguments set by the caller), continue in host
 *    function `cont` when it returns (return DONE);
 *  - boot_host_return_to: for hooks and wrappers that DECLINE: the translated function runs and
 *    returns into `cont` (return capture, shadow comparison).
 * `cont` calls boot_host_frame_pop, which restores sp and x30 and sets *target to the original
 * return address. The frame moves sp by 32: only for callees without stack-passed arguments. */
static inline int boot_host_return_to(BootContext *c, uint64_t self, uint64_t cont, uint64_t s0, uint64_t s1) {
    uint64_t frame[4] = {c->x[30], s0, s1, BOOT_HOST_FRAME_MAGIC};
    if (!boot_host_write(c, self, c->sp - 32, frame, 32)) return 0;
    c->sp -= 32; c->x[30] = cont;
    return 1;
}
static inline int boot_host_call_guest(BootContext *c, uint64_t self, uint64_t *target, uint64_t fn, uint64_t cont, uint64_t s0, uint64_t s1) {
    if (!boot_host_return_to(c, self, cont, s0, s1)) return BOOT_HOST_STOP;
    *target = fn;
    return BOOT_HOST_DONE;
}
static inline int boot_host_frame_pop(BootContext *c, uint64_t self, uint64_t *target, uint64_t *s0, uint64_t *s1) {
    uint64_t frame[4];
    if (!boot_host_read(c, self, c->sp, frame, 32)) return 0;
    if (frame[3] != BOOT_HOST_FRAME_MAGIC) { boot_fault(c, BOOT_TARGET, self, c->sp, 32); return 0; }
    c->sp += 32; c->x[30] = frame[0]; *target = frame[0];
    if (s0) *s0 = frame[1];
    if (s1) *s1 = frame[2];
    return 1;
}

/* ---------------------------------------------------------------- startup and dispatch */
#ifdef TOTK_HOST_FN_SUBSYSTEMS   /* tests: statements registering their functions */
static inline void boot_host_fn_subsystems(void) { TOTK_HOST_FN_SUBSYSTEMS }
#else
static inline void boot_host_fn_subsystems(void);   /* boot_runtime.h, after the subsystem headers */
#endif
static inline BOOL CALLBACK boot_host_fn_once(PINIT_ONCE once, void *parameter, void **context) {
    (void)once; (void)parameter; (void)context;
    boot_host_fn_subsystems();
    return TRUE;
}
static inline void boot_host_fn_startup(void) { InitOnceExecuteOnce(&boot_host_fns()->once, boot_host_fn_once, NULL, NULL); }

/* The function selected for the current kernel-mode call (BOOT_HOST_FN_KERNEL), per host thread. */
static inline BootHostFnEntry **boot_host_fn_pending(void) { static _Thread_local BootHostFnEntry *p; return &p; }
/* boot_svc_kernel case BOOT_SVC_HOSTCALL (on the master context under the kernel lock with
 * TOTK_CORES>1): 2 = continue at *target, 0 = declined, <0 = stop. */
static inline int boot_host_fn_kernel(BootContext *c, uint64_t pc, uint64_t next, uint64_t *target) {
    BootHostFnEntry *e = *boot_host_fn_pending();
    if (!e) return -1;
    *target = next;
    int rc = e->fn(c, pc, target, e->user);
    if (rc == BOOT_HOST_DECLINE && !c->status) return 0;
    if (rc < 0 || c->status) { if (!c->status) boot_fault(c, BOOT_TARGET, pc, pc, 0); return -1; }
    return 2;
}
/* Debug watch (TOTK_WATCH_ADDR=hex): logs every change of 64 guest bytes there, tagged with the host function
 * that made it, or "between" for changes made since the previous host call (game code or late GPU writes). */
static inline uint64_t boot_host_fn_watch_addr(void) {
    static volatile LONG once; static uint64_t addr;
    if (!once) { char v[40]; DWORD n = GetEnvironmentVariableA("TOTK_WATCH_ADDR", v, sizeof v); addr = n && n < sizeof v ? strtoull(v, NULL, 16) : 0; once = 1; }
    return addr;
}
static inline void boot_host_fn_watch(BootContext *c, uint64_t self, const char *who) {
    static uint8_t last[64]; static int have; static unsigned lines;
    uint64_t a = boot_host_fn_watch_addr(); uint8_t now[64];
    if (!a || lines > 400) return;    { unsigned k = 0; while (k < c->region_count && !(a >= c->regions[k].base && a - c->regions[k].base + sizeof now <= c->regions[k].size)) ++k;      if (k == c->region_count || !c->regions[k].data) return;   /* not mapped yet: never fault the guest */      memcpy(now, c->regions[k].data + (a - c->regions[k].base), sizeof now); }
    if (have && !memcmp(now, last, sizeof now)) return;
    FILE *f = fopen("watch.log", "a");
    if (f) { fprintf(f, "%s step %llu pc %llx lr %llx:", have ? who : "first", (unsigned long long)c->steps, (unsigned long long)self, (unsigned long long)c->x[30]);
        for (unsigned i = 0; i < sizeof now; ++i) fprintf(f, "%s%02x", i % 8 ? "" : " ", now[i]);
        fprintf(f, "\n"); fclose(f); ++lines; }
    memcpy(last, now, sizeof now); have = 1;
}
/* Dispatch miss / marked-page branch: next[0] is the transfer target. Returns 0 when it is not an
 * enabled host function or the function declined (the caller continues: translated page, or the
 * BOOT_TARGET fault); 1 when handled: next[0] is the continuation (*direct cleared) or c->status is
 * set. next == NULL only runs startup (boot_execute entry). */
static inline int boot_host_fn_call(BootContext *c, uint64_t *next, uint32_t *direct) {
    boot_host_fn_startup();
    if (!next || c->status) return 0;
    uint64_t self = next[0], key = self;
    if (!boot_host_fn_is_host(self)) {
        uint64_t logical = boot_offset(c, self);
        if (logical >= ((uint64_t)BOOT_PAGE_TABLE_SIZE << 12) || !boot_host_fn_mark[logical >> 12]) return 0;
        key = BOOT_HOST_KEY_GUEST | logical;
    }
    BootHostFnEntry *e = boot_host_fn_find(key);
    BootHostFunction fn = e ? e->fn : NULL;
    if (!fn || !e->enabled) return 0;
    InterlockedIncrement64(&e->calls);
    uint64_t target = c->x[30];
    int rc;
    if (e->flags & BOOT_HOST_FN_KERNEL) {
        *boot_host_fn_pending() = e;
        int r = boot_svc(c, self, c->x[30], BOOT_SVC_HOSTCALL, &target);   /* 1: at next, 2: at target */
        *boot_host_fn_pending() = NULL;
        if (r == 1) target = c->x[30];
        rc = r == 0 && !c->status ? BOOT_HOST_DECLINE : r < 0 || c->status ? BOOT_HOST_STOP : BOOT_HOST_DONE;
    } else {
        if (boot_host_fn_watch_addr()) boot_host_fn_watch(c, self, "between");
#ifdef TOTK_DRAW_PATH
        BootNwContext nw_previous;int nw_traced=0;
        if(boot_nw_on()){unsigned kind=boot_nw_kind(key);if(kind){nw_previous=boot_nw_enter(kind,key,e->flags);nw_traced=1;}}
#endif
        int render = (e->flags & BOOT_HOST_FN_NOLOCK) ? 0 : boot_render_lock();   /* host functions reach the renderer (boot_mt_late.h) */
        rc = fn(c, self, &target, e->user);
        boot_render_unlock(render);
#ifdef TOTK_DRAW_PATH
        if(nw_traced)boot_nw_leave(nw_previous,rc,c->status);
#endif
        if (boot_host_fn_watch_addr()) boot_host_fn_watch(c, self, e->name);
    }
    if (rc == BOOT_HOST_DECLINE && !c->status) { InterlockedIncrement64(&e->declines); return 0; }
    if (rc < 0 || c->status) { if (!c->status) boot_fault(c, BOOT_TARGET, self, self, 0); return 1; }
    c->steps += e->steps;
    next[0] = target; *direct = 0;
    return 1;
}
#endif
