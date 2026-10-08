/*
 * libdrm_macros.h override for building libdrm_amdgpu on top of drm_xnu:
 * identical to libdrm's except that drm_mmap/drm_munmap go through the
 * DRM fd's backend (IOConnectMapMemory64 on macOS).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LIBDRM_LIBDRM_H
#define LIBDRM_LIBDRM_H

#define drm_private __attribute__((visibility("hidden")))
#define drm_public  __attribute__((visibility("default")))

#define STATIC_ASSERT(COND) \
   do { \
      (void) sizeof(char [1 - 2*!(COND)]); \
   } while (0)

#include <sys/mman.h>
#include "drm_xnu.h"

#define drm_mmap(addr, length, prot, flags, fd, offset) \
    drm_xnu_mmap(addr, length, prot, flags, fd, offset)
#define drm_munmap(addr, length) drm_xnu_munmap(addr, length)

#endif
