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
#include <sys/proc.h>
#include <sys/kauth.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/thread_call.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "rdna3_bootargs.h"
#include "RDNA3Trace.h"

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
        dmub = rd3_arg("rdna3dmub", dmub);
        fDmubMode = dmub;
        msi = rd3_arg("rdna3msi", msi);
        PE_parse_boot_argn("rdna3doorbell", &db, sizeof(db));
        pm = rd3_arg("rdna3pm", pm);
        PE_parse_boot_argn("rdna3pmmask", &pmmask, sizeof(pmmask));
        PE_parse_boot_argn("rdna3mes", &mes, sizeof(mes));
        fMesWanted = mes != 0;
        /* stage 4 item J: amdgpu's exact sequence where the verified
         * profile differs; off by default (ladder step amdgpu) */
        fAmdgpuExact = rd3_arg("rdna3amdgpu", 0) != 0;
        {
            /* user queues (amdgpu.user_queue=1), off by default like
             * amdgpu's: they need the MES scheduler, so rdna3userq=1 brings
             * it up too, and VMIDs 8-15 then belong to the MES */
            uint32_t uq = 0;
            PE_parse_boot_argn("rdna3userq", &uq, sizeof(uq));
            fUserqWanted = uq != 0;
            if (fUserqWanted)
                fMesWanted = true;
        }
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
            uint32_t idlecap = rd3_arg("rdna3idlecap", 0);
            fIdleCapWanted = idlecap != 0;
            /* PM control from userspace: off unless asked for (not in
             * the default profile); rdna3baco=1 only with rdna3pmctl */
            fPmCtl = rd3_arg("rdna3pmctl", 0);
            fBacoWanted = rd3_arg("rdna3baco", 0) != 0;
            /* VCN 4.0 + JPEG 4.0 (stage 4 item G): opt-in, not part of the
             * verified profile; rdna3vcn=1 also loads VCN1's firmware.
             * =1 static power gating, =2 dynamic (DPG, as amdgpu on this
             * card), =3 DPG without the PSP's SRAM load (item I) */
            uint32_t vcn = 0;
            PE_parse_boot_argn("rdna3vcn", &vcn, sizeof(vcn));
            fVcnWanted = vcn != 0;
            fVcnMode = vcn > 3 ? 1 : vcn;
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
    if (fSmuIrqCall) {
        fSmuIrqStop = true;
        rd3_thread_call_drain_free(fSmuIrqCall);
        fSmuIrqCall = nullptr;
    }

    /* 2. the engines, in gfx_v11_0_hw_fini order, while fDev, the BAR and
     *    the doorbells are still mapped; with rdna3amdgpu=1
     *    amdgpu_device_fini_hw's amdgpu_fence_driver_hw_fini comes first */
    if (fAmdgpuExact)
        fenceDriverHwFini();
    /* amdgpu_device_ip_fini_early: GFXOFF disallowed, clock gating off
     * (UNGATE) first */
    if (fGfxOffCall) {
        IOLock *l = fOwner->drmLock();
        fGfxOffStop = true;
        rd3_thread_call_drain(fGfxOffCall);
        if (l)
            IOLockLock(l);
        gfxOffHold();
        if (l)
            IOLockUnlock(l);
        rd3_thread_call_drain_free(fGfxOffCall);
        fGfxOffCall = nullptr;
    }
    /* vcn_v4_0_hw_fini / jpeg_v4_0_hw_fini: gate the video block, the PMFW
     * powers it down */
    if (fVcnIdleCall) {
        fVcnStop = true;
        rd3_thread_call_drain_free(fVcnIdleCall);
        fVcnIdleCall = nullptr;
    }
    /* amdgpu_device_ip_fini_early / amdgpu_device_ip_fini: hw_fini, then
     * sw_fini, in reverse IP order (JPEG before VCN); sw_fini clears
     * fw_shared while the BAR is still mapped */
    if (fVcn && fDev && fVcn->sw_ready) {
        jpeg40_hw_fini(fDev, fVcn);
        vcn40_hw_fini(fDev, fVcn);
        jpeg40_sw_fini(fDev, fVcn);
        vcn40_sw_fini(fDev, fVcn);
    }
    if (fVcn)
        vcnFreeSaved();
    if (fCgWanted && fGfx && fGfx->ready && fDev)
        gfx11_set_clock_gating(fDev, 0, RD_GFX_CG_GC_11_0_3);
    if (fCgWanted && fSocCgWanted && fDev)
        soc21_set_clock_gating(fDev, 0);
    if (fKcq && fKcq->rb.ready && fDev)
        gfx11_set_eop_interrupt(fDev, &fKcq->rb, 0);
    /* gfx_v11_0_hw_fini: amdgpu_irq_put of the three CP fault sources
     * (rdna3amdgpu=1, which enabled them) */
    if (fAmdgpuExact && fGfx && fGfx->ready && fDev) {
        gfx11_set_priv_reg_fault_state(fDev, 0);
        gfx11_set_priv_inst_fault_state(fDev, 0);
        gfx11_set_bad_op_fault_state(fDev, 0);
    }
    if (fGfx && fGfx->async && fGfx->ready && fDev) {
        gfx11_set_eop_interrupt(fDev, fGfx, 0);
        kgqStop();   /* unmapped while the KIQ / the scheduler still run */
    }
    if (fKiq && fDev)
        gfx11_mec_stop(fDev, fKiq, fKcq, fMes);
    if (fGfx && fGfx->ready && fDev) {
        gfx11_set_eop_interrupt(fDev, fGfx, 0);
        gfx11_cp_gfx_enable(fDev, 0);
    }
    if (fSdma && fSdma->ready && fDev) {
        sdma60_set_trap_interrupt(fDev, fSdma, 0);
        if (fAmdgpuExact)   /* sdma_v6_0_hw_fini: every instance */
            sdma60_hw_fini(fDev, fDev->ipver[HWIP_SDMA1].present ? 2 : 1);
        else
            sdma60_stop(fDev, fSdma);
    }
    if (fIh && fIh->enabled && fDev)
        ih60_disable(fDev, fIh);
    ihSysFree();
    /* gfx_v11_0_hw_fini's gfxhub_v3_0_3_gart_disable with rdna3amdgpu=1 */
    if (fAmdgpuExact && fGfx && fDev && fGmc)
        gfxhub303_gart_disable(fDev);
    /* smu_hw_fini with rdna3amdgpu=1: the thermal alert off (no UNLOAD on
     * a card with display hardware) */
    if (fAmdgpuExact && fPmUp && fSmu && fDev)
        smu13_pm_fini(fDev, fSmu);
    /* psp_hw_fini with rdna3amdgpu=1 (TAs, ASD, DESTROY_TMR, ring
     * destroy); the verified default only stops the ring */
    if (fPsp && fPsp->ring_created) {
        if (fAmdgpuExact)
            psp13_hw_fini(fPsp);
        else
            psp13_ring_stop(fPsp);
    }
    /* gmc_v11_0_hw_fini with rdna3amdgpu=1: VM fault interrupts off, MMHUB
     * GART off */
    if (fAmdgpuExact && fGmc && fDev)
        gmc11_hw_fini(fDev);
    /* ... and soc21_common_hw_fini last (IP block 0): doorbell apertures off */
    if (fAmdgpuExact && fDev && fDev->doorbell_ok)
        soc21_common_hw_fini(fDev);

    /* 3. memory: mappings, firmware, state; fDev last. The firmware images
     * go like amdgpu_ucode_release in psp_sw_fini (sOS, TA),
     * smu_v13_0_fini_microcode (the SMC image among the blobs) and
     * gfx_v11_0_free_microcode (PFP, ME, RLC, MEC) */
    OSSafeReleaseNULL(fVram);
    OSSafeReleaseNULL(fDoorbell);
    for (unsigned i = 0; i < AMD_BLOB_COUNT; i++)
        if (fBlobs[i].data)
            IOFree(fBlobs[i].data, fBlobs[i].len);
    if (fSos.data)
        IOFree(fSos.data, fSos.len);
    if (fTa.data)
        IOFree(fTa.data, fTa.len);
    fTa.data = nullptr;
    if (fSmu) IOFree(fSmu, sizeof(*fSmu));
    if (fUserOdTable) IOFree(fUserOdTable, sizeof(*fUserOdTable));
    fUserOdTable = nullptr;
    if (fVcn) IOFree(fVcn, sizeof(*fVcn));
    fVcn = nullptr;
    if (fDmub) IOFree(fDmub, sizeof(*fDmub));
    fDmub = nullptr;
    if (fRom) IOFree(fRom, RD_VBIOS_MAX_LEN);
    fVmReady = false;
    vmFreeLeftovers();   /* before the table pool goes */
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

