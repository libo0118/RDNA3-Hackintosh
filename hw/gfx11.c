/*
 * gfx11.c - see gfx11.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gfx11.h"
#include "gen/gc_11_0_0_gfx.h"
#include "gen/gc_11_0_0_reset.h"
#include "gen/gc_11_0_0_sdma.h"
#include "gen/gc_11_0_0_gds.h"
#include "gen/gc_11_0_0_rs64.h"
#include "gen/gc_11_0_0_mes.h"
#include "gen/gc_11_0_0_cg.h"
#include "gen/gc_11_0_0_pm.h"
#include "gen/gc_11_0_0_userq.h"
#include "gen/gc_11_0_0_irq.h"
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
#define SHARED_APERTURE_START  0x2000000000000000ull   /* gmc_v11_0_early_init */
#define PRIVATE_APERTURE_START 0x1000000000000000ull
#define CACHE_FLUSH_AND_INV_TS_EVENT 0x14
#define MAX_HW_CONTEXTS 8
#define GFX11_RING_NOP PACKET3(PACKET3_NOP, 0x3FFF)   /* ring->funcs->nop */

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

static void cp_compute_enable(rdna3_dev *d, int enable);

/* gfx_v11_0_gfxhub_enable */
int gfx11_gfxhub_enable(rdna3_dev *d, rdna3_gmc *g)
{
    int r = gfxhub303_gart_enable(d, g);
    if (r)
        return r;
    rdna3_hdp_flush(d);
    gfxhub303_set_fault_enable_default(d, 1);
    return gfxhub303_flush_tlb(d, 0, 0);
}

/* gfx_v11_0_init_golden_registers: golden_settings_gc_11_0 via
 * soc15_program_register_sequence */
static void golden_registers(rdna3_dev *d)
{
    uint32_t and_mask = 0x20000000, or_mask = 0x20000000;
    uint32_t tmp = RG(d, regTCP_CNTL);
    tmp = (tmp & ~and_mask) | (or_mask & and_mask);
    WG(d, regTCP_CNTL, tmp);
}

/* gfx_v11_0_init_gds_vmid: no GDS, GWS or OA for VMIDs 1-15 until a job
 * that owns a partition switches it in (VMID0 keeps access for the HWS) */
static void init_gds_vmid(rdna3_dev *d)
{
    uint32_t vmid;
    for (vmid = 1; vmid < 16; vmid++) {
        rdna3_wreg(d, OFF(d, regGDS_VMID0_BASE) + 2 * vmid, 0);
        rdna3_wreg(d, OFF(d, regGDS_VMID0_SIZE) + 2 * vmid, 0);
        rdna3_wreg(d, OFF(d, regGDS_GWS_VMID0) + vmid, 0);
        rdna3_wreg(d, OFF(d, regGDS_OA_VMID0) + vmid, 0);
    }
}

/* gc_11_0_0_offset.h / _sh_mask.h */
#define regSPI_GDBG_PER_VMID_CNTL                0x1f72
#define regSPI_GDBG_PER_VMID_CNTL_BASE_IDX       0
#define SPI_GDBG_PER_VMID_CNTL__TRAP_EN__SHIFT   0x3
#define SPI_GDBG_PER_VMID_CNTL__TRAP_EN_MASK     0x00000008u
/* gfx_v11_0.c */
#define LDS_APP_BASE           0x1
#define SCRATCH_APP_BASE       0x2

/* Ported from gfx_v11_0_init_compute_vmid (rdna3amdgpu=1): VMIDs
 * first_kfd_vmid..15 get the compute apertures and the trap enabled
 *   LDS:     0x60000000'00000000 - 0x60000001'00000000 (4GB)
 *   Scratch: 0x60000001'00000000 - 0x60000002'00000000 (4GB)
 *   GPUVM:   0x60010000'00000000 - 0x60020000'00000000 (1TB)
 * and no GDS, GWS or OA */
static void gfx_v11_0_init_compute_vmid(rdna3_dev *d)
{
    uint32_t i, data, sh_mem_bases;
    sh_mem_bases = (LDS_APP_BASE << SH_MEM_BASES__SHARED_BASE__SHIFT) | SCRATCH_APP_BASE;
    for (i = RD_FIRST_KFD_VMID; i < 16; i++) {
        grbm_select(d, 0, 0, 0, i);
        /* CP and shaders */
        WG(d, regSH_MEM_CONFIG, DEFAULT_SH_MEM_CONFIG);
        WG(d, regSH_MEM_BASES, sh_mem_bases);
        /* Enable trap for each kfd vmid. */
        data = RG(d, regSPI_GDBG_PER_VMID_CNTL);
        data = REG_SET_FIELD(data, SPI_GDBG_PER_VMID_CNTL, TRAP_EN, 1);
        WG(d, regSPI_GDBG_PER_VMID_CNTL, data);
    }
    grbm_select(d, 0, 0, 0, 0);
    /* Initialize all compute VMIDs to have no GDS, GWS, or OA acccess.
     * These should be enabled by FW for target VMIDs. */
    for (i = RD_FIRST_KFD_VMID; i < 16; i++) {
        rdna3_wreg(d, OFF(d, regGDS_VMID0_BASE) + 2 * i, 0);
        rdna3_wreg(d, OFF(d, regGDS_VMID0_SIZE) + 2 * i, 0);
        rdna3_wreg(d, OFF(d, regGDS_GWS_VMID0) + i, 0);
        rdna3_wreg(d, OFF(d, regGDS_OA_VMID0) + i, 0);
    }
}

/* gfx_v11_0_constants_init, minus what discovery already gave us (RB/CU
 * info). The SH_MEM apertures go to the graphics VMIDs (id_mgr num_ids =
 * first_kfd_vmid) and gfx_v11_0_init_compute_vmid sets up the rest with
 * rdna3amdgpu=1; the verified default gives all 16 the graphics apertures */
static void constants_init(rdna3_dev *d)
{
    uint32_t tmp, i, num_ids = d->amdgpu_exact ? RD_FIRST_KFD_VMID : 16;

    tmp = RG(d, regGRBM_CNTL);
    tmp = REG_SET_FIELD(tmp, GRBM_CNTL, READ_TIMEOUT, 0xff);
    WG(d, regGRBM_CNTL, tmp);
    /* one gfx pipe in use: all occlusion counters to pipe 0 */
    tmp = RG(d, regDB_RING_CONTROL);
    tmp = REG_SET_FIELD(tmp, DB_RING_CONTROL, COUNTER_CONTROL, 1);
    WG(d, regDB_RING_CONTROL, tmp);

    for (i = 0; i < num_ids; i++) {
        grbm_select(d, 0, 0, 0, i);
        WG(d, regSH_MEM_CONFIG, DEFAULT_SH_MEM_CONFIG);
        if (i) {
            tmp = REG_SET_FIELD(0u, SH_MEM_BASES, PRIVATE_BASE, PRIVATE_APERTURE_START >> 48);
            tmp = REG_SET_FIELD(tmp, SH_MEM_BASES, SHARED_BASE, SHARED_APERTURE_START >> 48);
            WG(d, regSH_MEM_BASES, tmp);
        }
    }
    grbm_select(d, 0, 0, 0, 0);
    if (d->amdgpu_exact)
        gfx_v11_0_init_compute_vmid(d);
    init_gds_vmid(d);
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

/* gfx_v11_0_ring_set_wptr_gfx / _compute (mes_v11_0_ring_set_wptr for the MES
 * pipes): poll slot, then the doorbell (dwords) or, gfx only, CP_RB0_WPTR */
void gfx11_commit(rdna3_dev *d, rdna3_gfx_ring *r)
{
    if (r->amdgpu_pad) {
        /* amdgpu_ring_commit: we pad to match fetch size */
        uint32_t count = 0x100 - ((uint32_t)r->wptr & 0xff);
        count &= 0xff;
        if (count != 0)
            gfx11_ring_insert_nop(d, r, count);
    }
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

/* gfx_v11_0_cp_gfx_enable */
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

/* the ring starts over (wptr 0): no SET_Q_PREEMPTION_MODE gate to chain
 * to yet (amdgpu_ring's set_q_mode_offs 0) */
static void shadow_state_reset(rdna3_gfx_ring *r)
{
    r->set_q_mode_offs = 0;
    r->set_q_mode_ptr = 0;
    r->set_q_mode_ptr_valid = 0;
    r->set_q_mode_token = 0;
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
    /* gfx_v11_0_cp_gfx_switch_pipe(PIPE_ID0): init gfx ring 0 for pipe 0 */
    tmp = RG(d, regGRBM_GFX_CNTL);
    tmp = REG_SET_FIELD(tmp, GRBM_GFX_CNTL, PIPEID, 0);
    WG(d, regGRBM_GFX_CNTL, tmp);

    tmp = REG_SET_FIELD(0u, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
    tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ, rb_bufsz - 2);
    WG(d, regCP_RB0_CNTL, tmp);
    r->wptr = 0;
    shadow_state_reset(r);
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
    if (!r->async) {
        gfx11_cp_gfx_enable(d, 1);
        RLOG(d, "GFX: CP unhalted, ME_CNTL 0x%08x CP_STAT 0x%08x", RG(d, regCP_ME_CNTL), RG(d, regCP_STAT));
    }
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
    {
        rdna3_gfx_status st;
        gfx11_read_status(d, r, &st);
        gfx11_log_status(d, &st, "ring test");
        RERR(d, "  RB0_CNTL 0x%08x BASE 0x%08x_%08x RPTR_ADDR 0x%08x_%08x WPTR_POLL 0x%08x_%08x "
             "DOORBELL_CNTL 0x%08x RB_VMID 0x%x ACTIVE 0x%x",
             RG(d, regCP_RB0_CNTL), RG(d, regCP_RB0_BASE_HI), RG(d, regCP_RB0_BASE),
             RG(d, regCP_RB0_RPTR_ADDR_HI), RG(d, regCP_RB0_RPTR_ADDR),
             RG(d, regCP_RB_WPTR_POLL_ADDR_HI), RG(d, regCP_RB_WPTR_POLL_ADDR_LO),
             RG(d, regCP_RB_DOORBELL_CONTROL), RG(d, regCP_RB_VMID), RG(d, regCP_RB_ACTIVE));
        /* RS64: where PSP pointed the PFP/ME instruction and data caches,
         * and where the pipes are executing */
        RERR(d, "  RS64 PFP_IC 0x%08x_%08x (cntl 0x%08x) ME_IC 0x%08x_%08x DC0 0x%08x_%08x "
             "PC_START pfp 0x%08x me 0x%08x INSTR_PNTR pfp 0x%x me 0x%x p0 0x%x p1 0x%x",
             RG(d, regCP_PFP_IC_BASE_HI), RG(d, regCP_PFP_IC_BASE_LO), RG(d, regCP_PFP_IC_BASE_CNTL),
             RG(d, regCP_ME_IC_BASE_HI), RG(d, regCP_ME_IC_BASE_LO),
             RG(d, regCP_GFX_RS64_DC_BASE0_HI), RG(d, regCP_GFX_RS64_DC_BASE0_LO),
             RG(d, regCP_PFP_PRGRM_CNTR_START), RG(d, regCP_ME_PRGRM_CNTR_START),
             RG(d, regCP_PFP_INSTR_PNTR), RG(d, regCP_ME_INSTR_PNTR),
             RG(d, regCP_GFX_RS64_INSTR_PNTR0), RG(d, regCP_GFX_RS64_INSTR_PNTR1));
        RERR(d, "  GFX MQD 0x%08x_%08x HQD active %u base 0x%08x_%08x cntl 0x%08x rptr 0x%x wptr 0x%x",
             RG(d, regCP_GFX_MQD_BASE_ADDR_HI), RG(d, regCP_GFX_MQD_BASE_ADDR), RG(d, regCP_GFX_HQD_ACTIVE),
             RG(d, regCP_GFX_HQD_BASE_HI), RG(d, regCP_GFX_HQD_BASE), RG(d, regCP_GFX_HQD_CNTL),
             RG(d, regCP_GFX_HQD_RPTR), RG(d, regCP_GFX_HQD_WPTR));
    }
    return RDNA3_ETIME;
}

uint32_t gfx11_ring_space(rdna3_dev *d, rdna3_gfx_ring *r)
{
    /* gfx11 rptr is 32 bit (gfx_v11_0_ring_get_rptr_gfx, gfx_v11_0_ring_get_rptr_compute) */
    uint32_t rptr = wb_read32(d, r, RD_GFX_WB_RPTR) & r->buf_mask;
    uint32_t used = ((uint32_t)r->wptr - rptr) & r->buf_mask;
    return r->buf_mask - used;   /* one slot stays empty: full != empty */
}

/* amdgpu_ring_alloc: wait (bounded) until 'ndw' dwords fit */
static int ring_wait_space(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t ndw)
{
    uint32_t i;
    /* align requested size with padding so unlock_commit can pad safely */
    if (r->amdgpu_pad)
        ndw = (ndw + 0xff) & ~0xffu;
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
    gfxhub303_flush_regs(d, vmid, r->vm_inv_eng ? r->vm_inv_eng : RD_GFX_VM_INV_ENG, &f);
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
                            /* nbio_v4_3_get_hdp_flush_req_offset, nbio_v4_3_get_hdp_flush_done_offset */
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

static void ring_poke(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t idx, uint32_t v)
{
    rdna3_vram_write(d, r->ring + (uint64_t)(idx & r->buf_mask) * 4, &v, 4);
}

/* gfx_v11_0_ring_emit_init_cond_exec */
uint32_t gfx11_emit_init_cond_exec(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t addr)
{
    uint32_t ret;
    gfx11_write(d, r, PACKET3(PACKET3_COND_EXEC, 3));
    gfx11_write(d, r, (uint32_t)addr);
    gfx11_write(d, r, (uint32_t)(addr >> 32));
    /* discard following DWs if *cond_exec_gpu_addr==0 */
    gfx11_write(d, r, 0);
    ret = (uint32_t)r->wptr & r->buf_mask;
    /* patch dummy value later */
    gfx11_write(d, r, 0);
    return ret;
}

/* amdgpu_ring_patch_cond_exec */
void gfx11_ring_patch_cond_exec(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t offset)
{
    uint32_t cur = ((uint32_t)r->wptr - 1) & r->buf_mask;
    if (cur < offset)
        cur += r->ring_size >> 2;
    ring_poke(d, r, offset, cur - offset);
}

/* gfx_v11_0_ring_emit_gfx_shadow. Each job gets a prefix (the job's shadow
 * VAs) and a postfix (all zero: shadowing off); each starts with a NOP whose
 * first payload dword gates the next one through COND_EXEC. The prefix is
 * skipped unless the previous postfix ran (it re-arms its gate with
 * WRITE_DATA); the postfix runs while its prefix's gate is set. The CPU
 * clears the previous prefix's gate when the configuration did not change,
 * so a postfix the CP has not reached yet does not drop the shadow state. */
void gfx11_emit_gfx_shadow(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t shadow_va, uint64_t csa_va,
                           uint64_t gds_va, int init_shadow, uint32_t vmid)
{
    uint32_t offs, end = 0;
    uint64_t ring_mc = rdna3_vram_mc(d, r->ring);

    if (!r->cp_gfx_shadow)
        return;

    /* the dw in the NOP controls if the next SET_Q_MODE packet should be
     * executed or not; 64 bits reserved */
    gfx11_write(d, r, PACKET3(PACKET3_NOP, 1));
    offs = (uint32_t)r->wptr & r->buf_mask;
    /* start with skipping the prefix and always executing the postfix;
     * the postfix's WRITE_DATA below changes that once it ran */
    gfx11_write(d, r, shadow_va ? 1 : 0);
    gfx11_write(d, r, 0);

    if (r->set_q_mode_offs)
        end = gfx11_emit_init_cond_exec(d, r, ring_mc + ((uint64_t)r->set_q_mode_offs << 2));

    /* when the postfix SET_Q_MODE executes the next prefix must as well */
    if (!shadow_va) {
        uint64_t addr = ring_mc + ((uint64_t)offs << 2);
        gfx11_write(d, r, PACKET3(PACKET3_WRITE_DATA, 3));
        gfx11_write(d, r, WRITE_DATA_DST_SEL(5) | WR_CONFIRM);
        gfx11_write(d, r, (uint32_t)addr);
        gfx11_write(d, r, (uint32_t)(addr >> 32));
        gfx11_write(d, r, 0x1);
    }

    gfx11_write(d, r, PACKET3(PACKET3_SET_Q_PREEMPTION_MODE, 7));
    gfx11_write(d, r, (uint32_t)shadow_va);
    gfx11_write(d, r, (uint32_t)(shadow_va >> 32));
    gfx11_write(d, r, (uint32_t)gds_va);
    gfx11_write(d, r, (uint32_t)(gds_va >> 32));
    gfx11_write(d, r, (uint32_t)csa_va);
    gfx11_write(d, r, (uint32_t)(csa_va >> 32));
    gfx11_write(d, r, shadow_va ? PACKET3_SET_Q_PREEMPTION_MODE_IB_VMID(vmid) : 0);
    gfx11_write(d, r, init_shadow ? PACKET3_SET_Q_PREEMPTION_MODE_INIT_SHADOW_MEM : 0);

    if (r->set_q_mode_offs)
        gfx11_ring_patch_cond_exec(d, r, end);

    if (shadow_va) {
        uint64_t token = shadow_va ^ csa_va ^ gds_va ^ vmid;
        /* same configuration: try to skip the last postfix so the state
         * is not saved and restored every time */
        if (r->set_q_mode_ptr_valid && r->set_q_mode_token == token)
            ring_poke(d, r, r->set_q_mode_ptr, 0);
        r->set_q_mode_token = token;
    } else {
        r->set_q_mode_ptr = r->set_q_mode_offs;
        r->set_q_mode_ptr_valid = 1;
    }
    r->set_q_mode_offs = offs;
}

static uint32_t submit_amdgpu(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ctx, uint32_t vmid,
                              uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n,
                              const rdna3_gfx_job_ext *x);

uint32_t gfx11_submit_job(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ctx, uint32_t vmid,
                          uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n)
{
    return gfx11_submit_job_ex(d, r, ctx, vmid, pd_addr, ibs, n, NULL);
}

void gfx11_emit_gds_switch(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t vmid,
                           uint32_t gds_base, uint32_t gds_size, uint32_t gws_base,
                           uint32_t gws_size, uint32_t oa_base, uint32_t oa_size)
{
    /* gfx_v11_0_write_data_to_reg(ring, 0, false, reg, val): ME, no confirm */
    const uint32_t reg[4] = { OFF(d, regGDS_VMID0_BASE) + 2 * vmid,
                              OFF(d, regGDS_VMID0_SIZE) + 2 * vmid,
                              OFF(d, regGDS_GWS_VMID0) + vmid, OFF(d, regGDS_OA_VMID0) + vmid };
    const uint32_t val[4] = { gds_base, gds_size,
                              gws_size << GDS_GWS_VMID0__SIZE__SHIFT | gws_base,
                              (1u << (oa_size + oa_base)) - (1u << oa_base) };
    uint32_t i;
    for (i = 0; i < 4; i++) {
        gfx11_write(d, r, PACKET3(PACKET3_WRITE_DATA, 3));
        gfx11_write(d, r, WRITE_DATA_ENGINE_SEL(0) | WRITE_DATA_DST_SEL(0));
        gfx11_write(d, r, reg[i]);
        gfx11_write(d, r, 0);
        gfx11_write(d, r, val[i]);
    }
}

uint32_t gfx11_submit_job_ex(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ctx, uint32_t vmid,
                             uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n,
                             const rdna3_gfx_job_ext *x)
{
    /* emit_frame_size for what we emit: vm flush 5+5+7+2, gds switch 20,
     * mem sync 8, hdp 7, cntxcntl 3, frame 2+2, user fence 8, fence 8,
     * plus 4 per IB */
    uint32_t i, seq, ndw = 19 + 20 + 8 + 7 + 3 + 4 + 8 + 8 + 4 * n, cond_exec = 0;
    uint64_t cond_addr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_COND_EXE);
    int ctx_switch = ctx != r->last_ctx, secure;
    if (!r->ready || !n || n > RD_GFX_MAX_IBS || vmid > 15)
        return 0;
    if (r->cp_gfx_shadow)
        ndw += 2 * (22 + 5);   /* SET_Q_PREEMPTION_MODE + COND_EXEC, twice */
    if (d->amdgpu_exact)
        return submit_amdgpu(d, r, ctx, vmid, pd_addr, ibs, n, x);
    if (ring_wait_space(d, r, ndw))
        return 0;
    if (pd_addr)
        gfx11_emit_vm_flush(d, r, vmid, pd_addr);
    if (x && x->gds_switch)
        gfx11_emit_gds_switch(d, r, vmid, x->gds_base, x->gds_size, x->gws_base, x->gws_size,
                              x->oa_base, x->oa_size);
    gfx11_emit_mem_sync(d, r);
    /* amdgpu_ib_schedule: amdgpu_ring_emit_gfx_shadow with the job's VAs,
     * then amdgpu_ring_init_cond_exec on ring->cond_exe_gpu_addr. The
     * COND_EXEC only matters for MCBP preemption (gfx_v11_0_ring_preempt_ib
     * clears the slot), which is not ported: without cp_gfx_shadow it is
     * left out and the stream stays the one verified on the card. */
    if (r->cp_gfx_shadow) {
        gfx11_emit_gfx_shadow(d, r, x ? x->shadow_va : 0, x ? x->csa_va : 0, x ? x->gds_va : 0,
                              x ? x->init_shadow : 0, vmid);
        cond_exec = gfx11_emit_init_cond_exec(d, r, cond_addr);
    }
    gfx11_emit_hdp_flush(d, r);
    gfx11_emit_cntxcntl(d, r, ctx_switch);
    secure = (ibs[0].flags & RD_GFX_IB_FLAG_TMZ) != 0;
    gfx11_emit_frame_cntl(d, r, 1, secure);
    for (i = 0; i < n; i++)
        gfx11_emit_ib(d, r, vmid, ibs[i].va, ibs[i].ndw);
    gfx11_emit_frame_cntl(d, r, 0, secure);
    if (x && x->uf_addr)   /* the job's number: the fence emitted next */
        gfx11_emit_user_fence(d, r, x->uf_addr, (uint32_t)(r->fence_seq + 1));
    /* amdgpu_ib_schedule: the postfix (shadowing off) and a second
     * COND_EXEC before the fence; the first COND_EXEC then covers
     * everything up to and including the fence */
    if (r->cp_gfx_shadow) {
        gfx11_emit_gfx_shadow(d, r, 0, 0, 0, 0, 0);
        gfx11_emit_init_cond_exec(d, r, cond_addr);
    }
    seq = gfx11_emit_fence(d, r);
    if (r->cp_gfx_shadow)
        gfx11_ring_patch_cond_exec(d, r, cond_exec);
    gfx11_commit(d, r);
    r->last_ctx = ctx;
    r->last_vmid = vmid;
    return seq;
}

/* amdgpu_fence_emit_polling / amdgpu_fence_emit: the next sequence number
 * through the ring's emit_fence (with AMDGPU_FENCE_FLAG_INT, the EOP
 * interrupt, once the ring has one); the fence is that number in the
 * ring's writeback slot, there is no fence object */
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

void gfx11_emit_user_fence(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t addr, uint64_t seq)
{
    gfx11_write(d, r, PACKET3(PACKET3_RELEASE_MEM, 6));
    gfx11_write(d, r, (1u << 22) /* GCR_SEQ */ | (1u << 21) /* GCR_GL2_WB */ |
                      (1u << 13) /* GCR_GLM_INV */ | (1u << 12) /* GCR_GLM_WB */ |
                      (3u << 25) /* CACHE_POLICY bypass */ |
                      CACHE_FLUSH_AND_INV_TS_EVENT | (5u << 8) /* EVENT_INDEX */);
    gfx11_write(d, r, 2u << 29);   /* DATA_SEL 2: 64 bits, INT_SEL 0 */
    gfx11_write(d, r, (uint32_t)addr);
    gfx11_write(d, r, (uint32_t)(addr >> 32));
    gfx11_write(d, r, (uint32_t)seq);
    gfx11_write(d, r, (uint32_t)(seq >> 32));
    gfx11_write(d, r, 0);
}

/* gfx_v11_0_set_eop_interrupt_state: the gfx or the compute EOP source of the ring */
void gfx11_set_eop_interrupt(rdna3_dev *d, rdna3_gfx_ring *r, int enable)
{
    uint32_t tmp;
    if (r->kind == RD_RING_GFX) {   /* gfx_v11_0_set_gfx_eop_interrupt_state, me 0 pipe 0 */
        tmp = RG(d, regCP_INT_CNTL_RING0);   /* gfx_v11_0_get_cpg_int_cntl(0, 0) */
        tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, TIME_STAMP_INT_ENABLE, enable ? 1 : 0);
        tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GENERIC0_INT_ENABLE, enable ? 1 : 0);
        WG(d, regCP_INT_CNTL_RING0, tmp);
    } else {                        /* gfx_v11_0_set_compute_eop_interrupt_state, me 1 */
        /* gfx_v11_0_get_cpc_int_cntl(1, pipe): PIPE0..3 are consecutive */
        uint32_t reg = OFF(d, regCP_ME1_PIPE0_INT_CNTL) + r->pipe;
        tmp = rdna3_rreg(d, reg);
        tmp = REG_SET_FIELD(tmp, CP_ME1_PIPE0_INT_CNTL, TIME_STAMP_INT_ENABLE, enable ? 1 : 0);
        tmp = REG_SET_FIELD(tmp, CP_ME1_PIPE0_INT_CNTL, GENERIC0_INT_ENABLE, enable ? 1 : 0);
        rdna3_wreg(d, reg, tmp);
    }
    r->fence_irq = enable;
}

