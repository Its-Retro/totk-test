/* Draw, clear and fixed-function translation for host_gpu_render.h (included at its end).
 * Enum values are NVB197's (gpu-reference/clb197.h); both the OpenGL-style and the
 * D3D-style encodings the class accepts are mapped. Unmappable state stops the draw. */

#include "host_draw_census.h"   /* TOTK_DRAW_CENSUS=1 diagnostic; off = one branch per hook */

/* G2 (batch-4) point sprites: helpers for the draw path below.
 * NVN rasterises a POINTS primitive as a point_size x point_size square centred on the vertex (the size set by
 * nvnCommandBufferSetPointSize, 1.0 until set, unless the vertex shader writes gl_PointSize) and gives the fragment shader
 * gl_PointCoord, 0..1 across the square. nxport gets both from Metal natively (crates/engine/src/metal/mod.rs:2700-2705
 * records SetPointSize; metal/msl_specialize.rs:375-379 passes it as the point size when the vertex shader writes none;
 * Metal's [[point_coord]] has its origin at the upper-left). D3D12 has neither: a point is one pixel, and a pixel shader
 * that reads the point coordinate (maxwell_hlsl.py op_ipa, a[0x2e0]) declares a POINTCOORD input that no vertex shader
 * outputs, so CreateGraphicsPipelineState refuses the pipeline (E_INVALIDARG: all 3584 refused draws in play-94's last
 * boot were NVN mode 0, POINTS) and the draw is lost.
 * Such a draw (topology type POINT, pixel shader input signature with POINTCOORD) gets a geometry shader that emits the
 * square as two triangles: make_point_gs.py writes one per vertex-shader output signature, shader-gs\pt-<FNV-1a 64 of the
 * vertex shader's DXIL OSG1 part, 16 hex>.gs.dxil beside the manifest; it passes every vertex output through unchanged
 * and adds POINTCOORD, (0,0) at the upper-left corner in render-target rows (as Metal; TOTK_POINT_SPRITE=flip starts it
 * at the bottom, for an A/B against a capture). The geometry shader reads its parameters from entry 32 of the draw's
 * vertex attribute table (t2, root table 2, visible to every stage; the per-stage constant tables are not visible to it):
 * point size, viewport half extents in pixels, winding flags (boot_pt_params). A point whose centre is outside the view
 * volume in x/y is not drawn (as on the source GPU; z is clipped by the rasteriser, the four corners share it); the size
 * is clamped to 1..2047 pixels. The pipeline keeps topology type POINT, draws with culling off (a point has no facing:
 * the square is wound front-facing, so SV_IsFrontFace and two-sided stencil see a front face) and its key carries the
 * geometry shader's layout (boot_pt_gs_key), so the pre-creation file and the pipeline library keep it apart.
 * A geometry-shader point expansion is the standard native D3D technique; only semantics come from nxport. Nothing is
 * dropped or substituted: without a geometry shader file the pipeline is created exactly as before (and logged once).
 * TOTK_POINT_SPRITE=0: off, the pipelines of before. */

typedef struct BootPtGs { uint64_t key; void *dxil; size_t size; } BootPtGs;   /* dxil NULL: no file (not retried) */
#define BOOT_PT_GS_MAX 128
static BootPtGs boot_pt_gs[BOOT_PT_GS_MAX]; static unsigned boot_pt_gs_count; static SRWLOCK boot_pt_lock = SRWLOCK_INIT;
static uint32_t boot_pt_size_bits = 0x3f800000u;   /* nvnCommandBufferSetPointSize as last played (float bits), NVN default 1.0 */
static uint64_t boot_pt_draws, boot_pt_missing;    /* point-sprite pipelines built, draws whose layout had no file */

static inline int boot_pt_mode(void) {   /* 0 off, 1 on, 2 on with POINTCOORD.y counted from the bottom */
    static int m = -1;
    if (m < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_POINT_SPRITE", v, sizeof v);
                 m = !(n && n < sizeof v) ? 1 : v[0] == '0' ? 0 : v[0] == 'f' ? 2 : 1; }
    return m;
}
static inline void boot_pt_note_size(uint32_t bits) { boot_pt_size_bits = bits; }   /* host_nvn_play.h: SetPointSize(cmd, float) */

/* One part of a DXIL container ('DXBC', digest, version, size, part count, part offsets; part = fourcc, size, data). */
static inline const uint8_t *boot_pt_part(const void *dxil, size_t n, const char fourcc[4], uint32_t *size) {
    const uint8_t *p = (const uint8_t *)dxil; uint32_t count, off, sz;
    if (!p || n < 32 || memcmp(p, "DXBC", 4)) return NULL;
    memcpy(&count, p + 28, 4);
    if (count > 64 || 32 + 4 * (size_t)count > n) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        memcpy(&off, p + 32 + 4 * i, 4);
        if ((size_t)off + 8 > n) return NULL;
        memcpy(&sz, p + off + 4, 4);
        if (!memcmp(p + off, fourcc, 4) && (size_t)off + 8 + sz <= n) { *size = sz; return p + off + 8; }
    }
    return NULL;
}
/* 1 when the pixel shader's input signature (ISG1: count, element offset, 32-byte elements whose second word is the
 * semantic name's offset in the part) has a POINTCOORD element. */
static inline int boot_pt_reads_coord(const void *ps, size_t n) {
    uint32_t sz, count, eo; const uint8_t *s = boot_pt_part(ps, n, "ISG1", &sz);
    if (!s || sz < 8) return 0;
    memcpy(&count, s, 4); memcpy(&eo, s + 4, 4);
    for (uint32_t i = 0; i < count && (size_t)eo + 32 * (size_t)(i + 1) <= sz; ++i) {
        uint32_t name; memcpy(&name, s + eo + 32 * i + 4, 4);
        if (name < sz && sz - name >= 11 && !memcmp(s + name, "POINTCOORD", 11)) return 1;
    }
    return 0;
}
/* The vertex shader's output layout: FNV-1a 64 of its OSG1 part (make_point_gs.py hashes the same bytes); 0 = none. */
static inline uint64_t boot_pt_layout(const void *vs, size_t n) {
    uint32_t sz; const uint8_t *s = boot_pt_part(vs, n, "OSG1", &sz);
    if (!s) return 0;
    uint64_t h = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < sz; ++i) { h ^= s[i]; h *= UINT64_C(1099511628211); }
    return h ? h : 1;
}
/* The geometry shader for a layout (loaded once; render thread and pre-creation thread). */
static inline const BootPtGs *boot_pt_gs_load(const char *manifest_path, uint64_t key) {
    const BootPtGs *g = NULL;
    AcquireSRWLockShared(&boot_pt_lock);
    for (unsigned i = 0; i < boot_pt_gs_count; ++i) if (boot_pt_gs[i].key == key) { g = &boot_pt_gs[i]; break; }
    ReleaseSRWLockShared(&boot_pt_lock);
    if (g) return g;
    char path[1100]; const char *mf = boot_manifest_file(manifest_path);
    const char *sep = strrchr(mf, '\\'), *sep2 = strrchr(mf, '/'); if (sep2 > sep) sep = sep2;
    snprintf(path, sizeof path, "%.*sshader-gs\\pt-%016llx.gs.dxil", sep ? (int)(sep - mf + 1) : 0, mf, (unsigned long long)key);
    void *data = NULL; size_t n = 0; FILE *f = fopen(path, "rb");
    if (f) {
        long len = -1; if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
        if (len > 0 && len < (1L << 24) && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)len)) != NULL && fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); data = NULL; }
        if (data) n = (size_t)len;
        fclose(f);
    }
    AcquireSRWLockExclusive(&boot_pt_lock);
    for (unsigned i = 0; i < boot_pt_gs_count; ++i) if (boot_pt_gs[i].key == key) { g = &boot_pt_gs[i]; break; }   /* the other thread's */
    if (!g && boot_pt_gs_count < BOOT_PT_GS_MAX) {
        BootPtGs *e = &boot_pt_gs[boot_pt_gs_count++]; e->key = key; e->dxil = data; e->size = n; data = NULL; g = e;
        if (!e->dxil) boot_draw_path_logf("point sprite: no geometry shader %s for this vertex-shader output layout: the POINTS draw keeps its pipeline of before", path);
    }
    ReleaseSRWLockExclusive(&boot_pt_lock);
    free(data);   /* never freed once stored: pipeline jobs keep pointing at it */
    return g;
}
/* The key bytes of a pipeline with this geometry shader (0 = none); appended to the pipeline key. */
static inline uint64_t boot_pt_gs_key(const void *gs) {
    if (!gs) return 0;
    uint64_t k = 0;
    AcquireSRWLockShared(&boot_pt_lock);
    for (unsigned i = 0; i < boot_pt_gs_count; ++i) if (boot_pt_gs[i].dxil == gs) { k = boot_pt_gs[i].key; break; }
    ReleaseSRWLockShared(&boot_pt_lock);
    return k;
}
/* Pipeline description of a draw: a POINT pipeline whose pixel shader reads POINTCOORD gets its geometry shader and no
 * culling. Returns 1 when the geometry shader was set. */
static inline int boot_pt_apply(const char *manifest_path, D3D12_GRAPHICS_PIPELINE_STATE_DESC *d) {
    if (d->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT || !d->PS.pShaderBytecode || d->GS.pShaderBytecode || !boot_pt_mode()) return 0;
    if (!boot_pt_reads_coord(d->PS.pShaderBytecode, d->PS.BytecodeLength)) return 0;
    uint64_t key = boot_pt_layout(d->VS.pShaderBytecode, d->VS.BytecodeLength);
    const BootPtGs *g = key ? boot_pt_gs_load(manifest_path, key) : NULL;
    if (!g || !g->dxil) { ++boot_pt_missing; return 0; }
    d->GS.pShaderBytecode = g->dxil; d->GS.BytecodeLength = g->size;
    d->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ++boot_pt_draws;
    return 1;
}
/* Entry 32 of the vertex attribute table (8 words, read by the geometry shader as a VertexAttrib): point size (float
 * bits), viewport half width and half height in pixels (|scale|, boot_render_viewport_map), flags: bit 0 the pipeline's
 * front face is counter-clockwise, bit 1 POINTCOORD.y from the bottom. */
static inline void boot_pt_params(uint32_t e[8], const BootDrawState *s, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d) {
    float hx = s->viewport[0].scale[0], hy = s->viewport[0].scale[1];
    if (hx < 0) hx = -hx;
    if (hy < 0) hy = -hy;
    e[0] = boot_pt_size_bits; memcpy(&e[1], &hx, 4); memcpy(&e[2], &hy, 4);
    e[3] = (d->RasterizerState.FrontCounterClockwise ? 1u : 0u) | (boot_pt_mode() == 2 ? 2u : 0u);
}

/* Draw trace (diagnostics, off unless frames\draw-trace.request arms it in host_gpu_draw_exec.h):
 * while `active`, boot_render_draw brackets its draw with a pipeline-statistics query (slot
 * `query`, resolved into `rb`) and records what it bound. */
#define BOOT_RENDER_TRACE_QUERIES 1024
#define BOOT_RENDER_SO_PROBES 128
#define BOOT_RENDER_SO_SLOT 4096    /* bytes per captured draw: filled-size word, then up to 255 float4 positions */
typedef struct BootRenderTrace {
    int active, drawn; unsigned query; ID3D12QueryHeap *qh; ID3D12Resource *rb;
    uint8_t vs[32], ps[32]; D3D12_VIEWPORT vp; D3D12_RECT sc; uint32_t draw_count, indexed, topo, ps_override;
    uint32_t write_mask[8], blend_enable[8], rt_index[8], zt_index;
    uint32_t rt_w[8], rt_h[8], rt_state[8], rt_fmt[8], pso_rtfmt[8], pso_samples, pso_cull, pso_front, pso_dsvfmt;   /* bound resources and PSO as recorded */
    void *const_ps; size_t const_ps_size; int const_ps_on;   /* bisect: every pixel shader replaced by constant magenta */
    /* position capture: a traced draw is repeated with stream output of SV_Position (rasterizer off) into
     * slot so_count of so_buf, then copied to so_rb; so_last is that slot or -1 */
    ID3D12Resource *so_buf, *so_rb, *so_zero; ID3D12PipelineState *so_pso[BOOT_RENDER_SO_PROBES]; unsigned so_count; int so_last;
    /* textures the traced draw bound (boot_render_draw_trace_tex): handle, TIC header, host slot and
     * resource, and how much of the guest data at layer 0 / the middle layer is nonzero */
    char tex_info[2048]; size_t tex_len;
} BootRenderTrace;
static BootRenderTrace boot_render_trace;

static inline D3D12_COMPARISON_FUNC boot_render_cmp(uint32_t v) {
    if (v >= 0x200 && v <= 0x207) return (D3D12_COMPARISON_FUNC)(v - 0x200 + 1);   /* OGL_NEVER.. */
    if (v >= 1 && v <= 8) return (D3D12_COMPARISON_FUNC)v;                          /* D3D_NEVER.. */
    return (D3D12_COMPARISON_FUNC)0;
}
/* render-37: SPH word 18 declares the colour exports, as Program.ps_outputs() in the
 * offline translator. Read the exact byte snapshot used for the program's identity. Unknown or
 * truncated metadata preserves the old masks; a known zero map is a depth/UAV-only PS.
 * LASRecomp masks absent fragment outputs too (vulkan/exec.c, k.write_mask): otherwise
 * NVIDIA can overwrite bound attachments the source shader never exports. */
static inline uint32_t boot_render_ps_output_map(const void *program, size_t size) {
    uint32_t outputs = UINT32_MAX;
    if (program && size >= 0x50) memcpy(&outputs, (const uint8_t *)program + 0x48, sizeof outputs);
    return outputs;
}
static inline uint8_t boot_render_ps_write_mask(uint32_t outputs, unsigned slot, uint32_t channel_mask) {
    return slot < 8 && ((outputs >> (slot * 4)) & 15u) ? (uint8_t)(channel_mask & 15u) : 0;
}

static inline D3D12_BLEND_OP boot_render_blend_op(uint32_t v) {
    switch (v) {
        case 0x8006: case 1: return D3D12_BLEND_OP_ADD;
        case 0x800a: case 2: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800b: case 3: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: case 4: return D3D12_BLEND_OP_MIN;
        case 0x8008: case 5: return D3D12_BLEND_OP_MAX;
        default: return (D3D12_BLEND_OP)0;
    }
}
static inline D3D12_BLEND boot_render_blend(uint32_t v, int alpha) {
    switch (v) {
        case 0x4000: case 1: return D3D12_BLEND_ZERO;
        case 0x4001: case 2: return D3D12_BLEND_ONE;
        case 0x4300: case 3: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x4301: case 4: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x4302: case 5: return D3D12_BLEND_SRC_ALPHA;
        case 0x4303: case 6: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x4304: case 7: return D3D12_BLEND_DEST_ALPHA;
        case 0x4305: case 8: return D3D12_BLEND_INV_DEST_ALPHA;
        case 0x4306: case 9: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x4307: case 10: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x4308: case 11: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0xc001: case 14: case 0x61: return D3D12_BLEND_BLEND_FACTOR;      /* 0x61..0x64: NVN constant factors (native path) */
        case 0xc002: case 15: case 0x62: return D3D12_BLEND_INV_BLEND_FACTOR;
        case 0xc900: case 16: return alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
        case 0xc901: case 17: return alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
        case 0xc902: case 18: return D3D12_BLEND_SRC1_ALPHA;
        case 0xc903: case 19: return D3D12_BLEND_INV_SRC1_ALPHA;
        /* constant alpha (OGL only): D3D12's alpha factor; boot_render_draw falls back to a
         * blend factor replicating the constant's alpha where the device lacks it */
        case 0xc003: case 0x63: return D3D12_BLEND_ALPHA_FACTOR;
        case 0xc004: case 0x64: return D3D12_BLEND_INV_ALPHA_FACTOR;
        default: return (D3D12_BLEND)0;
    }
}
static inline D3D12_STENCIL_OP boot_render_stencil_op(uint32_t v) {
    switch (v) {   /* OGL: KEEP 0x1e00, ZERO 0, REPLACE 0x1e01, INCR 0x1e02, DECR 0x1e03, INVERT 0x150a, INCR_WRAP 0x8507, DECR_WRAP 0x8508; D3D 1..8 */
        case 0x1e00: case 1: return D3D12_STENCIL_OP_KEEP;
        case 0: case 2: return D3D12_STENCIL_OP_ZERO;
        case 0x1e01: case 3: return D3D12_STENCIL_OP_REPLACE;
        case 0x1e02: case 4: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1e03: case 5: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150a: case 6: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: case 7: return D3D12_STENCIL_OP_INCR;
        case 0x8508: case 8: return D3D12_STENCIL_OP_DECR;
        default: return (D3D12_STENCIL_OP)0;
    }
}
static inline int boot_render_zt_format(uint32_t z, DXGI_FORMAT *f) {
    switch (z) {   /* NVB197_SET_ZT_FORMAT_V_* */
        case 0x13: *f = DXGI_FORMAT_D16_UNORM; return 1;
        case 0x14: case 0x15: case 0x16: *f = DXGI_FORMAT_D24_UNORM_S8_UINT; return 1;
        case 0x0a: *f = DXGI_FORMAT_D32_FLOAT; return 1;
        case 0x19: *f = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; return 1;
        default: return 0;
    }
}

/* Viewport (NVB197 VIEWPORT_SCALE/OFFSET): window = offset + scale * ndc per axis, window y counted
 * in rows from the top of the surface (WINDOW_ORIGIN upper left), so scale_y < 0 puts ndc +y at the
 * top, as D3D12's own viewport transform (y_w = TopLeftY + (1 - ndc_y) * Height / 2) does. D3D12 needs
 * Height > 0: scale_y < 0 maps directly (Height = -2 scale_y); scale_y > 0 has the vertex shader negate
 * clip y (HostState host_flip_y, maxwell_runtime.hlsli finish_vs) and Height = 2 scale_y. Either way
 * the D3D12 window position equals the source GPU's (a Vulkan backend gets the same transform from a
 * negative viewport height, VK_KHR_maintenance1). */
static inline int boot_render_viewport_flip(float scale_y) { return scale_y > 0; }
static inline void boot_render_viewport_map(const BootViewport *v, D3D12_VIEWPORT *vp) {
    float sy = boot_render_viewport_flip(v->scale[1]) ? v->scale[1] : -v->scale[1];
    vp->Width = 2.0f * v->scale[0]; vp->TopLeftX = v->offset[0] - v->scale[0];
    vp->Height = 2.0f * sy; vp->TopLeftY = v->offset[1] - sy;
    vp->MinDepth = 0; vp->MaxDepth = 1;
}
/* FRONT_FACE (OGL CW 0x900 / CCW 0x901) in window space, swapped by WINDOW_ORIGIN.flip_y. D3D12's
 * FrontCounterClockwise is the same window-space winding (the viewport above keeps window positions). */
static inline BOOL boot_render_front_ccw(uint32_t front_face, uint32_t flip_y) { return (front_face == 0x901) != (flip_y != 0); }

/* Vertex attribute table divisor (maxwell_runtime.hlsli vin_attr4: 0 = index by vertex, otherwise
 * base instance + instance / divisor; K2 (batch-4), before it (base + instance) / divisor). VERTEX_STREAM_A_FREQUENCY
 * is used only for streams with SET_VERTEX_STREAM_INSTANCE_A.IS_INSTANCED set
 * (Ryujinx StateUpdater.UpdateVertexBufferState: instanced ? Divisor : 0); a per-instance stream
 * with frequency 0 reads the same element for every instance (encoded as the largest divisor: element `base`, or 0
 * with shaders translated before K2). Games leave stale frequency values (e.g. 1) on per-vertex streams. */
static inline uint32_t boot_render_attrib_divisor(const BootVertexStream *st) {
    if (!st->per_instance) return 0;
    return st->divisor ? st->divisor : 0xffffffffu;
}
/* K2 (batch-4): HostState host_instance_offset (word 8), read only by vertex shaders translated with K2
 * (maxwell_runtime.hlsli host_iid_offset): the instances before this host draw (TOTK_INSTANCE_EXPAND, else 0), so
 * InstanceId counts from 0 without the base instance and a per-instance attribute reads base + instance / divisor, as on
 * the source GPU; host_instance_base (word 5) keeps base instance + it, as before (indirect draws: the GPU-written base).
 * TOTK_INSTANCE_ID_ZERO_BASED=0 writes 0xffffffff: those shaders then add the base instance to InstanceId and read
 * (base + instance) / divisor, exactly as before. Shaders translated before K2 never read the word. */
static inline uint32_t boot_render_iid_offset(uint32_t before) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_INSTANCE_ID_ZERO_BASED", v, sizeof v); on = !(k && k < sizeof v && v[0] == '0'); }
    return on ? before : 0xffffffffu;
}
/* vbounds: vertex attribute table size word (maxwell_runtime.hlsli VertexAttrib.size): the bytes bound from the stream's
 * start address (NVN BindVertexBuffer size, host_nvn_exec.h boot_nvn_st_vertex; VERTEX_STREAM_LIMIT + 1 - address).
 * vin_attr4 reads element bytes at or past it as 0, as the source GPU does past the stream limit: TOTK draws with base
 * vertices beyond the bound buffers while meshes stream in, which the source GPU collapses and we drew as spikes from
 * the memory behind the stream. 0 = unknown, no bound (also a limit below the address, and sizes above 4 GiB - 16, where
 * the shader's 32-bit element offset could wrap inside the bound). TOTK_VERTEX_BOUNDS=0: 0 for every stream, the
 * unbounded fetch of before (read once). */
static inline int boot_render_vertex_bounds(void) {
    static int v = -1;
    if (v < 0) { char t[8]; DWORD k = GetEnvironmentVariableA("TOTK_VERTEX_BOUNDS", t, sizeof t); v = !(k && k < sizeof t && t[0] == '0'); }
    return v;
}
static inline uint32_t boot_render_stream_size(const BootVertexStream *st) {
    if (!st->enabled || st->limit < st->address || st->limit - st->address >= UINT64_C(0xfffffff0)) return 0;
    return (uint32_t)(st->limit - st->address + 1);
}

/* Depth targets share the target cache; their format field holds 0x10000 | ZT format. */
/* A null RTV (R8G8B8A8, 2D) in a module-static heap: render target slots with format NONE. */
static ID3D12DescriptorHeap *boot_render_null_rtv_heap;
static inline D3D12_CPU_DESCRIPTOR_HANDLE boot_render_null_rtv(BootRender *r) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = {0};
    if (!boot_render_null_rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(ID3D12Device_CreateDescriptorHeap(r->gpu->device, &hd, &IID_ID3D12DescriptorHeap, (void **)&boot_render_null_rtv_heap))) { boot_render_null_rtv_heap = NULL; return h; }
        D3D12_RENDER_TARGET_VIEW_DESC v; memset(&v, 0, sizeof v);
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM; v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        boot_render_null_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(boot_render_null_rtv_heap, &h);
        ID3D12Device_CreateRenderTargetView(r->gpu->device, NULL, &v, h);
    }
    boot_render_null_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(boot_render_null_rtv_heap, &h);
    return h;
}
static inline BootRenderTarget *boot_render_depth(BootRender *r, const BootDepthTarget *z) {
    DXGI_FORMAT fmt;
    if (!boot_render_zt_format(z->format, &fmt) || !z->width || !z->height) return NULL;
    BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned i = boot_render_rt_memo_find(T, z->address, z->width, z->height, 0x10000u | z->format); i < T->count; ++i) {   /* batch-b */
        BootRenderTarget *x = &T->rts[i];
        if (x->texture && x->address == z->address && x->width == z->width && x->height == z->height && x->format == (0x10000u | z->format)) {
            boot_render_rt_memo_put(i); boot_render_rt_touch(T, i, z->log_gob_h, z->log_gob_d, 1); return x; }
    }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0}; D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = z->width; d.Height = z->height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = boot_render_depth_resource_format(fmt); d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    int slot = boot_render_rt_slot(r, boot_render_rt_bytes(&d));
    if (slot < 0) return NULL;
    BootRenderTarget *x = &T->rts[slot];
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = fmt; cv.DepthStencil.Depth = 1.0f;
    D3D12_RESOURCE_STATES spare_state; ID3D12Resource *spare = boot_render_rt_spare_take(&d, &spare_state);
    if (spare) {
        x->texture = spare; x->state = spare_state;
        if (!boot_render_begin(r)) { boot_render_rt_spare_put(spare, spare_state); x->texture = NULL; return NULL; }
        boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    } else if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                    &IID_ID3D12Resource, (void **)&x->texture))) { x->texture = NULL; return NULL; }
    x->address = z->address; x->width = z->width; x->height = z->height; x->format = 0x10000u | z->format; x->dxgi = fmt;
    x->state = D3D12_RESOURCE_STATE_DEPTH_WRITE; x->rtv = (uint32_t)slot;
    boot_render_rt_placed(T, (unsigned)slot, &d);
    boot_render_dsv_create(r, x);
    if (spare)   /* contents of a new committed resource: depth 0, stencil 0 */
        BOOT_GP_CLEAR_DSV("new depth target", r->cl, boot_render_dsv(r, x),
                                                        (D3D12_CLEAR_FLAGS)(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 0.0f, 0, 0, NULL);
    boot_render_rt_touch(T, (unsigned)slot, z->log_gob_h, z->log_gob_d, 1);
    return x;
}

/* Guest GPU VA -> GPU VA inside one region buffer (the range must be contiguous). */
static inline int boot_render_resolve(BootRender *r, uint64_t va, uint64_t size, D3D12_GPU_VIRTUAL_ADDRESS *out) {
    if ((va >> 12) < r->va_base_page || !size) return 0;
    uint64_t page = (va >> 12) - r->va_base_page;
    if (page >= r->ptable_pages) return 0;
    uint32_t e = r->ptable_cpu[page]; if (e == 0xffffffffu) return 0;
    unsigned region = e >> 24; uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    if (region >= r->region_count || off + size > r->region_size[region]) return 0;
    for (uint64_t p = 1; p <= ((va & 0xfff) + size - 1) >> 12; ++p)
        if (page + p >= r->ptable_pages || r->ptable_cpu[page + p] != e + (uint32_t)p) return 0;
    *out = ID3D12Resource_GetGPUVirtualAddress(r->region[region]) + off;
    return 1;
}

/* Quads, fans, polygons, quad strips and line loops -> lists/strips in the ring.
 * Returns the expanded index count (0 on failure). */
static inline uint32_t boot_render_expand(BootRender *r, const BootDrawState *s, const uint8_t *src, uint32_t idx_bytes,
                                          D3D12_INDEX_BUFFER_VIEW *ib, D3D_PRIMITIVE_TOPOLOGY *topo) {
    uint32_t n = s->count, out_n = 0;
    switch (s->topology) {
        case 7: out_n = n / 4 * 6; *topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case 6: case 9: out_n = n >= 3 ? (n - 2) * 3 : 0; *topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case 8: out_n = n >= 4 ? (n - 2) / 2 * 6 : 0; *topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case 2: out_n = n >= 2 ? n + 1 : 0; *topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        default: return 0;
    }
    uint64_t bytes = ((uint64_t)out_n * 4 + 255) & ~(uint64_t)255;
    if (!out_n || r->ring_used + bytes > r->ring_size) return 0;
    uint32_t *o = (uint32_t *)(r->ring_cpu + r->ring_used), k = 0;
#define SRC(i) (src ? (idx_bytes == 1 ? (uint32_t)src[(i)] : idx_bytes == 2 ? (uint32_t)((const uint16_t *)src)[(i)] : ((const uint32_t *)src)[(i)]) : (uint32_t)(i))
    if (s->topology == 7) {
        for (uint32_t q = 0; q + 3 < n; q += 4) { o[k++] = SRC(q); o[k++] = SRC(q + 1); o[k++] = SRC(q + 2); o[k++] = SRC(q); o[k++] = SRC(q + 2); o[k++] = SRC(q + 3); }
    } else if (s->topology == 6 || s->topology == 9) {
        for (uint32_t i = 1; i + 1 < n; ++i) { o[k++] = SRC(0); o[k++] = SRC(i); o[k++] = SRC(i + 1); }
    } else if (s->topology == 8) {
        for (uint32_t i = 0; i + 3 < n; i += 2) { o[k++] = SRC(i); o[k++] = SRC(i + 1); o[k++] = SRC(i + 3); o[k++] = SRC(i); o[k++] = SRC(i + 3); o[k++] = SRC(i + 2); }
    } else {
        for (uint32_t i = 0; i < n; ++i) o[k++] = SRC(i);
        o[k++] = SRC(0);
    }
#undef SRC
    ib->BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used; ib->SizeInBytes = k * 4; ib->Format = DXGI_FORMAT_R32_UINT;
    r->ring_used += bytes;
    return k;
}

/* Ring descriptors per draw: 19 vertex CBVs, 19 pixel CBVs, page table and vertex attribute SRVs. */
#define BOOT_RENDER_DRAW_DESCRIPTORS 40
#define BOOT_RENDER_PSO_KEY (64 + sizeof(D3D12_BLEND_DESC) + sizeof(D3D12_RASTERIZER_DESC) + sizeof(D3D12_DEPTH_STENCIL_DESC) + 64)
static uint8_t boot_render_pso_keys[BOOT_RENDER_MAX_PSO][BOOT_RENDER_PSO_KEY]; static uint32_t boot_render_pso_key_size[BOOT_RENDER_MAX_PSO];
static const BootRender *boot_render_pso_owner;
/* explode-work2: a pipeline D3D12 refuses (CreateGraphicsPipelineState fails) is remembered by its key hash and not
 * re-created on every later draw (each refused call costs a full pipeline compile); the first refusal of each key logs the
 * shader pair and the fixed-function state, so the bad pair can be found. TOTK_PSO_BAD_CACHE=0 retries every draw (as before). */
#define BOOT_RENDER_PSO_BAD 256
static uint64_t boot_render_pso_bad_key[BOOT_RENDER_PSO_BAD]; static HRESULT boot_render_pso_bad_hr[BOOT_RENDER_PSO_BAD];
static unsigned boot_render_pso_bad_count; static uint64_t boot_render_pso_bad_hits;
static inline int boot_render_pso_bad_on(void) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_PSO_BAD_CACHE", v, sizeof v); on = !(n && n < sizeof v && v[0] == '0'); }
    return on;
}
static inline HRESULT boot_render_pso_bad_find(uint64_t k0) {
    if (!boot_render_pso_bad_on()) return S_OK;
    for (unsigned i = 0; i < boot_render_pso_bad_count; ++i)
        if (boot_render_pso_bad_key[i] == k0) { ++boot_render_pso_bad_hits; return boot_render_pso_bad_hr[i]; }
    return S_OK;
}
static inline void boot_render_pso_bad_add(uint64_t k0, HRESULT hr, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, const uint8_t *vs_id, const uint8_t *ps_id) {
    for (unsigned i = 0; i < boot_render_pso_bad_count; ++i) if (boot_render_pso_bad_key[i] == k0) return;
    if (boot_render_pso_bad_count == BOOT_RENDER_PSO_BAD) return;   /* full: later refusals retry and are not logged */
    boot_render_pso_bad_key[boot_render_pso_bad_count] = k0; boot_render_pso_bad_hr[boot_render_pso_bad_count++] = hr;
    char vs[65], ps[65]; for (int i = 0; i < 32; ++i) { snprintf(vs + 2 * i, 3, "%02x", vs_id[i]); snprintf(ps + 2 * i, 3, "%02x", ps_id[i]); }
    const D3D12_BLEND_DESC *b = &d->BlendState; const D3D12_RASTERIZER_DESC *ra = &d->RasterizerState; const D3D12_DEPTH_STENCIL_DESC *z = &d->DepthStencilState;
    unsigned blend = 0, logic = 0; for (unsigned i = 0; i < 8; ++i) { blend |= (b->RenderTarget[i].BlendEnable ? 1u : 0u) << i; logic |= (b->RenderTarget[i].LogicOpEnable ? 1u : 0u) << i; }
    boot_draw_path_logf("pso refused #%u hr=0x%08lx: vs %s ps %s (vs %zu bytes, ps %zu bytes) | topo type %u strip cut %u | rts %u fmt %u %u %u %u %u %u %u %u dsv %u samples %u/%u"
        " | a2c %u independent %u blend mask %02x logic mask %02x op %u rt0 write %x | fill %u cull %u ccw %u clip %u ms %u aa-line %u conservative %u bias %d/%g/%g"
        " | depth %u write %u func %u stencil %u read %02x write %02x",
        boot_render_pso_bad_count, (unsigned long)hr, vs, ps, (size_t)d->VS.BytecodeLength, (size_t)d->PS.BytecodeLength,
        (unsigned)d->PrimitiveTopologyType, (unsigned)d->IBStripCutValue, d->NumRenderTargets,
        (unsigned)d->RTVFormats[0], (unsigned)d->RTVFormats[1], (unsigned)d->RTVFormats[2], (unsigned)d->RTVFormats[3],
        (unsigned)d->RTVFormats[4], (unsigned)d->RTVFormats[5], (unsigned)d->RTVFormats[6], (unsigned)d->RTVFormats[7],
        (unsigned)d->DSVFormat, d->SampleDesc.Count, d->SampleDesc.Quality,
        b->AlphaToCoverageEnable, b->IndependentBlendEnable, blend, logic, (unsigned)b->RenderTarget[0].LogicOp, (unsigned)b->RenderTarget[0].RenderTargetWriteMask,
        (unsigned)ra->FillMode, (unsigned)ra->CullMode, ra->FrontCounterClockwise, ra->DepthClipEnable, ra->MultisampleEnable, ra->AntialiasedLineEnable,
        (unsigned)ra->ConservativeRaster, ra->DepthBias, (double)ra->DepthBiasClamp, (double)ra->SlopeScaledDepthBias,
        z->DepthEnable, (unsigned)z->DepthWriteMask, (unsigned)z->DepthFunc, z->StencilEnable, z->StencilReadMask, z->StencilWriteMask);
}
/* Hash index over the cache (open addressing on the FNV key; slot = cache index + 1, 0 = empty). */
#define BOOT_RENDER_PSO_INDEX (BOOT_RENDER_MAX_PSO * 2)
static uint32_t boot_render_pso_index[BOOT_RENDER_PSO_INDEX];
/* A full PSO cache is recycled instead of failing the submission ("pso cache full" stopped the game
 * once a long session had created BOOT_RENDER_MAX_PSO pipelines): its pipelines move to a retired
 * list, the cache restarts empty (refilled on demand), and the retired pipelines, which recorded or
 * executing command lists may still use, are released once the queue is idle
 * (boot_render_pso_release_retired, after the waiting submission in host_gpu_draw_exec.h). */