bool RDNA3Bringup::loadFirmware(const char *name, RDNA3Firmware *out, bool optional)
{
    /* the root volume may not be mounted yet early in boot: retry ~2 min */
    for (int attempt = 0; attempt < (optional ? 1 : 120) && !fAbort; attempt++) {
        if (!rootFsReady()) {
            IOSleep(1000);
            continue;
        }
        char path[256];
        for (unsigned d = 0; d < sizeof(kFwDirs) / sizeof(kFwDirs[0]); d++) {
            snprintf(path, sizeof(path), "%s%s", kFwDirs[d], name);
            if (readFileFromDisk(path, out)) {
                BLOG("firmware %s: %u bytes (%s)", name, out->len, path);
                /* rdna3amdgpu=1: amdgpu_ucode_request -> amdgpu_ucode_validate,
                 * the header's size_bytes must be the file's size */
                if (fAmdgpuExact && amd_ucode_validate((const uint8_t *)out->data, out->len) != AMD_FW_OK) {
                    BLOG("firmware %s: \"%s\" failed to validate", name, path);
                    IOFree(out->data, out->len);
                    out->data = nullptr;
                    out->len = 0;
                    return false;
                }
                return true;
            }
        }
        if (optional)
            break;
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

/* amdgpu_device_mode1_reset: bus mastering off, the PMFW's mode-1 reset,
 * the PCI config written back (amdgpu_device_load_pci_state) BEFORE
 * anything reads the BARs again, then MEMSIZE readable. The reset clears
 * the card's config space; reading MEMSIZE through a BAR the card no
 * longer decodes was the order here before, and a passthrough host does
 * not survive every such access (card run 40: the host hung in the
 * guest's second S3 suspend reset). Returns smu13_mode1_reset's result. */
int RDNA3Bringup::mode1Reset()
{
    static const uint8_t kSaved[] = { 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x3C };
    uint32_t cfg[sizeof(kSaved)], i;
    uint16_t cmd = fPci->configRead16(kIOPCIConfigCommand);
    for (i = 0; i < sizeof(kSaved); i++)
        cfg[i] = fPci->configRead32(kSaved[i]);
    fPci->setBusMasterEnable(false);
    int r = smu13_mode1_reset(fDev);
    for (i = 0; i < sizeof(kSaved); i++)
        fPci->configWrite32(kSaved[i], cfg[i]);
    fPci->configWrite16(kIOPCIConfigCommand, cmd);
    for (i = 0; i < fDev->usec_timeout; i++) {
        if (rdna3_rreg(fDev, mmRCC_CONFIG_MEMSIZE) != 0xffffffffu)
            break;
        IODelay(1);
    }
    return r;
}

/* Passthrough: a PCI bus reset after the host's amdgpu leaves the PSP with
 * neither sOS alive nor the bootloader ready (C2PMSG_35 reads 0), and every
 * bootloader command times out. The only way back is what amdgpu's GPU
 * recovery uses, a mode-1 reset through the PMFW (soc21_asic_mode1_reset:
 * bus mastering off, PCI config saved and restored around it).
 * rdna3reset=0: never, 1 (default): when stuck, 2: always. */
void RDNA3Bringup::resetIfPspStuck()
{
    uint32_t mode = 1;
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
    mode1Reset();
    ret = psp13_wait_for_bootloader(bl);
    psp13_log_state(bl, "after mode-1 reset");
    publish("RDNA3,InitResetResult", (uint64_t)(int64_t)ret);
    BLOG("PSP: mode-1 reset %s", ret == RDNA3_OK ? "done, bootloader ready" : "did not bring the bootloader back");
    IOFree(bl, sizeof(*bl));
}

/* amdgpu_device_init: "Post card if necessary". amdgpu_device_need_post
 * (BIOS scratch 7) -> amdgpu_device_asic_init for GC 11: wait for the PSP
 * bootloader (amdgpu_psp_wait_for_bootloader), then amdgpu_atomfirmware_asic_init. In a passthrough VM whose
 * UEFI did not run the card's option ROM the GPU is not posted: the FB
 * location reads 0. boot-arg rdna3post=0 skips it. */
bool RDNA3Bringup::postIfNeeded()
{
    static const uint32_t kScratchBytes = 20u * 1024u;   /* amdgpu's minimum FB scratch */
    uint32_t want = 1, *scratch = nullptr, scratchBytes = kScratchBytes;
    rdna3_atom *atom = nullptr;
    rdna3_psp *bl = nullptr;
    int ret, need;
    bool ok = true;

    PE_parse_boot_argn("rdna3post", &want, sizeof(want));
    if (!fRom)
        fRom = (uint8_t *)IOMalloc(RD_VBIOS_MAX_LEN);
    atom = (rdna3_atom *)IOMalloc(sizeof(*atom));
    bl = (rdna3_psp *)IOMalloc(sizeof(*bl));
    if (!fRom || !atom || !bl) {
        BLOG("post: no memory");
        ok = false;
        goto out;
    }
    bzero(bl, sizeof(*bl));
    bl->d = fDev;
    ret = vbios_read(fDev, &fVbios, fRom, RD_VBIOS_MAX_LEN);
    /* rdna3amdgpu=1: amdgpu_atomfirmware_allocate_fb_scratch sizes the
     * interpreter's FB scratch from vram_usagebyfirmware */
    if (ret == RDNA3_OK && fAmdgpuExact)
        scratchBytes = vbios_atom_scratch_bytes(&fVbios);
    scratch = (uint32_t *)IOMalloc(scratchBytes);
    if (!scratch) {
        BLOG("post: no memory");
        ok = false;
        goto out;
    }
    bzero(scratch, scratchBytes);
    if (ret == RDNA3_OK)
        ret = atom_init(atom, fDev, &fVbios, scratch, scratchBytes);
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
    if (scratch) IOFree(scratch, scratchBytes);
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
    fDev->amdgpu_exact = fAmdgpuExact ? 1 : 0;
    /* amdgpu_device_has_dc_support: the display side drives the DCN */
    fDev->amdgpu_dc = fDmubMode ? 1 : 0;
    publish("RDNA3,AmdgpuExact", fAmdgpuExact ? 1 : 0);
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
    if (fAmdgpuExact) {
        /* gmc_v11_0_sw_init -> amdgpu_gmc_get_vbios_allocations: the pre-OS
         * scanout amdgpu keeps (stolen VGA + extended) stays out as well */
        uint64_t vga = 0, ext = 0;
        gmc11_get_vbios_allocations(fDev, fVramSize, &vga, &ext);
        publish("RDNA3,VRAMStolenKB", (vga + ext) >> 10);
        if (vga + ext > lo)
            lo = (vga + ext + 0xFFFFF) & ~0xFFFFFull;
    }
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
    if (fAmdgpuExact) {
        /* amdgpu_ucode_ip_version_decode: the file names amdgpu would ask
         * for from the discovered IP versions, against the fixed set */
        static const struct { int block, hwip; } names[] = {
            { AMD_UCODE_BLOCK_GC, HWIP_GC },   { AMD_UCODE_BLOCK_SDMA, HWIP_SDMA0 }, { AMD_UCODE_BLOCK_MP0, HWIP_MP0 },
            { AMD_UCODE_BLOCK_MP1, HWIP_MP1 }, { AMD_UCODE_BLOCK_VCN, HWIP_VCN },
        };
        const char *files[5] = { amd_navi32_fw_names[AMD_BLOB_PFP], amd_navi32_fw_names[AMD_BLOB_SDMA],
                                 amd_navi32_sos_name, amd_navi32_fw_names[AMD_BLOB_SMC],
                                 amd_navi32_fw_names[AMD_BLOB_VCN] };
        uint32_t bad = 0;
        for (unsigned i = 0; i < 5; i++) {
            const rdna3_ipver *v = &fDev->ipver[names[i].hwip];
            char pre[32];
            int n;
            if (!v->present)
                continue;
            n = amd_ucode_ip_version_decode(names[i].block, v->major, v->minor, v->rev, pre, sizeof(pre));
            if (n <= 0 || strncmp(files[i], pre, (size_t)n) != 0 || (files[i][n] != '_' && files[i][n] != '.')) {
                BLOG("firmware: amdgpu would load %s* for this IP, the bundle has %s", pre, files[i]);
                bad |= 1u << i;
            }
        }
        publish("RDNA3,FirmwareNameMismatch", bad);
    }
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
    if (fVcnWanted) {
        /* amdgpu_vcn_setup_ucode: one VCN ucode per instance (VCN1 too) */
        uint32_t n = 0;
        for (uint32_t i = 0; i < RD_VCN_MAX_INST; i++)
            if (fDev->reg_base[HWIP_VCN][i][1])
                n = i + 1;
        fst = blobs[AMD_BLOB_VCN].data ? amd_vcn_setup_ucode(blobs, n, fPlan) : AMD_FW_E_MISSING;
        BLOG("VCN: %u instance(s) in discovery, VCN firmware plan: %s", n, amd_fw_status_str(fst));
    }
    BLOG("firmware plan: %u ucodes, %u KiB", fPlan->count, fPlan->total_aligned / 1024);
    /* psp_init_ta_microcode: with rdna3amdgpu=1 psp_load_fw starts ASD and
     * the HDCP / DTM TAs from the TA file (optional, as for amdgpu) */
    if (fAmdgpuExact && !fTa.data)
        loadFirmware(amd_navi32_ta_name, &fTa, true);
    for (unsigned i = 0; i < AMD_BLOB_COUNT; i++)
        fBlobRefs[i] = blobs[i];

    /* rdna3amdgpu=1: amdgpu_device_init's reset on init - the sOS already
     * runs (soc21_need_reset_on_init: the driver was not unloaded cleanly,
     * a kext reload): amdgpu_asic_reset, mode 1 on MP1 13.0.10 with display
     * hardware. After a cold boot the sOS is not up (Linux on the card does
     * no reset here). */
    if (fAmdgpuExact && soc21_need_reset_on_init(fDev)) {
        BLOG("reset on init: the PSP sOS already runs, mode-1 reset first");
        int r = mode1Reset();
        publish("RDNA3,ResetOnInit", (uint64_t)(int64_t)r);
        if (r)
            BLOG("asic reset on init failed (%d)", r);
    }

    if (!hwInit())
        return;

    /* AMDGPU_INFO_FW_VERSION (amdgpu_firmware_info): the headers' versions,
     * the PMFW's own (smu_cmn_get_smc_version) and the running MES pipes' */
    {
        uint32_t ver[32], feat[32], ta[RD_TA_TYPES] = {};
        amd_fw_versions(fBlobRefs, fPsp ? &fPsp->sos : nullptr, ver, feat);
        /* AMDGPU_INFO_FW_ASD / _TA: psp_init_ta_microcode's view of the TA
         * file (only its versions: the kext loads no TA). Optional - an
         * older install lacks the file and reports 0, as for a TA amdgpu
         * does not find. */
        {
            RDNA3Firmware taFw = {};
            amd_psp_ta t;
            if (loadFirmware(amd_navi32_ta_name, &taFw, true)) {
                amd_fw_status tst = amd_psp_ta_parse(taFw.data, taFw.len, &t);
                if (tst == AMD_FW_OK)
                    amd_fw_ta_versions(&t, ver, feat, ta);
                else
                    BLOG("firmware %s: %s", amd_navi32_ta_name, amd_fw_status_str(tst));
                IOFree(taFw.data, taFw.len);
            }
        }
        if (fSmuVersion)
            ver[AMDGPU_INFO_FW_SMC] = fSmuVersion;
        if (fMes && fMes->sched_version) {
            ver[AMDGPU_INFO_FW_MES] = fMes->sched_version & 0xfff;
            feat[AMDGPU_INFO_FW_MES] = (fMes->sched_version >> 12) & 0xfff;
        }
        if (fMes && fMes->kiq_version) {
            ver[AMDGPU_INFO_FW_MES_KIQ] = fMes->kiq_version & 0xfff;
            feat[AMDGPU_INFO_FW_MES_KIQ] = (fMes->kiq_version >> 12) & 0xfff;
        }
        fOwner->drmSetFirmware(ver, feat, ta);
        userqStart(ver);
        BLOG("firmware versions: ME 0x%x PFP 0x%x MEC 0x%x RLC 0x%x SDMA 0x%x SMC 0x%x SOS 0x%x MES 0x%x "
             "ASD 0x%x", ver[AMDGPU_INFO_FW_GFX_ME], ver[AMDGPU_INFO_FW_GFX_PFP], ver[AMDGPU_INFO_FW_GFX_MEC],
             ver[AMDGPU_INFO_FW_GFX_RLC], ver[AMDGPU_INFO_FW_SDMA], ver[AMDGPU_INFO_FW_SMC],
             ver[AMDGPU_INFO_FW_SOS], ver[AMDGPU_INFO_FW_MES], ver[AMDGPU_INFO_FW_ASD]);
    }

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
        if (fVcn) {
            publish("RDNA3,IRQFenceVCN", fIrqVcn);
            publish("RDNA3,IRQFenceJPEG", fIrqJpeg);
        }
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

    /* rdna3amdgpu=1: soc21_common_hw_init - ASPM (soc21_program_aspm when
     * amdgpu_device_should_use_aspm: amdgpu.aspm=-1, PCIe DPM on, the link's
     * ASPM enabled), then nbio_v4_3_init_registers */
    if (fAmdgpuExact) {
        UInt8 cap = 0;
        int linkAspm = 0, ltr = 0, did;
        const amd_ip_entry *pcie = amd_discovery_find(fDisc, AMD_HWID_PCIE, 0);
        if (fPci->findPCICapability(kIOPCIPCIExpressCapability, &cap) && cap) {
            linkAspm = (fPci->configRead16(cap + 0x10) & 0x3) != 0;   /* Link Control: ASPM L0s / L1 */
            ltr = (fPci->configRead16(cap + 0x28) & 0x400) != 0;      /* Device Control 2: LTR enable */
        }
        did = soc21_program_aspm(fDev, rd_should_use_aspm(-1, 1, linkAspm),
                                 pcie ? RD_IP_VERSION(pcie->major, pcie->minor, pcie->revision) : 0, ltr);
        publish("RDNA3,ASPMProgrammed", (uint64_t)did);
        nbio43_init_registers(fDev);
    }
    rdna3_nbio_remap_hdp(fDev);
    /* a known word in VRAM: after a reset it tells whether VRAM survived */
    fResetMagic = rdna3_vram_alloc(fDev, 4096, 4096, "reset-magic");
    if (fResetMagic != UINT64_MAX) {
        uint32_t m = kResetMagic;
        rdna3_vram_write(fDev, fResetMagic, &m, 4);
    }

    /* PSP */
    ret = psp13_init(fPsp, fDev, fSos.data, fSos.len);
    if (!ret && fTa.data && psp13_set_ta(fPsp, fTa.data, fTa.len) != RDNA3_OK)
        BLOG("PSP: TA file unusable: no ASD / HDCP / DTM");
    /* psp_hw_init -> psp_load_fw; after a reset or a wake hwInit runs this
     * again as amdgpu_device_ip_resume -> psp_resume: psp_hw_start, the
     * non-PSP firmware, ASD, RL and the TAs (the KM ring is allocated anew,
     * the VRAM pool starts over) */
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
        fSmuVersion = smuv;
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
                fOwner->displayMirror(fDmub, true, fReinit);   /* rdna3mirror=1: step 9b */
                /* rdna3mall=1 (step mall): display power, item O. The
                 * PMFW fetches the watermark range table from VRAM
                 * (dcn32_notify_wm_ranges) */
                if (rd3_arg("rdna3mall", 0)) {
                    uint64_t wm = rdna3_vram_alloc(fDev, 4096, 4096, "dal-wm-table");
                    if (wm != UINT64_MAX)
                        fOwner->displayPowerInit(fDev, fDmub, wm);
                    else
                        BLOG("display power: no VRAM for the watermark range table");
                }
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

        /* IH 6.0 ring 0, polled (no MSI registration yet): ih_v6_0_hw_init,
         * which ih_v6_0_resume is after a reset or a wake */
        if (!fIh)
            fIh = (rdna3_ih *)IOMalloc(sizeof(*fIh));
        if (!fIh) {
            publishString("RDNA3,Stage2", "failed: IH (no memory)");
            return false;
        }
        bzero(fIh, sizeof(*fIh));
        if (fAmdgpuExact && ihSysAlloc()) {
            /* ih_v6_0 as amdgpu: GTT rings, snooped, rptr doorbells, ring 1 */
            const uint64_t ringPages = RD_IH_RING_SIZE / 4096 + 1;
            ret = ih60_init_amdgpu(fDev, fIh, fIh1, fIhSys[0].md->getBytesNoCopy(), fGmc->gart_start,
                                   fIhSys[1].md->getBytesNoCopy(), fGmc->gart_start + ringPages * 4096,
                                   RD_IH_RING_SIZE, fGmc->dummy_page_pa, fMsi ? 1 : 0);
            publish("RDNA3,IHSystemMemory", ret ? 0 : 1);
        } else {
            ret = ih60_init(fDev, fIh, 0, fGmc->dummy_page_pa, fMsi ? 1 : 0);
        }
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
                /* the accelerator's VM first, before any DRM client can
                 * take the VMIDs (WindowServer's IOSurface maps need it) */
                fOwner->platformVmReady();
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
        if (!ret && fAmdgpuExact) {
            /* rdna3amdgpu=1: the DMA ring as amdgpu sets it up - padded to
             * align_mask 0xf with burst NOPs when the firmware's feature
             * version is >= 20 (amdgpu_sdma_init_microcode), and the VM
             * invalidation engine amdgpu_gmc_allocate_vm_inv_eng gives sdma0
             * (the same 15-ring table as the GFX stage uses) */
            static const uint32_t hubs[15] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0 };
            uint32_t ver[32], feat[32], eng[15];
            amd_fw_versions(fBlobRefs, nullptr, ver, feat);
            fSdma->amdgpu_pad = 1;
            fSdma->burst_nop = feat[AMDGPU_INFO_FW_SDMA] >= 20;
            if (gmc11_allocate_vm_inv_eng(hubs, nullptr, 15, fMesWanted, eng) == RDNA3_OK)
                fSdma->vm_inv_eng = eng[9];
            publish("RDNA3,SDMAVmInvEng", fSdma->vm_inv_eng);
            publish("RDNA3,SDMABurstNop", (uint64_t)fSdma->burst_nop);
        }
        if (!ret)
            ret = sdma60_start(fDev, fSdma, 0, 0);
        publish("RDNA3,SDMAResult", (uint64_t)(int64_t)ret);
        if (!ret && fAmdgpuExact) {
            /* amdgpu_ib_ring_tests -> sdma_v6_0_ring_test_ib; a ring whose
             * IB test fails is not used */
            if (fIbTestBuf == UINT64_MAX)
                fIbTestBuf = rdna3_vram_alloc(fDev, 4096, 4096, "ib-test");
            ret = sdma60_ring_test_ib(fDev, fSdma, fIbTestBuf);
            publish("RDNA3,IBTestSDMA", (uint64_t)(int64_t)ret);
            if (ret) {
                BLOG("SDMA: IB test failed (%d): the ring is not used", ret);
                fSdma->ready = 0;
            }
        }
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
        /* rdna3amdgpu=1: gfx_v11_0_early_init / sw_init's queue topology
         * (amdgpu.num_kcq, async_gfx_ring, compute_multipipe defaults), the
         * rings padded to their fetch size (amdgpu_ring_commit) and
         * gfx_v11_0_rlc_init's update_spm_vmid(0xf) */
        if (fAmdgpuExact) {
            const rdna3_ipver *gcv = &fDev->ipver[HWIP_GC];
            int tr = gfx11_sw_init_topo(&fGfxTopo, gcv->major, gcv->minor, gcv->rev, -1, 1, -1);
            if (tr == RDNA3_OK) {
                char names[128];
                uint32_t n = 0;
                for (uint32_t c = 0; c < fGfxTopo.num_compute_rings && n + 16 < sizeof(names); c++) {
                    for (const char *q = fGfxTopo.compute[c].name; *q; q++)
                        names[n++] = *q;
                    names[n++] = ' ';
                }
                names[n ? n - 1 : 0] = 0;
                publishString("RDNA3,ComputeRings", names);
                publish("RDNA3,MaxHwContexts", fGfxTopo.max_hw_contexts);
            }
            fGfx->amdgpu_pad = 1;
            gfx11_update_spm_vmid(fDev, 0xf);
        }
        /* gfx_v11_0_check_fw_cp_gfx_shadow on the CP firmware headers (the
         * AMDGPU_INFO_FW_GFX_* versions the UAPI answers DEV_INFO from); not
         * a VF, so off - as amdgpu leaves it on bare metal */
        {
            uint32_t fv[32], ff[32];
            const rdna3_ipver *gc = &fDev->ipver[HWIP_GC];
            amd_fw_versions(fBlobRefs, nullptr, fv, ff);
            fGfx->cp_gfx_shadow = amd_gfx11_check_fw_cp_gfx_shadow(
                AMD_IP_VERSION(gc->major, gc->minor, gc->rev), fv[AMDGPU_INFO_FW_GFX_ME],
                fv[AMDGPU_INFO_FW_GFX_PFP], fv[AMDGPU_INFO_FW_GFX_MEC], /* amdgpu_sriov_vf */ 0);
        }
        ret = gfx11_rs64_from_fw(fBlobs[AMD_BLOB_PFP].data, fBlobs[AMD_BLOB_PFP].len,
                                 fBlobs[AMD_BLOB_ME].data, fBlobs[AMD_BLOB_ME].len,
                                 fBlobs[AMD_BLOB_MEC].data, fBlobs[AMD_BLOB_MEC].len, &rs64);
        /* gfx_v11_0_hw_init; after a reset or a wake this is
         * gfx_v11_0_resume, which is gfx_v11_0_hw_init again */
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
                uint64_t mesFwStart[2];
                if (gfx11_mes_from_fw(fBlobs[AMD_BLOB_MES].data, fBlobs[AMD_BLOB_MES].len,
                                      fBlobs[AMD_BLOB_MES1].data, fBlobs[AMD_BLOB_MES1].len,
                                      mesFwStart) == RDNA3_OK) {
                    kr = gfx11_mes_kiq_ring_init(fDev, fKiq, mesFwStart);
                } else {
                    BLOG("MES firmware headers unusable: MEC KIQ (scheduling will not work)");
                    kr = gfx11_mec_ring_init(fDev, fKiq, 1, RD_KIQ_PIPE, 0, RD_DB_KIQ, 0);
                }
                publish("RDNA3,KIQOnMES", fKiq->me == 3 ? 1 : 0);
                if (!kr)
                    kr = gfx11_mec_ring_init(fDev, fKcq, 0, RD_KCQ0_PIPE, 0, RD_DB_MEC_RING0, 0);
                if (!kr && fAmdgpuExact) {
                    /* our KCQ is amdgpu's compute_ring[0] (comp_1.0.0): high
                     * priority with eight KCQs (amdgpu_ring_to_mqd_prop) */
                    fKcq->high_prio = fGfxTopo.num_compute_rings &&
                                      fGfxTopo.compute[0].hw_prio == RD_GFX_PIPE_PRIO_HIGH;
                    fKcq->rb.amdgpu_pad = 1;
                    fKiq->rb.amdgpu_pad = 1;
                }
                if (!kr)
                    kr = gfx11_kiq_start(fDev, fKiq);
                publish("RDNA3,KIQResult", (uint64_t)(int64_t)kr);
                /* rdna3amdgpu=1: mes_v11_0_kiq_hw_init starts the scheduler
                 * right after the KIQ when its firmware maps the kernel
                 * queues (enable_legacy_queue_map, MES 0x47 on); the KCQ
                 * and the gfx ring are then mapped by it */
                if (!kr && fAmdgpuExact && fMesWanted && fKiq->me == 3 &&
                    (gfx11_mes_fw_sched_version(fDev) & RD_MES_VERSION_MASK) >= RD_MES_LEGACY_MAP_VER)
                    mesStart("with the KIQ (legacy queue map)");
                bool mesMaps = !kr && fAmdgpuExact && fMes && fMes->ready && fMes->legacy_queue_map;
                publish("RDNA3,KernelQueuesByMES", mesMaps ? 1 : 0);
                if (!kr && mesMaps) {
                    kr = gfx11_kcq_start_mes(fDev, fKiq, fKcq, fMes);
                    if (kr) {
                        /* not amdgpu, but the path verified on the card:
                         * the queue off the scheduler, the KIQ maps it */
                        BLOG("KCQ: the MES scheduler did not map it (%d): the KIQ maps it instead", kr);
                        (void)gfx11_kcq_unmap_mes(fDev, fMes, fKcq);
                        publish("RDNA3,KernelQueuesByMESFallback", 1);
                        kr = gfx11_kcq_start(fDev, fKiq, fKcq);
                    }
                } else if (!kr) {
                    kr = gfx11_kcq_start(fDev, fKiq, fKcq);
                }
                publish("RDNA3,KCQResult", (uint64_t)(int64_t)kr);
                if (!kr && fAmdgpuExact) {
                    /* gmc_v11_0_late_init -> amdgpu_gmc_allocate_vm_inv_eng over
                     * amdgpu's rings on this card: gfx_0.0.0, comp_1.x.y (8),
                     * sdma0/1, vcn_unified_0/1, jpeg_dec, mes_kiq; the first
                     * compute ring (ours) gets engine 1, gfx keeps 0 */
                    static const uint32_t hubs[15] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0 };
                    uint32_t eng[15];
                    if (gmc11_allocate_vm_inv_eng(hubs, nullptr, 15, fMesWanted, eng) == RDNA3_OK) {
                        fGfx->vm_inv_eng = eng[0];
                        fKcq->rb.vm_inv_eng = eng[1];
                        publish("RDNA3,KCQVmInvEng", eng[1]);
                    }
                }
                if (!kr && fIrqLive)
                    gfx11_set_eop_interrupt(fDev, &fKcq->rb, 1);
                if (kr)
                    BLOG("compute queues unavailable (%d): GFX and DMA rings only", kr);
            }
            /* gfx_v11_0_cp_async_gfx_ring_resume, after the KCQs as in
             * gfx_v11_0_cp_resume */
            if (fGfx->async) {
                ret = RDNA3_ENODEV;
                if (fAmdgpuExact && fMes && fMes->ready && fMes->legacy_queue_map) {
                    ret = gfx11_kgq_start_mes(fDev, fMes, fGfx);   /* amdgpu_gfx_enable_kgq */
                    if (ret) {
                        BLOG("GFX: the MES scheduler did not map ring 0 (%d): the KIQ maps it instead", ret);
                        (void)gfx11_kgq_unmap_mes(fDev, fMes, fGfx, 1 /* RESET_QUEUES */);
                        publish("RDNA3,KernelQueuesByMESFallback", 1);
                    }
                }
                if (ret)
                    ret = (fKiq && fKiq->rb.ready) ? gfx11_kgq_start(fDev, fKiq, fGfx) : RDNA3_ENODEV;
            }
        }
        if (!ret && !fGfx->ready)
            ret = RDNA3_ENODEV;   /* async queue never mapped (no KIQ) */
        publish("RDNA3,GFXResult", (uint64_t)(int64_t)ret);
        publish("RDNA3,GFXAsync", fGfx->async ? 1 : 0);
        /* mes_v11_0_hw_init comes after GFX in soc21's IP order: the
         * kernel queues are mapped by the KIQ already (step 8c, opt-in) */
        if (!ret && fMesWanted && fKiq && fKiq->rb.ready && fKiq->me == 3 && !(fMes && fMes->ready))
            mesStart("level 8");
        /* amdgpu_device_ip_late_init -> amdgpu_device_set_cg_state(GATE) */
        if (!ret && fCgWanted)
            cgStart("level 8");
        /* ... and amdgpu_device_set_pg_state(GATE) ->
         * gfx_v11_0_set_powergating_state (GC 11.0.3) -> amdgpu_gfx_off_ctrl:
         * the bring-up's own hold goes, GFXOFF is allowed 100 ms later */
        if (!ret && fGfxOffWanted && fPmUp && fSmu) {
            fGfxOffCall = thread_call_allocate((thread_call_func_t)gfxOffTick, this);
            if (fGfxOffCall) {
                RD3DrmScope lk(fOwner);
                gfxOffRelease();
            }
        }
        /* the gfx ring's fence interrupt (amdgpu_fence_driver_hw_init's
         * amdgpu_irq_get per ring, after the IP blocks are up) */
        if (!ret && fIrqLive)
            gfx11_set_eop_interrupt(fDev, fGfx, 1);
        if (!ret && fAmdgpuExact) {
            /* amdgpu_device_ip_late_init -> gfx_v11_0_late_init: the CP's
             * privileged register / instruction and bad opcode interrupts */
            if (fIrqLive) {
                gfx11_late_init(fDev);
                publish("RDNA3,CPFaultIrqs", 1);
            }
            /* amdgpu_ib_ring_tests -> gfx_v11_0_ring_test_ib on the gfx ring
             * and the KCQ; a ring whose IB test fails is not used */
            if (fIbTestBuf == UINT64_MAX)
                fIbTestBuf = rdna3_vram_alloc(fDev, 4096, 4096, "ib-test");
            int ib = gfx11_ring_test_ib(fDev, fGfx, fIbTestBuf);
            publish("RDNA3,IBTestGFX", (uint64_t)(int64_t)ib);
            if (ib) {
                BLOG("GFX: IB test failed (%d): the ring is not used", ib);
                fGfx->ready = 0;
                ret = ib;
            }
            if (fKcq && fKcq->rb.ready) {
                ib = gfx11_ring_test_ib(fDev, &fKcq->rb, fIbTestBuf);
                publish("RDNA3,IBTestCOMP", (uint64_t)(int64_t)ib);
                if (ib) {
                    BLOG("KCQ: IB test failed (%d): the ring is not used", ib);
                    fKcq->rb.ready = 0;
                }
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

    /* stage 4 item G: VCN 4.0 + JPEG 4.0 after GFX, as in soc21's IP order */
    if (fLevel >= 8 && fVcnWanted)
        vcnStart();

    /* rdna3amdgpu=1: soc21_common_late_init - the selfring doorbell aperture */
    if (fAmdgpuExact && fDev->doorbell_ok)
        soc21_common_late_init(fDev);

    publish("RDNA3,GPUResets", fGpuResets);
    publish("RDNA3,VRAMLost", fVramLostCount);
    return true;
}

/* ------------------------- VCN 4.0 / JPEG 4.0 ------------------------- */

/* vcn_v4_0 / jpeg_v4_0 early, sw and hw init, then the IB tests
 * (amdgpu_ib_ring_tests: a ring whose IB test fails is not used). Runs at
 * the end of hwInit; again after a GPU reset or a wake (VRAM blocks are
 * kept), then as amdgpu_device_ip_resume: vcn_v4_0_resume (the VCPU BOs
 * back from vcnSuspend's copy) and jpeg_v4_0_resume, VCN before JPEG,
 * both after GFX. */
void RDNA3Bringup::vcnStart()
{
    static const char *const modeName[] = { "static PG", "DPG, SRAM through the PSP", "DPG, SRAM through the LMA" };
    int r, rj = RDNA3_ENODEV;
    uint32_t ready = 0, i, gb = 0;
    uint64_t tmr[RD_VCN_MAX_INST];
    int pgMode = fVcnMode == 2 ? RD_VCN_PG_DPG : fVcnMode == 3 ? RD_VCN_PG_DPG_DIRECT : RD_VCN_PG_STATIC;
    bool resume;

    publishString("RDNA3,VCN", "starting");
    if (!fGmc || !fGmc->mmhub_enabled || !fIh || !fVmReady) {
        BLOG("VCN: needs the GMC, IH and the VM pool (level 6+)");
        publishString("RDNA3,VCN", "skipped: no GMC/IH");
        return;
    }
    if (!fVcn) {
        fVcn = (rdna3_vcn *)IOMalloc(sizeof(*fVcn));
        if (!fVcn) {
            publishString("RDNA3,VCN", "failed: no memory");
            return;
        }
        bzero(fVcn, sizeof(*fVcn));
    }
    resume = fVcn->sw_ready != 0;
    /* adev->gfx.config.gb_addr_config (GC 11.0.3 regGB_ADDR_CONFIG) */
    if (fDev->reg_base[HWIP_GC][0][0])
        gb = rdna3_rreg(fDev, fDev->reg_base[HWIP_GC][0][0] + 0x13de);
    tmr[0] = fPsp->ucode_tmr_mc[AMD_UCODE_VCN];
    tmr[1] = fPsp->ucode_tmr_mc[AMD_UCODE_VCN1];
    r = vcn40_early_init(fDev, fVcn, fBlobs[AMD_BLOB_VCN].data, fBlobs[AMD_BLOB_VCN].len, gb, fPmUp ? 1 : 0, pgMode);
    fVcn->psp = fPsp;   /* amdgpu_vcn_psp_update_sram */
    publish("RDNA3,VCNFirmware", fVcn->fw_version);
    publish("RDNA3,VCNInstances", fVcn->num_vcn_inst);
    publish("RDNA3,VCNTmrMC0", tmr[0]);
    publish("RDNA3,VCNTmrMC1", tmr[1]);
    publishString("RDNA3,VCNPowerGating", modeName[pgMode]);
    if (!r && (!tmr[0] || (fVcn->num_vcn_inst > 1 && !tmr[1]))) {
        BLOG("VCN: the PSP did not load vcn_4_0_0.bin for every instance (TMR 0x%llx / 0x%llx)",
             (unsigned long long)tmr[0], (unsigned long long)tmr[1]);
        r = RDNA3_ENOENT;
    }
    if (resume) {
        /* vcn_v4_0_resume: where the PSP put the images this time */
        for (i = 0; i < RD_VCN_MAX_INST; i++)
            fVcn->tmr_mc[i] = tmr[i];
        if (!r)
            r = vcn40_resume(fDev, fVcn);
        BLOG("VCN: resume (%s): %d, %u VCPU BO(s) restored so far", modeName[pgMode], r, fVcn->restored_bos);
    } else {
        if (!r)
            r = jpeg40_early_init(fDev, fVcn);
        if (!r)
            r = vcn40_sw_init(fDev, fVcn, tmr);
        if (!r)
            r = jpeg40_sw_init(fDev, fVcn);
        if (!r)
            r = vcn40_hw_init(fDev, fVcn);
    }
    vcnFreeSaved();   /* amdgpu_vcn_resume freed its copy */
    fVcnSuspended = false;
    publish("RDNA3,VCNResult", (uint64_t)(int64_t)r);
    if (!r) {
        for (i = 0; i < fVcn->num_vcn_inst; i++) {
            int t = vcn40_unified_ring_test_ib(fDev, fVcn, &fVcn->ring_enc[i]);
            publish(i ? "RDNA3,VCN1IBTest" : "RDNA3,VCN0IBTest", (uint64_t)(int64_t)t);
            if (t)
                fVcn->ring_enc[i].ready = 0;
            else
                ready |= 1u << i;
        }
        rj = resume ? jpeg40_resume(fDev, fVcn) : jpeg40_hw_init(fDev, fVcn);
        if (!rj) {
            rj = jpeg40_dec_ring_test_ib(fDev, fVcn);
            if (rj)
                fVcn->jpeg.ready = 0;
        }
    }
    publish("RDNA3,JPEGResult", (uint64_t)(int64_t)rj);
    publish("RDNA3,VCNStarts", fVcn->starts);
    publish("RDNA3,VCNDPGStarts", fVcn->dpg_starts);
    publish("RDNA3,VCNSRAMUploads", fVcn->sram_uploads);
    publish("RDNA3,VCNSRAMUploadErrors", fVcn->sram_upload_errs);
    publish("RDNA3,VCNResumes", fVcn->resumes);
    BLOG("VCN: firmware 0x%08x, %u instance(s), %s, unified rings ready 0x%x, JPEG %s (%d / %d)", fVcn->fw_version,
         fVcn->num_vcn_inst, modeName[pgMode], ready, rj ? "off" : "ready", r, rj);
    if (ready || !rj) {
        const rdna3_ipver *vv = &fDev->ipver[HWIP_VCN];
        fOwner->drmVcnReady(AMD_IP_VERSION(vv->major, vv->minor, vv->rev), fVcn->num_vcn_inst,
                            fVcn->harvest_config, ready, !rj);
        publishString("RDNA3,VCN", ready == 3 && !rj ? "VCN0 VCN1 JPEG up" : "partly up");
    } else {
        publishString("RDNA3,VCN", "failed");
    }
    if (!fVcnIdleCall)
        fVcnIdleCall = thread_call_allocate((thread_call_func_t)vcnIdleTick, this);
    vcnIdleSchedule();   /* amdgpu_vcn_ring_end_use after the tests: gate in 1 s */
}

/* the VCPU BO copies amdgpu_vcn_resume / amdgpu_vcn_sw_fini are done with */
void RDNA3Bringup::vcnFreeSaved()
{
    uint64_t size = vcn40_vcpu_bo_size();
    for (uint32_t i = 0; i < RD_VCN_MAX_INST; i++)
        if (fVcn->saved_bo[i] && !fVcn->saved_bo_valid[i]) {
            IOFree(fVcn->saved_bo[i], (vm_size_t)size);
            fVcn->saved_bo[i] = nullptr;
        }
}

/* amdgpu_device_ip_suspend for the video block (a system sleep, before a
 * mode-1 reset / BACO): the idle work cancelled, the pending video fences
 * given 100 ms and then forced (as amdgpu's fence driver at suspend),
 * jpeg_v4_0_suspend, then vcn_v4_0_suspend with amdgpu_vcn_suspend's copy
 * of each VCPU BO in system memory. DRM lock held. */
void RDNA3Bringup::vcnSuspend()
{
    uint64_t size = vcn40_vcpu_bo_size();
    uint32_t i;
    int rv, rj, k;

    if (!fVcn || !fDev || !fVcn->sw_ready || fVcnSuspended)
        return;
    fVcnSuspended = true;   /* vcnIdleTick leaves the block alone from here */
    if (fVcnIdleCall)
        thread_call_cancel(fVcnIdleCall);   /* no wait: the tick needs the DRM lock held here */
    for (k = 0; k < 100; k++) {
        bool busy = false;
        for (i = 0; i < RD_VCN_MAX_INST; i++)
            if (fVcn->ring_enc[i].ring_size &&
                !vcn40_fence_passed(fDev, &fVcn->ring_enc[i], fVcn->ring_enc[i].fence_seq))
                busy = true;
        if (fVcn->jpeg.ring_size && !vcn40_fence_passed(fDev, &fVcn->jpeg, fVcn->jpeg.fence_seq))
            busy = true;
        if (!busy)
            break;
        IOSleep(1);
    }
    for (i = 0; i < RD_VCN_MAX_INST; i++)
        if (fVcn->ring_enc[i].ring_size)
            rdna3_vram_write(fDev, fVcn->ring_enc[i].wb + RD_VCN_WB_FENCE, &fVcn->ring_enc[i].fence_seq, 4);
    if (fVcn->jpeg.ring_size)
        rdna3_vram_write(fDev, fVcn->jpeg.wb + RD_VCN_WB_FENCE, &fVcn->jpeg.fence_seq, 4);
    for (i = 0; i < fVcn->num_vcn_inst && i < RD_VCN_MAX_INST; i++)
        if (!fVcn->saved_bo[i])
            fVcn->saved_bo[i] = IOMalloc((vm_size_t)size);
    rj = jpeg40_suspend(fDev, fVcn);
    rv = vcn40_suspend(fDev, fVcn);
    publish("RDNA3,VCNSuspends", fVcn->suspends);
    publish("RDNA3,VCNSuspendResult", (uint64_t)(int64_t)rv);
    BLOG("VCN: suspended (JPEG %d, VCN %d), VCPU BOs %s", rj, rv,
         fVcn->saved_bo_valid[0] ? "saved to system memory" : "NOT saved (cleared at resume)");
    IOLockLock(fLock);
    IOLockWakeup(fLock, &fFenceEvent, false);
    IOLockUnlock(fLock);
}

/* after a wake that kept the card's power (the PSP sOS still runs):
 * amdgpu_device_ip_resume for VCN / JPEG. DRM lock held. */
void RDNA3Bringup::vcnResume()
{
    if (!fVcnSuspended || !fVcn || !fDev || !fPsp)
        return;
    if (!psp13_is_sos_alive(fPsp)) {
        BLOG("VCN: the card lost its power in the sleep: the video block waits for the next bring-up");
        return;
    }
    vcnStart();
}

/* amdgpu_pci_shutdown -> amdgpu_device_ip_suspend: the video block stops
 * before the machine powers off (card run 31: with rdna3vcn=2 a VCN job
 * still pending kept the idle tick running into the halt, which ended in a
 * nested panic) */
void RDNA3Bringup::vcnShutdown()
{
    IOLock *l = fOwner->drmLock();
    if (!fVcn)
        return;
    fVcnStop = true;
    if (fVcnIdleCall)
        thread_call_cancel_wait(fVcnIdleCall);
    if (l)
        IOLockLock(l);
    if (fDev && fVcn->sw_ready && !fVcnSuspended) {
        jpeg40_hw_fini(fDev, fVcn);
        vcn40_hw_fini(fDev, fVcn);
        fVcnSuspended = true;
    }
    if (l)
        IOLockUnlock(l);
    BLOG("VCN: stopped for the power-off");
}

void RDNA3Bringup::vcnIdleSchedule()
{
    uint64_t deadline;
    if (!fVcnIdleCall || fVcnStop)
        return;
    clock_interval_to_deadline(RD_VCN_IDLE_TIMEOUT_MS, kMillisecondScale, &deadline);
    thread_call_enter_delayed(fVcnIdleCall, deadline);
}

/* amdgpu_vcn_idle_work_handler / amdgpu_jpeg_idle_work_handler: gate what
 * has nothing left to do, look again in 1 s otherwise */
void RDNA3Bringup::vcnIdleTick(void *owner, void *)
{
    RDNA3Bringup *b = (RDNA3Bringup *)owner;
    IOLock *l = b->fOwner->drmLock();
    if (!l)
        return;
    IOLockLock(l);
    if (!b->fVcnStop && !b->fVcnSuspended && b->fVcn && b->fDev) {
        int vg = vcn40_idle_work_handler(b->fDev, b->fVcn);
        int jg = jpeg40_idle_work_handler(b->fDev, b->fVcn);
        b->publish("RDNA3,VCNGated", b->fVcn->cur_state == RD_PG_STATE_GATE ? 1 : 0);
        b->publish("RDNA3,VCNStarts", b->fVcn->starts);
        if (!vg || !jg)
            b->vcnIdleSchedule();
    }
    IOLockUnlock(l);
}

/* a VCN or JPEG job that never ends: amdgpu would reset the GPU; here the
 * video block is stopped, its pending fences complete and its rings stay
 * off until the next bring-up (the rest of the GPU keeps running) */
int RDNA3Bringup::vcnHang(uint32_t ip, uint32_t ring, uint64_t seq)
{
    uint32_t i;
    BLOG("hang: %s ring %u, fence %llu not signalled within 10 s: video block off",
         ip == AMDGPU_HW_IP_VCN_JPEG ? "JPEG" : "VCN", ring, (unsigned long long)seq);
    for (i = 0; i < RD_VCN_MAX_INST; i++)
        if (fVcn->ring_enc[i].ring_size) {
            fVcn->ring_enc[i].ready = 0;
            rdna3_vram_write(fDev, fVcn->ring_enc[i].wb + RD_VCN_WB_FENCE, &fVcn->ring_enc[i].fence_seq, 4);
        }
    if (fVcn->jpeg.ring_size) {
        fVcn->jpeg.ready = 0;
        rdna3_vram_write(fDev, fVcn->jpeg.wb + RD_VCN_WB_FENCE, &fVcn->jpeg.fence_seq, 4);
    }
    vcn40_set_powergating_state(fDev, fVcn, RD_PG_STATE_GATE);
    jpeg40_set_powergating_state(fDev, fVcn, RD_PG_STATE_GATE);
    fVcnHangs++;
    publish("RDNA3,VCNHangs", fVcnHangs);
    publishString("RDNA3,VCN", "off after a hang");
    IOLockLock(fLock);
    IOLockWakeup(fLock, &fFenceEvent, false);
    IOLockUnlock(fLock);
    return fenceSignaled(ip, ring, seq) ? 0 : -RD_ETIME;
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
    /* a re-init (reset, resume): the user's overdrive settings survive it
     * like amdgpu's user_overdrive_table */
    if (fPmCtl && fSmu->od_valid && fSmu->user_od) {
        if (!fUserOdTable)
            fUserOdTable = (OverDriveTableExternal_t *)IOMalloc(sizeof(*fUserOdTable));
        if (fUserOdTable) {
            memcpy(fUserOdTable, &fSmu->od_user, sizeof(*fUserOdTable));
            fUserOd = true;
        }
    }
    bzero(fSmu, sizeof(*fSmu));

    /* the VBIOS: boot-up clocks and the PMFW BoardTable */
    ret = vbios_read(fDev, &fVbios, fRom, RD_VBIOS_MAX_LEN);
    publish("RDNA3,VBIOSResult", (uint64_t)(int64_t)ret);
    if (ret == RDNA3_OK) {
        publish("RDNA3,VBIOSSize", fVbios.len);
        publishString("RDNA3,VBIOSPartNumber", fVbios.part_number);
        vb = &fVbios;
        {
            /* AMDGPU_INFO_VBIOS_INFO: amdgpu_atom_parse's strings */
            rdna3_vbios_strings vs;
            struct drm_amdgpu_info_vbios vi;
            vbios_strings(&fVbios, &vs);
            bzero(&vi, sizeof(vi));
            memcpy(vi.name, vs.name, sizeof(vi.name));
            memcpy(vi.vbios_pn, vs.pn, sizeof(vi.vbios_pn));
            memcpy(vi.vbios_ver_str, vs.ver_str, sizeof(vi.vbios_ver_str));
            memcpy(vi.date, vs.date, sizeof(vi.date));
            vi.version = vs.version;
            fOwner->drmSetVbios(fVbios.data, fVbios.len, &vi);
            BLOG("VBIOS %s, P/N %s, version %s, %s", vs.name, vs.pn, vs.ver_str, vs.date);
        }
        if (fAmdgpuExact) {
            /* amdgpu_device_init: amdgpu_atomfirmware_get_clock_info
             * (soc21_get_xclk = the spll reference), gmc_v11_0_sw_init:
             * amdgpu_atomfirmware_get_vram_info (the module BIOS scratch 4
             * names), amdgpu_ttm_reserve_tmr / psp_ras_initialize: the
             * memory-training and dynamic-boot-config capabilities */
            rdna3_vbios_clocks ck;
            rdna3_vbios_vram vr;
            rdna3_vbios_boot bt;
            uint32_t xclk = 0, module = 0;
            int cr = vbios_clock_info(&fVbios, &ck);
            if (cr == RDNA3_OK) {
                xclk = ck.spll_ref * 10;
                publish("RDNA3,XclkKHz", xclk);
            } else {
                BLOG("VBIOS: amdgpu_atomfirmware_get_clock_info failed (%d)", cr);
            }
            if (vbios_bootup_values(fDev, &fVbios, &bt) == RDNA3_OK) {
                const uint8_t *fwi = vbios_data_table(&fVbios, RD_ATOM_DT_FIRMWAREINFO, nullptr, nullptr, nullptr);
                uint32_t scr = fwi ? (uint32_t)fwi[0x18] | ((uint32_t)fwi[0x19] << 8) |
                                     ((uint32_t)fwi[0x1a] << 16) | ((uint32_t)fwi[0x1b] << 24) : 0;
                if (scr)
                    module = (rdna3_rreg(fDev, scr + 4) & 0x00ff0000) >> 16;
                if (vbios_mem_training_supported(bt.firmware_capability))
                    BLOG("VBIOS: two-stage memory training requested (cap 0x%x): not done by this driver",
                         bt.firmware_capability);
                if (vbios_dynamic_boot_config_supported(bt.firmware_capability))
                    BLOG("VBIOS: dynamic boot config (GECC) present (cap 0x%x): no RAS here",
                         bt.firmware_capability);
            }
            if (vbios_vram_info(&fVbios, module, &vr) == RDNA3_OK && vr.type) {
                publish("RDNA3,VRAMType", vr.type);
                publish("RDNA3,VRAMWidth", vr.width);
                publish("RDNA3,VRAMVendor", vr.vendor);
            } else {
                vr.type = vr.width = 0;
            }
            fOwner->drmSetAsicInfo(xclk, vr.type, vr.width, 0);
        }
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
    if (fAmdgpuExact) {
        /* smu_v13_0_register_irq_handler: the IVs reach smuIrq */
        if (!fSmuIrqCall)
            fSmuIrqCall = thread_call_allocate((thread_call_func_t)smuIrqTick, this);
        publish("RDNA3,PMThermalAlert", fSmu->irq_enabled ? 1 : 0);
        publish("RDNA3,PMPcieLevels", fSmu->pcie_levels);
        BLOG("PM: rdna3amdgpu=1: %u PCIe levels sent, thermal alert at %u C, %s",
             fSmu->pcie_levels, fSmu->software_shutdown_temp,
             fDev->amdgpu_dc ? "display core present" : "DALNotPresent sent");
    }
    pmCtlInit(vb);
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
        if (ret != RDNA3_OK && fReinit) {
            /* after a power loss or a mode-1 reset the GOP's firmware is
             * gone: ours, as rdna3dmub=1 loads it (dm_dmub_hw_init) */
            BLOG("DMUB: the GOP's firmware did not survive (%d): loading our own", ret);
            publish("RDNA3,DMUBReloaded", 1);
        } else if (ret != RDNA3_OK) {
            publishString("RDNA3,DMUB", "failed: attach");
            return;
        }
    }
    if (fDmubMode == 3 && fDmub->hw_init) {
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
    /* rdna3dmubreset=1 (step dmubreset): amdgpu's dmub_dcn32_reset instead
     * of the stop + reset verified on the card (hw/dmub32.c) */
    if (ret == RDNA3_OK)
        fDmub->amd_reset = rd3_arg("rdna3dmubreset", 0) ? 1 : 0;
    if (ret == RDNA3_OK)
        ret = dmub32_hw_init(fDmub, fVbios.data, fVbios.len, pspLoaded, 100000);
    if (fDmub->amd_reset)
        publish("RDNA3,DMUBAmdReset", (uint64_t)(fDmub->reset_acked | fDmub->reset_stopped << 1));
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
    /* rdna3amdgpu=1, after a reset or a wake: mes_v11_0_resume is
     * mes_v11_0_hw_init (the start above) + amdgpu_mes_resume */
    if (!ret && fAmdgpuExact && fReinit)
        gfx11_mes_resume(fDev, fMes);
    publish("RDNA3,MESResult", (uint64_t)(int64_t)ret);
    publish("RDNA3,MESSchedVersion", fMes->sched_version);
    publish("RDNA3,MESKiqVersion", fMes->kiq_version);
    publish("RDNA3,MESLegacyQueueMap", fMes->legacy_queue_map ? 1 : 0);
    BLOG("MES: scheduler %s %s (%d), firmware sched 0x%x kiq 0x%x", why, ret ? "FAILED" : "up", ret,
         fMes->sched_version, fMes->kiq_version);
    if (fUserqMask && !ret) {
        userqIrqs();
        /* a restarted scheduler has forgotten the queues it had: the DRM
         * puts them back (amdgpu_userq_post_reset), or, after a system
         * sleep, amdgpu_userq_resume does */
        if (fMesStarted) {
            fUserqRestart = true;
            fUserqRestartVramLost = fReinit && fLastVramLost;
            BLOG("user queues: the scheduler restarted (%s): the mapped queues go back on it", why);
        }
    }
    if (!ret)
        fMesStarted = true;
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
    uint32_t pasid;      /* the process id the MES knows this VM's user queues by */
    bool     live;       /* page tables exist */
    bool     guilty;     /* a job of this VM hung or page-faulted: no more submissions */
    uint32_t resetSeen, vramLostSeen;   /* fResetCount / fVramLostCount when created */
    uint64_t faultAddr;  /* amdgpu_vm_update_fault_cache: last fault for AMDGPU_INFO_GPUVM_FAULT */
    uint32_t faultStatus, faultHub;
    bool     gfxFlush;   /* next GFX job re-points the VMID and flushes from the ring */
    bool     compFlush;  /* same for the next compute job (the hub is shared, the rings are not) */
    bool     dmaFlush;   /* rdna3amdgpu=1: same for the next DMA job (amdgpu_vm_flush on the SDMA ring) */
    int      ownerPid;   /* the process that opened it (card run 49: a page fault named only "VMID 3") */
    uint32_t ownerUid;   /* its effective uid: rdna3_vmid_admit's per-uid cap */
    char     owner[24];
    bool     orphaned;   /* its file closed with a job still running: kept until the job is done */
    bool     kernel;     /* the accelerator's own (vmCreate(.., true)): not in the clients' pool */
};


void RDNA3Bringup::markGuilty(uint32_t vmid, const char *why)
{
    RDNA3Vm *v = vmid < 16 ? fVmByVmid[vmid] : nullptr;
    if (!v) {
        BLOG("%s on VMID %u, which no client owns", why, vmid);
        return;
    }
    if (!v->guilty)
        BLOG("%s: VMID %u's client (%s, pid %d) is guilty, its submissions now fail", why, vmid,
             v->owner[0] ? v->owner : "?", v->ownerPid);
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
    /* VRAMLOST is the DRM's, per context (rdna3_drm_device_vram_lost) */
    if (v->vramLostSeen != fVramLostCount)
        fl |= AMDGPU_CTX_QUERY2_FLAGS_RESET;
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
        /* P2: the RLC's view at the end of the variant (GPM_STAT power /
         * clock / LS, PG and CGCG state, CP busy) next to the PMFW's
         * activity; read after the samples, so the read wakes nothing
         * the variant measured */
        logBusy();
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
    /* smu_v13_0_0_read_sensor (MP1 13.0.0 / 13.0.10) has no
     * AMDGPU_PP_SENSOR_GPU_INPUT_POWER: -EOPNOTSUPP, which amdgpu_info_ioctl
     * turns into -EINVAL. The metrics table's power is the average
     * (AverageSocketPower, AMDGPU_INFO_SENSOR_GPU_AVG_POWER). */
    case AMDGPU_INFO_SENSOR_GPU_INPUT_POWER:
        return -RD_EINVAL;
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

/* -------------------------- PM control -------------------------- */

uint32_t RDNA3Bringup::pmctlGates(bool admin) const
{
    uint32_t g = 0;
    if (fPmCtl >= 1)
        g |= RD_PMCTL_GATE_WRITE;
    if (fPmCtl >= 2)
        g |= RD_PMCTL_GATE_OD;
    if (fBacoWanted && fPmCtl >= 1)
        g |= RD_PMCTL_GATE_BACO;
    if (admin)
        g |= RD_PMCTL_GATE_ADMIN;
    return g;
}

/* smu_late_init after smu13_pm_init: the queries (no message the default
 * profile does not send), then with rdna3pmctl the overdrive tables
 * (smu_set_default_od_settings) and, after a re-init, the user's settings
 * again (smu_restore_dpm_user_profile) */
void RDNA3Bringup::pmCtlInit(const rdna3_vbios *vb)
{
    int r = smu13_pm_query(fDev, fSmu, vb, fPmCtl >= 2);
    publish("RDNA3,PMQueryResult", (uint64_t)(int64_t)r);
    if (fPmCtl >= 1) {
        int in_suspend = 0;
        if (fReinit && fUserOd && fUserOdTable) {
            memcpy(&fSmu->od_user, fUserOdTable, sizeof(fSmu->od_user));
            fSmu->user_od = 1;
            in_suspend = 1;
        }
        r = smu13_set_default_od_settings(fDev, fSmu, in_suspend);
        publish("RDNA3,PMODResult", (uint64_t)(int64_t)r);
        if (fReinit) {
            /* smu_restore_dpm_user_profile: power limit, fan, overdrive */
            if (fUserPowerLimit && smu13_set_ppt_limit(fDev, fSmu, fUserPowerLimit))
                BLOG("PM: restoring the power limit (%u W) failed", fUserPowerLimit);
            if (fUserFanMode != RD_FAN_CTRL_AUTO) {
                smu13_set_fan_control_mode(fDev, fSmu, fUserFanMode);
                if (fUserFanMode == RD_FAN_CTRL_MANUAL && fUserFanPwm)
                    smu13_set_fan_speed_pwm(fDev, fSmu, fUserFanPwm);
                if (fUserFanMode == RD_FAN_CTRL_MANUAL && fUserFanRpm)
                    smu13_set_fan_speed_rpm(fDev, fSmu, fUserFanRpm);
            }
            if (!r && fSmu->user_od && smu13_restore_user_od_settings(fDev, fSmu))
                BLOG("PM: Failed to upload customized OD settings");
        }
    }
    pmCtlPublish();
    BLOG("PM control: rdna3pmctl=%u%s, power limit %u W (%u..%u W), OD %s, BACO support 0x%x",
         fPmCtl, fBacoWanted ? " rdna3baco=1" : "", fSmu->current_power_limit, fSmu->power_limit_min,
         fSmu->power_limit_max, fSmu->od_enabled ? "enabled" : "off",
         (unsigned)smu13_get_bamaco_support(fDev, fSmu));
}

/* what the PM control knows, in the IORegistry (RDNA3,PM*) */
void RDNA3Bringup::pmCtlPublish()
{
    rdna3_temp_range tr;
    publish("RDNA3,PMCtl", fPmCtl);
    publish("RDNA3,PMBACOAllowed", fBacoWanted ? 1 : 0);
    publish("RDNA3,PMPowerLimitW", fSmu->current_power_limit);
    publish("RDNA3,PMPowerLimitDefaultW", fSmu->power_limit_default);
    publish("RDNA3,PMPowerLimitMinW", fSmu->power_limit_min);
    publish("RDNA3,PMPowerLimitMaxW", fSmu->power_limit_max);
    publish("RDNA3,PMFanControl", fSmu->no_fan ? 0 : 1);
    publish("RDNA3,PMFanMode", smu13_get_fan_control_mode(fDev, fSmu));
    publish("RDNA3,PMODEnabled", fSmu->od_enabled ? 1 : 0);
    publish("RDNA3,PMODFeatures", fSmu->pptable.SkuTable.OverDriveLimitsBasicMax.FeatureCtrlMask);
    publish("RDNA3,PMODPptPercentMax", (uint64_t)(int64_t)fSmu->od_ppt_pct_max);
    publish("RDNA3,PMODPptPercentMin", (uint64_t)(int64_t)fSmu->od_ppt_pct_min);
    publish("RDNA3,PMGfxClkStandardMHz", fSmu->pstate_gfx.standard);
    publish("RDNA3,PMGfxClkPeakMHz", fSmu->pstate_gfx.peak);
    publish("RDNA3,PMUniqueID", fSmu->unique_id);
    publish("RDNA3,PMProfile", fSmu->power_profile_mode);
    publish("RDNA3,PMWorkloadMask", fSmu->workload_mask);
    if (smu13_get_thermal_temperature_range(fSmu, &tr) == RDNA3_OK) {
        publish("RDNA3,PMTempLimitMem", fSmu->pptable.SkuTable.TemperatureLimit[TEMP_MEM]);
        publish("RDNA3,PMTempEmergencyHotspot", (uint64_t)(tr.hotspot_emergency_max / 1000));
        publish("RDNA3,PMShutdownTempC", (uint64_t)tr.software_shutdown_temp);
    }
    if (fSmu->od_valid) {
        const OverDriveTable_t *od = &fSmu->od.OverDriveTable;
        publish("RDNA3,PMODGfxClkMinMHz", (uint64_t)(int64_t)od->GfxclkFmin);
        publish("RDNA3,PMODGfxClkMaxMHz", (uint64_t)(int64_t)od->GfxclkFmax);
        publish("RDNA3,PMODUclkMinMHz", od->UclkFmin);
        publish("RDNA3,PMODUclkMaxMHz", od->UclkFmax);
        publish("RDNA3,PMODVddGfxOffsetmV", (uint64_t)(int64_t)od->VoltageOffsetPerZoneBoundary[0]);
        publish("RDNA3,PMODPptPercent", (uint64_t)(int64_t)od->Ppt);
        publish("RDNA3,PMODFanMode", od->FanMode);
        publish("RDNA3,PMODFanTargetTempC", od->FanTargetTemperature);
        publish("RDNA3,PMODFanZeroRPM", od->FanZeroRpmEnable);
        publish("RDNA3,PMODUser", fSmu->user_od ? 1 : 0);
    }
}

/* the settings smu_restore_dpm_user_profile brings back after a re-init */
void RDNA3Bringup::pmCtlRemember(const rdna3_pmctl_req *rq)
{
    switch (rq->op) {
    case RD_PMCTL_POWER_LIMIT:
        fUserPowerLimit = fSmu->current_power_limit;
        break;
    case RD_PMCTL_FAN_MODE:
        fUserFanMode = (uint32_t)rq->a[0];
        if (fUserFanMode != RD_FAN_CTRL_MANUAL)
            fUserFanPwm = fUserFanRpm = 0;
        break;
    case RD_PMCTL_FAN_PWM:   /* PWM and RPM cannot co-exist */
        fUserFanPwm = (uint32_t)rq->a[0];
        fUserFanRpm = 0;
        break;
    case RD_PMCTL_FAN_RPM:
        fUserFanRpm = (uint32_t)rq->a[0];
        fUserFanPwm = 0;
        break;
    default:
        break;
    }
    fUserOd = fSmu->od_valid && fSmu->user_od;
}

/* rdna3baco=1: the chip through BACO and back, then levels 4-8 again like
 * after a mode-1 reset. A test of smu_v13_0_baco_enter / _exit on the card;
 * no suspend path uses BACO. DRM lock held. */
int RDNA3Bringup::bacoCycle()
{
    int support = smu13_get_bamaco_support(fDev, fSmu);
    if (!(support & RD_BACO_SUPPORT)) {
        BLOG("BACO: not supported (0x%x)", (unsigned)support);
        return RDNA3_EOPNOTSUPP;
    }
    forceFences();
    bool ok = gpuReset(true);
    fOwner->displayAfterReset(true);   /* the display went with the chip */
    publish("RDNA3,Resets", fResetCount);
    return ok ? RDNA3_OK : RDNA3_EIO;
}

int RDNA3Bringup::pmctl(const rdna3_pmctl_req *rq, rdna3_pmctl_reply *rp, bool admin)
{
    uint32_t gates = pmctlGates(admin);
    int r;

    if (!fPmUp || !fFinished || !fSmu || !fDev) {
        bzero(rp, sizeof(*rp));
        rp->op = rq->op;
        rp->info.gates = gates;
        rp->err = RDNA3_ENODEV;
        return rp->err;
    }
    if (rq->op == RD_PMCTL_BACO_CYCLE && rq->abi == RDNA3_PMCTL_ABI) {
        if (!(gates & RD_PMCTL_GATE_BACO))
            r = RDNA3_EOPNOTSUPP;
        else if (!(gates & RD_PMCTL_GATE_ADMIN))
            r = RDNA3_EPERM;
        else
            r = bacoCycle();
        BLOG("PM control: BACO cycle -> %d", r);
        bzero(rp, sizeof(*rp));
        rp->op = rq->op;
        if (fPmUp)
            smu13_pmctl_info(fDev, fSmu, gates, &rp->info);
        rp->err = r;
        return r;
    }
    r = smu13_pmctl(fDev, fSmu, rq, rp, gates);
    if (rq->op != RD_PMCTL_INFO) {
        BLOG("PM control: op %u (%u args: %d %d %d %d) -> %d", rq->op, rq->nargs,
             rq->nargs > 0 ? rq->a[0] : 0, rq->nargs > 1 ? rq->a[1] : 0,
             rq->nargs > 2 ? rq->a[2] : 0, rq->nargs > 3 ? rq->a[3] : 0, r);
        if (r == RDNA3_OK) {
            pmCtlRemember(rq);
            pmCtlPublish();
        }
    }
    fMetricsAt = mach_absolute_time();   /* the reply just read the metrics */
    return r;
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
    for (uint32_t i = 0; fVcn && i < RD_VCN_MAX_INST; i++)
        if (fVcn->ring_enc[i].ring_size)
            rdna3_vram_write(fDev, fVcn->ring_enc[i].wb + RD_VCN_WB_FENCE, &fVcn->ring_enc[i].fence_seq, 4);
    if (fVcn && fVcn->jpeg.ring_size)
        rdna3_vram_write(fDev, fVcn->jpeg.wb + RD_VCN_WB_FENCE, &fVcn->jpeg.fence_seq, 4);
    IOLockLock(fLock);
    IOLockWakeup(fLock, &fFenceEvent, false);
    IOLockUnlock(fLock);
}

/* Ported from amdgpu_fence_driver_hw_fini (rdna3amdgpu=1, at unload): each
 * ring waits for its last emitted fence (amdgpu_fence_wait_empty, bounded to
 * 10 s here: amdgpu waits without a limit and relies on the job timeout);
 * a ring that does not get there is forced. The fence interrupts are put
 * afterwards with the engines' hw_fini. */
void RDNA3Bringup::fenceDriverHwFini()
{
    struct { uint32_t ip, ring; uint64_t seq; bool live; } r[2 + 1 + RD_VCN_MAX_INST + 1];
    uint32_t n = 0;
    if (!fDev)
        return;
    r[n++] = { AMDGPU_HW_IP_GFX, 0, fGfx ? fGfx->fence_seq : 0, fGfx && fGfx->ready };
    r[n++] = { AMDGPU_HW_IP_COMPUTE, 0, fKcq ? fKcq->rb.fence_seq : 0, fKcq && fKcq->rb.ready };
    r[n++] = { AMDGPU_HW_IP_DMA, 0, fSdma ? fSdma->fence_seq : 0, fSdma && fSdma->ready };
    for (uint32_t i = 0; i < RD_VCN_MAX_INST; i++)
        r[n++] = { AMDGPU_HW_IP_VCN_ENC, i, fVcn ? fVcn->ring_enc[i].fence_seq : 0,
                   fVcn && fVcn->ring_enc[i].ring_size != 0 };
    r[n++] = { AMDGPU_HW_IP_VCN_JPEG, 0, fVcn ? fVcn->jpeg.fence_seq : 0, fVcn && fVcn->jpeg.ring_size != 0 };
    for (uint32_t i = 0; i < n; i++) {
        bool done = !r[i].live || !r[i].seq;   /* if (!seq) return 0 */
        for (uint32_t ms = 0; !done && ms < 10000; ms++) {
            done = fenceSignaled(r[i].ip, r[i].ring, r[i].seq) != 0;
            if (!done)
                IOSleep(1);
        }
        if (!done) {
            BLOG("unload: ring %u.%u stuck at fence %llu: forced", r[i].ip, r[i].ring,
                 (unsigned long long)r[i].seq);
            forceFences();   /* amdgpu_fence_driver_force_completion */
            break;
        }
    }
}

/* recovery steps can take the bus down with them (card run 19 froze the
 * whole VM host during a GFX recovery, its last lines lost): each step is
 * logged, then 100 ms for IOLog to reach the serial port before it runs */
#define HLOG(fmt, ...) do { BLOG("hang: " fmt, ##__VA_ARGS__); IOSleep(100); } while (0)

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

    if ((ip == AMDGPU_HW_IP_VCN_ENC || ip == AMDGPU_HW_IP_VCN_JPEG) && fVcn)
        return vcnHang(ip, ring, seq);
    /* whatever runs next re-programs its GDS partition (amdgpu_vmid_reset) */
    memset(fGdsKnown, 0, sizeof(fGdsKnown));
    HLOG("%s ring %u, fence %llu not signalled within 10 s",
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
        HLOG("soft recovery: killing the waves of VMID %u", vmid);
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
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ring != UINT64_MAX)
        HLOG("SDMA0 soft reset");
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ring != UINT64_MAX &&
        sdma60_soft_reset(fDev, fSdma) == RDNA3_OK) {
        fSdmaResets++;
        fResetCount++;
        /* the ring starts over: the next DMA job of every VM flushes again */
        for (uint32_t id = 1; id < 16; id++)
            if (fVmByVmid[id])
                fVmByVmid[id]->dmaFlush = true;
        publish("RDNA3,SDMAResets", fSdmaResets);
        publish("RDNA3,Resets", fResetCount);
        IOLockLock(fLock);
        IOLockWakeup(fLock, &fFenceEvent, false);
        IOLockUnlock(fLock);
        BLOG("hang: SDMA0 soft reset done, the DMA ring runs again");
        fOwner->displayAfterReset(false);   /* D4: DCN untouched? */
        return fenceSignaled(ip, ring, seq) ? 0 : -RD_ETIME;
    }

    /* rdna3amdgpu=1 with the MES scheduler: amdgpu_job_timedout's per ring
     * reset (amdgpu_ring_reset -> gfx_v11_0_reset_kgq / _reset_kcq), then the
     * ring's fences forced (amdgpu_fence_driver_force_completion) */
    if (fAmdgpuExact && r && fMes && fMes->ready) {
        int rr;
        HLOG("%s ring reset through the MES", ip == AMDGPU_HW_IP_GFX ? "GFX" : "compute");
        rr = ip == AMDGPU_HW_IP_GFX ? gfx11_reset_kgq(fDev, fGfx, fMes, vmid)
                                    : gfx11_reset_kcq(fDev, fKcq, fMes, vmid);
        if (rr == RDNA3_OK) {
            fResetCount++;   /* gpu_reset_counter */
            gfx11_force_fence_completion(fDev, r);
            for (uint32_t id = 1; id < 16; id++)
                if (fVmByVmid[id]) {
                    if (ip == AMDGPU_HW_IP_GFX)
                        fVmByVmid[id]->gfxFlush = true;
                    else
                        fVmByVmid[id]->compFlush = true;
                }
            IOLockLock(fLock);
            IOLockWakeup(fLock, &fFenceEvent, false);
            IOLockUnlock(fLock);
            BLOG("hang: ring reset done, the ring runs again");
            publish("RDNA3,Resets", fResetCount);
            return fenceSignaled(ip, ring, seq) ? 0 : -RD_ETIME;
        }
        HLOG("ring reset failed (%d): the full recovery next", rr);
    }

    /* the engine needs a reset; pending fences complete first so every
     * waiter returns (guilty contexts see -ECANCELED afterwards) */
    HLOG("forcing the pending fences");
    /* amdgpu_device_pre_asic_reset (rdna3amdgpu=1): the interrupt off while
     * the fences are forced (amdgpu_fence_driver_isr_toggle) */
    if (fAmdgpuExact && fIntr)
        fIntr->disable();
    forceFences();
    if (fAmdgpuExact && fIntr && fIrqLive)
        fIntr->enable();
    if (r && fGfx && fGfx->ready) {
        HLOG("GFX soft reset (GRBM_SOFT_RESET CP/GFX, KIQ/KCQ/GFX rings restart)");
        ok = gfx11_soft_reset(fDev, fGfx, fKiq, fKcq) == RDNA3_OK;
        if (!ok)
            HLOG("GFX soft reset failed: the whole chip next");
        if (ok) {
            for (uint32_t id = 1; id < 16; id++)
                if (fVmByVmid[id])
                    fVmByVmid[id]->gfxFlush = fVmByVmid[id]->compFlush = fVmByVmid[id]->dmaFlush = true;
            fResetCount++;
            BLOG("hang: GFX soft reset done (%u resets so far)", fResetCount);
            /* the KIQ restart reset both MES pipes: the scheduler again */
            if (fMes && fMes->ring.rb.ring_size)
                mesStart("after soft reset");
            if (fCgWanted)
                cgStart("after soft reset");
        }
    }
    if (ok)
        fOwner->displayAfterReset(false);   /* D4: DCN untouched? */
    if (!ok) {
        ok = gpuReset();
        fOwner->displayAfterReset(true);    /* D4: the display went with the chip */
    }
    publish("RDNA3,GPUResets", fGpuResets);
    publish("RDNA3,Resets", fResetCount);
    return fenceSignaled(ip, ring, seq) ? 0 : -RD_ETIME;
}

/* amdgpu_device_gpu_recover with the mode-1 method (soc21_asic_reset_method
 * on MP1 13.0.10, soc21_asic_reset -> amdgpu_device_mode1_reset): the PMFW
 * resets the chip, the PSP bootloader comes back, levels 4-8 run again.
 * VRAM normally survives; the magic word says so. */
/* every VM's next job flushes its TLBs, on every engine */
void RDNA3Bringup::vmFlushAllOnNextJob()
{
    for (uint32_t id = 1; id < 16; id++)
        if (fVmByVmid[id])
            fVmByVmid[id]->gfxFlush = fVmByVmid[id]->compFlush = fVmByVmid[id]->dmaFlush = true;
}

/* the reset magic word in VRAM: gone = VRAM lost, which clients see as
 * AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST */
bool RDNA3Bringup::checkVramLost(const char *what)
{
    uint32_t magic = 0;
    if (fResetMagic == UINT64_MAX)
        return false;
    rdna3_vram_read(fDev, fResetMagic, &magic, 4);
    fLastVramLost = magic != kResetMagic;
    if (fLastVramLost) {
        fVramLostCount++;
        BLOG("%s: VRAM content lost (0x%08x), clients see VRAMLOST", what, magic);
        return true;
    }
    return false;
}

/* VRAM went with the power (or the reset): every page table is garbage.
 * amdgpu evicts them to GTT before a sleep and amdgpu_vm_validate brings
 * them back; here the pool starts over, each live VM gets a new empty
 * root on both hubs, and the DRM maps its clients' ranges again
 * (takeVmReplay). Under the DRM lock, after hwInit. */
void RDNA3Bringup::vmRebuildAfterVramLoss()
{
    uint32_t n = 0, bad = 0;
    if (!fVmReady || !fPtPool || !fDev)
        return;
    gpuvm_pool_reset(fPtPool);
    for (uint32_t id = 1; id < 16; id++) {
        RDNA3Vm *v = fVmByVmid[id];
        if (!v || !v->live)
            continue;
        if (gpuvm_vm_init(fDev, &v->vm, fPtPool) != RDNA3_OK) {
            bad++;   /* stays live on an empty root: its jobs fault, it sees VRAMLOST */
            continue;
        }
        v->vm.vmid = id;
        fVmPd[id] = gpuvm_pd_addr(fDev, &v->vm);
        mmhub302_setup_vm_pt_regs(fDev, id, fVmPd[id]);
        if (fGmc && fGmc->gfxhub_enabled)
            gfxhub303_setup_vm_pt_regs(fDev, id, fVmPd[id]);
        vmFlush(id);
        v->gfxFlush = v->compFlush = v->dmaFlush = true;
        n++;
    }
    fVmReplay = true;
    BLOG("VRAM lost: page tables rebuilt for %u VM(s)%s", n, bad ? ", some without a root" : "");
}

/* amdgpu_device_ip_resume after a full reset or a power loss: everything
 * from level 4 on is programmed again by hwInit(). gmc_v11_0_resume adds
 * amdgpu_vmid_reset_all: no VMID keeps its PASID mapping. The caller holds
 * fDrmLock. */
bool RDNA3Bringup::reinitFromLevel4()
{
    bool ok;
    rdna3_vram_alloc_init(fDev, fAllocLo, fAllocHi);
    fDev->doorbell_ok = 0;
    if (fSdma) fSdma->ready = 0;
    if (fGfx) fGfx->ready = 0;
    if (fKiq) { fKiq->rb.ready = 0; fKiq->mapped = 0; }
    if (fKcq) { fKcq->rb.ready = 0; fKcq->mapped = 0; }
    fReinit = true;
    fOwner->drmHeldHere(true);
    ok = hwInit();
    fOwner->drmHeldHere(false);
    fReinit = false;
    fResetCount++;
    memset(fVmidPasid, 0, sizeof(fVmidPasid));
    return ok;
}

/* amdgpu_device_ip_suspend (phase 2, the display is the accelerator's):
 * the EOP interrupts off, then in reverse IP order VCN / JPEG, MES, SDMA,
 * GFX, PSP, IH, GMC and the SoC block. 'all' false: the video block only
 * (a reset without rdna3amdgpu=1 resets the rest anyway) */
void RDNA3Bringup::ipSuspend(bool all)
{
    if (fKcq && fKcq->rb.ready)
        gfx11_set_eop_interrupt(fDev, &fKcq->rb, 0);
    if (fGfx && fGfx->ready)
        gfx11_set_eop_interrupt(fDev, fGfx, 0);

    /* amdgpu_device_pre_asic_reset -> amdgpu_device_ip_suspend: the video
     * block suspended while the chip still runs (vcnStart resumes it) */
    vcnSuspend();
    /* rdna3amdgpu=1: mes_v11_0_suspend (amdgpu_mes_suspend: every gang) */
    if (all && fMes && fMes->ready && fDev)
        gfx11_mes_suspend(fDev, fMes);
    /* amdgpu_device_ip_suspend before a full reset: sdma_v6_0_suspend
     * (rdna3amdgpu=1) */
    if (all && fSdma && fDev)
        sdma60_hw_fini(fDev, fDev->ipver[HWIP_SDMA1].present ? 2 : 1);
    /* ... and gfx_v11_0_suspend after it (reverse IP order) */
    if (all)
        gfxSuspend();

    /* amdgpu_device_ip_suspend goes on in reverse IP order (rdna3amdgpu=1):
     * psp_suspend, ih_v6_0_suspend (= ih_v6_0_hw_fini), gmc_v11_0_suspend
     * (= gmc_v11_0_hw_fini) */
    if (all) {
        if (fPsp && fPsp->ring_created)
            psp13_suspend(fPsp);
        if (fIh && fIh->enabled && fDev)
            ih60_disable(fDev, fIh);
        if (fGmc && fDev)
            gmc11_hw_fini(fDev);
        /* soc21_common_suspend (= soc21_common_hw_fini), the first IP block */
        if (fDev && fDev->doorbell_ok)
            soc21_common_hw_fini(fDev);
    }
}

bool RDNA3Bringup::gpuReset(bool baco)
{
    uint32_t i;
    bool ok;

    if (baco) {
        HLOG("BACO cycle #%u: enter + exit through the PMFW, then bring-up from level 4",
             fBacoCycles + 1);
    } else {
        fGpuResets++;
        HLOG("GPU reset #%llu: mode-1 through the PMFW, then bring-up from level 4",
             (unsigned long long)fGpuResets);
    }
    if (fIntr)
        fIntr->disable();
    vmFlushAllOnNextJob();
    ipSuspend(fAmdgpuExact);

    /* soc21_need_full_reset is true on every GC 11: no IP soft reset first.
     * soc21_asic_mode1_reset: no bus mastering across the reset */
    fPci->setBusMasterEnable(false);
    if (baco) {
        /* amdgpu_device_baco_enter / _exit (smu_v13_0_baco_enter / _exit,
         * BACO_SEQ_BACO, no runtime-PM audio path) */
        int r = smu13_baco_enter(fDev, fSmu, 0, RD_BACO_SEQ_BACO);
        HLOG("BACO enter -> %d", r);
        if (r == RDNA3_OK) {
            IOSleep(100);
            r = smu13_baco_exit(fDev, fSmu, 0);
            HLOG("BACO exit -> %d", r);
        }
        fBacoCycles++;
        publish("RDNA3,PMBACOCycles", fBacoCycles);
        publish("RDNA3,PMBACOResult", (uint64_t)(int64_t)r);
    } else {
        /* amdgpu_device_mode1_reset: SMU mode-1 when the PMFW supports it */
        if (fAmdgpuExact && fSmu && fPmUp)
            HLOG("mode-1 reset: PMFW support %s", smu13_is_mode1_reset_supported(fSmu) ? "yes" : "no");
        mode1Reset();
    }
    if (psp13_wait_for_bootloader(fPsp) != RDNA3_OK)
        BLOG("GPU reset: the PSP bootloader did not come back");
    for (i = 0; i < fDev->usec_timeout; i++) {
        if (rdna3_rreg(fDev, mmRCC_CONFIG_MEMSIZE) != 0xffffffffu)
            break;
        IODelay(1);
    }
    bool vramLost = checkVramLost("GPU reset");
    ok = reinitFromLevel4();
    if (ok && vramLost)
        vmRebuildAfterVramLoss();
    if (fIntr)
        fIntr->enable();
    BLOG("%s #%llu %s", baco ? "BACO cycle" : "GPU reset",
         baco ? (unsigned long long)fBacoCycles : (unsigned long long)fGpuResets,
         ok ? "done, rings back up" : "FAILED");
    publish("RDNA3,VRAMLost", fVramLostCount);
    return ok;
}

/* D4 (RDNA3Resume.cpp): after a system wake, does the PSP sOS still run?
 * 1 yes, 0 no (the card lost its power), -1 unknown */
int RDNA3Bringup::sosAlive()
{
    if (!fPsp || !fPsp->d)
        return -1;
    return psp13_is_sos_alive(fPsp) ? 1 : 0;
}

/* D4 (rdna3resume=1, from RDNA3Resume.cpp): amdgpu_device_resume for a GPU
 * that lost its power in the sleep: amdgpu_device_need_post ->
 * amdgpu_device_asic_init (postIfNeeded), then
 * amdgpu_device_ip_resume, which here is the whole hwInit() again, as
 * gpuReset() runs it after the mode-1 reset. VRAM did not survive: the
 * clients see VRAM lost. Under the DRM lock. */
/* amdgpu_device_suspend (S3) for what is not the display (the
 * accelerator's dm_suspend ran first): amdgpu_fence_driver_hw_fini (every
 * ring idle, else forced), then amdgpu_device_ip_suspend_phase2 in reverse
 * IP order (ipSuspend). smu_suspend has nothing to send on SMU 13.0.7:
 * smu_disable_dpms leaves the features to the PMFW there, and mp1_state
 * stays NONE for a sleep. No ring takes work until the resume. Under the
 * DRM lock. */
void RDNA3Bringup::forgetVramLoss()
{
    if (!fLastResumeVramLost || !fVramLostCount)
        return;
    fVramLostCount--;
    fLastResumeVramLost = false;
    BLOG("resume: every evicted VRAM range is back: the clients are not told VRAM was lost");
    publish("RDNA3,VRAMLost", fVramLostCount);
}

bool RDNA3Bringup::systemSuspend()
{
    if (!fFinished || !fDev || fSuspended)
        return false;
    fSystemSuspends++;
    BLOG("suspend #%u: rings idle, then the IPs down in reverse order (amdgpu_device_suspend)", fSystemSuspends);
    fenceDriverHwFini();
    if (fIntr)
        fIntr->disable();
    ipSuspend(true);
    if (fSdma) fSdma->ready = 0;
    if (fGfx) fGfx->ready = 0;
    if (fKiq) { fKiq->rb.ready = 0; fKiq->mapped = 0; }
    if (fKcq) { fKcq->rb.ready = 0; fKcq->mapped = 0; }
    memset(fGdsKnown, 0, sizeof(fGdsKnown));
    /* amdgpu_pmops_suspend_noirq: amdgpu_acpi_should_gpu_reset is true for
     * a dGPU going to S3 (the target state is not s2idle), so
     * amdgpu_asic_reset - mode 1 on MP1 13.0.10 (soc21_asic_reset ->
     * amdgpu_device_mode1_reset): the wake starts from a reset GPU whether
     * or not the platform cut its power. Card run 39: QEMU kept the card
     * powered through the guest's S3, the wake found the sOS still running
     * with its TMR freed and LOAD_IP_FW SMC timed out; the next sleep lost
     * the card (config space all 1s) and the host hung taking it back */
    if (rdna3_rreg(fDev, mmRCC_CONFIG_MEMSIZE) != 0xffffffffu) {
        int r = mode1Reset();
        BLOG("suspend #%u: mode-1 reset before the sleep (amdgpu_pmops_suspend_noirq) -> %d", fSystemSuspends, r);
        publish("RDNA3,SuspendResetResult", (uint64_t)(int64_t)r);
    }
    fSuspended = true;
    publish("RDNA3,SystemSuspends", fSystemSuspends);
    return true;
}

bool RDNA3Bringup::resumeAfterPowerLoss()
{
    uint64_t fbStart = fDev ? fDev->fb_start : 0;
    bool ok;

    if (!fFinished || !fDev || !fPsp)
        return false;
    fResumes++;
    BLOG("resume #%u: %s: post if needed, then bring-up from level 4", fResumes,
         fSuspended ? "after amdgpu_device_suspend" : "the GPU lost its power in the sleep");
    IOSleep(100);
    if (fIntr)
        fIntr->disable();
    forceFences();
    memset(fGdsKnown, 0, sizeof(fGdsKnown));
    vmFlushAllOnNextJob();
    fPci->setBusMasterEnable(false);
    /* as run(): in a VM the wake comes with a bus reset (QEMU resets the
     * devices on an S3 wake), which leaves the PSP stuck */
    resetIfPspStuck();
    if (!postIfNeeded()) {
        BLOG("resume: ASIC_Init failed");
        publish("RDNA3,ResumeResult", 1);
        return false;
    }
    if (psp13_wait_for_bootloader(fPsp) != RDNA3_OK)
        BLOG("resume: the PSP bootloader is not ready");
    if (rdna3_read_fb_location(fDev) != RDNA3_OK || fDev->fb_start != fbStart) {
        BLOG("resume: the FB location moved (0x%llx -> 0x%llx): every MC address is stale, giving up",
             (unsigned long long)fbStart, (unsigned long long)fDev->fb_start);
        publish("RDNA3,ResumeResult", 2);
        return false;
    }
    bool vramLost = checkVramLost("resume");
    fLastResumeVramLost = vramLost;
    /* rdna3amdgpu=1: soc21_common_resume - an aborted S3 with the TOS still
     * running gets a reset first (soc21_need_reset_on_resume); this wake
     * follows a completed sleep (amdgpu: suspend_complete), so it does not;
     * soc21_common_hw_init is in hwInit below */
    if (fAmdgpuExact && soc21_need_reset_on_resume(fDev, 1, 1)) {
        BLOG("resume: S3 suspend aborted, resetting");
        mode1Reset();
    }
    ok = reinitFromLevel4();
    if (ok && vramLost)
        vmRebuildAfterVramLoss();
    fSuspended = false;
    if (fIntr)
        fIntr->enable();
    publish("RDNA3,Resumes", fResumes);
    publish("RDNA3,ResumeResult", ok ? 0 : 3);
    publish("RDNA3,VRAMLost", fVramLostCount);
    BLOG("resume #%u %s", fResumes, ok ? "done, rings back up" : "FAILED");
    return ok;
}

/* ------------------------------ GPUVM ------------------------------ */


void *RDNA3Bringup::vmCreate(bool quiet, bool kernel)
{
    RDNA3Vm *v = (RDNA3Vm *)IOMalloc(sizeof(*v));
    if (!v)
        return nullptr;
    bzero(v, sizeof(*v));
    v->vm.root = UINT64_MAX;
    v->resetSeen = fResetCount;
    v->vramLostSeen = fVramLostCount;
    /* the opener's thread (IOServiceOpen -> the user client's start) */
    v->ownerPid = proc_selfpid();
    v->ownerUid = kauth_cred_getuid(kauth_cred_get());
    v->kernel = kernel;
    if (kernel)
        strlcpy(v->owner, "accelerator", sizeof(v->owner));   /* maybe made on a client's thread */
    else
        proc_selfname(v->owner, sizeof(v->owner));
    if (!fVmReady)
        return v;
    /* one VMID per open file for now (amdgpu hands VMIDs out per job); with
     * user queues VMIDs 8-15 are the MES's (SET_HW_RESOURCES vmid masks,
     * amdgpu's first_kfd_vmid) */
    fNextPasid = fNextPasid % 0xfffe + 1;
    v->pasid = fNextPasid;
    /* rdna3amdgpu=1: amdgpu's GFXHUB id_mgr hands out VMIDs below
     * first_kfd_vmid only (8..15 have the compute apertures) */
    for (uint32_t id = 1; id < vmidLimit(); id++) {
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
            v->gfxFlush = v->compFlush = v->dmaFlush = true;
            BLOG("VM: VMID %u for %s (pid %d)", id, v->owner[0] ? v->owner : "?", v->ownerPid);
            break;
        }
    }
    if (!v->live) {
        /* amdgpu never hands out a VM that cannot run: amdgpu_vm_init
         * fails without its root table, and VMIDs are grabbed per job
         * (amdgpu_vmid_grab waits for one). With one VMID per file here a
         * record-only VM would map nothing and every CS of it would fail
         * with -EINVAL: no VM, so the open fails and the client sees no
         * device */
        if (!quiet)
            BLOG("VM: no %s left for %s (pid %d): no VM, a DRM open fails",
                 vmidsFree() ? "page tables" : "VMID", v->owner[0] ? v->owner : "?", v->ownerPid);
        IOFree(v, sizeof(*v));
        return nullptr;
    }
    publish("RDNA3,VMIDsInUse", (uint64_t)__builtin_popcount(fVmidUsed));
    return v;
}

