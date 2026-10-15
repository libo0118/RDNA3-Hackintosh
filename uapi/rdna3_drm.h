/*
 * rdna3_drm.h - the Linux DRM + amdgpu UAPI, implemented OS-independently.
 *
 * Userspace (libdrm / libdrm_amdgpu / RADV) issues exactly the ioctls it
 * issues on Linux; the userspace shim (userspace/libdrm_xnu) forwards
 * (nr, payload) to the kext's IOUserClient, which calls
 * rdna3_drm_ioctl(). Struct layouts are the real ABI from
 * include/uapi/drm/amdgpu_drm.h and drm.h (third_party/amd).
 *
 * Status of each ioctl is tracked in docs/STAGE4.md. Work for the GPU (CS,
 * user queues) goes to the OS layer's rings (rdna3_drm_ops.submit): CS is
 * -ENODEV without one, and a ring the OS has not brought up is -EINVAL (as
 * amdgpu for a ring that is not ready; -ENODEV on an enumeration-only
 * device).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DRM_H
#define RDNA3_DRM_H

#include <stdint.h>
#include <stddef.h>
#include "amdgpu_drm.h"
#include "amd_discovery.h"
#include "amd_mqd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux errno values: the shim returns them to libdrm as-is */
enum {
    RD_EPERM = 1, RD_ENOENT = 2, RD_EINTR = 4, RD_EIO = 5, RD_ENXIO = 6, RD_EBADF = 9, RD_EACCES = 13,
    RD_ENOMEM = 12, RD_EFAULT = 14, RD_EBUSY = 16, RD_EINVAL = 22, RD_ENOSPC = 28,
    RD_ECANCELED = 89,
    RD_ENOSYS = 38, RD_ENODEV = 19, RD_ENODATA = 61, RD_ETIME = 62, RD_EOPNOTSUPP = 95,
};

/* ioctl numbers (the 'nr' byte of DRM_IO*, drm.h) */
#define RD_NR_VERSION              0x00
#define RD_NR_GEM_CLOSE            0x09
#define RD_NR_GET_CAP              0x0c
#define RD_NR_SET_CLIENT_CAP       0x0d
#define RD_NR_PRIME_HANDLE_TO_FD   0x2d
#define RD_NR_PRIME_FD_TO_HANDLE   0x2e
#define RD_NR_SYNCOBJ_CREATE       0xBF
#define RD_NR_SYNCOBJ_DESTROY      0xC0
#define RD_NR_SYNCOBJ_HANDLE_TO_FD 0xC1
#define RD_NR_SYNCOBJ_FD_TO_HANDLE 0xC2
#define RD_NR_SYNCOBJ_WAIT         0xC3
#define RD_NR_SYNCOBJ_RESET        0xC4
#define RD_NR_SYNCOBJ_SIGNAL       0xC5
#define RD_NR_SYNCOBJ_TIMELINE_WAIT   0xCA
#define RD_NR_SYNCOBJ_QUERY        0xCB
#define RD_NR_SYNCOBJ_TRANSFER     0xCC
#define RD_NR_SYNCOBJ_TIMELINE_SIGNAL 0xCD
#define RD_NR_AMDGPU(x)            (DRM_COMMAND_BASE + (x))
/* private, not a Linux DRM ioctl: read or set a syncobj's fence by value,
 * so libdrm_xnu can carry a sync_file across processes as a file holding
 * (ip, ring, seq) - sequence numbers are per device, not per file */
#define RD_NR_XNU_SYNCOBJ_FENCE    0xF0
struct rd_xnu_syncobj_fence {
    uint32_t handle, op;           /* RD_XNU_FENCE_GET / _SET */
    uint32_t ip, ring;
    uint64_t seq;                  /* 0 with signaled = 0: no fence (reset) */
    uint32_t signaled, pad;
};
#define RD_XNU_FENCE_GET 0
#define RD_XNU_FENCE_SET 1
/* private: PRIME without file descriptors in the kernel. EXPORT gives a
 * device-wide random token for a BO (the same one while it stays
 * exported); IMPORT turns a token into a handle of the calling file (the
 * handle it already has for that BO, as PRIME does). libdrm_xnu carries
 * the token in an unlinked file sized like the BO, which is the "dma-buf
 * fd" (holding the fd is the capability; libdrm reads the size with
 * lseek). An export lives while its exporter's file is open or another
 * file still holds the BO. Userptr BOs are not exported (-EPERM, as
 * amdgpu_gem_prime_export). */
#define RD_NR_XNU_BO_EXPORT        0xF1
#define RD_NR_XNU_BO_IMPORT        0xF2
struct rd_xnu_bo_share {
    uint32_t handle, pad;          /* EXPORT: in; IMPORT: out */
    uint64_t token;                /* EXPORT: out; IMPORT: in */
    uint64_t size;                 /* out */
};
#define RD_MAX_EXPORTS 1024
#define RD_MAX_EXPORTS_PER_FILE (RD_MAX_EXPORTS / 4)   /* no client fills the device's table */
/* private, root only: what display pipe 'pipe' scans out right now
 * (HUBP/OTG read back) and rows [row0, row0 + nrows) of it scaled to
 * out_w x out_h, RGB888 - the report's proof of what reaches each panel.
 * nrows 0: the surface cannot be sampled (tiled, DCC, outside the BAR). */
