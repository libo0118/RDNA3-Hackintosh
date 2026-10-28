/*
 * test_iop_copy.c - common/rdna3_iop_copy.h on the host (DRIVER-PLAN N61): the display pipe's copy of a presented
 * surface into the scanout puts the same bytes where the 32-bit loop put them, with either loop, at any pitch.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna3_iop_copy.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the reference: the old loop of performTransaction (32-bit words, min(pitch) & ~3 per row) */
static void ref_copy(uint8_t *dst, uint32_t dpitch, const uint8_t *src, uint32_t spitch, uint32_t h, uint32_t row_bytes)
{
    for (uint32_t y = 0; y < h; y++) memcpy(dst + (size_t)y * dpitch, src + (size_t)y * spitch, row_bytes);
}

static int one(uint32_t w, uint32_t h, uint32_t spitch, uint32_t dpitch, uint32_t soff, uint32_t doff, int want_width)
{
    const size_t sn = (size_t)spitch * h + 16, dn = (size_t)dpitch * h + 16;
    uint8_t *src = malloc(sn), *a = malloc(dn), *b = malloc(dn);
    if (!src || !a || !b) { free(src); free(a); free(b); return 0; }
    for (size_t i = 0; i < sn; i++) src[i] = (uint8_t)(i * 131u + 7u);
    memset(a, 0xA5, dn); memset(b, 0xA5, dn);
    const uint32_t row = ((spitch < dpitch ? spitch : dpitch) & ~3u) < w * 4u ? ((spitch < dpitch ? spitch : dpitch) & ~3u) : w * 4u;
    const int width = rd3_iop_copy_rows(a + doff, dpitch, src + soff, spitch, h, row);
    ref_copy(b + doff, dpitch, src + soff, spitch, h, row);
    const int ok = !memcmp(a, b, dn) && width == want_width;
    free(src); free(a); free(b);
    return ok;
}

int main(void)
{
    CHECK(rd3_iop_copy64_ok(0x1000, 0x2000, 7680, 7680, 7680));
    CHECK(!rd3_iop_copy64_ok(0x1004, 0x2000, 7680, 7680, 7680));
    CHECK(!rd3_iop_copy64_ok(0x1000, 0x2000, 7684, 7680, 7680));
    CHECK(!rd3_iop_copy64_ok(0x1000, 0x2000, 7680, 7680, 7676));
    /* malloc gives 16-byte alignment: offsets 0 keep 8-byte alignment */
    CHECK(one(1920, 8, 7680, 7680, 0, 0, 8));        /* WindowServer's 1920x1080 frame, rows of 7680 */
    CHECK(one(1920, 8, 7744, 7680, 0, 0, 8));        /* a padded source pitch */
    CHECK(one(1366, 6, 5464, 5504, 0, 0, 8));        /* 1366 * 4 = 5464 = 683 * 8 */
    CHECK(one(1365, 6, 5460, 5504, 0, 0, 4));        /* an odd width: 5460 is not a multiple of 8 */
    CHECK(one(1920, 4, 7680, 7680, 4, 0, 4));        /* a source only 4-byte aligned */
    CHECK(one(1920, 4, 7680, 7680, 0, 4, 4));        /* a destination only 4-byte aligned */
    CHECK(one(800, 5, 3204, 3200, 0, 0, 4));         /* an odd source pitch */
    CHECK(rd3_iop_mbps(8294400, 4000) == 2073 && rd3_iop_mbps(1, 0) == 0);
    CHECK(rd3_iop_copy_log_due(0) && rd3_iop_copy_log_due(3) && !rd3_iop_copy_log_due(4) && rd3_iop_copy_log_due(2048));
    printf("test_iop_copy: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
