#ifndef TOTK_HOST_NVN_GPU_TS_H
#define TOTK_HOST_NVN_GPU_TS_H
/* G1 (nxport-gaps, decided-gaps.json): TIMESTAMP reports with the GPU's time, opt-in with TOTK_GPU_TS=gpu.
 *
 * Today (host_nvn_play.h, report_counter) a TIMESTAMP report carries the host clock at the moment t2 translates the
 * command, in GPU ticks. TOTK measures its per-frame GPU span from a start / end pair of TIMESTAMP reports read two frames
 * later (nxport crates/engine/src/metal/mod.rs:3928-3935, MIT) and steers its dynamic resolution with it, so the span it
 * sees is t2's translation time (25-40 ms), not the GPU's (9-15 ms in play-94), and most 3D targets render at 1024x576.
 * With TOTK_GPU_TS=gpu the GPU writes a TIMESTAMP report in command order: a D3D12 TIMESTAMP query at the report's place
 * in the list (written once the GPU has finished the work before it, bottom of pipe, as NVN's TIMESTAMP) is resolved into
 * a DEFAULT buffer of ours, a one-thread compute (gpu-ts-shaders/gpu_ts.hlsl, DXIL in host_nvn_gpu_ts_dxil.h) converts it
 * and stages NVNcounterData {0, ticks}, and a GPU copy puts the 16 bytes at the report address with the region transition
 * and GPU-write bookkeeping of boot_render_gpu_write, as the occlusion reports do (host_nvn_occl.h). No CPU wait and no
 * CPU-side rewrite: the guest sees the GPU's time exactly where it saw the record-time one, early fence included. The
 * record-time write is not recorded for such a report (two copies to the same bytes have no defined order without a
 * barrier between them).
 *
 * Semantics as nxport --gpu-timestamps (metal/mod.rs:2994-3003, 3727-3733, 3937-3967): only TIMESTAMP (0) reports carry
 * the GPU's time; TIMESTAMP_TOP (0x10) keeps counter 0 and today's timestamp (TOTK reports it next to SAMPLES_PASSED for
 * the render-enable conditions, which compare only the counter words). TOTK_GPU_TS=gpu-top stamps TIMESTAMP_TOP reports
 * too, with the same bottom-of-pipe time (an A/B, in case TOTK pairs a TOP report with a bottom one). The counter word
 * stays 0, as today (Maxwell's TIMESTAMP report; nxport writes the time there as well, nvnDeviceGetTimestampInNanoseconds
 * reads only +8). Units differ from nxport on purpose: nxport replaces nvnDeviceGetTimestampInNanoseconds and writes
 * nanoseconds at both ends; we keep the game's own conversion, so the word stays in GPU timer ticks, 614.4 MHz (32 x the
 * 19.2 MHz system tick, ns * 384/625), on the host QPC timeline as today's value: the D3D12 timestamp is placed on the QPC
 * timeline with ID3D12CommandQueue::GetClockCalibration (sampled every 50 ms on the CPU, passed as root constants) and
 * scaled by 614.4 MHz / GetTimestampFrequency in 32.32 fixed point (exact to a tick; checked against 128-bit math in
 * play-work/p96/host-a/harness/gts_math_test.c). The GPU's values are strictly increasing (+1 tick), as today's are.
 *
 * Today's value instead (counted as fallbacks): a dilated guest clock (TOTK_GUEST_CLOCK not "wall": today's value is
 * then the guest clock, not QPC), objects missing or their creation failed, a report that is not 16 contiguous bytes of
 * one guest region, no list to record into.
 *
 * Caveat for the A/B: a TOTK frame is several of our lists (9 in play-94), each kicked when t2 has translated its
 * submission. When a frame's start and end reports are in different lists the GPU idles between them while t2
 * translates, and real timestamps include that idle (as on the Switch, where the submit is a cheap push).
 *
 * Once a second while the mode is on, a "gpu timestamps" line in draw-path.log: frames, reports the GPU wrote and
 * fallbacks; the span from the first to the last TIMESTAMP report of each frame (present to present) as the record-time
 * value had it and as the GPU wrote it, and how much later the GPU's stamp is (the staged records are copied out once a
 * second and read a second later, never waited for); and the render-target sizes drawn in that second, depth and colour,
 * largest first: the resolution TOTK's dynamic resolution picked.
 *
 * TOTK_GPU_TS unset or any other value (ns, fast, ...): nothing here runs and the report path is today's exactly. */
