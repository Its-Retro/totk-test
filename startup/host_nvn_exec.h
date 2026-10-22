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
/* data-snapshot: the copy arena (host_gpu_render.h boot_ds_arena) follows the driver buffer in the same block, so the page table
 * maps it next to the driver buffer and vertex streams read from a copy go through gmem_load like guest memory. The block is
 * BOOT_NVN_NATIVE_BLOCK (2 MiB) exactly with TOTK_DATA_SNAPSHOT=0. */
static inline uint64_t boot_nvn_block_bytes(void) { return BOOT_NVN_NATIVE_BLOCK + boot_ds_arena_bytes(); }
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
    float vp[16][4]; float depth_range[16][2]; int32_t scissor[16][4];   /* batch-3 N10: vp float (SetViewports takes floats) */
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
/* NVN vertex format -> Maxwell (size code, numeric type, bgra). 0 size = unsupported. The hand-written table (formats
 * 1-48, 61-63, 105), kept for TOTK_VERTEX_FORMATS=0 (batch-3 K3, below). */
static inline int boot_nvn_vertex_format_hand(uint32_t f, uint32_t *size, uint32_t *type) {
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
/* batch-3 K3: NVN vertex format -> Maxwell (size code, numeric type) from the NVN driver's own vertex attribute format
 * column (sdk+0xd01614 + 48 * format - 8, i.e. byte +0x10 of each 48-byte per-format record whose +0x18 is the
 * render-target format of boot_nvn_rt_formats and +0x20 the TIC word of boot_nvn_format_tic): size bits 21-26, type
 * bits 27-29 (1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled, 7 float). 0x26800000 is the driver's placeholder
 * for formats with no vertex encoding (depth, compressed, RGBX*, RGB9E5F, BGR*): 0 (unsupported) here too. Written by
 * gen_game_headers.py from the dumped 1.0.0 sdk. Formats 1-48, 61-63 and 105 match the hand-written table above bit for
 * bit; 81-104 (*_UI2F / *_I2F: integer data the fetch unit converts to float) and 106-108 (RGB10A2 I / UI2F / I2F) are
 * new: the hand-written table left those attributes inactive, reading (0,0,0,1). maxwell_runtime.hlsli conv_ already
 * converts types 5 and 6, and vin_layout has every size code these use. Cross-check: nxport (MIT)
 * crates/engine/src/metal/translated.rs:2134-2216 maps 81-104 the same way (it also maps 64 and 109-120, which the
 * driver does not, and lacks 106-108). TOTK_VERTEX_FORMATS=0: the hand-written table, as before. */
#include "generated_nvn_format_vertex.h"   /* boot_nvn_vertex_words[161]: gen_game_headers.py, from the dumped sdk */
static inline int boot_nvn_vertex_format(uint32_t f, uint32_t *size, uint32_t *type) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_VERTEX_FORMATS", v, sizeof v); on = !(k && k < sizeof v && v[0] == '0'); }
    if (!on) return boot_nvn_vertex_format_hand(f, size, type);
    uint32_t w = f < 161 ? boot_nvn_vertex_words[f] : 0;
    if (!w) return 0;
    *size = (w >> 21) & 0x3f; *type = (w >> 27) & 7;
    return 1;
}
/* K3 diagnostic (TOTK_STATE_DIAG): a vertex format code is logged once when only the driver table decodes it, or when it
 * has no vertex encoding (the attribute reads (0,0,0,1)); format 0 (none) is not. */
static inline void boot_nvn_vertex_format_note(uint32_t f, unsigned attr, int decoded) {
    if (!boot_render_state_diag_on() || (!decoded && !f)) return;
    uint32_t s0, t0;
    if (decoded && boot_nvn_vertex_format_hand(f, &s0, &t0)) return;
    if (decoded) ++boot_state_diag.vfmt_new; else ++boot_state_diag.vfmt_unsupported;
    static uint8_t seen[162]; uint32_t k = f < 161 ? f : 161;
    if (seen[k]) return;
    seen[k] = 1;
    boot_gp_log("nvn native: K3 vertex format %u (attribute %u): %s", f, attr,
                decoded ? "decoded from the driver table (before: inactive, read as (0,0,0,1))" : "no vertex encoding, the attribute reads (0,0,0,1)");
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
        if ((boot_ax_live(BOOT_AX_L_DIAG, sizeof now), boot_host_read(c, self, tex, now, sizeof now)) && memcmp(now + 0x10, o + 0x10, 0x10) && boot_nvn_target_object_changed++ < 16) {   /* async-xlate: counted on X */
            uint64_t a, b; memcpy(&a, o + 0x18, 8); memcpy(&b, now + 0x18, 8);
            boot_draw_log("nvn native: target texture re-initialized since recording%s (recorded offset %llx, now %llx)", "", a & UINT64_C(0xffffffffffff), b & UINT64_C(0xffffffffffff));
        }
    } else if ((boot_ax_live(BOOT_AX_L_OBJECT, sizeof o), !boot_host_read(c, self, tex, o, sizeof o))) return 0;   /* async-xlate: counted on X */
    uint64_t pool, off, va = 0; memcpy(&pool, o + 0x10, 8); memcpy(&off, o + 0x18, 8);
    if (!pool) return 0;
    if (!boot_ax_obj_word(pool + 0x18, &va) && (boot_ax_live(BOOT_AX_L_POOLWORD, 8), !boot_host_read(c, self, pool + 0x18, &va, 8))) return 0;   /* async-xlate: on X the word t2 captured */
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
/* gpu-resident (TOTK_GPU_RESIDENT, default 1; =0: every block a GPU mapping uses is opened whole, exactly as before; =2: dry
 * run, whole blocks as with 0 plus the perf3 "guest heaps" part saying what 1 would open; mechanism and measurements:
 * host_gpu_render.h boot_gr_view, play-work/gpu-resident/README.md). The GPU can only reach guest memory through the GPU
 * mappings (NVN memory pools mapped by the driver; CPU-only pools have none), so with 1 a mapping inside a fastmem section is
 * given a view of just its span instead of the whole block:
 *  - spans: the mappings' section byte ranges rounded out to the granule (TOTK_GPU_RESIDENT_KB, default 2 MiB) and merged
 *    where they overlap or touch, per section. Mappings adjacent in host memory always share a view, so a range that spans
 *    them stays contiguous in one region (boot_render_guest, constant buffer and vertex views need that), as with whole
 *    blocks. More spans than BOOT_NVN_GR_BUDGET (or the number of sections, if larger): the smallest gaps between spans of a
 *    section are bridged until they fit, which locks the fewest extra bytes for that count (a section bridged throughout
 *    is one span, the whole-block case). (Doubling the granule instead merged a section whole as soon as the granule
 *    reached the spacing of regularly placed pools: 400 pools of 64 KiB 7 MiB apart became one 2.8 GiB span.);
 *  - a span is served by the smallest live view of its section containing it, unless that view is more than twice the span
 *    plus 32 MiB; otherwise a new view of the span plus 1/16 of its size (at least a granule) on both sides is opened, so a
 *    growing span is reopened a logarithmic number of times;
 *  - views (and whole-block regions of sections) no mapping uses are released right after the next page-table rebuild,
 *    which waits for the queue (as every rebuild did), so nothing recorded can name them. Unmapped or finalized pools thus
 *    stop pinning RAM: their pages become ordinary pageable memory. A region table filled meanwhile by old and new views is
 *    reclaimed at once: the queue is waited for and the views this assignment did not pick are released (the old page
 *    table they serve is replaced before anything is recorded again);
 * The page table, the regions' layout (one buffer per region), the shaders and every read and write path are unchanged:
 * the GPU does exactly the same work, only fewer pages are locked. Not section-backed blocks (the native block,
 * VirtualAlloc'd memory), a section changed under the assignment, or a view that cannot be opened (region table full, D3D12
 * failure) fall back to the whole block (counted as fallbacks / failures). The assignment is cached while the mappings, the
 * sections and the views are unchanged (one hash per submission). The idea is nxport's (MIT; reference/nxport-main,
 * crates/engine/src/metal/translated.rs pool_buffer_slow / drop_released_pool_buffers): one zero-copy buffer per pool's
 * page-rounded range, made when the GPU first uses it and dropped when the pool's memory is released. Here the GPU mappings
 * stand for the pools (shaders reach memory through the page table, not per binding), merged into spans for the 256-region
 * limit, and released at the rebuild after their mapping goes. */
