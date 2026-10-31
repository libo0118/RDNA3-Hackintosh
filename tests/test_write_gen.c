/*
 * test_write_gen.c - common/rdna3_write_gen.h on the host (card runs
 * 2026-10-17d/f/g: WindowServer writes buf[1] of each display surface
 * through the family's aperture every frame and never draws buf[2]; copy
 * forward bursts rotate the three buffers with no swap and leave an old
 * frame in buf[2], which mode 0 then flipped to and mode 1 copied over the
 * panel).
 *
 * First the table and the decision one by one, then a model of 17d's
 * sequence - per frame: page-on b1, copy forward b0 -> b1, swap(b0, b1),
 * swap(b0, b2) - with copy forward bursts in between, run in mode 1 (the
 * copy) and mode 0 (the flip) with the rule, and in mode 1 with only the
 * old stale-back hold (rdna3_swap_copy.h) for comparison. Every buffer holds
 * a frame number; checked: the panel's frame never goes back, every frame
 * WindowServer drew and swapped reaches the panel, copy forward never
 * copies an older frame over a newer one, and (mode 1) the panel never
 * shows a buffer WindowServer writes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_write_gen.h"
#include "rdna3_swap_copy.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void table(void)
{
    struct rd3_wg w;
    unsigned long long ev = 1, g1, g2;
    memset(&w, 0, sizeof w);
    CHECK(rd3_wg_gen(&w, 0x1000) == 0);
    CHECK(rd3_wg_bump(&w, 0, &ev) == 0 && ev == 0);   /* no address: nothing */
    CHECK(rd3_wg_bump(0, 0x1000, 0) == 0);
    g1 = rd3_wg_bump(&w, 0x1000, &ev);
    CHECK(g1 == 1 && ev == 0 && rd3_wg_gen(&w, 0x1000) == 1);
    g2 = rd3_wg_bump(&w, 0x2000, 0);
    CHECK(g2 == 2 && rd3_wg_gen(&w, 0x2000) == 2);
    CHECK(rd3_wg_bump(&w, 0x1000, 0) == 3 && rd3_wg_gen(&w, 0x1000) == 3);   /* the same slot, a newer write */
    CHECK(rd3_wg_forget(&w, 0x1000) && rd3_wg_gen(&w, 0x1000) == 0);
    CHECK(!rd3_wg_forget(&w, 0x1000) && !rd3_wg_forget(&w, 0x9000));
    CHECK(rd3_wg_bump(&w, 0x1000, 0) == 4);           /* the generations only grow, a freed address too */
    /* a full table evicts the oldest generation */
    memset(&w, 0, sizeof w);
    for (unsigned i = 0; i < RD3_WG_SLOTS; i++)
        CHECK(rd3_wg_bump(&w, 0x10000ull * (i + 1), &ev) == i + 1 && ev == 0);
    rd3_wg_bump(&w, 0x10000, 0);                      /* the first address written again: no longer the oldest */
    CHECK(rd3_wg_bump(&w, 0x900000, &ev) == RD3_WG_SLOTS + 2 && ev == 0x20000);
    CHECK(rd3_wg_gen(&w, 0x20000) == 0 && rd3_wg_gen(&w, 0x10000) == RD3_WG_SLOTS + 1);
    CHECK(w.evictedMax == 2);   /* the evicted generation is kept (ECC review round 1: eviction fails open) */
}

