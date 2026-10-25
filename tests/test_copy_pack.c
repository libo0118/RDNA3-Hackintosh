/* What a Metal blit copy between a texture and a buffer becomes in Vulkan
 * (metal/plugin/nvmtl_copy_pack.h): one aspect per region, the packing of
 * depth/stencil, no multisampled image (card run 2026-10-25c, Chess).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_copy_pack.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int run(uint32_t fmt, uint32_t samples, uint32_t img_bpp, uint32_t ov, uint64_t off, uint32_t *a, uint32_t *b)
{
    const char *why = NULL; int rc = nvmtl_cp_resolve(fmt, samples, img_bpp, ov, off, a, b, &why);
    CHECK(rc == 0 || why != NULL);
    return rc;
}

int main(void)
{
    uint32_t a = 0, b = 0;
    /* colour: the image's own element size, no aspect option */
    CHECK(run(37, 1, 4, 0, 0, &a, &b) == 0 && a == NVMTL_CP_ASPECT_COLOR && b == 4);
    CHECK(run(9, 1, 1, 0, 3, &a, &b) == 0 && a == NVMTL_CP_ASPECT_COLOR && b == 1);   /* colour offsets are not restricted here */
    CHECK(run(97, 1, 16, 0, 0, &a, &b) == 0 && b == 16);
    CHECK(run(37, 1, 0, 0, 0, &a, &b) == 0 && b == 4);
    /* a depth option on a colour image, and two aspects at once */
    CHECK(run(37, 1, 4, NVMTL_CP_ASPECT_DEPTH, 0, &a, &b) != 0);
    CHECK(run(NVMTL_CP_D32S8, 1, 4, NVMTL_CP_ASPECT_DEPTH | NVMTL_CP_ASPECT_STENCIL, 0, &a, &b) != 0);
    /* depth-only formats: depth, their own packing, whatever the image says */
    CHECK(run(NVMTL_CP_D16, 1, 4, 0, 0, &a, &b) == 0 && a == NVMTL_CP_ASPECT_DEPTH && b == 2);
    CHECK(run(NVMTL_CP_D32, 1, 4, 0, 0, &a, &b) == 0 && a == NVMTL_CP_ASPECT_DEPTH && b == 4);
    CHECK(run(NVMTL_CP_X8D24, 1, 4, 0, 0, &a, &b) == 0 && b == 4);
    CHECK(run(NVMTL_CP_D32, 1, 4, NVMTL_CP_ASPECT_STENCIL, 0, &a, &b) != 0);   /* no stencil in D32 */
    /* stencil-only: 1 B, without an option too (was the image's 4 B) */
    CHECK(run(NVMTL_CP_S8, 1, 4, 0, 0, &a, &b) == 0 && a == NVMTL_CP_ASPECT_STENCIL && b == 1);
    CHECK(run(NVMTL_CP_S8, 1, 4, NVMTL_CP_ASPECT_DEPTH, 0, &a, &b) != 0);
    /* combined formats: one aspect per region, depth when no option names one */
    CHECK(run(NVMTL_CP_D32S8, 1, 4, 0, 0, &a, &b) == 0 && a == NVMTL_CP_ASPECT_DEPTH && b == 4);
    CHECK(run(NVMTL_CP_D32S8, 1, 4, NVMTL_CP_ASPECT_DEPTH, 8, &a, &b) == 0 && a == NVMTL_CP_ASPECT_DEPTH && b == 4);
    CHECK(run(NVMTL_CP_D32S8, 1, 4, NVMTL_CP_ASPECT_STENCIL, 8, &a, &b) == 0 && a == NVMTL_CP_ASPECT_STENCIL && b == 1);
    CHECK(run(NVMTL_CP_D24S8, 1, 4, NVMTL_CP_ASPECT_DEPTH, 0, &a, &b) == 0 && b == 4);   /* D24 copies as X8_D24 */
    CHECK(run(NVMTL_CP_D24S8, 1, 4, NVMTL_CP_ASPECT_STENCIL, 0, &a, &b) == 0 && b == 1);
    CHECK(run(NVMTL_CP_D16S8, 1, 4, NVMTL_CP_ASPECT_DEPTH, 0, &a, &b) == 0 && b == 2);
    /* the buffer offset of a depth/stencil copy is a multiple of 4 */
    CHECK(run(NVMTL_CP_D32S8, 1, 4, NVMTL_CP_ASPECT_STENCIL, 2, &a, &b) != 0);
    CHECK(run(NVMTL_CP_S8, 1, 4, 0, 1, &a, &b) != 0);
    CHECK(run(NVMTL_CP_D32, 1, 4, 0, 4, &a, &b) == 0);
    CHECK(run(NVMTL_CP_D32, 1, 4, 0, (uint64_t)1 << 40, &a, &b) == 0);
    /* multisampled images are refused whatever the format (the Chess case: a 4-sample image) */
    CHECK(run(37, 4, 4, 0, 0, &a, &b) != 0);
    CHECK(run(NVMTL_CP_D32S8, 4, 4, NVMTL_CP_ASPECT_DEPTH, 0, &a, &b) != 0);
    CHECK(run(NVMTL_CP_D32, 2, 4, 0, 0, &a, &b) != 0);
    CHECK(run(37, 8, 4, 0, 0, &a, &b) != 0);
    /* a refusal leaves a reason */
    { const char *why = NULL; CHECK(nvmtl_cp_resolve(37, 4, 4, 0, 0, &a, &b, &why) != 0 && why && why[0]); }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_copy_pack: ok\n");
    return 0;
}
