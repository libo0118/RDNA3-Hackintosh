/*
 * nvmtl_handoff.h - IOSurface hand-offs before the GPU write was in the surface (DRIVER-PLAN N59).
 *
 * Plain C (no Objective-C, no Vulkan): tests/test_handoff.c checks the counters and the SUMMARY line exactly as
 * NVMTLEventSync.m uses them.
 *
 * Metal's contract: once a command buffer is SCHEDULED (its scheduled handlers ran, waitUntilScheduled returned) a
 * consumer may take the surfaces it writes - Core Animation hands a layer's IOSurface to WindowServer then, and
 * WindowServer presents its own buffers then - because Apple's drivers order every later GPU or CPU access of the
 * surface after the write in the kernel. The plugin cannot: its GPU writes reach the IOSurface only when the part that
 * wrote them is PUBLISHED after the GPU finished (-nvmtlSurfaceOut: a CPU copy-out of a private image, or the
 * family-VRAM dirty mark), and it marks the command buffer scheduled when the first part is submitted, before that.
 * A consumer that takes the surface in between reads the frame it held before: two or three frames back in a ring of
 * buffers (card run 2026-10-26: animations step back).
 *
 * Counted per process for every command buffer that writes an IOSurface, at the publish of its last such part:
 *   cbs        command buffers that wrote IOSurfaces
 *   early      of them, the scheduled point was OBSERVED before the publish (by=handler: a scheduled handler ran;
 *              by=wait: waitUntilScheduled returned); gap = publish - that moment
 *   held       NVMTL_SCHED_AFTER_PUBLISH=1: the scheduled point held until the publish (held_us: how long)
 * `vk: handoff: SUMMARY ...` (nvlog_uncapped, the shared 10 s timer) when a counter moved.
 */
#ifndef NVMTL_HANDOFF_H
#define NVMTL_HANDOFF_H

#include <stdint.h>
#include <stdio.h>

enum { NVMTL_HO_NONE = 0, NVMTL_HO_HANDLER = 1, NVMTL_HO_WAIT = 2 };

typedef struct {
    uint64_t cbs, early, by_handler, by_wait, gap_ns, gap_max_ns, held, held_ns;
} nvmtl_ho_stats;

/* the publish of a command buffer's last IOSurface-writing part: `observed` how its scheduled point was seen before
 * it (NVMTL_HO_*), `gap_ns` since then; `held_ns` > 0 when the knob held the scheduled point that long */
static inline void nvmtl_ho_publish(nvmtl_ho_stats *s, int observed, uint64_t gap_ns, int held, uint64_t held_ns)
{
    __atomic_fetch_add(&s->cbs, 1, __ATOMIC_RELAXED);
    if (held) { __atomic_fetch_add(&s->held, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&s->held_ns, held_ns, __ATOMIC_RELAXED); }
    if (observed == NVMTL_HO_NONE) return;
    __atomic_fetch_add(&s->early, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(observed == NVMTL_HO_HANDLER ? &s->by_handler : &s->by_wait, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->gap_ns, gap_ns, __ATOMIC_RELAXED);
    uint64_t m = __atomic_load_n(&s->gap_max_ns, __ATOMIC_RELAXED);
    while (gap_ns > m && !__atomic_compare_exchange_n(&s->gap_max_ns, &m, gap_ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* the SUMMARY body; 0 when nothing was counted */
static inline int nvmtl_ho_format(const nvmtl_ho_stats *s, int hold_on, char *out, size_t cap)
{
    const uint64_t cbs = __atomic_load_n(&s->cbs, __ATOMIC_RELAXED), early = __atomic_load_n(&s->early, __ATOMIC_RELAXED);
    const uint64_t held = __atomic_load_n(&s->held, __ATOMIC_RELAXED);
    if (!cbs || !cap) return 0;
    snprintf(out, cap, "cbs=%llu early=%llu by_handler=%llu by_wait=%llu gap_us_avg=%llu gap_us_max=%llu held=%llu held_us_avg=%llu hold=%d",
             (unsigned long long)cbs, (unsigned long long)early, (unsigned long long)__atomic_load_n(&s->by_handler, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s->by_wait, __ATOMIC_RELAXED),
             (unsigned long long)(early ? __atomic_load_n(&s->gap_ns, __ATOMIC_RELAXED) / early / 1000u : 0),
             (unsigned long long)(__atomic_load_n(&s->gap_max_ns, __ATOMIC_RELAXED) / 1000u), (unsigned long long)held,
             (unsigned long long)(held ? __atomic_load_n(&s->held_ns, __ATOMIC_RELAXED) / held / 1000u : 0), hold_on);
    return 1;
}

/* NVMTL_SCHED_AFTER_PUBLISH: on when the value starts with '1' */
static inline int nvmtl_ho_hold_on(const char *v) { return v && v[0] == '1'; }

#endif
