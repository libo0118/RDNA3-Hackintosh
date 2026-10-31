/*
 * rdna3_iop_copy.h - the display pipe's (IOP) CPU copy of a presented IOSurface into the scanout, and what it costs
 * (DRIVER-PLAN N61). Plain C, no kernel headers: accel/rdna3-accel.cpp (RD3DisplayPipe::performTransaction) copies
 * with it, tests/test_iop_copy.c checks it on the host.
 *
 * Every frame WindowServer presents reaches the panel through this copy (rdna3acceliop=1, iop_flip 0): the surface's
 * system memory (cached, read through the kernel's map of its memory descriptor) into the head's scanout VRAM through
 * BAR0 (a write-combined map, fb/rdna3-fb.cpp RD3K_VRAM_FN_SCANOUT). Card run 2026-10-28 counted no time for it. The
 * copy was 32-bit volatile loads and stores, one per pixel; a row whose source, destination, pitches and length are
 * all 8-byte aligned now moves 8 bytes per access (the same bytes in the same order, half the bus transactions; a
 * write-combined BAR merges them either way, an uncached one would not). Anything else keeps the 32-bit loop.
 *
 * Per head: copies, bytes, the copy's time, the longest, and the time the transaction spent in its gates before the
 * copy (the N59 stale wait, the VRAM page-off wait) - debug.rdna3accel_iop_copy_* / iop_gate_us.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_IOP_COPY_H
#define RDNA3_IOP_COPY_H

#include <stdint.h>

/* the 64-bit loop applies: every address and length it touches is 8-byte aligned */
static inline int rd3_iop_copy64_ok(uintptr_t src, uintptr_t dst, uint32_t spitch, uint32_t dpitch, uint32_t row_bytes)
{
    return !((src | dst) & 7u) && !((spitch | dpitch | row_bytes) & 7u);
}

/* copy `h` rows of `row_bytes` (a multiple of 4) from src (pitch spitch) to dst (pitch dpitch); returns 8 when the
 * 64-bit loop ran, 4 for the 32-bit one. Rows are stepped by bytes: the caller passes pitches of whole 32-bit words
 * (performTransaction refuses others), which is where the old word-stepped loop put them. */
static inline int rd3_iop_copy_rows(volatile void *dst, uint32_t dpitch, const volatile void *src, uint32_t spitch,
                                    uint32_t h, uint32_t row_bytes)
{
    if (rd3_iop_copy64_ok((uintptr_t)src, (uintptr_t)dst, spitch, dpitch, row_bytes)) {
        const uint32_t q = row_bytes / 8u;
        for (uint32_t y = 0; y < h; y++) {
            const volatile uint64_t *sp = (const volatile uint64_t *)((const volatile uint8_t *)src + (uint64_t)y * spitch);
            volatile uint64_t *dp = (volatile uint64_t *)((volatile uint8_t *)dst + (uint64_t)y * dpitch);
            for (uint32_t x = 0; x < q; x++) dp[x] = sp[x];
        }
        return 8;
    }
    const uint32_t words = row_bytes / 4u;
    for (uint32_t y = 0; y < h; y++) {
        const volatile uint32_t *sp = (const volatile uint32_t *)((const volatile uint8_t *)src + (uint64_t)y * spitch);
        volatile uint32_t *dp = (volatile uint32_t *)((volatile uint8_t *)dst + (uint64_t)y * dpitch);
        for (uint32_t x = 0; x < words; x++) dp[x] = sp[x];
    }
    return 4;
}

