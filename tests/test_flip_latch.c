/*
 * test_flip_latch.c - common/rdna3_flip_latch.h on the host (card run
 * 2026-10-17: windows and menus flicker on both screens; the swap returned
 * before its flip latched).
 *
 * The model: a head at 60 Hz (vblank every 16667 us), a flip programmed at
 * the swap latches at the next vblank; until then the panel scans the
 * buffer shown before. WindowServer gets the old buffer back when the swap
 * returns and starts writing into it (copy forward, the next frame) 100 us
 * later. Without the wait it writes into the buffer the panel still scans
 * (the half-drawn frame the user sees); with rd3_latch_step's wait, polled
 * every 1 ms as the kext polls, never, and no swap waits longer than a
 * frame and a poll.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_flip_latch.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { PERIOD = 16667, POLL = 1000, WRITE_DELAY = 100, SWAPS = 600 };

static uint32_t rng = 12345u;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % n;
}

/* the vblank after t (a flip at t latches there) */
static uint64_t next_vblank(uint64_t t)
{
    return (t / PERIOD + 1) * PERIOD;
}

struct run { unsigned on_panel, swaps, timeouts; uint64_t max_wait; };

/* 'wait': the swap polls rd3_latch_step until the flip latched (the fix);
 * 'bufs': back buffers taking turns (1: the buffer drawn is the one shown) */
static struct run simulate(int wait, unsigned bufs)
{
    struct run r = { 0, 0, 0, 0 };
    uint64_t t = 1000, latch_at = 0;   /* the last flip's latch time */
    int shown = -1, prev = -1;
    unsigned next = 0;
    rng = 777u;
    for (unsigned i = 0; i < SWAPS; i++) {
        /* WindowServer draws buffer 'next' and swaps it at t */
        const int buf = (int)next;
        prev = shown;
        shown = buf;
        latch_at = next_vblank(t);
        uint64_t ret = t;
        if (wait) {
            const uint32_t frame0 = (uint32_t)(t / PERIOD);
            uint64_t now = t;
            enum rd3_latch_step s;
            do {
                now += POLL;
                s = rd3_latch_step(1, now < latch_at, frame0, (uint32_t)(now / PERIOD), now - t,
                                   rd3_latch_cap_us(PERIOD));
            } while (s == RD3_LATCH_WAIT);
            if (s == RD3_LATCH_TIMEOUT)
                r.timeouts++;
            ret = now;
            if (now - t > r.max_wait)
                r.max_wait = now - t;
        }
        /* the buffer WindowServer writes next: with two backs the one shown
         * before this swap; it starts after the swap returned */
        next = (next + 1) % bufs;
        const uint64_t write = ret + WRITE_DELAY;
        const int target = (int)next;
        /* the panel scans 'prev' until latch_at, 'shown' after */
        const int scanned = write < latch_at ? prev : shown;
        if (bufs > 1 && target == scanned)
            r.on_panel++;
        r.swaps++;
        /* the next swap 2..30 ms later */
        t = write + 2000 + rnd(28000);
    }
    return r;
}

/* ---- the accelerator's swap, as rd3AccelLatchEntry / rd3AccelLatchFlipped /
 * rd3AccelLatchWait run it, against a head that latches at the next vblank
 * ('stuck' heads never do; 'frozen' heads' frame counter stops too, a
 * blanked head). 'pre' microseconds of the swap's own work come before the
 * flip (the hashes, flipToLocked's 20 ms wait for a stuck previous flip). ---- */