#define RD_NR_XNU_SCANOUT          0xF3
#define RD_XNU_SCANOUT_W           320
#define RD_XNU_SCANOUT_ROWS        4
struct rd_xnu_scanout {
    uint32_t pipe, row0;                           /* in */
    uint32_t powered, lit, otg, format, sw_mode, dcc;
    uint32_t width, height, pitch_px, flip_pending;
    uint32_t out_w, out_h, nrows, blanked;         /* blanked: HUBP_BLANK_EN */
    uint64_t address, inuse;                       /* MC addresses */
    uint32_t underflow;                            /* HUBP_UNDERFLOW_STATUS (sticky) */
    uint32_t crc_valid, crc_rg, crc_b;             /* OTG CRC0 of the frame sent (row0 0 only) */
    uint8_t  rgb[RD_XNU_SCANOUT_W * RD_XNU_SCANOUT_ROWS * 3];
    /* what the pipe has to keep up with (rdna3_scanout_info): DET and
     * compressed buffer (64 KiB segments), DISPCLK and the pipe's DPPCLK,
     * DSCL_MODE / LB_MEMORY_CTRL / DSCL_MEM_PWR_CTRL, DLG DST_AFTER_SCALER /
     * REFCYC_PER_REQ_DELIVERY / REFCYC_PER_LINE_DELIVERY_L */
    uint32_t det_segs, compbuf_segs, dispclk_khz, dppclk_khz;
    uint32_t dscl_mode, lb_memory_ctrl, dscl_mem_pwr;
    uint32_t dlg_after_scaler, dlg_req_delivery, dlg_line_delivery;
    uint32_t flags;                                /* in (row0 0): RD_XNU_SCANOUT_* */
};
/* after the read, clear the sticky HUBP underflow status (hubp2_clear_underflow):
 * a later look shows whether it comes back */
#define RD_XNU_SCANOUT_CLEAR_UNDERFLOW 1u
/* every call (not only row0): sample at RD_XNU_SCANOUT_HIRES_W wide (at
 * most the surface's width), as many rows per call as rgb[] holds - card
 * run 54: stale regions in apps were too small to see at 320 wide */
#define RD_XNU_SCANOUT_HIRES           2u
#define RD_XNU_SCANOUT_HIRES_W         1280
/* private: this file's device-wide id (non-zero, never reused while the
 * device lives). DRM_AMDGPU_SCHED names the file whose contexts it changes
 * by a DRM fd; the kext has no fds, so libdrm_xnu puts the id of that fd's
 * connection into drm_amdgpu_sched_in.fd (0: not a DRM fd). */
#define RD_NR_XNU_FILE_ID          0xF4
struct rd_xnu_file_id {
    uint32_t id, pad;              /* out */
};

/* private: a GEM BO over the VRAM the accelerator (IOGraphicsAccelerator2)
 * keeps for an IOSurface's plane, so RADV renders where WindowServer and
 * the display read the surface, without a copy per frame (A3; the NVK
 * route of the Metal plugin did the same with RM handles). IMPORT looks the
 * surface up in the calling process (its IOSurface access rules) and gives
 * a new handle; the BO keeps that VRAM allocated while it lives. DIRTY
 * marks the plane's VRAM newer than the surface's system-memory copy after
 * a GPU write, so a CPU lock pages it off instead of reading stale bytes.
 * Such a BO cannot be exported (-EPERM): each process imports for itself.
 * -ENOSPC also when the file would hold more than RD_SURFACE_BO_FILE_MAX.
 * -ENOENT: no such surface for this process; -ENODEV: the surface has no
 * VRAM of the accelerator's (not drawn by it yet); -ENXIO: the accelerator
 * is not up (may pass); -ENOSYS: the OS has no hook. A kext without this
 * ioctl answers -EINVAL. */
#define RD_NR_XNU_SURFACE_BO       0xF5
#define RD_XNU_SURFACE_BO_IMPORT   0
#define RD_XNU_SURFACE_BO_DIRTY    1
#define RD_XNU_SURFACE_MAX_PLANE   15
/* the most accelerator VRAM one file may hold through surface BOs */
#define RD_SURFACE_BO_FILE_MAX(dev) ((dev)->vis_vram_size / 2)
struct rd_xnu_surface_bo {
    uint32_t op;                   /* RD_XNU_SURFACE_BO_* */
    uint32_t surface_id, plane;    /* in */
    uint32_t handle;               /* IMPORT: out */
    uint64_t size;                 /* IMPORT: in, the bytes the caller will address (0: any;
                                      -ENOSPC when the VRAM is smaller); out, the BO's size */
};

/* We advertise the amdgpu DRM version RADV/libdrm expect from Linux 6.x */
#define RD_DRM_MAJOR 3
#define RD_DRM_MINOR 61
#define RD_DRM_PATCH 0

#define RD_MAX_HANDLES  16384
#define RD_MAX_CTX      256
#define RD_MAX_ORPHANS  16      /* closed files whose job never finished (one per VMID) */
#define RD_MAX_SYNCOBJ  4096
#define RD_MAX_VA       65536
#define RD_TA_TYPES     16      /* > TA_FW_TYPE_MAX_INDEX (amdgpu_ucode.h) */

struct rdna3_drm_device;

/* A buffer object. 'backing' is owned by the OS layer. */
typedef struct rdna3_bo {
    uint32_t refcnt;
    uint64_t size;
    uint64_t alignment;
    uint32_t domains;         /* AMDGPU_GEM_DOMAIN_* requested */
    uint32_t placement;       /* domain actually used */
    uint64_t flags;
    uint64_t vram_offset;     /* VRAM placement */
    void    *backing;         /* OS object (IOBufferMemoryDescriptor, malloc...) */
    uint64_t mmap_offset;     /* fake offset handed to mmap() */
    uint8_t  metadata[256];
    uint32_t metadata_size;
    uint64_t tiling_info;
    uint8_t  userptr;         /* GEM_USERPTR: backing wraps the client's pages, no mmap */
    uint8_t  userptr_ro;      /* AMDGPU_GEM_USERPTR_READONLY: never mapped writable */
    uint8_t  foreign;         /* RD_NR_XNU_SURFACE_BO: VRAM the accelerator owns; bo_free drops
                                 the OS's hold on it, no VRAM usage is counted */
    uint32_t visit;           /* rdna3_drm_device_for_each_vram_bo: seen in this pass */
    uint64_t user_addr;
} rdna3_bo;

