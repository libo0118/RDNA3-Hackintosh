/*
 * End-to-end userspace test without a GPU: the unmodified libdrm_amdgpu
 * (third_party/libdrm) + our xf86drm_xnu core + drm_xnu with the loopback
 * backend, which runs uapi/rdna3_drm.c in-process. This is the same call
 * path RADV's winsys takes on macOS, minus the IOKit hop.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "amdgpu.h"
#include "amdgpu_drm.h"
#include "xf86drm.h"
#include "drm_xnu.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

int main(void)
{
    amdgpu_device_handle dev;
    uint32_t major = 0, minor = 0;
    struct amdgpu_gpu_info gi;
    struct drm_amdgpu_info_device di;
    drmDevicePtr devs[4];
    drmVersionPtr ver;
    int fd, n;

    n = drmGetDevices2(0, devs, 4);
    CHECK(n == 1 && devs[0]->deviceinfo.pci->device_id == 0x747E);
    CHECK(!strcmp(devs[0]->nodes[DRM_NODE_RENDER], "/dev/dri/renderD128"));
    drmFreeDevices(devs, n);

    fd = drm_xnu_open("/dev/dri/renderD128", 0);
    CHECK(fd >= 0);
    ver = drmGetVersion(fd);
    CHECK(ver && !strcmp(ver->name, "amdgpu"));
    drmFreeVersion(ver);
    CHECK(drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER);

    /* dup()s the fd internally (F_DUPFD_CLOEXEC) - must keep working */
    CHECK(amdgpu_device_initialize(fd, &major, &minor, &dev) == 0);
    CHECK(major == 3 && minor >= 49);
    CHECK(amdgpu_device_get_fd(dev) != fd && drm_xnu_is_drm_fd(amdgpu_device_get_fd(dev)));
    /* RADV closes its own fd right after creating the winsys */
    CHECK(drm_xnu_close(fd) == 0);
    CHECK(drm_xnu_is_drm_fd(amdgpu_device_get_fd(dev)));

    memset(&gi, 0, sizeof(gi));
    CHECK(amdgpu_query_gpu_info(dev, &gi) == 0);
    CHECK(gi.asic_id == 0x747E && gi.family_id == AMDGPU_FAMILY_GC_11_0_0);
    CHECK(gi.num_shader_engines == 3 && gi.cu_active_number == 60);
    memset(&di, 0, sizeof(di));
    CHECK(amdgpu_query_info(dev, AMDGPU_INFO_DEV_INFO, sizeof(di), &di) == 0);
    /* no CP gfx shadowing on a physical function (gfx_v11_0_check_fw_cp_gfx_shadow) */
    CHECK(di.shadow_size == 0 && di.shadow_alignment == 0 && di.csa_size == 0 && di.csa_alignment == 0);
    printf("libdrm_amdgpu sees: %s, family %u, %u CUs\n",
           amdgpu_get_marketing_name(dev) ? amdgpu_get_marketing_name(dev) : "(no name)",
           gi.family_id, gi.cu_active_number);

    /* BO: alloc, CPU map, write/read back, VA map/unmap, free */
    for (int dom = AMDGPU_GEM_DOMAIN_GTT; dom <= AMDGPU_GEM_DOMAIN_VRAM; dom <<= 1) {
        struct amdgpu_bo_alloc_request req = { 1 << 20, 4096, (uint32_t)dom, 0 };
        amdgpu_bo_handle bo;
        amdgpu_va_handle vah;
        uint64_t va = 0;
        uint32_t *p = NULL;
        unsigned bad = 0;

        CHECK(amdgpu_bo_alloc(dev, &req, &bo) == 0);
        CHECK(amdgpu_bo_cpu_map(bo, (void **)&p) == 0 && p);
        for (unsigned i = 0; p && i < (1u << 18); i++)
            p[i] = i ^ 0xA5A5A5A5u;
        for (unsigned i = 0; p && i < (1u << 18); i++)
            bad += p[i] != (i ^ 0xA5A5A5A5u);
        CHECK(bad == 0);
        CHECK(amdgpu_bo_cpu_unmap(bo) == 0);
        {
            /* drm_mmap twice (the kernel hands back the same mapping), two
             * unmaps: only the last goes to the backend, with the BO's offset
             * (DRIVER-PLAN N47 (b): the kext finds the mapping through it) */
            extern uint32_t lb_unmap_refused;
            union drm_amdgpu_gem_mmap mm;
            uint32_t kh = 0;
            void *a, *b;
            memset(&mm, 0, sizeof(mm));
            CHECK(amdgpu_bo_export(bo, amdgpu_bo_handle_type_kms, &kh) == 0);
            mm.in.handle = kh;
            CHECK(drmCommandWriteRead(amdgpu_device_get_fd(dev), DRM_AMDGPU_GEM_MMAP, &mm, sizeof(mm)) == 0);
            a = drm_xnu_mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, amdgpu_device_get_fd(dev), (off_t)mm.out.addr_ptr);
            b = drm_xnu_mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, amdgpu_device_get_fd(dev), (off_t)mm.out.addr_ptr);
            CHECK(a != MAP_FAILED && a == b);
            CHECK(drm_xnu_munmap(b, 1 << 20) == 0 && drm_xnu_munmap(a, 1 << 20) == 0 && lb_unmap_refused == 0);
        }
        CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, 1 << 20, 4096, 0, &va,
                                    &vah, 0) == 0 && va);
        CHECK(amdgpu_bo_va_op(bo, 0, 1 << 20, va, 0, AMDGPU_VA_OP_MAP) == 0);
        CHECK(amdgpu_bo_va_op(bo, 0, 1 << 20, va, 0, AMDGPU_VA_OP_UNMAP) == 0);
        CHECK(amdgpu_va_range_free(vah) == 0);
        CHECK(amdgpu_bo_free(bo) == 0);
    }

    {
        /* an unmap after GEM_CLOSE fails, and the entry stays: a retry must
         * not reach libc's munmap (the kernel's mapping would live on) */
        extern uint32_t lb_unmap_refused;
        struct amdgpu_bo_alloc_request rq;
        amdgpu_bo_handle b2;
        union drm_amdgpu_gem_mmap mm;
        uint32_t kh = 0, before = lb_unmap_refused;
        void *a;
        memset(&rq, 0, sizeof(rq));
        rq.alloc_size = 4096;
        rq.phys_alignment = 4096;
        rq.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
        CHECK(amdgpu_bo_alloc(dev, &rq, &b2) == 0);
        CHECK(amdgpu_bo_export(b2, amdgpu_bo_handle_type_kms, &kh) == 0);
        memset(&mm, 0, sizeof(mm));
        mm.in.handle = kh;
        CHECK(drmCommandWriteRead(amdgpu_device_get_fd(dev), DRM_AMDGPU_GEM_MMAP, &mm, sizeof(mm)) == 0);
        a = drm_xnu_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, amdgpu_device_get_fd(dev),
                         (off_t)mm.out.addr_ptr);
        CHECK(a != MAP_FAILED);
        CHECK(amdgpu_bo_free(b2) == 0);   /* libdrm did not map it: no unmap first */
        CHECK(drm_xnu_munmap(a, 4096) == -1 && lb_unmap_refused == before + 1);
        CHECK(drm_xnu_munmap(a, 4096) == -1 && lb_unmap_refused == before + 2);   /* still ours */
    }
    /* syncobj "fds": a sync_file export is a file holding the fence by value
     * (usable in any process), an opaque export is the same object
     * (process-local), transfer copies points */
    {
        uint32_t a = 0, b = 0, c = 0;
        int sfd = -1, ofd = -1;
        CHECK(amdgpu_cs_create_syncobj2(dev, 0, &a) == 0 && amdgpu_cs_create_syncobj2(dev, 0, &b) == 0);
        /* no fence yet: -EINVAL as in DRM, -ETIME when waiting for a submit */
        CHECK(amdgpu_cs_syncobj_wait(dev, &b, 1, 0, 0, NULL) == -EINVAL);
        CHECK(amdgpu_cs_syncobj_wait(dev, &b, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == -ETIME);
        CHECK(amdgpu_cs_syncobj_signal(dev, &a, 1) == 0);
        CHECK(amdgpu_cs_syncobj_export_sync_file(dev, a, &sfd) == 0 && sfd >= 0);
        CHECK(amdgpu_cs_syncobj_import_sync_file(dev, b, sfd) == 0);
        CHECK(amdgpu_cs_syncobj_wait(dev, &b, 1, 0, 0, NULL) == 0);
        CHECK(amdgpu_cs_export_syncobj(dev, a, &ofd) == 0 && ofd >= 0 && ofd != sfd);
        CHECK(amdgpu_cs_import_syncobj(dev, ofd, &c) == 0 && c == a);
        CHECK(amdgpu_cs_syncobj_reset(dev, &b, 1) == 0);
        CHECK(amdgpu_cs_syncobj_timeline_signal(dev, &a, (uint64_t[]){ 3 }, 1) == 0);
        CHECK(amdgpu_cs_syncobj_transfer(dev, b, 4, a, 3, 0) == 0);
        {
            uint64_t pt = 0;
            CHECK(amdgpu_cs_syncobj_query(dev, &b, &pt, 1) == 0 && pt == 4);
        }
        CHECK(amdgpu_cs_syncobj_import_sync_file(dev, b, 12345) != 0);   /* not ours */
        close(sfd); close(ofd);
        CHECK(amdgpu_cs_destroy_syncobj(dev, a) == 0 && amdgpu_cs_destroy_syncobj(dev, b) == 0);
    }
    /* GEM_USERPTR through libdrm_amdgpu (amdgpu_create_bo_from_user_mem):
     * the BO is the client's own pages, VA-mappable, never mmap()ed */
    {
        static uint8_t pages[4 * 4096] __attribute__((aligned(4096)));
        amdgpu_bo_handle ubo = NULL;
        amdgpu_va_handle vah;
        uint64_t va = 0;
        void *p = NULL;
        CHECK(amdgpu_create_bo_from_user_mem(dev, pages, sizeof(pages), &ubo) == 0 && ubo);
        CHECK(amdgpu_create_bo_from_user_mem(dev, pages + 8, 4096, &ubo) != 0);   /* unaligned */
        CHECK(amdgpu_bo_cpu_map(ubo, &p) != 0);
        CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, sizeof(pages), 4096, 0, &va,
                                    &vah, 0) == 0 && va);
        CHECK(amdgpu_bo_va_op(ubo, 0, sizeof(pages), va, 0, AMDGPU_VA_OP_MAP) == 0);
        CHECK(amdgpu_bo_va_op(ubo, 0, sizeof(pages), va, 0, AMDGPU_VA_OP_UNMAP) == 0);
        CHECK(amdgpu_va_range_free(vah) == 0);
        CHECK(amdgpu_bo_free(ubo) == 0);
    }
    /* PRIME: a dma-buf fd carries the BO to another connection (another
     * process in real life): same size through lseek, a handle there; the
     * export dies with the last holder; userptr BOs are not exported */
    {
        struct amdgpu_bo_alloc_request req = { 256 << 10, 4096, AMDGPU_GEM_DOMAIN_VRAM, 0 };
        static uint8_t upages[4096] __attribute__((aligned(4096)));
        amdgpu_bo_handle bo = NULL, same = NULL, ubo = NULL;
        struct amdgpu_bo_import_result imp;
        uint32_t dmafd = 0, h2 = 0, h3 = 0;
        int fd2 = drm_xnu_open("/dev/dri/renderD128", 0), fd3;
        CHECK(fd2 >= 0);
        CHECK(amdgpu_bo_alloc(dev, &req, &bo) == 0);
        CHECK(amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd, &dmafd) == 0);
        CHECK(lseek((int)dmafd, 0, SEEK_END) == (off_t)(256 << 10));
        CHECK(drmPrimeFDToHandle(fd2, (int)dmafd, &h2) == 0 && h2);
        CHECK(drmPrimeFDToHandle(fd2, (int)dmafd, &h3) == 0 && h3 == h2);   /* one handle per file */
        memset(&imp, 0, sizeof(imp));
        CHECK(amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd, dmafd, &imp) == 0);
        same = imp.buf_handle;
        CHECK(same == bo && imp.alloc_size == (256 << 10));               /* libdrm's own dedupe */
        CHECK(amdgpu_bo_free(same) == 0);
        CHECK(drmPrimeFDToHandle(fd2, 12345, &h3) != 0);                    /* not ours */
        CHECK(amdgpu_create_bo_from_user_mem(dev, upages, sizeof(upages), &ubo) == 0);
        {
            uint32_t ufd = 0;
            CHECK(amdgpu_bo_export(ubo, amdgpu_bo_handle_type_dma_buf_fd, &ufd) != 0 && errno == EPERM);
        }
        CHECK(amdgpu_bo_free(ubo) == 0);
        /* the exporter frees its BO: the importer keeps it, the token too */
        CHECK(amdgpu_bo_free(bo) == 0);
        fd3 = drm_xnu_open("/dev/dri/renderD128", 0);
        CHECK(fd3 >= 0 && drmPrimeFDToHandle(fd3, (int)dmafd, &h3) == 0 && h3);
        CHECK(drm_xnu_close(fd3) == 0);
        /* the importers go; the exporter's file still holds the export (as
         * the dma-buf fd would on Linux): importable until that file closes */
        CHECK(drmCloseBufferHandle(fd2, h2) == 0);
        CHECK(drm_xnu_close(fd2) == 0);
        fd3 = drm_xnu_open("/dev/dri/renderD128", 0);
        CHECK(fd3 >= 0 && drmPrimeFDToHandle(fd3, (int)dmafd, &h3) == 0);
        CHECK(drm_xnu_close(fd3) == 0);
        close((int)dmafd);
    }
    /* an IOSurface plane as a dma-buf (A3): RADV's OPAQUE_FD import reaches
     * RD_NR_XNU_SURFACE_BO in its own connection; every import sees the
     * same memory; nothing is exported */
    {
        extern uint32_t lb_surface_dirty_count;
        struct amdgpu_bo_import_result imp, imp2;
        uint8_t *p = NULL, *q = NULL;
        int sfd = drm_xnu_surface_fd(0x51, 0, 5000), bad;
        CHECK(sfd >= 0 && lseek(sfd, 0, SEEK_END) == 8192);   /* whole pages */
        memset(&imp, 0, sizeof(imp));
        CHECK(amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd, (uint32_t)sfd, &imp) == 0);
        CHECK(imp.alloc_size == 8192 && amdgpu_bo_cpu_map(imp.buf_handle, (void **)&p) == 0 && p);
        memset(&imp2, 0, sizeof(imp2));
        CHECK(amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd, (uint32_t)sfd, &imp2) == 0);
        CHECK(amdgpu_bo_cpu_map(imp2.buf_handle, (void **)&q) == 0 && q);
        if (p && q) {
            p[100] = 0xA3;
            CHECK(q[100] == 0xA3);
        }
        CHECK(amdgpu_bo_free(imp2.buf_handle) == 0);
        CHECK(amdgpu_bo_free(imp.buf_handle) == 0);
        close(sfd);
        bad = drm_xnu_surface_fd(0x52, 0, 4096);              /* not this process's surface */
        CHECK(bad >= 0 && amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd, (uint32_t)bad, &imp) != 0);
        close(bad);
        bad = drm_xnu_surface_fd(0x51, 0, 128u << 10);        /* larger than the surface */
        CHECK(bad >= 0 && amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd, (uint32_t)bad, &imp) != 0);
        close(bad);
        CHECK(drm_xnu_surface_fd(0, 0, 4096) == -1 && errno == EINVAL);
        CHECK(drm_xnu_surface_fd(0x51, 0, 0) == -1 && errno == EINVAL);
        CHECK(drm_xnu_surface_fd(0x51, 0, UINT64_MAX) == -1 && errno == EINVAL);
        CHECK(drm_xnu_surface_dirty(0x51, 0) == 0 && lb_surface_dirty_count == 1);
        CHECK(drm_xnu_surface_dirty(0x52, 0) == -1 && errno == ENOENT);
        /* the Metal plugin's probe: surface 0 never exists (the kext knows
         * the ioctl); an unknown ioctl answers EINVAL (what an older kext
         * gives the probe) */
        CHECK(drm_xnu_surface_dirty(0, 0) == -1 && errno == ENOENT);
        {
            uint8_t junk[32] = { 0 };
            CHECK(drm_xnu_ioctl_nr(fd, 0xF9, junk, sizeof(junk)) == -1 && errno == EINVAL);
        }
        CHECK(drm_xnu_surface_dirty(0x51, 0) == 0 && lb_surface_dirty_count == 2);   /* still asked */
        /* DRIVER-PLAN N41: the try-lock dirty and the GPU marks; an unknown op poisons only itself */
        {
            extern uint32_t lb_surface_try_count, lb_surface_mark_count[2];
            CHECK(drm_xnu_surface_dirty_try(0x51, 0) == 0 && lb_surface_try_count == 1 && lb_surface_dirty_count == 3);
            CHECK(drm_xnu_surface_dirty_try(0x52, 0) == -1 && errno == ENOENT);
            CHECK(drm_xnu_surface_mark(0x51, 1) == 0 && lb_surface_mark_count[1] == 1);
            CHECK(drm_xnu_surface_mark(0x51, 0) == 0 && lb_surface_mark_count[0] == 1);
            CHECK(drm_xnu_surface_mark(0x52, 7) == -1 && errno == ENOENT);
            CHECK(drm_xnu_surface_mark(0, 1) == -1 && errno == EINVAL);   /* not taken as "no such op" */
            CHECK(drm_xnu_surface_mark(0x51, 1) == 0 && lb_surface_mark_count[1] == 2);
            CHECK(drm_xnu_surface_dirty(0x51, 0) == 0 && lb_surface_dirty_count == 4);
        }
    }
    /* malformed surface files: refused, nothing imported, the reason kept */
    {
        static const struct { const char *magic; uint32_t version, id, plane; uint64_t size; size_t len; int err; } bad[] = {
            { "RD3SURF1", 2, 0x51, 0, 4096, 32, EBADF },   /* another version */
            { "RD3SURF2", 1, 0x51, 0, 4096, 32, EBADF },   /* another magic: not a dma-buf either */
            { "RD3SURF1", 1, 0x51, 0, 4096, 31, EBADF },   /* short: read as a (bad) dma-buf */
            { "RD3SURF1", 1, 0, 0, 4096, 32, EINVAL },     /* surface 0 */
            { "RD3SURF1", 1, 0x51, 16, 4096, 32, EINVAL }, /* past the last plane */
            { "RD3SURF1", 1, 0x51, 0, 1u << 20, 32, ENOSPC },
            { "RD3SURF1", 1, 0x77, 0, 4096, 32, ENOENT },  /* not this process's */
        };
        unsigned i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            char path[] = "/tmp/rdna3-test-surf.XXXXXX";
            uint8_t b[32];
            uint32_t h = 0;
            int tf = mkstemp(path), r;
            CHECK(tf >= 0);
            if (tf < 0)
                continue;
            unlink(path);
            memset(b, 0, sizeof(b));
            memcpy(b, bad[i].magic, 8);
            memcpy(b + 8, &bad[i].version, 4);
            memcpy(b + 12, &bad[i].id, 4);
            memcpy(b + 16, &bad[i].plane, 4);
            memcpy(b + 24, &bad[i].size, 8);
            CHECK(write(tf, b, bad[i].len) == (ssize_t)bad[i].len);
            errno = 0;
            r = drmPrimeFDToHandle(fd, tf, &h);
            if (r == 0 || errno != bad[i].err)
                fprintf(stderr, "surface file %u: r %d errno %d (want %d)\n", i, r, errno, bad[i].err);
            CHECK(r != 0 && errno == bad[i].err && h == 0);
            if (bad[i].len == 32 && bad[i].version == 1 && !memcmp(bad[i].magic, "RD3SURF1", 8))
                CHECK(drm_xnu_surface_last_error() == bad[i].err);
            close(tf);
        }
    }
    /* CS on the (software) DMA ring through libdrm_amdgpu, like RADV's
     * winsys: raw submit with chunks, then the fence query */
    {
        struct amdgpu_bo_alloc_request req = { 64 << 10, 4096, AMDGPU_GEM_DOMAIN_GTT, 0 };
        amdgpu_bo_handle bo;
        amdgpu_va_handle vah;
        amdgpu_context_handle ctx;
        uint64_t va = 0, seq = 0;
        uint32_t *p = NULL, n = 0, expired = 0, so = 0;
        struct drm_amdgpu_cs_chunk ch[2];
        struct drm_amdgpu_cs_chunk_ib ib;
        struct drm_amdgpu_cs_chunk_sem out;
        struct amdgpu_cs_fence fence;

        CHECK(amdgpu_bo_alloc(dev, &req, &bo) == 0);
        CHECK(amdgpu_bo_cpu_map(bo, (void **)&p) == 0 && p);
        CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, 64 << 10, 4096, 0, &va,
                                    &vah, 0) == 0);
        CHECK(amdgpu_bo_va_op(bo, 0, 64 << 10, va, 0, AMDGPU_VA_OP_MAP) == 0);
        memset(p, 0, 64 << 10);
        /* WRITE_LINEAR 2 dwords at +32K */
        p[n++] = 2; p[n++] = (uint32_t)(va + 0x8000); p[n++] = (uint32_t)((va + 0x8000) >> 32);
        p[n++] = 1; p[n++] = 0x11111111; p[n++] = 0x22222222;
        /* CONST_FILL 64 bytes at +48K */
        p[n++] = 11; p[n++] = (uint32_t)(va + 0xC000); p[n++] = (uint32_t)((va + 0xC000) >> 32);
        p[n++] = 0x77777777; p[n++] = 63;
        /* COPY_LINEAR 8 bytes +32K -> +40K */
        p[n++] = 1; p[n++] = 7; p[n++] = 0;
        p[n++] = (uint32_t)(va + 0x8000); p[n++] = (uint32_t)((va + 0x8000) >> 32);
        p[n++] = (uint32_t)(va + 0xA000); p[n++] = (uint32_t)((va + 0xA000) >> 32);
        while (n & 7)
            p[n++] = 0;   /* NOP padding */

        CHECK(amdgpu_cs_ctx_create(dev, &ctx) == 0);
        CHECK(amdgpu_cs_create_syncobj2(dev, 0, &so) == 0);
        memset(&ib, 0, sizeof(ib));
        ib.va_start = va; ib.ib_bytes = n * 4; ib.ip_type = AMDGPU_HW_IP_DMA;
        out.handle = so;
        ch[0].chunk_id = AMDGPU_CHUNK_ID_IB; ch[0].length_dw = sizeof(ib) / 4;
        ch[0].chunk_data = (uint64_t)(uintptr_t)&ib;
        ch[1].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT; ch[1].length_dw = 1;
        ch[1].chunk_data = (uint64_t)(uintptr_t)&out;
        CHECK(amdgpu_cs_submit_raw2(dev, ctx, 0, 2, ch, &seq) == 0 && seq);
        memset(&fence, 0, sizeof(fence));
        fence.context = ctx; fence.ip_type = AMDGPU_HW_IP_DMA; fence.fence = seq;
        CHECK(amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired) == 0 &&
              expired);
        CHECK(amdgpu_cs_syncobj_wait(dev, &so, 1, INT64_MAX, 0, NULL) == 0);
        /* AMDGPU_CHUNK_ID_CP_GFX_SHADOW through amdgpu_cs_submit_raw2: taken
         * with INIT_SHADOW (a ring without shadowing ignores it), refused
         * with an unknown flag (amdgpu_cs_p2_shadow) */
        {
            struct drm_amdgpu_cs_chunk sch[2];
            struct drm_amdgpu_cs_chunk_cp_gfx_shadow sh;
            uint64_t s2 = 0;
            memset(&sh, 0, sizeof(sh));
            sh.shadow_va = va + 0x10000; sh.csa_va = va + 0x20000;
            sh.flags = AMDGPU_CS_CHUNK_CP_GFX_SHADOW_FLAGS_INIT_SHADOW;
            sch[0] = ch[0];
            sch[1].chunk_id = AMDGPU_CHUNK_ID_CP_GFX_SHADOW; sch[1].length_dw = sizeof(sh) / 4;
            sch[1].chunk_data = (uint64_t)(uintptr_t)&sh;
            CHECK(amdgpu_cs_submit_raw2(dev, ctx, 0, 2, sch, &s2) == 0 && s2 > seq);
            sh.flags = 4;
            CHECK(amdgpu_cs_submit_raw2(dev, ctx, 0, 2, sch, &s2) == -EINVAL);
        }
        /* FENCE_TO_HANDLE with fds (libdrm_xnu makes them): a sync_file
         * and an opaque syncobj fd of the CS fence */
        {
            uint32_t sfd = 0, ofd = 0, hs = 0;
            CHECK(amdgpu_cs_fence_to_handle(dev, &fence, AMDGPU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD, &sfd) == 0);
            CHECK(amdgpu_cs_create_syncobj2(dev, 0, &hs) == 0);
            CHECK(amdgpu_cs_syncobj_import_sync_file(dev, hs, (int)sfd) == 0);
            CHECK(amdgpu_cs_syncobj_wait(dev, &hs, 1, INT64_MAX, 0, NULL) == 0);
            CHECK(amdgpu_cs_fence_to_handle(dev, &fence, AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ_FD, &ofd) == 0);
            CHECK(ofd != sfd && amdgpu_cs_import_syncobj(dev, (int)ofd, &hs) == 0);
            CHECK(amdgpu_cs_syncobj_wait(dev, &hs, 1, INT64_MAX, 0, NULL) == 0);
            close((int)sfd); close((int)ofd);
        }
        /* the out fence as a sync_file, imported over another connection
         * and in a forked child (a different process, same device) */
        {
            int sfd = -1, fd2, st = -1;
            uint32_t h2 = 0;
            pid_t pid;
            CHECK(drmSyncobjExportSyncFile(amdgpu_device_get_fd(dev), so, &sfd) == 0 && sfd >= 0);
            fd2 = drm_xnu_open("/dev/dri/renderD128", 0);
            CHECK(fd2 >= 0);
            CHECK(drmSyncobjFDToHandle(fd2, sfd, &h2) == 0 && h2);
            CHECK(drmSyncobjWait(fd2, &h2, 1, 0, 0, NULL) == 0);
            CHECK(drmSyncobjDestroy(fd2, h2) == 0);
            pid = fork();
            if (pid == 0) {
                int cfd = drm_xnu_open("/dev/dri/renderD128", 0);
                uint32_t ch = 0;
                int ok = cfd >= 0 && drmSyncobjCreate(cfd, 0, &ch) == 0 &&
                         drmSyncobjImportSyncFile(cfd, ch, sfd) == 0 &&
                         drmSyncobjWait(cfd, &ch, 1, INT64_MAX, 0, NULL) == 0;
                _exit(ok ? 0 : 1);
            }
            CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
            close(sfd);
            drm_xnu_close(fd2);
        }
        CHECK(p[0x8000 / 4] == 0x11111111 && p[0x8000 / 4 + 1] == 0x22222222);
        CHECK(p[0xA000 / 4] == 0x11111111 && p[0xA000 / 4 + 1] == 0x22222222);
        CHECK(p[0xC000 / 4] == 0x77777777 && p[0xC000 / 4 + 15] == 0x77777777 &&
              p[0xC000 / 4 + 16] == 0);
        /* an IB outside any mapping is refused */
        ib.va_start = va + (128 << 10);
        CHECK(amdgpu_cs_submit_raw2(dev, ctx, 0, 1, ch, &seq) != 0);
        /* GFX is only advertised (rdna3enum), never executed */
        ib.va_start = va; ib.ip_type = AMDGPU_HW_IP_GFX;
        CHECK(amdgpu_cs_submit_raw2(dev, ctx, 0, 1, ch, &seq) != 0);
        CHECK(amdgpu_cs_destroy_syncobj(dev, so) == 0);
        CHECK(amdgpu_cs_ctx_free(ctx) == 0);
        CHECK(amdgpu_bo_va_op(bo, 0, 64 << 10, va, 0, AMDGPU_VA_OP_UNMAP) == 0);
        CHECK(amdgpu_va_range_free(vah) == 0);
        CHECK(amdgpu_bo_cpu_unmap(bo) == 0);
        CHECK(amdgpu_bo_free(bo) == 0);
        printf("libdrm_amdgpu CS: DMA IB of %u dwords ran, fence %llu signaled\n", n,
               (unsigned long long)seq);
    }

    /* contexts and syncobjs */
    {
        amdgpu_context_handle ctx;
        uint32_t so;
        CHECK(amdgpu_cs_ctx_create(dev, &ctx) == 0);
        /* DRM_AMDGPU_SCHED on a "master" fd naming the device's fd: the
         * shim turns that fd into its file id; root only (DRM_MASTER) */
        {
            int mfd = drm_xnu_open("/dev/dri/renderD128", 0);
            int want = geteuid() == 0 ? 0 : -EACCES;
            CHECK(mfd >= 0);
            CHECK(amdgpu_cs_ctx_override_priority(dev, ctx, mfd, AMDGPU_CTX_PRIORITY_HIGH) == want);
            CHECK(amdgpu_cs_ctx_override_priority(dev, ctx, mfd, 7) == (want ? want : -EINVAL));
            CHECK(drm_xnu_close(mfd) == 0);
        }
        CHECK(amdgpu_cs_ctx_free(ctx) == 0);
        CHECK(amdgpu_cs_create_syncobj2(dev, DRM_SYNCOBJ_CREATE_SIGNALED, &so) == 0);
        CHECK(amdgpu_cs_syncobj_wait(dev, &so, 1, 0, 0, NULL) == 0);
        CHECK(amdgpu_cs_destroy_syncobj(dev, so) == 0);
    }

    CHECK(amdgpu_device_deinitialize(dev) == 0);
    /* the orphaned connection is released on the next open */
    fd = drm_xnu_open("/dev/dri/renderD128", 0);
    CHECK(fd >= 0);
    CHECK(drm_xnu_close(fd) == 0);

    /* modifier names (Mesa's debug dumps free() them) */
    {
        char *n0 = drmGetFormatModifierName(0);
        /* AMD_FMT_MOD | TILE_VERSION GFX11 (4) | TILE 64K_R_X (27 & 0x1f) */
        char *na = drmGetFormatModifierName((2ull << 56) | 4 | (27ull << 8));
        CHECK(n0 && !strcmp(n0, "LINEAR"));
        CHECK(na && !strncmp(na, "AMD(TILE_VERSION=4,TILE=27,", 27));
        CHECK(drmGetFormatModifierName(1ull << 56) == NULL);
        free(n0);
        free(na);
    }
    {
        extern uint32_t lb_unmap_refused;
        CHECK(lb_unmap_refused == 2);   /* only the two below: every libdrm unmap named its BO (N47 (b)) */
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_libdrm_amdgpu: all checks passed\n");
    return 0;
}