/* ECC review round 1: a hold never freezes the panel, an eviction fails open, a newer buffer presents instead */
static void failopen(void)
{
    struct rd3_wg_head h;
    struct rd3_wg w;
    unsigned long long g[4];
    memset(&h, 0, sizeof h);
    memset(&w, 0, sizeof w);
    rd3_wg_presented(&h, 0x1000, 10, 1, 7, 1000);
    CHECK(h.presentedUs == 1000 && h.holds == 0);
    /* aged: RD3_WG_AGE_HOLDS holds in a row and RD3_WG_AGE_US since the last present */
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 9, 0, 1000 + RD3_WG_AGE_US) == RD3_WGW_UNWRITTEN);   /* the first hold */
    rd3_wg_held(&h);
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 9, 0, 1000 + RD3_WG_AGE_US) == RD3_WGW_UNWRITTEN);   /* the second */
    rd3_wg_held(&h);
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 9, 0, 1000 + RD3_WG_AGE_US - 1) == RD3_WGW_UNWRITTEN);   /* too soon */
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 9, 0, 1000 + RD3_WG_AGE_US) == RD3_WGW_AGED);
    CHECK(rd3_wg_decide(&h, 0x1000, 10, 1, 7, 0, 1000 + RD3_WG_AGE_US) == RD3_WGW_AGED);    /* the panel's own too */
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 9, 0, 500) == RD3_WGW_UNWRITTEN);   /* a clock before the present */
    CHECK(rd3_wg_presents(RD3_WGW_AGED) && rd3_wg_presents(RD3_WGW_EVICTED) && !rd3_wg_holds(RD3_WGW_AGED));
    rd3_wg_presented(&h, 0x2000, 0, 1, 9, 2000000);
    CHECK(h.holds == 0 && h.presentedUs == 2000000);
    /* eviction: a back with no generation while a generation newer than the head's frame was evicted presents */
    CHECK(rd3_wg_decide(&h, 0x3000, 0, 1, 1, 11, 2000001) == RD3_WGW_EVICTED);
    CHECK(rd3_wg_decide(&h, 0x3000, 0, 1, 1, 10, 2000001) == RD3_WGW_UNWRITTEN);   /* only older ones evicted */
    CHECK(rd3_wg_decide(&h, 0x3000, 4, 1, 1, 11, 2000001) == RD3_WGW_OLDER);      /* it has a generation: known */
    /* the full table keeps the newest evicted generation */
    for (unsigned i = 0; i < RD3_WG_SLOTS + 3; i++)
        rd3_wg_bump(&w, 0x100000ull * (i + 1), 0);
    CHECK(w.evictedMax == 3);
    /* the landed pick: the newest buffer written after the head's frame */
    g[0] = 5; g[1] = 12; g[2] = 0; g[3] = 11;
    CHECK(rd3_wg_landed_pick(g, 4, 10) == 1);
    CHECK(rd3_wg_landed_pick(g, 4, 12) == -1);
    CHECK(rd3_wg_landed_pick(g, 0, 0) == -1 && rd3_wg_landed_pick(0, 4, 0) == -1);
    /* a reset clears the hold run */
    rd3_wg_held(&h);
    CHECK(h.holds == 1 && rd3_wg_head_reset(&h) == 1 && h.holds == 0);
}

