// Block-linear tiling of render-target images on the GPU (host_gpu_render.h boot_render_gt_run):
// render-target write-back (linear rows -> guest memory) and reload (guest memory -> linear rows)
// without a CPU round trip. Same addressing as host_texture_layout.h boot_texture_tile_slice /
// boot_texture_detile_slice: one slice of width_bytes x rows, blocks of 2^log_h GOBs by 2^log_d
// slices, gx GOB columns. One thread per 4-byte word of a linear row. Compiled ahead of time
// (build.sh) into host_gpu_tile_dxil.h; bound with a root signature of 8 root constants (b0) and
// two root UAVs (u0 source, u1 destination, raw buffers at the operation's base addresses).
cbuffer Tile : register(b0) { uint width_bytes; uint rows; uint log_h; uint log_d; uint lin_pitch; uint gx; uint pad0; uint pad1; };
RWByteAddressBuffer src : register(u0);
RWByteAddressBuffer dst : register(u1);
uint tiled_offset(uint x, uint y) {
    uint block = 512u << (log_h + log_d);
    uint oy = (y >> (3u + log_h)) * gx * block + (((y >> 3u) & ((1u << log_h) - 1u)) << 9u) + ((y & 7u) >> 1u) * 64u + (y & 1u) * 16u;
    return oy + (x >> 6u) * block + ((x & 63u) >> 5u) * 256u + ((x & 31u) >> 4u) * 32u + (x & 15u);
}
// Bytes of the word at x that belong to the row (the rest keep the destination's bytes).
uint row_mask(uint x) { uint n = width_bytes - x; return n >= 4u ? 0xffffffffu : (1u << (n * 8u)) - 1u; }
[numthreads(64, 1, 1)] void cs_tile(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 4u, y = id.y;
    if (x >= width_bytes || y >= rows) return;
    uint v = src.Load(y * lin_pitch + x), t = tiled_offset(x, y), m = row_mask(x);
    if (m != 0xffffffffu) v = (dst.Load(t) & ~m) | (v & m);
    dst.Store(t, v);
}
[numthreads(64, 1, 1)] void cs_detile(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 4u, y = id.y;
    if (x >= width_bytes || y >= rows) return;
    dst.Store(y * lin_pitch + x, src.Load(tiled_offset(x, y)));
}
