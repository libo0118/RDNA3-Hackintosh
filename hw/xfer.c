/*
 * xfer.c - see xfer.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "xfer.h"

static int xfer(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t vram_off, const uint64_t *pages,
                uint64_t npages, uint64_t len, uint64_t win_off, uint32_t win_pages, int to_vram)
{
    uint64_t done = 0, page = 0;
    /* GTT pages as amdgpu maps them: system, snooped, read/write */
    const uint64_t flags = RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                           RD_PTE_WRITEABLE | RD_PTE_MTYPE_NV10(RD_MTYPE_UC);
    int ret = RDNA3_OK;

    if (!g || !r || !r->ready || !pages || !len || npages * 4096 < len || !win_pages ||
        (win_off & 4095) || win_off + (uint64_t)win_pages * 4096 > g->gart_size)
        return RDNA3_EINVAL;
    if (win_pages > RD_XFER_WINDOW_PAGES)
        win_pages = RD_XFER_WINDOW_PAGES;
    /* every page the copy touches is page aligned, before anything is
     * mapped (a refusal half way would leave the window mapped) */
    for (page = 0; page * 4096 < len; page++)
        if (pages[page] & 4095)
            return RDNA3_EINVAL;
    page = 0;

    while (done < len) {
        uint32_t n = 0, i;
        uint64_t chunk;
        /* map up to a window of pages (each page on its own: they are not
         * contiguous) */
        for (; n < win_pages && page + n < npages && done + (uint64_t)n * 4096 < len; n++) {
            ret = gmc11_gart_map(d, g, win_off + (uint64_t)n * 4096, pages[page + n], 1, flags);
            if (ret != RDNA3_OK)
                break;
        }
        if (ret != RDNA3_OK) {
            gmc11_gart_unmap(d, g, win_off, n);
            break;
        }
        /* drop stale translations of the window (DRIVER-PLAN N54): SDMA 6.0
         * walks VMID0 through the GFXHUB (sdma_v6_0_sw_init: ring->vm_hub =
         * AMDGPU_GFXHUB(0)), so amdgpu_gart_invalidate_tlb flushes every hub;
         * the MMHUB alone left the previous copy's pages in the GFXHUB's
         * TLB and the SDMA copied to / from them */
        gmc11_gart_tlb_flush(d, g);
        chunk = (uint64_t)n * 4096;
        if (chunk > len - done)
            chunk = len - done;
        for (i = 0; ret == RDNA3_OK && (uint64_t)i * (1u << 22) < chunk; i++) {
            uint64_t off = (uint64_t)i * (1u << 22);
            uint32_t bytes = (uint32_t)(chunk - off < (1u << 22) ? chunk - off : (1u << 22));
            uint64_t vmc = rdna3_vram_mc(d, vram_off + done + off), gmc = g->gart_start + win_off + off;
            ret = to_vram ? sdma60_copy_sync(d, r, gmc, vmc, bytes) : sdma60_copy_sync(d, r, vmc, gmc, bytes);
        }
        gmc11_gart_unmap(d, g, win_off, n);
        gmc11_gart_tlb_flush(d, g);
        if (ret != RDNA3_OK)
            break;
        done += chunk;
        page += n;
    }
    return ret;
}

int rdna3_copy_vram_to_pages(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t vram_off,
                             const uint64_t *pages, uint64_t npages, uint64_t len,
                             uint64_t win_off, uint32_t win_pages)
{
    return xfer(d, g, r, vram_off, pages, npages, len, win_off, win_pages, 0);
}

int rdna3_copy_pages_to_vram(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t vram_off,
                             const uint64_t *pages, uint64_t npages, uint64_t len,
                             uint64_t win_off, uint32_t win_pages)
{
    return xfer(d, g, r, vram_off, pages, npages, len, win_off, win_pages, 1);
}

/* the TT PTE flags xfer() maps with: system, snooped, read/write, MTYPE_UC */
static const uint64_t kGttFlags = RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                                  RD_PTE_WRITEABLE | RD_PTE_MTYPE_NV10(RD_MTYPE_UC);

int rdna3_gart_bind_pages(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, const uint64_t *pages, uint64_t npages)
{
    uint64_t i = 0;
    if (!g || !pages || !npages || (gart_off & 4095) || gart_off > g->gart_size ||
        npages > (g->gart_size - gart_off) / 4096)
        return RDNA3_EINVAL;
    for (i = 0; i < npages; i++)
        if (pages[i] & 4095)
            return RDNA3_EINVAL;
    for (i = 0; i < npages;) {
        uint64_t run = 1;
        int ret;
        while (i + run < npages && run < 0xffffffffull && pages[i + run] == pages[i] + run * 4096)
            run++;
        ret = gmc11_gart_map(d, g, gart_off + i * 4096, pages[i], (uint32_t)run, kGttFlags);
        if (ret != RDNA3_OK) {
            gmc11_gart_unmap(d, g, gart_off, (uint32_t)i);
            gmc11_gart_tlb_flush(d, g);
            return ret;
        }
        i += run;
    }
    /* amdgpu_gart_invalidate_tlb: every hub (SDMA 6.0 walks VMID0 through the GFXHUB, DRIVER-PLAN N54) */
    gmc11_gart_tlb_flush(d, g);
    return RDNA3_OK;
}

void rdna3_gart_unbind_pages(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint64_t npages)
{
    if (!g || !npages || (gart_off & 4095) || gart_off > g->gart_size)
        return;
    if (npages > (g->gart_size - gart_off) / 4096)
        npages = (g->gart_size - gart_off) / 4096;
    gmc11_gart_unmap(d, g, gart_off, (uint32_t)npages);
    gmc11_gart_tlb_flush(d, g);
}

int rdna3_copy_gart_rect_to_vram(rdna3_dev *d, rdna3_gmc *g, rdna3_sdma_ring *r, uint64_t gart_off,
                                 uint64_t bound_bytes, uint64_t src_off, uint32_t src_pitch, uint64_t vram_off,
                                 uint32_t dst_pitch, uint32_t row_bytes, uint32_t rows)
{
    uint64_t span_s, span_d;
    if (!g || !r || !r->ready || !rows || !row_bytes || row_bytes > src_pitch || row_bytes > dst_pitch ||
        gart_off > g->gart_size || bound_bytes > g->gart_size - gart_off || src_off >= bound_bytes)
        return RDNA3_EINVAL;
    span_s = (uint64_t)(rows - 1) * src_pitch + row_bytes;
    span_d = (uint64_t)(rows - 1) * dst_pitch + row_bytes;
    if (span_s > bound_bytes - src_off || vram_off > d->vram_size || span_d > d->vram_size - vram_off)
        return RDNA3_EINVAL;
    return sdma60_copy_rect_sync(d, r, g->gart_start + gart_off + src_off, src_pitch, rdna3_vram_mc(d, vram_off),
                                 dst_pitch, row_bytes, rows);
}