#define BOOT_NVN_GR_DEFER (-2)
#define BOOT_NVN_GR_BUDGET 192u   /* spans (of 256 region slots); a table filled by old and new views is reclaimed (boot_nvn_gr_reclaim) */
#define BOOT_NVN_GR_SECS 512u
typedef struct BootNvnGrSec { const uint8_t *host; uint64_t size, gen; HANDLE dup, src; } BootNvnGrSec;
static BootNvnGrSec boot_nvn_gr_sec[BOOT_NVN_GR_SECS]; static unsigned boot_nvn_gr_nsec;
static uint64_t boot_nvn_gr_sec_gen, boot_nvn_gr_seen_allocs = UINT64_MAX, boot_nvn_gr_seen_frees = UINT64_MAX;
typedef struct BootNvnGrEnt { uint64_t so, len, blk_size; uint8_t *blk; uint32_t sec, map; } BootNvnGrEnt;
typedef struct BootNvnGrSpan { uint64_t a, b, need; uint32_t sec, first, last; } BootNvnGrSpan;
static BootNvnGrEnt boot_nvn_gr_ent[BOOT_NVN_MAPS]; static uint32_t boot_nvn_gr_ord[BOOT_NVN_MAPS]; static unsigned boot_nvn_gr_n;
static BootNvnGrSpan boot_nvn_gr_span[BOOT_NVN_MAPS]; static unsigned boot_nvn_gr_nspan;
typedef struct BootNvnGrGap { uint64_t gap; uint32_t at, pad; } BootNvnGrGap;
static BootNvnGrGap boot_nvn_gr_gap[BOOT_NVN_MAPS]; static uint8_t boot_nvn_gr_bridge[BOOT_NVN_MAPS];
static int boot_nvn_gr_reg[BOOT_NVN_MAPS]; static uint64_t boot_nvn_gr_off[BOOT_NVN_MAPS];   /* last assignment, by deferred ordinal */
static uint64_t boot_nvn_gr_hash, boot_nvn_gr_w; static unsigned boot_nvn_gr_hash_n; static const BootRender *boot_nvn_gr_hash_r;
static uint32_t boot_nvn_gr_pass, boot_nvn_gr_pick[BOOT_RENDER_MAX_REGIONS];   /* assignment pass that picked the view in a slot */
static int boot_nvn_gr_best[BOOT_NVN_MAPS];   /* per span: its region */
/* Region table full while assigning: waits for the queue and releases the views this pass did not pick. Number released. */
static inline unsigned boot_nvn_gr_reclaim(BootRender *r) {
    unsigned freed = 0; int quiet = 0;
    for (unsigned j = 0; j < r->region_count; ++j) {
        const BootGrView *v = &boot_gr_view[j];
        if (!v->cpu || v->r != r || r->region_cpu[j] != v->cpu || boot_nvn_gr_pick[j] == boot_nvn_gr_pass) continue;
        if (!quiet) { if (!boot_render_gpu_quiet(r)) return 0; quiet = 1; }
        boot_gr_slot_release(r, j); ++freed;
    }
    ++boot_gr_st.reclaims;
    return freed;
}
/* The fastmem sections (host view, size, a duplicate of the section handle), copied while the section table cannot change: the
 * caller holds the kernel lock or is the kernel. A section keeps its generation while its handle names the same object
 * (CompareObjectHandles); views of a generation no longer listed turn stale (never used again, released at the next rebuild). */
static inline void boot_nvn_gr_refresh_locked(void) {
#if TOTK_FASTMEM
    static BOOL (WINAPI *same)(HANDLE, HANDLE); static int same_init;
    if (!same_init) { same_init = 1; HMODULE kb = GetModuleHandleA("kernelbase.dll");
        if (kb) same = (BOOL (WINAPI *)(HANDLE, HANDLE))(void (*)(void))GetProcAddress(kb, "CompareObjectHandles"); }
    static BootNvnGrSec next[BOOT_NVN_GR_SECS]; unsigned n = 0;
    for (uint32_t i = 0; i < boot_fm.alloc_count && n < BOOT_NVN_GR_SECS; ++i) {
        const BootFmAlloc *a = &boot_fm.allocs[i];
        if (!a->section || a->hwm != a->size) continue;   /* SEC_COMMIT sections only: every page of a view of one is committed */
        unsigned k = 0;
        while (k < boot_nvn_gr_nsec && !(boot_nvn_gr_sec[k].dup && boot_nvn_gr_sec[k].host == a->host && boot_nvn_gr_sec[k].size == a->size &&
                                          boot_nvn_gr_sec[k].src == a->section && same && same(boot_nvn_gr_sec[k].dup, a->section))) ++k;
        if (k < boot_nvn_gr_nsec) { next[n++] = boot_nvn_gr_sec[k]; boot_nvn_gr_sec[k].dup = NULL; continue; }
        HANDLE d = NULL;
        if (!DuplicateHandle(GetCurrentProcess(), a->section, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS) || !d) continue;
        next[n].host = a->host; next[n].size = a->size; next[n].gen = ++boot_nvn_gr_sec_gen; next[n].dup = d; next[n].src = a->section; ++n;
    }
    for (unsigned k = 0; k < boot_nvn_gr_nsec; ++k) if (boot_nvn_gr_sec[k].dup) CloseHandle(boot_nvn_gr_sec[k].dup);   /* views keep a section alive themselves */
    memcpy(boot_nvn_gr_sec, next, n * sizeof *next); boot_nvn_gr_nsec = n;   /* boot_fm.allocs is sorted by host: so is this */
    for (unsigned j = 0; j < BOOT_RENDER_MAX_REGIONS; ++j) {
        BootGrView *v = &boot_gr_view[j];
        if (!v->cpu || v->stale) continue;
        unsigned k = 0; while (k < n && boot_nvn_gr_sec[k].gen != v->gen) ++k;
        if (k == n) { v->stale = 1; ++boot_gr_st.stale; ++boot_gr_vgen; }
    }
    boot_nvn_gr_seen_allocs = boot_fm.section_allocs; boot_nvn_gr_seen_frees = boot_fm.section_frees; ++boot_gr_st.refreshes;
#endif
}
/* At the start of a submission's memory sync, when a section was made or freed since the last copy. With TOTK_CORES>1 the
 * kernel lock is taken as boot_mt_gpu_pack takes it (lock order: kernel lock, then w->lock), before anything is read. */