/* amdgpu_fence_read: the ring's fence writeback slot */
int gfx11_fence_passed(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq)
{
    return (int32_t)(wb_read32(d, r, RD_GFX_WB_FENCE) - seq) >= 0;
}

/* amdgpu_fence_wait_polling */
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
    if (r->async && !d->doorbell_ok)
        r->async = 0;   /* the KIQ needs doorbells: RB0 registers instead */
    r->mqd = r->async ? rdna3_vram_alloc(d, 4096, 4096, "gfx-mqd") : 0;
    /* amdgpu_gfx_mqd_sw_init's me.mqd_backup (rdna3amdgpu=1) */
    r->mqd_backup = r->async && d->amdgpu_exact ? rdna3_vram_alloc(d, 4096, 4096, "gfx-mqd-backup") : 0;
    if (r->ring == UINT64_MAX || r->wb == UINT64_MAX || r->csb == UINT64_MAX || r->mqd == UINT64_MAX ||
        r->mqd_backup == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, r->ring, 0, ring_size);
    rdna3_vram_write(d, r->wb, zero, sizeof(zero));
    {
        /* amdgpu_ring_init: "always set cond_exec_polling to CONTINUE" */
        uint32_t one = 1;
        rdna3_vram_write(d, r->wb + RD_GFX_WB_COND_EXE, &one, 4);
    }
    shadow_state_reset(r);

    ret = wait_rlc_autoload(d);
    if (ret)
        return ret;
    if (rs64)
        config_gfx_rs64(d, rs64);
    ret = gfx11_gfxhub_enable(d, g);
    if (ret)
        return ret;
    golden_registers(d);
    constants_init(d);
    if (d->doorbell_ok) {
        nbio43_gc_doorbell_init(d);   /* gfx_v11_0_hw_init */
        cp_set_doorbell_range(d);     /* gfx_v11_0_cp_resume */
    }

    /* gfx_v11_0_rlc_resume (PSP): gfx_v11_0_init_csb + gfx_v11_0_rlc_enable_srm */
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

    if (r->async) {
        /* gfx_v11_0_cp_resume (amdgpu_async_gfx_ring): both CPs run, the
         * KIQ maps the gfx queue next (gfx11_kgq_start) */
        cp_compute_enable(d, 1);
        gfx11_cp_gfx_enable(d, 1);
        RLOG(d, "GFX: CPs running, ME_CNTL 0x%08x CP_STAT 0x%08x; gfx queue waits for the KIQ",
             RG(d, regCP_ME_CNTL), RG(d, regCP_STAT));
        return RDNA3_OK;
    }
    cp_gfx_resume(d, r);
    cp_gfx_start(d, r);
    ret = gfx11_ring_test(d, r);   /* amdgpu_ring_test_helper: ready only when it passed */
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
#define MQD_CP_HQD_IQ_TIMER            150
#define MQD_CP_HQD_DEQUEUE_REQUEST     152
#define MQD_CP_MQD_CONTROL             162
#define MQD_CP_HQD_EOP_BASE_ADDR_LO    165
#define MQD_CP_HQD_EOP_BASE_ADDR_HI    166
#define MQD_CP_HQD_EOP_CONTROL         167
#define MQD_CP_HQD_PQ_WPTR_LO          182
#define MQD_CP_HQD_PQ_WPTR_HI          183

/* struct v11_gfx_mqd dword offsets (v11_structs.h); the HQD window
 * 0x1e7e.. (CP_GFX_MQD_BASE_ADDR..) maps onto dwords 128.. */
#define MQD_GFX_HQD_ACTIVE             130
#define MQD_GFX_HQD_VMID               131
#define MQD_GFX_HQD_QUEUE_PRIORITY     134
#define MQD_GFX_HQD_QUANTUM            135
#define MQD_GFX_HQD_BASE               136
#define MQD_GFX_HQD_BASE_HI            137
#define MQD_GFX_HQD_RPTR               138
#define MQD_GFX_HQD_RPTR_ADDR          139
#define MQD_GFX_HQD_RPTR_ADDR_HI       140
#define MQD_GFX_RB_WPTR_POLL_ADDR_LO   141
#define MQD_GFX_RB_WPTR_POLL_ADDR_HI   142
#define MQD_GFX_RB_DOORBELL_CONTROL    143
#define MQD_GFX_HQD_CNTL               145
#define MQD_GFX_HQD_WPTR               149
#define MQD_GFX_HQD_WPTR_HI            150
#define MQD_GFX_MQD_CONTROL            162

static uint32_t g_mqd[RD_MQD_DWORDS];   /* staging, bring-up is single threaded */

/* gfx_v11_0_compute_ring_init / amdgpu_gfx_kiq_init_ring (doorbell, me/pipe/queue,
 * 1024 dw ring) with the queue's EOP buffer zeroed (amdgpu_gfx_kiq_init); for
 * the MES pipes this is mes_v11_0_allocate_eop_buf and mes_v11_0_mqd_sw_init
 * (its mqd_backup is never read in amdgpu) */
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
    /* amdgpu_gfx_mqd_sw_init (rdna3amdgpu=1): the KCQ's MQD backup; the
     * KIQ runs on the MES (enable_mes_kiq), no backup there */
    r->mqd_backup = 0;
    if (d->amdgpu_exact && !kiq) {
        r->mqd_backup = rdna3_vram_alloc(d, 4096, 4096, "kcq-mqd-backup");
        if (r->mqd_backup == UINT64_MAX)
            return RDNA3_ENOMEM;
    }
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

/* amdgpu_ring_init_mqd -> gfx_v11_0_compute_mqd_init (amdgpu_ring_to_mqd_prop: kernel queue,
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
    /* prop->allow_tunneling: a high priority compute queue */
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, r->high_prio ? 1 : 0);
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
    /* set static priority for a compute queue/ring (amdgpu_ring_to_mqd_prop) */
    g_mqd[MQD_CP_HQD_PIPE_PRIORITY] = r->high_prio ? RD_GFX_PIPE_PRIO_HIGH : 0;
    g_mqd[MQD_CP_HQD_QUEUE_PRIORITY] = r->high_prio ? RD_GFX_QUEUE_PRIORITY_MAXIMUM : 0;
    tmp = REG_SET_FIELD(0u, CP_HQD_QUANTUM, QUANTUM_EN, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_SCALE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_DURATION, 1);
    g_mqd[MQD_CP_HQD_QUANTUM] = tmp;
    g_mqd[MQD_CP_HQD_ACTIVE] = rb->kind == RD_RING_KIQ ? 1 : 0;
    rdna3_vram_write(d, r->mqd, g_mqd, sizeof(g_mqd));
    /* gfx_v11_0_kcq_init_queue: memcpy_fromio(mqd_backup, mqd) */
    if (r->mqd_backup)
        rdna3_vram_write(d, r->mqd_backup, g_mqd, sizeof(g_mqd));
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

/* mes_v11_0_early_init -> amdgpu_mes_init_microcode for both pipes:
 * mes_uc_start_addr of each pipe's firmware */
int gfx11_mes_from_fw(const void *mes, uint32_t mes_len, const void *mes1, uint32_t mes1_len,
                      uint64_t uc_start[2])
{
    const struct mes_firmware_header_v1_0 *h[2];
    if (!mes || !mes1 || mes_len < sizeof(*h[0]) || mes1_len < sizeof(*h[0]))
        return RDNA3_EINVAL;
    h[0] = (const struct mes_firmware_header_v1_0 *)mes;
    h[1] = (const struct mes_firmware_header_v1_0 *)mes1;
    for (int i = 0; i < 2; i++) {
        if (h[i]->header.header_version_major != 1)
            return RDNA3_EINVAL;
        uc_start[i] = h[i]->mes_uc_start_addr_lo | ((uint64_t)h[i]->mes_uc_start_addr_hi << 32);
    }
    return RDNA3_OK;
}

/* mes_v11_0_kiq_ring_init: me 3 pipe 1 queue 0, doorbell MES_RING1 */
int gfx11_mes_kiq_ring_init(rdna3_dev *d, rdna3_mec_ring *r, const uint64_t uc_start[2])
{
    int ret = gfx11_mec_ring_init(d, r, 1, 1, 0, RD_DB_MES_RING1, 0);
    if (ret)
        return ret;
    r->me = 3;
    r->mes_uc_start[0] = uc_start[0];
    r->mes_uc_start[1] = uc_start[1];
    return RDNA3_OK;
}

/* mes_v11_0_enable(true) with enable_mes_kiq: both pipes from their start
 * addresses, then unhalted and active */
static void mes_enable(rdna3_dev *d, const uint64_t uc_start[2])
{
    uint32_t pipe, v = RG(d, regCP_MES_CNTL);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_RESET, 1);
    WG(d, regCP_MES_CNTL, v);
    for (pipe = 0; pipe < 2; pipe++) {
        uint64_t a = uc_start[pipe] >> 2;
        grbm_select(d, 3, pipe, 0, 0);
        WG(d, regCP_MES_PRGRM_CNTR_START, (uint32_t)a);
        WG(d, regCP_MES_PRGRM_CNTR_START_HI, (uint32_t)(a >> 32));
    }
    grbm_select(d, 0, 0, 0, 0);
    v = REG_SET_FIELD(0u, CP_MES_CNTL, MES_PIPE0_ACTIVE, 1);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_ACTIVE, 1);
    WG(d, regCP_MES_CNTL, v);
    d->os.udelay(d->os.ctx, 500);
}

/* mes_v11_0_enable(false) */
static void mes_disable(rdna3_dev *d)
{
    uint32_t v = RG(d, regCP_MES_CNTL);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_ACTIVE, 0);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_ACTIVE, 0);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_INVALIDATE_ICACHE, 1);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_RESET, 1);
    v = REG_SET_FIELD(v, CP_MES_CNTL, MES_HALT, 1);
    WG(d, regCP_MES_CNTL, v);
}

/* mes_v11_0_mqd_init for the KIQ ring */
static void mes_mqd_init(rdna3_dev *d, rdna3_mec_ring *r)
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
    tmp = REG_SET_FIELD(regCP_HQD_EOP_CONTROL_DEFAULT, CP_HQD_EOP_CONTROL, EOP_SIZE,
                        order_base_2(RD_MEC_HPD_SIZE / 4) - 1);   /* MES_EOP_SIZE 2048 */
    g_mqd[MQD_CP_HQD_EOP_BASE_ADDR_LO] = (uint32_t)eop;
    g_mqd[MQD_CP_HQD_EOP_BASE_ADDR_HI] = (uint32_t)(eop >> 32);
    g_mqd[MQD_CP_HQD_EOP_CONTROL] = tmp;
    rb->wptr = 0;
    g_mqd[MQD_CP_MQD_BASE_ADDR_LO] = (uint32_t)mqd & 0xfffffffc;
    g_mqd[MQD_CP_MQD_BASE_ADDR_HI] = (uint32_t)(mqd >> 32);
    g_mqd[MQD_CP_MQD_CONTROL] = REG_SET_FIELD(regCP_MQD_CONTROL_DEFAULT, CP_MQD_CONTROL, VMID, 0);
    g_mqd[MQD_CP_HQD_PQ_BASE_LO] = (uint32_t)hqd;
    g_mqd[MQD_CP_HQD_PQ_BASE_HI] = (uint32_t)(hqd >> 32);
    wb = rdna3_vram_mc(d, rb->wb + RD_GFX_WB_RPTR);
    g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_LO] = (uint32_t)wb & 0xfffffffc;
    g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_HI] = (uint32_t)(wb >> 32) & 0xffff;
    wb = rdna3_vram_mc(d, rb->wb + RD_GFX_WB_WPTR);
    g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_LO] = (uint32_t)wb & 0xfffffff8;
    g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_HI] = (uint32_t)(wb >> 32) & 0xffff;
    /* RPTR_BLOCK_SIZE: amdgpu passes the value pre-shifted by 8, which
     * REG_SET_FIELD masks to 0 */
    tmp = REG_SET_FIELD(regCP_HQD_PQ_CONTROL_DEFAULT, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
                        order_base_2(rb->ring_size / 4) - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, (order_base_2(4096 / 4) - 1) << 8);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, NO_UPDATE_RPTR, 1);
    g_mqd[MQD_CP_HQD_PQ_CONTROL] = tmp;
    tmp = REG_SET_FIELD(0u, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, rb->doorbell);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
    g_mqd[MQD_CP_HQD_PQ_DOORBELL_CONTROL] = tmp;
    g_mqd[MQD_CP_HQD_VMID] = 0;
    g_mqd[MQD_CP_HQD_ACTIVE] = 1;
    g_mqd[MQD_CP_HQD_PERSISTENT_STATE] =
        REG_SET_FIELD(regCP_HQD_PERSISTENT_STATE_DEFAULT, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    g_mqd[MQD_CP_HQD_IB_CONTROL] = regCP_HQD_IB_CONTROL_DEFAULT;
    g_mqd[MQD_CP_HQD_IQ_TIMER] = regCP_HQD_IQ_TIMER_DEFAULT;
    g_mqd[MQD_CP_HQD_QUANTUM] = regCP_HQD_QUANTUM_DEFAULT;
    rdna3_vram_write(d, r->mqd, g_mqd, sizeof(g_mqd));
    rdna3_hdp_flush(d);
}

