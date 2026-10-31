/* The decision behind deferring a drawable's copy-out to its present (metal/plugin/nvmtl_drawable_defer.h, DRIVER-PLAN N69).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_drawable_defer.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    for (int once = 0; once < 2; once++) for (int here = 0; here < 2; here++) {
        CHECK(!nvmtl_drawable_defer(0, once, here));                         /* switch off: nothing is left out */
        CHECK(nvmtl_drawable_defer(1, once, here) == (once && !here));       /* only a drawable, only in a command buffer that does not present it */
    }
    CHECK(!nvmtl_drawable_defer(1, 0, 0));    /* a texture never presented (every non-drawable surface) is always copied out */
    CHECK(!nvmtl_drawable_defer(1, 1, 1));    /* the presenting command buffer copies it */
    CHECK(nvmtl_drawable_defer(1, 1, 0));     /* an intermediate command buffer writing a drawable does not */
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    puts("drawable defer: ok");
    return 0;
}