uint32_t RDNA3Bringup::vmidsFree() const
{
    uint32_t n = 0;
    for (uint32_t id = 1; id < vmidLimit(); id++)
        if (!(fVmidUsed & (1u << id)))
            n++;
    return n;
}

/* the DRM kept this VM: its file closed while a job of it still ran after
 * the recovery (rdna3_drm_file_close). reclaimable: it goes once the job is
 * done (rdna3_drm_device_reap_orphans); else it never does. */
void RDNA3Bringup::vmOrphaned(void *p, bool reclaimable)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    if (!v || v->orphaned || !v->live)
        return;   /* a record-only VM holds no VMID */
    v->orphaned = true;
    fVmidsLeaked++;
    publish("RDNA3,VMIDsLeaked", fVmidsLeaked);
    BLOG("VM: %s (pid %d) closed with a job still running: VMID %u kept %s (%u kept now)",
         v->owner[0] ? v->owner : "?", v->ownerPid, v->vmid,
         reclaimable ? "until the job is done (a later GPU reset ends it)" : "for good (no orphan slot)",
         fVmidsLeaked);
}

/* the kext stops: VMs nobody destroyed - orphans whose job never finished,
 * which rdna3_drm_device_fini can no longer hand to a bring-up - lose
 * their memory; the hardware is down, the table pool goes after this */