typedef struct rdna3_va {
    uint32_t handle;          /* 0 = free slot */
    uint64_t va, offset, size, flags;
    rdna3_bo *bo;             /* referenced while mapped */
} rdna3_va;

/* a GPU fence: ring 'ring' of HW IP 'ip' passes sequence number 'seq' */
typedef struct rdna3_fence {
    uint32_t ip, ring;
    uint64_t seq;
} rdna3_fence;

typedef struct rdna3_syncobj {
    uint8_t  used, signaled, has_fence;
    uint64_t point;
    rdna3_fence fence;        /* when has_fence: signaled once it passes */
} rdna3_syncobj;

/* what a CS carries besides its IBs (amdgpu_job) */
typedef struct rdna3_cs_job {
    /* AMDGPU_CHUNK_ID_FENCE: once the job is done the GPU writes its 64-bit
     * sequence number at 'uf_offset' in the one-page BO 'uf_bo' (NULL: none) */
    rdna3_bo *uf_bo;
    uint64_t uf_offset;
    /* the ring's last handle: the user fence holds the ring's 32-bit fence
     * widened past it, the same number the CS ioctl returns */
    uint64_t seq_base;
    /* the GDS (bytes) / GWS (entries) / OA (counters) partitions of the
     * BO list's on-chip BOs; size 0: none (amdgpu_job_set_resources) */
    uint32_t gds_base, gds_size, gws_base, gws_size, oa_base, oa_size;
    /* AMDGPU_CHUNK_ID_CP_GFX_SHADOW (amdgpu_cs_p2_shadow -> job->shadow_va,
     * csa_va, gds_va, init_shadow): the gfx ring's SET_Q_PREEMPTION_MODE
     * (gfx_v11_0_ring_emit_gfx_shadow) when the CP shadows state; 0 = none.
     * Other rings and a ring without cp_gfx_shadow ignore them, as amdgpu. */
    uint64_t shadow_va, csa_va, gds_va;
    uint32_t init_shadow;
} rdna3_cs_job;

/* GFX 11 on-chip resources (gfx_v11_0_set_gds_init) */
#define RD_GDS_SIZE 0x1000
#define RD_GWS_SIZE 64
#define RD_OA_SIZE  16
#define RD_ONCHIP_DOMAINS (AMDGPU_GEM_DOMAIN_GDS | AMDGPU_GEM_DOMAIN_GWS | AMDGPU_GEM_DOMAIN_OA)
/* BO lists (AMDGPU_BO_LIST) per file: everything is resident, so a list
 * only remembers its on-chip BOs */
#define RD_MAX_BO_LISTS 256
/* user fences the GPU may still write: their BOs stay referenced */
#define RD_MAX_UF_PENDING 64

/* gfx_v11_0_get_gfx_shadow_info: what a gfx11 CP shadow area and its
 * firmware work area (CSA) need (AMDGPU_INFO_DEV_INFO shadow_* / csa_*) */
#define RD_MQD_SHADOW_BASE_SIZE      73728
#define RD_MQD_SHADOW_BASE_ALIGNMENT 256
#define RD_MQD_FWWORKAREA_SIZE       484
#define RD_MQD_FWWORKAREA_ALIGNMENT  256
#define RD_MAX_CS_IBS 8
typedef struct rdna3_ib {
    uint64_t va;              /* GPU VA in the submitting file's VM */
    uint32_t length_dw;
    uint32_t flags;           /* AMDGPU_IB_FLAG_* */
    /* a parse_cs ring (VCN_JPEG): the checked copy of the IB the engine
     * must run instead of 'va' (amdgpu_cs_patch_ibs copies it into a
     * kernel IB); valid during ops.submit only. NULL for other rings. */
    const uint32_t *kdata;
} rdna3_ib;

/* VCN 4.0: two instances at most, one unified ring each */
#define RD_VCN_MAX_INST 2

/* ---- user queues (amdgpu_userq.c, amdgpu_userq_fence.c, mes_userqueue.c) ----
 * A user queue is a ring in the client's VM that the MES scheduler runs
 * from a doorbell the client rings itself; the kernel only builds its MQD,
 * hands it to the MES (ADD_QUEUE / REMOVE_QUEUE) and keeps its fence
 * driver: a 64-bit seq64 slot the firmware writes the queue's rptr to. A
 * user queue fence is "slot >= value". In a syncobj it is an rdna3_fence
 * with ip RD_USERQ_FENCE_IP, ring = the fence driver's context and seq =
 * the value; once that context is gone the fence counts as signaled (amdgpu
 * signals a dying fence driver's fences with -ECANCELED). */
#define RD_MAX_USERQ          64      /* per file, ids 1..63 (AMDGPU_MAX_USERQ_COUNT 512) */
#define RD_USERQ_FENCE_IP     0x100u  /* rdna3_fence.ip of a user queue fence */
#define RD_MAX_FENCE_DRV      256     /* fence drivers alive at once, device-wide */
#define RD_USERQ_WAIT_REFS    64      /* fence drivers one queue may wait on (its fence_drv_xa) */
#define RD_USERQ_DEFERRED     4096    /* references parked on not yet signaled fences */
/* what one client may hold of the device-wide tables above: one queue's
 * driver each plus room for the ones its own fences still reference */
