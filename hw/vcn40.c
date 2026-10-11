/*
 * vcn40.c - see vcn40.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vcn40.h"
#include "doorbell.h"
#include "gen/vcn_4_0_0.h"

#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_GET_FIELD(v, reg, field) (((v) & (uint32_t)REG_FIELD_MASK(reg, field)) >> REG_FIELD_SHIFT(reg, field))

/* VCN registers by instance; JPEG 4.0 sits in VCN instance 0's space
 * (amdgpu_discovery copies the VCN bases to JPEG_HWIP) */
/* the register is pasted here, not in a nested macro, so 'reg' is not
 * expanded before its _BASE_IDX is formed */
#define VOFF(d, i, reg)       rdna3_reg_offset((d), HWIP_VCN, (int)(i), reg##_BASE_IDX, reg)
#define RV(d, i, reg)         rdna3_rreg((d), rdna3_reg_offset((d), HWIP_VCN, (int)(i), reg##_BASE_IDX, reg))
#define WV(d, i, reg, v)      rdna3_wreg((d), rdna3_reg_offset((d), HWIP_VCN, (int)(i), reg##_BASE_IDX, reg), (v))
#define RJ(d, reg)            rdna3_rreg((d), rdna3_reg_offset((d), HWIP_VCN, 0, reg##_BASE_IDX, reg))
#define WJ(d, reg, v)         rdna3_wreg((d), rdna3_reg_offset((d), HWIP_VCN, 0, reg##_BASE_IDX, reg), (v))
/* WREG32_P: keep the bits of 'mask', take the others from 'val' */
#define WV_P(d, i, reg, val, mask) do { \
    uint32_t o_ = rdna3_reg_offset((d), HWIP_VCN, (int)(i), reg##_BASE_IDX, reg); \
    uint32_t t_ = rdna3_rreg((d), o_) & (uint32_t)(mask); \
    t_ |= (uint32_t)(val) & ~(uint32_t)(mask); rdna3_wreg((d), o_, t_); } while (0)
