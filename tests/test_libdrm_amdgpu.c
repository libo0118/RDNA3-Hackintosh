/*
 * End-to-end userspace test without a GPU: the unmodified libdrm_amdgpu
 * (third_party/libdrm) + our xf86drm_xnu core + drm_xnu with the loopback
 * backend, which runs uapi/rdna3_drm.c in-process. This is the same call
 * path RADV's winsys takes on macOS, minus the IOKit hop.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
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
    CHECK(drm_xnu_close(fd) == 0);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_libdrm_amdgpu: all checks passed\n");
    return 0;
}
