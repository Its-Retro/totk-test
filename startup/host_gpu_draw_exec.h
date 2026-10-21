#ifndef TOTK_HOST_GPU_DRAW_EXEC_H
#define TOTK_HOST_GPU_DRAW_EXEC_H
/* Runtime glue (TOTK_DRAW_PATH): executes a validated submission's draw/clear events
 * with host_gpu_render.h, and presents queued display buffers. Included by
 * host_window.h (backend only), so translated pages never depend on this code.
 *
 * Guest memory: every GPU mapping of the channel's address space is resolved to the
 * guest region backing it and mapped zero-copy into the executor's page table. A
 * mapping backed by memory that cannot be shared (not a fully committed VirtualAlloc
 * region) stops the draw with a logged reason. */
#include "host_gpu_present.h"
#include <psapi.h>   /* K32GetProcessMemoryInfo / K32GetPerformanceInfo (kernel32): perf3 memory accounting */
#include "../call-chain/runtime.h"
#include "host_gpu_compute.h"
#include "guest_clock.h"

static inline void boot_draw_log(const char *fmt, const char *detail, uint64_t a, uint64_t b) {
    boot_draw_path_logf(fmt, detail, (unsigned long long)a, (unsigned long long)b);   /* t2-B: one kept handle (host_gpu_timestamp.h) */
}

static inline const char *boot_gpu_dred_op(unsigned op){
    static const char *names[]={"SetMarker","BeginEvent","EndEvent","DrawInstanced","DrawIndexedInstanced","ExecuteIndirect","Dispatch",
        "CopyBufferRegion","CopyTextureRegion","CopyResource","CopyTiles","ResolveSubresource","ClearRenderTargetView","ClearUnorderedAccessView",
        "ClearDepthStencilView","ResourceBarrier","ExecuteBundle","Present","ResolveQueryData","BeginSubmission","EndSubmission",
        "DecodeFrame","ProcessFrames","AtomicCopyBufferUint","AtomicCopyBufferUint64","ResolveSubresourceRegion","WriteBufferImmediate",
        "DecodeFrame1","SetProtectedResourceSession","DecodeFrame2","ProcessFrames1","BuildRaytracingAccelerationStructure",
        "EmitRaytracingAccelerationStructurePostbuildInfo","CopyRaytracingAccelerationStructure","DispatchRays","InitializeMetaCommand",
        "ExecuteMetaCommand","EstimateMotion","ResolveMotionVectorHeap","SetPipelineState1","InitializeExtensionCommand",
        "ExecuteExtensionCommand","DispatchMesh","EncodeFrame","ResolveEncoderOutputMetadata","Barrier","BeginCommandList",
        "DispatchGraph","SetProgram","ProcessFrames2"};   /* render-27a: D3D12_AUTO_BREADCRUMB_OP in order */
    return op<sizeof names/sizeof names[0]?names[op]:"op";
}
/* Writes the removal reason, the unfinished command lists' breadcrumbs around the last completed op, and the
 * page fault (address, allocations there and recently freed) to f. */
static inline void boot_gpu_dred_dump(ID3D12Device *device,FILE *f){
    if(!device || !f)return;
    fprintf(f,"D3D12 device removed: reason %08lx\n",(unsigned long)ID3D12Device_GetDeviceRemovedReason(device));
    boot_gpu_debug_drain(f);   /* render-35: validation messages up to the removal */
    boot_tile89_failure_tail(f);   /*97: one CPU snapshot before any82 dump lock */
    ID3D12DeviceRemovedExtendedData *d=NULL;
    if(FAILED(ID3D12Device_QueryInterface(device,&IID_ID3D12DeviceRemovedExtendedData,(void**)&d)) || !d){fprintf(f,"  DRED: not available\n");return;}
    /* render-27a: DRED 1.2 breadcrumbs carry the marker strings (TOTK_DRED_MARKERS=1): ops -12..+12 around the
     * first unfinished one, each with its marker, plus the last marker before the window (the draw being executed) */
    ID3D12DeviceRemovedExtendedData1 *d1=NULL; D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bo1; memset(&bo1,0,sizeof bo1);
    if(SUCCEEDED(ID3D12Device_QueryInterface(device,&IID_ID3D12DeviceRemovedExtendedData1,(void**)&d1)) && d1 &&
       SUCCEEDED(ID3D12DeviceRemovedExtendedData1_GetAutoBreadcrumbsOutput1(d1,&bo1))){
        unsigned shown=0,lists=0;
        for(const D3D12_AUTO_BREADCRUMB_NODE1 *n=bo1.pHeadAutoBreadcrumbNode;n;n=n->pNext){
            ++lists;
            UINT32 last=n->pLastBreadcrumbValue?*n->pLastBreadcrumbValue:0;
            if(last>=n->BreadcrumbCount || shown>=8)continue;
            ++shown;
            fprintf(f,"  DRED1 list %p queue %p: %u of %u ops completed, %u markers\n",(void*)n->pCommandList,(void*)n->pCommandQueue,
                    (unsigned)last,(unsigned)n->BreadcrumbCount,(unsigned)n->BreadcrumbContextsCount);
            UINT32 from=last>12?last-12:0,to=last+12<n->BreadcrumbCount?last+12:n->BreadcrumbCount;
            const wchar_t *before=NULL; UINT32 before_at=0;
            for(UINT32 c=0;c<n->BreadcrumbContextsCount;++c)
                if(n->pBreadcrumbContexts[c].BreadcrumbIndex<from && n->pBreadcrumbContexts[c].BreadcrumbIndex>=before_at){
                    before=n->pBreadcrumbContexts[c].pContextString; before_at=n->pBreadcrumbContexts[c].BreadcrumbIndex; }
            if(before)fprintf(f,"    (marker at op %u: %ls)\n",(unsigned)before_at,before);
            boot_dred_dump(f, (const void *)n->pCommandList, (const void *)n->pCommandQueue, (const uint32_t *)n->pCommandHistory, n->BreadcrumbCount, last);
            for(UINT32 i=from;i<to;++i){
                const wchar_t *ctx=NULL;
                for(UINT32 c=0;c<n->BreadcrumbContextsCount;++c) if(n->pBreadcrumbContexts[c].BreadcrumbIndex==i) ctx=n->pBreadcrumbContexts[c].pContextString;
                fprintf(f,"    %s%u %s%s%ls\n",i==last?"-> ":"   ",(unsigned)i,boot_gpu_dred_op((unsigned)n->pCommandHistory[i]),ctx?" ":"",ctx?ctx:L"");
            }
        }
        fprintf(f,"  DRED1: %u lists with breadcrumbs, %u unfinished shown\n",lists,shown);
    }
    if(d1)ID3D12DeviceRemovedExtendedData1_Release(d1);
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bo;memset(&bo,0,sizeof bo);
    if(!bo1.pHeadAutoBreadcrumbNode && SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetAutoBreadcrumbsOutput(d,&bo))){
        unsigned shown=0,lists=0;
        for(const D3D12_AUTO_BREADCRUMB_NODE *n=bo.pHeadAutoBreadcrumbNode;n;n=n->pNext){
            ++lists;
            UINT32 last=n->pLastBreadcrumbValue?*n->pLastBreadcrumbValue:0;
            if(last>=n->BreadcrumbCount || shown>=8)continue;   /* completed list */
            ++shown;
            fprintf(f,"  DRED list %p (%s) queue %p: %u of %u ops completed\n",(void*)n->pCommandList,n->pCommandListDebugNameA?n->pCommandListDebugNameA:"",
                    (void*)n->pCommandQueue,(unsigned)last,(unsigned)n->BreadcrumbCount);
            UINT32 from=last>6?last-6:0,to=last+6<n->BreadcrumbCount?last+6:n->BreadcrumbCount;
            for(UINT32 i=from;i<to;++i)fprintf(f,"    %s%u %s\n",i==last?"-> ":"   ",(unsigned)i,boot_gpu_dred_op((unsigned)n->pCommandHistory[i]));
        }
        fprintf(f,"  DRED: %u lists with breadcrumbs, %u unfinished shown\n",lists,shown);
    }else fprintf(f,"  DRED: no breadcrumbs\n");
    D3D12_DRED_PAGE_FAULT_OUTPUT po;memset(&po,0,sizeof po);
    if(SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetPageFaultAllocationOutput(d,&po))){
        fprintf(f,"  DRED page fault VA %llx\n",(unsigned long long)po.PageFaultVA);
        unsigned k=0;
        for(const D3D12_DRED_ALLOCATION_NODE *a=po.pHeadExistingAllocationNode;a && k<8;a=a->pNext,++k)
            fprintf(f,"    existing allocation type %u %s\n",(unsigned)a->AllocationType,a->ObjectNameA?a->ObjectNameA:"");
        k=0;
        for(const D3D12_DRED_ALLOCATION_NODE *a=po.pHeadRecentFreedAllocationNode;a && k<8;a=a->pNext,++k)
            fprintf(f,"    recently freed allocation type %u %s\n",(unsigned)a->AllocationType,a->ObjectNameA?a->ObjectNameA:"");
    }
    ID3D12DeviceRemovedExtendedData_Release(d);
}
/* A removed D3D12 device (TDR, device reset, paging failure) loses every object created on it.
 * Recreate the device, renderer and swapchain, give the GPU channels new command allocators, and
 * drop module caches holding objects of the old device. Host-only GPU memory (render targets not
 * yet written back) is lost. A compatible candidate does not reconstruct lost GPU
 * contents from guest memory. Returns 0 without retiring old objects if no safe
 * same-adapter candidate opens; the caller stops rather than migrating devices. */
static inline int boot_draw_device_recover(BootWindow *w) {
    if (!w->gpu || SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(w->gpu->device))) return 1;
    HRESULT reason = ID3D12Device_GetDeviceRemovedReason(w->gpu->device);
    {   FILE *lf = fopen("draw-path.log", "a"); if (lf) { boot_gpu_dred_dump(w->gpu->device, lf); fclose(lf); } }
    boot_light86_device_removed();
    boot_light84_device_removed();   /* diagnostic children must not pin the removed device */
    boot_psoc_device_lost();         /* play (73): the pipeline library holds the removed device; the new render object opens one for the replacement */
    BootGpu *g = boot_gpu_open_internal(0);   /* do not mutate the old debug queue */
    if (!g) { boot_draw_log("D3D12 device removed (reason %s%llx); no replacement device %llu", "", (uint32_t)reason, 0); return 0; }
    /* Fail closed BEFORE touching old renderer, swapchain, cache, channels or
     * validation-queue references. D3D12CreateDevice may reject the removed
     * original singleton while its children live; do not force their teardown
     * or silently migrate guest GPU state to a different adapter. */
    if (memcmp(&g->adapter.AdapterLuid, &w->gpu->adapter.AdapterLuid, sizeof g->adapter.AdapterLuid)) {
        char name[128]; snprintf(name, sizeof name, "%ls", g->adapter.Description);
        boot_draw_log("D3D12 recovery rejected different adapter %s; original objects retained (reason %llx, recovery %llu)", name, (uint32_t)reason, 0);
        boot_gpu_close(g); return 0;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1; memset(&o1, 0, sizeof o1);
    HRESULT caps = ID3D12Device_CheckFeatureSupport(g->device, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
    if (FAILED(caps) || !o1.WaveOps || !o1.WaveLaneCountMin || o1.WaveLaneCountMin > 32 || o1.WaveLaneCountMax < 32) {
        char why[160]; snprintf(why, sizeof why, "wave capability query %08lx, ops %u lanes %u..%u", (unsigned long)caps,
                               (unsigned)o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax);
        boot_draw_log("D3D12 recovery rejected %s; original objects retained (reason %llx, recovery %llu)", why, (uint32_t)reason, 0);
        boot_gpu_close(g); return 0;
    }
    boot_gpu_debug_attach(g->device);   /* accepted candidate only */
    {   char name[128]; snprintf(name, sizeof name, "%ls", g->adapter.Description);
        boot_draw_log("D3D12 device removed: accepted same-adapter device; resetting renderer and swapchain%s (reason %llx, recovery %llu)", "", (uint32_t)reason, 1);
        FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "  replacement adapter: %s, wave lanes %u..%u%s\n", name, o1.WaveLaneCountMin, o1.WaveLaneCountMax,
                          (o1.WaveLaneCountMin > 32 || o1.WaveLaneCountMax < 32) ? " (no 32-lane waves: [WaveSize(32)] compute programs cannot run)" : ""); fclose(lf); }
    }
    for (unsigned i = 0; i < boot_compute.shader_count; ++i)
        if (boot_compute.shaders[i].pso) { ID3D12PipelineState_Release(boot_compute.shaders[i].pso); boot_compute.shaders[i].pso = NULL; }
    boot_compute_pre_device_lost();   /* play (74): pipelines built ahead of use belong to the removed device */
    if (boot_compute.root) { ID3D12RootSignature_Release(boot_compute.root); boot_compute.root = NULL; }
    if (boot_compute.dirty) { ID3D12Resource_Release(boot_compute.dirty); boot_compute.dirty = NULL; boot_compute.dirty_cpu = NULL; boot_compute.dirty_words = 0; }
    for (unsigned i = 0; i < boot_render_clear_pso_count; ++i) ID3D12PipelineState_Release(boot_render_clear_psos[i].pso);
    boot_render_clear_pso_count = 0;
    if (boot_render_null_rtv_heap) { ID3D12DescriptorHeap_Release(boot_render_null_rtv_heap); boot_render_null_rtv_heap = NULL; }
    if (boot_render_trace.qh) { ID3D12QueryHeap_Release(boot_render_trace.qh); boot_render_trace.qh = NULL; }
    if (boot_render_trace.rb) { ID3D12Resource_Release(boot_render_trace.rb); boot_render_trace.rb = NULL; }
    boot_render_occl_close();
    if (boot_render_tex_slots) {
        for (uint32_t i = 0; i < BOOT_RENDER_TEX_SLOTS; ++i) if (boot_render_tex_slots[i].res) ID3D12Resource_Release(boot_render_tex_slots[i].res);
        memset(boot_render_tex_slots, 0, sizeof(BootRenderTexSlot) * BOOT_RENDER_TEX_SLOTS);
    }
    boot_render_sv_reset();   /* drain-work: staged slot descriptors and held resources belong to the removed device */
    memset(boot_render_ts_slots, 0, sizeof boot_render_ts_slots);
    boot_render_texmemo_reset();   /* nvnpar-work: remembered texture / sampler resolutions (host_gpu_render.h) */
    boot_render_rt_release_all();   /* render targets (host contents lost; guest memory stays authoritative) */
    boot_render_gt_release();       /* GPU write-back pipelines, lists and staging */
    boot_render_tex_pool_release(); /* idle textures kept for reuse */
    boot_render_alias_reset();
    for (unsigned i = 0; i < boot_render_upload_count; ++i) ID3D12Resource_Release(boot_render_uploads[i]);
    boot_render_upload_count = 0; boot_render_upload_bytes = 0;
    boot_render_staging_release(&boot_render_up_ring); boot_render_staging_release(&boot_render_rb_ring);
    boot_render_rt_spares_release();
    boot_render_release_deferred();   /* objects of the removed device */
    if (w->render) {   /* objects of the removed device are released; the shader cache's DXIL is kept by nobody */
        BootRender *r = w->render;
        for (unsigned i = 0; i < r->rt_count; ++i) if (r->rts[i].texture) ID3D12Resource_Release(r->rts[i].texture);
        for (unsigned i = 0; i < r->pso_count; ++i) if (r->psos[i].pso) ID3D12PipelineState_Release(r->psos[i].pso);
        free(r); w->render = NULL;
    }
    if (w->present) {
        BootPresent *p = w->present;
        if (p->swapchain) IDXGISwapChain3_Release(p->swapchain);
        if (p->staging) ID3D12Resource_Release(p->staging);
        if (p->dump) ID3D12Resource_Release(p->dump);
        free(p); w->present = NULL;
    }
    for (unsigned i = 0; i < 16; ++i) {
        BootGpuChannel *c = &w->nv_channels[i];
        if (!c->allocator) continue;
        ID3D12CommandAllocator_Release(c->allocator); c->allocator = NULL;
        if (FAILED(ID3D12Device_CreateCommandAllocator(g->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&c->allocator))) c->allocator = NULL;
    }
    BootGpu *old = w->gpu; w->gpu = g;
    boot_gpu_close(old);
    return 1;
}

