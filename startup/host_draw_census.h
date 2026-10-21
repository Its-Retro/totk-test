#ifndef TOTK_HOST_DRAW_CENSUS_H
#define TOTK_HOST_DRAW_CENSUS_H
/* Draw census (TOTK_DRAW_CENSUS=1): a per-frame watcher for what the renderer was asked to draw and what it
 * actually submitted. Diagnostic only: it never changes, skips or reorders GPU work, reads no GPU-owned memory,
 * decodes no command buffer and adds no query, readback or wait. Off by default; the environment is read once
 * into a cached static and, while off, every hook is one predictable branch (BOOT_CENSUS_ON()).
 *
 * Identity. A draw is keyed by a 64-bit hash of (vertex program id, pixel program id, stream 0 vertex buffer
 * address, index buffer address, first, count, base vertex, colour target 0 address); the instance count is
 * not part of it. A compute dispatch is keyed by (program data address, storage buffer addresses). Program ids
 * are the 32-byte shader ids (first 8 bytes hashed, first 4 printed, so a grep of shader-manifest.tsv finds
 * them); a draw that failed before the ids were known falls back to the program addresses ("va:" in the log).
 *
 * Outcome. Every draw ends as submitted or dropped:<reason> (the renderer's own error text with numbers cut
 * off, so reasons group). Dropped draws are still "issued by the game", which is how the two causes of a
 * vanished object are told apart.
 *
 * Vanish event. A key submitted in each of >= TOTK_DRAW_CENSUS_MIN_RUN (8) consecutive frames, not submitted
 * for 1..TOTK_DRAW_CENSUS_MAX_GAP (30) frames, then submitted again. Per key the table keeps the current run
 * length and the frame of the last submission, which is the same information as an 8-frame ring of key sets
 * (run >= 8 <=> present in each of the previous 8 frames) without 7 lookups per key per frame. The gap is
 * classified GAME (the key was not issued in any gap frame), OURS (issued and dropped in every gap frame, with
 * the drop reason) or MIXED. Events of the same (start frame, gap) are one group: a wall vanishing at a camera
 * cut is one line plus a few example keys, not thousands.
 *
 * Output: frames\draw-census.log (the previous run's file becomes draw-census.prev.log). Written only from the
 * present path through one buffered FILE*, flushed once per second. About 2000 vanish lines per run, then only
 * the once-per-second summary. See README.md next to this header for how to read it.
 *
 * Threading: draws, dispatches and presents all run under the renderer lock (BootWindow.lock), so no atomics.
 * Self-contained: needs only the C library and windows.h. The hooks take plain integers (BOOT_CENSUS_DRAW is a
 * macro, so the BootDrawState type is only needed at the call site). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define BOOT_CENSUS_SLOTS 65536u         /* power of two; 32 bytes each = 2 MiB, allocated only when enabled */
#define BOOT_CENSUS_LIVE_MAX 32768u      /* tracked keys; a new key past this is counted as overflow and ignored */
#define BOOT_CENSUS_SWEEP_EVERY 4u       /* frames between expiry sweeps of the table (or sooner when 3/4 full) */
#define BOOT_CENSUS_PENDING 8192u        /* vanish events waiting for the flicker detector's verdict */
#define BOOT_CENSUS_REASONS 48u
#define BOOT_CENSUS_GROUPS 32u
#define BOOT_CENSUS_GROUP_EXAMPLES 8u
#define BOOT_CENSUS_LINES 2000u          /* vanish lines per run (group headers + examples) */
#define BOOT_CENSUS_SUMMARY_LINES 200000u
#define BOOT_CENSUS_MARKS 32u

#define BOOT_CENSUS_E_STATE 1             /* BOOT_RENDER_E_STATE, kept literal so this header needs no renderer headers */
enum { BOOT_CENSUS_F_VA_VS = 1, BOOT_CENSUS_F_VA_PS = 2, BOOT_CENSUS_F_COMPUTE = 4, BOOT_CENSUS_F_HAS_PS = 8, BOOT_CENSUS_F_TAGGED = 128 };

