/*
 * test_vbl_irq.c - common/rdna3_vbl_irq.h on the host (DRIVER-PLAN N42:
 * IOGraphics' VBL from the DCN vblank interrupt, the OTG poll as fallback).
 *
 * The model: a head at 60 Hz; the vblank interrupt reaches the framebuffer
 * a little after each frame start while the path works; the framebuffer's
 * poll thread call looks every frame when it delivers itself, otherwise
 * only at rd3_virq_watch_at. Checked: while the interrupt works every frame
 * is delivered once and the poll delivers nothing; when it falls silent the
 * poll delivers from at most RD3_VIRQ_FALLBACK_FRAMES frames later on; when
 * it comes back the poll stops; a storm is cut at the limit, once per window.
 *
 * ECC review (N42 round 1): a backlog drained at once after the handler
 * stalled is not a storm (counted apart), a storm right after a stall is
 * still cut; the sources are written again after a wake / a mode-1 reset
 * only on a DCN that is back; the framebuffer's stop never leaves a
 * registration in the core (every interleaving of a REGISTER from a running
 * VBL thread call with stop(), and the order before the review failing).
 * Round 2: "up" per head from the restore's result (a failed restore is
 * down), the sources a head left enabled while down turned off at its next
 * up sync.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_vbl_irq.h"
#include "rdna3_fb_policy.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { PERIOD = 16667, IRQ_LAT = 40, FRAMES = 600, SILENT_FROM = 200, SILENT_TO = 300 };

struct fb {
    int live;
    uint64_t since;        /* last interrupt delivery, or when the path went live */
    uint64_t next_poll;
    uint32_t irq_deliv, poll_deliv, fallbacks;
    int64_t last_frame;    /* the last frame delivered */
    uint32_t dup, gap_max;
};

static void deliver(struct fb *f, int64_t frame, int from_irq)
{
    if (frame == f->last_frame) {
        f->dup++;
        return;
    }
    if (f->last_frame >= 0 && (uint32_t)(frame - f->last_frame) > f->gap_max)
        f->gap_max = (uint32_t)(frame - f->last_frame);
    f->last_frame = frame;
    if (from_irq)
        f->irq_deliv++;
    else
        f->poll_deliv++;
}

static void run(int path_live, struct fb *f)
{
    memset(f, 0, sizeof(*f));
    f->last_frame = -1;
    f->live = path_live;
    f->since = path_live ? 1 : 0;
    f->next_poll = PERIOD / 2;
    for (uint64_t t = 1; t < (uint64_t)FRAMES * PERIOD; t++) {
        const int64_t frame = (int64_t)(t / PERIOD);
        /* the interrupt: IRQ_LAT after each frame start, unless silent */
        if (f->live && t % PERIOD == IRQ_LAT && !(frame >= SILENT_FROM && frame < SILENT_TO)) {
            deliver(f, frame, 1);
            f->since = t;
        }
        if (t == f->next_poll) {
            if (rd3_virq_poll_due(f->live, f->since, t, PERIOD, RD3_VIRQ_FALLBACK_FRAMES)) {
                if (f->live)
                    f->fallbacks++;
                deliver(f, frame, 0);
                f->next_poll = t + PERIOD;
            } else {
                f->next_poll = rd3_virq_watch_at(f->since, PERIOD, RD3_VIRQ_FALLBACK_FRAMES);
                if (f->next_poll <= t)
                    f->next_poll = t + 1;
            }
        }
    }
}

/* ---- the framebuffer's stop against a REGISTER in flight ----
 * R (vblFire -> vblDeliver -> virqRegister): R0 the stopping test (and,
 * fixed, the in-flight count, under fLock), R1 the core's REGISTER, R2 the
 * count dropped. S (stop): S0 fStopping set (under fLock), S1 the wait for
 * the count (fixed only; blocks while it is not 0), S2 virqUnregister (only
 * when the framebuffer saw its registration), S3 after the timers are
 * drained: unregister again if a registration is still there (fixed only).
 * At the end the core must hold no registration. */
struct stopm {
    int fixed;
    int stopping, inflight, fb_reg, core_reg, r_go, late;
    int r_pc, s_pc;
};

static int stop_step_r(struct stopm *m)
{
    switch (m->r_pc) {
    case 0:
        m->r_go = m->fixed ? rd3fb_wake_enter(&m->inflight, m->stopping) : !m->stopping;
        m->r_pc = m->r_go ? 1 : 3;
        return 1;
    case 1:
        m->core_reg = 1;
        m->fb_reg = 1;
        m->r_pc = 2;
        return 1;
    case 2:
        if (m->fixed)
            rd3fb_wake_leave(&m->inflight);
        m->r_pc = 3;
        return 1;
    }
    return 0;
}

