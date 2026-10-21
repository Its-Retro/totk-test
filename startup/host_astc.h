#ifndef TOTK_HOST_ASTC_H
#define TOTK_HOST_ASTC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ASTC LDR decoder (Khronos Data Format Specification, ASTC chapter), 2D
 * footprints 4x4 .. 12x12, 16-byte block -> RGBA8.  Self-contained, no
 * global mutable state, header-only.
 *
 * Covered: all block modes (incl. dual plane), 1-4 partitions with the
 * spec's hash52/select_partition, LDR color endpoint modes 0,1,4,5,6,8,9,10,
 * 12,13 (bit transfer, blue contraction), bounded integer sequence encoding
 * (bits/trits/quints), color & weight unquantisation, weight-grid infill,
 * LDR void-extent blocks, and every illegal encoding listed by the spec
 * (decoded as the LDR error color, opaque magenta).
 *
 * HDR endpoint modes (2,3,7,11,14,15) and HDR void-extent blocks are NOT
 * decoded: the block is filled with the error color and BOOT_ASTC_E_HDR is
 * returned (an LDR-profile decoder would show the same magenta).
 *
 * Output conversion of the spec's 16-bit interpolation result C:
 *   default                        : C >> 8, the spec's decode_unorm8 rule
 *                                    (endpoints expanded e*257); bit-exact
 *                                    with astcenc ASTCENC_FLG_USE_DECODE_UNORM8.
 *   BOOT_ASTC_FLAG_SRGB            : RGB endpoints expanded (e<<8)|0x80,
 *                                    alpha e*257, output C >> 8; result is
 *                                    sRGB-encoded bytes (matches astcenc's
 *                                    LDR_SRGB profile bit-exactly).
 *   BOOT_ASTC_FLAG_ROUND_NEAREST   : round(C*255/65535), nearest UNORM8 to the
 *                                    spec's float result C/65535 (decode_fp16
 *                                    style; differs from default by <= 1 LSB).
 *                                    Ignored together with BOOT_ASTC_FLAG_SRGB. */

enum {
    BOOT_ASTC_OK = 0,
    BOOT_ASTC_ERROR_BLOCK = 1,  /* illegal encoding: error color written (spec behaviour) */
    BOOT_ASTC_E_ARGS = -1,      /* bad pointer / footprint / size */
    BOOT_ASTC_E_HDR = -2        /* HDR content: not supported, error color written */
};
#define BOOT_ASTC_FLAG_SRGB 1u
#define BOOT_ASTC_FLAG_ROUND_NEAREST 2u

/* ---- bit access on a 128-bit block held as two little-endian u64 ---- */
static inline uint32_t boot_astc_get(const uint64_t w[2], unsigned pos, unsigned n){
    if(!n || pos >= 128) return 0;
    uint64_t v;
    if(pos >= 64) v = w[1] >> (pos - 64);
    else if(pos == 0) v = w[0];
    else v = (w[0] >> pos) | (w[1] << (64 - pos));
    if(n < 64) v &= (UINT64_C(1) << n) - 1;
    return (uint32_t)v;
}
/* Read n bits at pos, bits at or beyond `end` read as zero. */
static inline uint32_t boot_astc_get_lim(const uint64_t w[2], unsigned pos, unsigned n, unsigned end){
    if(pos >= end || !n) return 0;
    if(n > end - pos) n = end - pos;
    return boot_astc_get(w, pos, n);
}
static inline uint64_t boot_astc_rev64(uint64_t x){
    x = ((x >> 1) & UINT64_C(0x5555555555555555)) | ((x & UINT64_C(0x5555555555555555)) << 1);
    x = ((x >> 2) & UINT64_C(0x3333333333333333)) | ((x & UINT64_C(0x3333333333333333)) << 2);
    x = ((x >> 4) & UINT64_C(0x0F0F0F0F0F0F0F0F)) | ((x & UINT64_C(0x0F0F0F0F0F0F0F0F)) << 4);
    x = ((x >> 8) & UINT64_C(0x00FF00FF00FF00FF)) | ((x & UINT64_C(0x00FF00FF00FF00FF)) << 8);
    x = ((x >> 16) & UINT64_C(0x0000FFFF0000FFFF)) | ((x & UINT64_C(0x0000FFFF0000FFFF)) << 16);
    return (x >> 32) | (x << 32);
}

