/*
 * sim_navi32.c - behavioural model of the Navi 32 registers that Stage 2
 * bring-up touches, plus an end-to-end test of hw/psp13.c + hw/smu13.c.
 *
 * The model follows the protocol as amdgpu drives it (not AMD internals):
 *   - PSP bootloader: C2PMSG_35 bit31 = ready; a write of a command loads
 *     the bytes at MC(C2PMSG_36 << 20) and sets bit31 again. SOSDRV makes
 *     the sOS alive (C2PMSG_81 != 0) and ready for ring creation
 *     (C2PMSG_64 = 0x80000000).
 *   - sOS: C2PMSG_64 = KM<<16 creates the ring at C2PMSG_69/70; each
 *     C2PMSG_67 (wptr) write consumes one psp_gfx_rb_frame, executes the
 *     command found at its MC address and writes the fence.
 *   - PMFW (MP1) answers mailbox messages once LOAD_IP_FW(SMU) happened.
 * It checks the ordering rules amdgpu relies on and that the CPU flushed
 * HDP before every doorbell-like register write.
 *
 *   sim_navi32 [linux-firmware/amdgpu dir]
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "psp13.h"
#include "smu13.h"
#include "vbios.h"
#include "dcaux.h"
#include "edid.h"
#include "gen/dcn_3_2_1_aux.h"
#include "gmc11.h"
#include "gen/gc_11_0_3_gmc.h"
#include "gen/mmhub_3_0_2.h"
#include "ih60.h"
#include "gen/osssys_6_0_0.h"
#include "sdma60.h"
#include "gpuvm.h"
#include "gen/gc_11_0_0_sdma.h"
#include "gfx11.h"
#include "dcn32.h"
#include "doorbell.h"
#include "gen/nbio_4_3_0_doorbell.h"
#include "gen/dcn_3_2_1.h"
#include "gen/gc_11_0_0_gfx.h"
#include "gen/gc_11_0_0_reset.h"
#include "gen/gc_11_0_3_gmc.h"
#include "gen/mmhub_3_0_2.h"
#include "navi3x_regs.h"
#include "psp_gfx_if.h"
#include "amdgpu_ucode_hdr.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

/* ----------------------------- model ----------------------------- */

#define MMIO_LEN   (1u << 20)
#define VRAM_SIZE  (16ull << 30)       /* what MEMSIZE reports */
#define BAR_LEN    (64u << 20)         /* CPU-visible window we model */
#define FB_START   0x8000000000ull     /* MC base like real dGPUs */

#define MP0_BASE   0x16000u
#define MP1_BASE   0x16000u
#define GC_BASE0   0x1260u
#define MM_BASE0   0x1A000u
/* absolute dword of a GC register from its BASE_IDX (segments 0 / 1) */
#define GCR(reg) ((reg##_BASE_IDX ? 0xA000u : GC_BASE0) + reg)
#define OSS_BASE0  0x4260u
#define NBIO_B0    0x0u
#define NBIO_B1    0x14u
#define NBIO_B2    0xD20u
#define NBIO_B5    0x04040000u /* beyond the BAR: reached through the RSMU window */
#define SMUIO_B0   0x16800u
#define ROM_STRAP  1u          /* REGS_ROM_OFFSET_CTRL.ROM_OFFSET */
#define SMN_ROM_OFFSET_CTRL ((NBIO_B5 + regREGS_ROM_OFFSET_CTRL) * 4u)
#define VBIOS_LEN  8192u
#define DCN_B2     0x34C0u   /* DCN segment 2 (dcn_3_2_1 DCN_BASE__INST0_SEG2) */
#define HUBP(reg, n) (DCN_B2 + reg + (regHUBP1_DCHUBP_CNTL - regHUBP0_DCHUBP_CNTL) * (n))
#define OTG(reg, n)  (DCN_B2 + reg + (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL) * (n))

typedef struct sim {
    uint32_t reg[MMIO_LEN / 4];
    uint8_t *vram;
    int verbose;

    /* bootloader log */
    uint32_t bl_cmds[16];
    unsigned nbl;
    int sos_alive;

    /* sOS */
    uint64_t ring_mc;
    uint32_t rptr_dw;
    uint32_t ip_fw_types[64];
    unsigned nipfw;
    int tmr_setup, toc_loaded, autoload, autoload_after_rlc_g, smu_loaded;
    int tmr_after_smu;
    int pending_writes;   /* CPU VRAM writes not yet flushed via HDP */
    int unflushed_doorbells;
    uint32_t smu_msgs;
    /* virtual time: the sOS needs ~50 ms to boot after SOSDRV, so that
     * psp_v13_0_bootloader_load_sos()'s "wait for C2PMSG_81 to change"
     * (read after a 20 ms delay) sees the transition like on hardware */
    uint64_t now_us;
    uint64_t sos_boot_at;
    unsigned tlb_flushes;
    unsigned mm_tlb_flushes, mm_sem_held, mm_flush_without_sem, mm_private_inv;
    uint64_t sdma_rptr;         /* bytes */
    uint64_t cp_rptr;           /* dwords */
    unsigned cp_pkts, cp_bad, cp_while_halted, cp_clear_state, cp_ctx_regs;
    unsigned cp_wait_stuck, cp_acquire, cp_pfp_sync, cp_frame_begin, cp_frame_end, cp_ibs;
    unsigned cp_ib_dw, cp_ib_fault, ring_tlb_flushes, hdp_engine_flushes;
    uint32_t cp_last_cntxcntl, cp_last_ib_vmid;
    unsigned dcn_flips, dcn_pitch_unlocked;
    unsigned ih_ivs, ih_msi, ih_dropped;   /* IVs the "hardware" queued, MSIs it raised */

    /* RSMU index/data window: registers outside the MMIO BAR (S2A doorbell ranges) */
    uint32_t smn_index;
    struct { uint32_t addr, val; } smn[64];
    unsigned nsmn;

    /* doorbells: 64-bit writes into BAR2, dispatched to the engine whose
     * doorbell register names the index */
    unsigned db_writes, db_unclaimed;
    uint32_t db_last_index;

    /* MEC: one HQD register file per (pipe, queue) of MEC1, selected by
     * GRBM_GFX_CNTL for register access, loaded from the MQD by MAP_QUEUES */
    struct hqd {
        uint32_t reg[64];     /* CP_MQD_BASE_ADDR .. CP_HQD_PQ_WPTR_HI window */
        uint64_t rptr;        /* dwords */
        int mapped_by_kiq;
    } hqd[4][4];
    unsigned kiq_maps, kiq_unmaps, mec_runs, mec_bad_queue;
    uint64_t set_resources_mask;
    uint32_t cp_cur_me, cp_cur_pipe, cp_cur_queue;   /* engine running cp_exec */
    int dcn_hold;              /* 1: flips stay pending (no vupdate yet) */
    /* DP AUX engine 0 with a sink behind it (DPCD, EDID at I2C 0x50); the
     * other engines answer with a receive timeout */
    uint8_t  aux_tx[20], aux_rx[20];
    uint32_t aux_ptr, aux_nrx, aux_status, aux_arb;
    unsigned aux_gos, aux_bad;
    uint8_t  dpcd[0x800];
    uint8_t  edid[256];
    uint32_t i2c_off, i2c_seg;
    /* recovery: SQ_CMD kills, RLC safe mode, GRBM soft resets, mode-1 resets,
     * page-fault IVs */
    unsigned sq_kills, sq_outside_safe, soft_resets, mode1_resets, fault_ivs;
    uint32_t sq_last_vmid;
    int rlc_safe;

    /* VBIOS behind the SMUIO ROM window (auto-incrementing index) */
    uint8_t  vbios[VBIOS_LEN];
    uint32_t rom_index;
    unsigned rom_reads;
    /* PMFW: what the driver programmed, and the tables it exchanges */
    struct {
        uint64_t driver_addr, tool_addr;
        uint32_t allowed_lo, allowed_hi;
        uint64_t enabled;
        int pptable_written, btc, bad_board, unload;
        unsigned unflushed, bad_addr, pcie_overrides, soft_limits, xfers;
        uint32_t last_pcie_arg, workload, ppt_limit;
        PPTable_t pptable;    /* as written by the driver */
    } smu;
    unsigned sdma_pkts, sdma_bad, sdma_while_halted;
} sim;

static sim S;

static uint32_t *R(uint32_t dw) { return &S.reg[dw]; }
static uint32_t *smn_slot(uint32_t addr, int create);
#define MP0(n) (MP0_BASE + regMP0_SMN_C2PMSG_##n)
#define MP1(n) (MP1_BASE + regMP1_SMN_C2PMSG_##n)

static uint8_t *mc_ptr(uint64_t mc, uint64_t len)
{
    if (mc < FB_START || mc - FB_START + len > BAR_LEN)
        return NULL;
    return S.vram + (mc - FB_START);
}

/* ---- VBIOS and PMFW tables ---- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

#define VB_ROM_HDR   0x100
#define VB_MSG       0x80
#define VB_DATA_DIR  0x200
#define VB_FWINFO    0x300
#define VB_SMCDPM    0x400
#define VB_SMUINFO   0x700
#define VB_VRAMUSAGE 0x840
#define VB_BOARD_VERSION 0xB0A2Du
#define PMFW_SKU_VERSION 0x12345u
#define PMFW_BOARD_PLACEHOLDER 0xDEADu
/* features this PMFW model can run: DCN DPM needs a display driver it does not have */
#define SIM_SMU_SUPPORTED ((RD_SMU_FEATURES_AMDGPU | RD_SMU_FEATURE(FEATURE_GFXOFF_BIT)) & \
                           ~RD_SMU_FEATURE(FEATURE_DPM_DCN_BIT))

static void make_vbios(void)
{
    uint8_t *b = S.vbios;
    BoardTable_t bt;
    memset(b, 0, VBIOS_LEN);
    b[0] = 0x55; b[1] = 0xAA; b[2] = VBIOS_LEN / 512;
    memcpy(b + 0x30, " 761295520", 10);
    put16(b + 0x48, VB_ROM_HDR);
    memcpy(b + VB_MSG, "\r\nRX 7800 XT SIM\0", 17);
    /* atom_rom_header_v2_2 */
    put16(b + VB_ROM_HDR, 0x24); b[VB_ROM_HDR + 2] = 2; b[VB_ROM_HDR + 3] = 2;
    memcpy(b + VB_ROM_HDR + 4, "ATOM", 4);
    put16(b + VB_ROM_HDR + 0x10, VB_MSG);
    put16(b + VB_ROM_HDR + 0x20, VB_DATA_DIR);
    /* atom_master_data_table_v2_1 */
    put16(b + VB_DATA_DIR, 4 + 2 * RD_ATOM_DT_COUNT); b[VB_DATA_DIR + 2] = 2; b[VB_DATA_DIR + 3] = 1;
    put16(b + VB_DATA_DIR + 4 + 2 * RD_ATOM_DT_FIRMWAREINFO, VB_FWINFO);
    put16(b + VB_DATA_DIR + 4 + 2 * RD_ATOM_DT_SMC_DPM_INFO, VB_SMCDPM);
    put16(b + VB_DATA_DIR + 4 + 2 * RD_ATOM_DT_SMU_INFO, VB_SMUINFO);
    put16(b + VB_DATA_DIR + 4 + 2 * RD_ATOM_DT_VRAM_USAGEBYFIRMWARE, VB_VRAMUSAGE);
    /* atom_firmware_info_v3_4 */
    put16(b + VB_FWINFO, 108); b[VB_FWINFO + 2] = 3; b[VB_FWINFO + 3] = 4;
    put32(b + VB_FWINFO + 4, 0x02000f);
    put32(b + VB_FWINFO + 8, 50000);    /* 500 MHz boot gfxclk */
    put32(b + VB_FWINFO + 12, 9600);    /* 96 MHz boot uclk */
    put16(b + VB_FWINFO + 28, 800);
    put16(b + VB_FWINFO + 30, 1250);
    put32(b + VB_FWINFO + 60, 0);       /* pptable from VBIOS/PMFW, not the driver */
    /* smc_dpm_info 4.9: header + BoardTable_t */
    put16(b + VB_SMCDPM, (uint16_t)(4 + sizeof(bt))); b[VB_SMCDPM + 2] = 4; b[VB_SMCDPM + 3] = 9;
    memset(&bt, 0, sizeof(bt));
    bt.Version = VB_BOARD_VERSION;
    bt.VddGfxVrMapping = 0x11;
    bt.HsrEnabled = 1;
    memcpy(b + VB_SMCDPM + 4, &bt, sizeof(bt));
    /* atom_smu_info_v4_0 */
    put16(b + VB_SMUINFO, 288); b[VB_SMUINFO + 2] = 4; b[VB_SMUINFO + 3] = 0;
    put32(b + VB_SMUINFO + 40, 60000);  /* socclk */
    put32(b + VB_SMUINFO + 56, 40000);  /* dcefclk */
    put32(b + VB_SMUINFO + 72, 100000); /* fclk */
    put32(b + VB_SMUINFO + 196, 40000); /* dclk0 */
    put32(b + VB_SMUINFO + 200, 50000); /* vclk0 */
    /* vram_usagebyfirmware_v2_1: 48 MiB at the top */
    put16(b + VB_VRAMUSAGE, 12); b[VB_VRAMUSAGE + 2] = 2; b[VB_VRAMUSAGE + 3] = 1;
    put32(b + VB_VRAMUSAGE + 4, (uint32_t)((VRAM_SIZE - (48ull << 20)) >> 10));
    put16(b + VB_VRAMUSAGE + 8, 48 * 1024);
}

/* the "combo" powerplay table the PMFW hands out: driver portion + PPTable_t
 * whose BoardTable is a placeholder the driver must replace from the VBIOS */
static void make_combo_pptable(uint8_t *out)
{
    PPTable_t *pp = (PPTable_t *)(out + RD_SMU_PP_SMC_PPTABLE_OFF);
    memset(out, 0, RD_SMU_COMBO_PPTABLE_SIZE);
    put16(out, RD_SMU_PP_SMC_PPTABLE_OFF + sizeof(PPTable_t));
    out[2] = RD_SMU_PP_TABLE_FORMAT_REVISION; out[3] = 0;
    out[4] = 2;                                  /* table_revision */
    put16(out + 6, RD_SMU_PP_SMC_PPTABLE_OFF);   /* table_size */
    put32(out + RD_SMU_PP_PLATFORM_CAPS_OFF, RD_SMU_PP_PLATFORM_CAP_POWERPLAY |
          RD_SMU_PP_PLATFORM_CAP_BACO | RD_SMU_PP_PLATFORM_CAP_MACO);
    out[RD_SMU_PP_THERMAL_CTRL_OFF] = 28;
    put16(out + RD_SMU_PP_SHUTDOWN_TEMP_OFF, 118);
    pp->SkuTable.Version = PMFW_SKU_VERSION;
    pp->SkuTable.SocketPowerLimitAc[PPT_THROTTLER_PPT0] = 263;
    pp->SkuTable.SocketPowerLimitDc[PPT_THROTTLER_PPT0] = 263;
    pp->SkuTable.MsgLimits.Power[PPT_THROTTLER_PPT0][POWER_SOURCE_AC] = 280;
    pp->SkuTable.TemperatureLimit[TEMP_EDGE] = 100;
    pp->SkuTable.TemperatureLimit[TEMP_HOTSPOT] = 110;
    pp->SkuTable.TemperatureLimit[TEMP_MEM] = 105;
    pp->SkuTable.PcieGenSpeed[0] = 2; pp->SkuTable.PcieLaneCount[0] = 6; pp->SkuTable.LclkFreq[0] = 600;
    pp->SkuTable.PcieGenSpeed[1] = 3; pp->SkuTable.PcieLaneCount[1] = 6; pp->SkuTable.LclkFreq[1] = 900;
    pp->SkuTable.PcieGenSpeed[2] = 3; pp->SkuTable.PcieLaneCount[2] = 6; pp->SkuTable.LclkFreq[2] = 1200;
    pp->SkuTable.FanMaximumRpm = 3300;
    pp->SkuTable.DriverReportedClocks.GameClockAc = 2124;
    pp->BoardTable.Version = PMFW_BOARD_PLACEHOLDER;
}

static void make_metrics(SmuMetricsExternal_t *m)
{
    memset(m, 0, sizeof(*m));
    m->SmuMetrics.CurrClock[PPCLK_GFXCLK] = 2100;
    m->SmuMetrics.CurrClock[PPCLK_UCLK] =
        (S.smu.enabled & RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT)) ? 1218 : 96;
    m->SmuMetrics.AverageGfxclkFrequencyPreDs = 2050;
    m->SmuMetrics.AvgTemperature[TEMP_EDGE] = 45;
    m->SmuMetrics.AvgTemperature[TEMP_HOTSPOT] = 60;
    m->SmuMetrics.AvgTemperature[TEMP_MEM] = 50;
    m->SmuMetrics.AverageSocketPower = 150;
    m->SmuMetrics.AverageGfxActivity = 12;
    m->SmuMetrics.AvgFanRpm = 900;
    m->SmuMetrics.AvgVoltage[SVI_PLANE_GFX] = 850;
}

/* DPM levels per PPCLK (MHz); gfxclk is fine grained (two end points) */
static unsigned sim_dpm_levels(uint32_t clk, const uint32_t **lv, int *fine)
{
    static const uint32_t gfx[] = { 500, 2124 }, uclk[] = { 96, 500, 1000, 1218 },
                          soc[] = { 600, 900, 1200 }, fclk[] = { 1000, 1800 };
    *fine = 0;
    switch (clk) {
    case PPCLK_GFXCLK: *lv = gfx; *fine = 1; return 2;
    case PPCLK_UCLK:   *lv = uclk; return 4;
    case PPCLK_SOCCLK: *lv = soc; return 3;
    case PPCLK_FCLK:   *lv = fclk; return 2;
    default: *lv = NULL; return 0;
    }
}

static int sim_dpm_enabled(uint32_t clk)
{
    switch (clk) {
    case PPCLK_GFXCLK: return (S.smu.enabled & RD_SMU_FEATURE(FEATURE_DPM_GFXCLK_BIT)) != 0;
    case PPCLK_UCLK:   return (S.smu.enabled & RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT)) != 0;
    case PPCLK_SOCCLK: return (S.smu.enabled & RD_SMU_FEATURE(FEATURE_DPM_SOCCLK_BIT)) != 0;
    case PPCLK_FCLK:   return (S.smu.enabled & RD_SMU_FEATURE(FEATURE_DPM_FCLK_BIT)) != 0;
    default: return 0;
    }
}

