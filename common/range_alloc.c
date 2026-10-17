/*
 * range_alloc.c - see range_alloc.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "range_alloc.h"

void range_alloc_init(range_alloc *ra, uint64_t base, uint64_t size)
{
    ra->base = base;
    ra->end = base + size;
    ra->nfree = size ? 1 : 0;
    ra->lost_ranges = ra->full_skips = 0;
    ra->lost_bytes = 0;
    ra->fr[0].start = base;
    ra->fr[0].size = size;
}

static void remove_at(range_alloc *ra, uint32_t i)
{
    for (; i + 1 < ra->nfree; i++)
        ra->fr[i] = ra->fr[i + 1];
    ra->nfree--;
}

static int insert_at(range_alloc *ra, uint32_t i, uint64_t start, uint64_t size)
{
    uint32_t j;
    if (ra->nfree >= RANGE_ALLOC_MAX_FREE)
        return -1;
    for (j = ra->nfree; j > i; j--)
        ra->fr[j] = ra->fr[j - 1];
    ra->fr[i].start = start;
    ra->fr[i].size = size;
    ra->nfree++;
    return 0;
}

uint64_t range_alloc_get(range_alloc *ra, uint64_t size, uint64_t align)
{
    uint32_t i;
    if (!size || (align & (align - 1)))
        return RANGE_ALLOC_FAIL;
    if (!align)
        align = 1;
    for (i = 0; i < ra->nfree; i++) {
        uint64_t s = ra->fr[i].start, e = s + ra->fr[i].size;
        uint64_t a = (s + align - 1) & ~(align - 1);
        if (a < s || a > e || e - a < size)
            continue;
        /* split: [s,a) stays free, [a,a+size) used, [a+size,e) free. With
         * the list full the middle split has no slot for the tail: this
         * range is skipped with nothing changed (card run 2026-10-17c: the
         * old code had already cut the range to [s,a) when the insert
         * failed, so the tail - 15 GiB of a 16 GiB heap - was gone for good) */
        if (a + size < e) {
            if (a > s) {
                if (ra->nfree >= RANGE_ALLOC_MAX_FREE) {
                    ra->full_skips++;
                    continue;
                }
                ra->fr[i].size = a - s;
                if (insert_at(ra, i + 1, a + size, e - (a + size)))
                    return RANGE_ALLOC_FAIL;   /* cannot happen: checked above */
            } else {
                ra->fr[i].start = a + size;
                ra->fr[i].size = e - (a + size);
            }
        } else if (a > s) {
            ra->fr[i].size = a - s;
        } else {
            remove_at(ra, i);
        }
        return a;
    }
    return RANGE_ALLOC_FAIL;
}

int range_alloc_put(range_alloc *ra, uint64_t start, uint64_t size)
{
    uint32_t i = 0;
    uint64_t end = start + size;
    if (!size || start < ra->base || end > ra->end || end < start)
        return -1;
    while (i < ra->nfree && ra->fr[i].start < start)
        i++;
    /* overlap checks with neighbours */
    if (i > 0 && ra->fr[i - 1].start + ra->fr[i - 1].size > start)
        return -1;
    if (i < ra->nfree && ra->fr[i].start < end)
        return -1;
    /* merge left / right */
    if (i > 0 && ra->fr[i - 1].start + ra->fr[i - 1].size == start) {
        ra->fr[i - 1].size += size;
        if (i < ra->nfree && ra->fr[i].start == end) {
            ra->fr[i - 1].size += ra->fr[i].size;
            remove_at(ra, i);
        }
        return 0;
    }
    if (i < ra->nfree && ra->fr[i].start == end) {
        ra->fr[i].start = start;
        ra->fr[i].size += size;
        return 0;
    }
    if (insert_at(ra, i, start, size)) {
        ra->lost_ranges++;
        ra->lost_bytes += size;
        return -1;
    }
    return 0;
}

uint64_t range_alloc_free_bytes(const range_alloc *ra)
{
    uint64_t t = 0;
    uint32_t i;
    for (i = 0; i < ra->nfree; i++)
        t += ra->fr[i].size;
    return t;
}

uint64_t range_alloc_largest_free(const range_alloc *ra)
{
    uint64_t m = 0;
    uint32_t i;
    for (i = 0; i < ra->nfree; i++)
        if (ra->fr[i].size > m)
            m = ra->fr[i].size;
    return m;
}
