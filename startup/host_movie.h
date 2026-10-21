#ifndef TOTK_HOST_MOVIE_H
#define TOTK_HOST_MOVIE_H
/* Native movie:: (subsdk0 "nnMedia") replacement: the game's pre-rendered cutscene movies (WebM: VP9 video)
 * are demuxed with libwebm (mkvparser) and decoded with libvpx on a host thread. No IPC, no nvdrv, no
 * NVDEC/nvhost device and no GPU command buffer is involved: the game's movie::Decoder normally reaches
 * TVMR/NvMMLite -> NvRmChannelOpen -> NvOsDrvOpen("/dev/nvhost-ctrl"); that whole layer is skipped by
 * replacing the movie:: API objects themselves.
 *
 * Design adapted from LASRecomp runtime/hle/movie.c (MIT; see THIRD-PARTY-NOTICES.md): the game owns
 * every object (vtable at +0) and the host state lives behind +8. Unlike LAS (fake vtable), the object keeps
 * the real subsdk0 vtable; its entries (subsdk0 movie::Decoder / movie::Extractor wrapper functions) are
 * guest-entry hooks (host_fn.h), as are the two constructors and the movie::MediaData functions main imports.
 *
 * What the game does with it (main, static trace 2026-10-20; play-work/movie-work/README.md has the detail):
 *   player open   main+0x240c6c0  Extractor(ContainerType 3=webm) -> SetDataSource(path) -> GetTrackCount
 *                 main+0x240c864  per track GetTrackConfiguration; first "video/" track: Decoder(type 3=vp9,
 *                                 DecoderOutputFormat 4 = NV12 tight, DecoderMode 1 = CPU buffers, prio),
 *                                 RegisterClientEvents({input, output, format-changed, error} EventType*),
 *                                 Configure(track format), FindInt32 width/height, GetOutputBufferSize,
 *                                 SelectTrack. Audio tracks are never selected (the cutscene audio is the
 *                                 game's own stream); the Ganondorf movie has no audio track at all.
 *   worker thread main+0x240d570  WaitAny(input+0x10, output+0x38, format+0x60, exit+0xb0):
 *                                 input  -> AcquireInputBufferIndex loop -> message to the reader thread
 *                                 output -> AcquireOutputBufferIndex loop; flags 4 (EOS): Release + finished
 *                                           (player+0x708 = 1, polled by the cutscene via main+0x240c4e4),
 *                                           else queue {index, time} for the renderer
 *                                 format -> GetOutputFormat: width, height, stride, crop (stride/crop unused)
 *   reader thread main+0x240d480  GetTrackIndexForAvailableData (non-zero = end -> SendInputBuffer EOS on every
 *                                 queued input), GetInputBuffer, Extractor::Read, GetSampleTime,
 *                                 SendInputBufferForDecode(idx, Offset, Size, time, flags), Advance
 *   renderer      main+0x240dc78  one frame per game frame: Buffer.SetDataAndCapacity(gpu pool cpu ptr,
 *                                 w*h*3/2), GetOutputBuffer(idx, Buffer), ReleaseOutputBufferIndex(idx);
 *                                 textures: Y plane w x h at offset 0, interleaved UV w/2 x h/2 at w*h
 *   teardown      main+0x240c22c  Flush+Stop, stop reader, renderer releases its frames (after Stop!),
 *                                 delete renderer, Flush + delete decoder, delete extractor, stop worker.
 *
 * Event signalling. nn::os::SignalEvent can only run as guest code, so events are signalled on the calling
 * guest thread before a decoder method returns, by tail-calling sdk nn::os::SignalEvent through
 * boot_host_call_guest with a continuation (movie.cont) that chains the next event and finally returns the
 * method's status. Decoding runs ahead on a host thread with bounded queues (4 inputs, 6 outputs); a call
 * never returns leaving the game with nothing to do: if the game holds no input to fill, no frame to release
 * and nothing is announced, the call waits (bounded) for the decode thread to make progress first. Stop,
 * Flush and the destructors clear all four events (as LAS's dec_stop) so nothing wakes the worker into a
 * deleted decoder (the game deletes the decoder before stopping its worker thread).
 *
 * Audio: none (user decision 2026-10-20: no movie audio until the frame rate is stable). The game does not
 * route movie audio through movie:: at all (see above), so no silence feed is needed; TOTK_MOVIE_AUDIO=1 only
 * logs audio tracks at the TODO(audio) hook in mv_ext_set_source.
 *
 * Modes (TOTK_MOVIE): native (default) = libwebm + libvpx when the codec shim is linked (weak references,
 * like libopus), otherwise stub; stub = every movie opens and immediately reports end of stream (the cutscene
 * moves on); off = no hooks (the original SDK path, which stops at nvhost-ctrl). A movie that cannot be
 * opened or decoded natively falls back to the stub for that object. Logs: movie.log (every call; rate
 * limited for per-frame calls unless TOTK_MOVIE_LOG=2) and one-line summaries in draw-path.log.
 * Runtime_support only (included under TOTK_DRAW_PATH after host_svc_fs.h, whose RomFS index it uses). */
#include <stdint.h>

/* ------------------------------------------------------------------ codec shim API (plain C) */
/* Implemented by startup/host_movie_vpx.c (libvpx, third_party/build_vpx.py) and startup/host_movie_webm.cpp
 * (libwebm mkvparser, third_party/build_webm.py). The runtime references them weakly: when the archives are
 * not linked every pointer is NULL and the movie code uses the stub. */
#ifdef TOTK_HOST_MOVIE_SHIM
#define TM_WEAK
#else
#define TM_WEAK __attribute__((weak))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*TmReadFn)(void *user, int64_t pos, int32_t len, void *buf);   /* 0 = ok */
typedef struct TmWebmTrack { int32_t number, type, width, height; int64_t default_duration_ns; char codec_id[32]; } TmWebmTrack;
typedef struct TmWebmFrame { int32_t track, key; int64_t time_ns, pos; int32_t size, pad; } TmWebmFrame;
TM_WEAK void *tm_webm_open(TmReadFn read, void *user, int64_t size, int32_t *error);
TM_WEAK int32_t tm_webm_tracks(void *w, TmWebmTrack *out, int32_t cap);   /* type 1 video, 2 audio */
TM_WEAK int64_t tm_webm_duration_ns(void *w);
TM_WEAK int32_t tm_webm_next(void *w, TmWebmFrame *f);                     /* 1 frame, 0 end, <0 error */
TM_WEAK int32_t tm_webm_rewind(void *w, int64_t time_ns);                  /* next frame: cluster at/before time */
TM_WEAK void tm_webm_close(void *w);
typedef struct TmImage { const uint8_t *plane[3]; int32_t stride[3]; int32_t width, height, fmt; } TmImage;   /* fmt 0 = I420 8 bit */
TM_WEAK void *tm_vp9_open(int32_t threads, int32_t *error);
TM_WEAK int32_t tm_vp9_decode(void *d, const uint8_t *data, uint32_t size);  /* size 0: flush; 0 = ok */
TM_WEAK int32_t tm_vp9_frame(void *d, TmImage *img);                       /* 1 frame (valid until the next decode), 0 none */
TM_WEAK void tm_vp9_close(void *d);
TM_WEAK const char *tm_vp9_version(void);
#ifdef __cplusplus
}
#endif
#include <string.h>
#include <stddef.h>
/* I420 -> NV12, tight (stride = width), the layout of DecoderOutputFormat 4 (subsdk0 0x1229a8). Shared with the
 * offline test (play-work/movie-work/tests/test_movie_decode.c). */
static inline uint32_t mv_nv12(uint8_t *dst, int32_t w, int32_t h, const TmImage *img) {
    int32_t cw = w < img->width ? w : img->width, ch = h < img->height ? h : img->height;
    uint8_t *uv = dst + (size_t)w * (size_t)h;
    if (cw < w || ch < h) memset(dst, 0, (size_t)w * (size_t)h), memset(uv, 128, (size_t)(w / 2) * (size_t)(h / 2) * 2);
    for (int32_t y = 0; y < ch; ++y) memcpy(dst + (size_t)y * (size_t)w, img->plane[0] + (size_t)y * (size_t)img->stride[0], (size_t)cw);
    for (int32_t y = 0; y < ch / 2; ++y) {
        const uint8_t *u = img->plane[1] + (size_t)y * (size_t)img->stride[1], *v = img->plane[2] + (size_t)y * (size_t)img->stride[2];
        uint8_t *o = uv + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < cw / 2; ++x) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
    return (uint32_t)((size_t)w * (size_t)h + (size_t)(w / 2) * (size_t)(h / 2) * 2);
}

#ifndef TOTK_HOST_MOVIE_SHIM
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ pinned addresses */
/* subsdk0 (module 2) offsets from subsdk0-dynsym.tsv + the vtable relocations (_ZTVN5movie7DecoderE at
 * 0x5606f0, _ZTVN5movie9ExtractorE at 0x560890: slot k of the vptr (= vtable + 0x10) is entry k + 2). */
#define MV_GOT_DECODER_VT     UINT64_C(0x58d5f0)   /* GOT: &vtable for movie::Decoder (Decoder ctor 0x124538) */
#define MV_GOT_EXTRACTOR_VT   UINT64_C(0x58d610)   /* GOT: &vtable for movie::Extractor (ctor 0x127384) */
#define MV_PLT_DELETE         UINT64_C(0x336380)   /* PLT stub: operator delete(void*) (both D0 destructors) */
#define MV_STR_VP9            UINT64_C(0x4adcd1)   /* "video/x-vnd.on2.vp9" (DecoderImpl::DecoderTypeToMime) */
#define MV_STR_VP8            UINT64_C(0x4b03bb)   /* "video/x-vnd.on2.vp8" */
#define MV_STR_AVC            UINT64_C(0x4b8152)   /* "video/avc" */
#define MV_STR_RAW            UINT64_C(0x4ab824)   /* "video/raw" */
/* sdk (module 3) */
#define MV_SDK_SIGNAL_EVENT   UINT64_C(0x124610)   /* nn::os::SignalEvent(EventType*) */
#define MV_SDK_CLEAR_EVENT    UINT64_C(0x1246b0)   /* nn::os::ClearEvent(EventType*) */
/* movie::Status values (subsdk0 AndroidErrorToNnError 0x1157bc, DecoderImpl) */
#define MV_ST_OK              0u
#define MV_ST_UNKNOWN         0x1000u
#define MV_ST_NOMEM           0x1002u
#define MV_ST_BAD_SIZE        0x1005u   /* -1009 (buffer too small) */
#define MV_ST_BAD_STATE       0x1006u
#define MV_ST_BAD_ARG         0x100bu
#define MV_ST_NO_BUFFER       0x1011u   /* AcquireInput/OutputBufferIndex: list empty */
#define MV_ST_EOS             0x2000u   /* ERROR_END_OF_STREAM (-1011) */
#define MV_FLAG_EOS           4u
/* movie::Buffer layout (subsdk0 0x127e54..0x128078): +0 offset i32, +4 size i32, +8 int32 data, +0xc uint32
 * data, +0x10 owns-memory u8, +0x18 capacity u64, +0x20 int64 data, +0x28 base, +0x30 nvn dev, +0x38 nvn sync */
#define MV_BUF_SIZE           0x40
/* Host handles stored in guest objects; never valid guest addresses (bits 48+ set), so a missed path
 * that dereferences one faults visibly instead of corrupting memory. */
#define MV_TAG_MD             UINT64_C(0x4d44)     /* movie::MediaData +0 */
#define MV_TAG_EXT            UINT64_C(0x4d58)     /* movie::Extractor +8 */
#define MV_TAG_DEC            UINT64_C(0x4d43)     /* movie::Decoder +8 */
#define MV_TAG_IN             UINT64_C(0x4d49)     /* input Buffer base: decoder slot << 8 | input index */
#define MV_NIN                4
#define MV_NOUT               6
#define MV_IN_CAP             (2u << 20)
#define MV_MAX_DEC            8
#define MV_MAX_EXT            8
#define MV_MAX_MD             1024
#define MV_STEPS              32
#define MV_WAIT_MS            4000                 /* bound of one liveness wait (decoder far behind) */

