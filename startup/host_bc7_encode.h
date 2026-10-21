#ifndef TOTK_HOST_BC7_ENCODE_H
#define TOTK_HOST_BC7_ENCODE_H
/* bc7-work: GPU BC7 encoder core (D3D12 compute), independent of the renderer so the standalone harness
 * (play-work/bc7-work/harness/bc7_gpu_test.c) runs exactly what the runtime records.
 *
 * Shaders: bc7-shaders/bc7_encode.hlsl, ported from Microsoft DirectXTex's BC7Encode.hlsl (tag may2026,
 * commit 4feb3e11a020f35b796fc769a74216a555d4f5ef, MIT License, Copyright (c) Microsoft Corporation;
 * THIRD-PARTY-NOTICES.md). The pass sequence below is DirectXTex's GPUCompressBC::Compress for
 * BC7 (BCDirectCompute.cpp): modes 4/5/6, then 1/3/7 (ping-pong between two mode buffers), optionally
 * 0/2, then the block packer. One addition (bc7_encode.hlsl step 6): the 2/3-subset passes skip blocks whose
 * error after the earlier passes is already <= refine_err (0 = DirectXTex's result exactly; see
 * BOOT_BC7_REFINE_ERR for the default and the harness numbers behind it).
 *
 * Interface (the plug-in point for every producer of texels, see host_bc7.h BootBc7Producer):
 *   input  : raw RGBA8 texels (byte 0 = R, sRGB images in their stored sRGB bytes) of one band of block
 *            rows of one image (a mip level of a layer), in a GPU buffer in UNORDERED_ACCESS state. Stage 1
 *            fills it with a copy of the CPU-decoded RGBA8 texture; a GPU ASTC decoder (stage 2) can write
 *            it directly with a UAV and needs only a UAV barrier before boot_bc7_record_band.
 *   output : 16-byte BC7 blocks at block_y * out_pitch_blocks + block_x from the image's first block row
 *            (a GetCopyableFootprints placed footprint, so it copies straight into the BC7 subresource).
 *   scratch: two mode buffers of BOOT_BC7_MODE_BYTES each (UNORDERED_ACCESS).
 * Everything is bound as root UAVs (no descriptor heap; the shader clamps reads and guards writes because
 * root views are not bounds checked). Partial blocks (images or mips not a multiple of 4, down to 1x1)
 * are padded by edge replication. */
#include <stdint.h>
#include <string.h>
#include "host_bc7_dxil.h"

#define BOOT_BC7_BATCH_BLOCKS 32768u                         /* blocks per dispatch (2-subset passes: one group per block, <= 65535) */
#define BOOT_BC7_MODE_BYTES ((uint64_t)BOOT_BC7_BATCH_BLOCKS * 16u)
#define BOOT_BC7_ROOT_CONSTANTS 12u
enum { BOOT_BC7_FLAG_MODE02 = 1u,   /* also try the 3-subset modes 0 and 2 (DirectXTex TEX_COMPRESS_BC7_USE_3SUBSETS) */
       BOOT_BC7_FLAG_QUICK = 2u };  /* modes 4/5/6 only (DirectXTex TEX_COMPRESS_BC7_QUICK) */
enum { BOOT_BC7_PSO_456 = 0, BOOT_BC7_PSO_137, BOOT_BC7_PSO_02, BOOT_BC7_PSO_PACK, BOOT_BC7_PSOS };
#ifndef BOOT_BC7_DISPATCH   /* the runtime routes it through its GPU profiler */
#define BOOT_BC7_DISPATCH(cl, groups) ID3D12GraphicsCommandList_Dispatch((cl), (groups), 1, 1)
#endif

typedef HRESULT (*BootBc7CreatePso)(void *ctx, ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *cd, ID3D12PipelineState **out);
typedef void (*BootBc7RootNote)(const void *root, const void *blob, size_t n);
typedef struct BootBc7Encoder {
    ID3D12Device *device; ID3D12RootSignature *root; ID3D12PipelineState *pso[BOOT_BC7_PSOS];
} BootBc7Encoder;

