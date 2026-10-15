/*
 * drm_xnu.h - Linux DRM device semantics on macOS, backed by RDNA3Accel.
 *
 * A "DRM fd" is the read end of a private pipe, so it can be stored,
 * dup()ed (libdrm_amdgpu does) and closed like on Linux; the connection
 * is found again from any duplicate through the pipe's inode. Same approach as NullMoth's
 * nvrm_xnu.c, implemented independently for the amdgpu UAPI.
 *
 * Patch points in libdrm (userspace/patches/libdrm-xnu.patch):
 *   open("/dev/dri/renderD128")  -> drm_xnu_open()
 *   ioctl(fd, req, arg)          -> drm_xnu_ioctl()   (drmIoctl)
 *   mmap(.., fd, off)            -> drm_xnu_mmap()    (drm_mmap / amdgpu_bo_cpu_map)
 *   munmap                       -> drm_xnu_munmap()
 *   close(fd)                    -> drm_xnu_close()
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DRM_XNU_H
#define DRM_XNU_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_XNU_RENDER_NODE "/dev/dri/renderD128"

int   drm_xnu_open(const char *path, int flags);
int   drm_xnu_is_drm_fd(int fd);
/* request: an ioctl number built with the platform's _IOWR(); the DRM nr
 * is its low byte and the payload size comes from IOCPARM_LEN(). Returns
 * 0 or -1 with errno set, like ioctl(2). */
int   drm_xnu_ioctl(int fd, unsigned long request, void *arg);
/* Same, with explicit nr/size (for callers that do not build _IOWR). */
int   drm_xnu_ioctl_nr(int fd, uint32_t nr, void *arg, uint32_t size);
void *drm_xnu_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   drm_xnu_munmap(void *addr, size_t len);
int   drm_xnu_close(int fd);
/* An IOSurface plane as a "dma-buf fd" for Vulkan's OPAQUE_FD / DMA_BUF
 * import (VkImportMemoryFdInfoKHR, amdgpu_bo_import): the importer gets a
 * BO over the VRAM the accelerator keeps for the surface, if its process
 * may use the surface; 'size' bytes must fit in it. Returns an fd for
 * close(), or -1 with errno set. */
int   drm_xnu_surface_fd(uint32_t surface_id, uint32_t plane, uint64_t size);
/* after a GPU write into such memory: the surface's VRAM is newer than its
 * system-memory copy (a CPU lock pages it off). 0, or -1 with errno set
 * (ENOENT: no such surface here, ENODEV: it has no accelerator VRAM). */
int   drm_xnu_surface_dirty(uint32_t surface_id, uint32_t plane);
int   drm_xnu_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                       uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                       uint8_t *bus, uint8_t *dev, uint8_t *func);

#ifdef __cplusplus
}
#endif
#endif