/* PMFW mailbox: message 'v' with the argument register; leaves the result
 * in C2PMSG_90 and the reply in C2PMSG_82 */
static void smu_msg(uint32_t v)
{
    uint32_t arg = *R(MP1(82));
    uint32_t resp = PPSMC_Result_OK;
    uint8_t *tbl = mc_ptr(S.smu.driver_addr, RD_SMU_COMBO_PPTABLE_SIZE);
    const uint32_t *lv;
    int fine;
    unsigned n;

    switch (v) {
    case PPSMC_MSG_TestMessage:        *R(MP1(82)) = arg + 1; break;
    case PPSMC_MSG_GetSmuVersion:      *R(MP1(82)) = 0x00526100; break;
    case PPSMC_MSG_GetDriverIfVersion: *R(MP1(82)) = 0x35; break;
    case PPSMC_MSG_SetDriverDramAddrHigh:
        S.smu.driver_addr = (S.smu.driver_addr & 0xffffffffull) | ((uint64_t)arg << 32); break;
    case PPSMC_MSG_SetDriverDramAddrLow:
        S.smu.driver_addr = (S.smu.driver_addr & ~0xffffffffull) | arg; break;
    case PPSMC_MSG_SetToolsDramAddrHigh:
        S.smu.tool_addr = (S.smu.tool_addr & 0xffffffffull) | ((uint64_t)arg << 32); break;
    case PPSMC_MSG_SetToolsDramAddrLow:
        S.smu.tool_addr = (S.smu.tool_addr & ~0xffffffffull) | arg; break;
    case PPSMC_MSG_TransferTableSmu2Dram:
        S.smu.xfers++;
        if (!tbl) { S.smu.bad_addr++; resp = PPSMC_Result_Failed; break; }
        switch (arg & 0xffff) {
        case TABLE_COMBO_PPTABLE: make_combo_pptable(tbl); break;
        case TABLE_SMU_METRICS: {
            SmuMetricsExternal_t m;
            make_metrics(&m);
            memcpy(tbl, &m, sizeof(m));
            break;
        }
        default: resp = PPSMC_Result_Failed;
        }
        break;
    case PPSMC_MSG_TransferTableDram2Smu:
        S.smu.xfers++;
        if (!tbl) { S.smu.bad_addr++; resp = PPSMC_Result_Failed; break; }
        if ((arg & 0xffff) != TABLE_PPTABLE) { resp = PPSMC_Result_Failed; break; }
        if (S.pending_writes)
            S.smu.unflushed++;
        memcpy(&S.smu.pptable, tbl, sizeof(S.smu.pptable));
        S.smu.bad_board = S.smu.pptable.BoardTable.Version != VB_BOARD_VERSION ||
                          S.smu.pptable.SkuTable.Version != PMFW_SKU_VERSION;
        S.smu.pptable_written = 1;
        break;
    case PPSMC_MSG_RunDcBtc:
        if (!S.smu.pptable_written) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        S.smu.btc = 1;
        break;
    case PPSMC_MSG_SetAllowedFeaturesMaskLow:  S.smu.allowed_lo = arg; break;
    case PPSMC_MSG_SetAllowedFeaturesMaskHigh: S.smu.allowed_hi = arg; break;
    case PPSMC_MSG_EnableAllSmuFeatures:
        if (!S.smu.pptable_written || !S.smu.btc) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        S.smu.enabled = (((uint64_t)S.smu.allowed_hi << 32) | S.smu.allowed_lo) & SIM_SMU_SUPPORTED;
        break;
    case PPSMC_MSG_DisableAllSmuFeatures: S.smu.enabled = 0; break;
    case PPSMC_MSG_GetRunningSmuFeaturesLow:  *R(MP1(82)) = (uint32_t)S.smu.enabled; break;
    case PPSMC_MSG_GetRunningSmuFeaturesHigh: *R(MP1(82)) = (uint32_t)(S.smu.enabled >> 32); break;
    case PPSMC_MSG_OverridePcieParameters:
        S.smu.pcie_overrides++;
        S.smu.last_pcie_arg = arg;
        break;
    case PPSMC_MSG_GetDpmFreqByIndex:
        n = sim_dpm_levels(arg >> 16, &lv, &fine);
        if (!n || !sim_dpm_enabled(arg >> 16)) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        if ((arg & 0xffff) == 0xff)
            *R(MP1(82)) = n | (fine ? 0x80000000u : 0);
        else if ((arg & 0xffff) < n)
            *R(MP1(82)) = lv[arg & 0xffff];
        else
            resp = PPSMC_Result_Failed;
        break;
    case PPSMC_MSG_GetMinDpmFreq:
    case PPSMC_MSG_GetMaxDpmFreq:
        n = sim_dpm_levels(arg >> 16, &lv, &fine);
        if (!n || !sim_dpm_enabled(arg >> 16)) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        *R(MP1(82)) = v == PPSMC_MSG_GetMinDpmFreq ? lv[0] : lv[n - 1];
        break;
    case PPSMC_MSG_SetSoftMinByFreq:
    case PPSMC_MSG_SetSoftMaxByFreq:
        if (!sim_dpm_enabled(arg >> 16)) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        S.smu.soft_limits++;
        break;
    case PPSMC_MSG_GetPptLimit: *R(MP1(82)) = S.smu.ppt_limit ? S.smu.ppt_limit : 263; break;
    case PPSMC_MSG_SetPptLimit: S.smu.ppt_limit = arg; break;
    case PPSMC_MSG_SetWorkloadMask: S.smu.workload = arg; break;
    case PPSMC_MSG_NotifyPowerSource: break;
    case PPSMC_MSG_PrepareMp1ForUnload: S.smu.unload = 1; break;
    default: resp = PPSMC_Result_UnknownCmd;
    }
    *R(MP1(90)) = resp;
}

/* ---- DisplayPort sink behind AUX engine 0 ---- */

#define AUXR(reg, n) (DCN_B2 + reg + (regDP_AUX1_AUX_CONTROL - regDP_AUX0_AUX_CONTROL) * (n))
#define HPDR(reg, n) (DCN_B2 + reg + (regHPD1_DC_HPD_CONTROL - regHPD0_DC_HPD_CONTROL) * (n))

static uint8_t edid_sum(const uint8_t *b) { uint32_t i, s = 0; for (i = 0; i < 127; i++) s += b[i]; return (uint8_t)(256 - (s & 0xff)); }

static void make_sink(void)
{
    uint8_t *e = S.edid;
    static const uint8_t dtd_1440p[18] = { 0x56, 0x5E, 0x00, 0xA0, 0xA0, 0xA0, 0x29, 0x50, 0x30, 0x20,
                                           0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E };
    static const uint8_t dtd_1080p[18] = { 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C,
                                           0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E };
    memset(S.dpcd, 0, sizeof(S.dpcd));
    S.dpcd[0x000] = 0x14;   /* DPCD 1.4 */
    S.dpcd[0x001] = 0x1e;   /* HBR3 */
    S.dpcd[0x002] = 0x84;   /* 4 lanes, enhanced framing */
    S.dpcd[0x600] = 0x01;   /* D0 */
    memset(e, 0, sizeof(S.edid));
    memcpy(e, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
    e[8] = 0x48; e[9] = 0x8E;             /* "RDN": R=18 D=4 N=14 -> 0 10010 00100 01110 */
    e[10] = 0x00; e[11] = 0x78;           /* product 0x7800 */
    e[16] = 1; e[17] = 34;                /* week 1 of 2024 */
    e[18] = 1; e[19] = 4;                 /* EDID 1.4 */
    e[20] = 0x80 | 0x05;                  /* digital, 8 bpc, DisplayPort */
    e[21] = 60; e[22] = 34;               /* cm */
    e[23] = 0x78;                         /* gamma 2.2 */
    e[24] = 0x0a;                         /* RGB, preferred timing descriptor */
    e[35] = 0x21;                         /* 640x480@60, 800x600@60 */
    e[38] = 209; e[39] = 0xC0;            /* 1920x1080@60 standard timing */
    e[40] = 0x01; e[41] = 0x01;           /* unused slots */
    memset(e + 42, 0x01, 12);
    memcpy(e + 54, dtd_1440p, 18);
    e[72] = 0; e[73] = 0; e[74] = 0; e[75] = 0xFC; e[76] = 0;
    memcpy(e + 77, "RDNA3 SIM\n   ", 13);
    e[90] = 0; e[91] = 0; e[92] = 0; e[93] = 0x10;   /* dummy descriptors */
    e[108] = 0; e[109] = 0; e[110] = 0; e[111] = 0x10;
    e[126] = 1;                           /* one extension */
    e[127] = edid_sum(e);
    e[128] = 0x02; e[129] = 0x03; e[130] = 0x04; e[131] = 0x00;
    memcpy(e + 132, dtd_1080p, 18);
    e[255] = edid_sum(e + 128);
    S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 0)] = HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK;
}

/* AUX_SW_GO on engine n: decode the request FIFO, answer like the sink */
static void aux_go(uint32_t n)
{
    uint32_t wr = (S.reg[AUXR(regDP_AUX0_AUX_SW_CONTROL, n)] & DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK)
                  >> DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES__SHIFT;
    uint32_t action = S.aux_tx[0] & 0xf0, addr = ((S.aux_tx[0] & 0xf) << 16) | (S.aux_tx[1] << 8) | S.aux_tx[2];
    uint32_t len = wr > 3 ? (uint32_t)S.aux_tx[3] + 1 : 0, i;
    uint32_t reply = RD_AUX_REPLY_ACK;
    S.aux_gos++;
    S.aux_nrx = 0;
    memset(S.aux_rx, 0, sizeof(S.aux_rx));
    if (n != 0) {   /* nothing plugged in: the receiver times out */
        S.aux_status = DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_MASK;
        return;
    }
    if (wr < 3 || wr > 20) { S.aux_bad++; S.aux_status = DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK; return; }
    switch (action) {
    case RD_AUX_DP_READ:
        for (i = 0; i < len && addr + i < sizeof(S.dpcd); i++)
            S.aux_rx[1 + i] = S.dpcd[addr + i];
        S.aux_nrx = 1 + i;
        break;
    case RD_AUX_DP_WRITE:
        if (wr != 4 + len) { S.aux_bad++; }
        for (i = 0; i < len && addr + i < sizeof(S.dpcd); i++)
            S.dpcd[addr + i] = S.aux_tx[4 + i];
        S.aux_nrx = 1;
        break;
    case RD_AUX_I2C_WRITE:
    case RD_AUX_I2C_WRITE_MOT:
        if (addr == 0x50) {
            if (len) S.i2c_off = S.aux_tx[4];
        } else if (addr == 0x30) {
            if (len) S.i2c_seg = S.aux_tx[4];
        } else {
            reply = RD_AUX_REPLY_I2C_NACK;
        }
        if (action == RD_AUX_I2C_WRITE)
            S.i2c_seg = 0;
        S.aux_nrx = 1;
        break;
    case RD_AUX_I2C_READ:
    case RD_AUX_I2C_READ_MOT:
        if (addr != 0x50) {
            reply = RD_AUX_REPLY_I2C_NACK;
            S.aux_nrx = 1;
            break;
        }
        for (i = 0; i < len; i++) {
            uint32_t o = S.i2c_seg * 256 + S.i2c_off;
            S.aux_rx[1 + i] = o < sizeof(S.edid) ? S.edid[o] : 0xff;
            S.i2c_off = (S.i2c_off + 1) & 0xff;
        }
        if (action == RD_AUX_I2C_READ)
            S.i2c_seg = 0;
        S.aux_nrx = 1 + len;
        break;
    default:
        S.aux_bad++;
        reply = RD_AUX_REPLY_NACK;
        S.aux_nrx = 1;
    }
    S.aux_rx[0] = (uint8_t)(reply << 4);
    S.aux_status = DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK |
                   (S.aux_nrx << DP_AUX0_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT__SHIFT);
}

/* mode-1 reset: every block back to power-on except the memory controller
 * (VRAM content stays); the PSP bootloader is ready again */
static void sim_mode1_reset(void)
{
    uint32_t memsize = *R(mmRCC_CONFIG_MEMSIZE);
    uint32_t fbb = *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_BASE), fbt = *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_TOP);
    memset(S.reg, 0, sizeof(S.reg));
    memset(S.hqd, 0, sizeof(S.hqd));
    memset(&S.smu, 0, sizeof(S.smu));
    S.nsmn = 0;
    *smn_slot(SMN_ROM_OFFSET_CTRL, 1) = ROM_STRAP;
    S.sos_alive = 0; S.sos_boot_at = 0; S.rptr_dw = 0; S.ring_mc = 0;
    S.smu_loaded = 0; S.tmr_setup = 0; S.toc_loaded = 0; S.autoload = 0;
    S.cp_rptr = 0; S.sdma_rptr = 0; S.rlc_safe = 0;
    *R(MP0(35)) = 0x80000000u;
    *R(MP0(33)) = 0x80000000u;
    *R(mmRCC_CONFIG_MEMSIZE) = memsize;
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_BASE) = fbb;
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_TOP) = fbt;
    S.mode1_resets++;
}

/* HQD register window (GC segment 0): CP_MQD_BASE_ADDR .. CP_HQD_PQ_WPTR_HI */
#define HQD_FIRST regCP_MQD_BASE_ADDR
#define HQD_LAST  regCP_HQD_PQ_WPTR_HI
static struct hqd *hqd_sel(void)
{
    uint32_t c = S.reg[GCR(regGRBM_GFX_CNTL)];
    uint32_t me = (c & GRBM_GFX_CNTL__MEID_MASK) >> GRBM_GFX_CNTL__MEID__SHIFT;
    uint32_t pipe = (c & GRBM_GFX_CNTL__PIPEID_MASK) >> GRBM_GFX_CNTL__PIPEID__SHIFT;
    uint32_t q = (c & GRBM_GFX_CNTL__QUEUEID_MASK) >> GRBM_GFX_CNTL__QUEUEID__SHIFT;
    if (me != 1 || q > 3)
        return NULL;
    return &S.hqd[pipe][q];
}
#define HQD(hq, r_) ((hq)->reg[(r_) - HQD_FIRST])

/* MAP_QUEUES: the firmware loads the HQD from the MQD in memory */
static const struct { uint32_t mqd, reg; } mqd_map[] = {
    { 128, regCP_MQD_BASE_ADDR }, { 129, regCP_MQD_BASE_ADDR_HI }, { 130, regCP_HQD_ACTIVE },
    { 131, regCP_HQD_VMID }, { 132, regCP_HQD_PERSISTENT_STATE }, { 136, regCP_HQD_PQ_BASE },
    { 137, regCP_HQD_PQ_BASE_HI }, { 138, regCP_HQD_PQ_RPTR }, { 139, regCP_HQD_PQ_RPTR_REPORT_ADDR },
    { 140, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI }, { 141, regCP_HQD_PQ_WPTR_POLL_ADDR },
    { 142, regCP_HQD_PQ_WPTR_POLL_ADDR_HI }, { 143, regCP_HQD_PQ_DOORBELL_CONTROL },
    { 145, regCP_HQD_PQ_CONTROL }, { 149, regCP_HQD_IB_CONTROL }, { 162, regCP_MQD_CONTROL },
    { 165, regCP_HQD_EOP_BASE_ADDR }, { 166, regCP_HQD_EOP_BASE_ADDR_HI }, { 167, regCP_HQD_EOP_CONTROL },
    { 182, regCP_HQD_PQ_WPTR_LO }, { 183, regCP_HQD_PQ_WPTR_HI },
};
static int hqd_load_mqd(struct hqd *h, uint64_t mqd_mc, uint32_t doorbell_index)
{
    const uint32_t *m = (const uint32_t *)mc_ptr(mqd_mc, 4 * RD_MQD_DWORDS);
    if (!m || m[0] != 0xC0310800u)
        return 0;
    for (unsigned i = 0; i < sizeof(mqd_map) / sizeof(mqd_map[0]); i++)
        h->reg[mqd_map[i].reg - HQD_FIRST] = m[mqd_map[i].mqd];
    HQD(h, regCP_HQD_ACTIVE) = 1;
    HQD(h, regCP_HQD_PQ_DOORBELL_CONTROL) =
        (HQD(h, regCP_HQD_PQ_DOORBELL_CONTROL) & ~CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) |
        ((doorbell_index << CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT) &
         CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) | CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK;
    h->rptr = 0;
    h->mapped_by_kiq = 1;
    return 1;
}

/* IH model: an engine queues an 8-dword IV on ring 0 (IH_RB_BASE, size
 * from IH_RB_CNTL), advances IH_RB_WPTR (+ the writeback dword), flags an
 * overflow when it catches the rptr, and raises an MSI when ENABLE_INTR */
static void ih_push_iv(uint32_t client, uint32_t src, uint32_t ring_id, uint32_t vmid,
                       uint32_t pasid, uint32_t sd0, uint32_t sd1);
static void ih_push(uint32_t client, uint32_t src, uint32_t ring_id)
{
    ih_push_iv(client, src, ring_id, 0, 0, 0, 0);
}
static void ih_push_iv(uint32_t client, uint32_t src, uint32_t ring_id, uint32_t vmid,
                       uint32_t pasid, uint32_t sd0, uint32_t sd1)
{
    uint32_t cntl = S.reg[OSS_BASE0 + regIH_RB_CNTL];
    uint64_t base = ((uint64_t)S.reg[OSS_BASE0 + regIH_RB_BASE] << 8) |
                    ((uint64_t)S.reg[OSS_BASE0 + regIH_RB_BASE_HI] << 40);
    uint64_t wb = S.reg[OSS_BASE0 + regIH_RB_WPTR_ADDR_LO] |
                  ((uint64_t)S.reg[OSS_BASE0 + regIH_RB_WPTR_ADDR_HI] << 32);
    uint32_t size = 4u << ((cntl & IH_RB_CNTL__RB_SIZE_MASK) >> IH_RB_CNTL__RB_SIZE__SHIFT);
    uint32_t wreg = S.reg[OSS_BASE0 + regIH_RB_WPTR], wptr = wreg & (size - 1);
    uint32_t rptr = S.reg[OSS_BASE0 + regIH_RB_RPTR] & (size - 1);
    uint32_t iv[8] = { client | (src << 8) | (ring_id << 16) | (vmid << 24), 0, 0, pasid, sd0, sd1, 0, 0 };
    uint8_t *p = (cntl & IH_RB_CNTL__RB_ENABLE_MASK) ? mc_ptr(base + wptr, 32) : NULL;
    if (!p) {
        S.ih_dropped++;
        return;
    }
    memcpy(p, iv, 32);
    wptr = (wptr + 32) & (size - 1);
    wreg = wptr | (wreg & IH_RB_WPTR__RB_OVERFLOW_MASK) | (wptr == rptr ? IH_RB_WPTR__RB_OVERFLOW_MASK : 0);
    S.reg[OSS_BASE0 + regIH_RB_WPTR] = wreg;
    if ((cntl & IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK) && mc_ptr(wb, 4))
        memcpy(mc_ptr(wb, 4), &wreg, 4);
    S.ih_ivs++;
    if (cntl & IH_RB_CNTL__ENABLE_INTR_MASK)
        S.ih_msi++;
}

