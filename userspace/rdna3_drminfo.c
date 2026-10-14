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
 *   rdna3_drminfo --hang-test  (rdna3lvl>=7) submits a DMA job (--hang-test-gfx:
 *                              a GFX job, rdna3lvl>=8) that never finishes (POLL_REGMEM / WAIT_REG_MEM on
 *                              a value that never comes): the kernel must
 *                              notice within its 10 s cap, recover (SDMA soft
 *                              reset / GFX soft recovery or reset) and run the
 *                              next DMA and GFX jobs correctly. Run it last.
 *   rdna3_drminfo --userq-test  (rdna3userq=1) a compute user queue through the
 *                              MES: the client writes its ring and wptr in GTT
 *                              BOs, rings its own doorbell (a doorbell BO in
 *                              BAR2) and waits for the fence USERQ_SIGNAL put
 *                              into a syncobj; then USERQ_WAIT and USERQ_FREE.
 *   rdna3_drminfo --vcn-test   (rdna3vcn=1) the video IPs: HW_IP_INFO / COUNT of
 *                              VCN_DEC / VCN_ENC / VCN_JPEG, FW_VCN, VIDEO_CAPS,
 *                              then an encode session create + close and a decode
 *                              create + destroy message on the unified ring and a
 *                              JPEG register write, each waited for.
 *   rdna3_drminfo --scanout DIR  (root) what each display pipe scans out right
 *                              now: HUBP state and a 320-wide picture of it,
 *                              DIR/scanout-pipe<N>.ppm - the report's proof of
 *                              what reaches each panel. Nothing else runs.
 *   rdna3_drminfo --scanout-hires DIR  the same, 1280 wide (an app's
 *                              stale regions and text, card run 54)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include "drm_xnu.h"
#include "rdna3_drm.h"

#define IOCTL(fd, nr, arg) drm_xnu_ioctl_nr((fd), (nr), &(arg), sizeof(arg))

static int job_test(int fd, int gfx, int hang)
{
    union drm_amdgpu_gem_create c;
    union drm_amdgpu_gem_mmap m;
    struct drm_amdgpu_gem_va va;
    struct drm_amdgpu_cs_chunk ch;
    struct drm_amdgpu_cs_chunk_ib ib;
    union drm_amdgpu_cs cs;
    union drm_amdgpu_wait_cs w;
    struct timespec ts;
    /* a VA of its own per job in this process: '--dma-test --gfx-test'
     * runs two, and the first BO stays mapped (amdgpu refuses a second map
     * over it with EINVAL: the after-the-wake GFX test of 2026-10-12) */
    static uint64_t next_gva = 0x100000;
    uint64_t chunk_ptr, gva = next_gva;
    uint32_t *p, n = 0;
    int bad = 0;

    next_gva += 64 << 10;

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
    if (hang && !gfx) {
        /* SDMA POLL_REGMEM (sdma_v6_0_ring_emit_wait_reg_mem): memory, func
         * 3 (equal), +32K == 0xDEADBEEF, retry count 0xfff = forever */
        p[n++] = 8u | (1u << 31) | (3u << 28);
        p[n++] = (uint32_t)(gva + 0x8000); p[n++] = 0;
        p[n++] = 0xDEADBEEF; p[n++] = 0xffffffff;
        p[n++] = 10u | (0xfffu << 16);
        while (n & 7)
            p[n++] = 0;
    } else if (hang) {
        /* PM4 WAIT_REG_MEM (gfx_v11_0_wait_reg_mem): memory, equal, ME,
         * +32K == 0xDEADBEEF: the CP waits forever */
        p[n++] = (3u << 30) | (0x3Cu << 8) | (5u << 16);
        p[n++] = 3u | (1u << 4);
        p[n++] = (uint32_t)(gva + 0x8000); p[n++] = 0;
        p[n++] = 0xDEADBEEF; p[n++] = 0xffffffff;
        p[n++] = 4;
    } else if (!gfx) {
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
    w.in.timeout = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec +
                   (hang ? 30000000000ull : 2000000000ull);
    if (hang) {
        /* the kernel caps the wait at 10 s and recovers; any return within
         * the 30 s is the pass, the job's own result does not matter */
        struct timespec t1;
        int r = IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double s = (double)(t1.tv_sec - ts.tv_sec) + (double)(t1.tv_nsec - ts.tv_nsec) / 1e9;
        printf("%s hang job: WAIT_CS returned %d (status %llu) after %.1f s -> %s\n",
               gfx ? "GFX" : "DMA", r, (unsigned long long)w.out.status, s,
               s < 29.0 ? "recovered (kernel noticed the hang)" : "NOT recovered");
        drm_xnu_munmap(p, 64 << 10);
        return s < 29.0 ? 0 : 1;
    }
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

/* a BO of 'dom', mapped for the CPU and at 'gva' in the GPU VM (a doorbell
 * BO only for the CPU: the GPU never reaches doorbells through a VM) */
static void *uq_bo(int fd, uint32_t size, uint32_t dom, uint64_t gva, uint32_t *handle)
{
    union drm_amdgpu_gem_create c;
    union drm_amdgpu_gem_mmap m;
    struct drm_amdgpu_gem_va va;
    void *p;
    memset(&c, 0, sizeof(c));
    c.in.bo_size = size;
    c.in.alignment = 4096;
    c.in.domains = dom;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c)) { perror("GEM_CREATE"); return NULL; }
    *handle = c.out.handle;
    memset(&m, 0, sizeof(m));
    m.in.handle = c.out.handle;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m)) { perror("GEM_MMAP"); return NULL; }
    p = drm_xnu_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.out.addr_ptr);
    if (p == MAP_FAILED) { perror("mmap"); return NULL; }
    memset(p, 0, dom == AMDGPU_GEM_DOMAIN_DOORBELL ? 0 : size);
    if (!gva)
        return p;
    memset(&va, 0, sizeof(va));
    va.handle = c.out.handle;
    va.operation = AMDGPU_VA_OP_MAP;
    va.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
    va.va_address = gva;
    va.map_size = size;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va)) { perror("GEM_VA"); return NULL; }
    return p;
}

