/*
 * RDNA3Bringup.cpp - see RDNA3Bringup.hpp.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Bringup.hpp"
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "smu13.h"
#include "navi3x_regs.h"
}

/* kpi.mach; declared here to avoid depending on private headers */
extern "C" kern_return_t kernel_thread_start(thread_continue_t continuation,
                                             void *parameter, thread_t *new_thread);
extern "C" void thread_deallocate(thread_t thread);

#define BLOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

static const char *kBundleID = "io.github.voten641.RDNA3Accel";

/* --------------------------- rdna3_os --------------------------- */

void RDNA3Bringup::osUdelay(void *, uint32_t us) { IODelay(us); }
void RDNA3Bringup::osMsleep(void *, uint32_t ms) { IOSleep(ms); }
void RDNA3Bringup::osWmb(void *) { OSSynchronizeIO(); __asm__ volatile("mfence" ::: "memory"); }

void RDNA3Bringup::osVlog(void *, int level, const char *fmt, va_list ap)
{
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    IOLog("RDNA3: %s%s\n", level == RDNA3_LOG_ERR ? "ERROR: " : "", buf);
}

uint32_t RDNA3Bringup::osMmioRd(void *ctx, uint64_t off)
{
    RDNA3Bringup *b = (RDNA3Bringup *)ctx;
    return *(volatile uint32_t *)(b->fMmioBase + off);
}

void RDNA3Bringup::osMmioWr(void *ctx, uint64_t off, uint32_t v)
{
    RDNA3Bringup *b = (RDNA3Bringup *)ctx;
    *(volatile uint32_t *)(b->fMmioBase + off) = v;
}

/* --------------------------- lifecycle --------------------------- */

bool RDNA3Bringup::init(RDNA3Accel *owner, IOPCIDevice *pci, IOMemoryMap *mmio,
                        uint32_t level, const amd_discovery *disc, uint64_t vramSize)
{
    fOwner = owner;
    fPci = pci;
    fMmio = mmio;
    fMmioBase = (volatile uint8_t *)mmio->getVirtualAddress();
    fLevel = level;
    fVramSize = vramSize;
    {
        uint32_t msi = 0, db = 1;
        PE_parse_boot_argn("rdna3msi", &msi, sizeof(msi));
        PE_parse_boot_argn("rdna3doorbell", &db, sizeof(db));
        fMsi = msi != 0;
        fDoorbells = db != 0;
    }
    fLock = IOLockAlloc();
    fDisc = (amd_discovery *)IOMalloc(sizeof(*fDisc));
    fDev = (rdna3_dev *)IOMalloc(sizeof(*fDev));
    fPsp = (rdna3_psp *)IOMalloc(sizeof(*fPsp));
    fPlan = (amd_ucode_plan *)IOMalloc(sizeof(*fPlan));
    if (!fLock || !fDisc || !fDev || !fPsp || !fPlan)
        return false;
    memcpy(fDisc, disc, sizeof(*fDisc));
    return true;
}

void RDNA3Bringup::start()
{
    thread_t t;
    fRunning = true;
    if (kernel_thread_start((thread_continue_t)&RDNA3Bringup::threadMain, this, &t) !=
        KERN_SUCCESS) {
        BLOG("cannot start bring-up thread");
        fRunning = false;
        fFinished = true;
        return;
    }
    thread_deallocate(t);
}

void RDNA3Bringup::threadMain(void *arg, int)
{
    RDNA3Bringup *b = (RDNA3Bringup *)arg;
    b->run();
    IOLockLock(b->fLock);
    b->fFinished = true;
    IOLockWakeup(b->fLock, &b->fFinished, false);
    IOLockUnlock(b->fLock);
    thread_terminate(current_thread());
}