static ID3D12PipelineState *boot_render_pso_retired[BOOT_RENDER_MAX_PSO]; static unsigned boot_render_pso_retired_count;
static uint64_t boot_render_pso_recycles;
/* play (44): pipelines recorded by earlier boots (pso-keys.bin), recreated in the background */
#define BOOT_RENDER_PSO_PRE 16384u
typedef struct { uint64_t h; uint32_t k; uint8_t *key; ID3D12PipelineState *pso; } BootRenderPsoPre;
static BootRenderPsoPre boot_render_pso_pre[BOOT_RENDER_PSO_PRE]; static SRWLOCK boot_render_pso_pre_lock = SRWLOCK_INIT;
static volatile LONG boot_render_pso_pre_started; static volatile LONG boot_render_pso_pre_made, boot_render_pso_pre_failed;
static uint64_t boot_render_pso_pre_taken;
typedef struct { uint32_t magic, k, vs_mask, has_ps, nrt; } BootRenderPsoRecord;   /* then k key bytes */
static inline uint64_t boot_render_pso_hash(const uint8_t *b, size_t k) {
    uint64_t h = UINT64_C(1469598103934665603); for (size_t i = 0; i < k; ++i) { h ^= b[i]; h *= UINT64_C(1099511628211); } return h ? h : 1;
}
static inline ID3D12PipelineState *boot_render_pso_pre_take(const uint8_t *buf, size_t k) {
    if (!boot_render_pso_pre_started) return NULL;
    uint64_t h = boot_render_pso_hash(buf, k); ID3D12PipelineState *p = NULL;
    AcquireSRWLockExclusive(&boot_render_pso_pre_lock);
    for (uint32_t i = (uint32_t)h & (BOOT_RENDER_PSO_PRE - 1), n = 0; n < BOOT_RENDER_PSO_PRE && boot_render_pso_pre[i].h; ++n, i = (i + 1) & (BOOT_RENDER_PSO_PRE - 1))
        if (boot_render_pso_pre[i].h == h && boot_render_pso_pre[i].k == k && boot_render_pso_pre[i].pso && !memcmp(boot_render_pso_pre[i].key, buf, k)) {
            p = boot_render_pso_pre[i].pso; boot_render_pso_pre[i].pso = NULL; break; }
    ReleaseSRWLockExclusive(&boot_render_pso_pre_lock);
    if (p) ++boot_render_pso_pre_taken;
    return p;
}
/* 1 when the key is already in the table (made or taken): the thread skips it */
static inline int boot_render_pso_pre_put(const uint8_t *key, uint32_t k, ID3D12PipelineState *pso) {
    uint64_t h = boot_render_pso_hash(key, k); int dup = 0;
    AcquireSRWLockExclusive(&boot_render_pso_pre_lock);
    uint32_t i = (uint32_t)h & (BOOT_RENDER_PSO_PRE - 1), n = 0;
    for (; n < BOOT_RENDER_PSO_PRE && boot_render_pso_pre[i].h; ++n, i = (i + 1) & (BOOT_RENDER_PSO_PRE - 1))
        if (boot_render_pso_pre[i].h == h && boot_render_pso_pre[i].k == k && !memcmp(boot_render_pso_pre[i].key, key, k)) { dup = 1; break; }
    /* a taken entry (pso NULL) is refilled, so a parallel-compile worker's result reaches the draw waiting for it */
    if (dup && pso && !boot_render_pso_pre[i].pso) { boot_render_pso_pre[i].pso = pso; pso = NULL; }
    if (!dup && n < BOOT_RENDER_PSO_PRE - 1 && pso) {
        uint8_t *c = (uint8_t *)malloc(k);
        if (c) { memcpy(c, key, k); boot_render_pso_pre[i].h = h; boot_render_pso_pre[i].k = k; boot_render_pso_pre[i].key = c; boot_render_pso_pre[i].pso = pso; pso = NULL; }
    }
    ReleaseSRWLockExclusive(&boot_render_pso_pre_lock);
    if (pso) ID3D12PipelineState_Release(pso);
    return dup;
}
static inline int boot_render_pso_pre_known(const uint8_t *key, uint32_t k) {
    uint64_t h = boot_render_pso_hash(key, k); int found = 0;
    AcquireSRWLockShared(&boot_render_pso_pre_lock);
    for (uint32_t i = (uint32_t)h & (BOOT_RENDER_PSO_PRE - 1), n = 0; n < BOOT_RENDER_PSO_PRE && boot_render_pso_pre[i].h; ++n, i = (i + 1) & (BOOT_RENDER_PSO_PRE - 1))
        if (boot_render_pso_pre[i].h == h && boot_render_pso_pre[i].k == k && !memcmp(boot_render_pso_pre[i].key, key, k)) { found = 1; break; }
    ReleaseSRWLockShared(&boot_render_pso_pre_lock);
    return found;
}
typedef struct { ID3D12Device *dev; ID3D12RootSignature *root; char manifest[512]; } BootRenderPsoPreArgs;
typedef struct { uint8_t id[32]; uint32_t stage, mask; char *path; } BootRenderPsoManifestEntry;
static inline void *boot_render_pso_read_file(const char *path, size_t *n) {
    FILE *b = fopen(path, "rb"); if (!b) return NULL;
    fseek(b, 0, SEEK_END); long len = ftell(b); fseek(b, 0, SEEK_SET);
    void *data = len > 0 ? malloc((size_t)len) : NULL;
    if (data && fread(data, 1, (size_t)len, b) != (size_t)len) { free(data); data = NULL; }
    fclose(b); *n = (size_t)len; return data;
}
static DWORD WINAPI boot_render_pso_pre_thread(void *arg) {
    BootRenderPsoPreArgs *a = (BootRenderPsoPreArgs *)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    FILE *kf = fopen("pso-keys.bin", "rb"), *mf = fopen(a->manifest, "r");
    if (!mf && !strcmp(a->manifest, "shader-manifest.tsv")) mf = fopen("startup\\shader-manifest.tsv", "r");
    BootRenderPsoManifestEntry *m = NULL; size_t mn = 0, mcap = 0; char line[1024];
    while (kf && mf && fgets(line, sizeof line, mf)) {
        char hex[65], st[4], mask[16], tex[512], path[900]; uint8_t id[32];
        if (sscanf(line, "%64s %3s %15s %511s %899[^\r\n]", hex, st, mask, tex, path) != 5 || !boot_render_hex(hex, id)) continue;
        if (mn == mcap) { size_t c = mcap ? mcap * 2 : 4096; void *p = realloc(m, c * sizeof *m); if (!p) break; m = p; mcap = c; }
        memcpy(m[mn].id, id, 32); m[mn].stage = st[0] == 'v' ? 0 : 1; m[mn].mask = mask[0] == '-' ? 0 : (uint32_t)strtoul(mask, NULL, 16);
        m[mn].path = _strdup(path); if (m[mn].path) ++mn;
    }
    if (mf) fclose(mf);
    BootRenderPsoRecord rec; uint8_t key[BOOT_RENDER_PSO_KEY];
    while (kf && fread(&rec, sizeof rec, 1, kf) == 1 && rec.magic == 0x4b4f5350u && rec.k <= sizeof key && fread(key, 1, rec.k, kf) == rec.k) {
        if (rec.k < 64 + sizeof(D3D12_BLEND_DESC) + sizeof(D3D12_RASTERIZER_DESC) + sizeof(D3D12_DEPTH_STENCIL_DESC) + 12 + 32 || boot_render_pso_pre_known(key, rec.k)) continue;
        const char *vp = NULL, *pp = NULL;
        for (size_t i = 0; i < mn && (!vp || (rec.has_ps && !pp)); ++i) {
            if (!vp && m[i].stage == 0 && !memcmp(m[i].id, key, 32)) vp = m[i].path;
            if (rec.has_ps && !pp && m[i].stage == 1 && m[i].mask == rec.vs_mask && !memcmp(m[i].id, key + 32, 32)) pp = m[i].path;
        }
        if (!vp || (rec.has_ps && !pp)) { InterlockedIncrement(&boot_render_pso_pre_failed); continue; }
        /* p96 (batch 4): the redirects of boot_render_shader_load (TOTK_LDC_FIX=0, TOTK_SHADER_BATCH4=0), so a pipeline made
         * here has the bytecode the draw loads (the draw takes it by key, which holds the program ids, not the bytecode) */
        char vpath[900], ppath[900]; ppath[0] = 0;
        snprintf(vpath, sizeof vpath, "%s", vp); boot_render_ldc_fix_path(vpath, sizeof vpath); boot_render_batch4_path(vpath, sizeof vpath);
        if (pp) { snprintf(ppath, sizeof ppath, "%s", pp); boot_render_ldc_fix_path(ppath, sizeof ppath); boot_render_batch4_path(ppath, sizeof ppath); }
        size_t vn = 0, pn = 0; void *vb = boot_render_pso_read_file(vpath, &vn), *pb = rec.has_ps ? boot_render_pso_read_file(ppath, &pn) : NULL;
        if (vb && (!rec.has_ps || pb)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d); size_t o = 64;
            d.pRootSignature = a->root; d.VS.pShaderBytecode = vb; d.VS.BytecodeLength = vn;
            if (pb) { d.PS.pShaderBytecode = pb; d.PS.BytecodeLength = pn; }
            memcpy(&d.BlendState, key + o, sizeof d.BlendState); o += sizeof d.BlendState;
            memcpy(&d.RasterizerState, key + o, sizeof d.RasterizerState); o += sizeof d.RasterizerState;
            memcpy(&d.DepthStencilState, key + o, sizeof d.DepthStencilState); o += sizeof d.DepthStencilState;
            memcpy(&d.PrimitiveTopologyType, key + o, 4); o += 4; memcpy(&d.IBStripCutValue, key + o, 4); o += 4;
            memcpy(&d.DSVFormat, key + o, 4); o += 4; memcpy(d.RTVFormats, key + o, 32);
            d.SampleMask = UINT_MAX; d.NumRenderTargets = rec.nrt; d.SampleDesc.Count = 1;
            if (rec.k == o + 32 + 8) {   /* G2: a point-sprite pipeline (boot_psoj_key): its geometry shader by layout */
                uint64_t g; memcpy(&g, key + o + 32, 8); const BootPtGs *pg = boot_pt_gs_load(a->manifest, g);
                if (pg && pg->dxil) { d.GS.pShaderBytecode = pg->dxil; d.GS.BytecodeLength = pg->size; } }
            ID3D12PipelineState *pso = NULL;
            if (SUCCEEDED(boot_psoc_create_graphics(NULL, a->dev, &d, key, rec.k, &pso))) {   /* play (73): library first */
                boot_render_pso_pre_put(key, rec.k, pso); InterlockedIncrement(&boot_render_pso_pre_made); }
            else InterlockedIncrement(&boot_render_pso_pre_failed);
        } else InterlockedIncrement(&boot_render_pso_pre_failed);
        free(vb); free(pb);
    }
    if (kf) fclose(kf);
    for (size_t i = 0; i < mn; ++i) free(m[i].path);
    free(m);
    FILE *lf = fopen("draw-path.log", "a");
    if (lf) { fprintf(lf, "nvn native: PSO pre-creation done: %ld pipelines, %ld failed\n", (long)boot_render_pso_pre_made, (long)boot_render_pso_pre_failed); fclose(lf); }
    return 0;
}
static inline void boot_render_pso_pre_start(BootRender *r) {
    if (boot_render_pso_pre_started || InterlockedExchange(&boot_render_pso_pre_started, 1)) return;
    BootRenderPsoPreArgs *a = (BootRenderPsoPreArgs *)calloc(1, sizeof *a); if (!a) return;
    boot_psoc_open(r);   /* play (73): opened here, not on the thread */
    a->dev = r->gpu->device; a->root = r->root; snprintf(a->manifest, sizeof a->manifest, "%s", r->manifest_path);
    ID3D12Device_AddRef(a->dev); ID3D12RootSignature_AddRef(a->root);
    HANDLE t = CreateThread(NULL, 0, boot_render_pso_pre_thread, a, 0, NULL); if (t) CloseHandle(t);
}
/* A pipeline the draw path created: appended for the next boot's pre-creation. */
static inline void boot_render_pso_record(const uint8_t *buf, size_t k, uint32_t vs_mask, int has_ps, uint32_t nrt) {
    static FILE *f; if (!f) f = fopen("pso-keys.bin", "ab"); if (!f) return;
    BootRenderPsoRecord rec = {0x4b4f5350u, (uint32_t)k, vs_mask, (uint32_t)has_ps, nrt};
    fwrite(&rec, sizeof rec, 1, f); fwrite(buf, 1, k, f); fflush(f);
}
static inline int boot_render_pso_recycle(BootRender *r) {
    if (boot_render_pso_retired_count) return 0;   /* the previous generation is not released yet */
    for (unsigned i = 0; i < r->pso_count; ++i) { boot_render_pso_retired[boot_render_pso_retired_count++] = r->psos[i].pso; r->psos[i].pso = NULL; }
    r->pso_count = 0;
    memset(boot_render_pso_index, 0, sizeof boot_render_pso_index); memset(boot_render_pso_key_size, 0, sizeof boot_render_pso_key_size);
    ++boot_render_pso_recycles;
    FILE *lf = fopen("draw-path.log", "a");
    if (lf) { fprintf(lf, "pso cache recycled (%u pipelines retired, recycle %llu)\n", boot_render_pso_retired_count, (unsigned long long)boot_render_pso_recycles); fclose(lf); }
    return 1;
}
/* Only with the queue idle (no recorded or executing list can reference a retired pipeline). */
static inline void boot_render_pso_release_retired(void) {
    for (unsigned i = 0; i < boot_render_pso_retired_count; ++i) if (boot_render_pso_retired[i]) ID3D12PipelineState_Release(boot_render_pso_retired[i]);
    boot_render_pso_retired_count = 0;
}
/* pso-work: parallel pipeline compiles. A pipeline the draw path does not have yet is a driver compile, 0.3-0.8 s
 * for one of TOTK's large translated pixel shaders, and the draws compiled them one after another on the render
 * thread (play-84, first minutes in the open world: 565 compiles, 113 s of "pso" phase; one submission 9.8 s for 19
 * pipelines, 9.3 s of it compiles). Pipelines with no new shader cost ~0 (the driver keeps compiled shaders), so
 * the time is in first visits. At the first miss of a submission the current draw's pipeline is queued here and
 * the rest of the submission is read ahead (boot_render_pso_lookahead_hook, host_nvn_play.h boot_nvn_la_run: the
 * remaining records with a copy of the bound state, nothing executed or recorded); every pipeline its draws will
 * need that exists nowhere yet (r->psos, the pre-creation table, this table) is queued too, in draw order. Up to
 * boot_psoj_threads() workers (below-normal priority, half the logical processors, at most 8) compile them
 * concurrently through the persistent library (boot_psoc_create_graphics: D3D12 pipeline creation is
 * free-threaded, the library calls keep boot_psoc.lock) and hand the result over through the pre-creation table
 * (boot_render_pso_pre_put), where the draw takes it. Each draw still has its own exact pipeline before it records:
 * a job still queued is compiled by the render thread itself, a running one is waited for (its worker raised to
 * normal priority meanwhile); nothing is skipped or substituted. A wrong prediction only costs a background compile
 * (stored in the library, never bound). Jobs own copies of the bytecode and references to the device and root
 * signature. TOTK_PSO_PARALLEL=0 restores the serial path; TOTK_PSO_THREADS=<1..8> sets the worker count. */
#define BOOT_PSOJ_SLOTS 256u
#define BOOT_PSOJ_MAX_THREADS 8u
#define BOOT_PSOJ_WALK_JOBS 192u   /* jobs one read-ahead may queue */
typedef struct BootPsoJob {
    uint64_t h, seq; uint32_t k, state;   /* state 0 free, 1 queued, 2 compiling */
    unsigned runner;                      /* worker index + 1 while a worker compiles it, 0 = the render thread */
    uint8_t key[BOOT_RENDER_PSO_KEY];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d; void *vs, *ps; ID3D12Device *dev;
} BootPsoJob;
typedef struct BootPsojCounters {
    uint64_t walks, walk_qpc, walk_draws, queued_walk, queued_self, done_bg, done_t2, inline_n, mispredicted, compile_qpc, blocked_qpc, waited;
} BootPsojCounters;
static struct {
    SRWLOCK lock; CONDITION_VARIABLE work, done;
    BootPsoJob job[BOOT_PSOJ_SLOTS]; unsigned queued, running, peak, threads, idle; uint64_t seq;
    HANDLE thread[BOOT_PSOJ_MAX_THREADS];
    uint64_t walked_exec;                 /* boot_render_exec_id of the last read-ahead */
    uint64_t roll_exec; BootPsojCounters c, base;   /* monotonic counters; base: at the start of submission roll_exec */
} boot_psoj;
static int (*boot_render_pso_lookahead_hook)(BootRender *r);   /* host_nvn_play.h: reads the rest of the submission ahead */
/* program ids the read-ahead computed (boot_psoj_program_id), valid for read-ahead boot_psoj_walk_no */
#define BOOT_PSOJ_IDS 512u
static struct { uint64_t va, walk; uint8_t id[32]; } boot_psoj_ids[BOOT_PSOJ_IDS];
static uint64_t boot_psoj_walk_no;
static inline int boot_psoj_enabled(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_PSO_PARALLEL", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
static inline unsigned boot_psoj_threads(void) {
    static unsigned n;
    if (!n) {
        char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_PSO_THREADS", v, sizeof v); unsigned want = 0;
        if (k && k < sizeof v) want = (unsigned)strtoul(v, NULL, 10);
        if (!want) { SYSTEM_INFO si; GetSystemInfo(&si); want = si.dwNumberOfProcessors / 3; }   /* 16 logical -> 5: the game keeps ~7 cores busy; workers hold the library lock below normal priority (review) */
        n = want < 1 ? 1 : want > BOOT_PSOJ_MAX_THREADS ? BOOT_PSOJ_MAX_THREADS : want;
    }
    return n;
}
static inline uint64_t boot_psoj_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (uint64_t)q.QuadPart; }
/* Under the lock: counters of a new submission start from here. */
static inline void boot_psoj_roll_locked(void) {
    if (boot_psoj.roll_exec != boot_render_exec_id) { boot_psoj.roll_exec = boot_render_exec_id; boot_psoj.base = boot_psoj.c; boot_psoj.peak = boot_psoj.running; }
}
static inline BootPsoJob *boot_psoj_find_locked(uint64_t h, const uint8_t *key, size_t k) {
    for (unsigned i = 0; i < BOOT_PSOJ_SLOTS; ++i) {
        BootPsoJob *j = &boot_psoj.job[i];
        if (j->state && j->h == h && j->k == k && !memcmp(j->key, key, k)) return j;
    }
    return NULL;
}
static inline void boot_psoj_started_locked(void) { if (++boot_psoj.running > boot_psoj.peak) boot_psoj.peak = boot_psoj.running; }
/* Compiles a job marked compiling (state 2) by its runner, outside the lock; frees the slot and wakes the waiters. */
static inline void boot_psoj_compile(BootPsoJob *j, int by_render_thread) {
    uint64_t q0 = boot_psoj_qpc(); ID3D12PipelineState *pso = NULL;
    HRESULT hr = boot_psoc_create_graphics(NULL, j->dev, &j->d, j->key, j->k, &pso);
    uint64_t took = boot_psoj_qpc() - q0;
    if (SUCCEEDED(hr) && pso) boot_render_pso_pre_put(j->key, j->k, pso);   /* the draw takes it from there */
    else if (pso) ID3D12PipelineState_Release(pso);
    free(j->vs); free(j->ps); j->vs = j->ps = NULL;
    if (j->d.pRootSignature) ID3D12RootSignature_Release(j->d.pRootSignature);
    ID3D12Device_Release(j->dev);
    AcquireSRWLockExclusive(&boot_psoj.lock);
    boot_psoj.c.compile_qpc += took; if (by_render_thread) ++boot_psoj.c.done_t2; else ++boot_psoj.c.done_bg;
    --boot_psoj.running; j->state = 0; j->dev = NULL; j->d.pRootSignature = NULL;
    WakeAllConditionVariable(&boot_psoj.done);
    ReleaseSRWLockExclusive(&boot_psoj.lock);
}
static DWORD WINAPI boot_psoj_worker(void *arg) {
    unsigned me = (unsigned)(uintptr_t)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockExclusive(&boot_psoj.lock);
    for (;;) {
        BootPsoJob *next = NULL;   /* the oldest queued job: draw order */
        for (unsigned i = 0; i < BOOT_PSOJ_SLOTS; ++i)
            if (boot_psoj.job[i].state == 1 && (!next || boot_psoj.job[i].seq < next->seq)) next = &boot_psoj.job[i];
        if (!next) { ++boot_psoj.idle; SleepConditionVariableSRW(&boot_psoj.work, &boot_psoj.lock, INFINITE, 0); --boot_psoj.idle; continue; }
        next->state = 2; next->runner = me + 1; --boot_psoj.queued; boot_psoj_started_locked();
        ReleaseSRWLockExclusive(&boot_psoj.lock);
        boot_psoj_compile(next, 0);
        AcquireSRWLockExclusive(&boot_psoj.lock);
    }
}
/* Render thread: queues a pipeline (own copies of its bytecode). 1 queued, 0 already queued or compiling, -1 no room. */
static inline int boot_psoj_enqueue(BootRender *r, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, const uint8_t *key, size_t k, int from_walk) {
    if (k > BOOT_RENDER_PSO_KEY || !d->VS.pShaderBytecode) return 0;
    uint64_t h = boot_render_pso_hash(key, k);
    void *vs = malloc(d->VS.BytecodeLength ? d->VS.BytecodeLength : 1), *ps = d->PS.pShaderBytecode ? malloc(d->PS.BytecodeLength ? d->PS.BytecodeLength : 1) : NULL;
    if (!vs || (d->PS.pShaderBytecode && !ps)) { free(vs); free(ps); return -1; }
    memcpy(vs, d->VS.pShaderBytecode, d->VS.BytecodeLength); if (ps) memcpy(ps, d->PS.pShaderBytecode, d->PS.BytecodeLength);
    int rc = -1; unsigned spawn = UINT_MAX;
    AcquireSRWLockExclusive(&boot_psoj.lock);
    if (boot_psoj_find_locked(h, key, k)) rc = 0;
    else for (unsigned i = 0; i < BOOT_PSOJ_SLOTS; ++i) {
        BootPsoJob *j = &boot_psoj.job[i]; if (j->state) continue;
        j->h = h; j->k = (uint32_t)k; memcpy(j->key, key, k); j->seq = ++boot_psoj.seq; j->runner = 0;
        j->d = *d; j->d.VS.pShaderBytecode = vs; j->d.PS.pShaderBytecode = ps; j->vs = vs; j->ps = ps;
        j->dev = r->gpu->device; ID3D12Device_AddRef(j->dev);
        if (j->d.pRootSignature) ID3D12RootSignature_AddRef(j->d.pRootSignature);
        j->state = 1; ++boot_psoj.queued; vs = ps = NULL; rc = 1;
        if (from_walk) ++boot_psoj.c.queued_walk; else ++boot_psoj.c.queued_self;
        if (!boot_psoj.idle && boot_psoj.threads < boot_psoj_threads()) spawn = boot_psoj.threads++;
        else WakeConditionVariable(&boot_psoj.work);
        break;
    }
    ReleaseSRWLockExclusive(&boot_psoj.lock);
    free(vs); free(ps);
    if (spawn != UINT_MAX) {   /* a failed thread only leaves the job to the render thread (boot_psoj_wait) */
        HANDLE t = CreateThread(NULL, 0, boot_psoj_worker, (void *)(uintptr_t)spawn, 0, NULL);
        boot_psoj.thread[spawn] = t;
    }
    return rc;
}
/* Render thread, before it compiles a pipeline itself: when a job has the key, it is compiled here (still queued)
 * or waited for (compiling on a worker). 1 when there was a job: its pipeline is in the pre-creation table then. */
static inline int boot_psoj_wait(const uint8_t *key, size_t k) {
    if (!boot_psoj.seq) return 0;   /* nothing ever queued (the render thread is the only producer) */
    uint64_t h = boot_render_pso_hash(key, k), q0 = boot_psoj_qpc(); int had = 0; HANDLE boosted = NULL;
    AcquireSRWLockExclusive(&boot_psoj.lock);
    for (;;) {
        BootPsoJob *j = boot_psoj_find_locked(h, key, k);
        if (!j) break;
        had = 1;
        if (j->state == 1) {   /* not started: compiled here at the render thread's priority */
            j->state = 2; j->runner = 0; --boot_psoj.queued; boot_psoj_started_locked();
            ReleaseSRWLockExclusive(&boot_psoj.lock);
            boot_psoj_compile(j, 1);
            AcquireSRWLockExclusive(&boot_psoj.lock);
            continue;
        }
        if (!boosted && j->runner && j->runner <= BOOT_PSOJ_MAX_THREADS && (boosted = boot_psoj.thread[j->runner - 1]) != NULL)
            SetThreadPriority(boosted, THREAD_PRIORITY_NORMAL);
        SleepConditionVariableSRW(&boot_psoj.done, &boot_psoj.lock, INFINITE, 0);
    }
    if (had) { ++boot_psoj.c.waited; boot_psoj.c.blocked_qpc += boot_psoj_qpc() - q0; }
    ReleaseSRWLockExclusive(&boot_psoj.lock);
    if (boosted) SetThreadPriority(boosted, THREAD_PRIORITY_BELOW_NORMAL);
    return had;
}
/* Render thread: a pipeline it creates itself (no job had it), timed for the hitch line. */
static inline uint64_t boot_psoj_inline_begin(void) {
    if (!boot_psoj_enabled()) return 0;
    AcquireSRWLockExclusive(&boot_psoj.lock); boot_psoj_roll_locked(); boot_psoj_started_locked(); ReleaseSRWLockExclusive(&boot_psoj.lock);
    return boot_psoj_qpc();
}
static inline void boot_psoj_inline_end(uint64_t q0) {
    if (!q0) return;
    uint64_t took = boot_psoj_qpc() - q0;
    AcquireSRWLockExclusive(&boot_psoj.lock);
    --boot_psoj.running; ++boot_psoj.c.inline_n; boot_psoj.c.compile_qpc += took; boot_psoj.c.blocked_qpc += took;
    if (boot_psoj.walked_exec == boot_render_exec_id) ++boot_psoj.c.mispredicted;   /* the read-ahead of this submission did not have it */
    ReleaseSRWLockExclusive(&boot_psoj.lock);
}
/* Render thread, at a pipeline miss: once per submission, queues this pipeline and reads the rest of the
 * submission ahead (its new pipelines are queued behind it). */
static inline void boot_psoj_lookahead(BootRender *r, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, const uint8_t *key, size_t k) {
    AcquireSRWLockExclusive(&boot_psoj.lock); boot_psoj_roll_locked(); ReleaseSRWLockExclusive(&boot_psoj.lock);
    if (!boot_render_pso_lookahead_hook || boot_psoj.walked_exec == boot_render_exec_id) return;
    boot_psoj.walked_exec = boot_render_exec_id;
    ++boot_psoj_walk_no;
    boot_psoj_enqueue(r, d, key, k, 0);
    uint64_t q0 = boot_psoj_qpc();
    boot_render_pso_lookahead_hook(r);
    uint64_t took = boot_psoj_qpc() - q0;
    AcquireSRWLockExclusive(&boot_psoj.lock); ++boot_psoj.c.walks; boot_psoj.c.walk_qpc += took; ReleaseSRWLockExclusive(&boot_psoj.lock);
}
static inline void boot_psoj_walk_draw(void) { ++boot_psoj.c.walk_draws; }   /* render thread only: no lock */
/* After a hitch line (host_nvn_play.h): this submission's pipeline work, when there was any. */
static inline void boot_render_pso_hitch_log(uint64_t submission) {
    if (!boot_psoj_enabled()) return;
    AcquireSRWLockExclusive(&boot_psoj.lock); boot_psoj_roll_locked();
    BootPsojCounters c = boot_psoj.c, b = boot_psoj.base; unsigned peak = boot_psoj.peak, threads = boot_psoj.threads, queued = boot_psoj.queued;
    ReleaseSRWLockExclusive(&boot_psoj.lock);
    uint64_t made = (c.done_bg - b.done_bg) + (c.done_t2 - b.done_t2) + (c.inline_n - b.inline_n);
    if (!made && c.walks == b.walks) return;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); uint64_t per_ms = (uint64_t)f.QuadPart / 1000; if (!per_ms) per_ms = 1;
    boot_psoc_log("nvn native: hitch pso (submission %llu): pipelines created %llu = %llu by workers + %llu queued ones by the render thread + %llu inline "
                  "(%llu not foreseen by the read-ahead); compile ms summed %llu vs render thread blocked %llu ms (%llu waits); peak parallel %u on %u workers; "
                  "read-ahead %llu (%llu ms, %llu distinct draw states, %llu queued + %llu own), %u still queued",
                  (unsigned long long)submission, (unsigned long long)made, (unsigned long long)(c.done_bg - b.done_bg), (unsigned long long)(c.done_t2 - b.done_t2),
                  (unsigned long long)(c.inline_n - b.inline_n), (unsigned long long)(c.mispredicted - b.mispredicted),
                  (unsigned long long)((c.compile_qpc - b.compile_qpc) / per_ms), (unsigned long long)((c.blocked_qpc - b.blocked_qpc) / per_ms),
                  (unsigned long long)(c.waited - b.waited), peak, threads, (unsigned long long)(c.walks - b.walks), (unsigned long long)((c.walk_qpc - b.walk_qpc) / per_ms),
                  (unsigned long long)(c.walk_draws - b.walk_draws), (unsigned long long)(c.queued_walk - b.queued_walk), (unsigned long long)(c.queued_self - b.queued_self), queued);
}
/* Draw trace: one texture the traced draw bound (after boot_render_texture resolved it). Prints the
 * shader stage and handle index, the TIC header, the host slot (valid, table, resource shape) and the
 * nonzero bytes in the first 4 KiB of the guest data and 4 KiB at the middle of it (all zero there =
 * the image was built from memory the game had not filled, or the address is wrong). */
