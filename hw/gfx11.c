/*
 * gfx11.c - see gfx11.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gfx11.h"
#include "gen/gc_11_0_0_gfx.h"
#include "clearstate_defs.h"
#include "clearstate_gfx11.h"
#include "amdgpu_ucode_hdr.h"
#include "navi3x_regs.h"
#include "doorbell.h"

#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_SET_FIELD(orig, reg, field, val) \
    (((orig) & ~(uint32_t)REG_FIELD_MASK(reg, field)) | \
     ((uint32_t)REG_FIELD_MASK(reg, field) & ((uint32_t)(val) << REG_FIELD_SHIFT(reg, field))))
#define REG_GET_FIELD(v, reg, field) \
    (((v) & (uint32_t)REG_FIELD_MASK(reg, field)) >> REG_FIELD_SHIFT(reg, field))

/* paste reg##_BASE_IDX at the outermost macro so 'reg' is not expanded first */
#define OFF(d, reg)   rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg)
#define RG(d, reg)    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg))
#define WG(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg), (v))

/* soc21_enum.h / gfx_v11_0.c */
#define SH_MEM_ADDRESS_MODE_64          0
#define SH_MEM_ALIGNMENT_MODE_UNALIGNED 3
#define DEFAULT_SH_MEM_CONFIG \
    ((SH_MEM_ADDRESS_MODE_64 << SH_MEM_CONFIG__ADDRESS_MODE__SHIFT) | \
     (SH_MEM_ALIGNMENT_MODE_UNALIGNED << SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT) | \
     (3 << SH_MEM_CONFIG__INITIAL_INST_PREFETCH__SHIFT))
#define SHARED_APERTURE_START  0x2000000000000000ull   /* gmc_v11_0_sw_init */
#define PRIVATE_APERTURE_START 0x1000000000000000ull
#define CACHE_FLUSH_AND_INV_TS_EVENT 0x14
#define MAX_HW_CONTEXTS 8

/* clear-state staging (kernel stacks are small; bring-up is single threaded) */
static uint32_t g_csb[1024];

static uint32_t order_base_2(uint32_t n)
{
    uint32_t r = 0;
    while ((1u << r) < n)
        r++;
    return r;
}

/* soc21_grbm_select */
static void grbm_select(rdna3_dev *d, uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid)
{
    uint32_t v = 0;
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID, pipe);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, MEID, me);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, VMID, vmid);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, QUEUEID, queue);
    WG(d, regGRBM_GFX_CNTL, v);
}

int gfx11_rs64_from_fw(const void *pfp, uint32_t pfp_len, const void *me, uint32_t me_len,
                       const void *mec, uint32_t mec_len, rdna3_rs64_start *out)
{
    const struct gfx_firmware_header_v2_0 *h[3];
    if (!pfp || !me || !mec || pfp_len < sizeof(*h[0]) || me_len < sizeof(*h[0]) ||
        mec_len < sizeof(*h[0]))
        return RDNA3_EINVAL;
    h[0] = (const struct gfx_firmware_header_v2_0 *)pfp;
    h[1] = (const struct gfx_firmware_header_v2_0 *)me;
    h[2] = (const struct gfx_firmware_header_v2_0 *)mec;
    for (int i = 0; i < 3; i++)
        if (h[i]->header.header_version_major != 2)
            return RDNA3_EINVAL;
    out->pfp_lo = h[0]->ucode_start_addr_lo; out->pfp_hi = h[0]->ucode_start_addr_hi;
    out->me_lo = h[1]->ucode_start_addr_lo;  out->me_hi = h[1]->ucode_start_addr_hi;
    out->mec_lo = h[2]->ucode_start_addr_lo; out->mec_hi = h[2]->ucode_start_addr_hi;
    return RDNA3_OK;
}