static inline void boot_bc7_encoder_release(BootBc7Encoder *e) {
    for (unsigned i = 0; i < BOOT_BC7_PSOS; ++i) if (e->pso[i]) ID3D12PipelineState_Release(e->pso[i]);
    if (e->root) ID3D12RootSignature_Release(e->root);
    memset(e, 0, sizeof *e);
}
static inline HRESULT boot_bc7_create_pso_plain(void *ctx, ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *cd, ID3D12PipelineState **out) {
    (void)ctx; return ID3D12Device_CreateComputePipelineState(dev, cd, &IID_ID3D12PipelineState, (void **)out);
}
/* Root signature (b0: 12 constants, u0 output, u1 mode input, u2 texels) and the four pipelines. 1 = ready. */
static inline int boot_bc7_encoder_open(BootBc7Encoder *e, ID3D12Device *dev, BootBc7CreatePso create, void *ctx, BootBc7RootNote note) {
    memset(e, 0, sizeof *e);
    if (!create) create = boot_bc7_create_pso_plain;
    D3D12_ROOT_PARAMETER1 p[4]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = BOOT_BC7_ROOT_CONSTANTS;
    for (unsigned i = 1; i < 4; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[i].Descriptor.ShaderRegister = i - 1; p[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    }
    for (unsigned i = 0; i < 4; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 4; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) { if (err) ID3D10Blob_Release(err); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&e->root);
    if (SUCCEEDED(hr) && note) note(e->root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { e->root = NULL; return 0; }
    static const struct { const uint8_t *code; size_t n; } cs[BOOT_BC7_PSOS] = {
        {boot_bc7_TryMode456CS_dxil, sizeof boot_bc7_TryMode456CS_dxil}, {boot_bc7_TryMode137CS_dxil, sizeof boot_bc7_TryMode137CS_dxil},
        {boot_bc7_TryMode02CS_dxil, sizeof boot_bc7_TryMode02CS_dxil}, {boot_bc7_EncodeBlockCS_dxil, sizeof boot_bc7_EncodeBlockCS_dxil}};
    for (unsigned i = 0; i < BOOT_BC7_PSOS; ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd; memset(&cd, 0, sizeof cd); cd.pRootSignature = e->root;
        cd.CS.pShaderBytecode = cs[i].code; cd.CS.BytecodeLength = cs[i].n;
        if (FAILED(create(ctx, dev, &cd, &e->pso[i])) || !e->pso[i]) { e->pso[i] = NULL; boot_bc7_encoder_release(e); return 0; }
    }
    e->device = dev;
    return 1;
}

/* One band of block rows of one image. */
typedef struct BootBc7Band {
    uint32_t width, height;              /* the image (a mip level of a layer) in texels */
    uint32_t block_row0, block_rows;     /* encodes block rows [block_row0, block_row0 + block_rows) */
    D3D12_GPU_VIRTUAL_ADDRESS pixels;    /* raw RGBA8 rows from texel row block_row0 * 4, `pitch` bytes apart */
    uint32_t pitch;                      /* (rows past the image are never read: reads clamp to height - 1) */
    D3D12_GPU_VIRTUAL_ADDRESS out;       /* the image's block row 0 in the output */
    uint32_t out_pitch_blocks;           /* blocks between output rows (>= ceil(width / 4)) */
} BootBc7Band;
static inline uint32_t boot_bc7_blocks_x(uint32_t w) { return (w + 3) / 4; }
static inline uint32_t boot_bc7_blocks_y(uint32_t h) { return (h + 3) / 4; }
/* The tightly packed payload of a BC7 image (host_bc7_cache.h: per subresource in D3D12 order, sub = level + layer *
 * levels, ceil(h/4) rows of ceil(w/4) * 16 bytes): offset of subresource `sub`, its row bytes and rows. Both the cache
 * write (footprint rows -> tight) and the cache-hit upload (tight -> footprint rows) use it. */
static inline uint64_t boot_bc7_tight_offset(uint32_t w, uint32_t h, uint32_t levels, unsigned sub, uint32_t *row_bytes, uint32_t *rows) {
    const unsigned layer = sub / levels, level = sub % levels;
    uint64_t layer_bytes = 0, off = 0;
    for (unsigned l = 0; l < levels; ++l) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        uint64_t n = (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
        if (l < level) off += n;
        layer_bytes += n;
        if (l == level) { *row_bytes = ((lw + 3) / 4) * 16; *rows = (lh + 3) / 4; }
    }
    return (uint64_t)layer * layer_bytes + off;
}
/* Row pitch of the RGBA8 input buffer for an image `w` texels wide (D3D12 placed footprints: 256-aligned). */
static inline uint32_t boot_bc7_in_pitch(uint32_t w) { return (w * 4u + 255u) & ~255u; }
/* Stage 1 producer step: records the copy of texel rows [y0, y1) of subresource `sub` of the RGBA8 texture
 * `tex` (COPY_SOURCE; `fmt` R8G8B8A8_UNORM or _UNORM_SRGB: the bytes are copied unchanged, so sRGB images are
 * encoded on their sRGB bytes) into `dst` at `dst_off` (COPY_DEST, 512-aligned), rows `pitch` bytes apart. */
static inline void boot_bc7_copy_rows(ID3D12GraphicsCommandList *cl, ID3D12Resource *tex, UINT sub, DXGI_FORMAT fmt, uint32_t width, uint32_t y0, uint32_t y1,
                                      ID3D12Resource *dst, uint64_t dst_off, uint32_t pitch) {
    D3D12_TEXTURE_COPY_LOCATION d, s; memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
    d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Offset = dst_off; d.PlacedFootprint.Footprint.Format = fmt; d.PlacedFootprint.Footprint.Width = width;
    d.PlacedFootprint.Footprint.Height = y1 - y0; d.PlacedFootprint.Footprint.Depth = 1; d.PlacedFootprint.Footprint.RowPitch = pitch;
    s.pResource = tex; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = sub;
    D3D12_BOX box = {0, y0, 0, width, y1, 1};
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &d, 0, 0, 0, &s, &box);
}
/* Records the passes for the band into `cl` (any open direct or compute list; the caller transitions the
 * buffers around it). A UAV barrier separates every pass; the last pass's writes to `out` are complete
 * after the next barrier the caller records. Returns the number of dispatches. Note for callers recording
 * into the renderer's own list (stage 2 inline use): the compute root signature and pipeline are left set
 * to the encoder's. */
static inline unsigned boot_bc7_record_band(const BootBc7Encoder *e, ID3D12GraphicsCommandList *cl, const BootBc7Band *b,
                                            D3D12_GPU_VIRTUAL_ADDRESS mode_a, D3D12_GPU_VIRTUAL_ADDRESS mode_b, unsigned flags, uint32_t refine_err) {
    const uint32_t bx = boot_bc7_blocks_x(b->width), first = b->block_row0 * bx, end = (b->block_row0 + b->block_rows) * bx;
    if (!e->root || !b->width || !b->height || !b->block_rows || end <= first) return 0;
    const float alpha_weight = 1.0f;   /* DirectXTex TEX_ALPHA_WEIGHT_DEFAULT */
    uint32_t aw; memcpy(&aw, &alpha_weight, 4);
    D3D12_RESOURCE_BARRIER uav; memset(&uav, 0, sizeof uav); uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    unsigned n = 0;
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, e->root);
    for (uint32_t start = first; start < end; start += BOOT_BC7_BATCH_BLOCKS) {
        const uint32_t stop = end - start < BOOT_BC7_BATCH_BLOCKS ? end : start + BOOT_BC7_BATCH_BLOCKS, count = stop - start;
        /* {tex_width, num_block_x, format, mode_id, start_block_id, num_total_blocks (batch end), alpha_weight,
         *  tex_height, band_y0, in_pitch, out_pitch_blocks, refine_err} */
        uint32_t c[BOOT_BC7_ROOT_CONSTANTS] = {b->width, bx, 98u, 0, start, stop, aw, b->height, b->block_row0 * 4, b->pitch, b->out_pitch_blocks, refine_err};
        struct { unsigned pso, mode; D3D12_GPU_VIRTUAL_ADDRESS out, in; uint32_t groups; } pass[7]; unsigned np = 0;
        const uint32_t g4 = (count + 3) / 4;   /* 4 blocks per group (16 threads each) */
        pass[np].pso = BOOT_BC7_PSO_456; pass[np].mode = 0; pass[np].out = mode_a; pass[np].in = mode_b; pass[np++].groups = g4;
        D3D12_GPU_VIRTUAL_ADDRESS cur = mode_a, other = mode_b;
        if (!(flags & BOOT_BC7_FLAG_QUICK)) {
            static const unsigned m137[3] = {1, 3, 7};
            for (unsigned i = 0; i < 3; ++i) {   /* 1: a -> b, 3: b -> a, 7: a -> b */
                pass[np].pso = BOOT_BC7_PSO_137; pass[np].mode = m137[i]; pass[np].in = cur; pass[np].out = other; pass[np++].groups = count;
                D3D12_GPU_VIRTUAL_ADDRESS t = cur; cur = other; other = t;
            }
            if (flags & BOOT_BC7_FLAG_MODE02) {
                static const unsigned m02[2] = {0, 2};
                for (unsigned i = 0; i < 2; ++i) {
                    pass[np].pso = BOOT_BC7_PSO_02; pass[np].mode = m02[i]; pass[np].in = cur; pass[np].out = other; pass[np++].groups = count;
                    D3D12_GPU_VIRTUAL_ADDRESS t = cur; cur = other; other = t;
                }
            }
        }
        pass[np].pso = BOOT_BC7_PSO_PACK; pass[np].mode = 0; pass[np].in = cur; pass[np].out = b->out; pass[np++].groups = g4;
        for (unsigned i = 0; i < np; ++i) {
            if (n) ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &uav);
            c[3] = pass[i].mode;
            ID3D12GraphicsCommandList_SetPipelineState(cl, e->pso[pass[i].pso]);
            ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, BOOT_BC7_ROOT_CONSTANTS, c, 0);
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, pass[i].out);
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 2, pass[i].in);
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 3, b->pixels);
            BOOT_BC7_DISPATCH(cl, pass[i].groups);
            ++n;
        }
    }
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &uav);
    return n;
}
#endif
