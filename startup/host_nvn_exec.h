#ifndef TOTK_HOST_NVN_EXEC_H
#define TOTK_HOST_NVN_EXEC_H
/* Native NVN executor (TOTK_NVN=native, runtime_support with TOTK_DRAW_PATH): plays the API-level
 * records of submitted command buffers (host_nvn.h) into the existing D3D12 renderer
 * (host_gpu_render*.h), so render targets, textures (TIC/TSC written by the driver's
 * TexturePoolRegisterTexture), shaders (AOT DXIL by program id), PSOs and present are shared with the
 * emulated path. No Maxwell command stream exists: draw state is assembled from NVN calls and the
 * driver's CPU-side objects (layouts: nvn-work/DESIGN.md appendix A).
 *
 * Driver-side shader ABI reproduced here (DESIGN.md A): a driver constant buffer per queue, mapped at a
 * host GPU virtual address next to the guest mappings:
 *   c0 [0, 0x7000)            storage-buffer table: SSBO (stage, i) at table[stage]*16 + 16*i = {lo, hi, size}
 *   c2 [0x7000 + g*0x200)     bound textures of Maxwell bind group g: low handle word at 0x20 + 8*i
 *   dummy [0x8000, 0x8100)    zeros, bound to unused uniform-buffer slots (as the driver does)
 * c1 = shader data + control[0x700] (64 KiB), c(3+i) = uniform buffer i (size rounded up to 256).
 *
 * GPU-side constant buffers (no host waits inside a frame): uniform buffers and program constants are read
 * by the GPU from guest memory when the draw or dispatch executes (CBVs on the zero-copy regions,
 * BootCbufBinding.valid == 2); the driver buffer (c0/c2, host memory written per draw) stays a snapshot.
 * Guest memory the executor changes in command order (UpdateUniformBuffer, reports, ClearBuffer) is
 * written by GPU copies between barriers (boot_render_gpu_write). Queue submissions execute without
 * waiting; compute dispatches of programs with learnt store ranges are deferred (host_gpu_compute.h);
 * the CPU waits only where the game waits (nvnSyncWait / nvnQueueFinish) or where it must read guest
 * memory that pending GPU work may write (boot_render_cpu_read, indirect arguments). */

#define BOOT_NVN_NATIVE_BLOCK  (UINT64_C(2) << 20)
#define BOOT_NVN_C0_SIZE       0x7000u
#define BOOT_NVN_C2_BASE       0x7000u
#define BOOT_NVN_DUMMY_BASE    0x8000u

/* Program stages observed at nvnProgramSetShaders(Ext): BootNvnProgram (host_nvn.h). */

typedef struct BootNvnExec {
    /* bound state (from records, persistent across command buffers like an NVN queue) */
    BootNvnProgram prog; int has_prog;
    uint8_t blend[8][8]; uint32_t color, channel_mask; uint8_t ds[8]; uint32_t polygon;
    uint32_t vattr[16], n_vattr; uint32_t vstride[16], vdivisor[16], n_vstream;
    uint64_t vbuf[16], vbuf_size[16];
    uint64_t ubo[6][16], ubo_size[6][16], ssbo[6][16], ssbo_size[6][16];
    uint32_t tex[6][32];
    uint64_t tex_pool, smp_pool; uint32_t tex_count, smp_count;
    BootColorTarget rt[8]; uint32_t rt_count; BootDepthTarget zt;
    int32_t vp[16][4]; float depth_range[16][2]; int32_t scissor[16][4];
    float blend_color[4]; uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    float poly_offset[3]; uint32_t restart, restart_index, depth_clamp, guest_barrier;
    uint32_t depth_mode, origin_upper_left;
    /* host GPU mapping of the driver buffer; the page table is rebuilt only when the guest mappings change */
    uint8_t *block; uint64_t block_va; int block_region;
    BootWindow *w; uint64_t map_sig; int mapped;
    uint64_t idles, idle_qpc, remaps;
    uint8_t last_c0[BOOT_NVN_C0_SIZE], last_c2[5][0x200];
    uint32_t raster_discard;
    /* render enable (SetRenderEnable / SetRenderEnableConditional, host_nvn_play.h boot_nvn_render_on): the plain
     * state, the conditional mode (0 none, 1 RENDER_IF_EQUAL, 2 RENDER_IF_NOT_EQUAL) and its counter address */
    uint32_t render_enable, render_cond_mode; uint64_t render_cond_addr, render_off, render_cond_evals;
    uint64_t draws, clears, dispatches, skipped, submits, discarded, submit_qpc;
} BootNvnExec;
static inline BootNvnExec *boot_nvn_exec(void) { static BootNvnExec *e; if (!e) e = calloc(1, sizeof *e); return e; }

static inline void boot_nvn_exec_defaults(BootNvnExec *e) {
    for (unsigned t = 0; t < 8; ++t) { static const uint8_t b[8] = {0, 0, 0x0c, 0x11, 2, 1, 2, 1}; memcpy(e->blend[t], b, 8); e->blend[t][0] = (uint8_t)t; }
    e->color = 0x08030000u; e->channel_mask = 0xffffffffu;
    {   static const uint8_t d[8] = {0x22, 0, 0, 0, 0x18, 0x11, 0x18, 0x11}; memcpy(e->ds, d, 8); }
    e->polygon = 0x14;
    for (unsigned i = 0; i < 16; ++i) { e->depth_range[i][0] = 0; e->depth_range[i][1] = 1; e->scissor[i][2] = e->scissor[i][3] = 0x4000; }
    e->stencil_mask[0] = e->stencil_mask[1] = e->stencil_vmask[0] = e->stencil_vmask[1] = 0xff;
    e->origin_upper_left = 1;
    e->render_enable = 1;
}

/* NVN stage -> Maxwell bind group (0 VS, 1 TCS, 2 TES, 3 GS, 4 PS) and driver SSBO table slot. */
static const uint8_t boot_nvn_stage_group[6] = {0, 4, 3, 1, 2, 0};
static const uint8_t boot_nvn_stage_ssbo[6] = {0x11, 0x51, 0x41, 0x21, 0x31, 0};

/* NVN format -> Maxwell render target format, from the driver's own per-format table (sdk+0xd01614 + 48 *
 * format, 161 entries, byte +0x18 of each; +0x04/+0x05 block width/height): every format the driver can
 * render to. Depth formats (49..54) have their zeta format there instead (boot_nvn_zt_format). 0 = the
 * driver cannot render to it either (RGB, compressed, *_UI2F/_I2F). */