/* gfx_v11_0_wait_for_rlc_autoload_complete (GC 11.0.3 register) */
static int wait_rlc_autoload(rdna3_dev *d)
{
    uint32_t i, cp = 0, boot = 0;
    for (i = 0; i < d->usec_timeout; i++) {
        cp = RG(d, regCP_STAT);
        boot = RG(d, regRLC_RLCS_BOOTLOAD_STATUS);
        if (cp == 0 && REG_GET_FIELD(boot, RLC_RLCS_BOOTLOAD_STATUS, BOOTLOAD_COMPLETE))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GFX: rlc autoload timeout (CP_STAT 0x%08x, BOOTLOAD_STATUS 0x%08x)", cp, boot);
    return RDNA3_ETIME;
}

/* gfx_v11_0_config_gfx_rs64 */
static void config_gfx_rs64(rdna3_dev *d, const rdna3_rs64_start *s)
{
    uint32_t pipe, tmp;

    for (pipe = 0; pipe < 2; pipe++) {
        grbm_select(d, 0, pipe, 0, 0);
        WG(d, regCP_PFP_PRGRM_CNTR_START, (s->pfp_hi << 30) | (s->pfp_lo >> 2));
        WG(d, regCP_PFP_PRGRM_CNTR_START_HI, s->pfp_hi >> 2);
    }
    grbm_select(d, 0, 0, 0, 0);
    tmp = RG(d, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, 1);
    WG(d, regCP_ME_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, 0);
    WG(d, regCP_ME_CNTL, tmp);

    for (pipe = 0; pipe < 2; pipe++) {
        grbm_select(d, 0, pipe, 0, 0);
        WG(d, regCP_ME_PRGRM_CNTR_START, (s->me_hi << 30) | (s->me_lo >> 2));
        WG(d, regCP_ME_PRGRM_CNTR_START_HI, s->me_hi >> 2);
    }
    grbm_select(d, 0, 0, 0, 0);
    tmp = RG(d, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, 1);
    WG(d, regCP_ME_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, 0);
    WG(d, regCP_ME_CNTL, tmp);

    for (pipe = 0; pipe < 4; pipe++) {
        grbm_select(d, 1, pipe, 0, 0);
        WG(d, regCP_MEC_RS64_PRGRM_CNTR_START, (s->mec_lo >> 2) | (s->mec_hi << 30));
        WG(d, regCP_MEC_RS64_PRGRM_CNTR_START_HI, s->mec_hi >> 2);
    }
    grbm_select(d, 0, 0, 0, 0);
    tmp = RG(d, regCP_MEC_RS64_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 1);
    WG(d, regCP_MEC_RS64_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 0);
    WG(d, regCP_MEC_RS64_CNTL, tmp);
}

/* gfx_v11_0_gfxhub_enable */
static int gfxhub_enable(rdna3_dev *d, rdna3_gmc *g)
{
    int r = gfxhub303_gart_enable(d, g);
    if (r)
        return r;
    rdna3_hdp_flush(d);
    gfxhub303_set_fault_enable_default(d, 1);
    return gfxhub303_flush_tlb(d, 0, 0);
}

/* golden_settings_gc_11_0 via soc15_program_register_sequence */
static void golden_registers(rdna3_dev *d)
{
    uint32_t and_mask = 0x20000000, or_mask = 0x20000000;
    uint32_t tmp = RG(d, regTCP_CNTL);
    tmp = (tmp & ~and_mask) | (or_mask & and_mask);
    WG(d, regTCP_CNTL, tmp);
}

/* gfx_v11_0_constants_init, minus what discovery already gave us (RB/CU
 * info) and the compute/GDS VMID setup (with the compute queues) */
static void constants_init(rdna3_dev *d)
{
    uint32_t tmp, i;

    tmp = RG(d, regGRBM_CNTL);
    tmp = REG_SET_FIELD(tmp, GRBM_CNTL, READ_TIMEOUT, 0xff);
    WG(d, regGRBM_CNTL, tmp);
    /* one gfx pipe in use: all occlusion counters to pipe 0 */
    tmp = RG(d, regDB_RING_CONTROL);
    tmp = REG_SET_FIELD(tmp, DB_RING_CONTROL, COUNTER_CONTROL, 1);
    WG(d, regDB_RING_CONTROL, tmp);

    for (i = 0; i < 16; i++) {
        grbm_select(d, 0, 0, 0, i);
        WG(d, regSH_MEM_CONFIG, DEFAULT_SH_MEM_CONFIG);
        if (i) {
            tmp = REG_SET_FIELD(0u, SH_MEM_BASES, PRIVATE_BASE, PRIVATE_APERTURE_START >> 48);
            tmp = REG_SET_FIELD(tmp, SH_MEM_BASES, SHARED_BASE, SHARED_APERTURE_START >> 48);
            WG(d, regSH_MEM_BASES, tmp);
        }
    }
    grbm_select(d, 0, 0, 0, 0);
}

/* ---------------------------- clear state ---------------------------- */

uint32_t gfx11_csb_size(void)
{
    const struct cs_section_def *sect;
    const struct cs_extent_def *ext;
    uint32_t count = 2 + 3;    /* begin clear state + context control */
    for (sect = gfx11_cs_data; sect->section; ++sect)
        for (ext = sect->section; ext->extent; ++ext) {
            if (sect->id != SECT_CONTEXT)
                return 0;
            count += 2 + ext->reg_count;
        }
    return count + 3 + 2 + 2;  /* tile steering override, end, CLEAR_STATE */
}

uint32_t gfx11_csb_fill(uint32_t *buf, uint32_t max_dw)
{
    const struct cs_section_def *sect;
    const struct cs_extent_def *ext;
    uint32_t c = 0, i;
    if (max_dw < gfx11_csb_size())
        return 0;
    buf[c++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
    buf[c++] = PACKET3_PREAMBLE_BEGIN_CLEAR_STATE;
    buf[c++] = PACKET3(PACKET3_CONTEXT_CONTROL, 1);
    buf[c++] = 0x80000000;
    buf[c++] = 0x80000000;
    for (sect = gfx11_cs_data; sect->section; ++sect)
        for (ext = sect->section; ext->extent; ++ext) {
            buf[c++] = PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count);
            buf[c++] = ext->reg_index - PACKET3_SET_CONTEXT_REG_START;
            for (i = 0; i < ext->reg_count; i++)
                buf[c++] = ext->extent[i];
        }
    buf[c++] = PACKET3(PACKET3_SET_CONTEXT_REG, 1);
    /* SOC15_REG_OFFSET(PA_SC_TILE_STEERING_OVERRIDE) - SET_CONTEXT_REG_START: the
     * context space starts at the GC segment-1 base, so this is the bare offset */
    buf[c++] = regPA_SC_TILE_STEERING_OVERRIDE;
    buf[c++] = 0;              /* pa_sc_tile_steering_override */
    buf[c++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
    buf[c++] = PACKET3_PREAMBLE_END_CLEAR_STATE;
    buf[c++] = PACKET3(PACKET3_CLEAR_STATE, 0);
    buf[c++] = 0;
    return c;
}

/* ------------------------------ ring ------------------------------ */

static uint32_t wb_read32(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t off)
{
    uint32_t v;
    rdna3_vram_read(d, r->wb + off, &v, 4);
    return v;
}

void gfx11_write(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t v)
{
    rdna3_vram_write(d, r->ring + (r->wptr & r->buf_mask) * 4, &v, 4);
    r->wptr++;
}

/* gfx_v11_0_ring_set_wptr_gfx / _compute: poll slot, then the doorbell
 * (dwords) or, gfx only, CP_RB0_WPTR */
void gfx11_commit(rdna3_dev *d, rdna3_gfx_ring *r)
{
    rdna3_vram_write(d, r->wb + RD_GFX_WB_WPTR, &r->wptr, 8);
    rdna3_hdp_flush(d);
    if (r->use_doorbell) {
        rdna3_doorbell_wr64(d, r->doorbell, r->wptr);
        return;
    }
    if (r->kind != RD_RING_GFX) {
        RERR(d, "GFX: a MEC queue needs a doorbell");
        return;
    }
    WG(d, regCP_RB0_WPTR, (uint32_t)r->wptr);
    WG(d, regCP_RB0_WPTR_HI, (uint32_t)(r->wptr >> 32));
}

void gfx11_cp_gfx_enable(rdna3_dev *d, int enable)
{
    uint32_t i, tmp = RG(d, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, enable ? 0 : 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
    WG(d, regCP_ME_CNTL, tmp);
    for (i = 0; i < d->usec_timeout; i++) {
        if (RG(d, regCP_STAT) == 0)
            return;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GFX: failed to %s cp gfx", enable ? "unhalt" : "halt");
}

/* gfx_v11_0_cp_gfx_resume for ring 0 */
static void cp_gfx_resume(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint32_t tmp, rb_bufsz = order_base_2(r->ring_size / 8);
    uint64_t rb = rdna3_vram_mc(d, r->ring);
    uint64_t rptr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_RPTR);
    uint64_t wpoll = rdna3_vram_mc(d, r->wb + RD_GFX_WB_WPTR);

    WG(d, regCP_RB_WPTR_DELAY, 0);
    WG(d, regCP_RB_VMID, 0);
    tmp = RG(d, regGRBM_GFX_CNTL);
    tmp = REG_SET_FIELD(tmp, GRBM_GFX_CNTL, PIPEID, 0);
    WG(d, regGRBM_GFX_CNTL, tmp);

    tmp = REG_SET_FIELD(0u, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
    tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ, rb_bufsz - 2);
    tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, KMD_QUEUE, 1);
    WG(d, regCP_RB0_CNTL, tmp);
    r->wptr = 0;
    WG(d, regCP_RB0_WPTR, 0);
    WG(d, regCP_RB0_WPTR_HI, 0);
    WG(d, regCP_RB0_RPTR_ADDR, (uint32_t)rptr);
    WG(d, regCP_RB0_RPTR_ADDR_HI, (uint32_t)(rptr >> 32) & CP_RB0_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);
    WG(d, regCP_RB_WPTR_POLL_ADDR_LO, (uint32_t)wpoll);
    WG(d, regCP_RB_WPTR_POLL_ADDR_HI, (uint32_t)(wpoll >> 32));
    d->os.msleep(d->os.ctx, 1);
    WG(d, regCP_RB0_CNTL, tmp);
    WG(d, regCP_RB0_BASE, (uint32_t)(rb >> 8));
    WG(d, regCP_RB0_BASE_HI, (uint32_t)(rb >> 40));
    WG(d, regCP_RB_ACTIVE, 1);
    /* gfx_v11_0_cp_gfx_set_doorbell */
    r->use_doorbell = d->doorbell_ok;
    r->doorbell = RD_DB_INDEX(RD_DB_GFX_RING0);
    tmp = RG(d, regCP_RB_DOORBELL_CONTROL);
    if (r->use_doorbell) {
        tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, r->doorbell);
        tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
    } else {
        tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
    }
    WG(d, regCP_RB_DOORBELL_CONTROL, tmp);
}

/* gfx_v11_0_cp_set_doorbell_range */
static void cp_set_doorbell_range(rdna3_dev *d)
{
    WG(d, regCP_RB_DOORBELL_RANGE_LOWER, RD_DB_INDEX(RD_DB_GFX_RING0) << 2);
    WG(d, regCP_RB_DOORBELL_RANGE_UPPER, RD_DB_INDEX(RD_DB_GFX_USERQUEUE_END) << 2);
    WG(d, regCP_MEC_DOORBELL_RANGE_LOWER, RD_DB_INDEX(RD_DB_KIQ) << 2);
    WG(d, regCP_MEC_DOORBELL_RANGE_UPPER, RD_DB_INDEX(RD_DB_USERQUEUE_END) << 2);
}

/* gfx_v11_0_cp_gfx_start */
static void cp_gfx_start(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint32_t n, i;
    WG(d, regCP_MAX_CONTEXT, MAX_HW_CONTEXTS - 1);
    WG(d, regCP_DEVICE_ID, 1);
    gfx11_cp_gfx_enable(d, 1);
    n = gfx11_csb_fill(g_csb, 1024);
    for (i = 0; i < n; i++)
        gfx11_write(d, r, g_csb[i]);
    gfx11_commit(d, r);
}

int gfx11_ring_test(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint32_t scratch = OFF(d, regSCRATCH_REG0), i, v = 0;
    rdna3_wreg(d, scratch, 0xCAFEDEAD);
    gfx11_write(d, r, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
    gfx11_write(d, r, scratch - PACKET3_SET_UCONFIG_REG_START);
    gfx11_write(d, r, 0xDEADBEEF);
    gfx11_commit(d, r);
    for (i = 0; i < d->usec_timeout; i++) {
        v = rdna3_rreg(d, scratch);
        if (v == 0xDEADBEEF)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GFX: ring test timeout (SCRATCH_REG0 0x%08x, CP_STAT 0x%08x, GRBM_STATUS 0x%08x)",
         v, RG(d, regCP_STAT), RG(d, regGRBM_STATUS));
    return RDNA3_ETIME;
}

uint32_t gfx11_ring_space(rdna3_dev *d, rdna3_gfx_ring *r)
{
    /* gfx11 rptr is 32 bit (gfx_v11_0_ring_get_rptr_gfx) */
    uint32_t rptr = wb_read32(d, r, RD_GFX_WB_RPTR) & r->buf_mask;
    uint32_t used = ((uint32_t)r->wptr - rptr) & r->buf_mask;
    return r->buf_mask - used;   /* one slot stays empty: full != empty */
}

/* amdgpu_ring_alloc: wait (bounded) until 'ndw' dwords fit */
static int ring_wait_space(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t ndw)
{
    uint32_t i;
    if (ndw > r->buf_mask)
        return RDNA3_EINVAL;
    for (i = 0; i < d->usec_timeout; i++) {
        if (gfx11_ring_space(d, r) >= ndw)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GFX: ring full (rptr 0x%x, wptr 0x%llx)", wb_read32(d, r, RD_GFX_WB_RPTR),
         (unsigned long long)r->wptr);
    return RDNA3_ETIME;
}

/* gfx_v11_0_ring_emit_ib_gfx (no MCBP preemption) */
void gfx11_emit_ib(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t vmid, uint64_t va, uint32_t ndw)
{
    gfx11_write(d, r, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
    gfx11_write(d, r, (uint32_t)va & ~3u);
    gfx11_write(d, r, (uint32_t)(va >> 32));
    gfx11_write(d, r, ndw | (vmid << 24));
}

/* gfx_v11_0_ring_emit_wreg: WRITE_DATA from the PFP (gfx), the ME
 * (compute) or with "no inc addr" (KIQ) */
void gfx11_emit_wreg(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t reg, uint32_t val)
{
    uint32_t cmd = r->kind == RD_RING_GFX ? WRITE_DATA_ENGINE_SEL(1) | WR_CONFIRM
                 : r->kind == RD_RING_KIQ ? (1u << 16) : WR_CONFIRM;
    gfx11_write(d, r, PACKET3(PACKET3_WRITE_DATA, 3));
    gfx11_write(d, r, cmd);
    gfx11_write(d, r, reg);
    gfx11_write(d, r, 0);
    gfx11_write(d, r, val);
}

/* gfx_v11_0_wait_reg_mem */
void gfx11_emit_wait_reg_mem(rdna3_dev *d, rdna3_gfx_ring *r, int eng_sel, int mem_space, int opt,
                             uint32_t addr0, uint32_t addr1, uint32_t ref, uint32_t mask,
                             uint32_t inv)
{
    gfx11_write(d, r, PACKET3(PACKET3_WAIT_REG_MEM, 5));
    gfx11_write(d, r, WAIT_REG_MEM_MEM_SPACE(mem_space) | WAIT_REG_MEM_OPERATION(opt) |
                      WAIT_REG_MEM_FUNCTION(3) /* equal */ | WAIT_REG_MEM_ENGINE(eng_sel));
    gfx11_write(d, r, addr0);
    gfx11_write(d, r, addr1);
    gfx11_write(d, r, ref);
    gfx11_write(d, r, mask);
    gfx11_write(d, r, inv);
}

/* gfx_v11_0_ring_emit_vm_flush -> gmc_v11_0_emit_flush_gpu_tlb (GFXHUB: no
 * semaphore) + PFP_SYNC_ME */
void gfx11_emit_vm_flush(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t vmid, uint64_t pd_addr)
{
    rdna3_vm_flush_regs f;
    gfxhub303_flush_regs(d, vmid, RD_GFX_VM_INV_ENG, &f);
    gfx11_emit_wreg(d, r, f.pt_lo, (uint32_t)pd_addr);
    gfx11_emit_wreg(d, r, f.pt_hi, (uint32_t)(pd_addr >> 32));
    /* gfx_v11_0_ring_emit_reg_write_reg_wait: write req, wait for the ack
     * bit (usepfp only on the gfx ring) */
    gfx11_emit_wait_reg_mem(d, r, r->kind == RD_RING_GFX ? 1 : 0, 0, 1, f.req, f.ack, f.req_val,
                            1u << vmid, 0x20);
    if (r->kind == RD_RING_GFX) {   /* compute doesn't have PFP */
        gfx11_write(d, r, PACKET3(PACKET3_PFP_SYNC_ME, 0));
        gfx11_write(d, r, 0);
    }
}

/* gfx_v11_0_ring_emit_hdp_flush: kick the NBIO HDP flush engine for this
 * CP pipe (amdgpu_gfx_get_hdp_flush_mask: CP0 << pipe for gfx from the PFP,
 * CP2 << pipe for compute, CP9 for the KIQ) and wait for its done bit */
void gfx11_emit_hdp_flush(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint32_t m = r->kind == RD_RING_GFX ? BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP0_MASK << r->pipe
               : r->kind == RD_RING_KIQ ? BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP9_MASK
               : BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP2_MASK << r->pipe;
    gfx11_emit_wait_reg_mem(d, r, r->kind == RD_RING_GFX ? 1 : 0, 0, 1,
                            SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_GPU_HDP_FLUSH_REQ),
                            SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_GPU_HDP_FLUSH_DONE), m, m, 0x20);
}

/* gfx_v11_0_emit_mem_sync: ACQUIRE_MEM invalidating/writing back GL2, GLM,
 * GL1, GLV, GLK, GLI over the whole address space */
void gfx11_emit_mem_sync(rdna3_dev *d, rdna3_gfx_ring *r)
{
    const uint32_t gcr_cntl = (1u << 14) /* GL2_INV */ | (1u << 15) /* GL2_WB */ |
                              (1u << 5) /* GLM_INV */ | (1u << 4) /* GLM_WB */ |
                              (1u << 9) /* GL1_INV */ | (1u << 8) /* GLV_INV */ |
                              (1u << 7) /* GLK_INV */ | (1u << 0) /* GLI_INV */;
    gfx11_write(d, r, PACKET3(PACKET3_ACQUIRE_MEM, 6));
    gfx11_write(d, r, 0);            /* CP_COHER_CNTL */
    gfx11_write(d, r, 0xffffffff);   /* CP_COHER_SIZE */
    gfx11_write(d, r, 0xffffff);     /* CP_COHER_SIZE_HI */
    gfx11_write(d, r, 0);            /* CP_COHER_BASE */
    gfx11_write(d, r, 0);            /* CP_COHER_BASE_HI */
    gfx11_write(d, r, 0x0000000A);   /* POLL_INTERVAL */
    gfx11_write(d, r, gcr_cntl);
}

/* gfx_v11_0_ring_emit_cntxcntl */
void gfx11_emit_cntxcntl(rdna3_dev *d, rdna3_gfx_ring *r, int ctx_switch)
{
    uint32_t dw2 = 0x80000000;   /* load_enable, otherwise the packet is a NOP */
    if (ctx_switch)
        dw2 |= 0x8001 /* global config + uconfig */ | 0x01000000 /* cs sh regs */ |
               0x10002 /* per-context state + gfx sh regs */;
    gfx11_write(d, r, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
    gfx11_write(d, r, dw2);
    gfx11_write(d, r, 0);
}

/* gfx_v11_0_ring_emit_frame_cntl */
void gfx11_emit_frame_cntl(rdna3_dev *d, rdna3_gfx_ring *r, int start, int secure)
{
    gfx11_write(d, r, PACKET3(PACKET3_FRAME_CONTROL, 0));
    gfx11_write(d, r, (secure ? FRAME_TMZ : 0) | FRAME_CMD(start ? 0 : 1));
}

uint32_t gfx11_submit_job(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ctx, uint32_t vmid,
                          uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n)
{
    /* emit_frame_size for what we emit: vm flush 5+5+7+2, mem sync 8,
     * hdp 7, cntxcntl 3, frame 2+2, fence 8, plus 4 per IB */
    uint32_t i, seq, ndw = 19 + 8 + 7 + 3 + 4 + 8 + 4 * n;
    int ctx_switch = ctx != r->last_ctx, secure;
    if (!r->ready || !n || n > RD_GFX_MAX_IBS || vmid > 15)
        return 0;
    if (ring_wait_space(d, r, ndw))
        return 0;
    if (pd_addr)
        gfx11_emit_vm_flush(d, r, vmid, pd_addr);
    gfx11_emit_mem_sync(d, r);
    gfx11_emit_hdp_flush(d, r);
    gfx11_emit_cntxcntl(d, r, ctx_switch);
    secure = (ibs[0].flags & RD_GFX_IB_FLAG_TMZ) != 0;
    gfx11_emit_frame_cntl(d, r, 1, secure);
    for (i = 0; i < n; i++)
        gfx11_emit_ib(d, r, vmid, ibs[i].va, ibs[i].ndw);
    gfx11_emit_frame_cntl(d, r, 0, secure);
    seq = gfx11_emit_fence(d, r);
    gfx11_commit(d, r);
    r->last_ctx = ctx;
    return seq;
}

uint32_t gfx11_emit_fence(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint64_t addr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_FENCE);
    uint32_t seq = ++r->fence_seq;
    gfx11_write(d, r, PACKET3(PACKET3_RELEASE_MEM, 6));
    gfx11_write(d, r, (1u << 22) /* GCR_SEQ */ | (1u << 21) /* GCR_GL2_WB */ |
                      (1u << 13) /* GCR_GLM_INV */ | (1u << 12) /* GCR_GLM_WB */ |
                      (3u << 25) /* CACHE_POLICY bypass */ |
                      CACHE_FLUSH_AND_INV_TS_EVENT | (5u << 8) /* EVENT_INDEX */);
    /* DATA_SEL 1: low 32 bits; INT_SEL 2: interrupt once the write landed */
    gfx11_write(d, r, (1u << 29) | (r->fence_irq ? 2u << 24 : 0));
    gfx11_write(d, r, (uint32_t)addr);
    gfx11_write(d, r, (uint32_t)(addr >> 32));
    gfx11_write(d, r, seq);
    gfx11_write(d, r, 0);
    gfx11_write(d, r, 0);
    return seq;
}

void gfx11_set_eop_interrupt(rdna3_dev *d, rdna3_gfx_ring *r, int enable)
{
    uint32_t tmp;
    if (r->kind == RD_RING_GFX) {   /* gfx_v11_0_set_gfx_eop_interrupt_state, me 0 pipe 0 */
        tmp = RG(d, regCP_INT_CNTL_RING0);
        tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, TIME_STAMP_INT_ENABLE, enable ? 1 : 0);
        tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GENERIC0_INT_ENABLE, enable ? 1 : 0);
        WG(d, regCP_INT_CNTL_RING0, tmp);
    } else {                        /* gfx_v11_0_set_compute_eop_interrupt_state, me 1 */
        uint32_t reg = OFF(d, regCP_ME1_PIPE0_INT_CNTL) + r->pipe;   /* PIPE0..3 are consecutive */
        tmp = rdna3_rreg(d, reg);
        tmp = REG_SET_FIELD(tmp, CP_ME1_PIPE0_INT_CNTL, TIME_STAMP_INT_ENABLE, enable ? 1 : 0);
        tmp = REG_SET_FIELD(tmp, CP_ME1_PIPE0_INT_CNTL, GENERIC0_INT_ENABLE, enable ? 1 : 0);
        rdna3_wreg(d, reg, tmp);
    }
    r->fence_irq = enable;
}