/* --userq-test: DRM_AMDGPU_USERQ / _SIGNAL / _WAIT with a compute queue */
/* the last GPUVM fault of this client's VM (AMDGPU_INFO_GPUVM_FAULT), after a
 * job that did not finish: a fence or an IB behind an unmapped VA shows
 * here (card run 2026-10-12: user queue and VCN fences never came back) */
static void print_vm_fault(int fd)
{
    struct drm_amdgpu_info in;
    struct drm_amdgpu_info_gpuvm_fault vf;
    memset(&in, 0, sizeof(in));
    memset(&vf, 0, sizeof(vf));
    in.return_pointer = (uint64_t)(uintptr_t)&vf;
    in.return_size = sizeof(vf);
    in.query = AMDGPU_INFO_GPUVM_FAULT;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in))
        printf("GPUVM fault: query failed\n");
    else if (!vf.status && !vf.addr)
        printf("GPUVM fault: none recorded\n");
    else
        printf("GPUVM fault: addr 0x%llx status 0x%08x vmhub %u (%s)\n", (unsigned long long)vf.addr, vf.status,
               vf.vmhub, vf.vmhub < 8 ? "GFXHUB" : "MMHUB");
}

static int userq_test(int fd)
{
    const uint64_t gva = 0x400000;   /* ring, wptr/rptr, EOP, dst: a page each */
    struct drm_amdgpu_info in;
    struct drm_amdgpu_info_device di;
    struct { uint32_t shadow_size, shadow_alignment, csa_size, csa_alignment; } meta;   /* uq metadata, gfx */
    struct drm_amdgpu_userq_mqd_compute_gfx11 mqd;
    struct drm_amdgpu_userq_signal sg;
    struct drm_amdgpu_userq_wait uw;
    struct drm_syncobj_create sc;
    struct drm_syncobj_wait sw;
    union drm_amdgpu_userq uq;
    struct timespec ts;
    uint32_t hdb, hring, hwp, heop, hdst, n = 0;
    volatile uint64_t *db, *wp;
    volatile uint32_t *dst;
    uint32_t *ring;
    int bad = 0, r;

    memset(&in, 0, sizeof(in));
    memset(&di, 0, sizeof(di));
    in.return_pointer = (uint64_t)(uintptr_t)&di;
    in.return_size = sizeof(di);
    in.query = AMDGPU_INFO_DEV_INFO;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in)) { perror("AMDGPU_INFO"); return 1; }
    memset(&meta, 0, sizeof(meta));
    in.return_pointer = (uint64_t)(uintptr_t)&meta;
    in.return_size = sizeof(meta);
    in.query = AMDGPU_INFO_UQ_FW_AREAS;
    in.query_hw_ip.type = AMDGPU_HW_IP_GFX;
    r = IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in);
    printf("user queues: IP mask 0x%x, gfx shadow %u (align %u), CSA %u (align %u)%s\n", di.userq_ip_mask,
           meta.shadow_size, meta.shadow_alignment, meta.csa_size, meta.csa_alignment,
           r ? " - UQ_FW_AREAS FAILED" : "");
    if (!(di.userq_ip_mask & (1u << AMDGPU_HW_IP_COMPUTE))) {
        printf("USERQ TEST: no compute user queues (boot-arg rdna3userq=1, the MES and its firmware >= 120 "
               "needed; see RDNA3,UserQueues)\n");
        return 1;
    }
    db = uq_bo(fd, 4096, AMDGPU_GEM_DOMAIN_DOORBELL, 0, &hdb);
    ring = uq_bo(fd, 4096, AMDGPU_GEM_DOMAIN_GTT, gva, &hring);
    wp = uq_bo(fd, 4096, AMDGPU_GEM_DOMAIN_GTT, gva + 0x1000, &hwp);   /* wptr @0, rptr @8 */
    if (!uq_bo(fd, 4096, AMDGPU_GEM_DOMAIN_GTT, gva + 0x2000, &heop))
        return 1;
    dst = uq_bo(fd, 4096, AMDGPU_GEM_DOMAIN_GTT, gva + 0x3000, &hdst);
    if (!db || !ring || !wp || !dst)
        return 1;
    memset(&mqd, 0, sizeof(mqd));
    mqd.eop_va = gva + 0x2000;
    memset(&uq, 0, sizeof(uq));
    uq.in.op = AMDGPU_USERQ_OP_CREATE;
    uq.in.ip_type = AMDGPU_HW_IP_COMPUTE;
    uq.in.doorbell_handle = hdb;
    uq.in.doorbell_offset = 0;
    uq.in.queue_va = gva;
    uq.in.queue_size = 4096;
    uq.in.wptr_va = gva + 0x1000;
    uq.in.rptr_va = gva + 0x1008;
    uq.in.mqd = (uint64_t)(uintptr_t)&mqd;
    uq.in.mqd_size = sizeof(mqd);
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), uq)) { perror("USERQ CREATE"); return 1; }
    printf("compute user queue %u created\n", uq.out.queue_id);

    /* WRITE_DATA of two dwords, then a RELEASE_MEM that interrupts: the
     * firmware writes the queue's rptr to its fence slot */
    ring[n++] = (3u << 30) | (0x37u << 8) | ((3u + 2 - 1) << 16);
    ring[n++] = (5u << 8) | (1u << 20);
    ring[n++] = (uint32_t)(gva + 0x3000); ring[n++] = 0;
    ring[n++] = 0xC0FFEE11; ring[n++] = 0xC0FFEE12;
    ring[n++] = (3u << 30) | (0x49u << 8) | (6u << 16);
    /* CACHE_FLUSH_AND_INV_TS_EVENT, EVENT_INDEX 5, with the GL2 / GLM
     * write-back gfx_v11_0_ring_emit_fence asks for (card run 33: without
     * it the queue ran but its writes stayed in the GPU caches) */
    ring[n++] = (1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | 0x14 | (5u << 8);
    ring[n++] = (1u << 29) | (2u << 24);     /* DATA_SEL 1, INT_SEL 2 */
    ring[n++] = (uint32_t)(gva + 0x3010); ring[n++] = 0;
    ring[n++] = 0xC0FFEE13; ring[n++] = 0; ring[n++] = 0;
    wp[0] = n;
    __sync_synchronize();
    memset(&sc, 0, sizeof(sc));
    if (IOCTL(fd, RD_NR_SYNCOBJ_CREATE, sc)) { perror("SYNCOBJ_CREATE"); return 1; }
    memset(&sg, 0, sizeof(sg));
    sg.queue_id = uq.out.queue_id;
    sg.syncobj_handles = (uint64_t)(uintptr_t)&sc.handle;
    sg.num_syncobj_handles = 1;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL), sg)) { perror("USERQ_SIGNAL"); return 1; }
    db[0] = n;                               /* the doorbell: 64-bit wptr */
    __sync_synchronize();

    clock_gettime(CLOCK_MONOTONIC, &ts);
    memset(&sw, 0, sizeof(sw));
    sw.handles = (uint64_t)(uintptr_t)&sc.handle;
    sw.count_handles = 1;
    sw.timeout_nsec = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + 2000000000ll;
    r = IOCTL(fd, RD_NR_SYNCOBJ_WAIT, sw);
    bad += dst[0] != 0xC0FFEE11 || dst[1] != 0xC0FFEE12 || dst[4] != 0xC0FFEE13;
    printf("user queue fence %s; dst %08x %08x .. %08x (want c0ffee11 c0ffee12 .. c0ffee13), rptr %llu of %u\n",
           r ? "did NOT signal within 2 s" : "signaled", dst[0], dst[1], dst[4],
           (unsigned long long)wp[1], n);
    bad += r != 0;
    memset(&uw, 0, sizeof(uw));
    uw.waitq_id = uq.out.queue_id;
    uw.syncobj_handles = (uint64_t)(uintptr_t)&sc.handle;
    uw.num_syncobj_handles = 1;
    r = IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), uw);
    printf("USERQ_WAIT: %d, %u fence(s)\n", r, uw.num_fences);
    bad += r != 0;
    memset(&uq, 0, sizeof(uq));
    uq.in.op = AMDGPU_USERQ_OP_FREE;
    uq.in.queue_id = sg.queue_id;
    r = IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), uq);
    printf("USERQ FREE: %d\n", r);
    bad += r != 0;
    if (bad)
        print_vm_fault(fd);
    printf("USERQ TEST: %s\n", bad ? "FAILED" : "PASSED (a compute user queue ran through the MES)");
    return bad != 0;
}