static void decide(void)
{
    struct rd3_wg_head h;
    memset(&h, 0, sizeof h);
    CHECK(rd3_wg_decide(0, 0x1000, 5, 1, 7, 0, 0) == RD3_WGW_OFF);
    CHECK(rd3_wg_decide(&h, 0, 5, 1, 7, 0, 0) == RD3_WGW_OFF);
    /* nothing presented since the reset: the first swap presents, whatever its back */
    CHECK(rd3_wg_decide(&h, 0x1000, 0, 1, 7, 0, 0) == RD3_WGW_FIRST);
    rd3_wg_presented(&h, 0x1000, 0, 1, 7, 0);
    CHECK(h.presented && h.gen == 0 && h.addr == 0x1000);
    /* never written, the panel's buffer: SAME; another never written: UNWRITTEN */
    CHECK(rd3_wg_decide(&h, 0x1000, 0, 1, 7, 0, 0) == RD3_WGW_SAME);
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 9, 0, 0) == RD3_WGW_UNWRITTEN);
    /* written: NEWER */
    CHECK(rd3_wg_decide(&h, 0x1000, 3, 1, 8, 0, 0) == RD3_WGW_NEWER);
    rd3_wg_presented(&h, 0x1000, 3, 1, 8, 0);
    CHECK(h.gen == 3);
    CHECK(rd3_wg_decide(&h, 0x1000, 3, 1, 8, 0, 0) == RD3_WGW_SAME);   /* swapped again, not written since */
    CHECK(rd3_wg_decide(&h, 0x2000, 2, 1, 9, 0, 0) == RD3_WGW_OLDER);  /* written before the head's frame */
    CHECK(rd3_wg_presents(RD3_WGW_NEWER) && rd3_wg_presents(RD3_WGW_FIRST) && rd3_wg_presents(RD3_WGW_CONTENT));
    CHECK(!rd3_wg_presents(RD3_WGW_SAME) && !rd3_wg_presents(RD3_WGW_OFF) && rd3_wg_holds(RD3_WGW_UNWRITTEN));
    CHECK(!rd3_wg_holds(RD3_WGW_OFF) && rd3_wg_holds(RD3_WGW_OLDER) && rd3_wg_holds(RD3_WGW_SAME));
    /* the content guard: a buffer this head presented whose samples changed with no write seen */
    CHECK(rd3_wg_decide(&h, 0x1000, 3, 1, 99, 0, 0) == RD3_WGW_CONTENT);
    CHECK(rd3_wg_decide(&h, 0x1000, 3, 0, 99, 0, 0) == RD3_WGW_SAME);  /* not sampled: no guard */
    CHECK(rd3_wg_decide(&h, 0x2000, 2, 1, 99, 0, 0) == RD3_WGW_OLDER); /* never presented here: no record to differ from */
    /* presented by the guard: the head's generation never goes back */
    rd3_wg_presented(&h, 0x2000, 2, 1, 99, 0);
    CHECK(h.gen == 3 && h.addr == 0x2000);
    CHECK(rd3_wg_decide(&h, 0x2000, 2, 1, 99, 0, 0) == RD3_WGW_SAME);
    CHECK(rd3_wg_decide(&h, 0x2000, 2, 1, 100, 0, 0) == RD3_WGW_CONTENT);
    /* not sampled at its presentation: its record goes (unknown is never "unchanged") */
    rd3_wg_presented(&h, 0x2000, 2, 0, 0, 0);
    CHECK(rd3_wg_head_sig(&h, 0x2000) == 0);
    CHECK(rd3_wg_decide(&h, 0x2000, 2, 1, 100, 0, 0) == RD3_WGW_SAME);
    /* more buffers than sample slots: the oldest slot is reused */
    for (unsigned i = 0; i < RD3_WG_HEAD_SIGS + 2; i++)
        rd3_wg_presented(&h, 0x10000ull * (i + 1), 10 + i, 1, i, 0);
    CHECK(rd3_wg_head_sig(&h, 0x10000ull * (RD3_WG_HEAD_SIGS + 2)) != 0);
    /* a freed buffer leaves the samples, the generation stays */
    {
        const unsigned long long g = h.gen, last = h.addr;
        CHECK(rd3_wg_head_forget(&h, last) && h.gen == g && h.addr == 0 && rd3_wg_head_sig(&h, last) == 0);
        CHECK(!rd3_wg_head_forget(&h, 0x77));
    }
    /* a reset: the next swap presents */
    CHECK(rd3_wg_head_reset(&h) == 1 && !h.presented);
    CHECK(rd3_wg_decide(&h, 0x2000, 0, 1, 1, 0, 0) == RD3_WGW_FIRST);
    CHECK(rd3_wg_head_reset(&h) == 0);
    /* copy forward's source and the flip-buffer pick */
    CHECK(!rd3_wg_src_newer(0, 0) && rd3_wg_src_newer(5, 4) && !rd3_wg_src_newer(5, 5) && !rd3_wg_src_newer(3, 4));
    CHECK(rd3_wg_flip_pick(1, 0, 9, 1) == 2 && rd3_wg_flip_pick(0, 1, 1, 9) == 1);
    CHECK(rd3_wg_flip_pick(0, 0, 5, 3) == 1 && rd3_wg_flip_pick(0, 0, 3, 5) == 2 && rd3_wg_flip_pick(0, 0, 4, 4) == 2);
    CHECK(rd3_wg_flip_pick(1, 1, 5, 3) == 1);
    CHECK(rd3_wg_gen16(0x12345ull) == 0x2345u);
}

