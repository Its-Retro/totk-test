#ifndef TOTK_HOST_GPU_COMPUTE_H
#define TOTK_HOST_GPU_COMPUTE_H
/* Maxwell compute (NVB1C0) dispatch on D3D12.
 *
 * A launch (host_gpu_command_stage.h, SEND_SIGNALING_PCAS_B with SCHEDULE) records the QMD address
 * and the compute registers; this file executes it in submission order:
 *   - the QMD (NVA0C0_QMDV01_07 layout, gpu-reference/cla0c0qmd.h) is read from guest memory after
 *     the staged writes preceding the launch (QMDs are uploaded inline);
 *   - the program (no SPH for compute: code at PROGRAM_REGION + QMD.PROGRAM_OFFSET through the
 *     self-branch terminator) is identified by sha256 of its code and looked up in
 *     shader-manifest.tsv as <id> cs <variant> <tex handles> <dxil>, the variant being the block
 *     size, shared memory size and local memory size the HLSL was compiled for
 *     (translate_runtime_shaders.py). A miss dumps the program to
 *     missing-shaders/cs-<variant>-<id>.bin (+ .qmd) and stops the submission: nothing is skipped;
 *   - the eight QMD constant buffers become b0..b7 (snapshots, like draws), HostState b18 carries
 *     the page table extent and the group offset of split grids; guest memory is the same
 *     zero-copy region table draws use (read and written through the space7 UAVs);
 *   - the dispatch is recorded after all earlier GPU work completed (so snapshots see it) and
 *     completes before anything later is recorded (so later snapshots and the CPU see its writes);
 *     pages the shader stored to are reported through a CPU-visible bitmap (u1, space0) and
 *     cached textures overlapping them are invalidated;
 *   - QMD semaphore releases (RELEASE0/1) are performed after completion.
 * Global state lives in this module, not in BootRender, so a live renderer keeps its layout. */
#include "host_gpu_render.h"
#include "host_gpu_stage_draw.h"
#include "guest_clock.h"

#define BOOT_COMPUTE_MAX_SHADERS 2048   /* module-static; a full table made later programs permanent manifest misses */

typedef struct BootQmd {
    uint32_t words[64];
    uint32_t program_offset, grid[3], block[3], shared_size, cb_mask, local_low, local_high, local_crs;
    uint32_t sampler_via_header, barrier_count, register_count;
    uint64_t cb_address[8]; uint32_t cb_size[8];
    uint32_t cb_direct;            /* slots read by the GPU from guest memory (native NVN path), not snapshotted */
    uint32_t release_enable[2], release_op[2], release_format[2], release_reduce[2], release_one_word[2], release_payload[2];
    uint64_t release_address[2];
} BootQmd;

typedef struct BootComputeShader {
    uint8_t id[32]; char variant[64]; void *dxil; size_t size; uint32_t tex_count, tex[64], writes;
    ID3D12PipelineState *pso;
} BootComputeShader;

static uint64_t boot_compute_hitch_load_qpc, boot_compute_hitch_pso, boot_compute_hitch_pso_qpc;   /* play (45) */
static uint64_t boot_compute_prewait_skipped;   /* dispatches that did not wait for earlier work (no guest-memory stores pending) */
static struct {
    ID3D12RootSignature *root;
    BootComputeShader shaders[BOOT_COMPUTE_MAX_SHADERS]; unsigned shader_count;
    ID3D12Resource *dirty; uint64_t *dirty_cpu; uint32_t dirty_words;
    uint8_t *program; size_t program_size;
    uint64_t dispatches, groups;
} boot_compute;

/* ---------------------------------------------------------------- compute pipelines built ahead of use (play (74))
 * A compute pipeline depends on nothing but its program (the root signature is fixed), so every program of the
 * manifest can be compiled before the game first dispatches it. Compiled on the first dispatch, one program took
 * 1.1 s in the play-71 run (a stutter in the prologue's gameplay) and twelve took 358 ms at the first area load.
 * A background thread (below normal priority) builds them, the programs an earlier boot dispatched
 * (compute-used.txt) first. The dispatch takes a finished pipeline, waits for the one the thread is building (the
 * rest of that compile is shorter than compiling it again), or, when the thread has not reached the program,
 * claims it and compiles itself (the thread then skips it). Same bytes and root signature as the on-demand
 * compile: nothing is skipped or delayed, the pipeline is the one the dispatch would have created.
 * TOTK_COMPUTE_PRE=0 turns the thread off. */
