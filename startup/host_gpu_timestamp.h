#ifndef TOTK_HOST_GPU_TIMESTAMP_H
#define TOTK_HOST_GPU_TIMESTAMP_H
#include "host_gpu.h"
typedef struct BootGpuTimestamp {
    ID3D12QueryHeap*heap;
    ID3D12Resource*readback;
    ID3D12CommandAllocator*allocator;
    ID3D12GraphicsCommandList*list;
    uint64_t ticks,frequency;
    unsigned submitted,completed;
} BootGpuTimestamp;
static inline int boot_gpu_timestamp_close(BootGpuTimestamp*t){
    if(!t)return 1;
    if(t->submitted && !t->completed)return 0;
    if(t->list)ID3D12GraphicsCommandList_Release(t->list);
    if(t->allocator)ID3D12CommandAllocator_Release(t->allocator);
    if(t->readback)ID3D12Resource_Release(t->readback);
    if(t->heap)ID3D12QueryHeap_Release(t->heap);
    free(t);return 1;
}
/* Own resources until queue completion is proven, including on failure. */
static inline BootGpuTimestamp*boot_gpu_timestamp_prepare(BootGpu*g){
    if(!g)return NULL;
    BootGpuTimestamp*t=calloc(1,sizeof(*t));if(!t)return NULL;
    if(FAILED(ID3D12CommandQueue_GetTimestampFrequency(g->queue,&t->frequency)) || !t->frequency)goto fail;
    D3D12_QUERY_HEAP_DESC query={0};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=1;
    if(FAILED(ID3D12Device_CreateQueryHeap(g->device,&query,&IID_ID3D12QueryHeap,(void**)&t->heap)))goto fail;
    D3D12_HEAP_PROPERTIES heap={0};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc={0};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=8;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(ID3D12Device_CreateCommittedResource(g->device,&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&t->readback)))goto fail;
    if(FAILED(ID3D12Device_CreateCommandAllocator(g->device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&t->allocator)))goto fail;
    if(FAILED(ID3D12Device_CreateCommandList(g->device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,t->allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&t->list)))goto fail;
    ID3D12GraphicsCommandList_EndQuery(t->list,t->heap,D3D12_QUERY_TYPE_TIMESTAMP,0);
    ID3D12GraphicsCommandList_ResolveQueryData(t->list,t->heap,D3D12_QUERY_TYPE_TIMESTAMP,0,1,t->readback,0);
    if(FAILED(ID3D12GraphicsCommandList_Close(t->list)))goto fail;
    return t;
fail:boot_gpu_timestamp_close(t);return NULL;
}
static inline int boot_gpu_timestamp_execute(BootGpu*g,BootGpuTimestamp*t){
    if(!g || !t || t->submitted)return 0;
    ID3D12CommandList*lists[]={(ID3D12CommandList*)t->list};
    ID3D12CommandQueue_ExecuteCommandLists(g->queue,1,lists);t->submitted=1;
    if(!boot_gpu_wait(g))return 0;t->completed=1;
    uint64_t*mapped=NULL;D3D12_RANGE range={0,8},empty={0,0};
    if(FAILED(ID3D12Resource_Map(t->readback,0,&range,(void**)&mapped)))return 0;
    t->ticks=*mapped;ID3D12Resource_Unmap(t->readback,0,&empty);return 1;
}
/* Correlate a completed GPU sample with the host QPC timeline. These are
 * host nanoseconds; conversion to the source GPU timer remains separate. */
static inline int boot_gpu_timestamp_host_ns(BootGpu*g,const BootGpuTimestamp*t,uint64_t*out){
    if(!g || !t || !out || !t->completed || !t->frequency)return 0;
    uint64_t gpu=0,cpu=0;LARGE_INTEGER frequency;
    if(FAILED(ID3D12CommandQueue_GetClockCalibration(g->queue,&gpu,&cpu)) ||
       !QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0)return 0;
    __int128_t ns=(__int128_t)cpu*1000000000/(uint64_t)frequency.QuadPart;
    ns+=((__int128_t)t->ticks-gpu)*1000000000/t->frequency;
    if(ns<0 || ns>UINT64_MAX)return 0;
    *out=(uint64_t)ns;return 1;
}
/* ---------------------------------------------------------------- render-40: GPU timestamp profiler
 * Opt-in: TOTK_GPU_PROFILE=1 (consecutive draws with the same pipeline and the same bound target textures are timed
 * as one run) or TOTK_GPU_PROFILE=2 (every draw timed alone). Off (unset / any other value, the default): every site
 * costs one load and compare of boot_gp_mode and records exactly the D3D12 calls it recorded before; nothing is
 * created or allocated. Instrumentation only: no site adds, removes, reorders or waits for GPU work.
 *
 * On: one TIMESTAMP query heap of BOOT_GP_QUERIES queries used as a ring (monotonic sequence number, slot = seq mod
 * size) and one READBACK buffer with an 8-byte slot per query, so the readback ring spans several frames (up to
 * BOOT_GP_FRAMES frames waiting). Each site records an EndQuery on the list its work is recorded on, right after
 * the work; a list's first query is recorded right after its Reset (list begin), its last right before its Close
 * (list end), followed by ResolveQueryData of that list's own queries (contiguous runs) into their readback slots.
 * A frame is present to present (boot_present). It is read at a later present once the queue fence of its last
 * list has completed (GetCompletedValue only, never a wait); a frame that would be overwritten before that (query
 * or list ring wrapped, more than BOOT_GP_FRAMES waiting) is dropped and counted. Device removal (the fence reads
 * UINT64_MAX) or a new device disables the profiler; its heap/readback are then left allocated on purpose (lists
 * already recorded may still reference them). One recording thread (the render thread) is assumed, as for r->cl.
 *
 * Attribution caveats: a timestamp is written once the GPU has finished everything before it in the list (bottom
 * of pipe), so the time between two consecutive timestamps of one list is charged to the work recorded between
 * them: the tagged item plus any untagged commands since the previous mark (state setting, uninstrumented
 * diagnostics). Consecutive draws overlap on the GPU and a timestamp between them limits that overlap; mode 1
 * therefore marks runs of draws (one mark per pipeline/target change, which is what a pass is) rather than single
 * draws, while barrier, copy, clear and compute boundaries are marked individually in both modes. Mode 2 gives
 * per-draw cost but inflates small draws. A barrier batch's time is from the previous mark to the mark after it:
 * the drain it causes shows up there (or in the first item after it, if the GPU defers the wait). The gap from one
 * list's end timestamp to the next list's begin timestamp is GPU idle on this queue (or time in unprofiled lists:
 * boot_gpu_wait signals, diagnostic probe lists), including list launch overhead. */
