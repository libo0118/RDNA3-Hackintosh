/*
 * test_a3_display.c - common/rdna3_a3_display.h on the host: which
 * IOSurfaces the accelerator keeps out of A3 (card run 58: WindowServer's
 * display framebuffers, whose BOs no swap ever scanned out).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "rdna3_a3_display.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* card run 2026-10-15c: two 1920x1080 heads - a surface of that size may be either's */
    const uint32_t w2[4] = { 1920, 1920, 0, 0 }, h2[4] = { 1080, 1080, 0, 0 };
    /* head 0 switched to 1600x900, head 1 unknown yet (no framebuffer answer, no swap) */
    const uint32_t w1[4] = { 1600, 0, 0, 0 }, h1[4] = { 900, 0, 0, 0 };
    CHECK(rd3_a3_display_proc("WindowServer"));
    CHECK(!rd3_a3_display_proc("WindowServerHelper") && !rd3_a3_display_proc("") && !rd3_a3_display_proc(NULL));
    /* the run's surfaces 1-4: WindowServer, the scanout size -> heads 0 and 1 */
    CHECK(rd3_a3_display_heads("WindowServer", 1920, 1080, w2, h2, 4) == 0x3u);
    /* the menu bar strip, rotated sizes, window backing stores of WindowServer: not a framebuffer */
    CHECK(rd3_a3_display_heads("WindowServer", 1920, 24, w2, h2, 4) == 0);
    CHECK(rd3_a3_display_heads("WindowServer", 1080, 1920, w2, h2, 4) == 0);
    CHECK(rd3_a3_display_heads("WindowServer", 608, 343, w2, h2, 4) == 0);
    /* an application's own surface of the scanout size (a full-screen game) keeps A3 */
    CHECK(rd3_a3_display_heads("Chess", 1920, 1080, w2, h2, 4) == 0);
    CHECK(rd3_a3_display_heads("WindowServerHelper", 1920, 1080, w2, h2, 4) == 0);
    CHECK(rd3_a3_display_heads("", 1920, 1080, w2, h2, 4) == 0);
    CHECK(rd3_a3_display_heads(NULL, 1920, 1080, w2, h2, 4) == 0);
    /* after a mode switch the new size matches; the old one no longer does */
    CHECK(rd3_a3_display_heads("WindowServer", 1600, 900, w1, h1, 4) == 0x1u);
    CHECK(rd3_a3_display_heads("WindowServer", 1920, 1080, w1, h1, 4) == 0);
    /* a head whose size is not known matches nothing, not a 0x0 surface either */
    CHECK(rd3_a3_display_heads("WindowServer", 0, 0, w1, h1, 4) == 0);
    CHECK(rd3_a3_display_heads("WindowServer", 1600, 0, w1, h1, 4) == 0);
    /* only the heads asked about */
    {
        const uint32_t w[4] = { 0, 0, 2560, 0 }, h[4] = { 0, 0, 1440, 0 };
        CHECK(rd3_a3_display_heads("WindowServer", 2560, 1440, w, h, 4) == 0x4u);
        CHECK(rd3_a3_display_heads("WindowServer", 2560, 1440, w, h, 2) == 0);
    }
    CHECK(rd3_a3_display_heads("WindowServer", 1920, 1080, NULL, h2, 4) == 0);
    /* sizes beyond 32 bits never match a head */
    CHECK(rd3_a3_display_heads("WindowServer", 0x100000000ull + 1920, 1080, w2, h2, 4) == 0);
    /* the size rule alone, for the kernel's two sources (the framebuffers' answers, the swaps) */
    CHECK(rd3_a3_size_heads(1920, 1080, w2, h2, 4) == 0x3u);
    CHECK((rd3_a3_size_heads(1600, 900, w1, h1, 4) | rd3_a3_size_heads(1600, 900, w2, h2, 4)) == 0x1u);
    /* never past 32 heads */
    {
        uint32_t w[40], h[40];
        unsigned i;
        for (i = 0; i < 40; i++) { w[i] = 800; h[i] = 600; }
        CHECK(rd3_a3_size_heads(800, 600, w, h, 40) == 0xffffffffu);
    }

    /* card run 58 (2026-10-15d): every WindowServer import stays out by default */
    CHECK(rd3_a3_ws_policy(0, 0x3u, 0, 0, 0, 0, 0) == RD3_A3_ALLOW);            /* an app's own full-screen surface */
    CHECK(rd3_a3_ws_policy(0, 0, 0, 0, 0, 0, 0) == RD3_A3_ALLOW);               /* an app's window */
    CHECK(rd3_a3_ws_policy(1, 0x1u, 0, 0, 0, 0, 0) == RD3_A3_REFUSE_DISPLAY);   /* WindowServer's framebuffer */
    CHECK(rd3_a3_ws_policy(1, 0x1u, 0, 1, 0, 0, 0) == RD3_A3_REFUSE_DISPLAY);   /* rdna3a3ws=1 is not rdna3a3display=1 */
    CHECK(rd3_a3_ws_policy(1, 0x1u, 1, 0, 0, 0, 0) == RD3_A3_ALLOW);
    CHECK(rd3_a3_ws_policy(1, 0, 0, 0, 0, 0, 0) == RD3_A3_REFUSE_WS);           /* the menu bar, a window it composites */
    CHECK(rd3_a3_ws_policy(1, 0, 1, 0, 0, 0, 0) == RD3_A3_REFUSE_WS);
    CHECK(rd3_a3_ws_policy(1, 0, 0, 1, 0, 0, 0) == RD3_A3_ALLOW);
    /* DRIVER-PLAN N71 (c): a share record of the current allocation lets WindowServer import read-only */
    CHECK(rd3_a3_ws_policy(1, 0x3u, 0, 0, 1, 42, 42) == RD3_A3_ALLOW_SHARED);   /* a full-screen app's drawable */
    CHECK(rd3_a3_ws_policy(1, 0, 0, 0, 1, 42, 42) == RD3_A3_ALLOW_SHARED);      /* a window's drawable */
    CHECK(rd3_a3_ws_policy(1, 0x3u, 1, 1, 1, 42, 42) == RD3_A3_ALLOW_SHARED);   /* read-only wins over the old allows */
    CHECK(rd3_a3_ws_policy(1, 0x3u, 0, 0, 1, 41, 42) == RD3_A3_REFUSE_DISPLAY); /* a record of an older allocation */
    CHECK(rd3_a3_ws_policy(1, 0, 0, 0, 1, 43, 42) == RD3_A3_REFUSE_WS);
    CHECK(rd3_a3_ws_policy(1, 0x3u, 0, 0, 1, 0, 0) == RD3_A3_REFUSE_DISPLAY);   /* generation 0 never matches */
    CHECK(rd3_a3_ws_policy(1, 0, 0, 0, 1, 0, 0) == RD3_A3_REFUSE_WS);
    CHECK(rd3_a3_ws_policy(0, 0x3u, 0, 0, 1, 42, 42) == RD3_A3_ALLOW);          /* an app: its own import, as before */
    /* the switch off: the card run 58 table, whatever the record */
    {
        int disp, heads, ws;
        for (disp = 0; disp < 2; disp++)
            for (ws = 0; ws < 2; ws++)
                for (heads = 0; heads < 2; heads++) {
                    const unsigned m = heads ? 0x1u : 0u;
                    CHECK(rd3_a3_ws_policy(1, m, disp, ws, 0, 42, 42) == rd3_a3_ws_policy(1, m, disp, ws, 0, 0, 0));
                    CHECK(rd3_a3_ws_policy(1, m, disp, ws, 0, 42, 42) ==
                          (m ? (disp ? RD3_A3_ALLOW : RD3_A3_REFUSE_DISPLAY) : (ws ? RD3_A3_ALLOW : RD3_A3_REFUSE_WS)));
                    CHECK(rd3_a3_ws_policy(1, m, disp, ws, 0, 42, 42) != RD3_A3_ALLOW_SHARED);
                }
    }
    /* who may record: an app with an import of the live allocation; never WindowServer, never generation 0 */
    CHECK(rd3_a3_share_may_record(0, 42, 42));
    CHECK(!rd3_a3_share_may_record(1, 42, 42));
    CHECK(!rd3_a3_share_may_record(0, 41, 42));   /* its import is of an allocation the surface no longer has */
    CHECK(!rd3_a3_share_may_record(0, 0, 0));
    CHECK(!rd3_a3_share_may_record(0, 0, 42));
    CHECK(!rd3_a3_share_matches(0, 0) && rd3_a3_share_matches(7, 7) && !rd3_a3_share_matches(7, 8));
    /* WindowServer by name AND uid 88 for the shared import */
    CHECK(rd3_a3_ws_identity(1, 88));
    CHECK(!rd3_a3_ws_identity(1, 501));   /* a user process named WindowServer */
    CHECK(!rd3_a3_ws_identity(0, 88));
    CHECK(!rd3_a3_ws_identity(1, 0));
    /* the flicker test: A B A B */
    CHECK(rd3_flicker_abab(0xa, 0xb, 0xa, 3));
    CHECK(!rd3_flicker_abab(0xa, 0xb, 0xa, 2));    /* too early */
    CHECK(!rd3_flicker_abab(0xa, 0xa, 0xa, 9));    /* a still picture */
    CHECK(!rd3_flicker_abab(0xc, 0xb, 0xa, 9));    /* a new picture every swap */
    CHECK(!rd3_flicker_abab(0xa, 0xa, 0xb, 9));    /* changed once, then still */

    /* per band: one band alternating is enough, a still band is not */
    {
        const uint32_t now[3] = { 1, 2, 7 }, b1[3] = { 1, 2, 8 }, b2[3] = { 1, 2, 7 };
        const uint32_t still[3] = { 1, 2, 7 };
        CHECK(rd3_flicker_abab_any(now, b1, b2, 3, 3));
        CHECK(!rd3_flicker_abab_any(now, still, still, 3, 3));
        CHECK(!rd3_flicker_abab_any(now, b1, b2, 3, 2));   /* not three in a row yet */
        CHECK(!rd3_flicker_abab_any(now, b1, b2, 2, 3));   /* the alternating band not asked about */
        CHECK(!rd3_flicker_abab_any(NULL, b1, b2, 3, 3));
        CHECK(rd3_flicker_abab_bands(now, b1, b2, 3, 3) == 1);   /* a caret: one band */
    }
    {
        const uint32_t now[3] = { 5, 2, 7 }, b1[3] = { 6, 2, 8 }, b2[3] = { 5, 2, 7 };
        CHECK(rd3_flicker_abab_bands(now, b1, b2, 3, 3) == 2);   /* a picture: two bands */
        CHECK(rd3_flicker_abab_bands(now, b1, b2, 3, 2) == 0);
    }
    /* the bands' rows: band 0 is the menu bar (top 32 rows), the rest split the picture */
    {
        unsigned rows[4], k, b, y, last = 0, seen_top = 0;
        k = rd3_flicker_band_rows(0, 8, 1080, 4, rows);
        CHECK(k == 4 && rows[0] == 4 && rows[3] == 28);   /* inside a 24-row menu bar too: 4, 12, 20 */
        for (b = 1; b < 8; b++) {
            k = rd3_flicker_band_rows(b, 8, 1080, 2, rows);
            CHECK(k == 2);
            for (y = 0; y < k; y++) {
                CHECK(rows[y] >= 32 && rows[y] < 1080 && rows[y] > last);
                last = rows[y];
            }
        }
        CHECK(last > 1000);   /* the bottom band reaches the Dock */
        k = rd3_flicker_band_rows(0, 8, 20, 4, rows);   /* a picture lower than the menu bar band */
        for (y = 0; y < k; y++) seen_top |= rows[y] >= 20;
        CHECK(k == 4 && !seen_top);
        CHECK(rd3_flicker_band_rows(8, 8, 1080, 2, rows) == 0);
        CHECK(rd3_flicker_band_rows(3, 8, 1080, 2, NULL) == 0);
    }

    if (fails) {
        printf("test_a3_display: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_a3_display: all checks passed\n");
    return 0;
}
