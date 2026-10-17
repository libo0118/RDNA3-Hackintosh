/*
 * test_swap_front.c - common/rdna3_swap_front.h on the host: the swap's
 * buffer for the panel and copy forward's source (card run 58b, 2026-10-16:
 * flicker on both screens with two back buffers taking turns).
 *
 * The model: a picture of 8 bands; WindowServer draws each frame's damage
 * into the next back (one back, or two taking turns) after copy forward
 * brought the regions the last frame changed (and this one does not
 * redraw) forward from the front. The panel shows the back itself
 * (zero-copy flip). The old rule (copy only from a front that was shown)
 * shows A B A B in the changed bands; rd3_cf_source shows every frame as
 * drawn, and a missed copy is healed by copying the whole picture into
 * that back next time.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_a3_display.h"
#include "rdna3_swap_front.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { BANDS = 8, BUFS = 3 };   /* buffer 0: the front (framebuffer resource), 1 and 2: the backs */
static int gNoHeal;   /* 1: the misses are not healed (to show the heal is needed) */

struct sim {
    uint32_t buf[BUFS][BANDS];
    int shown;                  /* the buffer the head showed last, -1 none */
    struct rd3_cf_heal heal;    /* the rects each buffer missed */
    unsigned abab, wrong, copies, frames, healed, ababAfter;
};

/* band b of the model as a rect (x 0, y b, 1 x 1) and back */
static unsigned rects_of(unsigned mask, struct rd3_rect *r)
{
    unsigned n = 0;
    for (unsigned b = 0; b < BANDS; b++)
        if (mask & (1u << b)) { r[n].x = 0; r[n].y = (int)b; r[n].w = 1; r[n].h = 1; n++; }
    return n;
}
static unsigned mask_of(const struct rd3_rect *r, unsigned n)
{
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++)
        for (int y = r[i].y; y < r[i].y + r[i].h && y < BANDS; y++)
            if (y >= 0) m |= 1u << y;
    return m;
}

/* one frame: copy forward into 'back', draw the damage, swap (flip to the
 * back). 'miss': nothing can be copied this frame (SDMA refused, the rects
 * too big for the CPU); 'sdmaOff': the copy engine is off, the CPU copies
 * the (small) rects but not the whole picture. */
static void frame(struct sim *s, int back, unsigned lastDamage, unsigned damage, uint32_t value, int newRule, int miss,
                  int sdmaOff, uint32_t *truth, uint32_t h1[BANDS], uint32_t h2[BANDS])
{
    const unsigned rects = lastDamage & ~damage;   /* CGXDisplayCreateCopyForwardShape */
    struct rd3_rect r[BANDS], extra[RD3_CF_PEND_RECTS];
    const void *id = &s->buf[back];
    int from = -1;
    if (newRule) {
        const struct rd3_cf_in in = { 0 == back, 1, s->shown >= 0, s->shown == back, s->shown == 0,
                                      s->shown == 1 || s->shown == 2, s->shown == 0, 0 };
        const enum rd3_cf_pick p = rd3_cf_source(&in);
        from = p == RD3_CF_COPY_SRC ? 0 : p == RD3_CF_COPY_SHOWN ? s->shown : -1;
        /* nothing copied with rects to copy: they are pending in the target */
        if (from < 0 && rects && !gNoHeal && (p == RD3_CF_SKIP_NOTHING_SHOWN || p == RD3_CF_SKIP_FOREIGN))
            rd3_cf_pend_add(&s->heal, id, r, rects_of(rects, r), NULL);
    } else if (s->shown == 0) {
        from = 0;   /* card run 56-58b: only from a front that was shown; the front never is */
    }
    if (from >= 0) {
        const unsigned ne = gNoHeal ? 0 : rd3_cf_pend_take(&s->heal, id, &s->buf[from], 1, BANDS, !sdmaOff, extra,
                                                           RD3_CF_PEND_RECTS, NULL);
        const unsigned what = rects | mask_of(extra, ne);
        if (miss) {
            if (!gNoHeal) rd3_cf_pend_add(&s->heal, id, r, rects_of(what, r), &s->buf[from]);
        } else {
            for (unsigned b = 0; b < BANDS; b++)
                if (what & (1u << b)) { s->buf[back][b] = s->buf[from][b]; s->copies++; }
            s->healed += ne > 0;
        }
    }
    for (unsigned b = 0; b < BANDS; b++)
        if (damage & (1u << b)) { s->buf[back][b] = value; truth[b] = value; }
    /* the swap: a1 the front, a2 the back */
    const enum rd3_swap_pick pick = rd3_swap_source(1, 0, 0, 1, 0);
    CHECK(pick == RD3_SWAP_A2);
    s->shown = back;
    s->frames++;
    s->abab += rd3_flicker_abab_bands(s->buf[back], h1, h2, BANDS, s->frames) > 0;
    s->wrong += memcmp(s->buf[back], truth, sizeof s->buf[back]) != 0;
    memcpy(h2, h1, BANDS * sizeof *h1);
    memcpy(h1, s->buf[back], BANDS * sizeof *h1);
}

