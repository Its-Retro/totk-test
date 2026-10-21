// batch-2 (nxport-gaps N3+G3): SAMPLES_PASSED reports from D3D12 occlusion queries (host_nvn_occl.h). NVN keeps one running
// sample counter that ResetCounter zeroes and ReportCounter writes (semantics as in nxport crates/engine/src/metal/mod.rs:
// 3430-3437, MIT). Each counted host draw has its own OCCLUSION query; the slots are resolved (one uint64 each) into this
// buffer right before the dispatch. One group of 64 threads adds slots [lo, lo + count) to the running sum at `acc` (or to 0
// when flags bit 0, a reset since the last sum, is set) and stores the new sum. With flags bit 1 (a report) it also stages
// NVNcounterData {u64 sum, u64 timestamp} at `stage` and counts the report, and whether it was 0, in the two words at
// acc + 8; the host copies the staged 16 bytes to the report address in command order. 64-bit values are uint2 {lo, hi}
// with explicit carries (no 64-bit integer shader operations needed). Compiled ahead of time (build.sh) into
// host_nvn_occl_dxil.h; root signature: 8 root constants (b0) and one root UAV (u0, the buffer).
cbuffer Occl : register(b0) { uint lo; uint count; uint flags; uint stage; uint ticks_lo; uint ticks_hi; uint acc; uint pad0; };
RWByteAddressBuffer buf : register(u0);
groupshared uint2 part[64];
uint2 add64(uint2 a, uint2 b) { uint l = a.x + b.x; return uint2(l, a.y + b.y + (l < a.x ? 1u : 0u)); }
[numthreads(64, 1, 1)] void cs_occl_sum(uint t : SV_GroupIndex) {
    uint2 s = uint2(0u, 0u);
    for (uint i = t; i < count; i += 64u) s = add64(s, buf.Load2((lo + i) * 8u));
    part[t] = s;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint w = 32u; w > 0u; w >>= 1) {
        if (t < w) part[t] = add64(part[t], part[t + w]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0u) {
        uint2 sum = part[0];
        if ((flags & 1u) == 0u) sum = add64(sum, buf.Load2(acc));
        buf.Store2(acc, sum);
        if ((flags & 2u) != 0u) {
            buf.Store4(stage, uint4(sum, ticks_lo, ticks_hi));
            uint2 n = buf.Load2(acc + 8u);
            buf.Store2(acc + 8u, uint2(n.x + 1u, n.y + ((sum.x | sum.y) == 0u ? 1u : 0u)));
        }
    }
}
