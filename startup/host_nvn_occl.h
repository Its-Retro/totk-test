#ifndef TOTK_HOST_NVN_OCCL_H
#define TOTK_HOST_NVN_OCCL_H
/* batch-2 (nxport-gaps N3+G3): SAMPLES_PASSED occlusion queries counted on the GPU. nvnCommandBufferReportCounter type 1
 * used to write a made-up "visible" count (host_nvn_play.h) and ResetCounter was ignored, so nothing TOTK culls by
 * occlusion was ever culled: neither by its CPU-side test of the query results (the query read at main+0x10c4d44 in 1.1.2,
 * nxport crates/engine/src/metal/mod.rs:3544-3551) nor by SetRenderEnableConditional on report pairs (nxport
 * mod.rs:2706-2735: Hyrule Field grass patches). Every boot logs "report_counter type 1"; nxport counts ~77 reports per
 * Hateno frame.
 *
 * NVN semantics (as nxport metal/mod.rs:3430-3437, MIT): one running sample counter; ResetCounter(SAMPLES_PASSED) zeroes
 * it and every ReportCounter(SAMPLES_PASSED) writes NVNcounterData {u64 count, u64 timestamp} with the samples counted
 * since the reset. Here, while counting, boot_render_draw brackets each host draw with a D3D12 OCCLUSION query (the
 * existing slots of boot_render_occl: host_gpu_render.h, host_gpu_render_draw.h). A report resolves its slots [lo, next)
 * into a DEFAULT-heap buffer of ours, a one-group compute (occl-shaders/occl_sum.hlsl, DXIL in host_nvn_occl_dxil.h) adds
 * them to the running sum kept in that buffer (or to 0 after a reset) and stages {sum, timestamp}, and a GPU copy puts the
 * 16 bytes at the report address with the region transition and GPU-write bookkeeping of boot_render_gpu_write. All of it
 * is recorded on r->cl in command order (a multi-threaded recording replays it in that order, after the chunks that ended
 * the queries): no CPU wait is added, and render-38c's predication reads the report at its place in the queue, so
 * conditional rendering on a report of the same submission is exact (nxport renders those, mod.rs:2708-2713). The
 * timestamp word is today's CPU-side value (TOTK_GPU_TS; real GPU time is gap G1).
 *
 * Counting runs from a ResetCounter to the next ReportCounter: the counter is not observable between a report and the next
 * reset, and a query per draw for the whole frame would cost CPU time for nothing. A report with no reset since the
 * previous one means the game reads the counter cumulatively: counting then never stops again (as NVN's counter, and
 * nxport's `counting`, mod.rs:3442-3443), and that report and the later ones up to the next reset keep today's value,
 * since the draws in between were not counted (logged once). TOTK_OCCLUSION=continuous counts from the first reset on.
 * The slots restart at 0 every submission (host_nvn_play.h) and hold 65536: the slots of an open interval are summed into
 * the running sum at the end of a submission and before they run out.
 *
 * Today's value ("visible", the caller's old path) is still written for a report before any reset, while the objects or
 * the running sum are missing (creation or recording failed, device recreated), at an address that is not 16 contiguous
 * bytes of one guest region, and when a draw was skipped or failed since the reset: an uncounted box would report 0 and
 * the game would cull what it guards (nxport metal/translated.rs:4479-4482 hit exactly that with targetless query boxes).
 *
 * Latency (test for pop-in): TOTK reads its queries on a worker without waiting for the GPU (nxport report_guard.rs:1-8:
 * reads came first and saw the counts of three frames earlier). Our reports land when the frame's lists execute, after
 * its 25-40 ms translation, so the CPU-side culling sees counts a few frames old: an object coming out from behind an
 * occluder can appear a few frames late. New query slots read zeros until written, as with today's value (that value is
 * written in command order too). nxport's report guard (pages unreadable until written) is the follow-up if this shows.
 *
 * TOTK_OCCLUSION=fake (or 0): today's behaviour exactly (every report "visible", ResetCounter ignored, no query recorded).
 * Every 300 presents (the frame-N.bmp cadence) an "occlusion" line: resets, reports by how they were written, queries,
 * folds, GPU-written reports and how many were 0 (read back from the sum buffer, one period late), draws recorded under
 * predication and draws/clears dropped by the render enable. */
