/*
 * xf86drm_xnu.c - the subset of libdrm's core API (xf86drm.h) that
 * libdrm_amdgpu and Mesa RADV use, implemented over drm_xnu instead of
 * Linux device nodes. Semantics follow libdrm's xf86drm.c (MIT).
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xf86drm.h"
#include "util_math.h"   /* MAX3 for DRM_NODE_NAME_MAX */
#include "drm_xnu.h"

#define drm_public __attribute__((visibility("default")))

/* ---------------------------- ioctl ---------------------------- */

drm_public int drmIoctl(int fd, unsigned long request, void *arg)
{
    int ret;
    if (!drm_xnu_is_drm_fd(fd)) {
        errno = ENOTTY;
        return -1;
    }
    do {
        ret = drm_xnu_ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

drm_public int drmCommandNone(int fd, unsigned long drmCommandIndex)
{
    return drm_xnu_ioctl_nr(fd, (uint32_t)(DRM_COMMAND_BASE + drmCommandIndex), NULL, 0)
               ? -errno : 0;
}

static int command(int fd, unsigned long idx, void *data, unsigned long size)
{
    int r;
    do {
        r = drm_xnu_ioctl_nr(fd, (uint32_t)(DRM_COMMAND_BASE + idx), data, (uint32_t)size);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r ? -errno : 0;
}

drm_public int drmCommandRead(int fd, unsigned long i, void *d, unsigned long s) { return command(fd, i, d, s); }
drm_public int drmCommandWrite(int fd, unsigned long i, void *d, unsigned long s) { return command(fd, i, d, s); }
drm_public int drmCommandWriteRead(int fd, unsigned long i, void *d, unsigned long s) { return command(fd, i, d, s); }

/* ---------------------------- version / caps ---------------------------- */

drm_public drmVersionPtr drmGetVersion(int fd)
{
    struct drm_version v;
    drmVersionPtr r;
    memset(&v, 0, sizeof(v));
    if (drmIoctl(fd, DRM_IOCTL_VERSION, &v))
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->version_major = v.version_major;
    r->version_minor = v.version_minor;
    r->version_patchlevel = v.version_patchlevel;
    r->name_len = (int)v.name_len;
    r->date_len = (int)v.date_len;
    r->desc_len = (int)v.desc_len;
    r->name = calloc(1, v.name_len + 1);
    r->date = calloc(1, v.date_len + 1);
    r->desc = calloc(1, v.desc_len + 1);
    v.name = r->name; v.date = r->date; v.desc = r->desc;
    if (!r->name || !r->date || !r->desc || drmIoctl(fd, DRM_IOCTL_VERSION, &v)) {
        drmFreeVersion(r);
        return NULL;
    }
    return r;
}

drm_public void drmFreeVersion(drmVersionPtr v)
{
    if (!v)
        return;
    free(v->name);
    free(v->date);
    free(v->desc);
    free(v);
}

drm_public int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    struct drm_get_cap cap;
    int r;
    memset(&cap, 0, sizeof(cap));
    cap.capability = capability;
    r = drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap);
    if (r)
        return r;
    *value = cap.value;
    return 0;
}

drm_public int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
    struct drm_set_client_cap cap;
    memset(&cap, 0, sizeof(cap));
    cap.capability = capability;
    cap.value = value;
    return drmIoctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
}

drm_public void drmMsg(const char *format, ...)
{
    va_list ap;
    if (!getenv("LIBGL_DEBUG") && !getenv("DRM_XNU_TRACE"))
        return;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}

/* render nodes only: no master, no auth */
drm_public int drmIsMaster(int fd) { (void)fd; return 0; }
drm_public int drmSetMaster(int fd) { (void)fd; errno = EINVAL; return -1; }
drm_public int drmDropMaster(int fd) { (void)fd; errno = EINVAL; return -1; }
drm_public int drmGetMagic(int fd, drm_magic_t *m) { (void)fd; *m = 0; errno = EINVAL; return -EINVAL; }
drm_public int drmAuthMagic(int fd, drm_magic_t m) { (void)fd; (void)m; return -EINVAL; }

/* ---------------------------- GEM / PRIME ---------------------------- */

drm_public int drmCloseBufferHandle(int fd, uint32_t handle)
{
    struct drm_gem_close c;
    memset(&c, 0, sizeof(c));
    c.handle = handle;
    return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

drm_public int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    struct drm_prime_handle a;
    int r;
    memset(&a, 0, sizeof(a));
    a.handle = handle;
    a.flags = flags;
    r = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &a);
    if (r)
        return r;
    *prime_fd = a.fd;
    return 0;
}

drm_public int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    struct drm_prime_handle a;
    int r;
    memset(&a, 0, sizeof(a));
    a.fd = prime_fd;
    r = drmIoctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &a);
    if (r)
        return r;
    *handle = a.handle;
    return 0;
}

/* ---------------------------- syncobj ---------------------------- */