#define RD_FENCE_DRV_PER_FILE     (RD_MAX_FENCE_DRV / 2)
#define RD_USERQ_DEFERRED_PER_FILE (RD_USERQ_DEFERRED / 4)
/* amdgpu_seq64: one kernel BO of 64-bit slots, mapped read-only into every
 * user queue client's VM at AMDGPU_VA_RESERVED_SEQ64_START (sign-extended).
 * 64 KiB instead of the 2 MiB reservation: 8192 slots. */
#define RD_SEQ64_SIZE         (64u << 10)
#define RD_SEQ64_SLOTS        (RD_SEQ64_SIZE / 8)
#define RD_SEQ64_VA           0xFFFFFFFFFFC00000ull   /* amdgpu_seq64_get_va_base */
/* the doorbell BAR: the kernel doorbells and the MES page first
 * (amdgpu_doorbell_create_kernel_doorbells: 0x591 dwords page-aligned + a
 * page), then doorbell BOs, a page at a time */
#define RD_DOORBELL_KERNEL_SIZE (12u << 10)
#define RD_MAX_DOORBELL_PAGES 2048

enum { RD_USERQ_STATE_UNMAPPED = 0, RD_USERQ_STATE_MAPPED, RD_USERQ_STATE_PREEMPTED,
       RD_USERQ_STATE_HUNG };

/* amdgpu_userq_obj: a kernel BO with its CPU view and VMID 0 address */
typedef struct rdna3_userq_obj {
    rdna3_bo *obj;
    void     *cpu_ptr;
    uint64_t  gpu_addr;
} rdna3_userq_obj;

/* amdgpu_userq_fence_driver */
typedef struct rdna3_userq_fence_drv {
    uint32_t refcnt;                 /* 0: free entry */
    uint32_t context;                /* dma_fence context, never reused */
    uint64_t va, gpu_addr;           /* seq64 slot: in the VMs, in VMID 0 */
    volatile uint64_t *cpu_addr;
    uint32_t doorbell_index;         /* adev->userq_xa key */
    uint64_t last_seq;               /* highest fence value created on it */
    const struct rdna3_drm_file *owner;   /* the client that made it, NULL once it closed */
} rdna3_userq_fence_drv;

/* amdgpu_usermode_queue */
typedef struct rdna3_userq {
    uint32_t used;
    uint32_t queue_type;             /* AMDGPU_HW_IP_GFX / COMPUTE / DMA */
    uint32_t state;                  /* RD_USERQ_STATE_* */
    uint32_t doorbell_handle;
    uint32_t doorbell_index;
    int32_t  priority;               /* AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_* */
    amd_mqd_prop prop;               /* userq_prop */
    rdna3_userq_obj mqd, fw_obj, wptr_obj;
    rdna3_bo *db_obj;
    uint32_t fence_ctx;              /* its fence driver, by context */
    uint32_t fence_drv_xa[RD_USERQ_WAIT_REFS];   /* fence drivers it waits on (referenced) */
    uint32_t nxa;
    rdna3_fence last_fence;          /* ip 0: none yet */
} rdna3_userq;

