/*
 * drm_xnu.h - Linux DRM device semantics on macOS, backed by RDNA3Accel.
 *
 * A "DRM fd" is a real file descriptor (dup of /dev/null) so that it can
 * be stored, polled for validity and closed like on Linux; the library
 * keeps the fd -> io_connect_t table. Same approach as NullMoth's
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

#ifdef __cplusplus
}
#endif
#endif