static inline void boot_render_draw_trace_tex(BootRender *r, uint64_t pool, unsigned stage, uint32_t hidx, uint32_t handle) {
    BootRenderTrace *tr = &boot_render_trace; uint32_t tic = handle & 0xfffff;
    if (tr->tex_len + 256 >= sizeof tr->tex_info) return;
    const uint32_t *words = boot_render_desc_words(r, pool, tic, 0);
    BootTic t; memset(&t, 0, sizeof t); int decoded = words && boot_render_tic_decode_any(words, &t);
    const BootRenderTexSlot *slot = boot_render_tex_slots && tic < BOOT_RENDER_TEX_SLOTS ? &boot_render_tex_slots[tic] : NULL;
    D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    if (slot && slot->res) slot->res->lpVtbl->GetDesc(slot->res, &d);
    unsigned nz0 = 0, nzm = 0;
    if (slot && slot->size) {
        const uint8_t *p = boot_render_guest(r, slot->va, 4096); uint64_t mid = slot->va + (slot->size / 2 & ~UINT64_C(4095));
        const uint8_t *q = boot_render_guest(r, mid, 4096);
        for (unsigned k = 0; k < 4096; ++k) { nz0 += p && p[k]; nzm += q && q[k]; }
    }
    tr->tex_len += (size_t)snprintf(tr->tex_info + tr->tex_len, sizeof tr->tex_info - tr->tex_len,
        " [s%u h%u tic %u type %u fmt %02x %ux%ux%u mips %u va %llx | slot %s dim %u res %llux%ux%u mips %u dxgi %d | nz %u/%u]",
        stage, hidx, tic, decoded ? t.texture_type : 99u, t.components, t.width, t.height, t.depth, t.max_mip + 1, (unsigned long long)t.address,
        !slot ? "-" : slot->valid ? "valid" : slot->res ? "stale" : boot_render_fb_on[tic] ? "feedback-copy" : (boot_render_alias && boot_render_alias[tic].on) ? "rt-alias" : "null", slot ? slot->dim : 0u,
        (unsigned long long)d.Width, d.Height, (unsigned)d.DepthOrArraySize, (unsigned)d.MipLevels, (int)d.Format, nz0, nzm);
}
/* Feedback reads: a draw that samples a colour target it also renders to. TOTK's cave terrain pixel
 * shader (73f9b49a / 33ee4310) blends with the G-buffer under it: it fetches albedo, normal and the
 * R32F depth target (50e310000 / 50e6e0000 / 50ee80000) at its own pixel while writing them. Maxwell
 * allows this; on D3D12 the render-to-texture view (boot_render_alias_use) moved the bound target to
 * the shader-resource state, so the draw ran with its target out of the render-target state and the
 * cave's colour writes were lost (black wall, only depth and earlier passes visible). Such a draw
 * samples a copy of the target taken just before it (what every pixel reads is its own pre-draw
 * value, as on the hardware for non-overlapping primitives) and the target stays a render target.
 * The copy is refreshed before every feedback draw, so consecutive draws see each other's output. */
/* 32 (was 16): TOTK cycles more than 16 feedback targets per frame (G-buffer, shadows ...). Each eviction
 * is a full submit/idle (copy_for), which play-70 profiled at ~42% of the submit thread. Max 32: the
 * per-frame pin mask (BootRenderFbFrame99.copies) is 32 bits. Victims are least recently used. */
#define BOOT_RENDER_FB_COPIES 32
static uint64_t boot_render_fb_copy_used[BOOT_RENDER_FB_COPIES], boot_render_fb_copy_clock, boot_render_fb_evictions;
#define BOOT_RENDER_FB_SLOTS 256
#define BOOT_RENDER_FB_SLOT_BASE (BOOT_RENDER_TEX_SLOTS - BOOT_RENDER_FB_SLOTS)
/*99: stable renderer-owned frames. A foreign unwind retains its pins; it cannot
 * leave a global pointer into an abandoned C stack frame. Existing renderer
 * entry/feedback caches are serialized by the renderer's owning lock. */
typedef struct BootRenderFbFrame99 { BootRender *r; unsigned active, prev, copies; uint64_t slots[4]; } BootRenderFbFrame99;
static BootRenderFbFrame99 boot_render_fb_frames99[16];
static unsigned boot_render_fb_current99 = UINT_MAX;
static unsigned boot_render_fb_copy_pins99[BOOT_RENDER_FB_COPIES], boot_render_fb_slot_pins99[BOOT_RENDER_FB_SLOTS];
typedef struct BootRenderFbCopy {
    ID3D12Resource *rt, *copy; D3D12_RESOURCE_STATES state;
    BootRender *owner; ID3D12Device *device; ID3D12CommandQueue *queue; ID3D12Fence *fence;
    uint64_t pb_epoch, pb_loads, pb_gen, pb_exec;   /* batch-c: when the copy was last taken (TOTK_FB_COPY_PER_BARRIER); epoch 0 = never */
} BootRenderFbCopy;
static BootRenderFbCopy boot_render_fb_copies[BOOT_RENDER_FB_COPIES];
typedef struct BootRenderFbIndex { uint32_t words[8]; ID3D12Resource *rt; BootRenderFbCopy *copy; } BootRenderFbIndex;
static BootRenderFbIndex *boot_render_fb_index;
static uint64_t boot_render_fb_draws, boot_render_fb_refreshes;
static inline void boot_render_fb_slots_forget(BootRender *r, ID3D12Resource *copy);
static inline int boot_render_fb_copy_owner99(const BootRender *r, const BootRenderFbCopy *c) {
    return c->owner == r && c->device == r->gpu->device && c->queue == r->gpu->queue && c->fence == r->gpu->fence;
}
static inline int boot_render_fb_pin_copy(BootRender *r, BootRenderFbCopy *c) {
    if (boot_render_fb_current99 >= 16 || !boot_render_fb_copy_owner99(r,c)) return 0;
    BootRenderFbFrame99 *frame = &boot_render_fb_frames99[boot_render_fb_current99];
    if (!frame->active || frame->r != r || c < boot_render_fb_copies || c >= boot_render_fb_copies + BOOT_RENDER_FB_COPIES) return 0;
    unsigned k = (unsigned)(c - boot_render_fb_copies);
    if (!(frame->copies & (1u << k))) { frame->copies |= 1u << k; ++boot_render_fb_copy_pins99[k]; }
    return 1;
}
static inline BootRenderFbCopy *boot_render_fb_copy_for(BootRender *r, ID3D12Resource *rt) {
    if (boot_render_fb_current99 >= 16 || !boot_render_fb_frames99[boot_render_fb_current99].active || boot_render_fb_frames99[boot_render_fb_current99].r != r) return NULL;
    D3D12_RESOURCE_DESC d; rt->lpVtbl->GetDesc(rt, &d);
    BootRenderFbCopy *free_entry = NULL, *victim = NULL;
    for (unsigned k = 0; k < BOOT_RENDER_FB_COPIES; ++k) {
        BootRenderFbCopy *e = &boot_render_fb_copies[k];
        if (!e->copy) { if (!free_entry) free_entry = e; continue; }
        /* A current device's idle wait cannot retire a foreign device's work. */
        if (!boot_render_fb_copy_owner99(r,e)) continue;
        if (!boot_render_fb_copy_pins99[k] && (!victim || boot_render_fb_copy_used[k] < boot_render_fb_copy_used[victim - boot_render_fb_copies])) victim = e;
        if (e->rt != rt) continue;
        D3D12_RESOURCE_DESC c; e->copy->lpVtbl->GetDesc(e->copy, &c);
        if (c.Width == d.Width && c.Height == d.Height && c.Format == d.Format && c.MipLevels == d.MipLevels && c.DepthOrArraySize == d.DepthOrArraySize) {
            boot_render_fb_copy_used[k] = ++boot_render_fb_copy_clock;
            return boot_render_fb_pin_copy(r,e) ? e : NULL;
        }
        if (!boot_render_fb_copy_pins99[k]) victim = e;   /* same target, new size: replace this one first */
    }
    if (!free_entry) free_entry = victim;
    if (!free_entry) return NULL;   /* retain all provisional/foreign ownership */
    boot_render_fb_copy_used[free_entry - boot_render_fb_copies] = ++boot_render_fb_copy_clock;
    if (free_entry->copy) {
        ++boot_render_fb_evictions;
        /* The full existing retirement discipline protects CopyResource/barriers
         * even if a preparing Draw later aborts. Closed is not an idle proof. */
        if (r->recording ? !boot_render_submit(r) : !boot_gpu_wait(r->gpu)) return NULL;
        boot_render_fb_slots_forget(r,free_entry->copy);
        if (boot_render_fb_index) for (uint32_t i = 0; i < BOOT_RENDER_TEX_SLOTS; ++i)
            if (boot_render_fb_index[i].copy == free_entry) boot_render_fb_on[i] = 0;
        ID3D12Resource_Release(free_entry->copy); memset(free_entry,0,sizeof *free_entry);
    }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_RESOURCE_DESC cd = d; cd.Flags = D3D12_RESOURCE_FLAG_NONE; cd.Alignment = 0;
    if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&free_entry->copy))) {
        free_entry->copy = NULL; return NULL;
    }
    free_entry->rt = rt; free_entry->state = D3D12_RESOURCE_STATE_COPY_DEST;
    free_entry->pb_epoch = 0;   /* batch-c: a new copy holds nothing yet */
    free_entry->owner = r; free_entry->device = r->gpu->device; free_entry->queue = r->gpu->queue; free_entry->fence = r->gpu->fence;
    return boot_render_fb_pin_copy(r,free_entry) ? free_entry : NULL;
}
/* batch-c: one feedback copy per barrier (TOTK_FB_COPY_PER_BARRIER, default on; =0 copies before every feedback
 * draw, as before). On Maxwell/NVN a draw that samples a target it renders to sees that target as of the last
 * barrier the game issued (nvnCommandBufferBarrier: the texture cache is not coherent with colour writes until
 * then): what a pixel reads of writes made by draws since the barrier is undefined. So the copy the first feedback
 * draw after a sync point takes serves every later feedback draw of the same target until the next sync point.
 * Sync points (each starts a new epoch): an NVN barrier, a draw with another set of bound targets (colour or depth),
 * and anything else that can write a target outside the draws: a clear, a compute dispatch, a buffer<->texture copy,
 * a clear of a buffer (counted as "other"), a target reloaded from guest memory, a target texture created or evicted,
 * a new queue submission (checked per copy too, so a reload inside the draw's own texture loop also refreshes). The
 * draw that takes a copy writes that target (its write mask is set), so "written since the copy" holds after every
 * copy, and a new epoch always takes a new one: the first feedback draw after any sync point is exact. */
