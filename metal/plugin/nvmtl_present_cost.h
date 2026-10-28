/*
 * nvmtl_present_cost.h - what putting a finished frame into its IOSurface costs, per process (DRIVER-PLAN N61).
 *
 * Plain C99 (no Objective-C, no Vulkan): tests/test_present_cost.c checks the counters, the SUMMARY body and the
 * geometry of the GPU copy-out exactly as NVMTLObjects.m uses them.
 *
 * Card run 2026-10-28 (reports/2026-10-28b, app survey): the frame-order page advanced ~60 frames a second, the scanout
 * showed 44 distinct frames in 10 s. WindowServer composites its displays into PRIVATE images (its display surfaces are
 * kept out of A3, N27) and -nvmtlSurfaceOut put each finished frame into the IOSurface after the GPU: an image ->
 * staging buffer copy submitted ON ITS OWN and waited for, then a CPU copy of the staging buffer into the surface
 * (`G13 surface-out`; the plugin trace's 781 kept copy-outs of 1920x1080: median 1.5 ms, average 10 ms, one in ten
 * over 24 ms, longest 251 ms), on the one execution thread that advances every command buffer. The N59 hand-off held
 * each command buffer's scheduled point until that copy-out was done (`held 15858 (avg 12147 us)`).
 *
 * Paths of a copy-out (`path` below):
 *   NVMTL_SO_CPU  the old path: staging submit + fence wait, then the CPU copy (lock / gpu / cpu split measured)
 *   NVMTL_SO_DMA  the command buffer's last part copied the image into the IOSurface's own pages (imported as a GPU
 *                 buffer, snooped system memory) on the GPU: the frame is in the surface when the GPU completes; the
 *                 publish only takes and gives back the IOSurface write lock so the seeds move (the kernel's N59 gate
 *                 and every other reader of the seeds see the new frame)
 * Why a copy-out that could have gone the GPU way went the CPU way (`why`, counted, the reason in the log):
 *   OFF     NVMTL_SURFOUT_DMA=0 (the A/B switch: the old path for every surface)
 *   FIRST   the texture's first copy-out (a wrapper made per frame never imports; the second one imports)
 *   SHAPE   format / samples / pitch / offset / alignment the GPU copy cannot take (nvmtl_so_plan says which)
 *   RO      the surface's pages are not writable in this process (the kernel would wire them read-only: a GPU write
 *           there is a VM fault)
 *   IMPORT  the host-pointer import was refused (RADV / GEM_USERPTR; the vk: line says why)
 *   MOVED   the surface's base address is not the one imported (a remapped / purged surface): the import is retired
 *   RECORD  recording the copy into the command buffer failed
 */
#ifndef NVMTL_PRESENT_COST_H
#define NVMTL_PRESENT_COST_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum { NVMTL_SO_CPU = 0, NVMTL_SO_DMA = 1, NVMTL_SO_NPATH = 2 };
enum { NVMTL_SO_OK = 0, NVMTL_SO_WHY_OFF = 1, NVMTL_SO_WHY_FIRST = 2, NVMTL_SO_WHY_SHAPE = 3, NVMTL_SO_WHY_RO = 4,
       NVMTL_SO_WHY_IMPORT = 5, NVMTL_SO_WHY_MOVED = 6, NVMTL_SO_WHY_RECORD = 7, NVMTL_SO_NWHY = 8 };

/* a copy-out this long or longer is a slow one (one frame at 60 Hz) */
#define NVMTL_PC_SLOW_NS 16000000ull
/* the copy-out a texture imports its surface at (0-based count of its earlier copy-outs) */
#define NVMTL_SO_IMPORT_AFTER 1u

typedef struct {
    uint64_t n[NVMTL_SO_NPATH], bytes[NVMTL_SO_NPATH], ns[NVMTL_SO_NPATH], max_ns[NVMTL_SO_NPATH];
    uint64_t lock_ns, gpu_ns, cpu_ns;      /* the CPU path's split: IOSurfaceLock, staging submit + wait, the CPU copy */
    uint64_t slow;                         /* copy-outs of either path >= NVMTL_PC_SLOW_NS */
    uint64_t why[NVMTL_SO_NWHY];           /* CPU copy-outs and the reason the GPU path was not taken */
    uint64_t imports, import_fail;         /* surfaces imported as GPU buffers / imports refused */
    uint64_t in_n, in_bytes, in_ns, in_max_ns;   /* surface-in uploads (IOSurface -> private image) */
} nvmtl_pc_stats;