#include <stdarg.h>
#include <stdio.h>
#ifndef BOOT_GP_QUERIES
#define BOOT_GP_QUERIES 65536u   /* power of two */
#endif
#define BOOT_GP_MASK (BOOT_GP_QUERIES - 1u)
#define BOOT_GP_LISTS 4          /* command lists followed at once (r->cl, the GPU write-back list) */
#define BOOT_GP_RUNS 32          /* contiguous query runs per list (lists recorded interleaved) */
#define BOOT_GP_SUBS 4096u       /* executed-list ring, power of two */
#define BOOT_GP_FRAMES 4         /* frames waiting for their fence */
#define BOOT_GP_TAB 4096u        /* aggregation entries per print window, power of two */
enum { BOOT_GP_BEGIN, BOOT_GP_TAIL, BOOT_GP_DRAW, BOOT_GP_COMPUTE, BOOT_GP_COPY, BOOT_GP_BARRIER, BOOT_GP_CLEAR, BOOT_GP_PRESENT,
       BOOT_GP_OTHER, BOOT_GP_KINDS };
static const char *const boot_gp_kind_name[BOOT_GP_KINDS] = {"begin", "tail", "draw", "compute", "copy", "barrier", "clear", "present", "other"};
typedef struct {
    const char *file, *cat;
    uint64_t key, info;          /* draws: vs<<32|ps ids, first target WxH/format/count; compute: program id (0: host pass) */
    uint32_t line, n;            /* n: items (draws in the run) */
    uint16_t bt, bu, ba, bf;     /* barrier batch: transitions, UAV, aliasing, full-subresource transitions */
    uint8_t kind, list;
} BootGpMark;
typedef struct {
    ID3D12GraphicsCommandList *cl; int open, ended, bad;
    uint64_t begin_seq, end_seq;
    unsigned nrun; uint64_t run_first[BOOT_GP_RUNS]; uint32_t run_n[BOOT_GP_RUNS];
    int draw_active, next_set; uint32_t draw_count;   /* the pending draw run (closed by the next mark) */
    uint64_t draw_id, draw_key, draw_info, next_id, next_key, next_info;
    const char *draw_file, *next_file; unsigned draw_line, next_line;
} BootGpList;
typedef struct { uint64_t begin, end, fence; uint8_t list; } BootGpSub;
typedef struct { uint64_t lo, hi, fence; int64_t qpc0, qpc1; } BootGpFrame;
typedef struct {
    const char *file, *cat; uint64_t key, info, ticks, n, marks, bt, bu, ba, bf; uint32_t line; uint8_t kind, used;
} BootGpEntry;
static int boot_gp_mode = -1;   /* -1 not decided yet (first list begin), 0 off, 1 draw runs, 2 per draw */
static struct {
    ID3D12Device *device; ID3D12QueryHeap *heap; ID3D12Resource *rb; uint64_t freq; int64_t qpf;
    BootGpMark *marks; uint64_t seq, guard;
    BootGpList lists[BOOT_GP_LISTS];
    BootGpSub subs[BOOT_GP_SUBS]; uint64_t sub_n, cur_lo; int cur_bad;   /* open frame dropped: |1 bad list, |2 ring wrap (counted once, at its present) */
    BootGpFrame frames[BOOT_GP_FRAMES]; unsigned fhead, fcount;
    int64_t last_present, last_print;
    uint64_t last_end, last_sub;   /* end timestamp of the last aggregated list, its index + 1 (0 none) */
    /* print window */
    uint64_t frames_n, drop_wrap, drop_queue, drop_bad, drop_list;
    double frame_ms;
    uint64_t busy, idle, lists_n, marks_n, runs_n, kind_ticks[BOOT_GP_KINDS], kind_n[BOOT_GP_KINDS], batches, bt, bu, ba, bf;
    BootGpEntry *tab; unsigned tab_used; uint64_t tab_full;
} boot_gp;
/* t2-B: draw-path.log lines from the render thread's periodic summaries (boot_gp_log here: ~30 "gpu prof" lines every
 * ~2 s; boot_draw_log in host_gpu_draw_exec.h: ~40 "nvn native" lines every 100 submissions) went through an
 * fopen/fclose per line: 10-20 ms of CreateFile/CloseHandle per summary on the render thread (play-81 profile:
 * ZwCreateFile + ZwClose 2.8% of t2, all under these two). The file is now opened once, for appending, and kept:
 * every line is one WriteFile with FILE_APPEND_DATA (an atomic append at the end of the file, interleaving with the
 * other fopen("a") writers exactly as before), unbuffered (nothing is lost if the process dies), with the bytes
 * fopen's text mode wrote ("\n" -> "\r\n"). If the open fails the line is dropped and the next line tries again,
 * as with a failed fopen. */
