// Runtime ABI shared by every translated Maxwell shader (see maxwell_hlsl.py).
//
// Bindings (root signature built by the host):
//   b0..b17  : the stage's 18 Maxwell constant buffers, each up to 64 KiB
//   b18      : HostState (per-draw values the source GPU kept in registers)
//   t0..,space7 / u0..,space7: guest memory, one zero-copy buffer per guest region
//   t1,space0: GPU VA page table, 4 KiB pages -> (region << 24 | page in region), 0xffffffff unmapped
//   t2,space0: vertex attribute/stream table for vertex pulling
//   t0,space1..6,8,9 / s0,space1..2: bindless texture and sampler descriptor arrays,
//              indexed by the NVN handle's TIC (bits 0-19) and TSC (bits 20-31). One array per
//              texture type (1D, 2D, 2D array, 3D, cube, cube array, buffer, 1D array); a TIC
//              index holds a view only in the array of its header's type, null in the others.
// Source behaviour that cannot be expressed is never approximated here silently:
// every approximation is listed in maxwell_hlsl.APPROXIMATIONS.

#ifndef TEX_BANK
#define TEX_BANK 2          // NVB197_SET_BINDLESS_TEXTURE as programmed by the game
#endif

cbuffer HostState : register(b18) {
    float host_y_direction;    // SR_Y_DIRECTION: +1 or -1 by window origin
    uint  host_depth_minus1;   // 1 when the source uses [-1,1] clip depth
    uint  host_gpu_page_count; // entries in the GPU page table
    uint  host_gpu_va_base_page;   // in 4 KiB pages
    uint  host_vertex_base;    // first vertex for vertex pulling
    uint  host_instance_base;
    uint  host_flip_y;         // 1 when the source viewport has negative y scale (D3D12 needs positive height)
    uint  host_provoking_first;   // render-36: 1 = flat inputs from the first vertex (D3D), 0 = the last (Maxwell/NVN default)
    uint  host_instance_offset;   // K2 (batch-4): instances before this host draw (TOTK_INSTANCE_EXPAND: one host draw per
                                  // instance), else 0; host_instance_base is the draw's base instance plus it. 0xffffffff:
                                  // the host's TOTK_INSTANCE_ID_ZERO_BASED=0 (host_iid_offset)
};
#ifdef INSTANCE_ID_ZERO_BASED
// K2 (batch-4): what a vertex shader translated with K2 adds to SV_InstanceID (translate_game_shaders.py
// INSTANCE_ID_ZERO_BASED): the instances before this host draw, so InstanceId counts from 0 without the base instance, as on
// the source GPU. 0xffffffff (host TOTK_INSTANCE_ID_ZERO_BASED=0): the base instance, as before; vin_attr4 then reads
// element iid / divisor, exactly the rule of shaders translated without K2.
uint host_iid_offset() { return host_instance_offset == 0xffffffffu ? host_instance_base : host_instance_offset; }
#endif

#define CBUF(n) cbuffer CB##n : register(b##n) { uint4 cb##n[4096]; }
CBUF(0) CBUF(1) CBUF(2) CBUF(3) CBUF(4) CBUF(5) CBUF(6) CBUF(7) CBUF(8)
CBUF(9) CBUF(10) CBUF(11) CBUF(12) CBUF(13) CBUF(14) CBUF(15) CBUF(16) CBUF(17)
// Out-of-range constant reads return zero, as on the source GPU.
#define CBUF_LOAD(n) uint cbuf_load##n(uint off) { return off < 65536u ? cb##n[off >> 4][(off >> 2) & 3u] : 0u; }
CBUF_LOAD(0) CBUF_LOAD(1) CBUF_LOAD(2) CBUF_LOAD(3) CBUF_LOAD(4) CBUF_LOAD(5) CBUF_LOAD(6) CBUF_LOAD(7) CBUF_LOAD(8)
CBUF_LOAD(9) CBUF_LOAD(10) CBUF_LOAD(11) CBUF_LOAD(12) CBUF_LOAD(13) CBUF_LOAD(14) CBUF_LOAD(15) CBUF_LOAD(16) CBUF_LOAD(17)

// ---------------------------------------------------------------- float helpers
float fmul_fmz(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float ffma_fmz(float a, float b, float c) { return ((a == 0.0f || b == 0.0f) ? 0.0f : a * b) + c; }
bool fnum(float a, float b) { return a == a && b == b; }
bool fne_ordered(float a, float b) { return a < b || a > b; }
float round_ne(float x) { return round(x); }   // HLSL round: nearest, ties to even

// ---------------------------------------------------------------- packed half helpers (HADD2/HMUL2/HFMA2)
// fp32 -> fp16 bits, round to nearest even, with fp16 subnormals, overflow to infinity and quiet NaN.
// Written out instead of f32tof16 so the rounding does not depend on the host compiler/driver.
uint h_round(float f) {
    uint x = asuint(f), s = (x >> 16) & 0x8000u, e = (x >> 23) & 0xffu, m = x & 0x7fffffu;
    if (e == 0xffu) return s | 0x7c00u | (m != 0u ? 0x200u | (m >> 13) : 0u);
    int E = (int)e - 112;                       // fp16 biased exponent
    if (E >= 31) return s | 0x7c00u;
    if (E <= 0) {                               // fp16 subnormal or zero
        if (E < -10) return s;
        uint M = m | 0x800000u, sh = (uint)(14 - E), r = M >> sh, rem = M & ((1u << sh) - 1u), hf = 1u << (sh - 1u);
        if (rem > hf || (rem == hf && (r & 1u) != 0u)) r++;
        return s | r;
    }
    uint r = ((uint)E << 10) | (m >> 13), rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u) != 0u)) r++;   // a carry rounds up into the exponent / infinity
    return s | r;
}
// FTZ forms: fp16 subnormal inputs and results become a zero of the same sign.
float h_ftz(float x) { return abs(x) < 6.103515625e-05f ? asfloat(asuint(x) & 0x80000000u) : x; }
uint h_round_ftz(float f) { uint h = h_round(f); return (h & 0x7c00u) == 0u ? h & 0x8000u : h; }

uint f2i_s32(float x) {
    if (x != x) return 0u;
    if (x >= 2147483648.0f) return 0x7fffffffu;
    if (x <= -2147483648.0f) return 0x80000000u;
    return (uint)(int)x;
}
uint f2i_u32(float x) {
    if (!(x > 0.0f)) return 0u;
    if (x >= 4294967296.0f) return 0xffffffffu;
    return (uint)x;
}
uint f2i_s16(float x) { return x != x ? 0u : (uint)(int)clamp(x, -32768.0f, 32767.0f); }
uint f2i_u16(float x) { return x != x ? 0u : (uint)clamp(x, 0.0f, 65535.0f); }
uint f2i_s8(float x)  { return x != x ? 0u : (uint)(int)clamp(x, -128.0f, 127.0f); }
uint f2i_u8(float x)  { return x != x ? 0u : (uint)clamp(x, 0.0f, 255.0f); }

