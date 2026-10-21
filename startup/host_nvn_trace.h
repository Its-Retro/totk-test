#ifndef TOTK_HOST_NVN_TRACE_H
#define TOTK_HOST_NVN_TRACE_H
/* Stage 0: NVN call tracer (TOTK_NVN=trace). Reference data for the native NVN layer; the game's
 * own NVN driver (translated sdk code) still does all the work, so rendering is unchanged.
 *
 * How it attaches: main imports nvnBootstrapLoader (sdk+0x4900b0) through its PLT, so a guest-entry
 * hook sees every bootstrap call. The hook declines (the real bootstrap runs) after redirecting the
 * return into nvn.trace.bootstrap_ret, which replaces the returned driver pointer by the fake address
 * of a tracing wrapper "nvnt:<name>". The game's proc loader then calls the wrapped
 * nvnDeviceGetProcAddress for each of its names, and every result is wrapped the same way, so the
 * game's 534 NVN function-pointer globals end up holding wrapper addresses. A wrapper logs the call
 * (x0-x7, q0-q3, selected argument memory), pushes a return frame (host_fn.h) and jumps to the real
 * driver function; nvn.trace.ret logs the result (x0, q0, selected out memory) and returns to the
 * caller. Real driver addresses are kept as logical offsets in nvn-trace-procs.txt, so a process
 * resumed from a checkpoint finds them again.
 *
 * Output (cwd): nvn-trace.bin (records below), nvn-trace-names.txt (index -> name). Records:
 *   u16 type, u16 index, u32 size (whole record), u64 seq, u64 thread (tpidr), then
 *   type 1 call:    u64 x[8], u64 q[4] (low 8 bytes)
 *   type 2 return:  u64 x0, u64 q0
 *   type 3 memory:  u64 address, u32 arg, u32 phase (0 call, 1 return), bytes (size - 40)
 *   type 4 resolve: u64 real logical offset (index = name)
 * TOTK_NVN_TRACE_LIMIT (default 4000000) caps the records; afterwards wrappers only forward (no
 * return frames). The buffer is flushed when full, at every nvnQueuePresentTexture and at the limit.
 * Decoder: nvn-work/tools/nvn_trace_decode.py. */
#include "host_nvn_names.h"

enum { BOOT_NVNT_CALL = 1, BOOT_NVNT_RET = 2, BOOT_NVNT_MEM = 3, BOOT_NVNT_RESOLVE = 4 };
typedef struct BootNvnTraceSpec { int8_t arg[2]; int8_t count_arg[2]; uint32_t size[2]; int8_t ret_arg; uint32_t ret_size; } BootNvnTraceSpec;
typedef struct BootNvnTrace {
    SRWLOCK lock;
    FILE *file;
    uint8_t *buffer; size_t used;
    volatile LONG64 seq, records, limit;
    uint64_t procs[BOOT_NVN_NAME_COUNT];      /* real driver functions, logical offsets (0 = unknown) */
    BootNvnTraceSpec spec[BOOT_NVN_NAME_COUNT];
    int getproc, present;
    int ready;
} BootNvnTrace;
static inline BootNvnTrace *boot_nvn_trace(void) { static BootNvnTrace t; return &t;   /* zero = SRWLOCK_INIT */ }
#define BOOT_NVNT_BUFFER (UINT64_C(1) << 20)