enum { BOOT_NVN_FORMATS = 161 };
#include "generated_nvn_format_rt.h"   /* boot_nvn_rt_formats[], boot_nvn_format_blocks[]: gen_game_headers.py, from the dumped sdk */
static inline uint32_t boot_nvn_rt_format(uint32_t f) { return f < BOOT_NVN_FORMATS ? boot_nvn_rt_formats[f] : 0; }
static inline uint32_t boot_nvn_zt_format(uint32_t f) {   /* STENCIL8, DEPTH16, DEPTH24, DEPTH32F, DEPTH24_STENCIL8, DEPTH32F_STENCIL8 */
    switch (f) { case 49: return 0x17; case 50: return 0x13; case 51: return 0x14; case 52: return 0x0a; case 53: return 0x14; case 54: return 0x19; default: return 0; }
}
/* NVN vertex format -> Maxwell (size code, numeric type, bgra). 0 size = unsupported. */
static inline int boot_nvn_vertex_format(uint32_t f, uint32_t *size, uint32_t *type) {
    /* channel groups of the public NVN order: R8(1..4) R16(5..9) R32(10..12) RG8(13..16) RG16(17..21) RG32(22..24)
     * RGB8(25..28) RGB16(29..33) RGB32(34..36) RGBA8(37..40) RGBA16(41..45) RGBA32(46..48) */
    static const struct { uint8_t first, count, size8, size16, size32; } g[4] = {{0}};
    (void)g;
    enum { SN = 1, UN = 2, SI = 3, UI = 4, FL = 7 };
    static const uint8_t t8[4] = {UN, SN, UI, SI}, t16[5] = {FL, UN, SN, UI, SI}, t32[3] = {FL, UI, SI};
    if (f >= 1 && f <= 4) { *size = 0x1d; *type = t8[f - 1]; return 1; }
    if (f >= 5 && f <= 9) { *size = 0x1b; *type = t16[f - 5]; return 1; }
    if (f >= 10 && f <= 12) { *size = 0x12; *type = t32[f - 10]; return 1; }
    if (f >= 13 && f <= 16) { *size = 0x18; *type = t8[f - 13]; return 1; }
    if (f >= 17 && f <= 21) { *size = 0x0f; *type = t16[f - 17]; return 1; }
    if (f >= 22 && f <= 24) { *size = 0x04; *type = t32[f - 22]; return 1; }
    if (f >= 25 && f <= 28) { *size = 0x13; *type = t8[f - 25]; return 1; }
    if (f >= 29 && f <= 33) { *size = 0x05; *type = t16[f - 29]; return 1; }
    if (f >= 34 && f <= 36) { *size = 0x02; *type = t32[f - 34]; return 1; }
    if (f >= 37 && f <= 40) { *size = 0x0a; *type = t8[f - 37]; return 1; }
    if (f >= 41 && f <= 45) { *size = 0x03; *type = t16[f - 41]; return 1; }
    if (f >= 46 && f <= 48) { *size = 0x01; *type = t32[f - 46]; return 1; }
    if (f == 61) { *size = 0x30; *type = UN; return 1; }          /* RGB10A2 */
    if (f == 62) { *size = 0x30; *type = UI; return 1; }          /* RGB10A2UI */
    if (f == 63) { *size = 0x31; *type = FL; return 1; }          /* R11G11B10F */
    if (f == 105) { *size = 0x30; *type = SN; return 1; }         /* RGB10A2SN */
    return 0;
}

/* One layer and mip level of a block-linear texture as a render target, the way the driver programs it (the
 * emulated draw trace: a view of layer L / level M is bound at base + L * layer stride + level offset, with
 * that level's extent and mip-adjusted block height, one layer). `layer_stride` is the driver's (texture
 * object +0x4a); 0 computes it for a single-level texture. `bpp` bytes per texel. */
static inline void boot_nvn_subresource(uint64_t base, uint32_t w, uint32_t h, uint32_t bpp, unsigned log_h, unsigned log_d,
                                        uint64_t layer_stride, uint32_t layer, uint32_t level,
                                        uint64_t *address, uint32_t *lw, uint32_t *lh, unsigned *llog_h) {
    BootTic t; memset(&t, 0, sizeof t); BootTexFormat f; memset(&f, 0, sizeof f);
    t.width = w; t.height = h; t.depth = 1; t.texture_type = BOOT_TEX_2D_ARRAY; t.log_gob_h = log_h; t.log_gob_d = log_d;
    f.block_w = f.block_h = 1; f.bytes = (uint8_t)bpp;
    BootTexLevel lv[16]; if (level > 15) level = 15;
    uint64_t computed = boot_tex_layout(&t, &f, lv, level + 1);
    if (!layer_stride) layer_stride = computed;   /* exact for one level (the level offsets below need only the levels before) */
    *address = base + (uint64_t)layer * layer_stride + lv[level].offset;
    *lw = lv[level].width; *lh = lv[level].height; *llog_h = lv[level].log_gob_h;
}
static inline uint32_t boot_nvn_zt_bytes(uint32_t zt_format) { return zt_format == 0x13 ? 2 : zt_format == 0x17 ? 1 : zt_format == 0x19 ? 8 : 4; }

/* Driver texture object -> colour/depth target (DESIGN.md A). `view` is the texture view's first 16 bytes as
 * captured when SetRenderTargets was recorded (the game reuses its view objects before the submit), NULL for
 * none: u16 min layer, u16 layers, u8 base level, u8 levels, u8 target, u8 set-mask, u32 format. Only the
 * levels (bit 0), layers (bit 1) and format (bit 2) bits are honoured, and a format override only when the
 * table knows it as a target of the same kind.
 * Layer and level views (shadow cascades and cube faces rendered one layer at a time, the lighting pass's
 * two-layer array, bloom mip chains) bind that subresource's own memory (boot_nvn_subresource): the renderer
 * keys targets by address, extent and format and ignores BootColorTarget.layer, so binding the base address
 * drew every layer and level over layer 0 / level 0, and the rest of the texture (e.g. 50fc10000, layer 1 of
 * the 640x360 R11G11B10 array at 50fb20000, sampled by the lighting pass) never existed: what the guest-memory
 * path decoded there was stale bytes (NaN/Inf in R11G11B10), and same-address targets of different extents
 * wrote each other back and reloaded (rectangular blocks). A linear render target (flags +0x28 bit 0x40,
 * row stride at +0x50) is pitch-linear, as in the emulated path (width = stride in bytes). */
/* `object`: the texture object's first 0x80 bytes as captured when SetRenderTargets was recorded (NULL: read
 * now). The driver bakes the target's address into the command buffer at record time; the game re-initializes
 * ping-pong texture objects (other storage in the same object) between recording and submission, so reading the
 * object at submit drew into the other buffer of the pair while the shaders sampled this one from guest memory
 * (play-7 native: 256x256 RGBA16F drawn at 57f40000, sampled at 57fc0000; 80x45 drawn at 589a0000, sampled at
 * 589c0000; the emulated path draws and samples 58180000 / 58b80000). */
