#ifndef TOTK_HOST_GPU_STAGE_DRAW_H
#define TOTK_HOST_GPU_STAGE_DRAW_H
/* Draw/clear recording for the Maxwell 3D command stage.
 *
 * Draws and clears are recorded in submission order as events, each with the
 * decoded register state, the constant-buffer bindings latched by
 * BIND_GROUP_CONSTANT_BUFFER, the instance index, and the number of memory
 * writes staged before it (so constant-buffer snapshots can overlay exactly the
 * writes that precede the draw). Execution happens after the whole submission
 * has validated (see boot_gpu_execute_events). */
#include "host_gpu_draw_state.h"
#include <stdlib.h>

typedef struct BootCopyParams {   /* NVB0B5 registers latched at LAUNCH_DMA */
    uint32_t launch, offset_in_hi, offset_in_lo, offset_out_hi, offset_out_lo, pitch_in, pitch_out, line_length, line_count;
    uint32_t remap, remap_const_a, remap_const_b;
    uint32_t dst_block, dst_width, dst_height, dst_depth, dst_layer, dst_origin;
    uint32_t src_block, src_width, src_height, src_depth, src_layer, src_origin;
    uint32_t sem_a, sem_b, sem_payload;
} BootCopyParams;

/* NVB1C0 (compute) state latched at SEND_SIGNALING_PCAS_B with SCHEDULE: the QMD itself is read
 * from guest memory when the event executes (after the staged writes that precede it, which is
 * how the source uploads QMDs inline). Register meanings: gpu-reference/clb1c0.h. */
typedef struct BootComputeParams {
    uint64_t qmd;                  /* SEND_PCAS_A << 8 */
    uint64_t program_region;       /* SET_PROGRAM_REGION_A/B */
    uint64_t tic_address, tsc_address; uint32_t tic_max, tsc_max;
    uint32_t bindless_cb;          /* SET_BINDLESS_TEXTURE constant buffer slot */
    uint32_t local_a, local_b, local_window, shared_window;
} BootComputeParams;

typedef struct BootDrawEvent {
    uint32_t kind;                 /* 1 draw, 2 clear, 3 copy-engine DMA, 4 compute dispatch,
                                    * 5 ZPASS counter reset, 6 ZPASS report (value words from write_index on) */
    uint32_t write_index;          /* staged writes preceding this event */
    uint32_t instance, clear_value;
    uint32_t clear_regs[8];        /* colour (4), depth, stencil, clear-surface control, pad;
                                    * draws: [0] SET_ZPASS_PIXEL_COUNT enable, [1] occlusion query slot + 1 (set at execution) */
    BootCbufBinding cbufs[5][18];
    BootCopyParams copy;
    BootComputeParams compute;
    BootDrawState state;
} BootDrawEvent;

typedef struct BootDrawRecorder {
    BootCbufBinding cbufs[5][18];  /* latched per pipeline bind group */
    uint32_t begin, in_draw, instance;
    BootDrawEvent *events; uint32_t count, capacity;
} BootDrawRecorder;

/* Largest event count of a recent submission: a new submission's array starts that large instead
 * of growing from 64 by repeated reallocation (each one copying every event recorded so far). */
static uint32_t boot_draw_event_hint;
static inline BootDrawEvent *boot_draw_event(BootDrawRecorder *d) {
    if (d->count == d->capacity) {
        uint32_t cap = d->capacity ? d->capacity * 2 : (boot_draw_event_hint > 64 ? boot_draw_event_hint : 64);
        if (cap > 65536 && d->capacity < 65536) cap = 65536;
        if (cap > 65536) return NULL;
        BootDrawEvent *e = realloc(d->events, (size_t)cap * sizeof *e);
        if (!e) return NULL;
        d->events = e; d->capacity = cap;
    }
    BootDrawEvent *e = &d->events[d->count++];
    if (d->count > boot_draw_event_hint) boot_draw_event_hint = d->count < 65536 ? (d->count + 1023) & ~1023u : 65536;
    memset(e, 0, sizeof *e);
    return e;
}

