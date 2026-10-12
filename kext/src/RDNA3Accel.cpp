/*
 * RDNA3Accel.cpp - Stage 1, see RDNA3Accel.hpp for the safety levels.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"
#include "RDNA3Regs.h"
#include "RDNA3Bringup.hpp"
#include <IOKit/pwr_mgt/RootDomain.h>

#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>
#include "rdna3_bootargs.h"

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

#define super IOService
OSDefineMetaClassAndStructors(RDNA3Accel, IOService)

/* ------------------------------------------------------------------ */

IOService *RDNA3Accel::probe(IOService *provider, SInt32 *score)
{
    char dummy[8];
    if (PE_parse_boot_argn("-rdna3off", dummy, sizeof(dummy))) {
        LOG("disabled by -rdna3off");
        return nullptr;
    }
    return super::probe(provider, score);
}

bool RDNA3Accel::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) {
        LOG("provider is not an IOPCIDevice");
        return false;
    }
    fPci->retain();

    fLevel = rd3_arg("rdna3lvl", 0);   /* 8 by default: rdna3_bootargs.h */
    setNumber("RDNA3,Level", fLevel);
    LOG("start, safety level %u, build %s (%s)", fLevel, KEXT_COMMIT, KEXT_VERSION);
    /* the framebuffer publishes its own: a mismatch means one of the two
     * kexts is a stale copy (the Auxiliary KC keeps the last setup.sh one) */
    setProperty("RDNA3,Commit", KEXT_COMMIT);

    readConfigSpace();

    if (fLevel >= 1) {
        if (!isInD0()) {
            LOG("device not in D0, refusing to touch MMIO");
        } else if (mapMmio() && waitForIfwi() && readVramSize() && fLevel >= 2) {
            amd_discovery *disc = (amd_discovery *)IOMalloc(sizeof(*disc));
            if (disc) {
                bool ok = readDiscovery(disc);
                if (ok && fLevel >= 3)
                    readGcStatus(disc);
                /* Stage 3: which heads the firmware lit (reads only) - before
                 * the heaps, so they start above the boot scanout */
                if (ok && !displayInit(disc))
                    LOG("display heads unknown");
                if (ok && !drmInit(disc))
                    LOG("DRM/amdgpu UAPI not available");
                else if (ok && !platformInit())
                    LOG("accelerator interface not available");
                if (ok && fLevel >= 4) {
                    /* no suspend/resume yet: after a sleep the GPU has
                     * lost the firmware and the GOP's display, and only a
                     * modeset of our own could bring the picture back. Keep
                     * the machine from idle sleep while the bring-up runs
                     * (rdna3sleep=1 allows it, to test) */
                    uint32_t sleepOk = 0;
                    PE_parse_boot_argn("rdna3sleep", &sleepOk, sizeof(sleepOk));
                    IOPMrootDomain *root = getPMRootDomain();
                    if (!sleepOk && root) {
                        fSleepAssertion = root->createPMAssertion(kIOPMDriverAssertionPreventSystemIdleSleepBit,
                                                                  kIOPMDriverAssertionLevelOn, this,
                                                                  "RDNA3Accel: no GPU suspend/resume yet");
                        setNumber("RDNA3,IdleSleepBlocked", fSleepAssertion ? 1 : 0);
                    }
                    fBringup = new RDNA3Bringup;
                    if (fBringup && fBringup->init(this, fPci, fMmioMap, fLevel, disc, fVramSize)) {
                        fBringup->start();
                        statsStart();   /* idle until the SMU is up */
                    } else {
                        LOG("cannot set up Stage 2 bring-up");
                        if (fBringup) {
                            fBringup->stopAndFree();
                            delete fBringup;
                            fBringup = nullptr;
                        }
                    }
                }
                IOFree(disc, sizeof(*disc));
            }
        }
    }

    /* in the power tree only to hear about shutdown and restart */
    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    PMinit();
    provider->joinPMtree(this);
    fPmJoined = registerPowerDriver(this, states, 2) == kIOReturnSuccess;

    registerService();
    LOG("started");
    return true;
}