static inline int boot_draw_open(BootWindow *w) {
    if (w->render) return 1;
    /* Native NVN owns queue creation, so no GPU channel (which opens the device) exists: open it here. */
    if (!w->gpu && !(w->gpu = boot_gpu_open())) { boot_draw_log("render open: %s %llu %llu", "no D3D12 device", 0, 0); return 0; }
    BootRender *r = calloc(1, sizeof *r);
    if (!r || !w->gpu) { free(r); return 0; }
    char manifest[600]; DWORD n = GetEnvironmentVariableA("TOTK_SHADER_MANIFEST", manifest, sizeof manifest);
    if (!n || n >= sizeof manifest) snprintf(manifest, sizeof manifest, "shader-manifest.tsv");
    if (!boot_render_open(r, w->gpu, NULL, 0, 0, manifest)) { boot_draw_log("render open: %s %llu %llu", r->error, 0, 0); free(r); return 0; }
    w->render = r;
    return 1;
}

/* A block is shareable with D3D12 when it is a whole VirtualAlloc allocation. Blocks the submit path
 * moved to VirtualAlloc are never freed, so a positive answer is remembered (VirtualQuery is a
 * kernel call and this runs for every mapping of every submission). */
static inline int boot_draw_is_alloc_base(void *p) {
    /* allocation bases are multiples of the 64 KiB allocation granularity: no kernel call needed */
    if (!p || ((uintptr_t)p & 0xffff)) return 0;
    static void *known[256]; static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (known[i] == p) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.AllocationBase != p) return 0;
    if (count < 256) known[count++] = p;
    return 1;
}

/* GPU arenas: guest-adjacent MapPhysicalMemory regions the GPU uses are packed by the submit path
 * into one VirtualAlloc block, so D3D12 needs one heap for many small regions (the region table
 * holds BOOT_RENDER_MAX_REGIONS). The block's first page holds a tag and the payload size; region
 * data starts at +4096. Arena memory is never passed to free(). */
#define BOOT_GPU_ARENA_HEADER 4096
static const char boot_gpu_arena_tag[16] = "TOTK-GPU-ARENA1";
/* Memo of boot_gpu_arena_base results (two VirtualQuery calls each; the submit path asks for
 * every GPU-mapped region on every submission). Arena membership of an address only changes when
 * an arena is created or released, which bumps the generation and drops every entry. */
static uint64_t boot_gpu_arena_generation = 1;
static struct { const void *p; uint8_t *base; uint64_t generation; } boot_gpu_arena_memo[8192];
static inline void boot_gpu_arena_forget(void) { ++boot_gpu_arena_generation; }
static inline uint8_t *boot_gpu_arena_lookup(const void *p);
/* Arenas confirmed in the current generation, as address ranges: any pointer inside one resolves
 * without a kernel call (an arena is one VirtualAlloc block, so its range holds nothing else). */
static struct { const uint8_t *base; uint64_t size; } boot_gpu_arena_known[64];
static unsigned boot_gpu_arena_known_count; static uint64_t boot_gpu_arena_known_generation;
static inline uint8_t *boot_gpu_arena_base(const void *p) {
    if (!p) return NULL;
    if (boot_gpu_arena_known_generation != boot_gpu_arena_generation) { boot_gpu_arena_known_count = 0; boot_gpu_arena_known_generation = boot_gpu_arena_generation; }
    for (unsigned i = 0; i < boot_gpu_arena_known_count; ++i)
        if ((const uint8_t *)p >= boot_gpu_arena_known[i].base + BOOT_GPU_ARENA_HEADER &&
            (uint64_t)((const uint8_t *)p - boot_gpu_arena_known[i].base) < boot_gpu_arena_known[i].size) return (uint8_t *)boot_gpu_arena_known[i].base;
    size_t slot = (((uintptr_t)p >> 12) ^ ((uintptr_t)p >> 25)) & 8191;
    if (boot_gpu_arena_memo[slot].p == p && boot_gpu_arena_memo[slot].generation == boot_gpu_arena_generation) return boot_gpu_arena_memo[slot].base;
    uint8_t *base = boot_gpu_arena_lookup(p);
    boot_gpu_arena_memo[slot].p = p; boot_gpu_arena_memo[slot].base = base; boot_gpu_arena_memo[slot].generation = boot_gpu_arena_generation;
    if (base && boot_gpu_arena_known_count < 64) {
        uint64_t payload; memcpy(&payload, base + 16, 8);
        boot_gpu_arena_known[boot_gpu_arena_known_count].base = base; boot_gpu_arena_known[boot_gpu_arena_known_count++].size = BOOT_GPU_ARENA_HEADER + payload;
    }
    return base;
}
static inline uint8_t *boot_gpu_arena_lookup(const void *p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED) || !mbi.AllocationBase) return NULL;
    uint8_t *base = (uint8_t *)mbi.AllocationBase;
    if ((uint8_t *)p < base + BOOT_GPU_ARENA_HEADER) return NULL;
    /* The allocation's first page may be reserved or decommitted (heap segments): only an
     * arena (committed read-write header page) can carry the tag. */
    if (!VirtualQuery(base, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE) return NULL;
    if (memcmp(base, boot_gpu_arena_tag, 16)) return NULL;
    return base;
}
/* fastmem direct mode (host_fastmem.h) installs a section allocator: an arena must be a fully committed
 * section view so the guest view can map the moved regions (D3D12 opens section views like
 * VirtualAlloc blocks). NULL: VirtualAlloc as before. */
__declspec(selectany) uint8_t *(*boot_gpu_arena_alloc_hook)(uint64_t size) = 0;
static inline uint8_t *boot_gpu_arena_new(uint64_t payload) {
    uint8_t *base = boot_gpu_arena_alloc_hook ? boot_gpu_arena_alloc_hook(BOOT_GPU_ARENA_HEADER + payload)
                                              : VirtualAlloc(NULL, (size_t)(BOOT_GPU_ARENA_HEADER + payload), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) return NULL;
    memcpy(base, boot_gpu_arena_tag, 16); memcpy(base + 16, &payload, 8);
    boot_gpu_arena_forget();
    return base;
}
/* D3D12-shareable allocation holding `p` (a region's data): an arena, or a whole VirtualAlloc block
 * starting at p (heap, or a region moved before arenas existed). */
static inline int boot_gpu_block(void *p, uint64_t region_size, uint8_t **base, uint64_t *size) {
    uint8_t *arena = boot_gpu_arena_base(p);
    if (arena) { uint64_t payload; memcpy(&payload, arena + 16, 8); *base = arena; *size = BOOT_GPU_ARENA_HEADER + payload; return 1; }
    if (boot_draw_is_alloc_base(p)) { *base = (uint8_t *)p; *size = region_size; return 1; }
    return 0;
}

/* Physical memory pool: MapPhysicalMemory takes its memory from GPU arenas up front (page ranges of a few
 * large arenas), so every physical region is GPU-shareable where it was created and the submit paths
 * (boot_gpu_pack_regions) never move live guest memory. Moving it raced with the other cores under
 * TOTK_CORES>1: they access guest memory lock-free through the fastmem page table, so a move lost their
 * writes and let them read the freed block. Unmapped ranges go back to the pool (zeroed when reused);
 * pool arenas are never released. Runs in the kernel (single-core, or under the multi-core kernel lock). */
#define BOOT_GPU_POOL_ARENA (UINT64_C(256) << 20)   /* payload of a pool arena (larger if one request is) */
#define BOOT_GPU_POOL_MAX 64
typedef struct BootGpuPoolRange { uint64_t off, len; } BootGpuPoolRange;
typedef struct BootGpuPoolArena { uint8_t *base; uint64_t payload, hwm; BootGpuPoolRange *free; unsigned free_count, free_cap; } BootGpuPoolArena;
typedef struct BootGpuPool { BootGpuPoolArena arena[BOOT_GPU_POOL_MAX]; unsigned count, disabled; uint64_t allocs, frees, reused_bytes; } BootGpuPool;
__declspec(selectany) BootGpuPool boot_gpu_pool = {0};
static inline BootGpuPoolArena *boot_gpu_pool_find(const void *p) {   /* pool arena holding p (payload), or NULL */
    for (unsigned i = 0; i < boot_gpu_pool.count; ++i) { BootGpuPoolArena *a = &boot_gpu_pool.arena[i];
        if ((const uint8_t *)p >= a->base + BOOT_GPU_ARENA_HEADER && (uint64_t)((const uint8_t *)p - a->base - BOOT_GPU_ARENA_HEADER) < a->payload) return a; }
    return NULL;
}
static inline int boot_gpu_pool_arena(const void *arena_base) {   /* is this arena (base) a pool arena */
    for (unsigned i = 0; i < boot_gpu_pool.count; ++i) if (boot_gpu_pool.arena[i].base == (const uint8_t *)arena_base) return 1;
    return 0;
}
static inline int boot_gpu_pool_insert(BootGpuPoolArena *a, uint64_t off, uint64_t len) {   /* free range, coalesced */
    unsigned i = 0; while (i < a->free_count && a->free[i].off < off) ++i;
    if (i && a->free[i - 1].off + a->free[i - 1].len > off) return 0;               /* overlaps: double free */
    if (i < a->free_count && off + len > a->free[i].off) return 0;
    int left = i && a->free[i - 1].off + a->free[i - 1].len == off, right = i < a->free_count && off + len == a->free[i].off;
    if (left && right) { a->free[i - 1].len += len + a->free[i].len; memmove(&a->free[i], &a->free[i + 1], (a->free_count - i - 1) * sizeof *a->free); --a->free_count; return 1; }
    if (left) { a->free[i - 1].len += len; return 1; }
    if (right) { a->free[i].off = off; a->free[i].len += len; return 1; }
    if (a->free_count == a->free_cap) { unsigned n = a->free_cap ? a->free_cap * 2 : 64; void *q = realloc(a->free, n * sizeof *a->free); if (!q) return 0; a->free = q; a->free_cap = n; }
    memmove(&a->free[i + 1], &a->free[i], (a->free_count - i) * sizeof *a->free);
    a->free[i].off = off; a->free[i].len = len; ++a->free_count; return 1;
}
/* Zero-filled pool memory for a MapPhysicalMemory region (size: multiple of 4 KiB), or NULL: the caller
 * allocates as before. */
static inline uint8_t *boot_gpu_pool_alloc(uint64_t size) {
    if (!size || (size & 4095) || boot_gpu_pool.disabled) return NULL;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < boot_gpu_pool.count; ++i) { BootGpuPoolArena *a = &boot_gpu_pool.arena[i];
            for (unsigned k = 0; k < a->free_count; ++k) { BootGpuPoolRange *f = &a->free[k];
                if (f->len < size) continue;
                uint64_t off = f->off;
                f->off += size; f->len -= size;
                if (!f->len) { memmove(f, f + 1, (a->free_count - k - 1) * sizeof *f); --a->free_count; }
                uint8_t *p = a->base + BOOT_GPU_ARENA_HEADER + off;
                if (off < a->hwm) { uint64_t dirty = (off + size < a->hwm ? off + size : a->hwm) - off; memset(p, 0, (size_t)dirty); boot_gpu_pool.reused_bytes += dirty; }
                if (off + size > a->hwm) a->hwm = off + size;
                ++boot_gpu_pool.allocs; return p;
            }
        }
        if (pass || boot_gpu_pool.count == BOOT_GPU_POOL_MAX) break;
        uint64_t payload = size > BOOT_GPU_POOL_ARENA ? size : BOOT_GPU_POOL_ARENA;
        uint8_t *base = boot_gpu_arena_new(payload);
        if (!base) return NULL;
        BootGpuPoolArena *a = &boot_gpu_pool.arena[boot_gpu_pool.count];
        memset(a, 0, sizeof *a); a->base = base; a->payload = payload;
        if (!boot_gpu_pool_insert(a, 0, payload)) { free(a->free); memset(a, 0, sizeof *a); boot_gpu_pool.disabled = 1; return NULL; }   /* the arena stays (unused) */
        ++boot_gpu_pool.count;
    }
    return NULL;
}
/* Return [p, p+size) (inside one pool arena) to the pool. 0: not pool memory. */
static inline int boot_gpu_pool_free(uint8_t *p, uint64_t size) {
    BootGpuPoolArena *a = boot_gpu_pool_find(p);
    if (!a || !size) return 0;
    uint64_t off = (uint64_t)(p - a->base - BOOT_GPU_ARENA_HEADER);
    if (off + size > a->payload) size = a->payload - off;
    if (boot_gpu_pool_insert(a, off, size)) ++boot_gpu_pool.frees;
    return 1;
}

/* Page table from the channel's GPU mappings; each backing resolved to a guest region. */
static inline int boot_draw_memory(const Region *regions, unsigned region_count, BootWindow *w, uint32_t as_fd) {
    BootRender *r = w->render;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (!m->size || m->as_fd != as_fd) continue;
        if (m->gpu < lo) lo = m->gpu;
        if (m->gpu + m->size > hi) hi = m->gpu + m->size;
    }
    if (lo >= hi) { boot_draw_log("no GPU mappings%s %llu %llu", "", as_fd, 0); return 0; }
    if (!boot_render_ptable(r, lo & ~UINT64_C(0xfff), hi)) { boot_draw_log("page table: %s %llu %llu", r->error, lo, hi); return 0; }
    memset(boot_render_region_used, 0, sizeof boot_render_region_used);   /* the page table was just cleared */
    for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (!m->size || m->as_fd != as_fd || !m->cpu) continue;
        uint64_t backing = boot_gpu_mapping_backing_size(m);
        unsigned k = 0;
        while (k < region_count && !(m->cpu >= regions[k].base && m->cpu - regions[k].base < regions[k].size)) ++k;
        if (k == region_count) { boot_draw_log("mapping without guest region%s gpu %llx cpu %llx", "", m->gpu, m->cpu); return 0; }
        const Region *g = &regions[k];
        /* heap, or physical memory the submit path moved into a GPU arena / VirtualAlloc block */
        uint8_t *block = g->data; uint64_t block_size = g->size;
        if (g->reserved != 3 && !(g->reserved == 1 && boot_gpu_block(g->data, g->size, &block, &block_size))) {
            boot_draw_log("GPU mapping backed by non-shareable region%s gpu %llx type %llu", "", m->gpu, g->reserved); return 0; }
        int idx = boot_render_region(r, block, block_size);
        if (idx < 0) { boot_draw_log("region: %s %llu %llu", r->error, g->base, g->size); return 0; }
        uint64_t off = (uint64_t)(g->data - block) + (m->cpu - g->base);
        if (backing > g->size - (m->cpu - g->base)) backing = g->size - (m->cpu - g->base);
        if (!boot_render_map(r, m->gpu, (unsigned)idx, off, backing)) { boot_draw_log("map%s gpu %llx off %llx", "", m->gpu, off); return 0; }
    }
    return 1;
}

/* Byte offset of (x bytes, y) in a block-linear surface (GOB layout as host_texture_layout.h). */
static inline uint64_t boot_bl_offset(uint32_t x, uint32_t y, uint32_t width_bytes, unsigned log_bh) {
    uint64_t bh = (uint64_t)1 << log_bh, gx = (width_bytes + 63) / 64;
    return (y / (8 * bh)) * gx * 512 * bh + (x / 64) * 512 * bh + ((y % (8 * bh)) / 8) * 512 +
           ((x & 63) / 32) * 256 + ((y & 7) / 2) * 64 + ((x & 31) / 16) * 32 + (y & 1) * 16 + (x & 15);
}