int gfx11_fence_passed(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq)
{
    return (int32_t)(wb_read32(d, r, RD_GFX_WB_FENCE) - seq) >= 0;
}

int gfx11_wait_fence(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq)
{
    uint32_t i;
    for (i = 0; i < d->usec_timeout; i++) {
        if (gfx11_fence_passed(d, r, seq))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GFX: fence %u timeout (last %u)", seq, wb_read32(d, r, RD_GFX_WB_FENCE));
    return RDNA3_ETIME;
}

/* ---------------------------- hw_init ---------------------------- */

int gfx11_hw_init(rdna3_dev *d, rdna3_gmc *g, rdna3_gfx_ring *r, const rdna3_rs64_start *rs64,
                  uint32_t ring_size)
{
    uint32_t tmp, csb_dw = gfx11_csb_size();
    uint8_t zero[64] = { 0 };
    uint64_t csb_mc;
    int ret;

    if (!d->ipver[HWIP_GC].present || d->ipver[HWIP_GC].major != 11)
        return RDNA3_ENODEV;
    if (!ring_size)
        ring_size = RD_GFX_RING_SIZE;
    if ((ring_size & (ring_size - 1)) || !csb_dw || csb_dw > 1024)
        return RDNA3_EINVAL;

    r->kind = RD_RING_GFX;
    r->pipe = 0;
    r->ring_size = ring_size;
    r->buf_mask = ring_size / 4 - 1;
    r->wptr = 0;
    r->fence_seq = 0;
    r->ready = 0;
    r->last_ctx = 0;
    r->csb_dw = csb_dw;
    r->ring = rdna3_vram_alloc(d, ring_size, 4096, "gfx-ring");
    r->wb = rdna3_vram_alloc(d, 4096, 4096, "gfx-wb");
    r->csb = rdna3_vram_alloc(d, (csb_dw * 4 + 4095) & ~4095u, 4096, "gfx-csb");
    if (r->ring == UINT64_MAX || r->wb == UINT64_MAX || r->csb == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, r->ring, 0, ring_size);
    rdna3_vram_write(d, r->wb, zero, sizeof(zero));

    ret = wait_rlc_autoload(d);
    if (ret)
        return ret;
    if (rs64)
        config_gfx_rs64(d, rs64);
    ret = gfxhub_enable(d, g);
    if (ret)
        return ret;
    golden_registers(d);
    constants_init(d);
    if (d->doorbell_ok) {
        nbio43_gc_doorbell_init(d);   /* gfx_v11_0_hw_init */
        cp_set_doorbell_range(d);     /* gfx_v11_0_cp_resume */
    }

    /* gfx_v11_0_rlc_resume (PSP): init_csb + enable SRM */
    {
        uint32_t n = gfx11_csb_fill(g_csb, 1024);
        rdna3_vram_write(d, r->csb, g_csb, 4ull * n);
        rdna3_hdp_flush(d);
    }
    csb_mc = rdna3_vram_mc(d, r->csb);
    WG(d, regRLC_CSIB_ADDR_HI, (uint32_t)(csb_mc >> 32));
    WG(d, regRLC_CSIB_ADDR_LO, (uint32_t)csb_mc & 0xfffffffc);
    WG(d, regRLC_CSIB_LENGTH, csb_dw);
    tmp = RG(d, regRLC_SRM_CNTL);
    tmp |= RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK | RLC_SRM_CNTL__SRM_ENABLE_MASK;
    WG(d, regRLC_SRM_CNTL, tmp);

    cp_gfx_resume(d, r);
    cp_gfx_start(d, r);
    ret = gfx11_ring_test(d, r);
    if (ret)
        return ret;
    r->ready = 1;
    RLOG(d, "GFX: ring 0 %u KiB at MC 0x%llx, clear state %u dw, ring test passed",
         ring_size / 1024, (unsigned long long)rdna3_vram_mc(d, r->ring), csb_dw);
    return RDNA3_OK;
}

/* ------------------------- MEC queues (KIQ, KCQ) ------------------------- */

/* v11_compute_mqd dword offsets (v11_structs.h) */
#define MQD_HEADER                     0
#define MQD_COMPUTE_PIPELINESTAT_ENABLE 11
#define MQD_COMPUTE_STATIC_THREAD_MGMT_SE0 23
#define MQD_COMPUTE_STATIC_THREAD_MGMT_SE1 24
#define MQD_COMPUTE_STATIC_THREAD_MGMT_SE2 26
#define MQD_COMPUTE_STATIC_THREAD_MGMT_SE3 27
#define MQD_COMPUTE_MISC_RESERVED      32
#define MQD_CP_MQD_BASE_ADDR_LO        128
#define MQD_CP_MQD_BASE_ADDR_HI        129
#define MQD_CP_HQD_ACTIVE              130
#define MQD_CP_HQD_VMID                131
#define MQD_CP_HQD_PERSISTENT_STATE    132
#define MQD_CP_HQD_PIPE_PRIORITY       133
#define MQD_CP_HQD_QUEUE_PRIORITY      134
#define MQD_CP_HQD_QUANTUM             135
#define MQD_CP_HQD_PQ_BASE_LO          136
#define MQD_CP_HQD_PQ_BASE_HI          137
#define MQD_CP_HQD_PQ_RPTR             138
#define MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_LO 139
#define MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_HI 140
#define MQD_CP_HQD_PQ_WPTR_POLL_ADDR_LO 141
#define MQD_CP_HQD_PQ_WPTR_POLL_ADDR_HI 142
#define MQD_CP_HQD_PQ_DOORBELL_CONTROL 143
#define MQD_CP_HQD_PQ_CONTROL          145
#define MQD_CP_HQD_IB_CONTROL          149
#define MQD_CP_HQD_DEQUEUE_REQUEST     152
#define MQD_CP_MQD_CONTROL             162
#define MQD_CP_HQD_EOP_BASE_ADDR_LO    165
#define MQD_CP_HQD_EOP_BASE_ADDR_HI    166
#define MQD_CP_HQD_EOP_CONTROL         167
#define MQD_CP_HQD_PQ_WPTR_LO          182
#define MQD_CP_HQD_PQ_WPTR_HI          183

static uint32_t g_mqd[RD_MQD_DWORDS];   /* staging, bring-up is single threaded */

int gfx11_mec_ring_init(rdna3_dev *d, rdna3_mec_ring *r, int kiq, uint32_t pipe, uint32_t queue,
                        uint32_t doorbell_assignment, uint32_t ring_size)
{
    uint8_t zero[64] = { 0 };
    rdna3_gfx_ring *rb = &r->rb;
    if (!ring_size)
        ring_size = 1024 * 4;   /* amdgpu_ring_init(..., 1024 dw) for KIQ/KCQ */
    if ((ring_size & (ring_size - 1)) || pipe > 3 || queue > 3)
        return RDNA3_EINVAL;
    r->me = 1;
    r->pipe = pipe;
    r->queue = queue;
    r->mapped = 0;
    rb->kind = kiq ? RD_RING_KIQ : RD_RING_COMPUTE;
    rb->pipe = pipe;
    rb->use_doorbell = 1;
    rb->doorbell = RD_DB_INDEX(doorbell_assignment);
    rb->ring_size = ring_size;
    rb->buf_mask = ring_size / 4 - 1;
    rb->wptr = 0;
    rb->fence_seq = 0;
    rb->ready = 0;
    rb->last_ctx = 0;
    rb->fence_irq = 0;
    rb->csb = UINT64_MAX;
    rb->csb_dw = 0;
    rb->ring = rdna3_vram_alloc(d, ring_size, 4096, kiq ? "kiq-ring" : "kcq-ring");
    rb->wb = rdna3_vram_alloc(d, 4096, 4096, kiq ? "kiq-wb" : "kcq-wb");
    r->mqd = rdna3_vram_alloc(d, 4096, 4096, kiq ? "kiq-mqd" : "kcq-mqd");
    r->eop = rdna3_vram_alloc(d, RD_MEC_HPD_SIZE, 4096, kiq ? "kiq-eop" : "kcq-eop");
    if (rb->ring == UINT64_MAX || rb->wb == UINT64_MAX || r->mqd == UINT64_MAX || r->eop == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, rb->ring, 0, ring_size);
    rdna3_vram_write(d, rb->wb, zero, sizeof(zero));
    rdna3_vram_fill(d, r->mqd, 0, 4096);
    rdna3_vram_fill(d, r->eop, 0, RD_MEC_HPD_SIZE);
    return RDNA3_OK;
}

/* gfx_v11_0_cp_compute_enable (rs64) */
static void cp_compute_enable(rdna3_dev *d, int enable)
{
    uint32_t v = RG(d, regCP_MEC_RS64_CNTL);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_INVALIDATE_ICACHE, enable ? 0 : 1);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, enable ? 0 : 1);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, enable ? 0 : 1);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, enable ? 0 : 1);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, enable ? 0 : 1);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE0_ACTIVE, enable ? 1 : 0);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE1_ACTIVE, enable ? 1 : 0);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE2_ACTIVE, enable ? 1 : 0);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_PIPE3_ACTIVE, enable ? 1 : 0);
    v = REG_SET_FIELD(v, CP_MEC_RS64_CNTL, MEC_HALT, enable ? 0 : 1);
    WG(d, regCP_MEC_RS64_CNTL, v);
    d->os.udelay(d->os.ctx, 50);
}