IOReturn RDNA3Accel::setPowerState(unsigned long state, IOService *)
{
    /* D4: system sleep and wake (RDNA3Resume.cpp); the wake work runs on
     * a thread call */
    bool vcn = fBringup && fBringup->finished() && fBringup->vcnWanted() && fDrmLock;
    bool s3 = fBringup && fBringup->finished() && fDrmLock && rd3_arg("rdna3resume", 0) && !fShuttingDown;
    if (state == 0) {
        displayWillSleep();
        /* amdgpu_device_suspend: the display first (phase 1), then the
         * other IPs in reverse order, JPEG and VCN among them (rdna3vcn);
         * with rdna3resume=1 all of them (P4), else the video block only */
        if (s3) {
            IOLockLock(fDrmLock);
            fBringup->systemSuspend();
            IOLockUnlock(fDrmLock);
        } else if (vcn) {
            IOLockLock(fDrmLock);
            fBringup->vcnSuspend();
            IOLockUnlock(fDrmLock);
        }
    } else if (fDispAsleep) {
        displayDidWake();   /* the wake work resumes the GPU and VCN / JPEG */
    } else if (fBringup && fBringup->suspended() && fDrmLock) {
        /* suspended with no display wake work (no heads): the resume here */
        IOLockLock(fDrmLock);
        bool ok = fBringup->resumeAfterPowerLoss();
        drmVmReplayIfNeeded();
        IOLockUnlock(fDrmLock);
        LOG("wake: GPU resume after the suspend %s", ok ? "done" : "FAILED");
    } else if (vcn) {
        /* no display wake work: VCN / JPEG straight away (vcnResume does
         * nothing when the PSP sOS is gone, i.e. the card lost its power) */
        IOLockLock(fDrmLock);
        fBringup->vcnResume();
        IOLockUnlock(fDrmLock);
    }
    return kIOPMAckImplied;
}

void RDNA3Accel::systemWillShutdown(IOOptionBits specifier)
{
    LOG("system %s: display back to the boot head", specifier == kIOMessageSystemWillRestart ? "restart" : "power off");
    fShuttingDown = true;   /* D4: the power-off is no sleep */
    if (fBringup && fBringup->finished() && fBringup->vcnWanted())
        fBringup->vcnShutdown();
    displayShutdown();
    if (fBringup && fDrmLock) {
        IOLockLock(fDrmLock);
        fBringup->pmShutdown();   /* rdna3amdgpu=1 only */
        IOLockUnlock(fDrmLock);
    }
    super::systemWillShutdown(specifier);
}

void RDNA3Accel::stop(IOService *provider)
{
    LOG("stop");
    if (fPmJoined) {
        PMstop();
        fPmJoined = false;
    }
    statsStop();
    displayStateFree();   /* D4: the wake work first */
    if (fSleepAssertion) {
        if (IOPMrootDomain *root = getPMRootDomain())
            root->releasePMAssertion(fSleepAssertion);
        fSleepAssertion = 0;
    }
    if (fBringup) {
        fBringup->stopAndFree();
        delete fBringup;
        fBringup = nullptr;
    }
    platformFree();
    drmFree();
    displayFree();
    OSSafeReleaseNULL(fMmioMap);
    fMmio = nullptr;
    fMmioLen = 0;
    OSSafeReleaseNULL(fPci);
    super::stop(provider);
}

void RDNA3Accel::free()
{
    if (fIdxLock) {
        IOSimpleLockFree(fIdxLock);
        fIdxLock = nullptr;
    }
    super::free();
}

void RDNA3Accel::setNumber(const char *key, uint64_t val, unsigned bits)
{
    OSNumber *n = OSNumber::withNumber(val, bits);
    if (n) {
        setProperty(key, n);
        n->release();
    }
}

/* ---------------------------- level 0 ----------------------------- */

