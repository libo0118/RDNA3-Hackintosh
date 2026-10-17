/*
 * range_alloc.h - first-fit allocator over an address range with
 * coalescing frees. Used for the user-visible VRAM heap. Fixed capacity,
 * no allocation of its own, C99 without libc.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RANGE_ALLOC_H
#define RANGE_ALLOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* card run 2026-10-17c: 1024 free ranges were not enough for RADV's mix of
 * 4 KiB .. 36 MiB BOs; a full list made a split fail halfway (below) */
#define RANGE_ALLOC_MAX_FREE 4096
#define RANGE_ALLOC_FAIL     UINT64_MAX

typedef struct range_alloc {
    uint64_t base, end;
    uint32_t nfree;
    /* what the fixed free list cost, for the report: a put that found the
     * list full gives the range up (lost_*); a get that skipped a free
     * range only because splitting it needed a slot (full_skips) */
    uint32_t lost_ranges, full_skips;
    uint64_t lost_bytes;
    struct { uint64_t start, size; } fr[RANGE_ALLOC_MAX_FREE]; /* sorted by start */
} range_alloc;

void     range_alloc_init(range_alloc *ra, uint64_t base, uint64_t size);
uint64_t range_alloc_get(range_alloc *ra, uint64_t size, uint64_t align);
/* returns 0, or -1 if the range overlaps free space / is outside, or if
 * the free list is full (the range is then lost, see lost_bytes) */
int      range_alloc_put(range_alloc *ra, uint64_t start, uint64_t size);
uint64_t range_alloc_free_bytes(const range_alloc *ra);
/* the largest free range (what a contiguous BO can still get, ignoring
 * alignment) */
uint64_t range_alloc_largest_free(const range_alloc *ra);

#ifdef __cplusplus
}
#endif
#endif