#define BOOT_COMPUTE_PRE_MAX 1024
enum { BOOT_CPRE_IDLE = 0, BOOT_CPRE_BUILDING = 1, BOOT_CPRE_READY = 2, BOOT_CPRE_CLAIMED = 3, BOOT_CPRE_GONE = 4 };
typedef struct BootComputePre {
    uint8_t id[32]; char variant[64]; char *path; volatile LONG state; ID3D12PipelineState *pso; uint8_t used_before, noted;
} BootComputePre;
static BootComputePre boot_compute_pre[BOOT_COMPUTE_PRE_MAX];
static unsigned boot_compute_pre_count;   /* filled before the thread starts, read-only while it runs */
static volatile LONG boot_compute_pre_started, boot_compute_pre_stop, boot_compute_pre_built, boot_compute_pre_failed;
static HANDLE boot_compute_pre_handle;
static uint64_t boot_compute_pre_taken, boot_compute_pre_claimed, boot_compute_pre_waited, boot_compute_pre_wait_qpc;
typedef struct BootComputePreArgs { ID3D12Device *dev; ID3D12RootSignature *root; } BootComputePreArgs;
static DWORD WINAPI boot_compute_pre_thread(void *arg) {
    BootComputePreArgs *a = (BootComputePreArgs *)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (int pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < boot_compute_pre_count && !boot_compute_pre_stop; ++i) {
            BootComputePre *p = &boot_compute_pre[i];
            if ((pass == 0) != (p->used_before != 0)) continue;
            if (InterlockedCompareExchange(&p->state, BOOT_CPRE_BUILDING, BOOT_CPRE_IDLE) != BOOT_CPRE_IDLE) continue;   /* the draw path has it */
            ID3D12PipelineState *pso = NULL; void *data = NULL; long n = 0;
            FILE *f = fopen(p->path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
                data = n > 0 ? malloc((size_t)n) : NULL;
                if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
                fclose(f);
            }
            if (data) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
                d.pRootSignature = a->root; d.CS.pShaderBytecode = data; d.CS.BytecodeLength = (size_t)n;
                if (FAILED(ID3D12Device_CreateComputePipelineState(a->dev, &d, &IID_ID3D12PipelineState, (void **)&pso))) pso = NULL;
                free(data);
            }
            if (pso && boot_compute_pre_stop) { ID3D12PipelineState_Release(pso); pso = NULL; }
            if (pso) { p->pso = pso; InterlockedIncrement(&boot_compute_pre_built); InterlockedExchange(&p->state, BOOT_CPRE_READY); }
            else { InterlockedIncrement(&boot_compute_pre_failed); InterlockedExchange(&p->state, BOOT_CPRE_GONE); }
        }
    {   FILE *lf = fopen("draw-path.log", "a");
        if (lf) { fprintf(lf, "nvn native: compute PSO pre-creation done: %ld pipelines, %ld failed\n", (long)boot_compute_pre_built, (long)boot_compute_pre_failed); fclose(lf); } }
    ID3D12RootSignature_Release(a->root); ID3D12Device_Release(a->dev); free(a);
    return 0;
}
/* After the compute root signature exists (and the manifest is loaded: the first lookup loaded it). */
static inline void boot_compute_pre_start(BootRender *r) {
    if (boot_compute_pre_started || !boot_compute.root) return;
    {   char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_COMPUTE_PRE", v, sizeof v);
        if (k && k < sizeof v && v[0] == '0') { boot_compute_pre_started = -1; return; } }
    boot_compute_pre_started = 1;
    boot_compute_pre_count = 0;
    AcquireSRWLockShared(&boot_manifest.lock);
    for (uint32_t i = 0; i < boot_manifest.n && boot_compute_pre_count < BOOT_COMPUTE_PRE_MAX; ++i) {
        const BootManifestEntry *m = &boot_manifest.e[i]; unsigned j;
        if (m->kind != 'c' || !m->var || !m->path) continue;
        for (j = 0; j < boot_compute_pre_count; ++j)   /* the first line of a key wins (boot_manifest_load_locked) */
            if (!memcmp(boot_compute_pre[j].id, m->id, 32) && !strcmp(boot_compute_pre[j].variant, m->var)) break;
        if (j < boot_compute_pre_count) continue;
        BootComputePre *p = &boot_compute_pre[boot_compute_pre_count];
        memset(p, 0, sizeof *p); memcpy(p->id, m->id, 32); snprintf(p->variant, sizeof p->variant, "%s", m->var);
        p->path = _strdup(m->path); if (!p->path) break;
        ++boot_compute_pre_count;
    }
    ReleaseSRWLockShared(&boot_manifest.lock);
    {   FILE *uf = fopen("compute-used.txt", "r"); char line[160];   /* "<id hex> <variant>" per program an earlier boot dispatched */
        while (uf && fgets(line, sizeof line, uf)) {
            char hex[65], var[64]; uint8_t id[32];
            if (sscanf(line, "%64s %63s", hex, var) != 2 || !boot_render_hex(hex, id)) continue;
            for (unsigned j = 0; j < boot_compute_pre_count; ++j)
                if (!memcmp(boot_compute_pre[j].id, id, 32) && !strcmp(boot_compute_pre[j].variant, var)) { boot_compute_pre[j].used_before = 1; boot_compute_pre[j].noted = 1; break; }
        }
        if (uf) fclose(uf); }
    BootComputePreArgs *a = (BootComputePreArgs *)calloc(1, sizeof *a);
    if (!a) return;
    a->dev = r->gpu->device; a->root = boot_compute.root;
    ID3D12Device_AddRef(a->dev); ID3D12RootSignature_AddRef(a->root);
    boot_compute_pre_handle = CreateThread(NULL, 0, boot_compute_pre_thread, a, 0, NULL);
    if (!boot_compute_pre_handle) { ID3D12RootSignature_Release(a->root); ID3D12Device_Release(a->dev); free(a); }
}
/* The pipeline built for `sh` ahead of its first dispatch, or NULL (compile it here). */
static inline ID3D12PipelineState *boot_compute_pre_take(const BootComputeShader *sh) {
    if (boot_compute_pre_started != 1) return NULL;
    for (unsigned i = 0; i < boot_compute_pre_count; ++i) {
        BootComputePre *p = &boot_compute_pre[i];
        if (memcmp(p->id, sh->id, 32) || strcmp(p->variant, sh->variant)) continue;
        if (!p->noted) {   /* the next boot builds this program first */
            p->noted = 1;
            FILE *uf = fopen("compute-used.txt", "a");
            if (uf) { for (unsigned k = 0; k < 32; ++k) fprintf(uf, "%02x", p->id[k]); fprintf(uf, " %s\n", p->variant); fclose(uf); }
        }
        LONG s = InterlockedCompareExchange(&p->state, BOOT_CPRE_CLAIMED, BOOT_CPRE_IDLE);
        if (s == BOOT_CPRE_IDLE) { ++boot_compute_pre_claimed; return NULL; }
        if (s == BOOT_CPRE_BUILDING) {
            LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
            while (p->state == BOOT_CPRE_BUILDING) Sleep(1);
            QueryPerformanceCounter(&q1);
            ++boot_compute_pre_waited; boot_compute_pre_wait_qpc += (uint64_t)(q1.QuadPart - q0.QuadPart);
            ++boot_compute_hitch_pso; boot_compute_hitch_pso_qpc += (uint64_t)(q1.QuadPart - q0.QuadPart);   /* shows in the hitch line */
        }
        if (InterlockedCompareExchange(&p->state, BOOT_CPRE_GONE, BOOT_CPRE_READY) == BOOT_CPRE_READY) {
            ID3D12PipelineState *pso = p->pso; p->pso = NULL; ++boot_compute_pre_taken; return pso; }
        return NULL;
    }
    return NULL;
}
/* Device loss (host_gpu_draw_exec.h): the pipelines belong to the removed device. The thread is joined, the table
 * emptied and the next dispatch starts a new generation on the replacement device. */
static inline void boot_compute_pre_device_lost(void) {
    if (boot_compute_pre_started != 1) return;
    boot_compute_pre_stop = 1;
    if (boot_compute_pre_handle) { WaitForSingleObject(boot_compute_pre_handle, 30000); CloseHandle(boot_compute_pre_handle); boot_compute_pre_handle = NULL; }
    for (unsigned i = 0; i < boot_compute_pre_count; ++i) {
        if (boot_compute_pre[i].pso) ID3D12PipelineState_Release(boot_compute_pre[i].pso);
        free(boot_compute_pre[i].path);
    }
    memset(boot_compute_pre, 0, sizeof boot_compute_pre); boot_compute_pre_count = 0;
    boot_compute_pre_stop = 0; boot_compute_pre_started = 0;
}

/* ---------------------------------------------------------------- deferred dispatches (native NVN path)
 * By default every dispatch completes before anything later is recorded (a waiting submission right
 * after it): later constant-buffer snapshots, texture builds and the CPU see its stores, and the pages it
 * stored (dirty bitmap, written by the shader) invalidate cached textures over them.
 *
 * With boot_compute_defer set (TOTK_NVN=native: constant buffers are read on the GPU, guest-memory updates
 * are GPU copies), a dispatch of a program whose stores are known is recorded without waiting: GPU order
 * and a UAV barrier put it after earlier work and before later work. What a program stores is learnt from
 * its first BOOT_COMPUTE_LEARN dispatches, which still complete at once (their dirty bitmap is read
 * alone). A dispatch is deferred only if the learnt ranges overlap no cached texture (a texture copy over
 * memory the dispatch writes would need rebuilding before later draws). Each deferred dispatch of a batch
 * stores into its own bitmap slot (the root UAV points at slot k = its place in the batch; a dispatch that
 * completes at once uses slot 0). When the queue is next idle (any waiting submission: syncs, the guards
 * below), the bits invalidate as before, and stores outside a program's learnt ranges are attributed to
 * that program and extend its ranges: a program used with many buffers, or with outputs of varying length,
 * converges (with one shared bitmap they sent the whole batch back to learning, two waiting dispatches per
 * program each time: play-12 had 2362 relearns for 2910 deferred dispatches). CPU reads of guest memory in
 * the meantime (texture builds, index expansion, indirect
 * arguments: boot_render_cpu_read) that overlap a pending dispatch's ranges wait for it first. */