/* mes_v11_0_queue_init_register for the KIQ pipe */
static void mes_queue_init_register(rdna3_dev *d, rdna3_mec_ring *r)
{
    uint32_t v;
    grbm_select(d, 3, r->pipe, 0, 0);
    v = RG(d, regCP_HQD_VMID);
    WG(d, regCP_HQD_VMID, REG_SET_FIELD(v, CP_HQD_VMID, VMID, 0));
    v = RG(d, regCP_HQD_PQ_DOORBELL_CONTROL);
    WG(d, regCP_HQD_PQ_DOORBELL_CONTROL, REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 0));
    WG(d, regCP_MQD_BASE_ADDR, g_mqd[MQD_CP_MQD_BASE_ADDR_LO]);
    WG(d, regCP_MQD_BASE_ADDR_HI, g_mqd[MQD_CP_MQD_BASE_ADDR_HI]);
    WG(d, regCP_MQD_CONTROL, 0);
    WG(d, regCP_HQD_PQ_BASE, g_mqd[MQD_CP_HQD_PQ_BASE_LO]);
    WG(d, regCP_HQD_PQ_BASE_HI, g_mqd[MQD_CP_HQD_PQ_BASE_HI]);
    WG(d, regCP_HQD_PQ_RPTR_REPORT_ADDR, g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_LO]);
    WG(d, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, g_mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_HI]);
    WG(d, regCP_HQD_PQ_CONTROL, g_mqd[MQD_CP_HQD_PQ_CONTROL]);
    WG(d, regCP_HQD_PQ_WPTR_POLL_ADDR, g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_LO]);
    WG(d, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, g_mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_HI]);
    WG(d, regCP_HQD_PQ_DOORBELL_CONTROL, g_mqd[MQD_CP_HQD_PQ_DOORBELL_CONTROL]);
    WG(d, regCP_HQD_PERSISTENT_STATE, g_mqd[MQD_CP_HQD_PERSISTENT_STATE]);
    WG(d, regCP_HQD_ACTIVE, g_mqd[MQD_CP_HQD_ACTIVE]);
    grbm_select(d, 0, 0, 0, 0);
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
    if (kiq->me == 3) {
        /* mes_v11_0_kiq_hw_init (PSP loaded both MES pipes); the MEC runs
         * the KCQs (gfx_v11_0_cp_resume enables it first) */
        cp_compute_enable(d, 1);
        mes_enable(d, kiq->mes_uc_start);
        tmp = RG(d, regRLC_CP_SCHEDULERS) & 0xffffff00;   /* mes_v11_0_kiq_setting */
        tmp |= (kiq->me << 5) | (kiq->pipe << 3) | kiq->queue;
        WG(d, regRLC_CP_SCHEDULERS, tmp);
        WG(d, regRLC_CP_SCHEDULERS, tmp | 0x80);
        mes_mqd_init(d, kiq);
        mes_queue_init_register(d, kiq);
        kiq->mapped = 1;
        kiq->rb.wptr = 0;
        ret = kiq_ring_test(d, kiq);
        if (ret)
            return ret;
        kiq->rb.ready = 1;
        RLOG(d, "KIQ: MES me 3 pipe 1 up (start 0x%llx/0x%llx), doorbell 0x%x, ring test passed",
             (unsigned long long)kiq->mes_uc_start[0], (unsigned long long)kiq->mes_uc_start[1],
             kiq->rb.doorbell);
        return RDNA3_OK;
    }
    cp_compute_enable(d, 1);
    /* gfx_v11_0_kiq_setting: tell the RLC which queue is the KIQ */
    tmp = RG(d, regRLC_CP_SCHEDULERS) & 0xffffff00;
    tmp |= (kiq->me << 5) | (kiq->pipe << 3) | kiq->queue;
    WG(d, regRLC_CP_SCHEDULERS, tmp);
    WG(d, regRLC_CP_SCHEDULERS, tmp | 0x80);
    /* gfx_v11_0_kiq_resume -> gfx_v11_0_kiq_init_queue */
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

/* ---------------------------- MES scheduler ---------------------------- */

int gfx11_mes_sched_init(rdna3_dev *d, rdna3_mes *m)
{
    /* mes_v11_0_sw_init's scheduler pipe: mes_v11_0_ring_init (me 3 pipe 0,
     * doorbell MES_RING0) with its EOP and MQD; amdgpu_mes_init's masks
     * and writeback slots are in mes_set_hw_resources and m->wb; the KIQ
     * pipe is gfx11_mes_kiq_ring_init */
    int ret = gfx11_mec_ring_init(d, &m->ring, 0, 0, 0, RD_DB_MES_RING0, 0);   /* 1024 dw */
    if (ret)
        return ret;
    m->ring.me = 3;
    m->wb = m->ring.rb.wb + 256;   /* the ring's writeback page holds the MES slots too */
    rdna3_vram_fill(d, m->wb, 0, 256);
    m->seq = 0;
    m->ready = 0;
    m->submitted = m->failed = 0;
    return RDNA3_OK;
}

/* gfx_v11_0_kiq_map_queues for AMDGPU_RING_TYPE_MES: me 2, ENGINE_SEL 5 */
static void kiq_map_mes(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *r)
{
    rdna3_gfx_ring *k = &kiq->rb;
    uint64_t mqd = rdna3_vram_mc(d, r->mqd), wptr = rdna3_vram_mc(d, r->rb.wb + RD_GFX_WB_WPTR);
    gfx11_write(d, k, PACKET3(PACKET3_MAP_QUEUES, 5));
    gfx11_write(d, k, (0u << 4) /* QUEUE_SEL */ | (0u << 8) /* VMID */ | (r->queue << 13) |
                      (r->pipe << 16) | (2u << 18) /* me 2 */ | (0u << 21) /* normal */ |
                      (0u << 24) | (5u << 26) /* ENGINE_SEL MES */ | (1u << 29) /* NUM_QUEUES */);
    gfx11_write(d, k, r->rb.doorbell << 2);
    gfx11_write(d, k, (uint32_t)mqd);
    gfx11_write(d, k, (uint32_t)(mqd >> 32));
    gfx11_write(d, k, (uint32_t)wptr);
    gfx11_write(d, k, (uint32_t)(wptr >> 32));
    gfx11_commit(d, k);
}

static void put64(uint32_t *p, uint64_t v)
{
    p[0] = (uint32_t)v;
    p[1] = (uint32_t)(v >> 32);
}

int gfx11_mes_submit(rdna3_dev *d, rdna3_mes *m, uint32_t *pkt, uint32_t status_dw)
{
    rdna3_gfx_ring *rb = &m->ring.rb;
    uint32_t q[RD_MES_API_DWORDS], i, timeout = 210000;   /* 2.1 s in 10 us steps */
    uint64_t zero = 0, fence = 0, status = 0, seq;
    if (!rb->ring_size || status_dw + 4 > RD_MES_API_DWORDS)
        return RDNA3_EINVAL;
    rdna3_vram_write(d, m->wb + RD_MES_WB_STATUS, &zero, 8);
    /* rdna3amdgpu=1: amdgpu_ring_alloc + the wait for room
     * (mes_v11_0_submit_pkt_and_poll_completion's error_undo path) */
    if (d->amdgpu_exact) {
        uint64_t wptr_old = rb->wptr;
        if (ring_wait_space(d, rb, 2 * RD_MES_API_DWORDS)) {
            RERR(d, "MES ring buffer is full.");
            rb->wptr = wptr_old;   /* amdgpu_ring_undo */
            return RDNA3_ETIME;
        }
    }
    seq = ++m->seq;
    put64(pkt + status_dw, rdna3_vram_mc(d, m->wb + RD_MES_WB_STATUS));
    put64(pkt + status_dw + 2, 1);
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        gfx11_write(d, rb, pkt[i]);
    /* union MESAPI__QUERY_MES_STATUS: header, mes_healthy, api_status at dw 2 */
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        q[i] = 0;
    q[0] = RD_MES_API_HEADER(RD_MES_SCH_API_QUERY_SCHEDULER_STATUS);
    put64(q + 2, rdna3_vram_mc(d, m->wb + RD_MES_WB_FENCE));
    put64(q + 4, seq);
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        gfx11_write(d, rb, q[i]);
    gfx11_commit(d, rb);
    m->submitted++;
    for (i = 0; i < timeout; i++) {
        rdna3_vram_read(d, m->wb + RD_MES_WB_FENCE, &fence, 8);
        if (fence >= seq)
            break;
        d->os.udelay(d->os.ctx, 10);
    }
    rdna3_vram_read(d, m->wb + RD_MES_WB_STATUS, &status, 8);
    if (fence < seq || !status) {
        m->failed++;
        RERR(d, "MES: no answer to opcode %u (fence %llu of %llu, status %llu)", (pkt[0] >> 4) & 0xff,
             (unsigned long long)fence, (unsigned long long)seq, (unsigned long long)status);
        return RDNA3_ETIME;
    }
    return RDNA3_OK;
}

/* mes_v11_0_set_hw_resources with amdgpu_mes_init's masks */
static int mes_set_hw_resources(rdna3_dev *d, rdna3_mes *m)
{
    uint32_t p[RD_MES_API_DWORDS], i;
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        p[i] = 0;
    p[0] = RD_MES_API_HEADER(RD_MES_SCH_API_SET_HW_RSRC);
    p[1] = 0xffffff00;          /* vmid_mask_mmhub: VMIDs 8-23 for user queues */
    p[2] = 0xffffff00;          /* vmid_mask_gfxhub */
    p[3] = 0x1000;              /* gds_size (gfx_v11_0_set_gds_init) */
    p[4] = 0;                   /* paging_vmid */
    for (i = 0; i < 4; i++)
        p[5 + i] = 0xc;         /* compute_hqd_mask: queues 2-3 of the first MEC's pipes */
    p[13] = 0xfffffffe;         /* gfx_hqd_mask[0]: all but the kernel gfx queue */
    p[15] = 0xfc;               /* sdma_hqd_mask[0..1], two SDMA 6.0 engines */
    p[16] = 0xfc;
    for (i = 0; i < 5; i++)
        p[17 + i] = RD_MES_DB_START_DW + i * 2;   /* aggregated_doorbells[prio] */
    put64(p + 22, rdna3_vram_mc(d, m->wb + RD_MES_WB_SCH_CTX));
    put64(p + 24, rdna3_vram_mc(d, m->wb + RD_MES_WB_QUERY_FENCE));
    for (i = 0; i < 5; i++) {
        p[26 + i] = d->reg_base[HWIP_GC][0][i];
        p[34 + i] = d->reg_base[HWIP_MMHUB][0][i];
        p[42 + i] = d->reg_base[HWIP_OSSSYS][0][i];
    }
    /* api_status at 50; disable_reset, use_different_vmid_compute,
     * disable_mes_log, enable_level_process_quantum_check,
     * enable_reg_active_poll */
    p[54] = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 6) | (1u << 10);
    p[55] = 50;                 /* oversubscription_timer */
    return gfx11_mes_submit(d, m, p, 50);
}

/* mes_v11_0_query_sched_status */
static int mes_query_sched_status(rdna3_dev *d, rdna3_mes *m)
{
    uint32_t p[RD_MES_API_DWORDS], i;
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        p[i] = 0;
    p[0] = RD_MES_API_HEADER(RD_MES_SCH_API_QUERY_SCHEDULER_STATUS);
    return gfx11_mes_submit(d, m, p, 2);
}

int gfx11_mes_sched_start(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mes *m)
{
    uint8_t zero[64] = { 0 };
    int ret;
    if (!kiq || !kiq->rb.ready || kiq->me != 3 || !m->ring.rb.ring_size)
        return RDNA3_EINVAL;
    m->ready = 0;
    m->ring.rb.wptr = 0;
    rdna3_vram_fill(d, m->ring.rb.ring, 0, m->ring.rb.ring_size);
    rdna3_vram_write(d, m->ring.rb.wb, zero, sizeof(zero));
    rdna3_vram_fill(d, m->wb, 0, 256);
    /* mes_v11_0_queue_init(AMDGPU_MES_SCHED_PIPE) */
    mes_mqd_init(d, &m->ring);
    kiq_map_mes(d, kiq, &m->ring);
    ret = kiq_ring_test(d, kiq);   /* mes_v11_0_kiq_enable_queue */
    if (ret) {
        RERR(d, "MES: the KIQ did not process MAP_QUEUES for the scheduler pipe");
        return ret;
    }
    m->ring.mapped = 1;
    /* mes_v11_0_get_fw_version: CP_MES_GP3_LO of each pipe */
    grbm_select(d, 3, 0, 0, 0);
    m->sched_version = RG(d, regCP_MES_GP3_LO);
    grbm_select(d, 3, 1, 0, 0);
    m->kiq_version = RG(d, regCP_MES_GP3_LO);
    grbm_select(d, 0, 0, 0, 0);
    m->legacy_queue_map = (m->sched_version & RD_MES_VERSION_MASK) >= RD_MES_LEGACY_MAP_VER;
    ret = mes_set_hw_resources(d, m);
    if (!ret)
        ret = mes_query_sched_status(d, m);
    if (ret) {
        RERR(d, "MES: scheduler pipe mapped but silent (firmware 0x%x)", m->sched_version);
        gfx11_mes_sched_stop(d, m);
        return ret;
    }
    m->ready = 1;
    m->ring.rb.ready = 1;
    RLOG(d, "MES: scheduler pipe up, firmware sched 0x%x kiq 0x%x%s, doorbell 0x%x", m->sched_version,
         m->kiq_version, m->legacy_queue_map ? " (legacy queue map)" : "", m->ring.rb.doorbell);
    return RDNA3_OK;
}

/* mes_v11_0_kiq_dequeue */
void gfx11_mes_sched_stop(rdna3_dev *d, rdna3_mes *m)
{
    uint32_t i, v;
    if (!m->ring.mapped)
        return;
    grbm_select(d, 3, m->ring.pipe, 0, 0);
    if (RG(d, regCP_HQD_ACTIVE) & 1) {
        WG(d, regCP_HQD_DEQUEUE_REQUEST, 1);
        for (i = 0; i < d->usec_timeout; i++) {
            if (!(RG(d, regCP_HQD_ACTIVE) & 1))
                break;
            d->os.udelay(d->os.ctx, 1);
        }
    }
    v = RG(d, regCP_HQD_PQ_DOORBELL_CONTROL);
    v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 0);
    v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_HIT, 1);
    WG(d, regCP_HQD_PQ_DOORBELL_CONTROL, v);
    WG(d, regCP_HQD_PQ_DOORBELL_CONTROL, 0);
    WG(d, regCP_HQD_PQ_WPTR_LO, 0);
    WG(d, regCP_HQD_PQ_WPTR_HI, 0);
    WG(d, regCP_HQD_PQ_RPTR, 0);
    grbm_select(d, 0, 0, 0, 0);
    m->ring.mapped = 0;
    m->ready = 0;
    m->ring.rb.ready = 0;
}

/* --------------------------- user queues --------------------------- */

/* struct v11_gfx_mqd / v11_compute_mqd fields user queues add (v11_structs.h) */
#define MQD_GFX_SHADOW_BASE_LO         0
#define MQD_GFX_SHADOW_BASE_HI         1
#define MQD_GFX_GDS_BKUP_BASE_LO       2
#define MQD_GFX_GDS_BKUP_BASE_HI       3
#define MQD_GFX_FW_WORK_AREA_BASE_LO   4
#define MQD_GFX_FW_WORK_AREA_BASE_HI   5
#define MQD_GFX_FENCE_ADDRESS_LO       510
#define MQD_GFX_FENCE_ADDRESS_HI       511
#define MQD_COMPUTE_FENCE_ADDRESS_LO   446
#define MQD_COMPUTE_FENCE_ADDRESS_HI   447

/* Ported from gfx_v11_0_gfx_mqd_set_priority: 0 low, 1 high */
static uint32_t gfx_mqd_priority(const amd_mqd_prop *p)
{
    uint32_t tmp = regCP_GFX_HQD_QUEUE_PRIORITY_DEFAULT;
    return REG_SET_FIELD(tmp, CP_GFX_HQD_QUEUE_PRIORITY, PRIORITY_LEVEL,
                         p->hqd_pipe_priority == AMD_GFX_PIPE_PRIO_HIGH ? 1 : 0);
}