static uint64_t boot_render_fb_pb_epoch = 1, boot_render_fb_pb_reuses, boot_render_fb_pb_barriers, boot_render_fb_pb_ends[3];
static inline int boot_render_fb_pb_on(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_FB_COPY_PER_BARRIER", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
/* why: 0 a guest barrier, 1 another set of bound targets, 2 anything else (see above) */
static inline void boot_render_fb_pb_sync(unsigned why) { ++boot_render_fb_pb_epoch; if (why < 3) ++boot_render_fb_pb_ends[why]; }
/* the NVN executor decoded nvnCommandBufferBarrier (host_nvn_play.h; any flags) */
static inline void boot_render_fb_pb_barrier(void) { ++boot_render_fb_pb_barriers; boot_render_fb_pb_sync(0); }
typedef struct BootRenderFbPbSet { BootRender *r; ID3D12Resource *rt[8], *zt; unsigned n; uint64_t gen, loads, exec; } BootRenderFbPbSet;
static BootRenderFbPbSet boot_render_fb_pb_set;
/* Per draw, after its targets are bound and before its textures (where the copies are taken). */
static inline void boot_render_fb_pb_draw(BootRender *r, BootRenderTarget *const *rts, unsigned n, const BootRenderTarget *zt, unsigned guest_barrier) {
    BootRenderFbPbSet cur; boot_render_zero(&cur, sizeof cur);   /* heavy-work: rep stosb */
    cur.r = r; cur.n = n; cur.zt = zt ? zt->texture : NULL;
    for (unsigned i = 0; i < n && i < 8; ++i) cur.rt[i] = rts[i] ? rts[i]->texture : NULL;
    cur.gen = boot_render_rtab(r)->generation; cur.loads = boot_render_rt_loads; cur.exec = boot_render_exec_id;
    BootRenderFbPbSet *p = &boot_render_fb_pb_set;
    if (guest_barrier) ++boot_render_fb_pb_epoch;   /* legacy path barrier flag (the NVN executor already counted its barrier) */
    if (p->r != cur.r || p->n != cur.n || p->zt != cur.zt || !boot_render_memeq(p->rt, cur.rt, sizeof cur.rt)) boot_render_fb_pb_sync(1);
    else if (p->gen != cur.gen || p->loads != cur.loads || p->exec != cur.exec) boot_render_fb_pb_sync(2);
    *p = cur;
}
/* Whether copy `c` still holds target `t` as of the current epoch's start (taken in this epoch, nothing since). */
static inline int boot_render_fb_pb_valid(BootRender *r, const BootRenderTarget *t, const BootRenderFbCopy *c) {
    return c->pb_epoch && c->pb_epoch == boot_render_fb_pb_epoch && c->copy && c->rt == t->texture && c->owner == r &&
           c->state == (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
           c->pb_loads == boot_render_rt_loads && c->pb_gen == boot_render_rtab(r)->generation && c->pb_exec == boot_render_exec_id;
}
/* Copies bound target `t` into its feedback copy (recorded in order: earlier draws' writes included). */
static inline int boot_render_fb_refresh(BootRender *r, BootRenderTarget *t, BootRenderFbCopy *c) {
    if (boot_render_fb_pb_on() && boot_render_fb_pb_valid(r, t, c)) {   /* batch-c: taken since the last sync point */
        ++boot_render_fb_pb_reuses;
        return boot_render_rt_barrier(r, t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (!r->recording && !boot_render_begin(r)) return 0;
    if (!boot_render_rt_barrier(r, t, D3D12_RESOURCE_STATE_COPY_SOURCE)) return 0;
    if (c->state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = c->copy;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = c->state; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        BOOT_GP_RB("fb feedback", r->cl, 1, &b); c->state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    BOOT_GP_CRES("fb feedback", r->cl, c->copy, t->texture);
    D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = c->copy;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    BOOT_GP_RB("fb feedback", r->cl, 1, &b); c->state = b.Transition.StateAfter;
    ++boot_render_fb_refreshes;
    c->pb_epoch = boot_render_fb_pb_epoch; c->pb_loads = boot_render_rt_loads; c->pb_gen = boot_render_rtab(r)->generation; c->pb_exec = boot_render_exec_id;   /* batch-c */
    return boot_render_rt_barrier(r, t, D3D12_RESOURCE_STATE_RENDER_TARGET);
}
/* The bound colour target (index into rts) a TIC entry's render-to-texture view samples, or -1. */
static inline int boot_render_fb_bound(uint32_t tic, BootRenderTarget *const *rts, unsigned rt_count) {
    if (!boot_render_alias || !boot_render_alias[tic].on) return -1;
    for (unsigned k = 0; k < rt_count; ++k) if (rts[k] && rts[k]->texture == boot_render_alias[tic].texture) return (int)k;
    return -1;
}
/* TOTK_FEEDBACK_COPY=0 turns the copies off (A/B: the old in-place behaviour). */
static inline int boot_render_fb_enabled(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_FEEDBACK_COPY", e, sizeof e); v = !(n && n < sizeof e && e[0] == '0'); }
    return v;
}
/* Feedback views without a wait: the copy's view goes to a reserved 2D descriptor slot (the top
 * BOOT_RENDER_FB_SLOTS indices of the table) and the draw's bindless handle constant buffer (c2) gets a
 * private snapshot whose handle names that slot, so the TIC index's own descriptor (the target's view,
 * which draws recorded before may read) is never rewritten. A slot keeps its view (copy resource + TIC
 * words): later feedback draws of the same target reuse it, and the copy itself is refreshed in GPU order
 * before each one. A slot is only rewritten once the GPU finished the lists that used it. Game TIC indices
 * in the reserved range turn this off (the in-place path below then runs). */
typedef struct BootRenderFbSlot {
    ID3D12Resource *copy; uint32_t words[8]; uint64_t used_list, submitted_fence; unsigned issued_pending;
    BootRender *owner; ID3D12Device *device; ID3D12CommandQueue *queue; ID3D12Fence *fence; ID3D12DescriptorHeap *heap;
} BootRenderFbSlot;
static BootRenderFbSlot boot_render_fb_slots[BOOT_RENDER_FB_SLOTS]; static unsigned boot_render_fb_slot_next;
static int boot_render_fb_slots_off; static uint64_t boot_render_fb_slot_writes, boot_render_fb_slot_hits, boot_render_fb_slot_waits;
static int boot_render_fb_have_slots99;
static inline int boot_render_fb_slot_owner99(const BootRender *r, const BootRenderFbSlot *f) {
    return f->owner == r && f->device == r->gpu->device && f->queue == r->gpu->queue && f->fence == r->gpu->fence && f->heap == r->srv_heap;
}
static inline void boot_render_fb_slots_forget(BootRender *r, ID3D12Resource *copy) {
    for (unsigned k = 0; k < BOOT_RENDER_FB_SLOTS; ++k)
        if (!boot_render_fb_slot_pins99[k] && boot_render_fb_slot_owner99(r,&boot_render_fb_slots[k]) &&
            (!copy || boot_render_fb_slots[k].copy == copy)) memset(&boot_render_fb_slots[k],0,sizeof boot_render_fb_slots[k]);
}
static inline int boot_render_fb_pin_slot99(BootRender *r, unsigned k) {
    if (k >= BOOT_RENDER_FB_SLOTS || boot_render_fb_current99 >= 16) return 0;
    BootRenderFbFrame99 *frame = &boot_render_fb_frames99[boot_render_fb_current99];
    if (!frame->active || frame->r != r) return 0;
    uint64_t bit = UINT64_C(1) << (k & 63);
    if (!(frame->slots[k >> 6] & bit)) { frame->slots[k >> 6] |= bit; ++boot_render_fb_slot_pins99[k]; }
    return 1;
}
static inline void boot_render_fb_commit(BootRender *r) {
    if (boot_render_fb_current99 >= 16) return;
    const BootRenderFbFrame99 *frame = &boot_render_fb_frames99[boot_render_fb_current99];
    if (!frame->active || frame->r != r) return;
    if (!(frame->slots[0] | frame->slots[1] | frame->slots[2] | frame->slots[3])) return;
    for (unsigned k = 0; k < BOOT_RENDER_FB_SLOTS; ++k)
        if (frame->slots[k >> 6] & (UINT64_C(1) << (k & 63))) {
            boot_render_fb_slots[k].used_list = boot_render_list_id; boot_render_fb_slots[k].issued_pending = 1;
        }
}
/* The caller has received success from the ORIGINAL main queue Signal. Other
 * queue Signals, including full waits while this CL is open, do not call here. */
static inline void boot_render_fb_signaled99(BootRender *r, uint64_t value) {
    if (!boot_render_fb_have_slots99) return;
    for (unsigned k = 0; k < BOOT_RENDER_FB_SLOTS; ++k) {
        BootRenderFbSlot *f = &boot_render_fb_slots[k];
        if (f->copy && f->owner == r && f->device == r->gpu->device && f->queue == r->gpu->queue && f->fence == r->gpu->fence && f->heap == r->srv_heap &&
            f->used_list == boot_render_list_id) { f->submitted_fence = value; f->issued_pending = 0; }
    }
}
static inline int boot_render_fb_slot_retire99(BootRender *r, BootRenderFbSlot *f);
static inline int boot_render_fb_game_tic(BootRender *r, uint32_t index) {
    if (index < BOOT_RENDER_FB_SLOT_BASE || index >= BOOT_RENDER_TEX_SLOTS) return 1;
    if (boot_render_fb_slot_pins99[index - BOOT_RENDER_FB_SLOT_BASE]) return 0;
    BootRenderFbSlot *f = &boot_render_fb_slots[index - BOOT_RENDER_FB_SLOT_BASE];
    if (f->copy && !boot_render_fb_slot_owner99(r,f)) return 0;
    if (!boot_render_fb_slots_taken) {
        if (r->recording ? !boot_render_submit(r) : !boot_gpu_wait(r->gpu)) return 0;
        boot_render_fb_slots_taken = 1; boot_render_fb_slots_off = 1;
        boot_render_fb_slots_forget(r,NULL);   /* outer noncolliding/foreign owners remain */
    }
    /* A pinned outer slot may survive the first disable, then be consumed and
     * unpinned later. EVERY subsequent ordinary writer must retire that survivor. */
    if (f->copy) {
        if (!boot_render_fb_slot_retire99(r,f)) return 0;
        memset(f,0,sizeof *f);
    }
    return 1;
}
static inline int boot_render_fb_slot_retire99(BootRender *r, BootRenderFbSlot *f) {
    if (!f->copy || !f->used_list) return 1;
    if (f->owner != r || f->device != r->gpu->device || f->queue != r->gpu->queue || f->fence != r->gpu->fence || f->heap != r->srv_heap) return 0;
    /* GT resume can append to this same logical list after an older real fence.
     * Close/drain that open owner BEFORE looking up its latest real Signal. */
    if (r->recording && f->used_list == boot_render_list_id && (!boot_render_submit(r) || !boot_render_begin(r))) return 0;
    if (f->issued_pending || !f->submitted_fence) return 0;
    ++boot_render_fb_slot_waits;
    return boot_render_wait_fence(r->gpu,f->submitted_fence);
}
static inline int boot_render_fb_slot(BootRender *r, BootRenderFbCopy *c, BootRenderTarget *t, const uint32_t words[8]) {
    if (boot_render_fb_slots_taken || boot_render_fb_slots_off) return -1;
    if (!boot_render_fb_pin_copy(r,c)) return -2;
    for (unsigned k = 0; k < BOOT_RENDER_FB_SLOTS; ++k) {
        BootRenderFbSlot *f = &boot_render_fb_slots[k];
        if (f->copy == c->copy && f->owner == r && f->device == r->gpu->device && f->queue == r->gpu->queue && f->fence == r->gpu->fence && f->heap == r->srv_heap && !memcmp(f->words,words,32)) {
            if (!boot_render_fb_pin_slot99(r,k)) return -2;
            ++boot_render_fb_slot_hits; return (int)(BOOT_RENDER_FB_SLOT_BASE+k);
        }
    }
    unsigned k = boot_render_fb_slot_next, searched = 0;
    while (searched < BOOT_RENDER_FB_SLOTS && boot_render_fb_slot_pins99[k]) { k = (k+1)%BOOT_RENDER_FB_SLOTS; ++searched; }
    if (searched == BOOT_RENDER_FB_SLOTS) return -1;
    BootRenderFbSlot *f = &boot_render_fb_slots[k];
    if (!boot_render_fb_slot_retire99(r,f)) return -2;
    BootTic tic_hdr; memset(&tic_hdr,0,sizeof tic_hdr); boot_render_tic_decode_any(words,&tic_hdr);
    D3D12_SHADER_RESOURCE_VIEW_DESC rv = {0}; rv.Format=t->dxgi; rv.Shader4ComponentMapping=boot_render_swizzle(&tic_hdr);
    rv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; rv.Texture2D.MipLevels=1;
    if (!boot_render_fb_pin_slot99(r,k)) return -2;
    ID3D12Device_CreateShaderResourceView(r->gpu->device,c->copy,&rv,boot_render_tex_cpu(r,BOOT_DIM_2D,BOOT_RENDER_FB_SLOT_BASE+k));
    f->copy=c->copy; memcpy(f->words,words,32); f->used_list=0; f->submitted_fence=0; f->issued_pending=0;
    f->owner=r; f->device=r->gpu->device; f->queue=r->gpu->queue; f->fence=r->gpu->fence; f->heap=r->srv_heap;
    boot_render_fb_have_slots99=1;
    boot_render_fb_slot_next=(k+1)%BOOT_RENDER_FB_SLOTS; ++boot_render_fb_slot_writes;
    return (int)(BOOT_RENDER_FB_SLOT_BASE+k);
}
/* Points TIC index `tic`'s 2D descriptor at the feedback copy of rts[k] (words: the TIC entry). */
static inline int boot_render_fb_bind(BootRender *r, uint32_t tic, const uint32_t words[8], BootRenderTarget *t) {
    if (!boot_render_fb_index && !(boot_render_fb_index = calloc(BOOT_RENDER_TEX_SLOTS, sizeof *boot_render_fb_index))) return 0;
    BootRenderFbCopy *c = boot_render_fb_copy_for(r, t->texture); if (!c) return 0;
    /* earlier recorded draws may read this index's current view (the target itself): finish them first */
    if (r->recording && !boot_render_submit(r)) return 0;
    if (!boot_render_fb_refresh(r, t, c)) return 0;
    BootTic tic_hdr; memset(&tic_hdr, 0, sizeof tic_hdr); boot_render_tic_decode_any(words, &tic_hdr);
    D3D12_SHADER_RESOURCE_VIEW_DESC rv = {0};
    rv.Format = t->dxgi; rv.Shader4ComponentMapping = boot_render_swizzle(&tic_hdr);
    rv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; rv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(r->gpu->device, c->copy, &rv, boot_render_tex_cpu(r, BOOT_DIM_2D, tic));
    boot_render_alias_forget(tic);   /* the index no longer views the target: a later plain use rewrites it */
    BootRenderFbIndex *x = &boot_render_fb_index[tic]; memcpy(x->words, words, 32); x->rt = t->texture; x->copy = c; boot_render_fb_on[tic] = 1;
    boot_render_texmemo[tic] = 0;   /* nvnpar-work: a feedback view at the index */
    return 1;
}
/* play: CPU time per draw phase (QPC ticks), reported with the native perf lines (host_nvn_play.h):
 * 0 ring/segment, 1 program ids + shader lookup, 2 targets, 3 textures + samplers + target states,
 * 4 vertex table + indices, 5 PSO key + lookup/create, 6 CBV/SRV descriptors + snapshots, 7 bind + draw,
 * 8 native state build (boot_nvn_exec_state). A draw that returns early adds only the phases it ran. */
static uint64_t boot_render_ph_qpc[9], boot_render_ph_draws, boot_render_ph_instanced, boot_render_ph_instances;
/* heavy-work: the phase clock above is read ~12 times per draw on the render thread; QueryPerformanceCounter costs ~45 ns a
 * read here (4 ms of t2 per frame at 7,000 draws), the TSC ~10 ns. Same units (QPC ticks): TSC ticks scaled by the ratio
 * measured against QPC over the first 200 ms of use, anchored at the switch and never going backwards. Only the phase
 * and cost lines of draw-path.log read it. TOTK_PHASE_TSC=0: QueryPerformanceCounter as before. */
static inline uint64_t boot_render_ph_now(void) {
    static int mode = -1; static uint64_t q0, t0, qs, ts, last; static double scale;
    if (mode == 1) {
        uint64_t t = __builtin_ia32_rdtsc(); if (t < ts) t = ts;
        uint64_t v = qs + (uint64_t)((double)(t - ts) * scale);
        if (v < last) v = last;
        last = v; return v;
    }
    LARGE_INTEGER q; QueryPerformanceCounter(&q); uint64_t qv = (uint64_t)q.QuadPart;
    if (mode < 0) {
        char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_PHASE_TSC", e, sizeof e);
        mode = n && n < sizeof e && e[0] == '0' ? 2 : 0; q0 = qv; t0 = __builtin_ia32_rdtsc();
    } else if (mode == 0) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        if (qv > q0 && qv - q0 >= (uint64_t)f.QuadPart / 5) {
            uint64_t t = __builtin_ia32_rdtsc();
            if (t > t0) { scale = (double)(qv - q0) / (double)(t - t0); qs = qv; ts = t; last = qv; mode = 1; } else mode = 2;
        }
    }
    return qv;
}
static inline int boot_render_cost_log_on(void) {
    static int on = -1;
    if (on < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_DRAW_COST_LOG", e, sizeof e); on = !(n && n < sizeof e && e[0] == '0'); }
    return on;
}
/* nvnpar-work: where the texture phase goes (TOTK_TEXTURE_DETAIL, default on with the cost line; =0 off): per handle the
 * draw resolves, remembered (boot_render_texmemo) or resolved by boot_render_texture, classified by what that did
 * (unchanged return, target alias reused, target view written, built from guest memory, shared with another slot,
 * anything else: stale assemblies, CPU-watch checks, buffers, target copies, mip chains); feedback views refreshed at
 * the top of the loop, the feedback / depth-alias tests after a resolution, the samplers, the handle snapshot before
 * the loop and the target bookkeeping after it; the fence waits inside the phase and every submit made inside it by
 * call site (each one a complete GPU drain). Two clock reads per resolved handle; nothing else changes. */
enum { BOOT_TXD_SAME, BOOT_TXD_ALIAS, BOOT_TXD_VIEW, BOOT_TXD_BUILD, BOOT_TXD_SHARED, BOOT_TXD_OTHER, BOOT_TXD_FBTOP, BOOT_TXD_FB, BOOT_TXD_SMP,
       BOOT_TXD_PRE, BOOT_TXD_AFTER, BOOT_TXD_WAIT, BOOT_TXD_N };
static uint64_t boot_render_txd_q[BOOT_TXD_N], boot_render_txd_n[BOOT_TXD_N], boot_render_txd_memo;
static uint64_t boot_render_txd_last_q[BOOT_TXD_N], boot_render_txd_last_n[BOOT_TXD_N], boot_render_txd_last_memo, boot_render_txd_last_site_n[48], boot_render_txd_last_site_q[48];
static uint64_t boot_render_txd_last_memo_hits[2], boot_render_txd_last_memo_res[2];
static inline int boot_render_txd_on(void) {
    static int on = -1;
    if (on < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_TEXTURE_DETAIL", e, sizeof e); on = !(n && n < sizeof e && e[0] == '0') && boot_render_cost_log_on(); }
    return on;
}
static inline void boot_render_txd_report(FILE *lf, uint64_t draws, uint64_t per) {
    static const char *const nm[BOOT_TXD_N] = {"unchanged", "target alias", "target view", "guest build", "shared", "other", "feedback views", "feedback/alias tests",
                                               "samplers", "handle snapshot", "after the loop", "fence waits"};
    char line[1600]; size_t at = 0;
    uint64_t memo = boot_render_txd_memo - boot_render_txd_last_memo, res = 0;
    for (unsigned c = 0; c <= BOOT_TXD_FBTOP; ++c) res += boot_render_txd_n[c] - boot_render_txd_last_n[c];
    uint64_t th = boot_render_texmemo_hits - boot_render_txd_last_memo_hits[0], tr = boot_render_texmemo_resolves - boot_render_txd_last_memo_res[0];
    uint64_t sh = boot_render_smpmemo_hits - boot_render_txd_last_memo_hits[1], sr = boot_render_smpmemo_resolves - boot_render_txd_last_memo_res[1];
    at += (size_t)snprintf(line + at, sizeof line - at, " handles %.2f/draw, remembered %.1f%% (TOTK_TEXSET_CACHE %s%s; all textures %.1f%%, samplers %.1f%%);",
                           (double)(memo + res) / (double)draws, memo + res ? 100.0 * (double)memo / (double)(memo + res) : 0.0, boot_render_texmemo_on() ? "on" : "off",
                           boot_render_texmemo_verify_on() ? ", verify" : "", th + tr ? 100.0 * (double)th / (double)(th + tr) : 0.0, sh + sr ? 100.0 * (double)sh / (double)(sh + sr) : 0.0);
    for (unsigned c = 0; c < BOOT_TXD_N && at < sizeof line; ++c) {
        uint64_t q = boot_render_txd_q[c] - boot_render_txd_last_q[c], n = boot_render_txd_n[c] - boot_render_txd_last_n[c], ns = per ? q * 1000000000u / per : 0;
        if (c <= BOOT_TXD_FBTOP) at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu.%02llu (%.2f/draw)", nm[c], (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000 / 10), (double)n / (double)draws);
        else at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu.%02llu", nm[c], (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000 / 10));
    }
    if (at < sizeof line) at += (size_t)snprintf(line + at, sizeof line - at, "; GPU drains inside:");
    for (unsigned i = 0; i < 48 && boot_render_submit_sites[i].file && at < sizeof line; ++i) {
        uint64_t n = boot_render_txd_site_n[i] - boot_render_txd_last_site_n[i], q = boot_render_txd_site_q[i] - boot_render_txd_last_site_q[i];
        if (!n) continue;
        const char *name = strrchr(boot_render_submit_sites[i].file, '/'), *b2 = strrchr(boot_render_submit_sites[i].file, '\\');
        if (!name || (b2 && b2 > name)) name = b2;
        uint64_t ns = per ? q * 1000000000u / per : 0;
        at += (size_t)snprintf(line + at, sizeof line - at, " %s:%d=%llu (%llu.%02llu us/draw)", name ? name + 1 : boot_render_submit_sites[i].file, boot_render_submit_sites[i].line,
                               (unsigned long long)n, (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000 / 10));
    }
    fprintf(lf, "render texture detail (us/draw over %llu draws):%s\n", (unsigned long long)draws, line);
    memcpy(boot_render_txd_last_q, boot_render_txd_q, sizeof boot_render_txd_q); memcpy(boot_render_txd_last_n, boot_render_txd_n, sizeof boot_render_txd_n);
    boot_render_txd_last_memo = boot_render_txd_memo;
    memcpy(boot_render_txd_last_site_n, boot_render_txd_site_n, sizeof boot_render_txd_site_n); memcpy(boot_render_txd_last_site_q, boot_render_txd_site_q, sizeof boot_render_txd_site_q);
    boot_render_txd_last_memo_hits[0] = boot_render_texmemo_hits; boot_render_txd_last_memo_res[0] = boot_render_texmemo_resolves;
    boot_render_txd_last_memo_hits[1] = boot_render_smpmemo_hits; boot_render_txd_last_memo_res[1] = boot_render_smpmemo_resolves;
}
/* nvnpar-work 1f (TOTK_CB_DEDUPE, default on; =0: every slot resolved; =2: resolved anyway and compared, mismatches
 * counted and logged). A stage's 18 constant-buffer slots are resolved one by one; the slots a shader leaves unbound all
 * read the same dummy range of the driver block (valid 1, 256 bytes), so ~12 of 15 uniform slots per stage ask
 * boot_render_cbv_ex for the same snapshot in a row. A slot whose snapshot key (address, size) is the one the previous
 * snapshot slot of the stage asked for, with no snapshot cache or page write since (boot_render_cb_epoch: every cache
 * entry written and every guest write noted moves it) in the same ring scope, would find the entry that call left
 * (its hit, or the one its miss created) and return the same view: it takes that view and counts the hit. Not with
 * feedback patches, staged overlays or the CBV tap (they make cbv_ex private or observed). */
static uint64_t boot_render_cb_dedupe_hits, boot_render_cb_dedupe_bad;
static inline int boot_render_cb_dedupe_mode(void) {
    static int v = -1;
    if (v < 0) { char e[8]; DWORD n = GetEnvironmentVariableA("TOTK_CB_DEDUPE", e, sizeof e); v = n && n < sizeof e && e[0] == '0' ? 0 : n && n < sizeof e && e[0] == '2' ? 2 : 1; }
    return v;
}
/* batch-b: the render thread's CPU time per host draw by phase over the last 16384 draws (the counters above; "state"
 * is boot_nvn_exec_state, host_nvn_play.h), plus the depth-feedback GPU copies (boot_render_texture_from_target), in
 * draw-path.log. TOTK_DRAW_COST_LOG=0: off. */
static uint64_t boot_render_cost_last_qpc[9], boot_render_cost_last_draws, boot_render_cost_last_dfb[3];
static inline void boot_render_cost_report(void) {
    const int on = boot_render_cost_log_on();
    if (!on || boot_render_ph_draws - boot_render_cost_last_draws < 16384) return;
    static const char *const nm[9] = {"ring", "shaders", "targets", "textures", "vertex", "pso", "descriptors", "bind", "state"};
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    const uint64_t draws = boot_render_ph_draws - boot_render_cost_last_draws, per = (uint64_t)fq.QuadPart * draws;
    char line[384]; size_t at = 0; uint64_t total = 0;
    for (unsigned i = 0; i < 9; ++i) {
        uint64_t d = boot_render_ph_qpc[i] - boot_render_cost_last_qpc[i], ns = per ? d * 1000000000u / per : 0; total += d;
        if (at < sizeof line) at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu.%02llu", nm[i], (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000 / 10));
    }
    uint64_t tns = per ? total * 1000000000u / per : 0;
    FILE *lf = fopen("draw-path.log", "a");
    if (lf) {
        fprintf(lf, "render draw cost (us/draw over %llu draws):%s; sum %llu.%02llu; depth feedback on the GPU: built %llu, in place %llu, views skipped %llu\n",
                (unsigned long long)draws, line, (unsigned long long)(tns / 1000), (unsigned long long)(tns % 1000 / 10),
                (unsigned long long)(boot_render_dfb_built - boot_render_cost_last_dfb[0]), (unsigned long long)(boot_render_dfb_refreshes - boot_render_cost_last_dfb[1]),
                (unsigned long long)(boot_render_dfb_alias_skips - boot_render_cost_last_dfb[2]));
        if (boot_render_txd_on()) boot_render_txd_report(lf, draws, per);   /* nvnpar-work */
        fclose(lf);
    }
    memcpy(boot_render_cost_last_qpc, boot_render_ph_qpc, sizeof boot_render_cost_last_qpc); boot_render_cost_last_draws = boot_render_ph_draws;
    boot_render_cost_last_dfb[0] = boot_render_dfb_built; boot_render_cost_last_dfb[1] = boot_render_dfb_refreshes; boot_render_cost_last_dfb[2] = boot_render_dfb_alias_skips;
}
/* play (41): instances of the next boot_render_draw (0 = 1), set by the native executor just before the call */
static uint32_t boot_render_next_instances;
/* play (43): the previous draw's programs (see the shaders section of boot_render_draw) */
static struct {
    BootRender *r; uint64_t vs_va, ps_va, exec, map_gen, big, epoch; unsigned shaders; size_t vs_size, ps_size; int has_ps;
    uint8_t vs_id[32], ps_id[32]; BootRenderShader *vs, *ps; uint32_t vs_mask, ps_outputs; int stores;
} boot_render_prog_memo; static uint64_t boot_render_prog_memo_hits;
/* heavy-work: the memo above with 64 entries, direct-mapped by the two program addresses (draws alternate between a few
 * program pairs); each entry is validated exactly as the single one was (same render object, programs, submission, page
 * table, no large write, same shader count, no host write to either program since the entry was taken).
 * TOTK_PROG_MEMO_WAYS=1: the single previous-draw entry as before. */
#define BOOT_RENDER_PROG_MEMO_WAYS 64
static __typeof__(boot_render_prog_memo) boot_render_prog_memo_way[BOOT_RENDER_PROG_MEMO_WAYS];
static inline __typeof__(boot_render_prog_memo) *boot_render_prog_memo_at(uint64_t vs, uint64_t ps) {
    static int ways = -1;
    if (ways < 0) { char v[8]; DWORD n = GetEnvironmentVariableA("TOTK_PROG_MEMO_WAYS", v, sizeof v); ways = n && n < sizeof v && atoi(v) <= 1 ? 1 : BOOT_RENDER_PROG_MEMO_WAYS; }
    if (ways == 1) return &boot_render_prog_memo;
    uint64_t h = (vs >> 4) ^ (ps >> 4) * UINT64_C(0x9e3779b97f4a7c15); h ^= h >> 29;
    return &boot_render_prog_memo_way[h & (BOOT_RENDER_PROG_MEMO_WAYS - 1)];
}
static inline size_t boot_render_program_cached_size(uint64_t va) {   /* program-id cache entry size for va, 0 if none */
    unsigned way = (unsigned)((va >> 6) ^ (va >> 17)) & (BOOT_RENDER_PROGRAM_CACHE - 1);
    return boot_render_program_cache[way].bytes && boot_render_program_cache[way].va == va ? boot_render_program_cache[way].size : 0;
}
/* render-40 (host_gpu_timestamp.h): a draw run is one pipeline + the bound target textures; its tag the program ids
 * (first 4 bytes of the VS/PS ids) and the first target's size, format and the target count */
static inline void boot_gp_draw_tag(ID3D12GraphicsCommandList *cl, const char *file, unsigned line, const ID3D12PipelineState *pso,
                                    BootRenderTarget *const *rts, unsigned n, const BootRenderTarget *zt, const uint8_t *vs, const uint8_t *ps) {
    uint64_t id = (uint64_t)(uintptr_t)pso * 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < n && i < 8; ++i) id = (id ^ (uint64_t)(uintptr_t)(rts[i] ? rts[i]->texture : NULL)) * 0x100000001b3ull;
    id = (id ^ (uint64_t)(uintptr_t)(zt ? zt->texture : NULL)) * 0x100000001b3ull;
    const BootRenderTarget *t = n && rts[0] ? rts[0] : zt;
    uint64_t info = t ? ((uint64_t)(t->width & 0xffff) << 48 | (uint64_t)(t->height & 0xffff) << 32 | (uint64_t)((unsigned)t->dxgi & 0xffff) << 16) : 0;
    info |= (uint64_t)(n & 15) << 8 | (zt ? 1u : 0u);
    uint32_t v = vs ? (uint32_t)vs[0] << 24 | (uint32_t)vs[1] << 16 | (uint32_t)vs[2] << 8 | vs[3] : 0;
    uint32_t p = ps ? (uint32_t)ps[0] << 24 | (uint32_t)ps[1] << 16 | (uint32_t)ps[2] << 8 | ps[3] : 0;
    boot_gp_draw_begin(cl, file, line, id, (uint64_t)v << 32 | p, info);
}
#define BOOT_GP_DRAW_BEGIN(cl, pso, rts, n, zt, vs, ps) do { if (boot_gp_mode > 0) boot_gp_draw_tag((cl), __FILE__, __LINE__, (pso), (rts), (n), (zt), (vs), (ps)); } while (0)
/* batch-3 state diagnostics (TOTK_STATE_DIAG, default on; =0: none of these checks run). They change no draw: each check
 * logs its first hits to draw-path.log, and host_nvn_exec.h boot_nvn_state_diag_present writes the non-zero counters
 * every 300 presents (last 300, total). Counters bumped on game threads (N5, N6) are approximate.
 *   restart_*      K1/G5 indexed point/line/triangle LIST draws with primitive restart on (boot_render_restart_diag):
 *                  seen, index ranges scanned, draws whose indices hold the restart index, draws not checked
 *   int_blend_off  G8 pipeline descriptions whose blend was dropped on an integer target (read-ahead ones included)
 *   vfmt_*         K3 vertex attributes (per vertex-state build) only the driver table decodes / with no vertex encoding
 *   stencil_*      N4/G7 stencil draws; back ops (ds[6..7]) / back ref or masks ([1]) differ from the front; either
 *                  with back faces drawn (not culled)
 *   bias_*         N11/G9 polygon-offset draws; of points or lines; with fractional units
 *   viewport_fractional  N10 SetViewports rectangles with a fraction
 *   flush_*        N6 nvnTextureFlushTexels calls / texture ranges pushed
 *   tic_*          N5 registrations compared with the driver's TIC words: checked, identical, source (swizzle) mismatch,
 *                  of those equal to the composed swizzle / to neither rule, driver = ours where composing differs,
 *                  other field mismatch, pool entry not mapped, not queued (queue full) */
typedef struct BootStateDiag {
    uint64_t restart_list_draws, restart_scanned, restart_hits, restart_unscanned, int_blend_off, vfmt_new, vfmt_unsupported;
    uint64_t stencil_draws, stencil_back_ops, stencil_back_values, stencil_back_drawn, bias_draws, bias_not_triangles, bias_fractional;
    uint64_t viewport_fractional, flush_texels, flush_pushed;
    uint64_t tic_checked, tic_same, tic_source_bad, tic_compose, tic_neither, tic_replace, tic_other_bad, tic_unreadable, tic_dropped;
} BootStateDiag;
static BootStateDiag boot_state_diag;
static inline int boot_render_state_diag_on(void) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_STATE_DIAG", v, sizeof v); on = !(k && k < sizeof v && v[0] == '0'); }
    return on;
}
/* Guest GPU VA -> host pointer as boot_render_guest, without its tiling-list guard: a diagnostic never waits (the bytes
 * may predate pending GPU writes). NULL unless the range is contiguous in one region. */
static inline const uint8_t *boot_render_diag_guest(const BootRender *r, uint64_t va, uint64_t size) {
    if ((va >> 12) < r->va_base_page) return NULL;
    uint64_t page = (va >> 12) - r->va_base_page;
    if (page >= r->ptable_pages) return NULL;
    uint32_t e = r->ptable_cpu[page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count) return NULL;
    unsigned region = e >> 24; uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    if (off > r->region_size[region] || size > r->region_size[region] - off) return NULL;
    uint64_t last = size ? ((va & 0xfff) + size - 1) >> 12 : 0;
    for (uint64_t p = 1; p <= last; ++p)
        if (page + p >= r->ptable_pages || r->ptable_cpu[page + p] != e + (uint32_t)p) return NULL;
    return r->region_cpu[region] + off;
}
/* K1/G5 diagnostic: an indexed point/line/triangle LIST draw with primitive restart on whose indices hold the restart
 * index. NVN (GL rules) ends the primitive there and drops its unfinished vertices; D3D12 cuts strips only, so in a list
 * the restart index is drawn as a real vertex (index 0xffff / 0xffffffff, fetched past the stream). nxport (MIT,
 * crates/engine/src/metal/translated.rs:1984-2056) rewrites such index lists; the rewrite waits for this count. A scan
 * is memoised by (first index VA, count, index size, mapping generation) and at most 1 MiB of indices is scanned per
 * present; indirect draws (first/count on the GPU) are counted as not checked. */
#define BOOT_RENDER_RESTART_MEMO 1024
static struct { uint64_t va, gen; uint32_t count, bytes, hit; } boot_render_restart_memo[BOOT_RENDER_RESTART_MEMO];
static uint64_t boot_render_restart_budget;   /* index bytes scanned since the last present */
static inline void boot_render_restart_diag(BootRender *r, const BootDrawState *s, uint32_t idx_bytes, int indirect) {
    BootStateDiag *g = &boot_state_diag; ++g->restart_list_draws;
    if (indirect || !s->count) { ++g->restart_unscanned; return; }
    uint64_t va = s->index_address + (uint64_t)s->first * idx_bytes, bytes = (uint64_t)s->count * idx_bytes;
    unsigned k = (unsigned)((va >> 2) ^ (va >> 15) ^ ((uint64_t)s->count * 0x9e3779b1u)) & (BOOT_RENDER_RESTART_MEMO - 1);
    if (boot_render_restart_memo[k].va == va && boot_render_restart_memo[k].count == s->count && boot_render_restart_memo[k].bytes == idx_bytes &&
        boot_render_restart_memo[k].gen == boot_render_map_gen) { if (boot_render_restart_memo[k].hit) ++g->restart_hits; return; }
    const uint8_t *p = boot_render_restart_budget + bytes <= (UINT64_C(1) << 20) ? boot_render_diag_guest(r, va, bytes) : NULL;
    if (!p) { ++g->restart_unscanned; return; }
    boot_render_restart_budget += bytes; ++g->restart_scanned;
    uint32_t n = 0, at = 0, rv = s->restart_index;
    if (idx_bytes == 1) { for (uint32_t i = 0; i < s->count; ++i) if ((uint32_t)p[i] == rv && !n++) at = i; }
    else if (idx_bytes == 2) { for (uint32_t i = 0; i < s->count; ++i) { uint16_t x; memcpy(&x, p + 2 * (size_t)i, 2); if ((uint32_t)x == rv && !n++) at = i; } }
    else { for (uint32_t i = 0; i < s->count; ++i) { uint32_t x; memcpy(&x, p + 4 * (size_t)i, 4); if (x == rv && !n++) at = i; } }
    boot_render_restart_memo[k].va = va; boot_render_restart_memo[k].count = s->count; boot_render_restart_memo[k].bytes = idx_bytes;
    boot_render_restart_memo[k].gen = boot_render_map_gen; boot_render_restart_memo[k].hit = n != 0;
    if (!n) return;
    ++g->restart_hits;
    static unsigned logged;
    if (logged < 16) {
        ++logged;
        boot_gp_log("nvn native: K1 restart index in an indexed list draw: topology %u, index size %u, restart index %x found %u times (first at %u) "
                    "in first %u count %u, indices at %llx, base vertex %d (submission %llu)", s->topology, idx_bytes, rv, n, at, s->first, s->count,
                    (unsigned long long)va, (int)s->base_vertex_reg, (unsigned long long)boot_render_exec_id);
    }
}
/* vbounds diagnostic (TOTK_STATE_DIAG; log only, changes no draw): the island spikes survived the vertex-fetch bound in
 * play-95, now converging on one point. For each direct draw this finds the highest and lowest vertex the draw fetches
 * (indices scanned once per index range and map generation, like K1, within a per-present byte budget) and compares
 * every enabled per-vertex attribute's byte range with the stream's bound size: PARTIAL = some vertices inside, some past
 * (those read 0 now: triangles pulled to the model origin, visible), FULL = the whole draw past the bound (all vertices
 * read 0: collapses, as on the source GPU). It also flags index ranges that run past the bound index buffer limit. */
#define BOOT_RENDER_VB_MEMO 2048
static volatile LONG boot_render_present_no;   /* call-bind: presents so far (host_nvn.h boot_nvn_ev_present), on these lines to correlate with the call-bind watch */
static struct { uint64_t va, gen; uint32_t count, bytes, lo, hi; } boot_render_vb_memo[BOOT_RENDER_VB_MEMO];
static uint64_t boot_render_vb_budget;   /* index bytes scanned since the last present (reset in boot_nvn_state_diag_present) */
static uint64_t boot_render_vb_checked, boot_render_vb_unscanned, boot_render_vb_partial, boot_render_vb_full, boot_render_vb_idx_past;
static inline uint32_t boot_render_vb_elem_bytes(uint32_t size) {
    switch (size) {
    case 0x01u: return 16u; case 0x02u: return 12u; case 0x03u: case 0x04u: return 8u; case 0x05u: return 6u;
    case 0x0au: case 0x0fu: case 0x12u: case 0x30u: case 0x31u: case 0x2fu: return 4u; case 0x13u: case 0x33u: return 3u;
    case 0x18u: case 0x1bu: case 0x32u: return 2u; case 0x1du: case 0x34u: return 1u; default: return 0u;
    }
}
static inline void boot_render_vbounds_diag(BootRender *r, const BootDrawState *s, const uint8_t *vs_id) {
    uint32_t lo, hi; int32_t base = 0;
    uint32_t idx_bytes = s->index_size == 0 ? 1u : s->index_size == 1 ? 2u : 4u;
    if (s->indexed) {
        uint64_t va = s->index_address + (uint64_t)s->first * idx_bytes, bytes = (uint64_t)s->count * idx_bytes;
        if (s->index_limit >= s->index_address && va + bytes - 1 > s->index_limit) {
            static unsigned logged_idx;
            if (++boot_render_vb_idx_past, logged_idx < 8) { ++logged_idx;
                boot_gp_log("nvn native: vbounds diag INDEX PAST LIMIT: indices %llx..%llx, index buffer limit %llx (first %u count %u size %u, submission %llu, present %ld)",
                            (unsigned long long)va, (unsigned long long)(va + bytes - 1), (unsigned long long)s->index_limit, s->first, s->count, idx_bytes,
                            (unsigned long long)boot_render_exec_id, (long)boot_render_present_no); }
        }
        unsigned k = (unsigned)((va >> 2) ^ (va >> 15) ^ ((uint64_t)s->count * 0x9e3779b1u)) & (BOOT_RENDER_VB_MEMO - 1);
        if (boot_render_vb_memo[k].va == va && boot_render_vb_memo[k].count == s->count && boot_render_vb_memo[k].bytes == idx_bytes &&
            boot_render_vb_memo[k].gen == boot_render_map_gen) { lo = boot_render_vb_memo[k].lo; hi = boot_render_vb_memo[k].hi; }
        else {
            const uint8_t *p = boot_render_vb_budget + bytes <= (UINT64_C(16) << 20) ? boot_render_diag_guest(r, va, bytes) : NULL;
            if (!p) { ++boot_render_vb_unscanned; return; }
            boot_render_vb_budget += bytes; lo = UINT32_MAX; hi = 0;
            for (uint32_t i = 0; i < s->count; ++i) {
                uint32_t x;
                if (idx_bytes == 1) x = p[i];
                else if (idx_bytes == 2) { uint16_t h; memcpy(&h, p + 2 * (size_t)i, 2); x = h; }
                else memcpy(&x, p + 4 * (size_t)i, 4);
                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
            boot_render_vb_memo[k].va = va; boot_render_vb_memo[k].count = s->count; boot_render_vb_memo[k].bytes = idx_bytes;
            boot_render_vb_memo[k].gen = boot_render_map_gen; boot_render_vb_memo[k].lo = lo; boot_render_vb_memo[k].hi = hi;
        }
        base = (int32_t)s->base_vertex_reg;
    } else { lo = s->first; hi = s->first + s->count - 1; }
    ++boot_render_vb_checked;
    int64_t vlo = (int64_t)lo + base, vhi = (int64_t)hi + base;
    for (unsigned i = 0; i < 32; ++i) {
        const BootVertexAttrib *a = &s->attrib[i]; const BootVertexStream *st = &s->stream[a->stream & 15];
        if (a->inactive || !st->enabled || st->per_instance || !st->stride) continue;
        uint32_t size = boot_render_stream_size(st), elem = boot_render_vb_elem_bytes(a->size);
        if (!size || !elem) continue;
        int64_t need_lo = vlo * st->stride + a->offset, need_hi = vhi * st->stride + a->offset + elem;
        int full = need_lo >= (int64_t)size, partial = !full && need_hi > (int64_t)size;
        if (!full && !partial) continue;
        static unsigned logged_partial, logged_full;
        if (full) ++boot_render_vb_full; else ++boot_render_vb_partial;
        if (partial ? logged_partial < 48 : logged_full < 8) {
            if (partial) ++logged_partial; else ++logged_full;
            char vs[17]; for (int b = 0; b < 8; ++b) snprintf(vs + 2 * b, 3, "%02x", vs_id[b]);
            boot_gp_log("nvn native: vbounds diag %s: vs %s attr %u stream %llx size %u stride %u offset %u elem %u | vertices %lld..%lld (indices %u..%u, base %d) "
                        "need %lld bytes | topo %u indexed %u count %u first %u inst %u | totals partial %llu full %llu checked %llu unscanned %llu (submission %llu, present %ld)",
                        partial ? "PARTIAL" : "FULL", vs, i, (unsigned long long)st->address, size, st->stride, a->offset, elem, (long long)vlo, (long long)vhi,
                        lo, hi, base, (long long)need_hi, s->topology, s->indexed, s->count, s->first, s->base_instance_reg,
                        (unsigned long long)boot_render_vb_partial, (unsigned long long)boot_render_vb_full, (unsigned long long)boot_render_vb_checked,
                        (unsigned long long)boot_render_vb_unscanned, (unsigned long long)boot_render_exec_id, (long)boot_render_present_no);
        }
    }
}
/* fmt100 diagnostic (TOTK_STATE_DIAG; log only, changes no draw; 2026-10-22): K3 vertex format 100 (RGBA8 SSCALED, attribute 1)
 * decoded (TOTK_VERTEX_FORMATS=1) removed the high-detail LOD sheets on the Sky Island (play-109 A/B with TOTK_GPU_TS=gpu), but on
 * 2026-10-21 the same decode broke stone-ruin slabs. Each distinct (vertex program, attribute, stream layout) that fetches an
 * 8_8_8_8 SSCALED attribute is logged once (64 at most): stream, offset, stride, divisor, bound size, the draw's shape, every active
 * attribute's format and the attribute's raw signed bytes for the first four elements the draw fetches (indexed draws: from the
 * base vertex, indices not scanned; per-instance streams: from the base instance), so a correct use (trees, terrain) and a broken
 * one (ruins) can be compared. */
static inline void boot_render_fmt100_diag(BootRender *r, const BootDrawState *s, const uint8_t *vs_id) {
    static struct { uint64_t vs, lay; } seen[64]; static unsigned n_seen;
    if (n_seen >= 64) return;
    for (unsigned i = 0; i < 32 && n_seen < 64; ++i) {
        const BootVertexAttrib *a = &s->attrib[i];
        if (a->inactive || a->size != 0x0au || a->type != 6u) continue;
        const BootVertexStream *st = &s->stream[a->stream & 15];
        uint64_t vs; memcpy(&vs, vs_id, 8);
        const uint64_t lay = (uint64_t)i | (uint64_t)(a->stream & 31) << 5 | (uint64_t)a->offset << 10 | (uint64_t)st->stride << 24 | (uint64_t)st->divisor << 40;
        unsigned k = 0;
        while (k < n_seen && !(seen[k].vs == vs && seen[k].lay == lay)) ++k;
        if (k < n_seen) continue;
        seen[n_seen].vs = vs; seen[n_seen].lay = lay; ++n_seen;
        char fm[256]; size_t at = 0; fm[0] = 0;
        for (unsigned j = 0; j < 32 && at + 24 < sizeof fm; ++j) {
            const BootVertexAttrib *b = &s->attrib[j];
            if (!b->inactive) at += (size_t)snprintf(fm + at, sizeof fm - at, " %u:%x/%u@%u+%u", j, b->size, b->type, b->stream & 15, b->offset);
        }
        int8_t sm[16]; memset(sm, 0, sizeof sm); unsigned got = 0;
        if (st->enabled && st->stride) {
            const int64_t first = st->divisor ? (int64_t)s->base_instance_reg : s->indexed ? (int64_t)(int32_t)s->base_vertex_reg : (int64_t)s->first;
            for (unsigned q = 0; q < 4; ++q) {
                const int64_t el = first + (int64_t)q;
                if (el < 0) break;
                const uint8_t *p = boot_render_diag_guest(r, st->address + (uint64_t)el * st->stride + a->offset, 4);
                if (!p) break;
                memcpy(sm + 4 * q, p, 4); ++got;
            }
        }
        char vs_hex[17]; for (int b = 0; b < 8; ++b) snprintf(vs_hex + 2 * b, 3, "%02x", vs_id[b]);
        boot_gp_log("nvn native: fmt100 diag #%u: vs %s attr %u (RGBA8 SSCALED) stream %u va %llx offset %u stride %u divisor %u size %u | draw topo %u indexed %u count %u first %u base %d inst %u | attrs%s | first elements (%u read): (%d,%d,%d,%d) (%d,%d,%d,%d) (%d,%d,%d,%d) (%d,%d,%d,%d) (submission %llu)",
                    n_seen, vs_hex, i, a->stream & 15, (unsigned long long)st->address, a->offset, st->stride, st->divisor, boot_render_stream_size(st),
                    s->topology, s->indexed, s->count, s->first, (int)s->base_vertex_reg, s->base_instance_reg, fm, got,
                    sm[0], sm[1], sm[2], sm[3], sm[4], sm[5], sm[6], sm[7], sm[8], sm[9], sm[10], sm[11], sm[12], sm[13], sm[14], sm[15], (unsigned long long)boot_render_exec_id);
    }
}
/* batch-3 G8: NVN ignores blending on an integer colour target (integer data passes the blend unit unchanged); D3D12
 * refuses a pipeline that enables it there (E_INVALIDARG) and the draw is lost. Blend stays off for UINT/SINT targets,
 * as nxport does (MIT, crates/engine/src/metal/translated.rs:7308-7309: blending only for pf_kind 0). With
 * TOTK_STATE_DIAG, a blend asked on a non-integer format D3D12 does not report as blendable is logged once per format
 * (not changed). TOTK_INT_NO_BLEND=0: blend as asked, as before. */
static inline int boot_render_blend_ignored(BootRender *r, DXGI_FORMAT f) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_INT_NO_BLEND", v, sizeof v); on = !(k && k < sizeof v && v[0] == '0'); }
    int is_signed;
    if (boot_render_dxgi_int_bits(f, &is_signed)) {
        if (!on) return 0;
        static unsigned logged; ++boot_state_diag.int_blend_off;
        if (logged < 8 && boot_render_state_diag_on()) { ++logged; boot_gp_log("nvn native: G8 blend ignored on an integer target (DXGI format %u), as NVN", (unsigned)f); }
        return 1;
    }
    if (boot_render_state_diag_on() && (unsigned)f < 256) {
        static int8_t blendable[256];   /* 0 not asked yet, 1 blendable, -1 not */
        if (!blendable[f]) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT fs; memset(&fs, 0, sizeof fs); fs.Format = f;
            int no = SUCCEEDED(ID3D12Device_CheckFeatureSupport(r->gpu->device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) && !(fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE);
            blendable[f] = no ? -1 : 1;
            if (no) boot_gp_log("nvn native: G8 blend asked on DXGI format %u, which D3D12 does not report as blendable (the pipeline may be refused)", (unsigned)f);
        }
    }
    return 0;
}
/* pso-work: the fixed-function part of a draw's pipeline description (blend, rasterizer, depth/stencil, topology
 * type, strip cut, target formats), moved unchanged out of boot_render_draw_impl99 so the read-ahead
 * (boot_render_pso_predict) derives exactly the description the draw will. rt_on[i] / rt_fmt[i]: colour target i
 * is bound and its DXGI format (a format-NONE slot is a null view); has_zt / zt_fmt the same for depth; ib_fmt the
 * index format the draw binds. Returns 0, or the draw's error code with r->error set. */
static inline int boot_render_pso_fixed99(BootRender *r, const BootDrawState *s, D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, uint32_t ps_outputs,
                                          const uint8_t rt_on[8], const DXGI_FORMAT rt_fmt[8], int has_zt, DXGI_FORMAT zt_fmt,
                                          D3D_PRIMITIVE_TOPOLOGY topo, int needs_expand, int indexed, DXGI_FORMAT ib_fmt, int *replicate_out) {
    *replicate_out = 0;
    d->BlendState.IndependentBlendEnable = TRUE;
    int const_alpha = 0, const_color = 0;   /* constant-alpha / constant-color blend factors in use */
    for (unsigned i = 0; i < s->rt_count; ++i) {
        unsigned bi = s->blend_per_target ? i : 0;
        const BootBlend *b = &s->blend[bi];
        D3D12_RENDER_TARGET_BLEND_DESC *t = &d->BlendState.RenderTarget[i];
        if (!rt_on[i]) { t->RenderTargetWriteMask = 0; continue; }
        t->RenderTargetWriteMask = boot_render_ps_write_mask(ps_outputs, i, s->rt[s->single_ct_write ? 0 : s->rt_map[i]].write_mask);
        if (s->blend[i].enabled && !boot_render_blend_ignored(r, rt_fmt[i])) {   /* batch-3 G8 */
            t->BlendEnable = TRUE;
            t->SrcBlend = boot_render_blend(b->color_src, 0); t->DestBlend = boot_render_blend(b->color_dst, 0); t->BlendOp = boot_render_blend_op(b->color_op);
            if (b->separate_alpha) {
                t->SrcBlendAlpha = boot_render_blend(b->alpha_src, 1); t->DestBlendAlpha = boot_render_blend(b->alpha_dst, 1); t->BlendOpAlpha = boot_render_blend_op(b->alpha_op);
            } else {
                t->SrcBlendAlpha = boot_render_blend(b->color_src, 1); t->DestBlendAlpha = boot_render_blend(b->color_dst, 1); t->BlendOpAlpha = t->BlendOp;
            }
            if (!t->SrcBlend || !t->DestBlend || !t->BlendOp || !t->SrcBlendAlpha || !t->DestBlendAlpha || !t->BlendOpAlpha) {
                static char why[96];
                snprintf(why, sizeof why, "blend factor/op (color %x %x op %x, alpha %x %x op %x)", b->color_src, b->color_dst, b->color_op,
                         b->alpha_src, b->alpha_dst, b->alpha_op);
                return boot_render_fail(r, BOOT_RENDER_E_STATE, why);
            }
            D3D12_BLEND *f4[4] = {&t->SrcBlend, &t->DestBlend, &t->SrcBlendAlpha, &t->DestBlendAlpha};
            for (unsigned k = 0; k < 4; ++k) {
                if (*f4[k] == D3D12_BLEND_ALPHA_FACTOR || *f4[k] == D3D12_BLEND_INV_ALPHA_FACTOR) const_alpha = 1;
                if (k < 2 && (*f4[k] == D3D12_BLEND_BLEND_FACTOR || *f4[k] == D3D12_BLEND_INV_BLEND_FACTOR)) const_color = 1;
            }
        }
    }
    d->SampleMask = UINT_MAX;
    d->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    if (s->cull_enable && s->cull_face == 0x408) return boot_render_fail(r, BOOT_RENDER_E_STATE, "cull front and back");
    d->RasterizerState.CullMode = !s->cull_enable ? D3D12_CULL_MODE_NONE : s->cull_face == 0x404 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
    /* Window coordinates are the source GPU's (boot_render_viewport_map): the front face is decided
     * on the same window-space winding, so only WINDOW_ORIGIN.flip_y swaps it (clb197.h SET_WINDOW_ORIGIN_FLIP_Y;
     * Ryujinx StateUpdater.UpdateFrontFace). */
    d->RasterizerState.FrontCounterClockwise = boot_render_front_ccw(s->front_face, s->flip_y);
    d->RasterizerState.DepthClipEnable = s->depth_clamp ? FALSE : TRUE;   /* depth clamp = no near/far clipping, depth clamped to the viewport range */
    if (s->depth_bias_fill) {
        d->RasterizerState.DepthBias = (INT)s->depth_bias; d->RasterizerState.SlopeScaledDepthBias = s->depth_bias_slope;
        d->RasterizerState.DepthBiasClamp = s->depth_bias_clamp;
    }
    if (has_zt) {
        d->DSVFormat = zt_fmt;
        d->DepthStencilState.DepthEnable = s->depth_test ? TRUE : FALSE;
        d->DepthStencilState.DepthWriteMask = (s->depth_test && s->depth_write) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d->DepthStencilState.DepthFunc = s->depth_test ? boot_render_cmp(s->depth_func) : D3D12_COMPARISON_FUNC_ALWAYS;
        if (!d->DepthStencilState.DepthFunc) return boot_render_fail(r, BOOT_RENDER_E_STATE, "depth func");
        if (s->stencil_test) {
            D3D12_DEPTH_STENCILOP_DESC o = {boot_render_stencil_op(s->stencil_fail), boot_render_stencil_op(s->stencil_zfail),
                                            boot_render_stencil_op(s->stencil_zpass), boot_render_cmp(s->stencil_func)};
            if (!o.StencilFailOp || !o.StencilDepthFailOp || !o.StencilPassOp || !o.StencilFunc) return boot_render_fail(r, BOOT_RENDER_E_STATE, "stencil op");
            d->DepthStencilState.StencilEnable = TRUE; d->DepthStencilState.FrontFace = o; d->DepthStencilState.BackFace = o;
            d->DepthStencilState.StencilReadMask = (UINT8)s->stencil_func_mask; d->DepthStencilState.StencilWriteMask = (UINT8)s->stencil_mask;
        }
    } else if (s->depth_test || s->stencil_test) {
        /* depth test with no depth target bound behaves as always-pass on the source */
    }
    d->PrimitiveTopologyType = (topo == D3D_PRIMITIVE_TOPOLOGY_POINTLIST) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT :
                              (topo == D3D_PRIMITIVE_TOPOLOGY_LINELIST || topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE :
                              D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (s->primitive_restart && indexed && !needs_expand && (topo == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP || topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP))
        d->IBStripCutValue = ib_fmt == DXGI_FORMAT_R16_UINT ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    d->NumRenderTargets = s->rt_count;
    for (unsigned i = 0; i < s->rt_count; ++i) d->RTVFormats[i] = rt_on[i] ? rt_fmt[i] : DXGI_FORMAT_R8G8B8A8_UNORM;   /* null view format */
    d->SampleDesc.Count = 1;
    /* Without device support for the alpha factor, constant alpha becomes the blend factor with
     * the constant's alpha replicated (bound below); that cannot coexist with constant colour. */
    static int alpha_factor_support = -1;
    if (const_alpha && alpha_factor_support < 0) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS13 o13 = {0};
        alpha_factor_support = SUCCEEDED(ID3D12Device_CheckFeatureSupport(r->gpu->device, D3D12_FEATURE_D3D12_OPTIONS13, &o13, sizeof o13)) &&
                               o13.AlphaBlendFactorSupported;
    }
    int replicate_alpha = const_alpha && !alpha_factor_support;
    *replicate_out = replicate_alpha;
    if (replicate_alpha) {
        if (const_color) return boot_render_fail(r, BOOT_RENDER_E_STATE, "constant colour and constant alpha blend without alpha-factor support");
        for (unsigned i = 0; i < 8; ++i) {
            D3D12_RENDER_TARGET_BLEND_DESC *t = &d->BlendState.RenderTarget[i];
            D3D12_BLEND *f4[4] = {&t->SrcBlend, &t->DestBlend, &t->SrcBlendAlpha, &t->DestBlendAlpha};
            for (unsigned k = 0; k < 4; ++k) {
                if (*f4[k] == D3D12_BLEND_ALPHA_FACTOR) *f4[k] = D3D12_BLEND_BLEND_FACTOR;
                if (*f4[k] == D3D12_BLEND_INV_ALPHA_FACTOR) *f4[k] = D3D12_BLEND_INV_BLEND_FACTOR;
            }
        }
    }
    return 0;
}
/* pso-work: the pipeline key (shader ids and every fixed-function field), as boot_render_draw_impl99 builds it. */
static inline size_t boot_psoj_key(uint8_t *buf, const uint8_t vs_id[32], const uint8_t ps_id[32], const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d) {
    size_t k = 0;
    memcpy(buf + k, vs_id, 32); k += 32; memcpy(buf + k, ps_id, 32); k += 32;
    memcpy(buf + k, &d->BlendState, sizeof d->BlendState); k += sizeof d->BlendState;
    memcpy(buf + k, &d->RasterizerState, sizeof d->RasterizerState); k += sizeof d->RasterizerState;
    memcpy(buf + k, &d->DepthStencilState, sizeof d->DepthStencilState); k += sizeof d->DepthStencilState;
    memcpy(buf + k, &d->PrimitiveTopologyType, 4); k += 4; memcpy(buf + k, &d->IBStripCutValue, 4); k += 4;
    memcpy(buf + k, &d->DSVFormat, 4); k += 4; memcpy(buf + k, d->RTVFormats, 32); k += 32;
    {   uint64_t g = boot_pt_gs_key(d->GS.pShaderBytecode); if (g) { memcpy(buf + k, &g, 8); k += 8; } }   /* G2: the point-sprite GS layout */
    return k;
}
/* 1 when the draw path already has the pipeline (r->psos) or the pre-creation table knows the key. */
static inline int boot_psoj_known(BootRender *r, const uint8_t *buf, size_t k) {
    uint64_t h = UINT64_C(1469598103934665603);   /* FNV-1a, as the draw path's index */
    for (size_t i = 0; i < k; ++i) { h ^= buf[i]; h *= UINT64_C(1099511628211); }
    for (unsigned x = (unsigned)(h % BOOT_RENDER_PSO_INDEX), n = 0; n < BOOT_RENDER_PSO_INDEX && boot_render_pso_index[x]; x = (x + 1) % BOOT_RENDER_PSO_INDEX, ++n) {
        unsigned i = boot_render_pso_index[x] - 1; uint64_t p0; memcpy(&p0, r->psos[i].key, 8);
        if (i < r->pso_count && p0 == h && boot_render_pso_key_size[i] == k && !memcmp(boot_render_pso_keys[i], buf, k)) return 1;
    }
    return boot_render_pso_pre_known(buf, (uint32_t)k);
}
/* A program id for the read-ahead that leaves the draw path's program cache alone: one of its entries is used when
 * its bytes still match guest memory, otherwise the id (SHA-256 of header and code up to the terminator, as
 * boot_render_program_id) is computed from guest memory and kept for this read-ahead only. */
static inline int boot_psoj_program_id(BootRender *r, uint64_t va, uint8_t id[32]) {
    unsigned way = (unsigned)((va >> 6) ^ (va >> 17)) & (BOOT_RENDER_PROGRAM_CACHE - 1); const uint8_t *q;
    if (boot_render_program_cache[way].bytes && boot_render_program_cache[way].va == va &&
        (q = boot_render_guest(r, va, boot_render_program_cache[way].size > 0x80 ? boot_render_program_cache[way].size : 0x80)) != NULL &&
        !memcmp(q, boot_render_program_cache[way].bytes, boot_render_program_cache[way].size)) { memcpy(id, boot_render_program_cache[way].id, 32); return 1; }
    unsigned slot = (unsigned)(((va >> 4) * UINT64_C(0x9e3779b97f4a7c15)) >> 55) & (BOOT_PSOJ_IDS - 1);
    if (boot_psoj_ids[slot].walk == boot_psoj_walk_no && boot_psoj_ids[slot].va == va) { memcpy(id, boot_psoj_ids[slot].id, 32); return 1; }
    const uint8_t *p = boot_render_guest(r, va, 0x80);
    if (!p) return 0;
    const uint8_t *code = p + 0x50;
    for (uint64_t i = 0; i < (1u << 20); ++i) {
        uint64_t addr = i * 8; if (addr % 32 == 0) continue;
        const uint8_t *wp = boot_render_guest(r, va + 0x50 + addr, 8);
        if (!wp || wp != code + addr) return 0;
        uint64_t w; memcpy(&w, wp, 8);
        if ((w & (UINT64_C(0xfff0) << 48 | 0x20 | (UINT64_C(0xf) << 16) | 0x1f)) == ((UINT64_C(0xe240) << 48) | (UINT64_C(0x7) << 16) | 0xf)) {
            int64_t off = (int64_t)((w >> 20) & 0xffffff); if (off & 0x800000) off -= 0x1000000;
            int64_t target = (int64_t)addr + 8 + off;
            if (target == (int64_t)addr || (target % 32 == 0 && target + 8 == (int64_t)addr)) {
                boot_render_sha256(p, 0x50 + (size_t)addr + 8, id);   /* header and code are contiguous here (checked above) */
                boot_psoj_ids[slot].va = va; boot_psoj_ids[slot].walk = boot_psoj_walk_no; memcpy(boot_psoj_ids[slot].id, id, 32);
                return 1;
            }
        }
    }
    return 0;
}
/* pso-work: the pipeline a later draw of this submission will need, from the read-ahead's draw state (host_nvn_play.h
 * boot_nvn_la_state), derived as boot_render_draw_impl99 derives it (same shaders, target formats, topology and
 * boot_render_pso_fixed99); queued when it exists nowhere yet. 1 queued, 0 known or not derivable, -1 no room. */
static inline int boot_render_pso_predict(BootRender *r, const BootDrawState *s) {
    boot_psoj_walk_draw();
    if (s->unsupported || s->stage_enabled[0] || !s->stage_enabled[1] || boot_render_pso_owner != r || s->rt_count > 8) return 0;
    int has_ps = s->stage_enabled[5] != 0;
    uint8_t vs_id[32], ps_id[32]; memset(ps_id, 0, sizeof ps_id);
    const uint64_t vs_va = s->program_base + s->stage_offset[1], ps_va = s->program_base + s->stage_offset[5];
    if (!boot_psoj_program_id(r, vs_va, vs_id) || (has_ps && !boot_psoj_program_id(r, ps_va, ps_id))) return 0;
    const uint8_t *vsph = boot_render_guest(r, vs_va, 0x50), *psph = has_ps ? boot_render_guest(r, ps_va, 0x50) : NULL;
    if (!vsph || (has_ps && !psph)) return 0;
    uint32_t vs_mask = 0;   /* VS output map selects the PS variant (as the draw) */
    for (unsigned g = 0; g < 32; ++g) if ((vsph[0x36 + g / 2] >> ((g & 1) * 4)) & 15) vs_mask |= 1u << g;
    uint32_t ps_outputs = has_ps ? boot_render_ps_output_map(psph, 0x50) : 0;
    BootRenderShader *vs = boot_render_shader(r, vs_id, 0, 0), *ps = has_ps ? boot_render_shader(r, ps_id, 1, vs_mask) : NULL;
    if (!vs || (has_ps && !ps)) return 0;
    uint8_t rt_on[8] = {0}; DXGI_FORMAT rt_fmt[8] = {DXGI_FORMAT_UNKNOWN};
    for (unsigned i = 0; i < s->rt_count; ++i) {   /* boot_render_target: format NONE is a null view, else the format's DXGI */
        const BootColorTarget *c = &s->rt[s->rt_map[i]];
        if (c->format == 0) continue;
        if (!boot_render_ct_format(c->format, &rt_fmt[i]) || !c->width || !c->height) return 0;
        rt_on[i] = 1;
    }
    int has_zt = 0; DXGI_FORMAT zt_fmt = DXGI_FORMAT_UNKNOWN;
    if (s->zt.enabled) { if (!boot_render_zt_format(s->zt.format, &zt_fmt) || !s->zt.width || !s->zt.height) return 0; has_zt = 1; }
    static const D3D_PRIMITIVE_TOPOLOGY direct[6] = {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
        D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP};
    D3D_PRIMITIVE_TOPOLOGY topo = s->topology < 6 ? direct[s->topology] : D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    int needs_expand = topo == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (needs_expand) {   /* boot_render_expand: line loops become strips, quads / fans / polygons / quad strips lists */
        if (s->topology != 2 && (s->topology < 6 || s->topology > 9)) return 0;
        topo = s->topology == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    uint32_t idx_bytes = s->index_size == 0 ? 1 : s->index_size == 1 ? 2 : 4;
    DXGI_FORMAT ib_fmt = DXGI_FORMAT_UNKNOWN;   /* expanded: 32-bit; 8-bit indices are widened to 16 */
    if (needs_expand) ib_fmt = DXGI_FORMAT_R32_UINT;
    else if (s->indexed) ib_fmt = idx_bytes == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
    d.pRootSignature = r->root; d.VS.pShaderBytecode = vs->dxil; d.VS.BytecodeLength = vs->size;
    if (ps) { d.PS.pShaderBytecode = ps->dxil; d.PS.BytecodeLength = ps->size; }
    int replicate_alpha = 0;
    if (boot_render_pso_fixed99(r, s, &d, ps_outputs, rt_on, rt_fmt, has_zt, zt_fmt, topo, needs_expand, (int)s->indexed, ib_fmt, &replicate_alpha)) return 0;
    boot_pt_apply(r->manifest_path, &d);   /* G2: as the draw (boot_pt_apply above) */
    uint8_t buf[BOOT_RENDER_PSO_KEY]; size_t k = boot_psoj_key(buf, vs_id, ps_id, &d);
    if (boot_psoj_known(r, buf, k)) return 0;
    return boot_psoj_enqueue(r, &d, buf, k, 1);
}
/* suspect-diag (host_gpu_render.h, top): the draw in flight. After the programs are known: its vertex shader, family and the
 * draw-census key (the same hash as host_draw_census.h boot_census_draw_slow, so a key greps frames\draw-census.log). */
static inline void boot_sd_draw_begin(const BootDrawState *s, const uint8_t *vs_id, const uint8_t *ps_id) {
    BootSdState *S = boot_sd_state(); if (!S) return;
    if (boot_render_exec_id != S->exec_last) { S->exec_last = boot_render_exec_id; S->draw = 0; }
    ++S->draw; S->in_draw = 1; S->draw_copies = S->draw_direct = 0;
    uint64_t vt, pt = 0; memcpy(&vt, vs_id, 8); if (ps_id) memcpy(&pt, ps_id, 8);
    S->vs8 = vt; S->fam = boot_sd_family(S, vt); S->material = !strcmp(S->fam, "material");
    uint64_t h = boot_census_mix(1, vt);
    h = boot_census_mix(h, pt); h = boot_census_mix(h, s->stream[0].address); h = boot_census_mix(h, s->index_address);
    h = boot_census_mix(h, ((uint64_t)s->first << 32) | s->count); h = boot_census_mix(h, s->base_vertex_reg);
    h = boot_census_mix(h, s->rt_count ? s->rt[s->rt_map[0]].address : (s->zt.enabled ? s->zt.address : 0));
    S->key = boot_census_final(h);
}
/* After the vertex attribute table: S2 (K2), S4 (the vertex shader's bound texture slots), S1 (vertex stream heads). */
static inline void boot_sd_draw_vs(BootRender *r, const BootDrawState *s, const BootRenderShader *vs, int indirect, uint32_t instances,
                                   const uint32_t *handles, const uint8_t *handle_valid) {
    BootSdState *S = boot_sd_state(); if (!S || !S->in_draw) return;
    for (unsigned i = 0; vs && i < vs->tex_count && i < 64; ++i) {   /* S4 */
        if (!handle_valid[i]) continue;
        const uint32_t tic = handles[i] & 0xfffffu;
        if (tic >= BOOT_SD_TIC) continue;
        S->vs_tex[tic] = (uint32_t)S->frame + 1;
        S->vs_tex_va[tic] = boot_render_tex_slots ? boot_render_tex_slots[tic].va : 0;
    }
    ++S->c.k2_draws;   /* S2 */
    const BootSdVs *info = boot_sd_vs_info(S, S->vs8);
    if (indirect) ++S->c.k2_indirect;   /* base instance written by the GPU: unknown here */
    else if (s->base_instance_reg) {
        ++S->c.k2_base;
        if (!info || (info->flags & 8)) ++S->c.k2_unknown;
        else if (!(info->flags & 4)) ++S->c.k2_pre;
        else {
            char attrs[160]; size_t at = 0; unsigned div = 0, freq0 = 0; attrs[0] = 0;
            for (unsigned g = 0; g < 32; ++g) {
                if (!(info->attr_mask >> g & 1)) continue;
                const BootVertexAttrib *a = &s->attrib[g]; const BootVertexStream *st = &s->stream[a->stream & 15];
                if (a->inactive || !st->enabled || !st->per_instance) continue;
                const uint32_t d = boot_render_attrib_divisor(st);
                if (d <= 1) continue;
                if (d == 0xffffffffu) ++freq0; else ++div;
                if (at + 24 < sizeof attrs) at += (size_t)snprintf(attrs + at, sizeof attrs - at, " a%u:%s", g, d == 0xffffffffu ? "freq0" : "div");
                if (at + 24 < sizeof attrs && d != 0xffffffffu) at += (size_t)snprintf(attrs + at, sizeof attrs - at, "%u", d);
            }
            const int iid = (info->flags & 2) != 0;
            if (iid || div || freq0) {
                ++S->c.k2_changed; if (S->material) ++S->c.k2_changed_mat;
                if (iid) ++S->c.k2_iid;
                if (div) ++S->c.k2_div;
                if (freq0) ++S->c.k2_freq0;
                if (S->ev_s2 < S->events_max) {
                    ++S->ev_s2;
                    boot_sd_event(S, "S2 K2 CHANGES frame %llu sub %llu draw %u: vs %016llx family %s key %016llx base instance %u instances %u%s per-instance attrs:%s",
                                   (unsigned long long)S->frame, (unsigned long long)boot_render_exec_id, S->draw, (unsigned long long)__builtin_bswap64(S->vs8), S->fam,
                                   (unsigned long long)S->key, s->base_instance_reg, instances, iid ? " reads InstanceId," : "", at ? attrs : " none");
                }
            }
        }
    }
    uint32_t done = 0;   /* S1: the first 4 KiB of each stream an attribute of this vertex shader fetches */
    for (unsigned g = 0; g < 32; ++g) {
        if (info && !(info->flags & 8) && !(info->attr_mask >> g & 1)) continue;
        const BootVertexAttrib *a = &s->attrib[g]; const unsigned k = a->stream & 15; const BootVertexStream *st = &s->stream[k];
        if (a->inactive || !st->enabled || !st->address || (done >> k & 1)) continue;
        done |= 1u << k;
        uint64_t n = 4096 - (st->address & 0xfff);
        const uint32_t bound = boot_render_stream_size(st);
        if (bound && bound < n) n = bound;
        const uint8_t *p = boot_sd_guest(r, st->address, n, NULL);
        if (p && (boot_ds_vmask >> k & 1)) boot_sd_note(r, BOOT_SD_K_VSNAP, st->address, p, boot_ds_vcopy_cpu[k], n);   /* data-snapshot: the copy the GPU reads */
        else if (p) boot_sd_note(r, BOOT_SD_K_VERTEX, st->address, p, NULL, n);
    }
}
/* End of the draw (boot_render_draw): snapshot / direct constant buffer counts. */
static inline void boot_sd_draw_end(void) {
    BootSdState *S = boot_sd_st; if (!S || !S->in_draw) return;
    ++S->c.draws; if (S->material) ++S->c.draws_mat;
    if (S->draw_copies) { ++S->c.draws_copy; if (S->material) ++S->c.draws_mat_copy; }
    if (S->draw_direct) ++S->c.draws_direct;
    S->in_draw = 0; S->vs8 = 0; S->key = 0; S->fam = "-"; S->material = 0;
}
/* Every present of the native path (host_nvn_play.h, renderer lock held; frames: the frame-N.bmp number of the next frame):
 * the frame number, S3's turn, and every 300 presents one line per suspect (here and in draw-path.log). */
static inline void boot_sd_present(uint64_t frames) {
    BootSdState *S = boot_sd_state(); if (!S) return;
    S->frame = frames; boot_sd_tag_frame = (LONG64)frames; ++S->presents;
    if (S->presents % S->gr_every == 0) S->gr_due = 1;
    if (S->presents % 300 == 0) {
        const BootSdCount *c = &S->c, *l = &S->l; char line[2400]; size_t at = 0;
#define BOOT_SD_D(x) (unsigned long long)(c->x - l->x)
        uint64_t ck = 0, cp = 0, tk = 0, tp = 0;
        for (unsigned k = 0; k < BOOT_SD_K_N; ++k) { ck += c->ch_kept[k] - l->ch_kept[k]; cp += c->ch_plain[k] - l->ch_plain[k]; tk += c->ch_kept[k]; tp += c->ch_plain[k]; }
        at += (size_t)snprintf(line + at, sizeof line - at, "suspect-diag S1 rerecord-version frame %llu (nvn present %ld), last 300 presents: calls %llu (keeping versions %llu, versions kept %llu, re-records %llu); "
                               "RANGES CHANGED during the call: in calls keeping versions %llu, in other calls %llu; per kind checked/changed-keeping/changed-other/host-written/region-gone/unsampled/dup:",
                               (unsigned long long)S->frame, (long)boot_render_present_no, BOOT_SD_D(calls), BOOT_SD_D(calls_kept), BOOT_SD_D(kept), BOOT_SD_D(rerecords),
                               (unsigned long long)ck, (unsigned long long)cp);
        for (unsigned k = 0; k < BOOT_SD_K_N && at < sizeof line; ++k)
            at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu/%llu/%llu/%llu/%llu/%llu/%llu", boot_sd_kind_name[k], BOOT_SD_D(checked[k]),
                                   BOOT_SD_D(ch_kept[k]), BOOT_SD_D(ch_plain[k]), BOOT_SD_D(hostw[k]), BOOT_SD_D(gone[k]), BOOT_SD_D(unsampled[k]), BOOT_SD_D(dup[k]));
        if (at < sizeof line) at += (size_t)snprintf(line + at, sizeof line - at, "; draws %llu, with a CB snapshot copy %llu, with a direct CBV %llu (material draws %llu, with a copy %llu); "
                                                     "re-check %.2f ms; totals: changed keeping %llu, other %llu", BOOT_SD_D(draws), BOOT_SD_D(draws_copy), BOOT_SD_D(draws_direct),
                                                     BOOT_SD_D(draws_mat), BOOT_SD_D(draws_mat_copy), (double)(c->recheck_qpc - l->recheck_qpc) * 1000.0 / (double)S->qpf,
                                                     (unsigned long long)tk, (unsigned long long)tp);
        boot_sd_event(S, "%s", line); boot_gp_log("%s", line);
        snprintf(line, sizeof line, "suspect-diag S2 k2-instance-id frame %llu, last 300 presents: draws %llu, base instance != 0 %llu (indirect, base unknown %llu); "
                                    "K2 changes the result %llu (material %llu; reads InstanceId %llu, divisor > 1 %llu, frequency 0 %llu); vertex shader without K2 %llu, "
                                    "no .hlsl found %llu; totals: changes %llu (TOTK_INSTANCE_ID_ZERO_BASED %s)",
                 (unsigned long long)S->frame, BOOT_SD_D(k2_draws), BOOT_SD_D(k2_base), BOOT_SD_D(k2_indirect), BOOT_SD_D(k2_changed), BOOT_SD_D(k2_changed_mat),
                 BOOT_SD_D(k2_iid), BOOT_SD_D(k2_div), BOOT_SD_D(k2_freq0), BOOT_SD_D(k2_pre), BOOT_SD_D(k2_unknown), (unsigned long long)c->k2_changed,
                 boot_render_iid_offset(0) == 0 ? "on" : "off (the old rule runs)");
        boot_sd_event(S, "%s", line); boot_gp_log("%s", line);
        snprintf(line, sizeof line, "suspect-diag S3 gpu-resident frame %llu, last 300 presents: checks %llu, pages compared %llu (%llu KiB), span views compared %llu; "
                                    "BYTE MISMATCHES %llu, page-table CPU/GPU copy differ %llu, entry not the mapping's region/offset %llu (bytes still equal %llu), "
                                    "unmapped %llu, unreadable %llu, view without section %llu; check %.3f ms avg, %.3f ms max; totals: mismatches %llu (TOTK_GPU_RESIDENT %d)",
                 (unsigned long long)S->frame, BOOT_SD_D(gr_checks), BOOT_SD_D(gr_pages), BOOT_SD_D(gr_bytes) >> 10, BOOT_SD_D(gr_views), BOOT_SD_D(gr_mismatch),
                 BOOT_SD_D(gr_ptcopy), BOOT_SD_D(gr_ptother), BOOT_SD_D(gr_ptother_eq), BOOT_SD_D(gr_unmapped), BOOT_SD_D(gr_unreadable), BOOT_SD_D(gr_nosec),
                 c->gr_checks > l->gr_checks ? (double)(c->gr_qpc - l->gr_qpc) * 1000.0 / (double)S->qpf / (double)(c->gr_checks - l->gr_checks) : 0.0,
                 (double)c->gr_maxq * 1000.0 / (double)S->qpf, (unsigned long long)c->gr_mismatch, boot_gr_mode());
        boot_sd_event(S, "%s", line); boot_gp_log("%s", line);
        snprintf(line, sizeof line, "suspect-diag S4 tex-live-trim frame %llu, last 300 presents: evictions %llu (live trim %llu), of slots a vertex shader bound in the last %u presents %llu; "
                                    "builds %llu, rebuilds of such slots (same address) %llu (after a trim eviction %llu), slots reused for another image %llu; "
                                    "totals: VS evictions %llu, VS rebuilds after eviction %llu (TOTK_TEX_LIVE_TRIM %s, min age %u)",
                 (unsigned long long)S->frame, BOOT_SD_D(tx_evict), BOOT_SD_D(tx_evict_live), S->tex_n, BOOT_SD_D(tx_evict_vs), BOOT_SD_D(tx_builds),
                 BOOT_SD_D(tx_rebuild_vs), BOOT_SD_D(tx_rebuild_vs_ev), BOOT_SD_D(tx_reuse_vs), (unsigned long long)c->tx_evict_vs,
                 (unsigned long long)c->tx_rebuild_vs_ev, boot_render_tex_live_trim_on() ? "on" : "off", boot_render_tex_min_age());
        boot_sd_event(S, "%s", line); boot_gp_log("%s", line);
#undef BOOT_SD_D
        S->l = S->c; S->c.gr_maxq = 0; S->l.gr_maxq = 0;
    }
    if (S->log && S->dirty) { fflush(S->log); S->dirty = 0; }
}
/* data-snapshot (host_gpu_render.h, the design note before host_gpu_render_draw.h): the draw path's vertex streams and index
 * buffers (here, after boot_render_vertex_bounds / boot_render_stream_size). boot_ds_cb_plan ran first for the draw (it folded the
 * GPU-ordered writes recorded since the previous draw into boot_ds_pend). */
/* Vertex streams: a stream the snapshot takes is copied into the arena part of the open list's segment (shared by the list's
 * draws while no host write touched it) and every enabled attribute of the stream reads the copy. Per stream a memo: the same
 * stream (address, size) as the previous draw's, in the same call, with no class change, that stayed zero-copy stays so. */
static inline void boot_ds_vertex(BootRender *r, const BootDrawState *s, uint32_t *attribs) {
    boot_ds_vmask = 0;
    if (!boot_ds_on()) return;
    const int draw_snap = boot_ds_draw_cb;   /* (the plan's answer: v2 OR-ed the 36 slots again) */
    const int timed = !(boot_ds_plan_n & 63); LARGE_INTEGER t0, t1; if (timed) { QueryPerformanceCounter(&t0); boot_ds_plan_timing = 2; }
    if (boot_ds_arena.cpu && r->recording && boot_render_seg.r == r && boot_render_seg.cur < BOOT_DS_PARTS && boot_render_vertex_bounds()) {
        uint32_t used = 0;
        for (unsigned i = 0; i < 32; ++i) if (attribs[i * 8 + 4] >> 31) used |= 1u << (s->attrib[i].stream & 15);
        const uint64_t call = boot_ds_core.call, gen = boot_ds_cls_gen;
        for (unsigned k = 0; k < 16 && used >> k; ++k) {
            if (!(used >> k & 1)) continue;
            const BootVertexStream *st = &s->stream[k];
            if (!st->enabled || !st->address) continue;
            const uint32_t size = boot_render_stream_size(st);
            if (!size) { ++boot_ds_c.unbounded; continue; }
            const uint32_t inst = st->per_instance && size <= boot_ds_cfg.instance_cap;
            __typeof__(boot_ds_vmemo[0]) *vm = &boot_ds_vmemo[k];
            if (vm->va == st->address && vm->size == size && vm->call == call && vm->gen == gen && vm->inst == inst) { ++boot_ds_c.memo_hits; continue; }
            if (size > boot_ds_cfg.stream_cap) {
                ++boot_ds_c.too_large;
                if (boot_ds_det && boot_ds_det->cap) boot_ds_sample(r, BOOT_DS_K_VERTEX, st->address, 4096 - (st->address & 0xfff), boot_ds_cls_at(&boot_ds_core, st->address));
                vm->va = st->address; vm->size = size; vm->call = call; vm->gen = gen; vm->inst = inst;
                continue;
            }
            const int t = boot_ds_decide(r, BOOT_DS_K_VERTEX, st->address, size, (int)inst);
            if (t == 0) { vm->va = st->address; vm->size = size; vm->call = call; vm->gen = gen; vm->inst = inst; continue; }   /* zero-copy by class */
            vm->call = 0;
            if (t < 0) continue;
            __typeof__(boot_ds_vc[0]) *c = &boot_ds_vc[boot_ds_hash32(st->address ^ ((uint64_t)size << 40)) & (BOOT_DS_VCACHE - 1)];
            uint64_t off;
            if (c->va == st->address && c->n == size && c->list == boot_render_list_id && boot_render_cb_clean(st->address, size, c->epoch + 1)) off = c->off;
            else {
                const uint8_t *live = boot_ds_guest(r, st->address, size, NULL);
                if (!live) { ++boot_ds_c.split; continue; }
                off = boot_ds_arena_alloc(&boot_ds_arena, boot_render_seg.cur, boot_render_list_id, size);
                if (off == UINT64_MAX) { ++boot_ds_c.arena_full; continue; }
                LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
                const uint64_t sh = boot_ds_fill_core(&boot_ds_core, boot_ds_arena.cpu + off, st->address, live, size, boot_render_map_gen, boot_render_big_writes, boot_ds_clean_fn, NULL);
                QueryPerformanceCounter(&q1); boot_ds_c.q_copy += (uint64_t)(q1.QuadPart - q0.QuadPart);
                ++boot_ds_c.copies[BOOT_DS_K_VERTEX]; boot_ds_c.bytes[BOOT_DS_K_VERTEX] += size; boot_ds_c.shadow_bytes[BOOT_DS_K_VERTEX] += sh;
                if (boot_ds_det) boot_ds_det->call_bytes += size;
                c->va = st->address; c->n = size; c->list = boot_render_list_id; c->epoch = boot_render_cb_epoch; c->off = off;
            }
            ++boot_ds_c.bind_cb[BOOT_DS_K_VERTEX];
            const uint64_t nva = boot_ds_arena.va + off;
            for (unsigned i = 0; i < 32; ++i)
                if ((attribs[i * 8 + 4] >> 31) && (s->attrib[i].stream & 15) == k) { attribs[i * 8] = (uint32_t)nva; attribs[i * 8 + 1] = (uint32_t)(nva >> 32); }
            boot_ds_vmask |= 1u << k; boot_ds_vcopy_cpu[k] = boot_ds_arena.cpu + off;
        }
    }
    if (timed) { QueryPerformanceCounter(&t1); boot_ds_c.q_vtx += (uint64_t)(t1.QuadPart - t0.QuadPart); ++boot_ds_c.vtx_timed; ++boot_ds_c.timer_intervals; boot_ds_plan_timing = 0; }   /* its own figure (v2fix) */
    if (draw_snap || boot_ds_vmask) ++boot_ds_c.draws_snap;
}
/* Zero-copy index buffer of a direct indexed draw: [va, va + n). A range the snapshot takes is copied into the ring and the
 * view points there (the draw's ring room covers 8 bytes per index). S1 notes both kinds (index-zc, index-snap). */
static inline void boot_ds_index(BootRender *r, uint64_t va, uint64_t n, D3D12_INDEX_BUFFER_VIEW *ib) {
    const uint8_t *live = NULL; uint8_t *dst = NULL;
    int t = boot_ds_on() && n && n <= boot_ds_cfg.stream_cap && boot_ds_decide(r, BOOT_DS_K_INDEX, va, n, 0) > 0;
    const uint64_t bytes = (n + 255) & ~UINT64_C(255);
    if (t && r->ring_used + bytes > r->ring_size) { ++boot_ds_c.ring_full; t = 0; }
    if (t && !(live = boot_ds_guest(r, va, n, NULL))) { ++boot_ds_c.split; t = 0; }
    if (t) {
        dst = r->ring_cpu + r->ring_used;
        LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
        const uint64_t sh = boot_ds_fill_core(&boot_ds_core, dst, va, live, n, boot_render_map_gen, boot_render_big_writes, boot_ds_clean_fn, NULL);
        QueryPerformanceCounter(&q1); boot_ds_c.q_copy += (uint64_t)(q1.QuadPart - q0.QuadPart);
        ib->BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used; r->ring_used += bytes;
        ++boot_ds_c.copies[BOOT_DS_K_INDEX]; ++boot_ds_c.bind_cb[BOOT_DS_K_INDEX]; boot_ds_c.bytes[BOOT_DS_K_INDEX] += n; boot_ds_c.shadow_bytes[BOOT_DS_K_INDEX] += sh;
        if (boot_ds_det) boot_ds_det->call_bytes += n;
    }
    if (boot_sd_on() && n <= 0xffffffffu) {   /* suspect-diag S1: what the GPU reads later (zero-copy) or the copy it reads */
        const uint8_t *p = t ? live : boot_sd_guest(r, va, n, NULL);
        if (p) boot_sd_note(r, t ? BOOT_SD_K_ISNAP : BOOT_SD_K_IZC, va, p, t ? dst : NULL, n);
    }
}
static inline int boot_render_draw_impl99(BootRender *r, const BootRenderDraw *in) {
    const UINT draw_instances = boot_render_next_instances ? boot_render_next_instances : 1; boot_render_next_instances = 0;
    /* render-38d: an indirect draw whose arguments the GPU reads (ExecuteIndirect); s->first/count are placeholders */
    BootRenderIndOp ind = boot_render_next_ind; boot_render_next_ind.kind = 0;
    if (ind.kind != BOOT_RENDER_IND_DRAW && ind.kind != BOOT_RENDER_IND_INDEXED) ind.kind = 0;
    const BootDrawState *s = in->state;
    uint64_t cbv_tap_token=0;
    if (s->unsupported) { snprintf(r->error, sizeof r->error, "unsupported register state at method 0x%x", s->unsupported); return BOOT_RENDER_E_STATE; }
    /* Vertex B is required; vertex A (the A+B combined form) is not supported. A draw without a
     * pixel program (depth-only passes) uses a null D3D12 pixel shader. */
    if (s->stage_enabled[0] || !s->stage_enabled[1]) {
        snprintf(r->error, sizeof r->error, "pipeline stages unsupported: enabled %d%d%d%d%d%d", s->stage_enabled[0], s->stage_enabled[1],
                 s->stage_enabled[2], s->stage_enabled[3], s->stage_enabled[4], s->stage_enabled[5]);
        return BOOT_RENDER_E_STATE;
    }
    int has_ps = s->stage_enabled[5];
    if (!s->count) return BOOT_RENDER_OK;
    uint64_t ph = boot_render_ph_now(); ++boot_render_ph_draws;
    boot_render_txd_in = 0;   /* nvnpar-work: a draw that returned inside its texture phase left it set */
    boot_render_cost_report();   /* batch-b */
    /* The CBV descriptor ring and the upload ring (constant-buffer snapshots, widened or expanded
     * indices) belong to the recorded command list's segment. Without room for this draw's worst case
     * (38 CBVs, 2 x 18 snapshots of up to 64 KiB, HostState, 8 bytes of indices per vertex) the
     * recorded work is executed (without waiting for it) and the draw continues in the next segment;
     * a draw larger than a segment gets the whole rings once the queue is idle. */
    {   /* this draw's constant buffers: in place (native NVN, guest memory viewable) or a snapshot of its size; the
         * feedback-patched slot 2 always gets a snapshot */
        uint64_t cb = 0;
        boot_ds_cb_plan(r, in);   /* data-snapshot: slots read from a copy (their ring bytes count below) */
        for (unsigned g = 0; g < 2; ++g)
            for (unsigned slot = 0; slot < 18; ++slot) {
                const BootCbufBinding *b = &in->cbufs[g ? 4 : 0][slot];
                if (slot != 2 && b->valid == 2 && !boot_ds_cbt[g][slot] && boot_render_cbv_direct_ok(r, b->address, b->size)) continue;
                cb += boot_render_cb_ring_bytes(b->valid ? b->size : 0);
            }
        uint64_t need = cb + 3 * 256 + (uint64_t)s->count * 8 + 256 + sizeof(uint32_t) * 32 * 8 + (ind.kind ? 256 : 0);
        if (r->recording && (r->cbv_used + BOOT_RENDER_DRAW_DESCRIPTORS > boot_render_cbv_end || r->ring_used + need > r->ring_size)) {
            ++boot_render_ring_full;
            if (!boot_render_seg.whole && boot_render_seg.r == r && need <= boot_render_seg_bytes()) {
                if (!boot_render_flush(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "execute (ring segment full)");
            } else if (!boot_render_submit(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "submit (rings full)");
        }
        if (!r->recording && !boot_render_begin_in(r, boot_render_seg_bind(r) && need > boot_render_seg_bytes()))
            return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset"); }
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[0] += ph1 - ph; ph = ph1; }
    /* shaders */
    uint8_t vs_id[32], ps_id[32] = {0};
    const uint64_t memo_vs = s->program_base + s->stage_offset[1], memo_ps = has_ps ? s->program_base + s->stage_offset[5] : 0;
    int memo_hit = 0;
    {   __typeof__(boot_render_prog_memo) *m = boot_render_prog_memo_at(memo_vs, memo_ps);
        if (m->r == r && m->vs_va == memo_vs && m->ps_va == memo_ps && m->has_ps == has_ps && m->exec == boot_render_exec_id &&
            m->map_gen == boot_render_map_gen && m->big == boot_render_big_writes && m->shaders == boot_render_shader_total(r) &&
            boot_render_cb_clean(memo_vs, m->vs_size, m->epoch) && (!has_ps || boot_render_cb_clean(memo_ps, m->ps_size, m->epoch))) {
            memo_hit = 1; ++boot_render_prog_memo_hits; goto programs_known; } }
    if (!boot_render_program_id(r, s->program_base + s->stage_offset[1], vs_id) ||
        (has_ps && !boot_render_program_id(r, s->program_base + s->stage_offset[5], ps_id))) return boot_render_fail(r, BOOT_RENDER_E_SHADER, "program terminator not found");
    if (has_ps) boot_render_program_id(r, s->program_base + s->stage_offset[5], ps_id);   /* keeps PS bytes last */
    if (BOOT_CENSUS_ON()) BOOT_CENSUS_PROGRAMS(vs_id, has_ps ? ps_id : NULL);
    uint32_t ps_outputs = has_ps ? boot_render_ps_output_map(boot_render_last_program, boot_render_last_program_size) : 0;
    BootRenderShader *vs = boot_render_shader(r, vs_id, 0, 0);
    if (!vs) { boot_render_program_id(r, s->program_base + s->stage_offset[1], vs_id); boot_render_dump_last_program(vs_id, "vs");
               snprintf(r->error, sizeof r->error, "vertex program not in manifest: vs %02x%02x%02x%02x%02x%02x%02x%02x at %llx",
                        vs_id[0], vs_id[1], vs_id[2], vs_id[3], vs_id[4], vs_id[5], vs_id[6], vs_id[7], (unsigned long long)(s->program_base + s->stage_offset[1]));
               return BOOT_RENDER_E_SHADER; }
    uint8_t *vsph = boot_render_guest(r, s->program_base + s->stage_offset[1], 0x50);
    uint32_t vs_mask = 0;   /* VS output map selects the PS variant with the matching input signature */
    for (unsigned g = 0; g < 32; ++g) if ((vsph[0x36 + g / 2] >> ((g & 1) * 4)) & 15) vs_mask |= 1u << g;
    BootRenderShader *ps = has_ps ? boot_render_shader(r, ps_id, 1, vs_mask) : NULL;
    if (has_ps && !ps) { boot_render_program_id(r, s->program_base + s->stage_offset[5], ps_id); char tag[24]; snprintf(tag, sizeof tag, "ps-%x", vs_mask); boot_render_dump_last_program(ps_id, tag);
                         snprintf(r->error, sizeof r->error, "pixel program/variant not in manifest: %s-%02x%02x%02x%02x%02x%02x%02x%02x at %llx", tag,
                                  ps_id[0], ps_id[1], ps_id[2], ps_id[3], ps_id[4], ps_id[5], ps_id[6], ps_id[7], (unsigned long long)(s->program_base + s->stage_offset[5]));
                         return BOOT_RENDER_E_SHADER; }
    int stores = boot_render_shader_stores(r, vs) || boot_render_shader_stores(r, ps);
programs_known:
    {   __typeof__(boot_render_prog_memo) *m = boot_render_prog_memo_at(memo_vs, memo_ps);
        if (memo_hit) { memcpy(vs_id, m->vs_id, 32); memcpy(ps_id, m->ps_id, 32); vs = m->vs; ps = m->ps; vs_mask = m->vs_mask; ps_outputs = m->ps_outputs; stores = m->stores;
                         if (BOOT_CENSUS_ON()) BOOT_CENSUS_PROGRAMS(vs_id, has_ps ? ps_id : NULL); }
        else {
            m->vs_size = boot_render_program_cached_size(memo_vs); m->ps_size = has_ps ? boot_render_program_cached_size(memo_ps) : 0;
            if (m->vs_size && (!has_ps || m->ps_size)) {
                m->r = r; m->vs_va = memo_vs; m->ps_va = memo_ps; m->has_ps = has_ps; m->exec = boot_render_exec_id; m->map_gen = boot_render_map_gen;
                m->big = boot_render_big_writes; m->epoch = ++boot_render_cb_epoch; m->shaders = boot_render_shader_total(r);
                memcpy(m->vs_id, vs_id, 32); memcpy(m->ps_id, ps_id, 32); m->vs = vs; m->ps = ps; m->vs_mask = vs_mask; m->ps_outputs = ps_outputs; m->stores = stores;
            } else m->r = NULL;
        } }
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[1] += ph1 - ph; ph = ph1; }
    if (boot_sd_on()) boot_sd_draw_begin(s, vs_id, has_ps ? ps_id : NULL);   /* suspect-diag: the draw in flight */
    /* targets */
    BootRenderTarget *rts[8] = {0}, *zt = NULL; D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8], dsv = {0};
    for (unsigned i = 0; i < s->rt_count; ++i) {
        if (s->rt[s->rt_map[i]].format == 0) {
            /* SET_COLOR_TARGET_FORMAT_V_DISABLED (0): nothing bound at this slot (Ryujinx StateUpdater.IsRtEnabled);
             * a null view discards the shader's writes to it */
            rts[i] = NULL; rtvs[i] = boot_render_null_rtv(r);
            if (!rtvs[i].ptr) return boot_render_fail(r, BOOT_RENDER_E_D3D, "null render target view");
            continue;
        }
        rts[i] = boot_render_target(r, &s->rt[s->rt_map[i]]);
        if (!rts[i]) {
            const BootColorTarget *c = &s->rt[s->rt_map[i]];
            snprintf(r->error, sizeof r->error, "render target format/layout: rt %u va %llx format %02x %ux%u pitch %u layers %u layer %u array pitch %u gob %u/%u/%u (%u host targets)",
                     s->rt_map[i], (unsigned long long)c->address, c->format, c->width, c->height, c->pitch_layout, c->layers, c->layer, c->array_pitch,
                     c->log_gob_w, c->log_gob_h, c->log_gob_d, boot_render_rtab(r)->count);
            return BOOT_RENDER_E_FORMAT;
        }
        boot_render_rt_barrier(r, rts[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[i] = boot_render_rtv(r, rts[i]);
    }
    if (s->zt.enabled) {
        zt = boot_render_depth(r, &s->zt);
        if (!zt) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "depth target format");
        boot_render_rt_barrier(r, zt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsv = boot_render_dsv(r, zt);
    }
    if (boot_render_fb_pb_on()) boot_render_fb_pb_draw(r, rts, s->rt_count, zt, in->guest_barrier);   /* batch-c: feedback copy sync points */
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[2] += ph1 - ph; ph = ph1; }
    boot_render_dfb_begin(zt && s->depth_test && s->depth_write ? zt->texture : NULL);   /* batch-b: depth feedback on the GPU */
    const int txd = boot_render_txd_on(); const uint64_t txd_wait0 = boot_render_seg_wait_qpc;   /* nvnpar-work: texture phase detail */
    boot_render_txd_in = txd;
    /* textures (before anything is placed in the per-list rings: creating one may execute the list) */
    uint8_t fb_done[8] = {0}, sampled[8] = {0}; int fb_any = 0, zt_sampled = 0;
    const uint64_t sub_built0 = boot_render_tex_sub_built;   /* wb-02: target-subresource assemblies built by this loop */
    /*99: classify and consume the same per-Draw handle snapshot. Native driver
     * CB2 is stable; generic callers deliberately snapshot here, before helpers. */
    /*99 SNAPSHOT BEGIN: exact bounded CPU fixture extraction. */
    BootRenderCbPatch fb_patch[128]; unsigned fb_patches = 0, fb_redirects = 0;
    uint32_t fb_handles[2][64]; uint8_t fb_handle_valid[2][64], fb_patch_at[2][64], fb_redirected[128];   /* heavy-work: zeroed with rep stosb */
    boot_render_zero(fb_handles, sizeof fb_handles); boot_render_zero(fb_handle_valid, sizeof fb_handle_valid);
    boot_render_zero(fb_patch_at, sizeof fb_patch_at); boot_render_zero(fb_redirected, sizeof fb_redirected);
    int fb_game_range = 0;
    for (unsigned stage = 0; stage < 2; ++stage) {
        const BootRenderShader *sh = stage ? ps : vs; const BootCbufBinding *c2 = &in->cbufs[stage ? 4 : 0][2];
        if (sh && sh->tex_count > 64) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "feedback handle snapshot");
        uint64_t effective = c2->valid ? (c2->size < 65536 ? c2->size : 65536) : 0;
        if (sh && sh->tex_count && effective && c2->address > UINT64_MAX - effective)
            return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "feedback binding address overflow");
        for (unsigned i = 0; sh && i < sh->tex_count; ++i) {
            uint64_t offset = (uint64_t)sh->tex[i] * 4;
            if (!c2->valid || offset + 4 > c2->size || offset + 4 > 65536) continue;
            unsigned prior = 0; while (prior < i && sh->tex[prior] != sh->tex[i]) ++prior;
            if (prior < i) { fb_handles[stage][i] = fb_handles[stage][prior]; fb_handle_valid[stage][i] = fb_handle_valid[stage][prior]; fb_patch_at[stage][i] = fb_patch_at[stage][prior]; continue; }
            if (offset > UINT64_MAX - c2->address || c2->address + offset > UINT64_MAX - 3)
                return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "feedback handle address overflow");
            const uint8_t *word = boot_render_guest(r, c2->address + offset, 4);
            if (!word) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "feedback handle snapshot");
            uint32_t handle; memcpy(&handle, word, 4);
            /* Match prior Write32 overlay order/range used by cbv_ex before its
             * final patches. Unaligned writes may replace part of this word. */
            uint64_t word_va = c2->address + offset;
            for (uint32_t k = 0; k < r->overlay_count; ++k) {
                const BootRenderWrite *w = &r->overlay[k];
                if (w->kind != 0 || w->gpu < c2->address || w->gpu > UINT64_MAX - 4 || w->gpu + 4 > c2->address + effective) continue;
                uint64_t lo = w->gpu > word_va ? w->gpu : word_va;
                uint64_t hi = w->gpu + 4 < word_va + 4 ? w->gpu + 4 : word_va + 4;
                if (lo < hi) memcpy((uint8_t *)&handle + (size_t)(lo-word_va), (const uint8_t *)&w->value + (size_t)(lo-w->gpu), (size_t)(hi-lo));
            }
            if (fb_patches >= 128) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "feedback handle snapshot");
            fb_handles[stage][i] = handle; fb_handle_valid[stage][i] = 1; fb_patch_at[stage][i] = (uint8_t)fb_patches;
            fb_patch[fb_patches++] = (BootRenderCbPatch){stage ? 4u : 0u, (uint32_t)offset, handle};
            if ((handle & 0xfffffu) >= BOOT_RENDER_FB_SLOT_BASE && (handle & 0xfffffu) < BOOT_RENDER_TEX_SLOTS) fb_game_range = 1;
        }
    }
    if (fb_game_range) {
        /* Before this Draw owns private slots. Each ordinary writer still checks
         * an outer/nested Draw's exact provisional descriptor below. */
        for (unsigned stage = 0; stage < 2; ++stage) {
            const BootRenderShader *sh = stage ? ps : vs;
            for (unsigned i = 0; sh && i < sh->tex_count; ++i) if (fb_handle_valid[stage][i] &&
                (fb_handles[stage][i] & 0xfffffu) >= BOOT_RENDER_FB_SLOT_BASE &&
                !boot_render_fb_game_tic(r, fb_handles[stage][i] & 0xfffffu))
                return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending feedback/game TIC collision");
        }
    }
    /*99 SNAPSHOT END. */
    if (txd) { uint64_t q = boot_render_ph_now(); boot_render_txd_q[BOOT_TXD_PRE] += q - ph; }
    for (unsigned stage = 0; stage < 2; ++stage) {
        BootRenderShader *sh = stage ? ps : vs;
        for (unsigned i = 0; sh && i < sh->tex_count; ++i) {
            /* Hardware reads a handle only when the texture instruction executes; a shader may
             * reference handles on paths this draw never takes. Without a bound handle buffer the
             * slot keeps its null descriptor. A bound handle must resolve (format/layout errors stop). */
            if (!fb_handle_valid[stage][i]) continue;
            uint32_t handle = fb_handles[stage][i];
            uint32_t tic = handle & 0xfffff, tsc = s->sampler_via_header ? tic : handle >> 20;
            boot_bc7_bind(r, s->tic_address, stage, tic);   /* bc7-work: vertex-stage textures stay RGBA8 (TOTK_BC7) */
            if (tic < BOOT_RENDER_FB_SLOT_BASE && boot_render_texmemo_ok(s->tic_address, tic)) {
                /* nvnpar-work: a remembered binding (host_gpu_render.h boot_render_texmemo): boot_render_texture's unchanged
                 * return, and the index has no feedback view and no target alias, so the feedback test above and the
                 * feedback / depth-alias tests below would find nothing */
                boot_render_tex_touch(tic); ++boot_render_host_hits; ++boot_render_texmemo_hits;
                boot_render_tex_pend(tic); ++boot_render_txd_memo;
                int rc = boot_render_sampler(r, s->tsc_address, tsc); if (rc) return rc;
                if (boot_render_trace.active) boot_render_draw_trace_tex(r, s->tic_address, stage, sh->tex[i], handle);
                continue;
            }
            uint64_t txq = txd ? boot_render_ph_now() : 0;
            if (tic < BOOT_RENDER_TEX_SLOTS && boot_render_fb_on[tic] && boot_render_fb_index) {
                /* still a feedback view of a target this draw renders to: refresh the copy only */
                const uint32_t *w = boot_render_desc_words(r, s->tic_address, tic, 0);
                BootRenderFbIndex *x = &boot_render_fb_index[tic]; int k = -1;
                for (unsigned j = 0; j < s->rt_count && k < 0; ++j) if (rts[j] && rts[j]->texture == x->rt) k = (int)j;
                if (w && k >= 0 && !memcmp(w, x->words, 32) && x->copy && x->copy->copy && boot_render_fb_pin_copy(r, x->copy)) {
                    if (!fb_done[k] && !boot_render_fb_refresh(r, rts[k], x->copy)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "feedback copy");
                    fb_done[k] = 1; fb_any = 1; boot_render_tex_pend(tic);
                    int rc = boot_render_sampler(r, s->tsc_address, tsc); if (rc) return rc;
                    if (boot_render_trace.active) boot_render_draw_trace_tex(r, s->tic_address, stage, sh->tex[i], handle);
                    if (txd) { boot_render_txd_q[BOOT_TXD_FBTOP] += boot_render_ph_now() - txq; ++boot_render_txd_n[BOOT_TXD_FBTOP]; }
                    continue;
                }
            }
            const uint64_t txc_alias = boot_render_alias_hits, txc_view = boot_render_alias_views, txc_built = boot_tex_built, txc_shared = boot_render_tex_shared;
            int rc = boot_render_texture(r, s->tic_address, tic); if (rc) return rc;
            if (txd) {
                unsigned c = boot_render_texmemo_pure ? BOOT_TXD_SAME : boot_render_alias_hits != txc_alias ? BOOT_TXD_ALIAS : boot_render_alias_views != txc_view ? BOOT_TXD_VIEW :
                             boot_render_tex_shared != txc_shared ? BOOT_TXD_SHARED : boot_tex_built != txc_built ? BOOT_TXD_BUILD : BOOT_TXD_OTHER;
                uint64_t q = boot_render_ph_now(); boot_render_txd_q[c] += q - txq; ++boot_render_txd_n[c]; txq = q;
            }
            boot_render_tex_pend(tic);
            {   int k = boot_render_fb_bound(tic, rts, s->rt_count);
                if (k >= 0 && s->rt[s->rt_map[k]].write_mask && boot_render_fb_enabled()) {
                    /* samples a target this draw renders to (render-to-texture view of rts[k]) */
                    const uint32_t *w = boot_render_desc_words(r, s->tic_address, tic, 0);
                    if (!w) return boot_render_fail(r, BOOT_RENDER_E_D3D, "feedback copy");
                    int slot = -1; BootRenderFbCopy *fc = NULL;
                    if (!s->sampler_via_header && (fb_redirects < 16 || fb_redirected[fb_patch_at[stage][i]]) && !boot_render_fb_slots_off) {
                        fc = boot_render_fb_copy_for(r,rts[k]->texture);
                        if (!fc) return boot_render_fail(r,BOOT_RENDER_E_D3D,"feedback copy ownership");
                        if (!fb_done[k] && !boot_render_fb_refresh(r, rts[k], fc)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "feedback copy");
                        fb_done[k] = 1;
                        slot = boot_render_fb_slot(r, fc, rts[k], w);
                        if (slot < -1) return boot_render_fail(r,BOOT_RENDER_E_D3D,"feedback descriptor retirement");
                    }
                    if (slot >= 0) {   /* this draw reads the slot through its own c2 snapshot */
                        BootRenderCbPatch *patch = &fb_patch[fb_patch_at[stage][i]];
                        if (!fb_redirected[fb_patch_at[stage][i]]) { fb_redirected[fb_patch_at[stage][i]] = 1; ++fb_redirects; }
                        patch->value = (handle & ~0xfffffu) | (uint32_t)slot;
                    } else {
                        if (!boot_render_fb_bind(r, tic, w, rts[k])) return boot_render_fail(r, BOOT_RENDER_E_D3D, "feedback copy");
                        fb_patch[fb_patch_at[stage][i]].value = handle;
                        if (fb_redirected[fb_patch_at[stage][i]]) { fb_redirected[fb_patch_at[stage][i]] = 0; --fb_redirects; }
                    }
                    fb_done[k] = 1; fb_any = 1;
                } else if (k >= 0) sampled[k] = 1;   /* read, not written: stays in the state its view left */
                if (zt && boot_render_alias && boot_render_alias[tic].on && boot_render_alias[tic].texture == zt->texture) {
                    if (s->depth_test && s->depth_write) {
                        /* samples the depth target it writes: D3D12 cannot read a depth resource while it is being
                         * written, so this texture is built from guest memory (written back first), as before */
                        ++boot_render_depth_alias_written;
                        boot_render_alias_forget(tic); boot_render_no_depth_alias = 1;
                        rc = boot_render_texture(r, s->tic_address, tic); boot_render_tex_pend(tic);
                        boot_render_no_depth_alias = 0; if (rc) return rc;
                    } else zt_sampled = 1;
                }
            }
            if (txd) { uint64_t q = boot_render_ph_now(); boot_render_txd_q[BOOT_TXD_FB] += q - txq; txq = q; }
            rc = boot_render_sampler(r, s->tsc_address, tsc); if (rc) return rc;
            if (txd) { boot_render_txd_q[BOOT_TXD_SMP] += boot_render_ph_now() - txq; ++boot_render_txd_n[BOOT_TXD_SMP]; }
            if (boot_render_trace.active) boot_render_draw_trace_tex(r, s->tic_address, stage, sh->tex[i], handle);
        }
    }
    boot_bc7_bind_stage = -1;   /* bc7-work: the texture loop is over */
    const uint64_t txd_after = txd ? boot_render_ph_now() : 0;
    /* textures copied above from a target this draw renders to hold its pre-draw image (what this draw reads,
     * as a feedback read): the draw's writes make them stale for later uses (boot_render_texture_from_target) */
    if (boot_render_rt_copy_count) for (unsigned i = 0; i < s->rt_count; ++i) if (rts[i]) boot_render_rt_copies_written(rts[i]);
    /* wb-02: a texture assembled above from target subresources (boot_render_texture_subres) recorded its copies as of
     * now (boot_render_asm_record: copied = the write sequence), after the bound targets were touched, so this draw's
     * writes into a bound target that is one of its sources would not make boot_render_asm_stale fire (the target
     * stays dirty, so the retouch below skips it, and these slots are not in the rt-copies list): the slot would keep
     * the pre-draw image until the target is drawn again (a cubemap sampled while its last face / mip is rendered).
     * The draw writes after those copies: the bound targets are stamped again, later in draw order. */
    if (boot_render_tex_sub_built != sub_built0) {
        BootRenderRtTable *T = boot_render_rtab(r);
        for (unsigned i = 0; i <= s->rt_count; ++i) {
            BootRenderTarget *x = i < s->rt_count ? rts[i] : zt;
            if (!x || x < T->rts || x >= T->rts + T->count || boot_render_rt_untiled(x)) continue;
            boot_render_rt_written_at[x - T->rts] = ++boot_render_rt_write_seq;
        }
    }
    /* Bound targets were marked dirty when bound; a write-back in the texture loop above (a texture over one of
     * them, built from guest memory) cleans it again, and this draw's writes would then go untracked: later
     * readers of that memory would get the image from before the draw. Mark them dirty again. */
    {   BootRenderRtTable *T = boot_render_rtab(r);
        for (unsigned i = 0; i <= s->rt_count; ++i) {
            BootRenderTarget *x = i < s->rt_count ? rts[i] : zt;
            if (!x || x < T->rts || x >= T->rts + T->count) continue;
            unsigned k = (unsigned)(x - T->rts); BootRenderRtInfo *xi = &T->info[k];
            if (xi->dirty || !xi->known || boot_render_rt_untiled(x)) continue;
            boot_render_rt_touch(T, k, xi->log_gob_h, xi->log_gob_d, 1); ++boot_render_rt_retouched;
        }
    }
    boot_render_dfb_end(zt);   /* batch-b: this draw's depth writes make its depth-feedback copies stale for later uses */
    /* a render-target write-back above may have executed the list: continue in a new one */
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    /* a texture view of a bound target (render-to-texture) moved it to the shader-resource state:
     * every bound target renders from the render-target / depth-write state */
    for (unsigned i = 0; i < s->rt_count; ++i)
        if (rts[i] && !sampled[i] && !boot_render_rt_barrier(r, rts[i], D3D12_RESOURCE_STATE_RENDER_TARGET)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target state");
    /* depth sampled while bound (depth writes off): the read-only depth state, shader readable */
    if (zt && !boot_render_rt_barrier(r, zt, zt_sampled ? (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) : D3D12_RESOURCE_STATE_DEPTH_WRITE))
        return boot_render_fail(r, BOOT_RENDER_E_D3D, "depth target state");
    if (fb_any) ++boot_render_fb_draws;
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[3] += ph1 - ph; ph = ph1;
        if (txd) { boot_render_txd_q[BOOT_TXD_AFTER] += ph1 - txd_after; boot_render_txd_q[BOOT_TXD_WAIT] += boot_render_seg_wait_qpc - txd_wait0; }
        boot_render_txd_in = 0; }
    /* vertex pulling table (maxwell_runtime.hlsli VertexAttrib): snapshotted per draw into the ring
     * with its own t1/t2 descriptor pair (below), so a change needs no flush */
    uint32_t attribs[33 * 8];   /* G2 (batch-4): entry 32 = point-sprite parameters (boot_pt_params), zero otherwise */
    boot_render_zero(attribs, sizeof attribs);   /* batch-b: rep stosb (memset was a byte loop) */
    const int vbounds = boot_render_vertex_bounds();
    for (unsigned i = 0; i < 32; ++i) {
        const BootVertexAttrib *a = &s->attrib[i]; const BootVertexStream *st = &s->stream[a->stream & 15];
        uint32_t *e = attribs + i * 8;
        e[0] = (uint32_t)st->address; e[1] = (uint32_t)(st->address >> 32); e[2] = st->stride; e[3] = a->offset;
        e[4] = a->size | (a->type << 6) | (a->bgra << 9) | ((!a->inactive && st->enabled) ? 0x80000000u : 0);
        e[5] = boot_render_attrib_divisor(st);
        e[6] = vbounds ? boot_render_stream_size(st) : 0;   /* vbounds: bound bytes (0 = no bound); e[7] stays 0 */
    }
    boot_ds_vertex(r, s, attribs);   /* data-snapshot: streams it takes read their copy in the arena (stream VA rewritten) */
    if (boot_render_state_diag_on() && !ind.kind && s->count) boot_render_vbounds_diag(r, s, vs_id);   /* vbounds diagnostic (log only) */
    if (boot_render_state_diag_on() && s->count) boot_render_fmt100_diag(r, s, vs_id);   /* fmt100 diagnostic (log only) */
    if (boot_sd_on()) boot_sd_draw_vs(r, s, vs, ind.kind != 0, draw_instances, fb_handles[0], fb_handle_valid[0]);   /* suspect-diag S1 / S2 / S4 */
    /* topology and index source */
    static const D3D_PRIMITIVE_TOPOLOGY direct[6] = {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
        D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP};
    D3D_PRIMITIVE_TOPOLOGY topo = s->topology < 6 ? direct[s->topology] : D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    int needs_expand = topo == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (needs_expand && s->topology > 9) return boot_render_fail(r, BOOT_RENDER_E_STATE, "adjacency/patch topology");
    uint32_t idx_bytes = s->index_size == 0 ? 1 : s->index_size == 1 ? 2 : 4;
    if (ind.kind && (needs_expand || (s->indexed && idx_bytes == 1)))   /* render-38d: the executor keeps these on the CPU path */
        return boot_render_fail(r, BOOT_RENDER_E_STATE, "indirect draw needing CPU index work");
    D3D12_INDEX_BUFFER_VIEW ib = {0}; uint32_t draw_count = s->count; int indexed = s->indexed;
    if (indexed && (needs_expand || idx_bytes == 1)) {
        if (!boot_render_cpu_read(r, s->index_address + (uint64_t)s->first * idx_bytes, (uint64_t)s->count * idx_bytes)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "pending GPU writes");
        const uint8_t *src = boot_render_guest(r, s->index_address + (uint64_t)s->first * idx_bytes, (uint64_t)s->count * idx_bytes);
        if (!src) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "index buffer");
        if (boot_sd_on()) boot_sd_note(r, BOOT_SD_K_INDEX, s->index_address + (uint64_t)s->first * idx_bytes, src, NULL, (uint64_t)s->count * idx_bytes);   /* suspect-diag S1 */
        if (needs_expand) draw_count = boot_render_expand(r, s, src, idx_bytes, &ib, &topo);
        else {   /* 8-bit indices: D3D12 has none, widen into the ring */
            uint64_t bytes = ((uint64_t)s->count * 2 + 255) & ~(uint64_t)255;
            if (r->ring_used + bytes > r->ring_size) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring");
            uint16_t *o = (uint16_t *)(r->ring_cpu + r->ring_used);
            for (uint32_t i = 0; i < s->count; ++i) o[i] = (src[i] == 0xff && s->primitive_restart) ? 0xffff : src[i];
            ib.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used; ib.SizeInBytes = s->count * 2; ib.Format = DXGI_FORMAT_R16_UINT;
            r->ring_used += bytes;
        }
        if (!draw_count) return boot_render_fail(r, BOOT_RENDER_E_STATE, "index expansion");
    } else if (indexed && ind.kind) {   /* render-38d: StartIndexLocation (the arguments' first index) selects it on the GPU */
        if (!boot_render_ind_ib(r, s->index_address, idx_bytes, &ib)) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "indirect index buffer outside guest memory");
    } else if (indexed) {
        if (!boot_render_resolve(r, s->index_address + (uint64_t)s->first * idx_bytes, (uint64_t)s->count * idx_bytes, &ib.BufferLocation))
            return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "index buffer outside one guest region");
        ib.SizeInBytes = s->count * idx_bytes; ib.Format = idx_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        boot_ds_index(r, s->index_address + (uint64_t)s->first * idx_bytes, (uint64_t)s->count * idx_bytes, &ib);   /* data-snapshot (and S1 index-zc) */
    } else if (needs_expand) {
        draw_count = boot_render_expand(r, s, NULL, 0, &ib, &topo); indexed = 1;
        if (!draw_count) return boot_render_fail(r, BOOT_RENDER_E_STATE, "vertex expansion");
    }
    if (s->primitive_restart && s->indexed && !needs_expand && idx_bytes != 1) {
        uint32_t cut = idx_bytes == 4 ? 0xffffffffu : 0xffffu;
        if (s->restart_index != cut) return boot_render_fail(r, BOOT_RENDER_E_STATE, "primitive restart index other than all-ones");
    }
    if (s->primitive_restart && s->indexed && (s->topology == 0 || s->topology == 1 || s->topology == 4) && boot_render_state_diag_on())
        boot_render_restart_diag(r, s, idx_bytes, ind.kind != 0);   /* batch-3 K1/G5 diagnostic (log only) */
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[4] += ph1 - ph; ph = ph1; }
    /* pipeline state */
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    boot_render_zero(&d, sizeof d);   /* heavy-work: rep stosb */
    d.pRootSignature = r->root;
    d.VS.pShaderBytecode = vs->dxil; d.VS.BytecodeLength = vs->size;
    if (ps) { d.PS.pShaderBytecode = ps->dxil; d.PS.BytecodeLength = ps->size; }
    if (ps && boot_render_trace.const_ps_on && boot_render_trace.const_ps) {
        d.PS.pShaderBytecode = boot_render_trace.const_ps; d.PS.BytecodeLength = boot_render_trace.const_ps_size; memset(ps_id, 0xcd, 32);
        ps_outputs = 15u;   /* diagnostic magenta PS exports only SV_Target0 */
    }
    /* pso-work: blend, rasterizer, depth/stencil, topology type, strip cut and target formats (boot_render_pso_fixed99) */
    int replicate_alpha = 0;
    {   uint8_t rt_on[8] = {0}; DXGI_FORMAT rt_fmt[8] = {DXGI_FORMAT_UNKNOWN};
        for (unsigned i = 0; i < s->rt_count && i < 8; ++i) if (rts[i]) { rt_on[i] = 1; rt_fmt[i] = rts[i]->dxgi; }
        int rc = boot_render_pso_fixed99(r, s, &d, ps_outputs, rt_on, rt_fmt, zt != NULL, zt ? zt->dxgi : DXGI_FORMAT_UNKNOWN, topo, needs_expand, indexed, ib.Format, &replicate_alpha);
        if (rc) return rc; }
    const int point_gs = boot_pt_apply(r->manifest_path, &d);   /* G2 (batch-4): point sprites (boot_pt_apply above) */
    int flip = boot_render_viewport_flip(s->viewport[0].scale[1]);
    /* key = shaders + every fixed-function field set above, compared in full (module-static copies
     * beside r->psos, whose 32-byte key holds a hash of it) */
    uint8_t key[32]; memset(key, 0, sizeof key);
    uint8_t buf[BOOT_RENDER_PSO_KEY]; size_t k = 0;
    memcpy(buf + k, vs_id, 32); k += 32; memcpy(buf + k, ps_id, 32); k += 32;
    memcpy(buf + k, &d.BlendState, sizeof d.BlendState); k += sizeof d.BlendState;
    memcpy(buf + k, &d.RasterizerState, sizeof d.RasterizerState); k += sizeof d.RasterizerState;
    memcpy(buf + k, &d.DepthStencilState, sizeof d.DepthStencilState); k += sizeof d.DepthStencilState;
    memcpy(buf + k, &d.PrimitiveTopologyType, 4); k += 4; memcpy(buf + k, &d.IBStripCutValue, 4); k += 4;
    memcpy(buf + k, &d.DSVFormat, 4); k += 4; memcpy(buf + k, d.RTVFormats, 32); k += 32;
    if (point_gs) { uint64_t g = boot_pt_gs_key(d.GS.pShaderBytecode); memcpy(buf + k, &g, 8); k += 8; }   /* G2: as boot_psoj_key */
    if (boot_render_pso_owner != r) {   /* keys belong to another render object: none can match */
        boot_render_pso_owner = r; memset(boot_render_pso_key_size, 0, sizeof boot_render_pso_key_size);
        memset(boot_render_pso_index, 0, sizeof boot_render_pso_index);
        for (unsigned i = 0; i < r->pso_count; ++i) { uint64_t p0; memcpy(&p0, r->psos[i].key, 8); (void)p0; } }
    ID3D12PipelineState *pso = NULL;
    /* play (38): the previous draw's pipeline when its full key is the same (no hash, no probe) */
    static unsigned pso_memo = UINT_MAX;
    if (pso_memo < r->pso_count && boot_render_pso_owner == r && boot_render_pso_key_size[pso_memo] == k && boot_render_memeq(boot_render_pso_keys[pso_memo], buf, k)) {   /* heavy-work: 8-byte compare */
        pso = r->psos[pso_memo].pso; ++boot_render_pso_memo_hits; goto pso_found; }
    {   uint64_t h = UINT64_C(1469598103934665603);   /* FNV-1a */
        for (size_t i = 0; i < k; ++i) { h ^= buf[i]; h *= UINT64_C(1099511628211); }
        memcpy(key, &h, 8); }
    {   uint64_t k0; memcpy(&k0, key, 8);
        for (unsigned h = (unsigned)(k0 % BOOT_RENDER_PSO_INDEX), n = 0; n < BOOT_RENDER_PSO_INDEX && boot_render_pso_index[h]; h = (h + 1) % BOOT_RENDER_PSO_INDEX, ++n) {
            unsigned i = boot_render_pso_index[h] - 1; uint64_t p0; memcpy(&p0, r->psos[i].key, 8);
            if (i < r->pso_count && p0 == k0 && boot_render_pso_key_size[i] == k && boot_render_memeq(boot_render_pso_keys[i], buf, k)) { pso = r->psos[i].pso; pso_memo = i; break; }
        } }
    if (!pso) {
        {   uint64_t k0; memcpy(&k0, key, 8); HRESULT bad = boot_render_pso_bad_find(k0);   /* explode-work2: refused before, not retried */
            if (FAILED(bad)) { snprintf(r->error, sizeof r->error, "CreateGraphicsPipelineState hr=0x%08lx (refused before)", (unsigned long)bad); return BOOT_RENDER_E_D3D; } }
        if (r->pso_count == BOOT_RENDER_MAX_PSO && !boot_render_pso_recycle(r)) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "pso cache full");
        boot_render_pso_pre_start(r);   /* play (44): prebuilt by the background thread when an earlier boot used it */
        HRESULT hr = S_OK; pso = boot_render_trace.const_ps_on ? NULL : boot_render_pso_pre_take(buf, k);
        if (!pso && !boot_render_trace.const_ps_on && boot_psoj_enabled()) {   /* pso-work: this and the submission's next new pipelines compile in parallel */
            boot_psoj_lookahead(r, &d, buf, k);
            if (boot_psoj_wait(buf, k) && (pso = boot_render_pso_pre_take(buf, k)) != NULL) boot_render_pso_record(buf, k, vs_mask, ps != NULL, d.NumRenderTargets);
        }
        if (!pso) {
            uint64_t pq0 = boot_psoj_inline_begin();   /* pso-work: timed for the hitch line */
            hr = boot_psoc_create_graphics(r, r->gpu->device, &d, buf, k, &pso);   /* play (73): from the persistent library, else compiled and stored */
            boot_psoj_inline_end(pq0);
            if (SUCCEEDED(hr) && !boot_render_trace.const_ps_on) boot_render_pso_record(buf, k, vs_mask, ps != NULL, d.NumRenderTargets);
        }
        if (FAILED(hr)) {
            if (!boot_render_trace.const_ps_on) { uint64_t k0; memcpy(&k0, key, 8); boot_render_pso_bad_add(k0, hr, &d, vs_id, ps_id); }
            snprintf(r->error, sizeof r->error, "CreateGraphicsPipelineState hr=0x%08lx", (unsigned long)hr); return BOOT_RENDER_E_D3D; }
        memcpy(boot_render_pso_keys[r->pso_count], buf, k); boot_render_pso_key_size[r->pso_count] = (uint32_t)k;
        {   uint64_t k0; memcpy(&k0, key, 8); unsigned h = (unsigned)(k0 % BOOT_RENDER_PSO_INDEX);
            while (boot_render_pso_index[h]) h = (h + 1) % BOOT_RENDER_PSO_INDEX;
            boot_render_pso_index[h] = r->pso_count + 1; }
        memcpy(r->psos[r->pso_count].key, key, 32); pso_memo = r->pso_count; r->psos[r->pso_count++].pso = pso;
    }
    pso_found:;
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[5] += ph1 - ph; ph = ph1; }
    /* constant buffers: VS (bind group 1) and PS (bind group 4), plus HostState at b18 */
    if (r->cbv_used + BOOT_RENDER_DRAW_DESCRIPTORS > boot_render_cbv_end) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "cbv ring");
    uint32_t vs_base = BOOT_RENDER_HEAP_RING + r->cbv_used, ps_base = vs_base + 19, mem_base = vs_base + 38;
    /*88/89: resource/feedback/capacity preparation is complete. Begin only the
     * resolver scope here; a separate issued callback follows the real Draw. */
    if(boot_cbv_tap_registered) {
        BootCbvTapBegin begin={0}; begin.renderer=(uintptr_t)r; begin.draw=(uintptr_t)in; begin.pso=(uintptr_t)pso;
        memcpy(begin.vs,vs_id,32); memcpy(begin.ps,ps_id,32); begin.sub=boot_render_exec_id; begin.event=boot_render_watch_event;
        begin.count=draw_count; begin.instances=draw_instances; begin.indexed=(unsigned)indexed; begin.topology=(unsigned)topo;
        begin.vs_mask=vs_mask; begin.rt_count=s->rt_count; begin.samples=d.SampleDesc.Count;
        for(unsigned j=0;j<s->rt_count && j<8;++j) {
            const BootColorTarget *raw=&s->rt[s->rt_map[j]]; const BootRenderTarget *rt=rts[j];
            begin.target[j].layer=raw->layer; begin.target[j].layers=raw->layers; begin.target[j].pitch=raw->array_pitch; begin.target[j].map=s->rt_map[j];
            if(rt) { begin.rt_address[j]=rt->address; begin.rt_resource[j]=(uintptr_t)rt->texture;
                begin.target[j].width=rt->width; begin.target[j].height=rt->height; begin.target[j].format=rt->dxgi; begin.target[j].state=rt->state; }
        }
        cbv_tap_token=boot_cbv_tap_begin(&begin);
    }
    uint32_t host[12]; memset(host, 0, sizeof host); memcpy(host, in->host_state, sizeof in->host_state);   /* K2: words 8-11 below */
    host[1] = s->z_clip_zero_to_w ? 0u : 1u;     /* [-W, W] clip space: vertex shader remaps z */
    host[2] = r->ptable_pages; host[3] = r->va_base_page;
    host[4] = s->indexed ? s->base_vertex_reg : s->first;
    host[5] = s->base_instance_reg + in->host_state[5];   /* SET_GLOBAL_BASE_INSTANCE_INDEX + instance index */
    host[6] = flip ? 1u : 0u;
    {   static int first = -1;   /* render-36: flat inputs from the last vertex (Maxwell/NVN default); A/B TOTK_PROVOKING_FIRST=1 */
        if (first < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_PROVOKING_FIRST", v, sizeof v); first = k && k < sizeof v && v[0] == '1'; }
        host[7] = (uint32_t)first; }
    host[8] = boot_render_iid_offset(in->host_state[5]);   /* K2 (batch-4): HostState host_instance_offset (boot_render_iid_offset above) */
    D3D12_GPU_VIRTUAL_ADDRESS ind_host = 0;   /* render-38d: b18 = the ring slot the pre-pass writes */
    if (ind.kind) {
        if (!boot_render_ind_stage(r, &ind, host, sizeof host, 256, 4)) { boot_cbv_tap_end(cbv_tap_token,1); return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring (indirect HostState)"); }
        ind_host = boot_render_ind_slot_gpu(&ind);
    }
    /* Only redirected handles are real patches. The 99 snapshot also records an identity entry for every
     * valid handle (for classification); passing those to cbv_ex made every textured draw bypass the CB
     * cache and the GPU-side direct path (play-69: texture/CB phase 5-9 -> 50-160 us per draw). The
     * identity values equal the guest bytes cbv_ex copies anyway, so dropping them changes no data. */
    BootRenderCbPatch fb_live[128]; unsigned fb_lives = 0;
    for (unsigned p = 0; p < fb_patches; ++p) if (fb_redirected[p]) fb_live[fb_lives++] = fb_patch[p];
    for (unsigned stage = 0; stage < 2; ++stage) {
        unsigned group = stage ? 4 : 0;   /* bind groups: 0 vertex (A+B), 1 tess control, 2 tess eval, 3 geometry, 4 pixel (clb197.h BIND_GROUP_*: pipeline program p >= 1 uses group p - 1) */ uint32_t base = stage ? ps_base : vs_base;
        boot_render_cbv_capturing = 1; boot_render_cbv_cap_base = base;   /* play (39): resolve first, write below */
        const int dd_mode = boot_cbv_tap_registered || r->overlay_count ? 0 : boot_render_cb_dedupe_mode();   /* nvnpar-work 1f */
        int dd_have = 0; uint64_t dd_va = 0, dd_epoch = 0, dd_scope = 0; uint32_t dd_size = 0; D3D12_CONSTANT_BUFFER_VIEW_DESC dd_cv; memset(&dd_cv, 0, sizeof dd_cv);
        for (unsigned slot = 0; slot < 18; ++slot) {
            const BootCbufBinding *b = &in->cbufs[group][slot];
            boot_cbv_tap_request(stage,group,slot,b->address,b->size,b->valid);
            unsigned patched = 0; if (slot == 2) for (unsigned p = 0; p < fb_lives; ++p) patched += fb_live[p].group == group;
            const int ds_take = !patched && b->valid == 2 ? boot_ds_cbt[stage][slot] : 0;   /* data-snapshot: the rule that copies it (0: the live range) */
            if (!patched && b->valid == 2 && !ds_take && boot_render_cbv_direct(r, b->address, b->size, base + slot)) continue;   /* GPU-side (native NVN) */
            const uint32_t dd_req = b->valid ? b->size : 0;
            const int dd_same = dd_mode && !patched && dd_have && dd_va == b->address && dd_size == dd_req && dd_epoch == boot_render_cb_epoch && dd_scope == boot_render_cb_scope;
            if (dd_same && dd_mode == 1) { boot_render_cbv_cap[slot] = dd_cv; ++boot_render_cb_hits; ++boot_render_cb_dedupe_hits; continue; }   /* the previous slot's snapshot */
            const uint64_t dd_hits0 = boot_render_cb_hits;
            if (!patched && dd_mode) { dd_have = 1; dd_va = b->address; dd_size = dd_req; }
            boot_ds_cb_now = ds_take;
            const int cbv_ok = boot_render_cbv_ex(r, b->address, b->valid ? b->size : 0, base + slot, fb_live, patched ? fb_lives : 0, group);
            boot_ds_cb_now = 0;
            if (!cbv_ok) {
                uint64_t page = (b->address >> 12) - r->va_base_page;
                int mapped = (b->address >> 12) >= r->va_base_page && page < r->ptable_pages && r->ptable_cpu[page] != 0xffffffffu;
                snprintf(r->error, sizeof r->error, "constant buffer stage %u slot %u va %llx size %u: page %s (table %llx..%llx), ring %llu/%llu",
                         stage, slot, (unsigned long long)b->address, b->size, mapped ? "mapped" : "unmapped",
                         (unsigned long long)r->va_base_page << 12, (unsigned long long)(r->va_base_page + r->ptable_pages) << 12,
                         (unsigned long long)r->ring_used, (unsigned long long)r->ring_size);
                boot_render_cbv_capturing = 0;
                boot_cbv_tap_end(cbv_tap_token,1);
            return BOOT_RENDER_E_MEMORY;
            }
            if (dd_same && (boot_render_cb_hits != dd_hits0 + 1 || memcmp(&boot_render_cbv_cap[slot], &dd_cv, sizeof dd_cv)) && ++boot_render_cb_dedupe_bad <= 16) {   /* TOTK_CB_DEDUPE=2 */
                FILE *lf = fopen("draw-path.log", "a");
                if (lf) { fprintf(lf, "cb dedupe verify: stage %u slot %u va %llx size %u resolved to another view than the slot before (submission %llu)\n", stage, slot,
                                  (unsigned long long)b->address, b->size, (unsigned long long)boot_render_exec_id); fclose(lf); }
            }
            if (!patched && dd_mode) { dd_cv = boot_render_cbv_cap[slot]; dd_epoch = boot_render_cb_epoch; dd_scope = boot_render_cb_scope; }
        }
        boot_render_cbv_capturing = 0;
        /* HostState: the previous copy in this ring scope when the bytes are the same (both stages share it) */
        static uint32_t last_host[12]; static uint64_t last_host_loc, last_host_scope, last_host_offset, last_host_ring_bytes;
        static ID3D12Resource *last_host_ring; static uint8_t *last_host_cpu;
        unsigned host_reused=!ind_host && last_host_scope==boot_render_cb_scope && !memcmp(last_host,host,sizeof host);
        if (ind_host) last_host_scope = UINT64_MAX;   /* render-38d: the GPU-patched copy is not shared with later draws */
        else if (!host_reused) {
            if (r->ring_used + 256 > r->ring_size) { boot_cbv_tap_end(cbv_tap_token,1); return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring"); }
            uint8_t *dst = r->ring_cpu + r->ring_used;
            memset(dst, 0, 256); memcpy(dst, host, sizeof host);
            last_host_offset=r->ring_used; last_host_ring=r->ring; last_host_cpu=r->ring_cpu; last_host_ring_bytes=r->ring_size;
            last_host_loc = ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used; r->ring_used += 256;
            memcpy(last_host, host, sizeof host); last_host_scope = boot_render_cb_scope;
        }
        memset(&boot_render_cbv_cap[18], 0, sizeof boot_render_cbv_cap[18]);
        boot_render_cbv_cap[18].BufferLocation = ind_host ? ind_host : last_host_loc; boot_render_cbv_cap[18].SizeInBytes = 256;
        boot_cbv_tap_request(stage,group,18,0,sizeof host,1);
        if(boot_cbv_tap_active()) {
            const uint8_t *owned=last_host_ring==r->ring && last_host_cpu==r->ring_cpu && boot_render_cb_tap_span(r,last_host_offset,256,0) ? r->ring_cpu+last_host_offset : NULL;
            boot_cbv_tap_resolved(last_host_loc,256,last_host_ring,last_host_offset,last_host_ring_bytes,
                host_reused?BOOT_CBV_TAP_HOST_CACHE:BOOT_CBV_TAP_HOST_FRESH,UINT32_MAX,boot_render_cb_scope,boot_render_list_id,
                boot_render_map_gen,0,owned,256);
        }
        /* the stage table: the previous draw's descriptors when every view is the same (same ring scope) */
        static D3D12_CONSTANT_BUFFER_VIEW_DESC last_tab[2][19]; static uint32_t last_base[2]; static uint64_t last_scope[2]; static BootRender *last_r[2];
        unsigned table_reused=0;
        if (last_scope[stage] == boot_render_cb_scope && last_r[stage] == r && boot_render_memeq(last_tab[stage], boot_render_cbv_cap, sizeof boot_render_cbv_cap)) {   /* batch-b */
            base = last_base[stage]; ++boot_render_tables_reused; table_reused=1;
        } else {
            if (!boot_mtr_cbv_table(r, boot_render_cbv_cap, 19, boot_render_cpu(r, r->srv_heap, r->srv_inc, base)))   /* heavy-work: on the recording worker */
            for (unsigned k = 0; k < 19; ++k)
                ID3D12Device_CreateConstantBufferView(r->gpu->device, &boot_render_cbv_cap[k], boot_render_cpu(r, r->srv_heap, r->srv_inc, base + k));
            memcpy(last_tab[stage], boot_render_cbv_cap, sizeof boot_render_cbv_cap); last_base[stage] = base; last_scope[stage] = boot_render_cb_scope; last_r[stage] = r;
            ++boot_render_tables_written;
        }
        if (stage) ps_base = base; else vs_base = base;
        if(boot_cbv_tap_active()) {
            BootCbvTapCv actual[19]; for(unsigned k=0;k<19;++k) actual[k]=(BootCbvTapCv){boot_render_cbv_cap[k].BufferLocation,boot_render_cbv_cap[k].SizeInBytes};
            boot_cbv_tap_table(stage,group,base,table_reused,boot_render_cb_scope,boot_render_list_id,boot_render_map_gen,actual);
        }
    }
    boot_cbv_tap_end(cbv_tap_token,0);
    /* t1 page table, t2 this draw's vertex attribute table (ring snapshot); play (39): the previous draw's pair
     * when the attribute bytes and the page table are the same in this ring scope */
    if (point_gs) boot_pt_params(attribs + 32 * 8, s, &d);   /* G2: read by the geometry shader */
    const size_t attr_bytes = point_gs ? sizeof attribs : 32 * 32;   /* G2: entry 32 goes to the ring only for point-sprite draws */
    static uint32_t last_attribs[33 * 8]; static uint64_t last_attr_scope; static uint32_t last_mem_base, last_pt_pages; static ID3D12Resource *last_pt; static BootRender *last_attr_r;
    int attr_reuse = last_attr_scope == boot_render_cb_scope && last_attr_r == r && last_pt == r->ptable && last_pt_pages == r->ptable_pages &&
                     boot_render_memeq(last_attribs, attribs, sizeof attribs);   /* batch-b */
    if (attr_reuse) { mem_base = last_mem_base; ++boot_render_tables_reused; }
    else {   if (r->ring_used + attr_bytes > r->ring_size) { boot_cbv_tap_issued(cbv_tap_token,(uintptr_t)in,0); return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring"); }
        memcpy(r->ring_cpu + r->ring_used, attribs, attr_bytes);
        D3D12_SHADER_RESOURCE_VIEW_DESC pt; memset(&pt, 0, sizeof pt);
        pt.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; pt.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pt.Format = DXGI_FORMAT_UNKNOWN; pt.Buffer.NumElements = r->ptable_pages; pt.Buffer.StructureByteStride = 4;
        if (!boot_mtr_srv(r, r->ptable, &pt, boot_render_cpu(r, r->srv_heap, r->srv_inc, mem_base)))   /* heavy-work: on the recording worker */
        ID3D12Device_CreateShaderResourceView(r->gpu->device, r->ptable, &pt, boot_render_cpu(r, r->srv_heap, r->srv_inc, mem_base));
        D3D12_SHADER_RESOURCE_VIEW_DESC at = pt; at.Buffer.FirstElement = r->ring_used / 32; at.Buffer.NumElements = (UINT)(attr_bytes / 32); at.Buffer.StructureByteStride = 32;
        if (!boot_mtr_srv(r, r->ring, &at, boot_render_cpu(r, r->srv_heap, r->srv_inc, mem_base + 1)))
        ID3D12Device_CreateShaderResourceView(r->gpu->device, r->ring, &at, boot_render_cpu(r, r->srv_heap, r->srv_inc, mem_base + 1));
        r->ring_used += (attr_bytes + 255) & ~(size_t)255;
        memcpy(last_attribs, attribs, sizeof attribs); last_attr_scope = boot_render_cb_scope; last_attr_r = r;
        last_mem_base = mem_base; last_pt = r->ptable; last_pt_pages = r->ptable_pages; ++boot_render_tables_written;
    }
    r->cbv_used += BOOT_RENDER_DRAW_DESCRIPTORS;
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[6] += ph1 - ph; ph = ph1; }
    if (ind.kind && !boot_render_ind_prepass(r, &ind)) {   /* render-38d */
        boot_cbv_tap_issued(cbv_tap_token,(uintptr_t)in,0); return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "indirect arguments outside one guest region"); }
    /* bind and draw */
    ID3D12GraphicsCommandList *cl = r->cl;
    int bound = boot_render_bound.epoch == boot_render_bind_epoch;   /* play (38): skip what the list already has */
    if (!bound) { memset(&boot_render_bound, 0xff, sizeof boot_render_bound); boot_render_bound.epoch = boot_render_bind_epoch;
                  ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, r->root); }
    if (boot_render_bound.pso != pso) { ID3D12GraphicsCommandList_SetPipelineState(cl, pso); boot_render_bound.pso = pso; } else ++boot_render_binds_skipped;
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, boot_render_gpu(r, r->srv_heap, r->srv_inc, vs_base));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, boot_render_gpu(r, r->srv_heap, r->srv_inc, ps_base));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 2, boot_render_gpu(r, r->srv_heap, r->srv_inc, mem_base));
    if (!bound) {   /* fixed tables: once per list */
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 3, boot_render_gpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_TEX0));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 4, boot_render_gpu(r, r->sampler_heap, r->sampler_inc, 0));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 5, boot_render_gpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_ARENA_SRV));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 6, boot_render_gpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_ARENA_UAV));
    }
    {   int has_dsv = zt != NULL; unsigned n = s->rt_count < 8 ? s->rt_count : 8;
        if (boot_render_bound.nrt != s->rt_count || boot_render_bound.has_dsv != has_dsv || (n && !boot_render_memeq(boot_render_bound.rtv, rtvs, n * sizeof rtvs[0])) ||
            (has_dsv && boot_render_bound.dsv.ptr != dsv.ptr)) {
            ID3D12GraphicsCommandList_OMSetRenderTargets(cl, s->rt_count, s->rt_count ? rtvs : NULL, FALSE, zt ? &dsv : NULL);
            boot_render_bound.nrt = s->rt_count; boot_render_bound.has_dsv = has_dsv; if (n) memcpy(boot_render_bound.rtv, rtvs, n * sizeof rtvs[0]);
            boot_render_bound.dsv = dsv;
        } else ++boot_render_binds_skipped; }
    float bf[4]; memcpy(bf, s->blend_const, sizeof bf);
    if (replicate_alpha) bf[0] = bf[1] = bf[2] = bf[3];
    if (memcmp(boot_render_bound.bf, bf, sizeof bf)) { ID3D12GraphicsCommandList_OMSetBlendFactor(cl, bf); memcpy(boot_render_bound.bf, bf, sizeof bf); }
    if (s->stencil_test && boot_render_bound.sref != (s->stencil_ref & 0xff)) {
        ID3D12GraphicsCommandList_OMSetStencilRef(cl, s->stencil_ref & 0xff); boot_render_bound.sref = s->stencil_ref & 0xff; }
    /* Maxwell viewport: window = scale * ndc + offset per axis; depth window_z = offset_z + scale_z * z_ndc. */
    const BootViewport *v = &s->viewport[0];
    D3D12_VIEWPORT vp;
    boot_render_viewport_map(v, &vp);
    if (vp.Width < 0) { boot_cbv_tap_issued(cbv_tap_token,(uintptr_t)in,0); return boot_render_fail(r, BOOT_RENDER_E_STATE, "negative viewport width"); }
    if (s->z_clip_zero_to_w) { vp.MinDepth = v->offset[2]; vp.MaxDepth = v->offset[2] + v->scale[2]; }
    else { vp.MinDepth = v->offset[2] - v->scale[2]; vp.MaxDepth = v->offset[2] + v->scale[2]; }
    if (vp.MinDepth < 0) vp.MinDepth = 0;
    if (vp.MaxDepth > 1) vp.MaxDepth = 1;
    if (memcmp(&boot_render_bound.vp, &vp, sizeof vp)) { ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); boot_render_bound.vp = vp; }
    const BootRenderTarget *first = zt;
    for (unsigned i = s->rt_count; i-- > 0; ) if (rts[i]) first = rts[i];
    uint32_t tw = first ? first->width : 0, th = first ? first->height : 0;
    D3D12_RECT sc = {0, 0, (LONG)tw, (LONG)th};
    if (s->scissor[0].enabled) { sc.left = s->scissor[0].x0; sc.right = s->scissor[0].x1; sc.top = s->scissor[0].y0; sc.bottom = s->scissor[0].y1; }
    if (memcmp(&boot_render_bound.sc, &sc, sizeof sc)) { ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc); boot_render_bound.sc = sc; }
    if (boot_render_bound.topo != topo) { ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, topo); boot_render_bound.topo = topo; }
    BootRenderTrace *tr = &boot_render_trace;
    if (tr->active && tr->qh && tr->query < BOOT_RENDER_TRACE_QUERIES) {
        memcpy(tr->vs, vs_id, 32); memcpy(tr->ps, ps_id, 32); tr->vp = vp; tr->sc = sc;
        tr->draw_count = draw_count; tr->indexed = (uint32_t)indexed; tr->topo = (uint32_t)topo;
        for (unsigned i = 0; i < 8; ++i) {
            tr->write_mask[i] = d.BlendState.RenderTarget[i].RenderTargetWriteMask; tr->blend_enable[i] = d.BlendState.RenderTarget[i].BlendEnable;
            tr->rt_index[i] = (i < s->rt_count && rts[i]) ? (uint32_t)(rts[i] - boot_render_rtab(r)->rts) : 0xffu;
            tr->rt_w[i] = tr->rt_h[i] = tr->rt_state[i] = tr->rt_fmt[i] = 0;
            if (i < s->rt_count && rts[i] && rts[i]->texture) { D3D12_RESOURCE_DESC rd; rts[i]->texture->lpVtbl->GetDesc(rts[i]->texture, &rd);
                tr->rt_w[i] = (uint32_t)rd.Width; tr->rt_h[i] = rd.Height; tr->rt_state[i] = (uint32_t)rts[i]->state; tr->rt_fmt[i] = (uint32_t)rd.Format; }
            tr->pso_rtfmt[i] = (uint32_t)d.RTVFormats[i];
        }
        tr->zt_index = zt ? (uint32_t)(zt - boot_render_rtab(r)->rts) : 0xffu;
        tr->pso_samples = d.SampleDesc.Count; tr->pso_cull = (uint32_t)d.RasterizerState.CullMode; tr->pso_front = (uint32_t)d.RasterizerState.FrontCounterClockwise; tr->pso_dsvfmt = (uint32_t)d.DSVFormat;
        ID3D12GraphicsCommandList_BeginQuery(cl, tr->qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, tr->query);
    }
    int occl = boot_render_occl.want;   /* ZPASS_PIXEL_COUNT enabled: count this draw's passing samples */
    if (occl) {
        if (!boot_render_occl.qh || !boot_render_occl.rb || boot_render_occl.next >= BOOT_RENDER_OCCL_QUERIES) {
            boot_cbv_tap_issued(cbv_tap_token,(uintptr_t)in,0);
            return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "occlusion query slots");
        }
        ID3D12GraphicsCommandList_BeginQuery(cl, boot_render_occl.qh, D3D12_QUERY_TYPE_OCCLUSION, boot_render_occl.next);
    }
    /* Guest-memory stores of earlier draws (UAV, space7) must be complete and visible before a later
     * draw reads guest memory (SRV, space7: vertex pulling, LDG). D3D12 orders neither without a UAV
     * barrier: the next draw's vertex shader may run while the storing draw's pixels are still in
     * flight. TOTK's G-buffer pixel shaders set per-tile flags (buffer + 0x1c0 + tile * 16 = 1) that
     * the tiled lighting pass's vertex shader (f835f38a) reads to cull its 64x64 tile quads: without
     * the barrier, tiles read stale 0 flags -> black tiles and missing local (torch) light. The Maxwell
     * command stream orders them with its own barrier/WFI methods (not recorded as events), so the
     * barrier goes before a draw the guest put behind a memory barrier (WAIT_FOR_IDLE / shader cache
     * invalidation, recorded with the draw: host_gpu_stage_draw.h) and, in any case, before the first
     * non-storing draw after storing ones; storing draws without a guest barrier between them (e.g. the
     * G-buffer pass) stay unserialized, as on the source GPU. */
    /* render-21 (A/B, TOTK_UAV_SERIALIZE=1): also between consecutive storing draws of different pixel programs. The
     * tiled-lighting variants (cap-blocks events 1014-1017: VS 383cb2bb/950221ef cull 16x16 tiles by the flags at
     * buffer + 0x41c0 + tile * 16, every variant PS stores word 3 of its tiles (+0x41cc) and the row-quad variants
     * (VS a98d96fd, PS a08137b7/f0d8509c) read those words to skip tiles an earlier variant shaded) are storing
     * draws back to back, so no barrier separates one variant's stores from the next variant's reads: a tile can
     * be skipped by both and keep the clear colour (black 16x16 half-res blocks at character/sword edges). */
    static int serialize = -1;
    if (serialize < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_UAV_SERIALIZE", v, sizeof v); serialize = k && k < sizeof v && v[0] == '1'; }
    static const BootRenderShader *last_store_ps;
    uint64_t tile89_pending_before=0,tile89_uav_before=0;const BootRenderShader *tile89_old_ps=NULL;unsigned tile89_emitted=0;
    if(boot_tile89_on()) { tile89_pending_before=(unsigned)boot_render_uav_pending;tile89_uav_before=boot_render_uav_barriers;tile89_old_ps=last_store_ps; }
    int serial = serialize && stores && boot_render_uav_pending && ps != last_store_ps;
    if (stores) last_store_ps = ps;
    if (boot_render_uav_pending && (!stores || in->guest_barrier || serial)) {
        D3D12_RESOURCE_BARRIER ub; memset(&ub, 0, sizeof ub); ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = NULL;
        BOOT_GP_RB("draw UAV", cl, 1, &ub);
        boot_render_uav_pending = 0; ++boot_render_uav_barriers;tile89_emitted=1;
    }
    if (stores) { boot_render_gmem_pending = 1; boot_render_uav_pending = 1; ++boot_render_store_draws; }   /* until a waiting submission / the next UAV barrier */
    if (stores && boot_ds_on()) boot_ds_store_stages = (boot_render_shader_stores(r, vs) ? 1u : 0u) | (ps && boot_render_shader_stores(r, ps) ? 2u : 0u);   /* data-snapshot: which stage stores */
    BootTile89Record *tile89=boot_tile89_issue_record(cbv_tap_token,(uintptr_t)in);
    if(tile89) {
        uint64_t *v89=tile89->v;v89[4]=(uintptr_t)r->gpu->device;v89[5]=(uintptr_t)r->gpu->queue;v89[6]=(uintptr_t)cl;v89[9]=(uintptr_t)r->gpu->fence;
        if(boot_render_seg.r==r && boot_render_seg.cur<BOOT_RENDER_SEGMENTS) { v89[7]=(uintptr_t)boot_render_seg.alloc[boot_render_seg.cur];v89[8]=boot_render_seg.cur; }
        else v89[8]=UINT64_MAX;
        v89[12]=(uintptr_t)d.VS.pShaderBytecode;v89[13]=d.VS.BytecodeLength;v89[14]=(uintptr_t)d.PS.pShaderBytecode;v89[15]=d.PS.BytecodeLength;
        v89[22]=s->first;v89[23]=s->base_vertex_reg;v89[24]=s->base_instance_reg;v89[31]=boot_render_trace.const_ps_on!=0;
        for(unsigned j=0;j<8;++j)v89[32+j]=host[j];
        v89[40]=(unsigned)stores;v89[41]=tile89_pending_before;v89[42]=in->guest_barrier;v89[43]=(unsigned)serialize;v89[44]=(uintptr_t)tile89_old_ps;
        v89[45]=(unsigned)serial;v89[46]=tile89_emitted;v89[47]=tile89_uav_before;v89[48]=boot_render_uav_barriers;v89[49]=boot_render_gw_settles;v89[212]=1;
        v89[213]=boot_render_gw_dest_count;for(unsigned j=0;j<boot_render_gw_dest_count && j<8;++j)v89[214+j]=(uintptr_t)boot_render_gw_dest[j];
        v89[222]=boot_render_gw_dest_count>8?boot_render_gw_dest_count-8:0;v89[223]=(unsigned)boot_render_gw_all;
        if(boot_tile89_tls.native_draw==(uintptr_t)in && boot_tile89_tls.native_executor) {
            const uint64_t *native89=boot_tile89_tls.native_values;v89[25]=1;v89[26]=native89[8];v89[27]=native89[9];v89[28]=native89[10];v89[29]=native89[11];v89[30]=1;
            v89[51]=native89[6];if(native89[6]) { v89[52]=boot_tile89_tls.barrier_flags;v89[53]=boot_tile89_tls.barrier_address;v89[54]=native89[7]; }
            v89[55]=boot_tile89_tls.native_executor;v89[208]=native89[4];v89[209]=native89[5];v89[210]=native89[3];
        }
        v89[211]=1;
    }
    boot_render_gw_settle(r);   /* guest memory written by GPU copies before this draw: readable again */
    if(tile89)tile89->v[50]=boot_render_gw_settles;
    boot_render_tex_commit();   /* the bound texture slots belong to this list */
    boot_render_fb_commit(r);   /*99: actual consuming logical list, never predicted Signal */
    if (boot_render_markers_on())   /* render-27a */
        boot_render_marker(cl, "draw", vs_id, has_ps ? ps_id : NULL, s->rt_count ? s->rt[s->rt_map[0]].address : (zt ? zt->address : 0));
    /* Diagnostic79: at the existing actual Draw* call, after resource resolution.
     * Hashes, PSO masks and target pointers are existing host locals. */
    if(boot_title_event_active && boot_title_interesting) {
        BootTitleRecord x={0}; x.kind=BOOT_TITLE_DRAW; x.sub=boot_title_sub; x.event=boot_title_event;
        memcpy(&x.v[0],vs_id,32); if(has_ps) memcpy(&x.v[4],ps_id,32);
        x.v[8]=draw_count; x.v[9]=draw_instances; x.v[10]=(unsigned)indexed; x.v[11]=(unsigned)topo; x.v[12]=s->rt_count;
        x.v[13]=d.SampleDesc.Count; x.v[14]=d.RasterizerState.CullMode; x.v[15]=has_ps; x.v[16]=d.DSVFormat;
        memcpy(&x.v[17],&vp.TopLeftX,4); memcpy(&x.v[18],&vp.TopLeftY,4); memcpy(&x.v[19],&vp.Width,4); memcpy(&x.v[20],&vp.Height,4);
        x.v[21]=((uint64_t)(uint32_t)sc.left<<32)|(uint32_t)sc.top; x.v[22]=((uint64_t)(uint32_t)sc.right<<32)|(uint32_t)sc.bottom;
        boot_title_push(&x);
        BootRenderRtTable *table=boot_render_image_table(r);
        for(unsigned j=0;j<s->rt_count && j<8;++j) {
            const BootColorTarget *raw=&s->rt[s->rt_map[j]]; const BootRenderTarget *rt=rts[j];
            BootTitleRecord z={0}; z.kind=BOOT_TITLE_HOST_RT; z.sub=boot_title_sub; z.event=boot_title_event;
            z.v[0]=j; z.v[1]=s->rt_map[j]; z.v[2]=raw->address; z.v[3]=raw->layers; z.v[4]=raw->layer; z.v[5]=raw->array_pitch; z.v[6]=raw->write_mask;
            z.v[7]=d.BlendState.RenderTarget[j].RenderTargetWriteMask; z.v[8]=d.BlendState.RenderTarget[j].BlendEnable; z.v[9]=d.RTVFormats[j];
            if(rt) { z.v[10]=(uintptr_t)rt->texture; z.v[11]=rt->address; z.v[12]=rt->width; z.v[13]=rt->height; z.v[14]=rt->dxgi; z.v[15]=rt->state;
                if(table && rt>=table->rts && rt<table->rts+table->count) { unsigned k=(unsigned)(rt-table->rts); z.v[16]=k; z.v[17]=boot_render_rt_written_at[k];
                    z.v[18]=table->info[k].known|((uint64_t)table->info[k].dirty<<8)|((uint64_t)table->info[k].stale<<16); z.v[19]=table->generation; } }
            boot_title_push(&z);
        }
    }
    /* Diagnostic86: select only the observed exact lighting shader and three
     * ordinary single-subresource attachments. No guest reads or new queries. */
    BootLight86Batch *light86=NULL;
    if(boot_light86_on() && has_ps && s->rt_count==3 && indexed && draw_count==864 && draw_instances==1 && topo==D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
       d.SampleDesc.Count==1 && boot_light86_hashes(vs_id,ps_id) && rts[0] && rts[1] && rts[2] &&
       boot_light86_in_window(&boot_light86.config,boot_render_exec_id,rts[0]->address,rts[1]->address)) {
        BootLight86Source source[3]={0}; uint64_t values[BOOT_L86_VALUES]={0};
        BootRenderRtTable *table=boot_render_image_table(r);
        for(unsigned j=0;j<3;++j) {
            const BootRenderTarget *rt=rts[j]; const BootColorTarget *raw=&s->rt[s->rt_map[j]];
            source[j]=(BootLight86Source){(uintptr_t)rt->texture,rt->address,0,UINT64_MAX,0,rt->width,rt->height,rt->dxgi,rt->state,raw->layer,raw->layers,raw->array_pitch};
            if(table && rt>=table->rts && rt<table->rts+table->count) { unsigned i=(unsigned)(rt-table->rts); source[j].sequence=boot_render_rt_written_at[i]; source[j].index=i; source[j].generation=table->generation; }
        }
        values[0]=(uintptr_t)pso; values[1]=vs_mask; values[2]=ps->vs_mask; values[3]=ps_outputs;
        values[4]=draw_count; values[5]=draw_instances; values[6]=(unsigned)indexed; values[7]=(unsigned)topo;
        values[8]=ib.BufferLocation; values[9]=ib.SizeInBytes; values[10]=ib.Format; values[11]=d.NumRenderTargets;
        values[12]=d.SampleDesc.Count; values[13]=d.SampleDesc.Quality; values[14]=d.SampleMask;
        values[15]=d.BlendState.AlphaToCoverageEnable; values[16]=d.BlendState.IndependentBlendEnable;
        for(unsigned j=0;j<3;++j) {
            const D3D12_RENDER_TARGET_BLEND_DESC *b=&d.BlendState.RenderTarget[j]; unsigned k=17+j*10;
            values[k]=b->BlendEnable; values[k+1]=b->LogicOpEnable; values[k+2]=b->SrcBlend; values[k+3]=b->DestBlend; values[k+4]=b->BlendOp;
            values[k+5]=b->SrcBlendAlpha; values[k+6]=b->DestBlendAlpha; values[k+7]=b->BlendOpAlpha; values[k+8]=b->LogicOp; values[k+9]=b->RenderTargetWriteMask;
        }
        for(unsigned j=0;j<4;++j) values[47+j]=boot_light86_bits(bf[j]);
        values[51]=d.RasterizerState.FillMode; values[52]=d.RasterizerState.CullMode; values[53]=d.RasterizerState.FrontCounterClockwise;
        values[54]=(uint32_t)d.RasterizerState.DepthBias; values[55]=boot_light86_bits(d.RasterizerState.DepthBiasClamp); values[56]=boot_light86_bits(d.RasterizerState.SlopeScaledDepthBias);
        values[57]=d.RasterizerState.DepthClipEnable; values[58]=d.RasterizerState.MultisampleEnable; values[59]=d.RasterizerState.AntialiasedLineEnable; values[60]=d.RasterizerState.ForcedSampleCount; values[61]=d.RasterizerState.ConservativeRaster;
        values[62]=d.DSVFormat; values[63]=d.DepthStencilState.DepthEnable; values[64]=d.DepthStencilState.DepthWriteMask; values[65]=d.DepthStencilState.DepthFunc;
        values[66]=d.DepthStencilState.StencilEnable; values[67]=d.DepthStencilState.StencilReadMask; values[68]=d.DepthStencilState.StencilWriteMask;
        values[69]=d.DepthStencilState.FrontFace.StencilFailOp; values[70]=d.DepthStencilState.FrontFace.StencilDepthFailOp; values[71]=d.DepthStencilState.FrontFace.StencilPassOp; values[72]=d.DepthStencilState.FrontFace.StencilFunc;
        values[73]=d.DepthStencilState.BackFace.StencilFailOp; values[74]=d.DepthStencilState.BackFace.StencilDepthFailOp; values[75]=d.DepthStencilState.BackFace.StencilPassOp; values[76]=d.DepthStencilState.BackFace.StencilFunc;
        values[77]=boot_render_bound.sref; values[78]=s->stencil_test;
        values[79]=boot_light86_bits(vp.TopLeftX); values[80]=boot_light86_bits(vp.TopLeftY); values[81]=boot_light86_bits(vp.Width); values[82]=boot_light86_bits(vp.Height); values[83]=boot_light86_bits(vp.MinDepth); values[84]=boot_light86_bits(vp.MaxDepth);
        values[85]=(uint32_t)sc.left; values[86]=(uint32_t)sc.top; values[87]=(uint32_t)sc.right; values[88]=(uint32_t)sc.bottom;
        values[89]=(uintptr_t)(zt?zt->texture:NULL); values[90]=zt?zt->address:0; values[91]=zt?zt->state:0; values[92]=dsv.ptr;
        values[93]=(uintptr_t)d.VS.pShaderBytecode; values[94]=d.VS.BytecodeLength; values[95]=(uintptr_t)d.PS.pShaderBytecode; values[96]=d.PS.BytecodeLength;
        values[97]=d.IBStripCutValue; values[98]=d.PrimitiveTopologyType; values[99]=boot_render_bound.epoch;
        for(unsigned j=0;j<3;++j) { values[100+j]=s->rt_map[j]; values[103+j]=d.RTVFormats[j]; values[106+j]=rtvs[j].ptr; values[109+j]=s->rt[s->rt_map[j]].write_mask; }
        unsigned query_active=occl || (tr->active && tr->qh && tr->query<BOOT_RENDER_TRACE_QUERIES);
        values[112]=query_active; values[113]=replicate_alpha;
        light86=boot_light86_begin(r,cl,boot_render_exec_id,boot_render_watch_event,source,values,query_active?BOOT_L86_SKIP_QUERY:0);
        if(light86) boot_light86_snapshot(light86,cl,2,1);
    }

    boot_light84_draw(boot_render_exec_id, boot_render_watch_event);
    if (indexed) {
        ID3D12GraphicsCommandList_IASetIndexBuffer(cl, &ib);
        boot_dred_work(cl, ind.kind ? 5u : BOOT_DRED_INDEXED, fb_any ? BOOT_DRED_FEEDBACK : BOOT_DRED_MAIN, vs_id, has_ps ? ps_id : NULL, s->rt_count ? s->rt[s->rt_map[0]].address : (zt ? zt->address : 0), boot_render_exec_id, boot_render_watch_event);
        BOOT_GP_DRAW_BEGIN(cl, pso, rts, s->rt_count, zt, vs_id, has_ps ? ps_id : NULL);   /* render-40 */
        int pred = boot_render_pred_begin(r);   /* render-38c: the game's conditional render, decided on the GPU */
        if (ind.kind) boot_render_ind_execute(r, &ind);   /* render-38d: D3D12 op 5 (ExecuteIndirect) in the DRED ledger */
        else ID3D12GraphicsCommandList_DrawIndexedInstanced(cl, draw_count, draw_instances, 0, 0, 0);
        boot_render_pred_end(r, pred);
        BOOT_GP_DRAW_END(cl);
    } else {
        boot_dred_work(cl, ind.kind ? 5u : BOOT_DRED_DRAW, fb_any ? BOOT_DRED_FEEDBACK : BOOT_DRED_MAIN, vs_id, has_ps ? ps_id : NULL, s->rt_count ? s->rt[s->rt_map[0]].address : (zt ? zt->address : 0), boot_render_exec_id, boot_render_watch_event);
        BOOT_GP_DRAW_BEGIN(cl, pso, rts, s->rt_count, zt, vs_id, has_ps ? ps_id : NULL);   /* render-40 */
        int pred = boot_render_pred_begin(r);   /* render-38c */
        if (ind.kind) boot_render_ind_execute(r, &ind);   /* render-38d */
        else ID3D12GraphicsCommandList_DrawInstanced(cl, draw_count, draw_instances, 0, 0);   /* first vertex/instance are applied in the shader */
        boot_render_pred_end(r, pred);
        BOOT_GP_DRAW_END(cl);
    }
    boot_cbv_tap_issued(cbv_tap_token,(uintptr_t)in,1);
    if(light86) {
        boot_light86_snapshot(light86,cl,0,0); boot_light86_snapshot(light86,cl,1,0); boot_light86_snapshot(light86,cl,2,0); boot_light86_end(light86,1);
    }
    {   uint64_t ph1 = boot_render_ph_now(); boot_render_ph_qpc[7] += ph1 - ph; ph = ph1; }
    if (occl) {
        ID3D12GraphicsCommandList_EndQuery(cl, boot_render_occl.qh, D3D12_QUERY_TYPE_OCCLUSION, boot_render_occl.next);
        boot_render_occl.got = ++boot_render_occl.next;
    }
    if (tr->active && tr->qh && tr->query < BOOT_RENDER_TRACE_QUERIES) {
        ID3D12GraphicsCommandList_EndQuery(cl, tr->qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, tr->query);
        ID3D12GraphicsCommandList_ResolveQueryData(cl, tr->qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, tr->query, 1, tr->rb,
                                                   (UINT64)tr->query * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        tr->drawn = 1;
        tr->so_last = -1;
        if (tr->so_count < BOOT_RENDER_SO_PROBES && d.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) {
            /* repeat the draw with SV_Position streamed out (no rasterization): what the rasterizer received */
            if (!tr->so_buf) {
                tr->so_buf = boot_render_buffer(r, D3D12_HEAP_TYPE_DEFAULT, (uint64_t)BOOT_RENDER_SO_PROBES * BOOT_RENDER_SO_SLOT, D3D12_RESOURCE_STATE_COMMON, NULL);
                tr->so_rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, (uint64_t)BOOT_RENDER_SO_PROBES * BOOT_RENDER_SO_SLOT, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
                void *z = NULL; tr->so_zero = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ, &z);
                if (z) memset(z, 0, 256);
            }
            static const D3D12_SO_DECLARATION_ENTRY so_decl = {0, "SV_Position", 0, 0, 4, 0};
            static const UINT so_stride = 16;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC sd = d;
            sd.StreamOutput.pSODeclaration = &so_decl; sd.StreamOutput.NumEntries = 1;
            sd.StreamOutput.pBufferStrides = &so_stride; sd.StreamOutput.NumStrides = 1;
            sd.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
            ID3D12PipelineState *sp = NULL;
            if (tr->so_buf && tr->so_rb && tr->so_zero &&
                SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(r->gpu->device, &sd, &IID_ID3D12PipelineState, (void **)&sp))) {
                unsigned k = tr->so_count++; tr->so_pso[k] = sp; tr->so_last = (int)k;   /* released after the submission (boot_render_trace_so_release) */
                UINT64 at = (UINT64)k * BOOT_RENDER_SO_SLOT;
                ID3D12GraphicsCommandList_CopyBufferRegion(cl, tr->so_buf, at, tr->so_zero, 0, 16);   /* filled size 0 (COMMON -> COPY_DEST promotion) */
                D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tr->so_buf;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_STREAM_OUT;
                ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
                D3D12_STREAM_OUTPUT_BUFFER_VIEW sv = {ID3D12Resource_GetGPUVirtualAddress(tr->so_buf) + at + 16, BOOT_RENDER_SO_SLOT - 16,
                                                      ID3D12Resource_GetGPUVirtualAddress(tr->so_buf) + at};
                ID3D12GraphicsCommandList_SetPipelineState(cl, sp);
                ID3D12GraphicsCommandList_SOSetTargets(cl, 0, 1, &sv);
                boot_render_tex_commit();
                boot_render_fb_commit(r);
                boot_dred_work(cl, indexed ? BOOT_DRED_INDEXED : BOOT_DRED_DRAW, BOOT_DRED_SO, vs_id, NULL, 0, boot_render_exec_id, boot_render_watch_event);
                if (ind.kind) boot_render_ind_execute(r, &ind);   /* render-38d: all of the arguments' instances */
                else if (indexed) ID3D12GraphicsCommandList_DrawIndexedInstanced(cl, draw_count, 1, 0, 0, 0);
                else ID3D12GraphicsCommandList_DrawInstanced(cl, draw_count, 1, 0, 0);
                ID3D12GraphicsCommandList_SOSetTargets(cl, 0, 0, NULL);
                ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
                ID3D12GraphicsCommandList_CopyBufferRegion(cl, tr->so_rb, at, tr->so_buf, at, BOOT_RENDER_SO_SLOT);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
            }
        }
    }
    return BOOT_RENDER_OK;
}
/*99: retain every provisional copy/descriptor through all early returns and
 * optional secondary Draws. Nested entry owns only its unique per-frame pins. */
