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

    psp13_early_init(psp);
    psp->asd.shared = psp->hdcp.shared = psp->dtm.shared = UINT64_MAX;

    /* psp_v13_0_init_microcode -> psp_init_sos_microcode */
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

/* Ported from psp_check_pmfw_centralized_cstate_management (bare metal):
 * the PMFW manages DF C-states on these MP0 versions and is loaded before
 * SETUP_TMR; MP0 13.0.10 is not one of them */
int psp13_pmfw_centralized_cstate_management(const rdna3_ipver *v)
{
    if (v->major == 11)
        return v->rev == 0 || v->rev == 4 || v->rev == 5 || v->rev == 7 || v->rev == 9 ||
               v->rev == 11 || v->rev == 12 || v->rev == 13;
    if (v->major == 13 && v->minor == 0)
        return v->rev == 0 || v->rev == 2 || v->rev == 7;
    return 0;
}

/* Ported from psp_early_init, the MP0 13.0.x cases (the psp_v13_0 callbacks
 * are psp13.c itself): RLC autoload through the PSP, the TMR allocated by
 * the driver (no boot-time TMR) on 13.0.0 / 13.0.7 / 13.0.10 */
void psp13_early_init(rdna3_psp *psp)
{
    const rdna3_ipver *v = &psp->d->ipver[HWIP_MP0];
    psp->autoload_supported = 1;
    psp->boot_time_tmr = 1;
    if (v->major == 13 && v->minor == 0) {
        switch (v->rev) {
        case 2:
            psp->boot_time_tmr = 0;
            psp->autoload_supported = 0;
            break;
        case 6: case 14:
            psp->autoload_supported = 0;
            break;
        default:   /* 0, 1, 3, 4, 5, 7, 8, 10, 11 */
            psp->boot_time_tmr = 0;
            break;
        }
    }
    psp->pmfw_centralized_cstate_management = psp13_pmfw_centralized_cstate_management(v);
}

/* Ported from acquire_psp_cmd_buf: psp->cmd zeroed for the next command */
static struct psp_gfx_cmd_resp *acquire_psp_cmd_buf(rdna3_psp *psp)
{
    if (psp->cmd_busy)
        RERR(psp->d, "PSP: command buffer taken twice");
    psp->cmd_busy = 1;
    bzero_local(&psp->cmd_buf, sizeof(psp->cmd_buf));
    return &psp->cmd_buf;
}