static inline void nvmtl_pc_max(uint64_t *m, uint64_t v)
{
    uint64_t cur = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(m, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* one copy-out: its path, bytes, the whole time (lock to unlock) and, for the CPU path, the split */
static inline void nvmtl_pc_out(nvmtl_pc_stats *s, int path, uint64_t bytes, uint64_t total_ns, uint64_t lock_ns,
                                uint64_t gpu_ns, uint64_t cpu_ns)
{
    const int p = path == NVMTL_SO_DMA ? NVMTL_SO_DMA : NVMTL_SO_CPU;
    __atomic_fetch_add(&s->n[p], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->bytes[p], bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->ns[p], total_ns, __ATOMIC_RELAXED);
    nvmtl_pc_max(&s->max_ns[p], total_ns);
    if (total_ns >= NVMTL_PC_SLOW_NS) __atomic_fetch_add(&s->slow, 1, __ATOMIC_RELAXED);
    if (p == NVMTL_SO_CPU) {
        __atomic_fetch_add(&s->lock_ns, lock_ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->gpu_ns, gpu_ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->cpu_ns, cpu_ns, __ATOMIC_RELAXED);
    }
}

static inline void nvmtl_pc_why(nvmtl_pc_stats *s, int why)
{
    if (why > 0 && why < NVMTL_SO_NWHY) __atomic_fetch_add(&s->why[why], 1, __ATOMIC_RELAXED);
}

static inline void nvmtl_pc_in(nvmtl_pc_stats *s, uint64_t bytes, uint64_t ns)
{
    __atomic_fetch_add(&s->in_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->in_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->in_ns, ns, __ATOMIC_RELAXED);
    nvmtl_pc_max(&s->in_max_ns, ns);
}

static inline const char *nvmtl_so_why_name(int why)
{
    static const char *const names[NVMTL_SO_NWHY] = { "ok", "off", "first", "shape", "ro", "import", "moved", "record" };
    return why >= 0 && why < NVMTL_SO_NWHY ? names[why] : "?";
}

/* MB/s of `bytes` in `ns` (0 when no time was measured) */
static inline uint64_t nvmtl_pc_mbps(uint64_t bytes, uint64_t ns)
{
    return ns ? (uint64_t)((double)bytes * 1000.0 / (double)ns) : 0;
}

/* the moved-counter of the SUMMARY: a line is printed only when it changed */
static inline uint64_t nvmtl_pc_events(const nvmtl_pc_stats *s)
{
    return __atomic_load_n(&s->n[0], __ATOMIC_RELAXED) + __atomic_load_n(&s->n[1], __ATOMIC_RELAXED)
         + __atomic_load_n(&s->in_n, __ATOMIC_RELAXED) + __atomic_load_n(&s->import_fail, __ATOMIC_RELAXED);
}

/* the `vk: present cost: SUMMARY` body; 0 when nothing was counted */
static inline int nvmtl_pc_format(const nvmtl_pc_stats *s, int dma_mode, char *out, size_t cap)
{
#define L_(x) ((unsigned long long)__atomic_load_n(&s->x, __ATOMIC_RELAXED))
    const unsigned long long nc = L_(n[0]), nd = L_(n[1]), outs = nc + nd, ins = L_(in_n);
    if ((!outs && !ins && !L_(import_fail)) || !cap) return 0;
    const unsigned long long ns_all = L_(ns[0]) + L_(ns[1]);
    const unsigned long long mx = L_(max_ns[0]) > L_(max_ns[1]) ? L_(max_ns[0]) : L_(max_ns[1]);
    snprintf(out, cap,
             "outs=%llu cpu=%llu dma=%llu out_mb=%llu out_us_avg=%llu out_us_max=%llu cpu_us_avg=%llu cpu_us_max=%llu "
             "dma_us_avg=%llu dma_us_max=%llu lock_us_avg=%llu gpu_us_avg=%llu memcpy_us_avg=%llu memcpy_mbps=%llu slow=%llu "
             "why_off=%llu why_first=%llu why_shape=%llu why_ro=%llu why_import=%llu why_moved=%llu why_record=%llu "
             "imports=%llu import_fail=%llu ins=%llu in_mb=%llu in_us_avg=%llu in_us_max=%llu dma_mode=%d",
             outs, nc, nd, (L_(bytes[0]) + L_(bytes[1])) >> 20, outs ? ns_all / outs / 1000u : 0, mx / 1000u,
             nc ? L_(ns[0]) / nc / 1000u : 0, L_(max_ns[0]) / 1000u, nd ? L_(ns[1]) / nd / 1000u : 0, L_(max_ns[1]) / 1000u,
             nc ? L_(lock_ns) / nc / 1000u : 0, nc ? L_(gpu_ns) / nc / 1000u : 0, nc ? L_(cpu_ns) / nc / 1000u : 0,
             (unsigned long long)nvmtl_pc_mbps(L_(bytes[0]), L_(cpu_ns)), L_(slow),
             L_(why[NVMTL_SO_WHY_OFF]), L_(why[NVMTL_SO_WHY_FIRST]), L_(why[NVMTL_SO_WHY_SHAPE]), L_(why[NVMTL_SO_WHY_RO]),
             L_(why[NVMTL_SO_WHY_IMPORT]), L_(why[NVMTL_SO_WHY_MOVED]), L_(why[NVMTL_SO_WHY_RECORD]),
             L_(imports), L_(import_fail), ins, L_(in_bytes) >> 20, ins ? L_(in_ns) / ins / 1000u : 0, L_(in_max_ns) / 1000u, dma_mode);
#undef L_
    return 1;
}

/* NVMTL_SURFOUT_DMA: the GPU copy-out is ON by default, off only when the value starts with '0' (the A/B switch) */
static inline int nvmtl_so_dma_mode(const char *v) { return !(v && v[0] == '0'); }

/* a rate-limited log line is due for the k-th (0-based) event: the first 8, then every 1024th */
static inline int nvmtl_pc_log_due(uint64_t k) { return k < 8 || (k & 1023u) == 0; }

/* The GPU copy-out of a private image into its IOSurface plane (vkCmdCopyImageToBuffer into the imported surface):
 * what is imported and where the plane's rows land, or why it cannot be done (NVMTL_SO_WHY_SHAPE with `bad` naming
 * the rule). Inputs are the IOSurface's (IOSurfaceGetBaseAddress / AllocSize / BaseAddressOfPlane / BytesPerRowOfPlane)
 * and the image's (w, h, bytes per element, block size, samples, layers); `align` is Vulkan's
 * minImportedHostPointerAlignment. */
typedef struct {
    uintptr_t base, plane_base; size_t alloc, row_bytes;
    uint32_t w, h, bpp, bw, bh, samples, layers; size_t align;
} nvmtl_so_geom;
typedef struct { uintptr_t import_base; size_t import_len, offset; uint32_t row_bytes; const char *bad; } nvmtl_so_plan_t;

static inline int nvmtl_so_plan(const nvmtl_so_geom *g, nvmtl_so_plan_t *p)
{
    p->import_base = 0; p->import_len = 0; p->offset = 0; p->row_bytes = 0; p->bad = NULL;
#define BAD_(why) do { p->bad = (why); return NVMTL_SO_WHY_SHAPE; } while (0)
    if (!g->base || !g->plane_base || !g->alloc || !g->row_bytes || !g->w || !g->h) BAD_("no surface memory or an empty image");
    if (g->samples > 1) BAD_("multisampled image");
    if (g->bw > 1 || g->bh > 1) BAD_("block-compressed format");
    if (g->layers > 1) BAD_("layered image");
    if (g->bpp != 1 && g->bpp != 2 && g->bpp != 4 && g->bpp != 8 && g->bpp != 16) BAD_("element size not 1/2/4/8/16 bytes");
    if (!g->align || (g->align & (g->align - 1))) BAD_("no host-pointer import alignment");
    if (g->base & (g->align - 1)) BAD_("surface base not aligned for a host-pointer import");
    if (g->plane_base < g->base) BAD_("plane before the surface base");
    if (g->row_bytes % g->bpp) BAD_("bytes per row not a whole number of elements");
    if (g->row_bytes / g->bpp > 0xFFFFFFFFu || g->row_bytes > 0xFFFFFFFFu) BAD_("bytes per row too large");
    if ((size_t)g->w * g->bpp > g->row_bytes) BAD_("a row of the image is wider than the surface's row");
    const size_t off = (size_t)(g->plane_base - g->base);
    if (off % g->bpp || off % 4) BAD_("plane offset not a multiple of the element size and of 4");
    const size_t len = (g->alloc + g->align - 1) & ~(g->align - 1);
    if (len < g->alloc) BAD_("allocation size overflows");
    const size_t need = (size_t)(g->h - 1) * g->row_bytes + (size_t)g->w * g->bpp;
    if (off > g->alloc || need > g->alloc - off) BAD_("the image's rows do not fit in the surface allocation");
#undef BAD_
    p->import_base = g->base; p->import_len = len; p->offset = off; p->row_bytes = (uint32_t)g->row_bytes;
    return NVMTL_SO_OK;
}

#endif