void RDNA3Bringup::stopAndFree()
{
    fAbort = true;
    if (fLock && fRunning) {
        IOLockLock(fLock);
        while (!fFinished)
            IOLockSleep(fLock, &fFinished, THREAD_UNINT);
        IOLockUnlock(fLock);
    }
    teardownInterrupt();   /* no handler may touch fIh / the rings past here */
    if (fPsp && fPsp->ring_created)
        psp13_ring_stop(fPsp);
    OSSafeReleaseNULL(fVram);
    OSSafeReleaseNULL(fDoorbell);
    for (unsigned i = 0; i < AMD_BLOB_COUNT; i++)
        if (fBlobs[i].data)
            IOFree(fBlobs[i].data, fBlobs[i].len);
    if (fSos.data)
        IOFree(fSos.data, fSos.len);
    if (fDisc) IOFree(fDisc, sizeof(*fDisc));
    if (fDev) IOFree(fDev, sizeof(*fDev));
    if (fPsp) IOFree(fPsp, sizeof(*fPsp));
    if (fPlan) IOFree(fPlan, sizeof(*fPlan));
    fVmReady = false;
    if (fPtPool) IOFree(fPtPool, sizeof(*fPtPool));
    fPtPool = nullptr;
    if (fKcq && fKcq->rb.ready && fDev)
        gfx11_set_eop_interrupt(fDev, &fKcq->rb, 0);
    if (fKiq && fDev)
        gfx11_mec_stop(fDev, fKiq, fKcq);
    if (fKcq) IOFree(fKcq, sizeof(*fKcq));
    if (fKiq) IOFree(fKiq, sizeof(*fKiq));
    fKcq = nullptr; fKiq = nullptr;
    if (fGfx) {
        if (fGfx->ready && fDev) gfx11_set_eop_interrupt(fDev, fGfx, 0);
        if (fGfx->ready && fDev) gfx11_cp_gfx_enable(fDev, 0);
        IOFree(fGfx, sizeof(*fGfx));
    }
    if (fSdma) {
        if (fSdma->ready && fDev) sdma60_set_trap_interrupt(fDev, fSdma, 0);
        if (fSdma->ready && fDev) sdma60_stop(fDev, fSdma);
        IOFree(fSdma, sizeof(*fSdma));
    }
    if (fIh) {
        if (fIh->enabled && fDev) ih60_disable(fDev, fIh);
        IOFree(fIh, sizeof(*fIh));
    }
    if (fGmc) IOFree(fGmc, sizeof(*fGmc));
    if (fLock) IOLockFree(fLock);
    fDisc = nullptr; fDev = nullptr; fPsp = nullptr; fPlan = nullptr; fGmc = nullptr; fIh = nullptr; fSdma = nullptr; fGfx = nullptr; fLock = nullptr;
}

void RDNA3Bringup::publish(const char *key, uint64_t v)
{
    OSNumber *n = OSNumber::withNumber(v, 64);
    if (n) {
        fOwner->setProperty(key, n);
        n->release();
    }
}

void RDNA3Bringup::publishString(const char *key, const char *s)
{
    OSString *o = OSString::withCString(s);
    if (o) {
        fOwner->setProperty(key, o);
        o->release();
    }
}

/* --------------------------- firmware --------------------------- */

void RDNA3Bringup::fwCallback(unsigned int, int result, const void *data, uint32_t len,
                              void *ctx)
{
    RDNA3Bringup *b = (RDNA3Bringup *)ctx;
    RDNA3Firmware fw = {};
    if (result == kOSReturnSuccess && data && len) {
        fw.data = (uint8_t *)IOMalloc(len);
        if (fw.data) {
            memcpy(fw.data, data, len);
            fw.len = len;
        }
    }
    IOLockLock(b->fLock);
    b->fFwResult = fw;
    b->fFwDone = true;
    IOLockWakeup(b->fLock, &b->fFwDone, false);
    IOLockUnlock(b->fLock);
}

bool RDNA3Bringup::loadFirmware(const char *name, RDNA3Firmware *out)
{
    /* kernelmanagerd may not be up yet early in boot: retry for ~2 min */
    for (int attempt = 0; attempt < 120 && !fAbort; attempt++) {
        OSKextRequestTag tag;
        fFwDone = false;
        fFwResult = {};
        OSReturn r = OSKextRequestResource(kBundleID, name,
                                           (OSKextRequestResourceCallback)fwCallback,
                                           this, &tag);
        if (r == kOSReturnSuccess) {
            IOLockLock(fLock);
            while (!fFwDone)
                IOLockSleep(fLock, &fFwDone, THREAD_UNINT);
            IOLockUnlock(fLock);
            if (fFwResult.data) {
                *out = fFwResult;
                BLOG("firmware %s: %u bytes", name, out->len);
                return true;
            }
        }
        IOSleep(1000);
    }
    BLOG("firmware %s: not available (copy it into the kext's Resources)", name);
    return false;
}

/* ----------------------------- VRAM ----------------------------- */

bool RDNA3Bringup::mapVram()
{
    fPci->setMemoryEnable(true);
    fVram = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapWriteCombineCache);
    if (!fVram) {
        BLOG("cannot map BAR0 (VRAM aperture)");
        return false;
    }
    fDev->vram_cpu = (volatile uint8_t *)fVram->getVirtualAddress();
    fDev->vram_cpu_len = fVram->getLength();
    BLOG("BAR0 VRAM aperture %llu MiB at phys 0x%llx",
         (unsigned long long)(fDev->vram_cpu_len >> 20),
         (unsigned long long)fVram->getPhysicalAddress());
    publish("RDNA3,VRAMApertureMB", fDev->vram_cpu_len >> 20);
    return true;
}