void RDNA3Bringup::vmFreeLeftovers()
{
    uint32_t n = 0;
    for (uint32_t id = 1; id < 16; id++) {
        if (!fVmByVmid[id])
            continue;
        IOFree(fVmByVmid[id], sizeof(RDNA3Vm));
        fVmByVmid[id] = nullptr;
        fVmPd[id] = 0;
        n++;
    }
    fVmidUsed = 0;
    fVmidsLeaked = 0;
    if (n)
        BLOG("stop: %u VM(s) still held (orphans of hung jobs) freed", n);
}

bool RDNA3Bringup::vmCanOpen(uint32_t uid, bool console)
{
    char who[384];
    size_t len = 0;
    uint32_t mine = 0, kernel = 0, freeIds, pool;
    int verdict;
    if (!fVmReady)
        return true;
    freeIds = vmidsFree();
    for (uint32_t id = 1; id < 16; id++) {
        if (!fVmByVmid[id])
            continue;
        if (fVmByVmid[id]->kernel)
            kernel++;   /* the accelerator's own, whichever thread made it */
        else if (fVmByVmid[id]->ownerUid == uid)
            mine++;
    }
    /* the clients' pool: the VMIDs less the kernel's own */
    pool = vmidLimit() - 1 > kernel ? vmidLimit() - 1 - kernel : 0;
    verdict = rdna3_vmid_admit(pool, freeIds, mine, uid, console ? 1 : 0);
    if (verdict == RD_VMID_ADMIT)
        return true;
    who[0] = 0;
    for (uint32_t id = 1; id < vmidLimit() && len < sizeof(who); id++) {
        RDNA3Vm *v = fVmByVmid[id];
        if (!v)
            continue;
        int w = snprintf(who + len, sizeof(who) - len, " %u=%s/%d/%u%s", id, v->owner[0] ? v->owner : "?",
                         v->ownerPid, v->ownerUid, v->orphaned ? "(leaked)" : "");
        if (w < 0)
            break;
        len += (size_t)w;
    }
    fVmidRefusals++;
    BLOG("VM: DRM open #%u of uid %u%s refused (-ENOSPC, %s): %u of %u client VMIDs free, this uid "
         "holds %u of its %u, %u leaked (VMID=process/pid/uid:%s); that client sees no device",
         fVmidRefusals, uid, console ? " (console)" : "", rdna3_vmid_admit_reason(verdict), freeIds, pool,
         mine, rdna3_vmid_uid_cap(pool, console ? 1 : 0), fVmidsLeaked, who);
    publish("RDNA3,VMIDRefusals", fVmidRefusals);
    return false;
}