static inline int boot_render_draw(BootRender *r, const BootRenderDraw *in) {
    unsigned index = 0; while (index < 16 && boot_render_fb_frames99[index].active) ++index;
    if (index == 16) { if (BOOT_CENSUS_ON()) BOOT_CENSUS_DRAW(in->state, BOOT_RENDER_E_CAPACITY, "feedback draw ownership");
                       return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "feedback draw ownership"); }
    BootRenderFbFrame99 *frame = &boot_render_fb_frames99[index]; memset(frame,0,sizeof *frame);
    frame->active = 1; frame->prev = boot_render_fb_current99; frame->r = r; boot_render_fb_current99 = index;
    int result = boot_render_draw_impl99(r, in);
    if (boot_sd_on()) boot_sd_draw_end();   /* suspect-diag */
    if (BOOT_CENSUS_ON()) BOOT_CENSUS_DRAW(in->state, result, result ? r->error : NULL);
    boot_render_fb_current99 = frame->prev;
    if (frame->copies) for (unsigned k = 0; k < BOOT_RENDER_FB_COPIES; ++k) if (frame->copies & (1u << k)) --boot_render_fb_copy_pins99[k];
    for (unsigned word = 0; word < 4; ++word) if (frame->slots[word])
        for (unsigned bit = 0; bit < 64; ++bit) if (frame->slots[word] & (UINT64_C(1) << bit)) --boot_render_fb_slot_pins99[word*64+bit];
    memset(frame,0,sizeof *frame);
    return result;
}
/* After the traced submission completed: the stream-output pipelines can go. */
static inline void boot_render_trace_so_release(void) {
    BootRenderTrace *tr = &boot_render_trace;
    for (unsigned k = 0; k < tr->so_count; ++k) if (tr->so_pso[k]) { ID3D12PipelineState_Release(tr->so_pso[k]); tr->so_pso[k] = NULL; }
    tr->so_count = 0;
}