/* SOC15_WAIT_ON_RREG */
#define WAIT_V(d, i, reg, expect, mask) \
    rdna3_wait_reg((d), rdna3_reg_offset((d), HWIP_VCN, (int)(i), reg##_BASE_IDX, reg), \
                   (uint32_t)(expect), (uint32_t)(mask), 0, 0)

#define GPU_PAGE_ALIGN(x) (((uint64_t)(x) + 4095) & ~4095ull)

static void mdelay(rdna3_dev *d, uint32_t ms)
{
    d->os.udelay(d->os.ctx, ms * 1000);
}

static uint32_t wb_read32(rdna3_dev *d, const rdna3_vcn_ring *r, uint32_t off)
{
    uint32_t v = 0;
    rdna3_vram_read(d, r->wb + off, &v, 4);
    return v;
}

static uint8_t fws_read8(rdna3_dev *d, const rdna3_vcn *v, uint32_t i, uint32_t off)
{
    uint8_t b = 0;
    rdna3_vram_read(d, v->fw_shared[i] + off, &b, 1);
    return b;
}

static void fws_write8(rdna3_dev *d, const rdna3_vcn *v, uint32_t i, uint32_t off, uint8_t b)
{
    rdna3_vram_write(d, v->fw_shared[i] + off, &b, 1);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ============================== VCN 4.0 ============================== */

/* Ported from vcn_v4_0_early_init and amdgpu_vcn_early_init: the unified
 * queue reuses enc ring 0, the instances come from discovery, the
 * firmware (one file for both instances on 4.0.0) from vcn_4_0_0.bin.
 * The header part of amdgpu_vcn_sw_init (fw_version and the naming
 * convention it logs) is done here too, so AMDGPU_INFO_FW_VCN has it. */
int vcn40_early_init(rdna3_dev *d, rdna3_vcn *v, const uint8_t *fw, size_t fw_len,
                     uint32_t gb_addr_config, int dpm_enabled)
{
    uint32_t i, fw_check, ver;
    int keep_sw = v->sw_ready;

    if (!keep_sw) {
        uint8_t *p = (uint8_t *)v;
        for (i = 0; i < sizeof(*v); i++)
            p[i] = 0;
    }
    if (!d->ipver[HWIP_VCN].present || d->ipver[HWIP_VCN].major != 4 || d->ipver[HWIP_VCN].minor != 0) {
        RERR(d, "VCN: discovery has no VCN 4.0");
        return RDNA3_ENODEV;
    }
    v->num_vcn_inst = 0;
    for (i = 0; i < RD_VCN_MAX_INST; i++)
        if (d->reg_base[HWIP_VCN][i][1])
            v->num_vcn_inst = i + 1;
    if (!v->num_vcn_inst)
        return RDNA3_ENODEV;
    v->harvest_config = 0;
    v->num_enc_rings = 1;   /* re-use enc ring as unified ring */
    /* soc21_common_early_init, GC 11.0.3: VCN_MGCG | JPEG_MGCG,
     * PG VCN | VCN_DPG | JPEG; DPG masked until it is ported */
    v->cg_flags = RD_CG_VCN_MGCG | RD_CG_JPEG_MGCG;
    v->pg_flags = RD_PG_VCN | RD_PG_JPEG;
    v->gb_addr_config = gb_addr_config;
    v->dpm_enabled = dpm_enabled;
    /* the block starts gated (also after a GPU reset: the PMFW powered it
     * down); smu_sw_init: power_gate.vcn_gated / jpeg_gated = 1 */
    v->cur_state = RD_PG_STATE_GATE;
    v->jpeg_cur_state = RD_PG_STATE_GATE;
    v->vcn_gated = 1;
    v->jpeg_gated = 1;
    v->total_submission_cnt = 0;
    v->jpeg_total_submission_cnt = 0;

    /* the firmware file the OS read (amdgpu/vcn_4_0_0.bin): a common header */
    if (!fw || fw_len < 32 || rd_le32(fw) > fw_len) {
        RERR(d, "VCN: no firmware (vcn_4_0_0.bin)");
        return RDNA3_ENOENT;
    }
    ver = rd_le32(fw + 16);
    v->fw_version = ver;
    v->fw_ucode_size = rd_le32(fw + 20);
    fw_check = (ver >> 20) & 0xf;
    if (fw_check) {
        v->fw_rev = ver & 0xfff;
        v->enc_minor = (ver >> 12) & 0xff;
        v->enc_major = fw_check;
        v->dec_ver = (ver >> 24) & 0xf;
        v->vep = (ver >> 28) & 0xf;
        RLOG(d, "VCN: %u instance(s), found VCN firmware Version ENC: %u.%u DEC: %u VEP: %u Revision: %u",
             v->num_vcn_inst, v->enc_major, v->enc_minor, v->dec_ver, v->vep, v->fw_rev);
    } else {
        RLOG(d, "VCN: %u instance(s), found VCN firmware Version: %u.%u Family ID: %u", v->num_vcn_inst,
             (ver >> 24) & 0xff, (ver >> 8) & 0xff, ver & 0xff);
    }
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_fw_shared_init (dGPU, no firmware log) */
static void fw_shared_init(rdna3_dev *d, rdna3_vcn *v, uint32_t i)
{
    uint8_t z[RD_VCN4_FWS_SIZE];
    uint32_t flags = RD_FW_SHARED_FLAG_0_UNIFIED_QUEUE | RD_VCN_SMU_DPM_INTERFACE_FLAG, k;
    for (k = 0; k < sizeof(z); k++)
        z[k] = 0;
    rdna3_vram_write(d, v->fw_shared[i], z, sizeof(z));
    rdna3_vram_write(d, v->fw_shared[i] + RD_VCN4_FWS_PRESENT_FLAG_0, &flags, 4);
    fws_write8(d, v, i, RD_VCN4_FWS_SQ_IS_ENABLED, 1);
    fws_write8(d, v, i, RD_VCN4_FWS_SMU_DPM_INTERFACE, RD_VCN_SMU_DPM_INTERFACE_DGPU);
}

/* amdgpu_ring_init: ring and write-pointer shadow in VRAM */
static int ring_init(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t size, uint32_t align_mask, const char *tag)
{
    uint8_t z[64] = { 0 };
    if (!r->ring || r->ring == UINT64_MAX) {
        r->ring = rdna3_vram_alloc(d, size, 4096, tag);
        r->wb = rdna3_vram_alloc(d, 4096, 4096, tag);
        if (r->ring == UINT64_MAX || r->wb == UINT64_MAX)
            return RDNA3_ENOMEM;
    }
    r->ring_size = size;
    r->buf_mask = size / 4 - 1;
    r->align_mask = align_mask;
    r->wptr = 0;
    r->gpu_addr = rdna3_vram_mc(d, r->ring);
    r->ready = 0;
    rdna3_vram_fill(d, r->ring, 0, size);
    rdna3_vram_write(d, r->wb, z, sizeof(z));
    rdna3_vram_write(d, r->wb + RD_VCN_WB_FENCE, &r->fence_seq, 4);
    rdna3_hdp_flush(d);
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_sw_init, with amdgpu_vcn_sw_init (vcpu_bo: stack,
 * context and fw_shared; the image itself is in the TMR) and
 * amdgpu_vcn_resume (no saved_bo: the block starts zeroed) */
int vcn40_sw_init(rdna3_dev *d, rdna3_vcn *v, const uint64_t tmr_mc[RD_VCN_MAX_INST])
{
    const uint64_t bo_size = RD_VCN_STACK_SIZE + RD_VCN_CONTEXT_SIZE + RD_VCN_FW_SHARED_SIZE;
    uint32_t i;
    int r;

    for (i = 0; i < v->num_vcn_inst; i++) {
        rdna3_vcn_ring *ring = &v->ring_enc[i];
        if (v->harvest_config & (1u << i))
            continue;
        v->tmr_mc[i] = tmr_mc ? tmr_mc[i] : 0;
        if (!v->vcpu_bo[i]) {
            v->vcpu_bo[i] = rdna3_vram_alloc(d, bo_size, 4096, "vcn-vcpu");
            if (v->vcpu_bo[i] == UINT64_MAX) {
                v->vcpu_bo[i] = 0;
                return RDNA3_ENOMEM;
            }
        }
        v->fw_shared[i] = v->vcpu_bo[i] + bo_size - RD_VCN_FW_SHARED_SIZE;
        /* amdgpu_vcn_resume */
        rdna3_vram_fill(d, v->vcpu_bo[i], 0, bo_size);

        ring->me = i;
        ring->jpeg = 0;
        ring->use_doorbell = d->doorbell_ok;
        ring->doorbell_index = (RD_DB_VCN0_1 << 1) + 2 + 8 * i;
        r = ring_init(d, ring, RD_VCN_RING_SIZE, 0x3f, i ? "vcn-unified-1" : "vcn-unified-0");
        if (r)
            return r;
        fw_shared_init(d, v, i);
    }
    rdna3_hdp_flush(d);
    v->sw_ready = 1;
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_mc_resume (firmware loaded by the PSP: cache
 * window 0 is the image in the TMR) */
static void vcn40_mc_resume(rdna3_dev *d, rdna3_vcn *v, uint32_t inst)
{
    uint32_t size = (uint32_t)GPU_PAGE_ALIGN(v->fw_ucode_size + 8), offset;
    uint64_t gpu_addr = rdna3_vram_mc(d, v->vcpu_bo[inst]);
    uint64_t fws = rdna3_vram_mc(d, v->fw_shared[inst]);

    /* cache window 0: fw */
    WV(d, inst, regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, (uint32_t)v->tmr_mc[inst]);
    WV(d, inst, regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, (uint32_t)(v->tmr_mc[inst] >> 32));
    WV(d, inst, regUVD_VCPU_CACHE_OFFSET0, 0);
    offset = 0;
    WV(d, inst, regUVD_VCPU_CACHE_SIZE0, size);

    /* cache window 1: stack */
    WV(d, inst, regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW, (uint32_t)(gpu_addr + offset));
    WV(d, inst, regUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH, (uint32_t)((gpu_addr + offset) >> 32));
    WV(d, inst, regUVD_VCPU_CACHE_OFFSET1, 0);
    WV(d, inst, regUVD_VCPU_CACHE_SIZE1, RD_VCN_STACK_SIZE);

    /* cache window 2: context */
    WV(d, inst, regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW, (uint32_t)(gpu_addr + offset + RD_VCN_STACK_SIZE));
    WV(d, inst, regUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH,
       (uint32_t)((gpu_addr + offset + RD_VCN_STACK_SIZE) >> 32));
    WV(d, inst, regUVD_VCPU_CACHE_OFFSET2, 0);
    WV(d, inst, regUVD_VCPU_CACHE_SIZE2, RD_VCN_CONTEXT_SIZE);

    /* non-cache window */
    WV(d, inst, regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW, (uint32_t)fws);
    WV(d, inst, regUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH, (uint32_t)(fws >> 32));
    WV(d, inst, regUVD_VCPU_NONCACHE_OFFSET0, 0);
    WV(d, inst, regUVD_VCPU_NONCACHE_SIZE0, (uint32_t)GPU_PAGE_ALIGN(RD_VCN4_FWS_SIZE));
}

/* Ported from vcn_v4_0_disable_static_power_gating */
static void vcn40_disable_static_power_gating(rdna3_dev *d, rdna3_vcn *v, uint32_t inst)
{
    uint32_t data;

    if (v->pg_flags & RD_PG_VCN) {
        data = (1u << UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDS_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDLM_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTC_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTA_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDAB_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTB_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDNA_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDNB_PWR_CONFIG__SHIFT);
        WV(d, inst, regUVD_PGFSM_CONFIG, data);
        WAIT_V(d, inst, regUVD_PGFSM_STATUS, RD_UVD_PGFSM_STATUS__UVDM_UVDU_UVDLM_PWR_ON_3_0, 0x3F3FFFFF);
    } else {
        uint32_t value = inst ? 0x2200800u : 0;
        data = (1u << UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDS_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDLM_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDTC_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDTA_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDAB_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDTB_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDNA_PWR_CONFIG__SHIFT
              | 1u << UVD_PGFSM_CONFIG__UVDNB_PWR_CONFIG__SHIFT);
        WV(d, inst, regUVD_PGFSM_CONFIG, data);
        WAIT_V(d, inst, regUVD_PGFSM_STATUS, value, 0x3F3FFFFF);
    }

    data = RV(d, inst, regUVD_POWER_STATUS);
    data &= ~0x103u;
    if (v->pg_flags & RD_PG_VCN)
        data |= RD_UVD_PGFSM_CONFIG__UVDM_UVDU_PWR_ON | UVD_POWER_STATUS__UVD_PG_EN_MASK;
    WV(d, inst, regUVD_POWER_STATUS, data);
}

/* Ported from vcn_v4_0_enable_static_power_gating */
static void vcn40_enable_static_power_gating(rdna3_dev *d, rdna3_vcn *v, uint32_t inst)
{
    uint32_t data;

    if (v->pg_flags & RD_PG_VCN) {
        /* Before power off, this indicator has to be turned on */
        data = RV(d, inst, regUVD_POWER_STATUS);
        data &= ~(uint32_t)UVD_POWER_STATUS__UVD_POWER_STATUS_MASK;
        data |= RD_UVD_POWER_STATUS__UVD_POWER_STATUS_TILES_OFF;
        WV(d, inst, regUVD_POWER_STATUS, data);

        data = (2u << UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDS_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTC_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTA_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDLM_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDAB_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDTB_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDNA_PWR_CONFIG__SHIFT
              | 2u << UVD_PGFSM_CONFIG__UVDNB_PWR_CONFIG__SHIFT);
        WV(d, inst, regUVD_PGFSM_CONFIG, data);

        data = (2u << UVD_PGFSM_STATUS__UVDM_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDS_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDF_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDTC_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDB_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDTA_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDLM_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDTD_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDTE_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDE_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDAB_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDTB_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDNA_PWR_STATUS__SHIFT
              | 2u << UVD_PGFSM_STATUS__UVDNB_PWR_STATUS__SHIFT);
        WAIT_V(d, inst, regUVD_PGFSM_STATUS, data, 0x3F3FFFFF);
    }
}

/* Ported from vcn_v4_0_disable_clock_gating: with AMD_CG_SUPPORT_VCN_MGCG
 * (GC 11.0.3) the firmware gates on its own and this is a no-op */
static void vcn40_disable_clock_gating(rdna3_dev *d, rdna3_vcn *v, uint32_t inst)
{
    uint32_t data;

    if (v->cg_flags & RD_CG_VCN_MGCG)
        return;

    /* VCN disable CGC */
    data = RV(d, inst, regUVD_CGC_CTRL);
    data &= ~(uint32_t)UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
    data |= 1u << UVD_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
    data |= 4u << UVD_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
    WV(d, inst, regUVD_CGC_CTRL, data);

    data = RV(d, inst, regUVD_CGC_GATE);
    data &= ~(uint32_t)(UVD_CGC_GATE__SYS_MASK | UVD_CGC_GATE__UDEC_MASK | UVD_CGC_GATE__MPEG2_MASK |
                        UVD_CGC_GATE__REGS_MASK | UVD_CGC_GATE__RBC_MASK | UVD_CGC_GATE__LMI_MC_MASK |
                        UVD_CGC_GATE__LMI_UMC_MASK | UVD_CGC_GATE__IDCT_MASK | UVD_CGC_GATE__MPRD_MASK |
                        UVD_CGC_GATE__MPC_MASK | UVD_CGC_GATE__LBSI_MASK | UVD_CGC_GATE__LRBBM_MASK |
                        UVD_CGC_GATE__UDEC_RE_MASK | UVD_CGC_GATE__UDEC_CM_MASK | UVD_CGC_GATE__UDEC_IT_MASK |
                        UVD_CGC_GATE__UDEC_DB_MASK | UVD_CGC_GATE__UDEC_MP_MASK | UVD_CGC_GATE__WCB_MASK |
                        UVD_CGC_GATE__VCPU_MASK | UVD_CGC_GATE__MMSCH_MASK);
    WV(d, inst, regUVD_CGC_GATE, data);
    WAIT_V(d, inst, regUVD_CGC_GATE, 0, 0xFFFFFFFF);

    data = RV(d, inst, regUVD_CGC_CTRL);
    data &= ~(uint32_t)(UVD_CGC_CTRL__UDEC_RE_MODE_MASK | UVD_CGC_CTRL__UDEC_CM_MODE_MASK |
                        UVD_CGC_CTRL__UDEC_IT_MODE_MASK | UVD_CGC_CTRL__UDEC_DB_MODE_MASK |
                        UVD_CGC_CTRL__UDEC_MP_MODE_MASK | UVD_CGC_CTRL__SYS_MODE_MASK |
                        UVD_CGC_CTRL__UDEC_MODE_MASK | UVD_CGC_CTRL__MPEG2_MODE_MASK |
                        UVD_CGC_CTRL__REGS_MODE_MASK | UVD_CGC_CTRL__RBC_MODE_MASK |
                        UVD_CGC_CTRL__LMI_MC_MODE_MASK | UVD_CGC_CTRL__LMI_UMC_MODE_MASK |
                        UVD_CGC_CTRL__IDCT_MODE_MASK | UVD_CGC_CTRL__MPRD_MODE_MASK |
                        UVD_CGC_CTRL__MPC_MODE_MASK | UVD_CGC_CTRL__LBSI_MODE_MASK |
                        UVD_CGC_CTRL__LRBBM_MODE_MASK | UVD_CGC_CTRL__WCB_MODE_MASK |
                        UVD_CGC_CTRL__VCPU_MODE_MASK | UVD_CGC_CTRL__MMSCH_MODE_MASK);
    WV(d, inst, regUVD_CGC_CTRL, data);

    data = RV(d, inst, regUVD_SUVD_CGC_GATE);
    data |= (uint32_t)(UVD_SUVD_CGC_GATE__SRE_MASK | UVD_SUVD_CGC_GATE__SIT_MASK | UVD_SUVD_CGC_GATE__SMP_MASK |
                       UVD_SUVD_CGC_GATE__SCM_MASK | UVD_SUVD_CGC_GATE__SDB_MASK |
                       UVD_SUVD_CGC_GATE__SRE_H264_MASK | UVD_SUVD_CGC_GATE__SRE_HEVC_MASK |
                       UVD_SUVD_CGC_GATE__SIT_H264_MASK | UVD_SUVD_CGC_GATE__SIT_HEVC_MASK |
                       UVD_SUVD_CGC_GATE__SCM_H264_MASK | UVD_SUVD_CGC_GATE__SCM_HEVC_MASK |
                       UVD_SUVD_CGC_GATE__SDB_H264_MASK | UVD_SUVD_CGC_GATE__SDB_HEVC_MASK |
                       UVD_SUVD_CGC_GATE__SCLR_MASK | UVD_SUVD_CGC_GATE__UVD_SC_MASK |
                       UVD_SUVD_CGC_GATE__ENT_MASK | UVD_SUVD_CGC_GATE__SIT_HEVC_DEC_MASK |
                       UVD_SUVD_CGC_GATE__SIT_HEVC_ENC_MASK | UVD_SUVD_CGC_GATE__SITE_MASK |
                       UVD_SUVD_CGC_GATE__SRE_VP9_MASK | UVD_SUVD_CGC_GATE__SCM_VP9_MASK |
                       UVD_SUVD_CGC_GATE__SIT_VP9_DEC_MASK | UVD_SUVD_CGC_GATE__SDB_VP9_MASK |
                       UVD_SUVD_CGC_GATE__IME_HEVC_MASK);
    WV(d, inst, regUVD_SUVD_CGC_GATE, data);

    data = RV(d, inst, regUVD_SUVD_CGC_CTRL);
    data &= ~(uint32_t)(UVD_SUVD_CGC_CTRL__SRE_MODE_MASK | UVD_SUVD_CGC_CTRL__SIT_MODE_MASK |
                        UVD_SUVD_CGC_CTRL__SMP_MODE_MASK | UVD_SUVD_CGC_CTRL__SCM_MODE_MASK |
                        UVD_SUVD_CGC_CTRL__SDB_MODE_MASK | UVD_SUVD_CGC_CTRL__SCLR_MODE_MASK |
                        UVD_SUVD_CGC_CTRL__UVD_SC_MODE_MASK | UVD_SUVD_CGC_CTRL__ENT_MODE_MASK |
                        UVD_SUVD_CGC_CTRL__IME_MODE_MASK | UVD_SUVD_CGC_CTRL__SITE_MODE_MASK);
    WV(d, inst, regUVD_SUVD_CGC_CTRL, data);
}

/* Ported from vcn_v4_0_enable_clock_gating (a no-op with VCN_MGCG, as above) */
static void vcn40_enable_clock_gating(rdna3_dev *d, rdna3_vcn *v, uint32_t inst)
{
    uint32_t data;

    if (v->cg_flags & RD_CG_VCN_MGCG)
        return;

    /* enable VCN CGC */
    data = RV(d, inst, regUVD_CGC_CTRL);
    data |= 0u << UVD_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
    data |= 1u << UVD_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
    data |= 4u << UVD_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
    WV(d, inst, regUVD_CGC_CTRL, data);

    data = RV(d, inst, regUVD_CGC_CTRL);
    data |= (uint32_t)(UVD_CGC_CTRL__UDEC_RE_MODE_MASK | UVD_CGC_CTRL__UDEC_CM_MODE_MASK |
                       UVD_CGC_CTRL__UDEC_IT_MODE_MASK | UVD_CGC_CTRL__UDEC_DB_MODE_MASK |
                       UVD_CGC_CTRL__UDEC_MP_MODE_MASK | UVD_CGC_CTRL__SYS_MODE_MASK |
                       UVD_CGC_CTRL__UDEC_MODE_MASK | UVD_CGC_CTRL__MPEG2_MODE_MASK |
                       UVD_CGC_CTRL__REGS_MODE_MASK | UVD_CGC_CTRL__RBC_MODE_MASK |
                       UVD_CGC_CTRL__LMI_MC_MODE_MASK | UVD_CGC_CTRL__LMI_UMC_MODE_MASK |
                       UVD_CGC_CTRL__IDCT_MODE_MASK | UVD_CGC_CTRL__MPRD_MODE_MASK |
                       UVD_CGC_CTRL__MPC_MODE_MASK | UVD_CGC_CTRL__LBSI_MODE_MASK |
                       UVD_CGC_CTRL__LRBBM_MODE_MASK | UVD_CGC_CTRL__WCB_MODE_MASK |
                       UVD_CGC_CTRL__VCPU_MODE_MASK | UVD_CGC_CTRL__MMSCH_MODE_MASK);
    WV(d, inst, regUVD_CGC_CTRL, data);

    data = RV(d, inst, regUVD_SUVD_CGC_CTRL);
    data |= (uint32_t)(UVD_SUVD_CGC_CTRL__SRE_MODE_MASK | UVD_SUVD_CGC_CTRL__SIT_MODE_MASK |
                       UVD_SUVD_CGC_CTRL__SMP_MODE_MASK | UVD_SUVD_CGC_CTRL__SCM_MODE_MASK |
                       UVD_SUVD_CGC_CTRL__SDB_MODE_MASK | UVD_SUVD_CGC_CTRL__SCLR_MODE_MASK |
                       UVD_SUVD_CGC_CTRL__UVD_SC_MODE_MASK | UVD_SUVD_CGC_CTRL__ENT_MODE_MASK |
                       UVD_SUVD_CGC_CTRL__IME_MODE_MASK | UVD_SUVD_CGC_CTRL__SITE_MODE_MASK);
    WV(d, inst, regUVD_SUVD_CGC_CTRL, data);
}

/* Ported from smu_dpm_set_vcn_enable (the power_gate.vcn_gated bookkeeping
 * around the ppt's dpm_set_vcn_enable) */
static int vcn40_dpm_set_vcn_enable(rdna3_dev *d, rdna3_vcn *v, int enable)
{
    int r;
    if (v->vcn_gated ^ (enable ? 1 : 0))
        return RDNA3_OK;
    r = smu13_set_vcn_enable(d, v->num_vcn_inst, v->harvest_config, enable);
    if (!r)
        v->vcn_gated = !enable;
    return r;
}

/* Ported from smu_dpm_set_jpeg_enable */
static int jpeg40_dpm_set_jpeg_enable(rdna3_dev *d, rdna3_vcn *v, int enable)
{
    int r;
    if (v->jpeg_gated ^ (enable ? 1 : 0))
        return RDNA3_OK;
    r = smu13_set_jpeg_enable(d, enable);
    if (!r)
        v->jpeg_gated = !enable;
    return r;
}

/* Ported from vcn_v4_0_start (non-DPG path; the DPG start is a
 * todo, see vcn40.h) */
static int vcn40_start(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t tmp, i, j, k, status = 0;
    int r = 0;

    if (v->dpm_enabled) {   /* amdgpu_dpm_enable_uvd(adev, true) */
        int s = vcn40_dpm_set_vcn_enable(d, v, 1);
        if (s)
            RERR(d, "VCN: PowerUpVcn failed (%d)", s);
    }

    for (i = 0; i < v->num_vcn_inst; ++i) {
        rdna3_vcn_ring *ring = &v->ring_enc[i];
        uint8_t qm;
        if (v->harvest_config & (1u << i))
            continue;

        /* disable VCN power gating */
        vcn40_disable_static_power_gating(d, v, i);

        /* set VCN status busy */
        tmp = RV(d, i, regUVD_STATUS) | RD_UVD_STATUS__UVD_BUSY;
        WV(d, i, regUVD_STATUS, tmp);

        /* SW clock gating */
        vcn40_disable_clock_gating(d, v, i);

        /* enable VCPU clock */
        WV_P(d, i, regUVD_VCPU_CNTL, UVD_VCPU_CNTL__CLK_EN_MASK, ~UVD_VCPU_CNTL__CLK_EN_MASK);

        /* disable master interrupt */
        WV_P(d, i, regUVD_MASTINT_EN, 0, ~UVD_MASTINT_EN__VCPU_EN_MASK);

        /* enable LMI MC and UMC channels */
        WV_P(d, i, regUVD_LMI_CTRL2, 0, ~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);

        tmp = RV(d, i, regUVD_SOFT_RESET);
        tmp &= ~(uint32_t)UVD_SOFT_RESET__LMI_SOFT_RESET_MASK;
        tmp &= ~(uint32_t)UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK;
        WV(d, i, regUVD_SOFT_RESET, tmp);

        /* setup regUVD_LMI_CTRL */
        tmp = RV(d, i, regUVD_LMI_CTRL);
        WV(d, i, regUVD_LMI_CTRL, tmp | UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK | UVD_LMI_CTRL__MASK_MC_URGENT_MASK |
                                  UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK | UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK);

        /* setup regUVD_MPC_CNTL */
        tmp = RV(d, i, regUVD_MPC_CNTL);
        tmp &= ~(uint32_t)UVD_MPC_CNTL__REPLACEMENT_MODE_MASK;
        tmp |= 0x2u << UVD_MPC_CNTL__REPLACEMENT_MODE__SHIFT;
        WV(d, i, regUVD_MPC_CNTL, tmp);

        /* setup UVD_MPC_SET_MUXA0 */
        WV(d, i, regUVD_MPC_SET_MUXA0, ((0x1u << UVD_MPC_SET_MUXA0__VARA_1__SHIFT) |
                                        (0x2u << UVD_MPC_SET_MUXA0__VARA_2__SHIFT) |
                                        (0x3u << UVD_MPC_SET_MUXA0__VARA_3__SHIFT) |
                                        (0x4u << UVD_MPC_SET_MUXA0__VARA_4__SHIFT)));
        /* setup UVD_MPC_SET_MUXB0 */
        WV(d, i, regUVD_MPC_SET_MUXB0, ((0x1u << UVD_MPC_SET_MUXB0__VARB_1__SHIFT) |
                                        (0x2u << UVD_MPC_SET_MUXB0__VARB_2__SHIFT) |
                                        (0x3u << UVD_MPC_SET_MUXB0__VARB_3__SHIFT) |
                                        (0x4u << UVD_MPC_SET_MUXB0__VARB_4__SHIFT)));
        /* setup UVD_MPC_SET_MUX */
        WV(d, i, regUVD_MPC_SET_MUX, ((0x0u << UVD_MPC_SET_MUX__SET_0__SHIFT) |
                                      (0x1u << UVD_MPC_SET_MUX__SET_1__SHIFT) |
                                      (0x2u << UVD_MPC_SET_MUX__SET_2__SHIFT)));

        vcn40_mc_resume(d, v, i);

        /* VCN global tiling registers */
        WV(d, i, regUVD_GFX10_ADDR_CONFIG, v->gb_addr_config);

        /* unblock VCPU register access */
        WV_P(d, i, regUVD_RB_ARB_CTRL, 0, ~UVD_RB_ARB_CTRL__VCPU_DIS_MASK);

        /* release VCPU reset to boot */
        WV_P(d, i, regUVD_VCPU_CNTL, 0, ~UVD_VCPU_CNTL__BLK_RST_MASK);

        for (j = 0; j < 10; ++j) {
            for (k = 0; k < 100; ++k) {
                status = RV(d, i, regUVD_STATUS);
                if (status & 2)
                    break;
                mdelay(d, 10);
            }
            r = 0;
            if (status & 2)
                break;
            RERR(d, "VCN[%u] is not responding, trying to reset the VCPU!!!", i);
            WV_P(d, i, regUVD_VCPU_CNTL, UVD_VCPU_CNTL__BLK_RST_MASK, ~UVD_VCPU_CNTL__BLK_RST_MASK);
            mdelay(d, 10);
            WV_P(d, i, regUVD_VCPU_CNTL, 0, ~UVD_VCPU_CNTL__BLK_RST_MASK);
            mdelay(d, 10);
            r = -1;
        }
        if (r) {
            RERR(d, "VCN[%u] is not responding, giving up!!! (UVD_STATUS 0x%08x)", i, status);
            v->last_err = RDNA3_ETIME;
            return RDNA3_ETIME;
        }

        /* enable master interrupt */
        WV_P(d, i, regUVD_MASTINT_EN, UVD_MASTINT_EN__VCPU_EN_MASK, ~UVD_MASTINT_EN__VCPU_EN_MASK);

        /* clear the busy bit of VCN_STATUS */
        WV_P(d, i, regUVD_STATUS, 0, ~(2u << UVD_STATUS__VCPU_REPORT__SHIFT));

        WV(d, i, regVCN_RB1_DB_CTRL, ring->doorbell_index << VCN_RB1_DB_CTRL__OFFSET__SHIFT |
                                     VCN_RB1_DB_CTRL__EN_MASK);

        WV(d, i, regUVD_RB_BASE_LO, (uint32_t)ring->gpu_addr);
        WV(d, i, regUVD_RB_BASE_HI, (uint32_t)(ring->gpu_addr >> 32));
        WV(d, i, regUVD_RB_SIZE, ring->ring_size / 4);

        tmp = RV(d, i, regVCN_RB_ENABLE);
        tmp &= ~(uint32_t)VCN_RB_ENABLE__RB1_EN_MASK;
        WV(d, i, regVCN_RB_ENABLE, tmp);
        qm = fws_read8(d, v, i, RD_VCN4_FWS_SQ_QUEUE_MODE);
        fws_write8(d, v, i, RD_VCN4_FWS_SQ_QUEUE_MODE, (uint8_t)(qm | RD_FW_QUEUE_RING_RESET));
        rdna3_hdp_flush(d);
        WV(d, i, regUVD_RB_RPTR, 0);
        WV(d, i, regUVD_RB_WPTR, 0);

        tmp = RV(d, i, regUVD_RB_RPTR);
        WV(d, i, regUVD_RB_WPTR, tmp);
        ring->wptr = RV(d, i, regUVD_RB_WPTR);
        {
            uint32_t w = (uint32_t)ring->wptr;
            rdna3_vram_write(d, ring->wb + RD_VCN_WB_WPTR, &w, 4);
        }

        tmp = RV(d, i, regVCN_RB_ENABLE);
        tmp |= VCN_RB_ENABLE__RB1_EN_MASK;
        WV(d, i, regVCN_RB_ENABLE, tmp);
        qm = fws_read8(d, v, i, RD_VCN4_FWS_SQ_QUEUE_MODE);
        fws_write8(d, v, i, RD_VCN4_FWS_SQ_QUEUE_MODE,
                   (uint8_t)(qm & ~(RD_FW_QUEUE_RING_RESET | RD_FW_QUEUE_DPG_HOLD_OFF)));
        rdna3_hdp_flush(d);
    }
    v->starts++;
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_stop (non-DPG path) */
static int vcn40_stop(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t tmp, i;
    int r;

    for (i = 0; i < v->num_vcn_inst; ++i) {
        uint8_t qm;
        if (v->harvest_config & (1u << i))
            continue;
        qm = fws_read8(d, v, i, RD_VCN4_FWS_SQ_QUEUE_MODE);
        fws_write8(d, v, i, RD_VCN4_FWS_SQ_QUEUE_MODE, (uint8_t)(qm | RD_FW_QUEUE_DPG_HOLD_OFF));
        rdna3_hdp_flush(d);

        /* wait for vcn idle */
        r = WAIT_V(d, i, regUVD_STATUS, RD_UVD_STATUS__IDLE, 0x7);
        if (r)
            return r;

        tmp = UVD_LMI_STATUS__VCPU_LMI_WRITE_CLEAN_MASK | UVD_LMI_STATUS__READ_CLEAN_MASK |
              UVD_LMI_STATUS__WRITE_CLEAN_MASK | UVD_LMI_STATUS__WRITE_CLEAN_RAW_MASK;
        r = WAIT_V(d, i, regUVD_LMI_STATUS, tmp, tmp);
        if (r)
            return r;

        /* disable LMI UMC channel */
        tmp = RV(d, i, regUVD_LMI_CTRL2);
        tmp |= UVD_LMI_CTRL2__STALL_ARB_UMC_MASK;
        WV(d, i, regUVD_LMI_CTRL2, tmp);
        tmp = UVD_LMI_STATUS__UMC_READ_CLEAN_RAW_MASK | UVD_LMI_STATUS__UMC_WRITE_CLEAN_RAW_MASK;
        r = WAIT_V(d, i, regUVD_LMI_STATUS, tmp, tmp);
        if (r)
            return r;

        /* block VCPU register access */
        WV_P(d, i, regUVD_RB_ARB_CTRL, UVD_RB_ARB_CTRL__VCPU_DIS_MASK, ~UVD_RB_ARB_CTRL__VCPU_DIS_MASK);

        /* reset VCPU */
        WV_P(d, i, regUVD_VCPU_CNTL, UVD_VCPU_CNTL__BLK_RST_MASK, ~UVD_VCPU_CNTL__BLK_RST_MASK);

        /* disable VCPU clock */
        WV_P(d, i, regUVD_VCPU_CNTL, 0, ~UVD_VCPU_CNTL__CLK_EN_MASK);

        /* apply soft reset */
        tmp = RV(d, i, regUVD_SOFT_RESET);
        tmp |= UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK;
        WV(d, i, regUVD_SOFT_RESET, tmp);
        tmp = RV(d, i, regUVD_SOFT_RESET);
        tmp |= UVD_SOFT_RESET__LMI_SOFT_RESET_MASK;
        WV(d, i, regUVD_SOFT_RESET, tmp);

        /* clear status */
        WV(d, i, regUVD_STATUS, 0);

        /* apply HW clock gating */
        vcn40_enable_clock_gating(d, v, i);

        /* enable VCN power gating */
        vcn40_enable_static_power_gating(d, v, i);
    }

    if (v->dpm_enabled) {   /* amdgpu_dpm_enable_uvd(adev, false) */
        r = vcn40_dpm_set_vcn_enable(d, v, 0);
        if (r)
            RERR(d, "VCN: PowerDownVcn failed (%d)", r);
    }
    v->stops++;
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_set_powergating_state */
int vcn40_set_powergating_state(rdna3_dev *d, rdna3_vcn *v, int state)
{
    int ret;
    if (state == v->cur_state)
        return RDNA3_OK;
    if (state == RD_PG_STATE_GATE)
        ret = vcn40_stop(d, v);
    else
        ret = vcn40_start(d, v);
    if (!ret)
        v->cur_state = state;
    return ret;
}

/* Ported from vcn_v4_0_set_clockgating_state */
int vcn40_set_clockgating_state(rdna3_dev *d, rdna3_vcn *v, int gate)
{
    uint32_t i;
    for (i = 0; i < v->num_vcn_inst; ++i) {
        if (v->harvest_config & (1u << i))
            continue;
        if (gate) {
            if (RV(d, i, regUVD_STATUS) != RD_UVD_STATUS__IDLE)
                return RDNA3_EBUSY;
            vcn40_enable_clock_gating(d, v, i);
        } else {
            vcn40_disable_clock_gating(d, v, i);
        }
    }
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_is_idle */
int vcn40_is_idle(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t i;
    int ret = 1;
    for (i = 0; i < v->num_vcn_inst; ++i) {
        if (v->harvest_config & (1u << i))
            continue;
        ret &= (RV(d, i, regUVD_STATUS) == RD_UVD_STATUS__IDLE);
    }
    return ret;
}

/* Ported from vcn_v4_0_wait_for_idle */
int vcn40_wait_for_idle(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t i;
    int ret = 0;
    for (i = 0; i < v->num_vcn_inst; ++i) {
        if (v->harvest_config & (1u << i))
            continue;
        ret = WAIT_V(d, i, regUVD_STATUS, RD_UVD_STATUS__IDLE, RD_UVD_STATUS__IDLE);
        if (ret)
            return ret;
    }
    return ret;
}

/* Ported from amdgpu_vcn_ring_begin_use (VCN 4: the firmware handles the
 * DPG pause itself, so only the power state) */
void vcn40_ring_begin_use(rdna3_dev *d, rdna3_vcn *v)
{
    int r;
    v->total_submission_cnt++;
    r = vcn40_set_powergating_state(d, v, RD_PG_STATE_UNGATE);
    if (r)
        v->last_err = r;
}

/* Ported from amdgpu_vcn_ring_end_use (the OS schedules the idle work) */
void vcn40_ring_end_use(rdna3_dev *d, rdna3_vcn *v)
{
    (void)d;
    if (v->total_submission_cnt)
        v->total_submission_cnt--;
}

/* amdgpu_fence_count_emitted: fences on the ring not signaled yet */
static uint32_t fence_count_emitted(rdna3_dev *d, const rdna3_vcn_ring *r)
{
    uint32_t done;
    if (!r->ring_size)
        return 0;
    done = wb_read32(d, r, RD_VCN_WB_FENCE);
    return r->fence_seq - done;
}

/* Ported from amdgpu_vcn_idle_work_handler */
int vcn40_idle_work_handler(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t fences = 0, j;
    for (j = 0; j < v->num_vcn_inst; ++j) {
        if (v->harvest_config & (1u << j))
            continue;
        fences += fence_count_emitted(d, &v->ring_enc[j]);
    }
    if (!fences && !v->total_submission_cnt) {
        int r = vcn40_set_powergating_state(d, v, RD_PG_STATE_GATE);
        if (r)
            v->last_err = r;
        return 1;
    }
    return 0;
}

/* --------------------------- the unified ring --------------------------- */

/* Ported from vcn_v4_0_unified_ring_get_rptr */
uint64_t vcn40_unified_ring_get_rptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    return RV(d, r->me, regUVD_RB_RPTR);
}

/* Ported from vcn_v4_0_unified_ring_get_wptr */
uint64_t vcn40_unified_ring_get_wptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    if (r->use_doorbell)
        return wb_read32(d, r, RD_VCN_WB_WPTR);
    return RV(d, r->me, regUVD_RB_WPTR);
}

/* Ported from vcn_v4_0_unified_ring_set_wptr */
void vcn40_unified_ring_set_wptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    uint32_t w = (uint32_t)r->wptr;
    if (r->use_doorbell) {
        rdna3_vram_write(d, r->wb + RD_VCN_WB_WPTR, &w, 4);
        rdna3_hdp_flush(d);
        rdna3_doorbell_wr32(d, r->doorbell_index, w);
    } else {
        rdna3_hdp_flush(d);
        WV(d, r->me, regUVD_RB_WPTR, w);
    }
}

/* Ported from jpeg_v4_0_dec_ring_get_rptr */
uint64_t jpeg40_dec_ring_get_rptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    (void)r;
    return RJ(d, regUVD_JRBC_RB_RPTR);
}

/* Ported from jpeg_v4_0_dec_ring_get_wptr */
uint64_t jpeg40_dec_ring_get_wptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    if (r->use_doorbell)
        return wb_read32(d, r, RD_VCN_WB_WPTR);
    return RJ(d, regUVD_JRBC_RB_WPTR);
}

/* Ported from jpeg_v4_0_dec_ring_set_wptr */
void jpeg40_dec_ring_set_wptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    uint32_t w = (uint32_t)r->wptr;
    if (r->use_doorbell) {
        rdna3_vram_write(d, r->wb + RD_VCN_WB_WPTR, &w, 4);
        rdna3_hdp_flush(d);
        rdna3_doorbell_wr32(d, r->doorbell_index, w);
    } else {
        rdna3_hdp_flush(d);
        WJ(d, regUVD_JRBC_RB_WPTR, w);
    }
}

static uint64_t ring_get_rptr(rdna3_dev *d, rdna3_vcn_ring *r)
{
    return r->jpeg ? jpeg40_dec_ring_get_rptr(d, r) : vcn40_unified_ring_get_rptr(d, r);
}

/* amdgpu_ring_alloc: room for 'ndw' (plus the commit's padding); amdgpu's
 * scheduler bounds what is in flight, here the ring's rptr does */
int vcn40_ring_alloc(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t ndw)
{
    uint32_t i, need = ndw + r->align_mask + 1, size = r->buf_mask + 1, used = 0;
    if (need >= size)
        return RDNA3_EINVAL;
    for (i = 0; i < d->usec_timeout; i++) {
        used = (uint32_t)(r->wptr - ring_get_rptr(d, r)) & r->buf_mask;
        if (size - used > need)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "%s ring %u: no room for %u dwords (rptr 0x%llx wptr 0x%llx)", r->jpeg ? "JPEG" : "VCN", r->me,
         ndw, (unsigned long long)ring_get_rptr(d, r), (unsigned long long)r->wptr);
    return RDNA3_ETIME;
}

/* amdgpu_ring_write: wptr wraps at the ring size (ptr_mask = buf_mask) */
void vcn40_ring_write(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t v)
{
    rdna3_vram_write(d, r->ring + (r->wptr & r->buf_mask) * 4, &v, 4);
    r->wptr = (r->wptr + 1) & r->buf_mask;
}

/* Ported from jpeg_v2_0_dec_ring_nop (JRBC NOPs come in pairs) */
void jpeg40_dec_ring_nop(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count / 2; i++) {
        vcn40_ring_write(d, r, PACKETJ(0, 0, 0, PACKETJ_TYPE6));
        vcn40_ring_write(d, r, 0);
    }
}

/* amdgpu_ring_commit: pad to the fetch size with the ring's NOP
 * (amdgpu_ring_insert_nop with VCN_ENC_CMD_NO_OP, jpeg_v2_0_dec_ring_nop),
 * then set_wptr */
void vcn40_ring_commit(rdna3_dev *d, rdna3_vcn_ring *r)
{
    uint32_t count = r->align_mask + 1 - ((uint32_t)r->wptr & r->align_mask), i;
    count &= r->align_mask;
    if (r->jpeg) {
        jpeg40_dec_ring_nop(d, r, count);
        jpeg40_dec_ring_set_wptr(d, r);
        return;
    }
    for (i = 0; i < count; i++)
        vcn40_ring_write(d, r, VCN_ENC_CMD_NO_OP);
    vcn40_unified_ring_set_wptr(d, r);
}

/* Ported from vcn_v2_0_enc_ring_emit_ib */
void vcn40_enc_ring_emit_ib(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t vmid, uint64_t va, uint32_t ndw)
{
    vcn40_ring_write(d, r, VCN_ENC_CMD_IB);
    vcn40_ring_write(d, r, vmid);
    vcn40_ring_write(d, r, (uint32_t)va);
    vcn40_ring_write(d, r, (uint32_t)(va >> 32));
    vcn40_ring_write(d, r, ndw);
}

/* Ported from vcn_v2_0_enc_ring_emit_fence (32-bit, always with a trap) */
void vcn40_enc_ring_emit_fence(rdna3_dev *d, rdna3_vcn_ring *r, uint64_t addr, uint32_t seq)
{
    vcn40_ring_write(d, r, VCN_ENC_CMD_FENCE);
    vcn40_ring_write(d, r, (uint32_t)addr);
    vcn40_ring_write(d, r, (uint32_t)(addr >> 32));
    vcn40_ring_write(d, r, seq);
    vcn40_ring_write(d, r, VCN_ENC_CMD_TRAP);
}

/* Ported from vcn_v2_0_enc_ring_insert_end */
void vcn40_enc_ring_insert_end(rdna3_dev *d, rdna3_vcn_ring *r)
{
    vcn40_ring_write(d, r, VCN_ENC_CMD_END);
}

/* Ported from vcn_v2_0_enc_ring_emit_reg_wait */
void vcn40_enc_ring_emit_reg_wait(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val, uint32_t mask)
{
    vcn40_ring_write(d, r, VCN_ENC_CMD_REG_WAIT);
    vcn40_ring_write(d, r, reg << 2);
    vcn40_ring_write(d, r, mask);
    vcn40_ring_write(d, r, val);
}

/* Ported from vcn_v2_0_enc_ring_emit_wreg */
void vcn40_enc_ring_emit_wreg(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val)
{
    vcn40_ring_write(d, r, VCN_ENC_CMD_REG_WRITE);
    vcn40_ring_write(d, r, reg << 2);
    vcn40_ring_write(d, r, val);
}

/* Ported from vcn_v2_0_enc_ring_emit_vm_flush: amdgpu_gmc_emit_flush_gpu_tlb
 * (PTB write, amdgpu_ring_emit_reg_write_reg_wait_helper on the invalidation
 * engine) and the wait for the PTB write. The kext flushes the hubs by MMIO
 * when a VM's mappings change, so its jobs do not emit this. */
void vcn40_enc_ring_emit_vm_flush(rdna3_dev *d, rdna3_vcn_ring *r, const rdna3_vm_flush_regs *f,
                                  uint64_t pd_addr)
{
    vcn40_enc_ring_emit_wreg(d, r, f->pt_lo, (uint32_t)pd_addr);
    vcn40_enc_ring_emit_wreg(d, r, f->pt_hi, (uint32_t)(pd_addr >> 32));
    vcn40_enc_ring_emit_wreg(d, r, f->req, f->req_val);
    vcn40_enc_ring_emit_reg_wait(d, r, f->ack, f->req_val & 0xffff, f->req_val & 0xffff);
    vcn40_enc_ring_emit_reg_wait(d, r, f->pt_lo, (uint32_t)pd_addr, 0xffffffff);
}

int vcn40_fence_passed(rdna3_dev *d, const rdna3_vcn_ring *r, uint32_t seq)
{
    return (int32_t)(wb_read32(d, r, RD_VCN_WB_FENCE) - seq) >= 0;
}

int vcn40_wait_fence(rdna3_dev *d, const rdna3_vcn_ring *r, uint32_t seq)
{
    uint32_t i;
    for (i = 0; i < d->usec_timeout; i++) {
        if (vcn40_fence_passed(d, r, seq))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "%s ring %u: fence %u timeout (last %u)", r->jpeg ? "JPEG" : "VCN", r->me, seq,
         wb_read32(d, r, RD_VCN_WB_FENCE));
    return RDNA3_ETIME;
}

/* Ported from amdgpu_vcn_enc_ring_test_ring */
int vcn40_enc_ring_test_ring(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r)
{
    uint64_t rptr;
    uint32_t i;
    int ret;

    vcn40_ring_begin_use(d, v);
    if (v->cur_state != RD_PG_STATE_UNGATE) {
        vcn40_ring_end_use(d, v);
        return v->last_err ? v->last_err : RDNA3_EIO;
    }
    ret = vcn40_ring_alloc(d, r, 16);
    if (ret) {
        vcn40_ring_end_use(d, v);
        return ret;
    }
    rptr = vcn40_unified_ring_get_rptr(d, r);
    vcn40_ring_write(d, r, VCN_ENC_CMD_END);
    vcn40_ring_commit(d, r);
    vcn40_ring_end_use(d, v);
    for (i = 0; i < d->usec_timeout; i++) {
        if (vcn40_unified_ring_get_rptr(d, r) != rptr)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "VCN%u: ring test timeout (rptr 0x%llx, UVD_STATUS 0x%08x)", r->me, (unsigned long long)rptr,
         RV(d, r->me, regUVD_STATUS));
    return RDNA3_ETIME;
}

/* a kernel IB on the unified ring (amdgpu_job_submit_direct of an IB from
 * the direct pool): VMID 0, ring fence */
static uint32_t submit_direct(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r, uint64_t ib_mc, uint32_t ndw)
{
    uint32_t seq;
    vcn40_ring_begin_use(d, v);
    if (vcn40_ring_alloc(d, r, 5 + 5 + 1)) {
        vcn40_ring_end_use(d, v);
        return 0;
    }
    vcn40_enc_ring_emit_ib(d, r, 0, ib_mc, ndw);
    seq = ++r->fence_seq;
    r->emitted++;
    vcn40_enc_ring_emit_fence(d, r, rdna3_vram_mc(d, r->wb + RD_VCN_WB_FENCE), seq);
    vcn40_enc_ring_insert_end(d, r);
    vcn40_ring_commit(d, r);
    vcn40_ring_end_use(d, v);
    return seq;
}

/* Ported from amdgpu_vcn_unified_ring_ib_header: the single-queue checksum
 * and engine-info packages in front of an IB; returns the checksum slot */
static uint32_t unified_ring_ib_header(uint32_t *ib, uint32_t *len, uint32_t ib_pack_in_dw, int enc)
{
    uint32_t slot;
    ib[(*len)++] = 0x00000010;   /* single queue checksum */
    ib[(*len)++] = 0x30000002;
    slot = (*len)++;
    ib[(*len)++] = ib_pack_in_dw;
    ib[(*len)++] = 0x00000010;   /* engine info */
    ib[(*len)++] = 0x30000001;
    ib[(*len)++] = enc ? 0x2 : 0x3;
    ib[(*len)++] = ib_pack_in_dw * 4;
    return slot;
}

/* Ported from amdgpu_vcn_unified_ring_ib_checksum */
static void unified_ring_ib_checksum(uint32_t *ib, uint32_t slot, uint32_t ib_pack_in_dw)
{
    uint32_t i, checksum = 0;
    for (i = 0; i < ib_pack_in_dw; i++)
        checksum += ib[slot + 2 + i];
    ib[slot] = checksum;
}

/* Ported from amdgpu_vcn_enc_get_create_msg / amdgpu_vcn_enc_get_destroy_msg
 * (with the unified queue headers): session info, task info, op */
static uint32_t enc_session_msg(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r, uint64_t ib_off, uint32_t handle,
                                uint64_t msg_mc, uint32_t op)
{
    uint32_t ib[24], len = 0, slot, i;
    const uint32_t ib_size_dw = 16 + 8;
    slot = unified_ring_ib_header(ib, &len, 0x11, 1);
    ib[len++] = 0x00000018;
    ib[len++] = 0x00000001;   /* session info */
    ib[len++] = handle;
    ib[len++] = (uint32_t)(msg_mc >> 32);
    ib[len++] = (uint32_t)msg_mc;
    ib[len++] = 0x00000000;
    ib[len++] = 0x00000014;
    ib[len++] = 0x00000002;   /* task info */
    ib[len++] = 0x0000001c;
    ib[len++] = 0x00000000;
    ib[len++] = 0x00000000;
    ib[len++] = 0x00000008;
    ib[len++] = op;           /* 0x08000001 op initialize, 0x08000002 op close session */
    for (i = len; i < ib_size_dw; ++i)
        ib[i] = 0;
    unified_ring_ib_checksum(ib, slot, 0x11);
    rdna3_vram_write(d, ib_off, ib, sizeof(ib));
    rdna3_hdp_flush(d);
    return submit_direct(d, v, r, rdna3_vram_mc(d, ib_off), ib_size_dw);
}

/* Ported from amdgpu_vcn_dec_get_create_msg / amdgpu_vcn_dec_get_destroy_msg */
static void dec_msg(rdna3_dev *d, uint64_t msg_off, uint32_t handle, int create)
{
    uint32_t msg[14] = { 0 };
    uint32_t n = create ? 14 : 6;
    rdna3_vram_fill(d, msg_off, 0, 4096);
    msg[0] = 0x00000028;
    msg[1] = create ? 0x00000038 : 0x00000018;
    msg[2] = create ? 0x00000001 : 0x00000000;
    msg[3] = create ? 0x00000000 : 0x00000002;
    msg[4] = handle;
    msg[5] = 0x00000000;
    if (create) {
        msg[6] = 0x00000001;
        msg[7] = 0x00000028;
        msg[8] = 0x00000010;
        msg[9] = 0x00000000;
        msg[10] = 0x00000007;
        msg[11] = 0x00000000;
        msg[12] = 0x00000780;
        msg[13] = 0x00000440;
    }
    rdna3_vram_write(d, msg_off, msg, 4ull * n);
}

/* Ported from amdgpu_vcn_dec_sw_send_msg (unified queue): a decode-buffer
 * package naming the message buffer */
static uint32_t dec_sw_send_msg(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r, uint64_t ib_off, uint64_t msg_mc)
{
    uint32_t ib[72], len = 0, slot, i, ib_pack_in_dw;
    const uint32_t ib_size_dw = 64 + 8;
    ib_pack_in_dw = RD_VCN_DECODE_BUFFER_DW + 4 + 2;   /* engine info + decoding ib in dw */
    slot = unified_ring_ib_header(ib, &len, ib_pack_in_dw, 0);
    ib[len++] = RD_VCN_DECODE_BUFFER_DW * 4 + 8;
    ib[len++] = RD_VCN_IB_FLAG_DECODE_BUFFER;
    for (i = 0; i < RD_VCN_DECODE_BUFFER_DW; i++)
        ib[len + i] = 0;
    ib[len + 0] |= RD_VCN_CMD_FLAG_MSG_BUFFER;   /* valid_buf_flag */
    ib[len + 1] = (uint32_t)(msg_mc >> 32);       /* msg_buffer_address_hi */
    ib[len + 2] = (uint32_t)msg_mc;               /* msg_buffer_address_lo */
    len += RD_VCN_DECODE_BUFFER_DW;
    for (i = len; i < ib_size_dw; ++i)
        ib[i] = 0;
    unified_ring_ib_checksum(ib, slot, ib_pack_in_dw);
    rdna3_vram_write(d, ib_off, ib, sizeof(ib));
    rdna3_hdp_flush(d);
    return submit_direct(d, v, r, rdna3_vram_mc(d, ib_off), ib_size_dw);
}

/* Ported from amdgpu_vcn_unified_ring_test_ib: amdgpu_vcn_enc_ring_test_ib
 * (create + destroy session) and amdgpu_vcn_dec_sw_ring_test_ib (create +
 * destroy message), the last fence waited for */
int vcn40_unified_ring_test_ib(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r)
{
    /* page 0-1: IBs, page 2: the encode session buffer (128 KiB + a page in
     * amdgpu, the firmware only needs it mapped), page 3: decode message */
    uint64_t buf = v->ib_test_buf;
    uint32_t seq;
    int ret;
    if (!buf) {   /* once: kept across GPU resets */
        buf = rdna3_vram_alloc(d, (128u << 10) + 4 * 4096u, 4096, "vcn-ib-test");
        if (buf == UINT64_MAX)
            return RDNA3_ENOMEM;
        v->ib_test_buf = buf;
    }
    rdna3_vram_fill(d, buf, 0, 4 * 4096u);
    /* amdgpu_vcn_enc_ring_test_ib */
    seq = enc_session_msg(d, v, r, buf, 1, rdna3_vram_mc(d, buf + 4 * 4096u), 0x08000001);
    if (seq)
        seq = enc_session_msg(d, v, r, buf + 128, 1, rdna3_vram_mc(d, buf + 4 * 4096u), 0x08000002);
    ret = seq ? vcn40_wait_fence(d, r, seq) : RDNA3_EIO;
    if (ret) {
        RERR(d, "VCN%u: encode IB test failed (%d)", r->me, ret);
        return ret;
    }
    /* amdgpu_vcn_dec_sw_ring_test_ib */
    dec_msg(d, buf + 3 * 4096u, 1, 1);
    seq = dec_sw_send_msg(d, v, r, buf + 4096, rdna3_vram_mc(d, buf + 3 * 4096u));
    if (seq) {
        ret = vcn40_wait_fence(d, r, seq);
        if (!ret) {
            dec_msg(d, buf + 3 * 4096u, 1, 0);
            seq = dec_sw_send_msg(d, v, r, buf + 4096 + 512, rdna3_vram_mc(d, buf + 3 * 4096u));
            ret = seq ? vcn40_wait_fence(d, r, seq) : RDNA3_EIO;
        }
    } else {
        ret = RDNA3_EIO;
    }
    if (ret)
        RERR(d, "VCN%u: decode IB test failed (%d)", r->me, ret);
    return ret;
}

/* Ported from vcn_v4_0_hw_init (bare metal): the doorbell range and the
 * ring test (amdgpu_ring_test_helper) of every instance */
int vcn40_hw_init(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t i;
    int r;
    for (i = 0; i < v->num_vcn_inst; ++i) {
        rdna3_vcn_ring *ring = &v->ring_enc[i];
        if (v->harvest_config & (1u << i))
            continue;
        nbio43_vcn_doorbell_range(d, ring->use_doorbell, (RD_DB_VCN0_1 << 1) + 8 * i, i);
        r = vcn40_enc_ring_test_ring(d, v, ring);
        ring->ready = r == RDNA3_OK;
        if (r) {
            RERR(d, "VCN%u: ring test failed (%d)", i, r);
            return r;
        }
        RLOG(d, "VCN%u: unified ring at MC 0x%llx, doorbell 0x%x%s, ring test passed", i,
             (unsigned long long)ring->gpu_addr, ring->doorbell_index, ring->use_doorbell ? "" : " (unused)");
    }
    return RDNA3_OK;
}

/* Ported from vcn_v4_0_hw_fini (bare metal) */
int vcn40_hw_fini(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t i;
    for (i = 0; i < v->num_vcn_inst; ++i) {
        if (v->harvest_config & (1u << i))
            continue;
        v->ring_enc[i].ready = 0;
        if (v->cur_state != RD_PG_STATE_GATE && RV(d, i, regUVD_STATUS))
            return vcn40_set_powergating_state(d, v, RD_PG_STATE_GATE);
    }
    return RDNA3_OK;
}

uint32_t vcn40_submit(rdna3_dev *d, rdna3_vcn *v, uint32_t inst, uint32_t vmid, const rdna3_vcn_ib *ibs,
                      uint32_t n, uint64_t uf_mc)
{
    rdna3_vcn_ring *r;
    uint32_t i, seq;
    if (inst >= v->num_vcn_inst || !v->ring_enc[inst].ready || !n)
        return 0;
    r = &v->ring_enc[inst];
    vcn40_ring_begin_use(d, v);   /* amdgpu_ring_alloc -> begin_use */
    if (v->cur_state != RD_PG_STATE_UNGATE || vcn40_ring_alloc(d, r, 5 * n + 5 + 5 + 1)) {
        vcn40_ring_end_use(d, v);
        return 0;
    }
    /* amdgpu_ib_schedule: the IBs, the user fence, the ring fence, END */
    for (i = 0; i < n; i++)
        vcn40_enc_ring_emit_ib(d, r, vmid, ibs[i].va, ibs[i].length_dw);
    seq = ++r->fence_seq;
    if (uf_mc)
        vcn40_enc_ring_emit_fence(d, r, uf_mc, seq);   /* 32-bit: the ring has no 64-bit fence */
    r->emitted++;
    vcn40_enc_ring_emit_fence(d, r, rdna3_vram_mc(d, r->wb + RD_VCN_WB_FENCE), seq);
    vcn40_enc_ring_insert_end(d, r);
    vcn40_ring_commit(d, r);
    vcn40_ring_end_use(d, v);
    return seq;
}

/* Ported from vcn_v4_0_process_interrupt (the fences are polled: the OS
 * wakes its fence waiters after every interrupt) */
int vcn40_process_interrupt(rdna3_vcn *v, uint32_t client_id, uint32_t src_id, uint32_t *inst)
{
    uint32_t ip_instance;
    switch (client_id) {
    case RD_IH_CLIENTID_VCN:
        ip_instance = 0;
        break;
    case RD_IH_CLIENTID_VCN1:
        ip_instance = 1;
        break;
    default:
        return 0;
    }
    switch (src_id) {
    case VCN_4_0__SRCID__UVD_ENC_GENERAL_PURPOSE:
        v->irqs++;
        if (inst)
            *inst = ip_instance;
        return 1;
    default:
        if (client_id == RD_IH_CLIENTID_VCN && src_id == VCN_4_0__SRCID__JPEG_DECODE)
            return 0;   /* jpeg_v4_0_process_interrupt's */
        v->bad_irqs++;
        return 0;
    }
}

/* ============================== JPEG 4.0 ============================== */

/* Ported from jpeg_v4_0_early_init: one instance, one ring */
int jpeg40_early_init(rdna3_dev *d, rdna3_vcn *v)
{
    if (!d->ipver[HWIP_VCN].present || d->ipver[HWIP_VCN].major != 4)
        return RDNA3_ENODEV;
    v->jpeg_present = 1;
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_sw_init and amdgpu_jpeg_sw_init (no JPEG DPG):
 * the ring and the pool the checked IB copies run from */
int jpeg40_sw_init(rdna3_dev *d, rdna3_vcn *v)
{
    rdna3_vcn_ring *r = &v->jpeg;
    int ret;
    if (!v->jpeg_present)
        return RDNA3_ENODEV;
    r->me = 0;
    r->jpeg = 1;
    r->use_doorbell = d->doorbell_ok;
    r->doorbell_index = (RD_DB_VCN0_1 << 1) + 1;
    ret = ring_init(d, r, RD_JPEG_RING_SIZE, 0xf, "jpeg-dec");
    if (ret)
        return ret;
    if (!v->jpeg_ib_pool || v->jpeg_ib_pool == UINT64_MAX) {
        v->jpeg_ib_pool = rdna3_vram_alloc(d, (uint64_t)RD_JPEG_IB_SLOTS * RD_JPEG_IB_SLOT_SIZE, 4096, "jpeg-ib");
        if (v->jpeg_ib_pool == UINT64_MAX)
            return RDNA3_ENOMEM;
    }
    {
        uint32_t i;
        for (i = 0; i < RD_JPEG_IB_SLOTS; i++)
            v->jpeg_ib_seq[i] = r->fence_seq;
    }
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_disable_clock_gating */
static void jpeg40_disable_clock_gating(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t data = RJ(d, regJPEG_CGC_CTRL);
    if (v->cg_flags & RD_CG_JPEG_MGCG) {
        data |= 1u << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
        data &= ~(uint32_t)JPEG_CGC_CTRL__JPEG_DEC_MODE_MASK;
    } else {
        data &= ~(uint32_t)JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
    }
    data |= 1u << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
    data |= 4u << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
    WJ(d, regJPEG_CGC_CTRL, data);

    data = RJ(d, regJPEG_CGC_GATE);
    data &= ~(uint32_t)(JPEG_CGC_GATE__JPEG_DEC_MASK | JPEG_CGC_GATE__JPEG2_DEC_MASK | JPEG_CGC_GATE__JMCIF_MASK |
                        JPEG_CGC_GATE__JRBBM_MASK);
    WJ(d, regJPEG_CGC_GATE, data);
}

/* Ported from jpeg_v4_0_enable_clock_gating */
static void jpeg40_enable_clock_gating(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t data = RJ(d, regJPEG_CGC_CTRL);
    if (v->cg_flags & RD_CG_JPEG_MGCG) {
        data |= 1u << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
        data |= JPEG_CGC_CTRL__JPEG_DEC_MODE_MASK;
    } else {
        data &= ~(uint32_t)JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
    }
    data |= 1u << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
    data |= 4u << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
    WJ(d, regJPEG_CGC_CTRL, data);

    data = RJ(d, regJPEG_CGC_GATE);
    data |= (uint32_t)(JPEG_CGC_GATE__JPEG_DEC_MASK | JPEG_CGC_GATE__JPEG2_DEC_MASK | JPEG_CGC_GATE__JMCIF_MASK |
                       JPEG_CGC_GATE__JRBBM_MASK);
    WJ(d, regJPEG_CGC_GATE, data);
}

/* Ported from jpeg_v4_0_disable_static_power_gating */
static int jpeg40_disable_static_power_gating(rdna3_dev *d, rdna3_vcn *v)
{
    if (v->pg_flags & RD_PG_JPEG) {
        uint32_t data = 1u << UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG__SHIFT;
        int r;
        WJ(d, regUVD_PGFSM_CONFIG, data);
        r = WAIT_V(d, 0, regUVD_PGFSM_STATUS, RD_UVD_PGFSM_STATUS_UVDJ_PWR_ON,
                   UVD_PGFSM_STATUS__UVDJ_PWR_STATUS_MASK);
        if (r) {
            RERR(d, "JPEG: disable power gating failed");
            return r;
        }
    }
    /* disable anti hang mechanism */
    WV_P(d, 0, regUVD_JPEG_POWER_STATUS, 0, ~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);
    /* keep the JPEG in static PG mode */
    WV_P(d, 0, regUVD_JPEG_POWER_STATUS, 0, ~UVD_JPEG_POWER_STATUS__JPEG_PG_MODE_MASK);
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_enable_static_power_gating */
static int jpeg40_enable_static_power_gating(rdna3_dev *d, rdna3_vcn *v)
{
    /* enable anti hang mechanism */
    WV_P(d, 0, regUVD_JPEG_POWER_STATUS, UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK,
         ~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);
    if (v->pg_flags & RD_PG_JPEG) {
        uint32_t data = 2u << UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG__SHIFT;
        int r;
        WJ(d, regUVD_PGFSM_CONFIG, data);
        r = WAIT_V(d, 0, regUVD_PGFSM_STATUS, 2u << UVD_PGFSM_STATUS__UVDJ_PWR_STATUS__SHIFT,
                   UVD_PGFSM_STATUS__UVDJ_PWR_STATUS_MASK);
        if (r) {
            RERR(d, "JPEG: enable power gating failed");
            return r;
        }
    }
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_start */
static int jpeg40_start(rdna3_dev *d, rdna3_vcn *v)
{
    rdna3_vcn_ring *ring = &v->jpeg;
    int r;

    if (v->dpm_enabled) {   /* amdgpu_dpm_enable_jpeg(adev, true) */
        r = jpeg40_dpm_set_jpeg_enable(d, v, 1);
        if (r)
            RERR(d, "JPEG: PowerUpJpeg failed (%d)", r);
    }
    /* disable power gating */
    r = jpeg40_disable_static_power_gating(d, v);
    if (r)
        return r;
    /* JPEG disable CGC */
    jpeg40_disable_clock_gating(d, v);
    /* MJPEG global tiling registers */
    WJ(d, regJPEG_DEC_GFX10_ADDR_CONFIG, v->gb_addr_config);
    /* enable JMI channel */
    WV_P(d, 0, regUVD_JMI_CNTL, 0, ~UVD_JMI_CNTL__SOFT_RESET_MASK);
    /* enable System Interrupt for JRBC */
    WV_P(d, 0, regJPEG_SYS_INT_EN, JPEG_SYS_INT_EN__DJRBC_MASK, ~JPEG_SYS_INT_EN__DJRBC_MASK);

    WJ(d, regUVD_LMI_JRBC_RB_VMID, 0);
    WJ(d, regUVD_JRBC_RB_CNTL, (0x00000001u | 0x00000002u));
    WJ(d, regUVD_LMI_JRBC_RB_64BIT_BAR_LOW, (uint32_t)ring->gpu_addr);
    WJ(d, regUVD_LMI_JRBC_RB_64BIT_BAR_HIGH, (uint32_t)(ring->gpu_addr >> 32));
    WJ(d, regUVD_JRBC_RB_RPTR, 0);
    WJ(d, regUVD_JRBC_RB_WPTR, 0);
    WJ(d, regUVD_JRBC_RB_CNTL, 0x00000002u);
    WJ(d, regUVD_JRBC_RB_SIZE, ring->ring_size / 4);
    ring->wptr = RJ(d, regUVD_JRBC_RB_WPTR);
    {
        uint32_t w = (uint32_t)ring->wptr;
        rdna3_vram_write(d, ring->wb + RD_VCN_WB_WPTR, &w, 4);
    }
    v->jpeg_starts++;
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_stop */
static int jpeg40_stop(rdna3_dev *d, rdna3_vcn *v)
{
    int r;
    /* reset JMI */
    WV_P(d, 0, regUVD_JMI_CNTL, UVD_JMI_CNTL__SOFT_RESET_MASK, ~UVD_JMI_CNTL__SOFT_RESET_MASK);
    jpeg40_enable_clock_gating(d, v);
    /* enable power gating */
    r = jpeg40_enable_static_power_gating(d, v);
    if (r)
        return r;
    if (v->dpm_enabled) {   /* amdgpu_dpm_enable_jpeg(adev, false) */
        r = jpeg40_dpm_set_jpeg_enable(d, v, 0);
        if (r)
            RERR(d, "JPEG: PowerDownJpeg failed (%d)", r);
    }
    v->jpeg_stops++;
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_is_idle */
int jpeg40_is_idle(rdna3_dev *d, rdna3_vcn *v)
{
    (void)v;
    return (RJ(d, regUVD_JRBC_STATUS) & UVD_JRBC_STATUS__RB_JOB_DONE_MASK) == UVD_JRBC_STATUS__RB_JOB_DONE_MASK;
}

/* Ported from jpeg_v4_0_wait_for_idle */
int jpeg40_wait_for_idle(rdna3_dev *d, rdna3_vcn *v)
{
    (void)v;
    return WAIT_V(d, 0, regUVD_JRBC_STATUS, UVD_JRBC_STATUS__RB_JOB_DONE_MASK, UVD_JRBC_STATUS__RB_JOB_DONE_MASK);
}

/* Ported from jpeg_v4_0_set_clockgating_state */
int jpeg40_set_clockgating_state(rdna3_dev *d, rdna3_vcn *v, int gate)
{
    if (gate) {
        if (!jpeg40_is_idle(d, v))
            return RDNA3_EBUSY;
        jpeg40_enable_clock_gating(d, v);
    } else {
        jpeg40_disable_clock_gating(d, v);
    }
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_set_powergating_state */
int jpeg40_set_powergating_state(rdna3_dev *d, rdna3_vcn *v, int state)
{
    int ret;
    if (state == v->jpeg_cur_state)
        return RDNA3_OK;
    if (state == RD_PG_STATE_GATE)
        ret = jpeg40_stop(d, v);
    else
        ret = jpeg40_start(d, v);
    if (!ret)
        v->jpeg_cur_state = state;
    return ret;
}

/* Ported from amdgpu_jpeg_ring_begin_use */
void jpeg40_ring_begin_use(rdna3_dev *d, rdna3_vcn *v)
{
    int r;
    v->jpeg_total_submission_cnt++;
    r = jpeg40_set_powergating_state(d, v, RD_PG_STATE_UNGATE);
    if (r)
        v->last_err = r;
}

/* Ported from amdgpu_jpeg_ring_end_use */
void jpeg40_ring_end_use(rdna3_dev *d, rdna3_vcn *v)
{
    (void)d;
    if (v->jpeg_total_submission_cnt)
        v->jpeg_total_submission_cnt--;
}

/* Ported from amdgpu_jpeg_idle_work_handler */
int jpeg40_idle_work_handler(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t fences = v->jpeg_present ? fence_count_emitted(d, &v->jpeg) : 0;
    if (!fences && !v->jpeg_total_submission_cnt) {
        int r = jpeg40_set_powergating_state(d, v, RD_PG_STATE_GATE);
        if (r)
            v->last_err = r;
        return 1;
    }
    return 0;
}

/* Ported from jpeg_v2_0_dec_ring_insert_start */
void jpeg40_dec_ring_insert_start(rdna3_dev *d, rdna3_vcn_ring *r)
{
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x68e04);
    vcn40_ring_write(d, r, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x80010000);
}

/* Ported from jpeg_v2_0_dec_ring_insert_end */
void jpeg40_dec_ring_insert_end(rdna3_dev *d, rdna3_vcn_ring *r)
{
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x68e04);
    vcn40_ring_write(d, r, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x00010000);
}

/* Ported from jpeg_v2_0_dec_ring_emit_fence (32-bit, with the trap) */
void jpeg40_dec_ring_emit_fence(rdna3_dev *d, rdna3_vcn_ring *r, uint64_t addr, uint32_t seq)
{
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JPEG_GPCOM_DATA0_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, seq);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JPEG_GPCOM_DATA1_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, seq);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_LOW_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, (uint32_t)addr);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_HIGH_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, (uint32_t)(addr >> 32));
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JPEG_GPCOM_CMD_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x8);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JPEG_GPCOM_CMD_INTERNAL_OFFSET, 0, PACKETJ_CONDITION_CHECK0, PACKETJ_TYPE4));
    vcn40_ring_write(d, r, 0);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x3fbc);
    vcn40_ring_write(d, r, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x1);
    vcn40_ring_write(d, r, PACKETJ(0, 0, 0, PACKETJ_TYPE7));
    vcn40_ring_write(d, r, 0);
}

