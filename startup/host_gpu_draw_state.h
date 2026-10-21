#ifndef TOTK_HOST_GPU_DRAW_STATE_H
#define TOTK_HOST_GPU_DRAW_STATE_H
/* Maxwell 3D register file (index = method / 4) -> host description of a draw.
 *
 * Pure decoding: no memory access, no D3D12. Register offsets and fields are
 * NVIDIA's NVB197 definitions (gpu-reference/clb197.h). Bindings that the
 * hardware latches at method time (constant buffers) are passed in separately.
 * Anything this layer cannot represent sets `unsupported` with the register,
 * so the draw stops instead of rendering something different. */
#include <stdint.h>
#include <string.h>

#define R3D(m) (regs[(m) / 4])
static inline float boot_f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

typedef struct BootColorTarget {
    uint64_t address; uint32_t width, height, format, log_gob_w, log_gob_h, log_gob_d, pitch_layout;
    uint32_t layers, array_pitch, layer, write_mask;
} BootColorTarget;
typedef struct BootDepthTarget {
    uint64_t address; uint32_t enabled, width, height, format, log_gob_h, log_gob_d, layers, array_pitch;
} BootDepthTarget;
typedef struct BootViewport { float scale[3], offset[3], min_z, max_z; uint32_t x, y, w, h; } BootViewport;
typedef struct BootScissor { uint32_t enabled, x0, x1, y0, y1; } BootScissor;
typedef struct BootVertexStream { uint64_t address, limit; uint32_t stride, enabled, divisor, per_instance; } BootVertexStream;
typedef struct BootVertexAttrib { uint32_t stream, inactive, offset, size, type, bgra; } BootVertexAttrib;
typedef struct BootCbufBinding { uint64_t address; uint32_t size, valid; } BootCbufBinding;
typedef struct BootBlend {
    uint32_t enabled, separate_alpha, color_op, color_src, color_dst, alpha_op, alpha_src, alpha_dst;
} BootBlend;

typedef struct BootDrawState {
    uint32_t topology, instance_mode, indexed, index_size, first, count, base_vertex, base_instance;
    uint64_t index_address, index_limit;
    uint32_t rt_count, rt_map[8];
    BootColorTarget rt[8];
    BootDepthTarget zt;
    BootViewport viewport[16];
    BootScissor scissor[16];
    uint64_t program_base; uint32_t stage_enabled[6], stage_type[6], stage_offset[6];
    uint64_t tic_address, tsc_address; uint32_t tic_max, tsc_max, sampler_via_header;
    BootVertexStream stream[16];
    BootVertexAttrib attrib[32];
    uint32_t depth_test, depth_write, depth_func, stencil_test, cull_enable, front_face, cull_face;
    uint32_t window_origin_lower_left, flip_y, blend_per_target;
    BootBlend blend[8];
    float blend_const[4];
    uint32_t z_clip_zero_to_w, alpha_test, depth_bias_fill, primitive_restart, restart_index;
    float depth_bias, depth_bias_slope, depth_bias_clamp;
    uint32_t stencil_func, stencil_ref, stencil_func_mask, stencil_mask, stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t base_vertex_reg, base_instance_reg, single_ct_write, srgb_write;
    uint32_t depth_clamp;      /* nvnCommandBufferSetDepthClamp: clamp instead of clipping at near/far (native path) */
    uint32_t unsupported;      /* first register that cannot be represented, 0 if none */
} BootDrawState;