/* gfx_v11_0_compute_mqd_init (amdgpu_ring_to_mqd_prop: kernel queue,
 * doorbell, hqd_active only for the KIQ, default priorities) */
static void compute_mqd_init(rdna3_dev *d, rdna3_mec_ring *r)
{
    rdna3_gfx_ring *rb = &r->rb;
    uint64_t eop = rdna3_vram_mc(d, r->eop) >> 8, mqd = rdna3_vram_mc(d, r->mqd);
    uint64_t hqd = rdna3_vram_mc(d, rb->ring) >> 8, wb;
    uint32_t tmp, i;

    for (i = 0; i < RD_MQD_DWORDS; i++)
        g_mqd[i] = 0;
    g_mqd[MQD_HEADER] = 0xC0310800;
    g_mqd[MQD_COMPUTE_PIPELINESTAT_ENABLE] = 1;
    g_mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE0] = 0xffffffff;
    g_mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE1] = 0xffffffff;
    g_mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE2] = 0xffffffff;
    g_mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE3] = 0xffffffff;
    g_mqd[MQD_COMPUTE_MISC_RESERVED] = 7;
    g_mqd[MQD_CP_HQD_EOP_BASE_ADDR_LO] = (uint32_t)eop;
    g_mqd[MQD_CP_HQD_EOP_BASE_ADDR_HI] = (uint32_t)(eop >> 32);
    /* EOP size: register value is 2^(EOP_SIZE+1) dwords */
    tmp = regCP_HQD_EOP_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE, order_base_2(RD_MEC_HPD_SIZE / 4) - 1);
    g_mqd[MQD_CP_HQD_EOP_CONTROL] = tmp;
    tmp = regCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, rb->doorbell);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_SOURCE, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_HIT, 0);
    g_mqd[MQD_CP_HQD_PQ_DOORBELL_CONTROL] = tmp;
    g_mqd[MQD_CP_HQD_DEQUEUE_REQUEST] = 0;
    g_mqd[MQD_CP_HQD_PQ_RPTR] = 0;
    g_mqd[MQD_CP_HQD_PQ_WPTR_LO] = 0;
    g_mqd[MQD_CP_HQD_PQ_WPTR_HI] = 0;
    g_mqd[MQD_CP_MQD_BASE_ADDR_LO] = (uint32_t)mqd & 0xfffffffc;
    g_mqd[MQD_CP_MQD_BASE_ADDR_HI] = (uint32_t)(mqd >> 32);
    g_mqd[MQD_CP_MQD_CONTROL] = REG_SET_FIELD(regCP_MQD_CONTROL_DEFAULT, CP_MQD_CONTROL, VMID, 0);
    g_mqd[MQD_CP_HQD_PQ_BASE_LO] = (uint32_t)hqd;
    g_mqd[MQD_CP_HQD_PQ_BASE_HI] = (uint32_t)(hqd >> 32);
    tmp = regCP_HQD_PQ_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE, order_base_2(rb->ring_size / 4) - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, order_base_2(4096 / 4) - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    g_mqd[MQD_CP_HQD_PQ_CONTROL] = tmp;
    wb = rdna3_vram_mc(d, rb->wb + RD_GFX_WB_RPTR);
    g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_LO] = (uint32_t)wb & 0xfffffffc;
    g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_HI] = (uint32_t)(wb >> 32) & 0xffff;
    wb = rdna3_vram_mc(d, rb->wb + RD_GFX_WB_WPTR);
    g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_LO] = (uint32_t)wb & 0xfffffffc;
    g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_HI] = (uint32_t)(wb >> 32) & 0xffff;
    g_mqd[MQD_CP_HQD_VMID] = 0;
    g_mqd[MQD_CP_HQD_PERSISTENT_STATE] =
        REG_SET_FIELD(regCP_HQD_PERSISTENT_STATE_DEFAULT, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    g_mqd[MQD_CP_HQD_IB_CONTROL] =
        REG_SET_FIELD(regCP_HQD_IB_CONTROL_DEFAULT, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
    g_mqd[MQD_CP_HQD_PIPE_PRIORITY] = 0;
    g_mqd[MQD_CP_HQD_QUEUE_PRIORITY] = 0;
    tmp = REG_SET_FIELD(0u, CP_HQD_QUANTUM, QUANTUM_EN, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_SCALE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_DURATION, 1);
    g_mqd[MQD_CP_HQD_QUANTUM] = tmp;
    g_mqd[MQD_CP_HQD_ACTIVE] = rb->kind == RD_RING_KIQ ? 1 : 0;
    rdna3_vram_write(d, r->mqd, g_mqd, sizeof(g_mqd));
    rdna3_hdp_flush(d);
}

/* gfx_v11_0_kiq_init_register: HQD registers from the MQD, under the
 * queue's GRBM selection */
static int kiq_init_register(rdna3_dev *d, rdna3_mec_ring *r)
{
    uint32_t tmp, i;
    int ret = RDNA3_OK;

    grbm_select(d, r->me, r->pipe, r->queue, 0);
    tmp = RG(d, regCP_PQ_WPTR_POLL_CNTL);
    WG(d, regCP_PQ_WPTR_POLL_CNTL, REG_SET_FIELD(tmp, CP_PQ_WPTR_POLL_CNTL, EN, 0));
    WG(d, regCP_HQD_EOP_BASE_ADDR, g_mqd[MQD_CP_HQD_EOP_BASE_ADDR_LO]);
    WG(d, regCP_HQD_EOP_BASE_ADDR_HI, g_mqd[MQD_CP_HQD_EOP_BASE_ADDR_HI]);
    WG(d, regCP_HQD_EOP_CONTROL, g_mqd[MQD_CP_HQD_EOP_CONTROL]);
    WG(d, regCP_HQD_PQ_DOORBELL_CONTROL, g_mqd[MQD_CP_HQD_PQ_DOORBELL_CONTROL]);
    if (RG(d, regCP_HQD_ACTIVE) & 1) {   /* disable the queue if it's active */
        WG(d, regCP_HQD_DEQUEUE_REQUEST, 1);
        for (i = 0; i < d->usec_timeout; i++) {
            if (!(RG(d, regCP_HQD_ACTIVE) & 1))
                break;
            d->os.udelay(d->os.ctx, 1);
        }
        if (i >= d->usec_timeout)
            ret = RDNA3_ETIME;
        WG(d, regCP_HQD_DEQUEUE_REQUEST, g_mqd[MQD_CP_HQD_DEQUEUE_REQUEST]);
        WG(d, regCP_HQD_PQ_RPTR, g_mqd[MQD_CP_HQD_PQ_RPTR]);
        WG(d, regCP_HQD_PQ_WPTR_LO, g_mqd[MQD_CP_HQD_PQ_WPTR_LO]);
        WG(d, regCP_HQD_PQ_WPTR_HI, g_mqd[MQD_CP_HQD_PQ_WPTR_HI]);
    }
    WG(d, regCP_MQD_BASE_ADDR, g_mqd[MQD_CP_MQD_BASE_ADDR_LO]);
    WG(d, regCP_MQD_BASE_ADDR_HI, g_mqd[MQD_CP_MQD_BASE_ADDR_HI]);
    WG(d, regCP_MQD_CONTROL, g_mqd[MQD_CP_MQD_CONTROL]);
    WG(d, regCP_HQD_PQ_BASE, g_mqd[MQD_CP_HQD_PQ_BASE_LO]);
    WG(d, regCP_HQD_PQ_BASE_HI, g_mqd[MQD_CP_HQD_PQ_BASE_HI]);
    WG(d, regCP_HQD_PQ_CONTROL, g_mqd[MQD_CP_HQD_PQ_CONTROL]);
    WG(d, regCP_HQD_PQ_RPTR_REPORT_ADDR, g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_LO]);
    WG(d, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_HI]);
    WG(d, regCP_HQD_PQ_WPTR_POLL_ADDR, g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_LO]);
    WG(d, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_HI]);
    WG(d, regCP_MEC_DOORBELL_RANGE_LOWER, RD_DB_INDEX(RD_DB_KIQ) << 2);
    WG(d, regCP_MEC_DOORBELL_RANGE_UPPER, RD_DB_INDEX(RD_DB_USERQUEUE_END) << 2);
    WG(d, regCP_HQD_PQ_DOORBELL_CONTROL, g_mqd[MQD_CP_HQD_PQ_DOORBELL_CONTROL]);
    WG(d, regCP_HQD_PQ_WPTR_LO, g_mqd[MQD_CP_HQD_PQ_WPTR_LO]);
    WG(d, regCP_HQD_PQ_WPTR_HI, g_mqd[MQD_CP_HQD_PQ_WPTR_HI]);
    WG(d, regCP_HQD_VMID, g_mqd[MQD_CP_HQD_VMID]);
    WG(d, regCP_HQD_PERSISTENT_STATE, g_mqd[MQD_CP_HQD_PERSISTENT_STATE]);
    WG(d, regCP_HQD_ACTIVE, g_mqd[MQD_CP_HQD_ACTIVE]);
    tmp = RG(d, regCP_PQ_STATUS);
    WG(d, regCP_PQ_STATUS, REG_SET_FIELD(tmp, CP_PQ_STATUS, DOORBELL_ENABLE, 1));
    grbm_select(d, 0, 0, 0, 0);
    return ret;
}