/* BIND_GROUP_CONSTANT_BUFFER(j): binds the buffer currently selected by
 * SET_CONSTANT_BUFFER_SELECTOR_A/B/C (size, address high, address low) to a slot. */
static inline int boot_draw_bind_cbuf(BootDrawRecorder *d, const uint32_t *regs, uint32_t method, uint32_t value) {
    unsigned group = (method - 0x2410) / 32, slot = (value >> 4) & 31;
    if (group >= 5 || slot >= 18) return 0;
    BootCbufBinding *b = &d->cbufs[group][slot];
    if (value & 1) {
        b->size = regs[0x2380 / 4] & 0x1ffff;
        b->address = ((uint64_t)(regs[0x2384 / 4] & 0xff) << 32) | regs[0x2388 / 4];
        b->valid = 1;
    } else memset(b, 0, sizeof *b);
    return 1;
}

static inline int boot_draw_begin(BootDrawRecorder *d, uint32_t value) {
    if (d->in_draw) return 0;
    uint32_t mode = (value >> 26) & 3;   /* INSTANCE_ID: FIRST, SUBSEQUENT, UNCHANGED */
    if (mode == 0) d->instance = 0; else if (mode == 1) ++d->instance; else if (mode != 2) return 0;
    d->begin = value; d->in_draw = 1;
    return 1;
}
static inline int boot_draw_end(BootDrawRecorder *d) { if (!d->in_draw) return 0; d->in_draw = 0; return 1; }

/* DRAW_VERTEX_ARRAY / DRAW_INDEX_BUFFER inside BEGIN..END. */
/* Memory barrier points of the 3D stream (WAIT_FOR_IDLE, INVALIDATE_SHADER_CACHES(_NO_WFI),
 * FLUSH_PENDING_WRITES, INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI): the guest orders shader stores before
 * later shader reads with them. Noted here and carried by the next draw (clear_regs[2], unused by
 * draws), so execution can order guest-memory stores of earlier draws before it (UAV barrier,
 * host_gpu_render_draw.h). One stage records at a time (window lock), so a module static suffices. */
static uint32_t boot_draw_barrier_pending;
static inline void boot_draw_note_barrier(void) { boot_draw_barrier_pending = 1; }
static inline int boot_draw_record(BootDrawRecorder *d, const uint32_t *regs, uint32_t write_index, int indexed, uint32_t count) {
    if (!d->in_draw) return 0;
    BootDrawEvent *e = boot_draw_event(d); if (!e) return 0;
    e->kind = 1; e->write_index = write_index; e->instance = d->instance;
    memcpy(e->cbufs, d->cbufs, sizeof e->cbufs);
    boot_draw_state_decode(regs, d->begin, &e->state);
    e->state.indexed = (uint32_t)indexed; e->state.count = count;
    e->state.first = indexed ? regs[0x17dc / 4] : regs[0xd74 / 4];
    e->clear_regs[0] = regs[0x1514 / 4] & 1;   /* SET_ZPASS_PIXEL_COUNT: this draw's passing samples count */
    e->clear_regs[2] = boot_draw_barrier_pending; boot_draw_barrier_pending = 0;   /* a guest memory barrier precedes this draw */
    return 1;
}
/* Occlusion counter points, in order with the draws (host_gpu_draw_exec.h resolves them once the draws
 * before them ran on the host GPU): CLEAR_REPORT_VALUE ZPASS_PIXEL_CNT, and a ZPASS report whose value
 * words are the staged writes starting at write_index. */
