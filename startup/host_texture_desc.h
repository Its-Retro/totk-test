#ifndef TOTK_HOST_TEXTURE_DESC_H
#define TOTK_HOST_TEXTURE_DESC_H
/* Maxwell texture (TIC) and sampler (TSC) descriptors -> host description.
 *
 * Field positions are NVIDIA's documented TEXHEAD_BL / TEXSAMP layout
 * (open-gpu-doc classes/3d/clb097tex.h). Mip/layer layout follows
 * texture_layout_model.py, which matches Nintendo's own BNTX mip offsets and
 * sizes for 2,999 of 3,000 sampled game textures (the remaining one is 3D and
 * needs the header's block depth). Unknown formats or layouts return 0 so the
 * caller stops instead of guessing. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <dxgiformat.h>

static inline uint32_t boot_bits256(const uint32_t w[8], unsigned lo, unsigned n) {
    uint64_t v = ((uint64_t)w[lo / 32] | ((lo / 32 + 1 < 8) ? (uint64_t)w[lo / 32 + 1] << 32 : 0)) >> (lo % 32);
    return (uint32_t)(v & ((n == 32) ? 0xffffffffu : ((1u << n) - 1u)));
}

enum { BOOT_TEX_1D = 0, BOOT_TEX_2D = 1, BOOT_TEX_3D = 2, BOOT_TEX_CUBE = 3, BOOT_TEX_1D_ARRAY = 4,
       BOOT_TEX_2D_ARRAY = 5, BOOT_TEX_1D_BUFFER = 6, BOOT_TEX_2D_NO_MIPMAP = 7, BOOT_TEX_CUBE_ARRAY = 8 };
enum { BOOT_TEX_DECODE_NONE = 0, BOOT_TEX_DECODE_ASTC = 1 };

typedef struct BootTic {
    uint64_t address;
    uint32_t components, data_type[4], source[4];
    uint32_t header_version, log_gob_h, log_gob_d, tile_width, texture_type;
    uint32_t width, height, depth, max_mip, srgb, normalized, depth_texture;
    uint32_t view_min_mip, view_max_mip;
    uint32_t pitch;   /* pitch-linear headers (versions 1/2): bytes per row, 32-byte aligned; 0 = block-linear */
    uint32_t base_layer;   /* first array layer of the view (deko3d TextureImageControl view_layer_base_*) */
} BootTic;

/* Header versions (TEXHEAD_*): 0 1D buffer, 1 pitch + colour key, 2 pitch, 3 block-linear,
 * 4 block-linear + colour key. Returns 0 for anything this decoder cannot place in memory. */
