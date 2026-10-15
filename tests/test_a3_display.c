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
    /* card run 2026-10-15c: two 1920x1080 heads */
    const uint32_t w2[4] = { 1920, 1920, 0, 0 }, h2[4] = { 1080, 1080, 0, 0 };
    /* head 0 switched to 1600x900, head 1 unknown yet (no swap, no mode) */
    const uint32_t w1[4] = { 1600, 0, 0, 0 }, h1[4] = { 900, 0, 0, 0 };
    /* the run's surfaces 1-4: WindowServer, the scanout size -> head 0 (the first that matches) */
    CHECK(rd3_a3_display_head("WindowServer", 1920, 1080, w2, h2, 4) == 0);
    /* the menu bar strip, cursors, window backing stores of WindowServer: not a framebuffer */
    CHECK(rd3_a3_display_head("WindowServer", 1920, 24, w2, h2, 4) == -1);
    CHECK(rd3_a3_display_head("WindowServer", 1080, 1920, w2, h2, 4) == -1);
    CHECK(rd3_a3_display_head("WindowServer", 608, 343, w2, h2, 4) == -1);
    /* an application's own surface of the scanout size (a full-screen game) keeps A3 */
    CHECK(rd3_a3_display_head("Chess", 1920, 1080, w2, h2, 4) == -1);
    CHECK(rd3_a3_display_head("WindowServerHelper", 1920, 1080, w2, h2, 4) == -1);
    CHECK(rd3_a3_display_head("", 1920, 1080, w2, h2, 4) == -1);
    CHECK(rd3_a3_display_head(NULL, 1920, 1080, w2, h2, 4) == -1);
    /* after a mode switch the new size matches; the old one no longer does */
    CHECK(rd3_a3_display_head("WindowServer", 1600, 900, w1, h1, 4) == 0);
    CHECK(rd3_a3_display_head("WindowServer", 1920, 1080, w1, h1, 4) == -1);
    /* a head whose size is not known yet matches nothing, not a 0x0 surface either */
    CHECK(rd3_a3_display_head("WindowServer", 0, 0, w1, h1, 4) == -1);
    CHECK(rd3_a3_display_head("WindowServer", 1600, 0, w1, h1, 4) == -1);
    /* only the heads asked about */
    {
        const uint32_t w[4] = { 0, 0, 2560, 0 }, h[4] = { 0, 0, 1440, 0 };
        CHECK(rd3_a3_display_head("WindowServer", 2560, 1440, w, h, 4) == 2);
        CHECK(rd3_a3_display_head("WindowServer", 2560, 1440, w, h, 2) == -1);
    }
    CHECK(rd3_a3_display_head("WindowServer", 1920, 1080, NULL, h2, 4) == -1);
    /* sizes beyond 32 bits never match a head */
    CHECK(rd3_a3_display_head("WindowServer", 0x100000000ull + 1920, 1080, w2, h2, 4) == -1);

    if (fails) {
        printf("test_a3_display: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_a3_display: all checks passed\n");
    return 0;
}
