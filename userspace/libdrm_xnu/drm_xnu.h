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
 * system-memory copy (a CPU lock pages it off). 0, or -1 with errno set:
 * ENOENT no such surface here, ENODEV it has no accelerator VRAM, ENXIO
 * the accelerator (or the device) is not up yet, ENOSYS the kext has no
 * such ioctl (asked once). Surface 0 never exists: a probe of the kext. */
int   drm_xnu_surface_dirty(uint32_t surface_id, uint32_t plane);
/* DRIVER-PLAN N41. The same mark, answering EBUSY instead of waiting when
 * the accelerator's lock is taken (a page-off may hold it while it waits
 * for a GPU mark). */
int   drm_xnu_surface_dirty_try(uint32_t surface_id, uint32_t plane);
/* WindowServer's GPU write of the surface is pending (at commit) or done:
 * a CPU lock's page-off waits, bounded, while one is pending. 0, or -1:
 * ENXIO the accelerator's waits are off or it is not up (or not
 * WindowServer), ENOSYS a kext without the op (asked once), EINVAL
 * surface 0. */
int   drm_xnu_surface_mark(uint32_t surface_id, int pending);
/* the kernel's errno for this thread's last surface fd import (0: it
 * worked), which Vulkan reports only as a VkResult */
int   drm_xnu_surface_last_error(void);
/* DRIVER-PLAN N71 (c). The result of this thread's last surface fd import:
 * its errno (0: it worked), and when it worked the kernel's flags
 * (DRM_XNU_SURFACE_SHARED_RO: WindowServer's READ-ONLY import of an
 * application's shared allocation - no CPU mapping, never written) and the
 * allocation generation the BO is over (a new one after the producer
 * re-backed the surface; 0 from an older kext). Either pointer may be NULL. */
#define DRM_XNU_SURFACE_SHARED_RO 0x1u
int   drm_xnu_surface_last_import(uint32_t *flags, uint64_t *gen);
/* the same import's producer (SHARED_RO: the pid that shared it; 0 unknown
 * or not shared) */
int   drm_xnu_surface_last_import_pid(int32_t *pid);
/* the process understands a READ-ONLY import (never writes, maps or uploads
 * into one): only then does the kernel give WindowServer one; without the
 * call (an older plugin) its import is refused as before. */
void  drm_xnu_surface_accept_ro(int on);
/* DRIVER-PLAN N71 (c), the producer: after its GPU finished a frame in the
 * surface's VRAM, let WindowServer import that allocation read-only. Goes to
 * the connection this process imported the plane on (writable). 0, or -1:
 * ENOENT no writable import of it here (or the surface is gone), EPERM
 * refused (WindowServer, or the surface was re-backed: import again), ENXIO
 * sharing is off (debug.rdna3accel_a3share 0), ENODEV an accelerator
 * without it, ENOSYS a kext without the op, EINVAL surface 0 / plane > 15. */
int   drm_xnu_surface_share(uint32_t surface_id, uint32_t plane);
/* the share's ENOENT misses so far: no import known (never imported here, or
 * forgotten), the import's connection closed, and imports the 64-entry
 * table forgot (the oldest goes when it is full) */
void  drm_xnu_surface_share_stats(uint64_t *noimport, uint64_t *deadconn, uint64_t *evicted);
int   drm_xnu_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                       uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                       uint8_t *bus, uint8_t *dev, uint8_t *func);

#ifdef __cplusplus
}
#endif
#endif