static uint64_t boot_nvn_target_object_changed;
static inline int boot_nvn_texture_target_obj(BootContext *c, uint64_t self, uint64_t tex, const uint8_t *object, const uint8_t *view, BootColorTarget *ct, BootDepthTarget *zt) {
    uint8_t o[0x80];
    if (!tex) return 0;
    uint64_t obj_pool = 0; if (object) memcpy(&obj_pool, object + 0x10, 8);
    if (object && obj_pool) {
        memcpy(o, object, sizeof o);
        uint8_t now[0x20];
        if (boot_host_read(c, self, tex, now, sizeof now) && memcmp(now + 0x10, o + 0x10, 0x10) && boot_nvn_target_object_changed++ < 16) {
            uint64_t a, b; memcpy(&a, o + 0x18, 8); memcpy(&b, now + 0x18, 8);
            boot_draw_log("nvn native: target texture re-initialized since recording%s (recorded offset %llx, now %llx)", "", a & UINT64_C(0xffffffffffff), b & UINT64_C(0xffffffffffff));
        }
    } else if (!boot_host_read(c, self, tex, o, sizeof o)) return 0;
    uint64_t pool, off, va = 0; memcpy(&pool, o + 0x10, 8); memcpy(&off, o + 0x18, 8);
    if (!pool || !boot_host_read(c, self, pool + 0x18, &va, 8)) return 0;
    uint32_t w; uint16_t h, d; memcpy(&w, o + 0x20, 4); memcpy(&h, o + 0x24, 2); memcpy(&d, o + 0x26, 2);
    uint32_t fmt = o[0x2a], target = o[0x2b] / 2, log_h = o[0x71], log_d = o[0x72]; uint32_t base_fmt = fmt; uint64_t view_lo = 0, view_hi = 0;
    uint64_t address = va + (off & UINT64_C(0xffffffffffff));
    {   /* The driver's own GPU address of the storage (texture object +0x60): what it programs for the target and
         * writes into TIC entries (nvn-01's boot_nvn_tic_from_texture uses it too). Memory pool +0x18 plus the
         * object's offset is not always that address (play-8: 256x256 RGBA16F drawn 0x80000 and 80x45 RGBA8 0x20000
         * below where the TIC samples, so lighting read never-written guest memory). */
        uint64_t driver; memcpy(&driver, o + 0x60, 8);
        static unsigned logged;
        if (driver && driver != address && logged++ < 16)
            boot_draw_log("nvn native: target address from the texture object%s %llx (pool + offset %llx)", "", driver, address);
        if (driver) address = driver;
    }
    uint32_t layers = (target == 4 || target == 8 || target == 9 || target == 3) ? (d ? d : 1) : 1, layer = 0, level = 0, slice = 0;
    if (view) {
        memcpy(&view_lo, view, 8); memcpy(&view_hi, view + 8, 8);
        uint32_t vf; memcpy(&vf, view + 8, 4);
        if (view[7] & 4) {
            if (zt ? boot_nvn_zt_format(vf) != 0 : boot_nvn_rt_format(vf) != 0) fmt = vf;
            else { static unsigned n; if (n++ < 16) { char why[96]; snprintf(why, sizeof why, " %u (base %u, view %016llx %016llx)", vf, base_fmt, (unsigned long long)view_lo, (unsigned long long)view_hi); boot_draw_log("nvn native: view format ignored%s %llu %llu", why, target, 0); } }
        }
        if (view[7] & 2) { uint16_t lo, n; memcpy(&lo, view, 2); memcpy(&n, view + 2, 2); if (!n || lo + n <= layers) { layer = lo; layers = n ? n : layers - lo; } }
        if (view[7] & 1) level = view[4];
        /* render-11: a slice of a 3D texture (NVN target 2; its depth is not a layer count, so the check above
         * ignored the view and every slice was drawn over slice 0) */
        if (target == 2 && (view[7] & 2)) { uint16_t lo; memcpy(&lo, view, 2); if (lo < (d ? d : 1)) slice = lo; }
    }
    uint32_t tex_flags = o[0x28], row_stride, layer_stride; memcpy(&row_stride, o + 0x50, 4); memcpy(&layer_stride, o + 0x4a, 4);
    int linear = (tex_flags & 0x40) && row_stride && !zt;
    /* a block-compressed texture rendered through a 1x1-block view format (block-sized texels): extent in blocks */
    if (base_fmt < BOOT_NVN_FORMATS && fmt < BOOT_NVN_FORMATS && boot_nvn_format_blocks[fmt] == 0x11 && boot_nvn_format_blocks[base_fmt] != 0x11) {
        uint32_t bw = boot_nvn_format_blocks[base_fmt] >> 4, bh = boot_nvn_format_blocks[base_fmt] & 15;
        w = (w + bw - 1) / bw; h = (uint16_t)((h + bh - 1) / bh);
    }
    /* 3D slice z of a block-linear 3D texture (render-11): its GOBs start at (z >> bd) * slab + (z & (2^bd - 1))
     * GOB-blocks of 512 << bh bytes (host_texture_layout.h boot_texture_detile_3d); the target keeps the
     * texture's block depth, so its write-back tiles the slice between the other slices' GOBs
     * (boot_texture_tile_slice). TOTK renders its 8x8x8 colour-grading LUT slice by slice: drawn over slice 0,
     * the other seven slices stayed zero in guest memory and the LUT read back one colour (file-select blue). */
    if (slice && !zt && !linear && !level) {
        uint32_t bpp = 0; DXGI_FORMAT dx;
        if (boot_render_ct_format(boot_nvn_rt_format(fmt), &dx)) bpp = boot_render_dxgi_bytes(dx);
        if (bpp && log_h <= 5 && log_d <= 5) {
            uint64_t gx = ((uint64_t)w * bpp + 63) / 64, gy = ((uint64_t)h + (UINT64_C(8) << log_h) - 1) >> (3 + log_h);
            uint64_t slab = gx * gy * (UINT64_C(512) << (log_h + log_d));
            uint64_t sub = address + (uint64_t)(slice >> log_d) * slab + ((uint64_t)(slice & ((1u << log_d) - 1)) << (9 + log_h));
            static unsigned logged; if (logged++ < 24) {
                char why[112]; snprintf(why, sizeof why, " slice %u of %ux%ux%u at %llx (block h/d %u/%u):", slice, w, (unsigned)h, (unsigned)d, (unsigned long long)address, log_h, log_d);
                boot_draw_log("nvn native: 3D target view%s %llx %llu", why, sub, 0); }
            address = sub;
        }
    }
    if ((layer || level) && !linear) {   /* the view's subresource (see above) */
        uint32_t bpp = 0;
        if (zt) bpp = boot_nvn_zt_bytes(boot_nvn_zt_format(fmt));
        else { DXGI_FORMAT dx; if (boot_render_ct_format(boot_nvn_rt_format(fmt), &dx)) bpp = boot_render_dxgi_bytes(dx); }
        if (bpp) {
            uint32_t lw, lh; unsigned llog_h; uint64_t sub;
            boot_nvn_subresource(address, w, h, bpp, log_h, log_d, layer ? layer_stride : 0, layer, level, &sub, &lw, &lh, &llog_h);
            static unsigned logged; if (logged++ < 24) {
                char why[112]; snprintf(why, sizeof why, " layer %u level %u of %ux%u at %llx (layer stride %x):", layer, level, w, (unsigned)h, (unsigned long long)address, layer_stride);
                boot_draw_log("nvn native: target view%s %llx %llu", why, sub, (uint64_t)lw << 32 | lh); }
            address = sub; w = lw; h = (uint16_t)lh; if (level) log_h = llog_h;   /* level 0 keeps the texture's block height */
            layer = 0;
        }
    }
    if (zt) {
        memset(zt, 0, sizeof *zt);
        zt->enabled = 1; zt->address = address; zt->width = w; zt->height = h; zt->format = boot_nvn_zt_format(fmt);
        zt->log_gob_h = log_h; zt->log_gob_d = log_d; zt->layers = layers;
        if (!zt->format) { static unsigned n; if (n++ < 32) { char why[96]; snprintf(why, sizeof why, " %u (base %u, view %016llx %016llx)", fmt, base_fmt, (unsigned long long)view_lo, (unsigned long long)view_hi); boot_draw_log("nvn native: unknown depth format%s target %llu %llu", why, target, 0); } }
        return zt->format != 0;
    }
    memset(ct, 0, sizeof *ct);
    ct->address = address; ct->width = w; ct->height = h; ct->format = boot_nvn_rt_format(fmt);
    ct->log_gob_h = log_h; ct->log_gob_d = log_d; ct->layers = layers; ct->layer = layer;
    if (linear && layers <= 1) { ct->pitch_layout = 1; ct->width = row_stride; ct->log_gob_h = ct->log_gob_d = 0; }
    if (!ct->format) { static unsigned n; if (n++ < 32) { char why[96]; snprintf(why, sizeof why, " %u (base %u, view %016llx %016llx)", fmt, base_fmt, (unsigned long long)view_lo, (unsigned long long)view_hi); boot_draw_log("nvn native: unknown color format%s target %llu %llu", why, target, 0); } }
    return ct->format != 0;
}
static inline int boot_nvn_texture_target(BootContext *c, uint64_t self, uint64_t tex, const uint8_t *view, BootColorTarget *ct, BootDepthTarget *zt) {
    return boot_nvn_texture_target_obj(c, self, tex, NULL, view, ct, zt);
}

/* Maps the guest GPU mappings (as boot_draw_memory) plus the native block above them. The GPU reads the
 * page table while earlier submissions execute: the mappings are resolved to (region, offset) each
 * submission, and the table is rebuilt only when that result changed, after the queue is idle. */