static void *volatile boot_draw_path_handle;   /* HANDLE of draw-path.log, NULL until the first line */
static inline void boot_draw_path_write(const char *text, size_t n) {
    HANDLE h = (HANDLE)boot_draw_path_handle;
    if (!h) {
        h = CreateFileA("draw-path.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
        void *was = InterlockedCompareExchangePointer((void *volatile *)&boot_draw_path_handle, (void *)h, NULL);
        if (was) { CloseHandle(h); h = (HANDLE)was; }   /* another thread opened it first */
    }
    char small[2048]; char *out = n * 2 <= sizeof small ? small : (char *)malloc(n * 2);
    if (!out) return;
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) { if (text[i] == '\n') out[m++] = '\r'; out[m++] = text[i]; }
    DWORD done = 0; WriteFile(h, out, (DWORD)m, &done, NULL);
    if (out != small) free(out);
}
/* suspect-diag (host_gpu_render.h, TOTK_SUSPECT_DIAG): the frame-N.bmp number of the frame being built, appended to every line
 * below as " [frame N]" (the glitch.log / flash locator numbering); -1 (the switch off) appends nothing. */
static volatile LONG64 boot_sd_tag_frame = -1;
/* One line: the formatted text and "\n" (as vfprintf + fputc('\n') to an fopen("a") stream did). */
static inline void boot_draw_path_vlog(const char *fmt, va_list ap) {
    char buf[1024]; va_list aq; va_copy(aq, ap);
    int k = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    if (k < 0) { va_end(aq); return; }
    char *line = buf;
    if ((size_t)k >= sizeof buf - 1) {
        line = (char *)malloc((size_t)k + 2);
        if (!line) { va_end(aq); return; }
        vsnprintf(line, (size_t)k + 1, fmt, aq);
    }
    va_end(aq);
    {   const LONG64 sd_frame = boot_sd_tag_frame;   /* suspect-diag: " [frame N]" */
        char *sd_line = sd_frame >= 0 ? (char *)malloc((size_t)k + 40) : NULL;
        if (sd_line) {
            memcpy(sd_line, line, (size_t)k);
            int t = snprintf(sd_line + k, 40, " [frame %lld]\n", (long long)sd_frame);
            if (t <= 0 || t >= 40) { sd_line[k] = '\n'; t = 1; }
            boot_draw_path_write(sd_line, (size_t)k + (size_t)t);
            free(sd_line); if (line != buf) free(line);
            return;
        }
    }
    line[k] = '\n';
    boot_draw_path_write(line, (size_t)k + 1);
    if (line != buf) free(line);
}
static inline void boot_draw_path_logf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); boot_draw_path_vlog(fmt, ap); va_end(ap); }
__attribute__((format(printf, 1, 2))) static inline void boot_gp_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); boot_draw_path_vlog(fmt, ap); va_end(ap);
}
static inline void boot_gp_disable(const char *why) {
    if (boot_gp_mode > 0) boot_gp_log("gpu prof: disabled (%s)", why);
    boot_gp_mode = 0;
}
static inline int boot_gp_init(BootGpu *g) {
    char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_GPU_PROFILE", v, sizeof v);
    int mode = k && k < sizeof v ? (v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0) : 0;
    boot_gp_mode = 0;
    if (!mode || !g || !g->device || !g->queue || !g->fence) return 0;
    LARGE_INTEGER qf; uint64_t freq = 0;
    if (!QueryPerformanceFrequency(&qf) || qf.QuadPart <= 0 || FAILED(ID3D12CommandQueue_GetTimestampFrequency(g->queue, &freq)) || !freq) {
        boot_gp_log("gpu prof: no timestamp frequency, profiler off"); return 0; }
    D3D12_QUERY_HEAP_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = BOOT_GP_QUERIES;
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof bd); bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (uint64_t)BOOT_GP_QUERIES * 8;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    boot_gp.marks = (BootGpMark *)calloc(BOOT_GP_QUERIES, sizeof *boot_gp.marks);
    boot_gp.tab = (BootGpEntry *)calloc(BOOT_GP_TAB, sizeof *boot_gp.tab);
    if (!boot_gp.marks || !boot_gp.tab ||
        FAILED(ID3D12Device_CreateQueryHeap(g->device, &qd, &IID_ID3D12QueryHeap, (void **)&boot_gp.heap)) ||
        FAILED(ID3D12Device_CreateCommittedResource(g->device, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                    &IID_ID3D12Resource, (void **)&boot_gp.rb))) {
        if (boot_gp.heap) ID3D12QueryHeap_Release(boot_gp.heap);
        free(boot_gp.marks); free(boot_gp.tab); memset(&boot_gp, 0, sizeof boot_gp);
        boot_gp_log("gpu prof: query heap / readback creation failed, profiler off"); return 0;
    }
    boot_gp.device = g->device; boot_gp.freq = freq; boot_gp.qpf = qf.QuadPart;
    boot_gp_mode = mode;
    boot_gp_log("gpu prof: on (render-40, TOTK_GPU_PROFILE=%d: %s), %u timestamp queries, %llu ticks/s", mode,
                mode == 1 ? "draw runs" : "every draw", BOOT_GP_QUERIES, (unsigned long long)freq);
    return 1;
}
static inline uint64_t boot_gp_id64(const uint8_t *id) {
    uint64_t v = 0; if (id) for (unsigned i = 0; i < 8; ++i) v = v << 8 | id[i];
    return v;
}
static inline BootGpList *boot_gp_list_of(ID3D12GraphicsCommandList *cl) {
    for (unsigned i = 0; i < BOOT_GP_LISTS; ++i) if (boot_gp.lists[i].cl == cl && boot_gp.lists[i].open) return &boot_gp.lists[i];
    return NULL;
}
static inline uint64_t boot_gp_live(void) { return boot_gp.fcount ? boot_gp.frames[boot_gp.fhead].lo : boot_gp.cur_lo; }
static inline void boot_gp_drop_oldest(uint64_t *counter) {
    boot_gp.fhead = (boot_gp.fhead + 1) % BOOT_GP_FRAMES; --boot_gp.fcount; ++*counter;
}
/* Query or list ring about to overwrite something still needed: drop the oldest waiting frame (or, with none
 * waiting, the open frame and the open lists) until it does not. */