// ---------------------------------------------------------------- integer helpers
uint shl_clamp(uint a, uint s) { return s >= 32u ? 0u : a << s; }
uint shr_u_clamp(uint a, uint s) { return s >= 32u ? 0u : a >> s; }
uint shr_s_clamp(uint a, uint s) { return (uint)((int)a >> min(s, 31u)); }
uint shf_count(uint s, uint mask, bool wrap) { return wrap ? (s & mask) : min(s, mask + 1u); }
uint shf_l(uint lo, uint hi, uint s, uint mask, bool wrap) {
    uint n = shf_count(s, mask, wrap);
    if (n == 0u) return hi;
    if (n < 32u) return (hi << n) | (lo >> (32u - n));
    if (n < 64u) return lo << (n - 32u);
    return 0u;
}
uint shf_r_u(uint lo, uint hi, uint s, uint mask, bool wrap) {
    uint n = shf_count(s, mask, wrap);
    if (n == 0u) return lo;
    if (n < 32u) return (lo >> n) | (hi << (32u - n));
    if (n < 64u) return hi >> (n - 32u);
    return 0u;
}
uint shf_r_s(uint lo, uint hi, uint s, uint mask, bool wrap) {
    uint n = shf_count(s, mask, wrap);
    if (n == 0u) return lo;
    if (n < 32u) return (lo >> n) | (hi << (32u - n));
    if (n < 64u) return (uint)((int)hi >> (n - 32u));
    return (uint)((int)hi >> 31);
}
uint bfe_u(uint a, uint b) {
    uint pos = b & 0xffu, len = (b >> 8) & 0xffu;
    if (len == 0u || pos >= 32u) return 0u;
    len = min(len, 32u - pos);
    uint v = a >> pos;
    return len == 32u ? v : v & ((1u << len) - 1u);
}
uint bfe_s(uint a, uint b) {
    uint pos = b & 0xffu, len = (b >> 8) & 0xffu;
    if (len == 0u) return 0u;
    if (pos >= 32u) return (uint)((int)a >> 31);
    len = min(len, 32u - pos);
    return (uint)(((int)(a << (32u - pos - len))) >> (32u - len));
}
uint bfi_(uint insert, uint b, uint base) {
    uint pos = b & 0xffu, len = (b >> 8) & 0xffu;
    if (len == 0u || pos >= 32u) return base;
    len = min(len, 32u - pos);
    uint mask = (len == 32u ? 0xffffffffu : ((1u << len) - 1u)) << pos;
    return (base & ~mask) | ((insert << pos) & mask);
}
int2 aoffi2(uint packed) {   // 4-bit signed texel offsets, x in bits 0-3, y in bits 4-7
    return int2((int)(packed << 28) >> 28, (int)(packed << 24) >> 28);
}
int aoffi1(uint packed) { return (int)(packed << 28) >> 28; }
int3 aoffi3(uint packed) {   // TLD 3D: x bits 0-3, y bits 4-7, z bits 8-11
    return int3((int)(packed << 28) >> 28, (int)(packed << 24) >> 28, (int)(packed << 20) >> 28);
}

// ---------------------------------------------------------------- wave
#if __SHADER_TARGET_MAJOR >= 6
uint lane_id() { return WaveGetLaneIndex(); }
uint wave_ballot(bool p) { return WaveActiveBallot(p).x; }
#else
uint lane_id() { return 0u; }
uint wave_ballot(bool p) { return p ? 1u : 0u; }
bool WaveActiveAllTrue(bool p) { return p; }
bool WaveActiveAnyTrue(bool p) { return p; }
bool WaveActiveAllEqual(bool p) { return true; }
#endif
uint invocation_id_() { return 0u; }
#if __SHADER_TARGET_MAJOR >= 6
uint2 shfl_core_(uint v, uint src, uint lane, uint max_lane, bool up) {
    bool ok = up ? ((int)src >= (int)max_lane) : (src <= max_lane);
    uint r = WaveReadLaneAt(v, ok ? src : lane);
    return uint2(ok ? r : v, ok ? 1u : 0u);
}
uint2 shfl_idx(uint v, uint b, uint c)  { uint lane = WaveGetLaneIndex(), seg = (c >> 8) & 31u, lo = lane & seg, hi = lo | ((c & 31u) & ~seg);
                                          return shfl_core_(v, lo | (b & ~seg), lane, hi, false); }
uint2 shfl_up(uint v, uint b, uint c)   { uint lane = WaveGetLaneIndex(), seg = (c >> 8) & 31u, lo = lane & seg, hi = lo | ((c & 31u) & ~seg);
                                          return shfl_core_(v, lane - b, lane, hi, true); }
uint2 shfl_down(uint v, uint b, uint c) { uint lane = WaveGetLaneIndex(), seg = (c >> 8) & 31u, lo = lane & seg, hi = lo | ((c & 31u) & ~seg);
                                          return shfl_core_(v, lane + b, lane, hi, false); }
uint2 shfl_bfly(uint v, uint b, uint c) { uint lane = WaveGetLaneIndex(), seg = (c >> 8) & 31u, lo = lane & seg, hi = lo | ((c & 31u) & ~seg);
                                          return shfl_core_(v, lane ^ b, lane, hi, false); }
float fswzadd_(float a, float b, uint swz) {
    uint m = (swz >> (2u * (WaveGetLaneIndex() & 3u))) & 3u;
    float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f), mb = m == 2u ? -1.0f : 1.0f;
    return a * ma + b * mb;
}
#endif
#if __SHADER_TARGET_MAJOR > 6 || (__SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 6)
uint thread_kill_() { return IsHelperLane() ? 1u : 0u; }
#else
uint thread_kill_() { return 0u; }   // only helper lanes read nonzero; their results are discarded
#endif