void RDNA3Accel::readConfigSpace()
{
    uint16_t vendor = fPci->configRead16(kIOPCIConfigVendorID);
    uint16_t device = fPci->configRead16(kIOPCIConfigDeviceID);
    uint8_t  rev    = fPci->configRead8(kIOPCIConfigRevisionID);
    uint16_t svid   = fPci->configRead16(kIOPCIConfigSubSystemVendorID);
    uint16_t sdid   = fPci->configRead16(kIOPCIConfigSubSystemID);
    uint16_t cmd    = fPci->configRead16(kIOPCIConfigCommand);

    LOG("PCI %04x:%04x rev %02x subsys %04x:%04x cmd 0x%04x", vendor, device,
        rev, svid, sdid, cmd);
    setNumber("RDNA3,VendorID", vendor, 16);
    setNumber("RDNA3,DeviceID", device, 16);
    setNumber("RDNA3,Revision", rev, 8);
    setNumber("RDNA3,SubsystemVendorID", svid, 16);
    setNumber("RDNA3,SubsystemID", sdid, 16);

    uint32_t bar5 = fPci->configRead32(RDNA3_MMIO_BAR_CFG);
    LOG("BAR5 (MMIO) raw 0x%08x, memory decode %s, bus master %s", bar5,
        (cmd & kIOPCICommandMemorySpace) ? "on" : "off",
        (cmd & kIOPCICommandBusMaster) ? "on" : "off");

    UInt8 pcie = 0;
    if (fPci->findPCICapability(kIOPCIPCIExpressCapability, &pcie) && pcie) {
        uint16_t lnksta = fPci->configRead16(pcie + 0x12);
        unsigned speed = lnksta & 0xf, width = (lnksta >> 4) & 0x3f;
        LOG("PCIe link Gen%u x%u (behind the Navi switch this is the "
            "internal upstream link)", speed, width);
        setNumber("RDNA3,LinkSpeed", speed, 8);
        setNumber("RDNA3,LinkWidth", width, 8);
    }
}

bool RDNA3Accel::isInD0()
{
    UInt8 pm = 0;
    if (!fPci->findPCICapability(kIOPCIPowerManagementCapability, &pm) || !pm)
        return true; /* no PM capability -> always D0 */
    uint16_t pmcsr = fPci->configRead16(pm + 4);
    LOG("PMCSR 0x%04x (D%u)", pmcsr, pmcsr & 3);
    return (pmcsr & 3) == 0;
}

/* ---------------------------- level 1 ----------------------------- */

bool RDNA3Accel::mapMmio()
{
    fIdxLock = IOSimpleLockAlloc();
    if (!fIdxLock)
        return false;

    /* Memory decode is needed for MMIO. Bus mastering stays untouched. */
    fPci->setMemoryEnable(true);

    fMmioMap = fPci->mapDeviceMemoryWithRegister(RDNA3_MMIO_BAR_CFG,
                                                 kIOMapInhibitCache);
    if (!fMmioMap) {
        LOG("failed to map BAR5");
        return false;
    }
    fMmio    = (volatile uint8_t *)fMmioMap->getVirtualAddress();
    fMmioLen = fMmioMap->getLength();
    LOG("BAR5 phys 0x%llx len 0x%llx", (unsigned long long)fMmioMap->getPhysicalAddress(),
        (unsigned long long)fMmioLen);
    setNumber("RDNA3,MMIOLength", fMmioLen, 64);
    return fMmio != nullptr;
}

bool RDNA3Accel::regValid(uint64_t byteOff) const
{
    return fMmio && byteOff + 4 <= fMmioLen;
}

uint32_t RDNA3Accel::rreg(uint32_t dw)
{
    uint64_t off = REG_BYTES(dw);
    if (!regValid(off))
        return ~0u;
    return *(volatile uint32_t *)(fMmio + off);
}

bool RDNA3Accel::waitForIfwi()
{
    /* amdgpu waits for the PSP/IFWI to signal completion before reading
     * VRAM size; usually done long before boot, can take up to 2 s. */
    if (!regValid(REG_BYTES(mmMP0_SMN_C2PMSG_33))) {
        LOG("C2PMSG_33 outside BAR5, skipping IFWI wait");
        return true;
    }
    uint32_t msg = 0;
    for (uint32_t i = 0; i < RDNA3_IFWI_WAIT_MS; i++) {
        msg = rreg(mmMP0_SMN_C2PMSG_33);
        if (msg == ~0u) {
            LOG("MMIO reads all-ones: device not responding, stopping here");
            return false;
        }
        if (msg & C2PMSG_33_IFWI_DONE)
            break;
        IOSleep(1);
    }
    LOG("C2PMSG_33 0x%08x (IFWI %s)", msg,
        (msg & C2PMSG_33_IFWI_DONE) ? "ready" : "NOT ready");
    setNumber("RDNA3,C2PMSG33", msg);
    return (msg & C2PMSG_33_IFWI_DONE) != 0;
}