/* ==== Bare metal 2026-10-31: the formats a presented surface comes in ====
 * With the Metal plugin on the MacPro7,1 SMBIOS (two 1920x1080 heads, rdna3agdc=3) WindowServer presents 10-bit
 * surfaces: IOSurface pixel format 'l10r' (0x6c313072) = kCVPixelFormatType_ARGB2101010LEPacked, one little-endian
 * 32-bit word per pixel, A[31:30] R[29:20] G[19:10] B[9:0] (DRM_FORMAT_ARGB2101010; the plugin renders it as
 * VK_FORMAT_A2R10G10B10_UNORM_PACK32 = 58, Metal's BGR10A2Unorm). The scanout is 8 bits per channel (XRGB8888, the
 * 'BGRA' word B[7:0] G[15:8] R[23:16] A[31:24]), so performTransaction's CPU copy converts it: each channel keeps
 * its top 8 bits, the 2-bit alpha is widened (a * 0x55). SDMA cannot convert and the zero-copy flip would scan a
 * 10-bit buffer out as 8-bit: both are refused for l10r (the real port, DCN scanout of ARGB2101010 through
 * hubp2_program_pixel_format / dpp3_cnv_setup, is DRIVER-PLAN N66). Every other format is still refused. */
#define RD3_IOP_FMT_BGRA 0x42475241u   /* 'BGRA' */
#define RD3_IOP_FMT_L10R 0x6c313072u   /* 'l10r' */
enum rd3_iop_fmt { RD3_IOP_FMT_IS_BGRA = 0, RD3_IOP_FMT_IS_L10R, RD3_IOP_FMT_IS_OTHER };
static inline enum rd3_iop_fmt rd3_iop_fmt_kind(uint32_t fourcc)
{
    return fourcc == RD3_IOP_FMT_BGRA ? RD3_IOP_FMT_IS_BGRA : fourcc == RD3_IOP_FMT_L10R ? RD3_IOP_FMT_IS_L10R : RD3_IOP_FMT_IS_OTHER;
}

/* one l10r pixel as the scanout's 8-bit word */
static inline uint32_t rd3_iop_l10r_px(uint32_t v)
{
    const uint32_t a = (v >> 30) * 0x55u;
    return (a << 24) | (((v >> 22) & 0xffu) << 16) | (((v >> 12) & 0xffu) << 8) | ((v >> 2) & 0xffu);
}

/* rd3_iop_copy_rows for an l10r source: the same rows and bytes, each 32-bit word converted. Two pixels per 64-bit
 * load / store when everything is 8-aligned (returns 8; the write-combined BAR gets the same access sizes as the
 * BGRA copy), else one per 32-bit access (returns 4). The source is the surface's system memory (cached), as for
 * the plain copy; the conversion costs a few ALU operations per pixel on top of the BAR writes. */
static inline int rd3_iop_convert_l10r_rows(volatile void *dst, uint32_t dpitch, const volatile void *src, uint32_t spitch,
                                            uint32_t h, uint32_t row_bytes)
{
    if (rd3_iop_copy64_ok((uintptr_t)src, (uintptr_t)dst, spitch, dpitch, row_bytes)) {
        const uint32_t q = row_bytes / 8u;
        for (uint32_t y = 0; y < h; y++) {
            const volatile uint64_t *sp = (const volatile uint64_t *)((const volatile uint8_t *)src + (uint64_t)y * spitch);
            volatile uint64_t *dp = (volatile uint64_t *)((volatile uint8_t *)dst + (uint64_t)y * dpitch);
            for (uint32_t x = 0; x < q; x++) {
                const uint64_t v = sp[x];   /* little endian: the first pixel in the low word */
                dp[x] = (uint64_t)rd3_iop_l10r_px((uint32_t)v) | ((uint64_t)rd3_iop_l10r_px((uint32_t)(v >> 32)) << 32);
            }
        }
        return 8;
    }
    const uint32_t words = row_bytes / 4u;
    for (uint32_t y = 0; y < h; y++) {
        const volatile uint32_t *sp = (const volatile uint32_t *)((const volatile uint8_t *)src + (uint64_t)y * spitch);
        volatile uint32_t *dp = (volatile uint32_t *)((volatile uint8_t *)dst + (uint64_t)y * dpitch);
        for (uint32_t x = 0; x < words; x++) dp[x] = rd3_iop_l10r_px(sp[x]);
    }
    return 4;
}

