#ifndef TOTK_HOST_GPU_PRESENT_H
#define TOTK_HOST_GPU_PRESENT_H
/* Presentation of a queued display buffer (IGraphicBufferProducer::QUEUE_BUFFER).
 *
 * Source: the host render target the game drew into for that buffer when one exists
 * (found by the buffer's GPU address), otherwise the guest buffer memory itself
 * (block-linear RGBA8, detiled on the CPU). Output: a flip-model DXGI swapchain on the
 * game window, plus optional frame dumps (frames/frame-NNNNN.bmp) for inspection.
 * The queue is synchronous: work recorded before the present has completed. */
#include "host_gpu_render.h"
#include <dxgi1_4.h>
#include "host_present_pace.h"   /* even host present cadence for slow frames */

typedef struct BootPresent {
    IDXGISwapChain3 *swapchain; HWND hwnd; uint32_t width, height;
    ID3D12Resource *staging; uint32_t staging_w, staging_h;
    ID3D12Resource *dump; uint64_t dump_size;
    uint64_t frames; uint32_t dump_every; char dump_dir[260];
} BootPresent;

static inline int boot_present_swapchain(BootRender *r, BootPresent *p, HWND hwnd) {
    /*93: the swapchain/copy extent is physical pixels, not DPI-virtualized
     * client units. Change only this query's thread context and restore it
     * before any existing DXGI/D3D call or return after a successful enter. */
    DWORD dpi_query_incoming_error = GetLastError();
    DPI_AWARENESS_CONTEXT dpi_query_previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!dpi_query_previous) return 0;
    SetLastError(dpi_query_incoming_error);
    RECT rc; BOOL dpi_query_ok = GetClientRect(hwnd, &rc);
    DWORD dpi_query_error = GetLastError();
    if (!SetThreadDpiAwarenessContext(dpi_query_previous)) return 0;
    SetLastError(dpi_query_error);
    if (!dpi_query_ok) return 0;
    uint32_t w = (uint32_t)(rc.right - rc.left), h = (uint32_t)(rc.bottom - rc.top);
    /* Minimised window: empty client area. Keep the current swapchain (or size a new one
     * 1280x720); presenting to a minimised window is harmless (DXGI reports it occluded). */
    if (!w || !h) { if (p->swapchain && p->hwnd == hwnd) return 1; w = 1280; h = 720; }
    if (p->swapchain && p->hwnd == hwnd && p->width == w && p->height == h) return 1;
    if (p->swapchain) { IDXGISwapChain3_Release(p->swapchain); p->swapchain = NULL; }
    IDXGIFactory2 *f = NULL; if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&f))) return 0;
    DXGI_SWAP_CHAIN_DESC1 d = {0};
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1 = NULL;
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown *)r->gpu->queue, hwnd, &d, NULL, NULL, &sc1);
    IDXGIFactory2_Release(f);
    if (FAILED(hr)) return 0;
    hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&p->swapchain);
    IDXGISwapChain1_Release(sc1);
    if (FAILED(hr)) return 0;
    p->hwnd = hwnd; p->width = w; p->height = h;
    return 1;
}

static inline void boot_present_dump(BootPresent *p, const uint8_t *rgba, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!p->dump_every || p->frames % p->dump_every) return;
    char path[400]; snprintf(path, sizeof path, "%s/frame-%05llu.bmp", p->dump_dir, (unsigned long long)p->frames);
    FILE *o = fopen(path, "wb"); if (!o) return;
    uint32_t size = 54 + w * 3 * h; int32_t hh = -(int32_t)h;
    uint8_t hdr[54] = {'B', 'M'}; memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &hh, 4); hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, o);
    uint8_t *row = malloc((size_t)w * 3);
    for (uint32_t y = 0; row && y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) { const uint8_t *s = rgba + (size_t)y * pitch + x * 4; row[x * 3] = s[2]; row[x * 3 + 1] = s[1]; row[x * 3 + 2] = s[0]; }
        fwrite(row, 1, (size_t)w * 3, o);
    }
    free(row); fclose(o);
}

static int boot_present_no_wait;   /* set by the native NVN executor (host_nvn_play.h) */
/* Presents `src` (a host texture in state `*state`, w x h) or, when src is NULL, the guest
 * block-linear RGBA8 buffer at `guest` (w x h, block height 2^log_gob_h GOBs). */
