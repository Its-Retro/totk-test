// render-38c: predicate of nvnCommandBufferSetRenderEnableConditional, evaluated on the GPU (host_nvn_predicate.h).
// Compares the counter words (64 bits each) of the two NVNcounterData at the condition's address (+0 and +16) and
// writes the 64-bit predicate 1 (equal) / 0 (not equal) into the predication ring; the draws and clears under the
// condition are then bracketed with SetPredication(ring, slot, EQUAL_ZERO for RENDER_IF_EQUAL, NOT_EQUAL_ZERO for
// RENDER_IF_NOT_EQUAL), which skips them on the GPU without any CPU readback. Compiled ahead of time (build.sh) into
// host_nvn_predicate_dxil.h; bound with a root signature of 8 root constants (b0) and two root UAVs (u0 the guest
// region at the condition's 256-byte aligned base, u1 the predication ring), the layout of tile-shaders/tile.hlsl.
cbuffer Predicate : register(b0) { uint src_offset; uint dst_offset; uint pad0; uint pad1; uint pad2; uint pad3; uint pad4; uint pad5; };
RWByteAddressBuffer src : register(u0);
RWByteAddressBuffer dst : register(u1);
[numthreads(1, 1, 1)] void cs_predicate(uint3 id : SV_DispatchThreadID) {
    uint2 a = src.Load2(src_offset), b = src.Load2(src_offset + 16u);
    dst.Store2(dst_offset, uint2(all(a == b) ? 1u : 0u, 0u));
}