/* Byte offset of (x bytes, y, z) in a block-linear surface of width_bytes x height rows with blocks of
 * 2^log_bh GOBs by 2^log_bd slices (suyu SwizzleImpl addressing; z = 0 and log_bd = 0 is the 2D case). */
static inline uint64_t boot_bl_offset3(uint32_t x, uint32_t y, uint32_t z, uint32_t width_bytes, uint32_t height, unsigned log_bh, unsigned log_bd) {
    uint64_t block = (uint64_t)512 << (log_bh + log_bd), gx = ((uint64_t)width_bytes + 63) / 64, gy = ((uint64_t)height + (8u << log_bh) - 1) >> (3 + log_bh);
    return (uint64_t)(z >> log_bd) * gx * gy * block + ((uint64_t)(z & ((1u << log_bd) - 1)) << (9 + log_bh)) +
           (uint64_t)(y >> (3 + log_bh)) * gx * block + ((uint64_t)((y >> 3) & ((1u << log_bh) - 1)) << 9) +
           (uint64_t)(x / 64) * block + ((x & 63) / 32) * 256 + ((y & 7) / 2) * 64 + ((x & 31) / 16) * 32 + (y & 1) * 16 + (x & 15);
}
static inline uint64_t boot_bl_size3(uint32_t width_bytes, uint32_t height, uint32_t depth, unsigned log_bh, unsigned log_bd) {
    uint64_t gx = ((uint64_t)width_bytes + 63) / 64, gy = ((uint64_t)height + (8u << log_bh) - 1) >> (3 + log_bh);
    uint64_t gz = ((uint64_t)depth + (1u << log_bd) - 1) >> log_bd;
    return gx * gy * gz * ((uint64_t)512 << (log_bh + log_bd));
}

/* NVB0B5 LAUNCH_DMA on the CPU, in submission order. Supported: pitch<->pitch, pitch->block-linear,
 * block-linear->pitch (2D, one layer), constant fills via remap into pitch memory, semaphores. */
static inline int boot_draw_copy_execute(BootRender *r, const BootCopyParams *c) {
    uint32_t transfer = c->launch & 3, sem = (c->launch >> 3) & 3, src_pitch = (c->launch >> 7) & 1, dst_pitch = (c->launch >> 8) & 1;
    uint32_t multi = (c->launch >> 9) & 1, remap = (c->launch >> 10) & 1;
    uint64_t in = ((uint64_t)(c->offset_in_hi & 0xff) << 32) | c->offset_in_lo, out = ((uint64_t)(c->offset_out_hi & 0xff) << 32) | c->offset_out_lo;
    uint32_t len = c->line_length, lines = multi ? c->line_count : 1;
    if (transfer) {
        if ((c->launch >> 19) & 1) { boot_draw_log("copy: semaphore reduction%s %llu %llu", "", c->launch, 0); return 0; }
        if (remap || (src_pitch && dst_pitch)) {
            /* host render targets over the pitch ranges: GPU writes reach guest memory first */
            uint64_t span_in = (uint64_t)(lines ? lines - 1 : 0) * c->pitch_in + (uint64_t)len * 16, span_out = (uint64_t)(lines ? lines - 1 : 0) * c->pitch_out + (uint64_t)len * 16;
            if (!boot_render_flush_targets(r, in, span_in) || !boot_render_flush_targets(r, out, span_out)) { boot_draw_log("copy: render target write-back: %s %llx %llx", r->error, in, out); return 0; }
        }
        if (remap) {
            uint32_t cs = ((c->remap >> 16) & 3) + 1, nsrc = ((c->remap >> 20) & 3) + 1, ndst = ((c->remap >> 24) & 3) + 1;
            if (!dst_pitch) { boot_draw_log("copy: remap into block-linear%s %llu %llu", "", c->remap, 0); return 0; }
            for (uint32_t y = 0; y < lines; ++y) {
                uint8_t *d = boot_render_guest(r, out + (uint64_t)y * c->pitch_out, (uint64_t)len * cs * ndst);
                const uint8_t *sp = NULL;
                for (uint32_t k = 0; k < ndst; ++k)
                    if (((c->remap >> (4 * k)) & 7) < 4) sp = boot_render_guest(r, in + (uint64_t)y * c->pitch_in, (uint64_t)len * cs * nsrc);
                if (!d) { boot_draw_log("copy: remap destination%s %llx %llu", "", out, len); return 0; }
                for (uint32_t x = 0; x < len; ++x) for (uint32_t k = 0; k < ndst; ++k) {
                    uint32_t sel = (c->remap >> (4 * k)) & 7, v = 0;
                    if (sel == 6) continue;   /* NO_WRITE */
                    if (sel == 4) v = c->remap_const_a; else if (sel == 5) v = c->remap_const_b;
                    else if (sel < 4 && sp && sel < nsrc) memcpy(&v, sp + ((size_t)x * nsrc + sel) * cs, cs);
                    else { boot_draw_log("copy: remap selector%s %llu %llu", "", sel, nsrc); return 0; }
                    memcpy(d + ((size_t)x * ndst + k) * cs, &v, cs);
                }
            }
            boot_render_invalidate(out, (uint64_t)lines * (c->pitch_out ? c->pitch_out : len * cs * ndst));
            boot_render_rt_guest_written(r, out, (uint64_t)lines * (c->pitch_out ? c->pitch_out : len * cs * ndst));
        } else if (src_pitch && dst_pitch) {
            for (uint32_t y = 0; y < lines; ++y) {
                const uint8_t *sp = boot_render_guest(r, in + (uint64_t)y * c->pitch_in, len);
                uint8_t *d = boot_render_guest(r, out + (uint64_t)y * c->pitch_out, len);
                if (!sp || !d) { boot_draw_log("copy: pitch range%s %llx %llx", "", in, out); return 0; }
                memmove(d, sp, len);
            }
            boot_render_invalidate(out, (uint64_t)(lines - 1) * c->pitch_out + len);
            boot_render_rt_guest_written(r, out, (uint64_t)(lines - 1) * c->pitch_out + len);
        } else {
            /* Block-linear on one or both sides: byte (x, y) of layer z through the GOB/block layout,
             * blocks 2^bh GOBs tall and 2^bd slices deep (NVB0B5 SET_*_BLOCK_SIZE, SET_*_LAYER = z). */
            const uint32_t sblock = c->src_block, dblock = c->dst_block;
            const unsigned sbh = (sblock >> 4) & 15, sbd = (sblock >> 8) & 15, dbh = (dblock >> 4) & 15, dbd = (dblock >> 8) & 15;
            const uint32_t sdepth = c->src_depth ? c->src_depth : 1, ddepth = c->dst_depth ? c->dst_depth : 1;
            if ((!src_pitch && ((sblock & 15) || sbh > 5 || sbd > 5 || c->src_layer >= sdepth)) ||
                (!dst_pitch && ((dblock & 15) || dbh > 5 || dbd > 5 || c->dst_layer >= ddepth))) {
                boot_draw_log("copy: block-linear parameters%s %llx %llx", "", ((uint64_t)sblock << 32) | c->src_layer, ((uint64_t)dblock << 32) | c->dst_layer); return 0; }
            const uint32_t sox = c->src_origin & 0xffff, soy = c->src_origin >> 16, dox = c->dst_origin & 0xffff, doy = c->dst_origin >> 16;
            uint64_t stot = src_pitch ? (uint64_t)(lines - 1) * c->pitch_in + len : boot_bl_size3(c->src_width, c->src_height, sdepth, sbh, sbd);
            uint64_t dtot = dst_pitch ? (uint64_t)(lines - 1) * c->pitch_out + len : boot_bl_size3(c->dst_width, c->dst_height, ddepth, dbh, dbd);
            /* host render targets over either side: GPU writes reach guest memory first */
            if (!boot_render_flush_targets(r, in, stot) || !boot_render_flush_targets(r, out, dtot)) { boot_draw_log("copy: render target write-back: %s %llx %llx", r->error, in, out); return 0; }
            uint8_t *sbl = src_pitch ? NULL : boot_render_guest(r, in, stot), *dbl = dst_pitch ? NULL : boot_render_guest(r, out, dtot);
            if ((!src_pitch && !sbl) || (!dst_pitch && !dbl)) { boot_draw_log("copy: block-linear surface%s %llx %llx", "", src_pitch ? out : in, src_pitch ? dtot : stot); return 0; }
            for (uint32_t y = 0; y < lines; ++y) {
                const uint8_t *srow = src_pitch ? boot_render_guest(r, in + (uint64_t)y * c->pitch_in, len) : NULL;
                uint8_t *drow = dst_pitch ? boot_render_guest(r, out + (uint64_t)y * c->pitch_out, len) : NULL;
                if ((src_pitch && !srow) || (dst_pitch && !drow)) { boot_draw_log("copy: pitch side%s %llx %llu", "", src_pitch ? in : out, y); return 0; }
                for (uint32_t x = 0; x < len; ) {
                    uint32_t run = len - x;
                    if (!src_pitch) { uint32_t bx = sox + x; if (16 - (bx & 15) < run) run = 16 - (bx & 15);
                        if (bx + run > c->src_width || soy + y >= c->src_height) { boot_draw_log("copy: outside source surface%s %llu %llu", "", bx, soy + y); return 0; } }
                    if (!dst_pitch) { uint32_t bx = dox + x; if (16 - (bx & 15) < run) run = 16 - (bx & 15);
                        if (bx + run > c->dst_width || doy + y >= c->dst_height) { boot_draw_log("copy: outside destination surface%s %llu %llu", "", bx, doy + y); return 0; } }
                    const uint8_t *sp = src_pitch ? srow + x : sbl + boot_bl_offset3(sox + x, soy + y, c->src_layer, c->src_width, c->src_height, sbh, sbd);
                    uint8_t *dp = dst_pitch ? drow + x : dbl + boot_bl_offset3(dox + x, doy + y, c->dst_layer, c->dst_width, c->dst_height, dbh, dbd);
                    memmove(dp, sp, run);
                    x += run;
                }
            }
            boot_render_invalidate(out, dtot);
            boot_render_rt_guest_written(r, out, dtot);
        }
    }
    if (sem) {
        uint64_t sa = ((uint64_t)(c->sem_a & 0xff) << 32) | c->sem_b;
        uint8_t *sp = boot_render_guest(r, sa, sem == 2 ? 16 : 4);
        if (!sp) { boot_draw_log("copy: semaphore address%s %llx %llu", "", sa, sem); return 0; }
        memcpy(sp, &c->sem_payload, 4);
        if (sem == 2) {
            uint32_t z = 0; uint64_t ts;
            if (boot_guest_dilated()) ts = boot_guest_gpu_ticks();   /* guest clock, GPU timer units */
            else { LARGE_INTEGER q; QueryPerformanceCounter(&q); ts = (uint64_t)q.QuadPart; }
            memcpy(sp + 4, &z, 4); memcpy(sp + 8, &ts, 8);
        }
        boot_render_cb_written(sa, sem == 2 ? 16 : 4);
    }
    return 1;
}

/* Applies staged plain stores [*applied, upto) to guest memory (idempotent: the regular commit
 * path writes the same values again later). Keeps copies and draws ordered after earlier uploads. */
static inline int boot_draw_apply_writes(BootRender *r, const BootGpuStage *stage, uint32_t *applied, uint32_t upto) {
    for (; *applied < upto; ++*applied) {
        const BootGpuMemoryWrite *w = &boot_gpu_stage_writes((BootGpuStage *)stage)[*applied];
        if (w->kind != 0) continue;
        uint8_t *p = boot_render_guest(r, w->gpu, 4);
        if (!p) { boot_draw_log("staged write outside mapped guest memory%s %llx %llu", "", w->gpu, *applied); return 0; }
        memcpy(p, &w->value, 4);
        boot_render_cb_written(w->gpu, 4);   /* snapshots of this range taken earlier in the list are stale */
    }
    return 1;
}

/* Draw trace (diagnostics, off by default): creating frames\draw-trace.request (optionally holding
 * an event count, default 400, and optionally the number of a submission's events to skip first) logs the next events' state to frames\draw-trace.txt, with D3D12
 * pipeline statistics per executed draw (vertices in, primitives rasterized, pixel invocations). */