/* one pipe's picture, all rows; returns the number of rows read */
static uint32_t scanout_grab(int fd, uint32_t pipe, struct rd_xnu_scanout *info, uint8_t *img,
                             size_t cap, uint32_t flags)
{
    static struct rd_xnu_scanout s;
    uint32_t row = 0;
    for (;;) {
        memset(&s, 0, sizeof(s));
        s.pipe = pipe;
        s.row0 = row;
        s.flags = (row ? 0 : flags) | (flags & RD_XNU_SCANOUT_HIRES);
        if (IOCTL(fd, RD_NR_XNU_SCANOUT, s))
            return row ? row : (uint32_t)-1;
        if (row == 0)
            *info = s;
        if (!s.nrows || (size_t)(row + s.nrows) * s.out_w * 3 > cap)
            return row;
        memcpy(img + (size_t)row * s.out_w * 3, s.rgb, (size_t)s.nrows * s.out_w * 3);
        row += s.nrows;
        if (row >= s.out_h)
            return row;
    }
}

static int scanout_dump(int fd, const char *dir, uint32_t hires)
{
    enum { CAP = RD_XNU_SCANOUT_HIRES_W * RD_XNU_SCANOUT_HIRES_W * 4 * 3 };
    uint8_t *img = malloc(CAP), *img2 = malloc(CAP);
    int lit = 0;
    if (!img || !img2)
        return 1;
    for (uint32_t p = 0; p < 4; p++) {
        struct rd_xnu_scanout s, s2 = {0};
        /* the sticky underflow is read, then cleared: the second look 2 s
         * later tells whether it keeps coming (bandwidth) or was a switch */
        uint32_t rows = scanout_grab(fd, p, &s, img, CAP, RD_XNU_SCANOUT_CLEAR_UNDERFLOW | hires), i, n, nonblack = 0,
                 diff = 0;
        uint64_t sum[3] = {0, 0, 0};
        if (rows == (uint32_t)-1) {
            perror("XNU_SCANOUT");
            free(img); free(img2);
            return 1;
        }
        printf("pipe %u: %s%s otg %u %ux%u pitch %u fmt %u sw %u dcc %u address 0x%llx in use 0x%llx%s\n",
               p, s.powered ? "" : "power-gated ", s.lit ? "LIT" : s.blanked ? "BLANKED" : "off", s.otg,
               s.width, s.height,
               s.pitch_px, s.format, s.sw_mode, s.dcc, (unsigned long long)s.address,
               (unsigned long long)s.inuse, s.flip_pending ? " flip pending" : "");
        if (s.underflow)
            printf("  UNDERFLOW status %u: the pipe ran out of data (bandwidth / watermarks)\n", s.underflow);
        if (s.crc_valid)
            printf("  OTG CRC %04x/%04x/%04x (what leaves the pipe)\n", s.crc_rg & 0xffff, s.crc_rg >> 16, s.crc_b);
        if (s.powered)
            printf("  DET %u x 64 KiB (compbuf %u), DISPCLK %u kHz, DPPCLK %u kHz (pipe), DSCL mode %u LB 0x%08x "
                   "mem pwr 0x%08x, DLG after scaler %u/%u req %u line %u\n",
                   s.det_segs, s.compbuf_segs, s.dispclk_khz, s.dppclk_khz, s.dscl_mode, s.lb_memory_ctrl,
                   s.dscl_mem_pwr, (s.dlg_after_scaler >> 16) & 7, s.dlg_after_scaler & 0x1fff, s.dlg_req_delivery,
                   s.dlg_line_delivery);
        if (!s.lit)
            continue;
        lit++;
        if (!rows || rows < s.out_h) {
            printf("  picture: not readable (%u of %u rows; tiled, DCC or outside the BAR)\n", rows, s.out_h);
            continue;
        }
        n = s.out_w * rows;
        for (i = 0; i < n; i++) {
            const uint8_t *px = img + (size_t)i * 3;
            nonblack += (px[0] | px[1] | px[2]) > 8;
            sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
        }
        /* a second look 2 s later: is something drawing? */
        sleep(2);
        if (scanout_grab(fd, p, &s2, img2, CAP, hires) == rows)
            for (i = 0; i < n * 3; i++)
                diff += img[i] != img2[i];
        printf("  picture %ux%u: %u%% not black, mean rgb %u %u %u, %u bytes changed in 2 s%s\n",
               s.out_w, rows, (unsigned)((uint64_t)nonblack * 100 / n), (unsigned)(sum[0] / n),
               (unsigned)(sum[1] / n), (unsigned)(sum[2] / n), diff,
               s2.inuse != s.inuse ? " (flipped meanwhile)" : "");
        if (s2.underflow)
            printf("  UNDERFLOW again within 2 s of clearing it (status %u): it keeps running out of data\n",
                   s2.underflow);
        else if (s.underflow)
            printf("  no underflow in the 2 s after clearing it: it happened before (a switch)\n");
        if (dir) {
            char path[1024];
            FILE *f;
            snprintf(path, sizeof(path), "%s/scanout-pipe%u.ppm", dir, p);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", s.out_w, rows);
                fwrite(img, 1, (size_t)n * 3, f);
                fclose(f);
                printf("  -> %s\n", path);
            } else {
                perror(path);
            }
        }
    }
    printf("%d pipe(s) lit\n", lit);
    free(img); free(img2);
    return 0;
}