/* MB/s of `bytes` in `us` microseconds (0 when no time was measured) */
static inline uint64_t rd3_iop_mbps(uint64_t bytes, uint64_t us)
{
    return us ? bytes / us : 0;   /* bytes per us = MB/s (10^6) */
}

/* a log line is due for the k-th (0-based) copy: the first 4, then every 1024th */
static inline int rd3_iop_copy_log_due(uint64_t k) { return k < 4u || (k & 1023u) == 0; }

/* ==== N63: the IOP copy by SDMA ====
 * The CPU copy above moves ~8 MB per 1920x1080 frame through the BAR on WindowServer's thread (card run 2026-10-28d:
 * avg 20 ms per head, 40 ms a frame with two heads). With the switch on (boot-arg rdna3iopsdma, sysctl
 * debug.rdna3accel_iop_sdma) the accelerator binds the surface's wired system pages into a GART range of the core kext
 * once per source slot (amdgpu_ttm_alloc_gart -> amdgpu_gart_bind of a GTT BO; kept while the slot keeps the memory
 * descriptor prepared, unbound before it is completed) and has SDMA0 copy the pitched rectangle from that range into
 * the scanout (amdgpu_copy_buffer, COPY_LINEAR: one span when the pitches match, else one per row), waiting for the
 * fence before the transaction returns. Any failure copies that frame with the CPU (counted by reason). */

/* the core's GART range table (kext/src/RDNA3Bringup.cpp iopGart*): first fit in an area of the GART nothing else
 * uses; a handle names a slot and the generation it was bound in, so a handle from before a reset or a wake (the
 * table is reset with the GART) or of an unbound range finds nothing */
#define RD3_IG_SLOTS 32u
struct rd3_ig_slot { uint64_t off, pages; uint32_t gen, used; };
struct rd3_ig {
    uint64_t base, pages;   /* the area: GART byte offset of its first page, its size in 4 KiB pages */
    uint32_t gen, live;     /* the last generation handed out (kept across resets), slots in use */
    struct rd3_ig_slot s[RD3_IG_SLOTS];
};

/* (re)start with nothing bound; the generation goes on so older handles stay invalid */
static inline void rd3_ig_reset(struct rd3_ig *t, uint64_t base, uint64_t pages)
{
    const uint32_t gen = t->gen;
    for (uint32_t i = 0; i < RD3_IG_SLOTS; i++) { t->s[i].off = 0; t->s[i].pages = 0; t->s[i].gen = 0; t->s[i].used = 0; }
    t->base = base & ~4095ull; t->pages = pages; t->gen = gen; t->live = 0;
}

static inline uint64_t rd3_ig_handle(uint32_t idx, uint32_t gen) { return ((uint64_t)gen << 32) | (uint64_t)(idx + 1u); }

/* the slot a handle names, or NULL (0, unknown, unbound, from an older generation) */
static inline struct rd3_ig_slot *rd3_ig_find(struct rd3_ig *t, uint64_t handle)
{
    const uint32_t idx = (uint32_t)(handle & 0xffffffffu), gen = (uint32_t)(handle >> 32);
    if (!idx || idx > RD3_IG_SLOTS || !gen) return 0;
    struct rd3_ig_slot *s = &t->s[idx - 1u];
    return (s->used && s->gen == gen) ? s : 0;
}

/* `npages` free pages starting at area page `at` */
static inline int rd3_ig_free_at(const struct rd3_ig *t, uint64_t at, uint64_t npages)
{
    if (at > t->pages || npages > t->pages - at) return 0;
    for (uint32_t i = 0; i < RD3_IG_SLOTS; i++) {
        const struct rd3_ig_slot *s = &t->s[i];
        if (!s->used) continue;
        const uint64_t so = (s->off - t->base) / 4096u;
        if (at < so + s->pages && so < at + npages) return 0;
    }
    return 1;
}