#include "host_nvn_occl_dxil.h"

#define BOOT_NVN_OCCL_ACC    ((uint64_t)BOOT_RENDER_OCCL_QUERIES * 8)   /* u64 running sum, then u32 reports and u32 zero reports */
#define BOOT_NVN_OCCL_STAGE  (BOOT_NVN_OCCL_ACC + 256)                  /* staged NVNcounterData records, 16 bytes each, round robin */
#define BOOT_NVN_OCCL_STAGES 64u
#define BOOT_NVN_OCCL_BYTES  (BOOT_NVN_OCCL_STAGE + (uint64_t)BOOT_NVN_OCCL_STAGES * 16)
static struct {
    ID3D12Device *device; ID3D12RootSignature *root; ID3D12PipelineState *pso; ID3D12Resource *buf, *rb; int failed;
    int ever_reset, counting, reported, reset_pending, continuous, lost, uncertain;
    uint32_t lo, stage;                 /* first slot not summed yet; next staging record */
    uint64_t fail_mark;                 /* skipped + failed draws at the reset (a change: a box may be missing) */
    uint64_t rb_fence; int rb_due, rb_valid; uint64_t continuous_from;
    uint64_t resets, reports, gpu_reports, old_unset, old_lost, old_failed, old_region, folds, cap_folds, slots, record_failures;
} boot_nvn_occl;
/* 0 fake (today), 1 counted on the GPU from each reset to its report (default), 2 counted from the first reset on */
static inline int boot_nvn_occl_mode(void) {
    static int v = -1;
    if (v < 0) { char s[16]; DWORD n = GetEnvironmentVariableA("TOTK_OCCLUSION", s, sizeof s); int ok = n && n < sizeof s;
                 v = ok && (!_stricmp(s, "fake") || !strcmp(s, "0")) ? 0 : ok && !_stricmp(s, "continuous") ? 2 : 1;
                 boot_draw_log("nvn native: occlusion queries %s (TOTK_OCCLUSION mode %llu, 2 = continuous) %llu", v ? "counted on the GPU" : "reported visible (fake)", (uint64_t)v, 0); }
    return v;
}
static inline void boot_nvn_occl_release(void) {
    if (boot_nvn_occl.pso) ID3D12PipelineState_Release(boot_nvn_occl.pso);
    if (boot_nvn_occl.root) ID3D12RootSignature_Release(boot_nvn_occl.root);
    if (boot_nvn_occl.buf) ID3D12Resource_Release(boot_nvn_occl.buf);
    if (boot_nvn_occl.rb) ID3D12Resource_Release(boot_nvn_occl.rb);
    boot_nvn_occl.pso = NULL; boot_nvn_occl.root = NULL; boot_nvn_occl.buf = NULL; boot_nvn_occl.rb = NULL;
    boot_nvn_occl.device = NULL; boot_nvn_occl.failed = 0; boot_nvn_occl.rb_due = boot_nvn_occl.rb_valid = 0;
}
/* Root signature (8 root constants b0, root UAV u0), pipeline, sum buffer and stats readback of the render object's device;
 * once per device (a recreated device gets new ones, and the running sum is lost until the next reset). */