/* Ported from gfx_v11_0_gfx_mqd_init */
int gfx11_gfx_mqd_init(rdna3_dev *d, uint32_t *mqd, const amd_mqd_prop *p)
{
    uint64_t hqd_gpu_addr, wb_gpu_addr;
    uint32_t tmp, rb_bufsz;
    (void)d;

    /* set up gfx hqd wptr */
    mqd[MQD_GFX_HQD_WPTR] = 0;
    mqd[MQD_GFX_HQD_WPTR_HI] = 0;
    /* set the pointer to the MQD */
    mqd[MQD_CP_MQD_BASE_ADDR_LO] = (uint32_t)p->mqd_gpu_addr & 0xfffffffc;
    mqd[MQD_CP_MQD_BASE_ADDR_HI] = (uint32_t)(p->mqd_gpu_addr >> 32);
    /* set up mqd control */
    tmp = regCP_GFX_MQD_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
    mqd[MQD_GFX_MQD_CONTROL] = tmp;
    /* the ring buffer's vmid: 0, the MES sets the queue's */
    mqd[MQD_GFX_HQD_VMID] = 0;
    mqd[MQD_GFX_HQD_QUEUE_PRIORITY] = gfx_mqd_priority(p);
    /* set up time quantum */
    tmp = regCP_GFX_HQD_QUANTUM_DEFAULT;
    mqd[MQD_GFX_HQD_QUANTUM] = REG_SET_FIELD(tmp, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
    /* set up gfx hqd base, this is similar as CP_RB_BASE */
    hqd_gpu_addr = p->hqd_base_gpu_addr >> 8;
    mqd[MQD_GFX_HQD_BASE] = (uint32_t)hqd_gpu_addr;
    mqd[MQD_GFX_HQD_BASE_HI] = (uint32_t)(hqd_gpu_addr >> 32);
    /* set up hqd_rptr_addr/_hi, similar as CP_RB_RPTR */
    wb_gpu_addr = p->rptr_gpu_addr;
    mqd[MQD_GFX_HQD_RPTR_ADDR] = (uint32_t)wb_gpu_addr & 0xfffffffc;
    mqd[MQD_GFX_HQD_RPTR_ADDR_HI] = (uint32_t)(wb_gpu_addr >> 32) & 0xffff;
    /* set up rb_wptr_poll addr */
    wb_gpu_addr = p->wptr_gpu_addr;
    mqd[MQD_GFX_RB_WPTR_POLL_ADDR_LO] = (uint32_t)wb_gpu_addr & 0xfffffffc;
    mqd[MQD_GFX_RB_WPTR_POLL_ADDR_HI] = (uint32_t)(wb_gpu_addr >> 32) & 0xffff;
    /* set up the gfx_hqd_control, similar as CP_RB0_CNTL */
    rb_bufsz = order_base_2(p->queue_size / 4) - 1;
    tmp = regCP_GFX_HQD_CNTL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, rb_bufsz - 2);
    if (p->tmz_queue)
        tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, TMZ_MATCH, 1);
    mqd[MQD_GFX_HQD_CNTL] = tmp;
    /* set up cp_doorbell_control */
    tmp = regCP_RB_DOORBELL_CONTROL_DEFAULT;
    if (p->use_doorbell) {
        tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, p->doorbell_index);
        tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
    } else {
        tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
    }
    mqd[MQD_GFX_RB_DOORBELL_CONTROL] = tmp;
    /* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR */
    mqd[MQD_GFX_HQD_RPTR] = regCP_GFX_HQD_RPTR_DEFAULT;
    /* activate the queue */
    mqd[MQD_GFX_HQD_ACTIVE] = 1;
    /* set gfx UQ items */
    mqd[MQD_GFX_SHADOW_BASE_LO] = (uint32_t)p->shadow_addr;
    mqd[MQD_GFX_SHADOW_BASE_HI] = (uint32_t)(p->shadow_addr >> 32);
    mqd[MQD_GFX_GDS_BKUP_BASE_LO] = (uint32_t)p->gds_bkup_addr;
    mqd[MQD_GFX_GDS_BKUP_BASE_HI] = (uint32_t)(p->gds_bkup_addr >> 32);
    mqd[MQD_GFX_FW_WORK_AREA_BASE_LO] = (uint32_t)p->csa_addr;
    mqd[MQD_GFX_FW_WORK_AREA_BASE_HI] = (uint32_t)(p->csa_addr >> 32);
    mqd[MQD_GFX_FENCE_ADDRESS_LO] = (uint32_t)p->fence_address;
    mqd[MQD_GFX_FENCE_ADDRESS_HI] = (uint32_t)(p->fence_address >> 32);
    return RDNA3_OK;
}

/* Ported from gfx_v11_0_compute_mqd_init */
int gfx11_compute_mqd_init(rdna3_dev *d, uint32_t *mqd, const amd_mqd_prop *p)
{
    uint64_t hqd_gpu_addr, wb_gpu_addr, eop_base_addr;
    uint32_t tmp;
    (void)d;

    mqd[MQD_HEADER] = 0xC0310800;
    mqd[MQD_COMPUTE_PIPELINESTAT_ENABLE] = 0x00000001;
    mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE0] = 0xffffffff;
    mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE1] = 0xffffffff;
    mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE2] = 0xffffffff;
    mqd[MQD_COMPUTE_STATIC_THREAD_MGMT_SE3] = 0xffffffff;
    mqd[MQD_COMPUTE_MISC_RESERVED] = 0x00000007;

    eop_base_addr = p->eop_gpu_addr >> 8;
    mqd[MQD_CP_HQD_EOP_BASE_ADDR_LO] = (uint32_t)eop_base_addr;
    mqd[MQD_CP_HQD_EOP_BASE_ADDR_HI] = (uint32_t)(eop_base_addr >> 32);
    /* set the EOP size, register value is 2^(EOP_SIZE+1) dwords */
    tmp = regCP_HQD_EOP_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE, order_base_2(RD_MEC_HPD_SIZE / 4) - 1);
    mqd[MQD_CP_HQD_EOP_CONTROL] = tmp;

    /* disable the queue if it's active */
    mqd[MQD_CP_HQD_DEQUEUE_REQUEST] = 0;
    mqd[MQD_CP_HQD_PQ_RPTR] = 0;
    mqd[MQD_CP_HQD_PQ_WPTR_LO] = 0;
    mqd[MQD_CP_HQD_PQ_WPTR_HI] = 0;
    /* set the pointer to the MQD */
    mqd[MQD_CP_MQD_BASE_ADDR_LO] = (uint32_t)p->mqd_gpu_addr & 0xfffffffc;
    mqd[MQD_CP_MQD_BASE_ADDR_HI] = (uint32_t)(p->mqd_gpu_addr >> 32);
    /* set MQD vmid to 0 */
    tmp = regCP_MQD_CONTROL_DEFAULT;
    mqd[MQD_CP_MQD_CONTROL] = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, 0);
    /* set the pointer to the HQD, this is similar CP_RB0_BASE/_HI */
    hqd_gpu_addr = p->hqd_base_gpu_addr >> 8;
    mqd[MQD_CP_HQD_PQ_BASE_LO] = (uint32_t)hqd_gpu_addr;
    mqd[MQD_CP_HQD_PQ_BASE_HI] = (uint32_t)(hqd_gpu_addr >> 32);
    /* set up the HQD, this is similar to CP_RB0_CNTL */
    tmp = regCP_HQD_PQ_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE, order_base_2(p->queue_size / 4) - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, order_base_2(4096 / 4) - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, p->allow_tunneling ? 1 : 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    if (p->tmz_queue)
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TMZ, 1);
    mqd[MQD_CP_HQD_PQ_CONTROL] = tmp;
    /* set the wb address whether it's enabled or not */
    wb_gpu_addr = p->rptr_gpu_addr;
    mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_LO] = (uint32_t)wb_gpu_addr & 0xfffffffc;
    mqd[MQD_CP_HQD_PQ_RPTR_REPORT_ADDR_HI] = (uint32_t)(wb_gpu_addr >> 32) & 0xffff;
    /* only used if CP_PQ_WPTR_POLL_CNTL.CP_PQ_WPTR_POLL_CNTL__EN_MASK=1 */
    wb_gpu_addr = p->wptr_gpu_addr;
    mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_LO] = (uint32_t)wb_gpu_addr & 0xfffffffc;
    mqd[MQD_CP_HQD_PQ_WPTR_POLL_ADDR_HI] = (uint32_t)(wb_gpu_addr >> 32) & 0xffff;
    /* enable the doorbell if requested */
    tmp = 0;
    if (p->use_doorbell) {
        tmp = regCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, p->doorbell_index);
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_SOURCE, 0);
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_HIT, 0);
    }
    mqd[MQD_CP_HQD_PQ_DOORBELL_CONTROL] = tmp;
    /* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR */
    mqd[MQD_CP_HQD_PQ_RPTR] = regCP_HQD_PQ_RPTR_DEFAULT;
    /* set the vmid for the queue */
    mqd[MQD_CP_HQD_VMID] = 0;
    tmp = regCP_HQD_PERSISTENT_STATE_DEFAULT;
    mqd[MQD_CP_HQD_PERSISTENT_STATE] = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    /* set MIN_IB_AVAIL_SIZE */
    tmp = regCP_HQD_IB_CONTROL_DEFAULT;
    mqd[MQD_CP_HQD_IB_CONTROL] = REG_SET_FIELD(tmp, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
    /* set static priority for a compute queue/ring */
    mqd[MQD_CP_HQD_PIPE_PRIORITY] = p->hqd_pipe_priority;
    mqd[MQD_CP_HQD_QUEUE_PRIORITY] = p->hqd_queue_priority;
    mqd[MQD_CP_HQD_ACTIVE] = p->hqd_active ? 1 : 0;
    /* set UQ fenceaddress */
    mqd[MQD_COMPUTE_FENCE_ADDRESS_LO] = (uint32_t)p->fence_address;
    mqd[MQD_COMPUTE_FENCE_ADDRESS_HI] = (uint32_t)(p->fence_address >> 32);
    return RDNA3_OK;
}

/* the GC 11.0.3 user queue gate of gfx_v11_0's gpu_early_init (firmware levels;
 * amdgpu.user_queue=1 is the kext's rdna3userq=1) */
int gfx11_userq_supported(uint32_t me_ver, uint32_t pfp_ver, uint32_t mec_ver, uint32_t mes_ver)
{
    return me_ver >= 2390 && pfp_ver >= 2530 && mec_ver >= 2600 && mes_ver >= 120;
}

/* Ported from convert_to_mes_queue_type: AMDGPU_RING_TYPE_GFX / COMPUTE /
 * SDMA (the user queue's AMDGPU_HW_IP_* has the same numbers) to
 * MES_QUEUE_TYPE_*; -1 for anything else (amdgpu: BUG()) */
static int mes_queue_type(uint32_t queue_type)
{
    switch (queue_type) {
    case 0: return 0;   /* MES_QUEUE_TYPE_GFX */
    case 1: return 1;   /* MES_QUEUE_TYPE_COMPUTE */
    case 2: return 2;   /* MES_QUEUE_TYPE_SDMA */
    default: return -1;
    }
}

/* Ported from convert_to_mes_priority_level: AMDGPU_MES_PRIORITY_LEVEL_* to
 * MES_AMD_PRIORITY_LEVEL_* (the same numbers; NORMAL for anything else) */
static uint32_t mes_priority_level(uint32_t level)
{
    return level <= AMD_MES_PRIORITY_LEVEL_REALTIME ? level : AMD_MES_PRIORITY_LEVEL_NORMAL;
}

/* Ported from mes_v11_0_add_hw_queue */
int gfx11_mes_add_hw_queue(rdna3_dev *d, rdna3_mes *m, const amd_mes_add_queue *in,
                           uint32_t vm_cntx_cntl)
{
    uint32_t p[RD_MES_API_DWORDS], i;
    int type = mes_queue_type(in->queue_type);
    if (!m->ready || type < 0)
        return RDNA3_EINVAL;
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        p[i] = 0;
    p[0] = RD_MES_API_HEADER(RD_MES_SCH_API_ADD_QUEUE);
    p[1] = in->process_id;
    put64(p + 2, in->page_table_base_addr);
    put64(p + 4, in->process_va_start);
    put64(p + 6, in->process_va_end);
    put64(p + 8, in->process_quantum);
    put64(p + 10, in->process_context_addr);
    put64(p + 12, in->gang_quantum);
    put64(p + 14, in->gang_context_addr);
    p[16] = mes_priority_level(in->inprocess_gang_priority);
    p[17] = mes_priority_level(in->gang_global_priority_level);
    p[18] = in->doorbell_offset;
    put64(p + 19, in->mqd_addr);
    if (((m->sched_version & RD_MES_API_VERSION_MASK) >> RD_MES_API_VERSION_SHIFT) >= 2)
        put64(p + 21, in->wptr_mc_addr);
    else
        put64(p + 21, in->wptr_addr);
    p[27] = (uint32_t)type;            /* queue_type */
    /* For KFD, gds_size is re-used for queue size (needed in MES for AQL queues) */
    p[29] = in->queue_size;            /* gds_size */
    p[30] = in->gws_base;
    p[31] = in->gws_size;
    put64(p + 33, in->tba_addr);       /* trap_handler_addr */
    p[35] = vm_cntx_cntl;
    p[36] = (in->paging ? 1u << 0 : 0) | (in->is_kfd_process ? 1u << 9 : 0) |
            (in->trap_en ? 1u << 10 : 0) | (in->is_aql_queue ? 1u << 11 : 0) |
            (in->skip_process_ctx_clear ? 1u << 12 : 0) |
            (in->exclusively_scheduled ? 1u << 14 : 0);
    put64(p + 41, in->tma_addr);
    return gfx11_mes_submit(d, m, p, RD_MES_ADD_QUEUE_STATUS_DW);
}

/* Ported from mes_v11_0_remove_hw_queue */
int gfx11_mes_remove_hw_queue(rdna3_dev *d, rdna3_mes *m, const amd_mes_remove_queue *in)
{
    uint32_t p[RD_MES_API_DWORDS], i;
    if (!m->ready)
        return RDNA3_EINVAL;
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        p[i] = 0;
    p[0] = RD_MES_API_HEADER(RD_MES_SCH_API_REMOVE_QUEUE);
    p[1] = in->doorbell_offset;
    put64(p + 2, in->gang_context_addr);
    return gfx11_mes_submit(d, m, p, RD_MES_REMOVE_QUEUE_STATUS_DW);
}

/* gfx11_kiq_set_resources + gfx11_kiq_map_queues (amdgpu_gfx_enable_kcq) */
static void kiq_map_kcq(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq)
{
    rdna3_gfx_ring *k = &kiq->rb;
        /* amdgpu_queue_mask_bit_to_set_resource_bit */
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

/* gfx_v11_0_gfx_mqd_init for gfx ring 0 (me 0 pipe 0 queue 0), normal priority */
static void gfx_mqd_init(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint64_t mqd = rdna3_vram_mc(d, r->mqd), hqd = rdna3_vram_mc(d, r->ring) >> 8, wb;
    uint32_t tmp, rb_bufsz, i;

    for (i = 0; i < RD_MQD_DWORDS; i++)
        g_mqd[i] = 0;
    grbm_select(d, 0, 0, 0, 0);
    g_mqd[MQD_GFX_HQD_WPTR] = 0;
    g_mqd[MQD_GFX_HQD_WPTR_HI] = 0;
    g_mqd[MQD_CP_MQD_BASE_ADDR_LO] = (uint32_t)mqd & 0xfffffffc;
    g_mqd[MQD_CP_MQD_BASE_ADDR_HI] = (uint32_t)(mqd >> 32);
    tmp = RG(d, regCP_GFX_MQD_CONTROL);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
    g_mqd[MQD_GFX_MQD_CONTROL] = tmp;
    g_mqd[MQD_GFX_HQD_VMID] = 0;
    tmp = RG(d, regCP_GFX_HQD_QUEUE_PRIORITY);
    g_mqd[MQD_GFX_HQD_QUEUE_PRIORITY] = REG_SET_FIELD(tmp, CP_GFX_HQD_QUEUE_PRIORITY, PRIORITY_LEVEL, 0);
    tmp = RG(d, regCP_GFX_HQD_QUANTUM);
    g_mqd[MQD_GFX_HQD_QUANTUM] = REG_SET_FIELD(tmp, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
    g_mqd[MQD_GFX_HQD_BASE] = (uint32_t)hqd;
    g_mqd[MQD_GFX_HQD_BASE_HI] = (uint32_t)(hqd >> 32);
    wb = rdna3_vram_mc(d, r->wb + RD_GFX_WB_RPTR);
    g_mqd[MQD_GFX_HQD_RPTR_ADDR] = (uint32_t)wb & 0xfffffffc;
    g_mqd[MQD_GFX_HQD_RPTR_ADDR_HI] = (uint32_t)(wb >> 32) & 0xffff;
    wb = rdna3_vram_mc(d, r->wb + RD_GFX_WB_WPTR);
    g_mqd[MQD_GFX_RB_WPTR_POLL_ADDR_LO] = (uint32_t)wb & 0xfffffffc;
    g_mqd[MQD_GFX_RB_WPTR_POLL_ADDR_HI] = (uint32_t)(wb >> 32) & 0xffff;
    rb_bufsz = order_base_2(r->ring_size / 4) - 1;
    tmp = RG(d, regCP_GFX_HQD_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, rb_bufsz - 2);
    g_mqd[MQD_GFX_HQD_CNTL] = tmp;
    tmp = RG(d, regCP_RB_DOORBELL_CONTROL);
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, r->doorbell);
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
    g_mqd[MQD_GFX_RB_DOORBELL_CONTROL] = tmp;
    g_mqd[MQD_GFX_HQD_RPTR] = RG(d, regCP_GFX_HQD_RPTR);
    g_mqd[MQD_GFX_HQD_ACTIVE] = 1;
    rdna3_vram_write(d, r->mqd, g_mqd, sizeof(g_mqd));
    /* gfx_v11_0_kgq_init_queue: memcpy_fromio(mqd_backup, mqd) */
    if (r->mqd_backup)
        rdna3_vram_write(d, r->mqd_backup, g_mqd, sizeof(g_mqd));
    rdna3_hdp_flush(d);
}

/* gfx11_kiq_map_queues for the gfx ring (amdgpu_gfx_enable_kgq) */
static void kiq_map_kgq(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_gfx_ring *r)
{
    rdna3_gfx_ring *k = &kiq->rb;
    uint64_t mqd = rdna3_vram_mc(d, r->mqd), wptr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_WPTR);
    gfx11_write(d, k, PACKET3(PACKET3_MAP_QUEUES, 5));
    gfx11_write(d, k, (0u << 4) /* QUEUE_SEL */ | (0u << 8) /* VMID */ | (0u << 13) /* queue */ |
                      (0u << 16) /* pipe */ | (0u << 18) /* me 0 */ | (0u << 21) /* normal */ |
                      (0u << 24) /* all on one pipe */ | (4u << 26) /* ENGINE_SEL gfx */ |
                      (1u << 29) /* NUM_QUEUES */);
    gfx11_write(d, k, r->doorbell << 2);
    gfx11_write(d, k, (uint32_t)mqd);
    gfx11_write(d, k, (uint32_t)(mqd >> 32));
    gfx11_write(d, k, (uint32_t)wptr);
    gfx11_write(d, k, (uint32_t)(wptr >> 32));
    gfx11_commit(d, k);
}

