/*
 * rdna3_drm.h - the Linux DRM + amdgpu UAPI, implemented OS-independently.
 *
 * Userspace (libdrm / libdrm_amdgpu / RADV) issues exactly the ioctls it
 * issues on Linux; the userspace shim (userspace/libdrm_xnu) forwards
 * (nr, payload) to the kext's IOUserClient, which calls
 * rdna3_drm_ioctl(). Struct layouts are the real ABI from
 * include/uapi/drm/amdgpu_drm.h and drm.h (third_party/amd).
 *
 * Status of each ioctl is tracked in docs/STAGE4.md. Anything that needs
 * the GPU to execute work (CS) returns -ENODEV until the rings exist.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DRM_H
#define RDNA3_DRM_H

#include <stdint.h>
#include <stddef.h>
#include "amdgpu_drm.h"
#include "amd_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux errno values: the shim returns them to libdrm as-is */
enum {
    RD_EPERM = 1, RD_ENOENT = 2, RD_EINTR = 4, RD_EIO = 5, RD_EBADF = 9,
    RD_ENOMEM = 12, RD_EFAULT = 14, RD_EBUSY = 16, RD_EINVAL = 22, RD_ENOSPC = 28,
    RD_ENOSYS = 38, RD_ENODEV = 19, RD_ETIME = 62, RD_EOPNOTSUPP = 95,
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

/* We advertise the amdgpu DRM version RADV/libdrm expect from Linux 6.x */
#define RD_DRM_MAJOR 3
#define RD_DRM_MINOR 61
#define RD_DRM_PATCH 0

#define RD_MAX_HANDLES  16384
#define RD_MAX_CTX      256
#define RD_MAX_SYNCOBJ  4096
#define RD_MAX_VA       65536

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
} rdna3_bo;

typedef struct rdna3_va {
    uint32_t handle;          /* 0 = free slot */
    uint64_t va, offset, size, flags;
} rdna3_va;

typedef struct rdna3_syncobj {
    uint8_t  used, signaled;
    uint64_t point;
} rdna3_syncobj;

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
    uint64_t next_mmap_offset;
} rdna3_drm_device;

typedef struct rdna3_drm_file {
    rdna3_drm_device *dev;
    rdna3_bo **bos;           /* [RD_MAX_HANDLES], index = handle - 1 */
    uint8_t  ctx_used[RD_MAX_CTX];
    rdna3_syncobj *syncobjs;  /* [RD_MAX_SYNCOBJ], index = handle - 1 */
    rdna3_va *vas;            /* [RD_MAX_VA] */
    uint32_t nvas;
    uint64_t client_caps;
} rdna3_drm_file;

/* Fill dev_info and sizes for a Navi 3x from discovery + PCI identity. */
void rdna3_drm_device_init(rdna3_drm_device *dev, const rdna3_drm_ops *ops,
                           const amd_discovery *disc, uint16_t pci_device,
                           uint8_t pci_revision, uint32_t rev_id,
                           uint64_t vram_size, uint64_t vis_vram_size,
                           uint64_t gtt_size);

int  rdna3_drm_file_open(rdna3_drm_device *dev, rdna3_drm_file *f);
void rdna3_drm_file_close(rdna3_drm_file *f);

/* Returns 0 or a negative Linux errno. 'data' is a kernel copy of the
 * ioctl payload of 'size' bytes; results are written back into it. */
int  rdna3_drm_ioctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size);

/* Lookup for the OS mmap path: BO whose fake offset is 'off'. */
rdna3_bo *rdna3_drm_bo_by_mmap_offset(rdna3_drm_file *f, uint64_t off);
rdna3_bo *rdna3_drm_bo_lookup(rdna3_drm_file *f, uint32_t handle);

const char *rdna3_drm_ioctl_name(uint32_t nr);

#ifdef __cplusplus
}
#endif
#endif