/* Ported from release_psp_cmd_buf */
static void release_psp_cmd_buf(rdna3_psp *psp)
{
    psp->cmd_busy = 0;
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

/* psp_v13_0_ring_stop() (bare metal); amdgpu waits for the response flag
 * alone (bit 31), the verified default for the status bits too */
int psp13_ring_stop(rdna3_psp *psp)
{
    rdna3_dev *d = psp->d;
    rdna3_wreg(d, C2P(psp, 64), GFX_CTRL_CMD_ID_DESTROY_RINGS);
    mdelay(d, 20);
    psp->ring_created = 0;
    if (d->amdgpu_exact)
        return rdna3_wait_reg(d, C2P(psp, 64), 0x80000000u, 0x80000000u, 0, 0);
    return rdna3_wait_reg(d, C2P(psp, 64), MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK, 0, 0);
}

/* Ported from psp_v13_0_ring_destroy: stop, and the ring buffer is no
 * longer the PSP's (amdgpu frees it; it stays in the bump allocator) */
int psp13_ring_destroy(rdna3_psp *psp)
{
    int ret = psp13_ring_stop(psp);
    if (ret)
        RERR(psp->d, "PSP: fail to stop psp ring");
    return ret;
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
    struct psp_gfx_cmd_resp *cmd;
    const amd_psp_bin *toc = &psp->sos.bin[AMD_PSP_BIN_TOC];
    int ret;

    ret = psp13_copy_fw(psp, AMD_PSP_BIN_TOC);
    if (ret)
        return ret;
    cmd = acquire_psp_cmd_buf(psp);
    cmd->cmd_id = GFX_CMD_ID_LOAD_TOC;
    cmd->cmd.cmd_load_toc.toc_phy_addr_lo = (uint32_t)rdna3_vram_mc(psp->d, psp->fw_pri);
    cmd->cmd.cmd_load_toc.toc_phy_addr_hi = (uint32_t)(rdna3_vram_mc(psp->d, psp->fw_pri) >> 32);
    cmd->cmd.cmd_load_toc.toc_size = toc->size;
    ret = psp13_cmd_submit(psp, cmd);
    if (!ret)
        *tmr_size = cmd->resp.tmr_size;
    release_psp_cmd_buf(psp);
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
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    uint64_t mc = rdna3_vram_mc(psp->d, psp->tmr);
    uint64_t pa = rdna3_vram_pa(psp->d, psp->tmr);
    int ret;

    cmd->cmd_id = GFX_CMD_ID_SETUP_TMR;
    cmd->cmd.cmd_setup_tmr.buf_phy_addr_lo = (uint32_t)mc;
    cmd->cmd.cmd_setup_tmr.buf_phy_addr_hi = (uint32_t)(mc >> 32);
    cmd->cmd.cmd_setup_tmr.buf_size = psp->tmr_size;
    cmd->cmd.cmd_setup_tmr.bitfield.virt_phy_addr = 1;
    cmd->cmd.cmd_setup_tmr.system_phy_addr_lo = (uint32_t)pa;
    cmd->cmd.cmd_setup_tmr.system_phy_addr_hi = (uint32_t)(pa >> 32);
    RLOG(psp->d, "PSP: reserve 0x%x from MC 0x%llx for TMR", psp->tmr_size,
         (unsigned long long)mc);
    ret = psp13_cmd_submit(psp, cmd);
    release_psp_cmd_buf(psp);
    return ret;
}

/* psp_rlc_autoload_start() */
int psp13_autoload_rlc(rdna3_psp *psp)
{
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    int ret;
    cmd->cmd_id = GFX_CMD_ID_AUTOLOAD_RLC;
    ret = psp13_cmd_submit(psp, cmd);
    release_psp_cmd_buf(psp);
    return ret;
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
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    int ret;

    cmd->cmd_id = GFX_CMD_ID_LOAD_IP_FW;
    cmd->cmd.cmd_load_ip_fw.fw_phy_addr_lo = (uint32_t)mc;
    cmd->cmd.cmd_load_ip_fw.fw_phy_addr_hi = (uint32_t)(mc >> 32);
    cmd->cmd.cmd_load_ip_fw.fw_size = u->size;
    cmd->cmd.cmd_load_ip_fw.fw_type = (enum psp_gfx_fw_type)u->psp_type;
    ret = psp13_cmd_submit(psp, cmd);
    if (!ret && u->ucode_id < 64)
        psp->ucode_tmr_mc[u->ucode_id] = cmd->resp.fw_addr_lo | ((uint64_t)cmd->resp.fw_addr_hi << 32);
    release_psp_cmd_buf(psp);
    RLOG(psp->d, "PSP: LOAD_IP_FW %-22s %6u bytes -> %s (status 0x%x)", u->name, u->size,
         ret ? "FAILED" : "ok", psp->last_resp_status);
    return ret;
}

/* psp_execute_ip_fw_load() / psp_prep_load_ip_fw_cmd_buf() with the
 * fw_type psp_get_fw_type gives AMDGPU_UCODE_ID_VCN0_RAM / VCN1_RAM: the
 * buffer is the caller's (amdgpu_vcn_psp_update_sram), nothing is placed
 * in the TMR */
int psp13_load_ip_fw_buf(rdna3_psp *psp, uint32_t fw_type, uint64_t mc, uint32_t size)
{
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    int ret;

    cmd->cmd_id = GFX_CMD_ID_LOAD_IP_FW;
    cmd->cmd.cmd_load_ip_fw.fw_phy_addr_lo = (uint32_t)mc;
    cmd->cmd.cmd_load_ip_fw.fw_phy_addr_hi = (uint32_t)(mc >> 32);
    cmd->cmd.cmd_load_ip_fw.fw_size = size;
    cmd->cmd.cmd_load_ip_fw.fw_type = (enum psp_gfx_fw_type)fw_type;
    ret = psp13_cmd_submit(psp, cmd);
    release_psp_cmd_buf(psp);
    if (ret || psp->last_resp_status)
        RLOG(psp->d, "PSP: LOAD_IP_FW type %u, %u bytes -> %s (status 0x%x)", fw_type, size,
             ret ? "FAILED" : "ok", psp->last_resp_status);
    return ret;
}

/* ----------------------------- TAs ----------------------------- */

static void ta_ctx_bind(rdna3_psp *psp, rdna3_psp_ta_ctx *c, int ta)
{
    const amd_psp_bin *b = &psp->ta.bin[ta];
    c->bin = (psp->ta_fw && b->size) ? psp->ta_fw + b->offset : 0;
    c->bin_size = psp->ta_fw ? b->size : 0;
}

/* psp_init_ta_microcode (parse_ta_v2_microcode): the TA file's ASD, HDCP
 * and DTM become their contexts' bin_desc */
int psp13_set_ta(rdna3_psp *psp, const uint8_t *fw, size_t len)
{
    amd_fw_status st;
    psp->ta_fw = 0;
    psp->ta_len = 0;
    if (fw) {
        st = amd_psp_ta_parse(fw, len, &psp->ta);
        if (st != AMD_FW_OK) {
            RERR(psp->d, "PSP: TA firmware: %s", amd_fw_status_str(st));
            return RDNA3_EINVAL;
        }
        psp->ta_fw = fw;
        psp->ta_len = len;
    }
    ta_ctx_bind(psp, &psp->asd, AMD_TA_ASD);
    ta_ctx_bind(psp, &psp->hdcp, AMD_TA_HDCP);
    ta_ctx_bind(psp, &psp->dtm, AMD_TA_DTM);
    return RDNA3_OK;
}

/* Ported from psp_ta_init_shared_buf: 16 KiB, 4 KiB aligned, CPU visible */
static int psp_ta_init_shared_buf(rdna3_psp *psp, rdna3_psp_ta_ctx *c)
{
    c->shared = rdna3_vram_alloc(psp->d, c->shared_mem_size, 4096, "psp-ta-shared");
    if (c->shared == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(psp->d, c->shared, 0, c->shared_mem_size);
    rdna3_hdp_flush(psp->d);
    c->shared_mc = rdna3_vram_mc(psp->d, c->shared);
    return RDNA3_OK;
}

/* Ported from psp_ta_free_shared_buf: the bump allocator frees nothing, the
 * buffer is forgotten (it comes back with the next rdna3_vram_alloc_init) */
static void psp_ta_free_shared_buf(rdna3_psp_ta_ctx *c)
{
    c->shared = UINT64_MAX;
    c->shared_mc = 0;
}

/* Ported from psp_free_shared_bufs: the TA buffers this driver allocates
 * (HDCP, DTM); the TMR stays with the allocator */
static void psp_free_shared_bufs(rdna3_psp *psp)
{
    psp_ta_free_shared_buf(&psp->hdcp);
    psp_ta_free_shared_buf(&psp->dtm);
}

/* Ported from psp_prep_ta_load_cmd_buf */
static void psp_prep_ta_load_cmd_buf(struct psp_gfx_cmd_resp *cmd, uint64_t ta_bin_mc,
                                     const rdna3_psp_ta_ctx *c)
{
    cmd->cmd_id = (enum psp_gfx_cmd_id)c->ta_load_type;
    cmd->cmd.cmd_load_ta.app_phy_addr_lo = (uint32_t)ta_bin_mc;
    cmd->cmd.cmd_load_ta.app_phy_addr_hi = (uint32_t)(ta_bin_mc >> 32);
    cmd->cmd.cmd_load_ta.app_len = c->bin_size;
    cmd->cmd.cmd_load_ta.cmd_buf_phy_addr_lo = (uint32_t)c->shared_mc;
    cmd->cmd.cmd_load_ta.cmd_buf_phy_addr_hi = (uint32_t)(c->shared_mc >> 32);
    cmd->cmd.cmd_load_ta.cmd_buf_len = c->shared_mem_size;
}

/* Ported from psp_prep_ta_unload_cmd_buf */
static void psp_prep_ta_unload_cmd_buf(struct psp_gfx_cmd_resp *cmd, uint32_t session_id)
{
    cmd->cmd_id = GFX_CMD_ID_UNLOAD_TA;
    cmd->cmd.cmd_unload_ta.session_id = session_id;
}

/* Ported from psp_prep_ta_invoke_cmd_buf */
static void psp_prep_ta_invoke_cmd_buf(struct psp_gfx_cmd_resp *cmd, uint32_t ta_cmd_id,
                                       uint32_t session_id)
{
    cmd->cmd_id = GFX_CMD_ID_INVOKE_CMD;
    cmd->cmd.cmd_invoke_cmd.session_id = session_id;
    cmd->cmd.cmd_invoke_cmd.ta_cmd_id = ta_cmd_id;
}

/* Ported from psp_ta_load: the TA image through fw_pri (psp_copy_fw), the
 * session id from the response */
int psp13_ta_load(rdna3_psp *psp, rdna3_psp_ta_ctx *c)
{
    struct psp_gfx_cmd_resp *cmd;
    int ret;

    if (!c->bin || !c->bin_size || c->bin_size > PSP_1_MEG)
        return RDNA3_EINVAL;
    cmd = acquire_psp_cmd_buf(psp);
    rdna3_vram_fill(psp->d, psp->fw_pri, 0, PSP_1_MEG);
    rdna3_vram_write(psp->d, psp->fw_pri, c->bin, c->bin_size);
    rdna3_hdp_flush(psp->d);
    psp_prep_ta_load_cmd_buf(cmd, rdna3_vram_mc(psp->d, psp->fw_pri), c);
    ret = psp13_cmd_submit(psp, cmd);
    c->resp_status = cmd->resp.status;
    if (!ret)
        c->session_id = cmd->resp.session_id;
    release_psp_cmd_buf(psp);
    return ret;
}

/* Ported from psp_ta_unload */
int psp13_ta_unload(rdna3_psp *psp, rdna3_psp_ta_ctx *c)
{
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    int ret;
    psp_prep_ta_unload_cmd_buf(cmd, c->session_id);
    ret = psp13_cmd_submit(psp, cmd);
    c->resp_status = cmd->resp.status;
    release_psp_cmd_buf(psp);
    return ret;
}

/* Ported from psp_ta_invoke */
int psp13_ta_invoke(rdna3_psp *psp, uint32_t ta_cmd_id, rdna3_psp_ta_ctx *c)
{
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    int ret;
    psp_prep_ta_invoke_cmd_buf(cmd, ta_cmd_id, c->session_id);
    ret = psp13_cmd_submit(psp, cmd);
    c->resp_status = cmd->resp.status;
    release_psp_cmd_buf(psp);
    return ret;
}

/* Ported from psp_asd_initialize (bare metal; the card has display
 * hardware, so MP0 13.0.10 does not bypass it) */
int psp13_asd_initialize(rdna3_psp *psp)
{
    int ret;
    if (!psp->asd.bin_size)
        return RDNA3_OK;
    psp->asd.shared_mc = 0;
    psp->asd.shared_mem_size = PSP_ASD_SHARED_MEM_SIZE;
    psp->asd.ta_load_type = GFX_CMD_ID_LOAD_ASD;
    ret = psp13_ta_load(psp, &psp->asd);
    if (!ret)
        psp->asd.initialized = 1;
    RLOG(psp->d, "PSP: LOAD_ASD %u bytes -> %s (session 0x%x, status 0x%x)", psp->asd.bin_size,
         ret ? "FAILED" : "ok", psp->asd.session_id, psp->asd.resp_status);
    return ret;
}

/* Ported from psp_asd_terminate */
int psp13_asd_terminate(rdna3_psp *psp)
{
    int ret;
    if (!psp->asd.initialized)
        return RDNA3_OK;
    ret = psp13_ta_unload(psp, &psp->asd);
    if (!ret)
        psp->asd.initialized = 0;
    return ret;
}

static int ta_initialize(rdna3_psp *psp, rdna3_psp_ta_ctx *c, uint32_t size, const char *name)
{
    int ret;
    if (!c->bin_size || !c->bin) {
        RLOG(psp->d, "%s: optional %s ta ucode is not available", name, name);
        return RDNA3_OK;
    }
    c->shared_mem_size = size;
    c->ta_load_type = GFX_CMD_ID_LOAD_TA;
    if (c->shared == UINT64_MAX) {
        ret = psp_ta_init_shared_buf(psp, c);
        if (ret)
            return ret;
    }
    ret = psp13_ta_load(psp, c);
    if (!ret)
        c->initialized = 1;
    RLOG(psp->d, "PSP: LOAD_TA %s %u bytes -> %s (session 0x%x, status 0x%x)", name, c->bin_size,
         ret ? "FAILED" : "ok", c->session_id, c->resp_status);
    return ret;
}

static int ta_terminate(rdna3_psp *psp, rdna3_psp_ta_ctx *c)
{
    int ret;
    if (!c->initialized)
        return RDNA3_OK;
    ret = psp13_ta_unload(psp, c);
    c->initialized = 0;
    return ret;
}

/* Ported from psp_hdcp_initialize */
int psp13_hdcp_initialize(rdna3_psp *psp)
{
    return ta_initialize(psp, &psp->hdcp, PSP_HDCP_SHARED_MEM_SIZE, "HDCP");
}

/* Ported from psp_hdcp_invoke */
int psp13_hdcp_invoke(rdna3_psp *psp, uint32_t ta_cmd_id)
{
    if (!psp->hdcp.initialized)
        return RDNA3_EINVAL;
    return psp13_ta_invoke(psp, ta_cmd_id, &psp->hdcp);
}

/* Ported from psp_hdcp_terminate */
int psp13_hdcp_terminate(rdna3_psp *psp)
{
    return ta_terminate(psp, &psp->hdcp);
}

/* Ported from psp_dtm_initialize */
int psp13_dtm_initialize(rdna3_psp *psp)
{
    return ta_initialize(psp, &psp->dtm, PSP_DTM_SHARED_MEM_SIZE, "DTM");
}

/* Ported from psp_dtm_invoke */
int psp13_dtm_invoke(rdna3_psp *psp, uint32_t ta_cmd_id)
{
    if (!psp->dtm.initialized)
        return RDNA3_EINVAL;
    return psp13_ta_invoke(psp, ta_cmd_id, &psp->dtm);
}

/* Ported from psp_dtm_terminate */
int psp13_dtm_terminate(rdna3_psp *psp)
{
    return ta_terminate(psp, &psp->dtm);
}

/* Ported from psp_rl_load: the register list binary of the sOS file
 * (PSP_FW_TYPE_PSP_RL) through fw_pri, LOAD_IP_FW GFX_FW_TYPE_REG_LIST */
int psp13_rl_load(rdna3_psp *psp)
{
    const amd_psp_bin *rl = &psp->sos.bin[AMD_PSP_BIN_RL];
    struct psp_gfx_cmd_resp *cmd;
    uint64_t mc;
    int ret;

    if (!rl->size)   /* is_psp_fw_valid */
        return RDNA3_OK;
    ret = psp13_copy_fw(psp, AMD_PSP_BIN_RL);
    if (ret)
        return ret;
    mc = rdna3_vram_mc(psp->d, psp->fw_pri);
    cmd = acquire_psp_cmd_buf(psp);
    cmd->cmd_id = GFX_CMD_ID_LOAD_IP_FW;
    cmd->cmd.cmd_load_ip_fw.fw_phy_addr_lo = (uint32_t)mc;
    cmd->cmd.cmd_load_ip_fw.fw_phy_addr_hi = (uint32_t)(mc >> 32);
    cmd->cmd.cmd_load_ip_fw.fw_size = rl->size;
    cmd->cmd.cmd_load_ip_fw.fw_type = GFX_FW_TYPE_REG_LIST;
    ret = psp13_cmd_submit(psp, cmd);
    release_psp_cmd_buf(psp);
    psp->rl_loaded = !ret;
    RLOG(psp->d, "PSP: LOAD_IP_FW REG_LIST %u bytes -> %s (status 0x%x)", rl->size,
         ret ? "FAILED" : "ok", psp->last_resp_status);
    return ret;
}

/* Ported from psp_prep_tmr_unload_cmd_buf (bare metal: DESTROY_TMR) */
static void psp_prep_tmr_unload_cmd_buf(struct psp_gfx_cmd_resp *cmd)
{
    cmd->cmd_id = GFX_CMD_ID_DESTROY_TMR;
}

/* Ported from psp_tmr_unload */
static int psp_tmr_unload(rdna3_psp *psp)
{
    struct psp_gfx_cmd_resp *cmd = acquire_psp_cmd_buf(psp);
    int ret;
    psp_prep_tmr_unload_cmd_buf(cmd);
    RDBG(psp->d, "PSP: free PSP TMR buffer");
    ret = psp13_cmd_submit(psp, cmd);
    release_psp_cmd_buf(psp);
    return ret;
}

/* Ported from psp_tmr_terminate */
int psp13_tmr_terminate(rdna3_psp *psp)
{
    return psp_tmr_unload(psp);
}

/* Ported from psp_hw_fini: no RAS, RAP, secure display or XGMI TA runs
 * on this card */
int psp13_hw_fini(rdna3_psp *psp)
{
    if (psp->ta_fw) {
        psp13_dtm_terminate(psp);
        psp13_hdcp_terminate(psp);
    }
    psp13_asd_terminate(psp);
    psp13_tmr_terminate(psp);
    psp13_ring_destroy(psp);
    psp_free_shared_bufs(psp);
    return RDNA3_OK;
}

/* Ported from psp_suspend: the TAs, ASD and the TMR go, the ring stops */
int psp13_suspend(rdna3_psp *psp)
{
    int ret;
    if (psp->ta_fw) {
        ret = psp13_hdcp_terminate(psp);
        if (ret) {
            RERR(psp->d, "PSP: Failed to terminate hdcp ta");
            return ret;
        }
        ret = psp13_dtm_terminate(psp);
        if (ret) {
            RERR(psp->d, "PSP: Failed to terminate dtm ta");
            return ret;
        }
    }
    ret = psp13_asd_terminate(psp);
    if (ret) {
        RERR(psp->d, "PSP: Failed to terminate asd");
        return ret;
    }
    ret = psp13_tmr_terminate(psp);
    if (ret) {
        RERR(psp->d, "PSP: Failed to terminate tmr");
        return ret;
    }
    ret = psp13_ring_stop(psp);
    if (ret)
        RERR(psp->d, "PSP: ring stop failed");
    return ret;
}

/* --------------------------- hw_init --------------------------- */

/* psp_load_smu_fw (not in a BACO runtime suspend, not SR-IOV) */
static int psp_load_smu_fw(rdna3_psp *psp, const amd_ucode_plan *plan, const uint64_t *mc)
{
    const amd_ucode *smc = amd_ucode_plan_find(plan, AMD_UCODE_SMC);
    int ret;
    if (!smc)
        return RDNA3_OK;
    ret = psp13_load_ip_fw(psp, smc, mc[smc - plan->ucode]);
    if (ret)
        RERR(psp->d, "PSP: load smu failed!");
    return ret;
}

/* psp_hw_init -> amdgpu_ucode_init_bo (psp13_stage_ucodes) + psp_load_fw:
 * psp_hw_start, psp_load_non_psp_fw, then (rdna3amdgpu=1) ASD, the RL and
 * the TAs */
int psp13_hw_init(rdna3_psp *psp, const amd_ucode_plan *plan,
                  const amd_fw_blob_ref blobs[AMD_BLOB_COUNT])
{
    static uint64_t mc[AMD_UCODE_MAX];
    rdna3_dev *d = psp->d;
    /* the verified default loads the PMFW before SETUP_TMR, as amdgpu
     * does where the PMFW manages DF C-states (not MP0 13.0.10) */
    int centralized = d->amdgpu_exact ? psp->pmfw_centralized_cstate_management : 1;
    uint32_t i;
    int ret;

    /* psp_hw_start() */
    ret = psp13_bootloader_load_all(psp);
    if (ret)
        return ret;
    ret = psp13_ring_create(psp);
    if (ret)
        return ret;
    if (!psp->boot_time_tmr || psp->autoload_supported) {
        ret = psp13_tmr_init(psp);
        if (ret)
            return ret;
    }
    ret = psp13_stage_ucodes(psp, plan, blobs, mc);
    if (ret)
        return ret;
    /* "For ASICs with DF Cstate management centralized to PMFW, TMR setup
     * should be performed after PMFW loaded and before other non-psp
     * firmware loaded." */
    if (centralized) {
        ret = psp_load_smu_fw(psp, plan, mc);
        if (ret)
            return ret;
    }
    if (!psp->boot_time_tmr || !psp->autoload_supported) {
        ret = psp13_tmr_load(psp);
        if (ret)
            return ret;
    }

    /* psp_load_non_psp_fw() */
    if (psp->autoload_supported && !centralized) {
        ret = psp_load_smu_fw(psp, plan, mc);
        if (ret)
            return ret;
    }
    for (i = 0; i < plan->count; i++) {
        const amd_ucode *u = &plan->ucode[i];
        if (u->ucode_id == AMD_UCODE_SMC)
            continue; /* fw_load_skip_check(): autoload / centralized loaded it */
        ret = psp13_load_ip_fw(psp, u, mc[i]);
        if (ret)
            return ret;
        if (psp->autoload_supported && u->ucode_id == AMD_UCODE_RLC_G) {
            ret = psp13_autoload_rlc(psp);
            if (ret) {
                RERR(psp->d, "PSP: failed to start RLC autoload");
                return ret;
            }
            RLOG(psp->d, "PSP: RLC autoload started");
        }
    }
    if (!d->amdgpu_exact)
        return RDNA3_OK;

    /* psp_load_fw, after psp_load_non_psp_fw */
    ret = psp13_asd_initialize(psp);
    if (ret) {
        RERR(d, "PSP: load asd failed!");
        goto failed;
    }
    ret = psp13_rl_load(psp);
    if (ret) {
        RERR(d, "PSP: load RL failed!");
        goto failed;
    }
    if (psp->ta_fw) {
        if (psp13_hdcp_initialize(psp))
            RERR(d, "HDCP: Failed to initialize HDCP");
        if (psp13_dtm_initialize(psp))
            RERR(d, "DTM: Failed to initialize DTM");
    }
    return RDNA3_OK;
failed:
    psp_free_shared_bufs(psp);
    psp13_ring_destroy(psp);
    return ret;
}