drm_public int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    struct drm_syncobj_create a;
    int r;
    memset(&a, 0, sizeof(a));
    a.flags = flags;
    r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &a);
    if (r)
        return r;
    *handle = a.handle;
    return 0;
}

drm_public int drmSyncobjDestroy(int fd, uint32_t handle)
{
    struct drm_syncobj_destroy a;
    memset(&a, 0, sizeof(a));
    a.handle = handle;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &a);
}

drm_public int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
    struct drm_syncobj_handle a;
    int r;
    memset(&a, 0, sizeof(a));
    a.handle = handle;
    r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &a);
    if (r)
        return r;
    *obj_fd = a.fd;
    return 0;
}

drm_public int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
    struct drm_syncobj_handle a;
    int r;
    memset(&a, 0, sizeof(a));
    a.fd = obj_fd;
    r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &a);
    if (r)
        return r;
    *handle = a.handle;
    return 0;
}

drm_public int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    struct drm_syncobj_handle a;
    memset(&a, 0, sizeof(a));
    a.fd = sync_file_fd;
    a.handle = handle;
    a.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &a);
}

drm_public int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    struct drm_syncobj_handle a;
    int r;
    memset(&a, 0, sizeof(a));
    a.fd = -1;
    a.handle = handle;
    a.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &a);
    if (r)
        return r;
    *sync_file_fd = a.fd;
    return 0;
}

drm_public int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                              int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
    struct drm_syncobj_wait a;
    int r;
    memset(&a, 0, sizeof(a));
    a.handles = (uintptr_t)handles;
    a.timeout_nsec = timeout_nsec;
    a.count_handles = num_handles;
    a.flags = flags;
    r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &a);
    if (r < 0)
        return -errno;
    if (first_signaled)
        *first_signaled = a.first_signaled;
    return r;
}

drm_public int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
    struct drm_syncobj_array a;
    memset(&a, 0, sizeof(a));
    a.handles = (uintptr_t)handles;
    a.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &a);
}

drm_public int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
    struct drm_syncobj_array a;
    memset(&a, 0, sizeof(a));
    a.handles = (uintptr_t)handles;
    a.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &a);
}

drm_public int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                                        uint32_t handle_count)
{
    struct drm_syncobj_timeline_array a;
    memset(&a, 0, sizeof(a));
    a.handles = (uintptr_t)handles;
    a.points = (uintptr_t)points;
    a.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a);
}

drm_public int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                                      unsigned num_handles, int64_t timeout_nsec,
                                      unsigned flags, uint32_t *first_signaled)
{
    struct drm_syncobj_timeline_wait a;
    int r;
    memset(&a, 0, sizeof(a));
    a.handles = (uintptr_t)handles;
    a.points = (uintptr_t)points;
    a.timeout_nsec = timeout_nsec;
    a.count_handles = num_handles;
    a.flags = flags;
    r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &a);
    if (r < 0)
        return -errno;
    if (first_signaled)
        *first_signaled = a.first_signaled;
    return r;
}

drm_public int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                                uint32_t handle_count, uint32_t flags)
{
    struct drm_syncobj_timeline_array a;
    memset(&a, 0, sizeof(a));
    a.handles = (uintptr_t)handles;
    a.points = (uintptr_t)points;
    a.count_handles = handle_count;
    a.flags = flags;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &a);
}

drm_public int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
    return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

drm_public int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                                  uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
    struct drm_syncobj_transfer a;
    memset(&a, 0, sizeof(a));
    a.src_handle = src_handle;
    a.dst_handle = dst_handle;
    a.src_point = src_point;
    a.dst_point = dst_point;
    a.flags = flags;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &a);
}

/* ---------------------------- devices ---------------------------- */