/* Ported from jpeg_v2_0_dec_ring_emit_ib; the ring has parse_cs, so the
 * JRBC fetches the IB (the kernel's checked copy) in VMID ib_vmid (0) */
void jpeg40_dec_ring_emit_ib(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t vmid, uint32_t ib_vmid,
                             uint64_t addr, uint32_t ndw)
{
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JPEG_IH_CTRL_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, vmid << JPEG_IH_CTRL__IH_VMID__SHIFT);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_IB_VMID_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, ib_vmid | (ib_vmid << 4) | (ib_vmid << 8));
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JPEG_VMID_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, vmid | (vmid << 4) | (vmid << 8));
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_IB_64BIT_BAR_LOW_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, (uint32_t)addr);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_IB_64BIT_BAR_HIGH_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, (uint32_t)(addr >> 32));
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_IB_SIZE_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, ndw);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_RB_MEM_RD_64BIT_BAR_LOW_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, (uint32_t)r->gpu_addr);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_LMI_JRBC_RB_MEM_RD_64BIT_BAR_HIGH_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, (uint32_t)(r->gpu_addr >> 32));
    vcn40_ring_write(d, r, PACKETJ(0, 0, PACKETJ_CONDITION_CHECK0, PACKETJ_TYPE2));
    vcn40_ring_write(d, r, 0);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_RB_COND_RD_TIMER_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x01400200);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_RB_REF_DATA_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x2);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_STATUS_INTERNAL_OFFSET, 0, PACKETJ_CONDITION_CHECK3, PACKETJ_TYPE3));
    vcn40_ring_write(d, r, 0x2);
}

