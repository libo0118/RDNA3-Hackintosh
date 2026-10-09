/*
 * End-to-end userspace test without a GPU: the unmodified libdrm_amdgpu
 * (third_party/libdrm) + our xf86drm_xnu core + drm_xnu with the loopback
 * backend, which runs uapi/rdna3_drm.c in-process. This is the same call
 * path RADV's winsys takes on macOS, minus the IOKit hop.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