typedef struct rdna3_drm_ops {
    void *ctx;
    void *(*zalloc)(void *ctx, size_t size);
    void  (*free)(void *ctx, void *p, size_t size);
    /* back a BO (domain chosen in bo->placement); returns 0 or -errno */
    int   (*bo_alloc)(void *ctx, rdna3_bo *bo);
    void  (*bo_free)(void *ctx, rdna3_bo *bo);
    /* user memory access for pointers embedded in ioctl structs */
    int   (*copyin)(void *ctx, void *dst, uint64_t uaddr, size_t len);
    int   (*copyout)(void *ctx, uint64_t uaddr, const void *src, size_t len);
    /* GPU timestamp in counter ticks (AMDGPU_INFO_TIMESTAMP), may be NULL */
    uint64_t (*gpu_clock)(void *ctx);
    /* MMIO read for AMDGPU_INFO_READ_MMR_REG (whitelisted regs only), may be NULL */
    uint32_t (*read_reg)(void *ctx, uint32_t dword_offset);
    /* GPU page tables, all optional (NULL = mappings are only recorded):
     * vm_create when a file opens, vm_map/vm_unmap for GEM_VA (va, size and
     * offset page aligned, flags = AMDGPU_VM_PAGE_*), vm_destroy on close. */
    void *(*vm_create)(void *ctx);
    void  (*vm_destroy)(void *ctx, void *vm);
    int   (*vm_map)(void *ctx, void *vm, rdna3_bo *bo, uint64_t va, uint64_t offset,
                    uint64_t size, uint32_t flags);
    void  (*vm_unmap)(void *ctx, void *vm, uint64_t va, uint64_t size);
    /* command submission, optional (NULL = CS is -ENODEV). Only rings the
     * OS marked in available_rings reach submit. submit queues the IBs on
     * (ip, ring) in 'vm' and returns the fence sequence; fence_signaled
     * polls; fence_wait blocks until 'seq' passed or the absolute
     * CLOCK_MONOTONIC deadline (ns, UINT64_MAX = forever), 0 / -RD_ETIME.
     * 'job': user fence and GDS/GWS/OA partitions (never NULL). */
    int   (*submit)(void *ctx, void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs,
                    uint32_t nibs, const rdna3_cs_job *job, uint64_t *seq);
    int   (*fence_signaled)(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq);
    int   (*fence_wait)(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t abs_ns);
    /* GPU reset / fault state of a VM for AMDGPU_CTX_OP_QUERY_STATE*:
     * AMDGPU_CTX_QUERY2_FLAGS_RESET (a reset happened since the VM was
     * made), _GUILTY (one of its jobs hung or faulted). Optional; a guilty
     * VM's submissions fail with -ECANCELED like amdgpu's. _VRAMLOST is
     * ignored: the UAPI keeps it per context (rdna3_drm_device_vram_lost). */
    uint32_t (*vm_query)(void *ctx, void *vm);
    /* GEM_USERPTR: wire the client's pages [uaddr, uaddr + bo->size) for the
     * GPU (file_os = rdna3_drm_file.os, the owning task); optional.
     * flags = AMDGPU_GEM_USERPTR_*. */
    int   (*bo_userptr)(void *ctx, void *file_os, rdna3_bo *bo, uint64_t uaddr, uint32_t flags);
    /* AMDGPU_INFO_SENSOR: AMDGPU_INFO_SENSOR_* -> value in amdgpu's units
     * (MHz, millidegrees, %, W, mV); optional, -EINVAL when missing */
    int   (*sensor)(void *ctx, uint32_t type, uint32_t *value);
    /* AMDGPU_INFO_GPUVM_FAULT: the last page fault of this VM (addr, hub
     * L2_PROTECTION_FAULT_STATUS, AMDGPU_VMHUB_TYPE_*); optional */
    int   (*vm_fault_info)(void *ctx, void *vm, uint64_t *addr, uint32_t *status, uint32_t *vmhub);
    /* 64 random bits for BO export tokens; optional (a counter mixed with
     * the GPU clock otherwise, guessable) */
    uint64_t (*random64)(void *ctx);
    /* RD_NR_XNU_SCANOUT: fill everything after row0 (pipe and row0 are
     * set); optional, -EOPNOTSUPP when missing. The OS checks privilege. */
    int   (*scanout)(void *ctx, struct rd_xnu_scanout *s);
    /* may the calling thread raise scheduling priority? Linux: DRM master
     * (drm_is_current_master) or CAP_SYS_NICE; the OS's stand-in (root on
     * macOS). Gates DRM_AMDGPU_SCHED and context priorities above NORMAL.
     * Optional: NULL = never. */
    int   (*privileged)(void *ctx);
    /* ---- user queues, all optional; DRM_AMDGPU_USERQ needs them and an
     * rdna3_drm_device_set_userq() from the OS once the MES runs ---- */
    /* amdgpu_bo_kmap: the kernel's CPU view of a BO; NULL: none (VRAM
     * outside the visible BAR, a userptr BO) */
    void *(*bo_kmap)(void *ctx, rdna3_bo *bo);
    /* amdgpu_bo_gpu_offset after amdgpu_ttm_alloc_gart: the BO's address
     * in VMID 0, binding a GTT BO (one page at most) to GART. 0 / -errno */
    int   (*bo_gpu_addr)(void *ctx, rdna3_bo *bo, uint64_t *mc);
    /* adev->mqds[ip].init_mqd (gfx_v11_0_gfx_mqd_init,
     * gfx_v11_0_compute_mqd_init, sdma_v6_0_mqd_init) on a zeroed MQD */
    int   (*userq_mqd_init)(void *ctx, uint32_t ip, void *mqd, const amd_mqd_prop *prop);
    /* adev->mes.funcs->add_hw_queue / remove_hw_queue; the OS fills
     * process_id (the VM's PASID) and page_table_base_addr from 'vm' */
    int   (*userq_add_hw_queue)(void *ctx, void *vm, amd_mes_add_queue *in);
    int   (*userq_remove_hw_queue)(void *ctx, const amd_mes_remove_queue *in);
    /* a user queue fence wait: sleep until done(arg) - which the OS calls
     * with its lock held - or the absolute CLOCK_MONOTONIC deadline abs_ns
     * (UINT64_MAX: the OS's hang cap). 0 / -RD_ETIME. The OS may drop its
     * lock while it sleeps; the UAPI holds no pointers across the call. */
    int   (*userq_wait)(void *ctx, int (*done)(void *arg), void *arg, uint64_t abs_ns);
    /* CLOCK_MONOTONIC in ns, for the bounded waits of a queue teardown
     * (amdgpu_userq_wait_for_last_fence: 100 ms); optional */
    uint64_t (*now_ns)(void *ctx);
    /* RD_NR_XNU_SURFACE_BO, both optional (-ENODEV when missing).
     * surface_bo: place 'bo' (zeroed but for refcnt) over the VRAM of plane
     * 'plane' of IOSurface 'surface_id' as the client 'file_os' sees it:
     * set vram_offset, size (page aligned) and backing (the OS's hold, which
     * bo_free drops). surface_dirty: mark that VRAM newest. Both may drop
     * the OS's lock (the accelerator takes its own first); the UAPI holds
     * no pointers into the device across the call. 0 / -errno. */
    int   (*surface_bo)(void *ctx, void *file_os, rdna3_bo *bo, uint32_t surface_id, uint32_t plane);
    int   (*surface_dirty)(void *ctx, void *file_os, uint32_t surface_id, uint32_t plane);
    /* optional: a closing file's job still runs after the recovery, so its
     * VM stays (orphaned = 1) until rdna3_drm_device_reap_orphans finds the
     * job done and vm_destroy frees it; orphaned = 0: the VM never goes
     * (no orphan slot left) */
    void  (*vm_orphaned)(void *ctx, void *vm, int reclaimable);
} rdna3_drm_ops;