/* Ported from jpeg_v2_0_dec_ring_emit_reg_wait */
void jpeg40_dec_ring_emit_reg_wait(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val, uint32_t mask)
{
    uint32_t reg_offset = reg << 2;
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_RB_COND_RD_TIMER_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, 0x01400200);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_RB_REF_DATA_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    vcn40_ring_write(d, r, val);
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    if (reg_offset >= 0x10000 && reg_offset <= 0x105ff) {
        vcn40_ring_write(d, r, 0);
        vcn40_ring_write(d, r, PACKETJ((reg_offset >> 2), 0, 0, PACKETJ_TYPE3));
    } else {
        vcn40_ring_write(d, r, reg_offset);
        vcn40_ring_write(d, r, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR, 0, 0, PACKETJ_TYPE3));
    }
    vcn40_ring_write(d, r, mask);
}

/* Ported from jpeg_v2_0_dec_ring_emit_wreg */
void jpeg40_dec_ring_emit_wreg(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val)
{
    uint32_t reg_offset = reg << 2;
    vcn40_ring_write(d, r, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0));
    if (reg_offset >= 0x10000 && reg_offset <= 0x105ff) {
        vcn40_ring_write(d, r, 0);
        vcn40_ring_write(d, r, PACKETJ((reg_offset >> 2), 0, 0, PACKETJ_TYPE0));
    } else {
        vcn40_ring_write(d, r, reg_offset);
        vcn40_ring_write(d, r, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR, 0, 0, PACKETJ_TYPE0));
    }
    vcn40_ring_write(d, r, val);
}