/* Decodes everything a draw reads from registers. `begin` is the BEGIN value. */
static inline void boot_draw_state_decode(const uint32_t *regs, uint32_t begin, BootDrawState *s) {
    memset(s, 0, sizeof *s);
    s->topology = begin & 0xffff;
    s->instance_mode = (begin >> 26) & 3;
    if ((begin >> 24) & 1) s->unsupported = 0x1618;            /* primitive ID load */
    /* render targets */
    uint32_t sel = R3D(0x121c);
    s->rt_count = sel & 15;
    for (unsigned i = 0; i < 8; ++i) s->rt_map[i] = (sel >> (4 + 3 * i)) & 7;
    for (unsigned j = 0; j < 8; ++j) {
        BootColorTarget *t = &s->rt[j]; uint32_t b = 0x800 + j * 64;
        t->address = ((uint64_t)(R3D(b) & 0xff) << 32) | R3D(b + 4);
        t->width = R3D(b + 8) & 0xfffffff; t->height = R3D(b + 12) & 0x1ffff; t->format = R3D(b + 16) & 0xff;
        uint32_t mem = R3D(b + 20);
        t->log_gob_w = mem & 15; t->log_gob_h = (mem >> 4) & 15; t->log_gob_d = (mem >> 8) & 15;
        t->pitch_layout = (mem >> 12) & 1;
        t->layers = R3D(b + 24) & 0xfffffff; t->array_pitch = R3D(b + 28); t->layer = R3D(b + 32);
        uint32_t wm = R3D(0x1a00 + 4 * j);
        t->write_mask = (wm & 1) | ((wm >> 3) & 2) | ((wm >> 6) & 4) | ((wm >> 9) & 8);
    }
    /* depth target */
    s->zt.enabled = R3D(0x1538) & 1;
    s->zt.address = ((uint64_t)(R3D(0xfe0) & 0xff) << 32) | R3D(0xfe4);
    s->zt.format = R3D(0xfe8) & 0xff;
    s->zt.log_gob_h = (R3D(0xfec) >> 4) & 15; s->zt.log_gob_d = (R3D(0xfec) >> 8) & 15;
    s->zt.array_pitch = R3D(0xff0);
    s->zt.width = R3D(0x1228); s->zt.height = R3D(0x122c); s->zt.layers = R3D(0x1230) & 0xffff;
    /* viewports and scissors */
    for (unsigned j = 0; j < 16; ++j) {
        BootViewport *v = &s->viewport[j]; uint32_t b = 0xa00 + j * 32;
        for (unsigned k = 0; k < 3; ++k) { v->scale[k] = boot_f32(R3D(b + 4 * k)); v->offset[k] = boot_f32(R3D(b + 12 + 4 * k)); }
        uint32_t h = R3D(0xc00 + j * 16), vv = R3D(0xc04 + j * 16);
        v->x = h & 0xffff; v->w = h >> 16; v->y = vv & 0xffff; v->h = vv >> 16;
        v->min_z = boot_f32(R3D(0xc08 + j * 16)); v->max_z = boot_f32(R3D(0xc0c + j * 16));
        BootScissor *c = &s->scissor[j];
        c->enabled = R3D(0xe00 + j * 16) & 1;
        uint32_t sh = R3D(0xe04 + j * 16), sv = R3D(0xe08 + j * 16);
        c->x0 = sh & 0xffff; c->x1 = sh >> 16; c->y0 = sv & 0xffff; c->y1 = sv >> 16;
    }
    /* shaders: program region + per-pipeline-slot enable/type/offset */
    s->program_base = ((uint64_t)(R3D(0x1608) & 0xff) << 32) | R3D(0x160c);
    for (unsigned j = 0; j < 6; ++j) {
        uint32_t sh = R3D(0x2000 + j * 64);
        s->stage_enabled[j] = sh & 1; s->stage_type[j] = (sh >> 4) & 15; s->stage_offset[j] = R3D(0x2004 + j * 64);
    }
    s->stage_enabled[1] = 1;   /* VERTEX_B ignores its enable bit: hardware always runs it (yuzu IsShaderConfigEnabled) */
    /* texture/sampler pools */
    s->tsc_address = ((uint64_t)(R3D(0x155c) & 0xff) << 32) | R3D(0x1560); s->tsc_max = R3D(0x1564) & 0x3fffff;
    s->tic_address = ((uint64_t)(R3D(0x1574) & 0xff) << 32) | R3D(0x1578); s->tic_max = R3D(0x157c) & 0x3fffff;
    s->sampler_via_header = R3D(0x1234) & 1;
    /* vertex input */
    for (unsigned j = 0; j < 16; ++j) {
        BootVertexStream *v = &s->stream[j]; uint32_t f = R3D(0x1c00 + j * 16);
        v->stride = f & 0xfff; v->enabled = (f >> 12) & 1;
        v->address = ((uint64_t)(R3D(0x1c04 + j * 16) & 0xff) << 32) | R3D(0x1c08 + j * 16);
        v->divisor = R3D(0x1c0c + j * 16);
        v->per_instance = R3D(0x1880 + j * 4) & 1;   /* SET_VERTEX_STREAM_INSTANCE_A: the frequency (divisor) applies only then */
        v->limit = ((uint64_t)(R3D(0x1f00 + j * 8) & 0xff) << 32) | R3D(0x1f04 + j * 8);
    }
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t a = R3D(0x1160 + 4 * i); BootVertexAttrib *v = &s->attrib[i];
        v->stream = a & 31; v->inactive = (a >> 6) & 1; v->offset = (a >> 7) & 0x3fff;
        v->size = (a >> 21) & 63; v->type = (a >> 27) & 7; v->bgra = a >> 31;
    }
    /* index buffer */
    s->index_address = ((uint64_t)(R3D(0x17c8) & 0xff) << 32) | R3D(0x17cc);
    s->index_limit = ((uint64_t)(R3D(0x17d0) & 0xff) << 32) | R3D(0x17d4);
    s->index_size = R3D(0x17d8) & 3;
    /* fixed-function state */
    s->depth_test = R3D(0x12cc) & 1; s->depth_write = R3D(0x12e8) & 1; s->depth_func = R3D(0x130c);
    s->stencil_test = R3D(0x1380) & 1;
    s->cull_enable = R3D(0x1918) & 1; s->front_face = R3D(0x191c); s->cull_face = R3D(0x1920);
    s->window_origin_lower_left = R3D(0x13ac) & 1; s->flip_y = (R3D(0x13ac) >> 4) & 1;
    s->blend_per_target = R3D(0x12e4) & 1;
    for (unsigned j = 0; j < 8; ++j) {
        BootBlend *b = &s->blend[j];
        b->enabled = R3D(0x1360 + 4 * j) & 1;
        if (s->blend_per_target) {
            uint32_t base = 0x1e00 + j * 32;
            b->separate_alpha = R3D(base) & 1; b->color_op = R3D(base + 4); b->color_src = R3D(base + 8);
            b->color_dst = R3D(base + 12); b->alpha_op = R3D(base + 16); b->alpha_src = R3D(base + 20); b->alpha_dst = R3D(base + 24);
        } else {
            b->separate_alpha = R3D(0x133c) & 1; b->color_op = R3D(0x1340); b->color_src = R3D(0x1344);
            b->color_dst = R3D(0x1348); b->alpha_op = R3D(0x134c); b->alpha_src = R3D(0x1350); b->alpha_dst = R3D(0x1358);
        }
    }
    for (unsigned k = 0; k < 4; ++k) s->blend_const[k] = boot_f32(R3D(0x131c + 4 * k));
    s->z_clip_zero_to_w = R3D(0xd7c) & 1;
    s->alpha_test = R3D(0x12ec) & 1;
    s->depth_bias_fill = R3D(0xdc8) & 1; s->depth_bias = boot_f32(R3D(0x15bc));
    s->depth_bias_slope = boot_f32(R3D(0x156c)); s->depth_bias_clamp = boot_f32(R3D(0x187c));
    s->srgb_write = R3D(0x15b8) & 1;
    s->primitive_restart = R3D(0x1644) & 1; s->restart_index = R3D(0x1648);
    s->stencil_fail = R3D(0x1384); s->stencil_zfail = R3D(0x1388); s->stencil_zpass = R3D(0x138c);
    s->stencil_func = R3D(0x1390); s->stencil_ref = R3D(0x1394); s->stencil_func_mask = R3D(0x1398); s->stencil_mask = R3D(0x139c);
    s->base_vertex_reg = R3D(0x1434); s->base_instance_reg = R3D(0x1438);
    s->single_ct_write = R3D(0xf90) & 1;
    /* Stages beyond vertex B + pixel are not translated yet (tessellation/geometry). */
    for (unsigned j = 2; j < 5; ++j) if (s->stage_enabled[j] && !s->unsupported) s->unsupported = 0x2000 + j * 64;
    if (s->alpha_test && !s->unsupported) s->unsupported = 0x12ec;   /* legacy fixed-function alpha test */
}
#undef R3D
#endif
