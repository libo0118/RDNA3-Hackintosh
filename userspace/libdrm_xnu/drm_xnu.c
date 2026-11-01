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
static struct { void *addr; size_t len; uint64_t off; uint64_t seq; int ci; } g_maps[MAX_MAPS];
static uint64_t g_map_seq;   /* the order of the maps, for munmap of an address mapped more than once */
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
/* PRIME "dma-buf fds" (DRM_IOCTL_PRIME_HANDLE_TO_FD / FD_TO_HANDLE): an
 * unlinked file whose length is the BO's size (libdrm_amdgpu reads it with
 * lseek(SEEK_END)) and whose first bytes carry the device-wide export
 * token of RD_NR_XNU_BO_EXPORT. Passing the fd passes the capability. */
#define XNU_DMABUF_MAGIC "RD3BUF01"
struct xnu_dmabuf { char magic[8]; uint32_t version, pad; uint64_t token, size; };
struct xnu_bo_share { uint32_t handle, pad; uint64_t token, size; };
struct xnu_prime_handle { uint32_t handle, flags; int32_t fd; };
#define XNU_NR_PRIME_HANDLE_TO_FD    0x2d
#define XNU_NR_PRIME_FD_TO_HANDLE    0x2e
/* an IOSurface plane as a "dma-buf fd" (drm_xnu_surface_fd): importing it
 * asks the kext for a BO over the surface's VRAM in the importing
 * connection (RD_NR_XNU_SURFACE_BO), checked against that process's own
 * IOSurface access; nothing is exported, so nothing outlives the importer */
#define XNU_SURFBUF_MAGIC "RD3SURF1"
struct xnu_surfbuf { char magic[8]; uint32_t version, surface_id, plane, pad; uint64_t size; };
/* RD_NR_XNU_SURFACE_BO; the out tail (N71 (c)) stays 0 from an older kext */
struct xnu_surface_bo { uint32_t op, surface_id, plane, handle; uint64_t size; uint32_t out_ver, out_flags; uint64_t alloc_gen; };
#define XNU_NR_SURFACE_BO            0xF5   /* RD_NR_XNU_SURFACE_BO */
static int rd_errno_to_host(int e);
#define XNU_SURFACE_BO_IMPORT        0
#define XNU_SURFACE_BO_DIRTY         1
#define XNU_SURFACE_BO_MARK          2   /* DRIVER-PLAN N41 */
#define XNU_SURFACE_BO_DIRTY_TRY     3
#define XNU_SURFACE_BO_SHARE         4   /* DRIVER-PLAN N71 (c) */
#define XNU_SURFACE_BO_OUT_SHARED_RO 0x1u
#define XNU_NR_BO_EXPORT             0xF1   /* RD_NR_XNU_BO_EXPORT */
#define XNU_NR_BO_IMPORT             0xF2
#define XNU_DRM_CLOEXEC              02000000   /* Linux O_CLOEXEC, as drm.h DRM_CLOEXEC */
/* DRM_AMDGPU_FENCE_TO_HANDLE with an fd result: the kext makes a syncobj
 * (AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ), the fd is made here */
#define XNU_NR_AMDGPU_FENCE_TO_HANDLE (0x40 + 0x14)
struct xnu_fence_to_handle {
    uint32_t ctx_id, ip_type, ip_instance, ring;
    uint64_t seq_no;
    uint32_t what, pad;
};
#define XNU_FENCE_TO_HANDLE_GET_SYNCOBJ    0
#define XNU_FENCE_TO_HANDLE_GET_SYNCOBJ_FD 1
#define XNU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD 2
/* DRM_AMDGPU_SCHED names the file whose contexts it re-prioritizes by a DRM
 * fd (amdgpu_sched_*_priority_override: fget + amdgpu_file_to_fpriv); the
 * kext has no fds, so the fd is replaced by its connection's file id
 * (RD_NR_XNU_FILE_ID), 0 when it is no DRM fd of this process */
