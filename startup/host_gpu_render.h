#ifndef TOTK_HOST_GPU_RENDER_H
#define TOTK_HOST_GPU_RENDER_H
/* D3D12 execution of decoded Maxwell draws.
 *
 * Guest GPU memory is used in place: one VirtualAlloc'd, fully committed guest
 * region is opened with ID3D12Device3::OpenExistingHeapFromAddress and exposed
 * to every translated shader as `gpu_arena` (test_existing_heap.c verified
 * zero-copy reads and writes on the target GPU). A page table maps 64 KiB
 * guest GPU pages to arena offsets, matching maxwell_runtime.hlsli.
 * Constant buffers are snapshotted per draw into an upload ring, because the
 * source rewrites them between draws (LOAD_CONSTANT_BUFFER).
 * Shaders are looked up by canonical id (sha256 of SPH + code through the
 * terminator, computed from guest memory) in shader-manifest.tsv. Nothing is
 * compiled at runtime; an unknown program, format or state stops the draw. */
#define COBJMACROS
#include "host_gpu.h"
#include "host_gpu_timestamp.h"   /* render-40: GPU timestamp profiler (TOTK_GPU_PROFILE) */
#include "host_gpu_draw_state.h"
#include "host_texture_desc.h"
#include "host_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bcrypt.h>
#include "host_gpu_dred_trace.h"
#include "host_gpu_cbv_tap.h"
#include "host_tile_binding_trace.h"
#include "host_image_trace.h"
#include "host_title_trace.h"
#include "host_light_copy_probe.h"
#include "host_light_draw_probe.h"
#include "host_light_input_probe.h"

/* Recording rotates over BOOT_RENDER_SEGMENTS segments of the CBV descriptor ring and of the upload
 * ring (boot_render_begin): a list that fills its segment is executed without waiting for it
 * (boot_render_kick) and recording continues in the next segment, whose previous list must have
 * completed. boot_render_submit still waits for the queue. */
#ifndef BOOT_RENDER_SEGMENTS   /* tests may use smaller rings */
#define BOOT_RENDER_SEGMENTS      16   /* deeper GPU pipelining: a segment is reused ~2 submissions later (was 8: ~1) */
#endif
#ifndef BOOT_RENDER_CBV_SEGMENT
#define BOOT_RENDER_CBV_SEGMENT   24576      /* CBV descriptors per recorded list (16 x 24576 = 393216; heap total 918k < 1M) */
#endif
#ifndef BOOT_RENDER_RING_BYTES
#define BOOT_RENDER_RING_BYTES    (UINT64_C(384) << 20)   /* upload ring: BOOT_RENDER_SEGMENTS segments of 24 MiB */
#endif
#define BOOT_RENDER_CBV_RING      (BOOT_RENDER_SEGMENTS * BOOT_RENDER_CBV_SEGMENT)
#define BOOT_RENDER_TEX_SLOTS     65536      /* TIC indices exposed per dimension table */
#define BOOT_RENDER_DIMS          8          /* 1d, 2d, 2d array, 3d, cube, cube array, buffer, 1d array */
/* Descriptor table per texture dimension; register spaces as declared in maxwell_runtime.hlsli
 * (space 7 is guest memory). */
enum { BOOT_DIM_1D = 0, BOOT_DIM_2D = 1, BOOT_DIM_2D_ARRAY = 2, BOOT_DIM_3D = 3, BOOT_DIM_CUBE = 4, BOOT_DIM_CUBE_ARRAY = 5,
       BOOT_DIM_BUFFER = 6, BOOT_DIM_1D_ARRAY = 7 };
static const UINT boot_render_dim_space[BOOT_RENDER_DIMS] = {1, 2, 3, 4, 5, 6, 8, 9};
#define BOOT_RENDER_SAMPLERS      1024       /* per table (regular, comparison); D3D12 caps a visible sampler heap at 2048 */
#define BOOT_RENDER_MAX_RT        64
#define BOOT_RENDER_MAX_PSO       8192   /* the prologue alone needs more than 512 */
#define BOOT_RENDER_MAX_SHADERS   1024
#define BOOT_RENDER_MAX_REGIONS   256        /* zero-copy guest memory buffers (maxwell_runtime.hlsli space7) */

enum { BOOT_RENDER_OK = 0, BOOT_RENDER_E_STATE = 1, BOOT_RENDER_E_SHADER = 2, BOOT_RENDER_E_MEMORY = 3,
       BOOT_RENDER_E_FORMAT = 4, BOOT_RENDER_E_D3D = 5, BOOT_RENDER_E_CAPACITY = 6 };

typedef struct BootRenderShader { uint8_t id[32]; uint32_t stage, vs_mask, tex_count, tex[64]; void *dxil; size_t size; } BootRenderShader;
typedef struct BootRenderTarget {
    uint64_t address; uint32_t width, height, format; DXGI_FORMAT dxgi;
    ID3D12Resource *texture; D3D12_RESOURCE_STATES state; uint32_t rtv;
} BootRenderTarget;
typedef struct BootRenderPso { uint8_t key[32]; ID3D12PipelineState *pso; } BootRenderPso;
/* Same layout as BootGpuMemoryWrite: staged guest writes that precede a draw. */
typedef struct BootRenderWrite { uint64_t gpu; uint32_t value, kind; } BootRenderWrite;
static ID3D12Resource *boot_render_uploads[256]; static unsigned boot_render_upload_count;
static uint64_t boot_render_upload_bytes;   /* upload buffer bytes held until the recorded list executes */
#ifndef BOOT_RENDER_UPLOAD_CHUNK   /* tests set a small chunk to exercise chunked uploads */
#define BOOT_RENDER_UPLOAD_CHUNK  (UINT64_C(64) << 20)
#endif
#define BOOT_RENDER_UPLOAD_BUDGET (UINT64_C(512) << 20)
static uint64_t boot_render_rt_alias_count;   /* textures sampled straight from host render targets */
static uint64_t boot_render_created_buffers, boot_render_created_regions;   /* D3D12 objects created (profiling) */

typedef struct BootRender {
    BootGpu *gpu;
    ID3D12RootSignature *root;
    ID3D12DescriptorHeap *srv_heap, *sampler_heap, *rtv_heap, *dsv_heap;
    UINT srv_inc, sampler_inc, rtv_inc, dsv_inc; int attribs_dirty_in_list;
    /* guest memory */
    ID3D12Heap *region_heap[BOOT_RENDER_MAX_REGIONS]; ID3D12Resource *region[BOOT_RENDER_MAX_REGIONS];
    uint8_t *region_cpu[BOOT_RENDER_MAX_REGIONS]; uint64_t region_size[BOOT_RENDER_MAX_REGIONS]; unsigned region_count;
    uint8_t *arena_cpu; uint64_t arena_size;   /* region 0, linearly mapped from va_base (tests) */
    ID3D12Resource *ptable, *attribs, *host_state; uint32_t *ptable_cpu; void *attribs_cpu; uint32_t *host_state_cpu;
    uint32_t ptable_pages, va_base_page;
    ID3D12Resource *ring; uint8_t *ring_cpu; uint64_t ring_size, ring_used; uint32_t cbv_used;
    /* caches */
    BootRenderShader shaders[BOOT_RENDER_MAX_SHADERS]; unsigned shader_count;
    BootRenderTarget rts[BOOT_RENDER_MAX_RT]; unsigned rt_count;
    BootRenderPso psos[BOOT_RENDER_MAX_PSO]; unsigned pso_count;
    char manifest_path[512];
    ID3D12CommandAllocator *alloc; ID3D12GraphicsCommandList *cl; int recording;
    const BootRenderWrite *overlay; uint32_t overlay_count;   /* applied to constant-buffer snapshots */
    char error[256];
} BootRender;

static inline int boot_render_fail(BootRender *r, int code, const char *what) {
    snprintf(r->error, sizeof r->error, "%s", what); return code;
}

/* ---------------------------------------------------------------- suspect-diag (play-work/suspect-diag, 2026-10-21)
 * TOTK_SUSPECT_DIAG=1 (=2: suspect 1 hashes only ranges of draws whose vertex shader is a 'material' shader; unset or 0: off,
 * every hook is one cached branch and nothing else runs or changes). "Detect, don't bisect": one play session counts, for
 * each suspect of the sky-island vertex explosion that came back in play-99..102, the events that suspect would have to
 * cause, so the first skydive names the change instead of a bisect:
 *  S1 re-record versioning (play-98, TOTK_RERECORD_VERSION): guest threads run during the submit call. The CPU-side reads of
 *     guest memory of the call's translation (constant-buffer snapshot copies, 8-bit / quad index expansion, guest texture
 *     builds) and the ranges the GPU reads later in place (direct CBVs, the first 4 KiB of each vertex stream of the draw)
 *     are hashed when read; at the end of the call (under the renderer lock) the same bytes are hashed again: a range that
 *     changed was written by a guest thread while the call ran. Calls that kept a re-recorded version (threads released
 *     without the wait) and calls that did not are counted apart. Budget: TOTK_SUSPECT_DIAG_MB (default 4) MiB hashed per kind
 *     per call (beyond it ranges are counted as unsampled).
 *  S2 K2 (play-97 batch 4, TOTK_INSTANCE_ID_ZERO_BASED): draws where the rule changes the result: base instance != 0 and the
 *     vertex shader (translated with K2) reads InstanceId, or has a per-instance attribute with divisor > 1 or frequency 0
 *     (from the shader's .hlsl next to its DXIL, read once per vertex shader).
 *  S3 gpu-resident span views (play-101, TOTK_GPU_RESIDENT): every TOTK_SUSPECT_DIAG_GR_EVERY (30) presents, sampled pages
 *     of the GPU mappings (within ~0.4 ms): the page-table entry (CPU table and the GPU's copy) against the mapping's
 *     region and offset, and the bytes behind the entry against the canonical host view of the guest memory; plus a page of
 *     a few open span views against the section's host view.
 *  S4 live texture trim (play-99, TOTK_TEX_LIVE_TRIM / TOTK_TEX_MIN_AGE): evictions of texture slots a vertex shader bound
 *     in the last TOTK_SUSPECT_DIAG_TEX_N (600) presents, and rebuilds of such slots (same guest address: the same image).
 * Output: frames\suspect-diag.log (previous run: suspect-diag.prev.log): a header with the switches, one line per suspect
 * every 300 presents (also written to draw-path.log), and the first TOTK_SUSPECT_DIAG_EVENTS (40) events of each suspect (S1: of each kind)
 * with the frame number (frame-N.bmp / frames\glitch.log "GLITCH present N" numbering), submission, draw, vertex shader id
 * (first 8 bytes, as in shader-manifest.tsv), its family from startup\shader-labels.tsv and the draw-census key. With the
 * switch on, every draw-path.log line also ends in " [frame N]". */
enum { BOOT_SD_K_CB, BOOT_SD_K_DIRECT, BOOT_SD_K_INDEX, BOOT_SD_K_TEX, BOOT_SD_K_VERTEX,
       BOOT_SD_K_CBSNAP, BOOT_SD_K_VSNAP, BOOT_SD_K_IZC, BOOT_SD_K_ISNAP,   /* data-snapshot: its copies (hash of the copy, re-hash of the
                                                                         * guest range: changed = written after the capture, harmless) and the
                                                                         * zero-copy index buffers the GPU reads later */
       BOOT_SD_K_N };
static int boot_sd_mode = -1;   /* -1 not read yet, 0 off, 1 on, 2 on with suspect 1 sampled on 'material' draws */
static inline __attribute__((noinline)) void boot_sd_mode_init(void) {
    char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_SUSPECT_DIAG", v, sizeof v);
    boot_sd_mode = (k && k < sizeof v && v[0] >= '1' && v[0] <= '9') ? (v[0] == '2' ? 2 : 1) : 0;
}
static inline int boot_sd_on(void) {
    if (__builtin_expect(boot_sd_mode < 0, 0)) boot_sd_mode_init();
    return __builtin_expect(boot_sd_mode > 0, 0);
}
/* 64-bit hash of n bytes in four lanes (a changed word always changes its lane; ~memory speed). */
static inline uint64_t boot_sd_hash(const uint8_t *p, uint64_t n) {
    const uint64_t P = UINT64_C(0x100000001b3) | (UINT64_C(0x9e3779b9) << 40);
    uint64_t a = n ^ UINT64_C(0x243f6a8885a308d3), b = UINT64_C(0x13198a2e03707344), c = UINT64_C(0xa4093822299f31d0), d = UINT64_C(0x082efa98ec4e6c89), i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4]; memcpy(w, p + i, 32);
        a = (a ^ w[0]) * P; b = (b ^ w[1]) * P; c = (c ^ w[2]) * P; d = (d ^ w[3]) * P;
    }
    for (; i < n; ++i) a = (a ^ p[i]) * P;
    uint64_t h = a ^ ((b << 17) | (b >> 47)) ^ ((c << 31) | (c >> 33)) ^ ((d << 47) | (d >> 17));
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd); h ^= h >> 33;
    return h;
}
#define BOOT_SD_RECS 32768u
#define BOOT_SD_SEEN 65536u
#define BOOT_SD_LABELS 65536u
#define BOOT_SD_VS 16384u
#define BOOT_SD_TIC 65536u
/* One range read during the call: guest GPU VA, the region it was read through (index, host base, byte offset, the span
 * view's section offset and generation when the region is a view), its hash, and the draw it belongs to. */
typedef struct BootSdRec { const uint8_t *base; uint64_t va, off, hash, vs8, key, lo, gen, epoch, big; uint32_t n, reg, kind, draw, exec, frame; } BootSdRec;
typedef struct BootSdLabel { uint64_t key; const char *family; } BootSdLabel;
typedef struct BootSdVs { uint64_t key; uint32_t attr_mask, flags; } BootSdVs;   /* flags: 1 scanned, 2 reads InstanceId, 4 K2 prologue, 8 no .hlsl */
typedef struct BootSdCount {
    /* S1 */
    uint64_t calls, calls_kept, kept, rerecords, checked[BOOT_SD_K_N], ch_kept[BOOT_SD_K_N], ch_plain[BOOT_SD_K_N], gone[BOOT_SD_K_N],
             unsampled[BOOT_SD_K_N], dup[BOOT_SD_K_N], bytes[BOOT_SD_K_N], hostw[BOOT_SD_K_N], draws, draws_copy, draws_mat, draws_mat_copy, draws_direct, recheck_qpc;
    /* S2 */
    uint64_t k2_draws, k2_base, k2_indirect, k2_changed, k2_changed_mat, k2_iid, k2_div, k2_freq0, k2_pre, k2_unknown;
    /* S3 */
    uint64_t gr_checks, gr_pages, gr_bytes, gr_views, gr_mismatch, gr_ptcopy, gr_ptother, gr_ptother_eq, gr_unmapped, gr_unreadable, gr_nosec, gr_qpc, gr_maxq;
    /* S4 */
    uint64_t tx_evict, tx_evict_live, tx_evict_vs, tx_builds, tx_rebuild_vs, tx_rebuild_vs_ev, tx_reuse_vs;
} BootSdCount;
typedef struct BootSdState {
    FILE *log; int dirty;
    uint64_t frame, presents, budget, qpf;   /* frame: frame-N.bmp / glitch.log number of the frame being built */
    uint32_t events_max, tex_n, gr_every, gr_pages_max;
    unsigned ev_s1[BOOT_SD_K_N], ev_s2, ev_s3, ev_s4;   /* events written (S1: per kind) */
    /* draw in flight */
    int in_draw, material; uint64_t vs8, key, exec_last; uint32_t draw, draw_copies, draw_direct; const char *fam;
    /* S1 records of the running call */
    BootSdRec *rec; uint32_t nrec, stamp; uint64_t call_bytes, kind_bytes[BOOT_SD_K_N]; uint64_t *seen; uint32_t *seen_stamp;
    uint64_t last_kept, last_rerecords;
    /* S2: shader families and vertex shader facts */
    char *labels_text; BootSdLabel *labels; uint32_t nlabels; BootSdVs *vs;
    /* S3 */
    int gr_due; uint32_t gr_cursor, gr_vcursor;
    /* S4: per TIC slot, frame + 1 of the last vertex-shader binding (0: never), the slot's guest address then, evicted since */
    uint32_t *vs_tex; uint64_t *vs_tex_va; uint8_t *evicted;
    BootSdCount c, l;   /* totals and the totals at the last summary */
} BootSdState;
static BootSdState *boot_sd_st;
static inline BootSdState *boot_sd_state(void);   /* below (with the gpu-resident view table) */
static inline void boot_sd_note(BootRender *r, unsigned kind, uint64_t va, const uint8_t *p, const uint8_t *hp, uint64_t n);
static inline void boot_sd_tex_build(BootRender *r, uint32_t index, uint64_t va, const uint8_t *src, uint64_t n, uint64_t sum);
static inline void boot_sd_tex_evict(uint32_t index, int live);
static inline void boot_sd_vs_scan(const uint8_t id[32], const char *dxil_path);
/* ---------------------------------------------------------------- data-snapshot (play-work/data-snapshot, 2026-10-21)
 * TOTK_DATA_SNAPSHOT (default 1; 0 = the zero-copy behaviour of play-103 exactly). The design note and the code are before
 * host_gpu_render_draw.h at the end of this header; here what the code above that point calls. */
static int boot_ds_mode_v = -1;   /* -1 not read yet, 0 off, 1 learned ranges (default), 2 every range the CPU may write */
static inline __attribute__((noinline)) void boot_ds_mode_init(void);
static inline int boot_ds_mode(void) {
    if (__builtin_expect(boot_ds_mode_v < 0, 0)) boot_ds_mode_init();
    return boot_ds_mode_v;
}
static inline int boot_ds_on(void) { return __builtin_expect(boot_ds_mode() > 0, 1); }
static int boot_ds_cb_now;              /* 1 while boot_render_cbv_ex copies a binding the snapshot took (S1 kind cb-snap) */
static uint8_t boot_ds_cbt[2][18];      /* the draw in flight: constant-buffer slots (vertex, pixel) read from a copy */
static uint32_t boot_ds_vmask;          /* the draw in flight: vertex streams read from a copy ... */
static uint8_t *boot_ds_vcopy_cpu[16];  /* ... and the copy's host bytes (S1 vertex-snap hashes what the GPU reads) */
static uint64_t boot_ds_bind_gen;       /* v2fix: moves whenever the native executor rebuilds a draw's constant-buffer rows ... */
static const void *boot_ds_bind_of;     /* ... in this table (host_nvn_exec.h boot_nvn_st_buffers): the plan then compares no rows */
static inline int boot_ds_fill_wanted(uint64_t va, uint64_t n);
static inline void boot_ds_fill(BootRender *r, uint8_t *dst, uint64_t va, const uint8_t *live, uint64_t n);
static inline void boot_ds_idle(void);
static inline void boot_ds_s1_changed(BootRender *r, uint64_t va, uint64_t n, unsigned kind);
/* ---------------------------------------------------------------- async-xlate (play-work/async-xlate-work: stage1/README.md, stage2/README.md)
 * The translation thread X, the FIFO, the capture and the virtual fences are host_nvn_async.h; here what the renderer reads and marks.
 * The capture: at the submit call t2 copies, through its own context and a snapshot of the GPU mappings (never renderer state), every
 * uniform-buffer range the submission can bind, at exactly the size boot_render_cbv_ex copies (boot_render_cb_view_bytes), into an arena
 * and a guest GPU VA -> (offset, length, flags) table. While X translates that submission boot_ax_cap_view points at it, and
 * boot_render_cbv_ex takes a binding data-snapshot takes (boot_ds_cb_now) from it (boot_ax_cb_src), unless GPU work not yet idle at the
 * capture may write the range (flagged: GPU order decides, the bytes are read when X records the draw, as before) or the page table, the
 * big-write count or a host write moved since the capture. NULL whenever TOTK_ASYNC_XLATE is unset/0 (one not-taken branch per copy). */
#define BOOT_AX_KIND_CB 0u   /* = BOOT_NVN_AXR_CB (host_nvn.h; static-asserted in host_nvn_async.h) */
enum { BOOT_AX_CAPF_GPU = 1,      /* GPU work not yet idle at the capture may write it: GPU order decides (no bytes kept) */
       BOOT_AX_CAPF_ZERO = 2,     /* some pages had no CPU backing at the capture: zero there, as boot_render_cbv_ex's page path */
       BOOT_AX_CAPF_UNMAPPED = 4, /* not copied: its first page had no CPU backing (cbv_ex fails such a binding) */
       BOOT_AX_CAPF_OVER = 8 };   /* not copied: the arena or the table was full */
typedef struct BootAxCap {
    uint8_t *arena; uint64_t used, cap, committed;   /* reserved once (cap bytes), committed on demand in 1 MiB steps */
    uint64_t *key; uint32_t *off, *len, hcap, n; uint8_t *flag;   /* open addressing by guest GPU VA (0 = empty), at most half full */
    uint64_t map_gen, big, epoch;                    /* boot_render_map_gen, boot_render_big_writes, boot_render_cb_epoch at the capture */
} BootAxCap;
static const BootAxCap *boot_ax_cap_view;
/* constant-buffer copies on X by source: from the capture; not captured (no entry: no summary or inherited binding reached it; an entry
 * shorter than the copy; its first page unmapped at the capture; over budget); captured but stale; GPU-ordered at the capture; not taken by
 * data-snapshot (GPU-only pools, pending GPU writes, budget, compute); program constants (shader-code class, taken only with
 * TOTK_DATA_SNAPSHOT_CB=2: shader code is live by design) */
enum { BOOT_AX_CB_HIT, BOOT_AX_CB_MISS, BOOT_AX_CB_SHORT, BOOT_AX_CB_UNMAPPED, BOOT_AX_CB_OVER, BOOT_AX_CB_STALE, BOOT_AX_CB_GPU, BOOT_AX_CB_NOT_TAKEN, BOOT_AX_CB_CODE, BOOT_AX_CB_N };
static uint64_t boot_ax_cb_n[BOOT_AX_CB_N], boot_ax_cb_bytes[BOOT_AX_CB_N];
static inline uint32_t boot_ax_cap_slot(uint64_t va, uint32_t hcap) { va ^= va >> 29; va *= UINT64_C(0xbf58476d1ce4e5b9); va ^= va >> 32; return (uint32_t)va & (hcap - 1); }
/* The entry of start address va, UINT32_MAX if none. */
static inline uint32_t boot_ax_cap_at(const BootAxCap *c, uint64_t va) {
    if (!c || !c->hcap || !va) return UINT32_MAX;
    for (uint32_t m = c->hcap - 1, j = boot_ax_cap_slot(va, c->hcap), k = 0; k <= m && c->key[j]; j = (j + 1) & m, ++k)
        if (c->key[j] == va) return j;
    return UINT32_MAX;
}
/* The captured bytes of [va, va + n) (an entry starting at va with at least n bytes), NULL if none. */
static inline const uint8_t *boot_ax_cap_find(const BootAxCap *c, uint64_t va, uint64_t n) {
    const uint32_t j = boot_ax_cap_at(c, va);
    return j != UINT32_MAX && c->len[j] && n <= c->len[j] ? c->arena + c->off[j] : NULL;
}
/* n bytes of the arena (16-byte aligned), committing it in 1 MiB steps; NULL when beyond the reservation or out of memory. */
static inline uint8_t *boot_ax_arena_take(BootAxCap *c, uint64_t n) {
    const uint64_t at = (c->used + 15) & ~UINT64_C(15);
    if (!c->arena || at > c->cap || n > c->cap - at) return NULL;
    if (at + n > c->committed) {
        uint64_t to = (at + n + (UINT64_C(1) << 20) - 1) & ~((UINT64_C(1) << 20) - 1); if (to > c->cap) to = c->cap;
        if (!VirtualAlloc(c->arena + c->committed, (SIZE_T)(to - c->committed), MEM_COMMIT, PAGE_READWRITE)) return NULL;
        c->committed = to;
    }
    c->used = at + n; return c->arena + at;
}
/* Empties the capture and sizes its table for `ranges` ranges; the arena (cap_bytes) is reserved at the first use. 0: no memory. */
static inline int boot_ax_cap_begin(BootAxCap *c, uint32_t ranges, uint64_t cap_bytes) {
    c->used = 0; c->n = 0;
    if (!c->arena) { c->arena = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)cap_bytes, MEM_RESERVE, PAGE_NOACCESS); c->cap = c->arena ? cap_bytes : 0; c->committed = 0; }
    uint32_t h = 1024; while (h < 2u * ranges + 2 && h < (1u << 24)) h *= 2;
    if (h > c->hcap) {
        uint64_t *k = (uint64_t *)realloc(c->key, (size_t)h * 8); if (k) c->key = k;
        uint32_t *o = k ? (uint32_t *)realloc(c->off, (size_t)h * 4) : NULL; if (o) c->off = o;
        uint32_t *l = o ? (uint32_t *)realloc(c->len, (size_t)h * 4) : NULL; if (l) c->len = l;
        uint8_t *g = l ? (uint8_t *)realloc(c->flag, h) : NULL; if (g) c->flag = g;
        if (!g) { c->hcap = 0; return 0; }
        c->hcap = h;
    }
    memset(c->key, 0, (size_t)c->hcap * 8);
    return c->arena != NULL;
}
/* An entry for start address va: one per start (a longer range replaces a shorter one; a GPU-ordered entry stays so). n > 0: returns the
 * n bytes to fill (NULL when the arena is full: the entry then notes OVER if it holds nothing yet); n == 0: notes `flags` only (GPU-ordered,
 * unmapped). *slot: the entry (when the table had room). */
static inline uint8_t *boot_ax_cap_put(BootAxCap *c, uint64_t va, uint64_t n, unsigned flags, uint32_t *slot) {
    if (!va || n > 0xffffffffu || !c->hcap) return NULL;
    uint32_t m = c->hcap - 1, j = boot_ax_cap_slot(va, c->hcap);
    for (uint32_t k = 0; k <= m && c->key[j] && c->key[j] != va; ++k) j = (j + 1) & m;
    if (c->key[j] != va) {
        if (c->key[j] || 2 * (c->n + 1) > c->hcap) return NULL;   /* table full: no entry (counted as not captured) */
        c->key[j] = va; ++c->n; c->off[j] = 0; c->len[j] = 0; c->flag[j] = 0;
    }
    if (slot) *slot = j;
    if (c->flag[j] & BOOT_AX_CAPF_GPU) return NULL;
    if (!n || (flags & BOOT_AX_CAPF_GPU)) { c->len[j] = 0; c->flag[j] = (uint8_t)flags; return NULL; }
    if (c->len[j] >= n) return NULL;
    uint8_t *dst = boot_ax_arena_take(c, n);
    if (!dst) { if (!c->len[j]) c->flag[j] = BOOT_AX_CAPF_OVER; return NULL; }
    c->off[j] = (uint32_t)(dst - c->arena); c->len[j] = (uint32_t)n; c->flag[j] = (uint8_t)flags;
    return dst;
}
/* stage 2: the call-start chunks of the submission X translates (in its capture's arena, sorted by chunk number): boot_ds_capture fills
 * the shadow through boot_ax_dyn_page instead of guest memory while it is set. */
typedef struct BootAxDyn { uint64_t *chunk; uint32_t *off, *mask, n, cap; const uint8_t *arena; } BootAxDyn;
static const BootAxDyn *boot_ax_dyn_view;
static inline const uint8_t *boot_ax_dyn_page(void *ctx, uint64_t va) {   /* BootDsPageFn: the captured 4 KiB page at va, NULL if not captured */
    const BootAxDyn *d = (const BootAxDyn *)ctx; const uint64_t ch = va >> 16; uint32_t lo = 0, hi = d->n;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (d->chunk[mid] < ch) lo = mid + 1; else hi = mid; }
    if (lo == d->n || d->chunk[lo] != ch) return NULL;
    const unsigned p = (unsigned)((va >> 12) & 15);
    return (d->mask[lo] >> p & 1u) ? d->arena + d->off[lo] + (size_t)p * 4096 : NULL;
}
/* The sentinel's log (TOTK_ASYNC_XLATE_CHECK=1, host_nvn_async.h): for a sampled submission X logs the records it reads and the
 * constant-buffer bytes it copies, and t2 replays the log after the join. NULL otherwise. */
enum { BOOT_AX_CHK_REC = 1, BOOT_AX_CHK_CB = 2 };
typedef struct BootAxChkEnt { uint64_t at, h, va; uint32_t n, kind; } BootAxChkEnt;   /* kind: type << 24 | source << 16 | op */
typedef struct BootAxChkLog { BootAxChkEnt *e; uint32_t n, cap; uint64_t lost; } BootAxChkLog;
static BootAxChkLog *boot_ax_chk_log;
#define BOOT_AX_H0 UINT64_C(0xcbf29ce484222325)
static inline uint64_t boot_ax_hash(const void *p, uint64_t n, uint64_t h) {   /* 64-bit, 8 bytes a step (not cryptographic) */
    const uint8_t *b = (const uint8_t *)p;
    for (; n >= 8; b += 8, n -= 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * UINT64_C(0x100000001b3); h ^= h >> 29; }
    for (; n; ++b, --n) h = (h ^ *b) * UINT64_C(0x100000001b3);
    return h ^ (h >> 32);
}
/* X, boot_render_cbv_ex: a constant-buffer copy (src 1 from the capture, 2 taken by data-snapshot but not from the capture, 3 not taken). */
static inline void boot_ax_chk_cb(uint64_t va, uint64_t n, const uint8_t *bytes, unsigned src) {
    BootAxChkLog *L = boot_ax_chk_log;
    if (!L) return;
    if (L->n >= L->cap) { ++L->lost; return; }
    BootAxChkEnt *e = &L->e[L->n++]; e->at = 0; e->va = va; e->n = (uint32_t)n; e->kind = (uint32_t)BOOT_AX_CHK_CB << 24 | (src & 0xffu) << 16;
    e->h = boot_ax_hash(bytes, n, BOOT_AX_H0);
}
/* What X shares with the renderer code. The registry epoch moves with every observed registration, program and texel write
 * (host_nvn_async.h boot_ax_reg_note, before the call's own effect); a submission notes it at its capture. While X translates a stage-2
 * submission boot_ax_x_epoch1 is that epoch + 1 (0 everywhere else): registry lookups then see the entries as they were at the capture
 * (boot_ax_hist, host_nvn.h boot_nvn_prog_hist) and the texel queue applies only the writes before it (boot_ax_texel_cut1). boot_ax_x_item:
 * the FIFO item X runs (0 idle); boot_ax_idle_done: every item up to it completed on the GPU with its deferred stores applied (a queue
 * idle marks it, boot_render_idle_cleanup): the capture's GPU-order test drops the write ranges of those. */
static volatile LONG64 boot_ax_reg_epoch, boot_ax_x_item, boot_ax_idle_done, boot_ax_fifo_done_seq;
static _Thread_local int boot_ax_on_x;   /* 1 on X while it translates a captured submission */
static _Thread_local uint64_t boot_ax_x_epoch1, boot_ax_texel_cut1;
static int boot_ax_hist_on;              /* TOTK_ASYNC_XLATE >= 2 at boot (host_nvn.h registration): registrations keep the entries they replace */
static uint64_t boot_ax_texel_held;      /* texel writes left for a later submission (written after the capture of the one X translated) */
static inline void boot_ax_idle_mark(void) {
    const uint64_t cur = (uint64_t)__atomic_load_n(&boot_ax_x_item, __ATOMIC_ACQUIRE);
    const uint64_t d = cur ? cur - 1 : (uint64_t)__atomic_load_n(&boot_ax_fifo_done_seq, __ATOMIC_ACQUIRE);
    for (LONG64 o = __atomic_load_n(&boot_ax_idle_done, __ATOMIC_ACQUIRE); (uint64_t)o < d && InterlockedCompareExchange64(&boot_ax_idle_done, (LONG64)d, o) != o;
         o = __atomic_load_n(&boot_ax_idle_done, __ATOMIC_ACQUIRE)) {}
}
/* Residual live guest reads X makes, by class (counted only on X while it translates; the async-xlate line prints them). */
enum { BOOT_AX_L_DBIND, BOOT_AX_L_OBJECT, BOOT_AX_L_POOLWORD, BOOT_AX_L_CODE, BOOT_AX_L_DIAG, BOOT_AX_L_SHADER, BOOT_AX_L_TEXEL, BOOT_AX_L_REHASH, BOOT_AX_L_N };
static uint64_t boot_ax_live_n[BOOT_AX_L_N], boot_ax_live_bytes[BOOT_AX_L_N];
static inline void boot_ax_live(unsigned cls, uint64_t n) { if (boot_ax_on_x && cls < BOOT_AX_L_N) { ++boot_ax_live_n[cls]; boot_ax_live_bytes[cls] += n; } }
static inline int boot_ds_code_range(uint64_t va, uint64_t n);   /* the range lies in a shader-code pool (data-snapshot classes, X) */

/* ---------------------------------------------------------------- persistent pipeline library (play (73))
 * Pipelines are compiled by the driver the first time they are created (hundreds of ms for a large pixel
 * shader: the "pso" phase of the hitch lines in draw-path.log, 0.4-2.5 s for a handful of new pipelines).
 * An ID3D12PipelineLibrary keeps the compiled pipelines across runs: every pipeline the draw path, the
 * background pre-creation thread (pso-keys.bin), the compute dispatcher and the render-target helpers create
 * is looked up by a 128-bit name (LoadGraphicsPipeline / LoadComputePipeline) and, when the library does not
 * hold it, created as before and stored (StorePipeline). The library is serialized to
 * <manifest directory>\pso-cache-<vendor>-<device>-<subsystem>-r<revision>.bin by a background thread once
 * enough new entries exist (and at most every BOOT_PSOC_SAVE_GAP_MS), after a present, and when the window
 * closes. The file is dropped (the library starts empty) when its header does not match: another adapter,
 * driver version (UMD version from DXGI, and the runtime's own check inside CreatePipelineLibrary), a changed
 * shader manifest (content hash), another format version, or a failed checksum.
 * The name hashes everything a pipeline depends on that the draw code decides: the pipeline key (shader ids
 * and every fixed-function field), the render target count, the bytes of the vertex/pixel/compute shaders and
 * the serialized root signature (registered with boot_psoc_root_note where it is created). A pipeline whose
 * root signature is not registered is not cached. The runtime additionally compares the full description on
 * Load, so a name collision or a stale entry can only cost a normal compile, never a wrong pipeline.
 * TOTK_PSO_CACHE=0 turns it all off. Device loss: boot_psoc_device_lost releases the library (it holds the
 * removed device); the next render object opens a new one for the replacement device from the same file.
 * Locking: one SRWLOCK around every library call (Load/Store are short; Serialize copies the library once per
 * save, on the saver thread). The reference recomps (LASRecomp, SuperMarioWonderRecomp) persist Vulkan pipelines
 * with a VkPipelineCache blob validated by the driver's pipeline-cache header (vendor, device, driver version,
 * cache UUID); the same checks are made here from the DXGI adapter description, the UMD version and the
 * runtime's own library validation. */
#include <stdarg.h>
#define BOOT_PSOC_MAGIC 0x434f5350u
#define BOOT_PSOC_VERSION 1u
#define BOOT_PSOC_SAVE_ENTRIES 8      /* new entries that make a save worthwhile */
#define BOOT_PSOC_SAVE_GAP_MS 15000   /* never save more often than this */
#define BOOT_PSOC_TAIL_MS 90000       /* fewer new entries are saved once the last save is this old */
typedef struct BootPsocHeader {
    uint32_t magic, version, vendor, device, subsys, revision;
    uint64_t driver, manifest_hash, blob_size, blob_hash, reserved;
} BootPsocHeader;   /* 64 bytes: the blob that follows stays 16-byte aligned in the malloc'd file image */
static struct {
    SRWLOCK lock; ID3D12PipelineLibrary *lib; ID3D12Device *dev; void *blob;   /* blob: file image the library was created from (it must outlive the library) */
    int state;                       /* 0 closed, 1 open, -1 off (TOTK_PSO_CACHE=0 or no ID3D12Device1) */
    char path[600]; BootPsocHeader id;
    volatile LONG dirty, saving;     /* entries stored since the last save; 1 while a save runs (or is starting) */
    ULONGLONG last_save_ms;
    volatile LONG loaded, load_miss, stored, store_failed, saves;
    SRWLOCK root_lock; const void *roots[8]; uint64_t root_hash[8]; unsigned root_count, root_next;
} boot_psoc;
static inline void boot_psoc_log(const char *fmt, ...) {
    FILE *lf = fopen("draw-path.log", "a"); if (!lf) return;
    va_list ap; va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap); fputc('\n', lf); fclose(lf);
}
static inline uint64_t boot_psoc_hash(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p; h ^= (uint64_t)n * UINT64_C(0x9e3779b97f4a7c15);
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * UINT64_C(0xff51afd7ed558ccd); h ^= h >> 32; }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); h = (h ^ w) * UINT64_C(0xc4ceb9fe1a85ec53); h ^= h >> 29; }
    return h;
}
/* two independent 64-bit chains = the 128-bit pipeline name */
static inline void boot_psoc_chain(uint64_t h[2], const void *p, size_t n) { h[0] = boot_psoc_hash(p, n, h[0]); h[1] = boot_psoc_hash(p, n, h[1]); }
static inline void boot_psoc_name(WCHAR out[33], const uint64_t h[2]) {
    static const char hx[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) { out[i] = (WCHAR)hx[(h[0] >> (60 - 4 * i)) & 15]; out[16 + i] = (WCHAR)hx[(h[1] >> (60 - 4 * i)) & 15]; }
    out[32] = 0;
}
static inline int boot_psoc_enabled(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_PSO_CACHE", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
/* The serialized root signature, hashed where it is created (the pipelines it serves are named with it). */
static inline void boot_psoc_root_note(const void *root, const void *blob, size_t n) {
    uint64_t h = boot_psoc_hash(blob, n, UINT64_C(0x746f6f72)); unsigned i;
    AcquireSRWLockExclusive(&boot_psoc.root_lock);
    for (i = 0; i < boot_psoc.root_count; ++i) if (boot_psoc.roots[i] == root) break;
    if (i == boot_psoc.root_count) { if (boot_psoc.root_count < 8) i = boot_psoc.root_count++; else i = boot_psoc.root_next++ & 7; }
    boot_psoc.roots[i] = root; boot_psoc.root_hash[i] = h;
    ReleaseSRWLockExclusive(&boot_psoc.root_lock);
}
static inline int boot_psoc_root_hash(const void *root, uint64_t *h) {
    int found = 0;
    AcquireSRWLockShared(&boot_psoc.root_lock);
    for (unsigned i = 0; i < boot_psoc.root_count; ++i) if (boot_psoc.roots[i] == root) { *h = boot_psoc.root_hash[i]; found = 1; break; }
    ReleaseSRWLockShared(&boot_psoc.root_lock);
    return found;
}
static inline uint8_t *boot_psoc_read_all(const char *path, size_t *n) {
    *n = 0; FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t *data = NULL; long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len > 0 && len < 0x60000000L && fseek(f, 0, SEEK_SET) == 0 && (data = (uint8_t *)malloc((size_t)len)) != NULL && fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); data = NULL; }
    fclose(f); if (data) *n = (size_t)len;
    return data;
}
/* User-mode driver version of the adapter (the value the runtime ties a library blob to); 0 when DXGI does not say. */
static inline uint64_t boot_psoc_driver_version(LUID luid) {
    uint64_t v = 0; IDXGIFactory4 *f = NULL; IDXGIAdapter1 *a = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&f)) && f) {
        if (SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(f, luid, &IID_IDXGIAdapter1, (void **)&a)) && a) {
            LARGE_INTEGER u; u.QuadPart = 0;
            if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(a, &IID_IDXGIDevice, &u))) v = (uint64_t)u.QuadPart;
            IDXGIAdapter1_Release(a);
        }
        IDXGIFactory4_Release(f);
    }
    return v;
}
/* Releases the library (device loss, another device). Entries not saved yet are lost; the file on disk stays valid. */
static inline void boot_psoc_device_lost(void) {
    AcquireSRWLockExclusive(&boot_psoc.lock);
    if (boot_psoc.lib) ID3D12PipelineLibrary_Release(boot_psoc.lib);
    free(boot_psoc.blob);
    boot_psoc.lib = NULL; boot_psoc.blob = NULL; boot_psoc.dev = NULL; boot_psoc.dirty = 0;
    if (boot_psoc.state > 0) boot_psoc.state = 0;
    ReleaseSRWLockExclusive(&boot_psoc.lock);
}
/* Opens the library of r's device (once per device); 1 when pipelines can be loaded and stored. */
static inline int boot_psoc_open(const BootRender *r) {
    ID3D12Device *dev = r->gpu->device;
    if (boot_psoc.state < 0 || (boot_psoc.state > 0 && boot_psoc.dev == dev)) return boot_psoc.state > 0;
    if (boot_psoc.state > 0) boot_psoc_device_lost();   /* a library of another device */
    if (!boot_psoc_enabled()) { boot_psoc.state = -1; return 0; }
    ID3D12Device1 *d1 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device1, (void **)&d1)) || !d1) {
        boot_psoc.state = -1; boot_psoc_log("pso cache: off (no ID3D12Device1)"); return 0; }
    const DXGI_ADAPTER_DESC1 *ad = &r->gpu->adapter; BootPsocHeader id; memset(&id, 0, sizeof id);
    id.magic = BOOT_PSOC_MAGIC; id.version = BOOT_PSOC_VERSION; id.vendor = ad->VendorId; id.device = ad->DeviceId; id.subsys = ad->SubSysId; id.revision = ad->Revision;
    id.driver = boot_psoc_driver_version(ad->AdapterLuid);
    char mfile[sizeof r->manifest_path + 16]; snprintf(mfile, sizeof mfile, "%s", r->manifest_path);
    if (GetFileAttributesA(mfile) == INVALID_FILE_ATTRIBUTES && !strcmp(mfile, "shader-manifest.tsv")) snprintf(mfile, sizeof mfile, "startup\\shader-manifest.tsv");
    size_t mn = 0; uint8_t *md = boot_psoc_read_all(mfile, &mn);
    id.manifest_hash = md ? boot_psoc_hash(md, mn, UINT64_C(0x6d616e69)) : 0; free(md);
    char dir[sizeof mfile]; snprintf(dir, sizeof dir, "%s", mfile);
    char *sep = strrchr(dir, '\\'); char *sep2 = strrchr(dir, '/'); if (sep2 > sep) sep = sep2;
    if (sep) sep[1] = 0; else dir[0] = 0;
    char path[sizeof boot_psoc.path]; snprintf(path, sizeof path, "%spso-cache-%04x-%04x-%08x-r%02x.bin", dir, id.vendor, id.device, id.subsys, id.revision);
    size_t fn = 0; uint8_t *file = boot_psoc_read_all(path, &fn);
    const char *why = NULL; ID3D12PipelineLibrary *lib = NULL; void *keep = NULL; BootPsocHeader h; memset(&h, 0, sizeof h);
    if (!file) why = "no cache file";
    else if (fn < sizeof h) why = "short file";
    else {
        memcpy(&h, file, sizeof h);
        if (h.magic != id.magic || h.version != id.version) why = "other format";
        else if (h.vendor != id.vendor || h.device != id.device || h.subsys != id.subsys || h.revision != id.revision) why = "other adapter";
        else if (h.driver != id.driver) why = "driver version changed";
        else if (h.manifest_hash != id.manifest_hash) why = "shader manifest changed";
        else if (h.blob_size != fn - sizeof h) why = "size mismatch";
        else if (boot_psoc_hash(file + sizeof h, (size_t)h.blob_size, UINT64_C(0x626c6f62)) != h.blob_hash) why = "checksum mismatch";
        else {
            HRESULT hr = ID3D12Device1_CreatePipelineLibrary(d1, file + sizeof h, (SIZE_T)h.blob_size, &IID_ID3D12PipelineLibrary, (void **)&lib);
            if (FAILED(hr) || !lib) {   /* D3D12_ERROR_DRIVER_VERSION_MISMATCH / ADAPTER_NOT_FOUND / corrupt */
                lib = NULL; why = "rejected by the runtime"; boot_psoc_log("pso cache: runtime rejected %s (hr=0x%08lx)", path, (unsigned long)hr); }
            else keep = file;
        }
    }
    if (!lib) {
        HRESULT hr = ID3D12Device1_CreatePipelineLibrary(d1, NULL, 0, &IID_ID3D12PipelineLibrary, (void **)&lib);
        if (FAILED(hr) || !lib) { ID3D12Device1_Release(d1); free(file); boot_psoc.state = -1; boot_psoc_log("pso cache: off (CreatePipelineLibrary hr=0x%08lx)", (unsigned long)hr); return 0; }
    }
    ID3D12Device1_Release(d1);
    if (!keep) free(file);
    AcquireSRWLockExclusive(&boot_psoc.lock);
    boot_psoc.lib = lib; boot_psoc.dev = dev; boot_psoc.blob = keep; boot_psoc.id = id; boot_psoc.dirty = 0;
    snprintf(boot_psoc.path, sizeof boot_psoc.path, "%s", path); boot_psoc.last_save_ms = GetTickCount64(); boot_psoc.state = 1;
    ReleaseSRWLockExclusive(&boot_psoc.lock);
    if (keep) boot_psoc_log("pso cache: %s loaded, %llu KiB (driver %llx)", path, (unsigned long long)(h.blob_size >> 10), (unsigned long long)id.driver);
    else boot_psoc_log("pso cache: %s starts empty (%s)", path, why ? why : "?");
    return 1;
}
/* Serializes the library to the cache file. The caller owns boot_psoc.saving. */
static inline void boot_psoc_save_body(int force) {
    if (boot_psoc.state <= 0 || (!force && boot_psoc.dirty < 1)) return;
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    uint8_t *buf = NULL; BootPsocHeader h; char path[sizeof boot_psoc.path]; LONG took = 0; size_t n = 0; memset(&h, 0, sizeof h); path[0] = 0;
    AcquireSRWLockExclusive(&boot_psoc.lock);
    if (boot_psoc.lib && boot_psoc.state > 0) {
        n = (size_t)ID3D12PipelineLibrary_GetSerializedSize(boot_psoc.lib);
        if (n && n < 0x60000000u) buf = (uint8_t *)malloc(sizeof h + n);
        if (buf && SUCCEEDED(ID3D12PipelineLibrary_Serialize(boot_psoc.lib, buf + sizeof h, n))) {
            took = InterlockedExchange(&boot_psoc.dirty, 0); h = boot_psoc.id; snprintf(path, sizeof path, "%s", boot_psoc.path);
        } else { free(buf); buf = NULL; }
    }
    ReleaseSRWLockExclusive(&boot_psoc.lock);
    boot_psoc.last_save_ms = GetTickCount64();
    if (!buf) return;
    h.blob_size = n; h.blob_hash = boot_psoc_hash(buf + sizeof h, n, UINT64_C(0x626c6f62)); memcpy(buf, &h, sizeof h);
    char tmp[sizeof path + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int ok = 0; FILE *f = fopen(tmp, "wb");
    if (f) { ok = fwrite(buf, 1, sizeof h + n, f) == sizeof h + n; ok = fclose(f) == 0 && ok; }
    if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) { InterlockedExchangeAdd(&boot_psoc.dirty, took); remove(tmp); }
    else InterlockedIncrement(&boot_psoc.saves);
    free(buf); QueryPerformanceCounter(&q1);
    boot_psoc_log("pso cache: %s %llu KiB (+%ld entries) in %lld ms; loaded %ld, created+stored %ld, store failures %ld, load misses %ld",
                  ok ? "saved" : "SAVE FAILED", (unsigned long long)(n >> 10), (long)took, (long long)((q1.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart),
                  (long)boot_psoc.loaded, (long)boot_psoc.stored, (long)boot_psoc.store_failed, (long)boot_psoc.load_miss);
}
static DWORD WINAPI boot_psoc_save_thread(void *arg) {
    (void)arg; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    boot_psoc_save_body(0); InterlockedExchange(&boot_psoc.saving, 0);
    return 0;
}
/* After a present: starts the saver thread when enough new pipelines exist (a few seconds apart at most). */
static inline void boot_psoc_tick(void) {
    LONG d = boot_psoc.dirty; if (d <= 0) return;
    ULONGLONG since = GetTickCount64() - boot_psoc.last_save_ms;
    if (since < BOOT_PSOC_SAVE_GAP_MS || (d < BOOT_PSOC_SAVE_ENTRIES && since < BOOT_PSOC_TAIL_MS)) return;
    if (InterlockedCompareExchange(&boot_psoc.saving, 1, 0)) return;
    HANDLE t = CreateThread(NULL, 0, boot_psoc_save_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&boot_psoc.saving, 0);
}
/* Window close: the entries since the last save are written before the process goes (waits for a running save). */
static inline void boot_psoc_exit(void) {
    if (boot_psoc.state <= 0 || boot_psoc.dirty < 1) return;
    unsigned waited = 0;
    while (InterlockedCompareExchange(&boot_psoc.saving, 1, 0)) { if (++waited > 1000) return; Sleep(10); }
    boot_psoc_save_body(0); InterlockedExchange(&boot_psoc.saving, 0);
}
/* The pipeline: from the library when it holds it, else compiled and stored. r is NULL on the pre-creation thread
 * (the library is opened by the render thread before that thread starts). */
static inline HRESULT boot_psoc_create_graphics(const BootRender *r, ID3D12Device *dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,
                                                const void *key, size_t klen, ID3D12PipelineState **out) {
    if (r) boot_psoc_open(r);
    uint64_t rh = 0; WCHAR name[33]; int use = boot_psoc.state > 0 && boot_psoc.dev == dev && boot_psoc_root_hash(d->pRootSignature, &rh);
    if (use) {
        uint64_t h[2] = {UINT64_C(0x47524158), UINT64_C(0x5053474f)}; uint32_t nrt = d->NumRenderTargets;
        boot_psoc_chain(h, &rh, 8); boot_psoc_chain(h, key, klen); boot_psoc_chain(h, &nrt, 4);
        boot_psoc_chain(h, d->VS.pShaderBytecode, d->VS.BytecodeLength);
        if (d->PS.pShaderBytecode) boot_psoc_chain(h, d->PS.pShaderBytecode, d->PS.BytecodeLength);
        if (d->GS.pShaderBytecode) boot_psoc_chain(h, d->GS.pShaderBytecode, d->GS.BytecodeLength);   /* G2 point sprites (batch-4) */
        boot_psoc_name(name, h);
        HRESULT hr = E_FAIL;
        AcquireSRWLockExclusive(&boot_psoc.lock);
        if (boot_psoc.lib && boot_psoc.dev == dev) hr = ID3D12PipelineLibrary_LoadGraphicsPipeline(boot_psoc.lib, name, d, &IID_ID3D12PipelineState, (void **)out);
        ReleaseSRWLockExclusive(&boot_psoc.lock);
        if (SUCCEEDED(hr) && *out) { InterlockedIncrement(&boot_psoc.loaded); return hr; }
        *out = NULL; InterlockedIncrement(&boot_psoc.load_miss);
    }
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, d, &IID_ID3D12PipelineState, (void **)out);
    if (use && SUCCEEDED(hr)) {
        HRESULT sh = E_FAIL;
        AcquireSRWLockExclusive(&boot_psoc.lock);
        if (boot_psoc.lib && boot_psoc.dev == dev) sh = ID3D12PipelineLibrary_StorePipeline(boot_psoc.lib, name, *out);
        ReleaseSRWLockExclusive(&boot_psoc.lock);
        if (SUCCEEDED(sh)) { InterlockedIncrement(&boot_psoc.stored); InterlockedIncrement(&boot_psoc.dirty); } else InterlockedIncrement(&boot_psoc.store_failed);
    }
    return hr;
}
static inline HRESULT boot_psoc_create_compute(const BootRender *r, ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *d, ID3D12PipelineState **out) {
    if (r) boot_psoc_open(r);
    uint64_t rh = 0; WCHAR name[33]; int use = boot_psoc.state > 0 && boot_psoc.dev == dev && boot_psoc_root_hash(d->pRootSignature, &rh);
    if (use) {
        uint64_t h[2] = {UINT64_C(0x434f4d50), UINT64_C(0x55544521)};
        boot_psoc_chain(h, &rh, 8); boot_psoc_chain(h, d->CS.pShaderBytecode, d->CS.BytecodeLength);
        boot_psoc_name(name, h);
        HRESULT hr = E_FAIL;
        AcquireSRWLockExclusive(&boot_psoc.lock);
        if (boot_psoc.lib && boot_psoc.dev == dev) hr = ID3D12PipelineLibrary_LoadComputePipeline(boot_psoc.lib, name, d, &IID_ID3D12PipelineState, (void **)out);
        ReleaseSRWLockExclusive(&boot_psoc.lock);
        if (SUCCEEDED(hr) && *out) { InterlockedIncrement(&boot_psoc.loaded); return hr; }
        *out = NULL; InterlockedIncrement(&boot_psoc.load_miss);
    }
    HRESULT hr = ID3D12Device_CreateComputePipelineState(dev, d, &IID_ID3D12PipelineState, (void **)out);
    if (use && SUCCEEDED(hr)) {
        HRESULT sh = E_FAIL;
        AcquireSRWLockExclusive(&boot_psoc.lock);
        if (boot_psoc.lib && boot_psoc.dev == dev) sh = ID3D12PipelineLibrary_StorePipeline(boot_psoc.lib, name, *out);
        ReleaseSRWLockExclusive(&boot_psoc.lock);
        if (SUCCEEDED(sh)) { InterlockedIncrement(&boot_psoc.stored); InterlockedIncrement(&boot_psoc.dirty); } else InterlockedIncrement(&boot_psoc.store_failed);
    }
    return hr;
}


/* ---------------------------------------------------------------- setup */
static inline ID3D12Resource *boot_render_buffer(BootRender *r, D3D12_HEAP_TYPE type, uint64_t size, D3D12_RESOURCE_STATES state, void **map) {
    D3D12_HEAP_PROPERTIES hp = {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *res = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&res))) return NULL;
    ++boot_render_created_buffers;
    if (map && FAILED(ID3D12Resource_Map(res, 0, NULL, map))) { ID3D12Resource_Release(res); return NULL; }
    return res;
}

static inline int boot_render_root_signature(BootRender *r) {
    /* Ranges: per stage 19 CBVs (b0..b18); SRVs t0..t2 space0; UAV u0 space0;
     * eight texture arrays (spaces boot_render_dim_space); samplers (spaces 1..2). */
    D3D12_DESCRIPTOR_RANGE1 vs_cbv = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 19, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 ps_cbv = vs_cbv;
    D3D12_DESCRIPTOR_RANGE1 mem[1] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0}};
    D3D12_DESCRIPTOR_RANGE1 arena_srv = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BOOT_RENDER_MAX_REGIONS, 0, 7,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 arena_uav = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, BOOT_RENDER_MAX_REGIONS, 0, 7,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 tex[BOOT_RENDER_DIMS];
    for (unsigned i = 0; i < BOOT_RENDER_DIMS; ++i) {
        D3D12_DESCRIPTOR_RANGE1 t = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BOOT_RENDER_TEX_SLOTS, 0, boot_render_dim_space[i],
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, BOOT_RENDER_TEX_SLOTS * i};
        tex[i] = t;
    }
    D3D12_DESCRIPTOR_RANGE1 smp[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, BOOT_RENDER_SAMPLERS, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, BOOT_RENDER_SAMPLERS, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, BOOT_RENDER_SAMPLERS}};
    D3D12_ROOT_PARAMETER1 p[7] = {0};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &vs_cbv; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[1] = p[0]; p[1].DescriptorTable.pDescriptorRanges = &ps_cbv; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = mem; p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable.NumDescriptorRanges = BOOT_RENDER_DIMS;
    p[3].DescriptorTable.pDescriptorRanges = tex; p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[4].DescriptorTable.NumDescriptorRanges = 2;
    p[4].DescriptorTable.pDescriptorRanges = smp; p[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[5].DescriptorTable.NumDescriptorRanges = 1;
    p[5].DescriptorTable.pDescriptorRanges = &arena_srv; p[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[6] = p[5]; p[6].DescriptorTable.pDescriptorRanges = &arena_uav;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {0};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 7; desc.Desc_1_1.pParameters = p;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;   /* draw-trace position capture (host_gpu_render_draw.h) */
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) {
        snprintf(r->error, sizeof r->error, "root signature: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(r->gpu->device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                  &IID_ID3D12RootSignature, (void **)&r->root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(r->root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    return SUCCEEDED(hr);
}

/* SRV/UAV heap layout (shader visible):
 *   [0, 2)                       page table, vertex attribs
 *   [ARENA_SRV, +REGIONS)        guest memory region SRVs (space7)
 *   [ARENA_UAV, +REGIONS)        guest memory region UAVs (space7)
 *   [RING, RING + CBV_RING)      CBV ring
 *   [TEX0, TEX0 + DIMS*SLOTS)    texture tables, one block of SLOTS per dimension */
#define BOOT_RENDER_HEAP_ARENA_SRV 2
#define BOOT_RENDER_HEAP_ARENA_UAV (BOOT_RENDER_HEAP_ARENA_SRV + BOOT_RENDER_MAX_REGIONS)
#define BOOT_RENDER_HEAP_RING (BOOT_RENDER_HEAP_ARENA_UAV + BOOT_RENDER_MAX_REGIONS)
#define BOOT_RENDER_HEAP_TEX0 (BOOT_RENDER_HEAP_RING + BOOT_RENDER_CBV_RING)
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_cpu(BootRender *r, ID3D12DescriptorHeap *h, UINT inc, uint32_t index) {
    D3D12_CPU_DESCRIPTOR_HANDLE c; h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c); c.ptr += (SIZE_T)index * inc; (void)r; return c;
}
static inline D3D12_GPU_DESCRIPTOR_HANDLE boot_render_gpu(BootRender *r, ID3D12DescriptorHeap *h, UINT inc, uint32_t index) {
    D3D12_GPU_DESCRIPTOR_HANDLE g; h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g); g.ptr += (UINT64)index * inc; (void)r; return g;
}

static inline int boot_render_map(BootRender *r, uint64_t va, unsigned region, uint64_t offset, uint64_t size);
/* Profiling: executed command lists (each one a CPU wait for queue completion) per call site and
 * the time spent in them, logged by boot_draw_execute. */
static struct { const char *file; int line; uint64_t count; } boot_render_submit_sites[48];
static uint64_t boot_render_submit_total, boot_render_submit_qpc;
/* Lists executed without a wait (segment full), and CPU time spent waiting for a segment's previous
 * list before reusing it. */
static uint64_t boot_render_kicks, boot_render_seg_waits, boot_render_seg_wait_qpc;
static uint64_t boot_render_ring_full;   /* lists executed early because the segment's rings were full */
/* End of the current list's CBV descriptor segment (relative to BOOT_RENDER_HEAP_RING). Starts at one
 * segment: a list left open by an earlier module stays within the first 4096 descriptors. */
static uint32_t boot_render_cbv_end = BOOT_RENDER_CBV_SEGMENT;
/* Scope of constant-buffer snapshots that later draws may share (boot_render_cbv): a new list (the
 * ring segment changes) or a page-table change starts a new scope. */
static uint64_t boot_render_cb_scope = 1;
/* Tap-only evidence of the actual interval assigned by successful begin. A
 * changed/unknown scope offers no bytes; this never changes original caching. */
static ID3D12Resource *boot_render_cb_tap_ring;
static uint64_t boot_render_cb_tap_scope,boot_render_cb_tap_start,boot_render_cb_tap_end;
static inline int boot_render_cb_tap_span(BootRender *r,uint64_t offset,uint64_t bytes,unsigned fresh) {
    if(!boot_cbv_tap_registered || boot_render_cb_tap_ring!=r->ring || boot_render_cb_tap_scope!=boot_render_cb_scope ||
       boot_render_cb_tap_end!=r->ring_size || offset<boot_render_cb_tap_start || offset>r->ring_size || bytes>r->ring_size-offset) return 0;
    return fresh?offset==r->ring_used:offset<=r->ring_used && bytes<=r->ring_used-offset;
}
/* Texture-slot reuse (boot_render_slot_free). A texture slot's descriptors live at a fixed heap index, so
 * rewriting them, or releasing the slot's resource, must wait until no recorded or executing list still
 * uses them. That used to be a submit and a full queue wait whenever a valid slot changed while a list was
 * open (2+ per submission, 40-60 ms each: "command lists by site host_gpu_render.h:...texture"). Now every
 * list gets an id (boot_render_list_id, one per boot_render_begin), draws and dispatches stamp the slots
 * they bound with the id of the list they are recorded in (boot_render_tex_commit), kicks remember the
 * fence of each list id, and a rewrite waits only for the fence of the list that last used the slot (often
 * long done: no wait), closing the open list only when the open list itself uses the slot. */
static uint64_t boot_render_list_id = 1;
/* play (38): graphics state boot_render_draw last set on r->cl, valid while boot_render_bind_epoch is unchanged.
 * The epoch moves whenever the list starts over (begin, gt resume) or other code sets graphics state on it
 * (compute pipeline, clear helper draw). Fields are byte-compared; an unknown field is all 0xff. */
static struct {
    uint64_t epoch; ID3D12PipelineState *pso; UINT nrt; int has_dsv; D3D12_CPU_DESCRIPTOR_HANDLE rtv[8], dsv;
    float bf[4]; UINT sref; D3D12_VIEWPORT vp; D3D12_RECT sc; D3D_PRIMITIVE_TOPOLOGY topo; D3D12_INDEX_BUFFER_VIEW ib;
} boot_render_bound;
static uint64_t boot_render_bind_epoch = 1, boot_render_binds_skipped, boot_render_pso_memo_hits;
static uint64_t boot_render_slot_list[BOOT_RENDER_TEX_SLOTS];          /* list id that last bound the slot (0: never) */
static struct { uint64_t id, fence; } boot_render_list_fence[65536];   /* list id -> fence signalled after it (~18 min of lists) */
static uint32_t boot_render_tex_pending[256]; static unsigned boot_render_tex_pending_n, boot_render_tex_pending_over;
static uint64_t boot_render_slot_free_open, boot_render_slot_free_waited, boot_render_slot_free_done, boot_render_slot_free_drain;
/* Host writes to guest memory while recording, per 4 KiB guest GPU page (direct-mapped by page
 * number): the epoch of the last write. Ranges too large for the table bump big_writes instead.
 * exec_id changes per executed submission and map_gen whenever the page table changes. */
#define BOOT_RENDER_CB_PAGES 16384
static uint64_t boot_render_cb_page[BOOT_RENDER_CB_PAGES], boot_render_cb_epoch = 1, boot_render_big_writes, boot_render_exec_id = 1, boot_render_map_gen;
static inline void boot_render_cb_written(uint64_t va, uint64_t size) {
    if (!size) return;
    uint64_t first = va >> 12, last = (va + size - 1) >> 12;
    if (last < first || last - first >= BOOT_RENDER_CB_PAGES) { ++boot_render_cb_scope; ++boot_render_big_writes; return; }
    uint64_t e = ++boot_render_cb_epoch;
    for (uint64_t p = first; p <= last; ++p) boot_render_cb_page[p & (BOOT_RENDER_CB_PAGES - 1)] = e;
}
static inline int boot_render_cb_clean(uint64_t va, uint64_t bytes, uint64_t epoch) {
    if (!bytes) return 1;
    for (uint64_t p = va >> 12, last = (va + bytes - 1) >> 12; p <= last; ++p)
        if (boot_render_cb_page[p & (BOOT_RENDER_CB_PAGES - 1)] >= epoch) return 0;
    return 1;
}
static inline int boot_render_submit_impl(BootRender *r);
/* nvnpar-work: submits made while a draw resolves its textures (host_gpu_render_draw.h, texture detail), by call site,
 * with their time (kick + queue wait + cleanup): each one is a complete GPU drain in the middle of the frame. */
static int boot_render_txd_in;
static uint64_t boot_render_txd_site_n[48], boot_render_txd_site_q[48];
static inline int boot_render_submit_at(BootRender *r, const char *file, int line) {
    if (!r->recording) return 1;
    unsigned i = 0;
    while (i < 48 && boot_render_submit_sites[i].file && (boot_render_submit_sites[i].line != line || strcmp(boot_render_submit_sites[i].file, file))) ++i;
    if (i < 48) { if (!boot_render_submit_sites[i].file) { boot_render_submit_sites[i].file = file; boot_render_submit_sites[i].line = line; } ++boot_render_submit_sites[i].count; }
    ++boot_render_submit_total;
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    int ok = boot_render_submit_impl(r);
    QueryPerformanceCounter(&b); boot_render_submit_qpc += (uint64_t)(b.QuadPart - a.QuadPart);
    if (boot_render_txd_in && i < 48) { ++boot_render_txd_site_n[i]; boot_render_txd_site_q[i] += (uint64_t)(b.QuadPart - a.QuadPart); }
    return ok;
}
#define boot_render_submit(r) boot_render_submit_at((r), __FILE__, __LINE__)
/* Raw SRV/UAV (space7) of guest region `i`, or null views when the slot is empty. */
static inline void boot_render_region_views(BootRender *r, unsigned i) {
    ID3D12Resource *res = r->region[i]; uint64_t size = res ? r->region_size[i] : 4;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.Buffer.NumElements = (UINT)(size / 4); sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    ID3D12Device_CreateShaderResourceView(r->gpu->device, res, &sv, boot_render_cpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_ARENA_SRV + i));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {0};
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.Buffer.NumElements = (UINT)(size / 4); uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(r->gpu->device, res, NULL, &uv, boot_render_cpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_ARENA_UAV + i));
}
/* nvnpar-work 1a: texture binding set cache (TOTK_TEXSET_CACHE, default on; =0: every binding resolved as before;
 * TOTK_TEXSET_VERIFY=1: every remembered binding is resolved anyway and a resolution that did not take the unchanged
 * return is counted and logged). A draw's texture set is resolved one binding at a time (boot_render_texture /
 * boot_render_sampler per handle of the draw's c2 snapshot); with the TIC ring and per-frame descriptor rewrites a
 * key over the whole set would still have to check each binding, so the set is cached per binding: (pool, index) ->
 * "the last resolution of this entry took the unchanged return" (texture: valid slot, TIC words equal to the slot's,
 * no zero-copy view, built from guest bytes (size != 0, not a subresource assembly: no target staleness to ask), not
 * CPU-watched, not a feedback view, words from the host descriptor table; sampler: valid slot, equal words from the
 * host table). That return reads the entry and the slot and changes nothing (the LRU stamp and the host-hit counter
 * aside, which a remembered binding updates the same way), so it is exact to take it again while none of what it
 * read has changed. Everything it reads is written only by: a resolution of the same index (any other outcome; each
 * resolution starts by forgetting the entry), boot_render_tex_poison (range invalidation, re-hash, target writes,
 * rt-copy registrations, watch checks), boot_render_tex_trim evictions, boot_render_fb_bind (feedback view at the
 * index), boot_render_host_register (a new descriptor for the index) and the device recovery that clears every slot;
 * each of them forgets the entry (the last one all entries). A remembered entry has no feedback view and no target
 * alias (both are cleared by the resolution that remembers it and set again only by the paths above), so the draw
 * loop also skips the feedback / alias tests for it (they would find nothing). The key holds the pool address and a
 * 16-bit epoch (all entries forgotten by moving it). */
static uint64_t boot_render_texmemo[BOOT_RENDER_TEX_SLOTS], boot_render_smpmemo[BOOT_RENDER_SAMPLERS];
static uint64_t boot_render_texmemo_epoch = 1, boot_render_texmemo_hits, boot_render_texmemo_sets, boot_render_smpmemo_hits, boot_render_smpmemo_sets;
static uint64_t boot_render_texmemo_resolves, boot_render_smpmemo_resolves, boot_render_texmemo_resets, boot_render_texmemo_verified, boot_render_texmemo_bad;
static int boot_render_texmemo_pure;   /* the resolution just made took the remembered (unchanged) return */
static inline int boot_render_texmemo_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD n = GetEnvironmentVariableA("TOTK_TEXSET_CACHE", t, sizeof t); v = !(n && n < sizeof t && t[0] == '0'); }
    return v;
}
static inline int boot_render_texmemo_verify_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD n = GetEnvironmentVariableA("TOTK_TEXSET_VERIFY", t, sizeof t); v = n && n < sizeof t && t[0] == '1'; }
    return v;
}
static inline uint64_t boot_render_texmemo_key(uint64_t pool) {
    return pool < (UINT64_C(1) << 48) - 1 ? (pool + 1) | ((boot_render_texmemo_epoch & 0xffff) << 48) : 0;
}
/* 1 = take the remembered return (not with the verify mode, which resolves everything). */
static inline int boot_render_texmemo_ok(uint64_t pool, uint32_t index) {
    uint64_t k = boot_render_texmemo_key(pool);
    return k && boot_render_texmemo[index] == k && boot_render_texmemo_on() && !boot_render_texmemo_verify_on();
}
static inline void boot_render_texmemo_reset(void) {   /* every entry forgotten */
    ++boot_render_texmemo_resets;
    if (!(++boot_render_texmemo_epoch & 0xffff)) {   /* the epoch field wrapped: clear for real */
        memset(boot_render_texmemo, 0, sizeof boot_render_texmemo); memset(boot_render_smpmemo, 0, sizeof boot_render_smpmemo); ++boot_render_texmemo_epoch; }
}
/* drain-work: texture slot versioning (TOTK_SLOT_VERSIONING, default on; =0: the submit and full GPU wait as before).
 * A texture slot's descriptors live at a fixed heap index (the TIC index the shader takes from the handle) and the GPU
 * reads them when a list executes, so a valid slot rebuilt while a list that may still run uses it (the open list, or
 * an executed one whose fence is not reached) was never rewritten in place: boot_render_slot_free submitted the open
 * list and drained the queue first (the tower look-down view: ~2.7 times a frame, ~12.8 ms a frame). The descriptors
 * are versioned in time instead. The open list is executed at once without a wait (recording continues in the next
 * list, as when a segment is full), the rebuilt slot's descriptor writes go into a CPU-only staging heap, and right
 * before the next list is executed (boot_render_kick) the lists that read the old descriptors are waited for (normally
 * complete by then: a whole list was recorded meanwhile) and the staged descriptors are copied into place. Every draw
 * reads exactly the descriptors it read before (the old ones up to the reuse, the rebuilt ones after it), in the same
 * queue order; the slot's index, the handles, constant buffers and shaders are unchanged. The rebuild's GPU work
 * (barriers, copies into a reused or pooled texture) is recorded after the old list in queue order, and the replaced
 * resource is pooled or released only once the old lists completed (boot_render_sv_take). Waits that remain are
 * counted by reason on the "texture slot reuse: already done" line. */
#define BOOT_RENDER_SV_STAGE 4096       /* staged descriptors (and versioned slots) between two list executions */
#define BOOT_RENDER_SV_RETIRE 1024      /* replaced slot resources held until their lists completed */
static uint8_t boot_render_sv_flag[BOOT_RENDER_TEX_SLOTS];   /* 1: the index's descriptor writes go to the staging heap */
static uint32_t boot_render_sv_idx[BOOT_RENDER_SV_STAGE]; static unsigned boot_render_sv_idx_n;
static struct { uint32_t index, dim; } boot_render_sv_ent[BOOT_RENDER_SV_STAGE]; static unsigned boot_render_sv_n;
static uint64_t boot_render_sv_guard;   /* fence after which no executed list reads a versioned index's visible descriptors */
static ID3D12DescriptorHeap *boot_render_sv_heap; static ID3D12Device *boot_render_sv_dev; static UINT boot_render_sv_inc;
static uint64_t boot_render_sv_kicks, boot_render_sv_deferred, boot_render_sv_staged, boot_render_sv_applies, boot_render_sv_kick_waits, boot_render_sv_wait_qpc,
    boot_render_sv_full_waits, boot_render_sv_retire_waits, boot_render_sv_held, boot_render_sv_fallbacks, boot_render_slot_free_open_waits;
static inline int boot_render_sv_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD n = GetEnvironmentVariableA("TOTK_SLOT_VERSIONING", t, sizeof t); v = !(n && n < sizeof t && t[0] == '0'); }
    return v;
}
static inline int boot_render_sv_apply(BootRender *r, int full);
/* drain2-work (the play-93 stretched polygon): boot_render_slot_free guards only the rebuild of a VALID slot. An index
 * whose slot is not valid (a view of a render target or of a feedback copy, a slot a failed build left invalid) had
 * its descriptors rewritten in place with no check, although recorded draws may still read them. TIC 12738 (the scene
 * depth, 529a00000 1024x576 ZF32, sampled by the pixel shaders of the multi-pass meshes vs c88e3c0a / 16b86625, draw
 * trace #3274..#3354: depth EQUAL without writes, ALWAYS with writes, off, LESS with writes) switches several times a
 * frame between a copy of the depth target (draws that write depth) and a view of the target itself (draws that do
 * not). Copy -> view went through slot_free; view -> copy did not, so the copy's descriptor replaced the view for every
 * draw already recorded in the open list: the EQUAL pass that builds the mask the ALWAYS pass writes depth through read
 * the copy's texture before the copy commands ran, i.e. whatever it held. With TOTK_SLOT_VERSIONING=0 the pool hands
 * back the copy released a moment before (nearly the same depth: invisible); with versioning that texture is held until
 * its fence, the pool gives another one, and the mesh lands in the scene depth. Versioning also applied both versions,
 * staged in one list, as the last one. Now every descriptor write checks the index's last RECORDED use
 * (boot_render_slot_rec, set by boot_render_tex_commit): by the open list -> that list is executed first (no wait) and
 * the write is staged; by an executed list still running -> staged until it completes. TOTK_SLOT_VERSIONING=0: the
 * writes stay in place as before (counted and logged: the same hazard, masked by the pool). */
static uint64_t boot_render_slot_rec[BOOT_RENDER_TEX_SLOTS];   /* list id of the last recorded draw / dispatch that bound the index (0: none) */
static uint64_t boot_render_sv_wg[4];   /* such writes: [0] read by the open list (executed first), [1] by an executed list still running,
                                           [2] by the open list after the index was staged (two versions in one list), [3] left in place (versioning off) */
/* blacktex-work (play-104/105, versioning off: environment objects and the floor black for a moment, sprites / cards /
 * decals black squares). The writes "left in place" above are that hazard: when the open list executes, the draws
 * recorded in it before the write read the NEW descriptors (play-105: 97956 writes = ~10900 rewrites of an index, ~0.7
 * per present). ~90% of the sampled ones are the scene depth (529a00000 1024x576 ZF32, its TIC a ring index): a draw that
 * does not write depth makes the index a view of the depth target (the slot not valid), the next draw that writes depth
 * and samples it rebuilds it as a copy (boot_render_texture_from_target: not valid, so no slot_free) and its descriptors
 * replace the view in place, so every earlier draw of the list samples the copy's texture before the copy commands ran
 * (a pooled image of that shape, or a new zero-filled one: black); the next draw that does not write depth makes it a
 * view again through slot_free (a valid slot: a submission and a full GPU wait, play-105 9791). The rest are colour target
 * views rewritten after the target table changed. (A guard line shows the slot as it was before the write: "valid 0
 * resource no size 0" is a target view, not an empty slot; what is written is a view or a texture.) TOTK_TEX_KEEP (default on;
 * =0: exactly as before) protects those writes with versioning off as with versioning on: the reading open list is
 * executed first without a wait and the writes are staged, copied in place right before the next list executes once
 * the lists that read the old ones completed (boot_render_sv_apply), so every recorded draw keeps the descriptors it was
 * recorded with; and the depth copy is refreshed in place instead of turning back into a view (TOTK_DFB_KEEP_COPY, so far
 * only with versioning), which removes the copy -> view switches and their full waits. boot_render_slot_free itself is
 * unchanged (versioning off: submit and wait). A write while no list is open whose index a list still running bound
 * (the guard never sees it; the "not recording means idle" invariant says it cannot happen) is waited for and counted. */
static inline int boot_render_tex_keep_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_KEEP", t, sizeof t); v = !(n && n < sizeof t && t[0] == '0'); }
    return v;
}
static uint16_t boot_render_slot_rec_n[BOOT_RENDER_TEX_SLOTS];   /* bindings of the index by draws / dispatches recorded in list boot_render_slot_rec[i] (saturating) */
static uint64_t boot_render_tk_events[2], boot_render_tk_draws[2], boot_render_tk_idle_waits;   /* [0] kept on the old descriptors, [1] slipped through (in place):
    rewrites (one per index and reading list), the recorded bindings that read the index; writes with no open list while a reading list ran (waited for) */
static inline void boot_render_slot_rec_mark(uint32_t index) {   /* the index is bound by a draw / dispatch recorded in the open list */
    boot_render_slot_rec_n[index] = boot_render_slot_rec[index] == boot_render_list_id ? (uint16_t)(boot_render_slot_rec_n[index] + (boot_render_slot_rec_n[index] < 0xffffu)) : (uint16_t)1;
    boot_render_slot_rec[index] = boot_render_list_id;
}
static inline void boot_render_tk_note(unsigned slipped, uint32_t index, uint64_t used) {   /* once per index and reading list (in place: one per write) */
    static uint32_t li = UINT32_MAX; static uint64_t ll;
    if (index == li && used == ll) return;
    li = index; ll = used; ++boot_render_tk_events[slipped]; boot_render_tk_draws[slipped] += boot_render_slot_rec_n[index];
}
static inline int boot_render_flush(BootRender *r);
static inline int boot_render_wait_fence_at(BootGpu *g, uint64_t value, int line);
static inline int boot_render_sv_mark(BootRender *r, uint32_t index, uint64_t fence);
static inline void boot_render_sv_wg_log(BootRender *r, unsigned dim, uint32_t index, unsigned kind, uint64_t used, uint64_t fence, int how);
static inline void boot_render_sv_write_guard(BootRender *r, unsigned dim, uint32_t index) {
    const uint64_t used = boot_render_slot_rec[index];
    if (!used || index >= BOOT_RENDER_TEX_SLOTS - 256u) return;   /* never bound by recorded work; the top 256: feedback slots (their own retirement) */
    const int open = used == boot_render_list_id;
    uint64_t f = 0;
    if (!open) {
        if (boot_render_sv_flag[index]) return;   /* staged: no list was executed since, so its guard covers every earlier use */
        if (boot_render_list_fence[used & 65535].id != used) return;   /* never executed, or long complete */
        f = boot_render_list_fence[used & 65535].fence;
        if (ID3D12Fence_GetCompletedValue(r->gpu->fence) >= f) return;
    }
    const unsigned kind = open ? (boot_render_sv_flag[index] ? 2u : 0u) : 1u;
    if (!boot_render_sv_on() && !boot_render_tex_keep_on()) {   /* blacktex-work: TOTK_TEX_KEEP=0, as before */
        ++boot_render_sv_wg[3]; boot_render_tk_note(1, index, used); boot_render_sv_wg_log(r, dim, index, kind, used, f, 0); return; }
    ++boot_render_sv_wg[kind]; boot_render_tk_note(0, index, used);   /* blacktex-work: its readers keep the old descriptors (either mode) */
    if (open) {   /* its draws read the current contents: executed first (without a wait), recording goes on in the next list */
        const uint64_t lid = boot_render_list_id;
        if (!boot_render_flush(r)) return;   /* failed: the queue was waited for, in place is safe */
        f = boot_render_list_fence[lid & 65535].id == lid ? boot_render_list_fence[lid & 65535].fence : r->gpu->submitted;
    }
    const int staged = boot_render_sv_mark(r, index, f);
    if (!staged) { ++boot_render_sv_fallbacks; boot_render_wait_fence_at(r->gpu, f, __LINE__); }   /* no staging heap: a wait, as before versioning */
    boot_render_sv_wg_log(r, dim, index, kind, used, f, staged ? (boot_render_sv_on() ? 1 : 3) : 2);
}
/* blacktex-work: a descriptor write with no open list, the index bound by an executed list still running: waited for. */
static inline void boot_render_tk_idle_write(BootRender *r, uint32_t index) {
    const uint64_t used = boot_render_slot_rec[index];
    if (index >= BOOT_RENDER_TEX_SLOTS - 256u || boot_render_list_fence[used & 65535].id != used) return;   /* feedback slots; never executed, or long complete */
    const uint64_t f = boot_render_list_fence[used & 65535].fence;
    if (ID3D12Fence_GetCompletedValue(r->gpu->fence) >= f) return;
    ++boot_render_tk_idle_waits; boot_render_wait_fence_at(r->gpu, f, __LINE__);
}
/* Where a descriptor of texture table `dim` at TIC index `index` is written: in place, or (the index versioned) into the
 * staging heap, copied in place by boot_render_sv_apply in the order written. */
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_tex_dst(BootRender *r, unsigned dim, uint32_t index) {
    if (index < BOOT_RENDER_TEX_SLOTS && boot_render_slot_rec[index]) {
        if (r->recording) boot_render_sv_write_guard(r, dim, index);   /* drain2-work */
        else if (boot_render_tex_keep_on()) boot_render_tk_idle_write(r, index);   /* blacktex-work: no open list, so no guard */
    }
    if (index < BOOT_RENDER_TEX_SLOTS && boot_render_sv_flag[index]) {
        if (boot_render_sv_n == BOOT_RENDER_SV_STAGE) boot_render_sv_apply(r, 1);   /* full: wait for the old lists, everything in place */
        if (boot_render_sv_flag[index] && boot_render_sv_heap) {
            D3D12_CPU_DESCRIPTOR_HANDLE c; boot_render_sv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(boot_render_sv_heap, &c);
            c.ptr += (SIZE_T)boot_render_sv_n * boot_render_sv_inc;
            boot_render_sv_ent[boot_render_sv_n].index = index; boot_render_sv_ent[boot_render_sv_n].dim = dim; ++boot_render_sv_n; ++boot_render_sv_staged;
            return c;
        }
    }
    /* blacktex review: the real 'slipped through' check (boot_render_tk_note(1) above only runs with TOTK_TEX_KEEP off): an
     * in-place write of an index the open list's recorded draws read (after the guard, which executes that list first). */
    if (boot_render_tex_keep_on() && r->recording && index < BOOT_RENDER_TEX_SLOTS - 256u && boot_render_slot_rec[index] == boot_render_list_id)
        boot_render_tk_note(1, index, boot_render_list_id);
    return boot_render_cpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_TEX0 + dim * BOOT_RENDER_TEX_SLOTS + index);
}
/* Null SRV in texture table `dim` at TIC index `index`: sampling returns 0, GetDimensions 0. */
static const D3D12_SRV_DIMENSION boot_render_dim_srv[BOOT_RENDER_DIMS] = {D3D12_SRV_DIMENSION_TEXTURE1D, D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY,
    D3D12_SRV_DIMENSION_BUFFER, D3D12_SRV_DIMENSION_TEXTURE1DARRAY};
static inline void boot_render_null_srv(BootRender *r, unsigned dim, uint32_t index) {
    D3D12_SHADER_RESOURCE_VIEW_DESC nv; memset(&nv, 0, sizeof nv);
    nv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nv.ViewDimension = boot_render_dim_srv[dim]; nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dim) {
        case BOOT_DIM_1D: nv.Texture1D.MipLevels = 1; break;
        case BOOT_DIM_1D_ARRAY: nv.Texture1DArray.MipLevels = 1; nv.Texture1DArray.ArraySize = 1; break;
        case BOOT_DIM_2D: nv.Texture2D.MipLevels = 1; break;
        case BOOT_DIM_2D_ARRAY: nv.Texture2DArray.MipLevels = 1; nv.Texture2DArray.ArraySize = 1; break;
        case BOOT_DIM_3D: nv.Texture3D.MipLevels = 1; break;
        case BOOT_DIM_CUBE: nv.TextureCube.MipLevels = 1; break;
        case BOOT_DIM_CUBE_ARRAY: nv.TextureCubeArray.MipLevels = 1; nv.TextureCubeArray.NumCubes = 1; break;
        default: nv.Buffer.NumElements = 1; break;
    }
    ID3D12Device_CreateShaderResourceView(r->gpu->device, NULL, &nv, boot_render_tex_dst(r, dim, index));   /* drain-work: staged while versioned */
}
/* Registers fully committed VirtualAlloc memory as a zero-copy guest region; returns its index or -1. */
static inline int boot_render_add_region(BootRender *r, void *cpu, uint64_t size) {
    if (r->region_count == BOOT_RENDER_MAX_REGIONS) { boot_render_fail(r, 0, "region table full"); return -1; }
    ++boot_render_created_regions;
    unsigned i = r->region_count;
    ID3D12Device3 *d3 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(r->gpu->device, &IID_ID3D12Device3, (void **)&d3))) { boot_render_fail(r, 0, "ID3D12Device3 unavailable"); return -1; }
    HRESULT hr = ID3D12Device3_OpenExistingHeapFromAddress(d3, cpu, &IID_ID3D12Heap, (void **)&r->region_heap[i]);
    ID3D12Device3_Release(d3);
    if (FAILED(hr)) { boot_render_fail(r, 0, "OpenExistingHeapFromAddress failed (region must be fully committed)"); return -1; }
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(ID3D12Device_CreatePlacedResource(r->gpu->device, r->region_heap[i], 0, &d, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                 &IID_ID3D12Resource, (void **)&r->region[i]))) { boot_render_fail(r, 0, "region placed resource"); return -1; }
    r->region_cpu[i] = cpu; r->region_size[i] = size;
    boot_render_region_views(r, i);
    return (int)r->region_count++;
}

/* Set once this module's descriptor layout and caches are in force for the render object (see
 * boot_render_layout). Module-static: a rebuilt runtime DLL starts at 0. */
static int boot_render_layout_current;
static inline void boot_render_tex_retire_previous(BootRender *r);
static inline void boot_render_alias_reset(void);
/* Opens the renderer. `arena` (fully committed, `size` bytes) becomes region 0 and guest GPU VA
 * [va_base, va_base + size) maps linearly onto it; further regions can be added and mapped. */
/* Page table: the GPU reads the mapped UPLOAD buffer (boot_render_pt_gpu, write-combined: CPU reads of it are
 * very slow); r->ptable_cpu is a cached shadow copy in normal memory that every CPU lookup reads. Writes go to both. */
static uint32_t *boot_render_pt_gpu;
/* play (74): chunks (4096 pages = 16 KiB of entries) that boot_render_map wrote since the table was last cleared. A
 * remap used to memset the whole table twice (shadow and write-combined upload copy: 2 x 24 MiB at 6 M pages,
 * 603 remaps in a 23 minute run, a burst of them at every area load); only the mapped chunks hold anything else
 * than the unmapped marker, so only those are cleared. The set always covers every entry written through
 * boot_render_map (the only writer besides the full clears); a table that is new, resized or not the one the set
 * was kept for is cleared whole. */
#define BOOT_RENDER_PT_CHUNK_SHIFT 12
static uint8_t boot_render_pt_dirty[((1u << 26) >> BOOT_RENDER_PT_CHUNK_SHIFT)];
/* t2-B: chunks boot_render_map ever wrote an entry of (never cleared). The compute store bitmap only holds bits of
 * entries that were mapped when a dispatch ran (maxwell_runtime.hlsli: gmem_mark runs after the mapped check), so
 * boot_compute_idle skips the chunks never mapped instead of reading the whole 16 MiB bitmap per waiting submit. */
static uint8_t boot_render_pt_ever[((1u << 26) >> BOOT_RENDER_PT_CHUNK_SHIFT)];
static const uint32_t *boot_render_pt_dirty_for; static uint32_t boot_render_pt_dirty_pages;
static uint64_t boot_render_pt_partial_clears, boot_render_pt_full_clears;
static inline uint32_t *boot_render_pt_shadow(uint32_t *old, uint32_t pages) {
    uint32_t *s = (uint32_t *)realloc(old, (size_t)(pages ? pages : 1) * 4);
    if (!s) free(old);
    return s;
}
static inline int boot_render_open(BootRender *r, BootGpu *g, void *arena, uint64_t size, uint64_t va_base, const char *manifest) {
    memset(r, 0, sizeof *r); r->gpu = g;
    snprintf(r->manifest_path, sizeof r->manifest_path, "%s", manifest);
    r->arena_cpu = arena; r->arena_size = size;
    r->va_base_page = (uint32_t)(va_base >> 12); r->ptable_pages = arena ? (uint32_t)((size + 0xfff) >> 12) : 1;
    r->ptable = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, (uint64_t)r->ptable_pages * 4, D3D12_RESOURCE_STATE_GENERIC_READ, (void **)&boot_render_pt_gpu);
    r->ptable_cpu = boot_render_pt_shadow(r->ptable_cpu, r->ptable_pages);
    r->attribs = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, 32 * 32, D3D12_RESOURCE_STATE_GENERIC_READ, &r->attribs_cpu);
    r->ring_size = BOOT_RENDER_RING_BYTES;
    r->ring = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, r->ring_size, D3D12_RESOURCE_STATE_GENERIC_READ, (void **)&r->ring_cpu);
    if (!r->ptable || !r->ptable_cpu || !r->attribs || !r->ring) return boot_render_fail(r, 0, "upload buffers");
    for (uint32_t i = 0; i < r->ptable_pages; ++i) r->ptable_cpu[i] = boot_render_pt_gpu[i] = 0xffffffffu;   /* filled by boot_render_map */
    if (!boot_render_root_signature(r)) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, BOOT_RENDER_HEAP_TEX0 + BOOT_RENDER_DIMS * BOOT_RENDER_TEX_SLOTS,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g->device, &hd, &IID_ID3D12DescriptorHeap, (void **)&r->srv_heap))) return boot_render_fail(r, 0, "srv heap");
    D3D12_DESCRIPTOR_HEAP_DESC sd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2 * BOOT_RENDER_SAMPLERS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g->device, &sd, &IID_ID3D12DescriptorHeap, (void **)&r->sampler_heap))) return boot_render_fail(r, 0, "sampler heap");
    D3D12_DESCRIPTOR_HEAP_DESC rd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BOOT_RENDER_MAX_RT, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g->device, &rd, &IID_ID3D12DescriptorHeap, (void **)&r->rtv_heap))) return boot_render_fail(r, 0, "rtv heap");
    r->srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(g->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r->sampler_inc = ID3D12Device_GetDescriptorHandleIncrementSize(g->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    r->rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(g->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC dd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, BOOT_RENDER_MAX_RT, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g->device, &dd, &IID_ID3D12DescriptorHeap, (void **)&r->dsv_heap))) return boot_render_fail(r, 0, "dsv heap");
    r->dsv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(g->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    /* memory views: page table (t1) and vertex attributes (t2) */
    D3D12_SHADER_RESOURCE_VIEW_DESC pt = {0};
    pt.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; pt.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pt.Format = DXGI_FORMAT_UNKNOWN; pt.Buffer.NumElements = r->ptable_pages; pt.Buffer.StructureByteStride = 4;
    ID3D12Device_CreateShaderResourceView(g->device, r->ptable, &pt, boot_render_cpu(r, r->srv_heap, r->srv_inc, 0));
    D3D12_SHADER_RESOURCE_VIEW_DESC at = pt; at.Buffer.NumElements = 32; at.Buffer.StructureByteStride = 32;
    ID3D12Device_CreateShaderResourceView(g->device, r->attribs, &at, boot_render_cpu(r, r->srv_heap, r->srv_inc, 1));
    for (unsigned i = 0; i < BOOT_RENDER_MAX_REGIONS; ++i) boot_render_region_views(r, i);
    if (arena && (boot_render_add_region(r, arena, size) != 0 || !boot_render_map(r, va_base, 0, 0, size))) return 0;
    /* null texture descriptors so unused TIC slots are well defined */
    for (unsigned dim = 0; dim < BOOT_RENDER_DIMS; ++dim)
        for (uint32_t i = 0; i < BOOT_RENDER_TEX_SLOTS; ++i) boot_render_null_srv(r, dim, i);
    boot_render_layout_current = 1; boot_render_alias_reset();
    if (FAILED(ID3D12Device_CreateCommandAllocator(g->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&r->alloc)) ||
        FAILED(ID3D12Device_CreateCommandList(g->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&r->cl)))
        return boot_render_fail(r, 0, "command list");
    boot_dred_created(r->cl);
    boot_light84_prepare(r, g->device, g->fence, g->queue, r->cl);
    boot_light86_prepare(r, g->device, g->fence, g->queue, r->cl);
    { HRESULT rf98_close=ID3D12GraphicsCommandList_Close(r->cl);
      if(FAILED(rf98_close)){BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_OPEN_CLOSE,.known=BOOT_RF98_HR,.hr=(uint32_t)rf98_close,.allocator=(uintptr_t)r->alloc};boot_rf98_note(&x);} }
    boot_psoc_open(r);   /* play (73): the persistent pipeline library of this device, read before the first draw */
    return 1;
}

/* The live service keeps one render object across rebuilt runtime DLLs: its descriptor heap and
 * root signature may come from an older layout (six texture tables), and its shader/PSO caches may
 * hold DXIL that has since been retranslated. On the first submission a module executes, bring the
 * object to this module's layout: a larger heap with every fixed descriptor rewritten (texture
 * slots are module-static, so they are rebuilt on use anyway), this module's root signature, and
 * empty shader/PSO caches (reloaded from the manifest on demand). Render targets are kept. */
static inline int boot_render_layout(BootRender *r) {
    if (boot_render_layout_current) return 1;
    if (r->recording && !boot_render_submit(r)) return boot_render_fail(r, 0, "submit before layout update");
    const UINT need = BOOT_RENDER_HEAP_TEX0 + BOOT_RENDER_DIMS * BOOT_RENDER_TEX_SLOTS;
    D3D12_DESCRIPTOR_HEAP_DESC have; r->srv_heap->lpVtbl->GetDesc(r->srv_heap, &have);
    if (have.NumDescriptors < need) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, need, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ID3D12DescriptorHeap *heap = NULL, *old = r->srv_heap;
        if (FAILED(ID3D12Device_CreateDescriptorHeap(r->gpu->device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap))) return boot_render_fail(r, 0, "srv heap (layout update)");
        r->srv_heap = heap;
        D3D12_SHADER_RESOURCE_VIEW_DESC pt = {0};
        pt.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; pt.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pt.Format = DXGI_FORMAT_UNKNOWN; pt.Buffer.NumElements = r->ptable_pages; pt.Buffer.StructureByteStride = 4;
        ID3D12Device_CreateShaderResourceView(r->gpu->device, r->ptable, &pt, boot_render_cpu(r, r->srv_heap, r->srv_inc, 0));
        D3D12_SHADER_RESOURCE_VIEW_DESC at = pt; at.Buffer.NumElements = 32; at.Buffer.StructureByteStride = 32;
        ID3D12Device_CreateShaderResourceView(r->gpu->device, r->attribs, &at, boot_render_cpu(r, r->srv_heap, r->srv_inc, 1));
        for (unsigned i = 0; i < BOOT_RENDER_MAX_REGIONS; ++i) boot_render_region_views(r, i);
        for (unsigned dim = 0; dim < BOOT_RENDER_DIMS; ++dim)
            for (uint32_t i = 0; i < BOOT_RENDER_TEX_SLOTS; ++i) boot_render_null_srv(r, dim, i);
        ID3D12DescriptorHeap_Release(old);   /* the GPU is idle: nothing recorded references it */
        ID3D12RootSignature *root = r->root;
        if (!boot_render_root_signature(r)) { r->root = root; return 0; }
        if (root) ID3D12RootSignature_Release(root);
    }
    boot_render_tex_retire_previous(r);   /* after the heap has this module's size */
    boot_render_alias_reset();            /* texture descriptors were rewritten */
    for (unsigned i = 0; i < r->pso_count; ++i) if (r->psos[i].pso) ID3D12PipelineState_Release(r->psos[i].pso);
    r->pso_count = 0;
    /* The cached DXIL was allocated by whichever module loaded it (possibly another CRT heap): it
     * is left allocated (at most BOOT_RENDER_MAX_SHADERS blobs, once per module load). */
    r->shader_count = 0;   /* the overflow table (boot_render_shader_extra) empties with it */
    {   D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1; memset(&o1, 0, sizeof o1);
        ID3D12Device_CheckFeatureSupport(r->gpu->device, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "render device: %ls, wave lanes %u..%u\n", r->gpu->adapter.Description, o1.WaveLaneCountMin, o1.WaveLaneCountMax); fclose(lf); } }
    boot_render_layout_current = 1;
    return 1;
}

/* ---------------------------------------------------------------- guest memory */
/* Guest ranges that executed GPU render-target write-back/reload lists read or write, until their
 * fence (boot_render_gt_run): CPU accesses overlapping one wait for it. */
#ifndef BOOT_RENDER_GT_PENDING
#define BOOT_RENDER_GT_PENDING 512   /* play: was 64; a full table waited for the oldest list */
#endif
static struct { uint64_t va, end, fence; } boot_render_gt_pending[BOOT_RENDER_GT_PENDING]; static unsigned boot_render_gt_npending;
/* play (42): highest fence of a pending tiling list per 64 KiB chunk of guest VA (hashed: a collision only
 * sends a guard to the full path), and the last completed fence value read */
#define BOOT_RENDER_GT_CHUNK_TABLE 65536u
static uint64_t boot_render_gt_chunk_fence[BOOT_RENDER_GT_CHUNK_TABLE], boot_render_gt_done_seen, boot_render_gt_fast, boot_render_gt_slow;
static uint64_t boot_render_gt_newest, boot_render_gt_gen;   /* data-snapshot v2fix: the newest pending list's fence; moves with every one (boot_ds_gt_state) */
static inline void boot_render_gt_guard(BootRender *r, uint64_t va, uint64_t size);
/* Guest GPU VA -> host pointer through the page table. The range must be contiguous within one
 * region (NVMap objects are contiguous guest allocations), otherwise NULL. */
static inline uint8_t *boot_render_guest(BootRender *r, uint64_t va, uint64_t size) {
    if (boot_render_gt_npending) boot_render_gt_guard(r, va, size);   /* GPU writes to it land first */
    if ((va >> 12) < r->va_base_page) return NULL;
    uint64_t page = (va >> 12) - r->va_base_page;
    if (page >= r->ptable_pages) return NULL;
    uint32_t e = r->ptable_cpu[page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count) return NULL;
    unsigned region = e >> 24; uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    if (off > r->region_size[region] || size > r->region_size[region] - off) return NULL;
    uint64_t last = size ? ((va & 0xfff) + size - 1) >> 12 : 0;
    for (uint64_t p = 1; p <= last; ++p)
        if (page + p >= r->ptable_pages || r->ptable_cpu[page + p] != e + (uint32_t)p) return NULL;
    return r->region_cpu[region] + off;
}
/* Maps guest GPU pages [va, va+size) onto region `region` starting at byte `offset` (4 KiB granular). */
static inline int boot_render_map(BootRender *r, uint64_t va, unsigned region, uint64_t offset, uint64_t size) {
    if ((va & 0xfff) || (offset & 0xfff) || region >= r->region_count || offset + size > r->region_size[region]) return 0;
    ++boot_render_cb_scope; ++boot_render_map_gen;   /* snapshots were taken through the previous mapping */
    for (uint64_t p = 0; p < (size + 0xfff) >> 12; ++p) {
        uint64_t page = (va >> 12) + p;
        if (page < r->va_base_page || page - r->va_base_page >= r->ptable_pages) return 0;
        {   uint64_t chunk = (page - r->va_base_page) >> BOOT_RENDER_PT_CHUNK_SHIFT;   /* play (74) */
            if (chunk < sizeof boot_render_pt_dirty) boot_render_pt_dirty[chunk] = 1; else boot_render_pt_dirty_for = NULL;
            if (chunk < sizeof boot_render_pt_ever) boot_render_pt_ever[chunk] = 1; }   /* t2-B */
        r->ptable_cpu[page - r->va_base_page] = boot_render_pt_gpu[page - r->va_base_page] = (region << 24) | (uint32_t)((offset >> 12) + p);
    }
    return 1;
}

/* (Re)creates the page table covering guest GPU VA [lo, hi); all entries unmapped. */
static inline int boot_render_ptable(BootRender *r, uint64_t lo, uint64_t hi) {
    uint32_t base = (uint32_t)(lo >> 12), pages = (uint32_t)(((hi + 0xfff) >> 12) - base);
    if (!pages || pages > (1u << 26)) return boot_render_fail(r, 0, "GPU VA span too large for the page table");
    int clear_all = 0;   /* play (74) */
    if (pages != r->ptable_pages || !r->ptable) {
        clear_all = 1;
        if (r->recording && !boot_render_submit(r)) return 0;
        if (r->ptable) ID3D12Resource_Release(r->ptable);
        r->ptable = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, (uint64_t)pages * 4, D3D12_RESOURCE_STATE_GENERIC_READ, (void **)&boot_render_pt_gpu);
        r->ptable_cpu = boot_render_pt_shadow(r->ptable_cpu, pages);
        if (!r->ptable || !r->ptable_cpu) return boot_render_fail(r, 0, "page table");
        D3D12_SHADER_RESOURCE_VIEW_DESC pt = {0};
        pt.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; pt.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pt.Format = DXGI_FORMAT_UNKNOWN; pt.Buffer.NumElements = pages; pt.Buffer.StructureByteStride = 4;
        ID3D12Device_CreateShaderResourceView(r->gpu->device, r->ptable, &pt, boot_render_cpu(r, r->srv_heap, r->srv_inc, 0));
    } else if (r->recording && !boot_render_submit(r)) return 0;
    r->va_base_page = base; r->ptable_pages = pages;
    static int clear_mode = -1;   /* TOTK_PT_CLEAR=full: clear the whole table as before; =verify: partial clear, then scan the table and log any entry left mapped */
    if (clear_mode < 0) { char v[16]; DWORD k = GetEnvironmentVariableA("TOTK_PT_CLEAR", v, sizeof v); clear_mode = (k && k < sizeof v) ? (v[0] == 'f' ? 1 : v[0] == 'v' ? 2 : 0) : 0; }
    if (clear_all || clear_mode == 1 || boot_render_pt_dirty_for != r->ptable_cpu || boot_render_pt_dirty_pages != pages) {
        memset(r->ptable_cpu, 0xff, (size_t)pages * 4); memset(boot_render_pt_gpu, 0xff, (size_t)pages * 4);
        memset(boot_render_pt_dirty, 0, sizeof boot_render_pt_dirty);
        boot_render_pt_dirty_for = r->ptable_cpu; boot_render_pt_dirty_pages = pages; ++boot_render_pt_full_clears;
    } else {
        const uint32_t chunks = (pages + (1u << BOOT_RENDER_PT_CHUNK_SHIFT) - 1) >> BOOT_RENDER_PT_CHUNK_SHIFT;
        for (uint32_t c = 0; c < chunks; ++c) if (boot_render_pt_dirty[c]) {
            uint32_t first = c << BOOT_RENDER_PT_CHUNK_SHIFT, n = pages - first < (1u << BOOT_RENDER_PT_CHUNK_SHIFT) ? pages - first : (1u << BOOT_RENDER_PT_CHUNK_SHIFT);
            memset(r->ptable_cpu + first, 0xff, (size_t)n * 4); memset(boot_render_pt_gpu + first, 0xff, (size_t)n * 4);
            boot_render_pt_dirty[c] = 0;
        }
        ++boot_render_pt_partial_clears;
        if (clear_mode == 2) {
            uint64_t stale = 0; for (uint32_t i = 0; i < pages; ++i) if (r->ptable_cpu[i] != 0xffffffffu || boot_render_pt_gpu[i] != 0xffffffffu) ++stale;
            if (stale) { FILE *lf = fopen("draw-path.log", "a"); if (lf) { fprintf(lf, "page table partial clear left %llu entries mapped (BUG)\n", (unsigned long long)stale); fclose(lf); } }
        }
    }
    ++boot_render_cb_scope; ++boot_render_map_gen;
    return 1;
}
/* Region index for host memory `cpu` (start of a fully committed VirtualAlloc allocation), registering
 * it or re-registering it when it has grown. Returns -1 on failure. */
/* Regions referenced by the current submission's page table (cleared per submission by
 * boot_draw_memory). The table holds BOOT_RENDER_MAX_REGIONS (the shader root signature's
 * size); when it is full, a region no current GPU mapping uses gives up its slot. Kept
 * outside BootRender so the live window's render object keeps its layout. */
static uint8_t boot_render_region_used[BOOT_RENDER_MAX_REGIONS];
static uint64_t boot_render_evicted_regions;
/* Executed work may still read a region (the native NVN path executes submissions without waiting):
 * wait for the queue before a region's view is released. */
static inline int boot_render_gpu_quiet(BootRender *r) {
    BootGpu *g = r->gpu;
    if (r->recording && !boot_render_submit(r)) return 0;
    if (!g || !g->fence || ID3D12Fence_GetCompletedValue(g->fence) >= g->submitted) return 1;
    return boot_gpu_wait(g);
}
static inline int boot_render_region_slot(BootRender *r, void *cpu, uint64_t size) {
    if (r->region_count < BOOT_RENDER_MAX_REGIONS) return boot_render_add_region(r, cpu, size);
    unsigned j = 0;
    while (j < r->region_count && r->region_cpu[j]) ++j;
    if (j == r->region_count) { j = 0; while (j < r->region_count && boot_render_region_used[j]) ++j; }
    if (j == r->region_count) { boot_render_fail(r, 0, "region table full (all regions used by this submission)"); return -1; }
    if (!boot_render_gpu_quiet(r)) return -1;
    if (r->region[j]) ID3D12Resource_Release(r->region[j]);
    if (r->region_heap[j]) ID3D12Heap_Release(r->region_heap[j]);
    r->region[j] = NULL; r->region_heap[j] = NULL; r->region_cpu[j] = NULL; r->region_size[j] = 0;
    ++boot_render_evicted_regions;
    unsigned saved = r->region_count; r->region_count = j;
    int idx = boot_render_add_region(r, cpu, size);
    r->region_count = saved;
    return idx;
}
/* Releases the zero-copy view of host block `cpu` (the caller frees the block afterwards; the GPU
 * is idle between submissions). */
static inline void boot_render_forget(BootRender *r, void *cpu) {
    if (!r) return;
    for (unsigned i = 0; i < r->region_count; ++i) if (r->region_cpu[i] == cpu) {
        boot_render_gpu_quiet(r);
        ID3D12Resource_Release(r->region[i]); ID3D12Heap_Release(r->region_heap[i]);
        r->region[i] = NULL; r->region_heap[i] = NULL; r->region_cpu[i] = NULL; r->region_size[i] = 0;
        boot_render_region_used[i] = 0;
    }
}
static inline int boot_render_region(BootRender *r, void *cpu, uint64_t size) {
    int found = -1;
    for (unsigned i = 0; i < r->region_count && found < 0; ++i) {
        if (r->region_cpu[i] != cpu) continue;
        if (r->region_size[i] >= size) found = (int)i;
        else {
            /* grown heap: recreate the view over the larger committed size */
            if (!boot_render_gpu_quiet(r)) return -1;
            ID3D12Resource_Release(r->region[i]); ID3D12Heap_Release(r->region_heap[i]);
            unsigned saved = r->region_count; r->region_count = i;
            found = boot_render_add_region(r, cpu, size);
            r->region_count = saved;
            if (found < 0) return -1;
        }
    }
    if (found < 0) found = boot_render_region_slot(r, cpu, size);
    if (found >= 0) boot_render_region_used[found] = 1;
    return found;
}
/* gpu-resident (TOTK_GPU_RESIDENT; play-work/gpu-resident/README.md): zero-copy GPU views of only the guest memory the GPU
 * maps. OpenExistingHeapFromAddress makes every page of the allocation it opens resident and locks it for the heap's lifetime,
 * touched or not (probe, RTX 4070 Laptop: a 512 MiB SEC_COMMIT view nobody had touched: +512 MiB non-local, -498 MiB available
 * RAM at once; trimming the working set drops the locked pages from it, but they stay in RAM until the heap is released), and
 * it takes only an allocation base (an interior address: E_INVALIDARG). Opening whole blocks locked the 3 GiB heap view (the
 * heap's maximum, more than its size) and every 256 MiB pool arena (their free ranges too): 4.6 GiB of play-99's 5.5 GiB
 * non-local. The native path (boot_nvn_exec_memory) instead maps a second view of just the GPU-mapped span of a fastmem
 * section (MapViewOfFile at a 64 KiB aligned section offset, any 4 KiB multiple in size) and opens that: only the span is
 * locked (a 64 MiB view of a 1 GiB section: +64 MiB non-local, the other pages untouched). The view shares the section's
 * pages with the guest and host views (GPU reads of CPU writes and CPU reads of GPU writes matched, both ways) and keeps the
 * section alive by itself. Cost: ~0.34 ms per MiB to open (page locking), ~0.07 ms per MiB to release, ~0.5 ms per heap;
 * 256 heaps alive changed a 256-copy submission by 0.01-0.14 ms. Views are recorded here by region slot. boot_render_region and
 * boot_render_forget never match them (their key, a block address, is never a view address). */
typedef struct BootGrView { const BootRender *r; ID3D12Resource *res; uint8_t *cpu; uint64_t lo, size, gen; uint32_t stale, pad; } BootGrView;
static BootGrView boot_gr_view[BOOT_RENDER_MAX_REGIONS];   /* by region slot; cpu != NULL: the slot holds a view this module mapped */
static uint8_t boot_gr_hole[BOOT_RENDER_MAX_REGIONS];      /* slots released at an idle page-table rebuild: no recorded work names them */
static uint64_t boot_gr_vgen = 1;                          /* bumped at every change of the view table (assignment cache key) */
static struct {
    uint64_t opened, opened_bytes, open_qpc, released, released_bytes, wholes_released, wholes_released_bytes, reused, fallbacks,
             failures, dropped, stale, refreshes, coarsened, tex_views_dropped, rebuilds, reclaims;
    /* at the last page-table rebuild */
    uint32_t mode, views, wholes, mappings, deferred, sections, spans, budget;
    uint64_t granule, view_bytes, whole_bytes, resident_bytes, mapped_bytes, today_bytes, would_bytes, stale_bytes;
} boot_gr_st;
/* TOTK_GPU_RESIDENT: 1 (default) = views; 0 = every GPU-mapped block opened whole, exactly as before (no perf3 part either);
 * 2 = dry run: blocks opened whole as with 0, and the perf3 part says what 1 would open. */
static inline int boot_gr_mode(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_GPU_RESIDENT", e, sizeof e); v = (n && n < sizeof e) ? (e[0] == '0' ? 0 : e[0] == '2' ? 2 : 1) : 1; }
    return v;
}
/* TOTK_GPU_RESIDENT_KB: smallest span granule (rounded to 64 KiB, default 2048): mappings closer than this share a view */
static inline uint64_t boot_gr_granule(void) {
    static uint64_t v;
    if (!v) {
        char e[16]; DWORD n = GetEnvironmentVariableA("TOTK_GPU_RESIDENT_KB", e, sizeof e);
        uint64_t k = (n && n < sizeof e) ? strtoull(e, NULL, 10) : 2048;
        k = k < 64 ? 64 : k > (UINT64_C(1) << 22) ? (UINT64_C(1) << 22) : k;
        v = ((k + 63) / 64 * 64) << 10;
    }
    return v;
}
static inline void boot_gr_tex_forget(BootRender *r, ID3D12Resource *res);   /* with the texture slots below */
/* Maps section bytes [lo, lo + size) (lo 64 KiB aligned) as a view and opens it as a region in a free slot; slot or -1. */
static inline int boot_gr_view_open(BootRender *r, HANDLE section, uint64_t lo, uint64_t size, uint64_t gen) {
    unsigned j = 0;
    while (j < r->region_count && !(boot_gr_hole[j] && !r->region_cpu[j] && !r->region[j] && !r->region_heap[j])) ++j;
    if (j == BOOT_RENDER_MAX_REGIONS) { ++boot_gr_st.failures; return -1; }
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    uint8_t *v = (uint8_t *)MapViewOfFile(section, FILE_MAP_ALL_ACCESS, (DWORD)(lo >> 32), (DWORD)lo, (SIZE_T)size);
    if (!v) { ++boot_gr_st.failures; return -1; }
    ID3D12Device3 *d3 = NULL; ID3D12Heap *heap = NULL; ID3D12Resource *res = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(r->gpu->device, &IID_ID3D12Device3, (void **)&d3))) {
        if (FAILED(ID3D12Device3_OpenExistingHeapFromAddress(d3, v, &IID_ID3D12Heap, (void **)&heap))) heap = NULL;
        ID3D12Device3_Release(d3);
    }
    if (heap) {
        D3D12_RESOURCE_DESC d = {0};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(ID3D12Device_CreatePlacedResource(r->gpu->device, heap, 0, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&res))) res = NULL;
    }
    if (!res) { if (heap) ID3D12Heap_Release(heap); UnmapViewOfFile(v); ++boot_gr_st.failures; return -1; }
    ++boot_render_created_regions;
    r->region_heap[j] = heap; r->region[j] = res; r->region_cpu[j] = v; r->region_size[j] = size;
    boot_render_region_views(r, j);
    if (j == r->region_count) ++r->region_count;
    boot_gr_hole[j] = 0; boot_render_region_used[j] = 1;
    boot_gr_view[j].r = r; boot_gr_view[j].res = res; boot_gr_view[j].cpu = v; boot_gr_view[j].lo = lo; boot_gr_view[j].size = size;
    boot_gr_view[j].gen = gen; boot_gr_view[j].stale = 0;
    ++boot_gr_vgen;
    QueryPerformanceCounter(&t1);
    ++boot_gr_st.opened; boot_gr_st.opened_bytes += size; boot_gr_st.open_qpc += (uint64_t)(t1.QuadPart - t0.QuadPart);
    if (boot_gr_st.opened <= 64) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "gpu-resident: view opened in slot %u: section gen %llu bytes [%llx, %llx) (%llu KiB) in %.2f ms\n", j, (unsigned long long)gen,
                          (unsigned long long)lo, (unsigned long long)(lo + size), (unsigned long long)(size >> 10), (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart); fclose(lf); }
    }
    return (int)j;
}
/* Releases region slot j (a view of this module, or a whole-block region nothing uses); the queue is idle. */
static inline void boot_gr_slot_release(BootRender *r, unsigned j) {
    BootGrView *v = &boot_gr_view[j];
    const int view = v->cpu && v->r == r && r->region_cpu[j] == v->cpu;
    if (r->region[j]) { boot_gr_tex_forget(r, r->region[j]); ID3D12Resource_Release(r->region[j]); }
    if (r->region_heap[j]) ID3D12Heap_Release(r->region_heap[j]);
    if (view) { ++boot_gr_st.released; boot_gr_st.released_bytes += r->region_size[j]; UnmapViewOfFile(v->cpu); }
    else { ++boot_gr_st.wholes_released; boot_gr_st.wholes_released_bytes += r->region_size[j]; }
    r->region[j] = NULL; r->region_heap[j] = NULL; r->region_cpu[j] = NULL; r->region_size[j] = 0;
    boot_render_region_views(r, j);   /* null views */
    boot_render_region_used[j] = 0;
    memset(v, 0, sizeof *v); boot_gr_hole[j] = 1; ++boot_gr_vgen;
}
/* Forgets entries whose slot was changed elsewhere (boot_render_region_slot eviction, a replaced render object). */
static inline void boot_gr_check(BootRender *r) {
    static const BootRender *last;
    if (last != r) { memset(boot_gr_hole, 0, sizeof boot_gr_hole); last = r; }   /* holes are known only for the render object they were made in */
    for (unsigned j = 0; j < BOOT_RENDER_MAX_REGIONS; ++j) {
        BootGrView *v = &boot_gr_view[j];
        if (!v->cpu) continue;
        if (v->r == r && j < r->region_count && r->region_cpu[j] == v->cpu && r->region[j] == v->res) continue;
        if (v->r == r) UnmapViewOfFile(v->cpu);   /* released by its evictor; the mapping is this module's */
        memset(v, 0, sizeof *v); ++boot_gr_st.dropped; ++boot_gr_vgen;   /* another render object: its objects went with it */
    }
}
/* perf3 part (TOTK_GPU_RESIDENT 1 or 2, after the first page-table rebuild of the native path). */
static inline void boot_gr_log(FILE *f) {
    if (!boot_gr_mode() || !boot_gr_st.rebuilds) return;
    LARGE_INTEGER q; QueryPerformanceFrequency(&q);
    if (boot_gr_mode() == 2)
        fprintf(f, "; guest heaps (TOTK_GPU_RESIDENT 2, dry run, granule %llu KiB): regions %u (whole blocks %u, %llu MiB opened, %llu MiB resident), GPU-mapped %llu MiB "
                   "(%u mappings, %u in %u fastmem sections), views would open %u spans %llu MiB",
                (unsigned long long)(boot_gr_st.granule >> 10), boot_gr_st.wholes, boot_gr_st.wholes, (unsigned long long)(boot_gr_st.whole_bytes >> 20),
                (unsigned long long)(boot_gr_st.resident_bytes >> 20), (unsigned long long)(boot_gr_st.mapped_bytes >> 20), boot_gr_st.mappings,
                boot_gr_st.deferred, boot_gr_st.sections, boot_gr_st.spans, (unsigned long long)(boot_gr_st.would_bytes >> 20));
    else
        fprintf(f, "; guest heaps (TOTK_GPU_RESIDENT 1, granule %llu KiB): regions %u (views %u %llu MiB, whole blocks %u %llu MiB), opened %llu MiB, resident %llu MiB, "
                   "GPU-mapped %llu MiB (%u mappings, %u in %u fastmem sections, %u spans, budget %u), whole-block opening would pin %llu MiB; "
                   "views opened %llu (%llu MiB, %.0f ms), released %llu (%llu MiB), whole blocks released %llu (%llu MiB), reused %llu, fallbacks %llu, failures %llu, "
                   "dropped %llu, stale %llu (%llu MiB alive), span gaps bridged %llu, texture views dropped %llu, section refreshes %llu, table reclaims %llu",
                (unsigned long long)(boot_gr_st.granule >> 10), boot_gr_st.views + boot_gr_st.wholes, boot_gr_st.views, (unsigned long long)(boot_gr_st.view_bytes >> 20),
                boot_gr_st.wholes, (unsigned long long)(boot_gr_st.whole_bytes >> 20), (unsigned long long)((boot_gr_st.view_bytes + boot_gr_st.whole_bytes) >> 20),
                (unsigned long long)(boot_gr_st.resident_bytes >> 20), (unsigned long long)(boot_gr_st.mapped_bytes >> 20), boot_gr_st.mappings, boot_gr_st.deferred,
                boot_gr_st.sections, boot_gr_st.spans, boot_gr_st.budget, (unsigned long long)(boot_gr_st.today_bytes >> 20),
                (unsigned long long)boot_gr_st.opened, (unsigned long long)(boot_gr_st.opened_bytes >> 20), (double)boot_gr_st.open_qpc * 1000.0 / (double)q.QuadPart,
                (unsigned long long)boot_gr_st.released, (unsigned long long)(boot_gr_st.released_bytes >> 20), (unsigned long long)boot_gr_st.wholes_released,
                (unsigned long long)(boot_gr_st.wholes_released_bytes >> 20), (unsigned long long)boot_gr_st.reused, (unsigned long long)boot_gr_st.fallbacks,
                (unsigned long long)boot_gr_st.failures, (unsigned long long)boot_gr_st.dropped, (unsigned long long)boot_gr_st.stale,
                (unsigned long long)(boot_gr_st.stale_bytes >> 20), (unsigned long long)boot_gr_st.coarsened,
                (unsigned long long)boot_gr_st.tex_views_dropped, (unsigned long long)boot_gr_st.refreshes, (unsigned long long)boot_gr_st.reclaims);
}
/* ---------------------------------------------------------------- shaders */
/* batch-b: n-byte equality 8 bytes at a time, and a zero fill as one rep stosb (the memcmp / memset the DLL links work
 * one byte per iteration); same results as !memcmp / memset. TOTK_FAST_BYTES=0: memcmp / memset. */
static inline int boot_render_fast_bytes(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_FAST_BYTES", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
static inline int boot_render_memeq(const void *a, const void *b, size_t n) {
    if (!boot_render_fast_bytes()) return !memcmp(a, b, n);
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t x, y; __builtin_memcpy(&x, p + i, 8); __builtin_memcpy(&y, q + i, 8); if (x != y) return 0; }
    for (; i < n; ++i) if (p[i] != q[i]) return 0;
    return 1;
}
static inline void boot_render_zero(void *p, size_t n) {
    if (!boot_render_fast_bytes()) { memset(p, 0, n); return; }
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(0) : "memory");
}
static inline void boot_render_sha256(const void *data, size_t size, uint8_t out[32]) {
    /* opening the provider dominates hashing small inputs: one handle per module */
    static BCRYPT_ALG_HANDLE alg;
    if (!alg && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    BCryptHashData(h, (PUCHAR)data, (ULONG)size, 0);
    BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h);
}
/* Canonical id of the program at `va`: SPH (0x50) + code through the self-branch terminator. */
static const uint8_t *boot_render_last_program; static size_t boot_render_last_program_size;
/* Program id cache (by guest VA): the bytes an id was computed from are kept, and a lookup is a hit
 * only when guest memory at `va` still holds exactly those bytes contiguously, so the result equals
 * a fresh scan + hash. Guest code rewritten in place simply misses. */
#define BOOT_RENDER_PROGRAM_CACHE 1024
/* A hit compared in this submission (exec_id) through the same page table, with no host write to
 * its pages since (epoch) and no large write, is not compared again: the bytes cannot differ. */
static struct { uint64_t va; uint8_t *bytes; size_t size; uint8_t id[32]; uint64_t exec, map_gen, big, epoch; const uint8_t *at; } boot_render_program_cache[BOOT_RENDER_PROGRAM_CACHE];
static uint64_t boot_render_program_hits, boot_render_program_misses;
/* Writes the last identified program (SPH + code) to missing-shaders/<id>.bin. */
static inline void boot_render_dump_last_program(const uint8_t id[32], const char *stage) {
#if TOTK_PLAY   /* play build: only the offline manifest; a miss is logged and the draw skipped */
    (void)id; (void)stage; return;
#endif
    char path[160]; int k = snprintf(path, sizeof path, "missing-shaders/%s-", stage);
    for (int i = 0; i < 32; ++i) k += snprintf(path + k, sizeof path - k, "%02x", id[i]);
    snprintf(path + k, sizeof path - k, ".bin");
    {   WIN32_FILE_ATTRIBUTE_DATA a;   /* already dumped (every draw of the program asks again) */
        if (boot_render_last_program && GetFileAttributesExA(path, GetFileExInfoStandard, &a) && !a.nFileSizeHigh && a.nFileSizeLow == boot_render_last_program_size) return; }
    CreateDirectoryA("missing-shaders", NULL);
    FILE *f = fopen(path, "wb"); if (f && boot_render_last_program) fwrite(boot_render_last_program, 1, boot_render_last_program_size, f); if (f) fclose(f);
}
static inline int boot_render_program_id(BootRender *r, uint64_t va, uint8_t id[32]) {
    unsigned way = (unsigned)((va >> 6) ^ (va >> 17)) & (BOOT_RENDER_PROGRAM_CACHE - 1);
    {   const uint8_t *q;
        __typeof__(boot_render_program_cache[0]) *pc = &boot_render_program_cache[way];
        if (pc->bytes && pc->va == va && pc->exec == boot_render_exec_id && pc->map_gen == boot_render_map_gen && pc->big == boot_render_big_writes &&
            boot_render_cb_clean(va, pc->size, pc->epoch) && pc->at == boot_render_guest(r, va, 1)) {
            memcpy(id, pc->id, 32); ++boot_render_program_hits;
            boot_render_last_program = pc->bytes; boot_render_last_program_size = pc->size;
            return 1;
        }
        if (boot_render_program_cache[way].bytes && boot_render_program_cache[way].va == va &&
            (q = boot_render_guest(r, va, boot_render_program_cache[way].size > 0x80 ? boot_render_program_cache[way].size : 0x80)) != NULL &&
            boot_render_memeq(q, boot_render_program_cache[way].bytes, boot_render_program_cache[way].size)) {   /* batch-b */
            memcpy(id, boot_render_program_cache[way].id, 32); ++boot_render_program_hits; boot_ax_live(BOOT_AX_L_SHADER, boot_render_program_cache[way].size);   /* async-xlate: compared live */
            pc->exec = boot_render_exec_id; pc->map_gen = boot_render_map_gen; pc->big = boot_render_big_writes; pc->epoch = ++boot_render_cb_epoch; pc->at = q;
            boot_render_last_program = boot_render_program_cache[way].bytes; boot_render_last_program_size = boot_render_program_cache[way].size;
            return 1;
        } }
    ++boot_render_program_misses;
    uint8_t *p = boot_render_guest(r, va, 0x80);
    if (!p) return 0;
    /* Graphics programs carry the SPH at the start of the program region entry. */
    const uint8_t *sph = p, *code = p + 0x50;
    for (uint64_t i = 0; i < (1u << 20); ++i) {
        uint64_t addr = i * 8; if (addr % 32 == 0) continue;
        const uint8_t *wp = boot_render_guest(r, va + 0x50 + addr, 8);
        if (!wp) return 0;
        if (wp != code + addr) { /* code crosses into a non-contiguous page: not supported */ return 0; }
        uint64_t w; memcpy(&w, wp, 8);
        /* BRA with immediate target to itself (target = addr + 8 + sext24) or onto the bundle's
         * scheduling word just before it: the terminator as maxwell_hlsl.Program detects it. */
        if ((w & (UINT64_C(0xfff0) << 48 | 0x20 | (UINT64_C(0xf) << 16) | 0x1f)) == ((UINT64_C(0xe240) << 48) | (UINT64_C(0x7) << 16) | 0xf)) {
            int64_t off = (int64_t)((w >> 20) & 0xffffff); if (off & 0x800000) off -= 0x1000000;
            int64_t target = (int64_t)addr + 8 + off;
            if (target == (int64_t)addr || (target % 32 == 0 && target + 8 == (int64_t)addr)) {
                size_t n = 0x50 + (size_t)addr + 8;
                uint8_t *buf = malloc(n); if (!buf) return 0;
                memcpy(buf, sph, 0x50); memcpy(buf + 0x50, code, (size_t)addr + 8);
                boot_render_sha256(buf, n, id);
                /* keep the program's bytes (cache entry); a manifest miss dumps the last ones for AOT translation */
                free(boot_render_program_cache[way].bytes);
                boot_render_program_cache[way].va = va; boot_render_program_cache[way].bytes = buf; boot_render_program_cache[way].size = n;
                {   __typeof__(boot_render_program_cache[0]) *nc = &boot_render_program_cache[way];   /* just read: verified now */
                    nc->exec = boot_render_exec_id; nc->map_gen = boot_render_map_gen; nc->big = boot_render_big_writes; nc->epoch = ++boot_render_cb_epoch; nc->at = p; }
                memcpy(boot_render_program_cache[way].id, id, 32);
                boot_render_last_program = buf; boot_render_last_program_size = n; boot_ax_live(BOOT_AX_L_SHADER, n); return 1;   /* async-xlate: read live (by design) */
            }
        }
    }
    return 0;
}
static inline int boot_render_hex(const char *s, uint8_t out[32]) {
    for (int i = 0; i < 32; ++i) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0; out[i] = (uint8_t)v; }
    return 1;
}
/* Shader cache index (module-static, BootRender keeps its layout): open addressing over (id, stage,
 * vs mask) -> r->shaders index + 1, rebuilt when the cache was emptied (layout update) or belongs to
 * another render object. Manifest misses (untranslated programs) are remembered while the manifest
 * file keeps its size and write time, so draws of an untranslated program do not re-read the whole
 * manifest each time; a manifest updated by the AOT translator is read again. */
/* Shaders beyond BootRender's BOOT_RENDER_MAX_SHADERS (its layout is kept across modules) go to a
 * module-static overflow table: a long session loads more distinct vertex/pixel programs (effects,
 * UI) than 1024, and a full cache made every later program a permanent "not in manifest" skip.
 * Indices 0..MAX-1 are r->shaders, MAX.. the overflow table (boot_render_shader_at). */
#define BOOT_RENDER_EXTRA_SHADERS 7168
#define BOOT_RENDER_ALL_SHADERS (BOOT_RENDER_MAX_SHADERS + BOOT_RENDER_EXTRA_SHADERS)
static BootRenderShader boot_render_shader_extra[BOOT_RENDER_EXTRA_SHADERS]; static unsigned boot_render_shader_extra_count;
static inline unsigned boot_render_shader_total(const BootRender *r) { return r->shader_count + (r->shader_count == BOOT_RENDER_MAX_SHADERS ? boot_render_shader_extra_count : 0); }
static inline BootRenderShader *boot_render_shader_at(BootRender *r, unsigned i) { return i < BOOT_RENDER_MAX_SHADERS ? &r->shaders[i] : &boot_render_shader_extra[i - BOOT_RENDER_MAX_SHADERS]; }
static inline unsigned boot_render_shader_index_of(const BootRender *r, const BootRenderShader *s) {
    return (s >= r->shaders && s < r->shaders + BOOT_RENDER_MAX_SHADERS) ? (unsigned)(s - r->shaders) : BOOT_RENDER_MAX_SHADERS + (unsigned)(s - boot_render_shader_extra);
}
#define BOOT_RENDER_SHADER_HASH 16384
static uint16_t boot_render_shader_hash[BOOT_RENDER_SHADER_HASH]; static const BootRender *boot_render_shader_hash_owner; static unsigned boot_render_shader_hashed;
#define BOOT_RENDER_SHADER_MISSES 256
static struct { uint8_t id[32]; uint32_t stage, mask; } boot_render_shader_miss[BOOT_RENDER_SHADER_MISSES];
static unsigned boot_render_shader_miss_count; static uint64_t boot_render_manifest_stamp, boot_render_shader_miss_hits;
static inline unsigned boot_render_shader_slot(const uint8_t id[32], uint32_t stage, uint32_t vs_mask) {
    uint64_t h; memcpy(&h, id, 8); h ^= (uint64_t)(stage + 1) * UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)vs_mask * UINT64_C(0xc2b2ae3d27d4eb4f);
    return (unsigned)(h ^ (h >> 29)) & (BOOT_RENDER_SHADER_HASH - 1);
}
static inline int boot_render_shader_is(const BootRenderShader *s, const uint8_t id[32], uint32_t stage, uint32_t vs_mask) {
    return s->stage == stage && (stage == 0 || s->vs_mask == vs_mask) && !memcmp(s->id, id, 32);
}
static inline void boot_render_shader_index(BootRender *r, unsigned i) {
    const BootRenderShader *s = boot_render_shader_at(r, i);
    unsigned k = boot_render_shader_slot(s->id, s->stage, s->stage ? s->vs_mask : 0);
    while (boot_render_shader_hash[k]) k = (k + 1) & (BOOT_RENDER_SHADER_HASH - 1);
    boot_render_shader_hash[k] = (uint16_t)(i + 1);
}
/* Size and write time of the manifest (0 when it cannot be read). */
static inline uint64_t boot_render_manifest_time(const BootRender *r) {
#if TOTK_PLAY   /* the manifest never changes while playing: misses stay remembered, no file polling */
    static uint64_t once; if (once) return once;
#endif
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(r->manifest_path, GetFileExInfoStandard, &a) &&
        !(!strcmp(r->manifest_path, "shader-manifest.tsv") && GetFileAttributesExA("startup\\shader-manifest.tsv", GetFileExInfoStandard, &a))) return 0;
    uint64_t stamp = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) ^ ((uint64_t)a.nFileSizeLow * UINT64_C(0x100000001b3)) ^ a.nFileSizeHigh;
#if TOTK_PLAY
    once = stamp ? stamp : 1;
#endif
    return stamp;
}
/* play (47): the manifest, parsed once. kind 'v' (any mask: the first line wins, as the scan did), 'p' (VS output
 * mask), 'c' (variant string). */
typedef struct { uint8_t id[32]; char kind; uint32_t mask; char *var, *tex, *path; } BootManifestEntry;
static struct { SRWLOCK lock; BootManifestEntry *e; uint32_t n, cap, *slot, slots; uint64_t stamp; int loaded; } boot_manifest = {SRWLOCK_INIT, NULL, 0, 0, NULL, 0, 0, 0};
static inline uint64_t boot_manifest_key(const uint8_t id[32], char kind, uint32_t mask, const char *var) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < 32; ++i) { h ^= id[i]; h *= UINT64_C(1099511628211); }
    h ^= (uint8_t)kind; h *= UINT64_C(1099511628211);
    if (kind == 'p') { h ^= mask; h *= UINT64_C(1099511628211); }
    if (kind == 'c' && var) for (const char *v = var; *v; ++v) { h ^= (uint8_t)*v; h *= UINT64_C(1099511628211); }
    return h;
}
static inline int boot_manifest_same(const BootManifestEntry *m, const uint8_t id[32], char kind, uint32_t mask, const char *var) {
    return m->kind == kind && !memcmp(m->id, id, 32) && (kind != 'p' || m->mask == mask) && (kind != 'c' || (var && m->var && !strcmp(m->var, var)));
}
static inline const char *boot_manifest_file(const char *manifest_path) {
    if (GetFileAttributesA(manifest_path) == INVALID_FILE_ATTRIBUTES && !strcmp(manifest_path, "shader-manifest.tsv")) return "startup\\shader-manifest.tsv";
    return manifest_path;
}
static inline uint64_t boot_manifest_stamp_of(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a; if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) ^ ((uint64_t)a.nFileSizeLow * UINT64_C(0x100000001b3)) ^ a.nFileSizeHigh;
}
static inline void boot_manifest_load_locked(const char *path) {
    for (uint32_t i = 0; i < boot_manifest.n; ++i) { free(boot_manifest.e[i].var); free(boot_manifest.e[i].tex); free(boot_manifest.e[i].path); }
    free(boot_manifest.slot); boot_manifest.slot = NULL; boot_manifest.slots = 0; boot_manifest.n = 0;
    boot_manifest.stamp = boot_manifest_stamp_of(path); boot_manifest.loaded = 1;
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        char hex[65], st[4], third[64], tex[512], p[900]; uint8_t id[32];
        if (sscanf(line, "%64s %3s %63s %511s %899[^\r\n]", hex, st, third, tex, p) != 5 || !boot_render_hex(hex, id)) continue;
        if (boot_manifest.n == boot_manifest.cap) {
            uint32_t c = boot_manifest.cap ? boot_manifest.cap * 2 : 32768; void *q = realloc(boot_manifest.e, (size_t)c * sizeof *boot_manifest.e);
            if (!q) break; boot_manifest.e = q; boot_manifest.cap = c;
        }
        BootManifestEntry *m = &boot_manifest.e[boot_manifest.n];
        memcpy(m->id, id, 32); m->kind = st[0] == 'v' ? 'v' : st[0] == 'c' ? 'c' : 'p';
        m->mask = third[0] == '-' ? 0 : (uint32_t)strtoul(third, NULL, 16);
        m->var = m->kind == 'c' ? _strdup(third) : NULL; m->tex = _strdup(tex); m->path = _strdup(p);
        if (!m->tex || !m->path || (m->kind == 'c' && !m->var)) { free(m->var); free(m->tex); free(m->path); break; }
        ++boot_manifest.n;
    }
    fclose(f);
    uint32_t slots = 1; while (slots < 2 * boot_manifest.n + 2) slots <<= 1;
    boot_manifest.slot = (uint32_t *)calloc(slots, 4); if (!boot_manifest.slot) return;
    boot_manifest.slots = slots;
    for (uint32_t i = 0; i < boot_manifest.n; ++i) {   /* the first line of a key wins */
        BootManifestEntry *m = &boot_manifest.e[i];
        uint32_t k = (uint32_t)boot_manifest_key(m->id, m->kind, m->mask, m->var) & (slots - 1); int dup = 0;
        for (; boot_manifest.slot[k]; k = (k + 1) & (slots - 1)) {
            BootManifestEntry *o = &boot_manifest.e[boot_manifest.slot[k] - 1];
            if (boot_manifest_same(o, m->id, m->kind, m->mask, m->var)) { dup = 1; break; }
        }
        if (!dup) boot_manifest.slot[k] = i + 1;
    }
}
/* water-work (2026-10-21): TOTK_SHADER_DENY (unset = off). Manifest rows it names are treated as absent, so those programs
 * miss and their draws / dispatches are skipped exactly as before the rows existed: the run-time A/B of a staged shader
 * install without editing shader-manifest.tsv (an edit would also drop the PSO cache, whose file carries the manifest's
 * content hash). Value: a comma list of <hex id prefix, 8-64 digits>[:<variant>] (variant = a pixel row's VS output mask in
 * hex, or a compute row's launch string such as 32x1x1s896l0; none = every row of that id; vertex rows ignore it), and
 * 'water' = the 7 programs play-work/water-work installs (the compute one only in its new 32x1x1s896l0 variant). The first
 * 32 denied lookups are logged to draw-path.log. Cost: lookups happen on shader-cache misses only; one parse per run. */
static inline int boot_manifest_denied(const uint8_t id[32], char kind, uint32_t mask, const char *var) {
    static volatile LONG state; static unsigned count, logged;
    static struct { uint8_t nib[64]; unsigned len; char var[24]; } deny[32];
    if (state != 2) {
        if (InterlockedCompareExchange(&state, 1, 0) == 0) {
            char e[1024]; DWORD k = GetEnvironmentVariableA("TOTK_SHADER_DENY", e, sizeof e);
            static const char water[] = "fffb1d80c0da1918:1,65455ad2f8ba3f19:1,7cf7f4c86641501d:1,7f194afcf0714900:0,"
                                        "3b084d77987ed6db,c33061b840068972,5dac95a2886b3669:32x1x1s896l0";
            char list[1400]; size_t o = 0; list[0] = 0;
            if (k && k < sizeof e)
                for (const char *t = e; *t; ) {   /* expand 'water' */
                    const char *end = strchr(t, ','); size_t n = end ? (size_t)(end - t) : strlen(t);
                    const char *add = (n == 5 && !_strnicmp(t, "water", 5)) ? water : t; size_t an = add == water ? sizeof water - 1 : n;
                    if (o + an + 2 < sizeof list) { if (o) list[o++] = ','; memcpy(list + o, add, an); o += an; list[o] = 0; }
                    t += n; if (*t == ',') ++t;
                }
            for (const char *t = list; *t && count < 32; ) {
                const char *end = strchr(t, ','); size_t n = end ? (size_t)(end - t) : strlen(t);
                unsigned len = 0; size_t j = 0;
                for (; j < n && len < 64; ++j) {
                    char ch = t[j]; int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                    if (v < 0) break;
                    deny[count].nib[len++] = (uint8_t)v;
                }
                deny[count].var[0] = 0;
                if (j < n && t[j] == ':') { size_t vn = n - j - 1; if (vn >= sizeof deny[count].var) vn = sizeof deny[count].var - 1; memcpy(deny[count].var, t + j + 1, vn); deny[count].var[vn] = 0; }
                if (len >= 8) { deny[count].len = len; ++count; }
                t += n; if (*t == ',') ++t;
            }
            if (count) { FILE *lf = fopen("draw-path.log", "a"); if (lf) { fprintf(lf, "shader deny (TOTK_SHADER_DENY): %u entries: %s\n", count, list); fclose(lf); } }
            MemoryBarrier(); state = 2;
        } else while (state != 2) Sleep(0);
    }
    for (unsigned d = 0; d < count; ++d) {
        unsigned i = 0;
        for (; i < deny[d].len; ++i) if (((id[i / 2] >> ((i & 1) ? 0 : 4)) & 15) != deny[d].nib[i]) break;
        if (i < deny[d].len) continue;
        if (deny[d].var[0] && kind == 'p' && strtoul(deny[d].var, NULL, 16) != mask) continue;
        if (deny[d].var[0] && kind == 'c' && (!var || strcmp(deny[d].var, var))) continue;
        if (logged < 32) {
            FILE *lf = fopen("draw-path.log", "a");
            if (lf) { fprintf(lf, "shader deny: %c %02x%02x%02x%02x%02x%02x%02x%02x %s treated as not in the manifest\n", kind, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
                              kind == 'c' && var ? var : ""); fclose(lf); ++logged; }
        }
        return 1;
    }
    return 0;
}
/* Copies the entry's texture list and DXIL path; 0 when the manifest has none. */
static inline int boot_manifest_find(const char *manifest_path, const uint8_t id[32], char kind, uint32_t mask, const char *var,
                                     char *tex, size_t tex_cap, char *path, size_t path_cap) {
    const char *file = boot_manifest_file(manifest_path);
    if (boot_manifest_denied(id, kind, mask, var)) return 0;   /* water-work: TOTK_SHADER_DENY (off when unset) */
    for (int attempt = 0; attempt < 2; ++attempt) {
        int found = 0;
        AcquireSRWLockShared(&boot_manifest.lock);
        if (boot_manifest.loaded && boot_manifest.slots)
            for (uint32_t k = (uint32_t)boot_manifest_key(id, kind, mask, var) & (boot_manifest.slots - 1); boot_manifest.slot[k]; k = (k + 1) & (boot_manifest.slots - 1)) {
                const BootManifestEntry *m = &boot_manifest.e[boot_manifest.slot[k] - 1];
                if (boot_manifest_same(m, id, kind, mask, var)) { snprintf(tex, tex_cap, "%s", m->tex); snprintf(path, path_cap, "%s", m->path); found = 1; break; }
            }
        int loaded = boot_manifest.loaded; uint64_t stamp = boot_manifest.stamp;
        ReleaseSRWLockShared(&boot_manifest.lock);
        if (found) return 1;
        if (loaded && boot_manifest_stamp_of(file) == stamp) return 0;   /* unchanged file: a real miss */
        AcquireSRWLockExclusive(&boot_manifest.lock);
        if (!boot_manifest.loaded || boot_manifest_stamp_of(file) != boot_manifest.stamp) boot_manifest_load_locked(file);
        ReleaseSRWLockExclusive(&boot_manifest.lock);
    }
    return 0;
}
/* shader-manifest.tsv: <canonical id hex>\t<vs|ps>\t<vs output mask hex or ->\t<dxil path> */
static inline BootRenderShader *boot_render_shader_load(BootRender *r, const uint8_t id[32], uint32_t stage, uint32_t vs_mask);
static inline BootRenderShader *boot_render_shader(BootRender *r, const uint8_t id[32], uint32_t stage, uint32_t vs_mask) {
    if (stage == 0) vs_mask = 0;
    if (r->shader_count < BOOT_RENDER_MAX_SHADERS) boot_render_shader_extra_count = 0;   /* cache emptied (layout update) */
    unsigned total = boot_render_shader_total(r);
    if (boot_render_shader_hash_owner != r || boot_render_shader_hashed > total) {
        memset(boot_render_shader_hash, 0, sizeof boot_render_shader_hash); boot_render_shader_hash_owner = r; boot_render_shader_hashed = 0; }
    while (boot_render_shader_hashed < total) boot_render_shader_index(r, boot_render_shader_hashed++);
    for (unsigned k = boot_render_shader_slot(id, stage, vs_mask); boot_render_shader_hash[k]; k = (k + 1) & (BOOT_RENDER_SHADER_HASH - 1)) {
        BootRenderShader *s = boot_render_shader_at(r, boot_render_shader_hash[k] - 1u);
        if (boot_render_shader_is(s, id, stage, vs_mask)) return s;
    }
    if (total == BOOT_RENDER_ALL_SHADERS) {
        static int logged; if (!logged++) { FILE *lf = fopen("draw-path.log", "a"); if (lf) { fprintf(lf, "shader cache full (%u programs): later programs are skipped\n", total); fclose(lf); } }
        return NULL; }
    uint64_t stamp = boot_render_manifest_time(r);
    if (stamp != boot_render_manifest_stamp) { boot_render_manifest_stamp = stamp; boot_render_shader_miss_count = 0; }
    for (unsigned i = 0; i < boot_render_shader_miss_count; ++i)
        if (boot_render_shader_miss[i].stage == stage && boot_render_shader_miss[i].mask == vs_mask && !memcmp(boot_render_shader_miss[i].id, id, 32)) {
            ++boot_render_shader_miss_hits; return NULL; }
    BootRenderShader *s = boot_render_shader_load(r, id, stage, vs_mask);
    if (s) { boot_render_shader_index(r, boot_render_shader_index_of(r, s)); boot_render_shader_hashed = boot_render_shader_total(r); }
    else if (stamp) {
        unsigned k = boot_render_shader_miss_count < BOOT_RENDER_SHADER_MISSES ? boot_render_shader_miss_count++ : (unsigned)(boot_render_shader_miss_hits % BOOT_RENDER_SHADER_MISSES);
        memcpy(boot_render_shader_miss[k].id, id, 32); boot_render_shader_miss[k].stage = stage; boot_render_shader_miss[k].mask = vs_mask;
    }
    return s;
}
/* batch-d (LDC.64 base-register aliasing: play-work/batch-d): the fixed DXIL replaced the affected manifest rows in place
 * and the previous files were kept in startup\shader-hlsl-pre-ldc (same names). TOTK_LDC_FIX=0 loads those instead
 * (A/B, off switch); a row without a kept file loads its manifest file as before. */
static inline void boot_render_ldc_fix_path(char *path, size_t cap) {
    static int off = -1;
    if (off < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_LDC_FIX", v, sizeof v); off = n && n < sizeof v && v[0] == '0'; }
    if (!off) return;
    static const char gis[] = "\\shader-hlsl-gis\\";
    char *at = strstr(path, gis); if (!at) return;
    char alt[900]; int k = snprintf(alt, sizeof alt, "%.*s\\shader-hlsl-pre-ldc\\%s", (int)(at - path), path, at + (sizeof gis - 1));
    if (k <= 0 || (size_t)k >= sizeof alt || (size_t)k >= cap) return;
    if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) memcpy(path, alt, (size_t)k + 1);
}
/* nxport gap batch 4 (p96: K2 instance ID, K4 IADD3.RS carry, shd-S2 TMML order, L3 ATOM after KIL; play-work/p96/
 * shader-batch): the retranslated DXIL replaced manifest files in place and swap.py kept the files before it in
 * startup\shader-pre-p96 (same relative paths). TOTK_SHADER_BATCH4=0 loads those instead (A/B, off switch); a row without
 * a kept file loads its manifest file as before. TOTK_LDC_FIX=0 (shader-hlsl-pre-ldc) wins over it. */
static inline void boot_render_batch4_path(char *path, size_t cap) {
    static int off = -1;
    if (off < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_SHADER_BATCH4", v, sizeof v); off = n && n < sizeof v && v[0] == '0'; }
    if (!off) return;
    const char *at = strstr(path, "\\shader-hlsl-gis\\"); if (!at) at = strstr(path, "\\shader-hlsl\\");
    if (!at) return;
    char alt[900]; int k = snprintf(alt, sizeof alt, "%.*s\\shader-pre-p96%s", (int)(at - path), path, at);
    if (k <= 0 || (size_t)k >= sizeof alt || (size_t)k >= cap) return;
    if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) memcpy(path, alt, (size_t)k + 1);
}
static inline BootRenderShader *boot_render_shader_load(BootRender *r, const uint8_t id[32], uint32_t stage, uint32_t vs_mask) {
    /* play (47): the entry from the manifest index (the probe's startup\shader-manifest.tsv fallback included) */
    char tex[512], path[900]; BootRenderShader *found = NULL;
    if (!boot_manifest_find(r->manifest_path, id, stage ? 'p' : 'v', vs_mask, NULL, tex, sizeof tex, path, sizeof path)) return NULL;
    boot_render_ldc_fix_path(path, sizeof path);   /* batch-d: TOTK_LDC_FIX=0 -> shader-hlsl-pre-ldc */
    boot_render_batch4_path(path, sizeof path);    /* nxport gap batch 4 (p96): TOTK_SHADER_BATCH4=0 -> shader-pre-p96 */
    do {
        FILE *b = fopen(path, "rb"); if (!b) break;
        fseek(b, 0, SEEK_END); long n = ftell(b); fseek(b, 0, SEEK_SET);
        void *data = malloc((size_t)n);
        if (data && fread(data, 1, (size_t)n, b) == (size_t)n) {
            found = r->shader_count < BOOT_RENDER_MAX_SHADERS ? &r->shaders[r->shader_count++] : &boot_render_shader_extra[boot_render_shader_extra_count++];
            memcpy(found->id, id, 32); found->stage = stage; found->vs_mask = vs_mask; found->dxil = data; found->size = (size_t)n;
            found->tex_count = 0;
            for (char *t = tex; *t && *t != '-' && found->tex_count < 64; ) {
                found->tex[found->tex_count++] = (uint32_t)strtoul(t, &t, 10);
                if (*t == ',') ++t;
            }
        } else free(data);
        fclose(b);
    } while (0);
    if (found && !stage && boot_sd_on()) boot_sd_vs_scan(id, path);   /* suspect-diag S2: what the vertex shader's .hlsl says */
    return found;
}

/* Guest-memory stores by graphics shaders. A translated vertex/pixel program that stores to guest
 * memory declares the space7 UAVs (GMEM_WRITABLE); its DXIL lists them in the PSV0 part's resource
 * bindings. boot_render_gmem_pending is set while work that may store (recorded or executing) has
 * not been waited for: a compute dispatch's CPU-side snapshots must wait for it, other work need not
 * (host_gpu_compute.h). */
static int boot_render_gmem_pending;
/* Native NVN path: guest ranges written by GPU copies (boot_render_gpu_write) since the queue was last
 * idle (overflow: all of guest memory), and a hook run after every waiting submission (queue idle). */
#define BOOT_RENDER_GW_RANGES 1024
/* fence: 0 while the write's list is open, else the queue fence value after which it has completed */
static struct { uint64_t va, size, fence; } boot_render_gw[BOOT_RENDER_GW_RANGES];
static unsigned boot_render_gw_count; static int boot_render_gw_all;
static void (*boot_render_idle_hook)(BootRender *r);
/* Stores recorded since the last UAV barrier (host_gpu_render_draw.h orders them before later readers). */
static int boot_render_uav_pending; static uint64_t boot_render_uav_barriers;
static uint8_t boot_render_shader_writes[BOOT_RENDER_ALL_SHADERS]; static const void *boot_render_shader_writes_of[BOOT_RENDER_ALL_SHADERS];
/* 1 when the DXIL container declares any UAV (or cannot be read), else 0. */
static inline int boot_render_dxil_writes(const void *dxil, size_t size) {
    const uint8_t *b = dxil; uint32_t n, off, cc, sz;
    if (!b || size < 32 || memcmp(b, "DXBC", 4)) return 1;
    memcpy(&n, b + 28, 4);
    if (n > 64 || 32 + (uint64_t)n * 4 > size) return 1;
    for (uint32_t i = 0; i < n; ++i) {
        memcpy(&off, b + 32 + 4 * i, 4);
        if ((uint64_t)off + 8 > size) return 1;
        memcpy(&cc, b + off, 4); memcpy(&sz, b + off + 4, 4);
        if (memcmp(&cc, "PSV0", 4)) continue;
        if ((uint64_t)off + 8 + sz > size) return 1;
        const uint8_t *d = b + off + 8; uint32_t info, count, entry;
        if (sz < 4) return 1;
        memcpy(&info, d, 4);
        if ((uint64_t)info + 8 > sz) return 1;
        memcpy(&count, d + 4 + info, 4);
        if (!count) return 0;
        memcpy(&entry, d + 8 + info, 4);
        if (entry < 16 || (uint64_t)info + 12 + (uint64_t)count * entry > sz) return 1;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t type; memcpy(&type, d + 12 + info + (size_t)k * entry, 4);
            if (type >= 6 && type <= 9) return 1;   /* PSVResourceType UAVTyped/Raw/Structured(+counter) */
        }
        return 0;
    }
    return 1;
}
static inline int boot_render_shader_stores(BootRender *r, const BootRenderShader *s) {
    if (!s) return 0;
    unsigned i = boot_render_shader_index_of(r, s);
    if (i >= BOOT_RENDER_ALL_SHADERS) return 1;
    if (!boot_render_shader_writes[i] || boot_render_shader_writes_of[i] != s->dxil) {
        boot_render_shader_writes[i] = (uint8_t)(1 + boot_render_dxil_writes(s->dxil, s->size)); boot_render_shader_writes_of[i] = s->dxil; }
    return boot_render_shader_writes[i] == 2;
}

/* ---------------------------------------------------------------- render targets */
/* NVB197_SET_COLOR_TARGET_FORMAT_V_* (open-gpu-doc clb197.h) -> DXGI. X (padding) channels use the
 * format with alpha: the guest never reads them. A2R10G10B10 has no DXGI equivalent. */
static inline int boot_render_ct_format(uint32_t ct, DXGI_FORMAT *out) {
    DXGI_FORMAT f;
    switch (ct) {
    case 0xc0: case 0xc3: f = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case 0xc1: case 0xc4: f = DXGI_FORMAT_R32G32B32A32_SINT; break;
    case 0xc2: case 0xc5: f = DXGI_FORMAT_R32G32B32A32_UINT; break;
    case 0xc6: f = DXGI_FORMAT_R16G16B16A16_UNORM; break;
    case 0xc7: f = DXGI_FORMAT_R16G16B16A16_SNORM; break;
    case 0xc8: f = DXGI_FORMAT_R16G16B16A16_SINT; break;
    case 0xc9: f = DXGI_FORMAT_R16G16B16A16_UINT; break;
    case 0xca: case 0xce: f = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case 0xcb: f = DXGI_FORMAT_R32G32_FLOAT; break;
    case 0xcc: f = DXGI_FORMAT_R32G32_SINT; break;
    case 0xcd: f = DXGI_FORMAT_R32G32_UINT; break;
    case 0xcf: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case 0xd0: f = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case 0xd1: f = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case 0xd2: f = DXGI_FORMAT_R10G10B10A2_UINT; break;
    case 0xd5: case 0xf9: f = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case 0xd6: case 0xfa: f = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case 0xd7: f = DXGI_FORMAT_R8G8B8A8_SNORM; break;
    case 0xd8: f = DXGI_FORMAT_R8G8B8A8_SINT; break;
    case 0xd9: f = DXGI_FORMAT_R8G8B8A8_UINT; break;
    case 0xda: f = DXGI_FORMAT_R16G16_UNORM; break;
    case 0xdb: f = DXGI_FORMAT_R16G16_SNORM; break;
    case 0xdc: f = DXGI_FORMAT_R16G16_SINT; break;
    case 0xdd: f = DXGI_FORMAT_R16G16_UINT; break;
    case 0xde: f = DXGI_FORMAT_R16G16_FLOAT; break;
    case 0xe0: f = DXGI_FORMAT_R11G11B10_FLOAT; break;
    case 0xe3: f = DXGI_FORMAT_R32_SINT; break;
    case 0xe4: f = DXGI_FORMAT_R32_UINT; break;
    case 0xe5: f = DXGI_FORMAT_R32_FLOAT; break;
    case 0xe6: f = DXGI_FORMAT_B8G8R8X8_UNORM; break;
    case 0xe7: f = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; break;
    case 0xe8: f = DXGI_FORMAT_B5G6R5_UNORM; break;
    case 0xe9: f = DXGI_FORMAT_B5G5R5A1_UNORM; break;
    case 0xea: f = DXGI_FORMAT_R8G8_UNORM; break;
    case 0xeb: f = DXGI_FORMAT_R8G8_SNORM; break;
    case 0xec: f = DXGI_FORMAT_R8G8_SINT; break;
    case 0xed: f = DXGI_FORMAT_R8G8_UINT; break;
    case 0xee: f = DXGI_FORMAT_R16_UNORM; break;
    case 0xef: f = DXGI_FORMAT_R16_SNORM; break;
    case 0xf0: f = DXGI_FORMAT_R16_SINT; break;
    case 0xf1: f = DXGI_FORMAT_R16_UINT; break;
    case 0xf2: f = DXGI_FORMAT_R16_FLOAT; break;
    case 0xf3: f = DXGI_FORMAT_R8_UNORM; break;
    case 0xf4: f = DXGI_FORMAT_R8_SNORM; break;
    case 0xf5: f = DXGI_FORMAT_R8_SINT; break;
    case 0xf6: f = DXGI_FORMAT_R8_UINT; break;
    default: return 0;
    }
    *out = f; return 1;
}
static inline uint32_t boot_render_dxgi_bytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G32_UINT: return 8;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: return 2;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_R8_UINT: return 1;
        default: return 4;
    }
}
/* Integer colour formats: bits per channel and signedness (0 bits = not an integer format). */
static inline uint32_t boot_render_dxgi_int_bits(DXGI_FORMAT f, int *is_signed) {
    *is_signed = 0;
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32_SINT: *is_signed = 1; return 32;
        case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32_UINT: return 32;
        case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16_SINT: *is_signed = 1; return 16;
        case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16_UINT: return 16;
        case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8_SINT: *is_signed = 1; return 8;
        case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8_UINT: return 8;
        case DXGI_FORMAT_R10G10B10A2_UINT: return 10;
        default: return 0;
    }
}
/* Staging rings: persistently mapped UPLOAD (texture and target loads) and READBACK (target
 * write-backs) buffers, sub-allocated instead of creating and destroying a committed buffer per
 * operation. Grow-only chunks; boot_render_submit waits for the queue, so the upload ring is
 * reset there. The readback ring is reset by its user at the start of each write-back batch (its
 * results are consumed right after the batch's submission). Chunks belong to this module's
 * device; boot_draw_device_recover drops them. */
static inline int boot_render_begin(BootRender *r);
#define BOOT_RENDER_STAGE_CHUNKS 8
#define BOOT_RENDER_STAGE_MIN    (UINT64_C(32) << 20)
typedef struct BootRenderStaging {
    ID3D12Resource *res[BOOT_RENDER_STAGE_CHUNKS]; uint8_t *cpu[BOOT_RENDER_STAGE_CHUNKS]; uint64_t size[BOOT_RENDER_STAGE_CHUNKS];
    unsigned count, cur, used; uint64_t off; ID3D12Device *device;
} BootRenderStaging;
static BootRenderStaging boot_render_up_ring, boot_render_rb_ring;
/* drain2-work: the upload ring (and the deferred releases below) recycled per executed list as its fence completes,
 * not only when the queue is idle (versioning removed most idle points: the ring filled and its user submitted and
 * waited). Chunk k of the upload ring remembers the list id of its last allocation (the list whose copies read it; a
 * kick also marks the chunk being filled for the next list). Recording moves to an existing chunk, or wraps to chunk 0
 * once all are allocated, only when that list completed; a chunk still read is skipped (a new one is made while there
 * are fewer than BOOT_RENDER_STAGE_CHUNKS), and only when no chunk is free does the caller submit and wait, as before
 * (counted). TOTK_LIST_RECYCLE=1 / 0; unset: as TOTK_SLOT_VERSIONING. */
static uint64_t boot_render_up_lid[BOOT_RENDER_STAGE_CHUNKS];
static uint64_t boot_render_up_recycles, boot_render_up_busy, boot_render_up_full_waits, boot_render_up_full_qpc;
static inline int boot_render_recycle_on(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_LIST_RECYCLE", e, sizeof e); v = (n && n < sizeof e) ? e[0] != '0' : boot_render_sv_on(); }
    return v;
}
/* 1 when nothing recorded in list `lid` can still run: 0 (none), executed and its fence reached, never executed (discarded)
 * or older than the fence table. The open list and a list not begun yet (a kick's mark) are not complete. */
static inline int boot_render_list_done(BootRender *r, uint64_t lid) {
    if (!lid) return 1;
    if (lid > boot_render_list_id || (lid == boot_render_list_id && r->recording)) return 0;
    if (boot_render_list_fence[lid & 65535].id != lid) return 1;
    return ID3D12Fence_GetCompletedValue(r->gpu->fence) >= boot_render_list_fence[lid & 65535].fence;
}
static inline void boot_render_staging_reset(BootRenderStaging *s) {
    s->cur = 0; s->off = 0; s->used = 0;
    if (s == &boot_render_up_ring) memset(boot_render_up_lid, 0, sizeof boot_render_up_lid);   /* drain2-work: idle, every list completed */
}
static inline void boot_render_staging_release(BootRenderStaging *s) {
    for (unsigned i = 0; i < s->count; ++i) if (s->res[i]) ID3D12Resource_Release(s->res[i]);
    memset(s, 0, sizeof *s);
}
/* `bytes` at a 512-byte aligned offset (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) of a chunk, or NULL
 * when every chunk is in use (the caller executes its recorded work and retries). */
static inline uint8_t *boot_render_staging_alloc(BootRender *r, BootRenderStaging *s, D3D12_HEAP_TYPE type, uint64_t bytes,
                                                 ID3D12Resource **res, uint64_t *offset) {
    if (s->device != r->gpu->device) { boot_render_staging_release(s); s->device = r->gpu->device; }
    if (!bytes) bytes = 1;
    const int up = s == &boot_render_up_ring, rec = up && boot_render_recycle_on(); int wrapped = 0;   /* drain2-work */
    for (;;) {
        if (s->cur < s->count) {
            uint64_t at = (s->off + 511) & ~UINT64_C(511);
            if (at <= s->size[s->cur] && bytes <= s->size[s->cur] - at) {
                s->off = at + bytes; s->used = 1; *res = s->res[s->cur]; *offset = at;
                if (up) boot_render_up_lid[s->cur] = boot_render_list_id;   /* drain2-work: the list whose copies read it */
                return s->cpu[s->cur] + at; }
            ++s->cur; s->off = 0;
            if (rec && s->cur < s->count && !boot_render_list_done(r, boot_render_up_lid[s->cur])) { s->cur = s->count; ++boot_render_up_busy; }   /* drain2-work: still read */
            continue;
        }
        if (rec && s->count == BOOT_RENDER_STAGE_CHUNKS && s->used && !wrapped && boot_render_list_done(r, boot_render_up_lid[0])) {
            wrapped = 1; s->cur = 0; s->off = 0; ++boot_render_up_recycles; continue; }   /* drain2-work: wrap over completed lists */
        if (s->count == BOOT_RENDER_STAGE_CHUNKS) {
            if (s->used) return NULL;
            /* idle and still too small for this request: start over with fresh chunks */
            ID3D12Device *device = s->device; boot_render_staging_release(s); s->device = device;
        }
        uint64_t size = bytes > BOOT_RENDER_STAGE_MIN ? (bytes + 0xffff) & ~UINT64_C(0xffff) : BOOT_RENDER_STAGE_MIN;
        void *map = NULL;
        ID3D12Resource *chunk = boot_render_buffer(r, type, size, type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, &map);
        if (!chunk) return NULL;
        if (up) boot_render_up_lid[s->count] = 0;   /* drain2-work */
        s->res[s->count] = chunk; s->cpu[s->count] = map; s->size[s->count] = size; s->cur = s->count++; s->off = 0;
    }
}
/* Upload staging for recorded copies (call with the command list open): executes the recorded
 * work to free the ring when it is full. */
static inline uint8_t *boot_render_upload_alloc(BootRender *r, uint64_t bytes, ID3D12Resource **res, uint64_t *offset) {
    uint8_t *p = boot_render_staging_alloc(r, &boot_render_up_ring, D3D12_HEAP_TYPE_UPLOAD, bytes, res, offset);
    if (!p && r->recording) {   /* drain2-work: counted (ring full: no chunk free of running lists) */
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a); ++boot_render_up_full_waits;
        if (boot_render_submit(r) && boot_render_begin(r)) p = boot_render_staging_alloc(r, &boot_render_up_ring, D3D12_HEAP_TYPE_UPLOAD, bytes, res, offset);
        QueryPerformanceCounter(&b); boot_render_up_full_qpc += (uint64_t)(b.QuadPart - a.QuadPart);
    }
    return p;
}

/* Pitch-linear colour targets (SET_COLOR_TARGET_MEMORY.LAYOUT = PITCH): the WIDTH register holds the
 * row pitch in bytes (Ryujinx RtColorState.WidthOrStride: width = pitch / bytes per pixel). They render into a host texture
 * like block-linear targets (cache format | 0x20000) and, because the source GPU writes them straight
 * to memory where the CPU, the copy engine and compute read them, are written back to guest memory
 * (boot_render_flush_pitch) before anything else reads guest memory and when a submission ends. */
#define BOOT_RENDER_PITCH_FLAG 0x20000u
/* Render target table. BootRender keeps its BOOT_RENDER_MAX_RT (64) entries only for its layout
 * (a live render object outlives rebuilt runtime DLLs); the targets live in this table instead:
 * BOOT_RENDER_RT_CAP entries, its own RTV/DSV heaps, LRU state and the per-target flags, in one
 * VirtualAlloc'd block registered process-wide (TOTK_RENDER_RT_TABLE) so a rebuilt module adopts
 * it with every target's contents and dirty state. A frame of the game uses more than 64 targets
 * (the 64-entry table evicted and reloaded ~80 targets per submission, each eviction a waiting
 * write-back); eviction now happens only when the table or the byte budget is full. */
#ifndef BOOT_RENDER_RT_CAP
#define BOOT_RENDER_RT_CAP 512
#endif
#ifndef BOOT_RENDER_RT_BUDGET   /* host texture bytes held by targets before LRU eviction (tests set a small one) */
#define BOOT_RENDER_RT_BUDGET (UINT64_C(1536) << 20)
#endif
typedef struct BootRenderRtInfo { uint8_t known, dirty, log_gob_h, log_gob_d, stale; uint64_t last_use, extent; } BootRenderRtInfo;
typedef struct BootRenderRtTable {
    char magic[8]; uint32_t table_size, cap, count, reserved;
    BootRender *owner; ID3D12Device *device; ID3D12DescriptorHeap *rtv, *dsv; UINT rtv_inc, dsv_inc;
    uint64_t bytes, clock, generation;   /* texture bytes held; LRU clock; bumped when an entry's texture changes */
    BootRenderTarget rts[BOOT_RENDER_RT_CAP]; BootRenderRtInfo info[BOOT_RENDER_RT_CAP];
    uint64_t tex_bytes[BOOT_RENDER_RT_CAP]; uint32_t pitch_pitch[BOOT_RENDER_RT_CAP]; uint8_t pitch_dirty[BOOT_RENDER_RT_CAP];
} BootRenderRtTable;
static BootRenderRtTable *boot_render_rtt;
static uint64_t boot_render_rt_peak, boot_render_rt_syncs, boot_render_rt_creates;
static uint64_t boot_render_rt_dirty_epoch = 1;   /* bumped whenever a target becomes dirty */
static inline BootRenderRtTable *boot_render_rt_bind(BootRender *r);
/* The target table of render object `r` (never NULL: a table that cannot be created has no room). */
static inline BootRenderRtTable *boot_render_rtab(BootRender *r) {
    BootRenderRtTable *t = boot_render_rtt;
    return (t && t->owner == r && t->device == r->gpu->device) ? t : boot_render_rt_bind(r);
}
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_rtv(BootRender *r, const BootRenderTarget *x) {
    BootRenderRtTable *t = boot_render_rtab(r); D3D12_CPU_DESCRIPTOR_HANDLE c = {0};
    if (t->rtv) { t->rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(t->rtv, &c); c.ptr += (SIZE_T)x->rtv * t->rtv_inc; }
    return c;
}
/* Single-plane depth targets (D32F, D16) are typeless resources, so shaders can sample them directly
 * (R32_FLOAT / R16_UNORM views, boot_render_texture) instead of a write-back to guest memory and a
 * texture built from it; the depth-stencil view names the depth format. */
static inline DXGI_FORMAT boot_render_depth_resource_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_TYPELESS : f == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_TYPELESS : f;
}
static inline DXGI_FORMAT boot_render_depth_srv_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : f == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_UNKNOWN;
}
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_dsv(BootRender *r, const BootRenderTarget *x);
static inline void boot_render_dsv_create(BootRender *r, const BootRenderTarget *x) {
    D3D12_DEPTH_STENCIL_VIEW_DESC v; memset(&v, 0, sizeof v);
    v.Format = x->dxgi; v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateDepthStencilView(r->gpu->device, x->texture, &v, boot_render_dsv(r, x));
}
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_dsv(BootRender *r, const BootRenderTarget *x) {
    BootRenderRtTable *t = boot_render_rtab(r); D3D12_CPU_DESCRIPTOR_HANDLE c = {0};
    if (t->dsv) { t->dsv->lpVtbl->GetCPUDescriptorHandleForHeapStart(t->dsv, &c); c.ptr += (SIZE_T)x->rtv * t->dsv_inc; }
    return c;
}
/* Releases every target and the heaps (device recovery, another render object); the table stays registered. */
static inline void boot_render_rt_table_clear(BootRenderRtTable *t) {
    for (unsigned i = 0; i < t->count; ++i) if (t->rts[i].texture) ID3D12Resource_Release(t->rts[i].texture);
    if (t->rtv) ID3D12DescriptorHeap_Release(t->rtv);
    if (t->dsv) ID3D12DescriptorHeap_Release(t->dsv);
    uint32_t size = t->table_size, cap = t->cap; uint64_t generation = t->generation;
    memset(t, 0, sizeof *t); memcpy(t->magic, "TOTKRT01", 8); t->table_size = size; t->cap = cap; t->generation = generation + 1;
}
static inline void boot_render_rt_release_all(void) { if (boot_render_rtt) boot_render_rt_table_clear(boot_render_rtt); }
static inline uint64_t boot_render_rt_bytes(const D3D12_RESOURCE_DESC *d);
static inline BootRenderRtTable *boot_render_rt_bind(BootRender *r) {
    static BootRenderRtTable none;   /* cap 0: no room (allocation failed) */
    char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_RENDER_RT_TABLE", v, sizeof v);
    BootRenderRtTable *t = (n && n < sizeof v) ? (BootRenderRtTable *)(uintptr_t)strtoull(v, NULL, 16) : NULL;
    /* another module's table of a different layout: not readable here (left as it is) */
    if (t && (memcmp(t->magic, "TOTKRT01", 8) || t->table_size != sizeof *t || t->cap != BOOT_RENDER_RT_CAP)) t = NULL;
    if (!t) {
        t = VirtualAlloc(NULL, sizeof *t, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!t) return &none;
        memcpy(t->magic, "TOTKRT01", 8); t->table_size = sizeof *t; t->cap = BOOT_RENDER_RT_CAP;
        snprintf(v, sizeof v, "%llx", (unsigned long long)(uintptr_t)t);
        SetEnvironmentVariableA("TOTK_RENDER_RT_TABLE", v);
    }
    boot_render_rtt = t;
    if (t->owner == r && t->device == r->gpu->device) return t;   /* adopted from the previous module */
    boot_render_rt_table_clear(t);   /* another render object's (or removed device's) targets */
    D3D12_DESCRIPTOR_HEAP_DESC rd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BOOT_RENDER_RT_CAP, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, BOOT_RENDER_RT_CAP, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(ID3D12Device_CreateDescriptorHeap(r->gpu->device, &rd, &IID_ID3D12DescriptorHeap, (void **)&t->rtv))) { t->rtv = NULL; return &none; }
    if (FAILED(ID3D12Device_CreateDescriptorHeap(r->gpu->device, &dd, &IID_ID3D12DescriptorHeap, (void **)&t->dsv))) {
        ID3D12DescriptorHeap_Release(t->rtv); t->rtv = NULL; t->dsv = NULL; return &none; }
    t->rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(r->gpu->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    t->dsv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(r->gpu->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    t->owner = r; t->device = r->gpu->device;
    /* targets of a module from before this table (kept in BootRender): moved here with new views */
    for (unsigned i = 0; i < r->rt_count && i < BOOT_RENDER_MAX_RT; ++i) {
        BootRenderTarget *o = &r->rts[i];
        if (!o->texture) continue;
        unsigned k = t->count++;
        t->rts[k] = *o; t->rts[k].rtv = k;
        D3D12_RESOURCE_DESC d; o->texture->lpVtbl->GetDesc(o->texture, &d); t->tex_bytes[k] = boot_render_rt_bytes(&d); t->bytes += t->tex_bytes[k];
        if (o->format & 0x10000u) boot_render_dsv_create(r, &t->rts[k]);
        else ID3D12Device_CreateRenderTargetView(r->gpu->device, o->texture, NULL, boot_render_rtv(r, &t->rts[k]));
    }
    memset(r->rts, 0, sizeof r->rts); r->rt_count = 0;
    ++t->generation;
    return t;
}
static uint64_t boot_render_pitch_flushes;
static inline int boot_render_begin(BootRender *r);
/* Uploads the guest image of a pitch-linear target into its host texture (first use per submission). */
static inline int boot_render_pitch_load(BootRender *r, BootRenderTarget *x, uint32_t pitch) {
    uint32_t bpp = boot_render_dxgi_bytes(x->dxgi);
    D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
    ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
    if (!boot_render_begin(r)) return 0;
    ID3D12Resource *upload = NULL; uint64_t base = 0; uint8_t *up = boot_render_upload_alloc(r, total, &upload, &base);
    if (!up) return 0;
    for (uint32_t y = 0; y < x->height; ++y) {
        const uint8_t *src = boot_render_guest(r, x->address + (uint64_t)y * pitch, (uint64_t)x->width * bpp);
        if (!src) return 0;
        memcpy(up + fp.Offset + (size_t)y * fp.Footprint.RowPitch, src, (size_t)x->width * bpp);
    }
    fp.Offset += base;
    D3D12_RESOURCE_STATES was = x->state;
    D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = x->texture;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = was; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    BOOT_GP_RB("rt load", r->cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, srcl = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
    srcl.PlacedFootprint = fp;
    BOOT_GP_CTR("rt load", r->cl, &dst, 0, 0, 0, &srcl, NULL);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = was;
    BOOT_GP_RB("rt load", r->cl, 1, &b);
    return 1;
}
/* Block-linear colour targets live in host textures; the source GPU writes them to guest memory,
 * where textures, the copy engine and other views read them. Per target (BootRenderRtTable.info, by
 * index): its guest block layout, whether the GPU wrote it since it was last written back, and LRU
 * state. A target's guest range is written back (boot_render_rt_writeback, CPU) before a texture or
 * copy reads memory overlapping it; a target that becomes dirty invalidates cached textures over its
 * range, so a texture sampled after further rendering is rebuilt from written-back memory. On
 * creation a block-linear target is loaded from guest memory (a draw may cover only part of it). A
 * target used while another target over the same memory (an aliased allocation) is dirty first takes
 * guest memory's image with that one written back (boot_render_rt_refresh), like a new target would.
 * The table is managed LRU within BOOT_RENDER_RT_CAP entries and BOOT_RENDER_RT_BUDGET bytes: the
 * least recently used target not bound by the current draw is written back and evicted. Write-backs
 * and reloads that only feed GPU work run on the GPU without a wait (boot_render_gt_run). */
static uint64_t boot_render_rt_writebacks, boot_render_rt_evictions, boot_render_rt_loads;
/* render-24, first stage of "GPU images are authoritative" (SuperMarioWonderRecomp: render targets are never written
 * back to guest memory; LASRecomp: render-target aliasing by explicit alias views). A host target owns its image:
 * it is loaded from guest memory when it is created and when something outside the 3D engine really wrote that
 * memory (copy engine, compute stores, CPU: boot_render_rt_guest_written -> stale), never because another host
 * target over the same memory was written back or drawn. TOTK reuses its transient memory for differently shaped
 * targets every frame (the title UI 50de00000.. at 95x17, 120x75, 285x52, 380x70, 480x300; the bloom chain; depth
 * 529a00000 at 64x64 / 1024x576 / 1280x720): each bind wrote the other shapes back ("overlap sync"), every write-back
 * marked the clean ones stale, and their next use (often a texture read after this frame's draws) replaced the
 * image with the other shapes' bytes. frames\flicker.log (play-39): every logged present reloads 40-80 targets
 * this way and the dark presents are those whose sampled targets were reverted. Dirty targets are still written back
 * where guest memory is read (guest-path textures, copies, CPU reads, evictions); those write-backs no longer revert
 * other targets. TOTK_RT_OVERLAP_RELOAD=1 restores the previous model (A/B). */
static inline int boot_render_rt_overlap_reload(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_RT_OVERLAP_RELOAD", e, sizeof e); v = n && n < sizeof e && e[0] == '1'; }
    return v;
}
static uint64_t boot_render_rt_retouched;   /* bound targets made dirty again after a write-back during their draw's texture setup */
static inline void boot_render_invalidate(uint64_t va, uint64_t size);
static inline void boot_render_invalidate_textures(uint64_t va, uint64_t size);
static inline int boot_render_rt_barrier_at(BootRender *r, BootRenderTarget *t, D3D12_RESOURCE_STATES to, const char *file, unsigned line);
#define boot_render_rt_barrier(r, t, to) boot_render_rt_barrier_at((r), (t), (to), __FILE__, __LINE__)   /* render-40: tagged with the caller */

static inline uint64_t boot_render_rt_extent(const BootRenderTarget *x, const BootRenderRtInfo *in) {
    return boot_texture_slice_extent(x->width * boot_render_dxgi_bytes(x->dxgi), x->height, in->log_gob_h, in->log_gob_d);
}
/* Initial contents of a new block-linear target: its guest image (left cleared when unmapped). */
static inline int boot_render_rt_load(BootRender *r, BootRenderTarget *x, unsigned log_gob_h, unsigned log_gob_d) {
    uint32_t wb = x->width * boot_render_dxgi_bytes(x->dxgi);
    uint64_t extent = boot_texture_slice_extent(wb, x->height, log_gob_h, log_gob_d);
    const uint8_t *src = extent ? boot_render_guest(r, x->address, extent) : NULL;
    if (!src) return 1;
    D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
    ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
    if (!boot_render_begin(r)) return 0;
    ID3D12Resource *upload = NULL; uint64_t base = 0; uint8_t *up = boot_render_upload_alloc(r, total, &upload, &base);
    if (!up) return 0;
    if (!boot_texture_detile_slice(up + fp.Offset, fp.Footprint.RowPitch, src, (size_t)extent, wb, x->height, log_gob_h, log_gob_d)) return 0;
    fp.Offset += base;
    D3D12_RESOURCE_STATES was = x->state;
    D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = x->texture;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = was; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    BOOT_GP_RB("rt load", r->cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, srcl = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
    srcl.PlacedFootprint = fp;
    BOOT_GP_CTR("rt load", r->cl, &dst, 0, 0, 0, &srcl, NULL);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = was;
    BOOT_GP_RB("rt load", r->cl, 1, &b);
    ++boot_render_rt_loads;
    return 1;
}
/* Writes targets idx[0..n) back to guest memory, in that order (a later one wins where they
 * overlap), each only if the GPU wrote it since its last write-back. The copies are recorded after
 * all recorded work and complete with one submission per batch (bounded by the readback ring);
 * guest memory is written after the batch completed. Returns 0 on failure with r->error set. */
/* Profiling: write-back batches (each a submission that waits) by the call site that asked for them. */
static const char *boot_render_wb_file; static int boot_render_wb_line;
static struct { const char *file; int line; uint64_t batches, targets; } boot_render_wb_sites[16];
static inline void boot_render_wb_count(unsigned targets) {
    unsigned i = 0;
    while (i < 16 && boot_render_wb_sites[i].file && (boot_render_wb_sites[i].line != boot_render_wb_line || boot_render_wb_sites[i].file != boot_render_wb_file)) ++i;
    if (i == 16) return;
    if (!boot_render_wb_sites[i].file) { boot_render_wb_sites[i].file = boot_render_wb_file ? boot_render_wb_file : "?"; boot_render_wb_sites[i].line = boot_render_wb_line; }
    ++boot_render_wb_sites[i].batches; boot_render_wb_sites[i].targets += targets;
}
#define BOOT_RENDER_WB_SITE() (boot_render_wb_file = __FILE__, boot_render_wb_line = __LINE__)
/* Targets whose image is not kept in guest memory by write-back/reload: pitch-linear ones (handled by
 * the pitch path) and depth formats with more than one plane (D24S8, D32S8). Single-plane depth (D32F,
 * D16) is tiled like colour: the game samples its depth buffers (scene depth -> linear depth, fog, light
 * volumes; shadow-map cascades rendered one layer/mip at a time and sampled as one 2D-array texture),
 * which read guest memory - all zero while depth never left the host texture. */
static inline int boot_render_rt_untiled(const BootRenderTarget *x) {
    if (x->format & BOOT_RENDER_PITCH_FLAG) return 1;
    if (!(x->format & 0x10000u)) return 0;
    static int on = -1;   /* TOTK_DEPTH_WRITEBACK=0: depth stays host-only (A/B, the old behaviour) */
    if (on < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_DEPTH_WRITEBACK", e, sizeof e); on = !(n && n < sizeof e && e[0] == '0'); }
    return !on || (x->dxgi != DXGI_FORMAT_D32_FLOAT && x->dxgi != DXGI_FORMAT_D16_UNORM);
}
static inline int boot_render_rt_writeback_many(BootRender *r, const unsigned *idx, unsigned n) {
    BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned next = 0; next < n; ) {
        struct { unsigned i; uint8_t *dst, *src; uint64_t extent; UINT pitch; } job[16]; unsigned jobs = 0;
        boot_render_staging_reset(&boot_render_rb_ring);   /* earlier batches were consumed */
        for (; next < n && jobs < 16; ++next) {
            unsigned i = idx[next]; BootRenderTarget *x = &T->rts[i]; BootRenderRtInfo *in = &T->info[i];
            if (!in->dirty) continue;
            if (!x->texture || !in->known || boot_render_rt_untiled(x)) { in->dirty = 0; continue; }
            uint64_t extent = boot_render_rt_extent(x, in);
            uint8_t *dst = extent ? boot_render_guest(r, x->address, extent) : NULL;
            if (!dst) { in->dirty = 0; snprintf(r->error, sizeof r->error, "render target %llx write-back outside guest memory", (unsigned long long)x->address); return 0; }
            D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
            ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
            if (!boot_render_begin(r)) { boot_render_fail(r, 0, "command list reset"); return 0; }
            ID3D12Resource *rb = NULL; uint64_t base = 0;
            uint8_t *mapped = boot_render_staging_alloc(r, &boot_render_rb_ring, D3D12_HEAP_TYPE_READBACK, total, &rb, &base);
            if (!mapped) { if (jobs) break; boot_render_fail(r, 0, "render target readback"); return 0; }
            D3D12_RESOURCE_STATES was = x->state;
            boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dl = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
            dl.PlacedFootprint = fp; dl.PlacedFootprint.Offset += base;
            BOOT_GP_CTR("write-back cpu", r->cl, &dl, 0, 0, 0, &sl, NULL);
            boot_render_rt_barrier(r, x, was);
            job[jobs].i = i; job[jobs].dst = dst; job[jobs].src = mapped + fp.Offset; job[jobs].extent = extent; job[jobs].pitch = fp.Footprint.RowPitch; ++jobs;
        }
        if (!jobs) continue;
        boot_render_wb_count(jobs);
        if (!boot_render_submit(r)) { boot_render_fail(r, 0, "render target write-back"); return 0; }
        for (unsigned j = 0; j < jobs; ++j) {
            unsigned i = job[j].i; BootRenderTarget *x = &T->rts[i]; BootRenderRtInfo *in = &T->info[i];
            in->dirty = 0;
            if (!boot_texture_tile_slice(job[j].dst, (size_t)job[j].extent, job[j].src, job[j].pitch, x->width * boot_render_dxgi_bytes(x->dxgi), x->height,
                                         in->log_gob_h, in->log_gob_d)) { boot_render_fail(r, 0, "render target write-back"); return 0; }
            uint64_t extent = job[j].extent;
            boot_render_invalidate(x->address, extent);
            /* other clean targets over this memory no longer hold its contents: reload them on next use */
            for (unsigned k = 0; k < T->count; ++k) {
                BootRenderTarget *o = &T->rts[k]; BootRenderRtInfo *oi = &T->info[k];
                if (k == i || !o->texture || !oi->known || oi->dirty || boot_render_rt_untiled(o)) continue;
                if (boot_render_rt_overlap_reload() && o->address < x->address + extent && x->address < o->address + boot_render_rt_extent(o, oi)) oi->stale = 1;
            }
            ++boot_render_rt_writebacks;
        }
    }
    return 1;
}
static inline int boot_render_rt_writeback_impl(BootRender *r, unsigned i) { return boot_render_rt_writeback_many(r, &i, 1); }
#define boot_render_rt_writeback(r, i) (BOOT_RENDER_WB_SITE(), boot_render_rt_writeback_impl((r), (i)))
/* Writes back every GPU-written block-linear target overlapping guest VA [va, va+size). */
/* Targets sharing memory (aliased allocations) are written back oldest use first, so the most
 * recent GPU write is the one guest memory keeps. */
#define boot_render_flush_targets(r, va, size) (BOOT_RENDER_WB_SITE(), boot_render_flush_targets_impl((r), (va), (size)))
/* Dirty targets over guest VA [va, va+size) into list[], oldest use first; returns their count. */
static inline unsigned boot_render_rt_dirty_over(const BootRenderRtTable *T, uint64_t va, uint64_t size, unsigned *list) {
    unsigned n = 0;
    for (unsigned i = 0; i < T->count; ++i) {
        const BootRenderTarget *x = &T->rts[i]; const BootRenderRtInfo *in = &T->info[i];
        if (!in->dirty || !x->texture || !in->known) continue;
        uint64_t extent = boot_render_rt_extent(x, in);
        if (!extent || !(x->address < va + size && va < x->address + extent)) continue;
        unsigned k = n++;   /* insertion by last use, oldest first */
        while (k && T->info[list[k - 1]].last_use > in->last_use) { list[k] = list[k - 1]; --k; }
        list[k] = i;
    }
    return n;
}
/* render-26: with render-24 a dirty target is no longer written back when another target over its memory is bound,
 * so it can stay dirty while a later-drawn target over the same memory has already been written back (clean).
 * Writing only the dirty ones back then put the OLDER image over the newer one in guest memory, and a texture read
 * through the guest path (a TIC in another format / shape than the target: "rt format mismatch", decoded from guest
 * memory) saw the older bytes, e.g. 50e1d0000 640x360 read as fmt 0c after a scene change (play-40 GPU hang right
 * after such reads). Guest memory must end with every byte from the last target that drew it: the dirty targets over
 * the range, plus every clean one drawn after the oldest of them, are written back in draw order
 * (boot_render_rt_written_at, render-14). */
static uint64_t boot_render_rt_written_at[BOOT_RENDER_RT_CAP];   /* render-14 (defined below): tentative declaration
 * for the draw order here */
static inline unsigned boot_render_rt_latest_over(BootRenderRtTable *T, uint64_t va, uint64_t size, unsigned *list) {
    unsigned n = boot_render_rt_dirty_over(T, va, size, list);
    if (!n || boot_render_rt_overlap_reload()) return n;
    uint64_t oldest = UINT64_MAX;
    for (unsigned q = 0; q < n; ++q) if (boot_render_rt_written_at[list[q]] < oldest) oldest = boot_render_rt_written_at[list[q]];
    for (unsigned i = 0; i < T->count && n < BOOT_RENDER_RT_CAP; ++i) {
        BootRenderTarget *x = &T->rts[i]; BootRenderRtInfo *in = &T->info[i];
        if (in->dirty || !x->texture || !in->known || boot_render_rt_untiled(x) || boot_render_rt_written_at[i] <= oldest) continue;
        uint64_t extent = boot_render_rt_extent(x, in);
        if (!extent || !(x->address < va + size && va < x->address + extent)) continue;
        /* only if it overlaps one of the dirty targets being written (otherwise its bytes are untouched) */
        int hit = 0;
        for (unsigned q = 0; q < n && !hit; ++q) {
            const BootRenderTarget *y = &T->rts[list[q]]; uint64_t ye = boot_render_rt_extent(y, &T->info[list[q]]);
            hit = T->info[list[q]].dirty && y->address < x->address + extent && x->address < y->address + ye;
        }
        if (!hit) continue;
        in->dirty = 1;   /* re-written (write-backs take dirty targets only; they leave it clean again) */
        list[n++] = i;
    }
    for (unsigned a = 1; a < n; ++a) {   /* draw order, oldest first */
        unsigned v = list[a], b = a;
        while (b && boot_render_rt_written_at[list[b - 1]] > boot_render_rt_written_at[v]) { list[b] = list[b - 1]; --b; }
        list[b] = v;
    }
    return n;
}
static inline int boot_render_flush_targets_impl(BootRender *r, uint64_t va, uint64_t size) {
    BootRenderRtTable *T = boot_render_rtab(r);
    unsigned list[BOOT_RENDER_RT_CAP], n = boot_render_rt_latest_over(T, va, size, list);
    return n ? boot_render_rt_writeback_many(r, list, n) : 1;
}
/* wb-01: the target whose image a write-back leaves last over guest VA [va, va+size). boot_render_flush_targets writes
 * boot_render_rt_latest_over's list in draw order (the dirty targets over the range, then the clean ones drawn after
 * the oldest of them that overlap one of them), each over its whole footprint, so the list's last entry is the image
 * guest memory ends with where that entry's footprint is. The same choice without boot_render_rt_latest_over's side
 * effect (it marks those clean targets dirty for the write-back). `list`/`n`: boot_render_rt_dirty_over for the range.
 * -1: n == 0, or with TOTK_RT_OVERLAP_RELOAD=1 (write-back order by last use, dirty targets only) n != 1. */
static inline int boot_render_rt_newest_over(const BootRenderRtTable *T, uint64_t va, uint64_t size, const unsigned *list, unsigned n) {
    if (!n) return -1;
    if (boot_render_rt_overlap_reload()) return n == 1 ? (int)list[0] : -1;
    uint64_t oldest = UINT64_MAX; unsigned best = list[0];
    for (unsigned q = 0; q < n; ++q) {   /* ties as the stable sort there: the later entry */
        uint64_t w = boot_render_rt_written_at[list[q]];
        if (w < oldest) oldest = w;
        if (w >= boot_render_rt_written_at[best]) best = list[q];
    }
    for (unsigned i = 0; i < T->count; ++i) {
        const BootRenderTarget *x = &T->rts[i]; const BootRenderRtInfo *in = &T->info[i];
        if (in->dirty || !x->texture || !in->known || boot_render_rt_untiled(x) || boot_render_rt_written_at[i] <= oldest) continue;
        uint64_t extent = boot_render_rt_extent(x, in);
        if (!extent || !(x->address < va + size && va < x->address + extent)) continue;
        int hit = 0;
        for (unsigned q = 0; q < n && !hit; ++q) {
            const BootRenderTarget *y = &T->rts[list[q]]; uint64_t ye = boot_render_rt_extent(y, &T->info[list[q]]);
            hit = y->address < x->address + extent && x->address < y->address + ye;
        }
        if (hit && boot_render_rt_written_at[i] >= boot_render_rt_written_at[best]) best = i;   /* appended after the dirty ones */
    }
    return (int)best;
}
static inline int boot_render_gt_run(BootRender *r, const unsigned *idx, unsigned n, int load, unsigned load_h, unsigned load_d);
static inline int boot_render_rt_refresh(BootRender *r, unsigned i, uint64_t extent, unsigned log_h, unsigned log_d);
static uint64_t boot_render_gt_fallbacks;
/* Guest memory [va, va+size) was written outside the 3D engine (copy engine): block-linear targets
 * over it reload their guest image on next use (callers write back dirty ones before writing). */
/* render-25: memory owned by GPU images, per 64 KiB granule. A granule is owned once a host render target over it
 * is drawn or cleared, and stops being owned when something outside the 3D engine writes it (copy engine, compute
 * stores, CPU: boot_render_rt_guest_written) or when a target over it is evicted (its image then lives in guest
 * memory). A new host target whose whole footprint is owned starts cleared instead of loading guest memory: those
 * bytes are only other shapes' images written back (TOTK reuses its transient memory for differently shaped targets),
 * and loading them gave one wrong frame at every camera cut / title scene change (play-40, after render-24).
 * Sparse: 4 GiB of address space per 64 KiB table (one byte per granule), allocated on first use. */
#define BOOT_RENDER_OWN_TOPS 64
static struct { uint32_t top; uint8_t *g; } boot_render_own[BOOT_RENDER_OWN_TOPS];
static uint64_t boot_render_rt_owned_starts;   /* new targets started cleared */
static inline uint8_t *boot_render_own_table(uint64_t va, int create) {
    uint32_t top = (uint32_t)(va >> 32);
    for (unsigned k = 0; k < BOOT_RENDER_OWN_TOPS; ++k) {
        if (boot_render_own[k].g && boot_render_own[k].top == top) return boot_render_own[k].g;
        if (!boot_render_own[k].g) {
            if (!create) return NULL;
            boot_render_own[k].g = calloc(65536, 1); boot_render_own[k].top = top;
            return boot_render_own[k].g;
        }
    }
    return NULL;   /* table full: nothing more is tracked (targets load from guest memory as before) */
}
static inline void boot_render_own_set(uint64_t va, uint64_t size, uint8_t on) {
    if (!size) return;
    for (uint64_t gr = va >> 16, last = (va + size - 1) >> 16; gr <= last; ++gr) {
        uint8_t *t = boot_render_own_table(gr << 16, on);
        if (t) t[gr & 0xffff] = on;
    }
}
static inline int boot_render_own_all(uint64_t va, uint64_t size) {
    if (!size) return 0;
    for (uint64_t gr = va >> 16, last = (va + size - 1) >> 16; gr <= last; ++gr) {
        uint8_t *t = boot_render_own_table(gr << 16, 0);
        if (!t || !t[gr & 0xffff]) return 0;
    }
    return 1;
}
/* Diagnostics71 only; implementation follows source-sequence declarations. */
static inline void boot_render_image_external(BootRender *r, uint64_t va, uint64_t size, unsigned kind);
static inline void boot_render_rt_guest_written(BootRender *r, uint64_t va, uint64_t size) {
    boot_render_own_set(va, size, 0);   /* render-25: written outside the 3D engine */
    BootRenderRtTable *T = boot_render_rtab(r);
    if (boot_image_trace_enabled) boot_render_image_external(r, va, size, BOOT_IMAGE_EXTERNAL_BEFORE);
    for (unsigned i = 0; i < T->count; ++i) {
        BootRenderTarget *x = &T->rts[i]; BootRenderRtInfo *in = &T->info[i];
        if (!x->texture || !in->known || boot_render_rt_untiled(x)) continue;
        if (x->address < va + size && va < x->address + boot_render_rt_extent(x, in)) in->stale = 1;
    }
    if (boot_image_trace_enabled) boot_render_image_external(r, va, size, BOOT_IMAGE_EXTERNAL_AFTER);
}
/* Textures copied on the GPU from a dirty colour target (boot_render_texture_from_target): their slot is
 * a snapshot of the target, so a later write to that target invalidates it (the target being dirty
 * already, boot_render_rt_touch would not invalidate textures over its memory again). */
#define BOOT_RENDER_RT_COPIES 128   /* wb-01: 64 -> 128 (a copy now registers every dirty target over its memory) */
/* render-14: textures assembled from targets (boot_render_texture_mips_from_targets) remember their source
 * targets and when they were copied; a draw into any of those targets after that makes the slot stale, and its
 * next use copies the targets again in queue order (no wait). The rt-copies list above could lose a slot
 * (64 entries, overflowed every frame by the rotating TIC indices of the shadow-map arrays) or miss a target
 * whose host resource changed, and the slot then kept its first copy: TOTK's title-screen local-light array
 * (512x288x2 at 50f2e0000) was assembled once and sampled stale ever after (play-27, lightprobe: the shader's
 * light lookup did not correlate with the light targets). Direct-mapped by TIC index; a collision only drops
 * the extra check for the evicted slot (the rt-copies invalidation still applies). */
/* render-15: a pool of BOOT_RENDER_ASM_POOL records reached through a per-TIC-index map (no collisions); a record
 * reused for another texture (least recently assembled) forces its old texture's rebuild, so an assembled
 * texture is either tracked or rebuilt on its next use, never left stale. The 256-entry direct-mapped table of
 * render-14 collided with the shadow-map arrays' rotating TIC indices and dropped the title light array's record
 * on some frames (the mural flashed between lit and dark). */
#define BOOT_RENDER_ASM_POOL 1024
static struct { uint32_t index, n; uint16_t k[64]; ID3D12Resource *src[64]; uint64_t copied, used; int on; } boot_render_asm[BOOT_RENDER_ASM_POOL];
static uint16_t *boot_render_asm_of; static uint64_t boot_render_asm_clock, boot_render_asm_evictions;
static uint64_t boot_render_rt_write_seq, boot_render_rt_written_at[BOOT_RENDER_RT_CAP], boot_render_asm_refreshes;
static struct { ID3D12Resource *rt; uint32_t index; } boot_render_rt_copies[BOOT_RENDER_RT_COPIES];
static unsigned boot_render_rt_copy_count, boot_render_rt_copy_next;
/* heavy-work: a 4096-bit filter over the target textures that have registrations above: a bit is set wherever an entry is
 * added and the filter is rebuilt from the remaining entries by every full scan of boot_render_rt_copies_written, so every
 * entry's bit is always set. A target whose bit is clear has no entry and its scan (once per bound target of every draw,
 * ~1 us per draw in play-89's lookdown profile) is skipped. TOTK_RT_COPY_FILTER=0: always scan. */
static uint64_t boot_render_rt_copy_bloom[64];
static inline unsigned boot_render_rt_copy_bit(const void *p) { uint64_t h = ((uint64_t)(uintptr_t)p >> 4) * UINT64_C(0x9e3779b97f4a7c15); return (unsigned)(h >> 52); }
static inline void boot_render_rt_copy_note(const void *p) { unsigned b = boot_render_rt_copy_bit(p); boot_render_rt_copy_bloom[b >> 6] |= UINT64_C(1) << (b & 63); }
static inline int boot_render_rt_copy_filter_on(void) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_RT_COPY_FILTER", v, sizeof v); on = !(n && n < sizeof v && v[0] == '0'); }
    return on;
}
static uint64_t boot_render_tex_from_rt, boot_render_tex_from_rt_invalidated;
/* Diagnostics71: renderer-thread state only. CPU API records never inspect this table. */
static uint64_t boot_render_image_drain_id;
static unsigned boot_render_image_cpu_origin;
static inline BootRenderRtTable *boot_render_image_table(BootRender *r) {
    /* Observe existing storage only: unlike rtab(), this cannot bind/allocate. */
    BootRenderRtTable *T = boot_render_rtt;
    return T && T->owner == r && T->device == r->gpu->device ? T : NULL;
}
static inline void boot_render_image_source(BootRender *r, unsigned i, unsigned kind, uint64_t va, uint64_t size, uint64_t extra) {
    if (!boot_image_trace_overlap(va, size)) return;
    BootRenderRtTable *T = boot_render_image_table(r); if (!T) return;
    BootImageTraceRecord e = {0}; e.kind = kind;
    e.v[0] = boot_render_exec_id; e.v[1] = boot_render_image_drain_id; e.v[2] = boot_render_image_cpu_origin;
    e.v[3] = va; e.v[4] = size; e.v[5] = i; e.v[13] = boot_image_trace_epoch(); e.v[14] = T->generation; e.v[15] = extra;
    if (i < T->count) {
        const BootRenderTarget *x = &T->rts[i]; const BootRenderRtInfo *in = &T->info[i];
        e.v[6] = x->address; e.v[7] = boot_render_rt_extent(x, in); e.v[8] = (uintptr_t)x->texture;
        e.v[9] = in->known | ((uint64_t)in->dirty << 8) | ((uint64_t)in->stale << 16);
        e.v[10] = boot_render_rt_written_at[i]; e.v[11] = boot_render_rt_write_seq; e.v[12] = in->last_use;
        e.v[16] = x->width; e.v[17] = x->height; e.v[18] = x->format;
    }
    boot_image_trace_push(&e);
}
static inline void boot_render_image_external(BootRender *r, uint64_t va, uint64_t size, unsigned kind) {
    if (!boot_image_trace_overlap(va, size)) return;
    BootRenderRtTable *T = boot_render_image_table(r); if (!T) return; unsigned found = 0;
    for (unsigned i = 0; i < T->count; ++i) {
        const BootRenderTarget *x = &T->rts[i]; const BootRenderRtInfo *in = &T->info[i];
        if (!x->texture || !in->known || boot_render_rt_untiled(x)) continue;
        uint64_t extent = boot_render_rt_extent(x, in);
        if (x->address >= va ? x->address - va < size : va - x->address < extent) {
            boot_render_image_source(r, i, kind, va, size, 0); ++found;
        }
    }
    if (!found) boot_render_image_source(r, UINT32_MAX, kind, va, size, 0);
}
static int boot_render_tex_from_rt_off;   /* tests: the guest path instead (A/B) */
static inline void boot_render_rt_copies_written(const BootRenderTarget *x);
/* render-32: guest CPU writes into texture memory (nvnTextureWriteTexels / WriteTexelsStrided run in the driver on the
 * game's thread) queued as guest ranges and applied when the next submission starts recording: host render targets
 * over them reload that memory on their next use (stale), lose GPU ownership (render-25) and cached textures over
 * them rebuild. Under "GPU images are authoritative" this is the one case where guest memory is newer than a GPU
 * image (e.g. a history/exposure texture reset by the CPU at a camera cut). Overflow merges into one covering range. */
#define BOOT_RENDER_CPU_TEXELS 256
static struct { uint64_t va, size, ep; } boot_render_cpu_texels[BOOT_RENDER_CPU_TEXELS];   /* async-xlate: ep, the registry epoch of the earliest write not applied */
static unsigned boot_render_cpu_texels_n; static SRWLOCK boot_render_cpu_texels_lock = SRWLOCK_INIT;
static uint64_t boot_render_cpu_texel_calls, boot_render_cpu_texel_ranges;   /* calls seen / ranges applied (perf3) */
static inline void boot_render_cpu_texels_push(uint64_t va, uint64_t size) {
    if (!va || !size) return;
    const uint64_t ep = (uint64_t)__atomic_load_n(&boot_ax_reg_epoch, __ATOMIC_ACQUIRE);   /* async-xlate: this write's epoch (boot_ax_reg_note ran before) */
    AcquireSRWLockExclusive(&boot_render_cpu_texels_lock);
    ++boot_render_cpu_texel_calls;
    unsigned n = boot_render_cpu_texels_n;
    for (unsigned i = 0; i < n; ++i)   /* the same texture written again before the drain (the earlier write's epoch stays: it is due first) */
        if (boot_render_cpu_texels[i].va == va && boot_render_cpu_texels[i].size == size) { n = UINT32_MAX; break; }
    if (n != UINT32_MAX) {
        if (n < BOOT_RENDER_CPU_TEXELS) { boot_render_cpu_texels[n].va = va; boot_render_cpu_texels[n].size = size; boot_render_cpu_texels[n].ep = ep; boot_render_cpu_texels_n = n + 1; }
        else {   /* full: widen the last entry to cover this one too */
            uint64_t lo = boot_render_cpu_texels[n - 1].va < va ? boot_render_cpu_texels[n - 1].va : va;
            uint64_t hi = boot_render_cpu_texels[n - 1].va + boot_render_cpu_texels[n - 1].size; if (va + size > hi) hi = va + size;
            boot_render_cpu_texels[n - 1].va = lo; boot_render_cpu_texels[n - 1].size = hi - lo;
        }
    }
    ReleaseSRWLockExclusive(&boot_render_cpu_texels_lock);
}
static inline void boot_render_rt_guest_written(BootRender *r, uint64_t va, uint64_t size);
static inline void boot_render_cpu_texels_drain(BootRender *r) {
    if (!boot_render_cpu_texels_n) return;   /* racy read: a push now is applied by the next submission */
    struct { uint64_t va, size; } q[BOOT_RENDER_CPU_TEXELS]; unsigned n = 0;
    const uint64_t cut = boot_ax_texel_cut1 ? boot_ax_texel_cut1 - 1 : UINT64_MAX;   /* async-xlate stage 2 (X): only the writes before this submission's capture */
    AcquireSRWLockExclusive(&boot_render_cpu_texels_lock);
    unsigned kept = 0;
    for (unsigned i = 0; i < boot_render_cpu_texels_n; ++i) {
        if (boot_render_cpu_texels[i].ep <= cut) { q[n].va = boot_render_cpu_texels[i].va; q[n].size = boot_render_cpu_texels[i].size; ++n; }
        else boot_render_cpu_texels[kept++] = boot_render_cpu_texels[i];
    }
    boot_render_cpu_texels_n = kept; boot_ax_texel_held += kept;
    ReleaseSRWLockExclusive(&boot_render_cpu_texels_lock);
    unsigned trace_origin = boot_render_image_cpu_origin;
    if (boot_image_trace_enabled) { ++boot_render_image_drain_id; boot_render_image_cpu_origin = 1; }
    for (unsigned i = 0; i < n; ++i) {
        if (boot_image_trace_overlap(q[i].va, q[i].size)) {
            BootImageTraceRecord e = {0}; e.kind = BOOT_IMAGE_CPU_DRAIN;
            e.v[0] = boot_render_exec_id; e.v[1] = boot_render_image_drain_id; e.v[2] = i; e.v[3] = n;
            e.v[4] = q[i].va; e.v[5] = q[i].size; e.v[6] = boot_image_trace_epoch(); boot_image_trace_push(&e);
        }
        boot_render_rt_guest_written(r, q[i].va, q[i].size);   /* stale + ownership released */
        boot_render_invalidate(q[i].va, q[i].size);             /* cached textures and constant-buffer snapshots */
    }
    boot_render_image_cpu_origin = trace_origin;
    boot_render_cpu_texel_ranges += n;
}
/* Records a use of target `i` by a draw or clear that writes it. */
static inline void boot_render_rt_touch(BootRenderRtTable *T, unsigned i, unsigned log_gob_h, unsigned log_gob_d, int writes) {
    BootRenderRtInfo *in = &T->info[i]; const BootRenderTarget *x = &T->rts[i];
    in->last_use = ++T->clock;
    if (boot_render_rt_untiled(x)) return;
    {   uint64_t was = in->extent;
        in->known = 1; in->log_gob_h = (uint8_t)log_gob_h; in->log_gob_d = (uint8_t)log_gob_d; in->extent = boot_render_rt_extent(x, in);
        if (in->dirty && in->extent != was) ++boot_render_rt_dirty_epoch; }   /* a dirty target's footprint changed: overlap answers change */
    if (writes && !in->dirty) {
        boot_render_own_set(x->address, in->extent, 1);   /* render-25 */
        in->dirty = 1; ++boot_render_rt_dirty_epoch;
        /* textures over it must be rebuilt after write-back (guest memory itself is unchanged) */
        boot_render_invalidate_textures(x->address, in->extent);
    }
    if (writes) boot_render_rt_written_at[i] = ++boot_render_rt_write_seq;   /* render-14 */
    if (writes && boot_render_rt_copy_count) boot_render_rt_copies_written(x);
}
/* Whether a block-linear target other than `i` over guest VA [x->address, +extent) holds GPU writes
 * that guest memory (and so target i's contents) does not have yet: an aliased allocation drawn as
 * another target since target i was last loaded. */
/* A "no overlapping dirty target" answer stays valid while no target became dirty (boot_render_rt_dirty_epoch),
 * no entry's texture changed (T->generation) and the extent asked is the same: it was looped over every entry
 * for every bound target of every draw. */
static uint64_t boot_render_ovl_epoch[BOOT_RENDER_RT_CAP], boot_render_ovl_gen[BOOT_RENDER_RT_CAP], boot_render_ovl_extent[BOOT_RENDER_RT_CAP];
static const void *boot_render_ovl_table; static uint64_t boot_render_ovl_hits;
static inline int boot_render_rt_overlap_dirty_impl(const BootRenderRtTable *T, unsigned i, uint64_t extent);
static inline int boot_render_rt_overlap_dirty(const BootRenderRtTable *T, unsigned i, uint64_t extent) {
    if (boot_render_ovl_table != T) { memset(boot_render_ovl_extent, 0, sizeof boot_render_ovl_extent); boot_render_ovl_table = T; }
    if (i < BOOT_RENDER_RT_CAP && boot_render_ovl_extent[i] == extent + 1 && boot_render_ovl_epoch[i] == boot_render_rt_dirty_epoch && boot_render_ovl_gen[i] == T->generation) {
        ++boot_render_ovl_hits; return 0; }
    int hit = boot_render_rt_overlap_dirty_impl(T, i, extent);
    if (!hit && i < BOOT_RENDER_RT_CAP) { boot_render_ovl_extent[i] = extent + 1; boot_render_ovl_epoch[i] = boot_render_rt_dirty_epoch; boot_render_ovl_gen[i] = T->generation; }
    return hit;
}
static inline int boot_render_rt_overlap_dirty_impl(const BootRenderRtTable *T, unsigned i, uint64_t extent) {
    const BootRenderTarget *x = &T->rts[i];
    for (unsigned k = 0; k < T->count; ++k) {
        const BootRenderRtInfo *o = &T->info[k];
        if (k == i || !o->dirty) continue;
        const BootRenderTarget *y = &T->rts[k];
        if (!y->texture || !o->known || boot_render_rt_untiled(y)) continue;
        if (y->address < x->address + extent && x->address < y->address + o->extent) return 1;
    }
    return 0;
}
/* Host bytes of a target texture (budget accounting; padding ignored). */
static inline uint64_t boot_render_rt_bytes(const D3D12_RESOURCE_DESC *d) {
    uint64_t bpp = d->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8 : d->Format == DXGI_FORMAT_D16_UNORM ? 2 : boot_render_dxgi_bytes(d->Format);
    return (uint64_t)d->Width * d->Height * bpp;
}
/* Textures of evicted targets, kept idle for a later target of the same shape and format instead
 * of destroying one and creating another. A reused texture is cleared to zero, like a new committed
 * resource. */
#define BOOT_RENDER_RT_SPARES 24
static struct { ID3D12Resource *texture; D3D12_RESOURCE_STATES state; uint64_t age; } boot_render_rt_spares[BOOT_RENDER_RT_SPARES];
static uint64_t boot_render_rt_spare_clock, boot_render_rt_spare_hits;
/* Objects that recorded or executing lists may still reference: released after the next submission
 * that waits for the queue (boot_render_submit_impl). An evicted target's texture goes to the spares
 * without a submission, so the spare it displaces may still be in use by the GPU. */
#define BOOT_RENDER_DEFERRED 512
static ID3D12Resource *boot_render_deferred[BOOT_RENDER_DEFERRED]; static unsigned boot_render_deferred_count;
static uint64_t boot_render_deferred_lid[BOOT_RENDER_DEFERRED];   /* drain2-work: the open list when it was put here (every list that may reference it precedes or is it) */
static uint64_t boot_render_deferred_polled, boot_render_deferred_full_waits;
static inline void boot_render_release_later(ID3D12Resource *t) {
    if (!t) return;
    if (boot_render_deferred_count < BOOT_RENDER_DEFERRED) { boot_render_deferred_lid[boot_render_deferred_count] = boot_render_list_id; boot_render_deferred[boot_render_deferred_count++] = t; }
    else ID3D12Resource_Release(t);   /* not reached: boot_render_rt_slot drains the list before it fills */
}
static inline void boot_render_release_deferred(void) {
    for (unsigned i = 0; i < boot_render_deferred_count; ++i) ID3D12Resource_Release(boot_render_deferred[i]);
    boot_render_deferred_count = 0;
}
/* drain2-work: the entries whose list completed, released after each list execution (boot_render_kick) and before the
 * list is drained by a waiting submission (render target eviction), in order (TOTK_LIST_RECYCLE, see the upload ring). */
static inline void boot_render_release_poll(BootRender *r) {
    if (!boot_render_deferred_count || !boot_render_recycle_on()) return;
    unsigned w = 0;
    for (unsigned i = 0; i < boot_render_deferred_count; ++i) {
        if (!boot_render_list_done(r, boot_render_deferred_lid[i])) { boot_render_deferred_lid[w] = boot_render_deferred_lid[i]; boot_render_deferred[w++] = boot_render_deferred[i]; continue; }
        ID3D12Resource_Release(boot_render_deferred[i]); ++boot_render_deferred_polled;
    }
    boot_render_deferred_count = w;
}
static inline void boot_render_rt_spare_put(ID3D12Resource *t, D3D12_RESOURCE_STATES state) {
    unsigned k = 0, oldest = 0;
    for (; k < BOOT_RENDER_RT_SPARES && boot_render_rt_spares[k].texture; ++k)
        if (boot_render_rt_spares[k].age < boot_render_rt_spares[oldest].age) oldest = k;
    if (k == BOOT_RENDER_RT_SPARES) { boot_render_release_later(boot_render_rt_spares[oldest].texture); k = oldest; }
    boot_render_rt_spares[k].texture = t; boot_render_rt_spares[k].state = state; boot_render_rt_spares[k].age = ++boot_render_rt_spare_clock;
}
static inline ID3D12Resource *boot_render_rt_spare_take(const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES *state) {
    for (unsigned k = 0; k < BOOT_RENDER_RT_SPARES; ++k) {
        ID3D12Resource *t = boot_render_rt_spares[k].texture; if (!t) continue;
        D3D12_RESOURCE_DESC sd; t->lpVtbl->GetDesc(t, &sd);
        if (sd.Width != d->Width || sd.Height != d->Height || sd.Format != d->Format || sd.Flags != d->Flags || sd.DepthOrArraySize != 1 || sd.MipLevels != 1) continue;
        *state = boot_render_rt_spares[k].state; boot_render_rt_spares[k].texture = NULL; ++boot_render_rt_spare_hits;
        return t;
    }
    return NULL;
}
static inline void boot_render_rt_spares_release(void) {
    for (unsigned k = 0; k < BOOT_RENDER_RT_SPARES; ++k) if (boot_render_rt_spares[k].texture) ID3D12Resource_Release(boot_render_rt_spares[k].texture);
    memset(boot_render_rt_spares, 0, sizeof boot_render_rt_spares);
}
static inline int boot_render_flush_pitch(BootRender *r);
/* Least recently used target the current draw has not bound (its targets are the last nine uses), or -1. */
static inline int boot_render_rt_victim(const BootRenderRtTable *T) {
    int victim = -1; uint64_t oldest = UINT64_MAX;
    for (unsigned i = 0; i < T->count; ++i) {
        if (!T->rts[i].texture) continue;
        uint64_t u = T->info[i].last_use;
        if (u && u + 9 > T->clock) continue;   /* 0: adopted, not used since */
        if (u < oldest) { oldest = u; victim = (int)i; }
    }
    return victim;
}
/* Writes target `victim` back and frees its entry; the texture goes to the spares. */
static inline int boot_render_rt_evict(BootRender *r, BootRenderRtTable *T, unsigned victim) {
    BootRenderTarget *x = &T->rts[victim];
    if ((x->format & BOOT_RENDER_PITCH_FLAG) && T->pitch_dirty[victim] && !boot_render_flush_pitch(r)) return 0;
    /* its image goes to guest memory on the GPU (no wait), else through the CPU */
    if (T->info[victim].dirty && !boot_render_gt_run(r, &victim, 1, -1, 0, 0)) { ++boot_render_gt_fallbacks; if (!boot_render_rt_writeback(r, victim)) return 0; }
    /* No submission: the texture moves to the spares, where it is only reused by later recording
     * (queue order) or released after a waiting submission (boot_render_release_later). Drain the
     * deferred releases while there is room for this eviction's. */
    if (boot_render_deferred_count + 1 >= BOOT_RENDER_DEFERRED / 2) boot_render_release_poll(r);   /* drain2-work: what completed lists released first */
    if (boot_render_deferred_count + 1 >= BOOT_RENDER_DEFERRED / 2) {
        ++boot_render_deferred_full_waits;   /* drain2-work */
        if (r->recording && !boot_render_submit(r)) { boot_render_fail(r, 0, "submit before render target eviction"); return 0; }
        boot_render_release_deferred();   /* not recording: the queue is idle */
    }
    if (T->info[victim].known && !boot_render_rt_untiled(x)) boot_render_own_set(x->address, T->info[victim].extent, 0);   /* render-25 */
    boot_render_rt_spare_put(x->texture, x->state);
    T->bytes -= T->tex_bytes[victim] <= T->bytes ? T->tex_bytes[victim] : T->bytes; T->tex_bytes[victim] = 0;
    memset(x, 0, sizeof *x);
    memset(&T->info[victim], 0, sizeof T->info[victim]);
    T->pitch_dirty[victim] = 0; T->pitch_pitch[victim] = 0;
    ++boot_render_rt_evictions; ++T->generation;
    return 1;
}
/* Index for a new target of `need` bytes: least recently used targets are written back and evicted
 * while the table holds more than BOOT_RENDER_RT_BUDGET bytes, then the first free entry, a new one,
 * or (table full) the least recently used one. -1 on failure. */
static inline int boot_render_rt_slot(BootRender *r, uint64_t need) {
    BootRenderRtTable *T = boot_render_rtab(r);
    while (T->bytes + need > BOOT_RENDER_RT_BUDGET) {
        int v = boot_render_rt_victim(T);
        if (v < 0) break;   /* everything left is bound by this draw: go over the budget */
        if (!boot_render_rt_evict(r, T, (unsigned)v)) return -1;
    }
    for (unsigned i = 0; i < T->count; ++i) if (!T->rts[i].texture) return (int)i;
    if (T->count < T->cap) return (int)T->count++;
    int victim = boot_render_rt_victim(T);
    if (victim < 0) { boot_render_fail(r, 0, "render target table full (all bound)"); return -1; }
    return boot_render_rt_evict(r, T, (unsigned)victim) ? victim : -1;
}
/* A target texture placed in entry `slot`: budget accounting. */
static inline void boot_render_rt_placed(BootRenderRtTable *T, unsigned slot, const D3D12_RESOURCE_DESC *d) {
    T->tex_bytes[slot] = boot_render_rt_bytes(d); T->bytes += T->tex_bytes[slot];
    if (T->bytes > boot_render_rt_peak) boot_render_rt_peak = T->bytes;
    ++T->generation; ++boot_render_rt_creates;
}
/* Render-target watch (diagnostic, render-08): frames\rt-watch.request holding a GPU address (hex) logs to
 * draw-path.log every creation of a colour target at that address (size, format, whether its first image came
 * from guest memory, the first guest word) and its first 64 uses as a draw or clear target with the guest word
 * at that time. The file is re-read when a target is created. */
static uint64_t boot_render_watch_va; static unsigned boot_render_watch_uses;
static unsigned boot_render_watch_event;   /* render-20: the native event being recorded (set by host_nvn_play.h's trace hook) */
/* render-27a (TOTK_DRED_MARKERS=1, debug): a PIX-style marker (Unicode, metadata 0) before each draw and dispatch naming
 * the submission, the native event, the program ids and the first target, so a DRED report after a device hang
 * (boot_gpu_dred_dump, breadcrumb contexts) names the exact draw. Off by default (a string per draw). */
/* Diagnostic82 uses explicit recording/Execute ledgers, not a Reset epoch inferred from DRED node order. */
static inline int boot_render_markers_on(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_DRED_MARKERS", e, sizeof e); v = n && n < sizeof e && e[0] == '1'; }
    return v;
}
static inline void boot_render_marker(ID3D12GraphicsCommandList *cl, const char *kind, const uint8_t *a, const uint8_t *b, uint64_t target) {
    /* drawcost: the same text the snprintf below makes, formatted straight into the wide string (the snprintf was
     * 1.4 ms per frame on t2 with TOTK_DRED_MARKERS=1). A kind of at most 64 characters keeps the text under 153
     * characters, so the 160-character truncation below never applies; longer (or NULL) kinds take the snprintf. */
    size_t kn = 0; while (kind && kn <= 64 && kind[kn]) ++kn;
    if (kind && kn <= 64) {
        static const char hx[] = "0123456789abcdef";
        wchar_t w[160]; unsigned n = 0; char d[24]; unsigned dn;
        for (size_t i = 0; i < kn; ++i) w[n++] = (wchar_t)(unsigned char)kind[i];
        w[n++] = L' '; w[n++] = L's';
        { uint64_t v = boot_render_exec_id; dn = 0; do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v); while (dn) w[n++] = (wchar_t)d[--dn]; }
        w[n++] = L' '; w[n++] = L'e';
        { unsigned v = boot_render_watch_event; dn = 0; do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v); while (dn) w[n++] = (wchar_t)d[--dn]; }
        for (unsigned h = 0; h < 2; ++h) {
            const uint8_t *p = h ? b : a; w[n++] = L' ';
            for (unsigned i = 0; i < 8; ++i) { unsigned v = p ? p[i] : 0; w[n++] = (wchar_t)hx[v >> 4]; w[n++] = (wchar_t)hx[v & 15]; }
        }
        w[n++] = L' '; w[n++] = L'r'; w[n++] = L't'; w[n++] = L' ';
        { uint64_t v = target; dn = 0; do { d[dn++] = hx[v & 15]; v >>= 4; } while (v); while (dn) w[n++] = (wchar_t)d[--dn]; }
        w[n] = 0;
        ID3D12GraphicsCommandList_SetMarker(cl, 0, w, (UINT)((n + 1) * sizeof(wchar_t)));
        return;
    }
    char t[160]; wchar_t w[160];
    int n = snprintf(t, sizeof t, "%s s%llu e%u %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x rt %llx", kind,
                     (unsigned long long)boot_render_exec_id, boot_render_watch_event,
                     a ? a[0] : 0, a ? a[1] : 0, a ? a[2] : 0, a ? a[3] : 0, a ? a[4] : 0, a ? a[5] : 0, a ? a[6] : 0, a ? a[7] : 0,
                     b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[3] : 0, b ? b[4] : 0, b ? b[5] : 0, b ? b[6] : 0, b ? b[7] : 0,
                     (unsigned long long)target);
    if (n < 0) return;
    if (n >= (int)sizeof t) n = (int)sizeof t - 1;
    for (int i = 0; i <= n; ++i) w[i] = (wchar_t)(unsigned char)t[i];
    ID3D12GraphicsCommandList_SetMarker(cl, 0, w, (UINT)((n + 1) * sizeof(wchar_t)));
}
static inline void boot_render_watch_load(void) {
    FILE *q = fopen("frames\\rt-watch.request", "r"); unsigned long long a = 0;
    if (q) { if (fscanf(q, "%llx", &a) != 1) a = 0; fclose(q); }
    if (a != boot_render_watch_va) { boot_render_watch_va = a; boot_render_watch_uses = 0; }
}
static inline uint8_t *boot_render_guest(BootRender *r, uint64_t va, uint64_t size);
static inline void boot_render_watch_log(BootRender *r, const BootColorTarget *t, const char *what, int loaded) {
    uint32_t word = 0; const uint8_t *g = boot_render_guest(r, t->address, 4); if (g) memcpy(&word, g, 4);
    FILE *f = fopen("draw-path.log", "a"); if (!f) return;
    fprintf(f, "rt-watch %llx: %s %ux%u fmt %02x pitch %u layers %u%s guest word %08x (submission %llu)\n", (unsigned long long)t->address, what,
            t->width, t->height, t->format, t->pitch_layout, t->layers, loaded < 0 ? "" : loaded ? " (image loaded from guest memory)" : " (no guest image loaded)",
            word, (unsigned long long)boot_render_exec_id);
    fclose(f);
}
static inline BootRenderTarget *boot_render_target_null98(BootRender *r,const BootColorTarget *t,unsigned reason,int slot,ID3D12Resource *spare,unsigned created,uint32_t hr) {
    BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_TARGET_NULL,.detail=reason,.known=BOOT_RF98_TARGET|(created?BOOT_RF98_HR:0),.hr=hr,.address=t->address,.format=t->format,.width=t->width,.height=t->height,.layers=t->layers,.pitch=t->pitch_layout,.array_pitch=t->array_pitch,.slot=(unsigned)slot,.spare=(uintptr_t)spare,.create_called=created};boot_rf98_note(&x);return NULL;
}
/* batch-b: the last 8 target table indices boot_render_target / boot_render_depth returned (every bound target of every
 * draw scanned the whole table: 512 entries, ~1 us per draw looking down from a tower). A key (address, extent, format)
 * is in the table at most once: both functions create an entry only after a scan of the whole table found none (and
 * entries never change their key; module adoption copies unique entries). So a remembered index whose entry still has
 * the key is the entry the scan from 0 would stop at: the scan starts there; otherwise it starts at 0 as before.
 * TOTK_RT_MEMO=0: off. */
static unsigned boot_render_rt_memo[8], boot_render_rt_memo_at;
static inline unsigned boot_render_rt_memo_find(const BootRenderRtTable *T, uint64_t address, uint32_t width, uint32_t height, uint32_t format) {
    static int on = -1;
    if (on < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_RT_MEMO", e, sizeof e); on = !(n && n < sizeof e && e[0] == '0'); }
    if (!on) return 0;
    for (unsigned q = 0; q < 8; ++q) {
        unsigned i = boot_render_rt_memo[q];
        if (i >= T->count) continue;
        const BootRenderTarget *x = &T->rts[i];
        if (x->texture && x->address == address && x->width == width && x->height == height && x->format == format) return i;
    }
    return 0;
}
static inline void boot_render_rt_memo_put(unsigned i) {
    for (unsigned q = 0; q < 8; ++q) if (boot_render_rt_memo[q] == i) return;
    boot_render_rt_memo[boot_render_rt_memo_at++ & 7] = i;
}
static inline BootRenderTarget *boot_render_target(BootRender *r, const BootColorTarget *t) {
    DXGI_FORMAT fmt;
    int rf98_format=boot_render_ct_format(t->format, &fmt);
    if (!rf98_format || !t->width || !t->height) return boot_render_target_null98(r,t,rf98_format?BOOT_RF98_T_SHAPE:BOOT_RF98_T_FORMAT,-1,NULL,0,0);
    uint32_t width = t->width, key = t->format;
    if (t->pitch_layout) {
        uint32_t bpp = boot_render_dxgi_bytes(fmt);
        if (t->width % bpp || t->layers > 1) return boot_render_target_null98(r,t,BOOT_RF98_T_PITCH,-1,NULL,0,0);
        width = t->width / bpp; key |= BOOT_RENDER_PITCH_FLAG;
    } else if (t->log_gob_w || t->log_gob_h > 5 || t->log_gob_d > 5) return boot_render_target_null98(r,t,BOOT_RF98_T_GOB,-1,NULL,0,0);
    BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned i = boot_render_rt_memo_find(T, t->address, width, t->height, key); i < T->count; ++i) {   /* batch-b: from the remembered entry */
        BootRenderTarget *x = &T->rts[i];
        if (x->texture && x->address == t->address && x->width == width && x->height == t->height && x->format == key) {
            boot_render_rt_memo_put(i);
            if (t->pitch_layout && !T->pitch_dirty[i]) {
                T->pitch_pitch[i] = t->width;
                if (!boot_render_pitch_load(r, x, t->width)) return boot_render_target_null98(r,t,BOOT_RF98_T_CACHED_PITCH,(int)i,NULL,0,0);
                T->pitch_dirty[i] = 1;
            }
            if (!t->pitch_layout) {
                /* Guest memory is newer than this target where another target over the same memory
                 * (an aliased allocation) was drawn since: its writes go to guest memory first (oldest
                 * first, this one included) and the target reloads, as a newly created one would. */
                uint64_t extent = boot_texture_slice_extent(width * boot_render_dxgi_bytes(fmt), t->height, t->log_gob_h, t->log_gob_d);
                int sync = boot_render_rt_overlap_reload() && boot_render_rt_overlap_dirty(T, i, extent);   /* render-24 */
                if (sync) {
                    ++boot_render_rt_syncs;
                    if (!boot_render_rt_refresh(r, i, extent, t->log_gob_h, t->log_gob_d)) return boot_render_target_null98(r,t,BOOT_RF98_T_OVERLAP,(int)i,NULL,0,0);
                    T->info[i].stale = 0;
                } else if (T->info[i].stale) {   /* guest memory changed under it (copy engine, a write-back over it) */
                    if (!boot_render_rt_refresh(r, i, extent, t->log_gob_h, t->log_gob_d)) return boot_render_target_null98(r,t,BOOT_RF98_T_STALE,(int)i,NULL,0,0);
                    T->info[i].stale = 0;
                }
            }
            if (boot_render_watch_va && t->address == boot_render_watch_va && boot_render_watch_uses++ < 64) boot_render_watch_log(r, t, "bound as target", -1);
            boot_render_rt_touch(T, i, t->log_gob_h, t->log_gob_d, 1);
            return x;
        }
    }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width; d.Height = t->height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    int slot = boot_render_rt_slot(r, boot_render_rt_bytes(&d));
    if (slot < 0) return boot_render_target_null98(r,t,BOOT_RF98_T_SLOT,slot,NULL,0,0);
    BootRenderTarget *x = &T->rts[slot];
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = fmt;
    D3D12_RESOURCE_STATES spare_state = D3D12_RESOURCE_STATE_RENDER_TARGET; ID3D12Resource *spare = boot_render_rt_spare_take(&d, &spare_state);
    HRESULT rf98_target_create=0;
    if (spare) x->texture = spare;
    else if (FAILED(rf98_target_create=ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                         &IID_ID3D12Resource, (void **)&x->texture))) { x->texture = NULL;
        BootRenderFailure98 rf98={BOOT_RF98_R(r),.stage=BOOT_RF98_RT_CREATE,.known=BOOT_RF98_HR|BOOT_RF98_TARGET,.hr=(uint32_t)rf98_target_create,.address=t->address,.format=t->format,.dxgi=(unsigned)d.Format,.width=(unsigned)d.Width,.height=d.Height,.layers=d.DepthOrArraySize,.pitch=t->pitch_layout,.array_pitch=t->array_pitch,.slot=(unsigned)slot,.create_called=1};boot_rf98_note(&rf98);
        return boot_render_target_null98(r,t,BOOT_RF98_T_CREATE,slot,spare,1,(uint32_t)rf98_target_create); }
    x->address = t->address; x->width = width; x->height = t->height; x->format = key; x->dxgi = fmt;
    x->state = spare_state; x->rtv = (uint32_t)slot;
    boot_render_rt_placed(T, (unsigned)slot, &d);
    uint64_t loads = boot_render_rt_loads; int loaded = 0;
    /* on failure the texture goes to the spares instead of being released: recorded commands may reference it */
#define BOOT_RENDER_RT_UNPLACE() do { boot_render_rt_spare_put(x->texture, x->state); x->texture = NULL; T->bytes -= T->tex_bytes[slot]; T->tex_bytes[slot] = 0; } while (0)
    if (t->pitch_layout) {
        /* initial contents: the guest image (a draw may blend with or only partly cover it) */
        T->pitch_pitch[slot] = t->width;
        if (!boot_render_pitch_load(r, x, t->width)) { BOOT_RENDER_RT_UNPLACE(); return boot_render_target_null98(r,t,BOOT_RF98_T_NEW_PITCH,slot,spare,!spare,(uint32_t)rf98_target_create); }
        T->pitch_dirty[slot] = 1; loaded = 1;
    } else {
        /* targets over this memory write their images back first, then the guest image loads; render-25: not when
         * the whole footprint is owned by GPU images (only other shapes' write-backs are there): the target starts
         * cleared (below). A live target of the same address and size (another format: a reinterpretation of its
         * bits) still loads. TOTK_RT_OVERLAP_RELOAD=1 (render-24's A/B) loads as before. */
        uint64_t new_extent = boot_texture_slice_extent(width * boot_render_dxgi_bytes(fmt), t->height, t->log_gob_h, t->log_gob_d);
        int owned = !boot_render_rt_overlap_reload() && boot_render_own_all(t->address, new_extent);
        for (unsigned k = 0; owned && k < T->count; ++k)
            if (k != (unsigned)slot && T->rts[k].texture && T->rts[k].address == t->address && T->rts[k].width == width && T->rts[k].height == t->height) owned = 0;
        if (owned) ++boot_render_rt_owned_starts;
        else if (!boot_render_rt_refresh(r, (unsigned)slot, new_extent, t->log_gob_h, t->log_gob_d)) { BOOT_RENDER_RT_UNPLACE(); return boot_render_target_null98(r,t,BOOT_RF98_T_NEW_REFRESH,slot,spare,!spare,(uint32_t)rf98_target_create); }
        loaded = boot_render_rt_loads != loads;
    }
    ID3D12Device_CreateRenderTargetView(r->gpu->device, x->texture, NULL, boot_render_rtv(r, x));
    if (spare && !loaded) {   /* nothing loaded: the zero contents of a new committed resource (render-25: also an owned footprint) */
        static const float zero[4] = {0, 0, 0, 0};
        if (!boot_render_begin(r)) { BOOT_RENDER_RT_UNPLACE(); return boot_render_target_null98(r,t,BOOT_RF98_T_SPARE_BEGIN,slot,spare,0,0); }
        boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_RENDER_TARGET);
        BOOT_GP_CLEAR_RTV("new target", r->cl, boot_render_rtv(r, x), zero, 0, NULL);
    }
#undef BOOT_RENDER_RT_UNPLACE
    boot_render_watch_load();
    if (boot_render_watch_va && t->address == boot_render_watch_va) boot_render_watch_log(r, t, "target created", loaded);
    boot_render_rt_touch(T, (unsigned)slot, t->log_gob_h, t->log_gob_d, 1);
    return x;
}

/* ---------------------------------------------------------------- occlusion queries */
/* ZPASS_PIXEL_COUNT: while `want` is set, boot_render_draw brackets its draw call with a D3D12
 * OCCLUSION query (non-binary: samples passing the depth/stencil tests) in slot `next` and sets
 * `got` = slot + 1. Slots recorded in the open command list, [lo, next), are resolved into `rb`
 * (one uint64 per slot) just before the list closes in boot_render_submit, so after a submit the
 * counts of every slot below `next` are readable. The caller restarts slots at 0 per submission. */
#define BOOT_RENDER_OCCL_QUERIES 65536u
static struct { ID3D12QueryHeap *qh; ID3D12Resource *rb; int want; uint32_t next, lo, got; } boot_render_occl;
static inline int boot_render_occl_open(BootRender *r) {
    if (boot_render_occl.qh && boot_render_occl.rb) return 1;
    if (!boot_render_occl.qh) {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, BOOT_RENDER_OCCL_QUERIES, 0};
        if (FAILED(ID3D12Device_CreateQueryHeap(r->gpu->device, &qd, &IID_ID3D12QueryHeap, (void **)&boot_render_occl.qh))) {
            boot_render_occl.qh = NULL; snprintf(r->error, sizeof r->error, "occlusion query heap"); return 0; }
    }
    boot_render_occl.rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, (uint64_t)BOOT_RENDER_OCCL_QUERIES * 8, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!boot_render_occl.rb) { snprintf(r->error, sizeof r->error, "occlusion readback buffer"); return 0; }
    return 1;
}
static inline void boot_render_occl_close(void) {
    if (boot_render_occl.qh) ID3D12QueryHeap_Release(boot_render_occl.qh);
    if (boot_render_occl.rb) ID3D12Resource_Release(boot_render_occl.rb);
    memset(&boot_render_occl, 0, sizeof boot_render_occl);
}
static inline void boot_render_occl_resolve(BootRender *r) {
    if (boot_render_occl.qh && boot_render_occl.rb && boot_render_occl.next > boot_render_occl.lo) {
        BOOT_GP_PRE(r->cl);   /* render-40 */
        ID3D12GraphicsCommandList_ResolveQueryData(r->cl, boot_render_occl.qh, D3D12_QUERY_TYPE_OCCLUSION, boot_render_occl.lo,
                                                   boot_render_occl.next - boot_render_occl.lo, boot_render_occl.rb, (UINT64)boot_render_occl.lo * 8);
        BOOT_GP_POST(BOOT_GP_OTHER, "occlusion resolve", r->cl, 0, 0); }
    boot_render_occl.lo = boot_render_occl.next;
}

/* ---------------------------------------------------------------- multi-threaded command recording (heavy-work, TOTK_MT_RECORD)
 * The render thread (t2) spends a large part of every draw inside D3D12: the command list calls (barriers, root
 * arguments, pipeline, targets, viewport, draw, markers, timestamps) and the per-draw CBV/SRV table writes. Here those
 * calls are recorded by worker threads, each into its own real command list, while t2 goes on translating.
 *
 * How: r->cl becomes a proxy object (BootMtrList) with the ID3D12GraphicsCommandList vtable. Every call on it is
 * captured into a byte stream in call order, with its arguments copied (arrays, structs, marker strings). The stream
 * is cut into chunks at safe points (no query, predication, stream output, event or split barrier open) every
 * TOTK_MT_RECORD_CHUNK draws. A sealed chunk goes to a worker, which resets its own real list and allocator, first
 * re-establishes the exact D3D12 state bound at the cut (descriptor heaps, both root signatures and every root
 * argument, pipeline, topology, index/vertex buffers, render targets, blend factor, stencil ref, viewports, scissors:
 * tracked by the proxy while capturing), then replays the chunk and closes the list. Close on the proxy (the
 * renderer's kick) seals the last chunk, replays what is still queued on t2 itself, waits for the workers and
 * boot_mtr_execute executes all chunk lists in order in one ExecuteCommandLists call.
 *
 * Why the result is the same: the queue receives exactly the original command sequence, split into consecutive lists
 * of one ExecuteCommandLists call (resource states carry across them and implicit decay does not happen between
 * lists of one call; barriers act on all earlier work of the queue). Everything else the renderer does (resource and
 * descriptor management, ring allocation, texture/target tables, write-backs, feedback copies, fences) stays on t2 and
 * in the original order, and the kick is still synchronous: the lists are executed before boot_render_kick returns.
 * Arguments the driver reads at record time are captured at call time: CPU descriptors (render target, depth and
 * clear-UAV views) are copied into the proxy's own CPU heaps when the call is made, so a later rewrite of the
 * renderer's view slot cannot change what a deferred call binds. Objects referenced by the calls (resources, pipelines,
 * heaps, signatures) are alive until the lists complete on the GPU, as before, hence at replay time.
 * The per-draw constant-buffer tables and the vertex-attribute SRV pair (TOTK_MT_RECORD_DESC, default on) are created
 * on the worker too: those ring slots are written by that draw only and read by the GPU only (after the execute).
 *
 * Off switch: TOTK_MT_RECORD=0 (r->cl stays the real list, nothing here runs). TOTK_MT_RECORD_THREADS=n workers (1-8,
 * default 4; 0 = capture and replay everything on t2 at the kick, an A/B of the capture cost). TOTK_MT_RECORD_CHUNK=n
 * draws per chunk (16-4096, default 64; later chunks of a long list grow). TOTK_MT_RECORD_DESC=0 keeps the table
 * writes on t2. Not used while a diagnostic that holds the real list is active (tile89, light84/86, cbv tap). */
#define BOOT_MTR_CHUNKS 64            /* real lists one recorded list can be split into (the last one takes the rest) */
#define BOOT_MTR_ALLOCS 512           /* command allocators of chunks 1.. (chunk 0 records on the renderer's own allocator) */
#define BOOT_MTR_MAX_THREADS 8
#define BOOT_MTR_QUEUE 256
#define BOOT_MTR_SNAP_BLOCK 4096u     /* CPU descriptors per snapshot heap block */
#define BOOT_MTR_SNAP_BLOCKS 64
#define BOOT_MTR_ROOT 32              /* root parameters tracked per signature */
#define BOOT_MTR_ROOTC 16             /* of them, constant parameters */
#define BOOT_MTR_ROOTCW 32            /* dwords per constant parameter */
#define BOOT_MTR_MAGIC UINT64_C(0x3152434552544d42)
enum { BOOT_MTR_RK_NONE, BOOT_MTR_RK_TABLE, BOOT_MTR_RK_CBV, BOOT_MTR_RK_SRV, BOOT_MTR_RK_UAV, BOOT_MTR_RK_CONST };
enum { BOOT_MTR_OP_DRAW = 1, BOOT_MTR_OP_DRAWIDX, BOOT_MTR_OP_DISPATCH, BOOT_MTR_OP_COPYBUF, BOOT_MTR_OP_COPYTEX, BOOT_MTR_OP_COPYRES,
       BOOT_MTR_OP_COPYTILES, BOOT_MTR_OP_RESOLVESUB, BOOT_MTR_OP_TOPO, BOOT_MTR_OP_VIEWPORTS, BOOT_MTR_OP_SCISSORS, BOOT_MTR_OP_BLEND,
       BOOT_MTR_OP_SREF, BOOT_MTR_OP_PSO, BOOT_MTR_OP_BARRIER, BOOT_MTR_OP_BUNDLE, BOOT_MTR_OP_HEAPS, BOOT_MTR_OP_ROOTSIG,
       BOOT_MTR_OP_TABLE, BOOT_MTR_OP_CONST1, BOOT_MTR_OP_CONSTS, BOOT_MTR_OP_ROOTVIEW, BOOT_MTR_OP_IB, BOOT_MTR_OP_VB, BOOT_MTR_OP_SO,
       BOOT_MTR_OP_OMRT, BOOT_MTR_OP_CLEARDSV, BOOT_MTR_OP_CLEARRTV, BOOT_MTR_OP_CLEARUAV, BOOT_MTR_OP_DISCARD, BOOT_MTR_OP_BEGINQ,
       BOOT_MTR_OP_ENDQ, BOOT_MTR_OP_RESOLVEQ, BOOT_MTR_OP_PRED, BOOT_MTR_OP_MARKER, BOOT_MTR_OP_ENDEVENT, BOOT_MTR_OP_EXECIND,
       BOOT_MTR_OP_CLEARSTATE, BOOT_MTR_OP_DESC_CBVS, BOOT_MTR_OP_DESC_SRV };
typedef struct BootMtrRoot {
    ID3D12RootSignature *sig; uint32_t mask, cany; uint8_t kind[BOOT_MTR_ROOT]; uint64_t v[BOOT_MTR_ROOT];
    uint32_t cmask[BOOT_MTR_ROOTC]; uint32_t c[BOOT_MTR_ROOTC][BOOT_MTR_ROOTCW];
} BootMtrRoot;
typedef struct BootMtrState {
    ID3D12DescriptorHeap *heaps[2]; UINT nheaps;
    BootMtrRoot g, c;
    ID3D12PipelineState *pso;
    int topo_set; D3D12_PRIMITIVE_TOPOLOGY topo;
    int ib_set, ib_null; D3D12_INDEX_BUFFER_VIEW ib;
    uint32_t vb_mask; D3D12_VERTEX_BUFFER_VIEW vb[32];
    int om_set, has_dsv; UINT nrt; D3D12_CPU_DESCRIPTOR_HANDLE rtv[8], dsv;
    int bf_set, bf_null; float bf[4];
    int sref_set; UINT sref;
    int vp_set; UINT nvp; D3D12_VIEWPORT vp[16];
    int sc_set; UINT nsc; D3D12_RECT sc[16];
} BootMtrState;
typedef struct BootMtrChunk {
    struct BootMtrList *L; unsigned index;
    uint8_t *buf; size_t used, cap;
    BootMtrState start; int fresh; ID3D12PipelineState *init_pso;
    ID3D12CommandAllocator *alloc; int alloc_idx, alloc_reset;   /* chunk 0: the renderer's allocator; else a pool entry */
    unsigned draws, cmds;
    volatile LONG state;   /* 0 capturing, 1 queued, 2 replaying, 3 done */
    HRESULT hr; uint64_t qpc; int on_worker;
} BootMtrChunk;
typedef struct BootMtrSnap { ID3D12DescriptorHeap *block[BOOT_MTR_SNAP_BLOCKS]; SIZE_T base[BOOT_MTR_SNAP_BLOCKS]; UINT inc; unsigned cur, pos; } BootMtrSnap;
typedef struct BootMtrList {
    const ID3D12GraphicsCommandListVtbl *lpVtbl;   /* first: the object is used as an ID3D12GraphicsCommandList */
    uint64_t magic; volatile LONG refs;
    ID3D12Device *device; BootGpu *gpu;
    ID3D12GraphicsCommandList *lists[BOOT_MTR_CHUNKS];   /* lists[0]: the renderer's own list */
    /* chunk allocators: free again once the proxy's own fence (signalled right after the list's ExecuteCommandLists)
     * passed the value of their last use (UINT64_MAX: taken by the list being recorded) */
    struct { ID3D12CommandAllocator *a; uint64_t fence; int used; } alloc[BOOT_MTR_ALLOCS]; unsigned nalloc, alloc_next;
    ID3D12Fence *fence; uint64_t fence_value; int executed;
    ID3D12CommandAllocator *main_alloc;   /* the renderer's allocator given to Reset */
    BootMtrChunk chunk[BOOT_MTR_CHUNKS]; unsigned cur;
    BootMtrState st;
    int open, tainted, oom, blocked_noted; unsigned queries, events, split_barriers, pred_on, so_on, chunk_draws, want;
    unsigned pending;   /* chunks queued or replaying (under the pool lock) */
    BootMtrSnap snap[3];   /* RTV, DSV, CBV/SRV/UAV (clear-UAV CPU views) */
    unsigned list_draws;
} BootMtrList;
static struct {
    SRWLOCK lock; CONDITION_VARIABLE work, done;
    BootMtrChunk *q[BOOT_MTR_QUEUE]; unsigned head, tail;
    unsigned threads; int started;
} boot_mtr_pool = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT, CONDITION_VARIABLE_INIT, {0}, 0, 0, 0, 0};
static struct {
    uint64_t installs, diag_skips, lists, chunks, worker_chunks, t2_chunks, single_lists, cmds, draws, bytes, closed_calls, oom, failed;
    uint64_t blocked[7];   /* split deferred by: query, predication, stream output, event, split barrier, tainted list, chunk cap */
    uint64_t tainted, snap_blocks, snap_fail, desc_tables, desc_srvs, alloc_made, alloc_short;
    uint64_t t2_replay_qpc, worker_replay_qpc, wait_qpc, last_log_lists;
} boot_mtr_stats;
static const ID3D12GraphicsCommandListVtbl boot_mtr_vtbl;
static inline int boot_mtr_env(const char *name, int def, int lo, int hi) {
    char v[16]; DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    if (!n || n >= sizeof v) return def;
    int x = atoi(v); return x < lo ? lo : x > hi ? hi : x;
}
static inline int boot_mtr_on(void) { static int on = -1; if (on < 0) on = boot_mtr_env("TOTK_MT_RECORD", 1, 0, 1); return on; }
static inline int boot_mtr_desc_on(void) { static int on = -1; if (on < 0) on = boot_mtr_env("TOTK_MT_RECORD_DESC", 1, 0, 1); return on; }
static inline unsigned boot_mtr_chunk_base(void) { static int n = -1; if (n < 0) n = boot_mtr_env("TOTK_MT_RECORD_CHUNK", 64, 16, 4096); return (unsigned)n; }
static inline BootMtrList *boot_mtr_of(ID3D12GraphicsCommandList *cl) {
    return cl && cl->lpVtbl == &boot_mtr_vtbl ? (BootMtrList *)(void *)cl : NULL;
}
static inline uint64_t boot_mtr_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (uint64_t)q.QuadPart; }

/* ---- capture stream */
static inline void *boot_mtr_put(BootMtrList *L, uint32_t op, size_t bytes) {
    BootMtrChunk *c = &L->chunk[L->cur];
    size_t need = 8 + ((bytes + 7) & ~(size_t)7);
    if (c->used + need > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536; while (cap < c->used + need) cap *= 2;
        uint8_t *b = (uint8_t *)realloc(c->buf, cap);
        if (!b) { L->oom = 1; ++boot_mtr_stats.oom; return NULL; }
        c->buf = b; c->cap = cap;
    }
    uint32_t *h = (uint32_t *)(void *)(c->buf + c->used); h[0] = op; h[1] = (uint32_t)need;
    c->used += need; ++c->cmds;
    return h + 2;
}

/* ---- state tracking (what a list cut at this point must bind first) */
static inline void boot_mtr_root_sig(BootMtrRoot *R, ID3D12RootSignature *sig) {
    if (R->sig == sig) return;   /* the same signature keeps its arguments */
    memset(R, 0, sizeof *R); R->sig = sig;
}
static inline int boot_mtr_root_view(BootMtrList *L, BootMtrRoot *R, UINT i, uint8_t kind, uint64_t v) {
    if (i >= BOOT_MTR_ROOT) { L->tainted = 1; return 0; }
    R->kind[i] = kind; R->v[i] = v; R->mask |= 1u << i;
    if (i < BOOT_MTR_ROOTC) { R->cmask[i] = 0; R->cany &= ~(1u << i); }
    return 1;
}
static inline void boot_mtr_root_consts(BootMtrList *L, BootMtrRoot *R, UINT i, UINT n, const void *data, UINT off) {
    if (i >= BOOT_MTR_ROOTC || off > BOOT_MTR_ROOTCW || n > BOOT_MTR_ROOTCW - off) { L->tainted = 1; return; }
    if (R->kind[i] != BOOT_MTR_RK_CONST) { R->cmask[i] = 0; R->kind[i] = BOOT_MTR_RK_CONST; }
    memcpy(&R->c[i][off], data, (size_t)n * 4);
    for (UINT k = 0; k < n; ++k) R->cmask[i] |= 1u << (off + k);
    R->mask |= 1u << i; if (n) R->cany |= 1u << i;
}

/* ---- CPU descriptor snapshots (copied when the call is made: the driver reads them at record time) */
static inline int boot_mtr_snap(BootMtrList *L, unsigned which, const D3D12_CPU_DESCRIPTOR_HANDLE *src, UINT n, int contiguous, D3D12_CPU_DESCRIPTOR_HANDLE *out) {
    static const D3D12_DESCRIPTOR_HEAP_TYPE type[3] = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    BootMtrSnap *S = &L->snap[which];
    if (!n) return 1;
    if (n > 8) return 0;
    if (S->pos + n > BOOT_MTR_SNAP_BLOCK) { ++S->cur; S->pos = 0; }
    if (S->cur >= BOOT_MTR_SNAP_BLOCKS) { S->cur = BOOT_MTR_SNAP_BLOCKS; return 0; }
    if (!S->block[S->cur]) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {type[which], BOOT_MTR_SNAP_BLOCK, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(ID3D12Device_CreateDescriptorHeap(L->device, &hd, &IID_ID3D12DescriptorHeap, (void **)&S->block[S->cur]))) { S->block[S->cur] = NULL; return 0; }
        D3D12_CPU_DESCRIPTOR_HANDLE h; S->block[S->cur]->lpVtbl->GetCPUDescriptorHandleForHeapStart(S->block[S->cur], &h); S->base[S->cur] = h.ptr;
        if (!S->inc) S->inc = ID3D12Device_GetDescriptorHandleIncrementSize(L->device, type[which]);
        ++boot_mtr_stats.snap_blocks;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dst = {S->base[S->cur] + (SIZE_T)S->pos * S->inc};
    if (contiguous || n == 1) ID3D12Device_CopyDescriptorsSimple(L->device, n, dst, src[0], type[which]);
    else { UINT one[8]; for (UINT i = 0; i < n; ++i) one[i] = 1;
           ID3D12Device_CopyDescriptors(L->device, 1, &dst, &n, n, src, one, type[which]); }
    for (UINT i = 0; i < n; ++i) out[i].ptr = dst.ptr + (SIZE_T)i * S->inc;
    S->pos += n;
    return 1;
}

/* ---- worker pool */
static void boot_mtr_replay(BootMtrChunk *c);
static DWORD WINAPI boot_mtr_worker(void *arg) {
    (void)arg;
    for (;;) {
        AcquireSRWLockExclusive(&boot_mtr_pool.lock);
        while (boot_mtr_pool.head == boot_mtr_pool.tail) SleepConditionVariableSRW(&boot_mtr_pool.work, &boot_mtr_pool.lock, INFINITE, 0);
        BootMtrChunk *c = boot_mtr_pool.q[boot_mtr_pool.head++ % BOOT_MTR_QUEUE]; c->state = 2; c->on_worker = 1;
        ReleaseSRWLockExclusive(&boot_mtr_pool.lock);
        uint64_t q0 = boot_mtr_qpc(); boot_mtr_replay(c); c->qpc = boot_mtr_qpc() - q0;
        AcquireSRWLockExclusive(&boot_mtr_pool.lock);
        c->state = 3; --c->L->pending;
        ReleaseSRWLockExclusive(&boot_mtr_pool.lock);
        WakeAllConditionVariable(&boot_mtr_pool.done);
    }
    return 0;
}
static inline void boot_mtr_pool_start(void) {
    if (boot_mtr_pool.started) return;
    boot_mtr_pool.started = 1;
    unsigned n = (unsigned)boot_mtr_env("TOTK_MT_RECORD_THREADS", 4, 0, BOOT_MTR_MAX_THREADS);
    for (unsigned i = 0; i < n; ++i) {
        HANDLE h = CreateThread(NULL, 256 << 10, boot_mtr_worker, NULL, 0, NULL);
        if (!h) break;
        CloseHandle(h); ++boot_mtr_pool.threads;
    }
}
/* Hands chunk c to a worker (1), or 0 when there are none / the queue is full (the caller replays it at the kick). */
static inline int boot_mtr_enqueue(BootMtrChunk *c) {
    if (!boot_mtr_pool.threads) return 0;
    AcquireSRWLockExclusive(&boot_mtr_pool.lock);
    int ok = boot_mtr_pool.tail - boot_mtr_pool.head < BOOT_MTR_QUEUE;
    if (ok) { c->state = 1; boot_mtr_pool.q[boot_mtr_pool.tail++ % BOOT_MTR_QUEUE] = c; ++c->L->pending; }
    ReleaseSRWLockExclusive(&boot_mtr_pool.lock);
    if (ok) WakeConditionVariable(&boot_mtr_pool.work);
    return ok;
}

/* ---- chunk allocators */
/* A free pool allocator for the next chunk (index; its reset flag in *reset), or -1: none free and the pool is full. */
static inline int boot_mtr_alloc_take(BootMtrList *L, int *reset) {
    uint64_t done = ID3D12Fence_GetCompletedValue(L->fence);
    for (unsigned n = 0; n < L->nalloc; ++n) {
        unsigned i = (L->alloc_next + n) % L->nalloc;
        if (L->alloc[i].fence != UINT64_MAX && L->alloc[i].fence <= done) {
            L->alloc_next = (i + 1) % L->nalloc; *reset = L->alloc[i].used; L->alloc[i].used = 1; L->alloc[i].fence = UINT64_MAX; return (int)i; }
    }
    if (L->nalloc == BOOT_MTR_ALLOCS) { ++boot_mtr_stats.alloc_short; return -1; }
    ID3D12CommandAllocator *a = NULL;
    if (FAILED(ID3D12Device_CreateCommandAllocator(L->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a)) || !a) { ++boot_mtr_stats.alloc_short; return -1; }
    unsigned i = L->nalloc++; L->alloc[i].a = a; L->alloc[i].used = 1; L->alloc[i].fence = UINT64_MAX; *reset = 0; ++boot_mtr_stats.alloc_made;
    return (int)i;
}
/* ---- chunk boundaries */
static inline void boot_mtr_chunk_open(BootMtrList *L, unsigned k, const BootMtrState *start, int fresh, ID3D12PipelineState *init_pso) {
    BootMtrChunk *c = &L->chunk[k];
    c->L = L; c->index = k; c->used = 0; c->draws = c->cmds = 0; c->state = 0; c->hr = S_OK; c->qpc = 0; c->on_worker = 0;
    c->fresh = fresh; c->init_pso = init_pso; c->alloc = L->main_alloc; c->alloc_idx = -1; c->alloc_reset = 0;
    if (fresh) memset(&c->start, 0, sizeof c->start); else c->start = *start;
}
static inline void boot_mtr_seal(BootMtrList *L, int idx, int reset) {
    BootMtrChunk *c = &L->chunk[L->cur];
    c->draws = L->chunk_draws;
    if (!boot_mtr_enqueue(c)) c->state = 1;   /* stays for the kick */
    ++L->cur; L->chunk_draws = 0; L->want = 0; L->blocked_noted = 0;
    boot_mtr_chunk_open(L, L->cur, &L->st, 0, NULL);
    c = &L->chunk[L->cur]; c->alloc_idx = idx; c->alloc = L->alloc[idx].a; c->alloc_reset = reset;
}
static inline void boot_mtr_try_split(BootMtrList *L) {
    int why = L->queries ? 0 : L->pred_on ? 1 : L->so_on ? 2 : L->events ? 3 : L->split_barriers ? 4 : L->tainted ? 5 : L->cur + 1 >= BOOT_MTR_CHUNKS ? 6 : -1;
    int reset = 0, idx = why < 0 ? boot_mtr_alloc_take(L, &reset) : -1;
    if (why < 0 && idx < 0) why = 6;
    if (why < 0) { boot_mtr_seal(L, idx, reset); return; }
    if (!L->blocked_noted) { L->blocked_noted = 1; ++boot_mtr_stats.blocked[why]; }
    if (why >= 5) L->want = 0;   /* no later cut in this list */
}
static inline int boot_mtr_pre(BootMtrList *L) {
    if (!L->open || L->oom) { if (!L->open) ++boot_mtr_stats.closed_calls; return 0; }
    if (L->want) boot_mtr_try_split(L);
    return 1;
}
static inline void boot_mtr_drawn(BootMtrList *L) {
    ++L->list_draws;
    unsigned base = boot_mtr_chunk_base(), target = base + (L->cur / 16) * base;   /* later chunks of a long list are larger */
    if (++L->chunk_draws >= target && !L->tainted && L->cur + 1 < BOOT_MTR_CHUNKS) L->want = 1;
}

/* ---- replay (worker or t2) */
static inline void boot_mtr_root_emit(ID3D12GraphicsCommandList *cl, const BootMtrRoot *R, int compute) {
    if (!R->sig) return;
    if (compute) ID3D12GraphicsCommandList_SetComputeRootSignature(cl, R->sig); else ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, R->sig);
    for (UINT i = 0; i < BOOT_MTR_ROOT; ++i) {
        if (!(R->mask & (1u << i))) continue;
        uint64_t v = R->v[i]; D3D12_GPU_DESCRIPTOR_HANDLE h = {v};
        switch (R->kind[i]) {
            case BOOT_MTR_RK_TABLE: if (compute) ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, i, h); else ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, i, h); break;
            case BOOT_MTR_RK_CBV: if (compute) ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, i, v); else ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, i, v); break;
            case BOOT_MTR_RK_SRV: if (compute) ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, i, v); else ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(cl, i, v); break;
            case BOOT_MTR_RK_UAV: if (compute) ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, i, v); else ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, i, v); break;
            case BOOT_MTR_RK_CONST:
                if (i < BOOT_MTR_ROOTC) for (UINT k = 0; k < BOOT_MTR_ROOTCW; ) {
                    if (!(R->cmask[i] & (1u << k))) { ++k; continue; }
                    UINT e = k; while (e < BOOT_MTR_ROOTCW && (R->cmask[i] & (1u << e))) ++e;
                    if (compute) ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, i, e - k, &R->c[i][k], k);
                    else ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, i, e - k, &R->c[i][k], k);
                    k = e;
                }
                break;
            default: break;
        }
    }
}
static inline void boot_mtr_prologue(ID3D12GraphicsCommandList *cl, const BootMtrState *s) {
    if (s->nheaps) ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, s->nheaps, s->heaps);
    boot_mtr_root_emit(cl, &s->g, 0);
    boot_mtr_root_emit(cl, &s->c, 1);
    if (s->pso) ID3D12GraphicsCommandList_SetPipelineState(cl, s->pso);
    if (s->topo_set) ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, s->topo);
    if (s->ib_set) ID3D12GraphicsCommandList_IASetIndexBuffer(cl, s->ib_null ? NULL : &s->ib);
    for (UINT i = 0; i < 32; ++i) if (s->vb_mask & (1u << i)) ID3D12GraphicsCommandList_IASetVertexBuffers(cl, i, 1, &s->vb[i]);
    if (s->om_set) ID3D12GraphicsCommandList_OMSetRenderTargets(cl, s->nrt, s->nrt ? s->rtv : NULL, FALSE, s->has_dsv ? &s->dsv : NULL);
    if (s->bf_set) ID3D12GraphicsCommandList_OMSetBlendFactor(cl, s->bf_null ? NULL : s->bf);
    if (s->sref_set) ID3D12GraphicsCommandList_OMSetStencilRef(cl, s->sref);
    if (s->vp_set) ID3D12GraphicsCommandList_RSSetViewports(cl, s->nvp, s->nvp ? s->vp : NULL);
    if (s->sc_set) ID3D12GraphicsCommandList_RSSetScissorRects(cl, s->nsc, s->nsc ? s->sc : NULL);
}
typedef struct { ID3D12Resource *dst; UINT64 dst_off; ID3D12Resource *src; UINT64 src_off, n; } BootMtrCopyBuf;
typedef struct { D3D12_TEXTURE_COPY_LOCATION dst, src; UINT x, y, z, has_box; D3D12_BOX box; } BootMtrCopyTex;
typedef struct { ID3D12Resource *tiled, *buffer; D3D12_TILED_RESOURCE_COORDINATE coord; D3D12_TILE_REGION_SIZE size; UINT64 off; D3D12_TILE_COPY_FLAGS flags; } BootMtrCopyTiles;
typedef struct { ID3D12Resource *dst, *src; UINT dst_sub, src_sub; DXGI_FORMAT format; } BootMtrResolve;
typedef struct { UINT compute, index; UINT64 v; } BootMtrRootArg;   /* tables / root views: kind in v's companion */
typedef struct { UINT compute, index, kind; UINT pad; UINT64 v; } BootMtrRootView;
typedef struct { UINT compute, index, n, off; } BootMtrConsts;   /* + n dwords */
typedef struct { UINT start, n, has; UINT pad; } BootMtrViews;     /* + n views */
typedef struct { UINT n, has_dsv; D3D12_CPU_DESCRIPTOR_HANDLE rtv[8], dsv; } BootMtrOm;
typedef struct { D3D12_CPU_DESCRIPTOR_HANDLE h; D3D12_CLEAR_FLAGS flags; FLOAT depth; UINT stencil, n; } BootMtrClearDsv;   /* + n rects */
typedef struct { D3D12_CPU_DESCRIPTOR_HANDLE h; FLOAT color[4]; UINT n, pad; } BootMtrClearRtv;                     /* + n rects */
typedef struct { D3D12_GPU_DESCRIPTOR_HANDLE gpu; D3D12_CPU_DESCRIPTOR_HANDLE cpu; ID3D12Resource *res; UINT values[4]; UINT n, is_float; } BootMtrClearUav;   /* + n rects */
typedef struct { ID3D12Resource *res; UINT has, nrects, first, count; } BootMtrDiscard;   /* + nrects rects */
typedef struct { ID3D12QueryHeap *heap; D3D12_QUERY_TYPE type; UINT index, count; ID3D12Resource *dst; UINT64 off; } BootMtrQuery;
typedef struct { ID3D12Resource *buffer; UINT64 off; D3D12_PREDICATION_OP op; UINT pad; } BootMtrPred;
typedef struct { UINT metadata, size, begin, pad; } BootMtrMarker;   /* + size bytes */
typedef struct { ID3D12CommandSignature *sig; ID3D12Resource *args, *count; UINT64 args_off, count_off; UINT max, pad; } BootMtrExecInd;
typedef struct { SIZE_T first; UINT inc, n; } BootMtrDescCbvs;   /* + n views */
typedef struct { ID3D12Resource *res; SIZE_T dst; UINT has_desc, pad; D3D12_SHADER_RESOURCE_VIEW_DESC desc; } BootMtrDescSrv;
static inline void boot_mtr_play(ID3D12GraphicsCommandList *cl, ID3D12Device *dev, const uint8_t *b, size_t used) {
    for (size_t at = 0; at + 8 <= used; ) {
        const uint32_t *h = (const uint32_t *)(const void *)(b + at); const uint8_t *p = (const uint8_t *)(h + 2);
        const UINT *u = (const UINT *)(const void *)p; uint32_t op = h[0], size = h[1];
        if (size < 8) break;
        at += size;
        switch (op) {
            case BOOT_MTR_OP_DRAW: ID3D12GraphicsCommandList_DrawInstanced(cl, u[0], u[1], u[2], u[3]); break;
            case BOOT_MTR_OP_DRAWIDX: ID3D12GraphicsCommandList_DrawIndexedInstanced(cl, u[0], u[1], u[2], (INT)u[3], u[4]); break;
            case BOOT_MTR_OP_DISPATCH: ID3D12GraphicsCommandList_Dispatch(cl, u[0], u[1], u[2]); break;
            case BOOT_MTR_OP_COPYBUF: { const BootMtrCopyBuf *a = (const void *)p; ID3D12GraphicsCommandList_CopyBufferRegion(cl, a->dst, a->dst_off, a->src, a->src_off, a->n); break; }
            case BOOT_MTR_OP_COPYTEX: { const BootMtrCopyTex *a = (const void *)p; ID3D12GraphicsCommandList_CopyTextureRegion(cl, &a->dst, a->x, a->y, a->z, &a->src, a->has_box ? &a->box : NULL); break; }
            case BOOT_MTR_OP_COPYRES: { ID3D12Resource *const *a = (ID3D12Resource *const *)(const void *)p; ID3D12GraphicsCommandList_CopyResource(cl, a[0], a[1]); break; }
            case BOOT_MTR_OP_COPYTILES: { const BootMtrCopyTiles *a = (const void *)p; ID3D12GraphicsCommandList_CopyTiles(cl, a->tiled, &a->coord, &a->size, a->buffer, a->off, a->flags); break; }
            case BOOT_MTR_OP_RESOLVESUB: { const BootMtrResolve *a = (const void *)p; ID3D12GraphicsCommandList_ResolveSubresource(cl, a->dst, a->dst_sub, a->src, a->src_sub, a->format); break; }
            case BOOT_MTR_OP_TOPO: ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, (D3D12_PRIMITIVE_TOPOLOGY)u[0]); break;
            case BOOT_MTR_OP_VIEWPORTS: ID3D12GraphicsCommandList_RSSetViewports(cl, u[0], u[1] ? (const D3D12_VIEWPORT *)(const void *)(p + 8) : NULL); break;
            case BOOT_MTR_OP_SCISSORS: ID3D12GraphicsCommandList_RSSetScissorRects(cl, u[0], u[1] ? (const D3D12_RECT *)(const void *)(p + 8) : NULL); break;
            case BOOT_MTR_OP_BLEND: ID3D12GraphicsCommandList_OMSetBlendFactor(cl, u[0] ? (const FLOAT *)(const void *)(p + 8) : NULL); break;
            case BOOT_MTR_OP_SREF: ID3D12GraphicsCommandList_OMSetStencilRef(cl, u[0]); break;
            case BOOT_MTR_OP_PSO: ID3D12GraphicsCommandList_SetPipelineState(cl, *(ID3D12PipelineState *const *)(const void *)p); break;
            case BOOT_MTR_OP_BARRIER: ID3D12GraphicsCommandList_ResourceBarrier(cl, u[0], (const D3D12_RESOURCE_BARRIER *)(const void *)(p + 8)); break;
            case BOOT_MTR_OP_BUNDLE: ID3D12GraphicsCommandList_ExecuteBundle(cl, *(ID3D12GraphicsCommandList *const *)(const void *)p); break;
            case BOOT_MTR_OP_HEAPS: ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, u[0], (ID3D12DescriptorHeap *const *)(const void *)(p + 8)); break;
            case BOOT_MTR_OP_ROOTSIG: {
                ID3D12RootSignature *s; memcpy(&s, p + 8, sizeof s);
                if (u[0]) ID3D12GraphicsCommandList_SetComputeRootSignature(cl, s); else ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s);
                break; }
            case BOOT_MTR_OP_TABLE: {
                const BootMtrRootArg *a = (const void *)p; D3D12_GPU_DESCRIPTOR_HANDLE g = {a->v};
                if (a->compute) ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, a->index, g); else ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, a->index, g);
                break; }
            case BOOT_MTR_OP_CONST1:
                if (u[0]) ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(cl, u[1], u[2], u[3]); else ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(cl, u[1], u[2], u[3]);
                break;
            case BOOT_MTR_OP_CONSTS: {
                const BootMtrConsts *a = (const void *)p; const void *d = p + sizeof *a;
                if (a->compute) ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, a->index, a->n, d, a->off);
                else ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, a->index, a->n, d, a->off);
                break; }
            case BOOT_MTR_OP_ROOTVIEW: {
                const BootMtrRootView *a = (const void *)p;
                if (a->kind == BOOT_MTR_RK_CBV) { if (a->compute) ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, a->index, a->v); else ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, a->index, a->v); }
                else if (a->kind == BOOT_MTR_RK_SRV) { if (a->compute) ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, a->index, a->v); else ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(cl, a->index, a->v); }
                else { if (a->compute) ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, a->index, a->v); else ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, a->index, a->v); }
                break; }
            case BOOT_MTR_OP_IB: ID3D12GraphicsCommandList_IASetIndexBuffer(cl, u[0] ? (const D3D12_INDEX_BUFFER_VIEW *)(const void *)(p + 8) : NULL); break;
            case BOOT_MTR_OP_VB: { const BootMtrViews *a = (const void *)p; ID3D12GraphicsCommandList_IASetVertexBuffers(cl, a->start, a->n, a->has ? (const D3D12_VERTEX_BUFFER_VIEW *)(const void *)(p + sizeof *a) : NULL); break; }
            case BOOT_MTR_OP_SO: { const BootMtrViews *a = (const void *)p; ID3D12GraphicsCommandList_SOSetTargets(cl, a->start, a->n, a->has ? (const D3D12_STREAM_OUTPUT_BUFFER_VIEW *)(const void *)(p + sizeof *a) : NULL); break; }
            case BOOT_MTR_OP_OMRT: { const BootMtrOm *a = (const void *)p; ID3D12GraphicsCommandList_OMSetRenderTargets(cl, a->n, a->n ? a->rtv : NULL, FALSE, a->has_dsv ? &a->dsv : NULL); break; }
            case BOOT_MTR_OP_CLEARDSV: { const BootMtrClearDsv *a = (const void *)p; ID3D12GraphicsCommandList_ClearDepthStencilView(cl, a->h, a->flags, a->depth, (UINT8)a->stencil, a->n, a->n ? (const D3D12_RECT *)(const void *)(p + sizeof *a) : NULL); break; }
            case BOOT_MTR_OP_CLEARRTV: { const BootMtrClearRtv *a = (const void *)p; ID3D12GraphicsCommandList_ClearRenderTargetView(cl, a->h, a->color, a->n, a->n ? (const D3D12_RECT *)(const void *)(p + sizeof *a) : NULL); break; }
            case BOOT_MTR_OP_CLEARUAV: {
                const BootMtrClearUav *a = (const void *)p; const D3D12_RECT *rc = a->n ? (const D3D12_RECT *)(const void *)(p + sizeof *a) : NULL;
                if (a->is_float) { float f[4]; memcpy(f, a->values, sizeof f); ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(cl, a->gpu, a->cpu, a->res, f, a->n, rc); }
                else ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(cl, a->gpu, a->cpu, a->res, a->values, a->n, rc);
                break; }
            case BOOT_MTR_OP_DISCARD: {
                const BootMtrDiscard *a = (const void *)p; D3D12_DISCARD_REGION g = {a->nrects, a->nrects ? (const D3D12_RECT *)(const void *)(p + sizeof *a) : NULL, a->first, a->count};
                ID3D12GraphicsCommandList_DiscardResource(cl, a->res, a->has ? &g : NULL); break; }
            case BOOT_MTR_OP_BEGINQ: { const BootMtrQuery *a = (const void *)p; ID3D12GraphicsCommandList_BeginQuery(cl, a->heap, a->type, a->index); break; }
            case BOOT_MTR_OP_ENDQ: { const BootMtrQuery *a = (const void *)p; ID3D12GraphicsCommandList_EndQuery(cl, a->heap, a->type, a->index); break; }
            case BOOT_MTR_OP_RESOLVEQ: { const BootMtrQuery *a = (const void *)p; ID3D12GraphicsCommandList_ResolveQueryData(cl, a->heap, a->type, a->index, a->count, a->dst, a->off); break; }
            case BOOT_MTR_OP_PRED: { const BootMtrPred *a = (const void *)p; ID3D12GraphicsCommandList_SetPredication(cl, a->buffer, a->off, a->op); break; }
            case BOOT_MTR_OP_MARKER: {
                const BootMtrMarker *a = (const void *)p; const void *d = a->size ? p + sizeof *a : NULL;
                if (a->begin) ID3D12GraphicsCommandList_BeginEvent(cl, a->metadata, d, a->size); else ID3D12GraphicsCommandList_SetMarker(cl, a->metadata, d, a->size);
                break; }
            case BOOT_MTR_OP_ENDEVENT: ID3D12GraphicsCommandList_EndEvent(cl); break;
            case BOOT_MTR_OP_EXECIND: { const BootMtrExecInd *a = (const void *)p; ID3D12GraphicsCommandList_ExecuteIndirect(cl, a->sig, a->max, a->args, a->args_off, a->count, a->count_off); break; }
            case BOOT_MTR_OP_CLEARSTATE: ID3D12GraphicsCommandList_ClearState(cl, *(ID3D12PipelineState *const *)(const void *)p); break;
            case BOOT_MTR_OP_DESC_CBVS: {
                const BootMtrDescCbvs *a = (const void *)p; const D3D12_CONSTANT_BUFFER_VIEW_DESC *v = (const void *)(p + sizeof *a);
                for (UINT k = 0; k < a->n; ++k) { D3D12_CPU_DESCRIPTOR_HANDLE d = {a->first + (SIZE_T)k * a->inc}; ID3D12Device_CreateConstantBufferView(dev, &v[k], d); }
                break; }
            case BOOT_MTR_OP_DESC_SRV: {
                const BootMtrDescSrv *a = (const void *)p; D3D12_CPU_DESCRIPTOR_HANDLE d = {a->dst};
                ID3D12Device_CreateShaderResourceView(dev, a->res, a->has_desc ? &a->desc : NULL, d); break; }
            default: break;
        }
    }
}
/* Records chunk c into its real list: reset (with its own allocator), the bound state of the cut, the commands, close. */
static void boot_mtr_replay(BootMtrChunk *c) {
    BootMtrList *L = c->L; unsigned k = c->index; HRESULT hr = S_OK;
    ID3D12CommandAllocator *alloc = c->alloc;   /* chunk 0: the renderer's (it manages its resets); else ours, free since its fence passed */
    if (!alloc) { c->hr = E_INVALIDARG; return; }
    if (k && c->alloc_reset && FAILED(hr = ID3D12CommandAllocator_Reset(alloc))) { c->hr = hr; return; }
    ID3D12GraphicsCommandList *cl = L->lists[k];
    if (!cl) {
        hr = ID3D12Device_CreateCommandList(L->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, c->init_pso, &IID_ID3D12GraphicsCommandList, (void **)&cl);
        if (FAILED(hr)) { c->hr = hr; return; }
        L->lists[k] = cl;
    } else if (FAILED(hr = ID3D12GraphicsCommandList_Reset(cl, alloc, c->init_pso))) { c->hr = hr; return; }
    if (!c->fresh) boot_mtr_prologue(cl, &c->start);
    boot_mtr_play(cl, L->device, c->buf, c->used);
    c->hr = ID3D12GraphicsCommandList_Close(cl);
}

/* ---- the proxy's methods */
#define BOOT_MTR_SELF BootMtrList *L = (BootMtrList *)(void *)This
static HRESULT STDMETHODCALLTYPE boot_mtr_QueryInterface(ID3D12GraphicsCommandList *This, REFIID riid, void **ppv) {
    static const GUID unk = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    if (!ppv) return E_POINTER;
    if (!memcmp(riid, &IID_ID3D12GraphicsCommandList, sizeof(GUID)) || !memcmp(riid, &unk, sizeof(GUID))) {
        BOOT_MTR_SELF; InterlockedIncrement(&L->refs); *ppv = This; return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE boot_mtr_AddRef(ID3D12GraphicsCommandList *This) { BOOT_MTR_SELF; return (ULONG)InterlockedIncrement(&L->refs); }
static inline void boot_mtr_drain(BootMtrList *L);
static ULONG STDMETHODCALLTYPE boot_mtr_Release(ID3D12GraphicsCommandList *This) {
    BOOT_MTR_SELF; LONG n = InterlockedDecrement(&L->refs);
    if (n) return (ULONG)n;
    boot_mtr_drain(L);
    for (unsigned k = 0; k < BOOT_MTR_CHUNKS; ++k) { if (L->lists[k]) ID3D12GraphicsCommandList_Release(L->lists[k]); free(L->chunk[k].buf); }
    for (unsigned i = 0; i < L->nalloc; ++i) ID3D12CommandAllocator_Release(L->alloc[i].a);
    if (L->fence) ID3D12Fence_Release(L->fence);
    for (unsigned t = 0; t < 3; ++t) for (unsigned b = 0; b < BOOT_MTR_SNAP_BLOCKS; ++b) if (L->snap[t].block[b]) ID3D12DescriptorHeap_Release(L->snap[t].block[b]);
    L->magic = 0; free(L);
    return 0;
}
static HRESULT STDMETHODCALLTYPE boot_mtr_GetPrivateData(ID3D12GraphicsCommandList *This, REFGUID g, UINT *n, void *d) { BOOT_MTR_SELF; return ID3D12GraphicsCommandList_GetPrivateData(L->lists[0], g, n, d); }
static HRESULT STDMETHODCALLTYPE boot_mtr_SetPrivateData(ID3D12GraphicsCommandList *This, REFGUID g, UINT n, const void *d) { BOOT_MTR_SELF; return ID3D12GraphicsCommandList_SetPrivateData(L->lists[0], g, n, d); }
static HRESULT STDMETHODCALLTYPE boot_mtr_SetPrivateDataInterface(ID3D12GraphicsCommandList *This, REFGUID g, const IUnknown *d) { BOOT_MTR_SELF; return ID3D12GraphicsCommandList_SetPrivateDataInterface(L->lists[0], g, d); }
static HRESULT STDMETHODCALLTYPE boot_mtr_SetName(ID3D12GraphicsCommandList *This, const WCHAR *name) { BOOT_MTR_SELF; return ID3D12GraphicsCommandList_SetName(L->lists[0], name); }
static HRESULT STDMETHODCALLTYPE boot_mtr_GetDevice(ID3D12GraphicsCommandList *This, REFIID riid, void **d) { BOOT_MTR_SELF; return ID3D12GraphicsCommandList_GetDevice(L->lists[0], riid, d); }
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE boot_mtr_GetType(ID3D12GraphicsCommandList *This) { (void)This; return D3D12_COMMAND_LIST_TYPE_DIRECT; }
/* Waits until no chunk of L is queued or replaying; queued ones are replayed here. */
static inline void boot_mtr_drain(BootMtrList *L) {
    for (;;) {
        BootMtrChunk *c = NULL;
        AcquireSRWLockExclusive(&boot_mtr_pool.lock);
        if (boot_mtr_pool.head != boot_mtr_pool.tail) { c = boot_mtr_pool.q[boot_mtr_pool.head++ % BOOT_MTR_QUEUE]; c->state = 2; c->on_worker = 0; }
        else if (!L->pending) { ReleaseSRWLockExclusive(&boot_mtr_pool.lock); return; }
        else { uint64_t w0 = boot_mtr_qpc(); SleepConditionVariableSRW(&boot_mtr_pool.done, &boot_mtr_pool.lock, INFINITE, 0); boot_mtr_stats.wait_qpc += boot_mtr_qpc() - w0; }
        ReleaseSRWLockExclusive(&boot_mtr_pool.lock);
        if (c) {   /* help: replay a queued chunk (any proxy's) on this thread */
            uint64_t q0 = boot_mtr_qpc(); boot_mtr_replay(c); c->qpc = boot_mtr_qpc() - q0;
            AcquireSRWLockExclusive(&boot_mtr_pool.lock); c->state = 3; --c->L->pending; ReleaseSRWLockExclusive(&boot_mtr_pool.lock);
            WakeAllConditionVariable(&boot_mtr_pool.done);
        }
    }
}
static inline void boot_mtr_log(void) {
    if (boot_mtr_stats.lists - boot_mtr_stats.last_log_lists < 2048) return;
    boot_mtr_stats.last_log_lists = boot_mtr_stats.lists;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); double ms = 1000.0 / (double)f.QuadPart;
    FILE *lf = fopen("draw-path.log", "a"); if (!lf) return;
    fprintf(lf, "mt record (TOTK_MT_RECORD, %u workers, chunk %u draws, descriptors %s): lists %llu (one chunk %llu), chunks %llu (workers %llu, render thread %llu), "
            "commands %llu, draws %llu, MiB %llu; replay ms: workers %.0f, render thread %.0f, kick waits %.0f; cuts deferred by query %llu, predication %llu, "
            "stream out %llu, event %llu, split barrier %llu, tainted %llu, chunk cap %llu; tainted lists %llu, snapshot heaps %llu (failed %llu), "
            "deferred tables %llu + srvs %llu, calls on a closed list %llu, out of memory %llu, failed lists %llu, chunk allocators %llu (short %llu)\n",
            boot_mtr_pool.threads, boot_mtr_chunk_base(), boot_mtr_desc_on() ? "deferred" : "render thread",
            (unsigned long long)boot_mtr_stats.lists, (unsigned long long)boot_mtr_stats.single_lists, (unsigned long long)boot_mtr_stats.chunks,
            (unsigned long long)boot_mtr_stats.worker_chunks, (unsigned long long)boot_mtr_stats.t2_chunks, (unsigned long long)boot_mtr_stats.cmds,
            (unsigned long long)boot_mtr_stats.draws, (unsigned long long)(boot_mtr_stats.bytes >> 20),
            (double)boot_mtr_stats.worker_replay_qpc * ms, (double)boot_mtr_stats.t2_replay_qpc * ms, (double)boot_mtr_stats.wait_qpc * ms,
            (unsigned long long)boot_mtr_stats.blocked[0], (unsigned long long)boot_mtr_stats.blocked[1], (unsigned long long)boot_mtr_stats.blocked[2],
            (unsigned long long)boot_mtr_stats.blocked[3], (unsigned long long)boot_mtr_stats.blocked[4], (unsigned long long)boot_mtr_stats.blocked[5],
            (unsigned long long)boot_mtr_stats.blocked[6], (unsigned long long)boot_mtr_stats.tainted, (unsigned long long)boot_mtr_stats.snap_blocks,
            (unsigned long long)boot_mtr_stats.snap_fail, (unsigned long long)boot_mtr_stats.desc_tables, (unsigned long long)boot_mtr_stats.desc_srvs,
            (unsigned long long)boot_mtr_stats.closed_calls, (unsigned long long)boot_mtr_stats.oom, (unsigned long long)boot_mtr_stats.failed,
            (unsigned long long)boot_mtr_stats.alloc_made, (unsigned long long)boot_mtr_stats.alloc_short);
    fclose(lf);
}
static HRESULT STDMETHODCALLTYPE boot_mtr_Close(ID3D12GraphicsCommandList *This) {
    BOOT_MTR_SELF;
    if (!L->open) return E_FAIL;
    L->open = 0;
    BootMtrChunk *last = &L->chunk[L->cur]; last->draws = L->chunk_draws; last->state = 1;
    /* the last chunk on this thread while the workers finish theirs, then whatever is still queued */
    uint64_t q0 = boot_mtr_qpc(); boot_mtr_replay(last); last->qpc = boot_mtr_qpc() - q0; last->state = 3;
    boot_mtr_drain(L);
    HRESULT hr = L->oom ? E_OUTOFMEMORY : S_OK;
    for (unsigned k = 0; k <= L->cur; ++k) {
        BootMtrChunk *c = &L->chunk[k];
        if (k < L->cur && c->state == 1) {   /* not handed out (no workers or a full queue) */
            uint64_t t0 = boot_mtr_qpc(); boot_mtr_replay(c); c->qpc = boot_mtr_qpc() - t0; c->state = 3; c->on_worker = 0; }
        if (FAILED(c->hr) && SUCCEEDED(hr)) hr = c->hr;
        if (c->on_worker) { ++boot_mtr_stats.worker_chunks; boot_mtr_stats.worker_replay_qpc += c->qpc; }
        else { ++boot_mtr_stats.t2_chunks; boot_mtr_stats.t2_replay_qpc += c->qpc; }
        boot_mtr_stats.cmds += c->cmds; boot_mtr_stats.bytes += c->used;
    }
    ++boot_mtr_stats.lists; boot_mtr_stats.chunks += L->cur + 1; boot_mtr_stats.draws += L->list_draws;
    if (!L->cur) ++boot_mtr_stats.single_lists;
    if (L->tainted) ++boot_mtr_stats.tainted;
    if (FAILED(hr)) ++boot_mtr_stats.failed;
    boot_mtr_log();
    return hr;
}
static HRESULT STDMETHODCALLTYPE boot_mtr_Reset(ID3D12GraphicsCommandList *This, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso) {
    BOOT_MTR_SELF;
    if (L->open) return E_FAIL;
    if (!alloc) return E_INVALIDARG;
    boot_mtr_drain(L);   /* nothing of the previous list is still being recorded */
    if (!L->executed)   /* the previous list was never executed (a failed kick): its chunk allocators hold nothing in flight */
        for (unsigned k = 1; k <= L->cur && k < BOOT_MTR_CHUNKS; ++k)
            if (L->chunk[k].alloc_idx >= 0 && L->alloc[L->chunk[k].alloc_idx].fence == UINT64_MAX) L->alloc[L->chunk[k].alloc_idx].fence = 0;
    L->executed = 0; L->main_alloc = alloc;
    memset(&L->st, 0, sizeof L->st); L->st.pso = pso;
    L->cur = 0; L->tainted = L->oom = L->blocked_noted = 0; L->queries = L->events = L->split_barriers = L->pred_on = L->so_on = 0;
    L->chunk_draws = L->want = 0; L->list_draws = 0;
    for (unsigned t = 0; t < 3; ++t) { L->snap[t].cur = 0; L->snap[t].pos = 0; }
    boot_mtr_chunk_open(L, 0, NULL, 1, pso);
    L->open = 1;
    return S_OK;
}
static void STDMETHODCALLTYPE boot_mtr_ClearState(ID3D12GraphicsCommandList *This, ID3D12PipelineState *pso) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    ID3D12PipelineState **p = boot_mtr_put(L, BOOT_MTR_OP_CLEARSTATE, sizeof pso); if (p) *p = pso;
    L->tainted = 1;
}
static void STDMETHODCALLTYPE boot_mtr_DrawInstanced(ID3D12GraphicsCommandList *This, UINT a, UINT b, UINT c, UINT d) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_DRAW, 16); if (p) { p[0] = a; p[1] = b; p[2] = c; p[3] = d; }
    boot_mtr_drawn(L);
}
static void STDMETHODCALLTYPE boot_mtr_DrawIndexedInstanced(ID3D12GraphicsCommandList *This, UINT a, UINT b, UINT c, INT d, UINT e) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_DRAWIDX, 20); if (p) { p[0] = a; p[1] = b; p[2] = c; p[3] = (UINT)d; p[4] = e; }
    boot_mtr_drawn(L);
}
static void STDMETHODCALLTYPE boot_mtr_Dispatch(ID3D12GraphicsCommandList *This, UINT x, UINT y, UINT z) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_DISPATCH, 12); if (p) { p[0] = x; p[1] = y; p[2] = z; }
    boot_mtr_drawn(L);
}
static void STDMETHODCALLTYPE boot_mtr_CopyBufferRegion(ID3D12GraphicsCommandList *This, ID3D12Resource *dst, UINT64 doff, ID3D12Resource *src, UINT64 soff, UINT64 n) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrCopyBuf *p = boot_mtr_put(L, BOOT_MTR_OP_COPYBUF, sizeof *p); if (p) { p->dst = dst; p->dst_off = doff; p->src = src; p->src_off = soff; p->n = n; }
}
static void STDMETHODCALLTYPE boot_mtr_CopyTextureRegion(ID3D12GraphicsCommandList *This, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
                                                         const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrCopyTex *p = boot_mtr_put(L, BOOT_MTR_OP_COPYTEX, sizeof *p);
    if (p) { p->dst = *dst; p->src = *src; p->x = x; p->y = y; p->z = z; p->has_box = box != NULL; if (box) p->box = *box; else memset(&p->box, 0, sizeof p->box); }
}
static void STDMETHODCALLTYPE boot_mtr_CopyResource(ID3D12GraphicsCommandList *This, ID3D12Resource *dst, ID3D12Resource *src) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    ID3D12Resource **p = boot_mtr_put(L, BOOT_MTR_OP_COPYRES, 2 * sizeof dst); if (p) { p[0] = dst; p[1] = src; }
}
static void STDMETHODCALLTYPE boot_mtr_CopyTiles(ID3D12GraphicsCommandList *This, ID3D12Resource *tiled, const D3D12_TILED_RESOURCE_COORDINATE *coord,
                                                 const D3D12_TILE_REGION_SIZE *size, ID3D12Resource *buffer, UINT64 off, D3D12_TILE_COPY_FLAGS flags) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrCopyTiles *p = boot_mtr_put(L, BOOT_MTR_OP_COPYTILES, sizeof *p);
    if (p) { memset(p, 0, sizeof *p); p->tiled = tiled; p->buffer = buffer; if (coord) p->coord = *coord; if (size) p->size = *size; p->off = off; p->flags = flags; }
}
static void STDMETHODCALLTYPE boot_mtr_ResolveSubresource(ID3D12GraphicsCommandList *This, ID3D12Resource *dst, UINT dsub, ID3D12Resource *src, UINT ssub, DXGI_FORMAT f) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrResolve *p = boot_mtr_put(L, BOOT_MTR_OP_RESOLVESUB, sizeof *p); if (p) { p->dst = dst; p->src = src; p->dst_sub = dsub; p->src_sub = ssub; p->format = f; }
}
static void STDMETHODCALLTYPE boot_mtr_IASetPrimitiveTopology(ID3D12GraphicsCommandList *This, D3D12_PRIMITIVE_TOPOLOGY t) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_TOPO, 4); if (p) p[0] = (UINT)t;
    L->st.topo_set = 1; L->st.topo = t;
}
static void STDMETHODCALLTYPE boot_mtr_RSSetViewports(ID3D12GraphicsCommandList *This, UINT n, const D3D12_VIEWPORT *v) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = v ? (n <= 16 ? n : 16) : 0;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_VIEWPORTS, 8 + (size_t)k * sizeof *v);
    if (p) { p[0] = v ? k : n; p[1] = v != NULL; if (k) memcpy(p + 2, v, (size_t)k * sizeof *v); }
    if (n > 16 || (n && !v)) { L->tainted = 1; return; }
    L->st.vp_set = 1; L->st.nvp = n; if (n) memcpy(L->st.vp, v, (size_t)n * sizeof *v);
}
static void STDMETHODCALLTYPE boot_mtr_RSSetScissorRects(ID3D12GraphicsCommandList *This, UINT n, const D3D12_RECT *r) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = r ? (n <= 16 ? n : 16) : 0;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_SCISSORS, 8 + (size_t)k * sizeof *r);
    if (p) { p[0] = r ? k : n; p[1] = r != NULL; if (k) memcpy(p + 2, r, (size_t)k * sizeof *r); }
    if (n > 16 || (n && !r)) { L->tainted = 1; return; }
    L->st.sc_set = 1; L->st.nsc = n; if (n) memcpy(L->st.sc, r, (size_t)n * sizeof *r);
}
static void STDMETHODCALLTYPE boot_mtr_OMSetBlendFactor(ID3D12GraphicsCommandList *This, const FLOAT bf[4]) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_BLEND, 8 + 16); if (p) { p[0] = bf != NULL; p[1] = 0; if (bf) memcpy(p + 2, bf, 16); else memset(p + 2, 0, 16); }
    L->st.bf_set = 1; L->st.bf_null = bf == NULL; if (bf) memcpy(L->st.bf, bf, 16);
}
static void STDMETHODCALLTYPE boot_mtr_OMSetStencilRef(ID3D12GraphicsCommandList *This, UINT v) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_SREF, 4); if (p) p[0] = v;
    L->st.sref_set = 1; L->st.sref = v;
}
static void STDMETHODCALLTYPE boot_mtr_SetPipelineState(ID3D12GraphicsCommandList *This, ID3D12PipelineState *pso) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    ID3D12PipelineState **p = boot_mtr_put(L, BOOT_MTR_OP_PSO, sizeof pso); if (p) *p = pso;
    L->st.pso = pso;
}
static void STDMETHODCALLTYPE boot_mtr_ResourceBarrier(ID3D12GraphicsCommandList *This, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    if (!n || !b) { UINT *p = boot_mtr_put(L, BOOT_MTR_OP_BARRIER, 8); if (p) { p[0] = 0; p[1] = 0; } return; }
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_BARRIER, 8 + (size_t)n * sizeof *b);
    if (p) { p[0] = n; p[1] = 0; memcpy(p + 2, b, (size_t)n * sizeof *b); }
    for (UINT i = 0; i < n; ++i) {
        if (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) ++L->split_barriers;
        else if ((b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) && L->split_barriers) --L->split_barriers;
    }
}
static void STDMETHODCALLTYPE boot_mtr_ExecuteBundle(ID3D12GraphicsCommandList *This, ID3D12GraphicsCommandList *bundle) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    ID3D12GraphicsCommandList **p = boot_mtr_put(L, BOOT_MTR_OP_BUNDLE, sizeof bundle); if (p) *p = bundle;
    L->tainted = 1;   /* the bundle changes bound state the proxy does not see */
}
static void STDMETHODCALLTYPE boot_mtr_SetDescriptorHeaps(ID3D12GraphicsCommandList *This, UINT n, ID3D12DescriptorHeap *const *heaps) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = heaps ? (n <= 2 ? n : 2) : 0;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_HEAPS, 8 + (size_t)k * sizeof *heaps); if (p) { p[0] = k; p[1] = 0; if (k) memcpy(p + 2, heaps, (size_t)k * sizeof *heaps); }
    if (k != n) { L->tainted = 1; return; }
    if (L->st.nheaps != n || (n && memcmp(L->st.heaps, heaps, (size_t)n * sizeof *heaps))) {   /* other heaps: tables set before are not carried */
        for (unsigned g = 0; g < 2; ++g) { BootMtrRoot *R = g ? &L->st.c : &L->st.g;
            for (UINT i = 0; i < BOOT_MTR_ROOT; ++i) if (R->kind[i] == BOOT_MTR_RK_TABLE) { R->kind[i] = BOOT_MTR_RK_NONE; R->mask &= ~(1u << i); } }
    }
    L->st.nheaps = n; memset(L->st.heaps, 0, sizeof L->st.heaps); if (n) memcpy(L->st.heaps, heaps, (size_t)n * sizeof *heaps);
}
static inline void boot_mtr_rootsig(BootMtrList *L, int compute, ID3D12RootSignature *sig) {
    if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_ROOTSIG, 8 + sizeof sig); if (p) { p[0] = (UINT)compute; p[1] = 0; memcpy(p + 2, &sig, sizeof sig); }
    boot_mtr_root_sig(compute ? &L->st.c : &L->st.g, sig);
}
static void STDMETHODCALLTYPE boot_mtr_SetComputeRootSignature(ID3D12GraphicsCommandList *This, ID3D12RootSignature *s) { BOOT_MTR_SELF; boot_mtr_rootsig(L, 1, s); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRootSignature(ID3D12GraphicsCommandList *This, ID3D12RootSignature *s) { BOOT_MTR_SELF; boot_mtr_rootsig(L, 0, s); }
static inline void boot_mtr_table(BootMtrList *L, int compute, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (!boot_mtr_pre(L)) return;
    BootMtrRootArg *p = boot_mtr_put(L, BOOT_MTR_OP_TABLE, sizeof *p); if (p) { p->compute = (UINT)compute; p->index = i; p->v = h.ptr; }
    boot_mtr_root_view(L, compute ? &L->st.c : &L->st.g, i, BOOT_MTR_RK_TABLE, h.ptr);
}
static void STDMETHODCALLTYPE boot_mtr_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { BOOT_MTR_SELF; boot_mtr_table(L, 1, i, h); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { BOOT_MTR_SELF; boot_mtr_table(L, 0, i, h); }
static inline void boot_mtr_const1(BootMtrList *L, int compute, UINT i, UINT v, UINT off) {
    if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_CONST1, 16); if (p) { p[0] = (UINT)compute; p[1] = i; p[2] = v; p[3] = off; }
    boot_mtr_root_consts(L, compute ? &L->st.c : &L->st.g, i, 1, &v, off);
}
static void STDMETHODCALLTYPE boot_mtr_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList *This, UINT i, UINT v, UINT off) { BOOT_MTR_SELF; boot_mtr_const1(L, 1, i, v, off); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList *This, UINT i, UINT v, UINT off) { BOOT_MTR_SELF; boot_mtr_const1(L, 0, i, v, off); }
static inline void boot_mtr_consts(BootMtrList *L, int compute, UINT i, UINT n, const void *d, UINT off) {
    if (!boot_mtr_pre(L)) return;
    UINT k = d ? n : 0;
    BootMtrConsts *p = boot_mtr_put(L, BOOT_MTR_OP_CONSTS, sizeof *p + (size_t)k * 4);
    if (p) { p->compute = (UINT)compute; p->index = i; p->n = k; p->off = off; if (k) memcpy(p + 1, d, (size_t)k * 4); }
    if (k != n) { L->tainted = 1; return; }
    boot_mtr_root_consts(L, compute ? &L->st.c : &L->st.g, i, n, d, off);
}
static void STDMETHODCALLTYPE boot_mtr_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList *This, UINT i, UINT n, const void *d, UINT off) { BOOT_MTR_SELF; boot_mtr_consts(L, 1, i, n, d, off); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList *This, UINT i, UINT n, const void *d, UINT off) { BOOT_MTR_SELF; boot_mtr_consts(L, 0, i, n, d, off); }
static inline void boot_mtr_rootview(BootMtrList *L, int compute, UINT kind, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) {
    if (!boot_mtr_pre(L)) return;
    BootMtrRootView *p = boot_mtr_put(L, BOOT_MTR_OP_ROOTVIEW, sizeof *p); if (p) { p->compute = (UINT)compute; p->index = i; p->kind = kind; p->pad = 0; p->v = v; }
    boot_mtr_root_view(L, compute ? &L->st.c : &L->st.g, i, (uint8_t)kind, v);
}
static void STDMETHODCALLTYPE boot_mtr_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { BOOT_MTR_SELF; boot_mtr_rootview(L, 1, BOOT_MTR_RK_CBV, i, v); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { BOOT_MTR_SELF; boot_mtr_rootview(L, 0, BOOT_MTR_RK_CBV, i, v); }
static void STDMETHODCALLTYPE boot_mtr_SetComputeRootShaderResourceView(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { BOOT_MTR_SELF; boot_mtr_rootview(L, 1, BOOT_MTR_RK_SRV, i, v); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRootShaderResourceView(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { BOOT_MTR_SELF; boot_mtr_rootview(L, 0, BOOT_MTR_RK_SRV, i, v); }
static void STDMETHODCALLTYPE boot_mtr_SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { BOOT_MTR_SELF; boot_mtr_rootview(L, 1, BOOT_MTR_RK_UAV, i, v); }
static void STDMETHODCALLTYPE boot_mtr_SetGraphicsRootUnorderedAccessView(ID3D12GraphicsCommandList *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { BOOT_MTR_SELF; boot_mtr_rootview(L, 0, BOOT_MTR_RK_UAV, i, v); }
static void STDMETHODCALLTYPE boot_mtr_IASetIndexBuffer(ID3D12GraphicsCommandList *This, const D3D12_INDEX_BUFFER_VIEW *v) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_IB, 8 + sizeof *v); if (p) { p[0] = v != NULL; p[1] = 0; if (v) memcpy(p + 2, v, sizeof *v); else memset(p + 2, 0, sizeof *v); }
    L->st.ib_set = 1; L->st.ib_null = v == NULL; if (v) L->st.ib = *v;
}
static void STDMETHODCALLTYPE boot_mtr_IASetVertexBuffers(ID3D12GraphicsCommandList *This, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = v && n <= 32 ? n : 0;
    BootMtrViews *p = boot_mtr_put(L, BOOT_MTR_OP_VB, sizeof *p + (size_t)k * sizeof *v);
    if (p) { p->start = start; p->n = v ? k : n; p->has = v != NULL; p->pad = 0; if (k) memcpy(p + 1, v, (size_t)k * sizeof *v); }
    if (start > 32 || n > 32 - start || (v && k != n)) { L->tainted = 1; return; }
    for (UINT i = 0; i < n; ++i) { L->st.vb_mask |= 1u << (start + i); if (v) L->st.vb[start + i] = v[i]; else memset(&L->st.vb[start + i], 0, sizeof v[0]); }
}
static void STDMETHODCALLTYPE boot_mtr_SOSetTargets(ID3D12GraphicsCommandList *This, UINT start, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *v) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = v && n <= 4 ? n : 0;
    BootMtrViews *p = boot_mtr_put(L, BOOT_MTR_OP_SO, sizeof *p + (size_t)k * sizeof *v);
    if (p) { p->start = start; p->n = v ? k : n; p->has = v != NULL; p->pad = 0; if (k) memcpy(p + 1, v, (size_t)k * sizeof *v); }
    if (v && k != n) { L->tainted = 1; return; }
    int any = 0; for (UINT i = 0; v && i < n; ++i) if (v[i].SizeInBytes) any = 1;
    L->so_on = (unsigned)any;   /* the renderer binds slot 0 only (trace probes) and unbinds with (0, 0, NULL) */
}
static void STDMETHODCALLTYPE boot_mtr_OMSetRenderTargets(ID3D12GraphicsCommandList *This, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rtv, WINBOOL single,
                                                          const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrOm *p = boot_mtr_put(L, BOOT_MTR_OP_OMRT, sizeof *p); if (!p) return;
    memset(p, 0, sizeof *p);
    if (n > 8 || (n && !rtv)) { L->tainted = 1; ++boot_mtr_stats.snap_fail; p->n = 0; return; }
    if (n && !boot_mtr_snap(L, 0, rtv, n, single != 0, p->rtv)) {   /* no snapshot room: bind the views as they are now */
        ++boot_mtr_stats.snap_fail; UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(L->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < n; ++i) p->rtv[i].ptr = single ? rtv[0].ptr + (SIZE_T)i * inc : rtv[i].ptr; }
    p->n = n;
    if (dsv) { p->has_dsv = 1; if (!boot_mtr_snap(L, 1, dsv, 1, 1, &p->dsv)) { ++boot_mtr_stats.snap_fail; p->dsv = *dsv; } }
    L->st.om_set = 1; L->st.nrt = n; memcpy(L->st.rtv, p->rtv, sizeof p->rtv); L->st.has_dsv = p->has_dsv; L->st.dsv = p->dsv;
}
static void STDMETHODCALLTYPE boot_mtr_ClearDepthStencilView(ID3D12GraphicsCommandList *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                             UINT8 stencil, UINT n, const D3D12_RECT *rects) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = rects ? n : 0;
    BootMtrClearDsv *p = boot_mtr_put(L, BOOT_MTR_OP_CLEARDSV, sizeof *p + (size_t)k * sizeof *rects); if (!p) return;
    if (!boot_mtr_snap(L, 1, &dsv, 1, 1, &p->h)) { ++boot_mtr_stats.snap_fail; p->h = dsv; }
    p->flags = flags; p->depth = depth; p->stencil = stencil; p->n = k; if (k) memcpy(p + 1, rects, (size_t)k * sizeof *rects);
}
static void STDMETHODCALLTYPE boot_mtr_ClearRenderTargetView(ID3D12GraphicsCommandList *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT n, const D3D12_RECT *rects) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = rects ? n : 0;
    BootMtrClearRtv *p = boot_mtr_put(L, BOOT_MTR_OP_CLEARRTV, sizeof *p + (size_t)k * sizeof *rects); if (!p) return;
    if (!boot_mtr_snap(L, 0, &rtv, 1, 1, &p->h)) { ++boot_mtr_stats.snap_fail; p->h = rtv; }
    if (color) memcpy(p->color, color, sizeof p->color); else memset(p->color, 0, sizeof p->color);
    p->n = k; p->pad = 0; if (k) memcpy(p + 1, rects, (size_t)k * sizeof *rects);
}
static inline void boot_mtr_clear_uav(BootMtrList *L, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource *res, const void *values,
                                      UINT n, const D3D12_RECT *rects, int is_float) {
    if (!boot_mtr_pre(L)) return;
    UINT k = rects ? n : 0;
    BootMtrClearUav *p = boot_mtr_put(L, BOOT_MTR_OP_CLEARUAV, sizeof *p + (size_t)k * sizeof *rects); if (!p) return;
    p->gpu = gpu; p->res = res; p->n = k; p->is_float = (UINT)is_float;
    if (!boot_mtr_snap(L, 2, &cpu, 1, 1, &p->cpu)) { ++boot_mtr_stats.snap_fail; p->cpu = cpu; }
    if (values) memcpy(p->values, values, 16); else memset(p->values, 0, 16);
    if (k) memcpy(p + 1, rects, (size_t)k * sizeof *rects);
}
static void STDMETHODCALLTYPE boot_mtr_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                                    ID3D12Resource *res, const UINT values[4], UINT n, const D3D12_RECT *rects) {
    BOOT_MTR_SELF; boot_mtr_clear_uav(L, gpu, cpu, res, values, n, rects, 0);
}
static void STDMETHODCALLTYPE boot_mtr_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                                     ID3D12Resource *res, const float values[4], UINT n, const D3D12_RECT *rects) {
    BOOT_MTR_SELF; boot_mtr_clear_uav(L, gpu, cpu, res, values, n, rects, 1);
}
static void STDMETHODCALLTYPE boot_mtr_DiscardResource(ID3D12GraphicsCommandList *This, ID3D12Resource *res, const D3D12_DISCARD_REGION *region) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT k = region && region->pRects ? region->NumRects : 0;
    BootMtrDiscard *p = boot_mtr_put(L, BOOT_MTR_OP_DISCARD, sizeof *p + (size_t)k * sizeof(D3D12_RECT)); if (!p) return;
    p->res = res; p->has = region != NULL; p->nrects = k; p->first = region ? region->FirstSubresource : 0; p->count = region ? region->NumSubresources : 0;
    if (k) memcpy(p + 1, region->pRects, (size_t)k * sizeof(D3D12_RECT));
}
static inline void boot_mtr_query(BootMtrList *L, uint32_t op, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index, UINT count, ID3D12Resource *dst, UINT64 off) {
    if (!boot_mtr_pre(L)) return;
    BootMtrQuery *p = boot_mtr_put(L, op, sizeof *p); if (p) { p->heap = heap; p->type = type; p->index = index; p->count = count; p->dst = dst; p->off = off; }
}
static void STDMETHODCALLTYPE boot_mtr_BeginQuery(ID3D12GraphicsCommandList *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
    BOOT_MTR_SELF; boot_mtr_query(L, BOOT_MTR_OP_BEGINQ, heap, type, index, 0, NULL, 0);
    if (L->open && type != D3D12_QUERY_TYPE_TIMESTAMP) ++L->queries;   /* Begin and End stay in one list */
}
static void STDMETHODCALLTYPE boot_mtr_EndQuery(ID3D12GraphicsCommandList *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
    BOOT_MTR_SELF; boot_mtr_query(L, BOOT_MTR_OP_ENDQ, heap, type, index, 0, NULL, 0);
    if (type != D3D12_QUERY_TYPE_TIMESTAMP && L->queries) --L->queries;
}
static void STDMETHODCALLTYPE boot_mtr_ResolveQueryData(ID3D12GraphicsCommandList *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
                                                        ID3D12Resource *dst, UINT64 off) {
    BOOT_MTR_SELF; boot_mtr_query(L, BOOT_MTR_OP_RESOLVEQ, heap, type, start, count, dst, off);
}
static void STDMETHODCALLTYPE boot_mtr_SetPredication(ID3D12GraphicsCommandList *This, ID3D12Resource *buffer, UINT64 off, D3D12_PREDICATION_OP op) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrPred *p = boot_mtr_put(L, BOOT_MTR_OP_PRED, sizeof *p); if (p) { p->buffer = buffer; p->off = off; p->op = op; p->pad = 0; }
    L->pred_on = buffer != NULL;   /* never carried across a cut */
}
static inline void boot_mtr_marker(BootMtrList *L, UINT metadata, const void *data, UINT size, UINT begin) {
    if (!boot_mtr_pre(L)) return;
    UINT k = data ? size : 0;
    BootMtrMarker *p = boot_mtr_put(L, BOOT_MTR_OP_MARKER, sizeof *p + k); if (!p) return;
    p->metadata = metadata; p->size = k; p->begin = begin; p->pad = 0; if (k) memcpy(p + 1, data, k);
    if (begin) ++L->events;
}
static void STDMETHODCALLTYPE boot_mtr_SetMarker(ID3D12GraphicsCommandList *This, UINT metadata, const void *data, UINT size) { BOOT_MTR_SELF; boot_mtr_marker(L, metadata, data, size, 0); }
static void STDMETHODCALLTYPE boot_mtr_BeginEvent(ID3D12GraphicsCommandList *This, UINT metadata, const void *data, UINT size) { BOOT_MTR_SELF; boot_mtr_marker(L, metadata, data, size, 1); }
static void STDMETHODCALLTYPE boot_mtr_EndEvent(ID3D12GraphicsCommandList *This) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    UINT *p = boot_mtr_put(L, BOOT_MTR_OP_ENDEVENT, 0); (void)p;
    if (L->events) --L->events;
}
static void STDMETHODCALLTYPE boot_mtr_ExecuteIndirect(ID3D12GraphicsCommandList *This, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args, UINT64 args_off,
                                                       ID3D12Resource *count, UINT64 count_off) {
    BOOT_MTR_SELF; if (!boot_mtr_pre(L)) return;
    BootMtrExecInd *p = boot_mtr_put(L, BOOT_MTR_OP_EXECIND, sizeof *p);
    if (p) { p->sig = sig; p->max = max; p->pad = 0; p->args = args; p->args_off = args_off; p->count = count; p->count_off = count_off; }
    boot_mtr_drawn(L);   /* the renderer's signatures hold draw / dispatch arguments only (no state changes) */
}
static const ID3D12GraphicsCommandListVtbl boot_mtr_vtbl = {
    .QueryInterface = boot_mtr_QueryInterface, .AddRef = boot_mtr_AddRef, .Release = boot_mtr_Release,
    .GetPrivateData = boot_mtr_GetPrivateData, .SetPrivateData = boot_mtr_SetPrivateData, .SetPrivateDataInterface = boot_mtr_SetPrivateDataInterface,
    .SetName = boot_mtr_SetName, .GetDevice = boot_mtr_GetDevice, .GetType = boot_mtr_GetType,
    .Close = boot_mtr_Close, .Reset = boot_mtr_Reset, .ClearState = boot_mtr_ClearState,
    .DrawInstanced = boot_mtr_DrawInstanced, .DrawIndexedInstanced = boot_mtr_DrawIndexedInstanced, .Dispatch = boot_mtr_Dispatch,
    .CopyBufferRegion = boot_mtr_CopyBufferRegion, .CopyTextureRegion = boot_mtr_CopyTextureRegion, .CopyResource = boot_mtr_CopyResource,
    .CopyTiles = boot_mtr_CopyTiles, .ResolveSubresource = boot_mtr_ResolveSubresource, .IASetPrimitiveTopology = boot_mtr_IASetPrimitiveTopology,
    .RSSetViewports = boot_mtr_RSSetViewports, .RSSetScissorRects = boot_mtr_RSSetScissorRects, .OMSetBlendFactor = boot_mtr_OMSetBlendFactor,
    .OMSetStencilRef = boot_mtr_OMSetStencilRef, .SetPipelineState = boot_mtr_SetPipelineState, .ResourceBarrier = boot_mtr_ResourceBarrier,
    .ExecuteBundle = boot_mtr_ExecuteBundle, .SetDescriptorHeaps = boot_mtr_SetDescriptorHeaps,
    .SetComputeRootSignature = boot_mtr_SetComputeRootSignature, .SetGraphicsRootSignature = boot_mtr_SetGraphicsRootSignature,
    .SetComputeRootDescriptorTable = boot_mtr_SetComputeRootDescriptorTable, .SetGraphicsRootDescriptorTable = boot_mtr_SetGraphicsRootDescriptorTable,
    .SetComputeRoot32BitConstant = boot_mtr_SetComputeRoot32BitConstant, .SetGraphicsRoot32BitConstant = boot_mtr_SetGraphicsRoot32BitConstant,
    .SetComputeRoot32BitConstants = boot_mtr_SetComputeRoot32BitConstants, .SetGraphicsRoot32BitConstants = boot_mtr_SetGraphicsRoot32BitConstants,
    .SetComputeRootConstantBufferView = boot_mtr_SetComputeRootConstantBufferView, .SetGraphicsRootConstantBufferView = boot_mtr_SetGraphicsRootConstantBufferView,
    .SetComputeRootShaderResourceView = boot_mtr_SetComputeRootShaderResourceView, .SetGraphicsRootShaderResourceView = boot_mtr_SetGraphicsRootShaderResourceView,
    .SetComputeRootUnorderedAccessView = boot_mtr_SetComputeRootUnorderedAccessView, .SetGraphicsRootUnorderedAccessView = boot_mtr_SetGraphicsRootUnorderedAccessView,
    .IASetIndexBuffer = boot_mtr_IASetIndexBuffer, .IASetVertexBuffers = boot_mtr_IASetVertexBuffers, .SOSetTargets = boot_mtr_SOSetTargets,
    .OMSetRenderTargets = boot_mtr_OMSetRenderTargets, .ClearDepthStencilView = boot_mtr_ClearDepthStencilView,
    .ClearRenderTargetView = boot_mtr_ClearRenderTargetView, .ClearUnorderedAccessViewUint = boot_mtr_ClearUnorderedAccessViewUint,
    .ClearUnorderedAccessViewFloat = boot_mtr_ClearUnorderedAccessViewFloat, .DiscardResource = boot_mtr_DiscardResource,
    .BeginQuery = boot_mtr_BeginQuery, .EndQuery = boot_mtr_EndQuery, .ResolveQueryData = boot_mtr_ResolveQueryData,
    .SetPredication = boot_mtr_SetPredication, .SetMarker = boot_mtr_SetMarker, .BeginEvent = boot_mtr_BeginEvent, .EndEvent = boot_mtr_EndEvent,
    .ExecuteIndirect = boot_mtr_ExecuteIndirect,
};

/* ---- renderer hooks */
/* boot_render_begin_in, before the list is reset: r->cl (closed) becomes the proxy once. */
static inline void boot_mtr_install(BootRender *r) {
    if (!boot_mtr_on() || !r->cl || boot_mtr_of(r->cl) || r->recording) return;
    static int diag = -1;
    if (diag < 0) diag = boot_tile89_on() || boot_light84_on() || boot_light86_on() || boot_cbv_tap_registered;
    if (diag) { if (!boot_mtr_stats.diag_skips++) { FILE *lf = fopen("draw-path.log", "a"); if (lf) { fprintf(lf, "mt record: off (a diagnostic holding the render list is active)\n"); fclose(lf); } } return; }
    BootMtrList *L = (BootMtrList *)calloc(1, sizeof *L);
    if (!L) return;
    L->lpVtbl = &boot_mtr_vtbl; L->magic = BOOT_MTR_MAGIC; L->refs = 1; L->device = r->gpu->device; L->gpu = r->gpu;
    if (FAILED(ID3D12Device_CreateFence(L->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&L->fence)) || !L->fence) { free(L); return; }
    L->executed = 1;
    for (unsigned k = 0; k < BOOT_MTR_CHUNKS; ++k) L->chunk[k].alloc_idx = -1;
    L->lists[0] = r->cl;
    boot_mtr_pool_start();
    r->cl = (ID3D12GraphicsCommandList *)(void *)L;
    ++boot_mtr_stats.installs;
    FILE *lf = fopen("draw-path.log", "a");
    if (lf) { fprintf(lf, "mt record: on, %u workers, chunk %u draws, per-draw tables %s (TOTK_MT_RECORD=0 off)\n", boot_mtr_pool.threads, boot_mtr_chunk_base(),
                      boot_mtr_desc_on() ? "on the workers" : "on the render thread"); fclose(lf); }
}
/* boot_render_kick: executes the closed list (all of its chunk lists, in order). */
static inline void boot_mtr_execute(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *cl) {
    BootMtrList *L = boot_mtr_of(cl);
    if (!L) { ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl); return; }
    ID3D12CommandQueue_ExecuteCommandLists(queue, L->cur + 1, (ID3D12CommandList **)L->lists);
    /* the chunk allocators are free again when this signal completes (a failed signal never completes: they stay taken) */
    uint64_t v = ++L->fence_value; ID3D12CommandQueue_Signal(queue, L->fence, v);
    for (unsigned k = 1; k <= L->cur && k < BOOT_MTR_CHUNKS; ++k) if (L->chunk[k].alloc_idx >= 0) L->alloc[L->chunk[k].alloc_idx].fence = v;
    L->executed = 1;
}
/* The per-draw constant-buffer table (host_gpu_render_draw.h): created by the chunk's worker. 0 = not deferred. */
static inline int boot_mtr_cbv_table(BootRender *r, const D3D12_CONSTANT_BUFFER_VIEW_DESC *v, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE first) {
    BootMtrList *L = boot_mtr_of(r->cl);
    if (!L || !L->open || L->oom || !boot_mtr_desc_on() || !n) return 0;
    BootMtrDescCbvs *p = boot_mtr_put(L, BOOT_MTR_OP_DESC_CBVS, sizeof *p + (size_t)n * sizeof *v);
    if (!p) return 0;
    --L->chunk[L->cur].cmds;
    p->first = first.ptr; p->inc = r->srv_inc; p->n = n; memcpy(p + 1, v, (size_t)n * sizeof *v);
    ++boot_mtr_stats.desc_tables;
    return 1;
}
static inline int boot_mtr_srv(BootRender *r, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *d, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    BootMtrList *L = boot_mtr_of(r->cl);
    if (!L || !L->open || L->oom || !boot_mtr_desc_on()) return 0;
    BootMtrDescSrv *p = boot_mtr_put(L, BOOT_MTR_OP_DESC_SRV, sizeof *p);
    if (!p) return 0;
    --L->chunk[L->cur].cmds;
    memset(p, 0, sizeof *p); p->res = res; p->dst = dst.ptr; p->has_desc = d != NULL; if (d) p->desc = *d;
    ++boot_mtr_stats.desc_srvs;
    return 1;
}

/* ---------------------------------------------------------------- recording */
/* Segmented recording (module-static: BootRender keeps its layout across rebuilt runtime DLLs).
 * Segment s owns CBV descriptors [s*CBV_SEGMENT, (s+1)*CBV_SEGMENT) of the ring and bytes
 * [s*seg, (s+1)*seg) of the upload ring (seg = ring size / SEGMENTS), plus a command allocator
 * (segment 0 uses r->alloc). fence[s] is the queue fence value signalled after the last list
 * recorded in s; a segment is reused only once that value completed. A `whole` list (one draw
 * needing more than a segment) owns the entire rings. Invariant kept by every caller: when
 * r->recording is 0 the queue is idle (a kick is always followed by boot_render_begin). */
static struct {
    BootRender *r; ID3D12Device *device; ID3D12CommandAllocator *alloc[BOOT_RENDER_SEGMENTS];
    uint64_t fence[BOOT_RENDER_SEGMENTS], ring_total;
    unsigned cur, whole, unknown;
} boot_render_seg;
static inline uint64_t boot_render_seg_bytes(void) { return (boot_render_seg.ring_total / BOOT_RENDER_SEGMENTS) & ~UINT64_C(0xffff); }
/* Waits until the queue fence reached `value` (0: nothing to wait for). */
/* Per call site: fence waits that actually blocked (count, QPC ticks), logged with the native perf lines. */
static struct { int line; uint64_t count, qpc; } boot_render_wait_sites[32];
static inline void boot_render_wait_site(int line, uint64_t qpc) {
    for (unsigned i = 0; i < 32; ++i) {
        if (!boot_render_wait_sites[i].line) boot_render_wait_sites[i].line = line;
        if (boot_render_wait_sites[i].line == line) { ++boot_render_wait_sites[i].count; boot_render_wait_sites[i].qpc += qpc; return; }
    }
}
#define boot_render_wait_fence(g, value) boot_render_wait_fence_at((g), (value), __LINE__)
static inline int boot_render_wait_fence_at(BootGpu *g, uint64_t value, int line) {
    uint64_t done = ID3D12Fence_GetCompletedValue(g->fence);
    boot_light84_retire(g->fence, done);
    boot_light86_retire(g->fence, done);
    if (done == UINT64_MAX) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_WAIT_FIRST,.known=BOOT_RF98_FIRST|BOOT_RF98_REQUEST,.target=value,.first=done,.line=(unsigned)line};boot_rf98_note(&x);return 0; }
    if (done >= value) return 1;
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    uint64_t rf98_first=done;HRESULT rf98_event=ID3D12Fence_SetEventOnCompletion(g->fence, value, g->event);
    DWORD rf98_wait=0,rf98_error=0;unsigned rf98_known=BOOT_RF98_HR|BOOT_RF98_FIRST|BOOT_RF98_FINAL|BOOT_RF98_REQUEST;
    if(SUCCEEDED(rf98_event)) { rf98_wait=WaitForSingleObject(g->event, 30000);rf98_known|=BOOT_RF98_WAIT;
        if(rf98_wait==WAIT_FAILED){rf98_error=GetLastError();rf98_known|=BOOT_RF98_LAST_ERROR;} }
    int ok = SUCCEEDED(rf98_event) && rf98_wait == WAIT_OBJECT_0;
    done = ID3D12Fence_GetCompletedValue(g->fence);
    boot_light84_retire(g->fence, done);
    boot_light86_retire(g->fence, done);
    QueryPerformanceCounter(&b); boot_render_seg_wait_qpc += (uint64_t)(b.QuadPart - a.QuadPart); ++boot_render_seg_waits;
    boot_render_wait_site(line, (uint64_t)(b.QuadPart - a.QuadPart));
    if(!(ok && done != UINT64_MAX && done >= value)) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_WAIT_END,.known=rf98_known,.target=value,.first=rf98_first,.final=done,.hr=(uint32_t)rf98_event,.wait=rf98_wait,.last_error=rf98_error,.line=(unsigned)line};boot_rf98_note(&x); }
    return ok && done != UINT64_MAX && done >= value;
}
/* (Re)binds the segment state to render object `r` (first use by this module, or a recreated
 * device/renderer). Work submitted before is waited for; a list left open stays open and, when
 * executed, counts for every segment (its ring range is not known here). */
static inline int boot_render_seg_bind(BootRender *r) {
    if (boot_render_seg.r == r && boot_render_seg.device == r->gpu->device && boot_render_seg.alloc[0] == r->alloc) return 1;
    if (!r->recording && !boot_gpu_wait(r->gpu)) { BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_BIND_WAIT};boot_rf98_note(&x);return 0; }
    for (unsigned i = 1; i < BOOT_RENDER_SEGMENTS; ++i) if (boot_render_seg.alloc[i]) ID3D12CommandAllocator_Release(boot_render_seg.alloc[i]);
    memset(&boot_render_seg, 0, sizeof boot_render_seg);
    D3D12_RESOURCE_DESC rd; r->ring->lpVtbl->GetDesc(r->ring, &rd);
    boot_render_seg.ring_total = rd.Width;
    boot_render_seg.alloc[0] = r->alloc;
    for (unsigned i = 1; i < BOOT_RENDER_SEGMENTS; ++i) {
        HRESULT rf98_create=ID3D12Device_CreateCommandAllocator(r->gpu->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                       (void **)&boot_render_seg.alloc[i]);
        if (FAILED(rf98_create)) {
            BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_ALLOC_CREATE,.known=BOOT_RF98_HR,.hr=(uint32_t)rf98_create,.segment=i};boot_rf98_note(&x);
            for (unsigned k = 1; k < i; ++k) ID3D12CommandAllocator_Release(boot_render_seg.alloc[k]);
            memset(&boot_render_seg, 0, sizeof boot_render_seg); return 0;
        }
    }
    boot_render_seg.r = r; boot_render_seg.device = r->gpu->device;
    boot_render_seg.unknown = r->recording; boot_render_seg.cur = BOOT_RENDER_SEGMENTS - 1;
    return 1;
}
/* Opens a list in the next segment (or, with `whole`, over the entire rings after the queue idled). */
/* Region buffers left in COPY_DEST by GPU-ordered writes (boot_render_gpu_write) of the open list:
 * consecutive writes share one COMMON -> COPY_DEST transition, and every region goes back to COMMON in
 * one barrier before the next command that uses guest memory otherwise (draw, dispatch, copy out of a
 * region: boot_render_gw_settle) and before the list is closed, so other lists and the next list start
 * from COMMON as before. A list begun afresh has none (a discarded list executed nothing). */
static ID3D12Resource *boot_render_gw_dest[BOOT_RENDER_MAX_REGIONS]; static unsigned boot_render_gw_dest_count;
static uint64_t boot_render_gw_settles, boot_render_gw_transitions;
static inline void boot_render_gw_settle(BootRender *r) {
    if (!boot_render_gw_dest_count) return;
    D3D12_RESOURCE_BARRIER b[BOOT_RENDER_MAX_REGIONS]; memset(b, 0, sizeof(D3D12_RESOURCE_BARRIER) * boot_render_gw_dest_count);
    for (unsigned i = 0; i < boot_render_gw_dest_count; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[i].Transition.pResource = boot_render_gw_dest[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    if (r->recording) { BOOT_GP_RB("gw settle", r->cl, boot_render_gw_dest_count, b); ++boot_render_gw_settles; }
    boot_render_gw_dest_count = 0;
}
/* render-38c: GPU predication of the game's draws and clears under nvnCommandBufferSetRenderEnableConditional
 * (host_nvn_predicate.h evaluates the condition on the GPU into a slot of `buffer` and arms this). Only the D3D12
 * calls that execute a game draw or clear (boot_render_draw_impl's draw, boot_render_clear's clears and its masked
 * clear draw) are bracketed with SetPredication(buffer, offset, op) / SetPredication(NULL) while `armed`: whatever
 * is recorded around them (uploads, copies, barriers, render-target loads and write-backs, initial clears of new
 * targets, queries and their resolves, the game's compute, the predicate dispatch) is never predicated, and no list
 * is closed or reset with predication set, so nothing depends on predication state surviving a list. `state` is the
 * buffer's state in the open list: COMMON when a list is (re)opened (boot_render_pred_list_begin, both Reset
 * sites), put back to COMMON before it is closed (boot_render_kick), as boot_render_gw_settle does for regions. */
typedef struct BootRenderPred {
    ID3D12Resource *buffer; uint64_t offset; D3D12_PREDICATION_OP op; int armed;
    D3D12_RESOURCE_STATES state;
    uint64_t predicated, transitions;   /* D3D12 draws/clears recorded under predication; buffer barriers */
} BootRenderPred;
static BootRenderPred boot_render_pred;
static uint64_t boot_render_store_draws;   /* draws whose shaders store to guest memory (host_gpu_render_draw.h) */
/* render-38d: the indirect ring (host_gpu_render.h, indirect commands) and its state in the open list, kept as the predicate buffer's:
 * COMMON when a list is (re)opened, put back to COMMON before it is closed (boot_render_kick). */
static ID3D12Resource *boot_render_ind_ring; static D3D12_RESOURCE_STATES boot_render_ind_state;
static inline void boot_render_ind_ring_to(BootRender *r, D3D12_RESOURCE_STATES to) {
    if (!boot_render_ind_ring || boot_render_ind_state == to || !r->recording) return;
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b); b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = boot_render_ind_ring; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = boot_render_ind_state; b.Transition.StateAfter = to;
    BOOT_GP_RB("indirect ring", r->cl, 1, &b); boot_render_ind_state = to;
}
static inline void boot_render_pred_list_begin(void) { boot_render_pred.state = D3D12_RESOURCE_STATE_COMMON; boot_render_ind_state = D3D12_RESOURCE_STATE_COMMON; }
static inline void boot_render_pred_to(BootRender *r, D3D12_RESOURCE_STATES to) {
    if (!boot_render_pred.buffer || boot_render_pred.state == to || !r->recording) return;
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b); b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = boot_render_pred.buffer; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = boot_render_pred.state; b.Transition.StateAfter = to;
    BOOT_GP_RB("predicate state", r->cl, 1, &b); boot_render_pred.state = to; ++boot_render_pred.transitions;
}
/* before the game's draw/clear call: 1 = predication set (end it with boot_render_pred_end right after the call) */
static inline int boot_render_pred_begin(BootRender *r) {
    if (!boot_render_pred.armed || !boot_render_pred.buffer || !r->recording) return 0;
    boot_render_pred_to(r, D3D12_RESOURCE_STATE_PREDICATION);
    ID3D12GraphicsCommandList_SetPredication(r->cl, boot_render_pred.buffer, boot_render_pred.offset, boot_render_pred.op);
    ++boot_render_pred.predicated; return 1;
}
static inline void boot_render_pred_end(BootRender *r, int set) {
    if (set) ID3D12GraphicsCommandList_SetPredication(r->cl, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
}
static inline int boot_render_begin_in(BootRender *r, int whole) {
    if (r->recording) return 1;
    boot_render_gw_dest_count = 0;
    if (!boot_render_seg_bind(r)) return 0;
    unsigned s = (boot_render_seg.cur + 1) % BOOT_RENDER_SEGMENTS;
    uint64_t seg = boot_render_seg_bytes();
    if (whole || !seg) {
        for (unsigned i = 0; i < BOOT_RENDER_SEGMENTS; ++i) if (!boot_render_wait_fence(r->gpu, boot_render_seg.fence[i])) { BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_BEGIN_WAIT,.known=BOOT_RF98_REQUEST,.target=boot_render_seg.fence[i],.allocator=(uintptr_t)boot_render_seg.alloc[i],.segment=i,.whole=(unsigned)(whole || !seg)};boot_rf98_note(&x);return 0; }
    } else if (!boot_render_wait_fence(r->gpu, boot_render_seg.fence[s])) { BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_BEGIN_WAIT,.known=BOOT_RF98_REQUEST,.target=boot_render_seg.fence[s],.allocator=(uintptr_t)boot_render_seg.alloc[s],.segment=s};boot_rf98_note(&x);return 0; }
    boot_mtr_install(r);   /* heavy-work (TOTK_MT_RECORD): r->cl records through the multi-threaded proxy from here on */
    ID3D12CommandAllocator *alloc = boot_render_seg.alloc[s];
    HRESULT rf98_alloc=ID3D12CommandAllocator_Reset(alloc),rf98_list=0;unsigned rf98_list_called=0;
    if (FAILED(rf98_alloc) || FAILED((rf98_list_called=1,rf98_list=ID3D12GraphicsCommandList_Reset(r->cl, alloc, NULL)))) { BootRenderFailure98 x={BOOT_RF98_R(r),.stage=rf98_list_called?BOOT_RF98_LIST_RESET:BOOT_RF98_ALLOC_RESET,.known=BOOT_RF98_HR|BOOT_RF98_REQUEST,.hr=(uint32_t)(rf98_list_called?rf98_list:rf98_alloc),.target=boot_render_seg.fence[s],.allocator=(uintptr_t)alloc,.segment=s,.whole=(unsigned)(whole || !seg)};boot_rf98_note(&x);boot_light84_error(BOOT_L84_ABORT_RESET); boot_light86_error(BOOT_L86_ABORT_RESET); return 0; }
    boot_dred_reset(r->cl, r->gpu->queue, r->gpu->device, boot_render_exec_id, 1);
    boot_light84_reset(r, r->cl, alloc, s);
    boot_light86_reset(r, r->cl, alloc, s);
    ID3D12DescriptorHeap *heaps[2] = {r->srv_heap, r->sampler_heap};
    ID3D12GraphicsCommandList_SetDescriptorHeaps(r->cl, 2, heaps);
    boot_render_pred_list_begin();   /* render-38c: a new list, no predication, predicate buffer in COMMON */
    BOOT_GP_LIST_BEGIN(r->gpu, r->cl, "render list");   /* render-40: the list's first timestamp */
    boot_render_seg.cur = s; boot_render_seg.whole = whole || !seg;
    if (boot_render_seg.whole) {
        r->ring_used = 0; r->ring_size = boot_render_seg.ring_total; r->cbv_used = 0; boot_render_cbv_end = BOOT_RENDER_CBV_RING;
    } else {
        r->ring_used = (uint64_t)s * seg; r->ring_size = r->ring_used + seg;
        r->cbv_used = s * BOOT_RENDER_CBV_SEGMENT; boot_render_cbv_end = r->cbv_used + BOOT_RENDER_CBV_SEGMENT;
    }
    r->recording = 1; ++boot_render_cb_scope; ++boot_render_list_id; ++boot_render_bind_epoch;
    if(boot_cbv_tap_registered) { boot_render_cb_tap_ring=r->ring; boot_render_cb_tap_scope=boot_render_cb_scope;
        boot_render_cb_tap_start=r->ring_used; boot_render_cb_tap_end=r->ring_size; }
    return 1;
}
static inline int boot_render_begin(BootRender *r) { return boot_render_begin_in(r, 0); }
/* play (55b): TOTK_SYNC_WAIT_FENCE=1. Syncs then no longer idle the queue, so the invariant "not recording means
 * idle" (texture slot reuse, deferred releases, render-target eviction, feedback copies rely on it) is kept by
 * opening the next list right after every kick that used to be followed by a waiting sync. */
static inline int boot_sync_fence_on(void) { static int on = -1; if (on < 0) { const char *v = getenv("TOTK_SYNC_WAIT_FENCE"); on = v && !strcmp(v, "1"); } return on; }
static inline void boot_render_fb_signaled99(BootRender *r, uint64_t value);
/* drain-work (texture slot versioning, see boot_render_tex_dst): the resource a versioned rebuild replaces is held until
 * the lists that may still read it completed, then pooled or released as the rebuild asked. */
static struct { ID3D12Resource *res; uint64_t fence; int pool; } boot_render_sv_ret[BOOT_RENDER_SV_RETIRE]; static unsigned boot_render_sv_ret_n;
static ID3D12Resource *boot_render_sv_hold; static uint64_t boot_render_sv_hold_fence; static BootRender *boot_render_sv_hold_r;
static inline void boot_render_tex_pool_put(ID3D12Resource *t);
/* Held resources whose fence is reached (all: the queue is idle) go to the texture pool or are released. */
static inline void boot_render_sv_retire_poll(BootRender *r, int all) {
    if (!boot_render_sv_ret_n) return;
    const uint64_t done = all ? UINT64_MAX : ID3D12Fence_GetCompletedValue(r->gpu->fence);
    unsigned w = 0;
    for (unsigned i = 0; i < boot_render_sv_ret_n; ++i) {
        if (boot_render_sv_ret[i].fence > done) { boot_render_sv_ret[w++] = boot_render_sv_ret[i]; continue; }
        if (boot_render_sv_ret[i].pool) boot_render_tex_pool_put(boot_render_sv_ret[i].res); else ID3D12Resource_Release(boot_render_sv_ret[i].res);
    }
    boot_render_sv_ret_n = w;
}
/* Called where a rebuild drops the slot's previous resource (pool: to the texture pool, else released): 1 = taken, held
 * until the fence of its lists; 0 = dispose of it now (not the versioned slot's resource, or its lists completed). */
static inline int boot_render_sv_take(ID3D12Resource *t, int pool) {
    if (!t || t != boot_render_sv_hold || !boot_render_sv_hold_r) return 0;
    BootRender *r = boot_render_sv_hold_r; const uint64_t f = boot_render_sv_hold_fence; boot_render_sv_hold = NULL;
    if (ID3D12Fence_GetCompletedValue(r->gpu->fence) >= f) return 0;
    if (boot_render_sv_ret_n == BOOT_RENDER_SV_RETIRE) {   /* full: wait for its lists, then as before */
        ++boot_render_sv_retire_waits; LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        boot_render_wait_fence(r->gpu, f);
        QueryPerformanceCounter(&b); boot_render_sv_wait_qpc += (uint64_t)(b.QuadPart - a.QuadPart);
        boot_render_sv_retire_poll(r, 0);
        return 0;
    }
    boot_render_sv_ret[boot_render_sv_ret_n].res = t; boot_render_sv_ret[boot_render_sv_ret_n].fence = f; boot_render_sv_ret[boot_render_sv_ret_n].pool = pool;
    ++boot_render_sv_ret_n; ++boot_render_sv_held;
    return 1;
}
/* The staged descriptors in place, in the order written, once every list that may read the old ones completed (waited
 * for when needed). Called before every list execution (boot_render_kick), when the staging heap is full (full = 1) and
 * when the queue is idle. A failed wait (device removed) still copies: nothing runs on that device any more. */
static inline int boot_render_sv_apply(BootRender *r, int full) {
    int ok = 1;
    if (boot_render_sv_n || boot_render_sv_idx_n) {
        if (boot_render_sv_guard && ID3D12Fence_GetCompletedValue(r->gpu->fence) < boot_render_sv_guard) {
            LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
            ok = boot_render_wait_fence(r->gpu, boot_render_sv_guard);
            QueryPerformanceCounter(&b); boot_render_sv_wait_qpc += (uint64_t)(b.QuadPart - a.QuadPart);
            if (full) ++boot_render_sv_full_waits; else ++boot_render_sv_kick_waits;
        }
        if (boot_render_sv_heap && boot_render_sv_dev == r->gpu->device) {
            D3D12_CPU_DESCRIPTOR_HANDLE s; boot_render_sv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(boot_render_sv_heap, &s);
            for (unsigned k = 0; k < boot_render_sv_n; ++k) {
                D3D12_CPU_DESCRIPTOR_HANDLE src; src.ptr = s.ptr + (SIZE_T)k * boot_render_sv_inc;
                ID3D12Device_CopyDescriptorsSimple(r->gpu->device, 1, boot_render_cpu(r, r->srv_heap, r->srv_inc,
                    BOOT_RENDER_HEAP_TEX0 + boot_render_sv_ent[k].dim * BOOT_RENDER_TEX_SLOTS + boot_render_sv_ent[k].index), src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
        for (unsigned k = 0; k < boot_render_sv_idx_n; ++k) boot_render_sv_flag[boot_render_sv_idx[k]] = 0;
        boot_render_sv_n = 0; boot_render_sv_idx_n = 0; boot_render_sv_guard = 0; ++boot_render_sv_applies;
    }
    boot_render_sv_retire_poll(r, 0);
    return ok;
}
/* Device recovery: staged descriptors and held resources belong to the removed device. */
static inline void boot_render_sv_reset(void) {
    for (unsigned k = 0; k < boot_render_sv_idx_n; ++k) boot_render_sv_flag[boot_render_sv_idx[k]] = 0;
    boot_render_sv_n = 0; boot_render_sv_idx_n = 0; boot_render_sv_guard = 0;
    for (unsigned i = 0; i < boot_render_sv_ret_n; ++i) ID3D12Resource_Release(boot_render_sv_ret[i].res);
    boot_render_sv_ret_n = 0; boot_render_sv_hold = NULL; boot_render_sv_hold_r = NULL;
    if (boot_render_sv_heap) { ID3D12DescriptorHeap_Release(boot_render_sv_heap); boot_render_sv_heap = NULL; }
    boot_render_sv_dev = NULL;
    memset(boot_render_slot_rec, 0, sizeof boot_render_slot_rec); memset(boot_render_up_lid, 0, sizeof boot_render_up_lid);   /* drain2-work: lists of the removed device */
}
/* Closes and executes the open list without waiting; its segment's fence is signalled after it. */
static inline int boot_render_kick(BootRender *r) {
    if (!r->recording) return 1;
    if (boot_render_sv_n || boot_render_sv_idx_n || boot_render_sv_ret_n) boot_render_sv_apply(r, 0);   /* drain-work: versioned slots' descriptors in place first */
    boot_render_occl_resolve(r);
    boot_render_gw_settle(r);   /* regions leave the list in COMMON */
    boot_render_pred_to(r, D3D12_RESOURCE_STATE_COMMON);   /* render-38c: and the predicate buffer */
    boot_render_ind_ring_to(r, D3D12_RESOURCE_STATE_COMMON);   /* render-38d: and the indirect ring */
    BOOT_GP_LIST_END(r->cl);   /* render-40: last timestamp + resolve of the list's timestamps */
    r->recording = 0; r->attribs_dirty_in_list = 0;
    HRESULT rf98_close=ID3D12GraphicsCommandList_Close(r->cl);
    if (FAILED(rf98_close)) { BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_KICK_CLOSE,.known=BOOT_RF98_HR,.hr=(uint32_t)rf98_close,.allocator=(uintptr_t)boot_render_seg.alloc[boot_render_seg.cur],.segment=boot_render_seg.cur,.whole=boot_render_seg.whole};boot_rf98_note(&x);boot_light84_error(BOOT_L84_ABORT_CLOSE); boot_light86_error(BOOT_L86_ABORT_CLOSE); return 0; }
    boot_dred_execute(r->cl, r->gpu->queue, 1);
    boot_mtr_execute(r->gpu->queue, r->cl);   /* heavy-work: the list, or the chunk lists of a multi-threaded recording in order */
    BootGpu *g = r->gpu;
    if (g->submitted == UINT64_MAX) { boot_light84_error(BOOT_L84_ABORT_SIGNAL); boot_light86_error(BOOT_L86_ABORT_SIGNAL); return 0; }
    uint64_t value = ++g->submitted;
    HRESULT tile89_signal_result=ID3D12CommandQueue_Signal(g->queue, g->fence, value);
    boot_tile89_signal(r->cl,g->queue,g->fence,value,tile89_signal_result);
    if (FAILED(tile89_signal_result)) { BootRenderFailure98 x={BOOT_RF98_R(r),.stage=BOOT_RF98_KICK_SIGNAL,.known=BOOT_RF98_HR|BOOT_RF98_REQUEST,.hr=(uint32_t)tile89_signal_result,.target=value,.segment=boot_render_seg.cur};boot_rf98_note(&x);boot_light84_error(BOOT_L84_ABORT_SIGNAL); boot_light86_error(BOOT_L86_ABORT_SIGNAL); return 0; }
    boot_render_fb_signaled99(r, value);   /*99: original Signal succeeded*/
    BOOT_GP_LIST_EXEC(r->cl, value);   /* render-40 */
    boot_light84_fence(r, r->cl, g->fence, value);
    boot_light86_fence(r, r->cl, g->fence, value);
    boot_render_list_fence[boot_render_list_id & 65535].id = boot_render_list_id; boot_render_list_fence[boot_render_list_id & 65535].fence = value;
    if (boot_render_seg.r != r || boot_render_seg.whole || boot_render_seg.unknown) {
        for (unsigned i = 0; i < BOOT_RENDER_SEGMENTS; ++i) boot_render_seg.fence[i] = value;
        boot_render_seg.unknown = 0;
    } else boot_render_seg.fence[boot_render_seg.cur] = value;
    for (unsigned i = boot_render_gw_count; i-- > 0 && !boot_render_gw[i].fence;) boot_render_gw[i].fence = value;   /* GPU writes of this list */
    if (boot_render_up_ring.cur < boot_render_up_ring.count) boot_render_up_lid[boot_render_up_ring.cur] = boot_render_list_id + 1;   /* drain2-work: the next list goes on filling it */
    boot_render_release_poll(r);   /* drain2-work */
    return 1;
}
/* A full segment: execute it and continue recording in the next one without waiting for it. On
 * failure the queue is waited for, so the "not recording means idle" invariant holds. */
static inline int boot_render_flush(BootRender *r) {
    ++boot_render_kicks;
    if (boot_render_kick(r) && boot_render_begin(r)) return 1;
    boot_gpu_wait(r->gpu);
    return 0;
}
static inline void boot_render_idle_cleanup(BootRender *r);
static inline int boot_render_submit_impl(BootRender *r) {
    if (!r->recording) return 1;
    if (!boot_render_kick(r)) return 0;
    if (!boot_gpu_wait(r->gpu)) return 0;
    boot_render_idle_cleanup(r);
    return 1;
}
/* Queue idle (native NVN path: syncs wait here, nothing is recording since submissions only execute):
 * executes an open list, waits for the queue and releases what waited for completion. */
static inline int boot_render_idle(BootRender *r) {
    if (r->recording) return boot_render_submit(r);
    if (!boot_gpu_wait(r->gpu)) return 0;
    boot_render_idle_cleanup(r);
    return 1;
}
static inline void boot_render_idle_cleanup(BootRender *r) {
    for (unsigned i = 0; i < boot_render_upload_count; ++i) ID3D12Resource_Release(boot_render_uploads[i]);
    boot_render_upload_count = 0; boot_render_upload_bytes = 0;
    boot_render_staging_reset(&boot_render_up_ring);   /* every upload recorded so far has executed */
    boot_render_release_deferred();
    boot_render_sv_apply(r, 0); boot_render_sv_retire_poll(r, 1); boot_render_sv_hold = NULL;   /* drain-work: idle: nothing staged or held is in use */
    boot_render_gmem_pending = 0;   /* every guest-memory store recorded so far has completed */
    boot_ds_idle();   /* data-snapshot: storing draws' and dispatches' storage buffers have completed too (v2fix: before the GPU-write table empties) */
    boot_render_gw_count = 0; boot_render_gw_all = 0;
    if (boot_render_idle_hook) boot_render_idle_hook(r);
    boot_render_uav_pending = 0;
    boot_render_gt_npending = 0;    /* and every GPU render-target write-back */
    /* idle: the whole ring is free again (a module that does not segment sees its full size) */
    if (boot_render_seg.r == r && boot_render_seg.ring_total) r->ring_size = boot_render_seg.ring_total;
    boot_ax_idle_mark();   /* async-xlate: items before the one X runs are complete, their deferred stores applied */
}

static inline int boot_render_rt_barrier_at(BootRender *r, BootRenderTarget *t, D3D12_RESOURCE_STATES to, const char *file, unsigned line) {
    if (t->state == to) return 1;
    /* a write-back earlier in this draw may have submitted the list: record into an open one */
    if (!r->recording && !boot_render_begin(r)) return 0;
    D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t->texture; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t->state; b.Transition.StateAfter = to;
    boot_gp_rb(BOOT_GP_BARRIER, "rt state", file, line, r->cl, 1, &b); t->state = to; return 1;
}

/* ---------------------------------------------------------------- GPU render-target write-back */
/* Render-target write-backs and reloads that only feed other GPU work (an evicted target, an
 * aliased target drawn after another one over the same memory, a new target over memory a target
 * still holds) run on the GPU: the target is copied to linear rows and tiled into the zero-copy
 * guest region by a compute shader, and the reloaded target is detiled from it the same way
 * (tile-shaders/tile.hlsl, DXIL in host_gpu_tile_dxil.h), in a list executed after the recorded
 * work, without waiting for it. The guest ranges such a list reads or writes stay pending until its
 * fence completes: a CPU access to guest memory through boot_render_guest that overlaps one waits
 * for that fence first (boot_render_gt_guard), so the CPU sees the written-back image and its own
 * writes land after it. A waiting submission completes everything. Anything the GPU path cannot do
 * (no contiguous zero-copy range, objects that fail to create) uses the CPU write-back. */
#include "host_gpu_tile_dxil.h"
#ifndef BOOT_RENDER_GT_ALLOCS
#define BOOT_RENDER_GT_ALLOCS 32   /* play: was 8; allocator reuse waited on the list 8 back */
#endif
#ifndef BOOT_RENDER_GT_CHUNKS
#define BOOT_RENDER_GT_CHUNKS 16   /* play: was 4 (32 MiB each); <= 32, chunk masks are 32-bit */
#endif
#define BOOT_RENDER_GT_CHUNK_MIN (UINT64_C(32) << 20)
static struct {
    ID3D12Device *device; ID3D12RootSignature *root; ID3D12PipelineState *tile, *detile; ID3D12GraphicsCommandList *cl; int failed;
    ID3D12CommandAllocator *alloc[BOOT_RENDER_GT_ALLOCS]; uint64_t alloc_fence[BOOT_RENDER_GT_ALLOCS]; unsigned next_alloc;
    ID3D12Resource *chunk[BOOT_RENDER_GT_CHUNKS]; uint64_t chunk_size[BOOT_RENDER_GT_CHUNKS], chunk_fence[BOOT_RENDER_GT_CHUNKS]; unsigned chunks, cur; uint64_t off;
} boot_render_gt;
static uint64_t boot_render_gt_lists, boot_render_gt_writebacks, boot_render_gt_loads, boot_render_gt_guard_waits, boot_render_gt_resumed;
static inline void boot_render_gt_release(void) {
    if (boot_render_gt.cl) ID3D12GraphicsCommandList_Release(boot_render_gt.cl);
    for (unsigned i = 0; i < BOOT_RENDER_GT_ALLOCS; ++i) if (boot_render_gt.alloc[i]) ID3D12CommandAllocator_Release(boot_render_gt.alloc[i]);
    for (unsigned i = 0; i < boot_render_gt.chunks; ++i) if (boot_render_gt.chunk[i]) ID3D12Resource_Release(boot_render_gt.chunk[i]);
    if (boot_render_gt.tile) ID3D12PipelineState_Release(boot_render_gt.tile);
    if (boot_render_gt.detile) ID3D12PipelineState_Release(boot_render_gt.detile);
    if (boot_render_gt.root) ID3D12RootSignature_Release(boot_render_gt.root);
    memset(&boot_render_gt, 0, sizeof boot_render_gt);
    boot_render_gt_npending = 0;
}
static inline int boot_render_gt_open(BootRender *r) {
    if (boot_render_gt.device == r->gpu->device) return !boot_render_gt.failed;
    if (boot_render_gt.device) boot_render_gt_release();   /* objects of another (removed) device */
    boot_render_gt.device = r->gpu->device; boot_render_gt.failed = 1;
    D3D12_ROOT_PARAMETER1 p[3]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 8;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor.ShaderRegister = 0; p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.ShaderRegister = 1; p[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    for (unsigned i = 0; i < 3; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 3; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) { if (err) ID3D10Blob_Release(err); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(r->gpu->device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                  &IID_ID3D12RootSignature, (void **)&boot_render_gt.root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(boot_render_gt.root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { boot_render_gt.root = NULL; return 0; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd; memset(&cd, 0, sizeof cd); cd.pRootSignature = boot_render_gt.root;
    cd.CS.pShaderBytecode = boot_gt_tile_dxil; cd.CS.BytecodeLength = sizeof boot_gt_tile_dxil;
    if (FAILED(boot_psoc_create_compute(r, r->gpu->device, &cd, &boot_render_gt.tile))) { boot_render_gt.tile = NULL; return 0; }
    cd.CS.pShaderBytecode = boot_gt_detile_dxil; cd.CS.BytecodeLength = sizeof boot_gt_detile_dxil;
    if (FAILED(boot_psoc_create_compute(r, r->gpu->device, &cd, &boot_render_gt.detile))) { boot_render_gt.detile = NULL; return 0; }
    for (unsigned i = 0; i < BOOT_RENDER_GT_ALLOCS; ++i)
        if (FAILED(ID3D12Device_CreateCommandAllocator(r->gpu->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&boot_render_gt.alloc[i]))) {
            boot_render_gt.alloc[i] = NULL; return 0; }
    if (FAILED(ID3D12Device_CreateCommandList(r->gpu->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, boot_render_gt.alloc[0], NULL, &IID_ID3D12GraphicsCommandList,
                                              (void **)&boot_render_gt.cl))) { boot_render_gt.cl = NULL; return 0; }
    boot_dred_created(boot_render_gt.cl);
    ID3D12GraphicsCommandList_Close(boot_render_gt.cl);
    boot_render_gt.failed = 0;
    return 1;
}
/* Linear staging chunk k (DEFAULT heap, UAV capable) of `size` bytes, replacing an idle one. */
static inline int boot_render_gt_chunk_new(BootRender *r, unsigned k, uint64_t size) {
    if (boot_render_gt.chunk[k]) { ID3D12Resource_Release(boot_render_gt.chunk[k]); boot_render_gt.chunk[k] = NULL; }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                    &IID_ID3D12Resource, (void **)&boot_render_gt.chunk[k]))) { boot_render_gt.chunk[k] = NULL; boot_render_gt.chunk_size[k] = 0; return 0; }
    ++boot_render_created_buffers;
    boot_render_gt.chunk_size[k] = size; boot_render_gt.chunk_fence[k] = 0;
    return 1;
}
/* `bytes` at a 512-byte aligned offset of a linear staging chunk no unfinished list uses; *chunk is
 * its index. Chunks are created up to BOOT_RENDER_GT_CHUNKS, then reused round robin after their
 * last list's fence; `mask` holds the chunks this operation already took (never reset under it).
 * 0 on failure. */
static inline int boot_render_gt_alloc(BootRender *r, uint64_t bytes, unsigned *mask, unsigned *chunk, uint64_t *offset) {
    uint64_t size = bytes > BOOT_RENDER_GT_CHUNK_MIN ? (bytes + 0xffff) & ~UINT64_C(0xffff) : BOOT_RENDER_GT_CHUNK_MIN;
    for (unsigned tries = 0; tries <= BOOT_RENDER_GT_CHUNKS; ++tries) {
        unsigned c = boot_render_gt.cur;
        if (c < boot_render_gt.chunks) {
            uint64_t at = (boot_render_gt.off + 511) & ~UINT64_C(511);
            if (at <= boot_render_gt.chunk_size[c] && bytes <= boot_render_gt.chunk_size[c] - at) {
                boot_render_gt.off = at + bytes; *chunk = c; *offset = at; *mask |= 1u << c; return 1; }
        }
        unsigned k = boot_render_gt.chunks < BOOT_RENDER_GT_CHUNKS ? boot_render_gt.chunks : (c + 1) % boot_render_gt.chunks;
        if (*mask & (1u << k)) return 0;   /* would reuse a chunk this operation still needs */
        if (k == boot_render_gt.chunks) { if (!boot_render_gt_chunk_new(r, k, size)) return 0; boot_render_gt.chunks = k + 1; }
        else {
            if (!boot_render_wait_fence(r->gpu, boot_render_gt.chunk_fence[k])) return 0;
            if (bytes > boot_render_gt.chunk_size[k] && !boot_render_gt_chunk_new(r, k, size)) return 0;
        }
        boot_render_gt.cur = k; boot_render_gt.off = 0;
    }
    return 0;
}
/* Waits for every executed tiling list that reads or writes guest VA [va, va+size). */
static inline void boot_render_gt_guard(BootRender *r, uint64_t va, uint64_t size) {
    if (size < (UINT64_C(1) << 28) && va < (UINT64_C(1) << 48)) {   /* fast path: no pending list writes these chunks */
        uint64_t need = 0, last = (va + (size ? size : 1) - 1) >> 16;
        for (uint64_t c = va >> 16; c <= last; ++c) { uint64_t f = boot_render_gt_chunk_fence[c & (BOOT_RENDER_GT_CHUNK_TABLE - 1)]; if (f > need) need = f; }
        if (need <= boot_render_gt_done_seen) { ++boot_render_gt_fast; return; }
        boot_render_gt_done_seen = ID3D12Fence_GetCompletedValue(r->gpu->fence);
        if (need <= boot_render_gt_done_seen) { ++boot_render_gt_fast; return; }
    }
    ++boot_render_gt_slow;
    uint64_t end = va + (size ? size : 1), done = ID3D12Fence_GetCompletedValue(r->gpu->fence);
    unsigned k = 0;
    for (unsigned i = 0; i < boot_render_gt_npending; ++i) {
        uint64_t f = boot_render_gt_pending[i].fence;
        if (f > done && boot_render_gt_pending[i].va < end && va < boot_render_gt_pending[i].end) {
            ++boot_render_gt_guard_waits;
            if (boot_render_wait_fence(r->gpu, f)) done = ID3D12Fence_GetCompletedValue(r->gpu->fence);
        }
        if (f > done) boot_render_gt_pending[k++] = boot_render_gt_pending[i];
    }
    boot_render_gt_npending = k; if (done > boot_render_gt_done_seen) boot_render_gt_done_seen = done;
}
static inline void boot_render_gt_pend(BootRender *r, uint64_t va, uint64_t size, uint64_t fence) {
    if (boot_render_gt_npending == BOOT_RENDER_GT_PENDING) boot_render_gt_guard(r, boot_render_gt_pending[0].va, boot_render_gt_pending[0].end - boot_render_gt_pending[0].va);
    if (boot_render_gt_npending == BOOT_RENDER_GT_PENDING) {   /* only completed ones were dropped: wait for the oldest */
        boot_render_wait_fence(r->gpu, boot_render_gt_pending[0].fence);
        boot_render_gt_guard(r, 0, UINT64_MAX / 2);
    }
    for (uint64_t c = va >> 16, last = (va + (size ? size : 1) - 1) >> 16, n = 0; c <= last; ++c, ++n) {
        if (n == BOOT_RENDER_GT_CHUNK_TABLE) { for (unsigned j = 0; j < BOOT_RENDER_GT_CHUNK_TABLE; ++j) if (boot_render_gt_chunk_fence[j] < fence) boot_render_gt_chunk_fence[j] = fence; break; }
        uint64_t *t = &boot_render_gt_chunk_fence[c & (BOOT_RENDER_GT_CHUNK_TABLE - 1)]; if (*t < fence) *t = fence;
    }
    if (fence > boot_render_gt_newest) boot_render_gt_newest = fence;
    ++boot_render_gt_gen;
    boot_render_gt_pending[boot_render_gt_npending].va = va; boot_render_gt_pending[boot_render_gt_npending].end = va + size;
    boot_render_gt_pending[boot_render_gt_npending++].fence = fence;
}
/* GPU VA of guest range [va, va+size) inside one zero-copy region (contiguous pages, 4-byte
 * aligned), or 0. */
static inline D3D12_GPU_VIRTUAL_ADDRESS boot_render_gt_guest_va(BootRender *r, uint64_t va, uint64_t size) {
    if ((va & 3) || !size || (va >> 12) < r->va_base_page) return 0;
    uint64_t page = (va >> 12) - r->va_base_page;
    if (page >= r->ptable_pages) return 0;
    uint32_t e = r->ptable_cpu[page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count || !r->region[e >> 24]) return 0;
    unsigned region = e >> 24; uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    if (off + size > r->region_size[region]) return 0;
    for (uint64_t p = 1; p <= ((va & 0xfff) + size - 1) >> 12; ++p)
        if (page + p >= r->ptable_pages || r->ptable_cpu[page + p] != e + (uint32_t)p) return 0;
    return ID3D12Resource_GetGPUVirtualAddress(r->region[region]) + off;
}
/* Whether an executed, unfinished tiling list reads or writes guest VA [va, va+size). */
static inline int boot_render_gt_overlaps(BootRender *r, uint64_t va, uint64_t size) {
    if (!boot_render_gt_npending) return 0;
    uint64_t done = ID3D12Fence_GetCompletedValue(r->gpu->fence);
    for (unsigned i = 0; i < boot_render_gt_npending; ++i)
        if (boot_render_gt_pending[i].fence > done && boot_render_gt_pending[i].va < va + size && va < boot_render_gt_pending[i].end) return 1;
    return 0;
}
/* Writes dirty targets idx[0..n) back to guest memory (in that order) and then, when load >= 0,
 * loads target `load` (block height/depth log_h/log_d) from guest memory, on the GPU after all
 * recorded work, without waiting. Returns 0 with nothing recorded when the GPU path cannot do it. */
static inline int boot_render_gt_run(BootRender *r, const unsigned *idx, unsigned n, int load, unsigned load_h, unsigned load_d) {
    BootRenderRtTable *T = boot_render_rtab(r);
    if (!boot_render_gt_open(r)) return 0;
    struct { BootRenderTarget *x; D3D12_GPU_VIRTUAL_ADDRESS guest; uint64_t extent, at; unsigned chunk, h, d; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; } op[17];
    unsigned ops = 0, mask = 0;
    if (n > 16) return 0;
    for (unsigned j = 0; j <= n; ++j) {
        BootRenderTarget *x; unsigned h, d;
        if (j < n) {
            unsigned i = idx[j]; BootRenderRtInfo *in = &T->info[i]; x = &T->rts[i];
            if (!in->dirty) continue;
            if (!x->texture || !in->known || boot_render_rt_untiled(x)) return 0;
            h = in->log_gob_h; d = in->log_gob_d;
        } else {
            if (load < 0) break;
            x = &T->rts[load]; h = load_h; d = load_d;
            if (!x->texture || boot_render_rt_untiled(x)) return 0;
        }
        uint32_t wb = x->width * boot_render_dxgi_bytes(x->dxgi);
        op[ops].x = x; op[ops].h = h; op[ops].d = d;
        op[ops].extent = boot_texture_slice_extent(wb, x->height, h, d);
        op[ops].guest = op[ops].extent ? boot_render_gt_guest_va(r, x->address, op[ops].extent) : 0;
        if (!op[ops].guest || op[ops].extent > UINT32_MAX) return 0;
        D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd); UINT rows; UINT64 rs, total;
        ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &op[ops].fp, &rows, &rs, &total);
        if (!boot_render_gt_alloc(r, total, &mask, &op[ops].chunk, &op[ops].at)) return 0;
        ++ops;
    }
    if (!ops) return 1;
    /* after the recorded work: execute it (no wait); its segment is resumed below */
    int resume = r->recording && boot_render_seg.r == r; unsigned seg = boot_render_seg.cur, seg_whole = boot_render_seg.whole;
    uint64_t seg_used = r->ring_used, seg_size = r->ring_size; uint32_t seg_cbv = r->cbv_used, seg_end = boot_render_cbv_end;
    if (r->recording) { ++boot_render_kicks; if (!boot_render_kick(r)) { boot_gpu_wait(r->gpu); return 0; } }
    unsigned a = boot_render_gt.next_alloc; boot_render_gt.next_alloc = (a + 1) % BOOT_RENDER_GT_ALLOCS;
    ID3D12GraphicsCommandList *cl = boot_render_gt.cl;
    if (!boot_render_wait_fence(r->gpu, boot_render_gt.alloc_fence[a]) || FAILED(ID3D12CommandAllocator_Reset(boot_render_gt.alloc[a])) ||
        FAILED(ID3D12GraphicsCommandList_Reset(cl, boot_render_gt.alloc[a], NULL))) { boot_gpu_wait(r->gpu); return 0; }
    boot_dred_reset(cl, r->gpu->queue, r->gpu->device, boot_render_exec_id, 2);
    BOOT_GP_LIST_BEGIN(r->gpu, cl, "write-back list");   /* render-40 */
    unsigned writes = load >= 0 ? ops - 1 : ops;
    D3D12_RESOURCE_BARRIER b[2]; memset(b, 0, sizeof b);
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    uint8_t used[BOOT_RENDER_GT_CHUNKS] = {0};
    for (unsigned j = 0; j < ops; ++j) used[op[j].chunk] = 1;
    /* chunks: COMMON -> COPY_DEST (target images copied out) */
    for (unsigned k = 0; k < BOOT_RENDER_GT_CHUNKS; ++k) if (used[k]) {
        b[0].Transition.pResource = boot_render_gt.chunk[k]; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("write-back gpu", cl, 1, &b[0]); }
    for (unsigned j = 0; j < writes; ++j) {
        BootRenderTarget *x = op[j].x;
        if (x->state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            b[0].Transition.pResource = x->texture; b[0].Transition.StateBefore = x->state; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            BOOT_GP_RB("write-back gpu", cl, 1, &b[0]); }
        D3D12_TEXTURE_COPY_LOCATION dl = {boot_render_gt.chunk[op[j].chunk], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        dl.PlacedFootprint = op[j].fp; dl.PlacedFootprint.Offset = op[j].at + op[j].fp.Offset;
        BOOT_GP_CTR("write-back gpu", cl, &dl, 0, 0, 0, &sl, NULL);
        if (x->state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b[0].Transition.StateAfter = x->state;
            BOOT_GP_RB("write-back gpu", cl, 1, &b[0]); }
    }
    for (unsigned k = 0; k < BOOT_RENDER_GT_CHUNKS; ++k) if (used[k]) {
        b[0].Transition.pResource = boot_render_gt.chunk[k]; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        BOOT_GP_RB("write-back gpu", cl, 1, &b[0]); }
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, boot_render_gt.root);
    D3D12_RESOURCE_BARRIER uav; memset(&uav, 0, sizeof uav); uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = NULL;
    for (unsigned j = 0; j < ops; ++j) {
        BootRenderTarget *x = op[j].x; int is_load = j == writes;
        uint32_t wb = x->width * boot_render_dxgi_bytes(x->dxgi);
        uint32_t c[8] = {wb, x->height, op[j].h, op[j].d, op[j].fp.Footprint.RowPitch, (wb + 63) / 64, 0, 0};
        D3D12_GPU_VIRTUAL_ADDRESS lin = ID3D12Resource_GetGPUVirtualAddress(boot_render_gt.chunk[op[j].chunk]) + op[j].at + op[j].fp.Offset;
        if (j) BOOT_GP_RB("write-back gpu", cl, 1, &uav);   /* in order: later targets win where they overlap */
        ID3D12GraphicsCommandList_SetPipelineState(cl, is_load ? boot_render_gt.detile : boot_render_gt.tile);
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 8, c, 0);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, is_load ? op[j].guest : lin);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 2, is_load ? lin : op[j].guest);
        boot_dred_work(cl, BOOT_DRED_DISPATCH, BOOT_DRED_TILE, NULL, NULL, x->address, boot_render_exec_id, boot_render_watch_event);
        BOOT_GP_DISPATCH(is_load ? "detile (gpu reload)" : "tile (gpu write-back)", 0, cl, ((wb + 3) / 4 + 63) / 64, x->height, 1);
    }
    if (load >= 0) {   /* detiled rows -> the target */
        BootRenderTarget *x = op[writes].x; unsigned k = op[writes].chunk;
        b[0].Transition.pResource = boot_render_gt.chunk[k]; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = x->texture; b[1].Transition.StateBefore = x->state; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("write-back gpu", cl, x->state == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2, b);
        D3D12_TEXTURE_COPY_LOCATION dl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, sl = {boot_render_gt.chunk[k], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
        sl.PlacedFootprint = op[writes].fp; sl.PlacedFootprint.Offset = op[writes].at + op[writes].fp.Offset;
        BOOT_GP_CTR("write-back gpu", cl, &dl, 0, 0, 0, &sl, NULL);
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[1].Transition.StateAfter = x->state;
        if (x->state != D3D12_RESOURCE_STATE_COPY_DEST) BOOT_GP_RB("write-back gpu", cl, 1, &b[1]);
        used[k] = 2;   /* left in COPY_SOURCE */
    }
    for (unsigned k = 0; k < BOOT_RENDER_GT_CHUNKS; ++k) if (used[k]) {
        b[0].Transition.pResource = boot_render_gt.chunk[k]; b[0].Transition.StateBefore = used[k] == 2 ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        BOOT_GP_RB("write-back gpu", cl, 1, &b[0]); }
    BootGpu *g = r->gpu;
    BOOT_GP_LIST_END(cl);   /* render-40 */
    if (FAILED(ID3D12GraphicsCommandList_Close(cl))) { boot_gpu_wait(g); return 0; }
    boot_dred_execute(cl, g->queue, 2);
    ID3D12CommandQueue_ExecuteCommandLists(g->queue, 1, (ID3D12CommandList **)&cl);
    uint64_t fence = ++g->submitted;
    if (FAILED(ID3D12CommandQueue_Signal(g->queue, g->fence, fence))) { boot_gpu_wait(g); return 0; }
    boot_render_gt.alloc_fence[a] = fence;
    BOOT_GP_LIST_EXEC(cl, fence);   /* render-40 */
    for (unsigned j = 0; j < ops; ++j) { boot_render_gt.chunk_fence[op[j].chunk] = fence; boot_render_gt_pend(r, op[j].x->address, op[j].extent, fence); }
    ++boot_render_gt_lists;
    /* bookkeeping as boot_render_rt_writeback_many does after its copies */
    for (unsigned j = 0; j < writes; ++j) {
        BootRenderTarget *x = op[j].x; unsigned i = (unsigned)(x - T->rts); uint64_t extent = op[j].extent;
        T->info[i].dirty = 0;
        boot_render_invalidate(x->address, extent);
        for (unsigned k = 0; k < T->count; ++k) {
            BootRenderTarget *o = &T->rts[k]; BootRenderRtInfo *oi = &T->info[k];
            if (k == i || !o->texture || !oi->known || oi->dirty || boot_render_rt_untiled(o)) continue;
            if (boot_render_rt_overlap_reload() && o->address < x->address + extent && x->address < o->address + boot_render_rt_extent(o, oi)) oi->stale = 1;
        }
        ++boot_render_rt_writebacks; ++boot_render_gt_writebacks;
    }
    if (load >= 0) { T->info[load].stale = 0; ++boot_render_rt_loads; ++boot_render_gt_loads; }
    /* Recording continues in the segment it stopped in (a new list on the same allocator, the rest of
     * its ring space), so no segment change waits for older lists; otherwise in the next segment. */
    if (resume && boot_render_seg.r == r && boot_render_seg.cur == seg && SUCCEEDED(ID3D12GraphicsCommandList_Reset(r->cl, boot_render_seg.alloc[seg], NULL))) {
        boot_dred_reset(r->cl, r->gpu->queue, r->gpu->device, boot_render_exec_id, 3);
        boot_light84_reset(r, r->cl, boot_render_seg.alloc[seg], seg);
        boot_light86_reset(r, r->cl, boot_render_seg.alloc[seg], seg);
        ID3D12DescriptorHeap *heaps[2] = {r->srv_heap, r->sampler_heap};
        ID3D12GraphicsCommandList_SetDescriptorHeaps(r->cl, 2, heaps);
        boot_render_pred_list_begin();   /* render-38c */
        BOOT_GP_LIST_BEGIN(r->gpu, r->cl, "render list (resumed)");   /* render-40 */
        r->ring_used = seg_used; r->ring_size = seg_size; r->cbv_used = seg_cbv; boot_render_cbv_end = seg_end; boot_render_seg.whole = seg_whole;
        r->recording = 1; ++boot_render_gt_resumed; ++boot_render_bind_epoch;   /* a new list: no state */
        return 1;
    }
    if (!boot_render_begin(r)) return 0;
    return 1;
}
/* Target i (guest VA [address, +extent), block height/depth log_h/log_d) takes guest memory's image,
 * after every dirty target over that memory is written back (oldest first, i included): on the GPU
 * when possible, else with the CPU write-back and load. */
static uint64_t boot_render_rt_kept_image;   /* render-22: refreshes that kept a current target's image */
/* render-23 diagnostic: the last 256 refreshes of any target (reload from guest memory, or render-22's kept image),
 * read by the present-time flicker detector (host_gpu_draw_exec.h, TOTK_FLICKER_LOG=1). */
static struct { uint64_t exec, address; uint32_t width, height, format, overlaps; uint8_t stale, dirty, kept; } boot_render_reload_ring[256];
static uint64_t boot_render_reload_n;
/* render-27 diagnostic: the last 256 textures assembled from targets as a NEW texture (not refreshed in place), with
 * why the in-place path was not taken and the first four source targets (address, size, write sequence), for the
 * flicker detector (frames\flicker.log) */
static struct { uint64_t exec, address, seq, src[4]; uint32_t index, width, height, layers, levels, why, n, srcw[4], srch[4]; uint64_t srcseq[4]; } boot_render_build_ring[256];
static uint64_t boot_render_build_n;
static const char *const boot_render_build_why[] = {"in-place refused", "slot not valid", "no resource", "dimension", "not assembled before",
                                                     "other address", "TIC words changed", "resource shape/format or shared", "slot image is a target"};
static inline int boot_render_rt_refresh(BootRender *r, unsigned i, uint64_t extent, unsigned log_h, unsigned log_d) {
    BootRenderRtTable *T = boot_render_rtab(r); BootRenderTarget *x = &T->rts[i];
    unsigned list[BOOT_RENDER_RT_CAP], n = boot_render_rt_dirty_over(T, x->address, extent, list);
    if (boot_image_trace_overlap(x->address, extent)) {
        boot_render_image_source(r, i, BOOT_IMAGE_REFRESH_DEST, x->address, extent, n);
        for (unsigned q = 0; q < n; ++q)
            boot_render_image_source(r, list[q], BOOT_IMAGE_REFRESH_SOURCE, x->address, extent, ((uint64_t)i << 32) | q);
    }
    {   unsigned k = (unsigned)(boot_render_reload_n++ & 255);   /* render-23 */
        boot_render_reload_ring[k].exec = boot_render_exec_id; boot_render_reload_ring[k].address = x->address;
        boot_render_reload_ring[k].width = x->width; boot_render_reload_ring[k].height = x->height; boot_render_reload_ring[k].format = x->format;
        boot_render_reload_ring[k].overlaps = n; boot_render_reload_ring[k].stale = T->info[i].stale; boot_render_reload_ring[k].dirty = T->info[i].dirty;
        boot_render_reload_ring[k].kept = 0; }
    /* render-20 diagnostic: rt-watch (frames\rt-watch.request) also logs every reload of the watched target from
     * guest memory, why (stale / dirty overlap), and the dirty targets written back first, oldest first (first 64) */
    {   static unsigned logged;
        if (boot_render_watch_va && x->address == boot_render_watch_va && logged < 64) {
            FILE *f = fopen("draw-path.log", "a");
            if (f) {
                ++logged;
                fprintf(f, "rt-watch %llx: reload #%u %ux%u fmt %x from guest memory (stale %u dirty %u, submission %llu, event %u); written back first:",
                        (unsigned long long)x->address, i, x->width, x->height, x->format, T->info[i].stale, T->info[i].dirty,
                        (unsigned long long)boot_render_exec_id, boot_render_watch_event);
                for (unsigned q = 0; q < n; ++q)
                    fprintf(f, " [#%u %llx %ux%u fmt %x use %llu]", list[q], (unsigned long long)T->rts[list[q]].address, T->rts[list[q]].width,
                            T->rts[list[q]].height, T->rts[list[q]].format, (unsigned long long)T->info[list[q]].last_use);
                fputc('\n', f); fclose(f);
            }
        }
    }
    /* render-22: target i is current (not stale: nothing changed guest memory under it outside the 3D engine) and
     * clean, so guest memory already holds its image except where dirty targets over the same memory (other
     * shapes at aliased addresses) will now be written. Those go to guest memory and i keeps its host image
     * (it is not reloaded): if i was drawn after all of them, its image is written back over theirs (it is the
     * newest there); otherwise their bytes stay in guest memory's overlap only. Reloading i instead replaced its
     * whole image with guest memory around a small overlap: the title's local-light array layer (50f3a0000,
     * 512x288, block-linear extent to 50f460000) shares its last 64 KiB with a 320x180 R32F target (50f450000) the
     * game draws about once a second, and each of those draws reloaded the layer (cap-flicker: every 60
     * submissions, "stale 0 dirty 0", written back first #301 50f450000) and the mural's lights flickered.
     * A stale or dirty i, or no GPU write-back path, takes the reload below as before. */
    {   BootRenderRtInfo *ii = &T->info[i];
        if (n && n < 16 && ii->known && !ii->stale && !ii->dirty) {
            int newest = boot_render_rt_written_at[i] != 0;
            for (unsigned q = 0; q < n; ++q) if (boot_render_rt_written_at[list[q]] > boot_render_rt_written_at[i]) newest = 0;
            unsigned m = n;
            if (newest) { list[m++] = i; ii->dirty = 1; }   /* written back last (gt_run writes dirty targets only) */
            if (boot_render_gt_run(r, list, m, -1, 0, 0)) { ii->stale = 0; ++boot_render_rt_kept_image; boot_render_reload_ring[(boot_render_reload_n - 1) & 255].kept = 1; return 1; }
            ii->dirty = 0;
        }
    }
    if (n || boot_render_gt_overlaps(r, x->address, extent)) {
        if (n <= 16 && boot_render_gt_run(r, list, n, (int)i, log_h, log_d)) return 1;
        ++boot_render_gt_fallbacks;
    }
    return boot_render_flush_targets(r, x->address, extent) && boot_render_rt_load(r, x, log_h, log_d);
}

/* Writes pitch-linear render targets drawn since the last flush back to guest memory (completes
 * all recorded GPU work first). Returns 0 on failure with r->error set. */
static inline void boot_render_invalidate(uint64_t va, uint64_t size);
static inline int boot_render_flush_pitch(BootRender *r) {
    BootRenderRtTable *T = boot_render_rtab(r);
    int any = 0;
    for (unsigned i = 0; i < T->count; ++i) any |= T->pitch_dirty[i];
    if (!any) return 1;
    /* every dirty target's copy is recorded after the recorded work; one submission per batch */
    for (unsigned next = 0; next < T->count; ) {
        struct { unsigned i; const uint8_t *src; UINT pitch; } job[16]; unsigned jobs = 0;
        boot_render_staging_reset(&boot_render_rb_ring);
        for (; next < T->count && jobs < 16; ++next) {
            if (!T->pitch_dirty[next]) continue;
            BootRenderTarget *x = &T->rts[next];
            D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
            ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
            if (!boot_render_begin(r)) { boot_render_fail(r, 0, "command list reset"); return 0; }
            ID3D12Resource *rb = NULL; uint64_t base = 0;
            uint8_t *mapped = boot_render_staging_alloc(r, &boot_render_rb_ring, D3D12_HEAP_TYPE_READBACK, total, &rb, &base);
            if (!mapped) { if (jobs) break; T->pitch_dirty[next] = 0; boot_render_fail(r, 0, "pitch target readback"); return 0; }
            D3D12_RESOURCE_STATES was = x->state;
            boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dl = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
            dl.PlacedFootprint = fp; dl.PlacedFootprint.Offset += base;
            BOOT_GP_CTR("pitch write-back", r->cl, &dl, 0, 0, 0, &sl, NULL);
            boot_render_rt_barrier(r, x, was);
            job[jobs].i = next; job[jobs].src = mapped + fp.Offset; job[jobs].pitch = fp.Footprint.RowPitch; ++jobs;
        }
        if (!jobs) continue;
        if (!boot_render_submit(r)) { boot_render_fail(r, 0, "pitch target readback"); return 0; }
        for (unsigned j = 0; j < jobs; ++j) {
            unsigned i = job[j].i; T->pitch_dirty[i] = 0;
            BootRenderTarget *x = &T->rts[i]; uint32_t pitch = T->pitch_pitch[i], bpp = boot_render_dxgi_bytes(x->dxgi);
            for (uint32_t y = 0; y < x->height; ++y) {
                uint8_t *dst = boot_render_guest(r, x->address + (uint64_t)y * pitch, (uint64_t)x->width * bpp);
                if (!dst) { snprintf(r->error, sizeof r->error, "pitch target %llx write-back outside guest memory", (unsigned long long)x->address); return 0; }
                memcpy(dst, job[j].src + (size_t)y * job[j].pitch, (size_t)x->width * bpp);
            }
            boot_render_invalidate(x->address, (uint64_t)pitch * x->height);
            ++boot_render_pitch_flushes;
        }
    }
    return 1;
}

/* Copies the guest constant buffer [va, va+size) into the ring and creates a CBV for it. */
/* Copies guest GPU VA [va, va+size) page by page: the range may span pages packed into different
 * host blocks (GPU arenas place regions independently of guest addresses). */
static inline int boot_render_guest_copy(BootRender *r, uint64_t va, uint8_t *dst, uint64_t size) {
    while (size) {
        uint64_t chunk = 4096 - (va & 0xfff); if (chunk > size) chunk = size;
        uint8_t *src = boot_render_guest(r, va, chunk); if (!src) return 0;
        memcpy(dst, src, (size_t)chunk); va += chunk; dst += chunk; size -= chunk;
    }
    return 1;
}
/* Snapshot sharing. Within one scope (one recorded list, one page table) a draw binding the same
 * range (va, bytes) as an earlier snapshot reuses that snapshot when no CPU write has touched the
 * range since it was taken: its bytes are then exactly what a new copy would hold. Every write the
 * host makes to guest memory while a list is open is reported through boot_render_cb_written (staged
 * stores, copy-engine and compute results, write-backs via boot_render_invalidate); GPU writes reach
 * guest memory only after a waiting submission, which ends the scope. Write tracking is per 4 KiB
 * guest GPU page in a direct-mapped table (a collision only causes a needless copy). */
#define BOOT_RENDER_CB_CACHE 2048
typedef struct BootRenderCbEntry { uint64_t va, scope, epoch, offset; uint32_t bytes, aligned; } BootRenderCbEntry;
static BootRenderCbEntry boot_render_cb_cache[BOOT_RENDER_CB_CACHE];
static uint64_t boot_render_cb_hits, boot_render_cb_misses, boot_render_cb_copied, boot_render_cb_zeroed;
/* A word of one draw's constant-buffer snapshot replaced (feedback views, host_gpu_render_draw.h). */
typedef struct BootRenderCbPatch { uint32_t group, offset, value; } BootRenderCbPatch;
/* play (39): while a draw resolves a stage table, CBV writes into [cap_base, cap_base + 19) are captured
 * (boot_render_draw then writes the table only when it differs from the previous draw's). */
static D3D12_CONSTANT_BUFFER_VIEW_DESC boot_render_cbv_cap[19]; static int boot_render_cbv_capturing; static uint32_t boot_render_cbv_cap_base;
static uint64_t boot_render_tables_reused, boot_render_tables_written;
static inline void boot_render_cbv_put(BootRender *r, const D3D12_CONSTANT_BUFFER_VIEW_DESC *cv, uint32_t descriptor) {
    if (boot_render_cbv_capturing && descriptor - boot_render_cbv_cap_base < 19) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC *d = &boot_render_cbv_cap[descriptor - boot_render_cbv_cap_base];
        memset(d, 0, sizeof *d); d->BufferLocation = cv->BufferLocation; d->SizeInBytes = cv->SizeInBytes; return;
    }
    ID3D12Device_CreateConstantBufferView(r->gpu->device, cv, boot_render_cpu(r, r->srv_heap, r->srv_inc, descriptor));
}
static inline int boot_render_cbv_ex(BootRender *r, uint64_t va, uint32_t size, uint32_t descriptor, const BootRenderCbPatch *patch, unsigned patches, uint32_t group);
static inline int boot_render_cbv(BootRender *r, uint64_t va, uint32_t size, uint32_t descriptor) { return boot_render_cbv_ex(r, va, size, descriptor, NULL, 0, 0); }
/* async-xlate: the bytes boot_render_cbv_ex copies for a uniform-buffer binding of `bound` bytes (host_nvn_exec.h boot_nvn_st_buffers rounds
 * the bound size up to 256, cbv_ex then copies min(size, its 256-byte aligned view, 64 KiB)). The capture copies exactly this many, so a
 * binding it holds is never short (stage 1 copied the bound size: every binding that was not a multiple of 256 bytes missed). */
static inline uint32_t boot_render_cb_view_bytes(uint64_t bound) {
    const uint32_t size = (uint32_t)((bound + 255) & ~UINT64_C(255));
    uint64_t aligned = ((uint64_t)(size ? size : 256) + 255) & ~UINT64_C(255); if (aligned > 65536) aligned = 65536;
    return size ? (size < aligned ? size : (uint32_t)aligned) : 0;
}
/* async-xlate (on X only, boot_ax_cap_view set): the source of a constant-buffer copy. taken: data-snapshot takes the binding (boot_ds_cb_now):
 * the capture's bytes when it holds the range and nothing moved since (page table, big writes, host writes at a later epoch); else guest
 * memory, counted by why (no entry, short, unmapped or over budget at the capture, stale; GPU-ordered at the capture and program constants
 * by design). A binding data-snapshot does not take reads guest memory as before: GPU order decides for it. live may be NULL (the page
 * path: the captured bytes or NULL). */
static inline const uint8_t *boot_ax_cb_src(uint64_t va, uint64_t n, const uint8_t *live, int taken) {
    const BootAxCap *c = boot_ax_cap_view; unsigned k = BOOT_AX_CB_NOT_TAKEN; const uint8_t *p = live;
    if (taken) {
        const uint32_t j = boot_ax_cap_at(c, va); const unsigned f = j != UINT32_MAX ? c->flag[j] : 0u;
        if (j == UINT32_MAX) k = boot_ds_code_range(va, n) ? BOOT_AX_CB_CODE : BOOT_AX_CB_MISS;
        else if (f & BOOT_AX_CAPF_GPU) k = BOOT_AX_CB_GPU;
        else if (!c->len[j]) k = (f & BOOT_AX_CAPF_OVER) ? BOOT_AX_CB_OVER : BOOT_AX_CB_UNMAPPED;
        else if (n > c->len[j]) k = BOOT_AX_CB_SHORT;
        else if (c->map_gen != boot_render_map_gen || c->big != boot_render_big_writes || !boot_render_cb_clean(va, n, c->epoch + 1)) k = BOOT_AX_CB_STALE;
        else { k = BOOT_AX_CB_HIT; p = c->arena + c->off[j]; }
    }
    ++boot_ax_cb_n[k]; boot_ax_cb_bytes[k] += n;
    return p;
}
/* As boot_render_cbv; patches of bind group `group` are applied to a private snapshot (never cached). */
static inline int boot_render_cbv_ex(BootRender *r, uint64_t va, uint32_t size, uint32_t descriptor, const BootRenderCbPatch *patch, unsigned patches, uint32_t group) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv = {0};
    uint64_t aligned = ((uint64_t)(size ? size : 256) + 255) & ~UINT64_C(255);
    if (aligned > 65536) aligned = 65536;
    size_t n = size ? (size < aligned ? size : (size_t)aligned) : 0;
    if (n && va > UINT64_MAX - (uint64_t)n) return 0;   /*99: reject wrapping binding before cache/guest/copy*/
    if (!n) va = 0;   /* an unbound slot reads zeros wherever it points */
    unsigned mine = 0; for (unsigned i = 0; i < patches; ++i) mine += patch[i].group == group && patch[i].offset + 4 <= n;
    BootRenderCbEntry *ce = NULL;
    if (!r->overlay_count && !mine) {
        ce = &boot_render_cb_cache[(unsigned)((va >> 8) ^ (va >> 21) ^ (n * 0x9e37u)) & (BOOT_RENDER_CB_CACHE - 1)];
        if (ce->scope == boot_render_cb_scope && ce->va == va && ce->bytes == n && ce->aligned == aligned && boot_render_cb_clean(va, n, ce->epoch)) {
            ++boot_render_cb_hits;
            cv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->ring) + ce->offset; cv.SizeInBytes = (UINT)aligned;
            if(boot_cbv_tap_active()) {
                const uint8_t *owned=boot_render_cb_tap_span(r,ce->offset,aligned,0) ? r->ring_cpu+ce->offset : NULL;
                boot_cbv_tap_resolved(cv.BufferLocation,cv.SizeInBytes,r->ring,ce->offset,r->ring_size,
                    BOOT_CBV_TAP_RING_CACHE,UINT32_MAX,boot_render_cb_scope,boot_render_list_id,boot_render_map_gen,0,owned,(size_t)aligned);
            }
            boot_render_cbv_put(r, &cv, descriptor);
            return 1;
        }
    }
    if (r->ring_used + aligned > r->ring_size) return 0;
    uint8_t *dst = r->ring_cpu + r->ring_used;
    uint32_t cbv_tap_flags=mine?BOOT_CBV_TAP_FLAG_PATCHED:0;
    if (n) {
        uint8_t *src = boot_render_guest(r, va, n);
        if (src) {
            const uint8_t *fsrc = boot_ax_cap_view ? boot_ax_cb_src(va, n, src, boot_ds_cb_now) : src;   /* async-xlate: on X, the bytes t2 captured at the submit call */
            if (boot_ds_cb_now || boot_ds_fill_wanted(va, n)) boot_ds_fill(r, dst, va, fsrc, n);   /* data-snapshot: the bytes at the submit call where it has them */
            else memcpy(dst, src, n);
            if (boot_ax_chk_log) boot_ax_chk_cb(va, n, dst, fsrc != src ? 1u : boot_ds_cb_now ? 2u : 3u);   /* async-xlate sentinel: what the copy put in the ring */
            boot_render_cb_copied += n; if (boot_sd_on()) boot_sd_note(r, boot_ds_cb_now ? BOOT_SD_K_CBSNAP : BOOT_SD_K_CB, va, src, dst, n);   /* suspect-diag S1 */
        }
        else {
            /* Games bind constant buffers larger than the memory behind them (up to 64 KiB) and read
             * only the used part: copy the mapped pages, zero the rest. The first page must map. */
            if (!boot_render_guest(r, va, 1)) return 0;
            const uint8_t *cap = boot_ax_cap_view ? boot_ax_cb_src(va, n, NULL, boot_ds_cb_now) : NULL;   /* async-xlate: the captured bytes of the mapped pages */
            for (uint64_t at = 0; at < n;) {
                uint64_t chunk = 4096 - ((va + at) & 0xfff); if (chunk > n - at) chunk = n - at;
                uint8_t *page = boot_render_guest(r, va + at, chunk);
                if (page) { memcpy(dst + at, cap ? cap + at : page, (size_t)chunk); boot_render_cb_copied += chunk; }
                else { memset(dst + at, 0, (size_t)chunk); boot_render_cb_zeroed += chunk; }
                at += chunk;
            }
        }
        for (uint32_t i = 0; i < r->overlay_count; ++i) {   /* writes staged earlier in this submission */
            const BootRenderWrite *w = &r->overlay[i];
            if (w->kind == 0 && n >= 4 && w->gpu >= va && w->gpu - va <= (uint64_t)n - 4) {
                memcpy(dst + (w->gpu - va), &w->value, 4); cbv_tap_flags|=BOOT_CBV_TAP_FLAG_OVERLAY;
            }
        }
    }
    if (aligned > n) { memset(dst + n, 0, (size_t)aligned - n); boot_render_cb_zeroed += aligned - n; }
    for (unsigned i = 0; mine && i < patches; ++i) if (patch[i].group == group && patch[i].offset + 4 <= n) memcpy(dst + patch[i].offset, &patch[i].value, 4);
    ++boot_render_cb_misses;
    if (ce) { ce->scope = boot_render_cb_scope; ce->va = va; ce->bytes = (uint32_t)n; ce->aligned = (uint32_t)aligned; ce->offset = r->ring_used; ce->epoch = ++boot_render_cb_epoch; }
    cv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used; cv.SizeInBytes = (UINT)aligned;
    if(boot_cbv_tap_active()) boot_cbv_tap_resolved(cv.BufferLocation,cv.SizeInBytes,r->ring,r->ring_used,r->ring_size,
        mine?BOOT_CBV_TAP_RING_PATCHED:n?BOOT_CBV_TAP_RING_FRESH:BOOT_CBV_TAP_RING_ZERO,UINT32_MAX,
        boot_render_cb_scope,boot_render_list_id,boot_render_map_gen,cbv_tap_flags,boot_render_cb_tap_span(r,r->ring_used,aligned,1)?dst:NULL,(size_t)aligned);
    r->ring_used += aligned;
    boot_render_cbv_put(r, &cv, descriptor);
    return 1;
}

/* ---------------------------------------------------------------- textures and samplers */
#include "host_texture_layout.h"
#include "host_astc.h"
/* ---- play (50): parallel ASTC decode into a pitched destination (the upload ring) ---- */
static uint64_t boot_tex_t_detile, boot_tex_t_decode, boot_tex_t_copy, boot_tex_t_create, boot_tex_decoded_bytes, boot_tex_built;
static inline uint64_t boot_tex_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
#define BOOT_ASTC_BAND_ROWS 4u   /* block rows per band */
typedef struct {
    volatile LONG gen;                                   /* 0 while the parameters are being written */
    const uint8_t *blocks; uint32_t width, height, bw, bh, bx, by, bands; uint8_t *dst; size_t pitch;
    volatile LONG done, hard, hdr, errors;
    volatile LONG64 next;                                /* gen << 32 | next band */
} BootAstcJob;
static BootAstcJob boot_astc_job;
static struct { HANDLE sem; unsigned n; volatile LONG started; } boot_astc_pool;
/* Decodes block rows [j0, j1) of the job's image. */
static inline void boot_astc_rows(const BootAstcJob *j, uint32_t j0, uint32_t j1, LONG *hard, LONG *hdr, LONG *errors) {
    uint8_t tmp[12 * 12 * 4];
    for (uint32_t bj = j0; bj < j1; ++bj)
        for (uint32_t bi = 0; bi < j->bx; ++bi) {
            int r = boot_astc_decode_block_ex(j->blocks + ((size_t)bj * j->bx + bi) * 16, j->bw, j->bh, 0, tmp, (size_t)j->bw * 4);
            if (r == BOOT_ASTC_E_HDR) *hdr = 1;
            else if (r == BOOT_ASTC_ERROR_BLOCK) ++*errors;
            else if (r != BOOT_ASTC_OK) { if (!*hard) *hard = r; continue; }
            uint32_t x0 = bi * j->bw, y0 = bj * j->bh;
            uint32_t cw = j->width - x0 < j->bw ? j->width - x0 : j->bw, ch = j->height - y0 < j->bh ? j->height - y0 : j->bh;
            for (uint32_t y = 0; y < ch; ++y) memcpy(j->dst + (size_t)(y0 + y) * j->pitch + (size_t)x0 * 4, tmp + (size_t)y * j->bw * 4, (size_t)cw * 4);
        }
}
/* Claims and decodes bands of the current job; returns when none is left. */
static inline void boot_astc_work(void) {
    BootAstcJob *j = &boot_astc_job;
    for (;;) {
        LONG64 v = InterlockedIncrement64(&j->next) - 1; LONG g = (LONG)(v >> 32); uint32_t band = (uint32_t)v;
        if (!g || j->gen != g) return;
        MemoryBarrier();
        uint32_t bands = j->bands, by = j->by;
        MemoryBarrier();
        if (j->gen != g || band >= bands) return;
        LONG hard = 0, hdr = 0, errors = 0;
        uint32_t j0 = band * BOOT_ASTC_BAND_ROWS, j1 = j0 + BOOT_ASTC_BAND_ROWS < by ? j0 + BOOT_ASTC_BAND_ROWS : by;
        boot_astc_rows(j, j0, j1, &hard, &hdr, &errors);
        if (hard) InterlockedCompareExchange(&j->hard, hard, 0);
        if (hdr) InterlockedExchange(&j->hdr, 1);
        if (errors) InterlockedExchangeAdd(&j->errors, errors);
        InterlockedIncrement(&j->done);
    }
}
static DWORD WINAPI boot_astc_worker(void *arg) {
    (void)arg;
    for (;;) { WaitForSingleObject(boot_astc_pool.sem, INFINITE); boot_astc_work(); }
}
static inline void boot_astc_pool_start(void) {
    if (InterlockedExchange(&boot_astc_pool.started, 1)) return;
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    unsigned n = cpus > 2 ? cpus - 2 : 0; if (n > 8) n = 8;
    if (!n || !(boot_astc_pool.sem = CreateSemaphoreA(NULL, 0, 1 << 20, NULL))) return;
    for (unsigned i = 0; i < n; ++i) { HANDLE t = CreateThread(NULL, 0, boot_astc_worker, NULL, 0, NULL); if (t) { CloseHandle(t); ++boot_astc_pool.n; } }
}
/* ASTC image (linear block order) -> RGBA8 rows of `pitch` bytes at dst. Same result as
 * boot_astc_decode_image: <0 on a hard error or BOOT_ASTC_E_HDR, else the count of illegal blocks. */
static inline int boot_astc_decode_pitched(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, uint8_t *dst, size_t pitch) {
    if (!blocks || !dst || !width || !height || !boot_astc_footprint_ok(bw, bh) || pitch < (size_t)width * 4) return BOOT_ASTC_E_ARGS;
    BootAstcJob *j = &boot_astc_job; static LONG gen;
    uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    boot_astc_pool_start();
    if ((uint64_t)bx * by < 256 || !boot_astc_pool.n) {   /* small: on the caller */
        BootAstcJob local = {0}; local.blocks = blocks; local.width = width; local.height = height; local.bw = bw; local.bh = bh;
        local.bx = bx; local.by = by; local.dst = dst; local.pitch = pitch;
        LONG hard = 0, hdr = 0, errors = 0; boot_astc_rows(&local, 0, by, &hard, &hdr, &errors);
        return hard ? hard : hdr ? BOOT_ASTC_E_HDR : errors;
    }
    if (++gen <= 0) gen = 1;
    j->gen = 0; MemoryBarrier();
    j->blocks = blocks; j->width = width; j->height = height; j->bw = bw; j->bh = bh; j->bx = bx; j->by = by;
    j->bands = (by + BOOT_ASTC_BAND_ROWS - 1) / BOOT_ASTC_BAND_ROWS; j->dst = dst; j->pitch = pitch;
    j->done = 0; j->hard = 0; j->hdr = 0; j->errors = 0;
    MemoryBarrier(); j->gen = gen;
    InterlockedExchange64(&j->next, (LONG64)gen << 32);
    unsigned wake = j->bands - 1 < boot_astc_pool.n ? j->bands - 1 : boot_astc_pool.n;
    if (wake) ReleaseSemaphore(boot_astc_pool.sem, (LONG)wake, NULL);
    boot_astc_work();
    for (unsigned spin = 0; j->done < (LONG)j->bands; ++spin) { if (spin < 2000) YieldProcessor(); else SwitchToThread(); }
    InterlockedExchange64(&j->next, 0);   /* late workers find no job */
    return j->hard ? j->hard : j->hdr ? BOOT_ASTC_E_HDR : j->errors;
}
typedef struct BootRenderTexSlot {
    uint32_t words[8]; ID3D12Resource *res; uint32_t valid, dim; uint64_t va, size;
    ID3D12Resource *view_of; uint64_t view_offset;   /* zero-copy buffer views: the guest region resource and byte offset viewed */
} BootRenderTexSlot;
typedef struct BootRenderTsSlot { uint32_t words[8]; uint32_t valid; } BootRenderTsSlot;
/* The slot table is VirtualAlloc'd and registered process-wide (environment variable
 * TOTK_RENDER_TEX_TABLE): a rebuilt runtime DLL resumed in the live service releases the previous
 * module's textures (boot_render_tex_retire_previous) instead of leaking them with its statics. */
typedef struct BootRenderTexTable { char magic[8]; uint32_t entry_size, count; BootRenderTexSlot slots[BOOT_RENDER_TEX_SLOTS]; } BootRenderTexTable;
static BootRenderTexSlot *boot_render_tex_slots;
/* One past the highest slot this module made valid: boot_render_invalidate scans [0, high). */
static uint32_t boot_render_tex_high;
/* gpu-resident: a released guest view (boot_gr_slot_release, queue idle) must not stay behind a zero-copy buffer texture: its
 * revalidation compares the region resource pointer (boot_render_texture), which a new view could get again. Such slots are
 * rebuilt at their next use; their descriptor reads zero meanwhile. */
static inline void boot_gr_tex_forget(BootRender *r, ID3D12Resource *res) {
    if (!boot_render_tex_slots || !res) return;
    for (uint32_t i = 0; i < boot_render_tex_high && i < BOOT_RENDER_TEX_SLOTS; ++i) {
        BootRenderTexSlot *s = &boot_render_tex_slots[i];
        if (s->view_of != res) continue;
        s->valid = 0; s->view_of = NULL; s->size = 0; boot_render_texmemo[i] = 0;
        boot_render_null_srv(r, BOOT_DIM_BUFFER, i);
        ++boot_gr_st.tex_views_dropped;
    }
}
/* render-39: a slot is marked for a rebuild ("poisoned") by flipping a bit of its stored TIC word 0, so the
 * next lookup mismatches. Flipping twice before the rebuild restored the word and the stale texture was
 * reused (e.g. a CPU texel drain then the verifier in one submission, or two verifier passes over an unused
 * slot). boot_render_tex_poison flips once; every build path ends in boot_render_tex_mark, which clears it. */
static uint8_t boot_render_tex_poisoned[BOOT_RENDER_TEX_SLOTS];
/* batch-b (depth feedback on the GPU, boot_render_texture_from_target; TOTK_DEPTH_FEEDBACK_GPU=0: off): the depth
 * target the current draw tests and writes (set around its texture loop, else NULL), the slots built from it during
 * that loop (invalidated once the loop ends: boot_render_dfb_end), and per slot whether its texture and descriptors are
 * such a copy (cleared by boot_render_tex_mark, i.e. by every other build). */
static ID3D12Resource *boot_render_dfb_zt;
static uint8_t boot_render_dfb_slot[BOOT_RENDER_TEX_SLOTS];
static uint32_t boot_render_dfb_list[32]; static unsigned boot_render_dfb_n, boot_render_dfb_over;
static uint64_t boot_render_dfb_built, boot_render_dfb_refreshes, boot_render_dfb_alias_skips;
static inline int boot_render_dfb_on(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_DEPTH_FEEDBACK_GPU", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
static uint64_t boot_render_dfb_kept;   /* drain2-work: target views not taken, the slot's depth copy refreshed in place instead */
static inline int boot_render_dfb_keep_on(void) {   /* TOTK_DFB_KEEP_COPY (default on, with TOTK_SLOT_VERSIONING or TOTK_TEX_KEEP: blacktex-work) */
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_DFB_KEEP_COPY", e, sizeof e); v = (boot_render_sv_on() || boot_render_tex_keep_on()) && !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
/* render-39: CPU-written textures checked at use (boot_render_tex_watch_check below); entry + 1 per slot */
static uint8_t boot_render_tex_watch_of[BOOT_RENDER_TEX_SLOTS];
/* wb-02: slots assembled from target subresources (boot_render_texture_subres): checked at use like the assemblies
 * of render-14 (boot_render_asm_stale); every build path ends in boot_render_tex_mark, which clears it */
static uint8_t boot_render_tex_subres[BOOT_RENDER_TEX_SLOTS];
static inline void boot_render_tex_watch_forget(uint32_t index);
/* t2-B: indices of the slots that may be valid. Every path that makes a slot valid ends in boot_render_tex_mark,
 * which adds the index; the range scans (boot_render_invalidate_textures, boot_render_textures_overlap) walk this
 * list instead of every index below boot_render_tex_high (the TIC ring makes that ~65536 slots, 5 MiB per scan:
 * play-81 profile, 11% of the render thread in those two loops, under compute stores in boot_compute_idle and the
 * deferral check of every dispatch). A listed slot found invalid there leaves the list (a later build adds it
 * again), so the list always holds every valid slot and the scans find exactly what the full loops found. */
static uint32_t boot_render_tex_live[BOOT_RENDER_TEX_SLOTS], boot_render_tex_live_n;
static uint8_t boot_render_tex_live_in[BOOT_RENDER_TEX_SLOTS];
/* mem-work (texture census, TOTK_TEX_CENSUS): non-zero while the slot's resource holds ASTC decoded to RGBA8 (set by the
 * guest build after boot_render_tex_mark, which clears it for every other build path): bit 0 ASTC, bits 1-4 the memory
 * components the TIC swizzle reads (R, G, B, A), bit 5 sRGB view, bit 6 blocks larger than 4x4. Diagnostic only. */
static uint8_t boot_render_tex_astc[BOOT_RENDER_TEX_SLOTS];
static uint64_t boot_render_tex_marks;   /* mem-work: builds so far (every path that makes a slot valid ends in tex_mark) */
/* bc7-work (host_bc7.h): build serial per slot (every build path ends in boot_render_tex_mark), checked by queued encodes;
 * TIC format of the last guest build (0x80 | components, 0 unknown), for the reinterpretation check; whether the slot
 * holds a BC7 texture (set by host_bc7.h, 0 after every build) for the vertex-stage rule */
static uint32_t boot_render_tex_serial[BOOT_RENDER_TEX_SLOTS];
static uint8_t boot_render_tex_tic_fmt[BOOT_RENDER_TEX_SLOTS], boot_render_tex_bc7[BOOT_RENDER_TEX_SLOTS];
static inline void boot_render_tex_mark(uint32_t index) {
    ++boot_render_tex_serial[index]; boot_render_tex_tic_fmt[index] = 0; boot_render_tex_bc7[index] = 0;   /* bc7-work */
    boot_render_texmemo[index] = 0;   /* nvnpar-work: rebuilt */
    if (index >= boot_render_tex_high) boot_render_tex_high = index + 1;
    if (!boot_render_tex_live_in[index]) { boot_render_tex_live_in[index] = 1; boot_render_tex_live[boot_render_tex_live_n++] = index; }   /* t2-B */
    boot_render_tex_poisoned[index] = 0;
    boot_render_tex_subres[index] = 0;   /* wb-02 */
    boot_render_dfb_slot[index] = 0;     /* batch-b: set again by boot_render_dfb_note after a depth-feedback copy */
    boot_render_tex_astc[index] = 0; ++boot_render_tex_marks;   /* mem-work: census flag (set again by an ASTC guest build), build count */
    if (boot_render_tex_watch_of[index]) boot_render_tex_watch_forget(index);   /* a guest build registers it again */
}
static inline void boot_render_tex_poison(uint32_t index) {
    boot_render_texmemo[index] = 0;   /* nvnpar-work */
    if (boot_render_tex_poisoned[index]) return;
    boot_render_tex_poisoned[index] = 1; boot_render_tex_slots[index].words[0] ^= 0x80000000u;
}
/* TIC indices whose 2D descriptor views a feedback copy of a bound target (host_gpu_render_draw.h,
 * boot_render_fb_bind): recorded draws may still read it, so it is rewritten only after they ran. */
static uint8_t boot_render_fb_on[BOOT_RENDER_TEX_SLOTS];
/* Guest CPU writes to texture memory. A slot's texture is built from guest memory once and reused
 * while its TIC words are unchanged; only GPU-side writes (copy engine, compute, target write-backs)
 * invalidate it. The game also fills textures from the CPU after their first use (the font glyph
 * cache: menu text was missing; streamed images), which the hardware sees at once. Each built slot
 * keeps a checksum of the guest bytes it was made from; at the end of every guest submission
 * boot_render_tex_verify re-hashes slots round-robin within a byte budget (TOTK_TEX_VERIFY_MB,
 * default 64; 0 = off) and marks changed ones for a rebuild on their next use. */
static uint64_t boot_render_tex_sum[BOOT_RENDER_TEX_SLOTS];   /* checksum of [slot.va, slot.va + slot.size) at build */
static uint32_t boot_render_tex_verify_cursor;
static uint64_t boot_render_tex_verified_bytes, boot_render_tex_stale_rebuilds;
static inline uint64_t boot_render_tex_checksum(const uint8_t *p, uint64_t n) {
    /* Fletcher-style over 64-bit words in four lanes: position-sensitive, ~memory speed */
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0, i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4]; memcpy(w, p + i, 32);
        a0 += w[0]; b0 += a0; a1 += w[1]; b1 += a1; a2 += w[2]; b2 += a2; a3 += w[3]; b3 += a3;
    }
    uint64_t t = 0; for (; i < n; ++i) t = t * 131 + p[i];
    return (a0 ^ (b0 * 3)) + ((a1 ^ (b1 * 5)) << 1) + ((a2 ^ (b2 * 7)) << 2) + ((a3 ^ (b3 * 11)) << 3) + t + n;
}
/* Texture dedup index: TIC entry (and table) hash -> a slot that holds that image (index + 1). */
#define BOOT_RENDER_TEX_DEDUP (1u << 16)
static uint32_t boot_render_tex_dedup_map[BOOT_RENDER_TEX_DEDUP];
static uint64_t boot_render_tex_shared;   /* builds that shared another slot's resource */
static inline uint32_t boot_render_tex_dedup_hash(const uint32_t words[8], unsigned dim) {
    uint32_t h = 2166136261u ^ dim; for (unsigned k = 0; k < 8; ++k) h = (h ^ words[k]) * 16777619u;
    return (h ^ (h >> 16)) & (BOOT_RENDER_TEX_DEDUP - 1);
}
static inline void boot_render_tex_dedup_note(uint32_t index) {
    const BootRenderTexSlot *s = &boot_render_tex_slots[index];
    if (s->res && s->size) boot_render_tex_dedup_map[boot_render_tex_dedup_hash(s->words, s->dim)] = index + 1;
}
/* A valid slot other than `index` built from the identical TIC entry, table and guest bytes, or -1. */
static inline int boot_render_tex_same(uint32_t index, const uint32_t words[8], unsigned dim, uint64_t sum, uint64_t size) {
    uint32_t o = boot_render_tex_dedup_map[boot_render_tex_dedup_hash(words, dim)];
    if (!o--) return -1;
    if (o == index || o >= BOOT_RENDER_TEX_SLOTS) return -1;
    const BootRenderTexSlot *s = &boot_render_tex_slots[o];
    if (!s->valid || !s->res || s->view_of || s->dim != dim || s->size != size || boot_render_tex_sum[o] != sum || memcmp(s->words, words, 32)) return -1;
    return (int)o;
}
/* Texture residency budget. The game writes its TIC entries into a ring of indices (a new index per
 * frame for the same image), so slots whose index is not used again kept their host texture until
 * the ring wrapped: ~3200 textures / 5.7 GiB for ~400 distinct images, all of it charged to process
 * commit (video memory counts against system commit). Each use stamps the slot with the clock of
 * the guest submission; at the end of a guest submission (queue idle, nothing recorded) the least
 * recently used textures beyond the budget are released (boot_render_tex_trim). Never evicted: a
 * slot used in the submission that just ended or the one before. Module-static: a rebuilt runtime
 * starts with every stamp 0 (oldest). */
#ifndef BOOT_RENDER_TEX_BUDGET   /* host texture bytes held by TIC slots before LRU eviction (tests set a small one) */
#define BOOT_RENDER_TEX_BUDGET (UINT64_C(1536) << 20)
#endif
static uint32_t boot_render_tex_last[BOOT_RENDER_TEX_SLOTS];   /* clock of the slot's last use */
static uint32_t boot_render_tex_clock = 2;
static struct { ID3D12Resource *res; uint64_t bytes, width; uint32_t height, format; uint16_t depth, mips; } boot_render_tex_sized[BOOT_RENDER_TEX_SLOTS];   /* size cache per slot (a freed resource's address can come back with another shape) */
static uint64_t boot_render_tex_resident, boot_render_tex_resident_count, boot_render_tex_resident_peak;
static uint64_t boot_render_tex_evictions, boot_render_tex_evicted_bytes;
static inline void boot_render_tex_touch(uint32_t index) { boot_render_tex_last[index] = boot_render_tex_clock; }
/* A draw or dispatch bound texture slot `index` (its texture call returned): pending until the draw is
 * recorded (boot_render_tex_commit), counted as used by the open list meanwhile. */
static inline void boot_render_tex_pend(uint32_t index) {
    if (index >= BOOT_RENDER_TEX_SLOTS) return;
    boot_render_slot_list[index] = boot_render_list_id;
    if (boot_render_tex_pending_n < 256) boot_render_tex_pending[boot_render_tex_pending_n++] = index;
    else { boot_render_tex_pending_over = 1; boot_render_slot_rec_mark(index); }   /* drain2-work: not listed, counted as recorded now */
}
/* The draw / dispatch is being recorded now: its slots belong to the open list. */
static inline void boot_render_tex_commit(void) {
    for (unsigned i = 0; i < boot_render_tex_pending_n; ++i) { boot_render_slot_list[boot_render_tex_pending[i]] = boot_render_list_id; boot_render_slot_rec_mark(boot_render_tex_pending[i]); }   /* drain2-work: + recorded (blacktex-work: + counted) */
    boot_render_tex_pending_n = 0; boot_render_tex_pending_over = 0;
}
/* nvnpar-work (diagnostic, draw-path.log): a slot rewritten while the open list uses it closes the list and waits for the
 * GPU. The first 32 and every 256th after: the slot's state (poisoned: its guest bytes, a target over it or a re-hash
 * changed it; otherwise its TIC words changed), why the list counts as using it, and when it was used. */
static inline void boot_render_slot_reuse_log(uint32_t index, int pending) {
    static uint64_t n;
    if (++n > 32 && n % 256) return;
    if (index >= BOOT_RENDER_TEX_SLOTS || !boot_render_tex_slots) return;
    const BootRenderTexSlot *s = &boot_render_tex_slots[index];
    FILE *lf = fopen("draw-path.log", "a"); if (!lf) return;
    fprintf(lf, "texture slot reuse inside the open list #%llu: TIC %u dim %u va %llx size %llu valid %u poisoned %u words %08x %08x %08x %08x; %s; last use clock %u (now %u); submission %llu, list %llu\n",
            (unsigned long long)n, index, s->dim, (unsigned long long)s->va, (unsigned long long)s->size, s->valid, boot_render_tex_poisoned[index],
            s->words[0], s->words[1], s->words[2], s->words[3], pending ? "bound by the draw being prepared" : "bound earlier in the open list",
            boot_render_tex_last[index], boot_render_tex_clock, (unsigned long long)boot_render_exec_id, (unsigned long long)boot_render_list_id);
    fclose(lf);
}
/* drain-work (texture slot versioning): from here on slot `index`'s descriptor writes are staged until every list up to
 * fence `fence` completed (boot_render_sv_apply), and its current resource is held until then (boot_render_sv_take).
 * 0: no staging heap (the caller waits as before). */
static inline int boot_render_sv_mark(BootRender *r, uint32_t index, uint64_t fence) {   /* drain2-work: sv_defer without the held resource */
    if (index >= BOOT_RENDER_TEX_SLOTS) return 0;
    if (!boot_render_sv_heap || boot_render_sv_dev != r->gpu->device) {
        if (boot_render_sv_n || boot_render_sv_idx_n) return 0;   /* not reached: device recovery resets everything first */
        if (boot_render_sv_heap) { ID3D12DescriptorHeap_Release(boot_render_sv_heap); boot_render_sv_heap = NULL; }
        boot_render_sv_dev = NULL;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, BOOT_RENDER_SV_STAGE, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(ID3D12Device_CreateDescriptorHeap(r->gpu->device, &hd, &IID_ID3D12DescriptorHeap, (void **)&boot_render_sv_heap))) { boot_render_sv_heap = NULL; return 0; }
        boot_render_sv_dev = r->gpu->device;
        boot_render_sv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(r->gpu->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!boot_render_sv_flag[index]) {
        if (boot_render_sv_idx_n == BOOT_RENDER_SV_STAGE) boot_render_sv_apply(r, 1);   /* full: wait, everything in place */
        boot_render_sv_flag[index] = 1; boot_render_sv_idx[boot_render_sv_idx_n++] = index;
    }
    if (fence > boot_render_sv_guard) boot_render_sv_guard = fence;
    return 1;
}
static inline int boot_render_sv_defer(BootRender *r, uint32_t index, uint64_t fence) {
    if (!boot_render_sv_mark(r, index, fence)) return 0;
    boot_render_sv_hold = boot_render_tex_slots ? boot_render_tex_slots[index].res : NULL; boot_render_sv_hold_fence = fence; boot_render_sv_hold_r = r;
    return 1;
}
/* drain2-work: the first 32 and every 1024th descriptor write boot_render_sv_write_guard caught (draw-path.log). */
static inline void boot_render_sv_wg_log(BootRender *r, unsigned dim, uint32_t index, unsigned kind, uint64_t used, uint64_t fence, int how) {
    static uint64_t n;
    if (++n > 32 && n % 1024) return;
    static const char *const why[3] = {"the open list", "an executed list still running", "the open list, the index already staged (two versions in one list)"};
    static const char *const act[4] = {"written in place (TOTK_SLOT_VERSIONING=0, as before)", "staged (a reading open list was executed first, without a wait)", "waited for (no staging heap)",
                                       "staged with TOTK_SLOT_VERSIONING=0 (TOTK_TEX_KEEP: a reading open list was executed first, without a wait)"};
    const BootRenderTexSlot *s = boot_render_tex_slots && index < BOOT_RENDER_TEX_SLOTS ? &boot_render_tex_slots[index] : NULL;
    FILE *lf = fopen("draw-path.log", "a"); if (!lf) return;
    fprintf(lf, "drain2 descriptor guard #%llu: TIC %u table %u rewritten while %s may read it; slot valid %u resource %s dim %u va %llx size %llu feedback view %u;"
                " last recorded use list %llu (list now %llu, %u bindings by its draws / dispatches), its fence %llu (completed %llu); %s (slot fields: before this write)\n",
            (unsigned long long)n, index, dim, why[kind < 3 ? kind : 0], s ? s->valid : 0u, s && s->res ? "yes" : "no", s ? s->dim : 0u,
            (unsigned long long)(s ? s->va : 0), (unsigned long long)(s ? s->size : 0), index < BOOT_RENDER_TEX_SLOTS ? (unsigned)boot_render_fb_on[index] : 0u,
            (unsigned long long)used, (unsigned long long)boot_render_list_id, index < BOOT_RENDER_TEX_SLOTS ? (unsigned)boot_render_slot_rec_n[index] : 0u, (unsigned long long)fence,
            (unsigned long long)ID3D12Fence_GetCompletedValue(r->gpu->fence), act[how >= 0 && how < 4 ? how : 0]);
    fclose(lf);
}
/* Before slot `index`'s descriptors or resource change: 1 when no list that may still run uses them (drain-work, with
 * TOTK_SLOT_VERSIONING: or when every list that may still use them has been executed, the open one without a wait, and
 * the slot is versioned: boot_render_sv_defer). */
static inline int boot_render_slot_free(BootRender *r, uint32_t index) {
    if (!r->recording) return 1;   /* invariant: no open list means the queue is idle */
    const int sv = index < BOOT_RENDER_TEX_SLOTS && boot_render_sv_on();   /* drain-work */
    if (sv) boot_render_sv_hold = NULL;
    uint64_t used = index < BOOT_RENDER_TEX_SLOTS ? boot_render_slot_list[index] : boot_render_list_id;
    int open = used == boot_render_list_id || boot_render_tex_pending_over;
    for (unsigned i = 0; i < boot_render_tex_pending_n && !open; ++i) open = boot_render_tex_pending[i] == index;
    if (open) {
        ++boot_render_slot_free_open; boot_render_slot_reuse_log(index, used != boot_render_list_id);
        if (sv) {   /* drain-work: the open list is executed without a wait (recording goes on in the next one) */
            const uint64_t lid = boot_render_list_id;
            if (!boot_render_flush(r)) return 0;
            ++boot_render_sv_kicks;
            const uint64_t f = boot_render_list_fence[lid & 65535].id == lid ? boot_render_list_fence[lid & 65535].fence : r->gpu->submitted;
            if (boot_render_sv_defer(r, index, f)) return 1;
            ++boot_render_sv_fallbacks; ++boot_render_slot_free_open_waits; return boot_render_wait_fence(r->gpu, f);
        }
        ++boot_render_slot_free_open_waits; return boot_render_submit(r) && boot_render_begin(r);   /* as before */
    }
    if (used && boot_render_list_fence[used & 65535].id == used) {
        uint64_t f = boot_render_list_fence[used & 65535].fence;
        if (ID3D12Fence_GetCompletedValue(r->gpu->fence) >= f) { ++boot_render_slot_free_done; return 1; }
        if (sv && boot_render_sv_defer(r, index, f)) { ++boot_render_sv_deferred; return 1; }   /* drain-work: no wait */
        ++boot_render_slot_free_waited; return boot_render_wait_fence(r->gpu, f);
    }
    /* A list id not in the table and not open was never executed (discarded) or is over 65536 lists old
     * (long complete): nothing on the GPU can still use the slot. (play-24: the 1024-entry table missed
     * slots last used many frames ago and drained the queue ~1.3 times per submission.) */
    if (used) { ++boot_render_slot_free_done; return 1; }
    /* never bound, or its list is unknown (discarded, or older than the table): wait for everything already
     * executed; the open list (which does not use the slot) stays open */
    ++boot_render_slot_free_drain;
    return boot_render_wait_fence(r->gpu, r->gpu->submitted);
}
static inline int boot_render_tex_table(void) {
    if (boot_render_tex_slots) return 1;
    BootRenderTexTable *t = VirtualAlloc(NULL, sizeof *t, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t) return 0;
    memcpy(t->magic, "TOTKTEX1", 8); t->entry_size = sizeof(BootRenderTexSlot); t->count = BOOT_RENDER_TEX_SLOTS;
    boot_render_tex_slots = t->slots;
    char v[32]; snprintf(v, sizeof v, "%llx", (unsigned long long)(uintptr_t)t);
    SetEnvironmentVariableA("TOTK_RENDER_TEX_TABLE", v);
    return 1;
}
/* Releases the textures of the module that ran before this one (GPU idle, recorded work submitted)
 * and clears every texture descriptor, then registers this module's table. */
static inline void boot_render_tex_retire_previous(BootRender *r) {
    char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_RENDER_TEX_TABLE", v, sizeof v);
    BootRenderTexTable *old = (n && n < sizeof v) ? (BootRenderTexTable *)(uintptr_t)strtoull(v, NULL, 16) : NULL;
    BootRenderTexTable *mine = boot_render_tex_slots ? (BootRenderTexTable *)((uint8_t *)boot_render_tex_slots - offsetof(BootRenderTexTable, slots)) : NULL;
    if (old && old != mine && !memcmp(old->magic, "TOTKTEX1", 8) && old->entry_size == sizeof(BootRenderTexSlot) && old->count == BOOT_RENDER_TEX_SLOTS) {
        for (uint32_t i = 0; i < BOOT_RENDER_TEX_SLOTS; ++i) if (old->slots[i].res) ID3D12Resource_Release(old->slots[i].res);
        memset(old->magic, 0, 8);
        VirtualFree(old, 0, MEM_RELEASE);
        for (unsigned dim = 0; dim < BOOT_RENDER_DIMS; ++dim)
            for (uint32_t i = 0; i < BOOT_RENDER_TEX_SLOTS; ++i) boot_render_null_srv(r, dim, i);
    }
    boot_render_tex_table();
}
static BootRenderTsSlot boot_render_ts_slots[BOOT_RENDER_SAMPLERS];
static uint64_t boot_render_tex_created[BOOT_RENDER_DIMS];   /* host views created per table (diagnostics) */
static uint64_t boot_render_tex_reused;                      /* rebuilds that re-uploaded into the slot's texture */


/* sun-work3: TIC swizzle of the depth formats. NVN's format table gives Z16 (0x3a) and Z24S8 (0x29) the default
 * swizzle (G, 0, 0, 1): Maxwell holds their depth in the G lane (Z24S8: R = stencil uint, G = depth unorm), which is
 * why NVN's table reads G for them. The
 * D3D12 views of these formats (R16_UNORM, R24_UNORM_X8_TYPELESS; boot_tex_format) hold the depth in component 0
 * and have no component 1, so the mapping FROM_MEMORY_COMPONENT_1 read 0: every sample and every depth compare of
 * a Z16 / Z24S8 texture returned 0. TOTK's sun-shadow cascades are Z16 (720x720x4 at 523e70000, sampled by ps
 * bdfebdf1 with TEX.LL.DC): every compare failed, so the shadow visibility was 0 wherever a cascade covers the
 * scene (no direct sun on Great Sky Island; play-89 zero-flooded and play-90 empty cascades gave the same picture).
 * G of these two formats now reads component 0. ZF32 (0x2f) already uses R. TOTK_DEPTH_SWIZZLE=0: the old mapping. */
static inline int boot_render_depth_swizzle_on(void) {
    static int v = -1; if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_DEPTH_SWIZZLE", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
static inline UINT boot_render_swizzle(const BootTic *t) {
    UINT m[4];
    for (unsigned c = 0; c < 4; ++c) switch (t->source[c]) {
        case 2: m[c] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0; break;
        case 3: m[c] = ((t->components == 0x3a || t->components == 0x29) && boot_render_depth_swizzle_on()) ?
                       D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 : D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1; break;
        case 4: m[c] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2; break;
        case 5: m[c] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3; break;
        case 6: case 7: m[c] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1; break;
        default: m[c] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0; break;
    }
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(m[0], m[1], m[2], m[3]);
}

/* A TIC index is visible in exactly one texture table (the one for its type): the others hold null
 * descriptors. tex_query_dims and the buffer test in tex_fetch_1d (maxwell_runtime.hlsli) rely on it. */
static inline void boot_render_tex_isolate(BootRender *r, uint32_t index, unsigned keep) {
    for (unsigned d = 0; d < BOOT_RENDER_DIMS; ++d) if (d != keep) boot_render_null_srv(r, d, index);
}
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_tex_cpu(BootRender *r, unsigned dim, uint32_t index) {
    return boot_render_tex_dst(r, dim, index);   /* drain-work: in place, or staged while the index is versioned */
}
/* boot_render_tex_isolate after writing the SRV `sv` of `res` at the index's own table, plus one exception: a 2D
 * texture is also visible as a one-layer 2D array. Maxwell samples a 2D TIC with an array instruction (TEX.ARRAY,
 * the layer clamped to 0); TOTK's UI text shader (d6cd1dbf, tex_implicit_2da) reads its glyph atlas (a 2D R8 TIC,
 * 2048x4096) that way, and the empty 2D-array table returned 0 alpha: every glyph was discarded (no text in any
 * menu, both paths). tex_query_dims tries the 2D table first, so its answer is unchanged. */
static uint64_t boot_render_tex_array_views;
static inline void boot_render_tex_isolate_srv(BootRender *r, uint32_t index, unsigned dim, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *sv) {
    boot_render_tex_isolate(r, index, dim);
    if (dim != BOOT_DIM_2D || !res || sv->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) return;
    D3D12_RESOURCE_DESC rd; res->lpVtbl->GetDesc(res, &rd);
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.DepthOrArraySize != 1) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC a = {0};
    a.Format = sv->Format; a.Shader4ComponentMapping = sv->Shader4ComponentMapping; a.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    a.Texture2DArray.MostDetailedMip = sv->Texture2D.MostDetailedMip; a.Texture2DArray.MipLevels = sv->Texture2D.MipLevels; a.Texture2DArray.ArraySize = 1;
    ID3D12Device_CreateShaderResourceView(r->gpu->device, res, &a, boot_render_tex_cpu(r, BOOT_DIM_2D_ARRAY, index));
    ++boot_render_tex_array_views;
}
static inline void boot_render_tex_log(const BootTic *t, uint32_t index, const uint32_t words[8], const char *what) {
    static unsigned logged; if (logged >= 32) return; ++logged;
    FILE *f = fopen("draw-path.log", "a"); if (!f) return;
    fprintf(f, "texture TIC %u %s: type %u header %u format %02x/%u %ux%ux%u base layer %u levels %u view mips %u..%u gob h/d %u/%u va %llx words",
            index, what, t->texture_type, t->header_version, t->components, t->data_type[0], t->width, t->height, t->depth, t->base_layer,
            t->max_mip + 1, t->view_min_mip, t->view_max_mip, t->log_gob_h, t->log_gob_d, (unsigned long long)t->address);
    for (unsigned i = 0; i < 8; ++i) fprintf(f, " %08x", words[i]);
    fputc('\n', f); fclose(f);
}
/* Region holding guest GPU VA `va` and the byte offset in it (page table lookup), or -1. */
static inline int boot_render_region_of(BootRender *r, uint64_t va, uint64_t *offset) {
    if ((va >> 12) < r->va_base_page || (va >> 12) - r->va_base_page >= r->ptable_pages) return -1;
    uint32_t e = r->ptable_cpu[(va >> 12) - r->va_base_page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count || !r->region[e >> 24]) return -1;
    *offset = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    return (int)(e >> 24);
}

/* ---------------------------------------------------------------- GPU-side constant buffers and guest writes
 * (native NVN path, host_nvn_play.h; the emulated path does not use them).
 *
 * A constant buffer bound with BootCbufBinding.valid == 2 is read by the GPU straight from guest memory
 * (CBV on the zero-copy region resource) when the draw executes, instead of a CPU snapshot taken when it
 * is recorded: no CPU read of guest memory, so no wait for earlier GPU work that writes it. Guest memory
 * the executor must change in command order (uniform buffer updates, reports, buffer clears) is written
 * by the GPU: staged in the upload ring and copied in with CopyBufferRegion between transition barriers,
 * so it is ordered after the recorded work before it and before the work after it. */
static uint64_t boot_render_cb_direct, boot_render_gpu_writes, boot_render_gpu_write_bytes;
/* Contiguous region bytes behind [va, va + want) (stops at the first page mapped elsewhere). */
static inline uint64_t boot_render_region_span_walk(BootRender *r, uint64_t va, uint64_t want, int *region, uint64_t *offset) {
    uint64_t off0; int reg = boot_render_region_of(r, va, &off0);
    if (reg < 0) return 0;
    uint64_t span = 0x1000 - (va & 0xfff);
    while (span < want) {
        uint64_t o; if (boot_render_region_of(r, va + span, &o) != reg || o != off0 + span) break;
        span += 0x1000;
    }
    if (span > want) span = want;
    if (off0 + span > r->region_size[reg]) span = r->region_size[reg] > off0 ? r->region_size[reg] - off0 : 0;
    *region = reg; *offset = off0;
    return span;
}
/* heavy-work: the walk above, memoized (each draw asks for its constant buffers twice, and the next draws ask again). A hit
 * needs the same page table (mapping generation, table storage, base, size) and the same region object and size at the
 * result's index, so the walk would return the same: it reads only page-table entries (unchanged without a mapping
 * generation change) and the validity of that region. Failed walks are not kept. TOTK_SPAN_CACHE=0: off. */
#define BOOT_RENDER_SPAN_CACHE 1024
static struct { const BootRender *r; uint64_t va, want, gen, span, off, size; const uint32_t *pt; ID3D12Resource *res; uint32_t base, pages; int reg; } boot_render_span_cache[BOOT_RENDER_SPAN_CACHE];
static uint64_t boot_render_span_hits, boot_render_span_misses;
static inline uint64_t boot_render_region_span(BootRender *r, uint64_t va, uint64_t want, int *region, uint64_t *offset) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_SPAN_CACHE", v, sizeof v); on = !(n && n < sizeof v && v[0] == '0'); }
    if (!on) return boot_render_region_span_walk(r, va, want, region, offset);
    uint64_t h = (va >> 8) ^ (va >> 21) ^ (want >> 8) * UINT64_C(0x9e3779b1);
    __typeof__(boot_render_span_cache[0]) *c = &boot_render_span_cache[h & (BOOT_RENDER_SPAN_CACHE - 1)];
    if (c->r == r && c->va == va && c->want == want && c->gen == boot_render_map_gen && c->pt == r->ptable_cpu && c->base == r->va_base_page &&
        c->pages == r->ptable_pages && c->reg >= 0 && (unsigned)c->reg < r->region_count && r->region[c->reg] == c->res && r->region_size[c->reg] == c->size) {
        *region = c->reg; *offset = c->off; ++boot_render_span_hits; return c->span; }
    int reg = -1; uint64_t off = 0;
    uint64_t span = boot_render_region_span_walk(r, va, want, &reg, &off);
    ++boot_render_span_misses;
    if (reg < 0) return span;   /* unmapped: outputs untouched, as the walk leaves them */
    *region = reg; *offset = off;
    if (r->region[reg]) {
        c->r = r; c->va = va; c->want = want; c->gen = boot_render_map_gen; c->span = span; c->off = off; c->size = r->region_size[reg];
        c->pt = r->ptable_cpu; c->res = r->region[reg]; c->base = r->va_base_page; c->pages = r->ptable_pages; c->reg = reg;
    }
    return span;
}
/* Whether boot_render_cbv_direct can view [va, va + size) in place (no ring snapshot). */
static inline int boot_render_cbv_direct_ok(BootRender *r, uint64_t va, uint32_t size) {
    if (!size || (va & 255)) return 0;
    uint64_t want = ((uint64_t)size + 255) & ~UINT64_C(255); if (want > 65536) want = 65536;
    int reg; uint64_t off;
    return (boot_render_region_span(r, va, want, &reg, &off) & ~UINT64_C(255)) >= 256 && !(off & 255);
}
/* Ring bytes a constant-buffer snapshot of `size` takes (boot_render_cbv_ex; 256 for an unbound slot). */
static inline uint64_t boot_render_cb_ring_bytes(uint32_t size) {
    uint64_t a = ((uint64_t)(size ? size : 256) + 255) & ~UINT64_C(255);
    return a > 65536 ? 65536 : a;
}
/* CBV on guest memory: 256-byte aligned address; size rounded up to 256 (max 64 KiB) and clipped to the
 * contiguous region bytes. Returns 0 when that is impossible (the caller takes a snapshot instead). */
static inline int boot_render_cbv_direct(BootRender *r, uint64_t va, uint32_t size, uint32_t descriptor) {
    if (!size || (va & 255)) return 0;
    uint64_t want = ((uint64_t)size + 255) & ~UINT64_C(255); if (want > 65536) want = 65536;
    int reg; uint64_t off;
    uint64_t span = boot_render_region_span(r, va, want, &reg, &off) & ~UINT64_C(255);
    if (span < 256 || (off & 255)) return 0;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv = {ID3D12Resource_GetGPUVirtualAddress(r->region[reg]) + off, (UINT)span};
    if(boot_cbv_tap_active()) boot_cbv_tap_resolved(cv.BufferLocation,cv.SizeInBytes,r->region[reg],off,r->region_size[reg],
        BOOT_CBV_TAP_DIRECT_REGION,(uint32_t)reg,boot_render_cb_scope,boot_render_list_id,boot_render_map_gen,0,NULL,0);
    boot_render_cbv_put(r, &cv, descriptor);
    if (boot_sd_on()) boot_sd_note(r, BOOT_SD_K_DIRECT, va, r->region_cpu[reg] ? r->region_cpu[reg] + off : NULL, NULL, span);   /* suspect-diag S1 */
    ++boot_render_cb_direct;
    return 1;
}
/* GPU-ordered write of n bytes (n <= 64 KiB) into guest memory at va. 0 = not possible (unmapped or not
 * contiguous; the caller completes the GPU work and writes with the CPU instead). */
static inline int boot_render_gpu_write(BootRender *r, uint64_t va, const void *data, uint64_t n) {
    if (!n) return 1;
    if (n > 65536) return 0;
    int reg; uint64_t off;
    if (boot_render_region_span(r, va, n, &reg, &off) < n) return 0;
    uint64_t aligned = (n + 255) & ~UINT64_C(255);
    if (r->recording && r->ring_used + aligned > r->ring_size && (++boot_render_ring_full, !boot_render_flush(r))) return 0;
    if (!r->recording && !boot_render_begin(r)) return 0;
    if (r->ring_used + aligned > r->ring_size) return 0;
    memcpy(r->ring_cpu + r->ring_used, data, (size_t)n);
    /* EXPERIMENT (TOTK_GW_NOBARRIER=1): skip the transitions to measure their cost; ordering is not guaranteed */
    static int nobar = -1; if (nobar < 0) { char e[8]; DWORD k = GetEnvironmentVariableA("TOTK_GW_NOBARRIER", e, sizeof e); nobar = k && k < sizeof e && e[0] == '1'; }
    if (!nobar) {   /* COMMON -> COPY_DEST once per region until the next settle (boot_render_gw_settle) */
        unsigned i = 0; while (i < boot_render_gw_dest_count && boot_render_gw_dest[i] != r->region[reg]) ++i;
        if (i == boot_render_gw_dest_count) {
            D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r->region[reg];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            BOOT_GP_RB("guest write", r->cl, 1, &b);
            boot_render_gw_dest[boot_render_gw_dest_count++] = r->region[reg]; ++boot_render_gw_transitions;
        }
    }
    BOOT_GP_CBR("guest write", r->cl, r->region[reg], off, r->ring, r->ring_used, n);
    r->ring_used += aligned;
    boot_render_gmem_pending = 1;
    if (boot_render_gw_count && !boot_render_gw[boot_render_gw_count - 1].fence && boot_render_gw[boot_render_gw_count - 1].va <= va + n &&
        va <= boot_render_gw[boot_render_gw_count - 1].va + boot_render_gw[boot_render_gw_count - 1].size) {   /* extends the previous write */
        uint64_t lo = boot_render_gw[boot_render_gw_count - 1].va < va ? boot_render_gw[boot_render_gw_count - 1].va : va;
        uint64_t hi = boot_render_gw[boot_render_gw_count - 1].va + boot_render_gw[boot_render_gw_count - 1].size; if (va + n > hi) hi = va + n;
        boot_render_gw[boot_render_gw_count - 1].va = lo; boot_render_gw[boot_render_gw_count - 1].size = hi - lo;
    } else if (boot_render_gw_count < BOOT_RENDER_GW_RANGES) { boot_render_gw[boot_render_gw_count].va = va; boot_render_gw[boot_render_gw_count].size = n; boot_render_gw[boot_render_gw_count++].fence = 0; }
    else boot_render_gw_all = 1;   /* CPU readers of these ranges wait (boot_render_cpu_read) */
    ++boot_render_gpu_writes; boot_render_gpu_write_bytes += n;
    return 1;
}
/* CPU reads of guest memory that GPU work recorded earlier may still write (texture builds, index
 * expansion, indirect arguments): a module that leaves such work pending (deferred compute,
 * host_gpu_compute.h) installs a guard that completes it first when the range may be affected. */
static int (*boot_render_cpu_read_guard)(BootRender *r, uint64_t va, uint64_t size);
static inline int boot_render_cpu_read(BootRender *r, uint64_t va, uint64_t size) {
    /* a write still in the open list: execute it and wait (as before); writes in executed lists: wait only for
     * the newest such list's fence (later, unrelated work keeps running) */
    int open_hit = 0; uint64_t wait = 0;
    if (boot_render_gw_all) { if (r->recording) open_hit = 1; else wait = r->gpu->submitted; }
    for (unsigned i = 0; i < boot_render_gw_count && !open_hit; ++i)
        if (boot_render_gw[i].va < va + size && va < boot_render_gw[i].va + boot_render_gw[i].size) {
            if (!boot_render_gw[i].fence) open_hit = 1; else if (boot_render_gw[i].fence > wait) wait = boot_render_gw[i].fence;
        }
    if (open_hit) { if (!boot_render_submit(r)) return 0; }
    else if (wait && !boot_render_wait_fence(r->gpu, wait)) return 0;
    return !boot_render_cpu_read_guard || boot_render_cpu_read_guard(r, va, size);
}
/* Any valid texture slot built from guest memory overlapping [va, va + size). */
static inline int boot_render_textures_overlap(uint64_t va, uint64_t size);

/* Render-target views per TIC index (boot_render_texture): the entry's TIC words, the target entry
 * and texture viewed, and the table generation (bumped whenever an entry's texture changes, so the
 * target a fresh scan would pick is the same one). Any other view written at the index forgets it. */
typedef struct BootRenderAlias { uint32_t words[8]; ID3D12Resource *texture; uint64_t generation, dirty_epoch; uint32_t k, on; } BootRenderAlias;
static BootRenderAlias *boot_render_alias; static const BootRender *boot_render_alias_owner;
static uint64_t boot_render_alias_hits, boot_render_alias_views;   /* views reused / written */
static int boot_render_fb_slots_taken;   /* a game TIC index fell in the top 256 (reserved for feedback views) */
static uint64_t boot_render_alias_format_misses;   /* targets at a TIC's address and size in another format (sampled from guest memory instead) */
/* Diagnostic: each distinct (address, TIC format, target format) mismatch once in draw-path.log (first 128): a
 * TIC that reads a target's bits as another format (e.g. R16_FLOAT over an R16_UNORM target) decodes guest bytes
 * the target wrote in its own format, which can give NaN/Inf. */
static inline void boot_render_alias_miss_log(const BootTic *t, uint32_t index, DXGI_FORMAT want, const BootRenderTarget *x, DXGI_FORMAT view) {
    static struct { uint64_t address; uint32_t want, view; } seen[128]; static unsigned n;
    for (unsigned i = 0; i < n; ++i) if (seen[i].address == t->address && seen[i].want == (uint32_t)want && seen[i].view == (uint32_t)view) return;
    if (n == 128) return;
    seen[n].address = t->address; seen[n].want = (uint32_t)want; seen[n].view = (uint32_t)view; ++n;
    FILE *f = fopen("draw-path.log", "a"); if (!f) return;
    fprintf(f, "rt format mismatch: tic %u at %llx %ux%u fmt %02x type %u/%u/%u/%u srgb %u -> dxgi %d; target %ux%u guest fmt %x dxgi %d view %d\n",
            index, (unsigned long long)t->address, t->width, t->height, t->components, t->data_type[0], t->data_type[1], t->data_type[2], t->data_type[3], t->srgb,
            (int)want, x->width, x->height, x->format, (int)x->dxgi, (int)view);
    fclose(f);
}
static uint64_t boot_render_depth_alias_views, boot_render_depth_alias_written;   /* depth targets sampled directly / sampled while the draw writes them */
static int boot_render_no_depth_alias;   /* set by a draw that writes the depth target it samples: that texture comes from guest memory */
static inline void boot_render_alias_reset(void) {
    if (boot_render_alias) memset(boot_render_alias, 0, sizeof(BootRenderAlias) * BOOT_RENDER_TEX_SLOTS);
}
static inline BootRenderAlias *boot_render_alias_entry(BootRender *r, uint32_t index) {
    if (!boot_render_alias) { boot_render_alias = calloc(BOOT_RENDER_TEX_SLOTS, sizeof(BootRenderAlias)); if (!boot_render_alias) return NULL; }
    if (boot_render_alias_owner != r) { boot_render_alias_reset(); boot_render_alias_owner = r; }   /* another descriptor heap */
    return &boot_render_alias[index];
}
static inline void boot_render_alias_forget(uint32_t index) { if (boot_render_alias && index < BOOT_RENDER_TEX_SLOTS) boot_render_alias[index].on = 0; }
/* Target T->rts[k] sampled by a draw: reloaded when guest memory is newer (copy engine, a write-back
 * over it) or another target over its memory holds newer GPU writes (an aliased allocation, checked
 * when `overlaps` is set), then shader readable. */
static inline int boot_render_alias_use(BootRender *r, BootRenderRtTable *T, unsigned k, int overlaps) {
    BootRenderTarget *x = &T->rts[k]; BootRenderRtInfo *xi = &T->info[k];
    if (xi->known && !(x->format & BOOT_RENDER_PITCH_FLAG)) {
        uint64_t extent = boot_render_rt_extent(x, xi);
        int sync = overlaps && boot_render_rt_overlap_reload() && boot_render_rt_overlap_dirty(T, k, extent);   /* render-24 */
        if (sync || xi->stale) {
            if (sync) ++boot_render_rt_syncs;
            if (!boot_render_rt_refresh(r, k, extent, xi->log_gob_h, xi->log_gob_d)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target reload");
            xi->stale = 0;
        }
    }
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    D3D12_RESOURCE_STATES want = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (x->state != want) {
        D3D12_RESOURCE_BARRIER rb = {0}; rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; rb.Transition.pResource = x->texture;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; rb.Transition.StateBefore = x->state; rb.Transition.StateAfter = want;
        BOOT_GP_RB("rt alias", r->cl, 1, &rb); x->state = want;
    }
    ++boot_render_rt_alias_count;
    return BOOT_RENDER_OK;
}
/* Textures a TIC slot dropped for one of another shape: kept idle (they are replaced only after the
 * recorded work that used them completed) for a later texture of the same description instead of
 * destroying one and creating another; least recently pooled ones are released beyond
 * BOOT_RENDER_TEX_POOL entries or BOOT_RENDER_TEX_POOL_BYTES (estimated). Pooled textures are in
 * the shader-resource state. */
#define BOOT_RENDER_TEX_POOL 48
#define BOOT_RENDER_TEX_POOL_BYTES (UINT64_C(256) << 20)
static struct { ID3D12Resource *res; uint64_t bytes, age; } boot_render_tex_pool[BOOT_RENDER_TEX_POOL];
static uint64_t boot_render_tex_pool_bytes, boot_render_tex_pool_clock, boot_render_tex_pooled;
static inline uint64_t boot_render_tex_estimate(const D3D12_RESOURCE_DESC *d) {
    return (uint64_t)d->Width * d->Height * (d->DepthOrArraySize ? d->DepthOrArraySize : 1) * boot_render_dxgi_bytes(d->Format) * (d->MipLevels > 1 ? 4 : 3) / 3;
}
static uint64_t boot_render_tex_alias_pooled, boot_render_tex_pool_dropped;   /* t2-B: churn counters (nvn native stats) */
static inline void boot_render_tex_pool_drop(unsigned k) {
    ++boot_render_tex_pool_dropped;
    ID3D12Resource_Release(boot_render_tex_pool[k].res);
    boot_render_tex_pool_bytes -= boot_render_tex_pool[k].bytes; memset(&boot_render_tex_pool[k], 0, sizeof boot_render_tex_pool[k]);
}
/* Slots holding resource `x` (texture dedup shares one resource between slots with identical TIC
 * entries; slot textures hold one reference per slot and nothing else references them). */
static inline ULONG boot_render_res_refs(ID3D12Resource *x) { ULONG n = x->lpVtbl->AddRef(x); x->lpVtbl->Release(x); return n - 1; }
static inline void boot_render_tex_pool_put(ID3D12Resource *t) {
    if (boot_render_sv_take(t, 1)) return;   /* drain-work: a versioned slot's previous texture, pooled once its lists completed */
    if (boot_render_res_refs(t) > 1) { ID3D12Resource_Release(t); return; }   /* still another slot's image */
    D3D12_RESOURCE_DESC d; t->lpVtbl->GetDesc(t, &d);
    uint64_t bytes = boot_render_tex_estimate(&d);
    if (bytes > BOOT_RENDER_TEX_POOL_BYTES / 4) { ID3D12Resource_Release(t); return; }
    for (;;) {
        unsigned free_k = BOOT_RENDER_TEX_POOL, oldest = BOOT_RENDER_TEX_POOL;
        for (unsigned k = 0; k < BOOT_RENDER_TEX_POOL; ++k) {
            if (!boot_render_tex_pool[k].res) { if (free_k == BOOT_RENDER_TEX_POOL) free_k = k; continue; }
            if (oldest == BOOT_RENDER_TEX_POOL || boot_render_tex_pool[k].age < boot_render_tex_pool[oldest].age) oldest = k;
        }
        if (free_k < BOOT_RENDER_TEX_POOL && boot_render_tex_pool_bytes + bytes <= BOOT_RENDER_TEX_POOL_BYTES) {
            boot_render_tex_pool[free_k].res = t; boot_render_tex_pool[free_k].bytes = bytes; boot_render_tex_pool[free_k].age = ++boot_render_tex_pool_clock;
            boot_render_tex_pool_bytes += bytes; return;
        }
        if (oldest == BOOT_RENDER_TEX_POOL) { ID3D12Resource_Release(t); return; }
        boot_render_tex_pool_drop(oldest);
    }
}
static inline ID3D12Resource *boot_render_tex_pool_take(const D3D12_RESOURCE_DESC *d) {
    for (unsigned k = 0; k < BOOT_RENDER_TEX_POOL; ++k) {
        ID3D12Resource *t = boot_render_tex_pool[k].res; if (!t) continue;
        D3D12_RESOURCE_DESC pd; t->lpVtbl->GetDesc(t, &pd);
        if (pd.Dimension != d->Dimension || pd.Width != d->Width || pd.Height != d->Height || pd.DepthOrArraySize != d->DepthOrArraySize ||
            pd.MipLevels != d->MipLevels || pd.Format != d->Format || pd.SampleDesc.Count != 1 || pd.Flags != d->Flags) continue;
        boot_render_tex_pool_bytes -= boot_render_tex_pool[k].bytes; memset(&boot_render_tex_pool[k], 0, sizeof boot_render_tex_pool[k]);
        return t;
    }
    return NULL;
}
static inline void boot_render_tex_pool_release(void) {
    for (unsigned k = 0; k < BOOT_RENDER_TEX_POOL; ++k) if (boot_render_tex_pool[k].res) boot_render_tex_pool_drop(k);
}
/* TEXHEAD_1D_BUFFER: a typed buffer view of `width` elements at the header's address. The view is
 * placed on the zero-copy guest region resource (current guest contents at execution time, no
 * invalidation needed); an address that is not element aligned within its region is copied into a
 * buffer of its own instead (rebuilt when guest memory changes, like textures). */
/* Checksum of the guest bytes slot `index` was just built from (boot_render_tex_verify compares). */
static inline void boot_render_tex_seal(BootRender *r, uint32_t index) {
    const BootRenderTexSlot *slot = &boot_render_tex_slots[index];
    const uint8_t *p = slot->size ? boot_render_guest(r, slot->va, slot->size) : NULL;
    boot_render_tex_sum[index] = p ? boot_render_tex_checksum(p, slot->size) : 0;
    if (p) boot_ax_live(BOOT_AX_L_TEXEL, slot->size);   /* async-xlate: a buffer texture built from guest memory (counted on X) */
}
/* Host descriptors (native NVN path, host_nvn.h): nvnTexturePoolRegisterTexture / nvnSamplerPoolRegisterSampler
 * describe each pool entry from the NVN objects (texture + view, sampler) as decoded BootTic / BootTsc
 * values, so the renderer never reads the TIC/TSC words the driver writes into the guest pool. Entries are
 * keyed by the pool's GPU address and index; the 32-byte key (hash of the description) stands in for the
 * descriptor words wherever slots compare them. Pools without host entries (the emulated path) and indices
 * never registered read the guest words as before. */
#define BOOT_RENDER_HOST_POOLS 4
typedef struct BootRenderHostDesc { uint32_t key[8]; BootTic tic; BootTsc tsc; } BootRenderHostDesc;
typedef struct BootRenderHostTable { uint64_t pool; uint32_t cap; BootRenderHostDesc *desc; uint8_t *valid; uint64_t *ep; } BootRenderHostTable;   /* ep: async-xlate */
static BootRenderHostTable boot_render_host_tables[2][BOOT_RENDER_HOST_POOLS];   /* [0] textures, [1] samplers */
static uint64_t boot_render_host_hits, boot_render_host_guest_reads;
static inline BootRenderHostTable *boot_render_host_table(int smp, uint64_t pool, int create) {
    BootRenderHostTable *t = boot_render_host_tables[smp ? 1 : 0];
    for (unsigned k = 0; k < BOOT_RENDER_HOST_POOLS; ++k) if (t[k].desc && t[k].pool == pool) return &t[k];
    if (!create) return NULL;
    for (unsigned k = 0; k < BOOT_RENDER_HOST_POOLS; ++k) if (!t[k].desc) {
        uint32_t cap = smp ? BOOT_RENDER_SAMPLERS : BOOT_RENDER_TEX_SLOTS;
        t[k].desc = (BootRenderHostDesc *)calloc(cap, sizeof(BootRenderHostDesc)); t[k].valid = (uint8_t *)calloc(cap, 1);
        if (!t[k].desc || !t[k].valid) { free(t[k].desc); free(t[k].valid); t[k].desc = NULL; t[k].valid = NULL; return NULL; }
        t[k].pool = pool; t[k].cap = cap; return &t[k];
    }
    return NULL;
}
/* async-xlate stage 2 (TOTK_ASYNC_XLATE >= 2): the registry history (DESIGN 5.6, SMW's replay marks). A registration that replaces a valid
 * entry keeps the old one with the registry epoch it was registered at (ev) and the one that replaced it (er); the table holds each
 * entry's epoch (ep). X translating a submission captured at epoch e reads an entry registered after e from here: the newest kept entry of
 * that key with ev <= e. Registrations hold the renderer lock, as X does for each item, so the ring needs no lock of its own. The ring is
 * in er order: a lookup stops at the first entry retired at or before e. Its texture / sampler memo is reset when X starts and ends such a
 * submission (boot_ax_hist_memo_reset), so neither version is remembered for the other. */
#define BOOT_AX_HIST 8192u
typedef struct BootAxHist { uint64_t pool, ev, er; uint32_t index, smp, valid, pad; BootRenderHostDesc d; } BootAxHist;
static BootAxHist *boot_ax_hist; static uint64_t boot_ax_hist_n;
static uint64_t boot_ax_hist_c[4];   /* entries kept, lookups served an older entry, lookups that found none (current used), memo resets */
static inline void boot_ax_hist_keep(int smp, uint64_t pool, uint32_t index, const BootRenderHostTable *t, uint64_t er) {
    if (!boot_ax_hist && !(boot_ax_hist = (BootAxHist *)calloc(BOOT_AX_HIST, sizeof *boot_ax_hist))) return;
    BootAxHist *h = &boot_ax_hist[boot_ax_hist_n++ % BOOT_AX_HIST];
    h->pool = pool; h->index = index; h->smp = (uint32_t)smp; h->valid = t->valid[index]; h->pad = 0; h->ev = t->ep[index]; h->er = er; h->d = t->desc[index];
    ++boot_ax_hist_c[0];
}
static inline const BootAxHist *boot_ax_hist_at(int smp, uint64_t pool, uint32_t index, uint64_t e) {
    const uint64_t n = boot_ax_hist_n < BOOT_AX_HIST ? boot_ax_hist_n : BOOT_AX_HIST;
    for (uint64_t k = 0; boot_ax_hist && k < n; ++k) {
        const BootAxHist *h = &boot_ax_hist[(boot_ax_hist_n - 1 - k) % BOOT_AX_HIST];
        if (h->er <= e) break;
        if (h->index == index && h->pool == pool && h->smp == (uint32_t)smp && h->ev <= e) { ++boot_ax_hist_c[1]; return h; }
    }
    ++boot_ax_hist_c[2]; return NULL;
}
static inline void boot_ax_hist_memo_reset(uint64_t e) {
    const uint64_t n = boot_ax_hist_n < BOOT_AX_HIST ? boot_ax_hist_n : BOOT_AX_HIST;
    for (uint64_t k = 0; boot_ax_hist && k < n; ++k) {
        const BootAxHist *h = &boot_ax_hist[(boot_ax_hist_n - 1 - k) % BOOT_AX_HIST];
        if (h->er <= e) break;
        if (h->smp) { if (h->index < BOOT_RENDER_SAMPLERS) boot_render_smpmemo[h->index] = 0; } else if (h->index < BOOT_RENDER_TEX_SLOTS) boot_render_texmemo[h->index] = 0;
        ++boot_ax_hist_c[3];
    }
}
/* Registers entry `index` of the texture (smp 0, tic) or sampler (smp 1, tsc) pool at GPU address `pool`. */
static inline int boot_render_host_register(int smp, uint64_t pool, uint32_t index, const BootTic *tic, const BootTsc *tsc) {
    if (smp) { if (index < BOOT_RENDER_SAMPLERS) boot_render_smpmemo[index] = 0; }   /* nvnpar-work: a new descriptor */
    else if (index < BOOT_RENDER_TEX_SLOTS) boot_render_texmemo[index] = 0;
    BootRenderHostTable *t = boot_render_host_table(smp, pool, 1);
    if (!t || index >= t->cap) return 0;
    if (boot_ax_hist_on) {   /* async-xlate stage 2: the entry it replaces stays readable for the submissions captured before */
        if (!t->ep) t->ep = (uint64_t *)calloc(t->cap, 8);
        if (t->ep) { const uint64_t er = (uint64_t)__atomic_load_n(&boot_ax_reg_epoch, __ATOMIC_ACQUIRE); if (t->valid[index]) boot_ax_hist_keep(smp, pool, index, t, er); t->ep[index] = er; }
    }
    BootRenderHostDesc d; memset(&d, 0, sizeof d);
    if (tic) d.tic = *tic;
    if (tsc) d.tsc = *tsc;
    uint8_t h[32]; boot_render_sha256(&d.tic, sizeof d.tic + sizeof d.tsc, h); memcpy(d.key, h, 32);
    t->desc[index] = d; t->valid[index] = 1;
    return 1;
}
/* The descriptor words of entry `index`: the host key when registered, else the guest pool's words. */
static inline const uint32_t *boot_render_desc_words(BootRender *r, uint64_t pool, uint32_t index, int smp) {
    BootRenderHostTable *t = boot_render_host_table(smp, pool, 0);
    if (t && index < t->cap && t->valid[index]) {
        if (boot_ax_x_epoch1 && t->ep && t->ep[index] >= boot_ax_x_epoch1) {   /* async-xlate stage 2: registered after this submission's capture */
            const BootAxHist *h = boot_ax_hist_at(smp, pool, index, boot_ax_x_epoch1 - 1);
            if (h && h->valid) { ++boot_render_host_hits; return h->d.key; }
        }
        ++boot_render_host_hits; return t->desc[index].key; }
    if (t) ++boot_render_host_guest_reads;
    return (const uint32_t *)boot_render_guest(r, pool + (uint64_t)index * 32, 32);
}
static inline const BootRenderHostDesc *boot_render_host_of(const uint32_t *words) {
    if (boot_ax_hist && (const void *)words >= (const void *)boot_ax_hist && (const void *)words < (const void *)(boot_ax_hist + BOOT_AX_HIST))   /* async-xlate: a kept entry */
        return &boot_ax_hist[((const uint8_t *)words - (const uint8_t *)boot_ax_hist) / sizeof *boot_ax_hist].d;
    for (unsigned s = 0; s < 2; ++s) for (unsigned k = 0; k < BOOT_RENDER_HOST_POOLS; ++k) {
        const BootRenderHostTable *t = &boot_render_host_tables[s][k];
        if (t->desc && (const void *)words >= (const void *)t->desc && (const void *)words < (const void *)(t->desc + t->cap))
            return (const BootRenderHostDesc *)((const uint8_t *)t->desc + ((const uint8_t *)words - (const uint8_t *)t->desc) / sizeof(BootRenderHostDesc) * sizeof(BootRenderHostDesc));
    }
    return NULL;
}
static inline int boot_render_tic_decode_any(const uint32_t *w, BootTic *t) {
    const BootRenderHostDesc *h = boot_render_host_of(w);
    if (h) { *t = h->tic; return 1; }
    return boot_tic_decode(w, t);
}
static inline void boot_render_tsc_decode_any(const uint32_t *w, BootTsc *s) {
    const BootRenderHostDesc *h = boot_render_host_of(w);
    if (h) *s = h->tsc; else boot_tsc_decode(w, s);
}
static inline int boot_render_texture_buffer(BootRender *r, BootRenderTexSlot *slot, const BootTic *t, const BootTexFormat *f,
                                             const uint32_t words[8], uint32_t index) {
    if (f->block_w != 1 || f->block_h != 1 || f->decode) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "buffer texture with a block-compressed format");
    if (t->width > (1u << 27)) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "buffer texture larger than 2^27 elements");
    uint64_t bytes = (uint64_t)t->width * f->bytes;
    if (!boot_render_cpu_read(r, t->address, bytes)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending GPU writes");
    uint8_t *src = boot_render_guest(r, t->address, bytes);
    if (!src) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "buffer texture outside guest memory");
    if (!boot_render_flush_targets(r, t->address, bytes)) return BOOT_RENDER_E_D3D;
    if (slot->valid && !boot_render_slot_free(r, (uint32_t)(slot - boot_render_tex_slots))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush");
    if (slot->res) { if (!boot_render_sv_take(slot->res, 0)) ID3D12Resource_Release(slot->res); slot->res = NULL; }   /* drain-work: held while versioned */
    slot->valid = 0;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = f->unorm; sv.Shader4ComponentMapping = boot_render_swizzle(t); sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Buffer.NumElements = t->width;
    uint64_t offset = 0; int region = boot_render_region_of(r, t->address, &offset);
    ID3D12Resource *target;
    if (region >= 0 && offset % f->bytes == 0) {
        target = r->region[region]; sv.Buffer.FirstElement = offset / f->bytes;
        slot->view_of = target; slot->view_offset = offset; slot->size = 0;
    } else {
        void *map = NULL;
        target = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, &map);
        if (!target) return boot_render_fail(r, BOOT_RENDER_E_D3D, "buffer texture copy");
        memcpy(map, src, (size_t)bytes); ID3D12Resource_Unmap(target, 0, NULL);
        slot->res = target; slot->view_of = NULL; slot->size = bytes;
    }
    ID3D12Device_CreateShaderResourceView(r->gpu->device, target, &sv, boot_render_tex_cpu(r, BOOT_DIM_BUFFER, index));
    boot_render_tex_isolate(r, index, BOOT_DIM_BUFFER);
    memcpy(slot->words, words, 32); slot->valid = 1; slot->dim = BOOT_DIM_BUFFER; slot->va = t->address; boot_render_tex_mark(index);
    boot_render_tex_seal(r, index);
    if (boot_render_tex_created[BOOT_DIM_BUFFER]++ < 4) boot_render_tex_log(t, index, words, slot->res ? "buffer (copied)" : "buffer (zero-copy)");
    return BOOT_RENDER_OK;
}

/* A 2D single-level texture over exactly one dirty colour target's memory, in another format of the same
 * texel size and the same block-linear layout (a target sampled as another format: the guest path would
 * write the target back, wait, and decode the same bytes in the TIC's format). Built on the GPU instead,
 * in queue order and without a wait: target -> buffer (target format) -> texture (TIC format), the same
 * bytes reinterpreted as the hardware reads them. The slot is a snapshot: writes to the target invalidate
 * it (boot_render_rt_copies_written). Returns -1 when the case does not apply (nothing changed). */
static inline void boot_render_rt_copies_written(const BootRenderTarget *x) {
    int filter = boot_render_rt_copy_filter_on();   /* heavy-work */
    if (filter) { unsigned b = boot_render_rt_copy_bit(x->texture); if (!(boot_render_rt_copy_bloom[b >> 6] >> (b & 63) & 1)) return; }
    uint64_t keep[64]; if (filter) memset(keep, 0, sizeof keep);
    for (unsigned k = 0; k < boot_render_rt_copy_count; ++k) {
        if (boot_render_rt_copies[k].rt != x->texture) { if (filter) { unsigned b = boot_render_rt_copy_bit(boot_render_rt_copies[k].rt); keep[b >> 6] |= UINT64_C(1) << (b & 63); } continue; }
        uint32_t i = boot_render_rt_copies[k].index;
        if (boot_render_tex_slots && boot_render_tex_slots[i].valid && boot_render_tex_slots[i].res) { boot_render_tex_poison(i); ++boot_render_tex_from_rt_invalidated; }
        boot_render_rt_copies[k] = boot_render_rt_copies[--boot_render_rt_copy_count]; --k;
    }
    if (filter) memcpy(boot_render_rt_copy_bloom, keep, sizeof keep);   /* exactly the remaining entries */
}
/* wb-01: registrations of slot `index` (its earlier build's sources) are dropped when it is built again. */
static inline void boot_render_rt_copies_forget(uint32_t index) {
    for (unsigned k = 0; k < boot_render_rt_copy_count; ++k)
        if (boot_render_rt_copies[k].index == index) { boot_render_rt_copies[k] = boot_render_rt_copies[--boot_render_rt_copy_count]; --k; }
}
static inline void boot_render_rt_copies_add(ID3D12Resource *rt, uint32_t index) {
    for (unsigned k = 0; k < boot_render_rt_copy_count; ++k) if (boot_render_rt_copies[k].rt == rt && boot_render_rt_copies[k].index == index) return;
    if (boot_render_rt_copy_count == BOOT_RENDER_RT_COPIES) {   /* full: the oldest entry's slot is rebuilt on its next use */
        unsigned k = boot_render_rt_copy_next % BOOT_RENDER_RT_COPIES; uint32_t i = boot_render_rt_copies[k].index;
        if (boot_render_tex_slots[i].valid) boot_render_tex_poison(i);
        boot_render_rt_copies[k] = boot_render_rt_copies[--boot_render_rt_copy_count];
    }
    boot_render_rt_copies[boot_render_rt_copy_count].rt = rt; boot_render_rt_copies[boot_render_rt_copy_count++].index = index; ++boot_render_rt_copy_next;
    boot_render_rt_copy_note(rt);   /* heavy-work */
}
/* wb-01: textures over dirty targets that still take the guest path (write-back + GPU wait), by reason; the first
 * 64 distinct (address, reason) pairs are logged. */
enum { BOOT_RT_DECLINE_SHAPE, BOOT_RT_DECLINE_SEVERAL, BOOT_RT_DECLINE_OTHER, BOOT_RT_DECLINE_STALE, BOOT_RT_DECLINE_LAYOUT, BOOT_RT_DECLINES };
static uint64_t boot_render_tex_rt_declines[BOOT_RT_DECLINES], boot_render_tex_from_rt_newest;
static inline int boot_render_tex_rt_decline(unsigned why, const BootRenderRtTable *T, const BootTic *t, uint32_t index, unsigned dim, unsigned levels,
                                             uint32_t layers, unsigned n, int k) {
    static const char *const name[BOOT_RT_DECLINES] = {
        "not one level/layer of a 2D uncompressed block-linear texture", "several dirty targets (TOTK_RT_OVERLAP_RELOAD=1)",
        "the newest target over it is another image", "the newest target over it is stale", "the newest target's layout differs" };
    ++boot_render_tex_rt_declines[why];
    static struct { uint64_t address; unsigned why; } seen[64]; static unsigned m;
    for (unsigned i = 0; i < m; ++i) if (seen[i].address == t->address && seen[i].why == why) return -1;
    if (m == 64) return -1;
    seen[m].address = t->address; seen[m].why = why; ++m;
    FILE *lf = fopen("draw-path.log", "a"); if (!lf) return -1;
    fprintf(lf, "texture over dirty targets (guest path: write-back and wait): tic %u at %llx %ux%u fmt %02x type %u dim %u levels %u layers %u %s: %s; %u dirty over it",
            index, (unsigned long long)t->address, t->width, t->height, t->components, t->data_type[0], dim, levels, layers, t->pitch ? "pitch" : "block-linear", name[why], n);
    if (k >= 0) {
        const BootRenderTarget *x = &T->rts[k];
        fprintf(lf, ", newest #%d at %llx %ux%u guest fmt %x dxgi %d%s", k, (unsigned long long)x->address, x->width, x->height, x->format, (int)x->dxgi,
                T->info[k].dirty ? "" : " (clean)");
    }
    fputc('\n', lf); fclose(lf);
    return -1;
}
static ID3D12Resource *boot_render_reint_buf; static uint64_t boot_render_reint_bytes;
static inline int boot_render_texture_subres(BootRender *r, BootRenderTexSlot *slot, const BootTic *t, const BootTexFormat *f, const uint32_t words[8],
                                             uint32_t index, unsigned dim, unsigned levels, uint64_t stride, uint32_t layers, uint64_t data_va,
                                             const unsigned *list, unsigned n);   /* wb-02 (below) */
/* batch-b (depth feedback on the GPU, TOTK_DEPTH_FEEDBACK_GPU=0: off). A draw that tests and writes the depth target
 * that one of its textures samples (play-87, looking down from a tower: ~42 such draws per frame over the 1280x720 ZF32
 * scene depth, sampled as fmt 2f) took the guest path for that texture: D3D12 cannot read a depth resource while it is
 * written, so the texture was rebuilt for every such draw from guest memory: a render-to-texture view built and dropped
 * first (boot_render_slot_free: a submission and a GPU wait), then every dirty target over the memory written back
 * (another submission and wait), the bytes detiled on the CPU and uploaded. That was ~58% of the render thread (~14 of
 * the ~25 us per draw). The texture the guest path builds is the image the write-back leaves last over its memory
 * (boot_render_rt_newest_over), decoded in the TIC's format. When that is exactly one single-plane depth target of this
 * address and extent (D32F read as R32F, D16 as R16 UNORM: the views boot_render_depth_srv_format makes; same texel size
 * and block layout, checked below as for colour targets), the decode returns the target's own bits, so the copy on the
 * GPU in queue order (target -> reinterpretation buffer in the depth plane's typeless format -> texture in the view
 * format, as boot_render_assemble_copies does for depth arrays) is the same texture without the write-back.
 * Rebuilding the same slot (the next such draw: the slot is invalidated once this draw's textures are bound, as its own
 * depth writes come after the copy) copies into the slot's own texture in queue order and keeps its descriptors, as
 * boot_render_texture_subres does for "the same image": draws recorded before it read the earlier image (the barriers
 * order the copy after them), so neither a submission nor a wait. */
static inline int boot_render_dfb_source(const BootRenderTarget *x, DXGI_FORMAT want) {
    return x->texture && (x->format & 0x10000u) && !(x->format & BOOT_RENDER_PITCH_FLAG) &&
           (x->dxgi == DXGI_FORMAT_D32_FLOAT || x->dxgi == DXGI_FORMAT_D16_UNORM) && boot_render_depth_srv_format(x->dxgi) == want && boot_render_dfb_on();
}
static inline void boot_render_dfb_note(uint32_t index, const BootRenderTarget *x, const uint32_t words[8], unsigned dim, int in_place) {
    boot_render_dfb_slot[index] = 1;   /* after boot_render_tex_mark, which clears it */
    {   uint32_t h = boot_render_tex_dedup_hash(words, dim);   /* not guest bytes: never shared by the guest path's dedup */
        if (boot_render_tex_dedup_map[h] == index + 1) boot_render_tex_dedup_map[h] = 0; }
    if (x->texture == boot_render_dfb_zt) { if (boot_render_dfb_n < 32) boot_render_dfb_list[boot_render_dfb_n++] = index; else boot_render_dfb_over = 1; }
    if (in_place) ++boot_render_dfb_refreshes; else ++boot_render_dfb_built;
}
/* The same slot again: its texture (this slot's alone, the view's shape) refreshed from target x in queue order; the
 * descriptors written by its first build are kept (same TIC words, so the same view). -1: not this case (nothing changed). */
static inline int boot_render_dfb_in_place(BootRender *r, BootRenderRtTable *T, BootRenderTexSlot *slot, BootRenderTarget *x, const uint32_t words[8],
                                           uint32_t index, unsigned dim, uint64_t data_va, uint64_t stride, DXGI_FORMAT want,
                                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *xfp, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *tfp, uint64_t xtotal,
                                           const unsigned *list, unsigned n, int kx) {
    if (!boot_render_dfb_slot[index] || !slot->valid || !slot->res || slot->view_of || slot->dim != dim || slot->va != data_va || slot->size != stride) return -1;
    if ((slot->words[0] ^ words[0]) != (boot_render_tex_poisoned[index] ? 0x80000000u : 0u) || memcmp(slot->words + 1, words + 1, 28)) return -1;
    if (!boot_render_reint_buf || boot_render_reint_bytes < xtotal || boot_render_res_refs(slot->res) != 1) return -1;
    D3D12_RESOURCE_DESC pd; slot->res->lpVtbl->GetDesc(slot->res, &pd);
    if (pd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || pd.Width != x->width || pd.Height != x->height || pd.DepthOrArraySize != 1 || pd.MipLevels != 1 ||
        pd.Format != want || pd.SampleDesc.Count != 1 || pd.Flags != D3D12_RESOURCE_FLAG_NONE) return -1;
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    ID3D12Resource *tex = slot->res;
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b[2]; memset(b, 0, sizeof b);
    for (unsigned k = 0; k < 2; ++k) { b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    b[0].Transition.pResource = tex; b[0].Transition.StateBefore = srv; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    BOOT_GP_RB("depth feedback", r->cl, 1, &b[0]);
    D3D12_RESOURCE_STATES was = x->state;
    if (!boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
        slot->res = NULL; slot->valid = 0; boot_render_release_later(tex);   /* left in COPY_DEST: rebuilt on its next use */
        return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target state");
    }
    D3D12_TEXTURE_COPY_LOCATION bl = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, xl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    bl.PlacedFootprint = *xfp;
    BOOT_GP_CTR("depth feedback", r->cl, &bl, 0, 0, 0, &xl, NULL);
    boot_render_rt_barrier(r, x, was);
    b[0].Transition.pResource = boot_render_reint_buf; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    BOOT_GP_RB("depth feedback", r->cl, 1, &b[0]);
    D3D12_TEXTURE_COPY_LOCATION tl = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, bl2 = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
    bl2.PlacedFootprint = *tfp;
    BOOT_GP_CTR("depth feedback", r->cl, &tl, 0, 0, 0, &bl2, NULL);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.pResource = tex; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[1].Transition.StateAfter = srv;
    BOOT_GP_RB("depth feedback", r->cl, 2, b);
    memcpy(slot->words, words, 32); boot_render_tex_mark(index);   /* slot->va / size / dim and the verifier's sum: unchanged (no guest bytes written) */
    boot_render_rt_copies_forget(index);   /* wb-01, as the first build: the source, then every other dirty target over the memory */
    boot_render_rt_copies_add(x->texture, index);
    for (unsigned q = 0; q < n; ++q) if ((int)list[q] != kx) boot_render_rt_copies_add(T->rts[list[q]].texture, index);
    boot_render_dfb_note(index, x, words, dim, 1);
    return BOOT_RENDER_OK;
}
/* Around a draw's texture loop (host_gpu_render_draw.h): `zt` is the depth target the draw tests and writes, or NULL.
 * After the loop the slots built from it are invalidated (the guest path's equivalent: the target, written back while
 * building them, was made dirty again after the loop and every texture over its memory invalidated). */
static inline void boot_render_dfb_begin(ID3D12Resource *zt) { boot_render_dfb_zt = boot_render_dfb_on() ? zt : NULL; boot_render_dfb_n = 0; boot_render_dfb_over = 0; }
static inline void boot_render_dfb_end(const BootRenderTarget *zt) {
    if (boot_render_dfb_over && zt) boot_render_rt_copies_written(zt);   /* more than the list holds: every copy of the target */
    for (unsigned q = 0; q < boot_render_dfb_n; ++q) {
        uint32_t i = boot_render_dfb_list[q];
        if (boot_render_tex_slots[i].valid && boot_render_tex_slots[i].res) boot_render_tex_poison(i);
    }
    boot_render_dfb_n = 0; boot_render_dfb_over = 0; boot_render_dfb_zt = NULL;
}
/* wb-01 (render-39 follow-up): with render-39 a poisoned slot stays poisoned until rebuilt; before it, two
 * invalidations restored the slot. TOTK's transient memory holds several targets at once (other shapes and
 * formats over the same addresses), and when two of them went from clean to dirty (boot_render_rt_touch: one
 * invalidation each) a texture over them was silently kept with its old image. Now it is rebuilt, but this path
 * took only a single dirty target over the memory, so each rebuild wrote every target there back and waited for the
 * GPU (play-74..78: ~4 write-back submissions per frame at the texture site, ~6 targets each, 8 fps). The source is
 * now the target the write-back would leave last in this memory (boot_render_rt_newest_over, draw order): when it is
 * exactly this image (address, size, texel size, block layout), the guest path would decode its bytes and nothing
 * else, so the copy from it on the GPU gives the same texture without the write-back. Every dirty target over the
 * memory is registered: a later draw to any of them (dirty already, so boot_render_rt_touch invalidates nothing)
 * rebuilds the slot; clean ones invalidate it when they become dirty, as before. */
static inline int boot_render_texture_from_target(BootRender *r, BootRenderTexSlot *slot, const BootTic *t, const BootTexFormat *f, const uint32_t words[8],
                                                  uint32_t index, unsigned dim, unsigned levels, uint64_t stride, uint32_t layers, uint64_t data_va) {
    if (boot_render_tex_from_rt_off) return -1;
    BootRenderRtTable *T = boot_render_rtab(r);
    unsigned list[BOOT_RENDER_RT_CAP], n = boot_render_rt_dirty_over(T, data_va, stride * layers, list);
    if (!n) return -1;   /* no target to write back: the guest path reads memory as it is, without a wait for targets */
    if (dim != BOOT_DIM_2D || levels != 1 || layers != 1 || t->pitch || t->base_layer || t->view_min_mip || f->decode || f->block_w != 1 || f->block_h != 1)
        return boot_render_texture_subres(r, slot, t, f, words, index, dim, levels, stride, layers, data_va, list, n);   /* wb-02: several subresources (declines count as shape) */
    DXGI_FORMAT want = (t->srgb && f->srgb != DXGI_FORMAT_UNKNOWN) ? f->srgb : f->unorm;
    int kx = boot_render_rt_newest_over(T, data_va, stride, list, n);
    if (kx < 0) return boot_render_tex_rt_decline(BOOT_RT_DECLINE_SEVERAL, T, t, index, dim, levels, layers, n, -1);
    BootRenderTarget *x = &T->rts[kx]; BootRenderRtInfo *in = &T->info[kx];
    const int dfb = boot_render_dfb_source(x, want);   /* batch-b: a single-plane depth target read as its own bits */
    if (!x->texture || ((x->format & (0x10000u | BOOT_RENDER_PITCH_FLAG)) && !dfb) || x->address != data_va || x->width != t->width || x->height != t->height)
        return boot_render_tex_rt_decline(BOOT_RT_DECLINE_OTHER, T, t, index, dim, levels, layers, n, kx);
    if (in->stale) return boot_render_tex_rt_decline(BOOT_RT_DECLINE_STALE, T, t, index, dim, levels, layers, n, kx);
    /* play-89: a depth source whose only difference is trailing padding (texture stride >= target extent, same texel
     * size and block layout) holds the same single-level image (TOTK_DFB_EXTENT_RELAX=0: exact extent as before) */
    {   static int relax = -1; if (relax < 0) { char e[8]; DWORD k = GetEnvironmentVariableA("TOTK_DFB_EXTENT_RELAX", e, sizeof e); relax = !(k && k < sizeof e && e[0] == '0'); }
        int same_bl = boot_render_dxgi_bytes(x->dxgi) == f->bytes && in->log_gob_h == t->log_gob_h && in->log_gob_d == t->log_gob_d;
        int ext_ok = in->extent == stride || (relax && dfb && same_bl && stride >= in->extent);
        if (!same_bl || !ext_ok) {
            static unsigned logged; if (dfb && logged < 24) { ++logged;
                boot_draw_path_logf("depth feedback layout decline: tic %u %ux%u: target bytes %u gob h/d %u/%u extent %llu; texture bytes %u gob h/d %u/%u stride %llu",
                    (unsigned)index, (unsigned)t->width, (unsigned)t->height, (unsigned)boot_render_dxgi_bytes(x->dxgi), (unsigned)in->log_gob_h, (unsigned)in->log_gob_d,
                    (unsigned long long)in->extent, (unsigned)f->bytes, (unsigned)t->log_gob_h, (unsigned)t->log_gob_d, (unsigned long long)stride); }
            return boot_render_tex_rt_decline(BOOT_RT_DECLINE_LAYOUT, T, t, index, dim, levels, layers, n, kx); } }
    D3D12_RESOURCE_DESC xd; x->texture->lpVtbl->GetDesc(x->texture, &xd);
    if (xd.MipLevels != 1 || xd.DepthOrArraySize != 1) return boot_render_tex_rt_decline(BOOT_RT_DECLINE_LAYOUT, T, t, index, dim, levels, layers, n, kx);
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = t->width; d.Height = t->height; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = want; d.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT xfp, tfp; UINT rows; UINT64 rs, xtotal, ttotal;
    ID3D12Device_GetCopyableFootprints(r->gpu->device, &xd, 0, 1, 0, &xfp, &rows, &rs, &xtotal);
    ID3D12Device_GetCopyableFootprints(r->gpu->device, &d, 0, 1, 0, &tfp, &rows, &rs, &ttotal);
    if (xfp.Footprint.RowPitch != tfp.Footprint.RowPitch || xtotal != ttotal || xfp.Offset || tfp.Offset)
        return boot_render_tex_rt_decline(BOOT_RT_DECLINE_LAYOUT, T, t, index, dim, levels, layers, n, kx);
    if (dfb) { int rc = boot_render_dfb_in_place(r, T, slot, x, words, index, dim, data_va, stride, want, &xfp, &tfp, xtotal, list, n, kx); if (rc >= 0) return rc; }
    if (!boot_render_cpu_read(r, data_va, stride)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending GPU writes");
    const uint8_t *src = boot_render_guest(r, data_va, stride);
    if (!src) return -1;
    /* as the guest path: a changed slot may still be referenced by recorded draws */
    if (slot->valid && !boot_render_slot_free(r, (uint32_t)(slot - boot_render_tex_slots))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush");
    ID3D12Resource *previous = slot->res, *tex = NULL; slot->res = NULL; slot->valid = 0;
    if (!boot_render_begin(r)) { if (previous) boot_render_tex_pool_put(previous); return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset"); }
    D3D12_RESOURCE_BARRIER b[2]; memset(b, 0, sizeof b);
    for (unsigned k = 0; k < 2; ++k) { b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (previous) {
        D3D12_RESOURCE_DESC pd; previous->lpVtbl->GetDesc(previous, &pd);
        if (pd.Dimension == d.Dimension && pd.Width == d.Width && pd.Height == d.Height && pd.DepthOrArraySize == 1 && pd.MipLevels == 1 && pd.Format == d.Format &&
            pd.Flags == d.Flags && boot_render_res_refs(previous) == 1) {
            tex = previous; previous = NULL; ++boot_render_tex_reused;
            b[0].Transition.pResource = tex; b[0].Transition.StateBefore = srv; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            BOOT_GP_RB("reinterpret", r->cl, 1, &b[0]);
        } else { boot_render_tex_pool_put(previous); previous = NULL; }
    }
    if (!tex && (tex = boot_render_tex_pool_take(&d)) != NULL) {
        b[0].Transition.pResource = tex; b[0].Transition.StateBefore = srv; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("reinterpret", r->cl, 1, &b[0]); ++boot_render_tex_pooled;
    }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    if (!tex && FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex)))
        return boot_render_fail(r, BOOT_RENDER_E_D3D, "texture resource (from target)");
    if (boot_render_reint_bytes < xtotal) {   /* the staging buffer (default heap, COPY_DEST between uses) */
        if (boot_render_reint_buf) boot_render_release_later(boot_render_reint_buf);
        boot_render_reint_buf = NULL; boot_render_reint_bytes = 0;
        D3D12_RESOURCE_DESC bd = {0}; uint64_t want_bytes = xtotal < (UINT64_C(16) << 20) ? (UINT64_C(16) << 20) : xtotal;
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = want_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&boot_render_reint_buf))) {
            boot_render_reint_buf = NULL; boot_render_release_later(tex); return boot_render_fail(r, BOOT_RENDER_E_D3D, "texture staging (from target)"); }
        boot_render_reint_bytes = want_bytes;
    }
    D3D12_RESOURCE_STATES was = x->state;
    if (!boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE)) { boot_render_release_later(tex); return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target state"); }
    D3D12_TEXTURE_COPY_LOCATION bl = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, xl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    bl.PlacedFootprint = xfp;
    BOOT_GP_CTR("reinterpret", r->cl, &bl, 0, 0, 0, &xl, NULL);
    boot_render_rt_barrier(r, x, was);
    b[0].Transition.pResource = boot_render_reint_buf; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    BOOT_GP_RB("reinterpret", r->cl, 1, &b[0]);
    D3D12_TEXTURE_COPY_LOCATION tl = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, bl2 = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
    bl2.PlacedFootprint = tfp;
    BOOT_GP_CTR("reinterpret", r->cl, &tl, 0, 0, 0, &bl2, NULL);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.pResource = tex; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[1].Transition.StateAfter = srv;
    BOOT_GP_RB("reinterpret", r->cl, 2, b);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = want; sv.Shader4ComponentMapping = boot_render_swizzle(t); sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(r->gpu->device, tex, &sv, boot_render_tex_cpu(r, dim, index));
    boot_render_tex_isolate_srv(r, index, dim, tex, &sv);
    memcpy(slot->words, words, 32); slot->res = tex; slot->valid = 1; slot->dim = dim; boot_render_tex_mark(index);
    slot->va = data_va; slot->size = stride;
    boot_render_tex_sum[index] = boot_render_tex_checksum(src, stride);   /* guest bytes as they are (the verifier's reference) */
    boot_render_rt_copies_forget(index);   /* wb-01: the source, then every other dirty target over the memory */
    boot_render_rt_copies_add(x->texture, index);
    for (unsigned q = 0; q < n; ++q) if ((int)list[q] != kx) boot_render_rt_copies_add(T->rts[list[q]].texture, index);
    if (dfb) boot_render_dfb_note(index, x, words, dim, 0);   /* batch-b */
    if (n > 1 || !in->dirty) ++boot_render_tex_from_rt_newest;
    ++boot_render_tex_from_rt; ++boot_render_tex_created[dim];
    return BOOT_RENDER_OK;
}
/* A 2D mipmapped texture whose every level is a host colour target (a mip chain rendered level by level:
 * TOTK's bloom chain, TIC "fmt 21 320x180 mips 5" at 50e2d0000 with levels drawn at 50e320000, 50e334000,
 * 50e339000, 50e33a800). The render-to-texture view below covers only level 0 (the target at the TIC's
 * address), so levels 1.. read as level 0 and the blurred levels were never seen (no bloom). Built on the
 * GPU in queue order: each level's target is copied into that mip of the slot's texture; the copies are
 * registered like boot_render_texture_from_target's, so drawing to any level rebuilds the slot on its next
 * use. Returns -1 when not every level has a target of the TIC's format (nothing changed).
 * Also 2D arrays whose layers (and levels) are targets (render-07): TOTK's half-res local-light buffer is a
 * two-layer R11G11B10 array (512x288x2 at 50f2c0000, layers drawn at 50f2c0000 and 50f380000) that the tiled
 * shading pass samples as an array. Arrays took the guest path, which decoded memory the targets never wrote
 * back (play-19: "slot valid ... nz 0/0" while the targets held the light): every local light, the torch and
 * the title screen's mural lights, was lost in both NVN paths. */
static uint64_t boot_render_tex_mips_from_rt, boot_render_tex_mips_in_place;
/* The copies of an assembly: target k[q] into subresource q of `tex` (state COPY_DEST). Colour targets copy
 * directly. Depth targets (render-09: Z16 / Z32F shadow-map arrays, e.g. TOTK's 512x512x2 two-level sun
 * shadow map at 524210000 that the tiled shading samples as a Z16 array) cannot be copied into a colour texture
 * directly, so they go through the reinterpretation buffer: every layer and level into its placed footprint
 * (the depth resource's typeless format), then the buffer into the texture (the view format). */
static uint64_t boot_render_tex_depth_assembled;
static inline void boot_render_asm_record(const BootRenderRtTable *T, uint32_t index, const int *k, unsigned n) {
    if (index >= BOOT_RENDER_TEX_SLOTS) return;
    if (!boot_render_asm_of && !(boot_render_asm_of = calloc(BOOT_RENDER_TEX_SLOTS, sizeof *boot_render_asm_of))) return;
    unsigned h = boot_render_asm_of[index];
    if (h) --h;
    else {   /* a free record, else the least recently assembled one (its texture is rebuilt on next use) */
        unsigned best = 0;
        for (unsigned e = 0; e < BOOT_RENDER_ASM_POOL; ++e) {
            if (!boot_render_asm[e].on) { best = e; break; }
            if (boot_render_asm[e].used < boot_render_asm[best].used) best = e;
        }
        if (boot_render_asm[best].on) {
            uint32_t old = boot_render_asm[best].index;
            if (old < BOOT_RENDER_TEX_SLOTS) {
                boot_render_asm_of[old] = 0;
                if (boot_render_tex_slots && boot_render_tex_slots[old].valid) boot_render_tex_poison(old);
            }
            ++boot_render_asm_evictions;
        }
        h = best; boot_render_asm_of[index] = (uint16_t)(h + 1);
    }
    boot_render_asm[h].index = index; boot_render_asm[h].n = n; boot_render_asm[h].on = 1; boot_render_asm[h].copied = boot_render_rt_write_seq;
    boot_render_asm[h].used = ++boot_render_asm_clock;
    for (unsigned q = 0; q < n && q < 64; ++q) { boot_render_asm[h].k[q] = (uint16_t)k[q]; boot_render_asm[h].src[q] = T->rts[k[q]].texture; }
}
/* An assembled slot whose source targets were drawn (or replaced) since its copies. */
/* render-17 A/B switches for the target-assembly path (read once; "0" turns a stage off):
 *   TOTK_TEX_ASSEMBLE=0  no assembly of mip chains / arrays / depth arrays from targets (render-01/07/09)
 *   TOTK_ASM_REFRESH=0   assembled textures are not refreshed when their targets are drawn again (render-14/15) */
static inline int boot_render_ab_env(const char *name, int *cache) {
    if (*cache < 0) { char v[8]; DWORD n = GetEnvironmentVariableA(name, v, sizeof v); *cache = !(n && n < sizeof v && v[0] == '0'); }
    return *cache;
}
static int boot_render_ab_assemble = -1, boot_render_ab_refresh = -1;
static inline int boot_render_asm_stale(BootRender *r, uint32_t index) {
    if (!boot_render_ab_env("TOTK_ASM_REFRESH", &boot_render_ab_refresh)) return 0;
    if (!boot_render_asm_of || index >= BOOT_RENDER_TEX_SLOTS || !boot_render_asm_of[index]) return 0;
    unsigned h = boot_render_asm_of[index] - 1u;
    if (!boot_render_asm[h].on || boot_render_asm[h].index != index) return 0;
    const BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned q = 0; q < boot_render_asm[h].n; ++q) {
        unsigned i = boot_render_asm[h].k[q];
        if (i >= T->count || T->rts[i].texture != boot_render_asm[h].src[q] || boot_render_rt_written_at[i] > boot_render_asm[h].copied) {
            ++boot_render_asm_refreshes; return 1; }
    }
    return 0;
}
/* Records only suspicious unchanged-descriptor returns, at most once per
 * assembly/submission/observed API epoch; no extra GPU work or guest-byte reads. */
/* Diagnostic79: only preexisting renderer storage; never bind/allocate a table. */
static inline void boot_render_title_source(const BootRenderRtTable *T,unsigned i,unsigned slot,unsigned layer,uint64_t copied,const void *historical) {
    if(!boot_title_event_active || !T || i>=T->count) return;
    const BootRenderTarget *x=&T->rts[i]; const BootRenderRtInfo *in=&T->info[i];
    BootTitleRecord e={0}; e.kind=BOOT_TITLE_SOURCE; e.sub=boot_title_sub; e.event=boot_title_event;
    e.v[0]=slot; e.v[1]=layer; e.v[2]=i; e.v[3]=x->address; e.v[4]=(uintptr_t)x->texture;
    e.v[5]=x->width; e.v[6]=x->height; e.v[7]=x->format; e.v[8]=x->dxgi; e.v[9]=(uint64_t)x->state;
    e.v[10]=in->known | ((uint64_t)in->dirty<<8) | ((uint64_t)in->stale<<16);
    e.v[11]=boot_render_rt_written_at[i]; e.v[12]=copied; e.v[13]=boot_render_rt_write_seq; e.v[14]=T->generation;
    e.v[15]=boot_render_rt_extent(x,in); e.v[16]=in->last_use; e.v[17]=(uintptr_t)historical; boot_title_push(&e);
}
static inline void boot_render_title_assembly(BootRender *r,unsigned index,unsigned kind,unsigned why,unsigned layers,unsigned levels) {
    if(!boot_title_event_active || !boot_render_tex_slots || index>=BOOT_RENDER_TEX_SLOTS) return;
    const BootRenderTexSlot *slot=&boot_render_tex_slots[index];
    if(!boot_title_range(slot->va,1) || !boot_render_asm_of || !boot_render_asm_of[index]) return;
    unsigned h=boot_render_asm_of[index]-1u;
    if(h>=BOOT_RENDER_ASM_POOL || !boot_render_asm[h].on || boot_render_asm[h].index!=index) return;
    BootRenderRtTable *T=boot_render_image_table(r); if(!T) return;
    boot_title_interesting=1;
    BootTitleRecord e={0}; e.kind=kind; e.sub=boot_title_sub; e.event=boot_title_event;
    e.v[0]=index; e.v[1]=slot->va; e.v[2]=(uintptr_t)slot->res; e.v[3]=slot->dim; e.v[4]=slot->valid;
    e.v[5]=why; e.v[6]=layers; e.v[7]=levels; e.v[8]=boot_render_asm[h].n; e.v[9]=boot_render_asm[h].copied;
    e.v[10]=boot_render_rt_write_seq; e.v[11]=T->generation; e.v[12]=h;
    for(unsigned j=0;j<8;++j) e.v[13+j]=slot->words[j]; /* already cached TIC metadata */
    boot_title_push(&e);
    for(unsigned q=0;q<boot_render_asm[h].n && q<64;++q)
        boot_render_title_source(T,boot_render_asm[h].k[q],index,q,boot_render_asm[h].copied,boot_render_asm[h].src[q]);
}
static inline void boot_render_image_asm_fast(BootRender *r, uint32_t index) {
    if (!boot_image_trace_enabled || !boot_render_asm_of || !boot_render_asm_of[index]) return;
    unsigned h = boot_render_asm_of[index] - 1u;
    if (!boot_render_asm[h].on || boot_render_asm[h].index != index) return;
    BootRenderRtTable *T = boot_render_image_table(r); if (!T) return; unsigned stale = 0;
    for (unsigned q = 0; q < boot_render_asm[h].n; ++q) {
        unsigned i = boot_render_asm[h].k[q];
        if (i < T->count && T->info[i].stale && boot_image_trace_overlap(T->rts[i].address, boot_render_rt_extent(&T->rts[i], &T->info[i]))) ++stale;
    }
    if (!stale) return;
    static uint64_t last_exec[BOOT_RENDER_ASM_POOL], last_epoch[BOOT_RENDER_ASM_POOL];
    uint64_t epoch = boot_image_trace_epoch();
    if (last_exec[h] == boot_render_exec_id && last_epoch[h] == epoch) return;
    last_exec[h] = boot_render_exec_id; last_epoch[h] = epoch;
    for (unsigned q = 0; q < boot_render_asm[h].n; ++q) {
        unsigned i = boot_render_asm[h].k[q];
        if (i >= T->count || !T->info[i].stale) continue;
        BootImageTraceRecord e = {0}; e.kind = BOOT_IMAGE_ASM_SOURCE;
        uint64_t extent = boot_render_rt_extent(&T->rts[i], &T->info[i]);
        if (!boot_image_trace_overlap(T->rts[i].address, extent)) continue;
        e.v[0] = boot_render_exec_id; e.v[1] = index; e.v[2] = h; e.v[3] = q; e.v[4] = i;
        e.v[5] = T->rts[i].address; e.v[6] = extent; e.v[7] = (uintptr_t)T->rts[i].texture;
        e.v[8] = (uintptr_t)boot_render_asm[h].src[q]; e.v[9] = boot_render_asm[h].copied;
        e.v[10] = boot_render_rt_written_at[i]; e.v[11] = boot_render_rt_write_seq;
        e.v[12] = T->info[i].known | ((uint64_t)T->info[i].dirty << 8) | ((uint64_t)T->info[i].stale << 16);
        e.v[13] = epoch; e.v[14] = T->generation; boot_image_trace_push(&e);
    }
}
static inline int boot_render_assemble_copies(BootRender *r, BootRenderRtTable *T, const int *k, unsigned n, ID3D12Resource *tex,
                                              const D3D12_RESOURCE_DESC *d, int depth, uint32_t tic, uint64_t address, unsigned why) {
    if (!depth) {
        BootLight84Batch *probe = NULL;
        if (boot_light84_on() && n == 2) {
            BootLight84Source source[2];
            for (unsigned q = 0; q < 2; ++q) { unsigned i = (unsigned)k[q];
                source[q] = (BootLight84Source){(uintptr_t)T->rts[i].texture, T->rts[i].address, boot_render_rt_written_at[i], i, T->generation}; }
            probe = boot_light84_begin(r, r->cl, boot_render_exec_id, boot_render_watch_event, tic, address, tex, d, source, why);
        }
        for (unsigned q = 0; q < n; ++q) {
            BootRenderTarget *x = &T->rts[k[q]];
            if (!boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE)) { boot_light84_end(probe, 0); return 0; }
            boot_light84_copy(probe, r->cl, 0, q, x->texture, 0);
            D3D12_TEXTURE_COPY_LOCATION dl = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
            dl.SubresourceIndex = q;
            BOOT_GP_CTR("assembly", r->cl, &dl, 0, 0, 0, &sl, NULL);
            if (probe) {
                D3D12_RESOURCE_BARRIER snapshot = {0}; snapshot.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                snapshot.Transition.pResource = tex; snapshot.Transition.Subresource = q;
                snapshot.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; snapshot.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                BOOT_GP_RB("assembly probe", r->cl, 1, &snapshot);
                boot_light84_copy(probe, r->cl, 1, q, tex, q);
                snapshot.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; snapshot.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                BOOT_GP_RB("assembly probe", r->cl, 1, &snapshot);
            }
        }
        boot_light84_end(probe, 1);
        return 1;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[64]; UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(r->gpu->device, d, 0, n, 0, fp, NULL, NULL, &total);
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    if (boot_render_reint_bytes < total) {   /* the staging buffer shared with boot_render_texture_from_target (COPY_DEST between uses) */
        if (boot_render_reint_buf) boot_render_release_later(boot_render_reint_buf);
        boot_render_reint_buf = NULL; boot_render_reint_bytes = 0;
        D3D12_RESOURCE_DESC bd = {0}; uint64_t want_bytes = total < (UINT64_C(16) << 20) ? (UINT64_C(16) << 20) : total;
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = want_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&boot_render_reint_buf))) {
            boot_render_reint_buf = NULL; return 0; }
        boot_render_reint_bytes = want_bytes;
    }
    for (unsigned q = 0; q < n; ++q) {
        BootRenderTarget *x = &T->rts[k[q]];
        D3D12_RESOURCE_DESC xd; x->texture->lpVtbl->GetDesc(x->texture, &xd);
        if (!boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE)) return 0;
        D3D12_TEXTURE_COPY_LOCATION bl = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        bl.PlacedFootprint = fp[q]; bl.PlacedFootprint.Footprint.Format = xd.Format;   /* the depth plane's typeless format */
        BOOT_GP_CTR("assembly reinterpret", r->cl, &bl, 0, 0, 0, &sl, NULL);
    }
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.pResource = boot_render_reint_buf;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    BOOT_GP_RB("assembly reinterpret", r->cl, 1, &b);
    for (unsigned q = 0; q < n; ++q) {
        D3D12_TEXTURE_COPY_LOCATION dl = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, bl = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
        dl.SubresourceIndex = q; bl.PlacedFootprint = fp[q];
        BOOT_GP_CTR("assembly reinterpret", r->cl, &dl, 0, 0, 0, &bl, NULL);
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    BOOT_GP_RB("assembly reinterpret", r->cl, 1, &b);
    return 1;
}
static inline int boot_render_texture_mips_from_targets(BootRender *r, BootRenderTexSlot *slot, const BootTic *t, const BootTexFormat *f,
                                                        const uint32_t words[8], uint32_t index, unsigned dim, uint32_t layers) {
    if (boot_render_tex_from_rt_off || t->pitch || t->base_layer || t->view_min_mip || f->decode || f->block_w != 1 || f->block_h != 1) return -1;
    if (!boot_render_ab_env("TOTK_TEX_ASSEMBLE", &boot_render_ab_assemble)) return -1;
    unsigned levels = t->max_mip + 1; if (levels > 16) levels = 16;
    {   uint32_t extent = t->width > t->height ? t->width : t->height;
        unsigned possible = 1; while (possible < 16 && (extent >> possible)) ++possible;
        if (levels > possible) levels = possible; }
    if (dim == BOOT_DIM_2D) layers = 1; else if (dim != BOOT_DIM_2D_ARRAY || !layers) return -1;
    if (levels < 2 && layers < 2) return -1;
    if (layers * levels > 64) return -1;
    const unsigned n = layers * levels;   /* subresource order: level + layer * levels (D3D12CalcSubresource) */
    DXGI_FORMAT want = (t->srgb && f->srgb != DXGI_FORMAT_UNKNOWN) ? f->srgb : f->unorm;
    BootTexLevel lv[16]; BootTic lt = *t; lt.texture_type = dim == BOOT_DIM_2D ? BOOT_TEX_2D : BOOT_TEX_2D_ARRAY;
    uint64_t stride = boot_tex_layout(&lt, f, lv, levels);
    if (!stride) return -1;
    BootRenderRtTable *T = boot_render_rtab(r); int k[64], depth = -1;
    for (unsigned s = 0; s < n; ++s) {
        unsigned l = s % levels, L = s / levels; uint64_t at = t->address + (uint64_t)L * stride + lv[l].offset;
        k[s] = -1;
        for (unsigned i = 0; i < T->count; ++i) {
            const BootRenderTarget *x = &T->rts[i];
            int is_depth = (x->format & 0x10000u) != 0;   /* a single-plane depth target is read through its colour view (R16_UNORM / R32_FLOAT) */
            if (!x->texture || (x->format & BOOT_RENDER_PITCH_FLAG) || x->address != at || x->width != lv[l].width || x->height != lv[l].height ||
                (is_depth ? boot_render_depth_srv_format(x->dxgi) : x->dxgi) != want) continue;
            if (is_depth && boot_render_no_depth_alias) continue;
            if (k[s] < 0 || T->info[i].last_use >= T->info[k[s]].last_use) k[s] = (int)i;
        }
        if (k[s] < 0) {   /* render-13 diagnostic: why a 2D array was not assembled (first 32 addresses) */
            static uint64_t seen[32]; static unsigned nseen; int known = 0;
            for (unsigned q = 0; q < nseen; ++q) known |= seen[q] == t->address;
            if (dim == BOOT_DIM_2D_ARRAY && !known && nseen < 32) {
                seen[nseen++] = t->address;
                FILE *lf = fopen("draw-path.log", "a");
                if (lf) {
                    fprintf(lf, "texture TIC %u: array %ux%ux%u levels %u dxgi %d at %llx not assembled: no target for layer %u level %u at %llx %ux%u; targets there:",
                            index, t->width, t->height, layers, levels, (int)want, (unsigned long long)t->address, L, l, (unsigned long long)at, lv[l].width, lv[l].height);
                    for (unsigned i = 0; i < T->count; ++i) if (T->rts[i].texture && T->rts[i].address >= t->address && T->rts[i].address < t->address + (uint64_t)layers * stride)
                        fprintf(lf, " [%llx %ux%u dxgi %d fmt %x]", (unsigned long long)T->rts[i].address, T->rts[i].width, T->rts[i].height, (int)T->rts[i].dxgi, T->rts[i].format);
                    fputc('\n', lf); fclose(lf);
                }
            }
            return -1;
        }
        int is_depth = (T->rts[k[s]].format & 0x10000u) != 0;
        if (depth >= 0 && depth != is_depth) return -1;   /* one kind of target per texture */
        depth = is_depth;
    }
    /* Rebuild of the same image (a level target was drawn again: boot_render_rt_copies_written flipped bit 31 of
     * words[0]; the TIC is otherwise unchanged): the slot's texture and views stay, and the levels are copied into
     * it again in queue order. Draws recorded before read the previous contents (they run before the copies), so
     * nothing waits and no descriptor is rewritten (play-12: 10 flushes per submission for the bloom chain). */
    unsigned why = !slot->valid ? 1 : !slot->res ? 2 : slot->dim != dim ? 3 : slot->size != 0 ? 4 : slot->va != t->address ? 5 :
                   (((slot->words[0] ^ words[0]) & 0x7fffffffu) || memcmp(slot->words + 1, words + 1, 28)) ? 6 : 0;   /* render-27 */
    int same = slot->valid && slot->res && slot->dim == dim && slot->size == 0 && slot->va == t->address &&
               ((slot->words[0] ^ words[0]) & 0x7fffffffu) == 0 && !memcmp(slot->words + 1, words + 1, 28);
    if (same) {
        D3D12_RESOURCE_DESC pd; slot->res->lpVtbl->GetDesc(slot->res, &pd);
        same = pd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && pd.Width == t->width && pd.Height == t->height && pd.DepthOrArraySize == layers &&
               pd.MipLevels == levels && pd.Format == want && boot_render_res_refs(slot->res) == 1;   /* no other slot shares the image */
    }
    if (!same && !why) why = 7;
    for (unsigned i = 0; same && i < T->count; ++i) same = T->rts[i].texture != slot->res;   /* never a target itself */
    if (!same && !why) why = 8;
    /* as the other paths: a changed slot may still be referenced by recorded draws */
    if (!same && slot->valid && !boot_render_slot_free(r, (uint32_t)(slot - boot_render_tex_slots))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush");
    for (unsigned q = 0; q < n; ++q) { int rc = boot_render_alias_use(r, T, (unsigned)k[q], 1); if (rc) return rc; }   /* current contents */
    if (same) {
        if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
        const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = slot->res; b.Transition.StateBefore = srv; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("assembly", r->cl, 1, &b);
        {   D3D12_RESOURCE_DESC sd; slot->res->lpVtbl->GetDesc(slot->res, &sd);
            if (!boot_render_assemble_copies(r, T, k, n, slot->res, &sd, depth, index, t->address, why)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target copies (in place)"); }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = srv;
        BOOT_GP_RB("assembly", r->cl, 1, &b);
        memcpy(slot->words, words, 32); boot_render_tex_mark(index);
        for (unsigned q = 0; q < boot_render_rt_copy_count; ++q)   /* this index's remaining registrations: re-added below */
            if (boot_render_rt_copies[q].index == index) { boot_render_rt_copies[q] = boot_render_rt_copies[--boot_render_rt_copy_count]; --q; }
        for (unsigned l = 0; l < n; ++l) {
            if (boot_render_rt_copy_count == BOOT_RENDER_RT_COPIES) {
                unsigned q = boot_render_rt_copy_next % BOOT_RENDER_RT_COPIES; uint32_t i = boot_render_rt_copies[q].index;
                if (boot_render_tex_slots[i].valid) boot_render_tex_poison(i);
                boot_render_rt_copies[q] = boot_render_rt_copies[--boot_render_rt_copy_count];
            }
            boot_render_rt_copies[boot_render_rt_copy_count].rt = T->rts[k[l]].texture; boot_render_rt_copies[boot_render_rt_copy_count++].index = index; ++boot_render_rt_copy_next; boot_render_rt_copy_note(T->rts[k[l]].texture);
        }
        ++boot_render_tex_mips_in_place;
        boot_render_asm_record(T, index, k, n);
        boot_render_title_assembly(r, index, BOOT_TITLE_ASSEMBLY, why, layers, levels);
        return BOOT_RENDER_OK;
    }
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = t->width; d.Height = t->height; d.DepthOrArraySize = (UINT16)layers; d.MipLevels = (UINT16)levels;
    d.Format = want; d.SampleDesc.Count = 1;
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource *previous = slot->res, *tex = NULL; slot->res = NULL; slot->valid = 0;
    if (!boot_render_begin(r)) { if (previous) boot_render_tex_pool_put(previous); return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset"); }
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (previous) {
        D3D12_RESOURCE_DESC pd; previous->lpVtbl->GetDesc(previous, &pd);
        if (pd.Dimension == d.Dimension && pd.Width == d.Width && pd.Height == d.Height && pd.DepthOrArraySize == d.DepthOrArraySize && pd.MipLevels == d.MipLevels &&
            pd.Format == d.Format && pd.Flags == d.Flags && boot_render_res_refs(previous) == 1) { tex = previous; previous = NULL; ++boot_render_tex_reused; }
        else { boot_render_tex_pool_put(previous); previous = NULL; }
    }
    if (!tex && (tex = boot_render_tex_pool_take(&d)) != NULL) ++boot_render_tex_pooled;
    if (tex) { b.Transition.pResource = tex; b.Transition.StateBefore = srv; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; BOOT_GP_RB("assembly", r->cl, 1, &b); }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    if (!tex && FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex)))
        return boot_render_fail(r, BOOT_RENDER_E_D3D, "texture resource (mips from targets)");
    if (!boot_render_assemble_copies(r, T, k, n, tex, &d, depth, index, t->address, why)) { boot_render_release_later(tex); return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target copies"); }
    b.Transition.pResource = tex; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = srv;
    BOOT_GP_RB("assembly", r->cl, 1, &b);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = want; sv.Shader4ComponentMapping = boot_render_swizzle(t);
    if (dim == BOOT_DIM_2D) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = levels; }
    else { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sv.Texture2DArray.MipLevels = levels; sv.Texture2DArray.ArraySize = layers; }
    ID3D12Device_CreateShaderResourceView(r->gpu->device, tex, &sv, boot_render_tex_cpu(r, dim, index));
    boot_render_tex_isolate_srv(r, index, dim, tex, &sv);
    memcpy(slot->words, words, 32); slot->res = tex; slot->valid = 1; slot->dim = dim; boot_render_tex_mark(index);
    slot->va = t->address; slot->size = 0;   /* contents come from targets, not guest bytes: no verify */
    for (unsigned l = 0; l < n; ++l) {
        if (boot_render_rt_copy_count == BOOT_RENDER_RT_COPIES) {   /* full: the oldest entry's slot is rebuilt on its next use */
            unsigned q = boot_render_rt_copy_next % BOOT_RENDER_RT_COPIES; uint32_t i = boot_render_rt_copies[q].index;
            if (boot_render_tex_slots[i].valid) boot_render_tex_poison(i);
            boot_render_rt_copies[q] = boot_render_rt_copies[--boot_render_rt_copy_count];
        }
        boot_render_rt_copies[boot_render_rt_copy_count].rt = T->rts[k[l]].texture; boot_render_rt_copies[boot_render_rt_copy_count++].index = index; ++boot_render_rt_copy_next; boot_render_rt_copy_note(T->rts[k[l]].texture);
    }
    boot_render_asm_record(T, index, k, n);
    boot_render_title_assembly(r, index, BOOT_TITLE_ASSEMBLY, why, layers, levels);
    static uint64_t assembled_at[64]; static unsigned assembled_n; int first = 1;   /* render-13: once per address, not only the first 8 */
    for (unsigned q = 0; q < assembled_n; ++q) first &= assembled_at[q] != t->address;
    if (first && assembled_n < 64) assembled_at[assembled_n++] = t->address;
    ++boot_render_tex_mips_from_rt;
    {   unsigned j = (unsigned)(boot_render_build_n++ & 255);   /* render-27 */
        boot_render_build_ring[j].exec = boot_render_exec_id; boot_render_build_ring[j].address = t->address; boot_render_build_ring[j].seq = boot_render_rt_write_seq;
        boot_render_build_ring[j].index = index; boot_render_build_ring[j].width = t->width; boot_render_build_ring[j].height = t->height;
        boot_render_build_ring[j].layers = layers; boot_render_build_ring[j].levels = levels; boot_render_build_ring[j].why = why; boot_render_build_ring[j].n = n;
        for (unsigned q = 0; q < 4; ++q) {
            boot_render_build_ring[j].src[q] = q < n ? T->rts[k[q]].address : 0;
            boot_render_build_ring[j].srcw[q] = q < n ? T->rts[k[q]].width : 0; boot_render_build_ring[j].srch[q] = q < n ? T->rts[k[q]].height : 0;
            boot_render_build_ring[j].srcseq[q] = q < n ? boot_render_rt_written_at[k[q]] : 0;
        }
    }
    if (first) {
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "texture TIC %u: %u mips x %u layers assembled from render targets at %llx %ux%u\n", index, levels, layers, (unsigned long long)t->address, t->width, t->height); fclose(lf); }
    }
    if (depth) {
        static uint32_t logged_tic[64]; static unsigned logged_n; int seen = 0;
        for (unsigned q = 0; q < logged_n; ++q) seen |= logged_tic[q] == index;
        if (!seen && logged_n < 64) {
            logged_tic[logged_n++] = index;
            FILE *lf = fopen("draw-path.log", "a");
            if (lf) { fprintf(lf, "texture TIC %u: depth array assembled (%u mips x %u layers) from depth targets at %llx %ux%u\n", index, levels, layers, (unsigned long long)t->address, t->width, t->height); fclose(lf); }
        }
        ++boot_render_tex_depth_assembled;
    }
    ++boot_render_tex_created[dim];
    return BOOT_RENDER_OK;
}
/* wb-02: multi-subresource textures (3D volumes, cubemaps, cube arrays, 2D arrays, mip chains) over dirty targets,
 * assembled on the GPU in queue order instead of the guest path's write-back + wait (play-74..79: ~419 write-back
 * submissions per 100 at the texture site, ~6 targets each; the logged ones were TOTK's 3D volumes drawn slice by slice
 * (64x64x64 / 32x32x32 R8 at 639e0000.., 8x8x8 R11G11B10 LUTs at 95600000..) and its cubemaps drawn face by face and
 * mip by mip (64x64 R11G11B10 at 57380000 / 58f20000, 16x16 R16F at 59000000)).
 * The rule, the guest path's: boot_render_flush_targets writes W = boot_render_rt_latest_over(texture range) back in
 * draw order (the dirty targets, plus every clean one drawn after the oldest of them that overlaps one of them), each
 * over its own footprint, and the guest decode then reads each subresource from the bytes the last of them left there.
 * A unit of the texture is one (level, layer) subresource of a 1D-free layered/mipped texture, or one z-slice of a 3D
 * texture's level 0 (the only 3D subresources the native path renders into: host_nvn_exec.h
 * boot_nvn_texture_target_obj, slice z at base + (z >> bd) * slab + (z & (2^bd - 1)) * 512 * 2^bh; levels > 0 are
 * whole units). A target maps onto a unit when it starts at the unit's address with the unit's extent (width, height),
 * texel size and block layout (host_texture_layout.h: the same GOB math boot_texture_detile / boot_texture_detile_3d
 * decode with). Distinct units never share a byte (layers and levels are disjoint ranges; 3D slices interleave inside
 * GOB blocks but each slice owns its own GOBs: boot_texture_tile_slice writes only those), so for each unit the image
 * the write-back would leave is the newest member of W that maps onto it, unless a member that maps onto no unit and
 * overlaps it is newer (then: guest path). Every dirty member must map onto a unit. Units no member of W touches keep
 * their guest bytes (nothing writes them back): they are uploaded from guest memory as the guest path does, and every
 * other unit is a GPU copy of its target (CopyTextureRegion into subresource level + layer * levels, DstZ = slice for
 * 3D; through the reinterpretation buffer when the formats differ (same texel size, the bits the hardware reads) or the
 * destination is 3D). The slot records the dirty members of W as an assembly (boot_render_asm_record, render-14/15): a
 * draw into any of them makes its next use rebuild it in place (same resource and descriptors, queue order, no wait); a
 * clean target over it that becomes dirty invalidates it through its guest range (slot size = the texture's bytes). */
enum { BOOT_RT_SUB_SHAPE, BOOT_RT_SUB_COMPRESSED, BOOT_RT_SUB_DEPTH, BOOT_RT_SUB_3D_SLICE, BOOT_RT_SUB_PARTIAL, BOOT_RT_SUB_TEXEL,
       BOOT_RT_SUB_LAYOUT, BOOT_RT_SUB_OTHER, BOOT_RT_SUB_STALE, BOOT_RT_SUB_MANY, BOOT_RT_SUBS };
static uint64_t boot_render_tex_sub_declines[BOOT_RT_SUBS];
static uint64_t boot_render_tex_sub_built, boot_render_tex_sub_copies, boot_render_tex_sub_in_place, boot_render_tex_sub_guest;
#define BOOT_RENDER_SUB_UNITS 256
typedef struct BootRenderSubUnit { uint64_t va, size; uint32_t sub, level, z, slice; } BootRenderSubUnit;
static inline int boot_render_tex_sub_decline(unsigned why, const BootRenderRtTable *T, const BootTic *t, uint32_t index, unsigned dim, unsigned levels,
                                              uint32_t layers, unsigned n, int k, const char *detail) {
    static const char *const name[BOOT_RT_SUBS] = {
        "1D, pitch-linear or more than 256 subresources/slices", "compressed format", "a depth target over it",
        "a 3D target that is not one level-0 slice", "a target not covering exactly one subresource (partial mip / other extent)",
        "a target of another texel size", "a target of another block layout", "the newest target over a subresource is another image",
        "the newest target over a subresource is stale", "more than 64 targets over it" };
    ++boot_render_tex_sub_declines[why]; ++boot_render_tex_rt_declines[BOOT_RT_DECLINE_SHAPE];
    static struct { uint64_t address; unsigned why; } seen[64]; static unsigned m;
    for (unsigned i = 0; i < m; ++i) if (seen[i].address == t->address && seen[i].why == why) return -1;
    if (m == 64) return -1;
    seen[m].address = t->address; seen[m].why = why; ++m;
    FILE *lf = fopen("draw-path.log", "a"); if (!lf) return -1;
    fprintf(lf, "texture over dirty targets not assembled (guest path: write-back and wait): tic %u at %llx %ux%ux%u fmt %02x type %u dim %u levels %u layers %u block h/d %u/%u: %s; %u targets over it",
            index, (unsigned long long)t->address, t->width, t->height, t->depth, t->components, t->data_type[0], dim, levels, layers, t->log_gob_h, t->log_gob_d,
            name[why], n);
    if (k >= 0 && (unsigned)k < T->count) {
        const BootRenderTarget *x = &T->rts[k]; const BootRenderRtInfo *in = &T->info[k];
        fprintf(lf, ", target #%d at %llx %ux%u guest fmt %x dxgi %d block h/d %u/%u%s%s", k, (unsigned long long)x->address, x->width, x->height, x->format,
                (int)x->dxgi, (unsigned)in->log_gob_h, (unsigned)in->log_gob_d, in->dirty ? "" : " (clean)", in->stale ? " (stale)" : "");
    }
    if (detail && *detail) fprintf(lf, " (%s)", detail);
    fputc('\n', lf); fclose(lf);
    return -1;
}
/* Two block-linear layouts of a width_bytes x rows image put every byte at the same offset: the same block height and
 * depth, or one GOB column within one block row of both (byte (x, y) at ((y >> 3) << 9) + its GOB offset in each). */
static inline int boot_render_bl_equivalent(uint32_t width_bytes, uint32_t rows, unsigned h1, unsigned d1, unsigned h2, unsigned d2) {
    if (h1 == h2 && d1 == d2) return 1;
    unsigned h = h1 < h2 ? h1 : h2;
    return width_bytes <= 64 && rows <= (8u << h);
}
static inline int boot_render_texture_subres(BootRender *r, BootRenderTexSlot *slot, const BootTic *t, const BootTexFormat *f, const uint32_t words[8],
                                             uint32_t index, unsigned dim, unsigned levels, uint64_t stride, uint32_t layers, uint64_t data_va,
                                             const unsigned *list, unsigned n) {
    BootRenderRtTable *T = boot_render_rtab(r);
    char detail[160]; detail[0] = 0;
    if (boot_render_rt_overlap_reload()) return boot_render_tex_rt_decline(BOOT_RT_DECLINE_SEVERAL, T, t, index, dim, levels, layers, n, -1);
    /* TOTK_ASM_REFRESH=0 (A/B): boot_render_asm_stale never reports a redraw, and these slots would keep their first
     * copies: the guest path (write-back), as before wb-02 */
    if (!boot_render_ab_env("TOTK_ASM_REFRESH", &boot_render_ab_refresh)) return -1;
    if (dim == BOOT_DIM_1D || dim == BOOT_DIM_1D_ARRAY || dim == BOOT_DIM_BUFFER || t->pitch || !layers)
        return boot_render_tex_sub_decline(BOOT_RT_SUB_SHAPE, T, t, index, dim, levels, layers, n, -1, "");
    if (f->decode || f->block_w != 1 || f->block_h != 1) return boot_render_tex_sub_decline(BOOT_RT_SUB_COMPRESSED, T, t, index, dim, levels, layers, n, -1, "");
    /* the guest path's layout (boot_render_texture: lt, lv, stride) */
    BootTic lt = *t; int layered = t->depth > 1 || t->base_layer != 0;
    if (dim == BOOT_DIM_2D && layered) lt.texture_type = BOOT_TEX_2D_ARRAY;
    else if (dim == BOOT_DIM_CUBE && layered) lt.texture_type = BOOT_TEX_CUBE_ARRAY;
    BootTexLevel lv[16];
    if (!levels || levels > 16 || boot_tex_layout(&lt, f, lv, levels) != stride) {
        snprintf(detail, sizeof detail, "layer stride %llx", (unsigned long long)stride);
        return boot_render_tex_sub_decline(BOOT_RT_SUB_LAYOUT, T, t, index, dim, levels, layers, n, -1, detail);
    }
    const int vol = dim == BOOT_DIM_3D;
    const DXGI_FORMAT want = (t->srgb && f->srgb != DXGI_FORMAT_UNKNOWN) ? f->srgb : f->unorm;
    const unsigned images = vol ? 1 : layers;
    const uint64_t total = stride * layers;
    /* units: (level, layer) subresources; 3D: the z-slices of level 0, then the other levels whole */
    BootRenderSubUnit u[BOOT_RENDER_SUB_UNITS]; unsigned nu = 0;
    const unsigned bh = lv[0].log_gob_h, bd = lv[0].log_gob_d;
    const uint64_t slab = (uint64_t)boot_div_up(lv[0].width_bytes, 64) * boot_div_up(lv[0].rows, 8u << bh) * (UINT64_C(512) << (bh + bd));
    for (unsigned L = 0; L < images; ++L)
        for (unsigned l = 0; l < levels; ++l) {
            unsigned count = vol && l == 0 ? lv[0].depth : 1;
            if (nu + count > BOOT_RENDER_SUB_UNITS) return boot_render_tex_sub_decline(BOOT_RT_SUB_SHAPE, T, t, index, dim, levels, layers, n, -1, "");
            for (unsigned z = 0; z < count; ++z) {
                BootRenderSubUnit *e = &u[nu++];
                e->sub = l + L * levels; e->level = l; e->z = z; e->slice = vol && l == 0;
                if (e->slice) {
                    e->va = data_va + (uint64_t)(z >> bd) * slab + ((uint64_t)(z & ((1u << bd) - 1)) << (9 + bh));
                    e->size = boot_texture_slice_extent(lv[0].width_bytes, lv[0].rows, bh, bd);
                } else { e->va = data_va + (uint64_t)L * stride + lv[l].offset; e->size = lv[l].size; }
            }
        }
    /* W: boot_render_rt_latest_over without its side effect (it marks the clean members dirty for the write-back).
     * Its exact closure (wb-02 r2): `oldest` and the overlap test take every entry of its growing list, the dirty
     * targets the write-back then skips (pitch-linear, multi-plane depth) and the clean ones appended before (lower
     * table index), so a clean target overlapping only an appended one is in W too. Members are the tiled entries;
     * w starts with the dirty ones (the assembly record's sources). */
    unsigned w[BOOT_RENDER_RT_CAP], nw = 0, n_all = n; uint64_t oldest = UINT64_MAX;
    for (unsigned q = 0; q < n; ++q) {
        if (boot_render_rt_written_at[list[q]] < oldest) oldest = boot_render_rt_written_at[list[q]];
        if (!boot_render_rt_untiled(&T->rts[list[q]])) w[nw++] = list[q];
    }
    if (!nw) return -1;   /* no tiled dirty target: the guest path, as before wb-02 */
    const unsigned dirty_n = nw;
    for (unsigned i = 0; i < T->count && n_all < BOOT_RENDER_RT_CAP; ++i) {
        const BootRenderTarget *x = &T->rts[i]; const BootRenderRtInfo *in = &T->info[i];
        if (in->dirty || !x->texture || !in->known || boot_render_rt_untiled(x) || boot_render_rt_written_at[i] <= oldest) continue;
        uint64_t extent = boot_render_rt_extent(x, in);
        if (!extent || !(x->address < data_va + total && data_va < x->address + extent)) continue;
        int hit = 0;
        for (unsigned q = 0; q < n + (nw - dirty_n) && !hit; ++q) {   /* latest_over's list: the dirty ones, then the appended */
            unsigned j = q < n ? list[q] : w[dirty_n + (q - n)];
            const BootRenderTarget *y = &T->rts[j]; uint64_t ye = boot_render_rt_extent(y, &T->info[j]);
            hit = y->address < x->address + extent && x->address < y->address + ye;
        }
        if (hit) { w[nw++] = i; ++n_all; }
    }
    if (dirty_n > 64) return boot_render_tex_sub_decline(BOOT_RT_SUB_MANY, T, t, index, dim, levels, layers, nw, -1, "");   /* the assembly record's capacity */
    /* each member onto its unit (-1: none); a dirty member that maps onto none: guest path */
    static int map[BOOT_RENDER_RT_CAP];
    for (unsigned q = 0; q < nw; ++q) {
        unsigned i = w[q]; const BootRenderTarget *x = &T->rts[i]; const BootRenderRtInfo *in = &T->info[i];
        int dirty = q < dirty_n; unsigned why = BOOT_RT_SUBS; int e = -1;
        map[q] = -1;
        for (unsigned v = 0; v < nu && e < 0; ++v) if (u[v].va == x->address) e = (int)v;
        if (x->format & 0x10000u) why = BOOT_RT_SUB_DEPTH;
        else if (e >= 0 && vol && !u[e].slice && lv[u[e].level].depth > 1) {
            /* wb-02 r2: a 2D target at the start of a whole 3D level > 0 of depth > 1 would be copied into its z = 0
             * only (and the level's guest upload skipped): not one slice */
            why = BOOT_RT_SUB_3D_SLICE;
            if (dirty) snprintf(detail, sizeof detail, "at %llx, the start of 3D level %u (depth %u)", (unsigned long long)x->address, u[e].level, lv[u[e].level].depth);
        } else if (e < 0) {
            why = vol && x->address >= data_va && x->address < data_va + lv[0].size ? BOOT_RT_SUB_3D_SLICE : BOOT_RT_SUB_PARTIAL;
            if (dirty) snprintf(detail, sizeof detail, "at %llx, no subresource or slice starts there", (unsigned long long)x->address);
        } else {
            const BootRenderSubUnit *s = &u[e]; const BootTexLevel *v = &lv[s->level];
            D3D12_RESOURCE_DESC xd; x->texture->lpVtbl->GetDesc(x->texture, &xd);
            if (boot_render_dxgi_bytes(x->dxgi) != f->bytes) {
                why = BOOT_RT_SUB_TEXEL;
                if (dirty) snprintf(detail, sizeof detail, "%u bytes per texel, the texture %u", boot_render_dxgi_bytes(x->dxgi), (unsigned)f->bytes);
            } else if (x->width != v->width || x->height != v->height) {
                why = BOOT_RT_SUB_PARTIAL;
                if (dirty) snprintf(detail, sizeof detail, "%ux%u at level %u layer %u, which is %ux%u", x->width, x->height, s->level, s->sub / levels, v->width, v->height);
            } else if (s->slice ? (in->log_gob_h != bh || in->log_gob_d != bd)
                                : !boot_render_bl_equivalent(v->width_bytes, v->rows, in->log_gob_h, in->log_gob_d, v->log_gob_h, v->log_gob_d)) {
                why = BOOT_RT_SUB_LAYOUT;
                if (dirty) snprintf(detail, sizeof detail, "level %u %s %u has block h/d %u/%u", s->level, s->slice ? "slice" : "layer", s->slice ? s->z : s->sub / levels,
                                    s->slice ? bh : v->log_gob_h, s->slice ? bd : v->log_gob_d);
            } else if (xd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || xd.MipLevels != 1 || xd.DepthOrArraySize != 1) {
                why = BOOT_RT_SUB_LAYOUT;
                if (dirty) snprintf(detail, sizeof detail, "target resource %u levels x %u layers", (unsigned)xd.MipLevels, (unsigned)xd.DepthOrArraySize);
            } else map[q] = e;
        }
        if (map[q] < 0 && dirty) return boot_render_tex_sub_decline(why, T, t, index, dim, levels, layers, nw, (int)i, detail);
    }
    /* per unit, the member the write-back would leave last over it (draw order; ties: the later member, as its stable sort) */
    int src[BOOT_RENDER_SUB_UNITS]; unsigned sources = 0, copies_via_buffer = 0; uint64_t buffer_bytes = 0;
    for (unsigned v = 0; v < nu; ++v) {
        int best = -1; uint64_t bw = 0;
        for (unsigned q = 0; q < nw; ++q) {
            if (map[q] >= 0 && map[q] != (int)v) continue;   /* another unit: no byte in common */
            if (map[q] < 0) {
                const BootRenderTarget *x = &T->rts[w[q]]; uint64_t extent = boot_render_rt_extent(x, &T->info[w[q]]);
                if (!(x->address < u[v].va + u[v].size && u[v].va < x->address + extent)) continue;
            }
            uint64_t at = boot_render_rt_written_at[w[q]];
            if (best < 0 || at >= bw) { best = (int)q; bw = at; }
        }
        src[v] = -1;
        if (best < 0) continue;
        if (map[best] < 0) {
            snprintf(detail, sizeof detail, "level %u %s %u", u[v].level, u[v].slice ? "slice" : "layer", u[v].slice ? u[v].z : u[v].sub / levels);
            return boot_render_tex_sub_decline(BOOT_RT_SUB_OTHER, T, t, index, dim, levels, layers, nw, (int)w[best], detail);
        }
        if (T->info[w[best]].stale) return boot_render_tex_sub_decline(BOOT_RT_SUB_STALE, T, t, index, dim, levels, layers, nw, (int)w[best], "");
        src[v] = best; ++sources;
        const BootRenderTarget *x = &T->rts[w[best]];
        if (vol || x->dxgi != want) {
            uint64_t rp = ((uint64_t)x->width * f->bytes + 255) & ~UINT64_C(255);
            buffer_bytes = ((buffer_bytes + 511) & ~UINT64_C(511)) + rp * x->height; ++copies_via_buffer;
        }
    }
    if (!sources) return -1;   /* no unit takes a target's image (not reached: every dirty member maps onto a unit) */
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = vol ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = t->width; d.Height = t->height; d.DepthOrArraySize = (UINT16)(vol ? t->depth : layers); d.MipLevels = (UINT16)levels; d.Format = want; d.SampleDesc.Count = 1;
    if (!boot_render_cpu_read(r, data_va, total)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending GPU writes");
    const uint8_t *src_bytes = boot_render_guest(r, data_va, total);
    if (!src_bytes) return -1;   /* the guest path reports it */
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    /* the same image again (a source was drawn since: boot_render_asm_stale, or a clean member became dirty: poisoned):
     * copied into the slot's own resource in queue order, descriptors kept (draws recorded before read the old image) */
    int same = slot->valid && slot->res && !slot->view_of && slot->dim == dim && slot->va == data_va && slot->size == total &&
               ((slot->words[0] ^ words[0]) & 0x7fffffffu) == 0 && !memcmp(slot->words + 1, words + 1, 28);
    if (same) {
        D3D12_RESOURCE_DESC pd; slot->res->lpVtbl->GetDesc(slot->res, &pd);
        same = pd.Dimension == d.Dimension && pd.Width == d.Width && pd.Height == d.Height && pd.DepthOrArraySize == d.DepthOrArraySize &&
               pd.MipLevels == d.MipLevels && pd.Format == d.Format && pd.Flags == d.Flags && boot_render_res_refs(slot->res) == 1;
    }
    for (unsigned i = 0; same && i < T->count; ++i) same = T->rts[i].texture != slot->res;
    ID3D12Resource *tex = NULL;
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (same) {
        if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
        tex = slot->res;
        b.Transition.pResource = tex; b.Transition.StateBefore = srv; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("wb02 subres", r->cl, 1, &b);
    } else {
        /* as the other paths: a changed slot may still be referenced by recorded draws */
        if (slot->valid && !boot_render_slot_free(r, (uint32_t)(slot - boot_render_tex_slots))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush");
        ID3D12Resource *previous = slot->res; slot->res = NULL; slot->valid = 0;
        if (!boot_render_begin(r)) { if (previous) boot_render_tex_pool_put(previous); return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset"); }
        if (previous) {
            D3D12_RESOURCE_DESC pd; previous->lpVtbl->GetDesc(previous, &pd);
            if (pd.Dimension == d.Dimension && pd.Width == d.Width && pd.Height == d.Height && pd.DepthOrArraySize == d.DepthOrArraySize && pd.MipLevels == d.MipLevels &&
                pd.Format == d.Format && pd.Flags == d.Flags && boot_render_res_refs(previous) == 1) { tex = previous; previous = NULL; ++boot_render_tex_reused; }
            else { boot_render_tex_pool_put(previous); previous = NULL; }
        }
        if (!tex && (tex = boot_render_tex_pool_take(&d)) != NULL) ++boot_render_tex_pooled;
        if (tex) { b.Transition.pResource = tex; b.Transition.StateBefore = srv; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; BOOT_GP_RB("wb02 subres", r->cl, 1, &b); }
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        if (!tex && FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex)))
            return boot_render_fail(r, BOOT_RENDER_E_D3D, "texture resource (subresources from targets)");
    }
    /* guest bytes of the units no target covers: whole subresources, and runs of 3D slices (never a byte a copy below writes) */
    unsigned first_unit = 0, guest_subs = 0; int ok = 1; const char *why = NULL;
    for (unsigned s = 0; ok && s < levels * images; ++s) {
        while (first_unit < nu && u[first_unit].sub != s) ++first_unit;
        if (first_unit == nu) break;
        const BootTexLevel *v = &lv[s % levels]; const unsigned L = s / levels;
        uint8_t *lin = NULL; size_t linear = (size_t)v->width_bytes * v->rows * v->depth;
        for (unsigned z0 = 0; ok && z0 < v->depth; ) {
            unsigned zn;
            if (u[first_unit].slice) {   /* 3D level 0: the next run of uncovered slices */
                while (z0 < v->depth && src[first_unit + z0] >= 0) ++z0;
                if (z0 == v->depth) break;
                zn = 1; while (z0 + zn < v->depth && src[first_unit + z0 + zn] < 0) ++zn;
            } else { if (src[first_unit] >= 0) break; zn = v->depth; }
            if (!lin) {
                const uint8_t *level_src = src_bytes + (uint64_t)L * stride + v->offset;
                if (!(lin = malloc(linear ? linear : 1))) { ok = 0; why = "memory"; break; }
                ok = vol ? boot_texture_detile_3d(lin, linear, level_src, (size_t)v->size, v->width_bytes, v->rows, v->depth, v->log_gob_h, v->log_gob_d)
                         : boot_texture_detile(lin, linear, level_src, (size_t)v->size, v->width_bytes, v->rows, v->log_gob_h);
                if (!ok) { why = "detile"; break; }
            }
            uint64_t rp = ((uint64_t)v->width_bytes + 255) & ~UINT64_C(255), bytes = rp * v->rows * zn;
            ID3D12Resource *upload = NULL; uint64_t at = 0; uint8_t *up = boot_render_upload_alloc(r, bytes, &upload, &at);
            if (!up || !boot_render_begin(r)) { ok = 0; why = "upload buffer"; break; }
            for (unsigned z = 0; z < zn; ++z)
                for (uint32_t y = 0; y < v->rows; ++y)
                    memcpy(up + ((size_t)z * v->rows + y) * rp, lin + ((size_t)(z0 + z) * v->rows + y) * v->width_bytes, v->width_bytes);
            D3D12_TEXTURE_COPY_LOCATION dl = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, ul = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
            dl.SubresourceIndex = s;
            ul.PlacedFootprint.Offset = at; ul.PlacedFootprint.Footprint.Format = want; ul.PlacedFootprint.Footprint.Width = v->width;
            ul.PlacedFootprint.Footprint.Height = v->height; ul.PlacedFootprint.Footprint.Depth = zn; ul.PlacedFootprint.Footprint.RowPitch = (UINT)rp;
            BOOT_GP_CTR("wb02 guest upload", r->cl, &dl, 0, 0, z0, &ul, NULL);
            ++guest_subs; z0 += zn;
        }
        free(lin);
    }
    /* the targets: straight into their subresource when the formats match (2D), else through the reinterpretation buffer
     * (default heap, COPY_DEST between uses: boot_render_texture_from_target, boot_render_assemble_copies) */
    if (ok && copies_via_buffer && boot_render_reint_bytes < buffer_bytes) {
        if (boot_render_reint_buf) boot_render_release_later(boot_render_reint_buf);
        boot_render_reint_buf = NULL; boot_render_reint_bytes = 0;
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_RESOURCE_DESC bd = {0}; uint64_t want_bytes = buffer_bytes < (UINT64_C(16) << 20) ? (UINT64_C(16) << 20) : buffer_bytes;
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = want_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&boot_render_reint_buf))) {
            boot_render_reint_buf = NULL; ok = 0; why = "reinterpretation buffer"; }
        else boot_render_reint_bytes = want_bytes;
    }
    uint64_t cursor = 0;
    for (unsigned pass = 0; ok && pass < 2; ++pass) {   /* pass 0: direct copies and target -> buffer; pass 1: buffer -> texture */
        if (pass == 1) {
            if (!copies_via_buffer) break;
            if (!boot_render_begin(r)) { ok = 0; why = "command list"; break; }
            D3D12_RESOURCE_BARRIER bb = b; bb.Transition.pResource = boot_render_reint_buf;
            bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; bb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            BOOT_GP_RB("wb02 reinterpret", r->cl, 1, &bb);
            cursor = 0;
        }
        for (unsigned v = 0; ok && v < nu; ++v) {
            if (src[v] < 0) continue;
            BootRenderTarget *x = &T->rts[w[src[v]]];
            int direct = !vol && x->dxgi == want;
            if (pass == 1 && direct) continue;
            D3D12_TEXTURE_COPY_LOCATION dl = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, xl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
            D3D12_TEXTURE_COPY_LOCATION bl = {boot_render_reint_buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
            dl.SubresourceIndex = u[v].sub;
            if (!direct) {
                uint64_t rp = ((uint64_t)x->width * f->bytes + 255) & ~UINT64_C(255);
                cursor = (cursor + 511) & ~UINT64_C(511);
                bl.PlacedFootprint.Offset = cursor; bl.PlacedFootprint.Footprint.Width = x->width; bl.PlacedFootprint.Footprint.Height = x->height;
                bl.PlacedFootprint.Footprint.Depth = 1; bl.PlacedFootprint.Footprint.RowPitch = (UINT)rp;
                cursor += rp * x->height;
            }
            if (pass == 1) {   /* the texture's format: the same bits */
                bl.PlacedFootprint.Footprint.Format = want;
                BOOT_GP_CTR("wb02 reinterpret", r->cl, &dl, 0, 0, u[v].slice ? u[v].z : 0, &bl, NULL);
                continue;
            }
            D3D12_RESOURCE_STATES was = x->state;
            if (!boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE)) { ok = 0; why = "render target state"; break; }
            if (direct) BOOT_GP_CTR("wb02 direct", r->cl, &dl, 0, 0, 0, &xl, NULL);
            else {
                D3D12_RESOURCE_DESC xd; x->texture->lpVtbl->GetDesc(x->texture, &xd);
                bl.PlacedFootprint.Footprint.Format = xd.Format;   /* the target resource's own format */
                BOOT_GP_CTR("wb02 reinterpret", r->cl, &bl, 0, 0, 0, &xl, NULL);
            }
            boot_render_rt_barrier(r, x, was);
            ++boot_render_tex_sub_copies;
        }
        if (ok && pass == 1) {
            D3D12_RESOURCE_BARRIER bb = b; bb.Transition.pResource = boot_render_reint_buf;
            bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; bb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            BOOT_GP_RB("wb02 reinterpret", r->cl, 1, &bb);
        }
    }
    if (ok && !boot_render_begin(r)) { ok = 0; why = "command list"; }
    if (!ok) {
        /* copies may be recorded: the resource is released after the list executes; the slot is rebuilt on its next use */
        if (same) { slot->res = NULL; slot->valid = 0; }
        boot_render_release_later(tex);
        snprintf(r->error, sizeof r->error, "texture from target subresources (%s): TIC %u type %u format %02x %ux%ux%u levels %u", why ? why : "?",
                 index, t->texture_type, t->components, t->width, t->height, t->depth, levels);
        return BOOT_RENDER_E_D3D;
    }
    b.Transition.pResource = tex; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = srv;
    BOOT_GP_RB("wb02 subres", r->cl, 1, &b);
    if (!same) {   /* the guest path's view (boot_render_texture, view:) */
        UINT first_mip = t->view_min_mip < levels ? t->view_min_mip : levels - 1;
        UINT view_mips = t->view_max_mip >= first_mip ? t->view_max_mip - first_mip + 1 : 1;
        if (first_mip + view_mips > levels) view_mips = levels - first_mip;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
        sv.Format = want; sv.Shader4ComponentMapping = boot_render_swizzle(t); sv.ViewDimension = boot_render_dim_srv[dim];
        switch (dim) {
            case BOOT_DIM_2D: sv.Texture2D.MostDetailedMip = first_mip; sv.Texture2D.MipLevels = view_mips; break;
            case BOOT_DIM_2D_ARRAY: sv.Texture2DArray.MostDetailedMip = first_mip; sv.Texture2DArray.MipLevels = view_mips; sv.Texture2DArray.ArraySize = layers; break;
            case BOOT_DIM_3D: sv.Texture3D.MostDetailedMip = first_mip; sv.Texture3D.MipLevels = view_mips; break;
            case BOOT_DIM_CUBE: sv.TextureCube.MostDetailedMip = first_mip; sv.TextureCube.MipLevels = view_mips; break;
            default: sv.TextureCubeArray.MostDetailedMip = first_mip; sv.TextureCubeArray.MipLevels = view_mips; sv.TextureCubeArray.NumCubes = layers / 6; break;
        }
        ID3D12Device_CreateShaderResourceView(r->gpu->device, tex, &sv, boot_render_tex_cpu(r, dim, index));
        boot_render_tex_isolate_srv(r, index, dim, tex, &sv);
        ++boot_render_tex_created[dim];
    } else ++boot_render_tex_sub_in_place;
    memcpy(slot->words, words, 32); slot->res = tex; slot->valid = 1; slot->dim = dim; slot->view_of = NULL; boot_render_tex_mark(index);
    boot_render_tex_subres[index] = 1;   /* after the mark (which clears it): the use check asks boot_render_asm_stale */
    slot->va = data_va; slot->size = total;   /* range invalidation (a clean target over it becoming dirty, write-backs, CPU / copy writes) */
    boot_render_tex_sum[index] = boot_render_tex_checksum(src_bytes, total);   /* guest bytes as they are (the verifier's reference) */
    {   uint32_t h = boot_render_tex_dedup_hash(words, dim);   /* never shared by the guest path's dedup: not guest bytes alone */
        if (boot_render_tex_dedup_map[h] == index + 1) boot_render_tex_dedup_map[h] = 0; }
    boot_render_rt_copies_forget(index);
    {   int k[64]; for (unsigned q = 0; q < dirty_n; ++q) k[q] = (int)w[q];   /* the dirty members (W starts with them): a draw into */
        boot_render_asm_record(T, index, k, dirty_n); }                        /* one leaves it dirty; a clean one becoming dirty poisons the range */
    ++boot_render_tex_sub_built; boot_render_tex_sub_guest += guest_subs;
    {   static uint64_t logged_at[64]; static unsigned logged_n; int first = 1;
        for (unsigned q = 0; q < logged_n; ++q) first &= logged_at[q] != t->address;
        if (first && logged_n < 64) {
            logged_at[logged_n++] = t->address;
            FILE *lf = fopen("draw-path.log", "a");
            if (lf) { fprintf(lf, "texture TIC %u: %ux%ux%u dim %u levels %u layers %u at %llx assembled on the GPU: %u of %u %s from targets (%u through the buffer), %u guest uploads, %u targets over it\n",
                              index, t->width, t->height, t->depth, dim, levels, layers, (unsigned long long)t->address, sources, nu, vol ? "slices/levels" : "subresources",
                              copies_via_buffer, guest_subs, nw); fclose(lf); }
        }
    }
    return BOOT_RENDER_OK;
}
/* render-39: guest CPU writes into textures the game keeps updating after their first use, seen when the texture
 * is next used instead of when the round-robin verifier (boot_render_tex_verify) reaches it. The font glyph cache
 * (R8 2048x4096 at 10fc0000, TIC 290: 8 MiB) gets new glyphs written by the CPU while text is on screen; with
 * ~64 MiB re-hashed per submission over all resident textures a new glyph waited for the cursor to come round
 * (letters appearing late, one by one), and was then a full 8 MiB rebuild that first waited for the GPU to
 * finish the previous use of the slot.
 * Detection: guest memory is pagefile-backed sections mapped into the fixed view (host_fastmem.h, MapViewOfFile3
 * into placeholders), so MEM_WRITE_WATCH / GetWriteWatch (LASRecomp, SuperMarioWonderRecomp) cannot be used: it
 * exists only for private VirtualAlloc memory (tested on this machine, Windows 11 26200: VirtualAlloc2 with
 * MEM_RESERVE_PLACEHOLDER | MEM_WRITE_WATCH and GetWriteWatch on a MapViewOfFile3 or MapViewOfFile section view
 * all fail with ERROR_INVALID_PARAMETER). Translated stores are plain host stores with no dirty tracking, and
 * TOTK writes the cache directly (nvnTextureWriteTexels: 0-3 calls per 100 submissions, already handled by
 * boot_render_cpu_texels_push). So a watched texture is hashed in bands at its first use in each submission (at
 * most once per submission) and the verifier skips it. Glyph caches (R8, 1 MiB and more) are checked at every
 * such use (8 MiB: ~0.3 ms, measured at 26 GB/s); the others share TOTK_TEX_WATCH_MB per submission (default 8,
 * 0 = the whole watch off) and stop being watched after 600 unchanged checks. Watched: single-level, single-layer
 * 2D uncompressed textures that are pitch-linear or R8 glyph caches, up to 16 MiB, and any texture up to 4 MiB
 * the verifier found rewritten. Memory the GPU writes (pending GPU writes over it, or render-25 ownership) is not
 * watched: its guest bytes are not the image. No check waits for the GPU.
 * Update: changed bands of a single-level uncompressed 2D texture (rows for pitch-linear, whole block rows for
 * block-linear) are copied into the slot's own resource in queue order (barrier, CopyTextureRegion into rows
 * y0..y1, barrier): no descriptor rewrite, no wait for earlier GPU work. Anything else is poisoned and rebuilt.
 * Technique credit: SuperMarioWonderRecomp (MIT), runtime/src/nvn/nvn_sampler.cpp check_linear_content (check
 * at use, once per submit) and nvn_upload.cpp (dirty-row partial re-upload of pitch-linear textures). */
#define BOOT_RENDER_TEX_WATCH 16
#define BOOT_RENDER_TEX_WATCH_BANDS 256
typedef struct BootRenderTexWatch {
    uint32_t index, on, seeded, always, inplace, pitch, width_bytes, rows, width, log_gob_h, rows_per_band, bands;
    DXGI_FORMAT fmt; ID3D12Resource *res; uint64_t va, size, band_bytes, exec, quiet;
    uint64_t sum[BOOT_RENDER_TEX_WATCH_BANDS];
} BootRenderTexWatch;
static BootRenderTexWatch boot_render_tex_watch[BOOT_RENDER_TEX_WATCH];
static uint8_t boot_render_tex_watch_want[BOOT_RENDER_TEX_SLOTS];   /* rewritten since its build, or watched before its rebuild */
static uint64_t boot_render_tex_watch_limit = UINT64_MAX, boot_render_tex_watch_budget, boot_render_tex_watch_exec_at, boot_render_tex_watch_exec_bytes;
static uint64_t boot_render_tex_watch_adds, boot_render_tex_watch_checks, boot_render_tex_watch_hashed, boot_render_tex_watch_refreshes,
                boot_render_tex_watch_rows, boot_render_tex_watch_uploaded, boot_render_tex_watch_rebuilds, boot_render_tex_watch_deferred,
                boot_render_tex_watch_gpu_drops, boot_render_tex_watch_ticks;   /* ticks: QPC spent checking and uploading */
/* The GPU writes [va, va + size): a pending GPU write over it (not the overflow flag) or a render-25 owned granule. */
static inline int boot_render_tex_watch_gpu(uint64_t va, uint64_t size) {
    for (unsigned i = 0; i < boot_render_gw_count; ++i)
        if (boot_render_gw[i].va < va + size && va < boot_render_gw[i].va + boot_render_gw[i].size) return 1;
    for (uint64_t gr = va >> 16, last = (va + size - 1) >> 16; gr <= last; ++gr) {
        const uint8_t *t = boot_render_own_table(gr << 16, 0);
        if (t && t[gr & 0xffff]) return 1;
    }
    return 0;
}
static inline void boot_render_tex_watch_config(void) {
    if (boot_render_tex_watch_limit != UINT64_MAX) return;
    char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_WATCH_MB", v, sizeof v);
    uint64_t mb = (n && n < sizeof v) ? (uint64_t)strtoull(v, NULL, 10) : 8;
    if (mb > 4096) mb = 4096;
    boot_render_tex_watch_budget = mb << 20;
    boot_render_tex_watch_limit = mb ? UINT64_C(16) << 20 : 0;
}
static inline void boot_render_tex_watch_drop(uint32_t e) {
    BootRenderTexWatch *w = &boot_render_tex_watch[e];
    if (w->on && w->index < BOOT_RENDER_TEX_SLOTS && boot_render_tex_watch_of[w->index] == e + 1) boot_render_tex_watch_of[w->index] = 0;
    w->on = 0; w->res = NULL;
}
/* The slot is being rebuilt (boot_render_tex_mark): drop its entry, and watch the new build if it is from guest memory. */
static inline void boot_render_tex_watch_forget(uint32_t index) {
    uint32_t e = boot_render_tex_watch_of[index];
    boot_render_tex_watch_of[index] = 0;
    if (e && e <= BOOT_RENDER_TEX_WATCH) { boot_render_tex_watch_want[index] = 1; boot_render_tex_watch[e - 1].on = 0; boot_render_tex_watch[e - 1].res = NULL; }
}
/* Watched and checked at a use in the last 8 submissions: the round-robin verifier skips it. */
static inline int boot_render_tex_watch_fresh(uint32_t index) {
    uint32_t e = boot_render_tex_watch_of[index];
    return e && e <= BOOT_RENDER_TEX_WATCH && boot_render_tex_watch[e - 1].on && boot_render_tex_watch[e - 1].exec + 8 > boot_render_exec_id;
}
static inline uint64_t boot_render_tex_watch_band(const BootRenderTexWatch *w, const uint8_t *p, uint32_t b) {
    uint64_t at = (uint64_t)b * w->band_bytes, n = w->size - at < w->band_bytes ? w->size - at : w->band_bytes;
    return boot_render_tex_checksum(p + at, n);
}
/* End of a build from guest bytes `src` (boot_render_texture): watch the slot when it qualifies. */
static inline void boot_render_tex_watch_note(uint32_t index, const BootTic *t, const BootTexFormat *f, const BootTexLevel *lv,
                                              unsigned levels, unsigned layers, unsigned dim, DXGI_FORMAT fmt, const uint8_t *src) {
    boot_render_tex_watch_config();
    const BootRenderTexSlot *s = &boot_render_tex_slots[index];
    if (boot_render_tex_watch_of[index]) boot_render_tex_watch_drop(boot_render_tex_watch_of[index] - 1u);
    if (!boot_render_tex_watch_limit || !src || !s->valid || !s->res || s->view_of || !s->size || s->size > boot_render_tex_watch_limit) return;
    int simple = dim == BOOT_DIM_2D && levels == 1 && layers == 1 && !f->decode && f->block_w == 1 && f->block_h == 1 && lv[0].offset == 0;
    int always = simple && fmt == DXGI_FORMAT_R8_UNORM && s->size >= (UINT64_C(1) << 20);   /* glyph cache */
    int seeded = always || (simple && t->pitch);
    if (!seeded && (!boot_render_tex_watch_want[index] || s->size > (UINT64_C(4) << 20))) return;
    if (boot_render_tex_watch_gpu(s->va, s->size)) return;   /* GPU-written memory */
    uint32_t e = BOOT_RENDER_TEX_WATCH, victim = BOOT_RENDER_TEX_WATCH;
    for (uint32_t k = 0; k < BOOT_RENDER_TEX_WATCH && e == BOOT_RENDER_TEX_WATCH; ++k) {
        const BootRenderTexWatch *x = &boot_render_tex_watch[k];
        if (!x->on) e = k;
        else if (!x->always && (victim == BOOT_RENDER_TEX_WATCH || x->quiet > boot_render_tex_watch[victim].quiet)) victim = k;
    }
    if (e == BOOT_RENDER_TEX_WATCH) {   /* full: a seeded texture displaces the quietest other one */
        if (!seeded || victim == BOOT_RENDER_TEX_WATCH) return;
        boot_render_tex_watch_drop(victim); e = victim;
    }
    BootRenderTexWatch *w = &boot_render_tex_watch[e]; memset(w, 0, offsetof(BootRenderTexWatch, sum));
    w->index = index; w->seeded = (uint32_t)seeded; w->always = (uint32_t)always; w->res = s->res; w->va = s->va; w->size = s->size; w->fmt = fmt;
    w->exec = boot_render_exec_id;   /* built from the current bytes in this submission */
    w->pitch = t->pitch; w->width = lv[0].width; w->width_bytes = lv[0].width_bytes; w->rows = lv[0].rows; w->log_gob_h = lv[0].log_gob_h;
    uint64_t unit = 4096; uint32_t unit_rows = 0;
    if (simple && t->pitch) { unit = t->pitch; unit_rows = 1; }
    else if (simple) {
        size_t linear = 0, tiled = 0;
        if (boot_texture_sizes(w->width_bytes, w->rows, w->log_gob_h, &linear, &tiled) && tiled <= s->size) {
            unit = (((uint64_t)w->width_bytes + 63) / 64) * (UINT64_C(512) << w->log_gob_h); unit_rows = 8u << w->log_gob_h; }
    }
    w->inplace = unit_rows != 0;
    uint64_t units = (s->size + unit - 1) / unit, per = (UINT64_C(65536) + unit - 1) / unit;
    if (!per) per = 1;
    while ((units + per - 1) / per > BOOT_RENDER_TEX_WATCH_BANDS) ++per;
    w->band_bytes = per * unit; w->rows_per_band = (uint32_t)(per * unit_rows);
    w->bands = (uint32_t)((s->size + w->band_bytes - 1) / w->band_bytes);
    for (uint32_t b = 0; b < w->bands; ++b) w->sum[b] = boot_render_tex_watch_band(w, src, b);
    w->on = 1; boot_render_tex_watch_of[index] = (uint8_t)(e + 1); ++boot_render_tex_watch_adds;
    if (boot_render_tex_watch_adds <= 16) {
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "texture watch: TIC %u %ux%u dxgi %d %s %llu KiB at %llx: %s, %u bands\n", index, t->width, t->height, (int)fmt,
                          t->pitch ? "pitch-linear" : "block-linear", (unsigned long long)(s->size >> 10), (unsigned long long)s->va,
                          always ? "glyph cache" : seeded ? "CPU-written candidate" : "rewritten since its build", w->bands); fclose(lf); }
    }
}
/* Copies the changed bands of guest image `p` into the watched slot's resource, in command order. 0: not done
 * (nothing recorded for the failing band; the caller rebuilds the slot, which rewrites every byte). */
static inline int boot_render_tex_watch_upload(BootRender *r, BootRenderTexWatch *w, const uint8_t *p, const uint8_t *changed) {
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const UINT row_pitch = (w->width_bytes + 255u) & ~255u;
    for (uint32_t b = 0; b < w->bands; ) {
        if (!changed[b]) { ++b; continue; }
        uint32_t b1 = b; while (b1 < w->bands && changed[b1]) ++b1;
        uint64_t y0 = (uint64_t)b * w->rows_per_band, y1 = (uint64_t)b1 * w->rows_per_band;
        if (y1 > w->rows) y1 = w->rows;
        if (y0 >= y1) { b = b1; continue; }
        uint32_t n = (uint32_t)(y1 - y0);
        if (!boot_render_begin(r)) return 0;
        ID3D12Resource *upload = NULL; uint64_t at = 0; uint8_t *up = boot_render_upload_alloc(r, (uint64_t)row_pitch * n, &upload, &at);
        if (!up || !r->recording) return 0;
        if (w->pitch) {
            for (uint32_t y = 0; y < n; ++y) memcpy(up + (size_t)y * row_pitch, p + (y0 + y) * w->pitch, w->width_bytes);
        } else {   /* block-linear: a band is whole block rows, laid out as an image of n rows */
            uint64_t off = (uint64_t)b * w->band_bytes; size_t linear = 0, tiled = 0;
            if (!boot_texture_sizes(w->width_bytes, n, w->log_gob_h, &linear, &tiled) || off + tiled > w->size) return 0;
            uint8_t *lin = row_pitch == w->width_bytes ? up : (uint8_t *)malloc(linear);
            if (!lin) return 0;
            int ok = boot_texture_detile(lin, linear, p + off, (size_t)(w->size - off), w->width_bytes, n, w->log_gob_h);
            if (ok && lin != up) for (uint32_t y = 0; y < n; ++y) memcpy(up + (size_t)y * row_pitch, lin + (size_t)y * w->width_bytes, w->width_bytes);
            if (lin != up) free(lin);
            if (!ok) return 0;
        }
        D3D12_RESOURCE_BARRIER br = {0}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = w->res;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; br.Transition.StateBefore = srv; br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("tex watch upload", r->cl, 1, &br);
        D3D12_TEXTURE_COPY_LOCATION dst = {w->res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, src = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
        src.PlacedFootprint.Offset = at; src.PlacedFootprint.Footprint.Format = w->fmt; src.PlacedFootprint.Footprint.Width = w->width;
        src.PlacedFootprint.Footprint.Height = n; src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = row_pitch;
        BOOT_GP_CTR("tex watch upload", r->cl, &dst, 0, (UINT)y0, 0, &src, NULL);
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; br.Transition.StateAfter = srv;
        BOOT_GP_RB("tex watch upload", r->cl, 1, &br);
        boot_render_tex_watch_rows += n; boot_render_tex_watch_uploaded += (uint64_t)row_pitch * n;
        b = b1;
    }
    return 1;
}
/* A watched slot about to be used with unchanged TIC words: its guest bytes are compared band by band (once per
 * submission); changed bands are re-uploaded in place, or the slot is poisoned so the caller rebuilds it. */
static inline void boot_render_tex_watch_check(BootRender *r, uint32_t index) {
    uint32_t e = boot_render_tex_watch_of[index] - 1u;
    if (e >= BOOT_RENDER_TEX_WATCH) { boot_render_tex_watch_of[index] = 0; return; }
    BootRenderTexWatch *w = &boot_render_tex_watch[e]; const BootRenderTexSlot *s = &boot_render_tex_slots[index];
    if (w->exec == boot_render_exec_id) return;
    if (!w->on || w->index != index || s->res != w->res || s->va != w->va || s->size != w->size || s->view_of) { boot_render_tex_watch_drop(e); return; }
    if (boot_render_tex_watch_exec_at != boot_render_exec_id) { boot_render_tex_watch_exec_at = boot_render_exec_id; boot_render_tex_watch_exec_bytes = 0; }
    if (!w->always && boot_render_tex_watch_exec_bytes && boot_render_tex_watch_exec_bytes + w->size > boot_render_tex_watch_budget) {
        ++boot_render_tex_watch_deferred; return; }   /* the others share the budget; glyph caches are always checked */
    w->exec = boot_render_exec_id; boot_render_tex_watch_exec_bytes += w->size;
    /* GPU writes over it: not a CPU-written texture (no wait, no decode of GPU-owned bytes); the verifier as before */
    if (boot_render_tex_watch_gpu(w->va, w->size)) { ++boot_render_tex_watch_gpu_drops; boot_render_tex_watch_drop(e); boot_render_tex_watch_want[index] = 0; return; }
    const uint8_t *p = boot_render_guest(r, w->va, w->size);
    if (!p) { boot_render_tex_watch_drop(e); return; }
    uint64_t t0 = boot_tex_now();
    ++boot_render_tex_watch_checks; boot_render_tex_watch_hashed += w->size;
    uint64_t now[BOOT_RENDER_TEX_WATCH_BANDS]; uint8_t changed[BOOT_RENDER_TEX_WATCH_BANDS]; unsigned any = 0;
    for (uint32_t b = 0; b < w->bands; ++b) { now[b] = boot_render_tex_watch_band(w, p, b); changed[b] = now[b] != w->sum[b]; any += changed[b]; }
    if (!any) {
        if (!w->always && ++w->quiet > 600) { boot_render_tex_watch_drop(e); boot_render_tex_watch_want[index] = 0; }
        boot_render_tex_watch_ticks += boot_tex_now() - t0;
        return;
    }
    w->quiet = 0;
    /* the hashes taken before the copy are kept: a CPU write racing the copy is seen again next submission */
    int done = w->inplace && boot_render_res_refs(w->res) == 1 && boot_render_tex_watch_upload(r, w, p, changed);
    boot_render_tex_watch_ticks += boot_tex_now() - t0;
    if (!done) { ++boot_render_tex_watch_rebuilds; boot_render_tex_poison(index); return; }
    for (uint32_t b = 0; b < w->bands; ++b) w->sum[b] = now[b];
    boot_render_tex_sum[index] = 0;   /* the build-time checksum no longer describes the resource (no dedup sharing) */
    if (++boot_render_tex_watch_refreshes <= 16 || boot_render_tex_watch_refreshes % 1000 == 0) {
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "texture watch: TIC %u rewritten by the CPU: %u of %u bands re-uploaded in place (refresh %llu)\n", index, any, w->bands,
                          (unsigned long long)boot_render_tex_watch_refreshes); fclose(lf); }
    }
}
#include "host_bc7.h"   /* bc7-work: ASTC textures held as BC7 (TOTK_BC7=1) */
static inline int boot_render_fb_game_tic(BootRender *r, uint32_t index);   /*99: defined in draw header*/
/* Creates the host texture for TIC index `index` (guest TIC pool at `pool`) and writes its SRV into
 * the table of its type: 1D, 1D array, 2D, 2D array, 3D, cube, cube array or 1D buffer. */
/* nvnpar-work: the descriptor words of (pool, index) come from the host table (registered), not guest memory. */
static inline int boot_render_texmemo_host(int smp, uint64_t pool, uint32_t index) {
    const BootRenderHostTable *t = boot_render_host_table(smp, pool, 0);
    return t && index < t->cap && t->valid[index];
}
static inline int boot_render_texture_resolve(BootRender *r, uint64_t pool, uint32_t index);
static inline int boot_render_texture(BootRender *r, uint64_t pool, uint32_t index) {
    if (index < BOOT_RENDER_TEX_SLOTS - 256) {
        uint64_t k = boot_render_texmemo_key(pool);
        if (k && boot_render_texmemo[index] == k && boot_render_texmemo_on()) {   /* nvnpar-work: remembered (see boot_render_texmemo) */
            if (!boot_render_texmemo_verify_on()) { boot_render_tex_touch(index); ++boot_render_host_hits; ++boot_render_texmemo_hits; return BOOT_RENDER_OK; }
            boot_render_texmemo[index] = 0; boot_render_texmemo_pure = 0; ++boot_render_texmemo_verified;
            int rc = boot_render_texture_resolve(r, pool, index);
            if (!boot_render_texmemo_pure && ++boot_render_texmemo_bad <= 16) {
                FILE *lf = fopen("draw-path.log", "a");
                if (lf) { fprintf(lf, "texset cache verify: TIC %u of pool %llx was remembered but its resolution changed something (rc %d, submission %llu)\n",
                                  index, (unsigned long long)pool, rc, (unsigned long long)boot_render_exec_id); fclose(lf); }
            }
            return rc;
        }
    }
    if (index < BOOT_RENDER_TEX_SLOTS) boot_render_texmemo[index] = 0;   /* re-resolved: remembered again only by the unchanged return */
    return boot_render_texture_resolve(r, pool, index);
}
static inline int boot_render_texture_resolve(BootRender *r, uint64_t pool, uint32_t index) {
    if (index >= BOOT_RENDER_TEX_SLOTS) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "TIC index beyond host table");
    if (index >= BOOT_RENDER_TEX_SLOTS - 256 && !boot_render_fb_game_tic(r, index))
        return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending feedback/game TIC collision");
    if (!boot_render_tex_table()) return boot_render_fail(r, BOOT_RENDER_E_D3D, "texture slot table");
    ++boot_render_texmemo_resolves; boot_render_texmemo_pure = 0;
    const int memo_entry_fb = boot_render_fb_on[index], memo_entry_watch = boot_render_tex_watch_of[index] != 0;   /* nvnpar-work */
    boot_render_tex_touch(index);   /* before anything below can submit: an eviction never takes this slot */
    if (boot_render_fb_on[index]) {   /* leaving a feedback copy: draws recorded with it run first */
        boot_render_fb_on[index] = 0;
        if (r->recording && (!boot_render_submit(r) || !boot_render_begin(r))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush (feedback view)");
    }
    const uint32_t *words = boot_render_desc_words(r, pool, index, 0);
    if (!words) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "TIC entry outside guest memory");
    BootRenderTexSlot *slot = &boot_render_tex_slots[index];
    if (!slot->valid && !slot->res) {   /* render target view made for the same entry, table unchanged */
        BootRenderAlias *a = boot_render_alias_entry(r, index); BootRenderRtTable *T = boot_render_rtab(r);
        if (a && a->on && !(boot_render_no_depth_alias && a->k < T->count && (T->rts[a->k].format & 0x10000u)) && a->generation == T->generation && a->k < T->count && T->rts[a->k].texture == a->texture && !memcmp(a->words, words, 32)) {
            ++boot_render_alias_hits;
            int rc = boot_render_alias_use(r, T, a->k, a->dirty_epoch != boot_render_rt_dirty_epoch);
            if (!rc) a->dirty_epoch = boot_render_rt_dirty_epoch;   /* no other target over it is dirty now */
            return rc;
        }
    }
    boot_render_alias_forget(index);   /* any other outcome rewrites (or keeps) another view at this index */
    if (slot->valid && boot_render_tex_watch_of[index] && !memcmp(slot->words, words, 32))
        boot_render_tex_watch_check(r, index);   /* render-39: CPU writes since the last check (may poison: rebuilt below) */
    if (slot->valid && !memcmp(slot->words, words, 32)) {
        if (!slot->view_of && !((slot->size == 0 || boot_render_tex_subres[index]) && slot->res && boot_render_asm_stale(r, index))) {   /* wb-02: | subresource assemblies */
            if (boot_title_event_active && slot->size == 0 && slot->res) boot_render_title_assembly(r, index, BOOT_TITLE_SAMPLE, 0, 0, 0);
            if (boot_image_trace_enabled && slot->size == 0 && slot->res) boot_render_image_asm_fast(r, index);
            /* nvnpar-work: an unchanged return that read nothing time-dependent is remembered (boot_render_texmemo) */
            if (!memo_entry_fb && !memo_entry_watch && slot->size && !boot_render_tex_subres[index] && index < BOOT_RENDER_TEX_SLOTS - 256 &&
                boot_render_texmemo_host(0, pool, index) && boot_render_texmemo_key(pool)) {
                boot_render_texmemo_pure = 1;
                if (boot_render_texmemo_on()) { boot_render_texmemo[index] = boot_render_texmemo_key(pool); ++boot_render_texmemo_sets; }
            }
            return BOOT_RENDER_OK;   /* render-14: stale assembly refreshed below */
        }
        /* zero-copy buffer view: still valid while the address maps to the same region resource */
        uint64_t off = 0; int region = boot_render_region_of(r, slot->va, &off);
        if (region >= 0 && r->region[region] == slot->view_of && off == slot->view_offset) return BOOT_RENDER_OK;
    }
    slot->view_of = NULL;
    BootTic t; BootTexFormat f;
    if (!boot_render_tic_decode_any(words, &t)) {
        char why[112]; snprintf(why, sizeof why, "TIC header version %u / layout (gob %u/%u, tile width %u, type %u) unsupported",
                                 t.header_version, t.log_gob_h, t.log_gob_d, t.tile_width, t.texture_type);
        return boot_render_fail(r, BOOT_RENDER_E_FORMAT, why);
    }
    if (!boot_tex_format(t.components, t.data_type[0], &f)) {
        char why[64]; snprintf(why, sizeof why, "TIC texture format %02x type %u", t.components, t.data_type[0]);
        return boot_render_fail(r, BOOT_RENDER_E_FORMAT, why);
    }
    if (t.texture_type == BOOT_TEX_1D_BUFFER) {
        if (t.header_version != 0) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "1D buffer type with a non-buffer header");
        return boot_render_texture_buffer(r, slot, &t, &f, words, index);
    }
    /* Table, layer count and memory layout. A 1D/2D/cube header with depth > 1 or a base layer views
     * one image (one or six layers) of a layered resource, which is laid out as the array type
     * (base layer: the TIC view_layer_base fields, deko3d source/maxwell/texture_image_control_block.h). The view
     * starts at the base layer. */
    unsigned dim; uint32_t layers = 1, depth = 1; BootTic lt = t;
    int layered = t.depth > 1 || t.base_layer != 0;
    switch (t.texture_type) {
        case BOOT_TEX_1D: dim = BOOT_DIM_1D; if (layered) lt.texture_type = BOOT_TEX_1D_ARRAY; break;
        case BOOT_TEX_1D_ARRAY: dim = BOOT_DIM_1D_ARRAY; layers = t.depth; break;
        case BOOT_TEX_2D: case BOOT_TEX_2D_NO_MIPMAP: dim = BOOT_DIM_2D; if (layered) lt.texture_type = BOOT_TEX_2D_ARRAY; break;
        case BOOT_TEX_2D_ARRAY: dim = BOOT_DIM_2D_ARRAY; layers = t.depth; break;
        case BOOT_TEX_3D:
            dim = BOOT_DIM_3D; depth = t.depth;
            if (t.base_layer) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "3D texture with a base layer");
            break;
        case BOOT_TEX_CUBE: dim = BOOT_DIM_CUBE; layers = 6; if (layered) lt.texture_type = BOOT_TEX_CUBE_ARRAY; break;
        case BOOT_TEX_CUBE_ARRAY: dim = BOOT_DIM_CUBE_ARRAY; layers = 6 * t.depth; break;
        default: {
            char why[48]; snprintf(why, sizeof why, "TIC texture type %u", t.texture_type);
            return boot_render_fail(r, BOOT_RENDER_E_FORMAT, why);
        }
    }
    if (dim == BOOT_DIM_1D || dim == BOOT_DIM_1D_ARRAY) lt.height = t.height = 1;
    if ((dim == BOOT_DIM_CUBE || dim == BOOT_DIM_CUBE_ARRAY) && t.width != t.height) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "cube texture with unequal width and height");
    /* Render-to-texture: the image lives in a host render target (GPU writes do not reach guest
     * memory), so sample that resource directly. The view is remembered (boot_render_alias): a later
     * use of the same TIC entry while the target table is unchanged only reloads a stale target and
     * makes it shader readable, without rewriting the index's eight descriptors. */
    /* Only a target that IS this image qualifies: same address, same extent (the block-linear layout
     * depends on the width and height) and the TIC's own view format (an SRV of a typed resource
     * cannot reinterpret its bits). Transient memory is reused by targets of other formats and sizes
     * (e.g. 50e1d0000 is R11G11B10 320x180, RGBA8 320x180 and R11G11B10 640x360 in one frame); taking
     * the first entry at the address sampled another format's pixels decoded as this one (red/green
     * noise in the composite). Several qualifying entries: the most recently used. No qualifying
     * entry: the guest path below writes the targets over this memory back (oldest first) and
     * decodes the newest bytes in the TIC's format, as the hardware reads them. */
    if ((dim == BOOT_DIM_2D && t.max_mip) || (dim == BOOT_DIM_2D_ARRAY && !t.base_layer)) {   /* levels / layers rendered one by one */
        int rc = boot_render_texture_mips_from_targets(r, slot, &t, &f, words, index, dim, layers);
        if (rc >= 0) return rc;
    }
    if (dim == BOOT_DIM_2D && !t.base_layer) {
        BootRenderRtTable *T = boot_render_rtab(r);
        DXGI_FORMAT want = (t.srgb && f.srgb != DXGI_FORMAT_UNKNOWN) ? f.srgb : f.unorm;
        int best = -1;
        for (unsigned k = 0; k < T->count; ++k) {
            const BootRenderTarget *x = &T->rts[k];
            if (!x->texture || x->address != t.address || x->width != t.width || x->height != t.height) continue;
            /* a single-plane depth target is sampled through its colour view (R32_FLOAT / R16_UNORM): the
             * TIC's ZF32 / Z16 / R32F / R16 reads the same bits the write-back would put in guest memory */
            int depth = (x->format & 0x10000u) != 0;
            if (depth && (boot_render_no_depth_alias || (x->format & BOOT_RENDER_PITCH_FLAG))) continue;
            DXGI_FORMAT view = depth ? boot_render_depth_srv_format(x->dxgi) : x->dxgi;
            if (view == DXGI_FORMAT_UNKNOWN || view != want || f.decode) { ++boot_render_alias_format_misses; boot_render_alias_miss_log(&t, index, want, x, view); continue; }
            if (best < 0 || T->info[k].last_use >= T->info[best].last_use) best = (int)k;
        }
        /* batch-b: the view chosen is the depth target this draw tests and writes. The draw would drop it (D3D12 cannot
         * read it while writing it: host_gpu_render_draw.h, boot_render_depth_alias_written) and call again with depth
         * targets excluded (boot_render_no_depth_alias). That call's result directly, without building the view first
         * (its slot_free waited for the GPU): when no colour target qualifies here (the second call's loop then finds
         * none either), the target needs no reload (alias_use would reload a stale one first) and there is no mip
         * assembly attempt in between (2D, one level), the second call continues exactly as below. */
        if (best >= 0 && boot_render_dfb_zt && T->rts[best].texture == boot_render_dfb_zt && !t.max_mip && !T->info[best].stale && !boot_render_rt_overlap_reload()) {
            int other = 0;
            for (unsigned k = 0; k < T->count && !other; ++k) {
                const BootRenderTarget *x = &T->rts[k];
                if (!x->texture || x->address != t.address || x->width != t.width || x->height != t.height || (x->format & 0x10000u)) continue;
                other = x->dxgi != DXGI_FORMAT_UNKNOWN && x->dxgi == want && !f.decode;
            }
            if (!other) { best = -1; ++boot_render_dfb_alias_skips; }
        }
        /* drain2-work: a slot that holds a depth-feedback copy of this depth target (same TIC words: only the poison bit
         * differs) is refreshed in place (boot_render_dfb_in_place: the target copied into the slot's own texture here in
         * queue order, its descriptors kept) instead of becoming a view of the target. The same texels for every draw that
         * reads it (drawing the target poisons the slot again, the next use refreshes it); no descriptor rewrite, so no list
         * execution, staging or wait (TIC 12738: view / copy switches several times a frame). Only when the target is the
         * newest dirty one over the copy's memory (what the refresh copies; otherwise the view, as before). TOTK_DFB_KEEP_COPY=0:
         * the view. */
        if (best >= 0 && boot_render_dfb_keep_on() && slot->valid && slot->res && !slot->view_of && boot_render_dfb_slot[index] && (T->rts[best].format & 0x10000u) &&
            T->info[best].dirty && !T->info[best].stale && slot->dim == dim && slot->va == t.address &&
            (slot->words[0] ^ words[0]) == (boot_render_tex_poisoned[index] ? 0x80000000u : 0u) && !memcmp(slot->words + 1, words + 1, 28)) {
            unsigned dl[BOOT_RENDER_RT_CAP], dn = boot_render_rt_dirty_over(T, slot->va, slot->size, dl); int newest = dn && !(boot_render_rt_overlap_reload() && dn != 1);
            for (unsigned q = 0; q < dn && newest; ++q) if ((int)dl[q] != best && boot_render_rt_written_at[dl[q]] >= boot_render_rt_written_at[best]) newest = 0;
            if (newest) { best = -1; ++boot_render_dfb_kept; }
        }
        if (best >= 0) {
            unsigned k = (unsigned)best; BootRenderTarget *x = &T->rts[k];
            if (slot->valid && !boot_render_slot_free(r, (uint32_t)(slot - boot_render_tex_slots))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush");
            /* t2-B: the slot's idle texture goes to the texture pool (as the guest path's `previous` and tex_trim's
             * evictions) for a later build of its shape, instead of being destroyed: the TIC ring turns slots that
             * held a texture into target views every frame (play-81: DestroyAllocation2 / FreeGpuVirtualAddress
             * under boot_render_texture, CreateCommittedResource + a CPU wait in the builds that followed) */
            if (slot->res) { if (slot->dim != BOOT_DIM_BUFFER) { boot_render_tex_pool_put(slot->res); ++boot_render_tex_alias_pooled; } else if (!boot_render_sv_take(slot->res, 0)) ID3D12Resource_Release(slot->res); slot->res = NULL; }   /* drain-work: held while versioned */
            slot->valid = 0;
            int rc = boot_render_alias_use(r, T, k, 1); if (rc) return rc;
            D3D12_SHADER_RESOURCE_VIEW_DESC rv = {0};
            int depth = (x->format & 0x10000u) != 0; if (depth) ++boot_render_depth_alias_views;
            rv.Format = depth ? boot_render_depth_srv_format(x->dxgi) : x->dxgi; rv.Shader4ComponentMapping = boot_render_swizzle(&t);
            rv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; rv.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(r->gpu->device, x->texture, &rv, boot_render_tex_cpu(r, dim, index));
            boot_render_tex_isolate_srv(r, index, dim, x->texture, &rv);
            slot->dim = dim; slot->va = t.address; slot->size = 0;
            BootRenderAlias *a = boot_render_alias_entry(r, index);
            if (a) { memcpy(a->words, words, 32); a->texture = x->texture; a->generation = T->generation; a->dirty_epoch = boot_render_rt_dirty_epoch; a->k = k; a->on = 1; }
            ++boot_render_alias_views;
            return BOOT_RENDER_OK;
        }
    }
    /* Mip levels: the header's count, bounded by what the extent allows (D3D12 rejects more). */
    unsigned levels = t.max_mip + 1; if (levels > 16) levels = 16;
    {   uint32_t extent = t.width > t.height ? t.width : t.height; if (depth > extent) extent = depth;
        unsigned possible = 1; while (possible < 16 && (extent >> possible)) ++possible;
        if (levels > possible) levels = possible; }
    BootTexLevel lv[16]; uint64_t stride;
    if (t.pitch) {
        /* Pitch-linear: one level of rows `pitch` bytes apart (no mipmaps or layers in this layout). */
        if (layers != 1 || layered || dim == BOOT_DIM_3D) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "pitch-linear texture with layers or depth");
        levels = 1; memset(lv, 0, sizeof lv[0]);
        lv[0].width = t.width; lv[0].height = t.height; lv[0].depth = 1;
        lv[0].width_bytes = (t.width + f.block_w - 1) / f.block_w * f.bytes;
        lv[0].rows = (lv[0].height + f.block_h - 1) / f.block_h;
        if (lv[0].width_bytes > t.pitch) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "pitch smaller than a row");
        lv[0].size = (uint64_t)t.pitch * (lv[0].rows - 1) + lv[0].width_bytes; stride = lv[0].size;
    } else stride = boot_tex_layout(&lt, &f, lv, levels);
    if (!stride) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "texture layout");
    uint64_t data_va = t.address + (uint64_t)t.base_layer * stride;
    {   int rc = boot_render_texture_from_target(r, slot, &t, &f, words, index, dim, levels, stride, layers, data_va);   /* GPU, no wait */
        if (rc >= 0) return rc; }
    /* render targets the GPU wrote over this memory (e.g. slices or mips rendered into a 3D texture) */
    if (!boot_render_flush_targets(r, data_va, stride * layers)) return BOOT_RENDER_E_D3D;
    if (!boot_render_cpu_read(r, data_va, stride * layers)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending GPU writes");
    uint8_t *src = boot_render_guest(r, data_va, stride * layers);
    if (!src) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "texture data outside guest memory");
    /* A changed slot may still be referenced by recorded draws: finish them first. */
    if (slot->valid && !boot_render_slot_free(r, (uint32_t)(slot - boot_render_tex_slots))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush");
    /* The slot's previous texture is idle now (submitted above, or nothing recorded): a rebuild
     * with the same shape and format re-uploads into it instead of destroying and creating one. */
    ID3D12Resource *previous = slot->res; slot->res = NULL;
    slot->valid = 0;
    /* Dedup: the game rotates TIC indices (a new index per frame for the same image), so one image
     * would be uploaded into a resource per index. A valid slot with the identical TIC entry whose
     * build saw the same guest bytes already holds this image: share its resource. */
    DXGI_FORMAT fmt = (t.srgb && f.srgb != DXGI_FORMAT_UNKNOWN) ? f.srgb : f.unorm;
    uint64_t sum_now = boot_render_tex_checksum(src, stride * layers); ID3D12Resource *tex = NULL; HRESULT hr = S_OK;
    if (boot_sd_on()) boot_sd_tex_build(r, index, data_va, src, stride * layers, sum_now);   /* suspect-diag S1 / S4 */
    BootBc7Pre bc7pre = {0};   /* bc7-work */
    {   int o = boot_render_tex_same(index, words, dim, sum_now, stride * layers);
        if (o >= 0 && boot_bc7_share_ok(o)) {   /* bc7-work: a vertex-stage binding never shares a BC7 image */
            tex = boot_render_tex_slots[o].res; ID3D12Resource_AddRef(tex);
            fmt = boot_bc7_view_format(tex, fmt);   /* bc7-work: a slot switched to BC7 is viewed as BC7 */
            if (previous) { boot_render_tex_pool_put(previous); previous = NULL; }
            ++boot_render_tex_shared;
            goto view;
        }
    }
    /* bc7-work: exclusions, content key and the BC7 disk cache (a hit is uploaded as BC7: no ASTC decode, no RGBA8). The
     * later stages plug in at host_bc7.h boot_bc7_choose_producer: GPU ASTC decode (stage 2) and direct ASTC 4x4 -> BC7
     * block transcoding (stage 3) replace the CPU decode below for the textures they take. */
    {   int bc7 = boot_bc7_try_cached(r, index, &t, &f, dim, levels, layers, stride, data_va, src, &previous, &tex, &fmt, &bc7pre);
        if (bc7 > 0) goto view;
        if (bc7 < 0) return BOOT_RENDER_E_FORMAT; }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d = {0};
    int one_d = dim == BOOT_DIM_1D || dim == BOOT_DIM_1D_ARRAY;
    d.Dimension = one_d ? D3D12_RESOURCE_DIMENSION_TEXTURE1D : dim == BOOT_DIM_3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = t.width; d.Height = one_d ? 1 : t.height; d.DepthOrArraySize = (UINT16)(dim == BOOT_DIM_3D ? depth : layers); d.MipLevels = (UINT16)levels;
    d.Format = fmt; d.SampleDesc.Count = 1;
    if (previous) {
        D3D12_RESOURCE_DESC pd; previous->lpVtbl->GetDesc(previous, &pd);
        if (pd.Dimension == d.Dimension && pd.Width == d.Width && pd.Height == d.Height && pd.DepthOrArraySize == d.DepthOrArraySize &&
            pd.MipLevels == d.MipLevels && pd.Format == d.Format && pd.SampleDesc.Count == 1 && pd.Flags == d.Flags &&
            boot_render_res_refs(previous) == 1 && boot_render_begin(r)) {   /* not re-uploaded while another slot shares it */
            tex = previous; previous = NULL; ++boot_render_tex_reused;
            D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            BOOT_GP_RB("texture upload", r->cl, 1, &b);
        } else { boot_render_tex_pool_put(previous); previous = NULL; }
    }
    if (!tex && (tex = boot_render_tex_pool_take(&d)) != NULL) {   /* an idle texture of this shape from another slot */
        if (boot_render_begin(r)) {
            D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            BOOT_GP_RB("texture upload", r->cl, 1, &b); ++boot_render_tex_pooled;
        } else { ID3D12Resource_Release(tex); tex = NULL; }
    }
    {   uint64_t tc0 = boot_tex_now();   /* play (50) */
        if (!tex) hr = ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex);
        boot_tex_t_create += boot_tex_now() - tc0; ++boot_tex_built; }
    if (FAILED(hr)) {
        char why[128]; snprintf(why, sizeof why, "texture resource %ux%ux%u levels %u format %d hr=0x%08lx", t.width, t.height, d.DepthOrArraySize, levels, (int)fmt, (unsigned long)hr);
        return boot_render_fail(r, BOOT_RENDER_E_D3D, why);
    }
    unsigned images = dim == BOOT_DIM_3D ? 1 : layers, subs = levels * images;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fps = calloc(subs, sizeof *fps); UINT *rows = calloc(subs, sizeof *rows);
    UINT64 *row_sizes = calloc(subs, sizeof *row_sizes), total = 0;
    int ok = fps && rows && row_sizes;
    const char *why = ok ? NULL : "memory";
    if (ok) ID3D12Device_GetCopyableFootprints(r->gpu->device, &d, 0, subs, 0, fps, rows, row_sizes, &total);
    /* Uploaded in chunks of whole subresources (at most BOOT_RENDER_UPLOAD_CHUNK bytes each unless one
     * subresource is larger): large arrays (e.g. ASTC decoded to RGBA8) would otherwise need one
     * upload buffer the size of the whole texture. */
    for (unsigned first = 0; ok && first < subs; ) {
        unsigned end = first + 1;
        while (end < subs && fps[end].Offset - fps[first].Offset + (end + 1 < subs ? fps[end + 1].Offset - fps[end].Offset : total - fps[end].Offset)
                             <= BOOT_RENDER_UPLOAD_CHUNK) ++end;
        UINT64 base = fps[first].Offset, bytes = (end < subs ? fps[end].Offset : total) - base;
        /* staging from the upload ring (a full ring executes the recorded work first) */
        if (!boot_render_begin(r)) { ok = 0; why = "command list"; break; }
        ID3D12Resource *upload = NULL; uint64_t ring_at = 0; uint8_t *up = boot_render_upload_alloc(r, bytes, &upload, &ring_at);
        if (!up) { ok = 0; why = "upload buffer"; break; }
        for (unsigned sub = first; ok && sub < end; ++sub) {
            unsigned layer = sub / levels, l = sub % levels;
            const BootTexLevel *v = &lv[l];
            size_t slice = (size_t)v->width_bytes * v->rows, linear = slice * v->depth;
            uint64_t tt0 = boot_tex_now();   /* play (50): detile time */
            uint8_t *lin = malloc(linear ? linear : 1);
            const uint8_t *level_src = src + layer * stride + v->offset;
            if (!lin) ok = 0;
            else if (t.pitch) { for (uint32_t y = 0; y < v->rows; ++y) memcpy(lin + (size_t)y * v->width_bytes, level_src + (size_t)y * t.pitch, v->width_bytes); }
            else if (dim == BOOT_DIM_3D) ok = boot_texture_detile_3d(lin, linear, level_src, (size_t)v->size, v->width_bytes, v->rows, v->depth, v->log_gob_h, v->log_gob_d);
            else ok = boot_texture_detile(lin, linear, level_src, (size_t)v->size, v->width_bytes, v->rows, v->log_gob_h);
            if (!ok && !why) why = lin ? "detile" : "memory";
            uint64_t tt1 = boot_tex_now(); boot_tex_t_detile += tt1 - tt0;
            const uint8_t *rowsrc = lin; size_t pitch = v->width_bytes; uint8_t *rgba = NULL; int direct = 0;
            if (ok && f.decode == BOOT_TEX_DECODE_ASTC) {   /* play (50): parallel, straight into the upload ring */
                for (uint32_t z = 0; ok && z < v->depth; ++z)
                    ok = boot_astc_decode_pitched(lin + z * slice, v->width, v->height, f.block_w, f.block_h,
                                                  up + (fps[sub].Offset - base) + (size_t)z * rows[sub] * fps[sub].Footprint.RowPitch, fps[sub].Footprint.RowPitch) >= 0;
                if (!ok && !why) why = "ASTC decode";
                direct = 1; boot_tex_decoded_bytes += (uint64_t)v->width * v->height * 4 * v->depth;
            }
            uint64_t tt2 = boot_tex_now(); boot_tex_t_decode += tt2 - tt1;
            /* rows[sub]: rows per slice in the host format (block rows for BC, pixel rows otherwise) */
            for (UINT z = 0; ok && !direct && z < fps[sub].Footprint.Depth; ++z)
                for (UINT y = 0; y < rows[sub]; ++y)
                    memcpy(up + (fps[sub].Offset - base) + ((size_t)z * rows[sub] + y) * fps[sub].Footprint.RowPitch,
                           rowsrc + ((size_t)z * rows[sub] + y) * pitch, (size_t)row_sizes[sub]);
            free(rgba); free(lin); boot_tex_t_copy += boot_tex_now() - tt2;
        }
        if (ok && !boot_render_begin(r)) { ok = 0; why = "command list"; }
        for (unsigned sub = first; ok && sub < end; ++sub) {
            D3D12_TEXTURE_COPY_LOCATION dst = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}; dst.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION srcl = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
            srcl.PlacedFootprint = fps[sub]; srcl.PlacedFootprint.Offset = srcl.PlacedFootprint.Offset - base + ring_at;
            BOOT_GP_CTR("texture upload", r->cl, &dst, 0, 0, 0, &srcl, NULL);
        }
        if (!ok) break;
        first = end;
    }
    if (ok) {
        D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        BOOT_GP_RB("texture upload", r->cl, 1, &b);
    }
    free(fps); free(rows); free(row_sizes);
    if (!ok) {
        /* copies of earlier chunks may be recorded: the resource is released after the list executes */
        if (r->recording) boot_render_submit(r);
        ID3D12Resource_Release(tex);
        snprintf(r->error, sizeof r->error, "texture upload/decode (%s): TIC %u type %u format %02x %ux%ux%u levels %u device %08lx", why ? why : "command list",
                 index, t.texture_type, t.components, t.width, t.height, t.depth, levels, (unsigned long)ID3D12Device_GetDeviceRemovedReason(r->gpu->device));
        return BOOT_RENDER_E_FORMAT;
    }
view:;
    /* View: the header's mip range (clb097tex.h NVB097_TEXHEAD_BL_RES_VIEW_MIN/MAX_MIP_LEVEL) within the levels uploaded. */
    UINT first_mip = t.view_min_mip < levels ? t.view_min_mip : levels - 1;
    UINT view_mips = t.view_max_mip >= first_mip ? t.view_max_mip - first_mip + 1 : 1;
    if (first_mip + view_mips > levels) view_mips = levels - first_mip;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt; sv.Shader4ComponentMapping = boot_render_swizzle(&t); sv.ViewDimension = boot_render_dim_srv[dim];
    switch (dim) {
        case BOOT_DIM_1D: sv.Texture1D.MostDetailedMip = first_mip; sv.Texture1D.MipLevels = view_mips; break;
        case BOOT_DIM_1D_ARRAY: sv.Texture1DArray.MostDetailedMip = first_mip; sv.Texture1DArray.MipLevels = view_mips; sv.Texture1DArray.ArraySize = layers; break;
        case BOOT_DIM_2D: sv.Texture2D.MostDetailedMip = first_mip; sv.Texture2D.MipLevels = view_mips; break;
        case BOOT_DIM_2D_ARRAY: sv.Texture2DArray.MostDetailedMip = first_mip; sv.Texture2DArray.MipLevels = view_mips; sv.Texture2DArray.ArraySize = layers; break;
        case BOOT_DIM_3D: sv.Texture3D.MostDetailedMip = first_mip; sv.Texture3D.MipLevels = view_mips; break;
        case BOOT_DIM_CUBE: sv.TextureCube.MostDetailedMip = first_mip; sv.TextureCube.MipLevels = view_mips; break;
        default: sv.TextureCubeArray.MostDetailedMip = first_mip; sv.TextureCubeArray.MipLevels = view_mips; sv.TextureCubeArray.NumCubes = layers / 6; break;
    }
    ID3D12Device_CreateShaderResourceView(r->gpu->device, tex, &sv, boot_render_tex_cpu(r, dim, index));
    boot_render_tex_isolate_srv(r, index, dim, tex, &sv);
    memcpy(slot->words, words, 32); slot->res = tex; slot->valid = 1; slot->dim = dim; boot_render_tex_mark(index);
    slot->va = data_va; slot->size = stride * layers;
    if (f.decode == BOOT_TEX_DECODE_ASTC) {   /* mem-work: census only */
        unsigned cm = 0; for (unsigned c = 0; c < 4; ++c) if (t.source[c] >= 2 && t.source[c] <= 5) cm |= 1u << (t.source[c] - 2);
        boot_render_tex_astc[index] = (uint8_t)(1u | cm << 1 | (fmt == f.srgb && f.srgb != DXGI_FORMAT_UNKNOWN ? 32u : 0u) | (f.block_w * f.block_h > 16 ? 64u : 0u));
    } else boot_render_tex_astc[index] = 0;
    boot_render_tex_sum[index] = sum_now; boot_render_tex_dedup_note(index); boot_ax_live(BOOT_AX_L_TEXEL, slot->size);   /* async-xlate: built from guest memory (counted on X) */
    boot_render_rt_copies_forget(index);   /* wb-01: a copy's registrations no longer describe this build (range invalidation does) */
    boot_render_tex_watch_note(index, &t, &f, lv, levels, layers, dim, fmt, src);   /* render-39 */
    boot_bc7_built(r, index, t.components, &bc7pre, &sv);   /* bc7-work: a cache miss queues its BC7 encode */
    if (dim != BOOT_DIM_2D && dim != BOOT_DIM_2D_ARRAY && boot_render_tex_created[dim] < 4) boot_render_tex_log(&t, index, words, "created");
    ++boot_render_tex_created[dim];
    return BOOT_RENDER_OK;
}
/* Guest memory [va, va+size) changed (copy engine, CPU upload): cached textures overlapping it
 * are rebuilt on next use. The old resource stays alive until then (descriptors may reference it). */
static inline void boot_render_invalidate_textures(uint64_t va, uint64_t size) {
    if (!boot_render_tex_slots) return;
    for (uint32_t k = 0; k < boot_render_tex_live_n; ) {   /* t2-B: the slots that may be valid (boot_render_tex_live) */
        uint32_t i = boot_render_tex_live[k];
        BootRenderTexSlot *t = &boot_render_tex_slots[i];
        if (!t->valid) { boot_render_tex_live_in[i] = 0; boot_render_tex_live[k] = boot_render_tex_live[--boot_render_tex_live_n]; continue; }
        if (i < boot_render_tex_high && t->va < va + size && va < t->va + t->size) boot_render_tex_poison(i);   /* force mismatch */
        ++k;
    }
}
static inline void boot_render_invalidate(uint64_t va, uint64_t size) {
    boot_render_cb_written(va, size);   /* constant-buffer snapshots over it */
    boot_render_invalidate_textures(va, size);
}
static inline int boot_render_textures_overlap(uint64_t va, uint64_t size) {
    if (!boot_render_tex_slots) return 0;
    for (uint32_t k = 0; k < boot_render_tex_live_n; ) {   /* t2-B: as boot_render_invalidate_textures */
        uint32_t i = boot_render_tex_live[k];
        const BootRenderTexSlot *t = &boot_render_tex_slots[i];
        if (!t->valid) { boot_render_tex_live_in[i] = 0; boot_render_tex_live[k] = boot_render_tex_live[--boot_render_tex_live_n]; continue; }
        if (i < boot_render_tex_high && t->va < va + size && va < t->va + t->size) return 1;
        ++k;
    }
    return 0;
}
/* Bytes of a slot's resource (cached per slot while the slot keeps the same resource). */
static inline uint64_t boot_render_tex_slot_bytes(BootRender *r, uint32_t i, ID3D12Resource *res) {
    D3D12_RESOURCE_DESC d; res->lpVtbl->GetDesc(res, &d);
    if (boot_render_tex_sized[i].res == res && boot_render_tex_sized[i].width == d.Width && boot_render_tex_sized[i].height == d.Height &&
        boot_render_tex_sized[i].depth == d.DepthOrArraySize && boot_render_tex_sized[i].mips == d.MipLevels && boot_render_tex_sized[i].format == (uint32_t)d.Format)
        return boot_render_tex_sized[i].bytes;
    UINT64 total = d.Width;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        UINT subs = d.MipLevels * (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : d.DepthOrArraySize);
        total = 0; ID3D12Device_GetCopyableFootprints(r->gpu->device, &d, 0, subs, 0, NULL, NULL, NULL, &total);
        if (total == UINT64_MAX) total = boot_render_tex_estimate(&d);
    }
    boot_render_tex_sized[i].res = res; boot_render_tex_sized[i].bytes = total; boot_render_tex_sized[i].width = d.Width; boot_render_tex_sized[i].height = d.Height;
    boot_render_tex_sized[i].depth = d.DepthOrArraySize; boot_render_tex_sized[i].mips = d.MipLevels; boot_render_tex_sized[i].format = (uint32_t)d.Format;
    return total;
}
static int boot_render_tex_age_cmp(const void *a, const void *b) {
    uint32_t x = boot_render_tex_last[*(const uint32_t *)a], y = boot_render_tex_last[*(const uint32_t *)b];
    return x < y ? -1 : x > y;
}
/* End of a guest submission: the queue is idle and no list is open (call right after a waiting
 * boot_render_submit), so no recorded or executing work references any slot's texture. Recounts
 * the resident texture bytes and, beyond `budget`, releases least recently used slot textures down
 * to 7/8 of it (idle textures go to the texture pool for a later build of the same shape; the slot
 * rebuilds from guest memory on its next use, its descriptor reads as null until then). Then
 * advances the clock. Returns the number of evicted slots. */
static inline unsigned boot_render_tex_trim(BootRender *r, uint64_t budget) {
    if (!boot_render_tex_slots || r->recording) { ++boot_render_tex_clock; return 0; }
    uint64_t resident = 0, count = 0; unsigned evicted = 0;
    uint32_t *cand = NULL; uint32_t ncand = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < boot_render_tex_high; ++i) {
            BootRenderTexSlot *s = &boot_render_tex_slots[i];
            if (!s->res) continue;
            if (!pass) {   /* a resource shared by several slots (dedup) is counted once, at its first holder */
                int first = 1; if (boot_render_res_refs(s->res) > 1) for (uint32_t j = 0; j < i && first; ++j) first = boot_render_tex_slots[j].res != s->res;
                if (first) resident += boot_render_tex_slot_bytes(r, i, s->res);
                ++count; continue; }
            if (boot_render_tex_clock - boot_render_tex_last[i] >= 2) cand[ncand++] = i;
        }
        if (pass || resident <= budget) break;
        cand = malloc(sizeof *cand * (count ? count : 1));
        if (!cand) break;
    }
    if (cand && ncand) {
        qsort(cand, ncand, sizeof *cand, boot_render_tex_age_cmp);
        uint64_t target = budget - budget / 8;
        for (uint32_t k = 0; k < ncand && resident > target; ++k) {
            uint32_t i = cand[k]; BootRenderTexSlot *s = &boot_render_tex_slots[i];
            uint64_t bytes = boot_render_res_refs(s->res) > 1 ? 0 : boot_render_tex_slot_bytes(r, i, s->res);   /* shared: freed with its last holder */
            ID3D12Resource *res = s->res; unsigned dim = s->dim;
            s->res = NULL; s->valid = 0; s->view_of = NULL; s->size = 0;
            boot_render_texmemo[i] = 0;   /* nvnpar-work */
            boot_render_tex_sized[i].res = NULL;
            if (dim < BOOT_RENDER_DIMS) boot_render_null_srv(r, dim, i);
            if (dim != BOOT_DIM_BUFFER) boot_render_tex_pool_put(res);   /* shader-resource state, idle */
            else ID3D12Resource_Release(res);
            resident -= bytes; --count; ++evicted; boot_render_tex_evicted_bytes += bytes;
            if (boot_sd_on()) boot_sd_tex_evict(i, 0);   /* suspect-diag S4 */
        }
        boot_render_tex_evictions += evicted;
    }
    free(cand);
    boot_render_tex_resident = resident; boot_render_tex_resident_count = count;
    if (resident > boot_render_tex_resident_peak) boot_render_tex_resident_peak = resident;
    boot_bc7_idle(r);   /* bc7-work: queue idle: finished encodes switch their slots to BC7; new encodes within the frame budget */
    ++boot_render_tex_clock;
    return evicted;
}

/* mem-work (texture lane): the texture budget between idle points. boot_render_tex_trim runs only with the queue idle
 * and no list open. On the native path with frame overlap that is rare: play-96 (island fall) went 1500 submissions
 * without one while the textures grew from 4883 to 7531 MiB against TOTK_TEX_BUDGET_MB=4096, past the 7180 MiB local
 * budget DXGI gives an 8 GB GPU (beyond it Windows demotes allocations to system RAM), and its clock (one tick per trim)
 * made "not used in the last two clocks" protect everything used since the trim before last. Every D3D12 texture is
 * also charged 1:1 to process private bytes and system commit (measured: +256 MiB DEFAULT-heap texture = +256 MiB
 * private, +0 working set).
 * TOTK_TEX_LIVE_TRIM=1: at the end of every native submission (after the kick, the new list open and still empty) the
 * valid slots are recounted from the live slot list (a resource shared by several slots once) and, beyond the budget,
 * the least recently used slot textures are released down to 7/8 of it as boot_render_tex_trim releases them (the
 * descriptor nulled, the slot rebuilt from guest memory at its next use; the texture kept by the pool when it has a
 * free entry, else released by a worker thread, see boot_render_rel_queue), but only a slot
 * whose last binding list (bound and recorded) completed on the GPU, that the open list does not use (nothing pending),
 * that is no feedback slot or view, not versioned, and not used in the last TOTK_TEX_MIN_AGE clock ticks (default 4).
 * The clock then ticks once per submission (~one per present in play), so the age is in frames, not in idle points.
 * The 2D-array view a 2D slot also carries (boot_render_tex_isolate_srv) is nulled with it. Unset / 0: nothing below
 * runs; boot_render_tex_trim at the idle points only, as before. */
static inline int boot_render_tex_env_flag(const char *name) {
    char e[8]; DWORD n = GetEnvironmentVariableA(name, e, sizeof e);
    return n && n < sizeof e && e[0] == '1';
}
static inline int boot_render_tex_live_trim_on(void) { static int v = -1; if (v < 0) v = boot_render_tex_env_flag("TOTK_TEX_LIVE_TRIM"); return v; }
static inline int boot_render_tex_census_on(void) { static int v = -1; if (v < 0) v = boot_render_tex_env_flag("TOTK_TEX_CENSUS"); return v; }
static inline uint64_t boot_render_tex_budget(void) {   /* TOTK_TEX_BUDGET_MB, parsed as the idle trims parse it */
    static uint64_t budget;
    if (!budget) {
        char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_BUDGET_MB", v, sizeof v);
        unsigned long long mb = (n && n < sizeof v) ? strtoull(v, NULL, 10) : 0;
        budget = mb >= 256 ? (uint64_t)mb << 20 : BOOT_RENDER_TEX_BUDGET;
    }
    return budget;
}
static inline uint32_t boot_render_tex_min_age(void) {
    static uint32_t a;
    if (!a) {
        char v[16]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_MIN_AGE", v, sizeof v);
        unsigned long x = (n && n < sizeof v) ? strtoul(v, NULL, 10) : 0;
        a = x >= 2 && x <= 1000000 ? (uint32_t)x : 4;
    }
    return a;
}
/* Bytes of slot i's resource from the size cache, else estimated (no device call: the census runs without a BootRender). */
static inline uint64_t boot_render_tex_bytes_cached(uint32_t i, ID3D12Resource *res) {
    D3D12_RESOURCE_DESC d; res->lpVtbl->GetDesc(res, &d);
    if (boot_render_tex_sized[i].res == res && boot_render_tex_sized[i].width == d.Width && boot_render_tex_sized[i].height == d.Height &&
        boot_render_tex_sized[i].depth == d.DepthOrArraySize && boot_render_tex_sized[i].mips == d.MipLevels && boot_render_tex_sized[i].format == (uint32_t)d.Format)
        return boot_render_tex_sized[i].bytes;
    return d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? (uint64_t)d.Width : boot_render_tex_estimate(&d);
}
/* The unique resources of the valid slots: an open-addressed table keyed by the resource pointer (the texture dedup
 * shares one resource between slots), rebuilt by every fill. age: clock ticks since the youngest holder's last use. */
typedef struct BootRenderTexSeen { ID3D12Resource *res; uint64_t bytes, va, size; uint32_t age, holders; uint8_t astc; } BootRenderTexSeen;
static BootRenderTexSeen *boot_render_tex_seen; static uint32_t boot_render_tex_seen_cap, boot_render_tex_seen_used, boot_render_tex_seen_n;
/* Fills the table from the live slot list (an invalid entry leaves the list, as in the range scans: the list keeps every
 * valid slot) and returns the resident bytes, each resource once (UINT64_MAX: no memory for the table); *count: slots
 * holding a resource. r NULL: cached or estimated sizes only. */
static inline uint64_t boot_render_tex_seen_fill(BootRender *r, uint64_t *count) {
    uint64_t resident = 0, slots = 0; uint32_t want = 1024;
    *count = 0; boot_render_tex_seen_n = 0;
    while (want < 2u * boot_render_tex_live_n + 2u) want <<= 1;
    if (want > boot_render_tex_seen_cap) {
        BootRenderTexSeen *t = (BootRenderTexSeen *)realloc(boot_render_tex_seen, (size_t)want * sizeof *t);
        if (!t) return UINT64_MAX;
        boot_render_tex_seen = t; boot_render_tex_seen_cap = want;
    }
    boot_render_tex_seen_used = want; memset(boot_render_tex_seen, 0, (size_t)want * sizeof *boot_render_tex_seen);
    const uint32_t mask = want - 1;
    for (uint32_t k = 0; k < boot_render_tex_live_n; ) {
        uint32_t i = boot_render_tex_live[k]; const BootRenderTexSlot *s = &boot_render_tex_slots[i];
        if (!s->valid) { boot_render_tex_live_in[i] = 0; boot_render_tex_live[k] = boot_render_tex_live[--boot_render_tex_live_n]; continue; }
        ++k;
        if (!s->res) continue;
        ++slots;
        const uint32_t age = boot_render_tex_clock - boot_render_tex_last[i];
        uint32_t h = (uint32_t)((((uint64_t)(uintptr_t)s->res >> 4) * UINT64_C(0x9E3779B97F4A7C15)) >> 40) & mask;
        while (boot_render_tex_seen[h].res && boot_render_tex_seen[h].res != s->res) h = (h + 1) & mask;
        BootRenderTexSeen *e = &boot_render_tex_seen[h];
        if (e->res) { ++e->holders; if (age < e->age) e->age = age; continue; }
        e->res = s->res; e->bytes = r ? boot_render_tex_slot_bytes(r, i, s->res) : boot_render_tex_bytes_cached(i, s->res);
        e->va = s->va; e->size = s->size; e->age = age; e->holders = 1; e->astc = boot_render_tex_astc[i];
        ++boot_render_tex_seen_n; resident += e->bytes;
    }
    *count = slots;
    return resident;
}
/* Texture releases off the render thread. A committed texture's Release costs 0.2-0.5 ms here (measured on this
 * RTX 4070 Laptop, game closed: 64x64 0.23 ms, 2048x2048 0.51 ms; the trim test: 375 evictions, 110 ms on the calling
 * thread, nearly all of it in the pool's releases of what it could not keep). The live trim hands a texture whose last
 * list completed and whose descriptors are nulled to a below-normal-priority worker (D3D12 objects are free-threaded);
 * ring full or no worker: released on the spot. */
#define BOOT_RENDER_REL_RING 16384u
static struct { SRWLOCK lock; HANDLE event; int ok, tried; uint32_t head, tail; uint64_t queued, released, inline_releases; ID3D12Resource *ring[BOOT_RENDER_REL_RING]; } boot_render_rel;
static DWORD WINAPI boot_render_rel_main(void *arg) {
    (void)arg;
    for (;;) {
        if (WaitForSingleObject(boot_render_rel.event, INFINITE) != WAIT_OBJECT_0) return 0;
        for (;;) {
            ID3D12Resource *t = NULL;
            AcquireSRWLockExclusive(&boot_render_rel.lock);
            if (boot_render_rel.tail != boot_render_rel.head) { t = boot_render_rel.ring[boot_render_rel.tail % BOOT_RENDER_REL_RING]; ++boot_render_rel.tail; }
            ReleaseSRWLockExclusive(&boot_render_rel.lock);
            if (!t) break;
            ID3D12Resource_Release(t);
            AcquireSRWLockExclusive(&boot_render_rel.lock); ++boot_render_rel.released; ReleaseSRWLockExclusive(&boot_render_rel.lock);
        }
    }
}
static inline void boot_render_rel_queue(ID3D12Resource *t) {   /* t: idle, no descriptor left; one reference handed over */
    if (!boot_render_rel.tried) {
        boot_render_rel.tried = 1;   /* the static SRWLOCK is zero: initialised */
        boot_render_rel.event = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE th = boot_render_rel.event ? CreateThread(NULL, 0, boot_render_rel_main, NULL, 0, NULL) : NULL;
        if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); boot_render_rel.ok = 1; }
    }
    int queued = 0;
    if (boot_render_rel.ok) {
        AcquireSRWLockExclusive(&boot_render_rel.lock);
        if (boot_render_rel.head - boot_render_rel.tail < BOOT_RENDER_REL_RING) { boot_render_rel.ring[boot_render_rel.head % BOOT_RENDER_REL_RING] = t; ++boot_render_rel.head; queued = 1; }
        ReleaseSRWLockExclusive(&boot_render_rel.lock);
    }
    if (queued) ++boot_render_rel.queued; else { ID3D12Resource_Release(t); ++boot_render_rel.inline_releases; }
}
/* The texture pool without its drops: kept only when an entry is free and the bytes fit (pool_put would release the oldest
 * entries on this thread to make room). 1: kept. */
static inline int boot_render_tex_pool_keep(ID3D12Resource *t) {
    D3D12_RESOURCE_DESC d; t->lpVtbl->GetDesc(t, &d);
    const uint64_t bytes = boot_render_tex_estimate(&d);
    if (bytes > BOOT_RENDER_TEX_POOL_BYTES / 4 || boot_render_tex_pool_bytes + bytes > BOOT_RENDER_TEX_POOL_BYTES) return 0;
    for (unsigned k = 0; k < BOOT_RENDER_TEX_POOL; ++k) if (!boot_render_tex_pool[k].res) {
        boot_render_tex_pool[k].res = t; boot_render_tex_pool[k].bytes = bytes; boot_render_tex_pool[k].age = ++boot_render_tex_pool_clock;
        boot_render_tex_pool_bytes += bytes; return 1;
    }
    return 0;
}
static uint64_t boot_render_tex_lt_runs, boot_render_tex_lt_skips, boot_render_tex_lt_evictions, boot_render_tex_lt_bytes, boot_render_tex_lt_short, boot_render_tex_lt_qpc, boot_render_tex_lt_max_qpc;
static uint64_t boot_render_tex_lt_marks = UINT64_MAX;
static uint32_t *boot_render_tex_lt_cand; static uint32_t boot_render_tex_lt_cap;
/* End of a native submission with TOTK_TEX_LIVE_TRIM=1 (see above). No slot built since the last call and the resident
 * bytes within the budget: only the clock ticks (nothing can have grown). Returns the number of evicted slots. */
static inline unsigned boot_render_tex_trim_live(BootRender *r, uint64_t budget) {
    if (!boot_render_tex_slots) return 0;
    if (boot_render_tex_lt_marks == boot_render_tex_marks && boot_render_tex_resident <= budget) { ++boot_render_tex_lt_skips; ++boot_render_tex_clock; return 0; }
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    ++boot_render_tex_lt_runs; boot_render_tex_lt_marks = boot_render_tex_marks;
    uint64_t count = 0; const uint64_t found = boot_render_tex_seen_fill(r, &count); uint64_t resident = found; unsigned evicted = 0, queued = 0;
    if (found == UINT64_MAX) { boot_render_tex_lt_marks = UINT64_MAX; ++boot_render_tex_clock; return 0; }   /* no memory for the table: nothing decided */
    if (found > boot_render_tex_resident_peak) boot_render_tex_resident_peak = found;   /* before evictions: the bytes that existed */
    if (found > budget && !boot_render_tex_pending_n && !boot_render_tex_pending_over) {
        if (boot_render_tex_lt_cap < boot_render_tex_live_n) {
            uint32_t *c = (uint32_t *)realloc(boot_render_tex_lt_cand, (size_t)boot_render_tex_live_n * sizeof *c);
            if (c) { boot_render_tex_lt_cand = c; boot_render_tex_lt_cap = boot_render_tex_live_n; }
        }
        uint32_t n = 0; const uint32_t min_age = boot_render_tex_min_age();
        for (uint32_t k = 0; k < boot_render_tex_live_n && n < boot_render_tex_lt_cap; ++k) {
            const uint32_t i = boot_render_tex_live[k]; const BootRenderTexSlot *s = &boot_render_tex_slots[i];
            if (!s->valid || !s->res || i >= BOOT_RENDER_TEX_SLOTS - 256u || boot_render_fb_on[i] || boot_render_sv_flag[i]) continue;
            if (boot_render_tex_clock - boot_render_tex_last[i] < min_age) continue;
            if (!boot_render_list_done(r, boot_render_slot_list[i]) || !boot_render_list_done(r, boot_render_slot_rec[i])) continue;
            boot_render_tex_lt_cand[n++] = i;
        }
        if (n) qsort(boot_render_tex_lt_cand, n, sizeof *boot_render_tex_lt_cand, boot_render_tex_age_cmp);
        const uint64_t target = budget - budget / 8;
        for (uint32_t k = 0; k < n && resident > target; ++k) {
            const uint32_t i = boot_render_tex_lt_cand[k]; BootRenderTexSlot *s = &boot_render_tex_slots[i];
            const int shared = boot_render_res_refs(s->res) > 1;
            uint64_t bytes = shared ? 0 : boot_render_tex_slot_bytes(r, i, s->res);   /* shared: freed with its last holder */
            ID3D12Resource *res = s->res; unsigned dim = s->dim;
            s->res = NULL; s->valid = 0; s->view_of = NULL; s->size = 0;
            boot_render_texmemo[i] = 0;
            boot_render_tex_sized[i].res = NULL;
            if (dim < BOOT_RENDER_DIMS) boot_render_null_srv(r, dim, i);
            if (dim == BOOT_DIM_2D) boot_render_null_srv(r, BOOT_DIM_2D_ARRAY, i);
            if (shared) ID3D12Resource_Release(res);   /* another slot's image: only this slot's reference */
            else if (dim == BOOT_DIM_BUFFER || !boot_render_tex_pool_keep(res)) { boot_render_rel_queue(res); ++queued; }
            resident = bytes < resident ? resident - bytes : 0; if (count) --count;
            ++evicted; boot_render_tex_evicted_bytes += bytes; boot_render_tex_lt_bytes += bytes;
            if (boot_sd_on()) boot_sd_tex_evict(i, 1);   /* suspect-diag S4 */
        }
        if (queued && boot_render_rel.ok) SetEvent(boot_render_rel.event);
        if (resident > target) ++boot_render_tex_lt_short;   /* the recent working set alone is over 7/8 of the budget */
        boot_render_tex_evictions += evicted; boot_render_tex_lt_evictions += evicted;
    }
    boot_render_tex_resident = resident; boot_render_tex_resident_count = count;
    ++boot_render_tex_clock;
    QueryPerformanceCounter(&q1);
    const uint64_t dq = (uint64_t)(q1.QuadPart - q0.QuadPart); boot_render_tex_lt_qpc += dq; if (dq > boot_render_tex_lt_max_qpc) boot_render_tex_lt_max_qpc = dq;
    return evicted;
}

/* nvnpar-work: helper threads for independent per-item work of the render thread (boot_render_par_for). The items
 * [0, n) are taken in index order by the caller and the helpers; the caller returns when every helper has stopped (no
 * helper outlives the call). fn may only read shared state and write its own item. TOTK_RENDER_HELPERS=n helpers (0-7,
 * default 3; 0: everything on the calling thread). */
typedef void (*BootRenderParFn)(void *ctx, unsigned i);
static struct {
    volatile LONG started; unsigned helpers; HANDLE go[8], done;
    BootRenderParFn fn; void *ctx; volatile LONG next, n, active; uint64_t jobs, items;
} boot_render_par;
static DWORD WINAPI boot_render_par_main(void *arg) {
    unsigned k = (unsigned)(uintptr_t)arg;
    for (;;) {
        if (WaitForSingleObject(boot_render_par.go[k], INFINITE) != WAIT_OBJECT_0) return 0;
        for (;;) { LONG i = InterlockedIncrement(&boot_render_par.next) - 1; if (i >= boot_render_par.n) break; boot_render_par.fn(boot_render_par.ctx, (unsigned)i); }
        if (!InterlockedDecrement(&boot_render_par.active)) SetEvent(boot_render_par.done);
    }
}
static inline unsigned boot_render_par_helpers(void) {
    if (boot_render_par.started) return boot_render_par.helpers;
    boot_render_par.started = 1;
    char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_RENDER_HELPERS", v, sizeof v);
    unsigned want = n && n < sizeof v ? (unsigned)strtoul(v, NULL, 10) : 3; if (want > 7) want = 7;
    if (!want || !(boot_render_par.done = CreateEventW(NULL, TRUE, FALSE, NULL))) return boot_render_par.helpers = 0;
    unsigned k = 0;
    for (; k < want; ++k) {
        if (!(boot_render_par.go[k] = CreateEventW(NULL, FALSE, FALSE, NULL))) break;
        HANDLE t = CreateThread(NULL, 0, boot_render_par_main, (void *)(uintptr_t)k, 0, NULL);
        if (!t) { CloseHandle(boot_render_par.go[k]); boot_render_par.go[k] = NULL; break; }
        SetThreadPriority(t, GetThreadPriority(GetCurrentThread()));   /* the render thread waits for them: same priority */
        CloseHandle(t);
    }
    return boot_render_par.helpers = k;
}
static inline void boot_render_par_for(unsigned n, BootRenderParFn fn, void *ctx) {
    unsigned h = n > 1 ? boot_render_par_helpers() : 0;
    ++boot_render_par.jobs; boot_render_par.items += n;
    if (!h) { for (unsigned i = 0; i < n; ++i) fn(ctx, i); return; }
    boot_render_par.fn = fn; boot_render_par.ctx = ctx; boot_render_par.n = (LONG)n; boot_render_par.active = (LONG)h;
    ResetEvent(boot_render_par.done);
    InterlockedExchange(&boot_render_par.next, 0);   /* full barrier: the job is visible before any helper starts */
    for (unsigned k = 0; k < h; ++k) SetEvent(boot_render_par.go[k]);
    for (;;) { LONG i = InterlockedIncrement(&boot_render_par.next) - 1; if (i >= (LONG)n) break; fn(ctx, (unsigned)i); }
    WaitForSingleObject(boot_render_par.done, INFINITE);
}
/* nvnpar-work: the re-hash with the checksums taken by boot_render_par_for. Exactly the slots and bytes of the loop
 * below, chosen in its order on this thread (eligibility, the guest pointer and its guard, the byte budget and the
 * cursor are the same); only the checksums run on the helpers, then the stale ones are marked in that order. Marking
 * later than the loop does changes nothing it decides: a slot's mark only excludes that slot, which is visited once. */
typedef struct { uint32_t index; const uint8_t *p; uint64_t size, sum; } BootRenderVerifyItem;
static void boot_render_tex_verify_hash(void *ctx, unsigned i) { BootRenderVerifyItem *it = (BootRenderVerifyItem *)ctx + i; it->sum = boot_render_tex_checksum(it->p, it->size); }
static inline unsigned boot_render_tex_verify_par(BootRender *r, uint64_t budget) {
    static BootRenderVerifyItem *items; static uint32_t cap;
    uint64_t done = 0; unsigned stale = 0, visited = 0, high = boot_render_tex_high, n = 0;
    uint32_t i = boot_render_tex_verify_cursor % high;
    while (visited < high && done < budget) {
        BootRenderTexSlot *slot = &boot_render_tex_slots[i];
        /* render-39: already marked for a rebuild, or watched and recently checked at its use: not re-hashed */
        if (slot->valid && slot->size && !slot->view_of && !boot_render_tex_poisoned[i] && !boot_render_tex_watch_fresh(i)) {
            const uint8_t *p = boot_render_guest(r, slot->va, slot->size);
            if (p) {
                done += slot->size;
                if (n == cap) {
                    uint32_t c2 = cap ? cap * 2 : 1024; BootRenderVerifyItem *g = (BootRenderVerifyItem *)realloc(items, (size_t)c2 * sizeof *g);
                    if (g) { items = g; cap = c2; }
                }
                if (n < cap) { items[n].index = i; items[n].p = p; items[n].size = slot->size; items[n].sum = 0; ++n; }
                else if (boot_render_tex_checksum(p, slot->size) != boot_render_tex_sum[i]) { boot_render_tex_poison(i); boot_render_tex_watch_want[i] = 1; ++stale; }   /* no room: here */
            }
        }
        ++visited; i = (i + 1) % high;
    }
    boot_render_par_for(n, boot_render_tex_verify_hash, items);
    for (unsigned k = 0; k < n; ++k)
        if (items[k].sum != boot_render_tex_sum[items[k].index]) { boot_render_tex_poison(items[k].index); boot_render_tex_watch_want[items[k].index] = 1; ++stale; }
    boot_render_tex_verify_cursor = i; boot_render_tex_verified_bytes += done; boot_render_tex_stale_rebuilds += stale; boot_ax_live(BOOT_AX_L_REHASH, done);   /* async-xlate: counted on X */
    return stale;
}
/* End of a guest submission (after boot_render_tex_trim): re-hash built slots round-robin until
 * `budget` bytes were read; a slot whose guest bytes changed since its build is marked so its next
 * use rebuilds it (the same TIC-word mismatch boot_render_invalidate_textures uses). Returns the
 * number of slots found stale. Zero-copy buffer views read guest memory directly and are skipped. */
static inline unsigned boot_render_tex_verify(BootRender *r, uint64_t budget) {
    if (!boot_render_tex_slots || !budget || !boot_render_tex_high) return 0;
    if (boot_render_par_helpers()) return boot_render_tex_verify_par(r, budget);   /* nvnpar-work */
    uint64_t done = 0; unsigned stale = 0, visited = 0, high = boot_render_tex_high;
    uint32_t i = boot_render_tex_verify_cursor % high;
    while (visited < high && done < budget) {
        BootRenderTexSlot *slot = &boot_render_tex_slots[i];
        /* render-39: already marked for a rebuild, or watched and recently checked at its use: not re-hashed */
        if (slot->valid && slot->size && !slot->view_of && !boot_render_tex_poisoned[i] && !boot_render_tex_watch_fresh(i)) {
            const uint8_t *p = boot_render_guest(r, slot->va, slot->size);
            if (p) {
                done += slot->size;
                if (boot_render_tex_checksum(p, slot->size) != boot_render_tex_sum[i]) { boot_render_tex_poison(i); boot_render_tex_watch_want[i] = 1; ++stale; }
            }
        }
        ++visited; i = (i + 1) % high;
    }
    boot_render_tex_verify_cursor = i; boot_render_tex_verified_bytes += done; boot_render_tex_stale_rebuilds += stale; boot_ax_live(BOOT_AX_L_REHASH, done);   /* async-xlate: counted on X */
    return stale;
}
static inline D3D12_TEXTURE_ADDRESS_MODE boot_render_address(uint32_t a) {
    switch (a) {   /* TEXSAMP ADDRESS_*: WRAP, MIRROR, CLAMP_TO_EDGE, BORDER, CLAMP_OGL, MIRROR_ONCE_* */
        case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case 5: case 6: case 7: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;   /* CLAMP_TO_EDGE; CLAMP_OGL approximated */
    }
}
static inline int boot_render_sampler_resolve(BootRender *r, uint64_t pool, uint32_t index);
static inline int boot_render_sampler(BootRender *r, uint64_t pool, uint32_t index) {
    if (index < BOOT_RENDER_SAMPLERS) {   /* nvnpar-work: remembered unchanged samplers (boot_render_texmemo) */
        uint64_t k = boot_render_texmemo_key(pool);
        if (k && boot_render_smpmemo[index] == k && boot_render_texmemo_on()) {
            if (!boot_render_texmemo_verify_on()) { ++boot_render_host_hits; ++boot_render_smpmemo_hits; return BOOT_RENDER_OK; }
            boot_render_smpmemo[index] = 0; boot_render_texmemo_pure = 0; ++boot_render_texmemo_verified;
            int rc = boot_render_sampler_resolve(r, pool, index);
            if (!boot_render_texmemo_pure && ++boot_render_texmemo_bad <= 16) {
                FILE *lf = fopen("draw-path.log", "a");
                if (lf) { fprintf(lf, "texset cache verify: TSC %u of pool %llx was remembered but its resolution changed something (rc %d, submission %llu)\n",
                                  index, (unsigned long long)pool, rc, (unsigned long long)boot_render_exec_id); fclose(lf); }
            }
            return rc;
        }
        boot_render_smpmemo[index] = 0;
    }
    return boot_render_sampler_resolve(r, pool, index);
}
static inline int boot_render_sampler_resolve(BootRender *r, uint64_t pool, uint32_t index) {
    if (index >= BOOT_RENDER_SAMPLERS) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "TSC index beyond host sampler table");
    ++boot_render_smpmemo_resolves; boot_render_texmemo_pure = 0;
    const uint32_t *words = boot_render_desc_words(r, pool, index, 1);
    if (!words) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "TSC entry outside guest memory");
    BootRenderTsSlot *slot = &boot_render_ts_slots[index];
    if (slot->valid && !memcmp(slot->words, words, 32)) {
        if (boot_render_texmemo_host(1, pool, index) && boot_render_texmemo_key(pool)) {   /* nvnpar-work: remembered */
            boot_render_texmemo_pure = 1;
            if (boot_render_texmemo_on()) { boot_render_smpmemo[index] = boot_render_texmemo_key(pool); ++boot_render_smpmemo_sets; }
        }
        return BOOT_RENDER_OK;
    }
    if (slot->valid && r->recording) { if (!boot_render_submit(r) || !boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "flush"); }
    BootTsc s; boot_render_tsc_decode_any(words, &s);
    D3D12_SAMPLER_DESC d = {0};
    int lin_min = s.min_filter >= 2, lin_mag = s.mag_filter == 2, lin_mip = s.mip_filter == 3;
    d.Filter = (D3D12_FILTER)((lin_min ? 0x10 : 0) | (lin_mag ? 0x4 : 0) | (lin_mip ? 0x1 : 0));
    if (s.min_filter == 3 && s.max_aniso) d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.AddressU = boot_render_address(s.address[0]); d.AddressV = boot_render_address(s.address[1]); d.AddressW = boot_render_address(s.address[2]);
    static const UINT aniso[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    d.MaxAnisotropy = aniso[s.max_aniso & 7];
    d.MipLODBias = s.lod_bias; d.MinLOD = s.min_lod; d.MaxLOD = s.mip_filter == 1 ? 0.0f : s.max_lod;
    for (unsigned i = 0; i < 4; ++i) d.BorderColor[i] = s.border[i];
    d.ComparisonFunc = (D3D12_COMPARISON_FUNC)(s.compare_func + 1);
    ID3D12Device_CreateSampler(r->gpu->device, &d, boot_render_cpu(r, r->sampler_heap, r->sampler_inc, index));
    D3D12_SAMPLER_DESC c = d; c.Filter = (D3D12_FILTER)(d.Filter | 0x80);   /* comparison variant */
    if (d.Filter == D3D12_FILTER_ANISOTROPIC) c.Filter = D3D12_FILTER_COMPARISON_ANISOTROPIC;
    ID3D12Device_CreateSampler(r->gpu->device, &c, boot_render_cpu(r, r->sampler_heap, r->sampler_inc, BOOT_RENDER_SAMPLERS + index));
    memcpy(slot->words, words, 32); slot->valid = 1;
    return BOOT_RENDER_OK;
}

/* Draw-time inputs not held in the register file. */
typedef struct BootRenderDraw {
    const BootDrawState *state;
    BootCbufBinding cbufs[5][18];    /* per pipeline bind group (0 vertex, 1 tess control, 2 tess eval, 3 geometry, 4 pixel), latched at bind time */
    uint32_t host_state[8];          /* HostState cbuffer words (maxwell_runtime.hlsli) */
    uint32_t guest_barrier;          /* a guest memory barrier (WFI / shader cache invalidate) precedes this draw */
} BootRenderDraw;

/* ---------------------------------------------------------------- indirect commands (native NVN path)
 * render-38d: nvnCommandBufferDrawElementsIndirect / DrawArraysIndirect / DispatchComputeIndirect executed with
 * D3D12 ExecuteIndirect on arguments the GPU reads, replacing the CPU read of the argument structure
 * (host_nvn_play.h boot_nvn_exec_indirect), which first executed the open list and waited for the whole queue
 * (boot_render_idle) at every indirect command: the arguments are written by the game's own GPU work (GPU-driven
 * culling), so a CPU read is only exact once everything recorded before has completed.
 *
 * Why our draw path needed the values on the CPU, and what replaces each use:
 *   - count, instance count, first index: only passed to DrawIndexedInstanced / DrawInstanced and used to size the
 *     index buffer view (index address + first * size, count * size). D3D12's DRAW / DRAW_INDEXED argument
 *     structures have NVN's layout ({count, instances, first[, base vertex], base instance}); the view starts at the
 *     index address and covers the contiguous guest region bytes behind it (at most BOOT_RENDER_IND_IB_MAX; reads
 *     past a view return 0 in D3D12, never fault), and StartIndexLocation selects the first index on the GPU.
 *   - first vertex, base vertex, base instance: translated vertex shaders pull vertices themselves and add
 *     HostState host_vertex_base (first vertex of a non-indexed draw, base vertex of an indexed one) and
 *     host_instance_base to SV_VertexID / SV_InstanceID (host_gpu_render_draw.h host[4]/host[5]). D3D12's
 *     StartVertexLocation / BaseVertexLocation / StartInstanceLocation are NOT added to SV_VertexID / SV_InstanceID
 *     (DXVK and vkd3d-proton both subtract gl_BaseVertex / gl_BaseInstance to emulate exactly that), and nothing is
 *     fetched through the input assembler, so those argument words are written 0 and the two values reach the shader
 *     through HostState instead: the draw's b18 is a GPU-written copy (below), not a ring snapshot.
 *   - dispatch group counts: the translated compute shaders read the grid from the driver constant buffer c0 at
 *     0x200 (host_nvn_play.h boot_nvn_exec_dispatch); the dispatch's c0 is likewise a GPU-written copy.
 *   - instancing: the default path is already one host draw for all instances (TOTK_INSTANCE_EXPAND=1, one host
 *     draw per instance, needs the count on the CPU and falls back); the constant snapshots, ring sizing and the
 *     census never needed the argument values (the census keys GPU-argument draws with first 0, count 1).
 *   - CPU index work (quads/fans/polygons/line loops expanded, 8-bit indices widened) needs first/count on the CPU:
 *     those draws keep the CPU path (counted). So do arguments not 4-byte aligned or not contiguous in one region.
 *
 * Per indirect command, recorded right before it (never predicated: render-38c only brackets the draw call):
 * region buffers written by GPU copies go back to COMMON (boot_render_gw_settle), a global UAV barrier orders the
 * read after earlier compute and storing draws (as render-38c's predicate dispatch), the indirect ring goes
 * UNORDERED_ACCESS, one 64-thread group (indirect-shaders/indirect.hlsl, DXIL in the generated block below) reads the
 * NVN structure from the zero-copy region and writes the D3D12 argument structure plus the patched constant buffer
 * copy into the command's ring slot, and the ring goes INDIRECT_ARGUMENT | VERTEX_AND_CONSTANT_BUFFER for the
 * ExecuteIndirect (MaxCommandCount 1, no count buffer) and its CBV. Its state is tracked per list like the predicate
 * buffer's (COMMON at list begin, back to COMMON before close). No value is read back; nothing is decoded on the CPU;
 * every command the game issued is executed (a zero count draws nothing on the GPU, as on the source GPU).
 *
 * Limits: dispatch group counts are clamped to 65535 per dimension (the CPU path splits larger grids into several
 * dispatches; ExecuteIndirect cannot, and is undefined beyond it). TOTK_INDIRECT=cpu restores the CPU read for
 * every indirect command (A/B). Counters: host_nvn_play.h's periodic "indirect" line. */
/* BEGIN render-38d indirect DXIL: generated by indirect-shaders/build.sh from indirect.hlsl (DXC, cs_6_0), do not edit */
/* indirect.cs.dxil: 4288 bytes, sha256 60de704587dd3c8aeeaab5457be6e1ad74f54a35bca5bf6692ab031ff2f27b46 */
static const uint8_t boot_gpu_indirect_dxil[4288] = {
    0x44,0x58,0x42,0x43,0xd6,0xb3,0xfe,0x94,0xe5,0x17,0xae,0x0d,0xe1,0x3b,0x26,0xf7,0x01,0xd0,0x32,0x21,0x01,0x00,0x00,0x00,
    0xc0,0x10,0x00,0x00,0x07,0x00,0x00,0x00,0x3c,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x5c,0x00,0x00,0x00,0x6c,0x00,0x00,0x00,
    0x2c,0x01,0x00,0x00,0x18,0x09,0x00,0x00,0x34,0x09,0x00,0x00,0x53,0x46,0x49,0x30,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x49,0x53,0x47,0x31,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x4f,0x53,0x47,0x31,
    0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x50,0x53,0x56,0x30,0xb8,0x00,0x00,0x00,0x34,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
    0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x63,0x73,0x5f,0x69,0x6e,0x64,0x69,
    0x72,0x65,0x63,0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x53,0x54,0x41,0x54,0xe4,0x07,0x00,0x00,0x60,0x00,0x05,0x00,
    0xf9,0x01,0x00,0x00,0x44,0x58,0x49,0x4c,0x00,0x01,0x00,0x00,0x10,0x00,0x00,0x00,0xcc,0x07,0x00,0x00,0x42,0x43,0xc0,0xde,
    0x21,0x0c,0x00,0x00,0xf0,0x01,0x00,0x00,0x0b,0x82,0x20,0x00,0x02,0x00,0x00,0x00,0x13,0x00,0x00,0x00,0x07,0x81,0x23,0x91,
    0x41,0xc8,0x04,0x49,0x06,0x10,0x32,0x39,0x92,0x01,0x84,0x0c,0x25,0x05,0x08,0x19,0x1e,0x04,0x8b,0x62,0x80,0x14,0x45,0x02,
    0x42,0x92,0x0b,0x42,0xa4,0x10,0x32,0x14,0x38,0x08,0x18,0x4b,0x0a,0x32,0x52,0x88,0x48,0x90,0x14,0x20,0x43,0x46,0x88,0xa5,
    0x00,0x19,0x32,0x42,0xe4,0x48,0x0e,0x90,0x91,0x22,0xc4,0x50,0x41,0x51,0x81,0x8c,0xe1,0x83,0xe5,0x8a,0x04,0x29,0x46,0x06,
    0x51,0x18,0x00,0x00,0x08,0x00,0x00,0x00,0x1b,0x8c,0xe0,0xff,0xff,0xff,0xff,0x07,0x40,0x02,0xa8,0x0d,0x86,0xf0,0xff,0xff,
    0xff,0xff,0x03,0x20,0x01,0xd5,0x06,0x62,0xf8,0xff,0xff,0xff,0xff,0x01,0x90,0x00,0x49,0x18,0x00,0x00,0x03,0x00,0x00,0x00,
    0x13,0x82,0x60,0x42,0x20,0x4c,0x08,0x06,0x00,0x00,0x00,0x00,0x89,0x20,0x00,0x00,0x34,0x00,0x00,0x00,0x32,0x22,0x48,0x09,
    0x20,0x64,0x85,0x04,0x93,0x22,0xa4,0x84,0x04,0x93,0x22,0xe3,0x84,0xa1,0x90,0x14,0x12,0x4c,0x8a,0x8c,0x0b,0x84,0xa4,0x4c,
    0x10,0x78,0x23,0x00,0x25,0x00,0x14,0xe6,0x08,0xc0,0xa0,0x0c,0x63,0x0c,0x22,0x85,0x18,0x63,0x0c,0x32,0x47,0x0d,0x97,0x3f,
    0x61,0x0f,0x21,0xf9,0xdc,0x46,0x15,0x2b,0x31,0xf9,0xc8,0x6d,0x23,0x62,0x8c,0x31,0xe6,0x08,0x10,0x4a,0xf7,0x0c,0x97,0x3f,
    0x61,0x0f,0x21,0xf9,0x21,0xd0,0x0c,0x0b,0x81,0x02,0x55,0x08,0x34,0xd6,0x20,0x36,0x47,0x10,0x14,0x63,0x8d,0x34,0x86,0xa3,
    0x57,0x14,0x30,0xd6,0x18,0x63,0x8c,0x91,0x28,0xde,0x34,0x5c,0xfe,0x84,0x3d,0x84,0xe4,0xaf,0x84,0xb4,0x12,0x93,0x8f,0xdc,
    0x36,0x2a,0xc6,0x18,0x63,0x94,0x62,0x8e,0x35,0x06,0xd1,0x81,0x80,0xc3,0xa4,0x29,0xa2,0x84,0xc9,0xdf,0xb0,0x89,0xd0,0x86,
    0x21,0x22,0x24,0x69,0xa3,0x8a,0x82,0x88,0x50,0x30,0xe8,0x9e,0x26,0x4d,0x11,0x25,0x4c,0xfe,0x0a,0x6f,0xd8,0x44,0x68,0xc3,
    0x10,0x11,0x92,0xb4,0x51,0x45,0x41,0x44,0x28,0x18,0x94,0x0f,0x11,0x9b,0x01,0x89,0x08,0x61,0x42,0xd0,0x18,0x63,0x8c,0x31,
    0xc6,0x18,0x63,0x8c,0x31,0x06,0xed,0x39,0x02,0x50,0x00,0x00,0x13,0x14,0x72,0xc0,0x87,0x74,0x60,0x87,0x36,0x68,0x87,0x79,
    0x68,0x03,0x72,0xc0,0x87,0x0d,0xaf,0x50,0x0e,0x6d,0xd0,0x0e,0x7a,0x50,0x0e,0x6d,0x00,0x0f,0x7a,0x30,0x07,0x72,0xa0,0x07,
    0x73,0x20,0x07,0x6d,0x90,0x0e,0x71,0xa0,0x07,0x73,0x20,0x07,0x6d,0x90,0x0e,0x78,0xa0,0x07,0x73,0x20,0x07,0x6d,0x90,0x0e,
    0x71,0x60,0x07,0x7a,0x30,0x07,0x72,0xd0,0x06,0xe9,0x30,0x07,0x72,0xa0,0x07,0x73,0x20,0x07,0x6d,0x90,0x0e,0x76,0x40,0x07,
    0x7a,0x60,0x07,0x74,0xd0,0x06,0xe6,0x10,0x07,0x76,0xa0,0x07,0x73,0x20,0x07,0x6d,0x60,0x0e,0x73,0x20,0x07,0x7a,0x30,0x07,
    0x72,0xd0,0x06,0xe6,0x60,0x07,0x74,0xa0,0x07,0x76,0x40,0x07,0x6d,0xe0,0x0e,0x78,0xa0,0x07,0x71,0x60,0x07,0x7a,0x30,0x07,
    0x72,0xa0,0x07,0x76,0x40,0x07,0x43,0x9e,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x86,0x3c,0x04,0x10,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0x79,0x0c,0x20,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0xf2,0x30,
    0x40,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0xe4,0x79,0x80,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,
    0xc8,0x13,0x01,0x01,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x90,0x87,0x02,0x02,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x80,0x2c,0x10,0x14,0x00,0x00,0x00,0x32,0x1e,0x98,0x18,0x19,0x11,0x4c,0x90,0x8c,0x09,0x26,0x47,0xc6,0x04,0x43,0x1a,
    0x25,0x30,0x02,0x50,0x0c,0x65,0x51,0x80,0x01,0x65,0x50,0x0e,0xa5,0x50,0x12,0x85,0x50,0x10,0x85,0x51,0x20,0x85,0x52,0x30,
    0x85,0x53,0x40,0x85,0x54,0x50,0x85,0x55,0x60,0x85,0x56,0x70,0x85,0x57,0x92,0x01,0x45,0x40,0x6e,0x04,0xa0,0x06,0xa8,0x17,
    0x08,0xe9,0x19,0x00,0xda,0x33,0x00,0xc4,0x67,0x00,0x08,0xcf,0x00,0x00,0x00,0x00,0x79,0x18,0x00,0x00,0xd1,0x00,0x00,0x00,
    0x1a,0x03,0x4c,0x90,0x46,0x02,0x13,0xc4,0x8e,0x0c,0x6f,0xec,0xed,0x4d,0x0c,0x24,0xc6,0x25,0xc7,0x45,0xc6,0x06,0x26,0xc7,
    0xa5,0x06,0x04,0x05,0x26,0x46,0xcc,0x46,0x26,0x8c,0x8c,0xe6,0x26,0x65,0x43,0x10,0x4c,0x10,0x86,0x63,0x82,0x30,0x20,0x1b,
    0x84,0x81,0xd8,0x20,0x10,0x04,0x85,0xb1,0xb9,0x09,0xc2,0x90,0x6c,0x18,0x0e,0x84,0x98,0x20,0x60,0x67,0xc0,0x62,0x8e,0x2e,
    0xec,0xac,0x6c,0x82,0x30,0x28,0x1b,0x12,0x42,0x59,0x08,0x62,0x60,0x08,0x60,0x43,0xd0,0x4c,0x10,0x34,0x32,0xe0,0x30,0x27,
    0x37,0x36,0x41,0x70,0xc2,0x60,0xc3,0x42,0x3c,0x10,0x41,0x0c,0x4c,0x14,0x45,0x00,0x07,0xb2,0x39,0xba,0x0d,0xcb,0xf0,0x4c,
    0xc4,0x30,0x30,0x51,0x14,0x01,0x1b,0x04,0x89,0x9a,0x20,0x70,0x66,0x40,0x24,0xc9,0x8d,0x2c,0x4d,0xae,0x6c,0x8c,0x6e,0x82,
    0x30,0x2c,0x1b,0x10,0xc2,0xba,0x08,0x62,0xc0,0x80,0x0d,0x41,0xb6,0x81,0x70,0x2a,0x0d,0x98,0x20,0x6c,0x65,0x40,0x65,0x4e,
    0x6e,0xec,0xeb,0xcd,0xcc,0x6c,0xae,0x8c,0x6e,0x82,0x30,0x30,0x13,0x84,0xa1,0x99,0x20,0x0c,0xce,0x04,0x61,0x78,0x26,0x08,
    0x8e,0x18,0x6c,0x40,0x90,0xce,0x23,0x3e,0x30,0x08,0x03,0x31,0xe0,0x11,0x26,0x77,0xf6,0x45,0x36,0x47,0x37,0x41,0x18,0xa0,
    0x0d,0x08,0x42,0x06,0x5e,0x19,0x7c,0x60,0x10,0x06,0x62,0xc0,0x24,0x4c,0xee,0xec,0xeb,0xee,0x4d,0x8e,0x6c,0x6e,0x82,0x30,
    0x44,0x1b,0x10,0xe4,0x0c,0x3c,0x34,0xf8,0xc0,0x20,0x0c,0xc4,0x80,0x48,0x98,0xdc,0xd9,0xd7,0xd8,0x1b,0x5c,0xde,0x04,0x61,
    0x90,0x36,0x20,0x88,0x1a,0x78,0x6b,0xf0,0x81,0x41,0x18,0x88,0x01,0x93,0x30,0xb9,0xb3,0xaf,0x31,0xb6,0xb0,0x36,0xb8,0x09,
    0xc2,0x30,0x6d,0x40,0x90,0x36,0xf0,0xdc,0xe0,0x03,0x83,0x30,0x10,0x03,0x26,0x73,0x74,0x61,0x67,0x65,0x5f,0x73,0x72,0x63,
    0x13,0x84,0x81,0xda,0x80,0x20,0x70,0xe0,0xc5,0xc1,0x07,0x06,0x61,0x20,0x06,0x4c,0xe6,0xe8,0xc2,0xce,0xca,0xbe,0xc8,0xe6,
    0xe8,0x26,0x08,0x43,0xb5,0x01,0x41,0xe6,0xc0,0xa3,0x83,0x0f,0x0c,0xc2,0x40,0x0c,0xb8,0xcc,0xd1,0x85,0x9d,0x95,0x7d,0xdd,
    0xbd,0xc9,0x91,0xcd,0x4d,0x10,0x06,0x6b,0x03,0x82,0xd8,0x81,0x77,0x07,0x1f,0x18,0x84,0x81,0x18,0x50,0x81,0x0b,0xa3,0x1b,
    0x43,0xfb,0xba,0x7b,0x93,0x23,0x9b,0x20,0x0c,0xd7,0x06,0x04,0xc9,0x03,0x4f,0x0f,0x3e,0x30,0x08,0x03,0x31,0x60,0x02,0x17,
    0x46,0x37,0x86,0xf6,0x35,0x27,0x37,0x36,0x41,0x18,0xb0,0x0d,0x08,0xc2,0x07,0x5e,0x1f,0x7c,0x60,0x10,0x06,0x62,0xc0,0x05,
    0x2e,0x8c,0x6e,0x0c,0xed,0xeb,0xee,0x4d,0x8e,0x6c,0x6e,0x82,0x30,0x64,0x1b,0x10,0xe4,0x0f,0x3c,0x50,0xf8,0xc0,0x20,0x0c,
    0xc4,0x80,0x04,0x5c,0x18,0x19,0xcc,0x04,0x61,0xd0,0x36,0x18,0x88,0x28,0x78,0xa3,0xf0,0x81,0x01,0x09,0xb8,0x30,0xb2,0x98,
    0x09,0xc2,0xb0,0x6d,0x30,0x90,0x52,0xf0,0x4c,0xe1,0x03,0x03,0x12,0x70,0x61,0x64,0x32,0x13,0x84,0x81,0xdb,0x60,0x20,0xa8,
    0xe0,0xa5,0xc2,0x07,0x06,0x24,0xe0,0xc2,0xc8,0x66,0x26,0x08,0x43,0xb7,0xc1,0x40,0x56,0xc1,0x63,0x85,0x0f,0x0c,0x48,0xc0,
    0x85,0x91,0xd1,0x4c,0x10,0x06,0x6f,0x83,0x81,0xb8,0x82,0xf7,0x0a,0x1f,0x18,0x6c,0x88,0xb0,0x31,0x30,0x83,0x34,0x60,0x83,
    0x37,0x90,0x83,0x3a,0xc0,0x83,0x3d,0xf0,0x83,0x50,0x20,0x85,0x53,0x50,0x85,0x56,0x80,0x85,0x0d,0x03,0xc1,0xc5,0xc2,0x04,
    0x41,0x00,0x36,0x00,0x1b,0x06,0x82,0x16,0x68,0x61,0x43,0x50,0x0b,0x1b,0x86,0x61,0x16,0x6c,0x81,0xcb,0xd8,0xdc,0x57,0x9a,
    0x1b,0x59,0x9a,0x5c,0xd9,0x18,0xdd,0x04,0xa1,0x1b,0x83,0x0d,0x03,0x36,0x0c,0x1b,0x08,0x22,0x17,0xca,0x40,0x17,0x36,0x14,
    0xb3,0x80,0x0b,0xc0,0xb6,0x0b,0x44,0xc4,0xe4,0xc2,0xdc,0xc6,0xd0,0xca,0xe6,0x58,0xa4,0xb9,0xcd,0xd1,0xcd,0x4d,0x10,0x86,
    0x8f,0x44,0x9a,0x1b,0xdd,0x1c,0x11,0xba,0x32,0xbc,0x2f,0xb6,0xb7,0x30,0x32,0x26,0x74,0x65,0x78,0x5f,0x73,0x74,0x6f,0x72,
    0x65,0x13,0x84,0x01,0x0c,0x58,0xd4,0xa5,0xb9,0xd1,0xcd,0x6d,0x60,0x7a,0xe1,0xf3,0x85,0x5f,0x00,0x07,0x37,0x08,0x07,0x4f,
    0x1c,0xc6,0x81,0x1c,0x90,0x2a,0x6c,0x6c,0x76,0x6d,0x2e,0x69,0x64,0x65,0x6e,0x74,0x53,0x82,0xa0,0x0a,0x19,0x9e,0x8b,0x5d,
    0x99,0xdc,0x5c,0xda,0x9b,0xdb,0x94,0x80,0x68,0x42,0x86,0xe7,0x62,0x17,0xc6,0x66,0x57,0x26,0x37,0x25,0x28,0xea,0x90,0xe1,
    0xb9,0xcc,0xa1,0x85,0x91,0x95,0xc9,0x35,0xbd,0x91,0x95,0xb1,0x4d,0x09,0x90,0x32,0x64,0x78,0x2e,0x72,0x65,0x73,0x6f,0x75,
    0x72,0x63,0x65,0x73,0x53,0x02,0xad,0x12,0x19,0x9e,0x0b,0x5d,0x1e,0x5c,0x59,0x90,0x9b,0xdb,0x1b,0x5d,0x18,0x5d,0xda,0x9b,
    0xdb,0xdc,0x14,0x21,0x16,0x6c,0xa1,0x0e,0x19,0x9e,0x4b,0x99,0x1b,0x9d,0x5c,0x1e,0xd4,0x5b,0x9a,0x1b,0xdd,0xdc,0x94,0x60,
    0x17,0xba,0x90,0xe1,0xb9,0x8c,0xbd,0xd5,0xb9,0xd1,0x95,0xc9,0xcd,0x4d,0x09,0xc8,0x01,0x00,0x00,0x00,0x79,0x18,0x00,0x00,
    0x51,0x00,0x00,0x00,0x33,0x08,0x80,0x1c,0xc4,0xe1,0x1c,0x66,0x14,0x01,0x3d,0x88,0x43,0x38,0x84,0xc3,0x8c,0x42,0x80,0x07,
    0x79,0x78,0x07,0x73,0x98,0x71,0x0c,0xe6,0x00,0x0f,0xed,0x10,0x0e,0xf4,0x80,0x0e,0x33,0x0c,0x42,0x1e,0xc2,0xc1,0x1d,0xce,
    0xa1,0x1c,0x66,0x30,0x05,0x3d,0x88,0x43,0x38,0x84,0x83,0x1b,0xcc,0x03,0x3d,0xc8,0x43,0x3d,0x8c,0x03,0x3d,0xcc,0x78,0x8c,
    0x74,0x70,0x07,0x7b,0x08,0x07,0x79,0x48,0x87,0x70,0x70,0x07,0x7a,0x70,0x03,0x76,0x78,0x87,0x70,0x20,0x87,0x19,0xcc,0x11,
    0x0e,0xec,0x90,0x0e,0xe1,0x30,0x0f,0x6e,0x30,0x0f,0xe3,0xf0,0x0e,0xf0,0x50,0x0e,0x33,0x10,0xc4,0x1d,0xde,0x21,0x1c,0xd8,
    0x21,0x1d,0xc2,0x61,0x1e,0x66,0x30,0x89,0x3b,0xbc,0x83,0x3b,0xd0,0x43,0x39,0xb4,0x03,0x3c,0xbc,0x83,0x3c,0x84,0x03,0x3b,
    0xcc,0xf0,0x14,0x76,0x60,0x07,0x7b,0x68,0x07,0x37,0x68,0x87,0x72,0x68,0x07,0x37,0x80,0x87,0x70,0x90,0x87,0x70,0x60,0x07,
    0x76,0x28,0x07,0x76,0xf8,0x05,0x76,0x78,0x87,0x77,0x80,0x87,0x5f,0x08,0x87,0x71,0x18,0x87,0x72,0x98,0x87,0x79,0x98,0x81,
    0x2c,0xee,0xf0,0x0e,0xee,0xe0,0x0e,0xf5,0xc0,0x0e,0xec,0x30,0x03,0x62,0xc8,0xa1,0x1c,0xe4,0xa1,0x1c,0xcc,0xa1,0x1c,0xe4,
    0xa1,0x1c,0xdc,0x61,0x1c,0xca,0x21,0x1c,0xc4,0x81,0x1d,0xca,0x61,0x06,0xd6,0x90,0x43,0x39,0xc8,0x43,0x39,0x98,0x43,0x39,
    0xc8,0x43,0x39,0xb8,0xc3,0x38,0x94,0x43,0x38,0x88,0x03,0x3b,0x94,0xc3,0x2f,0xbc,0x83,0x3c,0xfc,0x82,0x3b,0xd4,0x03,0x3b,
    0xb0,0xc3,0x0c,0xc4,0x21,0x07,0x7c,0x70,0x03,0x7a,0x28,0x87,0x76,0x80,0x87,0x19,0xd1,0x43,0x0e,0xf8,0xe0,0x06,0xe4,0x20,
    0x0e,0xe7,0xe0,0x06,0xf6,0x10,0x0e,0xf2,0xc0,0x0e,0xe1,0x90,0x0f,0xef,0x50,0x0f,0xf4,0x30,0x83,0x81,0xc8,0x01,0x1f,0xdc,
    0x40,0x1c,0xe4,0xa1,0x1c,0xc2,0x61,0x1d,0xdc,0x40,0x1c,0xe4,0x01,0x00,0x00,0x00,0x71,0x20,0x00,0x00,0x1d,0x00,0x00,0x00,
    0x06,0xb0,0x08,0xd2,0x8f,0x34,0x03,0x12,0x11,0xc2,0x64,0x02,0xd0,0x70,0xf9,0xce,0xe3,0x07,0x48,0x03,0x44,0x98,0x8f,0xdc,
    0xb6,0x19,0x50,0xc3,0xe5,0x3b,0x8f,0x1f,0x50,0x45,0x41,0x44,0xa5,0x03,0x0c,0x3e,0x72,0xdb,0x56,0x50,0x0d,0x97,0xef,0x3c,
    0x7e,0x40,0x15,0x05,0x11,0xb1,0x93,0x13,0x11,0x3e,0x72,0xdb,0x36,0xb0,0x0d,0x97,0xef,0x3c,0xbe,0x10,0x50,0x45,0x41,0x44,
    0xa5,0x03,0x0c,0x25,0x61,0x00,0x02,0xe6,0x23,0xb7,0x6d,0x04,0xd2,0x70,0xf9,0xce,0xe3,0x0b,0x11,0x01,0x4c,0x44,0x08,0x34,
    0xc3,0x42,0x58,0x80,0x34,0x5c,0xbe,0xf3,0xf8,0xd3,0x11,0x11,0xc0,0x20,0x0e,0x3e,0x72,0xdb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x48,0x41,0x53,0x48,0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3a,0xcb,0xd0,0xb3,0x87,0xb5,0xde,0x40,0x63,0xba,0xce,0x6e,
    0x56,0x76,0xb9,0xa7,0x44,0x58,0x49,0x4c,0x84,0x07,0x00,0x00,0x60,0x00,0x05,0x00,0xe1,0x01,0x00,0x00,0x44,0x58,0x49,0x4c,
    0x00,0x01,0x00,0x00,0x10,0x00,0x00,0x00,0x6c,0x07,0x00,0x00,0x42,0x43,0xc0,0xde,0x21,0x0c,0x00,0x00,0xd8,0x01,0x00,0x00,
    0x0b,0x82,0x20,0x00,0x02,0x00,0x00,0x00,0x13,0x00,0x00,0x00,0x07,0x81,0x23,0x91,0x41,0xc8,0x04,0x49,0x06,0x10,0x32,0x39,
    0x92,0x01,0x84,0x0c,0x25,0x05,0x08,0x19,0x1e,0x04,0x8b,0x62,0x80,0x14,0x45,0x02,0x42,0x92,0x0b,0x42,0xa4,0x10,0x32,0x14,
    0x38,0x08,0x18,0x4b,0x0a,0x32,0x52,0x88,0x48,0x90,0x14,0x20,0x43,0x46,0x88,0xa5,0x00,0x19,0x32,0x42,0xe4,0x48,0x0e,0x90,
    0x91,0x22,0xc4,0x50,0x41,0x51,0x81,0x8c,0xe1,0x83,0xe5,0x8a,0x04,0x29,0x46,0x06,0x51,0x18,0x00,0x00,0x08,0x00,0x00,0x00,
    0x1b,0x8c,0xe0,0xff,0xff,0xff,0xff,0x07,0x40,0x02,0xa8,0x0d,0x86,0xf0,0xff,0xff,0xff,0xff,0x03,0x20,0x01,0xd5,0x06,0x62,
    0xf8,0xff,0xff,0xff,0xff,0x01,0x90,0x00,0x49,0x18,0x00,0x00,0x03,0x00,0x00,0x00,0x13,0x82,0x60,0x42,0x20,0x4c,0x08,0x06,
    0x00,0x00,0x00,0x00,0x89,0x20,0x00,0x00,0x35,0x00,0x00,0x00,0x32,0x22,0x48,0x09,0x20,0x64,0x85,0x04,0x93,0x22,0xa4,0x84,
    0x04,0x93,0x22,0xe3,0x84,0xa1,0x90,0x14,0x12,0x4c,0x8a,0x8c,0x0b,0x84,0xa4,0x4c,0x10,0x7c,0x23,0x00,0x25,0x00,0x14,0xe6,
    0x08,0xc0,0xa0,0x0c,0x63,0x0c,0x22,0x85,0x18,0x63,0x0c,0x32,0x47,0x0d,0x97,0x3f,0x61,0x0f,0x21,0xf9,0xdc,0x46,0x15,0x2b,
    0x31,0xf9,0xc8,0x6d,0x23,0x62,0x8c,0x31,0xe6,0x08,0x10,0x4a,0xf7,0x0c,0x97,0x3f,0x61,0x0f,0x21,0xf9,0x21,0xd0,0x0c,0x0b,
    0x81,0x02,0x55,0x08,0x34,0xd6,0x20,0x36,0x47,0x10,0x14,0x63,0x8d,0x34,0x86,0xa3,0x57,0x14,0x30,0xd6,0x18,0x63,0x8c,0x91,
    0x28,0xde,0x34,0x5c,0xfe,0x84,0x3d,0x84,0xe4,0xaf,0x84,0xb4,0x12,0x93,0x8f,0xdc,0x36,0x2a,0xc6,0x18,0x63,0x94,0x62,0x8e,
    0x35,0x06,0xd1,0x81,0x80,0xc3,0xa4,0x29,0xa2,0x84,0xc9,0xdf,0xb0,0x89,0xd0,0x86,0x21,0x22,0x24,0x69,0xa3,0x8a,0x82,0x88,
    0x50,0x30,0xe8,0x9e,0x26,0x4d,0x11,0x25,0x4c,0xfe,0x0a,0x6f,0xd8,0x44,0x68,0xc3,0x10,0x11,0x92,0xb4,0x51,0x45,0x41,0x44,
    0x28,0x18,0x94,0x0f,0x11,0x9b,0x01,0x89,0x08,0x61,0x42,0xd0,0x18,0x63,0x8c,0x31,0xc6,0x18,0x63,0x8c,0x31,0x06,0xed,0x39,
    0x02,0x50,0x98,0x02,0x00,0x00,0x00,0x00,0x13,0x14,0x72,0xc0,0x87,0x74,0x60,0x87,0x36,0x68,0x87,0x79,0x68,0x03,0x72,0xc0,
    0x87,0x0d,0xaf,0x50,0x0e,0x6d,0xd0,0x0e,0x7a,0x50,0x0e,0x6d,0x00,0x0f,0x7a,0x30,0x07,0x72,0xa0,0x07,0x73,0x20,0x07,0x6d,
    0x90,0x0e,0x71,0xa0,0x07,0x73,0x20,0x07,0x6d,0x90,0x0e,0x78,0xa0,0x07,0x73,0x20,0x07,0x6d,0x90,0x0e,0x71,0x60,0x07,0x7a,
    0x30,0x07,0x72,0xd0,0x06,0xe9,0x30,0x07,0x72,0xa0,0x07,0x73,0x20,0x07,0x6d,0x90,0x0e,0x76,0x40,0x07,0x7a,0x60,0x07,0x74,
    0xd0,0x06,0xe6,0x10,0x07,0x76,0xa0,0x07,0x73,0x20,0x07,0x6d,0x60,0x0e,0x73,0x20,0x07,0x7a,0x30,0x07,0x72,0xd0,0x06,0xe6,
    0x60,0x07,0x74,0xa0,0x07,0x76,0x40,0x07,0x6d,0xe0,0x0e,0x78,0xa0,0x07,0x71,0x60,0x07,0x7a,0x30,0x07,0x72,0xa0,0x07,0x76,
    0x40,0x07,0x43,0x9e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x86,0x3c,0x04,0x10,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x0c,0x79,0x0c,0x20,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0xf2,0x30,0x40,0x00,0x08,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x30,0xe4,0x79,0x80,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0xc8,0x13,0x01,0x01,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x90,0x87,0x02,0x02,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x2c,0x10,
    0x0b,0x00,0x00,0x00,0x32,0x1e,0x98,0x14,0x19,0x11,0x4c,0x90,0x8c,0x09,0x26,0x47,0xc6,0x04,0x43,0x1a,0x25,0x30,0x02,0x50,
    0x12,0xc5,0x50,0x16,0x05,0x18,0x50,0x08,0xe4,0x46,0x00,0xa8,0x17,0x08,0xe9,0x19,0x00,0xe2,0x33,0x00,0x84,0x67,0x00,0x00,
    0x79,0x18,0x00,0x00,0x44,0x00,0x00,0x00,0x1a,0x03,0x4c,0x90,0x46,0x02,0x13,0xc4,0x8e,0x0c,0x6f,0xec,0xed,0x4d,0x0c,0x24,
    0xc6,0x25,0xc7,0x45,0xc6,0x06,0x26,0xc7,0xa5,0x06,0x04,0x05,0x26,0x46,0xcc,0x46,0x26,0x8c,0x8c,0xe6,0x26,0x65,0x43,0x10,
    0x4c,0x10,0x86,0x63,0x82,0x30,0x20,0x1b,0x84,0x81,0x98,0x20,0x0c,0xc9,0x06,0x61,0x30,0x28,0x8c,0xcd,0x4d,0x10,0x06,0x65,
    0xc3,0x80,0x24,0xc4,0x04,0x01,0x93,0x08,0x4c,0x10,0x86,0x65,0x43,0x42,0x2c,0x0c,0x41,0x0c,0x0d,0x01,0x6c,0x08,0x9c,0x09,
    0x82,0x06,0x4d,0x10,0x1c,0x67,0xc3,0x42,0x40,0x0c,0x41,0x0c,0x4d,0x14,0x45,0xc0,0x86,0x65,0x80,0x18,0x62,0x18,0x9a,0x28,
    0x8a,0x80,0x0d,0x82,0x34,0x4d,0x10,0xb8,0x68,0x82,0x30,0x30,0x1b,0x10,0xa2,0x62,0x08,0x62,0xb0,0x80,0x0d,0xc1,0xb5,0x81,
    0x78,0x28,0x0c,0x98,0x20,0x08,0x00,0x97,0xb1,0xb9,0xaf,0x34,0x37,0xb2,0x34,0xb9,0xb2,0x31,0xba,0x09,0x42,0xf7,0x4c,0x10,
    0x86,0x66,0xc3,0x60,0x0d,0xc3,0x06,0x82,0xe0,0x3a,0x6f,0x43,0xa1,0x6d,0x40,0xf6,0x55,0x61,0x63,0xb3,0x6b,0x73,0x49,0x23,
    0x2b,0x73,0xa3,0x9b,0x12,0x04,0x55,0xc8,0xf0,0x5c,0xec,0xca,0xe4,0xe6,0xd2,0xde,0xdc,0xa6,0x04,0x44,0x13,0x32,0x3c,0x17,
    0xbb,0x30,0x36,0xbb,0x32,0xb9,0x29,0x81,0x51,0x87,0x0c,0xcf,0x65,0x0e,0x2d,0x8c,0xac,0x4c,0xae,0xe9,0x8d,0xac,0x8c,0x6d,
    0x4a,0x90,0x94,0x21,0xc3,0x73,0x91,0x2b,0x9b,0x7b,0xab,0x93,0x1b,0x2b,0x9b,0x9b,0x12,0x60,0x75,0xc8,0xf0,0x5c,0xca,0xdc,
    0xe8,0xe4,0xf2,0xa0,0xde,0xd2,0xdc,0xe8,0xe6,0xa6,0x04,0x1f,0x00,0x00,0x00,0x00,0x79,0x18,0x00,0x00,0x51,0x00,0x00,0x00,
    0x33,0x08,0x80,0x1c,0xc4,0xe1,0x1c,0x66,0x14,0x01,0x3d,0x88,0x43,0x38,0x84,0xc3,0x8c,0x42,0x80,0x07,0x79,0x78,0x07,0x73,
    0x98,0x71,0x0c,0xe6,0x00,0x0f,0xed,0x10,0x0e,0xf4,0x80,0x0e,0x33,0x0c,0x42,0x1e,0xc2,0xc1,0x1d,0xce,0xa1,0x1c,0x66,0x30,
    0x05,0x3d,0x88,0x43,0x38,0x84,0x83,0x1b,0xcc,0x03,0x3d,0xc8,0x43,0x3d,0x8c,0x03,0x3d,0xcc,0x78,0x8c,0x74,0x70,0x07,0x7b,
    0x08,0x07,0x79,0x48,0x87,0x70,0x70,0x07,0x7a,0x70,0x03,0x76,0x78,0x87,0x70,0x20,0x87,0x19,0xcc,0x11,0x0e,0xec,0x90,0x0e,
    0xe1,0x30,0x0f,0x6e,0x30,0x0f,0xe3,0xf0,0x0e,0xf0,0x50,0x0e,0x33,0x10,0xc4,0x1d,0xde,0x21,0x1c,0xd8,0x21,0x1d,0xc2,0x61,
    0x1e,0x66,0x30,0x89,0x3b,0xbc,0x83,0x3b,0xd0,0x43,0x39,0xb4,0x03,0x3c,0xbc,0x83,0x3c,0x84,0x03,0x3b,0xcc,0xf0,0x14,0x76,
    0x60,0x07,0x7b,0x68,0x07,0x37,0x68,0x87,0x72,0x68,0x07,0x37,0x80,0x87,0x70,0x90,0x87,0x70,0x60,0x07,0x76,0x28,0x07,0x76,
    0xf8,0x05,0x76,0x78,0x87,0x77,0x80,0x87,0x5f,0x08,0x87,0x71,0x18,0x87,0x72,0x98,0x87,0x79,0x98,0x81,0x2c,0xee,0xf0,0x0e,
    0xee,0xe0,0x0e,0xf5,0xc0,0x0e,0xec,0x30,0x03,0x62,0xc8,0xa1,0x1c,0xe4,0xa1,0x1c,0xcc,0xa1,0x1c,0xe4,0xa1,0x1c,0xdc,0x61,
    0x1c,0xca,0x21,0x1c,0xc4,0x81,0x1d,0xca,0x61,0x06,0xd6,0x90,0x43,0x39,0xc8,0x43,0x39,0x98,0x43,0x39,0xc8,0x43,0x39,0xb8,
    0xc3,0x38,0x94,0x43,0x38,0x88,0x03,0x3b,0x94,0xc3,0x2f,0xbc,0x83,0x3c,0xfc,0x82,0x3b,0xd4,0x03,0x3b,0xb0,0xc3,0x0c,0xc4,
    0x21,0x07,0x7c,0x70,0x03,0x7a,0x28,0x87,0x76,0x80,0x87,0x19,0xd1,0x43,0x0e,0xf8,0xe0,0x06,0xe4,0x20,0x0e,0xe7,0xe0,0x06,
    0xf6,0x10,0x0e,0xf2,0xc0,0x0e,0xe1,0x90,0x0f,0xef,0x50,0x0f,0xf4,0x30,0x83,0x81,0xc8,0x01,0x1f,0xdc,0x40,0x1c,0xe4,0xa1,
    0x1c,0xc2,0x61,0x1d,0xdc,0x40,0x1c,0xe4,0x01,0x00,0x00,0x00,0x71,0x20,0x00,0x00,0x1d,0x00,0x00,0x00,0x06,0xb0,0x08,0xd2,
    0x8f,0x34,0x03,0x12,0x11,0xc2,0x64,0x02,0xd0,0x70,0xf9,0xce,0xe3,0x07,0x48,0x03,0x44,0x98,0x8f,0xdc,0xb6,0x19,0x50,0xc3,
    0xe5,0x3b,0x8f,0x1f,0x50,0x45,0x41,0x44,0xa5,0x03,0x0c,0x3e,0x72,0xdb,0x56,0x50,0x0d,0x97,0xef,0x3c,0x7e,0x40,0x15,0x05,
    0x11,0xb1,0x93,0x13,0x11,0x3e,0x72,0xdb,0x36,0xb0,0x0d,0x97,0xef,0x3c,0xbe,0x10,0x50,0x45,0x41,0x44,0xa5,0x03,0x0c,0x25,
    0x61,0x00,0x02,0xe6,0x23,0xb7,0x6d,0x04,0xd2,0x70,0xf9,0xce,0xe3,0x0b,0x11,0x01,0x4c,0x44,0x08,0x34,0xc3,0x42,0x58,0x80,
    0x34,0x5c,0xbe,0xf3,0xf8,0xd3,0x11,0x11,0xc0,0x20,0x0e,0x3e,0x72,0xdb,0x00,0x00,0x61,0x20,0x00,0x00,0x7b,0x00,0x00,0x00,
    0x13,0x04,0x4e,0x2c,0x10,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x34,0x66,0x00,0xca,0xae,0x08,0x4a,0xae,0x10,0x03,0x0a,0xaa,
    0xfc,0xff,0x0f,0x4a,0x31,0xa0,0x74,0x03,0x28,0x95,0xc0,0x08,0x40,0x11,0x00,0x00,0x23,0x06,0x09,0x00,0x82,0x60,0xf0,0x6c,
    0xc9,0x80,0x61,0xd1,0x88,0x41,0x02,0x80,0x20,0x18,0x3c,0x9c,0x42,0x60,0x98,0x34,0x62,0x90,0x00,0x20,0x08,0x06,0x4f,0xb7,
    0x10,0x59,0x36,0x8d,0x18,0x24,0x00,0x08,0x82,0xc1,0xe3,0x31,0x84,0xa6,0x51,0x23,0x06,0x06,0x00,0x82,0x60,0x40,0x88,0x01,
    0xb2,0x8d,0x18,0x1c,0x00,0x08,0x82,0x01,0x13,0x06,0x90,0xd0,0x8d,0x26,0x04,0xc3,0x70,0xc3,0x10,0x90,0xc1,0x2c,0x43,0x80,
    0x04,0xb3,0x04,0xc2,0x40,0xc5,0x90,0x07,0x06,0x12,0x8c,0x18,0x1c,0x00,0x08,0x82,0x01,0x53,0x06,0x94,0x31,0x8d,0x26,0x04,
    0x40,0x0d,0x41,0x8c,0x26,0x0c,0xc2,0x70,0x83,0x10,0x90,0xc1,0x88,0xc1,0x01,0x80,0x20,0x18,0x30,0x6a,0x90,0x2d,0x66,0x30,
    0xcb,0x30,0x14,0xc2,0x68,0x82,0x11,0x54,0x80,0x40,0x05,0x47,0x8c,0x18,0x1c,0x00,0x08,0x82,0x01,0xe3,0x06,0xdd,0x93,0x06,
    0xa3,0x09,0x01,0x50,0x83,0x77,0x25,0x04,0x30,0x62,0x80,0x00,0x20,0x08,0x06,0xd4,0x1b,0x78,0x55,0x20,0x06,0xa3,0x09,0x01,
    0x30,0x9a,0xa0,0x00,0xc3,0x0d,0x01,0x1c,0x80,0xc1,0x2c,0x83,0x41,0x04,0x23,0x06,0x07,0x00,0x82,0x60,0x60,0xdc,0x41,0x18,
    0x0c,0x60,0x30,0x4b,0x60,0x8c,0x26,0x34,0x41,0x51,0x66,0x70,0x25,0x04,0x30,0x62,0x80,0x00,0x20,0x08,0x06,0xd4,0x1d,0x98,
    0x01,0x17,0xa8,0xc1,0x68,0x42,0x00,0xcc,0x12,0x18,0x03,0x1d,0x83,0x50,0x30,0x84,0x34,0x8c,0x26,0x4c,0x42,0x69,0x6c,0x70,
    0x25,0x04,0x30,0x62,0xd0,0x00,0x20,0x08,0x06,0x91,0x1f,0xa4,0x01,0x19,0x04,0x70,0x40,0xc0,0x01,0x1c,0xc0,0xc1,0x19,0x14,
    0x77,0x07,0x30,0x62,0x70,0x00,0x20,0x08,0x06,0x4c,0x28,0xc0,0x81,0x18,0xf4,0xc1,0x68,0x42,0x30,0x0c,0x37,0x0c,0x01,0x19,
    0xcc,0x32,0x08,0x47,0x30,0x4b,0x80,0x8c,0x18,0x1c,0x00,0x08,0x82,0x01,0x43,0x0a,0x73,0x50,0x06,0x7f,0x30,0x9a,0x10,0x08,
    0xc3,0x0d,0x66,0x10,0x90,0xc1,0x2c,0x43,0xd2,0x04,0xa3,0x09,0xc3,0x30,0xdc,0x80,0x06,0x01,0x19,0x54,0x1a,0xdc,0xc1,0xcd,
    0x32,0x28,0x8c,0x30,0x9a,0x60,0x00,0x15,0x08,0x30,0x62,0x80,0x00,0x20,0x08,0x06,0x54,0x2a,0xe0,0xc1,0x1b,0x04,0x7c,0x30,
    0x9a,0x10,0x00,0x23,0x06,0x07,0x00,0x82,0x60,0xc0,0xb8,0x42,0x1f,0xbc,0x81,0x2a,0x8c,0x26,0x04,0xc0,0x70,0x43,0xb0,0x0a,
    0x60,0x30,0xcb,0xc0,0x2c,0xc1,0x88,0xc1,0x01,0x80,0x20,0x18,0x18,0xb2,0xc0,0x07,0xc4,0x1e,0xcc,0x12,0x30,0x03,0x1d,0x83,
    0xb0,0x28,0x8a,0x4e,0x24,0xa3,0x09,0x4f,0x50,0x4b,0x00,0x23,0x06,0x0d,0x00,0x82,0x60,0x10,0xcd,0x82,0x1f,0xe4,0x41,0x50,
    0x0a,0x43,0x29,0x94,0x42,0x29,0xf0,0xc1,0x2c,0x41,0x83,0x00,0x00,0x00,0x00,0x00,
};
/* END render-38d indirect DXIL */

#define BOOT_RENDER_IND_SLOTS 2048u         /* ring slots, round robin: far more than commands in flight */
#define BOOT_RENDER_IND_SLOT 2048u          /* bytes per slot: constant buffer copy at +0 (<= 0x500), arguments at +ARGS */
#define BOOT_RENDER_IND_ARGS 0x700u
#define BOOT_RENDER_IND_IB_MAX (UINT64_C(256) << 20)
enum { BOOT_RENDER_IND_DRAW = 1, BOOT_RENDER_IND_INDEXED = 2, BOOT_RENDER_IND_DISPATCH = 3 };
/* One indirect command: set by the native executor (host_nvn_play.h) for the next boot_render_draw or
 * boot_compute_dispatch_decoded, which take it at entry; the staging fields are filled while it records. */
typedef struct BootRenderIndOp {
    uint64_t va; uint32_t kind;            /* NVN argument structure, BOOT_RENDER_IND_* (0 = not indirect) */
    unsigned slot; uint64_t stage_off;     /* ring slot; staged constant buffer offset in the upload ring */
    uint32_t stage_words, patch_word;      /* staged words; first one replaced from the arguments */
} BootRenderIndOp;
static BootRenderIndOp boot_render_next_ind;
static struct {
    ID3D12Device *device; ID3D12RootSignature *root; ID3D12PipelineState *pso; ID3D12CommandSignature *sig[4];
    int failed; unsigned next;
} boot_render_ind;
static uint64_t boot_render_ind_prepasses, boot_render_ind_executed[4], boot_render_ind_failures;
static const IID boot_render_iid_cmdsig = {0xc36a797c, 0xec80, 0x4f0a, {0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1}};   /* ID3D12CommandSignature */

/* 1 = GPU arguments (default), 0 = TOTK_INDIRECT=cpu (the CPU read after a queue wait, as before) */
static inline int boot_render_ind_mode(void) {
    static int v = -1;
    if (v < 0) { char s[16]; DWORD n = GetEnvironmentVariableA("TOTK_INDIRECT", s, sizeof s); v = !(n && n < sizeof s && !_stricmp(s, "cpu")); }
    return v;
}
static inline void boot_render_ind_log(const char *what, uint64_t value) {
    FILE *f = fopen("draw-path.log", "a"); if (!f) return;
    fprintf(f, "nvn native: indirect arguments on the GPU (render-38d): %s %llx\n", what, (unsigned long long)value); fclose(f);
}
static inline void boot_render_ind_release(void) {
    for (unsigned i = 0; i < 4; ++i) if (boot_render_ind.sig[i]) ID3D12CommandSignature_Release(boot_render_ind.sig[i]);
    if (boot_render_ind.pso) ID3D12PipelineState_Release(boot_render_ind.pso);
    if (boot_render_ind.root) ID3D12RootSignature_Release(boot_render_ind.root);
    if (boot_render_ind_ring) ID3D12Resource_Release(boot_render_ind_ring);
    boot_render_ind_ring = NULL; boot_render_ind_state = D3D12_RESOURCE_STATE_COMMON;
    memset(&boot_render_ind, 0, sizeof boot_render_ind);
}
/* Root signature (16 root constants b0, root SRV t0 staged constants, root UAVs u0 guest region / u1 ring), pipeline,
 * ring and the three command signatures of the render object's device; once per device. */
static inline int boot_render_ind_open(BootRender *r) {
    ID3D12Device *dev = r->gpu->device;
    if (boot_render_ind.device == dev) return !boot_render_ind.failed;
    if (boot_render_ind.device) boot_render_ind_release();   /* objects of another (removed) device */
    boot_render_ind.device = dev; boot_render_ind.failed = 1;
    D3D12_ROOT_PARAMETER1 p[4]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 16;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor.ShaderRegister = 0; p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.ShaderRegister = 0; p[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[3].Descriptor.ShaderRegister = 1; p[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    for (unsigned i = 0; i < 4; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 4; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) {
        if (err) ID3D10Blob_Release(err);
        boot_render_ind_log("root signature serialization failed, CPU path", 0); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&boot_render_ind.root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(boot_render_ind.root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { boot_render_ind.root = NULL; boot_render_ind_log("root signature hr, CPU path", (uint32_t)hr); return 0; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd; memset(&cd, 0, sizeof cd); cd.pRootSignature = boot_render_ind.root;
    cd.CS.pShaderBytecode = boot_gpu_indirect_dxil; cd.CS.BytecodeLength = sizeof boot_gpu_indirect_dxil;
    if (FAILED(hr = boot_psoc_create_compute(r, dev, &cd, &boot_render_ind.pso))) {
        boot_render_ind.pso = NULL; boot_render_ind_log("pipeline hr, CPU path", (uint32_t)hr); return 0; }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = (uint64_t)BOOT_RENDER_IND_SLOTS * BOOT_RENDER_IND_SLOT; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&boot_render_ind_ring))) {
        boot_render_ind_ring = NULL; boot_render_ind_log("ring hr, CPU path", (uint32_t)hr); return 0; }
    ++boot_render_created_buffers; boot_render_ind_state = D3D12_RESOURCE_STATE_COMMON;   /* untouched by any list yet */
    static const D3D12_INDIRECT_ARGUMENT_TYPE type[4] = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,
        D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    static const UINT stride[4] = {16, sizeof(D3D12_DRAW_ARGUMENTS), sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), sizeof(D3D12_DISPATCH_ARGUMENTS)};
    for (unsigned k = 1; k < 4; ++k) {
        D3D12_INDIRECT_ARGUMENT_DESC a; memset(&a, 0, sizeof a); a.Type = type[k];
        D3D12_COMMAND_SIGNATURE_DESC sd; memset(&sd, 0, sizeof sd); sd.ByteStride = stride[k]; sd.NumArgumentDescs = 1; sd.pArgumentDescs = &a;
        if (FAILED(hr = ID3D12Device_CreateCommandSignature(dev, &sd, NULL, &boot_render_iid_cmdsig, (void **)&boot_render_ind.sig[k]))) {
            boot_render_ind.sig[k] = NULL; boot_render_ind_log("command signature hr, CPU path", (uint32_t)hr); return 0; }
    }
    boot_render_ind.failed = 0;
    boot_render_ind_log("ready, ring slots", BOOT_RENDER_IND_SLOTS);
    return 1;
}
/* NVN argument structure size of a kind; whether the GPU can read it in place (4-byte aligned, contiguous in one region). */
static inline uint32_t boot_render_ind_bytes(uint32_t kind) { return kind == BOOT_RENDER_IND_INDEXED ? 20u : kind == BOOT_RENDER_IND_DRAW ? 16u : 12u; }
static inline int boot_render_ind_args_ok(BootRender *r, uint64_t va, uint32_t kind) {
    int reg; uint64_t off; uint32_t n = boot_render_ind_bytes(kind);
    return !(va & 3) && boot_render_region_span(r, va, n, &reg, &off) >= n;
}
/* Stages `bytes` of constant data (zero-padded to `staged`, a multiple of 256) in the upload ring for the pre-pass to
 * copy and takes the command's ring slot. 0 = no room (the caller fails the draw/dispatch as for any ring overflow). */
static inline int boot_render_ind_stage(BootRender *r, BootRenderIndOp *op, const void *data, uint32_t bytes, uint32_t staged, uint32_t patch_word) {
    if (!r->recording || r->ring_used + staged > r->ring_size || staged > BOOT_RENDER_IND_ARGS) return 0;
    uint8_t *p = r->ring_cpu + r->ring_used; memset(p, 0, staged); if (data && bytes) memcpy(p, data, bytes < staged ? bytes : staged);
    op->stage_off = r->ring_used; r->ring_used += staged; op->stage_words = staged / 4; op->patch_word = patch_word;
    op->slot = boot_render_ind.next; boot_render_ind.next = (op->slot + 1) % BOOT_RENDER_IND_SLOTS;
    return 1;
}
static inline D3D12_GPU_VIRTUAL_ADDRESS boot_render_ind_slot_gpu(const BootRenderIndOp *op) {
    return ID3D12Resource_GetGPUVirtualAddress(boot_render_ind_ring) + (uint64_t)op->slot * BOOT_RENDER_IND_SLOT;
}
/* Records the pre-pass of a staged command (see the top of this file). Leaves a compute pipeline bound (the bind
 * epoch is bumped, so the next draw binds its graphics state again). 0 = not possible (arguments no longer in one
 * region: the caller fails the command). */
static inline int boot_render_ind_prepass(BootRender *r, const BootRenderIndOp *op) {
    int reg; uint64_t off; uint32_t bytes = boot_render_ind_bytes(op->kind);
    if (!r->recording || boot_render_region_span(r, op->va, bytes, &reg, &off) < bytes) { ++boot_render_ind_failures; return 0; }
    ID3D12GraphicsCommandList *cl = r->cl;
    boot_render_gw_settle(r);   /* guest memory written by GPU copies earlier in this list: readable again */
    D3D12_RESOURCE_BARRIER b[2]; memset(b, 0, sizeof b); unsigned n = 0;
    b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b[n].UAV.pResource = NULL; ++n;   /* after earlier compute and storing draws */
    if (boot_render_ind_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[n].Transition.pResource = boot_render_ind_ring;
        b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[n].Transition.StateBefore = boot_render_ind_state; b[n].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; ++n;
    }
    BOOT_GP_RB("indirect pre-pass", cl, n, b);
    boot_render_ind_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; boot_render_uav_pending = 0; ++boot_render_uav_barriers;
    uint64_t base = off & ~UINT64_C(255);   /* root UAV at a 256-byte aligned address; the arguments at +src_offset */
    uint32_t slot = op->slot * BOOT_RENDER_IND_SLOT, words = bytes / 4;
    uint32_t copy = op->kind == BOOT_RENDER_IND_DRAW ? 2u : 3u;            /* count, instances [, first index] / grid */
    uint32_t patch_src = op->kind == BOOT_RENDER_IND_DRAW ? 2u : op->kind == BOOT_RENDER_IND_INDEXED ? 3u : 0u;
    uint32_t patch_words = op->kind == BOOT_RENDER_IND_DISPATCH ? 3u : 2u;
    uint32_t k[16] = {(uint32_t)(off - base), slot + BOOT_RENDER_IND_ARGS, words, copy, op->kind == BOOT_RENDER_IND_DISPATCH,
                      0, slot, op->stage_words, op->patch_word, patch_src, patch_words, 0, 0, 0, 0, 0};
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, boot_render_ind.root);
    ID3D12GraphicsCommandList_SetPipelineState(cl, boot_render_ind.pso); ++boot_render_bind_epoch;   /* replaced the graphics pipeline */
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 16, k, 0);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(r->ring) + op->stage_off);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(r->region[reg]) + base);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 3, ID3D12Resource_GetGPUVirtualAddress(boot_render_ind_ring));
    boot_dred_work(cl, BOOT_DRED_DISPATCH, BOOT_DRED_COMPUTE, NULL, NULL, op->va, boot_render_exec_id, boot_render_watch_event);
    BOOT_GP_DISPATCH("indirect pre-pass", 0, cl, 1, 1, 1);
    boot_render_ind_ring_to(r, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);   /* the writes are visible to both reads */
    ++boot_render_ind_prepasses;
    return 1;
}
/* The command itself (inside the caller's predication bracket for draws). */
static inline void boot_render_ind_execute(BootRender *r, const BootRenderIndOp *op) {
    boot_render_ind_ring_to(r, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ID3D12GraphicsCommandList_ExecuteIndirect(r->cl, boot_render_ind.sig[op->kind], 1, boot_render_ind_ring,
                                              (uint64_t)op->slot * BOOT_RENDER_IND_SLOT + BOOT_RENDER_IND_ARGS, NULL, 0);
    ++boot_render_ind_executed[op->kind];
}
/* Index buffer view from `va` over the contiguous region bytes behind it (StartIndexLocation picks the first index on
 * the GPU). The walk over the page table is cached per address and mapping generation. 0 = not in guest memory. */
static inline int boot_render_ind_ib(BootRender *r, uint64_t va, uint32_t idx_bytes, D3D12_INDEX_BUFFER_VIEW *ib) {
    static struct { BootRender *r; uint64_t va, gen, span; D3D12_GPU_VIRTUAL_ADDRESS gpu; } c[8]; static unsigned next;
    unsigned i = 0; while (i < 8 && !(c[i].r == r && c[i].va == va && c[i].gen == boot_render_map_gen)) ++i;
    if (i == 8) {
        int reg; uint64_t off, span = boot_render_region_span(r, va, BOOT_RENDER_IND_IB_MAX, &reg, &off);
        if (span < idx_bytes) return 0;
        i = next; next = (next + 1) & 7;
        c[i].r = r; c[i].va = va; c[i].gen = boot_render_map_gen; c[i].span = span;
        c[i].gpu = ID3D12Resource_GetGPUVirtualAddress(r->region[reg]) + off;
    }
    ib->BufferLocation = c[i].gpu; ib->SizeInBytes = (UINT)(c[i].span & ~(uint64_t)(idx_bytes - 1));
    ib->Format = idx_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    return 1;
}

/* ---------------------------------------------------------------- suspect-diag (see the top of this header) */
static const char *const boot_sd_kind_name[BOOT_SD_K_N] = {"cb-copy", "cb-direct", "index", "texture", "vertex", "cb-snap", "vertex-snap", "index-zc", "index-snap"};
static inline uint32_t boot_sd_env_u32(const char *name, uint32_t def, uint32_t lo, uint32_t hi) {
    char v[16]; DWORD k = GetEnvironmentVariableA(name, v, sizeof v);
    if (!k || k >= sizeof v) return def;
    unsigned long x = strtoul(v, NULL, 10);
    return x < lo ? lo : x > hi ? hi : (uint32_t)x;
}
static inline const char *boot_sd_env_raw(const char *name, char *buf, DWORD cap) {
    DWORD k = GetEnvironmentVariableA(name, buf, cap);
    return k && k < cap ? buf : "(unset)";
}
static inline int boot_sd_hexval(char ch) {
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}
/* shader-labels.tsv: <canonical id hex>\t<kind>\t<variant>\t<stem>\t<family>\t... ; key = the id's first 8 bytes as memcpy'd */
static inline void boot_sd_labels_load(BootSdState *S) {
    char path[600]; DWORD k = GetEnvironmentVariableA("TOTK_SHADER_LABELS", path, sizeof path);
    const char *tries[3] = {k && k < sizeof path ? path : NULL, "startup\\shader-labels.tsv", "shader-labels.tsv"};
    FILE *f = NULL;
    for (unsigned t = 0; t < 3 && !f; ++t) if (tries[t]) f = fopen(tries[t], "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = n > 0 && n < (64L << 20) ? (char *)malloc((size_t)n + 1) : NULL;
    BootSdLabel *tab = text ? (BootSdLabel *)calloc(BOOT_SD_LABELS, sizeof *tab) : NULL;
    if (!tab || fread(text, 1, (size_t)n, f) != (size_t)n) { free(text); free(tab); fclose(f); return; }
    fclose(f); text[n] = 0;
    uint32_t count = 0;
    for (char *line = text; *line; ) {
        char *end = strchr(line, '\n'); char *next = end ? end + 1 : line + strlen(line);
        if (end) { *end = 0; if (end > line && end[-1] == '\r') end[-1] = 0; }
        if (line[0] != '#') {
            char *field[5] = {line, NULL, NULL, NULL, NULL}; unsigned nf = 1;
            for (char *q = line; *q && nf < 5; ++q) if (*q == '\t') { *q = 0; field[nf++] = q + 1; }
            if (nf == 5) { char *t = strchr(field[4], '\t'); if (t) *t = 0; }
            uint8_t b[8]; int ok = nf == 5 && strlen(field[0]) >= 16;
            for (unsigned i = 0; ok && i < 8; ++i) {
                int hi = boot_sd_hexval(field[0][2 * i]), lo = boot_sd_hexval(field[0][2 * i + 1]);
                if (hi < 0 || lo < 0) ok = 0; else b[i] = (uint8_t)(hi << 4 | lo);
            }
            if (ok && count < BOOT_SD_LABELS / 2) {
                uint64_t key; memcpy(&key, b, 8); if (!key) key = 1;
                uint32_t h = (uint32_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 48) & (BOOT_SD_LABELS - 1);
                while (tab[h].key && tab[h].key != key) h = (h + 1) & (BOOT_SD_LABELS - 1);
                if (!tab[h].key) { tab[h].key = key; tab[h].family = field[4]; ++count; }
                else if (field[1][0] == 'v') tab[h].family = field[4];   /* a vertex shader row wins */
            }
        }
        line = next;
    }
    S->labels_text = text; S->labels = tab; S->nlabels = count;
}
static inline const char *boot_sd_family(const BootSdState *S, uint64_t key) {
    if (!S->labels) return "?";
    if (!key) key = 1;
    for (uint32_t h = (uint32_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 48) & (BOOT_SD_LABELS - 1), n = 0; n < BOOT_SD_LABELS && S->labels[h].key; ++n, h = (h + 1) & (BOOT_SD_LABELS - 1))
        if (S->labels[h].key == key) return S->labels[h].family;
    return "?";
}
static inline void boot_sd_event(BootSdState *S, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void boot_sd_event(BootSdState *S, const char *fmt, ...) {
    if (!S->log) return;
    va_list ap; va_start(ap, fmt); vfprintf(S->log, fmt, ap); va_end(ap);
    fputc('\n', S->log); S->dirty = 1;
}
/* State on first use (always under the renderer lock: draw path, submit-call end, present). NULL: no memory (switch off). */
static inline __attribute__((noinline)) BootSdState *boot_sd_init(void) {
    BootSdState *S = (BootSdState *)calloc(1, sizeof *S);
    if (S) {
        S->rec = (BootSdRec *)calloc(BOOT_SD_RECS, sizeof *S->rec);
        S->seen = (uint64_t *)calloc(BOOT_SD_SEEN, sizeof *S->seen); S->seen_stamp = (uint32_t *)calloc(BOOT_SD_SEEN, sizeof *S->seen_stamp);
        S->vs = (BootSdVs *)calloc(BOOT_SD_VS, sizeof *S->vs);
        S->vs_tex = (uint32_t *)calloc(BOOT_SD_TIC, sizeof *S->vs_tex); S->vs_tex_va = (uint64_t *)calloc(BOOT_SD_TIC, sizeof *S->vs_tex_va);
        S->evicted = (uint8_t *)calloc(BOOT_SD_TIC, 1);
    }
    if (!S || !S->rec || !S->seen || !S->seen_stamp || !S->vs || !S->vs_tex || !S->vs_tex_va || !S->evicted) {
        if (S) { free(S->rec); free(S->seen); free(S->seen_stamp); free(S->vs); free(S->vs_tex); free(S->vs_tex_va); free(S->evicted); free(S); }
        boot_sd_mode = 0; return NULL;
    }
    S->stamp = 1;
    S->budget = (uint64_t)boot_sd_env_u32("TOTK_SUSPECT_DIAG_MB", 4, 1, 1024) << 20;
    S->events_max = boot_sd_env_u32("TOTK_SUSPECT_DIAG_EVENTS", 40, 0, 100000);
    S->tex_n = boot_sd_env_u32("TOTK_SUSPECT_DIAG_TEX_N", 600, 1, 1000000);
    S->gr_every = boot_sd_env_u32("TOTK_SUSPECT_DIAG_GR_EVERY", 30, 1, 100000);
    S->gr_pages_max = boot_sd_env_u32("TOTK_SUSPECT_DIAG_GR_PAGES", 48, 1, 4096);
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); S->qpf = q.QuadPart > 0 ? (uint64_t)q.QuadPart : 1; }
    boot_sd_labels_load(S);
    CreateDirectoryA("frames", NULL);
    MoveFileExA("frames\\suspect-diag.log", "frames\\suspect-diag.prev.log", MOVEFILE_REPLACE_EXISTING);
    S->log = fopen("frames\\suspect-diag.log", "w");
    if (S->log) {
        setvbuf(S->log, NULL, _IOFBF, 1 << 16);
        char a[32], b[32], c[32], d[32], e[32], f[32], g[32];
        SYSTEMTIME t; GetLocalTime(&t);
        boot_sd_event(S, "# suspect-diag v1 %02u:%02u:%02u (TOTK_SUSPECT_DIAG=%d%s): TOTK_RERECORD_VERSION=%s TOTK_INSTANCE_ID_ZERO_BASED=%s TOTK_GPU_RESIDENT=%s "
                         "TOTK_TEX_LIVE_TRIM=%s TOTK_TEX_MIN_AGE=%s TOTK_SHADER_BATCH4=%s TOTK_OVERLAP=%s; budget %llu MiB per kind and call, %u events per suspect, "
                         "S4 window %u presents, S3 every %u presents (%u pages); shader labels %u",
                       t.wHour, t.wMinute, t.wSecond, boot_sd_mode, boot_sd_mode == 2 ? ": S1 on material draws only" : "",
                       boot_sd_env_raw("TOTK_RERECORD_VERSION", a, sizeof a), boot_sd_env_raw("TOTK_INSTANCE_ID_ZERO_BASED", b, sizeof b),
                       boot_sd_env_raw("TOTK_GPU_RESIDENT", c, sizeof c), boot_sd_env_raw("TOTK_TEX_LIVE_TRIM", d, sizeof d), boot_sd_env_raw("TOTK_TEX_MIN_AGE", e, sizeof e),
                       boot_sd_env_raw("TOTK_SHADER_BATCH4", f, sizeof f), boot_sd_env_raw("TOTK_OVERLAP", g, sizeof g),
                       (unsigned long long)(S->budget >> 20), S->events_max, S->tex_n, S->gr_every, S->gr_pages_max, S->nlabels);
        boot_sd_event(S, "# frame = frame-N.bmp / glitch.log 'GLITCH present N' number of the frame being built; sub = submission (boot_render_exec_id); "
                         "draw = draw number within the submission; vs = first 8 bytes of the vertex shader id; key = draw-census key (frames\\draw-census.log)");
        fflush(S->log);
    }
    boot_sd_st = S;
    return S;
}
static inline BootSdState *boot_sd_state(void) { return boot_sd_st ? boot_sd_st : boot_sd_mode > 0 ? boot_sd_init() : NULL; }
/* S2: what a vertex shader's .hlsl (next to its .vs.dxil) says: the attributes it fetches (vin_attr4(g...)), whether it reads
 * InstanceId (the token iid outside the prologue and the attribute fetches) and whether it was translated with K2 (its prologue
 * adds host_iid_offset()). Once per vertex shader, at its load. */
static inline void boot_sd_vs_scan(const uint8_t id[32], const char *dxil_path) {
    BootSdState *S = boot_sd_state(); if (!S || !dxil_path) return;
    uint64_t key; memcpy(&key, id, 8); if (!key) key = 1;
    uint32_t h = (uint32_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 50) & (BOOT_SD_VS - 1), n = 0;
    while (S->vs[h].key && S->vs[h].key != key && n < BOOT_SD_VS) { h = (h + 1) & (BOOT_SD_VS - 1); ++n; }
    if (n == BOOT_SD_VS || S->vs[h].key == key) return;
    BootSdVs *v = &S->vs[h]; v->key = key; v->flags = 1; v->attr_mask = 0;
    char hp[960]; const char *dot = strstr(dxil_path, ".vs.dxil"); size_t stem = dot ? (size_t)(dot - dxil_path) : 0;
    if (!dot || stem + 6 > sizeof hp) { v->flags |= 8; return; }
    memcpy(hp, dxil_path, stem); memcpy(hp + stem, ".hlsl", 6);
    FILE *f = fopen(hp, "rb"); if (!f) { v->flags |= 8; return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = len > 0 && len < (16L << 20) ? (char *)malloc((size_t)len + 1) : NULL;
    if (!t || fread(t, 1, (size_t)len, f) != (size_t)len) { free(t); fclose(f); v->flags |= 8; return; }
    fclose(f); t[len] = 0;
    for (char *line = t; *line; ) {
        char *end = strchr(line, '\n'); char *next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (strstr(line, "host_iid_offset()")) v->flags |= 4;
        const char *fa = strstr(line, "vin_attr4(");
        if (fa) { unsigned g = (unsigned)strtoul(fa + 10, NULL, 10); if (g < 32) v->attr_mask |= 1u << g; }
        else if (!strstr(line, "iid_")) {
            for (const char *q = strstr(line, "iid"); q; q = strstr(q + 3, "iid")) {
                char before = q > line ? q[-1] : ' ', after = q[3];
                int idb = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || (before >= '0' && before <= '9') || before == '_';
                int ida = (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') || (after >= '0' && after <= '9') || after == '_';
                if (!idb && !ida) { v->flags |= 2; break; }
            }
        }
        line = next;
    }
    free(t);
}
static inline const BootSdVs *boot_sd_vs_info(const BootSdState *S, uint64_t key) {
    if (!key) key = 1;
    for (uint32_t h = (uint32_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 50) & (BOOT_SD_VS - 1), n = 0; n < BOOT_SD_VS && S->vs[h].key; ++n, h = (h + 1) & (BOOT_SD_VS - 1))
        if (S->vs[h].key == key) return &S->vs[h];
    return NULL;
}
/* Guest GPU VA -> host pointer through the CPU page table, as boot_render_guest but without its GPU-write guard (no wait). */
static inline const uint8_t *boot_sd_guest(const BootRender *r, uint64_t va, uint64_t size, uint32_t *reg) {
    if ((va >> 12) < r->va_base_page || !size) return NULL;
    uint64_t page = (va >> 12) - r->va_base_page;
    if (page >= r->ptable_pages) return NULL;
    uint32_t e = r->ptable_cpu[page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count || !r->region_cpu[e >> 24]) return NULL;
    uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    if (off > r->region_size[e >> 24] || size > r->region_size[e >> 24] - off) return NULL;
    uint64_t last = ((va & 0xfff) + size - 1) >> 12;
    for (uint64_t q = 1; q <= last; ++q) if (page + q >= r->ptable_pages || r->ptable_cpu[page + q] != e + (uint32_t)q) return NULL;
    if (reg) *reg = e >> 24;
    return r->region_cpu[e >> 24] + off;
}
/* S1: a range read during the call (p: where it was read; hp: the bytes to hash now, p if NULL; a texture build passes its
 * build checksum instead, see boot_sd_tex_build). Duplicates within the call, budget overflow and ranges not in one region
 * are counted, not recorded. */
static inline void boot_sd_record(BootRender *r, unsigned kind, uint64_t va, const uint8_t *p, const uint8_t *hp, uint64_t n, int hashed, uint64_t hash) {
    BootSdState *S = boot_sd_state(); if (!S || !p || !n || kind >= BOOT_SD_K_N) return;
    if (S->in_draw) { if (kind == BOOT_SD_K_CB) ++S->draw_copies; else if (kind == BOOT_SD_K_DIRECT) ++S->draw_direct; }
    if (boot_sd_mode == 2 && !S->material) return;
    const uint64_t dk = (uint64_t)(uintptr_t)p ^ ((n > 0xffff ? 0xffff : n) << 48) ^ ((uint64_t)kind << 44);
    for (uint32_t h = (uint32_t)((dk * UINT64_C(0x9E3779B97F4A7C15)) >> 48) & (BOOT_SD_SEEN - 1), probe = 0; ; h = (h + 1) & (BOOT_SD_SEEN - 1), ++probe) {
        if (S->seen_stamp[h] != S->stamp) { S->seen_stamp[h] = S->stamp; S->seen[h] = dk; break; }
        if (S->seen[h] == dk) { ++S->c.dup[kind]; return; }
        if (probe >= 16) break;   /* crowded: record it anyway */
    }
    if (S->nrec >= BOOT_SD_RECS || S->kind_bytes[kind] + n > S->budget) { ++S->c.unsampled[kind]; return; }
    uint32_t reg = 0;
    const uint8_t *q = boot_sd_guest(r, va, n, &reg);
    if (q != p) { ++S->c.unsampled[kind]; return; }   /* not where the page table says (or not contiguous): not re-checkable */
    BootSdRec *e = &S->rec[S->nrec++];
    e->base = r->region_cpu[reg]; e->reg = reg; e->off = (uint64_t)(p - e->base); e->va = va; e->n = (uint32_t)n; e->kind = kind;
    const BootGrView *v = &boot_gr_view[reg];
    if (v->cpu && v->cpu == e->base) { e->lo = v->lo; e->gen = v->gen; } else { e->lo = 0; e->gen = 0; }
    e->hash = hashed ? hash : boot_sd_hash(hp ? hp : p, n);
    e->vs8 = S->in_draw ? S->vs8 : 0; e->key = S->in_draw ? S->key : 0; e->draw = S->in_draw ? S->draw : 0;
    e->exec = (uint32_t)boot_render_exec_id; e->frame = (uint32_t)S->frame;
    e->epoch = boot_render_cb_epoch; e->big = boot_render_big_writes;   /* host writes after this are told apart (boot_render_cb_written) */
    S->call_bytes += n; S->kind_bytes[kind] += n; S->c.bytes[kind] += n;
}
static inline void boot_sd_note(BootRender *r, unsigned kind, uint64_t va, const uint8_t *p, const uint8_t *hp, uint64_t n) {
    if (n > 0xffffffffu) { BootSdState *S = boot_sd_state(); if (S && kind < BOOT_SD_K_N) ++S->c.unsampled[kind]; return; }
    boot_sd_record(r, kind, va, p, hp, n, 0, 0);
}
/* S1 at the end of a submit call (renderer lock held): every recorded range hashed again. kept: versions kept without a wait
 * during the call (boot_nvn_rv), rerecords: recordings replaced during it. */
static inline void boot_sd_recheck(BootRender *r, uint64_t kept, uint64_t rerecords) {
    BootSdState *S = boot_sd_state(); if (!S) return;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    ++S->c.calls; S->c.rerecords += rerecords;
    if (kept) { ++S->c.calls_kept; S->c.kept += kept; }
    unsigned changed = 0;
    for (uint32_t i = 0; i < S->nrec; ++i) {
        const BootSdRec *e = &S->rec[i];
        int ok = e->reg < r->region_count && r->region_cpu[e->reg] == e->base && e->off + e->n <= r->region_size[e->reg];
        if (ok) {
            const BootGrView *v = &boot_gr_view[e->reg];
            const int view_now = v->cpu && v->cpu == e->base;
            ok = e->gen ? (view_now && v->lo == e->lo && v->gen == e->gen && !v->stale) : !view_now;
        }
        if (!ok) { ++S->c.gone[e->kind]; continue; }
        const uint8_t *p = e->base + e->off;
        const uint64_t h = e->kind == BOOT_SD_K_TEX ? boot_render_tex_checksum(p, e->n) : boot_sd_hash(p, e->n);
        ++S->c.checked[e->kind];
        if (h == e->hash) continue;
        /* written by the host itself since (executor CPU writes, boot_render_invalidate; GPU-ordered writes, boot_render_gpu_write): expected */
        int host = !boot_render_cb_clean(e->va, e->n, e->epoch + 1) || boot_render_big_writes != e->big || boot_render_gw_all;
        for (unsigned g = 0; !host && g < boot_render_gw_count; ++g) host = boot_render_gw[g].va < e->va + e->n && e->va < boot_render_gw[g].va + boot_render_gw[g].size;
        if (host) { ++S->c.hostw[e->kind]; continue; }
        ++changed;
        if (kept) ++S->c.ch_kept[e->kind]; else ++S->c.ch_plain[e->kind];
        if (e->kind == BOOT_SD_K_DIRECT || e->kind == BOOT_SD_K_VERTEX || e->kind == BOOT_SD_K_IZC)   /* data-snapshot: copy it from now on */
            boot_ds_s1_changed(r, e->va, e->n, e->kind == BOOT_SD_K_DIRECT ? 0u : e->kind == BOOT_SD_K_VERTEX ? 1u : 2u);
        if (S->ev_s1[e->kind] < S->events_max) {
            ++S->ev_s1[e->kind];
            boot_sd_event(S, "S1 CHANGED during the call: frame %u sub %u draw %u kind %s va %llx size %u vs %016llx family %s key %016llx; call: versions kept %llu, "
                             "re-records %llu, ranges %u (%llu KiB)%s",
                           e->frame, e->exec, e->draw, boot_sd_kind_name[e->kind], (unsigned long long)e->va, e->n, (unsigned long long)__builtin_bswap64(e->vs8),
                           e->vs8 ? boot_sd_family(S, e->vs8) : "-", (unsigned long long)e->key, (unsigned long long)kept, (unsigned long long)rerecords,
                           S->nrec, (unsigned long long)(S->call_bytes >> 10), e->gen ? " (span view)" : "");
        }
    }
    (void)changed;
    S->nrec = 0; S->call_bytes = 0; memset(S->kind_bytes, 0, sizeof S->kind_bytes);
    if (++S->stamp == 0) { memset(S->seen_stamp, 0, BOOT_SD_SEEN * sizeof *S->seen_stamp); S->stamp = 1; }
    QueryPerformanceCounter(&q1); S->c.recheck_qpc += (uint64_t)(q1.QuadPart - q0.QuadPart);
}
/* S4: a slot evicted by the idle trim (live 0) or the live trim (live 1). */
static inline void boot_sd_tex_evict(uint32_t index, int live) {
    BootSdState *S = boot_sd_state(); if (!S) return;
    ++S->c.tx_evict; if (live) ++S->c.tx_evict_live;
    if (index >= BOOT_SD_TIC) return;
    S->evicted[index] = 1;
    const uint32_t f = S->vs_tex[index];
    if (!f || (uint32_t)S->frame + 1 - f > S->tex_n) return;
    ++S->c.tx_evict_vs;
    if (S->ev_s4 < S->events_max) {
        ++S->ev_s4;
        boot_sd_event(S, "S4 EVICT frame %llu sub %llu: TIC slot %u (va %llx) by the %s trim, last bound by a vertex shader %u presents ago",
                       (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, index, (unsigned long long)S->vs_tex_va[index],
                       live ? "live" : "idle", (uint32_t)S->frame + 1 - f);
    }
}
/* A guest texture build of slot `index` from [va, va + n) (src), build checksum `sum` (boot_render_tex_checksum): S1 range,
 * and S4 when a vertex shader bound the slot recently. */
static inline void boot_sd_tex_build(BootRender *r, uint32_t index, uint64_t va, const uint8_t *src, uint64_t n, uint64_t sum) {
    BootSdState *S = boot_sd_state(); if (!S) return;
    ++S->c.tx_builds;
    if (n <= 0xffffffffu) boot_sd_record(r, BOOT_SD_K_TEX, va, src, NULL, n, 1, sum); else ++S->c.unsampled[BOOT_SD_K_TEX];
    if (index >= BOOT_SD_TIC) return;
    const uint32_t f = S->vs_tex[index]; const int ev = S->evicted[index]; S->evicted[index] = 0;
    if (!f || (uint32_t)S->frame + 1 - f > S->tex_n) return;
    if (S->vs_tex_va[index] != va) { ++S->c.tx_reuse_vs; return; }   /* the slot now holds another image */
    ++S->c.tx_rebuild_vs; if (ev) ++S->c.tx_rebuild_vs_ev;
    if (S->ev_s4 < S->events_max) {
        ++S->ev_s4;
        boot_sd_event(S, "S4 REBUILD frame %llu sub %llu draw %u: TIC slot %u (va %llx, %llu KiB) built again %s, last bound by a vertex shader %u presents ago; "
                         "building draw vs %016llx family %s key %016llx",
                       (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, S->in_draw ? S->draw : 0, index, (unsigned long long)va,
                       (unsigned long long)(n >> 10), ev ? "after a trim eviction" : "(not evicted: re-hash, TIC or format change)", (uint32_t)S->frame + 1 - f,
                       (unsigned long long)__builtin_bswap64(S->in_draw ? S->vs8 : 0), S->in_draw ? S->fam : "-", (unsigned long long)(S->in_draw ? S->key : 0));
    }
}

/* data-snapshot core (pure: no renderer state, no D3D12; tests/test_ds.c runs it offline). The learned-chunk table, the
 * class byte table, the detector's sampling rule and windows, the call-start copy (shadow) and the per-segment copy arena,
 * the pending GPU writes, the stored-page history and the constant-buffer plan (v2fix: play-work/data-snapshot/v2fix).
 * Chunks are 64 KiB of guest GPU VA; a chunk is "dynamic" once the detector (or S1) saw the game rewrite a range in it while
 * a submit call was being translated. */
#define BOOT_DS_CHUNK_SHIFT 16
#define BOOT_DS_CHUNK_BYTES (UINT64_C(1) << BOOT_DS_CHUNK_SHIFT)
#define BOOT_DS_PARTS 16   /* arena parts: one per ring segment (BOOT_RENDER_SEGMENTS) */
/* class bits of a chunk (OR of everything that maps into it) */
enum { BOOT_DS_C_DYN = 1,      /* learned: the game rewrites ranges here while a submit is translated */
       BOOT_DS_C_CPU = 2,      /* NVN memory pool with CPU access (CPU_UNCACHED / CPU_CACHED) */
       BOOT_DS_C_GPU = 4,      /* NVN memory pool with CPU_NO_ACCESS: the game's CPU never writes it */
       BOOT_DS_C_NOPOOL = 8,   /* GPU-mapped, no pool seen for its CPU address */
       BOOT_DS_C_HOST = 16,    /* the executor's own block (driver buffer, copy arena): never taken, never sampled */
       BOOT_DS_C_CODE = 32 };  /* NVN memory pool with SHADER_CODE (0x40): program constants, written at load only */
enum { BOOT_DS_K_CB, BOOT_DS_K_VERTEX, BOOT_DS_K_INDEX, BOOT_DS_K_N };
/* take decisions */
enum { BOOT_DS_T_NO = 0, BOOT_DS_T_LEARNED = 1, BOOT_DS_T_ALL = 2, BOOT_DS_T_INSTANCE = 3, BOOT_DS_T_EAGER = 4, BOOT_DS_T_N = 5 };
typedef struct BootDsChunk {
    uint64_t key;                                   /* chunk number + 1; 0 = empty slot */
    uint64_t learned, last_change, last_use;        /* call ids */
    uint64_t shadow_call, shadow_epoch, shadow_big, shadow_gen;
    uint32_t changes, kinds, shadow_mask, shadow_off;   /* shadow_mask: 4 KiB pages copied at the call start */
} BootDsChunk;
typedef struct BootDsCore {
    BootDsChunk *chunk; uint32_t cap, n;            /* open addressing, cap a power of two, at most 3/4 full */
    uint32_t *occ;                                  /* v2fix: the n occupied slots (the call-start copy visits n, not 2 x cap: 16384) */
    uint8_t *cls; uint64_t cls_base; uint32_t cls_n, cls_cap;   /* class byte of chunk cls_base + i */
    uint8_t *shadow; uint32_t shadow_cap, shadow_used; uint64_t shadow_call;
    uint64_t *shadow_chunks; uint32_t shadow_nchunks;   /* v2fix: the chunks this call's shadow holds (boot_ds_shadow_drop) */
    uint64_t call;                                  /* submit calls so far (the current one) */
} BootDsCore;
static inline uint32_t boot_ds_hash32(uint64_t k) {
    k ^= k >> 33; k *= UINT64_C(0xff51afd7ed558ccd); k ^= k >> 33; k *= UINT64_C(0xc4ceb9fe1a85ec53); k ^= k >> 33;
    return (uint32_t)k;
}
/* 64-bit hash of n bytes (the suspect-diag hash: a changed word always changes its lane). */
static inline uint64_t boot_ds_hash(const uint8_t *p, uint64_t n, uint64_t h0) {
    const uint64_t P = UINT64_C(0x100000001b3) | (UINT64_C(0x9e3779b9) << 40);
    uint64_t a = h0 ^ n ^ UINT64_C(0x243f6a8885a308d3), b = UINT64_C(0x13198a2e03707344), c = UINT64_C(0xa4093822299f31d0), d = UINT64_C(0x082efa98ec4e6c89), i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4]; memcpy(w, p + i, 32);
        a = (a ^ w[0]) * P; b = (b ^ w[1]) * P; c = (c ^ w[2]) * P; d = (d ^ w[3]) * P;
    }
    for (; i < n; ++i) a = (a ^ p[i]) * P;
    uint64_t h = a ^ ((b << 17) | (b >> 47)) ^ ((c << 31) | (c >> 33)) ^ ((d << 47) | (d >> 17));
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd); h ^= h >> 33;
    return h;
}
/* ---- class table */
static inline uint8_t boot_ds_cls_at(const BootDsCore *d, uint64_t va) {
    const uint64_t c = (va >> BOOT_DS_CHUNK_SHIFT) - d->cls_base;
    return d->cls && c < d->cls_n ? d->cls[c] : 0;
}
/* OR of the classes of the chunks [va, va + n) touches (n >= 1, va + n without wrap); chunks outside the table add nothing.
 * At most 256 chunks (16 MiB) are looked at: callers never take larger ranges. */
static inline unsigned boot_ds_cls_span(const BootDsCore *d, uint64_t va, uint64_t n) {
    if (!d->cls || !n) return 0;
    const uint64_t i0 = (va >> BOOT_DS_CHUNK_SHIFT) - d->cls_base, i1 = ((va + n - 1) >> BOOT_DS_CHUNK_SHIFT) - d->cls_base;
    if (i0 < d->cls_n && i1 < d->cls_n && i1 - i0 <= 1) return (unsigned)(d->cls[i0] | d->cls[i1]);   /* constant buffers: 1-2 chunks */
    unsigned bits = 0;
    for (uint64_t c = va >> BOOT_DS_CHUNK_SHIFT, last = (va + n - 1) >> BOOT_DS_CHUNK_SHIFT, k = 0; c <= last && k < 256; ++c, ++k) {
        const uint64_t i = c - d->cls_base;
        if (i < d->cls_n) bits |= d->cls[i];
    }
    return bits;
}
/* Clears the table to cover chunks [base, base + n); 0 = no memory (the table stays empty: nothing classified). */
static inline int boot_ds_cls_reset(BootDsCore *d, uint64_t base, uint32_t n) {
    if (n > d->cls_cap) {
        uint8_t *t = (uint8_t *)realloc(d->cls, n);
        if (!t) { d->cls_n = 0; return 0; }
        d->cls = t; d->cls_cap = n;
    }
    d->cls_base = base; d->cls_n = n;
    if (n) memset(d->cls, 0, n);
    return 1;
}
static inline void boot_ds_cls_mark(BootDsCore *d, uint64_t va, uint64_t n, unsigned bits) {
    if (!d->cls || !n || va > UINT64_MAX - n) return;
    uint64_t c0 = va >> BOOT_DS_CHUNK_SHIFT, c1 = (va + n - 1) >> BOOT_DS_CHUNK_SHIFT;
    if (c1 < d->cls_base || c0 >= d->cls_base + d->cls_n) return;
    if (c0 < d->cls_base) c0 = d->cls_base;
    if (c1 >= d->cls_base + d->cls_n) c1 = d->cls_base + d->cls_n - 1;
    for (uint64_t c = c0; c <= c1; ++c) d->cls[c - d->cls_base] |= (uint8_t)bits;
}
/* ---- learned chunks */
static inline int boot_ds_core_init(BootDsCore *d, uint32_t cap, uint32_t shadow_bytes) {
    memset(d, 0, sizeof *d);
    uint32_t c = 64; while (c < cap) c <<= 1;
    d->chunk = (BootDsChunk *)calloc(c, sizeof *d->chunk);
    d->occ = (uint32_t *)calloc(c, sizeof *d->occ);
    if (!d->chunk || !d->occ) { free(d->chunk); free(d->occ); d->chunk = NULL; d->occ = NULL; return 0; }
    d->cap = c;
    if (shadow_bytes) {
        d->shadow = (uint8_t *)malloc(shadow_bytes);
        d->shadow_chunks = (uint64_t *)calloc(shadow_bytes / BOOT_DS_CHUNK_BYTES + 1, sizeof *d->shadow_chunks);
        if (d->shadow && d->shadow_chunks) d->shadow_cap = shadow_bytes;
        else { free(d->shadow); free(d->shadow_chunks); d->shadow = NULL; d->shadow_chunks = NULL; }
    }
    return 1;
}
static inline BootDsChunk *boot_ds_chunk(BootDsCore *d, uint64_t chunk, int create) {
    if (!d->chunk) return NULL;
    const uint64_t key = chunk + 1; const uint32_t m = d->cap - 1;
    for (uint32_t i = boot_ds_hash32(key) & m, probe = 0; probe < d->cap; ++probe, i = (i + 1) & m) {
        BootDsChunk *e = &d->chunk[i];
        if (e->key == key) return e;
        if (!e->key) {
            if (!create || (uint64_t)d->n * 4 >= (uint64_t)d->cap * 3) return NULL;
            memset(e, 0, sizeof *e); e->key = key; d->occ[d->n++] = i;
            return e;
        }
    }
    return NULL;
}
/* The occupied slots, j < boot_ds_scan_n: entries are only added between rebuilds (boot_ds_age_core rebuilds the list too). */
static inline uint32_t boot_ds_scan_n(const BootDsCore *d) { return d->chunk ? d->n : 0; }
static inline BootDsChunk *boot_ds_scan_at(BootDsCore *d, uint32_t j) { return &d->chunk[d->occ[j]]; }
/* The chunks of [va, va + n) become dynamic (at most 64 chunks). Returns how many were new. */
static inline unsigned boot_ds_learn_core(BootDsCore *d, uint64_t va, uint64_t n, unsigned kind) {
    if (!n || va > UINT64_MAX - n) return 0;
    unsigned fresh = 0;
    for (uint64_t c = va >> BOOT_DS_CHUNK_SHIFT, last = (va + n - 1) >> BOOT_DS_CHUNK_SHIFT, k = 0; c <= last && k < 64; ++c, ++k) {
        BootDsChunk *e = boot_ds_chunk(d, c, 0);
        if (!e) {
            e = boot_ds_chunk(d, c, 1);
            if (!e) continue;   /* table full: not learned (counted by the caller through the return) */
            e->learned = d->call; e->last_use = d->call; ++fresh;
        }
        e->last_change = d->call; ++e->changes; e->kinds |= 1u << (kind & 7);
        const uint64_t i = c - d->cls_base;
        if (d->cls && i < d->cls_n) d->cls[i] |= BOOT_DS_C_DYN;
    }
    return fresh;
}
/* Marks the dynamic chunks in a freshly reset class table again. */
static inline void boot_ds_cls_relearn(BootDsCore *d) {
    for (uint32_t j = 0, n = boot_ds_scan_n(d); j < n; ++j) boot_ds_cls_mark(d, (boot_ds_scan_at(d, j)->key - 1) << BOOT_DS_CHUNK_SHIFT, 1, BOOT_DS_C_DYN);
}
/* Drops chunks no snapshot used for more than max_age calls (and learned more than max_age calls ago): their class bit goes
 * too. The table is rebuilt (no tombstones). Returns the number dropped; 0 without memory for the rebuild. */
static inline unsigned boot_ds_age_core(BootDsCore *d, uint64_t max_age) {
    if (!d->chunk || !d->n) return 0;
    unsigned drop = 0;
    for (uint32_t i = 0; i < d->cap; ++i) {
        const BootDsChunk *e = &d->chunk[i];
        if (e->key && d->call - e->last_use > max_age && d->call - e->learned > max_age) ++drop;
    }
    if (!drop) return 0;
    BootDsChunk *old = d->chunk, *t = (BootDsChunk *)calloc(d->cap, sizeof *t);
    if (!t) return 0;
    d->chunk = t; d->n = 0;
    for (uint32_t i = 0; i < d->cap; ++i) {
        const BootDsChunk *e = &old[i];
        if (!e->key) continue;
        if (d->call - e->last_use > max_age && d->call - e->learned > max_age) {
            const uint64_t k = (e->key - 1) - d->cls_base;
            if (d->cls && k < d->cls_n) d->cls[k] &= (uint8_t)~BOOT_DS_C_DYN;
            continue;
        }
        BootDsChunk *f = boot_ds_chunk(d, e->key - 1, 1);
        if (f) *f = *e;
    }
    free(old);
    return drop;
}
/* ---- decision */
/* Whether a range with class bits `cls` is read from a copy: mode 0 never, 1 learned chunks (and, with the per-instance rule,
 * small per-instance streams; with the eager rule, ranges in a pool the CPU can write: eager 1 not in shader-code pools, 2 in
 * those too), 2 everything the CPU may write. GPU-only pools and the executor's block never. The caller still keeps a range
 * that GPU work recorded earlier may write zero-copy (GPU order). */
static inline int boot_ds_take_core(int mode, unsigned cls, int instance_rule, int eager) {
    if (mode <= 0) return BOOT_DS_T_NO;
    if (cls & BOOT_DS_C_HOST) return BOOT_DS_T_NO;
    if ((cls & BOOT_DS_C_GPU) && !(cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL))) return BOOT_DS_T_NO;
    if (cls & BOOT_DS_C_DYN) return BOOT_DS_T_LEARNED;
    if (mode >= 2) return BOOT_DS_T_ALL;
    if (eager && (cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL)) && (eager >= 2 || !(cls & BOOT_DS_C_CODE))) return BOOT_DS_T_EAGER;
    if (instance_rule) return BOOT_DS_T_INSTANCE;
    return BOOT_DS_T_NO;
}
/* ---- detector sampling */
/* A range is sampled in the calls where its hash phase meets the call counter: exactly once in every `period` (a power of
 * two) consecutive calls. Two multiplies: it runs for every zero-copy binding the CPU may write. */
static inline int boot_ds_pick(uint64_t va, uint64_t n, uint64_t call, uint32_t period) {
    const uint32_t h = ((uint32_t)(va >> 8) ^ ((uint32_t)n * 0x85ebca6bu)) * 0x9e3779b1u;
    return !(((h >> 16) + (uint32_t)call) & (period - 1));
}
/* Bytes the detector hashes of an n-byte range: all of it up to 256; else the first 128 and a 128-byte window that moves on by
 * one step per sample through the rest (every byte is covered once in ceil((n - 128) / 128) samples). Returns the window count. */
static inline unsigned boot_ds_windows(uint64_t n, uint64_t sample, uint32_t off[2], uint32_t len[2]) {
    if (n <= 256) { off[0] = 0; len[0] = (uint32_t)n; off[1] = len[1] = 0; return n ? 1u : 0u; }
    const uint64_t steps = (n - 128 + 127) / 128;
    uint64_t o = 128 + (sample % steps) * 128;
    if (o + 128 > n) o = n - 128;
    off[0] = 0; len[0] = 128; off[1] = (uint32_t)o; len[1] = 128;
    return 2;
}
/* Next period: doubled when the call filled the record budget, halved when it used under a quarter of it. */
static inline uint32_t boot_ds_period_next(uint32_t period, uint32_t used, uint32_t budget) {
    if (!budget) return period;
    if (used >= budget && period < (1u << 20)) return period * 2;
    if (used < budget / 4 && period > 1) return period / 2;
    return period;
}
/* ---- the call-start copy */
typedef const uint8_t *(*BootDsPageFn)(void *ctx, uint64_t va);        /* host bytes of the 4 KiB guest GPU page at va, NULL: not mapped */
typedef int (*BootDsCleanFn)(void *ctx, uint64_t va, uint64_t n, uint64_t epoch);   /* 1: no host write to [va, va + n) at epoch or later */
static inline void boot_ds_shadow_begin(BootDsCore *d) { d->shadow_used = 0; d->shadow_call = d->call; d->shadow_nchunks = 0; }
/* Copies the mapped pages of chunk e into the shadow; 0 when it is full or nothing is mapped. */
static inline int boot_ds_shadow_take(BootDsCore *d, BootDsChunk *e, BootDsPageFn page, void *ctx, uint64_t epoch, uint64_t big, uint64_t gen) {
    if (!d->shadow || d->shadow_call != d->call || d->shadow_used > d->shadow_cap || d->shadow_cap - d->shadow_used < BOOT_DS_CHUNK_BYTES) return 0;
    const uint64_t base = (e->key - 1) << BOOT_DS_CHUNK_SHIFT; uint8_t *dst = d->shadow + d->shadow_used; uint32_t mask = 0;
    for (unsigned p = 0; p < BOOT_DS_CHUNK_BYTES / 4096; ++p) {
        const uint8_t *src = page(ctx, base + (uint64_t)p * 4096);
        if (src) { memcpy(dst + (size_t)p * 4096, src, 4096); mask |= 1u << p; }
    }
    if (!mask) return 0;
    e->shadow_off = d->shadow_used; e->shadow_mask = mask; e->shadow_call = d->call;
    e->shadow_epoch = epoch; e->shadow_big = big; e->shadow_gen = gen;
    if (d->shadow_chunks) d->shadow_chunks[d->shadow_nchunks++] = e->key - 1;   /* (one per 64 KiB of the shadow: room by construction) */
    d->shadow_used += (uint32_t)BOOT_DS_CHUNK_BYTES;
    return 1;
}
/* Whether any of [va, va + n) can come from this call's shadow (cheap pre-test). */
static inline int boot_ds_shadow_any(const BootDsCore *d, uint64_t va, uint64_t n) {
    return d->shadow_used && d->shadow_call == d->call && (boot_ds_cls_span(d, va, n) & BOOT_DS_C_DYN);
}
/* Copies [va, va + n) into dst, chunk piece by piece: from the shadow where this call copied every page of the piece, the page
 * table generation and the big-write count are those of the copy and no host write touched the piece since (clean), else from
 * `live` (the contiguous host bytes of the whole range). Returns the bytes taken from the shadow. v2fix: only a dynamic chunk
 * (class byte) can be in the shadow, so others skip the chunk-table probe (~2,400 eager CB copies a frame in play-105). */
static inline uint64_t boot_ds_fill_core(BootDsCore *d, uint8_t *dst, uint64_t va, const uint8_t *live, uint64_t n,
                                         uint64_t gen, uint64_t big, BootDsCleanFn clean, void *ctx) {
    uint64_t from_shadow = 0, at = 0;
    const int any = d->shadow_used && d->shadow_call == d->call;
    while (at < n) {
        const uint64_t cva = va + at, in = cva & (BOOT_DS_CHUNK_BYTES - 1);
        uint64_t piece = BOOT_DS_CHUNK_BYTES - in; if (piece > n - at) piece = n - at;
        const BootDsChunk *e = any && (boot_ds_cls_at(d, cva) & BOOT_DS_C_DYN) ? boot_ds_chunk(d, cva >> BOOT_DS_CHUNK_SHIFT, 0) : NULL;
        int use = e && e->shadow_call == d->call && e->shadow_gen == gen && e->shadow_big == big;
        if (use) {
            const uint32_t p0 = (uint32_t)(in >> 12), p1 = (uint32_t)((in + piece - 1) >> 12);
            const uint32_t need = ((2u << p1) - 1u) & ~((1u << p0) - 1u);
            use = (e->shadow_mask & need) == need;
        }
        if (use && clean) use = clean(ctx, cva, piece, e->shadow_epoch + 1);
        if (use) { memcpy(dst + at, d->shadow + e->shadow_off + in, (size_t)piece); from_shadow += piece; }
        else memcpy(dst + at, live + at, (size_t)piece);
        at += piece;
    }
    return from_shadow;
}
/* ---- copy arena (vertex copies: GPU-visible through the page table) */
typedef struct BootDsArena {
    uint8_t *cpu; uint64_t va; uint32_t part_bytes, used[BOOT_DS_PARTS], peak; uint64_t list[BOOT_DS_PARTS];
} BootDsArena;
/* bytes (256-aligned) in arena part `part` for the list `list_id` recorded in that ring segment: the part starts over when a new
 * list opens in the segment (its previous list has completed: boot_render_begin_in waited for the segment's fence). Returns the
 * byte offset in the arena, UINT64_MAX when the part is full. */
static inline uint64_t boot_ds_arena_alloc(BootDsArena *a, unsigned part, uint64_t list_id, uint64_t bytes) {
    if (!a->cpu || part >= BOOT_DS_PARTS || !bytes) return UINT64_MAX;
    if (a->list[part] != list_id) { a->list[part] = list_id; a->used[part] = 0; }
    const uint64_t need = (bytes + 255) & ~UINT64_C(255);
    if (need > (uint64_t)a->part_bytes - a->used[part]) return UINT64_MAX;
    const uint64_t off = (uint64_t)part * a->part_bytes + a->used[part];
    a->used[part] += (uint32_t)need;
    if (a->used[part] > a->peak) a->peak = a->used[part];
    return off;
}
/* ---- NVN memory pools by CPU (storage) address */
typedef struct BootDsPool { uint64_t obj, cpu, size; uint32_t flags, live; } BootDsPool;
static inline int boot_ds_pool_order(const void *x, const void *y) {
    const BootDsPool *a = (const BootDsPool *)x, *b = (const BootDsPool *)y;
    return a->cpu < b->cpu ? -1 : a->cpu > b->cpu ? 1 : (a->size > b->size ? -1 : a->size < b->size);
}
/* The innermost pool of `sorted` (live pools by start, larger first at a tie) containing cpu, or NULL. Pools can nest (nxport
 * nvn/api.rs: Kirby's shader pools inside a 1.4 GB pool): the nearest start at or below cpu may end before it, so up to 16
 * earlier starts are looked at and the smallest containing one wins. */
static inline const BootDsPool *boot_ds_pool_find(const BootDsPool *sorted, uint32_t n, uint64_t cpu) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (sorted[mid].cpu <= cpu) lo = mid + 1; else hi = mid; }
    const BootDsPool *best = NULL;
    for (uint32_t k = 0; k < 16 && lo > k; ++k) {
        const BootDsPool *p = &sorted[lo - 1 - k];
        if (cpu - p->cpu < p->size && (!best || p->size < best->size)) best = p;
    }
    return best;
}
/* Class bits of a pool's NVN_MEMORY_POOL_FLAGS (CPU_NO_ACCESS 0x1, CPU_UNCACHED 0x2, CPU_CACHED 0x4; nxport: writable = flags & 6). */
static inline unsigned boot_ds_pool_class(const BootDsPool *p) {
    if (!p) return BOOT_DS_C_NOPOOL;
    if (p->flags & 6u) return BOOT_DS_C_CPU | ((p->flags & 0x40u) ? BOOT_DS_C_CODE : 0u);
    if (p->flags & 1u) return BOOT_DS_C_GPU;
    return BOOT_DS_C_NOPOOL;
}
/* ---- GPU writes recorded and not known complete, until the next queue idle (everything recorded has completed then): per 64 KiB
 * chunk a 256-bit mask of its 256-byte blocks written (exact for constant buffers, which are 256-aligned; vertex and index ranges
 * that share a block with a write count as pending, the safe side) and the sources that wrote there; ranges of 256 KiB and more
 * apart; an "everything" flag for overflows. A lookup is one hash probe and a mask test per chunk (v1 scanned the whole GPU-write
 * table per binding: 2-4 ms per frame in play-104). `ver` moves with every change; cver[] / big_ver tell which chunks a change
 * touched since a given ver, so a memoized decision is re-checked only when its own chunks changed.
 * v2fix: an add that changes nothing (blocks and source already there) leaves ver alone, so repeats re-check no memo; the large
 * ranges are kept sorted and merged (overlapping ones into one range with both sources, touching ones with the same sources),
 * looked up by binary search, and big_ver moves only when the covered bytes or their sources grow (v2 appended a range per add:
 * the GPU-write entry growing in place re-added it whole per draw, 128 duplicates later 'all' turned the snapshot off). */
#define BOOT_DS_PEND_MAP 8192u
#define BOOT_DS_PEND_BIG 512u   /* distinct merged large ranges (v1 merged everything into 512 entries) */
#define BOOT_DS_PEND_BIG_BYTES (UINT64_C(256) << 10)
enum { BOOT_DS_P_NONE, BOOT_DS_P_GW, BOOT_DS_P_STORE, BOOT_DS_P_COMPUTE, BOOT_DS_P_TARGET, BOOT_DS_P_BOUND, BOOT_DS_P_N };
/* BOOT_DS_P_BOUND (v2fix): the storage buffers bound to a deferred dispatch (host_nvn_play.h): its stores outside the program's learnt
 * ranges are pending in GPU order too (the ranges grow only at the next idle). */
enum { BOOT_DS_OVF_MAP, BOOT_DS_OVF_BIG, BOOT_DS_OVF_GW, BOOT_DS_OVF_WIDE, BOOT_DS_OVF_WRAP, BOOT_DS_OVF_N };   /* why 'all' was set */
typedef struct BootDsPendSlot { uint64_t chunk; uint32_t gen, srcs; uint64_t blk[4]; } BootDsPendSlot;   /* srcs: bit per source */
typedef struct BootDsPendBig { uint64_t va, end; uint32_t srcs, pad; } BootDsPendBig;                    /* srcs: bit per source */
typedef struct BootDsPend {
    BootDsPendSlot map[BOOT_DS_PEND_MAP];
    BootDsPendBig big[BOOT_DS_PEND_BIG];   /* sorted by va, disjoint (touching only with different sources) */
    uint32_t nslot, nbig, gen, all, srcs_all, pad; uint64_t ver, overflows, ovf[BOOT_DS_OVF_N];   /* srcs_all: every source added since the reset */
    uint64_t cver[4096], big_ver, reset_ver;   /* ver of the last change touching chunks c with c & 4095 (big ranges, the all flag,
                                                  * resets: big_ver); reset_ver: the ver of the last reset */
} BootDsPend;
static inline void boot_ds_pend_reset(BootDsPend *p) {
    if (++p->gen == 0) { memset(p->map, 0, sizeof p->map); p->gen = 1; }   /* slots of older generations read as empty */
    p->nslot = p->nbig = 0; p->all = 0; p->srcs_all = 0; p->big_ver = p->reset_ver = ++p->ver;
}
static inline BootDsPendSlot *boot_ds_pend_slot(BootDsPend *p, uint64_t chunk, int create) {
    const uint32_t m = BOOT_DS_PEND_MAP - 1;
    for (uint32_t i = boot_ds_hash32(chunk + 1) & m, k = 0; k < 64; ++k, i = (i + 1) & m) {
        BootDsPendSlot *s = &p->map[i];
        if (s->gen != p->gen) {
            if (!create || p->nslot * 4 >= BOOT_DS_PEND_MAP * 3) return NULL;
            s->gen = p->gen; s->chunk = chunk; s->srcs = 0; s->blk[0] = s->blk[1] = s->blk[2] = s->blk[3] = 0; ++p->nslot;
            return s;
        }
        if (s->chunk == chunk) return s;
    }
    return NULL;
}
/* blocks [b0, b1] (0..255) of a mask: set (1 when a block was new) / any set */
static inline uint64_t boot_ds_blk_word(unsigned w, unsigned b0, unsigned b1) {
    const unsigned lo = w == (b0 >> 6) ? (b0 & 63) : 0, hi = w == (b1 >> 6) ? (b1 & 63) : 63;
    return (hi == 63 ? ~UINT64_C(0) : ((UINT64_C(1) << (hi + 1)) - 1)) & ~((UINT64_C(1) << lo) - 1);
}
static inline int boot_ds_blk_set(uint64_t m[4], unsigned b0, unsigned b1) {
    uint64_t grew = 0;
    for (unsigned w = b0 >> 6; w <= b1 >> 6; ++w) { const uint64_t x = boot_ds_blk_word(w, b0, b1); grew |= x & ~m[w]; m[w] |= x; }
    return grew != 0;
}
static inline int boot_ds_blk_any(const uint64_t m[4], unsigned b0, unsigned b1) {
    for (unsigned w = b0 >> 6; w <= b1 >> 6; ++w) if (m[w] & boot_ds_blk_word(w, b0, b1)) return 1;
    return 0;
}
/* Everything is pending until the next reset (`why`: BOOT_DS_OVF_*). Only the change to 'all' moves ver (nothing can change after it). */
static inline void boot_ds_pend_over(BootDsPend *p, unsigned src, unsigned why) {
    if (p->all) return;
    p->all = src ? src : BOOT_DS_P_GW; p->srcs_all |= 1u << (p->all & 7); ++p->overflows; ++p->ovf[why % BOOT_DS_OVF_N];
    p->big_ver = ++p->ver;
}
static inline void boot_ds_pend_all(BootDsPend *p, unsigned src) { boot_ds_pend_over(p, src, BOOT_DS_OVF_WRAP); }
/* First large range ending after va (binary search: the ranges are sorted and disjoint, so their ends ascend too). */
static inline uint32_t boot_ds_big_first(const BootDsPend *p, uint64_t va) {
    uint32_t lo = 0, hi = p->nbig;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (p->big[mid].end > va) hi = mid; else lo = mid + 1; }
    return lo;
}
/* [va, end) pending from source bit `bit` in the large-range list: 1 when the covered bytes or their sources grew, 0 when it was all
 * there already, -1 when the list is full. Overlapping ranges merge into one with the OR of their sources (the safe side: the sources
 * of a merged range may claim a little more than each wrote); touching ranges merge when their sources are the same. */
static inline int boot_ds_big_add(BootDsPend *p, uint64_t va, uint64_t end, uint32_t bit) {
    BootDsPendBig *E = p->big; const uint32_t n = p->nbig;
    uint32_t k0 = boot_ds_big_first(p, va), k1 = k0, srcs = bit;
    uint64_t nva = va, nend = end, at = va; int covered = k0 < n && E[k0].va <= va;
    for (; k1 < n && E[k1].va < end; ++k1) {   /* the ranges overlapping [va, end) */
        if (E[k1].va > at || !(E[k1].srcs & bit)) covered = 0;   /* a gap, or a part without this source */
        if (E[k1].end > at) at = E[k1].end;
        srcs |= E[k1].srcs; if (E[k1].va < nva) nva = E[k1].va; if (E[k1].end > nend) nend = E[k1].end;
    }
    if (covered && at >= end) return 0;
    if (k0 > 0 && E[k0 - 1].end == nva && E[k0 - 1].srcs == srcs) { --k0; nva = E[k0].va; }
    if (k1 < n && E[k1].va == nend && E[k1].srcs == srcs) { nend = E[k1].end; ++k1; }
    const uint32_t nn = n - (k1 - k0) + 1;
    if (nn > BOOT_DS_PEND_BIG) return -1;
    if (k1 != k0 + 1) memmove(&E[k0 + 1], &E[k1], (size_t)(n - k1) * sizeof *E);
    E[k0].va = nva; E[k0].end = nend; E[k0].srcs = srcs; E[k0].pad = 0; p->nbig = nn;
    return 1;
}
/* [va, va + n) may be written by GPU work of source `src` until the next reset. */
static inline void boot_ds_pend_add(BootDsPend *p, uint64_t va, uint64_t n, unsigned src) {
    if (!n || p->all) return;
    if (va > UINT64_MAX - n) { boot_ds_pend_over(p, src, BOOT_DS_OVF_WRAP); return; }
    const uint64_t end = va + n, nv = p->ver + 1; const uint32_t bit = 1u << (src & 7);
    if (n >= BOOT_DS_PEND_BIG_BYTES) {
        const int r = boot_ds_big_add(p, va, end, bit);
        if (r < 0) { boot_ds_pend_over(p, src, BOOT_DS_OVF_BIG); return; }
        if (r) { p->ver = p->big_ver = nv; p->srcs_all |= bit; }
        return;
    }
    int changed = 0;
    for (uint64_t c = va >> BOOT_DS_CHUNK_SHIFT, last = (end - 1) >> BOOT_DS_CHUNK_SHIFT; c <= last; ++c) {
        BootDsPendSlot *s = boot_ds_pend_slot(p, c, 1);
        if (!s) { if (changed) p->ver = nv; boot_ds_pend_over(p, src, BOOT_DS_OVF_MAP); return; }
        const uint64_t lo = c << BOOT_DS_CHUNK_SHIFT, a = va > lo ? va : lo, z = end < lo + BOOT_DS_CHUNK_BYTES ? end : lo + BOOT_DS_CHUNK_BYTES;
        const int grew = boot_ds_blk_set(s->blk, (unsigned)((a - lo) >> 8), (unsigned)((z - 1 - lo) >> 8));
        if (grew || !(s->srcs & bit)) { s->srcs |= bit; p->cver[c & 4095] = nv; changed = 1; }
    }
    if (changed) { p->ver = nv; p->srcs_all |= bit; }
}
/* The last GPU-write entry (boot_render_gw) grew in place from [olo, oend) to [nlo, nend) (always a superset there): only the new
 * bytes are added. Anything else (no old range, not a superset) adds the whole new range. */
static inline void boot_ds_pend_grow(BootDsPend *p, uint64_t olo, uint64_t oend, uint64_t nlo, uint64_t nend, unsigned src) {
    if (olo >= oend || nlo > olo || nend < oend) { if (nend > nlo) boot_ds_pend_add(p, nlo, nend - nlo, src); return; }
    if (nlo < olo) boot_ds_pend_add(p, nlo, olo - nlo, src);
    if (nend > oend) boot_ds_pend_add(p, oend, nend - oend, src);
}
/* Whether a change after ver `since` may concern [va, va + n): its chunks' filter entries, the big ranges, the all flag, resets.
 * 0: a decision taken at ver `since` still holds (the filter only errs towards 1). */
static inline int boot_ds_pend_changed(const BootDsPend *p, uint64_t va, uint64_t n, uint64_t since) {
    if (p->big_ver > since) return 1;
    const uint64_t c0 = va >> BOOT_DS_CHUNK_SHIFT, c1 = (va + n - 1) >> BOOT_DS_CHUNK_SHIFT;
    if (c1 - c0 > 3) return 1;
    for (uint64_t c = c0; c <= c1; ++c) if (p->cver[c & 4095] > since) return 1;
    return 0;
}
/* The sources (bit 1 << BOOT_DS_P_*) of pending GPU writes over [va, va + n) (n >= 1, no wrap): every overlapping big range, and the
 * sources that wrote each chunk where a written block overlaps (a chunk's sources are kept per chunk, not per block: the safe side). */
static inline unsigned boot_ds_pend_mask(BootDsPend *p, uint64_t va, uint64_t n) {
    if (p->all) return 1u << p->all;
    const uint64_t end = va + n; unsigned m = 0;
    for (uint32_t i = p->nbig ? boot_ds_big_first(p, va) : 0; i < p->nbig && p->big[i].va < end; ++i) m |= p->big[i].srcs;
    if (!p->nslot) return m;
    for (uint64_t c = va >> BOOT_DS_CHUNK_SHIFT, last = (end - 1) >> BOOT_DS_CHUNK_SHIFT, k = 0; c <= last; ++c, ++k) {
        if (k >= 256) return m | (1u << BOOT_DS_P_GW);   /* (no caller asks about 16 MiB) */
        if (p->cver[c & 4095] <= p->reset_ver) continue;   /* nothing was added to a chunk of this filter entry since the reset */
        const BootDsPendSlot *s = boot_ds_pend_slot(p, c, 0);
        if (!s) continue;
        const uint64_t lo = c << BOOT_DS_CHUNK_SHIFT, a = va > lo ? va : lo, z = end < lo + BOOT_DS_CHUNK_BYTES ? end : lo + BOOT_DS_CHUNK_BYTES;
        if (boot_ds_blk_any(s->blk, (unsigned)((a - lo) >> 8), (unsigned)((z - 1 - lo) >> 8))) m |= s->srcs;
    }
    return m;
}
/* The lowest source of a pending GPU write over [va, va + n), BOOT_DS_P_NONE when there is none. */
static inline unsigned boot_ds_pend_hit(BootDsPend *p, uint64_t va, uint64_t n) {
    const unsigned m = boot_ds_pend_mask(p, va, n);
    return m ? (unsigned)__builtin_ctz(m) : BOOT_DS_P_NONE;
}
/* The source that keeps a range with pending-write mask m zero-copy (BOOT_DS_P_NONE: a copy may be taken): the one exception is a
 * range under deferred compute's learnt ranges only, on pages verified CPU-written (`stored`: TOTK_DATA_SNAPSHOT_STORED, cpu_all:
 * boot_ds_cpu_all). A deferred dispatch's bound storage buffers (BOOT_DS_P_BOUND) are never cleared by a CPU verdict: the verdict
 * is from before the dispatch, which may store there for the first time (the GPU bit only comes at the next idle). */
static inline unsigned boot_ds_pend_verdict(unsigned m, int stored, int cpu_all) {
    if (!m) return BOOT_DS_P_NONE;
    if (m == 1u << BOOT_DS_P_COMPUTE && stored && cpu_all) return BOOT_DS_P_NONE;
    return (unsigned)__builtin_ctz(m);
}
/* Whether a change under pending-write mask m is judged by the compute store bitmaps after the next idle (boot_ds_attribute) instead
 * of being put down to GPU work at once: only deferred compute (learnt ranges, bound storage buffers) may have written it. */
static inline int boot_ds_attr_judged(unsigned m, int stored) {
    return stored && m && !(m & ~((1u << BOOT_DS_P_COMPUTE) | (1u << BOOT_DS_P_BOUND)));
}
/* The marks a compute dispatch leaves (host_nvn_play.h through boot_ds_dispatch_pre / _post): `after` 0 before it is recorded, 1
 * after; `deferred`: it stayed pending (boot_compute_deferred grew). TOTK_DATA_SNAPSHOT_STORED=0 (stored 0) is v1: every dispatch's
 * storage buffers, before it, as storing-draw writes (a dispatch that completes at once idles before anything later is recorded). */
static inline unsigned boot_ds_dispatch_src(int stored, int after, int deferred) {
    if (!stored) return after ? BOOT_DS_P_NONE : BOOT_DS_P_STORE;
    return after && deferred ? BOOT_DS_P_BOUND : BOOT_DS_P_NONE;
}
/* The stages whose storage buffers a storing draw marks (1 vertex, 2 pixel): the storing ones (host_gpu_render_draw.h
 * boot_ds_store_stages); both when unknown, and always with TOTK_DATA_SNAPSHOT_STORED=0 (v1). */
static inline unsigned boot_ds_store_stage_mask(unsigned stages, int stored) { return stored && stages ? stages & 3u : 3u; }
/* Storage buffers (va[i], size[i]) pending from `src`, each once per reset: `seen` / `seen_gen` (`seen_n` a power of two) remember
 * the (address, size, source) keys added since (a hash collision only adds again, which changes nothing). The key is exact only
 * for addresses below 2^40 and sizes below 1 MiB (above that, size bits overlap the address and source bits, and (A, 3 MiB, store)
 * once matched (A, 4 MiB, bound): the second range was skipped and its copy taken stale); other buffers are always added, and
 * boot_ds_pend_add's big-range path ignores a covered repeat itself. Returns how many were added. */
static inline unsigned boot_ds_mark_bufs(BootDsPend *p, uint64_t *seen, uint32_t *seen_gen, uint32_t seen_n, const uint64_t *va, const uint64_t *size, unsigned n, unsigned src) {
    unsigned added = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t a = va[i], s = size[i];
        if (!a || !s || a > UINT64_MAX - s) continue;
        const int exact = !(a >> 40) && !(s >> 20);   /* v2fix review: the key below is exact only in this range */
        const uint64_t key = a ^ (s << 40) ^ ((uint64_t)src << 60); const uint32_t h = boot_ds_hash32(key) & (seen_n - 1);
        if (exact && seen_gen[h] == p->gen && seen[h] == key) continue;   /* already pending since the idle */
        if (exact) { seen[h] = key; seen_gen[h] = p->gen; }
        boot_ds_pend_add(p, a, s, src); ++added;
    }
    return added;
}
/* ---- pages compute has stored to (the store bitmaps read at each idle, host_gpu_compute.h boot_compute_idle) and pages verified
 * CPU-written, per chunk, kept for the session. A deferred dispatch's learnt ranges are merged across gaps of up to 64 KiB
 * (BOOT_COMPUTE_MERGE_GAP), so they cover CPU-written bytes between its outputs. Decisions stay conservative: the whole learnt ranges
 * of pending dispatches are pending GPU writes, except pages verified CPU-written: they changed during a call, and the store bitmaps
 * read at the next idle (which hold that call's dispatches) showed no compute store on them. A later compute store on such a page
 * moves it back (the GPU mask wins). Attribution of a change inside a pending compute range: GPU when compute stored a page of it
 * before, else re-judged after the next idle (boot_ds_cpu_mark when the stores still miss it).
 * v2fix: once the table is full (3/4) compute stores still update the chunks it holds (a verified page compute stores to later loses
 * its verdict) and no CPU verdict is honoured any more (v2 stopped all updates and kept the verdicts: copies over pages compute now
 * writes, for the rest of the session). */
#define BOOT_DS_STORED 16384u
typedef struct BootDsStoredEnt { uint64_t key; uint32_t gpu, cpu; } BootDsStoredEnt;   /* key chunk + 1; page masks */
typedef struct BootDsStored { BootDsStoredEnt e[BOOT_DS_STORED]; uint32_t n, full; } BootDsStored;
static inline uint32_t boot_ds_page_bits(uint64_t lo, uint64_t hi) {   /* pages touched by [lo, hi) (lo < hi, one chunk), bits 0..15 */
    const uint32_t p0 = (uint32_t)((lo & (BOOT_DS_CHUNK_BYTES - 1)) >> 12), p1 = (uint32_t)(((hi - 1) & (BOOT_DS_CHUNK_BYTES - 1)) >> 12);
    return ((2u << p1) - 1u) & ~((1u << p0) - 1u);
}
static inline BootDsStoredEnt *boot_ds_stored_slot(BootDsStored *h, uint64_t chunk, int create) {
    const uint32_t m = BOOT_DS_STORED - 1; const uint64_t key = chunk + 1;
    for (uint32_t i = boot_ds_hash32(key) & m, k = 0; k < BOOT_DS_STORED; ++k, i = (i + 1) & m) {
        if (h->e[i].key == key) return &h->e[i];
        if (!h->e[i].key) {
            if (!create) return NULL;
            if ((uint64_t)h->n * 4 >= (uint64_t)BOOT_DS_STORED * 3) { h->full = 1; return NULL; }
            h->e[i].key = key; h->e[i].gpu = h->e[i].cpu = 0; ++h->n; return &h->e[i];
        }
    }
    return NULL;
}
/* Runs `body` for each chunk piece of [va, va + n) with E = its entry (NULL: none / no room) and BITS = the piece's page bits. */
#define BOOT_DS_STORED_EACH(h, va, n, create, body) do { \
    const uint64_t ds_end_ = (va) + (n); \
    for (uint64_t ds_c_ = (va) >> BOOT_DS_CHUNK_SHIFT, ds_l_ = (ds_end_ - 1) >> BOOT_DS_CHUNK_SHIFT; ds_c_ <= ds_l_; ++ds_c_) { \
        const uint64_t ds_lo_ = ds_c_ << BOOT_DS_CHUNK_SHIFT, ds_hi_ = ds_lo_ + BOOT_DS_CHUNK_BYTES; \
        BootDsStoredEnt *E = boot_ds_stored_slot((h), ds_c_, (create)); \
        const uint32_t BITS = boot_ds_page_bits((va) > ds_lo_ ? (va) : ds_lo_, ds_end_ < ds_hi_ ? ds_end_ : ds_hi_); \
        body \
    } } while (0)
/* compute stored to [va, va + n) (page granular; those pages are GPU-written from now on; when full, the chunks already held) */
static inline void boot_ds_stored_add(BootDsStored *h, uint64_t va, uint64_t n) {
    if (!n || va > UINT64_MAX - n) return;
    BOOT_DS_STORED_EACH(h, va, n, !h->full, { if (E) { E->gpu |= BITS; E->cpu &= ~BITS; } });
}
/* any page of [va, va + n) compute has stored to (1 as well when the table is full: the safe side) */
static inline int boot_ds_stored_any(BootDsStored *h, uint64_t va, uint64_t n) {
    if (!n || va > UINT64_MAX - n || h->full) return 1;
    BOOT_DS_STORED_EACH(h, va, n, 0, { if (E && (E->gpu & BITS)) return 1; });
    return 0;
}
/* [va, va + n) verified CPU-written (pages compute has not stored to) */
static inline void boot_ds_cpu_mark(BootDsStored *h, uint64_t va, uint64_t n) {
    if (!n || va > UINT64_MAX - n || h->full) return;
    BOOT_DS_STORED_EACH(h, va, n, 1, { if (!E) return; E->cpu |= BITS & ~E->gpu; });
}
/* every page of [va, va + n) verified CPU-written and none stored by compute: inside a pending compute range it is still a CPU copy
 * (0 once the table is full: stores it could not record may have reached those pages) */
static inline int boot_ds_cpu_all(BootDsStored *h, uint64_t va, uint64_t n) {
    if (!n || va > UINT64_MAX - n || h->full) return 0;
    BOOT_DS_STORED_EACH(h, va, n, 0, { if (!E || (E->cpu & BITS) != BITS || (E->gpu & BITS)) return 0; });
    return 1;
}
/* ---- v2fix: the call-start copy after a mid-call queue idle. GPU-ordered copies (boot_render_gw: UpdateUniformBuffer, ClearBuffer,
 * reports) and storing-draw stores never call boot_render_cb_written, so a chunk of this call's shadow they wrote while the call ran
 * would pass the epoch test once the idle dropped their pending marks: its pre-write bytes would be copied. At the idle (before the
 * marks are reset) such chunks leave the shadow; later copies in the call take the live bytes (complete in GPU order then). Compute
 * stores are not in `srcbits`: boot_compute_idle bumps the epochs of the pages it stored. Returns how many chunks were dropped. */
static inline unsigned boot_ds_shadow_drop(BootDsCore *d, BootDsPend *p, unsigned srcbits) {
    if (!d->shadow_used || d->shadow_call != d->call || !d->shadow_chunks) return 0;
    if (!p->all && !(p->srcs_all & srcbits)) return 0;
    unsigned dropped = 0;
    for (uint32_t j = 0; j < d->shadow_nchunks; ++j) {
        BootDsChunk *e = boot_ds_chunk(d, d->shadow_chunks[j], 0);
        if (!e || e->shadow_call != d->call) continue;
        if (p->all || (boot_ds_pend_mask(p, d->shadow_chunks[j] << BOOT_DS_CHUNK_SHIFT, BOOT_DS_CHUNK_BYTES) & srcbits)) { e->shadow_call = 0; ++dropped; }
    }
    return dropped;
}
/* ---- v2fix: budgets and the timer sample */
/* Whether a copy decision of `kind` (BOOT_DS_K_*) by rule t is charged to the constant-buffer budget (TOTK_DATA_SNAPSHOT_CB_MB): every
 * constant-buffer copy (eager and learned; v2 charged learned ones to the vertex/index budget, so the CBs known to race lost their
 * copies first when streams spent it), except with TOTK_DATA_SNAPSHOT_CB=0 (cb 0: v1's one shared budget). */
static inline int boot_ds_uses_cb_budget(unsigned kind, int t, int cb) { (void)t; return kind == BOOT_DS_K_CB && cb > 0; }
/* 1 for one call in 64: the copy timer runs on a sample (two QueryPerformanceCounter calls on each of ~2,400 copies a frame were
 * ~0.1 ms of the 0.3 ms they measured). */
static inline int boot_ds_tick64(uint64_t *n) { return !(++*n & 63); }
/* ---- v2fix: the constant-buffer plan (boot_ds_cb_plan, host_gpu_render.h), per draw: which uniform-buffer slots (36: vertex group 0
 * and pixel group 4, 18 each, slot bit g * 18 + slot) read a copy. A slot memo holds a binding's rule and answer (v2). The plan only
 * visits the slots whose answer may differ from the previous draw's: all of them when the binding table, the call or class
 * generation (stamp) or the render-target write-back state (gtok) is another; else the rows that changed (none when the producer's
 * bind generation says it did not rewrite the table: host_nvn_exec.h boot_nvn_st_buffers), plus the copy rules when pending GPU
 * writes moved (each re-checks its own chunks, boot_ds_pend_changed) and the taken copies when the call's budget is spent. Any other
 * slot hits its memo with an unchanged answer (play-105: ~29 memo hits, ~1.4 misses per draw, constant-buffer rows rebuilt in 77% of
 * the draws). The render-target state is part of the memo: v2 re-decided every copy on every draw while any write-back was pending,
 * even long completed (boot_render_gt_npending is pruned only by slow paths). */
#define BOOT_DS_SLOTS 36u
typedef struct BootDsMemo { uint64_t va, stamp, pver, gtok; uint32_t size; int8_t t, take, pad[2]; } BootDsMemo;
typedef struct BootDsPlan {   /* what a draw reads first, first: the header, then the row keys (5 cache lines), the memos */
    uint64_t valid, tmask, take, keyok;   /* slot bits: rows read on the GPU (valid 2), copy rules (t > 0), copies taken, keys held */
    const void *in; uint64_t bgen, stamp, pver, gtok; int primed;
    uint64_t key[BOOT_DS_SLOTS];          /* each slot's row as last planned (boot_ds_row_key) */
    BootDsMemo m[BOOT_DS_SLOTS];
} BootDsPlan;
typedef struct BootDsPlanOut { uint32_t visited, hits, rechecks, taken; } BootDsPlanOut;
typedef int (*BootDsRuleFn)(void *ctx, unsigned slot, uint64_t va, uint64_t n);   /* a binding not memoized: its rule (counts it, samples it) */
typedef int (*BootDsTakeFn)(void *ctx, uint64_t va, uint64_t n, int t);          /* the rule's copy may be taken now (pending writes, budget) */
/* render-target write-back state for the memo: 0 when none is pending (or the newest one's fence has completed), else the
 * write-back generation (moves with every one recorded) */
static inline uint64_t boot_ds_gt_token(unsigned npending, uint64_t newest, uint64_t done_seen, uint64_t gen) {
    return npending && newest > done_seen ? gen + 1 : 0;
}
/* A row (address, size, valid) as one exact word: addresses below 2^40 (the Maxwell GPU VA space), sizes below 2^22, valid < 4; 0 when
 * it does not fit (such a row is visited on every draw). */
static inline int boot_ds_row_key(const BootCbufBinding *b, uint64_t *k) {
    *k = b->address | (uint64_t)b->size << 40 | (uint64_t)b->valid << 62;
    return !((b->address >> 40) | ((uint64_t)b->size >> 22) | ((uint64_t)b->valid >> 2));
}
static inline uint64_t boot_ds_rows_changed(const BootDsPlan *P, const BootCbufBinding *g0, const BootCbufBinding *g4) {
    uint64_t m = 0;
    for (unsigned i = 0; i < BOOT_DS_SLOTS; ++i) {
        uint64_t k; const int ok = boot_ds_row_key(i < 18 ? &g0[i] : &g4[i - 18], &k);
        m |= (uint64_t)(!ok | (k != P->key[i])) << i;
    }
    return m | (~P->keyok & ((UINT64_C(1) << BOOT_DS_SLOTS) - 1));
}
/* One draw: rows g0 / g4 (18 each) of binding table `in`; bgen / rows_same: the producer's bind generation and whether `in` is the
 * table it describes; stamp: call and class generation; pver: boot_ds_pend.ver; over_budget: the copies' budget is spent. cbt[g][slot]
 * gets the rule of each slot read from a copy (0: the live range); only visited slots are written. */
static inline BootDsPlanOut boot_ds_plan_run(BootDsPlan *P, const BootCbufBinding *g0, const BootCbufBinding *g4, const void *in, uint64_t bgen,
                                             int rows_same, uint64_t stamp, uint64_t pver, uint64_t gtok, int over_budget, BootDsPend *pend,
                                             BootDsRuleFn rule, BootDsTakeFn take, void *ctx, uint8_t cbt[2][18]) {
    BootDsPlanOut o = {0, 0, 0, 0};
    uint64_t visit;
    if (!P->primed || P->in != in || P->stamp != stamp || P->gtok != gtok) visit = (UINT64_C(1) << BOOT_DS_SLOTS) - 1;
    else {
        visit = rows_same && P->bgen == bgen ? 0 : boot_ds_rows_changed(P, g0, g4);
        if (P->pver != pver) visit |= P->tmask;
        if (over_budget) visit |= P->take;
    }
    P->primed = 1; P->in = in; P->bgen = bgen; P->stamp = stamp; P->pver = pver; P->gtok = gtok;
    o.hits = (uint32_t)__builtin_popcountll(P->valid & ~visit);
    for (uint64_t v = visit; v; v &= v - 1) {
        const unsigned i = (unsigned)__builtin_ctzll(v), g = i >= 18, slot = i - 18 * g;
        const BootCbufBinding *b = g ? &g4[slot] : &g0[slot];
        const uint64_t bit = UINT64_C(1) << i;
        ++o.visited;
        P->keyok = boot_ds_row_key(b, &P->key[i]) ? P->keyok | bit : P->keyok & ~bit;
        P->valid &= ~bit; P->tmask &= ~bit; P->take &= ~bit; cbt[g][slot] = 0;
        if (b->valid != 2 || !b->size) continue;
        P->valid |= bit;
        const uint32_t n = b->size < 65536 ? b->size : 65536;
        BootDsMemo *m = &P->m[i];
        if (m->va == b->address && m->size == n && m->stamp == stamp) {
            ++o.hits;
            if (m->t <= 0) continue;
            if (m->gtok != gtok || (m->pver != pver && boot_ds_pend_changed(pend, b->address, n, m->pver))) { m->take = (int8_t)take(ctx, b->address, n, m->t); ++o.rechecks; }
            else if (m->take && over_budget) m->take = 0;   /* nothing new over its chunks: the answer holds until the budget is spent */
        } else {
            const int t = rule(ctx, slot, b->address, n);
            m->va = b->address; m->size = n; m->stamp = stamp; m->t = (int8_t)t; m->take = 0;
            if (t > 0) m->take = (int8_t)take(ctx, b->address, n, t);
        }
        m->pver = pver; m->gtok = gtok;
        if (m->t > 0) P->tmask |= bit;
        if (m->take) { P->take |= bit; cbt[g][slot] = (uint8_t)m->t; }
    }
    o.taken = (uint32_t)__builtin_popcountll(P->take);
    return o;
}
/* ---------------------------------------------------------------- data-snapshot (play-work/data-snapshot, 2026-10-21; v2 after play-104,
 * v2fix after the play-105 review: play-work/data-snapshot/v2fix/README.md)
 * TOTK_DATA_SNAPSHOT: the GPU reads the bytes the game had written when it submitted, for every guest range it may rewrite
 * afterwards. Guest threads run while t2 translates a submit (25-40 ms) and, with frame overlap, while the GPU still executes
 * it; on the Switch the GPU consumes a command buffer within ~1-2 ms of the submit. Ranges the GPU reads in place after the
 * call (direct CBVs, vertex streams through gmem_load, zero-copy index buffers) then mix the next frame's bytes with this
 * frame's draw parameters (play-103, TOTK_SUSPECT_DIAG S1: ~317 ranges per 300 presents changed during the call; 'cave' cb-direct
 * at a1d01200 / a1dc0000, 'effect' particle vertex streams around bc00000; the sky-island slab, giant effect triangles, the
 * overlap explosions; play-104 with v1: dark slabs off stone ruins, a CB range the first time it raced; nxport
 * metal/fence_guard.rs saw the same on TotK: "TotK writes the next frame's per-instance data into memory the running frame still
 * reads").
 *   What is copied:
 *     constant buffers (TOTK_DATA_SNAPSHOT_CB=1, default): every uniform-buffer binding (slots 3..17) in a memory pool the CPU can
 *       write, from its first use (eager; =2 also the program-constant slot 1 and shader-code pools; =0 learned chunks only, v1);
 *     vertex streams and index buffers: ranges in "dynamic" 64 KiB chunks (learned, below); with the per-instance rule (default
 *       on with TOTK_OVERLAP on) also per-instance vertex streams up to TOTK_DATA_SNAPSHOT_INSTANCE_KB;
 *     TOTK_DATA_SNAPSHOT=2 (measurement): everything the CPU may write.
 *     Constant buffers go to the upload ring through boot_render_cbv_ex (the snapshot path of every non-direct binding, its cache
 *     and TOTK_CB_DEDUPE), index buffers to the ring (the IB view points there), vertex streams to the copy arena:
 *     TOTK_DATA_SNAPSHOT_ARENA_MB after the driver buffer in the native block (host_nvn_exec.h), which the page table maps, so the
 *     attribute table's stream VA points at the copy (no shader change: vin_attr4 reads it through gmem_load; the bound size e[6]
 *     is unchanged). The arena has a part per ring segment, reused with the segment (its fence has completed when a list opens).
 *   What stays zero-copy: memory pools with CPU_NO_ACCESS (observed nvnMemoryPool* calls; the CPU never writes them; nxport
 *     nvn/api.rs keeps writable = flags & 6), the executor's block, and every range GPU work recorded before may write (GPU order
 *     must decide), kept as exact intervals until the next queue idle (boot_ds_pend): GPU-ordered copies (boot_render_gw:
 *     UpdateUniformBuffer, ClearBuffer, reports, occlusion counts, timestamps), the storage buffers of the stage of a storing draw
 *     that stores (nxport metal/mod.rs gpu_dirty: "a uniform buffer in such a range is bound zero-copy so it sees those writes in
 *     GPU order"), the learnt store ranges of pending deferred dispatches (merged across 64 KiB gaps; pages verified CPU-written
 *     are the exception: they changed during a call and the store bitmaps of the next idle showed no compute store there), the
 *     storage buffers bound to pending deferred dispatches (v2fix, BOOT_DS_P_BOUND: their stores outside the learnt ranges, which
 *     grow only at the next idle, 'compute ranges grown'; no CPU verdict clears them), pending render-target write-backs.
 *     Dispatches that are not deferred complete before anything later is recorded (host_gpu_compute.h), so their storage buffers
 *     are not pending (v1 marked every storage buffer of every dispatch, read-only ones too: TOTK_DATA_SNAPSHOT_STORED=0 does that
 *     again). Also zero-copy: vertex/index ranges not learned, streams without a bound size or above
 *     TOTK_DATA_SNAPSHOT_STREAM_KB, and copies over the per-call budgets (TOTK_DATA_SNAPSHOT_CB_MB for every constant-buffer copy,
 *     eager and learned; TOTK_DATA_SNAPSHOT_MB for vertex streams and index buffers; TOTK_DATA_SNAPSHOT_CB=0: one shared budget, v1).
 *   Submit-time bytes: at the start of the call (after the page table is current, before the texture re-hash and the translation)
 *     the pages of the recently used dynamic chunks are copied (TOTK_DATA_SNAPSHOT_EARLY, up to TOTK_DATA_SNAPSHOT_EARLY_MB; vertex
 *     and index chunks first). A copy made during the translation takes those bytes where this call copied them, the page table
 *     and big-write count are unchanged and no host write touched them since; else the bytes at the translation. (nxport
 *     metal/report_guard.rs acts at submit start for the same reason.) A queue idle during the call drops the chunks GPU-ordered
 *     copies or storing draws wrote since (v2fix: those writes have no host epoch, boot_ds_shadow_drop).
 *   Learning (the detector): each call samples up to TOTK_DATA_SNAPSHOT_SAMPLES ranges the CPU may write (zero-copy ones and eager
 *     CB copies; a hash phase picks a range once every `period` calls), hashes the first 128 bytes and a 128-byte window that moves
 *     through the range, and hashes them again at the end of the call (renderer lock held, as suspect-diag S1). A change no host
 *     write (boot_render_cb_written epochs, big writes) and no pending GPU work explains makes its chunks dynamic; changes pending
 *     GPU work explains are counted by source and the first ones logged with their range; a change inside a deferred dispatch's
 *     merged range on pages compute never stored to is judged after the next idle (its store bitmaps then hold that dispatch). Calls whose GPU work has not completed
 *     by the next call keep their records for a second check then. With TOTK_SUSPECT_DIAG=1, S1's changed ranges also teach it.
 *     Dynamic chunks stay (TOTK_DATA_SNAPSHOT_AGE=0, default; else they are dropped after that many calls without use).
 * Every 300 presents two "data-snapshot" lines in draw-path.log: what was copied (per kind and rule, MiB, from the call-start copy),
 * what stayed zero-copy and why, the detector and its attribution, the dynamic chunks, the call-start copy, arena and ring
 * pressure, the pools and the CPU cost per frame. The first TOTK_DATA_SNAPSHOT_EVENTS learn events and the first 32 changes put
 * down to GPU work are logged with their range. */
typedef struct BootDsCfg {
    int mode, early, instance, pools, overlap, cb, stored, plan; uint32_t samples, events;
    uint64_t budget, cb_budget, early_bytes, arena_bytes, stream_cap, instance_cap, age;
} BootDsCfg;
static BootDsCfg boot_ds_cfg;
static BootDsCore boot_ds_core;
static BootDsArena boot_ds_arena;
static BootDsPend boot_ds_pend;       /* GPU writes not known complete (exact intervals) */
static BootDsStored boot_ds_stored;   /* pages compute has stored to */
typedef struct BootDsRec { const uint8_t *base; uint64_t va, off, epoch, big, hash, lo, gen; uint32_t n, reg, kind, nw, woff[2], wlen[2]; } BootDsRec;
#define BOOT_DS_SEEN 4096u
typedef struct BootDsDet {
    BootDsRec *rec, *late; uint32_t nrec, nlate, cap, period, picked; uint64_t late_fence, call_bytes, cb_bytes;
    uint64_t seen[BOOT_DS_SEEN], seen_call[BOOT_DS_SEEN];
    int capture_due;
} BootDsDet;
static BootDsDet *boot_ds_det;
typedef struct BootDsCount {
    uint64_t calls, draws, draws_snap, bind_cb[BOOT_DS_K_N];          /* bind_cb: bindings / streams / index ranges taken */
    uint64_t copies[BOOT_DS_K_N], bytes[BOOT_DS_K_N], shadow_bytes[BOOT_DS_K_N], other_shadow, other_shadow_bytes;
    uint64_t cb_rule_copies[BOOT_DS_T_N], cb_rule_bytes[BOOT_DS_T_N], eager_hist[5], eager_hist_bytes[5];
    uint64_t rule[BOOT_DS_T_N], pend[BOOT_DS_P_N], gpu_only, over_budget, cb_over_budget, arena_full, unbounded, too_large, split, ring_full;
    uint64_t sampled, ch_learn, ch_host, ch_src[BOOT_DS_P_N], gone, late_checked, late_changed, learned_new, learn_full, s1_learn, s1_src[BOOT_DS_P_N], dropped;
    uint64_t verify_queued, verify_gpu, verify_cpu, verify_dropped, verified_takes;   /* changes inside pending compute ranges, judged after the idle */
    uint64_t cap_calls, cap_chunks, cap_bytes, cap_full, cap_pending, store_marks, gw_synced, compute_synced;
    uint64_t cls_bind[5];   /* direct CB decisions by class: dynamic, CPU-writable pool, GPU-only pool, no pool, unclassified */
    uint64_t q_plan, plan_timed, q_copy, q_det, q_cap, remaps, memo_hits, memo_rechecks;
    /* v2fix: deferred dispatches whose storage buffers were marked, call-start chunks dropped at a mid-call idle, plan slots visited
     * and draws with none, the vertex/stream decisions' own timer, the sampled CB-copy timer, the detector's samples inside timed plans
     * (taken off the plan: they are the detector's), timed intervals (each holds ~one QueryPerformanceCounter call) */
    uint64_t bound_marks, shadow_dropped, plan_slots, plan_skips, q_vtx, vtx_timed, q_cbcopy, cbcopy_timed, q_plan_det, q_vtx_det, timer_intervals;
} BootDsCount;
static BootDsCount boot_ds_c, boot_ds_l;
static double boot_ds_qpc_ticks;   /* v2fix: one QueryPerformanceCounter call, in its ticks (measured at start-up) */
static int boot_ds_plan_timing;    /* v2fix: 1 inside a timed plan, 2 inside timed vertex decisions (boot_ds_sample's own time is taken off theirs) */
static uint64_t boot_ds_presents, boot_ds_frame; static unsigned boot_ds_events_left, boot_ds_src_events_left = 32;
static uint64_t boot_ds_cls_gen = 1;   /* moves with every learn, class rebuild and aging: the slot memos start over */
static SRWLOCK boot_ds_init_lock = SRWLOCK_INIT;
static inline uint64_t boot_ds_env(const char *name, uint64_t def, uint64_t lo, uint64_t hi) {
    char v[24]; DWORD k = GetEnvironmentVariableA(name, v, sizeof v);
    if (!k || k >= sizeof v) return def;
    unsigned long long x = strtoull(v, NULL, 10);
    return x < lo ? lo : x > hi ? hi : (uint64_t)x;
}
static inline uint64_t boot_ds_qpc_freq(void) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart > 0 ? (uint64_t)f.QuadPart : 1; }
static inline __attribute__((noinline)) void boot_ds_mode_init(void) {
    AcquireSRWLockExclusive(&boot_ds_init_lock);
    if (boot_ds_mode_v < 0) {
        char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_DATA_SNAPSHOT", v, sizeof v);
        int mode = (k && k < sizeof v) ? (v[0] == '0' ? 0 : v[0] == '2' ? 2 : 1) : 1;
        BootDsCfg *g = &boot_ds_cfg; memset(g, 0, sizeof *g);
        if (mode) {
            k = GetEnvironmentVariableA("TOTK_OVERLAP", v, sizeof v); g->overlap = !(k && k < sizeof v && v[0] == '0');   /* as boot_mt_overlap_on */
            g->budget = boot_ds_env("TOTK_DATA_SNAPSHOT_MB", 8, 1, 256) << 20;
            g->cb = (int)boot_ds_env("TOTK_DATA_SNAPSHOT_CB", 1, 0, 2);
            g->cb_budget = boot_ds_env("TOTK_DATA_SNAPSHOT_CB_MB", 32, 1, 512) << 20;
            g->early = (int)boot_ds_env("TOTK_DATA_SNAPSHOT_EARLY", 1, 0, 1);
            g->early_bytes = g->early ? boot_ds_env("TOTK_DATA_SNAPSHOT_EARLY_MB", 4, 1, 64) << 20 : 0;
            g->arena_bytes = boot_ds_env("TOTK_DATA_SNAPSHOT_ARENA_MB", 32, 16, 256) << 20;
            g->stream_cap = boot_ds_env("TOTK_DATA_SNAPSHOT_STREAM_KB", 1024, 4, 16384) << 10;
            g->instance = (int)boot_ds_env("TOTK_DATA_SNAPSHOT_INSTANCE", (uint64_t)g->overlap, 0, 1);
            g->instance_cap = boot_ds_env("TOTK_DATA_SNAPSHOT_INSTANCE_KB", 64, 1, 4096) << 10;
            g->samples = (uint32_t)boot_ds_env("TOTK_DATA_SNAPSHOT_SAMPLES", 1024, 0, 16384);
            g->age = boot_ds_env("TOTK_DATA_SNAPSHOT_AGE", 0, 0, 100000000);
            g->pools = (int)boot_ds_env("TOTK_DATA_SNAPSHOT_POOLS", 1, 0, 1);
            g->stored = (int)boot_ds_env("TOTK_DATA_SNAPSHOT_STORED", 1, 0, 1);
            g->plan = (int)boot_ds_env("TOTK_DATA_SNAPSHOT_PLAN", 1, 0, 2);   /* v2fix: 0 = the CB plan visits all 36 slots every draw (A/B of its skips); 2 = cross-check (boot_ds_plan_check) */
            g->events = (uint32_t)boot_ds_env("TOTK_DATA_SNAPSHOT_EVENTS", 64, 0, 100000);
            BootDsDet *D = (BootDsDet *)calloc(1, sizeof *D);
            if (D && g->samples) { D->rec = (BootDsRec *)calloc(g->samples, sizeof *D->rec); D->late = (BootDsRec *)calloc(g->samples, sizeof *D->late); }
            if (!D || (g->samples && (!D->rec || !D->late)) || !boot_ds_core_init(&boot_ds_core, 16384, (uint32_t)g->early_bytes)) {
                if (D) { free(D->rec); free(D->late); free(D); }
                mode = 0; boot_gp_log("data-snapshot: no memory for its tables: off (zero-copy as before)");
            } else {
                D->cap = g->samples; D->period = 8; boot_ds_det = D; boot_ds_events_left = g->events;
                boot_ds_pend_reset(&boot_ds_pend);
                if (g->early && !boot_ds_core.shadow) g->early = 0;
                {   LARGE_INTEGER a, b, x; QueryPerformanceCounter(&a); for (unsigned i = 0; i < 1024; ++i) QueryPerformanceCounter(&x);   /* the timer's own cost */
                    QueryPerformanceCounter(&b); boot_ds_qpc_ticks = (double)(b.QuadPart - a.QuadPart) / 1025.0; }
                boot_gp_log("data-snapshot v2fix: TOTK_DATA_SNAPSHOT=%d (%s), constant buffers TOTK_DATA_SNAPSHOT_CB=%d (%s, budget %llu MiB per call%s); vertex/index: "
                            "learned chunks, copy budget %llu MiB per call; call-start copy %s (%llu MiB), arena %llu MiB (%u parts), streams up to %llu KiB, "
                            "per-instance rule %s (up to %llu KiB; TOTK_OVERLAP %s), detector %u ranges per call, aging %s, NVN pool flags %s, "
                            "TOTK_DATA_SNAPSHOT_STORED=%d (%s), CB plan %s; timer %.1f ns per call",
                            mode, mode == 2 ? "every range the CPU may write" : "learned dynamic ranges", g->cb,
                            g->cb == 0 ? "learned only" : g->cb == 1 ? "eager: every uniform buffer in a CPU-writable pool" : "eager, also program constants",
                            (unsigned long long)(g->cb_budget >> 20), g->cb ? ", eager and learned copies" : ": unused, learned CB copies share the vertex/index budget (v1)",
                            (unsigned long long)(g->budget >> 20), g->early ? "on" : "off",
                            (unsigned long long)(g->early_bytes >> 20), (unsigned long long)(g->arena_bytes >> 20), BOOT_DS_PARTS,
                            (unsigned long long)(g->stream_cap >> 10), g->instance ? "on" : "off", (unsigned long long)(g->instance_cap >> 10),
                            g->overlap ? "on" : "off", g->samples, g->age ? "on" : "off (dynamic chunks stay)", g->pools ? "observed" : "not observed",
                            g->stored, g->stored ? "deferred dispatches' storage buffers pending, changes in merged compute store ranges judged by the store bitmaps"
                                                 : "v1 attribution: every dispatch's and both stages' storage buffers pending, learnt ranges whole",
                            g->plan ? "visits changed slots only" : "visits every slot (TOTK_DATA_SNAPSHOT_PLAN=0)", boot_ds_qpc_ticks * 1e9 / (double)boot_ds_qpc_freq());
            }
        }
        g->mode = mode;
        __atomic_store_n(&boot_ds_mode_v, mode, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&boot_ds_init_lock);
}
/* Bytes of the copy arena after the driver buffer in the native block (0: snapshot off, the block stays 2 MiB). */
static inline uint64_t boot_ds_arena_bytes(void) { return boot_ds_on() ? boot_ds_cfg.arena_bytes : 0; }
/* Guest GPU VA -> host bytes through the CPU page table, without the render-target write-back wait of boot_render_guest
 * (ranges such a write-back may still write are kept zero-copy instead, boot_ds_gpu_pending). */
static inline const uint8_t *boot_ds_guest(const BootRender *r, uint64_t va, uint64_t size, uint32_t *reg) {
    if ((va >> 12) < r->va_base_page || !size || !r->ptable_cpu) return NULL;
    const uint64_t page = (va >> 12) - r->va_base_page;
    if (page >= r->ptable_pages) return NULL;
    const uint32_t e = r->ptable_cpu[page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count || !r->region_cpu[e >> 24]) return NULL;
    const uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    if (off > r->region_size[e >> 24] || size > r->region_size[e >> 24] - off) return NULL;
    const uint64_t last = ((va & 0xfff) + size - 1) >> 12;
    for (uint64_t q = 1; q <= last; ++q) if (page + q >= r->ptable_pages || r->ptable_cpu[page + q] != e + (uint32_t)q) return NULL;
    if (reg) *reg = e >> 24;
    return r->region_cpu[e >> 24] + off;
}
static inline const uint8_t *boot_ds_page_fn(void *ctx, uint64_t va) { return boot_ds_guest((const BootRender *)ctx, va, 4096, NULL); }
static inline int boot_ds_clean_fn(void *ctx, uint64_t va, uint64_t n, uint64_t epoch) { (void)ctx; return boot_render_cb_clean(va, n, epoch); }
/* ---- GPU writes recorded and not known complete. The GPU-write table (boot_render_gw) only grows between idles (new entries,
 * the last one extended in place), deferred dispatches only join boot_compute_pending_list: both are folded into boot_ds_pend
 * incrementally (boot_ds_pend_sync, once per draw and before every check outside draws); storing draws add their storing stage's
 * storage buffers when they are recorded, deferred dispatches their bound storage buffers (host_nvn_play.h). Everything is
 * dropped at the next idle. */
#define BOOT_DS_STORE_SEEN 1024u
static uint64_t boot_ds_st_seen[BOOT_DS_STORE_SEEN]; static uint32_t boot_ds_st_seen_gen[BOOT_DS_STORE_SEEN];
static unsigned boot_ds_gw_synced; static uint64_t boot_ds_gw_last_va, boot_ds_gw_last_size;
static unsigned boot_ds_cp_synced, boot_ds_cp_nseen; static const void *boot_ds_cp_seen[64];   /* used by host_nvn_exec.h's sync */
static void (*boot_ds_compute_sync)(void);   /* folds new deferred dispatches in (host_nvn_exec.h sets it) ... */
static const unsigned *boot_ds_cp_count;     /* ... and points this at boot_compute_pending: no call while nothing was deferred */
static uint32_t boot_ds_store_stages;        /* the draw just recorded stores from: 1 vertex, 2 pixel (host_gpu_render_draw.h) */
/* changes inside pending compute ranges on pages compute never stored to: judged after the next idle, whose store bitmaps hold the
 * dispatches of the call that saw the change (boot_ds_verify_run) */
#define BOOT_DS_VERIFY 256u
static struct { uint64_t va, idle; uint32_t n, kind; } boot_ds_verify[BOOT_DS_VERIFY]; static unsigned boot_ds_nverify;
static uint64_t boot_ds_idles;
static inline void boot_ds_pend_sync(void) {
    BootDsPend *p = &boot_ds_pend;
    if (boot_render_gw_all) { if (!p->all) boot_ds_pend_over(p, BOOT_DS_P_GW, BOOT_DS_OVF_GW); }
    else {
        const unsigned n = boot_render_gw_count;
        if (n < boot_ds_gw_synced) boot_ds_gw_synced = 0;   /* (only an idle empties the table, and it resets this too) */
        if (boot_ds_gw_synced) {   /* the last entry may have grown in place since: only its new bytes (v2 re-added it whole) */
            const unsigned i = boot_ds_gw_synced - 1;
            if (boot_render_gw[i].va != boot_ds_gw_last_va || boot_render_gw[i].size != boot_ds_gw_last_size) {
                boot_ds_pend_grow(p, boot_ds_gw_last_va, boot_ds_gw_last_va + boot_ds_gw_last_size, boot_render_gw[i].va, boot_render_gw[i].va + boot_render_gw[i].size, BOOT_DS_P_GW);
                boot_ds_gw_last_va = boot_render_gw[i].va; boot_ds_gw_last_size = boot_render_gw[i].size; ++boot_ds_c.gw_synced;
            }
        }
        for (unsigned i = boot_ds_gw_synced; i < n; ++i) { boot_ds_pend_add(p, boot_render_gw[i].va, boot_render_gw[i].size, BOOT_DS_P_GW); ++boot_ds_c.gw_synced; }
        if (n > boot_ds_gw_synced) { boot_ds_gw_last_va = boot_render_gw[n - 1].va; boot_ds_gw_last_size = boot_render_gw[n - 1].size; }
        boot_ds_gw_synced = n;
    }
    if (boot_ds_compute_sync && (!boot_ds_cp_count || *boot_ds_cp_count != boot_ds_cp_synced)) boot_ds_compute_sync();
}
/* Queue idle (boot_render_idle_cleanup, before it empties the GPU-write table): everything recorded has completed. Chunks of this
 * call's call-start copy that GPU-ordered copies or storing draws wrote since the capture leave the copy first (those writes have no
 * host epoch: the copy would pass for clean once the marks are gone); compute stores bump the epochs in boot_compute_idle. */
static inline void boot_ds_idle(void) {
    if (boot_ds_mode_v <= 0) return;
    ++boot_ds_idles;
    if (boot_ds_core.shadow_used && boot_ds_core.shadow_call == boot_ds_core.call) {
        boot_ds_pend_sync();
        boot_ds_c.shadow_dropped += boot_ds_shadow_drop(&boot_ds_core, &boot_ds_pend, (1u << BOOT_DS_P_GW) | (1u << BOOT_DS_P_STORE));
    }
    boot_ds_pend_reset(&boot_ds_pend);
    boot_ds_gw_synced = 0; boot_ds_gw_last_va = boot_ds_gw_last_size = 0; boot_ds_cp_synced = 0; boot_ds_cp_nseen = 0;
}
/* A deferred dispatch's learnt range (host_nvn_exec.h): pending as a whole (pages verified CPU-written excepted, boot_ds_gpu_pending). */
static inline void boot_ds_pend_compute(uint64_t va, uint64_t n) {
    ++boot_ds_c.compute_synced;
    boot_ds_pend_add(&boot_ds_pend, va, n, BOOT_DS_P_COMPUTE);
}
/* Pages a compute store bitmap showed stored (host_gpu_compute.h boot_compute_idle, every stored run). */
static inline void boot_ds_gpu_stored(uint64_t va, uint64_t n) { if (boot_ds_mode_v > 0) boot_ds_stored_add(&boot_ds_stored, va, n); }
/* Storage buffers of the storing stage(s) of a storing draw: GPU stores in command order until the next idle. */
static inline void boot_ds_stores(const uint64_t *va, const uint64_t *size, unsigned n) {
    if (!boot_ds_on()) return;
    ++boot_ds_c.store_marks;
    boot_ds_mark_bufs(&boot_ds_pend, boot_ds_st_seen, boot_ds_st_seen_gen, BOOT_DS_STORE_SEEN, va, size, n, BOOT_DS_P_STORE);
}
/* The stages whose storage buffers a storing draw just recorded marks (host_nvn_play.h): the storing ones; both when unknown, and
 * with TOTK_DATA_SNAPSHOT_STORED=0 (v1). */
static inline unsigned boot_ds_store_marked_stages(void) { return boot_ds_store_stage_mask(boot_ds_store_stages, boot_ds_cfg.stored); }
/* A compute dispatch (host_nvn_play.h boot_nvn_exec_dispatch), before and after boot_compute_dispatch_decoded. Default: a dispatch
 * that was deferred (boot_compute_deferred grew; exact even when the call idled first) marks its bound storage buffers as
 * BOOT_DS_P_BOUND: its stores outside the program's learnt ranges (those grow only at the next idle) are pending in GPU order too,
 * and a CPU verdict never clears them. Read-only buffers among them stay zero-copy until the idle (as v1); a change seen there is
 * still judged by the store bitmaps (boot_ds_attribute), so v2's attribution is kept. TOTK_DATA_SNAPSHOT_STORED=0: v1, every
 * dispatch's storage buffers before it as storing-draw writes (a dispatch that completes at once idles before anything later). */
static inline void boot_ds_dispatch_pre(const uint64_t *va, const uint64_t *size) {
    if (boot_ds_on() && boot_ds_dispatch_src(boot_ds_cfg.stored, 0, 0) == BOOT_DS_P_STORE) boot_ds_stores(va, size, 16);
}
static inline void boot_ds_dispatch_post(int deferred, const uint64_t *va, const uint64_t *size) {
    if (!deferred || !boot_ds_on()) return;
    const unsigned src = boot_ds_dispatch_src(boot_ds_cfg.stored, 1, deferred);
    if (!src) return;
    ++boot_ds_c.bound_marks;
    boot_ds_mark_bufs(&boot_ds_pend, boot_ds_st_seen, boot_ds_st_seen_gen, BOOT_DS_STORE_SEEN, va, size, 16, src);
}
/* Pending render-target write-backs over [va, va + n) (boot_render_gt_guard's fast path, without the wait). */
static inline int boot_ds_gt_busy(BootRender *r, uint64_t va, uint64_t n) {
    if (!boot_render_gt_npending) return 0;
    if (n >= (UINT64_C(1) << 28) || va >= (UINT64_C(1) << 48)) return 1;
    uint64_t need = 0;
    for (uint64_t c = va >> 16, last = (va + n - 1) >> 16; c <= last; ++c) { const uint64_t f = boot_render_gt_chunk_fence[c & (BOOT_RENDER_GT_CHUNK_TABLE - 1)]; if (f > need) need = f; }
    if (need <= boot_render_gt_done_seen) return 0;
    const uint64_t done = ID3D12Fence_GetCompletedValue(r->gpu->fence);
    if (done != UINT64_MAX && done > boot_render_gt_done_seen) boot_render_gt_done_seen = done;
    return need > boot_render_gt_done_seen;
}
/* The source of GPU work recorded earlier that may write [va, va + n) (0: none; then a CPU copy is the bytes GPU order shows).
 * boot_ds_pend must be synced (boot_ds_pend_sync) since the last GPU-ordered write. */
static inline unsigned boot_ds_gpu_pending(BootRender *r, uint64_t va, uint64_t n, int count) {
    const unsigned m = boot_ds_pend_mask(&boot_ds_pend, va, n);
    const int verified = m == 1u << BOOT_DS_P_COMPUTE && boot_ds_cfg.stored && boot_ds_cpu_all(&boot_ds_stored, va, n);   /* only a merged compute range */
    unsigned src = boot_ds_pend_verdict(m, boot_ds_cfg.stored, verified);                                                  /* over pages verified CPU-written */
    if (m && !src && count) ++boot_ds_c.verified_takes;
    if (!src && boot_ds_gt_busy(r, va, n)) src = BOOT_DS_P_TARGET;
    if (count && src < BOOT_DS_P_N) ++boot_ds_c.pend[src];
    return src;
}
static const char *const boot_ds_src_name[BOOT_DS_P_N] = {"none", "GPU-ordered copy/report", "storing draw's storage buffer", "deferred compute store", "render-target write-back",
                                                          "deferred dispatch's storage buffer"};
/* ---- the detector */
static inline void boot_ds_sample(BootRender *r, unsigned kind, uint64_t va, uint64_t n, unsigned cls) {
    BootDsDet *D = boot_ds_det;
    if (!D || D->nrec >= D->cap || (cls & BOOT_DS_C_HOST) || ((cls & BOOT_DS_C_GPU) && !(cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL)))) return;
    if (!boot_ds_pick(va, n, boot_ds_core.call, D->period)) return;
    const uint64_t key = va ^ (n << 40) ^ ((uint64_t)kind << 62); const uint32_t h = boot_ds_hash32(key) & (BOOT_DS_SEEN - 1);
    if (D->seen_call[h] == boot_ds_core.call && D->seen[h] == key) return;
    D->seen[h] = key; D->seen_call[h] = boot_ds_core.call; ++D->picked;
    uint32_t reg = 0; const uint8_t *p = boot_ds_guest(r, va, n, &reg);
    if (!p) return;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    BootDsRec *e = &D->rec[D->nrec++];
    e->base = r->region_cpu[reg]; e->reg = reg; e->off = (uint64_t)(p - e->base); e->va = va; e->n = (uint32_t)n; e->kind = kind;
    const BootGrView *v = &boot_gr_view[reg];
    if (v->cpu && v->cpu == e->base) { e->lo = v->lo; e->gen = v->gen; } else { e->lo = 0; e->gen = 0; }
    e->nw = boot_ds_windows(n, boot_ds_core.call / (D->period ? D->period : 1) + h, e->woff, e->wlen);
    uint64_t hh = 0; for (unsigned w = 0; w < e->nw; ++w) hh = boot_ds_hash(p + e->woff[w], e->wlen[w], hh);
    e->hash = hh; e->epoch = boot_render_cb_epoch; e->big = boot_render_big_writes;
    ++boot_ds_c.sampled;
    QueryPerformanceCounter(&q1); boot_ds_c.q_det += (uint64_t)(q1.QuadPart - q0.QuadPart);
    if (boot_ds_plan_timing) *(boot_ds_plan_timing == 1 ? &boot_ds_c.q_plan_det : &boot_ds_c.q_vtx_det) += (uint64_t)(q1.QuadPart - q0.QuadPart);   /* counted once: the detector's */
}
static inline int boot_ds_rec_live(const BootRender *r, const BootDsRec *e) {
    if (e->reg >= r->region_count || r->region_cpu[e->reg] != e->base || e->off + e->n > r->region_size[e->reg]) return 0;
    const BootGrView *v = &boot_gr_view[e->reg]; const int view_now = v->cpu && v->cpu == e->base;
    return e->gen ? (view_now && v->lo == e->lo && v->gen == e->gen && !v->stale) : !view_now;
}
static const char *const boot_ds_kind_name[BOOT_DS_K_N] = {"constant buffer", "vertex stream", "index buffer"};
static inline void boot_ds_learned(BootRender *r, uint64_t va, uint64_t n, unsigned kind, const char *how) {
    const unsigned cls = boot_ds_cls_span(&boot_ds_core, va, n);
    if ((cls & BOOT_DS_C_HOST) || ((cls & BOOT_DS_C_GPU) && !(cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL)))) return;
    if ((uint64_t)boot_ds_core.n * 4 >= (uint64_t)boot_ds_core.cap * 3) ++boot_ds_c.learn_full;   /* new chunks are not kept */
    const unsigned fresh = boot_ds_learn_core(&boot_ds_core, va, n, kind);
    boot_ds_c.learned_new += fresh; if (fresh) ++boot_ds_cls_gen;
    if (fresh && boot_ds_events_left) {
        --boot_ds_events_left;
        boot_gp_log("data-snapshot: learned %u chunk(s) at %llx (%s va %llx size %llu %s, pool class %s%s%s%s) at call %llu frame %llu; dynamic chunks %u",
                    fresh, (unsigned long long)(va & ~(BOOT_DS_CHUNK_BYTES - 1)), boot_ds_kind_name[kind % BOOT_DS_K_N], (unsigned long long)va,
                    (unsigned long long)n, how, (cls & BOOT_DS_C_CPU) ? "CPU-writable pool " : "", (cls & BOOT_DS_C_NOPOOL) ? "no pool " : "",
                    (cls & BOOT_DS_C_CODE) ? "shader code " : "", !(cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL)) ? "unclassified" : "",
                    (unsigned long long)boot_ds_core.call, (unsigned long long)boot_ds_frame, boot_ds_core.n);
    }
    (void)r;
}
static inline void boot_ds_src_event(unsigned src, uint64_t va, uint64_t n, unsigned kind, const char *who) {
    if (!boot_ds_src_events_left) return;
    --boot_ds_src_events_left;
    boot_gp_log("data-snapshot: %s change of %s va %llx size %llu put down to GPU work recorded before it (%s): kept zero-copy, not learned (call %llu frame %llu)",
                who, boot_ds_kind_name[kind % BOOT_DS_K_N], (unsigned long long)va, (unsigned long long)n, boot_ds_src_name[src % BOOT_DS_P_N],
                (unsigned long long)boot_ds_core.call, (unsigned long long)boot_ds_frame);
}
/* A change of [va, va + n) no host write explains: 1 put down to GPU work recorded before it (counted in `src_count`, logged), 0 the
 * game's CPU (learn it), 2 inside a pending compute range on pages compute never stored to: judged after the next idle. */
static inline int boot_ds_attribute(BootRender *r, uint64_t va, uint64_t n, unsigned kind, uint64_t *src_count, const char *who) {
    const unsigned m = boot_ds_pend_mask(&boot_ds_pend, va, n);
    if (!m) {
        if (!boot_ds_gt_busy(r, va, n)) return 0;
        ++src_count[BOOT_DS_P_TARGET]; boot_ds_src_event(BOOT_DS_P_TARGET, va, n, kind, who); return 1;
    }
    if (boot_ds_attr_judged(m, boot_ds_cfg.stored) && !boot_ds_stored_any(&boot_ds_stored, va, n)) {   /* deferred compute only (learnt
                                                                                                     * ranges, bound storage buffers) */
        if (boot_ds_cpu_all(&boot_ds_stored, va, n)) return 0;   /* verified before */
        if (boot_ds_nverify < BOOT_DS_VERIFY) {
            boot_ds_verify[boot_ds_nverify].va = va; boot_ds_verify[boot_ds_nverify].n = (uint32_t)n; boot_ds_verify[boot_ds_nverify].kind = kind;
            boot_ds_verify[boot_ds_nverify++].idle = boot_ds_idles; ++boot_ds_c.verify_queued;
        } else ++boot_ds_c.verify_dropped;
        return 2;
    }
    const unsigned src = (unsigned)__builtin_ctz(m);
    ++src_count[src % BOOT_DS_P_N]; boot_ds_src_event(src, va, n, kind, who);
    return 1;
}
/* After an idle (the compute store bitmaps of everything before it are in boot_ds_stored): changes queued before it are GPU when compute
 * stored to their pages, else CPU: their pages are marked verified CPU-written and learned. */
static inline void boot_ds_verify_run(BootRender *r) {
    unsigned k = 0;
    for (unsigned i = 0; i < boot_ds_nverify; ++i) {
        const uint64_t va = boot_ds_verify[i].va, n = boot_ds_verify[i].n; const unsigned kind = boot_ds_verify[i].kind;
        if (boot_ds_verify[i].idle == boot_ds_idles) { boot_ds_verify[k++] = boot_ds_verify[i]; continue; }   /* no idle since: keep */
        if (boot_ds_stored_any(&boot_ds_stored, va, n)) {
            ++boot_ds_c.verify_gpu; boot_ds_src_event(BOOT_DS_P_COMPUTE, va, n, kind, "after the idle (compute stored to it):");
        } else {
            ++boot_ds_c.verify_cpu; boot_ds_cpu_mark(&boot_ds_stored, va, n);
            boot_ds_learned(r, va, n, kind, "changed during the call inside a deferred dispatch's merged store range, compute never stored there");
        }
    }
    boot_ds_nverify = k;
}
/* Hashes the records again: a change no host write and no pending GPU work explains is the game writing during the call. */
static inline void boot_ds_recheck(BootRender *r, BootDsRec *rec, uint32_t n, int late) {
    boot_ds_pend_sync();
    for (uint32_t i = 0; i < n; ++i) {
        BootDsRec *e = &rec[i];
        if (!boot_ds_rec_live(r, e)) { ++boot_ds_c.gone; continue; }
        const uint8_t *p = e->base + e->off; uint64_t h = 0;
        for (unsigned w = 0; w < e->nw; ++w) h = boot_ds_hash(p + e->woff[w], e->wlen[w], h);
        if (late) ++boot_ds_c.late_checked;
        if (h == e->hash) continue;
        e->hash = h;   /* the late check compares against the end of the call */
        if (late) ++boot_ds_c.late_changed;
        if (!boot_render_cb_clean(e->va, e->n, e->epoch + 1) || e->big != boot_render_big_writes) { ++boot_ds_c.ch_host; continue; }
        if (boot_ds_attribute(r, e->va, e->n, e->kind, boot_ds_c.ch_src, "detector:")) continue;
        ++boot_ds_c.ch_learn;
        boot_ds_learned(r, e->va, e->n, e->kind, late ? "changed after the call while its GPU work ran" : "changed during the call");
    }
}
/* suspect-diag S1 saw [va, va + n) change during a call (host writes already excluded). */
static inline void boot_ds_s1_changed(BootRender *r, uint64_t va, uint64_t n, unsigned kind) {
    if (!boot_ds_on() || !n || va > UINT64_MAX - n) return;
    boot_ds_pend_sync();
    if (boot_ds_attribute(r, va, n, kind, boot_ds_c.s1_src, "suspect-diag S1:")) return;
    ++boot_ds_c.s1_learn;
    boot_ds_learned(r, va, n, kind, "changed during the call (suspect-diag S1)");
}
/* ---- decisions */
static inline void boot_ds_touch(uint64_t va, uint64_t n) {
    for (uint64_t c = va >> BOOT_DS_CHUNK_SHIFT, last = (va + n - 1) >> BOOT_DS_CHUNK_SHIFT, k = 0; c <= last && k < 64; ++c, ++k) {
        BootDsChunk *e = boot_ds_chunk(&boot_ds_core, c, 0);
        if (e) e->last_use = boot_ds_core.call;
    }
}
/* The rule that would copy [va, va + n) of kind `kind` (0: zero-copy), from its class only. `eager`: the eager CB rule applies. */
static inline int boot_ds_rule(unsigned cls, int instance_ok, int eager) {
    return boot_ds_take_core(boot_ds_cfg.mode, cls, instance_ok && boot_ds_cfg.instance, eager);
}
/* A rule's copy of a range of `kind` may be taken now: no pending GPU write over it, within the call's budget (constant buffers:
 * TOTK_DATA_SNAPSHOT_CB_MB for every rule, v2 charged learned ones to the vertex/index budget; vertex/index: TOTK_DATA_SNAPSHOT_MB). */
static inline int boot_ds_take_ok(BootRender *r, unsigned kind, uint64_t va, uint64_t n, int t) {
    if (boot_ds_gpu_pending(r, va, n, 1)) return 0;
    BootDsDet *D = boot_ds_det;
    if (D) {
        const int cb = boot_ds_uses_cb_budget(kind, t, boot_ds_cfg.cb);
        if (cb && D->cb_bytes + n > boot_ds_cfg.cb_budget) { ++boot_ds_c.cb_over_budget; return 0; }
        if (!cb && D->call_bytes + n > boot_ds_cfg.budget) { ++boot_ds_c.over_budget; return 0; }
    }
    ++boot_ds_c.rule[t % BOOT_DS_T_N];
    if (t == BOOT_DS_T_LEARNED) boot_ds_touch(va, n);
    return 1;
}
/* > 0: the rule that takes a copy; 0: zero-copy (sampled by the detector when the CPU may write it); -1: a copy was wanted but GPU
 * work may write the range or the call's budget is spent (zero-copy). Vertex streams and index buffers (constant buffers: the plan). */
static inline int boot_ds_decide(BootRender *r, unsigned kind, uint64_t va, uint64_t n, int instance_ok) {
    if (!n || va > UINT64_MAX - n) return 0;
    const unsigned cls = boot_ds_cls_span(&boot_ds_core, va, n);
    const int t = boot_ds_rule(cls, instance_ok, 0);
    if (!t) {
        if ((cls & BOOT_DS_C_GPU) && !(cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL))) ++boot_ds_c.gpu_only;
        else if (boot_ds_det && boot_ds_det->cap) boot_ds_sample(r, kind, va, n, cls);
        return 0;
    }
    return boot_ds_take_ok(r, kind, va, n, t) ? t : -1;
}
/* The draw's constant-buffer slots (vertex group 0, pixel group 4) that read a copy instead of a direct CBV, decided before the ring
 * room check (which then counts their snapshot bytes): boot_ds_plan_run (the core) over the slot memos. A binding equal to the
 * previous one at its slot, in the same call, with no class change since, has the same rule; a rule that copies is re-checked
 * against pending GPU writes only when those changed over its chunks (boot_ds_pend_changed) or the render-target write-back state
 * moved (v2: on every draw while any write-back was pending), and against the budget once it is spent (every rule; v2: eager only).
 * v2fix: slots whose rows, memo and answer cannot have changed are not visited at all; the producer's bind generation
 * (boot_ds_bind_gen, host_nvn_exec.h) spares the row compare when it did not rebuild the table. Within a call a range is sampled
 * at most once. */
static uint64_t boot_ds_plan_n;
static BootDsPlan boot_ds_plan;
static int boot_ds_draw_cb;   /* the draw in flight reads a constant buffer from a copy (boot_ds_vertex counts the draws with a copy) */
static inline int boot_ds_plan_rule(void *ctx, unsigned slot, uint64_t va, uint64_t n) {   /* a binding the slot memo does not hold */
    BootRender *r = (BootRender *)ctx;
    const unsigned cls = boot_ds_cls_span(&boot_ds_core, va, n);
    const int eager = boot_ds_cfg.cb >= 2 ? 2 : boot_ds_cfg.cb == 1 && slot >= 3 ? 1 : 0;   /* slot 1: program constants */
    const int t = boot_ds_rule(cls, 0, eager);
    ++boot_ds_c.cls_bind[(cls & BOOT_DS_C_DYN) ? 0 : (cls & BOOT_DS_C_CPU) ? 1 : (cls & BOOT_DS_C_NOPOOL) ? 3 : (cls & BOOT_DS_C_GPU) ? 2 : 4];
    if (!t) {
        if ((cls & BOOT_DS_C_GPU) && !(cls & (BOOT_DS_C_CPU | BOOT_DS_C_NOPOOL))) ++boot_ds_c.gpu_only;
        else if (boot_ds_det && boot_ds_det->cap) boot_ds_sample(r, BOOT_DS_K_CB, va, n, cls);
        return 0;
    }
    if (t == BOOT_DS_T_EAGER && boot_ds_det && boot_ds_det->cap) boot_ds_sample(r, BOOT_DS_K_CB, va, n, cls);   /* to learn the racing ones */
    return t;
}
static inline int boot_ds_plan_take(void *ctx, uint64_t va, uint64_t n, int t) { return boot_ds_take_ok((BootRender *)ctx, BOOT_DS_K_CB, va, n, t); }
/* The render-target write-back state for the slot memos (boot_ds_gt_token): one fence read per draw at most, only while the newest
 * pending write-back has not been seen complete (boot_render_gt_npending stays until a slow path or the idle prunes it). */
static inline uint64_t boot_ds_gt_state(BootRender *r) {
    if (!boot_render_gt_npending) return 0;
    if (boot_render_gt_newest > boot_render_gt_done_seen && r->gpu && r->gpu->fence) {
        const uint64_t done = ID3D12Fence_GetCompletedValue(r->gpu->fence);
        if (done != UINT64_MAX && done > boot_render_gt_done_seen) boot_render_gt_done_seen = done;
    }
    return boot_ds_gt_token(boot_render_gt_npending, boot_render_gt_newest, boot_render_gt_done_seen, boot_render_gt_gen);
}
/* TOTK_DATA_SNAPSHOT_PLAN=2 (diagnostic; play-106's LOD-dependent tree/terrain explosions on the Sky Island, gone with =0): the
 * skipping plan's answer is checked against a second plan that visits every slot on every draw (=0's answer, which the draw then
 * uses, so nothing breaks on screen). Each draw where they differ is counted; the first 48 differing slots are logged with the
 * slot's row, both answers, and the plan state that made the skip not visit it (`pre`: the skipping plan before this draw). */
static BootDsPlan boot_ds_plan_full;
static uint64_t boot_ds_plan_ck_draws, boot_ds_plan_ck_bad, boot_ds_plan_ck_slots;
static inline void boot_ds_plan_check(BootRender *r, const BootRenderDraw *in, const BootDsPlan *pre, int pre_rows_same, uint64_t pre_rows,
                                      uint64_t stamp, uint64_t pver, uint64_t gtok, int over) {
    uint8_t full[2][18];
    boot_ds_plan_full.primed = 0;
    (void)boot_ds_plan_run(&boot_ds_plan_full, in->cbufs[0], in->cbufs[4], (const void *)in->cbufs, boot_ds_bind_gen, 0, stamp, pver, gtok, over,
                           &boot_ds_pend, boot_ds_plan_rule, boot_ds_plan_take, r, full);
    ++boot_ds_plan_ck_draws;
    if (!(boot_ds_plan_ck_draws & ((UINT64_C(1) << 18) - 1)))
        boot_gp_log("nvn native: data-snapshot plan check (TOTK_DATA_SNAPSHOT_PLAN=2): draws %llu, draws where the skip differed %llu (slots %llu)",
                    (unsigned long long)boot_ds_plan_ck_draws, (unsigned long long)boot_ds_plan_ck_bad, (unsigned long long)boot_ds_plan_ck_slots);
    if (!memcmp(full, boot_ds_cbt, sizeof full)) return;
    ++boot_ds_plan_ck_bad;
    static unsigned logged;
    for (unsigned i = 0; i < BOOT_DS_SLOTS; ++i) {
        const unsigned g = i >= 18, slot = i - 18 * g;
        if (full[g][slot] == boot_ds_cbt[g][slot]) continue;
        ++boot_ds_plan_ck_slots;
        if (logged >= 48) continue;
        ++logged;
        const BootCbufBinding *b = g ? &in->cbufs[4][slot] : &in->cbufs[0][slot];
        const BootDsMemo *m = &pre->m[i];
        boot_gp_log("nvn native: data-snapshot plan check #%u: %s slot %u row %llx size %u valid %u: skip says %u, full walk %u | pre: primed %d same table %d "
                    "same stamp %d same gtok %d rows-same+bgen %d row changed %d pver same %d valid %d tmask %d take %d keyok %d key %llx | memo va %llx size %u "
                    "stamp same %d t %d take %d | draw %llu, draws %llu, differing draws %llu (submission %llu)",
                    logged, g ? "pixel" : "vertex", slot, (unsigned long long)b->address, b->size, b->valid, boot_ds_cbt[g][slot], full[g][slot],
                    pre->primed, pre->in == (const void *)in->cbufs, pre->stamp == stamp, pre->gtok == gtok, pre_rows_same, (int)((pre_rows >> i) & 1),
                    pre->pver == pver, (int)((pre->valid >> i) & 1), (int)((pre->tmask >> i) & 1), (int)((pre->take >> i) & 1), (int)((pre->keyok >> i) & 1),
                    (unsigned long long)pre->key[i], (unsigned long long)m->va, m->size, m->stamp == stamp, m->t, m->take,
                    (unsigned long long)boot_ds_c.draws, (unsigned long long)boot_ds_plan_ck_draws, (unsigned long long)boot_ds_plan_ck_bad,
                    (unsigned long long)boot_render_exec_id);
    }
    memcpy(boot_ds_cbt, full, sizeof full);   /* the draw takes the full walk's answer (=0, clean in the user's A/B) */
}
static inline void boot_ds_cb_plan(BootRender *r, const BootRenderDraw *in) {
    if (!boot_ds_on()) { memset(boot_ds_cbt, 0, sizeof boot_ds_cbt); return; }
    ++boot_ds_c.draws;
    const int timed = !(++boot_ds_plan_n & 63); LARGE_INTEGER q0, q1; if (timed) { QueryPerformanceCounter(&q0); boot_ds_plan_timing = 1; }
    boot_ds_pend_sync();   /* GPU-ordered writes recorded since the previous draw */
    const BootDsDet *D = boot_ds_det;
    const int over = D && (boot_ds_cfg.cb ? D->cb_bytes > boot_ds_cfg.cb_budget : D->call_bytes > boot_ds_cfg.budget);   /* (boot_ds_take_ok's budgets) */
    if (!boot_ds_cfg.plan) boot_ds_plan.primed = 0;   /* TOTK_DATA_SNAPSHOT_PLAN=0: every slot, every draw */
    const uint64_t stamp = boot_ds_core.call ^ (boot_ds_cls_gen << 40), gtok = boot_ds_gt_state(r), pver = boot_ds_pend.ver;
    const int rows_same = (const void *)in->cbufs == boot_ds_bind_of, ck = boot_ds_cfg.plan == 2;
    static BootDsPlan pre; uint64_t pre_rows = 0;   /* TOTK_DATA_SNAPSHOT_PLAN=2 only */
    if (ck) { pre = boot_ds_plan; pre_rows = boot_ds_rows_changed(&boot_ds_plan, in->cbufs[0], in->cbufs[4]); }
    const BootDsPlanOut o = boot_ds_plan_run(&boot_ds_plan, in->cbufs[0], in->cbufs[4], (const void *)in->cbufs, boot_ds_bind_gen, rows_same,
                                             stamp, pver, gtok, over, &boot_ds_pend,
                                             boot_ds_plan_rule, boot_ds_plan_take, r, boot_ds_cbt);
    if (ck) boot_ds_plan_check(r, in, &pre, rows_same && pre.bgen == boot_ds_bind_gen, pre_rows, stamp, pver, gtok, over);
    boot_ds_draw_cb = o.taken != 0;
    boot_ds_c.memo_hits += o.hits; boot_ds_c.memo_rechecks += o.rechecks; boot_ds_c.bind_cb[BOOT_DS_K_CB] += o.taken;
    boot_ds_c.plan_slots += o.visited; boot_ds_c.plan_skips += !o.visited;
    if (timed) { QueryPerformanceCounter(&q1); boot_ds_c.q_plan += (uint64_t)(q1.QuadPart - q0.QuadPart); ++boot_ds_c.plan_timed; ++boot_ds_c.timer_intervals; boot_ds_plan_timing = 0; }
}
static inline int boot_ds_fill_wanted(uint64_t va, uint64_t n) { return boot_ds_mode_v > 0 && boot_ds_shadow_any(&boot_ds_core, va, n); }
/* boot_render_cbv_ex's copy of a constant buffer: call-start bytes where there are any (else memcpy of `live`). boot_ds_cb_now: the
 * rule that took the binding (0: another snapshot over a dynamic chunk). The timer runs on 1 copy in 64 (boot_ds_tick64). */
static uint64_t boot_ds_fill_n;
static inline void boot_ds_fill(BootRender *r, uint8_t *dst, uint64_t va, const uint8_t *live, uint64_t n) {
    (void)r;
    const int timed = boot_ds_tick64(&boot_ds_fill_n); LARGE_INTEGER q0, q1; if (timed) QueryPerformanceCounter(&q0);
    const uint64_t sh = boot_ds_fill_core(&boot_ds_core, dst, va, live, n, boot_render_map_gen, boot_render_big_writes, boot_ds_clean_fn, NULL);
    if (timed) { QueryPerformanceCounter(&q1); boot_ds_c.q_cbcopy += (uint64_t)(q1.QuadPart - q0.QuadPart); ++boot_ds_c.cbcopy_timed; ++boot_ds_c.timer_intervals; }
    const unsigned t = (unsigned)boot_ds_cb_now;
    if (t) {
        ++boot_ds_c.copies[BOOT_DS_K_CB]; boot_ds_c.bytes[BOOT_DS_K_CB] += n; boot_ds_c.shadow_bytes[BOOT_DS_K_CB] += sh;
        ++boot_ds_c.cb_rule_copies[t % BOOT_DS_T_N]; boot_ds_c.cb_rule_bytes[t % BOOT_DS_T_N] += n;
        if (t == BOOT_DS_T_EAGER) { const unsigned k = n <= 256 ? 0 : n <= 1024 ? 1 : n <= 4096 ? 2 : n <= 16384 ? 3 : 4; ++boot_ds_c.eager_hist[k]; boot_ds_c.eager_hist_bytes[k] += n; }
        if (boot_ds_det) { if (boot_ds_uses_cb_budget(BOOT_DS_K_CB, (int)t, boot_ds_cfg.cb)) boot_ds_det->cb_bytes += n; else boot_ds_det->call_bytes += n; }
    } else if (sh) { ++boot_ds_c.other_shadow; boot_ds_c.other_shadow_bytes += sh; }
}
/* vertex copies of the open list by (stream address, size), and the per-stream memo: ds_c (host_gpu_render_draw.h) */
#define BOOT_DS_VCACHE 512u
static struct { uint64_t va, list, epoch, off; uint32_t n, pad; } boot_ds_vc[BOOT_DS_VCACHE];
static struct { uint64_t va, call, gen; uint32_t size, inst; } boot_ds_vmemo[16];
/* ---- the call */
static inline void boot_ds_call_begin(void) {
    if (!boot_ds_on()) return;
    ++boot_ds_core.call; ++boot_ds_c.calls;
    BootDsDet *D = boot_ds_det;
    if (D) { D->nrec = 0; D->picked = 0; D->call_bytes = 0; D->cb_bytes = 0; D->capture_due = 1; }
}
/* After the call's first page-table sync (renderer and window locks held, before the texture re-hash and the translation): the
 * late check of the previous call's records, then the call-start copy of the recently used dynamic chunks, vertex and index
 * chunks first (constant buffers are copied anyway with the eager rule; this only moves their bytes to the submit). */
static inline void boot_ds_capture(BootRender *r) {
    BootDsDet *D = boot_ds_det;
    if (!D || !D->capture_due) return;
    D->capture_due = 0;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    boot_ds_pend_sync();
    if (boot_ds_nverify) boot_ds_verify_run(r);
    if (D->nlate) {
        const uint64_t done = r->gpu && r->gpu->fence ? ID3D12Fence_GetCompletedValue(r->gpu->fence) : UINT64_MAX;
        if (done != UINT64_MAX && done < D->late_fence) boot_ds_recheck(r, D->late, D->nlate, 1);
        D->nlate = 0;
    }
    BootDsCore *d = &boot_ds_core;
    if (boot_ds_cfg.early && d->shadow && d->n) {
        boot_ds_shadow_begin(d); ++boot_ds_c.cap_calls;
        const uint32_t vi = (1u << BOOT_DS_K_VERTEX) | (1u << BOOT_DS_K_INDEX);
        for (int pass = 0, full = 0; pass < 2 && !full; ++pass)
            for (uint32_t j = 0, nj = boot_ds_scan_n(d); j < nj; ++j) {   /* the occupied chunks (v2 visited 2 x 16384 slots per call) */
                BootDsChunk *e = boot_ds_scan_at(d, j);
                if (!e->key || (d->call - e->last_use > 600 && d->call - e->learned > 600)) continue;
                if (!pass != !!(e->kinds & vi)) continue;   /* pass 0: vertex / index chunks; pass 1: the others */
                const uint64_t va = (e->key - 1) << BOOT_DS_CHUNK_SHIFT;
                if (boot_ds_cls_at(d, va) & BOOT_DS_C_HOST) continue;
                if (boot_ds_gpu_pending(r, va, BOOT_DS_CHUNK_BYTES, 0)) { ++boot_ds_c.cap_pending; continue; }   /* GPU order decides: no CPU copy */
                if (boot_ds_shadow_take(d, e, boot_ax_dyn_view ? boot_ax_dyn_page : boot_ds_page_fn, boot_ax_dyn_view ? (void *)(uintptr_t)boot_ax_dyn_view : (void *)r,   /* async-xlate stage 2: from the capture */
                                        boot_render_cb_epoch, boot_render_big_writes, boot_render_map_gen)) ++boot_ds_c.cap_chunks;
                else if (d->shadow_cap - d->shadow_used < BOOT_DS_CHUNK_BYTES) { ++boot_ds_c.cap_full; full = 1; break; }
            }
        boot_ds_c.cap_bytes += d->shadow_used;
    }
    QueryPerformanceCounter(&q1); boot_ds_c.q_cap += (uint64_t)(q1.QuadPart - q0.QuadPart);
}
/* End of the call (window lock held): the detector's check, its next period, and the records kept for the late check. */
static inline void boot_ds_call_done(BootRender *r) {
    BootDsDet *D = boot_ds_det;
    if (!D) return;
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    if (boot_ds_nverify) boot_ds_verify_run(r);
    boot_ds_recheck(r, D->rec, D->nrec, 0);
    D->period = boot_ds_period_next(D->period, D->nrec, D->cap);
    const uint64_t done = r->gpu && r->gpu->fence ? ID3D12Fence_GetCompletedValue(r->gpu->fence) : UINT64_MAX;
    if (D->nrec && r->gpu && done != UINT64_MAX && done < r->gpu->submitted) {   /* its GPU work still runs: check again at the next call */
        BootDsRec *t = D->late; D->late = D->rec; D->rec = t; D->nlate = D->nrec; D->late_fence = r->gpu->submitted;
    } else D->nlate = 0;
    D->nrec = 0;
    QueryPerformanceCounter(&q1); boot_ds_c.q_det += (uint64_t)(q1.QuadPart - q0.QuadPart);
}
/* ---- page-table rebuild: classes of the GPU mappings, the arena */
#define BOOT_DS_POOLS 8192u
static BootDsPool boot_ds_pool[BOOT_DS_POOLS]; static uint32_t boot_ds_npool;
static struct { uint64_t builder, cpu, size; uint32_t flags, set; } boot_ds_builder[64]; static unsigned boot_ds_builder_next;
static SRWLOCK boot_ds_pool_lock = SRWLOCK_INIT;
static uint64_t boot_ds_pool_inits, boot_ds_pool_finals, boot_ds_pool_lost, boot_ds_pool_noflags;
static uint64_t boot_ds_map_chunks[5];   /* at the last rebuild: GPU-mapped chunks CPU-writable, GPU-only, no pool, the executor's, shader code */
/* NVN memory pool calls the snapshot observes (host_nvn.h boot_nvn_policy: OBSERVE, no renderer lock) */
static inline int boot_ds_pool_fn(const char *n) {
    if (boot_ds_mode() <= 0 || !boot_ds_cfg.pools || strncmp(n, "nvnMemoryPool", 13)) return 0;
    n += 13;
    return !strcmp(n, "BuilderSetDefaults") || !strcmp(n, "BuilderSetFlags") || !strcmp(n, "BuilderSetStorage") || !strcmp(n, "Initialize") || !strcmp(n, "Finalize");
}
/* Before the driver's function: SetDefaults(builder), SetFlags(builder, flags), SetStorage(builder, memory, size),
 * Initialize(pool, builder), Finalize(pool). */
static inline void boot_ds_pool_note(const char *n, uint64_t x0, uint64_t x1, uint64_t x2) {
    if (!boot_ds_pool_fn(n)) return;
    n += 13;
    AcquireSRWLockExclusive(&boot_ds_pool_lock);
    if (!strncmp(n, "Builder", 7)) {
        unsigned j = 0; while (j < 64 && boot_ds_builder[j].builder != x0) ++j;
        if (j == 64) { j = boot_ds_builder_next++ & 63; memset(&boot_ds_builder[j], 0, sizeof boot_ds_builder[j]); boot_ds_builder[j].builder = x0; boot_ds_builder[j].flags = 0x22u; }
        if (!strcmp(n, "BuilderSetDefaults")) { boot_ds_builder[j].flags = 0x22u; boot_ds_builder[j].cpu = boot_ds_builder[j].size = 0; boot_ds_builder[j].set = 0; }   /* NVN default: CPU_UNCACHED | GPU_CACHED */
        else if (!strcmp(n, "BuilderSetFlags")) { boot_ds_builder[j].flags = (uint32_t)x1; boot_ds_builder[j].set |= 1; }
        else { boot_ds_builder[j].cpu = x1; boot_ds_builder[j].size = x2; boot_ds_builder[j].set |= 2; }
    } else if (!strcmp(n, "Initialize")) {
        unsigned j = 0; while (j < 64 && boot_ds_builder[j].builder != x1) ++j;
        if (j == 64 || !boot_ds_builder[j].cpu || !boot_ds_builder[j].size) ++boot_ds_pool_lost;
        else {
            uint32_t k = 0, free_k = UINT32_MAX;
            for (; k < boot_ds_npool; ++k) { if (boot_ds_pool[k].live && boot_ds_pool[k].obj == x0) break; if (!boot_ds_pool[k].live && free_k == UINT32_MAX) free_k = k; }
            if (k == boot_ds_npool) k = free_k != UINT32_MAX ? free_k : boot_ds_npool < BOOT_DS_POOLS ? boot_ds_npool++ : UINT32_MAX;
            if (k == UINT32_MAX) ++boot_ds_pool_lost;
            else {
                boot_ds_pool[k].obj = x0; boot_ds_pool[k].cpu = boot_ds_builder[j].cpu; boot_ds_pool[k].size = boot_ds_builder[j].size;
                boot_ds_pool[k].flags = boot_ds_builder[j].flags; boot_ds_pool[k].live = 1; ++boot_ds_pool_inits;
                if (!(boot_ds_builder[j].set & 1)) ++boot_ds_pool_noflags;
            }
        }
    } else {
        for (uint32_t k = 0; k < boot_ds_npool; ++k) if (boot_ds_pool[k].live && boot_ds_pool[k].obj == x0) { boot_ds_pool[k].live = 0; ++boot_ds_pool_finals; }
    }
    ReleaseSRWLockExclusive(&boot_ds_pool_lock);
}
/* Live pools sorted by start (malloc'd; NULL with n 0 when there are none). */
static inline BootDsPool *boot_ds_pools_sorted(uint32_t *n) {
    AcquireSRWLockShared(&boot_ds_pool_lock);
    uint32_t live = 0; for (uint32_t k = 0; k < boot_ds_npool; ++k) live += boot_ds_pool[k].live;
    BootDsPool *s = live ? (BootDsPool *)malloc((size_t)live * sizeof *s) : NULL; uint32_t m = 0;
    for (uint32_t k = 0; s && k < boot_ds_npool; ++k) if (boot_ds_pool[k].live) s[m++] = boot_ds_pool[k];
    ReleaseSRWLockShared(&boot_ds_pool_lock);
    if (s) qsort(s, m, sizeof *s, boot_ds_pool_order);
    *n = m; return s;
}
/* Start of a class rebuild over the page table's VA range; then boot_ds_map_class per GPU mapping, then boot_ds_map_done. */
static inline int boot_ds_map_begin(const BootRender *r) {
    memset(boot_ds_map_chunks, 0, sizeof boot_ds_map_chunks); ++boot_ds_c.remaps; ++boot_ds_cls_gen;
    const uint64_t c0 = (uint64_t)r->va_base_page >> 4, c1 = ((uint64_t)r->va_base_page + r->ptable_pages + 15) >> 4;
    return c1 > c0 && c1 - c0 < (UINT64_C(1) << 26) && boot_ds_cls_reset(&boot_ds_core, c0, (uint32_t)(c1 - c0));
}
static inline void boot_ds_map_class(uint64_t gpu, uint64_t cpu, uint64_t size, const BootDsPool *sorted, uint32_t n) {
    const unsigned bits = boot_ds_pool_class(boot_ds_pool_find(sorted, n, cpu));
    boot_ds_cls_mark(&boot_ds_core, gpu, size, bits);
    const uint64_t chunks = (size + BOOT_DS_CHUNK_BYTES - 1) >> BOOT_DS_CHUNK_SHIFT;
    boot_ds_map_chunks[(bits & BOOT_DS_C_CPU) ? 0 : (bits & BOOT_DS_C_GPU) ? 1 : 2] += chunks;
    if (bits & BOOT_DS_C_CODE) boot_ds_map_chunks[4] += chunks;
}
/* The executor's block [block_va, + bytes): never taken; its arena part after the driver buffer becomes the copy arena. */
static inline void boot_ds_map_done(uint8_t *block, uint64_t block_va, uint64_t bytes, uint64_t driver_bytes) {
    boot_ds_cls_mark(&boot_ds_core, block_va, bytes, BOOT_DS_C_HOST);
    boot_ds_map_chunks[3] = bytes >> BOOT_DS_CHUNK_SHIFT;
    boot_ds_cls_relearn(&boot_ds_core);
    if (block && bytes > driver_bytes + (UINT64_C(1) << 20)) {
        BootDsArena *a = &boot_ds_arena;
        if (a->cpu != block + driver_bytes || a->va != block_va + driver_bytes) { memset(a->list, 0, sizeof a->list); memset(a->used, 0, sizeof a->used); }
        a->cpu = block + driver_bytes; a->va = block_va + driver_bytes;
        a->part_bytes = (uint32_t)(((bytes - driver_bytes) / BOOT_DS_PARTS) & ~UINT64_C(0xffff));
        memset(boot_ds_vc, 0, sizeof boot_ds_vc);
    }
}
/* ---- every 300 presents (renderer and window locks held) */
static inline void boot_ds_present(BootRender *r, uint64_t frames) {
    if (!boot_ds_on()) return;
    boot_ds_frame = frames;
    if (++boot_ds_presents % 300) return;
    if (boot_ds_cfg.age) { const unsigned dropped = boot_ds_age_core(&boot_ds_core, boot_ds_cfg.age); boot_ds_c.dropped += dropped; if (dropped) ++boot_ds_cls_gen; }
    const BootDsCount *c = &boot_ds_c, *l = &boot_ds_l;
#define BOOT_DS_D(x) (unsigned long long)(c->x - l->x)
#define BOOT_DS_MB(x) (double)(c->x - l->x) / 1048576.0
    const double ms = 1000.0 / (double)boot_ds_qpc_freq() / 300.0;   /* per frame */
    /* timed on 1 draw / CB copy in 64; the detector's samples inside a timed plan are the detector's (v2 counted them twice) */
    const uint64_t qp = c->q_plan - l->q_plan, qpd = c->q_plan_det - l->q_plan_det;
    const double plan = c->plan_timed > l->plan_timed ? (double)(qp > qpd ? qp - qpd : 0) * 64.0 * ms : 0.0;
    const uint64_t qv = c->q_vtx - l->q_vtx, qvd = c->q_vtx_det - l->q_vtx_det;
    const double vtx = (double)(qv > qvd ? qv - qvd : 0) * 64.0 * ms, copies = (double)(c->q_copy - l->q_copy) * ms + (double)(c->q_cbcopy - l->q_cbcopy) * 64.0 * ms;
    const double det = (double)(c->q_det - l->q_det) * ms, cap = (double)(c->q_cap - l->q_cap) * ms, timer = (double)(c->timer_intervals - l->timer_intervals) * 64.0 * boot_ds_qpc_ticks * ms;
    uint64_t sb = 0, b = 0; for (unsigned k = 0; k < BOOT_DS_K_N; ++k) { sb += c->shadow_bytes[k] - l->shadow_bytes[k]; b += c->bytes[k] - l->bytes[k]; }
    uint64_t pend = 0, chg = 0, s1g = 0; for (unsigned k = 1; k < BOOT_DS_P_N; ++k) { pend += c->pend[k] - l->pend[k]; chg += c->ch_src[k] - l->ch_src[k]; s1g += c->s1_src[k] - l->s1_src[k]; }
    uint32_t pools = 0; AcquireSRWLockShared(&boot_ds_pool_lock); for (uint32_t k = 0; k < boot_ds_npool; ++k) pools += boot_ds_pool[k].live; ReleaseSRWLockShared(&boot_ds_pool_lock);
    const BootDsPend *P = &boot_ds_pend;
    boot_gp_log("data-snapshot frame %llu, last 300 presents (TOTK_DATA_SNAPSHOT=%d, CB rule %d%s%s%s): calls %llu, draws %llu, with a copy %llu; COPIED: constant buffers "
                "%llu bindings (%llu copies, %.2f MiB; eager %llu copies %.2f MiB [<=256 B %llu, <=1 KiB %llu, <=4 KiB %llu, <=16 KiB %llu, <=64 KiB %llu = %.2f MiB], "
                "learned %llu copies %.2f MiB; the rest shared), vertex streams %llu (%llu copies, %.2f MiB), index buffers %llu (%.2f MiB); %.0f%% of the bytes from "
                "the call-start copy (+%.2f MiB for other constant-buffer snapshots); takes by rule: learned %llu, all %llu, per-instance %llu, eager %llu; KEPT "
                "ZERO-COPY: GPU work may write it %llu (copies/reports %llu, storing draws %llu, deferred compute %llu, target write-backs %llu, deferred dispatches' "
                "storage buffers %llu), GPU-only pool %llu, over the budget %llu (constant buffers %llu), arena full %llu, ring full %llu, stream unbounded %llu / too "
                "large %llu / not contiguous %llu; CB decisions by class: dynamic %llu, CPU-writable %llu, GPU-only %llu, no pool %llu, unclassified %llu (memo hits %llu, "
                "rechecks %llu; plan slots visited %llu, draws with nothing to re-decide %llu)",
                (unsigned long long)frames, boot_ds_cfg.mode, boot_ds_cfg.cb, boot_ds_cfg.early ? ", call-start copy" : "", boot_ds_cfg.instance ? ", per-instance rule" : "",
                boot_ds_cfg.stored ? "" : ", STORED=0: v1 attribution",
                BOOT_DS_D(calls), BOOT_DS_D(draws), BOOT_DS_D(draws_snap), BOOT_DS_D(bind_cb[0]), BOOT_DS_D(copies[0]), BOOT_DS_MB(bytes[0]),
                BOOT_DS_D(cb_rule_copies[BOOT_DS_T_EAGER]), BOOT_DS_MB(cb_rule_bytes[BOOT_DS_T_EAGER]), BOOT_DS_D(eager_hist[0]), BOOT_DS_D(eager_hist[1]),
                BOOT_DS_D(eager_hist[2]), BOOT_DS_D(eager_hist[3]), BOOT_DS_D(eager_hist[4]), BOOT_DS_MB(eager_hist_bytes[4]),
                BOOT_DS_D(cb_rule_copies[BOOT_DS_T_LEARNED]), BOOT_DS_MB(cb_rule_bytes[BOOT_DS_T_LEARNED]),
                BOOT_DS_D(bind_cb[1]), BOOT_DS_D(copies[1]), BOOT_DS_MB(bytes[1]), BOOT_DS_D(bind_cb[2]), BOOT_DS_MB(bytes[2]),
                b ? 100.0 * (double)sb / (double)b : 0.0, BOOT_DS_MB(other_shadow_bytes), BOOT_DS_D(rule[1]), BOOT_DS_D(rule[2]), BOOT_DS_D(rule[3]), BOOT_DS_D(rule[4]),
                (unsigned long long)pend, BOOT_DS_D(pend[1]), BOOT_DS_D(pend[2]), BOOT_DS_D(pend[3]), BOOT_DS_D(pend[4]), BOOT_DS_D(pend[5]), BOOT_DS_D(gpu_only),
                BOOT_DS_D(over_budget), BOOT_DS_D(cb_over_budget), BOOT_DS_D(arena_full), BOOT_DS_D(ring_full), BOOT_DS_D(unbounded), BOOT_DS_D(too_large), BOOT_DS_D(split),
                BOOT_DS_D(cls_bind[0]), BOOT_DS_D(cls_bind[1]), BOOT_DS_D(cls_bind[2]), BOOT_DS_D(cls_bind[3]), BOOT_DS_D(cls_bind[4]), BOOT_DS_D(memo_hits), BOOT_DS_D(memo_rechecks),
                BOOT_DS_D(plan_slots), BOOT_DS_D(plan_skips));
    boot_gp_log("data-snapshot frame %llu: DETECTOR sampled %llu ranges (period %u), changed during the call %llu: learned %llu (new chunks %llu, table full %llu), "
                "host writes %llu, GPU work recorded before it %llu (copies/reports %llu, storing draws %llu, deferred compute %llu, target write-backs %llu, deferred "
                "dispatches' storage buffers %llu), gone %llu; late check %llu ranges, changed %llu; suspect-diag S1 changes: learned %llu, GPU work %llu (copies/reports "
                "%llu, storing draws %llu, deferred compute %llu, target %llu, deferred storage buffers %llu); DYNAMIC CHUNKS %u (dropped %llu); pending GPU writes: "
                "copies/reports folded in %llu, deferred dispatch ranges %llu, storing draws marked %llu, deferred dispatches marked %llu, chunks with pending writes now "
                "%u + %u large ranges, overflows %llu (all time: chunk map %llu, large ranges %llu, copies/reports table full %llu, wide compute %llu, other %llu); changes "
                "inside merged compute ranges judged after the idle: queued %llu, compute stored there %llu, verified CPU %llu (learned), dropped %llu; copies taken over "
                "verified CPU pages %llu; compute store history %u chunks%s; call-start copy %llu calls, %llu chunks "
                "(%.2f MiB), not taken: GPU work pending %llu, full %llu, dropped at a mid-call idle %llu; arena part peak %u of %u KiB; ring-full list executions %llu (all "
                "time); NVN pools live %u (initialized %llu, finalized %llu, unmatched %llu, without SetFlags %llu); GPU-mapped chunks at the last rebuild: CPU-writable %llu "
                "(shader code %llu), GPU-only %llu, no pool %llu, executor %llu (rebuilds %llu); CPU per frame: decisions %.3f ms (constant-buffer plan: v1 and v2 also "
                "counted the vertex part and the detector's samples in it), vertex/stream decisions %.3f ms, copies %.3f ms, detector %.3f ms, call-start copy %.3f ms, "
                "total %.3f ms (the timer itself ~%.3f ms of it)",
                (unsigned long long)frames, BOOT_DS_D(sampled), boot_ds_det ? boot_ds_det->period : 0,
                BOOT_DS_D(ch_learn) + BOOT_DS_D(ch_host) + (unsigned long long)chg, BOOT_DS_D(ch_learn), BOOT_DS_D(learned_new), BOOT_DS_D(learn_full), BOOT_DS_D(ch_host),
                (unsigned long long)chg, BOOT_DS_D(ch_src[1]), BOOT_DS_D(ch_src[2]), BOOT_DS_D(ch_src[3]), BOOT_DS_D(ch_src[4]), BOOT_DS_D(ch_src[5]),
                BOOT_DS_D(gone), BOOT_DS_D(late_checked), BOOT_DS_D(late_changed), BOOT_DS_D(s1_learn),
                (unsigned long long)s1g, BOOT_DS_D(s1_src[1]), BOOT_DS_D(s1_src[2]), BOOT_DS_D(s1_src[3]), BOOT_DS_D(s1_src[4]), BOOT_DS_D(s1_src[5]),
                boot_ds_core.n, BOOT_DS_D(dropped), BOOT_DS_D(gw_synced), BOOT_DS_D(compute_synced), BOOT_DS_D(store_marks), BOOT_DS_D(bound_marks),
                P->nslot, P->nbig, (unsigned long long)P->overflows, (unsigned long long)P->ovf[BOOT_DS_OVF_MAP], (unsigned long long)P->ovf[BOOT_DS_OVF_BIG],
                (unsigned long long)P->ovf[BOOT_DS_OVF_GW], (unsigned long long)P->ovf[BOOT_DS_OVF_WIDE], (unsigned long long)P->ovf[BOOT_DS_OVF_WRAP],
                BOOT_DS_D(verify_queued), BOOT_DS_D(verify_gpu),
                BOOT_DS_D(verify_cpu), BOOT_DS_D(verify_dropped), BOOT_DS_D(verified_takes), boot_ds_stored.n, boot_ds_stored.full ? " (full: no CPU verdicts)" : "",
                BOOT_DS_D(cap_calls), BOOT_DS_D(cap_chunks), BOOT_DS_MB(cap_bytes), BOOT_DS_D(cap_pending), BOOT_DS_D(cap_full), BOOT_DS_D(shadow_dropped),
                boot_ds_arena.peak >> 10, boot_ds_arena.part_bytes >> 10, (unsigned long long)boot_render_ring_full,
                pools, (unsigned long long)boot_ds_pool_inits, (unsigned long long)boot_ds_pool_finals, (unsigned long long)boot_ds_pool_lost,
                (unsigned long long)boot_ds_pool_noflags, (unsigned long long)boot_ds_map_chunks[0], (unsigned long long)boot_ds_map_chunks[4],
                (unsigned long long)boot_ds_map_chunks[1], (unsigned long long)boot_ds_map_chunks[2], (unsigned long long)boot_ds_map_chunks[3], BOOT_DS_D(remaps),
                plan, vtx, copies, det, cap, plan + vtx + copies + det + cap, timer);
#undef BOOT_DS_D
#undef BOOT_DS_MB
    boot_ds_l = boot_ds_c; boot_ds_arena.peak = 0;
    (void)r;
}
/* async-xlate stage 2 (X, after each submission; host_nvn_async.h boot_ax_publish): the chunks the next call-start copy takes (boot_ds_capture's
 * choice and order: recently used or learnt dynamic chunks, vertex / index first, never the executor's block), at most `cap` and the shadow's
 * room. t2 copies them at the next submit call into that submission's arena; X's boot_ds_capture then fills the shadow from there. */
static inline uint32_t boot_ax_chunk_list(uint64_t *out, uint32_t cap) {
    BootDsCore *d = &boot_ds_core; uint32_t n = 0;
    if (!boot_ds_on() || !boot_ds_cfg.early || !d->shadow || !d->n) return 0;
    const uint32_t room = d->shadow_cap / (uint32_t)BOOT_DS_CHUNK_BYTES; if (cap > room) cap = room;
    const uint32_t vi = (1u << BOOT_DS_K_VERTEX) | (1u << BOOT_DS_K_INDEX);
    for (int pass = 0; pass < 2 && n < cap; ++pass)
        for (uint32_t j = 0, nj = boot_ds_scan_n(d); j < nj && n < cap; ++j) {
            const BootDsChunk *e = boot_ds_scan_at(d, j);
            if (!e->key || (d->call - e->last_use > 600 && d->call - e->learned > 600)) continue;
            if (!pass != !!(e->kinds & vi)) continue;
            if (boot_ds_cls_at(d, (e->key - 1) << BOOT_DS_CHUNK_SHIFT) & BOOT_DS_C_HOST) continue;
            out[n++] = e->key - 1;
        }
    return n;
}
static inline int boot_ds_code_range(uint64_t va, uint64_t n) { return boot_ds_on() && (boot_ds_cls_span(&boot_ds_core, va, n) & BOOT_DS_C_CODE) != 0; }

#include "host_gpu_render_draw.h"
#endif