typedef struct rdna3_drm_device {
    rdna3_drm_ops ops;
    struct drm_amdgpu_info_device dev_info;
    uint64_t vram_size, vis_vram_size, gtt_size;
    uint64_t vram_usage, vis_vram_usage, gtt_usage;
    uint32_t accel_working;
    uint32_t available_rings[AMDGPU_HW_IP_NUM];   /* bitmask per HW IP */
    uint32_t ip_ver_major[AMDGPU_HW_IP_NUM], ip_ver_minor[AMDGPU_HW_IP_NUM];
    uint32_t fw_version[32], fw_feature[32];      /* by AMDGPU_INFO_FW_* */
    /* AMDGPU_INFO_FW_TA: version of each trusted application of the PSP TA
     * file by TA_FW_TYPE_* (the query's index; amd_fw_ta_versions) */
    uint32_t ta_version[RD_TA_TYPES];
    /* AMDGPU_INFO_VBIOS: the image (owned by the OS layer, NULL = none) and
     * amdgpu_atom_parse's strings */
    const uint8_t *vbios;
    uint32_t vbios_len;
    struct drm_amdgpu_info_vbios vbios_info;
    uint64_t next_mmap_offset;
    uint32_t gc_base0;            /* GC register segment 0 (discovery) */
    uint32_t gb_addr_config;      /* cached like adev->gfx.config.gb_addr_config */
    int      enumeration_only;    /* rings faked for libdrm (rdna3enum=1) */
    uint32_t ready_rings[AMDGPU_HW_IP_NUM];  /* rings that really execute */
    uint64_t last_seq[AMDGPU_HW_IP_NUM][4];  /* last submitted per ring */
    struct { rdna3_bo *bo; rdna3_fence fence; } uf_pending[RD_MAX_UF_PENDING];
    /* on-chip allocations, one bit per byte / entry / counter */
    uint64_t gds_map[RD_GDS_SIZE / 64], gws_map[RD_GWS_SIZE / 64], oa_map[1];
    /* exported BOs (RD_NR_XNU_BO_EXPORT): each entry holds a reference */
    struct { uint64_t token; rdna3_bo *bo; const struct rdna3_drm_file *owner; } exports[RD_MAX_EXPORTS];
    uint64_t export_ctr;
    /* open files, for DRM_AMDGPU_SCHED's target (by rdna3_drm_file.id) */
    struct rdna3_drm_file *files;
    uint32_t next_file_id;
    /* amdgpu_ip_version(adev, GC_HWIP, 0): AMD_IP_VERSION(major, minor, rev)
     * from discovery; with fw_version[] it decides cp_gfx_shadow
     * (amd_gfx11_check_fw_cp_gfx_shadow) */
    uint32_t gc_ip_version;
    /* amdgpu_sriov_vf(adev): 0, the card is a physical function (the
     * tests set it to see amdgpu's SR-IOV answers) */
    uint32_t sriov_vf;
    /* user queues: amdgpu_userq_get_supported_ip_mask (0: none), the
     * doorbell BAR's size and its pages handed out to doorbell BOs */
    uint32_t userq_ip_mask;
    uint64_t doorbell_size;
    uint8_t  doorbell_used[RD_MAX_DOORBELL_PAGES / 8];
    /* amdgpu_seq64 (created with the first fence driver) */
    rdna3_bo *seq64_bo;
    volatile uint64_t *seq64_cpu;
    uint64_t seq64_gpu_addr;
    uint8_t  seq64_used[RD_SEQ64_SLOTS / 8];
    /* fence drivers [RD_MAX_FENCE_DRV] and the references parked on user
     * queue fences until they signal [RD_USERQ_DEFERRED] (zalloc'ed with the
     * first one); the next dma_fence context */
    rdna3_userq_fence_drv *fence_drv;
    struct { uint32_t owner, ref; uint64_t seq; const struct rdna3_drm_file *file; } *uq_deferred;
    uint32_t next_fence_ctx;
    uint32_t visit_gen;       /* rdna3_drm_device_for_each_vram_bo */
    /* VCN 4.0 / JPEG 4.0 (rdna3_drm_device_set_vcn): vcn_num_inst 0 = the
     * video block is not up and the video IPs answer as without it */
    uint32_t vcn_ip_version;      /* UVD_HWIP: AMD_IP_VERSION(4, 0, 0) */
    uint32_t vcn_num_inst, vcn_harvest;
    uint32_t vcn_inst_ready;      /* instances whose unified ring runs (hw rings of VCN_ENC) */
    uint32_t jpeg_ready;
    /* adev->vram_lost_counter: AMDGPU_INFO_VRAM_LOST_COUNTER, and what each
     * context recorded at creation (rdna3_drm_device_vram_lost) */
    uint32_t vram_lost_counter;
    /* amdgpu_cs_pass1's -ECANCELED for a context older than the last VRAM
     * loss: 1 (rdna3_drm_device_init), 0 = such contexts keep submitting */
    uint32_t vram_lost_cs_refuse;
    /* closed files whose last jobs had not finished (rdna3_drm_file_close
     * after the recovery gave up): the VM, BOs and mappings stay until the
     * jobs' fences signal, as TTM's delayed delete keeps them in amdgpu */
    struct rdna3_orphan {
        void *vm;
        struct rdna3_bo **bos;        /* [RD_MAX_HANDLES] */
        struct rdna3_va *vas;         /* [RD_MAX_VA] */
        uint32_t nvas;
        uint64_t last_seq[AMDGPU_HW_IP_NUM][4];   /* the unfinished jobs */
    } orphans[RD_MAX_ORPHANS];
} rdna3_drm_device;