static FILE *boot_draw_trace_file; static unsigned boot_draw_trace_left, boot_draw_trace_seq, boot_draw_trace_skip;
static uint32_t boot_draw_trace_ids[BOOT_RENDER_TRACE_QUERIES];
static inline void boot_draw_trace_arm(BootRender *r) {
    /* Bisect switch: while frames\draw-constps.request exists, every pixel shader is replaced by
     * startup\diag-shaders\const.ps.dxil (constant magenta to all targets). */
    boot_render_trace.const_ps_on = GetFileAttributesA("frames\\draw-constps.request") != INVALID_FILE_ATTRIBUTES;
    if (boot_render_trace.const_ps_on && !boot_render_trace.const_ps) {
        FILE *b = fopen("startup\\diag-shaders\\const.ps.dxil", "rb");
        if (b) { fseek(b, 0, SEEK_END); long n = ftell(b); fseek(b, 0, SEEK_SET);
            void *data = n > 0 ? malloc((size_t)n) : NULL;
            if (data && fread(data, 1, (size_t)n, b) == (size_t)n) { boot_render_trace.const_ps = data; boot_render_trace.const_ps_size = (size_t)n; } else free(data);
            fclose(b); }
    }
    if (boot_draw_trace_left || GetFileAttributesA("frames\\draw-trace.request") == INVALID_FILE_ATTRIBUTES) return;
    unsigned n = 400; FILE *q = fopen("frames\\draw-trace.request", "r");
    boot_draw_trace_skip = 0;   /* optional second number: events of a submission to skip before tracing (reach its last draws) */
    if (q) { unsigned v, k; if (fscanf(q, "%u", &v) == 1 && v) n = v; if (fscanf(q, "%u", &k) == 1) boot_draw_trace_skip = k; fclose(q); }
    DeleteFileA("frames\\draw-trace.request");
    if (!boot_render_trace.qh) {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, BOOT_RENDER_TRACE_QUERIES, 0};
        if (FAILED(ID3D12Device_CreateQueryHeap(r->gpu->device, &qd, &IID_ID3D12QueryHeap, (void **)&boot_render_trace.qh))) return;
        boot_render_trace.rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, (uint64_t)BOOT_RENDER_TRACE_QUERIES * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                                                  D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        if (!boot_render_trace.rb) return;
    }
    boot_draw_trace_file = fopen("frames\\draw-trace.txt", "w");
    if (boot_draw_trace_file) boot_draw_trace_left = n;
}
static inline void boot_draw_trace_hex(FILE *f, const uint8_t *id, unsigned n) { for (unsigned i = 0; i < n; ++i) fprintf(f, "%02x", id[i]); }
static inline void boot_draw_trace_event(BootRender *r, const BootDrawEvent *e, uint32_t index, uint32_t count) {
    FILE *f = boot_draw_trace_file; const BootDrawState *s = &e->state;
    fprintf(f, "\n#%u event %u/%u kind %u", boot_draw_trace_seq, index, count, e->kind);
    if (e->kind == 4) { const BootComputeParams *c = &e->compute;
        fprintf(f, " compute qmd %llx program region %llx tic %llx tsc %llx bindless cb %u\n", (unsigned long long)c->qmd, (unsigned long long)c->program_region,
                (unsigned long long)c->tic_address, (unsigned long long)c->tsc_address, c->bindless_cb); return; }
    if (e->kind == 3) { const BootCopyParams *c = &e->copy;
        fprintf(f, " copy launch %x in %x:%08x out %x:%08x len %u lines %u\n", c->launch, c->offset_in_hi, c->offset_in_lo, c->offset_out_hi, c->offset_out_lo, c->line_length, c->line_count); return; }
    if (e->kind == 2) fprintf(f, " clear value %x rgba %08x %08x %08x %08x z %08x", e->clear_value, e->clear_regs[0], e->clear_regs[1], e->clear_regs[2], e->clear_regs[3], e->clear_regs[4]);
    else fprintf(f, " topo %u indexed %u isize %u first %u count %u inst %u basev %u basei %u", s->topology, s->indexed, s->index_size, s->first, s->count, e->instance, s->base_vertex_reg, s->base_instance_reg);
    fprintf(f, " unsupported %x\n  rt_count %u map", s->unsupported, s->rt_count);
    for (unsigned i = 0; i < 8; ++i) fprintf(f, " %u", s->rt_map[i]);
    fprintf(f, " single_ct_write %u srgb %u\n", s->single_ct_write, s->srgb_write);
    for (unsigned i = 0; i < 8; ++i) { const BootColorTarget *t = &s->rt[i];
        if (!t->address && !t->format) continue;
        fprintf(f, "  ct%u %llx %ux%u fmt %x pitch %u gob %u/%u/%u layers %u layer %u mask %x\n", i, (unsigned long long)t->address, t->width, t->height, t->format,
                t->pitch_layout, t->log_gob_w, t->log_gob_h, t->log_gob_d, t->layers, t->layer, t->write_mask); }
    fprintf(f, "  zt en %u %llx %ux%u fmt %x | depth test %u write %u func %x | stencil %u func %x ref %x masks %x/%x ops %x %x %x\n", s->zt.enabled,
            (unsigned long long)s->zt.address, s->zt.width, s->zt.height, s->zt.format, s->depth_test, s->depth_write, s->depth_func, s->stencil_test,
            s->stencil_func, s->stencil_ref, s->stencil_func_mask, s->stencil_mask, s->stencil_fail, s->stencil_zfail, s->stencil_zpass);
    const BootViewport *v = &s->viewport[0];
    fprintf(f, "  vp0 scale %g %g %g offset %g %g %g rect %u,%u %ux%u z %g..%g | z01 %u origin_ll %u flip_y %u | scissor0 en %u %u..%u %u..%u\n",
            v->scale[0], v->scale[1], v->scale[2], v->offset[0], v->offset[1], v->offset[2], v->x, v->y, v->w, v->h, v->min_z, v->max_z,
            s->z_clip_zero_to_w, s->window_origin_lower_left, s->flip_y, s->scissor[0].enabled, s->scissor[0].x0, s->scissor[0].x1, s->scissor[0].y0, s->scissor[0].y1);
    if (e->kind != 1) return;
    fprintf(f, "  cull %u face %x front %x | blend per_target %u", s->cull_enable, s->cull_face, s->front_face, s->blend_per_target);
    for (unsigned i = 0; i < s->rt_count && i < 8; ++i) { const BootBlend *b = &s->blend[i];
        fprintf(f, " [%u en %u sep %u c %x %x %x a %x %x %x]", i, b->enabled, b->separate_alpha, b->color_op, b->color_src, b->color_dst, b->alpha_op, b->alpha_src, b->alpha_dst); }
    fprintf(f, " const %g %g %g %g\n  stages", s->blend_const[0], s->blend_const[1], s->blend_const[2], s->blend_const[3]);
    for (unsigned j = 0; j < 6; ++j) fprintf(f, " %u:%u/%x", s->stage_enabled[j], s->stage_type[j], s->stage_offset[j]);
    fprintf(f, " program %llx tic %llx tsc %llx\n", (unsigned long long)s->program_base, (unsigned long long)s->tic_address, (unsigned long long)s->tsc_address);
    for (unsigned i = 0; i < 16; ++i) { const BootVertexStream *st = &s->stream[i];
        if (!st->enabled) continue;
        fprintf(f, "  stream%u %llx stride %u div %u%s bytes", i, (unsigned long long)st->address, st->stride, st->divisor, st->per_instance ? " instanced" : "");
        const uint8_t *p = boot_render_guest(r, st->address + (uint64_t)(s->indexed ? s->base_vertex_reg : s->first) * st->stride, 32);
        if (p) for (unsigned k = 0; k < 32; ++k) fprintf(f, "%s%02x", k % 4 ? "" : " ", p[k]); else fprintf(f, " (unmapped)");
        fputc('\n', f); }
    fprintf(f, "  attribs");
    for (unsigned i = 0; i < 32; ++i) { const BootVertexAttrib *a = &s->attrib[i];
        if (!a->inactive && s->stream[a->stream & 15].enabled) fprintf(f, " %u:s%u+%u sz%x ty%u%s", i, a->stream, a->offset, a->size, a->type, a->bgra ? "bgra" : ""); }
    fputc('\n', f);
    if (s->indexed) {
        uint32_t ib = s->index_size == 0 ? 1 : s->index_size == 1 ? 2 : 4;
        const uint8_t *p = boot_render_guest(r, s->index_address + (uint64_t)s->first * ib, 12 * ib);
        fprintf(f, "  index %llx first indices", (unsigned long long)s->index_address);
        for (unsigned k = 0; p && k < 12; ++k) { uint32_t x = 0; memcpy(&x, p + k * ib, ib); fprintf(f, " %u", x); }
        fputc('\n', f);
    }
    for (unsigned g = 0; g < 5; ++g) for (unsigned slot = 0; slot < 18; ++slot) {
        const BootCbufBinding *b = &e->cbufs[g][slot];
        if (!b->valid) continue;
        fprintf(f, "  cb g%u s%u %llx size %u:", g, slot, (unsigned long long)b->address, b->size);
        const uint8_t *p = boot_render_guest(r, b->address, 32);
        for (unsigned k = 0; p && k < 8; ++k) { uint32_t x; memcpy(&x, p + 4 * k, 4); fprintf(f, " %08x(%g)", x, boot_f32(x)); }
        if (slot == 0 || slot == 2) {   /* calibration (nvn-work): driver buffers, nonzero words */
            uint32_t lo = slot ? 0 : 0x100, hi = slot ? 0x200 : 0x800; if (hi > b->size) hi = b->size;
            const uint8_t *q = hi > lo ? boot_render_guest(r, b->address + lo, hi - lo) : NULL;
            for (uint32_t k = 0; q && k < (hi - lo) / 4; ++k) { uint32_t x; memcpy(&x, q + 4 * k, 4); if (x) fprintf(f, " [%x]=%08x", lo + 4 * k, x); }
        }
        /* sun-work2 diagnostic (off unless set): TOTK_TRACE_CB_FULL="4,5,1" prints every nonzero word of those slots
         * (up to 4 KiB, the guest bytes as the CPU sees them) as [byte offset]=hex(float). */
        {   static uint32_t full_mask = UINT32_MAX;
            if (full_mask == UINT32_MAX) {
                char v[64]; DWORD n = GetEnvironmentVariableA("TOTK_TRACE_CB_FULL", v, sizeof v); full_mask = 0;
                for (DWORD c = 0; n && n < sizeof v && c < n; ) {
                    unsigned s = 0, digits = 0; while (c < n && v[c] >= '0' && v[c] <= '9') { s = s * 10 + (unsigned)(v[c++] - '0'); ++digits; }
                    if (digits && s < 18) full_mask |= 1u << s;
                    while (c < n && (v[c] < '0' || v[c] > '9')) ++c;
                }
            }
            if ((full_mask >> slot) & 1u) {
                uint32_t hi = b->size < 4096u ? b->size : 4096u;
                const uint8_t *q = hi ? boot_render_guest(r, b->address, hi) : NULL;
                fprintf(f, "\n    full:");
                for (uint32_t k = 0; q && k < hi / 4; ++k) { uint32_t x; memcpy(&x, q + 4 * k, 4); if (x) fprintf(f, " [%x]=%08x(%g)", 4 * k, x, boot_f32(x)); }
            } }
        fputc('\n', f);
    }
}
/* After the submission completed: per-draw shader ids, viewport and pipeline statistics. */
/* Traced draws also copy the first 32 bytes of vertex stream 0 out of the guest region resource on
 * the GPU (the zero-copy view the shaders read through), to compare with what the CPU sees there. */
#define BOOT_DRAW_TRACE_VPROBES 1024
static ID3D12Resource *boot_draw_trace_vrb; static unsigned boot_draw_trace_vcount; static uint32_t boot_draw_trace_so_seq[BOOT_RENDER_SO_PROBES];
static struct { uint32_t seq; uint64_t va; uint8_t cpu[32]; } boot_draw_trace_vp[BOOT_DRAW_TRACE_VPROBES];
static inline void boot_draw_trace_vprobe(BootRender *r, const BootDrawState *s) {
    const BootVertexStream *st = &s->stream[0];
    if (!st->enabled || boot_draw_trace_vcount >= BOOT_DRAW_TRACE_VPROBES) return;
    uint64_t va = st->address + (uint64_t)(s->indexed ? s->base_vertex_reg : s->first) * st->stride;
    if ((va >> 12) < r->va_base_page || (va >> 12) - r->va_base_page >= r->ptable_pages) return;
    uint32_t e = r->ptable_cpu[(va >> 12) - r->va_base_page];
    if (e == 0xffffffffu || (e >> 24) >= r->region_count || !r->region[e >> 24]) return;
    uint64_t off = ((uint64_t)(e & 0xffffff) << 12) | (va & 0xfff);
    const uint8_t *cpu = boot_render_guest(r, va, 32);
    if (!cpu || off + 32 > r->region_size[e >> 24]) return;
    if (!boot_draw_trace_vrb) boot_draw_trace_vrb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, (uint64_t)BOOT_DRAW_TRACE_VPROBES * 32, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!boot_draw_trace_vrb || !boot_render_begin(r)) return;
    unsigned k = boot_draw_trace_vcount++;
    boot_render_gw_settle(r);
    ID3D12GraphicsCommandList_CopyBufferRegion(r->cl, boot_draw_trace_vrb, (UINT64)k * 32, r->region[e >> 24], off, 32);
    boot_draw_trace_vp[k].seq = boot_draw_trace_seq; boot_draw_trace_vp[k].va = va; memcpy(boot_draw_trace_vp[k].cpu, cpu, 32);
}
/* Per-tile flag words of TOTK's tiled passes (diagnostic, traced draws only): their vertex programs cull
 * the instance's 64x64 tile by guest words at c0[0x110]+0x1c0+16*instance (flags set by G-buffer pixel
 * programs) and c0[0x150]+0x41c0+16*instance (set by the tiled pass c2741faf). Read after the submission
 * completed (the frame's final values) and printed as "tileflags #seq inst N: a[...] b[...]". */
#define BOOT_DRAW_TRACE_TILEFLAGS 1024
static struct { uint32_t seq, inst; uint64_t va[2]; } boot_draw_trace_tf[BOOT_DRAW_TRACE_TILEFLAGS]; static unsigned boot_draw_trace_tfcount;
static inline void boot_draw_trace_tileflags(BootRender *r, const BootDrawEvent *e) {
    const BootCbufBinding *c0 = &e->cbufs[0][0];
    if (!c0->valid || c0->size < 0x158 || e->instance >= 4096 || boot_draw_trace_tfcount >= BOOT_DRAW_TRACE_TILEFLAGS) return;
    const uint8_t *p = boot_render_guest(r, c0->address + 0x110, 0x48);
    if (!p) return;
    uint64_t a, b; memcpy(&a, p, 8); memcpy(&b, p + 0x40, 8);
    unsigned k = boot_draw_trace_tfcount++;
    boot_draw_trace_tf[k].seq = boot_draw_trace_seq; boot_draw_trace_tf[k].inst = e->instance;
    boot_draw_trace_tf[k].va[0] = a ? a + 0x1c0 + 16ull * e->instance : 0; boot_draw_trace_tf[k].va[1] = b ? b + 0x41c0 + 16ull * e->instance : 0;
}
static inline void boot_draw_trace_flush(BootRender *r, unsigned queries) {
    FILE *f = boot_draw_trace_file; if (!f) return;
    {   /* tile flags (see boot_draw_trace_tileflags): the frame's final values */
        for (unsigned k = 0; k < boot_draw_trace_tfcount; ++k) {
            fprintf(f, "tileflags #%u inst %u:", boot_draw_trace_tf[k].seq, boot_draw_trace_tf[k].inst);
            for (unsigned j = 0; j < 2; ++j) {
                const uint8_t *g = boot_draw_trace_tf[k].va[j] ? boot_render_guest(r, boot_draw_trace_tf[k].va[j], 16) : NULL;
                uint32_t w[4] = {0, 0, 0, 0}; if (g) memcpy(w, g, 16);
                if (g) fprintf(f, " %c %llx [%x %x %x %x]", 'a' + j, (unsigned long long)boot_draw_trace_tf[k].va[j], w[0], w[1], w[2], w[3]);
                else fprintf(f, " %c -", 'a' + j);
            }
            fputc('\n', f);
        }
        boot_draw_trace_tfcount = 0;
    }
    {   uint8_t *g = NULL; D3D12_RANGE vr = {0, (SIZE_T)boot_draw_trace_vcount * 32};
        if (boot_draw_trace_vcount && SUCCEEDED(ID3D12Resource_Map(boot_draw_trace_vrb, 0, &vr, (void **)&g))) {
            for (unsigned k = 0; k < boot_draw_trace_vcount; ++k) {
                int same = !memcmp(g + 32 * k, boot_draw_trace_vp[k].cpu, 32);
                fprintf(f, "vprobe #%u stream0 %llx: GPU view %s", boot_draw_trace_vp[k].seq, (unsigned long long)boot_draw_trace_vp[k].va, same ? "matches CPU" : "DIFFERS:");
                if (!same) { for (unsigned b = 0; b < 32; ++b) fprintf(f, "%s%02x", b % 4 ? "" : " ", g[32 * k + b]); }
                fputc('\n', f);
            }
            D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(boot_draw_trace_vrb, 0, &none);
        }
        boot_draw_trace_vcount = 0; }
    {   /* streamed-out clip-space positions (what the rasterizer received), first 6 vertices per draw */
        BootRenderTrace *tr = &boot_render_trace; uint8_t *g = NULL; D3D12_RANGE sr = {0, (SIZE_T)tr->so_count * BOOT_RENDER_SO_SLOT};
        if (tr->so_count && SUCCEEDED(ID3D12Resource_Map(tr->so_rb, 0, &sr, (void **)&g))) {
            for (unsigned k = 0; k < tr->so_count; ++k) {
                const uint8_t *slot = g + (size_t)k * BOOT_RENDER_SO_SLOT; uint64_t filled; memcpy(&filled, slot, 8);
                fprintf(f, "positions #%u: %llu bytes streamed out:", boot_draw_trace_so_seq[k], (unsigned long long)filled);
                for (unsigned v = 0; v < 6 && (v + 1) * 16 <= filled && (v + 1) * 16 <= BOOT_RENDER_SO_SLOT - 16; ++v) {
                    float q[4]; memcpy(q, slot + 16 + 16 * v, 16); fprintf(f, " (%g %g %g %g)", q[0], q[1], q[2], q[3]); }
                fputc('\n', f);
            }
            D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(tr->so_rb, 0, &none);
        }
        boot_render_trace_so_release(); }
    D3D12_QUERY_DATA_PIPELINE_STATISTICS *q = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)queries * sizeof *q};
    if (queries && SUCCEEDED(ID3D12Resource_Map(boot_render_trace.rb, 0, &rr, (void **)&q))) {
        for (unsigned i = 0; i < queries; ++i)
            fprintf(f, "stats #%u: IA verts %llu prims %llu VS %llu clip in %llu clip out %llu PS %llu\n", boot_draw_trace_ids[i],
                    (unsigned long long)q[i].IAVertices, (unsigned long long)q[i].IAPrimitives, (unsigned long long)q[i].VSInvocations,
                    (unsigned long long)q[i].CInvocations, (unsigned long long)q[i].CPrimitives, (unsigned long long)q[i].PSInvocations);
        D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(boot_render_trace.rb, 0, &none);
    }
    fflush(f);
    (void)r;
}

/* ZPASS_PIXEL_CNT per channel: the counter value after the last executed submission (a report
 * without a reset in its own submission continues from it). */
