/*
 * rdna3k_abi.h - in-kernel interface between the accelerator kext
 * (RDNA3Accelerator, IOGraphicsAccelerator2 subclass) and the core kext
 * (RDNA3Accel, which owns the GPU) / its framebuffer (RDNA3Framebuffer,
 * Stage 3).
 *
 * Every call is IOService::callPlatformFunction(OSSymbol(name), false,
 * p1, ...). The shape follows the NullMoth accelerator <-> NVRM
 * interface so the ported accelerator keeps its call sites; the names are
 * our own so a NullMoth install never answers them.
 *
 * "device" / "memory" cookies are opaque to the accelerator: for us the
 * device cookie is the RDNA3Accel instance and a memory cookie is a VRAM
 * allocation handle returned by RD3K_VRAM_FN_ALLOC.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3K_ABI_H
#define RDNA3K_ABI_H

/* ---- VRAM (answered by the core kext; the accelerator asks fFB, which
 *      forwards to its provider until Stage 3 has its own framebuffer) ---- */
#define RD3K_VRAM_ABI_VERSION 2u
#define RD3K_VRAM_FN_ALLOC    "rdna3AllocVram"
#define RD3K_VRAM_FN_FREE     "rdna3FreeVram"
#define RD3K_VRAM_FN_SCANOUT  "rdna3ScanoutInfo"

/* flags, low byte: who asks, for the core's per-owner heap report (card run
 * 2026-10-17c: the CPU-visible heap ran out and the display's allocations
 * were refused; the core logs and counts the occupancy by owner). 0 = the
 * accelerator's own (WindowServer's framebuffers and legacy surfaces). An
 * older caller sends 0, an older core ignores the byte; the core clamps
 * unknown values to 0. Matches common/rdna3_vram_place.h (RD3_VO_*). */
#define RD3K_VRAM_OWNER_MASK       0xffu
#define RD3K_VRAM_OWNER_ACCEL      0u
#define RD3K_VRAM_OWNER_FB_SURFACE 1u   /* the framebuffer kext's mode / own surface */
#define RD3K_VRAM_OWNER_FB_CURSOR  2u   /* the hardware cursor */
#define RD3K_VRAM_OWNER_SWAP_D     3u   /* the swap copy's D buffer */

struct RD3KVramRequest {
    unsigned int       version;
    unsigned int       flags;
    unsigned long long size;          /* in: bytes wanted */
    unsigned long long actualSize;    /* out: bytes granted (page rounded) */
    unsigned long long phys;          /* out: CPU-physical (BAR0) address */
    void              *kva;           /* out: kernel mapping through BAR0 */
    void              *handle;        /* out: memory cookie */
    unsigned long long mappedTotal;   /* out: bytes of VRAM handed out so far */
    unsigned int width, height, pitch;/* scanout only */
    unsigned long long allocationCookie; /* out: MC (GPU) address of the allocation */
};

/* ---- GPU virtual addresses (answered by the core kext) ---- */
#define RD3K_GPUVA_ABI_VERSION     1u
#define RD3K_GPUVA_FLAG_UNMAP_ONLY 1u
#define RD3K_GPUVA_FN_ALLOC   "rdna3GpuVaAlloc"
#define RD3K_GPUVA_FN_MAP     "rdna3GpuVaMap"
#define RD3K_GPUVA_FN_FREE    "rdna3GpuVaFree"
#define RD3K_GPUVA_FN_HANDLES "rdna3DeviceHandles"

struct RD3KGpuVaRequest {
    unsigned int       version;
    unsigned int       flags;
    void              *kapiDevice;    /* device cookie */
    void              *kapiMemory;    /* memory cookie (MAP) */
    unsigned int       hClient;       /* unused on AMD */
    unsigned int       hDevice;       /* unused on AMD */
    unsigned int       hMemory;       /* unused on AMD */
    unsigned int       hVirt;         /* out of ALLOC: VA range id */
    unsigned long long size;
    unsigned long long align;
    unsigned long long gpuva;
    unsigned int       isVidmem;
    int                status;
};

struct RD3KRmHandles {
    unsigned int version;
    void        *kapiDevice;          /* device cookie */
    unsigned int hClient;
    unsigned int hDevice;
    unsigned int hSubDevice;
};

