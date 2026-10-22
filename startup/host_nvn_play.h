#ifndef TOTK_HOST_NVN_PLAY_H
#define TOTK_HOST_NVN_PLAY_H
/* Native NVN record player and D3D12 backend (TOTK_NVN=native, runtime_support with TOTK_DRAW_PATH).
 * Plays the API-level records of a submitted command buffer (host_nvn.h) with the bound state kept in
 * BootNvnExec (host_nvn_exec.h) and hands draws, clears and present to the existing renderer. */
#include "host_nvn_exec.h"
#include "host_nvn_predicate.h"   /* render-38c: conditional render on the GPU (D3D12 predication) */
#include "host_nvn_occl.h"        /* batch-2: SAMPLES_PASSED occlusion queries counted on the GPU */
#include "host_nvn_gpu_ts.h"      /* G1: TIMESTAMP reports written by the GPU (TOTK_GPU_TS=gpu, opt-in) */

typedef struct BootNvnOps {
    int ready;
    int bind_program, bind_blend, bind_color, bind_channel, bind_ds, bind_polygon, bind_ms, bind_vattr, bind_vstream, bind_vbuf,
        bind_ubo, bind_ssbo, bind_tex, set_tex_pool, set_smp_pool, set_rts, set_vp, set_vps, set_scissor, set_scissors,
        set_depth_range, set_depth_ranges, set_blend_color, set_stencil_ref, set_stencil_mask, set_stencil_vmask, set_poly_offset,
        set_restart, draw_arrays, draw_arrays_inst, draw_elements, draw_elements_bv, draw_elements_inst, draw_arrays_ind, draw_elements_ind,
        clear_color, clear_ds, barrier, update_ubo, clear_buffer, report_counter, report_value, call_commands, dispatch, dispatch_ind,
        set_raster_discard, set_depth_clamp, set_render_enable, set_render_enable_cond, reset_counter;
} BootNvnOps;
static inline BootNvnOps *boot_nvn_ops(void) {
    static BootNvnOps o;
    if (!o.ready) {
        #define BOOT_NVN_OP(f, n) o.f = boot_nvn_name_index("nvnCommandBuffer" n)
        BOOT_NVN_OP(bind_program, "BindProgram"); BOOT_NVN_OP(bind_blend, "BindBlendState"); BOOT_NVN_OP(bind_color, "BindColorState");
        BOOT_NVN_OP(bind_channel, "BindChannelMaskState"); BOOT_NVN_OP(bind_ds, "BindDepthStencilState"); BOOT_NVN_OP(bind_polygon, "BindPolygonState");
        BOOT_NVN_OP(bind_ms, "BindMultisampleState"); BOOT_NVN_OP(bind_vattr, "BindVertexAttribState"); BOOT_NVN_OP(bind_vstream, "BindVertexStreamState");
        BOOT_NVN_OP(bind_vbuf, "BindVertexBuffer"); BOOT_NVN_OP(bind_ubo, "BindUniformBuffer"); BOOT_NVN_OP(bind_ssbo, "BindStorageBuffer");
        BOOT_NVN_OP(bind_tex, "BindTexture"); BOOT_NVN_OP(set_tex_pool, "SetTexturePool"); BOOT_NVN_OP(set_smp_pool, "SetSamplerPool");
        BOOT_NVN_OP(set_rts, "SetRenderTargets"); BOOT_NVN_OP(set_vp, "SetViewport"); BOOT_NVN_OP(set_vps, "SetViewports");
        BOOT_NVN_OP(set_scissor, "SetScissor"); BOOT_NVN_OP(set_scissors, "SetScissors"); BOOT_NVN_OP(set_depth_range, "SetDepthRange");
        BOOT_NVN_OP(set_depth_ranges, "SetDepthRanges"); BOOT_NVN_OP(set_blend_color, "SetBlendColor"); BOOT_NVN_OP(set_stencil_ref, "SetStencilRef");
        BOOT_NVN_OP(set_stencil_mask, "SetStencilMask"); BOOT_NVN_OP(set_stencil_vmask, "SetStencilValueMask");
        BOOT_NVN_OP(set_poly_offset, "SetPolygonOffsetClamp"); BOOT_NVN_OP(set_restart, "SetPrimitiveRestart"); BOOT_NVN_OP(set_depth_clamp, "SetDepthClamp");
        BOOT_NVN_OP(draw_arrays, "DrawArrays"); BOOT_NVN_OP(draw_arrays_inst, "DrawArraysInstanced"); BOOT_NVN_OP(draw_elements, "DrawElements");
        BOOT_NVN_OP(draw_elements_bv, "DrawElementsBaseVertex"); BOOT_NVN_OP(draw_elements_inst, "DrawElementsInstanced");
        BOOT_NVN_OP(draw_arrays_ind, "DrawArraysIndirect"); BOOT_NVN_OP(draw_elements_ind, "DrawElementsIndirect");
        BOOT_NVN_OP(clear_color, "ClearColor"); BOOT_NVN_OP(clear_ds, "ClearDepthStencil"); BOOT_NVN_OP(barrier, "Barrier");
        BOOT_NVN_OP(update_ubo, "UpdateUniformBuffer"); BOOT_NVN_OP(clear_buffer, "ClearBuffer"); BOOT_NVN_OP(report_counter, "ReportCounter");
        BOOT_NVN_OP(report_value, "ReportValue"); BOOT_NVN_OP(call_commands, "CallCommands"); BOOT_NVN_OP(dispatch, "DispatchCompute");
        BOOT_NVN_OP(dispatch_ind, "DispatchComputeIndirect"); BOOT_NVN_OP(set_raster_discard, "SetRasterizerDiscard");
        BOOT_NVN_OP(set_render_enable, "SetRenderEnable"); BOOT_NVN_OP(set_render_enable_cond, "SetRenderEnableConditional");
        BOOT_NVN_OP(reset_counter, "ResetCounter");   /* batch-2 (host_nvn_occl.h) */
        #undef BOOT_NVN_OP
        o.ready = 1;
    }
    return &o;
}
static inline float boot_nvn_f(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
#include "host_nvn_fence_read.h"   /* fence-read trace (TOTK_FENCE_READ_TRACE=1, observation only; after BootNvnOps, which its record hook reads) */


/* Compute dispatch from the bound compute program (DESIGN.md appendix A, calibration 2026-10-15):
 * code at shader data + 0x100; control section: 0x700 c1 offset, 0x70c registers, 0x710 call-return stack,
 * 0x748/0x74c/0x750 block size, 0x754 shared memory, 0x764 barriers. Driver constant buffers (QMD slots):
 * c0 {0x200 group counts, 0x210 + 16*i uniform buffers (14), 0x310 + 16*i storage buffers, 0x420 driver
 * buffer}, c1 constants, c2 texture handles at 0x20 + 8*i, c(3+i) uniform buffer i. */
#define BOOT_NVN_CS_C0 0x9000u
#define BOOT_NVN_CS_C2 0xa000u
static inline void boot_nvn_exec_dispatch(BootContext *c, uint64_t self, BootRender *r, BootNvnExec *e, uint32_t gx, uint32_t gy, uint32_t gz) {
    const BootNvnProgramStage *cs = NULL;
    for (unsigned k = 0; e->has_prog && k < e->prog.count; ++k) if (e->prog.stage[k].kind == 6) cs = &e->prog.stage[k];
    if (!cs) { ++e->skipped; boot_draw_log("nvn native: dispatch without a compute program%s %llu %llu", "", e->dispatches, 0);
               if (BOOT_CENSUS_ON()) BOOT_CENSUS_COMPUTE(0, NULL, e->ssbo[5], 16, 1, "dispatch without a compute program");
               return; }
    uint32_t ctl[0x70 / 4];
    boot_ax_live(BOOT_AX_L_CODE, sizeof ctl);   /* async-xlate: counted as a live read on X (shader control data, written once at load) */
    if (!boot_host_read(c, self, cs->control + 0x700, ctl, sizeof ctl)) { ++e->skipped;
        if (BOOT_CENSUS_ON()) BOOT_CENSUS_COMPUTE(cs->data, NULL, e->ssbo[5], 16, 1, "compute control block read");
        return; }
    #define BOOT_NVN_CTL(off) ctl[((off) - 0x700) / 4]
    BootQmd q; memset(&q, 0, sizeof q);
    uint64_t code = cs->data + 0x100;
    BootComputeParams cp; memset(&cp, 0, sizeof cp);
    cp.program_region = code & ~UINT64_C(0xffffffff); q.program_offset = (uint32_t)code;
    cp.tic_address = e->tex_pool; cp.tic_max = e->tex_count ? e->tex_count - 1 : 0;
    cp.tsc_address = e->smp_pool; cp.tsc_max = e->smp_count ? e->smp_count - 1 : 0; cp.bindless_cb = 2;
    q.grid[0] = gx; q.grid[1] = gy; q.grid[2] = gz;
    q.block[0] = BOOT_NVN_CTL(0x748); q.block[1] = BOOT_NVN_CTL(0x74c); q.block[2] = BOOT_NVN_CTL(0x750) ? BOOT_NVN_CTL(0x750) : 1;
    q.shared_size = BOOT_NVN_CTL(0x754); q.local_crs = BOOT_NVN_CTL(0x710); q.barrier_count = BOOT_NVN_CTL(0x764); q.register_count = BOOT_NVN_CTL(0x70c);
    #undef BOOT_NVN_CTL
    /* driver constant buffers of the dispatch */
    uint8_t *c0 = e->block + BOOT_NVN_CS_C0, *c2 = e->block + BOOT_NVN_CS_C2;
    uint32_t groups[3] = {gx, gy, gz}; memcpy(c0 + 0x200, groups, 12);
    for (unsigned i = 0; i < 14; ++i) {
        uint64_t va = e->ubo[5][i] ? e->ubo[5][i] : e->block_va + BOOT_NVN_DUMMY_BASE; uint32_t size = e->ubo[5][i] ? (uint32_t)e->ubo_size[5][i] : 0x100;
        uint32_t v[4] = {(uint32_t)va, (uint32_t)(va >> 32), size, 0}; memcpy(c0 + 0x210 + 16 * i, v, 16);
    }
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t v[4] = {(uint32_t)e->ssbo[5][i], (uint32_t)(e->ssbo[5][i] >> 32), (uint32_t)e->ssbo_size[5][i], 0}; memcpy(c0 + 0x310 + 16 * i, v, 16);
    }
    memcpy(c0 + 0x420, &e->block_va, 8);
    for (unsigned i = 0; i < 32; ++i) { uint32_t v[2] = {e->tex[5][i], e->tex[5][i] ? 1u : 0u}; memcpy(c2 + 0x20 + 8 * i, v, 8); }
    boot_render_cb_written(e->block_va + BOOT_NVN_CS_C0, 0x500); boot_render_cb_written(e->block_va + BOOT_NVN_CS_C2, 0x200);
    q.cb_mask = 0xff;
    q.cb_address[0] = e->block_va + BOOT_NVN_CS_C0; q.cb_size[0] = 0x500;
    q.cb_address[1] = cs->data + cs->c1_offset; q.cb_size[1] = 0x10000; q.cb_direct = 1u << 1;
    q.cb_address[2] = e->block_va + BOOT_NVN_CS_C2; q.cb_size[2] = 0x200;
    for (unsigned i = 0; i < 5; ++i) {
        if (e->ubo[5][i]) { q.cb_address[3 + i] = e->ubo[5][i]; q.cb_size[3 + i] = (uint32_t)((e->ubo_size[5][i] + 255) & ~UINT64_C(255)); q.cb_direct |= 1u << (3 + i); }
        else { q.cb_address[3 + i] = e->block_va + BOOT_NVN_DUMMY_BASE; q.cb_size[3 + i] = 0x100; }
    }
    uint8_t id[32]; char variant[64] = "";
    const uint64_t ds_deferred0 = boot_compute_deferred; boot_ds_dispatch_pre(e->ssbo[5], e->ssbo_size[5]);   /* data-snapshot: TOTK_DATA_SNAPSHOT_STORED=0: v1's marks */
    int rc = boot_compute_dispatch_decoded(r, &cp, q, id, variant, sizeof variant);
    boot_ds_dispatch_post(boot_compute_deferred != ds_deferred0, e->ssbo[5], e->ssbo_size[5]);   /* data-snapshot v2fix: a deferred dispatch's storage buffers are pending until the idle */
    if (BOOT_CENSUS_ON()) BOOT_CENSUS_COMPUTE(cs->data, rc ? NULL : id, e->ssbo[5], 16, rc, rc ? r->error : NULL);
    if (rc) { ++e->skipped; boot_draw_log("nvn native: compute dispatch failed: %s %llu %llu", r->error, e->dispatches, rc); return; }
    if (boot_frt_on() && gx && gy && gz) boot_frt_dispatch(e, id);   /* fence-read trace: compute storage buffers over a watched range (an empty grid leaves id unset) */
    ++e->dispatches;
}

/* Native draw trace and non-finite probe (diagnostics, off by default; the emulated path's request files):
 * frames\draw-trace.request ("<events> [<skip>]") logs the next draws and clears of native submissions to
 * frames\draw-trace.txt in the emulated trace's format (state, constant buffers, resolved textures with their
 * host slots, shader ids, host targets) plus the bound handles per stage and every deferred texture bind;
 * frames\nan-probe.request ("<target address hex>") writes frames\nan-probe.txt for the next submission that
 * has that target: every event after which the count of non-finite texels in its middle row changed. Event
 * numbers are per submission (draw instances and clears), the same in both files. */
static uint32_t boot_nvn_trace_ev;   /* events of the current submission */
static int boot_nvn_trace_on;        /* the current event is traced */
/* render-10: a draw or clear into the watched render target (frames\rt-watch.request, host_gpu_render.h) is traced
 * on its own into frames\rt-watch-trace.txt (state, constant buffers, resolved textures), the first 16 times, when
 * no draw trace is running: what writes a target the game reads back much later (the file-select backdrop). */
static int boot_nvn_watch_trace; static unsigned boot_nvn_watch_traced;
static inline void boot_nvn_trace_pre(BootRender *r, BootNvnExec *e, uint32_t kind, const BootDrawState *s, BootCbufBinding cbufs[5][18],
                                      uint32_t instance, uint32_t clear_value, const uint32_t *clear_regs) {
    static BootDrawEvent ev;
    boot_render_watch_event = boot_nvn_trace_ev;   /* render-20: rt-watch reload lines name the event */
    boot_title_begin_event(boot_nvn_trace_ev, kind, s, instance, clear_value, clear_regs);
    if (!boot_title_requested && boot_render_watch_va && !boot_draw_trace_file && boot_nvn_watch_traced < 16) {
        int hit = 0;
        for (unsigned j = 0; j < s->rt_count && j < 8; ++j) hit |= s->rt[s->rt_map[j]].address == boot_render_watch_va && s->rt[s->rt_map[j]].format;
        if (hit && (boot_draw_trace_file = fopen("frames\\rt-watch-trace.txt", "a")) != NULL) {
            fprintf(boot_draw_trace_file, "\n=== rt-watch %llx: submission %llu event %u\n", (unsigned long long)boot_render_watch_va,
                    (unsigned long long)boot_render_exec_id, boot_nvn_trace_ev);
            boot_draw_trace_left = 1; boot_draw_trace_skip = 0; boot_nvn_watch_trace = 1; ++boot_nvn_watch_traced;
        }
    }
    boot_nvn_trace_on = boot_draw_trace_left && boot_draw_trace_file && boot_nvn_trace_ev >= boot_draw_trace_skip;
    if (!boot_nvn_trace_on) return;
    --boot_draw_trace_left; ++boot_draw_trace_seq;
    memset(&ev, 0, sizeof ev); ev.kind = kind; ev.instance = instance; ev.clear_value = clear_value; ev.state = *s;
    if (clear_regs) memcpy(ev.clear_regs, clear_regs, sizeof ev.clear_regs);
    if (cbufs) memcpy(ev.cbufs, cbufs, sizeof ev.cbufs);
    boot_draw_trace_event(r, &ev, boot_nvn_trace_ev, 0);
    FILE *f = boot_draw_trace_file;
    for (unsigned st = 0; st < 2; ++st) {
        fprintf(f, "  nvn %s handles", st ? "fs" : "vs");
        for (unsigned i = 0; i < 32; ++i) if (e->tex[st][i]) fprintf(f, " %u:%08x", i, e->tex[st][i]);
        fputc('\n', f);
    }
    BootRenderTrace *tr = &boot_render_trace;
    tr->active = kind == 1; tr->drawn = 0; tr->query = boot_draw_trace_seq % BOOT_RENDER_TRACE_QUERIES; tr->so_last = -1;
    tr->so_count = BOOT_RENDER_SO_PROBES;   /* no position capture: nothing reads it back in native mode */
    tr->tex_len = 0; tr->tex_info[0] = 0;
}
static inline void boot_nvn_trace_post(BootRender *r, const BootDrawState *s, int rc) {
    if (boot_nvn_trace_on) {
        BootRenderTrace *tr = &boot_render_trace; FILE *f = boot_draw_trace_file;
        fprintf(f, "  result %d %s\n", rc, rc ? r->error : "");
        if (tr->tex_len) fprintf(f, "  tex%s\n", tr->tex_info);
        if (tr->active && tr->drawn) {
            fprintf(f, "  vs "); boot_draw_trace_hex(f, tr->vs, 32); fprintf(f, " ps "); boot_draw_trace_hex(f, tr->ps, 32);
            fprintf(f, "\n  host draw count %u indexed %u topo %u vp %g,%g %gx%g depth %g..%g scissor %ld,%ld..%ld,%ld zt rt#%u rts",
                    tr->draw_count, tr->indexed, tr->topo, tr->vp.TopLeftX, tr->vp.TopLeftY, tr->vp.Width, tr->vp.Height, tr->vp.MinDepth, tr->vp.MaxDepth,
                    tr->sc.left, tr->sc.top, tr->sc.right, tr->sc.bottom, tr->zt_index);
            for (unsigned k = 0; k < s->rt_count && k < 8; ++k) fprintf(f, " #%u(mask %x blend %u tex %ux%u dxgi %u pso %u state %x)", tr->rt_index[k], tr->write_mask[k], tr->blend_enable[k],
                                                                         tr->rt_w[k], tr->rt_h[k], tr->rt_fmt[k], tr->pso_rtfmt[k], tr->rt_state[k]);
            fputc('\n', f);
        }
        tr->active = 0; boot_nvn_trace_on = 0;
    }
    if (boot_nvn_watch_trace) { fclose(boot_draw_trace_file); boot_draw_trace_file = NULL; boot_draw_trace_left = 0; boot_nvn_watch_trace = 0; }
    boot_title_end_event(rc);
    boot_nan_probe_record(r, boot_nvn_trace_ev);
    ++boot_nvn_trace_ev;
}
static inline void boot_nvn_trace_begin(BootRender *r) {
    boot_draw_trace_arm(r); boot_render_trace.active = 0; boot_nvn_trace_ev = 0; boot_nvn_trace_on = 0;
    boot_nan_probe_arm(r); boot_nan_probe.slots = 0; boot_nan_probe_record(r, UINT32_MAX);
}
static inline void boot_nvn_trace_end(BootRender *r) {
    boot_light88_sub_end(boot_render_exec_id);
    boot_light86_sub_end(boot_render_exec_id);
    boot_light84_sub_end(boot_render_exec_id);
    if (boot_draw_trace_file) {
        fflush(boot_draw_trace_file);
        if (!boot_draw_trace_left) { fclose(boot_draw_trace_file); boot_draw_trace_file = NULL; boot_draw_log("native draw trace complete%s %llu %llu", "", boot_draw_trace_seq, 0); }
    }
    if (!boot_nan_probe.armed || !boot_nan_probe.slots) return;
    if (r->recording && !boot_render_kick(r)) return;
    if (!boot_render_idle(r)) return;
    boot_nan_probe.armed = 0; DeleteFileA("frames\\nan-probe.request");
    FILE *f = fopen("frames\\nan-probe.txt", "w"); uint8_t *m = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)boot_nan_probe.slots * 512};
    if (!f || FAILED(ID3D12Resource_Map(boot_nan_probe.rb, 0, &rr, (void **)&m))) { if (f) fclose(f); return; }
    fprintf(f, "native: target %llx: %u probes (middle row, up to 64 texels) over %u events\n", (unsigned long long)boot_nan_probe.address, boot_nan_probe.slots, boot_nvn_trace_ev);
    unsigned last = UINT_MAX; uint32_t last0 = 0, last1 = 0;
    for (unsigned k = 0; k < boot_nan_probe.slots; ++k) {
        const uint8_t *p = m + (size_t)k * 512; uint32_t ev = boot_nan_probe.event[k];
        unsigned n = boot_nan_probe_nonfinite(p, boot_nan_probe.dxgi[k], boot_nan_probe.width[k]);
        uint32_t t0, t1; memcpy(&t0, p, 4); memcpy(&t1, p + 4 * (boot_nan_probe.width[k] / 2), 4);
        /* also every change of the first or middle texel: which event wrote a wrong colour (render-05) */
        if (n == last && t0 == last0 && t1 == last1) continue;
        last0 = t0; last1 = t1;
        fprintf(f, "after event %d: non-finite %u/%u texels (first %08x middle %08x)\n", ev == UINT32_MAX ? -1 : (int)ev, n, boot_nan_probe.width[k], t0, t1);
        last = n;
    }
    D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(boot_nan_probe.rb, 0, &none);
    fclose(f);
}

/* nvnCommandBufferCopyTextureToBuffer(cmd, texture, view, const NVNcopyRegion *region, NVNbufferAddress dst, flags)
 * (render-16): the region {x, y, z, width, height, depth} of the texture's image, tightly packed rows (the game
 * never sets a copy row stride), into guest memory at dst. The game reads such buffers on the CPU after its sync:
 * the autosave thumbnail is copied out of a render target this way, and the ignored command left every native
 * thumbnail black. The image is the host render target at the texture's address when one exists, else guest
 * memory (render-19: the copy only reads; looking the source up with boot_render_target created a host target for
 * a texture that was never drawn, marked it written, and samplers then took that frozen image instead of what
 * compute or copies later stored there). The copy waits for the queue (a readback, as the pitch-target flush
 * does), which only screenshot copies pay. Depth, compressed and 3D sources are not handled (logged). */
static inline int boot_nvn_copy_t2b_op(void) {
    static int op = -2; if (op == -2) op = boot_nvn_name_index("nvnCommandBufferCopyTextureToBuffer");
    return op;
}
/* flashfix: the view (16 bytes) and texture object (0x80) a copy record captured when it was recorded (host_nvn.h
 * boot_nvn_record_build, block arg 0x80 | texture argument `ta`), NULL for none (TOTK_COPY_OBJ_CAPTURE=0, older records). */
