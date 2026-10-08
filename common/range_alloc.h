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

#define RANGE_ALLOC_MAX_FREE 1024
#define RANGE_ALLOC_FAIL     UINT64_MAX

typedef struct range_alloc {
    uint64_t base, end;
    uint32_t nfree;
    struct { uint64_t start, size; } fr[RANGE_ALLOC_MAX_FREE]; /* sorted by start */
} range_alloc;

void     range_alloc_init(range_alloc *ra, uint64_t base, uint64_t size);
uint64_t range_alloc_get(range_alloc *ra, uint64_t size, uint64_t align);
/* returns 0, or -1 if the range overlaps free space / is outside */
int      range_alloc_put(range_alloc *ra, uint64_t start, uint64_t size);
uint64_t range_alloc_free_bytes(const range_alloc *ra);

#ifdef __cplusplus
}
#endif
#endif
