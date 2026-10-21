#ifndef TOTK_CORE_SCHEDULE_H
#define TOTK_CORE_SCHEDULE_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

/* Not yet connected to BootContext: runtime handoff integration is required. */

/* Pure queue operations. The caller supplies runnable state and commits CPU
 * context switches separately. Preferred affinity is never an active core. */
typedef struct CoreScheduleThread {
    uint64_t affinity, order;
    uint32_t priority, preferred, active, runnable;
    uint64_t last_scheduled;
} CoreScheduleThread;
#define CORE_SCHEDULE_NONE UINT32_MAX

static inline int core_schedule_before(const CoreScheduleThread *a,
                                       const CoreScheduleThread *b) {
    return a->priority < b->priority ||
        (a->priority == b->priority && a->order < b->order);
}

static inline unsigned core_schedule_front(const CoreScheduleThread *threads,
                                           unsigned count, unsigned core) {
    unsigned best = CORE_SCHEDULE_NONE;
    for (unsigned i = 0; i < count; ++i) {
        const CoreScheduleThread *t = &threads[i];
        if (!t->runnable || t->active != core) continue;
        if (best == CORE_SCHEDULE_NONE || core_schedule_before(t, &threads[best])) best = i;
    }
    return best;
}

static inline int core_schedule_valid(const CoreScheduleThread *threads,
                                     unsigned count, uint64_t allowed) {
    for (unsigned i = 0; i < count; ++i) {
        const CoreScheduleThread *t = &threads[i];
        if (!t->affinity || (t->affinity & ~allowed) || t->priority >= 64) return 0;
        if (t->preferred != CORE_SCHEDULE_NONE &&
            (t->preferred >= 64 || !(t->affinity & (UINT64_C(1) << t->preferred)))) return 0;
        if (t->active >= 64 || !(t->affinity & (UINT64_C(1) << t->active))) return 0;
    }
    return 1;
}

/* Preserve a valid active core across an ideal-core change. If affinity removes
 * it, use the requested ideal core or the first allowed core. */
static inline int core_schedule_affinity(CoreScheduleThread *t, uint64_t mask,
                                         unsigned preferred, uint64_t allowed) {
    if (!mask || (mask & ~allowed) || (preferred != CORE_SCHEDULE_NONE &&
        (preferred >= 64 || !(mask & (UINT64_C(1) << preferred))))) return 0;
    unsigned active = t->active;
    if (active >= 64 || !(mask & (UINT64_C(1) << active))) {
        active = preferred;
        if (active == CORE_SCHEDULE_NONE) {
            active = 0;
            while (!(mask & (UINT64_C(1) << active))) ++active;
        }
    }
    t->affinity = mask; t->preferred = preferred; t->active = active;
    return 1;
}

/* A suggestion must not be its source core's selected thread. Priority 0/1
 * source-core work excludes migration, matching the kernel migration cutoff.
 * Returns a queue index, not an acknowledged execution or completed wait. */
static inline unsigned core_schedule_suggestion(const CoreScheduleThread *threads,
                                                unsigned count, unsigned core) {
    unsigned best = CORE_SCHEDULE_NONE;
    if (core >= 64) return best;
    for (unsigned i = 0; i < count; ++i) {
        const CoreScheduleThread *t = &threads[i];
        if (!t->runnable || t->active == core || !(t->affinity & (UINT64_C(1) << core))) continue;
        unsigned source = core_schedule_front(threads, count, t->active);
        if (source == i || (source != CORE_SCHEDULE_NONE && threads[source].priority < 2)) continue;
        if (best == CORE_SCHEDULE_NONE || core_schedule_before(t, &threads[best])) best = i;
    }
    return best;
}

/* Insertion at the destination front preserves its existing FIFO order.
 * Validate overflow before touching any queue. Scheduling age is independent. */
static inline int core_schedule_move_front(CoreScheduleThread *threads, unsigned count,
                                           unsigned index, unsigned core) {
    if (index >= count || core >= 64 ||
        !(threads[index].affinity & (UINT64_C(1) << core))) return 0;
    uint64_t first = threads[index].order;
    for (unsigned i = 0; i < count; ++i) {
        if (i == index || threads[i].active != core ||
            threads[i].priority != threads[index].priority || !threads[i].runnable) continue;
        if (threads[i].order == UINT64_MAX) return 0;
        if (threads[i].order < first) first = threads[i].order;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (i != index && threads[i].active == core && threads[i].runnable &&
            threads[i].priority == threads[index].priority) ++threads[i].order;
    }
    threads[index].active = core;
    threads[index].order = first;
    return 1;
}

/* Fill one idle core without leaving the donor idle. A queued suggestion is
 * preferred; moving a donor's selected thread requires runnable replacement. */
static inline int core_schedule_fill_idle(CoreScheduleThread *threads, unsigned count,
                                          unsigned core, unsigned *selected) {
    if (core >= 64 || core_schedule_front(threads, count, core) != CORE_SCHEDULE_NONE) return 0;
    unsigned candidate = core_schedule_suggestion(threads, count, core);
    if (candidate == CORE_SCHEDULE_NONE) {
        for (unsigned i = 0; i < count; ++i) {
            const CoreScheduleThread *t = &threads[i];
            if (!t->runnable || t->active == core || t->priority < 2 ||
                !(t->affinity & (UINT64_C(1) << core)) ||
                core_schedule_front(threads, count, t->active) != i) continue;
            unsigned replacement = CORE_SCHEDULE_NONE;
            for (unsigned j = 0; j < count; ++j)
                if (j != i && threads[j].runnable && threads[j].active == t->active)
                    replacement = j;
            if (replacement != CORE_SCHEDULE_NONE && (candidate == CORE_SCHEDULE_NONE ||
                core_schedule_before(t, &threads[candidate]))) candidate = i;
        }
    }
    if (candidate == CORE_SCHEDULE_NONE) { *selected = candidate; return 1; }
    uint64_t last = 0;
    for (unsigned i = 0; i < count; ++i) if (threads[i].order > last) last = threads[i].order;
    if (last == UINT64_MAX) return 0;
    threads[candidate].active = core;
    threads[candidate].order = last + 1;
    *selected = candidate;
    return 1;
}

/* Yield rotates the current core's FIFO queue. Migration is a distinct mode;
 * callers must pass false for SleepThread(0), true for SleepThread(-1).
 * The caller may interleave other core fronts to represent concurrent cores.
 * Overflow fails before mutation so queue order can never silently wrap. */
static inline int core_schedule_yield(CoreScheduleThread *threads, unsigned count,
                                     unsigned current, int migrate, unsigned *selected) {
    if (current >= count || !threads[current].runnable || threads[current].active >= 64) return 0;
    uint64_t last = 0;
    for (unsigned i = 0; i < count; ++i) if (threads[i].order > last) last = threads[i].order;
    if (last >= UINT64_MAX - 1) return 0;
    unsigned core = threads[current].active;
    threads[current].order = last + 1;
    unsigned next = core_schedule_front(threads, count, core);
    if (migrate) {
        unsigned suggestion = core_schedule_suggestion(threads, count, core);
        if (suggestion != CORE_SCHEDULE_NONE &&
            threads[suggestion].priority <= threads[current].priority &&
            (threads[suggestion].priority < threads[current].priority || next == current ||
             threads[next].last_scheduled >= threads[suggestion].last_scheduled)) {
            if (!core_schedule_move_front(threads, count, suggestion, core)) return 0;
            next = core_schedule_front(threads, count, core);
        }
    }
    *selected = next;
    return 1;
}
#endif