static inline int boot_tic_decode(const uint32_t w[8], BootTic *t) {
    memset(t, 0, sizeof *t);
    t->components = boot_bits256(w, 0, 7);
    for (unsigned c = 0; c < 4; ++c) {
        t->data_type[c] = boot_bits256(w, 7 + 3 * c, 3);
        t->source[c] = boot_bits256(w, 19 + 3 * c, 3);
    }
    t->address = ((uint64_t)boot_bits256(w, 41, 23) << 9) | ((uint64_t)boot_bits256(w, 64, 16) << 32);
    t->header_version = boot_bits256(w, 85, 3);
    if (t->header_version == 0) {
        /* TEXHEAD_1D_BUFFER: byte offset in word 1 (not 512-aligned), 32-bit element count minus one
         * split over word 3 bits 0..15 (high) and word 4 bits 0..15 (low). */
        t->address = (uint64_t)w[1] | ((uint64_t)boot_bits256(w, 64, 16) << 32);
        t->width = ((boot_bits256(w, 96, 16) << 16) | boot_bits256(w, 128, 16)) + 1;
        t->height = t->depth = 1; t->texture_type = boot_bits256(w, 151, 4);
        t->srgb = boot_bits256(w, 150, 1);
        return t->texture_type == BOOT_TEX_1D_BUFFER;
    }
    /* view layer base bits 0-2 / 3-7 / 8-10: deko3d (zlib) source/maxwell/texture_image_control_block.h
     * view_layer_base_0_2 / 3_7 / 8_10; clb097tex.h lists the same bits as RESERVED4A / RESERVED_ADDRESS / RESERVED2A */
    t->base_layer = boot_bits256(w, 144, 3) | (boot_bits256(w, 80, 5) << 3) | (boot_bits256(w, 93, 3) << 8);
    t->log_gob_h = boot_bits256(w, 99, 3);
    t->log_gob_d = boot_bits256(w, 102, 3);
    t->tile_width = boot_bits256(w, 106, 3);
    t->depth_texture = boot_bits256(w, 123, 1);
    t->max_mip = boot_bits256(w, 124, 4);
    t->width = boot_bits256(w, 128, 16) + 1;
    t->srgb = boot_bits256(w, 150, 1);
    t->texture_type = boot_bits256(w, 151, 4);
    t->height = boot_bits256(w, 160, 16) + 1;
    t->depth = boot_bits256(w, 176, 14) + 1;
    t->normalized = boot_bits256(w, 191, 1);
    t->view_min_mip = boot_bits256(w, 224, 4);
    t->view_max_mip = boot_bits256(w, 228, 4);
    /* Pitch-linear (with or without colour key): word 3 bits 0..15 hold pitch >> 5 (clb097tex.h
     * NVB097_TEXHEAD_PITCH_PITCH_BITS20TO5). Block-linear: GOB heights/depths. 1D buffer views are not handled yet. */
    if (t->header_version == 1 || t->header_version == 2) {
        /* pitch addresses are 32-byte aligned (clb097tex.h NVB097_TEXHEAD_PITCH_ADDRESS_BITS31TO5); the whole
         * low word is read, as deko3d keeps it in TextureImageControl::address_low */
        t->address = (uint64_t)w[1] | ((uint64_t)boot_bits256(w, 64, 16) << 32);
        t->pitch = boot_bits256(w, 96, 16) << 5; t->log_gob_h = t->log_gob_d = 0; t->tile_width = 0;
        return t->pitch != 0;
    }
    /* block-linear with or without colour key share the layout fields */
    return (t->header_version == 3 || t->header_version == 4) && t->log_gob_h <= 5 && t->log_gob_d <= 5 && t->tile_width == 0;
}

/* Storage format: block footprint, bytes per block, the DXGI format the host
 * texture uses and whether CPU decoding is needed first. */
typedef struct BootTexFormat {
    uint8_t block_w, block_h, bytes, decode;
    DXGI_FORMAT unorm, srgb;
} BootTexFormat;