#define BOOT_NVN_MAPS BOOT_GPU_MAPPING_CAPACITY   /* one entry per guest GPU mapping */
typedef struct BootNvnMap { uint64_t gpu, off, size; int region; } BootNvnMap;
static uint64_t boot_nvn_pack_qpc;   /* play (74): time in boot_gpu_pack_regions (per submission), for the hitch line */
static inline int boot_nvn_exec_memory(BootContext *c, BootWindow *w, BootNvnExec *e) {
    BootRender *r = w->render;
    static BootNvnMap maps[BOOT_NVN_MAPS]; unsigned count = 0;
    uint64_t lo = UINT64_MAX, hi = 0; uint32_t as_fd = UINT32_MAX;
    for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (!m->size) continue;
        if (as_fd == UINT32_MAX) as_fd = m->as_fd;
        if (m->as_fd != as_fd) continue;
        if (m->gpu < lo) lo = m->gpu;
        if (m->gpu + m->size > hi) hi = m->gpu + m->size;
    }
    if (lo >= hi) { boot_draw_log("nvn native: no GPU mappings%s %llu %llu", "", 0, 0); return 0; }
    /* the regions these mappings use go into GPU arenas first, as for a GPU channel submission */
    {   /* play (74): the pack takes the kernel lock (every guest thread's next service call waits for it) and, when a region still has to
         * move (a repack of more than 64 blocks), parks the other cores and copies it: timed, and logged from 20 ms */
        LARGE_INTEGER p0, p1; QueryPerformanceCounter(&p0);
        int packed = boot_gpu_pack_regions(c, w, as_fd);
        QueryPerformanceCounter(&p1); uint64_t took = (uint64_t)(p1.QuadPart - p0.QuadPart); boot_nvn_pack_qpc += took;
        LARGE_INTEGER pf; QueryPerformanceFrequency(&pf);
        if (took * 50 >= (uint64_t)pf.QuadPart) { static unsigned logged; if (logged++ < 200) boot_draw_log("nvn native: GPU arena pack (kernel lock, other cores parked while regions move) took %s%llu us", "", took * 1000000u / (uint64_t)pf.QuadPart, 0); }
        if (!packed) { boot_draw_log("nvn native: GPU arena allocation failed%s %llu %llu", "", lo, hi); return 0; }
    }
    if (!e->block) {
        e->block = VirtualAlloc(NULL, BOOT_NVN_NATIVE_BLOCK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!e->block) return 0;
    }
    uint64_t block_va = (hi + 0xffff) & ~UINT64_C(0xffff);
    uint64_t sig = UINT64_C(1469598103934665603) ^ lo ^ (block_va << 1) ^ (uint64_t)(uintptr_t)r;
    /* drawcost: the mapping -> region lookup goes through the sorted region index of boot_gpu_pack_regions (boot_runtime.h,
     * rebuilt when the region generation, the context or the count changes; w->lock is held) instead of a linear scan of
     * the whole region table per mapping (open world: 30-41 ms per submission, 47% of t2). Regions never overlap, so the
     * region the index finds (re-checked against the current table) is the one the first-match scan returns; a miss, or
     * an index the table does not allow (overlap), runs the original scan. TOTK_PACK_VERIFY=1 runs both and keeps the scan. */
    const int pack_verify = boot_gpu_pack_verify(), indexed = boot_gpu_pack_index_ready(c, boot_gpu_pack_rgen());
    for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (!m->size || m->as_fd != as_fd || !m->cpu) continue;
        uint64_t backing = boot_gpu_mapping_backing_size(m);
        unsigned k = indexed ? boot_gpu_pack_find(c, m->cpu) : c->region_count;
        if (k < c->region_count && !(m->cpu >= c->regions[k].base && m->cpu - c->regions[k].base < c->regions[k].size)) k = c->region_count;
        if (k == c->region_count || pack_verify) {
            unsigned found = k; k = 0;
            while (k < c->region_count && !(m->cpu >= c->regions[k].base && m->cpu - c->regions[k].base < c->regions[k].size)) ++k;
            if (pack_verify && indexed && found != k) boot_gpu_pack_mismatch("exec memory: indexed region lookup differs from the linear scan (indexed, linear)", found, k);
        }
        if (k == c->region_count) { boot_draw_log("nvn native: mapping without guest region%s gpu %llx cpu %llx", "", m->gpu, m->cpu); return 0; }
        const Region *g = &c->regions[k];
        uint8_t *blk = g->data; uint64_t blk_size = g->size;
        if (g->reserved != 3 && !(g->reserved == 1 && boot_gpu_block(g->data, g->size, &blk, &blk_size))) continue;   /* not GPU-shareable */
        int idx = boot_render_region(r, blk, blk_size);
        if (idx < 0) { boot_draw_log("nvn native: region %s %llu %llu", r->error, g->base, g->size); return 0; }
        uint64_t off = (uint64_t)(g->data - blk) + (m->cpu - g->base);
        if (backing > g->size - (m->cpu - g->base)) backing = g->size - (m->cpu - g->base);
        if (count == BOOT_NVN_MAPS) { boot_draw_log("nvn native: too many GPU mappings%s %llu %llu", "", count, 0); return 0; }
        maps[count++] = (BootNvnMap){m->gpu, off, backing, idx};
        sig = (sig ^ m->gpu) * UINT64_C(1099511628211); sig = (sig ^ off) * UINT64_C(1099511628211);
        sig = (sig ^ backing) * UINT64_C(1099511628211); sig = (sig ^ (uint64_t)idx) * UINT64_C(1099511628211);
    }
    int idx = boot_render_region(r, e->block, BOOT_NVN_NATIVE_BLOCK);
    if (idx < 0) { boot_draw_log("nvn native: native block%s %llu %llu", r->error, 0, 0); return 0; }
    sig = (sig ^ (uint64_t)idx) * UINT64_C(1099511628211);
    if (e->mapped && sig == e->map_sig && r->ptable_cpu) return 1;
    if (e->mapped && !boot_render_idle(r)) return 0;   /* earlier submissions read the old table */
    e->mapped = 0; ++e->remaps;
    e->block_va = block_va;
    if (!boot_render_ptable(r, lo & ~UINT64_C(0xfff), e->block_va + BOOT_NVN_NATIVE_BLOCK)) { boot_draw_log("nvn native: page table %s %llu %llu", r->error, lo, hi); return 0; }
    memset(boot_render_region_used, 0, sizeof boot_render_region_used);
    for (unsigned i = 0; i < count; ++i) {
        boot_render_region_used[maps[i].region] = 1;
        if (!boot_render_map(r, maps[i].gpu, (unsigned)maps[i].region, maps[i].off, maps[i].size)) return 0;
    }
    boot_render_region_used[idx] = 1;
    if (!boot_render_map(r, e->block_va, (unsigned)idx, 0, BOOT_NVN_NATIVE_BLOCK)) { boot_draw_log("nvn native: native block%s %llu %llu", r->error, 0, 0); return 0; }
    e->block_region = idx;
    e->map_sig = sig; e->mapped = 1;
    return 1;
}

/* Bytes of the native block's c0 / c2 areas changed since the last boot_nvn_exec_block_written: only the
 * words the driver buffer writes are compared (it used to compare the whole 28 KiB c0 and every c2 per draw:
 * 2.5% of the main thread). */
static uint32_t boot_nvn_c0_lo = UINT32_MAX, boot_nvn_c0_hi; static uint8_t boot_nvn_c2_dirty;
/* play (40): one past the highest c0 byte ever written (the rest of the block is still zero) */
static uint32_t boot_nvn_c0_max;
static inline uint32_t boot_nvn_c0_size(void) {
    static int full = -1; if (full < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_C0_FULL", v, sizeof v); full = k && k < sizeof v && v[0] == '1'; }
    uint32_t n = (boot_nvn_c0_max + 255u) & ~255u;
    return full || n > BOOT_NVN_C0_SIZE ? BOOT_NVN_C0_SIZE : n ? n : 256u;
}
static inline void boot_nvn_block_put(uint8_t *at, const void *v, size_t n, uint32_t c0_offset, int c2_group) {
    if (c2_group < 0 && c0_offset + n > boot_nvn_c0_max) boot_nvn_c0_max = c0_offset + (uint32_t)n;
    if (!memcmp(at, v, n)) return;
    memcpy(at, v, n);
    if (c2_group >= 0) boot_nvn_c2_dirty |= (uint8_t)(1u << c2_group);
    else { if (c0_offset < boot_nvn_c0_lo) boot_nvn_c0_lo = c0_offset; if (c0_offset + n > boot_nvn_c0_hi) boot_nvn_c0_hi = c0_offset + (uint32_t)n; }
}
/* nvnpar-work: TOTK_STATE_CACHE (default on; =0: the puts and the full draw-state build of before, see boot_nvn_exec_state). */
static inline int boot_nvn_state_cache_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD k = GetEnvironmentVariableA("TOTK_STATE_CACHE", t, sizeof t); v = !(k && k < sizeof t && t[0] == '0'); }
    return v;
}
/* Byte equality without the linked memcmp (a byte loop in this runtime), 8 bytes at a time. */
static inline int boot_nvn_same(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    for (; n >= 8; n -= 8, p += 8, q += 8) { uint64_t x, y; memcpy(&x, p, 8); memcpy(&y, q, 8); if (x != y) return 0; }
    for (; n; --n, ++p, ++q) if (*p != *q) return 0;
    return 1;
}
/* Driver buffer contents for the bound storage buffers and textures of `stage` (NVN stage). */
/* nvnpar-work (TOTK_STATE_CACHE): an entry whose bytes the block already holds is not put: such a put would write
 * nothing and only raise c0_max to the entry's end (for the table the highest end, base + 256, is raised once below;
 * handle puts never touch it); entries that differ are put as before (same writes, same dirty range / group bits). */