typedef struct BootCensusEntry {           /* 32 bytes */
    uint64_t key;                          /* 0 = empty slot */
    uint32_t last_sub;                     /* frame of the last submission (0 = never) */
    uint32_t last_drop;                    /* frame of the last dropped draw (0 = never) */
    uint32_t vs32, ps32;                   /* first 4 bytes of the program ids, or the low 32 bits of the program addresses (flags) */
    uint16_t streak;                       /* consecutive frames with a submission, ending at last_sub (saturating) */
    uint16_t gap_drops;                    /* frames since last_sub in which the key was issued and dropped */
    uint8_t reason, flags;
} BootCensusEntry;
typedef struct BootCensusEvent {           /* 32 bytes */
    uint64_t key;
    uint32_t ret_frame, start;             /* frame the key came back / first frame it was missing */
    uint32_t vs32, ps32;
    uint16_t gap, prev_run, gap_drops;
    uint8_t reason, flags;
} BootCensusEvent;
typedef struct BootCensusReason { char text[44]; uint64_t hash, total, window; } BootCensusReason;

typedef struct BootCensus {
    int mode;                              /* 0 not decided yet, 1 on, 2 off */
    BootCensusEntry *table; BootCensusEvent *pend; uint32_t live, npend;
    uint32_t frame;                        /* the frame being drawn; first present ends frame 1 */
    uint32_t min_run, max_gap, delay;      /* delay: frames a vanish event waits before it is logged (flicker detector lag) */
    uint64_t pend_vs, pend_ps; int pend_valid, pend_has_ps;   /* program ids of the draw in flight (boot_census_programs) */
    uint32_t frame_keys;                   /* distinct submitted keys this frame */
    uint32_t marks[BOOT_CENSUS_MARKS], nmarks;                /* census frames of flicker DROP verdicts */
    BootCensusReason reasons[BOOT_CENSUS_REASONS]; uint8_t rtab[128]; uint32_t nreasons;
    /* window (since the last summary) and run totals */
    uint32_t w_frames, w_sub, w_drop, w_csub, w_cdrop, w_keys_sum, w_keys_max, w_ev[3], w_lost;
    uint64_t t_sub, t_drop, t_csub, t_cdrop, t_frames, t_ev[3], t_lost, t_gone, t_overflow;
    uint32_t last_keys;
    FILE *log; int log_failed; uint32_t lines_left, summary_lines; int budget_noted;
    int64_t freq, t0, sum_t, sum_interval;
} BootCensus;
static BootCensus boot_census;

#define BOOT_CENSUS_ON() (__builtin_expect(boot_census.mode != 2, 0))
/* BOOT_CENSUS_DRAW(const BootDrawState *s, int rc, const char *err): err is the renderer's error text for rc != 0 */
#define BOOT_CENSUS_DRAW(s, rc, err) boot_census_draw_slow((s)->program_base + (s)->stage_offset[1], \
    (s)->stage_enabled[5] ? (s)->program_base + (s)->stage_offset[5] : 0, (s)->stream[0].address, (s)->index_address, (s)->first, (s)->count, \
    (s)->base_vertex_reg, (s)->rt_count ? (s)->rt[(s)->rt_map[0]].address : ((s)->zt.enabled ? (s)->zt.address : 0), (rc), (err))
#define BOOT_CENSUS_PRESENT() boot_census_present_slow()

