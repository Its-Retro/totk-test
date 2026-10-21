#ifndef TOTK_HOST_NVN_PREDICATE_H
#define TOTK_HOST_NVN_PREDICATE_H
/* render-38c: nvnCommandBufferSetRenderEnableConditional evaluated on the GPU with D3D12 predication, replacing
 * the CPU evaluation of render-38 (host_nvn_play.h boot_nvn_render_on: boot_render_cpu_read executed the open list
 * and waited for the GPU whenever the compared counters were still GPU-written: ~40 evaluations and ~15 GPU syncs
 * per frame in play-74, fps 9 -> 5 over a run). render-38b's shadow of our ReportCounter values never hit in play-75
 * (0 hits, 77,802 misses): the counters are written by the game's own GPU work (compute visibility / culling).
 *
 * At the first draw or clear under a condition (and again after work that may have changed the counters, below), a
 * 1x1x1 compute dispatch (predicate-shaders/predicate.hlsl, DXIL in host_nvn_predicate_dxil.h) reads the two 64-bit
 * counter words at the condition's address and +16 from the zero-copy guest region, on the GPU, and writes 1 (equal)
 * or 0 into the next 8-byte slot of a predication ring. The draws and clears are then recorded as before, each D3D12
 * draw/clear call bracketed with SetPredication(ring, slot, op) (host_gpu_render.h boot_render_pred): RENDER_IF_EQUAL
 * skips when the slot is 0 (EQUAL_ZERO), RENDER_IF_NOT_EQUAL when it is 1 (NOT_EQUAL_ZERO). No CPU read of the
 * counters, no wait, nothing decoded: every draw the game issued is recorded and counted (e->draws, the census), and
 * the GPU drops exactly the ones the game's condition disables, reading the condition's memory at its place in the
 * queue (after the game's earlier GPU work, as the CPU path's wait did). Copies, compute, internal write-backs and
 * the predicate dispatch itself are never predicated (only the draw/clear calls are bracketed).
 *
 * Ordering: before the dispatch, region buffers written by GPU copies in the open list go back to COMMON
 * (boot_render_gw_settle) and a global UAV barrier orders it after earlier compute and storing draws; the ring goes
 * UNORDERED_ACCESS for the write and PREDICATION after it (that transition makes the write visible to the predicate
 * read). A list kicked or reset in between needs nothing more: the slot's value stays in the ring (lists execute in
 * order), the ring's state is tracked per list (boot_render_pred.state) and predication is set only around one call.
 *
 * Re-evaluated (one more dispatch, next slot) when the condition is set again, at every submission (the guest CPU
 * may have written the memory between submissions), after the game's compute dispatches, copies, and buffer clears /
 * reports / uniform updates over the counters; with TOTK_RENDER_COND=strict also after any draw whose shaders store
 * to guest memory (the CPU path's per-draw semantics; costs a dispatch and a UAV barrier per such draw). Otherwise
 * the slot is reused: the draws under one condition share one dispatch.
 *
 * Falls back to the CPU evaluation (for that draw/clear) for a counter address not 4-byte aligned or not contiguous
 * in one guest region for 24 bytes, or when the objects failed to create. TOTK_RENDER_COND=cpu selects the CPU path of
 * render-38/38b for every condition (A/B). TOTK_RENDER_ENABLE=0/2 keep their meaning (host_nvn_play.h). */
#include "host_nvn_predicate_dxil.h"

#define BOOT_NVN_PRED_SLOTS 4096   /* 8-byte predicate slots (32 KiB), round robin: far more than lists in flight */
static struct {
    ID3D12Device *device; ID3D12RootSignature *root; ID3D12PipelineState *pso; ID3D12Resource *ring; int failed;
    unsigned next;                                     /* next ring slot */
    int valid; unsigned slot; uint64_t addr, gen, stores;   /* the evaluation in use: its slot and validity key */
} boot_nvn_pred;
static uint64_t boot_nvn_pred_gen = 1;   /* bumped by work after which the counters must be read again */
static uint64_t boot_nvn_pred_evals, boot_nvn_pred_reused, boot_nvn_pred_fallbacks, boot_nvn_pred_cpu_reads, boot_nvn_pred_cpu_syncs;

