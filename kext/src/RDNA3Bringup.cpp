/*
 * RDNA3Bringup.cpp - see RDNA3Bringup.hpp.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Bringup.hpp"
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "smu13.h"
#include "navi3x_regs.h"
}
#include "dcaux.h"
#include "dcn32_clk.h"
#include "dcn32_clone.h"

/* kpi.mach; declared here to avoid depending on private headers */
extern "C" kern_return_t kernel_thread_start(thread_continue_t continuation,
                                             void *parameter, thread_t *new_thread);
extern "C" void thread_deallocate(thread_t thread);

#define BLOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

/* amdgpu_device_check_vram_lost: a word written at bring-up and read back after a reset */
static const uint32_t kResetMagic = 0x52444E33u;   /* "RDN3" */


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
        uint32_t msi = 0, db = 1, pm = 1, dmub = 0, mes = 0;
        uint64_t pmmask = 0;
        PE_parse_boot_argn("rdna3dmub", &dmub, sizeof(dmub));
        fDmubMode = dmub;
        PE_parse_boot_argn("rdna3msi", &msi, sizeof(msi));
        PE_parse_boot_argn("rdna3doorbell", &db, sizeof(db));
        PE_parse_boot_argn("rdna3pm", &pm, sizeof(pm));
        PE_parse_boot_argn("rdna3pmmask", &pmmask, sizeof(pmmask));
        PE_parse_boot_argn("rdna3mes", &mes, sizeof(mes));
        fMesWanted = mes != 0;
        {
            /* on by default since card run 8: ~200 W -> ~75 W idle, hotspot
             * 78 -> 55 C, DMA/GFX tests still pass; rdna3cg=0 turns it off */
            uint32_t cg = 1, go = 0;
            PE_parse_boot_argn("rdna3cg", &cg, sizeof(cg));
            fCgWanted = cg != 0;
            PE_parse_boot_argn("rdna3gfxoff", &go, sizeof(go));
            fGfxOffWanted = go != 0;
            /* HDP + ATHUB gating (soc21_cg.c), with the GFX one; opt-in
             * until a card run shows the watts: rdna3socg=1 */
            uint32_t socg = 0;
            PE_parse_boot_argn("rdna3socg", &socg, sizeof(socg));
            fSocCgWanted = socg != 0;
            uint32_t pmexp = 0;
            PE_parse_boot_argn("rdna3pmexp", &pmexp, sizeof(pmexp));
            fPmExpWanted = pmexp != 0;
            uint32_t idlecap = 0;
            PE_parse_boot_argn("rdna3idlecap", &idlecap, sizeof(idlecap));
            fIdleCapWanted = idlecap != 0;
        }
        fMsi = msi != 0;
        fDoorbells = db != 0;
        fPmMode = pm;
        fPmMask = pmmask;
    }
    fLock = IOLockAlloc();
    fIhLock = IOLockAlloc();
    fDisc = (amd_discovery *)IOMalloc(sizeof(*fDisc));
    fDev = (rdna3_dev *)IOMalloc(sizeof(*fDev));
    fPsp = (rdna3_psp *)IOMalloc(sizeof(*fPsp));
    fPlan = (amd_ucode_plan *)IOMalloc(sizeof(*fPlan));
    if (!fLock || !fIhLock || !fDisc || !fDev || !fPsp || !fPlan)
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
    /* 1. the second monitor goes while its DMCUB and the BAR still work */
    if (fDmub)
        fOwner->displayMirrorOff(fDmub);
    teardownInterrupt();   /* no handler may touch fIh / the rings past here */

    /* 2. the engines, in gfx_v11_0_hw_fini order, while fDev, the BAR and
     *    the doorbells are still mapped */
    /* amdgpu_device_ip_fini_early: GFXOFF disallowed, clock gating off
     * (UNGATE) first */
    if (fGfxOffCall) {
        IOLock *l = fOwner->drmLock();
        fGfxOffStop = true;
        thread_call_cancel_wait(fGfxOffCall);
        if (l)
            IOLockLock(l);
        gfxOffHold();
        if (l)
            IOLockUnlock(l);
        thread_call_free(fGfxOffCall);
        fGfxOffCall = nullptr;
    }
    if (fCgWanted && fGfx && fGfx->ready && fDev)
        gfx11_set_clock_gating(fDev, 0, RD_GFX_CG_GC_11_0_3);
    if (fCgWanted && fSocCgWanted && fDev)
        soc21_set_clock_gating(fDev, 0);
    if (fKcq && fKcq->rb.ready && fDev)
        gfx11_set_eop_interrupt(fDev, &fKcq->rb, 0);
    if (fGfx && fGfx->async && fGfx->ready && fDev) {
        gfx11_set_eop_interrupt(fDev, fGfx, 0);
        gfx11_kgq_stop(fDev, fKiq, fGfx);   /* unmapped while the KIQ still runs */
    }
    if (fKiq && fDev)
        gfx11_mec_stop(fDev, fKiq, fKcq, fMes);
    if (fGfx && fGfx->ready && fDev) {
        gfx11_set_eop_interrupt(fDev, fGfx, 0);
        gfx11_cp_gfx_enable(fDev, 0);
    }
    if (fSdma && fSdma->ready && fDev) {
        sdma60_set_trap_interrupt(fDev, fSdma, 0);
        sdma60_stop(fDev, fSdma);
    }
    if (fIh && fIh->enabled && fDev)
        ih60_disable(fDev, fIh);
    if (fPsp && fPsp->ring_created)
        psp13_ring_stop(fPsp);

    /* 3. memory: mappings, firmware, state; fDev last */
    OSSafeReleaseNULL(fVram);
    OSSafeReleaseNULL(fDoorbell);
    for (unsigned i = 0; i < AMD_BLOB_COUNT; i++)
        if (fBlobs[i].data)
            IOFree(fBlobs[i].data, fBlobs[i].len);
    if (fSos.data)
        IOFree(fSos.data, fSos.len);
    if (fSmu) IOFree(fSmu, sizeof(*fSmu));
    if (fDmub) IOFree(fDmub, sizeof(*fDmub));
    fDmub = nullptr;
    if (fRom) IOFree(fRom, RD_VBIOS_MAX_LEN);
    fVmReady = false;
    if (fPtPool) IOFree(fPtPool, sizeof(*fPtPool));
    fPtPool = nullptr;
    if (fMes) IOFree(fMes, sizeof(*fMes));
    fMes = nullptr;
    if (fKcq) IOFree(fKcq, sizeof(*fKcq));
    if (fKiq) IOFree(fKiq, sizeof(*fKiq));
    fKcq = nullptr; fKiq = nullptr;
    if (fGfx) IOFree(fGfx, sizeof(*fGfx));
    if (fSdma) IOFree(fSdma, sizeof(*fSdma));
    if (fIh) IOFree(fIh, sizeof(*fIh));
    if (fGmc) IOFree(fGmc, sizeof(*fGmc));
    if (fDisc) IOFree(fDisc, sizeof(*fDisc));
    if (fPsp) IOFree(fPsp, sizeof(*fPsp));
    if (fPlan) IOFree(fPlan, sizeof(*fPlan));
    if (fDev) IOFree(fDev, sizeof(*fDev));
    if (fLock) IOLockFree(fLock);
    if (fIhLock) IOLockFree(fIhLock);
    fIhLock = nullptr;
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

/* Reads the installed bundle's copy directly. Not OSKextRequestResource:
 * kernelmanagerd never answers it for a kext that OpenCore injected (or one
 * it judged invalid for the Auxiliary KC), and its callback may never run. */
static const char *kFwDirs[] = {
    "/Library/Extensions/RDNA3Accel.kext/Contents/Resources/",
    "/Library/Application Support/RDNA3VM/kexts/RDNA3Accel.kext/Contents/Resources/",
    "/Library/Application Support/RDNA3/firmware/",     /* package/oc_install.sh */
};

static bool readFileFromDisk(const char *path, RDNA3Firmware *out)
{
    vfs_context_t ctx = vfs_context_create(NULL);
    vnode_t vp = NULLVP;
    bool ok = false;
    if (!ctx)
        return false;
    if (vnode_open(path, FREAD, 0, 0, &vp, ctx) == 0) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        if (vnode_getattr(vp, &va, ctx) == 0 && va.va_data_size > 0 &&
            va.va_data_size < (64ull << 20)) {
            uint32_t len = (uint32_t)va.va_data_size;
            uint8_t *buf = (uint8_t *)IOMalloc(len);
            uio_t uio = buf ? uio_create(1, 0, UIO_SYSSPACE, UIO_READ) : NULL;
            if (uio && uio_addiov(uio, CAST_USER_ADDR_T(buf), len) == 0 &&
                VNOP_READ(vp, uio, 0, ctx) == 0 && uio_resid(uio) == 0) {
                out->data = buf;
                out->len = len;
                ok = true;
            }
            if (uio)
                uio_free(uio);
            if (!ok && buf)
                IOFree(buf, len);
        }
        vnode_close(vp, FREAD, ctx);
    }
    vfs_context_rele(ctx);
    return ok;
}

/* Is there a root file system to open paths on? A vnode_open 0.7 s into
 * boot panicked in namei ("Taking non-sleepable RW lock with preemption
 * enabled", card run 6): wait for the BSD layer (the IOBSD resource) and
 * a root vnode first. */
static bool rootFsReady()
{
    static bool bsd = false;
    if (!bsd) {
        OSDictionary *m = IOService::resourceMatching("IOBSD");
        if (m) {
            IOService *r = IOService::waitForMatchingService(m, 1000ull * 1000 * 1000);
            m->release();
            if (r) {
                r->release();
                bsd = true;
            }
        }
        if (!bsd)
            return false;
    }
    vnode_t rv = vfs_rootvnode();
    if (!rv)
        return false;
    vnode_put(rv);
    return true;
}