static inline void boot_nvn_gr_refresh(BootContext *c, BootWindow *w) {
#if TOTK_FASTMEM
    if (__atomic_load_n(&boot_fm.section_allocs, __ATOMIC_ACQUIRE) == boot_nvn_gr_seen_allocs &&
        __atomic_load_n(&boot_fm.section_frees, __ATOMIC_ACQUIRE) == boot_nvn_gr_seen_frees) return;
    BootCore *k = boot_mt.active && boot_mt_core_ctx(c) ? (BootCore *)(void *)c : NULL;
    if (k) {
        ReleaseSRWLockExclusive(&w->lock);
        __atomic_store_n(&k->in_guest, 0, __ATOMIC_SEQ_CST);
        AcquireSRWLockExclusive(&boot_mt.klock);
        boot_mt.kernel_core = k->index;
        AcquireSRWLockExclusive(&w->lock);
    }
    boot_nvn_gr_refresh_locked();
    if (k) { c->exclusive_valid = 0; boot_mt.kernel_core = UINT32_MAX; ReleaseSRWLockExclusive(&boot_mt.klock); boot_mt_guest_enter(k); }
#else
    (void)c; (void)w;
#endif
}
static inline int boot_nvn_gr_find(const uint8_t *blk, uint64_t size) {   /* section holding [blk, blk + size), or -1 */
    unsigned lo = 0, hi = boot_nvn_gr_nsec;
    while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (boot_nvn_gr_sec[mid].host <= blk) lo = mid + 1; else hi = mid; }
    if (!lo) return -1;
    const BootNvnGrSec *s = &boot_nvn_gr_sec[lo - 1];
    return blk >= s->host && (uint64_t)(blk - s->host) <= s->size && size <= s->size - (uint64_t)(blk - s->host) ? (int)(lo - 1) : -1;
}
static inline int boot_nvn_gr_is_host(const uint8_t *p) {   /* p is the host view base of a listed section */
    int k = p ? boot_nvn_gr_find(p, 1) : -1;
    return k >= 0 && boot_nvn_gr_sec[k].host == p;
}
static inline int boot_nvn_gr_cmp(const void *x, const void *y) {
    const uint32_t i = *(const uint32_t *)x, j = *(const uint32_t *)y;
    const BootNvnGrEnt *a = &boot_nvn_gr_ent[i], *b = &boot_nvn_gr_ent[j];
    if (a->sec != b->sec) return a->sec < b->sec ? -1 : 1;
    if (a->so != b->so) return a->so < b->so ? -1 : 1;
    return i < j ? -1 : i > j;
}
/* Spans at granule w over the sorted entries; stored when fill. Returns the count. */
static inline unsigned boot_nvn_gr_spans(uint64_t w, int fill) {
    unsigned n = 0; BootNvnGrSpan cur; memset(&cur, 0, sizeof cur); uint64_t ub = 0;
    for (uint32_t k = 0; k < boot_nvn_gr_n; ++k) {
        const BootNvnGrEnt *e = &boot_nvn_gr_ent[boot_nvn_gr_ord[k]];
        const uint64_t ssz = boot_nvn_gr_sec[e->sec].size, end = e->so + e->len;
        uint64_t ea = e->so / w * w, eb = (end + w - 1) / w * w;
        if (eb > ssz) eb = ssz;
        if (!k || e->sec != cur.sec || ea > cur.b) {
            if (k) { if (fill) { cur.last = k; boot_nvn_gr_span[n] = cur; } ++n; }
            cur.a = ea; cur.b = eb; cur.sec = e->sec; cur.first = k; cur.need = e->len; ub = end;
        } else {
            if (eb > cur.b) cur.b = eb;
            if (e->so >= ub) { cur.need += e->len; ub = end; } else if (end > ub) { cur.need += end - ub; ub = end; }
        }
    }
    if (boot_nvn_gr_n) { if (fill) { cur.last = boot_nvn_gr_n; boot_nvn_gr_span[n] = cur; } ++n; }
    if (fill) boot_nvn_gr_nspan = n;
    return n;
}
static inline int boot_nvn_gr_gap_cmp(const void *x, const void *y) {
    const BootNvnGrGap *a = (const BootNvnGrGap *)x, *b = (const BootNvnGrGap *)y;
    if (a->gap != b->gap) return a->gap < b->gap ? -1 : 1;
    return a->at < b->at ? -1 : a->at > b->at;
}
/* Sorts the entries and builds the spans at the granule, then bridges the smallest gaps down to the budget. */
static inline void boot_nvn_gr_build(void) {
    boot_nvn_gr_w = boot_gr_granule();
    for (uint32_t k = 0; k < boot_nvn_gr_n; ++k) boot_nvn_gr_ord[k] = k;
    qsort(boot_nvn_gr_ord, boot_nvn_gr_n, sizeof boot_nvn_gr_ord[0], boot_nvn_gr_cmp);
    const unsigned n = boot_nvn_gr_spans(boot_nvn_gr_w, 1);
    unsigned sections = 0, ng = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (!i || boot_nvn_gr_span[i].sec != boot_nvn_gr_span[i - 1].sec) { ++sections; continue; }
        boot_nvn_gr_gap[ng].gap = boot_nvn_gr_span[i].a - boot_nvn_gr_span[i - 1].b; boot_nvn_gr_gap[ng].at = i; boot_nvn_gr_gap[ng].pad = 0; ++ng;
    }
    const unsigned budget = sections > BOOT_NVN_GR_BUDGET ? sections : BOOT_NVN_GR_BUDGET;
    if (n <= budget) return;
    qsort(boot_nvn_gr_gap, ng, sizeof boot_nvn_gr_gap[0], boot_nvn_gr_gap_cmp);
    unsigned bridge = n - budget; if (bridge > ng) bridge = ng;
    memset(boot_nvn_gr_bridge, 0, n);
    for (unsigned k = 0; k < bridge; ++k) boot_nvn_gr_bridge[boot_nvn_gr_gap[k].at] = 1;
    unsigned m = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (i && boot_nvn_gr_bridge[i]) { BootNvnGrSpan *p = &boot_nvn_gr_span[m - 1]; p->b = boot_nvn_gr_span[i].b; p->need += boot_nvn_gr_span[i].need; p->last = boot_nvn_gr_span[i].last; }
        else boot_nvn_gr_span[m++] = boot_nvn_gr_span[i];
    }
    boot_nvn_gr_nspan = m; boot_gr_st.coarsened += bridge;
}
static inline uint64_t boot_nvn_gr_headroom(uint64_t a, uint64_t b) {   /* 1/16 of the span, at least a granule, in granules */
    const uint64_t w = boot_nvn_gr_w, h = (b - a) / 16;
    return h <= w ? w : (h + w - 1) / w * w;
}
/* Assigns a region and offset to every deferred mapping (maps[].region == BOOT_NVN_GR_DEFER) and folds them into sig. */
static inline int boot_nvn_gr_assign(BootRender *r, BootNvnMap *maps, uint64_t *sig) {
    const unsigned n = boot_nvn_gr_n;
    boot_gr_check(r);
    uint64_t h = UINT64_C(1469598103934665603) ^ n ^ (boot_gr_vgen << 20) ^ (boot_gr_granule() << 1);
    for (unsigned k = 0; k < n; ++k) {
        const BootNvnGrEnt *e = &boot_nvn_gr_ent[k];
        h = (h ^ e->so) * UINT64_C(1099511628211); h = (h ^ e->len) * UINT64_C(1099511628211);
        h = (h ^ boot_nvn_gr_sec[e->sec].gen) * UINT64_C(1099511628211);
    }
    int cached = boot_nvn_gr_hash_r == r && boot_nvn_gr_hash_n == n && boot_nvn_gr_hash == h;
    if (!cached) {
        const int raced = __atomic_load_n(&boot_fm.section_frees, __ATOMIC_ACQUIRE) != boot_nvn_gr_seen_frees;   /* a section went since the copy */
        boot_nvn_gr_build(); ++boot_nvn_gr_pass;
        for (unsigned s = 0; s < boot_nvn_gr_nspan; ++s) {   /* 1: spans an existing view serves */
            const BootNvnGrSpan *sp = &boot_nvn_gr_span[s]; const BootNvnGrSec *sec = &boot_nvn_gr_sec[sp->sec];
            int best = -1; uint64_t best_size = UINT64_MAX;
            for (unsigned j = 0; j < r->region_count && !raced; ++j) {
                const BootGrView *v = &boot_gr_view[j];
                if (!v->cpu || v->stale || v->gen != sec->gen || v->lo > sp->a || sp->b > v->lo + v->size) continue;
                if (v->size > 2 * (sp->b - sp->a) + (UINT64_C(32) << 20) || v->size >= best_size) continue;
                best = (int)j; best_size = v->size;
            }
            if (best >= 0) { ++boot_gr_st.reused; boot_render_region_used[best] = 1; boot_nvn_gr_pick[best] = boot_nvn_gr_pass; }
            boot_nvn_gr_best[s] = best;
        }
        for (unsigned s = 0; s < boot_nvn_gr_nspan; ++s) {   /* 2: new views (a full table gives up the views no span picked) */
            const BootNvnGrSpan *sp = &boot_nvn_gr_span[s]; const BootNvnGrSec *sec = &boot_nvn_gr_sec[sp->sec];
            int best = boot_nvn_gr_best[s];
            if (best < 0 && !raced) {
                const uint64_t room = boot_nvn_gr_headroom(sp->a, sp->b);
                uint64_t a = sp->a > room ? sp->a - room : 0, b = sp->b + room < sec->size ? sp->b + room : sec->size;
                a &= ~UINT64_C(0xffff); b = (b + 0xfff) & ~UINT64_C(0xfff);
                best = boot_gr_view_open(r, sec->dup, a, b - a, sec->gen);
                if (best < 0 && r->region_count == BOOT_RENDER_MAX_REGIONS && boot_nvn_gr_reclaim(r)) best = boot_gr_view_open(r, sec->dup, a, b - a, sec->gen);
                if (best >= 0) boot_nvn_gr_pick[best] = boot_nvn_gr_pass;
            }
            for (uint32_t k = sp->first; k < sp->last; ++k) {
                const uint32_t q = boot_nvn_gr_ord[k]; const BootNvnGrEnt *e = &boot_nvn_gr_ent[q];
                if (best >= 0) { boot_nvn_gr_reg[q] = best; boot_nvn_gr_off[q] = e->so - boot_gr_view[best].lo; continue; }
                int idx = boot_render_region(r, e->blk, e->blk_size);   /* fallback: the whole block, as with TOTK_GPU_RESIDENT=0 */
                if (idx < 0) { boot_draw_log("nvn native: region (gpu-resident fallback) %s %llu %llu", r->error, (uint64_t)(uintptr_t)e->blk, e->blk_size); return 0; }
                boot_nvn_gr_reg[q] = idx; boot_nvn_gr_off[q] = e->so - (uint64_t)(e->blk - sec->host); ++boot_gr_st.fallbacks;
            }
        }
        boot_nvn_gr_hash_r = r; boot_nvn_gr_hash_n = n;
        boot_nvn_gr_hash = UINT64_C(1469598103934665603) ^ n ^ (boot_gr_vgen << 20) ^ (boot_gr_granule() << 1);   /* views opened above changed the generation */
        for (unsigned k = 0; k < n; ++k) {
            const BootNvnGrEnt *e = &boot_nvn_gr_ent[k];
            boot_nvn_gr_hash = (boot_nvn_gr_hash ^ e->so) * UINT64_C(1099511628211); boot_nvn_gr_hash = (boot_nvn_gr_hash ^ e->len) * UINT64_C(1099511628211);
            boot_nvn_gr_hash = (boot_nvn_gr_hash ^ boot_nvn_gr_sec[e->sec].gen) * UINT64_C(1099511628211);
        }
        if (raced) boot_nvn_gr_hash_n = UINT32_MAX;   /* never taken: the next submission copies the sections and assigns again */
    }
    for (unsigned k = 0; k < n; ++k) {
        const BootNvnGrEnt *e = &boot_nvn_gr_ent[k]; BootNvnMap *m = &maps[e->map];
        m->region = boot_nvn_gr_reg[k]; m->off = boot_nvn_gr_off[k]; boot_render_region_used[m->region] = 1;
        *sig = (*sig ^ m->gpu) * UINT64_C(1099511628211); *sig = (*sig ^ m->off) * UINT64_C(1099511628211);
        *sig = (*sig ^ m->size) * UINT64_C(1099511628211); *sig = (*sig ^ (uint64_t)m->region) * UINT64_C(1099511628211);
    }
    return 1;
}
/* After a page-table rebuild (the queue was waited for, or nothing was ever submitted): releases views and whole-block regions of
 * sections that no mapping uses, and takes the numbers of the perf3 part. */
