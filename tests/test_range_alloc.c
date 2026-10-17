/* Randomised test of common/range_alloc against a bitmap model. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "range_alloc.h"

#define PAGES 4096
static unsigned char used[PAGES];
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* card run 2026-10-17c: a full free list. The old split cut the free range to
 * [s,a) before it found no slot for the tail, so the tail (15 GiB of a 16 GiB
 * heap) was gone and the refusal said "252472 KiB free". A get that needs a
 * slot it does not have now changes nothing; a put into a full list says it
 * lost the range. */
static void test_full_list(void)
{
    static range_alloc ra;
    const uint64_t base = 0x1000000, page = 4096;
    const unsigned n = 9000;
    const uint64_t total = (uint64_t)20000 * page;
    unsigned i, lost = 0;
    uint64_t freeb, off;
    range_alloc_init(&ra, base, total);
    for (i = 0; i < n; i++)
        CHECK(range_alloc_get(&ra, page, page) == base + (uint64_t)i * page);
    /* every other page back: n/2 separate holes, more than the list holds */
    for (i = 0; i < n; i += 2)
        if (range_alloc_put(&ra, base + (uint64_t)i * page, page))
            lost++;
    CHECK(ra.nfree == RANGE_ALLOC_MAX_FREE);
    CHECK(lost == n / 2 - (RANGE_ALLOC_MAX_FREE - 1));         /* the tail's chunk holds the other slot */
    CHECK(ra.lost_ranges == lost && ra.lost_bytes == (uint64_t)lost * page);
    /* every byte is free, live (the odd pages) or counted lost */
    freeb = range_alloc_free_bytes(&ra);
    CHECK(freeb + ra.lost_bytes + (uint64_t)(n / 2) * page == total);
    /* the tail [9000 pages, end) starts 8 pages past a 64 KiB boundary: an aligned request needs a split slot */
    CHECK(range_alloc_largest_free(&ra) == total - (uint64_t)n * page);
    off = range_alloc_get(&ra, 4 * page, 16 * page);
    CHECK(off == RANGE_ALLOC_FAIL);
    CHECK(range_alloc_free_bytes(&ra) == freeb && ra.nfree == RANGE_ALLOC_MAX_FREE);   /* nothing changed */
    CHECK(ra.full_skips >= 1);
    CHECK(range_alloc_largest_free(&ra) == total - (uint64_t)n * page);
    /* the same size at the range's own alignment needs no slot */
    off = range_alloc_get(&ra, 4 * page, page);
    CHECK(off == base + (uint64_t)n * page);
    CHECK(range_alloc_free_bytes(&ra) == freeb - 4 * page);
    /* a free next to the free tail merges and needs no slot */
    CHECK(range_alloc_put(&ra, off, 4 * page) == 0);
    CHECK(range_alloc_free_bytes(&ra) == freeb);
    /* a free between two live pages needs a slot: refused, counted, nothing else changes */
    CHECK(range_alloc_put(&ra, base + 8191 * page, page) == -1);
    CHECK(ra.lost_ranges == lost + 1 && ra.lost_bytes == (uint64_t)(lost + 1) * page);
    CHECK(range_alloc_free_bytes(&ra) == freeb);
    /* one between two holes merges them: room again, and the list shrinks by one */
    CHECK(range_alloc_put(&ra, base + 1 * page, page) == 0);
    CHECK(ra.nfree == RANGE_ALLOC_MAX_FREE - 1);
}

int main(void)
{
    static range_alloc ra;
    test_full_list();
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