/* ---- flips (answered by the framebuffer) ---- */
#define RD3K_FLIP_ABI_VERSION 2u
#define RD3K_FLIP_FN          "rdna3FlipToSurface"
/* RD3KFlipRequest.flags */
#define RD3K_FLIP_FLAG_HOME   1u      /* flip back to the boot scanout */
/* DRIVER-PLAN N38: while the head's previous flip is still pending, answer
 * kIOReturnBusy (flipResult RD3K_FLIP_RESULT_BUSY) and program nothing,
 * instead of waiting up to 20 ms for it (a framebuffer older than this
 * flag ignores it and waits as before) */
#define RD3K_FLIP_FLAG_NOWAIT 2u
#define RD3K_FLIP_RESULT_BUSY (-2)
struct RD3KFlipRequest {
    unsigned int version;
    unsigned int flags;               /* RD3K_FLIP_FLAG_* */
    void        *kapiMemory;
    unsigned int width, height;
    unsigned int pitch;
    int          flipResult;
    unsigned long long allocationCookie;
};

/* DRIVER-PLAN N38: the latch, for the accelerator's flip-done path - the
 * framebuffer sleeps (woken by its VBL thread, at least every 500 us) until
 * the head has no flip pending or timeoutUs (at most 100 ms) passed:
 * kIOReturnSuccess latched, kIOReturnTimeout still pending, kIOReturnNotReady
 * the state unreadable; 'pending' / 'frame' the last state read, 'waitedUs'
 * the time spent. A framebuffer without it answers kIOReturnUnsupported
 * (the accelerator then polls the core's rdna3DisplayStatus itself) */
#define RD3K_FLIP_FN_LATCHWAIT "rdna3FlipLatchWait"
/* version 2 (DRIVER-PLAN N42) appends irqFlags / irqLatUs: whether the
 * head's flip-done interrupt (the HUBP's, DC_IRQ_SOURCE_PFLIPn) is live and
 * ended this wait. The framebuffer answers 1 and 2 (writing the appended
 * fields for 2 only); one older than N42 refuses 2 with kIOReturnBadArgument
 * and the accelerator asks again with 1. */
#define RD3K_FLIPWAIT_VERSION  2u
#define RD3K_FLIPWAIT_MIN      1u
#define RD3K_FLIPWAIT_OK(v)    ((v) >= RD3K_FLIPWAIT_MIN && (v) <= RD3K_FLIPWAIT_VERSION)
#define RD3K_FLIPWAIT_MAX_US   100000u
#define RD3K_FLIPWAIT_IRQ_LIVE 1u     /* the head's pflip interrupt is enabled for the framebuffer */
#define RD3K_FLIPWAIT_IRQ_WOKE 2u     /* a pflip interrupt came during this wait (flip done) */
struct RD3KFlipLatchWait {
    unsigned int version;
    unsigned int timeoutUs;           /* in */
    unsigned int pending, frame;      /* out */
    unsigned int waitedUs, pad;       /* out */
    /* version 2 */
    unsigned int irqFlags;            /* out: RD3K_FLIPWAIT_IRQ_* */
    unsigned int irqLatUs;            /* out, IRQ_WOKE: the core's handler -> this wait saw the latch, us */
};

/* ---- display heads (answered by the core kext, asked by RDNA3Framebuffer):
 *      what the firmware left on screen, flips, frame counter ---- */
#define RD3K_DISPLAY_ABI_VERSION 2u
/* the core still answers a framebuffer built against version 1: a new
 * RDNA3Framebuffer waits for the user's approval in macOS, and until then
 * the one approved earlier runs against the new core. Version 2 only
 * appended RD3KDisplayHotplug's 'hotplug' and 'lost' (not written for 1). */
