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
#include <kern/thread_call.h>

extern "C" {
#include "amd_discovery.h"
#include "rdna3_drm.h"
#include "range_alloc.h"
#include "rdna3_pmctl.h"
}

/* Stopping a thread call whose function re-arms itself (or that a racing
 * kick may enter once more): its stop flag is set first; a run that read
 * the flag before that re-arms while thread_call_cancel_wait waits for it
 * (and that wait returns false), so cancel again until nothing was queued.
 * thread_call_free refuses a queued call, so the free retries the same way. */
static inline void rd3_thread_call_drain(thread_call_t c)
{
    __sync_synchronize();   /* the stop flag before the cancel */
    thread_call_cancel_wait(c);
    while (thread_call_cancel_wait(c)) {
    }
}
static inline void rd3_thread_call_drain_free(thread_call_t c)
{
    rd3_thread_call_drain(c);
    while (!thread_call_free(c))
        thread_call_cancel_wait(c);
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
    uint32_t                  gartSlot;  /* 1 + the GART slot a user queue's wptr page is bound
                                          * to (amdgpu_ttm_alloc_gart), 0 = none */
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
    /* the MES runs and rdna3userq=1: user queues for the IPs of 'ipMask'
     * (1 << AMDGPU_HW_IP_*), doorbell BOs from BAR2 */
    void  drmUserqReady(uint32_t ipMask, uint64_t doorbellSize);
    /* the VCN 4.0 block is up (rdna3vcn=1): the video IPs' INFO answers and
     * CS to VCN_ENC (instances in 'instReady') and VCN_JPEG */
    void  drmVcnReady(uint32_t ipVersion, uint32_t numInst, uint32_t harvest, uint32_t instReady,
                      bool jpegReady);
    static uint64_t drmGpuClock(void *ctx);
    void  drmSetClocks(uint32_t minSclkKhz, uint32_t maxSclkKhz, uint32_t minMclkKhz, uint32_t maxMclkKhz);
    RDNA3Bringup *bringup() const { return fBringup; }
    /* PM control from RDNA3UserClient (common/rdna3_pmctl.h): takes the
     * DRM lock; 'admin' = the calling task is an administrator */
    int   pmCtl(const rdna3_pmctl_req *rq, rdna3_pmctl_reply *rp, bool admin);
    IOLock       *drmLock() const { return fDrmLock; }
    /* hwInit() again (GPU reset from recover(), resume after a power
     * loss) runs with fDrmLock held by its caller: the drm* setters it
     * reaches must not take it again (card run 27: resume hung there) */
    void          drmHeldHere(bool on) { fDrmHeldBy = on ? IOThreadSelf() : nullptr; }
    bool          drmHeldByMe() const { return fDrmHeldBy && fDrmHeldBy == IOThreadSelf(); }
    /* AMDGPU_INFO_FW_VERSION table from the bring-up (amd_fw_versions);
     * ta: AMDGPU_INFO_FW_TA versions by TA_FW_TYPE_* (amd_fw_ta_versions) */
    void          drmSetFirmware(const uint32_t ver[32], const uint32_t feature[32],
                                 const uint32_t ta[RD_TA_TYPES]);
    /* AMDGPU_INFO_VBIOS: a copy of the image and its strings */
    void          drmSetVbios(const uint8_t *image, uint32_t len, const struct drm_amdgpu_info_vbios *info);
    /* rdna3amdgpu=1: DEV_INFO's xclk / VRAM type and width / MALL size
     * from the VBIOS and the discovery binary (0 keeps a value) */
    void          drmSetAsicInfo(uint32_t xclkKhz, uint32_t vramType, uint32_t vramWidth, uint64_t mallSize);
    /* amdgpu_discovery_harvest_ip / _get_mall_info / _get_vcn_info, from
     * the binary readDiscovery parsed; nullptr when it failed */
    const amd_disc_soc *discSoc() const { return fDiscSocOk ? &fDiscSoc : nullptr; }

    /* Stage 3: VRAM below this offset holds the firmware's scanout(s);
     * the bring-up and GEM heaps start above it (RDNA3Display.cpp) */
    uint64_t displayReserveEnd() const { return fDispReserveEnd; }
    /* Step 9b (rdna3mirror=1): show the boot head on the other TMDS
     * monitor too, through the DMCUB the bring-up started; off again
     * before the bring-up frees it (RDNA3Display.cpp) */
    /* 'probe': called from the bring-up, which owns the PMFW mailbox then:
     * the clone checks the PMFW answers after each DMCUB VBIOS command */
    void displayMirror(struct rdna3_dmub *dm, bool probe = false, bool reinit = false);
    /* rdna3mall=1: display power after the DMCUB runs (RDNA3DisplayPower.cpp) */
    void displayPowerInit(struct rdna3_dev *d, struct rdna3_dmub *dm, uint64_t wmTableOff);
    void displayMirrorOff(struct rdna3_dmub *dm);
    /* B7: an HPD pin changed (bring-up work loop, after the ack); the
     * connectors are probed again on a thread call */
    void displayHotplug(uint32_t pin, int connected);
    void displayShutdown();
    /* D4 (RDNA3Resume.cpp): a GPU recovery happened (chipReset: the mode-1
     * reset, else a GFX / SDMA soft reset); DRM lock held */
    void displayAfterReset(bool chipReset);

    /* Stage 7: requests from the accelerator kext (RDNA3Platform.cpp) */
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2,
                                          void *p3, void *p4) override;

    /* amdgpu_device_evict_resources before a sleep (RDNA3Evict.cpp): the
     * VRAM that holds clients' data copied to system memory, back after
     * the resume when VRAM was lost; fDrmLock held */
    void          vramEvict();
    void          vramRestore(bool vramLost);
    void          vramEvictFree();
    void          platformForEachVram(void (*fn)(void *ctx, uint64_t off, uint64_t size), void *ctx);
    struct RDNA3VramHandle *fVramHandles = nullptr;
    struct RDNA3EvictRange *fEvict = nullptr;
    uint32_t      fEvictN = 0, fEvictCap = 0;
    bool          fEvictIncomplete = false;
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
    IOThread      fDrmHeldBy = nullptr;
    range_alloc  *fVramHeap = nullptr;   /* user BOs, CPU-visible part of VRAM */
    range_alloc  *fVramHeapHi = nullptr; /* user BOs beyond the BAR window */
    uint64_t      fVramHeapVisEnd = 0;   /* offsets below this are in fVramHeap */
    IOMemoryMap  *fVramMap  = nullptr;   /* BAR0, for BO clears / kernel access */
    IODeviceMemory *fVramDev = nullptr;  /* BAR0 descriptor for user mappings */
    IODeviceMemory *fDoorbellDev = nullptr;  /* BAR2 descriptor: user queue doorbell BOs */
    bool          fUserqArg = false;     /* rdna3userq=1: the user queue hooks are installed */
    static void  *drmBoKmap(void *ctx, rdna3_bo *bo);
    static int    drmBoGpuAddr(void *ctx, rdna3_bo *bo, uint64_t *mc);
    static int    drmUserqMqdInit(void *ctx, uint32_t ip, void *mqd, const amd_mqd_prop *p);
    static int    drmUserqAdd(void *ctx, void *vm, amd_mes_add_queue *in);
    static int    drmUserqRemove(void *ctx, const amd_mes_remove_queue *in);
    static int    drmUserqWait(void *ctx, int (*done)(void *), void *arg, uint64_t absNs);
    static uint64_t drmNowNs(void *ctx);
    bool          drmInit(const amd_discovery *disc);
    void          drmFree();
    /* after a reset or a resume that lost VRAM: the clients' GPUVM ranges
     * into the rebuilt page tables (fDrmLock held) */
    void          drmVmReplayIfNeeded();
    static void  *drmZalloc(void *ctx, size_t n);
    static void   drmFreeMem(void *ctx, void *p, size_t n);
    static int    drmBoAlloc(void *ctx, rdna3_bo *bo);
    static int    drmBoUserptr(void *ctx, void *fileOs, rdna3_bo *bo, uint64_t uaddr, uint32_t flags);
    static int    drmSensor(void *ctx, uint32_t type, uint32_t *value);
    static int    drmScanout(void *ctx, struct rd_xnu_scanout *s);   /* RDNA3Display.cpp */
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
    IOReturn      platformCopyVram(struct RD3KCopyRequest *q);
    uint64_t      fAccelVramBytes = 0;
    uint32_t      fCopyVramFails = 0;      /* rdna3CopyVram refused (log limit) */
    uint32_t      fUserptrRo = 0;          /* GEM_USERPTR wired read-only after a read-write refusal */
    uint32_t      fUserptrFails = 0;       /* GEM_USERPTR refused (log limit) */
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
    /* D6 hotplug to the framebuffer (rdna3hotplug=1, RDNA3Hotplug.cpp) */
    struct RDNA3HotplugState *fHp = nullptr;
    void          hotplugInit();
    void          hotplugStop();
    void          hotplugSuspend();   /* system sleep: no looks until hotplugResume */
    void          hotplugDpLinkCheck();   /* the hotplug tick: DP link loss */
    void          hotplugResume();    /* the wake work, fDispLock not held */
    void          hotplugFree();
    bool          hotplugKick();
    void          hotplugHandle(uint32_t changed);
    void          hotplugPublish(uint32_t raw, uint32_t changed);
    void          hotplugTidyProps();
    static void   hotplugTick(void *owner, void *);
    IOReturn      displayHotplugQuery(struct RD3KDisplayHotplug *q);
    bool          displayInit(const amd_discovery *disc);
    void          displayFree();
    void          displayMeasureRefresh();
    IOReturn      displayInfo(struct RD3KDisplayInfo *r);
    IOReturn      displayFlip(struct RD3KDisplayFlip *f);
    IOReturn      displayStatus(struct RD3KDisplayStatus *s);
    IOReturn      displayCrc(uint32_t head, uint32_t *crc);
    /* display sleep from the framebuffer (rdna3DisplayBlank, rdna3dpms=1) */
    IOReturn      displayBlank(uint32_t head, bool blank);
    /* scaled modes from the framebuffer (rdna3DisplaySetSource) */
    IOReturn      displaySetSource(struct RD3KDisplaySource *s);
    /* real modes from the framebuffer (rdna3modeset=1, hw/dcn32_modeset.c) */
    IOReturn      displayModes(struct RD3KDisplayModes *r);
    IOReturn      displaySetMode(struct RD3KDisplayMode *m);
    struct rdna3_mode *fModeTab[4] = {};      /* the last list per head (RD3K_MAX_MODES) */
    uint32_t      fModeCount[4] = {};
    struct rdna3_dmub *volatile fDisplayDm = nullptr;   /* the bring-up's DMCUB once it runs */
    struct rdna3_dcn_scale *fScale = nullptr;
    /* rdna3dispclk=1: the PMFW's DISPCLK / DPPCLK limits, read once */
    struct rdna3_dcn_clk_limits *fClkLim = nullptr;
    const struct rdna3_dcn_clk_limits *displayClkLimits();
    /* rdna3dplink=1: the DP SST link per head (hw/dcn32_dp.c) for mode sets */
    struct rdna3_dp_link *fDpLink[4] = {};
    struct rdna3_dp_link *displayDpLink(uint32_t head, const struct rdna3_display_path *p);
    /* item O: display power (rdna3mall=1, rdna3subvp=1; RDNA3DisplayPower.cpp) */
    struct rdna3_dcn32_power *fPower = nullptr;
    struct rdna3_subvp_state *fSubvp = nullptr;
    struct rdna3_pstate_ctx  *fPowerCtx = nullptr;   /* the update's scratch */
    struct rdna3_pstate_ctx  *fPowerCtxMin = nullptr;   /* the same without SubVP (minimal transition) */
    bool          fPowerOn     = false;
    volatile bool fPowerStop   = false;
    struct thread_call *fPowerIdleCall = nullptr;
    uint32_t      fPowerUpdates = 0, fPowerIdles = 0;
    volatile uint32_t fPowerFlips = 0;
    uint32_t      fPowerFlipsSeen = 0;
    bool          displayPowerPclk(uint32_t *pclk, uint32_t *cursor);
    void          displayPowerUpdate(const char *why);
    void          displayPowerFlip(uint32_t head, uint64_t addr, bool immediate, int when);
    void          displayPowerQuiesce(bool pin);
    void          displayPowerSuspend();
    void          displayPowerWake(bool powerLost);
    void          displayPowerFree();
    static void   displayPowerTick(void *owner, void *);
    /* rdna3hdmi=1 / 3: DC's HDMI stream on TMDS heads (hw/dcn32_hdmi.c), the
     * sink's HDMI caps from the EDID of the last mode list */
    struct rdna3_hdmi_cfg *fHdmiCfg[4] = {};
    /* rdna3audio=1: the azalia endpoints (read_dce_straps, update_num_audio,
     * the hw_init of dcn32_init_hw, once) and each head's audio stream: the
     * EDID's audio info of the last mode list, the endpoint it holds */
    struct rdna3_audio_pool *fAudioPool = nullptr;
    struct rdna3_audio_stream *fAudio[4] = {};
    /* rdna3uhbr=1: dcn32_init_hw's dccg_init (dccg31_init: the HPO stream
     * encoders' SYMCLK32 from REFCLK), once before the first UHBR link */
    bool          fDccgInit = false;
    struct rdna3_audio_stream *displayAudio(uint32_t head, struct rdna3_dp_link *l, const struct rdna3_hdmi_cfg *hc,
                                            uint32_t fe, uint32_t otg);
    /* rdna3dsc=1: FEC + DSC policies on those links. rdna3mst=1: an MST
     * capable sink (hub, daisy chain) on such a head is taken over at the
     * next mode set: the topology probed, its sinks' EDIDs read, the head's
     * stream sent as one MST stream to one sink (rdna3mstport=N or the
     * first); back to SST when that does not run (hw/dp_mst.c) */
    struct rdna3_mst_mgr *fMstMgr[4] = {};
    int           displayMstTakeover(uint32_t head, struct rdna3_dp_link *l, const struct rdna3_mode *cur);
    int           displayMstRevert(uint32_t head, struct rdna3_dp_link *l, const struct rdna3_mode *cur);
    uint32_t      fFwVer[32] = {}, fFwFeat[32] = {}, fTaVer[RD_TA_TYPES] = {};
    uint8_t      *fVbiosCopy = nullptr;
    uint32_t      fVbiosCopyLen = 0;
    struct drm_amdgpu_info_vbios fVbiosInfo = {};
    IOReturn      displayCursor(struct RD3KDisplayCursor *c);
    /* D4: display state snapshot, standby on any head, sleep / wake
     * (RDNA3Resume.cpp, hw/dcn32_state.c) */
    struct rdna3_dcn_state *fDispState = nullptr;
    bool          fHeadBlank[4] = {};         /* the framebuffer's last power request */
    bool          fHeadLinkDown[4] = {};      /* rdna3dpms=2: monitor in standby */
    volatile bool fDispAsleep = false;        /* system sleep until the wake work ran */
    bool          fShuttingDown = false;
    uint32_t      fSleeps = 0, fWakes = 0;
    struct thread_call *fWakeCall = nullptr;
    static void   displayWakeWork(void *owner, void *);
    const struct rdna3_display_path *displayHeadPath(uint32_t head);
    uint32_t      displayHeadPclk(uint32_t head, const struct rdna3_display_path *p);
    struct rdna3_dp_link *displayStateDpLink(uint32_t head, bool sink);   /* fDispLock held */
    volatile uint32_t fDispLostGen = 0;      /* RD3KDisplayHotplug.lost */
    void          displayPictureLost(const char *why);
    void          displayStateSave(uint32_t head);   /* fDispLock held */
    void          displayStateSaveAll();
    void          displayStateFree();
    void          displayNoteBlank(uint32_t head, bool blank);
    int           displayHeadDpms(uint32_t head, bool blank);
    void          displayLinkUp(uint32_t head);   /* fDispLock held */
    int           displayHeadRelight(uint32_t head);   /* fDispLock held, rdna3hotplug=2 */
    uint32_t      displayStateRestore(const char *why, bool restore);   /* fDispLock held */
    void          displayWillSleep();
    void          displayDidWake();
    IOReturn      displayEdid(struct RD3KDisplayEdid *e);
    IOReturn      displayEdidLocked(struct RD3KDisplayEdid *e);
    void          displayProbeConnectors();
    void          displayPublishRoutes();
    void          displayReadVbios();
    static uint32_t dispMmioRd(void *ctx, uint64_t off);
    static void     dispMmioWr(void *ctx, uint64_t off, uint32_t v);
    /* output gamma from the framebuffer (rdna3DisplayGamma, rdna3gamma=1) */
    IOReturn      displayGamma(struct RD3KDisplayGamma *g);
    void          displayGammaReload(uint32_t head);   /* fDispLock held */
    uint32_t      fGammaCalls[4] = {};
    /* the table each head shows (nullptr: bypass): the LUT RAMs do not
     * survive a power loss, a restored head gets it again */
    struct rdna3_gamma_pwl *fGammaTab[4] = {};

    /* level 0 */
    void      readConfigSpace();
    bool      isInD0();

    /* level 1 */
    bool      mapMmio();
    bool      regValid(uint64_t byteOff) const;
    uint32_t  rreg(uint32_t dw);          /* returns ~0u when out of range */
    bool      waitForIfwi();
    bool      readVramSize();
    void      publishGpuIdentity(uint32_t mb);

    /* level 2 */
    void      vramRead(uint64_t pos, uint32_t *buf, uint32_t dwords);
    bool      readDiscovery(amd_discovery *disc);
    void      publishDiscovery(const amd_discovery *disc,
                               const uint8_t *blob, uint32_t len);

    /* level 3 */
    void      readGcStatus(const amd_discovery *disc);
    amd_disc_soc fDiscSoc = {};
    bool      fDiscSocOk = false;

    void      setNumber(const char *key, uint64_t val, unsigned bits = 32);
};

/* fDrmLock for a scope, unless this thread already holds it (a bring-up
 * callback reached from inside a DRM ioctl): the setters below are called
 * from both */
class RD3DrmScope {
public:
    explicit RD3DrmScope(const RDNA3Accel *a) : fLock(a && !a->drmHeldByMe() ? a->drmLock() : nullptr)
    {
        if (fLock)
            IOLockLock(fLock);
    }
    ~RD3DrmScope()
    {
        if (fLock)
            IOLockUnlock(fLock);
    }
    RD3DrmScope(const RD3DrmScope &) = delete;
    RD3DrmScope &operator=(const RD3DrmScope &) = delete;

private:
    IOLock *fLock;
};

#endif /* RDNA3_ACCEL_HPP */