/* gfx_v11_0_cp_async_gfx_ring_resume + the gfx ring test */
int gfx11_kgq_start(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_gfx_ring *r)
{
    uint8_t zero[16] = { 0 };
    int ret;
    if (!r->async || !r->mqd || !r->ring_size || !kiq || !kiq->rb.ready || r->kind != RD_RING_GFX)
        return RDNA3_EINVAL;
    r->ready = 0;
    r->wptr = 0;
    r->use_doorbell = 1;
    r->doorbell = RD_DB_INDEX(RD_DB_GFX_RING0);
    rdna3_vram_fill(d, r->ring, 0, r->ring_size);
    rdna3_vram_write(d, r->wb + RD_GFX_WB_RPTR, zero, sizeof(zero));   /* rptr + wptr poll */
    shadow_state_reset(r);
    gfx_mqd_init(d, r);   /* gfx_v11_0_kgq_init_queue (a fresh MQD) */
    kiq_map_kgq(d, kiq, r);
    ret = kiq_ring_test(d, kiq);   /* amdgpu_gfx_enable_kgq */
    if (ret) {
        RERR(d, "GFX: the KIQ did not process MAP_QUEUES for the gfx ring");
        return ret;
    }
    cp_gfx_start(d, r);
    ret = gfx11_ring_test(d, r);
    if (ret)
        return ret;
    r->ready = 1;
    RLOG(d, "GFX: ring 0 %u KiB at MC 0x%llx mapped by the KIQ (MQD 0x%llx, doorbell 0x%x), "
         "ring test passed", r->ring_size / 1024, (unsigned long long)rdna3_vram_mc(d, r->ring),
         (unsigned long long)rdna3_vram_mc(d, r->mqd), r->doorbell);
    return RDNA3_OK;
}

/* amdgpu_gfx_disable_kgq (gfx11_kiq_unmap_queues PREEMPT_QUEUES, ENGINE_SEL 4),
 * then the gfx CP halts as in gfx_v11_0_hw_fini */
