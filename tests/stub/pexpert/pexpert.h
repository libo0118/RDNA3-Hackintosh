/*
 * pexpert.h stub for host tests of common/rdna3_bootargs.h: the boot-args
 * come from a table the test fills (a NULL name ends it); a name without a
 * value reads as 1, as XNU's PE_parse_boot_argn does for a bare flag.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_TEST_PEXPERT_STUB_H
#define RDNA3_TEST_PEXPERT_STUB_H

#include <stdint.h>
#include <string.h>

struct test_bootarg { const char *name; uint32_t value; };
extern const struct test_bootarg *test_bootargs;

static inline int PE_parse_boot_argn(const char *name, void *out, int size)
{
    const struct test_bootarg *a;
    for (a = test_bootargs; a && a->name; a++)
        if (!strcmp(a->name, name)) {
            if (size >= (int)sizeof(uint32_t))
                memcpy(out, &a->value, sizeof(uint32_t));
            return 1;
        }
    return 0;
}

#endif