/* ---- integer sequence encoding ----
 * Range index 0..20: 2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256 */
static inline void boot_astc_range_shape(unsigned r, unsigned *bits, unsigned *trits, unsigned *quints){
    static const uint8_t b[21] = {1,0,2,0,1,3,1,2,4,2,3,5,3,4,6,4,5,7,5,6,8};
    static const uint8_t t[21] = {0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0};
    static const uint8_t q[21] = {0,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0};
    if(r > 20) r = 20;
    *bits = b[r]; *trits = t[r]; *quints = q[r];
}
static inline unsigned boot_astc_ise_bits(unsigned count, unsigned r){
    unsigned b, t, q;
    boot_astc_range_shape(r, &b, &t, &q);
    return count * b + (t ? (8 * count + 4) / 5 : 0) + (q ? (7 * count + 2) / 3 : 0);
}
/* Spec trit block decode: 8 bits T -> 5 trits. */
static inline void boot_astc_decode_trits(unsigned T, uint8_t t[5]){
    unsigned C;
    T &= 0xFF;
    if(((T >> 2) & 7) == 7){
        C = (((T >> 5) & 7) << 2) | (T & 3);
        t[4] = 2; t[3] = 2;
    }else{
        C = T & 0x1F;
        if(((T >> 5) & 3) == 3){ t[4] = 2; t[3] = (uint8_t)((T >> 7) & 1); }
        else{ t[4] = (uint8_t)((T >> 7) & 1); t[3] = (uint8_t)((T >> 5) & 3); }
    }
    if((C & 3) == 3){
        t[2] = 2; t[1] = (uint8_t)((C >> 4) & 1);
        t[0] = (uint8_t)((((C >> 3) & 1) << 1) | (((C >> 2) & 1) & (~(C >> 3) & 1)));
    }else if(((C >> 2) & 3) == 3){
        t[2] = 2; t[1] = 2; t[0] = (uint8_t)(C & 3);
    }else{
        t[2] = (uint8_t)((C >> 4) & 1); t[1] = (uint8_t)((C >> 2) & 3);
        t[0] = (uint8_t)((((C >> 1) & 1) << 1) | ((C & 1) & (~(C >> 1) & 1)));
    }
}
/* Spec quint block decode: 7 bits Q -> 3 quints. */
static inline void boot_astc_decode_quints(unsigned Q, uint8_t q[3]){
    Q &= 0x7F;
    if(((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0){
        unsigned n0 = ~Q & 1;
        q[2] = (uint8_t)(((Q & 1) << 2) | ((((Q >> 4) & 1) & n0) << 1) | (((Q >> 3) & 1) & n0));
        q[1] = 4; q[0] = 4;
    }else{
        unsigned C;
        if(((Q >> 1) & 3) == 3){
            q[2] = 4;
            C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1);
        }else{
            q[2] = (uint8_t)((Q >> 5) & 3);
            C = Q & 0x1F;
        }
        if((C & 7) == 5){ q[1] = 4; q[0] = (uint8_t)((C >> 3) & 3); }
        else{ q[1] = (uint8_t)((C >> 3) & 3); q[0] = (uint8_t)(C & 7); }
    }
}
/* Decode `count` (<= 64) integers of range r starting at bit `start`; the
 * sequence occupies exactly ise_bits(count,r) bits, anything beyond reads 0. */
static inline void boot_astc_ise_decode(const uint64_t w[2], unsigned start, unsigned count, unsigned r, uint8_t *out){
    unsigned nb, nt, nq, pos = start, end = start + boot_astc_ise_bits(count, r), i = 0;
    boot_astc_range_shape(r, &nb, &nt, &nq);
    if(nt){
        static const uint8_t tb[5] = {2,2,1,2,1};
        while(i < count){
            unsigned m[5], T = 0, tp = 0;
            uint8_t tv[5];
            for(unsigned k = 0; k < 5; ++k){
                m[k] = boot_astc_get_lim(w, pos, nb, end); pos += nb;
                T |= boot_astc_get_lim(w, pos, tb[k], end) << tp; pos += tb[k]; tp += tb[k];
            }
            boot_astc_decode_trits(T, tv);
            for(unsigned k = 0; k < 5 && i < count; ++k, ++i) out[i] = (uint8_t)((tv[k] << nb) | m[k]);
        }
    }else if(nq){
        static const uint8_t qb[3] = {3,2,2};
        while(i < count){
            unsigned m[3], Q = 0, qp = 0;
            uint8_t qv[3];
            for(unsigned k = 0; k < 3; ++k){
                m[k] = boot_astc_get_lim(w, pos, nb, end); pos += nb;
                Q |= boot_astc_get_lim(w, pos, qb[k], end) << qp; pos += qb[k]; qp += qb[k];
            }
            boot_astc_decode_quints(Q, qv);
            for(unsigned k = 0; k < 3 && i < count; ++k, ++i) out[i] = (uint8_t)((qv[k] << nb) | m[k]);
        }
    }else{
        for(; i < count; ++i, pos += nb) out[i] = (uint8_t)boot_astc_get_lim(w, pos, nb, end);
    }
}

/* ---- unquantisation ---- */
static inline unsigned boot_astc_replicate(unsigned v, unsigned from, unsigned to){
    unsigned r = 0, have = 0;
    if(!from) return 0;
    v &= (1u << from) - 1;
    while(have < to){ r = (r << from) | v; have += from; }
    return r >> (have - to);
}
static inline uint8_t boot_astc_unquant_color(unsigned r, unsigned v){
    unsigned nb, nt, nq;
    boot_astc_range_shape(r, &nb, &nt, &nq);
    if(!nt && !nq) return (uint8_t)boot_astc_replicate(v, nb, 8);
    unsigned m = v & ((1u << nb) - 1), D = v >> nb;
    unsigned A = (m & 1) ? 0x1FF : 0, b = (m >> 1) & 1, c = (m >> 2) & 1, d = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
    unsigned B = 0, C = 0;
    switch(r){
    case 4:  B = 0; C = 204; break;                                   /* 0..5   trit  a      */
    case 6:  B = 0; C = 113; break;                                   /* 0..9   quint a      */
    case 7:  B = b * 0x116; C = 93; break;                            /* b000b0bb0 */
    case 9:  B = b * 0x10C; C = 54; break;                            /* b0000bb00 */
    case 10: B = c * 0x10A + b * 0x085; C = 44; break;                /* cb000cbcb */
    case 12: B = c * 0x105 + b * 0x082; C = 26; break;                /* cb0000cbc */
    case 13: B = d * 0x104 + c * 0x082 + b * 0x041; C = 22; break;    /* dcb000dcb */
    case 15: B = d * 0x102 + c * 0x081 + b * 0x040; C = 13; break;    /* dcb0000dc */
    case 16: B = e * 0x102 + d * 0x081 + c * 0x040 + b * 0x020; C = 11; break; /* edcb000ed */
    case 18: B = e * 0x101 + d * 0x080 + c * 0x040 + b * 0x020; C = 6; break;  /* edcb0000e */
    case 19: B = f * 0x101 + e * 0x080 + d * 0x040 + c * 0x020 + b * 0x010; C = 5; break; /* fedcb000f */
    default: /* ranges 0..2 / 0..4: never legal for color endpoints */
        return (uint8_t)(nt ? (D * 255 / 2) : (D * 255 / 4));
    }
    unsigned T = (D * C + B) ^ A;
    return (uint8_t)((A & 0x80) | (T >> 2));
}
static inline uint8_t boot_astc_unquant_weight(unsigned r, unsigned v){
    unsigned nb, nt, nq, T;
    boot_astc_range_shape(r, &nb, &nt, &nq);
    if(!nt && !nq) T = boot_astc_replicate(v, nb, 6);
    else if(nb == 0){
        static const uint8_t t3[3] = {0,32,63}, q5[5] = {0,16,32,47,63};
        T = nt ? t3[v % 3] : q5[v % 5];
    }else{
        unsigned m = v & ((1u << nb) - 1), D = v >> nb;
        unsigned A = (m & 1) ? 0x7F : 0, b = (m >> 1) & 1, c = (m >> 2) & 1, B = 0, C = 0;
        switch(r){
        case 4:  B = 0; C = 50; break;                    /* 0..5  */
        case 6:  B = 0; C = 28; break;                    /* 0..9  */
        case 7:  B = b * 0x45; C = 23; break;             /* b000b0b, 0..11 */
        case 9:  B = b * 0x42; C = 13; break;             /* b0000b0, 0..19 */
        case 10: B = c * 0x42 + b * 0x21; C = 11; break;  /* cb000cb, 0..23 */
        default: break;
        }
        T = (D * C + B) ^ A;
        T = (A & 0x20) | (T >> 2);
    }
    return (uint8_t)(T > 32 ? T + 1 : T);
}

/* ---- partition selection (spec hash52 / select_partition) ---- */
static inline uint32_t boot_astc_hash52(uint32_t p){
    p ^= p >> 15;
    p *= 0xEEDE0891u; /* (2^4+1)*(2^7+1)*(2^17-1), mod 2^32 negated form from the spec */
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}
static inline unsigned boot_astc_select_partition(unsigned seed, unsigned x, unsigned y, unsigned z, unsigned count, int small_block){
    if(count <= 1) return 0;
    if(small_block){ x <<= 1; y <<= 1; z <<= 1; }
    seed += (count - 1) * 1024;
    uint32_t rnum = boot_astc_hash52((uint32_t)seed);
    unsigned s[13];
    s[1] = rnum & 0xF; s[2] = (rnum >> 4) & 0xF; s[3] = (rnum >> 8) & 0xF; s[4] = (rnum >> 12) & 0xF;
    s[5] = (rnum >> 16) & 0xF; s[6] = (rnum >> 20) & 0xF; s[7] = (rnum >> 24) & 0xF; s[8] = (rnum >> 28) & 0xF;
    s[9] = (rnum >> 18) & 0xF; s[10] = (rnum >> 22) & 0xF; s[11] = (rnum >> 26) & 0xF;
    s[12] = ((rnum >> 30) | (rnum << 2)) & 0xF;
    for(unsigned i = 1; i <= 12; ++i) s[i] *= s[i];
    unsigned sh1, sh2, sh3;
    if(seed & 1){ sh1 = (seed & 2) ? 4 : 5; sh2 = (count == 3) ? 6 : 5; }
    else{ sh1 = (count == 3) ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    sh3 = (seed & 0x10) ? sh1 : sh2;
    s[1] >>= sh1; s[2] >>= sh2; s[3] >>= sh1; s[4] >>= sh2;
    s[5] >>= sh1; s[6] >>= sh2; s[7] >>= sh1; s[8] >>= sh2;
    s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3; s[12] >>= sh3;
    uint32_t a = s[1] * x + s[2] * y + s[11] * z + (rnum >> 14);
    uint32_t b = s[3] * x + s[4] * y + s[12] * z + (rnum >> 10);
    uint32_t c = s[5] * x + s[6] * y + s[9] * z + (rnum >> 6);
    uint32_t d = s[7] * x + s[8] * y + s[10] * z + (rnum >> 2);
    a &= 0x3F; b &= 0x3F; c &= 0x3F; d &= 0x3F;
    if(count < 4) d = 0;
    if(count < 3) c = 0;
    if(a >= b && a >= c && a >= d) return 0;
    if(b >= c && b >= d) return 1;
    if(c >= d) return 2;
    return 3;
}

/* ---- block mode ---- */
typedef struct {
    unsigned gw, gh, dual, wrange; /* weight grid, dual-plane flag, weight range index */
} boot_astc_mode;
/* Returns 1 when the 11-bit block mode is a legal non-void-extent mode. */
static inline int boot_astc_decode_mode(unsigned m, boot_astc_mode *o){
    unsigned R, H = (m >> 9) & 1, D = (m >> 10) & 1, A = (m >> 5) & 3, B = (m >> 7) & 3, W, Hh;
    if(m & 3){
        R = ((m >> 4) & 1) | ((m & 3) << 1);
        switch((m >> 2) & 3){
        case 0: W = B + 4; Hh = A + 2; break;
        case 1: W = B + 8; Hh = A + 2; break;
        case 2: W = A + 2; Hh = B + 8; break;
        default:
            if(m & 0x100){ W = ((m >> 7) & 1) + 2; Hh = A + 2; }
            else{ W = A + 2; Hh = ((m >> 7) & 1) + 6; }
            break;
        }
    }else{
        R = ((m >> 4) & 1) | (((m >> 2) & 3) << 1);
        if(R < 2) return 0; /* reserved (low 4 bits zero) */
        switch((m >> 7) & 3){
        case 0: W = 12; Hh = A + 2; break;
        case 1: W = A + 2; Hh = 12; break;
        case 2: W = A + 6; Hh = ((m >> 9) & 3) + 6; D = 0; H = 0; break;
        default:
            switch((m >> 5) & 3){
            case 0: W = 6; Hh = 10; break;
            case 1: W = 10; Hh = 6; break;
            default: return 0; /* 111xx reserved (void extent handled earlier) */
            }
            break;
        }
    }
    o->gw = W; o->gh = Hh; o->dual = D; o->wrange = (R - 2) + 6 * H;
    return 1;
}

static inline int boot_astc_footprint_ok(unsigned bw, unsigned bh){
    switch(bw * 16 + bh){
    case 4*16+4: case 5*16+4: case 5*16+5: case 6*16+5: case 6*16+6: case 8*16+5: case 8*16+6:
    case 8*16+8: case 10*16+5: case 10*16+6: case 10*16+8: case 10*16+10: case 12*16+10: case 12*16+12:
        return 1;
    default: return 0;
    }
}

static inline void boot_astc_fill(uint8_t *out, size_t stride, unsigned bw, unsigned bh, const uint8_t rgba[4]){
    for(unsigned y = 0; y < bh; ++y) for(unsigned x = 0; x < bw; ++x) memcpy(out + y * stride + x * 4, rgba, 4);
}
static inline uint8_t boot_astc_out8(unsigned c16, unsigned flags){
    if((flags & BOOT_ASTC_FLAG_ROUND_NEAREST) && !(flags & BOOT_ASTC_FLAG_SRGB))
        return (uint8_t)((c16 * 255u + 32767u) / 65535u);
    return (uint8_t)(c16 >> 8);
}
static inline int boot_astc_clamp8(int v){ return v < 0 ? 0 : v > 255 ? 255 : v; }
/* Floor halving that does not depend on signed right shift semantics. */
static inline int boot_astc_half(int v){ return (v + 1024) / 2 - 512; }
static inline void boot_astc_blue_contract(int c[4]){
    c[0] = boot_astc_half(c[0] + c[2]);
    c[1] = boot_astc_half(c[1] + c[2]);
}
/* bit_transfer_signed(a, b): a = signed offset (out), b = base (out). */
static inline void boot_astc_bit_transfer(int *a, int *b){
    *b >>= 1; *b |= *a & 0x80;
    *a >>= 1; *a &= 0x3F;
    if(*a & 0x20) *a -= 0x40;
}
/* LDR endpoint decode; v holds unquantised 0..255 values.  Returns 0 for HDR modes. */
static inline int boot_astc_endpoints(unsigned mode, const uint8_t *vin, int e0[4], int e1[4]){
    int v[8];
    for(unsigned i = 0; i < 8; ++i) v[i] = (i < ((mode >> 2) + 1) * 2) ? vin[i] : 0;
    switch(mode){
    case 0:
        e0[0] = e0[1] = e0[2] = v[0]; e0[3] = 255;
        e1[0] = e1[1] = e1[2] = v[1]; e1[3] = 255;
        break;
    case 1:{
        int l0 = (v[0] >> 2) | (v[1] & 0xC0), l1 = l0 + (v[1] & 0x3F);
        if(l1 > 255) l1 = 255;
        e0[0] = e0[1] = e0[2] = l0; e0[3] = 255;
        e1[0] = e1[1] = e1[2] = l1; e1[3] = 255;
        break;}
    case 4:
        e0[0] = e0[1] = e0[2] = v[0]; e0[3] = v[2];
        e1[0] = e1[1] = e1[2] = v[1]; e1[3] = v[3];
        break;
    case 5:
        boot_astc_bit_transfer(&v[1], &v[0]);
        boot_astc_bit_transfer(&v[3], &v[2]);
        e0[0] = e0[1] = e0[2] = v[0]; e0[3] = v[2];
        e1[0] = e1[1] = e1[2] = boot_astc_clamp8(v[0] + v[1]); e1[3] = boot_astc_clamp8(v[2] + v[3]);
        break;
    case 6:
        e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = 255;
        e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = 255;
        break;
    case 8: case 12:{
        int a0 = mode == 12 ? v[6] : 255, a1 = mode == 12 ? v[7] : 255;
        if(v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]){
            e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = a0;
            e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = a1;
        }else{
            e0[0] = v[1]; e0[1] = v[3]; e0[2] = v[5]; e0[3] = a1;
            e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; e1[3] = a0;
            boot_astc_blue_contract(e0); boot_astc_blue_contract(e1);
        }
        break;}
    case 9: case 13:{
        boot_astc_bit_transfer(&v[1], &v[0]);
        boot_astc_bit_transfer(&v[3], &v[2]);
        boot_astc_bit_transfer(&v[5], &v[4]);
        if(mode == 13) boot_astc_bit_transfer(&v[7], &v[6]);
        else{ v[6] = 255; v[7] = 0; }
        if(v[1] + v[3] + v[5] >= 0){
            e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = v[6];
            e1[0] = v[0] + v[1]; e1[1] = v[2] + v[3]; e1[2] = v[4] + v[5]; e1[3] = v[6] + v[7];
        }else{
            e0[0] = v[0] + v[1]; e0[1] = v[2] + v[3]; e0[2] = v[4] + v[5]; e0[3] = v[6] + v[7];
            e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; e1[3] = v[6];
            boot_astc_blue_contract(e0); boot_astc_blue_contract(e1);
        }
        for(unsigned i = 0; i < 4; ++i){ e0[i] = boot_astc_clamp8(e0[i]); e1[i] = boot_astc_clamp8(e1[i]); }
        break;}
    case 10:
        e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = v[4];
        e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = v[5];
        break;
    default:
        return 0; /* HDR: 2,3,7,11,14,15 */
    }
    return 1;
}

/* Decode one 16-byte block of footprint bw x bh into bw*bh RGBA8 texels at
 * `out` (row pitch `stride` bytes).  Returns BOOT_ASTC_OK, BOOT_ASTC_ERROR_BLOCK
 * (illegal encoding, error color written), BOOT_ASTC_E_HDR or BOOT_ASTC_E_ARGS. */
static inline int boot_astc_decode_block_ex(const uint8_t block[16], unsigned bw, unsigned bh, unsigned flags, uint8_t *out, size_t stride){
    static const uint8_t magenta[4] = {255, 0, 255, 255};
    if(!block || !out || !boot_astc_footprint_ok(bw, bh) || stride < (size_t)bw * 4) return BOOT_ASTC_E_ARGS;
    uint64_t w[2] = {0, 0};
    for(unsigned i = 0; i < 8; ++i){ w[0] |= (uint64_t)block[i] << (8 * i); w[1] |= (uint64_t)block[8 + i] << (8 * i); }
    unsigned mode = boot_astc_get(w, 0, 11);

    if((mode & 0x1FF) == 0x1FC){ /* void-extent */
        unsigned lo_s = boot_astc_get(w, 12, 13), hi_s = boot_astc_get(w, 25, 13);
        unsigned lo_t = boot_astc_get(w, 38, 13), hi_t = boot_astc_get(w, 51, 13);
        int all_ones = lo_s == 0x1FFF && hi_s == 0x1FFF && lo_t == 0x1FFF && hi_t == 0x1FFF;
        if(boot_astc_get(w, 10, 2) != 3 || (!all_ones && (lo_s >= hi_s || lo_t >= hi_t))){
            boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_ERROR_BLOCK;
        }
        if(mode & 0x200){ boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_E_HDR; }
        uint8_t c[4];
        for(unsigned i = 0; i < 4; ++i) c[i] = boot_astc_out8(boot_astc_get(w, 64 + 16 * i, 16), flags);
        boot_astc_fill(out, stride, bw, bh, c);
        return BOOT_ASTC_OK;
    }

    boot_astc_mode bm;
    unsigned parts = boot_astc_get(w, 11, 2) + 1;
    if(!boot_astc_decode_mode(mode, &bm) || bm.gw > bw || bm.gh > bh || (bm.dual && parts == 4)){
        boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_ERROR_BLOCK;
    }
    unsigned planes = bm.dual ? 2 : 1, nweights = bm.gw * bm.gh * planes;
    unsigned wbits = boot_astc_ise_bits(nweights, bm.wrange);
    if(nweights > 64 || wbits < 24 || wbits > 96){
        boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_ERROR_BLOCK;
    }

    unsigned cem[4] = {0, 0, 0, 0}, seed = 0, start, below = 128 - wbits, ccs = 0;
    if(parts == 1){
        cem[0] = boot_astc_get(w, 13, 4);
        start = 17;
    }else{
        seed = boot_astc_get(w, 13, 10);
        unsigned enc = boot_astc_get(w, 23, 6);
        if((enc & 3) == 0){
            for(unsigned i = 0; i < parts; ++i) cem[i] = enc >> 2;
        }else{
            unsigned extra = 3 * parts - 4;
            below -= extra;
            enc |= boot_astc_get(w, below, extra) << 6;
            unsigned base = (enc & 3) - 1;
            for(unsigned i = 0; i < parts; ++i){
                unsigned cbit = (enc >> (2 + i)) & 1, mbits = (enc >> (2 + parts + 2 * i)) & 3;
                cem[i] = ((base + cbit) << 2) | mbits;
            }
        }
        start = 29;
    }
    if(bm.dual){ below -= 2; ccs = boot_astc_get(w, below, 2); }

    unsigned nvals = 0;
    for(unsigned i = 0; i < parts; ++i) nvals += ((cem[i] >> 2) + 1) * 2;
    if(nvals > 18 || below < start){
        boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_ERROR_BLOCK;
    }
    unsigned cbits = below - start;
    int crange = -1;
    for(int r = 20; r >= 0; --r) if(boot_astc_ise_bits(nvals, (unsigned)r) <= cbits){ crange = r; break; }
    if(crange < 4){ /* not even range 0..5 fits */
        boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_ERROR_BLOCK;
    }
    for(unsigned i = 0; i < parts; ++i){
        unsigned c = cem[i];
        if(c == 2 || c == 3 || c == 7 || c == 11 || c == 14 || c == 15){
            boot_astc_fill(out, stride, bw, bh, magenta); return BOOT_ASTC_E_HDR;
        }
    }

    /* color endpoints */
    uint8_t craw[18], cval[18];
    boot_astc_ise_decode(w, start, nvals, (unsigned)crange, craw);
    for(unsigned i = 0; i < nvals; ++i) cval[i] = boot_astc_unquant_color((unsigned)crange, craw[i]);
    unsigned ep[4][2][4];
    for(unsigned i = 0, off = 0; i < parts; ++i){
        int e0[4], e1[4];
        boot_astc_endpoints(cem[i], cval + off, e0, e1);
        off += ((cem[i] >> 2) + 1) * 2;
        for(unsigned c = 0; c < 4; ++c){
            if((flags & BOOT_ASTC_FLAG_SRGB) && c < 3){
                ep[i][0][c] = ((unsigned)e0[c] << 8) | 0x80;
                ep[i][1][c] = ((unsigned)e1[c] << 8) | 0x80;
            }else{
                ep[i][0][c] = (unsigned)e0[c] * 257u;
                ep[i][1][c] = (unsigned)e1[c] * 257u;
            }
        }
    }

    /* weights: stored bit-reversed from the top of the block */
    uint64_t rw[2] = {boot_astc_rev64(w[1]), boot_astc_rev64(w[0])};
    uint8_t wraw[64], wq[64];
    boot_astc_ise_decode(rw, 0, nweights, bm.wrange, wraw);
    for(unsigned i = 0; i < nweights; ++i) wq[i] = boot_astc_unquant_weight(bm.wrange, wraw[i]);

    unsigned ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
    int small_block = bw * bh < 31;
    for(unsigned y = 0; y < bh; ++y){
        unsigned gt = (dt * y * (bm.gh - 1) + 32) >> 6, jt = gt >> 4, ft = gt & 15;
        for(unsigned x = 0; x < bw; ++x){
            unsigned gs = (ds * x * (bm.gw - 1) + 32) >> 6, js = gs >> 4, fs = gs & 15;
            unsigned w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            unsigned v0 = js + jt * bm.gw;
            unsigned pw[2];
            for(unsigned p = 0; p < planes; ++p){
                unsigned i00 = v0, i01 = v0 + 1, i10 = v0 + bm.gw, i11 = v0 + bm.gw + 1;
                unsigned last = bm.gw * bm.gh - 1;
                /* zero-weighted taps may point past the grid edge: clamp the index */
                if(i01 > last) i01 = last;
                if(i10 > last) i10 = last;
                if(i11 > last) i11 = last;
                unsigned s = wq[i00 * planes + p] * w00 + wq[i01 * planes + p] * w01
                           + wq[i10 * planes + p] * w10 + wq[i11 * planes + p] * w11;
                pw[p] = (s + 8) >> 4;
            }
            unsigned part = boot_astc_select_partition(seed, x, y, 0, parts, small_block);
            uint8_t *px = out + y * stride + x * 4;
            for(unsigned c = 0; c < 4; ++c){
                unsigned wt = (bm.dual && c == ccs) ? pw[1] : pw[0];
                unsigned c0 = ep[part][0][c], c1 = ep[part][1][c];
                unsigned c16 = (c0 * (64 - wt) + c1 * wt + 32) >> 6;
                px[c] = boot_astc_out8(c16, flags);
            }
        }
    }
    return BOOT_ASTC_OK;
}
static inline int boot_astc_decode_block(const uint8_t block[16], unsigned bw, unsigned bh, uint8_t *rgba_out, size_t stride){
    return boot_astc_decode_block_ex(block, bw, bh, 0, rgba_out, stride);
}

/* Decode a linear (detiled, row-major block order) 2D ASTC image to tightly
 * packed RGBA8 (width*height*4 bytes).  `blocks` must hold
 * ceil(w/bw)*ceil(h/bh)*16 bytes.  Returns <0 on failure (BOOT_ASTC_E_ARGS,
 * or BOOT_ASTC_E_HDR if any block was HDR: the image is still fully written,
 * HDR blocks as magenta), otherwise the number of illegal-encoding blocks
 * (0 for a clean image). */
static inline int boot_astc_decode_image_ex(const uint8_t *blocks, size_t blocks_size, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, unsigned flags, uint8_t *rgba_out, size_t out_size){
    if(!blocks || !rgba_out || !width || !height || !boot_astc_footprint_ok(bw, bh)) return BOOT_ASTC_E_ARGS;
    uint64_t bx = ((uint64_t)width + bw - 1) / bw, by = ((uint64_t)height + bh - 1) / bh;
    uint64_t need_out = (uint64_t)width * height * 4, need_in = bx * by * 16;
    if(need_out > SIZE_MAX || need_in > SIZE_MAX || out_size < need_out || blocks_size < need_in) return BOOT_ASTC_E_ARGS;
    uint8_t tmp[12 * 12 * 4];
    int errors = 0, hdr = 0;
    for(uint64_t j = 0; j < by; ++j){
        for(uint64_t i = 0; i < bx; ++i){
            int r = boot_astc_decode_block_ex(blocks + (size_t)((j * bx + i) * 16), bw, bh, flags, tmp, (size_t)bw * 4);
            if(r == BOOT_ASTC_E_HDR) hdr = 1;
            else if(r == BOOT_ASTC_ERROR_BLOCK){ if(errors < INT32_MAX) ++errors; }
            else if(r != BOOT_ASTC_OK) return r;
            uint64_t x0 = i * bw, y0 = j * bh;
            unsigned cw = (unsigned)((width - x0) < bw ? (width - x0) : bw);
            unsigned ch = (unsigned)((height - y0) < bh ? (height - y0) : bh);
            for(unsigned y = 0; y < ch; ++y)
                memcpy(rgba_out + (size_t)(((y0 + y) * width + x0) * 4), tmp + (size_t)y * bw * 4, (size_t)cw * 4);
        }
    }
    return hdr ? BOOT_ASTC_E_HDR : errors;
}
/* Requested entry point: `blocks` size is implied by the dimensions. */
static inline int boot_astc_decode_image(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, uint8_t *rgba_out, size_t out_size){
    if(!width || !height || !bw || !bh) return BOOT_ASTC_E_ARGS;
    uint64_t need_in = (((uint64_t)width + bw - 1) / bw) * (((uint64_t)height + bh - 1) / bh) * 16;
    if(need_in > SIZE_MAX) return BOOT_ASTC_E_ARGS;
    return boot_astc_decode_image_ex(blocks, (size_t)need_in, width, height, bw, bh, 0, rgba_out, out_size);
}
#endif