drm_public int drmGetNodeTypeFromFd(int fd)
{
    if (!drm_xnu_is_drm_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return DRM_NODE_RENDER;
}

static char *node_name(void) { return strdup(DRM_XNU_RENDER_NODE); }

drm_public char *drmGetRenderDeviceNameFromFd(int fd)
{
    return drm_xnu_is_drm_fd(fd) ? node_name() : NULL;
}
drm_public char *drmGetPrimaryDeviceNameFromFd(int fd)
{
    /* libdrm_amdgpu uses this only to compare two fds for the same GPU */
    return drm_xnu_is_drm_fd(fd) ? node_name() : NULL;
}
drm_public char *drmGetDeviceNameFromFd2(int fd)
{
    return drmGetRenderDeviceNameFromFd(fd);
}

static drmDevicePtr make_device(void)
{
    /* one allocation, like libdrm's drmDeviceAlloc() */
    size_t sz = sizeof(drmDevice) + DRM_NODE_MAX * (sizeof(char *) + DRM_NODE_NAME_MAX) +
                sizeof(drmPciBusInfo) + sizeof(drmPciDeviceInfo);
    drmDevicePtr d;
    char *p;
    uint16_t vendor, device, subvendor, subdevice, domain;
    uint8_t rev, bus, dev, func;
    int i;

    if (drm_xnu_pci_info(&vendor, &device, &subvendor, &subdevice, &rev, &domain, &bus, &dev,
                         &func))
        return NULL;
    d = calloc(1, sz);
    if (!d)
        return NULL;
    p = (char *)(d + 1);
    d->nodes = (char **)p;
    p += DRM_NODE_MAX * sizeof(char *);
    for (i = 0; i < DRM_NODE_MAX; i++) {
        d->nodes[i] = p;
        p += DRM_NODE_NAME_MAX;
    }
    snprintf(d->nodes[DRM_NODE_RENDER], DRM_NODE_NAME_MAX, "%s", DRM_XNU_RENDER_NODE);
    d->available_nodes = 1 << DRM_NODE_RENDER;
    d->bustype = DRM_BUS_PCI;
    d->businfo.pci = (drmPciBusInfoPtr)p;
    p += sizeof(drmPciBusInfo);
    d->deviceinfo.pci = (drmPciDeviceInfoPtr)p;
    d->businfo.pci->domain = domain;
    d->businfo.pci->bus = bus;
    d->businfo.pci->dev = dev;
    d->businfo.pci->func = func;
    d->deviceinfo.pci->vendor_id = vendor;
    d->deviceinfo.pci->device_id = device;
    d->deviceinfo.pci->subvendor_id = subvendor;
    d->deviceinfo.pci->subdevice_id = subdevice;
    d->deviceinfo.pci->revision_id = rev;
    return d;
}

drm_public int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
    drmDevicePtr d;
    (void)flags;
    d = make_device();
    if (!d)
        return 0;
    if (devices && max_devices > 0)
        devices[0] = d;
    else
        free(d);
    return 1;
}

drm_public int drmGetDevices(drmDevicePtr devices[], int max_devices)
{
    return drmGetDevices2(DRM_DEVICE_GET_PCI_REVISION, devices, max_devices);
}

drm_public int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
    (void)flags;
    if (!drm_xnu_is_drm_fd(fd))
        return -EINVAL;
    *device = make_device();
    return *device ? 0 : -ENODEV;
}

drm_public int drmGetDevice(int fd, drmDevicePtr *device)
{
    return drmGetDevice2(fd, DRM_DEVICE_GET_PCI_REVISION, device);
}

drm_public int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
    (void)dev_id;
    (void)flags;
    *device = make_device();
    return *device ? 0 : -ENODEV;
}

drm_public void drmFreeDevice(drmDevicePtr *device)
{
    if (device && *device) {
        free(*device);
        *device = NULL;
    }
}

drm_public void drmFreeDevices(drmDevicePtr devices[], int count)
{
    int i;
    for (i = 0; devices && i < count; i++)
        drmFreeDevice(&devices[i]);
}

drm_public int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
    if (!a || !b || a->bustype != b->bustype)
        return 0;
    return !memcmp(a->businfo.pci, b->businfo.pci, sizeof(drmPciBusInfo));
}

/* Format modifier names (libdrm xf86drm.c). Mesa only prints them in debug
 * dumps and free()s the result; we name the cases RADV produces and
 * describe AMD modifiers by their fields (drm_fourcc.h AMD_FMT_MOD_*). */
drm_public char *drmGetFormatModifierVendor(uint64_t modifier)
{
    switch (modifier >> 56) {
    case 0x00: return strdup("NONE");
    case 0x02: return strdup("AMD");
    default:   return NULL;
    }
}

drm_public char *drmGetFormatModifierName(uint64_t modifier)
{
    char *s = NULL;

    if (modifier == 0)
        return strdup("LINEAR");
    if (modifier == 0x00ffffffffffffffull)
        return strdup("INVALID");
    if ((modifier >> 56) != 0x02)
        return NULL;
    if (asprintf(&s, "AMD(TILE_VERSION=%u,TILE=%u,DCC=%u,DCC_RETILE=%u,DCC_PIPE_ALIGN=%u,"
                 "DCC_INDEPENDENT_64B=%u,DCC_INDEPENDENT_128B=%u,DCC_MAX_COMPRESSED_BLOCK=%u,"
                 "PIPE_XOR_BITS=%u,BANK_XOR_BITS=%u,PACKERS=%u,RB=%u,PIPE=%u)",
                 (unsigned)(modifier & 0xff), (unsigned)((modifier >> 8) & 0x1f),
                 (unsigned)((modifier >> 13) & 1), (unsigned)((modifier >> 14) & 1),
                 (unsigned)((modifier >> 15) & 1), (unsigned)((modifier >> 16) & 1),
                 (unsigned)((modifier >> 17) & 1), (unsigned)((modifier >> 18) & 3),
                 (unsigned)((modifier >> 20) & 7), (unsigned)((modifier >> 23) & 7),
                 (unsigned)((modifier >> 26) & 7), (unsigned)((modifier >> 29) & 7),
                 (unsigned)((modifier >> 32) & 7)) < 0)
        return NULL;
    return s;
}