/* BAR2: the doorbell aperture (amdgpu_doorbell_init) */
bool RDNA3Bringup::mapDoorbell()
{
    fDoorbell = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2, kIOMapInhibitCache);
    if (!fDoorbell || fDoorbell->getLength() < 4096) {
        BLOG("no doorbell BAR (BAR2): rings keep their wptr registers, no compute queues");
        OSSafeReleaseNULL(fDoorbell);
        return false;
    }
    fDev->doorbell_cpu = (volatile uint8_t *)fDoorbell->getVirtualAddress();
    fDev->doorbell_len = fDoorbell->getLength();
    fDev->doorbell_bus = fDoorbell->getPhysicalAddress();
    BLOG("BAR2 doorbells %llu KiB at phys 0x%llx", (unsigned long long)(fDev->doorbell_len >> 10),
         (unsigned long long)fDev->doorbell_bus);
    publish("RDNA3,DoorbellKB", fDev->doorbell_len >> 10);
    return true;
}

/* ------------------------------ run ------------------------------ */

void RDNA3Bringup::run()
{
    rdna3_os os = {};
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT] = {};
    amd_fw_status fst;
    int ret;

    os.ctx = this;
    os.udelay = osUdelay;
    os.msleep = osMsleep;
    os.vlog = osVlog;
    os.wmb = osWmb;
    os.mmio_rd = osMmioRd;
    os.mmio_wr = osMmioWr;

    publishString("RDNA3,Stage2", "starting");
    rdna3_dev_init(fDev, &os, fMmio->getLength());
    fDev->vram_size = fVramSize;
    if (rdna3_dev_set_discovery(fDev, fDisc) != RDNA3_OK || !mapVram() ||
        rdna3_read_fb_location(fDev) != RDNA3_OK) {
        publishString("RDNA3,Stage2", "failed: device setup");
        return;
    }
    publish("RDNA3,FBStartMC", fDev->fb_start);

    /* Keep away from the first 16 MiB (pre-OS framebuffer / VGA stolen
     * memory) and from the top of VRAM (discovery, IFWI reservations):
     * stay inside the CPU-visible aperture, at most 512 MiB. */
    uint64_t hi = fDev->vram_cpu_len < (512ull << 20) ? fDev->vram_cpu_len : (512ull << 20);
    /* above the boot scanout (a 4K GOP framebuffer is 32 MiB) */
    uint64_t lo = fOwner->displayReserveEnd() > (16ull << 20) ? fOwner->displayReserveEnd() : (16ull << 20);
    if (lo + (64ull << 20) > hi) {
        publishString("RDNA3,Stage2", "failed: boot scanout leaves no room below 512 MiB");
        return;
    }
    rdna3_vram_alloc_init(fDev, lo, hi);

    rdna3_nbio_remap_hdp(fDev);

    /* firmware */
    if (!loadFirmware(amd_navi32_sos_name, &fSos)) {
        publishString("RDNA3,Stage2", "failed: no PSP firmware");
        return;
    }
    for (unsigned i = 0; i < AMD_BLOB_COUNT && !fAbort; i++) {
        if (loadFirmware(amd_navi32_fw_names[i], &fBlobs[i])) {
            blobs[i].data = fBlobs[i].data;
            blobs[i].len = fBlobs[i].len;
        }
    }
    fst = amd_ucode_plan_build(blobs, fPlan);
    if (fst != AMD_FW_OK) {
        BLOG("firmware plan: %s", amd_fw_status_str(fst));
        publishString("RDNA3,Stage2", "failed: firmware set incomplete");
        return;
    }
    BLOG("firmware plan: %u ucodes, %u KiB", fPlan->count, fPlan->total_aligned / 1024);

    /* PSP */
    ret = psp13_init(fPsp, fDev, fSos.data, fSos.len);
    if (!ret)
        ret = psp13_hw_init(fPsp, fPlan, blobs);
    publish("RDNA3,PSPResult", (uint64_t)(int64_t)ret);
    publish("RDNA3,SOSVersion", fPsp->sos_fw_version);
    if (ret) {
        publishString("RDNA3,Stage2", "failed: PSP");
        return;
    }
    publishString("RDNA3,Stage2", "PSP up, firmware loaded");

    if (fLevel >= 5) {
        uint32_t smuv = 0, ifv = 0;
        ret = smu13_hello(fDev, &smuv, &ifv);
        publish("RDNA3,SMUVersion", smuv);
        publish("RDNA3,SMUDriverIF", ifv);
        publishString("RDNA3,Stage2", ret ? "failed: SMU" : "PSP + SMU up");
        if (ret)
            return;
    }

    /* level 6: GMC (gmc_v11_0_hw_init -> gart_enable): GART table in
     * VRAM, MMHUB 3.0.2 VMID0 + contexts, VMID0 TLB flush. GFXHUB comes
     * with the GFX block (gfx_v11_0_hw_init), not here. */
    if (fLevel >= 6) {
        fGmc = (rdna3_gmc *)IOMalloc(sizeof(*fGmc));
        if (!fGmc) {
            publishString("RDNA3,Stage2", "failed: GMC (no memory)");
            return;
        }
        bzero(fGmc, sizeof(*fGmc));
        ret = gmc11_init(fDev, fGmc, 0);
        if (!ret)
            ret = gmc11_gart_enable(fDev, fGmc);
        publish("RDNA3,GMCResult", (uint64_t)(int64_t)ret);
        publish("RDNA3,GARTStartMC", fGmc->gart_start);
        publish("RDNA3,GARTSize", fGmc->gart_size);
        publishString("RDNA3,Stage2", ret ? "failed: GMC" : "PSP + SMU + MMHUB GART up");
        if (ret)
            return;
        /* GPUVM can now translate: let the GPU reach system memory (GTT BOs
         * through the page tables). amdgpu: pci_set_master() at probe. */
        fPci->setBusMasterEnable(true);
        publish("RDNA3,BusMaster", 1);
        /* soc21_common_hw_init: doorbell aperture (gfx ring, SDMA and the
         * compute queues ring their doorbells from level 7/8 on) */
        if (fDoorbells && mapDoorbell())
            nbio43_doorbell_init(fDev);
        publish("RDNA3,Doorbells", fDev->doorbell_ok ? 1 : 0);

        /* IH 6.0 ring 0, polled (no MSI registration yet) */
        fIh = (rdna3_ih *)IOMalloc(sizeof(*fIh));
        if (!fIh) {
            publishString("RDNA3,Stage2", "failed: IH (no memory)");
            return;
        }
        bzero(fIh, sizeof(*fIh));
        ret = ih60_init(fDev, fIh, 0, fGmc->dummy_page_pa, fMsi ? 1 : 0);
        publish("RDNA3,IHResult", (uint64_t)(int64_t)ret);
        if (!ret) {
            /* anything already queued (e.g. from firmware) is drained and counted */
            IOSleep(10);
            publish("RDNA3,IHEarlyIVs", ih60_process(fDev, fIh, nullptr, nullptr));
            if (fMsi && !setupInterrupt())
                BLOG("MSI: not available, the IH ring stays polled");
        }
        publishString("RDNA3,Stage2", ret ? "failed: IH" : "PSP + SMU + MMHUB GART + IH up");
        if (ret)
            return;

        /* page tables for user VMs: 16 MiB of 4 KiB tables */
        fPtPool = (rdna3_pt_pool *)IOMalloc(sizeof(*fPtPool));
        if (fPtPool && gpuvm_pool_init(fDev, fPtPool, 4096) == RDNA3_OK) {
            __sync_synchronize();   /* publish the pool before fVmReady */
            fVmReady = true;
            publish("RDNA3,VMTablePages", fPtPool->npages);
        } else {
            BLOG("no page-table pool: GEM_VA stays record-only");
        }
    }

    /* level 7: SDMA0 ring + ring test + a 64 KiB VRAM->VRAM copy check */
    if (fLevel >= 7) {
        fSdma = (rdna3_sdma_ring *)IOMalloc(sizeof(*fSdma));
        if (!fSdma) {
            publishString("RDNA3,Stage2", "failed: SDMA (no memory)");
            return;
        }
        bzero(fSdma, sizeof(*fSdma));
        ret = sdma60_start(fDev, fSdma, 0, 0);
        publish("RDNA3,SDMAResult", (uint64_t)(int64_t)ret);
        if (!ret) {
            const uint32_t len = 64 * 1024;
            uint64_t src = rdna3_vram_alloc(fDev, len, 4096, "sdma-selftest-src");
            uint64_t dst = rdna3_vram_alloc(fDev, len, 4096, "sdma-selftest-dst");
            uint32_t mism = 0xFFFFFFFFu;
            if (src != UINT64_MAX && dst != UINT64_MAX) {
                uint32_t i, a, b;
                for (i = 0; i < len / 4; i++) {
                    a = 0x0A3D0000u ^ i;
                    rdna3_vram_write(fDev, src + 4ull * i, &a, 4);
                }
                rdna3_vram_fill(fDev, dst, 0, len);
                ret = sdma60_copy_sync(fDev, fSdma, rdna3_vram_mc(fDev, src), rdna3_vram_mc(fDev, dst), len);
                if (!ret) {
                    mism = 0;
                    for (i = 0; i < len / 4; i++) {
                        rdna3_vram_read(fDev, dst + 4ull * i, &b, 4);
                        if (b != (0x0A3D0000u ^ i))
                            mism++;
                    }
                }
            }
            publish("RDNA3,SDMACopyMismatches", mism);
            if (!ret && mism)
                ret = RDNA3_EIO;
            if (!ret && fIrqLive) {
                /* the first real interrupt: a fence with a TRAP behind it must
                 * reach the handler; otherwise MSI is not working on this
                 * machine and polling stays (the sources stay off) */
                uint64_t before = fIrqFenceSdma;
                uint32_t i;
                sdma60_set_trap_interrupt(fDev, fSdma, 1);
                uint32_t seq = sdma60_emit_fence(fDev, fSdma);
                sdma60_commit(fDev, fSdma);
                sdma60_wait_fence(fDev, fSdma, seq);
                for (i = 0; i < 200 && fIrqFenceSdma == before; i++)
                    IOSleep(1);
                bool ok = fIrqFenceSdma != before;
                publish("RDNA3,MSIVerified", ok ? 1 : 0);
                if (!ok) {
                    BLOG("MSI: no SDMA trap interrupt within 200 ms (irqs %llu), back to polling",
                         (unsigned long long)fIrqCount);
                    sdma60_set_trap_interrupt(fDev, fSdma, 0);
                    fIrqLive = false;
                } else {
                    BLOG("MSI: verified, fence waits now sleep on the interrupt");
                }
            }
        }
        publishString("RDNA3,Stage2", ret ? "failed: SDMA" : "PSP + SMU + GART + IH + SDMA up, copy verified");
        if (!ret && fVmReady) {
            /* userspace may now submit to the DMA ring */
            fOwner->drmRingReady(AMDGPU_HW_IP_DMA, 1);
            publishString("RDNA3,Rings", "DMA0");
        }
        if (ret)
            return;
    }

    /* level 8: GFX 11 (RLC autoload check, RS64, GFXHUB, clear state) and
     * the kernel gfx ring 0 with its ring test */
    if (fLevel >= 8) {
        rdna3_rs64_start rs64;
        fGfx = (rdna3_gfx_ring *)IOMalloc(sizeof(*fGfx));
        if (!fGfx) {
            publishString("RDNA3,Stage2", "failed: GFX (no memory)");
            return;
        }
        bzero(fGfx, sizeof(*fGfx));
        ret = gfx11_rs64_from_fw(fBlobs[AMD_BLOB_PFP].data, fBlobs[AMD_BLOB_PFP].len,
                                 fBlobs[AMD_BLOB_ME].data, fBlobs[AMD_BLOB_ME].len,
                                 fBlobs[AMD_BLOB_MEC].data, fBlobs[AMD_BLOB_MEC].len, &rs64);
        if (!ret)
            ret = gfx11_hw_init(fDev, fGmc, fGfx, &rs64, 0);
        publish("RDNA3,GFXResult", (uint64_t)(int64_t)ret);
        if (!ret && fIrqLive)
            gfx11_set_eop_interrupt(fDev, fGfx, 1);
        /* MEC: the KIQ and the first kernel compute queue (needs doorbells) */
        if (!ret && fDev->doorbell_ok) {
            int kr;
            fKiq = (rdna3_mec_ring *)IOMalloc(sizeof(*fKiq));
            fKcq = (rdna3_mec_ring *)IOMalloc(sizeof(*fKcq));
            if (fKiq && fKcq) {
                bzero(fKiq, sizeof(*fKiq));
                bzero(fKcq, sizeof(*fKcq));
                kr = gfx11_mec_ring_init(fDev, fKiq, 1, RD_KIQ_PIPE, 0, RD_DB_KIQ, 0);
                if (!kr)
                    kr = gfx11_mec_ring_init(fDev, fKcq, 0, RD_KCQ0_PIPE, 0, RD_DB_MEC_RING0, 0);
                if (!kr)
                    kr = gfx11_kiq_start(fDev, fKiq);
                publish("RDNA3,KIQResult", (uint64_t)(int64_t)kr);
                if (!kr)
                    kr = gfx11_kcq_start(fDev, fKiq, fKcq);
                publish("RDNA3,KCQResult", (uint64_t)(int64_t)kr);
                if (!kr && fIrqLive)
                    gfx11_set_eop_interrupt(fDev, &fKcq->rb, 1);
                if (kr)
                    BLOG("compute queues unavailable (%d): GFX and DMA rings only", kr);
            }
        }
        if (!ret && fVmReady) {
            /* VMs opened before the GFXHUB came up get their context now
             * (gfxhub303_gart_enable reset VMID 1-15) */
            for (uint32_t id = 1; id < 16; id++)
                if (fVmidUsed & (1u << id)) {
                    gfxhub303_setup_vm_pt_regs(fDev, id, fVmPd[id]);
                    gfxhub303_flush_tlb(fDev, id, 0);
                }
            fOwner->drmRingReady(AMDGPU_HW_IP_GFX, 1);
            if (fKcq && fKcq->rb.ready) {
                fOwner->drmRingReady(AMDGPU_HW_IP_COMPUTE, 1);
                publishString("RDNA3,Rings", "DMA0 GFX0 COMP0");
            } else {
                publishString("RDNA3,Rings", "DMA0 GFX0");
            }
        }
        publishString("RDNA3,Stage2", ret ? "failed: GFX" : "GFX ring 0 up, ring test passed");
    }

    if (fMsi) {
        /* snapshot after bring-up (the handler keeps counting afterwards) */
        publish("RDNA3,MSILive", fIrqLive ? 1 : 0);
        publish("RDNA3,IRQs", fIrqCount);
        publish("RDNA3,IRQIVs", fIvCount);
        publish("RDNA3,IRQFenceGFX", fIrqFenceGfx);
        publish("RDNA3,IRQFenceSDMA", fIrqFenceSdma);
        publish("RDNA3,IRQFenceCompute", fIrqFenceComp);
        publish("RDNA3,IRQOther", fIrqOther);
    }
}

