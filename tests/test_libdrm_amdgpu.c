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
        CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, 1 << 20, 4096, 0, &va,
                                    &vah, 0) == 0 && va);
        CHECK(amdgpu_bo_va_op(bo, 0, 1 << 20, va, 0, AMDGPU_VA_OP_MAP) == 0);
        CHECK(amdgpu_bo_va_op(bo, 0, 1 << 20, va, 0, AMDGPU_VA_OP_UNMAP) == 0);
        CHECK(amdgpu_va_range_free(vah) == 0);
        CHECK(amdgpu_bo_free(bo) == 0);
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
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_libdrm_amdgpu: all checks passed\n");
    return 0;
}