/* soc21_get_config_memsize -> nbio_v4_3_get_memsize: RCC_CONFIG_MEMSIZE in MiB */
bool RDNA3Accel::readVramSize()
{
    uint32_t mb = rreg(mmRCC_CONFIG_MEMSIZE);
    if (mb == 0 || mb == ~0u) {
        LOG("RCC_CONFIG_MEMSIZE = 0x%08x, VRAM not usable (not posted?)", mb);
        return false;
    }
    fVramSize = (uint64_t)mb << 20;
    LOG("VRAM %u MiB", mb);
    setNumber("RDNA3,VRAMSizeMB", mb);
    return true;
}

/* ---------------------------- level 2 ----------------------------- */

/* Indirect VRAM read, same sequence as amdgpu_device_mm_access(). */
void RDNA3Accel::vramRead(uint64_t pos, uint32_t *buf, uint32_t dwords)
{
    const uint32_t chunk = 64; /* keep interrupts-off windows short */
    uint32_t done = 0;

    while (done < dwords) {
        uint32_t n = dwords - done < chunk ? dwords - done : chunk;
        uint32_t hi = ~0u;
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fIdxLock);
        for (uint32_t i = 0; i < n; i++, pos += 4) {
            uint32_t tmp = (uint32_t)(pos >> 31);
            *(volatile uint32_t *)(fMmio + REG_BYTES(mmMM_INDEX)) =
                (uint32_t)pos | MM_INDEX_APERTURE_VRAM;
            if (tmp != hi) {
                *(volatile uint32_t *)(fMmio + REG_BYTES(mmMM_INDEX_HI)) = tmp;
                hi = tmp;
            }
            buf[done + i] = *(volatile uint32_t *)(fMmio + REG_BYTES(mmMM_DATA));
        }
        IOSimpleLockUnlockEnableInterrupt(fIdxLock, is);
        done += n;
    }
}

/* amdgpu_discovery_read_binary_from_mem: after the IFWI's C2PMSG_33 bit
 * (waitForIfwi), the binary at the top of VRAM (RCC_CONFIG_MEMSIZE - 64 KiB) */
bool RDNA3Accel::readDiscovery(amd_discovery *disc)
{
    const uint32_t size = AMD_DISC_TMR_SIZE;
    uint64_t offset = fVramSize - AMD_DISC_TMR_OFFSET;
    uint32_t sig = 0;

    vramRead(offset, &sig, 1);
    if (sig != AMD_DISC_BINARY_SIGNATURE) {
        /* Newer IFWI publishes the location through the PSP scratch regs. */
        uint32_t tmrSize = rreg(mmDRIVER_SCRATCH_2);
        if (tmrSize && tmrSize != ~0u) {
            offset = ((uint64_t)rreg(mmDRIVER_SCRATCH_1) << 32) |
                     rreg(mmDRIVER_SCRATCH_0);
            LOG("discovery not at default offset, scratch says 0x%llx",
                (unsigned long long)offset);
            if (offset + size > fVramSize) {
                LOG("scratch offset outside VRAM, giving up");
                return false;
            }
        } else {
            LOG("no discovery signature (got 0x%08x)", sig);
            return false;
        }
    }

    uint32_t *blob = (uint32_t *)IOMalloc(size);
    if (!blob)
        return false;
    vramRead(offset, blob, size / 4);

    amd_disc_status st = amd_discovery_parse((const uint8_t *)blob, size, disc);
    LOG("IP discovery at VRAM 0x%llx: %s", (unsigned long long)offset,
        amd_disc_status_str(st));
    if (st == AMD_DISC_OK)
        publishDiscovery(disc, (const uint8_t *)blob, disc->bin_size);
    if (st == AMD_DISC_OK) {
        /* amdgpu_discovery_set_ip_blocks' default case after reg_base_init:
         * harvest_ip, get_mall_info, get_vcn_info */
        uint16_t device = fPci->configRead16(kIOPCIConfigDeviceID);
        uint8_t rev = fPci->configRead8(kIOPCIConfigRevisionID);
        fDiscSocOk = amd_discovery_soc_config((const uint8_t *)blob, size, disc, device, rev, &fDiscSoc) ==
                     AMD_DISC_OK;
        if (fDiscSocOk) {
            setNumber("RDNA3,NumUMC", fDiscSoc.num_umc);
            setNumber("RDNA3,MALLSizeMB", fDiscSoc.mall_size >> 20);
            setNumber("RDNA3,HarvestIPMask", fDiscSoc.harvest_ip_mask);
            setNumber("RDNA3,VCNHarvestConfig", fDiscSoc.vcn_harvest_config);
            LOG("discovery: %u UMC (active 0x%x), MALL %llu MiB, %u VCN (mask 0x%x, harvest 0x%x, "
                "codec-disable 0x%x/0x%x), %u SDMA, harvested IPs 0x%x",
                fDiscSoc.num_umc, fDiscSoc.umc_active_mask, (unsigned long long)(fDiscSoc.mall_size >> 20),
                fDiscSoc.num_vcn_inst, fDiscSoc.vcn_inst_mask, fDiscSoc.vcn_harvest_config,
                fDiscSoc.vcn_codec_disable_mask[0], fDiscSoc.vcn_codec_disable_mask[1], fDiscSoc.num_sdma,
                fDiscSoc.harvest_ip_mask);
        }
    }
    IOFree(blob, size);
    return st == AMD_DISC_OK;
}

