/*
 * drm_xnu.c - see drm_xnu.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_xnu.h"

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FD   4096
#define MAX_MAPS 65536
enum { SEL_IOCTL = 0 };

static io_connect_t g_conn[MAX_FD];
static struct { void *addr; size_t len; io_connect_t conn; } g_maps[MAX_MAPS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_trace = -1;

static int trace(void)
{
    if (g_trace < 0)
        g_trace = getenv("DRM_XNU_TRACE") ? 1 : 0;
    return g_trace;
}

static io_connect_t conn_of(int fd)
{
    io_connect_t c = 0;
    if (fd >= 0 && fd < MAX_FD) {
        pthread_mutex_lock(&g_lock);
        c = g_conn[fd];
        pthread_mutex_unlock(&g_lock);
    }
    return c;
}

int drm_xnu_is_drm_fd(int fd)
{
    return conn_of(fd) != 0;
}

int drm_xnu_open(const char *path, int flags)
{
    io_service_t svc;
    io_connect_t conn = 0;
    kern_return_t kr;
    int fd;
    (void)flags;

    if (path && strncmp(path, "/dev/dri/", 9) != 0) {
        errno = ENOENT;
        return -1;
    }
    svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA3Accel"));
    if (!svc) {
        errno = ENODEV;
        return -1;
    }
    kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        if (trace())
            fprintf(stderr, "drm_xnu: IOServiceOpen -> 0x%08x\n", kr);
        errno = kr == kIOReturnNotPrivileged ? EACCES : ENODEV;
        return -1;
    }
    fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0 || fd >= MAX_FD) {
        if (fd >= 0)
            close(fd);
        IOServiceClose(conn);
        errno = EMFILE;
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_conn[fd] = conn;
    pthread_mutex_unlock(&g_lock);
    return fd;
}

int drm_xnu_ioctl_nr(int fd, uint32_t nr, void *arg, uint32_t size)
{
    io_connect_t conn = conn_of(fd);
    uint64_t in = nr, out = 0;
    uint32_t nout = 1;
    size_t osz = size;
    kern_return_t kr;

    if (!conn) {
        errno = EBADF;
        return -1;
    }
    kr = IOConnectCallMethod(conn, SEL_IOCTL, &in, 1, arg, size, &out, &nout, arg, &osz);
    if (trace())
        fprintf(stderr, "drm_xnu: ioctl nr 0x%02x size %u -> kr 0x%x errno %llu\n", nr, size,
                kr, (unsigned long long)out);
    if (kr != KERN_SUCCESS) {
        errno = EIO;
        return -1;
    }
    if (out) {
        errno = (int)out;
        return -1;
    }
    return 0;
}

int drm_xnu_ioctl(int fd, unsigned long request, void *arg)
{
    return drm_xnu_ioctl_nr(fd, (uint32_t)(request & 0xff), arg,
                            (uint32_t)IOCPARM_LEN(request));
}

void *drm_xnu_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    io_connect_t conn = conn_of(fd);
    mach_vm_address_t a = 0;
    mach_vm_size_t l = 0;
    kern_return_t kr;
    int i;
    (void)addr; (void)prot; (void)flags;

    if (!conn) {
        errno = EBADF;
        return MAP_FAILED;
    }
    kr = IOConnectMapMemory64(conn, (uint32_t)((uint64_t)off >> 12), mach_task_self(), &a, &l,
                              kIOMapAnywhere);
    if (kr != KERN_SUCCESS || l < len) {
        if (trace())
            fprintf(stderr, "drm_xnu: map offset 0x%llx -> 0x%x\n", (unsigned long long)off, kr);
        errno = kr == kIOReturnNotFound ? EINVAL : ENOMEM;
        return MAP_FAILED;
    }
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_MAPS; i++)
        if (!g_maps[i].addr) {
            g_maps[i].addr = (void *)(uintptr_t)a;
            g_maps[i].len = (size_t)l;
            g_maps[i].conn = conn;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return (void *)(uintptr_t)a;
}

int drm_xnu_munmap(void *addr, size_t len)
{
    int i;
    (void)len;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].addr == addr) {
            io_connect_t c = g_maps[i].conn;
            g_maps[i].addr = NULL;
            pthread_mutex_unlock(&g_lock);
            /* memory type is not needed by IOConnectUnmapMemory64 lookups by address */
            return IOConnectUnmapMemory64(c, 0, mach_task_self(), (mach_vm_address_t)(uintptr_t)addr)
                       == KERN_SUCCESS ? 0 : (errno = EINVAL, -1);
        }
    pthread_mutex_unlock(&g_lock);
    return munmap(addr, len);
}

int drm_xnu_close(int fd)
{
    io_connect_t conn = 0;
    if (fd >= 0 && fd < MAX_FD) {
        pthread_mutex_lock(&g_lock);
        conn = g_conn[fd];
        g_conn[fd] = 0;
        pthread_mutex_unlock(&g_lock);
    }
    if (conn)
        IOServiceClose(conn);
    return close(fd);
}