// ---------------------------------------------------------------- guest GPU memory
// Guest memory is one zero-copy buffer per guest host allocation (region). A 4 KiB
// GPU page maps to (region << 24) | 4 KiB page within the region; 0xffffffff is unmapped.
ByteAddressBuffer      gpu_arena[]    : register(t0, space7);
StructuredBuffer<uint> gpu_ptable     : register(t1, space0);
#ifdef GMEM_WRITABLE
#ifdef STAGE_COMPUTE
globallycoherent RWByteAddressBuffer gpu_arena_rw[] : register(u0, space7);   // other groups' stores/atomics must be observable
#else
RWByteAddressBuffer    gpu_arena_rw[] : register(u0, space7);
#endif
#endif
uint2 gmem_addr(uint lo, uint hi, int off) {
    uint nlo = lo + (uint)off;
    uint carry = (off >= 0) ? (nlo < lo ? 1u : 0u) : (nlo > lo ? 0xffffffffu : 0u);
    return uint2(nlo, hi + carry);
}
// Returns (region, byte offset); region 0xffffffff when unmapped.
uint2 gmem_offset(uint2 va, uint add) {
    uint lo = va.x + add;
    uint hi = va.y + (lo < va.x ? 1u : 0u);
    uint page = (hi << 20) | (lo >> 12);           // 4 KiB pages over a 44-bit VA
    uint idx = page - host_gpu_va_base_page;
    if (idx >= host_gpu_page_count) return uint2(0xffffffffu, 0u);
    uint e = gpu_ptable[idx];
    if (e == 0xffffffffu) return uint2(0xffffffffu, 0u);
    return uint2(e >> 24, ((e & 0xffffffu) << 12) | (lo & 0xfffu));
}
#if defined(STAGE_COMPUTE) && defined(GMEM_WRITABLE)
// Compute reads through the UAV: a resource is in one state per dispatch, and loads must see
// stores of other threads (after their barriers) rather than a read-only cached view.
uint gmem_load(uint2 va, uint add) {
    uint2 o = gmem_offset(va, add);
    return o.x == 0xffffffffu ? 0u : gpu_arena_rw[NonUniformResourceIndex(o.x)].Load(o.y & ~3u);
}
#else
uint gmem_load(uint2 va, uint add) {
    uint2 o = gmem_offset(va, add);
    return o.x == 0xffffffffu ? 0u : gpu_arena[NonUniformResourceIndex(o.x)].Load(o.y & ~3u);
}
#endif
#ifdef STAGE_COMPUTE
// Pages a dispatch stores to: one bit per page-table entry, read by the host after completion to
// invalidate cached copies (textures) of guest memory (host_gpu_compute.h).
RWByteAddressBuffer gpu_dirty : register(u1, space0);
void gmem_mark(uint2 va, uint add) {
    uint lo = va.x + add, hi = va.y + (lo < va.x ? 1u : 0u);
    uint idx = ((hi << 20) | (lo >> 12)) - host_gpu_va_base_page;
    if (idx < host_gpu_page_count) {
        uint bit = 1u << (idx & 31u);
        if ((gpu_dirty.Load((idx >> 5) * 4u) & bit) == 0u) gpu_dirty.InterlockedOr((idx >> 5) * 4u, bit);
    }
}
#else
void gmem_mark(uint2 va, uint add) {}
#endif
#ifdef GMEM_WRITABLE
void gmem_store(uint2 va, uint add, uint v) { uint2 o = gmem_offset(va, add); if (o.x != 0xffffffffu) { gpu_arena_rw[NonUniformResourceIndex(o.x)].Store(o.y & ~3u, v); gmem_mark(va, add); } }
// Sub-word store (8/16-bit): read-modify-write of the containing word with atomics, so neighbouring bytes stored by other threads survive.
void gmem_store_bits(uint2 va, uint add, uint v, uint mask) {
    uint2 o = gmem_offset(va, add); uint old_;
    if (o.x == 0xffffffffu) return;
    uint sh = (o.y & 3u) * 8u;
    gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedAnd(o.y & ~3u, ~(mask << sh), old_);
    gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedOr(o.y & ~3u, (v & mask) << sh, old_);
    gmem_mark(va, add);
}
// Non-returning atomics (RED) whose address is the same in every active lane are merged into one atomic per wave
// (sum / min / max / and / or / xor of the active lanes' values), as Maxwell's compiler does for RED. The value left
// in memory is identical (these operations are associative and commutative, and wrap the same way). It matters because
// guest memory is zero-copy system memory across PCIe, where same-address atomics serialise at ~0.3 us each: one
// 320x180 pass doing two per pixel cost 34.8 ms of GPU time per frame (render-40 profile, play-81). Lanes that do
// not call the function (branches, KILLED guards) are not active. Helper lanes are active lanes for wave operations
// but their UAV writes are discarded, so they leave before any wave operation: otherwise a helper could become the
// leader and drop the merged atomic. Different addresses in one wave fall back to one atomic per lane.
#if defined(STAGE_FRAGMENT)
#define GMEM_NOHELPER_ if (IsHelperLane()) return;
#else
#define GMEM_NOHELPER_
#endif
#define GMEM_ATOMIC1(name, fn, T) void gmem_atomic1_##name(uint2 va, T v) { uint2 o = gmem_offset(va, 0u); T old_; if (o.x != 0xffffffffu) { gpu_arena_rw[NonUniformResourceIndex(o.x)].fn(o.y & ~3u, v, old_); gmem_mark(va, 0u); } }
#define GMEM_ATOMIC(name, fn, wave, T) GMEM_ATOMIC1(name, fn, T) void gmem_atomic_##name(uint2 va, uint v) { GMEM_NOHELPER_ if (WaveActiveAllEqual(va.x) && WaveActiveAllEqual(va.y)) { T s_ = wave((T)v); if (WaveIsFirstLane()) gmem_atomic1_##name(va, s_); } else gmem_atomic1_##name(va, (T)v); }
GMEM_ATOMIC(add, InterlockedAdd, WaveActiveSum, uint) GMEM_ATOMIC(min, InterlockedMin, WaveActiveMin, uint) GMEM_ATOMIC(max, InterlockedMax, WaveActiveMax, uint)
GMEM_ATOMIC(and, InterlockedAnd, WaveActiveBitAnd, uint) GMEM_ATOMIC(or, InterlockedOr, WaveActiveBitOr, uint) GMEM_ATOMIC(xor, InterlockedXor, WaveActiveBitXor, uint)
GMEM_ATOMIC(min_s, InterlockedMin, WaveActiveMin, int) GMEM_ATOMIC(max_s, InterlockedMax, WaveActiveMax, int)
#define GMEM_ATOMIC_RET(name, fn) uint gmem_atomic_ret_##name(uint2 va, uint v) { uint2 o = gmem_offset(va, 0u); uint old_ = 0u; if (o.x != 0xffffffffu) { gpu_arena_rw[NonUniformResourceIndex(o.x)].fn(o.y & ~3u, v, old_); gmem_mark(va, 0u); } return old_; }
GMEM_ATOMIC_RET(add, InterlockedAdd) GMEM_ATOMIC_RET(min, InterlockedMin) GMEM_ATOMIC_RET(max, InterlockedMax)
GMEM_ATOMIC_RET(and, InterlockedAnd) GMEM_ATOMIC_RET(or, InterlockedOr) GMEM_ATOMIC_RET(xor, InterlockedXor)
GMEM_ATOMIC_RET(exch, InterlockedExchange)
uint gmem_atomic_ret_min_s(uint2 va, uint v) { uint2 o = gmem_offset(va, 0u); int old_ = 0; if (o.x != 0xffffffffu) { gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedMin(o.y & ~3u, (int)v, old_); gmem_mark(va, 0u); } return (uint)old_; }
uint gmem_atomic_ret_max_s(uint2 va, uint v) { uint2 o = gmem_offset(va, 0u); int old_ = 0; if (o.x != 0xffffffffu) { gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedMax(o.y & ~3u, (int)v, old_); gmem_mark(va, 0u); } return (uint)old_; }
uint gmem_atomic_cas(uint2 va, uint cmp, uint v) { uint2 o = gmem_offset(va, 0u); uint old_ = 0u; if (o.x != 0xffffffffu) { gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedCompareExchange(o.y & ~3u, cmp, v, old_); gmem_mark(va, 0u); } return old_; }
// ATOM.INC/DEC (wrap at the operand, CUDA atomicInc/atomicDec) via compare-exchange
uint gmem_atomic_ret_inc(uint2 va, uint v) { uint2 o = gmem_offset(va, 0u); if (o.x == 0xffffffffu) return 0u; uint old_ = gpu_arena_rw[NonUniformResourceIndex(o.x)].Load(o.y & ~3u), seen_;
    [allow_uav_condition] for (uint k_ = 0u; k_ < 65536u; ++k_) { uint nv = old_ >= v ? 0u : old_ + 1u; gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedCompareExchange(o.y & ~3u, old_, nv, seen_); if (seen_ == old_) break; old_ = seen_; }
    gmem_mark(va, 0u); return old_; }