static int stop_step_s(struct stopm *m)
{
    switch (m->s_pc) {
    case 0:
        m->stopping = 1;
        m->s_pc = 1;
        return 1;
    case 1:
        if (m->fixed && rd3fb_stop_wake_wait(m->inflight, 0) != 1)
            return 0;   /* blocked: the REGISTER in flight first */
        m->s_pc = 2;
        return 1;
    case 2:
        if (m->fb_reg)
            m->core_reg = m->fb_reg = 0;
        m->s_pc = 3;
        return 1;
    case 3:
        if (m->fixed && m->fb_reg) {
            m->late++;
            m->core_reg = m->fb_reg = 0;
        }
        m->s_pc = 4;
        return 1;
    }
    return 0;
}

/* every interleaving (bit i of 'order': step i taken by R when it can):
 * the number of schedules that end with a registration left in the core */
static uint32_t stop_explore(int fixed, uint32_t *runs, uint32_t *lates)
{
    uint32_t bad = 0, order;
    *runs = *lates = 0;
    for (order = 0; order < (1u << 8); order++) {
        struct stopm m;
        int step = 0, guard = 0;
        memset(&m, 0, sizeof(m));
        m.fixed = fixed;
        while ((m.r_pc < 3 || m.s_pc < 4) && guard++ < 32) {
            const int want_r = (order >> (step & 7)) & 1;
            int did = want_r ? stop_step_r(&m) : stop_step_s(&m);
            if (!did)
                did = want_r ? stop_step_s(&m) : stop_step_r(&m);
            if (!did)
                break;   /* both blocked: a deadlock (counted as bad below) */
            step++;
        }
        (*runs)++;
        *lates += (uint32_t)m.late;
        if (m.core_reg || m.r_pc < 3 || m.s_pc < 4)
            bad++;
    }
    return bad;
}

