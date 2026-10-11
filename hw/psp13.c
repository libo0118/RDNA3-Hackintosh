/*
 * psp13.c - see psp13.h. Function names follow amdgpu so the two can be
 * compared side by side.
 *
 * SPDX-License-Identifier: MIT
 */
#include "psp13.h"
#include "navi3x_regs.h"
#include "psp_gfx_if.h"

/* amdgpu_psp.h */
#define MBOX_TOS_READY_FLAG (GFX_FLAG_RESPONSE)
#define MBOX_TOS_READY_MASK (GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK)
#define MBOX_TOS_RESP_FLAG  (GFX_FLAG_RESPONSE)
#define MBOX_TOS_RESP_MASK  (GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK)
#define PSP_RING_TYPE__KM   2u

#define C2P(psp, n) SOC15_OFF((psp)->d, MP0, 0, regMP0_SMN_C2PMSG_##n)

static void mdelay(rdna3_dev *d, uint32_t ms) { d->os.msleep(d->os.ctx, ms); }

static void bzero_local(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--)
        *q++ = 0;
}

int psp13_init(rdna3_psp *psp, rdna3_dev *d, const uint8_t *sos_fw, size_t sos_len)
{
    amd_fw_status st;

    bzero_local(psp, sizeof(*psp));
    psp->d = d;
    psp->sos_fw = sos_fw;
    psp->sos_len = sos_len;

    st = amd_psp_sos_parse(sos_fw, sos_len, &psp->sos);
    if (st != AMD_FW_OK) {
        RERR(d, "PSP: SOS firmware: %s", amd_fw_status_str(st));
        return RDNA3_EINVAL;
    }

    /* psp_sw_init(): fw_pri 1 MiB aligned to 1 MiB, fence + cmd 4 KiB;
     * psp_ring_init(): 4 KiB KM ring */
    psp->fw_pri = rdna3_vram_alloc(d, PSP_1_MEG, PSP_1_MEG, "psp-fwpri");
    psp->fence  = rdna3_vram_alloc(d, PSP_FENCE_BUFFER_SIZE, 4096, "psp-fence");
    psp->cmd    = rdna3_vram_alloc(d, PSP_CMD_BUFFER_SIZE, 4096, "psp-cmd");
    psp->ring   = rdna3_vram_alloc(d, PSP_RING_SIZE, 4096, "psp-ring");
    if (psp->fw_pri == UINT64_MAX || psp->fence == UINT64_MAX ||
        psp->cmd == UINT64_MAX || psp->ring == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, psp->fence, 0, PSP_FENCE_BUFFER_SIZE);
    rdna3_vram_fill(d, psp->ring, 0, PSP_RING_SIZE);
    rdna3_hdp_flush(d);
    psp->tmr = UINT64_MAX;
    psp->fw_buf = UINT64_MAX;
    return RDNA3_OK;
}

/* ------------------------- bootloader ------------------------- */

/* psp_v13_0_is_sos_alive() */
int psp13_is_sos_alive(rdna3_psp *psp)
{
    return rdna3_rreg(psp->d, C2P(psp, 81)) != 0;
}

int psp13_wait_for_bootloader(rdna3_psp *psp)
{
    int retry, ret = RDNA3_ETIME;
    /* 10 retries for 13.0.7 / 13.0.10 (psp_v13_0_wait_for_bootloader) */
    for (retry = 0; retry < 10; retry++) {
        ret = rdna3_wait_reg(psp->d, C2P(psp, 35), 0x80000000u, 0xffffffffu, 0, 0);
        if (ret == RDNA3_OK)
            return RDNA3_OK;
    }
    return ret;
}

int psp13_bootloader_stuck(rdna3_psp *psp)
{
    return !psp13_is_sos_alive(psp) &&
           !(rdna3_rreg(psp->d, C2P(psp, 35)) & 0x80000000u);
}

