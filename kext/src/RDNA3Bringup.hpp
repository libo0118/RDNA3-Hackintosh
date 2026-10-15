/*
 * RDNA3Bringup - Stage 2 driver: runs hw/ (PSP, SMU) on the real GPU.
 *
 * Runs on its own kernel thread because it has to wait for
 * the root volume to read the firmware files and the
 * PSP sequence takes seconds. Levels (boot-arg rdna3lvl=):
 *   4  map BAR0, discovery, FB location, HDP remap, PSP bootloader + sOS,
 *      KM ring, TMR, all IP firmware, RLC autoload
 *   5  + SMU hello (TestMessage / GetSmuVersion / GetDriverIfVersion);
 *        with rdna3pm=1 (default) the VBIOS is read through the ROM window
 *        and the PMFW power management is brought up (PPTable, DPM,
 *        throttlers, fan): rdna3pm=0 leaves the boot clocks, rdna3pm=2
 *        allows amdgpu's full feature set (UCLK/FCLK/DCN DPM too),
 *        rdna3pmmask=<hex> sets the allowed feature mask directly.
 *   6  + GMC, IH, doorbell aperture (BAR2; rdna3doorbell=0 keeps the rings
 *        on their wptr registers and skips the compute queues);
 *        with rdna3msi=1 the IH ring is drained from the device's
 *        MSI (IOFilterInterruptEventSource on our own work loop) and fence
 *        waits sleep until the handler wakes them; a 5 ms guard sleep covers
 *        a lost interrupt. Level 7 verifies the first SDMA trap interrupt
 *        and falls back to polling when none arrives.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_BRINGUP_HPP
#define RDNA3_BRINGUP_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

extern "C" {
#include "rdna3_hw.h"
#include "psp13.h"
#include "gmc11.h"
#include "ih60.h"
#include "sdma60.h"
#include "gpuvm.h"
#include "gfx11.h"
#include "soc21_cg.h"
#include "doorbell.h"
#include "smu13.h"
#include "dmub32.h"
#include "xfer.h"
#include "rdna3_drm.h"
#include "amd_fw.h"
#include "atom.h"
#include "vcn40.h"
}

class RDNA3Accel;
struct RDNA3Vm;

struct RDNA3Firmware {
    uint8_t *data;
    uint32_t len;
};

class RDNA3Bringup {
public:
    bool init(RDNA3Accel *owner, IOPCIDevice *pci, IOMemoryMap *mmio,
              uint32_t level, const amd_discovery *disc, uint64_t vramSize);
    void start();          /* spawns the thread */
    void stopAndFree();    /* waits for the thread, frees everything */

    /* rdna3_os callbacks (static trampolines) */
    static void     osUdelay(void *ctx, uint32_t us);
    static void     osMsleep(void *ctx, uint32_t ms);
    static void     osVlog(void *ctx, int level, const char *fmt, va_list ap);
    static void     osWmb(void *ctx);
    static uint32_t osMmioRd(void *ctx, uint64_t off);
    static void     osMmioWr(void *ctx, uint64_t off, uint32_t v);

    /* GPUVM for the DRM UAPI (called under RDNA3Accel's fDrmLock). Before
     * the GMC is up (rdna3lvl < 6, or the thread still running) a VM is
     * created "record only" and map/unmap do nothing. Once it is up a VM
     * always runs: without a free VMID or page tables vmCreate returns
     * nullptr (the file open fails) instead of a VM that cannot submit. */
    void *vmCreate(bool quiet = false);   /* quiet: no log line when there is no VM */
    /* a new DRM file of 'uid' (admin: kIOClientPrivilegeAdministrator)
     * may take a VMID (rdna3_vmid_admit; always before the GMC is up);
     * false logs the refusal, its reason and the VMIDs' holders */
    bool  vmCanOpen(uint32_t uid, bool admin);
    /* rdna3_drm_ops.vm_orphaned: the file closed with a job still running */
    void  vmOrphaned(void *vm, bool reclaimable);
    void  vmDestroy(void *vm);
    int   vmMap(void *vm, rdna3_bo *bo, uint64_t va, uint64_t offset, uint64_t size,
                uint32_t flags);
    void  vmUnmap(void *vm, uint64_t va, uint64_t size);
    bool  vmLive(void *vm) const;   /* false: record-only (created before the GMC) */
    /* VRAM -> system pages on SDMA0 through the last 16 MiB of GART (called
     * under the DRM lock, which also serializes the DMA ring's submits).
     * RDNA3_ENODEV until GMC and SDMA are up (rdna3lvl>=7). */
    int   copyVramToPages(uint64_t vramOff, const uint64_t *pages, uint64_t npages, uint64_t len);
    /* a pitched rectangle VRAM -> VRAM by SDMA0 (sdma60_copy_rect_sync) */
    int   copyVramRect(uint64_t srcOff, uint32_t srcPitch, uint64_t dstOff, uint32_t dstPitch, uint32_t rowBytes,
                       uint32_t rows);
    int   copyPagesToVram(uint64_t vramOff, const uint64_t *pages, uint64_t npages, uint64_t len);
    int   clearVram(uint64_t vramOff, uint64_t len);   /* SDMA CONST_FILL, 0 */

    /* command submission (DMA ring 0 = SDMA0 from rdna3lvl=7) */
    int   submit(void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                 const rdna3_cs_job *job, uint64_t *seq);
    int   fenceSignaled(uint32_t ip, uint32_t ring, uint64_t seq);
    /* capHit == nullptr: run recover() at the 10 s cap (caller holds the
     * DRM lock). Otherwise report the cap in *capHit and let the caller
     * take the lock and recover: the wait itself runs unlocked. */
    int   fenceWait(uint32_t ip, uint32_t ring, uint64_t seq, uint64_t absNs,
                    bool *capHit = nullptr);
    int   recoverHang(uint32_t ip, uint32_t ring, uint64_t seq) { return recover(ip, ring, seq); }

    /* MSI: called on the work loop for every interrupt */
    void  handleInterrupt();
    /* amdgpu_pci_shutdown with rdna3amdgpu=1 (DRM lock held) */
    void  pmShutdown();

    /* user queues (boot-arg rdna3userq=1 with the MES scheduler, amdgpu's
     * user_queue=1): a queue's MQD (adev->mqds[ip].init_mqd), MES ADD_QUEUE /
     * REMOVE_QUEUE in the process of a VM, a BO's VMID 0 address (a one-page
     * GTT BO is bound to a GART slot, amdgpu_ttm_alloc_gart), a sleep until
     * the next interrupt or 'absNs' (5 ms at most). DRM lock held. */
    int   userqMqdInit(uint32_t ip, void *mqd, const amd_mqd_prop *p);
    int   userqAdd(void *vm, amd_mes_add_queue *in);
    int   userqRemove(const amd_mes_remove_queue *in);
    int   gpuAddr(rdna3_bo *bo, uint64_t *mc);
    void  gartRelease(uint32_t slot);
    void  waitInterrupt(uint64_t absNs);

    /* AMDGPU_CTX_QUERY2_FLAGS_* of a VM: reset since it was made, VRAM
     * lost, guilty (one of its jobs hung or page-faulted) */
    uint32_t vmQuery(void *vm);
    /* AMDGPU_INFO_GPUVM_FAULT: the VM's last page fault */
    int      vmFaultInfo(void *vm, uint64_t *addr, uint32_t *status, uint32_t *vmhub);
    /* AMDGPU_INFO_SENSOR from the SMU metrics (re-read at most every 100 ms) */
    int      sensor(uint32_t type, uint32_t *value);
    /* power-management control for userspace (common/rdna3_pmctl.h):
     * fan, power limit, overdrive, profiles, clock levels, DF C-states,
     * BACO. Reads always; writes need rdna3pmctl=1 (overdrive 2), BACO
     * rdna3baco=1, and an administrator ('admin'). DRM lock held. */
    int      pmctl(const rdna3_pmctl_req *rq, rdna3_pmctl_reply *rp, bool admin);
    /* PerformanceStatistics: activity %, edge / hotspot / memory C, fan
     * rpm, gfx / memory clock MHz, power W; under the DRM lock */
    bool     stats(uint32_t v[8]);
    /* amdgpu_gfx_off_ctrl(false / true); the caller holds the DRM lock */
    void     gfxOffHold();
    void     gfxOffRelease();
    /* which GC block is busy (GRBM / CP status), next to the PM trend */
    void     logBusy();
    /* rdna3pmexp=1: idle-power variants one after the other, 10 s each,
     * from the 2 s stats tick (DRM lock held); true while it runs (the
     * caller then leaves the GC registers alone) */
    bool     pmExperimentTick();
    /* rdna3idlecap=1: GFXCLK soft max at the lowest level after 5 s
     * without a submission, lifted at the next one (2 s stats tick, DRM
     * lock held) */
    void     idleCapTick();
    /* D4, after a system wake (RDNA3Resume.cpp): the PSP sOS still runs (1),
     * not (0: the card lost its power), unknown (-1); and
     * amdgpu_device_resume for a card that lost it: post, then levels 4-8
     * again (hwInit), as gpuReset after its mode-1 reset. DRM lock held. */
    bool     finished() const { return fFinished; }
    bool     vcnWanted() const { return fVcnWanted; }
    int      sosAlive();
    bool     resumeAfterPowerLoss();
    /* P4 (rdna3resume=1): amdgpu_device_suspend before a system sleep;
     * the wake then always runs the resume (resumeAfterPowerLoss) */
    bool     systemSuspend();
    void     kgqStop();
    bool     suspended() const { return fSuspended; }
    /* VRAM was lost and the live VMs have new, empty roots: the DRM maps
     * every client's ranges again (rdna3_drm_device_vm_replay); true once */
    bool     takeVmReplay() { bool r = fVmReplay; fVmReplay = false; return r; }
    /* the last resume found VRAM lost; forgetVramLoss: every evicted range
     * came back, the clients are not told (amdgpu_device_resume) */
    bool     lastResumeLostVram() const { return fLastResumeVramLost; }
    bool     lastVramLost() const { return fLastVramLost; }   /* the last checkVramLost */
    /* VRAM losses so far (adev->vram_lost_counter), less the resumes whose
     * contents all came back (forgetVramLoss) */
    uint32_t vramLostCount() const { return fVramLostCount; }
    /* the MES scheduler started again after a reset and forgot its user
     * queues: the DRM puts them back (rdna3_drm_device_userq_reset);
     * true once, *vramLost: whether that reset lost VRAM */
    bool     takeUserqRestart(bool *vramLost)
    {
        bool r = fUserqRestart;
        fUserqRestart = false;
        *vramLost = fUserqRestartVramLost;
        return r;
    }
    void     forgetVramLoss();
    /* VCN / JPEG across a system sleep (rdna3vcn): amdgpu_device_ip_suspend's
     * jpeg_v4_0_suspend + vcn_v4_0_suspend before it (the VCPU BOs copied
     * to system memory), and after a wake that kept the card's power their
     * resume (a power loss resumes them in hwInit's vcnStart). DRM lock held. */
    void     vcnSuspend();
    void     vcnResume();