/* Ported from jpeg_v2_0_dec_ring_emit_vm_flush (see vcn40_enc_ring_emit_vm_flush) */
void jpeg40_dec_ring_emit_vm_flush(rdna3_dev *d, rdna3_vcn_ring *r, const rdna3_vm_flush_regs *f,
                                   uint64_t pd_addr)
{
    jpeg40_dec_ring_emit_wreg(d, r, f->pt_lo, (uint32_t)pd_addr);
    jpeg40_dec_ring_emit_wreg(d, r, f->pt_hi, (uint32_t)(pd_addr >> 32));
    jpeg40_dec_ring_emit_wreg(d, r, f->req, f->req_val);
    jpeg40_dec_ring_emit_reg_wait(d, r, f->ack, f->req_val & 0xffff, f->req_val & 0xffff);
    jpeg40_dec_ring_emit_reg_wait(d, r, f->pt_lo, (uint32_t)pd_addr, 0xffffffff);
}

/* Ported from amdgpu_jpeg_dec_ring_test_ring (jpeg_pitch: internal
 * regUVD_JPEG_PITCH_INTERNAL_OFFSET, external regUVD_JPEG_PITCH) */
int jpeg40_dec_ring_test_ring(rdna3_dev *d, rdna3_vcn *v)
{
    rdna3_vcn_ring *r = &v->jpeg;
    uint32_t tmp = 0, i;
    int ret;

    jpeg40_ring_begin_use(d, v);
    if (v->jpeg_cur_state != RD_PG_STATE_UNGATE) {
        jpeg40_ring_end_use(d, v);
        return v->last_err ? v->last_err : RDNA3_EIO;
    }
    ret = vcn40_ring_alloc(d, r, 3);
    if (ret) {
        jpeg40_ring_end_use(d, v);
        return ret;
    }
    WJ(d, regUVD_JPEG_PITCH, 0xCAFEDEAD);
    /* Add a read register to make sure the write register is executed. */
    (void)RJ(d, regUVD_JPEG_PITCH);
    vcn40_ring_write(d, r, RD_PACKET0(regUVD_JPEG_PITCH_INTERNAL_OFFSET, 0));
    vcn40_ring_write(d, r, 0xABADCAFE);
    vcn40_ring_commit(d, r);
    jpeg40_ring_end_use(d, v);
    for (i = 0; i < d->usec_timeout; i++) {
        tmp = RJ(d, regUVD_JPEG_PITCH);
        if (tmp == 0xABADCAFE)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "JPEG: ring test timeout (pitch 0x%08x, JRBC_STATUS 0x%08x)", tmp, RJ(d, regUVD_JRBC_STATUS));
    return RDNA3_ETIME;
}

