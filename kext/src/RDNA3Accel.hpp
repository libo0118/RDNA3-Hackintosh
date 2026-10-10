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
 *   5  + SMU mailbox hello; rdna3pm=1 (default) reads the VBIOS and brings
 *        up the PMFW power management (PPTable, GFXCLK/SOCCLK DPM,
 *        throttlers, fan, thermal CTF). rdna3pm=0: boot clocks only;
 *        rdna3pm=2: amdgpu's full feature set incl. UCLK/FCLK/DCN DPM;
 *        rdna3pmmask=<hex>: allowed feature bits directly.
 *   6  + GMC: GART table in VRAM, MMHUB 3.0.2 GART enable + TLB flush;
 *        IH 6.0 interrupt ring (polled).
 *   7  + SDMA0 ring, ring test, 64 KiB VRAM copy verified by the CPU.
 *   8  + GFX 11: RS64, GFXHUB, clear state, gfx ring 0 + ring test; with
 *        doorbells the KIQ and the first compute queue (MEC) + ring tests.
 *  rdna3doorbell=0 keeps the rings on their wptr registers (no compute).
 *  rdna3msi=1 drains the interrupt ring from the device's MSI (level 6+)
 *             and wakes fence waiters from it instead of polling.
 *  -rdna3off  disables the driver entirely (probe fails).
 *
 * Levels 0-3 never write anything except MM_INDEX/MM_INDEX_HI (the indirect
 * VRAM read window, which is what amdgpu does as its very first access).
 * Levels 4+ program the GPU (PSP, firmware) - test them with SSH access
 * (docs/STAGE3.md). Bus mastering stays off below level 6: PSP and the
 * firmware buffers all live in VRAM; level 6 turns it on once GPUVM can
 * translate, so GTT BOs (RDNA3Gtt, IODMACommand) are reachable.
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
class IOBufferMemoryDescriptor;
class IODMACommand;

/* A GTT (system memory) BO's backing: kernel buffer shared with the user
 * mapping, prepared for DMA, and the bus address of every 4 KiB page as
 * the GPU must see it (through the system mapper / VT-d when present).
 * bo->backing points at one of these. */
struct RDNA3Gtt {
    IOMemoryDescriptor       *md;        /* IOBufferMemoryDescriptor, or the client's pages (userptr) */
    IODMACommand             *dma;
    uint64_t                 *pages;
    uint64_t                  npages;
    bool                      prepared;  /* userptr: md->prepare() holds the pages wired */
};

/* VRAM layout (offsets from VRAM start):
 *   [16 MiB or the boot scanout's end, +kVramKernelLowSize)  kernel, CPU-visible
 *   [.., end of the BAR window)                              user BOs, visible
 *   [.., VRAM - kVramKernelTopSize)                          user BOs, not visible
 *   [VRAM - kVramKernelTopSize, VRAM - firmware reservation) kernel, GPU only
 *                                                            (PSP TMR), top-down
 * as amdgpu keeps CPU-visible VRAM for what the CPU maps and lets TTM place
 * NO_CPU_ACCESS kernel BOs top-down. */
static const uint64_t kVramKernelLowStart = 16ull << 20;
static const uint64_t kVramKernelLowSize  = 96ull << 20;
static const uint64_t kVramKernelTopSize  = 256ull << 20;
static const uint64_t kVramMinTopReserve  = 16ull << 20;

class RDNA3Accel : public IOService {
    OSDeclareDefaultStructors(RDNA3Accel)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;
    /* amdgpu_pci_shutdown, display part: the card goes back to the GOP's
     * single head before macOS powers off or restarts (a joined PM tree
     * member gets this call; the power states themselves are no-ops) */
    virtual void systemWillShutdown(IOOptionBits specifier) override;
    virtual IOReturn setPowerState(unsigned long state, IOService *dev) override;