/* ------------------------------------------------------------------ config and logs */
enum { MV_MODE_OFF = 0, MV_MODE_STUB = 1, MV_MODE_NATIVE = 2 };
typedef struct MvConfig { int mode, log, threads, audio, pace, lookahead_ms; } MvConfig;
static inline const MvConfig *mv_config(void) {
    static MvConfig cfg; static INIT_ONCE once = INIT_ONCE_STATIC_INIT; BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        const char *m = getenv("TOTK_MOVIE"), *l = getenv("TOTK_MOVIE_LOG"), *t = getenv("TOTK_MOVIE_THREADS");
        cfg.mode = !m || !*m || !_stricmp(m, "native") ? MV_MODE_NATIVE : !_stricmp(m, "stub") ? MV_MODE_STUB :
                   !_stricmp(m, "off") ? MV_MODE_OFF : MV_MODE_NATIVE;
        cfg.log = l && *l ? atoi(l) : 1;
        cfg.threads = t && *t ? atoi(t) : 2;   /* 2: ~7 ms per 1080p frame measured at below-normal priority beside the game */
        const char *a = getenv("TOTK_MOVIE_AUDIO"); cfg.audio = a && *a == '1';   /* reserved: see the TODO(audio) hook */
        const char *pc = getenv("TOTK_MOVIE_PACE"), *la = getenv("TOTK_MOVIE_LOOKAHEAD_MS");
        cfg.pace = !(pc && !_stricmp(pc, "game"));          /* wall (default): real time; game: one frame per game frame */
        cfg.lookahead_ms = la && *la ? atoi(la) : -1;       /* -1: adaptive, half the game's measured frame interval */
        if (cfg.threads < 1) cfg.threads = 1;
        if (cfg.threads > 8) cfg.threads = 8;
        InitOnceComplete(&once, 0, NULL);
    }
    return &cfg;
}
static inline int mv_native_available(void) {
    return tm_webm_open && tm_webm_tracks && tm_webm_next && tm_webm_rewind && tm_webm_close && tm_webm_duration_ns &&
           tm_vp9_open && tm_vp9_decode && tm_vp9_frame && tm_vp9_close;
}
static inline SRWLOCK *mv_log_lock(void) { static SRWLOCK l = SRWLOCK_INIT; return &l; }
static inline void mv_vlog(const char *file, const char *fmt, va_list ap) {
    FILE *f = fopen(file, "a");
    if (!f) return;
    fprintf(f, "[%10.3f t%05lu] ", (double)GetTickCount64() / 1000.0, (unsigned long)GetCurrentThreadId());
    vfprintf(f, fmt, ap); fputc('\n', f); fclose(f);
}
static inline void mv_log(const char *fmt, ...) {
    if (!mv_config()->log) return;
    va_list ap; va_start(ap, fmt);
    AcquireSRWLockExclusive(mv_log_lock()); mv_vlog("movie.log", fmt, ap); ReleaseSRWLockExclusive(mv_log_lock());
    va_end(ap);
}
/* A summary line in movie.log and draw-path.log (where the render diagnostics already go). */
static inline void mv_note(const char *fmt, ...) {
    va_list ap, aq; va_start(ap, fmt); va_copy(aq, ap);
    AcquireSRWLockExclusive(mv_log_lock());
    mv_vlog("movie.log", fmt, ap);
    FILE *f = fopen("draw-path.log", "a");
    if (f) { fputs("movie: ", f); vfprintf(f, fmt, aq); fputc('\n', f); fclose(f); }
    ReleaseSRWLockExclusive(mv_log_lock());
    va_end(aq); va_end(ap);
}
/* Per-frame calls: the first 48 and then every 256th unless TOTK_MOVIE_LOG=2. */
static inline int mv_sample(volatile LONG *counter) {
    LONG n = InterlockedIncrement(counter);
    return mv_config()->log >= 2 || n <= 48 || (n & 255) == 0;
}

/* ------------------------------------------------------------------ guest helpers */
static inline uint64_t mv_subsdk(const BootContext *c) { return c->code_bases[2]; }
static inline int mv_rd32(BootContext *c, uint64_t self, uint64_t a, uint32_t *v) { return boot_host_read(c, self, a, v, 4); }
static inline int mv_rd64(BootContext *c, uint64_t self, uint64_t a, uint64_t *v) { return boot_host_read(c, self, a, v, 8); }
static inline int mv_wr32(BootContext *c, uint64_t self, uint64_t a, uint32_t v) { return !a || boot_host_write(c, self, a, &v, 4); }
static inline int mv_wr64(BootContext *c, uint64_t self, uint64_t a, uint64_t v) { return !a || boot_host_write(c, self, a, &v, 8); }
static inline int mv_ret(BootContext *c, uint64_t v) { c->x[0] = v; return BOOT_HOST_DONE; }
static inline int mv_stop(const BootContext *c) { return c->status ? BOOT_HOST_STOP : BOOT_HOST_DONE; }
static inline uint64_t mv_handle(uint64_t tag, uint32_t gen, unsigned index) { return tag << 48 | (uint64_t)(gen & 0xffffffu) << 16 | index; }
static inline int mv_handle_split(uint64_t h, uint64_t tag, unsigned cap, uint32_t *gen, unsigned *index) {
    if (h >> 48 != tag || (h & 0xffff) >= cap) return 0;
    *gen = (uint32_t)(h >> 16) & 0xffffffu; *index = (unsigned)(h & 0xffff); return 1;
}
/* A guest string constant from subsdk0 .rodata, verified (0 when it does not read back as expected). */
static inline uint64_t mv_guest_str(BootContext *c, uint64_t off, const char *expect) {
    uint64_t a = mv_subsdk(c) + off; char got[32];
    size_t n = strlen(expect) + 1;
    if (!mv_subsdk(c) || n > sizeof got || !svc_fs_peek(c, a, got, n) || memcmp(got, expect, n)) return 0;
    return a;
}

/* ------------------------------------------------------------------ movie::MediaData (host key/value store) */
enum { MV_MD_I32 = 1, MV_MD_I64, MV_MD_STR, MV_MD_RECT };
typedef struct MvItem { char key[28]; int32_t type; int64_t v[4]; } MvItem;
typedef struct MvMd { uint32_t gen; int32_t used, n; MvItem it[16]; } MvMd;
typedef struct MvMdPool { SRWLOCK lock; uint32_t next; MvMd md[MV_MAX_MD]; } MvMdPool;
static inline MvMdPool *mv_mdpool(void) { static MvMdPool p = {SRWLOCK_INIT, 0, {{0, 0, 0, {{{0}, 0, {0}}}}}}; return &p; }
static inline uint64_t mv_md_alloc(void) {
    MvMdPool *p = mv_mdpool(); uint64_t h = 0;
    AcquireSRWLockExclusive(&p->lock);
    for (unsigned k = 0; k < MV_MAX_MD; ++k) {
        unsigned i = (p->next + k) % MV_MAX_MD;
        if (!p->md[i].used) { MvMd *m = &p->md[i]; m->used = 1; m->n = 0; ++m->gen; h = mv_handle(MV_TAG_MD, m->gen, i); p->next = i + 1; break; }
    }
    ReleaseSRWLockExclusive(&p->lock);
    return h;
}
/* Locked entry for a handle (caller releases with mv_md_unlock); NULL when not ours or stale. */
static inline MvMd *mv_md_lock(uint64_t h) {
    uint32_t gen; unsigned i; MvMdPool *p = mv_mdpool();
    if (!mv_handle_split(h, MV_TAG_MD, MV_MAX_MD, &gen, &i)) return NULL;
    AcquireSRWLockExclusive(&p->lock);
    if (p->md[i].used && p->md[i].gen == gen) return &p->md[i];
    ReleaseSRWLockExclusive(&p->lock); return NULL;
}
static inline void mv_md_unlock(void) { ReleaseSRWLockExclusive(&mv_mdpool()->lock); }
static inline MvItem *mv_md_slot(MvMd *m, const char *key) {
    for (int i = 0; i < m->n; ++i) if (!strcmp(m->it[i].key, key)) return &m->it[i];
    if (m->n >= 16 || strlen(key) >= sizeof m->it[0].key) return NULL;
    MvItem *e = &m->it[m->n++]; memset(e, 0, sizeof *e); strcpy(e->key, key); return e;
}
static inline void mv_md_set(MvMd *m, const char *key, int type, int64_t a, int64_t b, int64_t c, int64_t d) {
    MvItem *e = mv_md_slot(m, key); if (!e) return;
    e->type = type; e->v[0] = a; e->v[1] = b; e->v[2] = c; e->v[3] = d;
}
/* MediaData object at guest `obj`: its handle (0 = unreadable / not ours). */
static inline uint64_t mv_md_handle(BootContext *c, uint64_t self, uint64_t obj) {
    uint64_t h = 0; uint32_t gen; unsigned i;
    if (!obj || !mv_rd64(c, self, obj, &h) || !mv_handle_split(h, MV_TAG_MD, MV_MAX_MD, &gen, &i)) return 0;
    return h;
}

/* ------------------------------------------------------------------ RomFS source */
typedef struct MvSource { HANDLE file; uint64_t base, size; } MvSource;
static inline int mv_source_read(void *user, int64_t pos, int32_t len, void *buf) {
    MvSource *s = (MvSource *)user;
    if (pos < 0 || len < 0 || (uint64_t)pos > s->size || (uint64_t)len > s->size - (uint64_t)pos) return -1;
    return len == 0 || svc_romfs_pread(s->file, s->base + (uint64_t)pos, buf, (uint32_t)len) ? 0 : -1;
}
/* "<mount>:/Event/Movie/x.webm" (any mount; also a bare path) -> the RomFS file's image range and an own
 * read handle. The movie lives in the game's RomFS whatever mount name the resource system used. */
static inline int mv_source_open(BootContext *c, const char *path, MvSource *out) {
    const BootStorage *s = (const BootStorage *)(uintptr_t)c->storage_native;
    char p[0x320]; const char *colon = strstr(path, ":/");
    const char *rest = colon ? colon + 1 : path;
    if (strlen(rest) + 2 >= sizeof p || !s) return 0;
    snprintf(p, sizeof p, "%s%s", rest[0] == '/' ? "" : "/", rest);
    for (char *q = p; *q; ++q) if (*q == '\\') *q = '/';
    SvcRomfs *r = svc_romfs_acquire(c); if (!r) return 0;
    uint32_t entry; uint64_t off = 0, size = 0;
    int ok = svc_romfs_resolve(r, p, &entry) == 1 && svc_romfs_file(r, entry, &off, &size);
    if (!ok) {   /* resource-system prefix we do not know: try the movie directory by file name */
        const char *base = strrchr(p, '/'); char q[0x340];
        snprintf(q, sizeof q, "/Event/Movie/%s", base ? base + 1 : p);
        ok = svc_romfs_resolve(r, q, &entry) == 1 && svc_romfs_file(r, entry, &off, &size);
    }
    svc_romfs_release();
    if (!ok) return 0;
    HANDLE h = ReOpenFile(s->file, GENERIC_READ, FILE_SHARE_READ, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    out->file = h; out->base = off; out->size = size; return 1;
}
static inline void mv_source_close(MvSource *s) { if (s->file && s->file != INVALID_HANDLE_VALUE) CloseHandle(s->file); memset(s, 0, sizeof *s); }

/* Minimal EBML walk (stub mode, no libwebm): PixelWidth/PixelHeight of the first video track. */
static inline int mv_ebml_num(MvSource *s, uint64_t *pos, uint64_t end, uint64_t *v, int strip) {
    uint8_t b[8];
    if (*pos >= end || mv_source_read(s, (int64_t)*pos, 1, b)) return 0;
    int len = 1; while (len <= 8 && !(b[0] & (0x80 >> (len - 1)))) ++len;
    if (len > 8 || *pos + (uint64_t)len > end || (len > 1 && mv_source_read(s, (int64_t)*pos + 1, len - 1, b + 1))) return 0;
    uint64_t x = strip ? (uint64_t)(b[0] & (0xff >> len)) : b[0];
    for (int i = 1; i < len; ++i) x = x << 8 | b[i];
    if (strip && x == (UINT64_C(1) << (7 * len)) - 1) x = UINT64_MAX;   /* unknown size */
    *pos += (uint64_t)len; *v = x; return 1;
}
static inline void mv_ebml_probe(MvSource *s, uint64_t pos, uint64_t end, int depth, int32_t *w, int32_t *h) {
    while (pos < end && depth < 6 && !(*w && *h)) {
        uint64_t id, size;
        if (!mv_ebml_num(s, &pos, end, &id, 0) || !mv_ebml_num(s, &pos, end, &size, 1)) return;
        uint64_t body_end = size == UINT64_MAX || size > end - pos ? end : pos + size;
        if (id == 0x18538067 || id == 0x1654ae6b || id == 0xae || id == 0xe0) mv_ebml_probe(s, pos, body_end, depth + 1, w, h);
        else if ((id == 0xb0 || id == 0xba) && size <= 8) {
            uint8_t b[8] = {0}; uint64_t v = 0;
            if (!mv_source_read(s, (int64_t)pos, (int32_t)size, b)) for (uint64_t i = 0; i < size; ++i) v = v << 8 | b[i];
            if (id == 0xb0) *w = (int32_t)v; else *h = (int32_t)v;
        } else if (id == 0x1f43b675) return;   /* first Cluster: Tracks come before it */
        if (size == UINT64_MAX) return;
        pos = body_end;
    }
}

/* ------------------------------------------------------------------ Extractor */
typedef struct MvTrack { int32_t number, width, height, fps; int64_t duration_us; uint64_t mime_off; const char *mime; } MvTrack;
typedef struct MvExt {
    SRWLOCK lock;                                   /* first: survives the reset in the constructor */
    uint32_t gen; int32_t used, native, container;
    MvSource src; void *webm;
    int32_t ntracks; MvTrack tracks[4];
    uint32_t selected;
    int32_t have_cur, eof, cur_index; TmWebmFrame cur;
    int64_t largest, packets;
    char path[160];
    volatile LONG log_reads;
} MvExt;
typedef struct MvExtTable { SRWLOCK lock; MvExt e[MV_MAX_EXT]; } MvExtTable;
static inline MvExtTable *mv_exts(void) { static MvExtTable t; static INIT_ONCE once = INIT_ONCE_STATIC_INIT; BOOL p = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &p, NULL) && p) { InitializeSRWLock(&t.lock); for (int i = 0; i < MV_MAX_EXT; ++i) InitializeSRWLock(&t.e[i].lock); InitOnceComplete(&once, 0, NULL); }
    return &t; }
