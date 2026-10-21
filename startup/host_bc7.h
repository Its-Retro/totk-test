#ifndef TOTK_HOST_BC7_H
#define TOTK_HOST_BC7_H
/* bc7-work (stage 1): ASTC textures held as BC7 instead of RGBA8 (TOTK_BC7=1; default off for A/B).
 *
 * Why: no PC GPU samples ASTC, so host_astc.h decodes it to RGBA8 (4 bytes per texel; ASTC is 0.25-1). TOTK's
 * textures reached 7.5 GB that way, past the 4070 Laptop's ~7.2 GB VRAM budget. BC7 is 1 byte per texel: 4x less
 * for every decoded ASTC texture.
 *
 * Flow (included by host_gpu_render.h; every hook is a no-op while TOTK_BC7 is unset):
 *   build (boot_render_texture_resolve, guest ASTC path):
 *     boot_bc7_try_cached   before the resource is made: exclusion checks (BootBc7Why), the content key (SHA-256 of
 *                           the guest ASTC bytes + descriptor, host_bc7_cache.h) and the disk cache. A hit uploads the
 *                           cached BC7 blocks straight into a BC7 texture: no ASTC decode, no RGBA8 at all.
 *     boot_bc7_built        after a miss built the RGBA8 texture as before (drawn from at once: the first use never
 *                           waits for an encode): queues an encode job.
 *     boot_bc7_view_format  the dedup path shares a slot's resource: its view takes the resource's format.
 *   pump points (boot_bc7_pump): the start of every native submission and the end of every one (after its kick,
 *   host_nvn_play.h), and the idle points (end of boot_render_tex_trim). play-102 showed the idle points alone are far
 *   too rare in play (29 in 3000 presents): v1 pumped only there, so the one active job never finished.
 *     finish   an encode whose list completed: its blocks go to the disk cache, then every slot holding its RGBA8
 *              texture is switched to the BC7 one, slot by slot, when that is safe without a wait:
 *                in place  the slot's last binding lists (bound and recorded) completed and the open list does not
 *                          use it (the rule TOTK_TEX_LIVE_TRIM evicts by, boot_render_tex_trim_live);
 *                          The dedup map then points at the switched slot: the TIC ring's next index for the same
 *                          image (a new index per frame) shares the BC7 texture, so a hot image converges to BC7
 *                          while its busy indices age out; at an idle point every slot is quiet.
 *                staged    (TOTK_BC7_STAGED=1 only) a busy slot through the renderer's slot versioning: the new
 *                          descriptors are staged until the fence of the slot's last use and copied in place at the
 *                          next kick (boot_render_sv_apply), the job holding the RGBA8 texture until that fence. Off
 *                          by default: that kick waits when it comes before the guarded lists completed.
 *              A busy slot is tried again at the next pump point (BOOT_BC7_SWITCH_PUMPS at most).
 *     encode   at the end of a submission and at idle points: new encode work for at most TOTK_BC7_BUDGET_MS of GPU
 *              time per frame (estimated from measured blocks per GPU ms) in its own list on the render queue (after
 *              the kicked work in queue order): a band of RGBA8 rows copied into the encoder input, encoded
 *              (host_bc7_encode.h); a complete texture's blocks copied into the BC7 texture (and a readback buffer
 *              for the cache). Large textures span several pumps.
 *   present (host_nvn_play.h): boot_bc7_present resets the frame budget and logs every 300 presents.
 *   draw texture loop (host_gpu_render_draw.h): boot_bc7_bind before each binding is resolved, with its stage.
 *
 * Vertex-stage textures stay exact: BC7 is lossy, and vertex-fetched height / offset data must not change. A texture
 * a vertex shader binds (the renderer's only vertex-processing stage that samples: there is no game geometry or
 * tessellation stage; the point-sprite geometry shader samples nothing) is never transcoded: the build made for a
 * vertex binding is excluded (boot_bc7_bind_stage), its guest address joins a sticky set (boot_bc7_vtx_*) that
 * excludes every later build of that image under any TIC index, a finished encode of it is not switched, the dedup
 * path does not hand a BC7 image to a vertex binding (boot_bc7_share_ok), and a slot already switched (or built from
 * the cache) that a vertex shader binds later is rebuilt from guest memory as RGBA8 (poisoned before the binding
 * resolves; counted as vertex reverts).
 *
 * Correctness of the switch: a job holds a reference to the RGBA8 texture from its start, which makes every in-place
 * re-upload path refuse it (they all require boot_render_res_refs == 1) and keeps the pool from reusing it, so the
 * encoded bytes are exactly the texture's. A queued job is checked against the slot's build serial
 * (boot_render_tex_serial, bumped by boot_render_tex_mark on every build path) before it starts. Slots rebuilt,
 * poisoned or evicted meanwhile are simply not switched; the blocks still go to the cache under their content key.
 * Guest writes change the ASTC bytes, so the rebuilt texture looks up another key: a stale BC7 is never used.
 *
 * Plug-in points for the next stages:
 *   - producers (BootBc7Producer in host_bc7_cache.h, boot_bc7_producer_fill below): how a job's RGBA8 texels reach
 *     the encoder input buffer. Stage 1 copies the CPU-decoded RGBA8 texture. Stage 2 (GPU ASTC decode) adds a fill
 *     step that decodes raw ASTC into the same buffer (contract below), and needs a drawable texture at first use
 *     (keep the RGBA8 path until its BC7 lands, or encode inline: boot_bc7_record_band can record into r->cl).
 *     Stage 3 (direct ASTC 4x4 -> BC7 block transcoding) adds a `direct` step that writes BC7 blocks into the job's
 *     output buffer itself, falling back to fill + encode per band (or per block with a mask) where it cannot.
 *     boot_bc7_choose_producer picks one per texture; the job, the logs and the cache header record it.
 *   - exclusions (BootBc7Why): every reason a texture stays RGBA8 is one enum value with a counter, checked in
 *     boot_bc7_check; stage 4 turns individual reasons into supported paths (re-encode on invalidation for
 *     reinterpreted / partially updated textures, HDR -> BC6H).
 * Later stages, not built here: GPU ASTC decode (stage 2) and direct ASTC 4x4 -> BC7 block transcoding without a full
 * decode (stage 3) slot in at boot_bc7_choose_producer / boot_bc7_producer_fill; see the notes there.
 *
 * Environment: TOTK_BC7=1 on (default off). TOTK_BC7_CACHE=0 no disk cache. TOTK_BC7_BUDGET_MS=x GPU encode budget
 * per frame (default 2). TOTK_BC7_STAGED=1: busy slots also switch through staged descriptor writes (default off). TOTK_BC7_QUALITY=0 quick (modes 4/5/6), 1 default (DirectXTex modes 4/5/6 + 1/3/7, the
 * 2-subset passes skipping blocks already within error 15), 2 DirectXTex default exactly, 3 also modes 0/2.
 * TOTK_BC7_CACHE_MB=n cache directory budget (default 4096). TOTK_BC7_CACHE_DIR=path (default: bc7-cache next to the
 * shader manifest, i.e. startup\bc7-cache). */
#define BOOT_BC7_DISPATCH(cl, groups) BOOT_GP_DISPATCH("bc7 encode", 0, (cl), (groups), 1, 1)
#include "host_bc7_encode.h"
#include "host_bc7_cache.h"

#define BOOT_BC7_QUEUE 1024          /* queued encode jobs */
#define BOOT_BC7_FINISHING 128       /* encoded jobs: waiting for their list, or switching their slots */
#define BOOT_BC7_ALLOCS 8
#define BOOT_BC7_HOLDERS 32          /* slots switched per job (dedup shares one texture between TIC indices) */
#define BOOT_BC7_SWITCH_PUMPS 3600   /* a job whose slots stay busy this many pump points is let go (counted: busy) */
#define BOOT_BC7_VTX 16384u          /* guest addresses of images a vertex stage sampled (open addressing) */
enum { BOOT_BC7_AT_IDLE = 0, BOOT_BC7_AT_SUBMIT_BEGIN = 1, BOOT_BC7_AT_SUBMIT_END = 2, BOOT_BC7_AT_N };
enum { BOOT_BC7_J_ENCODED = 1, BOOT_BC7_J_SWITCHING = 2, BOOT_BC7_J_DROPPED = 3 };
#define BOOT_BC7_IN_BYTES (UINT64_C(8) << 20)                  /* encoder input scratch (RGBA8 rows of one band) */
#define BOOT_BC7_MIN_BYTES (UINT64_C(64) << 10)                /* smaller RGBA8 images stay RGBA8 */
#define BOOT_BC7_CACHE_MAX_PAYLOAD (UINT64_C(48) << 20)        /* larger encodes are not written to the cache */
#define BOOT_BC7_DYNAMIC_PRESENTS 120                          /* same address rebuilt with other bytes within this: dynamic */