/* a range of `npages` (first fit, lowest offset): its handle and GART byte offset, or 0 (no slot, no room) */
static inline uint64_t rd3_ig_alloc(struct rd3_ig *t, uint64_t npages, uint64_t *off)
{
    uint32_t free_slot = RD3_IG_SLOTS;
    uint64_t best = ~0ull;
    if (!npages || npages > t->pages) return 0;
    for (uint32_t i = 0; i < RD3_IG_SLOTS && free_slot == RD3_IG_SLOTS; i++)
        if (!t->s[i].used) free_slot = i;
    if (free_slot == RD3_IG_SLOTS) return 0;
    /* candidates: the area's start and the end of every bound range */
    if (rd3_ig_free_at(t, 0, npages)) best = 0;
    for (uint32_t i = 0; i < RD3_IG_SLOTS; i++) {
        if (!t->s[i].used) continue;
        const uint64_t at = (t->s[i].off - t->base) / 4096u + t->s[i].pages;
        if (at < best && rd3_ig_free_at(t, at, npages)) best = at;
    }
    if (best == ~0ull) return 0;
    if (++t->gen == 0) t->gen = 1;   /* 0 never names a range */
    struct rd3_ig_slot *s = &t->s[free_slot];
    s->off = t->base + best * 4096u; s->pages = npages; s->gen = t->gen; s->used = 1;
    t->live++;
    *off = s->off;
    return rd3_ig_handle(free_slot, s->gen);
}

/* forget a range; 1 = it was bound (its GART offset and pages out, for the unmap), 0 = the handle names nothing */
static inline int rd3_ig_release(struct rd3_ig *t, uint64_t handle, uint64_t *off, uint64_t *pages)
{
    struct rd3_ig_slot *s = rd3_ig_find(t, handle);
    if (!s) return 0;
    *off = s->off; *pages = s->pages;
    s->used = 0; s->gen = 0; s->off = 0; s->pages = 0;
    if (t->live) t->live--;
    return 1;
}

/* the source rectangle (rows of row_bytes, pitch apart, from byte src_off of the range) lies inside its `bytes` */
static inline int rd3_ig_rect_ok(uint64_t bytes, uint64_t src_off, uint32_t pitch, uint32_t row_bytes, uint32_t rows)
{
    if (!rows || !row_bytes || row_bytes > pitch || src_off >= bytes) return 0;
    const uint64_t span = (uint64_t)(rows - 1u) * pitch + row_bytes;
    return span <= bytes - src_off;
}

/* the 4 KiB pages under a memory descriptor, from its physical segments in order: `md_off` is the segment's offset
 * in the descriptor, `lead` the in-page offset of the descriptor's first byte (the range maps descriptor byte X at
 * range byte lead + X). A segment must start where that puts it in its page: at a page start (a new page), or inside
 * the page the previous segment ended in (the same page continued). 0 = added, -1 = a page the range cannot express,
 * -2 = the list is full */
static inline int rd3_iop_pages_add(uint64_t *list, uint64_t cap, uint64_t *n, uint32_t lead, uint64_t md_off,
                                    uint64_t pa, uint64_t len)
{
    if (!len) return 0;
    const uint64_t in = (md_off + lead) & 4095u;
    if ((pa & 4095u) != in) return -1;
    uint64_t page = pa & ~4095ull;
    const uint64_t end = pa + len;
    if (in) {   /* inside a page: the previous segment's last page, or the descriptor's first byte */
        if (*n) { if (list[*n - 1u] != page) return -1; }
        else if (md_off) return -1;
        else { if (*n >= cap) return -2; list[(*n)++] = page; }
        page += 4096u;
    }
    for (; page < end; page += 4096u) {
        if (*n >= cap) return -2;
        list[(*n)++] = page;
    }
    return 0;
}

