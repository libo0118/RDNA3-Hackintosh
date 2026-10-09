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
struct RD3KFlipRequest {
    unsigned int version;
    unsigned int flags;               /* 1 = flip back to the boot scanout */
    void        *kapiMemory;
    unsigned int width, height;
    unsigned int pitch;
    int          flipResult;
    unsigned long long allocationCookie;
};

/* ---- display heads (answered by the core kext, asked by RDNA3Framebuffer):
 *      what the firmware left on screen, flips, frame counter ---- */
#define RD3K_DISPLAY_ABI_VERSION 1u
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

/* display CRC: p1 = head (uintptr_t), p2 = unsigned *crc out */
#define RD3K_FN_DISPLAY_CRC "rdna3DisplayCrc"

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