static inline int boot_tex_format(uint32_t components, uint32_t data_type, BootTexFormat *f) {
    /* data_type: 1 snorm, 2 unorm, 3 sint, 4 uint, 7 float (red channel's type selects the view) */
#define BF(bw, bh, by, dec, u, s) do { f->block_w = bw; f->block_h = bh; f->bytes = by; f->decode = dec; f->unorm = u; f->srgb = s; return 1; } while (0)
    switch (components) {
    case 0x01: if (data_type == 7) BF(1,1,16,0,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,16,0,DXGI_FORMAT_R32G32B32A32_UINT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 3) BF(1,1,16,0,DXGI_FORMAT_R32G32B32A32_SINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x03: if (data_type == 7) BF(1,1,8,0,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 2) BF(1,1,8,0,DXGI_FORMAT_R16G16B16A16_UNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 1) BF(1,1,8,0,DXGI_FORMAT_R16G16B16A16_SNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,8,0,DXGI_FORMAT_R16G16B16A16_UINT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 3) BF(1,1,8,0,DXGI_FORMAT_R16G16B16A16_SINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x04: if (data_type == 7) BF(1,1,8,0,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,8,0,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x08: if (data_type == 2) BF(1,1,4,0,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
               if (data_type == 1) BF(1,1,4,0,DXGI_FORMAT_R8G8B8A8_SNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,4,0,DXGI_FORMAT_R8G8B8A8_UINT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 3) BF(1,1,4,0,DXGI_FORMAT_R8G8B8A8_SINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x07: if (data_type == 2) BF(1,1,4,0,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); break;
    case 0x09: if (data_type == 2) BF(1,1,4,0,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,4,0,DXGI_FORMAT_R10G10B10A2_UINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x0c: if (data_type == 7) BF(1,1,4,0,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 2) BF(1,1,4,0,DXGI_FORMAT_R16G16_UNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 1) BF(1,1,4,0,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,4,0,DXGI_FORMAT_R16G16_UINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x0f: if (data_type == 7) BF(1,1,4,0,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,4,0,DXGI_FORMAT_R32_UINT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 3) BF(1,1,4,0,DXGI_FORMAT_R32_SINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x10: BF(4,4,16,0,DXGI_FORMAT_BC6H_SF16,DXGI_FORMAT_UNKNOWN);
    case 0x11: BF(4,4,16,0,DXGI_FORMAT_BC6H_UF16,DXGI_FORMAT_UNKNOWN);
    case 0x15: if (data_type == 2) BF(1,1,2,0,DXGI_FORMAT_B5G6R5_UNORM,DXGI_FORMAT_UNKNOWN); break;
    case 0x17: BF(4,4,16,0,DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_BC7_UNORM_SRGB);
    case 0x18: if (data_type == 2) BF(1,1,2,0,DXGI_FORMAT_R8G8_UNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 1) BF(1,1,2,0,DXGI_FORMAT_R8G8_SNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,2,0,DXGI_FORMAT_R8G8_UINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x1b: if (data_type == 7) BF(1,1,2,0,DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_UNKNOWN);
               if (data_type == 2) BF(1,1,2,0,DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 1) BF(1,1,2,0,DXGI_FORMAT_R16_SNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,2,0,DXGI_FORMAT_R16_UINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x1d: if (data_type == 2) BF(1,1,1,0,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 1) BF(1,1,1,0,DXGI_FORMAT_R8_SNORM,DXGI_FORMAT_UNKNOWN);
               if (data_type == 4) BF(1,1,1,0,DXGI_FORMAT_R8_UINT,DXGI_FORMAT_UNKNOWN); break;
    case 0x20: BF(1,1,4,0,DXGI_FORMAT_R9G9B9E5_SHAREDEXP,DXGI_FORMAT_UNKNOWN);
    case 0x21: BF(1,1,4,0,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_UNKNOWN);
    case 0x24: BF(4,4,8,0,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC1_UNORM_SRGB);
    case 0x25: BF(4,4,16,0,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC2_UNORM_SRGB);
    case 0x26: BF(4,4,16,0,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC3_UNORM_SRGB);
    case 0x27: BF(4,4,8,0,data_type == 1 ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_UNKNOWN);
    case 0x28: BF(4,4,16,0,data_type == 1 ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC5_UNORM,DXGI_FORMAT_UNKNOWN);
    case 0x29: BF(1,1,4,0,DXGI_FORMAT_R24_UNORM_X8_TYPELESS,DXGI_FORMAT_UNKNOWN);   /* Z24S8 sampled as depth */
    case 0x2f: BF(1,1,4,0,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_UNKNOWN);               /* ZF32 */
    case 0x3a: BF(1,1,2,0,DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_UNKNOWN);               /* Z16 */
    /* ASTC: decoded to RGBA8 (no PC GPU samples ASTC) */
    case 0x40: BF(4,4,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x41: BF(5,5,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x42: BF(6,6,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x44: BF(8,8,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x45: BF(10,10,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x46: BF(12,12,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x50: BF(5,4,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x51: BF(6,5,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x52: BF(8,6,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x53: BF(10,8,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x54: BF(12,10,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x55: BF(8,5,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x56: BF(10,5,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 0x57: BF(10,6,16,1,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    default: break;
    }
#undef BF
    return 0;
}

/* ---------------------------------------------------------------- block-linear layout */
static inline uint32_t boot_div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static inline unsigned boot_mip_log_gob_h(uint32_t rows, unsigned log_bh) {
    while (log_bh > 0 && boot_div_up(rows, 8) <= (1u << log_bh) / 2) --log_bh;
    return log_bh;
}
static inline unsigned boot_mip_log_gob_d(uint32_t depth, unsigned log_bd) {
    while (log_bd > 0 && depth <= (1u << log_bd) / 2) --log_bd;
    return log_bd;
}
typedef struct BootTexLevel { uint64_t offset, size; uint32_t width, height, depth, width_bytes, rows; unsigned log_gob_h, log_gob_d; } BootTexLevel;
/* Fills levels[0..count) within one layer and returns the layer stride (0 on overflow). */
static inline uint64_t boot_tex_layout(const BootTic *t, const BootTexFormat *f, BootTexLevel *levels, unsigned count) {
    uint64_t off = 0;
    for (unsigned l = 0; l < count; ++l) {
        BootTexLevel *v = &levels[l];
        v->width = t->width >> l ? t->width >> l : 1; v->height = t->height >> l ? t->height >> l : 1;
        uint32_t d = t->texture_type == BOOT_TEX_3D ? t->depth : 1; v->depth = d >> l ? d >> l : 1;
        v->width_bytes = boot_div_up(v->width, f->block_w) * f->bytes; v->rows = boot_div_up(v->height, f->block_h);
        v->log_gob_h = boot_mip_log_gob_h(v->rows, t->log_gob_h);
        /* 3D: a single-level texture keeps the header's GOBS_PER_BLOCK_DEPTH as written (no game data covers
         * this case; Ryujinx SizeCalculator shrinks it there too); with mips every level, the base included,
         * shrinks it to the level's depth (Ryujinx SizeCalculator.GetBlockLinearTextureSize). The one 3D
         * BNTX texture in the game (TexToGo/3DWorleyPerlinNoise_Fi, 64^3 BC4, 7 mips) matches. */
        v->log_gob_d = (t->texture_type == BOOT_TEX_3D && count == 1) ? t->log_gob_d : boot_mip_log_gob_d(v->depth, t->log_gob_d);
        uint64_t gx = boot_div_up(v->width_bytes, 64), gy = boot_div_up(v->rows, 8u << v->log_gob_h), gz = boot_div_up(v->depth, 1u << v->log_gob_d);
        v->offset = off; v->size = (gx * gy * gz * ((uint64_t)512 << v->log_gob_h)) << v->log_gob_d;
        off += v->size;
        if (off > (UINT64_C(1) << 34)) return 0;
    }
    uint64_t layers = t->texture_type == BOOT_TEX_2D_ARRAY || t->texture_type == BOOT_TEX_CUBE || t->texture_type == BOOT_TEX_CUBE_ARRAY ||
                      t->texture_type == BOOT_TEX_1D_ARRAY;
    /* render-28: layers align to the base level's block height after it is shrunk to the image (Ryujinx
     * SizeCalculator.AlignLayerSize: the header's GOB height shrunk to the image height), not to the header's block height. A tiny
     * layered texture with a taller header block (TOTK's per-light 3x3 R8 occlusion cubes, 0xC00 apart = six 0x200
     * faces) otherwise got 8 KiB faces: faces 1-5 were read from the next lights' cubes, so each torch's light range
     * (cube sample x radius) was cut short in most directions - the warm torch light barely left the flame. */
    if (layers) { uint64_t align = (uint64_t)512 << (count ? levels[0].log_gob_h : t->log_gob_h); off = (off + align - 1) / align * align; }
    return off;
}

/* ---------------------------------------------------------------- sampler */
typedef struct BootTsc {
    uint32_t address[3], depth_compare, compare_func, srgb_border, max_aniso;
    uint32_t mag_filter, min_filter, mip_filter, unnormalized;
    float lod_bias, min_lod, max_lod, border[4];
} BootTsc;
static inline void boot_tsc_decode(const uint32_t w[8], BootTsc *s) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < 3; ++i) s->address[i] = (w[0] >> (3 * i)) & 7;
    s->depth_compare = (w[0] >> 9) & 1; s->compare_func = (w[0] >> 10) & 7;
    s->srgb_border = (w[0] >> 13) & 1; s->max_aniso = (w[0] >> 20) & 7;
    s->mag_filter = w[1] & 7; s->min_filter = (w[1] >> 4) & 3; s->mip_filter = (w[1] >> 6) & 3;
    int32_t bias = (int32_t)(w[1] << 7) >> 19;               /* 13-bit signed, 8 fractional bits */
    s->lod_bias = (float)bias / 256.0f;
    s->unnormalized = (w[1] >> 25) & 1;
    s->min_lod = (float)(w[2] & 0xfff) / 256.0f; s->max_lod = (float)((w[2] >> 12) & 0xfff) / 256.0f;
    for (unsigned i = 0; i < 4; ++i) { uint32_t b = w[4 + i]; memcpy(&s->border[i], &b, 4); }
}
#endif