bool RDNA3Bringup::loadFirmware(const char *name, RDNA3Firmware *out)
{
    /* the root volume may not be mounted yet early in boot: retry ~2 min */
    for (int attempt = 0; attempt < 120 && !fAbort; attempt++) {
        if (!rootFsReady()) {
            IOSleep(1000);
            continue;
        }
        char path[256];
        for (unsigned d = 0; d < sizeof(kFwDirs) / sizeof(kFwDirs[0]); d++) {
            snprintf(path, sizeof(path), "%s%s", kFwDirs[d], name);
            if (readFileFromDisk(path, out)) {
                BLOG("firmware %s: %u bytes (%s)", name, out->len, path);
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

/* Passthrough: a PCI bus reset after the host's amdgpu leaves the PSP with
 * neither sOS alive nor the bootloader ready (C2PMSG_35 reads 0), and every
 * bootloader command times out. The only way back is what amdgpu's GPU
 * recovery uses, a mode-1 reset through the PMFW (soc21_asic_mode1_reset:
 * bus mastering off, PCI config saved and restored around it).
 * rdna3reset=0: never, 1 (default): when stuck, 2: always. */
void RDNA3Bringup::resetIfPspStuck()
{
    static const uint8_t kSaved[] = { 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x3C };
    uint32_t mode = 1, cfg[sizeof(kSaved)], i;
    uint16_t cmd;
    rdna3_psp *bl;
    int stuck, ret;

    PE_parse_boot_argn("rdna3reset", &mode, sizeof(mode));
    bl = (rdna3_psp *)IOMalloc(sizeof(*bl));
    if (!bl)
        return;
    bzero(bl, sizeof(*bl));
    bl->d = fDev;
    psp13_log_state(bl, "init");
    stuck = psp13_bootloader_stuck(bl);
    publish("RDNA3,PSPStuckAtInit", (uint64_t)stuck);
    if (mode == 0 || (mode == 1 && !stuck)) {
        if (stuck)
            BLOG("PSP bootloader not ready, rdna3reset=0: no mode-1 reset");
        IOFree(bl, sizeof(*bl));
        return;
    }
    BLOG("PSP: %s: mode-1 reset before init (rdna3reset=0 skips it)",
         stuck ? "no sOS and the bootloader is not ready" : "rdna3reset=2");
    cmd = fPci->configRead16(kIOPCIConfigCommand);
    for (i = 0; i < sizeof(kSaved); i++)
        cfg[i] = fPci->configRead32(kSaved[i]);
    fPci->setBusMasterEnable(false);
    smu13_mode1_reset(fDev);
    for (i = 0; i < fDev->usec_timeout; i++) {
        if (rdna3_rreg(fDev, mmRCC_CONFIG_MEMSIZE) != 0xffffffffu)
            break;
        IODelay(1);
    }
    for (i = 0; i < sizeof(kSaved); i++)
        fPci->configWrite32(kSaved[i], cfg[i]);
    fPci->configWrite16(kIOPCIConfigCommand, cmd);
    ret = psp13_wait_for_bootloader(bl);
    psp13_log_state(bl, "after mode-1 reset");
    publish("RDNA3,InitResetResult", (uint64_t)(int64_t)ret);
    BLOG("PSP: mode-1 reset %s", ret == RDNA3_OK ? "done, bootloader ready" : "did not bring the bootloader back");
    IOFree(bl, sizeof(*bl));
}

/* amdgpu_device_init: "Post card if necessary". amdgpu_device_need_post
 * (BIOS scratch 7) -> amdgpu_device_asic_init for GC 11: wait for the PSP
 * bootloader, then amdgpu_atomfirmware_asic_init. In a passthrough VM whose
 * UEFI did not run the card's option ROM the GPU is not posted: the FB
 * location reads 0. boot-arg rdna3post=0 skips it. */
bool RDNA3Bringup::postIfNeeded()
{
    static const uint32_t kScratchBytes = 20u * 1024u;   /* amdgpu's minimum FB scratch */
    uint32_t want = 1, *scratch = nullptr;
    rdna3_atom *atom = nullptr;
    rdna3_psp *bl = nullptr;
    int ret, need;
    bool ok = true;

    PE_parse_boot_argn("rdna3post", &want, sizeof(want));
    if (!fRom)
        fRom = (uint8_t *)IOMalloc(RD_VBIOS_MAX_LEN);
    atom = (rdna3_atom *)IOMalloc(sizeof(*atom));
    scratch = (uint32_t *)IOMalloc(kScratchBytes);
    bl = (rdna3_psp *)IOMalloc(sizeof(*bl));
    if (!fRom || !atom || !scratch || !bl) {
        BLOG("post: no memory");
        ok = false;
        goto out;
    }
    bzero(scratch, kScratchBytes);
    bzero(bl, sizeof(*bl));
    bl->d = fDev;
    ret = vbios_read(fDev, &fVbios, fRom, RD_VBIOS_MAX_LEN);
    if (ret == RDNA3_OK)
        ret = atom_init(atom, fDev, &fVbios, scratch, kScratchBytes);
    if (ret != RDNA3_OK) {
        /* no VBIOS: amdgpu fails here only when the GPU needs posting */
        BLOG("post: no VBIOS/ATOM context (%d), assuming posted", ret);
        publish("RDNA3,PostResult", (uint64_t)(int64_t)ret);
        goto out;
    }
    need = atom_need_post(atom);
    publish("RDNA3,NeedPost", (uint64_t)(int64_t)need);
    if (need != 1)
        goto out;
    if (!want) {
        BLOG("post: GPU not posted, rdna3post=0: skipping ASIC_Init");
        goto out;
    }
    BLOG("GPU posting now...");
    if (psp13_wait_for_bootloader(bl) != RDNA3_OK)
        BLOG("post: PSP bootloader not ready (C2PMSG_35 bit 31), running ASIC_Init anyway");
    ret = atom_asic_init(atom);
    publish("RDNA3,PostResult", (uint64_t)(int64_t)ret);
    if (ret != RDNA3_OK) {
        BLOG("gpu post error! (%d)", ret);
        ok = false;
    } else {
        BLOG("post: done, ASIC_INIT_COMPLETE %s", atom_need_post(atom) == 0 ? "set" : "still clear");
    }
out:
    if (bl) IOFree(bl, sizeof(*bl));
    if (scratch) IOFree(scratch, kScratchBytes);
    if (atom) IOFree(atom, sizeof(*atom));
    return ok;
}

/* ------------------------------ run ------------------------------ */

void RDNA3Bringup::run()
{
    rdna3_os os = {};
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT] = {};
    amd_fw_status fst;

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
    if (rdna3_dev_set_discovery(fDev, fDisc) != RDNA3_OK || !mapVram()) {
        publishString("RDNA3,Stage2", "failed: device setup");
        return;
    }
    resetIfPspStuck();
    if (!postIfNeeded() || rdna3_read_fb_location(fDev) != RDNA3_OK) {
        publishString("RDNA3,Stage2", "failed: device setup");
        return;
    }
    publish("RDNA3,FBStartMC", fDev->fb_start);

    /* Kernel VRAM (see the layout in RDNA3Accel.hpp): CPU-visible window
     * above the first 16 MiB (pre-OS framebuffer / VGA stolen memory) and
     * the boot scanout (a 4K GOP framebuffer is 32 MiB), the rest of the BAR
     * for user BOs; GPU-only buffers top-down below the firmware's block. */
    uint64_t lo = fOwner->displayReserveEnd() > kVramKernelLowStart ? fOwner->displayReserveEnd()
                                                                     : kVramKernelLowStart;
    uint64_t hi = lo + kVramKernelLowSize;
    if (hi > fDev->vram_cpu_len) {
        publishString("RDNA3,Stage2", "failed: boot scanout leaves no room in the BAR window");
        return;
    }
    {
        /* amdgpu_ttm_reserve_tmr: the VBIOS's fw_reserved_size at the top */
        uint64_t reserve = fVbios.len ? vbios_fw_reserved_fb_size(&fVbios) : 0;
        if (reserve < kVramMinTopReserve)
            reserve = kVramMinTopReserve;
        publish("RDNA3,VRAMFwReservedKB", reserve >> 10);
        if (fVramSize > kVramKernelTopSize + reserve)
            rdna3_vram_alloc_top_init(fDev, fVramSize - kVramKernelTopSize, fVramSize - reserve);
    }
    fAllocLo = lo;
    fAllocHi = hi;
    rdna3_vram_alloc_init(fDev, lo, hi);

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
    /* rdna3dmub=3 attaches to the DMCUB the GOP left running: the PSP
     * must not load (and restart) it underneath */
    if (fDmubMode == 3) {
        blobs[AMD_BLOB_DMCUB].data = nullptr;
        blobs[AMD_BLOB_DMCUB].len = 0;
    }
    fst = amd_ucode_plan_build(blobs, fPlan);
    if (fst != AMD_FW_OK) {
        BLOG("firmware plan: %s", amd_fw_status_str(fst));
        publishString("RDNA3,Stage2", "failed: firmware set incomplete");
        return;
    }
    BLOG("firmware plan: %u ucodes, %u KiB", fPlan->count, fPlan->total_aligned / 1024);
    for (unsigned i = 0; i < AMD_BLOB_COUNT; i++)
        fBlobRefs[i] = blobs[i];

    if (!hwInit())
        return;

    if (fPmUp)
        pmPublishMetrics();

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

/* Levels 4-8 on the device: the PSP, firmware, SMU, GMC, IH, SDMA, GFX and
 * compute queues. Runs once from run() and again after a GPU reset
 * (fReinit: the structures and the page-table pool stay, the hardware is
 * programmed from scratch). Returns false when a level fails; the
 * IORegistry says which. */
bool RDNA3Bringup::hwInit()
{
    int ret;

    rdna3_nbio_remap_hdp(fDev);
    /* a known word in VRAM: after a reset it tells whether VRAM survived */
    fResetMagic = rdna3_vram_alloc(fDev, 4096, 4096, "reset-magic");
    if (fResetMagic != UINT64_MAX) {
        uint32_t m = kResetMagic;
        rdna3_vram_write(fDev, fResetMagic, &m, 4);
    }

    /* PSP */
    ret = psp13_init(fPsp, fDev, fSos.data, fSos.len);
    if (!ret)
        ret = psp13_hw_init(fPsp, fPlan, fBlobRefs);
    publish("RDNA3,PSPResult", (uint64_t)(int64_t)ret);
    publish("RDNA3,SOSVersion", fPsp->sos_fw_version);
    if (ret) {
        publishString("RDNA3,Stage2", "failed: PSP");
        return false;
    }
    publishString("RDNA3,Stage2", "PSP up, firmware loaded");
    /* step 5b: the PSP programs the DMCUB code windows when it loads the
     * image; see whether they are there now and after the SMU */
    if (fDmubMode && dmub32_is_supported(fDev))
        dmub32_log_regs(fDev, "after the PSP load");

    if (fLevel >= 5) {
        uint32_t smuv = 0, ifv = 0;
        ret = smu13_hello(fDev, &smuv, &ifv);
        publish("RDNA3,SMUVersion", smuv);
        publish("RDNA3,SMUDriverIF", ifv);
        publishString("RDNA3,Stage2", ret ? "failed: SMU" : "PSP + SMU up");
        if (ret)
            return false;
        /* smu_hw_init: power management, before the GMC like soc21's IP order */
        if (fPmMode)
            pmInit();
        if (fDmubMode && fPmMode && dmub32_is_supported(fDev))
            dmub32_log_regs(fDev, "after the SMU");
        /* amdgpu_dm hw_init comes after the SMU in soc21's IP order */
        if (fDmubMode) {
            dmubInit();
            if (fDmub && fDmub->hw_init) {
                /* rdna3dmubwait=0: dcn32_smu_wait_for_dmub_ack_mclk(false) as
                 * dcn32_update_clocks sends it without FAMS; the first card
                 * runs lost the PMFW at the first metrics transfer after the
                 * clone (step 9bw tests this) */
                uint32_t wait = 1;
                if (PE_parse_boot_argn("rdna3dmubwait", &wait, sizeof(wait)) && !wait) {
                    int r = dcn32_smu_send(fDev, RD_DALSMC_MSG_SetAlwaysWaitDmcubResp, 0, NULL);
                    BLOG("DAL SMU: SetAlwaysWaitDmcubResp 0 -> %d", r);
                    publish("RDNA3,DMUBWaitAckResult", (uint64_t)(int64_t)r);
                }
                fOwner->displayMirror(fDmub, true);   /* rdna3mirror=1: step 9b */
                /* does the PMFW still move a table to DRAM with the clone lit? */
                if (fPmUp && fSmu) {
                    int m = smu13_read_metrics(fDev, fSmu);
                    BLOG("clone: PMFW metrics transfer after the clone: %s (%d)", m ? "FAILED" : "ok", m);
                    dcn32_hubbub_log(fDev, "after the metrics transfer");
                    publish("RDNA3,CloneMetricsResult", (uint64_t)(int64_t)m);
                }
            }
        }
    }

    /* level 6: GMC (gmc_v11_0_hw_init -> gart_enable): GART table in
     * VRAM, MMHUB 3.0.2 VMID0 + contexts, VMID0 TLB flush. GFXHUB comes
     * with the GFX block (gfx_v11_0_hw_init), not here. */
    if (fLevel >= 6) {
        if (!fGmc)
            fGmc = (rdna3_gmc *)IOMalloc(sizeof(*fGmc));
        if (!fGmc) {
            publishString("RDNA3,Stage2", "failed: GMC (no memory)");
            return false;
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
            return false;
        /* GPUVM can now translate: let the GPU reach system memory (GTT BOs
         * through the page tables). amdgpu: pci_set_master() at probe. */
        fPci->setBusMasterEnable(true);
        publish("RDNA3,BusMaster", 1);
        /* after a reset the live VMs get their MMHUB contexts back (the
         * GFXHUB side follows at level 8) */
        for (uint32_t id = 1; id < 16; id++)
            if (fVmidUsed & (1u << id)) {
                mmhub302_setup_vm_pt_regs(fDev, id, fVmPd[id]);
                mmhub302_flush_tlb(fDev, id, 0);
            }
        /* soc21_common_hw_init: doorbell aperture (gfx ring, SDMA and the
         * compute queues ring their doorbells from level 7/8 on) */
        if (fDoorbells && mapDoorbell())
            nbio43_doorbell_init(fDev);
        publish("RDNA3,Doorbells", fDev->doorbell_ok ? 1 : 0);

        /* IH 6.0 ring 0, polled (no MSI registration yet) */
        if (!fIh)
            fIh = (rdna3_ih *)IOMalloc(sizeof(*fIh));
        if (!fIh) {
            publishString("RDNA3,Stage2", "failed: IH (no memory)");
            return false;
        }
        bzero(fIh, sizeof(*fIh));
        ret = ih60_init(fDev, fIh, 0, fGmc->dummy_page_pa, fMsi ? 1 : 0);
        publish("RDNA3,IHResult", (uint64_t)(int64_t)ret);
        if (!ret) {
            /* anything already queued (e.g. from firmware) is drained and counted */
            IOSleep(10);
            publish("RDNA3,IHEarlyIVs", ih60_process(fDev, fIh, nullptr, nullptr));
            if (fMsi && !fWorkLoop && !setupInterrupt())
                BLOG("MSI: not available, the IH ring stays polled");
            /* gmc_v11_0 late_init: page faults of both hubs raise IVs */
            gmc11_set_vm_fault_interrupts(fDev, 1);
            /* amdgpu_dm_hpd_init: hotplug of every pin through the IH, only
             * with an interrupt handler (the polled ring is drained by
             * fence waits, too late for a re-probe) */
            if (fIrqLive) {
                for (uint32_t n = 0; n < RD_AUX_ENGINES; n++)
                    dchpd_irq_set(fDev, n, 1);
                fHpdIrq = true;
                BLOG("HPD: hotplug interrupts enabled on %u pins", RD_AUX_ENGINES);
            }
        }
        publishString("RDNA3,Stage2", ret ? "failed: IH" : "PSP + SMU + MMHUB GART + IH up");
        if (ret)
            return false;

        /* page tables for user VMs: 16 MiB of 4 KiB tables (kept across a
         * GPU reset: VRAM survives a mode-1 reset) */
        if (!fReinit) {
            fPtPool = (rdna3_pt_pool *)IOMalloc(sizeof(*fPtPool));
            if (fPtPool && gpuvm_pool_init(fDev, fPtPool, 4096) == RDNA3_OK) {
                __sync_synchronize();   /* publish the pool before fVmReady */
                fVmReady = true;
                publish("RDNA3,VMTablePages", fPtPool->npages);
            } else {
                BLOG("no page-table pool: GEM_VA stays record-only");
            }
        }
    }

    /* level 7: SDMA0 ring + ring test + a 64 KiB VRAM->VRAM copy check */
    if (fLevel >= 7) {
        if (!fSdma)
            fSdma = (rdna3_sdma_ring *)IOMalloc(sizeof(*fSdma));
        if (!fSdma) {
            publishString("RDNA3,Stage2", "failed: SDMA (no memory)");
            return false;
        }
        bzero(fSdma, sizeof(*fSdma));
        /* SDMA 6.0 translates through the GFXHUB (sdma_v6_0: vm_hub =
         * AMDGPU_GFXHUB(0)); amdgpu enables it in GFX hw_init, which runs
         * before SDMA's, so without level 8 it comes up here */
        ret = fGmc->gfxhub_enabled ? 0 : gfx11_gfxhub_enable(fDev, fGmc);
        publish("RDNA3,GFXHUBResult", (uint64_t)(int64_t)ret);
        if (!ret)
            for (uint32_t id = 1; id < 16; id++)
                if (fVmidUsed & (1u << id)) {
                    gfxhub303_setup_vm_pt_regs(fDev, id, fVmPd[id]);
                    gfxhub303_flush_tlb(fDev, id, 0);
                }
        if (!ret)
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
            return false;
    }

    /* level 8: GFX 11 (RLC autoload check, RS64, GFXHUB, clear state) and
     * the kernel gfx ring 0 with its ring test */
    if (fLevel >= 8) {
        rdna3_rs64_start rs64;
        if (!fGfx)
            fGfx = (rdna3_gfx_ring *)IOMalloc(sizeof(*fGfx));
        if (!fGfx) {
            publishString("RDNA3,Stage2", "failed: GFX (no memory)");
            return false;
        }
        bzero(fGfx, sizeof(*fGfx));
        /* amdgpu_async_gfx_ring (default 1): the KIQ maps the gfx queue */
        fGfx->async = fDev->doorbell_ok;
        ret = gfx11_rs64_from_fw(fBlobs[AMD_BLOB_PFP].data, fBlobs[AMD_BLOB_PFP].len,
                                 fBlobs[AMD_BLOB_ME].data, fBlobs[AMD_BLOB_ME].len,
                                 fBlobs[AMD_BLOB_MEC].data, fBlobs[AMD_BLOB_MEC].len, &rs64);
        if (!ret)
            ret = gfx11_hw_init(fDev, fGmc, fGfx, &rs64, 0);
        /* MEC: the KIQ and the first kernel compute queue (needs doorbells) */
        if (!ret && fDev->doorbell_ok) {
            int kr;
            if (!fKiq)
                fKiq = (rdna3_mec_ring *)IOMalloc(sizeof(*fKiq));
            if (!fKcq)
                fKcq = (rdna3_mec_ring *)IOMalloc(sizeof(*fKcq));
            if (fKiq && fKcq) {
                bzero(fKiq, sizeof(*fKiq));
                bzero(fKcq, sizeof(*fKcq));
                /* GC 11: the KIQ is MES pipe 1 (amdgpu enable_mes_kiq);
                 * the MEC KIQ ignores MAP_QUEUES on the card */
                uint64_t mesStart[2];
                if (gfx11_mes_from_fw(fBlobs[AMD_BLOB_MES].data, fBlobs[AMD_BLOB_MES].len,
                                      fBlobs[AMD_BLOB_MES1].data, fBlobs[AMD_BLOB_MES1].len,
                                      mesStart) == RDNA3_OK) {
                    kr = gfx11_mes_kiq_ring_init(fDev, fKiq, mesStart);
                } else {
                    BLOG("MES firmware headers unusable: MEC KIQ (scheduling will not work)");
                    kr = gfx11_mec_ring_init(fDev, fKiq, 1, RD_KIQ_PIPE, 0, RD_DB_KIQ, 0);
                }
                publish("RDNA3,KIQOnMES", fKiq->me == 3 ? 1 : 0);
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
            /* gfx_v11_0_cp_async_gfx_ring_resume, after the KCQs as in
             * gfx_v11_0_cp_resume */
            if (fGfx->async)
                ret = (fKiq && fKiq->rb.ready) ? gfx11_kgq_start(fDev, fKiq, fGfx) : RDNA3_ENODEV;
        }
        if (!ret && !fGfx->ready)
            ret = RDNA3_ENODEV;   /* async queue never mapped (no KIQ) */
        publish("RDNA3,GFXResult", (uint64_t)(int64_t)ret);
        publish("RDNA3,GFXAsync", fGfx->async ? 1 : 0);
        /* mes_v11_0_hw_init comes after GFX in soc21's IP order: the
         * kernel queues are mapped by the KIQ already (step 8c, opt-in) */
        if (!ret && fMesWanted && fKiq && fKiq->rb.ready && fKiq->me == 3)
            mesStart("level 8");
        /* amdgpu_device_ip_late_init -> amdgpu_device_set_cg_state(GATE) */
        if (!ret && fCgWanted)
            cgStart("level 8");
        /* ... and amdgpu_device_set_pg_state(GATE) -> amdgpu_gfx_off_ctrl:
         * the bring-up's own hold goes, GFXOFF is allowed 100 ms later */
        if (!ret && fGfxOffWanted && fPmUp && fSmu) {
            fGfxOffCall = thread_call_allocate((thread_call_func_t)gfxOffTick, this);
            if (fGfxOffCall) {
                IOLock *l = fOwner->drmLock();
                if (l)
                    IOLockLock(l);
                gfxOffRelease();
                if (l)
                    IOLockUnlock(l);
            }
        }
        if (!ret && fIrqLive)
            gfx11_set_eop_interrupt(fDev, fGfx, 1);
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

    publish("RDNA3,GPUResets", fGpuResets);
    publish("RDNA3,VRAMLost", fVramLostCount);
    return true;
}

/* -------------------------- power management -------------------------- */

/* smu_hw_init -> smu_smc_hw_setup on the PMFW the PSP just loaded. Failure
 * here is not fatal for the rest of the bring-up: the GPU stays on its boot
 * clocks like with rdna3pm=0. */
void RDNA3Bringup::pmInit()
{
    uint64_t allowed = fPmMask ? fPmMask
                     : (fPmMode >= 2 ? RD_SMU_FEATURES_AMDGPU : RD_SMU_FEATURES_SAFE);
    /* GFXOFF wanted: the feature runs, AllowGfxOff comes after the GFX init */
    if (fGfxOffWanted && !fPmMask)
        allowed |= RD_SMU_FEATURE(FEATURE_GFXOFF_BIT);
    uint32_t gen = 3, width = 6;     /* Gen4 x16 unless the link says less */
    const rdna3_vbios *vb = nullptr;
    int ret;

    fPmUp = false;
    if (!fSmu)
        fSmu = (rdna3_smu *)IOMalloc(sizeof(*fSmu));
    if (!fRom)
        fRom = (uint8_t *)IOMalloc(RD_VBIOS_MAX_LEN);
    if (!fSmu || !fRom) {
        publishString("RDNA3,PM", "failed: no memory");
        return;
    }
    bzero(fSmu, sizeof(*fSmu));

    /* the VBIOS: boot-up clocks and the PMFW BoardTable */
    ret = vbios_read(fDev, &fVbios, fRom, RD_VBIOS_MAX_LEN);
    publish("RDNA3,VBIOSResult", (uint64_t)(int64_t)ret);
    if (ret == RDNA3_OK) {
        publish("RDNA3,VBIOSSize", fVbios.len);
        publishString("RDNA3,VBIOSPartNumber", fVbios.part_number);
        vb = &fVbios;
    } else {
        BLOG("PM: VBIOS not readable (%d), going on with the PMFW's BoardTable", ret);
    }

    /* smu_smc_hw_setup's pcie_gen / pcie_width from the link capability
     * (PCIe cap LNKCAP: max speed 1..5 -> 0..4, max width x1..x32 -> 1..7) */
    {
        UInt8 pcie = 0;
        if (fPci->findPCICapability(kIOPCIPCIExpressCapability, &pcie) && pcie) {
            uint32_t lnkcap = fPci->configRead32(pcie + 0x0C);
            uint32_t speed = lnkcap & 0xf, lanes = (lnkcap >> 4) & 0x3f;
            if (speed >= 1 && speed <= 5)
                gen = speed - 1;
            switch (lanes) {
            case 1: width = 1; break;
            case 2: width = 2; break;
            case 4: width = 3; break;
            case 8: width = 4; break;
            case 12: width = 5; break;
            case 16: width = 6; break;
            case 32: width = 7; break;
            default: break;
            }
        }
    }

    /* psp_sw_init: SCPM status from the PSP runtime database */
    {
        uint32_t scpm_status = 0;
        int scpm = psp13_scpm_enabled(fDev, &scpm_status);
        publish("RDNA3,SCPM", scpm_status);
        ret = smu13_pm_init(fDev, fSmu, vb, allowed, gen, width, scpm);
    }
    publish("RDNA3,PMResult", (uint64_t)(int64_t)ret);
    if (ret) {
        publishString("RDNA3,PM", "failed: smu_smc_hw_setup");
        /* DEV_INFO clocks: the VBIOS boot clocks (10 kHz units) at least */
        if (fOwner && fSmu->boot.gfxclk)
            fOwner->drmSetClocks(fSmu->boot.gfxclk * 10, fSmu->boot.gfxclk * 10, fSmu->boot.uclk * 10,
                                 fSmu->boot.uclk * 10);
        return;
    }
    fPmUp = true;
    publishString("RDNA3,PM", fSmu->dpm_running ? "dpm running" : "features enabled, dpm off");
    publish("RDNA3,PMFeaturesAllowed", fSmu->allowed);
    publish("RDNA3,PMFeaturesRunning", fSmu->enabled);
    publish("RDNA3,PMBootGfxClkMHz", fSmu->boot.gfxclk / 100);
    publish("RDNA3,PMBootUclkMHz", fSmu->boot.uclk / 100);
    publish("RDNA3,PMGfxClkMinMHz", fSmu->gfx.level[0]);
    publish("RDNA3,PMGfxClkMaxMHz", fSmu->gfx.level[fSmu->gfx.count ? fSmu->gfx.count - 1 : 0]);
    publish("RDNA3,PMUclkMaxMHz", fSmu->uclk.level[fSmu->uclk.count ? fSmu->uclk.count - 1 : 0]);
    publish("RDNA3,PMSocClkMaxMHz", fSmu->soc.level[fSmu->soc.count ? fSmu->soc.count - 1 : 0]);
    publish("RDNA3,PMPowerLimitW", fSmu->ppt_limit_w);
    publish("RDNA3,PMTempLimitEdge", fSmu->pptable.SkuTable.TemperatureLimit[TEMP_EDGE]);
    publish("RDNA3,PMTempLimitHotspot", fSmu->pptable.SkuTable.TemperatureLimit[TEMP_HOTSPOT]);
    publish("RDNA3,PMFanMaxRPM", fSmu->pptable.SkuTable.FanMaximumRpm);

    /* DEV_INFO clock range (amdgpu_kms with dpm_enabled: amdgpu_dpm_get_sclk
     * / get_mclk, lowest and highest DPM level), MHz -> kHz */
    if (fOwner && fSmu->gfx.count && fSmu->uclk.count)
        fOwner->drmSetClocks(fSmu->gfx.level[0] * 1000, fSmu->gfx.level[fSmu->gfx.count - 1] * 1000,
                             fSmu->uclk.level[0] * 1000, fSmu->uclk.level[fSmu->uclk.count - 1] * 1000);
}

/* SMU_TABLE_SMU_METRICS snapshot into the IORegistry */
/* ------------------------------ DMCUB ------------------------------ */

/* dm_dmub_sw_init + dm_dmub_hw_init: the PSP loaded the DMCUB image with
 * the rest of the firmware (LOAD_IP_FW GFX_FW_TYPE_DMUB); stop whatever the
 * VBIOS left running, give it data/VBIOS/mailbox/trace/state windows in
 * VRAM, release it and wait for DAL_FW + MAILBOX_RDY, then do the first
 * mailbox round trip (QUERY_FEATURE_CAPS) like dc_dmub_srv. Opt-in until
 * tested on the card: the boot display runs through this core. */
void RDNA3Bringup::dmubInit()
{
    const amd_fw_blob_ref *fw = &fBlobRefs[AMD_BLOB_DMCUB];
    int ret, pspLoaded;

    if (!fDmub)
        fDmub = (rdna3_dmub *)IOMalloc(sizeof(*fDmub));
    if (!fDmub) {
        publishString("RDNA3,DMUB", "failed: no memory");
        return;
    }
    if (fDmubMode == 3) {
        /* the GOP's firmware, as it runs: its inbox, no reset, no load */
        dmub32_log_regs(fDev, "before attach");
        ret = dmub32_attach(fDmub, fDev);
        publish("RDNA3,DMUBResult", (uint64_t)(int64_t)ret);
        publish("RDNA3,DMUBBootStatus", fDmub->boot_status);
        if (ret != RDNA3_OK) {
            publishString("RDNA3,DMUB", "failed: attach");
            return;
        }
        ret = dmub32_query_caps(fDmub);
        publish("RDNA3,DMUBCapsResult", (uint64_t)(int64_t)ret);
        if (ret == RDNA3_OK)
            publish("RDNA3,DMUBCaps", fDmub->feature_caps[0]);
        publishString("RDNA3,DMUB", ret == RDNA3_OK ? "attached, mailbox up" : "attached, mailbox silent");
        BLOG("DMUB: attached to the GOP's firmware, boot status 0x%x, caps query %d", fDmub->boot_status, ret);
        return;
    }
    if (!fw->data) {
        publishString("RDNA3,DMUB", "failed: no dcn_3_2_0_dmcub.bin");
        return;
    }
    /* the VBIOS image goes into window CW3: the firmware runs its tables */
    if (!fVbios.len) {
        if (!fRom)
            fRom = (uint8_t *)IOMalloc(RD_VBIOS_MAX_LEN);
        if (!fRom || vbios_read(fDev, &fVbios, fRom, RD_VBIOS_MAX_LEN) != RDNA3_OK) {
            publishString("RDNA3,DMUB", "failed: VBIOS not readable");
            return;
        }
    }
    pspLoaded = amd_ucode_plan_find(fPlan, AMD_UCODE_DMCUB) != nullptr;
    ret = dmub32_sw_init(fDmub, fDev, (const uint8_t *)fw->data, (uint32_t)fw->len, fVbios.len);
    if (ret == RDNA3_OK)
        ret = dmub32_hw_init(fDmub, fVbios.data, fVbios.len, pspLoaded, 100000);
    if (ret == RDNA3_ETIME) {
        /* amdgpu only warns here ("Wait for DMUB auto-load failed"); see
         * whether the firmware comes up late */
        if (dmub32_wait_boot(fDmub, 2000000) == RDNA3_OK) {
            BLOG("DMUB: firmware came up within 2 s after the 100 ms wait");
            publish("RDNA3,DMUBLateBoot", 1);
            ret = RDNA3_OK;
        }
        if (ret)
            dmub32_log_state(fDmub, "after 2 s more");
    }
    publish("RDNA3,DMUBResult", (uint64_t)(int64_t)ret);
    publish("RDNA3,DMUBVersion", fDmub->fw_version);
    publish("RDNA3,DMUBBootStatus", fDmub->boot_status);
    if (ret != RDNA3_OK) {
        publishString("RDNA3,DMUB", ret == RDNA3_ENODEV ? "not present" : "failed");
        return;
    }
    ret = dmub32_query_caps(fDmub);
    publish("RDNA3,DMUBCapsResult", (uint64_t)(int64_t)ret);
    if (ret == RDNA3_OK)
        publish("RDNA3,DMUBCaps", fDmub->feature_caps[0]);
    publishString("RDNA3,DMUB", ret == RDNA3_OK ? "running, mailbox up" : "running, mailbox silent");
    BLOG("DMUB: firmware 0x%08x, boot status 0x%x, %s, caps 0x%08x", fDmub->fw_version,
         fDmub->boot_status, pspLoaded ? "PSP-loaded" : "backdoor", fDmub->feature_caps[0]);
}

/* mes_v11_0_hw_init: the scheduler pipe mapped by the KIQ, SET_HW_RESOURCES,
 * QUERY_SCHEDULER_STATUS; the firmware version decides the legacy queue map */
/* gfx_v11_0_set_clockgating_state(GATE): the RLC may stop the GFX clocks
 * when it is idle, so the PMFW stops counting it as 100 % busy */
void RDNA3Bringup::cgStart(const char *why)
{
    int r = gfx11_set_clock_gating(fDev, 1, RD_GFX_CG_GC_11_0_3);
    BLOG("GFX clock gating (%s): %d", why, r);
    publish("RDNA3,GfxCGResult", (uint64_t)(int64_t)r);
    if (fSocCgWanted) {
        /* amdgpu_device_set_cg_state(GATE) also reaches soc21_common and gmc_v11_0 */
        int s = soc21_set_clock_gating(fDev, 1);
        publish("RDNA3,SocCGResult", (uint64_t)(int64_t)s);
        publish("RDNA3,SocCGState", soc21_clock_gating_state(fDev));
    }
}

void RDNA3Bringup::mesStart(const char *why)
{
    int ret = RDNA3_OK;
    if (!fMes) {
        fMes = (rdna3_mes *)IOMalloc(sizeof(*fMes));
        if (!fMes)
            return;
        bzero(fMes, sizeof(*fMes));
        ret = gfx11_mes_sched_init(fDev, fMes);
    }
    if (!ret)
        ret = gfx11_mes_sched_start(fDev, fKiq, fMes);
    publish("RDNA3,MESResult", (uint64_t)(int64_t)ret);
    publish("RDNA3,MESSchedVersion", fMes->sched_version);
    publish("RDNA3,MESKiqVersion", fMes->kiq_version);
    publish("RDNA3,MESLegacyQueueMap", fMes->legacy_queue_map ? 1 : 0);
    BLOG("MES: scheduler %s %s (%d), firmware sched 0x%x kiq 0x%x", why, ret ? "FAILED" : "up", ret,
         fMes->sched_version, fMes->kiq_version);
}

void RDNA3Bringup::pmPublishMetrics()
{
    int ret = smu13_read_metrics(fDev, fSmu);
    publish("RDNA3,PMMetricsResult", (uint64_t)(int64_t)ret);
    if (ret)
        return;
    publish("RDNA3,PMGfxClkMHz", smu13_metric_gfxclk_mhz(fSmu));
    publish("RDNA3,PMUclkMHz", smu13_metric_uclk_mhz(fSmu));
    publish("RDNA3,PMTempEdgeC", smu13_metric_temp_edge_c(fSmu));
    publish("RDNA3,PMTempHotspotC", smu13_metric_temp_hotspot_c(fSmu));
    publish("RDNA3,PMTempMemC", smu13_metric_temp_mem_c(fSmu));
    publish("RDNA3,PMPowerW", smu13_metric_power_w(fSmu));
    publish("RDNA3,PMGfxActivity", smu13_metric_gfx_activity(fSmu));
    publish("RDNA3,PMFanRPM", smu13_metric_fan_rpm(fSmu));
    publish("RDNA3,PMVddGfxmV", smu13_metric_vddgfx_mv(fSmu));
    publish("RDNA3,PMThrottlers", smu13_metric_throttlers(fSmu));
    BLOG("PM: gfxclk %u MHz, uclk %u MHz, edge %u C, hotspot %u C, %u W, fan %u rpm",
         smu13_metric_gfxclk_mhz(fSmu), smu13_metric_uclk_mhz(fSmu),
         smu13_metric_temp_edge_c(fSmu), smu13_metric_temp_hotspot_c(fSmu),
         smu13_metric_power_w(fSmu), smu13_metric_fan_rpm(fSmu));
}

/* ----------------------------- recovery ----------------------------- */

struct RDNA3Vm {
    rdna3_vm vm;
    uint32_t vmid;
    bool     live;       /* page tables exist */
    bool     guilty;     /* a job of this VM hung or page-faulted: no more submissions */
    uint32_t resetSeen, vramLostSeen;   /* fResetCount / fVramLostCount when created */
    uint64_t faultAddr;  /* amdgpu_vm_update_fault_cache: last fault for AMDGPU_INFO_GPUVM_FAULT */
    uint32_t faultStatus, faultHub;
    bool     gfxFlush;   /* next GFX job re-points the VMID and flushes from the ring */
    bool     compFlush;  /* same for the next compute job (the hub is shared, the rings are not) */
};


void RDNA3Bringup::markGuilty(uint32_t vmid, const char *why)
{
    RDNA3Vm *v = vmid < 16 ? fVmByVmid[vmid] : nullptr;
    if (!v) {
        BLOG("%s on VMID %u, which no client owns", why, vmid);
        return;
    }
    if (!v->guilty)
        BLOG("%s: VMID %u's client is guilty, its submissions now fail", why, vmid);
    v->guilty = true;
}

uint32_t RDNA3Bringup::vmQuery(void *p)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    uint32_t fl = 0;
    if (!v)
        return 0;
    if (v->guilty)
        fl |= AMDGPU_CTX_QUERY2_FLAGS_GUILTY;
    if (v->resetSeen != fResetCount)
        fl |= AMDGPU_CTX_QUERY2_FLAGS_RESET;
    if (v->vramLostSeen != fVramLostCount)
        fl |= AMDGPU_CTX_QUERY2_FLAGS_RESET | AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST;
    return fl;
}

int RDNA3Bringup::vmFaultInfo(void *p, uint64_t *addr, uint32_t *status, uint32_t *vmhub)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    if (!v)
        return -RD_EINVAL;
    *addr = v->faultAddr;
    *status = v->faultStatus;
    *vmhub = v->faultHub;
    return 0;
}

void RDNA3Bringup::gfxOffHold()
{
    fGfxOffHolds++;
    if (fGfxOffOn && fDev) {
        int r = smu13_gfx_off(fDev, 0);
        fGfxOffOn = false;
        if (r)
            BLOG("GFXOFF: DisallowGfxOff failed (%d)", r);
    }
}

void RDNA3Bringup::gfxOffRelease()
{
    if (fGfxOffHolds && --fGfxOffHolds == 0 && fGfxOffCall && !fGfxOffStop) {
        uint64_t dl;
        clock_interval_to_deadline(100, kMillisecondScale, &dl);
        thread_call_enter_delayed(fGfxOffCall, dl);
    }
}

void RDNA3Bringup::gfxOffTick(void *owner, void *)
{
    RDNA3Bringup *b = (RDNA3Bringup *)owner;
    IOLock *l = b->fOwner->drmLock();
    if (!l)
        return;
    IOLockLock(l);
    if (!b->fGfxOffStop && !b->fGfxOffHolds && !b->fGfxOffOn && b->fDev) {
        static bool logged = false;
        int r = smu13_gfx_off(b->fDev, 1);
        b->fGfxOffOn = r == RDNA3_OK;
        if (!logged || r) {
            BLOG("GFXOFF: AllowGfxOff -> %d", r);
            logged = true;
        }
        b->publish("RDNA3,GfxOffResult", (uint64_t)(int64_t)r);
    }
    IOLockUnlock(l);
}

bool RDNA3Bringup::stats(uint32_t v[8])
{
    uint64_t now = mach_absolute_time(), ns = 0;
    if (!fPmUp || !fFinished || !fSmu)
        return false;
    absolutetime_to_nanoseconds(now - fMetricsAt, &ns);
    if (!fMetricsAt || ns > 100ull * 1000 * 1000) {
        if (smu13_read_metrics(fDev, fSmu))
            return false;
        fMetricsAt = now;
    }
    v[0] = smu13_metric_gfx_activity(fSmu);
    v[1] = smu13_metric_temp_edge_c(fSmu);
    v[2] = smu13_metric_temp_hotspot_c(fSmu);
    v[3] = smu13_metric_temp_mem_c(fSmu);
    v[4] = smu13_metric_fan_rpm(fSmu);
    v[5] = smu13_metric_gfxclk_mhz(fSmu);
    v[6] = smu13_metric_uclk_mhz(fSmu);
    v[7] = smu13_metric_power_w(fSmu);
    return true;
}

/* Idle power (card runs 8-17): the PMFW reports 100 % GFX activity and
 * keeps GFXCLK near 2.7 GHz (52 W) where Linux idles at 40 MHz / 12 W.
 * Instead of one hypothesis per card run, try them in one boot, each for
 * 10 s (5 ticks, the first one settling), and log what the PMFW reports:
 *   0 baseline, no GC register reads (the trend's reads wake GC)
 *   1 + AllowGfxOff (amdgpu_gfx_off_ctrl(true))
 *   2 + SetWorkloadMask POWER_SAVING (smu_v13_0_0_set_power_profile_mode)
 *   3 + HDP/ATHUB clock gating (soc21_set_clock_gating)
 *   4 + GFXCLK soft max at the lowest level: the floor at a low clock
 *   5 everything back as configured */
bool RDNA3Bringup::pmExperimentTick()
{
    static const char *const kName[6] = { "baseline, no GC reads", "+ GFXOFF allowed",
                                          "+ workload POWER_SAVING", "+ HDP/ATHUB clock gating",
                                          "+ GFXCLK soft max lowest", "restored" };
    enum { kTicks = 5, kPhases = 6 };
    rdna3_pm_detail p;
    if (!fPmExpWanted || !fFinished || !fPmUp || !fSmu || !fDev || fPmExpPhase >= kPhases)
        return false;
    if (fPmExpTick % kTicks == 0) {
        int r = RDNA3_OK;
        switch (fPmExpPhase) {
        case 1:
            r = smu13_gfx_off(fDev, 1);
            fGfxOffOn = r == RDNA3_OK;
            break;
        case 2:
            r = smu13_set_workload(fDev, WORKLOAD_PPLIB_POWER_SAVING_BIT);
            break;
        case 3:
            r = soc21_set_clock_gating(fDev, 1);
            break;
        case 4:
            if (fSmu->gfx.count)
                r = smu13_set_soft_freq_range(fDev, fSmu, PPCLK_GFXCLK, 0, fSmu->gfx.level[0]);
            break;
        case 5:
            if (fSmu->gfx.count)
                r = smu13_set_soft_freq_range(fDev, fSmu, PPCLK_GFXCLK, fSmu->gfx.level[0],
                                              fSmu->gfx.level[fSmu->gfx.count - 1]);
            smu13_set_workload(fDev, WORKLOAD_PPLIB_DEFAULT_BIT);
            if (!fSocCgWanted)
                soc21_set_clock_gating(fDev, 0);
            if (!fGfxOffWanted && fGfxOffOn) {
                smu13_gfx_off(fDev, 0);
                fGfxOffOn = false;
            }
            break;
        default:
            break;
        }
        BLOG("PM experiment %u (%s): started (%d)", fPmExpPhase, kName[fPmExpPhase], r);
        bzero(fPmExpSum, sizeof(fPmExpSum));
        fPmExpSamples = 0;
    } else {
        smu13_metric_detail(fSmu, &p);
        fPmExpSum[0] += p.gfx_activity;
        fPmExpSum[1] += p.gfxclk_target;
        fPmExpSum[2] += p.gfxclk_pre_ds;
        fPmExpSum[3] += p.gfxclk_post_ds;
        fPmExpSum[4] += p.memclk_post_ds;
        fPmExpSum[5] += p.socket_w;
        fPmExpSum[6] += p.pcie_busy;
        fPmExpSum[7] += p.uclk_activity;
        fPmExpSamples++;
    }
    if (++fPmExpTick % kTicks == 0) {
        uint32_t n = fPmExpSamples ? fPmExpSamples : 1;
        BLOG("PM experiment %u (%s): activity %llu%%, gfxclk target %llu pre-DS %llu post-DS %llu MHz, "
             "memclk post-DS %llu MHz, %llu W, PCIe busy %llu, UCLK activity %llu%%",
             fPmExpPhase, kName[fPmExpPhase], fPmExpSum[0] / n, fPmExpSum[1] / n, fPmExpSum[2] / n,
             fPmExpSum[3] / n, fPmExpSum[4] / n, fPmExpSum[5] / n, fPmExpSum[6] / n, fPmExpSum[7] / n);
        fPmExpPhase++;
    }
    return true;
}

/* Idle power without the PMFW's help (card runs 8-17: it counts the GFX
 * 100 % busy and keeps GFXCLK near 2.7 GHz while nothing runs): no
 * submission for 5 s -> SetSoftMaxByFreq(GFXCLK, lowest level), the next
 * submission lifts it (smu_v13_0_set_soft_freq_limited_range). Not an
 * amdgpu policy: a workaround until the busy signal is understood. */
void RDNA3Bringup::idleCapTick()
{
    uint64_t ns = 0;
    if (!fIdleCapWanted || fIdleCapped || !fFinished || !fPmUp || !fSmu || !fDev || !fSmu->gfx.count)
        return;
    if (fPmExpWanted && fPmExpPhase < 6)
        return;   /* the experiment owns the clocks while it runs */
    absolutetime_to_nanoseconds(mach_absolute_time() - fLastSubmit, &ns);
    if (fLastSubmit && ns < 5ull * 1000 * 1000 * 1000)
        return;
    int r = smu13_set_soft_freq_range(fDev, fSmu, PPCLK_GFXCLK, 0, fSmu->gfx.level[0]);
    fIdleCapped = r == RDNA3_OK;
    if (fIdleCaps++ < 4)
        BLOG("PM: idle, GFXCLK capped at %u MHz (%d)", fSmu->gfx.level[0], r);
    publish("RDNA3,IdleCaps", fIdleCaps);
}

void RDNA3Bringup::idleCapLift()
{
    int r = smu13_set_soft_freq_range(fDev, fSmu, PPCLK_GFXCLK, 0, fSmu->gfx.level[fSmu->gfx.count - 1]);
    fIdleCapped = false;
    if (fIdleCaps <= 4)
        BLOG("PM: work, GFXCLK cap lifted to %u MHz (%d)", fSmu->gfx.level[fSmu->gfx.count - 1], r);
}

void RDNA3Bringup::logBusy()
{
    rdna3_gfx_status st;
    if (!fFinished || !fDev)
        return;
    if (fGfxOffOn) {
        BLOG("PM: GFX is off (GFXOFF), GC registers not read");
        return;
    }
    gfx11_read_status(fDev, fGfx, &st);
    BLOG("PM: GRBM_STATUS 0x%08x STATUS2 0x%08x SE0 0x%08x CP_STAT 0x%08x CPC 0x%08x CPF 0x%08x "
         "ME_CNTL 0x%08x RLC_SAFE_MODE 0x%x SDMA0 0x%08x", st.grbm_status, st.grbm_status2,
         st.grbm_status_se0, st.cp_stat, st.cp_cpc_status, st.cp_cpf_status, st.cp_me_cntl,
         st.rlc_safe_mode, st.sdma0_status);
    /* RLC_GPM_STAT bit 1 GFX_POWER_STATUS, bit 2 GFX_CLOCK_STATUS, bit 3
     * GFX_LS_STATUS: whether the RLC actually gated the GFX clocks, i.e.
     * whether the PMFW's 100 % activity comes from busy GC or ungated clocks */
    BLOG("PM: RLC_STAT 0x%08x GPM_STAT 0x%08x (pwr %u clk %u ls %u) PG_CNTL 0x%08x CGCG_CGLS 0x%08x "
         "CP_BUSY 0x%08x CPC_BUSY 0x%08x CPF_BUSY 0x%08x", st.rlc_stat, st.rlc_gpm_stat,
         (st.rlc_gpm_stat >> 1) & 1, (st.rlc_gpm_stat >> 2) & 1, (st.rlc_gpm_stat >> 3) & 1,
         st.rlc_pg_cntl, st.rlc_cgcg_cgls_ctrl, st.cp_busy_stat, st.cp_cpc_busy_stat,
         st.cp_cpf_busy_stat);
}

/* amdgpu_dpm_read_sensor through smu_v13_0_0_read_sensor: the units are
 * amdgpu's (MHz, millidegrees, %, W, mV). Called under the DRM lock once
 * the bring-up finished, so the SMU mailbox is ours. */
int RDNA3Bringup::sensor(uint32_t type, uint32_t *value)
{
    uint64_t now = mach_absolute_time(), ns = 0;
    if (!fPmUp || !fFinished || !fSmu)
        return -RD_EINVAL;
    switch (type) {
    case AMDGPU_INFO_SENSOR_STABLE_PSTATE_GFX_SCLK:
    case AMDGPU_INFO_SENSOR_PEAK_PSTATE_GFX_SCLK:
        *value = fSmu->gfx.level[fSmu->gfx.count ? fSmu->gfx.count - 1 : 0];
        return 0;
    case AMDGPU_INFO_SENSOR_STABLE_PSTATE_GFX_MCLK:
    case AMDGPU_INFO_SENSOR_PEAK_PSTATE_GFX_MCLK:
        *value = fSmu->uclk.level[fSmu->uclk.count ? fSmu->uclk.count - 1 : 0];
        return 0;
    default:
        break;
    }
    absolutetime_to_nanoseconds(now - fMetricsAt, &ns);
    if (!fMetricsAt || ns > 100ull * 1000 * 1000) {
        if (smu13_read_metrics(fDev, fSmu))
            return -RD_EIO;
        fMetricsAt = now;
    }
    switch (type) {
    case AMDGPU_INFO_SENSOR_GFX_SCLK:     *value = smu13_metric_gfxclk_mhz(fSmu); return 0;
    case AMDGPU_INFO_SENSOR_GFX_MCLK:     *value = smu13_metric_uclk_mhz(fSmu); return 0;
    case AMDGPU_INFO_SENSOR_GPU_TEMP:     *value = smu13_metric_temp_edge_c(fSmu) * 1000; return 0;
    case AMDGPU_INFO_SENSOR_GPU_LOAD:     *value = smu13_metric_gfx_activity(fSmu); return 0;
    case AMDGPU_INFO_SENSOR_GPU_AVG_POWER: *value = smu13_metric_power_w(fSmu); return 0;
    case AMDGPU_INFO_SENSOR_VDDGFX:       *value = smu13_metric_vddgfx_mv(fSmu); return 0;
    default:
        return -RD_EINVAL;
    }
}

/* amdgpu_fence_driver_force_completion on every kernel ring: the last
 * emitted seq lands, waiters wake up (and see the guilty flag) */
void RDNA3Bringup::forceFences()
{
    if (fGfx)
        gfx11_force_fence_completion(fDev, fGfx);
    if (fKcq)
        gfx11_force_fence_completion(fDev, &fKcq->rb);
    if (fKiq)
        gfx11_force_fence_completion(fDev, &fKiq->rb);
    if (fSdma && fSdma->wb != UINT64_MAX)
        rdna3_vram_write(fDev, fSdma->wb + RD_SDMA_WB_FENCE, &fSdma->fence_seq, 4);
    IOLockLock(fLock);
    IOLockWakeup(fLock, &fFenceEvent, false);
    IOLockUnlock(fLock);
}

/* amdgpu_job_timedout for (ip, ring): state dump, soft recovery of the
 * guilty VMID's waves, then a GFX soft reset, then the whole chip. Called
 * from fenceWait under the DRM lock, so nothing else touches the rings. */
int RDNA3Bringup::recover(uint32_t ip, uint32_t ring, uint64_t seq)
{
    rdna3_gfx_ring *r = ip == AMDGPU_HW_IP_GFX ? fGfx
                      : (ip == AMDGPU_HW_IP_COMPUTE && fKcq) ? &fKcq->rb : nullptr;
    uint32_t vmid = r ? r->last_vmid : (ip == AMDGPU_HW_IP_DMA ? fSdmaLastVmid : 0);
    rdna3_gfx_status st;
    bool ok = false;

    /* whatever runs next re-programs its GDS partition (amdgpu_vmid_reset) */
    memset(fGdsKnown, 0, sizeof(fGdsKnown));
    BLOG("hang: %s ring %u, fence %llu not signalled within 10 s",
         ip == AMDGPU_HW_IP_DMA ? "DMA" : ip == AMDGPU_HW_IP_GFX ? "GFX" : "COMPUTE", ring,
         (unsigned long long)seq);
    if (fGfx) {
        gfx11_read_status(fDev, fGfx, &st);
        gfx11_log_status(fDev, &st, "timeout");
    }
    if (vmid)
        markGuilty(vmid, "GPU hang");

    /* amdgpu_ring_soft_recovery: kill the waves, give the fence 100 ms */
    if (r && vmid) {
        gfx11_kill_waves(fDev, vmid);
        fSoftRecoveries++;
        publish("RDNA3,SoftRecoveries", fSoftRecoveries);
        for (int i = 0; i < 100 && !fenceSignaled(ip, ring, seq); i++)
            IOSleep(1);
        if (fenceSignaled(ip, ring, seq)) {
            BLOG("hang: soft recovery worked, the ring runs again");
            return 0;
        }
    }

    /* a DMA hang: sdma_v6_0_soft_reset on that engine alone (amdgpu would
     * use MES RESET_QUEUE, which needs the MES this driver does not run);
     * its pending fences read as done afterwards */
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ring != UINT64_MAX &&
        sdma60_soft_reset(fDev, fSdma) == RDNA3_OK) {
        fSdmaResets++;
        fResetCount++;
        publish("RDNA3,SDMAResets", fSdmaResets);
        publish("RDNA3,Resets", fResetCount);
        IOLockLock(fLock);
        IOLockWakeup(fLock, &fFenceEvent, false);
        IOLockUnlock(fLock);
        BLOG("hang: SDMA0 soft reset done, the DMA ring runs again");
        return fenceSignaled(ip, ring, seq) ? 0 : -RD_ETIME;
    }

    /* the engine needs a reset; pending fences complete first so every
     * waiter returns (guilty contexts see -ECANCELED afterwards) */
    forceFences();
    if (r && fGfx && fGfx->ready) {
        ok = gfx11_soft_reset(fDev, fGfx, fKiq, fKcq) == RDNA3_OK;
        if (ok) {
            for (uint32_t id = 1; id < 16; id++)
                if (fVmByVmid[id])
                    fVmByVmid[id]->gfxFlush = fVmByVmid[id]->compFlush = true;
            fResetCount++;
            BLOG("hang: GFX soft reset done (%u resets so far)", fResetCount);
            /* the KIQ restart reset both MES pipes: the scheduler again */
            if (fMes && fMes->ring.rb.ring_size)
                mesStart("after soft reset");
            if (fCgWanted)
                cgStart("after soft reset");
        }
    }
    if (!ok)
        ok = gpuReset();
    publish("RDNA3,GPUResets", fGpuResets);
    publish("RDNA3,Resets", fResetCount);
    return fenceSignaled(ip, ring, seq) ? 0 : -RD_ETIME;
}

/* amdgpu_device_gpu_recover with the mode-1 method (soc21): the PMFW
 * resets the chip, the PSP bootloader comes back, levels 4-8 run again.
 * VRAM normally survives; the magic word says so. */
bool RDNA3Bringup::gpuReset()
{
    uint32_t magic = 0, i;
    bool ok;

    fGpuResets++;
    BLOG("GPU reset #%llu: mode-1 through the PMFW, then bring-up from level 4",
         (unsigned long long)fGpuResets);
    if (fIntr)
        fIntr->disable();
    for (uint32_t id = 1; id < 16; id++)
        if (fVmByVmid[id])
            fVmByVmid[id]->gfxFlush = fVmByVmid[id]->compFlush = true;
    if (fKcq && fKcq->rb.ready)
        gfx11_set_eop_interrupt(fDev, &fKcq->rb, 0);
    if (fGfx && fGfx->ready)
        gfx11_set_eop_interrupt(fDev, fGfx, 0);

    /* soc21_asic_mode1_reset: no bus mastering across the reset */
    fPci->setBusMasterEnable(false);
    smu13_mode1_reset(fDev);
    if (psp13_wait_for_bootloader(fPsp) != RDNA3_OK)
        BLOG("GPU reset: the PSP bootloader did not come back");
    for (i = 0; i < fDev->usec_timeout; i++) {
        if (rdna3_rreg(fDev, mmRCC_CONFIG_MEMSIZE) != 0xffffffffu)
            break;
        IODelay(1);
    }
    if (fResetMagic != UINT64_MAX) {
        rdna3_vram_read(fDev, fResetMagic, &magic, 4);
        if (magic != kResetMagic) {
            fVramLostCount++;
            BLOG("GPU reset: VRAM content lost (0x%08x), clients see VRAMLOST", magic);
        }
    }

    /* everything from level 4 on is programmed again by hwInit() */
    rdna3_vram_alloc_init(fDev, fAllocLo, fAllocHi);
    fDev->doorbell_ok = 0;
    if (fSdma) fSdma->ready = 0;
    if (fGfx) fGfx->ready = 0;
    if (fKiq) { fKiq->rb.ready = 0; fKiq->mapped = 0; }
    if (fKcq) { fKcq->rb.ready = 0; fKcq->mapped = 0; }
    fReinit = true;
    ok = hwInit();
    fReinit = false;
    fResetCount++;
    if (fIntr)
        fIntr->enable();
    BLOG("GPU reset #%llu %s", (unsigned long long)fGpuResets, ok ? "done, rings back up" : "FAILED");
    publish("RDNA3,VRAMLost", fVramLostCount);
    return ok;
}

/* ------------------------------ GPUVM ------------------------------ */


void *RDNA3Bringup::vmCreate()
{
    RDNA3Vm *v = (RDNA3Vm *)IOMalloc(sizeof(*v));
    if (!v)
        return nullptr;
    bzero(v, sizeof(*v));
    v->vm.root = UINT64_MAX;
    v->resetSeen = fResetCount;
    v->vramLostSeen = fVramLostCount;
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
            fVmByVmid[id] = v;
            /* both hubs: VCN walks MMHUB, SDMA and shaders the GFXHUB */
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
        fVmByVmid[v->vmid] = nullptr;
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

int RDNA3Bringup::copyVramToPages(uint64_t vramOff, const uint64_t *pages, uint64_t npages,
                                  uint64_t len)
{
    const uint32_t winPages = 4096;   /* 16 MiB at the top of GART, nothing else maps there */
    if (!fGmc || !fGmc->mmhub_enabled || !fSdma || !fSdma->ready ||
        fGmc->gart_size < (uint64_t)winPages * 4096)
        return RDNA3_ENODEV;
    return rdna3_copy_vram_to_pages(fDev, fGmc, fSdma, vramOff, pages, npages, len,
                                    fGmc->gart_size - (uint64_t)winPages * 4096, winPages);
}

bool RDNA3Bringup::vmLive(void *p) const
{
    return p && ((const RDNA3Vm *)p)->live && fVmReady;
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

/* the VMID0 (MC) address the ring writes a user fence to */
int RDNA3Bringup::ufAddr(const rdna3_cs_job *job, uint64_t *mc, int *slot)
{
    const rdna3_bo *bo = job->uf_bo;
    const uint64_t ufOff = job->uf_offset;
    *slot = -1;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        *mc = rdna3_vram_mc(fDev, bo->vram_offset + ufOff);
        return 0;
    }
    const RDNA3Gtt *gt = (const RDNA3Gtt *)bo->backing;
    const uint64_t winPages = 4096;   /* copyVramToPages' window, above the slots */
    if (!gt || !gt->npages || !fGmc || !fGmc->mmhub_enabled ||
        fGmc->gart_size < (winPages + kUfSlots) * 4096)
        return -RD_ENODEV;
    uint64_t pa = gt->pages[0];
    uint64_t base = fGmc->gart_size - (winPages + kUfSlots) * 4096;
    int idle = -1;
    for (uint32_t i = 0; i < kUfSlots; i++) {
        if (fUfSlot[i].used && fUfSlot[i].pa == pa) {   /* already mapped there */
            *slot = (int)i;
            *mc = fGmc->gart_start + base + i * 4096ull + ufOff;
            return 0;
        }
        if (idle < 0 && (!fUfSlot[i].used || fenceSignaled(fUfSlot[i].ip, 0, fUfSlot[i].seq)))
            idle = (int)i;
    }
    if (idle < 0)
        return -RD_EBUSY;
    const uint64_t flags = RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                           RD_PTE_WRITEABLE | RD_PTE_MTYPE_NV10(RD_MTYPE_UC);
    if (gmc11_gart_map(fDev, fGmc, base + idle * 4096ull, pa, 1, flags) != RDNA3_OK)
        return -RD_ENOMEM;
    vmFlush(0);   /* both hubs: SDMA through the MMHUB, CP through the GFXHUB */
    fUfSlot[idle].pa = pa;
    fUfSlot[idle].used = true;
    fUfSlot[idle].seq = 0;
    *slot = idle;
    *mc = fGmc->gart_start + base + idle * 4096ull + ufOff;
    return 0;
}

int RDNA3Bringup::submit(void *p, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                         const rdna3_cs_job *job, uint64_t *seq)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    uint64_t ufMc = 0;
    int ufSlot = -1;
    if (!v || !v->live || ring != 0)
        return -RD_EINVAL;   /* record-only VM: the IB VA means nothing to the GPU */
    if (v->guilty)
        return -RD_ECANCELED;   /* amdgpu_cs_submit on a guilty context */
    fLastSubmit = mach_absolute_time();
    if (fIdleCapped)
        idleCapLift();   /* full clocks before the work reaches the ring */
    if (job->uf_bo) {
        int r = ufAddr(job, &ufMc, &ufSlot);
        if (r)
            return r;
    }
    int r = submitRing(v, ip, ibs, n, job, ufMc, seq);
    if (!r && ufSlot >= 0) {
        fUfSlot[ufSlot].ip = ip;
        fUfSlot[ufSlot].seq = *seq;
    }
    return r;
}

int RDNA3Bringup::submitRing(RDNA3Vm *v, uint32_t ip, const rdna3_ib *ibs, uint32_t n,
                             const rdna3_cs_job *job, uint64_t ufMc, uint64_t *seq)
{
    /* GFX and compute: user fence, and the GDS switch when the VMID's
     * partition is not the job's (amdgpu_vm_flush gds_switch_needed); the
     * DMA ring has no GDS */
    rdna3_gfx_job_ext x = {};
    const uint32_t want[6] = { job->gds_base, job->gds_size, job->gws_base, job->gws_size,
                               job->oa_base, job->oa_size };
    x.uf_addr = ufMc;
    x.gds_switch = !fGdsKnown[v->vmid] || memcmp(fGds[v->vmid], want, sizeof(want)) != 0;
    x.gds_base = want[0]; x.gds_size = want[1];
    x.gws_base = want[2]; x.gws_size = want[3];
    x.oa_base = want[4]; x.oa_size = want[5];
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ready) {
        for (uint32_t i = 0; i < n; i++)
            sdma60_emit_ib(fDev, fSdma, v->vmid, ibs[i].va, ibs[i].length_dw);
        fSdmaLastVmid = v->vmid;
        if (ufMc)   /* amdgpu_ib_schedule: the user fence, then the ring fence */
            sdma60_emit_fence64(fDev, fSdma, ufMc, (uint32_t)(fSdma->fence_seq + 1));
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
        uint32_t s = gfx11_submit_job_ex(fDev, fGfx, (uint64_t)(uintptr_t)v, v->vmid,
                                         v->gfxFlush ? gpuvm_pd_addr(fDev, &v->vm) : 0, g, n,
                                         &x);
        if (!s)
            return -RD_EBUSY;
        memcpy(fGds[v->vmid], want, sizeof(want));
        fGdsKnown[v->vmid] = true;
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
        uint32_t s = gfx11_submit_compute_job_ex(fDev, fKcq, (uint64_t)(uintptr_t)v, v->vmid,
                                                 v->compFlush ? gpuvm_pd_addr(fDev, &v->vm) : 0,
                                                 g, n, &x);
        if (!s)
            return -RD_EBUSY;
        memcpy(fGds[v->vmid], want, sizeof(want));
        fGdsKnown[v->vmid] = true;
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

int RDNA3Bringup::fenceWait(uint32_t ip, uint32_t ring, uint64_t seq, uint64_t absNs,
                            bool *capHit)
{
    /* userspace deadlines are CLOCK_MONOTONIC, which on macOS counts
     * mach_continuous_time. Every wait is capped at 10 s (amdgpu's job
     * timeout). With MSI the wait sleeps on fLock until the IH handler wakes
     * it (5 ms guard against a lost interrupt); without it the ring is
     * polled. The DRM ioctl path calls this with fDrmLock dropped (capHit
     * set), so other clients keep going while one waits. */
    if (capHit)
        *capHit = false;
    uint64_t start, now, capNs = 10ull * 1000 * 1000 * 1000;
    absolutetime_to_nanoseconds(mach_continuous_time(), &start);
    for (uint32_t spins = 0;; spins++) {
        if (fenceSignaled(ip, ring, seq))
            return 0;
        absolutetime_to_nanoseconds(mach_continuous_time(), &now);
        if (absNs != UINT64_MAX && now >= absNs)
            return -RD_ETIME;
        if (now - start >= capNs) {
            if (capHit) {
                *capHit = true;          /* the caller recovers under its lock */
                return -RD_ETIME;
            }
            return recover(ip, ring, seq);   /* amdgpu_job_timedout */
        }
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
            /* polled IH: page faults and other IVs are only seen here */
            if (fIh && fIh->enabled && IOLockTryLock(fIhLock)) {
                ih60_process(fDev, fIh, ivCallback, this);
                IOLockUnlock(fIhLock);
            }
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
    if (gmc11_iv_is_vm_fault(iv)) {
        /* gmc_v11_0_process_interrupt: no retry faults on a dGPU, the wave
         * is gone; the VM that owns the VMID is guilty from now on */
        rdna3_vm_fault f;
        gmc11_decode_vm_fault(b->fDev, iv, &f);
        gmc11_log_vm_fault(b->fDev, &f);
        b->fVmFaults++;
        if (f.vmid < 16 && b->fVmByVmid[f.vmid]) {
            b->fVmByVmid[f.vmid]->faultAddr = f.addr;
            b->fVmByVmid[f.vmid]->faultStatus = f.status;
            b->fVmByVmid[f.vmid]->faultHub = f.hub ? AMDGPU_VMHUB_TYPE_MM0 : AMDGPU_VMHUB_TYPE_GFX;
        }
        b->markGuilty(f.vmid, f.hub ? "MMHUB page fault" : "GFXHUB page fault");
        b->publish("RDNA3,VMFaults", b->fVmFaults);
        b->publish("RDNA3,LastFaultAddr", f.addr);
        b->publish("RDNA3,LastFaultVMID", f.vmid);
        b->publish("RDNA3,LastFaultStatus", f.status);
        return;
    }
    {
        /* DCN hotplug (amdgpu_dm handle_hpd_irq): ack and re-arm here, the
         * re-probe runs on the display side's thread call */
        int rx = 0, pin = dchpd_iv_pin(iv->client_id, iv->src_id, iv->src_data[0], &rx);
        if (pin >= 0) {
            if (!rx && pin < RD_AUX_ENGINES)
                b->fOwner->displayHotplug((uint32_t)pin, dchpd_irq_ack(b->fDev, (uint32_t)pin));
            return;
        }
    }
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
    IOLockLock(fIhLock);
    uint32_t n = ih60_process(fDev, fIh, ivCallback, this);
    IOLockUnlock(fIhLock);
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
    if (fHpdIrq && fDev)
        for (uint32_t n = 0; n < RD_AUX_ENGINES; n++)
            dchpd_irq_set(fDev, n, 0);
    fHpdIrq = false;
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