    /* Stage 4: DRM/amdgpu UAPI for RDNA3UserClient (RDNA3Drm.cpp) */
    bool  drmAvailable() const { return fDrm != nullptr; }
    int   drmOpen(rdna3_drm_file *f);
    void  drmClose(rdna3_drm_file *f);
    int   drmIoctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size);
    IOMemoryDescriptor *drmMemoryForOffset(rdna3_drm_file *f, uint64_t off,
                                           IOOptionBits *options);

    /* called by the bring-up thread when a ring really executes */
    void  drmRingReady(uint32_t ip, uint32_t mask);
    static uint64_t drmGpuClock(void *ctx);
    void  drmSetClocks(uint32_t minSclkKhz, uint32_t maxSclkKhz, uint32_t minMclkKhz, uint32_t maxMclkKhz);
    RDNA3Bringup *bringup() const { return fBringup; }

    /* Stage 3: VRAM below this offset holds the firmware's scanout(s);
     * the bring-up and GEM heaps start above it (RDNA3Display.cpp) */
    uint64_t displayReserveEnd() const { return fDispReserveEnd; }
    /* Step 9b (rdna3mirror=1): show the boot head on the other TMDS
     * monitor too, through the DMCUB the bring-up started; off again
     * before the bring-up frees it (RDNA3Display.cpp) */
    void displayMirror(struct rdna3_dmub *dm);
    void displayMirrorOff(struct rdna3_dmub *dm);
    /* B7: an HPD pin changed (bring-up work loop, after the ack); the
     * connectors are probed again on a thread call */
    void displayHotplug(uint32_t pin, int connected);
    void displayShutdown();

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
    range_alloc  *fVramHeap = nullptr;   /* user BOs, CPU-visible part of VRAM */
    range_alloc  *fVramHeapHi = nullptr; /* user BOs beyond the BAR window */
    uint64_t      fVramHeapVisEnd = 0;   /* offsets below this are in fVramHeap */
    IOMemoryMap  *fVramMap  = nullptr;   /* BAR0, for BO clears / kernel access */
    IODeviceMemory *fVramDev = nullptr;  /* BAR0 descriptor for user mappings */
    bool          drmInit(const amd_discovery *disc);
    void          drmFree();
    static void  *drmZalloc(void *ctx, size_t n);
    static void   drmFreeMem(void *ctx, void *p, size_t n);
    static int    drmBoAlloc(void *ctx, rdna3_bo *bo);
    static int    drmBoUserptr(void *ctx, void *fileOs, rdna3_bo *bo, uint64_t uaddr, uint32_t flags);
    static int    drmSensor(void *ctx, uint32_t type, uint32_t *value);
    static int    drmVmFaultInfo(void *ctx, void *vm, uint64_t *addr, uint32_t *status, uint32_t *vmhub);
    int           gttAlloc(rdna3_bo *bo);
    int           gttWire(rdna3_bo *bo, IOMemoryDescriptor *md, bool prepared);
    static void   gttFree(RDNA3Gtt *g);
    static void   drmBoFree(void *ctx, rdna3_bo *bo);
    static int    drmCopyin(void *ctx, void *dst, uint64_t uaddr, size_t len);
    static int    drmCopyout(void *ctx, uint64_t uaddr, const void *src, size_t len);
    static void  *drmVmCreate(void *ctx);
    static void   drmVmDestroy(void *ctx, void *vm);
    static int    drmVmMap(void *ctx, void *vm, rdna3_bo *bo, uint64_t va, uint64_t off,
                           uint64_t size, uint32_t flags);
    static void   drmVmUnmap(void *ctx, void *vm, uint64_t va, uint64_t size);
    static int    drmSubmit(void *ctx, void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs,
                            uint32_t n, const rdna3_cs_job *job, uint64_t *seq);
    static int    drmFenceSignaled(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq);
    static int    drmFenceWait(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t absNs);
    static uint32_t drmVmQuery(void *ctx, void *vm);

    /* Stage 7 */
    range_alloc  *fAccelVa = nullptr;
    void         *fAccelVm = nullptr;    /* the accelerator's GPUVM context (its own VMID) */
    IOReturn      platformPageoff(struct RD3KSurfPageoff *q);
    uint64_t      fAccelVramBytes = 0;
    bool          platformInit();
    void          platformFree();
    IOReturn      platformVramAlloc(struct RD3KVramRequest *r);
    IOReturn      platformVramFree(struct RD3KVramRequest *r);
    IOReturn      platformGpuVa(int op, struct RD3KGpuVaRequest *g);

    /* Stage 3: display heads the firmware lit (RDNA3Display.cpp) */
    struct rdna3_dev *fDispDev = nullptr;
    struct rdna3_dcn *fDcn     = nullptr;
    IOLock       *fDispLock    = nullptr;
    struct rdna3_connector *fConnectors = nullptr;   /* DP AUX / HPD probe (hw/dcaux.c) */
    uint32_t      fNConnectors = 0;
    /* VBIOS for Display Core: display paths and the AtomBIOS interpreter
     * (read here, before the bring-up thread starts using the ROM window) */
    uint8_t      *fDispRom     = nullptr;
    struct rdna3_vbios *fDispVbios = nullptr;
    struct rdna3_atom  *fAtom      = nullptr;
    uint32_t     *fAtomScratch = nullptr;
    struct rdna3_display_path *fPaths = nullptr;
    uint32_t      fNPaths      = 0;
    uint32_t      fDispRefresh[4] = {};   /* measured, mHz */
    bool          fDispMeasured = false;
    bool          fDcnCrcOn[4]  = {};
    uint64_t      fDispReserveEnd = 0;
    struct rdna3_dcn_clone *fMirror = nullptr;   /* hw/dcn32_clone.c plan, when lit */
    bool          fMirrorOn    = false;
    bool          fPmJoined = false;
    uint64_t      fSleepAssertion = 0;        /* IOPMDriverAssertionID: no idle sleep (no resume yet) */
    /* PerformanceStatistics (what AMD's drivers publish; read by Activity
     * Monitor-style tools), refreshed every 2 s from the SMU metrics */
    struct thread_call *fStatsCall = nullptr;
    volatile bool fStatsStop = false;
    uint32_t      fStatsTicks = 0;
    static void   statsTick(void *owner, void *);
    void          statsStart();
    void          statsStop();
    int           fCloneHead   = -1;          /* rdna3mirror=2: fDcn head of the clone */
    IOService    *fHeadNub     = nullptr;     /* "RDNA3DisplayHead" for its framebuffer */
    void          displayAddCloneHead();
    void          displayMirrorDisable(struct rdna3_dmub *dm);
    bool          displayCloneSinkPresent();
    struct rdna3_dmub *volatile fMirrorDm = nullptr;   /* the DMCUB the mirror uses (bring-up's) */
    struct thread_call *fHotplugCall = nullptr;
    volatile UInt32 fHotplugPending = 0;      /* bit n = HPD pin n changed */
    uint32_t      fHotplugEvents = 0;
    static void   displayHotplugWork(void *owner, void *);
    bool          displayInit(const amd_discovery *disc);
    void          displayFree();
    void          displayMeasureRefresh();
    IOReturn      displayInfo(struct RD3KDisplayInfo *r);
    IOReturn      displayFlip(struct RD3KDisplayFlip *f);
    IOReturn      displayStatus(struct RD3KDisplayStatus *s);
    IOReturn      displayCrc(uint32_t head, uint32_t *crc);
    IOReturn      displayCursor(struct RD3KDisplayCursor *c);
    IOReturn      displayEdid(struct RD3KDisplayEdid *e);
    void          displayProbeConnectors();
    void          displayPublishRoutes();
    void          displayReadVbios();
    static uint32_t dispMmioRd(void *ctx, uint64_t off);
    static void     dispMmioWr(void *ctx, uint64_t off, uint32_t v);

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