static inline uint64_t boot_census_mix(uint64_t h, uint64_t v) {
    h ^= v + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    h *= UINT64_C(0xff51afd7ed558ccd); h ^= h >> 32;
    return h;
}
static inline uint64_t boot_census_final(uint64_t h) {
    h ^= h >> 29; h *= UINT64_C(0xc4ceb9fe1a85ec53); h ^= h >> 32;
    return h ? h : 1;
}
static inline uint32_t boot_census_env_u32(const char *name, uint32_t def, uint32_t lo, uint32_t hi) {
    char v[16]; DWORD k = GetEnvironmentVariableA(name, v, sizeof v);
    if (!k || k >= sizeof v) return def;
    unsigned long n = strtoul(v, NULL, 10);
    return n < lo ? lo : n > hi ? hi : (uint32_t)n;
}
static inline __attribute__((noinline)) void boot_census_init(BootCensus *c) {
    char v[8]; DWORD k = GetEnvironmentVariableA("TOTK_DRAW_CENSUS", v, sizeof v);
    if (!(k && k < sizeof v && v[0] == '1')) { c->mode = 2; return; }
    c->table = (BootCensusEntry *)calloc(BOOT_CENSUS_SLOTS, sizeof *c->table);
    c->pend = (BootCensusEvent *)calloc(BOOT_CENSUS_PENDING, sizeof *c->pend);
    if (!c->table || !c->pend) { free(c->table); free(c->pend); c->table = NULL; c->pend = NULL; c->mode = 2; return; }
    c->min_run = boot_census_env_u32("TOTK_DRAW_CENSUS_MIN_RUN", 8, 2, 1000);
    c->max_gap = boot_census_env_u32("TOTK_DRAW_CENSUS_MAX_GAP", 30, 1, 600);
    k = GetEnvironmentVariableA("TOTK_FLICKER_LOG", v, sizeof v);
    c->delay = (k && k < sizeof v && v[0] == '1') ? 10 : 0;    /* the flicker detector reports up to 8 presents late */
    k = GetEnvironmentVariableA("TOTK_GLITCH_SCAN", v, sizeof v);   /* water-work: the glitch scanner reports ~2 presents + the readback lag late */
    if (k && k < sizeof v && v[0] && v[0] != '0') c->delay = 14;
    c->frame = 1; c->lines_left = BOOT_CENSUS_LINES; c->summary_lines = BOOT_CENSUS_SUMMARY_LINES;
    LARGE_INTEGER q; QueryPerformanceFrequency(&q); c->freq = q.QuadPart > 0 ? q.QuadPart : 1;
    QueryPerformanceCounter(&q); c->t0 = c->sum_t = q.QuadPart; c->sum_interval = c->freq;
    memcpy(c->reasons[0].text, "other", 6);
    c->nreasons = 1;
    c->mode = 1;
}

/* ------------------------------------------------------------------ drop reasons */
/* The renderer's error text up to the first ':' or digit ("render target format/layout: rt 3 va ..." ->
 * "render target format/layout"), so reasons group; without text, the result class. */
static inline uint8_t boot_census_reason(BootCensus *c, int rc, const char *err) {
    static const char *const cls[] = {"ok", "state", "shader", "memory", "format", "d3d", "capacity"};
    char buf[44]; size_t n = 0;
    if (err) while (err[n] && n < sizeof buf - 1 && err[n] != ':' && (err[n] < '0' || err[n] > '9')) { buf[n] = err[n]; ++n; }
    while (n && buf[n - 1] == ' ') --n;
    if (!n) { const char *p = rc >= 0 && rc < 7 ? cls[rc] : "error"; n = (size_t)snprintf(buf, sizeof buf, "code %s", p); }
    buf[n] = 0;
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)buf[i]; h *= UINT64_C(1099511628211); }
    uint32_t i = (uint32_t)h & 127u;
    for (unsigned probe = 0; probe < 128 && c->rtab[i]; ++probe, i = (i + 1) & 127u) {
        BootCensusReason *r = &c->reasons[c->rtab[i]];
        if (r->hash == h && !strcmp(r->text, buf)) return c->rtab[i];
    }
    if (c->nreasons >= BOOT_CENSUS_REASONS) return 0;
    uint8_t idx = (uint8_t)c->nreasons++;
    memcpy(c->reasons[idx].text, buf, n + 1); c->reasons[idx].hash = h;
    c->rtab[i] = idx;
    return idx;
}

