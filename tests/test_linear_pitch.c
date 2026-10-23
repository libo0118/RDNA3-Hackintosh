/* The row pitch a linear buffer texture can alias (metal/plugin/nvmtl_linear_pitch.h):
 * RADV's custom linear pitch is a multiple of 256 B (card runs 2026-10-17d / 18b:
 * vkCreateImage -> -1000158000 for every other bytesPerRow). SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_linear_pitch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    static const size_t pass[] = { 256, 512, 1024, 2048, 2304, 2560, 7680 };
    static const size_t fail[] = { 64, 128, 192, 320, 384, 576, 960 };
    static const uint32_t bpe[] = { 1, 2, 4, 8, 16 };
    for (unsigned b = 0; b < sizeof bpe / sizeof *bpe; b++) {
        for (unsigned i = 0; i < sizeof pass / sizeof *pass; i++) {
            if (!nvmtl_linear_pitch_ok(pass[i], bpe[b])) {
                fprintf(stderr, "pitch %zu bpe %u must pass\n", pass[i], (unsigned)bpe[b]); failures++; }
        }
        for (unsigned i = 0; i < sizeof fail / sizeof *fail; i++) {
            if (nvmtl_linear_pitch_ok(fail[i], bpe[b])) {
                fprintf(stderr, "pitch %zu bpe %u must fail\n", fail[i], (unsigned)bpe[b]); failures++; }
        }
    }
    /* bpe 0 (unknown): the 256 B rule alone */
    CHECK(nvmtl_linear_pitch_ok(256, 0));
    CHECK(!nvmtl_linear_pitch_ok(255, 0));
    /* nothing to alias, and a multiple of 256 that is not a whole number of texels */
    CHECK(!nvmtl_linear_pitch_ok(0, 4));
    CHECK(!nvmtl_linear_pitch_ok(0, 0));
    CHECK(nvmtl_linear_pitch_ok(768, 12));    /* 768 = 3 * 256 = 64 * 12 */
    CHECK(!nvmtl_linear_pitch_ok(256, 12));   /* a multiple of 256 that is not a whole number of 12 B texels */
    /* a pitch off by one texel never passes */
    CHECK(!nvmtl_linear_pitch_ok(256 + 4, 4));
    CHECK(!nvmtl_linear_pitch_ok(256 - 4, 4));
    /* 4 GiB - 256: the largest 32-bit multiple of 256 */
    CHECK(nvmtl_linear_pitch_ok((size_t)0xffffff00u, 4));
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_linear_pitch: ok\n");
    return 0;
}