/* gfx_v11_0_ring_test_ring for the KIQ: SCRATCH_REG0 through WRITE_DATA */
static int kiq_ring_test(rdna3_dev *d, rdna3_mec_ring *kiq)
{
    uint32_t scratch = OFF(d, regSCRATCH_REG0), i, v = 0;
    rdna3_wreg(d, scratch, 0xCAFEDEAD);
    gfx11_emit_wreg(d, &kiq->rb, scratch, 0xDEADBEEF);
    gfx11_commit(d, &kiq->rb);
    for (i = 0; i < d->usec_timeout; i++) {
        v = rdna3_rreg(d, scratch);
        if (v == 0xDEADBEEF)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "KIQ: ring test timeout (SCRATCH_REG0 0x%08x, CP_STAT 0x%08x)", v, RG(d, regCP_STAT));
    return RDNA3_ETIME;
}

int gfx11_kiq_start(rdna3_dev *d, rdna3_mec_ring *kiq)
{
    uint32_t tmp;
    int ret;
    if (!d->doorbell_ok || kiq->rb.kind != RD_RING_KIQ)
        return RDNA3_EINVAL;
    cp_compute_enable(d, 1);
    /* gfx_v11_0_kiq_setting: tell the RLC which queue is the KIQ */
    tmp = RG(d, regRLC_CP_SCHEDULERS) & 0xffffff00;
    tmp |= (kiq->me << 5) | (kiq->pipe << 3) | kiq->queue;
    WG(d, regRLC_CP_SCHEDULERS, tmp | 0x80);
    /* gfx_v11_0_kiq_init_queue */
    compute_mqd_init(d, kiq);
    ret = kiq_init_register(d, kiq);
    if (ret)
        RERR(d, "KIQ: the HQD would not dequeue");
    kiq->mapped = 1;
    kiq->rb.wptr = 0;
    ret = kiq_ring_test(d, kiq);
    if (ret)
        return ret;
    kiq->rb.ready = 1;
    RLOG(d, "KIQ: me %u pipe %u queue %u up, doorbell 0x%x, ring test passed", kiq->me, kiq->pipe,
         kiq->queue, kiq->rb.doorbell);
    return RDNA3_OK;
}