#define BOOT_COMPUTE_LEARN 2
#define BOOT_COMPUTE_PROGRAMS 512
#define BOOT_COMPUTE_RANGES 32
#define BOOT_COMPUTE_SLOTS 32          /* at most this many bitmap slots */
#define BOOT_COMPUTE_BITMAP_BUDGET (UINT64_C(16) << 20)   /* bytes of all slots together (CPU-visible memory) */
#define BOOT_COMPUTE_MERGE_GAP 0x10000  /* ranges this close merge (fewer ranges, slightly wider guards) */
typedef struct BootComputeWrites {
    uint8_t id[32]; uint32_t used, seen, wide, count;
    uint64_t va[BOOT_COMPUTE_RANGES], size[BOOT_COMPUTE_RANGES];
    uint64_t checked_exec; int texture_overlap;
} BootComputeWrites;
static BootComputeWrites boot_compute_writes[BOOT_COMPUTE_PROGRAMS];
static int boot_compute_defer;
static BootComputeWrites *boot_compute_pending_list[64], *boot_compute_learning;
static unsigned boot_compute_pending, boot_compute_slots = 1;   /* slots of the current bitmap */
static uint64_t boot_compute_deferred, boot_compute_relearned, boot_compute_guard_waits, boot_compute_sync_dispatches;
static uint64_t boot_compute_grown, boot_compute_texture_rejects;
static inline BootComputeWrites *boot_compute_writes_of(const uint8_t id[32]) {
    unsigned h; memcpy(&h, id, 4); h &= BOOT_COMPUTE_PROGRAMS - 1;
    for (unsigned n = 0; n < BOOT_COMPUTE_PROGRAMS; ++n, h = (h + 1) & (BOOT_COMPUTE_PROGRAMS - 1)) {
        BootComputeWrites *w = &boot_compute_writes[h];
        if (!w->used) { memset(w, 0, sizeof *w); memcpy(w->id, id, 32); w->used = 1; return w; }
        if (!memcmp(w->id, id, 32)) return w;
    }
    return NULL;
}
static inline void boot_compute_writes_add(BootComputeWrites *w, uint64_t va, uint64_t size) {
    for (uint32_t i = 0; i < w->count; ++i)   /* merge with an overlapping or adjacent range */
        if (va <= w->va[i] + w->size[i] + BOOT_COMPUTE_MERGE_GAP && w->va[i] <= va + size + BOOT_COMPUTE_MERGE_GAP) {
            uint64_t lo = va < w->va[i] ? va : w->va[i], hi = va + size > w->va[i] + w->size[i] ? va + size : w->va[i] + w->size[i];
            w->va[i] = lo; w->size[i] = hi - lo; return;
        }
    if (w->count == BOOT_COMPUTE_RANGES) { w->wide = 1; return; }
    w->va[w->count] = va; w->size[w->count++] = size;
}
static inline int boot_compute_writes_overlap(const BootComputeWrites *w, uint64_t va, uint64_t size) {
    if (w->wide) return 1;
    for (uint32_t i = 0; i < w->count; ++i) if (w->va[i] < va + size && va < w->va[i] + w->size[i]) return 1;
    return 0;
}
static inline int boot_compute_can_defer(BootComputeWrites *w) {
    if (!boot_compute_defer || !w || w->seen < BOOT_COMPUTE_LEARN || w->wide || boot_compute_pending == 64) return 0;
    if (w->checked_exec != boot_render_exec_id) {   /* textures change between submissions: re-check once per submission */
        w->texture_overlap = 0;
        for (uint32_t i = 0; i < w->count && !w->texture_overlap; ++i) w->texture_overlap = boot_render_textures_overlap(w->va[i], w->size[i]);
        w->checked_exec = boot_render_exec_id;
    }
    if (w->texture_overlap) ++boot_compute_texture_rejects;
    return !w->texture_overlap;
}
/* drawcost: memset(p, 0, n) as one rep stosb (the linked memset stores one byte per iteration) */
static inline void boot_compute_zero(void *p, size_t n) {
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(0) : "memory");
}
/* drawcost: the stored runs of one bitmap slot (ascending, disjoint) whose cached textures are invalidated in one pass
 * over the live texture slots with a binary search per slot, instead of one pass per run (boot_render_invalidate_textures).
 * Same slots poisoned: a texture overlaps some run iff it overlaps the first run ending above its start; poison is
 * idempotent; the live-list compaction is the one the first per-run pass did (nothing between changes a slot). */
#define BOOT_COMPUTE_INV_RUNS 4096
static uint64_t boot_compute_inv_lo[BOOT_COMPUTE_INV_RUNS], boot_compute_inv_hi[BOOT_COMPUTE_INV_RUNS]; static unsigned boot_compute_inv_n;
static inline void boot_compute_inv_flush(void) {
    unsigned n = boot_compute_inv_n; boot_compute_inv_n = 0;
    if (!n || !boot_render_tex_slots) return;
    for (uint32_t k = 0; k < boot_render_tex_live_n; ) {   /* as boot_render_invalidate_textures */
        uint32_t i = boot_render_tex_live[k];
        BootRenderTexSlot *t = &boot_render_tex_slots[i];
        if (!t->valid) { boot_render_tex_live_in[i] = 0; boot_render_tex_live[k] = boot_render_tex_live[--boot_render_tex_live_n]; continue; }
        if (i < boot_render_tex_high) {
            unsigned lo = 0, hi = n;   /* first run whose end is above the texture's start */
            while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (boot_compute_inv_hi[mid] <= t->va) lo = mid + 1; else hi = mid; }
            if (lo < n && boot_compute_inv_lo[lo] < t->va + t->size) boot_render_tex_poison(i);   /* force mismatch */
        }
        ++k;
    }
}
static inline void boot_compute_inv_add(uint64_t va, uint64_t size) {
    if (boot_compute_inv_n == BOOT_COMPUTE_INV_RUNS) boot_compute_inv_flush();
    boot_compute_inv_lo[boot_compute_inv_n] = va; boot_compute_inv_hi[boot_compute_inv_n++] = va + size;
}
/* Queue idle (boot_render_submit_impl hook): stored pages invalidate; deferred programs that stored
 * outside their learnt ranges learn again; the learning dispatch records its ranges. */
