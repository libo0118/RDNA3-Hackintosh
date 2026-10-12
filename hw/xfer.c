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
        /* the SDMA walks VMID0 through MMHUB: drop stale translations */
        mmhub302_flush_tlb(d, 0, 0);
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
        mmhub302_flush_tlb(d, 0, 0);
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