/* ------------------------------ GPUVM ------------------------------ */

struct RDNA3Vm {
    rdna3_vm vm;
    uint32_t vmid;
    bool     live;       /* page tables exist */
    bool     gfxFlush;   /* next GFX job re-points the VMID and flushes from the ring */
    bool     compFlush;  /* same for the next compute job (the hub is shared, the rings are not) */
};

void *RDNA3Bringup::vmCreate()
{
    RDNA3Vm *v = (RDNA3Vm *)IOMalloc(sizeof(*v));
    if (!v)
        return nullptr;
    bzero(v, sizeof(*v));
    v->vm.root = UINT64_MAX;
    if (!fVmReady)
        return v;
    /* one VMID per open file for now (amdgpu hands VMIDs out per job) */
    for (uint32_t id = 1; id < 16; id++) {
        if (!(fVmidUsed & (1u << id))) {
            if (gpuvm_vm_init(fDev, &v->vm, fPtPool) != RDNA3_OK)
                break;
            fVmidUsed |= 1u << id;
            v->vmid = id;
            v->vm.vmid = id;
            v->live = true;
            fVmPd[id] = gpuvm_pd_addr(fDev, &v->vm);
            /* both hubs: SDMA/VCN walk MMHUB, shaders GFXHUB */
            mmhub302_setup_vm_pt_regs(fDev, id, gpuvm_pd_addr(fDev, &v->vm));
            if (fGmc && fGmc->gfxhub_enabled)
                gfxhub303_setup_vm_pt_regs(fDev, id, gpuvm_pd_addr(fDev, &v->vm));
            vmFlush(id);
            v->gfxFlush = v->compFlush = true;
            break;
        }
    }
    if (!v->live)
        BLOG("VM: no VMID or page tables left, this client's VM is record-only");
    return v;
}