/* ------------------------------------------------------------------ key table */
static inline BootCensusEntry *boot_census_slot(BootCensus *c, uint64_t key) {
    BootCensusEntry *t = c->table;
    for (uint32_t i = (uint32_t)key & (BOOT_CENSUS_SLOTS - 1);; i = (i + 1) & (BOOT_CENSUS_SLOTS - 1)) {
        if (t[i].key == key) return &t[i];
        if (!t[i].key) {
            if (c->live >= BOOT_CENSUS_LIVE_MAX) { ++c->t_overflow; return NULL; }
            t[i].key = key; ++c->live; return &t[i];
        }
    }
}
/* backward-shift deletion (Knuth 6.4 algorithm R): no tombstones */
static inline void boot_census_del(BootCensus *c, uint32_t i) {
    BootCensusEntry *t = c->table; const uint32_t M = BOOT_CENSUS_SLOTS - 1; uint32_t j = i;
    for (;;) {
        j = (j + 1) & M;
        if (!t[j].key) break;
        uint32_t home = (uint32_t)t[j].key & M;
        int stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
        if (stays) continue;
        t[i] = t[j]; i = j;
    }
    memset(&t[i], 0, sizeof t[i]);
    --c->live;
}
static inline int boot_census_expired(const BootCensus *c, const BootCensusEntry *e, uint32_t f) {
    if (e->streak >= c->min_run && e->last_sub) return f - e->last_sub > c->max_gap;   /* armed: keep until a return could no longer count */
    uint32_t newest = e->last_sub > e->last_drop ? e->last_sub : e->last_drop;
    return f - newest >= 2;                                                              /* cannot extend a run any more */
}
static inline void boot_census_sweep(BootCensus *c, uint32_t f) {
    BootCensusEntry *t = c->table;
    for (uint32_t i = 0; i < BOOT_CENSUS_SLOTS;) {
        if (!t[i].key) { ++i; continue; }
        if (boot_census_expired(c, &t[i], f)) {
            if (t[i].streak >= c->min_run && t[i].last_sub) ++c->t_gone;
            boot_census_del(c, i); continue;
        }
        ++i;
    }
}
static inline void boot_census_tag(BootCensusEntry *e, uint32_t vs32, uint32_t ps32, uint8_t flags) {
    if (!(e->flags & BOOT_CENSUS_F_TAGGED) || !(flags & (BOOT_CENSUS_F_VA_VS | BOOT_CENSUS_F_VA_PS))) {   /* real ids replace address placeholders */
        e->vs32 = vs32; e->ps32 = ps32; e->flags = (uint8_t)(flags | BOOT_CENSUS_F_TAGGED);
    }
}