/* 0 cpu (render-38/38b), 1 gpu (default), 2 gpu, also re-evaluated after storing draws */
static inline int boot_nvn_render_cond_mode(void) {
    static int v = -1;
    if (v < 0) { char s[16]; DWORD n = GetEnvironmentVariableA("TOTK_RENDER_COND", s, sizeof s); int ok = n && n < sizeof s;
                 v = ok && !_stricmp(s, "cpu") ? 0 : ok && !_stricmp(s, "strict") ? 2 : 1;
                 boot_draw_log("nvn native: conditional render evaluated %s (TOTK_RENDER_COND mode %llu, 2 = strict) %llu", v ? "on the GPU (D3D12 predication)" : "on the CPU", (uint64_t)v, 0); }
    return v;
}
static inline void boot_nvn_pred_touch(void) { ++boot_nvn_pred_gen; }
/* a GPU-ordered store of [va, va + size) (uniform update, report, buffer clear): only stores over the counters count */
static inline void boot_nvn_pred_touch_range(const BootNvnExec *e, uint64_t va, uint64_t size) {
    if (e->render_cond_mode && va < e->render_cond_addr + 24 && e->render_cond_addr < va + size) ++boot_nvn_pred_gen;
}
static inline void boot_nvn_pred_release(void) {
    if (boot_render_pred.buffer && boot_render_pred.buffer == boot_nvn_pred.ring) { boot_render_pred.buffer = NULL; boot_render_pred.armed = 0; }
    if (boot_nvn_pred.pso) ID3D12PipelineState_Release(boot_nvn_pred.pso);
    if (boot_nvn_pred.root) ID3D12RootSignature_Release(boot_nvn_pred.root);
    if (boot_nvn_pred.ring) ID3D12Resource_Release(boot_nvn_pred.ring);
    memset(&boot_nvn_pred, 0, sizeof boot_nvn_pred);
}
/* Root signature (8 root constants b0, root UAVs u0 source / u1 ring: tile.hlsl's layout), pipeline and ring of the
 * render object's device; once per device (a recreated device gets new ones). */