/* Why a texture stays RGBA8 (counted per build; stage 4 can turn reasons into supported cases). */
enum {
    BOOT_BC7_X_NONE = 0,
    BOOT_BC7_X_DIM_1D,          /* 1D / 1D array: no block-compressed 1D textures in D3D12 */
    BOOT_BC7_X_DIM_3D,          /* 3D: not handled (slices of 4x4 blocks need their own path) */
    BOOT_BC7_X_FORMAT,          /* the host format is not R8G8B8A8_UNORM / _SRGB */
    BOOT_BC7_X_UNALIGNED,       /* level 0 not a multiple of 4 and no D3D12 UnalignedBlockTexturesSupported */
    BOOT_BC7_X_SMALL,           /* under BOOT_BC7_MIN_BYTES of RGBA8 */
    BOOT_BC7_X_CPU_WRITTEN,     /* texture watch: CPU texel writes / partial updates (rewritten since its build) */
    BOOT_BC7_X_GPU_WRITTEN,     /* the GPU writes this memory (pending GPU writes or GPU-owned granules): rendered to */
    BOOT_BC7_X_RENDER_TARGET,   /* a render target lies over the texture memory (aliased / rendered to) */
    BOOT_BC7_X_REINTERPRETED,   /* another texture view of other format (or unknown origin) overlaps the memory */
    BOOT_BC7_X_DYNAMIC,         /* the same address was built with other bytes within BOOT_BC7_DYNAMIC_PRESENTS presents */
    BOOT_BC7_X_HDR,             /* HDR ASTC (BC6H: stage 4). Today an HDR block already fails the RGBA8 build itself
                                 * (boot_astc_decode_pitched < 0), so this stays 0 until a decode path keeps them */
    BOOT_BC7_X_QUEUE_FULL,      /* BOOT_BC7_QUEUE jobs already waiting */
    BOOT_BC7_X_STALE,           /* the slot changed before its queued encode started (rebuilt, poisoned, evicted) */
    BOOT_BC7_X_NO_MEMORY,       /* a BC7 texture or buffer could not be created */
    BOOT_BC7_X_DEVICE,          /* encoder objects unavailable (device lost, PSO failure) */
    BOOT_BC7_X_VERTEX,          /* sampled by a vertex stage (now or before, any TIC index): lossy BC7 would move vertex-fetched data */
    BOOT_BC7_X_BUSY,            /* encoded, but its slots stayed in use for BOOT_BC7_SWITCH_PUMPS pump points: not switched */
    BOOT_BC7_X_COUNT
};
static const char *const boot_bc7_x_name[BOOT_BC7_X_COUNT] = {"none", "1d", "3d", "format", "unaligned", "small", "cpu-written", "gpu-written",
    "render-target", "reinterpreted", "dynamic", "hdr", "queue-full", "stale", "no-memory", "device", "vertex-stage", "busy"};

static struct {
    int init, on, cache_on, quality, unaligned, staged; double budget_ms; unsigned flags; uint32_t refine; char dir[MAX_PATH];
} boot_bc7_cfg;
static inline int boot_bc7_env_int(const char *name, int def) {
    char v[32]; DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n && n < sizeof v ? atoi(v) : def;
}
static inline int boot_bc7_on(void) {
    if (boot_bc7_cfg.init) return boot_bc7_cfg.on;
    boot_bc7_cfg.init = 1;
    boot_bc7_cfg.on = boot_bc7_env_int("TOTK_BC7", 0) == 1;
    boot_bc7_cfg.cache_on = boot_bc7_env_int("TOTK_BC7_CACHE", 1) != 0;
    boot_bc7_cfg.staged = boot_bc7_env_int("TOTK_BC7_STAGED", 0) == 1;
    boot_bc7_cfg.budget_ms = 2.0;
    { char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_BC7_BUDGET_MS", v, sizeof v); if (n && n < sizeof v) { double b = atof(v); if (b >= 0.05 && b <= 100) boot_bc7_cfg.budget_ms = b; } }
    int q = boot_bc7_env_int("TOTK_BC7_QUALITY", 1); if (q < 0 || q > 3) q = 1;
    static const unsigned qf[4] = {BOOT_BC7_FLAG_QUICK, 0, 0, BOOT_BC7_FLAG_MODE02};
    static const uint32_t qr[4] = {0, 15, 0, 0};
    boot_bc7_cfg.quality = q; boot_bc7_cfg.flags = qf[q]; boot_bc7_cfg.refine = qr[q];
    return boot_bc7_cfg.on;
}

typedef struct BootBc7Job {
    uint32_t index, serial;                 /* slot that built the RGBA8 texture, and that build's serial */
    uint32_t producer, dim, width, height, layers, levels, subs;
    DXGI_FORMAT rgba_fmt, bc7_fmt;
    ID3D12Resource *rgba;                   /* weak while queued; referenced from the start to the finish */
    ID3D12Resource *bc7, *out, *rb;         /* BC7 texture, encoder output (footprint layout), cache readback */
    int ref;                                /* holds a reference to rgba (from boot_bc7_start on) */
    int bc7_pooled, touched;                /* bc7 from the texture pool (shader-resource state); used in the open list */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;     /* the slot's view (the format is switched at the finish) */
    BootBc7Key key;
    uint32_t sub, row;                      /* progress: next subresource, next block row in it */
    uint64_t total, rgba_bytes, bc7_bytes, fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp; /* the BC7 texture's footprints (subs) */
    uint64_t va;                            /* guest address of the image (the vertex-stage set is keyed by it) */
    unsigned state, n_holders, rescanned, pumps, switched, abandon;   /* finishing: BOOT_BC7_J_*, switch progress */
    uint32_t holders[BOOT_BC7_HOLDERS];     /* slots still holding the RGBA8 texture */
    uint64_t hold_fence;                    /* staged switches: lists up to this fence may read an RGBA8 descriptor */
} BootBc7Job;
static struct {
    ID3D12Device *dev; int failed;
    BootBc7Encoder enc;
    ID3D12GraphicsCommandList *cl; ID3D12CommandAllocator *alloc[BOOT_BC7_ALLOCS]; uint64_t alloc_fence[BOOT_BC7_ALLOCS]; unsigned next_alloc;
    ID3D12QueryHeap *qheap; ID3D12Resource *qread; uint64_t ts_freq, ts_blocks[BOOT_BC7_ALLOCS]; int ts_pending[BOOT_BC7_ALLOCS];
    ID3D12Resource *in, *modes;             /* scratch, COMMON between lists */
    BootBc7Job queue[BOOT_BC7_QUEUE]; unsigned q_head, q_n;
    BootBc7Job active; int has_active;
    BootBc7Job finishing[BOOT_BC7_FINISHING]; unsigned n_finishing;
    double rate;                            /* blocks per GPU ms (measured, smoothed) */
    double frame_ms; uint64_t frame_qpc;    /* estimated encode ms spent in the current frame, its start */
    uint64_t presents, transcoded, swaps, saved, hit_saved, hits, misses, cache_off, enqueued, blocks, lists, gpu_us, cache_queued, cache_skipped;
    uint64_t hit_us, hash_us, x[BOOT_BC7_X_COUNT];
    uint64_t pumps[BOOT_BC7_AT_N], switch_inplace, switch_staged, switch_busy, vtx_reverts;
    uint64_t last_transcoded, last_gpu_us, last_saved, last_hits, last_kick_waits;
} boot_bc7;
/* Vertex stage (see the file comment): the stage of the binding the draw texture loop resolves (0 vertex, 1 pixel; -1
 * outside it), and the guest addresses of images a vertex stage sampled. Full table: every image counts as sampled. */
static int boot_bc7_bind_stage = -1;
static uint64_t boot_bc7_vtx_va[BOOT_BC7_VTX]; static unsigned boot_bc7_vtx_n; static int boot_bc7_vtx_full;
static inline unsigned boot_bc7_vtx_h(uint64_t va) { return (unsigned)((va * UINT64_C(0x9e3779b97f4a7c15)) >> 50) & (BOOT_BC7_VTX - 1); }
static inline int boot_bc7_vtx_has(uint64_t va) {
    if (boot_bc7_vtx_full) return 1;
    for (unsigned k = boot_bc7_vtx_h(va), n = 0; n < BOOT_BC7_VTX; k = (k + 1) & (BOOT_BC7_VTX - 1), ++n) {
        if (!boot_bc7_vtx_va[k]) return 0;
        if (boot_bc7_vtx_va[k] == va) return 1;
    }
    return 0;
}
static inline void boot_bc7_vtx_add(uint64_t va) {
    if (!va || boot_bc7_vtx_full) return;
    for (unsigned k = boot_bc7_vtx_h(va), n = 0; n < BOOT_BC7_VTX; k = (k + 1) & (BOOT_BC7_VTX - 1), ++n) {
        if (boot_bc7_vtx_va[k] == va) return;
        if (!boot_bc7_vtx_va[k]) {
            if (boot_bc7_vtx_n + 1 >= BOOT_BC7_VTX / 4 * 3) { boot_bc7_vtx_full = 1; return; }
            boot_bc7_vtx_va[k] = va; ++boot_bc7_vtx_n; return;
        }
    }
}
/* Draw texture loop (host_gpu_render_draw.h), before binding `tic` of `stage` (0 vertex, 1 pixel) resolves. A vertex
 * binding of a valid slot whose entry is unchanged marks the image; one already held as BC7 is rebuilt as RGBA8. */
static inline void boot_bc7_bind(BootRender *r, uint64_t pool, unsigned stage, uint32_t tic) {
    boot_bc7_bind_stage = (int)stage;
    if (stage || !boot_bc7_cfg.on || !boot_render_tex_slots || tic >= BOOT_RENDER_TEX_SLOTS - 256u) return;
    BootRenderTexSlot *s = &boot_render_tex_slots[tic];
    if (!s->valid || !s->size || s->view_of) return;   /* built by this binding: boot_bc7_built marks it */
    const uint32_t *w = boot_render_desc_words(r, pool, tic, 0);
    if (!w || memcmp(s->words, w, 32)) return;          /* another entry now: rebuilt by this binding */
    boot_bc7_vtx_add(s->va);
    if (boot_render_tex_bc7[tic]) { boot_render_tex_poison(tic); ++boot_bc7.vtx_reverts; }
}
/* The dedup path may share slot `o`'s texture: not a BC7 one with a vertex binding. */
static inline int boot_bc7_share_ok(int o) { return !(boot_bc7_bind_stage == 0 && boot_render_tex_bc7[o]); }
/* Address table (address -> image tag, key, present) for the dynamic check. The per-slot TIC format of the
 * reinterpretation check is boot_render_tex_tic_fmt (host_gpu_render.h: 0 after every build, set here for guest builds). */
static struct { uint64_t va, tag, key, present; } boot_bc7_seen[4096];

static inline DXGI_FORMAT boot_bc7_format_for(DXGI_FORMAT rgba) {
    return rgba == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_BC7_UNORM : rgba == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_UNKNOWN;
}
/* The view format of a resource shared by the dedup path: a slot switched to BC7 is viewed as BC7. */
static inline DXGI_FORMAT boot_bc7_view_format(ID3D12Resource *res, DXGI_FORMAT fmt) {
    if (!boot_bc7_cfg.on || !res) return fmt;
    D3D12_RESOURCE_DESC d; res->lpVtbl->GetDesc(res, &d);
    return d.Format == DXGI_FORMAT_BC7_UNORM || d.Format == DXGI_FORMAT_BC7_UNORM_SRGB ? d.Format : fmt;
}
static inline uint64_t boot_bc7_rgba_bytes(uint32_t w, uint32_t h, uint32_t layers, uint32_t levels) {
    uint64_t n = 0;
    for (uint32_t l = 0; l < levels; ++l) { uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1; n += (uint64_t)lw * lh * 4; }
    return n * layers;
}
static inline void boot_bc7_sha(const void *data, size_t n, uint8_t out[32]) { boot_render_sha256(data, n, out); }
static inline uint64_t boot_bc7_qpf(void) { static uint64_t f; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = (uint64_t)q.QuadPart; } return f; }