/* CP (ME/PFP) model for the kernel gfx ring: SET_UCONFIG_REG, the clear
 * state packets, RELEASE_MEM fence writes, WRITE_DATA, WAIT_REG_MEM, the
 * job framing packets and INDIRECT_BUFFER through the VMID's page tables */
static void sim_wr(void *ctx, uint64_t off, uint32_t v);
static uint32_t sim_rd(void *ctx, uint64_t off);
static void cp_run(uint32_t wptr_dw);
static void sdma_run(uint32_t wptr_bytes);

/* walk VMID n's 4-level tables like the GFXHUB would (FB_OFFSET is 0, so a
 * PDE/PTE address is a VRAM offset) */
static uint8_t *vm_ptr(uint32_t vmid, uint64_t va, uint64_t len)
{
    static const uint32_t shift[4] = { 39, 30, 21, 12 };
    uint32_t dist = regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
    uint64_t e = S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + dist * vmid] |
                 ((uint64_t)S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 + dist * vmid] << 32);
    va &= 0x0000FFFFFFFFFFFFull;
    if ((va & 4095) + len > 4096)
        return NULL;   /* the model reads within one page */
    for (int l = 0; l < 4; l++) {
        uint64_t t = e & 0x0000FFFFFFFFF000ull;
        if (!(e & 1) || t + 4096 > BAR_LEN)
            return NULL;
        memcpy(&e, S.vram + t + 8 * ((va >> shift[l]) & 511), 8);
    }
    if (!(e & 1) || (e & 0x0000FFFFFFFFF000ull) + 4096 > BAR_LEN)
        return NULL;
    return S.vram + (e & 0x0000FFFFFFFFF000ull) + (va & 4095);
}

/* run packets [rptr, end) of buffer 'buf' (index mask 'mask'); returns new rptr */
static uint64_t cp_exec(const uint32_t *buf, uint64_t mask, uint64_t rptr, uint64_t end, int in_ib)
{
#define PK(i) buf[(rptr + (i)) & mask]
    while ((rptr & mask) != (end & mask)) {
        uint32_t h = PK(0), type = h >> 30, op = (h >> 8) & 0xff, cnt = (h >> 16) & 0x3fff;
        S.cp_pkts++;
        if (type != 3) { S.cp_bad++; rptr++; continue; }
        switch (op) {
        case PACKET3_SET_UCONFIG_REG:
            for (uint32_t i = 0; i < cnt; i++)
                S.reg[0xC000 + PK(1) + i] = PK(2 + i);
            break;
        case PACKET3_SET_CONTEXT_REG: S.cp_ctx_regs += cnt; break;
        case PACKET3_CLEAR_STATE: S.cp_clear_state++; break;
        case PACKET3_CONTEXT_CONTROL: S.cp_last_cntxcntl = PK(1); break;
        case PACKET3_PREAMBLE_CNTL: case PACKET3_NOP: break;
        case PACKET3_PFP_SYNC_ME: S.cp_pfp_sync++; break;
        case PACKET3_SET_RESOURCES:
            if (cnt == 6 && S.cp_cur_me == 1) S.set_resources_mask = PK(2) | ((uint64_t)PK(3) << 32);
            else S.cp_bad++;
            break;
        case PACKET3_MAP_QUEUES: {
            uint32_t q = (PK(1) >> 13) & 7, pipe = (PK(1) >> 16) & 3, me = (PK(1) >> 18) & 3;
            uint32_t db = (PK(2) >> 2) & 0x3ffffff;
            uint64_t mqd = PK(3) | ((uint64_t)PK(4) << 32);
            if (cnt != 5 || S.cp_cur_me != 1 || me != 1 || q > 3) { S.cp_bad++; break; }
            if (!(S.set_resources_mask & (1ull << (pipe * 8 + q)))) { S.mec_bad_queue++; break; }
            if (!hqd_load_mqd(&S.hqd[pipe][q], mqd, db)) { S.cp_bad++; break; }
            S.kiq_maps++;
            break;
        }
        case PACKET3_UNMAP_QUEUES: {
            uint32_t db = (PK(2) >> 2) & 0x3ffffff;
            int found = 0;
            if (cnt != 4 || S.cp_cur_me != 1) { S.cp_bad++; break; }
            for (uint32_t p2 = 0; p2 < 4; p2++)
                for (uint32_t q2 = 0; q2 < 4; q2++) {
                    struct hqd *hq = &S.hqd[p2][q2];
                    uint32_t dc = HQD(hq, regCP_HQD_PQ_DOORBELL_CONTROL);
                    if ((dc & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK) &&
                        ((dc & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == db && hq->mapped_by_kiq) {
                        HQD(hq, regCP_HQD_ACTIVE) = 0; hq->mapped_by_kiq = 0; found = 1;
                    }
                }
            if (found) S.kiq_unmaps++; else S.mec_bad_queue++;
            break;
        }
        case PACKET3_ACQUIRE_MEM: if (cnt == 6 && PK(7)) S.cp_acquire++; else S.cp_bad++; break;
        case PACKET3_FRAME_CONTROL:
            if (PK(1) >> 28) S.cp_frame_end++; else S.cp_frame_begin++;
            break;
        case PACKET3_WRITE_DATA: {
            uint32_t dst = (PK(1) >> 8) & 0xf;
            for (uint32_t i = 0; i + 3 < cnt + 1; i++) {
                if (dst == 0) {
                    sim_wr(NULL, 4ull * (PK(2) + i), PK(4 + i));
                } else if (dst == 5) {
                    uint8_t *p = mc_ptr((PK(2) | ((uint64_t)PK(3) << 32)) + 4ull * i, 4);
                    if (p) memcpy(p, &PK(4 + i), 4); else S.cp_bad++;
                } else {
                    S.cp_bad++;
                }
            }
            break;
        }
        case PACKET3_WAIT_REG_MEM: {
            uint32_t ctl = PK(1), ref = PK(4), mask_ = PK(5), v;
            if (ctl & (1u << 4)) { S.cp_bad++; break; }   /* memory polls not modelled */
            if (ctl & (1u << 6)) {                        /* write, then wait */
                sim_wr(NULL, 4ull * PK(2), ref);
                v = sim_rd(NULL, 4ull * PK(3));
            } else {
                v = sim_rd(NULL, 4ull * PK(2));
            }
            if ((v & mask_) != (ref & mask_))
                S.cp_wait_stuck++;                        /* would hang the CP */
            break;
        }
        case PACKET3_INDIRECT_BUFFER: {
            uint64_t va = PK(1) | ((uint64_t)PK(2) << 32);
            uint32_t ndw = PK(3) & 0xfffff, vmid = (PK(3) >> 24) & 0xf, i;   /* bit 23 = VALID */
            static uint32_t ib[4096];
            if (in_ib || ndw > 4096) { S.cp_bad++; break; }
            for (i = 0; i < ndw; i++) {
                uint8_t *p = vm_ptr(vmid, va + 4ull * i, 4);
                if (!p) {
                    uint64_t fa = va + 4ull * i;
                    uint32_t cntl = S.reg[GC_BASE0 + regGCVM_CONTEXT0_CNTL +
                                          vmid * (regGCVM_CONTEXT1_CNTL - regGCVM_CONTEXT0_CNTL)];
                    S.cp_ib_fault++;
                    /* UTCL2 fault: status for the GFXHUB, IV on client GFX source 0 */
                    S.reg[GC_BASE0 + regGCVM_L2_PROTECTION_FAULT_STATUS] =
                        (vmid << 20) | (0x12u << 9) | (1u << 8) /* MAPPING_ERROR */;
                    if (cntl & GCVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK) {
                        S.fault_ivs++;
                        ih_push_iv(RD_IH_CLIENT_GFX, RD_IH_SRC_VM_FAULT, 0, vmid, 0,
                                   (uint32_t)(fa >> 12), (uint32_t)((fa >> 44) & 0xf) | 0x40);
                    }
                    break;
                }
                memcpy(&ib[i], p, 4);
            }
            if (i < ndw)
                break;
            S.cp_ibs++;
            S.cp_ib_dw += ndw;
            S.cp_last_ib_vmid = vmid;
            cp_exec(ib, ~0ull, 0, ndw, 1);
            break;
        }
        case PACKET3_RELEASE_MEM: {
            uint64_t a = PK(3) | ((uint64_t)PK(4) << 32);
            uint8_t *p = mc_ptr(a, 4);
            if (p && ((PK(2) >> 29) & 7) == 1) memcpy(p, &PK(5), 4); else S.cp_bad++;
            if (((PK(2) >> 24) & 7) == 2) {
                /* EOP interrupt: gated per engine, ring_id = queue<<4 | me<<2 | pipe */
                uint32_t en = S.cp_cur_me == 0
                    ? S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK
                    : S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + S.cp_cur_pipe] & CP_ME1_PIPE0_INT_CNTL__TIME_STAMP_INT_ENABLE_MASK;
                if (en)
                    ih_push(RD_IH_CLIENT_GRBM_CP, RD_IH_SRC_CP_EOP,
                            (S.cp_cur_queue << 4) | (S.cp_cur_me << 2) | S.cp_cur_pipe);
            }
            break;
        }
        default: S.cp_bad++; break;
        }
        rptr += 2 + cnt;
    }
#undef PK
    return rptr;
}

/* a mapped MEC queue runs its PQ up to the doorbell value (dwords) */
static void mec_run(uint32_t pipe, uint32_t q, uint64_t wptr)
{
    struct hqd *h = &S.hqd[pipe][q];
    uint32_t ctl = HQD(h, regCP_HQD_PQ_CONTROL);
    uint32_t ndw = 2u << (ctl & CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK);
    uint64_t base = ((uint64_t)HQD(h, regCP_HQD_PQ_BASE) << 8) | ((uint64_t)HQD(h, regCP_HQD_PQ_BASE_HI) << 40);
    uint64_t rwb = HQD(h, regCP_HQD_PQ_RPTR_REPORT_ADDR) | ((uint64_t)HQD(h, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI) << 32);
    uint32_t *ring = (uint32_t *)mc_ptr(base, 4ull * ndw);
    uint32_t mec = S.reg[GCR(regCP_MEC_RS64_CNTL)];
    uint8_t *wb;
    if (!(HQD(h, regCP_HQD_ACTIVE) & 1) || !ring) { S.mec_bad_queue++; return; }
    if ((mec & CP_MEC_RS64_CNTL__MEC_HALT_MASK) || !(mec & (CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK << pipe))) {
        S.cp_while_halted++;
        return;
    }
    if (S.pending_writes)
        S.unflushed_doorbells++;
    S.cp_cur_me = 1; S.cp_cur_pipe = pipe; S.cp_cur_queue = q;
    h->rptr = cp_exec(ring, ndw - 1, h->rptr, wptr, 0);
    S.cp_cur_me = 0; S.cp_cur_pipe = 0; S.cp_cur_queue = 0;
    S.mec_runs++;
    wb = mc_ptr(rwb, 4);
    if (wb) {
        uint32_t r32 = (uint32_t)(h->rptr & (ndw - 1));
        memcpy(wb, &r32, 4);
    }
}

/* 64-bit doorbell write (BAR2 dword index): find the engine that owns it */
static void sim_doorbell(void *ctx, uint32_t index, uint64_t v)
{
    uint32_t dc = S.reg[GCR(regCP_RB_DOORBELL_CONTROL)];
    uint32_t sd = S.reg[GC_BASE0 + regSDMA0_QUEUE0_DOORBELL];
    uint32_t so = S.reg[GC_BASE0 + regSDMA0_QUEUE0_DOORBELL_OFFSET];
    (void)ctx;
    S.db_writes++;
    S.db_last_index = index;
    if ((dc & CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK) &&
        ((dc & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == index) {
        cp_run((uint32_t)v);
        return;
    }
    if ((sd & SDMA0_QUEUE0_DOORBELL__ENABLE_MASK) &&
        ((so & SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK) >> 2) == index) {
        sdma_run((uint32_t)v);
        return;
    }
    for (uint32_t p = 0; p < 4; p++)
        for (uint32_t q = 0; q < 4; q++) {
            struct hqd *h = &S.hqd[p][q];
            uint32_t c = HQD(h, regCP_HQD_PQ_DOORBELL_CONTROL);
            if ((c & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK) &&
                ((c & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == index &&
                (HQD(h, regCP_HQD_ACTIVE) & 1)) {
                mec_run(p, q, v);
                return;
            }
        }
    S.db_unclaimed++;
}

static void cp_run(uint32_t wptr_dw)
{
    uint32_t cntl = S.reg[GCR(regCP_RB0_CNTL)];
    uint32_t me = S.reg[GCR(regCP_ME_CNTL)];
    uint64_t base = ((uint64_t)S.reg[GCR(regCP_RB0_BASE)] << 8) |
                    ((uint64_t)S.reg[GCR(regCP_RB0_BASE_HI)] << 40);
    uint32_t ndw = 2u << ((cntl & CP_RB0_CNTL__RB_BUFSZ_MASK) >> CP_RB0_CNTL__RB_BUFSZ__SHIFT);
    uint32_t *ring = (uint32_t *)mc_ptr(base, 4ull * ndw);
    uint64_t rptr_wb = ((uint64_t)S.reg[GCR(regCP_RB0_RPTR_ADDR_HI)] << 32) |
                       S.reg[GCR(regCP_RB0_RPTR_ADDR)];
    uint8_t *wb;
    if (!S.reg[GCR(regCP_RB_ACTIVE)] || !ring)
        return;
    if (me & (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK)) {
        S.cp_while_halted++;
        return;
    }
    if (S.pending_writes)
        S.unflushed_doorbells++;
    S.cp_rptr = cp_exec(ring, ndw - 1, S.cp_rptr, wptr_dw, 0);
    /* rptr writeback (dwords, 32 bit) */
    wb = mc_ptr(rptr_wb, 4);
    if (wb) {
        uint32_t r32 = (uint32_t)(S.cp_rptr & (ndw - 1));
        memcpy(wb, &r32, 4);
    }
}

/* SDMA 6.0 engine model: WRITE_LINEAR, COPY_LINEAR, CONST_FILL, FENCE, NOP */
static void sdma_run(uint32_t wptr_bytes)
{
    uint32_t cntl = S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_CNTL];
    uint32_t f32 = S.reg[0xA000 + regSDMA0_F32_CNTL];
    uint64_t base = ((uint64_t)S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_BASE] << 8) |
                    ((uint64_t)S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_BASE_HI] << 40);
    uint32_t size = 4u << ((cntl & SDMA0_QUEUE0_RB_CNTL__RB_SIZE_MASK) >> SDMA0_QUEUE0_RB_CNTL__RB_SIZE__SHIFT);
    uint64_t rptr_wb = ((uint64_t)S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_RPTR_ADDR_HI] << 32) |
                       S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_RPTR_ADDR_LO];
    uint8_t *ring = mc_ptr(base, size);
    if (!(cntl & SDMA0_QUEUE0_RB_CNTL__RB_ENABLE_MASK))
        return;
    if (f32 & SDMA0_F32_CNTL__HALT_MASK) {
        S.sdma_while_halted++;
        return;
    }
    if (S.pending_writes)
        S.unflushed_doorbells++;
#define RD(i) (*(uint32_t *)(ring + ((S.sdma_rptr + 4 * (i)) & (size - 1))))
    while (ring && (S.sdma_rptr & (size - 1)) != (wptr_bytes & (size - 1))) {
        uint32_t h = RD(0), op = h & 0xff, sub = (h >> 8) & 0xff, n;
        S.sdma_pkts++;
        if (op == SDMA_OP_NOP) {
            n = 1 + ((h >> 16) & 0x3fff);
        } else if (op == SDMA_OP_WRITE && sub == 0) {
            uint64_t a = RD(1) | ((uint64_t)RD(2) << 32);
            uint32_t cnt = (RD(3) & 0xfffff) + 1, i;
            uint8_t *p = mc_ptr(a, 4ull * cnt);
            for (i = 0; p && i < cnt; i++)
                memcpy(p + 4 * i, &RD(4 + i), 4);
            if (!p) S.sdma_bad++;
            n = 4 + cnt;
        } else if (op == SDMA_OP_COPY && sub == 0) {
            uint32_t len = RD(1) + 1;
            uint64_t s = RD(3) | ((uint64_t)RD(4) << 32), d = RD(5) | ((uint64_t)RD(6) << 32);
            uint8_t *ps = mc_ptr(s, len), *pd = mc_ptr(d, len);
            if (ps && pd) memmove(pd, ps, len); else S.sdma_bad++;
            n = 7;
        } else if (op == SDMA_OP_CONST_FILL) {
            uint64_t d = RD(1) | ((uint64_t)RD(2) << 32);
            uint32_t val = RD(3), len = RD(4) + 1, i;
            uint8_t *pd = mc_ptr(d, len);
            for (i = 0; pd && i + 4 <= len; i += 4) memcpy(pd + i, &val, 4);
            if (!pd) S.sdma_bad++;
            n = 5;
        } else if (op == SDMA_OP_FENCE) {
            uint64_t a = RD(1) | ((uint64_t)RD(2) << 32);
            uint8_t *p = mc_ptr(a, 4);
            if (p) memcpy(p, &RD(3), 4); else S.sdma_bad++;
            n = 4;
        } else if (op == SDMA_OP_TRAP) {
            if (S.reg[GC_BASE0 + regSDMA0_CNTL] & SDMA0_CNTL__TRAP_ENABLE_MASK)
                ih_push(RD_IH_CLIENT_GFX, RD_IH_SRC_SDMA_TRAP, 0);
            n = 2;
        } else {
            S.sdma_bad++;
            n = 1;
        }
        S.sdma_rptr += 4ull * n;
    }
#undef RD
    if (mc_ptr(rptr_wb, 8))
        memcpy(mc_ptr(rptr_wb, 8), &S.sdma_rptr, 8);
}