/* gfx_v11_0_suspend = gfx_v11_0_hw_fini (rdna3amdgpu=1, before a full
 * reset): the CP fault interrupts put, the KGQ and KCQ unmapped and the MES
 * KIQ stopped (amdgpu_mes_kiq_hw_fini), the CP halted, the GFXHUB GART off.
 * hwInit brings everything back (gfx_v11_0_resume = gfx_v11_0_hw_init). */
/* amdgpu_gfx_disable_kgq: with the MES scheduler running (rdna3amdgpu=1,
 * enable_mes) the scheduler preempts the gfx queue, else the KIQ unmaps it */
void RDNA3Bringup::kgqStop()
{
    if (fAmdgpuExact && fMes && fMes->ready)
        gfx11_kgq_stop_mes(fDev, fMes, fGfx);
    else
        gfx11_kgq_stop(fDev, fKiq, fGfx);
}

void RDNA3Bringup::gfxSuspend()
{
    if (!fDev || !fGfx)
        return;
    gfx11_set_priv_reg_fault_state(fDev, 0);
    gfx11_set_priv_inst_fault_state(fDev, 0);
    gfx11_set_bad_op_fault_state(fDev, 0);
    if (fGfx->async && fGfx->ready && fKiq && fKiq->rb.ready)
        kgqStop();
    if (fKiq)
        gfx11_mec_stop(fDev, fKiq, fKcq, fMes);
    gfx11_cp_gfx_enable(fDev, 0);
    if (fGmc)
        gfxhub303_gart_disable(fDev);
    fGfx->ready = 0;
}