#define XNU_NR_AMDGPU_SCHED          (0x40 + 0x15)
#define XNU_NR_FILE_ID               0xF4   /* RD_NR_XNU_FILE_ID */
struct xnu_sched { uint32_t op; int32_t fd; int32_t priority; uint32_t ctx_id; };
struct xnu_file_id { uint32_t id, pad; };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_surf_lock = PTHREAD_MUTEX_INITIALIZER;   /* drm_xnu_surface_dirty's connection */
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
            int i;
            be()->close(g_conns[ci].conn);
            g_conns[ci].used = 0;
            g_conns[ci].orphan = 0;
            /* its mappings went with the connection (a failed unmap kept
             * its entry until here) */
            for (i = 0; i < MAX_MAPS; i++)
                if (g_maps[i].addr && g_maps[i].ci == ci)
                    g_maps[i].addr = NULL;
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

static int prime_handle_to_fd(int ci, struct xnu_prime_handle *a)
{
    struct xnu_bo_share x;
    struct xnu_dmabuf b;
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    int fd, err;
    memset(&x, 0, sizeof(x));
    x.handle = a->handle;
    err = be()->ioctl(g_conns[ci].conn, XNU_NR_BO_EXPORT, &x, sizeof(x));
    if (err)
        return err;
    memset(&b, 0, sizeof(b));
    memcpy(b.magic, XNU_DMABUF_MAGIC, 8);
    b.version = 1;
    b.token = x.token;
    b.size = x.size;
    snprintf(path, sizeof(path), "%s/rdna3-dmabuf.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return errno ? errno : EMFILE;
    unlink(path);
    if (pwrite(fd, &b, sizeof(b), 0) != (ssize_t)sizeof(b) ||
        (x.size > sizeof(b) && ftruncate(fd, (off_t)x.size) != 0)) {
        err = errno ? errno : EIO;
        close(fd);
        return err;
    }
    if (a->flags & (XNU_DRM_CLOEXEC | O_CLOEXEC))
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    a->fd = fd;
    return 0;
}

/* the kernel's answer to the calling thread's last surface import: Vulkan
 * turns every refusal into one VkResult, the Metal plugin logs this */
static __thread int g_surface_err;

int drm_xnu_surface_last_error(void)
{
    return g_surface_err;
}

/* N71 (c): what the calling thread's last surface import got (flags
 * XNU_SURFACE_BO_OUT_*, the allocation generation; 0 from an older kext) */
static __thread uint32_t g_import_flags;
static __thread uint64_t g_import_gen;

int drm_xnu_surface_last_import(uint32_t *flags, uint64_t *gen)
{
    if (flags)
        *flags = g_surface_err ? 0u : g_import_flags;
    if (gen)
        *gen = g_surface_err ? 0u : g_import_gen;
    return g_surface_err;
}

/* N71 (c): the connections this process imported surfaces on writable: a
 * share record must reach the DRM file that holds the import (the kernel
 * looks the import up there). The newest import of a plane wins; a full
 * table forgets the oldest. Under g_surf_lock. */
#define XNU_SURF_IMPORTS 64
static struct { uint32_t surface_id, plane; uintptr_t conn; int ci; unsigned stamp; int used; } g_imports[XNU_SURF_IMPORTS];
static unsigned g_import_stamp;

static void surface_import_note(int ci, uint32_t surface_id, uint32_t plane)
{
    unsigned i, k = XNU_SURF_IMPORTS;
    pthread_mutex_lock(&g_surf_lock);
    for (i = 0; i < XNU_SURF_IMPORTS && k == XNU_SURF_IMPORTS; i++)
        if (g_imports[i].used && g_imports[i].surface_id == surface_id && g_imports[i].plane == plane)
            k = i;
    for (i = 0; i < XNU_SURF_IMPORTS && k == XNU_SURF_IMPORTS; i++)
        if (!g_imports[i].used)
            k = i;
    if (k == XNU_SURF_IMPORTS) {   /* full: the oldest goes */
        k = 0;
        for (i = 1; i < XNU_SURF_IMPORTS; i++)
            if (g_imports[i].stamp - g_imports[k].stamp > 0x80000000u)   /* i older than k (wrap-safe) */
                k = i;
    }
    g_imports[k].surface_id = surface_id;
    g_imports[k].plane = plane;
    g_imports[k].ci = ci;
    g_imports[k].conn = g_conns[ci].conn;
    g_imports[k].stamp = ++g_import_stamp;
    g_imports[k].used = 1;
    pthread_mutex_unlock(&g_surf_lock);
}

static int surface_fd_to_handle(int ci, int fd, uint32_t *handle)
{
    struct xnu_surfbuf b;
    struct xnu_surface_bo s;
    int err;
    g_import_flags = 0;
    g_import_gen = 0;
    if (pread(fd, &b, sizeof(b), 0) != (ssize_t)sizeof(b) || memcmp(b.magic, XNU_SURFBUF_MAGIC, 8) ||
        b.version != 1)
        return g_surface_err = EBADF;
    memset(&s, 0, sizeof(s));
    s.op = XNU_SURFACE_BO_IMPORT;
    s.surface_id = b.surface_id;
    s.plane = b.plane;
    s.size = b.size;
    err = be()->ioctl(g_conns[ci].conn, XNU_NR_SURFACE_BO, &s, sizeof(s));
    g_surface_err = rd_errno_to_host(err);
    if (err)
        return err;
    *handle = s.handle;
    if (s.out_ver) {
        g_import_flags = s.out_flags;
        g_import_gen = s.alloc_gen;
    }
    if (!(g_import_flags & XNU_SURFACE_BO_OUT_SHARED_RO))   /* a read-only import never shares */
        surface_import_note(ci, b.surface_id, b.plane);
    return 0;
}

static int prime_fd_to_handle(int ci, struct xnu_prime_handle *a)
{
    struct xnu_bo_share x;
    struct xnu_dmabuf b;
    struct stat st;
    int err;
    if (!fstat(a->fd, &st) && S_ISREG(st.st_mode) && st.st_size >= (off_t)sizeof(struct xnu_surfbuf) &&
        pread(a->fd, &b, 8, 0) == 8 && !memcmp(b.magic, XNU_SURFBUF_MAGIC, 8))
        return surface_fd_to_handle(ci, a->fd, &a->handle);
    if (fstat(a->fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof(b) ||
        pread(a->fd, &b, sizeof(b), 0) != (ssize_t)sizeof(b) ||
        memcmp(b.magic, XNU_DMABUF_MAGIC, 8) || b.version != 1)
        return EBADF;
    memset(&x, 0, sizeof(x));
    x.token = b.token;
    err = be()->ioctl(g_conns[ci].conn, XNU_NR_BO_IMPORT, &x, sizeof(x));
    if (err)
        return err;
    a->handle = x.handle;
    return 0;
}

static int fence_to_fd(int ci, struct xnu_fence_to_handle *a)
{
    uint32_t what = a->what, handle;
    int err, fd = -1;
    a->what = XNU_FENCE_TO_HANDLE_GET_SYNCOBJ;
    err = be()->ioctl(g_conns[ci].conn, XNU_NR_AMDGPU_FENCE_TO_HANDLE, a, sizeof(*a));
    a->what = what;
    if (err)
        return err;
    handle = *(uint32_t *)a;                  /* out.handle overlays the input */
    if (what == XNU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD) {
        struct xnu_syncobj_destroy d = { handle, 0 };
        err = sync_file_export(ci, handle, &fd);
        be()->ioctl(g_conns[ci].conn, XNU_NR_SYNCOBJ_DESTROY, &d, sizeof(d));
    } else {
        struct xnu_syncobj_handle h;
        memset(&h, 0, sizeof(h));
        h.handle = handle;                    /* opaque: the fd is this syncobj */
        err = syncobj_handle_to_fd(ci, &h);
        fd = h.fd;
    }
    if (err)
        return err;
    memset(a, 0, sizeof(*a));
    *(uint32_t *)a = (uint32_t)fd;
    return 0;
}

static int sched_ioctl(int ci, const struct xnu_sched *a)
{
    struct xnu_sched k = *a;
    struct xnu_file_id id = { 0, 0 };
    int ti = conn_index(a->fd);
    /* the kext still checks privilege first (-EACCES), then the id */
    if (ti >= 0 && be()->ioctl(g_conns[ti].conn, XNU_NR_FILE_ID, &id, sizeof(id)) != 0)
        id.id = 0;
    k.fd = (int32_t)id.id;
    return be()->ioctl(g_conns[ci].conn, XNU_NR_AMDGPU_SCHED, &k, sizeof(k));
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
    case 61:  return ENODATA;       /* RD_ENODATA */
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
    else if (nr == XNU_NR_AMDGPU_FENCE_TO_HANDLE && size >= sizeof(struct xnu_fence_to_handle) &&
             (((struct xnu_fence_to_handle *)arg)->what == XNU_FENCE_TO_HANDLE_GET_SYNCOBJ_FD ||
              ((struct xnu_fence_to_handle *)arg)->what == XNU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD))
        err = fence_to_fd(ci, (struct xnu_fence_to_handle *)arg);
    else if (nr == XNU_NR_PRIME_HANDLE_TO_FD && size >= sizeof(struct xnu_prime_handle))
        err = prime_handle_to_fd(ci, (struct xnu_prime_handle *)arg);
    else if (nr == XNU_NR_PRIME_FD_TO_HANDLE && size >= sizeof(struct xnu_prime_handle))
        err = prime_fd_to_handle(ci, (struct xnu_prime_handle *)arg);
    else if (nr == XNU_NR_AMDGPU_SCHED && size >= sizeof(struct xnu_sched))
        err = sched_ioctl(ci, (const struct xnu_sched *)arg);   /* in only (DRM_IOW) */
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
            g_maps[i].off = (uint64_t)off;
            g_maps[i].seq = ++g_map_seq;
            g_maps[i].ci = ci;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return p;
}

/* An address can be in the table more than once: the kernel hands a task
 * the mapping it already has for a second map of the same BO (xnu's
 * makeMapping reuses a compatible one), and the loopback maps BOs that share
 * memory (IOSurface imports) at one address. The newest entry goes; the
 * kernel's mapping goes with the last entry of its connection and BO. */
int drm_xnu_munmap(void *addr, size_t len)
{
    int i, j, ci;
    uint64_t off;
    pthread_mutex_lock(&g_lock);
    for (i = -1, j = 0; j < MAX_MAPS; j++)
        if (g_maps[j].addr == addr && (i < 0 || g_maps[j].seq > g_maps[i].seq))
            i = j;
    if (i < 0) {
        pthread_mutex_unlock(&g_lock);
        return munmap(addr, len);
    }
    ci = g_maps[i].ci;
    off = g_maps[i].off;
    for (j = 0; j < MAX_MAPS; j++)
        if (j != i && g_maps[j].addr == addr && g_maps[j].ci == ci && g_maps[j].off == off) {
            g_maps[i].addr = NULL;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    /* the entry stays until the kernel let the mapping go: a failed unmap
     * (the BO's handle already closed, a stale offset) must not leave a
     * retry to libc's munmap, which would drop the pages and leave the
     * kernel's IOMemoryMap, and the BO it holds, alive (N47 (b)) */
    if (be()->unmap(g_conns[ci].conn, addr, len, off)) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "drm_xnu: unmap of %p (offset 0x%llx) failed: the mapping stays until the device "
                        "is closed\n", addr, (unsigned long long)off);
        errno = EINVAL;
        return -1;
    }
    g_maps[i].addr = NULL;
    pthread_mutex_unlock(&g_lock);
    return 0;
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

int drm_xnu_surface_fd(uint32_t surface_id, uint32_t plane, uint64_t size)
{
    struct xnu_surfbuf b;
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    int fd;
    if (!surface_id || !size || size > UINT64_MAX - 4095) {
        errno = EINVAL;
        return -1;
    }
    size = (size + 4095) & ~4095ull;   /* whole pages, as dma-bufs are: RADV maps all of it */
    memset(&b, 0, sizeof(b));
    memcpy(b.magic, XNU_SURFBUF_MAGIC, 8);
    b.version = 1;
    b.surface_id = surface_id;
    b.plane = plane;
    b.size = size;
    snprintf(path, sizeof(path), "%s/rdna3-surface.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* sized like the surface: libdrm_amdgpu reads the BO size with lseek */
    if (pwrite(fd, &b, sizeof(b), 0) != (ssize_t)sizeof(b) ||
        (size > sizeof(b) && ftruncate(fd, (off_t)size) != 0)) {
        int e = errno ? errno : EIO;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* one connection for the process's dirty and GPU marks. A kext without the
 * ioctl, or without the op (EINVAL for an unknown nr or op; the plane is
 * in range here) is asked no more for that op ('*unknown'); a failed open
 * is tried again after 256 calls (the device may not be up yet) */
static int surface_op(uint32_t op, uint32_t surface_id, uint32_t plane, int *unknown)
{
    static int fd = -1, skip;
    struct xnu_surface_bo s;
    int r, e = 0;
    pthread_mutex_lock(&g_surf_lock);
    if (fd < 0 && !*unknown && !skip) {
        fd = drm_xnu_open(DRM_XNU_RENDER_NODE, O_RDWR);
        if (fd < 0) {
            e = errno ? errno : ENODEV;
            skip = 256;
        }
    } else if (fd < 0 && skip && !*unknown) {
        skip--;
        e = ENXIO;
    }
    if (*unknown)
        e = ENOSYS;
    pthread_mutex_unlock(&g_surf_lock);
    if (e) {
        errno = e;
        return -1;
    }
    memset(&s, 0, sizeof(s));
    s.op = op;
    s.surface_id = surface_id;
    s.plane = plane;
    r = drm_xnu_ioctl_nr(fd, XNU_NR_SURFACE_BO, &s, sizeof(s));
    if (r && errno == EINVAL && plane <= 15 && (op != XNU_SURFACE_BO_MARK || surface_id)) {
        pthread_mutex_lock(&g_surf_lock);
        *unknown = 1;
        pthread_mutex_unlock(&g_surf_lock);
        errno = ENOSYS;
    }
    return r;
}

int drm_xnu_surface_dirty(uint32_t surface_id, uint32_t plane)
{
    static int unknown;
    return surface_op(XNU_SURFACE_BO_DIRTY, surface_id, plane, &unknown);
}

int drm_xnu_surface_dirty_try(uint32_t surface_id, uint32_t plane)
{
    static int unknown;
    return surface_op(XNU_SURFACE_BO_DIRTY_TRY, surface_id, plane, &unknown);
}

int drm_xnu_surface_mark(uint32_t surface_id, int pending)
{
    static int unknown;
    return surface_op(XNU_SURFACE_BO_MARK, surface_id, pending ? 1u : 0u, &unknown);
}

int drm_xnu_surface_share(uint32_t surface_id, uint32_t plane)
{
    struct xnu_surface_bo s;
    uintptr_t conn = 0;
    unsigned i, best = XNU_SURF_IMPORTS;
    int ci = -1, err;
    if (!surface_id || plane > 15) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_surf_lock);
    for (i = 0; i < XNU_SURF_IMPORTS; i++)
        if (g_imports[i].used && g_imports[i].surface_id == surface_id && g_imports[i].plane == plane &&
            (best == XNU_SURF_IMPORTS || g_imports[i].stamp - g_imports[best].stamp < 0x80000000u))
            best = i;
    if (best < XNU_SURF_IMPORTS) {
        ci = g_imports[best].ci;
        conn = g_imports[best].conn;
    }
    pthread_mutex_unlock(&g_surf_lock);
    /* the connection must still be the one the import went to */
    pthread_mutex_lock(&g_lock);
    if (ci >= 0 && (!g_conns[ci].used || g_conns[ci].conn != conn))
        ci = -1;
    pthread_mutex_unlock(&g_lock);
    if (ci < 0) {
        errno = ENOENT;   /* no writable import of it in this process */
        return -1;
    }
    memset(&s, 0, sizeof(s));
    s.op = XNU_SURFACE_BO_SHARE;
    s.surface_id = surface_id;
    s.plane = plane;
    err = be()->ioctl(conn, XNU_NR_SURFACE_BO, &s, sizeof(s));
    if (trace())
        fprintf(stderr, "drm_xnu: surface %u plane %u share -> %d\n", surface_id, plane, err);
    if (err) {
        errno = err == 22 ? ENOSYS : rd_errno_to_host(err);   /* RD_EINVAL: a kext without the op */
        return -1;
    }
    return 0;
}

int drm_xnu_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                     uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                     uint8_t *bus, uint8_t *dev, uint8_t *func)
{
    return be()->pci_info(vendor, device, subvendor, subdevice, revision, domain, bus, dev, func);
}