int psp13_scpm_enabled(rdna3_dev *d, uint32_t *status)
{
    /* struct psp_runtime_data_header { u16 cookie, version; } and the
     * directory { u16 entry_count; entry_list[] } right after it, entries
     * { u32 entry_type; u16 offset, size; } 4-byte aligned */
    uint64_t hdr_pos, dir_pos;
    uint32_t w, count, i, ent[2];
    *status = 0;
    if (d->vram_size <= RD_PSP_RUNTIME_DB_OFFSET)
        return 0;
    hdr_pos = d->vram_size - RD_PSP_RUNTIME_DB_OFFSET;
    dir_pos = hdr_pos + 4;
    rdna3_vram_mm_read(d, hdr_pos, &w, 1);
    if ((w & 0xffff) != RD_PSP_RUNTIME_DB_COOKIE_ID) {
        RLOG(d, "PSP: no runtime database (cookie 0x%04x): SCPM off", w & 0xffff);
        return 0;
    }
    rdna3_vram_mm_read(d, dir_pos, &w, 1);
    count = w & 0xffff;
    if (count >= RD_PSP_RUNTIME_DB_MAX) {
        RERR(d, "PSP: invalid runtime database entry count %u", count);
        return 0;
    }
    for (i = 0; i < count; i++) {
        rdna3_vram_mm_read(d, dir_pos + 4 + 8ull * i, ent, 2);
        if (ent[0] != RD_PSP_RUNTIME_ENTRY_SCPM)
            continue;
        if ((ent[1] >> 16) < 4) {
            RERR(d, "PSP: invalid runtime database scpm entry size");
            return 0;
        }
        rdna3_vram_mm_read(d, hdr_pos + (ent[1] & 0xffff), status, 1);
        /* psp_sw_init: anything but SCPM_DISABLE (0) counts as enabled */
        RLOG(d, "PSP: runtime database: SCPM status 0x%x", *status);
        return *status != 0;
    }
    return 0;
}

void psp13_log_state(rdna3_psp *psp, const char *when)
{
    RLOG(psp->d, "PSP state (%s): C2PMSG_33 0x%08x 35 0x%08x 36 0x%08x 58 0x%08x 64 0x%08x 81 0x%08x",
         when, rdna3_rreg(psp->d, C2P(psp, 33)), rdna3_rreg(psp->d, C2P(psp, 35)),
         rdna3_rreg(psp->d, C2P(psp, 36)), rdna3_rreg(psp->d, C2P(psp, 58)),
         rdna3_rreg(psp->d, C2P(psp, 64)), rdna3_rreg(psp->d, C2P(psp, 81)));
}

/* psp_copy_fw() */
static int psp13_copy_fw(rdna3_psp *psp, uint8_t bin)
{
    const amd_psp_bin *b = &psp->sos.bin[bin];
    if (!b->size || b->size > PSP_1_MEG) {
        RERR(psp->d, "PSP: firmware piece %u invalid (size %u)", bin, b->size);
        return RDNA3_EINVAL;
    }
    rdna3_vram_fill(psp->d, psp->fw_pri, 0, PSP_1_MEG);
    rdna3_vram_write(psp->d, psp->fw_pri, psp->sos_fw + b->offset, b->size);
    rdna3_hdp_flush(psp->d);
    return RDNA3_OK;
}

static uint32_t fw_pri_mc_mb(rdna3_psp *psp)
{
    return (uint32_t)(rdna3_vram_mc(psp->d, psp->fw_pri) >> 20);
}

/* psp_v13_0_bootloader_load_component() */
static int psp13_bl_load_component(rdna3_psp *psp, const amd_psp_bl_step *s)
{
    int ret;
    if (psp13_is_sos_alive(psp))
        return RDNA3_OK;
    ret = psp13_wait_for_bootloader(psp);
    if (ret)
        return ret;
    ret = psp13_copy_fw(psp, s->src);
    if (ret)
        return ret;
    rdna3_wreg(psp->d, C2P(psp, 36), fw_pri_mc_mb(psp));
    rdna3_wreg(psp->d, C2P(psp, 35), s->bl_cmd);
    return psp13_wait_for_bootloader(psp);
}

