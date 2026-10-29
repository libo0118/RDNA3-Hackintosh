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

/* DRIVER-PLAN N63, the IOP copy by SDMA: a buffer of system pages bound to the GART for as long as its owner keeps
 * them wired (amdgpu_ttm_alloc_gart -> amdgpu_gart_bind of a GTT BO: amdgpu_gart_map with the TT PTE flags, then
 * amdgpu_gart_invalidate_tlb), unbound before the pages go (amdgpu_ttm_backend_unbind -> amdgpu_gart_unbind, TLB
 * flushed). Contiguous runs are written in one gmc11_gart_map. 'pages' are 4 KiB bus addresses; nothing is mapped
 * when one is not page aligned or the range leaves the GART. */
int  rdna3_gart_bind_pages(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, const uint64_t *pages, uint64_t npages);
void rdna3_gart_unbind_pages(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint64_t npages);
/* amdgpu_copy_buffer from such a bound buffer into VRAM, a pitched rectangle (sdma60_copy_rect_sync: one span when
 * the pitches match, one COPY_LINEAR per row otherwise), waited for: rows of row_bytes from byte src_off of the
 * buffer at GART offset gart_off (bound_bytes long), src_pitch apart, to VRAM offset vram_off, dst_pitch apart.
 * RDNA3_EINVAL when the rectangle leaves the buffer or VRAM. */
int  rdna3_copy_gart_rect_to_vram(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t gart_off,
                                  uint64_t bound_bytes, uint64_t src_off, uint32_t src_pitch, uint64_t vram_off,
                                  uint32_t dst_pitch, uint32_t row_bytes, uint32_t rows);

#ifdef __cplusplus
}
#endif
#endif