typedef struct rdna3_drm_file {
    rdna3_drm_device *dev;
    rdna3_bo **bos;           /* [RD_MAX_HANDLES], index = handle - 1 */
    uint8_t  ctx_used[RD_MAX_CTX];
    rdna3_syncobj *syncobjs;  /* [RD_MAX_SYNCOBJ], index = handle - 1 */
    rdna3_va *vas;            /* [RD_MAX_VA] */
    uint32_t nvas;
    void    *vm;              /* ops.vm_create() result, may be NULL */
    void    *os;              /* the OS's handle on the client (its task), for bo_userptr */
    uint32_t next_bo_list;
    struct { uint32_t handle; rdna3_bo *gds, *gws, *oa; } bo_lists[RD_MAX_BO_LISTS];
    uint64_t client_caps;
    /* ioctls of this file in progress: the OS layer may drop its lock while
     * a fence wait sleeps, and must not tear the file down under them */
    uint32_t inflight;
    /* amdgpu_ctx init_priority / override_priority (AMDGPU_CTX_PRIORITY_*,
     * override UNSET until DRM_AMDGPU_SCHED sets one) by context id */
    int16_t  ctx_init_prio[RD_MAX_CTX], ctx_override_prio[RD_MAX_CTX];
    /* the device's vram_lost_counter when each context was made (the
     * high half of amdgpu_ctx.generation): QUERY_STATE2 _VRAMLOST and CS
     * -ECANCELED once VRAM was lost after it */
    uint32_t ctx_vram_lost[RD_MAX_CTX];
    uint32_t id;                       /* RD_NR_XNU_FILE_ID */
    struct rdna3_drm_file *next_file;  /* rdna3_drm_device.files */
    /* amdgpu_userq_mgr: the file's user queues [RD_MAX_USERQ] (index = id - 1,
     * zalloc'ed with the first one) and whether seq64 is in its VM */
    rdna3_userq *userqs;
    uint32_t seq64_mapped;
    /* the VCN_ENC entity of each context (amdgpu_ctx_init_entity pins it
     * to one instance): instance + 1, 0 = none yet; and its job count
     * (entity->fence_seq, vcn_v4_0_limit_sched) */
    uint8_t  vcn_entity[RD_MAX_CTX];
    uint32_t vcn_entity_jobs[RD_MAX_CTX];
    /* the file's own last submission per ring (extended like
     * rdna3_drm_device.last_seq): its close waits for them */
    uint64_t last_seq[AMDGPU_HW_IP_NUM][4];
    /* rdna3_drm_file_close runs (its wait may drop the OS lock): no new
     * ioctls on the file */
    uint32_t closing;
} rdna3_drm_file;

/* Fill dev_info and sizes for a Navi 3x from discovery + PCI identity. */
void rdna3_drm_device_init(rdna3_drm_device *dev, const rdna3_drm_ops *ops,
                           const amd_discovery *disc, uint16_t pci_device,
                           uint8_t pci_revision, uint32_t rev_id,
                           uint64_t vram_size, uint64_t vis_vram_size,
                           uint64_t gtt_size);

/* drops what the device still holds (user fence BOs); call with the
 * rings stopped */
void rdna3_drm_device_fini(rdna3_drm_device *dev);

int  rdna3_drm_file_open(rdna3_drm_device *dev, rdna3_drm_file *f);
void rdna3_drm_file_close(rdna3_drm_file *f);

/* Returns 0 or a negative Linux errno. 'data' is a kernel copy of the
 * ioctl payload of 'size' bytes; results are written back into it. */
int  rdna3_drm_ioctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size);

/* Enumeration-only mode: report accel_working and one ring per GFX /
 * COMPUTE / DMA so libdrm_amdgpu and RADV can create a device (vulkaninfo,
 * memory and BO paths) before Stage 2 has rings. Every CS still fails
 * with -ENODEV. Kext: boot-arg rdna3enum=1. */
void rdna3_drm_device_set_enumeration_only(rdna3_drm_device *dev, int on);
/* the OS brought ring(s) 'mask' of HW IP 'ip' up: advertise them and let
 * CS reach ops.submit for them (also sets accel_working) */
void rdna3_drm_device_set_ring_ready(rdna3_drm_device *dev, uint32_t ip, uint32_t mask);
/* VRAM (and every page table) was lost and each live VM has a new empty
 * root: map every client's ranges again; returns the mappings that failed */
int  rdna3_drm_device_vm_replay(rdna3_drm_device *dev);
/* amdgpu_inc_vram_lost: the clients' VRAM contents are gone for good (a
 * reset or a resume found the reset magic lost and nothing put them back).
 * AMDGPU_INFO_VRAM_LOST_COUNTER counts up; every context made before it
 * reports AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST and its submissions fail with
 * -ECANCELED (amdgpu_cs_pass1); contexts made after it work. Once per
 * loss, device lock held. */
void rdna3_drm_device_vram_lost(rdna3_drm_device *dev);
/* refuse = 0: contexts made before a VRAM loss keep submitting (the
 * counter and QUERY_STATE2's VRAMLOST stay); 1 is amdgpu's and the default */
void rdna3_drm_device_set_vram_lost_cs(rdna3_drm_device *dev, int refuse);
/* the orphans (rdna3_drm_ops.vm_orphaned) whose jobs are done now (a later
 * GPU reset forces their fences): VM destroyed, mappings and BOs dropped.
 * Returns how many went; device lock held. rdna3_drm_file_open runs it
 * too. */