static void psp_exec(uint64_t cmd_mc, uint64_t fence_mc, uint32_t fence_val)
{
    struct psp_gfx_cmd_resp *cmd = (struct psp_gfx_cmd_resp *)mc_ptr(cmd_mc, sizeof(*cmd));
    uint32_t *fence = (uint32_t *)mc_ptr(fence_mc, 4);
    if (!cmd || !fence) {
        fprintf(stderr, "sim: frame points outside VRAM\n");
        failures++;
        return;
    }
    memset(&cmd->resp, 0, sizeof(cmd->resp));
    switch (cmd->cmd_id) {
    case GFX_CMD_ID_LOAD_TOC:
        S.toc_loaded = 1;
        cmd->resp.tmr_size = 0x600000;
        break;
    case GFX_CMD_ID_SETUP_TMR:
        S.tmr_setup = 1;
        S.tmr_after_smu = S.smu_loaded;
        if (!cmd->cmd.cmd_setup_tmr.bitfield.virt_phy_addr || cmd->cmd.cmd_setup_tmr.buf_size != 0x600000)
            cmd->resp.status = 0xFFFF0001;
        break;
    case GFX_CMD_ID_LOAD_IP_FW: {
        uint32_t t = cmd->cmd.cmd_load_ip_fw.fw_type;
        uint64_t mc = cmd->cmd.cmd_load_ip_fw.fw_phy_addr_lo |
                      ((uint64_t)cmd->cmd.cmd_load_ip_fw.fw_phy_addr_hi << 32);
        if (!mc_ptr(mc, cmd->cmd.cmd_load_ip_fw.fw_size) || (mc & 4095))
            cmd->resp.status = 0xFFFF0002;
        if (S.nipfw < 64)
            S.ip_fw_types[S.nipfw++] = t;
        if (t == GFX_FW_TYPE_SMU) {
            uint32_t *fl = smn_slot(RD_SMN_MP1_FIRMWARE_FLAGS, 1);
            S.smu_loaded = 1;
            *R(MP1(90)) = 1; /* PMFW up, mailbox idle */
            if (fl) *fl = MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK;
        }
        break;
    }
    case GFX_CMD_ID_AUTOLOAD_RLC:
        S.autoload = 1;
        S.autoload_after_rlc_g = S.nipfw && S.ip_fw_types[S.nipfw - 1] == GFX_FW_TYPE_RLC_G;
        break;
    default:
        cmd->resp.status = 0x100; /* PSP_ERR_UNKNOWN_COMMAND */
    }
    *fence = fence_val;
}

static uint32_t *smn_slot(uint32_t addr, int create)
{
    for (unsigned i = 0; i < S.nsmn; i++)
        if (S.smn[i].addr == addr)
            return &S.smn[i].val;
    if (!create || S.nsmn >= 64)
        return NULL;
    S.smn[S.nsmn].addr = addr;
    S.smn[S.nsmn].val = 0;
    return &S.smn[S.nsmn++].val;
}

/* what the driver wrote to a register beyond the BAR (SMN byte address) */
static uint32_t smn_val(uint32_t reg_dw)
{
    uint32_t *p = smn_slot(reg_dw * 4u, 0);
    return p ? *p : 0;
}

static void sim_wr(void *ctx, uint64_t off, uint32_t v)
{
    uint32_t dw = (uint32_t)(off / 4);
    (void)ctx;
    if (off >= MMIO_LEN)
        return;
    /* RSMU index/data: the indirect window to registers beyond the BAR */
    if (dw == NBIO_B1 + regBIF_BX_PF0_RSMU_INDEX) {
        S.smn_index = v;
        *R(dw) = v;
        return;
    }
    if (dw == NBIO_B1 + regBIF_BX_PF0_RSMU_DATA) {
        uint32_t *p = smn_slot(S.smn_index, 1);
        if (p) *p = v;
        *R(dw) = v;
        return;
    }
    /* MEC HQD registers: the file of the queue GRBM_GFX_CNTL selects */
    if (dw >= GC_BASE0 + HQD_FIRST && dw <= GC_BASE0 + HQD_LAST) {
        struct hqd *h = hqd_sel();
        uint32_t reg = dw - GC_BASE0;
        if (!h) { *R(dw) = v; return; }
        if (reg == regCP_HQD_DEQUEUE_REQUEST && v) {
            HQD(h, regCP_HQD_ACTIVE) = 0;   /* drained at once */
            return;
        }
        if (reg == regCP_HQD_ACTIVE && (v & 1) && !(HQD(h, regCP_HQD_PQ_DOORBELL_CONTROL) & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK))
            S.mec_bad_queue++;   /* a queue without a doorbell can never be kicked */
        if (reg == regCP_HQD_ACTIVE && (v & 1))
            h->rptr = 0;
        HQD(h, reg) = v;
        return;
    }

    /* HDP flush through the remap hole */
    if (off == RDNA3_MMIO_REG_HOLE_OFFSET + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL) {
        S.pending_writes = 0;
        return;
    }

    if (dw == MP0(35)) {
        uint64_t mc = (uint64_t)*R(MP0(36)) << 20;
        if (!(*R(MP0(35)) & 0x80000000u)) {
            fprintf(stderr, "sim: bootloader command while busy\n");
            failures++;
        }
        if (S.pending_writes)
            S.unflushed_doorbells++;
        if (!mc_ptr(mc, 4096)) {
            fprintf(stderr, "sim: bootloader buffer MC 0x%llx outside VRAM\n",
                    (unsigned long long)mc);
            failures++;
        }
        if (S.nbl < 16)
            S.bl_cmds[S.nbl++] = v;
        if (v == 0x20000u) { /* SOSDRV: boots asynchronously */
            S.sos_boot_at = S.now_us + 50000;
            *R(MP0(35)) = 0; /* bootloader busy, then gone */
        } else {
            *R(MP0(35)) = 0x80000000u;
        }
        return;
    }
    if (dw == MP0(64)) {
        if (v == (2u << 16)) {
            S.ring_mc = *R(MP0(69)) | ((uint64_t)*R(MP0(70)) << 32);
            if (*R(MP0(71)) != PSP_RING_SIZE || !mc_ptr(S.ring_mc, PSP_RING_SIZE))
                *R(dw) = 0x80000001u; /* status error */
            else
                *R(dw) = 0x80000000u;
        } else if (v == GFX_CTRL_CMD_ID_DESTROY_RINGS) {
            *R(dw) = 0x80000000u;
        }
        return;
    }
    if (dw == MP0(67)) {
        /* doorbell: consume frames from rptr to new wptr */
        if (S.pending_writes)
            S.unflushed_doorbells++;
        while (S.rptr_dw != v) {
            struct psp_gfx_rb_frame *f =
                (struct psp_gfx_rb_frame *)mc_ptr(S.ring_mc + S.rptr_dw * 4u, sizeof(*f));
            if (!f)
                break;
            psp_exec(f->cmd_buf_addr_lo | ((uint64_t)f->cmd_buf_addr_hi << 32),
                     f->fence_addr_lo | ((uint64_t)f->fence_addr_hi << 32), f->fence_value);
            S.rptr_dw = (S.rptr_dw + 16) % (PSP_RING_SIZE / 4);
        }
        *R(dw) = v;
        return;
    }
    /* GFXHUB invalidation engine 17: acknowledge the requested VMIDs */
    {
        uint32_t eng_dist = regGCVM_INVALIDATE_ENG1_REQ - regGCVM_INVALIDATE_ENG0_REQ;
        if (dw == GC_BASE0 + regGCVM_INVALIDATE_ENG0_REQ + RD_GFX_VM_INV_ENG * eng_dist) {
            /* the gfx ring's engine (gmc_v11_0_emit_flush_gpu_tlb) */
            S.reg[GC_BASE0 + regGCVM_INVALIDATE_ENG0_ACK + RD_GFX_VM_INV_ENG * eng_dist] = v & 0xffff;
            S.ring_tlb_flushes++;
            *R(dw) = v;
            return;
        }
        if (dw == GC_BASE0 + regGCVM_INVALIDATE_ENG0_REQ + 17 * eng_dist) {
            S.reg[GC_BASE0 + regGCVM_INVALIDATE_ENG0_ACK + 17 * eng_dist] = v & 0xffff;
            S.tlb_flushes++;
            if (S.pending_writes)
                S.unflushed_doorbells++;
            *R(dw) = v;
            return;
        }
    }
    /* DCN HUBPREQ: the LOW address write latches the flip (at the next
     * vupdate, which the model takes as "now" unless dcn_hold) */
    for (uint32_t n = 0; n < RD_DCN_MAX_PIPES; n++) {
        if (dw == HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, n)) {
            *R(dw) = v;
            S.dcn_flips++;
            if (S.dcn_hold) {
                S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, n)] |= HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
            } else {
                S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, n)] = v;
                S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, n)] =
                    S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, n)];
            }
            return;
        }
        if (dw == HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, n)) {
            if (!(S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, n)] &
                  HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK))
                S.dcn_pitch_unlocked++;
            *R(dw) = v;
            return;
        }
        if (dw == HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, n)) {
            /* SURFACE_FLIP_PENDING is read-only status */
            uint32_t pend = HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
            *R(dw) = (v & ~pend) | (*R(dw) & pend);
            return;
        }
    }
    /* NBIO HDP flush engine: a request completes at once */
    if (dw == NBIO_B2 + regBIF_BX_PF0_GPU_HDP_FLUSH_REQ) {
        S.reg[NBIO_B2 + regBIF_BX_PF0_GPU_HDP_FLUSH_DONE] = v;
        S.hdp_engine_flushes++;
        S.pending_writes = 0;
        *R(dw) = v;
        return;
    }
    /* CP gfx ring 0: a write of CP_RB0_WPTR runs PM4 up to it */
    if (dw == GCR(regCP_RB0_WPTR)) {
        *R(dw) = v;
        cp_run(v);
        return;
    }
    /* SDMA0 QUEUE0: a write of RB_WPTR runs the ring up to it */
    if (dw == GC_BASE0 + regSDMA0_QUEUE0_RB_WPTR) {
        *R(dw) = v;
        sdma_run(v);
        return;
    }
    /* IH: WPTR_OVERFLOW_CLEAR clears the overflow flag in IH_RB_WPTR */
    if (dw == OSS_BASE0 + regIH_RB_CNTL && (v & IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK))
        S.reg[OSS_BASE0 + regIH_RB_WPTR] &= ~(uint32_t)IH_RB_WPTR__RB_OVERFLOW_MASK;
    /* MMHUB engine 17: semaphore, request/ack, private invalidation */
    {
        uint32_t eng_dist = regMMVM_INVALIDATE_ENG1_REQ - regMMVM_INVALIDATE_ENG0_REQ;
        if (dw == MM_BASE0 + regMMVM_INVALIDATE_ENG0_SEM + 17 * eng_dist) {
            S.mm_sem_held = 0;      /* driver writes 0 to release */
            *R(dw) = 1;             /* next read acquires again */
            return;
        }
        if (dw == MM_BASE0 + regMMVM_INVALIDATE_ENG0_REQ + 17 * eng_dist) {
            if (!S.mm_sem_held)
                S.mm_flush_without_sem++;
            S.reg[MM_BASE0 + regMMVM_INVALIDATE_ENG0_ACK + 17 * eng_dist] = v & 0xffff;
            S.mm_tlb_flushes++;
            if (S.pending_writes)
                S.unflushed_doorbells++;
            *R(dw) = v;
            return;
        }
        if (dw == MM_BASE0 + regMMVM_L2_BANK_SELECT_RESERVED_CID2 && (v & (1u << 25))) {
            S.mm_private_inv++;
            *R(dw) = v & ~(1u << 25);
            return;
        }
    }
    if (dw == MP1(66)) {
        S.smu_msgs++;
        smu_msg(v);
        return;
    }
    /* DP AUX engines (DCN segment 2) */
    for (uint32_t n = 0; n < RD_AUX_ENGINES; n++) {
        if (dw == AUXR(regDP_AUX0_AUX_ARB_CONTROL, n)) {
            uint32_t st = 0;
            if (v & DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK)
                st = 1;   /* SW_CAN_ACCESS_AUX */
            if (v & DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK)
                st = 0;
            *R(dw) = (v & ~(DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK |
                            DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK)) |
                     (st << DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS__SHIFT);
            return;
        }
        if (dw == AUXR(regDP_AUX0_AUX_CONTROL, n)) {
            /* RESET_DONE follows RESET */
            *R(dw) = (v & ~DP_AUX0_AUX_CONTROL__AUX_RESET_DONE_MASK) |
                     ((v & DP_AUX0_AUX_CONTROL__AUX_RESET_MASK) ? DP_AUX0_AUX_CONTROL__AUX_RESET_DONE_MASK : 0);
            return;
        }
        if (dw == AUXR(regDP_AUX0_AUX_INTERRUPT_CONTROL, n)) {
            if (v & DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK)
                S.aux_status &= ~DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK;
            *R(dw) = v & ~DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK;
            return;
        }
        if (dw == AUXR(regDP_AUX0_AUX_SW_DATA, n)) {
            uint32_t idx = (v & DP_AUX0_AUX_SW_DATA__AUX_SW_INDEX_MASK) >> DP_AUX0_AUX_SW_DATA__AUX_SW_INDEX__SHIFT;
            if (v & DP_AUX0_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK)
                S.aux_ptr = idx;
            if (!(v & DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_RW_MASK)) {
                if (S.aux_ptr < sizeof(S.aux_tx))
                    S.aux_tx[S.aux_ptr] = (uint8_t)((v & DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK) >> 8);
                S.aux_ptr++;
            }
            *R(dw) = v;
            return;
        }
        if (dw == AUXR(regDP_AUX0_AUX_SW_CONTROL, n)) {
            *R(dw) = v & ~DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK;
            if (v & DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK)
                aux_go(n);
            return;
        }
    }
    if (dw == MP1(75)) {
        /* debug mailbox: DEBUGSMC_MSG_Mode1Reset takes the chip back to the
         * PSP bootloader with VRAM intact */
        if (v == RD_DEBUGSMC_MSG_Mode1Reset)
            sim_mode1_reset();
        *R(dw) = v;
        return;
    }
    if (dw == GCR(regRLC_SAFE_MODE)) {
        /* the RLC acknowledges at once: CMD clears, MESSAGE says in/out */
        S.rlc_safe = (v >> RLC_SAFE_MODE__MESSAGE__SHIFT) & 1;
        *R(dw) = v & ~RLC_SAFE_MODE__CMD_MASK;
        return;
    }
    if (dw == GCR(regSQ_CMD)) {
        if ((v & SQ_CMD__CMD_MASK) == 3) {
            S.sq_kills++;
            S.sq_last_vmid = (v & SQ_CMD__VM_ID_MASK) >> SQ_CMD__VM_ID__SHIFT;
            if (!S.rlc_safe)
                S.sq_outside_safe++;
        }
        *R(dw) = v;
        return;
    }
    if (dw == GCR(regGRBM_SOFT_RESET)) {
        if (v & GRBM_SOFT_RESET__SOFT_RESET_CP_MASK) {
            /* CP/CPF/CPC/CPG reset: queues gone, pointers at 0, VMID reset done */
            S.soft_resets++;
            S.cp_rptr = 0;
            memset(S.hqd, 0, sizeof(S.hqd));
            *R(GCR(regCP_RB_ACTIVE)) = 0;
            *R(GCR(regCP_VMID_RESET)) = 0;
        }
        *R(dw) = v;
        return;
    }
    if (dw == GC_BASE0 + regGCVM_L2_PROTECTION_FAULT_CNTL) {
        if (v & 1)   /* CLEAR_PROTECTION_FAULT_STATUS_ADDR */
            *R(GC_BASE0 + regGCVM_L2_PROTECTION_FAULT_STATUS) = 0;
        *R(dw) = v & ~1u;
        return;
    }
    if (dw == MM_BASE0 + regMMVM_L2_PROTECTION_FAULT_CNTL) {
        if (v & 1)
            *R(MM_BASE0 + regMMVM_L2_PROTECTION_FAULT_STATUS) = 0;
        *R(dw) = v & ~1u;
        return;
    }
    if (dw == SMUIO_B0 + regROM_INDEX) {
        S.rom_index = v;
        *R(dw) = v;
        return;
    }
    *R(dw) = v;
}

struct sim_seen { unsigned n; uint32_t client, src, vmid, pasid, d3; };
static void sim_iv_cb(void *ctx, const rdna3_iv *iv)
{
    struct sim_seen *s = (struct sim_seen *)ctx;
    s->n++;
    s->client = iv->client_id;
    s->src = iv->src_id;
    s->vmid = iv->vmid;
    s->pasid = iv->pasid;
    s->d3 = iv->src_data[3];
}

struct fault_seen { unsigned n; rdna3_dev *d; rdna3_vm_fault f; };
static void fault_cb(void *ctx, const rdna3_iv *iv)
{
    struct fault_seen *s = (struct fault_seen *)ctx;
    if (!gmc11_iv_is_vm_fault(iv))
        return;
    s->n++;
    gmc11_decode_vm_fault(s->d, iv, &s->f);
    gmc11_log_vm_fault(s->d, &s->f);
}

static void sim_tick(void)
{
    if (S.sos_boot_at && S.now_us >= S.sos_boot_at) {
        S.sos_boot_at = 0;
        S.sos_alive = 1;
        *R(MP0(81)) = 0x1;
        *R(MP0(58)) = 0x00330024;
        *R(MP0(64)) = 0x80000000u;
    }
}

static uint32_t sim_rd(void *ctx, uint64_t off)
{
    uint32_t dw = (uint32_t)(off / 4);
    (void)ctx;
    sim_tick();
    if (off >= MMIO_LEN)
        return ~0u;
    if (dw == NBIO_B1 + regBIF_BX_PF0_RSMU_DATA) {
        uint32_t *p = smn_slot(S.smn_index, 0);
        return p ? *p : 0;
    }
    for (uint32_t n = 0; n < RD_AUX_ENGINES; n++) {
        if (dw == AUXR(regDP_AUX0_AUX_SW_STATUS, n))
            return S.aux_status;
        if (dw == AUXR(regDP_AUX0_AUX_SW_DATA, n)) {
            uint32_t b = S.aux_ptr < S.aux_nrx ? S.aux_rx[S.aux_ptr] : 0;
            S.aux_ptr++;
            return (S.reg[dw] & ~DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK) | (b << 8);
        }
    }
    if (dw == SMUIO_B0 + regROM_DATA) {
        /* the window maps the ROM at ROM_STRAP << 17; reads auto-increment */
        uint32_t ro = S.rom_index - (ROM_STRAP << 17), w = 0xffffffffu;
        S.rom_reads++;
        if (S.rom_index >= (ROM_STRAP << 17) && ro + 4 <= VBIOS_LEN)
            memcpy(&w, S.vbios + ro, 4);
        S.rom_index += 4;
        return w;
    }
    if (dw >= GC_BASE0 + HQD_FIRST && dw <= GC_BASE0 + HQD_LAST) {
        struct hqd *h = hqd_sel();
        if (h)
            return HQD(h, dw - GC_BASE0);
    }
    if (off / 4 == MM_BASE0 + regMMVM_INVALIDATE_ENG0_SEM +
                   17 * (regMMVM_INVALIDATE_ENG1_REQ - regMMVM_INVALIDATE_ENG0_REQ)) {
        if (S.mm_sem_held)
            return 0;             /* busy until released */
        S.mm_sem_held = 1;
        return 1;
    }
    return S.reg[off / 4];
}