/* Producer choice per texture (stage plug-in point). Stage 1: always the CPU-decoded RGBA8 texture. Stage 3 picks
 * BOOT_BC7_PRODUCER_DIRECT / _DIRECT_MIXED for ASTC 4x4 here; stage 2 picks _GPU_DECODE_GPU_ENCODE. */
static inline uint32_t boot_bc7_choose_producer(const BootTic *t, const BootTexFormat *f) { (void)t; (void)f; return BOOT_BC7_PRODUCER_CPU_DECODE_GPU_ENCODE; }
/* Fill step of a producer: makes texel rows [y0, y1) of subresource `sub` (mip width mw) raw RGBA8 in `in` (rows `pitch`
 * bytes apart from offset 0). `in` arrives in COPY_DEST and must be left in UNORDERED_ACCESS with the writes visible
 * (a GPU decoder: transition, dispatch, UAV barrier). 0 = cannot (the job is dropped). */
static inline int boot_bc7_producer_fill(BootBc7Job *j, ID3D12GraphicsCommandList *cl, unsigned sub, uint32_t mw, uint32_t y0, uint32_t y1, ID3D12Resource *in, uint32_t pitch) {
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b); b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = in;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    switch (j->producer) {
        case BOOT_BC7_PRODUCER_CPU_DECODE_GPU_ENCODE:
            boot_bc7_copy_rows(cl, j->rgba, sub, j->rgba_fmt, mw, y0, y1, in, 0, pitch);
            BOOT_GP_RB("bc7 encode", cl, 1, &b);
            return 1;
        /* case BOOT_BC7_PRODUCER_GPU_DECODE_GPU_ENCODE: stage 2 (raw ASTC upload -> decode compute -> `in`) */
        default: return 0;
    }
}

/* ---- exclusions ---- */
static inline unsigned boot_bc7_check(BootRender *r, uint32_t index, const BootTic *t, unsigned dim, unsigned levels, unsigned layers,
                                      uint64_t va, uint64_t size, DXGI_FORMAT fmt) {
    if (dim == BOOT_DIM_1D || dim == BOOT_DIM_1D_ARRAY) return BOOT_BC7_X_DIM_1D;
    if (dim == BOOT_DIM_3D) return BOOT_BC7_X_DIM_3D;
    if (dim == BOOT_DIM_BUFFER || boot_bc7_format_for(fmt) == DXGI_FORMAT_UNKNOWN) return BOOT_BC7_X_FORMAT;
    if (boot_bc7_bind_stage == 0 || boot_bc7_vtx_has(va)) return BOOT_BC7_X_VERTEX;
    if (((t->width | t->height) & 3) && !boot_bc7_cfg.unaligned) return BOOT_BC7_X_UNALIGNED;
    if (boot_bc7_rgba_bytes(t->width, t->height, layers, levels) < BOOT_BC7_MIN_BYTES) return BOOT_BC7_X_SMALL;
    if (boot_render_tex_watch_want[index] || boot_render_tex_watch_of[index]) return BOOT_BC7_X_CPU_WRITTEN;
    if (boot_render_tex_watch_gpu(va, size)) return BOOT_BC7_X_GPU_WRITTEN;
    BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned k = 0; k < T->count; ++k) {
        const BootRenderTarget *x = &T->rts[k];
        if (!x->texture) continue;
        uint64_t ext = (uint64_t)x->width * x->height * boot_render_dxgi_bytes(x->dxgi); if (!ext) ext = 1;
        if (x->address < va + size && va < x->address + ext) return BOOT_BC7_X_RENDER_TARGET;
    }
    const uint8_t mine = (uint8_t)(0x80 | (t->components & 0x7f));
    for (uint32_t k = 0; k < boot_render_tex_live_n; ++k) {
        uint32_t i = boot_render_tex_live[k]; const BootRenderTexSlot *s = &boot_render_tex_slots[i];
        if (i == index || !s->valid || !s->size || s->view_of) continue;
        if (s->va < va + size && va < s->va + s->size && boot_render_tex_tic_fmt[i] != mine) return BOOT_BC7_X_REINTERPRETED;
    }
    return BOOT_BC7_X_NONE;
}

/* ---- build hooks ---- */
typedef struct BootBc7Pre { int state; BootBc7Key key; uint64_t rgba_bytes; uint32_t producer; } BootBc7Pre;   /* state: 1 queue an encode after the build, 2 built from the cache */
static inline void boot_bc7_log_on(BootRender *r) {
    static int done; if (done) return; done = 1;
    boot_gp_log("bc7: on (quality %d: flags %u refine %u; budget %.2f ms/frame; unaligned BC textures %s; cache %s%s%s, budget %u MiB)",
                boot_bc7_cfg.quality, boot_bc7_cfg.flags, boot_bc7_cfg.refine, boot_bc7_cfg.budget_ms, boot_bc7_cfg.unaligned ? "yes" : "no",
                boot_bc7_cfg.cache_on ? (boot_bc7_disk.ok ? "on at " : "UNAVAILABLE at ") : "off", boot_bc7_cfg.cache_on ? boot_bc7_cfg.dir : "", "",
                (unsigned)boot_bc7_env_int("TOTK_BC7_CACHE_MB", BOOT_BC7_CACHE_MB_DEFAULT));
    (void)r;
}
static inline void boot_bc7_setup(BootRender *r) {
    static ID3D12Device *seen;
    if (seen == r->gpu->device) return;
    seen = r->gpu->device;
    D3D12_FEATURE_DATA_D3D12_OPTIONS8 o8; memset(&o8, 0, sizeof o8);
    boot_bc7_cfg.unaligned = SUCCEEDED(ID3D12Device_CheckFeatureSupport(r->gpu->device, D3D12_FEATURE_D3D12_OPTIONS8, &o8, sizeof o8)) && o8.UnalignedBlockTexturesSupported;
    if (boot_bc7_cfg.cache_on && !boot_bc7_disk.started) {
        char v[MAX_PATH]; DWORD n = GetEnvironmentVariableA("TOTK_BC7_CACHE_DIR", v, sizeof v);
        if (n && n < sizeof v) snprintf(boot_bc7_cfg.dir, sizeof boot_bc7_cfg.dir, "%s", v);
        else {   /* next to the shader manifest, as the PSO cache (startup\bc7-cache) */
            char m[sizeof r->manifest_path + 32]; snprintf(m, sizeof m, "%s", r->manifest_path);
            if (GetFileAttributesA(m) == INVALID_FILE_ATTRIBUTES && !strcmp(m, "shader-manifest.tsv")) snprintf(m, sizeof m, "startup\\shader-manifest.tsv");
            char *sep = strrchr(m, '\\'), *sep2 = strrchr(m, '/'); if (sep2 > sep) sep = sep2;
            if (sep) sep[1] = 0; else m[0] = 0;
            snprintf(boot_bc7_cfg.dir, sizeof boot_bc7_cfg.dir, "%sbc7-cache", m);
        }
        int mb = boot_bc7_env_int("TOTK_BC7_CACHE_MB", BOOT_BC7_CACHE_MB_DEFAULT); if (mb < 64) mb = 64;
        boot_bc7_disk_open(boot_bc7_cfg.dir, (uint64_t)mb << 20);
    }
    boot_bc7_log_on(r);
}
/* Before the guest path makes the texture of slot `index` (after its dedup check). 1: built from the cache (tex and
 * fmt set, *previous consumed; the caller goes on to its view), 0: build as before, -1: failed after recording copies
 * (as an upload failure of the normal path). */