/* psp_v13_0_bootloader_load_sos() */
static int psp13_bl_load_sos(rdna3_psp *psp, const amd_psp_bl_step *s)
{
    int ret;
    if (psp13_is_sos_alive(psp)) {
        /* psp_v13_0_init_sos_version() */
        psp->sos_fw_version = rdna3_rreg(psp->d, C2P(psp, 58));
        RLOG(psp->d, "PSP: sOS already alive (0x%08x), skipping bootloader", psp->sos_fw_version);
        return RDNA3_OK;
    }
    ret = psp13_wait_for_bootloader(psp);
    if (ret)
        return ret;
    ret = psp13_copy_fw(psp, s->src);
    if (ret)
        return ret;
    rdna3_wreg(psp->d, C2P(psp, 36), fw_pri_mc_mb(psp));
    rdna3_wreg(psp->d, C2P(psp, 35), s->bl_cmd);
    /* "there might be handshake issue with hardware which needs delay" */
    mdelay(psp->d, 20);
    ret = rdna3_wait_reg(psp->d, C2P(psp, 81), rdna3_rreg(psp->d, C2P(psp, 81)), 0, 1, 0);
    if (!ret)
        psp->sos_fw_version = rdna3_rreg(psp->d, C2P(psp, 58));
    return ret;
}

int psp13_bootloader_load_all(rdna3_psp *psp)
{
    unsigned i;
    int ret;
    for (i = 0; i < amd_psp_bl_sequence_len; i++) {
        const amd_psp_bl_step *s = &amd_psp_bl_sequence[i];
        /* is_psp_fw_valid() */
        if (!psp->sos.bin[s->bin].size || !psp->sos.bin[s->src].size)
            continue;
        RLOG(psp->d, "PSP: bootloader load %s (%u bytes)", s->name, psp->sos.bin[s->src].size);
        ret = (s->bin == AMD_PSP_BIN_SOS) ? psp13_bl_load_sos(psp, s)
                                          : psp13_bl_load_component(psp, s);
        if (ret) {
            RERR(psp->d, "PSP: load %s failed (%d), C2PMSG_35=0x%08x", s->name, ret,
                 rdna3_rreg(psp->d, C2P(psp, 35)));
            return ret;
        }
    }
    RLOG(psp->d, "PSP: sOS running, version 0x%08x", psp->sos_fw_version);
    return RDNA3_OK;
}

/* ---------------------------- ring ---------------------------- */

/* psp_v13_0_ring_create() (bare metal) */
int psp13_ring_create(rdna3_psp *psp)
{
    rdna3_dev *d = psp->d;
    uint64_t mc = rdna3_vram_mc(d, psp->ring);
    int ret;

    ret = rdna3_wait_reg(d, C2P(psp, 64), MBOX_TOS_READY_FLAG, MBOX_TOS_READY_MASK, 0, 0);
    if (ret) {
        RERR(d, "PSP: trust OS not ready for ring creation (C2PMSG_64=0x%08x)",
             rdna3_rreg(d, C2P(psp, 64)));
        return ret;
    }
    rdna3_wreg(d, C2P(psp, 69), (uint32_t)mc);
    rdna3_wreg(d, C2P(psp, 70), (uint32_t)(mc >> 32));
    rdna3_wreg(d, C2P(psp, 71), PSP_RING_SIZE);
    rdna3_wreg(d, C2P(psp, 64), PSP_RING_TYPE__KM << 16);
    mdelay(d, 20);
    ret = rdna3_wait_reg(d, C2P(psp, 64), MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK, 0, 0);
    if (ret)
        RERR(d, "PSP: ring create failed (C2PMSG_64=0x%08x)", rdna3_rreg(d, C2P(psp, 64)));
    else
        psp->ring_created = 1;
    return ret;
}

/* psp_v13_0_ring_stop() (bare metal) */
int psp13_ring_stop(rdna3_psp *psp)
{
    rdna3_dev *d = psp->d;
    rdna3_wreg(d, C2P(psp, 64), GFX_CTRL_CMD_ID_DESTROY_RINGS);
    mdelay(d, 20);
    psp->ring_created = 0;
    return rdna3_wait_reg(d, C2P(psp, 64), MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK, 0, 0);
}