static void sim_udelay(void *c, uint32_t us) { (void)c; S.now_us += us; }
static void sim_msleep(void *c, uint32_t ms) { (void)c; S.now_us += 1000ull * ms; }
static void sim_wmb(void *c) { (void)c; }
static void sim_vlog(void *c, int lvl, const char *fmt, va_list ap)
{
    (void)c;
    if (!S.verbose && lvl > RDNA3_LOG_ERR)
        return;
    fputs(lvl == RDNA3_LOG_ERR ? "  [err] " : "  ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
}

static void sim_vram_written(void *c, uint64_t off, uint64_t len)
{
    (void)c; (void)off; (void)len;
    S.pending_writes = 1;
}

/* ------------------------- fixtures ------------------------- */

static void make_discovery(amd_discovery *disc)
{
    memset(disc, 0, sizeof(*disc));
#define IP(hw, ma, mi, re, b0, b1, b2) do { amd_ip_entry *e = &disc->ips[disc->num_ips++]; \
    e->hw_id = hw; e->major = ma; e->minor = mi; e->revision = re; \
    e->num_base = e->num_base_kept = 3; e->base[0] = b0; e->base[1] = b1; e->base[2] = b2; } while (0)
    IP(AMD_HWID_GC, 11, 0, 3, GC_BASE0, 0xA000, 0x1C000);
    IP(AMD_HWID_MP0, 13, 0, 7, MP0_BASE, 0, 0);
    IP(AMD_HWID_MP1, 13, 0, 7, MP1_BASE, 0, 0);
    IP(AMD_HWID_NBIF, 4, 3, 0, NBIO_B0, NBIO_B1, NBIO_B2);
    disc->ips[disc->num_ips - 1].num_base = disc->ips[disc->num_ips - 1].num_base_kept = 6;
    disc->ips[disc->num_ips - 1].base[5] = NBIO_B5;
    IP(AMD_HWID_SMUIO, 13, 0, 6, SMUIO_B0, 0x16A00, 0x440000);
    IP(AMD_HWID_HDP, 6, 0, 0, 0xF20, 0, 0);
    IP(AMD_HWID_MMHUB, 3, 0, 2, MM_BASE0, 0, 0);
    IP(AMD_HWID_OSSSYS, 6, 0, 0, OSS_BASE0, 0, 0);
    IP(AMD_HWID_SDMA0, 6, 0, 3, GC_BASE0, 0xA000, 0x1C000);
    IP(AMD_HWID_DMU, 3, 2, 1, 0x12, 0xC0, DCN_B2);
    disc->num_dies = 1;
}

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    uint8_t *b;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* minimal synthetic firmware: SOS v2 (sys, sos, kdb, toc) + one blob per IP */
static uint8_t syn_sos[16384], syn_blob[AMD_BLOB_COUNT][8192];

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void synth(amd_fw_blob_ref blobs[AMD_BLOB_COUNT], const uint8_t **sos, size_t *sos_len)
{
    unsigned i;
    uint32_t types[4][3] = { { 2, 0, 1024 }, { 1, 1024, 2048 }, { 3, 3072, 512 }, { 4, 3584, 256 } };
    memset(syn_sos, 0xA5, sizeof(syn_sos));
    w32(syn_sos + 0, 256 + 4096); w32(syn_sos + 4, 256);
    syn_sos[8] = 2; syn_sos[9] = 0; syn_sos[10] = 0; syn_sos[11] = 0;
    w32(syn_sos + 24, 256);
    w32(syn_sos + 32, 4);
    for (i = 0; i < 4; i++) {
        w32(syn_sos + 36 + 16 * i, types[i][0]);
        w32(syn_sos + 40 + 16 * i, 1);
        w32(syn_sos + 44 + 16 * i, types[i][1]);
        w32(syn_sos + 48 + 16 * i, types[i][2]);
    }
    *sos = syn_sos;
    *sos_len = 256 + 4096;

    for (i = 0; i < AMD_BLOB_COUNT; i++) {
        uint8_t *b = syn_blob[i];
        memset(b, (int)(0x10 + i), 8192);
        memset(b, 0, 256);
        w32(b + 0, 8192); w32(b + 4, 256);
        b[8] = 1;
        w32(b + 20, 1024); w32(b + 24, 256);
        blobs[i].data = b;
        blobs[i].len = 8192;
    }
    /* header versions the plan builder insists on */
    syn_blob[AMD_BLOB_SDMA][8] = 2;
    w32(syn_blob[AMD_BLOB_SDMA] + offsetof(struct sdma_firmware_header_v2_0, ctx_ucode_size_bytes), 512);
    w32(syn_blob[AMD_BLOB_SDMA] + offsetof(struct sdma_firmware_header_v2_0, ctl_ucode_offset), 2048);
    w32(syn_blob[AMD_BLOB_SDMA] + offsetof(struct sdma_firmware_header_v2_0, ctl_ucode_size_bytes), 512);
    for (i = AMD_BLOB_PFP; i <= AMD_BLOB_MEC; i++) {
        syn_blob[i][8] = 2;
        w32(syn_blob[i] + offsetof(struct gfx_firmware_header_v2_0, ucode_size_bytes), 1024);
        w32(syn_blob[i] + offsetof(struct gfx_firmware_header_v2_0, data_size_bytes), 512);
        w32(syn_blob[i] + offsetof(struct gfx_firmware_header_v2_0, data_offset_bytes), 4096);
    }
    syn_blob[AMD_BLOB_RLC][8] = 2;
    w32(syn_blob[AMD_BLOB_IMU] + offsetof(struct imu_firmware_header_v1_0, imu_iram_ucode_size_bytes), 512);
    w32(syn_blob[AMD_BLOB_IMU] + offsetof(struct imu_firmware_header_v1_0, imu_dram_ucode_size_bytes), 512);
    for (i = AMD_BLOB_MES; i <= AMD_BLOB_MES1; i++) {
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_size_bytes), 1024);
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_offset_bytes), 256);
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_data_size_bytes), 1024);
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_data_offset_bytes), 2048);
    }
    w32(syn_blob[AMD_BLOB_DMCUB] + offsetof(struct dmcub_firmware_header_v1_0, inst_const_bytes), 1024);
}

/* ---------------------------- test ---------------------------- */

