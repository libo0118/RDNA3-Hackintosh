/*
 * rdna3_hw.c - see rdna3_hw.h.
 *
 * Ports (MIT, Linux amdgpu): amdgpu_discovery_reg_base_init(),
 * amdgpu_device_rreg/wreg() + indirect PCIE access, psp_wait_for(),
 * amdgpu_device_mm_access(), gfxhub_v3_0_3_get_fb_location(),
 * nbio_v4_3_set_reg_remap()/remap_hdp_registers(), amdgpu_hdp_generic_flush().
 *
 * SPDX-License-Identifier: MIT
 */
#include "rdna3_hw.h"
#include "navi3x_regs.h"
#include "gen/mmhub_3_0_0.h"

void rdna3_log(rdna3_dev *d, int level, const char *fmt, ...)
{
    va_list ap;
    if (!d->os.vlog)
        return;
    va_start(ap, fmt);
    d->os.vlog(d->os.ctx, level, fmt, ap);
    va_end(ap);
}

static void zero(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--)
        *q++ = 0;
}

void rdna3_dev_init(rdna3_dev *d, const rdna3_os *os, uint64_t mmio_len)
{
    zero(d, sizeof(*d));
    d->os = *os;
    d->mmio_len = mmio_len;
    d->usec_timeout = 100000;
    d->rmmio_remap_offset = RDNA3_MMIO_REG_HOLE_OFFSET;
}

/* hw_id_map[] in amdgpu_discovery.c, restricted to what we use */
static int hwip_of(uint16_t hw_id)
{
    switch (hw_id) {
    case AMD_HWID_GC:     return HWIP_GC;
    case AMD_HWID_HDP:    return HWIP_HDP;
    case AMD_HWID_SDMA0:  return HWIP_SDMA0;
    case AMD_HWID_SDMA1:  return HWIP_SDMA1;
    case AMD_HWID_MMHUB:  return HWIP_MMHUB;
    case AMD_HWID_ATHUB:  return HWIP_ATHUB;
    case AMD_HWID_NBIF:   return HWIP_NBIO;
    case AMD_HWID_MP0:    return HWIP_MP0;
    case AMD_HWID_MP1:    return HWIP_MP1;
    case AMD_HWID_OSSSYS: return HWIP_OSSSYS;
    case AMD_HWID_SMUIO:  return HWIP_SMUIO;
    case AMD_HWID_DMU:    return HWIP_DCE;
    case AMD_HWID_VCN:    return HWIP_VCN;
    case AMD_HWID_THM:    return HWIP_THM;
    case AMD_HWID_CLKA:   return HWIP_CLK;
    case AMD_HWID_UMC:    return HWIP_UMC;
    default:              return -1;
    }
}

int rdna3_dev_set_discovery(rdna3_dev *d, const amd_discovery *disc)
{
    uint32_t i, k;
    for (i = 0; i < disc->num_ips; i++) {
        const amd_ip_entry *e = &disc->ips[i];
        int ip = hwip_of(e->hw_id);
        if (ip < 0 || e->instance >= RDNA3_MAX_INST)
            continue;
        for (k = 0; k < e->num_base_kept && k < RDNA3_MAX_SEG; k++)
            d->reg_base[ip][e->instance][k] = e->base[k];
        if (e->instance == 0) {
            d->ipver[ip].major = e->major;
            d->ipver[ip].minor = e->minor;
            d->ipver[ip].rev = e->revision;
            d->ipver[ip].present = 1;
            d->ipver[ip].num_base = (uint8_t)k;
        }
    }
    if (!d->ipver[HWIP_GC].present || !d->ipver[HWIP_MP0].present ||
        !d->ipver[HWIP_NBIO].present) {
        RERR(d, "discovery lacks GC/MP0/NBIO");
        return RDNA3_ENODEV;
    }
    if (d->ipver[HWIP_MP1].present && !d->ipver[HWIP_MP1].num_base) {
        RERR(d, "discovery: MP1 %u.%u.%u lacks register segment 0",
             d->ipver[HWIP_MP1].major, d->ipver[HWIP_MP1].minor, d->ipver[HWIP_MP1].rev);
        return RDNA3_ENODEV;
    }
    RLOG(d, "IP: GC %u.%u.%u MP0 %u.%u.%u MP1 %u.%u.%u NBIO %u.%u.%u HDP %u.%u.%u",
         d->ipver[HWIP_GC].major, d->ipver[HWIP_GC].minor, d->ipver[HWIP_GC].rev,
         d->ipver[HWIP_MP0].major, d->ipver[HWIP_MP0].minor, d->ipver[HWIP_MP0].rev,
         d->ipver[HWIP_MP1].major, d->ipver[HWIP_MP1].minor, d->ipver[HWIP_MP1].rev,
         d->ipver[HWIP_NBIO].major, d->ipver[HWIP_NBIO].minor, d->ipver[HWIP_NBIO].rev,
         d->ipver[HWIP_HDP].major, d->ipver[HWIP_HDP].minor, d->ipver[HWIP_HDP].rev);
    return RDNA3_OK;
}