private:
    uint32_t         fResumes = 0;          /* D4: resumes after a power loss */
    static void threadMain(void *arg, int wait_result);
    void run();
    /* optional: one look in each directory, no waiting for the file */
    bool loadFirmware(const char *name, RDNA3Firmware *out, bool optional = false);
    bool mapVram();
    bool postIfNeeded();
    void resetIfPspStuck();
    int  mode1Reset();
    void publish(const char *key, uint64_t v);
    void publishString(const char *key, const char *s);

    RDNA3Accel   *fOwner  = nullptr;
    IOPCIDevice  *fPci    = nullptr;
    IOMemoryMap  *fMmio   = nullptr;
    IOMemoryMap  *fVram   = nullptr;
    volatile uint8_t *fMmioBase = nullptr;
    uint32_t      fLevel  = 0;
    amd_discovery *fDisc  = nullptr;   /* owned copy */
    uint64_t      fVramSize = 0;

    rdna3_dev    *fDev    = nullptr;
    rdna3_psp    *fPsp    = nullptr;
    amd_ucode_plan *fPlan = nullptr;
    rdna3_gmc    *fGmc    = nullptr;   /* level 6 */
    rdna3_ih     *fIh     = nullptr;   /* level 6 */
    /* rdna3amdgpu=1: IH rings 0 and 1 in system memory (amdgpu's GTT
     * rings), mapped at the bottom of the GART */
    struct IhSys {
        IOBufferMemoryDescriptor *md;
        IODMACommand             *dma;
        uint64_t                  pages[RD_IH_RING_SIZE / 4096 + 1];
    };
    IhSys            fIhSys[2] = {};
    rdna3_ih        *fIh1 = nullptr;
    bool             ihSysAlloc();
    void             ihSysFree();
    rdna3_sdma_ring *fSdma = nullptr;  /* level 7 */
    rdna3_gfx_ring  *fGfx  = nullptr;  /* level 8 */
    rdna3_mec_ring  *fKiq  = nullptr;  /* level 8, with doorbells */
    rdna3_mec_ring  *fKcq  = nullptr;  /* level 8: the first kernel compute queue */
    struct rdna3_mes *fMes = nullptr;  /* level 8 + rdna3mes=1: MES scheduler pipe (C1) */
    bool             fMesWanted = false;
    /* rdna3amdgpu=1: amdgpu's own sequence where the verified default
     * differs (rdna3_dev.amdgpu_exact); ladder step amdgpu */
    bool             fAmdgpuExact = false;
    /* rdna3amdgpu=1: gfx_v11_0_sw_init's queue topology (ring names, KCQ
     * priority), the CP fault IVs of gfx_v11_0_late_init and
     * drm_sched_fault: a ring whose job faulted (bit AMDGPU_HW_IP_GFX /
     * _COMPUTE) is recovered by the next fence wait right away */
    rdna3_gfx_topo   fGfxTopo = {};
    /* rdna3amdgpu=1: the PASID each GFXHUB VMID's IH LUT entry holds
     * (amdgpu_vmid id->pasid); 0 = none / unknown */
    uint32_t         fVmidPasid[16] = {};
    volatile uint32_t fSchedFault = 0;
    uint64_t         fCpFaults = 0;
    uint64_t         fIbTestBuf = UINT64_MAX;
    void             gfxSuspend();
    void             fenceDriverHwFini();
    void             ipSuspend(bool all);
    /* rdna3amdgpu=1: the SMU's thermal / MP1 IVs (smu_v13_0_irq_process).
     * The hardware CTF is handled in the IV callback; whatever needs the
     * PMFW mailbox runs from fSmuIrqCall under the DRM lock (50 ms later
     * for the software CTF check, AMDGPU_SWCTF_EXTRA_DELAY) */
    struct thread_call *fSmuIrqCall = nullptr;
    struct SmuIv { uint32_t client, src, ctx; };
    static const uint32_t kSmuIvMax = 8;
    SmuIv            fSmuIrqQ[kSmuIvMax] = {};
    uint32_t         fSmuIrqN = 0;           /* under fIhLock */
    bool             fSmuIrqStop = false;
    uint64_t         fSmuIrqs = 0;
    void             smuIrq(const rdna3_iv *iv);
    static void      smuIrqTick(void *owner, void *);
    void             overTemp(const char *why);
    /* user queues: rdna3userq=1 (needs the MES, so it turns rdna3mes on);
     * the IPs that got them, the PASIDs the VMs' processes have for the MES,
     * GART slots of user queue wptr BOs (under the user fence slots) */
    bool             fUserqWanted = false;
    uint32_t         fUserqMask = 0;
    uint32_t         fNextPasid = 0;
    static const uint32_t kUqSlots = 64;
    bool             fUqSlot[kUqSlots] = {};
    void             userqStart(const uint32_t ver[32]);
    void             userqIrqs();
    void             userqFlushMesVmids();
    bool             fCgWanted = false;   /* GFX clock gating, default on (rdna3cg=0: off) */
    bool             fSocCgWanted = false; /* HDP + ATHUB gating (rdna3socg=1) */
    bool             fPmExpWanted = false; /* rdna3pmexp=1 */
    bool             fIdleCapWanted = false, fIdleCapped = false;   /* rdna3idlecap=1 */
    uint64_t         fLastSubmit = 0;      /* mach_absolute_time */
    uint32_t         fIdleCaps = 0;
    uint32_t         fSmuVersion = 0;      /* smu_cmn_get_smc_version */
    void             idleCapLift();
    uint32_t         fPmExpTick = 0, fPmExpPhase = 0, fPmExpSamples = 0;
    uint64_t         fPmExpSum[8] = {};
    /* GFXOFF (rdna3gfxoff=1, step 8o): amdgpu_gfx_off_ctrl. The GFX may
     * power off while nothing holds it; every DRM ioctl and the PM trend
     * read hold it on, the last release allows GFXOFF again 100 ms later
     * (GFX_OFF_DELAY_ENABLE). Holds and the SMU mailbox: under the DRM lock. */
    bool             fGfxOffWanted = false, fGfxOffOn = false, fGfxOffStop = false;
    uint32_t         fGfxOffHolds = 1;    /* the bring-up holds it until done */
    struct thread_call *fGfxOffCall = nullptr;
    static void      gfxOffTick(void *owner, void *);
    void             cgStart(const char *why);
    void             mesStart(const char *why);
    /* VCN 4.0 + JPEG 4.0 (rdna3vcn=1, off by default): the video rings for
     * VCN_ENC / VCN_JPEG CS; powered on demand, gated 1 s after the last
     * job (amdgpu_vcn_idle_work_handler) from fVcnIdleCall under the DRM lock */
    /* rdna3vcn=1 static power gating, =2 DPG as amdgpu runs it (indirect
     * SRAM through the PSP), =3 DPG with the SRAM written through the LMA */
    bool             fVcnWanted = false, fVcnStop = false, fVcnSuspended = false;
    uint32_t         fVcnMode = 0;
    rdna3_vcn       *fVcn = nullptr;
    uint32_t         fVcnHangs = 0;
    struct thread_call *fVcnIdleCall = nullptr;
    volatile uint64_t fIrqVcn = 0, fIrqJpeg = 0;
    void             vcnStart();
    void             vcnFreeSaved();
    void             vcnIdleSchedule();