void RDNA3Accel::publishDiscovery(const amd_discovery *disc,
                                  const uint8_t *blob, uint32_t len)
{
    /* Raw blob: tools/macos/ioreg_discovery.sh extracts it for
     * build/host/amd_discovery_dump, to diff against Linux's debugfs copy. */
    OSData *data = OSData::withBytes(blob, len);
    if (data) {
        setProperty("RDNA3,DiscoveryBinary", data);
        data->release();
    }

    OSArray *arr = OSArray::withCapacity(disc->num_ips);
    if (!arr)
        return;
    for (uint32_t i = 0; i < disc->num_ips; i++) {
        const amd_ip_entry *e = &disc->ips[i];
        const char *name = amd_hwid_name(e->hw_id);
        char ver[32];
        snprintf(ver, sizeof(ver), "%u.%u.%u", e->major, e->minor, e->revision);

        LOG("  die %u %-8s inst %u v%s base0 0x%08x", e->die, name ? name : "?",
            e->instance, ver, e->num_base_kept ? e->base[0] : 0);

        OSDictionary *d = OSDictionary::withCapacity(5);
        if (!d)
            continue;
        OSString *s;
        if ((s = OSString::withCString(name ? name : "?"))) {
            d->setObject("name", s);
            s->release();
        }
        if ((s = OSString::withCString(ver))) {
            d->setObject("version", s);
            s->release();
        }
        OSNumber *n;
        if ((n = OSNumber::withNumber(e->hw_id, 16))) {
            d->setObject("hwid", n);
            n->release();
        }
        if ((n = OSNumber::withNumber(e->instance, 8))) {
            d->setObject("instance", n);
            n->release();
        }
        OSData *bases = OSData::withBytes(e->base, e->num_base_kept * sizeof(uint32_t));
        if (bases) {
            d->setObject("bases", bases);
            bases->release();
        }
        arr->setObject(d);
        d->release();
    }
    setProperty("RDNA3,IPDiscovery", arr);
    arr->release();

    const amd_ip_entry *gc = amd_discovery_find(disc, AMD_HWID_GC, 0);
    if (gc && !(gc->major == 11 && gc->minor == 0 && gc->revision == 3))
        LOG("note: GC is %u.%u.%u, Navi 32 is expected to be 11.0.3",
            gc->major, gc->minor, gc->revision);
}

/* ---------------------------- level 3 ----------------------------- */

void RDNA3Accel::readGcStatus(const amd_discovery *disc)
{
    const amd_ip_entry *gc = amd_discovery_find(disc, AMD_HWID_GC, 0);
    if (!gc || gc->num_base_kept <= regGRBM_STATUS_BASE_IDX) {
        LOG("no GC base in discovery");
        return;
    }
    uint32_t dw = gc->base[regGRBM_STATUS_BASE_IDX] + regGRBM_STATUS;
    if (!regValid(REG_BYTES(dw))) {
        LOG("GRBM_STATUS 0x%x outside BAR5", dw);
        return;
    }
    uint32_t v = rreg(dw);
    LOG("GRBM_STATUS 0x%08x (GUI %s)", v,
        (v & GRBM_STATUS__GUI_ACTIVE) ? "active" : "idle");
    setNumber("RDNA3,GRBM_STATUS", v);
}