static inline void boot_gp_reclaim(void) {
    for (;;) {
        uint64_t g = boot_gp.seq, live = boot_gp_live();
        if (live < boot_gp.sub_n && boot_gp.subs[live & (BOOT_GP_SUBS - 1)].begin < g) g = boot_gp.subs[live & (BOOT_GP_SUBS - 1)].begin;
        for (unsigned i = 0; i < BOOT_GP_LISTS; ++i) {
            const BootGpList *L = &boot_gp.lists[i];
            if ((L->open || L->ended) && !L->bad && L->begin_seq < g) g = L->begin_seq;
        }
        boot_gp.guard = g;
        if (boot_gp.seq - g < BOOT_GP_QUERIES - 1 && boot_gp.sub_n - live < BOOT_GP_SUBS - 1) return;
        if (boot_gp.fcount) { boot_gp_drop_oldest(&boot_gp.drop_wrap); continue; }
        boot_gp.cur_bad |= 2; boot_gp.cur_lo = boot_gp.sub_n;   /* one frame larger than the rings: counted as a ring wrap */
        for (unsigned i = 0; i < BOOT_GP_LISTS; ++i) if (boot_gp.lists[i].open || boot_gp.lists[i].ended) boot_gp.lists[i].bad = 1;
        boot_gp.guard = boot_gp.seq;
        return;
    }
}
static inline void boot_gp_emit(BootGpList *L, unsigned kind, const char *cat, const char *file, unsigned line, uint64_t key, uint64_t info,
                                uint32_t n, const uint16_t *bar) {
    if (!L || L->bad) return;
    if (boot_gp.seq - boot_gp.guard >= BOOT_GP_QUERIES - 1) { boot_gp_reclaim(); if (L->bad) return; }
    uint64_t seq = boot_gp.seq++; unsigned q = (unsigned)(seq & BOOT_GP_MASK);
    BootGpMark *m = &boot_gp.marks[q];
    m->file = file; m->cat = cat; m->key = key; m->info = info; m->line = line; m->n = n;
    m->bt = bar ? bar[0] : 0; m->bu = bar ? bar[1] : 0; m->ba = bar ? bar[2] : 0; m->bf = bar ? bar[3] : 0;
    m->kind = (uint8_t)kind; m->list = (uint8_t)(L - boot_gp.lists);
    ID3D12GraphicsCommandList_EndQuery(L->cl, boot_gp.heap, D3D12_QUERY_TYPE_TIMESTAMP, q);
    if (L->nrun && q && L->run_first[L->nrun - 1] + L->run_n[L->nrun - 1] == seq) ++L->run_n[L->nrun - 1];
    else if (L->nrun < BOOT_GP_RUNS) { L->run_first[L->nrun] = seq; L->run_n[L->nrun++] = 1; }
    else L->bad = 1;   /* too interleaved with another list: this list (and its frame) is dropped */
}
/* the pending draw run ends here */
static inline void boot_gp_flush(BootGpList *L) {
    if (!L || !L->draw_active) return;
    L->draw_active = 0;
    if (L->draw_count) boot_gp_emit(L, BOOT_GP_DRAW, "draw", L->draw_file, L->draw_line, L->draw_key, L->draw_info, L->draw_count, NULL);
}
/* ---- sites (each guarded by one test of boot_gp_mode) */
static inline void boot_gp_pre_on(ID3D12GraphicsCommandList *cl) { boot_gp_flush(boot_gp_list_of(cl)); }
static inline void boot_gp_post_on(ID3D12GraphicsCommandList *cl, unsigned kind, const char *cat, const char *file, unsigned line, uint64_t key, uint64_t info) {
    boot_gp_emit(boot_gp_list_of(cl), kind, cat, file, line, key, info, 1, NULL);
}
/* before an item whose work is recorded by several calls: ends a pending draw run */
#define BOOT_GP_PRE(cl) do { if (boot_gp_mode > 0) boot_gp_pre_on(cl); } while (0)
/* after it: its mark */
#define BOOT_GP_POST(kind, cat, cl, key, info) do { if (boot_gp_mode > 0) boot_gp_post_on((cl), (kind), (cat), __FILE__, __LINE__, (key), (info)); } while (0)
static inline void boot_gp_rb(unsigned kind, const char *cat, const char *file, unsigned line, ID3D12GraphicsCommandList *cl, UINT n,
                              const D3D12_RESOURCE_BARRIER *b) {
    if (boot_gp_mode <= 0 || !n) { ID3D12GraphicsCommandList_ResourceBarrier(cl, n, b); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_ResourceBarrier(cl, n, b);
    uint16_t c[4] = {0, 0, 0, 0};
    for (UINT i = 0; i < n; ++i) {
        if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { ++c[0]; if (b[i].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) ++c[3]; }
        else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) ++c[1];
        else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) ++c[2];
    }
    boot_gp_emit(L, kind, cat, file, line, 0, 0, 1, c);
}
#define BOOT_GP_RB(cat, cl, n, b) boot_gp_rb(BOOT_GP_BARRIER, (cat), __FILE__, __LINE__, (cl), (n), (b))
#define BOOT_GP_RB_K(kind, cat, cl, n, b) boot_gp_rb((kind), (cat), __FILE__, __LINE__, (cl), (n), (b))
static inline void boot_gp_ctr(unsigned kind, const char *cat, const char *file, unsigned line, ID3D12GraphicsCommandList *cl,
                               const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
    if (boot_gp_mode <= 0) { ID3D12GraphicsCommandList_CopyTextureRegion(cl, dst, x, y, z, src, box); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, dst, x, y, z, src, box);
    boot_gp_emit(L, kind, cat, file, line, 0, 0, 1, NULL);
}
#define BOOT_GP_CTR(cat, cl, dst, x, y, z, src, box) boot_gp_ctr(BOOT_GP_COPY, (cat), __FILE__, __LINE__, (cl), (dst), (x), (y), (z), (src), (box))
#define BOOT_GP_CTR_K(kind, cat, cl, dst, x, y, z, src, box) boot_gp_ctr((kind), (cat), __FILE__, __LINE__, (cl), (dst), (x), (y), (z), (src), (box))
static inline void boot_gp_cbr(const char *cat, const char *file, unsigned line, ID3D12GraphicsCommandList *cl,
                               ID3D12Resource *dst, UINT64 doff, ID3D12Resource *src, UINT64 soff, UINT64 n) {
    if (boot_gp_mode <= 0) { ID3D12GraphicsCommandList_CopyBufferRegion(cl, dst, doff, src, soff, n); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, dst, doff, src, soff, n);
    boot_gp_emit(L, BOOT_GP_COPY, cat, file, line, 0, 0, 1, NULL);
}
#define BOOT_GP_CBR(cat, cl, dst, doff, src, soff, n) boot_gp_cbr((cat), __FILE__, __LINE__, (cl), (dst), (doff), (src), (soff), (n))
static inline void boot_gp_cres(const char *cat, const char *file, unsigned line, ID3D12GraphicsCommandList *cl, ID3D12Resource *dst, ID3D12Resource *src) {
    if (boot_gp_mode <= 0) { ID3D12GraphicsCommandList_CopyResource(cl, dst, src); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_CopyResource(cl, dst, src);
    boot_gp_emit(L, BOOT_GP_COPY, cat, file, line, 0, 0, 1, NULL);
}
#define BOOT_GP_CRES(cat, cl, dst, src) boot_gp_cres((cat), __FILE__, __LINE__, (cl), (dst), (src))
static inline void boot_gp_crtv(const char *cat, const char *file, unsigned line, ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                const FLOAT *color, UINT n, const D3D12_RECT *rects) {
    if (boot_gp_mode <= 0) { ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, color, n, rects); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, color, n, rects);
    boot_gp_emit(L, BOOT_GP_CLEAR, cat, file, line, 0, 0, 1, NULL);
}
#define BOOT_GP_CLEAR_RTV(cat, cl, rtv, color, n, rects) boot_gp_crtv((cat), __FILE__, __LINE__, (cl), (rtv), (color), (n), (rects))
static inline void boot_gp_cdsv(const char *cat, const char *file, unsigned line, ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT n, const D3D12_RECT *rects) {
    if (boot_gp_mode <= 0) { ID3D12GraphicsCommandList_ClearDepthStencilView(cl, dsv, flags, depth, stencil, n, rects); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_ClearDepthStencilView(cl, dsv, flags, depth, stencil, n, rects);
    boot_gp_emit(L, BOOT_GP_CLEAR, cat, file, line, 0, 0, 1, NULL);
}
#define BOOT_GP_CLEAR_DSV(cat, cl, dsv, flags, depth, stencil, n, rects) boot_gp_cdsv((cat), __FILE__, __LINE__, (cl), (dsv), (flags), (depth), (stencil), (n), (rects))
static inline void boot_gp_dispatch(const char *cat, const char *file, unsigned line, uint64_t key, ID3D12GraphicsCommandList *cl, UINT x, UINT y, UINT z) {
    if (boot_gp_mode <= 0) { ID3D12GraphicsCommandList_Dispatch(cl, x, y, z); return; }
    BootGpList *L = boot_gp_list_of(cl); boot_gp_flush(L);
    ID3D12GraphicsCommandList_Dispatch(cl, x, y, z);
    boot_gp_emit(L, BOOT_GP_COMPUTE, cat, file, line, key, 0, 1, NULL);
}
#define BOOT_GP_DISPATCH(cat, key, cl, x, y, z) boot_gp_dispatch((cat), __FILE__, __LINE__, (key), (cl), (x), (y), (z))
/* Draws: BOOT_GP_DRAW_BEGIN before the draw call's predication bracket (`id` names the run: pipeline + target
 * textures), BOOT_GP_DRAW_END after it. A different run ends the pending one first; any other mark ends it too, so a
 * run's time is from the mark before its first draw to the mark after its last. */
static inline void boot_gp_draw_begin(ID3D12GraphicsCommandList *cl, const char *file, unsigned line, uint64_t id, uint64_t key, uint64_t info) {
    BootGpList *L = boot_gp_list_of(cl); if (!L) return;
    if (!(L->draw_active && L->draw_id == id && boot_gp_mode == 1)) boot_gp_flush(L);
    L->next_set = 1; L->next_id = id; L->next_key = key; L->next_info = info; L->next_file = file; L->next_line = line;
}
static inline void boot_gp_draw_end_on(ID3D12GraphicsCommandList *cl) {
    BootGpList *L = boot_gp_list_of(cl); if (!L || !L->next_set) return;
    if (!L->draw_active) {
        L->draw_active = 1; L->draw_count = 0; L->draw_id = L->next_id; L->draw_key = L->next_key; L->draw_info = L->next_info;
        L->draw_file = L->next_file; L->draw_line = L->next_line;
    }
    ++L->draw_count; L->next_set = 0;
    if (boot_gp_mode == 2) boot_gp_flush(L);
}
#define BOOT_GP_DRAW_END(cl) do { if (boot_gp_mode > 0) boot_gp_draw_end_on(cl); } while (0)
/* ---- lists and frames */
static inline void boot_gp_list_begin_on(BootGpu *g, ID3D12GraphicsCommandList *cl, const char *what) {
    if (boot_gp_mode < 0 && !boot_gp_init(g)) return;
    if (boot_gp_mode <= 0) return;
    if (!g || g->device != boot_gp.device) { boot_gp_disable("D3D12 device replaced"); return; }
    BootGpList *L = NULL;
    for (unsigned i = 0; i < BOOT_GP_LISTS && !L; ++i) if (boot_gp.lists[i].cl == cl) L = &boot_gp.lists[i];
    for (unsigned i = 0; i < BOOT_GP_LISTS && !L; ++i) if (!boot_gp.lists[i].cl || (!boot_gp.lists[i].open && !boot_gp.lists[i].ended)) L = &boot_gp.lists[i];
    if (!L) return;   /* more lists than followed at once: this one is not timed (its time shows as idle) */
    memset(L, 0, sizeof *L); L->cl = cl; L->open = 1; L->begin_seq = boot_gp.seq;
    boot_gp_emit(L, BOOT_GP_BEGIN, what, __FILE__, __LINE__, 0, 0, 0, NULL);
}
#define BOOT_GP_LIST_BEGIN(g, cl, what) do { if (boot_gp_mode != 0) boot_gp_list_begin_on((g), (cl), (what)); } while (0)
static inline void boot_gp_list_end_on(ID3D12GraphicsCommandList *cl) {
    BootGpList *L = boot_gp_list_of(cl); if (!L) return;
    boot_gp_flush(L);
    L->end_seq = boot_gp.seq;
    boot_gp_emit(L, BOOT_GP_TAIL, "list tail", __FILE__, __LINE__, 0, 0, 0, NULL);
    if (!L->bad)
        for (unsigned i = 0; i < L->nrun; ++i) {
            unsigned q = (unsigned)(L->run_first[i] & BOOT_GP_MASK);
            ID3D12GraphicsCommandList_ResolveQueryData(cl, boot_gp.heap, D3D12_QUERY_TYPE_TIMESTAMP, q, L->run_n[i], boot_gp.rb, (UINT64)q * 8);
        }
    L->open = 0; L->ended = 1;
}
#define BOOT_GP_LIST_END(cl) do { if (boot_gp_mode > 0) boot_gp_list_end_on(cl); } while (0)
/* after the list's ExecuteCommandLists and the Signal of `fence` */
static inline void boot_gp_list_exec_on(ID3D12GraphicsCommandList *cl, uint64_t fence) {
    BootGpList *L = NULL;
    for (unsigned i = 0; i < BOOT_GP_LISTS && !L; ++i) if (boot_gp.lists[i].cl == cl && boot_gp.lists[i].ended) L = &boot_gp.lists[i];
    if (!L) return;
    L->ended = 0;
    if (L->bad) { boot_gp.cur_bad |= 1; return; }   /* the frame is counted once, at its present */
    if (boot_gp.sub_n - boot_gp_live() >= BOOT_GP_SUBS - 1) { boot_gp_reclaim(); if (L->bad) { boot_gp.cur_bad |= 1; return; } }
    BootGpSub *s = &boot_gp.subs[boot_gp.sub_n++ & (BOOT_GP_SUBS - 1)];
    s->begin = L->begin_seq; s->end = L->end_seq; s->fence = fence; s->list = (uint8_t)(L - boot_gp.lists);
}
#define BOOT_GP_LIST_EXEC(cl, fence) do { if (boot_gp_mode > 0) boot_gp_list_exec_on((cl), (fence)); } while (0)
static inline void boot_gp_add(const BootGpMark *m, uint64_t dt) {
    unsigned kind = m->kind < BOOT_GP_KINDS ? m->kind : BOOT_GP_OTHER;
    boot_gp.kind_ticks[kind] += dt; boot_gp.kind_n[kind] += m->n; ++boot_gp.marks_n;
    if (kind == BOOT_GP_DRAW) ++boot_gp.runs_n;   /* one mark per draw run; only frames actually read count */
    if (kind == BOOT_GP_BARRIER) { ++boot_gp.batches; boot_gp.bt += m->bt; boot_gp.bu += m->bu; boot_gp.ba += m->ba; boot_gp.bf += m->bf; }
    if (kind == BOOT_GP_BEGIN) return;
    uint64_t h = ((uint64_t)(uintptr_t)m->file ^ (uint64_t)m->line * 0x9e3779b97f4a7c15ull ^ (uint64_t)kind << 56 ^ m->key * 0xff51afd7ed558ccdull ^ m->info * 0xc4ceb9fe1a85ec53ull);
    h ^= h >> 29;
    for (unsigned probe = 0; probe < 64; ++probe) {
        BootGpEntry *e = &boot_gp.tab[(h + probe) & (BOOT_GP_TAB - 1)];
        if (!e->used) {
            if (boot_gp.tab_used >= BOOT_GP_TAB / 2) break;
            e->used = 1; e->file = m->file; e->cat = m->cat; e->line = m->line; e->kind = (uint8_t)kind; e->key = m->key; e->info = m->info; ++boot_gp.tab_used;
        } else if (e->file != m->file || e->line != m->line || e->kind != kind || e->key != m->key || e->info != m->info) continue;
        e->ticks += dt; e->n += m->n; ++e->marks; e->bt += m->bt; e->bu += m->bu; e->ba += m->ba; e->bf += m->bf;
        return;
    }
    ++boot_gp.tab_full;
}
/* One completed frame: per executed list, begin..end is busy, end of the previous list to begin of this one idle,
 * and each mark of the list is charged the time since the previous mark of the same list. */
static inline void boot_gp_aggregate(const BootGpFrame *f, const uint64_t *ts) {
    for (uint64_t s = f->lo; s < f->hi; ++s) {   /* plausible timestamps first, else the frame is dropped */
        const BootGpSub *x = &boot_gp.subs[s & (BOOT_GP_SUBS - 1)];
        uint64_t t0 = ts[x->begin & BOOT_GP_MASK], t1 = ts[x->end & BOOT_GP_MASK];
        if (t1 < t0 || t1 - t0 > boot_gp.freq) { ++boot_gp.drop_bad; boot_gp.last_sub = 0; return; }
    }
    for (uint64_t s = f->lo; s < f->hi; ++s) {
        const BootGpSub *x = &boot_gp.subs[s & (BOOT_GP_SUBS - 1)];
        uint64_t t0 = ts[x->begin & BOOT_GP_MASK], t1 = ts[x->end & BOOT_GP_MASK], prev = t0;
        if (boot_gp.last_sub && boot_gp.last_sub == s && t0 >= boot_gp.last_end && t0 - boot_gp.last_end < boot_gp.freq) boot_gp.idle += t0 - boot_gp.last_end;
        boot_gp.busy += t1 - t0; boot_gp.last_end = t1; boot_gp.last_sub = s + 1; ++boot_gp.lists_n;
        boot_gp_add(&boot_gp.marks[x->begin & BOOT_GP_MASK], 0);
        for (uint64_t q = x->begin + 1; q <= x->end; ++q) {
            const BootGpMark *m = &boot_gp.marks[q & BOOT_GP_MASK];
            if (m->list != x->list) continue;
            uint64_t t = ts[q & BOOT_GP_MASK], dt = t >= prev ? t - prev : 0;
            if (t > prev) prev = t;
            boot_gp_add(m, dt);
        }
    }
    ++boot_gp.frames_n; boot_gp.frame_ms += (double)(f->qpc1 - f->qpc0) * 1000.0 / (double)boot_gp.qpf;
}
static inline const char *boot_gp_base(const char *p) {
    const char *b = p ? p : "?";
    for (const char *c = b; *c; ++c) if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}
static inline void boot_gp_print(void) {
    double F = (double)boot_gp.frames_n, ms = 1000.0 / (double)boot_gp.freq / F;
    #define BOOT_GP_MS(t) ((double)(t) * ms)
    boot_gp_log("gpu prof: frames %llu, GPU busy %.3f ms/frame (of %.3f ms frame), idle between lists %.3f ms; by kind: draws %.3f ms (%.1f), "
                "compute %.3f ms (%.1f), copies %.3f ms (%.1f), barriers %.3f ms (%.1f batches, %.1f transitions, %.1f UAV), clears %.3f ms, "
                "present %.3f ms, other %.3f ms",
                (unsigned long long)boot_gp.frames_n, BOOT_GP_MS(boot_gp.busy), boot_gp.frame_ms / F, BOOT_GP_MS(boot_gp.idle),
                BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_DRAW]), (double)boot_gp.kind_n[BOOT_GP_DRAW] / F,
                BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_COMPUTE]), (double)boot_gp.kind_n[BOOT_GP_COMPUTE] / F,
                BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_COPY]), (double)boot_gp.kind_n[BOOT_GP_COPY] / F,
                BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_BARRIER]), (double)boot_gp.batches / F, (double)boot_gp.bt / F, (double)boot_gp.bu / F,
                BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_CLEAR]), BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_PRESENT]),
                BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_OTHER] + boot_gp.kind_ticks[BOOT_GP_TAIL]));
    boot_gp_log("gpu prof: mode %d, %.1f lists/frame, %.0f timestamps/frame, %.1f draw runs/frame, list tails %.3f ms, barriers also %.1f aliasing "
                "and %.1f full-subresource transitions/frame; dropped frames: %llu ring wrap, %llu not complete after %d presents, %llu list/overflow, %llu bad timestamps; %u tags%s",
                boot_gp_mode, (double)boot_gp.lists_n / F, (double)boot_gp.marks_n / F, (double)boot_gp.runs_n / F, BOOT_GP_MS(boot_gp.kind_ticks[BOOT_GP_TAIL]),
                (double)boot_gp.ba / F, (double)boot_gp.bf / F, (unsigned long long)boot_gp.drop_wrap, (unsigned long long)boot_gp.drop_queue, BOOT_GP_FRAMES,
                (unsigned long long)boot_gp.drop_list, (unsigned long long)boot_gp.drop_bad, boot_gp.tab_used, boot_gp.tab_full ? " (table full: some tags only in the totals)" : "");
    unsigned top[15], ntop = 0;
    for (unsigned k = 0; k < 15; ++k) {
        unsigned best = BOOT_GP_TAB;
        for (unsigned i = 0; i < BOOT_GP_TAB; ++i) {
            const BootGpEntry *e = &boot_gp.tab[i];
            if (!e->used || e->ticks == 0) continue;
            int taken = 0; for (unsigned j = 0; j < ntop; ++j) taken |= top[j] == i;
            if (!taken && (best == BOOT_GP_TAB || e->ticks > boot_gp.tab[best].ticks)) best = i;
        }
        if (best == BOOT_GP_TAB) break;
        top[ntop++] = best;
    }
    for (unsigned k = 0; k < ntop; ++k) {
        const BootGpEntry *e = &boot_gp.tab[top[k]]; char detail[160]; detail[0] = 0;
        if (e->kind == BOOT_GP_DRAW)
            snprintf(detail, sizeof detail, " vs %08x ps %08x rt %ux%u fmt %u x%u%s", (unsigned)(e->key >> 32), (unsigned)e->key,
                     (unsigned)(e->info >> 48), (unsigned)(e->info >> 32 & 0xffff), (unsigned)(e->info >> 16 & 0xffff), (unsigned)(e->info >> 8 & 15), (e->info & 1) ? " +depth" : "");
        else if (e->kind == BOOT_GP_COMPUTE && e->key)
            snprintf(detail, sizeof detail, " prog %016llx", (unsigned long long)e->key);
        else if (e->bt || e->bu || e->ba)
            snprintf(detail, sizeof detail, " (%.1f transitions [%.1f full], %.1f UAV, %.1f aliasing /frame)", (double)e->bt / F, (double)e->bf / F, (double)e->bu / F, (double)e->ba / F);
        double per = (double)e->n / F, us = e->n ? (double)e->ticks * 1e6 / (double)boot_gp.freq / (double)e->n : 0;
        boot_gp_log("gpu prof top %2u: %8.3f ms/frame %8.1f/frame %9.2f us each  %-7s %-20s %s:%u%s", k + 1, BOOT_GP_MS(e->ticks), per, us,
                    boot_gp_kind_name[e->kind], e->cat ? e->cat : "?", boot_gp_base(e->file), e->line, detail);
    }
    #undef BOOT_GP_MS
    boot_gp.frames_n = boot_gp.drop_wrap = boot_gp.drop_queue = boot_gp.drop_bad = boot_gp.drop_list = 0; boot_gp.frame_ms = 0;
    boot_gp.busy = boot_gp.idle = boot_gp.lists_n = boot_gp.marks_n = boot_gp.runs_n = boot_gp.batches = boot_gp.bt = boot_gp.bu = boot_gp.ba = boot_gp.bf = 0;
    memset(boot_gp.kind_ticks, 0, sizeof boot_gp.kind_ticks); memset(boot_gp.kind_n, 0, sizeof boot_gp.kind_n);
    memset(boot_gp.tab, 0, (size_t)BOOT_GP_TAB * sizeof *boot_gp.tab); boot_gp.tab_used = 0; boot_gp.tab_full = 0;
}
/* At a present, after its list was executed: closes the frame (present to present) and reads every waiting frame
 * whose last list has completed; prints every ~2 s. */
