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

/* gfx_v11_0_ring_set_wptr_gfx without doorbell (+ the poll slot) */
void gfx11_commit(rdna3_dev *d, rdna3_gfx_ring *r)
{
    rdna3_vram_write(d, r->wb + RD_GFX_WB_WPTR, &r->wptr, 8);
    rdna3_hdp_flush(d);
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
    /* gfx_v11_0_cp_gfx_set_doorbell(use_doorbell = false) */
    tmp = RG(d, regCP_RB_DOORBELL_CONTROL);
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
    WG(d, regCP_RB_DOORBELL_CONTROL, tmp);
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

/* gfx_v11_0_ring_emit_wreg for the gfx ring: WRITE_DATA from the PFP */
void gfx11_emit_wreg(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t reg, uint32_t val)
{
    gfx11_write(d, r, PACKET3(PACKET3_WRITE_DATA, 3));
    gfx11_write(d, r, WRITE_DATA_ENGINE_SEL(1) | WR_CONFIRM);
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
    /* gfx_v11_0_ring_emit_reg_write_reg_wait: write req, wait for the ack bit */
    gfx11_emit_wait_reg_mem(d, r, 1, 0, 1, f.req, f.ack, f.req_val, 1u << vmid, 0x20);
    gfx11_write(d, r, PACKET3(PACKET3_PFP_SYNC_ME, 0));
    gfx11_write(d, r, 0);
}

/* gfx_v11_0_ring_emit_hdp_flush: kick the NBIO HDP flush engine for CP0
 * (gfx pipe 0) and wait for its done bit, from the PFP */
void gfx11_emit_hdp_flush(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint32_t m = BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP0_MASK;
    gfx11_emit_wait_reg_mem(d, r, 1, 0, 1,
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
    gfx11_write(d, r, 1u << 29);   /* DATA_SEL 1: low 32 bits, no interrupt */
    gfx11_write(d, r, (uint32_t)addr);
    gfx11_write(d, r, (uint32_t)(addr >> 32));
    gfx11_write(d, r, seq);
    gfx11_write(d, r, 0);
    gfx11_write(d, r, 0);
    return seq;
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