/* ---- the model of 17d's sequence ----
 * Buffers 0..2 of one display surface (b0 the front, the head's
 * framebuffer resource) and the panel. frame[] is the frame number each
 * buffer holds (0 nothing); WindowServer draws frame f into b1 after its
 * page-on; copy forward copies its source's frame into the target (the
 * last frame's damage: a whole-buffer model), and WindowServer expects the
 * target to hold the front's frame afterwards. A burst rotates copy
 * forward 0 -> 1 -> 2 -> 0 with a new frame each step and no swap (the
 * family presents those by its flip-buffer path). */
enum { NB = 3, FRAMES = 400 };
struct model {
    int mode;           /* 0 flip, 1 copy */
    int genRule;        /* the write generations on (else the old rules: the stale-back hold in mode 1) */
    unsigned frame[NB];
    unsigned long long addr[NB];
    unsigned panelFrame;     /* what the panel shows */
    int panelBuf;            /* mode 0: the buffer flipped to (-1 none); mode 1: -1 (a copy of ours) */
    unsigned newest;         /* the newest frame WindowServer finished */
    int shownBuf;            /* mode 0: the buffer the head presented last (rd3_cf_source's "shown") */
    int frontCurrent;        /* mode 1: a swap's copy reached b0 */
    struct rd3_wg w;
    struct rd3_wg_head h;
    struct rd3_sc_stale st;  /* the old rule (mode 1 without the generations) */
    unsigned swaps;
    unsigned long long now;  /* microseconds: a swap every 8.3 ms (two per 60 Hz frame) */
    /* outcomes */
    unsigned back, held, presented, staleShown, olderCf, live, missed, replaced;
};
static unsigned long long gen_of(struct model *m, int b)
{
    return rd3_wg_gen(&m->w, m->addr[b]);
}
static unsigned long long sig_of(const struct model *m, int b)
{
    return 0x5000ull + m->frame[b];   /* the samples: a function of the frame */
}
static void write_buf(struct model *m, int b)
{
    if (m->genRule)
        rd3_wg_bump(&m->w, m->addr[b], 0);
}
/* copy forward(front src, target dst) as rd3_cf_source (mode 0) / rd3_sc_cf_source (mode 1) choose */
static void cf(struct model *m, int src, int dst)
{
    int from = src;
    const int newer = m->genRule && rd3_wg_src_newer(gen_of(m, src), m->h.gen);
    const unsigned want = m->frame[src] > m->panelFrame ? m->frame[src] : m->panelFrame;
    if (m->mode == 0 && m->shownBuf >= 0 && !newer)
        from = m->shownBuf;   /* COPY_SRC when it is the front, COPY_SHOWN, SKIP_BACK_IS_LAST when the target */
    else if (m->mode == 1 && src == 0 && !m->frontCurrent && !newer)
        from = -1;            /* RD3_SCCF_SCANOUT: the panel's buffer */
    if (from != dst)
        m->frame[dst] = from < 0 ? m->panelFrame : m->frame[from];
    if (m->frame[dst] < want)
        m->olderCf++;   /* the target left older than the front WindowServer named (or the panel) */
    write_buf(m, dst);
    if (!m->genRule)
        rd3_sc_stale_cf(&m->st, 1, m->addr[dst]);
}
/* one swap(b0, b) */
static void swap(struct model *m, int b)
{
    int present = 1;
    m->swaps++;
    m->now += 8333;
    if (m->genRule) {
        present = rd3_wg_presents(rd3_wg_decide(&m->h, m->addr[b], gen_of(m, b), 1, sig_of(m, b), m->w.evictedMax,
                                                m->now));
    } else if (m->mode == 1 && m->frame[b]) {
        unsigned at = 0;
        int nocf = 0;
        present = rd3_sc_stale_check(&m->st, RD3_SWAPMODE_COPY, 1, m->panelFrame != 0, m->addr[b], sig_of(m, b),
                                     &at, &nocf) != RD3_SCST_HOLD;
    }
    if (!m->frame[b] && (m->mode == 0 || m->panelFrame))
        present = 0;   /* an empty back: mode 0 skips it (rd3_swap_source), mode 1 holds it (rd3_sc_action) */
    if (!present) {
        m->held++;
        if (m->genRule)
            rd3_wg_held(&m->h);
        return;
    }
    if (m->frame[b] < m->panelFrame)
        m->back++;   /* the panel went back to an older frame */
    if (m->frame[b] < m->newest)
        m->staleShown++;
    m->presented++;
    m->panelFrame = m->frame[b];
    if (m->mode == 0) {
        m->panelBuf = b;
        m->shownBuf = b;
    } else {
        m->frame[0] = m->frame[b];   /* the copy into the front */
        m->frontCurrent = 1;
    }
    if (m->genRule)
        rd3_wg_presented(&m->h, m->addr[b], gen_of(m, b), 1, sig_of(m, b), m->now);
    else
        rd3_sc_stale_shown(&m->st, m->addr[b], sig_of(m, b), 0, m->swaps);
}
static void run(struct model *m, int mode, int genRule)
{
    static const int rot[6][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 }, { 0, 1 }, { 1, 2 }, { 2, 0 } };
    unsigned f = 0;
    memset(m, 0, sizeof *m);
    m->mode = mode;
    m->genRule = genRule;
    m->panelBuf = -1;
    m->shownBuf = -1;
    for (int b = 0; b < NB; b++)
        m->addr[b] = 0x88000000ull + 0x800000ull * (unsigned)b;
    for (unsigned k = 0; k < FRAMES; k++) {
        /* page-on b1: WindowServer draws the next frame into it */
        write_buf(m, 1);
        if (m->mode == 0 && m->panelBuf == 1)
            m->live++;   /* the buffer on the panel is written (mode 0's in-place rewrite) */
        cf(m, 0, 1);
        m->frame[1] = ++f;
        m->newest = f;
        swap(m, 1);
        m->missed += m->panelFrame != f;     /* a frame drawn and swapped that did not reach the panel */
        swap(m, 2);
        m->replaced += m->panelFrame != f;   /* b2's old frame over it */
        if (k % 50 == 25)
            for (unsigned r = 0; r < 6; r++) {   /* a burst */
                cf(m, rot[r][0], rot[r][1]);
                m->frame[rot[r][1]] = ++f;
                m->newest = f;
            }
    }
}

