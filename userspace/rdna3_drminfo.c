/*
 * rdna3_drminfo - Stage 4 acceptance test on a Mac with RDNA3Accel loaded
 * (rdna3lvl>=2): talks the Linux amdgpu UAPI through libdrm_xnu like
 * libdrm_amdgpu would, and exercises BO create / mmap / VA / close.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "drm_xnu.h"
#include "rdna3_drm.h"

#define IOCTL(fd, nr, arg) drm_xnu_ioctl_nr((fd), (nr), &(arg), sizeof(arg))

int main(void)
{
    struct drm_version v;
    struct drm_amdgpu_info in;
    struct drm_amdgpu_info_device di;
    union drm_amdgpu_gem_create c;
    union drm_amdgpu_gem_mmap m;
    struct drm_gem_close cl;
    char name[32] = {0};
    int fd, fails = 0;

    fd = drm_xnu_open(DRM_XNU_RENDER_NODE, 0);
    if (fd < 0) {
        perror("open RDNA3Accel");
        return 1;
    }
    memset(&v, 0, sizeof(v));
    v.name = name;
    v.name_len = sizeof(name) - 1;
    if (IOCTL(fd, RD_NR_VERSION, v)) { perror("DRM_IOCTL_VERSION"); return 1; }
    printf("driver %s %d.%d.%d\n", name, v.version_major, v.version_minor, v.version_patchlevel);

    memset(&in, 0, sizeof(in));
    memset(&di, 0, sizeof(di));
    in.return_pointer = (uint64_t)(uintptr_t)&di;
    in.return_size = sizeof(di);
    in.query = AMDGPU_INFO_DEV_INFO;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in)) { perror("AMDGPU_INFO"); return 1; }
    printf("device 0x%04x rev 0x%02x family %u: %u SE x %u SA, %u CUs, %u RBs, wave%u\n",
           di.device_id, di.pci_rev, di.family, di.num_shader_engines,
           di.num_shader_arrays_per_engine, di.cu_active_number, di.num_rb_pipes,
           di.wave_front_size);
    printf("VA 0x%llx-0x%llx, high 0x%llx-0x%llx\n",
           (unsigned long long)di.virtual_address_offset,
           (unsigned long long)di.virtual_address_max,
           (unsigned long long)di.high_va_offset, (unsigned long long)di.high_va_max);

    for (int dom = AMDGPU_GEM_DOMAIN_GTT; dom <= AMDGPU_GEM_DOMAIN_VRAM; dom <<= 1) {
        memset(&c, 0, sizeof(c));
        c.in.bo_size = 1 << 20;
        c.in.alignment = 4096;
        c.in.domains = dom;
        if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c)) { perror("GEM_CREATE"); fails++; continue; }
        memset(&m, 0, sizeof(m));
        m.in.handle = c.out.handle;
        if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m)) { perror("GEM_MMAP"); fails++; continue; }
        uint32_t *p = drm_xnu_mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                   (off_t)m.out.addr_ptr);
        if (p == MAP_FAILED) { perror("mmap"); fails++; continue; }
        for (unsigned i = 0; i < (1u << 18); i++)
            p[i] = i * 2654435761u;
        unsigned bad = 0;
        for (unsigned i = 0; i < (1u << 18); i++)
            bad += p[i] != i * 2654435761u;
        printf("%s BO handle %u: 1 MiB write/read-back %s\n",
               dom == AMDGPU_GEM_DOMAIN_VRAM ? "VRAM" : "GTT ", c.out.handle,
               bad ? "FAILED" : "ok");
        fails += bad != 0;
        drm_xnu_munmap(p, 1 << 20);
        cl.handle = c.out.handle;
        cl.pad = 0;
        if (IOCTL(fd, RD_NR_GEM_CLOSE, cl)) { perror("GEM_CLOSE"); fails++; }
    }
    drm_xnu_close(fd);
    printf("%s\n", fails ? "SOME CHECKS FAILED" : "Stage 4 UAPI smoke test passed");
    return fails ? 1 : 0;
}