/* a pool slot for an IB copy: the oldest one, once its job is done */
static int jpeg_ib_slot(rdna3_dev *d, rdna3_vcn *v, uint32_t *slot)
{
    uint32_t s = v->jpeg_ib_next % RD_JPEG_IB_SLOTS;
    int r = vcn40_wait_fence(d, &v->jpeg, v->jpeg_ib_seq[s]);
    if (r)
        return r;
    v->jpeg_ib_next = s + 1;
    *slot = s;
    return RDNA3_OK;
}

uint32_t jpeg40_submit(rdna3_dev *d, rdna3_vcn *v, uint32_t vmid, const rdna3_vcn_ib *ibs, uint32_t n,
                       uint64_t uf_mc)
{
    rdna3_vcn_ring *r = &v->jpeg;
    uint32_t i, seq, slots[8];
    if (!r->ready || !n || n > 8)
        return 0;
    for (i = 0; i < n; i++)
        if (!ibs[i].kdata || ibs[i].length_dw * 4ull > RD_JPEG_IB_SLOT_SIZE)
            return 0;
    jpeg40_ring_begin_use(d, v);
    if (v->jpeg_cur_state != RD_PG_STATE_UNGATE || vcn40_ring_alloc(d, r, 4 + 22 * n + 18 + 18 + 4)) {
        jpeg40_ring_end_use(d, v);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (jpeg_ib_slot(d, v, &slots[i])) {
            jpeg40_ring_end_use(d, v);
            return 0;
        }
        rdna3_vram_write(d, v->jpeg_ib_pool + (uint64_t)slots[i] * RD_JPEG_IB_SLOT_SIZE, ibs[i].kdata,
                         ibs[i].length_dw * 4ull);
    }
    rdna3_hdp_flush(d);
    /* amdgpu_ib_schedule: insert_start, the IBs, the user fence, the ring
     * fence, insert_end */
    jpeg40_dec_ring_insert_start(d, r);
    for (i = 0; i < n; i++)
        jpeg40_dec_ring_emit_ib(d, r, vmid, 0,
                                rdna3_vram_mc(d, v->jpeg_ib_pool + (uint64_t)slots[i] * RD_JPEG_IB_SLOT_SIZE),
                                ibs[i].length_dw);
    seq = ++r->fence_seq;
    if (uf_mc)
        jpeg40_dec_ring_emit_fence(d, r, uf_mc, seq);
    r->emitted++;
    jpeg40_dec_ring_emit_fence(d, r, rdna3_vram_mc(d, r->wb + RD_VCN_WB_FENCE), seq);
    jpeg40_dec_ring_insert_end(d, r);
    vcn40_ring_commit(d, r);
    jpeg40_ring_end_use(d, v);
    for (i = 0; i < n; i++)
        v->jpeg_ib_seq[slots[i]] = seq;
    return seq;
}

