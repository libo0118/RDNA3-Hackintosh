/* Randomised test of common/range_alloc against a bitmap model. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "range_alloc.h"

#define PAGES 4096
static unsigned char used[PAGES];
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    static range_alloc ra;
    struct { uint64_t off, size; } live[512];
    unsigned nlive = 0, it, i;
    range_alloc_init(&ra, 0x100000, (uint64_t)PAGES * 4096);
    srand(3);
    for (it = 0; it < 200000; it++) {
        if (nlive < 512 && (rand() % 3)) {
            uint64_t size = (uint64_t)(1 + rand() % 64) * 4096;
            uint64_t align = 4096ull << (rand() % 5);
            uint64_t off = range_alloc_get(&ra, size, align);
            if (off == RANGE_ALLOC_FAIL)
                continue;
            CHECK(off % align == 0 && off >= 0x100000);
            for (i = 0; i < size / 4096; i++) {
                unsigned p = (unsigned)((off - 0x100000) / 4096 + i);
                CHECK(p < PAGES && !used[p]);
                used[p] = 1;
            }
            live[nlive].off = off; live[nlive].size = size; nlive++;
        } else if (nlive) {
            unsigned k = (unsigned)rand() % nlive;
            CHECK(range_alloc_put(&ra, live[k].off, live[k].size) == 0);
            CHECK(range_alloc_put(&ra, live[k].off, live[k].size) == -1); /* double free */
            for (i = 0; i < live[k].size / 4096; i++)
                used[(live[k].off - 0x100000) / 4096 + i] = 0;
            live[k] = live[--nlive];
        }
    }
    while (nlive) { nlive--; CHECK(range_alloc_put(&ra, live[nlive].off, live[nlive].size) == 0); }
    CHECK(ra.nfree == 1 && range_alloc_free_bytes(&ra) == (uint64_t)PAGES * 4096);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_range_alloc: all checks passed\n");
    return 0;
}
