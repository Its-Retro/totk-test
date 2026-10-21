#ifndef TOTK_HOST_SDK_NVDRV_H
#define TOTK_HOST_SDK_NVDRV_H
/* Native SDK layer, family "nvdrv": the NVIDIA OS layer's NvOsDrvIoctl (sdk) for the nvmap and nvhost-as
 * mapping requests, handled in-process by boot_nvdrv_mapping_ioctl (boot_runtime.h), the same function
 * the nvdrv IPC Ioctl handler calls, so both reach the same window state (w->nvmap, w->gpu_mappings,
 * w->nv_address_spaces). No IPC message is built or parsed.
 *
 * NvOsDrvIoctl(fd w0, request w1, buf x2) -> NvError w0 (sdk+0x3f91a0, symbol present): the request's
 * bits 30 / 31 say the buffer is read / written, bits 16-29 give its size; the library passes the fd it
 * got from NvOsDrvOpen (an nvdrv fd: the runtime's w->nv_device_fds) unchanged to the session's Ioctl.
 * Handled: nvmap CREATE / ALLOC / FREE / PARAM / GET_ID / FROM_ID, as MAP_BUFFER_EX / UNMAP_BUFFER / REMAP /
 * ALLOC_SPACE / FREE_SPACE. Everything else, and every request the shared handler rejects (it rejects
 * before committing) declines: the translated library code then sends the IPC request, which the
 * runtime answers (or faults on) exactly as before. A request the guest driver itself fails (nvmap on an
 * unknown handle, exact mode, see host_nvmap.h) returns that NvError here, as the driver would.
 * NvOsDrvOpen / NvOsDrvClose stay on that path (device open / close happen once at boot; the session is
 * still needed for channel submits).
 *
 * Kernel entry (BOOT_HOST_FN_KERNEL): the handler reads guest regions and allocation state like the IPC
 * handler, so it runs under the kernel lock as that one does. */

#ifndef SDK_NVDRV_STEPS
#define SDK_NVDRV_STEPS 200
#endif

static inline int sdk_nvdrv_is_mapping(uint32_t request) {
    switch (request) {
    case UINT32_C(0xc008010e): case UINT32_C(0xc0080101): case UINT32_C(0xc0180105): case UINT32_C(0xc0200104): case UINT32_C(0xc00c0109):
    case UINT32_C(0xc0080103):
    case UINT32_C(0xc0284106): case UINT32_C(0xc0084105): case UINT32_C(0xc0184102): case UINT32_C(0xc0104103):
        return 1;
    default:
        return (request & UINT32_C(0xc000ffff)) == UINT32_C(0xc0004114);
    }
}

/* nvmap handle state for the log (play-90 open-world stop: PARAM on a handle missing from the table): whether the
 * handle is in the table (any fd, retained objects included) with its references / allocation, else whether it was
 * ever issued, then its operation trail (host_nvmap.h: first operation and last four, each with steps, guest thread,
 * fd, lr, the caller from the guest frame, result, refs after it and the op's value). w->lock held (shared). */