uint32_t rdna3_reg_offset(const rdna3_dev *d, int ip, int inst, int seg, uint32_t reg)
{
    return d->reg_base[ip][inst][seg] + reg;
}

/* ---------------------------- MMIO ---------------------------- */

/* nbio_v4_3_get_pcie_index_offset */
static uint32_t pcie_index(rdna3_dev *d)
{
    return SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_RSMU_INDEX);
}
/* nbio_v4_3_get_pcie_data_offset */
static uint32_t pcie_data(rdna3_dev *d)
{
    return SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_RSMU_DATA);
}

static uint32_t mmio_rd(rdna3_dev *d, uint32_t reg)
{
    return d->os.mmio_rd(d->os.ctx, (uint64_t)reg * 4u);
}
static void mmio_wr(rdna3_dev *d, uint32_t reg, uint32_t v)
{
    d->os.mmio_wr(d->os.ctx, (uint64_t)reg * 4u, v);
}

uint32_t rdna3_smn_rreg(rdna3_dev *d, uint32_t addr)
{
    /* amdgpu_device_indirect_rreg(): index, read back index (posting), data */
    mmio_wr(d, pcie_index(d), addr);
    (void)mmio_rd(d, pcie_index(d));
    return mmio_rd(d, pcie_data(d));
}

void rdna3_smn_wreg(rdna3_dev *d, uint32_t addr, uint32_t v)
{
    mmio_wr(d, pcie_index(d), addr);
    (void)mmio_rd(d, pcie_index(d));
    mmio_wr(d, pcie_data(d), v);
    (void)mmio_rd(d, pcie_data(d));
}

uint32_t rdna3_rreg(rdna3_dev *d, uint32_t reg)
{
    if ((uint64_t)reg * 4u < d->mmio_len)
        return mmio_rd(d, reg);
    return rdna3_smn_rreg(d, reg * 4u);
}

void rdna3_wreg(rdna3_dev *d, uint32_t reg, uint32_t v)
{
    if ((uint64_t)reg * 4u < d->mmio_len)
        mmio_wr(d, reg, v);
    else
        rdna3_smn_wreg(d, reg * 4u, v);
}