static void model(void)
{
    struct model m;
    /* mode 1 with the rule: every frame on the panel in order, b2's never, never a buffer WindowServer writes */
    run(&m, 1, 1);
    printf("model 17d mode 1 + generations: %u swaps, %u presented, %u held, %u stale shown, %u older copy "
           "forwards, %u back, %u missed, %u replaced\n", m.swaps, m.presented, m.held, m.staleShown, m.olderCf,
           m.back, m.missed, m.replaced);
    CHECK(m.presented == FRAMES && m.held == FRAMES && !m.staleShown && !m.back && !m.olderCf && !m.live);
    CHECK(!m.missed && !m.replaced);
    /* mode 0 with the rule: never flipped to b2's old frame, copy forward brings the burst's frame forward;
     * the panel still shows the buffer WindowServer writes next (mode 0's nature: counted, not fixed) */
    run(&m, 0, 1);
    printf("model 17d mode 0 + generations: %u swaps, %u presented, %u held, %u stale shown, %u older copy "
           "forwards, %u back, %u missed, %u replaced, %u writes into the buffer on the panel\n", m.swaps,
           m.presented, m.held, m.staleShown, m.olderCf, m.back, m.missed, m.replaced, m.live);
    CHECK(m.presented == FRAMES && !m.staleShown && !m.back && !m.olderCf && !m.missed && !m.replaced);
    CHECK(m.live == FRAMES - 1);
    /* the old rules: mode 1 with only the stale-back hold of 2026-10-17c (the burst's copy forward into b2 lifts
     * it: 17d's 0x8b7f0000 / 0x8ac10000 copies), mode 0 flipping to b2 and COPY_SHOWN copying the shown b1
     * over the burst's newer frame */
    run(&m, 1, 0);
    printf("model 17d mode 1, old rules: %u swaps, %u presented, %u held, %u stale shown, %u back, %u replaced\n",
           m.swaps, m.presented, m.held, m.staleShown, m.back, m.replaced);
    CHECK(m.staleShown > 0 && m.back > 0 && m.replaced > 0);
    run(&m, 0, 0);
    printf("model 17d mode 0, old rules: %u swaps, %u presented, %u held, %u stale shown, %u older copy "
           "forwards, %u back, %u replaced\n", m.swaps, m.presented, m.held, m.staleShown, m.olderCf, m.back,
           m.replaced);
    CHECK(m.staleShown > 0 && m.olderCf > 0 && m.replaced > 0);
}

