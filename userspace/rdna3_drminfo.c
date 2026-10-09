/*
 * rdna3_drminfo - Stage 4 acceptance test on a Mac with RDNA3Accel loaded
 * (rdna3lvl>=2): talks the Linux amdgpu UAPI through libdrm_xnu like
 * libdrm_amdgpu would, and exercises BO create / mmap / VA / close.
 *
 *   rdna3_drminfo --dma-test   (rdna3lvl>=7) also submits an SDMA IB from a
 *                              VRAM BO through AMDGPU_CS and checks what the
 *                              GPU wrote: the first userspace job on the card.
 *   rdna3_drminfo --gfx-test   (rdna3lvl>=8) the same with a PM4 WRITE_DATA IB
 *                              on the GFX ring.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include "drm_xnu.h"
#include "rdna3_drm.h"

#define IOCTL(fd, nr, arg) drm_xnu_ioctl_nr((fd), (nr), &(arg), sizeof(arg))

static int job_test(int fd, int gfx)
{
    union drm_amdgpu_gem_create c;
    union drm_amdgpu_gem_mmap m;
    struct drm_amdgpu_gem_va va;
    struct drm_amdgpu_cs_chunk ch;
    struct drm_amdgpu_cs_chunk_ib ib;
    union drm_amdgpu_cs cs;
    union drm_amdgpu_wait_cs w;
    struct timespec ts;
    uint64_t chunk_ptr, gva = 0x100000;
    uint32_t *p, n = 0;
    int bad = 0;

    memset(&c, 0, sizeof(c));
    c.in.bo_size = 64 << 10;
    c.in.alignment = 4096;
    c.in.domains = AMDGPU_GEM_DOMAIN_VRAM;    /* the GPU cannot reach system memory yet */
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c)) { perror("GEM_CREATE"); return 1; }
    memset(&m, 0, sizeof(m));
    m.in.handle = c.out.handle;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m)) { perror("GEM_MMAP"); return 1; }
    p = drm_xnu_mmap(NULL, 64 << 10, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.out.addr_ptr);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    memset(&va, 0, sizeof(va));
    va.handle = c.out.handle;
    va.operation = AMDGPU_VA_OP_MAP;
    va.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
    va.va_address = gva;
    va.map_size = 64 << 10;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va)) { perror("GEM_VA"); return 1; }

    memset(p, 0, 64 << 10);
    if (!gfx) {
        p[n++] = 2;                   /* SDMA WRITE_LINEAR 2 dwords at +32K */
        p[n++] = (uint32_t)(gva + 0x8000); p[n++] = 0; p[n++] = 1;
        p[n++] = 0xC0FFEE01; p[n++] = 0xC0FFEE02;
        p[n++] = 11;                  /* SDMA CONST_FILL 256 bytes at +48K */
        p[n++] = (uint32_t)(gva + 0xC000); p[n++] = 0; p[n++] = 0x5A5A5A5A; p[n++] = 255;
        while (n & 7)
            p[n++] = 0;
    } else {
        /* PM4 WRITE_DATA (DST_SEL 5 = memory, WR_CONFIRM): 2 dwords at +32K,
         * then 64 dwords of 0x5A5A5A5A at +48K */
        p[n++] = (3u << 30) | (0x37u << 8) | ((3u + 2 - 1) << 16);
        p[n++] = (5u << 8) | (1u << 20);
        p[n++] = (uint32_t)(gva + 0x8000); p[n++] = 0;
        p[n++] = 0xC0FFEE01; p[n++] = 0xC0FFEE02;
        p[n++] = (3u << 30) | (0x37u << 8) | ((3u + 64 - 1) << 16);
        p[n++] = (5u << 8) | (1u << 20);
        p[n++] = (uint32_t)(gva + 0xC000); p[n++] = 0;
        for (unsigned i = 0; i < 64; i++)
            p[n++] = 0x5A5A5A5A;
    }

    memset(&ib, 0, sizeof(ib));
    ib.va_start = gva;
    ib.ib_bytes = n * 4;
    ib.ip_type = gfx ? AMDGPU_HW_IP_GFX : AMDGPU_HW_IP_DMA;
    ch.chunk_id = AMDGPU_CHUNK_ID_IB;
    ch.length_dw = sizeof(ib) / 4;
    ch.chunk_data = (uint64_t)(uintptr_t)&ib;
    chunk_ptr = (uint64_t)(uintptr_t)&ch;
    memset(&cs, 0, sizeof(cs));
    cs.in.num_chunks = 1;
    cs.in.chunks = (uint64_t)(uintptr_t)&chunk_ptr;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_CS), cs)) {
        perror(gfx ? "AMDGPU_CS (GFX needs rdna3lvl>=8)" : "AMDGPU_CS (DMA needs rdna3lvl>=7)");
        return 1;
    }
    printf("%s job submitted, fence %llu\n", gfx ? "GFX" : "DMA", (unsigned long long)cs.out.handle);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    memset(&w, 0, sizeof(w));
    w.in.handle = cs.out.handle;
    w.in.ip_type = ib.ip_type;
    w.in.timeout = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + 2000000000ull;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w)) { perror("WAIT_CS"); return 1; }
    if (w.out.status) { printf("%s job did NOT complete within 2 s\n", gfx ? "GFX" : "DMA"); return 1; }

    bad += p[0x8000 / 4] != 0xC0FFEE01 || p[0x8000 / 4 + 1] != 0xC0FFEE02;
    for (unsigned i = 0; i < 64; i++)
        bad += p[0xC000 / 4 + i] != 0x5A5A5A5A;
    bad += p[0xC000 / 4 + 64] != 0;
    printf("%s job results %s\n", gfx ? "GFX" : "DMA",
           bad ? "WRONG" : (gfx ? "verified (PM4 WRITE_DATA)" : "verified (WRITE_LINEAR + CONST_FILL)"));
    if (bad)
        printf("  +0x8000: %08x %08x (want c0ffee01 c0ffee02), +0xC000: %08x .. +0xC0FC: %08x "
               "(want 5a5a5a5a), +0xC100: %08x (want 0)\n", p[0x8000 / 4], p[0x8000 / 4 + 1],
               p[0xC000 / 4], p[0xC000 / 4 + 63], p[0xC000 / 4 + 64]);
    drm_xnu_munmap(p, 64 << 10);
    return bad != 0;
}

int main(int argc, char **argv)
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
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--dma-test"))
            fails += job_test(fd, 0);
        else if (!strcmp(argv[a], "--gfx-test"))
            fails += job_test(fd, 1);
    }
    drm_xnu_close(fd);
    printf("%s\n", fails ? "SOME CHECKS FAILED" : "Stage 4 UAPI smoke test passed");
    return fails ? 1 : 0;
}