void RDNA3Bringup::vmFlush(uint32_t vmid)
{
    /* rdna3amdgpu=1: amdgpu_gmc_flush_gpu_tlb -> gmc_v11_0_flush_gpu_tlb for
     * the GFXHUB, then the MMHUB: with the KIQ running the invalidation goes
     * through it (amdgpu_gmc_fw_reg_write_reg_wait, "necessary ... for
     * GFXOFF to function properly under bare metal"); MMIO before that */
    if (fAmdgpuExact && fDev && fMes && fMes->ready) {
        /* the MES runs: amdgpu_gmc_fw_reg_write_reg_wait goes through
         * amdgpu_mes_reg_write_reg_wait (MISC WRM_REG_WR_WAIT); the KIQ
         * belongs to the MES now */
        rdna3_vm_flush_regs f;
        if (fGmc && fGmc->gfxhub_enabled) {
            rdna3_hdp_flush(fDev);
            gfxhub303_flush_regs(fDev, vmid, 17, &f);
            gfx11_mes_reg_write_reg_wait(fDev, fMes, f.req, f.ack, f.req_val, 1u << vmid);
        }
        rdna3_hdp_flush(fDev);
        mmhub302_flush_regs(fDev, vmid, 17, &f);
        gfx11_mes_reg_write_reg_wait(fDev, fMes, f.req, f.ack, f.req_val, 1u << vmid);
        return;
    }
    if (fAmdgpuExact && fDev && fKiq && fKiq->rb.ready) {
        rdna3_vm_flush_regs f;
        if (fGmc && fGmc->gfxhub_enabled) {   /* adev->gfx.is_poweron */
            rdna3_hdp_flush(fDev);
            gfxhub303_flush_regs(fDev, vmid, 17, &f);
            gfx11_kiq_reg_write_reg_wait(fDev, fKiq, f.req, f.ack, f.req_val, 1u << vmid);
        }
        rdna3_hdp_flush(fDev);
        mmhub302_flush_regs(fDev, vmid, 17, &f);
        gfx11_kiq_reg_write_reg_wait(fDev, fKiq, f.req, f.ack, f.req_val, 1u << vmid);
        return;
    }
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
        BLOG("VM: VMID %u of %s (pid %d) freed%s", v->vmid, v->owner[0] ? v->owner : "?", v->ownerPid,
             v->guilty ? " (it was guilty)" : "");
        publish("RDNA3,VMIDsInUse", (uint64_t)__builtin_popcount(fVmidUsed));
    }
    if (v->orphaned && fVmidsLeaked) {
        fVmidsLeaked--;
        publish("RDNA3,VMIDsLeaked", fVmidsLeaked);
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
    /* the UAPI checked offset + size against the BO; a VRAM mapping is
     * checked again here, as the GTT one below is */
    if (offset > bo->size || size > bo->size - offset)
        return -RD_EINVAL;
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
    if (fUserqMask)
        userqFlushMesVmids();
    v->gfxFlush = v->compFlush = v->dmaFlush = true;
    return 0;
}