static inline void boot_gp_frame_on(BootGpu *g) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (boot_gp.last_present) {
        if (boot_gp.cur_bad & 2) ++boot_gp.drop_wrap;
        else if (boot_gp.cur_bad) ++boot_gp.drop_list;
        else if (boot_gp.sub_n > boot_gp.cur_lo) {
            if (boot_gp.fcount == BOOT_GP_FRAMES) boot_gp_drop_oldest(&boot_gp.drop_queue);
            BootGpFrame *f = &boot_gp.frames[(boot_gp.fhead + boot_gp.fcount++) % BOOT_GP_FRAMES];
            f->lo = boot_gp.cur_lo; f->hi = boot_gp.sub_n; f->fence = boot_gp.subs[(boot_gp.sub_n - 1) & (BOOT_GP_SUBS - 1)].fence;
            f->qpc0 = boot_gp.last_present; f->qpc1 = now.QuadPart;
        }
    } else boot_gp.last_print = now.QuadPart;
    boot_gp.cur_lo = boot_gp.sub_n; boot_gp.cur_bad = 0; boot_gp.last_present = now.QuadPart;
    if (!g || !g->fence) return;
    uint64_t done = ID3D12Fence_GetCompletedValue(g->fence);
    if (done == UINT64_MAX) { boot_gp_disable("device removed"); return; }
    if (boot_gp.fcount && boot_gp.frames[boot_gp.fhead].fence <= done) {
        const uint64_t *ts = NULL; D3D12_RANGE rd = {0, (SIZE_T)BOOT_GP_QUERIES * 8}, wr = {0, 0};
        if (FAILED(ID3D12Resource_Map(boot_gp.rb, 0, &rd, (void **)&ts)) || !ts) { boot_gp_disable("readback map failed"); return; }
        while (boot_gp.fcount && boot_gp.frames[boot_gp.fhead].fence <= done) {
            boot_gp_aggregate(&boot_gp.frames[boot_gp.fhead], ts);
            boot_gp.fhead = (boot_gp.fhead + 1) % BOOT_GP_FRAMES; --boot_gp.fcount;
        }
        ID3D12Resource_Unmap(boot_gp.rb, 0, &wr);
    }
    if (boot_gp.frames_n && now.QuadPart - boot_gp.last_print >= 2 * boot_gp.qpf) { boot_gp_print(); boot_gp.last_print = now.QuadPart; }
}
#define BOOT_GP_FRAME(g) do { if (boot_gp_mode > 0) boot_gp_frame_on(g); } while (0)
#endif