/* ------------------------------------------------------------------ outcomes (draw / dispatch path) */
static inline void boot_census_submitted(BootCensus *c, uint64_t key, uint32_t vs32, uint32_t ps32, uint8_t flags) {
    BootCensusEntry *e = boot_census_slot(c, key);
    if (flags & BOOT_CENSUS_F_COMPUTE) { ++c->w_csub; ++c->t_csub; } else { ++c->w_sub; ++c->t_sub; }
    if (!e) return;
    const uint32_t f = c->frame;
    if (e->last_sub == f) return;                               /* the same key again this frame */
    ++c->frame_keys;
    boot_census_tag(e, vs32, ps32, flags);
    if (e->last_drop == f && e->gap_drops) --e->gap_drops;      /* dropped earlier this frame: not a gap frame */
    if (e->last_sub == f - 1) { if (e->streak < 0xffff) ++e->streak; }
    else {
        if (e->last_sub && e->streak >= c->min_run) {
            uint32_t gap = f - e->last_sub - 1;                 /* >= 1: last_sub < f - 1 */
            if (gap <= c->max_gap) {
                unsigned cls = e->gap_drops == 0 ? 0 : e->gap_drops >= gap ? 1 : 2;
                ++c->w_ev[cls]; ++c->t_ev[cls];
                if (c->npend < BOOT_CENSUS_PENDING) {
                    BootCensusEvent *v = &c->pend[c->npend++];
                    v->key = key; v->ret_frame = f; v->start = e->last_sub + 1; v->vs32 = e->vs32; v->ps32 = e->ps32;
                    v->gap = (uint16_t)gap; v->prev_run = e->streak; v->gap_drops = e->gap_drops; v->reason = e->reason; v->flags = e->flags;
                } else { ++c->w_lost; ++c->t_lost; }
            } else ++c->t_gone;                                 /* away too long to count as a vanish */
        }
        e->streak = 1;
    }
    e->last_sub = f; e->gap_drops = 0;
}
static inline void boot_census_dropped(BootCensus *c, uint64_t key, uint32_t vs32, uint32_t ps32, uint8_t flags, int rc, const char *err) {
    uint8_t why = boot_census_reason(c, rc, err);
    ++c->reasons[why].total; ++c->reasons[why].window;
    if (flags & BOOT_CENSUS_F_COMPUTE) { ++c->w_cdrop; ++c->t_cdrop; } else { ++c->w_drop; ++c->t_drop; }
    BootCensusEntry *e = boot_census_slot(c, key);
    if (!e) return;
    const uint32_t f = c->frame;
    boot_census_tag(e, vs32, ps32, flags);
    e->reason = why;
    if (e->last_drop != f) {
        e->last_drop = f;
        if (e->last_sub != f && e->gap_drops < 0xffff) ++e->gap_drops;
    }
}
static inline void boot_census_outcome(BootCensus *c, uint64_t key, uint32_t vs32, uint32_t ps32, uint8_t flags, int rc, const char *err) {
    if (rc) boot_census_dropped(c, key, vs32, ps32, flags, rc, err);
    else boot_census_submitted(c, key, vs32, ps32, flags);
}

/* Hook: the program ids of the draw in flight (host_gpu_render_draw.h, after both ids are known). */
static inline __attribute__((noinline)) void boot_census_programs_slow(const uint8_t *vs_id, const uint8_t *ps_id) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1) return;
    memcpy(&c->pend_vs, vs_id, 8);
    if (ps_id) memcpy(&c->pend_ps, ps_id, 8); else c->pend_ps = 0;
    c->pend_has_ps = ps_id != NULL; c->pend_valid = 1;
}
/* Hook: a draw finished (rc 0 = recorded, else dropped). Program addresses stand in for ids that were never read. */
static inline __attribute__((noinline)) void boot_census_draw_slow(uint64_t vs_va, uint64_t ps_va, uint64_t vbuf0, uint64_t ibuf, uint32_t first,
                                                            uint32_t count, uint32_t base_vertex, uint64_t rt0, int rc, const char *err) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1) return;
    uint64_t vt, pt; uint8_t flags;
    if (c->pend_valid) { vt = c->pend_vs; pt = c->pend_ps; flags = c->pend_has_ps ? BOOT_CENSUS_F_HAS_PS : 0; }
    else { vt = vs_va; pt = ps_va; flags = (uint8_t)(BOOT_CENSUS_F_VA_VS | (ps_va ? BOOT_CENSUS_F_VA_PS | BOOT_CENSUS_F_HAS_PS : 0)); }
    c->pend_valid = 0;
    uint64_t h = boot_census_mix(1, vt);
    h = boot_census_mix(h, pt); h = boot_census_mix(h, vbuf0); h = boot_census_mix(h, ibuf);
    h = boot_census_mix(h, ((uint64_t)first << 32) | count); h = boot_census_mix(h, base_vertex); h = boot_census_mix(h, rt0);
    boot_census_outcome(c, boot_census_final(h), (uint32_t)vt, (uint32_t)pt, flags, rc, err);
}
/* Hook: a draw that never reached the renderer because the bound state could not be built (host_nvn_play.h). */
static inline __attribute__((noinline)) void boot_census_nostate_slow(uint64_t ibuf, uint32_t first, uint32_t count, uint32_t mode) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1) return;
    uint64_t h = boot_census_mix(3, ibuf); h = boot_census_mix(h, ((uint64_t)first << 32) | count); h = boot_census_mix(h, mode);
    boot_census_outcome(c, boot_census_final(h), 0, 0, BOOT_CENSUS_F_VA_VS, BOOT_CENSUS_E_STATE, "native draw state (no program)");
}
/* Hook: a compute dispatch finished. `id` (32 bytes) may be NULL when it was never read; the key uses the program address. */
static inline __attribute__((noinline)) void boot_census_compute_slow(uint64_t cs_va, const uint8_t *id, const uint64_t *ssbo, unsigned n, int rc, const char *err) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1) return;
    uint64_t h = boot_census_mix(2, cs_va);
    for (unsigned i = 0; i < n; ++i) h = boot_census_mix(h, ssbo[i]);
    uint32_t name = (uint32_t)cs_va; uint8_t flags = BOOT_CENSUS_F_COMPUTE | BOOT_CENSUS_F_VA_VS;
    if (id) { memcpy(&name, id, 4); flags = BOOT_CENSUS_F_COMPUTE; }
    boot_census_outcome(c, boot_census_final(h), name, 0, flags, rc, err);
}