static uint64_t boot_draw_zpass_counter[16];
static inline int boot_draw_zpass_commit(BootGpuStage *stage, unsigned channel, uint32_t slots) {
    uint64_t *counts = NULL; uint32_t reports = 0;
    D3D12_RANGE rr = {0, (SIZE_T)slots * 8};
    if (slots && FAILED(ID3D12Resource_Map(boot_render_occl.rb, 0, &rr, (void **)&counts))) { boot_draw_log("occlusion readback map failed%s %llu %llu", "", slots, 0); return 0; }
    int ok = boot_gpu_stage_zpass_resolve(stage, counts, slots, &boot_draw_zpass_counter[channel & 15], &reports);
    if (slots) { D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(boot_render_occl.rb, 0, &none); }
    if (!ok) { boot_draw_log("occlusion report resolve failed%s (queries %llu, events %llu)", "", slots, stage->draw.count); return 0; }
    static uint64_t logged; if (reports && (++logged <= 16 || logged % 1000 == 0))
        boot_draw_log("occlusion reports%s: %llu in submission, counter %llu", "", reports, boot_draw_zpass_counter[channel & 15]);
    return 1;
}

/* Memory accounting appended to the perf3 line (absolute values; host GPU objects are charged to
 * process commit, video memory included): texture slots (resident, LRU evictions since the last
 * line), texture pool, render targets and spares, staging rings, GPU write-back staging, memory
 * batch pool, DXGI local/non-local usage against the budget, process private bytes, system commit. */
static BootGpu *boot_draw_perf_gpu;
static inline void boot_draw_perf_memory(FILE *f) {
    static uint64_t l_ev, l_evb, l_mb;
    uint64_t up = 0, rb = 0, gt = 0, mb = 0, spare = 0; unsigned spares = 0;
    for (unsigned i = 0; i < boot_render_up_ring.count; ++i) up += boot_render_up_ring.size[i];
    for (unsigned i = 0; i < boot_render_rb_ring.count; ++i) rb += boot_render_rb_ring.size[i];
    for (unsigned i = 0; i < boot_render_gt.chunks; ++i) if (boot_render_gt.chunk[i]) gt += boot_render_gt.chunk_size[i];
    for (unsigned i = 0; i < 16; ++i) if (boot_gpu_batch_pool.spans[i].upload) mb += 3 * (uint64_t)boot_gpu_batch_pool.spans[i].capacity;
    for (unsigned k = 0; k < BOOT_RENDER_RT_SPARES; ++k) if (boot_render_rt_spares[k].texture) {
        D3D12_RESOURCE_DESC d; boot_render_rt_spares[k].texture->lpVtbl->GetDesc(boot_render_rt_spares[k].texture, &d); spare += boot_render_tex_estimate(&d); ++spares; }
    unsigned pooled = 0; for (unsigned k = 0; k < BOOT_RENDER_TEX_POOL; ++k) if (boot_render_tex_pool[k].res) ++pooled;
    fprintf(f, "; memory: textures %llu (%llu MiB, peak %llu MiB, evicted %llu / %llu MiB), texture pool %u (%llu MiB), rt spares %u (%llu MiB), "
               "staging upload %u chunks %llu MiB / readback %u chunks %llu MiB, write-back staging %llu MiB, memory batch pool %llu MiB (+%llu created)",
            (unsigned long long)boot_render_tex_resident_count, (unsigned long long)(boot_render_tex_resident >> 20), (unsigned long long)(boot_render_tex_resident_peak >> 20),
            (unsigned long long)(boot_render_tex_evictions - l_ev), (unsigned long long)((boot_render_tex_evicted_bytes - l_evb) >> 20),
            pooled, (unsigned long long)(boot_render_tex_pool_bytes >> 20), spares, (unsigned long long)(spare >> 20),
            boot_render_up_ring.count, (unsigned long long)(up >> 20), boot_render_rb_ring.count, (unsigned long long)(rb >> 20),
            (unsigned long long)(gt >> 20), (unsigned long long)(mb >> 20), (unsigned long long)(boot_gpu_batch_pool.created - l_mb));
    l_ev = boot_render_tex_evictions; l_evb = boot_render_tex_evicted_bytes; l_mb = boot_gpu_batch_pool.created;
    {   /* DXGI video memory of this process (adapter of the render device, looked up once per device) */
        static ID3D12Device *dev; static IDXGIAdapter3 *ad;
        BootGpu *g = boot_draw_perf_gpu;
        if (g && g->device != dev) {
            if (ad) { IDXGIAdapter3_Release(ad); ad = NULL; }
            dev = g->device;
            IDXGIFactory4 *fac = NULL;
            if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&fac))) {
                LUID luid = g->adapter.AdapterLuid;
                if (FAILED(IDXGIFactory4_EnumAdapterByLuid(fac, luid, &IID_IDXGIAdapter3, (void **)&ad))) ad = NULL;
                IDXGIFactory4_Release(fac);
            }
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO lo = {0}, nl = {0};
        if (ad && SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(ad, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &lo)) &&
            SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(ad, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl)))
            fprintf(f, ", video memory local %llu / budget %llu MiB, non-local %llu / budget %llu MiB",
                    (unsigned long long)(lo.CurrentUsage >> 20), (unsigned long long)(lo.Budget >> 20),
                    (unsigned long long)(nl.CurrentUsage >> 20), (unsigned long long)(nl.Budget >> 20));
    }
    PROCESS_MEMORY_COUNTERS_EX pm; memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
    PERFORMANCE_INFORMATION pi; memset(&pi, 0, sizeof pi); pi.cb = sizeof pi;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm) && K32GetPerformanceInfo(&pi, sizeof pi))
        fprintf(f, ", process private %llu MiB, system commit %llu / %llu MiB", (unsigned long long)(pm.PrivateUsage >> 20),
                (unsigned long long)((uint64_t)pi.CommitTotal * pi.PageSize >> 20), (unsigned long long)((uint64_t)pi.CommitLimit * pi.PageSize >> 20));
}
/* Profiling line (with the every-100-submissions counters): command lists executed per call site
 * and the CPU time in them since the previous line, wall time per submission, program id cache and
 * timestamp report counts. */
static inline void boot_draw_perf_log(uint64_t events) {
    static uint64_t last_qpc, last_submits, last_wait, last_buffers, last_reports, last_batches, last_bytes;
    static uint64_t last_site[48];
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    FILE *f = fopen("draw-path.log", "a"); if (!f) return;
    double span = last_qpc ? (double)(now.QuadPart - (LONGLONG)last_qpc) / (double)freq.QuadPart : 0;
    fprintf(f, "perf: %.2f s per 100 submissions (last %llu events), command lists %llu (%.2f s), D3D12 buffers +%llu, program ids %llu hit / %llu miss, "
               "timestamp reports %llu, commit batches %llu (%llu MiB), memory batch buffers %llu\n",
            span, (unsigned long long)events, (unsigned long long)(boot_render_submit_total - last_submits),
            (double)(boot_render_submit_qpc - last_wait) / (double)freq.QuadPart, (unsigned long long)(boot_render_created_buffers - last_buffers),
            (unsigned long long)boot_render_program_hits, (unsigned long long)boot_render_program_misses,
            (unsigned long long)(boot_gpu_sync_reports - last_reports), (unsigned long long)(boot_gpu_sync_batches - last_batches),
            (unsigned long long)((boot_gpu_sync_bytes - last_bytes) >> 20), (unsigned long long)boot_gpu_batch_pool.created);
    fprintf(f, "perf: command lists by site:");
    for (unsigned i = 0; i < 48 && boot_render_submit_sites[i].file; ++i) {
        uint64_t n = boot_render_submit_sites[i].count - last_site[i]; last_site[i] = boot_render_submit_sites[i].count;
        if (!n) continue;
        const char *name = strrchr(boot_render_submit_sites[i].file, '/'); const char *b2 = strrchr(boot_render_submit_sites[i].file, '\\');
        if (!name || (b2 && b2 > name)) name = b2;
        fprintf(f, " %s:%d=%llu", name ? name + 1 : boot_render_submit_sites[i].file, boot_render_submit_sites[i].line, (unsigned long long)n);
    }
    fputc('\n', f);
    {   /* lists executed without waiting, segment waits, constant-buffer snapshot sharing */
        static uint64_t l_kicks, l_waits, l_wait_qpc, l_hits, l_misses, l_copied, l_zeroed;
        fprintf(f, "perf2: lists executed without wait %llu, segment waits %llu (%.3f s), cb snapshots %llu shared / %llu copied (%llu MiB copied, %llu MiB zeroed)\n",
                (unsigned long long)(boot_render_kicks - l_kicks), (unsigned long long)(boot_render_seg_waits - l_waits),
                (double)(boot_render_seg_wait_qpc - l_wait_qpc) / (double)freq.QuadPart,
                (unsigned long long)(boot_render_cb_hits - l_hits), (unsigned long long)(boot_render_cb_misses - l_misses),
                (unsigned long long)((boot_render_cb_copied - l_copied) >> 20), (unsigned long long)((boot_render_cb_zeroed - l_zeroed) >> 20));
        l_kicks = boot_render_kicks; l_waits = boot_render_seg_waits; l_wait_qpc = boot_render_seg_wait_qpc;
        l_hits = boot_render_cb_hits; l_misses = boot_render_cb_misses; l_copied = boot_render_cb_copied; l_zeroed = boot_render_cb_zeroed;
        static uint64_t l_wb[16][2];
        fprintf(f, "perf2: write-back batches by site (batches/targets):");
        for (unsigned i = 0; i < 16 && boot_render_wb_sites[i].file; ++i) {
            uint64_t nb = boot_render_wb_sites[i].batches - l_wb[i][0], nt = boot_render_wb_sites[i].targets - l_wb[i][1];
            l_wb[i][0] = boot_render_wb_sites[i].batches; l_wb[i][1] = boot_render_wb_sites[i].targets;
            if (!nb) continue;
            const char *name = strrchr(boot_render_wb_sites[i].file, '/'); const char *b2 = strrchr(boot_render_wb_sites[i].file, '\\');
            if (!name || (b2 && b2 > name)) name = b2;
            fprintf(f, " %s:%d=%llu/%llu", name ? name + 1 : boot_render_wb_sites[i].file, boot_render_wb_sites[i].line, (unsigned long long)nb, (unsigned long long)nt);
        }
        fprintf(f, ", evictions %llu, rt loads %llu\n", (unsigned long long)boot_render_rt_evictions, (unsigned long long)boot_render_rt_loads); }
    {   /* render target table, aliased-target syncs, render-target texture views, texture uploads, compute waits skipped (deltas) */
        static uint64_t l_ev, l_ld, l_wb, l_cr, l_sy, l_av, l_ah, l_sp, l_pw, l_tc, l_tr, l_tp, l_gl, l_gw, l_gd, l_gf, l_gg, l_fm;
        uint64_t tex_created = 0; for (unsigned d = 0; d < BOOT_RENDER_DIMS; ++d) tex_created += boot_render_tex_created[d];
        BootRenderRtTable *T = boot_render_rtt;
        fprintf(f, "perf3: rt table %u entries %llu MiB (peak %llu MiB), creates %llu, evictions %llu, loads %llu, write-backs %llu, alias syncs %llu, "
                   "spare reuses %llu; GPU write-back lists %llu (%llu write-backs, %llu loads, %llu CPU fallbacks, %llu guest-access waits); "
                   "rt texture views written %llu / reused %llu / format mismatches %llu; texture builds %llu (in place %llu, pooled %llu); compute dispatches without pre-wait %llu",
                T ? T->count : 0, (unsigned long long)(T ? T->bytes >> 20 : 0), (unsigned long long)(boot_render_rt_peak >> 20),
                (unsigned long long)(boot_render_rt_creates - l_cr), (unsigned long long)(boot_render_rt_evictions - l_ev), (unsigned long long)(boot_render_rt_loads - l_ld),
                (unsigned long long)(boot_render_rt_writebacks - l_wb), (unsigned long long)(boot_render_rt_syncs - l_sy), (unsigned long long)(boot_render_rt_spare_hits - l_sp),
                (unsigned long long)(boot_render_gt_lists - l_gl), (unsigned long long)(boot_render_gt_writebacks - l_gw), (unsigned long long)(boot_render_gt_loads - l_gd),
                (unsigned long long)(boot_render_gt_fallbacks - l_gf), (unsigned long long)(boot_render_gt_guard_waits - l_gg),
                (unsigned long long)(boot_render_alias_views - l_av), (unsigned long long)(boot_render_alias_hits - l_ah), (unsigned long long)(boot_render_alias_format_misses - l_fm),
                (unsigned long long)(tex_created - l_tc), (unsigned long long)(boot_render_tex_reused - l_tr), (unsigned long long)(boot_render_tex_pooled - l_tp),
                (unsigned long long)(boot_compute_prewait_skipped - l_pw));
        l_ev = boot_render_rt_evictions; l_ld = boot_render_rt_loads; l_wb = boot_render_rt_writebacks; l_cr = boot_render_rt_creates; l_sy = boot_render_rt_syncs;
        l_av = boot_render_alias_views; l_ah = boot_render_alias_hits; l_fm = boot_render_alias_format_misses; l_sp = boot_render_rt_spare_hits; l_pw = boot_compute_prewait_skipped;
        l_tc = tex_created; l_tr = boot_render_tex_reused; l_tp = boot_render_tex_pooled;
        l_gl = boot_render_gt_lists; l_gw = boot_render_gt_writebacks; l_gd = boot_render_gt_loads; l_gf = boot_render_gt_fallbacks; l_gg = boot_render_gt_guard_waits;
        {   static uint64_t l_ct, l_cr;   /* render-32 */
            fprintf(f, "; CPU texel writes %llu (ranges applied %llu)", (unsigned long long)(boot_render_cpu_texel_calls - l_ct),
                    (unsigned long long)(boot_render_cpu_texel_ranges - l_cr));
            l_ct = boot_render_cpu_texel_calls; l_cr = boot_render_cpu_texel_ranges; }
        {   static uint64_t l_fr, l_fn, l_dc[BOOT_RT_DECLINES];   /* wb-01: textures over dirty targets */
            fprintf(f, "; textures copied from targets %llu (newest of several %llu), over dirty targets by the guest path: shape %llu, several %llu, other image %llu, stale %llu, layout %llu",
                    (unsigned long long)(boot_render_tex_from_rt - l_fr), (unsigned long long)(boot_render_tex_from_rt_newest - l_fn),
                    (unsigned long long)(boot_render_tex_rt_declines[BOOT_RT_DECLINE_SHAPE] - l_dc[BOOT_RT_DECLINE_SHAPE]),
                    (unsigned long long)(boot_render_tex_rt_declines[BOOT_RT_DECLINE_SEVERAL] - l_dc[BOOT_RT_DECLINE_SEVERAL]),
                    (unsigned long long)(boot_render_tex_rt_declines[BOOT_RT_DECLINE_OTHER] - l_dc[BOOT_RT_DECLINE_OTHER]),
                    (unsigned long long)(boot_render_tex_rt_declines[BOOT_RT_DECLINE_STALE] - l_dc[BOOT_RT_DECLINE_STALE]),
                    (unsigned long long)(boot_render_tex_rt_declines[BOOT_RT_DECLINE_LAYOUT] - l_dc[BOOT_RT_DECLINE_LAYOUT]));
            l_fr = boot_render_tex_from_rt; l_fn = boot_render_tex_from_rt_newest;
            for (unsigned k = 0; k < BOOT_RT_DECLINES; ++k) l_dc[k] = boot_render_tex_rt_declines[k]; }
        {   static uint64_t l_sb, l_sc, l_si, l_sg, l_sd[BOOT_RT_SUBS];   /* wb-02: multi-subresource textures over dirty targets (the shape count above) */
            uint64_t sd[BOOT_RT_SUBS]; for (unsigned k = 0; k < BOOT_RT_SUBS; ++k) sd[k] = boot_render_tex_sub_declines[k] - l_sd[k];
            fprintf(f, "; textures assembled from target subresources %llu (subresources %llu, in place %llu, guest uploads %llu), shape declines: 1D/pitch/size %llu, compressed %llu, depth %llu, 3D not one slice %llu, partial mip %llu, texel size %llu, layout %llu, newest other image %llu, stale %llu, too many targets %llu",
                    (unsigned long long)(boot_render_tex_sub_built - l_sb), (unsigned long long)(boot_render_tex_sub_copies - l_sc),
                    (unsigned long long)(boot_render_tex_sub_in_place - l_si), (unsigned long long)(boot_render_tex_sub_guest - l_sg),
                    (unsigned long long)sd[BOOT_RT_SUB_SHAPE], (unsigned long long)sd[BOOT_RT_SUB_COMPRESSED], (unsigned long long)sd[BOOT_RT_SUB_DEPTH],
                    (unsigned long long)sd[BOOT_RT_SUB_3D_SLICE], (unsigned long long)sd[BOOT_RT_SUB_PARTIAL], (unsigned long long)sd[BOOT_RT_SUB_TEXEL],
                    (unsigned long long)sd[BOOT_RT_SUB_LAYOUT], (unsigned long long)sd[BOOT_RT_SUB_OTHER], (unsigned long long)sd[BOOT_RT_SUB_STALE],
                    (unsigned long long)sd[BOOT_RT_SUB_MANY]);
            l_sb = boot_render_tex_sub_built; l_sc = boot_render_tex_sub_copies; l_si = boot_render_tex_sub_in_place; l_sg = boot_render_tex_sub_guest;
            for (unsigned k = 0; k < BOOT_RT_SUBS; ++k) l_sd[k] = boot_render_tex_sub_declines[k]; }
        {   static uint64_t l_wc, l_wh, l_wr, l_wo, l_wu, l_wb, l_wd, l_wg, l_wt;   /* render-39: CPU-written texture watch */
            unsigned on = 0; for (unsigned k = 0; k < BOOT_RENDER_TEX_WATCH; ++k) on += boot_render_tex_watch[k].on != 0;
            LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
            fprintf(f, "; texture watch %u entries, checks %llu (%llu MiB hashed, %llu deferred, %llu GPU-written dropped, %.2f ms), in-place refreshes %llu (%llu rows, %llu KiB), rebuilds %llu",
                    on, (unsigned long long)(boot_render_tex_watch_checks - l_wc), (unsigned long long)((boot_render_tex_watch_hashed - l_wh) >> 20),
                    (unsigned long long)(boot_render_tex_watch_deferred - l_wd), (unsigned long long)(boot_render_tex_watch_gpu_drops - l_wg),
                    qf.QuadPart ? (double)(boot_render_tex_watch_ticks - l_wt) * 1e3 / (double)qf.QuadPart : 0.0,
                    (unsigned long long)(boot_render_tex_watch_refreshes - l_wr),
                    (unsigned long long)(boot_render_tex_watch_rows - l_wo), (unsigned long long)((boot_render_tex_watch_uploaded - l_wu) >> 10),
                    (unsigned long long)(boot_render_tex_watch_rebuilds - l_wb));
            l_wc = boot_render_tex_watch_checks; l_wh = boot_render_tex_watch_hashed; l_wr = boot_render_tex_watch_refreshes; l_wo = boot_render_tex_watch_rows;
            l_wu = boot_render_tex_watch_uploaded; l_wb = boot_render_tex_watch_rebuilds; l_wd = boot_render_tex_watch_deferred;
            l_wg = boot_render_tex_watch_gpu_drops; l_wt = boot_render_tex_watch_ticks; }
        boot_draw_perf_memory(f); fputc('\n', f); }
    fclose(f);
    last_qpc = (uint64_t)now.QuadPart; last_submits = boot_render_submit_total; last_wait = boot_render_submit_qpc; last_buffers = boot_render_created_buffers;
    last_reports = boot_gpu_sync_reports; last_batches = boot_gpu_sync_batches; last_bytes = boot_gpu_sync_bytes;
}
/* Non-finite probe (diagnostics, off by default): frames\nan-probe.request holding a render target's
 * GPU address (hex) makes the next submission that has that host target copy up to 64 texels of its
 * middle row after every event; frames\nan-probe.txt then lists every event that binds the target
 * or changes the count of texels with a NaN/infinite channel (float formats), with that count. */