static inline MvExt *mv_ext_get(BootContext *c, uint64_t self, uint64_t obj, int *ours) {
    uint64_t h = 0; uint32_t gen; unsigned i; *ours = 0;
    if (!obj || !mv_rd64(c, self, obj + 8, &h) || !mv_handle_split(h, MV_TAG_EXT, MV_MAX_EXT, &gen, &i)) return NULL;
    *ours = 1;
    MvExt *e = &mv_exts()->e[i];
    return e->used && e->gen == gen ? e : NULL;
}
/* Next frame of a selected track (caller holds e->lock). */
static inline void mv_ext_fill(MvExt *e) {
    while (e->native && e->webm && !e->have_cur && !e->eof) {
        int32_t r = tm_webm_next(e->webm, &e->cur);
        if (r <= 0) { e->eof = 1; if (r < 0) mv_log("extractor %s: demux error %d", e->path, r); break; }
        for (int i = 0; i < e->ntracks; ++i)
            if (e->tracks[i].number == e->cur.track && (e->selected >> i & 1)) { e->have_cur = 1; e->cur_index = i; break; }
    }
}

/* ------------------------------------------------------------------ Decoder */
enum { MV_IN_FREE = 0, MV_IN_GAME, MV_IN_QUEUED };
enum { MV_OUT_FREE = 0, MV_OUT_FILLING, MV_OUT_READY, MV_OUT_GAME };
enum { MV_DEC_CREATED = 0, MV_DEC_CONFIGURED, MV_DEC_STARTED, MV_DEC_STOPPED };
typedef struct MvPacket { int32_t idx; uint32_t off, size, flags; int64_t time; } MvPacket;
typedef struct MvOut { uint8_t *mem; uint32_t size, flags; int32_t state; int64_t time; } MvOut;
typedef struct MvDec {
    SRWLOCK lock; CONDITION_VARIABLE cv;            /* first: survive the reset in the constructor */
    uint32_t gen; int32_t used, native, type, fmt, mode, prio, state;
    int32_t width, height;
    uint64_t obj, events[4];
    uint8_t *in_mem[MV_NIN]; uint32_t in_cap; int32_t in_state[MV_NIN];
    MvPacket q[MV_NIN]; int32_t qhead, qcount;
    MvOut out[MV_NOUT]; uint32_t out_cap; int32_t ready[MV_NOUT], nready;
    int32_t input_eos, eos_emitted, eos_delivered, fmt_pending, fmt_signalled, busy, quit;
    int32_t pace, clock_on;                       /* wall-clock pacing (TOTK_MOVIE_PACE) and its media clock */
    int64_t frame_us, lookahead_us, clock_qpc, clock_pts, last_pts, render_qpc, render_ema_us;
    uint32_t flush_gen, last_error;
    HANDLE thread; void *vpx;
    int64_t eos_time;
    uint64_t n_send, n_frames, n_errors, n_out, n_release, n_drop, decode_us;
    volatile LONG log_acq_in, log_send, log_acq_out, log_get_out, log_rel, log_wait;
} MvDec;
typedef struct MvDecTable { SRWLOCK lock; MvDec d[MV_MAX_DEC]; } MvDecTable;
static inline MvDecTable *mv_decs(void) { static MvDecTable t; static INIT_ONCE once = INIT_ONCE_STATIC_INIT; BOOL p = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &p, NULL) && p) {
        InitializeSRWLock(&t.lock);
        for (int i = 0; i < MV_MAX_DEC; ++i) { InitializeSRWLock(&t.d[i].lock); InitializeConditionVariable(&t.d[i].cv); }
        InitOnceComplete(&once, 0, NULL);
    }
    return &t; }
static inline MvDec *mv_dec_get(BootContext *c, uint64_t self, uint64_t obj, int *ours, unsigned *index) {
    uint64_t h = 0; uint32_t gen; unsigned i; *ours = 0;
    if (!obj || !mv_rd64(c, self, obj + 8, &h) || !mv_handle_split(h, MV_TAG_DEC, MV_MAX_DEC, &gen, &i)) return NULL;
    *ours = 1; if (index) *index = i;
    MvDec *d = &mv_decs()->d[i];
    return d->used && d->gen == gen ? d : NULL;
}
static inline int mv_dec_free_inputs(const MvDec *d) { int n = 0; for (int i = 0; i < MV_NIN; ++i) n += d->in_state[i] == MV_IN_FREE; return n; }
static inline int mv_dec_game_inputs(const MvDec *d) { int n = 0; for (int i = 0; i < MV_NIN; ++i) n += d->in_state[i] == MV_IN_GAME; return n; }
static inline int mv_dec_game_outputs(const MvDec *d) { int n = 0; for (int i = 0; i < MV_NOUT; ++i) n += d->out[i].state == MV_OUT_GAME; return n; }
static inline int mv_dec_free_output(const MvDec *d) { for (int i = 0; i < MV_NOUT; ++i) if (d->out[i].state == MV_OUT_FREE) return i; return -1; }
/* ---- wall-clock pacing (TOTK_MOVIE_PACE=wall, default). The game's renderer (main+0x240dc78) shows the oldest
 * frame of its ring once per game frame, so below 30 game fps the movie would play slowly and fall behind. Instead
 * the decoder runs a media clock from the first displayed frame (stream time = its pts + wall time since), hands the
 * game at most one frame at a time (only when the game holds none): the frame nearest to the moment it will be
 * shown, i.e. the renderer's next call predicted from its measured interval (GetOutputBuffer cadence), so the
 * movie stays real time at any game rate; frames that a newer, already-due frame supersedes
 * are dropped, and end of stream is held until the last frame's display time has passed. All under d->lock. */