/* ------------------------------------------------------------------ log (present path only) */
static inline FILE *boot_census_open(BootCensus *c) {
    if (c->log || c->log_failed) return c->log;
    CreateDirectoryA("frames", NULL);
    MoveFileExA("frames\\draw-census.log", "frames\\draw-census.prev.log", MOVEFILE_REPLACE_EXISTING);
    c->log = fopen("frames\\draw-census.log", "w");
    if (!c->log) { c->log_failed = 1; return NULL; }
    setvbuf(c->log, NULL, _IOFBF, 1 << 16);
    fprintf(c->log, "# draw-census v1: min run %u frames, max gap %u frames, event delay %u frames; keys: draw = hash(vs,ps,vbuf0,ibuf,first,count,base,rt0)\n"
                    "# V = vanish group (frame it returned, first missing frame, gap in frames, keys; GAME = never issued, OURS = issued and dropped every gap frame)\n"
                    "# S = once-per-second summary, F = flicker detector DROP verdict or glitch scanner event (flicker-drop= counts both)\n", c->min_run, c->max_gap, c->delay);
    return c->log;
}
static inline void boot_census_prog(char *o, size_t n, uint32_t t, int va, int present) {
    if (!present) snprintf(o, n, "-");
    else if (va) snprintf(o, n, "va:%08x", t);
    else snprintf(o, n, "%02x%02x%02x%02x", t & 255u, (t >> 8) & 255u, (t >> 16) & 255u, t >> 24);
}
static inline int boot_census_flicker_near(const BootCensus *c, uint32_t start) {
    for (uint32_t i = 0; i < c->nmarks && i < BOOT_CENSUS_MARKS; ++i) {
        uint32_t m = c->marks[i]; uint32_t d = m > start ? m - start : start - m;
        if (d <= 2) return 1;
    }
    return 0;
}
static inline unsigned boot_census_class(const BootCensusEvent *v) { return v->gap_drops == 0 ? 0 : v->gap_drops >= v->gap ? 1 : 2; }
static inline void boot_census_flush_events(BootCensus *c) {
    uint32_t n = 0;
    while (n < c->npend && c->pend[n].ret_frame + c->delay <= c->frame) ++n;
    if (!n) return;
    FILE *f = c->lines_left ? boot_census_open(c) : NULL;
    if (f) {
        static const char *const cname[3] = {"GAME", "OURS", "MIXED"};
        struct { uint32_t ret, start, gap, keys, cls[3], shown; } g[BOOT_CENSUS_GROUPS]; uint32_t ng = 0;
        static uint8_t gid[BOOT_CENSUS_PENDING];
        for (uint32_t i = 0; i < n; ++i) {
            const BootCensusEvent *v = &c->pend[i]; uint32_t k = 0;
            while (k < ng && !(g[k].start == v->start && g[k].gap == v->gap)) ++k;
            if (k == ng) {   /* a new group; the last slot collects whatever does not fit (start 0) */
                if (ng == BOOT_CENSUS_GROUPS) k = BOOT_CENSUS_GROUPS - 1;
                else {
                    int rest = ng == BOOT_CENSUS_GROUPS - 1;
                    k = ng++; memset(&g[k], 0, sizeof g[k]);
                    g[k].ret = v->ret_frame; g[k].start = rest ? 0 : v->start; g[k].gap = rest ? 0 : v->gap;
                }
            }
            ++g[k].keys; ++g[k].cls[boot_census_class(v)]; gid[i] = (uint8_t)k;
        }
        for (uint32_t k = 0; k < ng && c->lines_left; ++k) {
            if (g[k].start) fprintf(f, "V frame=%u start=%u gap=%u keys=%u game=%u ours=%u mixed=%u flicker-drop=%s\n", g[k].ret, g[k].start, g[k].gap, g[k].keys,
                                    g[k].cls[0], g[k].cls[1], g[k].cls[2], boot_census_flicker_near(c, g[k].start) ? "near" : "no");
            else fprintf(f, "V frame=%u (further groups) keys=%u game=%u ours=%u mixed=%u\n", g[k].ret, g[k].keys, g[k].cls[0], g[k].cls[1], g[k].cls[2]);
            --c->lines_left;
            for (unsigned pass = 0; pass < 2; ++pass)           /* examples: our-side events first */
                for (uint32_t i = 0; i < n && c->lines_left && g[k].shown < BOOT_CENSUS_GROUP_EXAMPLES; ++i) {
                    const BootCensusEvent *v = &c->pend[i]; unsigned cls = boot_census_class(v);
                    if (gid[i] != k || (pass == 0) != (cls != 0)) continue;
                    char a[16], b[16];
                    boot_census_prog(a, sizeof a, v->vs32, v->flags & BOOT_CENSUS_F_VA_VS, 1);
                    boot_census_prog(b, sizeof b, v->ps32, v->flags & BOOT_CENSUS_F_VA_PS, (v->flags & BOOT_CENSUS_F_HAS_PS) != 0);
                    fprintf(f, "  %s %s key=%016llx vs=%s ps=%s run=%u dropped-in-gap=%u/%u", cname[cls], (v->flags & BOOT_CENSUS_F_COMPUTE) ? "cs" : "draw",
                            (unsigned long long)v->key, a, b, v->prev_run, v->gap_drops, v->gap);
                    if (cls) fprintf(f, " reason=\"%s\"", c->reasons[v->reason < c->nreasons ? v->reason : 0].text);
                    fputc('\n', f); --c->lines_left; ++g[k].shown;
                }
        }
        if (!c->lines_left && !c->budget_noted) { fprintf(f, "# vanish line budget (%u) used up; the per-second summaries continue\n", BOOT_CENSUS_LINES); c->budget_noted = 1; }
    }
    memmove(c->pend, c->pend + n, (size_t)(c->npend - n) * sizeof *c->pend);
    c->npend -= n;
}
static inline void boot_census_summary(BootCensus *c, int64_t now) {
    FILE *f = c->summary_lines ? boot_census_open(c) : NULL;
    if (f) {
        double secs = (double)(now - c->sum_t) / (double)c->freq, since = (double)(now - c->t0) / (double)c->freq;
        fprintf(f, "S t=%.0fs frames=%u fps=%.1f draws=%u dropped=%u cs=%u/%u keys/frame=%u(max %u) live=%u vanish game=%u ours=%u mixed=%u (run %llu/%llu/%llu) pending=%u lost=%u gone=%llu overflow=%llu",
                since, c->w_frames, secs > 0 ? (double)c->w_frames / secs : 0.0, c->w_sub, c->w_drop, c->w_csub, c->w_cdrop,
                c->w_frames ? c->w_keys_sum / c->w_frames : 0, c->w_keys_max, c->live, c->w_ev[0], c->w_ev[1], c->w_ev[2],
                (unsigned long long)c->t_ev[0], (unsigned long long)c->t_ev[1], (unsigned long long)c->t_ev[2], c->npend, c->w_lost,
                (unsigned long long)c->t_gone, (unsigned long long)c->t_overflow);
        const char *sep = " drops:";
        for (uint32_t i = 0; i < c->nreasons; ++i)
            if (c->reasons[i].window) { fprintf(f, "%s \"%s\"=%llu", sep, c->reasons[i].text, (unsigned long long)c->reasons[i].window); sep = ","; }
        fputc('\n', f); fflush(f); --c->summary_lines;
    }
    for (uint32_t i = 0; i < c->nreasons; ++i) c->reasons[i].window = 0;
    c->w_frames = c->w_sub = c->w_drop = c->w_csub = c->w_cdrop = c->w_keys_sum = c->w_keys_max = c->w_lost = 0;
    c->w_ev[0] = c->w_ev[1] = c->w_ev[2] = 0;
    c->sum_t = now;
}
/* Hook: a presented frame ends the census frame (host_nvn_play.h and host_gpu_draw_exec.h, before the flicker watch). */
static inline __attribute__((noinline)) void boot_census_present_slow(void) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1) return;
    const uint32_t f = c->frame;
    ++c->w_frames; ++c->t_frames; c->w_keys_sum += c->frame_keys; c->last_keys = c->frame_keys;
    if (c->frame_keys > c->w_keys_max) c->w_keys_max = c->frame_keys;
    boot_census_flush_events(c);
    if (f % BOOT_CENSUS_SWEEP_EVERY == 0 || c->live > BOOT_CENSUS_LIVE_MAX / 4 * 3) boot_census_sweep(c, f);
    ++c->frame; c->frame_keys = 0; c->pend_valid = 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (now.QuadPart - c->sum_t >= c->sum_interval) boot_census_summary(c, now.QuadPart);
}
/* Hook: the flicker detector (TOTK_FLICKER_LOG=1) flagged DROP at its present number `flicker_present` (counted from 0; the
 * census counts from 1, so census frame = flicker present + 1 while every present was copied). Called from the present path. */