struct khead {
    struct rd3_latch_state st;
    int has_flip, stuck, frozen;
    uint64_t flip_t, latch_at;   /* the last flip, when it latches */
    uint32_t frame0;
    unsigned timeouts, offs, resumed, flip_waits, pend_seen;
    uint64_t waited;             /* microseconds the swaps spent waiting for their flips */
};
static int kpending(const struct khead *h, uint64_t now)
{
    return h->has_flip && (h->stuck || now < h->latch_at);
}
static uint32_t kframe(const struct khead *h, uint64_t now)
{
    return h->frozen ? 7u : (uint32_t)(now / PERIOD);
}
static void kevent(struct khead *h, enum rd3_latch_event e)
{
    if (e == RD3_LATCH_EV_OFF) h->offs++;
    if (e == RD3_LATCH_EV_RESUMED) h->resumed++;
}
/* the wait after the flip: returns the time it ended */
static uint64_t kwait(struct khead *h, uint64_t now, uint64_t cap)
{
    for (;;) {
        now += POLL;
        const enum rd3_latch_step s = rd3_latch_step(1, kpending(h, now), h->frame0, kframe(h, now),
                                                     now - h->flip_t, cap);
        if (s == RD3_LATCH_WAIT) continue;
        if (s == RD3_LATCH_DONE) kevent(h, rd3_latch_seen(&h->st));
        else if (s == RD3_LATCH_TIMEOUT) { h->timeouts++; kevent(h, rd3_latch_timeout(&h->st)); }
        else rd3_latch_give_up(&h->st);
        return now;
    }
}
/* a swap at 'now' with the sysctl 'enabled'; returns when it returns */
static uint64_t kswap(struct khead *h, uint64_t now, int enabled, uint64_t pre)
{
    const uint64_t cap = rd3_latch_cap_us(PERIOD);
    /* the entry only looks */
    if (h->has_flip && !h->st.latched) {
        if (!kpending(h, now)) kevent(h, rd3_latch_seen(&h->st));
        else h->pend_seen++;
    }
    now += pre;
    h->has_flip = 1;
    h->flip_t = now;
    h->latch_at = next_vblank(now);
    h->frame0 = kframe(h, now);
    rd3_latch_flipped(&h->st);
    if (!kpending(h, now)) kevent(h, rd3_latch_seen(&h->st));
    else if (rd3_latch_wait_flip(enabled, &h->st)) {
        const uint64_t t = now;
        h->flip_waits++;
        now = kwait(h, now, cap);
        h->waited += now - t;
    } else rd3_latch_give_up(&h->st);
    return now;
}
static struct khead knew(int stuck, int frozen)
{
    struct khead h;
    memset(&h, 0, sizeof h);
    h.st.latched = 1;
    h.stuck = stuck;
    h.frozen = frozen;
    return h;
}
static void kmodel(void)
{
    const uint64_t cap = rd3_latch_cap_us(PERIOD);
    /* swap_latch=0: no wait at all */
    {
        struct khead h = knew(0, 0);
        uint64_t t = 1000;
        for (unsigned i = 0; i < 50; i++) t = kswap(&h, t, 0, 0) + 3000;   /* faster than the panel */
        CHECK(h.waited == 0 && h.flip_waits == 0 && h.pend_seen > 0);
    }
    /* a normal head: every flip waited for, none late */
    {
        struct khead h = knew(0, 0);
        uint64_t t = 1000;
        for (unsigned i = 0; i < 50; i++) t = kswap(&h, t, 1, 0) + 3000;
        CHECK(h.flip_waits == 50 && h.timeouts == 0 && h.pend_seen == 0);
    }
    /* stuck heads, with and without a moving frame counter, with and
     * without 20 ms of work before each flip (ECC review round 2: that
     * work once spent a shared budget, the waits ended as "budget", never
     * as timeouts, and the head never stopped waiting): three timeouts,
     * each bounded by the cap, then the waits are off (reported once);
     * the head that latches again resumes */
    for (unsigned frozen = 0; frozen < 2; frozen++)
        for (unsigned pre = 0; pre <= 20000; pre += 20000) {
            struct khead h = knew(1, (int)frozen);
            uint64_t t = 1000;
            for (unsigned i = 0; i < 20; i++) {
                const uint64_t before = h.waited;
                t = kswap(&h, t, 1, pre) + 3000;
                CHECK(h.waited - before <= cap + POLL);
            }
            CHECK(h.timeouts == RD3_LATCH_TIMEOUT_RUN && h.offs == 1 && h.flip_waits == RD3_LATCH_TIMEOUT_RUN);
            CHECK(h.waited <= RD3_LATCH_TIMEOUT_RUN * (cap + POLL));
            h.stuck = 0;
            h.frozen = 0;
            for (unsigned i = 0; i < 5; i++) t = kswap(&h, t, 1, pre) + 30000;
            CHECK(h.resumed == 1 && !h.st.off && h.timeouts == RD3_LATCH_TIMEOUT_RUN);
        }
    /* recovery seen only at the entry: the off head's flip latches before
     * the next swap, whose entry sees it (round 1: the run was never reset) */
    {
        struct khead h = knew(1, 0);
        uint64_t t = 1000;
        for (unsigned i = 0; i < 4; i++) t = kswap(&h, t, 1, 0) + 3000;
        CHECK(h.st.off);
        h.stuck = 0;
        t = kswap(&h, t, 1, 0) + 40000;   /* not waited (off), latches meanwhile */
        const unsigned waits = h.flip_waits;
        t = kswap(&h, t, 1, 0);
        CHECK(h.resumed == 1 && !h.st.off && h.flip_waits == waits + 1);
    }
}

