/*
 * dmub32.c - see dmub32.h. Ported from Linux amdgpu Display Core
 * (dmub/src/dmub_srv.c, dmub/src/dmub_dcn32.c, amdgpu_dm/amdgpu_dm_dmub.c,
 * dc/dc_dmub_srv.c), MIT, Copyright Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dmub32.h"
#include "gen/dcn_3_2_1_dmcub.h"

#define PSP_HEADER_BYTES_256   0x100
#define PSP_FOOTER_BYTES_256   0x100
#define DMUB_FW_META_OFFSET    0x24
#define DMUB_STACK_SIZE        (128u * 1024u)
#define DMUB_CONTEXT_SIZE      (512u * 1024u)
#define DMUB_MAILBOX_SIZE      (2u * RD_DMUB_RB_SIZE)
#define DMUB_FW_STATE_SIZE     (64u * 1024u)
#define DMUB_TRACE_BUFFER_SIZE (64u * 1024u)
#define DMUB_SCRATCH_MEM_SIZE  1024u
#define DMUB_IB_MEM_SIZE       4096u    /* sizeof(dmub_fams2_config_v2) rounded up; unused on DCN32 */
#define DMUB_SHARED_STATE_SIZE (5u * 256u)  /* (FEATURE__LAST + 1) * 256-byte blocks */
#define DMUB_LSDMA_RB_SIZE     (64u * 1024u)
#define DMUB_CURSOR_OFFLOAD_SIZE 4096u

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SETF(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
/* paste the register name here (not through RREG32_SOC15) so it is not
 * macro-expanded before reg##_BASE_IDX is formed */
#define RR(d, reg) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg))
#define WR(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg), (v))
#define UPD(d, name, field, x) WR(d, reg##name, SETF(RR(d, reg##name), name, field, x))

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static void udelay(rdna3_dev *d, uint32_t us) { d->os.udelay(d->os.ctx, us); }

/* ---- firmware blob ---- */

static int meta_from_blob(const uint8_t *blob, uint32_t size, uint32_t meta_offset, rdna3_dmub_meta *m)
{
    const uint8_t *p;
    if (!blob || size < 64 + meta_offset)
        return 0;
    p = blob + size - meta_offset - 64;
    if (rd32le(p) != RD_DMUB_FW_META_MAGIC)
        return 0;
    m->fw_region_size = rd32le(p + 4);
    m->trace_buffer_size = rd32le(p + 8);
    m->fw_version = rd32le(p + 12);
    m->dal_fw = p[16];
    m->shared_state_size = rd32le(p + 20);
    m->feature_bits = rd32le(p + 28);
    m->found = 1;
    return 1;
}

/* dmub_srv_get_fw_meta_info_from_raw_fw: the legacy region at the end of
 * BSS, else the combined one at the end of the instruction image behind a
 * 256- or 512-byte PSP footer, 16-byte alignment slack */
static void find_meta(rdna3_dmub *dm)
{
    uint32_t foot, i;
    if (dm->fw_bss_data && dm->bss_data_size &&
        meta_from_blob(dm->fw_bss_data, dm->bss_data_size, DMUB_FW_META_OFFSET, &dm->meta)) {
        dm->inst_const_size -= PSP_FOOTER_BYTES_256;
        return;
    }
    for (foot = PSP_FOOTER_BYTES_256; foot <= 2 * PSP_FOOTER_BYTES_256; foot += PSP_FOOTER_BYTES_256) {
        if (dm->inst_const_size <= foot)
            break;
        for (i = 0; i < 16; i++)
            if (meta_from_blob(dm->fw_inst_const, dm->inst_const_size - foot, i, &dm->meta)) {
                dm->inst_const_size -= foot;
                return;
            }
    }
    dm->inst_const_size -= PSP_FOOTER_BYTES_256;   /* default footer, no meta */
}

int dmub32_sw_init(rdna3_dmub *dm, rdna3_dev *d, const uint8_t *fw, uint32_t fw_len,
                   uint32_t vbios_size)
{
    uint32_t hdr_size, ucode_off, inst_bytes, bss_bytes, sizes[RD_DMUB_WIN_TOTAL], top = 0, i;
    uint64_t off;

    for (i = 0; i < sizeof(*dm); i++)
        ((uint8_t *)dm)[i] = 0;
    dm->d = d;
    /* dmcub_firmware_header_v1_0: common header (32) + inst_const_bytes + bss_data_bytes */
    if (!fw || fw_len < 40)
        return RDNA3_EINVAL;
    hdr_size = rd32le(fw + 4);
    dm->fw_version = rd32le(fw + 16);
    ucode_off = rd32le(fw + 24);
    inst_bytes = rd32le(fw + 32);
    bss_bytes = rd32le(fw + 36);
    if (hdr_size < 40 || inst_bytes <= PSP_HEADER_BYTES_256 + PSP_FOOTER_BYTES_256 ||
        (uint64_t)ucode_off + inst_bytes + bss_bytes > fw_len) {
        RERR(d, "DMUB: bad firmware header (ucode 0x%x inst %u bss %u, %u bytes)", ucode_off,
             inst_bytes, bss_bytes, fw_len);
        return RDNA3_EINVAL;
    }
    dm->fw_inst_const = fw + ucode_off + PSP_HEADER_BYTES_256;
    dm->inst_const_size = inst_bytes - PSP_HEADER_BYTES_256;
    dm->bss_data_size = bss_bytes;
    dm->fw_bss_data = bss_bytes ? fw + ucode_off + inst_bytes : NULL;
    find_meta(dm);
    if (!dm->meta.found)
        RERR(d, "DMUB: no firmware meta info, using default region sizes");
    dm->vbios_size = vbios_size;

    /* dmub_srv_calc_region_info: every window in FB (VRAM) on DCN32 */
    sizes[RD_DMUB_WIN_INST_CONST] = dm->inst_const_size;
    sizes[RD_DMUB_WIN_STACK] = DMUB_STACK_SIZE + DMUB_CONTEXT_SIZE;
    sizes[RD_DMUB_WIN_BSS_DATA] = dm->bss_data_size;
    sizes[RD_DMUB_WIN_VBIOS] = vbios_size;
    sizes[RD_DMUB_WIN_MAILBOX] = DMUB_MAILBOX_SIZE;
    sizes[RD_DMUB_WIN_TRACEBUFF] = dm->meta.found ? dm->meta.trace_buffer_size : DMUB_TRACE_BUFFER_SIZE;
    sizes[RD_DMUB_WIN_FW_STATE] = dm->meta.found ? dm->meta.fw_region_size : DMUB_FW_STATE_SIZE;
    sizes[RD_DMUB_WIN_SCRATCH_MEM] = align_up(DMUB_SCRATCH_MEM_SIZE, 64);
    sizes[RD_DMUB_WIN_IB_MEM] = align_up(DMUB_IB_MEM_SIZE, 64);
    sizes[RD_DMUB_WIN_SHARED_STATE] =
        dm->meta.shared_state_size > DMUB_SHARED_STATE_SIZE ? dm->meta.shared_state_size : DMUB_SHARED_STATE_SIZE;
    sizes[RD_DMUB_WIN_LSDMA_BUFFER] = DMUB_LSDMA_RB_SIZE;
    sizes[RD_DMUB_WIN_CURSOR_OFFLOAD] = align_up(DMUB_CURSOR_OFFLOAD_SIZE, 64);
    for (i = 0; i < RD_DMUB_WIN_TOTAL; i++) {
        dm->region[i].base = align_up(top, 256);
        dm->region[i].top = dm->region[i].base + align_up(sizes[i], 64);
        top = dm->region[i].top;
    }
    dm->fb_size = align_up(top, 4096);

    off = rdna3_vram_alloc(d, dm->fb_size, 4096, "dmub");
    if (off == UINT64_MAX)
        return RDNA3_ENOMEM;
    dm->fb_off = off;
    dm->sw_init = 1;
    RLOG(d, "DMUB: firmware 0x%08x, inst %u B, bss %u B, %u KiB of windows at VRAM 0x%llx%s",
         dm->fw_version, dm->inst_const_size, dm->bss_data_size, dm->fb_size / 1024,
         (unsigned long long)off, dm->meta.found ? "" : " (no meta)");
    return RDNA3_OK;
}

static uint64_t win_off(const rdna3_dmub *dm, unsigned w) { return dm->fb_off + dm->region[w].base; }
static uint32_t win_size(const rdna3_dmub *dm, unsigned w) { return dm->region[w].top - dm->region[w].base; }
static uint64_t win_mc(const rdna3_dmub *dm, unsigned w) { return rdna3_vram_mc(dm->d, win_off(dm, w)); }

/* ---- hardware (dmub_dcn32.c) ---- */

int dmub32_is_supported(rdna3_dev *d)
{
    return GET(RR(d, regCC_DC_PIPE_DIS), CC_DC_PIPE_DIS, DC_DMCUB_ENABLE) != 0;
}

int dmub32_is_hw_init(rdna3_dev *d)
{
    return GET(RR(d, regDMCUB_CNTL), DMCUB_CNTL, DMCUB_ENABLE) &&
           (RR(d, regDMCUB_SCRATCH0) & RD_DMUB_BOOT_DAL_FW);
}

static void dcn32_reset(rdna3_dev *d)
{
    const uint32_t timeout_us = 1000000;
    uint32_t i = 0, enabled, in_reset;

    enabled = GET(RR(d, regDMCUB_CNTL), DMCUB_CNTL, DMCUB_ENABLE);
    in_reset = GET(RR(d, regDMCUB_CNTL2), DMCUB_CNTL2, DMCUB_SOFT_RESET);
    if (enabled && in_reset == 0) {
        /* ask the running firmware to park itself first */
        WR(d, regDMCUB_GPINT_DATAIN1, RD_DMUB_GPINT(RD_DMUB_GPINT_STOP_FW, 0));
        for (; i < timeout_us; i++) {
            if (RR(d, regDMCUB_SCRATCH7) == RD_DMUB_GPINT_STOP_FW_RESPONSE)
                break;
            udelay(d, 1);
        }
        for (; i < timeout_us; i++) {
            if (GET(RR(d, regDMCUB_CNTL), DMCUB_CNTL, DMCUB_PWAIT_MODE_STATUS) & 1)
                break;
            udelay(d, 1);
        }
    }
    if (enabled) {
        UPD(d, DMCUB_CNTL2, DMCUB_SOFT_RESET, 1);
        udelay(d, 1);
        UPD(d, DMCUB_CNTL, DMCUB_ENABLE, 0);
    }
    if (i >= timeout_us)
        RERR(d, "DMUB: firmware did not stop (SCRATCH7 0x%08x)", RR(d, regDMCUB_SCRATCH7));

    UPD(d, DMCUB_REGION3_CW2_TOP_ADDRESS, DMCUB_REGION3_CW2_ENABLE, 0);
    UPD(d, DMCUB_REGION3_CW3_TOP_ADDRESS, DMCUB_REGION3_CW3_ENABLE, 0);
    UPD(d, DMCUB_REGION3_CW4_TOP_ADDRESS, DMCUB_REGION3_CW4_ENABLE, 0);
    UPD(d, DMCUB_REGION3_CW5_TOP_ADDRESS, DMCUB_REGION3_CW5_ENABLE, 0);
    UPD(d, DMCUB_REGION3_CW6_TOP_ADDRESS, DMCUB_REGION3_CW6_ENABLE, 0);
    UPD(d, DMCUB_REGION3_CW7_TOP_ADDRESS, DMCUB_REGION3_CW7_ENABLE, 0);

    WR(d, regDMCUB_INBOX1_RPTR, 0);
    WR(d, regDMCUB_INBOX1_WPTR, 0);
    WR(d, regDMCUB_OUTBOX1_RPTR, 0);
    WR(d, regDMCUB_OUTBOX1_WPTR, 0);
    WR(d, regDMCUB_OUTBOX0_RPTR, 0);
    WR(d, regDMCUB_OUTBOX0_WPTR, 0);
    WR(d, regDMCUB_SCRATCH0, 0);
    /* clear the GPINT command so we do not reset again */
    WR(d, regDMCUB_GPINT_DATAIN1, 0);
}

static void dcn32_reset_release(rdna3_dev *d, uint32_t psp_version)
{
    uint32_t v;
    UPD(d, MMHUBBUB_SOFT_RESET, DMUIF_SOFT_RESET, 0);
    WR(d, regDMCUB_SCRATCH15, psp_version & 0x001100FF);
    v = RR(d, regDMCUB_CNTL);
    v = SETF(v, DMCUB_CNTL, DMCUB_ENABLE, 1);
    v = SETF(v, DMCUB_CNTL, DMCUB_TRACEPORT_EN, 1);
    WR(d, regDMCUB_CNTL, v);
    UPD(d, DMCUB_CNTL2, DMCUB_SOFT_RESET, 0);
}

#define CW_PROGRAM(d, n, off64, base, top)                                                    \
    do {                                                                                      \
        WR(d, regDMCUB_REGION3_CW##n##_OFFSET, (uint32_t)(off64));                             \
        WR(d, regDMCUB_REGION3_CW##n##_OFFSET_HIGH, (uint32_t)((off64) >> 32));                \
        WR(d, regDMCUB_REGION3_CW##n##_BASE_ADDRESS, (base));                                  \
        WR(d, regDMCUB_REGION3_CW##n##_TOP_ADDRESS,                                            \
           SETF(SETF(0, DMCUB_REGION3_CW##n##_TOP_ADDRESS, DMCUB_REGION3_CW##n##_TOP_ADDRESS,  \
                     (top)),                                                                   \
                DMCUB_REGION3_CW##n##_TOP_ADDRESS, DMCUB_REGION3_CW##n##_ENABLE, 1));          \
    } while (0)

static void dcn32_backdoor_load(rdna3_dmub *dm)
{
    rdna3_dev *d = dm->d;
    /* dmub_dcn32_translate_addr: MC address -> FB physical */
    uint64_t fb_base = d->fb_start, fb_offset = d->fb_offset;
    uint64_t cw0 = win_mc(dm, RD_DMUB_WIN_INST_CONST) - fb_base + fb_offset;
    uint64_t cw1 = win_mc(dm, RD_DMUB_WIN_STACK) - fb_base + fb_offset;
    uint32_t v;

    UPD(d, DMCUB_SEC_CNTL, DMCUB_SEC_RESET, 1);
    UPD(d, DMCUB_CNTL, DMCUB_ENABLE, 0);
    CW_PROGRAM(d, 0, cw0, RD_DMUB_CW0_BASE, RD_DMUB_CW0_BASE + win_size(dm, RD_DMUB_WIN_INST_CONST) - 1);
    CW_PROGRAM(d, 1, cw1, RD_DMUB_CW1_BASE, RD_DMUB_CW1_BASE + win_size(dm, RD_DMUB_WIN_STACK) - 1);
    v = RR(d, regDMCUB_SEC_CNTL);
    v = SETF(v, DMCUB_SEC_CNTL, DMCUB_SEC_RESET, 0);
    v = SETF(v, DMCUB_SEC_CNTL, DMCUB_MEM_UNIT_ID, 0x20);
    WR(d, regDMCUB_SEC_CNTL, v);
}

static void dcn32_setup_windows(rdna3_dmub *dm)
{
    rdna3_dev *d = dm->d;
    uint64_t cw5 = win_mc(dm, RD_DMUB_WIN_TRACEBUFF);
    uint32_t cw5_size = win_size(dm, RD_DMUB_WIN_TRACEBUFF);

    CW_PROGRAM(d, 3, win_mc(dm, RD_DMUB_WIN_VBIOS), RD_DMUB_CW3_BASE,
               RD_DMUB_CW3_BASE + win_size(dm, RD_DMUB_WIN_VBIOS));
    CW_PROGRAM(d, 4, win_mc(dm, RD_DMUB_WIN_MAILBOX), RD_DMUB_CW4_BASE,
               RD_DMUB_CW4_BASE + win_size(dm, RD_DMUB_WIN_MAILBOX));
    CW_PROGRAM(d, 5, cw5, RD_DMUB_CW5_BASE, RD_DMUB_CW5_BASE + cw5_size);
    WR(d, regDMCUB_REGION5_OFFSET, (uint32_t)cw5);
    WR(d, regDMCUB_REGION5_OFFSET_HIGH, (uint32_t)(cw5 >> 32));
    WR(d, regDMCUB_REGION5_TOP_ADDRESS,
       SETF(SETF(0, DMCUB_REGION5_TOP_ADDRESS, DMCUB_REGION5_TOP_ADDRESS, cw5_size - 1),
            DMCUB_REGION5_TOP_ADDRESS, DMCUB_REGION5_ENABLE, 1));
    CW_PROGRAM(d, 6, win_mc(dm, RD_DMUB_WIN_FW_STATE), RD_DMUB_CW6_BASE,
               RD_DMUB_CW6_BASE + win_size(dm, RD_DMUB_WIN_FW_STATE));
}

/* What the PSP and the firmware left: code/stack windows (programmed by
 * the PSP when it loads the DMCUB, never by the driver), the fault
 * addresses and whether the microcontroller's timer is running. */
void dmub32_log_state(rdna3_dmub *dm, const char *when)
{
    dmub32_log_regs(dm->d, when);
}

void dmub32_log_regs(rdna3_dev *d, const char *when)
{
    uint32_t t0 = RR(d, regDMCUB_TIMER_CURRENT), t1;
    udelay(d, 10);
    t1 = RR(d, regDMCUB_TIMER_CURRENT);
    RLOG(d, "DMUB state (%s): CNTL 0x%08x CNTL2 0x%08x SEC_CNTL 0x%08x SCRATCH0 0x%08x 7 0x%08x "
         "14 0x%08x 15 0x%08x timer %u -> %u", when, RR(d, regDMCUB_CNTL), RR(d, regDMCUB_CNTL2),
         RR(d, regDMCUB_SEC_CNTL), RR(d, regDMCUB_SCRATCH0), RR(d, regDMCUB_SCRATCH7),
         RR(d, regDMCUB_SCRATCH14), RR(d, regDMCUB_SCRATCH15), t0, t1);
    RLOG(d, "DMUB state (%s): CW0 off 0x%08x%08x base 0x%08x top 0x%08x, CW1 off 0x%08x%08x "
         "base 0x%08x top 0x%08x", when, RR(d, regDMCUB_REGION3_CW0_OFFSET_HIGH),
         RR(d, regDMCUB_REGION3_CW0_OFFSET), RR(d, regDMCUB_REGION3_CW0_BASE_ADDRESS),
         RR(d, regDMCUB_REGION3_CW0_TOP_ADDRESS), RR(d, regDMCUB_REGION3_CW1_OFFSET_HIGH),
         RR(d, regDMCUB_REGION3_CW1_OFFSET), RR(d, regDMCUB_REGION3_CW1_BASE_ADDRESS),
         RR(d, regDMCUB_REGION3_CW1_TOP_ADDRESS));
    RLOG(d, "DMUB state (%s): faults inst 0x%08x data-write 0x%08x undefined 0x%08x, "
         "CW4 off 0x%08x base 0x%08x", when, RR(d, regDMCUB_INST_FETCH_FAULT_ADDR),
         RR(d, regDMCUB_DATA_WRITE_FAULT_ADDR), RR(d, regDMCUB_UNDEFINED_ADDRESS_FAULT_ADDR),
         RR(d, regDMCUB_REGION3_CW4_OFFSET), RR(d, regDMCUB_REGION3_CW4_BASE_ADDRESS));
}

int dmub32_wait_boot(rdna3_dmub *dm, uint32_t timeout_us)
{
    uint32_t i, st = 0;
    for (i = 0; i <= timeout_us; i += 100) {
        st = RR(dm->d, regDMCUB_SCRATCH0);
        if ((st & RD_DMUB_BOOT_DAL_FW) && (st & RD_DMUB_BOOT_MAILBOX_RDY)) {
            dm->boot_status = st;
            dm->hw_init = 1;
            return RDNA3_OK;
        }
        udelay(dm->d, 100);
    }
    dm->boot_status = st;
    return RDNA3_ETIME;
}

void dmub32_hw_reset(rdna3_dmub *dm)
{
    dcn32_reset(dm->d);
    dm->inbox1_wptr = dm->inbox1_rptr = dm->inbox1_last_wptr = 0;
    dm->submitted = dm->reported = 0;
    dm->hw_init = 0;
}

int dmub32_hw_init(rdna3_dmub *dm, const uint8_t *vbios, uint32_t vbios_len, int psp_loaded,
                   uint32_t timeout_us)
{
    rdna3_dev *d = dm->d;
    uint32_t i, st = 0, trace_size;

    if (!dm->sw_init)
        return RDNA3_EINVAL;
    if (!dmub32_is_supported(d)) {
        RLOG(d, "DMUB: not enabled on this ASIC (CC_DC_PIPE_DIS)");
        return RDNA3_ENODEV;
    }
    /* dm_dmub_hw_init: reset if it was running, before its memory changes */
    dmub32_log_state(dm, "before reset");
    dmub32_hw_reset(dm);
    dm->psp_loaded = psp_loaded;

    if (!psp_loaded)
        rdna3_vram_write(d, win_off(dm, RD_DMUB_WIN_INST_CONST), dm->fw_inst_const, dm->inst_const_size);
    if (dm->bss_data_size)
        rdna3_vram_write(d, win_off(dm, RD_DMUB_WIN_BSS_DATA), dm->fw_bss_data, dm->bss_data_size);
    if (vbios && vbios_len)
        rdna3_vram_write(d, win_off(dm, RD_DMUB_WIN_VBIOS), vbios,
                         vbios_len < dm->vbios_size ? vbios_len : dm->vbios_size);
    rdna3_vram_fill(d, win_off(dm, RD_DMUB_WIN_MAILBOX), 0, win_size(dm, RD_DMUB_WIN_MAILBOX));
    rdna3_vram_fill(d, win_off(dm, RD_DMUB_WIN_TRACEBUFF), 0, win_size(dm, RD_DMUB_WIN_TRACEBUFF));
    rdna3_vram_fill(d, win_off(dm, RD_DMUB_WIN_FW_STATE), 0, win_size(dm, RD_DMUB_WIN_FW_STATE));
    rdna3_vram_fill(d, win_off(dm, RD_DMUB_WIN_SHARED_STATE), 0, win_size(dm, RD_DMUB_WIN_SHARED_STATE));
    rdna3_hdp_flush(d);

    /* dmub_srv_hw_init */
    dcn32_reset(d);
    dm->inbox1_last_wptr = 0;
    if (!psp_loaded)
        dcn32_backdoor_load(dm);
    dcn32_setup_windows(dm);
    trace_size = win_size(dm, RD_DMUB_WIN_TRACEBUFF);
    /* outbox0 = the trace buffer after its header entry */
    WR(d, regDMCUB_OUTBOX0_BASE_ADDRESS, RD_DMUB_REGION5_BASE + RD_DMUB_TRACE_ENTRY_OFFSET);
    WR(d, regDMCUB_OUTBOX0_SIZE, trace_size - RD_DMUB_TRACE_ENTRY_OFFSET);
    /* inbox1 / outbox1: the two halves of the mailbox window */
    WR(d, regDMCUB_INBOX1_BASE_ADDRESS, RD_DMUB_CW4_BASE);
    WR(d, regDMCUB_INBOX1_SIZE, RD_DMUB_RB_SIZE);
    WR(d, regDMCUB_OUTBOX1_BASE_ADDRESS, RD_DMUB_CW4_BASE + RD_DMUB_RB_SIZE);
    WR(d, regDMCUB_OUTBOX1_SIZE, RD_DMUB_RB_SIZE);
    /* boot options (z10_disable = 0), then skip_phy_init_panel_sequence = 0 */
    WR(d, regDMCUB_SCRATCH14, 0);
    dcn32_reset_release(d, 0);
    dm->hw_init = 1;

    /* dmub_srv_wait_for_auto_load */
    for (i = 0; i <= timeout_us; i += 100) {
        st = RR(d, regDMCUB_SCRATCH0);
        if ((st & RD_DMUB_BOOT_DAL_FW) && (st & RD_DMUB_BOOT_MAILBOX_RDY))
            break;
        udelay(d, 100);
    }
    dm->boot_status = st;
    if (!((st & RD_DMUB_BOOT_DAL_FW) && (st & RD_DMUB_BOOT_MAILBOX_RDY))) {
        RERR(d, "DMUB: auto-load timed out (SCRATCH0 0x%08x, CNTL 0x%08x, SEC_CNTL 0x%08x)", st,
             RR(d, regDMCUB_CNTL), RR(d, regDMCUB_SEC_CNTL));
        dmub32_log_state(dm, "auto-load timeout");
        return RDNA3_ETIME;
    }
    RLOG(d, "DMUB: firmware 0x%08x running, boot status 0x%x", dm->fw_version, st);
    return RDNA3_OK;
}

/* ---- inbox1 ring (dmub_rb_*, dmub_srv_fb_cmd_queue/execute, wait_for_idle) ---- */

static uint32_t rb_size(const rdna3_dmub *dm) { return dm->rb_size ? dm->rb_size : RD_DMUB_RB_SIZE; }

static int rb_full(const rdna3_dmub *dm)
{
    uint32_t count = dm->inbox1_wptr >= dm->inbox1_rptr
                         ? dm->inbox1_wptr - dm->inbox1_rptr
                         : rb_size(dm) - (dm->inbox1_rptr - dm->inbox1_wptr);
    return count == rb_size(dm) - RD_DMUB_RB_CMD_SIZE;
}

static int update_rptr(rdna3_dmub *dm)
{
    uint32_t rptr = RR(dm->d, regDMCUB_INBOX1_RPTR);
    if (rptr > rb_size(dm))
        return RDNA3_EIO;
    if (dm->inbox1_rptr > rptr)
        dm->reported += (rptr + rb_size(dm) - dm->inbox1_rptr) / RD_DMUB_RB_CMD_SIZE;
    else
        dm->reported += (rptr - dm->inbox1_rptr) / RD_DMUB_RB_CMD_SIZE;
    dm->inbox1_rptr = rptr;
    return RDNA3_OK;
}

int dmub32_cmd_execute(rdna3_dmub *dm, uint8_t cmd[RD_DMUB_RB_CMD_SIZE], int reply,
                       uint32_t timeout_us)
{
    rdna3_dev *d = dm->d;
    uint64_t mb = dm->attached ? dm->inbox_vram : win_off(dm, RD_DMUB_WIN_MAILBOX);
    uint32_t slot, i, w[RD_DMUB_RB_CMD_SIZE / 4];
    int ret, bar;

    if (!dm->hw_init)
        return RDNA3_EIO;
    if ((ret = update_rptr(dm)) != RDNA3_OK)
        return ret;
    if (dm->inbox1_wptr > rb_size(dm))
        return RDNA3_EIO;
    if (rb_full(dm))
        return RDNA3_EBUSY;
    slot = dm->inbox1_wptr;
    bar = rdna3_vram_cpu(d, mb + slot, RD_DMUB_RB_CMD_SIZE) != NULL;
    if (bar) {
        rdna3_vram_write(d, mb + slot, cmd, RD_DMUB_RB_CMD_SIZE);
    } else {
        for (i = 0; i < RD_DMUB_RB_CMD_SIZE / 4; i++)
            w[i] = rd32le(cmd + 4 * i);
        rdna3_vram_mm_write(d, mb + slot, w, RD_DMUB_RB_CMD_SIZE / 4);
    }
    dm->inbox1_wptr = (dm->inbox1_wptr + RD_DMUB_RB_CMD_SIZE) % rb_size(dm);
    dm->submitted++;
    /* the DMCUB reads the ring from VRAM: make the BAR writes land first */
    rdna3_hdp_flush(d);
    WR(d, regDMCUB_INBOX1_WPTR, dm->inbox1_wptr);
    dm->inbox1_last_wptr = dm->inbox1_wptr;

    for (i = 0; i < timeout_us; i++) {
        if ((ret = update_rptr(dm)) != RDNA3_OK)
            return ret;
        if (dm->inbox1_rptr == dm->inbox1_wptr)
            break;
        udelay(d, 1);
    }
    if (dm->inbox1_rptr != dm->inbox1_wptr) {
        RERR(d, "DMUB: command 0x%08x not consumed (rptr 0x%x wptr 0x%x, SCRATCH0 0x%08x)",
             rd32le(cmd), dm->inbox1_rptr, dm->inbox1_wptr, RR(d, regDMCUB_SCRATCH0));
        return RDNA3_ETIME;
    }
    if (reply && bar) {
        rdna3_vram_read(d, mb + slot, cmd, RD_DMUB_RB_CMD_SIZE);
    } else if (reply) {
        rdna3_vram_mm_read(d, mb + slot, w, RD_DMUB_RB_CMD_SIZE / 4);
        for (i = 0; i < RD_DMUB_RB_CMD_SIZE; i++)
            cmd[i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
    }
    return RDNA3_OK;
}

#define AMASK ((uint32_t)DMCUB_REGION3_CW0_BASE_ADDRESS__DMCUB_REGION3_CW0_BASE_ADDRESS_MASK)
int dmub32_attach(rdna3_dmub *dm, rdna3_dev *d)
{
    static const struct { uint32_t off, off_hi, base, top; } cw[8] = {
        { regDMCUB_REGION3_CW0_OFFSET, regDMCUB_REGION3_CW0_OFFSET_HIGH, regDMCUB_REGION3_CW0_BASE_ADDRESS, regDMCUB_REGION3_CW0_TOP_ADDRESS },
        { regDMCUB_REGION3_CW1_OFFSET, regDMCUB_REGION3_CW1_OFFSET_HIGH, regDMCUB_REGION3_CW1_BASE_ADDRESS, regDMCUB_REGION3_CW1_TOP_ADDRESS },
        { regDMCUB_REGION3_CW2_OFFSET, regDMCUB_REGION3_CW2_OFFSET_HIGH, regDMCUB_REGION3_CW2_BASE_ADDRESS, regDMCUB_REGION3_CW2_TOP_ADDRESS },
        { regDMCUB_REGION3_CW3_OFFSET, regDMCUB_REGION3_CW3_OFFSET_HIGH, regDMCUB_REGION3_CW3_BASE_ADDRESS, regDMCUB_REGION3_CW3_TOP_ADDRESS },
        { regDMCUB_REGION3_CW4_OFFSET, regDMCUB_REGION3_CW4_OFFSET_HIGH, regDMCUB_REGION3_CW4_BASE_ADDRESS, regDMCUB_REGION3_CW4_TOP_ADDRESS },
        { regDMCUB_REGION3_CW5_OFFSET, regDMCUB_REGION3_CW5_OFFSET_HIGH, regDMCUB_REGION3_CW5_BASE_ADDRESS, regDMCUB_REGION3_CW5_TOP_ADDRESS },
        { regDMCUB_REGION3_CW6_OFFSET, regDMCUB_REGION3_CW6_OFFSET_HIGH, regDMCUB_REGION3_CW6_BASE_ADDRESS, regDMCUB_REGION3_CW6_TOP_ADDRESS },
        { regDMCUB_REGION3_CW7_OFFSET, regDMCUB_REGION3_CW7_OFFSET_HIGH, regDMCUB_REGION3_CW7_BASE_ADDRESS, regDMCUB_REGION3_CW7_TOP_ADDRESS },
    };
    const uint32_t seg = regDMCUB_INBOX1_BASE_ADDRESS_BASE_IDX;   /* all DMCUB registers: segment 2 */
    uint32_t inbox, size, i, n;
    for (i = 0; i < sizeof(*dm); i++)
        ((uint8_t *)dm)[i] = 0;
    dm->d = d;
    if (!dmub32_is_supported(d) || !GET(RR(d, regDMCUB_CNTL), DMCUB_CNTL, DMCUB_ENABLE) ||
        GET(RR(d, regDMCUB_CNTL2), DMCUB_CNTL2, DMCUB_SOFT_RESET)) {
        RERR(d, "DMUB attach: no running DMCUB (CNTL 0x%08x CNTL2 0x%08x)", RR(d, regDMCUB_CNTL),
             RR(d, regDMCUB_CNTL2));
        return RDNA3_ENODEV;
    }
    inbox = RR(d, regDMCUB_INBOX1_BASE_ADDRESS);
    size = RR(d, regDMCUB_INBOX1_SIZE);
    dm->boot_status = RR(d, regDMCUB_SCRATCH0);
    if (!size || size % RD_DMUB_RB_CMD_SIZE || size > (1u << 20)) {
        RERR(d, "DMUB attach: inbox1 at 0x%08x has size 0x%x", inbox, size);
        return RDNA3_EINVAL;
    }
    for (n = 0; n < 8; n++) {
        uint32_t base = rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, cw[n].base));
        uint32_t top = rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, cw[n].top));
        uint64_t off;
        if (!(top & DMCUB_REGION3_CW0_TOP_ADDRESS__DMCUB_REGION3_CW0_ENABLE_MASK))
            continue;
        /* BASE and TOP keep the low 29 bits of the DMCUB address (the
         * 0x6n000000 window bases written by dmub_dcn32_setup_windows are
         * truncated): compare in that space */
        base &= DMCUB_REGION3_CW0_BASE_ADDRESS__DMCUB_REGION3_CW0_BASE_ADDRESS_MASK;
        top &= DMCUB_REGION3_CW0_TOP_ADDRESS__DMCUB_REGION3_CW0_TOP_ADDRESS_MASK;
        if ((inbox & AMASK) < base || (inbox & AMASK) + size - 1 > top)
            continue;
        off = rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, cw[n].off)) |
              ((uint64_t)rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, cw[n].off_hi)) << 32);
        /* dmub_dcn32_setup_windows writes MC addresses for CW3-CW6; only
         * the backdoor CW0/CW1 are translated (MC - fb_base + fb_offset) */
        if (off >= d->fb_start && off - d->fb_start < d->vram_size)
            off -= d->fb_start;
        else if (off >= d->fb_offset && off - d->fb_offset < d->vram_size)
            off -= d->fb_offset;
        else
            off = UINT64_MAX;
        if (off == UINT64_MAX || off + ((inbox & AMASK) - base) + size > d->vram_size) {
            RERR(d, "DMUB attach: window CW%u is not in VRAM", n);
            return RDNA3_EINVAL;
        }
        dm->inbox_vram = off + ((inbox & AMASK) - base);
        break;
    }
    if (n == 8) {
        RERR(d, "DMUB attach: inbox1 at 0x%08x is behind no enabled code window", inbox);
        return RDNA3_EINVAL;
    }
    dm->rb_size = size;
    dm->inbox1_rptr = RR(d, regDMCUB_INBOX1_RPTR);
    dm->inbox1_wptr = RR(d, regDMCUB_INBOX1_WPTR);
    for (i = 0; i < 100000 && dm->inbox1_rptr != dm->inbox1_wptr; i++) {
        udelay(d, 1);
        dm->inbox1_rptr = RR(d, regDMCUB_INBOX1_RPTR);
    }
    if (dm->inbox1_rptr != dm->inbox1_wptr || dm->inbox1_wptr >= size) {
        RERR(d, "DMUB attach: inbox1 busy (rptr 0x%x wptr 0x%x size 0x%x)", dm->inbox1_rptr,
             dm->inbox1_wptr, size);
        return RDNA3_EBUSY;
    }
    dm->inbox1_last_wptr = dm->inbox1_wptr;
    dm->attached = 1;
    dm->hw_init = 1;
    RLOG(d, "DMUB attach: the GOP's firmware (SCRATCH0 0x%08x), inbox1 0x%08x+0x%x = VRAM 0x%llx "
         "behind CW%u, wptr 0x%x", dm->boot_status, inbox, size, (unsigned long long)dm->inbox_vram, n,
         dm->inbox1_wptr);
    return RDNA3_OK;
}