static inline void sdk_nvdrv_log_event(FILE *f, const BootNvMapEvent *e) {
    fprintf(f, " [%s steps %llu thread %u fd %u lr %llx caller %llx ->", boot_nvmap_op_name(e->op), (unsigned long long)e->steps,
            e->thread, e->fd, (unsigned long long)e->lr, (unsigned long long)e->caller);
    if (e->result == UINT32_MAX) fprintf(f, " REJECTED"); else if (e->result) fprintf(f, " NvError %x", e->result); else fprintf(f, " ok");
    fprintf(f, " refs %x value %x]", e->refs, e->value);
}
static inline void sdk_nvdrv_log_handle(FILE *f, BootWindow *w, uint32_t handle) {
    const BootNvMapObject *o = boot_nvmap_find_any(&w->nvmap, handle);
    if (o) fprintf(f, " | nvmap h %x IN TABLE: owner fd %u refs %u%s %s size %x addr %llx", handle, o->owner_fd, boot_nvmap_references(o),
                   boot_nvmap_retained(o) ? " RETAINED (handle freed, object kept for GPU mappings / display)" : "",
                   o->allocated ? "allocated" : "unallocated", o->size, (unsigned long long)o->address);
    else fprintf(f, " | nvmap h %x NOT IN TABLE (last issued %x: %s)", handle, w->nvmap.next_handle,
                 !handle || (handle & 3) || handle > w->nvmap.next_handle ? "never issued" : "issued earlier, freed or fd closed");
    const BootNvMapTrail *t = boot_nvmap_trail(handle);
    if (!t) { fprintf(f, "; no trail (slot now h %x)", boot_nvmap_trails()[(handle >> 2) & (BOOT_NVMAP_TRAIL_SLOTS - 1)].handle); return; }
    fprintf(f, "; %u ops:", t->count);
    sdk_nvdrv_log_event(f, &t->first);
    uint32_t rest = t->count - 1, n = rest < 4 ? rest : 4;
    if (rest > n) fprintf(f, " ...");
    for (uint32_t k = 0; k < n; ++k) sdk_nvdrv_log_event(f, &t->last[(rest - n + k) & 3]);
}
/* The handle an nvmap request names (0: not an nvmap handle request). */
static inline uint32_t sdk_nvdrv_request_handle(uint32_t request, const uint8_t *in, uint32_t length) {
    switch (request) {
    case UINT32_C(0xc00c0109): case UINT32_C(0xc0180105): case UINT32_C(0xc0200104): case UINT32_C(0xc0080103):
        return length >= 4 ? (uint32_t)little_read(in, 4) : 0;
    case UINT32_C(0xc008010e): return length >= 8 ? (uint32_t)little_read(in + 4, 4) : 0;
    default: return 0;
    }
}

/* Diagnostics (play-75 thread-25 stop): every request the shared mapping handler rejects is logged before the IPC
 * fallback (which rejects it again and stops with BOOT_SVC address 0 width 0). For MAP_BUFFER_EX also the nvmap object
 * and the regions covering its CPU range (the handler accepts exactly one READ|WRITE type-5 region); for MAP_BUFFER_EX
 * and the nvmap requests the handle's state and trail (sdk_nvdrv_log_handle). `answer`: NULL for a rejection, else
 * the NvError the driver returned to the guest (exact nvmap, host_nvmap.h). nvdrv-reject.log. */
static inline void sdk_nvdrv_log_reject(BootContext *c, uint32_t fd, uint32_t request, const uint8_t *in, uint32_t length,
                                        uint64_t fa, uint64_t fb, const uint32_t *answer) {
    FILE *f = fopen("nvdrv-reject.log", "a"); if (!f) return;
    unsigned t = c->current_thread;
    if (answer) fprintf(f, *answer ? "NvError %x returned to the guest: " : "driver no-op (NvSuccess %x) returned to the guest: ", *answer);
    fprintf(f, "steps %llu thread %u id %llu lr %llx caller %llx fd %u ioctl %08x fa %llx fb %llx in",
            (unsigned long long)c->steps, t, (unsigned long long)(t < c->thread_count ? c->threads[t].id : 0),
            (unsigned long long)c->x[30], (unsigned long long)boot_guest_frame_return(c), fd, request, (unsigned long long)fa, (unsigned long long)fb);
    for (uint32_t i = 0; i < length && i < 40; ++i) fprintf(f, "%s%02x", (i & 7) ? "" : " ", in[i]);
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native;
    uint32_t nvmap_handle = sdk_nvdrv_request_handle(request, in, length);
    if (w && nvmap_handle) { AcquireSRWLockShared(&w->lock); sdk_nvdrv_log_handle(f, w, nvmap_handle); ReleaseSRWLockShared(&w->lock); }
    if (w && request == UINT32_C(0xc0284106) && length >= 32) {
        uint32_t handle = (uint32_t)little_read(in + 8, 4); uint64_t offset = little_read(in + 16, 8), size = little_read(in + 24, 8);
        AcquireSRWLockShared(&w->lock);
        BootNvMapObject *o = boot_nvmap_find_handle(&w->nvmap, handle);
        uint64_t cpu = o ? o->address + offset : 0, len = o ? (size ? size : o->size - offset) : 0;
        fprintf(f, " | nvmap h %x %s addr %llx size %llx; cpu %llx+%llx; regions:", handle, o ? "found" : "MISSING",
                (unsigned long long)(o ? o->address : 0), (unsigned long long)(o ? o->size : 0), (unsigned long long)cpu, (unsigned long long)len);
        unsigned n = 0;
        for (unsigned i = 0; o && i < c->region_count && n < 8; ++i) { Region *r = &c->regions[i];
            if (cpu < r->base + r->size && r->base < cpu + len) { ++n;
                fprintf(f, " [%llx+%llx res %u type %u perm %u]", (unsigned long long)r->base, (unsigned long long)r->size,
                        (unsigned)r->reserved, (unsigned)c->memory_types[i], (unsigned)r->permissions); } }
        unsigned mappings = 0; for (unsigned j = 0; j < BOOT_GPU_MAPPING_CAPACITY; ++j) mappings += w->gpu_mappings.entries[j].size != 0;
        fprintf(f, " (%u regions) gpu mappings %u/%u", n, mappings, (unsigned)BOOT_GPU_MAPPING_CAPACITY);
        for (unsigned d = 0; d < 16; ++d) if (w->nv_device_fds[d] == fd && w->nv_address_spaces[d].initialized) {
            unsigned used = 0; for (unsigned j = 0; j < BOOT_GPU_VA_CAPACITY; ++j) used += w->nv_address_spaces[d].reservations[j].size != 0;
            fprintf(f, ", va reservations %u/%u", used, (unsigned)BOOT_GPU_VA_CAPACITY); }
        sdk_nvdrv_log_handle(f, w, handle);
        ReleaseSRWLockShared(&w->lock);
    }
    fputc('\n', f); fclose(f);
}