/* psp_ring_cmd_submit() */
static int psp13_ring_submit(rdna3_psp *psp, uint64_t cmd_mc, uint64_t fence_mc, uint32_t index)
{
    rdna3_dev *d = psp->d;
    const uint32_t ring_dw = PSP_RING_SIZE / 4;
    const uint32_t frame_dw = (uint32_t)sizeof(struct psp_gfx_rb_frame) / 4;
    struct psp_gfx_rb_frame f;
    uint32_t wptr = rdna3_rreg(d, C2P(psp, 67));   /* psp_v13_0_ring_get_wptr() */
    uint64_t frame_off;

    if (wptr % ring_dw == 0)
        frame_off = 0;
    else
        frame_off = (uint64_t)(wptr / frame_dw) * sizeof(f);
    if (frame_off + sizeof(f) > PSP_RING_SIZE) {
        RERR(d, "PSP: write pointer 0x%x out of ring", wptr);
        return RDNA3_EINVAL;
    }
    bzero_local(&f, sizeof(f));
    f.cmd_buf_addr_hi = (uint32_t)(cmd_mc >> 32);
    f.cmd_buf_addr_lo = (uint32_t)cmd_mc;
    f.fence_addr_hi = (uint32_t)(fence_mc >> 32);
    f.fence_addr_lo = (uint32_t)fence_mc;
    f.fence_value = index;
    rdna3_vram_write(d, psp->ring + frame_off, &f, sizeof(f));
    rdna3_hdp_flush(d);

    wptr = (wptr + frame_dw) % ring_dw;
    rdna3_wreg(d, C2P(psp, 67), wptr);   /* psp_v13_0_ring_set_wptr() */
    return RDNA3_OK;
}

/* psp_cmd_submit_buf() */
static int psp13_cmd_submit(rdna3_psp *psp, struct psp_gfx_cmd_resp *cmd)
{
    rdna3_dev *d = psp->d;
    uint32_t index, fence = 0, timeout = PSP_CMD_TIMEOUT_ITERS;
    struct psp_gfx_resp resp;
    int ret;

    if (!psp->ring_created)
        return RDNA3_ENODEV;

    rdna3_vram_fill(d, psp->cmd, 0, PSP_CMD_BUFFER_SIZE);
    rdna3_vram_write(d, psp->cmd, cmd, sizeof(*cmd));

    index = ++psp->fence_value;
    ret = psp13_ring_submit(psp, rdna3_vram_mc(d, psp->cmd), rdna3_vram_mc(d, psp->fence), index);
    if (ret) {
        psp->fence_value--;
        return ret;
    }

    rdna3_hdp_invalidate(d);
    for (;;) {
        rdna3_vram_read(d, psp->fence, &fence, sizeof(fence));
        if (fence == index || --timeout == 0)
            break;
        d->os.udelay(d->os.ctx, 80);
        rdna3_hdp_invalidate(d);
    }

    rdna3_vram_read(d, psp->cmd + offsetof(struct psp_gfx_cmd_resp, resp), &resp, sizeof(resp));
    cmd->resp = resp;
    psp->last_resp_status = resp.status;

    if (!timeout) {
        RERR(d, "PSP: cmd 0x%x timed out (fence 0x%x, want 0x%x)", cmd->cmd_id, fence, index);
        return RDNA3_ETIME;
    }
    if (resp.status)
        /* amdgpu only warns: some PSP firmware leaves junk in status */
        RLOG(d, "PSP: cmd 0x%x response status 0x%x", cmd->cmd_id, resp.status);
    return RDNA3_OK;
}

/* ---------------------------- TMR ---------------------------- */