static inline void boot_nvn_exec_driver_buffer(BootNvnExec *e, unsigned stage) {
    unsigned g = boot_nvn_stage_group[stage];
    uint8_t *c0 = e->block, *c2 = e->block + BOOT_NVN_C2_BASE + g * 0x200;
    uint32_t base = (uint32_t)boot_nvn_stage_ssbo[stage] * 16;
    if (stage == 5) return;   /* compute: QMD constant buffers (boot_nvn_exec_dispatch) */
    const int skip_equal = boot_nvn_state_cache_on();
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t v[4] = {(uint32_t)e->ssbo[stage][i], (uint32_t)(e->ssbo[stage][i] >> 32), (uint32_t)e->ssbo_size[stage][i], 0};
        if (!skip_equal || !boot_nvn_same(c0 + base + 16 * i, v, 16)) boot_nvn_block_put(c0 + base + 16 * i, v, 16, base + 16 * i, -1);
    }
    if (skip_equal && base + 256 > boot_nvn_c0_max) boot_nvn_c0_max = base + 256;   /* the skipped puts' c0_max */
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t w; memcpy(&w, c2 + 0x20 + 8 * i, 4);
        if (!skip_equal || w != e->tex[stage][i]) boot_nvn_block_put(c2 + 0x20 + 8 * i, &e->tex[stage][i], 4, 0, (int)g);
    }
    {   /* shared words the driver keeps in c0: texture pool address (0x100), point size clamp (0x610, read by vertex shaders) */
        uint32_t pool[2] = {(uint32_t)e->tex_pool, (uint32_t)(e->tex_pool >> 32)}, clamp = 0x44fffe00u;   /* 2047.9375 */
        boot_nvn_block_put(c0 + 0x100, pool, 8, 0x100, -1); boot_nvn_block_put(c0 + 0x610, &clamp, 4, 0x610, -1);
    }
}
/* After CPU writes to the native block: snapshots of the changed ranges taken earlier are stale. */
static inline void boot_nvn_exec_block_written(BootNvnExec *e) {
    if (boot_nvn_c0_lo < boot_nvn_c0_hi) { boot_render_cb_written(e->block_va + boot_nvn_c0_lo, boot_nvn_c0_hi - boot_nvn_c0_lo); boot_nvn_c0_lo = UINT32_MAX; boot_nvn_c0_hi = 0; }
    for (unsigned g = 0; boot_nvn_c2_dirty && g < 5; ++g)
        if (boot_nvn_c2_dirty & (1u << g)) boot_render_cb_written(e->block_va + BOOT_NVN_C2_BASE + g * 0x200, 0x200);
    boot_nvn_c2_dirty = 0;
}

/* nvnCommandBufferBindTexturesDeferred(cmd, stage, first, count, const NVNtextureHandle *handles). The driver
 * (sdk+0x46cc00) does not read the handles when the call is recorded: it stores a control record
 * {0x17, stage, first, count, pointer} and the handles are read from that pointer when the command buffer is
 * submitted. The game's material binding uses it (main 0x7063a8.., 0x72ca08.., 0x2148c80: one handle per call,
 * pointer into the material's handle array, materials flagged at +0x45 bit 2) for textures it swaps after the
 * material's command set was recorded. Before this the record was ignored and those draws sampled whatever an
 * earlier BindTexture left at the index (characters and the torch: NaN in the HDR target, white on screen).
 * flashfix: the records play during the translation, not at the submit call, and with frame overlap the next
 * frame's jobs rewrite these arrays meanwhile, so the handles come from the copy taken when nvnQueueSubmitCommands
 * was called (host_nvn.h boot_nvn_dsnap_take; `rec` = this record's shadow address). Guest memory is read here
 * only when the copy has no entry for the record (or TOTK_DEFER_SNAPSHOT=0); TOTK_DEFER_LATE_CHECK=1 reads it as
 * well and counts the handles that changed since the call. */
static inline int boot_nvn_exec_deferred_op(void) {
    static int op = -2; if (op == -2) op = boot_nvn_name_index("nvnCommandBufferBindTexturesDeferred");
    return op;
}
static inline void boot_nvn_exec_bind_deferred(BootContext *c, uint64_t self, BootNvnExec *e, const uint64_t *x, uint64_t rec) {
    static uint64_t calls, changed, failed; static unsigned late_logged;
    uint32_t stage = (uint32_t)x[1], first = (uint32_t)x[2], count = (uint32_t)x[3]; uint64_t at = x[4], h[32], now[32];
    ++calls; ++boot_nvn_dsnap.binds;
    if (stage >= 6 || first >= 32 || !count || !at) { if (failed++ < 16) boot_draw_log("nvn native: deferred texture bind ignored%s stage/first/count %llx at %llx", "", (uint64_t)stage << 32 | first << 16 | (count & 0xffff), at); return; }
    if (count > 32 - first) count = 32 - first;
    /* flashfix: the copy of the submit call, when it holds this record (same pointer and count, read then) */
    const BootNvnDsnapEnt *snap = boot_nvn_dsnap_find(rec);
    if (snap && (snap->ptr != at || snap->count != (uint32_t)x[3] || !snap->ok || snap->n < count)) snap = NULL;
    if (boot_nvn_dsnap.active) { if (snap) ++boot_nvn_dsnap.hits; else ++boot_nvn_dsnap.misses; }
    int use = snap && boot_nvn_defer_snapshot_on(), check = snap && boot_nvn_defer_late_check_on();
    if (use) memcpy(h, boot_nvn_dsnap.h + snap->at, 8 * (size_t)count);
    if ((!use || check) && !boot_host_read(c, self, at, use ? now : h, 8 * (size_t)count)) {
        if (!use) { if (failed++ < 16) boot_draw_log("nvn native: deferred texture handles unreadable%s at %llx count %llu", "", at, count); return; }
        check = 0;
    }
    if (check) {   /* TOTK_DEFER_LATE_CHECK=1: the call's handles against guest memory now */
        const uint64_t *was = boot_nvn_dsnap.h + snap->at, *cur = use ? now : h;
        boot_nvn_dsnap.late_checked += count;
        for (uint32_t i = 0; i < count; ++i) if ((uint32_t)was[i] != (uint32_t)cur[i]) {
            ++boot_nvn_dsnap.late_changed;
            if (late_logged < 32) { ++late_logged; char why[160];
                snprintf(why, sizeof why, " stage %u index %u: at the submit call %08x, at translation %08x (handle array %llx, record", stage, first + i,
                         (uint32_t)was[i], (uint32_t)cur[i], (unsigned long long)(at + 8 * i));
                boot_draw_log("nvn native: deferred texture handle changed after the submit call%s %llx, submission %llu)", why, rec, boot_render_exec_id); }
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = (uint32_t)h[i];
        if (e->tex[stage][first + i] != v) ++changed;
        if (boot_draw_trace_file && boot_draw_trace_left)   /* native draw trace (host_nvn_play.h): binds between the traced draws */
            fprintf(boot_draw_trace_file, "  nvn deferred bind stage %u index %u handle %llx (was %08x) from %llx\n", stage, first + i,
                    (unsigned long long)h[i], e->tex[stage][first + i], (unsigned long long)(at + 8 * i));
        if (calls <= 24) boot_draw_log("nvn native: deferred texture bind%s stage/index %llx handle %llx", "", (uint64_t)stage << 32 | (first + i), h[i]);
        e->tex[stage][first + i] = v;
    }
    if (!(calls & (calls - 1)) && calls >= 1024) boot_draw_log("nvn native: deferred texture binds%s %llu (bound handle changed %llu)", "", calls, changed);
}

/* drawcost: memset(p, 0, n) as one rep stosb (the memset the DLL links stores one byte per iteration: the two per-draw
 * clears below were 1 ms per frame on t2). The Windows x64 ABI keeps the direction flag clear. */
static inline void boot_nvn_zero(void *p, size_t n) {
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(0) : "memory");
}
/* sun-work (play-90): TOTK_SHADOW_CLIP (sun-work2: default OFF since play-91; =1 turns it on).
 * TOTK draws its sun cascades depth-only into Z16 targets with SetDepthClamp(TRUE). Honouring the clamp there
 * (DepthClipEnable = FALSE, play-86 onward) left the cascades of the open-world frames flooded with depth 0:
 * play-89 tower view, 4-layer array at 523dc0000: layer 0 100% texels == 0, layer 1 82%, layer 2 28%, layer 3 2%;
 * 10/16 dumps (clipping): 24 such texels in 68 maps. The screen-space sun-shadow pass (ps bdfebdf1) then compares
 * against 0 and returns 0 visibility for all geometry inside the shadow distance (no direct sun, no shadows).
 * With this on, a depth-only draw (no colour target) into a Z16 depth target clips at near/far as before play-86;
 * every other draw keeps the game's depth clamp. No draw is skipped. See play-work/sun-work/NOTES.md.
 * sun-work2 (play-90 data, play-work/sun-work2/NOTES2.md): with the clip on, the near cascades held only Link and
 * the screen-space sun visibility was unchanged (still 0 on the near islands), so the depth-0 flood was not why the
 * sun is missing. The clamp is what the game asks for (pancaking: casters in front of a cascade's near plane must
 * still occlude; nxport honours it too), so the hardware behaviour is the default again. */