#define RD3K_DISPLAY_ABI_MIN     1u
#define RD3K_DISPLAY_ABI_OK(v)   ((v) >= RD3K_DISPLAY_ABI_MIN && (v) <= RD3K_DISPLAY_ABI_VERSION)
#define RD3K_DISPLAY_FN_INFO     "rdna3DisplayInfo"
#define RD3K_DISPLAY_FN_FLIP     "rdna3DisplayFlip"
#define RD3K_DISPLAY_FN_STATUS   "rdna3DisplayStatus"
#define RD3K_MAX_HEADS           4
#define RD3K_FMT_XRGB8888        0u   /* IO32BitDirectPixels */
#define RD3K_FMT_XBGR8888        1u
struct RD3KDisplayHead {
    unsigned int       usable;        /* 32 bpp linear, inside BAR0: CPU draws, flips work */
    unsigned int       width, height; /* pixels */
    unsigned int       pitch;         /* bytes */
    unsigned int       format;        /* RD3K_FMT_* */
    unsigned long long phys;          /* CPU-physical (BAR0) address of the boot scanout */
    unsigned long long vramOffset;    /* VRAM offset of the boot scanout */
    unsigned int       hTotal, vTotal, hFront, hSync, vFront, vSync;
    unsigned int       hSyncNeg, vSyncNeg;
    unsigned int       refreshMilliHz;/* measured from the OTG frame counter, 0 = unknown */
};
struct RD3KDisplayInfo {
    unsigned int version;
    unsigned int nheads;
    struct RD3KDisplayHead head[RD3K_MAX_HEADS];
};
#define RD3K_DISPLAY_FLIP_IMMEDIATE 1u
struct RD3KDisplayFlip {
    unsigned int       version;
    unsigned int       head;
    unsigned int       flags;         /* RD3K_DISPLAY_FLIP_* */
    unsigned int       pitch;         /* bytes, multiple of 4 */
    unsigned long long vramOffset;    /* VRAM offset of the new surface (allocationCookie) */
    int                result;        /* 0 or a negative RDNA3_E* code */
};
/* EDID of the sink a head shows (rdna3DisplayEdid). The DP AUX / HPD probe
 * does not know yet which AUX engine feeds which OTG (that mapping is in
 * the VBIOS object table), so the core answers head 0 with the first
 * connected sink that returned an EDID; connector = its AUX engine. */
#define RD3K_DISPLAY_FN_EDID     "rdna3DisplayEdid"
#define RD3K_EDID_MAX            512u
struct RD3KDisplayEdid {
    unsigned int  version;
    unsigned int  head;
    unsigned int  connector;      /* out: AUX engine index */
    unsigned int  length;         /* out: bytes in data, 0 = no EDID */
    unsigned char data[RD3K_EDID_MAX];
};
/* scaled modes: show a smaller surface on the head's native timing through
 * the DPP scaler (dcn32_set_source); width x height == native restores
 * the bypass. result: 0 or a negative RDNA3_E* code */
#define RD3K_DISPLAY_FN_SOURCE   "rdna3DisplaySetSource"
struct RD3KDisplaySource {
    unsigned int       version;       /* RD3K_DISPLAY_ABI_VERSION */
    unsigned int       head;
    unsigned int       width, height; /* source pixels */
    unsigned int       pitch;         /* bytes, multiple of 256 */
    unsigned long long vramOffset;    /* VRAM offset of the surface */
    int                result;
};
/* display sleep: p1 = head, p2 = 1 blank / 0 unblank (HUBP blank, the
 * link stays up); kIOReturnUnsupported unless the core runs with rdna3dpms=1 */
#define RD3K_DISPLAY_FN_BLANK    "rdna3DisplayBlank"
/* real modes (rdna3modeset=1): the timings of the head's sink (EDID) the
 * core can drive on it (dcn32_modeset_plan), and a switch to one of them
 * (dcn32_modeset_apply) with the surface of the new size. index = the
 * position in the last list returned for that head. */
#define RD3K_DISPLAY_FN_MODES    "rdna3DisplayModes"
#define RD3K_DISPLAY_FN_MODE     "rdna3DisplaySetMode"
#define RD3K_MAX_MODES           24u
#define RD3K_MODE_CURRENT        1u   /* the timing the head runs now */
#define RD3K_MODE_PREFERRED      2u   /* the sink's preferred timing */
struct RD3KDisplayTiming {
    unsigned int width, height, pclkKhz, refreshMilliHz;
    unsigned int hTotal, vTotal, hFront, hSync, vFront, vSync, hSyncNeg, vSyncNeg;
    unsigned int flags;           /* RD3K_MODE_* */
};
struct RD3KDisplayModes {
    unsigned int version;
    unsigned int head;
    unsigned int count;           /* out */
    struct RD3KDisplayTiming mode[RD3K_MAX_MODES];
};
struct RD3KDisplayMode {
    unsigned int       version;
    unsigned int       head;
    unsigned int       index;         /* into the head's last RD3KDisplayModes list */
    unsigned int       pitch;         /* bytes, multiple of 256: the new surface */
    unsigned long long vramOffset;
    unsigned int       oldPitch;      /* the surface shown now, back on failure */
    unsigned long long oldVramOffset;
    int                result;        /* 0 or a negative RDNA3_E* code */
};
#define RD3K_DISPLAY_FN_CURSOR   "rdna3DisplayCursor"
#define RD3K_CURSOR_OP_IMAGE     1u   /* vramOffset + size (64/128/256) */
#define RD3K_CURSOR_OP_MOVE      2u   /* x, y (top-left, may be negative), visible */
struct RD3KDisplayCursor {
    unsigned int       version;
    unsigned int       head;
    unsigned int       op;            /* RD3K_CURSOR_OP_* */
    unsigned int       size;          /* square ARGB8888 premultiplied, pitch = size */
    unsigned long long vramOffset;
    int                x, y;
    unsigned int       visible;
    int                result;
};
struct RD3KDisplayStatus {
    unsigned int version;
    unsigned int head;
    unsigned int frame;               /* OTG frame counter (24 bit) */
    unsigned int scanline;
    unsigned int inVblank;
    unsigned int flipPending;
};
/* card run 2026-10-17b: what a head's HUBP scans (hubp2_read_state_common:
 * SURFACE_EARLIEST_INUSE) and what was last requested
 * (PRIMARY_SURFACE_ADDRESS), MC addresses, and the MC address of VRAM offset
 * 0 (fb_start), so the accelerator turns them into its VRAM offsets. A
 * function of its own (not a field of RD3KDisplayStatus): a core without
 * it refuses the call and the trace records 0 (unknown) */