static inline int boot_nvn_occl_open(BootRender *r) {
    ID3D12Device *dev = r->gpu->device;
    if (boot_nvn_occl.device == dev) return !boot_nvn_occl.failed;
    if (boot_nvn_occl.device) { boot_nvn_occl_release(); boot_nvn_occl.lost = 1; }   /* objects of another (removed) device */
    boot_nvn_occl.device = dev; boot_nvn_occl.failed = 1;
    D3D12_ROOT_PARAMETER1 p[2]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 8;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor.ShaderRegister = 0; p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    for (unsigned i = 0; i < 2; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 2; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) {
        if (err) ID3D10Blob_Release(err);
        boot_draw_log("nvn native: occlusion queries%s: root signature serialization failed, reports stay visible %llu %llu", "", 0, 0); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&boot_nvn_occl.root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(boot_nvn_occl.root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { boot_nvn_occl.root = NULL; boot_draw_log("nvn native: occlusion queries%s: root signature hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd; memset(&cd, 0, sizeof cd); cd.pRootSignature = boot_nvn_occl.root;
    cd.CS.pShaderBytecode = boot_nvn_occl_sum_dxil; cd.CS.BytecodeLength = sizeof boot_nvn_occl_sum_dxil;
    if (FAILED(hr = boot_psoc_create_compute(r, dev, &cd, &boot_nvn_occl.pso))) {
        boot_nvn_occl.pso = NULL; boot_draw_log("nvn native: occlusion queries%s: pipeline hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = BOOT_NVN_OCCL_BYTES; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&boot_nvn_occl.buf))) {
        boot_nvn_occl.buf = NULL; boot_draw_log("nvn native: occlusion queries%s: sum buffer hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    ++boot_render_created_buffers;
    boot_nvn_occl.rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST, NULL);   /* the counters, for the log only */
    boot_nvn_occl.failed = 0;
    boot_draw_log("nvn native: occlusion queries%s ready: sum buffer %llu bytes, DXIL %llu bytes", "", (uint64_t)BOOT_NVN_OCCL_BYTES, (uint64_t)sizeof boot_nvn_occl_sum_dxil);
    return 1;
}
static inline void boot_nvn_occl_transition(D3D12_RESOURCE_BARRIER *b, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    memset(b, 0, sizeof *b); b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b->Transition.pResource = res;
    b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b->Transition.StateBefore = from; b->Transition.StateAfter = to;
}
/* Records the sum of slots [lo, boot_render_occl.next): their resolve into the sum buffer, the dispatch (flags: 1 start from
 * 0, 2 report) and, for a report (reg >= 0), the copy of the staged 16 bytes to region `reg` + `off`, as boot_render_gpu_write
 * does (COMMON -> COPY_DEST once per region until boot_render_gw_settle; the range joins the GPU-write list, so CPU readers
 * wait for it). The sum buffer is COMMON outside this sequence: a list starts with it decayed to COMMON and every sequence
 * puts it back. 0 = nothing recorded (no open list). */
static inline int boot_nvn_occl_record(BootRender *r, uint32_t flags, uint64_t ticks, int reg, uint64_t off, uint64_t va) {
    if (!r->recording && !boot_render_begin(r)) return 0;
    ID3D12GraphicsCommandList *cl = r->cl; ID3D12Resource *b = boot_nvn_occl.buf;
    uint32_t lo = boot_nvn_occl.lo, hi = boot_render_occl.next, n = hi > lo ? hi - lo : 0;
    D3D12_RESOURCE_BARRIER t[2];
    if (n) {
        boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        BOOT_GP_RB("occlusion sum", cl, 1, t);
        BOOT_GP_PRE(cl);
        ID3D12GraphicsCommandList_ResolveQueryData(cl, boot_render_occl.qh, D3D12_QUERY_TYPE_OCCLUSION, lo, n, b, (UINT64)lo * 8);
        BOOT_GP_POST(BOOT_GP_OTHER, "occlusion resolve", cl, 0, 0);
    }
    boot_nvn_occl_transition(&t[0], b, n ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    BOOT_GP_RB("occlusion sum", cl, 1, t);
    uint32_t stage = (uint32_t)(BOOT_NVN_OCCL_STAGE + (uint64_t)(boot_nvn_occl.stage++ % BOOT_NVN_OCCL_STAGES) * 16);
    uint32_t k[8] = {lo, n, flags, stage, (uint32_t)ticks, (uint32_t)(ticks >> 32), (uint32_t)BOOT_NVN_OCCL_ACC, 0};
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, boot_nvn_occl.root);
    ID3D12GraphicsCommandList_SetPipelineState(cl, boot_nvn_occl.pso); ++boot_render_bind_epoch;   /* replaced the graphics pipeline */
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 8, k, 0);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(b));
    boot_dred_work(cl, BOOT_DRED_DISPATCH, BOOT_DRED_COMPUTE, NULL, NULL, va, boot_render_exec_id, boot_render_watch_event);
    BOOT_GP_DISPATCH("occlusion sum", 0, cl, 1, 1, 1);
    if (reg < 0) {
        boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        BOOT_GP_RB("occlusion sum", cl, 1, t);
    } else {
        unsigned nb = 1, i = 0;
        boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        while (i < boot_render_gw_dest_count && boot_render_gw_dest[i] != r->region[reg]) ++i;
        if (i == boot_render_gw_dest_count) {
            boot_nvn_occl_transition(&t[nb++], r->region[reg], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            boot_render_gw_dest[boot_render_gw_dest_count++] = r->region[reg]; ++boot_render_gw_transitions;
        }
        BOOT_GP_RB("occlusion report", cl, nb, t);
        BOOT_GP_CBR("occlusion report", cl, r->region[reg], off, b, stage, 16);
        boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        BOOT_GP_RB("occlusion report", cl, 1, t);
        boot_render_gmem_pending = 1;
        if (boot_render_gw_count && !boot_render_gw[boot_render_gw_count - 1].fence && boot_render_gw[boot_render_gw_count - 1].va <= va + 16 &&
            va <= boot_render_gw[boot_render_gw_count - 1].va + boot_render_gw[boot_render_gw_count - 1].size) {   /* extends the previous write */
            uint64_t a = boot_render_gw[boot_render_gw_count - 1].va < va ? boot_render_gw[boot_render_gw_count - 1].va : va;
            uint64_t z = boot_render_gw[boot_render_gw_count - 1].va + boot_render_gw[boot_render_gw_count - 1].size; if (va + 16 > z) z = va + 16;
            boot_render_gw[boot_render_gw_count - 1].va = a; boot_render_gw[boot_render_gw_count - 1].size = z - a;
        } else if (boot_render_gw_count < BOOT_RENDER_GW_RANGES) { boot_render_gw[boot_render_gw_count].va = va; boot_render_gw[boot_render_gw_count].size = 16; boot_render_gw[boot_render_gw_count++].fence = 0; }
        else boot_render_gw_all = 1;   /* CPU readers of these ranges wait (boot_render_cpu_read) */
        ++boot_render_gpu_writes; boot_render_gpu_write_bytes += 16;
    }
    boot_nvn_occl.lo = hi; boot_nvn_occl.slots += n;
    return 1;
}
/* Sums the open interval's slots into the running sum (no report). 0 = failed: the count is lost until the next reset. */
static inline int boot_nvn_occl_fold(BootRender *r) {
    if (boot_nvn_occl_open(r) && boot_nvn_occl_record(r, boot_nvn_occl.reset_pending ? 1u : 0u, 0, -1, 0, 0)) { boot_nvn_occl.reset_pending = 0; return 1; }
    boot_nvn_occl.lost = 1; ++boot_nvn_occl.record_failures; return 0;
}
/* nvnCommandBufferResetCounter(SAMPLES_PASSED). `fail_mark`: the executor's skipped + failed draws now. */
static inline void boot_nvn_occl_reset(BootRender *r, uint64_t fail_mark) {
    if (!boot_nvn_occl_mode()) {
        static int logged; if (!logged) { logged = 1; boot_draw_log("nvn native: ignored command %s %llu %llu", "nvnCommandBufferResetCounter", 0, 0); }   /* as before */
        return;
    }
    ++boot_nvn_occl.resets; boot_nvn_occl.ever_reset = 1; boot_nvn_occl.reported = 0; boot_nvn_occl.uncertain = 0;
    boot_nvn_occl.reset_pending = 1; boot_nvn_occl.fail_mark = fail_mark;
    if (boot_nvn_occl_mode() == 2) boot_nvn_occl.continuous = 1;
    boot_nvn_occl.lo = boot_render_occl.next;   /* draws counted before the reset are not part of the new count */
    int ok = boot_render_occl.qh && boot_render_occl.rb && boot_nvn_occl_open(r);   /* boot_render_draw fails a counted draw without both */
    boot_nvn_occl.lost = !ok; boot_nvn_occl.counting = ok;
}
/* Per host draw (host_nvn_play.h boot_nvn_exec_draw): 1 = count this draw's samples (boot_render_occl.want). Never 1 when
 * boot_render_draw would fail the draw for it (no query heap or readback buffer, slots full). */
static inline int boot_nvn_occl_want(BootRender *r) {
    if (!boot_nvn_occl.counting || !boot_render_occl.qh || !boot_render_occl.rb) return 0;
    if (boot_render_occl.next >= BOOT_RENDER_OCCL_QUERIES - 1) {   /* slots run out: sum them, then restart at 0 */
        if (!boot_nvn_occl_fold(r)) return 0;
        ++boot_nvn_occl.cap_folds; boot_render_occl.next = 0; boot_nvn_occl.lo = 0;
    }
    return 1;
}
/* nvnCommandBufferReportCounter(SAMPLES_PASSED, va): 1 = recorded on the GPU (the caller forgets its CPU shadow of va),
 * 0 = the caller writes today's value. `ticks`: the timestamp word the caller would write. */
static inline int boot_nvn_occl_report(BootRender *r, uint64_t va, uint64_t ticks, uint64_t fail_mark) {
    if (!boot_nvn_occl_mode()) return 0;
    ++boot_nvn_occl.reports;
    if (!boot_nvn_occl.ever_reset) { ++boot_nvn_occl.old_unset; return 0; }   /* no count to report yet */
    if (boot_nvn_occl.reported && !boot_nvn_occl.continuous && !boot_nvn_occl.lost) {   /* read cumulatively: count from now on, exact from the next reset */
        boot_nvn_occl.continuous = 1; boot_nvn_occl.lost = 1; boot_nvn_occl.counting = !!boot_render_occl.qh;
        boot_draw_log("nvn native: occlusion queries%s: a report without a reset since the last one; counting continuously from report %llu (resets %llu)", "", boot_nvn_occl.reports, boot_nvn_occl.resets);
    }
    if (fail_mark != boot_nvn_occl.fail_mark) boot_nvn_occl.uncertain = 1;
    boot_nvn_occl.reported = 1;
    int written = 0, reg = -1; uint64_t off = 0;
    int ready = boot_render_occl.qh && boot_nvn_occl_open(r);   /* first: a recreated device marks the sum lost here */
    if (!ready || boot_nvn_occl.lost) { boot_nvn_occl.lost = 1; ++boot_nvn_occl.old_lost; }
    else if (boot_nvn_occl.uncertain) { ++boot_nvn_occl.old_failed; boot_nvn_occl_fold(r); }   /* the running sum stays exact for later reports */
    else if (boot_render_region_span(r, va, 16, &reg, &off) < 16) { ++boot_nvn_occl.old_region; boot_nvn_occl_fold(r); }
    else if (boot_nvn_occl_record(r, (boot_nvn_occl.reset_pending ? 1u : 0u) | 2u, ticks, reg, off, va)) { boot_nvn_occl.reset_pending = 0; ++boot_nvn_occl.gpu_reports; written = 1; }
    else { boot_nvn_occl.lost = 1; ++boot_nvn_occl.record_failures; }
    if (!boot_nvn_occl.continuous) boot_nvn_occl.counting = 0;   /* nothing reads the counter again before the next reset */
    else if (!boot_nvn_occl.continuous_from) boot_nvn_occl.continuous_from = boot_nvn_occl.reports;
    return written;
}
/* Submission end, before its kick: the open interval's slots are summed (the slots restart at 0 next submission), and the
 * counters for the log are copied out when the present log asked for them. */
static inline void boot_nvn_occl_submit_end(BootRender *r) {
    if (!boot_nvn_occl_mode()) return;
    if (boot_nvn_occl.counting && !boot_nvn_occl.lost && boot_render_occl.next > boot_nvn_occl.lo && boot_nvn_occl_fold(r)) ++boot_nvn_occl.folds;
    if (boot_nvn_occl.rb_due && r->recording && boot_nvn_occl.rb && boot_nvn_occl.buf && boot_nvn_occl.device == r->gpu->device) {
        D3D12_RESOURCE_BARRIER t[1];
        boot_nvn_occl_transition(&t[0], boot_nvn_occl.buf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        BOOT_GP_RB("occlusion stats", r->cl, 1, t);
        BOOT_GP_CBR("occlusion stats", r->cl, boot_nvn_occl.rb, 0, boot_nvn_occl.buf, BOOT_NVN_OCCL_ACC, 16);
        boot_nvn_occl_transition(&t[0], boot_nvn_occl.buf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        BOOT_GP_RB("occlusion stats", r->cl, 1, t);
        boot_nvn_occl.rb_fence = r->gpu->submitted + 1; boot_nvn_occl.rb_due = 0; boot_nvn_occl.rb_valid = 1;   /* the open list's fence */
    }
}
/* Submission start, before the slots restart at 0: slots still not summed (a failed submission) make the count incomplete. */
static inline void boot_nvn_occl_submit_begin(void) {
    if (boot_nvn_occl.counting && boot_render_occl.next > boot_nvn_occl.lo) boot_nvn_occl.lost = 1;
    boot_nvn_occl.lo = 0;
}
/* Every 300 presents: the occlusion line (deltas since the previous one). */
static inline void boot_nvn_occl_present(BootRender *r, uint64_t present) {
    static uint64_t last = UINT64_MAX, l_resets, l_reports, l_gpu, l_unset, l_lost, l_failed, l_region, l_folds, l_cap, l_slots, l_recf, l_pred, l_off, l_rbn, l_rbz;
    if (!present || present % 300 || present == last || !boot_nvn_occl_mode()) return;
    uint64_t presents = last == UINT64_MAX ? present : present - last; last = present;
    BootNvnExec *e = boot_nvn_exec();
    uint64_t rbn = l_rbn, rbz = l_rbz; const char *rb = "not read yet";
    if (boot_nvn_occl.rb_valid && boot_nvn_occl.rb && boot_nvn_occl.device == r->gpu->device) {
        if (ID3D12Fence_GetCompletedValue(r->gpu->fence) >= boot_nvn_occl.rb_fence) {
            uint8_t *p = NULL; D3D12_RANGE rr = {0, 16};
            if (SUCCEEDED(ID3D12Resource_Map(boot_nvn_occl.rb, 0, &rr, (void **)&p))) {
                uint32_t w[2]; memcpy(w, p + 8, 8); rbn = w[0]; rbz = w[1]; rb = "read back";
                D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(boot_nvn_occl.rb, 0, &none);
            }
        } else rb = "copy pending";
    }
    boot_nvn_occl.rb_due = 1;   /* the next submission end copies the counters out again */
    char line[1100];
    snprintf(line, sizeof line, " (TOTK_OCCLUSION %s%s) over %llu presents: resets %llu, reports %llu: on the GPU %llu, today's value %llu (before any reset %llu, "
             "count incomplete %llu, draw skipped or failed %llu, not in one region %llu); queries %llu (%.1f per present), folds %llu (slots full %llu), "
             "record failures %llu; written on the GPU up to the previous line (%s): %llu reports, %llu of them 0 samples (+%llu / +%llu); draws recorded under predication +%llu, draws/clears dropped by the render enable +%llu;",
             boot_nvn_occl_mode() == 2 ? "continuous" : "gpu", boot_nvn_occl.continuous ? ", counting continuously" : "", (unsigned long long)presents,
             (unsigned long long)(boot_nvn_occl.resets - l_resets), (unsigned long long)(boot_nvn_occl.reports - l_reports), (unsigned long long)(boot_nvn_occl.gpu_reports - l_gpu),
             (unsigned long long)((boot_nvn_occl.old_unset - l_unset) + (boot_nvn_occl.old_lost - l_lost) + (boot_nvn_occl.old_failed - l_failed) + (boot_nvn_occl.old_region - l_region)),
             (unsigned long long)(boot_nvn_occl.old_unset - l_unset), (unsigned long long)(boot_nvn_occl.old_lost - l_lost), (unsigned long long)(boot_nvn_occl.old_failed - l_failed),
             (unsigned long long)(boot_nvn_occl.old_region - l_region), (unsigned long long)(boot_nvn_occl.slots - l_slots),
             presents ? (double)(boot_nvn_occl.slots - l_slots) / (double)presents : 0.0, (unsigned long long)(boot_nvn_occl.folds - l_folds),
             (unsigned long long)(boot_nvn_occl.cap_folds - l_cap), (unsigned long long)(boot_nvn_occl.record_failures - l_recf), rb, (unsigned long long)rbn, (unsigned long long)rbz,
             (unsigned long long)(rbn - l_rbn), (unsigned long long)(rbz - l_rbz), (unsigned long long)(boot_render_pred.predicated - l_pred),
             (unsigned long long)(e ? e->render_off - l_off : 0));
    boot_draw_log("nvn native: occlusion%s present %llu, continuous from report %llu", line, present, boot_nvn_occl.continuous_from);
    l_resets = boot_nvn_occl.resets; l_reports = boot_nvn_occl.reports; l_gpu = boot_nvn_occl.gpu_reports; l_unset = boot_nvn_occl.old_unset; l_lost = boot_nvn_occl.old_lost;
    l_failed = boot_nvn_occl.old_failed; l_region = boot_nvn_occl.old_region; l_folds = boot_nvn_occl.folds; l_cap = boot_nvn_occl.cap_folds; l_slots = boot_nvn_occl.slots;
    l_recf = boot_nvn_occl.record_failures; l_pred = boot_render_pred.predicated; l_off = e ? e->render_off : 0; l_rbn = rbn; l_rbz = rbz;
}

/* batch-2 (nxport-gaps N9 / G13, measurement only): the texture verifier re-hashes up to TOTK_TEX_VERIFY_MB of built
 * texture slots on the render thread at every submission (host_nvn_play.h, boot_render_tex_verify in host_gpu_render.h),
 * and its per-frame cost was never logged: the hitch lines only show it inside submissions over 200 ms. Every 300 presents
 * a "texture re-hash" line gives the bytes verified, the time (the hitch line's "re-hash": submission start to the start of
 * translation, i.e. the CPU texel drain plus the verifier), the stale rebuilds it found and how long one full round-robin
 * pass over the eligible slots takes in presents (how late a texture the CPU rewrote can be seen: G13). nxport re-checks
 * CPU-writable textures once per frame within 64 MiB, compressed ones by a sparse sample (metal/mod.rs:1799-1850). */
static uint64_t boot_nvn_rehash_qpc, boot_nvn_rehash_max, boot_nvn_rehash_submits;
static inline void boot_nvn_rehash_note(uint64_t qpc) {
    boot_nvn_rehash_qpc += qpc; ++boot_nvn_rehash_submits; if (qpc > boot_nvn_rehash_max) boot_nvn_rehash_max = qpc;
}
static inline void boot_nvn_rehash_present(uint64_t present) {
    static uint64_t last = UINT64_MAX, l_bytes, l_qpc, l_sub, l_stale; static long long budget_mb = -1;
    if (!present || present % 300 || present == last) return;
    uint64_t presents = last == UINT64_MAX ? present : present - last; last = present;
    if (budget_mb < 0) { char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_VERIFY_MB", v, sizeof v); budget_mb = (n && n < sizeof v) ? (long long)strtoull(v, NULL, 10) : 64; }
    uint64_t eligible = 0, slots = 0;
    for (uint32_t i = 0; boot_render_tex_slots && i < boot_render_tex_high; ++i) {   /* the verifier's own test, without its "recently checked" skip */
        const BootRenderTexSlot *s = &boot_render_tex_slots[i];
        if (s->valid && s->size && !s->view_of && !boot_render_tex_poisoned[i]) { eligible += s->size; ++slots; }
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    double ms = (double)(boot_nvn_rehash_qpc - l_qpc) * 1000.0 / (double)f.QuadPart, max_ms = (double)boot_nvn_rehash_max * 1000.0 / (double)f.QuadPart;
    double mib = (double)(boot_render_tex_verified_bytes - l_bytes) / 1048576.0, per = presents ? mib / (double)presents : 0.0;
    char line[640];
    snprintf(line, sizeof line, " (TOTK_TEX_VERIFY_MB %lld, parallel helpers %u) over %llu presents, %llu submissions: verified %.1f MiB (%.1f per present), "
             "re-hash %.1f ms (%.2f ms per present, max %.2f ms in one submission), stale rebuilds %llu; eligible slots %llu, %.1f MiB: one full pass per %.1f presents;",
             budget_mb, boot_render_par_helpers(), (unsigned long long)presents, (unsigned long long)(boot_nvn_rehash_submits - l_sub), mib, per, ms,
             presents ? ms / (double)presents : 0.0, max_ms, (unsigned long long)(boot_render_tex_stale_rebuilds - l_stale), (unsigned long long)slots,
             (double)eligible / 1048576.0, per > 0.0 ? (double)eligible / 1048576.0 / per : 0.0);
    boot_draw_log("nvn native: texture re-hash%s present %llu %llu", line, present, 0);
    l_bytes = boot_render_tex_verified_bytes; l_qpc = boot_nvn_rehash_qpc; l_sub = boot_nvn_rehash_submits; l_stale = boot_render_tex_stale_rebuilds; boot_nvn_rehash_max = 0;
}
#endif