int dmub32_query_caps(rdna3_dmub *dm)
{
    uint8_t cmd[RD_DMUB_RB_CMD_SIZE];
    uint32_t i, hdr;
    int ret;
    for (i = 0; i < sizeof(cmd); i++)
        cmd[i] = 0;
    /* struct dmub_feature_caps: 16 bytes */
    hdr = RD_DMUB_HDR(RD_DMUB_CMD_QUERY_FEATURE_CAPS, 0, 16, 1);
    cmd[0] = (uint8_t)hdr; cmd[1] = (uint8_t)(hdr >> 8); cmd[2] = (uint8_t)(hdr >> 16); cmd[3] = (uint8_t)(hdr >> 24);
    ret = dmub32_cmd_execute(dm, cmd, 1, 100000);
    if (ret != RDNA3_OK)
        return ret;
    /* the firmware clears ret_status once it filled the reply */
    if (rd32le(cmd) & RD_DMUB_HDR_RET_STATUS)
        return RDNA3_EIO;
    for (i = 0; i < 4; i++)
        dm->feature_caps[i] = rd32le(cmd + 4 + 4 * i);
    return RDNA3_OK;
}

int dmub32_vbios_cmd(rdna3_dmub *dm, uint32_t sub_type, const void *payload, uint32_t len)
{
    uint8_t cmd[RD_DMUB_RB_CMD_SIZE];
    uint32_t i, hdr;
    if (len > RD_DMUB_RB_CMD_SIZE - 4)
        return RDNA3_EINVAL;
    for (i = 0; i < sizeof(cmd); i++)
        cmd[i] = 0;
    hdr = RD_DMUB_HDR(RD_DMUB_CMD_VBIOS, sub_type, len, 0);
    cmd[0] = (uint8_t)hdr; cmd[1] = (uint8_t)(hdr >> 8); cmd[2] = (uint8_t)(hdr >> 16); cmd[3] = (uint8_t)(hdr >> 24);
    for (i = 0; i < len; i++)
        cmd[4 + i] = ((const uint8_t *)payload)[i];
    return dmub32_cmd_execute(dm, cmd, 0, 100000);
}