static void run(const char *fwdir)
{
    static amd_ucode_plan plan;
    static rdna3_psp psp;
    static rdna3_dev dev;
    static rdna3_gfx_ring gr;
    static rdna3_mec_ring kiq, kcq;
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT];
    amd_discovery disc;
    rdna3_os os = { NULL, sim_udelay, sim_msleep, sim_vlog, sim_wmb, sim_rd, sim_wr,
                     sim_vram_written, sim_doorbell };
    const uint8_t *sos = NULL;
    size_t sos_len = 0;
    uint32_t smu_ver = 0, ifv = 0;
    unsigned i;
    int ret;

    memset(&S, 0, sizeof(S));
    S.verbose = getenv("SIM_VERBOSE") != NULL;
    S.vram = calloc(1, BAR_LEN);
    *R(MP0(35)) = 0x80000000u;  /* bootloader ready */
    make_vbios();
    make_sink();
    *smn_slot(SMN_ROM_OFFSET_CTRL, 1) = ROM_STRAP;
    *R(MP0(33)) = 0x80000000u;  /* IFWI done */
    *R(mmRCC_CONFIG_MEMSIZE) = (uint32_t)(VRAM_SIZE >> 20);
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_BASE) = (uint32_t)(FB_START >> 24);
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_TOP) = (uint32_t)((FB_START + VRAM_SIZE - 1) >> 24);
    *R(GC_BASE0 + regGCMC_VM_FB_OFFSET) = 0;

    memset(blobs, 0, sizeof(blobs));
    if (fwdir) {
        sos = slurp(fwdir, amd_navi32_sos_name, &sos_len);
        for (i = 0; i < AMD_BLOB_COUNT; i++)
            blobs[i].data = slurp(fwdir, amd_navi32_fw_names[i], &blobs[i].len);
        CHECK(sos != NULL);
        if (!sos)
            return;
    } else {
        synth(blobs, &sos, &sos_len);
    }
    CHECK(amd_ucode_plan_build(blobs, &plan) == AMD_FW_OK);

    rdna3_dev_init(&dev, &os, MMIO_LEN);
    dev.usec_timeout = 100000; /* amdgpu default; virtual time is free */
    dev.vram_size = VRAM_SIZE;
    dev.vram_cpu = S.vram;
    dev.vram_cpu_len = BAR_LEN;
    make_discovery(&disc);
    CHECK(rdna3_dev_set_discovery(&dev, &disc) == RDNA3_OK);
    CHECK(rdna3_read_fb_location(&dev) == RDNA3_OK);
    CHECK(dev.fb_start == FB_START);
    rdna3_nbio_remap_hdp(&dev);
    CHECK(S.reg[NBIO_B2 + regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL] == RDNA3_MMIO_REG_HOLE_OFFSET);

    /* doorbell BAR: 2 MiB at a made-up bus address; without a CPU mapping
     * the os hook takes the writes (the kext maps BAR2 instead) */
    CHECK(nbio43_doorbell_init(&dev) == RDNA3_ENODEV && !dev.doorbell_ok);
    dev.doorbell_bus = 0xF0000000ull;
    dev.doorbell_len = 2u << 20;
    CHECK(nbio43_doorbell_init(&dev) == RDNA3_OK && dev.doorbell_ok);
    CHECK(S.reg[NBIO_B2 + regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN] & RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN_MASK);
    CHECK(S.reg[NBIO_B2 + regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW] == 0xF0000000u);
    CHECK(S.reg[NBIO_B2 + regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL] & BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_EN_MASK);
    rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);

    CHECK(psp13_init(&psp, &dev, sos, sos_len) == RDNA3_OK);
    ret = psp13_hw_init(&psp, &plan, blobs);
    CHECK(ret == RDNA3_OK);

    /* bootloader order mirrors psp_hw_start(): KDB, SPL(kdb), SYS, SOS */
    CHECK(S.nbl >= 3);
    CHECK(S.bl_cmds[0] == 0x80000u);
    CHECK(S.bl_cmds[S.nbl - 1] == 0x20000u);
    CHECK(S.sos_alive && psp.sos_fw_version == 0x00330024);
    CHECK(S.toc_loaded && S.tmr_setup && S.tmr_after_smu);
    CHECK(S.nipfw == plan.count);
    CHECK(S.ip_fw_types[0] == GFX_FW_TYPE_SMU);
    CHECK(S.autoload && S.autoload_after_rlc_g);
    CHECK(S.unflushed_doorbells == 0);

    CHECK(smu13_hello(&dev, &smu_ver, &ifv) == RDNA3_OK);
    CHECK(smu_ver == 0x00526100 && ifv == 0x35);
    CHECK(smu13_send_msg(&dev, 0x77, 0, NULL) != RDNA3_OK);

    /* VBIOS through the SMUIO ROM window, then the power-management
     * bring-up (smu_smc_hw_setup) against the PMFW model */
    {
        static uint8_t rom[RD_VBIOS_MAX_LEN];
        static rdna3_vbios vb;
        static rdna3_smu smu;
        rdna3_vbios_boot boot;
        uint64_t rs = 0, rsz = 0;
        uint32_t w = 0;

        CHECK(vbios_read(&dev, &vb, rom, sizeof(rom)) == RDNA3_OK);
        CHECK(vb.len == VBIOS_LEN && vb.rom_offset == (ROM_STRAP << 17) && vb.data_table == VB_DATA_DIR);
        CHECK(S.rom_reads == 16 + VBIOS_LEN / 4);
        CHECK(strcmp(vb.part_number, "RX 7800 XT SIM") == 0);
        CHECK(vbios_data_table(&vb, RD_ATOM_DT_POWERPLAYINFO, NULL, NULL, NULL) == NULL);
        CHECK(vbios_bootup_values(&dev, &vb, &boot) == RDNA3_OK);
        CHECK(boot.gfxclk == 50000 && boot.uclk == 9600 && boot.socclk == 60000 &&
              boot.fclk == 100000 && boot.vclk == 50000 && boot.vddc == 800 && boot.fw_crev == 4 &&
              boot.firmware_revision == 0x02000f && boot.pp_table_id == 0);
        CHECK(vbios_fw_vram_reservation(&vb, &rs, &rsz) == RDNA3_OK &&
              rs == VRAM_SIZE - (48ull << 20) && rsz == (48ull << 20));
        CHECK(smu13_check_fw_status(&dev) == RDNA3_OK);

        /* the default (no Display Core) feature set on a Gen4 x16 link */
        CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_SAFE, 3, 6) == RDNA3_OK);
        CHECK(S.smu.driver_addr == rdna3_vram_mc(&dev, smu.driver_table));
        CHECK(S.smu.tool_addr == rdna3_vram_mc(&dev, smu.tool_table));
        CHECK(S.smu.pptable_written && S.smu.btc && !S.smu.bad_board && S.smu.unflushed == 0 &&
              S.smu.bad_addr == 0);
        CHECK(smu.pp_frev == RD_SMU_PP_TABLE_FORMAT_REVISION && smu.platform_caps == 0x19 &&
              smu.thermal_controller == 28 && smu.software_shutdown_temp == 118);
        CHECK(smu.pptable.SkuTable.Version == PMFW_SKU_VERSION);
        CHECK(smu.board_table_appended && smu.pptable.BoardTable.Version == VB_BOARD_VERSION &&
              smu.pptable.BoardTable.VddGfxVrMapping == 0x11);
        CHECK((((uint64_t)S.smu.allowed_hi << 32) | S.smu.allowed_lo) == RD_SMU_FEATURES_SAFE);
        CHECK(smu.enabled == RD_SMU_FEATURES_SAFE && smu.dpm_running);
        CHECK(!(smu.enabled & RD_SMU_FEATURE(FEATURE_GFXOFF_BIT)));
        CHECK(smu.gfx.count == 2 && smu.gfx.fine_grained && smu.gfx.level[0] == 500 &&
              smu.gfx.level[1] == 2124);
        /* UCLK/FCLK DPM not allowed: the boot clocks from the VBIOS */
        CHECK(smu.uclk.count == 1 && smu.uclk.level[0] == 96);
        CHECK(smu.fclk.count == 1 && smu.fclk.level[0] == 1000);
        CHECK(smu.soc.count == 3 && smu.soc.level[2] == 1200);
        CHECK(S.smu.pcie_overrides == 0 && smu.pcie_levels == 3);
        CHECK(smu.ppt_limit_w == 263);
        CHECK(S.smu.workload == (1u << WORKLOAD_PPLIB_DEFAULT_BIT));

        CHECK(smu13_read_metrics(&dev, &smu) == RDNA3_OK && smu.metrics_valid);
        CHECK(smu13_metric_gfxclk_mhz(&smu) == 2050 && smu13_metric_uclk_mhz(&smu) == 96);
        CHECK(smu13_metric_temp_edge_c(&smu) == 45 && smu13_metric_temp_hotspot_c(&smu) == 60 &&
              smu13_metric_temp_mem_c(&smu) == 50);
        CHECK(smu13_metric_power_w(&smu) == 150 && smu13_metric_gfx_activity(&smu) == 12 &&
              smu13_metric_fan_rpm(&smu) == 900 && smu13_metric_vddgfx_mv(&smu) == 850);
        CHECK(smu13_metric_throttlers(&smu) == 0);

        CHECK(smu13_set_power_limit(&dev, &smu, 300) == RDNA3_EINVAL);   /* above MsgLimits */
        CHECK(smu13_set_power_limit(&dev, &smu, 270) == RDNA3_OK && S.smu.ppt_limit == 270);
        CHECK(smu13_get_power_limit(&dev, &smu, &w) == RDNA3_OK && w == 270);
        CHECK(smu13_set_soft_freq_range(&dev, &smu, PPCLK_GFXCLK, 1000, 2000) == RDNA3_OK &&
              S.smu.soft_limits == 2);
        CHECK(smu13_set_soft_freq_range(&dev, &smu, PPCLK_UCLK, 500, 0) != RDNA3_OK);

        /* amdgpu's full set on a Gen3 x8 host: UCLK DPM comes up, DCN DPM
         * is refused by the firmware, the three gen4/x16 link levels are
         * clamped by OverridePcieParameters */
        CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_AMDGPU, 2, 4) == RDNA3_OK);
        CHECK(smu.enabled == (RD_SMU_FEATURES_AMDGPU & ~RD_SMU_FEATURE(FEATURE_DPM_DCN_BIT)));
        CHECK(smu.uclk.count == 4 && smu.uclk.level[3] == 1218 && !smu.uclk.fine_grained);
        CHECK(smu.fclk.count == 2 && smu.fclk.level[1] == 1800);
        CHECK(S.smu.pcie_overrides == 3 && S.smu.last_pcie_arg == ((2u << 16) | (2u << 8) | 4));
        CHECK(smu13_read_metrics(&dev, &smu) == RDNA3_OK && smu13_metric_uclk_mhz(&smu) == 1218);
        CHECK(smu13_pm_fini(&dev, &smu) == RDNA3_OK && S.smu.unload);

        /* without a VBIOS the PMFW's placeholder BoardTable goes back as is */
        CHECK(smu13_pm_init(&dev, &smu, NULL, RD_SMU_FEATURES_SAFE, 3, 6) == RDNA3_OK);
        CHECK(!smu.board_table_appended && S.smu.bad_board &&
              smu.pptable.BoardTable.Version == PMFW_BOARD_PLACEHOLDER);
        CHECK(smu.gfx.count == 2 && smu.uclk.count == 1 && smu.uclk.level[0] == 0);
    }

    /* GMC: GART in VRAM, GFXHUB VMID0 + contexts, TLB flush */
    {
        static rdna3_gmc gmc;
        uint64_t pte = 0;
        CHECK(gmc11_init(&dev, &gmc, 0) == RDNA3_OK);
        CHECK(gmc.gart_start == 0 && gmc.gart_size == (512ull << 20));
        CHECK(gfxhub303_gart_enable(&dev, &gmc) == RDNA3_OK);
        gfxhub303_set_fault_enable_default(&dev, 1);
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_CNTL] & GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK);
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32] ==
              (uint32_t)(rdna3_vram_pa(&dev, gmc.gart_table) | 1));
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32] ==
              (uint32_t)(gmc.gart_end >> 12));
        CHECK((S.reg[GC_BASE0 + regGCVM_CONTEXT1_CNTL] & GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH_MASK) >>
              GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT == 3);
        CHECK(S.reg[GC_BASE0 + regGCVM_L2_CNTL] & GCVM_L2_CNTL__ENABLE_L2_CACHE_MASK);
        CHECK(gmc11_gart_map(&dev, &gmc, 0x10000, 0x123456000ull, 2,
                             RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                             RD_PTE_WRITEABLE) == RDNA3_OK);
        memcpy(&pte, S.vram + gmc.gart_table + (0x10000 / 4096 + 1) * 8, 8);
        CHECK((pte & 0x0000FFFFFFFFF000ull) == 0x123457000ull && (pte & RD_PTE_VALID));
        CHECK(gfxhub303_flush_tlb(&dev, 0, 0) == RDNA3_OK && S.tlb_flushes == 1);
        CHECK(gmc11_gart_map(&dev, &gmc, gmc.gart_size, 0, 1, RD_PTE_VALID) == RDNA3_EINVAL);
        CHECK(S.unflushed_doorbells == 0);

        /* MMHUB 3.0.2: gmc_v11_0_gart_enable() */
        CHECK(gmc11_gart_enable(&dev, &gmc) == RDNA3_OK && gmc.mmhub_enabled);
        CHECK(S.reg[MM_BASE0 + regMMVM_CONTEXT0_CNTL] & MMVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK);
        CHECK(S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32] ==
              (uint32_t)(rdna3_vram_pa(&dev, gmc.gart_table) | 1));
        CHECK(S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32] ==
              (uint32_t)(gmc.gart_end >> 12));
        CHECK(S.reg[MM_BASE0 + regMMVM_L2_CNTL] & MMVM_L2_CNTL__ENABLE_L2_CACHE_MASK);
        CHECK(S.reg[MM_BASE0 + regMMMC_VM_MX_L1_TLB_CNTL] & MMMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK);
        CHECK(S.reg[MM_BASE0 + regMMVM_L2_PROTECTION_FAULT_CNTL] &
              MMVM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK);
        CHECK(S.mm_tlb_flushes == 1 && S.mm_flush_without_sem == 0 && !S.mm_sem_held);
        CHECK(S.mm_private_inv == 1);
        CHECK(mmhub302_get_fb_location(&dev) == 0);   /* MMMC FB base not modelled */
        CHECK(S.unflushed_doorbells == 0);

        /* IH 6.0: ring in VRAM, polled */
        {
            static rdna3_ih ih;
            uint32_t iv[8] = { 0x0a | (0x5u << 8) | (2u << 16) | (3u << 24), 0x1234, 0x8000005, 0x00010007,
                               0x11, 0x22, 0x33, 0x44 };
            uint32_t wptr, cntl;
            struct sim_seen seen = { 0, 0, 0, 0, 0, 0 };
            CHECK(ih60_init(&dev, &ih, 4096, gmc.dummy_page_pa, 0) == RDNA3_OK && ih.enabled);
            cntl = S.reg[OSS_BASE0 + regIH_RB_CNTL];
            CHECK(cntl & IH_RB_CNTL__RB_ENABLE_MASK);
            CHECK(cntl & IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK);
            CHECK(((cntl & IH_RB_CNTL__RB_SIZE_MASK) >> IH_RB_CNTL__RB_SIZE__SHIFT) == 10); /* 4 KiB / 4 */
            CHECK(((cntl & IH_RB_CNTL__MC_SPACE_MASK) >> IH_RB_CNTL__MC_SPACE__SHIFT) == 4);
            CHECK(S.reg[OSS_BASE0 + regIH_RB_BASE] == (uint32_t)(rdna3_vram_mc(&dev, ih.ring) >> 8));
            CHECK(S.reg[OSS_BASE0 + regIH_RB_WPTR_ADDR_LO] == (uint32_t)rdna3_vram_mc(&dev, ih.wptr_wb));
            /* the "hardware" writes three IVs, the third one wrapping the 4 KiB ring */
            ih.rptr = 4096 - 64;
            S.reg[OSS_BASE0 + regIH_RB_RPTR] = ih.rptr;
            memcpy(S.vram + ih.ring + 4096 - 64, iv, 32);
            memcpy(S.vram + ih.ring + 4096 - 32, iv, 32);
            memcpy(S.vram + ih.ring, iv, 32);
            wptr = 32;
            memcpy(S.vram + ih.wptr_wb, &wptr, 4);
            CHECK(ih60_process(&dev, &ih, sim_iv_cb, &seen) == 3);
            CHECK(seen.n == 3 && seen.client == 0x0a && seen.src == 5 && seen.vmid == 3 &&
                  seen.pasid == 7 && seen.d3 == 0x44);
            CHECK(ih.rptr == 32 && S.reg[OSS_BASE0 + regIH_RB_RPTR] == 32);
            CHECK(ih60_process(&dev, &ih, sim_iv_cb, &seen) == 0);
            /* overflow: writeback and register both flag it; skip past wptr + 32 */
            wptr = 64 | IH_RB_WPTR__RB_OVERFLOW_MASK;
            memcpy(S.vram + ih.wptr_wb, &wptr, 4);
            S.reg[OSS_BASE0 + regIH_RB_WPTR] = wptr;
            CHECK(ih60_process(&dev, &ih, sim_iv_cb, &seen) == 4064 / 32);
            CHECK(ih.overflows == 1 && ih.rptr == 64);
            ih60_disable(&dev, &ih);
            CHECK(!ih.enabled && !(S.reg[OSS_BASE0 + regIH_RB_CNTL] & IH_RB_CNTL__RB_ENABLE_MASK));
        }

        /* SDMA 6.0: ring + test, copy, fill, fences across the ring wrap */
        {
            static rdna3_sdma_ring sd;
            uint64_t src, dst;
            uint32_t pat[256], out[256], j, k;
            S.reg[0xA000 + regSDMA0_F32_CNTL] = SDMA0_F32_CNTL__HALT_MASK;  /* reset state */
            CHECK(sdma60_start(&dev, &sd, 0, 4096) == RDNA3_OK && sd.ready);
            CHECK(S.sdma_while_halted == 0 && S.sdma_bad == 0);
            /* doorbell: QUEUE0_DOORBELL enabled at index 0x200, S2A port 2
             * range programmed through the RSMU window, ring test went
             * through the doorbell */
            CHECK(sd.use_doorbell && sd.doorbell == RD_DB_INDEX(RD_DB_SDMA_ENGINE0));
            CHECK(S.reg[GC_BASE0 + regSDMA0_QUEUE0_DOORBELL] & SDMA0_QUEUE0_DOORBELL__ENABLE_MASK);
            CHECK(((S.reg[GC_BASE0 + regSDMA0_QUEUE0_DOORBELL_OFFSET] & SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK) >> 2) == sd.doorbell);
            {
                uint32_t s2a = smn_val(regS2A_DOORBELL_ENTRY_2_CTRL);
                CHECK(s2a & S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_ENABLE_MASK);
                CHECK(((s2a & S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET_MASK) >>
                       S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET__SHIFT) == sd.doorbell);
                CHECK(((s2a & S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE_MASK) >>
                       S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE__SHIFT) == 40);
            }
            CHECK(S.db_writes >= 1 && S.db_last_index == sd.doorbell && S.db_unclaimed == 0);
            CHECK(S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_BASE] == (uint32_t)(rdna3_vram_mc(&dev, sd.ring) >> 8));
            CHECK(S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_CNTL] & SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK);
            src = rdna3_vram_alloc(&dev, 8192, 4096, "sdma-test-src");
            dst = rdna3_vram_alloc(&dev, 8192, 4096, "sdma-test-dst");
            for (j = 0; j < 256; j++) pat[j] = 0x5a000000u + j;
            rdna3_vram_write(&dev, src, pat, sizeof(pat));
            for (k = 0; k < 40; k++) {  /* 40 x 11 dwords > 1024-dword ring: wraps */
                CHECK(sdma60_copy_sync(&dev, &sd, rdna3_vram_mc(&dev, src), rdna3_vram_mc(&dev, dst),
                                       sizeof(pat)) == RDNA3_OK);
            }
            rdna3_vram_read(&dev, dst, out, sizeof(out));
            CHECK(!memcmp(out, pat, sizeof(pat)));
            sdma60_emit_fill(&dev, &sd, rdna3_vram_mc(&dev, dst), 0xabcdef01u, 64);
            k = sdma60_emit_fence(&dev, &sd);
            sdma60_commit(&dev, &sd);
            CHECK(k == 41 && sdma60_wait_fence(&dev, &sd, k) == RDNA3_OK);
            rdna3_vram_read(&dev, dst, out, 64);
            CHECK(out[0] == 0xabcdef01u && out[15] == 0xabcdef01u && out[16] == pat[16]);
            CHECK(S.sdma_bad == 0 && S.unflushed_doorbells == 0);
            /* INDIRECT packet layout and 8-dword alignment */
            {
                uint32_t pkt[6], start;
                sdma60_write(&dev, &sd, 0);          /* knock wptr off alignment */
                sdma60_emit_ib(&dev, &sd, 5, 0x123456789A0ull, 48);
                CHECK(((uint32_t)sd.wptr & 7) == 0);
                start = ((uint32_t)sd.wptr - 6) & sd.buf_mask;
                rdna3_vram_read(&dev, sd.ring + 4ull * start, pkt, sizeof(pkt));
                CHECK(pkt[0] == (SDMA_OP_INDIRECT | (5u << 16)) && pkt[1] == 0x456789A0u &&
                      pkt[2] == 0x123 && pkt[3] == 48 && pkt[4] == 0 && pkt[5] == 0);
                CHECK(!sdma60_fence_passed(&dev, &sd, sd.fence_seq + 1));
                CHECK(sdma60_fence_passed(&dev, &sd, sd.fence_seq));
            }
            sdma60_stop(&dev, &sd);
            CHECK(!sd.ready && (S.reg[0xA000 + regSDMA0_F32_CNTL] & SDMA0_F32_CNTL__HALT_MASK));
        }

        /* GFX 11: RLC autoload done, RS64 start addresses, GFXHUB, clear state,
         * ring 0 test through SCRATCH_REG0, fence via RELEASE_MEM */
        {
            rdna3_rs64_start rs = { 0x1000, 0x2, 0x2000, 0x3, 0x4000, 0x1 };
            uint32_t k;
            S.reg[GCR(regCP_ME_CNTL)] = CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK;
            S.reg[GCR(regRLC_RLCS_BOOTLOAD_STATUS)] = 0;
            CHECK(gfx11_hw_init(&dev, &gmc, &gr, &rs, 8192) == RDNA3_ETIME);   /* autoload not done */
            S.reg[GCR(regRLC_RLCS_BOOTLOAD_STATUS)] = RLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK;
            CHECK(gfx11_hw_init(&dev, &gmc, &gr, &rs, 8192) == RDNA3_OK && gr.ready);
            CHECK(gmc.gfxhub_enabled);
            /* gfx ring 0 through its doorbell; GC doorbell ranges and the
             * S2A entries 0/3 (nbio_v4_3_gc_doorbell_init) programmed */
            CHECK(gr.use_doorbell && gr.doorbell == RD_DB_INDEX(RD_DB_GFX_RING0));
            CHECK(S.reg[GCR(regCP_RB_DOORBELL_CONTROL)] & CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK);
            CHECK(((S.reg[GCR(regCP_RB_DOORBELL_CONTROL)] & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == gr.doorbell);
            CHECK(S.reg[GCR(regCP_RB_DOORBELL_RANGE_LOWER)] == (RD_DB_INDEX(RD_DB_GFX_RING0) << 2));
            CHECK(S.reg[GCR(regCP_MEC_DOORBELL_RANGE_UPPER)] == (RD_DB_INDEX(RD_DB_USERQUEUE_END) << 2));
            CHECK(smn_val(regS2A_DOORBELL_ENTRY_0_CTRL) == 0x30000007u && smn_val(regS2A_DOORBELL_ENTRY_3_CTRL) == 0x3000000du);
            CHECK(S.db_last_index == gr.doorbell && S.db_unclaimed == 0);
            CHECK(S.reg[GCR(regCP_ME_PRGRM_CNTR_START)] == ((0x3u << 30) | (0x2000 >> 2)));
            CHECK(S.reg[GCR(regCP_MEC_RS64_PRGRM_CNTR_START_HI)] == 0);
            CHECK(S.reg[0xC000 + 0x40] == 0xDEADBEEF);       /* SCRATCH_REG0 via SET_UCONFIG_REG */
            CHECK(S.cp_clear_state == 1 && S.cp_ctx_regs == 934 + 1 && S.cp_bad == 0 &&
                  S.cp_while_halted == 0);
            CHECK(gfx11_csb_size() == 960);
            CHECK(S.reg[GCR(regRLC_CSIB_LENGTH)] == 960);
            for (k = 0; k < 600; k++) {   /* 600 x 8 dw > 2048-dw ring: wraps */
                uint32_t seq = gfx11_emit_fence(&dev, &gr);
                gfx11_commit(&dev, &gr);
                CHECK(gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
            }
            CHECK(gr.fence_seq == 600 && S.cp_bad == 0 && S.unflushed_doorbells == 0);
        }

        /* GPUVM: 4-level tables, mapping across 2 MiB and 1 GiB boundaries */
        {
            static rdna3_pt_pool pool;
            static rdna3_vm vm;
            const uint64_t va = 0x00007FFFBFF00000ull;   /* crosses a PDB1 (1 GiB) boundary */
            const uint64_t pa = 0x123400000ull;
            uint64_t vpte = gpuvm_pte_flags((1u << 1) | (1u << 2) | (4u << 5), 0), e;
            CHECK(gpuvm_pool_init(&dev, &pool, 64) == RDNA3_OK && pool.nfree == 64);
            CHECK(gpuvm_vm_init(&dev, &vm, &pool) == RDNA3_OK && vm.tables == 1);
            CHECK((vpte & RD_PTE_VALID) && (vpte & RD_PTE_READABLE) && (vpte & RD_PTE_WRITEABLE) &&
                  !(vpte & RD_PTE_EXECUTABLE) && (vpte >> 48) == RD_MTYPE_UC);
            CHECK(gpuvm_map_linear(&dev, &vm, va, pa, 1024, vpte) == RDNA3_OK);
            /* root + 1 PDB1 + 2 PDB0 (one per GiB) + 3 PTB (2 MiB blocks: 1 + 2 + 1 MiB) */
            CHECK(vm.tables == 1 + 1 + 2 + 3);
            CHECK(gpuvm_lookup(&dev, &vm, va) == pa);
            CHECK(gpuvm_lookup(&dev, &vm, va + 0x100000 + 0x123) == pa + 0x100000 + 0x123);
            CHECK(gpuvm_lookup(&dev, &vm, va + 1023ull * 4096) == pa + 1023ull * 4096);
            CHECK(gpuvm_lookup(&dev, &vm, va + 1024ull * 4096) == 0);
            /* sign-extended high-half addresses hit the same tables */
            CHECK(gpuvm_lookup(&dev, &vm, va | 0xFFFF000000000000ull) == pa);
            /* the root PDE points at a table inside the pool */
            rdna3_vram_read(&dev, vm.root + 8ull * ((va >> 39) & 511), &e, 8);
            CHECK((e & RD_PTE_VALID) && (e & 0x0000FFFFFFFFF000ull) - rdna3_vram_pa(&dev, 0) >= pool.base);
            gpuvm_unmap(&dev, &vm, va + 4096, 1);
            CHECK(gpuvm_lookup(&dev, &vm, va + 4096) == 0 && gpuvm_lookup(&dev, &vm, va) == pa);
            CHECK(gpuvm_map_linear(&dev, &vm, va + 1, pa, 1, vpte) == RDNA3_EINVAL);
            gpuvm_vm_fini(&dev, &vm);
            CHECK(vm.tables == 0 && pool.nfree == 64);
            gfxhub303_setup_vm_pt_regs(&dev, 3, 0x123456789000ull | 1);
            CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 +
                        3 * (regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 -
                             regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32)] == 0x56789001u);
            /* exhaustion is reported, not corrupting */
            CHECK(gpuvm_vm_init(&dev, &vm, &pool) == RDNA3_OK);
            CHECK(gpuvm_map_linear(&dev, &vm, 0x1000000000ull, pa, 512ull * 70, vpte) == RDNA3_ENOMEM);
            gpuvm_vm_fini(&dev, &vm);
            CHECK(pool.nfree == 64);

            /* a GFX job in VMID 5 (amdgpu_ib_schedule order): VM flush from
             * the ring, ACQUIRE_MEM, HDP flush, CONTEXT_CONTROL, framed IBs
             * fetched through the page tables, fence */
            CHECK(gr.ready);
            if (gr.ready) {
                const uint64_t ibva = 0x0000400000001000ull, dstva = 0x0000400000200000ull;
                uint64_t ibpage = rdna3_vram_alloc(&dev, 4096, 4096, "test-ib");
                uint64_t dst = rdna3_vram_alloc(&dev, 4096, 4096, "test-dst");
                uint64_t f = gpuvm_pte_flags((1u << 1) | (1u << 2) | (1u << 3), 0);
                uint32_t ib[16], n = 0, got = 0, seq, scratch = 0xC000 + 0x40;
                rdna3_gfx_ib jobs[2];
                CHECK(gpuvm_vm_init(&dev, &vm, &pool) == RDNA3_OK);
                CHECK(gpuvm_map_linear(&dev, &vm, ibva, rdna3_vram_pa(&dev, ibpage), 1, f) == RDNA3_OK);
                CHECK(gpuvm_map_linear(&dev, &vm, dstva, rdna3_vram_pa(&dev, dst), 1, f) == RDNA3_OK);
                ib[n++] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
                ib[n++] = 0x40;                       /* SCRATCH_REG0 */
                ib[n++] = 0x5EED1234;
                ib[n++] = PACKET3(PACKET3_WRITE_DATA, 3);
                ib[n++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
                ib[n++] = (uint32_t)rdna3_vram_mc(&dev, dst);
                ib[n++] = (uint32_t)(rdna3_vram_mc(&dev, dst) >> 32);
                ib[n++] = 0xFACEFEED;
                rdna3_vram_write(&dev, ibpage, ib, 4ull * n);
                jobs[0].va = ibva; jobs[0].ndw = n; jobs[0].flags = 0;
                jobs[1].va = ibva; jobs[1].ndw = 3; jobs[1].flags = 0;
                S.reg[scratch] = 0;
                seq = gfx11_submit_job(&dev, &gr, 0xC0FFEE, 5, gpuvm_pd_addr(&dev, &vm), jobs, 2);
                CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                rdna3_vram_read(&dev, dst, &got, 4);
                CHECK(got == 0xFACEFEED && S.reg[scratch] == 0x5EED1234);
                CHECK(S.cp_ibs == 2 && S.cp_ib_dw == n + 3 && S.cp_last_ib_vmid == 5 && !S.cp_ib_fault);
                CHECK(S.ring_tlb_flushes == 1 && S.cp_pfp_sync == 1 && S.cp_acquire == 1 &&
                      S.hdp_engine_flushes == 1 && S.cp_frame_begin == 1 && S.cp_frame_end == 1);
                CHECK(S.cp_last_cntxcntl == 0x81018003u);   /* load_enable + context switch */
                CHECK(S.cp_wait_stuck == 0 && S.cp_bad == 0);
                /* same context, no VM change: no flush, no reload */
                seq = gfx11_submit_job(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1);
                CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                CHECK(S.ring_tlb_flushes == 1 && S.cp_last_cntxcntl == 0x80000000u && S.cp_ibs == 3);
                /* an unmapped IB faults instead of running garbage */
                jobs[0].va = 0x0000500000000000ull;
                seq = gfx11_submit_job(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1);
                CHECK(seq != 0 && S.cp_ib_fault == 1 && S.fault_ivs == 0);
                /* with the fault interrupts on the same fault reaches the IH
                 * ring as a VM fault IV the GMC decoder attributes to the
                 * GFXHUB, VMID 5 and the faulting page; the decoder clears
                 * the L2 status */
                {
                    static rdna3_ih ih3;
                    struct fault_seen fs;
                    uint32_t ctx5 = GC_BASE0 + regGCVM_CONTEXT0_CNTL +
                                    5 * (regGCVM_CONTEXT1_CNTL - regGCVM_CONTEXT0_CNTL);
                    memset(&fs, 0, sizeof(fs));
                    fs.d = &dev;
                    CHECK(ih60_init(&dev, &ih3, 4096, gmc.dummy_page_pa, 0) == RDNA3_OK);
                    gmc11_set_vm_fault_interrupts(&dev, 1);
                    CHECK(S.reg[ctx5] & GCVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK);
                    CHECK(S.reg[MM_BASE0 + regMMVM_CONTEXT0_CNTL + 5 * (regMMVM_CONTEXT1_CNTL - regMMVM_CONTEXT0_CNTL)] &
                          MMVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK);
                    seq = gfx11_submit_job(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1);
                    CHECK(seq != 0 && S.cp_ib_fault == 2 && S.fault_ivs == 1);
                    CHECK(ih60_process(&dev, &ih3, fault_cb, &fs) == 1);
                    CHECK(fs.n == 1 && fs.f.hub == 0 && fs.f.vmid == 5 && fs.f.client == RD_IH_CLIENT_GFX);
                    CHECK(fs.f.addr == 0x0000500000000000ull && fs.f.mapping_error && fs.f.cid == 0x12 &&
                          !fs.f.write && !fs.f.retry && fs.f.status != 0);
                    CHECK(S.reg[GC_BASE0 + regGCVM_L2_PROTECTION_FAULT_STATUS] == 0);
                    gmc11_set_vm_fault_interrupts(&dev, 0);
                    CHECK(!(S.reg[ctx5] & GCVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK));
                    seq = gfx11_submit_job(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1);
                    CHECK(seq != 0 && S.cp_ib_fault == 3 && S.fault_ivs == 1);
                }
                CHECK(gfx11_submit_job(&dev, &gr, 1, 16, 0, jobs, 1) == 0);
                CHECK(gfx11_submit_job(&dev, &gr, 1, 5, 0, jobs, 0) == 0);
                /* many jobs: the ring wraps and space accounting holds */
                jobs[0].va = ibva;
                for (uint32_t k = 0; k < 300; k++) {
                    seq = gfx11_submit_job(&dev, &gr, k & 1, 5, 0, jobs, 1);
                    CHECK(seq != 0);
                }
                CHECK(gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK && S.cp_bad == 0 &&
                      S.cp_wait_stuck == 0 && gfx11_ring_space(&dev, &gr) == gr.buf_mask);

                /* KIQ (MEC1 pipe 1 queue 0) through its HQD registers, then
                 * KCQ0 (pipe 0 queue 0) mapped by the KIQ's MAP_QUEUES; a
                 * compute job runs the same IB through the page tables */
                {
                    uint32_t ibs0 = S.cp_ibs, tlb0 = S.ring_tlb_flushes, sync0 = S.cp_pfp_sync, acq0 = S.cp_acquire;
                    uint32_t fault0 = S.cp_ib_fault;   /* the gfx test faulted one IB on purpose */
                    struct hqd *hk = &S.hqd[RD_KIQ_PIPE][0], *hc = &S.hqd[RD_KCQ0_PIPE][0];
                    CHECK(gfx11_mec_ring_init(&dev, &kiq, 1, RD_KIQ_PIPE, 0, RD_DB_KIQ, 0) == RDNA3_OK);
                    CHECK(gfx11_mec_ring_init(&dev, &kcq, 0, RD_KCQ0_PIPE, 0, RD_DB_MEC_RING0, 0) == RDNA3_OK);
                    CHECK(kiq.rb.kind == RD_RING_KIQ && kcq.rb.kind == RD_RING_COMPUTE && kcq.rb.doorbell == 6);
                    CHECK(gfx11_kcq_start(&dev, &kiq, &kcq) == RDNA3_EINVAL);   /* KIQ first */
                    CHECK(gfx11_kiq_start(&dev, &kiq) == RDNA3_OK && kiq.rb.ready);
                    CHECK((S.reg[GCR(regRLC_CP_SCHEDULERS)] & 0xff) == (0x80 | (1u << 5) | (RD_KIQ_PIPE << 3)));
                    CHECK(!(S.reg[GCR(regCP_MEC_RS64_CNTL)] & CP_MEC_RS64_CNTL__MEC_HALT_MASK));
                    CHECK(S.reg[GCR(regCP_MEC_RS64_CNTL)] & CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK);
                    CHECK(HQD(hk, regCP_HQD_ACTIVE) == 1 && !hk->mapped_by_kiq);
                    CHECK(HQD(hk, regCP_HQD_PQ_BASE) == (uint32_t)(rdna3_vram_mc(&dev, kiq.rb.ring) >> 8));
                    CHECK(((HQD(hk, regCP_HQD_PQ_DOORBELL_CONTROL) & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == kiq.rb.doorbell);
                    CHECK(HQD(hk, regCP_MQD_BASE_ADDR) == (uint32_t)rdna3_vram_mc(&dev, kiq.mqd));
                    CHECK(S.reg[GCR(regGRBM_GFX_CNTL)] == 0);   /* selection restored */
                    CHECK(S.reg[0xC000 + 0x40] == 0xDEADBEEF && S.cp_bad == 0 && S.mec_runs == 1);
                    CHECK(gfx11_kcq_start(&dev, &kiq, &kcq) == RDNA3_OK && kcq.rb.ready && kcq.mapped);
                    CHECK(S.kiq_maps == 1 && S.set_resources_mask == 1ull && S.mec_bad_queue == 0);
                    CHECK(HQD(hc, regCP_HQD_ACTIVE) == 1 && hc->mapped_by_kiq);
                    CHECK(HQD(hc, regCP_HQD_PQ_BASE) == (uint32_t)(rdna3_vram_mc(&dev, kcq.rb.ring) >> 8));
                    CHECK(((HQD(hc, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK)) == 9);   /* 1024 dw */
                    CHECK(HQD(hc, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK);
                    CHECK(S.cp_bad == 0 && S.db_unclaimed == 0);
                    /* the compute job */
                    rdna3_vram_fill(&dev, dst, 0, 4096);
                    S.reg[scratch] = 0;
                    jobs[0].va = ibva; jobs[0].ndw = n;
                    seq = gfx11_submit_compute_job(&dev, &kcq, 0xC0FFEE, 5, gpuvm_pd_addr(&dev, &vm), jobs, 1);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &kcq.rb, seq) == RDNA3_OK);
                    rdna3_vram_read(&dev, dst, &got, 4);
                    CHECK(got == 0xFACEFEED && S.reg[scratch] == 0x5EED1234);
                    CHECK(S.cp_ibs == ibs0 + 1 && S.cp_last_ib_vmid == 5 && S.cp_ib_fault == fault0);
                    CHECK(S.ring_tlb_flushes == tlb0 + 1 && S.cp_pfp_sync == sync0 /* no PFP on MEC */);
                    CHECK(S.cp_acquire == acq0 + 1 && S.cp_bad == 0 && S.cp_wait_stuck == 0);
                    CHECK(gfx11_submit_compute_job(&dev, &kcq, 1, 16, 0, jobs, 1) == 0);
                    CHECK(gfx11_submit_compute_job(&dev, &kcq, 1, 5, 0, jobs, 0) == 0);
                    for (uint32_t k = 0; k < 200; k++) {   /* wraps the 4 KiB queue */
                        seq = gfx11_submit_compute_job(&dev, &kcq, k, 5, 0, jobs, 1);
                        CHECK(seq != 0);
                    }
                    CHECK(gfx11_wait_fence(&dev, &kcq.rb, seq) == RDNA3_OK && S.cp_bad == 0 &&
                          gfx11_ring_space(&dev, &kcq.rb) == kcq.rb.buf_mask);

                    /* a hang: the CP stalls under a job (the model halts it),
                     * the fence never lands. Soft recovery kills the VMID's
                     * waves inside RLC safe mode, the fence is forced so the
                     * waiter returns, then the GFX soft reset brings ring 0
                     * and both compute queues back and jobs run again */
                    {
                        rdna3_gfx_status st;
                        uint32_t kills0 = S.sq_kills, resets0 = S.soft_resets;
                        jobs[0].va = ibva; jobs[0].ndw = n;
                        S.reg[GCR(regCP_ME_CNTL)] |= CP_ME_CNTL__ME_HALT_MASK;
                        seq = gfx11_submit_job(&dev, &gr, 7, 5, 0, jobs, 1);
                        CHECK(seq != 0 && !gfx11_fence_passed(&dev, &gr, seq) && gr.last_vmid == 5);
                        gfx11_read_status(&dev, &gr, &st);
                        CHECK(st.cp_me_cntl & CP_ME_CNTL__ME_HALT_MASK);
                        CHECK(st.rb0_rptr_wb != (uint32_t)gr.wptr);   /* the ring did not drain */
                        gfx11_log_status(&dev, &st, "hang test");
                        gfx11_kill_waves(&dev, 5);
                        CHECK(S.sq_kills == kills0 + 1 && S.sq_last_vmid == 5 && S.sq_outside_safe == 0 &&
                              !S.rlc_safe);
                        gfx11_force_fence_completion(&dev, &gr);
                        CHECK(gfx11_fence_passed(&dev, &gr, seq));
                        CHECK(gfx11_soft_reset(&dev, &gr, &kiq, &kcq) == RDNA3_OK);
                        CHECK(S.soft_resets == resets0 + 1 && gr.ready && kiq.rb.ready && kcq.rb.ready);
                        CHECK(!(S.reg[GCR(regCP_ME_CNTL)] & CP_ME_CNTL__ME_HALT_MASK) && !S.rlc_safe);
                        CHECK(S.reg[GCR(regCP_VMID_RESET)] == 0 && S.reg[GCR(regGRBM_GFX_CNTL)] == 0);
                        CHECK(HQD(hk, regCP_HQD_ACTIVE) == 1 && HQD(hc, regCP_HQD_ACTIVE) == 1 && hc->mapped_by_kiq);
                        CHECK(S.cp_bad == 0 && S.mec_bad_queue == 0);
                        S.reg[scratch] = 0;
                        seq = gfx11_submit_job(&dev, &gr, 8, 5, gpuvm_pd_addr(&dev, &vm), jobs, 1);
                        CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK && S.reg[scratch] == 0x5EED1234);
                        S.reg[scratch] = 0;
                        seq = gfx11_submit_compute_job(&dev, &kcq, 8, 5, gpuvm_pd_addr(&dev, &vm), jobs, 1);
                        CHECK(seq != 0 && gfx11_wait_fence(&dev, &kcq.rb, seq) == RDNA3_OK && S.reg[scratch] == 0x5EED1234);
                        CHECK(S.cp_bad == 0 && S.cp_wait_stuck == 0);
                    }
                }
                gpuvm_vm_fini(&dev, &vm);
            }
        }

        /* fence interrupts: IH in MSI mode, SDMA0 trap and CP EOP raise IVs the
         * ih60_iv_fence() decoder attributes to the right ring */
        {
            static rdna3_ih ih2;
            static rdna3_sdma_ring sd2;
            uint64_t a = rdna3_vram_alloc(&dev, 8192, 4096, "irq-test");
            uint32_t ivs0, msi0, seq, fences = 0;
            struct { uint32_t gfx, sdma, other; } got = { 0, 0, 0 };
            CHECK(ih60_init(&dev, &ih2, 4096, gmc.dummy_page_pa, 1) == RDNA3_OK && ih2.msi);
            CHECK(S.reg[OSS_BASE0 + regIH_RB_CNTL] & IH_RB_CNTL__RPTR_REARM_MASK);
            CHECK(S.reg[OSS_BASE0 + regIH_RB_CNTL] & IH_RB_CNTL__ENABLE_INTR_MASK);
            S.reg[0xA000 + regSDMA0_F32_CNTL] = SDMA0_F32_CNTL__HALT_MASK;
            CHECK(sdma60_start(&dev, &sd2, 0, 4096) == RDNA3_OK);
            ivs0 = S.ih_ivs; msi0 = S.ih_msi;
            /* without the trap source enabled a fence raises nothing */
            CHECK(sdma60_copy_sync(&dev, &sd2, rdna3_vram_mc(&dev, a), rdna3_vram_mc(&dev, a + 4096), 4096) == RDNA3_OK);
            CHECK(S.ih_ivs == ivs0 && S.ih_msi == msi0);
            sdma60_set_trap_interrupt(&dev, &sd2, 1);
            CHECK(S.reg[GC_BASE0 + regSDMA0_CNTL] & SDMA0_CNTL__TRAP_ENABLE_MASK);
            CHECK(sdma60_copy_sync(&dev, &sd2, rdna3_vram_mc(&dev, a), rdna3_vram_mc(&dev, a + 4096), 4096) == RDNA3_OK);
            CHECK(S.ih_ivs == ivs0 + 1 && S.ih_msi == msi0 + 1 && S.sdma_bad == 0);
            /* the engine honours SDMA0_CNTL even when the packet is there */
            S.reg[GC_BASE0 + regSDMA0_CNTL] &= ~SDMA0_CNTL__TRAP_ENABLE_MASK;
            seq = sdma60_emit_fence(&dev, &sd2);
            sdma60_commit(&dev, &sd2);
            CHECK(sdma60_wait_fence(&dev, &sd2, seq) == RDNA3_OK && S.ih_ivs == ivs0 + 1);
            S.reg[GC_BASE0 + regSDMA0_CNTL] |= SDMA0_CNTL__TRAP_ENABLE_MASK;
            /* GFX: RELEASE_MEM with INT_SEL 2 behind CP_INT_CNTL_RING0 */
            if (gr.ready) {
                seq = gfx11_emit_fence(&dev, &gr);
                gfx11_commit(&dev, &gr);
                CHECK(gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK && S.ih_ivs == ivs0 + 1);
                gfx11_set_eop_interrupt(&dev, &gr, 1);
                CHECK(S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK);
                seq = gfx11_emit_fence(&dev, &gr);
                gfx11_commit(&dev, &gr);
                CHECK(gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK && S.ih_ivs == ivs0 + 2 && S.cp_bad == 0);
            }
            /* drain: the two IVs map to their rings */
            {
                struct sim_seen seen = { 0, 0, 0, 0, 0, 0 };
                uint32_t n = ih60_process(&dev, &ih2, sim_iv_cb, &seen);
                CHECK(n == (gr.ready ? 2u : 1u) && seen.n == n);
            }
            /* compute: RELEASE_MEM on KCQ0 behind CP_ME1_PIPE0_INT_CNTL,
             * ring_id names me 1 pipe 0 queue 0 */
            if (kcq.rb.ready) {
                uint32_t dw[8]; rdna3_iv iv; uint32_t ivs1 = S.ih_ivs, off = (gr.ready ? 2u : 1u) * RD_IH_IV_SIZE;
                seq = gfx11_emit_fence(&dev, &kcq.rb);
                gfx11_commit(&dev, &kcq.rb);
                CHECK(gfx11_wait_fence(&dev, &kcq.rb, seq) == RDNA3_OK && S.ih_ivs == ivs1);
                gfx11_set_eop_interrupt(&dev, &kcq.rb, 1);
                CHECK(S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL)] & CP_ME1_PIPE0_INT_CNTL__TIME_STAMP_INT_ENABLE_MASK);
                seq = gfx11_emit_fence(&dev, &kcq.rb);
                gfx11_commit(&dev, &kcq.rb);
                CHECK(gfx11_wait_fence(&dev, &kcq.rb, seq) == RDNA3_OK && S.ih_ivs == ivs1 + 1);
                CHECK(ih60_process(&dev, &ih2, NULL, NULL) == 1);
                memcpy(dw, S.vram + ih2.ring + off, 32);
                ih60_decode_iv(dw, &iv);
                CHECK(iv.ring_id == 0x04 && ih60_iv_fence(&iv) == RD_IH_FENCE_COMPUTE0);
                gfx11_set_eop_interrupt(&dev, &kcq.rb, 0);
            }
            /* decode what was queued, in order, straight from the ring */
            for (uint32_t off = 0; off < 2 * RD_IH_IV_SIZE; off += RD_IH_IV_SIZE) {
                uint32_t dw[8]; rdna3_iv iv;
                memcpy(dw, S.vram + ih2.ring + off, 32);
                ih60_decode_iv(dw, &iv);
                switch (ih60_iv_fence(&iv)) {
                case RD_IH_FENCE_SDMA0: got.sdma++; break;
                case RD_IH_FENCE_GFX: got.gfx++; break;
                default: got.other++; break;
                }
                fences++;
            }
            CHECK(got.sdma == 1 && got.gfx == (gr.ready ? 1u : 0u) && got.other == (gr.ready ? 0u : 1u));
            /* other clients / rings are nobody's fence */
            {
                rdna3_iv iv = { RD_IH_CLIENT_GRBM_CP, RD_IH_SRC_CP_EOP, 0x10 /* me 1 */, 0, 0, 0, 0, 0, 0, { 0, 0, 0, 0 } };
                CHECK(ih60_iv_fence(&iv) == 0);
                iv.ring_id = 0; iv.src_id = 1;
                CHECK(ih60_iv_fence(&iv) == 0);
                iv.client_id = RD_IH_CLIENT_GFX; iv.src_id = RD_IH_SRC_SDMA_TRAP; iv.ring_id = 0x10; /* SDMA1 */
                CHECK(ih60_iv_fence(&iv) == 0);
            }
            /* MSI off: IVs still queue but nothing is raised; ring full = overflow */
            S.reg[OSS_BASE0 + regIH_RB_CNTL] &= ~IH_RB_CNTL__ENABLE_INTR_MASK;
            msi0 = S.ih_msi; ivs0 = S.ih_ivs;
            for (uint32_t k = 0; k < 4096 / 32 + 2; k++)
                ih_push(RD_IH_CLIENT_GFX, RD_IH_SRC_SDMA_TRAP, 0);
            CHECK(S.ih_msi == msi0 && S.ih_ivs == ivs0 + 4096 / 32 + 2);
            CHECK(S.reg[OSS_BASE0 + regIH_RB_WPTR] & IH_RB_WPTR__RB_OVERFLOW_MASK);
            CHECK(ih60_process(&dev, &ih2, NULL, NULL) > 0 && ih2.overflows == 1);
            sdma60_stop(&dev, &sd2);
            gfx11_set_eop_interrupt(&dev, &gr, 0);
            /* UNMAP_QUEUES through the KIQ, then the MEC halts */
            gfx11_mec_stop(&dev, &kiq, &kcq);
            CHECK(S.kiq_unmaps == 1 && !kcq.mapped && !kcq.rb.ready && !kiq.rb.ready);
            CHECK(HQD(&S.hqd[RD_KCQ0_PIPE][0], regCP_HQD_ACTIVE) == 0);
            CHECK(S.reg[GCR(regCP_MEC_RS64_CNTL)] & CP_MEC_RS64_CNTL__MEC_HALT_MASK);
            CHECK(S.mec_bad_queue == 0 && S.cp_bad == 0);
            ih60_disable(&dev, &ih2);
            (void)fences;
        }

    }

    /* DCN 3.2: the GOP left 2560x1440 linear ARGB8888 on pipe 1 -> OTG 2,
     * pipe 0 is blanked; take it over, flip, change pitch under lock */
    {
        static rdna3_dcn dc;
        const uint64_t gop = FB_START + 0x200000;
        uint32_t r = 0, g = 0, b = 0;
        memset(&dc, 0, sizeof(dc));   /* run() goes twice (synthetic + real firmware) */
        S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 0)] = HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
        S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] = 2u << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT;
        S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 2)] = 0;   /* not blanked, but its OTG 0 is off */
        S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 3)] = HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
        S.reg[OTG(regOTG0_OTG_CONTROL, 2)] = 1;
        S.reg[HUBP(regHUBP0_DCSURF_SURFACE_CONFIG, 1)] = RD_DCN_FMT_ARGB8888;
        S.reg[HUBP(regHUBP0_DCSURF_TILING_CONFIG, 1)] = 0;
        S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, 1)] = 2560u | (1440u << 16);
        S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, 1)] = 2560 - 1;
        S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 1)] = (uint32_t)gop;
        S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 1)] = (uint32_t)(gop >> 32);
        /* CVT-RB 2560x1440@60: htotal 2720, hfront 48, hsync 32; vtotal 1481, vfront 3, vsync 5 */
        S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] = 2720 - 1;
        S.reg[OTG(regOTG0_OTG_H_BLANK_START_END, 2)] = (2720 - 48) | ((2720 - 48 - 2560) << 16);
        S.reg[OTG(regOTG0_OTG_H_SYNC_A, 2)] = 32u << 16;
        S.reg[OTG(regOTG0_OTG_V_TOTAL, 2)] = 1481 - 1;
        S.reg[OTG(regOTG0_OTG_V_BLANK_START_END, 2)] = (1481 - 3) | ((1481 - 3 - 1440) << 16);
        S.reg[OTG(regOTG0_OTG_V_SYNC_A, 2)] = 5u << 16;
        S.reg[OTG(regOTG0_OTG_V_SYNC_A_CNTL, 2)] = 1;   /* negative */
        S.reg[OTG(regOTG0_OTG_STATUS_FRAME_COUNT, 2)] = 1234;
        S.reg[OTG(regOTG0_OTG_STATUS_POSITION, 2)] = 777;

        CHECK(dcn32_read_state(&dev, &dc) == 1);
        CHECK(dc.head[0].pipe == 1 && dc.head[0].otg == 2);
        CHECK(dc.head[0].width == 2560 && dc.head[0].height == 1440 && dc.head[0].pitch_px == 2560);
        CHECK(dc.head[0].address == gop && dcn32_head_usable(&dev, &dc.head[0]));
        CHECK(dc.head[0].h_total == 2720 && dc.head[0].h_active == 2560 && dc.head[0].h_front == 48 &&
              dc.head[0].h_sync == 32);
        CHECK(dc.head[0].v_total == 1481 && dc.head[0].v_active == 1440 && dc.head[0].v_front == 3 &&
              dc.head[0].v_sync == 5 && dc.head[0].v_sync_neg && !dc.head[0].h_sync_neg);
        CHECK(dcn32_frame_count(&dev, &dc, 0) == 1234 && dcn32_scanline(&dev, &dc, 0) == 777);

        /* same pitch: plain flip, latched */
        CHECK(dcn32_flip(&dev, &dc, 0, FB_START + 0x4000000, 2560, 0) == RDNA3_OK);
        CHECK(S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 1)] == (uint32_t)(FB_START + 0x4000000));
        CHECK(!dcn32_flip_pending(&dev, &dc, 0) && S.dcn_flips == 1);
        /* held: pending until the model latches */
        S.dcn_hold = 1;
        CHECK(dcn32_flip(&dev, &dc, 0, FB_START + 0x5000000, 2560, 0) == RDNA3_OK);
        CHECK(dcn32_flip_pending(&dev, &dc, 0));
        S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, 1)] &= ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
        CHECK(dcn32_flip_pending(&dev, &dc, 0));   /* EARLIEST_INUSE still the old one */
        S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, 1)] = (uint32_t)(FB_START + 0x5000000);
        CHECK(!dcn32_flip_pending(&dev, &dc, 0));
        S.dcn_hold = 0;
        /* pitch change goes under SURFACE_UPDATE_LOCK, and the lock is released */
        CHECK(dcn32_flip(&dev, &dc, 0, FB_START + 0x6000000, 2624, 1) == RDNA3_OK);
        CHECK(S.dcn_pitch_unlocked == 0 && S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, 1)] == 2623);
        CHECK(!(S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, 1)] & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK));
        CHECK(S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, 1)] & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK);
        /* bad requests: unaligned, outside VRAM, too narrow, no such head */
        CHECK(dcn32_flip(&dev, &dc, 0, FB_START + 0x6000010, 2624, 0) == RDNA3_EINVAL);
        CHECK(dcn32_flip(&dev, &dc, 0, FB_START + VRAM_SIZE - 4096, 2624, 0) == RDNA3_EINVAL);
        CHECK(dcn32_flip(&dev, &dc, 0, 0x1000, 2624, 0) == RDNA3_EINVAL);
        CHECK(dcn32_flip(&dev, &dc, 0, FB_START, 2000, 0) == RDNA3_EINVAL);
        CHECK(dcn32_flip(&dev, &dc, 1, FB_START, 2560, 0) == RDNA3_EINVAL);
        CHECK(S.dcn_flips == 3);
        /* hardware cursor: HUBP pipe 1 + DPP 1 */
        {
            const uint32_t dpp = regCNVC_CUR1_CURSOR0_CONTROL - regCNVC_CUR0_CURSOR0_CONTROL;
            uint32_t cc, dc_ctl;
            CHECK(dcn32_cursor_set_position(&dev, &dc, 0, 10, 10, 1) == RDNA3_EINVAL);   /* no image */
            CHECK(dcn32_cursor_set_image(&dev, &dc, 0, FB_START + 0x7000000, 100) == RDNA3_EINVAL);
            CHECK(dcn32_cursor_set_image(&dev, &dc, 0, FB_START + 0x7000080, 64) == RDNA3_EINVAL);
            CHECK(dcn32_cursor_set_image(&dev, &dc, 0, FB_START + 0x7000000, 256) == RDNA3_OK);
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_SURFACE_ADDRESS, 1)] == (uint32_t)(FB_START + 0x7000000));
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH, 1)] == (uint32_t)((FB_START + 0x7000000) >> 32));
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_SIZE, 1)] == (256u | (256u << 16)));
            cc = S.reg[HUBP(regCURSOR0_0_CURSOR_CONTROL, 1)];
            CHECK(((cc & CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE_MASK) >> CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE__SHIFT) == 2);
            CHECK(((cc & CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH_MASK) >> CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH__SHIFT) == 2);
            CHECK(!(cc & CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK));
            CHECK(dcn32_cursor_set_position(&dev, &dc, 0, 100, 200, 1) == RDNA3_OK);
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_POSITION, 1)] == ((100u << 16) | 200u));
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_HOT_SPOT, 1)] == 0);
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_CONTROL, 1)] & CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK);
            dc_ctl = S.reg[DCN_B2 + regCNVC_CUR0_CURSOR0_CONTROL + dpp * 1];
            CHECK((dc_ctl & CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK) &&
                  ((dc_ctl & CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE_MASK) >> CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE__SHIFT) == 2);
            CHECK(S.reg[DCN_B2 + regCNVC_CUR0_CURSOR0_CONTROL] == 0);   /* DPP 0 untouched */
            /* off the top-left edge: position 0, overhang in the hot spot */
            CHECK(dcn32_cursor_set_position(&dev, &dc, 0, -5, -7, 1) == RDNA3_OK);
            CHECK(S.reg[HUBP(regCURSOR0_0_CURSOR_POSITION, 1)] == 0 &&
                  S.reg[HUBP(regCURSOR0_0_CURSOR_HOT_SPOT, 1)] == ((5u << 16) | 7u));
            /* fully off screen or hidden: disabled on both sides */
            CHECK(dcn32_cursor_set_position(&dev, &dc, 0, -300, 10, 1) == RDNA3_OK);
            CHECK(!(S.reg[HUBP(regCURSOR0_0_CURSOR_CONTROL, 1)] & CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK));
            CHECK(!(S.reg[DCN_B2 + regCNVC_CUR0_CURSOR0_CONTROL + dpp] & CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK));
            CHECK(dcn32_cursor_set_position(&dev, &dc, 0, 2600, 10, 1) == RDNA3_OK);
            CHECK(!(S.reg[HUBP(regCURSOR0_0_CURSOR_CONTROL, 1)] & CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK));
            CHECK(dcn32_cursor_set_position(&dev, &dc, 0, 10, 10, 0) == RDNA3_OK);
            CHECK(!(S.reg[HUBP(regCURSOR0_0_CURSOR_CONTROL, 1)] & CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK));
        }
        /* CRC */
        CHECK(dcn32_crc_read(&dev, &dc, 0, &r, &g, &b) == RDNA3_ENODEV);
        dcn32_crc_enable(&dev, &dc, 0, 1);
        S.reg[OTG(regOTG0_OTG_CRC0_DATA_RG, 2)] = 0x22221111;
        S.reg[OTG(regOTG0_OTG_CRC0_DATA_B, 2)] = 0x3333;
        CHECK(dcn32_crc_read(&dev, &dc, 0, &r, &g, &b) == RDNA3_OK && r == 0x1111 && g == 0x2222 && b == 0x3333);
        /* tiled or DCC scanout cannot be drawn by the CPU */
        dc.head[0].sw_mode = 27;
        CHECK(!dcn32_head_usable(&dev, &dc.head[0]));
        /* display off: no heads */
        S.reg[OTG(regOTG0_OTG_CONTROL, 2)] = 0;
        CHECK(dcn32_read_state(&dev, &dc) == 0);
    }

    /* DisplayPort AUX: HPD sense, DPCD, I2C-over-AUX EDID, the probe of all
     * five engines, and the EDID parser on what came back */
    {
        static rdna3_connector conn[RD_AUX_ENGINES];
        static uint8_t edid[RD_EDID_MAX];
        rdna3_edid e;
        uint8_t dpcd[16], pwr = 2;
        uint32_t reply = 0, k, got1440 = 0, got1080 = 0, got480 = 0;
        int r;
        CHECK(dcaux_hpd_sense(&dev, 0) == 1 && dcaux_hpd_sense(&dev, 1) == 0);
        CHECK(S.reg[HPDR(regHPD0_DC_HPD_CONTROL, 1)] & HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK);
        CHECK(dcaux_dpcd_read(&dev, 0, 0x000, dpcd, 16) == 16);
        CHECK(dpcd[0] == 0x14 && dpcd[1] == 0x1e && dpcd[2] == 0x84 && S.aux_bad == 0);
        CHECK(dcaux_dpcd_write(&dev, 0, 0x600, &pwr, 1) == 1 && S.dpcd[0x600] == 2);
        CHECK(dcaux_transfer(&dev, 1, RD_AUX_DP_READ, 0, dpcd, 1, &reply) == RD_AUX_ERR_TIMEOUT);
        CHECK(dcaux_transfer(&dev, 0, RD_AUX_I2C_READ, 0x3a, dpcd, 1, &reply) == RD_AUX_ERR_NACK &&
              reply == RD_AUX_REPLY_I2C_NACK);
        CHECK(dcaux_transfer(&dev, 7, RD_AUX_DP_READ, 0, dpcd, 1, &reply) == RDNA3_EINVAL);
        /* the engine is given back after every transaction */
        CHECK(((S.reg[AUXR(regDP_AUX0_AUX_ARB_CONTROL, 0)] & DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK)
               >> DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS__SHIFT) == 0);
        r = dcaux_read_edid(&dev, 0, edid, sizeof(edid));
        CHECK(r == 256 && memcmp(edid, S.edid, 256) == 0);
        CHECK(dcaux_read_edid(&dev, 1, edid, sizeof(edid)) == RD_AUX_ERR_TIMEOUT);
        CHECK(edid_parse(S.edid, 256, &e) == 0);
        CHECK(strcmp(e.manufacturer, "RDN") == 0 && e.product == 0x7800 && e.digital &&
              e.version == 1 && e.revision == 4 && e.extensions == 1);
        CHECK(strcmp(e.name, "RDNA3 SIM") == 0 && e.width_cm == 60);
        for (k = 0; k < e.nmodes; k++) {
            const rdna3_mode *m = &e.mode[k];
            if (m->h_active == 2560 && m->v_active == 1440) {
                got1440++;
                CHECK(m->preferred && m->pixel_khz == 241500 && m->h_total == 2720 && m->v_total == 1481 &&
                      m->h_front == 48 && m->h_sync == 32 && m->v_front == 3 && m->v_sync == 5 &&
                      m->h_sync_pos && m->v_sync_pos && m->refresh_mhz / 1000 == 59);
            }
            if (m->h_active == 1920 && m->v_active == 1080) {
                got1080++;   /* once: the standard timing merged into the CTA detailed one */
                CHECK(m->pixel_khz == 148500 && m->h_total == 2200 && m->v_total == 1125);
            }
            if (m->h_active == 640 && m->v_active == 480)
                got480++;
        }
        CHECK(got1440 == 1 && got1080 == 1 && got480 == 1 && e.nmodes >= 4);
        CHECK(edid_parse(S.edid, 100, &e) == -1);
        S.edid[5] = 0;
        CHECK(edid_parse(S.edid, 128, &e) == -1);
        S.edid[5] = 0xff;
        /* the probe finds the one sink */
        CHECK(dcaux_probe(&dev, conn, RD_AUX_ENGINES) == 1);
        CHECK(conn[0].aux == 0 && conn[0].hpd && conn[0].dp && conn[0].dpcd_rev == 0x14 &&
              conn[0].max_link_rate == 0x1e && conn[0].max_lane_count == 4 && conn[0].edid_len == 256);
        CHECK(S.aux_bad == 0);
    }

    /* GPU reset: mode-1 through the PMFW debug mailbox takes the chip back
     * to the PSP bootloader with VRAM intact; PSP, the firmware and the SMU
     * come up again from there (what the kext redoes from level 4) */
    {
        static rdna3_psp p3;
        uint32_t ipfw0 = S.nipfw;
        CHECK(smu13_mode1_reset(&dev) == RDNA3_OK);
        CHECK(S.mode1_resets == 1 && !S.sos_alive && !S.smu_loaded);
        CHECK(psp13_wait_for_bootloader(&psp) == RDNA3_OK);
        CHECK(rdna3_rreg(&dev, mmRCC_CONFIG_MEMSIZE) == (uint32_t)(VRAM_SIZE >> 20));
        rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);
        CHECK(psp13_init(&p3, &dev, sos, sos_len) == RDNA3_OK);
        CHECK(psp13_hw_init(&p3, &plan, blobs) == RDNA3_OK);
        CHECK(S.sos_alive && S.nipfw == ipfw0 + plan.count && S.smu_loaded);
        CHECK(smu13_check_fw_status(&dev) == RDNA3_OK);
        CHECK(smu13_hello(&dev, &smu_ver, &ifv) == RDNA3_OK);
    }

    printf("sim_navi32: %s firmware, %u bootloader cmds, %u LOAD_IP_FW, TMR %u KiB, "
           "VRAM used %llu KiB\n", fwdir ? "real" : "synthetic", S.nbl, S.nipfw,
           psp.tmr_size / 1024, (unsigned long long)((dev.alloc_next - dev.alloc_lo) / 1024));

    /* failure path: bootloader never ready -> clean timeout */
    {
        static rdna3_psp p2;
        memset(&S.reg, 0, sizeof(S.reg));
        rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);
        CHECK(psp13_init(&p2, &dev, sos, sos_len) == RDNA3_OK);
        CHECK(psp13_bootloader_load_all(&p2) == RDNA3_ETIME);
    }

    if (fwdir) {
        for (i = 0; i < AMD_BLOB_COUNT; i++)
            free((void *)blobs[i].data);
        free((void *)sos);
    }
    free(S.vram);
}

int main(int argc, char **argv)
{
    run(NULL);
    if (argc > 1 && argv[1][0])
        run(argv[1]);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("sim_navi32: all checks passed\n");
    return 0;
}