#define BOOT_NAN_PROBE_SLOTS 16384u
static struct { uint64_t address; ID3D12Resource *rb; int armed; unsigned slots; uint32_t event[BOOT_NAN_PROBE_SLOTS];
                DXGI_FORMAT dxgi[BOOT_NAN_PROBE_SLOTS]; uint16_t width[BOOT_NAN_PROBE_SLOTS];
                int at_set; uint32_t at_x, at_y; } boot_nan_probe;   /* optional "<x> <y>" after the address: probe row y from x */
static inline void boot_nan_probe_arm(BootRender *r) {
    if (boot_nan_probe.armed || GetFileAttributesA("frames\\nan-probe.request") == INVALID_FILE_ATTRIBUTES) return;
    FILE *q = fopen("frames\\nan-probe.request", "r"); unsigned long long a = 0;
    boot_nan_probe.at_set = 0;
    if (q) { if (fscanf(q, "%llx", &a) != 1) a = 0; unsigned px, py; if (a && fscanf(q, "%u %u", &px, &py) == 2) { boot_nan_probe.at_set = 1; boot_nan_probe.at_x = px; boot_nan_probe.at_y = py; } fclose(q); }
    if (!a) return;
    if (!boot_nan_probe.rb) boot_nan_probe.rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, (uint64_t)BOOT_NAN_PROBE_SLOTS * 512, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!boot_nan_probe.rb) return;
    boot_nan_probe.address = a; boot_nan_probe.slots = 0; boot_nan_probe.armed = 1;
}
/* Records the target's state after event `event` (UINT32_MAX: before the first). */
static inline void boot_nan_probe_record(BootRender *r, uint32_t event) {
    if (!boot_nan_probe.armed || boot_nan_probe.slots >= BOOT_NAN_PROBE_SLOTS) return;
    BootRenderTarget *x = NULL;
    BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned t = 0; t < T->count && !x; ++t)
        if (T->rts[t].texture && T->rts[t].address == boot_nan_probe.address && !(T->rts[t].format & 0x10000u)) x = &T->rts[t];
    if (!x) return;
    uint32_t bpp = boot_render_dxgi_bytes(x->dxgi);
    if (bpp != 4 && bpp != 8) return;
    if (!boot_render_begin(r)) return;
    uint32_t w = x->width < 64 ? x->width : 64, x0 = (x->width - w) / 2, y = x->height / 2;
    if (boot_nan_probe.at_set && boot_nan_probe.at_x < x->width && boot_nan_probe.at_y < x->height) {
        x0 = boot_nan_probe.at_x; y = boot_nan_probe.at_y; if (x0 + w > x->width) w = x->width - x0; }
    unsigned k = boot_nan_probe.slots++;
    D3D12_RESOURCE_STATES was = x->state;
    boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dl = {boot_nan_probe.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    dl.PlacedFootprint.Offset = (UINT64)k * 512; dl.PlacedFootprint.Footprint.Format = x->dxgi;
    dl.PlacedFootprint.Footprint.Width = w; dl.PlacedFootprint.Footprint.Height = 1; dl.PlacedFootprint.Footprint.Depth = 1; dl.PlacedFootprint.Footprint.RowPitch = 512;
    D3D12_BOX box = {x0, y, 0, x0 + w, y + 1, 1};
    ID3D12GraphicsCommandList_CopyTextureRegion(r->cl, &dl, 0, 0, 0, &sl, &box);
    boot_render_rt_barrier(r, x, was);
    boot_nan_probe.event[k] = event; boot_nan_probe.dxgi[k] = x->dxgi; boot_nan_probe.width[k] = (uint16_t)w;
}
static inline unsigned boot_nan_probe_nonfinite(const uint8_t *p, DXGI_FORMAT f, unsigned w) {
    unsigned n = 0;
    for (unsigned i = 0; i < w; ++i) {
        uint32_t v; uint64_t q;
        switch (f) {
        case DXGI_FORMAT_R11G11B10_FLOAT: memcpy(&v, p + 4 * i, 4);
            n += ((v >> 6) & 31) == 31 || ((v >> 17) & 31) == 31 || ((v >> 27) & 31) == 31; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: memcpy(&q, p + 8 * i, 8);
            n += ((q >> 10) & 31) == 31 || ((q >> 26) & 31) == 31 || ((q >> 42) & 31) == 31 || ((q >> 58) & 31) == 31; break;
        case DXGI_FORMAT_R16G16_FLOAT: memcpy(&v, p + 4 * i, 4); n += ((v >> 10) & 31) == 31 || ((v >> 26) & 31) == 31; break;
        case DXGI_FORMAT_R32_FLOAT: memcpy(&v, p + 4 * i, 4); n += ((v >> 23) & 255) == 255; break;
        case DXGI_FORMAT_R32G32_FLOAT: memcpy(&q, p + 8 * i, 8); n += ((q >> 23) & 255) == 255 || ((q >> 55) & 255) == 255; break;
        default: break;
        }
    }
    return n;
}
/* After the submission completed: writes frames\nan-probe.txt and disarms. */
static inline void boot_nan_probe_finish(const BootGpuStage *stage) {
    if (!boot_nan_probe.armed || !boot_nan_probe.slots) return;
    boot_nan_probe.armed = 0; DeleteFileA("frames\\nan-probe.request");
    FILE *f = fopen("frames\\nan-probe.txt", "w"); uint8_t *m = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)boot_nan_probe.slots * 512};
    if (!f || FAILED(ID3D12Resource_Map(boot_nan_probe.rb, 0, &rr, (void **)&m))) { if (f) fclose(f); return; }
    fprintf(f, "target %llx: %u probes (middle row, up to 64 texels) over %u events\n", (unsigned long long)boot_nan_probe.address, boot_nan_probe.slots, stage->draw.count);
    unsigned last = UINT_MAX;
    for (unsigned k = 0; k < boot_nan_probe.slots; ++k) {
        const uint8_t *p = m + (size_t)k * 512; uint32_t ev = boot_nan_probe.event[k];
        unsigned n = boot_nan_probe_nonfinite(p, boot_nan_probe.dxgi[k], boot_nan_probe.width[k]);
        const BootDrawEvent *e = ev < stage->draw.count ? &stage->draw.events[ev] : NULL;
        int binds = 0;
        if (e && (e->kind == 1 || e->kind == 2))
            for (unsigned j = 0; j < e->state.rt_count && j < 8; ++j) binds |= e->state.rt[e->state.rt_map[j]].address == boot_nan_probe.address;
        if (n == last && !binds) continue;
        uint32_t t0, t1; memcpy(&t0, p, 4); memcpy(&t1, p + 4 * (boot_nan_probe.width[k] / 2), 4);
        fprintf(f, "after event %d kind %u: non-finite %u/%u texels (first %08x middle %08x)", ev == UINT32_MAX ? -1 : (int)ev, e ? e->kind : 0,
                n, boot_nan_probe.width[k], t0, t1);
        if (e && e->kind == 1) {
            const BootDrawState *s = &e->state;
            fprintf(f, " | binds %d topo %u count %u vs %x ps %x program %llx", binds, s->topology, s->count, s->stage_offset[1], s->stage_enabled[5] ? s->stage_offset[5] : 0,
                    (unsigned long long)s->program_base);
            for (unsigned j = 0; j < s->rt_count && j < 8; ++j) { const BootBlend *b = &s->blend[s->blend_per_target ? j : 0];
                fprintf(f, " [rt %llx fmt %x blend %u %x %x %x]", (unsigned long long)s->rt[s->rt_map[j]].address, s->rt[s->rt_map[j]].format, s->blend[j].enabled, b->color_op, b->color_src, b->color_dst); }
        } else if (e && e->kind == 2) fprintf(f, " | binds %d clear value %x rgba %08x %08x %08x %08x", binds, e->clear_value, e->clear_regs[0], e->clear_regs[1], e->clear_regs[2], e->clear_regs[3]);
        fputc('\n', f); last = n;
    }
    D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(boot_nan_probe.rb, 0, &none);
    fclose(f);
}
static inline int boot_draw_execute(const Region *regions, unsigned region_count, BootWindow *w, BootGpuChannel *channel, BootGpuStage *stage) {
    unsigned channel_index = (unsigned)(channel - w->nv_channels);
    {   /* only counter resets and reports (no draw before them): nothing for the host GPU to run */
        uint32_t k = 0; while (k < stage->draw.count && (stage->draw.events[k].kind == 5 || stage->draw.events[k].kind == 6)) ++k;
        if (k == stage->draw.count) return boot_draw_zpass_commit(stage, channel_index, 0);
    }
    if (!boot_draw_device_recover(w) || !boot_draw_open(w)) return 0;
    BootRender *r = w->render;
    if (!boot_render_occl_open(r)) { boot_draw_log("occlusion queries: %s %llu %llu", r->error, 0, 0); return 0; }
    boot_render_occl.next = boot_render_occl.lo = boot_render_occl.got = 0; boot_render_occl.want = 0;
    if (!boot_render_layout(r)) { boot_draw_log("render layout update: %s %llu %llu", r->error, 0, 0); return 0; }
    if (!boot_draw_memory(regions, region_count, w, channel->as_fd)) return 0;
    ++boot_render_exec_id;   /* guest CPU code may have changed memory since the last submission */
    boot_draw_trace_arm(r); boot_render_trace.active = 0;
    boot_nan_probe_arm(r); boot_nan_probe.slots = 0; boot_nan_probe_record(r, UINT32_MAX);
    unsigned trace_queries = 0;
    r->overlay = NULL; r->overlay_count = 0;   /* stores are applied in order instead (below) */
    static uint32_t regs[4096];
    uint32_t applied = 0, computes = 0;
    for (uint32_t i = 0; i < stage->draw.count; ++i) {
        BootDrawEvent *e = &stage->draw.events[i];
        if (i) boot_nan_probe_record(r, i - 1);
        if (!boot_draw_apply_writes(r, stage, &applied, e->write_index)) return 0;
        if (e->kind == 5 || e->kind == 6) continue;   /* occlusion counter points: boot_draw_zpass_commit */
        int rc;
        int traced = boot_draw_trace_left && boot_draw_trace_file && i >= boot_draw_trace_skip;
        if (traced) {
            --boot_draw_trace_left; ++boot_draw_trace_seq;
            boot_draw_trace_event(r, e, i, stage->draw.count);
            if (e->kind == 1) boot_draw_trace_vprobe(r, &e->state);
            if (e->kind == 1) boot_draw_trace_tileflags(r, e);
            boot_render_trace.active = e->kind == 1; boot_render_trace.drawn = 0; boot_render_trace.query = trace_queries; boot_render_trace.so_last = -1;
            boot_render_trace.tex_len = 0; boot_render_trace.tex_info[0] = 0;
        }
        if (e->kind == 4) {
            /* Compute: runs after earlier GPU work and completes before later events record
             * (host_gpu_compute.h). An untranslated program stops the submission (dumped for AOT). */
            BootQmd q; uint8_t id[32] = {0}; char variant[64] = "";
            memset(&q, 0, sizeof q);
            if (!boot_render_flush_pitch(r)) { boot_draw_log("pitch target flush failed: %s (event %llu of %llu)", r->error, i, stage->draw.count); return 0; }
            rc = boot_compute_dispatch(r, &e->compute, &q, id, variant, sizeof variant);
            char hex[17]; for (unsigned k = 0; k < 8; ++k) snprintf(hex + 2 * k, 3, "%02x", id[k]);
            if (rc) {
                boot_draw_log("compute dispatch failed: %s (event %llu of %llu)", r->error, i, stage->draw.count);
                char line[200]; snprintf(line, sizeof line, "program %s variant %s grid %ux%ux%u", hex, variant[0] ? variant : "-", q.grid[0], q.grid[1], q.grid[2]);
                boot_draw_log("  compute %s qmd %llx region %llx", line, e->compute.qmd, e->compute.program_region);
                return 0;
            }
            if (boot_compute.dispatches <= 64 || boot_compute.dispatches % 500 == 0) {
                char line[200]; snprintf(line, sizeof line, "program %s variant %s grid %ux%ux%u", hex, variant, q.grid[0], q.grid[1], q.grid[2]);
                boot_draw_log("executed compute dispatch: %s (dispatch %llu, event %llu)", line, boot_compute.dispatches, i);
            }
            if (traced) fprintf(boot_draw_trace_file, "  compute program %s variant %s grid %ux%ux%u result ok\n", hex, variant, q.grid[0], q.grid[1], q.grid[2]);
            ++computes;
            continue;
        }
        if (e->kind == 3) {
            /* the copy reads guest memory: finish GPU work that may still write it first */
            if (r->recording && !boot_render_submit(r)) { boot_draw_log("GPU submit failed%s %llu %llu", "", 0, 0); return 0; }
            if (!boot_render_flush_pitch(r)) { boot_draw_log("pitch target flush failed: %s (event %llu of %llu)", r->error, i, stage->draw.count); return 0; }
            if (!boot_draw_copy_execute(r, &e->copy)) return 0;
            continue;
        }
        if (e->kind == 1) {
            BootRenderDraw d; memset(d.host_state, 0, sizeof d.host_state);   /* state and cbufs are set below */
            d.state = &e->state; memcpy(d.cbufs, e->cbufs, sizeof d.cbufs);
            uint32_t one = 0x3f800000u; memcpy(&d.host_state[0], &one, 4);   /* y direction +1 */
            d.host_state[5] = e->instance;
            d.guest_barrier = e->clear_regs[2] != 0;
            boot_render_occl.want = (int)(e->clear_regs[0] & 1); boot_render_occl.got = 0;
            rc = boot_render_draw(r, &d);
            e->clear_regs[1] = boot_render_occl.got; boot_render_occl.want = 0;
        } else {
            /* boot_render_clear reads only the registers set below; the rest stay zero */
            static int regs_zeroed; if (!regs_zeroed) { memset(regs, 0, sizeof regs); regs_zeroed = 1; }
            for (unsigned k = 0; k < 4; ++k) regs[(0xd80 + 4 * k) / 4] = e->clear_regs[k];
            regs[0xd90 / 4] = e->clear_regs[4]; regs[0xda0 / 4] = e->clear_regs[5]; regs[0x10f8 / 4] = e->clear_regs[6];
            rc = boot_render_clear(r, &e->state, regs, e->clear_value);
        }
        if (traced) {
            BootRenderTrace *tr = &boot_render_trace; FILE *f = boot_draw_trace_file;
            fprintf(f, "  result %d %s\n", rc, rc ? r->error : "");
            if (tr->tex_len) fprintf(f, "  tex%s\n", tr->tex_info);   /* boot_render_draw_trace_tex */
            if (tr->active && tr->drawn) {
                if (boot_render_trace.so_last >= 0) boot_draw_trace_so_seq[boot_render_trace.so_last] = boot_draw_trace_seq;
                boot_draw_trace_ids[trace_queries++] = boot_draw_trace_seq;
                fprintf(f, "  vs "); boot_draw_trace_hex(f, tr->vs, 32); fprintf(f, " ps "); boot_draw_trace_hex(f, tr->ps, 32);
                fprintf(f, "\n  host draw count %u indexed %u topo %u vp %g,%g %gx%g depth %g..%g scissor %ld,%ld..%ld,%ld zt rt#%u rts",
                        tr->draw_count, tr->indexed, tr->topo, tr->vp.TopLeftX, tr->vp.TopLeftY, tr->vp.Width, tr->vp.Height, tr->vp.MinDepth, tr->vp.MaxDepth,
                        tr->sc.left, tr->sc.top, tr->sc.right, tr->sc.bottom, tr->zt_index);
                for (unsigned k = 0; k < e->state.rt_count && k < 8; ++k) fprintf(f, " #%u(mask %x blend %u tex %ux%u dxgi %u pso %u state %x)", tr->rt_index[k], tr->write_mask[k], tr->blend_enable[k],
                                                                                   tr->rt_w[k], tr->rt_h[k], tr->rt_fmt[k], tr->pso_rtfmt[k], tr->rt_state[k]);
                fprintf(f, " | pso samples %u cull %u frontccw %u dsv %u", tr->pso_samples, tr->pso_cull, tr->pso_front, tr->pso_dsvfmt);
                fputc('\n', f);
            }
            tr->active = 0;
            if (!boot_draw_trace_left) { boot_draw_log("draw trace complete%s %llu %llu", "", boot_draw_trace_seq, 0); }
        }
        if (rc == BOOT_RENDER_E_SHADER) {
            /* Program not translated yet: it was dumped to ../missing-shaders for AOT translation
             * (translate_runtime_shaders.py). Skip this draw; the rest of the frame still renders. */
            static uint64_t skipped; ++skipped;
            {   /* every distinct program is logged once (with what the draw targeted), then every 50th skip */
                static char seen[256][96]; static unsigned nseen; int fresh = 1;
                for (unsigned k = 0; k < nseen && fresh; ++k) fresh = strncmp(seen[k], r->error, sizeof seen[k] - 1) != 0;
                if (fresh && nseen < 256) { snprintf(seen[nseen++], sizeof seen[0], "%s", r->error); }
                if (fresh || skipped % 50 == 1) {
                    const BootDrawState *s = &e->state;
                    char where[160]; int n = snprintf(where, sizeof where, " rt");
                    for (unsigned k = 0; k < s->rt_count && k < 8 && n < (int)sizeof where - 24; ++k)
                        n += snprintf(where + n, sizeof where - n, " %llx/%x", (unsigned long long)s->rt[s->rt_map[k]].address, s->rt[s->rt_map[k]].write_mask);
                    if (n < (int)sizeof where - 24) snprintf(where + n, sizeof where - n, " zt %llx", s->zt.enabled ? (unsigned long long)s->zt.address : 0ull);
                    boot_draw_log("skipped draw, untranslated shader: %s (skipped %llu, event %llu)", r->error, skipped, i);
                    char line[220]; snprintf(line, sizeof line, "%s%s", fresh ? "(first of this program)" : "", where);
                    boot_draw_log("  skipped draw %s: count %llu inst %llu", line, e->state.count, e->instance);
                }
            }
            continue;
        }
        if (rc) {
            boot_draw_log("event failed: %s (event %llu of %llu)", r->error, i, stage->draw.count);
            r->overlay = NULL; r->overlay_count = 0;
            return 0;
        }
    }
    r->overlay = NULL; r->overlay_count = 0;
    if (stage->draw.count) boot_nan_probe_record(r, stage->draw.count - 1);
    if (!boot_render_submit(r)) { boot_draw_log("GPU submit failed%s %llu %llu", "", 0, 0); return 0; }
    boot_render_pso_release_retired();   /* queue idle: pipelines of a recycled PSO cache can go */
    {   /* queue idle, no list open: bound the textures the TIC slots hold (TOTK_TEX_BUDGET_MB overrides) */
        static uint64_t budget;
        if (!budget) {
            char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_BUDGET_MB", v, sizeof v);
            unsigned long long mb = (n && n < sizeof v) ? strtoull(v, NULL, 10) : 0;
            budget = mb >= 256 ? (uint64_t)mb << 20 : BOOT_RENDER_TEX_BUDGET;
        }
        boot_render_tex_trim(r, budget);
        /* textures the guest CPU rewrote since they were built (font glyph cache, streamed images):
         * re-hash round-robin within TOTK_TEX_VERIFY_MB per submission (default 64, 0 = off) */
        static uint64_t verify = UINT64_MAX;
        if (verify == UINT64_MAX) {
            char v[32]; DWORD n = GetEnvironmentVariableA("TOTK_TEX_VERIFY_MB", v, sizeof v);
            verify = (n && n < sizeof v) ? (uint64_t)strtoull(v, NULL, 10) << 20 : UINT64_C(64) << 20;
        }
        unsigned stale = boot_render_tex_verify(r, verify);
        static uint64_t logged; if (stale && (++logged <= 32 || logged % 500 == 0))
            boot_draw_log("textures rewritten by the guest since their build%s: %llu (total %llu)", "", stale, boot_render_tex_stale_rebuilds);
    }
    boot_nan_probe_finish(stage);
    /* pitch-linear render targets live in guest memory on the source GPU */
    if (!boot_render_flush_pitch(r)) { boot_draw_log("pitch target flush failed: %s (end of submission) %llu %llu", r->error, 0, 0); return 0; }
    if (computes || boot_render_pitch_flushes) {
        /* The submit path commits every staged store again after this function (idempotent for
         * draws). Stores already applied in order precede a compute dispatch, which may have
         * overwritten them: re-applying the old values would undo GPU results, so they take the
         * current memory contents (the in-order result) instead. Later stores are unaffected. */
        for (uint32_t k = 0; k < applied; ++k) {
            BootGpuMemoryWrite *w = &boot_gpu_stage_writes(stage)[k];
            if (w->kind != 0) continue;
            const uint8_t *p = boot_render_guest(r, w->gpu, 4);
            if (p) memcpy(&w->value, p, 4);
        }
    }
    /* every query slot was resolved when its command list closed (the submit above at the latest) */
    if (!boot_draw_zpass_commit(stage, channel_index, boot_render_occl.next)) return 0;
    if (boot_draw_trace_file) {
        boot_draw_trace_flush(r, trace_queries);
        if (!boot_draw_trace_left) { fclose(boot_draw_trace_file); boot_draw_trace_file = NULL; }
    }
    {   static uint64_t total, submissions; total += stage->draw.count;
        if (++submissions % 100 == 1) {   /* every 100th submission: logging is a file write */
            boot_draw_log("executed draw/clear events%s: %llu (total %llu)", "", stage->draw.count, total);
            boot_draw_log("render targets sampled as textures%s: %llu, host render targets %llu", "", boot_render_rt_alias_count, boot_render_rtab(r)->count);
            boot_draw_log("D3D12 buffers created%s: %llu, guest regions opened %llu", "", boot_render_created_buffers, boot_render_created_regions);
            boot_draw_log("render target write-backs%s: %llu, evictions %llu", "", boot_render_rt_writebacks, boot_render_rt_evictions);
            boot_draw_perf_gpu = r->gpu; boot_draw_perf_log(stage->draw.count);
        } }
    return 1;
}