uint gmem_atomic_ret_dec(uint2 va, uint v) { uint2 o = gmem_offset(va, 0u); if (o.x == 0xffffffffu) return 0u; uint old_ = gpu_arena_rw[NonUniformResourceIndex(o.x)].Load(o.y & ~3u), seen_;
    [allow_uav_condition] for (uint k_ = 0u; k_ < 65536u; ++k_) { uint nv = (old_ == 0u || old_ > v) ? v : old_ - 1u; gpu_arena_rw[NonUniformResourceIndex(o.x)].InterlockedCompareExchange(o.y & ~3u, old_, nv, seen_); if (seen_ == old_) break; old_ = seen_; }
    gmem_mark(va, 0u); return old_; }
#else
void gmem_store(uint2 va, uint add, uint v) {}
#endif

// ---------------------------------------------------------------- vertex pulling
// One entry per generic attribute (32), filled from VERTEX_ATTRIB_FORMAT and VERTEX_STREAM.
struct VertexAttrib {
    uint2 stream_va;   // GPU VA of the stream start
    uint  stride;      // bytes; 0 for a constant attribute
    uint  offset;      // byte offset in the vertex
    uint  format;      // bits 0-5 size code, 6-8 type (Maxwell encoding), 9 bgra, 31 enabled
    uint  divisor;     // 0 = per vertex, otherwise per-instance step rate
    uint  size;        // vbounds: bytes bound from stream_va (NVN BindVertexBuffer size); 0 = unknown, no bound
    uint  pad;
};
StructuredBuffer<VertexAttrib> vertex_attribs : register(t2, space0);