int main(void)
{
    struct fb f;

    /* no interrupt path: the poll delivers every frame */
    run(0, &f);
    CHECK(f.irq_deliv == 0 && f.poll_deliv >= FRAMES - 2 && f.dup == 0 && f.gap_max == 1);

    /* the interrupt path, silent for 100 frames in the middle */
    run(1, &f);
    CHECK(f.dup == 0);
    CHECK(f.irq_deliv == FRAMES - (SILENT_TO - SILENT_FROM));
    /* the silence: the poll from RD3_VIRQ_FALLBACK_FRAMES frames on */
    CHECK(f.poll_deliv >= SILENT_TO - SILENT_FROM - RD3_VIRQ_FALLBACK_FRAMES - 1 &&
          f.poll_deliv <= SILENT_TO - SILENT_FROM);
    CHECK(f.fallbacks == f.poll_deliv);
    CHECK(f.gap_max <= RD3_VIRQ_FALLBACK_FRAMES + 1);
    printf("test_vbl_irq: %u by interrupt, %u by the poll (fallback), longest gap %u frames\n", f.irq_deliv,
           f.poll_deliv, f.gap_max);

    /* the decision itself */
    CHECK(rd3_virq_poll_due(0, 100, 100, PERIOD, 3) == 1);
    CHECK(rd3_virq_poll_due(1, 0, 100, PERIOD, 3) == 1);              /* never live */
    CHECK(rd3_virq_poll_due(1, 1000, 1000 + 3 * PERIOD - 1, PERIOD, 3) == 0);
    CHECK(rd3_virq_poll_due(1, 1000, 1000 + 3 * PERIOD, PERIOD, 3) == 1);
    CHECK(rd3_virq_poll_due(1, 5000, 4000, PERIOD, 3) == 0);          /* an interrupt after the look began */
    CHECK(rd3_virq_watch_at(1000, PERIOD, 3) == 1000 + 3 * PERIOD + PERIOD / 4);

    /* storm guard: the (MAX+1)th interrupt in a window says storm, once */
    {
        struct rd3_virq_storm s;
        uint32_t i, hits = 0;
        memset(&s, 0, sizeof(s));
        for (i = 0; i < 1000; i++)
            hits += (uint32_t)rd3_virq_storm_note(&s, 1000000ull + i * 1000ull);   /* 1 MHz for 1 ms */
        CHECK(hits == 1 && s.storms == 1);
        /* a normal 240 Hz source never trips it */
        memset(&s, 0, sizeof(s));
        for (i = 0; i < 24000; i++)
            hits += (uint32_t)rd3_virq_storm_note(&s, 5000000ull + (uint64_t)i * 4166667ull);
        CHECK(hits == 1 && s.storms == 0);
        /* the next window starts counting again */
        memset(&s, 0, sizeof(s));
        for (i = 0; i <= RD3_VIRQ_STORM_MAX; i++)
            rd3_virq_storm_note(&s, 10ull + i);
        CHECK(s.storms == 1);
        CHECK(rd3_virq_storm_note(&s, 10ull + RD3_VIRQ_STORM_WINDOW_NS) == 0 && s.in_win == 1);
    }

    /* ECC review: a backlog drained after a stall is not a storm */
    {
        struct rd3_virq_storm s;
        uint64_t t = 1000000ull;
        uint32_t i, hits = 0;
        memset(&s, 0, sizeof(s));
        for (i = 0; i < 240; i++, t += 4166667ull)   /* 1 s at 240 Hz */
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        t += 2000000000ull;                           /* the handler stalled 2 s */
        for (i = 0; i < 1 + RD3_VIRQ_DRAIN_MAX; i++, t += 2000ull)   /* its backlog drained, 2 us apart */
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        t += 4166667ull;
        for (i = 0; i < 240; i++, t += 4166667ull)   /* 240 Hz again */
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        CHECK(hits == 0 && s.storms == 0);
        CHECK(s.drained == RD3_VIRQ_DRAIN_MAX && s.drains == 1 && s.drain_end_ns == 0);
        /* a 24 Hz head and a 20 Hz one: nothing counted as drained */
        memset(&s, 0, sizeof(s));
        for (i = 0, t = 1; i < 100; i++, t += 41666667ull)
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        for (i = 0; i < 100; i++, t += 50000000ull)
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        CHECK(hits == 0 && s.drained == 0 && s.drains == 0);
        /* a stall shorter than RD3_VIRQ_STALL_NS at 240 Hz with the most a
         * backlog could hold even for a source that does not wait for its
         * ack (12 frames): under the limit, nothing counted as drained */
        memset(&s, 0, sizeof(s));
        for (i = 0, t = 1; i < 240; i++, t += 4166667ull)
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        t += RD3_VIRQ_STALL_NS - 4166667ull - 1;
        for (i = 0; i < 12; i++, t += 2000ull)
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        for (i = 0; i < 240; i++, t += 4166667ull)
            hits += (uint32_t)rd3_virq_storm_note(&s, t);
        CHECK(hits == 0 && s.storms == 0 && s.drained == 0);
        /* a real storm right after a stall: RD3_VIRQ_DRAIN_MAX kept out of
         * the window, then cut when it fills - RD3_VIRQ_DRAIN_MAX IVs later
         * than without the stall, whether time passes or not */
        memset(&s, 0, sizeof(s));
        rd3_virq_storm_note(&s, 1000ull);
        t = 1000ull + 3 * RD3_VIRQ_STALL_NS;
        {
            uint32_t cut_at = 0;
            for (i = 0; i < 1000 && !cut_at; i++)
                if (rd3_virq_storm_note(&s, t + i))   /* back to back, 1 ns apart */
                    cut_at = i + 1;
            CHECK(s.storms == 1 && cut_at == RD3_VIRQ_STORM_MAX + 1 + RD3_VIRQ_DRAIN_MAX);
            CHECK(s.drains == 1 && s.drained == RD3_VIRQ_DRAIN_MAX);
        }
    }

    /* ECC review: the sources written again only on a DCN that is back;
     * round 2: per head, from the restore's result (the heads still lost
     * AFTER it), not from whether it ran. 'all' 0xf (RD3K_MAX_HEADS 4) */
    {
        const uint32_t all = 0xfu;
        /* the wake: power lost / suspended and not resumed: down whatever the check says */
        CHECK(rd3_virq_wake_heads_up(1, 0, 0, 0, all, 0x3, 0) == 0);   /* power lost, rdna3resume=0 */
        CHECK(rd3_virq_wake_heads_up(1, 0, 1, 0, all, 0x3, 0) == 0);   /* power lost, the resume FAILED */
        CHECK(rd3_virq_wake_heads_up(0, 1, 0, 0, all, 0x3, 0) == 0);   /* suspended (IPs down), not resumed */
        /* kept its power, not suspended, nothing lost: every head */
        CHECK(rd3_virq_wake_heads_up(0, 0, 0, 0, all, 0x3, 0) == all);
        CHECK(rd3_virq_wake_heads_up(0, 0, 0, 0, all, 0, 0) == all);   /* no snapshot checked */
        /* resumed and the restore brought both heads back */
        CHECK(rd3_virq_wake_heads_up(1, 0, 1, 1, all, 0x3, 0) == all);
        CHECK(rd3_virq_wake_heads_up(0, 1, 1, 1, all, 0x3, 0) == all);
        /* resumed but the restore FAILED on both heads: no head runs, the DCN is down */
        CHECK(rd3_virq_wake_heads_up(1, 0, 1, 1, all, 0, 0x3) == 0);
        /* resumed, head 1 still lost after the restore: every head but 1
         * (heads 2, 3 have no snapshot: not lost, nothing to enable there) */
        CHECK(rd3_virq_wake_heads_up(1, 0, 1, 1, all, 0x1, 0x2) == 0xd);
        /* kept its power, rdna3resume=0: the check's lost head gets nothing */
        CHECK(rd3_virq_wake_heads_up(0, 0, 0, 0, all, 0x1, 0x2) == 0xd);
        /* the mode-1 reset */
        CHECK(rd3_virq_heads_up(all, 0x3, 0) == all);      /* the heads stayed lit, or the restore brought them back */
        CHECK(rd3_virq_heads_up(all, 0, 0x3) == 0);        /* both still down: rdna3resume=0 (not restored), or
                                                            * rdna3resume=1 and the restore FAILED (the rule
                                                            * before round 2 said up here) */
        CHECK(rd3_virq_heads_up(all, 0x1, 0x2) == 0xd);    /* one restored, one still lost */
        CHECK(rd3_virq_heads_up(0x3, 0x1, 0x2) == 0x1);    /* the same in a 2-head table */
        CHECK(rd3_virq_heads_up(all, 0x1, 0x10) == all);   /* a lost bit outside the table counts as nothing */
    }

    /* round 2: a head's sources enabled when it went down are stale; at its
     * next up sync each is turned off unless wanted again at the same place */
    {
        const uint32_t si[3] = { 0, 0, 0 };   /* VBLANK at OTG 0, PFLIP at HUBP 0, VUPDATE at OTG 0 */
        const uint32_t same[3] = { 0, 0, 0 }, moved[3] = { 1, 0, 1 };
        CHECK(rd3_virq_stale_off(0, 0x3, si, same, 3) == 0);       /* nothing stale */
        CHECK(rd3_virq_stale_off(0x3, 0, si, same, 3) == 0x3);     /* unregistered while down: both off */
        CHECK(rd3_virq_stale_off(0x3, 0x3, si, same, 3) == 0);     /* wanted again where they are */
        CHECK(rd3_virq_stale_off(0x3, 0x1, si, same, 3) == 0x2);   /* PFLIP no longer wanted */
        CHECK(rd3_virq_stale_off(0x3, 0x3, si, moved, 3) == 0x1);  /* the head moved to OTG 1: VBLANK off at OTG 0 */
        CHECK(rd3_virq_stale_off(0x4, 0x3, si, same, 3) == 0x4);   /* a stale VUPDATE nobody wants */

        /* the core's sequence (RDNA3DisplayIrq.cpp): enabled 0x3 -> a resync
         * leaves the head down (its enables kept as stale, nothing written)
         * -> the framebuffer unregisters while down -> the next resync with
         * the head up writes both off; before round 2 they stayed on in a
         * DCN that kept its state and raised 'unrouted' for ever */
        uint32_t enabled = 0x3, stale = 0, hw = 0x3, want = 0x3, writes = 0;
        uint32_t up = rd3_virq_heads_up(0xfu, 0x2, 0x1);        /* head 0 still lost */
        if (!(up & 1u)) {
            stale |= enabled;
            enabled = 0;
        }
        CHECK(up == 0xe && stale == 0x3 && hw == 0x3);
        want = 0;                                                /* unregistered while down */
        up = rd3_virq_heads_up(0xfu, 0x3, 0);                    /* a later wake brings it back */
        if (up & 1u) {
            uint32_t off = rd3_virq_stale_off(stale, want, si, same, 3);
            hw &= ~off;
            writes += (uint32_t)__builtin_popcount(off);
            stale = 0;
        }
        CHECK(hw == 0 && stale == 0 && writes == 2 && enabled == 0);
    }

    /* ECC review: stop() against a REGISTER from a running VBL thread call */
    {
        uint32_t runs, lates;
        CHECK(stop_explore(0, &runs, &lates) > 0);    /* the order before the review leaves one behind */
        CHECK(stop_explore(1, &runs, &lates) == 0 && runs == 256);
        CHECK(lates == 0);                            /* the belt after the drain is never needed */
    }

    /* latency */
    {
        struct rd3_lat l;
        memset(&l, 0, sizeof(l));
        CHECK(rd3_lat_avg(&l) == 0);
        rd3_lat_add(&l, 10);
        rd3_lat_add(&l, 30);
        CHECK(l.n == 2 && l.max_us == 30 && rd3_lat_avg(&l) == 20);
    }
    CHECK(rd3_virq_pflip_seen(5, 5) == 0 && rd3_virq_pflip_seen(5, 6) == 1);

    if (fails) {
        printf("test_vbl_irq: %d FAILED\n", fails);
        return 1;
    }
    printf("test_vbl_irq: ok\n");
    return 0;
}
