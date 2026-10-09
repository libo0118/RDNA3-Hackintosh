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
static struct { uintptr_t conn; dev_t dev; ino_t ino; int fd; int used; int orphan; } g_conns[MAX_CONN];
static struct { void *addr; size_t len; int ci; } g_maps[MAX_MAPS];
/* Syncobj "fds" (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD / FD_TO_HANDLE): the kext has
 * no file descriptors, so an export is a /dev/null fd remembered here with
 * the syncobj it stands for. A sync_file export snapshots the fence into a
 * hidden syncobj (SYNCOBJ_TRANSFER); an opaque export shares the handle.
 * Process-local: it covers RADV's own exports/imports and VK_KHR_external_
 * semaphore_fd inside one process, not sharing between processes. */
#define MAX_SYNCFD 256
static struct { int fd; int ci; uint32_t handle; int snapshot; int used; } g_syncfd[MAX_SYNCFD];
struct xnu_syncobj_handle { uint32_t handle, flags; int32_t fd; uint32_t pad; };
struct xnu_syncobj_transfer { uint32_t src_handle, dst_handle; uint64_t src_point, dst_point; uint32_t flags, pad; };
struct xnu_syncobj_create { uint32_t handle, flags; };
struct xnu_syncobj_destroy { uint32_t handle, pad; };
#define XNU_NR_SYNCOBJ_CREATE        0xBF
#define XNU_NR_SYNCOBJ_DESTROY       0xC0
#define XNU_NR_SYNCOBJ_HANDLE_TO_FD  0xC1
#define XNU_NR_SYNCOBJ_FD_TO_HANDLE  0xC2
#define XNU_NR_SYNCOBJ_TRANSFER      0xCC
#define XNU_SYNCOBJ_EXPORT_SYNC_FILE 1u
#define XNU_SYNCOBJ_IMPORT_SYNC_FILE 1u
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

/* A connection outlives its original fd while dup()s of it exist
 * (RADV closes its fd after libdrm_amdgpu dup()ed it). Once the original
 * is closed the entry is an orphan; it is released as soon as no fd in
 * the process refers to the pipe any more. Called with g_lock held. */
static int inode_in_use(dev_t dev, ino_t ino)
{
    long maxfd = sysconf(_SC_OPEN_MAX);
    struct stat st;
    int fd;
    if (maxfd < 0 || maxfd > 65536)
        maxfd = 65536;
    for (fd = 0; fd < maxfd; fd++)
        if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_dev == dev && st.st_ino == ino)
            return 1;
    return 0;
}

static void gc_orphans_locked(void)
{
    int ci;
    for (ci = 0; ci < MAX_CONN; ci++)
        if (g_conns[ci].used && g_conns[ci].orphan &&
            !inode_in_use(g_conns[ci].dev, g_conns[ci].ino)) {
            be()->close(g_conns[ci].conn);
            g_conns[ci].used = 0;
            g_conns[ci].orphan = 0;
        }
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
    gc_orphans_locked();
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
    g_conns[ci].orphan = 0;
    pthread_mutex_unlock(&g_lock);
    return p[0];
}

static int syncobj_transfer(int ci, uint32_t src, uint32_t dst)
{
    struct xnu_syncobj_transfer t;
    memset(&t, 0, sizeof(t));
    t.src_handle = src;
    t.dst_handle = dst;
    return be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_TRANSFER, &t, sizeof(t));
}

static int syncobj_snapshot(int ci, uint32_t src, uint32_t *out)
{
    struct xnu_syncobj_create c = { 0, 0 };
    int err = be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_CREATE, &c, sizeof(c));
    if (err)
        return err;
    err = syncobj_transfer(ci, src, c.handle);
    if (err) {
        struct xnu_syncobj_destroy d = { c.handle, 0 };
        be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_DESTROY, &d, sizeof(d));
        return err;
    }
    *out = c.handle;
    return 0;
}

static int syncobj_handle_to_fd(int ci, struct xnu_syncobj_handle *a)
{
    int fd, i, err;
    uint32_t handle = a->handle;
    int snapshot = (a->flags & XNU_SYNCOBJ_EXPORT_SYNC_FILE) != 0;
    if (snapshot) {
        err = syncobj_snapshot(ci, a->handle, &handle);
        if (err)
            return err;
    }
    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno ? errno : EMFILE;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_SYNCFD && g_syncfd[i].used && g_syncfd[i].fd != fd; i++)
        ;
    if (i == MAX_SYNCFD) {
        pthread_mutex_unlock(&g_lock);
        close(fd);
        return EMFILE;
    }
    g_syncfd[i].fd = fd;
    g_syncfd[i].ci = ci;
    g_syncfd[i].handle = handle;
    g_syncfd[i].snapshot = snapshot;
    g_syncfd[i].used = 1;
    pthread_mutex_unlock(&g_lock);
    a->fd = fd;
    return 0;
}

static int syncobj_fd_to_handle(int ci, struct xnu_syncobj_handle *a)
{
    int i, found = -1;
    uint32_t src;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_SYNCFD; i++)
        if (g_syncfd[i].used && g_syncfd[i].fd == a->fd && g_syncfd[i].ci == ci)
            found = i;
    if (found < 0) {
        pthread_mutex_unlock(&g_lock);
        return EINVAL;
    }
    src = g_syncfd[found].handle;
    if (a->flags & XNU_SYNCOBJ_IMPORT_SYNC_FILE) {
        /* import into the caller's syncobj: copy the fence */
        pthread_mutex_unlock(&g_lock);
        return syncobj_transfer(ci, src, a->handle);
    }
    if (g_syncfd[found].snapshot) {
        pthread_mutex_unlock(&g_lock);
        return syncobj_snapshot(ci, src, &a->handle);
    }
    a->handle = src;   /* opaque fd: the same object */
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int drm_xnu_ioctl_nr(int fd, uint32_t nr, void *arg, uint32_t size)
{
    int ci = conn_index(fd), err;
    if (ci < 0) {
        errno = EBADF;
        return -1;
    }
    /* newer drm.h adds a timeline point + pad after the four words (size 24) */
    if (nr == XNU_NR_SYNCOBJ_HANDLE_TO_FD && size >= sizeof(struct xnu_syncobj_handle))
        err = syncobj_handle_to_fd(ci, (struct xnu_syncobj_handle *)arg);
    else if (nr == XNU_NR_SYNCOBJ_FD_TO_HANDLE && size >= sizeof(struct xnu_syncobj_handle))
        err = syncobj_fd_to_handle(ci, (struct xnu_syncobj_handle *)arg);
    else
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
    int ci = conn_index(fd), r;
    if (ci < 0)
        return close(fd);
    pthread_mutex_lock(&g_lock);
    if (g_conns[ci].fd == fd)
        g_conns[ci].orphan = 1;
    r = close(fd);
    gc_orphans_locked();
    pthread_mutex_unlock(&g_lock);
    return r;
}

int drm_xnu_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                     uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                     uint8_t *bus, uint8_t *dev, uint8_t *func)
{
    return be()->pci_info(vendor, device, subvendor, subdevice, revision, domain, bus, dev, func);
}