/* amdgpu_fill_buffer: a new VRAM BO the CPU cannot reach through BAR0 is
 * cleared by the kernel SDMA ring (PR #2 review: it exposed the previous
 * owner's data) */
int RDNA3Bringup::clearVram(uint64_t vramOff, uint64_t len)
{
    if (!fSdma || !fSdma->ready)
        return RDNA3_ENODEV;
    return sdma60_fill_sync(fDev, fSdma, rdna3_vram_mc(fDev, vramOff), 0, len);
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

int RDNA3Bringup::copyVramRect(uint64_t srcOff, uint32_t srcPitch, uint64_t dstOff, uint32_t dstPitch,
                               uint32_t rowBytes, uint32_t rows)
{
    if (!fSdma || !fSdma->ready)
        return RDNA3_ENODEV;
    if (!rows || !rowBytes || srcOff + (uint64_t)(rows - 1) * srcPitch + rowBytes > fDev->vram_size ||
        dstOff + (uint64_t)(rows - 1) * dstPitch + rowBytes > fDev->vram_size)
        return RDNA3_EINVAL;
    return sdma60_copy_rect_sync(fDev, fSdma, rdna3_vram_mc(fDev, srcOff), srcPitch, rdna3_vram_mc(fDev, dstOff),
                                 dstPitch, rowBytes, rows);
}

int RDNA3Bringup::copyPagesToVram(uint64_t vramOff, const uint64_t *pages, uint64_t npages, uint64_t len)
{
    const uint32_t winPages = 4096;   /* copyVramToPages' window */
    if (!fGmc || !fGmc->mmhub_enabled || !fSdma || !fSdma->ready ||
        fGmc->gart_size < (uint64_t)winPages * 4096)
        return RDNA3_ENODEV;
    return rdna3_copy_pages_to_vram(fDev, fGmc, fSdma, vramOff, pages, npages, len,
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
    if (fUserqMask)
        userqFlushMesVmids();
    v->gfxFlush = v->compFlush = v->dmaFlush = true;
}

/* ---------------------------- user queues ---------------------------- */

/* Ported from gfx_v11_0_set_userq_eop_interrupts and
 * sdma_v6_0_set_userq_trap_interrupts: the interrupts that carry user queue
 * fences (both SDMA 6.0 instances) */
void RDNA3Bringup::userqIrqs()
{
    gfx11_set_userq_eop_interrupts(fDev, fUserqMask, 1);
    if (fUserqMask & (1u << AMDGPU_HW_IP_DMA))
        sdma60_set_userq_trap_interrupts(fDev, 2, 1);
}

/* the user queue gates of gfx_v11_0's gpu_early_init and sdma_v6_0_sw_init:
 * the IPs whose firmware runs them get userq_mes_funcs, the UAPI learns of
 * them (amdgpu_userq_get_supported_ip_mask) */
void RDNA3Bringup::userqStart(const uint32_t ver[32])
{
    uint32_t mask = 0;
    if (!fUserqWanted)
        return;
    if (!fMes || !fMes->ready || !fDev->doorbell_ok || !fDoorbell || !fGmc || !fGmc->gfxhub_enabled) {
        BLOG("user queues: rdna3userq=1, but the MES scheduler or the doorbells are not up (level 8 needed)");
        publish("RDNA3,UserQueues", 0);
        return;
    }
    if (gfx11_userq_supported(ver[AMDGPU_INFO_FW_GFX_ME], ver[AMDGPU_INFO_FW_GFX_PFP],
                              ver[AMDGPU_INFO_FW_GFX_MEC], fMes->sched_version & RD_MES_VERSION_MASK))
        mask |= (1u << AMDGPU_HW_IP_GFX) | (1u << AMDGPU_HW_IP_COMPUTE);
    if (sdma60_userq_supported(ver[AMDGPU_INFO_FW_SDMA]))
        mask |= 1u << AMDGPU_HW_IP_DMA;
    fUserqMask = mask;
    userqIrqs();
    publish("RDNA3,UserQueues", mask);
    BLOG("user queues: IP mask 0x%x (ME %u PFP %u MEC %u MES %u SDMA %u), doorbell BAR %llu KiB", mask,
         ver[AMDGPU_INFO_FW_GFX_ME], ver[AMDGPU_INFO_FW_GFX_PFP], ver[AMDGPU_INFO_FW_GFX_MEC],
         fMes->sched_version & RD_MES_VERSION_MASK, ver[AMDGPU_INFO_FW_SDMA],
         (unsigned long long)(fDoorbell->getLength() >> 10));
    if (mask)
        fOwner->drmUserqReady(mask, fDoorbell->getLength());
}

/* what amdgpu's TLB flush by PASID achieves: the MES gives a process one of
 * VMIDs 8-15, which the kext does not learn, so a changed VM flushes them all */
void RDNA3Bringup::userqFlushMesVmids()
{
    for (uint32_t id = 8; id < 16; id++)
        vmFlush(id);
}

int RDNA3Bringup::userqMqdInit(uint32_t ip, void *mqd, const amd_mqd_prop *p)
{
    int r;
    if (!fUserqMask || !(fUserqMask & (1u << ip)))
        return -RD_EINVAL;
    if (ip == AMDGPU_HW_IP_GFX)
        r = gfx11_gfx_mqd_init(fDev, (uint32_t *)mqd, p);
    else if (ip == AMDGPU_HW_IP_COMPUTE)
        r = gfx11_compute_mqd_init(fDev, (uint32_t *)mqd, p);
    else
        r = sdma60_mqd_init(fDev, (uint32_t *)mqd, p);
    return r ? -RD_EINVAL : 0;
}

int RDNA3Bringup::userqAdd(void *p, amd_mes_add_queue *in)
{
    RDNA3Vm *v = (RDNA3Vm *)p;
    int r;
    if (!fUserqMask || !fMes || !fMes->ready || !fGmc || !v || !v->live || !fVmReady)
        return -RD_EINVAL;
    in->process_id = v->pasid;                                   /* vm->pasid */
    in->page_table_base_addr = gpuvm_pd_addr(fDev, &v->vm);      /* amdgpu_gmc_pd_addr */
    rdna3_hdp_flush(fDev);   /* the MQD and contexts were written through BAR0 */
    r = gfx11_mes_add_hw_queue(fDev, fMes, in, fGmc->vm_cntx_cntl);
    BLOG("user queue: ADD_QUEUE type %u doorbell 0x%x PASID %u: %d (PD 0x%llx, MQD 0x%llx, wptr VA 0x%llx "
         "MC 0x%llx, process ctx 0x%llx, gang ctx 0x%llx)", in->queue_type, in->doorbell_offset, in->process_id, r,
         (unsigned long long)in->page_table_base_addr, (unsigned long long)in->mqd_addr,
         (unsigned long long)in->wptr_addr, (unsigned long long)in->wptr_mc_addr,
         (unsigned long long)in->process_context_addr, (unsigned long long)in->gang_context_addr);
    return r == RDNA3_OK ? 0 : r == RDNA3_ETIME ? -RD_ETIME : -RD_EINVAL;
}

/* diagnostic (card run 33: the compute user queue ran - its HQD rptr
 * reached the wptr - but its fence never signalled): the interrupts the
 * kernel saw, and the last ones nobody claimed */
void RDNA3Bringup::userqDiag()
{
    uint32_t n = fIvOtherN, i;
    BLOG("userq diag: IRQs user queue %llu, other %llu, compute fence %llu", (unsigned long long)fIrqUserq,
         (unsigned long long)fIrqOther, (unsigned long long)fIrqFenceComp);
    for (i = n > 4 ? n - 4 : 0; i < n; i++) {
        const rdna3_iv *iv = &fIvOther[i & 3];
        BLOG("userq diag: unclaimed IV client %u src %u ring %u vmid %u pasid %u data %08x %08x %08x %08x",
             iv->client_id, iv->src_id, iv->ring_id, iv->vmid, iv->pasid, iv->src_data[0], iv->src_data[1],
             iv->src_data[2], iv->src_data[3]);
    }
}

int RDNA3Bringup::userqRemove(const amd_mes_remove_queue *in)
{
    int r;
    if (!fMes || !fMes->ready)
        return -RD_EINVAL;
    gfx11_userq_hqd_dump(fDev);   /* diagnostic: the queue's HQD before it goes */
    userqDiag();
    r = gfx11_mes_remove_hw_queue(fDev, fMes, in);
    BLOG("user queue: REMOVE_QUEUE doorbell 0x%x: %d", in->doorbell_offset, r);
    return r == RDNA3_OK ? 0 : r == RDNA3_ETIME ? -RD_ETIME : -RD_EINVAL;
}

/* amdgpu_bo_gpu_offset after amdgpu_ttm_alloc_gart: VRAM by its MC
 * address, a one-page GTT BO through a GART slot it keeps while it lives */
int RDNA3Bringup::gpuAddr(rdna3_bo *bo, uint64_t *mc)
{
    const uint64_t winPages = 4096;   /* copyVramToPages' window, then the user fence slots */
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        *mc = rdna3_vram_mc(fDev, bo->vram_offset);
        return 0;
    }
    RDNA3Gtt *gt = (RDNA3Gtt *)bo->backing;
    if (bo->placement != AMDGPU_GEM_DOMAIN_GTT || !gt || gt->npages != 1)
        return -RD_EINVAL;
    if (!fGmc || !fGmc->mmhub_enabled || fGmc->gart_size < (winPages + kUfSlots + kUqSlots) * 4096)
        return -RD_ENODEV;
    uint64_t base = fGmc->gart_size - (winPages + kUfSlots + kUqSlots) * 4096;
    if (gt->gartSlot) {
        *mc = fGmc->gart_start + base + (gt->gartSlot - 1) * 4096ull;
        return 0;
    }
    uint32_t i;
    for (i = 0; i < kUqSlots && fUqSlot[i]; i++)
        ;
    if (i == kUqSlots)
        return -RD_ENOSPC;
    const uint64_t flags = RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                           RD_PTE_WRITEABLE | RD_PTE_MTYPE_NV10(RD_MTYPE_UC);
    if (gmc11_gart_map(fDev, fGmc, base + i * 4096ull, gt->pages[0], 1, flags) != RDNA3_OK)
        return -RD_ENOMEM;
    vmFlush(0);
    fUqSlot[i] = true;
    gt->gartSlot = i + 1;
    *mc = fGmc->gart_start + base + i * 4096ull;
    return 0;
}

void RDNA3Bringup::gartRelease(uint32_t slot)
{
    const uint64_t winPages = 4096;
    if (!slot || slot > kUqSlots || !fGmc || fGmc->gart_size < (winPages + kUfSlots + kUqSlots) * 4096)
        return;
    uint64_t base = fGmc->gart_size - (winPages + kUfSlots + kUqSlots) * 4096;
    gmc11_gart_unmap(fDev, fGmc, base + (slot - 1) * 4096ull, 1);
    vmFlush(0);
    fUqSlot[slot - 1] = false;
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
        if (idle < 0 && (!fUfSlot[i].used || fenceSignaled(fUfSlot[i].ip, fUfSlot[i].ring, fUfSlot[i].seq)))
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
    if (!v || !v->live || (ring != 0 && !(ip == AMDGPU_HW_IP_VCN_ENC && ring < RD_VCN_MAX_INST)))
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
    int r = submitRing(v, ip, ring, ibs, n, job, ufMc, seq);
    if (!r && ufSlot >= 0) {
        fUfSlot[ufSlot].ip = ip;
        fUfSlot[ufSlot].ring = ring;
        fUfSlot[ufSlot].seq = *seq;
    }
    {
        /* card run 2026-10-17b's trace: every submission, so a plugin's
         * VkResult or timeout lines up with what reached the GPU */
        struct rd3_trace_ev e;
        bzero(&e, sizeof e);
        e.type = RD3_TR_CS;
        e.head = 0xff;
        e.arg = (uint8_t)ip;
        e.flags = (uint8_t)ring;
        e.v0 = (uint32_t)v->ownerPid;
        e.v1 = v->vmid;
        e.v2 = r ? 0u : (uint32_t)*seq;
        e.v3 = (n & 0xffffu) | ((job ? (job->bo_count > 0xffffu ? 0xffffu : job->bo_count) : 0u) << 16);
        e.v4 = (uint32_t)r;
        e.pa = n && ibs ? ibs[0].va : 0;
        rdna3TraceCore(&e);
    }
    return r;
}

int RDNA3Bringup::submitRing(RDNA3Vm *v, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                             const rdna3_cs_job *job, uint64_t ufMc, uint64_t *seq)
{
    /* VCN_ENC (ring = the VCN instance of the context's entity) and
     * VCN_JPEG (the checked copies): amdgpu_ib_schedule on the video rings;
     * the VM's page tables are on the MMHUB since vmCreate */
    if ((ip == AMDGPU_HW_IP_VCN_ENC || ip == AMDGPU_HW_IP_VCN_JPEG) && fVcn) {
        rdna3_vcn_ib vib[RD_MAX_CS_IBS];
        uint32_t s;
        if (n > RD_MAX_CS_IBS)
            return -RD_EINVAL;
        for (uint32_t i = 0; i < n; i++) {
            vib[i].va = ibs[i].va;
            vib[i].length_dw = ibs[i].length_dw;
            vib[i].kdata = ibs[i].kdata;
        }
        s = ip == AMDGPU_HW_IP_VCN_ENC ? vcn40_submit(fDev, fVcn, ring, v->vmid, vib, n, ufMc)
                                        : jpeg40_submit(fDev, fVcn, v->vmid, vib, n, ufMc);
        vcnIdleSchedule();
        if (!s)
            return -RD_EIO;
        *seq = s;
        return 0;
    }
    /* GFX and compute: user fence, and the GDS switch when the VMID's
     * partition is not the job's (amdgpu_vm_flush gds_switch_needed); the
     * DMA ring has no GDS */
    rdna3_gfx_job_ext x = {};
    const uint32_t want[6] = { job->gds_base, job->gds_size, job->gws_base, job->gws_size,
                               job->oa_base, job->oa_size };
    x.uf_addr = ufMc;
    x.uf_seq_base = job->seq_base;
    x.gds_switch = !fGdsKnown[v->vmid] || memcmp(fGds[v->vmid], want, sizeof(want)) != 0;
    x.gds_base = want[0]; x.gds_size = want[1];
    x.gws_base = want[2]; x.gws_size = want[3];
    x.oa_base = want[4]; x.oa_size = want[5];
    /* AMDGPU_CHUNK_ID_CP_GFX_SHADOW: amdgpu_ib_schedule hands them to
     * gfx_v11_0_ring_emit_gfx_shadow, which only a cp_gfx_shadow ring uses */
    x.shadow_va = job->shadow_va;
    x.csa_va = job->csa_va;
    x.gds_va = job->gds_va;
    x.init_shadow = job->init_shadow ? 1 : 0;
    /* rdna3amdgpu=1: amdgpu_vm_flush's pasid_mapping_needed (the VMID's IH
     * LUT entry holds another PASID) */
    if (fAmdgpuExact && v->vmid < 16 && v->pasid && fVmidPasid[v->vmid] != v->pasid) {
        x.pasid_mapping = 1;
        x.pasid = v->pasid;
    }
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ready && fAmdgpuExact) {
        /* rdna3amdgpu=1: amdgpu_ib_schedule on the DMA ring (sdma60_submit_job):
         * amdgpu_vm_flush on the ring's own invalidation engine, the PASID
         * mapping, COND_EXE, HDP flush, padded IBs, user fence, fence */
        uint64_t va[RD_MAX_CS_IBS];
        uint32_t ndw[RD_MAX_CS_IBS], fl[RD_MAX_CS_IBS];
        if (n > RD_MAX_CS_IBS)
            return -RD_EINVAL;
        for (uint32_t i = 0; i < n; i++) {
            va[i] = ibs[i].va;
            ndw[i] = ibs[i].length_dw;
            fl[i] = ibs[i].flags;
        }
        uint32_t s = sdma60_submit_job(fDev, fSdma, v->vmid, v->dmaFlush ? gpuvm_pd_addr(fDev, &v->vm) : 0,
                                       x.pasid_mapping ? x.pasid : 0,
                                       x.pasid_mapping ? gmc11_pasid_mapping_reg(fDev, v->vmid, 0) : 0,
                                       va, ndw, fl, n, ufMc, job->seq_base);
        if (!s)
            return -RD_EBUSY;
        if (x.pasid_mapping)
            fVmidPasid[v->vmid] = x.pasid;
        fSdmaLastVmid = v->vmid;
        v->dmaFlush = false;
        *seq = s;
        return 0;
    }
    if (ip == AMDGPU_HW_IP_DMA && fSdma && fSdma->ready) {
        for (uint32_t i = 0; i < n; i++)
            sdma60_emit_ib(fDev, fSdma, v->vmid, ibs[i].va, ibs[i].length_dw);
        fSdmaLastVmid = v->vmid;
        if (ufMc)   /* amdgpu_ib_schedule: the user fence, then the ring fence */
            sdma60_emit_fence64(fDev, fSdma, ufMc, rdna3_seq_widen(job->seq_base, fSdma->fence_seq + 1));
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
        if (x.pasid_mapping)
            fVmidPasid[v->vmid] = x.pasid;
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
        if (x.pasid_mapping)
            fVmidPasid[v->vmid] = x.pasid;
        memcpy(fGds[v->vmid], want, sizeof(want));
        fGdsKnown[v->vmid] = true;
        v->compFlush = false;
        *seq = s;
        return 0;
    }
    return -RD_EINVAL;
}