static uint64_t boot_nvn_shadow_clipped;
static inline int boot_nvn_shadow_clip(const BootDrawState *s) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_SHADOW_CLIP", v, sizeof v); on = k && k < sizeof v && v[0] == '1'; }
    if (!on || !s->depth_clamp || !s->zt.enabled || !s->zt.address || s->zt.format != 0x13u) return 0;   /* 0x13: Z16_UNORM */
    for (unsigned i = 0; i < s->rt_count && i < 8; ++i) if (s->rt[i].format) return 0;   /* depth-only passes */
    return 1;
}
/* nvnpar-work (TOTK_STATE_CACHE, default on; =0: every field derived for every draw, as before). The draw state is
 * derived in groups, each from its own BootNvnExec fields (the functions below hold the statements of the former single
 * function, in its order):
 *   program        prog, has_prog
 *   targets        rt_count, rt, channel_mask, zt
 *   viewports      vp, depth_range, scissor, origin_upper_left, depth_mode
 *   pools          tex_pool, tex_count, smp_pool, smp_count
 *   vertex input   vattr, n_vattr, vstride, vdivisor, n_vstream, vbuf, vbuf_size
 *   fixed function ds, stencil_ref/mask/vmask[0], polygon, poly_offset, color, blend, blend_color, restart, restart_index,
 *                  depth_clamp (and the targets, which the shadow clip reads)
 *   constant bufs  the program (vertex/pixel stage data and c1 offset), block_va, ubo/ubo_size of NVN stages 0 and 1
 *   driver buffer  per NVN stage s (0 vertex, 1 pixel when there is a pixel stage): block, ssbo[s], ssbo_size[s],
 *                  tex[s], tex_pool
 * A group whose input bytes are the same as at the build before keeps what that build wrote: the BootDrawState and the
 * cbufs are the executor's own (boot_nvn_exec_draw's statics), nothing else writes them, and the draw sets its
 * topology / index / first / count / base fields after this every time (fields no group writes stay 0, as after the
 * full build's zeroing). The driver-buffer puts of a stage write that stage's table and handle words (and the shared
 * pool / clamp words, the same for both stages); the graphics area [0, 0x8100) is written by these puts alone
 * (dispatches use 0x9000 / 0xa000), so with the inputs of the stage's last puts every put would find its bytes equal
 * (no write, no snapshot invalidation; c0_max was raised by the same puts before). The cbufs do not depend on the block
 * bytes and the puts do not depend on the cbufs, so they are separate groups. The c0 binding size and the block write
 * report run every draw. Any failure or a different state/cbuf array rebuilds everything. */
