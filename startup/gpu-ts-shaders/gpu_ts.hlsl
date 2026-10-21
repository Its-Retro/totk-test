// G1 (nxport-gaps, TOTK_GPU_TS=gpu): TIMESTAMP reports with the GPU's time (host_nvn_gpu_ts.h). A D3D12 TIMESTAMP query
// taken at the report's place in the command list has been resolved (one uint64, D3D12 timestamp ticks) to `slot` of this
// buffer right before the dispatch. One thread converts it to the GPU timer ticks the game expects (614.4 MHz, 32 x the
// 19.2 MHz system tick) on the host QPC timeline: `base` is that tick value at the calibration point whose D3D12 timestamp
// is `gpu`, and `mul` is 614.4 MHz / timestamp frequency in 32.32 fixed point (mul_int . mul_frac), so
//     ticks = base + floor((ts - gpu) * mul / 2^32)        ((ts - gpu) clamped to [0, 2^32 - 1]).
// The result is kept strictly increasing against the previous one stored at `last` (+1 tick, as the record-time
// values are distinct), stored there, and staged as NVNcounterData {u64 counter = 0, u64 timestamp} at `stage`; the host
// copies the 16 bytes to the report address in command order. 64-bit values are uint2 {lo, hi} with explicit carries,
// and 32 x 32 -> 64 products are built from 16-bit halves (no 64-bit integer shader operations needed). Compiled ahead of
// time (build.sh) into host_nvn_gpu_ts_dxil.h; root signature: 12 root constants (b0) and one root UAV (u0, the buffer).
cbuffer Ts : register(b0) { uint slot; uint stage; uint base_lo; uint base_hi; uint gpu_lo; uint gpu_hi; uint mul_int; uint mul_frac; uint last; uint pad0; uint pad1; uint pad2; };
RWByteAddressBuffer buf : register(u0);
uint2 add64(uint2 a, uint2 b) { uint l = a.x + b.x; return uint2(l, a.y + b.y + (l < a.x ? 1u : 0u)); }
uint2 sub64(uint2 a, uint2 b) { return uint2(a.x - b.x, a.y - b.y - (a.x < b.x ? 1u : 0u)); }
bool less64(uint2 a, uint2 b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }
uint2 mul32(uint a, uint b) {
    uint a0 = a & 0xffffu, a1 = a >> 16, b0 = b & 0xffffu, b1 = b >> 16;
    uint m1 = a1 * b0, m2 = a0 * b1;
    uint2 r = uint2(a0 * b0, a1 * b1);
    r = add64(r, uint2(m1 << 16, m1 >> 16));
    return add64(r, uint2(m2 << 16, m2 >> 16));
}
[numthreads(1, 1, 1)] void cs_gpu_ts() {
    uint2 ts = buf.Load2(slot), g = uint2(gpu_lo, gpu_hi);
    uint d = 0u;
    if (!less64(ts, g)) { uint2 dd = sub64(ts, g); d = dd.y != 0u ? 0xffffffffu : dd.x; }
    uint2 v = add64(uint2(base_lo, base_hi), mul32(d, mul_int));
    v = add64(v, uint2(mul32(d, mul_frac).y, 0u));
    uint2 next = add64(buf.Load2(last), uint2(1u, 0u));
    if (less64(v, next)) v = next;
    buf.Store2(last, v);
    buf.Store4(stage, uint4(0u, 0u, v));
}