/* CLEAR_SURFACE: value fields Z(0) stencil(1) R,G,B,A(2-5) MRT select(6-9) layer(10-25). */
/* Masked colour clears: D3D12 clears whole texels, so a clear of some channels draws a full-target
 * triangle with that channel write mask (clear-shaders/clear.hlsl, compiled ahead of time; the
 * graphics root signature, clear value in pixel-stage b0). PSOs are cached per format and mask. */
static void *boot_render_clear_blob[4]; static size_t boot_render_clear_blob_size[4];   /* vs, float, uint, sint ps */
static struct { DXGI_FORMAT format; UINT8 mask; ID3D12PipelineState *pso; } boot_render_clear_psos[64];
static unsigned boot_render_clear_pso_count;
static inline int boot_render_clear_masked(BootRender *r, BootRenderTarget *t, unsigned mask, const float c[4], const D3D12_RECT *rect) {
    static const char *names[4] = {"clear.vs.dxil", "clear-float.ps.dxil", "clear-uint.ps.dxil", "clear-sint.ps.dxil"};
    for (unsigned i = 0; i < 4; ++i) if (!boot_render_clear_blob[i]) {
        char path[160]; snprintf(path, sizeof path, "startup\\clear-shaders\\%s", names[i]);
        FILE *b = fopen(path, "rb");
        if (!b) { snprintf(path, sizeof path, "clear-shaders\\%s", names[i]); b = fopen(path, "rb"); }
        if (!b) { snprintf(r->error, sizeof r->error, "masked clear shader %s missing", names[i]); return BOOT_RENDER_E_SHADER; }
        fseek(b, 0, SEEK_END); long n = ftell(b); fseek(b, 0, SEEK_SET);
        void *data = n > 0 ? malloc((size_t)n) : NULL;
        if (data && fread(data, 1, (size_t)n, b) == (size_t)n) { boot_render_clear_blob[i] = data; boot_render_clear_blob_size[i] = (size_t)n; } else free(data);
        fclose(b);
        if (!boot_render_clear_blob[i]) return boot_render_fail(r, BOOT_RENDER_E_SHADER, "masked clear shader read");
    }
    int is_signed; uint32_t bits = boot_render_dxgi_int_bits(t->dxgi, &is_signed);
    unsigned kind = !bits ? 1 : is_signed ? 3 : 2;
    ID3D12PipelineState *pso = NULL;
    for (unsigned i = 0; i < boot_render_clear_pso_count; ++i)
        if (boot_render_clear_psos[i].format == t->dxgi && boot_render_clear_psos[i].mask == mask) { pso = boot_render_clear_psos[i].pso; break; }
    if (!pso) {
        if (boot_render_clear_pso_count == 64) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "masked clear PSO cache full");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
        d.pRootSignature = r->root;
        d.VS.pShaderBytecode = boot_render_clear_blob[0]; d.VS.BytecodeLength = boot_render_clear_blob_size[0];
        d.PS.pShaderBytecode = boot_render_clear_blob[kind]; d.PS.BytecodeLength = boot_render_clear_blob_size[kind];
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1; d.RTVFormats[0] = t->dxgi; d.SampleDesc.Count = 1;
        const uint32_t clear_key[3] = {(uint32_t)mask, (uint32_t)t->dxgi, kind};
        HRESULT hr = boot_psoc_create_graphics(r, r->gpu->device, &d, clear_key, sizeof clear_key, &pso);   /* play (73) */
        if (FAILED(hr)) { snprintf(r->error, sizeof r->error, "masked clear PSO hr=0x%08lx", (unsigned long)hr); return BOOT_RENDER_E_D3D; }
        boot_render_clear_psos[boot_render_clear_pso_count].format = t->dxgi; boot_render_clear_psos[boot_render_clear_pso_count].mask = (UINT8)mask;
        boot_render_clear_psos[boot_render_clear_pso_count++].pso = pso;
    }
    /* clear value: float4 for float/normalised formats, uint4/int4 (already scaled) for integer ones */
    if (r->recording && (r->cbv_used + 19 > boot_render_cbv_end || r->ring_used + 256 > r->ring_size) && (++boot_render_ring_full, !boot_render_flush(r)))
        return boot_render_fail(r, BOOT_RENDER_E_D3D, "execute (ring segment full)");
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    uint8_t *cb = r->ring_cpu + r->ring_used; memset(cb, 0, 256);
    memcpy(cb, c, 16);
    for (unsigned k = 0; k < 4; ++k) { int32_t v = (int32_t)c[k]; memcpy(cb + 16 + 4 * k, &v, 4); }
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv = {ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used, 256};
    uint32_t base = BOOT_RENDER_HEAP_RING + r->cbv_used;
    ID3D12Device_CreateConstantBufferView(r->gpu->device, &cv, boot_render_cpu(r, r->srv_heap, r->srv_inc, base));
    r->ring_used += 256; r->cbv_used += 19;
    ID3D12GraphicsCommandList *cl = r->cl;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = boot_render_rtv(r, t);
    D3D12_VIEWPORT vp = {0, 0, (float)t->width, (float)t->height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)t->width, (LONG)t->height}; if (rect) sc = *rect;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, r->root);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, boot_render_gpu(r, r->srv_heap, r->srv_inc, base));
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL); ++boot_render_bind_epoch;
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    boot_render_gw_settle(r);
    boot_dred_work(cl, BOOT_DRED_DRAW, BOOT_DRED_CLEAR, NULL, NULL, t->address, boot_render_exec_id, boot_render_watch_event);
    BOOT_GP_PRE(cl);   /* render-40 */
    int pred = boot_render_pred_begin(r);   /* render-38c: a masked clear is the game's clear */
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    boot_render_pred_end(r, pred);
    BOOT_GP_POST(BOOT_GP_CLEAR, "masked clear", cl, 0, 0);
    return BOOT_RENDER_OK;
}
/* CLEAR_SURFACE: value bits 0-1 depth/stencil, 2-5 RGBA, 6-9 target, 10-25 layer. A layer of a
 * layered target is the 2D target at address + layer * array pitch (SET_COLOR_TARGET_ARRAY_PITCH in 4-byte units, Ryujinx RtColorState.LayerSize * 4),
 * which is how each layer's host target is keyed. */