/* psp_load_toc(), psp_prep_load_toc_cmd_buf() */
int psp13_load_toc(rdna3_psp *psp, uint32_t *tmr_size)
{
    struct psp_gfx_cmd_resp cmd;
    const amd_psp_bin *toc = &psp->sos.bin[AMD_PSP_BIN_TOC];
    int ret;

    ret = psp13_copy_fw(psp, AMD_PSP_BIN_TOC);
    if (ret)
        return ret;
    bzero_local(&cmd, sizeof(cmd));
    cmd.cmd_id = GFX_CMD_ID_LOAD_TOC;
    cmd.cmd.cmd_load_toc.toc_phy_addr_lo = (uint32_t)rdna3_vram_mc(psp->d, psp->fw_pri);
    cmd.cmd.cmd_load_toc.toc_phy_addr_hi = (uint32_t)(rdna3_vram_mc(psp->d, psp->fw_pri) >> 32);
    cmd.cmd.cmd_load_toc.toc_size = toc->size;
    ret = psp13_cmd_submit(psp, &cmd);
    if (!ret)
        *tmr_size = cmd.resp.tmr_size;
    return ret;
}

int psp13_tmr_init(rdna3_psp *psp)
{
    uint32_t size = PSP_TMR_SIZE_DEFAULT;
    int ret;

    if (psp->sos.bin[AMD_PSP_BIN_TOC].size) {
        ret = psp13_load_toc(psp, &size);
        if (ret) {
            RERR(psp->d, "PSP: failed to load TOC");
            return ret;
        }
        psp->last_tmr_size_from_toc = size;
        RLOG(psp->d, "PSP: TOC asks for a %u KiB TMR", size / 1024);
        if (!size)
            size = PSP_TMR_SIZE_DEFAULT;
    }
    psp->tmr_size = size;
    /* psp_tmr_init: amdgpu_bo_create_kernel without a CPU address, i.e.
     * NO_CPU_ACCESS, which TTM places top-down, out of the visible VRAM */
    psp->tmr = rdna3_vram_alloc_top(psp->d, size, PSP_TMR_ALIGNMENT, "psp-tmr");
    return psp->tmr == UINT64_MAX ? RDNA3_ENOMEM : RDNA3_OK;
}

/* psp_tmr_load(), psp_prep_tmr_cmd_buf() (bare metal: SETUP_TMR) */
int psp13_tmr_load(rdna3_psp *psp)
{
    struct psp_gfx_cmd_resp cmd;
    uint64_t mc = rdna3_vram_mc(psp->d, psp->tmr);
    uint64_t pa = rdna3_vram_pa(psp->d, psp->tmr);

    bzero_local(&cmd, sizeof(cmd));
    cmd.cmd_id = GFX_CMD_ID_SETUP_TMR;
    cmd.cmd.cmd_setup_tmr.buf_phy_addr_lo = (uint32_t)mc;
    cmd.cmd.cmd_setup_tmr.buf_phy_addr_hi = (uint32_t)(mc >> 32);
    cmd.cmd.cmd_setup_tmr.buf_size = psp->tmr_size;
    cmd.cmd.cmd_setup_tmr.bitfield.virt_phy_addr = 1;
    cmd.cmd.cmd_setup_tmr.system_phy_addr_lo = (uint32_t)pa;
    cmd.cmd.cmd_setup_tmr.system_phy_addr_hi = (uint32_t)(pa >> 32);
    RLOG(psp->d, "PSP: reserve 0x%x from MC 0x%llx for TMR", psp->tmr_size,
         (unsigned long long)mc);
    return psp13_cmd_submit(psp, &cmd);
}

/* psp_rlc_autoload_start() */
int psp13_autoload_rlc(rdna3_psp *psp)
{
    struct psp_gfx_cmd_resp cmd;
    bzero_local(&cmd, sizeof(cmd));
    cmd.cmd_id = GFX_CMD_ID_AUTOLOAD_RLC;
    return psp13_cmd_submit(psp, &cmd);
}

/* ------------------------- IP firmware ------------------------- */

