/*
 * rdna3_attr_ring.h - does a DISCARDABLE BO hold what the GFX11 attribute
 * ring's SPI_ATTRIBUTE_RING_SIZE.BIG_PAGE assumes (DRIVER-PLAN N57)?
 * Plain C, no kernel headers: the kext's GEM create / GEM_VA logs use it and
 * tests/test_attr_ring.c checks it on the host.
 *
 * Mesa allocates the ring (radv_queue.c, ge_rings_bo: 3 SE x 1408 KiB on
 * Navi 32, 2 MiB aligned, VRAM, RADEON_FLAG_DISCARDABLE | 32BIT |
 * NO_CPU_ACCESS) and sets BIG_PAGE from discardable_allows_big_page
 * (ac_gpu_info.c: gfx10.3..gfx11 with dedicated VRAM; "VRAM is only
 * guaranteed with AMDGPU_GEM_CREATE_DISCARDABLE"). With BIG_PAGE the SPI
 * translates the ring in 64 KiB pages: one translation serves 64 KiB of VA.
 * That is right only when the ring is VRAM (system pages are scattered
 * 4 KiB pages), the VA is 64 KiB aligned, VA and physical address are
 * congruent modulo 64 KiB (one 64 KiB VA page = one 64 KiB physical page),
 * and the PTEs say so: amdgpu maps VRAM with fragments
 * (amdgpu_vm_pte_fragment, AMDGPU_PTE_FRAG >= 4 = 64 KiB); our GPUVM writes
 * fragment 0 (4 KiB) PTEs unless rdna3amdgpu=1.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_ATTR_RING_H
#define RDNA3_ATTR_RING_H

#include <stdint.h>

#define RD_BIGPAGE_BYTES (64ull << 10)
#define RD_BIGPAGE_FRAG 4u   /* AMDGPU_PTE_FRAG: 2^4 pages of 4 KiB = 64 KiB */

#define RD_BIGPAGE_NOT_VRAM   1u   /* GTT: scattered system pages */
#define RD_BIGPAGE_VA_ALIGN   2u   /* the VA is not 64 KiB aligned */
#define RD_BIGPAGE_PA_SKEW    4u   /* VA - PA is not a multiple of 64 KiB */
#define RD_BIGPAGE_FRAG_SMALL 8u   /* the PTEs' fragment covers less than 64 KiB */

/* 0 when a mapping of a DISCARDABLE BO at va, backed from pa (VRAM: one
 * contiguous range; pa ignored otherwise), with PTE fragment frag holds
 * what BIG_PAGE assumes; otherwise RD_BIGPAGE_* bits. */
static inline unsigned rdna3_bigpage_problems(int vram, uint64_t va, uint64_t pa, unsigned frag)
{
    unsigned p = 0;
    if (!vram)
        return RD_BIGPAGE_NOT_VRAM | (frag < RD_BIGPAGE_FRAG ? RD_BIGPAGE_FRAG_SMALL : 0u) |
               ((va & (RD_BIGPAGE_BYTES - 1)) ? RD_BIGPAGE_VA_ALIGN : 0u);
    if (va & (RD_BIGPAGE_BYTES - 1))
        p |= RD_BIGPAGE_VA_ALIGN;
    if ((va - pa) & (RD_BIGPAGE_BYTES - 1))
        p |= RD_BIGPAGE_PA_SKEW;
    if (frag < RD_BIGPAGE_FRAG)
        p |= RD_BIGPAGE_FRAG_SMALL;
    return p;
}

#endif