static inline __attribute__((noinline)) void boot_census_flicker_drop_slow(uint64_t flicker_present) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1) return;
    uint32_t cf = (uint32_t)flicker_present + 1;
    c->marks[c->nmarks % BOOT_CENSUS_MARKS] = cf; ++c->nmarks;
    FILE *f = c->summary_lines ? boot_census_open(c) : NULL;
    if (f) fprintf(f, "F flicker DROP at flicker present %llu (census frame %u; census is at frame %u)\n", (unsigned long long)flicker_present, cf, c->frame);
}
#define BOOT_CENSUS_FLICKER_DROP(p) boot_census_flicker_drop_slow(p)
/* water-work: the glitch scanner (TOTK_GLITCH_SCAN, host_gpu_draw_exec.h) found a one-present local flash in census frame
 * `cf` (the scanner records the census frame of every copied present, so no numbering offset applies). Same mark as a
 * flicker DROP: vanish groups starting within 2 frames of it print flicker-drop=yes. */
static inline __attribute__((noinline)) void boot_census_glitch_slow(uint32_t cf, const char *what) {
    BootCensus *c = &boot_census;
    if (c->mode == 0) boot_census_init(c);
    if (c->mode != 1 || !cf) return;
    c->marks[c->nmarks % BOOT_CENSUS_MARKS] = cf; ++c->nmarks;
    FILE *f = c->summary_lines ? boot_census_open(c) : NULL;
    if (f) fprintf(f, "F glitch scanner %s at census frame %u (census is at frame %u)\n", what ? what : "event", cf, c->frame);
}
#define BOOT_CENSUS_GLITCH(cf, what) boot_census_glitch_slow((cf), (what))
#define BOOT_CENSUS_PROGRAMS(vs, ps) boot_census_programs_slow((vs), (ps))
#define BOOT_CENSUS_COMPUTE(va, id, ssbo, n, rc, err) boot_census_compute_slow((va), (id), (ssbo), (n), (rc), (err))
#define BOOT_CENSUS_NOSTATE(ibuf, first, count, mode) boot_census_nostate_slow((ibuf), (first), (count), (mode))
#endif
