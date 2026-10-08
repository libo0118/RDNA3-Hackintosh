/*
 * backend_iokit.c - drm_xnu transport to RDNA3Accel's RDNA3UserClient.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_xnu_backend.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <mach/mach.h>

enum { SEL_IOCTL = 0 };

static io_service_t find_service(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA3Accel"));
}

static uintptr_t iokit_open(void)
{
    io_connect_t conn = 0;
    io_service_t svc = find_service();
    kern_return_t kr;
    if (!svc) {
        errno = ENODEV;
        return 0;
    }
    kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        errno = kr == kIOReturnNotPrivileged ? EACCES : ENODEV;
        return 0;
    }
    return (uintptr_t)conn;
}

static void iokit_close(uintptr_t c) { IOServiceClose((io_connect_t)c); }

static int iokit_ioctl(uintptr_t c, uint32_t nr, void *arg, uint32_t size)
{
    uint64_t in = nr, out = 0;
    uint32_t nout = 1;
    size_t osz = size;
    kern_return_t kr = IOConnectCallMethod((io_connect_t)c, SEL_IOCTL, &in, 1, arg, size,
                                           &out, &nout, arg, &osz);
    if (kr != KERN_SUCCESS)
        return EIO;
    return (int)out;
}

static void *iokit_map(uintptr_t c, uint64_t off, size_t len)
{
    mach_vm_address_t a = 0;
    mach_vm_size_t l = 0;
    kern_return_t kr = IOConnectMapMemory64((io_connect_t)c, (uint32_t)(off >> 12),
                                            mach_task_self(), &a, &l, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || l < len) {
        errno = kr == kIOReturnNotFound ? EINVAL : ENOMEM;
        return NULL;
    }
    return (void *)(uintptr_t)a;
}

static int iokit_unmap(uintptr_t c, void *addr, size_t len)
{
    (void)len;
    return IOConnectUnmapMemory64((io_connect_t)c, 0, mach_task_self(),
                                  (mach_vm_address_t)(uintptr_t)addr) == KERN_SUCCESS ? 0 : -1;
}

static int num_prop(io_service_t s, const char *key, uint32_t *v)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    int ok = 0;
    CFRelease(k);
    if (p && CFGetTypeID(p) == CFNumberGetTypeID())
        ok = CFNumberGetValue((CFNumberRef)p, kCFNumberSInt32Type, v);
    if (p)
        CFRelease(p);
    return ok;
}

static int iokit_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                          uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                          uint8_t *bus, uint8_t *dev, uint8_t *func)
{
    io_service_t s = find_service();
    uint32_t v = 0;
    if (!s)
        return -1;
    *vendor = num_prop(s, "RDNA3,VendorID", &v) ? (uint16_t)v : 0x1002;
    *device = num_prop(s, "RDNA3,DeviceID", &v) ? (uint16_t)v : 0;
    *subvendor = num_prop(s, "RDNA3,SubsystemVendorID", &v) ? (uint16_t)v : 0;
    *subdevice = num_prop(s, "RDNA3,SubsystemID", &v) ? (uint16_t)v : 0;
    *revision = num_prop(s, "RDNA3,Revision", &v) ? (uint8_t)v : 0;
    /* TODO: real PCI location from the IOPCIDevice "reg" property */
    *domain = 0; *bus = num_prop(s, "RDNA3,PCIBus", &v) ? (uint8_t)v : 0;
    *dev = 0; *func = 0;
    IOObjectRelease(s);
    return 0;
}

const drm_xnu_backend drm_xnu_backend_default = {
    iokit_open, iokit_close, iokit_ioctl, iokit_map, iokit_unmap, iokit_pci_info,
};