void RDNA3Bringup::vmFlush(uint32_t vmid)
{
    mmhub302_flush_tlb(fDev, vmid, 0);
    if (fGmc && fGmc->gfxhub_enabled)
        gfxhub303_flush_tlb(fDev, vmid, 0);
}

void RDNA3Bringup::vmDestroy(void *p)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    if (!v)
        return;
    if (v->live && fVmReady) {
        mmhub302_setup_vm_pt_regs(fDev, v->vmid, 0);
        if (fGmc && fGmc->gfxhub_enabled)
            gfxhub303_setup_vm_pt_regs(fDev, v->vmid, 0);
        vmFlush(v->vmid);
        gpuvm_vm_fini(fDev, &v->vm);
        fVmidUsed &= ~(1u << v->vmid);
        fVmPd[v->vmid] = 0;
    }
    IOFree(v, sizeof(*v));
}

struct GttPages {
    const RDNA3Gtt *g;
    uint64_t first;   /* page index of the mapping's start */
};

static uint64_t gttPagePa(void *ctx, uint64_t i)
{
    /* bus addresses from the BO's IODMACommand (mapper-translated when an
     * IOMMU is active); bus mastering is on since level 6 */
    GttPages *p = (GttPages *)ctx;
    return p->g->pages[p->first + i];
}