static inline int boot_nvn_pred_open(BootRender *r) {
    ID3D12Device *dev = r->gpu->device;
    if (boot_nvn_pred.device == dev) return !boot_nvn_pred.failed;
    if (boot_nvn_pred.device) boot_nvn_pred_release();   /* objects of another (removed) device */
    boot_nvn_pred.device = dev; boot_nvn_pred.failed = 1;
    D3D12_ROOT_PARAMETER1 p[3]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 8;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor.ShaderRegister = 0; p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.ShaderRegister = 1; p[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    for (unsigned i = 0; i < 3; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 3; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) {
        if (err) ID3D10Blob_Release(err);
        boot_draw_log("nvn native: conditional render predicate%s: root signature serialization failed, CPU evaluation %llu %llu", "", 0, 0); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&boot_nvn_pred.root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(boot_nvn_pred.root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { boot_nvn_pred.root = NULL; boot_draw_log("nvn native: conditional render predicate%s: root signature hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd; memset(&cd, 0, sizeof cd); cd.pRootSignature = boot_nvn_pred.root;
    cd.CS.pShaderBytecode = boot_nvn_predicate_dxil; cd.CS.BytecodeLength = sizeof boot_nvn_predicate_dxil;
    if (FAILED(hr = boot_psoc_create_compute(r, dev, &cd, &boot_nvn_pred.pso))) {
        boot_nvn_pred.pso = NULL; boot_draw_log("nvn native: conditional render predicate%s: pipeline hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = (uint64_t)BOOT_NVN_PRED_SLOTS * 8; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&boot_nvn_pred.ring))) {
        boot_nvn_pred.ring = NULL; boot_draw_log("nvn native: conditional render predicate%s: ring hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    ++boot_render_created_buffers;
    boot_render_pred.buffer = boot_nvn_pred.ring; boot_render_pred.state = D3D12_RESOURCE_STATE_COMMON;   /* untouched by any list yet */
    boot_nvn_pred.failed = 0;
    boot_draw_log("nvn native: conditional render predicate%s ready: %llu slots, DXIL %llu bytes", "", (uint64_t)BOOT_NVN_PRED_SLOTS, (uint64_t)sizeof boot_nvn_predicate_dxil);
    return 1;
}
/* Records the predicate of the counters at va into the next ring slot (boot_nvn_pred.slot). 0 = not possible here
 * (the caller evaluates on the CPU). Nothing is predicated while it records (predication is only set around one
 * draw/clear call), so the dispatch always runs. */
static inline int boot_nvn_pred_eval(BootRender *r, uint64_t va) {
    int reg; uint64_t off;
    if ((va & 3) || boot_render_region_span(r, va, 24, &reg, &off) < 24) return 0;
    if (!r->recording && !boot_render_begin(r)) return 0;
    ID3D12GraphicsCommandList *cl = r->cl;
    unsigned slot = boot_nvn_pred.next; boot_nvn_pred.next = (slot + 1) % BOOT_NVN_PRED_SLOTS;
    boot_render_gw_settle(r);   /* guest memory written by GPU copies earlier in this list: readable again */
    {   D3D12_RESOURCE_BARRIER ub; memset(&ub, 0, sizeof ub); ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = NULL;
        BOOT_GP_RB("predicate", cl, 1, &ub);   /* after earlier compute and storing draws */
        boot_render_uav_pending = 0; ++boot_render_uav_barriers; }
    boot_render_pred_to(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uint64_t base = off & ~UINT64_C(255);   /* root UAV at a 256-byte aligned address; the counters at +src_offset */
    uint32_t k[8] = {(uint32_t)(off - base), slot * 8u, 0, 0, 0, 0, 0, 0};
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, boot_nvn_pred.root);
    ID3D12GraphicsCommandList_SetPipelineState(cl, boot_nvn_pred.pso); ++boot_render_bind_epoch;   /* replaced the graphics pipeline */
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 8, k, 0);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(r->region[reg]) + base);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(boot_nvn_pred.ring));
    boot_dred_work(cl, BOOT_DRED_DISPATCH, BOOT_DRED_COMPUTE, NULL, NULL, va, boot_render_exec_id, boot_render_watch_event);
    BOOT_GP_DISPATCH("predicate", 0, cl, 1, 1, 1);
    boot_render_pred_to(r, D3D12_RESOURCE_STATE_PREDICATION);   /* the write is visible to the predicate read */
    boot_nvn_pred.slot = slot;
    return 1;
}
/* The CPU evaluation's read (render-38, fallback / TOTK_RENDER_COND=cpu), counted with whether it executed the open
 * list or waited for a fence (the GPU syncs this module removes). */
static inline int boot_nvn_pred_cpu_read(BootRender *r, uint64_t va) {
    ++boot_nvn_pred_cpu_reads;
    uint64_t waits = boot_render_seg_waits; int recording = r->recording;
    int ok = boot_render_cpu_read(r, va, 32);
    if ((recording && !r->recording) || boot_render_seg_waits != waits) ++boot_nvn_pred_cpu_syncs;
    return ok;
}
/* Arms predication for the next draw/clear under e's condition: 1 = armed (the GPU decides), 0 = evaluate on the CPU. */
static inline int boot_nvn_pred_arm(BootRender *r, BootNvnExec *e) {
    if (!boot_nvn_pred_open(r)) { ++boot_nvn_pred_fallbacks; return 0; }
    uint64_t stores = boot_nvn_render_cond_mode() == 2 ? boot_render_store_draws : 0;
    if (!boot_nvn_pred.valid || boot_nvn_pred.addr != e->render_cond_addr || boot_nvn_pred.gen != boot_nvn_pred_gen || boot_nvn_pred.stores != stores) {
        boot_nvn_pred.valid = 0;
        if (!boot_nvn_pred_eval(r, e->render_cond_addr)) {
            static unsigned logged;
            if (++boot_nvn_pred_fallbacks, logged < 8) { ++logged; boot_draw_log("nvn native: conditional render predicate%s: counters at %llx not GPU-readable in place, CPU evaluation (%llu)", "", e->render_cond_addr, boot_nvn_pred_fallbacks); }
            return 0;
        }
        boot_nvn_pred.valid = 1; boot_nvn_pred.addr = e->render_cond_addr; boot_nvn_pred.gen = boot_nvn_pred_gen; boot_nvn_pred.stores = stores;
        ++boot_nvn_pred_evals; ++e->render_cond_evals;
    } else ++boot_nvn_pred_reused;
    boot_render_pred.buffer = boot_nvn_pred.ring; boot_render_pred.offset = (uint64_t)boot_nvn_pred.slot * 8;
    boot_render_pred.op = e->render_cond_mode == 1 ? D3D12_PREDICATION_OP_EQUAL_ZERO : D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    boot_render_pred.armed = 1;
    return 1;
}
#endif