int  rdna3_drm_device_reap_orphans(rdna3_drm_device *dev);
/* every live BO placed in VRAM, once each (a BO several clients hold, an
 * export, the seq64 slots): amdgpu_device_evict_resources' list. The
 * callback must not free BOs. */
void rdna3_drm_device_for_each_vram_bo(rdna3_drm_device *dev, void (*fn)(void *ctx, rdna3_bo *bo), void *ctx);
/* amdgpu_userq_suspend / _resume around a system sleep, and
 * amdgpu_userq_pre_reset + _post_reset once a GPU reset restarted the MES
 * scheduler; each returns the queues that failed */
int  rdna3_drm_device_userq_suspend(rdna3_drm_device *dev);
/* amdgpu_userq_active: the user queues mapped on the scheduler, of one
 * process / of all (device lock held) */
int  rdna3_drm_file_userq_active(const rdna3_drm_file *f);
int  rdna3_drm_device_userq_active(const rdna3_drm_device *dev);
int  rdna3_drm_device_userq_resume(rdna3_drm_device *dev);
int  rdna3_drm_device_userq_reset(rdna3_drm_device *dev, int vram_lost);
/* Harvesting read from the GC (gfx11_get_cu_info): replaces the "all CUs
 * and RBs active" defaults in DEV_INFO. 'bitmap' uses cu_info->bitmap[0]'s
 * 4x4 layout. */
/* DEV_INFO engine/memory clock range in kHz (amdgpu_kms: from DPM when it
 * runs, else the VBIOS boot clocks). 0 leaves a value as it is. */
void rdna3_drm_device_set_clocks(rdna3_drm_device *dev, uint32_t min_sclk_khz, uint32_t max_sclk_khz,
                                 uint32_t min_mclk_khz, uint32_t max_mclk_khz);
void rdna3_drm_device_set_cu_info(rdna3_drm_device *dev, const uint32_t bitmap[4][4],
                                  uint32_t cu_active, uint32_t rb_mask);
/* gfx_v11_0_get_tcc_info's tcc_disabled_mask for DEV_INFO (rdna3amdgpu=1;
 * 0 otherwise, as before) */
void rdna3_drm_device_set_tcc_disabled(rdna3_drm_device *dev, uint64_t mask);
/* DEV_INFO values amdgpu takes from the VBIOS and the discovery binary
 * (rdna3amdgpu=1; the Navi 32 constants otherwise): gpu_counter_freq =
 * soc21_get_xclk (spll reference, kHz), vram_type / vram_bit_width =
 * amdgpu_atomfirmware_get_vram_info, mall_size = amdgpu_discovery_get_mall_info.
 * 0 leaves a value as it is. */
void rdna3_drm_device_set_asic_info(rdna3_drm_device *dev, uint32_t gpu_counter_freq_khz,
                                    uint32_t vram_type, uint32_t vram_bit_width, uint64_t mall_size);

/* user queues (amdgpu_userq_funcs set for the IPs in 'ip_mask', 1 <<
 * AMDGPU_HW_IP_*): the MES runs, the ops are in place. 'doorbell_size' is
 * the doorbell BAR's length; doorbell BOs come from it after the kernel's
 * RD_DOORBELL_KERNEL_SIZE. ip_mask 0 turns user queues off for new queues. */
void rdna3_drm_device_set_userq(rdna3_drm_device *dev, uint32_t ip_mask, uint64_t doorbell_size);

/* the VCN 4.0 block is up (kext: rdna3vcn=1): 'num_inst' instances
 * (harvest_config: AMDGPU_VCN_HARVEST_VCNn), the unified rings of the
 * instances in 'inst_ready' run, the JPEG ring if 'jpeg_ready'. HW_IP_INFO /
 * HW_IP_COUNT answer for VCN_DEC / VCN_ENC / VCN_JPEG as amdgpu_kms.c does,
 * VIDEO_CAPS from soc21_query_video_codecs, CS reaches ops.submit for
 * VCN_ENC (ring = the VCN instance the context's entity runs on) and
 * VCN_JPEG (rdna3_ib.kdata set). Not called: the answers stay as they were
 * without the video block. */
void rdna3_drm_device_set_vcn(rdna3_drm_device *dev, uint32_t ip_version, uint32_t num_inst,
                              uint32_t harvest_config, uint32_t inst_ready, int jpeg_ready);

/* the priority a context's jobs run at: its override when DRM_AMDGPU_SCHED
 * set one, else the one it was created with (amdgpu_ctx_priority_override's
 * ctx_prio); AMDGPU_CTX_PRIORITY_UNSET for no such context. The rings run
 * one queue each in submission order, so nothing acts on it yet. */
int32_t rdna3_drm_ctx_priority(const rdna3_drm_file *f, uint32_t ctx_id);

/* a reference the OS holds outside any file: a CPU mapping of the BO keeps
 * its memory until it is unmapped, after the client's handles are gone
 * (with the device's lock held, as for the ioctls) */
void rdna3_drm_bo_get(rdna3_bo *bo);
void rdna3_drm_bo_put(rdna3_drm_device *dev, rdna3_bo *bo);

/* Lookup for the OS mmap path: BO whose fake offset is 'off'. */
rdna3_bo *rdna3_drm_bo_by_mmap_offset(rdna3_drm_file *f, uint64_t off);
rdna3_bo *rdna3_drm_bo_lookup(rdna3_drm_file *f, uint32_t handle);

const char *rdna3_drm_ioctl_name(uint32_t nr);

#ifdef __cplusplus
}
#endif
#endif