static inline const uint8_t *boot_nvn_copy_captured(const BootNvnRecord *rec, unsigned ta) {
    return rec->blocks > 1 && rec->block[1].arg == (0x80u | ta) && rec->block[1].bytes >= 16 + 0x80 ? rec->block[1].data : NULL;
}
static inline void boot_nvn_exec_copy_tex_to_buf(BootContext *c, uint64_t self, BootRender *r, const uint64_t *x, const uint8_t *region, uint32_t region_n, const uint8_t *captured) {
    static unsigned logged, failed;
    int32_t rg[6]; uint8_t view[16]; const uint8_t *vp = NULL; BootColorTarget ct;
    if (region_n < 24 || !x[1] || !x[4]) { if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer without region or destination%s %llx %llx", "", x[1], x[4]); return; }
    memcpy(rg, region, 24);
    if (captured) { if (x[2]) vp = captured; }   /* flashfix: as recorded (zero view bytes select nothing) */
    else if (x[2] && (boot_ax_live(BOOT_AX_L_OBJECT, sizeof view), boot_host_read(c, self, x[2], view, sizeof view))) vp = view;   /* async-xlate: counted on X */
    if (!boot_nvn_texture_target_obj(c, self, x[1], captured ? captured + 16 : NULL, vp, &ct, NULL) || ct.pitch_layout) { if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: source not a colour image%s %llx %llu", "", x[1], 0); return; }
    DXGI_FORMAT cfmt;
    if (!boot_render_ct_format(ct.format, &cfmt) || !ct.width || !ct.height || ct.log_gob_w || ct.log_gob_h > 5 || ct.log_gob_d > 5) {
        if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: source format/layout%s %llx %llu", "", ct.address, ct.format); return; }
    uint32_t bpp = boot_render_dxgi_bytes(cfmt);
    if (!bpp || rg[0] < 0 || rg[1] < 0 || rg[3] <= 0 || rg[4] <= 0 || rg[2] != 0 || rg[5] > 1) { if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: region not supported%s %llx %llu", "", ct.address, (uint64_t)(uint32_t)rg[3] << 32 | (uint32_t)rg[4]); return; }
    uint32_t x0 = (uint32_t)rg[0], y0 = (uint32_t)rg[1], w = (uint32_t)rg[3], h = (uint32_t)rg[4];
    if (x0 >= ct.width || y0 >= ct.height) return;
    if (w > ct.width - x0) w = ct.width - x0;
    if (h > ct.height - y0) h = ct.height - y0;
    uint64_t row = (uint64_t)w * bpp;
    /* render-19: an existing, current host target of this image (same address, extent and format), not touched */
    BootRenderRtTable *T = boot_render_rtab(r); BootRenderTarget *t = NULL; int tk = -1;
    for (unsigned k = 0; k < T->count; ++k) {
        BootRenderTarget *o = &T->rts[k];
        if (!o->texture || o->address != ct.address || o->width != ct.width || o->height != ct.height || o->format != ct.format) continue;
        if (tk < 0 || T->info[k].last_use >= T->info[tk].last_use) tk = (int)k;
    }
    if (tk >= 0 && !T->info[tk].stale && !boot_render_rt_overlap_dirty(T, (unsigned)tk, boot_render_rt_extent(&T->rts[tk], &T->info[tk]))) t = &T->rts[tk];
    if (!t) {   /* guest memory: targets over it written back first (oldest first), then detiled on the CPU */
        uint32_t wb = ct.width * bpp;
        uint64_t extent = boot_texture_slice_extent(wb, ct.height, ct.log_gob_h, ct.log_gob_d);
        if (!extent || !boot_render_flush_targets(r, ct.address, extent) || !boot_render_idle(r)) {
            if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: guest image%s %llx %llu", r->error, ct.address, extent); return; }
        const uint8_t *src = boot_render_guest(r, ct.address, extent);
        uint8_t *lin = src ? malloc((size_t)wb * ct.height) : NULL;
        uint8_t *dst = lin ? boot_render_guest(r, x[4], row * h) : NULL;
        if (!dst || !boot_texture_detile_slice(lin, wb, src, (size_t)extent, wb, ct.height, ct.log_gob_h, ct.log_gob_d)) {
            free(lin); if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: guest detile%s %llx %llu", "", ct.address, x[4]); return; }
        for (uint32_t y = 0; y < h; ++y) memcpy(dst + (size_t)y * row, lin + (size_t)(y0 + y) * wb + (size_t)x0 * bpp, (size_t)row);
        free(lin);
        boot_render_invalidate(x[4], row * h);
        if (logged++ < 8) boot_draw_log("nvn native: copy texture to buffer (guest image)%s %llx -> %llx", "", ct.address, x[4]);
        return;
    }
    D3D12_RESOURCE_DESC sd; t->texture->lpVtbl->GetDesc(t->texture, &sd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
    ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
    if (!boot_render_begin(r)) return;
    boot_render_staging_reset(&boot_render_rb_ring);
    ID3D12Resource *rb = NULL; uint64_t base = 0;
    uint8_t *mapped = boot_render_staging_alloc(r, &boot_render_rb_ring, D3D12_HEAP_TYPE_READBACK, total, &rb, &base);
    if (!mapped) { if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: readback staging%s %llu %llu", "", total, 0); return; }
    D3D12_RESOURCE_STATES was = t->state;
    if (!boot_render_rt_barrier(r, t, D3D12_RESOURCE_STATE_COPY_SOURCE)) return;
    D3D12_TEXTURE_COPY_LOCATION dl = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {t->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    dl.PlacedFootprint = fp; dl.PlacedFootprint.Offset += base;
    BOOT_GP_CTR("nvn copy tex->buf", r->cl, &dl, 0, 0, 0, &sl, NULL);
    boot_render_rt_barrier(r, t, was);
    if (!boot_render_submit(r)) { if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: submit%s %llu %llu", "", 0, 0); return; }
    const uint8_t *src = mapped + fp.Offset;
    uint8_t *dst = boot_render_guest(r, x[4], row * h);
    if (!dst) { if (failed++ < 8) boot_draw_log("nvn native: copy texture to buffer: destination outside guest memory%s %llx %llu", "", x[4], row * h); return; }
    for (uint32_t y = 0; y < h; ++y) memcpy(dst + (size_t)y * row, src + (size_t)(y0 + y) * fp.Footprint.RowPitch + (size_t)x0 * bpp, (size_t)row);
    boot_render_invalidate(x[4], row * h);
    if (logged++ < 8) boot_draw_log("nvn native: copy texture to buffer%s %llx -> %llx", "", ct.address, x[4]);
}

/* nvnCommandBufferCopyBufferToTexture(cmd, NVNbufferAddress src, const NVNtexture *dst, const NVNtextureView *view,
 * const NVNcopyRegion *region, int flags) (movie-03, play-work/movie-work/README.md): tightly packed rows (the game never
 * sets a copy row stride: nvnCommandBufferSetCopyRowStride is unused) at src, into the region {x, y, z, width, height,
 * depth} of the texture's image. Was ignored ("ignored command"), so the cutscene movie's Y (R8) and UV (RG8) textures,
 * which the game fills every frame from its CPU-written NV12 buffers (main+0x240de94 / +0x240df10), never received a
 * frame and the movie shaders (ps 3083692fca0c29c3 / 4c48c3b838638e67) sampled stale memory: green with stripes.
 * The bytes are stored into the texture's guest memory in its own layout (block-linear with the texture's block
 * height/depth, or pitch rows), in command order on the CPU like the CPU-texel path (render-32): host render targets
 * over it go stale and cached textures over it rebuild at their next use, while work recorded earlier keeps the
 * resource it was recorded with. A partial region first writes back targets over the image (and waits), so the
 * bytes outside the region are current; the movie always copies whole images, which never waits. */
static inline int boot_nvn_copy_b2t_op(void) {
    static int op = -2; if (op == -2) op = boot_nvn_name_index("nvnCommandBufferCopyBufferToTexture");
    return op;
}
/* G2 (batch-4): nvnCommandBufferSetPointSize(cmd, float size), the point size of POINTS draws whose vertex shader writes
 * none (host_gpu_render_draw.h boot_pt_*; nxport crates/engine/src/metal/mod.rs:2700-2705). Was an ignored command. */
static inline int boot_nvn_point_size_op(void) {
    static int op = -2; if (op == -2) op = boot_nvn_name_index("nvnCommandBufferSetPointSize");
    return op;
}
static inline void boot_nvn_exec_copy_buf_to_tex(BootContext *c, uint64_t self, BootRender *r, const uint64_t *x, const uint8_t *region, uint32_t region_n, const uint8_t *captured) {
    static unsigned logged, failed;
    int32_t rg[6]; uint8_t view[16]; const uint8_t *vp = NULL; BootColorTarget ct;
    if (region_n < 24 || !x[1] || !x[2]) { if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture without region or source%s %llx %llx", "", x[1], x[2]); return; }
    memcpy(rg, region, 24);
    if (captured) { if (x[3]) vp = captured; }   /* flashfix: as recorded */
    else if (x[3] && (boot_ax_live(BOOT_AX_L_OBJECT, sizeof view), boot_host_read(c, self, x[3], view, sizeof view))) vp = view;   /* async-xlate: counted on X */
    if (!boot_nvn_texture_target_obj(c, self, x[2], captured ? captured + 16 : NULL, vp, &ct, NULL)) { if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture: destination not a colour image%s %llx %llu", "", x[2], 0); return; }
    DXGI_FORMAT cfmt;
    if (!boot_render_ct_format(ct.format, &cfmt) || !ct.width || !ct.height || ct.log_gob_w || ct.log_gob_h > 5 || ct.log_gob_d > 5) {
        if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture: destination format/layout%s %llx %llu", "", ct.address, ct.format); return; }
    uint32_t bpp = boot_render_dxgi_bytes(cfmt);
    if (!bpp || rg[0] < 0 || rg[1] < 0 || rg[3] <= 0 || rg[4] <= 0 || rg[2] != 0 || rg[5] > 1) { if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture: region not supported%s %llx %llu", "", ct.address, (uint64_t)(uint32_t)rg[3] << 32 | (uint32_t)rg[4]); return; }
    uint32_t x0 = (uint32_t)rg[0], y0 = (uint32_t)rg[1], w = (uint32_t)rg[3], h = (uint32_t)rg[4];
    uint32_t img_w = ct.pitch_layout ? ct.width / bpp : ct.width;   /* pitch targets carry the row stride in bytes */
    if (x0 >= img_w || y0 >= ct.height) return;
    uint64_t row = (uint64_t)w * bpp;   /* source rows: the region's width, tightly packed */
    if (w > img_w - x0) w = img_w - x0;
    if (h > ct.height - y0) h = ct.height - y0;
    const uint8_t *src = boot_render_guest(r, x[1], row * (h - 1) + (uint64_t)w * bpp);
    uint32_t wb = ct.pitch_layout ? ct.width : ct.width * bpp;
    uint64_t extent = ct.pitch_layout ? (uint64_t)wb * ct.height : boot_texture_slice_extent(wb, ct.height, ct.log_gob_h, ct.log_gob_d);
    int whole = x0 == 0 && y0 == 0 && w == img_w && h == ct.height;
    if (!src || !extent) { if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture: source outside guest memory%s %llx %llu", "", x[1], row * h); return; }
    if (!whole && (!boot_render_flush_targets(r, ct.address, extent) || !boot_render_idle(r))) {
        if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture: write-back%s %llx %llu", r->error, ct.address, extent); return; }
    uint8_t *dst = boot_render_guest(r, ct.address, extent);
    if (!dst) { if (failed++ < 8) boot_draw_log("nvn native: copy buffer to texture: destination outside guest memory%s %llx %llu", "", ct.address, extent); return; }
    if (ct.pitch_layout) {
        for (uint32_t y = 0; y < h; ++y) memcpy(dst + (size_t)(y0 + y) * wb + (size_t)x0 * bpp, src + (size_t)y * row, (size_t)w * bpp);
    } else if (whole) {
        boot_texture_tile_slice(dst, (size_t)extent, src, (size_t)row, wb, h, ct.log_gob_h, ct.log_gob_d);
    } else {   /* sub-rectangle: the GOB addressing of boot_texture_tile_slice with byte offsets */
        uint64_t block = UINT64_C(512) << (ct.log_gob_h + ct.log_gob_d), gx = ((uint64_t)wb + 63) / 64;
        uint32_t bh_mask = (1u << ct.log_gob_h) - 1, xb0 = x0 * bpp, xb1 = (x0 + w) * bpp;
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t yy = y0 + y;
            uint64_t oy = (uint64_t)(yy >> (3 + ct.log_gob_h)) * gx * block + ((uint64_t)((yy >> 3) & bh_mask) << 9) + ((yy & 7) / 2) * 64 + (yy & 1) * 16;
            const uint8_t *line = src + (size_t)y * row - xb0;
            for (uint32_t xb = xb0; xb < xb1;) {
                uint32_t n = 16 - (xb & 15); if (n > xb1 - xb) n = xb1 - xb;
                memcpy(dst + oy + (xb / 64) * block + ((xb & 63) / 32) * 256 + ((xb & 31) / 16) * 32 + (xb & 15), line + xb, n);
                xb += n;
            }
        }
    }
    boot_render_rt_guest_written(r, ct.address, extent);   /* targets over it reload; GPU ownership released */
    boot_render_invalidate(ct.address, extent);             /* cached textures rebuild at their next use */
    if (logged++ < 8) {
        char why[96]; snprintf(why, sizeof why, " %ux%u (fmt %x, bpp %u, block h/d %u/%u%s) at", w, h, ct.format, bpp, ct.log_gob_h, ct.log_gob_d, ct.pitch_layout ? ", pitch" : "");
        boot_draw_log("nvn native: copy buffer to texture%s %llx <- %llx", why, ct.address, x[1]);
    }
}

static uint64_t boot_nvn_render_failed;   /* flashfix: draws and clears the renderer failed (flicker.log per-present counters) */
/* One draw (all instances) with the bound state. */
static inline void boot_nvn_exec_draw(BootRender *r, BootNvnExec *e, uint32_t mode, int indexed, uint32_t index_type, uint64_t index_va,
                                      uint32_t first, uint32_t count, int32_t base_vertex, uint32_t base_instance, uint32_t instances) {
    static BootDrawState s; static BootRenderDraw d;
    if (!count || !instances) return;
    uint64_t ph0 = boot_render_ph_now();
    if (!boot_nvn_exec_state(e, &s, d.cbufs)) { ++e->skipped; if (BOOT_CENSUS_ON()) BOOT_CENSUS_NOSTATE(index_va, first, count, mode); return; }
    boot_render_ph_qpc[8] += boot_render_ph_now() - ph0;
    if (instances > 1) { ++boot_render_ph_instanced; boot_render_ph_instances += instances; }   /* one host draw per instance */
    s.topology = mode; s.indexed = (uint32_t)indexed; s.index_size = index_type; s.index_address = index_va;
    s.index_limit = index_va + (uint64_t)(first + count) * (index_type == 0 ? 1u : index_type == 1 ? 2u : 4u);
    s.first = first; s.count = count; s.base_vertex_reg = (uint32_t)base_vertex; s.base_instance_reg = base_instance;
    boot_nvn_state_diag_draw(e, &s, mode);   /* batch-3 N4/G7, N11/G9 diagnostics (log only) */
    d.state = &s; d.guest_barrier = e->guest_barrier; e->guest_barrier = 0;
    static int expand = -1;   /* play (41): one host draw for all instances unless TOTK_INSTANCE_EXPAND=1 */
    if (expand < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_INSTANCE_EXPAND", v, sizeof v); expand = k && k < sizeof v && v[0] == '1'; }
    uint32_t host_draws = expand ? instances : 1;
    for (uint32_t i = 0; i < host_draws; ++i) {
        memset(d.host_state, 0, sizeof d.host_state);
        uint32_t one = 0x3f800000u; memcpy(&d.host_state[0], &one, 4); d.host_state[5] = i;
        if (!expand) boot_render_next_instances = instances;
        boot_render_occl.want = boot_nvn_occl_want(r);   /* batch-2: count this draw's samples while an occlusion interval is open */
        boot_nvn_trace_pre(r, e, 1, &s, d.cbufs, i, 0, NULL);
        boot_tile89_native_enter(e,&d,first,(uint32_t)base_vertex,base_instance,instances,i,(unsigned)expand,d.guest_barrier,e->ssbo[0][4],e->ssbo_size[0][4],e->ssbo[1][4],e->ssbo_size[1][4]);
        const uint64_t ds_stores0 = boot_render_store_draws; boot_ds_store_stages = 0;   /* data-snapshot: a storing draw's storing stage writes its storage buffers */
        int rc = boot_render_draw(r, &d);
        if (boot_render_store_draws != ds_stores0) {
            const unsigned ds_st = boot_ds_store_marked_stages();   /* the storing stage(s); both when unknown and with TOTK_DATA_SNAPSHOT_STORED=0 (v1) */
            if (ds_st & 1u) boot_ds_stores(e->ssbo[0], e->ssbo_size[0], 16);   /* vertex stage */
            if (ds_st & 2u) boot_ds_stores(e->ssbo[1], e->ssbo_size[1], 16);   /* fragment stage */
        }
        if (boot_frt_on() && !rc) boot_frt_draw(r, e, boot_render_store_draws != ds_stores0, boot_ds_store_stages);   /* fence-read trace: graphics storage buffers over a watched range */
        boot_tile89_native_leave();
        boot_nvn_trace_post(r, &s, rc);
        if (rc == BOOT_RENDER_E_SHADER) { ++e->skipped; static uint64_t n; if (++n % 50 == 1) boot_draw_log("nvn native: skipped draw, untranslated shader: %s %llu %llu", r->error, n, 0); return; }
        if (rc) { ++e->skipped; ++boot_nvn_render_failed; boot_draw_log("nvn native: draw failed: %s %llu %llu", r->error, e->draws, mode); return; }
        ++e->draws; d.guest_barrier = 0;
    }
}
static inline void boot_nvn_exec_clear(BootRender *r, BootNvnExec *e, uint32_t value, const uint32_t color[4], uint32_t depth, uint32_t stencil) {
    static BootDrawState s; static BootCbufBinding cb[5][18]; static uint32_t regs[4096];
    memset(&s, 0, sizeof s); s.rt_count = e->rt_count;
    for (unsigned i = 0; i < 8; ++i) { s.rt_map[i] = i; s.rt[i] = e->rt[i]; s.rt[i].write_mask = 15; }
    s.zt = e->zt;
    s.scissor[0].enabled = 1; s.scissor[0].x0 = (uint32_t)e->scissor[0][0]; s.scissor[0].y0 = (uint32_t)e->scissor[0][1];
    s.scissor[0].x1 = s.scissor[0].x0 + (uint32_t)e->scissor[0][2]; s.scissor[0].y1 = s.scissor[0].y0 + (uint32_t)e->scissor[0][3];
    (void)cb;
    for (unsigned k = 0; k < 4; ++k) regs[(0xd80 + 4 * k) / 4] = color ? color[k] : 0;
    regs[0xd90 / 4] = depth; regs[0xda0 / 4] = stencil; regs[0x10f8 / 4] = 1;   /* clears honour scissor 0 */
    {   uint32_t cr[8] = {regs[0xd80 / 4], regs[0xd84 / 4], regs[0xd88 / 4], regs[0xd8c / 4], depth, stencil, 1, 0};
        boot_nvn_trace_pre(r, e, 2, &s, NULL, 0, value, cr); }
    int rc = boot_render_clear(r, &s, regs, value);
    boot_nvn_trace_post(r, &s, rc);
    if (rc) { ++boot_nvn_render_failed; boot_draw_log("nvn native: clear failed: %s %llu %llu", r->error, value, 0); }
    ++e->clears;
}
/* Guest memory store in command order: a GPU copy after the work recorded before it (constant buffers
 * are read on the GPU, so a CPU store now would also change what earlier recorded draws read). Where a
 * copy is impossible (not contiguous in one region, over 64 KiB) the queue is waited for and the CPU
 * stores: everything recorded before has then completed and nothing later has run. */
static inline int boot_nvn_exec_store(BootRender *r, uint64_t va, const void *src, size_t n) {
    if (boot_render_gpu_write(r, va, src, n)) return 1;
    if (!boot_render_idle(r)) return 0;
    uint8_t *p = boot_render_guest(r, va, n);
    if (!p) { boot_draw_log("nvn native: store outside guest memory%s %llx %llu", "", va, n); return 0; }
    memcpy(p, src, n); boot_render_cb_written(va, n);
    return 1;
}
/* nvnCommandBufferClearBuffer: 32-bit pattern, GPU-ordered in 64 KiB copies (or waited + CPU, see above). */
static inline void boot_nvn_exec_fill(BootRender *r, uint64_t va, uint64_t size, uint32_t value) {
    static uint32_t pattern[16384]; static uint32_t filled = 1;
    if (filled != value || pattern[0] != value) { for (unsigned i = 0; i < 16384; ++i) pattern[i] = value; filled = value; }
    size &= ~UINT64_C(3);
    for (uint64_t at = 0; at < size; ) {
        uint64_t n = size - at > 65536 ? 65536 : size - at;
        if (!boot_render_gpu_write(r, va + at, pattern, n)) {
            if (!boot_render_idle(r)) return;
            for (uint64_t k = at; k < size; k += 4) { uint8_t *p = boot_render_guest(r, va + k, 4); if (p) memcpy(p, &value, 4); }
            boot_render_cb_written(va + at, size - at);
            return;
        }
        at += n;
    }
}
/* render-38d: indirect commands, by BOOT_RENDER_IND_* kind: records (before rasterizer-discard / render-enable drops),
 * reached here, sent to the CPU read below, and its queue waits (GPU syncs); why a command took the CPU read:
 * 0 TOTK_INDIRECT=cpu, 1 CPU index work (quads/fans/polygons/line loops, 8-bit indices) or TOTK_INSTANCE_EXPAND=1,
 * 2 arguments not 4-byte aligned / not contiguous in one guest region, 3 GPU objects unavailable. */
static uint64_t boot_nvn_ind_records[4], boot_nvn_ind_seen[4], boot_nvn_ind_cpu[4], boot_nvn_ind_syncs, boot_nvn_ind_why[4];
/* Indirect arguments are read by the CPU here: recorded and executing work that may write them completes first. */
static inline const uint32_t *boot_nvn_exec_indirect(BootRender *r, BootNvnExec *e, uint64_t va, uint64_t n) {
    BootGpu *g = r->gpu;
    if ((r->recording || (g && g->fence && ID3D12Fence_GetCompletedValue(g->fence) < g->submitted)) && boot_render_idle(r)) { ++e->idles; ++boot_nvn_ind_syncs; }
    return (const uint32_t *)boot_render_guest(r, va, n);
}
/* render-38d: 1 = execute this indirect command on GPU-read arguments (host_gpu_render.h, indirect commands), 0 = the CPU read above. */
static inline int boot_nvn_ind_gpu(BootRender *r, uint32_t kind, uint32_t mode, uint32_t index_type, uint64_t va) {
    static int logged, expand = -1; static unsigned unreadable_logged;
    if (!logged) { logged = 1; boot_draw_log("nvn native: indirect draws and dispatches%s (TOTK_INDIRECT mode %llu, 0 = cpu) %llu",
                                             boot_render_ind_mode() ? " on GPU-read arguments, ExecuteIndirect (render-38d)" : " read on the CPU after a queue wait", (uint64_t)boot_render_ind_mode(), 0); }
    ++boot_nvn_ind_seen[kind];
    if (!boot_render_ind_mode()) { ++boot_nvn_ind_why[0]; return 0; }
    if (kind != BOOT_RENDER_IND_DISPATCH) {
        if (expand < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_INSTANCE_EXPAND", v, sizeof v); expand = k && k < sizeof v && v[0] == '1'; }
        /* topologies boot_render_draw expands on the CPU (line loop 2, fans 6, quads 7, quad strips 8, polygons 9, the rest
         * unsupported either way) and 8-bit indices (widened on the CPU) need first/count on the CPU */
        if (mode >= 6 || mode == 2 || (kind == BOOT_RENDER_IND_INDEXED && index_type == 0) || expand) { ++boot_nvn_ind_why[1]; return 0; }
    }
    if (!boot_render_ind_args_ok(r, va, kind)) {
        if (++boot_nvn_ind_why[2], unreadable_logged < 8) { ++unreadable_logged; boot_draw_log("nvn native: indirect arguments%s at %llx not GPU-readable in place, CPU read (%llu)", "", va, boot_nvn_ind_why[2]); }
        return 0;
    }
    if (!boot_render_ind_open(r)) { ++boot_nvn_ind_why[3]; return 0; }
    return 1;
}
/* Driver texture/sampler pool object {memory pool, offset, count} -> descriptor GPU address. */
static inline uint64_t boot_nvn_exec_pool_va(BootContext *c, uint64_t self, const uint8_t *pool_obj, uint32_t *count) {
    uint64_t mem, off, va = 0; uint32_t n; memcpy(&mem, pool_obj, 8); memcpy(&off, pool_obj + 8, 8); memcpy(&n, pool_obj + 16, 4);
    if (!mem) return 0;
    if (!boot_ax_obj_word(mem + 0x18, &va) && (boot_ax_live(BOOT_AX_L_POOLWORD, 8), !boot_host_read(c, self, mem + 0x18, &va, 8))) return 0;   /* async-xlate: on X the word t2 captured */
    *count = n; return va + off;
}

/* Render enable (nvnCommandBufferSetRenderEnable / SetRenderEnableConditional). While rendering is disabled the 3D
 * class discards draws and clears (clb197.h SET_RENDER_ENABLE_C; Ryujinx DrawManager Draw / DrawIndirect / Clear
 * return early on ConditionalRenderEnabled.False); copies, buffer clears, reports and compute are not affected. The conditional form compares the
 * counter words of the two NVNcounterData at the address (NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL 0 /
 * RENDER_IF_NOT_EQUAL 1); GPU-ordered stores into that memory complete before it is read. Both commands were
 * ignored before. The play-71 title traces (frames/dt71b) show that the presents whose light accumulation pair is
 * cleared again right before the local-light draws are exactly the dark ones (12 of 12 dark, 0 of 12 lit) and that
 * the game records that "reset" sequence only on dynamic-resolution transitions: executing rendering the game had
 * disabled darkened the mural for one to seven presents at every cut. This skips only work the game itself turned
 * off, as the rasterizer-discard path does. TOTK_RENDER_ENABLE=0 ignores both commands again (A/B); =2 honours
 * SetRenderEnable but treats the conditional form as enabled. */
static inline int boot_nvn_render_mode(void) {
    static int v = -1;
    if (v < 0) { char s[8]; DWORD n = GetEnvironmentVariableA("TOTK_RENDER_ENABLE", s, sizeof s); v = n && n < sizeof s && s[0] == '0' ? 0 : n && n < sizeof s && s[0] == '2' ? 2 : 1; }
    return v;
}
/* render-38b: the counters a conditional render compares are written by our own ReportCounter, whose values are
 * computed on the CPU at record time. A shadow of each report (address -> the 16 bytes stored) answers the condition
 * without boot_render_cpu_read, which executed the open list and waited for the GPU when the store was still
 * recorded: play-74 did ~40 such evaluations per frame (one per draw/clear under a condition, ~15 GPU syncs per
 * frame) and fell to 5-8 fps. The result is also cached until another report is recorded. A shadow miss (memory
 * the game wrote another way) keeps the old exact path. TOTK_RENDER_COND_SHADOW=0 disables the shadow. */
#define BOOT_NVN_RSHADOW 1024
static struct { uint64_t addr, v[2]; } boot_nvn_rshadow[BOOT_NVN_RSHADOW];
static uint64_t boot_nvn_rshadow_gen = 1, boot_nvn_rshadow_hits, boot_nvn_rshadow_misses;
static inline unsigned boot_nvn_rshadow_slot(uint64_t a) { return (unsigned)((a >> 4) ^ (a >> 14)) & (BOOT_NVN_RSHADOW - 1); }
static inline void boot_nvn_rshadow_put(uint64_t a, const uint64_t v[2]) {
    unsigned k = boot_nvn_rshadow_slot(a); boot_nvn_rshadow[k].addr = a; boot_nvn_rshadow[k].v[0] = v[0]; boot_nvn_rshadow[k].v[1] = v[1]; ++boot_nvn_rshadow_gen;
}
static inline void boot_nvn_rshadow_forget(uint64_t va, uint64_t size) {   /* other writes over report memory */
    if (!size) return;
    for (unsigned k = 0; k < BOOT_NVN_RSHADOW; ++k) if (boot_nvn_rshadow[k].addr && boot_nvn_rshadow[k].addr < va + size && va < boot_nvn_rshadow[k].addr + 16) boot_nvn_rshadow[k].addr = 0;
    ++boot_nvn_rshadow_gen;
}
static inline int boot_nvn_rshadow_get(uint64_t a, uint64_t *v0) {
    unsigned k = boot_nvn_rshadow_slot(a); if (!a || boot_nvn_rshadow[k].addr != a) return 0; *v0 = boot_nvn_rshadow[k].v[0]; return 1;
}
static inline int boot_nvn_rshadow_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD k = GetEnvironmentVariableA("TOTK_RENDER_COND_SHADOW", t, sizeof t); v = !(k && k < sizeof t && t[0] == '0'); } return v;
}
/* render-38c: a conditional render is decided on the GPU by default (host_nvn_predicate.h): this then arms
 * predication for the draw/clear about to be recorded and returns 1 (the draw is recorded and counted, the GPU skips
 * it if the condition fails). The CPU evaluation below remains the fallback and TOTK_RENDER_COND=cpu. Callers disarm
 * after the draw/clear (and every record starts disarmed). */
static inline int boot_nvn_render_on(BootRender *r, BootNvnExec *e) {
    boot_render_pred.armed = 0;
    int mode = boot_nvn_render_mode();
    if (!mode) return 1;
    if (!e->render_cond_mode) return (int)e->render_enable;
    if (mode == 2) return 1;
    if (boot_nvn_render_cond_mode() && boot_nvn_pred_arm(r, e)) return 1;   /* render-38c: decided on the GPU */
    static uint64_t cache_addr, cache_gen; static uint32_t cache_mode; static int cache_on;
    if (cache_gen == boot_nvn_rshadow_gen && cache_addr == e->render_cond_addr && cache_mode == e->render_cond_mode) return cache_on;
    uint64_t a = 0, b = 0; int on = 1, shadowed = 0;
    ++e->render_cond_evals;
    if (boot_nvn_rshadow_on() && boot_nvn_rshadow_get(e->render_cond_addr, &a) && boot_nvn_rshadow_get(e->render_cond_addr + 16, &b)) {
        shadowed = 1; ++boot_nvn_rshadow_hits; on = e->render_cond_mode == 1 ? a == b : a != b;
    } else if (++boot_nvn_rshadow_misses, boot_nvn_pred_cpu_read(r, e->render_cond_addr)) {   /* recorded GPU writes of the counters have completed */
        const uint8_t *p = boot_render_guest(r, e->render_cond_addr, 32);
        if (p) { memcpy(&a, p, 8); memcpy(&b, p + 16, 8); on = e->render_cond_mode == 1 ? a == b : a != b; }
    }
    if (shadowed) { cache_addr = e->render_cond_addr; cache_gen = boot_nvn_rshadow_gen; cache_mode = e->render_cond_mode; cache_on = on; }
    static unsigned re_logged;
    if (re_logged < 16) { ++re_logged; boot_draw_log("nvn native: conditional render%s: counters %llx %llx", on ? " on" : " off", a, b); }
    return on;
}

/* tiles-01 (diagnostic, opt-in TOTK_TILE_PROBE=1; off: one cached test per draw, clear and dispatch). The cave's
 * black 64x64 blocks are tiles of the tiled lighting pass that no variant shaded. That pass keeps per-tile words in
 * one storage buffer of 0xe5e0 bytes (tables at +0x1c0/+0x41c0/+0x81c0, 16 bytes per tile): a 1x1x1 compute
 * (66d5d23e) zeroes them, full-screen G-buffer classification pixel programs (c2741faf, 328097b7, df3b7214) set
 * words 0..2 of the +0x41c0 table to 1, and the instanced tile-quad variants (count 6, one instance per tile) cull
 * their quad by those words in the vertex program (383cb2bb/950221ef: w0 == 1 || (w1 == 1 && w2 == 1);
 * 4e0dd4d5/fd64e61d/4caf841b: w3) while their pixel programs set w3. This logs, per present, the order of that
 * work in the native command stream together with the game's render-enable state (render-38: was the tile pass, or
 * a clear of its target, inside a sequence the game disabled?) and, every 300 presents (the frames\frame-N.bmp
 * cadence, same N) and for the first two tile frames, the +0x41c0 words of every tile after the frame's GPU work
 * completed, as a grid in screen order (row 0 = top), one hex digit per tile: 1 = w0, 2 = w1, 4 = w2, 8 = w3 ('?'
 * = a word other than 0/1). A black block whose digit is 0, or 2/4 alone, was never selected by any variant
 * (classification/reset/order); one whose digit selects a variant was drawn and lost its light afterwards (a later
 * clear or draw, e.g. one the game disabled). Tokens: '|' command buffer, E0/E1 SetRenderEnable(false/true),
 * Q1/Q2 SetRenderEnableConditional (1 if equal, 2 if not equal), R dispatch with the buffer bound, C draw with it
 * bound to the pixel stage only, V<instances>@<vertex program address> draw with it bound to the vertex stage,
 * L clear of the target the last V drew to, o<n> other draws/clears, x<n> other draws/clears the game disabled;
 * prefix '!' = the game had rendering disabled (dropped unless TOTK_RENDER_ENABLE=0), '~' = rasterizer discard.
 * Native only: nothing decoded from command buffers, no GPU work added or skipped; the grid is read from guest
 * memory after the queue went idle (once per 300 presents), as draw-trace's tileflags lines are. */
#define BOOT_TILE_PROBE_BUFFER 0xe5e0u
typedef struct BootTileProbe {
    char seq[2048]; unsigned len, other, other_off, events, disabled;
    uint64_t base, light, frames, disabled_frames; uint32_t inst, tiles_x, tiles_y, candidates;
} BootTileProbe;
static BootTileProbe boot_tile_probe;
static inline int boot_tile_probe_on(void) {
    static int v = -1;
    if (v < 0) { char s[8]; DWORD n = GetEnvironmentVariableA("TOTK_TILE_PROBE", s, sizeof s); v = n && n < sizeof s && s[0] == '1'; }
    return v;
}
static inline void boot_tile_probe_put(const char *t) {
    BootTileProbe *p = &boot_tile_probe;
    size_t n = strlen(t);
    if (p->len + n + 4 >= sizeof p->seq) { if (p->len + 4 < sizeof p->seq) { memcpy(p->seq + p->len, "...", 4); p->len += 3; } return; }
    memcpy(p->seq + p->len, t, n + 1); p->len += (unsigned)n;
}
static inline void boot_tile_probe_runs(void) {   /* pending counts of other work, before the next token */
    BootTileProbe *p = &boot_tile_probe; char t[24];
    if (p->other) { snprintf(t, sizeof t, " o%u", p->other); boot_tile_probe_put(t); p->other = 0; }
    if (p->other_off) { snprintf(t, sizeof t, " x%u", p->other_off); boot_tile_probe_put(t); p->other_off = 0; }
}
static inline void boot_tile_probe_token(const char *t) { boot_tile_probe_runs(); boot_tile_probe_put(" "); boot_tile_probe_put(t); }
/* storage buffer binding of the tile buffer at an NVN stage (0 vertex, 1 fragment, 5 compute) */
static inline uint64_t boot_tile_probe_bound(const BootNvnExec *e, unsigned stage) {
    for (unsigned i = 0; i < 16; ++i) if (e->ssbo[stage][i] && e->ssbo_size[stage][i] == BOOT_TILE_PROBE_BUFFER) return e->ssbo[stage][i];
    return 0;
}
/* the game's own render-enable state, also with TOTK_RENDER_ENABLE=0/2 (`on`: what boot_nvn_render_on returned) */
static inline int boot_tile_probe_game_on(BootRender *r, BootNvnExec *e, int on) {
    if (!e->render_cond_mode) return (int)e->render_enable;
    if (boot_nvn_render_mode() == 1 && !boot_render_pred.armed) return on;   /* render-38c: armed = the GPU decides, read it here */
    uint64_t a = 0, b = 0;
    if (!boot_render_cpu_read(r, e->render_cond_addr, 32)) return 1;
    const uint8_t *q = boot_render_guest(r, e->render_cond_addr, 32);
    if (!q) return 1;
    memcpy(&a, q, 8); memcpy(&b, q + 16, 8);
    return e->render_cond_mode == 1 ? a == b : a != b;
}
static inline void boot_tile_probe_draw(BootRender *r, BootNvnExec *e, int on, int discard, uint32_t count, uint32_t instances) {
    BootTileProbe *p = &boot_tile_probe;
    int game = 1;
    if (!discard && (!e->render_enable || e->render_cond_mode)) game = boot_tile_probe_game_on(r, e, on);
    uint64_t vs = boot_tile_probe_bound(e, 0), ps = vs ? 0 : boot_tile_probe_bound(e, 1);
    if (!vs && !ps) {
        if (count == 6 && instances >= 64 && p->candidates < 4) {   /* a tile-shaped draw on another buffer size: report it */
            for (unsigned i = 0; i < 16; ++i) if (e->ssbo[0][i] && e->ssbo_size[0][i] >= 0x41c0 + 16ull * instances) {
                ++p->candidates; boot_draw_log("tile probe: tile-shaped draw%s (%llu instances) with vertex storage buffer size %llx", "", instances, e->ssbo_size[0][i]); break; }
        }
        if (game) ++p->other; else ++p->other_off;
        return;
    }
    char t[64]; const char *pre = discard ? "~" : game ? "" : "!";
    if (vs) {
        uint64_t code = 0;
        for (unsigned k = 0; e->has_prog && k < e->prog.count; ++k) if (e->prog.stage[k].kind == 1) code = e->prog.stage[k].data;
        snprintf(t, sizeof t, "%sV%u@%llx", pre, instances, (unsigned long long)(code & 0xffffff));
        p->base = vs; p->inst = instances; p->light = e->rt_count ? e->rt[0].address : 0; p->tiles_x = p->tiles_y = 0;
        const uint8_t *u = e->ubo[0][1] && e->ubo_size[0][1] >= 0x930 ? boot_render_guest(r, e->ubo[0][1] + 0x928, 8) : NULL;   /* c4[146].zw */
        if (u) { memcpy(&p->tiles_x, u, 4); memcpy(&p->tiles_y, u + 4, 4); }
    } else snprintf(t, sizeof t, "%sC", pre);
    boot_tile_probe_token(t); ++p->events; if (!game) ++p->disabled;
}
static inline void boot_tile_probe_clear(BootRender *r, BootNvnExec *e, int on, uint32_t index) {
    BootTileProbe *p = &boot_tile_probe;
    int game = 1;
    if (!e->render_enable || e->render_cond_mode) game = boot_tile_probe_game_on(r, e, on);
    if (index < 8 && p->light && e->rt[index].address == p->light) { boot_tile_probe_token(game ? "L" : "!L"); if (!game) ++p->disabled; }
    else if (game) ++p->other; else ++p->other_off;
}
static inline void boot_tile_probe_present(BootRender *r, uint64_t present) {
    BootTileProbe *p = &boot_tile_probe;
    boot_tile_probe_runs();
    int periodic = present % 300 == 0;
    if (p->events && (p->frames < 2 || periodic || (p->disabled && p->disabled_frames < 32))) {
        boot_draw_log("tile probe:%s (present %llu, submission %llu)", p->seq, present, boot_render_exec_id);
        if (p->disabled) ++p->disabled_frames;
        if (p->base && p->inst && p->inst <= 4096 && (p->frames < 2 || periodic) && boot_render_idle(r)) {
            const uint8_t *g = boot_render_guest(r, p->base + 0x41c0, 16ull * p->inst);
            unsigned cols = p->tiles_x && p->tiles_x <= 128 && (uint64_t)p->tiles_x * p->tiles_y == p->inst ? p->tiles_x : 16, sel = 0, w3 = 0, none = 0;
            char row[132], head[160];
            if (!g) boot_draw_log("tile probe: flags%s at %llx (%llu tiles) not contiguous in guest memory", "", p->base + 0x41c0, p->inst);
            snprintf(head, sizeof head, "%u tiles in rows of %u (grid %ux%u from c4[146].zw), buffer", p->inst, cols, p->tiles_x, p->tiles_y);
            for (uint32_t y = 0; g && y * cols < p->inst; ++y) {
                unsigned n = 0;
                for (uint32_t x = 0; x < cols && y * cols + x < p->inst; ++x) {
                    uint32_t w[4]; memcpy(w, g + 16ull * (y * cols + x), 16);
                    int bad = w[0] > 1 || w[1] > 1 || w[2] > 1 || w[3] > 1, d = (int)(w[0] | w[1] << 1 | w[2] << 2 | w[3] << 3);
                    if (w[0] == 1 || (w[1] == 1 && w[2] == 1)) ++sel;
                    if (w[3] == 1) ++w3;
                    if (!(w[0] | w[1] | w[2] | w[3])) ++none;
                    row[n++] = bad ? '?' : "0123456789abcdef"[d];
                }
                row[n] = 0;
                if (!y) boot_draw_log("tile probe: +0x41c0 words after the frame, %s %llx (digit w0|w1<<1|w2<<2|w3<<3, row 0 top), present %llu", head, p->base, present);
                boot_draw_log("tile probe:   %s  row %llu, present %llu", row, y, present);
            }
            if (g) { snprintf(head, sizeof head, " %u tiles selected by w0 or w1+w2, %u with w3 set, %u all zero; present", sel, w3, none);
                     boot_draw_log("tile probe:%s %llu (submission %llu)", head, present, boot_render_exec_id); }
        }
        ++p->frames;
    }
    p->len = 0; p->seq[0] = 0; p->events = p->disabled = 0; p->other = p->other_off = 0;
}

/* ---------------------------------------------------------------- overlap-work P4: early fence (host_nvn_overlap.h) */
/* Bindings carried into a submission from earlier ones (NVN queue state persists across command buffers). */
static inline void boot_ef_note_bound(const BootNvnExec *e) {
    if (!boot_ef.rec_on) return;
    for (unsigned i = 0; i < 16; ++i) boot_ef_note(e->vbuf[i], e->vbuf_size[i], 0);
    for (unsigned s = 0; s < 6; ++s) for (unsigned i = 0; i < 16; ++i) { boot_ef_note(e->ubo[s][i], e->ubo_size[s][i], 0); boot_ef_note(e->ssbo[s][i], e->ssbo_size[s][i], 1); }
}
/* Index buffers and indirect arguments a draw record reads on the GPU. */
static inline void boot_ef_note_draw(const BootNvnOps *o, int op, const uint64_t *x) {
    if (!boot_ef.rec_on) return;
    if (op == o->draw_elements || op == o->draw_elements_bv || op == o->draw_elements_inst) {
        uint32_t t = (uint32_t)x[2]; boot_ef_note(x[4], (uint64_t)(uint32_t)x[3] << (t < 2 ? t : 2), 0);
    } else if (op == o->draw_elements_ind) { boot_ef_note(x[3], UINT64_C(1) << 20, 0); boot_ef_note(x[4], 20, 0); }   /* index count on the GPU: 1 MiB */
    else if (op == o->draw_arrays_ind) boot_ef_note(x[2], 16, 0);
}
/* Wait for D3D12 fence value v without the renderer lock and outside guest code. 0 = timed out. */
static inline int boot_ef_value_wait(BootContext *c, BootGpu *g, uint64_t v, uint64_t timeout_ns) {
    ID3D12Fence *f = g->fence;
    if (ID3D12Fence_GetCompletedValue(f) >= v) return 1;
    if (!timeout_ns) return 0;
    DWORD ms = timeout_ns == UINT64_MAX || timeout_ns / 1000000 >= INFINITE ? INFINITE : (DWORD)(timeout_ns / 1000000);
    static _Thread_local HANDLE ev; if (!ev) ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    LONG depth = boot_ov_cs_release(); BootCore *k = boot_ov_leave_guest(c);
    uint64_t fq = boot_ov_freq(), t0 = boot_ov_qpc(); int ok = 1;
    while (ID3D12Fence_GetCompletedValue(f) < v) {   /* a stale completion event (earlier timed-out wait) only loops */
        DWORD left = INFINITE;
        if (ms != INFINITE) { uint64_t spent = (boot_ov_qpc() - t0) * 1000 / fq; if (spent >= ms) { ok = 0; break; } left = (DWORD)(ms - spent); }
        if (ev && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(f, v, ev))) WaitForSingleObject(ev, left); else Sleep(1);
    }
    uint64_t t1 = boot_ov_qpc();
    boot_ov_cs_reacquire(depth); boot_ov_enter_guest(k);
    ++boot_ef.prev_waits; boot_ef.prev_wait_qpc += t1 - t0;
    return ok;
}
/* nvnSyncWait of the frame waiter on the newest frame fence (host_nvn.h boot_nvn_sync_wait, renderer lock held, the
 * value not complete yet). 1 = satisfied (early), 0 = timed out, -1 = not taken: the ordinary wait. */
static inline int boot_ef_frame_wait(BootContext *c, uint64_t sync, uint64_t value, uint64_t timeout_ns) {
    if (!boot_ef_on() || !boot_ov_threads()) return -1;
    uint64_t me = c->tpidrro;
    AcquireSRWLockShared(&boot_ff.lock);
    int frame = boot_ff.frame_owner && me != boot_ff.frame_owner && sync == boot_ff.frame_sync && value == boot_ff.frame_value;
    ReleaseSRWLockShared(&boot_ff.lock);
    if (!frame) return -1;
    if (!boot_ef.waiter) boot_ef.waiter = me;
    if (boot_ef.waiter != me) { ++boot_ef.not_waiter; return -1; }   /* one frame waiter (nxport: guest-main only) */
    BootNvnExec *e = boot_nvn_exec(); BootWindow *w = e ? e->w : NULL; BootRender *r = w ? w->render : NULL; BootGpu *g = r ? r->gpu : NULL;
    if (!g || !g->fence || value > g->submitted) return -1;
#if !(TOTK_FASTMEM && TOTK_FM_FIXED)
    if (boot_ef_guard_on()) { ++boot_ef.full[BOOT_EF_FULL_NO_GUARD_BUILD]; return -1; }   /* the write guard needs the fixed view */
#endif
    if (value == boot_ef.early) { ++boot_ef.signals; return 1; }   /* this frame already returned early (guarded, or completed) */
    /* 1. at most one frame overlaps the next: the previous early-returned frame must have completed */
    uint64_t prev = boot_ef.prev;
    if (prev && prev < value && ID3D12Fence_GetCompletedValue(g->fence) < prev) {
        if (!boot_ef_value_wait(c, g, prev, timeout_ns)) return 0;
        AcquireSRWLockShared(&boot_ff.lock);   /* the renderer lock was released: still the newest frame of this renderer? */
        frame = sync == boot_ff.frame_sync && value == boot_ff.frame_value;
        ReleaseSRWLockShared(&boot_ff.lock);
        r = w->render; g = r ? r->gpu : NULL;
        if (!frame || !g || !g->fence) return -1;
    }
    if (boot_ef.early_qpc) { boot_ef.overlap_qpc += boot_ov_qpc() - boot_ef.early_qpc; boot_ef.early_qpc = 0; }
    boot_efg_release_done();   /* the previous frame's pages: writable again */
    if (ID3D12Fence_GetCompletedValue(g->fence) >= value) { boot_ef_reset_rec(); boot_ef.prev = value; return 1; }   /* done meanwhile */
    /* 2. what the write guard cannot cover takes the ordinary (full) wait */
    unsigned why = 0, every = boot_ef_idle_every();
    if (every && ++boot_ef.run >= every) { boot_ef.run = 0; why = BOOT_EF_FULL_PERIODIC; }   /* queue-idle housekeeping */
    if (!why) { BootRenderRtTable *T = boot_render_rtab(r); for (unsigned i = 0; i < T->count && !why; ++i) if (T->pitch_dirty[i]) why = BOOT_EF_FULL_PITCH; }
    if (!why && boot_render_gw_all) why = BOOT_EF_FULL_GW_ALL;
    if (!why && boot_ef_guard_on()) {   /* GPU writes to guest memory still pending: guarded as written ranges */
        uint64_t done = ID3D12Fence_GetCompletedValue(g->fence);
        boot_ef.rec_on = 1;
        for (unsigned i = 0; i < boot_render_gw_count; ++i) if (!boot_render_gw[i].fence || boot_render_gw[i].fence > done) boot_ef_note(boot_render_gw[i].va, boot_render_gw[i].size, 1);
        for (unsigned i = 0; i < boot_render_gt_npending; ++i) if (boot_render_gt_pending[i].fence > done && boot_render_gt_pending[i].end > boot_render_gt_pending[i].va)
            boot_ef_note(boot_render_gt_pending[i].va, boot_render_gt_pending[i].end - boot_render_gt_pending[i].va, 1);
        for (unsigned k = 0; k < boot_compute_pending && k < 64; ++k) {
            const BootComputeWrites *cw = boot_compute_pending_list[k]; if (!cw) continue;
            if (cw->wide) boot_ef.rec_overflow = 1;
            for (uint32_t j = 0; j < cw->count; ++j) boot_ef_note(cw->va[j], cw->size[j], 1);
        }
        if (boot_ef.rec_overflow) why = BOOT_EF_FULL_RECORD;
    }
    if (why) { ++boot_ef.full[why]; boot_ef_reset_rec(); boot_ef.prev = value; return -1; }
#if TOTK_FASTMEM && TOTK_FM_FIXED
    if (boot_ef_guard_on() && !boot_efg_protect(g->fence, value)) { ++boot_ef.full[BOOT_EF_FULL_PROTECT]; boot_ef.prev = value; return -1; }
#endif
    boot_ef_reset_rec();
    boot_ef.prev = boot_ef.early = value; boot_ef.early_qpc = boot_ov_qpc(); ++boot_ef.signals;
    return 1;
}
/* pso-work: read-ahead for parallel pipeline compiles (host_gpu_render_draw.h, boot_psoj). boot_nvn_exec_handle
 * notes where it is: the next record at each nesting depth and, for a CallCommands being executed, the nested
 * command buffers still to come. At the first pipeline miss of a submission the draw path calls boot_nvn_la_run,
 * which reads the rest of the submission from there with a copy of the bound state: the records that change
 * pipeline state update the copy (as boot_nvn_exec_handle does), draws give their state (boot_nvn_la_state: the
 * pipeline-relevant part of boot_nvn_exec_state, without its driver-buffer writes) and boot_render_pso_predict
 * queues the pipelines that exist nowhere yet. Nothing is executed, written or recorded: command memory and guest
 * memory are only read (boot_nvn_record_read pins a driver handle for this submission, as exec_handle would).
 * Bounded: 1M records, BOOT_PSOJ_WALK_JOBS jobs, BOOT_NVN_LA_MS of wall time. */
#define BOOT_NVN_LA_DEPTH 10
#define BOOT_NVN_LA_MS 500   /* includes loading the shaders the draws will load anyway */
#define BOOT_NVN_LA_SEEN 4096u
static struct {
    BootContext *c; uint64_t self; BootNvnExec *e; uint64_t exec; unsigned depth;
    uint64_t next[BOOT_NVN_LA_DEPTH]; const uint64_t *nest[BOOT_NVN_LA_DEPTH]; uint32_t nest_n[BOOT_NVN_LA_DEPTH], nest_i[BOOT_NVN_LA_DEPTH];
    uint64_t deadline, seen[BOOT_NVN_LA_SEEN]; unsigned jobs; int full;
} boot_nvn_la;
static inline void boot_nvn_la_at(unsigned depth, uint64_t next) {
    if (depth < BOOT_NVN_LA_DEPTH) { boot_nvn_la.depth = depth; boot_nvn_la.next[depth] = next; } }
static inline void boot_nvn_la_nest(unsigned depth, const uint64_t *nested, uint32_t n, uint32_t i) {
    if (depth < BOOT_NVN_LA_DEPTH) { boot_nvn_la.nest[depth] = nested; boot_nvn_la.nest_n[depth] = n; boot_nvn_la.nest_i[depth] = i; } }
static int boot_nvn_la_run(BootRender *r);
static inline void boot_nvn_la_begin(BootContext *c, uint64_t self, BootNvnExec *e) {
    boot_nvn_la.c = c; boot_nvn_la.self = self; boot_nvn_la.e = e; boot_nvn_la.exec = boot_render_exec_id; boot_nvn_la.depth = 0;
    memset(boot_nvn_la.nest_n, 0, sizeof boot_nvn_la.nest_n);
    if (boot_psoj_enabled()) boot_render_pso_lookahead_hook = boot_nvn_la_run;
}
/* The draw state fields a pipeline depends on, from the bound state (boot_nvn_exec_state's derivation). */
static inline int boot_nvn_la_state(const BootNvnExec *e, BootDrawState *s) {
    memset(s, 0, sizeof *s);
    if (!e->has_prog) return 0;
    const BootNvnProgramStage *vs = NULL, *ps = NULL;
    for (unsigned k = 0; k < e->prog.count; ++k) { if (e->prog.stage[k].kind == 1) vs = &e->prog.stage[k]; else if (e->prog.stage[k].kind == 5) ps = &e->prog.stage[k]; }
    if (!vs) return 0;
    uint64_t vs_code = vs->data + 0x30, ps_code = ps ? ps->data + 0x30 : 0;
    uint64_t base = ps && ps_code < vs_code ? ps_code : vs_code;
    base &= ~UINT64_C(0xffff);
    s->program_base = base; s->stage_enabled[1] = 1; s->stage_type[1] = 1; s->stage_offset[1] = (uint32_t)(vs_code - base);
    if (ps) { s->stage_enabled[5] = 1; s->stage_type[5] = 5; s->stage_offset[5] = (uint32_t)(ps_code - base); }
    s->rt_count = e->rt_count;
    for (unsigned i = 0; i < 8; ++i) {
        s->rt_map[i] = i; s->rt[i] = e->rt[i];
        s->rt[i].write_mask = (e->channel_mask >> (4 * i)) & 15;
    }
    s->zt = e->zt;
    s->window_origin_lower_left = !e->origin_upper_left; s->flip_y = 0;
    s->z_clip_zero_to_w = e->depth_mode ? 1 : 0;
    uint8_t d0 = e->ds[0];
    s->depth_test = d0 & 1; s->depth_write = (d0 >> 1) & 1; s->stencil_test = (d0 >> 2) & 1; s->depth_func = d0 >> 4;
    s->stencil_func = e->ds[4] & 15; s->stencil_fail = e->ds[4] >> 4; s->stencil_zfail = e->ds[5] >> 4; s->stencil_zpass = e->ds[5] & 15;
    s->stencil_ref = e->stencil_ref[0]; s->stencil_func_mask = e->stencil_vmask[0]; s->stencil_mask = e->stencil_mask[0];
    uint32_t cull = e->polygon & 3;
    s->cull_enable = cull != 0; s->cull_face = cull == 1 ? 0x404 : cull == 2 ? 0x405 : 0x408;
    s->front_face = (e->polygon >> 2) & 1 ? 0x901 : 0x900;
    s->depth_bias_fill = (e->polygon >> 15) & 1;
    s->depth_bias = e->poly_offset[1]; s->depth_bias_slope = e->poly_offset[0]; s->depth_bias_clamp = e->poly_offset[2];
    s->blend_per_target = 1;
    for (unsigned t = 0; t < 8; ++t) {
        BootBlend *b = &s->blend[t]; const uint8_t *o = e->blend[t];
        b->enabled = (e->color >> (8 + t)) & 1; b->separate_alpha = 1;
        b->color_op = o[3] & 15; b->alpha_op = o[3] >> 4;
        b->color_src = o[4]; b->color_dst = o[5]; b->alpha_src = o[6]; b->alpha_dst = o[7];
    }
    s->primitive_restart = e->restart; s->restart_index = e->restart_index;
    {   static int honour = -1; if (honour < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_DEPTH_CLAMP", v, sizeof v); honour = !(k && k < sizeof v && v[0] == '0'); }
        s->depth_clamp = honour ? e->depth_clamp : 0; }
    if (boot_nvn_shadow_clip(s)) s->depth_clamp = 0;   /* sun-work: as boot_nvn_exec_state (same pipeline) */
    return 1;
}
/* One draw of the read-ahead: skipped when an identical pipeline-relevant state was seen in this read-ahead. */
static inline void boot_nvn_la_draw(BootRender *r, const BootNvnExec *e, uint32_t mode, int indexed, uint32_t index_type) {
    if (e->raster_discard) return;
    if (!e->render_cond_mode && !e->render_enable && boot_nvn_render_mode()) return;   /* conditional rendering: assumed on */
    uint64_t h = UINT64_C(1469598103934665603);
#define BOOT_NVN_LA_MIX(p, n) do { const uint8_t *m_ = (const uint8_t *)(p); for (size_t i_ = 0; i_ < (n); ++i_) { h ^= m_[i_]; h *= UINT64_C(1099511628211); } } while (0)
    BOOT_NVN_LA_MIX(&e->prog.program, 8); BOOT_NVN_LA_MIX(&e->has_prog, sizeof e->has_prog);
    BOOT_NVN_LA_MIX(e->blend, sizeof e->blend); BOOT_NVN_LA_MIX(&e->color, 4); BOOT_NVN_LA_MIX(&e->channel_mask, 4); BOOT_NVN_LA_MIX(e->ds, 8);
    BOOT_NVN_LA_MIX(&e->polygon, 4); BOOT_NVN_LA_MIX(e->poly_offset, sizeof e->poly_offset); BOOT_NVN_LA_MIX(&e->restart, 4); BOOT_NVN_LA_MIX(&e->depth_clamp, 4);
    BOOT_NVN_LA_MIX(e->stencil_mask, 4); BOOT_NVN_LA_MIX(e->stencil_vmask, 4); BOOT_NVN_LA_MIX(&e->rt_count, 4);
    for (unsigned i = 0; i < 8; ++i) { uint32_t f[2] = {e->rt[i].format, e->rt[i].width && e->rt[i].height}; BOOT_NVN_LA_MIX(f, 8); }
    {   uint32_t z[3] = {e->zt.format, e->zt.width && e->zt.height, (uint32_t)indexed << 8 | (index_type & 255)}; BOOT_NVN_LA_MIX(z, 12); BOOT_NVN_LA_MIX(&mode, 4); }
#undef BOOT_NVN_LA_MIX
    if (!h) h = 1;
    for (unsigned i = (unsigned)h & (BOOT_NVN_LA_SEEN - 1), n = 0; n < 16; ++n, i = (i + 1) & (BOOT_NVN_LA_SEEN - 1)) {
        if (boot_nvn_la.seen[i] == h) return;
        if (!boot_nvn_la.seen[i]) { boot_nvn_la.seen[i] = h; break; }
    }
    static BootDrawState s;
    if (!boot_nvn_la_state(e, &s)) return;
    s.topology = mode; s.indexed = (uint32_t)indexed; s.index_size = index_type; s.count = 1;
    int q = boot_render_pso_predict(r, &s);
    if (q > 0 && ++boot_nvn_la.jobs >= BOOT_PSOJ_WALK_JOBS) boot_nvn_la.full = 1;
    if (q < 0) boot_nvn_la.full = 1;
}
/* One command buffer from `at` (nested ones in full). 1 at its end, 0 to stop the read-ahead. */
static inline int boot_nvn_la_walk(BootRender *r, BootNvnExec *e, uint64_t at, unsigned depth, uint8_t *scratch, size_t cap, uint64_t *left) {
    BootNvnOps *o = boot_nvn_ops();
    if (depth > 8) return 0;
    for (;;) {
        if (!*left || boot_nvn_la.full) return 0;
        if (!(--*left & 255)) { LARGE_INTEGER q; QueryPerformanceCounter(&q); if ((uint64_t)q.QuadPart > boot_nvn_la.deadline) return 0; }
        BootNvnRecord rec; uint64_t size = boot_nvn_record_read(boot_nvn_la.c, boot_nvn_la.self, at, &rec, scratch, cap);
        if (!size) return 0;
        if (rec.op == BOOT_NVN_OP_END) return 1;
        if (rec.op == BOOT_NVN_OP_JUMP) { at = rec.x[1]; continue; }
        at += size;
        int op = (int)rec.op; const uint64_t *x = rec.x;
        const uint8_t *b0 = rec.blocks > 0 ? rec.block[0].data : NULL; uint32_t b0n = rec.blocks > 0 ? rec.block[0].bytes : 0;
        if (op == o->bind_program) {
            const BootNvnProgram *p = boot_nvn_program_find(x[1]);
            if (p) { e->prog = *p; e->has_prog = 1; } else e->has_prog = 0;
        } else if (op == o->bind_blend) { if (b0n >= 8) memcpy(e->blend[b0[0] & 7], b0, 8); }
        else if (op == o->bind_color) { if (b0n >= 4) memcpy(&e->color, b0, 4); }
        else if (op == o->bind_channel) { if (b0n >= 4) memcpy(&e->channel_mask, b0, 4); }
        else if (op == o->bind_ds) { if (b0n >= 8) memcpy(e->ds, b0, 8); }
        else if (op == o->bind_polygon) { if (b0n >= 4) memcpy(&e->polygon, b0, 4); }
        else if (op == o->set_rts) {
            uint32_t k = x[1] < 8 ? (uint32_t)x[1] : 8; e->rt_count = k;
            memset(e->rt, 0, sizeof e->rt);
            const uint8_t *views = rec.blocks > 1 && rec.block[1].bytes >= 16 * (k + 1) ? rec.block[1].data : NULL;
            const uint8_t *objs = views && rec.block[1].bytes >= (16 + 0x80) * (k + 1) ? views + 16 * (k + 1) : NULL;
            for (uint32_t i = 0; i < k && rec.blocks > 0 && 8 * i + 8 <= rec.block[0].bytes; ++i) {
                uint64_t t; memcpy(&t, rec.block[0].data + 8 * i, 8);
                if (t) boot_nvn_texture_target_obj(boot_nvn_la.c, boot_nvn_la.self, t, objs ? objs + 0x80 * i : NULL, views ? views + 16 * i : NULL, &e->rt[i], NULL);
            }
            memset(&e->zt, 0, sizeof e->zt);
            if (x[4]) boot_nvn_texture_target_obj(boot_nvn_la.c, boot_nvn_la.self, x[4], objs ? objs + 0x80 * k : NULL, views ? views + 16 * k : NULL, NULL, &e->zt);
        }
        else if (op == o->set_stencil_mask) { if (x[1] & 1) e->stencil_mask[0] = (uint32_t)x[2]; if (x[1] & 2) e->stencil_mask[1] = (uint32_t)x[2]; }
        else if (op == o->set_stencil_vmask) { if (x[1] & 1) e->stencil_vmask[0] = (uint32_t)x[2]; if (x[1] & 2) e->stencil_vmask[1] = (uint32_t)x[2]; }
        else if (op == o->set_poly_offset) { for (unsigned k = 0; k < 3; ++k) e->poly_offset[k] = boot_nvn_f(rec.q[k]); }
        else if (op == o->set_restart) { e->restart = (uint32_t)(x[1] & 1); e->restart_index = (uint32_t)x[2]; }
        else if (op == o->set_depth_clamp) e->depth_clamp = (uint32_t)(x[1] & 1);
        else if (op == o->set_raster_discard) e->raster_discard = (uint32_t)(x[1] & 1);
        else if (op == o->set_render_enable) { e->render_enable = (uint32_t)(x[1] & 1); e->render_cond_mode = 0; }
        else if (op == o->set_render_enable_cond) { e->render_cond_mode = 1 + (uint32_t)(x[1] & 1); e->render_enable = 1; }
        else if (op == o->draw_arrays) { if (x[3]) boot_nvn_la_draw(r, e, (uint32_t)x[1], 0, 0); }
        else if (op == o->draw_arrays_inst) { if (x[3] && x[5]) boot_nvn_la_draw(r, e, (uint32_t)x[1], 0, 0); }
        else if (op == o->draw_elements || op == o->draw_elements_bv) { if (x[3]) boot_nvn_la_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2]); }
        else if (op == o->draw_elements_inst) { if (x[3] && x[7]) boot_nvn_la_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2]); }
        else if (op == o->draw_arrays_ind) boot_nvn_la_draw(r, e, (uint32_t)x[1], 0, 0);
        else if (op == o->draw_elements_ind) boot_nvn_la_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2]);
        else if (op == o->call_commands) {
            uint64_t nested[64]; uint32_t k = b0n / 8 < 64 ? b0n / 8 : 64; if (b0) memcpy(nested, b0, (size_t)k * 8);
            for (uint32_t i = 0; i < k; ++i) if (!boot_nvn_la_walk(r, e, nested[i], depth + 1, scratch, cap, left)) return 0;
        }
    }
}
/* boot_render_pso_lookahead_hook: the rest of the submission after the draw being executed. */
static int boot_nvn_la_run(BootRender *r) {
    if (boot_nvn_la.exec != boot_render_exec_id || !boot_nvn_la.e || boot_nvn_la.depth >= BOOT_NVN_LA_DEPTH) return 0;
    static BootNvnExec *x; static uint8_t *scratch; const size_t cap = 2 * 65536;
    if (!x) x = (BootNvnExec *)malloc(sizeof *x);
    if (!scratch) scratch = (uint8_t *)malloc(cap);
    if (!x || !scratch) return 0;
    memcpy(x, boot_nvn_la.e, sizeof *x);
    char err[sizeof r->error]; memcpy(err, r->error, sizeof err);   /* the derivation may fail and say why: not this draw's error */
    memset(boot_nvn_la.seen, 0, sizeof boot_nvn_la.seen); boot_nvn_la.jobs = 0; boot_nvn_la.full = 0;
    {   LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); boot_nvn_la.deadline = (uint64_t)q.QuadPart + (uint64_t)f.QuadPart * BOOT_NVN_LA_MS / 1000; }
    uint64_t left = UINT64_C(1) << 20; int go = 1;
    for (int d = (int)boot_nvn_la.depth; d >= 0 && go; --d) {
        go = boot_nvn_la_walk(r, x, boot_nvn_la.next[d], (unsigned)d, scratch, cap, &left);   /* the rest of this command buffer */
        if (go && d > 0)   /* then the parent's CallCommands: its command buffers still to come */
            for (uint32_t i = boot_nvn_la.nest_i[d - 1] + 1; go && i < boot_nvn_la.nest_n[d - 1]; ++i)
                go = boot_nvn_la_walk(r, x, boot_nvn_la.nest[d - 1][i], (unsigned)d, scratch, cap, &left);
    }
    memcpy(r->error, err, sizeof err);
    return (int)boot_nvn_la.jobs;
}

/* ---------------------------------------------------------------- nvnpar-work 2: parallel NVN translation (TOTK_NVN_PAR)
 * A submission is translated as the command buffers (recordings) it reaches: the submitted handle and every handle a
 * CallCommands names. Helper threads (TOTK_NVN_PAR_THREADS, 1-8, default 2) decode those recordings ahead of the render
 * thread, each one a unit keyed by its handle, in discovery order (a unit's CallCommands queue the units of the handles
 * it calls): the handle's resolution and every record up to its END, exactly as boot_nvn_record_read would return them
 * on the render thread. The render thread then plays a recording from its decoded unit (boot_nvn_exec_handle takes
 * the unit's next record when its address is the one it would read, else reads it itself as before), so the NVN state
 * changes, draws and everything else still run on the render thread, in the original order, with the original
 * functions: what moved off it is the record fetch and decode.
 * Exact: a unit holds only what reading the same address would give the render thread during this submit call:
 *   - a handle only as the submit call's frozen resolution (batch-a freeze: fixed for the call, the recording pinned by
 *     the render thread until the call ends); a handle not frozen (the live map, a new pin), another thread's call or
 *     the freeze being off stop the unit there (the render thread resolves it itself);
 *   - records only of the recording that frozen handle resolved to (a JUMP into another recording stops the unit):
 *     finished recordings never change, and a pinned one is not freed, so the locked fetch used here returns the same
 *     bytes, with the same bounds checks, as the render thread's own read (in place or locked);
 *   - the side effects of those reads on the render thread (freeze lookups and "moved since the call" counts, record
 *     read counts) are applied when the render thread takes the record; the helpers count nothing of the store's.
 * The decoded blocks are copies in a per-thread arena that lives until the submission's translation ended; the helpers
 * are stopped and idle before boot_nvn_exec_submit returns (before the call unpins its recordings).
 * Off (the default for now): TOTK_NVN_PAR unset or 0 decodes nothing; the counters ("nvn native: parallel
 * translation") say how many records the render thread took decoded, and why the rest were read by it. */
#define BOOT_NVN_PAR_UNITS 8192u
#define BOOT_NVN_PAR_MAX_THREADS 8
#define BOOT_NVN_PAR_UNIT_ITEMS (1u << 20)
#define BOOT_NVN_PAR_CHUNK ((size_t)4 << 20)
typedef struct BootNvnParItem { uint64_t at, size; BootNvnRecord rec; uint32_t flags, pad; } BootNvnParItem;
enum { BOOT_NVN_PAR_FROZEN = 1, BOOT_NVN_PAR_MOVED = 2, BOOT_NVN_PAR_READ = 4 };
enum { BOOT_NVN_PAR_FREE = 0, BOOT_NVN_PAR_QUEUED, BOOT_NVN_PAR_DECODING, BOOT_NVN_PAR_DONE, BOOT_NVN_PAR_STOPPED };
/* why a unit stopped before its END: handle not frozen, JUMP into another recording, a record the read would reject,
 * the item limit, the translation ended, out of memory */
enum { BOOT_NVN_PAR_WHY_HANDLE, BOOT_NVN_PAR_WHY_SLOT, BOOT_NVN_PAR_WHY_RECORD, BOOT_NVN_PAR_WHY_LIMIT, BOOT_NVN_PAR_WHY_STOP, BOOT_NVN_PAR_WHY_MEMORY, BOOT_NVN_PAR_WHYS };
typedef struct BootNvnParUnit { uint64_t entry; volatile LONG state; uint32_t n; const BootNvnParItem *items; } BootNvnParUnit;
typedef struct BootNvnParWorker {
    uint8_t **chunk; size_t *chunk_size; uint32_t nchunk, cur; size_t used;   /* byte arena: chunks never move, reused per submission */
    BootNvnParItem *tmp; uint32_t tmp_cap;
    uint64_t units, items, why[BOOT_NVN_PAR_WHYS];
} BootNvnParWorker;
static struct {
    volatile LONG started; unsigned threads; HANDLE wake; SRWLOCK lock;
    volatile LONG stop, busy, job; int call_op; DWORD tid;
    BootNvnParUnit unit[BOOT_NVN_PAR_UNITS]; uint32_t units; uint32_t queue[BOOT_NVN_PAR_UNITS]; uint32_t qhead, qtail; int full;
    BootNvnParWorker w[BOOT_NVN_PAR_MAX_THREADS];
    /* render thread: submissions run with helpers, records taken decoded, records read itself (no unit / unit not
     * decoded yet / unit exhausted or another address), units used, table full submissions */
    uint64_t jobs, taken, own_none, own_pending, own_other, units_used, fulls;
} boot_nvn_par;
static inline int boot_nvn_par_on(void) {
    static int v = -1; if (v < 0) { char t[8]; DWORD k = GetEnvironmentVariableA("TOTK_NVN_PAR", t, sizeof t); v = k && k < sizeof t && t[0] == '1'; }
    return v;
}
static inline uint32_t boot_nvn_par_hash(uint64_t k) { k ^= k >> 33; k *= UINT64_C(0xff51afd7ed558ccd); k ^= k >> 29; return (uint32_t)k & (BOOT_NVN_PAR_UNITS - 1); }
/* Lock held. The unit of `entry` (created and queued when `add`), or NULL. */
static inline BootNvnParUnit *boot_nvn_par_unit_locked(uint64_t entry, int add) {
    for (uint32_t j = boot_nvn_par_hash(entry), n = 0; n < BOOT_NVN_PAR_UNITS; j = (j + 1) & (BOOT_NVN_PAR_UNITS - 1), ++n) {
        BootNvnParUnit *u = &boot_nvn_par.unit[j];
        if (u->entry == entry && u->state != BOOT_NVN_PAR_FREE) return u;
        if (u->state == BOOT_NVN_PAR_FREE) {
            if (!add) return NULL;
            if (boot_nvn_par.units * 4 >= BOOT_NVN_PAR_UNITS * 3) { boot_nvn_par.full = 1; return NULL; }   /* load <= 3/4 */
            u->entry = entry; u->n = 0; u->items = NULL; u->state = BOOT_NVN_PAR_QUEUED; ++boot_nvn_par.units;
            boot_nvn_par.queue[boot_nvn_par.qtail++] = j;
            return u;
        }
    }
    return NULL;
}
static inline void *boot_nvn_par_alloc(BootNvnParWorker *me, size_t n) {
    n = (n + 15) & ~(size_t)15;
    while (me->cur < me->nchunk && me->used + n > me->chunk_size[me->cur]) { ++me->cur; me->used = 0; }
    if (me->cur == me->nchunk) {
        size_t sz = n > BOOT_NVN_PAR_CHUNK ? n : BOOT_NVN_PAR_CHUNK;
        uint8_t **c = (uint8_t **)realloc(me->chunk, (me->nchunk + 1) * sizeof *c); if (!c) return NULL; me->chunk = c;
        size_t *cs = (size_t *)realloc(me->chunk_size, (me->nchunk + 1) * sizeof *cs); if (!cs) return NULL; me->chunk_size = cs;
        if (!(me->chunk[me->nchunk] = (uint8_t *)malloc(sz))) return NULL;
        me->chunk_size[me->nchunk++] = sz; me->used = 0;
    }
    void *p = me->chunk[me->cur] + me->used; me->used += n;
    return p;
}
/* The locked read of boot_nvn_cmd_fetch, without the store's read counters. */
static inline int boot_nvn_par_fetch(uint64_t at, void *dst, size_t n) {
    uint32_t i = boot_nvn_slot_of(at); uint64_t off = at & 0xffffffffu; int ok = 0;
    BootNvnShadowStore *s = boot_nvn_shadow();
    AcquireSRWLockShared(&s->lock);
    if (i < s->count) {
        BootNvnSlot *t = boot_nvn_slot_at(s, i);
        AcquireSRWLockShared(&t->lock);
        if (t->next_free == UINT32_MAX && off + n <= t->len) { memcpy(dst, t->data + off, n); ok = 1; }
        ReleaseSRWLockShared(&t->lock);
    }
    ReleaseSRWLockShared(&s->lock);
    return ok;
}
/* boot_nvn_record_read's shadow branch (same checks, same fields), blocks copied into the arena. */
static inline uint64_t boot_nvn_par_decode_record(BootNvnParWorker *me, uint64_t at, BootNvnRecord *r, size_t cap, int *oom) {
    memset(r, 0, sizeof *r);
    uint32_t h; if (!boot_nvn_par_fetch(at, &h, 4)) return 0;
    r->op = h & 0xffff; r->words = h >> 16;
    if (r->op == BOOT_NVN_OP_END) return 4;
    if (r->op == BOOT_NVN_OP_JUMP) { uint32_t a[2]; if (!boot_nvn_par_fetch(at + 4, a, 8)) return 0; r->x[1] = a[0] | ((uint64_t)a[1] << 32); return 12; }
    if (r->op >= BOOT_NVN_NAME_COUNT || r->words * 4 < BOOT_NVN_RECORD_FIXED) return 0;
    uint8_t head[BOOT_NVN_RECORD_FIXED];
    if (!boot_nvn_par_fetch(at, head, sizeof head)) return 0;
    memcpy(&r->x[1], head + 4, 56); memcpy(r->q, head + 60, 16);
    uint64_t p = at + sizeof head, end = at + (uint64_t)r->words * 4; size_t used = 0;
    while (p < end && r->blocks < 2) {
        uint32_t bh[2]; if (!boot_nvn_par_fetch(p, bh, 8)) return 0;
        if (used + bh[1] > cap || p + 8 + bh[1] > end) return 0;
        uint8_t *d = (uint8_t *)boot_nvn_par_alloc(me, bh[1] ? bh[1] : 1);
        if (!d) { *oom = 1; return 0; }
        if (bh[1] && !boot_nvn_par_fetch(p + 8, d, bh[1])) return 0;
        r->block[r->blocks].arg = bh[0]; r->block[r->blocks].bytes = bh[1]; r->block[r->blocks].data = d;
        ++r->blocks; used += bh[1]; p += 8 + ((bh[1] + 3u) & ~3u);
    }
    return (uint64_t)r->words * 4;
}
static inline uint64_t boot_nvn_par_map_get(BootNvnHostMap *m, uint64_t k) {   /* boot_nvn_hmap_get without the lock-wait counters */
    uint64_t v = 0;
    AcquireSRWLockShared(&m->lock);
    if (k && m->cap) for (uint32_t j = boot_nvn_hmap_hash(k, m->cap); m->key[j]; j = (j + 1) & (m->cap - 1)) if (m->key[j] == k) { v = m->val[j]; break; }
    ReleaseSRWLockShared(&m->lock);
    return v;
}
static inline void boot_nvn_par_decode_unit(BootNvnParWorker *me, BootNvnParUnit *u) {
    uint64_t at = u->entry; uint32_t n = 0, slot = UINT32_MAX; unsigned why = BOOT_NVN_PAR_WHY_LIMIT; int done = 0, oom = 0;
    for (uint32_t steps = 0; steps < BOOT_NVN_PAR_UNIT_ITEMS; ++steps) {
        if (boot_nvn_par.stop) { why = BOOT_NVN_PAR_WHY_STOP; break; }
        if (n == me->tmp_cap) {
            uint32_t c2 = me->tmp_cap ? me->tmp_cap * 2 : 4096; BootNvnParItem *g = (BootNvnParItem *)realloc(me->tmp, (size_t)c2 * sizeof *g);
            if (!g) { why = BOOT_NVN_PAR_WHY_MEMORY; break; }
            me->tmp = g; me->tmp_cap = c2;
        }
        BootNvnParItem *it = &me->tmp[n]; memset(it, 0, sizeof *it); it->at = at;
        uint64_t size;
        if (!(at & BOOT_NVN_SHADOW)) {   /* a handle: the call's frozen resolution, as boot_nvn_freeze_resolve gives it */
            const BootNvnFzEnt *f = slot == UINT32_MAX ? boot_nvn_freeze_find(at) : NULL;
            if (!f || !f->shadow) { why = BOOT_NVN_PAR_WHY_HANDLE; break; }
            it->flags = BOOT_NVN_PAR_FROZEN | (boot_nvn_par_map_get(boot_nvn_handles(), at) != f->shadow ? BOOT_NVN_PAR_MOVED : 0);
            it->rec.op = BOOT_NVN_OP_JUMP; it->rec.words = 3; it->rec.x[1] = f->shadow; size = 12;
            slot = boot_nvn_slot_of(f->shadow);
        } else {
            if (boot_nvn_slot_of(at) != slot) { why = BOOT_NVN_PAR_WHY_SLOT; break; }   /* only the pinned recording */
            it->flags = BOOT_NVN_PAR_READ;
            size = boot_nvn_par_decode_record(me, at, &it->rec, 2 * 65536, &oom);
            if (!size) { why = oom ? BOOT_NVN_PAR_WHY_MEMORY : BOOT_NVN_PAR_WHY_RECORD; break; }
        }
        it->size = size; ++n;
        if (it->rec.op == BOOT_NVN_OP_END) { done = 1; break; }
        if (it->rec.op == BOOT_NVN_OP_JUMP) { at = it->rec.x[1]; continue; }
        at += size;
        if ((int)it->rec.op == boot_nvn_par.call_op && it->rec.blocks > 0) {   /* the units of the handles it calls, in order */
            uint64_t nested[64]; uint32_t k = it->rec.block[0].bytes / 8 < 64 ? it->rec.block[0].bytes / 8 : 64;
            memcpy(nested, it->rec.block[0].data, (size_t)k * 8);
            AcquireSRWLockExclusive(&boot_nvn_par.lock);
            for (uint32_t i = 0; i < k; ++i) if (nested[i] && !(nested[i] & BOOT_NVN_SHADOW)) boot_nvn_par_unit_locked(nested[i], 1);
            ReleaseSRWLockExclusive(&boot_nvn_par.lock);
        }
    }
    BootNvnParItem *items = n ? (BootNvnParItem *)boot_nvn_par_alloc(me, (size_t)n * sizeof *items) : NULL;
    if (n && !items) { n = 0; done = 0; why = BOOT_NVN_PAR_WHY_MEMORY; }
    if (n) memcpy(items, me->tmp, (size_t)n * sizeof *items);
    u->items = items; u->n = n; ++me->units; me->items += n;
    if (!done) ++me->why[why];
    InterlockedExchange(&u->state, done ? BOOT_NVN_PAR_DONE : BOOT_NVN_PAR_STOPPED);   /* published (full barrier) */
}
static DWORD WINAPI boot_nvn_par_main(void *arg) {
    BootNvnParWorker *me = (BootNvnParWorker *)arg;
    for (;;) {
        if (WaitForSingleObject(boot_nvn_par.wake, INFINITE) != WAIT_OBJECT_0) return 0;
        InterlockedIncrement(&boot_nvn_par.busy);
        for (unsigned idle = 0; !boot_nvn_par.stop && boot_nvn_par.job; ) {   /* a job may have started meanwhile: its tables are set */
            BootNvnParUnit *u = NULL;
            AcquireSRWLockExclusive(&boot_nvn_par.lock);
            while (boot_nvn_par.qhead < boot_nvn_par.qtail && !u) {
                BootNvnParUnit *x = &boot_nvn_par.unit[boot_nvn_par.queue[boot_nvn_par.qhead++]];
                if (x->state == BOOT_NVN_PAR_QUEUED) { x->state = BOOT_NVN_PAR_DECODING; u = x; }
            }
            ReleaseSRWLockExclusive(&boot_nvn_par.lock);
            if (u) { boot_nvn_par_decode_unit(me, u); idle = 0; continue; }
            if (++idle < 64) YieldProcessor(); else { SwitchToThread(); if (idle > 4096) Sleep(0); }
        }
        InterlockedDecrement(&boot_nvn_par.busy);   /* the event is reset when the job ends: wait for the next one */
    }
}
/* Render thread, before the translation of `handle`: helpers decode its units. 1 = a job runs. */
static inline int boot_nvn_par_begin(uint64_t handle) {
    if (!boot_nvn_par_on() || boot_nvn_dsnap_tls || !boot_nvn_dsnap.active || !boot_nvn_dsnap.fz_on || boot_nvn_dsnap.tid != boot_nvn_call_tid() || (handle & BOOT_NVN_SHADOW)) return 0;   /* async-xlate: X acts for the call's thread; stage 2: helpers would read the main table */
    if (!boot_nvn_par.started) {
        boot_nvn_par.started = 1; InitializeSRWLock(&boot_nvn_par.lock);
        char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_NVN_PAR_THREADS", v, sizeof v);
        unsigned want = k && k < sizeof v ? (unsigned)strtoul(v, NULL, 10) : 2; if (want < 1) want = 1; if (want > BOOT_NVN_PAR_MAX_THREADS) want = BOOT_NVN_PAR_MAX_THREADS;
        boot_nvn_par.wake = CreateEventW(NULL, TRUE, FALSE, NULL);
        for (unsigned i = 0; boot_nvn_par.wake && i < want; ++i) {
            HANDLE t = CreateThread(NULL, 0, boot_nvn_par_main, &boot_nvn_par.w[i], 0, NULL);
            if (!t) break;
            SetThreadPriority(t, GetThreadPriority(GetCurrentThread()));   /* ahead of the render thread: its priority */
            CloseHandle(t); ++boot_nvn_par.threads;
        }
        boot_draw_log("nvn native: parallel translation (TOTK_NVN_PAR)%s: %llu record decode threads %llu", "", boot_nvn_par.threads, 0);
    }
    if (!boot_nvn_par.threads) return 0;
    /* helpers idle (boot_nvn_par_end waited): reset the tables and arenas, then publish the job */
    memset(boot_nvn_par.unit, 0, sizeof boot_nvn_par.unit); boot_nvn_par.units = 0; boot_nvn_par.qhead = boot_nvn_par.qtail = 0; boot_nvn_par.full = 0;
    for (unsigned i = 0; i < boot_nvn_par.threads; ++i) { boot_nvn_par.w[i].cur = 0; boot_nvn_par.w[i].used = 0; }
    boot_nvn_par.call_op = boot_nvn_ops()->call_commands; boot_nvn_par.tid = boot_nvn_call_tid();
    boot_nvn_par_unit_locked(handle, 1);
    ++boot_nvn_par.jobs;
    InterlockedExchange(&boot_nvn_par.stop, 0); InterlockedExchange(&boot_nvn_par.job, 1);
    SetEvent(boot_nvn_par.wake);
    return 1;
}
/* Render thread, after the translation: helpers stopped and idle (the arenas and units are not used any more). */
static inline void boot_nvn_par_end(int running) {
    if (!running) return;
    InterlockedExchange(&boot_nvn_par.stop, 1);
    ResetEvent(boot_nvn_par.wake);
    for (unsigned spin = 0; boot_nvn_par.busy; ++spin) { if (spin < 256) YieldProcessor(); else SwitchToThread(); }   /* each checks the stop per record */
    InterlockedExchange(&boot_nvn_par.job, 0);
    if (boot_nvn_par.full) ++boot_nvn_par.fulls;
}
/* Render thread: the decoded unit of `handle` (decoded or stopped), NULL when there is none (yet). */
static inline const BootNvnParUnit *boot_nvn_par_take(uint64_t handle) {
    if (!boot_nvn_par.job) return NULL;
    AcquireSRWLockShared(&boot_nvn_par.lock);
    const BootNvnParUnit *u = (handle & BOOT_NVN_SHADOW) ? NULL : boot_nvn_par_unit_locked(handle, 0);
    ReleaseSRWLockShared(&boot_nvn_par.lock);
    if (!u) { ++boot_nvn_par.own_none; return NULL; }
    LONG st = __atomic_load_n(&u->state, __ATOMIC_ACQUIRE);
    if (st != BOOT_NVN_PAR_DONE && st != BOOT_NVN_PAR_STOPPED) { ++boot_nvn_par.own_pending; return NULL; }
    ++boot_nvn_par.units_used;
    return u;
}
/* Render thread: record `k` of unit `u` when it is the one at `at` (else NULL: the caller reads it), with the side
 * effects boot_nvn_record_read would have had for it. */
static inline const BootNvnParItem *boot_nvn_par_item(const BootNvnParUnit **pu, uint32_t *k, uint64_t at) {
    const BootNvnParUnit *u = *pu;
    if (!u) return NULL;
    if (*k >= u->n || u->items[*k].at != at) { *pu = NULL; ++boot_nvn_par.own_other; return NULL; }
    const BootNvnParItem *it = &u->items[(*k)++];
    if (it->flags & BOOT_NVN_PAR_FROZEN) { ++boot_nvn_dsnap.fz_reads; if (it->flags & BOOT_NVN_PAR_MOVED) ++boot_nvn_dsnap.fz_moved; }   /* boot_nvn_freeze_resolve */
    if (it->flags & BOOT_NVN_PAR_READ) ++boot_nvn_rl()->n[BOOT_NVN_RL_READS];   /* boot_nvn_record_read */
    ++boot_nvn_par.taken;
    return it;
}
static inline void boot_nvn_par_format(char *out, size_t cap) {
    uint64_t units = 0, items = 0, why[BOOT_NVN_PAR_WHYS] = {0};
    for (unsigned i = 0; i < boot_nvn_par.threads; ++i) { units += boot_nvn_par.w[i].units; items += boot_nvn_par.w[i].items; for (unsigned j = 0; j < BOOT_NVN_PAR_WHYS; ++j) why[j] += boot_nvn_par.w[i].why[j]; }
    snprintf(out, cap, " (TOTK_NVN_PAR %s, %u threads): translations with helpers %llu, records taken decoded %llu, read by the render thread: no unit %llu, unit not decoded yet %llu,"
             " unit exhausted or another address %llu; units used %llu, decoded %llu (%llu records), stopped early: handle not frozen %llu, other recording %llu, rejected record %llu,"
             " limit %llu, translation ended %llu, memory %llu; unit table full %llu", boot_nvn_par_on() ? "on" : "off", boot_nvn_par.threads,
             (unsigned long long)boot_nvn_par.jobs, (unsigned long long)boot_nvn_par.taken, (unsigned long long)boot_nvn_par.own_none, (unsigned long long)boot_nvn_par.own_pending,
             (unsigned long long)boot_nvn_par.own_other, (unsigned long long)boot_nvn_par.units_used, (unsigned long long)units, (unsigned long long)items,
             (unsigned long long)why[0], (unsigned long long)why[1], (unsigned long long)why[2], (unsigned long long)why[3], (unsigned long long)why[4], (unsigned long long)why[5],
             (unsigned long long)boot_nvn_par.fulls);
}

/* call-bind watch (TOTK_CALL_BIND_AT_RECORD=2; host_nvn.h "call-bind"): a call whose callee was recorded again (or released) between
 * the caller's CallCommands and its submit is played live, as before, and logged: present, QPC, caller and callee, the two versions;
 * for the first 4 per second also what differs: the vertex buffers bound (address, size, stride; streams 0-3), the first draw of each
 * version, and the caller's next draw after the call (count, first, base vertex, index buffer; for a direct indexed draw the index
 * range it reads and the vertices it needs against each version's stream capacity, so the log shows whether the live version makes
 * that draw read past its buffers). At most 32 events per second are logged; the rest are counted (the 300-present "call bind" line). */
typedef struct BootNvnCbSum { uint64_t vb[4], vbsz[4], ix; uint32_t stride[4], vbm, stm, records, draws, ixtype, count, first; int32_t base; int op; } BootNvnCbSum;
static inline void boot_nvn_cb_sum(BootContext *c, uint64_t self, uint64_t at, unsigned max, int until_draw, BootNvnCbSum *s) {
    static uint8_t scratch[2 * 65536]; BootNvnOps *o = boot_nvn_ops();
    memset(s, 0, sizeof *s); s->op = -1;
    for (unsigned n = 0; at && n < max; ++n) {
        BootNvnRecord rec; uint64_t size = boot_nvn_record_read(c, self, at, &rec, scratch, sizeof scratch);
        if (!size || rec.op == BOOT_NVN_OP_END) break;
        if (rec.op == BOOT_NVN_OP_JUMP) { at = rec.x[1]; continue; }
        at += size; ++s->records;
        const int op = (int)rec.op; const uint64_t *x = rec.x;
        if (op == o->bind_vbuf && x[1] < 4) { s->vb[x[1]] = x[2]; s->vbsz[x[1]] = x[3]; s->vbm |= 1u << x[1]; }
        else if (op == o->bind_vstream && rec.blocks) {
            for (uint32_t i = 0; i < 4 && i < (uint32_t)x[1] && 8 * i + 4 <= rec.block[0].bytes; ++i) { memcpy(&s->stride[i], rec.block[0].data + 8 * i, 4); s->stm |= 1u << i; }
        } else if (op == o->draw_arrays || op == o->draw_arrays_inst || op == o->draw_elements || op == o->draw_elements_bv || op == o->draw_elements_inst ||
                   op == o->draw_arrays_ind || op == o->draw_elements_ind) {
            if (!s->draws++) {
                s->op = op;
                if (op == o->draw_arrays || op == o->draw_arrays_inst) { s->first = (uint32_t)x[2]; s->count = (uint32_t)x[3]; }
                else if (op == o->draw_elements || op == o->draw_elements_bv || op == o->draw_elements_inst) {
                    s->ixtype = (uint32_t)x[2]; s->count = (uint32_t)x[3]; s->ix = x[4]; s->base = op == o->draw_elements ? 0 : (int32_t)x[5];
                } else if (op == o->draw_elements_ind) { s->ixtype = (uint32_t)x[2]; s->ix = x[3]; }
            }
            if (until_draw) break;
        }
    }
}
static inline const char *boot_nvn_cb_op_name(int op) {
    BootNvnOps *o = boot_nvn_ops();
    return op < 0 ? "none" : op == o->draw_arrays ? "DrawArrays" : op == o->draw_arrays_inst ? "DrawArraysInstanced" : op == o->draw_elements ? "DrawElements" :
           op == o->draw_elements_bv ? "DrawElementsBaseVertex" : op == o->draw_elements_inst ? "DrawElementsInstanced" : op == o->draw_arrays_ind ? "DrawArraysIndirect" : "DrawElementsIndirect";
}
static inline size_t boot_nvn_cb_sum_text(char *p, size_t cap, const char *who, const BootNvnCbSum *s) {
    int n = snprintf(p, cap, " | %s: %u records, %u draws (first %s count %u base %d)", who, s->records, s->draws, boot_nvn_cb_op_name(s->op), s->count, s->base);
    size_t at = n > 0 ? (size_t)n : 0;
    for (unsigned i = 0; i < 4 && at < cap; ++i) if ((s->vbm | s->stm) & (1u << i)) {
        n = snprintf(p + at, cap - at, ", vb%u %llx+%llx stride %u", i, (unsigned long long)s->vb[i], (unsigned long long)s->vbsz[i], s->stride[i]);
        if (n > 0) at += (size_t)n;
    }
    return at < cap ? at : cap;
}
static inline void boot_nvn_cb_watch(BootContext *c, uint64_t self, BootRender *r, uint64_t caller, uint64_t h, uint64_t bound_sh, uint64_t live_sh, uint64_t next, unsigned depth) {
    static uint64_t win; static unsigned win_ev, win_diff;
    const uint64_t now = boot_nvn_qpc(), f = boot_nvn_qpf();
    InterlockedIncrement64(&boot_nvn_cb.watch_events);
    if (now - win >= f) { win = now; win_ev = win_diff = 0; }
    if (win_ev >= 32) { InterlockedIncrement64(&boot_nvn_cb.watch_over); return; }
    ++win_ev; InterlockedIncrement64(&boot_nvn_cb.watch_logged);
    char line[1600]; size_t at = 0; int n;
    n = snprintf(line, sizeof line, " (call-bind watch, TOTK_CALL_BIND_AT_RECORD 2): present %u qpc %llu: caller %llx (depth %u) calls %llx: bound version slot %u, live slot %s%u (played live)",
                 (unsigned)boot_nvn_ev_now, (unsigned long long)now, (unsigned long long)caller, depth, (unsigned long long)h, boot_nvn_slot_of(bound_sh),
                 live_sh ? "" : "none ", live_sh ? boot_nvn_slot_of(live_sh) : 0);
    if (n > 0) at = (size_t)n;
    if (win_diff < 4 && at < sizeof line) {
        ++win_diff; InterlockedIncrement64(&boot_nvn_cb.watch_diffs);
        BootNvnCbSum bs, ls, nd; const uint64_t lp = live_sh ? boot_nvn_handle_pin(h) : 0;   /* the live version is played right after: pinned now */
        boot_nvn_cb_sum(c, self, bound_sh, 4096, 0, &bs);
        if (lp) boot_nvn_cb_sum(c, self, lp, 4096, 0, &ls); else memset(&ls, 0, sizeof ls), ls.op = -1;
        boot_nvn_cb_sum(c, self, next, 64, 1, &nd);   /* the caller's next draw after this call */
        at += boot_nvn_cb_sum_text(line + at, sizeof line - at, "bound", &bs);
        if (at < sizeof line) at += boot_nvn_cb_sum_text(line + at, sizeof line - at, "live", &ls);
        if (at < sizeof line) {
            n = snprintf(line + at, sizeof line - at, " | caller's next draw: %s count %u first %u base %d index %llx type %u", boot_nvn_cb_op_name(nd.op), nd.count, nd.first, nd.base,
                         (unsigned long long)nd.ix, nd.ixtype);
            if (n > 0) at += (size_t)n;
        }
        int64_t vlo = 0, vhi = -1;   /* the vertices that draw fetches */
        if (nd.op >= 0 && nd.count && nd.ix && (nd.op == boot_nvn_ops()->draw_elements || nd.op == boot_nvn_ops()->draw_elements_bv || nd.op == boot_nvn_ops()->draw_elements_inst) && nd.count <= (1u << 22)) {
            const uint32_t isz = nd.ixtype == 0 ? 1u : nd.ixtype == 1 ? 2u : 4u; const uint8_t *p = boot_render_diag_guest(r, nd.ix, (uint64_t)nd.count * isz);
            if (p) {
                uint32_t lo = UINT32_MAX, hi = 0;
                for (uint32_t i = 0; i < nd.count; ++i) {
                    uint32_t v; if (isz == 1) v = p[i]; else if (isz == 2) { uint16_t w; memcpy(&w, p + 2 * (size_t)i, 2); v = w; } else memcpy(&v, p + 4 * (size_t)i, 4);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                vlo = (int64_t)lo + nd.base; vhi = (int64_t)hi + nd.base;
            }
        } else if (nd.op >= 0 && nd.count && !nd.ix) { vlo = nd.first; vhi = (int64_t)nd.first + nd.count - 1; }
        if (vhi >= vlo && at < sizeof line) {
            n = snprintf(line + at, sizeof line - at, " -> vertices %lld..%lld; capacity bound/live:", (long long)vlo, (long long)vhi);
            if (n > 0) at += (size_t)n;
            for (unsigned i = 0; i < 4 && at < sizeof line; ++i) {
                const uint32_t bstr = (bs.stm & (1u << i)) ? bs.stride[i] : (nd.stm & (1u << i)) ? nd.stride[i] : 0, lstr = (ls.stm & (1u << i)) ? ls.stride[i] : bstr;
                if (!((bs.vbm | ls.vbm) & (1u << i)) || !bstr || !lstr) continue;
                const int64_t bc = (int64_t)(bs.vbsz[i] / bstr), lc = (int64_t)(ls.vbsz[i] / lstr);
                n = snprintf(line + at, sizeof line - at, " s%u %lld/%lld%s", i, (long long)bc, (long long)lc, vhi >= lc && vhi < bc ? " LIVE PAST (bound inside)" : vhi >= lc ? " live past" : vhi >= bc ? " bound past" : "");
                if (n > 0) at += (size_t)n;
            }
        }
    }
    boot_draw_log("nvn native: call-bind watch%s %llu %llu", line, (uint64_t)boot_nvn_cb.watch_events, (uint64_t)boot_nvn_cb.watch_over);
}
static inline int boot_nvn_exec_handle(BootContext *c, uint64_t self, BootRender *r, BootNvnExec *e, uint64_t handle, unsigned depth) {
    BootNvnOps *o = boot_nvn_ops();
    static _Thread_local uint8_t scratch[2 * 65536];
    if (depth > 8) return 0;
    if (!depth && boot_tile_probe_on()) boot_tile_probe_token("|");   /* tiles-01 diagnostic: a submitted command buffer */
    if (!depth) boot_nvn_la_begin(c, self, e);   /* pso-work: read-ahead cursor */
    const BootNvnParUnit *par = boot_nvn_par_take(handle); uint32_t par_k = 0;   /* nvnpar-work 2: decoded by the helpers */
    for (uint64_t at = handle, n = 0; n < (UINT64_C(1) << 24); ++n) {
        BootNvnRecord rec; uint64_t size;
        const BootNvnParItem *pit = boot_nvn_par_item(&par, &par_k, at);
        if (pit) { rec = pit->rec; size = pit->size; }
        else size = boot_nvn_record_read(c, self, at, &rec, scratch, sizeof scratch);
        if (!size) {
            if (!n && at == handle && boot_nvn_ev_skip(handle, depth)) return 1;   /* rec-evict: a released handle used again: its commands skipped, logged */
            boot_draw_log("nvn native: bad record%s at %llx %llu", "", at, n); return 0;
        }
        if (boot_ax_chk_log) boot_ax_chk_rec(at, &rec, size);   /* async-xlate sentinel (TOTK_ASYNC_XLATE_CHECK=1): what this read gave X */
        if (rec.op == BOOT_NVN_OP_END) return 1;
        if (rec.op == BOOT_NVN_OP_JUMP) { at = rec.x[1]; continue; }
        at += size;
        boot_nvn_la_at(depth, at);   /* pso-work */
        boot_render_pred.armed = 0;   /* render-38c: predication is armed only for the draw/clear of one record */
        int op = (int)rec.op; const uint64_t *x = rec.x;
        const uint8_t *b0 = rec.blocks > 0 ? rec.block[0].data : NULL; uint32_t b0n = rec.blocks > 0 ? rec.block[0].bytes : 0;
        if (boot_frt_on()) boot_frt_op(o, op, x, b0n, boot_nvn_copy_t2b_op());   /* fence-read trace: other GPU writes into a watched range */
        if (op == o->dispatch || op == o->dispatch_ind || op == o->clear_buffer || op == boot_nvn_copy_b2t_op() || op == boot_nvn_copy_t2b_op())
            boot_render_fb_pb_sync(2);   /* batch-c: may write a target (or its memory): feedback copies are retaken (clears: boot_render_clear) */
        if (op == o->bind_program) {
            const BootNvnProgram *p = boot_nvn_program_find(x[1]);
            if (p) { e->prog = *p; e->has_prog = 1; }
            else { e->has_prog = 0; boot_draw_log("nvn native: program not observed at SetShaders%s %llx %llu", "", x[1], 0); }
        } else if (op == o->bind_blend) { if (b0n >= 8) memcpy(e->blend[b0[0] & 7], b0, 8); }
        else if (op == o->bind_color) { if (b0n >= 4) memcpy(&e->color, b0, 4); }
        else if (op == o->bind_channel) { if (b0n >= 4) memcpy(&e->channel_mask, b0, 4); }
        else if (op == o->bind_ds) { if (b0n >= 8) memcpy(e->ds, b0, 8); }
        else if (op == o->bind_polygon) { if (b0n >= 4) memcpy(&e->polygon, b0, 4); }
        else if (op == o->bind_ms) { }
        else if (op == o->bind_vattr) {
            uint32_t k = x[1] < 16 ? (uint32_t)x[1] : 16; e->n_vattr = k;
            if (b0) memcpy(e->vattr, b0, k * 4 <= b0n ? k * 4 : b0n);
        } else if (op == o->bind_vstream) {
            uint32_t k = x[1] < 16 ? (uint32_t)x[1] : 16; e->n_vstream = k;
            for (uint32_t i = 0; b0 && i < k && 8 * i + 8 <= b0n; ++i) { memcpy(&e->vstride[i], b0 + 8 * i, 4); memcpy(&e->vdivisor[i], b0 + 8 * i + 4, 4); }
        } else if (op == o->bind_vbuf) { if (x[1] < 16) { e->vbuf[x[1]] = x[2]; e->vbuf_size[x[1]] = x[3]; boot_ef_note(x[2], x[3], 0); } }
        else if (op == o->bind_ubo) { if (x[1] < 6 && x[2] < 16) { e->ubo[x[1]][x[2]] = x[3]; e->ubo_size[x[1]][x[2]] = x[4]; boot_ef_note(x[3], x[4], 0); } }
        else if (op == o->bind_ssbo) { if (x[1] < 6 && x[2] < 16) { e->ssbo[x[1]][x[2]] = x[3]; e->ssbo_size[x[1]][x[2]] = x[4]; boot_ef_note(x[3], x[4], 1); } }
        else if (op == o->bind_tex) { if (x[1] < 6 && x[2] < 32) e->tex[x[1]][x[2]] = (uint32_t)x[3]; }
        else if (op == boot_nvn_exec_deferred_op()) boot_nvn_exec_bind_deferred(c, self, e, x, at - size);   /* flashfix: the record's address keys the submit-call copy */
        else if (op == boot_nvn_copy_t2b_op()) { boot_nvn_pred_touch(); boot_nvn_exec_copy_tex_to_buf(c, self, r, x, b0, b0n, boot_nvn_copy_captured(&rec, 1)); }   /* render-38c: may write the counters */
        else if (op == boot_nvn_copy_b2t_op()) boot_nvn_exec_copy_buf_to_tex(c, self, r, x, b0, b0n, boot_nvn_copy_captured(&rec, 2));   /* movie-03 */
        else if (op == o->set_tex_pool) { if (b0n >= 20) e->tex_pool = boot_nvn_exec_pool_va(c, self, b0, &e->tex_count); }
        else if (op == o->set_smp_pool) { if (b0n >= 20) e->smp_pool = boot_nvn_exec_pool_va(c, self, b0, &e->smp_count); }
        else if (op == o->set_rts) {
            uint32_t k = x[1] < 8 ? (uint32_t)x[1] : 8; e->rt_count = k;
            memset(e->rt, 0, sizeof e->rt);
            /* block 1: the views' contents captured at record time, 16 bytes per colour target then the depth view */
            const uint8_t *views = rec.blocks > 1 && rec.block[1].bytes >= 16 * (k + 1) ? rec.block[1].data : NULL;
            /* then the texture objects as recorded, 0x80 bytes each (host_nvn.h boot_nvn_record_render_targets) */
            const uint8_t *objs = views && rec.block[1].bytes >= (16 + 0x80) * (k + 1) ? views + 16 * (k + 1) : NULL;
            for (uint32_t i = 0; i < k && rec.blocks > 0 && 8 * i + 8 <= rec.block[0].bytes; ++i) {
                uint64_t t; memcpy(&t, rec.block[0].data + 8 * i, 8);
                if (t && !boot_nvn_texture_target_obj(c, self, t, objs ? objs + 0x80 * i : NULL, views ? views + 16 * i : NULL, &e->rt[i], NULL)) boot_draw_log("nvn native: render target texture%s %llx %llu", "", t, i);
            }
            memset(&e->zt, 0, sizeof e->zt);
            if (x[4] && !boot_nvn_texture_target_obj(c, self, x[4], objs ? objs + 0x80 * k : NULL, views ? views + 16 * k : NULL, NULL, &e->zt)) boot_draw_log("nvn native: depth target texture%s %llx %llu", "", x[4], 0);
        } else if (op == o->set_vp) { for (unsigned k = 0; k < 4; ++k) e->vp[0][k] = (float)(int32_t)x[1 + k]; }
        else if (op == o->set_vps) {
            for (uint32_t i = 0; b0 && i < (uint32_t)x[2] && x[1] + i < 16 && 16 * i + 16 <= b0n; ++i) {
                float f[4]; memcpy(f, b0 + 16 * i, 16); boot_nvn_viewport_store(e, (uint32_t)x[1] + i, f); }   /* batch-3 N10: floats kept */
        } else if (op == o->set_scissor) { for (unsigned k = 0; k < 4; ++k) e->scissor[0][k] = (int32_t)x[1 + k]; }
        else if (op == o->set_scissors) {
            for (uint32_t i = 0; b0 && i < (uint32_t)x[2] && x[1] + i < 16 && 16 * i + 16 <= b0n; ++i) memcpy(e->scissor[x[1] + i], b0 + 16 * i, 16);
        } else if (op == o->set_depth_range) { e->depth_range[0][0] = boot_nvn_f(rec.q[0]); e->depth_range[0][1] = boot_nvn_f(rec.q[1]); }
        else if (op == o->set_depth_ranges) {
            for (uint32_t i = 0; b0 && i < (uint32_t)x[2] && x[1] + i < 16 && 8 * i + 8 <= b0n; ++i) memcpy(e->depth_range[x[1] + i], b0 + 8 * i, 8);
        } else if (op == o->set_blend_color) { if (b0n >= 16) memcpy(e->blend_color, b0, 16); }
        else if (op == o->set_stencil_ref) { if (x[1] & 1) e->stencil_ref[0] = (uint32_t)x[2]; if (x[1] & 2) e->stencil_ref[1] = (uint32_t)x[2]; }
        else if (op == o->set_stencil_mask) { if (x[1] & 1) e->stencil_mask[0] = (uint32_t)x[2]; if (x[1] & 2) e->stencil_mask[1] = (uint32_t)x[2]; }
        else if (op == o->set_stencil_vmask) { if (x[1] & 1) e->stencil_vmask[0] = (uint32_t)x[2]; if (x[1] & 2) e->stencil_vmask[1] = (uint32_t)x[2]; }
        else if (op == o->set_poly_offset) { for (unsigned k = 0; k < 3; ++k) e->poly_offset[k] = boot_nvn_f(rec.q[k]); }
        else if (op == o->set_restart) { e->restart = (uint32_t)(x[1] & 1); e->restart_index = (uint32_t)x[2]; }
        else if (op == boot_nvn_point_size_op()) boot_pt_note_size(rec.q[0]);   /* G2: in command order, as the GPU state */
        else if (op == o->set_depth_clamp) {   /* Fable deload analysis: was ignored, so D3D12 always clipped at near/far */
            static unsigned logged; uint32_t v = (uint32_t)(x[1] & 1);
            if (!(logged & (1u << v))) { logged |= 1u << v; boot_draw_log("nvn native: SetDepthClamp%s %llu first seen %llu", "", v, 0); }
            e->depth_clamp = v; }
        else if (op == o->set_raster_discard) { e->raster_discard = (uint32_t)(x[1] & 1); }
        else if (op == o->set_render_enable) {   /* (cmd, NVNboolean enable): replaces any conditional state */
            e->render_enable = (uint32_t)(x[1] & 1); e->render_cond_mode = 0;
            static unsigned re_logged;
            if (!e->render_enable && re_logged < 8) { ++re_logged; boot_draw_log("nvn native: SetRenderEnable(false)%s at event %llu (submission %llu)", "", boot_nvn_trace_ev, boot_render_exec_id); }
            if (boot_tile_probe_on()) boot_tile_probe_token(e->render_enable ? "E1" : "E0");   /* tiles-01 diagnostic */
        }
        else if (op == o->set_render_enable_cond) {   /* (cmd, NVNconditionalRenderMode mode, NVNbufferAddress counters) */
            e->render_cond_mode = 1 + (uint32_t)(x[1] & 1); e->render_cond_addr = x[2]; e->render_enable = 1;
            boot_ef_note(x[2], 32, 0);   /* overlap-work P4: the counters are read on the GPU */
            boot_nvn_pred_touch();   /* render-38c: read the counters again at the next draw/clear */
            if (boot_tile_probe_on()) boot_tile_probe_token(e->render_cond_mode == 1 ? "Q1" : "Q2");   /* tiles-01 diagnostic */
        }
        else if (op == o->barrier) { boot_tile89_native_barrier(e,x[1],at>=size?at-size:0);e->guest_barrier = 1; boot_render_fb_pb_barrier(); }   /* batch-c: a feedback copy sync point */
        else if (op == o->draw_arrays || op == o->draw_arrays_inst || op == o->draw_elements || op == o->draw_elements_bv ||
                 op == o->draw_elements_inst || op == o->draw_arrays_ind || op == o->draw_elements_ind) {
            boot_ef_note_draw(o, op, x);   /* overlap-work P4 */
            if (op == o->draw_arrays_ind || op == o->draw_elements_ind) ++boot_nvn_ind_records[op == o->draw_elements_ind ? BOOT_RENDER_IND_INDEXED : BOOT_RENDER_IND_DRAW];   /* render-38d */
            if (boot_tile_probe_on()) {   /* tiles-01 diagnostic: classify before the drops below */
                int direct = op != o->draw_arrays_ind && op != o->draw_elements_ind;
                uint32_t inst = op == o->draw_arrays_inst ? (uint32_t)x[5] : op == o->draw_elements_inst ? (uint32_t)x[7] : direct ? 1u : 0u;
                int on = e->raster_discard ? 1 : boot_nvn_render_on(r, e);
                boot_tile_probe_draw(r, e, on, (int)e->raster_discard, direct ? (uint32_t)x[3] : 0, inst);
                if (e->raster_discard) { ++e->discarded; continue; }
                if (!on) { ++e->render_off; continue; }
            } else {
            if (e->raster_discard) { ++e->discarded; continue; }   /* rasterizer discard: no fragments; no stream output in use */
            if (!boot_nvn_render_on(r, e)) { ++e->render_off; continue; }   /* rendering disabled by the game: the 3D class drops the draw */
            }
            if (op == o->draw_arrays) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 0, 0, 0, (uint32_t)x[2], (uint32_t)x[3], 0, 0, 1);
            else if (op == o->draw_arrays_inst) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 0, 0, 0, (uint32_t)x[2], (uint32_t)x[3], 0, (uint32_t)x[4], (uint32_t)x[5]);
            else if (op == o->draw_elements) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2], x[4], 0, (uint32_t)x[3], 0, 0, 1);
            else if (op == o->draw_elements_bv) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2], x[4], 0, (uint32_t)x[3], (int32_t)x[5], 0, 1);
            else if (op == o->draw_elements_inst) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2], x[4], 0, (uint32_t)x[3], (int32_t)x[5], (uint32_t)x[6], (uint32_t)x[7]);
            else {   /* indirect arguments in guest memory */
                int el = op == o->draw_elements_ind; uint64_t va = el ? x[4] : x[2];
                uint32_t kind = el ? BOOT_RENDER_IND_INDEXED : BOOT_RENDER_IND_DRAW;
                if (boot_nvn_ind_gpu(r, kind, (uint32_t)x[1], el ? (uint32_t)x[2] : 0, va)) {   /* render-38d: read by the GPU (ExecuteIndirect) */
                    memset(&boot_render_next_ind, 0, sizeof boot_render_next_ind); boot_render_next_ind.va = va; boot_render_next_ind.kind = kind;
                    /* first 0, count 1, one instance: placeholders; count, instances, first index/vertex, base vertex and base
                     * instance come from the arguments on the GPU */
                    if (el) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2], x[3], 0, 1, 0, 0, 1);
                    else boot_nvn_exec_draw(r, e, (uint32_t)x[1], 0, 0, 0, 0, 1, 0, 0, 1);
                    boot_render_next_ind.kind = 0;   /* not taken when the draw stopped before the renderer */
                } else {   /* CPU read: earlier work in this submission has completed or was CPU-written */
                    ++boot_nvn_ind_cpu[kind];
                    const uint32_t *a = boot_nvn_exec_indirect(r, e, va, el ? 20 : 16);
                    if (!a) { boot_draw_log("nvn native: indirect draw arguments%s %llx %llu", "", va, 0); continue; }
                    if (el) boot_nvn_exec_draw(r, e, (uint32_t)x[1], 1, (uint32_t)x[2], x[3], a[2], a[0], (int32_t)a[3], a[4], a[1]);
                    else boot_nvn_exec_draw(r, e, (uint32_t)x[1], 0, 0, 0, a[2], a[0], 0, a[3], a[1]);
                }
            }
            boot_render_pred.armed = 0;   /* render-38c */
        } else if (op == o->clear_color) {
            uint32_t col[4] = {0}; if (b0n >= 16) memcpy(col, b0, 16);
            int on = boot_nvn_render_on(r, e);
            if (boot_tile_probe_on()) boot_tile_probe_clear(r, e, on, (uint32_t)x[1]);   /* tiles-01 diagnostic */
            if (!on) ++e->render_off;   /* rendering disabled by the game: clears are dropped too */
            else boot_nvn_exec_clear(r, e, ((uint32_t)(x[3] & 15) << 2) | ((uint32_t)(x[1] & 15) << 6), col, 0, 0);
            boot_render_pred.armed = 0;   /* render-38c */
        } else if (op == o->clear_ds) {
            uint32_t value = (x[1] & 1 ? 1u : 0) | ((x[3] & 0xff) ? 2u : 0);
            int on = value ? boot_nvn_render_on(r, e) : 1;
            if (value && boot_tile_probe_on()) boot_tile_probe_clear(r, e, on, 8);   /* tiles-01 diagnostic (depth/stencil: "other") */
            if (value && !on) ++e->render_off;
            else if (value) boot_nvn_exec_clear(r, e, value, NULL, rec.q[0], (uint32_t)x[2]);
            boot_render_pred.armed = 0;   /* render-38c */
        } else if (op == o->update_ubo) {
            if (b0) { boot_nvn_pred_touch_range(e, x[1] + x[3], b0n); boot_nvn_exec_store(r, x[1] + x[3], b0, b0n); boot_ef_note(x[1] + x[3], b0n, 1); }   /* render-38c */
        } else if (op == o->clear_buffer) {
            boot_nvn_rshadow_forget(x[1], x[2]);   /* render-38b */
            boot_nvn_pred_touch_range(e, x[1], x[2]);   /* render-38c */
            boot_nvn_exec_fill(r, x[1], x[2], (uint32_t)x[3]); boot_ef_note(x[1], x[2], 1);
        } else if (op == o->report_value) { uint32_t v = (uint32_t)x[1]; boot_nvn_rshadow_forget(x[2], 4); boot_nvn_pred_touch_range(e, x[2], 4); boot_nvn_exec_store(r, x[2], &v, 4); boot_ef_note(x[2], 4, 1); }
        else if (op == o->report_counter) {
            /* {counter, timestamp} (types 0 TIMESTAMP and 0x10 TIMESTAMP_TOP in this game): the report clock of the
             * emulated path (host_gpu_sync_memory.h: QPC ns, or the guest clock when dilated); work recorded before
             * the report completes before the submission returns. Occlusion samples (type 1) are counted on the GPU
             * (batch-2, host_nvn_occl.h); the value below ("visible") stays for TOTK_OCCLUSION=fake and where no count exists. */
            /* Fable deload analysis (2026-10-20): the timestamp must be in GPU ticks (614.4 MHz, ns * 384/625, as the
             * emulated path does in host_gpu_sync_memory.h:65). Storing ns made every GPU interval the game measures
             * 1.63x too long (LASRecomp hit the same, runtime/nvn/vulkan/exec.c:97-113), and TOTK's dynamic resolution
             * visibly reacted. TOTK_GPU_TS=ns restores the old value; =fast reports a fixed 2 ms from the last TOP
             * (0x10) report to each bottom (0) report, for an A/B against GPU-time-driven game logic. The occlusion
             * count grows by 0x10000 per report, so both reset-then-read and difference readers see "visible".
             * G1: TOTK_GPU_TS=gpu (mode 3) has the GPU write TIMESTAMP reports with its own time in command order
             * (host_nvn_gpu_ts.h); =gpu-top (mode 4) TIMESTAMP_TOP reports too. Value below = their fallback. */
            static int ts_mode = -1; static uint64_t occl, last_top; static uint8_t seen[64];
            if (ts_mode < 0) { char e[16]; DWORD k = GetEnvironmentVariableA("TOTK_GPU_TS", e, sizeof e);
                ts_mode = k && k < sizeof e && !strcmp(e, "ns") ? 0 : k && k < sizeof e && !strcmp(e, "fast") ? 2 : boot_gts_mode() ? 2 + boot_gts_mode() : 1; }
            uint32_t type = (uint32_t)x[1];
            if (type < 64 && !seen[type]) { seen[type] = 1; boot_draw_log("nvn native: report_counter type%s %llu first seen, ts mode %llu", "", type, (uint64_t)ts_mode); }
            uint64_t ns = 0; boot_gpu_sync_host_ns(&ns);
            uint64_t ticks = (uint64_t)((unsigned __int128)ns * 384 / 625);
            uint64_t v[2] = {type == 1 ? (occl += UINT64_C(0x10000)) : 0, ts_mode == 0 ? ns : ticks};
            if (ts_mode == 2) { if (type == 0x10) last_top = ticks; else if (type == 0 && last_top) v[1] = last_top + UINT64_C(1228800); }
            if (type == 1 && boot_nvn_occl_report(r, x[2], v[1], e->skipped + boot_nvn_render_failed)) {   /* batch-2: the GPU writes the count */
                boot_nvn_rshadow_forget(x[2], 16); boot_nvn_pred_touch_range(e, x[2], 16); boot_ef_note(x[2], 16, 1);
            } else if (ts_mode >= 3 && (type == 0 || (type == 0x10 && ts_mode == 4)) && boot_gts_report(r, x[2], type, v[1])) {   /* G1: the GPU writes {0, its time} */
                boot_nvn_rshadow_put(x[2], v);   /* the counter word as the GPU writes it (0) */
                boot_nvn_pred_touch_range(e, x[2], 16); boot_ef_note(x[2], 16, 1);
            } else {
            boot_nvn_rshadow_put(x[2], v);   /* render-38b */
            boot_nvn_pred_touch_range(e, x[2], 16);   /* render-38c */
            boot_nvn_exec_store(r, x[2], v, 16); boot_ef_note(x[2], 16, 1);
            }
        } else if (op == o->reset_counter) {   /* batch-2: SAMPLES_PASSED (host_nvn_occl.h); the other counters stay ignored */
            if ((uint32_t)x[1] == 1) boot_nvn_occl_reset(r, e->skipped + boot_nvn_render_failed);
        } else if (op == o->call_commands) {
            uint64_t nested[64], tgt[64]; uint32_t k = b0n / 8 < 64 ? b0n / 8 : 64; if (b0) memcpy(nested, b0, (size_t)k * 8);
            /* call-bind: the versions the caller bound when it recorded this CallCommands (block BOOT_NVN_CB_ARG; host_nvn.h) */
            const uint8_t *bv = rec.blocks > 1 && rec.block[1].arg == BOOT_NVN_CB_ARG ? rec.block[1].data : NULL;
            const uint32_t kb = bv ? (rec.block[1].bytes / 8 < k ? rec.block[1].bytes / 8 : k) : 0;
            for (uint32_t i = 0; i < k; ++i) {
                uint64_t b = 0, bsh, lsh; unsigned how;
                if (i < kb) memcpy(&b, bv + 8 * (size_t)i, 8);
                tgt[i] = boot_nvn_cb_target(nested[i], b, &how, &bsh, &lsh);
                if (how == BOOT_NVN_CB_R_DIVERGED && boot_nvn_cb_mode() == 2) boot_nvn_cb_watch(c, self, r, handle, nested[i], bsh, lsh, at, depth);
            }
            for (uint32_t i = 0; i < k; ++i) { boot_nvn_la_nest(depth, tgt, k, i); if (!boot_nvn_exec_handle(c, self, r, e, tgt[i], depth + 1)) return 0; }
            boot_nvn_la_nest(depth, NULL, 0, 0);   /* pso-work */
        } else if (op == o->dispatch) {
            if (boot_tile_probe_on() && boot_tile_probe_bound(e, 5)) boot_tile_probe_token("R");   /* tiles-01 diagnostic */
            boot_nvn_pred_touch();   /* render-38c: the counters are read again after the game's compute */
            boot_nvn_exec_dispatch(c, self, r, e, (uint32_t)x[1], (uint32_t)x[2], (uint32_t)x[3]);
        } else if (op == o->dispatch_ind) {
            if (boot_tile_probe_on() && boot_tile_probe_bound(e, 5)) boot_tile_probe_token("Ri");   /* tiles-01 diagnostic */
            boot_nvn_pred_touch();   /* render-38c */
            ++boot_nvn_ind_records[BOOT_RENDER_IND_DISPATCH];   /* render-38d */
            boot_ef_note(x[1], 12, 0);   /* overlap-work P4 */
            if (boot_nvn_ind_gpu(r, BOOT_RENDER_IND_DISPATCH, 0, 0, x[1])) {   /* render-38d: group counts read by the GPU (ExecuteIndirect) */
                memset(&boot_render_next_ind, 0, sizeof boot_render_next_ind); boot_render_next_ind.va = x[1]; boot_render_next_ind.kind = BOOT_RENDER_IND_DISPATCH;
                boot_nvn_exec_dispatch(c, self, r, e, 1, 1, 1);   /* placeholders: c0 0x200 and the grid are patched on the GPU */
                boot_render_next_ind.kind = 0;
            } else {
                ++boot_nvn_ind_cpu[BOOT_RENDER_IND_DISPATCH];
                const uint32_t *g = boot_nvn_exec_indirect(r, e, x[1], 12);
                if (g) boot_nvn_exec_dispatch(c, self, r, e, g[0], g[1], g[2]); else boot_draw_log("nvn native: indirect dispatch arguments%s %llx %llu", "", x[1], 0);
            }
        } else {
            static uint8_t logged[BOOT_NVN_NAME_COUNT];
            if (op >= 0 && op < BOOT_NVN_NAME_COUNT && !logged[op]) { logged[op] = 1; boot_draw_log("nvn native: ignored command %s %llu %llu", boot_nvn_names[op].name, 0, 0); }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- backend */
static inline uint64_t *boot_nvn_exec_fence(void) { static uint64_t v; return &v; }

/* ---------------------------------------------------------------- stall sampler (play (74), opt-in)
 * TOTK_STALL_SAMPLE=<ms>: when no submission has finished for <ms> (the game is stalled or a submission is stuck),
 * a thread samples the host RIP of every busy guest worker every 20 ms (suspend, read the control context,
 * resume: nothing is locked or allocated while a worker is suspended) and, when the submissions resume (or every
 * 3 s of a long stall), writes one block to draw-path.log: for each guest thread (index as in host-wait-graph.log,
 * "t<n>") the functions its samples fell in, as <module>+<function start RVA>. The generated pages are the
 * functions boot_page_<guest address >> 12>: resolve the RVAs with the DLL's PDB (DbgHelp SymFromAddr or
 * `llvm-symbolizer`), which names the guest code behind a guest-side stall; samples in d3d12/driver DLLs name a
 * host-side one. Samples in ntdll/KernelBase waits are only counted. Off (the default) nothing runs. */
static volatile uint64_t boot_stall_last_end;   /* QPC at the end of the latest submission */
typedef struct BootStallSample { uint16_t worker, thread; uint64_t rip; } BootStallSample;
#define BOOT_STALL_SAMPLES 640
static void boot_stall_flush(BootStallSample *s, unsigned n, uint64_t stall_ms, int partial) {
    unsigned waiting = 0;
    struct Agg { uint16_t worker, thread; char mod[40]; uint64_t func; unsigned count; } *agg = (struct Agg *)calloc(n ? n : 1, sizeof *agg);
    unsigned na = 0;
    for (unsigned i = 0; agg && i < n; ++i) {
        HMODULE hm = NULL; char name[MAX_PATH] = "?", *base = name; DWORD64 image = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)s[i].rip, &hm) && hm) {
            if (GetModuleFileNameA(hm, name, sizeof name)) { char *b = strrchr(name, '\\'); base = b ? b + 1 : name; } }
        if (!_stricmp(base, "ntdll.dll") || !_stricmp(base, "KERNELBASE.dll") || !_stricmp(base, "win32u.dll")) { ++waiting; continue; }   /* a wait, not work */
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry((DWORD64)s[i].rip, &image, NULL);
        uint64_t func = fe ? (uint64_t)fe->BeginAddress : (uint64_t)(s[i].rip - (uint64_t)(uintptr_t)hm);
        unsigned j; for (j = 0; j < na; ++j) if (agg[j].worker == s[i].worker && agg[j].func == func && !strncmp(agg[j].mod, base, sizeof agg[j].mod - 1)) break;
        if (j == na) { agg[na].worker = s[i].worker; agg[na].thread = s[i].thread; snprintf(agg[na].mod, sizeof agg[na].mod, "%s", base); agg[na].func = func; ++na; }
        ++agg[j].count;
    }
    FILE *lf = fopen("draw-path.log", "a");
    if (lf) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(lf, "nvn native: stall sample %02u:%02u:%02u.%03u stall %llu ms%s: %u running-thread samples, %u of them in OS waits\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long long)stall_ms, partial ? " (still stalled)" : "", n, waiting);
        for (unsigned w = 0; agg && w < BOOT_MT_MAX; ++w) {   /* per worker: its three most-sampled functions */
            unsigned printed = 0, total = 0; int th = -1;
            for (unsigned j = 0; j < na; ++j) if (agg[j].worker == w) { total += agg[j].count; th = agg[j].thread; }
            if (!total) continue;
            fprintf(lf, "  worker %u thread t%d: %u samples:", w, th, total);
            while (printed < 3) {
                unsigned best = UINT_MAX;
                for (unsigned j = 0; j < na; ++j) if (agg[j].worker == w && agg[j].count && (best == UINT_MAX || agg[j].count > agg[best].count)) best = j;
                if (best == UINT_MAX) break;
                fprintf(lf, " %s+0x%llx x%u", agg[best].mod, (unsigned long long)agg[best].func, agg[best].count); agg[best].count = 0; ++printed;
            }
            fputc('\n', lf);
        }
        fclose(lf);
    }
    free(agg);
}
static DWORD WINAPI boot_stall_sample_thread(void *arg) {
    uint64_t thr_ms = (uint64_t)(uintptr_t)arg; LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    const uint64_t per_ms = (uint64_t)fq.QuadPart / 1000 ? (uint64_t)fq.QuadPart / 1000 : 1;
    BootStallSample *buf = (BootStallSample *)malloc(BOOT_STALL_SAMPLES * sizeof *buf); if (!buf) return 0;
    unsigned n = 0; int active = 0; uint64_t stall_end_seen = 0, flushed_at = 0;
    for (;;) {
        Sleep(20);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        uint64_t end = boot_stall_last_end; if (!end) continue;
        uint64_t idle = (uint64_t)now.QuadPart > end ? (uint64_t)now.QuadPart - end : 0;
        if (idle > thr_ms * per_ms) {
            if (!active) { active = 1; stall_end_seen = end; flushed_at = (uint64_t)now.QuadPart; n = 0; }
            unsigned workers = boot_mt.workers < BOOT_MT_MAX ? boot_mt.workers : BOOT_MT_MAX;
            for (unsigned w = 0; w < workers && n < BOOT_STALL_SAMPLES; ++w) {
                BootCore *k = boot_mt.core[w];
                if (!k || !k->host || k->idle || k->thread >= BOOT_THREAD_CAPACITY) continue;
                uint16_t thread = (uint16_t)k->thread; uint64_t rip = 0;
                if (SuspendThread(k->host) == (DWORD)-1) continue;
                CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(k->host, &ctx)) rip = (uint64_t)ctx.Rip;
                ResumeThread(k->host);
                if (!rip) continue;
                buf[n].worker = (uint16_t)w; buf[n].thread = thread; buf[n].rip = rip; ++n;
            }
            if (n + 32 >= BOOT_STALL_SAMPLES || (uint64_t)now.QuadPart - flushed_at > 3000 * per_ms) {
                /* waits (ntdll / KernelBase) are told apart in the flush by their module name */
                boot_stall_flush(buf, n, idle / per_ms, 1); n = 0; flushed_at = (uint64_t)now.QuadPart; }
        } else if (active) {
            boot_stall_flush(buf, n, (end - stall_end_seen) / per_ms, 0); n = 0; active = 0;
        }
    }
}
static inline void boot_stall_sample_start(void) {
    static volatile LONG started;
    if (started || InterlockedExchange(&started, 1)) return;
    char v[16]; DWORD k = GetEnvironmentVariableA("TOTK_STALL_SAMPLE", v, sizeof v);
    if (!k || k >= sizeof v) return;
    unsigned long ms = strtoul(v, NULL, 10); if (ms < 50) return;
    HANDLE t = CreateThread(NULL, 0, boot_stall_sample_thread, (void *)(uintptr_t)ms, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(t); }
}
static uint64_t boot_nvn_mem_qpc, boot_nvn_kick_qpc;   /* play (74): time in the per-submission memory sync (page table) and in the list kick, for the hitch line */
static uint64_t boot_nvn_fence_waits, boot_nvn_fence_wait_qpc;   /* play (55) */
static inline int boot_nvn_exec_submit(void *ctx, BootContext *c, uint64_t self, uint64_t queue, uint64_t handle) {
    (void)ctx; (void)queue;
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native;
    if (!w) { boot_draw_log("nvn native: submit failed: %s %llu %llu", "no window", handle, 0); return 0; }
    BootNvnExec *e = boot_nvn_exec();
    if (!e) return 0;
    boot_stall_sample_start();   /* play (74): TOTK_STALL_SAMPLE=<ms>, off by default */
    AcquireSRWLockExclusive(&w->lock);
    int ok = boot_draw_device_recover(w);
    if (!ok) boot_draw_log("nvn native: submit failed: %s %llu %llu", "device recover", handle, 0);
    if (ok && !(ok = boot_draw_open(w))) boot_draw_log("nvn native: submit failed: %s %llu %llu", "draw open", handle, 0);
    BootRender *r = ok ? w->render : NULL;
    if (ok && !(ok = boot_render_occl_open(r))) boot_draw_log("nvn native: submit failed: %s %llu %llu", "occlusion open", handle, 0);
    if (ok && !(ok = boot_render_layout(r))) boot_draw_log("nvn native: submit failed: %s %llu %llu", "render layout", handle, 0);
    if (ok) {
        LARGE_INTEGER m0, m1; QueryPerformanceCounter(&m0);   /* play (74): the page-table rebuild (GPU idle wait + remap) ran unmeasured before the hitch clock */
        ok = boot_nvn_exec_memory(c, w, e);
        QueryPerformanceCounter(&m1); boot_nvn_mem_qpc += (uint64_t)(m1.QuadPart - m0.QuadPart);
        if (!ok) boot_draw_log("nvn native: submit failed: %s %llu %llu", "exec memory (page table)", handle, 0);
    }
    if (ok) boot_ds_capture(r);   /* data-snapshot: once per call, before the re-hash and the translation: the call-start copy */
    if (ok) {
        if (!e->submits++) boot_nvn_exec_defaults(e);
        LARGE_INTEGER hz_pre; QueryPerformanceCounter(&hz_pre);   /* play (45): includes the texture re-hash */
        boot_render_cpu_texels_drain(r);   /* render-32: CPU texel writes since the last submission */
        if (boot_gpu_info_queue && ID3D12InfoQueue_GetNumStoredMessages(boot_gpu_info_queue)) {   /* render-35 */
            FILE *df = fopen("draw-path.log", "a"); if (df) { fprintf(df, "d3d12 validation before submission %llu:\n", (unsigned long long)boot_render_exec_id); boot_gpu_debug_drain(df); fclose(df); } }
        {   /* textures the guest CPU rewrote since they were built (font glyph cache, streamed images): re-hash
             * round-robin within TOTK_TEX_VERIFY_MB before this submission records draws, as the emulated path does */
            static uint64_t verify = UINT64_MAX;
            if (verify == UINT64_MAX) { char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_VERIFY_MB", v, sizeof v);
                verify = (n && n < sizeof v) ? (uint64_t)strtoull(v, NULL, 10) << 20 : UINT64_C(64) << 20; }
            unsigned stale = verify ? boot_render_tex_verify(r, verify) : 0;
            static uint64_t logged; if (stale && (++logged <= 32 || logged % 500 == 0))
                boot_draw_log("nvn native: textures rewritten by the guest since their build%s: %llu (total %llu)", "", stale, boot_render_tex_stale_rebuilds);
        }
        boot_bc7_pump(r, BOOT_BC7_AT_SUBMIT_BEGIN);   /* bc7-work: finished encodes switch their slots (TOTK_BC7) */
        ++boot_render_exec_id; r->overlay = NULL; r->overlay_count = 0;
        boot_nvn_occl_submit_begin();   /* batch-2: before the occlusion slots restart */
        boot_render_occl.next = boot_render_occl.lo = boot_render_occl.got = 0; boot_render_occl.want = 0;
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        boot_nvn_rehash_note((uint64_t)(t0.QuadPart - hz_pre.QuadPart));   /* batch-2 (N9): re-hash time for the 300-present line */
        boot_title_submission(boot_render_exec_id, 0, e->draws, e->clears, e->skipped, 1);
        boot_nvn_trace_begin(r);
        boot_nvn_pred_touch();   /* render-38c: conditions are read again in every submission */
        boot_ef_index(w); boot_ef_note_bound(e);   /* overlap-work P4 (TOTK_EARLY_FENCE=1): guest ranges this submission's GPU work touches */
        {   const int par = boot_nvn_par_begin(handle);   /* nvnpar-work 2: helpers decode the recordings meanwhile */
            ok = boot_nvn_exec_handle(c, self, r, e, handle, 0);
            boot_nvn_par_end(par); }
        boot_render_pred.armed = 0;
        boot_nvn_occl_submit_end(r);   /* batch-2: an open occlusion interval's slots are summed in this submission */
        /* execute without waiting: the game waits on its syncs (boot_nvn_exec_wait) */
        if (r->recording) {
            LARGE_INTEGER k0, k1; QueryPerformanceCounter(&k0);
            int kicked = boot_render_kick(r);
            QueryPerformanceCounter(&k1); boot_nvn_kick_qpc += (uint64_t)(k1.QuadPart - k0.QuadPart);
            if (!kicked) { boot_draw_log("nvn native: GPU execute failed%s %llu %llu", "", 0, 0); ok = 0; }
        }
        if (ok && boot_sync_fence_on() && !r->recording && !boot_render_begin(r)) ok = 0;   /* play (55b): keep "not recording means idle" */
        if (ok && boot_render_tex_census_on()) boot_draw_perf_gpu = r->gpu;   /* mem-work: DXGI local / non-local usage on the perf3 line */
        if (ok && boot_render_tex_live_trim_on()) boot_render_tex_trim_live(r, boot_render_tex_budget());   /* mem-work: the texture budget between idle points */
        if (ok) boot_bc7_pump(r, BOOT_BC7_AT_SUBMIT_END);   /* bc7-work: after the kick: switches, encode work within the frame budget (TOTK_BC7) */
        boot_nvn_trace_end(r);
        boot_title_submission(boot_render_exec_id, 1, e->draws, e->clears, e->skipped, (uint64_t)ok);
        e->w = w;
        QueryPerformanceCounter(&t1); e->submit_qpc += (uint64_t)(t1.QuadPart - t0.QuadPart);
        {   /* play (45): hitch logger */
            static uint64_t last_end, last_ph[9], last_fence, last_cload, last_cpso, last_cpso_q; static unsigned last_pso, last_shaders;
            static uint64_t last_mem, last_kick, last_remaps, last_ringfull, last_segw, last_fbev, last_idles, last_idleq, last_pack;   /* play (74) */
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); uint64_t per_ms = (uint64_t)fq.QuadPart / 1000; if (!per_ms) per_ms = 1;
            uint64_t fence = 0; for (unsigned i = 0; i < 32; ++i) fence += boot_render_wait_sites[i].qpc;
            uint64_t dur = (uint64_t)(t1.QuadPart - hz_pre.QuadPart), gap = last_end && (uint64_t)hz_pre.QuadPart > last_end ? (uint64_t)hz_pre.QuadPart - last_end : 0;
            unsigned shaders = boot_render_shader_total(r), psos = r->pso_count;
            if (last_end && (dur > 200 * per_ms || gap > 200 * per_ms)) {
                static const char *nm[9] = {"ring", "shaders", "targets", "textures", "vertex", "pso", "descriptors", "bind", "state"};
                char line[1100]; size_t at = 0; SYSTEMTIME st; GetLocalTime(&st);
                at += (size_t)snprintf(line + at, sizeof line - at, " %02u:%02u:%02u.%03u submit %llu ms (re-hash %llu ms) after a %llu ms gap; phases ms:",
                                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long long)(dur / per_ms),
                                       (unsigned long long)((t0.QuadPart - hz_pre.QuadPart) / (LONGLONG)per_ms), (unsigned long long)(gap / per_ms));
                for (unsigned i = 0; i < 9 && at < sizeof line; ++i) at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu", nm[i], (unsigned long long)((boot_render_ph_qpc[i] - last_ph[i]) / per_ms));
                if (at < sizeof line) at += (size_t)snprintf(line + at, sizeof line - at, "; new graphics PSOs %d, shaders loaded %d; compute PSOs %llu (%llu ms), compute shader lookups %llu ms; fence waits %llu ms",
                                       psos >= last_pso ? (int)(psos - last_pso) : -1, shaders >= last_shaders ? (int)(shaders - last_shaders) : -1,
                                       (unsigned long long)(boot_compute_hitch_pso - last_cpso), (unsigned long long)((boot_compute_hitch_pso_qpc - last_cpso_q) / per_ms),
                                       (unsigned long long)((boot_compute_hitch_load_qpc - last_cload) / per_ms), (unsigned long long)((fence - last_fence) / per_ms));
                /* play (74): what the submission did outside the nine draw phases, and what ran in the gap before it (the page-table sync is before the clock) */
                if (at < sizeof line) at += (size_t)snprintf(line + at, sizeof line - at, "; memory sync %llu ms (GPU arena pack %llu ms, page-table rebuilds +%llu), list kick %llu ms, ring-full +%llu, segment waits +%llu, feedback evictions +%llu, sync waits +%llu (%llu ms)",
                                       (unsigned long long)((boot_nvn_mem_qpc - last_mem) / per_ms), (unsigned long long)((boot_nvn_pack_qpc - last_pack) / per_ms), (unsigned long long)(e->remaps - last_remaps), (unsigned long long)((boot_nvn_kick_qpc - last_kick) / per_ms),
                                       (unsigned long long)(boot_render_ring_full - last_ringfull), (unsigned long long)(boot_render_seg_waits - last_segw), (unsigned long long)(boot_render_fb_evictions - last_fbev),
                                       (unsigned long long)(e->idles - last_idles), (unsigned long long)((e->idle_qpc - last_idleq) / per_ms));
                boot_draw_log("nvn native: hitch%s (submission %llu, draws %llu)", line, e->submits, e->draws);
                {   char tl[256]; snprintf(tl, sizeof tl, " built %llu: detile %llu ms, ASTC decode %llu ms (%llu MiB RGBA8, %u decode threads), upload copy %llu ms, create %llu ms",
                                           (unsigned long long)boot_tex_built, (unsigned long long)(boot_tex_t_detile / per_ms), (unsigned long long)(boot_tex_t_decode / per_ms),
                                           (unsigned long long)(boot_tex_decoded_bytes >> 20), boot_astc_pool.n, (unsigned long long)(boot_tex_t_copy / per_ms),
                                           (unsigned long long)(boot_tex_t_create / per_ms));
                    boot_draw_log("nvn native: hitch textures%s (submission %llu %llu)", tl, e->submits, 0); }
                boot_render_pso_hitch_log(e->submits);   /* pso-work: parallel pipeline compiles */
            }
            memcpy(last_ph, boot_render_ph_qpc, sizeof last_ph); last_fence = fence; last_cload = boot_compute_hitch_load_qpc;
            last_mem = boot_nvn_mem_qpc; last_kick = boot_nvn_kick_qpc; last_remaps = e->remaps; last_ringfull = boot_render_ring_full; last_segw = boot_render_seg_waits;
            last_fbev = boot_render_fb_evictions; last_idles = e->idles; last_idleq = e->idle_qpc; last_pack = boot_nvn_pack_qpc;
            last_cpso = boot_compute_hitch_pso; last_cpso_q = boot_compute_hitch_pso_qpc; last_pso = psos; last_shaders = shaders;
            last_end = (uint64_t)t1.QuadPart; boot_stall_last_end = last_end;
            boot_tex_built = boot_tex_t_detile = boot_tex_t_decode = boot_tex_t_copy = boot_tex_t_create = boot_tex_decoded_bytes = 0;   /* play (50): per submission */
        }
        if (e->submits % 100 == 1) {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            boot_draw_log("nvn native: submissions %s%llu draws %llu", "", e->submits, e->draws);
            boot_draw_log("nvn native: clears %s%llu skipped %llu", "", e->clears, e->skipped);
            {   char pk[1280];   /* t2-A: GPU arena pack fast path counters, after the fields the existing parsers read */
                /* drawcost: cost per draw since the previous line: submit time (page-table sync + record + kick, QPC) and the
                 * submitting thread's CPU time (t2: GetThreadTimes, user + kernel, everything it ran in between) over the draws */
                static uint64_t dc_draws, dc_qpc, dc_cpu; static DWORD dc_tid; FILETIME tc, te, tk, tu; uint64_t cpu_now = 0;
                if (GetThreadTimes(GetCurrentThread(), &tc, &te, &tk, &tu))
                    cpu_now = ((uint64_t)tk.dwHighDateTime << 32 | tk.dwLowDateTime) + ((uint64_t)tu.dwHighDateTime << 32 | tu.dwLowDateTime);
                if (dc_tid != GetCurrentThreadId()) { dc_tid = GetCurrentThreadId(); dc_cpu = 0; }   /* async-xlate: the translating thread changed (t2 / X): no delta across threads */
                uint64_t dc_q = e->submit_qpc + boot_nvn_mem_qpc, dd = e->draws - dc_draws;
                double sub_us = dd ? (double)(dc_q - dc_qpc) * 1e6 / (double)f.QuadPart / (double)dd : 0.0;
                double cpu_us = dd && dc_cpu && cpu_now >= dc_cpu ? (double)(cpu_now - dc_cpu) / 10.0 / (double)dd : 0.0;
                dc_draws = e->draws; dc_qpc = dc_q; dc_cpu = cpu_now;
                snprintf(pk, sizeof pk, " %llu discarded %llu; gpu arena pack: skipped %llu unlocked + %llu locked, full %llu (%llu ms, moves %llu, index builds %llu), all pack calls %llu ms, verify %s mismatches %llu"
                         "; per draw (last %llu draws): submit %.2f us, thread cpu %.2f us",
                         (unsigned long long)(uint64_t)((double)e->submit_qpc * 1000.0 / (double)f.QuadPart), (unsigned long long)e->discarded,
                         (unsigned long long)boot_gpu_pack_stats.skipped_unlocked, (unsigned long long)boot_gpu_pack_stats.skipped_locked, (unsigned long long)boot_gpu_pack_stats.full,
                         (unsigned long long)((double)boot_gpu_pack_stats.full_qpc * 1000.0 / (double)f.QuadPart), (unsigned long long)boot_gpu_pack_stats.moves,
                         (unsigned long long)boot_gpu_pack_stats.index_builds, (unsigned long long)((double)boot_nvn_pack_qpc * 1000.0 / (double)f.QuadPart),
                         boot_gpu_pack_verify() ? "on" : "off", (unsigned long long)boot_gpu_pack_stats.verify_mismatches,
                         (unsigned long long)dd, sub_us, cpu_us);
                {   size_t at = strlen(pk); if (at < sizeof pk) boot_nvn_rl_format(pk + at, sizeof pk - at); }   /* reclock: record store counters, last */
                boot_draw_log("nvn native: submit cpu ms total%s", pk, 0, 0); }
            boot_draw_log("nvn native: render enable off: draws and clears dropped %s%llu, conditional evaluations %llu", "", e->render_off, e->render_cond_evals);
            {   char dl[512];   /* flashfix (host_nvn.h boot_nvn_dsnap) */
                snprintf(dl, sizeof dl, " (TOTK_DEFER_SNAPSHOT %s, TOTK_DEFER_LATE_CHECK %s): submit calls %llu, recordings read %llu, binds copied %llu (%llu handles, %llu unreadable), index lost %llu, copy time %llu us; "
                         "binds executed %llu: from the copy %llu, not in it %llu; late check: handles %llu, changed since the call %llu;",
                         boot_nvn_defer_snapshot_on() ? "on" : "off", boot_nvn_defer_late_check_on() ? "on" : "off", (unsigned long long)boot_nvn_dsnap.submits,
                         (unsigned long long)boot_nvn_dsnap.slots, (unsigned long long)boot_nvn_dsnap.entries, (unsigned long long)boot_nvn_dsnap.words,
                         (unsigned long long)boot_nvn_dsnap.unread, (unsigned long long)boot_nvn_dsnap.index_lost,
                         (unsigned long long)((double)boot_nvn_dsnap.take_qpc * 1e6 / (double)f.QuadPart), (unsigned long long)boot_nvn_dsnap.binds,
                         (unsigned long long)boot_nvn_dsnap.hits, (unsigned long long)boot_nvn_dsnap.misses, (unsigned long long)boot_nvn_dsnap.late_checked,
                         (unsigned long long)boot_nvn_dsnap.late_changed);
                boot_draw_log("nvn native: deferred bind snapshot%s draws/clears failed %llu, copies with recorded objects %llu", dl, boot_nvn_render_failed, (uint64_t)boot_nvn_copy_capture_on()); }
            {   char bl[768]; boot_nvn_batch_a_format(bl, sizeof bl);   /* batch-a (host_nvn.h): submit freeze, re-record wait */
                boot_draw_log("nvn native: submit freeze%s fast signal lock %llu, frame trace %llu", bl, (uint64_t)boot_mt_fast_signal_lock_on(), (uint64_t)boot_nvn_ftrace_on()); }
            boot_draw_log("nvn native: conditional render counter shadow (render-38b): hits %s%llu, misses (GPU read) %llu", "", boot_nvn_rshadow_hits, boot_nvn_rshadow_misses);
            {   char pl[192];   /* render-38c */
                snprintf(pl, sizeof pl, " (TOTK_RENDER_COND %s): predicate dispatches %llu, reused %llu, draws/clears predicated %llu, fallbacks %llu;",
                         boot_nvn_render_cond_mode() == 2 ? "strict" : boot_nvn_render_cond_mode() ? "gpu" : "cpu", (unsigned long long)boot_nvn_pred_evals,
                         (unsigned long long)boot_nvn_pred_reused, (unsigned long long)boot_render_pred.predicated, (unsigned long long)boot_nvn_pred_fallbacks);
                boot_draw_log("nvn native: conditional render on the GPU%s CPU reads %llu, of them GPU syncs %llu", pl, boot_nvn_pred_cpu_reads, boot_nvn_pred_cpu_syncs); }
            {   char il[512];   /* render-38d */
                snprintf(il, sizeof il, " (TOTK_INDIRECT %s): records draws %llu / indexed %llu / dispatches %llu, reached %llu / %llu / %llu; on the GPU: "
                         "executed %llu / %llu / %llu, pre-passes %llu, pre-pass failures %llu; CPU reads %llu / %llu / %llu (why: cpu mode %llu, CPU index work %llu, "
                         "not GPU-readable %llu, unavailable %llu);", boot_render_ind_mode() ? "gpu" : "cpu",
                         (unsigned long long)boot_nvn_ind_records[1], (unsigned long long)boot_nvn_ind_records[2], (unsigned long long)boot_nvn_ind_records[3],
                         (unsigned long long)boot_nvn_ind_seen[1], (unsigned long long)boot_nvn_ind_seen[2], (unsigned long long)boot_nvn_ind_seen[3],
                         (unsigned long long)boot_render_ind_executed[1], (unsigned long long)boot_render_ind_executed[2], (unsigned long long)boot_render_ind_executed[3],
                         (unsigned long long)boot_render_ind_prepasses, (unsigned long long)boot_render_ind_failures,
                         (unsigned long long)boot_nvn_ind_cpu[1], (unsigned long long)boot_nvn_ind_cpu[2], (unsigned long long)boot_nvn_ind_cpu[3],
                         (unsigned long long)boot_nvn_ind_why[0], (unsigned long long)boot_nvn_ind_why[1], (unsigned long long)boot_nvn_ind_why[2], (unsigned long long)boot_nvn_ind_why[3]);
                boot_draw_log("nvn native: indirect%s GPU syncs by CPU reads %llu, all indirect CPU reads %llu", il, boot_nvn_ind_syncs,
                              boot_nvn_ind_cpu[1] + boot_nvn_ind_cpu[2] + boot_nvn_ind_cpu[3]); }
            boot_draw_log("nvn native: sync waits %s%llu wait ms %llu", "", e->idles, (uint64_t)((double)e->idle_qpc * 1000.0 / (double)f.QuadPart));
            boot_draw_log("nvn native: sync waits for the value only (TOTK_SYNC_WAIT_FENCE) %s%llu wait ms %llu", "", boot_nvn_fence_waits, (uint64_t)((double)boot_nvn_fence_wait_qpc * 1000.0 / (double)f.QuadPart));
            {   char ol[1024]; boot_ov_format(ol, sizeof ol);   /* overlap-work P5 */
                boot_draw_log("nvn native: frame overlap%s stale resched kicks cleared %llu, early-fence signals %llu", ol, (uint64_t)boot_mt_stale_resched, boot_ef.signals); }
            boot_draw_log("nvn native: compute deferred %s%llu completed at once %llu", "", boot_compute_deferred, boot_compute_sync_dispatches);
            boot_draw_log("nvn native: compute relearned %s%llu guard waits %llu", "", boot_compute_relearned, boot_compute_guard_waits);
            boot_draw_log("nvn native: compute ranges grown %s%llu texture overlap rejects %llu", "", boot_compute_grown, boot_compute_texture_rejects);
            {   char cl[192];   /* play (74) */
                snprintf(cl, sizeof cl, " built %ld, failed %ld, taken %llu, claimed by the dispatch %llu, waited for %llu (%llu ms):", (long)boot_compute_pre_built, (long)boot_compute_pre_failed,
                         (unsigned long long)boot_compute_pre_taken, (unsigned long long)boot_compute_pre_claimed, (unsigned long long)boot_compute_pre_waited,
                         (unsigned long long)((double)boot_compute_pre_wait_qpc * 1000.0 / (double)f.QuadPart));
                boot_draw_log("nvn native: compute pipelines built ahead%s page table clears partial %llu full %llu", cl, boot_render_pt_partial_clears, boot_render_pt_full_clears);
                snprintf(cl, sizeof cl, " page-table sync (memory) %llu ms, list kick %llu ms:", (unsigned long long)((double)boot_nvn_mem_qpc * 1000.0 / (double)f.QuadPart),
                         (unsigned long long)((double)boot_nvn_kick_qpc * 1000.0 / (double)f.QuadPart));
                boot_draw_log("nvn native: submission time%s remaps %llu submissions %llu", cl, e->remaps, e->submits); }
            boot_draw_log("nvn native: ring full (list executed early) %s%llu segment fence waits %llu", "", boot_render_ring_full, boot_render_seg_waits);
            boot_draw_log("nvn native: texture slot reuse: open-list submits %s%llu waited %llu", "", boot_render_slot_free_open, boot_render_slot_free_waited);
            {   char sv[768];   /* drain-work: texture slot versioning, and the slot-reuse waits that remain, by reason */
                snprintf(sv, sizeof sv, " (TOTK_SLOT_VERSIONING %s: open-list reuses executed without a wait %llu, earlier-list reuses without a wait %llu, descriptors staged %llu,"
                         " copied in place before %llu list executions, replaced resources held to their fence %llu; waits by reason: earlier list still running at the next"
                         " execution %llu, staging full %llu, held-resource list full %llu (all three %llu ms), no staging heap %llu, open-list submit + full wait %llu,"
                         " earlier list (the waited count) %llu, never bound (the drained count) %llu):",
                         boot_render_sv_on() ? "on" : "off", (unsigned long long)boot_render_sv_kicks, (unsigned long long)boot_render_sv_deferred,
                         (unsigned long long)boot_render_sv_staged, (unsigned long long)boot_render_sv_applies, (unsigned long long)boot_render_sv_held,
                         (unsigned long long)boot_render_sv_kick_waits, (unsigned long long)boot_render_sv_full_waits, (unsigned long long)boot_render_sv_retire_waits,
                         (unsigned long long)((double)boot_render_sv_wait_qpc * 1000.0 / (double)f.QuadPart), (unsigned long long)boot_render_sv_fallbacks,
                         (unsigned long long)boot_render_slot_free_open_waits, (unsigned long long)boot_render_slot_free_waited, (unsigned long long)boot_render_slot_free_drain);
                boot_draw_log("nvn native: texture slot reuse: already done%s %llu drained %llu", sv, boot_render_slot_free_done, boot_render_slot_free_drain); }
            {   char d2[768];   /* drain2-work: the descriptor write guard, depth copies kept, per-list recycling */
                snprintf(d2, sizeof d2, " (read by the open list, executed first %llu; by an executed list still running, staged %llu; by the open list after the index was"
                         " staged %llu; left in place with TOTK_SLOT_VERSIONING=0 %llu); depth copies refreshed instead of target views (TOTK_DFB_KEEP_COPY %s) %llu;"
                         " upload ring (TOTK_LIST_RECYCLE %s): wraps over completed lists %llu, chunks skipped while read %llu, ring-full submit + wait %llu (%llu ms);"
                         " deferred releases after their list %llu, release-list full submits %llu; total",
                         (unsigned long long)boot_render_sv_wg[0], (unsigned long long)boot_render_sv_wg[1], (unsigned long long)boot_render_sv_wg[2],
                         (unsigned long long)boot_render_sv_wg[3], boot_render_dfb_keep_on() ? "on" : "off", (unsigned long long)boot_render_dfb_kept,
                         boot_render_recycle_on() ? "on" : "off", (unsigned long long)boot_render_up_recycles, (unsigned long long)boot_render_up_busy,
                         (unsigned long long)boot_render_up_full_waits, (unsigned long long)((double)boot_render_up_full_qpc * 1000.0 / (double)f.QuadPart),
                         (unsigned long long)boot_render_deferred_polled, (unsigned long long)boot_render_deferred_full_waits);
                boot_draw_log("nvn native: drain2: descriptor rewrites of indices recorded work may still read%s %llu, staged now %llu", d2,
                              boot_render_sv_wg[0] + boot_render_sv_wg[1] + boot_render_sv_wg[2] + boot_render_sv_wg[3], (uint64_t)boot_render_sv_n); }
            {   char tk[640];   /* blacktex-work: those rewrites once per index and reading list, the recorded bindings that read the index, and what slipped through */
                snprintf(tk, sizeof tk, " (TOTK_TEX_KEEP %s, TOTK_SLOT_VERSIONING %s, TOTK_DFB_KEEP_COPY %s): kept on the old descriptors: rewrites %llu, recorded bindings %llu;"
                         " writes with no open list while a reading list ran (waited for, expected 0) %llu; depth copies refreshed in place instead of target views %llu;"
                         " slipped through (recorded work read the new descriptors, expected 0 with TOTK_TEX_KEEP on): rewrites %llu, recorded bindings",
                         boot_render_tex_keep_on() ? "on" : "off", boot_render_sv_on() ? "on" : "off", boot_render_dfb_keep_on() ? "on" : "off",
                         (unsigned long long)boot_render_tk_events[0], (unsigned long long)boot_render_tk_draws[0], (unsigned long long)boot_render_tk_idle_waits,
                         (unsigned long long)boot_render_dfb_kept, (unsigned long long)boot_render_tk_events[1]);
                boot_draw_log("nvn native: blacktex: texture indices rewritten while recorded work read them%s %llu, full waits for open-list slot reuse %llu", tk,
                              boot_render_tk_draws[1], boot_render_slot_free_open_waits); }
            {   char sl[768]; const BootNvnStateMemo *m = &boot_nvn_state_memo;   /* nvnpar-work 1c / 1d / 1e */
                snprintf(sl, sizeof sl, " (TOTK_STATE_CACHE %s): draws %llu, full builds %llu, group rebuilds: program %llu, targets %llu, viewports %llu, pools %llu, vertex input %llu,"
                         " fixed function %llu, constant buffers %llu, driver buffer vertex %llu / pixel %llu; records decoded in place (TOTK_RECORD_ZERO_COPY %s) %llu; re-hash helpers (TOTK_RENDER_HELPERS) %u, jobs %llu, items %llu;",
                         boot_nvn_state_cache_on() ? "on" : "off", (unsigned long long)m->calls, (unsigned long long)m->full, (unsigned long long)m->builds[0], (unsigned long long)m->builds[1],
                         (unsigned long long)m->builds[2], (unsigned long long)m->builds[3], (unsigned long long)m->builds[4], (unsigned long long)m->builds[5], (unsigned long long)m->builds[6], (unsigned long long)m->builds[7], (unsigned long long)m->builds[8],
                         boot_nvn_record_zero_copy_on() ? "on" : "off", (unsigned long long)boot_nvn_record_zero_copies, boot_render_par.helpers,
                         (unsigned long long)boot_render_par.jobs, (unsigned long long)boot_render_par.items);
                boot_draw_log("nvn native: draw state cache%s shadow clips %llu %llu", sl, boot_nvn_shadow_clipped, 0); }
            {   char tl[512];   /* nvnpar-work 1a */
                snprintf(tl, sizeof tl, " (TOTK_TEXSET_CACHE %s%s): textures taken remembered %llu, resolved %llu (remembered after %llu); samplers taken remembered %llu, resolved %llu"
                         " (remembered after %llu); all forgotten %llu; verified %llu, changed %llu;", boot_render_texmemo_on() ? "on" : "off", boot_render_texmemo_verify_on() ? ", verify" : "",
                         (unsigned long long)boot_render_texmemo_hits, (unsigned long long)boot_render_texmemo_resolves, (unsigned long long)boot_render_texmemo_sets,
                         (unsigned long long)boot_render_smpmemo_hits, (unsigned long long)boot_render_smpmemo_resolves, (unsigned long long)boot_render_smpmemo_sets,
                         (unsigned long long)boot_render_texmemo_resets, (unsigned long long)boot_render_texmemo_verified, (unsigned long long)boot_render_texmemo_bad);
                boot_draw_log("nvn native: texture binding cache%s host descriptor hits %llu guest reads %llu", tl, boot_render_host_hits, boot_render_host_guest_reads); }
            {   char cl[96]; snprintf(cl, sizeof cl, " (TOTK_CB_DEDUPE %d): snapshot slots taken from the slot before", boot_render_cb_dedupe_mode());   /* nvnpar-work 1f */
                boot_draw_log("nvn native: constant buffer dedupe%s %llu, mismatches %llu", cl, boot_render_cb_dedupe_hits, boot_render_cb_dedupe_bad); }
            {   char pl[768]; boot_nvn_par_format(pl, sizeof pl);   /* nvnpar-work 2 */
                boot_draw_log("nvn native: parallel translation%s; unit table %llu, item limit %llu", pl, BOOT_NVN_PAR_UNITS, BOOT_NVN_PAR_UNIT_ITEMS); }
            {   char fl[400] = "";   /* batch-c: TOTK_FB_COPY_PER_BARRIER counters since the previous line, per presented frame */
                if (boot_render_fb_pb_on()) {
                    static uint64_t l_copies, l_reuses, l_barriers, l_frames, l_ends[3];
                    uint64_t frames = w && w->present ? w->present->frames : 0, df = frames >= l_frames ? frames - l_frames : 0;
                    double pf = df ? 1.0 / (double)df : 0.0;
                    snprintf(fl, sizeof fl, " (TOTK_FB_COPY_PER_BARRIER on, last %llu frames: copies %.1f, reuses %.1f, barriers %.1f per frame; "
                             "sync points barrier +%llu, targets +%llu, other +%llu; totals copies %llu reuses %llu barriers %llu)",
                             (unsigned long long)df, (double)(boot_render_fb_refreshes - l_copies) * pf, (double)(boot_render_fb_pb_reuses - l_reuses) * pf,
                             (double)(boot_render_fb_pb_barriers - l_barriers) * pf, (unsigned long long)(boot_render_fb_pb_ends[0] - l_ends[0]),
                             (unsigned long long)(boot_render_fb_pb_ends[1] - l_ends[1]), (unsigned long long)(boot_render_fb_pb_ends[2] - l_ends[2]),
                             (unsigned long long)boot_render_fb_refreshes, (unsigned long long)boot_render_fb_pb_reuses, (unsigned long long)boot_render_fb_pb_barriers);
                    l_copies = boot_render_fb_refreshes; l_reuses = boot_render_fb_pb_reuses; l_barriers = boot_render_fb_pb_barriers; l_frames = frames;
                    memcpy(l_ends, boot_render_fb_pb_ends, sizeof l_ends);
                }
                boot_draw_log("nvn native: feedback copies%s: evictions (each a full submit/idle) %llu feedback draws %llu", fl, boot_render_fb_evictions, boot_render_fb_draws); }
            {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);   /* CPU per draw phase (boot_render_draw), cumulative */
                static const char *names[9] = {"ring", "shaders", "targets", "textures", "vertex/index", "pso", "descriptors", "bind+draw", "nvn state"};
                char line[512]; size_t at = 0; uint64_t total = 0;
                for (unsigned i = 0; i < 9; ++i) total += boot_render_ph_qpc[i];
                for (unsigned i = 0; i < 9 && at < sizeof line; ++i)
                    at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu ms", names[i], (unsigned long long)(boot_render_ph_qpc[i] * 1000 / (uint64_t)fq.QuadPart));
                boot_draw_log("nvn native: draw phases%s: total %llu ms over %llu host draws", line, total * 1000 / (uint64_t)fq.QuadPart, boot_render_ph_draws);
                boot_draw_log("nvn native: instanced draws (one host draw per instance)%s %llu, instances %llu", "", boot_render_ph_instanced, boot_render_ph_instances);
                boot_draw_log("nvn native: gt guards fast %s%llu, full %llu", "", boot_render_gt_fast, boot_render_gt_slow);
            }
            {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);   /* blocking fence waits by host_gpu_render.h line */
                for (unsigned i = 0; i < 32 && boot_render_wait_sites[i].line; ++i)
                {   char n[32]; snprintf(n, sizeof n, "n=%llu ", (unsigned long long)boot_render_wait_sites[i].count);
                    boot_draw_log("nvn native: fence waits %shost_gpu_render.h:%llu %llu ms", n, (unsigned long long)boot_render_wait_sites[i].line,
                                  (unsigned long long)(boot_render_wait_sites[i].qpc * 1000 / (uint64_t)fq.QuadPart)); }
            }
            boot_draw_log("nvn native: mip chains rebuilt in place %s%llu from targets %llu", "", boot_render_tex_mips_in_place, boot_render_tex_mips_from_rt);
            {   char tb[160]; snprintf(tb, sizeof tb, " (pool drops, destroyed: %llu; live slot list %u; compute id cache hits %llu misses %llu)",
                                       (unsigned long long)boot_render_tex_pool_dropped, boot_render_tex_live_n, (unsigned long long)boot_compute_id_hits,
                                       (unsigned long long)boot_compute_id_misses);
                boot_draw_log("nvn native: t2-B texture churn: alias-path textures pooled%s %llu, pool takes %llu", tb, boot_render_tex_alias_pooled, boot_render_tex_pooled); }
            boot_draw_log("nvn native: direct constant buffers %s%llu GPU writes %llu", "", boot_render_cb_direct, boot_render_gpu_writes);
            boot_draw_log("nvn native: page table rebuilds %s%llu %llu", "", e->remaps, 0);
            if (boot_draw_mem_log_on()) { boot_draw_perf_gpu = r->gpu; boot_draw_perf_render = r; }   /* mem-work: DXGI local / non-local video memory and the host memory part on the perf3 line */
            boot_draw_perf_log(0);   /* waits and command lists by call site, as in the emulated path */
        }
    }
    if(!ok) {   /*98rev3: persist original results before NEW query/DRED. */
        DWORD rf98_saved=GetLastError();boot_rf98_dump();static volatile LONG rf98_query_claim;
        if(!InterlockedCompareExchange(&rf98_query_claim,1,0)) {
            BootGpu *rf98_gpu=w->gpu;ID3D12Device *rf98_device=rf98_gpu?rf98_gpu->device:NULL;
            if(rf98_device) {
                HRESULT rf98_reason=ID3D12Device_GetDeviceRemovedReason(rf98_device);
                FILE *rf98_file=fopen("draw-path.log","a");
                if(rf98_file) {
                    fprintf(rf98_file,"RENDER98 new_device_reason source=post_backend_false observation=NEW_QUERY pid=%lu gpu=%llx device=%llx hr=%08lx current_device_only=1 original_failed_operation=UNKNOWN\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)(uintptr_t)rf98_gpu,(unsigned long long)(uintptr_t)rf98_device,(unsigned long)(uint32_t)rf98_reason);
                    if(FAILED(rf98_reason)){(void)fflush(rf98_file);boot_gpu_dred_dump(rf98_device,rf98_file);}
                    fclose(rf98_file);
                }
            } else {
                FILE *rf98_file=fopen("draw-path.log","a");
                if(rf98_file){fprintf(rf98_file,"RENDER98 new_device_reason source=post_backend_false observation=NOT_CALLED pid=%lu gpu=%llx device=0 reason=missing_device original_failed_operation=UNKNOWN\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)(uintptr_t)rf98_gpu);fclose(rf98_file);}
            }
        }
        SetLastError(rf98_saved);
    }
    ReleaseSRWLockExclusive(&w->lock);
    return ok;
}
/* Fences are the D3D12 queue fence: nvnQueueFenceSync takes the value of the last executed list,
 * nvnSyncWait waits for it. Waiting makes the queue idle (boot_render_idle: uploads released, deferred
 * compute stores applied), then pitch-linear targets are copied to guest memory for CPU readers and the
 * texture budget is trimmed, as at the end of an emulated-path submission. */
static inline uint64_t boot_nvn_exec_signal(void *ctx, uint64_t queue) {
    (void)ctx; (void)queue; BootNvnExec *e = boot_nvn_exec(); BootWindow *w = e ? e->w : NULL;
    if (!w || !w->render) return ++*boot_nvn_exec_fence();
    AcquireSRWLockExclusive(&w->lock);
    BootRender *r = w->render;
    if (r->recording) { boot_render_kick(r); if (boot_sync_fence_on()) boot_render_begin(r); }   /* play (55b): next list open */
    uint64_t v = r->gpu ? r->gpu->submitted : 0;
    ReleaseSRWLockExclusive(&w->lock);
    if (v > *boot_nvn_exec_fence()) *boot_nvn_exec_fence() = v;
    return v;
}
static inline uint64_t boot_nvn_exec_completed(void *ctx, uint64_t queue) {
    (void)ctx; (void)queue; BootNvnExec *e = boot_nvn_exec(); BootWindow *w = e ? e->w : NULL;
    if (!w || !w->render || !w->render->gpu || !w->render->gpu->fence) return *boot_nvn_exec_fence();
    return boot_nw_fence_completed(w->render->gpu->fence,BOOT_NW_COMPLETED,0,0,0);
}
static uint64_t boot_nvn_fence_waits, boot_nvn_fence_wait_qpc;   /* play (55) */
static inline int boot_nvn_exec_wait(void *ctx, uint64_t queue, uint64_t value, uint64_t timeout_ns) {
    boot_nw_note(BOOT_NW_BE_ENTRY,BOOT_NW_A|BOOT_NW_B|BOOT_NW_C,queue,value,timeout_ns,0);
    if (boot_nvn_exec_completed(ctx, queue) >= value) return boot_nw_result(BOOT_NW_BE_READY,1,0);
    if (!timeout_ns) return boot_nw_result(BOOT_NW_BE_ZERO,0,0);
    BootNvnExec *e = boot_nvn_exec(); BootWindow *w = e ? e->w : NULL;
    if (!w) return boot_nw_result(BOOT_NW_BE_RETURN,1,3);
    {   /* play (55): wait for this value only (TOTK_SYNC_WAIT_FENCE=1) */
        int on = boot_sync_fence_on(); static unsigned no_idle;   /* play (55b): value waits in a row without a queue idle */
        BootRender *r0 = w->render; BootGpu *g = r0 ? r0->gpu : NULL;
        int pitch = 0;
        if (on && r0) { BootRenderRtTable *T = boot_render_rtab(r0); for (unsigned i = 0; i < T->count && !pitch; ++i) pitch = T->pitch_dirty[i]; }
        if (boot_nw_guard(1,on) && boot_nw_guard(2,g!=NULL) && boot_nw_guard(3,g->fence!=NULL) && boot_nw_guard(4,!pitch) &&
            boot_nw_guard(5,value <= boot_nw_value(BOOT_NW_SUBMITTED,g->submitted,(uint64_t)(uintptr_t)g,value)) && boot_nw_guard(6,no_idle < 16)) {
            static _Thread_local HANDLE ev; if (!ev) ev = CreateEventW(NULL, FALSE, FALSE, NULL);
            CRITICAL_SECTION *cs = boot_mt.active ? boot_render_cs() : NULL; LONG depth = 0;
            boot_nw_note(BOOT_NW_RELEASE_BEGIN,0,0,0,0,0);
            if (cs && (DWORD)(uintptr_t)cs->OwningThread == GetCurrentThreadId()) { depth = cs->RecursionCount; for (LONG i = 0; i < depth; ++i) LeaveCriticalSection(cs); }
            boot_nw_note(BOOT_NW_RELEASE_END,BOOT_NW_A,(uint64_t)depth,0,0,0);
            LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
            int ok = 1;
            if (boot_nw_guard(7,ev!=NULL) && SUCCEEDED(boot_nw_hresult(BOOT_NW_REGISTER,ID3D12Fence_SetEventOnCompletion(g->fence, value, ev)))) {
                DWORD ms = timeout_ns == UINT64_MAX || timeout_ns / 1000000 >= INFINITE ? INFINITE : (DWORD)(timeout_ns / 1000000);
                boot_nw_note(BOOT_NW_WAIT_BEGIN,BOOT_NW_A|BOOT_NW_B,ms,value,0,0);
                ok = boot_nw_result(BOOT_NW_WAIT_END,(int)WaitForSingleObject(ev, ms),0) == WAIT_OBJECT_0 || boot_nw_fence_completed(g->fence,BOOT_NW_COMPLETED,(uint64_t)(uintptr_t)g,value,1) >= value;
            } else ok = 0;
            QueryPerformanceCounter(&t1); ++boot_nvn_fence_waits; boot_nvn_fence_wait_qpc += (uint64_t)(t1.QuadPart - t0.QuadPart);
            boot_nw_span(BOOT_NW_VALUE_SPAN,t0.QuadPart,t1.QuadPart);
            boot_nw_note(BOOT_NW_REACQUIRE_BEGIN,BOOT_NW_A,(uint64_t)depth,0,0,0);
            for (LONG i = 0; i < depth; ++i) EnterCriticalSection(cs);
            boot_nw_note(BOOT_NW_REACQUIRE_END,BOOT_NW_A,(uint64_t)depth,0,0,0);
            if (ok) {
                boot_nw_note(BOOT_NW_WINDOW_BEGIN,BOOT_NW_A,1,0,0,0);
                AcquireSRWLockExclusive(&w->lock);   /* the queue went idle meanwhile: the old housekeeping, no wait */
                boot_nw_note(BOOT_NW_WINDOW_END,BOOT_NW_A,1,0,0,0);
                boot_nw_note(BOOT_NW_HOUSE_BEGIN,0,0,0,0,0);
                BootRender *r = w->render;
                if (boot_nw_guard(8,r!=NULL) && boot_nw_guard(9,r->gpu == g) && boot_nw_fence_completed(g->fence,BOOT_NW_HOUSE_COMPLETED,(uint64_t)(uintptr_t)g,value,1) >= boot_nw_value(BOOT_NW_SUBMITTED,g->submitted,(uint64_t)(uintptr_t)g,value) &&
                    (boot_nw_note(BOOT_NW_HOUSE_IDLE_BEGIN,0,0,0,0,0),boot_nw_result(BOOT_NW_HOUSE_IDLE_END,boot_render_idle(r),0)) && boot_nw_guard(10,!r->recording)) {
                    no_idle = 0;
                    static uint64_t budget;
                    if (!budget) { char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_BUDGET_MB", v, sizeof v); unsigned long long mb = (n && n < sizeof v) ? strtoull(v, NULL, 10) : 0;
                                   budget = mb >= 256 ? (uint64_t)mb << 20 : BOOT_RENDER_TEX_BUDGET; }
                    boot_nw_note(BOOT_NW_TRIM_BEGIN,0,0,0,0,0);
                    boot_render_tex_trim(r, budget); boot_render_pso_release_retired();
                    boot_nw_note(BOOT_NW_TRIM_END,0,0,0,0,0);
                } else ++no_idle;   /* 16 in a row: the next wait is the complete one */
                boot_nw_note(BOOT_NW_HOUSE_END,BOOT_NW_A,no_idle,0,0,0);
                ReleaseSRWLockExclusive(&w->lock);
                return boot_nw_result(BOOT_NW_BE_RETURN,1,1);
            }
            /* failed or timed out: fall through to the complete wait */
        }
        no_idle = 0;   /* the complete wait below idles the queue */
    }
    boot_nw_note(BOOT_NW_WINDOW_BEGIN,BOOT_NW_A,2,0,0,0);
    AcquireSRWLockExclusive(&w->lock);
    boot_nw_note(BOOT_NW_WINDOW_END,BOOT_NW_A,2,0,0,0);
    BootRender *r = w->render; int ok = 0;
    if (boot_nw_guard(11,r!=NULL)) {
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        boot_nw_note(BOOT_NW_COMPLETE_BEGIN,0,0,0,0,0);
        ok = boot_nw_result(BOOT_NW_COMPLETE_END,boot_render_idle(r),0) &&
            (boot_nw_note(BOOT_NW_PITCH_BEGIN,0,0,0,0,0),boot_nw_result(BOOT_NW_PITCH_END,boot_render_flush_pitch(r),0));
        static uint64_t budget;
        if (!budget) {
            char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_BUDGET_MB", v, sizeof v);
            unsigned long long mb = (n && n < sizeof v) ? strtoull(v, NULL, 10) : 0;
            budget = mb >= 256 ? (uint64_t)mb << 20 : BOOT_RENDER_TEX_BUDGET;
        }
        if (ok && !r->recording) { boot_nw_note(BOOT_NW_TRIM_BEGIN,0,0,0,0,0); boot_render_tex_trim(r, budget); boot_render_pso_release_retired(); boot_nw_note(BOOT_NW_TRIM_END,0,0,0,0,0); }   /* queue idle: as after an emulated submission */
        QueryPerformanceCounter(&t1); ++e->idles; e->idle_qpc += (uint64_t)(t1.QuadPart - t0.QuadPart);
    }
    ReleaseSRWLockExclusive(&w->lock);
    return boot_nw_result(BOOT_NW_BE_RETURN,ok,2);
}
static inline void boot_nvn_exec_gpu_wait(void *ctx, uint64_t queue, uint64_t other, uint64_t value) { (void)ctx; (void)queue; (void)other; (void)value; }
static inline void boot_nvn_exec_pool(void *ctx, uint64_t va, uint64_t storage, uint64_t size, uint32_t flags) { (void)ctx; (void)va; (void)storage; (void)size; (void)flags; }
/* Present a window texture: the newest render target at its address (as boot_draw_present). */
/* play (54): the game's present interval: a present happens no earlier than interval/60 s after the previous one.
 * play (63): "the previous one" is the previous present's scheduled time, not the time it actually happened (the
 * scheme of SuperMarioWonderRecomp runtime/src/nvn/nvn_queue.cpp pace_present, MIT): measured from the actual time
 * every wake-up overshoot (timer latency, the renderer-lock reacquire after the sleep) was added to the next frame's
 * period, so a locked 30 fps ran slower than 30 and its phase against the 60 Hz vblank crept until a frame landed on
 * the next vblank (a 50 ms frame every few seconds). The schedule is dropped (reset to now) only when a present is a
 * full period or more late, so a hitch is not paid back with a burst of frames. */
static uint64_t boot_present_interval_waits, boot_present_interval_wait_qpc;
/* N2: interval is the window's present interval when the caller has it (nvnWindowAcquireTexture, host_nvn.h
 * boot_present_pace_at_acquire), UINT32_MAX to read it from the window (the wait at present). */
static inline void boot_present_interval_pace_at(BootContext *c, uint64_t window, uint32_t known) {
    static int on = -1; static LARGE_INTEGER freq; static int64_t last; /* scheduled time of the previous present (QPC) */ static _Thread_local HANDLE timer;
    if (on < 0) { const char *v = getenv("TOTK_PRESENT_INTERVAL_PACE"); on = !(v && !strcmp(v, "0")); QueryPerformanceFrequency(&freq); }
    if (!on || !freq.QuadPart) return;
    BootNvnWindow wn; uint32_t interval = 1;
    if (known != UINT32_MAX) { if (known) interval = known < 8 ? known : 8; }
    else if (boot_nvn_peek(c, window, window, &wn, sizeof wn) == sizeof wn && wn.magic == BOOT_NVN_MAGIC_WINDOW && wn.interval) interval = wn.interval < 8 ? wn.interval : 8;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    int64_t period = (int64_t)interval * freq.QuadPart / 60;
    int64_t due = last ? last + period : 0;
    if (due > now.QuadPart) {
        /* sleep without the renderer lock (recursive critical section: release every level, restore after) */
        CRITICAL_SECTION *cs = boot_mt.active ? boot_render_cs() : NULL; LONG depth = 0;
        if (cs && (DWORD)(uintptr_t)cs->OwningThread == GetCurrentThreadId()) { depth = cs->RecursionCount; for (LONG i = 0; i < depth; ++i) LeaveCriticalSection(cs); }
        if (!timer) { timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS); if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL); }
        LARGE_INTEGER rel; rel.QuadPart = -(LONGLONG)((due - now.QuadPart) * 10000000 / freq.QuadPart);
        if (rel.QuadPart < 0 && timer && SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else if (rel.QuadPart < 0) Sleep((DWORD)((due - now.QuadPart) * 1000 / freq.QuadPart));
        for (LONG i = 0; i < depth; ++i) EnterCriticalSection(cs);
        LARGE_INTEGER after; QueryPerformanceCounter(&after); ++boot_present_interval_waits; boot_present_interval_wait_qpc += (uint64_t)(after.QuadPart - now.QuadPart);
        now = after;
    }
    /* A late frame is not snapped to a vsync grid: the next period counts from this frame's scheduled time unless it was a period or more late. */
    last = (due && now.QuadPart - due < period) ? due : now.QuadPart;
}
static inline void boot_present_interval_pace(BootContext *c, uint64_t window) { boot_present_interval_pace_at(c, window, UINT32_MAX); }
/* N2: present timestamps for the interval-change log (host_nvn.h boot_nvn_interval_changed); every 300 presents one
 * stderr line with the mean present-to-present time and the present-interval waits (counted, never logged before). */
static inline void boot_nvn_present_note(BootContext *c, uint64_t window) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint32_t k = boot_nvn_present_count; boot_nvn_present_qpc[k % BOOT_NVN_PRESENT_RING] = now.QuadPart; boot_nvn_present_count = ++k;
    if (k % 300 || !boot_nvn_pace_log_on()) return;
    static int64_t since; static uint64_t waits0, qpc0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); double qpc_ms = 1000.0 / (double)f.QuadPart;
    BootNvnWindow wn; uint32_t interval = 0;
    if (boot_nvn_peek(c, window, window, &wn, sizeof wn) == sizeof wn && wn.magic == BOOT_NVN_MAGIC_WINDOW) interval = wn.interval;
    fprintf(stderr, "nvn native: present pacing: %u presents, interval %u, wait at %s; last 300: %.2f ms per present, %llu waits %.1f ms; total %llu waits %.1f ms\n",
            k, interval, boot_present_pace_at_acquire() ? "acquire" : "present", since ? (double)(now.QuadPart - since) * qpc_ms / 300.0 : 0.0,
            (unsigned long long)(boot_present_interval_waits - waits0), (double)(boot_present_interval_wait_qpc - qpc0) * qpc_ms,
            (unsigned long long)boot_present_interval_waits, (double)boot_present_interval_wait_qpc * qpc_ms);
    since = now.QuadPart; waits0 = boot_present_interval_waits; qpc0 = boot_present_interval_wait_qpc;
}
/* flashfix: the executor's cumulative counters for the flicker log's per-present deltas (host_gpu_draw_exec.h). */
static inline void boot_nvn_flicker_counts(void) {
    BootNvnExec *e = boot_nvn_exec(); BootFlickerNvn *f = &boot_draw_flicker_nvn;
    if (!e) return;
    f->draws = e->draws; f->clears = e->clears; f->skipped = e->skipped; f->failed = boot_nvn_render_failed; f->render_off = e->render_off;
    f->discarded = e->discarded; f->deferred = boot_nvn_dsnap.binds; f->snap_hits = boot_nvn_dsnap.hits; f->snap_misses = boot_nvn_dsnap.misses;
    f->late_checked = boot_nvn_dsnap.late_checked; f->late_changed = boot_nvn_dsnap.late_changed;
    boot_draw_flicker_late_on = boot_nvn_defer_late_check_on();
}
/* batch-a frame trace (host_nvn.h boot_nvn_ftrace_write): the executor's cumulative draws, clears and skipped draws. */
static inline void boot_nvn_ftrace_exec_counts(uint64_t v[3]) {
    BootNvnExec *e = boot_nvn_exec(); v[0] = e ? e->draws : 0; v[1] = e ? e->clears : 0; v[2] = e ? e->skipped : 0;
}
static inline void boot_nvn_exec_present_body(BootWindow *w, const BootColorTarget *ctp);
static inline int boot_nvn_exec_present(void *ctx, BootContext *c, uint64_t queue, uint64_t window, uint32_t index) {
    (void)ctx; (void)queue;
    uint64_t trace_outer=boot_present_trace_frame;
    uint64_t trace_frame=boot_present_trace_begin(window,index);
    boot_nvn_present_note(c, window);
    if (!boot_present_pace_at_acquire()) boot_present_interval_pace(c, window);   /* N2: else the wait runs in nvnWindowAcquireTexture */
    boot_present_trace_event(BOOT_PT_INTERVAL_DONE,trace_frame,0,window,index,0);
    BootWindow *w = (BootWindow *)(uintptr_t)c->window_native;
    uint64_t tex = boot_nvn_window_texture(c, window, index);
    BootColorTarget ct;
    if (!w || !tex || !boot_nvn_texture_target(c, window, tex, NULL, &ct, NULL)) { boot_draw_log("nvn native: present texture%s %llx %llu", "", tex, index); boot_present_trace_frame=trace_outer; return 1; }
    boot_nvn_exec_present_body(w, &ct);
    boot_present_trace_frame=trace_outer;
    return 1;
}
/* async-xlate stage 2: a present's half that reads only guest objects and the game's timing (pacing, the window texture through the calling
 * context): t2 runs it at the call, the PRESENT op carries the result to X, which runs the body below in FIFO order. */