int main(void)
{
    /* rd3_latch_cap_us: unknown 35 ms, else 2.5 frames, 5 Hz the maximum */
    CHECK(rd3_latch_cap_us(0) == RD3_LATCH_CAP_US);
    CHECK(rd3_latch_cap_us(PERIOD) == 41667);   /* 60 Hz: 2.5 frames */
    CHECK(rd3_latch_cap_us(33333) == 83332);
    CHECK(rd3_latch_cap_us(200000) == RD3_LATCH_CAP_MAX_US);

    /* rd3_latch_step */
    CHECK(rd3_latch_step(0, 1, 0, 0, 0, 35000) == RD3_LATCH_UNKNOWN);
    CHECK(rd3_latch_step(1, 0, 5, 5, 0, 35000) == RD3_LATCH_DONE);
    CHECK(rd3_latch_step(1, 1, 5, 5, 1000, 35000) == RD3_LATCH_WAIT);
    CHECK(rd3_latch_step(1, 1, 5, 6, 16000, 35000) == RD3_LATCH_WAIT);      /* one vblank: may be latching */
    CHECK(rd3_latch_step(1, 1, 5, 7, 20000, 35000) == RD3_LATCH_TIMEOUT);   /* two frames: late */
    CHECK(rd3_latch_step(1, 1, 0xffffff, 1, 20000, 35000) == RD3_LATCH_TIMEOUT);   /* the 24-bit wrap */
    CHECK(rd3_latch_step(1, 1, 0xffffff, 0, 1000, 35000) == RD3_LATCH_WAIT);
    CHECK(rd3_latch_step(1, 1, 9, 9, 35000, 35000) == RD3_LATCH_TIMEOUT);   /* a frozen counter: the cap */
    CHECK(rd3_latch_step(1, 1, 9, 9, 34999, 35000) == RD3_LATCH_WAIT);
    /* 30 Hz: one frame is not late, the 60 Hz cap would have been */
    CHECK(rd3_latch_step(1, 1, 9, 10, 40000, rd3_latch_cap_us(33333)) == RD3_LATCH_WAIT);
    /* only the time since the flip counts (review round 2: a swap's work
     * before the flip once ended stuck waits as "budget spent") */
    CHECK(rd3_latch_step(1, 1, 9, 9, 2000, 35000) == RD3_LATCH_WAIT);

    /* rd3_latch_frame_us / rd3_latch_ask_frame */
    CHECK(rd3_latch_frame_us(60000) == 16666 && rd3_latch_frame_us(30000) == 33333);
    CHECK(rd3_latch_frame_us(0) == 0 && rd3_latch_frame_us(999) == 0);
    CHECK(rd3_latch_ask_frame(0, 0) && rd3_latch_ask_frame(0, RD3_LATCH_FRAME_TRIES - 1));
    CHECK(!rd3_latch_ask_frame(0, RD3_LATCH_FRAME_TRIES) && !rd3_latch_ask_frame(1, 0));

    /* rd3_empty_resample_due: never with swap_latch=0; 8, then every 256th */
    CHECK(!rd3_empty_resample_due(0, 0) && !rd3_empty_resample_due(0, 256));
    CHECK(rd3_empty_resample_due(1, 0) && rd3_empty_resample_due(1, 7) && !rd3_empty_resample_due(1, 8));
    CHECK(rd3_empty_resample_due(1, 256) && !rd3_empty_resample_due(1, 257));

    /* the state machine */
    {
        struct rd3_latch_state st = { 0, 0, 0, 1 };
        rd3_latch_flipped(&st);
        CHECK(rd3_latch_wait_flip(1, &st));
        CHECK(!rd3_latch_wait_flip(0, &st));   /* swap_latch=0: never */
        CHECK(rd3_latch_timeout(&st) == RD3_LATCH_EV_NONE && st.run == 1 && st.gave_up);
        rd3_latch_flipped(&st);
        CHECK(rd3_latch_timeout(&st) == RD3_LATCH_EV_NONE);
        rd3_latch_flipped(&st);
        CHECK(rd3_latch_timeout(&st) == RD3_LATCH_EV_OFF && st.off);
        rd3_latch_flipped(&st);
        CHECK(rd3_latch_timeout(&st) == RD3_LATCH_EV_NONE);   /* the transition is reported once */
        CHECK(!rd3_latch_wait_flip(1, &st));
        CHECK(rd3_latch_seen(&st) == RD3_LATCH_EV_RESUMED && !st.off && st.run == 0);
        CHECK(rd3_latch_seen(&st) == RD3_LATCH_EV_NONE);
        CHECK(rd3_latch_wait_flip(1, &st));
        rd3_latch_flipped(&st);
        rd3_latch_give_up(&st);
        CHECK(st.run == 0 && st.gave_up);   /* a give-up is not a timeout */
        st.off = 1;
        st.run = RD3_LATCH_TIMEOUT_RUN;
        CHECK(rd3_latch_rearm(&st) == RD3_LATCH_EV_RESUMED && st.latched && !st.off && !st.run);
    }

    /* rd3_latch_panel */
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0x100) == RD3_PANEL_SHOWN);
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0x200) == RD3_PANEL_LEAVING);
    CHECK(rd3_latch_panel(0x100, 0x200, 1, 1, 0x200) == RD3_PANEL_NO);   /* seen latched */
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 0, 0x200) == RD3_PANEL_NO);   /* latched by now */
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0x300) == RD3_PANEL_NO);
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0) == RD3_PANEL_NO);

    /* rd3_torn_bands / rd3_torn_classify */
    {
        uint32_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[8];
        memcpy(b, a, sizeof b);
        CHECK(rd3_torn_bands(a, b, 8) == 0);
        CHECK(rd3_torn_classify(rd3_torn_bands(a, b, 8), 0) == RD3_TORN_NONE);
        b[0] = 99;
        b[5] = 99;
        CHECK(rd3_torn_bands(a, b, 8) == 0x21u);
        CHECK(rd3_torn_bands(a, b, 4) == 0x01u);
        CHECK(rd3_torn_bands(NULL, b, 8) == 0);
        CHECK(rd3_torn_classify(0x21u, 0) == RD3_TORN_SHOWN);
        CHECK(rd3_torn_classify(0x21u, 1) == RD3_TORN_INPLACE);
    }

    /* rd3_log_due */
    CHECK(rd3_log_due(0, 8, 1023) && rd3_log_due(7, 8, 1023) && !rd3_log_due(8, 8, 1023));
    CHECK(rd3_log_due(1024, 8, 1023) && !rd3_log_due(1025, 8, 1023));

    /* the model: two backs taking turns */
    {
        const struct run before = simulate(0, 2), after = simulate(1, 2);
        printf("test_flip_latch: two backs, %u swaps: %u writes into the buffer the panel scanned without the wait, "
               "%u with it (longest wait %llu us, %u timeouts)\n", before.swaps, before.on_panel, after.on_panel,
               (unsigned long long)after.max_wait, after.timeouts);
        CHECK(before.on_panel > before.swaps / 2);   /* the old swap: most frames written on the panel */
        CHECK(after.on_panel == 0);
        CHECK(after.timeouts == 0);
        CHECK(after.max_wait <= PERIOD + POLL);
    }
    /* one back: the buffer drawn is the one shown either way (not counted
     * here; the kext counts it as rewritten in place) */
    {
        const struct run one = simulate(1, 1);
        CHECK(one.on_panel == 0 && one.timeouts == 0);
    }

    kmodel();

    /* card run 2026-10-18b: a mode 0 swap to the buffer the panel already scans */
    {
        const uint64_t A = 0x89040000ull, B = 0x8a000000ull;
        /* same and latched (by the record, the scanned address unknown): not flipped, no wait */
        CHECK(rd3_latch_same_shown(1, A, 1, 0, 0, 0, A) == RD3_SAME_PENDING);   /* record only: never skipped */
        /* same, the HUBP scans it and no flip pending (the record not latched yet): not flipped */
        CHECK(rd3_latch_same_shown(1, A, 0, 1, A, 0, A) == RD3_SAME_SKIP);
        CHECK(rd3_latch_same_shown(1, A, 1, 1, A, 0, A) == RD3_SAME_SKIP);
        /* same but its flip still pending: flip and wait as before */
        CHECK(rd3_latch_same_shown(1, A, 0, 0, 0, 0, A) == RD3_SAME_PENDING);
        CHECK(rd3_latch_same_shown(1, A, 0, 1, A, 1, A) == RD3_SAME_PENDING);
        CHECK(rd3_latch_same_shown(1, A, 1, 1, A, 1, A) == RD3_SAME_PENDING);   /* a flip pending the record missed */
        /* a different buffer: flip and wait as before */
        CHECK(rd3_latch_same_shown(1, A, 1, 0, 0, 0, B) == RD3_SAME_NO);
        CHECK(rd3_latch_same_shown(1, A, 1, 1, A, 0, B) == RD3_SAME_NO);
        /* the HUBP scans another buffer than the record says (a stale record): flip */
        CHECK(rd3_latch_same_shown(1, A, 1, 1, B, 0, A) == RD3_SAME_NO);
        /* nothing recorded (a forget: mode change, refused flip), no back address, or off: as before */
        CHECK(rd3_latch_same_shown(1, 0, 1, 0, 0, 0, A) == RD3_SAME_NO);
        CHECK(rd3_latch_same_shown(1, 0, 1, 1, A, 0, A) == RD3_SAME_NO);
        CHECK(rd3_latch_same_shown(1, 0, 1, 0, 0, 0, 0) == RD3_SAME_NO);
        CHECK(rd3_latch_same_shown(0, A, 1, 1, A, 0, A) == RD3_SAME_NO);
    }

    if (fails) {
        printf("test_flip_latch: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_flip_latch: all checks passed\n");
    return 0;
}