typedef struct BootNvnStateMemo {
    const BootDrawState *s; const BootCbufBinding *cb; int valid, clipped;
    BootNvnProgram prog; int has_prog;
    BootColorTarget rt[8]; uint32_t rt_count, channel_mask; BootDepthTarget zt;
    int32_t vp[16][4]; float depth_range[16][2]; int32_t scissor[16][4]; uint32_t origin_upper_left, depth_mode;
    uint64_t tex_pool, smp_pool; uint32_t tex_count, smp_count;
    uint32_t vattr[16], n_vattr, vstride[16], vdivisor[16], n_vstream; uint64_t vbuf[16], vbuf_size[16];
    uint8_t ds[8]; uint32_t stencil_ref, stencil_mask, stencil_vmask, polygon; float poly_offset[3]; uint32_t color;
    uint8_t blend[8][8]; float blend_color[4]; uint32_t restart, restart_index, depth_clamp;
    uint64_t block_va, ubo[2][16], ubo_size[2][16];   /* constant buffers */
    struct { int valid; uint8_t *block; uint64_t tex_pool, ssbo[16], ssbo_size[16]; uint32_t tex[32]; } db[2];   /* driver buffer per stage, as last put */
    uint64_t calls, builds[9], full;   /* draws, group rebuilds (program, targets, viewports, pools, vertex, fixed, cbufs, driver buffer 0 / 1), full builds */
} BootNvnStateMemo;
static BootNvnStateMemo boot_nvn_state_memo;
#define BOOT_NVN_SAME(f) boot_nvn_same(&m->f, &e->f, sizeof m->f)
#define BOOT_NVN_KEEP(f) memcpy(&m->f, &e->f, sizeof m->f)
static inline void boot_nvn_st_program(BootDrawState *s, const BootNvnProgramStage *vs, const BootNvnProgramStage *ps) {
    s->program_base = 0; boot_nvn_zero(s->stage_enabled, sizeof s->stage_enabled); boot_nvn_zero(s->stage_type, sizeof s->stage_type);
    boot_nvn_zero(s->stage_offset, sizeof s->stage_offset);
    uint64_t vs_code = vs->data + 0x30, ps_code = ps ? ps->data + 0x30 : 0;
    uint64_t base = ps && ps_code < vs_code ? ps_code : vs_code;
    base &= ~UINT64_C(0xffff);
    s->program_base = base; s->stage_enabled[1] = 1; s->stage_type[1] = 1; s->stage_offset[1] = (uint32_t)(vs_code - base);
    if (ps) { s->stage_enabled[5] = 1; s->stage_type[5] = 5; s->stage_offset[5] = (uint32_t)(ps_code - base); }
}
static inline void boot_nvn_st_targets(const BootNvnExec *e, BootDrawState *s) {
    /* targets */
    s->rt_count = e->rt_count;
    for (unsigned i = 0; i < 8; ++i) {
        s->rt_map[i] = i; s->rt[i] = e->rt[i];
        s->rt[i].write_mask = (e->channel_mask >> (4 * i)) & 15;
    }
    s->zt = e->zt;
}
static inline void boot_nvn_st_viewports(const BootNvnExec *e, BootDrawState *s) {
    /* viewports, scissors (NVN window origin upper left: window y grows down, scale_y negative) */
    for (unsigned j = 0; j < 16; ++j) {
        BootViewport *v = &s->viewport[j];
        float x = (float)e->vp[j][0], y = (float)e->vp[j][1], w = (float)e->vp[j][2], h = (float)e->vp[j][3];
        float n = e->depth_range[j][0], f = e->depth_range[j][1];
        v->scale[0] = w / 2; v->offset[0] = x + w / 2;
        v->scale[1] = e->origin_upper_left ? -h / 2 : h / 2; v->offset[1] = y + h / 2;
        if (e->depth_mode) { v->scale[2] = f - n; v->offset[2] = n; } else { v->scale[2] = (f - n) / 2; v->offset[2] = (n + f) / 2; }
        v->x = (uint32_t)e->vp[j][0]; v->y = (uint32_t)e->vp[j][1]; v->w = (uint32_t)e->vp[j][2]; v->h = (uint32_t)e->vp[j][3];
        v->min_z = n < f ? n : f; v->max_z = n < f ? f : n;
        BootScissor *sc = &s->scissor[j];
        sc->enabled = 1; sc->x0 = (uint32_t)e->scissor[j][0]; sc->y0 = (uint32_t)e->scissor[j][1];
        sc->x1 = sc->x0 + (uint32_t)e->scissor[j][2]; sc->y1 = sc->y0 + (uint32_t)e->scissor[j][3];
    }
    s->window_origin_lower_left = !e->origin_upper_left; s->flip_y = 0;
    s->z_clip_zero_to_w = e->depth_mode ? 1 : 0;
}
static inline void boot_nvn_st_pools(const BootNvnExec *e, BootDrawState *s) {
    /* textures and samplers: the driver wrote TIC/TSC entries into the pools */
    s->tic_address = e->tex_pool; s->tic_max = e->tex_count ? e->tex_count - 1 : 0;
    s->tsc_address = e->smp_pool; s->tsc_max = e->smp_count ? e->smp_count - 1 : 0; s->sampler_via_header = 0;
}
static inline void boot_nvn_st_vertex(const BootNvnExec *e, BootDrawState *s) {
    /* vertex input */
    for (unsigned j = 0; j < 16; ++j) {
        BootVertexStream *st = &s->stream[j];
        st->stride = j < e->n_vstream ? e->vstride[j] : 0; st->enabled = e->vbuf[j] != 0;
        st->address = e->vbuf[j]; st->limit = e->vbuf[j] + (e->vbuf_size[j] ? e->vbuf_size[j] - 1 : 0);
        st->divisor = j < e->n_vstream ? e->vdivisor[j] : 0; st->per_instance = st->divisor != 0;
    }
    boot_nvn_zero(s->attrib, sizeof s->attrib);   /* as after the full build's zeroing: attributes below set only some fields */
    for (unsigned i = 0; i < 32; ++i) {
        BootVertexAttrib *a = &s->attrib[i];
        if (i >= e->n_vattr) { a->inactive = 1; continue; }
        uint32_t wv = e->vattr[i], size = 0, type = 0;
        a->stream = wv & 31; a->offset = (wv >> 7) & 0x3fff;
        if (!boot_nvn_vertex_format(wv >> 21, &size, &type)) { a->inactive = 1; continue; }
        a->size = size; a->type = type;
    }
}
/* Returns 1 when the shadow clip applied (counted per draw, also when the group is kept). */
static inline int boot_nvn_st_fixed(const BootNvnExec *e, BootDrawState *s) {
    /* fixed function from the driver's state objects */
    uint8_t d0 = e->ds[0];
    s->depth_test = d0 & 1; s->depth_write = (d0 >> 1) & 1; s->stencil_test = (d0 >> 2) & 1; s->depth_func = d0 >> 4;
    s->stencil_func = e->ds[4] & 15; s->stencil_fail = e->ds[4] >> 4; s->stencil_zfail = e->ds[5] >> 4; s->stencil_zpass = e->ds[5] & 15;
    s->stencil_ref = e->stencil_ref[0]; s->stencil_func_mask = e->stencil_vmask[0]; s->stencil_mask = e->stencil_mask[0];
    uint32_t cull = e->polygon & 3;
    s->cull_enable = cull != 0; s->cull_face = cull == 1 ? 0x404 : cull == 2 ? 0x405 : 0x408;
    s->front_face = (e->polygon >> 2) & 1 ? 0x901 : 0x900;
    s->depth_bias_fill = (e->polygon >> 15) & 1;
    s->depth_bias = e->poly_offset[1]; s->depth_bias_slope = e->poly_offset[0]; s->depth_bias_clamp = e->poly_offset[2];
    s->blend_per_target = 1;
    for (unsigned t = 0; t < 8; ++t) {
        BootBlend *b = &s->blend[t]; const uint8_t *o = e->blend[t];
        b->enabled = (e->color >> (8 + t)) & 1; b->separate_alpha = 1;
        b->color_op = o[3] & 15; b->alpha_op = o[3] >> 4;
        b->color_src = o[4]; b->color_dst = o[5]; b->alpha_src = o[6]; b->alpha_dst = o[7];
    }
    memcpy(s->blend_const, e->blend_color, 16);
    s->primitive_restart = e->restart; s->restart_index = e->restart_index;
    {   static int honour = -1; if (honour < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_DEPTH_CLAMP", v, sizeof v); honour = !(k && k < sizeof v && v[0] == '0'); }
        s->depth_clamp = honour ? e->depth_clamp : 0; }   /* TOTK_DEPTH_CLAMP=0: old behaviour (always clip) */
    if (boot_nvn_shadow_clip(s)) {   /* sun-work: depth-only Z16 (shadow) pass clips, see boot_nvn_shadow_clip */
        s->depth_clamp = 0;
        if (!boot_nvn_shadow_clipped++) boot_draw_log("nvn native: shadow clip (TOTK_SHADOW_CLIP): depth clamp on a depth-only Z16 target drawn with near/far clipping%s %llu %llu", "", s->zt.address, s->zt.width);
        return 1;
    }
    return 0;
}
static inline void boot_nvn_st_buffers(BootCbufBinding cbufs[5][18], const BootNvnExec *e, const BootNvnProgramStage *vs, const BootNvnProgramStage *ps) {
    /* constant buffers per bind group */
    boot_nvn_zero(cbufs, sizeof(BootCbufBinding) * 5 * 18);
    for (unsigned stage = 0; stage < 2; ++stage) {
        const BootNvnProgramStage *ps2 = stage ? ps : vs;
        if (!ps2) continue;
        unsigned g = boot_nvn_stage_group[stage];
        BootCbufBinding *cb = cbufs[g];
        cb[0] = (BootCbufBinding){e->block_va, BOOT_NVN_C0_SIZE, 1};
        cb[1] = (BootCbufBinding){ps2->data + ps2->c1_offset, 0x10000, 2};   /* read on the GPU (valid 2) */
        cb[2] = (BootCbufBinding){e->block_va + BOOT_NVN_C2_BASE + g * 0x200, 0x200, 1};
        for (unsigned i = 0; i < 15; ++i) {
            if (e->ubo[stage][i]) cb[3 + i] = (BootCbufBinding){e->ubo[stage][i], (uint32_t)((e->ubo_size[stage][i] + 255) & ~UINT64_C(255)), 2};
            else cb[3 + i] = (BootCbufBinding){e->block_va + BOOT_NVN_DUMMY_BASE, 256, 1};
        }
    }
}
/* Draw state for the current bindings; returns 0 (logged) when something is not representable. */
static inline int boot_nvn_exec_state(BootNvnExec *e, BootDrawState *s, BootCbufBinding cbufs[5][18]) {
    BootNvnStateMemo *m = &boot_nvn_state_memo;
    if (!e->has_prog) { boot_nvn_zero(s, sizeof *s); m->valid = 0; return 0; }
    const BootNvnProgramStage *vs = NULL, *ps = NULL;
    for (unsigned k = 0; k < e->prog.count; ++k) { if (e->prog.stage[k].kind == 1) vs = &e->prog.stage[k]; else if (e->prog.stage[k].kind == 5) ps = &e->prog.stage[k]; }
    if (!vs) { boot_nvn_zero(s, sizeof *s); m->valid = 0; return 0; }
    enum { G_PROG = 1, G_RT = 2, G_VP = 4, G_POOL = 8, G_VTX = 16, G_FIX = 32, G_CB = 64, G_DB0 = 128, G_DB1 = 256, G_ALL = 511 };
    unsigned dirty = G_ALL;
    ++m->calls;
    if (boot_nvn_state_cache_on() && m->valid && m->s == s && m->cb == &cbufs[0][0]) {
        dirty = 0;
        if (!BOOT_NVN_SAME(prog) || m->has_prog != e->has_prog) dirty |= G_PROG | G_CB;   /* the constant buffers read the stages' program words */
        if (!BOOT_NVN_SAME(rt) || m->rt_count != e->rt_count || m->channel_mask != e->channel_mask || !BOOT_NVN_SAME(zt)) dirty |= G_RT | G_FIX;
        if (!BOOT_NVN_SAME(vp) || !BOOT_NVN_SAME(depth_range) || !BOOT_NVN_SAME(scissor) || m->origin_upper_left != e->origin_upper_left ||
            m->depth_mode != e->depth_mode) dirty |= G_VP;
        if (m->tex_pool != e->tex_pool || m->smp_pool != e->smp_pool || m->tex_count != e->tex_count || m->smp_count != e->smp_count) dirty |= G_POOL;
        if (!BOOT_NVN_SAME(vattr) || m->n_vattr != e->n_vattr || !BOOT_NVN_SAME(vstride) || !BOOT_NVN_SAME(vdivisor) || m->n_vstream != e->n_vstream ||
            !BOOT_NVN_SAME(vbuf) || !BOOT_NVN_SAME(vbuf_size)) dirty |= G_VTX;
        if (!BOOT_NVN_SAME(ds) || m->stencil_ref != e->stencil_ref[0] || m->stencil_mask != e->stencil_mask[0] || m->stencil_vmask != e->stencil_vmask[0] ||
            m->polygon != e->polygon || !BOOT_NVN_SAME(poly_offset) || m->color != e->color || !BOOT_NVN_SAME(blend) || !BOOT_NVN_SAME(blend_color) ||
            m->restart != e->restart || m->restart_index != e->restart_index || m->depth_clamp != e->depth_clamp) dirty |= G_FIX;
        if (m->block_va != e->block_va || !boot_nvn_same(m->ubo, e->ubo, sizeof m->ubo) || !boot_nvn_same(m->ubo_size, e->ubo_size, sizeof m->ubo_size)) dirty |= G_CB;
        for (unsigned st = 0; st < 2; ++st)
            if (!m->db[st].valid || m->db[st].block != e->block || m->db[st].tex_pool != e->tex_pool || !boot_nvn_same(m->db[st].ssbo, e->ssbo[st], sizeof m->db[st].ssbo) ||
                !boot_nvn_same(m->db[st].ssbo_size, e->ssbo_size[st], sizeof m->db[st].ssbo_size) || !boot_nvn_same(m->db[st].tex, e->tex[st], sizeof m->db[st].tex))
                dirty |= st ? G_DB1 : G_DB0;
    }
    if (dirty == G_ALL) { boot_nvn_zero(s, sizeof *s); ++m->full; }   /* the full build: everything from zero, as before */
    if (!ps) dirty &= ~(unsigned)G_DB1;   /* no pixel stage: its driver buffer is not written (as before); its memo stays */
    for (unsigned g = 0; g < 9; ++g) if (dirty & (1u << g)) ++m->builds[g];
    if (dirty & G_PROG) boot_nvn_st_program(s, vs, ps);
    if (dirty & G_RT) boot_nvn_st_targets(e, s);
    if (dirty & G_VP) boot_nvn_st_viewports(e, s);
    if (dirty & G_POOL) boot_nvn_st_pools(e, s);
    if (dirty & G_VTX) boot_nvn_st_vertex(e, s);
    if (dirty & G_FIX) m->clipped = boot_nvn_st_fixed(e, s);
    else if (m->clipped) ++boot_nvn_shadow_clipped;   /* counted per draw, as the build does (its first one logged) */
    if (dirty & G_CB) boot_nvn_st_buffers(cbufs, e, vs, ps);
    if (dirty & G_DB0) boot_nvn_exec_driver_buffer(e, 0);   /* the stages' puts, in the order of before */
    if (dirty & G_DB1) boot_nvn_exec_driver_buffer(e, 1);
    for (unsigned g = 0; g < 5; ++g)   /* after this draw's driver-buffer writes */
        if (cbufs[g][0].valid && cbufs[g][0].address == e->block_va) cbufs[g][0].size = boot_nvn_c0_size();
    boot_nvn_exec_block_written(e);
    if (dirty & G_PROG) { BOOT_NVN_KEEP(prog); m->has_prog = e->has_prog; }
    if (dirty & G_RT) { BOOT_NVN_KEEP(rt); m->rt_count = e->rt_count; m->channel_mask = e->channel_mask; BOOT_NVN_KEEP(zt); }
    if (dirty & G_VP) { BOOT_NVN_KEEP(vp); BOOT_NVN_KEEP(depth_range); BOOT_NVN_KEEP(scissor); m->origin_upper_left = e->origin_upper_left; m->depth_mode = e->depth_mode; }
    if (dirty & G_POOL) { m->tex_pool = e->tex_pool; m->smp_pool = e->smp_pool; m->tex_count = e->tex_count; m->smp_count = e->smp_count; }
    if (dirty & G_VTX) { BOOT_NVN_KEEP(vattr); m->n_vattr = e->n_vattr; BOOT_NVN_KEEP(vstride); BOOT_NVN_KEEP(vdivisor); m->n_vstream = e->n_vstream; BOOT_NVN_KEEP(vbuf); BOOT_NVN_KEEP(vbuf_size); }
    if (dirty & G_FIX) { BOOT_NVN_KEEP(ds); m->stencil_ref = e->stencil_ref[0]; m->stencil_mask = e->stencil_mask[0]; m->stencil_vmask = e->stencil_vmask[0];
                         m->polygon = e->polygon; BOOT_NVN_KEEP(poly_offset); m->color = e->color; BOOT_NVN_KEEP(blend); BOOT_NVN_KEEP(blend_color);
                         m->restart = e->restart; m->restart_index = e->restart_index; m->depth_clamp = e->depth_clamp; }
    if (dirty & G_CB) { m->block_va = e->block_va; memcpy(m->ubo, e->ubo, sizeof m->ubo); memcpy(m->ubo_size, e->ubo_size, sizeof m->ubo_size); }
    for (unsigned st = 0; st < 2; ++st) if (dirty & (st ? G_DB1 : G_DB0)) {
        m->db[st].valid = 1; m->db[st].block = e->block; m->db[st].tex_pool = e->tex_pool;
        memcpy(m->db[st].ssbo, e->ssbo[st], sizeof m->db[st].ssbo); memcpy(m->db[st].ssbo_size, e->ssbo_size[st], sizeof m->db[st].ssbo_size); memcpy(m->db[st].tex, e->tex[st], sizeof m->db[st].tex);
    }
    m->s = s; m->cb = &cbufs[0][0]; m->valid = 1;
    return 1;
}
#undef BOOT_NVN_SAME
#undef BOOT_NVN_KEEP

#endif
