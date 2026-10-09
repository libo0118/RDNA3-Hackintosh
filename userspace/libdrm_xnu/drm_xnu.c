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
 * no file descriptors.
 * - sync_file export: an unlinked temporary file holding the fence by value
 *   (struct xnu_sync_file: ip, ring, seq or "signaled"), read back through
 *   RD_NR_XNU_SYNCOBJ_FENCE. Sequence numbers are per device, so the fd works
 *   in any process (fork, SCM_RIGHTS, XPC fd passing); the kext only accepts
 *   a fence that was really submitted.
 * - opaque export: a /dev/null fd remembered here with the syncobj it
 *   stands for (the same object, so it stays process-local: a syncobj's
 *   later signals cannot follow it into another process). */
#define MAX_SYNCFD 256
static struct { int fd; int ci; uint32_t handle; int used; } g_syncfd[MAX_SYNCFD];
#define XNU_SYNC_FILE_MAGIC "RD3SYNC1"
struct xnu_sync_file { char magic[8]; uint32_t version, signaled, ip, ring; uint64_t seq; };
struct xnu_syncobj_fence { uint32_t handle, op, ip, ring; uint64_t seq; uint32_t signaled, pad; };
#define XNU_NR_SYNCOBJ_FENCE         0xF0   /* RD_NR_XNU_SYNCOBJ_FENCE */
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

static int syncobj_fence(int ci, struct xnu_syncobj_fence *x)
{
    return be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_FENCE, x, sizeof(*x));
}

static int sync_file_export(int ci, uint32_t handle, int *out)
{
    struct xnu_syncobj_fence x;
    struct xnu_sync_file sf;
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    int fd, err;
    memset(&x, 0, sizeof(x));
    x.handle = handle;
    x.op = 0;                                 /* GET */
    err = syncobj_fence(ci, &x);
    if (err)
        return err;
    memset(&sf, 0, sizeof(sf));
    memcpy(sf.magic, XNU_SYNC_FILE_MAGIC, 8);
    sf.version = 1;
    sf.signaled = x.signaled || !x.seq;       /* nothing to wait for */
    sf.ip = x.ip;
    sf.ring = x.ring;
    sf.seq = sf.signaled ? 0 : x.seq;
    snprintf(path, sizeof(path), "%s/rdna3-syncfile.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return errno ? errno : EMFILE;
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (pwrite(fd, &sf, sizeof(sf), 0) != (ssize_t)sizeof(sf)) {
        err = errno ? errno : EIO;
        close(fd);
        return err;
    }
    *out = fd;
    return 0;
}

/* 1: fd is one of our sync_files (fence read into *sf), 0: it is not */
static int sync_file_read(int fd, struct xnu_sync_file *sf)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)sizeof(*sf))
        return 0;
    if (pread(fd, sf, sizeof(*sf), 0) != (ssize_t)sizeof(*sf))
        return 0;
    return !memcmp(sf->magic, XNU_SYNC_FILE_MAGIC, 8) && sf->version == 1;
}

static int sync_file_apply(int ci, uint32_t handle, const struct xnu_sync_file *sf)
{
    struct xnu_syncobj_fence x;
    memset(&x, 0, sizeof(x));
    x.handle = handle;
    x.op = 1;                                 /* SET */
    x.signaled = sf->signaled ? 1 : 0;
    x.ip = sf->ip;
    x.ring = sf->ring;
    x.seq = sf->signaled ? 0 : sf->seq;
    if (!x.signaled && !x.seq)
        x.signaled = 1;
    return syncobj_fence(ci, &x);
}

static int syncobj_handle_to_fd(int ci, struct xnu_syncobj_handle *a)
{
    int fd, i, err;
    if (a->flags & XNU_SYNCOBJ_EXPORT_SYNC_FILE) {
        err = sync_file_export(ci, a->handle, &fd);
        if (err)
            return err;
        a->fd = fd;
        return 0;
    }
    {   /* opaque: the handle must exist (GET fails with EINVAL otherwise) */
        struct xnu_syncobj_fence x;
        memset(&x, 0, sizeof(x));
        x.handle = a->handle;
        err = syncobj_fence(ci, &x);
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
    g_syncfd[i].handle = a->handle;
    g_syncfd[i].used = 1;
    pthread_mutex_unlock(&g_lock);
    a->fd = fd;
    return 0;
}

static int syncobj_fd_to_handle(int ci, struct xnu_syncobj_handle *a)
{
    struct xnu_sync_file sf;
    int i, found = -1;
    /* a sync_file first: its fd number may reuse a stale opaque entry */
    if (sync_file_read(a->fd, &sf)) {
        struct xnu_syncobj_create c = { 0, 0 };
        int err;
        if (a->flags & XNU_SYNCOBJ_IMPORT_SYNC_FILE)
            return sync_file_apply(ci, a->handle, &sf);
        err = be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_CREATE, &c, sizeof(c));
        if (err)
            return err;
        err = sync_file_apply(ci, c.handle, &sf);
        if (err) {
            struct xnu_syncobj_destroy d = { c.handle, 0 };
            be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_DESTROY, &d, sizeof(d));
            return err;
        }
        a->handle = c.handle;
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_SYNCFD; i++)
        if (g_syncfd[i].used && g_syncfd[i].fd == a->fd && g_syncfd[i].ci == ci)
            found = i;
    if (found < 0) {
        pthread_mutex_unlock(&g_lock);
        return EINVAL;
    }
    if (a->flags & XNU_SYNCOBJ_IMPORT_SYNC_FILE) {
        /* an opaque fd given as a sync_file: copy its current fence */
        uint32_t src = g_syncfd[found].handle;
        pthread_mutex_unlock(&g_lock);
        return syncobj_transfer(ci, src, a->handle);
    }
    a->handle = g_syncfd[found].handle;   /* opaque fd: the same object */
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* The UAPI (uapi/rdna3_drm.h) answers with Linux errno numbers, as amdgpu
 * does, since that is what RADV and libdrm compare against (-ETIME on a
 * syncobj wait, -ECANCELED for a guilty context). On macOS a few of them
 * have different values, so translate at the one place errno is set. */
static int rd_errno_to_host(int e)
{
    switch (e) {
    case 62:  return ETIME;         /* RD_ETIME */
    case 89:  return ECANCELED;     /* RD_ECANCELED */
    case 38:  return ENOSYS;        /* RD_ENOSYS */
    case 95:  return EOPNOTSUPP;    /* RD_EOPNOTSUPP */
    default:  return e;             /* EPERM..ENOSPC share their numbers */
    }
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
        errno = rd_errno_to_host(err);
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