static int boot_compute_ran;   /* a dispatch was recorded since the last idle (its stores may have set bitmap bits) */
static inline void boot_compute_idle(BootRender *r) {
    /* every waiting submission runs this; without a dispatch since the last one the bitmap is still all
     * zero (cleared below): scanning and clearing it was ~3.5% of the main thread */
    if (!boot_compute.dirty_cpu || !boot_compute_ran) { boot_compute_pending = 0; return; }
    boot_compute_ran = 0;
    /* slot k < boot_compute_slots - 1 holds dispatch k's stores; the last slot is shared by the batch's remaining
     * dispatches (more than the slots of a large page table): a store there outside every sharer's ranges sends
     * those sharers back to learning, as with one shared bitmap */
    unsigned used = boot_compute_pending ? boot_compute_pending : 1, slots = used < boot_compute_slots ? used : boot_compute_slots;
    for (unsigned k = 0; k < slots; ++k) {
        uint64_t *bitmap = boot_compute.dirty_cpu + (size_t)k * boot_compute.dirty_words;
        unsigned first = k, last = k == slots - 1 ? used - 1 : k;   /* pending dispatches sharing slot k */
        BootComputeWrites *owner = boot_compute_pending ? (first == last ? boot_compute_pending_list[k] : NULL) : boot_compute_learning;
        int outside = 0;
        for (uint32_t wi = 0; wi < boot_compute.dirty_words; ++wi) {
            /* t2-B: the words of a page-table chunk boot_render_map never wrote are zero (boot_render_pt_ever) */
            const uint32_t cs = BOOT_RENDER_PT_CHUNK_SHIFT - 6, cw = 1u << cs;   /* bitmap words per chunk */
            if (!(wi & (cw - 1)) && (wi >> cs) < sizeof boot_render_pt_ever && !boot_render_pt_ever[wi >> cs]) { wi += cw - 1; continue; }
            /* drawcost: eight zero words at once (word by word they were skipped one at a time: nothing else differs;
             * a chunk boundary is a multiple of 64 words, so no skipped word was due a chunk check) */
            if (!(wi & 7) && wi + 8 <= boot_compute.dirty_words) {
                const uint64_t *g = bitmap + wi;
                if (!(g[0] | g[1] | g[2] | g[3] | g[4] | g[5] | g[6] | g[7])) { wi += 7; continue; }
            }
            uint64_t bits = bitmap[wi];
            if (!bits) continue;
            bitmap[wi] = 0;
            while (bits) {
                unsigned b = (unsigned)__builtin_ctzll(bits);   /* drawcost: was a bit-by-bit loop (same b and e) */
                uint64_t run = ~(bits >> b); unsigned e = run ? b + (unsigned)__builtin_ctzll(run) : 64;
                uint64_t va = (r->va_base_page + (uint64_t)wi * 64 + b) << 12, size = (uint64_t)(e - b) << 12;
                /* cached textures over stored pages are stale (drawcost: the texture part batched per slot, see boot_compute_inv_add) */
                if (boot_image_trace_enabled) boot_render_invalidate(va, size);
                else { boot_render_cb_written(va, size); boot_compute_inv_add(va, size); }
                boot_render_rt_guest_written(r, va, size);   /* render-18: so are host render targets over them (reloaded on next use) */
                boot_ds_gpu_stored(va, size);   /* data-snapshot: pages compute really stored to (deferred dispatches' pending writes) */
                if (owner && (!boot_compute_pending || !boot_compute_writes_overlap(owner, va, size))) {
                    if (boot_compute_pending) ++boot_compute_grown;   /* a deferred dispatch stored outside its ranges */
                    boot_compute_writes_add(owner, va, size);
                } else if (boot_compute_pending && !owner && !outside) {
                    int known = 0;
                    for (unsigned j = first; j <= last && !known; ++j) known = boot_compute_writes_overlap(boot_compute_pending_list[j], va, size);
                    outside = !known;
                }
                bits &= e == 64 ? 0 : ~((UINT64_C(1) << e) - 1);
            }
        }
        boot_compute_inv_flush();   /* drawcost */
        if (outside) for (unsigned j = first; j <= last; ++j) { boot_compute_pending_list[j]->seen = 0; ++boot_compute_relearned; }
    }
    boot_compute_pending = 0;
}
/* boot_render_cpu_read guard: complete pending dispatches whose ranges overlap the read. */
static inline int boot_compute_guard(BootRender *r, uint64_t va, uint64_t size) {
    int hit = 0;
    for (unsigned k = 0; k < boot_compute_pending && !hit; ++k) hit = boot_compute_writes_overlap(boot_compute_pending_list[k], va, size);
    if (!hit) return 1;
    ++boot_compute_guard_waits;
    return boot_render_idle(r);   /* the dispatch may sit in an executed list (nothing recording): wait for the queue */
}
static inline void boot_compute_hooks(void) { boot_render_idle_hook = boot_compute_idle; boot_render_cpu_read_guard = boot_compute_guard; }

/* Decodes the QMD fields this executor honours; unsupported features fail with a reason. */
static inline int boot_qmd_decode(const uint32_t *w, BootQmd *q, char *why, size_t why_size) {
    memset(q, 0, sizeof *q); memcpy(q->words, w, 256);
#define QMD_FAIL(msg) do { snprintf(why, why_size, "%s", msg); return 0; } while (0)
    uint32_t version = w[18] & 15, major = (w[18] >> 4) & 15;
    if (major != 1 || version != 7) { snprintf(why, why_size, "QMD version %u.%u (only 01_07)", major, version); return 0; }
    if ((w[6] >> 13) & 1) QMD_FAIL("dependent QMD scheduling");
    if (w[0] | w[1] | w[2] | w[3]) QMD_FAIL("QMD queue pointers (circular queue launch)");
    q->release_enable[0] = (w[6] >> 10) & 1; q->release_enable[1] = (w[6] >> 11) & 1;
    q->program_offset = w[8];
    q->sampler_via_header = (w[11] >> 30) & 1;
    q->grid[0] = w[12]; q->grid[1] = w[13] & 0xffff; q->grid[2] = w[13] >> 16;
    if (w[14] | (w[15] & 0xffff) | (w[15] >> 16)) QMD_FAIL("CTA raster resume offsets");
    q->shared_size = w[17] & 0x3ffff;
    q->block[0] = w[18] >> 16; q->block[1] = w[19] & 0xffff; q->block[2] = w[19] >> 16;
    q->cb_mask = w[20] & 0xff;
    for (unsigned k = 0; k < 2; ++k) {
        const uint32_t *r = w + 23 + 3 * k;
        q->release_address[k] = ((uint64_t)(r[1] & 0xff) << 32) | r[0];
        q->release_op[k] = (r[1] >> 20) & 7; q->release_format[k] = (r[1] >> 24) & 3; q->release_reduce[k] = (r[1] >> 26) & 1;
        q->release_one_word[k] = r[1] >> 31; q->release_payload[k] = r[2];
    }
    for (unsigned i = 0; i < 8; ++i) {
        q->cb_address[i] = ((uint64_t)(w[30 + 2 * i] & 0xff) << 32) | w[29 + 2 * i];
        q->cb_size[i] = w[30 + 2 * i] >> 15;
    }
    q->local_low = w[45] & 0xffffff; q->barrier_count = (w[45] >> 27) & 31;
    q->local_high = w[46] & 0xffffff; q->register_count = (w[46] >> 24) & 0xff; q->local_crs = w[47] & 0xffffff;
    if (!q->block[0] || !q->block[1] || !q->block[2] || (uint64_t)q->block[0] * q->block[1] * q->block[2] > 1024 || q->block[2] > 64)
        QMD_FAIL("thread block size outside the host limits (1024 threads, z <= 64)");
    if (q->shared_size > 32768) QMD_FAIL("shared memory above the host's 32 KiB group shared limit");
    if (q->local_high) QMD_FAIL("high local memory window");
#undef QMD_FAIL
    return 1;
}

/* Variant key: the compile-time parameters of the HLSL translation. */
static inline void boot_compute_variant(const BootQmd *q, char *out, size_t n) {
    snprintf(out, n, "%ux%ux%us%ul%u", q->block[0], q->block[1], q->block[2], q->shared_size, q->local_low);
}

/* Code through the terminator (as maxwell_hlsl.Program finds it) -> sha256; keeps the bytes. */
/* t2-B: program bytes by guest VA, as boot_render_program_id's cache for graphics programs: when guest memory at the
 * program address (one contiguous range) still holds exactly the bytes an id was computed from, the word-by-word
 * scan below would read the same words, stop at the same (first) terminator and hash the same bytes, so the cached
 * id is the result, without the scan and its SHA-256 (BCryptHashData under every dispatch in the play-81 profile). */
