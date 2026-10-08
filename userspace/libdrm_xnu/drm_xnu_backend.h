/*
 * drm_xnu_backend.h - transport under drm_xnu: IOKit on macOS, or an
 * in-process loopback into uapi/rdna3_drm.c for host tests.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DRM_XNU_BACKEND_H
#define DRM_XNU_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct drm_xnu_backend {
    /* returns an opaque non-zero connection or 0 with errno set */
    uintptr_t (*open)(void);
    void      (*close)(uintptr_t conn);
    /* returns 0 or a positive errno */
    int       (*ioctl)(uintptr_t conn, uint32_t nr, void *arg, uint32_t size);
    void     *(*map)(uintptr_t conn, uint64_t offset, size_t len);   /* NULL + errno */
    int       (*unmap)(uintptr_t conn, void *addr, size_t len);
    /* device identity for drmGetDevices2 */
    int       (*pci_info)(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                          uint16_t *subdevice, uint8_t *revision,
                          uint16_t *domain, uint8_t *bus, uint8_t *dev, uint8_t *func);
} drm_xnu_backend;

extern const drm_xnu_backend drm_xnu_backend_default;

#endif