public:
    void             vcnShutdown();
    void             userqDiag();
private:
    int              vcnHang(uint32_t ip, uint32_t ring, uint64_t seq);
    static void      vcnIdleTick(void *owner, void *);
    IOMemoryMap     *fDoorbell = nullptr;   /* BAR2 */
    /* power management (level 5, rdna3pm) */
    uint32_t         fPmMode = 1;
    uint64_t         fPmMask = 0;           /* rdna3pmmask override, 0 = by mode */
    rdna3_smu       *fSmu = nullptr;
    rdna3_vbios      fVbios = {};
    uint8_t         *fRom = nullptr;        /* RD_VBIOS_MAX_LEN */
    bool             fPmUp = false;
    uint64_t         fMetricsAt = 0;         /* mach_absolute_time of the last metrics read */
    void             pmInit();
    void             pmPublishMetrics();
    /* PM control (rdna3pmctl=0 off / 1 writes / 2 + overdrive, rdna3baco=1):
     * the user's settings amdgpu's smu_restore_dpm_user_profile puts back
     * after a re-init (power limit, fan mode and speed, overdrive) */
    uint32_t         fPmCtl = 0;
    bool             fBacoWanted = false;
    uint32_t         fBacoCycles = 0;
    uint32_t         fUserPowerLimit = 0, fUserFanMode = RD_FAN_CTRL_AUTO;
    uint32_t         fUserFanPwm = 0, fUserFanRpm = 0;
    bool             fUserOd = false;
    OverDriveTableExternal_t *fUserOdTable = nullptr;
    uint32_t         pmctlGates(bool admin) const;
    void             pmCtlInit(const rdna3_vbios *vb);
    void             pmCtlRemember(const rdna3_pmctl_req *rq);
    void             pmCtlPublish();
    int              bacoCycle();
    /* DMCUB (level 5, rdna3dmub=1): dm_dmub_hw_init on the PSP-loaded image */
    uint32_t         fDmubMode = 0;
    rdna3_dmub      *fDmub = nullptr;
    void             dmubInit();

    /* robustness: amdgpu_job_timedout -> soft recovery -> GFX soft reset ->
     * mode-1 reset and a new bring-up from level 4 (hwInit); page faults
     * from the IH make the VM guilty (gmc_v11_0_process_interrupt) */
    amd_fw_blob_ref  fBlobRefs[AMD_BLOB_COUNT] = {};
    uint64_t         fAllocLo = 0, fAllocHi = 0;
    bool             fReinit = false;        /* hwInit() runs again after a reset */
    bool             fSuspended = false;     /* systemSuspend ran, the resume has not */
    bool             fVmReplay = false;      /* takeVmReplay */
    bool             fLastResumeVramLost = false;
    bool             fLastVramLost = false;          /* the last checkVramLost */
    bool             fMesStarted = false;
    bool             fUserqRestart = false, fUserqRestartVramLost = false;
    uint32_t         fSystemSuspends = 0;
    uint32_t         fResetCount = 0, fVramLostCount = 0;
    uint64_t         fGpuResets = 0, fSoftRecoveries = 0, fVmFaults = 0;
    uint64_t         fResetMagic = UINT64_MAX;   /* VRAM slot: did VRAM survive the reset? */
    RDNA3Vm         *fVmByVmid[16] = {};
    bool             hwInit();
    int              recover(uint32_t ip, uint32_t ring, uint64_t seq);
    /* 'baco': the chip goes through BACO (enter + exit, rdna3baco=1)
     * instead of the mode-1 reset; the same re-init follows */
    bool             gpuReset(bool baco = false);
    /* shared by gpuReset and resumeAfterPowerLoss */
    void             vmFlushAllOnNextJob();
    bool             checkVramLost(const char *what);
    void             vmRebuildAfterVramLoss();
    bool             reinitFromLevel4();
    void             markGuilty(uint32_t vmid, const char *why);
    void             forceFences();
    bool             fDoorbells = true;     /* boot-arg rdna3doorbell */
    bool             mapDoorbell();
    rdna3_pt_pool *fPtPool = nullptr;  /* level 6: page tables for user VMs */
    uint32_t      fVmidUsed = 0;       /* bit n = VMID n taken (0 is the kernel's) */
    uint32_t      fVmidRefusals = 0;   /* DRM opens refused: no VMID left */
    uint32_t      fVmidsLeaked = 0;    /* VMs of closed files whose job still runs (vmOrphaned) */
    /* VMIDs for clients: 1..7 with user queues or rdna3amdgpu=1 (8..15 are
     * the MES's / amdgpu's first_kfd_vmid), else 1..15 */
    uint32_t      vmidLimit() const { return (fUserqWanted || fAmdgpuExact) ? (uint32_t)RD_FIRST_KFD_VMID : 16u; }
    uint32_t      vmidsFree() const;   /* client VMIDs not taken */
    uint64_t      fVmPd[16] = {};      /* page directory per live VMID */
    volatile bool fVmReady = false;
    void          vmFlush(uint32_t vmid);
    RDNA3Firmware fSos    = {};
    RDNA3Firmware fTa     = {};   /* psp_13_0_10_ta.bin, kept with rdna3amdgpu=1 (ASD, HDCP, DTM) */
    RDNA3Firmware fBlobs[AMD_BLOB_COUNT] = {};

    /* firmware request rendezvous */
    IOLock       *fLock   = nullptr;
    IOLock       *fIhLock = nullptr;
    uint32_t      fSdmaLastVmid = 0;   /* VMID of the last IB on the DMA ring (hang blame) */
    /* user fences (AMDGPU_CHUNK_ID_FENCE) in GTT BOs: the ring writes them
     * in VMID0, so their page gets one of these GART slots, right under the
     * 16 MiB transfer window (amdgpu_ttm_alloc_gart for uf_bo). A slot is
     * remapped only once its last job's fence passed. */
    static const uint32_t kUfSlots = 64;
    struct UfSlot { uint64_t pa; uint64_t seq; uint32_t ip, ring; bool used; };
    UfSlot        fUfSlot[kUfSlots] = {};
    int           ufAddr(const rdna3_cs_job *job, uint64_t *mc, int *slot);
    int           submitRing(RDNA3Vm *v, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                             const rdna3_cs_job *job, uint64_t ufMc, uint64_t *seq);
    /* GDS / GWS / OA partition each VMID has (amdgpu_vmid gds_base...):
     * a job whose partition differs switches it from the ring. Unknown
     * after a reset, so the next job writes it again. */
    uint32_t      fGds[16][6] = {};
    bool          fGdsKnown[16] = {};
    uint32_t      fSdmaResets = 0;   /* one IH consumer at a time (MSI action or a polling waiter) */

    /* interrupts (rdna3msi=1) */
    bool          fMsi = false;             /* asked for */
    volatile bool fIrqLive = false;         /* handler registered and verified */
    bool          fHpdIrq = false;          /* DCN HPD interrupts enabled (B7) */
    IOWorkLoop   *fWorkLoop = nullptr;
    IOFilterInterruptEventSource *fIntr = nullptr;
    int           fFenceEvent = 0;          /* IOLockSleep event on fLock */
    volatile uint64_t fIrqCount = 0, fIvCount = 0;
    volatile uint64_t fIrqFenceGfx = 0, fIrqFenceSdma = 0, fIrqFenceComp = 0, fIrqOther = 0;
    volatile uint64_t fIrqUserq = 0;        /* user queue fences (EOP / SDMA fence with a doorbell) */
    /* diagnostic (card run 33): the last IVs nobody claimed */
    rdna3_iv         fIvOther[4];
    volatile uint32_t fIvOtherN = 0;
    bool          setupInterrupt();
    void          teardownInterrupt();
    static bool   irqFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void   irqAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void   ivCallback(void *ctx, const rdna3_iv *iv);

    /* thread state */
    bool          fRunning = false;
    bool          fFinished = false;
    volatile bool fAbort = false;
};

#endif
