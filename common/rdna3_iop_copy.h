/*
 * rdna3_iop_copy.h - the display pipe's (IOP) CPU copy of a presented IOSurface into the scanout, and what it costs
 * (DRIVER-PLAN N61). Plain C, no kernel headers: accel/rdna3-accel.cpp (RD3DisplayPipe::performTransaction) copies
 * with it, tests/test_iop_copy.c checks it on the host.
 *
 * Every frame WindowServer presents reaches the panel through this copy (rdna3acceliop=1, iop_flip 0): the surface's
 * system memory (cached, read through the kernel's map of its memory descriptor) into the head's scanout VRAM through
 * BAR0 (a write-combined map, fb/rdna3-fb.cpp RD3K_VRAM_FN_SCANOUT). Card run 2026-10-28 counted no time for it. The
 * copy was 32-bit volatile loads and stores, one per pixel; a row whose source, destination, pitches and length are
 * all 8-byte aligned now moves 8 bytes per access (the same bytes in the same order, half the bus transactions; a
 * write-combined BAR merges them either way, an uncached one would not). Anything else keeps the 32-bit loop.
 *
 * Per head: copies, bytes, the copy's time, the longest, and the time the transaction spent in its gates before the
 * copy (the N59 stale wait, the VRAM page-off wait) - debug.rdna3accel_iop_copy_* / iop_gate_us.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_IOP_COPY_H
#define RDNA3_IOP_COPY_H

#include <stdint.h>

/* the 64-bit loop applies: every address and length it touches is 8-byte aligned */
static inline int rd3_iop_copy64_ok(uintptr_t src, uintptr_t dst, uint32_t spitch, uint32_t dpitch, uint32_t row_bytes)
{
    return !((src | dst) & 7u) && !((spitch | dpitch | row_bytes) & 7u);
}

/* copy `h` rows of `row_bytes` (a multiple of 4) from src (pitch spitch) to dst (pitch dpitch); returns 8 when the
 * 64-bit loop ran, 4 for the 32-bit one. Rows are stepped by bytes: the caller passes pitches of whole 32-bit words
 * (performTransaction refuses others), which is where the old word-stepped loop put them. */
static inline int rd3_iop_copy_rows(volatile void *dst, uint32_t dpitch, const volatile void *src, uint32_t spitch,
                                    uint32_t h, uint32_t row_bytes)
{
    if (rd3_iop_copy64_ok((uintptr_t)src, (uintptr_t)dst, spitch, dpitch, row_bytes)) {
        const uint32_t q = row_bytes / 8u;
        for (uint32_t y = 0; y < h; y++) {
            const volatile uint64_t *sp = (const volatile uint64_t *)((const volatile uint8_t *)src + (uint64_t)y * spitch);
            volatile uint64_t *dp = (volatile uint64_t *)((volatile uint8_t *)dst + (uint64_t)y * dpitch);
            for (uint32_t x = 0; x < q; x++) dp[x] = sp[x];
        }
        return 8;
    }
    const uint32_t words = row_bytes / 4u;
    for (uint32_t y = 0; y < h; y++) {
        const volatile uint32_t *sp = (const volatile uint32_t *)((const volatile uint8_t *)src + (uint64_t)y * spitch);
        volatile uint32_t *dp = (volatile uint32_t *)((volatile uint8_t *)dst + (uint64_t)y * dpitch);
        for (uint32_t x = 0; x < words; x++) dp[x] = sp[x];
    }
    return 4;
}

/* MB/s of `bytes` in `us` microseconds (0 when no time was measured) */
static inline uint64_t rd3_iop_mbps(uint64_t bytes, uint64_t us)
{
    return us ? bytes / us : 0;   /* bytes per us = MB/s (10^6) */
}

/* a log line is due for the k-th (0-based) copy: the first 4, then every 1024th */
static inline int rd3_iop_copy_log_due(uint64_t k) { return k < 4u || (k & 1023u) == 0; }

#endif