#define RD3K_DISPLAY_FN_SCANADDR "rdna3DisplayScanAddr"
struct RD3KDisplayScanAddr {
    unsigned int version;             /* RD3K_DISPLAY_ABI_VERSION */
    unsigned int head;
    unsigned long long inUse, requested, fbStart;
    unsigned int flipPending, pad;
};

/* hotplug (rdna3hotplug=1, kext/src/RDNA3Hotplug.cpp): the core polls the
 * HPD pins (DC_HPD_SENSE_DELAYED after the toggle filter, debounced) and
 * re-reads a changed sink's EDID; the framebuffer polls this function from
 * its VBL thread (about every half second) and, when the head's generation
 * moved, drops its cached EDID and calls IOFramebuffer's connect-change
 * interrupt, so IOGraphics asks kConnectionCheckEnable / hasDDCConnect /
 * getDDCBlock again. A second monitor's head comes and goes with its own
 * framebuffer (the RDNA3DisplayHead nub). Without rdna3hotplug=1 only
 * 'lost' moves (hotplug 0, generation 0, connected 1).
 * 'lost' moves when the picture in VRAM is gone while the monitor stayed:
 * a wake or a mode-1 reset whose VRAM did not all come back (the
 * eviction, kext/src/RDNA3Evict.cpp, was incomplete or did not run). The
 * framebuffer then raises the same connect-change interrupt, so IOGraphics
 * probes the head again and WindowServer draws everything again. */
#define RD3K_DISPLAY_FN_HOTPLUG  "rdna3DisplayHotplug"
struct RD3KDisplayHotplug {
    unsigned int version;         /* RD3K_DISPLAY_ABI_VERSION */
    unsigned int head;
    unsigned int generation;      /* out: +1 on every debounced change of this head's sink */
    unsigned int connected;       /* out: 1 = the head's connector senses a sink */
    unsigned int pin;             /* out: its HPD pin (HPD1 = 0), ~0u unknown */
    unsigned int events;          /* out: debounced changes on all pins since start */
    unsigned int hotplug;         /* out: 1 = rdna3hotplug polls the pins */
    unsigned int lost;            /* out: +1 every time the picture in VRAM was lost */
};

/* DRIVER-PLAN N42: the display interrupts of a head (amdgpu's
 * dm_crtc_high_irq / dm_pflip_high_irq model). The framebuffer REGISTERs a
 * function the core calls from its interrupt work loop (the bring-up's IH
 * handler, after dc_interrupt_ack) for each VBLANK (OTG VSTARTUP), PFLIP
 * (the head's HUBP flip latched) or VUPDATE interrupt of its head; the core
 * enables those sources (dc_interrupt_set) while a function is registered,
 * the interrupt path is live (rdna3msi=1) and rdna3vblirq / the sysctl
 * debug.rdna3_vblirq allow it, and disables a source that storms. The
 * function must only record and schedule (thread_call_enter, IOLockWakeup):
 * no call back into the core. Calls run under the core's registration lock,
 * so after UNREGISTER returns none runs or will run (the framebuffer
 * unregisters in stop() before its kext can go). STATS: the counters and
 * what is enabled now. Answers: kIOReturnSuccess (out fields valid; 'enabled'
 * may still be 0: 'why' says), kIOReturnNotReady (no display, or the
 * interrupt path not up yet: ask again later), kIOReturnUnsupported
 * (rdna3vblirq=0, or a core older than N42: IOService's answer). */