static inline int boot_draw_counter_point(BootDrawRecorder *d, uint32_t write_index, uint32_t kind) {
    BootDrawEvent *e = boot_draw_event(d); if (!e) return 0;
    e->kind = kind; e->write_index = write_index;
    return 1;
}
static inline int boot_draw_clear(BootDrawRecorder *d, const uint32_t *regs, uint32_t write_index, uint32_t value) {
    if (d->in_draw) return 0;
    BootDrawEvent *e = boot_draw_event(d); if (!e) return 0;
    e->kind = 2; e->write_index = write_index; e->clear_value = value;
    for (unsigned k = 0; k < 4; ++k) e->clear_regs[k] = regs[(0xd80 + 4 * k) / 4];
    e->clear_regs[4] = regs[0xd90 / 4]; e->clear_regs[5] = regs[0xda0 / 4]; e->clear_regs[6] = regs[0x10f8 / 4];
    boot_draw_state_decode(regs, 0, &e->state);
    return 1;
}
/* NVB0B5 LAUNCH_DMA: latch the copy engine registers (index = method / 4). */
static inline int boot_draw_copy(BootDrawRecorder *d, const uint32_t *regs, uint32_t write_index, uint32_t value) {
    BootDrawEvent *e = boot_draw_event(d); if (!e) return 0;
    BootCopyParams *c = &e->copy;
    e->kind = 3; e->write_index = write_index;
    c->launch = value;
    c->offset_in_hi = regs[0x400 / 4]; c->offset_in_lo = regs[0x404 / 4]; c->offset_out_hi = regs[0x408 / 4]; c->offset_out_lo = regs[0x40c / 4];
    c->pitch_in = regs[0x410 / 4]; c->pitch_out = regs[0x414 / 4]; c->line_length = regs[0x418 / 4]; c->line_count = regs[0x41c / 4];
    c->remap_const_a = regs[0x700 / 4]; c->remap_const_b = regs[0x704 / 4]; c->remap = regs[0x708 / 4];
    c->dst_block = regs[0x70c / 4]; c->dst_width = regs[0x710 / 4]; c->dst_height = regs[0x714 / 4]; c->dst_depth = regs[0x718 / 4];
    c->dst_layer = regs[0x71c / 4]; c->dst_origin = regs[0x720 / 4];
    c->src_block = regs[0x728 / 4]; c->src_width = regs[0x72c / 4]; c->src_height = regs[0x730 / 4]; c->src_depth = regs[0x734 / 4];
    c->src_layer = regs[0x738 / 4]; c->src_origin = regs[0x73c / 4];
    c->sem_a = regs[0x240 / 4]; c->sem_b = regs[0x244 / 4]; c->sem_payload = regs[0x248 / 4];
    return 1;
}
/* NVB1C0 SEND_SIGNALING_PCAS_B with SCHEDULE: latch the compute registers (index = method / 4). */
static inline int boot_draw_compute(BootDrawRecorder *d, const uint32_t *regs, uint32_t write_index) {
    BootDrawEvent *e = boot_draw_event(d); if (!e) return 0;
    BootComputeParams *c = &e->compute;
    e->kind = 4; e->write_index = write_index;
    c->qmd = (uint64_t)regs[0x2b4 / 4] << 8;
    c->program_region = ((uint64_t)(regs[0x1608 / 4] & 0xff) << 32) | regs[0x160c / 4];
    c->tsc_address = ((uint64_t)(regs[0x155c / 4] & 0xff) << 32) | regs[0x1560 / 4]; c->tsc_max = regs[0x1564 / 4] & 0xfffff;
    c->tic_address = ((uint64_t)(regs[0x1574 / 4] & 0xff) << 32) | regs[0x1578 / 4]; c->tic_max = regs[0x157c / 4] & 0x3fffff;
    c->bindless_cb = regs[0x2608 / 4] & 7;
    c->local_a = regs[0x790 / 4]; c->local_b = regs[0x794 / 4]; c->local_window = regs[0x77c / 4]; c->shared_window = regs[0x214 / 4];
    return 1;
}
#endif
