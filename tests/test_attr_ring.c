/* DRIVER-PLAN N57: when a DISCARDABLE BO (the GFX11 attribute ring) holds what SPI_ATTRIBUTE_RING_SIZE.BIG_PAGE assumes
 * (common/rdna3_attr_ring.h; the kext's `GEM DISCARDABLE` / `GEM_VA DISCARDABLE` log lines, tools/triage_reports.py `attribute ring`).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "rdna3_attr_ring.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    const uint64_t va = 0x00000000c0000000ull, pa = 0x0000008123400000ull;   /* both 2 MiB aligned */
    /* amdgpu's ring: VRAM, 2 MiB aligned VA and VRAM, fragments >= 64 KiB */
    CHECK(rdna3_bigpage_problems(1, va, pa, 9) == 0);
    CHECK(rdna3_bigpage_problems(1, va, pa, RD_BIGPAGE_FRAG) == 0);
    /* card run 2026-10-26h, the ring as our default GPUVM path maps it since then (VA 0x800000200000 -> PA 0x1a200000,
     * smallest fragment 5) and as it did before (rdna3vmfrag=0: 4 KiB PTEs, fragment 0) */
    CHECK(rdna3_bigpage_problems(1, 0x800000200000ull, 0x1a200000ull, 5) == 0);
    CHECK(rdna3_bigpage_problems(1, 0x800000200000ull, 0x1a200000ull, 0) == RD_BIGPAGE_FRAG_SMALL);
    CHECK(rdna3_bigpage_problems(1, va, pa, 0) == RD_BIGPAGE_FRAG_SMALL);
    CHECK(rdna3_bigpage_problems(1, va, pa, 3) == RD_BIGPAGE_FRAG_SMALL);
    /* VRAM only 4 KiB aligned under a 64 KiB aligned VA: one 64 KiB translation spans two physical 64 KiB pages */
    CHECK(rdna3_bigpage_problems(1, va, pa + 0x3000, 9) == RD_BIGPAGE_PA_SKEW);
    /* congruent but not 64 KiB aligned: the VA itself */
    CHECK(rdna3_bigpage_problems(1, va + 0x1000, pa + 0x1000, 9) == RD_BIGPAGE_VA_ALIGN);
    CHECK(rdna3_bigpage_problems(1, va + 0x1000, pa, 0) == (RD_BIGPAGE_VA_ALIGN | RD_BIGPAGE_PA_SKEW | RD_BIGPAGE_FRAG_SMALL));
    /* congruent modulo 64 KiB with a different 2 MiB offset is fine */
    CHECK(rdna3_bigpage_problems(1, va, pa + 0x10000, 4) == 0);
    /* GTT is always wrong: scattered system pages */
    CHECK(rdna3_bigpage_problems(0, va, 0, 0) == (RD_BIGPAGE_NOT_VRAM | RD_BIGPAGE_FRAG_SMALL));
    CHECK(rdna3_bigpage_problems(0, va, 0, 9) == RD_BIGPAGE_NOT_VRAM);
    if (failures)
        return 1;
    printf("test_attr_ring: ok\n");
    return 0;
}