static inline void boot_nvn_gr_rebuilt(BootRender *r, const BootNvnMap *maps, unsigned count, int mode) {
    ++boot_gr_st.rebuilds; boot_gr_st.mode = (uint32_t)mode;
    boot_gr_st.budget = BOOT_NVN_GR_BUDGET; boot_gr_st.mappings = count; boot_gr_st.deferred = boot_nvn_gr_n;
    if (!boot_nvn_gr_n) boot_nvn_gr_nspan = 0;   /* no mapping in a section: no spans (the last build is of other mappings) */
    if (mode == 1) {
        int quiet = 0;
        for (unsigned j = 0; j < r->region_count; ++j) {
            if (boot_render_region_used[j] || !r->region_cpu[j]) continue;
            const BootGrView *v = &boot_gr_view[j];
            if (!(v->cpu && v->r == r && r->region_cpu[j] == v->cpu) && !boot_nvn_gr_is_host(r->region_cpu[j])) continue;
            if (!quiet) { if (!boot_render_gpu_quiet(r)) return; quiet = 1; }   /* normally idle already: the rebuild waited */
            boot_gr_slot_release(r, j);
        }
    } else {
        boot_nvn_gr_build();   /* dry run: what the views would open */
    }
    /* numbers: what is opened (D3D12 locks the whole allocation of a whole-block region), what that locks (overlaps once), what the
     * mappings need, and what opening their blocks whole locks */
    static uint64_t seen[BOOT_RENDER_MAX_REGIONS]; unsigned nseen = 0;
    uint32_t views = 0, wholes = 0; uint64_t vb = 0, wb = 0, res = 0, today = 0, stale_b = 0, mapped = 0, would = 0;
    static struct { uint64_t gen, a, b; } iv[BOOT_RENDER_MAX_REGIONS]; unsigned niv = 0;
    for (unsigned j = 0; j < r->region_count; ++j) {
        if (!r->region[j]) continue;
        const BootGrView *v = &boot_gr_view[j];
        if (v->cpu && v->r == r && r->region_cpu[j] == v->cpu) {
            ++views; vb += v->size; if (v->stale) stale_b += v->size;
            iv[niv].gen = v->gen; iv[niv].a = v->lo; iv[niv].b = v->lo + v->size; ++niv;
            continue;
        }
        int k = boot_nvn_gr_find(r->region_cpu[j], 1);
        uint64_t sz = k >= 0 && boot_nvn_gr_sec[k].host == r->region_cpu[j] ? boot_nvn_gr_sec[k].size : r->region_size[j];
        ++wholes; wb += sz;
        if (k >= 0 && boot_nvn_gr_sec[k].host == r->region_cpu[j]) { iv[niv].gen = boot_nvn_gr_sec[k].gen; iv[niv].a = 0; iv[niv].b = sz; ++niv; }
        else res += sz;
    }
    for (unsigned i = 1; i < niv; ++i) {   /* by (section, start) */
        __typeof__(iv[0]) t = iv[i]; unsigned q = i;
        while (q && (iv[q - 1].gen > t.gen || (iv[q - 1].gen == t.gen && iv[q - 1].a > t.a))) { iv[q] = iv[q - 1]; --q; }
        iv[q] = t;
    }
    for (unsigned i = 0; i < niv;) {
        uint64_t a = iv[i].a, b = iv[i].b; const uint64_t gen = iv[i].gen; ++i;
        while (i < niv && iv[i].gen == gen && iv[i].a <= b) { if (iv[i].b > b) b = iv[i].b; ++i; }
        res += b - a;
    }
    for (unsigned s = 0; s < boot_nvn_gr_nspan; ++s) {
        const BootNvnGrSpan *sp = &boot_nvn_gr_span[s]; mapped += sp->need;
        const uint64_t room = boot_nvn_gr_headroom(sp->a, sp->b), ssz = boot_nvn_gr_sec[sp->sec].size;
        would += (sp->b + room < ssz ? sp->b + room : ssz) - (sp->a > room ? (sp->a - room) & ~UINT64_C(0xffff) : 0);
        unsigned q = 0; while (q < nseen && seen[q] != boot_nvn_gr_sec[sp->sec].gen) ++q;
        if (q == nseen && nseen < BOOT_RENDER_MAX_REGIONS) { seen[nseen++] = boot_nvn_gr_sec[sp->sec].gen; today += ssz; }
    }
    boot_gr_st.sections = nseen;
    uint8_t other[BOOT_RENDER_MAX_REGIONS]; memset(other, 0, sizeof other);   /* 1: a region of another block (opened whole either way); 2: counted */
    for (unsigned j = 0; j < r->region_count; ++j) {
        const BootGrView *v = &boot_gr_view[j];
        other[j] = r->region[j] && !(v->cpu && v->r == r && r->region_cpu[j] == v->cpu) && !boot_nvn_gr_is_host(r->region_cpu[j]);
    }
    for (unsigned i = 0; i < count; ++i) {
        const int j = maps[i].region;
        if (j < 0 || (unsigned)j >= r->region_count || !other[j]) continue;
        mapped += maps[i].size;   /* overlaps of these few counted twice */
        if (other[j] == 1) { other[j] = 2; today += r->region_size[j]; }
    }
    boot_gr_st.spans = boot_nvn_gr_nspan; boot_gr_st.views = views; boot_gr_st.wholes = wholes; boot_gr_st.view_bytes = vb; boot_gr_st.whole_bytes = wb;
    boot_gr_st.resident_bytes = res; boot_gr_st.mapped_bytes = mapped; boot_gr_st.today_bytes = today; boot_gr_st.would_bytes = would; boot_gr_st.stale_bytes = stale_b;
    boot_gr_st.granule = boot_nvn_gr_w ? boot_nvn_gr_w : boot_gr_granule();
    if (boot_gr_st.rebuilds <= 8 || boot_gr_st.rebuilds % 200 == 0) {
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) {
            fprintf(lf, "gpu-resident: rebuild %llu (mode %d, granule %llu KiB): %u mappings, %u in sections, %u spans; per section (host, MiB, mapped MiB, spans):",
                    (unsigned long long)boot_gr_st.rebuilds, mode, (unsigned long long)(boot_gr_st.granule >> 10), count, boot_nvn_gr_n, boot_nvn_gr_nspan);
            unsigned shown = 0;
            for (unsigned s = 0; s < boot_nvn_gr_nspan && shown < 24;) {
                const uint32_t sec = boot_nvn_gr_span[s].sec; uint64_t need = 0; unsigned ns = 0;
                while (s < boot_nvn_gr_nspan && boot_nvn_gr_span[s].sec == sec) { need += boot_nvn_gr_span[s].need; ++ns; ++s; }
                fprintf(lf, " [%p %llu %.1f %u]", (const void *)boot_nvn_gr_sec[sec].host, (unsigned long long)(boot_nvn_gr_sec[sec].size >> 20), (double)need / 1048576.0, ns); ++shown;
            }
            fputc('\n', lf); fclose(lf);
        }
    }
}
/* suspect-diag (host_gpu_render.h, top), S3: canonical host pointer of each mapping of the last boot_nvn_exec_memory (by its
 * index in maps[]): the guest region's host bytes at the mapping's CPU address, which every view and whole block aliases. */