/* QUEUE_BUFFER: present the host render target written for the buffer's memory, or the
 * guest buffer itself. `slot` is a registered graphic buffer (BootGraphicSlot). */
/* Diagnostics: when frames\dump-rts.request exists, read back every host render target into
 * frames\rt-<index>-<gpu address>.bin (32-byte header: address, width, height, dxgi format,
 * row pitch, guest format) and delete the request. */
static inline void boot_draw_dump_targets(BootRender *r, uint64_t presented) {
    if (GetFileAttributesA("frames\\dump-rts.request") == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA("frames\\dump-rts.request");
    FILE *index = fopen("frames\\rt-index.txt", "w");
    BootRenderRtTable *T = boot_render_rtab(r);
    for (unsigned t = 0; t < T->count; ++t) {
        BootRenderTarget *x = &T->rts[t];
        if (!x->texture) continue;
        D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
        ID3D12Device_GetCopyableFootprints(r->gpu->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
        ID3D12Resource *rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        if (!rb || !boot_render_begin(r)) { if (rb) ID3D12Resource_Release(rb); continue; }
        D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = x->texture;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = x->state; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        int moved = x->state != D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (moved) ID3D12GraphicsCommandList_ResourceBarrier(r->cl, 1, &b);
        D3D12_TEXTURE_COPY_LOCATION dl = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        dl.PlacedFootprint = fp;
        ID3D12GraphicsCommandList_CopyTextureRegion(r->cl, &dl, 0, 0, 0, &sl, NULL);
        if (moved) { b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = x->state; ID3D12GraphicsCommandList_ResourceBarrier(r->cl, 1, &b); }
        uint8_t *mapped = NULL;
        if (boot_render_submit(r) && SUCCEEDED(ID3D12Resource_Map(rb, 0, NULL, (void **)&mapped))) {
            char path[160]; snprintf(path, sizeof path, "frames\\rt-%02u-%llx.bin", t, (unsigned long long)x->address);
            FILE *o = fopen(path, "wb");
            if (o) { uint64_t hdr[4] = {x->address, (uint64_t)x->width | ((uint64_t)x->height << 32), (uint64_t)x->dxgi | ((uint64_t)fp.Footprint.RowPitch << 32), x->format};
                fwrite(hdr, 8, 4, o); fwrite(mapped + fp.Offset, 1, (size_t)fp.Footprint.RowPitch * fp.Footprint.Height, o); fclose(o); }
            ID3D12Resource_Unmap(rb, 0, NULL);
            if (index) fprintf(index, "%u address %llx %ux%u dxgi %u guest %x%s\n", t, (unsigned long long)x->address, x->width, x->height,
                               (unsigned)x->dxgi, x->format, x->address == presented ? " PRESENTED" : "");
        }
        ID3D12Resource_Release(rb);
    }
    if (index) fclose(index);
}
/* render-23 diagnostic (TOTK_FLICKER_LOG=1): every presented image is copied (no wait) into one of 8 readback
 * buffers; a later present reads the completed ones in order and computes the mean luma of the image (every 4th
 * pixel of every 4th row, 8-bit RGBA). A present whose mean drops by more than 30% from the previous one, and the
 * one after it, go to frames\flicker.log with the per-present deltas of the target counters (reloads from guest
 * memory, overlap syncs, render-22 kept images, assembly refreshes / builds, target creations / evictions, alias
 * views written) and every refresh recorded in that present's submissions (address, size, format, stale/dirty,
 * dirty overlapping targets, kept). Also a line every 600 presents with the min/max mean seen, so a log without
 * drops still shows the detector ran. Costs one 8 MiB copy per present. */
/* render-27: textures assembled as new ones in submissions [lo, hi] */
/* render-31: a presented image (8-bit RGBA readback) as frames\flicker-<present>.bmp, half size, 24-bit */
static inline void boot_draw_flicker_bmp(ID3D12Resource *rb, UINT pitch, uint32_t w, uint32_t h, uint64_t present, const char *tag) {
    static unsigned written; if (written >= 64) return;   /* at most 64 images per run */
    uint8_t *m = NULL; if (!rb || FAILED(ID3D12Resource_Map(rb, 0, NULL, (void **)&m))) return;
    uint32_t ow = w / 2, oh = h / 2, row = (ow * 3 + 3) & ~3u;
    char path[96]; snprintf(path, sizeof path, "frames\\flicker-%05llu-%s.bmp", (unsigned long long)present, tag);
    FILE *o = fopen(path, "wb");
    if (o) {
        uint32_t size = 54 + row * oh; uint8_t hd[54] = {'B', 'M'};
        memcpy(hd + 2, &size, 4); hd[10] = 54; hd[14] = 40; memcpy(hd + 18, &ow, 4); memcpy(hd + 22, &oh, 4); hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, o);
        uint8_t *line = calloc(1, row);
        for (uint32_t y = oh; line && y-- > 0; ) {   /* bottom-up */
            const uint8_t *src = m + (size_t)(y * 2) * pitch;
            for (uint32_t x = 0; x < ow; ++x) { line[x * 3] = src[x * 8 + 2]; line[x * 3 + 1] = src[x * 8 + 1]; line[x * 3 + 2] = src[x * 8]; }
            fwrite(line, 1, row, o);
        }
        free(line); fclose(o); ++written;
    }
    D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(rb, 0, &none);
}
static inline void boot_draw_flicker_builds(FILE *f, uint64_t lo, uint64_t hi) {
    uint64_t first = boot_render_build_n > 256 ? boot_render_build_n - 256 : 0;
    for (uint64_t q = first; q < boot_render_build_n; ++q) {
        unsigned j = (unsigned)(q & 255);
        if (boot_render_build_ring[j].exec < lo || boot_render_build_ring[j].exec > hi) continue;
        fprintf(f, "  built (submission %llu) TIC %u %llx %ux%u layers %u levels %u: %s; write seq %llu; from",
                (unsigned long long)boot_render_build_ring[j].exec, boot_render_build_ring[j].index, (unsigned long long)boot_render_build_ring[j].address,
                boot_render_build_ring[j].width, boot_render_build_ring[j].height, boot_render_build_ring[j].layers, boot_render_build_ring[j].levels,
                boot_render_build_why[boot_render_build_ring[j].why < 9 ? boot_render_build_ring[j].why : 0], (unsigned long long)boot_render_build_ring[j].seq);
        for (unsigned s = 0; s < 4 && s < boot_render_build_ring[j].n; ++s)
            fprintf(f, " [%llx %ux%u seq %llu]", (unsigned long long)boot_render_build_ring[j].src[s], boot_render_build_ring[j].srcw[s],
                    boot_render_build_ring[j].srch[s], (unsigned long long)boot_render_build_ring[j].srcseq[s]);
        fputc('\n', f);
    }
}
/* flashfix: native NVN executor counters for the flicker log, cumulative (host_nvn_play.h boot_nvn_flicker_counts sets them
 * before each native present; the emulated path leaves them 0). Per present, the DROP / next / sample lines end with
 * "| nvn ..." (draws, clears, skipped and failed draws, draws/clears the game disabled, rasterizer-discarded draws,
 * deferred texture binds and how many came from the submit-call copy / were not in it, and late-changed = handles that
 * guest memory no longer held at translation, of those compared, with TOTK_DEFER_LATE_CHECK=1), and a DROP line also with
 * the present before it ("before: ..."), so a flash frame compares with its neighbours. */
typedef struct BootFlickerNvn { uint64_t draws, clears, skipped, failed, render_off, discarded, deferred, snap_hits, snap_misses, late_checked, late_changed; } BootFlickerNvn;
static BootFlickerNvn boot_draw_flicker_nvn; static int boot_draw_flicker_late_on;
static inline BootFlickerNvn boot_draw_flicker_nvn_sub(const BootFlickerNvn *a, const BootFlickerNvn *b) {
    BootFlickerNvn d;
    d.draws = a->draws - b->draws; d.clears = a->clears - b->clears; d.skipped = a->skipped - b->skipped; d.failed = a->failed - b->failed;
    d.render_off = a->render_off - b->render_off; d.discarded = a->discarded - b->discarded; d.deferred = a->deferred - b->deferred;
    d.snap_hits = a->snap_hits - b->snap_hits; d.snap_misses = a->snap_misses - b->snap_misses;
    d.late_checked = a->late_checked - b->late_checked; d.late_changed = a->late_changed - b->late_changed;
    return d;
}
static inline void boot_draw_flicker_nvn_text(char *s, size_t n, const BootFlickerNvn *v) {
    char late[48];
    if (boot_draw_flicker_late_on) snprintf(late, sizeof late, "%llu/%llu", (unsigned long long)v->late_changed, (unsigned long long)v->late_checked);
    else snprintf(late, sizeof late, "off");
    snprintf(s, n, "nvn draws %llu clears %llu skipped %llu failed %llu render-off %llu discarded %llu deferred-binds %llu (copy %llu, not in copy %llu) late-changed %s",
             (unsigned long long)v->draws, (unsigned long long)v->clears, (unsigned long long)v->skipped, (unsigned long long)v->failed,
             (unsigned long long)v->render_off, (unsigned long long)v->discarded, (unsigned long long)v->deferred, (unsigned long long)v->snap_hits,
             (unsigned long long)v->snap_misses, late);
}
static inline void boot_draw_flicker_watch(BootRender *r, BootRenderTarget *x) {
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_FLICKER_LOG", v, sizeof v); on = k && k < sizeof v && v[0] == '1'; }
    if (!on || !x || !x->texture) return;
    enum { N = 8 };
    typedef struct { uint64_t loads, syncs, kept, refreshes, built, in_place, creates, evictions, views; BootFlickerNvn nvn; } Cnt;
    static struct { ID3D12Resource *rb; uint64_t bytes, fence, frame, exec_lo, exec_hi; UINT pitch; uint32_t w, h, dxgi; Cnt d; int busy; } ring[N];
    static uint64_t frame, harvest, last_exec; static Cnt last; static double prev = -1, lo = 1e9, hi = -1; static int after_drop;
    BootGpu *g = r->gpu;
    /* completed copies, in present order */
    while (harvest < frame) {
        unsigned k = (unsigned)(harvest % N);
        if (!ring[k].busy) { ++harvest; continue; }
        if (ID3D12Fence_GetCompletedValue(g->fence) < ring[k].fence) break;
        uint8_t *m = NULL; double sum = 0; uint64_t cnt = 0;
        if (SUCCEEDED(ID3D12Resource_Map(ring[k].rb, 0, NULL, (void **)&m))) {
            for (uint32_t yy = 0; yy < ring[k].h; yy += 4)
                for (uint32_t xx = 0; xx < ring[k].w; xx += 4) {
                    const uint8_t *px = m + (size_t)yy * ring[k].pitch + (size_t)xx * 4;
                    sum += 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]; ++cnt;
                }
            D3D12_RANGE none = {0, 0}; ID3D12Resource_Unmap(ring[k].rb, 0, &none);
        }
        ring[k].busy = 0;
        double mean = cnt ? sum / (double)cnt : 0;
        if (mean < lo) lo = mean;
        if (mean > hi) hi = mean;
        int drop = prev > 4.0 && mean < prev * 0.7;
        if (drop && BOOT_CENSUS_ON()) BOOT_CENSUS_FLICKER_DROP(ring[k].frame);
        if (drop) {   /* render-31: the dark present, the one before it, and (next harvest) the one after, as images */
            unsigned pk = (unsigned)((harvest + N - 1) % N);
            if (ring[pk].rb && ring[pk].frame + 1 == ring[k].frame)
                boot_draw_flicker_bmp(ring[pk].rb, ring[pk].pitch, ring[pk].w, ring[pk].h, ring[k].frame, "a-before");
            boot_draw_flicker_bmp(ring[k].rb, ring[k].pitch, ring[k].w, ring[k].h, ring[k].frame, "b-drop");
        } else if (after_drop) boot_draw_flicker_bmp(ring[k].rb, ring[k].pitch, ring[k].w, ring[k].h, ring[k].frame - 1, "c-after");
        if (drop || after_drop) {
            FILE *f = fopen("frames\\flicker.log", "a");
            if (f) {
                const Cnt *c = &ring[k].d;
                char nv[512], before[544] = "";   /* flashfix: the executor's counters of this present (and of the one before a drop) */
                boot_draw_flicker_nvn_text(nv, sizeof nv, &c->nvn);
                {   unsigned pk = (unsigned)((harvest + N - 1) % N);
                    if (drop && ring[pk].rb && ring[pk].frame + 1 == ring[k].frame) { char pv[512]; boot_draw_flicker_nvn_text(pv, sizeof pv, &ring[pk].d.nvn); snprintf(before, sizeof before, " | before: %s", pv); } }
                fprintf(f, "%s present %llu (submissions %llu..%llu): mean luma %.1f (previous %.1f) | reloads %llu syncs %llu kept %llu asm-refresh %llu asm-built %llu asm-in-place %llu creates %llu evictions %llu alias-views %llu | %s%s\n",
                        drop ? "DROP" : "next", (unsigned long long)ring[k].frame, (unsigned long long)ring[k].exec_lo, (unsigned long long)ring[k].exec_hi, mean, prev,
                        (unsigned long long)c->loads, (unsigned long long)c->syncs, (unsigned long long)c->kept, (unsigned long long)c->refreshes,
                        (unsigned long long)c->built, (unsigned long long)c->in_place, (unsigned long long)c->creates, (unsigned long long)c->evictions, (unsigned long long)c->views,
                        nv, before);
                uint64_t first = boot_render_reload_n > 256 ? boot_render_reload_n - 256 : 0;
                for (uint64_t q = first; q < boot_render_reload_n; ++q) {
                    unsigned j = (unsigned)(q & 255);
                    if (boot_render_reload_ring[j].exec < ring[k].exec_lo || boot_render_reload_ring[j].exec > ring[k].exec_hi) continue;
                    fprintf(f, "  refresh (submission %llu) %llx %ux%u fmt %x stale %u dirty %u overlaps %u%s\n", (unsigned long long)boot_render_reload_ring[j].exec,
                            (unsigned long long)boot_render_reload_ring[j].address, boot_render_reload_ring[j].width, boot_render_reload_ring[j].height,
                            boot_render_reload_ring[j].format, boot_render_reload_ring[j].stale, boot_render_reload_ring[j].dirty, boot_render_reload_ring[j].overlaps,
                            boot_render_reload_ring[j].kept ? " (kept image)" : " (reloaded)");
                }
                boot_draw_flicker_builds(f, ring[k].exec_lo, ring[k].exec_hi);   /* render-27 */
                fclose(f);
            }
        }
        after_drop = drop;
        if (ring[k].frame % 600 == 599) {
            FILE *f = fopen("frames\\flicker.log", "a");
            if (f) { fprintf(f, "present %llu: mean luma min %.1f max %.1f over the last 600\n", (unsigned long long)ring[k].frame, lo, hi); fclose(f); }
            lo = 1e9; hi = -1;
        }
        if (!drop && ring[k].frame % 60 == 30) {   /* render-27: a normal present's builds, for comparison */
            FILE *f = fopen("frames\\flicker.log", "a");
            if (f) { char nv[512]; boot_draw_flicker_nvn_text(nv, sizeof nv, &ring[k].d.nvn);   /* flashfix */
                     fprintf(f, "sample present %llu (submissions %llu..%llu): mean luma %.1f | %s\n", (unsigned long long)ring[k].frame,
                             (unsigned long long)ring[k].exec_lo, (unsigned long long)ring[k].exec_hi, mean, nv);
                     boot_draw_flicker_builds(f, ring[k].exec_lo, ring[k].exec_hi); fclose(f); }
        }
        prev = mean; ++harvest;
    }
    /* this present: copy into the next free slot (skipped when all slots are still in flight) */
    unsigned k = (unsigned)(frame % N);
    Cnt now = {boot_render_rt_loads, boot_render_rt_syncs, boot_render_rt_kept_image, boot_render_asm_refreshes, boot_render_tex_mips_from_rt,
               boot_render_tex_mips_in_place, boot_render_rt_creates, boot_render_rt_evictions, boot_render_alias_views, boot_draw_flicker_nvn};
    if (ring[k].busy || (x->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM && x->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) { last = now; last_exec = boot_render_exec_id; return; }
    D3D12_RESOURCE_DESC sd; x->texture->lpVtbl->GetDesc(x->texture, &sd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rs, total;
    ID3D12Device_GetCopyableFootprints(g->device, &sd, 0, 1, 0, &fp, &rows, &rs, &total);
    if (!ring[k].rb || ring[k].bytes < total) {
        if (ring[k].rb) ID3D12Resource_Release(ring[k].rb);   /* not busy: its copy completed */
        ring[k].rb = boot_render_buffer(r, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, NULL); ring[k].bytes = ring[k].rb ? total : 0;
        if (!ring[k].rb) return;
    }
    if (!boot_render_begin(r)) return;
    D3D12_RESOURCE_STATES was = x->state;
    if (!boot_render_rt_barrier(r, x, D3D12_RESOURCE_STATE_COPY_SOURCE)) return;
    D3D12_TEXTURE_COPY_LOCATION dl = {ring[k].rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, sl = {x->texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    dl.PlacedFootprint = fp; dl.PlacedFootprint.Offset = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(r->cl, &dl, 0, 0, 0, &sl, NULL);
    boot_render_rt_barrier(r, x, was);
    ring[k].busy = 1; ring[k].fence = g->submitted + 1;   /* the next signal executes the list holding this copy */
    ring[k].frame = frame++; ring[k].pitch = fp.Footprint.RowPitch; ring[k].w = x->width; ring[k].h = x->height; ring[k].dxgi = x->dxgi;
    ring[k].exec_lo = last_exec + 1; ring[k].exec_hi = boot_render_exec_id;
    ring[k].d.loads = now.loads - last.loads; ring[k].d.syncs = now.syncs - last.syncs; ring[k].d.kept = now.kept - last.kept;
    ring[k].d.refreshes = now.refreshes - last.refreshes; ring[k].d.built = now.built - last.built; ring[k].d.in_place = now.in_place - last.in_place;
    ring[k].d.creates = now.creates - last.creates; ring[k].d.evictions = now.evictions - last.evictions; ring[k].d.views = now.views - last.views;
    ring[k].d.nvn = boot_draw_flicker_nvn_sub(&now.nvn, &last.nvn);   /* flashfix */
    last = now; last_exec = boot_render_exec_id;
}
static inline int boot_draw_present(BootWindow *w, const BootGraphicSlot *slot) {
    if (!boot_draw_device_recover(w) || !boot_draw_open(w)) return 0;
    BootRender *r = w->render;
    if (BOOT_CENSUS_ON()) BOOT_CENSUS_PRESENT();
    if (!w->present) { w->present = calloc(1, sizeof(BootPresent)); if (!w->present) return 0;
        { char e[16]; DWORD k = GetEnvironmentVariableA("TOTK_FRAME_DUMP_EVERY", e, sizeof e);   /* periodic framesrame-N.bmp: default every 30 presents; 0 = off (fast mode) */
          w->present->dump_every = (k && k < sizeof e) ? (uint32_t)strtoul(e, NULL, 10) : 30u; }
        snprintf(w->present->dump_dir, sizeof w->present->dump_dir, "frames");
        CreateDirectoryA("frames", NULL); }
    /* Debug view (TOTK_PRESENT_ADDR=<hex GPU address>): present the newest render target at that address instead
     * of the game's display buffer, e.g. the G-buffer colour target, to watch the scene while the final image path
     * is being fixed. Unset = normal behaviour. */
    { static uint64_t debug_addr = 1;
      if (debug_addr == 1) { const char *e = getenv("TOTK_PRESENT_ADDR"); debug_addr = e ? strtoull(e, NULL, 16) : 0; }
      if (debug_addr) {
          BootRenderRtTable *T = boot_render_rtab(r); BootRenderTarget *x = NULL; uint64_t newest = 0;
          for (unsigned t = 0; t < T->count; ++t)
              if (T->rts[t].texture && T->rts[t].address == debug_addr && !(T->rts[t].format & 0x10000u) && (!x || T->info[t].last_use >= newest)) {
                  x = &T->rts[t]; newest = T->info[t].last_use; }
          if (x) { int rc = boot_render_present_target(r, w->present, w->hwnd, x);
                   if (rc) boot_draw_log("present (debug %s): %llu %llu", r->error, (unsigned long long)debug_addr, 0);
                   else return 1; }
      } }
    /* GPU address(es) of this buffer's NVMap object -> a host render target at that address. */
    for (unsigned i = 0; i < BOOT_GPU_MAPPING_CAPACITY; ++i) {
        const BootGpuMapping *m = &w->gpu_mappings.entries[i];
        if (!m->size || m->handle != slot->handle) continue;
        BootRenderRtTable *T = boot_render_rtab(r); BootRenderTarget *x = NULL; uint64_t newest = 0;
        for (unsigned t = 0; t < T->count; ++t)   /* the most recently drawn target at the address */
            if (T->rts[t].texture && T->rts[t].address == m->gpu && !(T->rts[t].format & 0x10000u) && (!x || T->info[t].last_use >= newest)) {
                x = &T->rts[t]; newest = T->info[t].last_use; }
        if (x) {
            boot_draw_dump_targets(r, x->address);
            boot_draw_flicker_watch(r, x);   /* render-23 */
            int rc = boot_render_present_target(r, w->present, w->hwnd, x);
            if (rc) boot_draw_log("present: %s %llu %llu", r->error, m->gpu, 0);
            return rc == 0;
        }
    }
    boot_draw_log("present: no host render target for buffer%s handle %llu cpu %llx", "", slot->handle, slot->cpu);
    return 1;   /* nothing rendered by the 3D engine yet: keep the queue moving */
}
#endif