#define RD3K_DISPLAY_FN_IRQ        "rdna3DisplayIrq"
#define RD3K_DISPIRQ_VERSION       1u
#define RD3K_DISPIRQ_OP_REGISTER   1u
#define RD3K_DISPIRQ_OP_UNREGISTER 2u
#define RD3K_DISPIRQ_OP_STATS      3u
#define RD3K_DISPIRQ_VBLANK        1u   /* DC_IRQ_SOURCE_VBLANKn */
#define RD3K_DISPIRQ_PFLIP         2u   /* DC_IRQ_SOURCE_PFLIPn */
#define RD3K_DISPIRQ_VUPDATE       4u   /* DC_IRQ_SOURCE_VUPDATEn */
/* why a wanted source is not enabled */
#define RD3K_DISPIRQ_WHY_NONE      0u
#define RD3K_DISPIRQ_WHY_SYSCTL    1u   /* debug.rdna3_vblirq=0 */
#define RD3K_DISPIRQ_WHY_NOIRQ     2u   /* no MSI handler (rdna3msi=0, or not registered) */
#define RD3K_DISPIRQ_WHY_STORM     3u   /* disabled after a storm */
#define RD3K_DISPIRQ_WHY_REG       4u   /* the enable bit did not read back */
#define RD3K_DISPIRQ_WHY_HEAD      5u   /* the head is gone (no OTG / HUBP) */
#define RD3K_DISPIRQ_WHY_DOWN      6u   /* the display, or this head alone (still lost after the restore), was not
                                         * brought back after a power loss / mode-1 reset: nothing is written */
typedef void (*RD3KDisplayIrqFn)(void *target, unsigned int head, unsigned int kind, unsigned int frame,
                                 unsigned long long irqAbs);
struct RD3KDisplayIrq {
    unsigned int version;          /* RD3K_DISPIRQ_VERSION */
    unsigned int op;               /* RD3K_DISPIRQ_OP_* */
    unsigned int head;
    unsigned int want;             /* REGISTER: RD3K_DISPIRQ_* sources */
    RD3KDisplayIrqFn fn;           /* REGISTER */
    void *target;                  /* REGISTER: fn's first argument */
    unsigned int enabled;          /* out: the sources enabled now */
    unsigned int why;              /* out: RD3K_DISPIRQ_WHY_* of a wanted source not enabled */
    unsigned int otg, pipe;        /* out: where (OTG for VBLANK / VUPDATE, HUBP for PFLIP) */
    unsigned long long vblank, pflip, vupdate;   /* out: interrupts of this head since start */
    unsigned long long storms;     /* out: storms cut on this head's sources */
};

/* display CRC: p1 = head (uintptr_t), p2 = unsigned *crc out */
#define RD3K_FN_DISPLAY_CRC "rdna3DisplayCrc"

/* output gamma (on unless rdna3gamma=0): the IOFramebuffer::setGammaTable table of a
 * head as the MPCC output gamma (hw/dcn32_gamma.c). channels 1 (one table
 * for R, G and B) or 3 (R, G, B tables one after the other) of count
 * entries, 0..65535; channels 0 = bypass. An identity table also bypasses.
 * kIOReturnUnsupported with rdna3gamma=0. */
#define RD3K_DISPLAY_FN_GAMMA    "rdna3DisplayGamma"
#define RD3K_GAMMA_MAX           1024u
struct RD3KDisplayGamma {
    unsigned int   version;       /* RD3K_DISPLAY_ABI_VERSION */
    unsigned int   head;
    unsigned int   channels;      /* 0, 1 or 3 */
    unsigned int   count;         /* entries per channel, 2..RD3K_GAMMA_MAX */
    unsigned short data[3 * RD3K_GAMMA_MAX];
    int            result;        /* 0 or a negative RDNA3_E* code */
};

/* ---- copies (answered by the core kext, forwarded by the framebuffer):
 *      a pitched rectangle between two VRAM allocations by SDMA, the
 *      accelerator's swap copy onto a scanout (A2) ---- */
#define RD3K_COPY_ABI_VERSION 1u
#define RD3K_COPY_FN          "rdna3CopyVram"
struct RD3KCopyRequest {
    unsigned int       version;
    unsigned int       flags;
    unsigned long long srcCookie, dstCookie;   /* allocationCookie: VRAM offsets */
    unsigned int       srcPitch, dstPitch;     /* bytes */
    unsigned int       rowBytes, rows;
    int                status;                 /* out: 0 or a negative RDNA3_E* code */
    unsigned int       pad;
    unsigned long long usec;                   /* out: the copy's time */
};