static inline int boot_bc7_try_cached(BootRender *r, uint32_t index, const BootTic *t, const BootTexFormat *f, unsigned dim, unsigned levels, unsigned layers,
                                      uint64_t stride, uint64_t va, const uint8_t *src, ID3D12Resource **previous, ID3D12Resource **tex, DXGI_FORMAT *fmt, BootBc7Pre *pre) {
    pre->state = 0;
    if (!boot_bc7_on() || f->decode != BOOT_TEX_DECODE_ASTC) return 0;
    boot_bc7_setup(r);
    const uint64_t size = stride * layers;
    unsigned why = boot_bc7_check(r, index, t, dim, levels, layers, va, size, *fmt);
    if (why) { ++boot_bc7.x[why]; return 0; }
    const DXGI_FORMAT bfmt = boot_bc7_format_for(*fmt);
    BootBc7Source s; memset(&s, 0, sizeof s);
    s.tic_format = t->components; s.srgb = t->srgb; s.dim = dim; s.width = t->width; s.height = t->height; s.depth = 1; s.layers = layers; s.levels = levels;
    s.block_w = f->block_w; s.block_h = f->block_h; s.log_gob_h = t->log_gob_h; s.log_gob_d = t->log_gob_d; s.pitch = t->pitch; s.out_format = (uint32_t)bfmt;
    s.layer_stride = stride; s.bytes = size;
    uint64_t h0 = boot_tex_now();
    boot_bc7_key(boot_bc7_sha, &s, src, (size_t)size, &pre->key);
    boot_bc7.hash_us += (boot_tex_now() - h0) * 1000000 / boot_bc7_qpf();
    pre->rgba_bytes = boot_bc7_rgba_bytes(t->width, t->height, layers, levels);
    pre->producer = boot_bc7_choose_producer(t, f);
    pre->state = 1;
    {   /* dynamic: the same image (address, size, format) built with other bytes a short while ago: not worth an
         * encode (a cache hit below is still used); streaming that reuses memory for another shape is not dynamic */
        uint64_t k64; memcpy(&k64, pre->key.b, 8);
        unsigned h = (unsigned)((va >> 9) ^ (va >> 21) ^ size) & 4095;
        uint64_t tag = size << 8 | (t->components & 0x7f);
        int dyn = boot_bc7_seen[h].va == va && boot_bc7_seen[h].tag == tag && boot_bc7_seen[h].key != k64 && boot_bc7.presents - boot_bc7_seen[h].present < BOOT_BC7_DYNAMIC_PRESENTS;
        boot_bc7_seen[h].va = va; boot_bc7_seen[h].tag = tag; boot_bc7_seen[h].key = k64; boot_bc7_seen[h].present = boot_bc7.presents;
        if (dyn) pre->state = -1;
    }
    if (!boot_bc7_cfg.cache_on || !boot_bc7_disk.ok) { ++boot_bc7.cache_off; }
    uint64_t r0 = boot_tex_now(), pay = 0; BootBc7FileHeader hdr; uint8_t *blocks = NULL;
    if (boot_bc7_cfg.cache_on && boot_bc7_disk.ok) {
        blocks = boot_bc7_disk_read(&pre->key, (uint32_t)bfmt, dim, t->width, t->height, layers, levels, &pay, &hdr);
        if (!blocks) ++boot_bc7.misses;
    }
    if (!blocks) {
        if (pre->state < 0) { ++boot_bc7.x[BOOT_BC7_X_DYNAMIC]; pre->state = 0; }
        return 0;
    }
    /* hit: footprints first (nothing is touched when they do not describe the payload), then a BC7 texture of the
     * RGBA8 path's shape (the previous one if it matches, the pool, or a new one) */
    D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = t->width; d.Height = t->height; d.DepthOrArraySize = (UINT16)layers; d.MipLevels = (UINT16)levels;
    d.Format = bfmt; d.SampleDesc.Count = 1;
    const unsigned subs = levels * layers;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fps = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT *)calloc(subs, sizeof *fps); UINT *rows = (UINT *)calloc(subs, sizeof *rows);
    UINT64 *rsz = (UINT64 *)calloc(subs, sizeof *rsz), total = 0;
    int ok = fps && rows && rsz;
    if (ok) {
        ID3D12Device_GetCopyableFootprints(r->gpu->device, &d, 0, subs, 0, fps, rows, rsz, &total);
        for (unsigned sub = 0; ok && sub < subs; ++sub) {
            uint32_t rb = 0, rn = 0; uint64_t at = boot_bc7_tight_offset(t->width, t->height, levels, sub, &rb, &rn);
            ok = rows[sub] == rn && rsz[sub] == rb && at + (uint64_t)rb * rn <= pay;
        }
    }
    ID3D12Resource *x = NULL; int to_copy_dest = 0;
    if (ok && *previous) {
        D3D12_RESOURCE_DESC pd; (*previous)->lpVtbl->GetDesc(*previous, &pd);
        if (pd.Dimension == d.Dimension && pd.Width == d.Width && pd.Height == d.Height && pd.DepthOrArraySize == d.DepthOrArraySize && pd.MipLevels == d.MipLevels &&
            pd.Format == d.Format && pd.SampleDesc.Count == 1 && pd.Flags == d.Flags && boot_render_res_refs(*previous) == 1) { x = *previous; to_copy_dest = 1; }
    }
    if (ok && !x && (x = boot_render_tex_pool_take(&d)) != NULL) { to_copy_dest = 1; ++boot_render_tex_pooled; }
    if (ok && !x) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&x))) x = NULL;
        if (!x) ++boot_bc7.x[BOOT_BC7_X_NO_MEMORY];
    }
    if (ok && to_copy_dest && !boot_render_begin(r)) { if (x != *previous) boot_render_tex_pool_put(x); x = NULL; }
    if (!x) { free(fps); free(rows); free(rsz); free(blocks); pre->state = 0; return 0; }   /* the RGBA8 path as before */
    if (x == *previous) *previous = NULL;
    else if (*previous) { boot_render_tex_pool_put(*previous); *previous = NULL; }
    if (to_copy_dest) {
        D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b); b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = x;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("bc7 cache upload", r->cl, 1, &b);
    }
    /* upload in chunks of whole subresources (as the RGBA8 path), tight rows re-pitched to the footprints */
    for (unsigned first = 0; ok && first < subs; ) {
        unsigned end = first + 1;
        while (end < subs && fps[end].Offset - fps[first].Offset + (end + 1 < subs ? fps[end + 1].Offset - fps[end].Offset : total - fps[end].Offset) <= BOOT_RENDER_UPLOAD_CHUNK) ++end;
        UINT64 base = fps[first].Offset, bytes = (end < subs ? fps[end].Offset : total) - base;
        if (!boot_render_begin(r)) { ok = 0; break; }
        ID3D12Resource *upload = NULL; uint64_t ring_at = 0; uint8_t *up = boot_render_upload_alloc(r, bytes, &upload, &ring_at);
        if (!up) { ok = 0; break; }
        for (unsigned sub = first; sub < end; ++sub) {
            uint32_t rb = 0, rn = 0; const uint8_t *from = blocks + boot_bc7_tight_offset(t->width, t->height, levels, sub, &rb, &rn);
            for (uint32_t y = 0; y < rn; ++y) memcpy(up + (fps[sub].Offset - base) + (size_t)y * fps[sub].Footprint.RowPitch, from + (size_t)y * rb, rb);
        }
        if (!boot_render_begin(r)) { ok = 0; break; }
        for (unsigned sub = first; sub < end; ++sub) {
            D3D12_TEXTURE_COPY_LOCATION dl = {x, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}; dl.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION sl = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
            sl.PlacedFootprint = fps[sub]; sl.PlacedFootprint.Offset = fps[sub].Offset - base + ring_at;
            BOOT_GP_CTR("bc7 cache upload", r->cl, &dl, 0, 0, 0, &sl, NULL);
        }
        first = end;
    }
    if (ok) {
        D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b); b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = x;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        BOOT_GP_RB("bc7 cache upload", r->cl, 1, &b);
    }
    free(fps); free(rows); free(rsz); free(blocks);
    if (!ok) {   /* as the RGBA8 path's upload failure: copies of earlier chunks may be recorded */
        if (r->recording) boot_render_submit(r);
        ID3D12Resource_Release(x);
        snprintf(r->error, sizeof r->error, "bc7 cache upload: TIC %u %ux%u layers %u levels %u", index, t->width, t->height, layers, levels);
        return -1;
    }
    ++boot_bc7.hits; boot_bc7.hit_saved += pre->rgba_bytes - pay; boot_bc7.hit_us += (boot_tex_now() - r0) * 1000000 / boot_bc7_qpf();
    ++boot_tex_built;
    *tex = x; *fmt = bfmt; pre->state = 2;
    return 1;
}
/* After every guest build of slot `index` (view `sv` written; `components`: its TIC format): records the format for the
 * reinterpretation check and queues the encode of a cache miss. */