int psp13_stage_ucodes(rdna3_psp *psp, const amd_ucode_plan *plan,
                       const amd_fw_blob_ref blobs[AMD_BLOB_COUNT],
                       uint64_t mc_out[AMD_UCODE_MAX])
{
    rdna3_dev *d = psp->d;
    uint64_t pos;
    uint32_t i;

    psp->fw_buf_size = plan->total_aligned;
    psp->fw_buf = rdna3_vram_alloc(d, plan->total_aligned, 4096, "ucode-buf");   /* amdgpu_ucode_create_bo */
    if (psp->fw_buf == UINT64_MAX)
        return RDNA3_ENOMEM;
    pos = psp->fw_buf;
    for (i = 0; i < plan->count; i++) {
        const amd_ucode *u = &plan->ucode[i];
        rdna3_vram_write(d, pos, blobs[u->blob].data + u->offset, u->size);
        mc_out[i] = rdna3_vram_mc(d, pos);
        pos += (u->size + 4095u) & ~4095ull;
    }
    rdna3_hdp_flush(d);
    return RDNA3_OK;
}

/* psp_execute_ip_fw_load(), psp_prep_load_ip_fw_cmd_buf(); the fw_type
 * (psp_get_fw_type) comes with the ucode from amd_ucode_plan_build() */
int psp13_load_ip_fw(rdna3_psp *psp, const amd_ucode *u, uint64_t mc)
{
    struct psp_gfx_cmd_resp cmd;
    int ret;

    bzero_local(&cmd, sizeof(cmd));
    cmd.cmd_id = GFX_CMD_ID_LOAD_IP_FW;
    cmd.cmd.cmd_load_ip_fw.fw_phy_addr_lo = (uint32_t)mc;
    cmd.cmd.cmd_load_ip_fw.fw_phy_addr_hi = (uint32_t)(mc >> 32);
    cmd.cmd.cmd_load_ip_fw.fw_size = u->size;
    cmd.cmd.cmd_load_ip_fw.fw_type = (enum psp_gfx_fw_type)u->psp_type;
    ret = psp13_cmd_submit(psp, &cmd);
    if (!ret && u->ucode_id < 64)
        psp->ucode_tmr_mc[u->ucode_id] = cmd.resp.fw_addr_lo | ((uint64_t)cmd.resp.fw_addr_hi << 32);
    RLOG(psp->d, "PSP: LOAD_IP_FW %-22s %6u bytes -> %s (status 0x%x)", u->name, u->size,
         ret ? "FAILED" : "ok", psp->last_resp_status);
    return ret;
}

/* --------------------------- hw_init --------------------------- */

int psp13_hw_init(rdna3_psp *psp, const amd_ucode_plan *plan,
                  const amd_fw_blob_ref blobs[AMD_BLOB_COUNT])
{
    static uint64_t mc[AMD_UCODE_MAX];
    const amd_ucode *smc;
    uint32_t i;
    int ret;

    /* psp_hw_start() */
    ret = psp13_bootloader_load_all(psp);
    if (ret)
        return ret;
    ret = psp13_ring_create(psp);
    if (ret)
        return ret;
    ret = psp13_tmr_init(psp);
    if (ret)
        return ret;
    ret = psp13_stage_ucodes(psp, plan, blobs, mc);
    if (ret)
        return ret;

    /* pmfw_centralized_cstate_management: PMFW before SETUP_TMR */
    smc = amd_ucode_plan_find(plan, AMD_UCODE_SMC);
    if (smc) {   /* psp_load_smu_fw() */
        ret = psp13_load_ip_fw(psp, smc, mc[smc - plan->ucode]);
        if (ret)
            return ret;
    }
    ret = psp13_tmr_load(psp);
    if (ret)
        return ret;

    /* psp_load_non_psp_fw() */
    for (i = 0; i < plan->count; i++) {
        const amd_ucode *u = &plan->ucode[i];
        if (u->ucode_id == AMD_UCODE_SMC)
            continue; /* fw_load_skip_check(): already loaded */
        ret = psp13_load_ip_fw(psp, u, mc[i]);
        if (ret)
            return ret;
        if (u->ucode_id == AMD_UCODE_RLC_G) {
            ret = psp13_autoload_rlc(psp);
            if (ret) {
                RERR(psp->d, "PSP: failed to start RLC autoload");
                return ret;
            }
            RLOG(psp->d, "PSP: RLC autoload started");
        }
    }
    return RDNA3_OK;
}