/* ---- the IOP copy by SDMA (answered by the core kext, asked by the accelerator directly; DRIVER-PLAN N63) ----
 * The display pipe's copy of a presented IOSurface (system memory) into a head's scanout (VRAM). BIND puts the
 * surface's wired 4 KiB pages into a GART range of the core (amdgpu_ttm_alloc_gart / amdgpu_gart_bind) and answers
 * a handle; COPY has SDMA0 copy a pitched rectangle from that range into VRAM and waits for its fence
 * (amdgpu_copy_buffer); UNBIND drops the range (amdgpu_gart_unbind) - the accelerator unbinds before it lets the
 * pages go. A GPU reset or a wake gives the GART a new table: every handle from before answers STALE (bind again).
 * All three run under the core's DRM lock (it serializes the DMA ring). A core without this function answers
 * kIOReturnUnsupported (IOService's): the accelerator copies with the CPU. */
#define RD3K_IOPSDMA_FN        "rdna3IopSdma"
#define RD3K_IOPSDMA_VERSION   1u
#define RD3K_IOPSDMA_OP_BIND   1u
#define RD3K_IOPSDMA_OP_COPY   2u
#define RD3K_IOPSDMA_OP_UNBIND 3u
/* status (the call answers kIOReturnSuccess with a status for everything but a bad version / op) */
#define RD3K_IOPSDMA_OK        0
#define RD3K_IOPSDMA_NODEV     1   /* no GMC / GART area / SDMA ring (yet): rdna3lvl < 7, a reset under way, stopping */
#define RD3K_IOPSDMA_STALE     2   /* the handle names no bound range (unbound, or bound before a reset / wake) */
#define RD3K_IOPSDMA_NOSPACE   3   /* BIND: no free slot or GART range that large */
#define RD3K_IOPSDMA_INVALID   4   /* bad pages (not page aligned, too many), a rectangle outside the range or VRAM */
#define RD3K_IOPSDMA_FAILED    5   /* the GART map or the SDMA copy failed ('err') */
/* COPY: the fence did not come in time. The copy may still run: the core takes no IOP copy any more and keeps
 * the bound ranges' PTEs (an unbind only unmaps once the ring drained) until the next GART table; the accelerator
 * switches its SDMA path off */
#define RD3K_IOPSDMA_TIMEOUT   6
struct RD3KIopSdma {
    unsigned int       version;     /* RD3K_IOPSDMA_VERSION */
    unsigned int       op;          /* RD3K_IOPSDMA_OP_* */
    int                status;      /* out: RD3K_IOPSDMA_* */
    int                err;         /* out: the RDNA3_E* code behind FAILED / INVALID */
    unsigned long long handle;      /* BIND out, COPY / UNBIND in */
    /* BIND */
    const unsigned long long *pages;   /* bus addresses, 4 KiB each, in the range's order */
    unsigned long long pageCount;
    unsigned long long gartOffset;  /* out: the range's GART byte offset (for the log) */
    unsigned long long liveRanges;  /* out (all ops): ranges bound now */
    /* COPY: rows of rowBytes from byte srcOffset of the range, srcPitch apart, to VRAM offset dstCookie (the
     * scanout's allocationCookie), dstPitch apart */
    unsigned long long srcOffset;
    unsigned long long dstCookie;
    unsigned int       srcPitch, dstPitch, rowBytes, rows;
    unsigned long long usec;        /* out: the op's time (the lock wait included) */
};

/* ---- GPU busy time (answered by the core kext; DRIVER-PLAN N49) ----
 * The accelerator's 250 ms perfstats tick takes the kernel rings' busy time
 * (common/rdna3_gpu_busy.h: amdgpu_ctx_fence_time's model) for Activity
 * Monitor: the totals give "Device Utilization %" (the SMU's GFX activity
 * reads 100 % at idle, P2), the per-process GPU time since the last take is
 * added to the process's IOAccelSubmitter2 "accumulatedGPUTime". */