int dmub32_transmitter_control(rdna3_dmub *dm, const rdna3_dig_tx_v1_7 *p)
{
    /* the payload is the union of the v1.6/v1.7 parameter blocks: the
     * whole 60 bytes after the header, as sizeof(cmd) - sizeof(header) */
    uint8_t b[RD_DMUB_RB_CMD_SIZE - 4];
    uint32_t i;
    for (i = 0; i < sizeof(b); i++)
        b[i] = 0;
    b[0] = p->phyid;
    b[1] = p->action;
    b[2] = p->digmode;
    b[3] = p->lanenum;
    b[4] = (uint8_t)p->symclk_10khz;
    b[5] = (uint8_t)(p->symclk_10khz >> 8);
    b[6] = (uint8_t)(p->symclk_10khz >> 16);
    b[7] = (uint8_t)(p->symclk_10khz >> 24);
    b[8] = p->hpdsel;
    b[9] = p->digfe_sel;
    b[10] = p->connobj_id;
    b[11] = p->hpo_instance;
    b[12] = p->txffe_lane_sel;
    b[13] = p->skip_phy_ssc_reduction;
    return dmub32_vbios_cmd(dm, RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL, b, sizeof(b));
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

int dmub32_set_pixel_clock(rdna3_dmub *dm, const rdna3_pixclk_v1_7 *p)
{
    /* payload_bytes = sizeof(struct set_pixel_clock_parameter_v1_7) = 16 */
    uint8_t b[16];
    uint32_t i;
    for (i = 0; i < sizeof(b); i++)
        b[i] = 0;
    put32(b, p->pixclk_100hz);
    b[4] = p->pll_id;
    b[5] = p->encoderobjid;
    b[6] = p->encoder_mode;
    b[7] = p->miscinfo;
    b[8] = p->crtc_id;
    b[9] = p->deep_color_ratio;
    return dmub32_vbios_cmd(dm, RD_DMUB_VBIOS_SET_PIXEL_CLOCK, b, sizeof(b));
}

int dmub32_encoder_control(rdna3_dmub *dm, const rdna3_dig_enc_v1_5 *p)
{
    /* payload_bytes = sizeof(union dig_encoder_control_parameters_v1_5) = 12 */
    uint8_t b[12];
    uint32_t i;
    for (i = 0; i < sizeof(b); i++)
        b[i] = 0;
    b[0] = p->digid;
    b[1] = p->action;
    b[2] = p->digmode;
    b[3] = p->lanenum;
    put32(b + 4, p->pclk_10khz);
    b[8] = p->bitpercolor;
    b[9] = p->dplinkrate_270mhz;
    return dmub32_vbios_cmd(dm, RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL, b, sizeof(b));
}

uint32_t dmub32_trace_wptr(rdna3_dmub *dm)
{
    return RR(dm->d, regDMCUB_OUTBOX0_WPTR);
}
