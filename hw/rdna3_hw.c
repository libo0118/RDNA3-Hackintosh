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
        }
    }
    if (!d->ipver[HWIP_GC].present || !d->ipver[HWIP_MP0].present ||
        !d->ipver[HWIP_NBIO].present) {
        RERR(d, "discovery lacks GC/MP0/NBIO");
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

static uint32_t pcie_index(rdna3_dev *d)
{
    return SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_RSMU_INDEX);
}
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

int rdna3_read_fb_location(rdna3_dev *d)
{
    uint64_t base = RREG32_SOC15(d, GC, 0, regGCMC_VM_FB_LOCATION_BASE);
    uint64_t top  = RREG32_SOC15(d, GC, 0, regGCMC_VM_FB_LOCATION_TOP);
    uint64_t off  = RREG32_SOC15(d, GC, 0, regGCMC_VM_FB_OFFSET);

    base &= GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK;
    top  &= GCMC_VM_FB_LOCATION_TOP__FB_TOP_MASK;
    d->fb_start  = base << 24;
    d->fb_end    = (top << 24) | 0xFFFFFFull;
    d->fb_offset = off << 24;
    RLOG(d, "FB MC 0x%llx-0x%llx, phys offset 0x%llx", (unsigned long long)d->fb_start,
         (unsigned long long)d->fb_end, (unsigned long long)d->fb_offset);
    if (d->fb_end <= d->fb_start) {
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
    d->nallocs = 0;
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

/* ---------------------------- NBIO / HDP ---------------------------- */

void rdna3_nbio_remap_hdp(rdna3_dev *d)
{
    WREG32_SOC15(d, NBIO, 0, regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL,
                 d->rmmio_remap_offset + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL);
    WREG32_SOC15(d, NBIO, 0, regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL,
                 d->rmmio_remap_offset + KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL);
}

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