/* gfx11_kiq_set_resources + gfx11_kiq_map_queues (amdgpu_gfx_enable_kcq) */
static void kiq_map_kcq(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq)
{
    rdna3_gfx_ring *k = &kiq->rb;
    uint64_t mask = 1ull << ((kcq->me - 1) * 32 + kcq->pipe * 8 + kcq->queue);
    uint64_t mqd = rdna3_vram_mc(d, kcq->mqd), wptr = rdna3_vram_mc(d, kcq->rb.wb + RD_GFX_WB_WPTR);
    gfx11_write(d, k, PACKET3(PACKET3_SET_RESOURCES, 6));
    gfx11_write(d, k, (0xau << 16) /* unmap latency ~1 s */ | (0u << 29) /* KIQ queue type */);
    gfx11_write(d, k, (uint32_t)mask);
    gfx11_write(d, k, (uint32_t)(mask >> 32));
    gfx11_write(d, k, 0);   /* cleaner shader addr lo */
    gfx11_write(d, k, 0);   /* hi */
    gfx11_write(d, k, 0);   /* oac mask */
    gfx11_write(d, k, 0);   /* gds heap */
    gfx11_write(d, k, PACKET3(PACKET3_MAP_QUEUES, 5));
    gfx11_write(d, k, (0u << 4) /* QUEUE_SEL */ | (0u << 8) /* VMID */ | (kcq->queue << 13) |
                      (kcq->pipe << 16) | (kcq->me << 18) | (0u << 21) /* compute */ |
                      (0u << 24) /* all on one pipe */ | (0u << 26) /* ENGINE_SEL compute */ |
                      (1u << 29) /* NUM_QUEUES */);
    gfx11_write(d, k, kcq->rb.doorbell << 2);
    gfx11_write(d, k, (uint32_t)mqd);
    gfx11_write(d, k, (uint32_t)(mqd >> 32));
    gfx11_write(d, k, (uint32_t)wptr);
    gfx11_write(d, k, (uint32_t)(wptr >> 32));
    gfx11_commit(d, k);
}

