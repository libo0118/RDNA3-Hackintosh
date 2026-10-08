/*
 * drm_xnu.c - fd table and Linux DRM semantics on top of a backend
 * (drm_xnu_backend.h). See drm_xnu.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_xnu.h"
#include "drm_xnu_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MAPS 65536
#define MAX_CONN 256

/* Every connection is identified by a private pipe: the returned "DRM fd"
 * is its read end. dup()/fcntl(F_DUPFD) of that fd (libdrm_amdgpu does
 * this in amdgpu_device_initialize) refer to the same pipe inode, so any
 * duplicate resolves to the same connection through fstat(). */
static struct { uintptr_t conn; dev_t dev; ino_t ino; int fd; int used; } g_conns[MAX_CONN];
static struct { void *addr; size_t len; int ci; } g_maps[MAX_MAPS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_trace = -1;

static const drm_xnu_backend *be(void) { return &drm_xnu_backend_default; }

static int trace(void)
{
    if (g_trace < 0)
        g_trace = getenv("DRM_XNU_TRACE") ? 1 : 0;
    return g_trace;
}

static int conn_index(int fd)
{
    struct stat st;
    int ci, found = -1;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        return -1;
    pthread_mutex_lock(&g_lock);
    for (ci = 0; ci < MAX_CONN; ci++)
        if (g_conns[ci].used && g_conns[ci].dev == st.st_dev && g_conns[ci].ino == st.st_ino) {
            found = ci;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return found;
}

int drm_xnu_is_drm_fd(int fd)
{
    return conn_index(fd) >= 0;
}

int drm_xnu_open(const char *path, int flags)
{
    struct stat st;
    uintptr_t conn;
    int p[2], ci;
    (void)flags;

    if (path && strncmp(path, "/dev/dri/", 9) != 0) {
        errno = ENOENT;
        return -1;
    }
    conn = be()->open();
    if (!conn)
        return -1;
    if (pipe(p) != 0) {
        be()->close(conn);
        return -1;
    }
    close(p[1]);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    if (fstat(p[0], &st) != 0) {
        close(p[0]);
        be()->close(conn);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    for (ci = 0; ci < MAX_CONN && g_conns[ci].used; ci++)
        ;
    if (ci == MAX_CONN) {
        pthread_mutex_unlock(&g_lock);
        close(p[0]);
        be()->close(conn);
        errno = EMFILE;
        return -1;
    }
    g_conns[ci].conn = conn;
    g_conns[ci].dev = st.st_dev;
    g_conns[ci].ino = st.st_ino;
    g_conns[ci].fd = p[0];
    g_conns[ci].used = 1;
    pthread_mutex_unlock(&g_lock);
    return p[0];
}

int drm_xnu_ioctl_nr(int fd, uint32_t nr, void *arg, uint32_t size)
{
    int ci = conn_index(fd), err;
    if (ci < 0) {
        errno = EBADF;
        return -1;
    }
    err = be()->ioctl(g_conns[ci].conn, nr, arg, size);
    if (trace())
        fprintf(stderr, "drm_xnu: fd %d ioctl nr 0x%02x size %u -> %d\n", fd, nr, size, err);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

int drm_xnu_ioctl(int fd, unsigned long request, void *arg)
{
#if defined(__APPLE__)
    uint32_t size = (uint32_t)IOCPARM_LEN(request);
#else
    uint32_t size = (uint32_t)_IOC_SIZE(request);
#endif
    return drm_xnu_ioctl_nr(fd, (uint32_t)(request & 0xff), arg, size);
}

void *drm_xnu_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    int ci = conn_index(fd), i;
    void *p;
    if (ci < 0)
        return mmap(addr, len, prot, flags, fd, off);
    p = be()->map(g_conns[ci].conn, (uint64_t)off, len);
    if (!p)
        return MAP_FAILED;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_MAPS; i++)
        if (!g_maps[i].addr) {
            g_maps[i].addr = p;
            g_maps[i].len = len;
            g_maps[i].ci = ci;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return p;
}

int drm_xnu_munmap(void *addr, size_t len)
{
    int i;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].addr == addr) {
            int ci = g_maps[i].ci;
            g_maps[i].addr = NULL;
            pthread_mutex_unlock(&g_lock);
            return be()->unmap(g_conns[ci].conn, addr, len) ? (errno = EINVAL, -1) : 0;
        }
    pthread_mutex_unlock(&g_lock);
    return munmap(addr, len);
}

int drm_xnu_close(int fd)
{
    int ci = conn_index(fd);
    uintptr_t conn = 0;
    if (ci >= 0) {
        pthread_mutex_lock(&g_lock);
        if (g_conns[ci].fd == fd) {
            conn = g_conns[ci].conn;
            g_conns[ci].used = 0;
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (conn)
        be()->close(conn);
    return close(fd);
}

int drm_xnu_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                     uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                     uint8_t *bus, uint8_t *dev, uint8_t *func)
{
    return be()->pci_info(vendor, device, subvendor, subdevice, revision, domain, bus, dev, func);
}