#define RD3K_GPUBUSY_FN      "rdna3GpuBusyTake"
#define RD3K_GPUBUSY_VERSION 2u   /* 2: cappedNs, uncounted[], sdmaEnds[] (ECC round 1) */
#define RD3K_GPUBUSY_PIDS    32u
#define RD3K_GPUBUSY_RINGS   3u   /* GFX, compute, SDMA (RD3_BUSY_*) */
#define RD3K_GPUBUSY_UNC     5u   /* not counted: VCN, JPEG, other rings, kernel SDMA copies, user queues */
#define RD3K_GPUBUSY_BY      5u   /* SDMA job ends by: interrupt, waiter, take, submission, reset */
struct RD3KGpuBusyPid {
    int                pid;
    unsigned int       jobs;           /* jobs ended since the last take */
    unsigned long long gpuNs;          /* their GPU time */
    unsigned long long lastSubmitAbs;  /* mach_absolute_time of the newest submission */
};
struct RD3KGpuBusy {
    unsigned int       version;        /* in: RD3K_GPUBUSY_VERSION */
    unsigned int       npids;          /* out: entries in pids[] */
    unsigned long long nowNs;          /* out: the core's clock at the take */
    unsigned long long busyNs;         /* out: any ring with work, total since load (up to nowNs) */
    unsigned long long ringNs[RD3K_GPUBUSY_RINGS];
    unsigned long long jobs, merged, flushed, stale, lostJobs, lostNs;   /* out: totals */
    unsigned int       inflight;       /* out: jobs in flight at the take */
    unsigned int       pad;
    unsigned long long cappedNs;       /* out: process time dropped above the wall time (total) */
    unsigned long long uncounted[RD3K_GPUBUSY_UNC];   /* out: submissions / copies / queues the busy time misses */
    unsigned long long sdmaEnds[RD3K_GPUBUSY_BY];     /* out: SDMA job ends per fence reader */
    struct RD3KGpuBusyPid pids[RD3K_GPUBUSY_PIDS];
};

/* user clients: newUserClient types 'RD'xxxx go to the core kext */
#define RD3K_UC_TYPE_BASE     0x52440000u
#define RD3K_FN_NEW_CLIENT    "rdna3NewAccelClient"

/* ---- IOSurface sharing escapes ---- */
#define RD3K_ESC_SURF_VRAM     0xE2u
#define RD3K_ESC_SURF_DIRTY    0xE3u
#define RD3K_SURFSHARE_VERSION 2u
#define RD3K_SS_BAD_VERSION   0x80000001u
#define RD3K_SS_OFF           0x80000002u
#define RD3K_SS_NO_SURFACE    0x80000003u
#define RD3K_SS_NO_CACHE      0x80000004u
#define RD3K_SS_NO_VRAM       0x80000005u
#define RD3K_SS_TOO_SMALL     0x80000006u
#define RD3K_SS_DISPLAY       0x80000007u   /* WindowServer's display framebuffer: kept out of A3 (card run 58) */
#define RD3K_SS_POLICY        0x80000008u   /* kept out of A3 by policy: WindowServer's inputs, or rdna3a3=0 (card run 58) */
/* the highest IOSurface plane an escape may name: the caller's 32-bit plane
 * reaches IOGraphicsAccelerator2::deviceCacheForIOSurface, and the device
 * cache's validity bits (valid[0], valid[6]) hold 16 planes. A plane above
 * it is refused with RD3K_SS_NO_CACHE before the lookup */
#define RD3K_SS_MAX_PLANE     15u
static inline int rd3k_ss_plane_ok(unsigned int plane) { return plane <= RD3K_SS_MAX_PLANE; }
/* DRIVER-PLAN N41: flags in the plane word of RD3K_ESC_SURF_DIRTY (the same
 * bits as common/rdna3_surf_pending.h RD3_SP_PLANE_*; uapi RD_XNU_SURFACE_BO_MARK
 * and _DIRTY_TRY set them). GPU_MARK: not a dirty mark but WindowServer's
 * pending (bit 0 set) / ready (bit 0 clear) mark of a surface its GPU
 * writes; only WindowServer's marks are taken, answered RD3K_SS_OFF while
 * debug.rdna3accel_surface_wait is 0. TRYLOCK: a dirty mark that gives up
 * with RD3K_SS_BUSY instead of waiting for the accelerator's lock (a
 * page-off may hold it while it waits for a pending mark). The plugin
 * learns the mode from the answer to its pending marks (an older
 * accelerator answers RD3K_SS_NO_CACHE: a plane out of range). */
#define RD3K_SS_PLANE_GPU_MARK 0x80000000u
#define RD3K_SS_PLANE_TRYLOCK  0x40000000u
#define RD3K_SS_BUSY          0x80000009u   /* TRYLOCK: the accelerator's lock was taken; nothing marked */
/* DRIVER-PLAN N71 (c), two more flags of the dirty escape's plane word.
 * SHARE: the producer's share record: an application that holds an A3
 * import of the plane records that WindowServer may import this allocation
 * READ-ONLY; 'size' carries the allocation generation of the caller's own
 * import (the core passes its BO's, never a user value). Answers: 0,
 * RD3K_SS_OFF (debug.rdna3accel_a3share 0), RD3K_SS_POLICY (WindowServer,
 * or an import of another allocation than the live one), RD3K_SS_NO_SURFACE,
 * RD3K_SS_NO_CACHE / _NO_VRAM. An older accelerator answers RD3K_SS_NO_CACHE
 * (a plane out of range): no share.
 * SHARE_FAULT: the core reports a GPU VM fault inside a read-only shared
 * import (its importer's task); 'size' the fault VA, hClient the producer's
 * pid. The accelerator turns sharing off (debug.rdna3accel_a3share 0) and
 * logs it. Never reachable from a user plane (the UAPI keeps planes <= 15). */
