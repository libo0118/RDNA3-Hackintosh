/*
 * test_strip.c - common/rdna3_strip.h on the host (DRIVER-PLAN N59): the frame-order strip painted into a row is
 * found at its x, its bits read back to the value for every counter, a blended block reads as torn, and a row of
 * ordinary picture (a gradient, a lone marker colour with no end marker) holds no strip.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_strip.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static uint8_t row[1920 * 3];
    rd3_strip_hit hit[RD3_STRIP_MAX];
    char bits[2 * RD3_STRIP_BITS + 1];
    for (uint32_t v = 0; v < RD3_STRIP_MOD; v += 7) {
        for (uint32_t x = 0; x < 1920; x++) { row[3 * x] = (uint8_t)(x / 8); row[3 * x + 1] = 128; row[3 * x + 2] = 128; }
        const int layer = (int)(v % RD3_STRIP_LAYERS);
        const uint32_t at = 100 + v % 900;
        rd3_strip_paint(row + 3 * at, layer, v);
        CHECK(rd3_strip_find(row, 1920, hit, RD3_STRIP_MAX) == 1);
        CHECK(hit[0].layer == layer && hit[0].x == at);
        rd3_strip_bits(row + 3 * at, 1920 - at, bits);
        CHECK(rd3_strip_value(bits) == (int)v);
    }
    /* two strips in one row, the second after the first's end marker */
    memset(row, 128, sizeof row);
    rd3_strip_paint(row + 3 * 10, RD3_SL_CSS, 5);
    rd3_strip_paint(row + 3 * (20 + RD3_STRIP_W), RD3_SL_WEBGL, 4095);
    CHECK(rd3_strip_find(row, 1920, hit, RD3_STRIP_MAX) == 2 && hit[0].layer == RD3_SL_CSS && hit[1].layer == RD3_SL_WEBGL
          && hit[1].x == 20 + RD3_STRIP_W);
    CHECK(rd3_strip_find(row, 1920, hit, 1) == 1);
    /* a blended block (half way between two frames) is torn; the halves not complements is torn */
    rd3_strip_paint(row + 3 * 10, RD3_SL_CSS, 5);
    memset(row + 3 * (10 + RD3_STRIP_MARK_W + RD3_STRIP_BIT_W / 2), 128, 3);
    rd3_strip_bits(row + 3 * 10, 1900, bits);
    CHECK(bits[0] == '?' && rd3_strip_value(bits) == -1);
    CHECK(rd3_strip_value("000000000000000000000000") == -1);
    CHECK(rd3_strip_value("100000000000011111111111") == 1);
    rd3_strip_bits(row + 3 * 10, 20, bits);   /* a row cut short */
    CHECK(bits[0] == 'x' && rd3_strip_value(bits) == -1);
    /* no strip: a gradient, a marker colour with no magenta end */
    for (uint32_t x = 0; x < 1920; x++) { row[3 * x] = (uint8_t)x; row[3 * x + 1] = (uint8_t)(x * 3); row[3 * x + 2] = (uint8_t)(x * 7); }
    CHECK(rd3_strip_find(row, 1920, hit, RD3_STRIP_MAX) == 0);
    memset(row, 0, sizeof row);
    for (uint32_t x = 50; x < 74; x++) row[3 * x + 1] = 255;
    CHECK(rd3_strip_find(row, 1920, hit, RD3_STRIP_MAX) == 0);
    CHECK(rd3_strip_class((const uint8_t *)"\x00\xff\x00") == 'G' && rd3_strip_class((const uint8_t *)"\xff\x00\xff") == 'M'
          && rd3_strip_class((const uint8_t *)"\x80\x80\x80") == '?');
    printf("test_strip: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