/* bare metal 2026-10-31: WindowServer composites on the CPU through an aperture mapping it keeps (no page-on per
 * frame): its Terminal text lands in the shown buffer (in place, SAME) or in the other one (OLDER), with no new
 * generation. Without a Metal WindowServer those present; the never-written back still holds; with Metal (the VM's
 * path) every decision is exactly as before. */
static void cpu_compositor(void)
{
    struct rd3_wg_head h;
    memset(&h, 0, sizeof h);
    /* the mapping */
    for (int w = RD3_WGW_OFF; w <= RD3_WGW_MAX; w++) {
        const enum rd3_wg_why why = (enum rd3_wg_why)w;
        CHECK(rd3_wg_cpu_rule(why, 1, 1) == why);   /* a Metal WindowServer: unchanged */
        CHECK(rd3_wg_cpu_rule(why, 0, 0) == why);   /* the switch off: unchanged */
        CHECK(rd3_wg_cpu_rule(why, 0, 1) == ((why == RD3_WGW_SAME || why == RD3_WGW_OLDER) ? RD3_WGW_CPU : why));
    }
    CHECK(rd3_wg_presents(RD3_WGW_CPU) && !rd3_wg_holds(RD3_WGW_CPU));
    CHECK(rd3_wg_hold_kind(RD3_WGW_SAME) == RD3_WGH_SAME && rd3_wg_hold_kind(RD3_WGW_OLDER) == RD3_WGH_OLDER &&
          rd3_wg_hold_kind(RD3_WGW_UNWRITTEN) == RD3_WGH_UNWRITTEN && rd3_wg_hold_kind(RD3_WGW_CPU) == -1 &&
          rd3_wg_hold_kind(RD3_WGW_NEWER) == -1 && RD3_WGH_N == 3);
    /* every hold reason has its kind, every other reason none */
    for (int w = RD3_WGW_OFF; w <= RD3_WGW_MAX; w++)
        CHECK((rd3_wg_hold_kind((enum rd3_wg_why)w) >= 0) == rd3_wg_holds((enum rd3_wg_why)w));
    /* the model: b0 / b1, both paged on once at start (gens 1, 2), the head shows b1; then WindowServer writes
     * new text into b1 in place and into b0 through the kept mapping, swapping each, with no page-on. Samples
     * do not catch the few changed characters (the same sig). */
    rd3_wg_presented(&h, 0xb1, 2, 1, 5, 0);
    unsigned shown_cpu = 0, shown_metal = 0, held_unwritten = 0;
    for (unsigned f = 0; f < 10; f++) {
        const unsigned long long back = (f & 1) ? 0xb0 : 0xb1, gen = (f & 1) ? 1 : 2;
        const enum rd3_wg_why base = rd3_wg_decide(&h, back, gen, 1, 5, 0, 1000 * f);
        if (rd3_wg_presents(rd3_wg_cpu_rule(base, 0, 1))) shown_cpu++;
        if (rd3_wg_presents(rd3_wg_cpu_rule(base, 1, 1))) shown_metal++;
        rd3_wg_presented(&h, back, gen, 1, 5, 1000 * f);   /* keep the head's record as a present would */
    }
    CHECK(shown_cpu == 10);    /* every new frame reaches the panel */
    CHECK(shown_metal == 0);   /* the old rule: held (SAME / OLDER), the text stayed old until a window moved */
    /* the never-written back still holds without Metal */
    const enum rd3_wg_why u = rd3_wg_decide(&h, 0xb2, 0, 1, 9, 0, 20000);
    if (rd3_wg_holds(rd3_wg_cpu_rule(u, 0, 1))) held_unwritten++;
    CHECK(u == RD3_WGW_UNWRITTEN && held_unwritten == 1);
}

int main(void)
{
    table();
    decide();
    failopen();
    model();
    cpu_compositor();
    if (fails) {
        printf("test_write_gen: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_write_gen: all checks passed\n");
    return 0;
}