#include "host_nvn_gpu_ts_dxil.h"

#define BOOT_GTS_QUERIES 64u                                             /* TIMESTAMP query slots, round robin */
#define BOOT_GTS_RECORDS 4096u                                           /* staged 16-byte records, round robin (read back for the log) */
#define BOOT_GTS_LAST    ((uint64_t)BOOT_GTS_QUERIES * 8)                /* u64: the last value the GPU wrote */
#define BOOT_GTS_STAGE   (BOOT_GTS_LAST + 256)
#define BOOT_GTS_BYTES   (BOOT_GTS_STAGE + (uint64_t)BOOT_GTS_RECORDS * 16)
static struct {
    ID3D12Device *device; ID3D12RootSignature *root; ID3D12PipelineState *pso; ID3D12QueryHeap *heap; ID3D12Resource *buf, *rb; int failed;
    uint64_t freq, qpf, mul, cal_qpc, cal_gpu, cal_base;   /* timestamp frequency, QPC frequency, 614.4 MHz / freq (32.32), calibration */
    uint64_t seq, frame;                                   /* records staged so far (record = seq % BOOT_GTS_RECORDS); presents */
    uint64_t prov[BOOT_GTS_RECORDS]; uint32_t frame_of[BOOT_GTS_RECORDS]; uint8_t type_of[BOOT_GTS_RECORDS];   /* per record: today's value, frame, type */
    uint64_t rb_list, rb_seq; int rb_due, rb_valid;        /* the copy of the records to `rb`: its list, seq at the copy */
    int64_t last_log; uint64_t log_frame, rt_clock, written[2], fallbacks, dilated;   /* print window */
} boot_gts;
/* 0 off (today), 1 TIMESTAMP reports written by the GPU (TOTK_GPU_TS=gpu), 2 TIMESTAMP_TOP too (TOTK_GPU_TS=gpu-top) */
static inline int boot_gts_mode(void) {
    static int v = -1;
    if (v < 0) { char s[16]; DWORD n = GetEnvironmentVariableA("TOTK_GPU_TS", s, sizeof s); int ok = n && n < sizeof s;
                 v = ok && !strcmp(s, "gpu") ? 1 : ok && !strcmp(s, "gpu-top") ? 2 : 0;
                 if (v) boot_draw_log("nvn native: gpu timestamps%s (G1, TOTK_GPU_TS mode %llu, 2 = TIMESTAMP_TOP too) %llu", ": TIMESTAMP reports written by the GPU", (uint64_t)v, 0); }
    return v;
}
static inline void boot_gts_release(void) {
    if (boot_gts.pso) ID3D12PipelineState_Release(boot_gts.pso);
    if (boot_gts.root) ID3D12RootSignature_Release(boot_gts.root);
    if (boot_gts.heap) ID3D12QueryHeap_Release(boot_gts.heap);
    if (boot_gts.buf) ID3D12Resource_Release(boot_gts.buf);
    if (boot_gts.rb) ID3D12Resource_Release(boot_gts.rb);
    boot_gts.pso = NULL; boot_gts.root = NULL; boot_gts.heap = NULL; boot_gts.buf = NULL; boot_gts.rb = NULL;
    boot_gts.device = NULL; boot_gts.failed = 0; boot_gts.rb_due = boot_gts.rb_valid = 0; boot_gts.cal_qpc = 0;
}
/* Root signature (12 root constants b0, root UAV u0), pipeline, query heap, buffer and log readback of the render object's
 * device; once per device (a recreated device gets new ones). */
