// render-38d: arguments of an indirect draw/dispatch moved into D3D12 form on the GPU (host_gpu_indirect.h).
// One 64-thread group per indirect command, recorded right before the command's ExecuteIndirect. It reads the game's
// NVN argument structure from the zero-copy guest region at its place in the queue (after the game's earlier GPU work:
// the culling compute that wrote it) and writes, into the command's slot of the indirect ring:
//   - the D3D12 argument structure (DRAW / DRAW_INDEXED / DISPATCH) the ExecuteIndirect reads: the first arg_copy
//     words of the NVN structure, the rest zero (D3D12 BaseVertexLocation / StartVertexLocation / StartInstanceLocation
//     do not reach SV_VertexID / SV_InstanceID, so first vertex, base vertex and base instance go to HostState instead);
//     dispatch group counts are clamped to D3D12's 65535 per dimension (beyond it ExecuteIndirect is undefined);
//   - a copy of the CPU-staged constant buffer (t0: HostState for draws, driver constant buffer c0 for dispatches) with
//     patch_words words from the NVN structure at patch_word: HostState host_vertex_base/host_instance_base (first
//     vertex or base vertex, base instance) or c0 0x200 group counts. The draw's b18 / the dispatch's c0 CBV is that copy.
// Nothing is decoded on the CPU: no argument value is ever read back. Compiled ahead of time (build.sh) into
// host_gpu_indirect_dxil.h; root signature: 16 root constants (b0), root SRV t0, root UAVs u0 (guest region at the
// arguments' 256-byte aligned base) and u1 (indirect ring).
cbuffer Indirect : register(b0) {
    uint src_offset;    // NVN arguments: byte offset from the u0 base
    uint arg_dst;       // D3D12 argument structure: byte offset in u1
    uint arg_words;     // its size in words (3 dispatch, 4 draw, 5 indexed draw)
    uint arg_copy;      // words taken from the NVN structure (the rest are written 0)
    uint arg_clamp;     // 1: clamp to 65535 (dispatch group counts)
    uint stage_src;     // CPU-staged constants: byte offset in t0
    uint stage_dst;     // their copy: byte offset in u1
    uint stage_words;   // words staged
    uint patch_word;    // first staged word replaced from the NVN structure
    uint patch_src;     // first NVN word copied there
    uint patch_words;   // words replaced
    uint pad0; uint pad1; uint pad2; uint pad3; uint pad4;
};
ByteAddressBuffer stage : register(t0);
RWByteAddressBuffer src : register(u0);
RWByteAddressBuffer dst : register(u1);
uint arg(uint word) { uint v = src.Load(src_offset + 4u * word); return arg_clamp != 0u ? min(v, 65535u) : v; }
[numthreads(64, 1, 1)] void cs_indirect(uint3 id : SV_DispatchThreadID) {
    for (uint k = id.x; k < stage_words; k += 64u)
        dst.Store(stage_dst + 4u * k, k - patch_word < patch_words ? arg(patch_src + k - patch_word) : stage.Load(stage_src + 4u * k));
    if (id.x < arg_words) dst.Store(arg_dst + 4u * id.x, id.x < arg_copy ? arg(id.x) : 0u);
}