static inline int sdk_nvdrv_ioctl(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    if (!sdk_config()->nvdrv) return BOOT_HOST_DECLINE;
    uint32_t fd = (uint32_t)c->x[0], request = (uint32_t)c->x[1];
    uint64_t buf = c->x[2];
    if (!sdk_nvdrv_is_mapping(request) || !buf) return BOOT_HOST_DECLINE;
    uint32_t length = (request >> 16) & 0x3fff;
    if (!length || length > 10240) return BOOT_HOST_DECLINE;
    static _Thread_local uint8_t in[10240], out[10240];
    /* Both directions are checked before anything is committed (a bad buffer stops here, restartable). */
    if (!boot_host_read(c, self, buf, in, length) || !boot_host_write(c, self, buf, in, length)) return BOOT_HOST_STOP;
    memcpy(out, in, length);
    uint64_t fa = 0, fb = 0;
    int rc = boot_nvdrv_mapping_ioctl(c, self, fd, request, in, out, &fa, &fb);
    if (rc < 0) return BOOT_HOST_STOP;                         /* guest access fault inside (c->status set) */
    if (!rc) {                                                 /* rejected before any change: the IPC path decides */
        if (!c->status) sdk_nvdrv_log_reject(c, fd, request, in, length, fa, fb, NULL);
        return c->status ? BOOT_HOST_STOP : BOOT_HOST_DECLINE;
    }
    if (rc == 2) {                                             /* the driver's own answer (exact nvmap): NvError, buffer untouched */
        static volatile long logged; uint32_t answer = (uint32_t)fa;
        long n = InterlockedIncrement(&logged);                /* the first 256 are logged with their handle trail */
        if (n <= 256) sdk_nvdrv_log_reject(c, fd, request, in, length, 0, 0, &answer);
        c->x[0] = answer;
        return BOOT_HOST_DONE;
    }
    if (!boot_host_write(c, self, buf, out, length)) return BOOT_HOST_STOP;
    c->x[0] = 0;                                               /* NvSuccess */
    return BOOT_HOST_DONE;
}

static const SdkEntry sdk_nvdrv_entries[] = {
    {0x3f91a0, "sdk.nvdrv.NvOsDrvIoctl", sdk_nvdrv_ioctl, 0, SDK_NVDRV_STEPS},
};
static inline unsigned sdk_nvdrv_register(void) {
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof sdk_nvdrv_entries / sizeof sdk_nvdrv_entries[0]; ++i)
        if (!boot_host_fn_hook(3, sdk_nvdrv_entries[i].sdk_offset, sdk_nvdrv_entries[i].name, sdk_nvdrv_entries[i].fn,
                               (void *)sdk_nvdrv_entries[i].user, BOOT_HOST_FN_KERNEL, sdk_nvdrv_entries[i].steps)) ++failed;
    return failed;
}
#endif