/* the newest fence seq of each (IP, ring) the trace has recorded as
 * signalled (card run 2026-10-17b): one FENCE event per newly seen seq */
static uint64_t gTrFenceSeen[16][8];
static int fenceTraced(uint32_t ip, uint32_t ring, uint64_t seq, int done)
{
    if (done && ip < 16 && ring < 8 && seq > gTrFenceSeen[ip][ring]) {
        struct rd3_trace_ev e;
        gTrFenceSeen[ip][ring] = seq;
        bzero(&e, sizeof e);
        e.type = RD3_TR_FENCE;
        e.head = 0xff;
        e.arg = (uint8_t)ip;
        e.flags = (uint8_t)ring;
        e.v2 = (uint32_t)seq;
        e.v3 = (uint32_t)(seq >> 32);
        rdna3TraceCore(&e);
    }
    return done;
}

int RDNA3Bringup::fenceSignaled(uint32_t ip, uint32_t ring, uint64_t seq)
{
    return fenceTraced(ip, ring, seq, fenceSignaledRaw(ip, ring, seq));
}

int RDNA3Bringup::fenceSignaledRaw(uint32_t ip, uint32_t ring, uint64_t seq)
{
    if (ip == AMDGPU_HW_IP_VCN_ENC && fVcn && ring < RD_VCN_MAX_INST && fVcn->ring_enc[ring].ring_size)
        return vcn40_fence_passed(fDev, &fVcn->ring_enc[ring], (uint32_t)seq);
    if (ip == AMDGPU_HW_IP_VCN_JPEG && fVcn && ring == 0 && fVcn->jpeg.ring_size)
        return vcn40_fence_passed(fDev, &fVcn->jpeg, (uint32_t)seq);
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
    uint32_t fbit = ip < 32 ? 1u << ip : 0;
    for (uint32_t spins = 0;; spins++) {
        if (fenceSignaled(ip, ring, seq)) {
            /* the job got through: a fault flag left from before is stale */
            if (fSchedFault & fbit)
                OSBitAndAtomic(~fbit, &fSchedFault);
            return 0;
        }
        absolutetime_to_nanoseconds(mach_continuous_time(), &now);
        if (absNs != UINT64_MAX && now >= absNs) {
            /* card run 2026-10-12: encode/decode jobs never came back while
             * the VCN ring tests passed; the state for the report */
            static SInt32 gVcnDiags;
            if (ip == AMDGPU_HW_IP_VCN_ENC && fVcn && ring < RD_VCN_MAX_INST && now - start >= 1000000000ull &&
                OSIncrementAtomic(&gVcnDiags) < 6)
                vcn40_ring_diag(fDev, fVcn, ring, seq);
            return -RD_ETIME;
        }
        /* drm_sched_fault (rdna3amdgpu=1): a CP fault IV named this ring */
        bool faulted = (fSchedFault & fbit) != 0;
        if (faulted)
            OSBitAndAtomic(~fbit, &fSchedFault);
        if (faulted || now - start >= capNs) {
            if (capHit) {
                *capHit = true;          /* the caller recovers under its lock */
                return -RD_ETIME;
            }
            return recover(ip, ring, seq);   /* amdgpu_job_timedout */
        }
        if (fIrqLive) {
            /* the 5 ms re-check covers a lost interrupt, as
             * amdgpu_fence_schedule_fallback's timer (amdgpu_fence_fallback) */
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

/* a user queue fence wait (RDNA3Accel::drmUserqWait, DRM lock dropped):
 * until the next interrupt, 5 ms at most, or 'absNs'; polls the IH ring
 * without MSI */
void RDNA3Bringup::waitInterrupt(uint64_t absNs)
{
    uint64_t now, ns = 5ull * 1000 * 1000, deadline;
    absolutetime_to_nanoseconds(mach_continuous_time(), &now);
    if (absNs != UINT64_MAX) {
        if (absNs <= now)
            return;
        if (absNs - now < ns)
            ns = absNs - now;
    }
    if (fIrqLive) {
        clock_interval_to_deadline((uint32_t)(ns / 1000) + 1, kMicrosecondScale, &deadline);
        IOLockLock(fLock);
        IOLockSleepDeadline(fLock, &fFenceEvent, deadline, THREAD_UNINT);
        IOLockUnlock(fLock);
        return;
    }
    if (fIh && fIh->enabled && IOLockTryLock(fIhLock)) {
        ih60_process(fDev, fIh, ivCallback, this);
        IOLockUnlock(fIhLock);
    }
    IOSleep(1);
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
    if (b->fAmdgpuExact && b->fPmUp && b->fSmu && smu13_iv_is_smu(iv->client_id, iv->src_id)) {
        b->smuIrq(iv);
        return;
    }
    if (b->fAmdgpuExact) {
        int which = -1;
        if (gfx11_handle_priv_fault(iv, b->fGfx, b->fKcq, &which)) {
            /* gfx_v11_0_priv_reg_irq / _priv_inst_irq / _bad_op_irq */
            static const char *const what[] = { "Illegal opcode in command stream",
                                                "Illegal register access in command stream",
                                                "Illegal instruction in command stream" };
            uint32_t k = iv->src_id - RD_IH_SRC_CP_BAD_OPCODE;
            b->fCpFaults++;
            IOLog("RDNA3: %s (ring_id 0x%x)\n", what[k < 3 ? k : 0], iv->ring_id);
            if (which >= 0) {
                /* drm_sched_fault: the ring's timeout handler runs at the
                 * next fence wait (handleInterrupt wakes the waiters) */
                OSBitOrAtomic(1u << (which ? AMDGPU_HW_IP_COMPUTE : AMDGPU_HW_IP_GFX), &b->fSchedFault);
            }
            return;
        }
    }
    if (gmc11_iv_is_vm_fault(iv)) {
        /* gmc_v11_0_process_interrupt: no retry faults on a dGPU, the wave
         * is gone; the VM that owns the VMID is guilty from now on */
        rdna3_vm_fault f;
        gmc11_decode_vm_fault(b->fDev, iv, &f);
        gmc11_log_vm_fault(b->fDev, &f);
        b->fVmFaults++;
        {
            /* card run 2026-10-17b's trace: the fault with its time and owner */
            struct rd3_trace_ev e;
            bzero(&e, sizeof e);
            e.type = RD3_TR_VMFAULT;
            e.head = 0xff;
            e.arg = (uint8_t)(f.hub ? 1 : 0);
            e.flags = (uint8_t)(f.write ? 1 : 0);
            e.v0 = f.vmid;
            e.v1 = f.cid;
            e.v2 = f.vmid < 16 && b->fVmByVmid[f.vmid] ? (uint32_t)b->fVmByVmid[f.vmid]->ownerPid : 0u;
            e.v3 = f.status;
            e.pa = f.addr;
            rdna3TraceCore(&e);
        }
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
         * re-probe runs on the display side's thread call (N42: only with
         * rdna3hpdirq=1; counted and logged either way) */
        int rx = 0, pin = dchpd_iv_pin(iv->client_id, iv->src_id, iv->src_data[0], &rx);
        if (pin >= 0) {
            if (pin < RD_AUX_ENGINES)
                b->fOwner->displayHpdIrq((uint32_t)pin, rx ? -1 : dchpd_irq_ack(b->fDev, (uint32_t)pin), rx);
            return;
        }
    }
    /* N42: DCN vblank / vupdate / pflip (amdgpu_dm_irq_handler ->
     * dm_crtc_high_irq / dm_pflip_high_irq) */
    if (b->fOwner->displayIrq(iv))
        return;
    if (b->fUserqMask && ih60_iv_userq_doorbell(iv)) {
        /* gfx_v11_0_eop_irq / sdma_v6_0_process_fence_irq for a user queue:
         * the fence value is in its seq64 slot, the waiters (woken after
         * every interrupt) read it there */
        b->fIrqUserq++;
        return;
    }
    if (b->fVcn) {
        /* vcn_v4_0_process_interrupt / jpeg_v4_0_process_interrupt: the
         * waiters read the fences after every interrupt */
        uint32_t inst = 0;
        if (vcn40_process_interrupt(b->fVcn, iv->client_id, iv->src_id, &inst)) {
            b->fIrqVcn++;
            return;
        }
        if (jpeg40_process_interrupt(b->fVcn, iv->client_id, iv->src_id)) {
            b->fIrqJpeg++;
            return;
        }
    }
    switch (ih60_iv_fence(iv)) {
    case RD_IH_FENCE_GFX:   b->fIrqFenceGfx++; break;
    case RD_IH_FENCE_SDMA0: b->fIrqFenceSdma++; break;
    case RD_IH_FENCE_COMPUTE0: b->fIrqFenceComp++; break;
    default:
        b->fIvOther[b->fIvOtherN & 3] = *iv;
        b->fIvOtherN++;
        b->fIrqOther++;
        break;
    }
}

/* amdgpu_ih_ring_init(use_bus_addr false) for rings 0 and 1: system pages
 * (44-bit DMA like the GTT BOs), each ring and its 8-byte wptr / rptr
 * shadows bound to the GART from offset 0 with gart_pte_flags (MTYPE_UC,
 * EXECUTABLE) + SYSTEM | SNOOPED (cached TT) */
bool RDNA3Bringup::ihSysAlloc()
{
    const uint64_t ringPages = RD_IH_RING_SIZE / 4096 + 1;
    const uint64_t flags = RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                           RD_PTE_WRITEABLE | RD_PTE_EXECUTABLE | RD_PTE_MTYPE_NV10(RD_MTYPE_UC);
    if (!fGmc || !fGmc->mmhub_enabled || fGmc->gart_size < 2 * ringPages * 4096)
        return false;
    if (!fIh1)
        fIh1 = (rdna3_ih *)IOMalloc(sizeof(*fIh1));
    if (!fIh1)
        return false;
    bzero(fIh1, sizeof(*fIh1));
    for (uint32_t r = 0; r < 2; r++) {
        IhSys *b = &fIhSys[r];
        if (!b->md) {
            b->md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut,
                                                                     ringPages * 4096, (1ull << 44) - 4096);
            if (!b->md)
                goto fail;
            b->dma = IODMACommand::withSpecification(IODMACommand::OutputHost64, 44, 0,
                                                     IODMACommand::kMapped, 0, 4096);
            if (!b->dma || b->dma->setMemoryDescriptor(b->md, true) != kIOReturnSuccess)
                goto fail;
            uint64_t off = 0, page = 0;
            while (page < ringPages) {
                IODMACommand::Segment64 seg[8];
                UInt32 n = 8;
                if (b->dma->gen64IOVMSegments(&off, seg, &n) != kIOReturnSuccess || !n)
                    goto fail;
                for (UInt32 i = 0; i < n; i++)
                    for (uint64_t x = 0; x < seg[i].fLength && page < ringPages; x += 4096)
                        b->pages[page++] = seg[i].fIOVMAddr + x;
            }
        }
        bzero(b->md->getBytesNoCopy(), (size_t)(ringPages * 4096));
        for (uint64_t p = 0; p < ringPages; p++)
            if (gmc11_gart_map(fDev, fGmc, (r * ringPages + p) * 4096, b->pages[p], 1, flags) != RDNA3_OK)
                goto fail;
    }
    vmFlush(0);
    return true;
fail:
    BLOG("IH: no system memory rings, back to VRAM");
    ihSysFree();
    return false;
}

/* amdgpu_ih_ring_fini for the GTT rings (unbound from the GART first) */
void RDNA3Bringup::ihSysFree()
{
    const uint64_t ringPages = RD_IH_RING_SIZE / 4096 + 1;
    if (fGmc && fDev && (fIhSys[0].md || fIhSys[1].md)) {
        gmc11_gart_unmap(fDev, fGmc, 0, (uint32_t)(2 * ringPages));
        vmFlush(0);
    }
    for (uint32_t r = 0; r < 2; r++) {
        if (fIhSys[r].dma) {
            fIhSys[r].dma->clearMemoryDescriptor(true);
            fIhSys[r].dma->release();
        }
        OSSafeReleaseNULL(fIhSys[r].md);
        fIhSys[r].dma = nullptr;
    }
    if (fIh1) {
        IOFree(fIh1, sizeof(*fIh1));
        fIh1 = nullptr;
    }
    if (fIh)
        fIh->ih1 = nullptr;
}

/* smu_v13_0_irq_process from the IV callback (fIhLock held): the hardware
 * CTF right away, the rest from fSmuIrqCall under the DRM lock, because
 * it may use the PMFW mailbox */
void RDNA3Bringup::smuIrq(const rdna3_iv *iv)
{
    uint64_t deadline;
    uint32_t delay = 0;
    fSmuIrqs++;
    if (iv->client_id == RD_IH_CLIENT_ROM_SMUIO) {
        if (smu13_irq_process(fDev, fSmu, iv->client_id, iv->src_id, 0) == RD_SMU_IRQ_HWCTF)
            overTemp("GPU HW CTF");
        return;
    }
    if (fSmuIrqN < kSmuIvMax) {
        fSmuIrqQ[fSmuIrqN].client = iv->client_id;
        fSmuIrqQ[fSmuIrqN].src = iv->src_id;
        fSmuIrqQ[fSmuIrqN].ctx = iv->src_data[0];
        fSmuIrqN++;
    }
    if (!fSmuIrqCall || fSmuIrqStop)
        return;
    if (iv->client_id == RD_IH_CLIENT_THM && iv->src_id == RD_THM_SRCID_DIG_THERM_L2H)
        delay = 50;   /* AMDGPU_SWCTF_EXTRA_DELAY */
    clock_interval_to_deadline(delay, kMillisecondScale, &deadline);
    thread_call_enter_delayed(fSmuIrqCall, deadline);
}

void RDNA3Bringup::smuIrqTick(void *owner, void *)
{
    RDNA3Bringup *b = (RDNA3Bringup *)owner;
    IOLock *l = b->fOwner->drmLock();
    SmuIv q[kSmuIvMax];
    uint32_t n, i, hot = 0;
    bool swctf = false;
    if (!l || !b->fIhLock)
        return;
    IOLockLock(l);
    IOLockLock(b->fIhLock);
    n = b->fSmuIrqN;
    for (i = 0; i < n; i++)
        q[i] = b->fSmuIrqQ[i];
    b->fSmuIrqN = 0;
    IOLockUnlock(b->fIhLock);
    for (i = 0; i < n && !b->fSmuIrqStop && b->fPmUp && b->fSmu; i++) {
        int r = smu13_irq_process(b->fDev, b->fSmu, q[i].client, q[i].src, q[i].ctx);
        if (r == RD_SMU_IRQ_SWCTF)
            swctf = true;
        else if (r == RD_SMU_IRQ_THROTTLE)
            b->publish("RDNA3,PMThrottleInterrupts", b->fSmu->throttle_int_counter);
    }
    /* smu_swctf_delayed_work_handler */
    if (swctf && !b->fSmuIrqStop && b->fPmUp && b->fSmu &&
        smu13_swctf_check(b->fDev, b->fSmu, &hot))
        b->overTemp("GPU SW CTF");
    b->publish("RDNA3,PMSmuInterrupts", b->fSmuIrqs);
    IOLockUnlock(l);
}

/* orderly_poweroff(true) on a critical temperature: the system's thermal
 * emergency */
void RDNA3Bringup::overTemp(const char *why)
{
    BLOG("%s: asking the system to power down (thermal emergency)", why);
    if (IOPMrootDomain *root = fOwner->getPMRootDomain())
        root->receivePowerNotification(kIOPMOverTemp);
}

/* amdgpu_pci_shutdown: mp1_state UNLOAD through amdgpu_device_ip_suspend,
 * i.e. the SMC block's smu_smc_hw_cleanup (thermal alert off) and
 * smu_v13_0_0_set_mp1_state (PrepareMp1ForUnload 0x55); nothing in a
 * passthrough VM (amdgpu_passthrough: the CPUID hypervisor bit). The
 * other blocks are not suspended first: the power goes next. */
void RDNA3Bringup::pmShutdown()
{
    uint32_t ecx = 0;
    if (!fAmdgpuExact || !fPmUp || !fSmu || !fDev)
        return;
#if defined(__x86_64__)
    {
        uint32_t eax = 1, ebx = 0, edx = 0;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    }
#endif
    if (ecx & (1u << 31)) {
        BLOG("shutdown: under a hypervisor, MP1 state left as is");
        return;
    }
    if (fSmu->irq_enabled)
        smu13_disable_thermal_alert(fDev, fSmu);
    int r = smu13_set_mp1_state(fDev, fSmu, RD_PP_MP1_STATE_UNLOAD);
    BLOG("shutdown: MP1 state UNLOAD (PrepareMp1ForUnload 0x55) -> %d", r);
}

void RDNA3Bringup::handleInterrupt()
{
    if (!fIh || !fDev)
        return;
    IOLockLock(fIhLock);
    uint32_t n = ih60_process(fDev, fIh, ivCallback, this);
    IOLockUnlock(fIhLock);
    /* amdgpu_fence_process for the rings: the waiters re-read their fence
     * slots (fenceSignaled) once woken */
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