#define BOOT_COMPUTE_ID_CACHE 256
static struct { uint64_t va; uint8_t *bytes; size_t size; uint8_t id[32]; } boot_compute_id_cache[BOOT_COMPUTE_ID_CACHE];
static uint64_t boot_compute_id_hits, boot_compute_id_misses;
static inline int boot_compute_program_id(BootRender *r, uint64_t va, uint8_t id[32]) {
    unsigned way = (unsigned)((va >> 8) ^ (va >> 17)) & (BOOT_COMPUTE_ID_CACHE - 1);
    if (boot_compute_id_cache[way].bytes && boot_compute_id_cache[way].va == va) {
        size_t cn = boot_compute_id_cache[way].size;
        const uint8_t *q = boot_render_guest(r, va, cn);
        uint8_t *copy = (q && !memcmp(q, boot_compute_id_cache[way].bytes, cn)) ? (uint8_t *)malloc(cn) : NULL;
        if (copy) {   /* boot_compute.program owns its bytes, as after a scan */
            memcpy(copy, boot_compute_id_cache[way].bytes, cn); memcpy(id, boot_compute_id_cache[way].id, 32);
            free(boot_compute.program); boot_compute.program = copy; boot_compute.program_size = cn;
            ++boot_compute_id_hits; return 1;
        }
    }
    ++boot_compute_id_misses;
    size_t cap = 4096, n = 0; uint8_t *buf = malloc(cap);
    if (!buf) return 0;
    for (uint64_t addr = 0; addr < (UINT64_C(1) << 20) * 8; addr += 8) {
        const uint8_t *wp = boot_render_guest(r, va + addr, 8);
        if (!wp) { free(buf); return 0; }
        if (n + 8 > cap) { uint8_t *g = realloc(buf, cap * 2); if (!g) { free(buf); return 0; } buf = g; cap *= 2; }
        memcpy(buf + n, wp, 8); n += 8;
        if (addr % 32 == 0) continue;
        uint64_t w; memcpy(&w, wp, 8);
        if ((w & (UINT64_C(0xfff0) << 48 | 0x20 | (UINT64_C(0xf) << 16) | 0x1f)) == ((UINT64_C(0xe240) << 48) | (UINT64_C(0x7) << 16) | 0xf)) {
            int64_t off = (int64_t)((w >> 20) & 0xffffff); if (off & 0x800000) off -= 0x1000000;
            int64_t target = (int64_t)addr + 8 + off;
            if (target == (int64_t)addr || (target % 32 == 0 && target + 8 == (int64_t)addr)) {
                boot_render_sha256(buf, n, id);
                {   uint8_t *keep = (uint8_t *)malloc(n);   /* t2-B: the id cache's copy */
                    if (keep) { memcpy(keep, buf, n); free(boot_compute_id_cache[way].bytes); boot_compute_id_cache[way].bytes = keep;
                                boot_compute_id_cache[way].va = va; boot_compute_id_cache[way].size = n; memcpy(boot_compute_id_cache[way].id, id, 32); } }
                free(boot_compute.program); boot_compute.program = buf; boot_compute.program_size = n;
                return 1;
            }
        }
    }
    free(buf); return 0;
}
static inline void boot_compute_dump(const uint8_t id[32], const char *variant, const BootQmd *q) {
#if TOTK_PLAY   /* play build: only the offline manifest, nothing is dumped for translation */
    (void)id; (void)variant; (void)q; return;
#endif
    char path[200]; int k = snprintf(path, sizeof path, "missing-shaders/cs-%s-", variant);
    for (int i = 0; i < 32; ++i) k += snprintf(path + k, sizeof path - k, "%02x", id[i]);
    CreateDirectoryA("missing-shaders", NULL);
    snprintf(path + k, sizeof path - k, ".bin");
    FILE *f = fopen(path, "wb"); if (f) { if (boot_compute.program) fwrite(boot_compute.program, 1, boot_compute.program_size, f); fclose(f); }
    snprintf(path + k, sizeof path - k, ".qmd");
    f = fopen(path, "wb"); if (f) { fwrite(q->words, 4, 64, f); fclose(f); }
}

/* shader-manifest.tsv line: <id hex> cs <variant> <tex handles|-> <dxil path> */
static inline BootComputeShader *boot_compute_shader(BootRender *r, const uint8_t id[32], const char *variant) {
    for (unsigned i = 0; i < boot_compute.shader_count; ++i) {
        BootComputeShader *s = &boot_compute.shaders[i];
        if (!memcmp(s->id, id, 32) && !strcmp(s->variant, variant)) return s;
    }
    if (boot_compute.shader_count == BOOT_COMPUTE_MAX_SHADERS) return NULL;
    char tex[512], path[900]; BootComputeShader *found = NULL;   /* play (47): from the manifest index */
    if (!boot_manifest_find(r->manifest_path, id, 'c', 0, variant, tex, sizeof tex, path, sizeof path)) return NULL;
    do {
        FILE *b = fopen(path, "rb"); if (!b) break;
        fseek(b, 0, SEEK_END); long n = ftell(b); fseek(b, 0, SEEK_SET);
        void *data = n > 0 ? malloc((size_t)n) : NULL;
        if (data && fread(data, 1, (size_t)n, b) == (size_t)n) {
            found = &boot_compute.shaders[boot_compute.shader_count++];
            memset(found, 0, sizeof *found);
            memcpy(found->id, id, 32); snprintf(found->variant, sizeof found->variant, "%s", variant);
            found->dxil = data; found->size = (size_t)n;
            for (char *t = tex; *t && *t != '-' && found->tex_count < 64; ) {
                found->tex[found->tex_count++] = (uint32_t)strtoul(t, &t, 10);
                if (*t == ',') ++t;
            }
        } else free(data);
        fclose(b);
    } while (0);
    return found;
}

/* Root signature: the graphics layout (maxwell_runtime.hlsli) with every table visible to compute,
 * plus the store bitmap as a root UAV (u1, space0). */
static inline int boot_compute_root_signature(BootRender *r) {
    if (boot_compute.root) return 1;
    D3D12_DESCRIPTOR_RANGE1 cbv = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 19, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 mem = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 arena_srv = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BOOT_RENDER_MAX_REGIONS, 0, 7,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 arena_uav = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, BOOT_RENDER_MAX_REGIONS, 0, 7,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    D3D12_DESCRIPTOR_RANGE1 tex[BOOT_RENDER_DIMS];
    for (unsigned i = 0; i < BOOT_RENDER_DIMS; ++i) {
        D3D12_DESCRIPTOR_RANGE1 t = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BOOT_RENDER_TEX_SLOTS, 0, boot_render_dim_space[i],
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, BOOT_RENDER_TEX_SLOTS * i};
        tex[i] = t;
    }
    D3D12_DESCRIPTOR_RANGE1 smp[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, BOOT_RENDER_SAMPLERS, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, BOOT_RENDER_SAMPLERS, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, BOOT_RENDER_SAMPLERS}};
    D3D12_ROOT_PARAMETER1 p[7]; memset(p, 0, sizeof p);
    for (unsigned i = 0; i < 6; ++i) { p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; p[i].DescriptorTable.NumDescriptorRanges = 1; }
    p[0].DescriptorTable.pDescriptorRanges = &cbv;
    p[1].DescriptorTable.pDescriptorRanges = &mem;
    p[2].DescriptorTable.pDescriptorRanges = tex; p[2].DescriptorTable.NumDescriptorRanges = BOOT_RENDER_DIMS;
    p[3].DescriptorTable.pDescriptorRanges = smp; p[3].DescriptorTable.NumDescriptorRanges = 2;
    p[4].DescriptorTable.pDescriptorRanges = &arena_srv;
    p[5].DescriptorTable.pDescriptorRanges = &arena_uav;
    p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[6].Descriptor.ShaderRegister = 1; p[6].Descriptor.RegisterSpace = 0; p[6].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc; memset(&desc, 0, sizeof desc);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters = 7; desc.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &err))) {
        snprintf(r->error, sizeof r->error, "compute root signature: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(r->gpu->device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                  &IID_ID3D12RootSignature, (void **)&boot_compute.root);
    if (SUCCEEDED(hr)) boot_psoc_root_note(boot_compute.root, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));   /* play (73) */
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) { snprintf(r->error, sizeof r->error, "compute root signature hr=0x%08lx", (unsigned long)hr); return 0; }
    return 1;
}

