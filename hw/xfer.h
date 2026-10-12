/*
 * xfer.h - GPU copies between VRAM and scattered system pages through a
 * GART window: what amdgpu does with amdgpu_ttm_copy_mem_to_mem over a
 * GTT-bound buffer (amdgpu_ttm_map_buffer puts the pages in a GART window,
 * SDMA copies, the window is reused). Used for IOSurface page-off, where
 * the CPU reading uncached VRAM through the BAR is slow.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_XFER_H
#define RDNA3_XFER_H

#include "rdna3_hw.h"
#include "gmc11.h"
#include "sdma60.h"

#ifdef __cplusplus
extern "C" {
#endif

/* amdgpu_ttm_map_buffer's window size class: 4 MiB per chunk here */
#define RD_XFER_WINDOW_PAGES 1024u

/* Copy 'len' bytes from VRAM offset 'vram_off' into the system pages
 * 'pages' (bus addresses, 4 KiB each), RD_XFER_WINDOW_PAGES at a time
 * through the GART window at GART offset 'win_off' (win_pages pages,
 * owned by the caller and reused per chunk). The window is left unmapped.
 * The SDMA ring must be idle for the caller (it waits on each chunk). */
int rdna3_copy_vram_to_pages(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t vram_off,
                             const uint64_t *pages, uint64_t npages, uint64_t len,
                             uint64_t win_off, uint32_t win_pages);
/* the other way: the system pages back into VRAM (a BO evicted before a
 * sleep, amdgpu_bo_restore / the TTM move back after the resume) */
int rdna3_copy_pages_to_vram(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t vram_off,
                             const uint64_t *pages, uint64_t npages, uint64_t len,
                             uint64_t win_off, uint32_t win_pages);

#ifdef __cplusplus
}
#endif
#endif