/* 40 frames, the damage moving over the bands (a window dragged, a menu
 * opening), with one or two backs; nothing can be copied on the frames in
 * 'misses', the copy engine is off on those in 'sdmaOff' (a bit per
 * frame); 'narrow': from frame 14 on only band 0 changes (a caret: nothing
 * redraws a stale region). The wrong frames from frame 'measure' on go to
 * 'wrongAfter'. */
static void run(int twoBacks, int newRule, uint64_t misses, uint64_t sdmaOff, int narrow, unsigned measure,
                struct sim *s, unsigned *wrongAfter)
{
    uint32_t truth[BANDS], h1[BANDS], h2[BANDS];
    unsigned last = 0, before = 0;
    memset(s, 0, sizeof *s);
    memset(truth, 0, sizeof truth);
    memset(h1, 0, sizeof h1);
    memset(h2, 0, sizeof h2);
    s->shown = -1;
    for (unsigned k = 0; k < 40; k++) {
        /* bands 0-5 change in turn, band 6 every fifth frame, band 7 once (frame 8: a menu that closed) */
        const unsigned damage = k == 0 ? 0xffu : narrow && k >= 14 ? 1u
                              : (1u << (k % 6)) | (k % 5 == 0 ? 0x40u : 0u) | (k == 8 ? 0x80u : 0u);
        const int back = twoBacks ? 1 + (int)(k & 1) : 1;
        if (k == measure) { before = s->wrong; s->ababAfter = s->abab; }
        frame(s, back, last, damage, 1000 + k, newRule, (int)((misses >> k) & 1u), (int)((sdmaOff >> k) & 1u),
              truth, h1, h2);
        last = damage;
    }
    if (wrongAfter) *wrongAfter = s->wrong - before;
    s->ababAfter = s->abab - s->ababAfter;
}