/* CPU-visible store bitmap: one bit per page-table entry (4 KiB guest GPU page). */
static inline int boot_compute_dirty(BootRender *r) {
    uint32_t words = (r->ptable_pages + 63) / 64; if (!words) words = 1;
    if (boot_compute.dirty && boot_compute.dirty_words >= words) return 1;
    /* the old bitmap may still be written by executed, unfinished dispatches (lists run without waiting, nothing
     * recording): wait for the queue (which also reads their bits) before it is released */
    if (boot_compute.dirty && !boot_render_idle(r)) return 0;
    if (boot_compute.dirty) { ID3D12Resource_Release(boot_compute.dirty); boot_compute.dirty = NULL; boot_compute.dirty_cpu = NULL; }
    uint64_t slot_bytes = (uint64_t)words * 8;
    unsigned slots = (unsigned)(BOOT_COMPUTE_BITMAP_BUDGET / slot_bytes);
    if (slots > BOOT_COMPUTE_SLOTS) slots = BOOT_COMPUTE_SLOTS;
    if (slots < 1) slots = 1;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = slot_bytes * slots; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(ID3D12Device_CreateCommittedResource(r->gpu->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                    &IID_ID3D12Resource, (void **)&boot_compute.dirty))) return 0;
    if (FAILED(ID3D12Resource_Map(boot_compute.dirty, 0, NULL, (void **)&boot_compute.dirty_cpu))) { ID3D12Resource_Release(boot_compute.dirty); boot_compute.dirty = NULL; return 0; }
    memset(boot_compute.dirty_cpu, 0, (size_t)(slot_bytes * slots));
    ID3D12Resource_SetName(boot_compute.dirty, L"compute store bitmap");   /* names the allocation in DRED page-fault reports */
    boot_compute.dirty_words = words;   /* the stride of one slot */
    boot_compute_slots = slots;
    {   static unsigned logged; FILE *lf = logged++ < 8 ? fopen("draw-path.log", "a") : NULL;
        if (lf) { fprintf(lf, "compute store bitmap: %u slots of %llu KiB (page table %u pages)\n", slots, (unsigned long long)(slot_bytes >> 10), r->ptable_pages); fclose(lf); } }
    return 1;
}

/* Semaphore release of a completed grid (QMD RELEASE0/1): payload, or a 32-bit reduction. */
static inline int boot_compute_release(BootRender *r, const BootQmd *q, unsigned k) {
    uint8_t *p = boot_render_guest(r, q->release_address[k], q->release_one_word[k] ? 4 : 16);
    if (!p) { snprintf(r->error, sizeof r->error, "QMD release %u address %llx outside guest memory", k, (unsigned long long)q->release_address[k]); return 0; }
    uint32_t v = q->release_payload[k];
    if (q->release_reduce[k]) {
        uint32_t old; memcpy(&old, p, 4); int s = q->release_format[k] == 1;
        if (q->release_format[k] > 1) { snprintf(r->error, sizeof r->error, "QMD release reduction format %u", q->release_format[k]); return 0; }
        switch (q->release_op[k]) {
            case 0: v = old + v; break;
            case 1: v = s ? (uint32_t)((int32_t)old < (int32_t)v ? (int32_t)old : (int32_t)v) : (old < v ? old : v); break;
            case 2: v = s ? (uint32_t)((int32_t)old > (int32_t)v ? (int32_t)old : (int32_t)v) : (old > v ? old : v); break;
            case 3: v = old >= v ? 0 : old + 1; break;          /* RED_INC: wrap at payload */
            case 4: v = (old == 0 || old > v) ? v : old - 1; break;   /* RED_DEC */
            case 5: v = old & v; break;
            case 6: v = old | v; break;
            default: v = old ^ v; break;
        }
    }
    memcpy(p, &v, 4);
    if (!q->release_one_word[k]) {
        uint64_t ticks;
        if (boot_guest_dilated()) ticks = boot_guest_gpu_ticks();   /* guest clock, GPU timer units */
        else {
            LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
            uint64_t ns = (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart); ticks = (uint64_t)((__uint128_t)ns * 384 / 625);
        }
        uint32_t z = 0; memcpy(p + 4, &z, 4); memcpy(p + 8, &ticks, 8);
    }
    boot_render_cb_written(q->release_address[k], q->release_one_word[k] ? 4 : 16);   /* a list may be open (empty grid) */
    return 1;
}

/* Executes one recorded launch. Returns BOOT_RENDER_OK or an error code with r->error set;
 * BOOT_RENDER_E_SHADER means the program was dumped for ahead-of-time translation. */
static inline int boot_compute_dispatch_decoded(BootRender *r, const BootComputeParams *c, BootQmd q, uint8_t id_out[32], char *variant_out, size_t variant_size);
static inline int boot_compute_dispatch(BootRender *r, const BootComputeParams *c, BootQmd *q_out, uint8_t id_out[32], char *variant_out, size_t variant_size) {
    BootQmd q; char why[160];
    const uint32_t *qw = (const uint32_t *)boot_render_guest(r, c->qmd, 256);
    if (!qw) { snprintf(r->error, sizeof r->error, "QMD %llx outside guest memory", (unsigned long long)c->qmd); return BOOT_RENDER_E_MEMORY; }
    if (!boot_qmd_decode(qw, &q, why, sizeof why)) { snprintf(r->error, sizeof r->error, "QMD %llx: %s", (unsigned long long)c->qmd, why); return BOOT_RENDER_E_STATE; }
    if (q_out) *q_out = q;
    {   /* calibration (nvn-work): TOTK_COMPUTE_CB_DUMP=N logs the first N dispatches' QMD and constant
         * buffers (c0 0x000-0x7ff and c2 nonzero words) to frames\compute-cb.txt */
        static long left = -1; static FILE *f;
        if (left < 0) { const char *e = getenv("TOTK_COMPUTE_CB_DUMP"); left = e ? atol(e) : 0; }
        if (left > 0 && (f || (f = fopen("frames\\compute-cb.txt", "w")))) {
            --left;
            fprintf(f, "dispatch program %llx+%x grid %u %u %u block %u %u %u shared %u regs %u cb_mask %x\n  qmd", (unsigned long long)c->program_region, q.program_offset,
                    q.grid[0], q.grid[1], q.grid[2], q.block[0], q.block[1], q.block[2], q.shared_size, q.register_count, q.cb_mask);
            for (unsigned k = 0; k < 64; ++k) fprintf(f, " %08x", qw[k]);
            fputc('\n', f);
            for (unsigned b = 0; b < 8; ++b) if ((q.cb_mask >> b) & 1) {
                fprintf(f, "  c%u %llx size %u:", b, (unsigned long long)q.cb_address[b], q.cb_size[b]);
                uint32_t n = q.cb_size[b] < 0x800 ? q.cb_size[b] / 4 : 0x200;
                const uint32_t *p = (const uint32_t *)boot_render_guest(r, q.cb_address[b], (uint64_t)n * 4);
                for (uint32_t k = 0; p && k < n; ++k) if (p[k] && (b == 0 || b == 2 || k < 16)) fprintf(f, " [%x]=%08x", k * 4, p[k]);
                fputc('\n', f);
            }
            fflush(f);
        }
    }
    return boot_compute_dispatch_decoded(r, c, q, id_out, variant_out, variant_size);
}
/* A dispatch described by decoded QMD fields: from a guest QMD (above) or assembled from NVN calls by the
 * native executor (host_nvn_play.h). c supplies the program region, texture/sampler pools and bindless slot. */