#define RD3K_SS_PLANE_SHARE       0x20000000u
#define RD3K_SS_PLANE_SHARE_FAULT 0x10000000u
struct RD3KSurfShareEsc {
    unsigned int version;
    unsigned int surfaceID;
    unsigned int plane;
    unsigned int hClient;
    unsigned int hSrcClient;
    unsigned int hSrcMemory;
    unsigned int status;
    unsigned int pad;
    unsigned long long size;
};

/* RD3K_ESC_SURF_GEM: the core kext asks the accelerator (RD3ACCEL_SS_FN,
 * p2 the client's task, p3 this cmd, p1 a struct RD3KSurfGem) for the
 * VRAM allocation behind an IOSurface plane, for a GEM BO over it
 * (RD_NR_XNU_SURFACE_BO). The accelerator takes a reference on it with
 * RD3K_VRAM_FN_REF while it still holds the surface, so the allocation
 * outlives its own free until the core drops that reference. */
#define RD3K_ESC_SURF_GEM      0xE4u
/* 2 (N71 (c)): the out tail (flags, sharePid, allocGen). The accelerator
 * answers both; it writes the tail only for a v2 caller. A v1 caller
 * cannot make a BO read-only, so it never gets a shared import (refused as
 * before); a new core retries v1 when an older accelerator answers
 * RD3K_SS_BAD_VERSION. */
#define RD3K_SURFGEM_VERSION   2u
#define RD3K_SURFGEM_VERSION_1 1u
/* out flags: SHARED_RO = WindowServer's read-only import of an application's
 * shared allocation (N71 (c)): the core must give it no CPU mapping and no
 * writable GPU page */
#define RD3K_SG_SHARED_RO      0x1u
#define RD3K_VRAM_FN_REF      "rdna3RefVram"
/* DRIVER-PLAN N41 (security review): the core's reference count of an
 * accelerator allocation (p1 the handle, p2 unsigned int * out): 1 only the
 * accelerator's own, more a GEM BO holds it. kIOReturnNotFound for a handle
 * the core does not know; an older core answers kIOReturnUnsupported. The
 * accelerator releases a parked allocation under VRAM pressure only when
 * no BO holds it (releasing a held one frees nothing). */
#define RD3K_VRAM_FN_REFS     "rdna3VramRefs"
struct RD3KSurfGem {
    unsigned int version;
    unsigned int surfaceID, plane;     /* in */
    unsigned int status;               /* out: 0 or RD3K_SS_* */
    void *kapiMemory;                  /* out: the allocation, referenced */
    unsigned long long bytes;          /* out */
    /* v2 (N71 (c)); a v1 caller's struct ends here (RD3K_SURFGEM_V1_SIZE) */
    unsigned int flags;                /* out: RD3K_SG_* */
    int sharePid;                      /* out: SHARED_RO: the producer that recorded the share */
    unsigned long long allocGen;       /* out: the allocation generation the reference is of (never 0) */
};
#define RD3K_SURFGEM_V1_SIZE 32u   /* sizeof the v1 struct: version .. bytes */

#ifdef RD3K_SS_KERNEL_ABI
#define RD3ACCEL_SS_FN     "rdna3SurfVram"
#define RD3K_SS_FN_PAGEOFF "rdna3SurfPageoff"
#define RD3K_SS_FN_RM      "rdna3SurfShare"
#define RD3K_SS_RM_VERSION 2u
enum { RD3K_SS_RM_SHARE_KAPI = 5 };
struct RD3KSurfShareRm {
    unsigned int version, op;
    void *kapiDevice, *kapiMemory;
    unsigned int target;
    unsigned int hClient, hObject;
    unsigned int status;
};
struct RD3KSurfPageoff {
    unsigned int version;
    unsigned int status;
    void *kapiDevice, *kapiMemory;
    const unsigned long long *pages;
    unsigned long long pageCount;
    unsigned long long length;
    unsigned long long usec;
};
#endif

#endif /* RDNA3K_ABI_H */