int gfx11_kcq_start(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq)
{
    int ret;
    if (!kiq->rb.ready || kcq->rb.kind != RD_RING_COMPUTE)
        return RDNA3_EINVAL;
    compute_mqd_init(d, kcq);   /* gfx_v11_0_kcq_init_queue: MQD only */
    kcq->rb.wptr = 0;
    kiq_map_kcq(d, kiq, kcq);
    /* amdgpu_gfx_enable_kcq: a KIQ ring test proves the MAP_QUEUES went through */
    ret = kiq_ring_test(d, kiq);
    if (ret) {
        RERR(d, "KCQ: the KIQ did not process MAP_QUEUES");
        return ret;
    }
    kcq->mapped = 1;
    ret = gfx11_ring_test(d, &kcq->rb);
    if (ret)
        return ret;
    kcq->rb.ready = 1;
    RLOG(d, "KCQ: me %u pipe %u queue %u mapped, doorbell 0x%x, ring test passed", kcq->me,
         kcq->pipe, kcq->queue, kcq->rb.doorbell);
    return RDNA3_OK;
}

void gfx11_mec_stop(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq)
{
    if (kiq && kiq->rb.ready && kcq && kcq->mapped) {
        /* gfx11_kiq_unmap_queues(RESET_QUEUES) */
        rdna3_gfx_ring *k = &kiq->rb;
        gfx11_write(d, k, PACKET3(PACKET3_UNMAP_QUEUES, 4));
        gfx11_write(d, k, (2u << 0) /* ACTION reset */ | (0u << 4) | (0u << 26) | (1u << 29));
        gfx11_write(d, k, kcq->rb.doorbell << 2);
        gfx11_write(d, k, 0);
        gfx11_write(d, k, 0);
        gfx11_write(d, k, 0);
        gfx11_commit(d, k);
        (void)kiq_ring_test(d, kiq);
        kcq->mapped = 0;
        kcq->rb.ready = 0;
    }
    cp_compute_enable(d, 0);
    if (kiq) {
        kiq->rb.ready = 0;
        kiq->mapped = 0;
    }
}

