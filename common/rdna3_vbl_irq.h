/*
 * rdna3_vbl_irq.h - the decisions of the vblank / page-flip interrupt path
 * (DRIVER-PLAN N42), pure C99 for the core kext, the framebuffer and the
 * host test (tests/test_vbl_irq.c).
 *
 * amdgpu's model: the DCN's VSTARTUP interrupt (DC_IRQ_SOURCE_VBLANKn) runs
 * dm_crtc_high_irq -> drm_crtc_handle_vblank, the HUBP flip interrupt
 * (DC_IRQ_SOURCE_PFLIPn) runs dm_pflip_high_irq, which sends the page-flip
 * event at flip done. Here the core's handler only counts, timestamps and
 * calls the framebuffer, whose HIGH ONCE thread call delivers IOGraphics'
 * VBL; the framebuffer's OTG frame-counter poll stays as the fallback when
 * no interrupt came for RD3_VIRQ_FALLBACK_FRAMES frames.
 *
 * Storm guard (no amdgpu counterpart: there a storm is a kernel splat):
 * more than RD3_VIRQ_STORM_MAX interrupts of one source within
 * RD3_VIRQ_STORM_WINDOW_NS disable the source; the poll takes over.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_VBL_IRQ_H
#define RDNA3_VBL_IRQ_H

#include <stdint.h>

#define RD3_VIRQ_FALLBACK_FRAMES  3u              /* frames without an interrupt before the poll delivers */
#define RD3_VIRQ_STORM_WINDOW_NS  100000000ull    /* 100 ms */
#define RD3_VIRQ_STORM_MAX        64u             /* per source per window: 640 Hz (a 240 Hz panel raises 24) */

/* one source's interrupts in the current storm window */
struct rd3_virq_storm {
    uint64_t win_start_ns;
    uint32_t in_win;
    uint32_t storms;       /* windows that went over the limit */
};

/* one interrupt of a source at 'now_ns' (monotonic): 1 when it is the one
 * that goes over RD3_VIRQ_STORM_MAX in the window (the caller disables the
 * source, once), 0 otherwise */
static inline int rd3_virq_storm_note(struct rd3_virq_storm *s, uint64_t now_ns)
{
    if (!s->win_start_ns || now_ns < s->win_start_ns || now_ns - s->win_start_ns >= RD3_VIRQ_STORM_WINDOW_NS) {
        s->win_start_ns = now_ns ? now_ns : 1;
        s->in_win = 0;
    }
    if (++s->in_win == RD3_VIRQ_STORM_MAX + 1) {
        s->storms++;
        return 1;
    }
    return 0;
}

/* latency accumulator (interrupt handler -> delivery), microseconds */
struct rd3_lat {
    uint64_t n, sum_us, max_us;
};
static inline void rd3_lat_add(struct rd3_lat *l, uint64_t us)
{
    l->n++;
    l->sum_us += us;
    if (us > l->max_us)
        l->max_us = us;
}
static inline uint64_t rd3_lat_avg(const struct rd3_lat *l)
{
    return l->n ? l->sum_us / l->n : 0;
}

/* the framebuffer's poll: must it deliver the VBL itself? 'live' = the
 * interrupt path is registered and enabled for the head; 'since' = the
 * last interrupt delivery, or when the path went live (0: never live);
 * 'period' one frame. 1 = deliver (no interrupt path, or it fell silent
 * for 'nframes' frames: the fallback), 0 = the interrupt delivers. All in
 * one clock (absolute time units). */
static inline int rd3_virq_poll_due(int live, uint64_t since, uint64_t now, uint64_t period, uint32_t nframes)
{
    if (!live || !since || !period)
        return 1;
    if (now < since)
        return 0;
    return now - since >= (uint64_t)nframes * period;
}

/* when the poll looks next while the interrupt path delivers: just after
 * the fallback would be due */
static inline uint64_t rd3_virq_watch_at(uint64_t since, uint64_t period, uint32_t nframes)
{
    return since + (uint64_t)nframes * period + period / 4;
}

/* a latch wait that started at pflip count 'seq0' and ended with the flip
 * latched at count 'seq1': 1 = the pflip interrupt of this flip came (the
 * wait was ended by it), 0 = the poll / vblank saw the latch first */
static inline int rd3_virq_pflip_seen(uint32_t seq0, uint32_t seq1)
{
    return seq1 != seq0;
}

#endif