static inline int boot_gts_open(BootRender *r) {
    ID3D12Device *dev = r->gpu->device;
    if (boot_gts.device == dev) return !boot_gts.failed;
    if (boot_gts.device) boot_gts_release();   /* objects of another (removed) device */
    boot_gts.device = dev; boot_gts.failed = 1;
    LARGE_INTEGER qf; uint64_t freq = 0;
    if (!QueryPerformanceFrequency(&qf) || qf.QuadPart <= 0 || FAILED(ID3D12CommandQueue_GetTimestampFrequency(r->gpu->queue, &freq)) || !freq ||
        ((((unsigned __int128)614400000u << 32) / freq) >> 63)) {
        boot_draw_log("nvn native: gpu timestamps%s: no usable timestamp frequency, reports keep today's value %llu %llu", "", freq, 0); return 0; }
    boot_gts.freq = freq; boot_gts.qpf = (uint64_t)qf.QuadPart; boot_gts.mul = (uint64_t)(((unsigned __int128)614400000u << 32) / freq);
    D3D12_ROOT_PARAMETER1 p[2]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 12;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor.ShaderRegister = 0; p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    for (unsigned i = 0; i < 2; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 2; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) {
        if (err) ID3D10Blob_Release(err);
        boot_draw_log("nvn native: gpu timestamps%s: root signature serialization failed, reports keep today's value %llu %llu", "", 0, 0); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&boot_gts.root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(boot_gts.root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { boot_gts.root = NULL; boot_draw_log("nvn native: gpu timestamps%s: root signature hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd; memset(&cd, 0, sizeof cd); cd.pRootSignature = boot_gts.root;
    cd.CS.pShaderBytecode = boot_nvn_gpu_ts_dxil; cd.CS.BytecodeLength = sizeof boot_nvn_gpu_ts_dxil;
    if (FAILED(hr = boot_psoc_create_compute(r, dev, &cd, &boot_gts.pso))) {
        boot_gts.pso = NULL; boot_draw_log("nvn native: gpu timestamps%s: pipeline hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_QUERY_HEAP_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = BOOT_GTS_QUERIES;
    if (FAILED(hr = ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&boot_gts.heap))) {
        boot_gts.heap = NULL; boot_draw_log("nvn native: gpu timestamps%s: query heap hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = BOOT_GTS_BYTES; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&boot_gts.buf))) {
        boot_gts.buf = NULL; boot_draw_log("nvn native: gpu timestamps%s: buffer hr %llx %llu", "", (uint64_t)(uint32_t)hr, 0); return 0; }
    ++boot_render_created_buffers;   /* committed: zero-filled, so the first "last" value is 0 */
    boot_gts.rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, (uint64_t)BOOT_GTS_RECORDS * 16, D3D12_RESOURCE_STATE_COPY_DEST, NULL);   /* for the log only */
    boot_gts.failed = 0;
    boot_draw_log("nvn native: gpu timestamps%s ready: timestamp frequency %llu Hz, DXIL %llu bytes", "", freq, (uint64_t)sizeof boot_nvn_gpu_ts_dxil);
    return 1;
}
/* Calibration (D3D12 timestamp -> QPC) at most every 50 ms; 0 = none yet. */
static inline int boot_gts_calibrate(BootRender *r) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (boot_gts.cal_qpc && (uint64_t)now.QuadPart - boot_gts.cal_qpc < boot_gts.qpf / 20) return 1;
    uint64_t gpu = 0, cpu = 0;
    if (FAILED(ID3D12CommandQueue_GetClockCalibration(r->gpu->queue, &gpu, &cpu))) return boot_gts.cal_qpc != 0;   /* keep the previous one */
    uint64_t ns = (uint64_t)((unsigned __int128)cpu * 1000000000u / boot_gts.qpf);   /* as boot_gpu_sync_host_ns, then ns * 384/625 */
    boot_gts.cal_gpu = gpu; boot_gts.cal_base = (uint64_t)((unsigned __int128)ns * 384 / 625); boot_gts.cal_qpc = (uint64_t)now.QuadPart;
    return 1;
}
/* A 16-byte range written by the GPU in the open list: joins the GPU-write list, so CPU readers wait for it (as
 * boot_render_gpu_write and boot_nvn_occl_record). */
static inline void boot_gts_gw_note(uint64_t va) {
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
/* nvnCommandBufferReportCounter TIMESTAMP (type 0; TIMESTAMP_TOP 0x10 with gpu-top) at `va`: 1 = recorded, the GPU writes
 * {0, its time} there in command order (the counter word is the 0 the caller's shadow holds); 0 = the caller writes today's
 * value. `prov`: today's value (for the log). */
static inline int boot_gts_report(BootRender *r, uint64_t va, uint32_t type, uint64_t prov) {
    if (boot_guest_dilated()) {
        if (!boot_gts.dilated++) boot_draw_log("nvn native: gpu timestamps%s: the guest clock is dilated (TOTK_GUEST_CLOCK not wall), reports keep today's guest-clock value %llu %llu", "", 0, 0);
        return 0;
    }
    int reg = -1; uint64_t off = 0;
    if (!boot_gts_open(r) || !boot_gts_calibrate(r) || boot_render_region_span(r, va, 16, &reg, &off) < 16 || (!r->recording && !boot_render_begin(r))) {
        ++boot_gts.fallbacks; return 0; }
    ID3D12GraphicsCommandList *cl = r->cl; ID3D12Resource *b = boot_gts.buf;
    uint64_t s = boot_gts.seq++; uint32_t q = (uint32_t)(s % BOOT_GTS_QUERIES), rec = (uint32_t)(s % BOOT_GTS_RECORDS);
    uint32_t stage = (uint32_t)(BOOT_GTS_STAGE + (uint64_t)rec * 16);
    D3D12_RESOURCE_BARRIER t[2];
    BOOT_GP_PRE(cl);
    ID3D12GraphicsCommandList_EndQuery(cl, boot_gts.heap, D3D12_QUERY_TYPE_TIMESTAMP, q);   /* the report's place: after the work before it */
    boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    BOOT_GP_RB("gpu timestamp", cl, 1, t);
    BOOT_GP_PRE(cl);
    ID3D12GraphicsCommandList_ResolveQueryData(cl, boot_gts.heap, D3D12_QUERY_TYPE_TIMESTAMP, q, 1, b, (UINT64)q * 8);
    BOOT_GP_POST(BOOT_GP_OTHER, "gpu timestamp resolve", cl, 0, 0);
    boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    BOOT_GP_RB("gpu timestamp", cl, 1, t);
    uint32_t k[12] = {q * 8, stage, (uint32_t)boot_gts.cal_base, (uint32_t)(boot_gts.cal_base >> 32), (uint32_t)boot_gts.cal_gpu, (uint32_t)(boot_gts.cal_gpu >> 32),
                      (uint32_t)(boot_gts.mul >> 32), (uint32_t)boot_gts.mul, (uint32_t)BOOT_GTS_LAST, 0, 0, 0};
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, boot_gts.root);
    ID3D12GraphicsCommandList_SetPipelineState(cl, boot_gts.pso); ++boot_render_bind_epoch;   /* replaced the graphics pipeline */
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 12, k, 0);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(b));
    boot_dred_work(cl, BOOT_DRED_DISPATCH, BOOT_DRED_COMPUTE, NULL, NULL, va, boot_render_exec_id, boot_render_watch_event);
    BOOT_GP_DISPATCH("gpu timestamp", 0, cl, 1, 1, 1);
    unsigned nb = 1, i = 0;
    boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    while (i < boot_render_gw_dest_count && boot_render_gw_dest[i] != r->region[reg]) ++i;
    if (i == boot_render_gw_dest_count) {   /* COMMON -> COPY_DEST once per region until boot_render_gw_settle */
        boot_nvn_occl_transition(&t[nb++], r->region[reg], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        boot_render_gw_dest[boot_render_gw_dest_count++] = r->region[reg]; ++boot_render_gw_transitions;
    }
    BOOT_GP_RB("gpu timestamp report", cl, nb, t);
    BOOT_GP_CBR("gpu timestamp report", cl, r->region[reg], off, b, stage, 16);
    if (boot_gts.rb_due && boot_gts.rb) {   /* the log asked for the staged records (read a second later, once this list completed) */
        BOOT_GP_CBR("gpu timestamp log", cl, boot_gts.rb, 0, b, BOOT_GTS_STAGE, (uint64_t)BOOT_GTS_RECORDS * 16);
        boot_gts.rb_due = 0; boot_gts.rb_valid = 1; boot_gts.rb_list = boot_render_list_id; boot_gts.rb_seq = boot_gts.seq;
    }
    boot_nvn_occl_transition(&t[0], b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    BOOT_GP_RB("gpu timestamp report", cl, 1, t);
    boot_gts_gw_note(va);
    boot_gts.prov[rec] = prov; boot_gts.frame_of[rec] = (uint32_t)boot_gts.frame; boot_gts.type_of[rec] = (uint8_t)type;
    ++boot_gts.written[type ? 1 : 0];
    return 1;
}
/* Render-target sizes drawn since the last line (targets the renderer touched), largest first: up to `cap` distinct sizes
 * as {w, h, targets}. */
static inline unsigned boot_gts_rt_sizes(BootRenderRtTable *T, uint64_t since, int depth, uint32_t (*out)[3], unsigned cap) {
    unsigned n = 0;
    for (unsigned t = 0; t < T->count; ++t) {
        const BootRenderTarget *x = &T->rts[t];
        if (!x->texture || T->info[t].last_use <= since || ((x->format & 0x10000u) != 0) != depth || (x->format & BOOT_RENDER_PITCH_FLAG)) continue;
        unsigned j = 0; while (j < n && (out[j][0] != x->width || out[j][1] != x->height)) ++j;
        if (j < n) { ++out[j][2]; continue; }
        if (n < cap) { out[n][0] = x->width; out[n][1] = x->height; out[n][2] = 1; ++n; }
    }
    for (unsigned a = 1; a < n; ++a)   /* by area, largest first */
        for (unsigned j = a; j > 0 && (uint64_t)out[j][0] * out[j][1] > (uint64_t)out[j - 1][0] * out[j - 1][1]; --j) {
            uint32_t s[3] = {out[j][0], out[j][1], out[j][2]};
            out[j][0] = out[j - 1][0]; out[j][1] = out[j - 1][1]; out[j][2] = out[j - 1][2]; out[j - 1][0] = s[0]; out[j - 1][1] = s[1]; out[j - 1][2] = s[2];
        }
    return n;
}
/* Every present while the mode is on (host_nvn_play.h boot_nvn_exec_present, renderer lock held): the frame count, and
 * once a second the "gpu timestamps" line. */
static inline void boot_gts_present(BootRender *r) {
    ++boot_gts.frame;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    BootRenderRtTable *T = boot_render_rtab(r);
    if (!boot_gts.last_log) { boot_gts.last_log = now.QuadPart; boot_gts.log_frame = boot_gts.frame; boot_gts.rt_clock = T->clock; boot_gts.rb_due = 1; return; }
    if (now.QuadPart - boot_gts.last_log < f.QuadPart) return;
    double secs = (double)(now.QuadPart - boot_gts.last_log) / (double)f.QuadPart;
    /* spans from the records copied out at the previous line (one period late) */
    char spans[320]; snprintf(spans, sizeof spans, "%s", boot_gts.rb_valid ? "GPU values not read back yet" : "no GPU values copied yet");
    if (boot_gts.rb_valid && boot_gts.rb && boot_gts.device == r->gpu->device) {
        const uint64_t lid = boot_gts.rb_list, slot = lid & 65535;
        int kicked = boot_render_list_fence[slot].id == lid, done = kicked && ID3D12Fence_GetCompletedValue(r->gpu->fence) >= boot_render_list_fence[slot].fence;
        if (!kicked && (lid < boot_render_list_id || boot_render_list_fence[slot].id > lid)) { boot_gts.rb_valid = 0; boot_gts.rb_due = 1; }   /* its list never ran: copy again */
        else if (done) {
            const uint8_t *p = NULL; D3D12_RANGE rr = {0, (SIZE_T)BOOT_GTS_RECORDS * 16}, none = {0, 0};
            if (SUCCEEDED(ID3D12Resource_Map(boot_gts.rb, 0, &rr, (void **)&p)) && p) {
                uint64_t lo = boot_gts.rb_seq > BOOT_GTS_RECORDS ? boot_gts.rb_seq - BOOT_GTS_RECORDS : 0, keep = boot_gts.seq > BOOT_GTS_RECORDS ? boot_gts.seq - BOOT_GTS_RECORDS : 0;
                if (lo < keep) lo = keep;   /* records still described by prov/frame_of */
                uint64_t frames = 0, reports = 0, gsum = 0, gmax = 0, psum = 0, nlat = 0, g0 = 0, g1 = 0, p0 = 0, p1 = 0, n = 0; int64_t lat = 0;
                uint32_t cur = 0; unsigned groups = 0;   /* frames seen; the first (maybe begun before lo) and the last (maybe cut at the copy) are left out */
                for (uint64_t s = lo; s < boot_gts.rb_seq; ++s) {
                    uint32_t rec = (uint32_t)(s % BOOT_GTS_RECORDS); uint64_t g;
                    if (boot_gts.type_of[rec]) continue;   /* TIMESTAMP reports only */
                    memcpy(&g, p + (size_t)rec * 16 + 8, 8); lat += (int64_t)(g - boot_gts.prov[rec]); ++nlat;
                    if (!groups || boot_gts.frame_of[rec] != cur) {
                        if (groups > 1 && n >= 2) { ++frames; reports += n; gsum += g1 - g0; psum += p1 - p0; if (g1 - g0 > gmax) gmax = g1 - g0; }
                        ++groups; cur = boot_gts.frame_of[rec]; g0 = g1 = g; p0 = p1 = boot_gts.prov[rec]; n = 1;
                    } else { g1 = g; p1 = boot_gts.prov[rec]; ++n; }
                }
                ID3D12Resource_Unmap(boot_gts.rb, 0, &none);
                const double tpm = 614400.0;   /* GPU ticks per ms */
                if (frames) snprintf(spans, sizeof spans, "first to last TIMESTAMP report of a frame (%llu frames, %.1f reports each): record-time %.2f ms, GPU %.2f ms (max %.2f ms); GPU stamps %.2f ms after the record-time ones",
                                     (unsigned long long)frames, (double)reports / (double)frames, (double)psum / (double)frames / tpm, (double)gsum / (double)frames / tpm,
                                     (double)gmax / tpm, nlat ? (double)lat / (double)nlat / tpm : 0.0);
                else snprintf(spans, sizeof spans, "no frame with two TIMESTAMP reports in the copied records (%llu GPU values, %.2f ms after the record-time ones)",
                              (unsigned long long)nlat, nlat ? (double)lat / (double)nlat / tpm : 0.0);
                boot_gts.rb_valid = 0; boot_gts.rb_due = 1;
            } else { boot_gts.rb_valid = 0; boot_gts.rb_due = 1; }   /* map failed: copy again */
        }
    } else if (!boot_gts.rb_valid) boot_gts.rb_due = 1;
    uint32_t dz[32][3], cz[32][3]; unsigned nd = boot_gts_rt_sizes(T, boot_gts.rt_clock, 1, dz, 32), nc = boot_gts_rt_sizes(T, boot_gts.rt_clock, 0, cz, 32);
    if (nd > 4) nd = 4;
    if (nc > 4) nc = 4;   /* the four largest of each */
    char rts[256]; size_t at = (size_t)snprintf(rts, sizeof rts, "; render targets drawn: depth");
    for (unsigned j = 0; j < nd && at < sizeof rts; ++j) at += (size_t)snprintf(rts + at, sizeof rts - at, " %ux%u x%u", dz[j][0], dz[j][1], dz[j][2]);
    if (at < sizeof rts) at += (size_t)snprintf(rts + at, sizeof rts - at, "%s, colour", nd ? "" : " none");
    for (unsigned j = 0; j < nc && at < sizeof rts; ++j) at += (size_t)snprintf(rts + at, sizeof rts - at, " %ux%u x%u", cz[j][0], cz[j][1], cz[j][2]);
    if (!nc && at < sizeof rts) snprintf(rts + at, sizeof rts - at, " none");
    char line[900];
    snprintf(line, sizeof line, " (G1, TOTK_GPU_TS %s) last %.1f s: %llu frames, reports written by the GPU: TIMESTAMP %llu, TIMESTAMP_TOP %llu, today's value %llu%s; %s%s;",
             boot_gts_mode() == 2 ? "gpu-top" : "gpu", secs, (unsigned long long)(boot_gts.frame - boot_gts.log_frame), (unsigned long long)boot_gts.written[0],
             (unsigned long long)boot_gts.written[1], (unsigned long long)boot_gts.fallbacks, boot_gts.dilated ? " (guest clock dilated: all)" : "", spans, rts);
    boot_draw_log("nvn native: gpu timestamps%s present %llu, records staged %llu", line, boot_gts.frame, boot_gts.seq);
    boot_gts.last_log = now.QuadPart; boot_gts.log_frame = boot_gts.frame; boot_gts.rt_clock = T->clock;
    boot_gts.written[0] = boot_gts.written[1] = boot_gts.fallbacks = 0; boot_gts.dilated = boot_gts.dilated ? 1 : 0;
}
#endif