uint32_t gfx11_submit_compute_job(rdna3_dev *d, rdna3_mec_ring *kcq, uint64_t ctx, uint32_t vmid,
                                  uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n)
{
    rdna3_gfx_ring *r = &kcq->rb;
    /* vm flush 5+5+7, mem sync 8, hdp 7, fence 8, 4 per IB */
    uint32_t i, seq, ndw = 17 + 8 + 7 + 8 + 4 * n;
    if (!r->ready || !n || n > RD_GFX_MAX_IBS || vmid > 15)
        return 0;
    if (ring_wait_space(d, r, ndw))
        return 0;
    if (pd_addr)
        gfx11_emit_vm_flush(d, r, vmid, pd_addr);
    gfx11_emit_mem_sync(d, r);
    gfx11_emit_hdp_flush(d, r);
    for (i = 0; i < n; i++) {   /* gfx_v11_0_ring_emit_ib_compute */
        gfx11_write(d, r, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
        gfx11_write(d, r, (uint32_t)ibs[i].va & ~3u);
        gfx11_write(d, r, (uint32_t)(ibs[i].va >> 32));
        gfx11_write(d, r, INDIRECT_BUFFER_VALID | ibs[i].ndw | (vmid << 24));
    }
    seq = gfx11_emit_fence(d, r);
    gfx11_commit(d, r);
    r->last_ctx = ctx;
    return seq;
}
