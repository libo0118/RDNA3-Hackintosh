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
 * drawn.
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

struct sim {
    uint32_t buf[BUFS][BANDS];
    int shown;                  /* the buffer the head showed last, -1 none */
    unsigned abab, wrong, copies, frames;
};

/* one frame: copy forward into 'back', draw the damage, swap (flip to the back) */
static void frame(struct sim *s, int back, unsigned lastDamage, unsigned damage, uint32_t value, int newRule,
                  uint32_t *truth, uint32_t h1[BANDS], uint32_t h2[BANDS])
{
    const unsigned rects = lastDamage & ~damage;   /* CGXDisplayCreateCopyForwardShape */
    int from = -1;
    if (newRule) {
        const enum rd3_cf_pick p = rd3_cf_source(0 == back, 0, s->shown >= 0, s->shown == back, s->shown == 0);
        from = p == RD3_CF_COPY_SRC ? 0 : p == RD3_CF_COPY_SHOWN ? s->shown : -1;
    } else if (s->shown == 0) {
        from = 0;   /* card run 56-58b: only from a front that was shown; the front never is */
    }
    for (unsigned b = 0; from >= 0 && b < BANDS; b++)
        if (rects & (1u << b)) { s->buf[back][b] = s->buf[from][b]; s->copies++; }
    for (unsigned b = 0; b < BANDS; b++)
        if (damage & (1u << b)) { s->buf[back][b] = value; truth[b] = value; }
    /* the swap: a1 the front, a2 the back */
    const enum rd3_swap_pick pick = rd3_swap_source(1, 0, 0, 1);
    CHECK(pick == RD3_SWAP_A2);
    s->shown = back;
    s->frames++;
    s->abab += rd3_flicker_abab_bands(s->buf[back], h1, h2, BANDS, s->frames) > 0;
    s->wrong += memcmp(s->buf[back], truth, sizeof s->buf[back]) != 0;
    memcpy(h2, h1, BANDS * sizeof *h1);
    memcpy(h1, s->buf[back], BANDS * sizeof *h1);
}

/* 40 frames, the damage moving over the bands (a window dragged, a menu
 * opening), with one or two backs */
static void run(int twoBacks, int newRule, struct sim *s)
{
    uint32_t truth[BANDS], h1[BANDS], h2[BANDS];
    unsigned last = 0;
    memset(s, 0, sizeof *s);
    memset(truth, 0, sizeof truth);
    memset(h1, 0, sizeof h1);
    memset(h2, 0, sizeof h2);
    s->shown = -1;
    for (unsigned k = 0; k < 40; k++) {
        const unsigned damage = k == 0 ? 0xffu : (1u << (k % BANDS)) | (k % 5 == 0 ? 0x81u : 0u);
        const int back = twoBacks ? 1 + (int)(k & 1) : 1;
        frame(s, back, last, damage, 1000 + k, newRule, truth, h1, h2);
        last = damage;
    }
}

int main(void)
{
    struct sim s;

    /* the swap's source */
    CHECK(rd3_swap_source(1, 0, 1, 1) == RD3_SWAP_A2);   /* a1 the front: a2, whatever a1 holds */
    CHECK(rd3_swap_source(1, 0, 1, 0) == RD3_SWAP_SKIP); /* an empty back: skipped, never the front */
    CHECK(rd3_swap_source(0, 1, 1, 1) == RD3_SWAP_A1);   /* the front as a2: a1 is the back */
    CHECK(rd3_swap_source(0, 1, 0, 1) == RD3_SWAP_SKIP);
    CHECK(rd3_swap_source(0, 0, 1, 1) == RD3_SWAP_A2);   /* no front known: the older rule */
    CHECK(rd3_swap_source(0, 0, 1, 0) == RD3_SWAP_A1);
    CHECK(rd3_swap_source(0, 0, 0, 0) == RD3_SWAP_SKIP);
    CHECK(rd3_swap_source(1, 1, 1, 0) == RD3_SWAP_A1);   /* two fronts: the older rule */

    /* copy forward's source */
    CHECK(rd3_cf_source(1, 1, 1, 1, 1) == RD3_CF_SKIP_SAME);
    CHECK(rd3_cf_source(0, 0, 1, 1, 0) == RD3_CF_SKIP_BACK_IS_LAST);  /* one back (card run 56) */
    CHECK(rd3_cf_source(0, 0, 1, 0, 0) == RD3_CF_COPY_SHOWN);         /* two backs (card run 58b) */
    CHECK(rd3_cf_source(0, 1, 1, 0, 1) == RD3_CF_COPY_SRC);           /* the front was shown last */
    CHECK(rd3_cf_source(0, 1, 1, 0, 0) == RD3_CF_COPY_SHOWN);         /* shown once, not last: the head's last */
    CHECK(rd3_cf_source(0, 1, 0, 0, 0) == RD3_CF_COPY_SRC);           /* no head known, the front was shown */
    CHECK(rd3_cf_source(0, 0, 0, 0, 0) == RD3_CF_SKIP_NOT_SHOWN);

    /* card run 56: one back - nothing to copy, every frame right, either rule */
    run(0, 0, &s);
    CHECK(s.wrong == 0 && s.abab == 0 && s.copies == 0);
    run(0, 1, &s);
    CHECK(s.wrong == 0 && s.abab == 0 && s.copies == 0);
    /* card run 58b: two backs, the old rule - stale bands, A B A B */
    run(1, 0, &s);
    CHECK(s.wrong > 0 && s.abab > 0 && s.copies == 0);
    printf("two backs, old rule: %u of %u frames wrong, %u A B A B\n", s.wrong, s.frames, s.abab);
    /* two backs, rd3_cf_source - every frame as drawn */
    run(1, 1, &s);
    CHECK(s.wrong == 0 && s.abab == 0 && s.copies > 0);

    if (fails) {
        printf("test_swap_front: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_swap_front: all checks passed\n");
    return 0;
}
