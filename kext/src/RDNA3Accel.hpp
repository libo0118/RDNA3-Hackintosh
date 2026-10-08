/*
 * RDNA3Accel - Stage 1: recognise a Navi 32 (RX 7800 XT) and read it safely.
 *
 * What it does, gated by the boot-arg  rdna3lvl=<n>  (default 0):
 *   0  match + PCI config space only (vendor/device/rev, link, power state).
 *      Never touches a BAR. Safe on any machine.
 *   1  + map BAR5 (MMIO), wait for IFWI ready, read VRAM size.
 *   2  + read the IP discovery binary from the top of VRAM via MM_INDEX/
 *      MM_DATA, validate and decode it, publish it in the IORegistry.
 *   3  + read GC GRBM_STATUS through the discovered GC register base.
 *
 *  -rdna3off  disables the driver entirely (probe fails).
 *
 * The driver NEVER writes anything except MM_INDEX/MM_INDEX_HI (the indirect
 * VRAM read window, which is what amdgpu does as its very first access) and
 * never enables bus mastering: there is no DMA in Stage 1.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_ACCEL_HPP
#define RDNA3_ACCEL_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

extern "C" {
#include "amd_discovery.h"
}

class RDNA3Accel : public IOService {
    OSDeclareDefaultStructors(RDNA3Accel)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;

private:
    IOPCIDevice  *fPci      = nullptr;
    IOMemoryMap  *fMmioMap  = nullptr;
    volatile uint8_t *fMmio = nullptr;
    uint64_t      fMmioLen  = 0;
    IOSimpleLock *fIdxLock  = nullptr;
    uint32_t      fLevel    = 0;
    uint64_t      fVramSize = 0;

    /* level 0 */
    void      readConfigSpace();
    bool      isInD0();

    /* level 1 */
    bool      mapMmio();
    bool      regValid(uint64_t byteOff) const;
    uint32_t  rreg(uint32_t dw);          /* returns ~0u when out of range */
    bool      waitForIfwi();
    bool      readVramSize();

    /* level 2 */
    void      vramRead(uint64_t pos, uint32_t *buf, uint32_t dwords);
    bool      readDiscovery(amd_discovery *disc);
    void      publishDiscovery(const amd_discovery *disc,
                               const uint8_t *blob, uint32_t len);

    /* level 3 */
    void      readGcStatus(const amd_discovery *disc);

    void      setNumber(const char *key, uint64_t val, unsigned bits = 32);
};

#endif /* RDNA3_ACCEL_HPP */
