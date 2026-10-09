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
 *   4  + Stage 2 bring-up on a kernel thread (RDNA3Bringup): BAR0, PSP
 *      bootloader/sOS, TMR, all IP firmware via PSP, RLC autoload.
 *   5  + SMU mailbox hello.
 *   6  + GMC: GART table in VRAM, MMHUB 3.0.2 GART enable + TLB flush;
 *        IH 6.0 interrupt ring (polled).
 *   7  + SDMA0 ring, ring test, 64 KiB VRAM copy verified by the CPU.
 *  -rdna3off  disables the driver entirely (probe fails).
 *
 * Levels 0-3 never write anything except MM_INDEX/MM_INDEX_HI (the indirect
 * VRAM read window, which is what amdgpu does as its very first access).
 * Levels 4+ program the GPU (PSP, firmware) - test them only with a second
 * GPU driving the display. Bus mastering is never enabled: PSP and the
 * firmware buffers all live in VRAM.
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
#include "rdna3_drm.h"
#include "range_alloc.h"
}

class RDNA3Bringup;

class RDNA3Accel : public IOService {
    OSDeclareDefaultStructors(RDNA3Accel)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;

    /* Stage 4: DRM/amdgpu UAPI for RDNA3UserClient (RDNA3Drm.cpp) */
    bool  drmAvailable() const { return fDrm != nullptr; }
    int   drmOpen(rdna3_drm_file *f);
    void  drmClose(rdna3_drm_file *f);
    int   drmIoctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size);
    IOMemoryDescriptor *drmMemoryForOffset(rdna3_drm_file *f, uint64_t off,
                                           IOOptionBits *options);

    /* Stage 7: requests from the accelerator kext (RDNA3Platform.cpp) */
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2,
                                          void *p3, void *p4) override;

private:
    IOPCIDevice  *fPci      = nullptr;
    IOMemoryMap  *fMmioMap  = nullptr;
    volatile uint8_t *fMmio = nullptr;
    uint64_t      fMmioLen  = 0;
    IOSimpleLock *fIdxLock  = nullptr;
    uint32_t      fLevel    = 0;
    uint64_t      fVramSize = 0;
    RDNA3Bringup *fBringup  = nullptr;

    /* Stage 4 */
    rdna3_drm_device *fDrm  = nullptr;
    IOLock       *fDrmLock  = nullptr;
    range_alloc  *fVramHeap = nullptr;
    IOMemoryMap  *fVramMap  = nullptr;   /* BAR0, for BO clears / kernel access */
    IODeviceMemory *fVramDev = nullptr;  /* BAR0 descriptor for user mappings */
    bool          drmInit(const amd_discovery *disc);
    void          drmFree();
    static void  *drmZalloc(void *ctx, size_t n);
    static void   drmFreeMem(void *ctx, void *p, size_t n);
    static int    drmBoAlloc(void *ctx, rdna3_bo *bo);
    static void   drmBoFree(void *ctx, rdna3_bo *bo);
    static int    drmCopyin(void *ctx, void *dst, uint64_t uaddr, size_t len);
    static int    drmCopyout(void *ctx, uint64_t uaddr, const void *src, size_t len);
    static void  *drmVmCreate(void *ctx);
    static void   drmVmDestroy(void *ctx, void *vm);
    static int    drmVmMap(void *ctx, void *vm, rdna3_bo *bo, uint64_t va, uint64_t off,
                           uint64_t size, uint32_t flags);
    static void   drmVmUnmap(void *ctx, void *vm, uint64_t va, uint64_t size);

    /* Stage 7 */
    range_alloc  *fAccelVa = nullptr;
    uint64_t      fAccelVramBytes = 0;
    bool          platformInit();
    void          platformFree();
    IOReturn      platformVramAlloc(struct RD3KVramRequest *r);
    IOReturn      platformVramFree(struct RD3KVramRequest *r);
    IOReturn      platformGpuVa(int op, struct RD3KGpuVaRequest *g);

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