static const uint8_t **boot_sd_map_host;
static inline void boot_sd_map_note(unsigned i, const uint8_t *host) {
    if (!boot_sd_map_host) boot_sd_map_host = (const uint8_t **)calloc(BOOT_NVN_MAPS, sizeof *boot_sd_map_host);
    if (boot_sd_map_host && i < BOOT_NVN_MAPS) boot_sd_map_host[i] = host;
}
/* Copies n bytes at p into out without touching unmapped memory (another thread may release guest memory meanwhile). */
static inline int boot_sd_safe_read(const void *p, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
static inline uint32_t boot_sd_mix32(uint64_t a, uint64_t b) { uint64_t h = (a * UINT64_C(0x9E3779B97F4A7C15)) ^ (b + UINT64_C(0x632BE59BD9B4E019)); h ^= h >> 29; h *= UINT64_C(0xBF58476D1CE4E5B9); return (uint32_t)(h >> 32); }
/* S3, when due (every TOTK_SUSPECT_DIAG_GR_EVERY presents) and the page table describes maps[0, count) (renderer lock held):
 * sampled pages of the mappings, round-robin from a cursor, within ~0.4 ms. For each: the page-table entry (CPU table and the
 * GPU's copy) against the mapping's region / offset, and the 4 KiB behind the entry (what the GPU reads: region view + offset)
 * against the same guest bytes through the canonical host view; a difference is read again twice before it counts. Then one
 * page of up to 4 open span views against their section's host view. */
static inline void boot_sd_gr_check(BootRender *r, const BootNvnMap *maps, unsigned count) {
    BootSdState *S = boot_sd_state();
    if (!S || !S->gr_due || !boot_sd_map_host || !count || !r->ptable_cpu || !boot_render_pt_gpu) return;
    S->gr_due = 0; ++S->c.gr_checks;
    LARGE_INTEGER q0, qn; QueryPerformanceCounter(&q0);
    const uint64_t limit = S->qpf * 4 / 10000;   /* 0.4 ms */
    static uint8_t a[4096], b[4096];
    unsigned k = 0, pages = 0;
    for (; k < count && pages < S->gr_pages_max; ++k) {
        const unsigned i = (S->gr_cursor + k) % count; const BootNvnMap *m = &maps[i]; const uint8_t *host = boot_sd_map_host[i];
        if (!host || !m->size || m->region < 0) continue;
        if ((k & 7) == 7) { QueryPerformanceCounter(&qn); if ((uint64_t)(qn.QuadPart - q0.QuadPart) > limit) break; }
        const uint64_t np = (m->size + 0xfff) >> 12, x = (uint64_t)(boot_sd_mix32(S->c.gr_checks, i) % np) << 12;
        const uint64_t gpu = m->gpu + x; uint64_t len = m->size - x < 4096 ? m->size - x : 4096;
        if ((gpu >> 12) < r->va_base_page || (gpu >> 12) - r->va_base_page >= r->ptable_pages) { ++S->c.gr_unmapped; continue; }
        const uint64_t page = (gpu >> 12) - r->va_base_page;
        const uint32_t pte = r->ptable_cpu[page], gpte = boot_render_pt_gpu[page];
        const uint32_t want = ((uint32_t)m->region << 24) | (uint32_t)((m->off + x) >> 12);
        ++pages; ++S->c.gr_pages;
        if (pte != gpte) {
            ++S->c.gr_ptcopy;
            if (S->ev_s3 < S->events_max) { ++S->ev_s3; boot_sd_event(S, "S3 PAGE TABLE COPY frame %llu sub %llu: gpu %llx (mapping %u gpu %llx size %llx): CPU entry %08x, GPU copy %08x",
                (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, (unsigned long long)gpu, i, (unsigned long long)m->gpu, (unsigned long long)m->size, pte, gpte); }
        }
        if (pte == 0xffffffffu || (pte >> 24) >= r->region_count || !r->region_cpu[pte >> 24]) {
            ++S->c.gr_unmapped;
            if (S->ev_s3 < S->events_max) { ++S->ev_s3; boot_sd_event(S, "S3 UNMAPPED frame %llu sub %llu: gpu %llx (mapping %u gpu %llx size %llx region %d off %llx): entry %08x, expected %08x",
                (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, (unsigned long long)gpu, i, (unsigned long long)m->gpu, (unsigned long long)m->size,
                m->region, (unsigned long long)m->off, pte, want); }
            continue;
        }
        const unsigned reg = pte >> 24; const uint64_t roff = (uint64_t)(pte & 0xffffff) << 12;
        if (roff >= r->region_size[reg]) { ++S->c.gr_unmapped; continue; }
        if (len > r->region_size[reg] - roff) len = r->region_size[reg] - roff;
        int same = 0, tries = 0, readable = 1;
        for (; tries < 3 && !same; ++tries) {
            if (!boot_sd_safe_read(r->region_cpu[reg] + roff, a, (size_t)len) || !boot_sd_safe_read(host + x, b, (size_t)len)) { readable = 0; break; }
            same = !memcmp(a, b, (size_t)len);
        }
        if (!readable) { ++S->c.gr_unreadable; continue; }
        S->c.gr_bytes += len;
        if (pte != want) { ++S->c.gr_ptother; if (same) ++S->c.gr_ptother_eq; }
        if (same) continue;
        ++S->c.gr_mismatch;
        if (S->ev_s3 < S->events_max) {
            unsigned at = 0; while (at < len && a[at] == b[at]) ++at;
            const BootGrView *v = &boot_gr_view[reg]; const int view = v->cpu && v->cpu == r->region_cpu[reg];
            uint32_t ga = 0, ha = 0; if (at + 4 <= len) { memcpy(&ga, a + at, 4); memcpy(&ha, b + at, 4); }
            ++S->ev_s3;
            boot_sd_event(S, "S3 BYTE MISMATCH frame %llu sub %llu: gpu %llx (mapping %u gpu %llx size %llx, assigned region %d off %llx) entry %08x (expected %08x) -> region %u %s "
                             "(view section offset %llx, size %llx, gen %llu%s) + %llx: first difference at +%u: GPU side %08x, host view %08x (host %p)",
                           (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, (unsigned long long)gpu, i, (unsigned long long)m->gpu, (unsigned long long)m->size,
                           m->region, (unsigned long long)m->off, pte, want, reg, view ? "span view" : "whole block", view ? (unsigned long long)v->lo : 0ull,
                           view ? (unsigned long long)v->size : 0ull, view ? (unsigned long long)v->gen : 0ull, view && v->stale ? ", stale" : "",
                           (unsigned long long)roff, at, ga, ha, (const void *)(host + x));
        }
    }
    S->gr_cursor = (S->gr_cursor + k) % count;
    for (unsigned n = 0, j = 0; j < BOOT_RENDER_MAX_REGIONS && n < 4; ++j) {   /* span views against their section's host view */
        const unsigned s = (S->gr_vcursor + j) % BOOT_RENDER_MAX_REGIONS; const BootGrView *v = &boot_gr_view[s];
        if (!v->cpu || v->stale || v->r != r || s >= r->region_count || r->region_cpu[s] != v->cpu || v->size < 4096) continue;
        ++n;
        unsigned q = 0; while (q < boot_nvn_gr_nsec && boot_nvn_gr_sec[q].gen != v->gen) ++q;
        if (q == boot_nvn_gr_nsec) { ++S->c.gr_nosec; continue; }
        const uint64_t o = (uint64_t)(boot_sd_mix32(S->c.gr_checks, s + 7777u) % (v->size >> 12)) << 12;
        if (v->lo + o + 4096 > boot_nvn_gr_sec[q].size) continue;
        int same = 0, readable = 1;
        for (int tries = 0; tries < 3 && !same; ++tries) {
            if (!boot_sd_safe_read(v->cpu + o, a, 4096) || !boot_sd_safe_read(boot_nvn_gr_sec[q].host + v->lo + o, b, 4096)) { readable = 0; break; }
            same = !memcmp(a, b, 4096);
        }
        if (!readable) { ++S->c.gr_unreadable; continue; }
        ++S->c.gr_views; S->c.gr_bytes += 4096;
        if (same) continue;
        ++S->c.gr_mismatch;
        if (S->ev_s3 < S->events_max) { ++S->ev_s3; boot_sd_event(S, "S3 VIEW MISMATCH frame %llu sub %llu: span view in region %u (section gen %llu offset %llx size %llx) + %llx differs from the section's host view %p",
            (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, s, (unsigned long long)v->gen, (unsigned long long)v->lo, (unsigned long long)v->size,
            (unsigned long long)o, (const void *)(boot_nvn_gr_sec[q].host + v->lo + o)); }
    }
    S->gr_vcursor = (S->gr_vcursor + 5) % BOOT_RENDER_MAX_REGIONS;
    QueryPerformanceCounter(&qn); const uint64_t took = (uint64_t)(qn.QuadPart - q0.QuadPart);
    S->c.gr_qpc += took; if (took > S->c.gr_maxq) S->c.gr_maxq = took;
}
/* S1 at the end of a submit call (host_nvn.h boot_nvn_queue_submit, before the call's pins go): the re-hash under the renderer
 * lock, with the versions kept and the recordings replaced since the previous call's end. */
static inline void boot_sd_call_end(BootContext *c) {
    if (!boot_sd_on()) return;
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native;
    if (!w) return;
    AcquireSRWLockExclusive(&w->lock);
    BootSdState *S = w->render ? boot_sd_state() : NULL;
    if (S) {
        const uint64_t kept = (uint64_t)__atomic_load_n(&boot_nvn_rv.n[BOOT_NVN_RV_KEPT], __ATOMIC_RELAXED);
        const uint64_t re = (uint64_t)__atomic_load_n(&boot_nvn_sub.rerecords, __ATOMIC_RELAXED);
        if (!S->c.calls) { S->last_kept = kept; S->last_rerecords = re; }   /* the first call: counted from here */
        boot_sd_recheck(w->render, kept - S->last_kept, re - S->last_rerecords);
        S->last_kept = kept; S->last_rerecords = re;
    }
    ReleaseSRWLockExclusive(&w->lock);
}
/* data-snapshot (host_gpu_render.h, the design note before host_gpu_render_draw.h): the parts that need the window, the GPU
 * mappings and deferred compute. */
/* Deferred dispatches recorded since the last sync (boot_compute_pending_list only grows between idles): their learnt store ranges
 * join the pending GPU writes (each program once per idle; narrowed to the pages compute has stored to, boot_ds_pend_compute). A
 * program whose ranges overflowed ("wide") is never deferred (boot_compute_can_defer), so it completed before anything later. */
static void boot_ds_compute_sync_nvn(void) {
    if (boot_compute_pending < boot_ds_cp_synced) { boot_ds_cp_synced = 0; boot_ds_cp_nseen = 0; }   /* (the idle resets both) */
    for (unsigned k = boot_ds_cp_synced; k < boot_compute_pending; ++k) {
        const BootComputeWrites *w = boot_compute_pending_list[k];
        unsigned j = 0; while (j < boot_ds_cp_nseen && boot_ds_cp_seen[j] != (const void *)w) ++j;
        if (j < boot_ds_cp_nseen) continue;
        if (boot_ds_cp_nseen < 64) boot_ds_cp_seen[boot_ds_cp_nseen++] = w;
        if (w->wide) { boot_ds_pend_over(&boot_ds_pend, BOOT_DS_P_COMPUTE, BOOT_DS_OVF_WIDE); continue; }
        for (uint32_t i = 0; i < w->count; ++i) boot_ds_pend_compute(w->va[i], w->size[i]);
    }
    boot_ds_cp_synced = boot_compute_pending;
}
/* After a page-table rebuild of the native path (boot_nvn_exec_memory, queue idle): the class of every GPU mapping of the
 * address space from the NVN memory pool holding its CPU address, the executor's block, and the copy arena behind the driver
 * buffer in that block. */
static inline void boot_ds_remapped(BootRender *r, BootWindow *w, uint32_t as_fd, uint8_t *block, uint64_t block_va, uint64_t block_bytes) {
    if (!boot_ds_on()) return;
    boot_ds_compute_sync = boot_ds_compute_sync_nvn; boot_ds_cp_count = &boot_compute_pending;   /* (pend_sync calls it only when that moved) */
    if (boot_ds_map_begin(r)) {
        uint32_t n = 0; BootDsPool *sorted = boot_ds_pools_sorted(&n);
        for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
            const BootGpuMapping *m = &w->gpu_mappings.entries[i];
            if (!m->size || m->as_fd != as_fd || !m->cpu) continue;
            const uint64_t backing = boot_gpu_mapping_backing_size(m);
            if (backing) boot_ds_map_class(m->gpu, m->cpu, backing, sorted, n);
        }
        free(sorted);
    }
    boot_ds_map_done(block, block_va, block_bytes, BOOT_NVN_NATIVE_BLOCK);
}
/* End of a submit call (host_nvn.h boot_nvn_queue_submit): the detector's check under the window lock. */
static inline void boot_ds_call_end(BootContext *c) {
    if (!boot_ds_on()) return;
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native;
    if (!w) return;
    AcquireSRWLockExclusive(&w->lock);
    if (w->render) boot_ds_call_done(w->render);
    ReleaseSRWLockExclusive(&w->lock);
}
static inline int boot_nvn_exec_memory(BootContext *c, BootWindow *w, BootNvnExec *e) {
    const int gr = boot_gr_mode();   /* gpu-resident: the section copy first (it may take the kernel lock, as the pack below) */
    if (gr) { boot_nvn_gr_refresh(c, w); boot_nvn_gr_n = 0; }
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
        e->block = VirtualAlloc(NULL, boot_nvn_block_bytes(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   /* data-snapshot: + the copy arena */
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
        const int gs = gr ? boot_nvn_gr_find(blk, blk_size) : -1;   /* gpu-resident: a section block's region is assigned after the loop */
        int idx = gs >= 0 && gr == 1 ? BOOT_NVN_GR_DEFER : boot_render_region(r, blk, blk_size);
        if (idx < 0 && idx != BOOT_NVN_GR_DEFER) { boot_draw_log("nvn native: region %s %llu %llu", r->error, g->base, g->size); return 0; }
        uint64_t off = (uint64_t)(g->data - blk) + (m->cpu - g->base);
        if (backing > g->size - (m->cpu - g->base)) backing = g->size - (m->cpu - g->base);
        if (count == BOOT_NVN_MAPS) { boot_draw_log("nvn native: too many GPU mappings%s %llu %llu", "", count, 0); return 0; }
        if (gs >= 0) {   /* gpu-resident: its section bytes */
            BootNvnGrEnt *ge = &boot_nvn_gr_ent[boot_nvn_gr_n++];
            ge->so = (uint64_t)(blk - boot_nvn_gr_sec[gs].host) + off; ge->len = backing ? backing : 1; ge->blk = blk; ge->blk_size = blk_size;
            ge->sec = (uint32_t)gs; ge->map = count;
        }
        if (boot_sd_on()) boot_sd_map_note(count, g->data + (m->cpu - g->base));   /* suspect-diag S3: the mapping's canonical host bytes */
        maps[count++] = (BootNvnMap){m->gpu, off, backing, idx};
        if (idx == BOOT_NVN_GR_DEFER) continue;   /* folded into sig once assigned */
        sig = (sig ^ m->gpu) * UINT64_C(1099511628211); sig = (sig ^ off) * UINT64_C(1099511628211);
        sig = (sig ^ backing) * UINT64_C(1099511628211); sig = (sig ^ (uint64_t)idx) * UINT64_C(1099511628211);
    }
    if (gr == 1 && boot_nvn_gr_n && !boot_nvn_gr_assign(r, maps, &sig)) return 0;   /* gpu-resident: views of the GPU-mapped spans */
    int idx = boot_render_region(r, e->block, boot_nvn_block_bytes());
    if (idx < 0) { boot_draw_log("nvn native: native block%s %llu %llu", r->error, 0, 0); return 0; }
    sig = (sig ^ (uint64_t)idx) * UINT64_C(1099511628211);
    if (e->mapped && sig == e->map_sig && r->ptable_cpu) { if (boot_sd_on()) boot_sd_gr_check(r, maps, count); return 1; }   /* suspect-diag S3 */
    if (e->mapped && !boot_render_idle(r)) return 0;   /* earlier submissions read the old table */
    e->mapped = 0; ++e->remaps;
    e->block_va = block_va;
    if (!boot_render_ptable(r, lo & ~UINT64_C(0xfff), e->block_va + boot_nvn_block_bytes())) { boot_draw_log("nvn native: page table %s %llu %llu", r->error, lo, hi); return 0; }
    memset(boot_render_region_used, 0, sizeof boot_render_region_used);
    for (unsigned i = 0; i < count; ++i) {
        boot_render_region_used[maps[i].region] = 1;
        if (!boot_render_map(r, maps[i].gpu, (unsigned)maps[i].region, maps[i].off, maps[i].size)) return 0;
    }
    boot_render_region_used[idx] = 1;
    if (!boot_render_map(r, e->block_va, (unsigned)idx, 0, boot_nvn_block_bytes())) { boot_draw_log("nvn native: native block%s %llu %llu", r->error, 0, 0); return 0; }
    if (gr) boot_nvn_gr_rebuilt(r, maps, count, gr);   /* gpu-resident: views no mapping uses are released (the queue is idle), perf3 numbers */
    e->block_region = idx;
    e->map_sig = sig; e->mapped = 1;
    boot_ds_remapped(r, w, as_fd, e->block, e->block_va, boot_nvn_block_bytes());   /* data-snapshot: pool classes, the copy arena */
    if (boot_sd_on()) boot_sd_gr_check(r, maps, count);   /* suspect-diag S3 */
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
    if (!use || check) boot_ax_live(use ? BOOT_AX_L_DIAG : BOOT_AX_L_DBIND, 8 * (uint64_t)count);   /* async-xlate: on X, not in the call's copy (must stay 0); the late check is a diagnostic */
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
    float vp[16][4]; float depth_range[16][2]; int32_t scissor[16][4]; uint32_t origin_upper_left, depth_mode;
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
/* batch-3 N10: a viewport coordinate (float: SetViewports keeps fractions) as the nearest integer for the integer copies;
 * exact for the integral values SetViewport and TOTK_VIEWPORT_FLOAT=0 store. */
static inline uint32_t boot_nvn_vp_int(float f) { return (uint32_t)(int32_t)(f < 0 ? f - 0.5f : f + 0.5f); }
static inline void boot_nvn_st_viewports(const BootNvnExec *e, BootDrawState *s) {
    /* viewports, scissors (NVN window origin upper left: window y grows down, scale_y negative) */
    for (unsigned j = 0; j < 16; ++j) {
        BootViewport *v = &s->viewport[j];
        float x = (float)e->vp[j][0], y = (float)e->vp[j][1], w = (float)e->vp[j][2], h = (float)e->vp[j][3];
        float n = e->depth_range[j][0], f = e->depth_range[j][1];
        v->scale[0] = w / 2; v->offset[0] = x + w / 2;
        v->scale[1] = e->origin_upper_left ? -h / 2 : h / 2; v->offset[1] = y + h / 2;
        if (e->depth_mode) { v->scale[2] = f - n; v->offset[2] = n; } else { v->scale[2] = (f - n) / 2; v->offset[2] = (n + f) / 2; }
        v->x = boot_nvn_vp_int(e->vp[j][0]); v->y = boot_nvn_vp_int(e->vp[j][1]); v->w = boot_nvn_vp_int(e->vp[j][2]); v->h = boot_nvn_vp_int(e->vp[j][3]);
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
        int known = boot_nvn_vertex_format(wv >> 21, &size, &type);
        boot_nvn_vertex_format_note(wv >> 21, i, known);   /* batch-3 K3 diagnostic */
        if (!known) { a->inactive = 1; continue; }
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
    boot_ds_bind_of = cbufs; ++boot_ds_bind_gen;   /* data-snapshot v2fix: the plan re-reads the rows only after a rebuild (boot_ds_cb_plan) */
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
/* batch-3 N10: nvnCommandBufferSetViewports takes float rectangles. The hardware viewport transform is float and so is
 * D3D12's (boot_render_viewport_map, from the float scale and offset of boot_nvn_st_viewports), so they are kept as
 * given and only the integer copies (BootViewport x/y/w/h) are rounded. Before, (int32_t) truncated them: a size such as
 * 959.9999 from float maths lost a pixel. nxport (MIT, crates/engine/src/nvn/cmd.rs:518-536) rounds them.
 * TOTK_VIEWPORT_FLOAT=0: truncated, as before. */
static inline void boot_nvn_viewport_store(BootNvnExec *e, uint32_t j, const float f[4]) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_VIEWPORT_FLOAT", v, sizeof v); on = !(n && n < sizeof v && v[0] == '0'); }
    int frac = 0;
    for (unsigned k = 0; k < 4; ++k) { float t = (float)(int32_t)f[k]; frac |= f[k] != t; e->vp[j][k] = on ? f[k] : t; }
    if (frac && boot_render_state_diag_on()) {
        static unsigned logged; ++boot_state_diag.viewport_fractional;
        if (logged < 8) { ++logged; boot_gp_log("nvn native: N10 fractional viewport %u: %.4f %.4f %.4f %.4f (%s)", j, f[0], f[1], f[2], f[3], on ? "kept" : "truncated, TOTK_VIEWPORT_FLOAT=0"); }
    }
}
/* batch-3 N6 (host_nvn.h boot_nvn_observe_flush_texels): TOTK_FLUSH_TEXELS=0 leaves nvnTextureFlushTexels to the driver,
 * unobserved, as before. */
static inline int boot_nvn_flush_texels_on(void) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_FLUSH_TEXELS", v, sizeof v); on = !(n && n < sizeof v && v[0] == '0'); }
    return on;
}
/* batch-3 N4/G7 and N11/G9 diagnostics (TOTK_STATE_DIAG; log only), per draw after its state build (host_nvn_play.h
 * boot_nvn_exec_draw).
 * N4/G7: the state applies the front stencil ops, ref and masks to both faces (boot_nvn_st_fixed reads ds[4..5] and
 * stencil_*[0]; boot_render_pso_fixed99 sets BackFace = FrontFace). The driver's depth-stencil state holds the back ops
 * in ds[6..7], and SetStencilRef / Mask / ValueMask store a back value ([1]); nxport (MIT) applies both faces
 * (crates/engine/src/metal/translated.rs:2663-2690). Counted: stencil draws whose back state differs, and of those the
 * ones that draw back faces (no back-face culling), where the difference shows. The first 16 of those are logged.
 * N11/G9: polygon offset goes to D3D12 as DepthBias = (INT)units for every primitive (boot_render_pso_fixed99); NVN
 * offsets only filled polygons (OFFSET_FILL) with float units (nxport: translated.rs:5609-5626). Each distinct
 * (units, factor, clamp, polygon word, draw primitive) is logged once, 64 at most. */
typedef struct BootNvnBiasSeen { float units, factor, clamp; uint32_t polygon, mode; } BootNvnBiasSeen;
static BootNvnBiasSeen boot_nvn_bias_seen[64]; static unsigned boot_nvn_bias_seen_n;
static inline void boot_nvn_state_diag_draw(const BootNvnExec *e, const BootDrawState *s, uint32_t mode) {
    if (!boot_render_state_diag_on()) return;
    BootStateDiag *g = &boot_state_diag;
    if ((e->ds[0] & 4) && s->zt.address) {
        ++g->stencil_draws;
        int ops = e->ds[6] != e->ds[4] || e->ds[7] != e->ds[5];
        int vals = e->stencil_ref[1] != e->stencil_ref[0] || e->stencil_mask[1] != e->stencil_mask[0] || e->stencil_vmask[1] != e->stencil_vmask[0];
        g->stencil_back_ops += (uint64_t)ops; g->stencil_back_values += (uint64_t)vals;
        if ((ops || vals) && !(s->cull_enable && s->cull_face == 0x405)) {   /* 0x405: back faces culled */
            static unsigned logged; ++g->stencil_back_drawn;
            if (logged < 16) {
                ++logged;
                boot_gp_log("nvn native: N4 two-sided stencil (back faces drawn): ds %02x %02x %02x %02x %02x %02x %02x %02x, ref %x/%x, write mask %x/%x, "
                            "value mask %x/%x (front/back), polygon %x, depth target format %x, draw primitive %u, program %llx (draw %llu)",
                            e->ds[0], e->ds[1], e->ds[2], e->ds[3], e->ds[4], e->ds[5], e->ds[6], e->ds[7], e->stencil_ref[0], e->stencil_ref[1],
                            e->stencil_mask[0], e->stencil_mask[1], e->stencil_vmask[0], e->stencil_vmask[1], e->polygon, s->zt.format, mode,
                            (unsigned long long)e->prog.program, (unsigned long long)e->draws);
            }
        }
    }
    if (s->depth_bias_fill) {
        ++g->bias_draws;
        int lines_points = mode <= 3 || mode == 10 || mode == 11;   /* NVN POINTS, LINES, LINE_LOOP, LINE_STRIP, LINES(_STRIP)_ADJACENCY */
        float u = e->poly_offset[1];
        int frac = u > -1e9f && u < 1e9f && u != (float)(int32_t)u;
        g->bias_not_triangles += (uint64_t)lines_points; g->bias_fractional += (uint64_t)frac;
        BootNvnBiasSeen b = {u, e->poly_offset[0], e->poly_offset[2], e->polygon, mode};
        unsigned i = 0;
        while (i < boot_nvn_bias_seen_n && memcmp(&boot_nvn_bias_seen[i], &b, sizeof b)) ++i;
        if (i == boot_nvn_bias_seen_n && i < 64) {
            boot_nvn_bias_seen[boot_nvn_bias_seen_n++] = b;
            boot_gp_log("nvn native: N11 depth bias %u: units %g factor %g clamp %g, polygon %x, draw primitive %u%s%s (D3D12 DepthBias %d)", i,
                        (double)b.units, (double)b.factor, (double)b.clamp, b.polygon, mode, lines_points ? ", points/lines (NVN offsets filled polygons only)" : "",
                        frac ? ", fractional units" : "", (int)s->depth_bias);
        }
    }
}
/* batch-3: every present (host_nvn_play.h boot_nvn_exec_present): the N5 compare of the registrations queued before the
 * last present (host_nvn.h), the K1 scan budget, and every 300 presents one draw-path.log line with the non-zero
 * counters of host_gpu_render_draw.h BootStateDiag (last 300, total). */
static inline void boot_nvn_tic_diag_check(BootWindow *w);
static const char *const boot_state_diag_names[] = {"restart_list_draws", "restart_scanned", "restart_hits", "restart_unscanned", "int_blend_off",
    "vfmt_new", "vfmt_unsupported", "stencil_draws", "stencil_back_ops", "stencil_back_values", "stencil_back_drawn", "bias_draws",
    "bias_not_triangles", "bias_fractional", "viewport_fractional", "flush_texels", "flush_pushed", "tic_checked", "tic_same", "tic_source_bad",
    "tic_compose", "tic_neither", "tic_replace", "tic_other_bad", "tic_unreadable", "tic_dropped"};
_Static_assert(sizeof boot_state_diag_names / sizeof boot_state_diag_names[0] == sizeof(BootStateDiag) / sizeof(uint64_t), "one name per counter");
static inline void boot_nvn_state_diag_present(BootWindow *w) {
    if (!boot_render_state_diag_on()) return;
    boot_nvn_tic_diag_check(w);
    boot_render_restart_budget = 0;
    boot_render_vb_budget = 0;   /* vbounds diagnostic budget */
    static uint32_t presents; static BootStateDiag last;
    if (++presents % 300) return;
    char line[2048]; size_t n = 0; line[0] = 0;
    const uint64_t *now = (const uint64_t *)&boot_state_diag, *was = (const uint64_t *)&last;
    for (size_t i = 0; i < sizeof boot_state_diag / sizeof(uint64_t); ++i) {
        if (!now[i]) continue;
        int k = snprintf(line + n, sizeof line - n, " %s %llu (%llu)", boot_state_diag_names[i], (unsigned long long)(now[i] - was[i]), (unsigned long long)now[i]);
        if (k < 0 || (size_t)k >= sizeof line - n) break;
        n += (size_t)k;
    }
    boot_gp_log("nvn native: batch-3 state diag after %u presents, last 300 (total):%s", presents, n ? line : " nothing counted");
    last = boot_state_diag;
}

#endif