/* --vcn-test: the video IPs the way Mesa sees them (HW_IP_INFO / COUNT,
 * FW_VCN, VIDEO_CAPS), then one job per ring: an encode session create +
 * close and a decode create + destroy message on the unified ring
 * (amdgpu_vcn_unified_ring_test_ib's IBs, from a client BO in its VM) and
 * a JPEG IB that writes the pitch register; each must signal its fence */
static const char *const vcn_codec_names[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_COUNT] = {
    "MPEG2", "MPEG4", "VC1", "H264", "HEVC", "JPEG", "VP9", "AV1" };

static int vcn_info(int fd, uint32_t query, uint32_t type, void *out, uint32_t size)
{
    struct drm_amdgpu_info in;
    memset(&in, 0, sizeof(in));
    in.return_pointer = (uint64_t)(uintptr_t)out;
    in.return_size = size;
    in.query = query;
    if (query == AMDGPU_INFO_VIDEO_CAPS)
        in.video_cap.type = type;
    else if (query == AMDGPU_INFO_FW_VERSION)
        in.query_fw.fw_type = type;
    else
        in.query_hw_ip.type = type;
    return IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in);
}

static int vcn_job(int fd, uint32_t ctx, uint32_t ip, uint64_t va, uint32_t ndw, const char *what)
{
    struct drm_amdgpu_cs_chunk ch;
    struct drm_amdgpu_cs_chunk_ib ib;
    union drm_amdgpu_cs cs;
    union drm_amdgpu_wait_cs w;
    struct timespec ts;
    uint64_t chunk_ptr;

    memset(&ib, 0, sizeof(ib));
    ib.va_start = va;
    ib.ib_bytes = ndw * 4;
    ib.ip_type = ip;
    ch.chunk_id = AMDGPU_CHUNK_ID_IB;
    ch.length_dw = sizeof(ib) / 4;
    ch.chunk_data = (uint64_t)(uintptr_t)&ib;
    chunk_ptr = (uint64_t)(uintptr_t)&ch;
    memset(&cs, 0, sizeof(cs));
    cs.in.ctx_id = ctx;
    cs.in.num_chunks = 1;
    cs.in.chunks = (uint64_t)(uintptr_t)&chunk_ptr;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_CS), cs)) {
        printf("  %s: AMDGPU_CS failed\n", what);
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    memset(&w, 0, sizeof(w));
    w.in.handle = cs.out.handle;
    w.in.ip_type = ip;
    w.in.ctx_id = ctx;
    w.in.timeout = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + 2000000000ull;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) || w.out.status) {
        printf("  %s: fence %llu did NOT signal within 2 s\n", what, (unsigned long long)cs.out.handle);
        return 1;
    }
    printf("  %s: fence %llu signalled\n", what, (unsigned long long)cs.out.handle);
    return 0;
}

