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
 * RD3_VIRQ_STORM_WINDOW_NS disable the source; the poll takes over. The
 * window runs on the time the handler sees an IV, so a backlog drained at
 * once after the handler stalled could look like a storm (ECC review). A
 * working source cannot build one: it raises once and waits for its ack
 * (the INT_STATUS latch, dal_irq_service_ack_generic; tests/sim_dc_irq.inc
 * stalls the handler 100 frames and finds one IV), so at most one IV of it
 * waits in the ring. Still, the IVs of a source that come within
 * RD3_VIRQ_DRAIN_NS after one that ended a quiet stretch of
 * RD3_VIRQ_STALL_NS are counted apart ('drained', 'drains': the stall is
 * visible in the report) and kept out of the window, at most
 * RD3_VIRQ_DRAIN_MAX of them, so a real storm after a stall is cut at most
 * that many IVs later. The IV's own 48-bit IH timestamp is not used: its
 * clock (the IH's, not mach_absolute_time) is nowhere established here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_VBL_IRQ_H
#define RDNA3_VBL_IRQ_H

#include <stdint.h>

#define RD3_VIRQ_FALLBACK_FRAMES  3u              /* frames without an interrupt before the poll delivers */
#define RD3_VIRQ_STORM_WINDOW_NS  100000000ull    /* 100 ms */
#define RD3_VIRQ_STORM_MAX        64u             /* per source per window: 640 Hz (a 240 Hz panel raises 24) */

#define RD3_VIRQ_STALL_NS         50000000ull     /* 50 ms without an IV of the source: longer than any frame (20 Hz) */
#define RD3_VIRQ_DRAIN_NS         10000000ull     /* 10 ms after such a quiet stretch: the backlog drains */
#define RD3_VIRQ_DRAIN_MAX        4u              /* IVs kept out of the window after one quiet stretch */

/* one source's interrupts in the current storm window */
struct rd3_virq_storm {
    uint64_t win_start_ns;
    uint32_t in_win;
    uint32_t storms;       /* windows that went over the limit */
    uint64_t last_ns;      /* the previous IV of the source (0: none yet) */
    uint64_t drain_end_ns; /* IVs before this one are a drained backlog (0: none) */
    uint64_t drained;      /* IVs counted as a drained backlog, not in the window */
    uint32_t drains;       /* quiet stretches followed by such a backlog */
    uint32_t drain_left;   /* IVs that may still be counted as drained after the last quiet stretch */
};

/* one interrupt of a source at 'now_ns' (monotonic, the handler's clock): 1
 * when it is the one that goes over RD3_VIRQ_STORM_MAX in the window (the
 * caller disables the source, once), 0 otherwise. The IVs right after a
 * quiet stretch (a stalled handler draining the ring) are counted in
 * 'drained' and not in the window. */
static inline int rd3_virq_storm_note(struct rd3_virq_storm *s, uint64_t now_ns)
{
    const uint64_t last = s->last_ns;
    s->last_ns = now_ns ? now_ns : 1;
    if (last && now_ns >= last && now_ns - last >= RD3_VIRQ_STALL_NS) {
        /* the first IV after a quiet stretch: what follows at once may be
         * the rest of a backlog; the window starts again with this one */
        s->drain_end_ns = now_ns + RD3_VIRQ_DRAIN_NS;
        s->drain_left = RD3_VIRQ_DRAIN_MAX;
        s->win_start_ns = 0;
    } else if (s->drain_end_ns) {
        if (now_ns >= last && now_ns < s->drain_end_ns && s->drain_left) {
            if (s->drain_left == RD3_VIRQ_DRAIN_MAX)
                s->drains++;   /* the first drained IV of this quiet stretch */
            s->drain_left--;
            s->drained++;
            return 0;
        }
        s->drain_end_ns = 0;
        s->drain_left = 0;
    }
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

/* After a wake (amdgpu_dm_irq_resume_early) or a mode-1 reset
 * (amdgpu_irq_gpu_reset_resume_helper) amdgpu sets the sources again on a
 * DCN its resume / reset path has just brought back (dm_resume). Here the
 * decision is per head, a mask (bit n = head n; 'all' = every head the
 * table holds): a head gets its sources written again only when it is up
 * after the restore (dcn32_state_restore with rdna3resume=1) or the check
 * alone - 'lost' the heads still lost AFTER it, 'live' the heads checked
 * and running. Nothing lost (or nothing checked): every head. Otherwise a
 * head still lost gets nothing (a head with no snapshot, or none in the
 * table, is not lost), and when no head runs the DCN is taken as down (0:
 * no OTG / HUBP register is written at all, an un-posted, gated DCN; the
 * sources are forgotten as off until a later wake or reset brings it
 * back). ECC review round 2: the reset's
 * rule was "up when the restore ran", so a restore that failed (2 heads
 * still lost) wrote the enables of heads that were down, and the wake
 * looked only at whether the GPU resume ran, not at the restore. */
static inline uint32_t rd3_virq_heads_up(uint32_t all, uint32_t live, uint32_t lost)
{
    if (!(lost & all))
        return all;
    if (!(live & ~lost & all))
        return 0;
    return all & ~lost;
}
/* the wake: the card lost its power or was suspended (its IPs down) and
 * was not resumed (rdna3resume=0, or resumeAfterPowerLoss failed): the DCN
 * is down whatever the check says (0); otherwise per head as above, with
 * the restore's result when rdna3resume=1 ran it, the check's otherwise */
static inline uint32_t rd3_virq_wake_heads_up(int power_lost, int suspended, int resume_on, int resumed,
                                              uint32_t all, uint32_t live, uint32_t lost)
{
    if ((power_lost || suspended) && !(resume_on && resumed))
        return 0;
    return rd3_virq_heads_up(all, live, lost);
}

/* ECC review round 2: a head that went down (the resync above left it out)
 * keeps the sources it had enabled as 'stale' (bit k at stale_inst[k]):
 * nothing is written while it is down, but a DCN that kept its state keeps
 * raising them. At the head's next sync while it is up each stale source
 * is written off (acked first) unless it is desired again at the same
 * instance (then the sync writes its enable anyway). Returns the mask to
 * turn off; 'inst' the head's instance per kind now. */
static inline uint32_t rd3_virq_stale_off(uint32_t stale, uint32_t desired, const uint32_t *stale_inst,
                                          const uint32_t *inst, unsigned kinds)
{
    uint32_t off = 0;
    for (unsigned k = 0; k < kinds && k < 32; k++) {
        const uint32_t bit = 1u << k;
        if ((stale & bit) && (!(desired & bit) || stale_inst[k] != inst[k]))
            off |= bit;
    }
    return off;
}

/* why the framebuffer never asked the core to REGISTER (RDNA3,FBStats
 * virq_noreg; tools/triage_reports.py FB_N42_NOREG) */
#define RD3FB_VIRQ_NOREG_NONE      0u   /* it asked (virq_reg_tries), or will */
#define RD3FB_VIRQ_NOREG_ARG       1u   /* rdna3vblirq=0 */
#define RD3FB_VIRQ_NOREG_NODCN     2u   /* no DCN head (the console timer) */
#define RD3FB_VIRQ_NOREG_NOVBL     3u   /* no VBL thread call */
#define RD3FB_VIRQ_NOREG_NOCALL    4u   /* no symbol / thread call for the interrupt */
#define RD3FB_VIRQ_NOREG_STOPPING  5u   /* the framebuffer was stopping at its first try */

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