static inline int boot_nvn_name_index(const char *name) {
    int lo = 0, hi = BOOT_NVN_NAME_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, d = strcmp(boot_nvn_names[mid].name, name);
        if (!d) return mid;
        if (d < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static inline void boot_nvn_trace_flush_locked(BootNvnTrace *t) {
    if (!t->used) return;
    if (!t->file) {
        const char *path = getenv("TOTK_NVN_TRACE_FILE");
        t->file = fopen(path && *path ? path : "nvn-trace.bin", "ab");
    }
    if (t->file) { fwrite(t->buffer, 1, t->used, t->file); fflush(t->file); }
    t->used = 0;
}
static inline void boot_nvn_trace_flush(void) {
    BootNvnTrace *t = boot_nvn_trace();
    AcquireSRWLockExclusive(&t->lock); boot_nvn_trace_flush_locked(t); ReleaseSRWLockExclusive(&t->lock);
}
static inline int boot_nvn_trace_active(void) { BootNvnTrace *t = boot_nvn_trace(); return t->records < t->limit; }
/* Appends one record (header + up to two payload parts). */
static inline void boot_nvn_trace_put(BootContext *c, unsigned type, unsigned index, const void *a, size_t an, const void *b, size_t bn) {
    BootNvnTrace *t = boot_nvn_trace();
    if (InterlockedIncrement64(&t->records) > t->limit) return;
    uint32_t size = (uint32_t)(24 + an + bn);
    uint8_t header[24]; uint16_t ty = (uint16_t)type, ix = (uint16_t)index;
    uint64_t seq = (uint64_t)InterlockedIncrement64(&t->seq), thread = c->tpidr;
    memcpy(header, &ty, 2); memcpy(header + 2, &ix, 2); memcpy(header + 4, &size, 4); memcpy(header + 8, &seq, 8); memcpy(header + 16, &thread, 8);
    AcquireSRWLockExclusive(&t->lock);
    if (!t->buffer) t->buffer = malloc(BOOT_NVNT_BUFFER);
    if (t->buffer && size <= BOOT_NVNT_BUFFER) {
        if (t->used + size > BOOT_NVNT_BUFFER) boot_nvn_trace_flush_locked(t);
        memcpy(t->buffer + t->used, header, 24); memcpy(t->buffer + t->used + 24, a, an);
        if (bn) memcpy(t->buffer + t->used + 24 + an, b, bn);
        t->used += size;
        if (t->records == t->limit) boot_nvn_trace_flush_locked(t);
    }
    ReleaseSRWLockExclusive(&t->lock);
}
/* Reads guest memory for the trace without faulting the guest: a failed read clears the fault. */
static inline size_t boot_nvn_peek(BootContext *c, uint64_t self, uint64_t a, void *dst, size_t n) {
    if (!a || c->status) return 0;
    size_t done = 0;
    while (done < n) {
        size_t chunk = 4096 - (size_t)((a + done) & 4095); if (chunk > n - done) chunk = n - done;
        const uint8_t *p = boot_access(c, a + done, (unsigned)chunk, READ, self);
        if (!p) { c->status = 0; c->fault_pc = c->fault_address = c->fault_width = 0; break; }
        memcpy((uint8_t *)dst + done, p, chunk); done += chunk;
    }
    return done;
}
static inline void boot_nvn_trace_mem(BootContext *c, uint64_t self, unsigned index, uint64_t address, unsigned arg, unsigned phase, size_t n) {
    if (!address || !n) return;
    if (n > 65536) n = 65536;
    uint8_t stack[1024]; uint8_t *buf = n <= sizeof stack ? stack : malloc(n);
    if (!buf) return;
    size_t got = boot_nvn_peek(c, self, address, buf, n);
    struct { uint64_t address; uint32_t arg, phase; } h = {address, arg, phase};
    if (got) boot_nvn_trace_put(c, BOOT_NVNT_MEM, index, &h, 16, buf, got);
    if (buf != stack) free(buf);
}
static inline int boot_nvn_ends(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix); return a >= b && !strcmp(s + a - b, suffix);
}
/* Which argument memory to record (object sizes, encodings, arrays). Object dumps are 256 bytes, an
 * upper bound of the small NVN objects; the decoder derives the real sizes. */
static inline void boot_nvn_trace_specs(BootNvnTrace *t) {
    static const struct { const char *name; BootNvnTraceSpec s; } explicit_specs[] = {
        {"nvnProgramSetShaders", {{2, -1}, {1, -1}, {16, 0}, 0, 512}},
        {"nvnProgramSetShadersExt", {{2, -1}, {1, -1}, {24, 0}, 0, 512}},
        {"nvnCommandBufferBindProgram", {{1, -1}, {-1, -1}, {512, 0}, -1, 0}},
        {"nvnQueueSubmitCommands", {{2, -1}, {1, -1}, {8, 0}, -1, 0}},
        {"nvnCommandBufferSetRenderTargets", {{2, 3}, {1, 1}, {8, 8}, -1, 0}},
        {"nvnCommandBufferSetViewports", {{3, -1}, {2, -1}, {16, 0}, -1, 0}},
        {"nvnCommandBufferSetScissors", {{3, -1}, {2, -1}, {16, 0}, -1, 0}},
        {"nvnCommandBufferSetDepthRanges", {{3, -1}, {2, -1}, {8, 0}, -1, 0}},
        {"nvnCommandBufferBindVertexAttribState", {{2, -1}, {1, -1}, {4, 0}, -1, 0}},
        {"nvnCommandBufferBindVertexStreamState", {{2, -1}, {1, -1}, {8, 0}, -1, 0}},
        {"nvnCommandBufferUpdateUniformBuffer", {{5, -1}, {4, -1}, {1, 0}, -1, 0}},
        {"nvnCommandBufferSetBlendColor", {{1, -1}, {-1, -1}, {16, 0}, -1, 0}},
        {"nvnCommandBufferClearColor", {{2, -1}, {-1, -1}, {16, 0}, -1, 0}},
        {"nvnCommandBufferCopyTextureToTexture", {{3, 6}, {-1, -1}, {24, 24}, -1, 0}},
        {"nvnCommandBufferCopyBufferToTexture", {{4, -1}, {-1, -1}, {24, 0}, -1, 0}},
        {"nvnCommandBufferCopyTextureToBuffer", {{3, -1}, {-1, -1}, {24, 0}, -1, 0}},
        {"nvnCommandBufferClearTexture", {{2, 3}, {-1, -1}, {24, 16}, -1, 0}},
        {"nvnTexturePoolRegisterTexture", {{2, 3}, {-1, -1}, {256, 64}, 0, 64}},
        {"nvnSamplerPoolRegisterSampler", {{2, -1}, {-1, -1}, {256, 0}, 0, 64}},
        {"nvnWindowBuilderSetTextures", {{2, -1}, {1, -1}, {8, 0}, -1, 0}},
        {"nvnDeviceGetInteger", {{-1, -1}, {-1, -1}, {0, 0}, 2, 4}},
        {"nvnTextureWriteTexels", {{2, -1}, {-1, -1}, {24, 0}, -1, 0}},
        {"nvnTextureReadTexels", {{2, -1}, {-1, -1}, {24, 0}, -1, 0}},
        {"nvnCommandBufferCallCommands", {{2, -1}, {1, -1}, {8, 0}, -1, 0}},
        /* state objects and pools are read when the command is recorded (copied by value) */
        {"nvnCommandBufferBindBlendState", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferBindChannelMaskState", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferBindColorState", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferBindDepthStencilState", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferBindMultisampleState", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferBindPolygonState", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferSetTexturePool", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
        {"nvnCommandBufferSetSamplerPool", {{1, -1}, {-1, -1}, {32, 0}, -1, 0}},
    };
    for (int i = 0; i < BOOT_NVN_NAME_COUNT; ++i) {
        const char *n = boot_nvn_names[i].name; BootNvnTraceSpec s = {{-1, -1}, {-1, -1}, {0, 0}, -1, 0};
        if (boot_nvn_ends(n, "Initialize")) { s.arg[0] = 1; s.size[0] = 256; s.ret_arg = 0; s.ret_size = 256; }
        else if (strstr(n, "State") && strstr(n, "Set") && !strstr(n, "CommandBuffer")) { s.ret_arg = 0; s.ret_size = 32; }
        else if (strstr(n, "Builder") && boot_nvn_ends(n, "SetDefaults")) { s.ret_arg = 0; s.ret_size = 256; }
        else if (!strncmp(n, "nvnTextureViewSet", 17)) { s.ret_arg = 0; s.ret_size = 64; }
        for (size_t k = 0; k < sizeof explicit_specs / sizeof explicit_specs[0]; ++k)
            if (!strcmp(explicit_specs[k].name, n)) s = explicit_specs[k].s;
        t->spec[i] = s;
    }
}
static inline uint64_t boot_nvn_trace_address(int index) {
    char name[112]; snprintf(name, sizeof name, "nvnt:%s", boot_nvn_names[index].name); return boot_host_fn_address(name);
}
static inline void boot_nvn_trace_learn(BootContext *c, int index, uint64_t real) {
    BootNvnTrace *t = boot_nvn_trace();
    uint64_t logical = boot_offset(c, real);
    if (index < 0 || logical == UINT64_MAX || t->procs[index] == logical) return;
    t->procs[index] = logical;
    boot_nvn_trace_put(c, BOOT_NVNT_RESOLVE, (unsigned)index, &logical, 8, NULL, 0);
    FILE *f = fopen("nvn-trace-procs.txt", "a");
    if (f) { fprintf(f, "%s %llx\n", boot_nvn_names[index].name, (unsigned long long)logical); fclose(f); }
}
/* A driver pointer returned by the bootstrap or GetProcAddress for `name` -> the wrapper's address. */
static inline uint64_t boot_nvn_trace_wrap(BootContext *c, uint64_t self, uint64_t name_address, uint64_t real) {
    char name[112] = {0};
    if (!real || boot_host_fn_is_host(real)) return real;
    size_t got = boot_nvn_peek(c, self, name_address, name, sizeof name - 1);
    name[got] = 0;
    int index = boot_nvn_name_index(name);
    if (index < 0) return real;
    boot_nvn_trace_learn(c, index, real);
    return boot_nvn_trace_address(index);
}
/* sdk nvnBootstrapLoader hook: let the real bootstrap run and wrap its result. */
static inline int boot_nvn_trace_bootstrap(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    return boot_host_return_to(c, self, boot_host_fn_address("nvn.trace.bootstrap_ret"), c->x[0], 0) ? BOOT_HOST_DECLINE : BOOT_HOST_STOP;
}
static inline int boot_nvn_trace_bootstrap_ret(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; uint64_t name = 0;
    if (!boot_host_frame_pop(c, self, target, &name, NULL)) return BOOT_HOST_STOP;
    c->x[0] = boot_nvn_trace_wrap(c, self, name, c->x[0]);
    return BOOT_HOST_DONE;
}
/* GPU virtual address -> guest CPU address through the nvhost-as mappings (runtime_support only). */
static inline uint64_t boot_nvn_gpu_cpu(BootContext *c, uint64_t va) {
#ifdef TOTK_DRAW_PATH
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native;
    if (w) for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (m->size && m->cpu && va >= m->gpu && va - m->gpu < boot_gpu_mapping_backing_size(m)) return m->cpu + (va - m->gpu);
    }
#else
    (void)c; (void)va;
#endif
    return 0;
}
/* Calibration dumps beyond the argument specs (ABI recovery for the native path):
 *  ProgramSetShaders(Ext): per shader entry k the control section (arg 100+k, CPU pointer, 2 KiB) and the
 *  shader binary start at its GPU address (arg 200+k, 512 bytes; header + SPH);
 *  CommandBufferBindProgram: the program's precompiled binding words (arg 300; pointer at +0x18, words at +0x14). */
static inline void boot_nvn_trace_extra(BootContext *c, uint64_t self, int index) {
    static int set_shaders = -2, set_shaders_ext, bind_program;
    if (set_shaders == -2) { set_shaders = boot_nvn_name_index("nvnProgramSetShaders"); set_shaders_ext = boot_nvn_name_index("nvnProgramSetShadersExt");
                             bind_program = boot_nvn_name_index("nvnCommandBufferBindProgram"); }
    if (index == set_shaders || index == set_shaders_ext) {
        uint64_t stride = index == set_shaders ? 16 : 24, count = c->x[1] < 8 ? c->x[1] : 8;
        for (uint64_t k = 0; k < count; ++k) {
            uint64_t e[2] = {0, 0};
            if (boot_nvn_peek(c, self, c->x[2] + k * stride, e, 16) != 16) break;
            boot_nvn_trace_mem(c, self, (unsigned)index, e[1], 100 + (unsigned)k, 0, 2048);
            uint64_t cpu = boot_nvn_gpu_cpu(c, e[0]);
            if (cpu) boot_nvn_trace_mem(c, self, (unsigned)index, cpu, 200 + (unsigned)k, 0, 512);
        }
    } else if (index == bind_program && c->x[1]) {
        static uint64_t seen[8192]; uint64_t h = (c->x[1] >> 4) * UINT64_C(0x9e3779b97f4a7c15) >> 51;   /* first binding of each program only */
        if (seen[h] == c->x[1]) return;
        seen[h] = c->x[1];
        uint32_t words = 0; uint64_t blob = 0;
        if (boot_nvn_peek(c, self, c->x[1] + 0x14, &words, 4) == 4 && boot_nvn_peek(c, self, c->x[1] + 0x18, &blob, 8) == 8 && words && words <= 1024)
            boot_nvn_trace_mem(c, self, (unsigned)index, blob, 300, 0, (size_t)words * 4);
    }
}
static inline int boot_nvn_trace_call(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    BootNvnTrace *t = boot_nvn_trace(); int index = (int)(intptr_t)user;
    uint64_t logical = t->procs[index], real = logical ? boot_address(c, logical) : 0;
    if (!real || real == UINT64_MAX) {
        fprintf(stderr, "nvn trace: no driver address for %s\n", boot_nvn_names[index].name);
        boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP;
    }
    int logging = boot_nvn_trace_active();
    if (logging) {
        uint64_t regs[12]; memcpy(regs, c->x, 64);
        for (unsigned k = 0; k < 4; ++k) memcpy(&regs[8 + k], c->q[k], 8);
        boot_nvn_trace_put(c, BOOT_NVNT_CALL, (unsigned)index, regs, sizeof regs, NULL, 0);
        const BootNvnTraceSpec *s = &t->spec[index];
        for (unsigned k = 0; k < 2; ++k) if (s->arg[k] >= 0) {
            uint64_t n = s->count_arg[k] >= 0 ? c->x[s->count_arg[k]] * (s->size[k] ? s->size[k] : 1) : s->size[k];
            boot_nvn_trace_mem(c, self, (unsigned)index, c->x[s->arg[k]], (unsigned)s->arg[k], 0, (size_t)(n > 65536 ? 65536 : n));
        }
        if (index == t->present) boot_nvn_trace_flush();
        boot_nvn_trace_extra(c, self, index);
    }
    if (index == t->getproc || logging) {
        /* return frame: s0 = index, s1 = the argument the return side needs (GetProcAddress: the name) */
        uint64_t keep = index == t->getproc ? c->x[1] : t->spec[index].ret_arg >= 0 ? c->x[t->spec[index].ret_arg] : 0;
        return boot_host_call_guest(c, self, target, real, boot_host_fn_address("nvn.trace.ret"), (uint64_t)index, keep);
    }
    *target = real;   /* past the limit: plain forward, x30 untouched */
    return BOOT_HOST_DONE;
}
static inline int boot_nvn_trace_ret(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; BootNvnTrace *t = boot_nvn_trace(); uint64_t s0 = 0, keep = 0;
    if (!boot_host_frame_pop(c, self, target, &s0, &keep)) return BOOT_HOST_STOP;
    int index = (int)s0;
    if (index < 0 || index >= BOOT_NVN_NAME_COUNT) { boot_fault(c, BOOT_TARGET, self, self, 0); return BOOT_HOST_STOP; }
    if (boot_nvn_trace_active()) {
        uint64_t r[2] = {c->x[0], 0}; memcpy(&r[1], c->q[0], 8);
        boot_nvn_trace_put(c, BOOT_NVNT_RET, (unsigned)index, r, 16, NULL, 0);
        if (index != t->getproc && t->spec[index].ret_arg >= 0)
            boot_nvn_trace_mem(c, self, (unsigned)index, keep, (unsigned)t->spec[index].ret_arg, 1, t->spec[index].ret_size);
    }
    if (index == t->getproc) c->x[0] = boot_nvn_trace_wrap(c, self, keep, c->x[0]);
    return BOOT_HOST_DONE;
}
/* Registration (TOTK_NVN=trace). */
static inline void boot_nvn_trace_register(void) {
    BootNvnTrace *t = boot_nvn_trace();
    const char *limit = getenv("TOTK_NVN_TRACE_LIMIT");
    t->limit = limit && *limit ? _atoi64(limit) : 4000000;
    t->getproc = boot_nvn_name_index("nvnDeviceGetProcAddress");
    t->present = boot_nvn_name_index("nvnQueuePresentTexture");
    boot_nvn_trace_specs(t);
    FILE *f = fopen("nvn-trace-procs.txt", "r");   /* driver addresses learnt by an earlier process (resume) */
    if (f) {
        char name[112]; unsigned long long logical;
        while (fscanf(f, "%111s %llx", name, &logical) == 2) { int i = boot_nvn_name_index(name); if (i >= 0) t->procs[i] = logical; }
        fclose(f);
    }
    FILE *names = fopen("nvn-trace-names.txt", "w");
    for (int i = 0; i < BOOT_NVN_NAME_COUNT; ++i) {
        char name[112]; snprintf(name, sizeof name, "nvnt:%s", boot_nvn_names[i].name);
        boot_host_fn_register(name, boot_nvn_trace_call, (void *)(intptr_t)i, 0, 0);
        if (names) fprintf(names, "%d %s\n", i, boot_nvn_names[i].name);
    }
    if (names) fclose(names);
    boot_host_fn_register("nvn.trace.ret", boot_nvn_trace_ret, NULL, 0, 0);
    boot_host_fn_register("nvn.trace.bootstrap_ret", boot_nvn_trace_bootstrap_ret, NULL, 0, 0);
    boot_host_fn_hook(3, 0x4900b0, "nvn.bootstrap", boot_nvn_trace_bootstrap, NULL, 0, 0);
    t->ready = 1;
}
#endif