// v / (2^k - 1) correctly rounded, 1 <= k <= 16, v <= 2^k - 1. The source GPU converts UNORM/SNORM vertex formats in
// fixed function, exactly (nxport: Metal *Normalized vertex formats, translated.rs:2134-2160); a runtime fp32 divide is
// only required to 2.5 ULP in D3D, and game vertex shaders recover integers with trunc(attr * (2^n - 1)) as table
// indices (foliage/wind, VS 744dd70b, 5df7f30c), where 1 ULP low picks the neighbouring entry. The quotient is the
// k-bit pattern of v repeated (binary 0.vvvv...); its expansion never ends in all zeros, so the round bit decides
// (no ties). Bit-exact against IEEE division for every v, unorm 2-16 bits and snorm 8/10/16
// (play-work/explode-work2/shader-translation-b.md section B).
float unorm_div_exact(uint v, uint k) {
    if (v == 0u) return 0.0f;
    if (v >= (1u << k) - 1u) return 1.0f;
    uint64_t X = (uint64_t)v << (64u - k);
    [unroll] for (uint s = 1u; s <= 32u; s <<= 1u) { uint sh = s * k; if (sh < 64u) X |= X >> sh; }
    uint L = firstbithigh(v);
    uint64_t W = X << (k - 1u - L);                                     // window starting at the leading 1
    uint M = (uint)(W >> 40) + ((uint)(W >> 39) & 1u);                  // 24-bit significand plus the round bit
    uint e = 127u - (k - L);
    if ((M >> 24) != 0u) { M >>= 1; e += 1u; }
    return asfloat((e << 23) | (M & 0x7fffffu));
}
float unorm_(uint v, uint bits_) {
    if (bits_ <= 16u) return unorm_div_exact(v, bits_);
    return (float)v / (bits_ >= 32u ? 4294967295.0f : (float)((1u << bits_) - 1u));   // HLSL masks shift counts: 1u << 32 is 1
}
float snorm_(uint v, uint bits_) {
    int s = (int)(v << (32u - bits_)) >> (32u - bits_);
    if (bits_ <= 16u) {   // |s| / (2^(bits_-1) - 1), exact; the most negative code clamps to -1
        uint a_ = (uint)abs(s);
        float r = a_ >= (1u << (bits_ - 1u)) - 1u ? 1.0f : unorm_div_exact(a_, bits_ - 1u);
        return s < 0 ? -r : r;
    }
    return max((float)s / (float)((1u << (bits_ - 1u)) - 1u), -1.0f);
}
uint conv_(uint raw, uint bits_, uint type) {
    // type (NVB197 NUMERICAL_TYPE): 1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled, 7 float
    if (type == 7u) return bits_ == 16u ? asuint(f16tof32(raw)) : raw;
    if (type == 1u) return asuint(snorm_(raw, bits_));
    if (type == 2u) return asuint(unorm_(raw, bits_));
    int s = (int)(raw << (32u - bits_)) >> (32u - bits_);
    if (type == 3u) return (uint)s;
    if (type == 4u) return raw;
    if (type == 6u) return asuint((float)s);       // SSCALED
    return asuint((float)raw);                       // USCALED
}
// Component layout of each NVB197_SET_VERTEX_ATTRIBUTE_A COMPONENT_BIT_WIDTHS code: bits per component (x,y,z,w).
uint4 vin_layout(uint size) {
    switch (size) {
    case 0x01u: return uint4(32, 32, 32, 32);  case 0x02u: return uint4(32, 32, 32, 0);
    case 0x03u: return uint4(16, 16, 16, 16);  case 0x04u: return uint4(32, 32, 0, 0);
    case 0x05u: return uint4(16, 16, 16, 0);   case 0x0au: return uint4(8, 8, 8, 8);
    case 0x0fu: return uint4(16, 16, 0, 0);    case 0x12u: return uint4(32, 0, 0, 0);
    case 0x13u: return uint4(8, 8, 8, 0);      case 0x18u: return uint4(8, 8, 0, 0);
    case 0x1bu: return uint4(16, 0, 0, 0);     case 0x1du: return uint4(8, 0, 0, 0);
    case 0x30u: return uint4(10, 10, 10, 2);
    case 0x2fu: return uint4(8, 8, 8, 8);      case 0x33u: return uint4(8, 8, 8, 0);   // A8B8G8R8, X8B8G8R8
    case 0x32u: return uint4(8, 8, 0, 0);      case 0x34u: return uint4(8, 0, 0, 0);   // G8R8, A8
    default:    return uint4(0, 0, 0, 0);      // unknown codes are reported by the host before drawing
    }
}
// All four components of generic attribute g; missing components read (0,0,0,1).
// The element's address (stream + index * stride + offset) is byte-granular on the source GPU:
// layouts such as stride 20 with an R8G8B8A8 attribute at offset 10 put elements at 2 (mod 4).
// gmem_load reads the aligned word containing an address, so the element's bytes are assembled
// from the aligned words that cover it (funnel shift by the byte misalignment).
// Element bytes at or past the stream's bound size read 0, per vertex and per instance stream (vbounds, below).
uint4 vin_attr4(uint g, uint vid, uint iid) {
    VertexAttrib a = vertex_attribs[g];
    uint fmt = a.format, size = fmt & 63u, type = (fmt >> 6) & 7u;
    bool integer = type == 3u || type == 4u;
    uint4 v = uint4(0u, 0u, 0u, integer ? 1u : 0x3f800000u);
    if ((fmt >> 31) == 0u) return v;
#ifdef INSTANCE_ID_ZERO_BASED
    // K2 (batch-4): per-instance element = base instance + instance / divisor (GL, Vulkan, NVN), iid counting from 0
    // (translate_game_shaders.py INSTANCE_ID_ZERO_BASED; nxport attribute.rs:506-510). host_instance_base - host_iid_offset()
    // is the draw's base instance (0 when iid already holds it: host_iid_offset). Frequency 0 (divisor 0xffffffff,
    // host_gpu_render_draw.h boot_render_attrib_divisor) reads element `base` for every instance, as Vulkan's divisor 0.
    // The vbounds check below bounds the final element.
    uint index = a.divisor == 0u ? vid : (host_instance_base - host_iid_offset()) + iid / a.divisor;
#else
    uint index = a.divisor == 0u ? vid : (iid / a.divisor);
#endif
    uint rel = index * a.stride + a.offset;                             // element offset from the stream start
    uint2 base = gmem_addr(a.stream_va.x, a.stream_va.y, (int)rel);
    uint4 lay = vin_layout(size);
    uint total = size == 0x31u ? 32u : lay.x + lay.y + lay.z + lay.w;   // element bits
    uint sh = (base.x & 3u) * 8u;                                       // misalignment in bits
    uint need = (sh + total + 31u) >> 5;                                // aligned words covering the element
    uint aw[5];
    [unroll] for (uint k = 0u; k < 5u; ++k) aw[k] = k < need ? gmem_load(base, 4u * k) : 0u;
    uint words[4];
    [unroll] for (uint j = 0u; j < 4u; ++j) words[j] = sh == 0u ? aw[j] : (aw[j] >> sh) | (aw[j + 1u] << (32u - sh));
    // vbounds: the source GPU bounds vertex fetches by the stream limit (NVN BindVertexBuffer size) and reads 0 past it.
    // TOTK draws with base vertices past the bound buffers while meshes stream in (nxport translated.rs:5683-5686:
    // base vertex 2533 into 408-vertex streams); such a draw collapses there, here it read whatever memory follows
    // (spikes on the sky islands). As nxport's zero-padded stream copy (translated.rs:5824-5850: bytes up to the size
    // copied, the rest 0) the bound is per byte: a partly covered element keeps its covered bytes. Everything after is
    // the normal decode: zero bytes give 0 / 0.0 components, and components the format lacks keep (0, 0, 0, 1 / 1.0).
    // One compare per fetch; size 0 (unknown: TOTK_VERTEX_BOUNDS=0 or an older host) makes size - 1 the largest uint.
    uint last = rel + max(total >> 3, 1u) - 1u;                         // the element's last byte (max: 32-bit wrap)
    if (max(rel, last) > a.size - 1u) {
        uint avail = rel < a.size ? a.size - rel : 0u;                  // element bytes inside the bound size
        [unroll] for (uint q = 0u; q < 4u; ++q) {
            uint keep = avail > 4u * q ? avail - 4u * q : 0u;
            words[q] &= keep >= 4u ? 0xffffffffu : (1u << (8u * keep)) - 1u;
        }
    }
    if (size == 0x31u) {   // 11_11_10 float
        uint w = words[0];
        [unroll] for (uint c = 0u; c < 3u; ++c) {
            uint bits_ = c == 2u ? 10u : 11u, x = (w >> (11u * c)) & ((1u << bits_) - 1u);
            uint mant = bits_ == 11u ? (x & 63u) << 17 : (x & 31u) << 18, e = x >> (bits_ - 5u);
            v[c] = e == 0u ? 0u : (((e + 112u) << 23) | mant);
        }
        return v;
    }
    uint bitpos = 0u;
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        uint bits_ = lay[c];
        if (bits_ != 0u) {
            uint raw = bits_ == 32u ? words[bitpos >> 5] : (words[bitpos >> 5] >> (bitpos & 31u)) & ((1u << bits_) - 1u);
            v[c] = conv_(raw, bits_, type);
            bitpos += bits_;
        }
    }
    if ((fmt >> 9) & 1u) v.xz = v.zx;   // BGRA component order
    return v;
}