int main(void)
{
    struct sim s;
    unsigned after = 0;

    /* the swap's source */
    CHECK(rd3_swap_source(1, 0, 1, 1, 0) == RD3_SWAP_A2);   /* a1 the front: a2, whatever a1 holds */
    CHECK(rd3_swap_source(1, 0, 1, 0, 0) == RD3_SWAP_SKIP); /* an empty back: skipped, never the front */
    CHECK(rd3_swap_source(1, 0, 1, 0, 1) == RD3_SWAP_SKIP); /* the second in a row too */
    CHECK(rd3_swap_source(1, 0, 1, 0, 2) == RD3_SWAP_A2);   /* the third: a black or clear frame, shown */
    CHECK(rd3_swap_source(1, 0, 0, 0, 7) == RD3_SWAP_A2);
    CHECK(rd3_swap_source(0, 1, 1, 1, 0) == RD3_SWAP_A1);   /* the front as a2: a1 is the back */
    CHECK(rd3_swap_source(0, 1, 0, 1, 0) == RD3_SWAP_SKIP);
    CHECK(rd3_swap_source(0, 1, 0, 1, 2) == RD3_SWAP_A1);
    CHECK(rd3_swap_source(0, 0, 1, 1, 0) == RD3_SWAP_A2);   /* no front known: the older rule */
    CHECK(rd3_swap_source(0, 0, 1, 0, 0) == RD3_SWAP_A1);
    CHECK(rd3_swap_source(0, 0, 0, 0, 9) == RD3_SWAP_SKIP); /* nothing drawn, no front: skipped */
    CHECK(rd3_swap_source(1, 1, 1, 0, 0) == RD3_SWAP_A1);   /* two fronts: the older rule */

    {
        /* two empty backs taking turns (review of e2bf78e): the caller counts the head's skipped swaps,
         * whichever back they named, so the third one in a row is shown */
        unsigned run = 0, shown = 0;
        for (unsigned i = 0; i < 6; i++) {
            if (rd3_swap_source(1, 0, 0, 0, run) == RD3_SWAP_SKIP) run++;
            else { run = 0; shown |= 1u << i; }
        }
        CHECK(shown == ((1u << 2) | (1u << 5)));
    }

    /* copy forward's source */
    {
        struct rd3_cf_in in = { 1, 1, 1, 1, 1, 1, 1, 1 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_SAME);
        CHECK(rd3_cf_source(NULL) == RD3_CF_SKIP_SAME);
        in = (struct rd3_cf_in){ 0, 1, 1, 1, 0, 1, 0, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_BACK_IS_LAST);   /* one back (card run 56) */
        in = (struct rd3_cf_in){ 0, 1, 1, 0, 0, 1, 0, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_COPY_SHOWN);          /* two backs (card run 58b) */
        in = (struct rd3_cf_in){ 0, 1, 1, 0, 0, 0, 0, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_FOREIGN);        /* another surface's frame: never copied */
        in = (struct rd3_cf_in){ 0, 1, 1, 0, 1, 1, 1, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_COPY_SRC);            /* the front was shown last */
        in = (struct rd3_cf_in){ 0, 1, 0, 0, 0, 0, 1, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_NOTHING_SHOWN);  /* head known, nothing shown: never the front */
        in = (struct rd3_cf_in){ 0, 0, 0, 0, 0, 0, 1, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_COPY_SRC);            /* no head known, the front was shown */
        in = (struct rd3_cf_in){ 0, 0, 0, 0, 0, 0, 0, 0 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_NO_HEAD);
        /* card run 2026-10-17d: the front written after the head's last frame (a copy forward burst's chain) is
         * the source - over the shown buffer, the target being the shown one, nothing shown yet */
        in = (struct rd3_cf_in){ 0, 1, 1, 0, 0, 1, 0, 1 };
        CHECK(rd3_cf_source(&in) == RD3_CF_COPY_SRC);
        in = (struct rd3_cf_in){ 0, 1, 1, 1, 0, 1, 0, 1 };
        CHECK(rd3_cf_source(&in) == RD3_CF_COPY_SRC);
        in = (struct rd3_cf_in){ 0, 1, 0, 0, 0, 0, 0, 1 };
        CHECK(rd3_cf_source(&in) == RD3_CF_COPY_SRC);
        in = (struct rd3_cf_in){ 1, 1, 1, 1, 1, 1, 1, 1 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_SAME);           /* one buffer: still nothing to do */
        in = (struct rd3_cf_in){ 0, 0, 0, 0, 0, 0, 0, 1 };
        CHECK(rd3_cf_source(&in) == RD3_CF_SKIP_NO_HEAD);        /* no head: no generation to compare with */
    }

    {
        /* the pending rects, per buffer */
        struct rd3_cf_heal h;
        struct rd3_rect r[RD3_CF_PEND_RECTS + 2], o[RD3_CF_PEND_RECTS + 2];
        int b[6];
        memset(&h, 0, sizeof h);
        for (unsigned i = 0; i < RD3_CF_PEND_RECTS + 2; i++) { r[i].x = 0; r[i].y = (int)i * 10; r[i].w = 10; r[i].h = 10; }
        rd3_cf_pend_add(&h, &b[0], &r[0], 1, NULL);
        rd3_cf_pend_add(&h, &b[1], &r[1], 1, NULL);
        rd3_cf_pend_add(&h, &b[1], &r[2], 1, NULL);
        CHECK(rd3_cf_pending(&h, &b[0]) && rd3_cf_pending(&h, &b[1]) && !rd3_cf_pending(&h, &b[2]));
        /* b[0]'s rect touches none of b[1]'s: taken */
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, NULL) == 1 && o[0].y == 0);
        CHECK(!rd3_cf_pending(&h, &b[0]));
        /* a rect the source missed itself stays pending */
        rd3_cf_pend_add(&h, &b[0], &r[1], 1, NULL);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, NULL) == 0 && rd3_cf_pending(&h, &b[0]));
        CHECK(rd3_cf_pend_take(&h, &b[1], &b[2], 100, 100, 1, o, 4, NULL) == 2 && !rd3_cf_pending(&h, &b[1]));
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, NULL) == 1 && !rd3_cf_pending(&h, &b[0]));
        /* too many rects: the whole picture, only from a clean source with the copy engine on */
        rd3_cf_pend_add(&h, &b[2], r, RD3_CF_PEND_RECTS + 1, NULL);
        rd3_cf_pend_add(&h, &b[3], &r[0], 1, NULL);
        CHECK(rd3_cf_pend_take(&h, &b[2], &b[3], 100, 50, 1, o, 4, NULL) == 0);   /* the source missed a rect */
        CHECK(rd3_cf_pend_take(&h, &b[2], &b[4], 100, 50, 0, o, 4, NULL) == 0);   /* the copy engine off */
        CHECK(rd3_cf_pend_take(&h, &b[2], &b[4], 100, 50, 1, o, 4, NULL) == 1 && o[0].w == 100 && o[0].h == 50);
        CHECK(!rd3_cf_pending(&h, &b[2]));
        rd3_cf_pend_add(&h, &b[5], NULL, 0, NULL);
        CHECK(rd3_cf_pending(&h, &b[5]));
        rd3_cf_pend_forget(&h, &b[5]);
        rd3_cf_pend_forget(&h, &b[3]);
        CHECK(!rd3_cf_pending(&h, &b[5]) && !rd3_cf_pending(&h, &b[3]) && !rd3_cf_pending(&h, NULL));
        /* a full table drops the oldest buffer */
        for (unsigned i = 0; i < 5; i++) rd3_cf_pend_add(&h, &b[i], &r[i], 1, NULL);
        CHECK(!rd3_cf_pending(&h, &b[0]) && rd3_cf_pending(&h, &b[4]));
        CHECK(rd3_rect_meet(&r[0], &r[0]) && !rd3_rect_meet(&r[0], &r[1]));
    }

    {
        /* review of 7b2c4c0: mutual blocking, overflow, eviction, clipping, empty adds */
        struct rd3_cf_heal h;
        struct rd3_cf_take t;
        struct rd3_rect r[RD3_CF_PEND_RECTS + 1], o[RD3_CF_PEND_RECTS + 1];
        int b[6];
        unsigned got = 0, tries = 0;
        memset(&h, 0, sizeof h);
        for (unsigned i = 0; i < RD3_CF_PEND_RECTS + 1; i++) { r[i].x = (int)i * 10; r[i].y = 0; r[i].w = 10; r[i].h = 10; }
        /* two backs each with the whole picture pending: each waits for the
         * other, then copies anyway after RD3_CF_PEND_BLOCKED_MAX blocked calls */
        rd3_cf_pend_add(&h, &b[0], NULL, 0, NULL);
        rd3_cf_pend_add(&h, &b[1], NULL, 0, NULL);
        while (!got && tries < 10) {
            got = rd3_cf_pend_take(&h, &b[tries & 1u], &b[(tries & 1u) ^ 1u], 100, 100, 1, o, 4, &t);
            tries++;
        }
        CHECK(got == 1 && t.forced == 1 && tries == 2 * RD3_CF_PEND_BLOCKED_MAX + 1);
        CHECK(rd3_cf_pend_take(&h, &b[(tries & 1u)], &b[(tries & 1u) ^ 1u], 100, 100, 1, o, 4, &t) == 1 && !t.forced);
        CHECK(!rd3_cf_pending(&h, &b[0]) && !rd3_cf_pending(&h, &b[1]));
        /* overlapping rects block, then go anyway */
        rd3_cf_pend_add(&h, &b[0], &r[0], 1, NULL);
        rd3_cf_pend_add(&h, &b[1], &r[0], 1, NULL);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0 && t.blocked == 1);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 1 && t.forced == 1);
        rd3_cf_pend_forget(&h, &b[1]);
        /* too many rects: the whole picture, reported */
        CHECK(rd3_cf_pend_add(&h, &b[2], r, RD3_CF_PEND_RECTS + 1, NULL) == RD3_PEND_OVERFLOW);
        CHECK(h.b[0].buf == &b[2] && h.b[0].whole && h.b[0].n == 0);
        rd3_cf_pend_forget(&h, &b[2]);
        /* a full table drops the oldest list that is not the source's */
        for (unsigned i = 0; i < 4; i++) rd3_cf_pend_add(&h, &b[i], &r[i], 1, NULL);
        CHECK(rd3_cf_pend_add(&h, &b[4], &r[4], 1, &b[0]) == RD3_PEND_EVICTED);
        CHECK(rd3_cf_pending(&h, &b[0]) && !rd3_cf_pending(&h, &b[1]) && rd3_cf_pending(&h, &b[4]));
        /* nothing valid: no entry claimed, nothing evicted */
        r[5].w = 0;
        CHECK(rd3_cf_pend_add(&h, &b[5], &r[5], 1, NULL) == 0 && !rd3_cf_pend_get(&h, &b[5]) &&
              rd3_cf_pending(&h, &b[2]));
        /* the picture got smaller: a pending rect is clipped, one outside it dropped */
        memset(&h, 0, sizeof h);
        r[0].x = 90; r[0].y = 0; r[0].w = 20; r[0].h = 20;
        r[1].x = 200; r[1].y = 0; r[1].w = 20; r[1].h = 20;
        rd3_cf_pend_add(&h, &b[0], r, 2, NULL);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 15, 1, o, 4, &t) == 1 && o[0].x == 90 && o[0].w == 10 &&
              o[0].h == 15 && !rd3_cf_pending(&h, &b[0]));
    }

    {
        /* review of 2b6b6df: forced only when something is copied, the
         * waiting starts again for the copy engine and for new rects */
        struct rd3_cf_heal h;
        struct rd3_cf_take t;
        struct rd3_rect r[2], o[4];
        int b[2];
        memset(&h, 0, sizeof h);
        r[0].x = 200; r[0].y = 0; r[0].w = 10; r[0].h = 10;   /* outside a 100 x 100 picture */
        r[1] = r[0];
        rd3_cf_pend_add(&h, &b[0], &r[0], 1, NULL);
        rd3_cf_pend_add(&h, &b[1], &r[1], 1, NULL);
        for (unsigned i = 0; i < RD3_CF_PEND_BLOCKED_MAX; i++)
            CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0 && t.blocked);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0 && !t.forced);   /* clipped away */
        /* the whole picture waiting for the copy engine: the blocked count starts again */
        memset(&h, 0, sizeof h);
        rd3_cf_pend_add(&h, &b[0], NULL, 0, NULL);
        rd3_cf_pend_add(&h, &b[1], NULL, 0, NULL);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0 && t.blocked);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0 && t.blocked);
        CHECK(h.b[0].blocked == 2);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 0, o, 4, &t) == 0 && h.b[0].blocked == 0);
        /* new rects merged into a list: the blocked count starts again */
        memset(&h, 0, sizeof h);
        r[0].x = 0; r[1].x = 0;
        rd3_cf_pend_add(&h, &b[0], &r[0], 1, NULL);
        rd3_cf_pend_add(&h, &b[1], &r[1], 1, NULL);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0 && h.b[0].blocked == 2);
        r[0].x = 50;
        rd3_cf_pend_add(&h, &b[0], &r[0], 1, NULL);
        CHECK(h.b[0].blocked == 0 && h.b[0].n == 2);
        /* the non-overlapping rect goes, the overlapping one waits its 3 calls again */
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 1 && o[0].x == 50 && t.blocked && !t.forced);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 0);
        CHECK(rd3_cf_pend_take(&h, &b[0], &b[1], 100, 100, 1, o, 4, &t) == 1 && t.forced);
    }

    /* card run 56: one back - nothing to copy, every frame right, either rule */
    run(0, 0, 0, 0, 0, 20, &s, NULL);
    CHECK(s.wrong == 0 && s.abab == 0 && s.copies == 0);
    run(0, 1, 0, 0, 0, 20, &s, NULL);
    CHECK(s.wrong == 0 && s.abab == 0 && s.copies == 0);
    /* card run 58b: two backs, the old rule - stale bands, A B A B */
    run(1, 0, 0, 0, 0, 20, &s, NULL);
    CHECK(s.wrong > 0 && s.abab > 0 && s.copies == 0);
    printf("two backs, old rule: %u of %u frames wrong, %u A B A B\n", s.wrong, s.frames, s.abab);
    /* two backs, rd3_cf_source - every frame as drawn */
    run(1, 1, 0, 0, 0, 20, &s, NULL);
    CHECK(s.wrong == 0 && s.abab == 0 && s.copies > 0);
    /* one missed copy (frame 9): healed by the whole-picture copy into that
     * back, nothing wrong from frame 20 on */
    run(1, 1, 1ull << 9, 0, 0, 20, &s, &after);
    CHECK(s.wrong > 0 && after == 0 && s.healed == 1);
    /* without the heal the missed regions stay stale in that back: wrong frames go on */
    gNoHeal = 1;
    run(1, 1, 1ull << 9, 0, 0, 20, &s, &after);
    CHECK(after > 0 && s.abab > 0);
    gNoHeal = 0;
    /* three in a row, into both backs (review of e2bf78e: one heal slot per
     * head left the first back stale), the damage narrow afterwards so
     * nothing redraws the stale regions: both backs healed */
    run(1, 1, 7ull << 9, 0, 0, 20, &s, &after);
    CHECK(after == 0 && s.healed >= 2);
    run(1, 1, 7ull << 9, 0, 1, 20, &s, &after);
    CHECK(after == 0 && s.healed >= 2);
    /* a miss, then the copy engine off for frames 10-29: the rects still go
     * by the CPU, the whole picture waits; healed once it is back */
    run(1, 1, 1ull << 9, 0xfffffull << 10, 1, 32, &s, &after);
    CHECK(after == 0 && s.healed >= 1);

    if (fails) {
        printf("test_swap_front: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_swap_front: all checks passed\n");
    return 0;
}