int rdna3_wait_reg(rdna3_dev *d, uint32_t reg, uint32_t expect, uint32_t mask,
                   int wait_changed, uint32_t usec)
{
    uint32_t i, v = 0;
    if (!usec)
        usec = d->usec_timeout;
    for (i = 0; i < usec; i++) {
        v = rdna3_rreg(d, reg);
        if (wait_changed ? (v != expect) : ((v & mask) == expect))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RDBG(d, "reg 0x%x wait timed out: mask 0x%x read 0x%x exp 0x%x", reg, mask, v, expect);
    return RDNA3_ETIME;
}

/* ---------------------------- VRAM ---------------------------- */

/* gmc_v11_0_vram_gtt_location + amdgpu_gmc_vram_location: the FB base comes
 * from MMHUB (mmhub_v3_0_get_fb_location), VRAM spans vram_size from there,
 * the physical offset is MMHUB's FB_OFFSET (get_mc_fb_offset). The GC copy
 * of these registers reads 0 until the gfxhub is programmed. */
int rdna3_read_fb_location(rdna3_dev *d)
{
    uint64_t base = RREG32_SOC15(d, MMHUB, 0, regMMMC_VM_FB_LOCATION_BASE);
    uint64_t top  = RREG32_SOC15(d, MMHUB, 0, regMMMC_VM_FB_LOCATION_TOP);
    uint64_t off  = RREG32_SOC15(d, MMHUB, 0, regMMMC_VM_FB_OFFSET);

    base &= MMMC_VM_FB_LOCATION_BASE__FB_BASE_MASK;
    top  &= MMMC_VM_FB_LOCATION_TOP__FB_TOP_MASK;
    d->fb_start  = base << 24;
    d->fb_end    = d->vram_size ? d->fb_start + d->vram_size - 1 : (top << 24) | 0xFFFFFFull;
    d->fb_offset = off << 24;
    RLOG(d, "FB MC 0x%llx-0x%llx, phys offset 0x%llx (MMHUB top 0x%llx)",
         (unsigned long long)d->fb_start, (unsigned long long)d->fb_end,
         (unsigned long long)d->fb_offset, (unsigned long long)((top << 24) | 0xFFFFFFull));
    if (!base && !top) {
        RERR(d, "FB location not programmed (vBIOS did not post this GPU?)");
        return RDNA3_ENODEV;
    }
    return RDNA3_OK;
}

void rdna3_vram_alloc_init(rdna3_dev *d, uint64_t lo, uint64_t hi)
{
    d->alloc_lo = lo;
    d->alloc_hi = hi;
    d->alloc_next = lo;
    d->alloc_top_next = d->alloc_top_hi;
    d->nallocs = 0;
}

void rdna3_vram_alloc_top_init(rdna3_dev *d, uint64_t lo, uint64_t hi)
{
    d->alloc_top_lo = lo;
    d->alloc_top_hi = hi;
    d->alloc_top_next = hi;
}

uint64_t rdna3_vram_alloc_top(rdna3_dev *d, uint64_t size, uint64_t align, const char *tag)
{
    uint64_t start;
    if (!d->alloc_top_hi)
        return rdna3_vram_alloc(d, size, align, tag);
    if (!align)
        align = 4096;
    if ((align & (align - 1)) || size > d->alloc_top_next ||
        d->nallocs >= RDNA3_VRAM_MAX_ALLOCS) {
        RERR(d, "VRAM top alloc of %llu bytes for %s failed", (unsigned long long)size, tag);
        return UINT64_MAX;
    }
    start = (d->alloc_top_next - size) & ~(align - 1);
    if (start < d->alloc_top_lo) {
        RERR(d, "VRAM top alloc of %llu bytes for %s failed", (unsigned long long)size, tag);
        return UINT64_MAX;
    }
    d->alloc_top_next = start;
    d->allocs[d->nallocs].offset = start;
    d->allocs[d->nallocs].size = size;
    d->allocs[d->nallocs].tag = tag;
    d->nallocs++;
    RDBG(d, "VRAM %-10s 0x%09llx +0x%llx (MC 0x%llx, top-down)", tag, (unsigned long long)start,
         (unsigned long long)size, (unsigned long long)rdna3_vram_mc(d, start));
    return start;
}

uint64_t rdna3_vram_alloc(rdna3_dev *d, uint64_t size, uint64_t align, const char *tag)
{
    uint64_t start;
    if (!align)
        align = 4096;
    if (align & (align - 1))
        return UINT64_MAX;
    start = (d->alloc_next + align - 1) & ~(align - 1);
    if (start < d->alloc_next || size > d->alloc_hi || start > d->alloc_hi - size ||
        d->nallocs >= RDNA3_VRAM_MAX_ALLOCS) {
        RERR(d, "VRAM alloc of %llu bytes for %s failed", (unsigned long long)size, tag);
        return UINT64_MAX;
    }
    d->alloc_next = start + size;
    d->allocs[d->nallocs].offset = start;
    d->allocs[d->nallocs].size = size;
    d->allocs[d->nallocs].tag = tag;
    d->nallocs++;
    RDBG(d, "VRAM %-10s 0x%09llx +0x%llx (MC 0x%llx)", tag, (unsigned long long)start,
         (unsigned long long)size, (unsigned long long)rdna3_vram_mc(d, start));
    return start;
}

volatile uint8_t *rdna3_vram_cpu(rdna3_dev *d, uint64_t off, uint64_t len)
{
    if (!d->vram_cpu || off > d->vram_cpu_len || len > d->vram_cpu_len - off)
        return NULL;
    return d->vram_cpu + off;
}

/* one aligned 32-bit load (amdgpu_fence_read: le32_to_cpu(*ring->fence_drv.
 * cpu_addr)): a value the GPU updates (fence, wptr writeback) must not be
 * read a byte at a time, or a carry the GPU writes between two byte loads
 * gives a value no write ever had (0x1FF -> 0x200 read as 0x2FF: a fence
 * 255 jobs early). DRIVER-PLAN N54, ECC round 1. */
uint32_t rdna3_vram_read32(rdna3_dev *d, uint64_t off)
{
    volatile uint8_t *src = (off & 3) ? NULL : rdna3_vram_cpu(d, off, 4);
    uint32_t v = 0;
    if (!src) {
        rdna3_vram_read(d, off, &v, 4);
        return v;
    }
    return *(const volatile uint32_t *)(const volatile void *)src;
}

void rdna3_vram_write(rdna3_dev *d, uint64_t off, const void *src, uint64_t len)
{
    volatile uint8_t *dst = rdna3_vram_cpu(d, off, len);
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    if (!dst) {
        RERR(d, "vram_write 0x%llx+0x%llx outside the BAR", (unsigned long long)off,
             (unsigned long long)len);
        return;
    }
    if (((uintptr_t)dst & 3) == 0 && ((uintptr_t)s & 3) == 0) {
        for (i = 0; i + 4 <= len; i += 4)
            *(volatile uint32_t *)(dst + i) = *(const uint32_t *)(const void *)(s + i);
    } else {
        i = 0;
    }
    for (; i < len; i++)
        dst[i] = s[i];
    if (d->os.vram_written)
        d->os.vram_written(d->os.ctx, off, len);
}

void rdna3_vram_fill(rdna3_dev *d, uint64_t off, uint8_t v, uint64_t len)
{
    volatile uint8_t *dst = rdna3_vram_cpu(d, off, len);
    uint64_t i;
    if (!dst)
        return;
    for (i = 0; i < len; i++)
        dst[i] = v;
    if (d->os.vram_written)
        d->os.vram_written(d->os.ctx, off, len);
}

void rdna3_vram_read(rdna3_dev *d, uint64_t off, void *dst, uint64_t len)
{
    volatile uint8_t *src = rdna3_vram_cpu(d, off, len);
    uint8_t *o = (uint8_t *)dst;
    uint64_t i;
    if (!src) {
        zero(dst, (size_t)len);
        return;
    }
    for (i = 0; i < len; i++)
        o[i] = src[i];
}

void rdna3_vram_mm_read(rdna3_dev *d, uint64_t pos, uint32_t *dst, uint32_t dwords)
{
    uint32_t hi = ~0u, i;
    for (i = 0; i < dwords; i++, pos += 4) {
        uint32_t tmp = (uint32_t)(pos >> 31);
        mmio_wr(d, mmMM_INDEX, (uint32_t)pos | 0x80000000u);
        if (tmp != hi) {
            mmio_wr(d, mmMM_INDEX_HI, tmp);
            hi = tmp;
        }
        dst[i] = mmio_rd(d, mmMM_DATA);
    }
}

void rdna3_vram_mm_write(rdna3_dev *d, uint64_t pos, const uint32_t *src, uint32_t dwords)
{
    uint32_t hi = ~0u, i;
    for (i = 0; i < dwords; i++, pos += 4) {
        uint32_t tmp = (uint32_t)(pos >> 31);
        mmio_wr(d, mmMM_INDEX, (uint32_t)pos | 0x80000000u);
        if (tmp != hi) {
            mmio_wr(d, mmMM_INDEX_HI, tmp);
            hi = tmp;
        }
        mmio_wr(d, mmMM_DATA, src[i]);
    }
}

/* ----------------------------- doorbells ----------------------------- */

void rdna3_doorbell_wr64(rdna3_dev *d, uint32_t index, uint64_t v)
{
    if (d->os.wmb)
        d->os.wmb(d->os.ctx);   /* ring contents before the doorbell */
    if (d->os.doorbell_wr) {
        d->os.doorbell_wr(d->os.ctx, index, v);
        return;
    }
    if (d->doorbell_cpu && (uint64_t)index * 4 + 8 <= d->doorbell_len)
        *(volatile uint64_t *)(d->doorbell_cpu + (uint64_t)index * 4) = v;
}

void rdna3_doorbell_wr32(rdna3_dev *d, uint32_t index, uint32_t v)
{
    if (d->os.wmb)
        d->os.wmb(d->os.ctx);
    if (d->os.doorbell_wr32) {
        d->os.doorbell_wr32(d->os.ctx, index, v);
        return;
    }
    if (d->doorbell_cpu && (uint64_t)index * 4 + 4 <= d->doorbell_len)
        *(volatile uint32_t *)(d->doorbell_cpu + (uint64_t)index * 4) = v;
}

/* ---------------------------- NBIO / HDP ---------------------------- */

/* nbio_v4_3_remap_hdp_registers */
void rdna3_nbio_remap_hdp(rdna3_dev *d)
{
    WREG32_SOC15(d, NBIO, 0, regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL,
                 d->rmmio_remap_offset + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL);
    WREG32_SOC15(d, NBIO, 0, regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL,
                 d->rmmio_remap_offset + KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL);
}

/* hdp_v6_0_flush_hdp (no ring: the MMIO write, read back to post it) */
void rdna3_hdp_flush(rdna3_dev *d)
{
    uint32_t reg = (d->rmmio_remap_offset + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL) >> 2;
    if (d->os.wmb)
        d->os.wmb(d->os.ctx);
    rdna3_wreg(d, reg, 0);
    (void)rdna3_rreg(d, reg);
}

void rdna3_hdp_invalidate(rdna3_dev *d)
{
    (void)d;
}

uint32_t rdna3_nbio_rev_id(rdna3_dev *d)
{
    uint32_t v = RREG32_SOC15(d, NBIO, 0, regRCC_STRAP0_RCC_DEV0_EPF0_STRAP0);
    if (v == 0xffffffffu)
        return 0;
    return (v & RCC_STRAP0_RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0_MASK) >>
           RCC_STRAP0_RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0__SHIFT;
}

uint64_t rdna3_gpu_clock_counter(rdna3_dev *d)
{
    uint64_t hi_pre = RREG32_SOC15(d, SMUIO, 0, regGOLDEN_TSC_COUNT_UPPER);
    uint64_t lo = RREG32_SOC15(d, SMUIO, 0, regGOLDEN_TSC_COUNT_LOWER);
    uint64_t hi_after = RREG32_SOC15(d, SMUIO, 0, regGOLDEN_TSC_COUNT_UPPER);
    if (hi_pre != hi_after)
        lo = RREG32_SOC15(d, SMUIO, 0, regGOLDEN_TSC_COUNT_LOWER);
    return lo | (hi_after << 32);
}

/* amdgpu_get_marketing_name (libdrm amdgpu_asic_id.c over data/amdgpu.ids):
 * the Navi 3x entries (the host test checks them against that file) */
const char *rdna3_marketing_name_ex(uint16_t device, uint8_t revision, int *exact)
{
    static const struct { uint16_t dev; uint8_t rev; const char *name; } ids[] = {
        { 0x7448, 0x00, "AMD Radeon Pro W7900" },
        { 0x7449, 0x00, "AMD Radeon Pro W7800 48GB" },
        { 0x744A, 0x00, "AMD Radeon Pro W7900 Dual Slot" },
        { 0x744B, 0x00, "AMD Radeon Pro W7900D" },
        { 0x744C, 0xC8, "AMD Radeon RX 7900 XTX" },
        { 0x744C, 0xCC, "AMD Radeon RX 7900 XT" },
        { 0x744C, 0xCE, "AMD Radeon RX 7900 GRE" },
        { 0x744C, 0xCF, "AMD Radeon RX 7900M" },
        { 0x745E, 0xCC, "AMD Radeon Pro W7800" },
        { 0x7460, 0x00, "AMD Radeon Pro V710" },
        { 0x7461, 0x00, "AMD Radeon Pro V710 MxGPU" },
        { 0x7470, 0x00, "AMD Radeon Pro W7700" },
        { 0x747E, 0xC8, "AMD Radeon RX 7800 XT" },
        { 0x747E, 0xD8, "AMD Radeon RX 7800M" },
        { 0x747E, 0xDB, "AMD Radeon RX 7700" },
        { 0x747E, 0xFF, "AMD Radeon RX 7700 XT" },
        { 0x7480, 0x00, "AMD Radeon Pro W7600" },
        { 0x7480, 0xC0, "AMD Radeon RX 7600 XT" },
        { 0x7480, 0xC1, "AMD Radeon RX 7700S" },
        { 0x7480, 0xC2, "AMD Radeon RX 7650 GRE" },
        { 0x7480, 0xC3, "AMD Radeon RX 7600S" },
        { 0x7480, 0xC7, "AMD Radeon RX 7600M XT" },
        { 0x7480, 0xCF, "AMD Radeon RX 7600" },
        { 0x7481, 0xC7, "AMD Steam Machine" },
        { 0x7483, 0xCF, "AMD Radeon RX 7600M" },
        { 0x7489, 0x00, "AMD Radeon Pro W7500" },
        { 0x7499, 0x00, "AMD Radeon Pro W7400" },
        { 0x7499, 0xC0, "AMD Radeon RX 7400" },
        { 0x7499, 0xC1, "AMD Radeon RX 7300" },
    };
    unsigned i;
    if (exact)
        *exact = 1;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (ids[i].dev == device && ids[i].rev == revision)
            return ids[i].name;
    if (exact)
        *exact = 0;
    switch (device) {
    case 0x744C: return "AMD Radeon RX 7900 Series";
    case 0x747E: return "AMD Radeon RX 7800 / 7700 Series";
    case 0x7480: return "AMD Radeon RX 7600 Series";
    default:     return "AMD Radeon RX 7000 Series";
    }
}

const char *rdna3_marketing_name(uint16_t device, uint8_t revision)
{
    return rdna3_marketing_name_ex(device, revision, 0);
}