typedef struct BootAxPresentPay { BootWindow *w; uint64_t trace_frame; BootColorTarget ct; } BootAxPresentPay;
static inline int boot_nvn_exec_present_call(BootContext *c, uint64_t window, uint32_t index, BootAxPresentPay *p) {
    uint64_t trace_outer=boot_present_trace_frame;
    p->trace_frame=boot_present_trace_begin(window,index);
    boot_nvn_present_note(c, window);
    if (!boot_present_pace_at_acquire()) boot_present_interval_pace(c, window);   /* N2: else the wait runs in nvnWindowAcquireTexture */
    boot_present_trace_event(BOOT_PT_INTERVAL_DONE,p->trace_frame,0,window,index,0);
    p->w = (BootWindow *)(uintptr_t)c->window_native;
    uint64_t tex = boot_nvn_window_texture(c, window, index);
    const int ok = p->w && tex && boot_nvn_texture_target(c, window, tex, NULL, &p->ct, NULL);
    if (!ok) boot_draw_log("nvn native: present texture%s %llx %llu", "", tex, index);
    boot_present_trace_frame=trace_outer;
    return ok;
}
static inline void boot_nvn_exec_present_x(const BootAxPresentPay *p) {
    uint64_t trace_outer=boot_present_trace_frame; boot_present_trace_frame=p->trace_frame;
    boot_nvn_exec_present_body(p->w, &p->ct);
    boot_present_trace_frame=trace_outer;
}
/* async-xlate stage 2: the queue idle a waiting nvnSyncWait / nvnQueueFinish runs today (boot_nvn_exec_wait's complete wait: everything
 * recorded executed, the queue idle (deferred compute stores applied), pitch-linear targets copied to guest memory, the texture budget,
 * retired pipelines), as an IDLE / FINISH op on X after everything queued before it. */
