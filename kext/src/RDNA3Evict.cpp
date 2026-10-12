/*
 * RDNA3Evict.cpp - amdgpu_device_evict_resources for a system sleep.
 *
 * On S3 the card loses its power and VRAM with it. amdgpu moves every VRAM
 * BO to system memory before the sleep (TTM evictions through SDMA) and
 * back when it is used again; the clients never notice. Here, with
 * rdna3resume=1, before amdgpu_device_suspend runs (the SDMA ring and the
 * GART still up): every DRM BO placed in VRAM, the seq64 slots, the
 * accelerator's surfaces and the boot scanout are copied into wired system
 * pages through a GART window (rdna3_copy_vram_to_pages). After the resume,
 * if VRAM was lost, they go back (rdna3_copy_pages_to_vram) before the
 * display is restored; a VRAM loss that every range came back from is not
 * reported to the clients (amdgpu_device_resume does not check the reset
 * magic either). The page tables are not in the list: the resume rebuilds
 * them and the DRM maps its clients again (vmRebuildAfterVramLoss).
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"
#include "RDNA3Bringup.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

struct RDNA3EvictRange {
    uint64_t  off, len;
    RDNA3Gtt *g;   /* the system pages holding it, nullptr: not saved */
};

static void evictAdd(void *ctx, uint64_t off, uint64_t len)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (!len)
        return;
    if (a->fEvictN == a->fEvictCap) {
        uint32_t cap = a->fEvictCap ? a->fEvictCap * 2 : 64;
        RDNA3EvictRange *n = (RDNA3EvictRange *)IOMalloc(sizeof(*n) * cap);
        if (!n) {
            a->fEvictIncomplete = true;
            return;
        }
        if (a->fEvict) {
            memcpy(n, a->fEvict, sizeof(*n) * a->fEvictN);
            IOFree(a->fEvict, sizeof(*n) * a->fEvictCap);
        }
        a->fEvict = n;
        a->fEvictCap = cap;
    }
    a->fEvict[a->fEvictN].off = off;
    a->fEvict[a->fEvictN].len = (len + 4095) & ~4095ull;
    a->fEvict[a->fEvictN].g = nullptr;
    a->fEvictN++;
}

static void evictAddBo(void *ctx, rdna3_bo *bo)
{
    evictAdd(ctx, bo->vram_offset, bo->size);
}

void RDNA3Accel::vramEvict()
{
    uint64_t saved = 0, failed = 0;
    vramEvictFree();
    if (!fBringup || !fDrm)
        return;
    rdna3_drm_device_for_each_vram_bo(fDrm, evictAddBo, this);
    platformForEachVram(evictAdd, this);
    if (fDispReserveEnd)
        evictAdd(this, 0, fDispReserveEnd);   /* the boot scanout (the GOP's framebuffer) */
    for (uint32_t i = 0; i < fEvictN; i++) {
        RDNA3EvictRange *e = &fEvict[i];
        IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, e->len, (1ull << 44) - 4096);
        rdna3_bo tmp;
        bzero(&tmp, sizeof(tmp));
        tmp.size = e->len;
        if (!md || gttWire(&tmp, md, false) != 0) {   /* gttWire releases md on failure */
            failed += e->len;
            fEvictIncomplete = true;
            continue;
        }
        RDNA3Gtt *g = (RDNA3Gtt *)tmp.backing;
        if (fBringup->copyVramToPages(e->off, g->pages, g->npages, e->len) != 0) {
            gttFree(g);
            failed += e->len;
            fEvictIncomplete = true;
            continue;
        }
        e->g = g;
        saved += e->len;
    }
    LOG("suspend: %u VRAM range(s), %llu MiB saved to system memory%s", fEvictN,
        (unsigned long long)(saved >> 20), fEvictIncomplete ? ", some NOT (their contents go with the power)" : "");
    setNumber("RDNA3,EvictedMiB", saved >> 20);
    setNumber("RDNA3,EvictFailedMiB", failed >> 20);
}

void RDNA3Accel::vramRestore(bool vramLost)
{
    uint32_t back = 0, bad = 0;
    if (!fEvictN)
        return;
    if (vramLost && fBringup) {
        for (uint32_t i = 0; i < fEvictN; i++) {
            RDNA3EvictRange *e = &fEvict[i];
            if (!e->g)
                continue;
            if (fBringup->copyPagesToVram(e->off, e->g->pages, e->g->npages, e->len) == 0)
                back++;
            else
                bad++;
        }
        if (!bad && !fEvictIncomplete)
            fBringup->forgetVramLoss();   /* the clients' contents are all back */
        LOG("resume: %u VRAM range(s) back from system memory%s", back,
            bad || fEvictIncomplete ? ", some NOT: the clients see VRAM lost" : "");
        setNumber("RDNA3,EvictRestoreFailures", bad);
    }
    vramEvictFree();
}

void RDNA3Accel::vramEvictFree()
{
    for (uint32_t i = 0; i < fEvictN; i++)
        if (fEvict[i].g)
            gttFree(fEvict[i].g);
    if (fEvict)
        IOFree(fEvict, sizeof(*fEvict) * fEvictCap);
    fEvict = nullptr;
    fEvictN = fEvictCap = 0;
    fEvictIncomplete = false;
}