static inline int64_t mv_qpc_us(void) {
    LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (int64_t)(t.QuadPart / f.QuadPart * 1000000 + t.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}
static inline int64_t mv_dec_media_now(const MvDec *d) { return d->clock_on ? d->clock_pts + (mv_qpc_us() - d->clock_qpc) : INT64_MIN; }
static inline int mv_dec_is_eos(const MvDec *d, int k) { return (d->out[d->ready[k]].flags & MV_FLAG_EOS) != 0; }
static inline void mv_dec_drop_head(MvDec *d) {
    int s = d->ready[0]; memmove(d->ready, d->ready + 1, (size_t)(--d->nready) * sizeof d->ready[0]);
    d->out[s].state = MV_OUT_FREE; ++d->n_drop;
}
/* Media time at which a frame handed out now will be shown: the renderer's next call (last call + its measured
 * interval; it shows one queued frame per game frame), or now + TOTK_MOVIE_LOOKAHEAD_MS when that is set. */
static inline int64_t mv_dec_display_time(const MvDec *d, int64_t now) {
    if (d->lookahead_us >= 0) return now + d->lookahead_us;
    /* game at or above the movie's frame rate: the frame due now (+- half a frame) is shown within a frame */
    if (!d->render_qpc || !d->render_ema_us || d->render_ema_us * 20 < d->frame_us * 21) return now;
    int64_t ema = d->render_ema_us > 200000 ? 200000 : d->render_ema_us;
    int64_t next = d->clock_pts + (d->render_qpc + ema - d->clock_qpc);
    return next > now ? next : now;
}
/* Drop ready frames while the following one is at least as near to the display time (never across EOS). */
static inline void mv_dec_drop_late(MvDec *d, int64_t at) {
    while (d->nready >= 2 && !mv_dec_is_eos(d, 0) && !mv_dec_is_eos(d, 1) &&
           d->out[d->ready[1]].time + d->out[d->ready[0]].time <= 2 * at) mv_dec_drop_head(d);
}
/* Microseconds to wait before the head may be handed out (0 = now): a frame goes out once it is the nearest to the
 * display time (pts - frame/2 <= display time); EOS once the last delivered frame's display time has passed. */
static inline int64_t mv_dec_head_wait(const MvDec *d, int64_t now, int64_t at) {
    if (!d->pace || !d->clock_on || !d->nready) return 0;
    /* slack: the nearest frame (half a frame) when the display time is predicted; 3/4 of a frame when the game runs at
     * or above the movie rate, so game-frame jitter never leaves a game frame without its movie frame */
    int64_t slack = at > now ? d->frame_us / 2 : d->frame_us * 3 / 4;
    int64_t w = mv_dec_is_eos(d, 0) ? d->last_pts + d->frame_us - now : d->out[d->ready[0]].time - slack - at;
    return w > 0 ? (at > now && !mv_dec_is_eos(d, 0) && w > 4000 ? 4000 : w) : 0;   /* a predicted display time moves: re-check */
}
/* An output may be announced: something is ready and (paced) the game holds no frame. */
static inline int mv_dec_output_announceable(const MvDec *d) {
    int n = 0; for (int i = 0; i < MV_NOUT; ++i) n += d->out[i].state == MV_OUT_GAME;
    return d->nready > 0 && (!d->pace || n == 0);
}
/* Queue an output (caller holds the lock). */
static inline void mv_dec_push_ready(MvDec *d, int slot, int64_t time, uint32_t flags, uint32_t size) {
    d->out[slot].state = MV_OUT_READY; d->out[slot].time = time; d->out[slot].flags = flags; d->out[slot].size = size;
    d->ready[d->nready++] = slot;
    if (flags & MV_FLAG_EOS) d->eos_emitted = 1;
    else if (!d->fmt_signalled) d->fmt_pending = 1;
}
/* Stub / end of stream without the thread: an EOS output in any free slot (caller holds the lock). */
static inline void mv_dec_emit_eos_now(MvDec *d, int64_t time) {
    if (d->eos_emitted) return;
    int slot = mv_dec_free_output(d);
    if (slot >= 0) mv_dec_push_ready(d, slot, time, MV_FLAG_EOS, 0);
}
/* Decode thread: packets in queue order -> frames in output slots. */
static inline void mv_dec_emit_frames(MvDec *d, uint32_t gen, int64_t time) {
    TmImage img;
    while (tm_vp9_frame(d->vpx, &img) == 1) {
        AcquireSRWLockExclusive(&d->lock);
        int slot;
        while (!d->quit && d->flush_gen == gen && (slot = mv_dec_free_output(d)) < 0) {
            /* paced: this frame is already due, so the oldest undelivered one is superseded */
            if (d->pace && d->clock_on && d->nready && !mv_dec_is_eos(d, 0) && time <= mv_dec_media_now(d)) { mv_dec_drop_head(d); continue; }
            SleepConditionVariableSRW(&d->cv, &d->lock, d->pace && d->clock_on ? 5 : INFINITE, 0);
        }
        if (d->quit || d->flush_gen != gen) { ReleaseSRWLockExclusive(&d->lock); return; }
        d->out[slot].state = MV_OUT_FILLING;
        ReleaseSRWLockExclusive(&d->lock);
        uint32_t size = img.fmt == 0 ? mv_nv12(d->out[slot].mem, d->width, d->height, &img) : 0;
        AcquireSRWLockExclusive(&d->lock);
        if (d->flush_gen == gen && !d->quit) { mv_dec_push_ready(d, slot, time, 0, size); ++d->n_frames; }
        else d->out[slot].state = MV_OUT_FREE;
        ReleaseSRWLockExclusive(&d->lock);
        WakeAllConditionVariable(&d->cv);
    }
}
static inline DWORD WINAPI mv_dec_thread(void *arg) {
    MvDec *d = (MvDec *)arg;
    AcquireSRWLockExclusive(&d->lock);
    for (;;) {
        while (!d->quit && !d->qcount) SleepConditionVariableSRW(&d->cv, &d->lock, INFINITE, 0);
        if (d->quit) break;
        MvPacket p = d->q[d->qhead]; uint32_t gen = d->flush_gen; d->busy = 1;
        ReleaseSRWLockExclusive(&d->lock);
        if (p.size && p.idx >= 0 && p.idx < MV_NIN && p.off < d->in_cap && p.size <= d->in_cap - p.off) {
            LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
            int32_t r = tm_vp9_decode(d->vpx, d->in_mem[p.idx] + p.off, p.size);
            QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
            d->decode_us += (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
            if (r) { if (d->n_errors++ < 16) mv_log("decoder %p: vp9 decode error %d (packet %u bytes, t=%lld)", (void *)d, r, p.size, (long long)p.time); }
            else mv_dec_emit_frames(d, gen, p.time);
        }
        if (p.flags & MV_FLAG_EOS) {
            if (!tm_vp9_decode(d->vpx, NULL, 0)) mv_dec_emit_frames(d, gen, p.time);
            AcquireSRWLockExclusive(&d->lock);
            int slot;
            while (!d->quit && d->flush_gen == gen && !d->eos_emitted && (slot = mv_dec_free_output(d)) < 0) SleepConditionVariableSRW(&d->cv, &d->lock, INFINITE, 0);
            if (!d->quit && d->flush_gen == gen && !d->eos_emitted) mv_dec_push_ready(d, slot, p.time, MV_FLAG_EOS, 0);
            ReleaseSRWLockExclusive(&d->lock);
        }
        AcquireSRWLockExclusive(&d->lock);
        if (d->flush_gen == gen && d->qcount) {
            if (d->in_state[p.idx] == MV_IN_QUEUED) d->in_state[p.idx] = MV_IN_FREE;
            d->qhead = (d->qhead + 1) % MV_NIN; --d->qcount;
        }
        d->busy = 0;
        if (d->n_frames && d->n_frames % 300 == 0 && p.size)
            mv_log("decoder %p: %llu frames, %.2f ms/packet, ready %d game-held %d, delivered %llu dropped %llu, clock-pts lag %lld us", (void *)d,
                   (unsigned long long)d->n_frames, d->n_send ? (double)d->decode_us / 1000.0 / (double)d->n_send : 0.0, d->nready,
                   mv_dec_game_outputs(d), (unsigned long long)d->n_out, (unsigned long long)d->n_drop,
                   d->clock_on ? (long long)(mv_dec_media_now(d) - d->last_pts) : 0LL);
        WakeAllConditionVariable(&d->cv);
    }
    d->busy = 0;
    ReleaseSRWLockExclusive(&d->lock);
    WakeAllConditionVariable(&d->cv);
    return 0;
}
/* Stops and joins the decode thread; drops queued work (lock not held). */
static inline void mv_dec_join(MvDec *d) {
    HANDLE t;
    AcquireSRWLockExclusive(&d->lock);
    t = d->thread; d->thread = NULL; d->quit = 1; ++d->flush_gen;
    ReleaseSRWLockExclusive(&d->lock);
    WakeAllConditionVariable(&d->cv);
    if (t) { if (WaitForSingleObject(t, 10000) != WAIT_OBJECT_0) mv_note("decoder %p: decode thread did not stop in 10 s", (void *)d); CloseHandle(t); }
    AcquireSRWLockExclusive(&d->lock); d->quit = 0; ReleaseSRWLockExclusive(&d->lock);
}
/* Drops queued inputs and undelivered outputs (Flush / Stop; caller holds the lock). Outputs the game holds
 * stay held until it releases them (the renderer releases its frames after Stop during teardown). */
static inline void mv_dec_drop(MvDec *d) {
    ++d->flush_gen;
    for (int i = 0; i < MV_NIN; ++i) d->in_state[i] = MV_IN_FREE;
    d->qhead = d->qcount = 0;
    for (int i = 0; i < MV_NOUT; ++i) if (d->out[i].state == MV_OUT_READY || d->out[i].state == MV_OUT_FILLING) d->out[i].state = MV_OUT_FREE;
    d->nready = 0; d->input_eos = d->eos_emitted = d->eos_delivered = 0; d->fmt_pending = 0; d->clock_on = 0;
}
static inline void mv_dec_release_resources(MvDec *d) {
    if (d->vpx) { tm_vp9_close(d->vpx); d->vpx = NULL; }
    for (int i = 0; i < MV_NIN; ++i) { free(d->in_mem[i]); d->in_mem[i] = NULL; }
    for (int i = 0; i < MV_NOUT; ++i) { free(d->out[i].mem); d->out[i].mem = NULL; }
}
/* Liveness: after a call the game must have something to do: an input it holds (the reader will send it),
 * a frame it holds (the renderer will release it), or an announced item. Otherwise wait for the decode
 * thread (caller holds the lock; native mode only). */
static inline void mv_dec_wait_progress(MvDec *d, const char *who) {
    if (!d->native || !d->thread || d->state != MV_DEC_STARTED) return;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        int idle_game = !mv_dec_game_inputs(d) && !mv_dec_game_outputs(d);
        int announce = mv_dec_output_announceable(d) || (!d->input_eos && mv_dec_free_inputs(d) > 0);
        if (!idle_game || announce || d->eos_delivered || d->state != MV_DEC_STARTED || d->quit) return;
        if (!d->busy && !d->qcount) return;   /* nothing in flight: nothing will come (shouldn't happen) */
        ULONGLONG now = GetTickCount64();
        if (now - start >= MV_WAIT_MS) { mv_note("decoder %p: %s waited %llu ms without progress (queued %d busy %d)", (void *)d, who, (unsigned long long)(now - start), d->qcount, d->busy); return; }
        SleepConditionVariableSRW(&d->cv, &d->lock, (DWORD)(MV_WAIT_MS - (now - start)), 0);
    }
}
/* Events to announce now: bit 0 input available, 1 output available, 2 format changed (lock held). */
static inline unsigned mv_dec_announce(MvDec *d) {
    unsigned m = 0;
    if (d->state != MV_DEC_STARTED) return 0;
    if (!d->input_eos && mv_dec_free_inputs(d) > 0) m |= 1;
    if (mv_dec_output_announceable(d)) m |= 2;
    if (d->fmt_pending) { m |= 4; d->fmt_pending = 0; d->fmt_signalled = 1; }
    return m;
}

/* ------------------------------------------------------------------ event chains (guest nn::os calls) */
/* s0: bits 0-3 events to signal, 4-7 events to clear (done first), 8-11 final action, 12-15 decoder slot,
 * 16-63 decoder generation. s1: the status to return (final 0) or the object to delete (final 1). */
enum { MV_FINAL_RETURN = 0, MV_FINAL_DELETE = 1 };
static inline uint64_t mv_cont_address(void) { return boot_host_fn_address("movie.cont"); }
static inline int mv_chain(BootContext *c, uint64_t self, uint64_t *target, uint64_t s0, uint64_t s1) {
    unsigned slot = (unsigned)(s0 >> 12) & 15; uint32_t gen = (uint32_t)(s0 >> 16);
    MvDec *d = slot < MV_MAX_DEC ? &mv_decs()->d[slot] : NULL;
    for (;;) {
        unsigned clear = (unsigned)(s0 >> 4) & 15, signal = (unsigned)s0 & 15, bit, is_clear = clear != 0;
        unsigned mask = is_clear ? clear : signal;
        if (!mask) break;
        bit = 0; while (!(mask >> bit & 1)) ++bit;
        s0 &= ~(UINT64_C(1) << (bit + (is_clear ? 4 : 0)));
        uint64_t ev = d && (d->gen & 0xffffffffu) == gen && bit < 4 ? d->events[bit] : 0;
        if (!ev) continue;
        c->x[0] = ev;
        uint64_t fn = c->code_bases[3] + (is_clear ? MV_SDK_CLEAR_EVENT : MV_SDK_SIGNAL_EVENT);
        return boot_host_call_guest(c, self, target, fn, mv_cont_address(), s0, s1);
    }
    c->x[0] = s1;
    if (((s0 >> 8) & 15) == MV_FINAL_DELETE) *target = mv_subsdk(c) + MV_PLT_DELETE;   /* returns to the caller (x30) */
    return BOOT_HOST_DONE;
}
static inline int mv_cont(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; uint64_t s0, s1;
    if (!boot_host_frame_pop(c, self, target, &s0, &s1)) return BOOT_HOST_STOP;
    return mv_chain(c, self, target, s0, s1);
}
static inline int mv_finish(BootContext *c, uint64_t self, uint64_t *target, unsigned slot, const MvDec *d,
                            unsigned signal, unsigned clear, unsigned final, uint64_t value) {
    uint64_t s0 = (uint64_t)(signal & 15) | (uint64_t)(clear & 15) << 4 | (uint64_t)(final & 15) << 8 |
                  (uint64_t)(slot & 15) << 12 | (uint64_t)(d ? d->gen : 0) << 16;
    return mv_chain(c, self, target, s0, value);
}

/* ------------------------------------------------------------------ hooks: constructors */
/* Object vptr = *(GOT entry) + 0x10; also checks that the vtable's slots are the hooked subsdk0 functions. */
static inline uint64_t mv_vptr(BootContext *c, uint64_t self, uint64_t got, const uint64_t *slots, unsigned n, const char *what) {
    uint64_t vt = 0, base = mv_subsdk(c);
    if (!base || !svc_fs_peek_u64(c, base + got, &vt) || !vt) return 0;
    static volatile LONG checked[2];
    volatile LONG *flag = &checked[got == MV_GOT_DECODER_VT ? 0 : 1];
    if (!InterlockedExchange(flag, 1))
        for (unsigned k = 0; k < n; ++k) {
            uint64_t e = 0;
            if (slots[k] && (!svc_fs_peek_u64(c, vt + 0x10 + 8 * k, &e) || e != base + slots[k]))
                mv_note("%s vtable slot %u = %llx, expected subsdk0+%llx: hook may be missed", what, k, (unsigned long long)e, (unsigned long long)slots[k]);
        }
    (void)self;
    return vt + 0x10;
}

/* movie::Extractor::Extractor(ContainerType w1[, CacheSize w2]) */
static inline int mv_ext_ctor(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    static const uint64_t mv_ext_slots[16] = {0, 0x127438, 0x127480, 0x127adc, 0x127b30, 0x127b50, 0x127b6c, 0x127b88, 0x127ba4,
        0x127bfc, 0x127be0, 0x127bc4, 0x127c18, 0x127c34, 0x127c50, 0x127a88};   /* slot 0 (D2) is not exported */
    uint64_t obj = c->x[0]; uint32_t container = (uint32_t)c->x[1];
    uint64_t vptr = mv_vptr(c, self, MV_GOT_EXTRACTOR_VT, mv_ext_slots, 16, "Extractor");
    if (!obj || !vptr) { mv_note("Extractor ctor: no object/vtable (obj %llx): original path", (unsigned long long)obj); return BOOT_HOST_DECLINE; }
    MvExtTable *t = mv_exts(); MvExt *e = NULL; unsigned index = 0;
    AcquireSRWLockExclusive(&t->lock);
    for (unsigned i = 0; i < MV_MAX_EXT; ++i) if (!t->e[i].used) { e = &t->e[i]; index = i; e->used = 1; ++e->gen; break; }
    ReleaseSRWLockExclusive(&t->lock);
    if (!e) { mv_note("Extractor ctor: table full"); return BOOT_HOST_DECLINE; }
    AcquireSRWLockExclusive(&e->lock);
    uint32_t gen = e->gen;
    memset((char *)e + offsetof(MvExt, gen), 0, sizeof *e - offsetof(MvExt, gen));
    e->gen = gen; e->used = 1; e->container = (int32_t)container;
    ReleaseSRWLockExclusive(&e->lock);
    uint64_t words[2] = {vptr, mv_handle(MV_TAG_EXT, gen, index)};
    if (!boot_host_write(c, self, obj, words, 16)) { e->used = 0; return BOOT_HOST_STOP; }
    mv_log("Extractor(%u) obj %llx -> slot %u (lr %llx)", container, (unsigned long long)obj, index, (unsigned long long)c->x[30]);
    return BOOT_HOST_DONE;
}
/* movie::Decoder::Decoder(DecoderType w1, DecoderOutputFormat w2, DecoderMode w3, int prio w4) */
static inline int mv_dec_ctor(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    static const uint64_t mv_dec_slots[19] = {0x1245a4, 0x1245ec, 0x124634, 0x124648, 0x1246d4, 0x12465c, 0x124670, 0x124684, 0x124698,
        0x124724, 0x124738, 0x1246ac, 0x1246e8, 0x124710, 0x1246fc, 0x1246c0, 0x12474c, 0x124760, 0x124774};
    uint64_t obj = c->x[0];
    uint64_t vptr = mv_vptr(c, self, MV_GOT_DECODER_VT, mv_dec_slots, 19, "Decoder");
    if (!obj || !vptr) { mv_note("Decoder ctor: no object/vtable (obj %llx): original path", (unsigned long long)obj); return BOOT_HOST_DECLINE; }
    MvDecTable *t = mv_decs(); MvDec *d = NULL; unsigned index = 0;
    AcquireSRWLockExclusive(&t->lock);
    for (unsigned i = 0; i < MV_MAX_DEC; ++i) if (!t->d[i].used) { d = &t->d[i]; index = i; d->used = 1; break; }
    ReleaseSRWLockExclusive(&t->lock);
    if (!d) { mv_note("Decoder ctor: table full"); return BOOT_HOST_DECLINE; }
    AcquireSRWLockExclusive(&d->lock);
    uint32_t gen = (d->gen + 1) & 0xffffffu;
    memset((char *)d + offsetof(MvDec, gen), 0, sizeof *d - offsetof(MvDec, gen));
    d->gen = gen; d->used = 1;
    d->type = (int32_t)c->x[1]; d->fmt = (int32_t)c->x[2]; d->mode = (int32_t)c->x[3]; d->prio = (int32_t)c->x[4]; d->obj = obj;
    d->native = mv_config()->mode == MV_MODE_NATIVE && mv_native_available() && d->type == 3 && d->fmt == 4 && d->mode == 1;
    ReleaseSRWLockExclusive(&d->lock);
    uint64_t words[2] = {vptr, mv_handle(MV_TAG_DEC, gen, index)};
    if (!boot_host_write(c, self, obj, words, 16)) { d->used = 0; return BOOT_HOST_STOP; }
    mv_note("Decoder(type %d, format %d, mode %d, prio %d) obj %llx -> slot %u, %s", d->type, d->fmt, d->mode, d->prio,
            (unsigned long long)obj, index, d->native ? "native vp9" : "stub (end of stream)");
    return BOOT_HOST_DONE;
}

/* ------------------------------------------------------------------ hooks: Extractor methods */
#define MV_EXT_PROLOGUE(name) \
    int ours_; MvExt *e = mv_ext_get(c, self, c->x[0], &ours_); \
    if (!ours_) return c->status ? BOOT_HOST_STOP : BOOT_HOST_DECLINE; \
    if (!e) { mv_log(name ": stale extractor %llx", (unsigned long long)c->x[0]); return mv_ret(c, MV_ST_BAD_STATE); }

/* SetDataSource(const char *path x1) */
static inline int mv_ext_set_source(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("SetDataSource")
    char path[0x300] = {0};
    if (!c->x[1] || !boot_host_read_string(c, self, c->x[1], path, sizeof path)) return c->status ? BOOT_HOST_STOP : mv_ret(c, MV_ST_BAD_ARG);
    AcquireSRWLockExclusive(&e->lock);
    snprintf(e->path, sizeof e->path, "%s", path);
    uint32_t status = MV_ST_OK; int32_t err = 0;
    if (!mv_source_open(c, path, &e->src)) {
        /* Still open as a stub (one 1920x1080 VP9 track, immediate end of stream): an open failure would leave
         * the cutscene's movie state to game code paths nobody has traced; end of stream is the normal exit. */
        MvTrack *t = &e->tracks[e->ntracks++];
        t->number = 1; t->fps = 30; t->width = 1920; t->height = 1080; t->mime = "video/x-vnd.on2.vp9"; t->mime_off = MV_STR_VP9;
        e->eof = 1;
        mv_note("SetDataSource(%s): not found in RomFS: stub track 1920x1080, end of stream", path);
    } else {
        e->native = mv_config()->mode == MV_MODE_NATIVE && mv_native_available() && (e->container == 2 || e->container == 3);
        if (e->native && !(e->webm = tm_webm_open(mv_source_read, &e->src, (int64_t)e->src.size, &err))) {
            mv_note("SetDataSource(%s): libwebm open failed (%d): stub", path, err); e->native = 0;
        }
        if (e->native) {
            TmWebmTrack tr[8]; int32_t n = tm_webm_tracks(e->webm, tr, 8);
            int64_t dur = tm_webm_duration_ns(e->webm) / 1000;
            for (int32_t i = 0; i < n && e->ntracks < 4; ++i) {
                if (tr[i].type == 2 && mv_config()->audio) {
                    /* TODO(audio) [TOTK_MOVIE_AUDIO=1]: no audio until the frame rate is stable (user, 2026-10-20).
                     * The game never selects a movie audio track (main+0x240c864 only takes "video/"; the cutscene
                     * sound is the game's own stream) and the Ganondorf movie has none, so nothing waits on audio:
                     * timing and end of stream come from the video track alone. When movie audio is wanted, expose
                     * the track here (mime "audio/opus" subsdk0+0x4abf62), decode Opus with the vendored libopus
                     * (third_party/build_opus.py) on the decode thread and mix it like host_audren voices. */
                    mv_note("SetDataSource(%s): audio track %d (%s) present; audio decode not implemented (TODO)", path, tr[i].number, tr[i].codec_id);
                }
                if (tr[i].type != 1) continue;   /* video only: the game never decodes movie audio */
                MvTrack *t = &e->tracks[e->ntracks++];
                t->number = tr[i].number; t->width = tr[i].width; t->height = tr[i].height; t->duration_us = dur;
                t->fps = tr[i].default_duration_ns > 0 ? (int32_t)((INT64_C(1000000000) + tr[i].default_duration_ns / 2) / tr[i].default_duration_ns) : 30;
                if (!strcmp(tr[i].codec_id, "V_VP9")) { t->mime = "video/x-vnd.on2.vp9"; t->mime_off = MV_STR_VP9; }
                else if (!strcmp(tr[i].codec_id, "V_VP8")) { t->mime = "video/x-vnd.on2.vp8"; t->mime_off = MV_STR_VP8; }
                else if (!strcmp(tr[i].codec_id, "V_MPEG4/ISO/AVC")) { t->mime = "video/avc"; t->mime_off = MV_STR_AVC; }
            }
        } else {   /* stub: one VP9 track with the file's own size (the renderer allocates w*h*3/2) */
            MvTrack *t = &e->tracks[e->ntracks++];
            t->number = 1; t->fps = 30; t->mime = "video/x-vnd.on2.vp9"; t->mime_off = MV_STR_VP9;
            mv_ebml_probe(&e->src, 0, e->src.size, 0, &t->width, &t->height);
            if (t->width <= 0 || t->height <= 0 || t->width > 8192 || t->height > 8192) t->width = 1920, t->height = 1080;
            e->eof = 1;
        }
        mv_note("SetDataSource(%s): RomFS image +%llx, %llu bytes, %s, %d video track(s) %dx%d", path, (unsigned long long)e->src.base,
                (unsigned long long)e->src.size, e->native ? "libwebm" : "stub", e->ntracks, e->ntracks ? e->tracks[0].width : 0, e->ntracks ? e->tracks[0].height : 0);
    }
    ReleaseSRWLockExclusive(&e->lock);
    return mv_ret(c, status);
}
/* SetDataSource(const void *x1, int w2) / SetDataSource(ClientStreamReader*, const char*): not used by TOTK */
static inline int mv_ext_set_source_other(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; MV_EXT_PROLOGUE("SetDataSource(other)")
    mv_note("Extractor::SetDataSource variant %d (%llx, %llx) not supported", (int)(uintptr_t)user, (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    return mv_ret(c, MV_ST_UNKNOWN);
}
/* GetTrackCount(int *x1) */
static inline int mv_ext_track_count(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("GetTrackCount")
    if (!c->x[1]) return mv_ret(c, MV_ST_BAD_ARG);
    if (!mv_wr32(c, self, c->x[1], (uint32_t)e->ntracks)) return BOOT_HOST_STOP;
    mv_log("GetTrackCount -> %d", e->ntracks);
    return mv_ret(c, MV_ST_OK);
}
/* GetTrackConfiguration(int w1, MediaData *x2) */
static inline int mv_ext_track_config(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("GetTrackConfiguration")
    int32_t i = (int32_t)c->x[1]; uint64_t h = mv_md_handle(c, self, c->x[2]);
    if (c->status) return BOOT_HOST_STOP;
    if (i < 0 || i >= e->ntracks || !h) { mv_log("GetTrackConfiguration(%d, md %llx): bad argument", i, (unsigned long long)c->x[2]); return mv_ret(c, MV_ST_BAD_ARG); }
    const MvTrack *t = &e->tracks[i];
    uint64_t mime = t->mime ? mv_guest_str(c, t->mime_off, t->mime) : 0;
    MvMd *m = mv_md_lock(h);
    if (!m) return mv_ret(c, MV_ST_BAD_ARG);
    m->n = 0;
    if (mime) mv_md_set(m, "mime", MV_MD_STR, (int64_t)mime, 0, 0, 0);
    mv_md_set(m, "width", MV_MD_I32, t->width, 0, 0, 0);
    mv_md_set(m, "height", MV_MD_I32, t->height, 0, 0, 0);
    mv_md_set(m, "frame-rate", MV_MD_I32, t->fps, 0, 0, 0);
    mv_md_set(m, "max-input-size", MV_MD_I32, MV_IN_CAP, 0, 0, 0);
    mv_md_set(m, "durationUs", MV_MD_I64, t->duration_us, 0, 0, 0);
    mv_md_unlock();
    mv_log("GetTrackConfiguration(%d) -> %s %dx%d %d fps %lld us (mime %llx)", i, t->mime ? t->mime : "?", t->width, t->height, t->fps,
           (long long)t->duration_us, (unsigned long long)mime);
    return mv_ret(c, MV_ST_OK);
}
/* SelectTrack(int w1) / UnselectTrack(int w1) (user = 1 select, 0 unselect) */
static inline int mv_ext_select(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; MV_EXT_PROLOGUE("SelectTrack")
    int32_t i = (int32_t)c->x[1];
    if (i < 0 || i >= e->ntracks) return mv_ret(c, MV_ST_BAD_ARG);
    AcquireSRWLockExclusive(&e->lock);
    if (user) e->selected |= 1u << i; else e->selected &= ~(1u << i);
    if (e->have_cur && !(e->selected >> e->cur_index & 1)) e->have_cur = 0;
    ReleaseSRWLockExclusive(&e->lock);
    mv_log("%s(%d)", user ? "SelectTrack" : "UnselectTrack", i);
    return mv_ret(c, MV_ST_OK);
}
/* SeekTo(long us x1) */
static inline int mv_ext_seek(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("SeekTo")
    int64_t us = (int64_t)c->x[1]; uint32_t st = MV_ST_OK;
    AcquireSRWLockExclusive(&e->lock);
    if (e->native && e->webm) { e->have_cur = 0; e->eof = 0; if (tm_webm_rewind(e->webm, us * 1000) < 0) st = MV_ST_UNKNOWN; }
    ReleaseSRWLockExclusive(&e->lock);
    mv_log("SeekTo(%lld us) -> %x", (long long)us, st);
    return mv_ret(c, st);
}
/* GetTrackIndexForAvailableData(size_t *x1): 0 + track of the next sample, MV_ST_EOS at the end */
static inline int mv_ext_next_track(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("GetTrackIndexForAvailableData")
    AcquireSRWLockExclusive(&e->lock);
    mv_ext_fill(e);
    int have = e->have_cur; int64_t index = e->cur_index;
    ReleaseSRWLockExclusive(&e->lock);
    if (!have) { if (mv_sample(&e->log_reads)) mv_log("GetTrackIndexForAvailableData -> end of stream (%lld packets)", (long long)e->packets); return mv_ret(c, MV_ST_EOS); }
    if (c->x[1] && !mv_wr64(c, self, c->x[1], (uint64_t)index)) return BOOT_HOST_STOP;
    return mv_ret(c, c->x[1] ? MV_ST_OK : MV_ST_BAD_ARG);
}
/* Input buffer host memory for a fake base (decoder input slot), or NULL. */
static inline uint8_t *mv_input_mem(uint64_t base, uint32_t *cap) {
    if (base >> 48 != MV_TAG_IN) return NULL;
    unsigned slot = (unsigned)(base >> 8) & 0xff, idx = (unsigned)base & 0xff;
    if (slot >= MV_MAX_DEC || idx >= MV_NIN) return NULL;
    MvDec *d = &mv_decs()->d[slot];
    if (!d->used || !d->in_mem[idx]) return NULL;
    *cap = d->in_cap; return d->in_mem[idx];
}
/* Read(Buffer *x1): the current sample at base + 0, Buffer range (0, n) (NnExtractor::ReadSampleData) */
static inline int mv_ext_read(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("Read")
    uint8_t b[MV_BUF_SIZE]; uint64_t buf = c->x[1];
    if (!buf || !boot_host_read(c, self, buf, b, sizeof b)) return c->status ? BOOT_HOST_STOP : mv_ret(c, MV_ST_BAD_ARG);
    uint64_t base, cap; memcpy(&base, b + 0x28, 8); memcpy(&cap, b + 0x18, 8);
    AcquireSRWLockExclusive(&e->lock);
    mv_ext_fill(e);
    if (!e->have_cur) { ReleaseSRWLockExclusive(&e->lock); return mv_ret(c, MV_ST_EOS); }
    TmWebmFrame f = e->cur; uint32_t hcap = 0; uint8_t *host = mv_input_mem(base, &hcap);
    if (host) cap = hcap;
    uint32_t n = (uint32_t)f.size;
    if ((uint64_t)n > cap) { mv_note("Read: packet of %u bytes > buffer capacity %llu: truncated", n, (unsigned long long)cap); n = (uint32_t)cap; }
    int ok = 1;
    if (host) ok = !mv_source_read(&e->src, f.pos, (int32_t)n, host);
    else if (base && n) {
        uint8_t *tmp = (uint8_t *)malloc(n);
        ok = tmp && !mv_source_read(&e->src, f.pos, (int32_t)n, tmp);
        if (ok && !boot_host_write(c, self, base, tmp, n)) ok = -1;
        free(tmp);
    } else if (n) ok = 0;
    if ((int64_t)f.size > e->largest) e->largest = f.size;
    ++e->packets;
    ReleaseSRWLockExclusive(&e->lock);
    if (ok < 0) return BOOT_HOST_STOP;
    if (!ok) { mv_note("Read: source read failed at %lld (%u bytes)", (long long)f.pos, n); return mv_ret(c, MV_ST_UNKNOWN); }
    int32_t range[4] = {0, (int32_t)n, 0, 0};   /* offset, size, int32 data, uint32 data */
    uint64_t zero = 0;
    if (!boot_host_write(c, self, buf, range, 16) || !boot_host_write(c, self, buf + 0x20, &zero, 8)) return BOOT_HOST_STOP;
    if (mv_sample(&e->log_reads)) mv_log("Read -> track %d, %u bytes, t=%lld us, key %d", f.track, n, (long long)(f.time_ns / 1000), f.key);
    return mv_ret(c, MV_ST_OK);
}
/* Advance() */
static inline int mv_ext_advance(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("Advance")
    AcquireSRWLockExclusive(&e->lock); e->have_cur = 0; ReleaseSRWLockExclusive(&e->lock);
    return mv_ret(c, MV_ST_OK);
}
/* GetSampleTime(long *us x1) */
static inline int mv_ext_sample_time(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_EXT_PROLOGUE("GetSampleTime")
    AcquireSRWLockExclusive(&e->lock);
    mv_ext_fill(e);
    int have = e->have_cur; int64_t us = e->cur.time_ns / 1000;
    ReleaseSRWLockExclusive(&e->lock);
    if (!have) return mv_ret(c, MV_ST_EOS);
    if (!c->x[1]) return mv_ret(c, MV_ST_BAD_ARG);
    if (!mv_wr64(c, self, c->x[1], (uint64_t)us)) return BOOT_HOST_STOP;
    return mv_ret(c, MV_ST_OK);
}
/* GetSampleMetaData(MediaData*) / GetBufferedRange(long*, long*) */
static inline int mv_ext_meta(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; MV_EXT_PROLOGUE("GetSampleMetaData")
    if (user) {   /* GetBufferedRange: a local file is fully buffered */
        int64_t dur = e->ntracks ? e->tracks[0].duration_us : 0;
        if (!mv_wr64(c, self, c->x[1], 0) || !mv_wr64(c, self, c->x[2], (uint64_t)dur)) return BOOT_HOST_STOP;
        return mv_ret(c, MV_ST_OK);
    }
    uint64_t h = mv_md_handle(c, self, c->x[1]);
    if (c->status) return BOOT_HOST_STOP;
    MvMd *m = h ? mv_md_lock(h) : NULL;
    if (!m) return mv_ret(c, MV_ST_BAD_ARG);
    m->n = 0;
    AcquireSRWLockExclusive(&e->lock);
    if (e->have_cur) { mv_md_set(m, "timeUs", MV_MD_I64, e->cur.time_ns / 1000, 0, 0, 0); mv_md_set(m, "is-sync-frame", MV_MD_I32, e->cur.key, 0, 0, 0); }
    ReleaseSRWLockExclusive(&e->lock);
    mv_md_unlock();
    return mv_ret(c, MV_ST_OK);
}
static inline void mv_ext_destroy(MvExt *e) {
    AcquireSRWLockExclusive(&e->lock);
    mv_note("Extractor %s closed: %lld packets, largest %lld bytes", e->path, (long long)e->packets, (long long)e->largest);
    if (e->webm) tm_webm_close(e->webm);
    e->webm = NULL; mv_source_close(&e->src);
    e->used = 0; ++e->gen;
    ReleaseSRWLockExclusive(&e->lock);
}
/* ~Extractor: D2 (user 0) returns, D0 (user 1) frees the object with operator delete (subsdk0 PLT). The real
 * destructors would call NnMediaCommon::Finalize, unbalanced here (our constructor never initialized it). */
static inline int mv_ext_dtor(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    int ours; MvExt *e = mv_ext_get(c, self, c->x[0], &ours);
    if (!ours) return c->status ? BOOT_HOST_STOP : BOOT_HOST_DECLINE;
    uint64_t obj = c->x[0];
    if (e) mv_ext_destroy(e);
    mv_log("~Extractor(%s) obj %llx", user ? "delete" : "complete", (unsigned long long)obj);
    if (!mv_wr64(c, self, obj + 8, 0)) return BOOT_HOST_STOP;
    return mv_finish(c, self, target, 0, NULL, 0, 0, user ? MV_FINAL_DELETE : MV_FINAL_RETURN, user ? obj : 0);
}

/* ------------------------------------------------------------------ hooks: Decoder methods */
#define MV_DEC_PROLOGUE(name) \
    int ours_; unsigned slot_ = 0; MvDec *d = mv_dec_get(c, self, c->x[0], &ours_, &slot_); \
    if (!ours_) return c->status ? BOOT_HOST_STOP : BOOT_HOST_DECLINE; \
    if (!d) { mv_log(name ": stale decoder %llx", (unsigned long long)c->x[0]); return mv_ret(c, MV_ST_BAD_STATE); }

/* Configure(MediaData *x1) */
static inline int mv_dec_configure(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_DEC_PROLOGUE("Configure")
    uint64_t h = mv_md_handle(c, self, c->x[1]);
    if (c->status) return BOOT_HOST_STOP;
    int32_t w = 0, hh = 0, fps = 30;
    MvMd *m = h ? mv_md_lock(h) : NULL;
    if (m) {
        for (int i = 0; i < m->n; ++i) {
            if (!strcmp(m->it[i].key, "width")) w = (int32_t)m->it[i].v[0];
            if (!strcmp(m->it[i].key, "height")) hh = (int32_t)m->it[i].v[0];
            if (!strcmp(m->it[i].key, "frame-rate") && m->it[i].v[0] > 0 && m->it[i].v[0] <= 240) fps = (int32_t)m->it[i].v[0];
        }
        mv_md_unlock();
    }
    if (w <= 0 || hh <= 0 || w > 8192 || hh > 8192) { w = 1920; hh = 1080; }
    AcquireSRWLockExclusive(&d->lock);
    if (d->state >= MV_DEC_STARTED) { ReleaseSRWLockExclusive(&d->lock); mv_log("Configure while started"); return mv_ret(c, MV_ST_BAD_STATE); }
    if (d->width != w || d->height != hh) mv_dec_release_resources(d);   /* buffer sizes follow the frame size */
    d->width = w; d->height = hh;
    d->pace = mv_config()->pace; d->frame_us = 1000000 / fps;
    d->lookahead_us = mv_config()->lookahead_ms >= 0 ? (int64_t)mv_config()->lookahead_ms * 1000 : -1;
    if (d->native) {
        int32_t err = 0;
        d->in_cap = MV_IN_CAP; d->out_cap = (uint32_t)((size_t)w * (size_t)hh + (size_t)(w / 2) * (size_t)(hh / 2) * 2);
        int ok = !d->vpx ? (d->vpx = tm_vp9_open(mv_config()->threads, &err)) != NULL : 1;
        for (int i = 0; ok && i < MV_NIN; ++i) ok = d->in_mem[i] || (d->in_mem[i] = (uint8_t *)malloc(d->in_cap)) != NULL;
        for (int i = 0; ok && i < MV_NOUT; ++i) ok = d->out[i].mem || (d->out[i].mem = (uint8_t *)malloc(d->out_cap)) != NULL;
        if (!ok) { mv_note("Configure: native decoder unavailable (vpx error %d / memory): stub", err); mv_dec_release_resources(d); d->native = 0; }
    }
    if (!d->native) d->in_cap = 0;
    d->state = MV_DEC_CONFIGURED;
    ReleaseSRWLockExclusive(&d->lock);
    mv_note("Configure %dx%d -> %s%s", w, hh, d->native ? "libvpx " : "stub", d->native && tm_vp9_version ? tm_vp9_version() : "");
    return mv_ret(c, MV_ST_OK);
}
/* RegisterClientEvents(DecoderEvents *x1): {input, output, format changed, error} EventType* */
static inline int mv_dec_register_events(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_DEC_PROLOGUE("RegisterClientEvents")
    uint64_t ev[4] = {0, 0, 0, 0};
    if (c->x[1] && !boot_host_read(c, self, c->x[1], ev, sizeof ev)) return BOOT_HOST_STOP;
    AcquireSRWLockExclusive(&d->lock); memcpy(d->events, ev, sizeof ev); ReleaseSRWLockExclusive(&d->lock);
    mv_log("RegisterClientEvents(%llx): input %llx output %llx format %llx error %llx", (unsigned long long)c->x[1],
           (unsigned long long)ev[0], (unsigned long long)ev[1], (unsigned long long)ev[2], (unsigned long long)ev[3]);
    return mv_ret(c, MV_ST_OK);
}
static inline void mv_dec_format(MvDec *d, MvMd *m, uint64_t raw_mime) {
    m->n = 0;
    if (raw_mime) mv_md_set(m, "mime", MV_MD_STR, (int64_t)raw_mime, 0, 0, 0);
    mv_md_set(m, "width", MV_MD_I32, d->width, 0, 0, 0);
    mv_md_set(m, "height", MV_MD_I32, d->height, 0, 0, 0);
    mv_md_set(m, "stride", MV_MD_I32, d->width, 0, 0, 0);
    mv_md_set(m, "slice-height", MV_MD_I32, d->height, 0, 0, 0);
    mv_md_set(m, "crop", MV_MD_RECT, 0, 0, d->width - 1, d->height - 1);
}
/* GetOutputFormat(MediaData *x1) / GetOutputBufferFormat(int w1, MediaData *x2) (user = argument register) */
static inline int mv_dec_get_format(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; MV_DEC_PROLOGUE("GetOutputFormat")
    uint64_t h = mv_md_handle(c, self, c->x[(uintptr_t)user]);
    if (c->status) return BOOT_HOST_STOP;
    uint64_t raw = mv_guest_str(c, MV_STR_RAW, "video/raw");
    MvMd *m = h ? mv_md_lock(h) : NULL;
    if (!m) return mv_ret(c, MV_ST_BAD_ARG);
    mv_dec_format(d, m, raw); mv_md_unlock();
    mv_log("GetOutputFormat -> %dx%d stride %d", d->width, d->height, d->width);
    return mv_ret(c, MV_ST_OK);
}
/* GetOutputBufferSize(int *x1, int *x2): NV12 frame size and the decoder mode (DecoderImpl 0x122478) */
static inline int mv_dec_buffer_size(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_DEC_PROLOGUE("GetOutputBufferSize")
    if (!c->x[1] || !c->x[2]) return mv_ret(c, MV_ST_BAD_ARG);
    uint32_t size = (uint32_t)(d->width * d->height + (d->width / 2) * (d->height / 2) * 2);
    if (!mv_wr32(c, self, c->x[2], (uint32_t)d->mode) || !mv_wr32(c, self, c->x[1], size)) return BOOT_HOST_STOP;
    return mv_ret(c, MV_ST_OK);
}
/* Start() */
static inline int mv_dec_start(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; MV_DEC_PROLOGUE("Start")
    AcquireSRWLockExclusive(&d->lock);
    if (d->state < MV_DEC_CONFIGURED) { ReleaseSRWLockExclusive(&d->lock); mv_log("Start before Configure"); return mv_ret(c, MV_ST_BAD_STATE); }
    d->state = MV_DEC_STARTED;
    if (d->native && !d->thread) {
        d->quit = 0;
        d->thread = CreateThread(NULL, 0, mv_dec_thread, d, 0, NULL);
        if (!d->thread) { mv_note("Start: CreateThread failed (%lu): stub", (unsigned long)GetLastError()); d->native = 0; }
    }
    if (!d->native) { mv_dec_emit_eos_now(d, 0); d->input_eos = 1; }   /* stub: the movie ends right away, no input wanted */
    unsigned sig = mv_dec_announce(d);
    ReleaseSRWLockExclusive(&d->lock);
    mv_note("Start (%s): signal %x", d->native ? "native" : "stub", sig);
    return mv_finish(c, self, target, slot_, d, sig, 0, MV_FINAL_RETURN, MV_ST_OK);
}
/* Stop() (user 0) / Flush() (user 1) / Reset() (user 2): no further events; clear the four */
static inline int mv_dec_stop(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    MV_DEC_PROLOGUE("Stop")
    int kind = (int)(uintptr_t)user;
    if (kind != 1) mv_dec_join(d);
    AcquireSRWLockExclusive(&d->lock);
    mv_dec_drop(d);
    if (kind == 0) d->state = MV_DEC_STOPPED;
    if (kind == 2) d->state = MV_DEC_CONFIGURED;
    ReleaseSRWLockExclusive(&d->lock);
    WakeAllConditionVariable(&d->cv);
    mv_note("%s: %llu packets, %llu frames decoded, %llu delivered, %llu dropped (late), %llu released, %llu decode errors", kind == 0 ? "Stop" : kind == 1 ? "Flush" : "Reset",
            (unsigned long long)d->n_send, (unsigned long long)d->n_frames, (unsigned long long)d->n_out, (unsigned long long)d->n_drop,
            (unsigned long long)d->n_release, (unsigned long long)d->n_errors);
    return mv_finish(c, self, target, slot_, d, 0, 15, MV_FINAL_RETURN, MV_ST_OK);
}
/* AcquireInputBufferIndex(int *idx x1, int *remaining x2) */
static inline int mv_dec_acquire_input(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; MV_DEC_PROLOGUE("AcquireInputBufferIndex")
    if (!c->x[1] || !c->x[2]) return mv_ret(c, MV_ST_BAD_ARG);
    AcquireSRWLockExclusive(&d->lock);
    int idx = -1;
    if (d->state == MV_DEC_STARTED && !d->input_eos)
        for (int i = 0; i < MV_NIN; ++i) if (d->in_state[i] == MV_IN_FREE) { idx = i; break; }
    if (idx >= 0) d->in_state[idx] = MV_IN_GAME;
    int remaining = idx >= 0 ? mv_dec_free_inputs(d) : 0;
    if (idx < 0) mv_dec_wait_progress(d, "AcquireInputBufferIndex");
    unsigned sig = idx < 0 ? mv_dec_announce(d) : 0;
    ReleaseSRWLockExclusive(&d->lock);
    if (!mv_wr32(c, self, c->x[1], (uint32_t)idx) || !mv_wr32(c, self, c->x[2], (uint32_t)remaining)) {
        if (idx >= 0) { AcquireSRWLockExclusive(&d->lock); d->in_state[idx] = MV_IN_FREE; ReleaseSRWLockExclusive(&d->lock); }
        return BOOT_HOST_STOP;
    }
    if (mv_sample(&d->log_acq_in)) mv_log("AcquireInputBufferIndex -> %d (remaining %d)%s", idx, remaining, idx < 0 ? " none" : "");
    return mv_finish(c, self, target, slot_, d, sig, 0, MV_FINAL_RETURN, idx >= 0 ? MV_ST_OK : MV_ST_NO_BUFFER);
}
/* GetInputBuffer(int w1, Buffer *x2): base = host input slot (tagged), capacity, range (0, 0) */
static inline int mv_dec_get_input(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_DEC_PROLOGUE("GetInputBuffer")
    int32_t idx = (int32_t)c->x[1];
    if (!c->x[2] || idx < 0 || idx >= MV_NIN) return mv_ret(c, MV_ST_BAD_ARG);
    uint8_t b[MV_BUF_SIZE]; memset(b, 0, sizeof b);
    uint64_t base = d->native ? MV_TAG_IN << 48 | (uint64_t)slot_ << 8 | (uint64_t)idx : 0, cap = d->in_cap;
    memcpy(b + 0x18, &cap, 8); memcpy(b + 0x28, &base, 8);
    if (!boot_host_write(c, self, c->x[2], b, sizeof b)) return BOOT_HOST_STOP;
    return mv_ret(c, MV_ST_OK);
}
/* SendInputBufferForDecode(int idx w1, int offset w2, int size w3, long time x4, uint32 flags w5) */
static inline int mv_dec_send(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; MV_DEC_PROLOGUE("SendInputBufferForDecode")
    int32_t idx = (int32_t)c->x[1], off = (int32_t)c->x[2], size = (int32_t)c->x[3]; int64_t time = (int64_t)c->x[4]; uint32_t flags = (uint32_t)c->x[5];
    if (idx < 0 || idx >= MV_NIN || off < 0 || size < 0) return mv_ret(c, MV_ST_BAD_ARG);
    AcquireSRWLockExclusive(&d->lock);
    ++d->n_send;
    if (d->state != MV_DEC_STARTED) {   /* after Stop/Flush (teardown): drop silently */
        d->in_state[idx] = MV_IN_FREE;
        ReleaseSRWLockExclusive(&d->lock);
        return mv_ret(c, MV_ST_OK);
    }
    if (d->native && !d->input_eos) {
        if (d->in_state[idx] == MV_IN_QUEUED) { ReleaseSRWLockExclusive(&d->lock); mv_log("Send(%d): buffer already queued", idx); return mv_ret(c, MV_ST_BAD_STATE); }
        d->in_state[idx] = MV_IN_QUEUED;
        MvPacket *p = &d->q[(d->qhead + d->qcount++) % MV_NIN];
        p->idx = idx; p->off = (uint32_t)off; p->size = (uint32_t)size; p->time = time; p->flags = flags;
        if (flags & MV_FLAG_EOS) d->input_eos = 1;
        WakeAllConditionVariable(&d->cv);
    } else {
        d->in_state[idx] = MV_IN_FREE;
        if (flags & MV_FLAG_EOS && !d->input_eos) { d->input_eos = 1; if (!d->native) mv_dec_emit_eos_now(d, time); }
    }
    mv_dec_wait_progress(d, "SendInputBufferForDecode");
    unsigned sig = mv_dec_announce(d);
    ReleaseSRWLockExclusive(&d->lock);
    if (mv_sample(&d->log_send)) mv_log("Send(%d, off %d, size %d, t=%lld, flags %x) -> signal %x", idx, off, size, (long long)time, flags, sig);
    return mv_finish(c, self, target, slot_, d, sig, 0, MV_FINAL_RETURN, MV_ST_OK);
}
/* AcquireOutputBufferIndex(int *idx x1, long *time x2, uint32 *flags x3, int *remaining x4) */
static inline int mv_dec_acquire_output(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; MV_DEC_PROLOGUE("AcquireOutputBufferIndex")
    if (!c->x[1]) return mv_ret(c, MV_ST_BAD_ARG);
    AcquireSRWLockExclusive(&d->lock);
    int idx = -1; int64_t time = 0; uint32_t flags = 0;
    if (d->state == MV_DEC_STARTED && mv_dec_output_announceable(d)) {
        /* paced: wait (this is the game's movie worker thread) until the head is due, dropping superseded frames */
        for (int64_t wait; d->pace && d->clock_on && d->state == MV_DEC_STARTED && d->nready;) {
            int64_t now = mv_dec_media_now(d), at = mv_dec_display_time(d, now);
            mv_dec_drop_late(d, at);
            if (!(wait = mv_dec_head_wait(d, now, at))) break;
            SleepConditionVariableSRW(&d->cv, &d->lock, (DWORD)(wait >= 200000 ? 200 : (wait + 999) / 1000), 0);
        }
        if (d->state == MV_DEC_STARTED && d->nready) {
            idx = d->ready[0]; memmove(d->ready, d->ready + 1, (size_t)(--d->nready) * sizeof d->ready[0]);
            d->out[idx].state = MV_OUT_GAME; time = d->out[idx].time; flags = d->out[idx].flags;
            if (flags & MV_FLAG_EOS) d->eos_delivered = 1;
            else {
                ++d->n_out; d->last_pts = time;
            }
        }
    }
    int remaining = d->pace ? 0 : d->nready;   /* paced: one at a time */
    if (idx < 0) mv_dec_wait_progress(d, "AcquireOutputBufferIndex");
    unsigned sig = idx < 0 ? mv_dec_announce(d) : 0;
    ReleaseSRWLockExclusive(&d->lock);
    if (!mv_wr32(c, self, c->x[1], (uint32_t)idx) || !mv_wr64(c, self, c->x[2], (uint64_t)time) ||
        !mv_wr32(c, self, c->x[3], flags) || !mv_wr32(c, self, c->x[4], (uint32_t)remaining)) return BOOT_HOST_STOP;
    if (flags & MV_FLAG_EOS) mv_note("AcquireOutputBufferIndex -> %d END OF STREAM (t=%lld, %llu frames delivered)", idx, (long long)time, (unsigned long long)d->n_out);
    else if (mv_sample(&d->log_acq_out)) mv_log("AcquireOutputBufferIndex -> %d t=%lld (remaining %d)", idx, (long long)time, remaining);
    return mv_finish(c, self, target, slot_, d, sig, 0, MV_FINAL_RETURN, idx >= 0 ? MV_ST_OK : MV_ST_NO_BUFFER);
}
/* GetOutputBuffer(int idx w1, Buffer *x2[, MediaData *x3]): the NV12 frame into the game's buffer at base
 * + 0, range (0, size) (DecoderImpl::GetOutputBuffer CPU path 0x122ebc). user = 1: also the format. */
static inline int mv_dec_get_output(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; MV_DEC_PROLOGUE("GetOutputBuffer")
    int32_t idx = (int32_t)c->x[1]; uint64_t buf = c->x[2];
    uint8_t b[MV_BUF_SIZE];
    if (!buf || idx < 0 || idx >= MV_NOUT) return mv_ret(c, MV_ST_BAD_ARG);
    if (!boot_host_read(c, self, buf, b, sizeof b)) return BOOT_HOST_STOP;
    uint64_t base, cap; memcpy(&base, b + 0x28, 8); memcpy(&cap, b + 0x18, 8);
    AcquireSRWLockExclusive(&d->lock);
    int held = d->out[idx].state == MV_OUT_GAME; uint32_t size = held ? d->out[idx].size : 0; const uint8_t *mem = d->out[idx].mem;
    if (held && size) {   /* the renderer's cadence = the game's frame interval (adaptive lookahead) */
        int64_t now = mv_qpc_us(), dt = now - d->render_qpc;
        if (d->render_qpc && dt > 0 && dt < 500000) {
            /* the renderer only calls when it has a frame: fold gaps of several game frames back to one interval */
            if (d->render_ema_us && dt * 5 > d->render_ema_us * 9) { int64_t k = (dt + d->render_ema_us / 2) / d->render_ema_us; if (k > 1) dt /= k; }
            d->render_ema_us = d->render_ema_us ? d->render_ema_us + (dt - d->render_ema_us) / 8 : dt;
        }
        d->render_qpc = now;
        if (d->pace && !d->clock_on) { d->clock_on = 1; d->clock_qpc = now; d->clock_pts = d->out[idx].time; }   /* media clock: first frame shown */
    }
    ReleaseSRWLockExclusive(&d->lock);   /* the slot is the game's until it releases it: copy unlocked */
    uint32_t st = MV_ST_OK; int32_t range[2] = {0, 0};
    if (!held) st = MV_ST_BAD_STATE;
    else if (size && (!base || cap < size)) st = MV_ST_BAD_SIZE;
    else if (size) {
        if (!boot_host_write(c, self, base, mem, size)) return BOOT_HOST_STOP;
        range[1] = (int32_t)size;
#ifdef TOTK_DRAW_PATH
        boot_render_cpu_texels_push(base, size);   /* CPU-written frame: textures/targets over it are stale (play-72: video stayed black) */
#endif
    }
    if (!boot_host_write(c, self, buf, range, 8)) return BOOT_HOST_STOP;
    if (user && c->x[3]) {
        uint64_t h = mv_md_handle(c, self, c->x[3]); MvMd *m = h ? mv_md_lock(h) : NULL;
        if (m) { mv_dec_format(d, m, mv_guest_str(c, MV_STR_RAW, "video/raw")); mv_md_unlock(); }
    }
    if (st || mv_sample(&d->log_get_out)) mv_log("GetOutputBuffer(%d, base %llx cap %llu) -> %u bytes, status %x", idx, (unsigned long long)base, (unsigned long long)cap, range[1], st);
    return mv_ret(c, st);
}
/* ReleaseOutputBufferIndex(int w1) */
static inline int mv_dec_release(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)user; MV_DEC_PROLOGUE("ReleaseOutputBufferIndex")
    int32_t idx = (int32_t)c->x[1];
    if (idx < 0 || idx >= MV_NOUT) return mv_ret(c, MV_ST_BAD_ARG);
    AcquireSRWLockExclusive(&d->lock);
    if (d->out[idx].state == MV_OUT_GAME) { d->out[idx].state = MV_OUT_FREE; ++d->n_release; }
    WakeAllConditionVariable(&d->cv);
    mv_dec_wait_progress(d, "ReleaseOutputBufferIndex");
    unsigned sig = mv_dec_announce(d);
    ReleaseSRWLockExclusive(&d->lock);
    if (mv_sample(&d->log_rel)) mv_log("ReleaseOutputBufferIndex(%d) -> signal %x", idx, sig);
    return mv_finish(c, self, target, slot_, d, sig, 0, MV_FINAL_RETURN, MV_ST_OK);
}
/* GetLastError() */
static inline int mv_dec_last_error(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user; MV_DEC_PROLOGUE("GetLastError")
    return mv_ret(c, d->last_error);
}
/* ~Decoder: D2 (user 0) returns, D0 (user 1) frees the object with operator delete (subsdk0 PLT) */
static inline int mv_dec_dtor(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    int ours; unsigned slot = 0; MvDec *d = mv_dec_get(c, self, c->x[0], &ours, &slot);
    if (!ours) return c->status ? BOOT_HOST_STOP : BOOT_HOST_DECLINE;
    uint64_t obj = c->x[0];
    if (!mv_wr64(c, self, obj + 8, 0)) return BOOT_HOST_STOP;
    unsigned clear = 0;
    if (d) {
        mv_dec_join(d);
        AcquireSRWLockExclusive(&d->lock);
        mv_dec_drop(d); mv_dec_release_resources(d);
        d->state = MV_DEC_STOPPED; clear = 15;
        ReleaseSRWLockExclusive(&d->lock);
        mv_note("~Decoder(%s) obj %llx: %llu frames decoded, %llu delivered", user ? "delete" : "complete", (unsigned long long)obj,
                (unsigned long long)d->n_frames, (unsigned long long)d->n_out);
    }
    /* The events are cleared through the still-valid generation; the slot is freed afterwards (a later
     * call through a stale object sees the generation change and gets MV_ST_BAD_STATE). */
    int rc = mv_finish(c, self, target, slot, d, 0, clear, user ? MV_FINAL_DELETE : MV_FINAL_RETURN, user ? obj : 0);
    if (d) { AcquireSRWLockExclusive(&mv_decs()->lock); d->used = 0; ReleaseSRWLockExclusive(&mv_decs()->lock); }
    return rc;
}

/* ------------------------------------------------------------------ hooks: MediaData */
/* MediaData() x0 = this */
static inline int mv_md_ctor(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target; (void)user;
    uint64_t h = mv_md_alloc();
    if (!h) mv_note("MediaData: pool of %d full", MV_MAX_MD);
    if (!c->x[0] || !mv_wr64(c, self, c->x[0], h)) return c->status ? BOOT_HOST_STOP : mv_ret(c, c->x[0]);
    return BOOT_HOST_DONE;
}
/* ~MediaData() (user 0) / Clear() (user 1) */
static inline int mv_md_dtor(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    uint64_t raw = 0; uint32_t gen; unsigned i;
    if (!c->x[0] || !mv_rd64(c, self, c->x[0], &raw)) return c->status ? BOOT_HOST_STOP : BOOT_HOST_DONE;
    if (!raw) return BOOT_HOST_DONE;
    if (!mv_handle_split(raw, MV_TAG_MD, MV_MAX_MD, &gen, &i)) return BOOT_HOST_DECLINE;   /* a real NnMediaData */
    MvMd *m = mv_md_lock(raw);
    if (m) { m->n = 0; if (!user) m->used = 0; mv_md_unlock(); }
    if (!user && !mv_wr64(c, self, c->x[0], 0)) return BOOT_HOST_STOP;
    return BOOT_HOST_DONE;
}
/* FindInt32(key x1, int *x2) (user 0) / FindInt64(key, long*) (1) / FindString(key, const char **) (2) /
 * FindRect(key, int*l, int*t, int*r, int*b) (3) / Contains(key) (4): bool in w0 */
static inline int mv_md_find(BootContext *c, uint64_t self, uint64_t *target, void *user) {
    (void)target;
    uint64_t raw = 0; uint32_t gen; unsigned i; int kind = (int)(uintptr_t)user;
    if (!c->x[0] || !mv_rd64(c, self, c->x[0], &raw)) return c->status ? BOOT_HOST_STOP : mv_ret(c, 0);
    if (raw && !mv_handle_split(raw, MV_TAG_MD, MV_MAX_MD, &gen, &i)) return BOOT_HOST_DECLINE;
    char key[32];
    if (!c->x[1] || !boot_host_read_string(c, self, c->x[1], key, sizeof key)) return c->status ? BOOT_HOST_STOP : mv_ret(c, 0);
    MvItem it; int found = 0;
    MvMd *m = raw ? mv_md_lock(raw) : NULL;
    if (m) { for (int k = 0; k < m->n; ++k) if (!strcmp(m->it[k].key, key)) { it = m->it[k]; found = 1; break; } mv_md_unlock(); }
    if (found) switch (kind) {
        case 0: found = (it.type == MV_MD_I32 || it.type == MV_MD_I64) && mv_wr32(c, self, c->x[2], (uint32_t)it.v[0]); break;
        case 1: found = (it.type == MV_MD_I32 || it.type == MV_MD_I64) && mv_wr64(c, self, c->x[2], (uint64_t)it.v[0]); break;
        case 2: found = it.type == MV_MD_STR && mv_wr64(c, self, c->x[2], (uint64_t)it.v[0]); break;
        case 3: found = it.type == MV_MD_RECT && mv_wr32(c, self, c->x[2], (uint32_t)it.v[0]) && mv_wr32(c, self, c->x[3], (uint32_t)it.v[1]) &&
                        mv_wr32(c, self, c->x[4], (uint32_t)it.v[2]) && mv_wr32(c, self, c->x[5], (uint32_t)it.v[3]); break;
        default: break;
    }
    if (c->status) return BOOT_HOST_STOP;
    if (mv_config()->log >= 2) mv_log("MediaData::Find(%d, %s) -> %d", kind, key, found);
    return mv_ret(c, (uint64_t)found);
}

/* ------------------------------------------------------------------ registration */
typedef struct MvHook { uint32_t offset; const char *name; BootHostFunction fn; uintptr_t user; } MvHook;
static inline unsigned boot_movie_register(void) {
    const MvConfig *cfg = mv_config();
    if (cfg->mode == MV_MODE_OFF) { mv_note("TOTK_MOVIE=off: movie:: not replaced"); return 0; }
    static const MvHook hooks[] = {
        {0x127370, "movie::Extractor::Extractor(ContainerType)", mv_ext_ctor, 0},
        {0x1272dc, "movie::Extractor::Extractor(ContainerType,CacheSize)", mv_ext_ctor, 1},
        {0x124524, "movie::Decoder::Decoder", mv_dec_ctor, 0},
        /* Extractor vtable */
        {0x1273fc, "movie::Extractor::~Extractor(D2)", mv_ext_dtor, 0},
        {0x127438, "movie::Extractor::~Extractor(D0)", mv_ext_dtor, 1},
        {0x127480, "movie::Extractor::SetDataSource(path)", mv_ext_set_source, 0},
        {0x127adc, "movie::Extractor::SetDataSource(mem)", mv_ext_set_source_other, 1},
        {0x127a88, "movie::Extractor::SetDataSource(reader)", mv_ext_set_source_other, 2},
        {0x127b30, "movie::Extractor::GetTrackCount", mv_ext_track_count, 0},
        {0x127b50, "movie::Extractor::GetTrackConfiguration", mv_ext_track_config, 0},
        {0x127b6c, "movie::Extractor::SelectTrack", mv_ext_select, 1},
        {0x127b88, "movie::Extractor::UnselectTrack", mv_ext_select, 0},
        {0x127ba4, "movie::Extractor::SeekTo", mv_ext_seek, 0},
        {0x127bfc, "movie::Extractor::GetTrackIndexForAvailableData", mv_ext_next_track, 0},
        {0x127be0, "movie::Extractor::Read", mv_ext_read, 0},
        {0x127bc4, "movie::Extractor::Advance", mv_ext_advance, 0},
        {0x127c18, "movie::Extractor::GetSampleTime", mv_ext_sample_time, 0},
        {0x127c34, "movie::Extractor::GetSampleMetaData", mv_ext_meta, 0},
        {0x127c50, "movie::Extractor::GetBufferedRange", mv_ext_meta, 1},
        /* Decoder vtable */
        {0x1245a4, "movie::Decoder::~Decoder(D2)", mv_dec_dtor, 0},
        {0x1245ec, "movie::Decoder::~Decoder(D0)", mv_dec_dtor, 1},
        {0x124634, "movie::Decoder::Configure", mv_dec_configure, 0},
        {0x124648, "movie::Decoder::RegisterClientEvents", mv_dec_register_events, 0},
        {0x1246d4, "movie::Decoder::GetOutputFormat", mv_dec_get_format, 1},
        {0x12465c, "movie::Decoder::Start", mv_dec_start, 0},
        {0x124670, "movie::Decoder::Stop", mv_dec_stop, 0},
        {0x124684, "movie::Decoder::Reset", mv_dec_stop, 2},
        {0x124698, "movie::Decoder::Flush", mv_dec_stop, 1},
        {0x124724, "movie::Decoder::AcquireInputBufferIndex", mv_dec_acquire_input, 0},
        {0x124738, "movie::Decoder::GetInputBuffer", mv_dec_get_input, 0},
        {0x1246ac, "movie::Decoder::SendInputBufferForDecode", mv_dec_send, 0},
        {0x1246e8, "movie::Decoder::AcquireOutputBufferIndex", mv_dec_acquire_output, 0},
        {0x124710, "movie::Decoder::GetOutputBuffer", mv_dec_get_output, 0},
        {0x1246fc, "movie::Decoder::GetOutputBufferSize", mv_dec_buffer_size, 0},
        {0x1246c0, "movie::Decoder::ReleaseOutputBufferIndex", mv_dec_release, 0},
        {0x12474c, "movie::Decoder::GetLastError", mv_dec_last_error, 0},
        {0x124760, "movie::Decoder::GetOutputBuffer(format)", mv_dec_get_output, 1},
        {0x124774, "movie::Decoder::GetOutputBufferFormat", mv_dec_get_format, 2},
        /* MediaData (main imports ctor, dtor, Clear, FindInt32, FindString, FindRect) */
        {0x128094, "movie::MediaData::MediaData", mv_md_ctor, 0},
        {0x1280c8, "movie::MediaData::~MediaData", mv_md_dtor, 0},
        {0x128194, "movie::MediaData::Clear", mv_md_dtor, 1},
        {0x1281b4, "movie::MediaData::FindInt32", mv_md_find, 0},
        {0x1281d4, "movie::MediaData::FindInt64", mv_md_find, 1},
        {0x1282a4, "movie::MediaData::FindString", mv_md_find, 2},
        {0x1282c4, "movie::MediaData::FindRect", mv_md_find, 3},
        {0x128294, "movie::MediaData::Contains", mv_md_find, 4},
    };
    unsigned failed = 0;
    if (!boot_host_fn_register("movie.cont", mv_cont, NULL, BOOT_HOST_FN_NOLOCK, 1)) ++failed;
    for (unsigned i = 0; i < sizeof hooks / sizeof hooks[0]; ++i)
        if (!boot_host_fn_hook(2, hooks[i].offset, hooks[i].name, hooks[i].fn, (void *)hooks[i].user, BOOT_HOST_FN_NOLOCK, MV_STEPS)) ++failed;
    mv_note("movie:: replaced (%u hooks, %u failed), mode %s, libvpx/libwebm %s, %d decode threads",
            (unsigned)(sizeof hooks / sizeof hooks[0]), failed, cfg->mode == MV_MODE_NATIVE ? "native" : "stub",
            mv_native_available() ? "linked" : "NOT linked (stub)", cfg->threads);
    return failed;
}
#endif /* !TOTK_HOST_MOVIE_SHIM */
#endif