/* Ported from amdgpu_jpeg_dec_ring_test_ib with amdgpu_jpeg_dec_set_reg:
 * a 16-dword kernel IB writes the pitch register */
int jpeg40_dec_ring_test_ib(rdna3_dev *d, rdna3_vcn *v)
{
    uint32_t ib[16], i, seq, tmp = 0;
    rdna3_vcn_ib one;
    int r;

    ib[0] = PACKETJ(regUVD_JPEG_PITCH_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0);
    ib[1] = 0xDEADBEEF;
    for (i = 2; i < 16; i += 2) {
        ib[i] = PACKETJ(0, 0, 0, PACKETJ_TYPE6);
        ib[i + 1] = 0;
    }
    one.va = 0;
    one.length_dw = 16;
    one.kdata = ib;
    seq = jpeg40_submit(d, v, 0, &one, 1, 0);
    if (!seq)
        return RDNA3_EIO;
    r = vcn40_wait_fence(d, &v->jpeg, seq);
    if (r)
        return r;
    for (i = 0; i < d->usec_timeout; i++) {
        tmp = RJ(d, regUVD_JPEG_PITCH);
        if (tmp == 0xDEADBEEF)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "JPEG: IB test: pitch 0x%08x", tmp);
    return RDNA3_ETIME;
}