int RDNA3Bringup::vmMap(void *p, rdna3_bo *bo, uint64_t va, uint64_t offset, uint64_t size,
                        uint32_t flags)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    int r;
    if (!v || !v->live || !fVmReady)
        return 0;   /* record-only VM */
    uint64_t pages = size / 4096;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        r = gpuvm_map_linear(fDev, &v->vm, va, rdna3_vram_pa(fDev, bo->vram_offset + offset), pages,
                             gpuvm_pte_flags(flags, 0));
    } else {
        const RDNA3Gtt *gt = (const RDNA3Gtt *)bo->backing;
        if (!gt || (offset & 4095) || offset / 4096 + pages > gt->npages)
            return -RD_EINVAL;
        GttPages g = { gt, offset / 4096 };
        r = gpuvm_map(fDev, &v->vm, va, pages, gttPagePa, &g, gpuvm_pte_flags(flags, 1));
    }
    if (r)
        return -RD_ENOMEM;
    vmFlush(v->vmid);
    v->gfxFlush = v->compFlush = true;
    return 0;
}

void RDNA3Bringup::vmUnmap(void *p, uint64_t va, uint64_t size)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    if (!v || !v->live || !fVmReady)
        return;
    gpuvm_unmap(fDev, &v->vm, va, size / 4096);
    vmFlush(v->vmid);
    v->gfxFlush = v->compFlush = true;
}