// ---------------------------------------------------------------- textures (NVN bindless)
Texture1D<float4>        tex1d_[]   : register(t0, space1);
Texture2D<float4>        tex2d_[]   : register(t0, space2);
Texture2DArray<float4>   tex2da_[]  : register(t0, space3);
Texture3D<float4>        tex3d_[]   : register(t0, space4);
TextureCube<float4>      texcube_[] : register(t0, space5);
TextureCubeArray<float4> texcubea_[] : register(t0, space6);
Buffer<float4>           texbuf_[]  : register(t0, space8);   // TEXHEAD_1D_BUFFER views (typed, over guest memory)
Texture1DArray<float4>   tex1da_[]  : register(t0, space9);
SamplerState             samp_[]    : register(s0, space1);
SamplerComparisonState   sampc_[]   : register(s0, space2);

uint tex_handle(uint index) { return cb2[index >> 2][index & 3u]; }   // handle index * 4 bytes in TEX_BANK
#define TIC(h) NonUniformResourceIndex((h) & 0xfffffu)
#define TSC(h) NonUniformResourceIndex((h) >> 20)
float arr_(uint a) { return (float)(a & 0xffffu); }

// TMML: (level accessed, unclamped LOD); implicit derivatives exist only in pixel shaders.
#ifdef STAGE_FRAGMENT
#define TEX_LOD(t, s, c) float2(t.CalculateLevelOfDetail(s, c), t.CalculateLevelOfDetailUnclamped(s, c))
#else
#define TEX_LOD(t, s, c) float2(0.0f, 0.0f)
#endif
float2 tex_lod_1d(uint h, float c) { return TEX_LOD(tex1d_[TIC(h)], samp_[TSC(h)], c); }
float2 tex_lod_2d(uint h, float2 c) { return TEX_LOD(tex2d_[TIC(h)], samp_[TSC(h)], c); }
float2 tex_lod_2da(uint h, float2 c) { return TEX_LOD(tex2da_[TIC(h)], samp_[TSC(h)], c); }
float2 tex_lod_3d(uint h, float3 c) { return TEX_LOD(tex3d_[TIC(h)], samp_[TSC(h)], c); }
float2 tex_lod_cube(uint h, float3 c) { return TEX_LOD(texcube_[TIC(h)], samp_[TSC(h)], c); }
float2 tex_lod_cubea(uint h, float3 c) { return TEX_LOD(texcubea_[TIC(h)], samp_[TSC(h)], c); }
float2 tex_lod_1da(uint h, float c) { return TEX_LOD(tex1da_[TIC(h)], samp_[TSC(h)], c); }

