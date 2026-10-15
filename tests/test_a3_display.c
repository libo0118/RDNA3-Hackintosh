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

    if (fails) {
        printf("test_a3_display: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_a3_display: all checks passed\n");
    return 0;
}