/* --------------------------- submission --------------------------- */

int RDNA3Bringup::submit(void *p, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                         uint64_t *seq)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    if (!v || !v->live || ring != 0)
        return -RD_EINVAL;   /* record-only VM: the IB VA means nothing to the GPU */
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ready) {
        for (uint32_t i = 0; i < n; i++)
            sdma60_emit_ib(fDev, fSdma, v->vmid, ibs[i].va, ibs[i].length_dw);
        *seq = sdma60_emit_fence(fDev, fSdma);
        sdma60_commit(fDev, fSdma);
        return 0;
    }
    if (ip == AMDGPU_HW_IP_GFX && fGfx && fGfx->ready) {
        /* amdgpu_ib_schedule order: VM flush, ACQUIRE_MEM, HDP flush,
         * CONTEXT_CONTROL, framed IBs, fence (gfx11_submit_job) */
        rdna3_gfx_ib g[RD_GFX_MAX_IBS];
        if (n > RD_GFX_MAX_IBS)
            return -RD_EINVAL;
        for (uint32_t i = 0; i < n; i++) {
            g[i].va = ibs[i].va;
            g[i].ndw = ibs[i].length_dw;
            g[i].flags = ibs[i].flags;
        }
        uint32_t s = gfx11_submit_job(fDev, fGfx, (uint64_t)(uintptr_t)v, v->vmid,
                                      v->gfxFlush ? gpuvm_pd_addr(fDev, &v->vm) : 0, g, n);
        if (!s)
            return -RD_EBUSY;
        v->gfxFlush = false;
        *seq = s;
        return 0;
    }
    if (ip == AMDGPU_HW_IP_COMPUTE && fKcq && fKcq->rb.ready) {
        rdna3_gfx_ib g[RD_GFX_MAX_IBS];
        if (n > RD_GFX_MAX_IBS)
            return -RD_EINVAL;
        for (uint32_t i = 0; i < n; i++) {
            g[i].va = ibs[i].va;
            g[i].ndw = ibs[i].length_dw;
            g[i].flags = ibs[i].flags;
        }
        uint32_t s = gfx11_submit_compute_job(fDev, fKcq, (uint64_t)(uintptr_t)v, v->vmid,
                                              v->compFlush ? gpuvm_pd_addr(fDev, &v->vm) : 0, g, n);
        if (!s)
            return -RD_EBUSY;
        v->compFlush = false;
        *seq = s;
        return 0;
    }
    return -RD_EINVAL;
}

int RDNA3Bringup::fenceSignaled(uint32_t ip, uint32_t ring, uint64_t seq)
{
    if (ring != 0)
        return 1;
    if (ip == AMDGPU_HW_IP_DMA && fSdma)
        return sdma60_fence_passed(fDev, fSdma, (uint32_t)seq);
    if (ip == AMDGPU_HW_IP_GFX && fGfx)
        return gfx11_fence_passed(fDev, fGfx, (uint32_t)seq);
    if (ip == AMDGPU_HW_IP_COMPUTE && fKcq)
        return gfx11_fence_passed(fDev, &fKcq->rb, (uint32_t)seq);
    return 1;
}

extern "C" uint64_t mach_continuous_time(void);