#ifdef STAGE_FRAGMENT
#define IMPLICIT_SAMPLE(t, s, c) t.Sample(s, c)
#define IMPLICIT_CMP(t, s, c, d) t.SampleCmp(s, c, d)
#else
#define IMPLICIT_SAMPLE(t, s, c) t.SampleLevel(s, c, 0.0f)
#define IMPLICIT_CMP(t, s, c, d) t.SampleCmpLevelZero(s, c, d)
#endif
float4 tex_implicit_1d(uint h, float c) { return IMPLICIT_SAMPLE(tex1d_[TIC(h)], samp_[TSC(h)], c); }
float4 tex_level_1d(uint h, float c, float l) { return tex1d_[TIC(h)].SampleLevel(samp_[TSC(h)], c, l); }
float4 tex_implicit_2d(uint h, float2 c) { return IMPLICIT_SAMPLE(tex2d_[TIC(h)], samp_[TSC(h)], c); }
float4 tex_level_2d(uint h, float2 c, float l) { return tex2d_[TIC(h)].SampleLevel(samp_[TSC(h)], c, l); }
float4 tex_bias_2d(uint h, float2 c, float b) {
#ifdef STAGE_FRAGMENT
    return tex2d_[TIC(h)].SampleBias(samp_[TSC(h)], c, b);
#else
    return tex2d_[TIC(h)].SampleLevel(samp_[TSC(h)], c, b);
#endif
}
float4 tex_cmp_2d(uint h, float2 c, float d) { return IMPLICIT_CMP(tex2d_[TIC(h)], sampc_[TSC(h)], c, d).xxxx; }
float4 tex_cmplevel_2d(uint h, float2 c, float d, float l) { return tex2d_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], c, d).xxxx; }
float4 tex_implicit_2da(uint h, float2 c, uint a) { return IMPLICIT_SAMPLE(tex2da_[TIC(h)], samp_[TSC(h)], float3(c, arr_(a))); }
float4 tex_level_2da(uint h, float2 c, uint a, float l) { return tex2da_[TIC(h)].SampleLevel(samp_[TSC(h)], float3(c, arr_(a)), l); }
float4 tex_cmp_2da(uint h, float2 c, uint a, float d) { return IMPLICIT_CMP(tex2da_[TIC(h)], sampc_[TSC(h)], float3(c, arr_(a)), d).xxxx; }
float4 tex_cmplevel_2da(uint h, float2 c, uint a, float d, float l) { return tex2da_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], float3(c, arr_(a)), d).xxxx; }
float4 tex_implicit_3d(uint h, float3 c) { return IMPLICIT_SAMPLE(tex3d_[TIC(h)], samp_[TSC(h)], c); }
float4 tex_level_3d(uint h, float3 c, float l) { return tex3d_[TIC(h)].SampleLevel(samp_[TSC(h)], c, l); }
float4 tex_implicit_cube(uint h, float3 c) { return IMPLICIT_SAMPLE(texcube_[TIC(h)], samp_[TSC(h)], c); }
float4 tex_level_cube(uint h, float3 c, float l) { return texcube_[TIC(h)].SampleLevel(samp_[TSC(h)], c, l); }
float4 tex_cmp_cube(uint h, float3 c, float d) { return IMPLICIT_CMP(texcube_[TIC(h)], sampc_[TSC(h)], c, d).xxxx; }
float4 tex_cmplevel_cube(uint h, float3 c, float d, float l) { return texcube_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], c, d).xxxx; }
float4 tex_implicit_cubea(uint h, float3 c, uint a) { return IMPLICIT_SAMPLE(texcubea_[TIC(h)], samp_[TSC(h)], float4(c, arr_(a))); }
float4 tex_level_cubea(uint h, float3 c, uint a, float l) { return texcubea_[TIC(h)].SampleLevel(samp_[TSC(h)], float4(c, arr_(a)), l); }
float4 tex_cmp_cubea(uint h, float3 c, uint a, float d) { return IMPLICIT_CMP(texcubea_[TIC(h)], sampc_[TSC(h)], float4(c, arr_(a)), d).xxxx; }
float4 tex_cmplevel_cubea(uint h, float3 c, uint a, float d, float l) { return texcubea_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], float4(c, arr_(a)), d).xxxx; }
float4 tex_bias_cube(uint h, float3 c, float b) {
#ifdef STAGE_FRAGMENT
    return texcube_[TIC(h)].SampleBias(samp_[TSC(h)], c, b);
#else
    return texcube_[TIC(h)].SampleLevel(samp_[TSC(h)], c, b);
#endif
}
float4 tex_bias_2da(uint h, float2 c, uint a, float b) {
#ifdef STAGE_FRAGMENT
    return tex2da_[TIC(h)].SampleBias(samp_[TSC(h)], float3(c, arr_(a)), b);
#else
    return tex2da_[TIC(h)].SampleLevel(samp_[TSC(h)], float3(c, arr_(a)), b);
#endif
}
float4 tex_bias_3d(uint h, float3 c, float b) {
#ifdef STAGE_FRAGMENT
    return tex3d_[TIC(h)].SampleBias(samp_[TSC(h)], c, b);
#else
    return tex3d_[TIC(h)].SampleLevel(samp_[TSC(h)], c, b);
#endif
}
float4 tex_bias_cubea(uint h, float3 c, uint a, float b) {
#ifdef STAGE_FRAGMENT
    return texcubea_[TIC(h)].SampleBias(samp_[TSC(h)], float4(c, arr_(a)), b);
#else
    return texcubea_[TIC(h)].SampleLevel(samp_[TSC(h)], float4(c, arr_(a)), b);
#endif
}
// 1D and 1D array (Maxwell array index: low 16 bits of the first coordinate register)
float4 tex_bias_1d(uint h, float c, float b) {
#ifdef STAGE_FRAGMENT
    return tex1d_[TIC(h)].SampleBias(samp_[TSC(h)], c, b);
#else
    return tex1d_[TIC(h)].SampleLevel(samp_[TSC(h)], c, b);
#endif
}
float4 tex_cmp_1d(uint h, float c, float d) { return IMPLICIT_CMP(tex1d_[TIC(h)], sampc_[TSC(h)], c, d).xxxx; }
float4 tex_cmplevel_1d(uint h, float c, float d, float l) { return tex1d_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], c, d).xxxx; }
float4 tex_implicit_1da(uint h, float c, uint a) { return IMPLICIT_SAMPLE(tex1da_[TIC(h)], samp_[TSC(h)], float2(c, arr_(a))); }
float4 tex_level_1da(uint h, float c, uint a, float l) { return tex1da_[TIC(h)].SampleLevel(samp_[TSC(h)], float2(c, arr_(a)), l); }
float4 tex_bias_1da(uint h, float c, uint a, float b) {
#ifdef STAGE_FRAGMENT
    return tex1da_[TIC(h)].SampleBias(samp_[TSC(h)], float2(c, arr_(a)), b);
#else
    return tex1da_[TIC(h)].SampleLevel(samp_[TSC(h)], float2(c, arr_(a)), b);
#endif
}
float4 tex_cmp_1da(uint h, float c, uint a, float d) { return IMPLICIT_CMP(tex1da_[TIC(h)], sampc_[TSC(h)], float2(c, arr_(a)), d).xxxx; }
float4 tex_cmplevel_1da(uint h, float c, uint a, float d, float l) { return tex1da_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], float2(c, arr_(a)), d).xxxx; }
// TEX.AOFFI: texel offsets (4-bit signed per axis, not allowed on cubes). D3D requires them to be
// immediate: the translator passes literals when the offset register holds a constant.
#ifdef STAGE_FRAGMENT
#define IMPLICIT_SAMPLE_O(t, s, c, o) t.Sample(s, c, o)
#define IMPLICIT_CMP_O(t, s, c, d, o) t.SampleCmp(s, c, d, o)
#define BIAS_SAMPLE_O(t, s, c, b, o) t.SampleBias(s, c, b, o)
#else
#define IMPLICIT_SAMPLE_O(t, s, c, o) t.SampleLevel(s, c, 0.0f, o)
#define IMPLICIT_CMP_O(t, s, c, d, o) t.SampleCmpLevelZero(s, c, d, o)
#define BIAS_SAMPLE_O(t, s, c, b, o) t.SampleLevel(s, c, b, o)
#endif
#define TEX_OFFSET_FNS(dim, arr, C, O, coord) \
float4 tex_implicit_##dim(uint h, C c, O o) { return IMPLICIT_SAMPLE_O(arr[TIC(h)], samp_[TSC(h)], coord, o); } \
float4 tex_level_##dim(uint h, C c, float l, O o) { return arr[TIC(h)].SampleLevel(samp_[TSC(h)], coord, l, o); } \
float4 tex_bias_##dim(uint h, C c, float b, O o) { return BIAS_SAMPLE_O(arr[TIC(h)], samp_[TSC(h)], coord, b, o); }
#define TEX_OFFSET_FNS_A(dim, arr, C, O, coord) \
float4 tex_implicit_##dim(uint h, C c, uint a, O o) { return IMPLICIT_SAMPLE_O(arr[TIC(h)], samp_[TSC(h)], coord, o); } \
float4 tex_level_##dim(uint h, C c, uint a, float l, O o) { return arr[TIC(h)].SampleLevel(samp_[TSC(h)], coord, l, o); } \
float4 tex_bias_##dim(uint h, C c, uint a, float b, O o) { return BIAS_SAMPLE_O(arr[TIC(h)], samp_[TSC(h)], coord, b, o); } \
float4 tex_cmp_##dim(uint h, C c, uint a, float d, O o) { return IMPLICIT_CMP_O(arr[TIC(h)], sampc_[TSC(h)], coord, d, o).xxxx; } \
float4 tex_cmplevel_##dim(uint h, C c, uint a, float d, float l, O o) { return arr[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], coord, d, o).xxxx; }
TEX_OFFSET_FNS(1d, tex1d_, float, int, c)
TEX_OFFSET_FNS(2d, tex2d_, float2, int2, c)
TEX_OFFSET_FNS(3d, tex3d_, float3, int3, c)
TEX_OFFSET_FNS_A(1da, tex1da_, float, int, float2(c, arr_(a)))
TEX_OFFSET_FNS_A(2da, tex2da_, float2, int2, float3(c, arr_(a)))
float4 tex_cmp_1d(uint h, float c, float d, int o) { return IMPLICIT_CMP_O(tex1d_[TIC(h)], sampc_[TSC(h)], c, d, o).xxxx; }
float4 tex_cmplevel_1d(uint h, float c, float d, float l, int o) { return tex1d_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], c, d, o).xxxx; }
float4 tex_cmp_2d(uint h, float2 c, float d, int2 o) { return IMPLICIT_CMP_O(tex2d_[TIC(h)], sampc_[TSC(h)], c, d, o).xxxx; }
float4 tex_cmplevel_2d(uint h, float2 c, float d, float l, int2 o) { return tex2d_[TIC(h)].SampleCmpLevelZero(sampc_[TSC(h)], c, d, o).xxxx; }
// Explicit gradients (TXD)
float4 tex_grad_1d(uint h, float c, float dx, float dy) { return tex1d_[TIC(h)].SampleGrad(samp_[TSC(h)], c, dx, dy); }
float4 tex_grad_1da(uint h, float c, uint a, float dx, float dy) { return tex1da_[TIC(h)].SampleGrad(samp_[TSC(h)], float2(c, arr_(a)), dx, dy); }
float4 tex_grad_2d(uint h, float2 c, float2 dx, float2 dy) { return tex2d_[TIC(h)].SampleGrad(samp_[TSC(h)], c, dx, dy); }
float4 tex_grad_2da(uint h, float2 c, uint a, float2 dx, float2 dy) { return tex2da_[TIC(h)].SampleGrad(samp_[TSC(h)], float3(c, arr_(a)), dx, dy); }
// Texel fetches (TLD/TLDS). A 1D fetch reads a TEXHEAD_1D_BUFFER view when the handle's TIC is one
// (header version 0, the TEXHEAD_1D layout of clb097tex.h): the buffer array holds a view
// only for buffer headers, so its element count is 0 otherwise. Offsets are added to the texel
// coordinate (Load's own offset operand needs an immediate).
float4 tex_fetch_1d(uint h, int c, int l) {
    uint n; texbuf_[TIC(h)].GetDimensions(n);
    if (n != 0u) return texbuf_[TIC(h)].Load(c);   // out-of-range elements read 0
    return tex1d_[TIC(h)].Load(int2(c, l));
}
float4 tex_fetch_1d(uint h, int c, int l, int o) { return tex_fetch_1d(h, c + o, l); }
float4 tex_fetch_1da(uint h, int c, uint a, int l) { return tex1da_[TIC(h)].Load(int3(c, (int)(a & 0xffffu), l)); }
float4 tex_fetch_1da(uint h, int c, uint a, int l, int o) { return tex_fetch_1da(h, c + o, a, l); }
float4 tex_fetch_2d(uint h, int2 c, int l) { return tex2d_[TIC(h)].Load(int3(c, l)); }
float4 tex_fetch_2d(uint h, int2 c, int l, int2 o) { return tex2d_[TIC(h)].Load(int3(c, l), o); }
float4 tex_fetch_2da(uint h, int2 c, uint a, int l) { return tex2da_[TIC(h)].Load(int4(c, (int)(a & 0xffffu), l)); }
float4 tex_fetch_2da(uint h, int2 c, uint a, int l, int2 o) { return tex_fetch_2da(h, c + o, a, l); }
float4 tex_fetch_3d(uint h, int3 c, int l) { return tex3d_[TIC(h)].Load(int4(c, l)); }
float4 tex_fetch_3d(uint h, int3 c, int l, int3 o) { return tex_fetch_3d(h, c + o, l); }
#define GATHER(comp, Comp) \
float4 tex_gather##comp##_2d(uint h, float2 c) { return tex2d_[TIC(h)].Gather##Comp(samp_[TSC(h)], c); } \
float4 tex_gather##comp##_2da(uint h, float2 c, uint a) { return tex2da_[TIC(h)].Gather##Comp(samp_[TSC(h)], float3(c, arr_(a))); } \
float4 tex_gather##comp##_cube(uint h, float3 c) { return texcube_[TIC(h)].Gather##Comp(samp_[TSC(h)], c); } \
float4 tex_gather##comp##_cubea(uint h, float3 c, uint a) { return texcubea_[TIC(h)].Gather##Comp(samp_[TSC(h)], float4(c, arr_(a))); }
GATHER(r, Red) GATHER(g, Green) GATHER(b, Blue) GATHER(a, Alpha)
float4 tex_gathercmpr_2d(uint h, float2 c, float d) { return tex2d_[TIC(h)].GatherCmpRed(sampc_[TSC(h)], c, d); }
float4 tex_gathercmpr_2da(uint h, float2 c, uint a, float d) { return tex2da_[TIC(h)].GatherCmpRed(sampc_[TSC(h)], float3(c, arr_(a)), d); }
float4 tex_gathercmpr_cube(uint h, float3 c, float d) { return texcube_[TIC(h)].GatherCmpRed(sampc_[TSC(h)], c, d); }
float4 tex_gathercmpr_cubea(uint h, float3 c, uint a, float d) { return texcubea_[TIC(h)].GatherCmpRed(sampc_[TSC(h)], float4(c, arr_(a)), d); }
// TXQ dimension: (width, height, depth or layers, levels), levels in w as Mesa NAK reads them (nak_nir_lower_tex.c)
// per type -- 1D (w, 0, 0, levels); 1D array (w, layers, 0, levels); 2D and cube (w, h, 0, levels);
// 2D array, cube array (cubes) and 3D (w, h, d, levels); buffer (elements, 0, 0, 0). The texture
// type is not known when the shader is translated: the view lives only in the array of its
// header's type and null views report zero width, so the first non-empty array is the type.
uint4 tex_query_dims(uint h, uint lod) {
    uint w, hgt, d, levels;
    tex2d_[TIC(h)].GetDimensions(lod, w, hgt, levels);
    if (w != 0u) return uint4(w, hgt, 0u, levels);
    tex2da_[TIC(h)].GetDimensions(lod, w, hgt, d, levels);
    if (w != 0u) return uint4(w, hgt, d, levels);
    tex3d_[TIC(h)].GetDimensions(lod, w, hgt, d, levels);
    if (w != 0u) return uint4(w, hgt, d, levels);
    texcube_[TIC(h)].GetDimensions(lod, w, hgt, levels);
    if (w != 0u) return uint4(w, hgt, 0u, levels);
    texcubea_[TIC(h)].GetDimensions(lod, w, hgt, d, levels);
    if (w != 0u) return uint4(w, hgt, d, levels);
    tex1d_[TIC(h)].GetDimensions(lod, w, levels);
    if (w != 0u) return uint4(w, 0u, 0u, levels);
    tex1da_[TIC(h)].GetDimensions(lod, w, d, levels);
    if (w != 0u) return uint4(w, d, 0u, levels);
    texbuf_[TIC(h)].GetDimensions(w);
    return uint4(w, 0u, 0u, 0u);
}