/* amdgpu_vcn_unified_ring_ib_header + _checksum around 'n' dwords at p+8 */
static uint32_t vcn_unified(uint32_t *p, uint32_t pack, int enc)
{
    uint32_t i, sum = 0;
    p[0] = 0x10; p[1] = 0x30000002; p[3] = pack;
    p[4] = 0x10; p[5] = 0x30000001; p[6] = enc ? 2 : 3; p[7] = pack * 4;
    for (i = 0; i < pack; i++)
        sum += p[4 + i];
    p[2] = sum;
    return 8;
}

static int vcn_test(int fd)
{
    struct drm_amdgpu_info_hw_ip ip;
    struct drm_amdgpu_info_video_caps caps;
    struct drm_amdgpu_info_firmware fw;
    union drm_amdgpu_gem_create c;
    union drm_amdgpu_gem_mmap m;
    struct drm_amdgpu_gem_va va;
    union drm_amdgpu_ctx cx;
    static const char *const ipn[] = { "VCN_DEC", "VCN_ENC", "VCN_JPEG" };
    const uint32_t ips[] = { AMDGPU_HW_IP_VCN_DEC, AMDGPU_HW_IP_VCN_ENC, AMDGPU_HW_IP_VCN_JPEG };
    uint64_t gva = 0x200000;
    uint32_t *p, n, i, k, count, enc_rings = 0, jpeg_rings = 0;
    int bad = 0;

    printf("VCN test:\n");
    for (i = 0; i < 3; i++) {
        memset(&ip, 0, sizeof(ip));
        count = 0;
        if (vcn_info(fd, AMDGPU_INFO_HW_IP_INFO, ips[i], &ip, sizeof(ip)) ||
            vcn_info(fd, AMDGPU_INFO_HW_IP_COUNT, ips[i], &count, 4)) {
            printf("  %s: INFO failed\n", ipn[i]);
            return 1;
        }
        printf("  %s: IP %u.%u (discovery 0x%06x), rings 0x%x, %u instance(s), IB align %u/%u\n", ipn[i],
               ip.hw_ip_version_major, ip.hw_ip_version_minor, ip.ip_discovery_version, ip.available_rings, count,
               ip.ib_start_alignment, ip.ib_size_alignment);
        if (ips[i] == AMDGPU_HW_IP_VCN_ENC)
            enc_rings = ip.available_rings;
        if (ips[i] == AMDGPU_HW_IP_VCN_JPEG)
            jpeg_rings = ip.available_rings;
    }
    memset(&fw, 0, sizeof(fw));
    if (!vcn_info(fd, AMDGPU_INFO_FW_VERSION, AMDGPU_INFO_FW_VCN, &fw, sizeof(fw)))
        printf("  firmware VCN 0x%08x (ENC %u.%u DEC %u Revision %u)\n", fw.ver, (fw.ver >> 20) & 0xf,
               (fw.ver >> 12) & 0xff, (fw.ver >> 24) & 0xf, fw.ver & 0xfff);
    for (k = 0; k < 2; k++) {
        memset(&caps, 0, sizeof(caps));
        if (vcn_info(fd, AMDGPU_INFO_VIDEO_CAPS, k ? AMDGPU_INFO_VIDEO_CAPS_ENCODE : AMDGPU_INFO_VIDEO_CAPS_DECODE,
                     &caps, sizeof(caps))) {
            printf("  VIDEO_CAPS %s failed\n", k ? "encode" : "decode");
            bad++;
            continue;
        }
        printf("  VIDEO_CAPS %s:", k ? "encode" : "decode");
        for (i = 0; i < AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_COUNT; i++)
            if (caps.codec_info[i].valid)
                printf(" %s %ux%u L%u", vcn_codec_names[i], caps.codec_info[i].max_width,
                       caps.codec_info[i].max_height, caps.codec_info[i].max_level);
        printf("\n");
    }
    if (!enc_rings && !jpeg_rings) {
        printf("VCN TEST: FAILED (no VCN_ENC / VCN_JPEG ring: boot with rdna3vcn=1)\n");
        return 1;
    }
    /* a JPEG ring alone means the kernel's VCN IB tests failed: the test
     * goes on with JPEG but cannot pass */
    if (!enc_rings) {
        printf("  VCN_ENC: no ring (the kernel's VCN IB tests failed: see the VCN lines of the log)\n");
        bad++;
    }
    if (!jpeg_rings) {
        printf("  VCN_JPEG: no ring\n");
        bad++;
    }

    /* the client's 64 KiB: IBs at 0, 4K, 8K, 12K; encode session buffer at
     * 16K (128 KiB in amdgpu, the firmware only needs it mapped); decode
     * message at 48K */
    memset(&c, 0, sizeof(c));
    c.in.bo_size = 256 << 10;
    c.in.alignment = 4096;
    c.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c)) { perror("GEM_CREATE"); return 1; }
    memset(&m, 0, sizeof(m));
    m.in.handle = c.out.handle;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m)) { perror("GEM_MMAP"); return 1; }
    p = drm_xnu_mmap(NULL, 256 << 10, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.out.addr_ptr);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    memset(&va, 0, sizeof(va));
    va.handle = c.out.handle;
    va.operation = AMDGPU_VA_OP_MAP;
    va.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
    va.va_address = gva;
    va.map_size = 256 << 10;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va)) { perror("GEM_VA"); return 1; }
    memset(p, 0, 256 << 10);
    memset(&cx, 0, sizeof(cx));
    cx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    if (IOCTL(fd, RD_NR_AMDGPU(DRM_AMDGPU_CTX), cx)) { perror("CTX"); return 1; }

    if (enc_rings) {
        /* amdgpu_vcn_enc_get_create_msg / _destroy_msg: session info, task
         * info, op initialize / close session */
        for (k = 0; k < 2; k++) {
            uint32_t *q = p + k * 1024;
            uint64_t sbuf = gva + 0x4000;
            n = vcn_unified(q, 0x11, 1);
            q[n++] = 0x18; q[n++] = 0x1; q[n++] = 1; q[n++] = (uint32_t)(sbuf >> 32); q[n++] = (uint32_t)sbuf;
            q[n++] = 0;
            q[n++] = 0x14; q[n++] = 0x2; q[n++] = 0x1c; q[n++] = 0; q[n++] = 0;
            q[n++] = 0x8; q[n++] = k ? 0x08000002 : 0x08000001;
            vcn_unified(q, 0x11, 1);   /* the checksum over the packages written above */
            bad += vcn_job(fd, cx.out.alloc.ctx_id, AMDGPU_HW_IP_VCN_ENC, gva + k * 4096, 24,
                           k ? "encode session close" : "encode session create");
        }
        /* amdgpu_vcn_dec_get_create_msg / _destroy_msg through the decode
         * buffer (amdgpu_vcn_dec_sw_send_msg) */
        for (k = 0; k < 2; k++) {
            uint32_t *q = p + (2 + k) * 1024, *msg = p + 0xC000 / 4;
            uint64_t mva = gva + 0xC000;
            memset(msg, 0, 4096);
            msg[0] = 0x28; msg[1] = k ? 0x18 : 0x38; msg[2] = k ? 0 : 1; msg[3] = k ? 2 : 0; msg[4] = 1;
            if (!k) {
                msg[6] = 1; msg[7] = 0x28; msg[8] = 0x10; msg[10] = 0x7; msg[12] = 0x780; msg[13] = 0x440;
            }
            n = 8;
            q[n++] = 33 * 4 + 8; q[n++] = 1;        /* AMDGPU_VCN_IB_FLAG_DECODE_BUFFER */
            q[n++] = 1;                              /* valid_buf_flag: MSG_BUFFER */
            q[n++] = (uint32_t)(mva >> 32); q[n++] = (uint32_t)mva;
            n += 30;
            vcn_unified(q, 33 + 4 + 2, 0);
            bad += vcn_job(fd, cx.out.alloc.ctx_id, AMDGPU_HW_IP_VCN_ENC, gva + (2 + k) * 4096, 72,
                           k ? "decode destroy message" : "decode create message (H264 1920x1088)");
        }
    }
    if (jpeg_rings) {
        /* amdgpu_jpeg_dec_set_reg from userspace: PACKETJ TYPE0 to the pitch
         * register, then NOPs (jpeg_v2_dec_ring_parse_cs accepts both) */
        uint32_t *q = p + 4 * 1024;
        q[0] = 0x401f;          /* PACKETJ(regUVD_JPEG_PITCH_INTERNAL_OFFSET, 0, 0, TYPE0) */
        q[1] = 0xDEADBEEF;
        for (i = 2; i < 16; i += 2) {
            q[i] = 0x60000000;  /* PACKETJ(0, 0, 0, TYPE6): NOP */
            q[i + 1] = 0;
        }
        bad += vcn_job(fd, cx.out.alloc.ctx_id, AMDGPU_HW_IP_VCN_JPEG, gva + 4 * 4096, 16, "JPEG pitch write");
    }
    drm_xnu_munmap(p, 256 << 10);
    if (bad)
        print_vm_fault(fd);
    printf("VCN TEST: %s\n", bad ? "FAILED" : "PASSED (video rings run jobs from a client VM)");
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
    if (argc > 1 && (!strcmp(argv[1], "--scanout") || !strcmp(argv[1], "--scanout-hires"))) {
        fails = scanout_dump(fd, argc > 2 ? argv[2] : NULL,
                             strcmp(argv[1], "--scanout") ? RD_XNU_SCANOUT_HIRES : 0);
        drm_xnu_close(fd);
        return fails;
    }

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
            fails += job_test(fd, 0, 0);
        else if (!strcmp(argv[a], "--gfx-test"))
            fails += job_test(fd, 1, 0);
        else if (!strcmp(argv[a], "--userq-test"))
            fails += userq_test(fd);
        else if (!strcmp(argv[a], "--vcn-test"))
            fails += vcn_test(fd);
        else if (!strcmp(argv[a], "--hang-test") || !strcmp(argv[a], "--hang-test-gfx")) {
            /* each hang, then the same engine must work again. The hung
             * job's client is guilty from then on (amdgpu_cs_submit returns
             * -ECANCELED for it, as for a guilty amdgpu context), so every
             * job runs on its own connection, like a new Vulkan device
             * after VK_ERROR_DEVICE_LOST */
            /* --hang-test: the DMA engine only (SDMA soft reset). The GFX
             * hang froze the whole host in the VM (card run 19): only with
             * --hang-test-gfx */
            int f = 0, gfx = !strcmp(argv[a], "--hang-test-gfx");
            for (int e = gfx; e < 1 + gfx; e++)
                for (int h = 1; h >= 0; h--) {
                    int jfd = drm_xnu_open(DRM_XNU_RENDER_NODE, 0);
                    if (jfd < 0) { perror("open RDNA3Accel"); f++; continue; }
                    f += job_test(jfd, e, h);
                    drm_xnu_close(jfd);
                }
            printf("HANG TEST: %s\n", f ? "FAILED" : gfx ? "PASSED (GFX recovered and ran the next job)"
                                                       : "PASSED (DMA recovered and ran the next job)");
            fails += f;
        }
    }
    drm_xnu_close(fd);
    printf("%s\n", fails ? "SOME CHECKS FAILED" : "Stage 4 UAPI smoke test passed");
    return fails ? 1 : 0;
}