static inline void boot_bc7_built(BootRender *r, uint32_t index, uint32_t components, const BootBc7Pre *pre, const D3D12_SHADER_RESOURCE_VIEW_DESC *sv) {
    (void)r;
    if (!boot_bc7_cfg.on) return;
    boot_render_tex_tic_fmt[index] = (uint8_t)(0x80 | (components & 0x7f));
    const BootRenderTexSlot *s = &boot_render_tex_slots[index];
    if (s->valid && s->res) {   /* every guest build: whether it holds BC7 (cache hit, dedup share), and a vertex binding's image */
        D3D12_RESOURCE_DESC rd; s->res->lpVtbl->GetDesc(s->res, &rd);
        boot_render_tex_bc7[index] = (uint8_t)(rd.Format == DXGI_FORMAT_BC7_UNORM || rd.Format == DXGI_FORMAT_BC7_UNORM_SRGB);
        if (boot_bc7_bind_stage == 0 && s->size) boot_bc7_vtx_add(s->va);
    }
    if (pre->state != 1) return;
    if (!s->valid || !s->res || s->view_of) return;
    if (boot_render_tex_watch_of[index]) { ++boot_bc7.x[BOOT_BC7_X_CPU_WRITTEN]; return; }   /* watched from this build on */
    D3D12_RESOURCE_DESC d; s->res->lpVtbl->GetDesc(s->res, &d);
    if (boot_bc7_format_for(d.Format) == DXGI_FORMAT_UNKNOWN || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { ++boot_bc7.x[BOOT_BC7_X_FORMAT]; return; }
    if (boot_bc7.q_n == BOOT_BC7_QUEUE) { ++boot_bc7.x[BOOT_BC7_X_QUEUE_FULL]; return; }
    BootBc7Job *j = &boot_bc7.queue[(boot_bc7.q_head + boot_bc7.q_n++) % BOOT_BC7_QUEUE]; memset(j, 0, sizeof *j);
    j->index = index; j->serial = boot_render_tex_serial[index]; j->producer = pre->producer; j->dim = s->dim;
    j->width = (uint32_t)d.Width; j->height = d.Height; j->layers = d.DepthOrArraySize; j->levels = d.MipLevels; j->subs = j->layers * j->levels;
    j->rgba_fmt = d.Format; j->bc7_fmt = boot_bc7_format_for(d.Format); j->rgba = s->res; j->sv = *sv; j->key = pre->key;
    j->rgba_bytes = pre->rgba_bytes; j->va = s->va;
    ++boot_bc7.enqueued;
}

/* ---- encoder objects (own list on the render queue, as host_gpu_render.h boot_render_gt) ---- */
static inline HRESULT boot_bc7_psoc(void *ctx, ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *cd, ID3D12PipelineState **out) {
    return boot_psoc_create_compute((const BootRender *)ctx, dev, cd, out);
}
static inline void boot_bc7_job_release(BootBc7Job *j) {
    if (j->rgba && j->ref) ID3D12Resource_Release(j->rgba);
    if (j->bc7) ID3D12Resource_Release(j->bc7);
    if (j->out) ID3D12Resource_Release(j->out);
    if (j->rb) ID3D12Resource_Release(j->rb);
    free(j->fp);
    memset(j, 0, sizeof *j);
}
static inline void boot_bc7_objects_release(void) {
    if (boot_bc7.has_active) { boot_bc7_job_release(&boot_bc7.active); boot_bc7.has_active = 0; }
    for (unsigned i = 0; i < boot_bc7.n_finishing; ++i) boot_bc7_job_release(&boot_bc7.finishing[i]);
    boot_bc7.n_finishing = 0; boot_bc7.q_n = 0;
    if (boot_bc7.cl) ID3D12GraphicsCommandList_Release(boot_bc7.cl);
    for (unsigned i = 0; i < BOOT_BC7_ALLOCS; ++i) if (boot_bc7.alloc[i]) ID3D12CommandAllocator_Release(boot_bc7.alloc[i]);
    if (boot_bc7.qheap) ID3D12QueryHeap_Release(boot_bc7.qheap);
    if (boot_bc7.qread) ID3D12Resource_Release(boot_bc7.qread);
    if (boot_bc7.in) ID3D12Resource_Release(boot_bc7.in);
    if (boot_bc7.modes) ID3D12Resource_Release(boot_bc7.modes);
    boot_bc7_encoder_release(&boot_bc7.enc);
    boot_bc7.cl = NULL; memset(boot_bc7.alloc, 0, sizeof boot_bc7.alloc); memset(boot_bc7.alloc_fence, 0, sizeof boot_bc7.alloc_fence);
    memset(boot_bc7.ts_pending, 0, sizeof boot_bc7.ts_pending);
    boot_bc7.qheap = NULL; boot_bc7.qread = NULL; boot_bc7.in = boot_bc7.modes = NULL; boot_bc7.dev = NULL; boot_bc7.failed = 0;
}
static inline ID3D12Resource *boot_bc7_buffer(BootRender *r, D3D12_HEAP_TYPE type, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags; ID3D12Resource *x = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&x))) return NULL;
    ++boot_render_created_buffers;
    return x;
}
static inline int boot_bc7_open(BootRender *r) {
    if (boot_bc7.dev == r->gpu->device) return !boot_bc7.failed;
    if (boot_bc7.dev) boot_bc7_objects_release();
    boot_bc7.dev = r->gpu->device; boot_bc7.failed = 1;
    if (!boot_bc7_encoder_open(&boot_bc7.enc, r->gpu->device, boot_bc7_psoc, r, boot_psoc_root_note)) return 0;
    for (unsigned i = 0; i < BOOT_BC7_ALLOCS; ++i)
        if (FAILED(ID3D12Device_CreateCommandAllocator(r->gpu->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&boot_bc7.alloc[i]))) {
            boot_bc7.alloc[i] = NULL; return 0; }
    if (FAILED(ID3D12Device_CreateCommandList(r->gpu->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, boot_bc7.alloc[0], NULL, &IID_ID3D12GraphicsCommandList, (void **)&boot_bc7.cl))) {
        boot_bc7.cl = NULL; return 0; }
    boot_dred_created(boot_bc7.cl);
    ID3D12GraphicsCommandList_Close(boot_bc7.cl);
    D3D12_QUERY_HEAP_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2 * BOOT_BC7_ALLOCS;
    if (FAILED(ID3D12Device_CreateQueryHeap(r->gpu->device, &qd, &IID_ID3D12QueryHeap, (void **)&boot_bc7.qheap))) { boot_bc7.qheap = NULL; return 0; }
    if (FAILED(ID3D12CommandQueue_GetTimestampFrequency(r->gpu->queue, &boot_bc7.ts_freq)) || !boot_bc7.ts_freq) return 0;
    if (!(boot_bc7.qread = boot_bc7_buffer(r, D3D12_HEAP_TYPE_READBACK, 16 * BOOT_BC7_ALLOCS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST))) return 0;
    if (!(boot_bc7.in = boot_bc7_buffer(r, D3D12_HEAP_TYPE_DEFAULT, BOOT_BC7_IN_BYTES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON))) return 0;
    if (!(boot_bc7.modes = boot_bc7_buffer(r, D3D12_HEAP_TYPE_DEFAULT, 2 * BOOT_BC7_MODE_BYTES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON))) return 0;
    if (boot_bc7.rate <= 0) boot_bc7.rate = boot_bc7_cfg.flags & BOOT_BC7_FLAG_QUICK ? 40000.0 : boot_bc7_cfg.flags & BOOT_BC7_FLAG_MODE02 ? 2000.0 : 3000.0;   /* blocks per GPU ms, harness-based start */
    boot_bc7.failed = 0;
    return 1;
}
static inline void boot_bc7_rb(ID3D12GraphicsCommandList *cl, ID3D12Resource *x, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t; memset(&t, 0, sizeof t); t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; t.Transition.pResource = x;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; t.Transition.StateBefore = a; t.Transition.StateAfter = b;
    BOOT_GP_RB("bc7 encode", cl, 1, &t);
}
#define BOOT_BC7_SRV (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
/* Starts the next queued job whose slot still holds its texture: references it and makes the BC7 texture and buffers.
 * The smallest of the first 16 queued jobs goes first (moved to the head; the head takes its place). */