static inline int boot_nvn_exec_idle_x(void) {
    BootNvnExec *e = boot_nvn_exec(); BootWindow *w = e ? e->w : NULL;
    if (!w) return 1;
    AcquireSRWLockExclusive(&w->lock);
    BootRender *r = w->render; int ok = 1;
    if (r) {
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        ok = boot_render_idle(r) && boot_render_flush_pitch(r);
        static uint64_t budget;
        if (!budget) { char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_BUDGET_MB", v, sizeof v); unsigned long long mb = (n && n < sizeof v) ? strtoull(v, NULL, 10) : 0;
                       budget = mb >= 256 ? (uint64_t)mb << 20 : BOOT_RENDER_TEX_BUDGET; }
        if (ok && !r->recording) { boot_render_tex_trim(r, budget); boot_render_pso_release_retired(); }
        QueryPerformanceCounter(&t1); ++e->idles; e->idle_qpc += (uint64_t)(t1.QuadPart - t0.QuadPart);
    }
    ReleaseSRWLockExclusive(&w->lock);
    return ok;
}
/* The part of a present that reads renderer state (on the calling thread before stage 2; X's PRESENT op in stage 2). */
static inline void boot_nvn_exec_present_body(BootWindow *w, const BootColorTarget *ctp) {
    const BootColorTarget ct = *ctp;
    AcquireSRWLockExclusive(&w->lock);
    int ok = boot_draw_device_recover(w) && boot_draw_open(w);
    if (ok && !w->present) {
        w->present = calloc(1, sizeof(BootPresent));
        if (w->present) { w->present->dump_every = 300; snprintf(w->present->dump_dir, sizeof w->present->dump_dir, "frames"); CreateDirectoryA("frames", NULL); }
        else ok = 0;
    }
    if (ok) {
        if (BOOT_CENSUS_ON()) BOOT_CENSUS_PRESENT();
        BootRender *r = w->render; BootRenderRtTable *T = boot_render_rtab(r); BootRenderTarget *x = NULL; uint64_t newest = 0;
        if (boot_tile_probe_on()) boot_tile_probe_present(r, w->present->frames);   /* tiles-01 diagnostic (frame-N.bmp index) */
        boot_nvn_occl_present(r, w->present->frames); boot_nvn_rehash_present(w->present->frames);   /* batch-2: 300-present lines */
        boot_bc7_present(r, w->present->frames);   /* bc7-work: frame encode budget, 300-present line (TOTK_BC7) */
        if (boot_gts_mode()) boot_gts_present(r);   /* G1: frame count, once a second the "gpu timestamps" line */
        for (unsigned t = 0; t < T->count; ++t)
            if (T->rts[t].texture && T->rts[t].address == ct.address && !(T->rts[t].format & 0x10000u) && (!x || T->info[t].last_use >= newest)) {
                x = &T->rts[t]; newest = T->info[t].last_use; }
        if (x) { boot_draw_dump_targets(r, x->address); boot_nvn_flicker_counts(); boot_draw_flicker_watch(r, x); boot_draw_glitch_scan(r, x, w->present->frames); /* water-work */ if (boot_render_present_target(r, w->present, w->hwnd, x)) boot_draw_log("nvn native: present %s %llx %llu", r->error, ct.address, 0); }
        else boot_draw_log("nvn native: present: no render target at%s %llx %llu", "", ct.address, 0);
        if (boot_sd_on()) boot_sd_present(w->present->frames);   /* suspect-diag: frame number, S3 turn, the 300-present lines */
        boot_ds_present(r, w->present->frames);   /* data-snapshot: aging and the 300-present lines (TOTK_DATA_SNAPSHOT=0: one cached branch) */
    }
    ReleaseSRWLockExclusive(&w->lock);
    boot_psoc_tick();   /* play (73): persists new pipelines (background thread) once enough exist */
    boot_nvn_state_diag_present(w);   /* batch-3: N5 compare, K1 scan budget, 300-present counters (TOTK_STATE_DIAG) */
}
static inline void boot_nvn_exec_install(BootNvnBackend *b) {
    boot_compute_defer = 1; boot_present_no_wait = 1;
    b->ctx = NULL; b->submit = boot_nvn_exec_submit; b->signal = boot_nvn_exec_signal; b->wait = boot_nvn_exec_wait; b->completed = boot_nvn_exec_completed;
    b->gpu_wait = boot_nvn_exec_gpu_wait; b->pool = boot_nvn_exec_pool; b->present = boot_nvn_exec_present;
}
#endif