/* pages of a range holding `bytes` from in-page offset `lead` */
static inline uint64_t rd3_iop_range_pages(uint32_t lead, uint64_t bytes) { return ((uint64_t)lead + bytes + 4095u) / 4096u; }

/* why a frame went to the CPU copy instead (debug.rdna3accel_iop_copy_sdma_fb_<name>) */
enum rd3_iop_sdma_fb {
    RD3_ISF_CORE = 0,   /* the core does not answer rdna3IopSdma (older core) or its GART / SDMA are not up */
    RD3_ISF_PAGES,      /* the surface's physical pages could not be listed in GART order (or no memory for the list) */
    RD3_ISF_BIND,       /* the core could not bind the pages (no free GART range or slot, or refused) */
    RD3_ISF_COPY,       /* the SDMA copy was refused or its fence did not come in time */
    RD3_ISF_VERIFY,     /* the scanout's samples after the copy differ from the source: the CPU copied it again */
    RD3_ISF_OFF,        /* three failures in a row switched the SDMA path off until the sysctl is written */
    RD3_ISF_SCANOUT,    /* the scanout's VRAM offset does not name the memory its BAR address does (or no base known) */
    RD3_ISF_FORMAT,     /* bare metal 2026-10-31: the surface is l10r, which the CPU copy converts and SDMA cannot */
    RD3_ISF_N
};
static inline const char *rd3_iop_sdma_fb_name(unsigned r)
{
    static const char *const n[RD3_ISF_N] = { "core", "pages", "bind", "copy", "verify", "off", "scanout", "format" };
    return r < RD3_ISF_N ? n[r] : "?";
}

/* the scanout the CPU writes (BAR address `phys`) is the VRAM SDMA writes (offset `cookie`): the BAR's base
 * (`base`, learned from the core's own allocations: phys = BAR0 + offset) must lie under phys by exactly cookie. A
 * framebuffer on the boot console without the core's head info reports offset 0 for a console that may lie
 * elsewhere: SDMA must never write there. */
static inline int rd3_iop_scanout_ok(uint64_t base, uint64_t phys, uint64_t cookie, uint64_t bytes, uint64_t vram_size)
{
    if (!base || !phys || phys < cookie || phys - cookie != base) return 0;
    return !vram_size || (cookie <= vram_size && bytes <= vram_size - cookie);
}

/* failures in a row that switch the SDMA path off (the A2 swap copy's rule) */
#define RD3_ISF_OFF_AFTER 3u
/* frames a slot waits after the core refused its bind for want of room (NOSPACE) or a GART / ring that is not up
 * (NODEV) before it lists its pages and asks again (each ask walks the whole page list) */
#define RD3_ISF_BACKOFF_FRAMES 64u

/* the scanout is read back and compared with the source for the k-th (0-based) SDMA copy: the first 256, then every
 * 16th (16 samples through the BAR, a few microseconds) */
static inline int rd3_iop_sdma_verify_due(uint64_t k) { return k < 256u || (k & 15u) == 0; }

/* sample i (0..15) of a w x h frame: a 4 x 4 grid moved by `seq` so successive checks look at other pixels, the
 * first sample the frame's first pixel, the last its last */
static inline void rd3_iop_sample_xy(uint32_t i, uint32_t w, uint32_t h, uint64_t seq, uint32_t *x, uint32_t *y)
{
    if (!w || !h) { *x = *y = 0; return; }
    if (i == 0) { *x = 0; *y = 0; return; }
    if (i == 15) { *x = w - 1u; *y = h - 1u; return; }
    *x = (uint32_t)((((uint64_t)(2u * (i & 3u) + 1u) * w) / 8u + seq * 97u) % w);
    *y = (uint32_t)((((uint64_t)(2u * (i >> 2) + 1u) * h) / 8u + seq * 61u) % h);
}

#endif