static inline int boot_bc7_start(BootRender *r) {
    while (boot_bc7.q_n) {
        {   unsigned best = 0, scan = boot_bc7.q_n < 16 ? boot_bc7.q_n : 16;
            for (unsigned k = 1; k < scan; ++k)
                if (boot_bc7.queue[(boot_bc7.q_head + k) % BOOT_BC7_QUEUE].rgba_bytes < boot_bc7.queue[(boot_bc7.q_head + best) % BOOT_BC7_QUEUE].rgba_bytes) best = k;
            if (best) {
                BootBc7Job t = boot_bc7.queue[boot_bc7.q_head]; boot_bc7.queue[boot_bc7.q_head] = boot_bc7.queue[(boot_bc7.q_head + best) % BOOT_BC7_QUEUE];
                boot_bc7.queue[(boot_bc7.q_head + best) % BOOT_BC7_QUEUE] = t;
            }
        }
        BootBc7Job *q = &boot_bc7.queue[boot_bc7.q_head]; BootBc7Job j = *q; memset(q, 0, sizeof *q);
        boot_bc7.q_head = (boot_bc7.q_head + 1) % BOOT_BC7_QUEUE; --boot_bc7.q_n;
        const BootRenderTexSlot *s = &boot_render_tex_slots[j.index];
        if (!s->valid || s->res != j.rgba || s->view_of || boot_render_tex_serial[j.index] != j.serial || boot_render_tex_poisoned[j.index]) { ++boot_bc7.x[BOOT_BC7_X_STALE]; continue; }
        D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = j.width; d.Height = j.height; d.DepthOrArraySize = (UINT16)j.layers; d.MipLevels = (UINT16)j.levels;
        d.Format = j.bc7_fmt; d.SampleDesc.Count = 1;
        if ((j.bc7 = boot_render_tex_pool_take(&d)) != NULL) j.bc7_pooled = 1;
        else {
            D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
            if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&j.bc7))) j.bc7 = NULL;
        }
        if (j.bc7) { ID3D12Resource_AddRef(j.rgba); j.ref = 1; }   /* from here on the RGBA8 texture is ours too (see the file comment) */
        j.fp = j.bc7 ? (D3D12_PLACED_SUBRESOURCE_FOOTPRINT *)calloc(j.subs, sizeof *j.fp) : NULL;
        if (j.fp) {
            ID3D12Device_GetCopyableFootprints(r->gpu->device, &d, 0, j.subs, 0, j.fp, NULL, NULL, &j.total);
            j.bc7_bytes = boot_bc7_payload_bytes(j.width, j.height, j.layers, j.levels);
            j.out = boot_bc7_buffer(r, D3D12_HEAP_TYPE_DEFAULT, j.total, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (j.out && boot_bc7_cfg.cache_on && boot_bc7_disk.ok) {
                if (j.bc7_bytes <= BOOT_BC7_CACHE_MAX_PAYLOAD) j.rb = boot_bc7_buffer(r, D3D12_HEAP_TYPE_READBACK, j.total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                else ++boot_bc7.cache_skipped;
            }
        }
        if (!j.bc7 || !j.fp || !j.out) { boot_bc7_job_release(&j); ++boot_bc7.x[BOOT_BC7_X_NO_MEMORY]; continue; }
        boot_bc7.active = j; boot_bc7.has_active = 1;
        return 1;
    }
    return 0;
}
/* The slots holding the job's RGBA8 texture (dedup shares it between TIC indices). */
static inline void boot_bc7_holders(BootBc7Job *j) {
    j->n_holders = 0;
    for (uint32_t k = 0; k < boot_render_tex_live_n && j->n_holders < BOOT_BC7_HOLDERS; ++k) {
        const uint32_t i = boot_render_tex_live[k]; const BootRenderTexSlot *s = &boot_render_tex_slots[i];
        if (s->valid && s->res == j->rgba && !s->view_of) j->holders[j->n_holders++] = i;
    }
}
/* Slot i's descriptors can be rewritten in place now: nothing recorded or executed that binds it can still run (the
 * rule boot_render_tex_trim_live evicts by; at an idle point every list is complete). */
static inline int boot_bc7_slot_quiet(BootRender *r, uint32_t i) {
    return i < BOOT_RENDER_TEX_SLOTS - 256u && !boot_render_tex_pending_n && !boot_render_tex_pending_over && !boot_render_fb_on[i] && !boot_render_sv_flag[i] &&
           boot_render_list_done(r, boot_render_slot_list[i]) && boot_render_list_done(r, boot_render_slot_rec[i]);
}
/* Fence after which no executed list binding slot i runs (0: none known). */
static inline uint64_t boot_bc7_slot_fence(uint32_t i) {
    const uint64_t ids[2] = {boot_render_slot_list[i], boot_render_slot_rec[i]}; uint64_t f = 0;
    for (unsigned k = 0; k < 2; ++k)
        if (ids[k] && boot_render_list_fence[ids[k] & 65535].id == ids[k] && boot_render_list_fence[ids[k] & 65535].fence > f) f = boot_render_list_fence[ids[k] & 65535].fence;
    return f;
}
/* Slot i (not quiet) can be switched through the renderer's slot versioning without any wait: executed lists read it,
 * the open list does not, and the staging heap has room (a full one is applied with a wait). */
static inline int boot_bc7_can_stage(BootRender *r, uint32_t i) {
    if (!boot_bc7_cfg.staged || !boot_render_sv_on() || i >= BOOT_RENDER_TEX_SLOTS - 256u || boot_render_tex_pending_n || boot_render_tex_pending_over) return 0;
    if (boot_render_fb_on[i] || boot_render_sv_flag[i] || boot_render_slot_list[i] == boot_render_list_id || boot_render_slot_rec[i] == boot_render_list_id) return 0;
    if (boot_render_sv_idx_n + 1 >= BOOT_RENDER_SV_STAGE || boot_render_sv_n + 2 >= BOOT_RENDER_SV_STAGE) return 0;
    if (boot_render_sv_heap && boot_render_sv_dev != r->gpu->device) return 0;
    return boot_bc7_slot_fence(i) != 0;
}
/* The slot's view of `res` (as boot_render_tex_isolate_srv writes it: the own table, and the one-layer 2D-array view
 * of a 2D texture; the other tables hold null descriptors since the build). */
static inline void boot_bc7_write_views(BootRender *r, uint32_t i, unsigned dim, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *sv) {
    ID3D12Device_CreateShaderResourceView(r->gpu->device, res, sv, boot_render_tex_cpu(r, dim, i));
    if (dim != BOOT_DIM_2D || sv->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) return;
    D3D12_RESOURCE_DESC rd; res->lpVtbl->GetDesc(res, &rd);
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.DepthOrArraySize != 1) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC a; memset(&a, 0, sizeof a);
    a.Format = sv->Format; a.Shader4ComponentMapping = sv->Shader4ComponentMapping; a.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    a.Texture2DArray.MostDetailedMip = sv->Texture2D.MostDetailedMip; a.Texture2DArray.MipLevels = sv->Texture2D.MipLevels; a.Texture2DArray.ArraySize = 1;
    ID3D12Device_CreateShaderResourceView(r->gpu->device, res, &a, boot_render_tex_cpu(r, BOOT_DIM_2D_ARRAY, i));
}
/* Switches the job's remaining holders that can be switched now. 1: the job is done (every holder switched or gone,
 * or the job let go) and no list that may read an RGBA8 descriptor of it is still running. */
static inline int boot_bc7_switch(BootRender *r, BootBc7Job *j, uint64_t done) {
    if (!j->abandon && boot_bc7_vtx_has(j->va)) { j->abandon = 1; ++boot_bc7.x[BOOT_BC7_X_VERTEX]; }   /* a vertex stage sampled it meanwhile */
    if (!j->abandon) {
        unsigned keep = 0;
        for (unsigned k = 0; k < j->n_holders; ++k) {
            const uint32_t i = j->holders[k]; BootRenderTexSlot *s = &boot_render_tex_slots[i];
            if (!s->valid || s->res != j->rgba || s->view_of || s->dim != j->dim || boot_render_tex_poisoned[i] || boot_render_dfb_slot[i] || boot_render_tex_subres[i]) continue;   /* no longer a holder */
            int how = boot_bc7_slot_quiet(r, i) ? 1 : boot_bc7_can_stage(r, i) ? 2 : 0;
            if (how == 2) {
                const uint64_t f = boot_bc7_slot_fence(i);
                if (boot_render_sv_mark(r, i, f)) { if (f > j->hold_fence) j->hold_fence = f; } else how = 0;
            }
            if (!how) { j->holders[keep++] = i; ++boot_bc7.switch_busy; continue; }   /* again at the next pump point */
            if (how == 1) ++boot_bc7.switch_inplace; else ++boot_bc7.switch_staged;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = j->sv; sv.Format = j->bc7_fmt;
            boot_bc7_write_views(r, i, s->dim, j->bc7, &sv);
            ID3D12Resource_AddRef(j->bc7); s->res = j->bc7; ID3D12Resource_Release(j->rgba);   /* the job still holds the RGBA8 one */
            boot_render_tex_sized[i].res = NULL; boot_render_tex_bc7[i] = 1;
            boot_render_tex_dedup_map[boot_render_tex_dedup_hash(s->words, s->dim)] = i + 1;   /* the next index for this image shares the BC7 one */
            if (boot_render_tex_watch_of[i]) {   /* a watch follows the resource (ASTC is never patched in place: changes rebuild) */
                uint32_t e = boot_render_tex_watch_of[i] - 1u;
                if (e < BOOT_RENDER_TEX_WATCH && boot_render_tex_watch[e].res == j->rgba) boot_render_tex_watch[e].res = j->bc7;
            }
            if (!j->switched++) boot_bc7.saved += j->rgba_bytes > j->bc7_bytes ? j->rgba_bytes - j->bc7_bytes : 0;
            ++boot_bc7.swaps;
        }
        j->n_holders = keep;
        if (keep) {
            if (++j->pumps <= BOOT_BC7_SWITCH_PUMPS) return 0;
            j->abandon = 1; ++boot_bc7.x[BOOT_BC7_X_BUSY];
        } else if (!j->rescanned) {   /* slots that shared it since (dedup): once */
            j->rescanned = 1; boot_bc7_holders(j);
            if (j->n_holders) return 0;
        }
    }
    return done >= j->hold_fence;
}
/* An encode completed on the GPU: its blocks go to the cache writer, its buffers are freed, its holders collected. */
static inline void boot_bc7_encoded(BootBc7Job *j) {
    ++boot_bc7.transcoded;
    if (j->rb) {
        uint8_t *p = NULL; D3D12_RANGE rr = {0, (SIZE_T)j->total};
        uint8_t *file = (uint8_t *)malloc(sizeof(BootBc7FileHeader) + (size_t)j->bc7_bytes);
        if (file && SUCCEEDED(ID3D12Resource_Map(j->rb, 0, &rr, (void **)&p)) && p) {
            for (unsigned sub = 0; sub < j->subs; ++sub) {   /* footprint rows -> tight payload */
                uint32_t rb = 0, rn = 0; uint8_t *o = file + sizeof(BootBc7FileHeader) + boot_bc7_tight_offset(j->width, j->height, j->levels, sub, &rb, &rn);
                for (uint32_t y = 0; y < rn; ++y) memcpy(o + (size_t)y * rb, p + j->fp[sub].Offset + (size_t)y * j->fp[sub].Footprint.RowPitch, rb);
            }
            D3D12_RANGE wr = {0, 0}; ID3D12Resource_Unmap(j->rb, 0, &wr);
            BootBc7FileHeader h; memset(&h, 0, sizeof h); memcpy(h.magic, "TOTKBC7", 8); h.version = BOOT_BC7_CACHE_VERSION; h.header_bytes = sizeof h;
            h.producer = j->producer; h.encoder = (uint32_t)boot_bc7_cfg.quality | boot_bc7_cfg.flags << 8; h.encoder_param = boot_bc7_cfg.refine;
            h.dxgi_format = (uint32_t)j->bc7_fmt; h.dim = j->dim; h.width = j->width; h.height = j->height; h.layers = j->layers; h.levels = j->levels;
            h.subresources = j->subs; h.payload_bytes = j->bc7_bytes; h.payload_check = boot_bc7_check64(file + sizeof h, j->bc7_bytes); h.key = j->key;
            memcpy(file, &h, sizeof h);
            if (boot_bc7_disk_write(&j->key, file, sizeof h + j->bc7_bytes)) ++boot_bc7.cache_queued;
        } else free(file);
        ID3D12Resource_Release(j->rb); j->rb = NULL;
    }
    if (j->out) { ID3D12Resource_Release(j->out); j->out = NULL; }
    free(j->fp); j->fp = NULL;
    boot_bc7_holders(j);
    j->state = BOOT_BC7_J_SWITCHING;
}
static inline void boot_bc7_rate_update(uint64_t done) {
    for (unsigned a = 0; a < BOOT_BC7_ALLOCS; ++a) {
        if (!boot_bc7.ts_pending[a] || boot_bc7.alloc_fence[a] > done) continue;
        boot_bc7.ts_pending[a] = 0;
        uint64_t *t = NULL; D3D12_RANGE rr = {16 * a, 16 * a + 16}, wr = {0, 0};
        if (FAILED(ID3D12Resource_Map(boot_bc7.qread, 0, &rr, (void **)&t)) || !t) continue;
        uint64_t t0 = t[2 * a], t1 = t[2 * a + 1]; ID3D12Resource_Unmap(boot_bc7.qread, 0, &wr);
        if (t1 <= t0) continue;
        double ms = (double)(t1 - t0) * 1000.0 / (double)boot_bc7.ts_freq;
        boot_bc7.gpu_us += (uint64_t)(ms * 1000.0);
        if (boot_bc7.ts_blocks[a] >= 1024 && ms > 0.01) {
            double rate = (double)boot_bc7.ts_blocks[a] / ms;
            boot_bc7.rate = boot_bc7.rate * 0.7 + rate * 0.3;
            if (boot_bc7.rate < 200) boot_bc7.rate = 200;
        }
    }
}
/* The encode list could not be closed or signalled (device trouble): after the wait every job it carried, finished or
 * active, is dropped (their blocks were never written) and the encoder stops. */