int RDNA3Bringup::fenceWait(uint32_t ip, uint32_t ring, uint64_t seq, uint64_t absNs)
{
    /* userspace deadlines are CLOCK_MONOTONIC, which on macOS counts
     * mach_continuous_time. Cap every wait at 10 s so a hung engine cannot
     * hold the DRM lock forever. With MSI the wait sleeps on fLock until the
     * IH handler wakes it (5 ms guard against a lost interrupt); without it
     * the ring is polled. TODO(uapi): drop fDrmLock while sleeping. */
    uint64_t start, now, capNs = 10ull * 1000 * 1000 * 1000;
    absolutetime_to_nanoseconds(mach_continuous_time(), &start);
    for (uint32_t spins = 0;; spins++) {
        if (fenceSignaled(ip, ring, seq))
            return 0;
        absolutetime_to_nanoseconds(mach_continuous_time(), &now);
        if ((absNs != UINT64_MAX && now >= absNs) || now - start >= capNs)
            return -RD_ETIME;
        if (fIrqLive) {
            uint64_t ns = 5ull * 1000 * 1000, left = capNs - (now - start), deadline;
            if (absNs != UINT64_MAX && absNs - now < left)
                left = absNs - now;
            if (left < ns)
                ns = left;
            clock_interval_to_deadline((uint32_t)(ns / 1000), kMicrosecondScale, &deadline);
            /* the handler wakes under fLock after the fence landed: re-check
             * under the lock so no wakeup between the check and the sleep
             * is lost */
            IOLockLock(fLock);
            if (!fenceSignaled(ip, ring, seq))
                IOLockSleepDeadline(fLock, &fFenceEvent, deadline, THREAD_UNINT);
            IOLockUnlock(fLock);
        } else if (spins < 1000) {
            IODelay(1);
        } else {
            IOSleep(1);
        }
    }
}

/* ---------------------------- interrupts ---------------------------- */

bool RDNA3Bringup::irqFilter(OSObject *, IOFilterInterruptEventSource *)
{
    return true;   /* MSI: the vector is ours alone, no register read at primary level */
}

void RDNA3Bringup::irqAction(OSObject *owner, IOInterruptEventSource *, int)
{
    RDNA3Accel *a = OSDynamicCast(RDNA3Accel, owner);
    RDNA3Bringup *b = a ? a->bringup() : nullptr;
    if (b)
        b->handleInterrupt();
}

void RDNA3Bringup::ivCallback(void *ctx, const rdna3_iv *iv)
{
    RDNA3Bringup *b = (RDNA3Bringup *)ctx;
    switch (ih60_iv_fence(iv)) {
    case RD_IH_FENCE_GFX:   b->fIrqFenceGfx++; break;
    case RD_IH_FENCE_SDMA0: b->fIrqFenceSdma++; break;
    case RD_IH_FENCE_COMPUTE0: b->fIrqFenceComp++; break;
    default:                b->fIrqOther++; break;
    }
}

void RDNA3Bringup::handleInterrupt()
{
    if (!fIh || !fDev)
        return;
    uint32_t n = ih60_process(fDev, fIh, ivCallback, this);
    IOLockLock(fLock);
    fIrqCount++;
    fIvCount += n;
    IOLockWakeup(fLock, &fFenceEvent, false);
    IOLockUnlock(fLock);
}

bool RDNA3Bringup::setupInterrupt()
{
    int index = -1, type = 0;
    for (int i = 0; fPci->getInterruptType(i, &type) == kIOReturnSuccess; i++)
        if (type & kIOInterruptTypePCIMessaged) {
            index = i;
            break;
        }
    if (index < 0) {
        BLOG("MSI: the device offers no message-signalled interrupt");
        return false;
    }
    fWorkLoop = IOWorkLoop::workLoop();
    fIntr = IOFilterInterruptEventSource::filterInterruptEventSource(fOwner, irqAction, irqFilter,
                                                                     fPci, index);
    if (!fWorkLoop || !fIntr || fWorkLoop->addEventSource(fIntr) != kIOReturnSuccess) {
        BLOG("MSI: cannot register interrupt %d", index);
        teardownInterrupt();
        return false;
    }
    fIntr->enable();
    fIrqLive = true;
    publish("RDNA3,MSIIndex", (uint64_t)index);
    BLOG("MSI: interrupt index %d registered on the bring-up work loop", index);
    return true;
}

void RDNA3Bringup::teardownInterrupt()
{
    fIrqLive = false;
    if (fIntr) {
        fIntr->disable();
        if (fWorkLoop)
            fWorkLoop->removeEventSource(fIntr);
        fIntr->release();
        fIntr = nullptr;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }
    if (fLock) {
        IOLockLock(fLock);
        IOLockWakeup(fLock, &fFenceEvent, false);   /* sleepers fall back to polling */
        IOLockUnlock(fLock);
    }
}