/* Ported from jpeg_v4_0_hw_init (bare metal): the doorbell range of
 * instance 0, VCN_JPEG_DB_CTRL, the ring test */
int jpeg40_hw_init(rdna3_dev *d, rdna3_vcn *v)
{
    rdna3_vcn_ring *r = &v->jpeg;
    int ret;
    if (!v->jpeg_present)
        return RDNA3_ENODEV;
    nbio43_vcn_doorbell_range(d, r->use_doorbell, RD_DB_VCN0_1 << 1, 0);
    WV(d, 0, regVCN_JPEG_DB_CTRL, r->doorbell_index << VCN_JPEG_DB_CTRL__OFFSET__SHIFT | VCN_JPEG_DB_CTRL__EN_MASK);
    ret = jpeg40_dec_ring_test_ring(d, v);
    r->ready = ret == RDNA3_OK;
    if (ret) {
        RERR(d, "JPEG: ring test failed (%d)", ret);
        return ret;
    }
    RLOG(d, "JPEG: decode ring at MC 0x%llx, doorbell 0x%x%s, ring test passed", (unsigned long long)r->gpu_addr,
         r->doorbell_index, r->use_doorbell ? "" : " (unused)");
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_hw_fini (bare metal) */
int jpeg40_hw_fini(rdna3_dev *d, rdna3_vcn *v)
{
    v->jpeg.ready = 0;
    if (v->jpeg_cur_state != RD_PG_STATE_GATE && RJ(d, regUVD_JRBC_STATUS))
        return jpeg40_set_powergating_state(d, v, RD_PG_STATE_GATE);
    return RDNA3_OK;
}

/* Ported from jpeg_v4_0_process_interrupt */
int jpeg40_process_interrupt(rdna3_vcn *v, uint32_t client_id, uint32_t src_id)
{
    if (client_id != RD_IH_CLIENTID_VCN)
        return 0;
    switch (src_id) {
    case VCN_4_0__SRCID__JPEG_DECODE:
        v->jpeg_irqs++;
        return 1;
    default:
        return 0;
    }
}