static inline void boot_bc7_record_failed(unsigned finished0) {
    for (unsigned i = finished0; i < boot_bc7.n_finishing; ++i) { boot_bc7_job_release(&boot_bc7.finishing[i]); ++boot_bc7.x[BOOT_BC7_X_DEVICE]; }
    boot_bc7.n_finishing = finished0;
    if (boot_bc7.has_active) { boot_bc7_job_release(&boot_bc7.active); boot_bc7.has_active = 0; ++boot_bc7.x[BOOT_BC7_X_DEVICE]; }
    boot_bc7.failed = 1;
}
/* Records and executes encode work for up to `ms` estimated GPU milliseconds. */
static inline void boot_bc7_record(BootRender *r, double ms) {
    BootGpu *g = r->gpu; unsigned a = boot_bc7.next_alloc;
    if (boot_bc7.alloc_fence[a] > ID3D12Fence_GetCompletedValue(g->fence) || boot_bc7.ts_pending[a]) return;   /* never waits */
    ID3D12GraphicsCommandList *cl = boot_bc7.cl;
    if (FAILED(ID3D12CommandAllocator_Reset(boot_bc7.alloc[a])) || FAILED(ID3D12GraphicsCommandList_Reset(cl, boot_bc7.alloc[a], NULL))) return;
    boot_bc7.next_alloc = (a + 1) % BOOT_BC7_ALLOCS;
    boot_dred_reset(cl, g->queue, g->device, boot_render_exec_id, 9);
    BOOT_GP_LIST_BEGIN(g, cl, "bc7 encode list");
    ID3D12GraphicsCommandList_EndQuery(cl, boot_bc7.qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * a);
    const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    boot_bc7_rb(cl, boot_bc7.in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    boot_bc7_rb(cl, boot_bc7.modes, D3D12_RESOURCE_STATE_COMMON, UAV);
    const D3D12_GPU_VIRTUAL_ADDRESS in_va = ID3D12Resource_GetGPUVirtualAddress(boot_bc7.in), mode_va = ID3D12Resource_GetGPUVirtualAddress(boot_bc7.modes);
    uint64_t budget = (uint64_t)(ms * boot_bc7.rate), done = 0; unsigned finished0 = boot_bc7.n_finishing;
    if (budget < 1024) budget = 1024;
    while (done < budget && boot_bc7.n_finishing < BOOT_BC7_FINISHING && (boot_bc7.has_active || boot_bc7_start(r))) {
        BootBc7Job *j = &boot_bc7.active;
        if (!j->touched) {
            j->touched = 1;
            boot_bc7_rb(cl, j->rgba, BOOT_BC7_SRV, D3D12_RESOURCE_STATE_COPY_SOURCE);
            boot_bc7_rb(cl, j->out, D3D12_RESOURCE_STATE_COMMON, UAV);
        }
        const uint32_t l = j->sub % j->levels, mw = j->width >> l ? j->width >> l : 1, mh = j->height >> l ? j->height >> l : 1;
        const uint32_t bx = boot_bc7_blocks_x(mw), by = boot_bc7_blocks_y(mh), pitch = boot_bc7_in_pitch(mw);
        uint64_t rows = (budget - done) / bx; if (!rows) rows = 1;
        uint64_t fit = BOOT_BC7_IN_BYTES / ((uint64_t)pitch * 4); if (rows > fit) rows = fit;
        if (rows > by - j->row) rows = by - j->row;
        const uint32_t y0 = j->row * 4, y1 = (uint32_t)((j->row + rows) * 4 < mh ? (j->row + rows) * 4 : mh);
        if (!boot_bc7_producer_fill(j, cl, j->sub, mw, y0, y1, boot_bc7.in, pitch)) {   /* no producer for it: back to its state, dropped */
            boot_bc7_rb(cl, j->rgba, D3D12_RESOURCE_STATE_COPY_SOURCE, BOOT_BC7_SRV); boot_bc7_rb(cl, j->out, UAV, D3D12_RESOURCE_STATE_COMMON);
            boot_bc7_rb(cl, boot_bc7.in, D3D12_RESOURCE_STATE_COPY_DEST, UAV);   /* as a fill would leave it */
            j->touched = -1; j->state = BOOT_BC7_J_DROPPED; j->fence = 0; boot_bc7.finishing[boot_bc7.n_finishing++] = *j;   /* released once its list ran */
            memset(j, 0, sizeof *j); boot_bc7.has_active = 0; ++boot_bc7.x[BOOT_BC7_X_DEVICE];
        } else {
            BootBc7Band b = {mw, mh, j->row, (uint32_t)rows, in_va, pitch, ID3D12Resource_GetGPUVirtualAddress(j->out) + j->fp[j->sub].Offset, j->fp[j->sub].Footprint.RowPitch / 16};
            boot_bc7_record_band(&boot_bc7.enc, cl, &b, mode_va, mode_va + BOOT_BC7_MODE_BYTES, boot_bc7_cfg.flags, boot_bc7_cfg.refine);
            done += rows * bx; j->row += (uint32_t)rows;
            if (j->row >= by) { j->row = 0; ++j->sub; }
            if (j->sub == j->subs) {   /* complete: blocks into the BC7 texture (and the readback), the RGBA8 back to sampling */
                boot_bc7_rb(cl, j->rgba, D3D12_RESOURCE_STATE_COPY_SOURCE, BOOT_BC7_SRV);
                boot_bc7_rb(cl, j->out, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
                if (j->bc7_pooled) boot_bc7_rb(cl, j->bc7, BOOT_BC7_SRV, D3D12_RESOURCE_STATE_COPY_DEST);
                for (unsigned sub = 0; sub < j->subs; ++sub) {
                    D3D12_TEXTURE_COPY_LOCATION dl = {j->bc7, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, sl = {j->out, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
                    dl.SubresourceIndex = sub; sl.PlacedFootprint = j->fp[sub];
                    BOOT_GP_CTR("bc7 encode", cl, &dl, 0, 0, 0, &sl, NULL);
                }
                boot_bc7_rb(cl, j->bc7, D3D12_RESOURCE_STATE_COPY_DEST, BOOT_BC7_SRV);
                if (j->rb) BOOT_GP_CBR("bc7 encode", cl, j->rb, 0, j->out, 0, j->total);
                boot_bc7_rb(cl, j->out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                j->fence = 0; j->state = BOOT_BC7_J_ENCODED; boot_bc7.finishing[boot_bc7.n_finishing++] = *j;
                memset(j, 0, sizeof *j); boot_bc7.has_active = 0;
            }
        }
        boot_bc7_rb(cl, boot_bc7.in, UAV, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (boot_bc7.has_active && boot_bc7.active.touched > 0) {   /* continues at the next idle point */
        boot_bc7.active.touched = 0;
        boot_bc7_rb(cl, boot_bc7.active.rgba, D3D12_RESOURCE_STATE_COPY_SOURCE, BOOT_BC7_SRV);
        boot_bc7_rb(cl, boot_bc7.active.out, UAV, D3D12_RESOURCE_STATE_COMMON);
    }
    boot_bc7_rb(cl, boot_bc7.in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    boot_bc7_rb(cl, boot_bc7.modes, UAV, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_EndQuery(cl, boot_bc7.qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * a + 1);
    ID3D12GraphicsCommandList_ResolveQueryData(cl, boot_bc7.qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * a, 2, boot_bc7.qread, 16 * a);
    BOOT_GP_LIST_END(cl);
    if (FAILED(ID3D12GraphicsCommandList_Close(cl))) { boot_gpu_wait(g); boot_bc7_record_failed(finished0); return; }
    boot_dred_execute(cl, g->queue, 9);
    ID3D12CommandQueue_ExecuteCommandLists(g->queue, 1, (ID3D12CommandList **)&cl);
    uint64_t fence = ++g->submitted;
    if (FAILED(ID3D12CommandQueue_Signal(g->queue, g->fence, fence))) { boot_gpu_wait(g); boot_bc7_record_failed(finished0); return; }
    BOOT_GP_LIST_EXEC(cl, fence);
    boot_bc7.alloc_fence[a] = fence; boot_bc7.ts_blocks[a] = done; boot_bc7.ts_pending[a] = 1;
    for (unsigned i = finished0; i < boot_bc7.n_finishing; ++i) boot_bc7.finishing[i].fence = fence;
    boot_bc7.blocks += done; ++boot_bc7.lists;
    boot_bc7.frame_ms += (double)done / boot_bc7.rate;
}
/* A pump point (see the file comment): `where` BOOT_BC7_AT_*. Never waits. */
static inline void boot_bc7_pump(BootRender *r, unsigned where) {
    if (!boot_bc7_cfg.on || !boot_render_tex_slots || where >= BOOT_BC7_AT_N) return;
    BootGpu *g = r->gpu;
    if (boot_bc7.dev && boot_bc7.dev != g->device) boot_bc7_objects_release();   /* device recovered: the old objects are gone */
    ++boot_bc7.pumps[where];
    const uint64_t done = ID3D12Fence_GetCompletedValue(g->fence);
    if (boot_bc7.dev) boot_bc7_rate_update(done);
    unsigned keep = 0;
    for (unsigned i = 0; i < boot_bc7.n_finishing; ++i) {
        BootBc7Job *j = &boot_bc7.finishing[i]; int release = 0;
        if (j->state == BOOT_BC7_J_DROPPED) release = j->fence <= done;
        else {
            if (j->state == BOOT_BC7_J_ENCODED && j->fence <= done) boot_bc7_encoded(j);
            if (j->state == BOOT_BC7_J_SWITCHING) release = boot_bc7_switch(r, j, done);
        }
        if (release) boot_bc7_job_release(j);   /* the RGBA8 texture is freed with its last slot reference */
        else { if (keep != i) boot_bc7.finishing[keep] = *j; ++keep; }
    }
    boot_bc7.n_finishing = keep;
    if (where == BOOT_BC7_AT_SUBMIT_BEGIN || (!boot_bc7.has_active && !boot_bc7.q_n)) return;   /* encode lists follow a kick or an idle point */
    uint64_t now = boot_tex_now();
    if (boot_bc7.frame_qpc && now - boot_bc7.frame_qpc > boot_bc7_qpf() / 20) { boot_bc7.frame_qpc = now; boot_bc7.frame_ms = 0; }   /* no present for 50 ms */
    if (!boot_bc7.frame_qpc) boot_bc7.frame_qpc = now;
    double left = boot_bc7_cfg.budget_ms - boot_bc7.frame_ms;
    if (left < 0.05) return;
    if (!boot_bc7_open(r)) {   /* no encoder: everything queued stays RGBA8 */
        boot_bc7.x[BOOT_BC7_X_DEVICE] += boot_bc7.q_n; boot_bc7.q_n = 0;
        if (boot_bc7.has_active) { boot_bc7_job_release(&boot_bc7.active); boot_bc7.has_active = 0; ++boot_bc7.x[BOOT_BC7_X_DEVICE]; }
        return;
    }
    boot_bc7_record(r, left);
}
/* End of boot_render_tex_trim (queue idle, nothing recording). */
static inline void boot_bc7_idle(BootRender *r) { boot_bc7_pump(r, BOOT_BC7_AT_IDLE); }
/* Every present (host_nvn_play.h): a new frame budget; every 300 presents one draw-path.log line. */
static inline void boot_bc7_present(BootRender *r, uint64_t frame) {
    (void)r;
    if (!boot_bc7_on()) return;
    ++boot_bc7.presents; boot_bc7.frame_ms = 0; boot_bc7.frame_qpc = boot_tex_now();
    if (boot_bc7.presents % 300) return;
    char xs[640]; size_t n = 0; xs[0] = 0;
    for (unsigned i = 1; i < BOOT_BC7_X_COUNT; ++i) {
        if (!boot_bc7.x[i]) continue;
        int k = snprintf(xs + n, sizeof xs - n, " %s %llu", boot_bc7_x_name[i], (unsigned long long)boot_bc7.x[i]);
        if (k < 0 || (size_t)k >= sizeof xs - n) break;
        n += (size_t)k;
    }
    const double mib = 1.0 / 1048576.0;
    boot_gp_log("bc7: present %llu (frame %llu): transcoded %llu (+%llu, %llu slots switched), MiB saved %.1f (encodes %.1f + cache hits %.1f, +%.1f);"
                " cache hits %llu (+%llu, %.1f ms reading) misses %llu%s, writes queued %llu done %lld dropped %lld failed %lld trims %lld;"
                " encode GPU %.1f ms (+%.1f = %.2f ms/frame), %.0f blocks/ms, %llu lists, hash %.1f ms; queue %u%s, finishing %u, producer %s;"
                " pumps idle %llu / submit begin %llu / end %llu; switches in place %llu staged %llu (kick waits +%llu), busy checks %llu, vertex reverts %llu"
                " (vertex images %u%s); exclusions:%s",
                (unsigned long long)boot_bc7.presents, (unsigned long long)frame, (unsigned long long)boot_bc7.transcoded,
                (unsigned long long)(boot_bc7.transcoded - boot_bc7.last_transcoded), (unsigned long long)boot_bc7.swaps,
                (boot_bc7.saved + boot_bc7.hit_saved) * mib, boot_bc7.saved * mib, boot_bc7.hit_saved * mib,
                (boot_bc7.saved + boot_bc7.hit_saved - boot_bc7.last_saved) * mib,
                (unsigned long long)boot_bc7.hits, (unsigned long long)(boot_bc7.hits - boot_bc7.last_hits), boot_bc7.hit_us / 1000.0,
                (unsigned long long)boot_bc7.misses, boot_bc7_cfg.cache_on ? "" : " (cache off)", (unsigned long long)boot_bc7.cache_queued,
                (long long)boot_bc7_disk.writes, (long long)boot_bc7_disk.dropped, (long long)boot_bc7_disk.write_fails, (long long)boot_bc7_disk.trims,
                boot_bc7.gpu_us / 1000.0, (boot_bc7.gpu_us - boot_bc7.last_gpu_us) / 1000.0, (boot_bc7.gpu_us - boot_bc7.last_gpu_us) / 1000.0 / 300.0,
                boot_bc7.rate, (unsigned long long)boot_bc7.lists, boot_bc7.hash_us / 1000.0, boot_bc7.q_n, boot_bc7.has_active ? " + 1 active" : "", boot_bc7.n_finishing,
                boot_bc7_producer_name[BOOT_BC7_PRODUCER_CPU_DECODE_GPU_ENCODE],
                (unsigned long long)boot_bc7.pumps[BOOT_BC7_AT_IDLE], (unsigned long long)boot_bc7.pumps[BOOT_BC7_AT_SUBMIT_BEGIN], (unsigned long long)boot_bc7.pumps[BOOT_BC7_AT_SUBMIT_END],
                (unsigned long long)boot_bc7.switch_inplace, (unsigned long long)boot_bc7.switch_staged, (unsigned long long)(boot_render_sv_kick_waits - boot_bc7.last_kick_waits),
                (unsigned long long)boot_bc7.switch_busy, (unsigned long long)boot_bc7.vtx_reverts, boot_bc7_vtx_n, boot_bc7_vtx_full ? ", table full" : "", n ? xs : " none");
    boot_bc7.last_transcoded = boot_bc7.transcoded; boot_bc7.last_gpu_us = boot_bc7.gpu_us; boot_bc7.last_saved = boot_bc7.saved + boot_bc7.hit_saved;
    boot_bc7.last_hits = boot_bc7.hits; boot_bc7.last_kick_waits = boot_render_sv_kick_waits;
}
#endif