static inline int boot_compute_dispatch_decoded(BootRender *r, const BootComputeParams *c, BootQmd q, uint8_t id_out[32], char *variant_out, size_t variant_size) {
    /* render-38d: an indirect dispatch whose group counts the GPU reads (ExecuteIndirect); q.grid is 1 x 1 x 1 */
    BootRenderIndOp ind = boot_render_next_ind; boot_render_next_ind.kind = 0;
    if (ind.kind != BOOT_RENDER_IND_DISPATCH) ind.kind = 0;
    if (c->bindless_cb != 2) { snprintf(r->error, sizeof r->error, "bindless texture constant buffer %u (shaders read handles from c2)", c->bindless_cb); return BOOT_RENDER_E_STATE; }
    if (!q.grid[0] || !q.grid[1] || !q.grid[2]) goto release;   /* an empty grid runs no threads; releases still happen */
    char variant[64]; boot_compute_variant(&q, variant, sizeof variant);
    if (variant_out) snprintf(variant_out, variant_size, "%s", variant);
    uint8_t id[32];
    if (!boot_compute_program_id(r, c->program_region + q.program_offset, id)) {
        snprintf(r->error, sizeof r->error, "compute program at %llx+%x: terminator not found", (unsigned long long)c->program_region, q.program_offset);
        return BOOT_RENDER_E_SHADER;
    }
    if (id_out) memcpy(id_out, id, 32);
    LARGE_INTEGER hz0, hz1; QueryPerformanceCounter(&hz0);   /* play (45): compute shader lookup/load time */
    BootComputeShader *sh = boot_compute_shader(r, id, variant);
    QueryPerformanceCounter(&hz1); boot_compute_hitch_load_qpc += (uint64_t)(hz1.QuadPart - hz0.QuadPart);
    if (!sh) { boot_compute_dump(id, variant, &q); snprintf(r->error, sizeof r->error, "compute program/variant %s not in manifest", variant); return BOOT_RENDER_E_SHADER; }
    if (!boot_compute_root_signature(r)) return BOOT_RENDER_E_D3D;
    boot_compute_pre_start(r);   /* play (74): every manifest program compiled in the background */
    boot_compute_hooks();
    boot_compute_ran = 1;
    BootComputeWrites *learnt = boot_compute_writes_of(id);
    int defer = boot_compute_can_defer(learnt);
    /* a dispatch that completes at once reads its own dirty bitmap: finish deferred ones first */
    if (!defer && boot_compute_pending && !boot_render_idle(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "GPU submit (deferred dispatches)");
    if (!sh->pso) sh->pso = boot_compute_pre_take(sh);   /* play (74): built ahead of use, else compiled below */
    if (!sh->pso) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
        d.pRootSignature = boot_compute.root; d.CS.pShaderBytecode = sh->dxil; d.CS.BytecodeLength = sh->size;
        LARGE_INTEGER pz0, pz1; QueryPerformanceCounter(&pz0);
        HRESULT hr = boot_psoc_create_compute(r, r->gpu->device, &d, &sh->pso);   /* play (73): from the persistent library, else compiled and stored */
        QueryPerformanceCounter(&pz1); ++boot_compute_hitch_pso; boot_compute_hitch_pso_qpc += (uint64_t)(pz1.QuadPart - pz0.QuadPart);
        if (FAILED(hr)) { snprintf(r->error, sizeof r->error, "CreateComputePipelineState hr=0x%08lx", (unsigned long)hr); return BOOT_RENDER_E_D3D; }
    }
    /* render-18: host render targets over the memory this program stores to (its learnt ranges) are written back
     * first, in queue order: the dispatch's stores then land on their current image, and the targets, marked stale
     * when the stores are seen (boot_compute_idle), reload the result on their next use instead of a later
     * write-back of the old host image overwriting it (a target drawn once, e.g. in a cutscene, and filled by
     * compute afterwards was sampled with its old contents for good). Learning dispatches (ranges unknown) and
     * programs with too many ranges only get the stale marking. */
    if (learnt && learnt->seen && !learnt->wide)
        for (uint32_t i = 0; i < learnt->count; ++i)
            if (!boot_render_flush_targets(r, learnt->va[i], learnt->size[i])) return boot_render_fail(r, BOOT_RENDER_E_D3D, "render target write-back before dispatch");
    /* Earlier work first: constant buffers and textures are snapshotted while recording, so guest
     * memory must hold every store recorded before. Only translated graphics programs that store to
     * guest memory can change it on the GPU (compute completes before later work records, render
     * target write-backs are CPU copies): without such a draw since the last waiting submission the
     * recorded work is executed without waiting for it (queue order keeps it before the dispatch). */
    if (r->recording && !defer) {   /* deferred: GPU order and the UAV barrier below keep it after earlier work */
        if (boot_render_gmem_pending) { if (!boot_render_submit(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "GPU submit before dispatch"); }
        else { ++boot_compute_prewait_skipped; if (!boot_render_flush(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "execute before dispatch"); }
    }
    if (!boot_compute_dirty(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "store bitmap");
    if (!boot_render_begin(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    boot_render_dfb_zt = NULL;   /* batch-b: no draw's depth target is being written (a draw that returned early may have left it set) */
    /* textures: handles from the bindless constant buffer, read only when the program references them */
    for (unsigned i = 0; i < sh->tex_count; ++i) {
        unsigned b = c->bindless_cb;
        if (!((q.cb_mask >> b) & 1) || (uint64_t)sh->tex[i] * 4 + 4 > q.cb_size[b]) continue;
        const uint8_t *hp = boot_render_guest(r, q.cb_address[b] + (uint64_t)sh->tex[i] * 4, 4);
        if (!hp) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "texture handle outside guest memory");
        uint32_t handle; memcpy(&handle, hp, 4);
        uint32_t tic = handle & 0xfffff, tsc = q.sampler_via_header ? tic : handle >> 20;
        int rc = boot_render_texture(r, c->tic_address, tic); if (rc) return rc;
        boot_render_tex_pend(tic);
        rc = boot_render_sampler(r, c->tsc_address, tsc); if (rc) return rc;
    }
    /* Room in the open list's segment for what this dispatch takes (its constant-buffer snapshots, a HostState
     * and a 19-descriptor table per grid slice), as boot_render_draw: otherwise the recorded
     * work is executed without waiting and the dispatch continues in the next segment (whose fence is waited for
     * only if the GPU still uses it); more than a segment: the whole rings once the queue is idle. Deferred
     * dispatches no longer end their list with a wait, so without this the segment filled ("cbv ring"). The
     * texture loop above may also have submitted: a list is begun here in any case. */
    {   uint64_t slices = (uint64_t)((q.grid[0] + 65534) / 65535) * ((q.grid[1] + 65534) / 65535) * ((q.grid[2] + 65534) / 65535);
        if (!slices) slices = 1;
        uint64_t need = 256 * slices + (ind.kind ? 0x700 : 0), descs = 19 * slices;   /* + this dispatch's snapshots (direct slots take none) */
        for (unsigned slot = 0; slot < 18; ++slot) {
            int valid = slot < 8 && ((q.cb_mask >> slot) & 1);
            if (valid && ((q.cb_direct >> slot) & 1) && boot_render_cbv_direct_ok(r, q.cb_address[slot], q.cb_size[slot])) continue;
            need += boot_render_cb_ring_bytes(valid ? q.cb_size[slot] : 0);
        }
        int big = need > boot_render_seg_bytes() || descs > BOOT_RENDER_CBV_SEGMENT;
        if (r->recording && (r->cbv_used + descs > boot_render_cbv_end || r->ring_used + need > r->ring_size)) {
            ++boot_render_ring_full;
            if (!boot_render_seg.whole && boot_render_seg.r == r && !big) {
                if (!boot_render_flush(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "execute (ring segment full, dispatch)");
            } else if (!boot_render_submit(r)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "submit (rings full, dispatch)");
        }
        if (!r->recording && !boot_render_begin_in(r, boot_render_seg_bind(r) && big)) return boot_render_fail(r, BOOT_RENDER_E_D3D, "command list reset");
    }
    if (r->cbv_used + 19 > boot_render_cbv_end) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "cbv ring");
    uint32_t base = BOOT_RENDER_HEAP_RING + r->cbv_used;
    for (unsigned slot = 0; slot < 18; ++slot) {
        int valid = slot < 8 && ((q.cb_mask >> slot) & 1);
        if (ind.kind && slot == 0 && valid) {   /* render-38d: c0 = the ring slot the pre-pass writes */
            uint32_t staged = (q.cb_size[0] + 255) & ~255u;
            const uint8_t *c0 = q.cb_size[0] >= 0x20c && staged <= BOOT_RENDER_IND_ARGS ? boot_render_guest(r, q.cb_address[0], q.cb_size[0]) : NULL;
            if (!c0 || !boot_render_ind_stage(r, &ind, c0, q.cb_size[0], staged, 0x200 / 4)) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring (indirect dispatch c0)");
            D3D12_CONSTANT_BUFFER_VIEW_DESC cv = {boot_render_ind_slot_gpu(&ind), staged};
            boot_render_cbv_put(r, &cv, base); continue;
        }
        if (valid && ((q.cb_direct >> slot) & 1) && boot_render_cbv_direct(r, q.cb_address[slot], q.cb_size[slot], base + slot)) continue;
        if (!boot_render_cbv(r, valid ? q.cb_address[slot] : 0, valid ? q.cb_size[slot] : 0, base + slot)) {
            snprintf(r->error, sizeof r->error, "compute constant buffer %u va %llx size %u", slot, (unsigned long long)q.cb_address[slot], q.cb_size[slot]);
            return BOOT_RENDER_E_MEMORY;
        }
    }
    if (r->ring_used + 256 > r->ring_size) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring");
    uint8_t *hs = r->ring_cpu + r->ring_used; uint64_t hs_offset = r->ring_used;
    memset(hs, 0, 256); r->ring_used += 256;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv = {ID3D12Resource_GetGPUVirtualAddress(r->ring) + hs_offset, 256};
    ID3D12Device_CreateConstantBufferView(r->gpu->device, &cv, boot_render_cpu(r, r->srv_heap, r->srv_inc, base + 18));
    r->cbv_used += 19;
    if (!defer) boot_compute_zero(boot_compute.dirty_cpu, (size_t)boot_compute.dirty_words * 8);   /* deferred: bits accumulate until idle */
    if (ind.kind && !boot_render_ind_prepass(r, &ind)) return boot_render_fail(r, BOOT_RENDER_E_MEMORY, "indirect dispatch arguments outside one guest region");   /* render-38d */
    ID3D12GraphicsCommandList *cl = r->cl;
    boot_render_gw_settle(r);   /* guest memory written by GPU copies before this dispatch: readable again */
    if (defer) { D3D12_RESOURCE_BARRIER ub; memset(&ub, 0, sizeof ub); ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; BOOT_GP_RB("compute UAV", cl, 1, &ub); }
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, boot_compute.root);
    ID3D12GraphicsCommandList_SetPipelineState(cl, sh->pso); ++boot_render_bind_epoch;   /* replaced the graphics pipeline */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, boot_render_gpu(r, r->srv_heap, r->srv_inc, base));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, boot_render_gpu(r, r->srv_heap, r->srv_inc, 0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 2, boot_render_gpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_TEX0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 3, boot_render_gpu(r, r->sampler_heap, r->sampler_inc, 0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 4, boot_render_gpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_ARENA_SRV));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 5, boot_render_gpu(r, r->srv_heap, r->srv_inc, BOOT_RENDER_HEAP_ARENA_UAV));
    {   /* this dispatch's bitmap slot: its place in the deferred batch (submissions above may have emptied it), else 0 */
        uint64_t slot = !defer ? 0 : boot_compute_pending < boot_compute_slots ? boot_compute_pending : boot_compute_slots - 1;
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 6, ID3D12Resource_GetGPUVirtualAddress(boot_compute.dirty) + slot * boot_compute.dirty_words * 8);
    }
    /* Grids beyond D3D12's 65535 groups per dimension run as several dispatches; HostState
     * words 4-6 carry the group offset (SR_CTAID = offset + SV_GroupID). One HostState per slice. */
    BOOT_GP_PRE(cl);   /* render-40: the dispatch (all its grid slices) is one mark */
    int first = 1;
    for (uint32_t z0 = 0; z0 < q.grid[2]; z0 += 65535)
    for (uint32_t y0 = 0; y0 < q.grid[1]; y0 += 65535)
    for (uint32_t x0 = 0; x0 < q.grid[0]; x0 += 65535) {
        uint32_t host[8] = {0, 0, r->ptable_pages, r->va_base_page, x0, y0, z0, 0};
        if (first) memcpy(hs, host, sizeof host);
        else {
            if (r->ring_used + 256 > r->ring_size || r->cbv_used + 1 > boot_render_cbv_end) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "ring (split grid)");
            uint8_t *h2 = r->ring_cpu + r->ring_used; memset(h2, 0, 256); memcpy(h2, host, sizeof host);
            /* a fresh 19-descriptor table: copy b0..b17 and point b18 at this slice's HostState */
            uint32_t nb = BOOT_RENDER_HEAP_RING + r->cbv_used;
            if (r->cbv_used + 19 > boot_render_cbv_end) return boot_render_fail(r, BOOT_RENDER_E_CAPACITY, "cbv ring (split grid)");
            ID3D12Device_CopyDescriptorsSimple(r->gpu->device, 18, boot_render_cpu(r, r->srv_heap, r->srv_inc, nb), boot_render_cpu(r, r->srv_heap, r->srv_inc, base),
                                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CONSTANT_BUFFER_VIEW_DESC c2 = {ID3D12Resource_GetGPUVirtualAddress(r->ring) + r->ring_used, 256};
            ID3D12Device_CreateConstantBufferView(r->gpu->device, &c2, boot_render_cpu(r, r->srv_heap, r->srv_inc, nb + 18));
            r->ring_used += 256; r->cbv_used += 19;
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, boot_render_gpu(r, r->srv_heap, r->srv_inc, nb));
            D3D12_RESOURCE_BARRIER ub; memset(&ub, 0, sizeof ub); ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = NULL;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &ub);
        }
        first = 0;
        uint32_t gx = q.grid[0] - x0 < 65535 ? q.grid[0] - x0 : 65535, gy = q.grid[1] - y0 < 65535 ? q.grid[1] - y0 : 65535;
        uint32_t gz = q.grid[2] - z0 < 65535 ? q.grid[2] - z0 : 65535;
        boot_render_tex_commit();
        if (boot_render_markers_on()) boot_render_marker(cl, "dispatch", sh->id, NULL, ((uint64_t)gx << 32) | ((uint64_t)gy << 16) | gz);   /* render-27a: grid */
        boot_dred_work(cl, ind.kind ? 5u : BOOT_DRED_DISPATCH, BOOT_DRED_COMPUTE, sh->id, NULL, ((uint64_t)gx << 32) | ((uint64_t)gy << 16) | gz, boot_render_exec_id, boot_render_watch_event);
        if (ind.kind) boot_render_ind_execute(r, &ind);   /* render-38d: D3D12 op 5 (ExecuteIndirect) in the DRED ledger */
        else ID3D12GraphicsCommandList_Dispatch(cl, gx, gy, gz);
    }
    ++boot_compute.dispatches; boot_compute.groups += (uint64_t)q.grid[0] * q.grid[1] * q.grid[2];
    BOOT_GP_POST(BOOT_GP_COMPUTE, "compute", cl, boot_gp_id64(sh->id), 0);
    if (defer) {
        { D3D12_RESOURCE_BARRIER ub; memset(&ub, 0, sizeof ub); ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; BOOT_GP_RB("compute UAV", cl, 1, &ub); }
        boot_compute_pending_list[boot_compute_pending++] = learnt; ++boot_compute_deferred;
        goto release;
    }
    /* Complete before anything later is recorded: its stores must be visible to later snapshots and the CPU.
     * The idle hook (boot_compute_idle) invalidates cached textures over the stored pages and records them
     * as this program's store ranges. */
    boot_compute_learning = learnt;
    int submitted = boot_render_submit(r);
    boot_compute_learning = NULL; ++boot_compute_sync_dispatches;
    if (!submitted) return boot_render_fail(r, BOOT_RENDER_E_D3D, "GPU submit after dispatch");
    if (learnt) ++learnt->seen;
release:
    for (unsigned k = 0; k < 2; ++k) if (q.release_enable[k] && !boot_compute_release(r, &q, k)) return BOOT_RENDER_E_MEMORY;
    return BOOT_RENDER_OK;
}
#endif