void gfx11_kgq_stop(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_gfx_ring *r)
{
    if (r->async && r->ready && kiq && kiq->rb.ready) {
        rdna3_gfx_ring *k = &kiq->rb;
        gfx11_write(d, k, PACKET3(PACKET3_UNMAP_QUEUES, 4));
        gfx11_write(d, k, (0u << 0) /* ACTION preempt */ | (0u << 4) | (4u << 26) | (1u << 29));
        gfx11_write(d, k, r->doorbell << 2);
        gfx11_write(d, k, 0);
        gfx11_write(d, k, 0);
        gfx11_write(d, k, 0);
        gfx11_commit(d, k);
        (void)kiq_ring_test(d, kiq);
    }
    r->ready = 0;
    gfx11_cp_gfx_enable(d, 0);
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

void gfx11_mec_stop(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq, struct rdna3_mes *mes)
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
    if (kiq && kiq->me == 3) {
        /* mes_v11_0_kiq_hw_fini */
        if (mes)
            gfx11_mes_sched_stop(d, mes);
        mes_disable(d);
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
    return gfx11_submit_compute_job_ex(d, kcq, ctx, vmid, pd_addr, ibs, n, NULL);
}

uint32_t gfx11_submit_compute_job_ex(rdna3_dev *d, rdna3_mec_ring *kcq, uint64_t ctx,
                                     uint32_t vmid, uint64_t pd_addr, const rdna3_gfx_ib *ibs,
                                     uint32_t n, const rdna3_gfx_job_ext *x)
{
    rdna3_gfx_ring *r = &kcq->rb;
    /* vm flush 5+5+7, gds switch 20, mem sync 8, hdp 7, user fence 8,
     * fence 8, 4 per IB */
    uint32_t i, seq, ndw = 17 + 20 + 8 + 7 + 8 + 8 + 4 * n;
    if (!r->ready || !n || n > RD_GFX_MAX_IBS || vmid > 15)
        return 0;
    if (d->amdgpu_exact)
        return submit_amdgpu(d, r, ctx, vmid, pd_addr, ibs, n, x);
    if (ring_wait_space(d, r, ndw))
        return 0;
    if (pd_addr)
        gfx11_emit_vm_flush(d, r, vmid, pd_addr);
    if (x && x->gds_switch)
        gfx11_emit_gds_switch(d, r, vmid, x->gds_base, x->gds_size, x->gws_base, x->gws_size,
                              x->oa_base, x->oa_size);
    gfx11_emit_mem_sync(d, r);
    gfx11_emit_hdp_flush(d, r);
    for (i = 0; i < n; i++) {   /* gfx_v11_0_ring_emit_ib_compute */
        gfx11_write(d, r, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
        gfx11_write(d, r, (uint32_t)ibs[i].va & ~3u);
        gfx11_write(d, r, (uint32_t)(ibs[i].va >> 32));
        gfx11_write(d, r, INDIRECT_BUFFER_VALID | ibs[i].ndw | (vmid << 24));
    }
    if (x && x->uf_addr)
        gfx11_emit_user_fence(d, r, x->uf_addr, (uint32_t)(r->fence_seq + 1));
    seq = gfx11_emit_fence(d, r);
    gfx11_commit(d, r);
    r->last_ctx = ctx;
    r->last_vmid = vmid;
    return seq;
}

/* ------------------------- clock gating ------------------------- */

static void rlc_safe_mode(rdna3_dev *d, int enter);

int gfx11_set_clock_gating(rdna3_dev *d, int enable, uint32_t flags)
{
    const uint32_t cp_int = CP_INT_CNTL__CNTX_BUSY_INT_ENABLE_MASK | CP_INT_CNTL__CNTX_EMPTY_INT_ENABLE_MASK |
                            CP_INT_CNTL__CMP_BUSY_INT_ENABLE_MASK | CP_INT_CNTL__GFX_IDLE_INT_ENABLE_MASK;
    uint32_t def, v;
    /* gfx_v11_0_is_rlc_enabled: amdgpu_gfx_rlc_enter_safe_mode does nothing without it */
    if (!(RG(d, regRLC_CNTL) & RLC_CNTL__RLC_ENABLE_F32_MASK)) {
        RERR(d, "GFX: clock gating: the RLC is not running");
        return RDNA3_ENODEV;
    }
    rlc_safe_mode(d, 1);

    /* gfx_v11_0_update_coarse_grain_clock_gating */
    if (flags & (RD_GFX_CG_CGCG | RD_GFX_CG_CGLS)) {
        if (enable) {
            def = v = RG(d, regRLC_CGTT_MGCG_OVERRIDE);
            if (flags & RD_GFX_CG_CGCG)
                v &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGCG_OVERRIDE_MASK;
            if (flags & RD_GFX_CG_CGLS)
                v &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK;
            if (v != def)
                WG(d, regRLC_CGTT_MGCG_OVERRIDE, v);
            /* the CGCG FSM (0x0000363F) */
            def = v = RG(d, regRLC_CGCG_CGLS_CTRL);
            if (flags & RD_GFX_CG_CGCG) {
                v &= ~RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD_MASK;
                v |= (0x36u << RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD__SHIFT) | RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK;
            }
            if (flags & RD_GFX_CG_CGLS) {
                v &= ~RLC_CGCG_CGLS_CTRL__CGLS_REP_COMPANSAT_DELAY_MASK;
                v |= (0xfu << RLC_CGCG_CGLS_CTRL__CGLS_REP_COMPANSAT_DELAY__SHIFT) | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;
            }
            if (v != def)
                WG(d, regRLC_CGCG_CGLS_CTRL, v);
            /* IDLE_POLL_COUNT (0x00900100) */
            def = v = RG(d, regCP_RB_WPTR_POLL_CNTL);
            v &= ~(CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY_MASK | CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK);
            v |= (0x0100u << CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY__SHIFT) |
                 (0x0090u << CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT);
            if (v != def)
                WG(d, regCP_RB_WPTR_POLL_CNTL, v);
            WG(d, regCP_INT_CNTL, RG(d, regCP_INT_CNTL) | cp_int);
            WG(d, regSDMA0_RLC_CGCG_CTRL, RG(d, regSDMA0_RLC_CGCG_CTRL) | SDMA0_RLC_CGCG_CTRL__CGCG_INT_ENABLE_MASK);
            WG(d, regSDMA1_RLC_CGCG_CTRL, RG(d, regSDMA1_RLC_CGCG_CTRL) | SDMA1_RLC_CGCG_CTRL__CGCG_INT_ENABLE_MASK);
        } else {
            def = v = RG(d, regRLC_CGCG_CGLS_CTRL);
            if (flags & RD_GFX_CG_CGCG)
                v &= ~RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK;
            if (flags & RD_GFX_CG_CGLS)
                v &= ~RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;
            if (v != def)
                WG(d, regRLC_CGCG_CGLS_CTRL, v);
            WG(d, regSDMA0_RLC_CGCG_CTRL, RG(d, regSDMA0_RLC_CGCG_CTRL) & ~SDMA0_RLC_CGCG_CTRL__CGCG_INT_ENABLE_MASK);
            WG(d, regSDMA1_RLC_CGCG_CTRL, RG(d, regSDMA1_RLC_CGCG_CTRL) & ~SDMA1_RLC_CGCG_CTRL__CGCG_INT_ENABLE_MASK);
        }
    }
    /* gfx_v11_0_update_medium_grain_clock_gating */
    if (flags & RD_GFX_CG_MGCG) {
        const uint32_t m = RLC_CGTT_MGCG_OVERRIDE__GRBM_CGTT_SCLK_OVERRIDE_MASK |
                           RLC_CGTT_MGCG_OVERRIDE__RLC_CGTT_SCLK_OVERRIDE_MASK |
                           RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK;
        def = v = RG(d, regRLC_CGTT_MGCG_OVERRIDE);
        v = enable ? (v & ~m) : (v | m);
        if (v != def)
            WG(d, regRLC_CGTT_MGCG_OVERRIDE, v);
    }
    /* gfx_v11_0_update_repeater_fgcg */
    if (flags & RD_GFX_CG_REPEATER_FGCG) {
        def = v = RG(d, regRLC_CGTT_MGCG_OVERRIDE);
        if (enable)
            v &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_REPEATER_FGCG_OVERRIDE_MASK;
        else
            v |= RLC_CGTT_MGCG_OVERRIDE__GFXIP_REPEATER_FGCG_OVERRIDE_MASK;
        if (v != def)
            WG(d, regRLC_CGTT_MGCG_OVERRIDE, v);
    }
    /* gfx_v11_0_enable_gui_idle_interrupt: me 0 pipe 0 */
    if (flags & (RD_GFX_CG_CGCG | RD_GFX_CG_CGLS | RD_GFX_CG_MGCG)) {
        const uint32_t r0 = CP_INT_CNTL_RING0__CNTX_BUSY_INT_ENABLE_MASK | CP_INT_CNTL_RING0__CNTX_EMPTY_INT_ENABLE_MASK |
                            CP_INT_CNTL_RING0__CMP_BUSY_INT_ENABLE_MASK | CP_INT_CNTL_RING0__GFX_IDLE_INT_ENABLE_MASK;
        v = RG(d, regCP_INT_CNTL_RING0);
        WG(d, regCP_INT_CNTL_RING0, enable ? (v | r0) : (v & ~r0));
    }
    rlc_safe_mode(d, 0);
    RLOG(d, "GFX: clock gating %s (flags 0x%x): RLC_CGTT_MGCG_OVERRIDE 0x%08x RLC_CGCG_CGLS_CTRL 0x%08x",
         enable ? "on" : "off", flags, RG(d, regRLC_CGTT_MGCG_OVERRIDE), RG(d, regRLC_CGCG_CGLS_CTRL));
    return RDNA3_OK;
}

/* ------------------------- hangs and recovery ------------------------- */

void gfx11_read_status(rdna3_dev *d, const rdna3_gfx_ring *r, rdna3_gfx_status *st)
{
    uint32_t wb = 0;
    st->grbm_status = RG(d, regGRBM_STATUS);
    st->grbm_status2 = RG(d, regGRBM_STATUS2);
    st->grbm_status_se0 = RG(d, regGRBM_STATUS_SE0);
    st->cp_stat = RG(d, regCP_STAT);
    st->cp_stalled_stat1 = RG(d, regCP_STALLED_STAT1);
    st->cp_cpc_status = RG(d, regCP_CPC_STATUS);
    st->cp_cpc_stalled_stat1 = RG(d, regCP_CPC_STALLED_STAT1);
    st->cp_cpf_status = RG(d, regCP_CPF_STATUS);
    st->cp_me_cntl = RG(d, regCP_ME_CNTL);
    st->rb0_rptr = RG(d, regCP_RB0_RPTR);
    st->rb0_wptr = RG(d, regCP_RB0_WPTR);
    st->cp_int_cntl = RG(d, regCP_INT_CNTL);
    st->rlc_safe_mode = RG(d, regRLC_SAFE_MODE);
    st->sdma0_status = rdna3_rreg(d, rdna3_reg_offset(d, HWIP_GC, 0, regSDMA0_STATUS_REG_BASE_IDX,
                                                      regSDMA0_STATUS_REG));
    if (r && r->wb != UINT64_MAX)
        rdna3_vram_read(d, r->wb + RD_GFX_WB_RPTR, &wb, 4);
    st->rb0_rptr_wb = wb;
    st->rlc_stat = RG(d, regRLC_STAT);
    st->rlc_gpm_stat = RG(d, regRLC_GPM_STAT);
    st->rlc_pg_cntl = RG(d, regRLC_PG_CNTL);
    st->rlc_cgcg_cgls_ctrl = RG(d, regRLC_CGCG_CGLS_CTRL);
    st->cp_busy_stat = RG(d, regCP_BUSY_STAT);
    st->cp_cpc_busy_stat = RG(d, regCP_CPC_BUSY_STAT);
    st->cp_cpf_busy_stat = RG(d, regCP_CPF_BUSY_STAT);
}

void gfx11_log_status(rdna3_dev *d, const rdna3_gfx_status *st, const char *why)
{
    RERR(d, "GFX %s: GRBM_STATUS 0x%08x (%s) STATUS2 0x%08x SE0 0x%08x", why, st->grbm_status,
         (st->grbm_status & GRBM_STATUS__GUI_ACTIVE_MASK) ? "busy" : "idle", st->grbm_status2,
         st->grbm_status_se0);
    RERR(d, "  CP_STAT 0x%08x STALLED1 0x%08x CPC 0x%08x/0x%08x CPF 0x%08x ME_CNTL 0x%08x",
         st->cp_stat, st->cp_stalled_stat1, st->cp_cpc_status, st->cp_cpc_stalled_stat1,
         st->cp_cpf_status, st->cp_me_cntl);
    RERR(d, "  RB0 rptr 0x%x (wb 0x%x) wptr 0x%x, CP_INT_CNTL 0x%08x, RLC_SAFE_MODE 0x%x, "
         "SDMA0_STATUS 0x%08x", st->rb0_rptr, st->rb0_rptr_wb, st->rb0_wptr, st->cp_int_cntl,
         st->rlc_safe_mode, st->sdma0_status);
    RERR(d, "  RLC_STAT 0x%08x GPM_STAT 0x%08x PG_CNTL 0x%08x CGCG_CGLS 0x%08x CP_BUSY 0x%08x "
         "CPC_BUSY 0x%08x CPF_BUSY 0x%08x", st->rlc_stat, st->rlc_gpm_stat, st->rlc_pg_cntl,
         st->rlc_cgcg_cgls_ctrl, st->cp_busy_stat, st->cp_cpc_busy_stat, st->cp_cpf_busy_stat);
}

int gfx11_is_idle(rdna3_dev *d)
{
    return (RG(d, regGRBM_STATUS) & GRBM_STATUS__GUI_ACTIVE_MASK) == 0;
}

/* gfx_v11_0_set_safe_mode / gfx_v11_0_unset_safe_mode */
static void rlc_safe_mode(rdna3_dev *d, int enter)
{
    uint32_t i, v = RLC_SAFE_MODE__CMD_MASK;
    if (enter)
        v |= 1u << RLC_SAFE_MODE__MESSAGE__SHIFT;
    WG(d, regRLC_SAFE_MODE, v);
    for (i = 0; i < d->usec_timeout; i++) {
        if (!(RG(d, regRLC_SAFE_MODE) & RLC_SAFE_MODE__CMD_MASK))
            return;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GFX: RLC did not %s safe mode", enter ? "enter" : "leave");
}

/* Ported from amdgpu_gfx_rlc_enter_safe_mode / amdgpu_gfx_rlc_exit_safe_mode
 * (GC 11.0.3 has CGCG and MGCG, so the RLC is always asked) */
void gfx11_rlc_safe_mode(rdna3_dev *d, int enter)
{
    if (!(RG(d, regRLC_CNTL) & RLC_CNTL__RLC_ENABLE_F32_MASK))
        return;
    rlc_safe_mode(d, enter);
}

/* gfx_v11_0_ring_soft_recovery */
void gfx11_kill_waves(rdna3_dev *d, uint32_t vmid)
{
    uint32_t v = 0;
    v = REG_SET_FIELD(v, SQ_CMD, CMD, 0x03);        /* SQ_IND_CMD_CMD_KILL */
    v = REG_SET_FIELD(v, SQ_CMD, MODE, 0x01);       /* SQ_IND_CMD_MODE_BROADCAST */
    v = REG_SET_FIELD(v, SQ_CMD, CHECK_VMID, 1);
    v = REG_SET_FIELD(v, SQ_CMD, VM_ID, vmid);
    rlc_safe_mode(d, 1);
    WG(d, regSQ_CMD, v);
    rlc_safe_mode(d, 0);
    RLOG(d, "GFX: killed the waves of VMID %u (soft recovery)", vmid);
}

/* amdgpu_fence_driver_force_completion -> amdgpu_fence_write(ring, sync_seq) */
void gfx11_force_fence_completion(rdna3_dev *d, rdna3_gfx_ring *r)
{
    if (r->wb == UINT64_MAX)
        return;
    rdna3_vram_write(d, r->wb + RD_GFX_WB_FENCE, &r->fence_seq, 4);
}

int gfx11_soft_reset(rdna3_dev *d, rdna3_gfx_ring *r, struct rdna3_mec_ring *kiq,
                     struct rdna3_mec_ring *kcq)
{
    uint32_t tmp, i, pipe, queue;
    uint8_t zero[8] = { 0 };
    int ret;

    rlc_safe_mode(d, 1);

    tmp = RG(d, regCP_INT_CNTL);
    tmp &= ~(CP_INT_CNTL__CMP_BUSY_INT_ENABLE_MASK | CP_INT_CNTL__CNTX_BUSY_INT_ENABLE_MASK |
             CP_INT_CNTL__CNTX_EMPTY_INT_ENABLE_MASK | CP_INT_CNTL__GFX_IDLE_INT_ENABLE_MASK);
    WG(d, regCP_INT_CNTL, tmp);

    /* dequeue every compute HQD (MEC1, 4 pipes x 4 queues) and the gfx HQD;
     * rdna3amdgpu=1 runs gfx_v11_0_soft_reset's own loop: soc21_grbm_select
     * with me = 0 .. num_mec - 1 */
    for (pipe = 0; pipe < 4; pipe++)
        for (queue = 0; queue < 4; queue++) {
            if (d->amdgpu_exact) {
                for (i = 0; i < 2; i++) {
                    grbm_select(d, i, pipe, queue, 0);
                    WG(d, regCP_HQD_DEQUEUE_REQUEST, 0x2);
                    WG(d, regSPI_COMPUTE_QUEUE_RESET, 0x1);
                }
                continue;
            }
            grbm_select(d, 1, pipe, queue, 0);
            WG(d, regCP_HQD_DEQUEUE_REQUEST, 0x2);
            WG(d, regSPI_COMPUTE_QUEUE_RESET, 0x1);
        }
    grbm_select(d, 0, 0, 0, 0);
    WG(d, regCP_GFX_HQD_DEQUEUE_REQUEST, 0x1);
    grbm_select(d, 0, 0, 0, 0);

    /* rdna3amdgpu=1: try to acquire the gfx mutex before access to
     * CP_VMID_RESET (gfx_v11_0_request_gfx_index_mutex) */
    if (d->amdgpu_exact && gfx11_request_gfx_index_mutex(d, 1)) {
        RERR(d, "GFX: Failed to acquire the gfx mutex during soft reset");
        rlc_safe_mode(d, 0);
        return RDNA3_EINVAL;
    }
    /* reset every VMID but 0; three reads give GFX_HQD_ACTIVE time to drop */
    WG(d, regCP_VMID_RESET, 0xfffffffe);
    (void)RG(d, regCP_VMID_RESET);
    (void)RG(d, regCP_VMID_RESET);
    (void)RG(d, regCP_VMID_RESET);
    /* release the gfx mutex */
    if (d->amdgpu_exact && gfx11_request_gfx_index_mutex(d, 0)) {
        RERR(d, "GFX: Failed to release the gfx mutex during soft reset");
        rlc_safe_mode(d, 0);
        return RDNA3_EINVAL;
    }
    for (i = 0; i < d->usec_timeout; i++) {
        if (!RG(d, regCP_HQD_ACTIVE) && !RG(d, regCP_GFX_HQD_ACTIVE))
            break;
        d->os.udelay(d->os.ctx, 1);
    }
    if (i >= d->usec_timeout) {
        RERR(d, "GFX: soft reset: the pipes would not drain");
        rlc_safe_mode(d, 0);
        return RDNA3_ETIME;
    }

    tmp = RG(d, regGRBM_SOFT_RESET);
    tmp |= GRBM_SOFT_RESET__SOFT_RESET_CP_MASK | GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK |
           GRBM_SOFT_RESET__SOFT_RESET_CPF_MASK | GRBM_SOFT_RESET__SOFT_RESET_CPC_MASK |
           GRBM_SOFT_RESET__SOFT_RESET_CPG_MASK;
    WG(d, regGRBM_SOFT_RESET, tmp);
    tmp = RG(d, regGRBM_SOFT_RESET);
    tmp &= ~(GRBM_SOFT_RESET__SOFT_RESET_CP_MASK | GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK |
             GRBM_SOFT_RESET__SOFT_RESET_CPF_MASK | GRBM_SOFT_RESET__SOFT_RESET_CPC_MASK |
             GRBM_SOFT_RESET__SOFT_RESET_CPG_MASK);
    WG(d, regGRBM_SOFT_RESET, tmp);

    tmp = RG(d, regCP_SOFT_RESET_CNTL);
    tmp |= CP_SOFT_RESET_CNTL__CMP_HQD_REG_RESET_MASK;
    WG(d, regCP_SOFT_RESET_CNTL, tmp);
    WG(d, regCP_ME_CNTL, 0);
    WG(d, regCP_MEC_RS64_CNTL, 0);
    for (i = 0; i < d->usec_timeout; i++) {
        if (!RG(d, regCP_VMID_RESET))
            break;
        d->os.udelay(d->os.ctx, 1);
    }
    if (i >= d->usec_timeout) {
        RERR(d, "GFX: soft reset: CP_VMID_RESET stuck at 0x%x", RG(d, regCP_VMID_RESET));
        rlc_safe_mode(d, 0);
        return RDNA3_ETIME;
    }

    tmp = RG(d, regCP_INT_CNTL);
    tmp |= CP_INT_CNTL__CMP_BUSY_INT_ENABLE_MASK | CP_INT_CNTL__CNTX_BUSY_INT_ENABLE_MASK |
           CP_INT_CNTL__CNTX_EMPTY_INT_ENABLE_MASK | CP_INT_CNTL__GFX_IDLE_INT_ENABLE_MASK;
    WG(d, regCP_INT_CNTL, tmp);
    rlc_safe_mode(d, 0);

    /* gfx_v11_0_cp_resume: ring 0 from scratch (the rptr writeback must
     * read 0 again, the fence slot keeps the last completed seq) */
    if (d->doorbell_ok)
        cp_set_doorbell_range(d);
    r->ready = 0;
    rdna3_vram_write(d, r->wb + RD_GFX_WB_RPTR, zero, 8);
    rdna3_hdp_flush(d);
    if (r->async && !kiq) {
        RERR(d, "GFX: the gfx queue needs the KIQ to come back after soft reset");
        return RDNA3_EINVAL;
    }
    if (r->async) {
        cp_compute_enable(d, 1);
        gfx11_cp_gfx_enable(d, 1);
    } else {
        cp_gfx_resume(d, r);
        cp_gfx_start(d, r);
        ret = gfx11_ring_test(d, r);
        if (ret) {
            RERR(d, "GFX: ring test failed after soft reset");
            return ret;
        }
        r->ready = 1;
    }
    r->last_ctx = 0;
    if (kiq) {
        kiq->mapped = 0;
        kiq->rb.ready = 0;
        if (kcq) {
            kcq->mapped = 0;
            kcq->rb.ready = 0;
            rdna3_vram_write(d, kcq->rb.wb + RD_GFX_WB_RPTR, zero, 8);
        }
        rdna3_vram_write(d, kiq->rb.wb + RD_GFX_WB_RPTR, zero, 8);
        rdna3_hdp_flush(d);
        ret = gfx11_kiq_start(d, kiq);
        if (!ret && kcq)
            ret = gfx11_kcq_start(d, kiq, kcq);
        if (ret) {
            RERR(d, "GFX: compute queues did not come back after soft reset (%d)", ret);
            return ret;
        }
        if (r->async) {
            ret = gfx11_kgq_start(d, kiq, r);
            if (ret) {
                RERR(d, "GFX: gfx queue did not come back after soft reset (%d)", ret);
                return ret;
            }
        }
    }
    RLOG(d, "GFX: soft reset done, rings back up");
    return RDNA3_OK;
}

/* Ported from gfx_v11_0_set_userq_eop_interrupts: EOP interrupts on every
 * pipe the MES may put a user queue on - ME0 pipe 0 for GFX, MEC1 pipes 0-3
 * for compute (amdgpu_irq_get per pipe; the kernel rings' own enables are
 * the same bits, so this never turns theirs off unless 'enable' is 0) */
void gfx11_set_userq_eop_interrupts(rdna3_dev *d, uint32_t ip_mask, int enable)
{
    rdna3_gfx_ring r;
    uint32_t p;

    for (p = 0; p < sizeof(r); p++)
        ((uint8_t *)&r)[p] = 0;
    if (ip_mask & (1u << 0)) {   /* AMDGPU_HW_IP_GFX */
        r.kind = RD_RING_GFX;
        gfx11_set_eop_interrupt(d, &r, enable);
    }
    if (ip_mask & (1u << 1)) {   /* AMDGPU_HW_IP_COMPUTE */
        r.kind = RD_RING_COMPUTE;
        for (p = 0; p < 4; p++) {
            r.pipe = p;
            gfx11_set_eop_interrupt(d, &r, enable);
        }
    }
}

/* ------------------------- queue topology ------------------------- */

static void bm_set(uint64_t *bm, uint32_t bit)
{
    if (bit < RD_MAX_QUEUES)
        bm[bit / 64] |= 1ull << (bit % 64);
}

static int bm_test(const uint64_t *bm, uint32_t bit)
{
    return bit < RD_MAX_QUEUES && ((bm[bit / 64] >> (bit % 64)) & 1);
}

static uint32_t bm_weight(const uint64_t *bm)
{
    uint32_t i, n = 0;
    for (i = 0; i < RD_MAX_QUEUES; i++)
        n += (uint32_t)bm_test(bm, i);
    return n;
}

/* sprintf(ring->name, "<prefix>_%d.%d.%d", me, pipe, queue) */
static void ring_name(char *out, const char *prefix, uint32_t me, uint32_t pipe, uint32_t queue)
{
    const uint32_t v[3] = { me, pipe, queue };
    uint32_t n = 0, i;
    while (*prefix && n < 8)
        out[n++] = *prefix++;
    out[n++] = '_';
    for (i = 0; i < 3; i++) {
        char tmp[10];
        uint32_t k = 0, x = v[i];
        do {
            tmp[k++] = (char)('0' + x % 10);
            x /= 10;
        } while (x && k < 3);
        while (k && n < 15)
            out[n++] = tmp[--k];
        if (i < 2 && n < 15)
            out[n++] = '.';
    }
    out[n] = 0;
}

/* Ported from amdgpu_gfx_get_num_kcq */
int gfx11_get_num_kcq(int num_kcq)
{
    if (num_kcq == -1)
        return 8;
    if (num_kcq > 8 || num_kcq < 0)
        return 8;   /* "set kernel compute queue number to 8 due to invalid parameter" */
    return num_kcq;
}

/* Ported from amdgpu_gfx_mec_queue_to_bit */
int gfx11_mec_queue_to_bit(const rdna3_gfx_topo *t, int mec, int pipe, int queue)
{
    int bit = 0;
    bit += mec * (int)t->mec_pipes * (int)t->mec_queues;
    bit += pipe * (int)t->mec_queues;
    bit += queue;
    return bit;
}

/* Ported from amdgpu_queue_mask_bit_to_mec_queue */
void gfx11_queue_mask_bit_to_mec_queue(const rdna3_gfx_topo *t, int bit, int *mec, int *pipe,
                                       int *queue)
{
    *queue = bit % (int)t->mec_queues;
    *pipe = (bit / (int)t->mec_queues) % (int)t->mec_pipes;
    *mec = (bit / (int)t->mec_queues) / (int)t->mec_pipes;
}

/* Ported from amdgpu_gfx_is_mec_queue_enabled (xcc 0) */
int gfx11_is_mec_queue_enabled(const rdna3_gfx_topo *t, int mec, int pipe, int queue)
{
    return bm_test(t->mec_bitmap, (uint32_t)gfx11_mec_queue_to_bit(t, mec, pipe, queue));
}

/* Ported from amdgpu_gfx_me_queue_to_bit */
int gfx11_me_queue_to_bit(const rdna3_gfx_topo *t, int me, int pipe, int queue)
{
    int bit = 0;
    bit += me * (int)t->me_pipes * (int)t->me_queues;
    bit += pipe * (int)t->me_queues;
    bit += queue;
    return bit;
}

/* Ported from amdgpu_gfx_bit_to_me_queue */
void gfx11_bit_to_me_queue(const rdna3_gfx_topo *t, int bit, int *me, int *pipe, int *queue)
{
    *queue = bit % (int)t->me_queues;
    *pipe = (bit / (int)t->me_queues) % (int)t->me_pipes;
    *me = (bit / (int)t->me_queues) / (int)t->me_pipes;
}

/* Ported from amdgpu_gfx_is_me_queue_enabled */
int gfx11_is_me_queue_enabled(const rdna3_gfx_topo *t, int me, int pipe, int queue)
{
    return bm_test(t->me_bitmap, (uint32_t)gfx11_me_queue_to_bit(t, me, pipe, queue));
}

/* Ported from amdgpu_gfx_is_compute_multipipe_capable (GC 11 > 9.0.0) */
static int is_compute_multipipe_capable(const rdna3_gfx_topo *t)
{
    if (t->compute_multipipe != -1)
        return t->compute_multipipe == 1;   /* "forcing compute pipe policy" */
    if (t->gc_major > 9 || (t->gc_major == 9 && (t->gc_minor || t->gc_rev)))
        return 1;
    return t->num_mec > 1;
}

/* Ported from amdgpu_gfx_is_graphics_multipipe_capable */
static int is_graphics_multipipe_capable(const rdna3_gfx_topo *t)
{
    return t->async_gfx_ring && t->me_pipes > 1;
}

/* Ported from amdgpu_gfx_is_high_priority_compute_queue: the first compute
 * ring when there is more than one */
int gfx11_is_high_priority_compute_queue(const rdna3_gfx_topo *t, uint32_t ring_id)
{
    return t->num_compute_rings > 1 && ring_id == 0;
}

/* Ported from amdgpu_gfx_is_high_priority_graphics_queue: pipe 1 queue 0
 * with more than one gfx pipe */
int gfx11_is_high_priority_graphics_queue(const rdna3_gfx_topo *t, uint32_t ring_id)
{
    const rdna3_gfx_ring_desc *r;
    if (ring_id >= RD_MAX_GFX_RINGS)
        return 0;
    r = &t->gfx[ring_id];
    if (is_graphics_multipipe_capable(t) && t->num_gfx_rings > 1 && r->pipe == 1 && r->queue == 0)
        return (uint32_t)gfx11_me_queue_to_bit(t, (int)r->me, (int)r->pipe, (int)r->queue) == ring_id;
    return 0;
}

/* Ported from amdgpu_gfx_graphics_queue_acquire */
static void graphics_queue_acquire(rdna3_gfx_topo *t)
{
    uint32_t i, queue, pipe, max_queues_per_me = t->me_pipes * t->me_queues;
    if (is_graphics_multipipe_capable(t)) {
        /* policy: amdgpu owns the first queue per pipe at this stage */
        for (i = 0; i < max_queues_per_me; i++) {
            pipe = i % t->me_pipes;
            queue = (i / t->me_pipes) % t->me_queues;
            bm_set(t->me_bitmap, pipe * t->me_queues + queue);
        }
    } else {
        for (i = 0; i < max_queues_per_me; ++i)
            bm_set(t->me_bitmap, i);
    }
    /* update the number of active graphics rings */
    t->num_gfx_rings = bm_weight(t->me_bitmap);
}

/* Ported from amdgpu_gfx_compute_queue_acquire (one XCC) */
static void compute_queue_acquire(rdna3_gfx_topo *t)
{
    uint32_t i, queue, pipe, max_queues_per_mec = t->mec_pipes * t->mec_queues;
    if (max_queues_per_mec > t->num_compute_rings)
        max_queues_per_mec = t->num_compute_rings;
    if (is_compute_multipipe_capable(t)) {
        /* policy: make queues evenly cross all pipes on MEC1 only */
        for (i = 0; i < max_queues_per_mec; i++) {
            pipe = i % t->mec_pipes;
            queue = (i / t->mec_pipes) % t->mec_queues;
            bm_set(t->mec_bitmap, pipe * t->mec_queues + queue);
        }
    } else {
        /* policy: amdgpu owns all queues in the given pipe */
        for (i = 0; i < max_queues_per_mec; ++i)
            bm_set(t->mec_bitmap, i);
    }
}

/* Ported from gfx_v11_0_gfx_ring_init (the ring's identity; the ring
 * itself is allocated by gfx11_hw_init) */
static void gfx_ring_init(rdna3_gfx_topo *t, uint32_t ring_id, uint32_t me, uint32_t pipe,
                          uint32_t queue)
{
    rdna3_gfx_ring_desc *r = &t->gfx[ring_id];
    r->me = me;
    r->pipe = pipe;
    r->queue = queue;
    /* doorbell_index.gfx_ring0 / gfx_ring1 (AMDGPU_NAVI10_DOORBELL_GFX_RING1 = 0x08C) */
    r->doorbell_index = RD_DB_INDEX(ring_id ? RD_DB_GFX_RING0 + 1 : RD_DB_GFX_RING0);
    r->eop_offset = 0;
    ring_name(r->name, "gfx", me, pipe, queue);
    r->irq_type = RD_CP_IRQ_GFX_ME0_PIPE0_EOP + pipe;
    r->hw_prio = gfx11_is_high_priority_graphics_queue(t, ring_id) ? RD_GFX_PIPE_PRIO_HIGH
                                                                   : RD_GFX_PIPE_PRIO_NORMAL;
}

/* Ported from gfx_v11_0_compute_ring_init (mec0 is me1) */
static void compute_ring_init(rdna3_gfx_topo *t, uint32_t ring_id, uint32_t mec, uint32_t pipe,
                              uint32_t queue)
{
    rdna3_gfx_ring_desc *r = &t->compute[ring_id];
    r->me = mec + 1;
    r->pipe = pipe;
    r->queue = queue;
    r->doorbell_index = RD_DB_INDEX(RD_DB_MEC_RING0 + ring_id);
    r->eop_offset = ring_id * RD_GFX11_MEC_HPD_SIZE;
    ring_name(r->name, "comp", r->me, pipe, queue);
    r->irq_type = RD_CP_IRQ_COMPUTE_MEC1_PIPE0_EOP + (r->me - 1) * t->mec_pipes + pipe;
    r->hw_prio = gfx11_is_high_priority_compute_queue(t, ring_id) ? RD_GFX_PIPE_PRIO_HIGH
                                                                  : RD_GFX_PIPE_PRIO_NORMAL;
}

/* Ported from gfx_v11_0_gpu_early_init (adev->gfx.config) */
static int gpu_early_init(rdna3_gfx_topo *t)
{
    if (t->gc_major != 11)
        return RDNA3_EINVAL;
    if (t->gc_minor == 0 && (t->gc_rev == 0 || t->gc_rev == 2 || t->gc_rev == 3)) {
        /* 11.0.3 also sets adev->gfx.ras = &gfx_v11_0_3_ras */
        t->max_hw_contexts = 8;
        t->sc_prim_fifo_size_frontend = 0x20;
        t->sc_prim_fifo_size_backend = 0x100;
        t->sc_hiz_tile_fifo_size = 0;
        t->sc_earlyz_tile_fifo_size = 0x4C0;
        return RDNA3_OK;
    }
    if ((t->gc_minor == 0 && (t->gc_rev == 1 || t->gc_rev == 4)) ||
        (t->gc_minor == 5 && t->gc_rev <= 2)) {
        t->max_hw_contexts = 8;
        t->sc_prim_fifo_size_frontend = 0x20;
        t->sc_prim_fifo_size_backend = 0x100;
        t->sc_hiz_tile_fifo_size = 0x80;
        t->sc_earlyz_tile_fifo_size = 0x300;
        return RDNA3_OK;
    }
    return RDNA3_EINVAL;   /* BUG() */
}

int gfx11_sw_init_topo(rdna3_gfx_topo *t, uint32_t gc_major, uint32_t gc_minor, uint32_t gc_rev,
                       int num_kcq, int async_gfx_ring, int compute_multipipe)
{
    uint32_t i, j, k, ring_id = 0, n;
    uint8_t *p = (uint8_t *)t;

    for (n = 0; n < sizeof(*t); n++)
        p[n] = 0;
    t->gc_major = gc_major;
    t->gc_minor = gc_minor;
    t->gc_rev = gc_rev;
    t->async_gfx_ring = async_gfx_ring;
    t->compute_multipipe = compute_multipipe;

    /* gfx_v11_0_early_init */
    t->num_gfx_rings = RD_GFX11_NUM_GFX_RINGS;
    n = (uint32_t)gfx11_get_num_kcq(num_kcq);
    t->num_compute_rings = n < RD_MAX_COMPUTE_RINGS ? n : RD_MAX_COMPUTE_RINGS;

    /* gfx_v11_0_sw_init */
    t->num_me = 1;
    t->me_pipes = 1;
    t->me_queues = 1;
    t->mec_pipes = 4;
    if (gc_major == 11 && gc_minor == 0 && (gc_rev == 0 || gc_rev == 2 || gc_rev == 3)) {
        t->num_mec = 2;
        t->mec_queues = 4;
    } else if (gc_major == 11 && ((gc_minor == 0 && (gc_rev == 1 || gc_rev == 4)) ||
                                  (gc_minor == 5 && gc_rev <= 2))) {
        t->num_mec = 1;
        t->mec_queues = 4;
    } else {
        t->num_mec = 1;
        t->mec_queues = 8;
    }

    /* gfx_v11_0_me_init: bitmap_zero + amdgpu_gfx_graphics_queue_acquire */
    graphics_queue_acquire(t);
    /* gfx_v11_0_mec_init: bitmap_zero + amdgpu_gfx_compute_queue_acquire;
     * the HPD EOP buffer holds one GFX11_MEC_HPD_SIZE slot per compute ring */
    compute_queue_acquire(t);
    t->mec_hpd_size = t->num_compute_rings * RD_GFX11_MEC_HPD_SIZE;

    /* set up the gfx ring */
    for (i = 0; i < t->num_me; i++)
        for (j = 0; j < t->me_queues; j++)
            for (k = 0; k < t->me_pipes; k++) {
                if (!gfx11_is_me_queue_enabled(t, (int)i, (int)k, (int)j))
                    continue;
                if (ring_id >= RD_MAX_GFX_RINGS)
                    return RDNA3_EINVAL;
                gfx_ring_init(t, ring_id, i, k, j);
                ring_id++;
            }

    ring_id = 0;
    /* set up the compute queues - allocate horizontally across pipes */
    for (i = 0; i < t->num_mec; ++i)
        for (j = 0; j < t->mec_queues; j++)
            for (k = 0; k < t->mec_pipes; k++) {
                if (!gfx11_is_mec_queue_enabled(t, (int)i, (int)k, (int)j))
                    continue;
                if (ring_id >= RD_MAX_COMPUTE_RINGS)
                    return RDNA3_EINVAL;
                compute_ring_init(t, ring_id, i, k, j);
                ring_id++;
            }

    return gpu_early_init(t);
}

/* Ported from gfx_v11_0_select_me_pipe_q */
void gfx11_select_me_pipe_q(rdna3_dev *d, uint32_t me, uint32_t pipe, uint32_t q, uint32_t vm)
{
    grbm_select(d, me, pipe, q, vm);
}

/* --------------------- rdna3amdgpu=1 ring helpers --------------------- */

/* Ported from gfx_v11_ring_insert_nop */
void gfx11_ring_insert_nop(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t num_nop)
{
    uint32_t i;
    /* Header itself is a NOP packet */
    if (num_nop == 1) {
        gfx11_write(d, r, GFX11_RING_NOP);
        return;
    }
    /* Max HW optimization till 0x3ffe, followed by remaining one NOP at a time */
    gfx11_write(d, r, PACKET3(PACKET3_NOP, num_nop - 2 < 0x3ffe ? num_nop - 2 : 0x3ffe));
    /* Header is at index 0, followed by num_nops - 1 NOP packet's */
    for (i = 1; i < num_nop; i++)
        gfx11_write(d, r, GFX11_RING_NOP);
}

/* Ported from gfx_v11_0_ring_emit_pipeline_sync */
void gfx11_emit_pipeline_sync(rdna3_dev *d, rdna3_gfx_ring *r)
{
    int usepfp = r->kind == RD_RING_GFX;
    uint32_t seq = r->fence_seq;   /* ring->fence_drv.sync_seq */
    uint64_t addr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_FENCE);
    gfx11_emit_wait_reg_mem(d, r, usepfp, 1, 0, (uint32_t)addr, (uint32_t)(addr >> 32), seq,
                            0xffffffff, 4);
}

/* Ported from gfx_v11_0_ring_emit_fence_kiq (AMDGPU_FENCE_FLAG_64BIT is a BUG) */
void gfx11_emit_fence_kiq(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t addr, uint32_t seq, int irq)
{
    /* write fence seq to the "addr" */
    gfx11_write(d, r, PACKET3(PACKET3_WRITE_DATA, 3));
    gfx11_write(d, r, WRITE_DATA_ENGINE_SEL(0) | WRITE_DATA_DST_SEL(5) | WR_CONFIRM);
    gfx11_write(d, r, (uint32_t)addr);
    gfx11_write(d, r, (uint32_t)(addr >> 32));
    gfx11_write(d, r, seq);
    if (irq) {
        /* set register to trigger INT */
        gfx11_write(d, r, PACKET3(PACKET3_WRITE_DATA, 3));
        gfx11_write(d, r, WRITE_DATA_ENGINE_SEL(0) | WRITE_DATA_DST_SEL(0) | WR_CONFIRM);
        gfx11_write(d, r, OFF(d, regCPC_INT_STATUS));
        gfx11_write(d, r, 0);
        gfx11_write(d, r, 0x20000000);   /* src_id is 178 */
    }
}

/* Ported from gfx_v11_0_ring_emit_reg_write_reg_wait */
void gfx11_emit_reg_write_reg_wait(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t reg0, uint32_t reg1,
                                   uint32_t ref, uint32_t mask)
{
    int usepfp = r->kind == RD_RING_GFX;
    gfx11_emit_wait_reg_mem(d, r, usepfp, 0, 1, reg0, reg1, ref, mask, 0x20);
}

/* Ported from gfx_v11_0_ring_get_wptr_gfx / gfx_v11_0_ring_get_wptr_compute
 * (mes_v11_0_ring_get_wptr is the compute one) */
uint64_t gfx11_ring_get_wptr(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint64_t wptr = 0;
    if (r->use_doorbell) {
        rdna3_vram_read(d, r->wb + RD_GFX_WB_WPTR, &wptr, 8);
    } else if (r->kind == RD_RING_GFX) {
        wptr = RG(d, regCP_RB0_WPTR);
        wptr += (uint64_t)RG(d, regCP_RB0_WPTR_HI) << 32;
    } else {
        RERR(d, "GFX: a compute ring without a doorbell has no wptr");   /* BUG() */
    }
    return wptr;
}

/* Ported from gfx_v11_0_wait_for_idle */
int gfx11_wait_for_idle(rdna3_dev *d)
{
    uint32_t i;
    for (i = 0; i < d->usec_timeout; i++) {
        /* read MC_STATUS */
        if (!(RG(d, regGRBM_STATUS) & GRBM_STATUS__GUI_ACTIVE_MASK))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    return RDNA3_ETIME;
}

/* Ported from gfx_v11_0_request_gfx_index_mutex */
int gfx11_request_gfx_index_mutex(rdna3_dev *d, int req)
{
    uint32_t i, tmp, val;
    for (i = 0; i < d->usec_timeout; i++) {
        /* Request with MeId=2, PipeId=0 */
        tmp = REG_SET_FIELD(0u, CP_GFX_INDEX_MUTEX, REQUEST, req ? 1 : 0);
        tmp = REG_SET_FIELD(tmp, CP_GFX_INDEX_MUTEX, CLIENTID, 4);
        WG(d, regCP_GFX_INDEX_MUTEX, tmp);
        val = RG(d, regCP_GFX_INDEX_MUTEX);
        if (req) {
            if (val == tmp)
                break;
        } else {
            tmp = REG_SET_FIELD(tmp, CP_GFX_INDEX_MUTEX, REQUEST, 1);
            /* unlocked or locked by firmware */
            if (val != tmp)
                break;
        }
        d->os.udelay(d->os.ctx, 1);
    }
    if (i >= d->usec_timeout)
        return RDNA3_EINVAL;
    return RDNA3_OK;
}

/* Ported from gfx_v11_0_update_spm_vmid (bare metal: no ring write) */
void gfx11_update_spm_vmid(rdna3_dev *d, uint32_t vmid)
{
    uint32_t pre_data = RG(d, regRLC_SPM_MC_CNTL), data;
    data = pre_data & ~(uint32_t)RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK;
    data |= (vmid & RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK) << RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT;
    if (pre_data != data)
        WG(d, regRLC_SPM_MC_CNTL, data);
}

/* gfx_v11_0_get_cpg_int_cntl (me 0: pipes 0 and 1) */
static uint32_t get_cpg_int_cntl(rdna3_dev *d, uint32_t me, uint32_t pipe)
{
    if (me != 0)
        return 0;
    switch (pipe) {
    case 0: return OFF(d, regCP_INT_CNTL_RING0);
    case 1: return OFF(d, regCP_INT_CNTL_RING1);
    default: return 0;
    }
}

/* gfx_v11_0_get_cpc_int_cntl: amdgpu controls only the first MEC */
static uint32_t get_cpc_int_cntl(rdna3_dev *d, uint32_t me, uint32_t pipe)
{
    if (me != 1)
        return 0;
    switch (pipe) {
    case 0: return OFF(d, regCP_ME1_PIPE0_INT_CNTL);
    case 1: return OFF(d, regCP_ME1_PIPE1_INT_CNTL);
    case 2: return OFF(d, regCP_ME1_PIPE2_INT_CNTL);
    case 3: return OFF(d, regCP_ME1_PIPE3_INT_CNTL);
    default: return 0;
    }
}

/* the ME / MEC loops of the three fault-state functions over GC 11.0.3's
 * gfx.me (1 ME, 1 pipe) and gfx.mec (2 MECs, 4 pipes); a zero mask skips
 * that side */
static void fault_state(rdna3_dev *d, uint32_t cpg_mask, uint32_t cpc_mask, int enable)
{
    uint32_t i, j, reg, v;
    for (i = 0; i < 1; i++)
        for (j = 0; j < 1; j++) {
            reg = get_cpg_int_cntl(d, i, j);
            if (reg && cpg_mask) {
                v = rdna3_rreg(d, reg);
                v = enable ? (v | cpg_mask) : (v & ~cpg_mask);
                rdna3_wreg(d, reg, v);
            }
        }
    for (i = 0; i < 2; i++)
        for (j = 0; j < 4; j++) {
            /* MECs start at 1 */
            reg = get_cpc_int_cntl(d, i + 1, j);
            if (reg && cpc_mask) {
                v = rdna3_rreg(d, reg);
                v = enable ? (v | cpc_mask) : (v & ~cpc_mask);
                rdna3_wreg(d, reg, v);
            }
        }
}

/* Ported from gfx_v11_0_set_priv_reg_fault_state */
void gfx11_set_priv_reg_fault_state(rdna3_dev *d, int enable)
{
    fault_state(d, CP_INT_CNTL_RING0__PRIV_REG_INT_ENABLE_MASK,
                CP_ME1_PIPE0_INT_CNTL__PRIV_REG_INT_ENABLE_MASK, enable);
}

/* Ported from gfx_v11_0_set_priv_inst_fault_state (CPG only) */
void gfx11_set_priv_inst_fault_state(rdna3_dev *d, int enable)
{
    fault_state(d, CP_INT_CNTL_RING0__PRIV_INSTR_INT_ENABLE_MASK, 0, enable);
}

/* Ported from gfx_v11_0_set_bad_op_fault_state */
void gfx11_set_bad_op_fault_state(rdna3_dev *d, int enable)
{
    fault_state(d, CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK,
                CP_ME1_PIPE0_INT_CNTL__OPCODE_ERROR_INT_ENABLE_MASK, enable);
}

/* Ported from gfx_v11_0_late_init: amdgpu_irq_get of priv_reg_irq,
 * priv_inst_irq and bad_op_irq (the first reference enables each) */
void gfx11_late_init(rdna3_dev *d)
{
    gfx11_set_priv_reg_fault_state(d, 1);
    gfx11_set_priv_inst_fault_state(d, 1);
    gfx11_set_bad_op_fault_state(d, 1);
}

/* Ported from gfx_v11_0_priv_reg_irq / gfx_v11_0_priv_inst_irq /
 * gfx_v11_0_bad_op_irq -> gfx_v11_0_handle_priv_fault */
int gfx11_handle_priv_fault(const rdna3_iv *iv, const rdna3_gfx_ring *gfx,
                            const rdna3_mec_ring *kcq, int *ring)
{
    uint32_t me_id, pipe_id, queue_id;
    if (iv->client_id != RD_IH_CLIENT_GRBM_CP ||
        (iv->src_id != RD_IH_SRC_CP_PRIV_REG && iv->src_id != RD_IH_SRC_CP_PRIV_INSTR &&
         iv->src_id != RD_IH_SRC_CP_BAD_OPCODE))
        return 0;
    me_id = (iv->ring_id & 0x0c) >> 2;
    pipe_id = (iv->ring_id & 0x03) >> 0;
    queue_id = (iv->ring_id & 0x70) >> 4;
    *ring = -1;
    switch (me_id) {
    case 0:
        /* the kernel gfx ring is gfx_0.0.0 */
        if (gfx && gfx->ready && pipe_id == 0 && queue_id == 0)
            *ring = 0;
        break;
    case 1:
    case 2:
        if (kcq && kcq->rb.ready && kcq->me == me_id && kcq->pipe == pipe_id &&
            kcq->queue == queue_id)
            *ring = 1;
        break;
    default:   /* BUG() */
        break;
    }
    return 1;
}

/* Ported from gfx_v11_0_ring_test_ib (the writeback slot is in the ring's
 * writeback page, the IB the caller's VRAM buffer, run in VMID 0) */
int gfx11_ring_test_ib(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ib)
{
    uint64_t gpu_addr;
    uint32_t v = 0xCAFEDEAD, pk[5], seq;
    rdna3_gfx_ib one;
    int ret;

    if (!r->ready || ib == UINT64_MAX)
        return RDNA3_EINVAL;
    gpu_addr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_IB_TEST);
    rdna3_vram_write(d, r->wb + RD_GFX_WB_IB_TEST, &v, 4);
    pk[0] = PACKET3(PACKET3_WRITE_DATA, 3);
    pk[1] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
    pk[2] = (uint32_t)gpu_addr;
    pk[3] = (uint32_t)(gpu_addr >> 32);
    pk[4] = 0xDEADBEEF;
    rdna3_vram_write(d, ib, pk, sizeof(pk));
    rdna3_hdp_flush(d);
    one.va = rdna3_vram_mc(d, ib);
    one.ndw = 5;
    one.flags = 0;
    /* amdgpu_ib_schedule on the ring, then dma_fence_wait_timeout */
    seq = r->kind == RD_RING_GFX ? gfx11_submit_job(d, r, 0, 0, 0, &one, 1)
                                 : gfx11_submit_compute_job(d, (rdna3_mec_ring *)r, 0, 0, 0, &one, 1);
    if (!seq)
        return RDNA3_EINVAL;
    ret = gfx11_wait_fence(d, r, seq);
    if (ret)
        return ret;
    rdna3_vram_read(d, r->wb + RD_GFX_WB_IB_TEST, &v, 4);
    if (v == 0xDEADBEEF)
        return RDNA3_OK;
    RERR(d, "GFX: IB test: 0x%08x instead of 0xDEADBEEF", v);
    return RDNA3_EINVAL;
}

/* amdgpu_fence_wait_polling on a KIQ fence for 'usec' */
static int kiq_fence_wait(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq, uint32_t usec)
{
    uint32_t i;
    for (i = 0; i < usec; i++) {
        if ((int32_t)(wb_read32(d, r, RD_GFX_WB_FENCE) - seq) >= 0)
            return 1;
        d->os.udelay(d->os.ctx, 1);
    }
    return 0;
}

/* Ported from amdgpu_gmc_fw_reg_write_reg_wait (KIQ path) */
int gfx11_kiq_reg_write_reg_wait(rdna3_dev *d, rdna3_mec_ring *kiq, uint32_t reg0, uint32_t reg1,
                                 uint32_t ref, uint32_t mask)
{
    rdna3_gfx_ring *r = &kiq->rb;
    uint32_t seq, cnt = 0;
    int ok;

    if (!r->ready)
        return RDNA3_ENODEV;
    if (ring_wait_space(d, r, 32))   /* amdgpu_ring_alloc(ring, 32) */
        goto failed_kiq;
    gfx11_emit_reg_write_reg_wait(d, r, reg0, reg1, ref, mask);
    /* amdgpu_fence_emit_polling(ring, &seq, MAX_KIQ_REG_WAIT) */
    seq = ++r->fence_seq;
    gfx11_emit_fence_kiq(d, r, rdna3_vram_mc(d, r->wb + RD_GFX_WB_FENCE), seq, 0);
    gfx11_commit(d, r);
    ok = kiq_fence_wait(d, r, seq, 5000);
    while (!ok && cnt++ < 1000) {
        d->os.msleep(d->os.ctx, 5);   /* MAX_KIQ_REG_BAILOUT_INTERVAL */
        ok = kiq_fence_wait(d, r, seq, 5000);
    }
    if (ok)
        return RDNA3_OK;
failed_kiq:
    RERR(d, "GMC: failed to write reg %x wait reg %x", reg0, reg1);
    return RDNA3_ETIME;
}

/* ---------------------------- MES API ---------------------------- */

static void mes_frame(uint32_t *p, uint32_t op)
{
    uint32_t i;
    for (i = 0; i < RD_MES_API_DWORDS; i++)
        p[i] = 0;
    p[0] = RD_MES_API_HEADER(op);
}

/* Ported from mes_v11_0_misc_op (union MESAPI__MISC: opcode at 1,
 * api_status at 2, the operation's data from 6) */
int gfx11_mes_misc_op(rdna3_dev *d, rdna3_mes *m, const rdna3_mes_misc *in)
{
    uint32_t p[RD_MES_API_DWORDS];
    if (!m->ready)
        return RDNA3_EINVAL;
    mes_frame(p, RD_MES_SCH_API_MISC);
    switch (in->op) {
    case RD_MES_MISC_OP_READ_REG:
        p[1] = 3;                      /* MESAPI_MISC__READ_REG */
        p[6] = in->reg0;
        put64(p + 7, in->buffer_addr);
        break;
    case RD_MES_MISC_OP_WRITE_REG:
        p[1] = 0;                      /* MESAPI_MISC__WRITE_REG */
        p[6] = in->reg0;
        p[7] = in->value;
        break;
    case RD_MES_MISC_OP_WRM_REG_WAIT:
        p[1] = 4;                      /* MESAPI_MISC__WAIT_REG_MEM */
        p[6] = 0;                      /* WRM_OPERATION__WAIT_REG_MEM */
        p[7] = in->ref;
        p[8] = in->mask;
        p[9] = in->reg0;
        p[10] = 0;
        break;
    case RD_MES_MISC_OP_WRM_REG_WR_WAIT:
        p[1] = 4;
        p[6] = 1;                      /* WRM_OPERATION__WR_WAIT_WR_REG */
        p[7] = in->ref;
        p[8] = in->mask;
        p[9] = in->reg0;
        p[10] = in->reg1;
        break;
    default:
        RERR(d, "MES: unsupported misc op (%u)", in->op);
        return RDNA3_EINVAL;
    }
    return gfx11_mes_submit(d, m, p, 2);
}

/* Ported from amdgpu_mes_reg_write_reg_wait */
int gfx11_mes_reg_write_reg_wait(rdna3_dev *d, rdna3_mes *m, uint32_t reg0, uint32_t reg1,
                                 uint32_t ref, uint32_t mask)
{
    rdna3_mes_misc in = { RD_MES_MISC_OP_WRM_REG_WR_WAIT, reg0, reg1, 0, ref, mask, 0 };
    int r = gfx11_mes_misc_op(d, m, &in);
    if (r)
        RERR(d, "MES: failed to reg_write_reg_wait");
    return r;
}

/* Ported from amdgpu_mes_rreg (the value lands in the MES writeback page) */
int gfx11_mes_rreg(rdna3_dev *d, rdna3_mes *m, uint32_t reg, uint32_t *val)
{
    rdna3_mes_misc in = { RD_MES_MISC_OP_READ_REG, reg, 0, 0, 0, 0, 0 };
    int r;
    *val = 0;
    in.buffer_addr = rdna3_vram_mc(d, m->wb + RD_MES_WB_READ_VAL);
    r = gfx11_mes_misc_op(d, m, &in);
    if (r)
        RERR(d, "MES: failed to read reg (0x%x)", reg);
    else
        rdna3_vram_read(d, m->wb + RD_MES_WB_READ_VAL, val, 4);
    return r;
}

/* Ported from amdgpu_mes_wreg */
int gfx11_mes_wreg(rdna3_dev *d, rdna3_mes *m, uint32_t reg, uint32_t val)
{
    rdna3_mes_misc in = { RD_MES_MISC_OP_WRITE_REG, reg, 0, val, 0, 0, 0 };
    int r = gfx11_mes_misc_op(d, m, &in);
    if (r)
        RERR(d, "MES: failed to write reg (0x%x)", reg);
    return r;
}

/* Ported from mes_v11_0_map_legacy_queue (ADD_QUEUE with map_legacy_kq;
 * pipe_id at 48, queue_id at 49) */
int gfx11_mes_map_legacy_queue(rdna3_dev *d, rdna3_mes *m, const rdna3_mes_legacy_queue *q)
{
    uint32_t p[RD_MES_API_DWORDS];
    int type = mes_queue_type(q->queue_type);
    if (!m->ready || type < 0)
        return RDNA3_EINVAL;
    mes_frame(p, RD_MES_SCH_API_ADD_QUEUE);
    p[48] = q->pipe;
    p[49] = q->queue;
    p[18] = q->doorbell_offset;
    put64(p + 19, q->mqd_addr);
    put64(p + 21, q->wptr_addr);
    p[27] = (uint32_t)type;
    p[36] = 1u << 13;                  /* map_legacy_kq */
    if (gfx11_mes_submit(d, m, p, RD_MES_ADD_QUEUE_STATUS_DW)) {
        RERR(d, "MES: failed to map legacy queue");
        return RDNA3_ETIME;
    }
    return RDNA3_OK;
}

/* Ported from mes_v11_0_unmap_legacy_queue (REMOVE_QUEUE: flags at 4,
 * pipe 9, queue 10, tf_addr 11, tf_data 13, queue_type 14) */
int gfx11_mes_unmap_legacy_queue(rdna3_dev *d, rdna3_mes *m, const rdna3_mes_legacy_queue *q,
                                 uint32_t action, uint64_t tf_addr, uint64_t tf_data)
{
    uint32_t p[RD_MES_API_DWORDS];
    int type = mes_queue_type(q->queue_type);
    if (!m->ready || type < 0)
        return RDNA3_EINVAL;
    mes_frame(p, RD_MES_SCH_API_REMOVE_QUEUE);
    p[1] = q->doorbell_offset;
    put64(p + 2, 0);                   /* gang_context_addr */
    p[9] = q->pipe;
    p[10] = q->queue;
    if (action == RD_PREEMPT_QUEUES_NO_UNMAP) {
        p[4] = 1u << 2;                /* preempt_legacy_gfx_queue */
        put64(p + 11, tf_addr);
        p[13] = (uint32_t)tf_data;
    } else {
        p[4] = 1u << 3;                /* unmap_legacy_queue */
        p[14] = (uint32_t)type;
    }
    if (gfx11_mes_submit(d, m, p, RD_MES_REMOVE_QUEUE_STATUS_DW)) {
        RERR(d, "MES: failed to unmap legacy queue");
        return RDNA3_ETIME;
    }
    return RDNA3_OK;
}

/* Ported from mes_v11_0_reset_legacy_queue, MES path (union MESAPI__RESET:
 * flags at 1, doorbell 4, queue_type 7, the LP gfx queue from 8, api_status
 * at 24) */
int gfx11_mes_reset_legacy_queue(rdna3_dev *d, rdna3_mes *m, const rdna3_mes_legacy_queue *q)
{
    uint32_t p[RD_MES_API_DWORDS];
    int type = mes_queue_type(q->queue_type);
    if (!m->ready || type < 0)
        return RDNA3_EINVAL;
    mes_frame(p, RD_MES_SCH_API_RESET);
    p[7] = (uint32_t)type;
    if (type == 0) {                   /* MES_QUEUE_TYPE_GFX */
        p[1] = 1u << 3;                /* reset_legacy_gfx */
        p[8] = q->pipe;
        p[9] = q->queue;
        put64(p + 11, q->mqd_addr);
        p[13] = q->doorbell_offset;
        put64(p + 14, q->wptr_addr);
        p[10] = q->vmid;
    } else {
        p[1] = 1u << 0;                /* reset_queue_only */
        p[4] = q->doorbell_offset;
    }
    if (gfx11_mes_submit(d, m, p, 24)) {
        RERR(d, "MES: failed to reset legacy queue");
        return RDNA3_ETIME;
    }
    return RDNA3_OK;
}

/* Ported from mes_v11_0_suspend_gang (union MESAPI__SUSPEND) */
int gfx11_mes_suspend_gang(rdna3_dev *d, rdna3_mes *m, int all, uint64_t gang_ctx,
                           uint64_t fence_addr, uint32_t fence_value)
{
    uint32_t p[RD_MES_API_DWORDS];
    if (!m->ready)
        return RDNA3_EINVAL;
    mes_frame(p, RD_MES_SCH_API_SUSPEND);
    p[1] = all ? 1u : 0;
    put64(p + 2, gang_ctx);
    put64(p + 4, fence_addr);
    p[6] = fence_value;
    return gfx11_mes_submit(d, m, p, 7);
}

/* Ported from mes_v11_0_resume_gang (union MESAPI__RESUME) */
int gfx11_mes_resume_gang(rdna3_dev *d, rdna3_mes *m, int all, uint64_t gang_ctx)
{
    uint32_t p[RD_MES_API_DWORDS];
    if (!m->ready)
        return RDNA3_EINVAL;
    mes_frame(p, RD_MES_SCH_API_RESUME);
    p[1] = all ? 1u : 0;
    put64(p + 2, gang_ctx);
    return gfx11_mes_submit(d, m, p, 4);
}

/* amdgpu_mes_suspend_resume_all_supported for GC 11 */
static int mes_suspend_resume_all_supported(rdna3_dev *d, const rdna3_mes *m)
{
    return d->ipver[HWIP_GC].major == 11 && (m->sched_version & RD_MES_VERSION_MASK) >= 0x63;
}

/* Ported from amdgpu_mes_suspend (mes_v11_0_suspend; mes_v11_0_hw_fini only
 * frees the SR-IOV MES info buffer) */
int gfx11_mes_suspend(rdna3_dev *d, rdna3_mes *m)
{
    int r;
    if (!mes_suspend_resume_all_supported(d, m))
        return RDNA3_OK;
    r = gfx11_mes_suspend_gang(d, m, 1, 0, 0, 0);
    if (r)
        RERR(d, "MES: failed to suspend all gangs");
    return r;
}

/* Ported from amdgpu_mes_resume (mes_v11_0_resume, after its hw_init) */
int gfx11_mes_resume(rdna3_dev *d, rdna3_mes *m)
{
    int r;
    if (!mes_suspend_resume_all_supported(d, m))
        return RDNA3_OK;
    r = gfx11_mes_resume_gang(d, m, 1, 0);
    if (r)
        RERR(d, "MES: failed to resume all gangs");
    return r;
}

/* Ported from mes_v11_0_ring_get_rptr */
uint64_t gfx11_ring_get_rptr(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint64_t v = 0;
    rdna3_vram_read(d, r->wb + RD_GFX_WB_RPTR, &v, 8);
    return v;
}

/* amdgpu_ring_clear_ring: the ring filled with ring->funcs->nop */
static void ring_clear(rdna3_dev *d, rdna3_gfx_ring *r)
{
    uint32_t i, nop = GFX11_RING_NOP;
    for (i = 0; i <= r->buf_mask; i++)
        rdna3_vram_write(d, r->ring + 4ull * i, &nop, 4);
}

/* gfx_v11_0_kcq_init_queue / gfx_v11_0_kgq_init_queue with reset: the MQD
 * from its backup (left as it is without one), wptr 0, the ring cleared */
static void queue_init_reset(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t mqd, uint64_t backup)
{
    uint64_t zero = 0;
    if (backup) {
        rdna3_vram_read(d, backup, g_mqd, sizeof(g_mqd));
        rdna3_vram_write(d, mqd, g_mqd, sizeof(g_mqd));
    }
    r->wptr = 0;
    rdna3_vram_write(d, r->wb + RD_GFX_WB_WPTR, &zero, 8);
    ring_clear(d, r);
    rdna3_hdp_flush(d);
}

/* Ported from gfx_v11_0_reset_kcq */
int gfx11_reset_kcq(rdna3_dev *d, rdna3_mec_ring *kcq, rdna3_mes *m, uint32_t vmid)
{
    rdna3_mes_legacy_queue q;
    uint32_t i;
    int r = RDNA3_OK;
    (void)vmid;
    if (!m || !m->ready || kcq->rb.kind != RD_RING_COMPUTE)
        return RDNA3_ENODEV;
    gfx11_rlc_safe_mode(d, 1);
    grbm_select(d, kcq->me, kcq->pipe, kcq->queue, 0);
    WG(d, regCP_HQD_DEQUEUE_REQUEST, 0x2);
    WG(d, regSPI_COMPUTE_QUEUE_RESET, 0x1);
    /* make sure dequeue is complete */
    for (i = 0; i < d->usec_timeout; i++) {
        if (!(RG(d, regCP_HQD_ACTIVE) & 1))
            break;
        d->os.udelay(d->os.ctx, 1);
    }
    if (i >= d->usec_timeout)
        r = RDNA3_ETIME;
    grbm_select(d, 0, 0, 0, 0);
    gfx11_rlc_safe_mode(d, 0);
    if (r) {
        RERR(d, "GFX: fail to wait on hqd deactivate");
        return r;
    }
    kcq->rb.ready = 0;
    queue_init_reset(d, &kcq->rb, kcq->mqd, kcq->mqd_backup);
    q.queue_type = 1;
    q.doorbell_offset = kcq->rb.doorbell;
    q.me = kcq->me;
    q.pipe = kcq->pipe;
    q.queue = kcq->queue;
    q.vmid = 0;
    q.mqd_addr = rdna3_vram_mc(d, kcq->mqd);
    q.wptr_addr = rdna3_vram_mc(d, kcq->rb.wb + RD_GFX_WB_WPTR);
    r = gfx11_mes_map_legacy_queue(d, m, &q);
    if (r) {
        RERR(d, "GFX: failed to remap kcq");
        return r;
    }
    kcq->mapped = 1;
    r = gfx11_ring_test(d, &kcq->rb);   /* amdgpu_ring_test_ring */
    if (!r)
        kcq->rb.ready = 1;
    return r;
}

/* Ported from gfx_v11_0_reset_kgq */
int gfx11_reset_kgq(rdna3_dev *d, rdna3_gfx_ring *r, rdna3_mes *m, uint32_t vmid)
{
    rdna3_mes_legacy_queue q;
    int ret;
    if (!m || !m->ready)
        return RDNA3_ENODEV;
    if (!r->async || !r->mqd || r->kind != RD_RING_GFX)
        return RDNA3_EINVAL;
    q.queue_type = 0;
    q.doorbell_offset = r->doorbell;
    q.me = 0;
    q.pipe = 0;
    q.queue = 0;
    q.vmid = vmid;
    q.mqd_addr = rdna3_vram_mc(d, r->mqd);
    q.wptr_addr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_WPTR);
    ret = gfx11_mes_reset_legacy_queue(d, m, &q);
    if (ret)
        return ret;
    r->ready = 0;
    queue_init_reset(d, r, r->mqd, r->mqd_backup);
    shadow_state_reset(r);
    ret = gfx11_mes_map_legacy_queue(d, m, &q);
    if (ret) {
        RERR(d, "GFX: failed to remap kgq");
        return ret;
    }
    ret = gfx11_ring_test(d, r);   /* amdgpu_ring_test_ring */
    if (!ret) {
        r->ready = 1;
        r->last_ctx = 0;
    }
    return ret;
}

/* rdna3amdgpu=1: amdgpu_ib_schedule's order for the gfx ring and the KCQ:
 * the memory sync only for an IB with AMDGPU_IB_FLAG_EMIT_MEM_SYNC, then
 * amdgpu_vm_flush (COND_EXEC on the gfx ring; pipeline sync, VM flush,
 * PASID mapping, GDS switch, a fence after a flush or mapping), the job's
 * COND_EXEC (gfx), HDP flush, CONTEXT_CONTROL and frames with the TMZ state
 * following each IB (gfx), the IBs, the user fence, the gfx shadow postfix
 * and the second COND_EXEC (gfx), the fence */
static uint32_t submit_amdgpu(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ctx, uint32_t vmid,
                              uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n,
                              const rdna3_gfx_job_ext *x)
{
    int gfx = r->kind == RD_RING_GFX;
    int gds = x && x->gds_switch, pasid = x && x->pasid_mapping;
    /* amdgpu_vm_need_pipeline_sync */
    int need_pipe = vmid && (pd_addr || gds);
    uint64_t cond_addr = rdna3_vram_mc(d, r->wb + RD_GFX_WB_COND_EXE);
    uint32_t i, seq, patch = 0, cond_exec = 0;
    /* the ring's emit_frame_size, roughly: vm flush part 5+7+5+19+5+20+8+5,
     * mem sync 8, COND_EXEC 5 twice, shadow 2 x 22, hdp 7, cntxcntl 3,
     * frames 2 per IB + 2, user fence 8, fence 8, 4 per IB */
    uint32_t ndw = 74 + 8 + 10 + 44 + 7 + 3 + 2 + 8 + 8 + 6 * n;
    int ctx_switch = ctx != r->last_ctx, secure = 0;

    if (ring_wait_space(d, r, ndw))
        return 0;
    if (ibs[0].flags & RD_GFX_IB_FLAG_EMIT_MEM_SYNC)
        gfx11_emit_mem_sync(d, r);

    /* amdgpu_vm_flush */
    if (pd_addr || gds || need_pipe) {
        if (gfx)
            patch = gfx11_emit_init_cond_exec(d, r, cond_addr);
        if (need_pipe)
            gfx11_emit_pipeline_sync(d, r);
        if (pd_addr)
            gfx11_emit_vm_flush(d, r, vmid, pd_addr);
        if (pasid)   /* amdgpu_gmc_emit_pasid_mapping -> gmc_v11_0_emit_pasid_mapping */
            gfx11_emit_wreg(d, r, gmc11_pasid_mapping_reg(d, vmid, 0), x->pasid);
        if (gds)
            gfx11_emit_gds_switch(d, r, vmid, x->gds_base, x->gds_size, x->gws_base, x->gws_size,
                                  x->oa_base, x->oa_size);
        if (pd_addr || pasid)
            (void)gfx11_emit_fence(d, r);   /* the flush's own fence (amdgpu_fence_emit) */
        if (gfx)
            gfx11_ring_patch_cond_exec(d, r, patch);
    }

    if (gfx) {
        gfx11_emit_gfx_shadow(d, r, x ? x->shadow_va : 0, x ? x->csa_va : 0, x ? x->gds_va : 0,
                              x ? x->init_shadow : 0, vmid);
        cond_exec = gfx11_emit_init_cond_exec(d, r, cond_addr);
    }
    gfx11_emit_hdp_flush(d, r);   /* amdgpu_device_flush_hdp */
    if (gfx) {
        gfx11_emit_cntxcntl(d, r, ctx_switch);
        secure = (ibs[0].flags & RD_GFX_IB_FLAG_TMZ) != 0;
        gfx11_emit_frame_cntl(d, r, 1, secure);
    }
    for (i = 0; i < n; i++) {
        if (gfx) {
            if (secure != ((ibs[i].flags & RD_GFX_IB_FLAG_TMZ) != 0)) {
                gfx11_emit_frame_cntl(d, r, 0, secure);
                secure = !secure;
                gfx11_emit_frame_cntl(d, r, 1, secure);
            }
            gfx11_emit_ib(d, r, vmid, ibs[i].va, ibs[i].ndw);
        } else {   /* gfx_v11_0_ring_emit_ib_compute */
            gfx11_write(d, r, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
            gfx11_write(d, r, (uint32_t)ibs[i].va & ~3u);
            gfx11_write(d, r, (uint32_t)(ibs[i].va >> 32));
            gfx11_write(d, r, INDIRECT_BUFFER_VALID | ibs[i].ndw | (vmid << 24));
        }
    }
    if (gfx)
        gfx11_emit_frame_cntl(d, r, 0, secure);
    if (x && x->uf_addr)
        gfx11_emit_user_fence(d, r, x->uf_addr, (uint32_t)(r->fence_seq + 1));
    if (gfx) {
        gfx11_emit_gfx_shadow(d, r, 0, 0, 0, 0, 0);
        gfx11_emit_init_cond_exec(d, r, cond_addr);
    }
    seq = gfx11_emit_fence(d, r);
    if (gfx)
        gfx11_ring_patch_cond_exec(d, r, cond_exec);
    gfx11_commit(d, r);
    r->last_ctx = ctx;
    r->last_vmid = vmid;
    return seq;
}