static inline int boot_render_clear(BootRender *r, const BootDrawState *s, const uint32_t *regs, uint32_t value) {
    boot_render_fb_pb_sync(2);   /* batch-c: a clear writes a target: feedback copies are retaken */
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    unsigned rgba = (value >> 2) & 15, target = (value >> 6) & 15, layer = (value >> 10) & 0xffff;
    D3D12_RECT rect = {0, 0, 0, 0}; int use_rect = 0;
    if ((regs[0x10f8 / 4] & 1) && s->scissor[0].enabled) {   /* SET_CLEAR_SURFACE_CONTROL: respect scissor 0 */
        rect.left = s->scissor[0].x0; rect.right = s->scissor[0].x1; rect.top = s->scissor[0].y0; rect.bottom = s->scissor[0].y1; use_rect = 1;
    }
    if (rgba) {
        if (target >= 8) return boot_render_fail(r, BOOT_RENDER_E_STATE, "clear target");
        if (s->rt[target].format == 0) goto clear_depth;   /* NONE: no colour target to clear */
        BootColorTarget ct = s->rt[target];
        if (layer) {
            if (layer >= (ct.layers ? ct.layers : 1) || !ct.array_pitch) return boot_render_fail(r, BOOT_RENDER_E_STATE, "clear layer outside the target");
            ct.address += (uint64_t)layer * ct.array_pitch * 4; ct.layers = 1;
        }
        BootRenderTarget *t = boot_render_target(r, &ct);
        if (!t) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "clear target format");
        boot_render_rt_barrier(r, t, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float c[4]; for (unsigned k = 0; k < 4; ++k) c[k] = boot_f32(regs[(0xd80 + 4 * k) / 4]);
        /* Integer targets: the clear colour is taken as normalised and scaled to the channel range (clb197.h
         * defines SET_COLOR_CLEAR_VALUE only as raw words; this conversion is not checked against hardware);
         * D3D12 converts the float clear value to the integer by value. */
        int is_signed; uint32_t bits = boot_render_dxgi_int_bits(t->dxgi, &is_signed);
        for (unsigned k = 0; bits && k < 4; ++k) {
            double v;
            if (!is_signed) { double mx = (double)((UINT64_C(1) << bits) - 1); v = mx * c[k]; v = v < 0 ? 0 : v > mx ? mx : v; }
            else { double mx = (double)((INT64_C(1) << (bits - 1)) - 1); v = mx * (c[k] * 2.0 - 1.0); v = v < -mx - 1 ? -mx - 1 : v > mx ? mx : v; }
            c[k] = (float)(int64_t)v;
        }
        if(boot_title_event_active && boot_title_range(ct.address,1)) {
            BootTitleRecord x={0}; x.kind=BOOT_TITLE_CLEAR; x.sub=boot_title_sub; x.event=boot_title_event;
            x.v[0]=target; x.v[1]=ct.address; x.v[2]=layer; x.v[3]=ct.layers; x.v[4]=ct.array_pitch; x.v[5]=rgba; x.v[6]=value;
            x.v[7]=(uintptr_t)t->texture; x.v[8]=t->address; x.v[9]=t->width; x.v[10]=t->height; x.v[11]=t->dxgi; x.v[12]=t->state;
            for(unsigned j=0;j<4;++j) { x.v[13+j]=regs[(0xd80+4*j)/4]; memcpy(&x.v[17+j],&c[j],4); }
            x.v[21]=use_rect; x.v[22]=((uint64_t)(uint32_t)rect.left<<32)|(uint32_t)rect.top; x.v[23]=((uint64_t)(uint32_t)rect.right<<32)|(uint32_t)rect.bottom;
            boot_title_push(&x);
        }
        if (rgba == 15) {   /* render-38c: predicated only around the clear itself (the target's creation/load above is not) */
            BOOT_GP_PRE(r->cl);   /* render-40: marks outside the predication bracket */
            int pred = boot_render_pred_begin(r);
            ID3D12GraphicsCommandList_ClearRenderTargetView(r->cl, boot_render_rtv(r, t), c, use_rect, use_rect ? &rect : NULL);
            boot_render_pred_end(r, pred);
            BOOT_GP_POST(BOOT_GP_CLEAR, "clear colour", r->cl, 0, 0);
        }
        else { int rc = boot_render_clear_masked(r, t, rgba, c, use_rect ? &rect : NULL); if (rc) return rc; }
    }
clear_depth:
    if (value & 3) {
        BootDepthTarget zt = s->zt;
        if (layer) {
            if (layer >= (zt.layers ? zt.layers : 1) || !zt.array_pitch) return boot_render_fail(r, BOOT_RENDER_E_STATE, "clear layer outside the depth target");
            zt.address += (uint64_t)layer * zt.array_pitch * 4; zt.layers = 1;
        }
        BootRenderTarget *z = boot_render_depth(r, &zt);
        if (!z) return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "clear depth target format");
        boot_render_rt_barrier(r, z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)(((value & 1) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((value & 2) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        BOOT_GP_PRE(r->cl);   /* render-40 */
        int pred = boot_render_pred_begin(r);   /* render-38c */
        ID3D12GraphicsCommandList_ClearDepthStencilView(r->cl, boot_render_dsv(r, z), f,
            boot_f32(regs[0xd90 / 4]), (UINT8)regs[0xda0 / 4], use_rect, use_rect ? &rect : NULL);
        boot_render_pred_end(r, pred);
        BOOT_GP_POST(BOOT_GP_CLEAR, "clear depth", r->cl, 0, 0);
    }
    return BOOT_RENDER_OK;
}