static inline int boot_present(BootRender *r, BootPresent *p, HWND hwnd, ID3D12Resource *src, D3D12_RESOURCE_STATES *state,
                               const uint8_t *guest, uint32_t w, uint32_t h, unsigned log_gob_h) {
    if (!boot_present_pace_wait()) return boot_render_fail(r, BOOT_RENDER_E_D3D, "Present (paced)");   /* previous paced present done */
    if (!boot_present_swapchain(r, p, hwnd)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "swapchain");
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list");
    ID3D12GraphicsCommandList *cl = r->cl;
    D3D12_RESOURCE_STATES src_state = state ? *state : D3D12_RESOURCE_STATE_COMMON;
    if (!src) {   /* guest memory path: detile into a staging texture */
        size_t lin = (size_t)w * 4 * h; uint8_t *buf = malloc(lin);
        if (!buf || !boot_texture_detile(buf, lin, guest, (size_t)-1 / 2, w * 4, h, log_gob_h)) { free(buf); return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "display buffer detile"); }
        if (!p->staging || p->staging_w != w || p->staging_h != h) {
            if (p->staging) ID3D12Resource_Release(p->staging);
            D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0}; D3D12_RESOURCE_DESC d = {0};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                            &IID_ID3D12Resource, (void **)&p->staging))) { free(buf); return boot_render_fail(r, BOOT_RENDER_E_D3D, "staging"); }
            p->staging_w = w; p->staging_h = h;
        }
        D3D12_RESOURCE_DESC td; p->staging->lpVtbl->GetDesc(p->staging, &td);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
        ID3D12Device_GetCopyableFootprints(r->gpu->device, &td, 0, 1, 0, &fp, &rows, &rs, &total);
        uint8_t *up; ID3D12Resource *upload = boot_render_buffer(r, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ, (void **)&up);
        if (!upload) { free(buf); return boot_render_fail(r, BOOT_RENDER_E_D3D, "upload"); }
        for (uint32_t y = 0; y < h; ++y) memcpy(up + fp.Offset + (size_t)y * fp.Footprint.RowPitch, buf + (size_t)y * w * 4, (size_t)w * 4);
        free(buf);
        D3D12_TEXTURE_COPY_LOCATION dl = {p->staging, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, sl = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
        sl.PlacedFootprint = fp;
        BOOT_GP_CTR_K(BOOT_GP_PRESENT, "present upload", cl, &dl, 0, 0, 0, &sl, NULL);
        if (boot_render_upload_count < 256) boot_render_uploads[boot_render_upload_count++] = upload;
        src = p->staging; src_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(p->swapchain);
    ID3D12Resource *back = NULL;
    if (FAILED(IDXGISwapChain3_GetBuffer(p->swapchain, index, &IID_ID3D12Resource, (void **)&back))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "back buffer");
    D3D12_RESOURCE_BARRIER b[2] = {{0}, {0}};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Transition.pResource = src; b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = src_state; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource = back; b[1].Transition.Subresource = 0;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    BOOT_GP_RB_K(BOOT_GP_PRESENT, "present", cl, src_state == D3D12_RESOURCE_STATE_COPY_SOURCE ? 0 : 1, &b[0]);
    BOOT_GP_RB_K(BOOT_GP_PRESENT, "present", cl, 1, &b[1]);
    /* copy the overlapping region (the window may differ from the buffer size) */
    D3D12_BOX box = {0, 0, 0, w < p->width ? w : p->width, h < p->height ? h : p->height, 1};
    D3D12_TEXTURE_COPY_LOCATION dl = {back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, sl = {src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    BOOT_GP_CTR_K(BOOT_GP_PRESENT, "present copy", cl, &dl, 0, 0, 0, &sl, &box);
    /* optional dump: readback of the presented source */
    uint8_t *mapped = NULL; D3D12_PLACED_SUBRESOURCE_FOOTPRINT dfp; int dumping = p->dump_every && !(p->frames % p->dump_every);
    if (dumping) {
        D3D12_RESOURCE_DESC sd; src->lpVtbl->GetDesc(src, &sd); UINT rows; UINT64 rs, total;
        ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &dfp, &rows, &rs, &total);
        if (!p->dump || p->dump_size < total) {
            if (p->dump) ID3D12Resource_Release(p->dump);
            p->dump = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, NULL); p->dump_size = total;
        }
        D3D12_TEXTURE_COPY_LOCATION ddl = {p->dump, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}; ddl.PlacedFootprint = dfp;
        if (p->dump) BOOT_GP_CTR_K(BOOT_GP_PRESENT, "frame dump", cl, &ddl, 0, 0, 0, &sl, NULL);
        else dumping = 0;
    }
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b[0].Transition.StateAfter = src_state;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    BOOT_GP_RB_K(BOOT_GP_PRESENT, "present", cl, src_state == D3D12_RESOURCE_STATE_COPY_SOURCE ? 0 : 1, &b[0]);
    BOOT_GP_RB_K(BOOT_GP_PRESENT, "present", cl, 1, &b[1]);
    /* The copy executes before the swapchain present on the same queue: without a frame dump (readback)
     * or a guest-memory source (staging upload) nothing here needs the copy completed (TOTK_NVN=native
     * presents without waiting; its syncs wait where the game waits). */
    int wait = dumping || !state || !boot_present_no_wait;
    int ok = wait ? boot_render_submit(r) : boot_render_kick(r);
    if (ok && !wait && boot_sync_fence_on()) ok = boot_render_begin(r);   /* play (55b): keep "not recording means idle" */
    ID3D12Resource_Release(back);
    if (!ok) return boot_render_fail(r, BOOT_RENDER_E_D3D, "present submit");
    if (wait) { for (unsigned i = 0; i < boot_render_upload_count; ++i) ID3D12Resource_Release(boot_render_uploads[i]);
                boot_render_upload_count = 0; }
    if (FAILED(boot_present_pace_submit(p->swapchain))) return boot_render_fail(r, BOOT_RENDER_E_D3D, "Present");
    if (dumping && SUCCEEDED(ID3D12Resource_Map(p->dump, 0, NULL, (void **)&mapped))) {
        boot_present_dump(p, mapped + dfp.Offset, dfp.Footprint.RowPitch, w, h);
        ID3D12Resource_Unmap(p->dump, 0, NULL);
    }
    ++p->frames;
    BOOT_GP_FRAME(r->gpu);   /* render-40: frame boundary (present to present) */
    return BOOT_RENDER_OK;
}
/* Presents a host render target (format must be 8-bit RGBA for the swapchain copy). */
static inline int boot_render_present_target(BootRender *r, BootPresent *p, HWND hwnd, BootRenderTarget *t) {
    if (t->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM && t->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        return boot_render_fail(r, BOOT_RENDER_E_FORMAT, "presented target is not 8-bit RGBA");
    return boot_present(r, p, hwnd, t->texture, &t->state, NULL, t->width, t->height, 0);
}
#endif
