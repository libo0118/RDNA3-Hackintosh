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
#include "dcn32_modeset.h"
#include "atom.h"
#include "dmub32.h"
#include "dcn32_mode.h"
#include "gfx11_info.h"
#include "xfer.h"
#include "gen/gc_11_0_0_harvest.h"
#include "gen/gc_11_0_0_gds.h"
#include "gen/gc_11_0_0_sdma_reset.h"
#include "gen/dcn_3_2_1_dmcub.h"
#include "gen/dcn_3_2_1_aux.h"
#include "gmc11.h"
#include "gen/gc_11_0_3_gmc.h"
#include "gen/mmhub_3_0_0.h"
#include "ih60.h"
#include "gen/osssys_6_0_0.h"
#include "sdma60.h"
#include "gpuvm.h"
#include "gen/gc_11_0_0_sdma.h"
#include "gfx11.h"
#include "dcn32.h"
#include "soc21_cg.h"
#include "gen/athub_3_0_0_cg.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/hdp_6_0_0_cg.h"
#include "gen/dcn_3_2_0_scale.h"
#include "gen/dcn_3_2_0_crc.h"
#include "doorbell.h"
#include "gen/nbio_4_3_0_doorbell.h"
#include "gen/dcn_3_2_1.h"
#include "gen/gc_11_0_0_gfx.h"
#include "gen/gc_11_0_0_reset.h"
#include "gen/gc_11_0_0_cg.h"
#include "gen/gc_11_0_0_rs64.h"
#include "gen/gc_11_0_0_mes.h"
#include "gen/dcn_3_2_0_gpio.h"
#include "gen/dcn_3_2_0_i2c.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "dcn32_clone.h"
#include "gen/gc_11_0_3_gmc.h"
#include "gen/mmhub_3_0_0.h"
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
#define DCN_B3     0x9000u   /* DCN segment 3: MPC */
#define DCN_B1     0xC0u     /* DCN segment 1: DCCG */
#define HUBP(reg, n) (DCN_B2 + reg + (regHUBP1_DCHUBP_CNTL - regHUBP0_DCHUBP_CNTL) * (n))
#define OTG(reg, n)  (DCN_B2 + reg + (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL) * (n))

typedef struct sim {
    uint32_t reg[MMIO_LEN / 4];
    uint8_t *vram;
    uint8_t  vram_top[2u << 20];   /* the last 2 MiB of VRAM, beyond the BAR window */
    int verbose;

    /* bootloader log */
    uint32_t bl_cmds[16];
    unsigned nbl;
    int sos_alive;
    uint64_t tmr_mc;              /* where SETUP_TMR put the TMR */

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
    } hqd[4][4], mes_hqd[2];   /* MEC1 4x4; MES pipes 0 (scheduler) and 1 (KIQ) */
    /* the card: the MEC KIQ runs WRITE_DATA but stalls on SET_RESOURCES /
     * MAP_QUEUES / UNMAP_QUEUES; GC 11 schedules through the MES KIQ */
    unsigned mec_kiq_sched_ignored;
    unsigned kiq_maps, kiq_unmaps, mec_runs, mec_bad_queue;
    /* MES scheduler pipe: mapped by the KIQ (me 2, ENGINE_SEL 5), runs
     * 64-dword MESAPI frames and writes their API status fences */
    struct {
        unsigned maps, frames, hw_rsrc, queries, bad, before_rsrc;
        uint32_t hw[64];
        int hang;
    } mes_sched;
    /* gfx queue mapped by the KIQ from its v11_gfx_mqd (ENGINE_SEL 4); the
     * RS64 gfx firmware fetches through CP_GFX_HQD_* then. rs64_needs_mqd
     * models the card: a register-programmed RB0 without an MQD makes the
     * CPG read MC 0 and the CP halts */
    int gfx_mapped, rs64_needs_mqd;
    unsigned kgq_maps, kgq_unmaps, cp_mqd_faults;
    uint64_t set_resources_mask;
    uint32_t cp_cur_me, cp_cur_pipe, cp_cur_queue;   /* engine running cp_exec */
    int dcn_hold;              /* 1: flips stay pending (no vupdate yet) */
    /* DP AUX engine 0 with a sink behind it (DPCD, EDID at I2C 0x50); the
     * other engines answer with a receive timeout */
    uint8_t  aux_tx[20], aux_rx[20];
    uint32_t aux_ptr, aux_nrx, aux_status, aux_arb;
    unsigned aux_gos, aux_bad, aux_non_aux;
    /* HW I2C engine: a TMDS sink (HDMI, or a DP++ adaptor) on DDC line
     * i2c_sink+1 answers 0x50 (EDID) and 0x30 (segment) with S.edid */
    int i2c_sink;              /* engine index, -1 = none */
    uint8_t i2c_buf[538], ddc_seg, ddc_off;
    uint32_t i2c_wi, i2c_ri;
    unsigned i2c_gos, i2c_nacks;
    uint8_t  dpcd[0x800];
    uint8_t  edid[256];
    /* DMCUB: the firmware's view (boot checks, mailbox, GPINT) */
    struct {
        int running, psp_loaded, hang;
        unsigned boots, stops, bad_boots, cmds, caps_queries, bad_cmds, tx_cmds;
        uint8_t tx[16];
        /* VBIOS commands in order: sub type, and the payloads of the last
         * SetPixelClock / DIGxEncoderControl */
        uint8_t seq[16], seq_tx_action[16];
        unsigned nseq;
        uint8_t pixclk[16], enc[12];
    } dmub;
    /* DCN pipe power domains (DOMAINn_PG_*) once dcn_pg is on: writes to
     * the HUBP/DPP of a gated pipe are lost, as on the chip */
    int dcn_pg;
    unsigned dcn_gated_drops, dcn_pg_no_request;
    /* DAL mailbox of the PMFW (dcn32_clk_mgr_smu_msg.c) */
    unsigned dal_msgs, dal_displays, dal_hard_min[16];
    /* the PMFW stops answering once the DMCUB ran this many VBIOS commands
     * (0 = never), as it did on the card after the clone */
    unsigned smu_wedge_at;
    unsigned hpd_irqs;
    /* lit OTGs count frames while time passes; with OTG_CRC_EN their CRC
     * is a function of what they scan out (surface address + timing) */
    int dcn_frames;
    uint32_t i2c_off, i2c_seg;
    /* recovery: SQ_CMD kills, RLC safe mode, GRBM soft resets, mode-1 resets,
     * page-fault IVs */
    unsigned sq_kills, sq_outside_safe, soft_resets, mode1_resets, fault_ivs;
    unsigned cg_writes, cg_outside_safe;   /* RLC clock-gating registers */
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
        int scpm;             /* the PSP handed the PPTable over; mask/table writes refused */
        uint8_t pp_frev;      /* combo table format; 0 = the header's 15 */
        unsigned scpm_violations;
        uint32_t hard_min[PPCLK_COUNT];
        unsigned unflushed, bad_addr, pcie_overrides, soft_limits, xfers;
        uint32_t last_pcie_arg, workload, ppt_limit;
        uint32_t gfxoff, gfxoff_msgs;   /* AllowGfxOff / DisallowGfxOff */
        PPTable_t pptable;    /* as written by the driver */
    } smu;
    unsigned sdma_pkts, sdma_bad, sdma_while_halted;
    unsigned sdma_ibs, sdma_ib_fault;   /* INDIRECT packets run, ones the GFXHUB could not translate */
    int      sdma_hang;              /* the engine stopped consuming its ring */
    uint32_t harvest_wgp[8][2];
    uint64_t tsc;                    /* GOLDEN_TSC: +4 on every access of either half */
    uint8_t  sysmem[1u << 20];       /* system RAM at SYS_PHYS, reached through GART */
    unsigned gart_unmapped;      /* INACTIVE_WGPS per SE/SA (CC_GC_SHADER_ARRAY_CONFIG) */
    unsigned sdma_resets, sdma_resets_unfrozen;
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

/* VMID0 on MMHUB: an MC address in the GART range goes through the flat
 * PTE table; system pages land in S.sysmem (bus address SYS_PHYS..) */
#define SYS_PHYS 0x200000000ull
static uint8_t *gart_page(uint64_t mc)
{
    uint64_t start = ((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32] |
                      ((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32] << 32)) << 12;
    uint64_t end = (((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32] |
                     ((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32] << 32)) << 12) | 0xfff;
    uint64_t table = ((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32] |
                      ((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32] << 32)) & ~1ull;
    uint64_t pte, pa;
    if (mc < start || mc > end || table + ((mc - start) >> 12) * 8 + 8 > BAR_LEN)
        return NULL;
    memcpy(&pte, S.vram + table + ((mc - start) >> 12) * 8, 8);   /* FB_OFFSET 0: pa = VRAM offset */
    if (!(pte & 1)) {
        S.gart_unmapped++;
        return NULL;
    }
    pa = pte & 0x0000FFFFFFFFF000ull;
    if (pte & 2)
        return (pa >= SYS_PHYS && pa - SYS_PHYS < sizeof(S.sysmem)) ? S.sysmem + (pa - SYS_PHYS) + (mc & 0xfff)
                                                                     : NULL;
    return pa + 4096 <= BAR_LEN ? S.vram + pa + (mc & 0xfff) : NULL;
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

/* ---- display object table and GPIO pin LUT (bios_parser2 view) ----
 * DP on UNIPHY A fed by DDC1 / HPD1 (the sink model sits on AUX0), HDMI on
 * UNIPHY B fed by DDC2 / HPD2, plus a generic object DC skips. */
#define VB_OBJ      0x1000
#define VB_GPIOLUT  0x1100
#define VB_IIO      0x1180
#define VB_CMD_DIR  0x900
#define VB_CMD_CODE 0xA00
#define ATOM_REG_A  0xFF00u    /* plain storage registers the bytecode pokes (16-bit operands) */
#define ATOM_REG_B  0xFF04u
#define ATOM_REG_C  0xFF08u
#define SIM_BIOS_SCRATCH 0xFE00u /* firmware_info bios_scratch_reg_startaddr (dword index) */

static void make_vbios_display(void)
{
    uint8_t *b = S.vbios, *t = b + VB_OBJ, *g = b + VB_GPIOLUT;
    static const struct { uint32_t reg; uint8_t shift, id; } pins[] = {
        { 0x34C0u + 0x28d1u, 0, 0x90 },   /* DDC1 */
        { 0x34C0u + 0x28d5u, 0, 0x91 },   /* DDC2 */
        { 0x34C0u + 0x28f5u, 0x00, 0x10 },/* HPD1 */
        { 0x34C0u + 0x28f5u, 0x08, 0x11 },/* HPD2 */
    };
    unsigned i;
    /* display_object_info_table_v1_4: 8-byte header, 3 paths of 16 bytes */
    t[2] = 1; t[3] = 4;
    put16(t + 4, 0x0088);
    t[6] = 3;
    /* path 0: DP enum 1 -> UNIPHY A, records at +0x40, encoder records at +0x60 */
    put16(t + 8, 0x3113); put16(t + 10, 0x40); put16(t + 12, 0x211E);
    put16(t + 16, 0x60); put16(t + 20, 0x0008);
    /* path 1: HDMI enum 1 -> UNIPHY B, records at +0x70 */
    put16(t + 24, 0x310C); put16(t + 26, 0x70); put16(t + 28, 0x221E); put16(t + 36, 0x0080);
    /* path 2: a generic object (GLSync) without encoder */
    put16(t + 40, 0x6102); put16(t + 42, 0);
    put16(t, 8 + 3 * 16);
    /* records: I2C, HPD, connector speed; then the encoder caps */
    memcpy(t + 0x40, "\x01\x04\x90\x00" "\x02\x04\x10\x01" "\x04\x08\xa4\x1f\x00\x00\x00\x00" "\xff\x02", 18);
    memcpy(t + 0x60, "\x14\x06\x0b\x00\x00\x00" "\xff\x02", 8);
    memcpy(t + 0x70, "\x01\x04\x91\x00" "\x02\x04\x11\x01" "\xff\x02", 10);
    /* atom_gpio_pin_lut_v2_1 */
    put16(g, (uint16_t)(4 + 8 * 4)); g[2] = 2; g[3] = 1;
    for (i = 0; i < 4; i++) {
        put32(g + 4 + 8 * i, pins[i].reg);
        g[4 + 8 * i + 4] = pins[i].shift;
        g[4 + 8 * i + 6] = pins[i].id;
    }
    put16(b + VB_DATA_DIR + 4 + 2 * RD_ATOM_DT_DISPLAYOBJECTINFO, VB_OBJ);
    put16(b + VB_DATA_DIR + 4 + 2 * RD_ATOM_DT_GPIO_PIN_LUT, VB_GPIOLUT);
}

/* ---- AtomBIOS command tables: a tiny assembler for the bytecode ---- */
static uint32_t vb_pc, vb_start;
static void E8(uint32_t v)  { S.vbios[vb_pc++] = (uint8_t)v; }
static void E16(uint32_t v) { put16(S.vbios + vb_pc, (uint16_t)v); vb_pc += 2; }
static void E32(uint32_t v) { put32(S.vbios + vb_pc, v); vb_pc += 4; }
static uint32_t here(void)  { return vb_pc - vb_start; }   /* jump targets: table-relative */
static void table_begin(unsigned index, unsigned ws_dwords, unsigned ps_bytes)
{
    vb_pc = (vb_pc + 15) & ~15u;
    vb_start = vb_pc;
    put16(S.vbios + VB_CMD_DIR + 4 + 2 * index, (uint16_t)vb_pc);
    E16(0); E8(1); E8(1);      /* size, frev, crev */
    E8(ws_dwords); E8(ps_bytes);
}
static void table_end(void) { put16(S.vbios + vb_start, (uint16_t)(vb_pc - vb_start)); }

enum { OP_MOVE_REG = 1, OP_MOVE_PS = 2, OP_MOVE_WS = 3, OP_MOVE_FB = 4, OP_SHL_WS_IMM = 21,
       OP_MUL_PS = 32, OP_DIV_WS = 39, OP_ADD_WS = 45, OP_SETPORT_ATI = 55, OP_SETFBBASE = 59,
       OP_COMPARE_WS = 62, OP_SWITCH = 66, OP_JUMP = 67, OP_JUMP_BELOW = 69, OP_DELAY_MS = 80,
       OP_DELAY_US = 81, OP_CALLTABLE = 82, OP_EOT = 91, OP_MASK_WS = 94, OP_SETDATABLOCK = 102,
       OP_XOR_WS = 105 };
/* attr: src arg (0-2), src alignment (3-5), dst alignment selector (6-7) */
#define A_PS 0x01
#define A_WS 0x02
#define A_FB 0x03
#define A_ID 0x04
#define A_IMM 0x05
#define A_IMM_B0 0x25

static void make_vbios_cmd_tables(void)
{
    uint8_t *b = S.vbios;
    uint32_t loop, sw_case3, t2, fixup5, fixup3;

    put16(b + VB_ROM_HDR + 0x1E, VB_CMD_DIR);
    put16(b + VB_CMD_DIR, 4 + 2 * RD_ATOM_CMD_COUNT); b[VB_CMD_DIR + 2] = 2; b[VB_CMD_DIR + 3] = 1;
    vb_pc = VB_CMD_CODE;

    /* ASIC_Init v2.1: REG_A = PS[0] (boot sclk), then BIOS_SCRATCH_7 =
     * ASIC_INIT_COMPLETE as the real table reports a posted GPU */
    table_begin(RD_ATOM_CMD_ASIC_INIT, 0, 8);
    S.vbios[vb_start + 2] = 2;
    E8(OP_MOVE_REG); E8(A_PS); E16(ATOM_REG_A); E8(0);
    E8(OP_MOVE_REG); E8(A_IMM); E16(SIM_BIOS_SCRATCH + 7); E32(0x200);
    E8(OP_EOT);
    table_end();

    /* SetEngineClock (10): PS[0] *= 2 through the quotient register */
    table_begin(RD_ATOM_CMD_SET_ENGINE_CLOCK, 0, 4);
    E8(OP_MUL_PS); E8(A_IMM); E8(0); E32(2);
    E8(OP_MOVE_PS); E8(A_WS); E8(0); E8(0x40);
    E8(OP_EOT);
    table_end();

    /* SetPixelClock (12): the opcode tour */
    table_begin(RD_ATOM_CMD_SET_PIXEL_CLOCK, 3, 8);
    E8(OP_MOVE_WS); E8(A_PS); E8(0); E8(0);                   /* WS0 = PS0 */
    E8(OP_ADD_WS); E8(A_IMM); E8(0); E32(0x10);              /* WS0 += 16 */
    E8(OP_MOVE_REG); E8(A_WS); E16(ATOM_REG_A); E8(0);        /* REG_A = WS0 */
    loop = here();
    E8(OP_ADD_WS); E8(A_IMM_B0); E8(1); E8(1);                /* WS1.b0++ */
    E8(OP_COMPARE_WS); E8(A_IMM_B0); E8(1); E8(5);
    E8(OP_JUMP_BELOW); E16(loop);                            /* while < 5 */
    E8(OP_MOVE_WS); E8(A_IMM); E8(2); E32(0x00F0F0F0);
    E8(OP_MASK_WS); E8(A_IMM); E8(2); E32(0xFFFF0000); E32(0x1234);   /* 0x00F01234 */
    E8(OP_SHL_WS_IMM); E8(0x00); E8(2); E8(4);               /* 0x0F012340 */
    E8(OP_XOR_WS); E8(A_IMM); E8(2); E32(0x0F000000);         /* 0x00012340 */
    E8(OP_MOVE_REG); E8(A_WS); E16(ATOM_REG_B); E8(2);
    E8(OP_SWITCH); E8(A_WS); E8(1);
    E8(0x63); E32(3); fixup3 = vb_pc; E16(0);
    E8(0x63); E32(5); fixup5 = vb_pc; E16(0);
    E8(0x5A); E8(0x5A);
    sw_case3 = here();                                       /* default and case 3: wrong */
    E8(OP_MOVE_PS); E8(A_IMM); E8(2); E32(0xBAD);
    t2 = here();
    put16(b + fixup3, (uint16_t)sw_case3);
    put16(b + fixup5, (uint16_t)t2);
    E8(OP_CALLTABLE); E8(RD_ATOM_CMD_SET_ENGINE_CLOCK);       /* PS[2] (callee PS0) *= 2 */
    E8(OP_SETDATABLOCK); E8(RD_ATOM_DT_FIRMWAREINFO);
    E8(OP_MOVE_PS); E8(A_ID); E8(1); E16(8);                  /* PS1 = fwinfo boot sclk */
    E8(OP_SETFBBASE); E8(A_IMM); E32(0x10);
    E8(OP_MOVE_FB); E8(A_IMM); E8(1); E32(0xABCD);            /* scratch[5] */
    E8(OP_MOVE_PS); E8(A_FB); E8(3); E8(1);                   /* PS3 = FB[1] */
    E8(OP_SETPORT_ATI); E16(1);
    E8(OP_MOVE_REG); E8(A_PS); E16(0); E8(1);                 /* IIO write method 0x81 */
    E8(OP_SETPORT_ATI); E16(0);
    E8(OP_DELAY_US); E8(10);
    E8(OP_DIV_WS); E8(A_IMM); E8(0); E32(7);
    E8(OP_MOVE_PS); E8(A_WS); E8(0); E8(0x40);                /* PS0 = WS0 / 7 */
    E8(OP_EOT);
    table_end();

    /* EnableDispPowerGating (13): a loop that never ends */
    table_begin(RD_ATOM_CMD_ENABLE_DISP_POWER_GATING, 0, 0);
    E8(OP_DELAY_MS); E8(255);
    E8(OP_JUMP); E16(6);
    table_end();

    /* BlankCRTC (34): reads data far outside the image */
    table_begin(RD_ATOM_CMD_BLANK_CRTC, 1, 0);
    E8(OP_MOVE_WS); E8(A_ID); E8(0); E16(0xFFF0);
    E8(OP_EOT);
    table_end();

    /* EnableCRTC (35): calls itself */
    table_begin(RD_ATOM_CMD_ENABLE_CRTC, 0, 0);
    E8(OP_CALLTABLE); E8(RD_ATOM_CMD_ENABLE_CRTC);
    E8(OP_EOT);
    table_end();

    /* SelectCRTC_Source (42): an opcode past the table ends it quietly */
    table_begin(RD_ATOM_CMD_SELECT_CRTC_SOURCE, 0, 0);
    E8(0x7F);
    table_end();

    /* indirect IO: write method 0x81 = "REG_C = data" (data table 23) */
    put16(b + VB_DATA_DIR + 4 + 2 * 23, VB_IIO);
    vb_pc = VB_IIO + 4;
    E8(1); E8(0x81);                  /* START 0x81 */
    E8(8); E8(32); E8(0); E8(0);      /* MOVE_DATA 32 bits from 0 to 0 */
    E8(3); E16(ATOM_REG_C);           /* WRITE */
    E8(9); E8(0); E8(0);              /* END */
}

static void make_vbios(void)
{
    uint8_t *b = S.vbios;
    BoardTable_t bt;
    memset(b, 0, VBIOS_LEN);
    b[0] = 0x55; b[1] = 0xAA; b[2] = VBIOS_LEN / 512;
    memcpy(b + 0x30, " 761295520", 10);
    put16(b + 0x48, VB_ROM_HDR);
    memcpy(b + VB_MSG, "\r\nRX 7800 XT SIM\0", 17);
    /* identification strings (atom.c): 4 strings at 0x130, then CR LF and
     * the name; the date at 0x50; the version anchor further on */
    b[0x2f] = 4;
    put16(b + 0x6e, 0x130);
    memcpy(b + 0x130, "113-SIMPN-001\0NAVI32\0PCI_EXPRESS\0GDDR6\0\r\nSIM RX 7800 XT   \0", 57);
    memcpy(b + 0x50, "10/09/26 12:34", 14);
    memcpy(b + 0x1A0, "ATOMBIOSBK-AMD VER022.001.002.008.000001", 40);
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
    put32(b + VB_FWINFO + 24, SIM_BIOS_SCRATCH);
    put32(b + VB_FWINFO + 84, 16 * 1024);  /* fw_reserved_size_in_kb */
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
    make_vbios_display();
    make_vbios_cmd_tables();
}

/* the "combo" powerplay table the PMFW hands out: driver portion + PPTable_t
 * whose BoardTable is a placeholder the driver must replace from the VBIOS */
static void make_combo_pptable(uint8_t *out)
{
    PPTable_t *pp = (PPTable_t *)(out + RD_SMU_PP_SMC_PPTABLE_OFF);
    memset(out, 0, RD_SMU_COMBO_PPTABLE_SIZE);
    put16(out, RD_SMU_PP_SMC_PPTABLE_OFF + sizeof(PPTable_t));
    out[2] = S.smu.pp_frev ? S.smu.pp_frev : RD_SMU_PP_TABLE_FORMAT_REVISION; out[3] = 0;
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

/* MM_INDEX/MM_INDEX_HI/MM_DATA: VRAM through the indirect window
 * (amdgpu_device_mm_access); backed for the BAR window and the top 2 MiB */
static uint32_t *mm_word(void)
{
    uint64_t pos = ((uint64_t)*R(mmMM_INDEX_HI) << 31) | (*R(mmMM_INDEX) & 0x7fffffffu);
    if (pos + 4 <= BAR_LEN)
        return (uint32_t *)(S.vram + pos);
    if (pos >= VRAM_SIZE - sizeof(S.vram_top) && pos + 4 <= VRAM_SIZE)
        return (uint32_t *)(S.vram_top + (pos - (VRAM_SIZE - sizeof(S.vram_top))));
    return NULL;
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
    case PPSMC_MSG_GetDriverIfVersion: *R(MP1(82)) = SMU13_0_0_DRIVER_IF_VERSION; break;
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
        if (S.smu.scpm) { S.smu.scpm_violations++; resp = PPSMC_Result_CmdRejectedPrereq; break; }
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
    case PPSMC_MSG_SetAllowedFeaturesMaskLow:
    case PPSMC_MSG_SetAllowedFeaturesMaskHigh:
        if (S.smu.scpm) { S.smu.scpm_violations++; resp = PPSMC_Result_CmdRejectedPrereq; break; }
        if (v == PPSMC_MSG_SetAllowedFeaturesMaskLow) S.smu.allowed_lo = arg;
        else S.smu.allowed_hi = arg;
        break;
    case PPSMC_MSG_EnableAllSmuFeatures:
        if (!S.smu.pptable_written || !S.smu.btc) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        /* SCPM: the PMFW runs its own feature set */
        S.smu.enabled = S.smu.scpm ? (SIM_SMU_SUPPORTED & ~RD_SMU_FEATURE(FEATURE_GFXOFF_BIT))
                                   : (((uint64_t)S.smu.allowed_hi << 32) | S.smu.allowed_lo) &
                                         SIM_SMU_SUPPORTED;
        break;
    case PPSMC_MSG_SetHardMinByFreq:
        if (!sim_dpm_enabled(arg >> 16) || (arg >> 16) >= PPCLK_COUNT) {
            resp = PPSMC_Result_CmdRejectedPrereq;
            break;
        }
        S.smu.hard_min[arg >> 16] = arg & 0xffff;
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
    case PPSMC_MSG_AllowGfxOff:     S.smu.gfxoff = 1; S.smu.gfxoff_msgs++; break;
    case PPSMC_MSG_DisallowGfxOff:  S.smu.gfxoff = 0; S.smu.gfxoff_msgs++; break;
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
    e[128] = 0x02; e[129] = 0x03; e[130] = 0x07; e[131] = 0x00;
    e[132] = 0x42; e[133] = 0x84; e[134] = 97;   /* video data block: VIC 4 (native), VIC 97 */
    memcpy(e + 135, dtd_1080p, 18);
    e[255] = edid_sum(e + 128);
    S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 0)] = HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK;
}

/* HPD pin n changes: the interrupt fires on the change when the new
 * (delayed) sense equals DC_HPD_INT_POLARITY with DC_HPD_INT_EN set, and
 * stays latched until acked (edge: amdgpu acks before flipping the
 * polarity, which a level source would turn into a storm) */
static void ih_push_iv(uint32_t client, uint32_t src, uint32_t ring_id, uint32_t vmid,
                       uint32_t pasid, uint32_t sd0, uint32_t sd1);
static void hpd_eval(uint32_t n)
{
    uint32_t *st = &S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, n)];
    uint32_t ctl = S.reg[HPDR(regHPD0_DC_HPD_INT_CONTROL, n)];
    uint32_t sense = (*st & HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK) ? 1 : 0;
    uint32_t pol = (ctl & HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_POLARITY_MASK) ? 1 : 0;
    if (!(ctl & HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_EN_MASK) || sense != pol ||
        (*st & HPD0_DC_HPD_INT_STATUS__DC_HPD_INT_STATUS_MASK))
        return;
    *st |= HPD0_DC_HPD_INT_STATUS__DC_HPD_INT_STATUS_MASK;
    S.hpd_irqs++;
    ih_push_iv(RD_IH_CLIENT_DCE, RD_IH_SRC_DC_HPD1_INT, 0, 0, 0, n, 0);
}
static void hpd_plug(uint32_t n, int on)
{
    uint32_t m = HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK | HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK;
    uint32_t *st = &S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, n)];
    *st = on ? (*st | m) : (*st & ~m);
    hpd_eval(n);
}

/* ---- DMCUB model ---- */
#define DMR(reg) (DCN_B2 + (reg))
static uint64_t dmub_cw_offset(unsigned n)
{
    static const uint32_t lo[8] = { regDMCUB_REGION3_CW0_OFFSET, regDMCUB_REGION3_CW1_OFFSET,
        regDMCUB_REGION3_CW2_OFFSET, regDMCUB_REGION3_CW3_OFFSET, regDMCUB_REGION3_CW4_OFFSET,
        regDMCUB_REGION3_CW5_OFFSET, regDMCUB_REGION3_CW6_OFFSET, regDMCUB_REGION3_CW7_OFFSET };
    return S.reg[DMR(lo[n])] | ((uint64_t)S.reg[DMR(lo[n] + 1)] << 32);   /* OFFSET_HIGH follows */
}
static int dmub_cw_enabled(uint32_t top_reg) { return (S.reg[DMR(top_reg)] & 0x80000000u) != 0; }

/* the core leaves reset: what the real firmware needs before it reports
 * DAL_FW | MAILBOX_RDY in SCRATCH0 */
static void dmub_boot(void)
{
    const uint8_t *vb = mc_ptr(dmub_cw_offset(3), 64);
    int ok = (S.dmub.psp_loaded || dmub_cw_enabled(regDMCUB_REGION3_CW0_TOP_ADDRESS)) &&
             dmub_cw_enabled(regDMCUB_REGION3_CW3_TOP_ADDRESS) &&
             dmub_cw_enabled(regDMCUB_REGION3_CW4_TOP_ADDRESS) &&
             dmub_cw_enabled(regDMCUB_REGION3_CW5_TOP_ADDRESS) &&
             dmub_cw_enabled(regDMCUB_REGION3_CW6_TOP_ADDRESS) &&
             (S.reg[DMR(regDMCUB_REGION5_TOP_ADDRESS)] & 0x80000000u) &&
             S.reg[DMR(regDMCUB_INBOX1_BASE_ADDRESS)] == RD_DMUB_CW4_BASE &&
             S.reg[DMR(regDMCUB_INBOX1_SIZE)] == RD_DMUB_RB_SIZE &&
             vb && memcmp(vb, S.vbios, 64) == 0;   /* the VBIOS copy behind CW3 */
    if (!ok) {
        S.dmub.bad_boots++;
        return;
    }
    S.dmub.running = 1;
    S.dmub.boots++;
    S.reg[DMR(regDMCUB_SCRATCH0)] = RD_DMUB_BOOT_DAL_FW | RD_DMUB_BOOT_MAILBOX_RDY | RD_DMUB_BOOT_HW_PWR_INIT;
}

static void dmub_inbox(uint32_t wptr)
{
    uint32_t rptr = S.reg[DMR(regDMCUB_INBOX1_RPTR)];
    uint64_t base = dmub_cw_offset(4);
    if (!S.dmub.running || S.dmub.hang)
        return;
    while (rptr != wptr) {
        uint8_t *c = mc_ptr(base + rptr, RD_DMUB_RB_CMD_SIZE);
        uint32_t hdr;
        if (!c) {
            S.dmub.bad_cmds++;
            break;
        }
        memcpy(&hdr, c, 4);
        S.dmub.cmds++;
        switch (hdr & 0xff) {
        case RD_DMUB_CMD_QUERY_FEATURE_CAPS:
            S.dmub.caps_queries++;
            c[4] = 1;      /* psr version */
            c[5] = 2;      /* fw_assisted_mclk_switch_ver */
            hdr &= ~RD_DMUB_HDR_RET_STATUS;
            memcpy(c, &hdr, 4);
            break;
        case RD_DMUB_CMD_VBIOS: {
            uint32_t sub = (hdr >> 8) & 0xff, len = (hdr >> 24) & 0x3f;
            if (S.dmub.nseq < 16) {
                S.dmub.seq_tx_action[S.dmub.nseq] = c[5];
                S.dmub.seq[S.dmub.nseq++] = (uint8_t)sub;
            }
            if (sub == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && len == 60) {
                memcpy(S.dmub.tx, c + 4, 16);
                S.dmub.tx_cmds++;
                /* what the table does to the back end of the PHY */
                if (c[4] < 5 && (c[5] == RD_ATOM_TX_ENABLE || c[5] == RD_ATOM_TX_DISABLE)) {
                    uint32_t *en = &S.reg[DCN_B2 + regDIG0_DIG_BE_EN_CNTL + RD_CLONE_DIGFE_STRIDE * c[4]];
                    if (c[5] == RD_ATOM_TX_ENABLE)
                        *en |= DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
                    else
                        *en &= ~DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
                }
            } else if (sub == RD_DMUB_VBIOS_SET_PIXEL_CLOCK && len == 16) {
                memcpy(S.dmub.pixclk, c + 4, 16);
            } else if (sub == RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL && len == 12) {
                memcpy(S.dmub.enc, c + 4, 12);
            } else {
                S.dmub.bad_cmds++;
            }
            break;
        }
        default:
            S.dmub.bad_cmds++;
        }
        rptr = (rptr + RD_DMUB_RB_CMD_SIZE) % RD_DMUB_RB_SIZE;
    }
    S.reg[DMR(regDMCUB_INBOX1_RPTR)] = rptr;
}

/* DC_I2C_CONTROL.GO: run TRANSACTION0..count-1 from the data buffer */
static void i2c_go(uint32_t ctl)
{
    uint32_t count = ((ctl & DC_I2C_CONTROL__DC_I2C_TRANSACTION_COUNT_MASK) >>
                      DC_I2C_CONTROL__DC_I2C_TRANSACTION_COUNT__SHIFT) + 1;
    uint32_t line = (ctl & DC_I2C_CONTROL__DC_I2C_DDC_SELECT_MASK) >> DC_I2C_CONTROL__DC_I2C_DDC_SELECT__SHIFT;
    uint32_t pad = S.reg[DCN_B2 + regDC_GPIO_DDC1_MASK + (regDC_GPIO_DDC2_MASK - regDC_GPIO_DDC1_MASK) * line];
    uint32_t setup = S.reg[DCN_B2 + regDC_I2C_DDC1_SETUP + (regDC_I2C_DDC2_SETUP - regDC_I2C_DDC1_SETUP) * line];
    int sink = (int)line == S.i2c_sink && !(pad & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK) &&
               (setup & DC_I2C_DDC1_SETUP__DC_I2C_DDC1_ENABLE_MASK);
    uint32_t pos = 0, t, i, nack = 0;
    S.i2c_gos++;
    for (t = 0; t < count && !nack; t++) {
        uint32_t tr = S.reg[DCN_B2 + regDC_I2C_TRANSACTION0 + t];
        uint32_t cnt = (tr & DC_I2C_TRANSACTION0__DC_I2C_COUNT0_MASK) >> DC_I2C_TRANSACTION0__DC_I2C_COUNT0__SHIFT;
        int rd = (tr & DC_I2C_TRANSACTION0__DC_I2C_RW0_MASK) != 0;
        uint32_t dev = S.i2c_buf[pos++ % sizeof(S.i2c_buf)] >> 1;
        if (!sink || (dev != 0x50 && dev != 0x30)) { nack = 1; break; }
        for (i = 0; i < cnt; i++, pos++) {
            uint8_t *b = &S.i2c_buf[pos % sizeof(S.i2c_buf)];
            if (!rd) {
                if (dev == 0x30) S.ddc_seg = *b; else S.ddc_off = *b;
            } else {
                uint32_t o = S.ddc_seg * 256u + S.ddc_off++;
                *b = o < sizeof(S.edid) ? S.edid[o] : 0xff;
            }
        }
        if (tr & DC_I2C_TRANSACTION0__DC_I2C_STOP0_MASK)
            S.ddc_seg = 0;   /* the segment pointer resets at STOP */
    }
    S.i2c_nacks += nack;
    S.reg[DCN_B2 + regDC_I2C_SW_STATUS] = nack ? DC_I2C_SW_STATUS__DC_I2C_SW_STOPPED_ON_NACK_MASK
                                               : DC_I2C_SW_STATUS__DC_I2C_SW_DONE_MASK;
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
    if (!(S.reg[DCN_B2 + regDC_GPIO_DDC1_MASK + (regDC_GPIO_DDC2_MASK - regDC_GPIO_DDC1_MASK) * n] &
          DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK)) {
        /* the pad is in I2C mode (the card, 2026-10-10): nothing goes out */
        S.aux_non_aux++;
        S.aux_status = DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_NON_AUX_MODE_MASK;
        return;
    }
    if (((S.reg[AUXR(regDP_AUX0_AUX_CONTROL, n)] & DP_AUX0_AUX_CONTROL__AUX_HPD_SEL_MASK) >>
         DP_AUX0_AUX_CONTROL__AUX_HPD_SEL__SHIFT) != n) {
        /* mode detection on another connector's HPD pin (the card: HPD_SEL 0) */
        S.aux_non_aux++;
        S.aux_status = DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_NON_AUX_MODE_MASK;
        return;
    }
    if ((int)n == S.i2c_sink) {   /* a TMDS sink: the mode detection sees no AUX */
        S.aux_non_aux++;
        S.aux_status = DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_NON_AUX_MODE_MASK;
        return;
    }
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
    uint32_t fbb = *R(MM_BASE0 + regMMMC_VM_FB_LOCATION_BASE), fbt = *R(MM_BASE0 + regMMMC_VM_FB_LOCATION_TOP);
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
    *R(MM_BASE0 + regMMMC_VM_FB_LOCATION_BASE) = fbb;
    *R(MM_BASE0 + regMMMC_VM_FB_LOCATION_TOP) = fbt;
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
    if (me == 3 && pipe < 2 && q == 0)
        return &S.mes_hqd[pipe];
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
/* MAP_QUEUES ENGINE_SEL 4: struct v11_gfx_mqd -> the gfx HQD registers */
static const struct { uint32_t mqd, reg; } gfx_mqd_map[] = {
    { 128, regCP_GFX_MQD_BASE_ADDR }, { 129, regCP_GFX_MQD_BASE_ADDR_HI }, { 130, regCP_GFX_HQD_ACTIVE },
    { 131, regCP_GFX_HQD_VMID }, { 134, regCP_GFX_HQD_QUEUE_PRIORITY }, { 135, regCP_GFX_HQD_QUANTUM },
    { 136, regCP_GFX_HQD_BASE }, { 137, regCP_GFX_HQD_BASE_HI }, { 138, regCP_GFX_HQD_RPTR },
    { 139, regCP_GFX_HQD_RPTR_ADDR }, { 140, regCP_GFX_HQD_RPTR_ADDR_HI },
    { 141, regCP_RB_WPTR_POLL_ADDR_LO }, { 142, regCP_RB_WPTR_POLL_ADDR_HI },
    { 143, regCP_RB_DOORBELL_CONTROL }, { 144, regCP_GFX_HQD_OFFSET }, { 145, regCP_GFX_HQD_CNTL },
    { 148, regCP_GFX_HQD_CSMD_RPTR }, { 149, regCP_GFX_HQD_WPTR }, { 150, regCP_GFX_HQD_WPTR_HI },
    { 162, regCP_GFX_MQD_CONTROL },
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
            if (S.cp_cur_me == 1) { S.mec_kiq_sched_ignored++; return rptr; }
            if (cnt == 6 && S.cp_cur_me == 3) S.set_resources_mask = PK(2) | ((uint64_t)PK(3) << 32);
            else S.cp_bad++;
            break;
        case PACKET3_MAP_QUEUES: {
            uint32_t q = (PK(1) >> 13) & 7, pipe = (PK(1) >> 16) & 3, me = (PK(1) >> 18) & 3;
            uint32_t db = (PK(2) >> 2) & 0x3ffffff, eng = (PK(1) >> 26) & 7;
            uint64_t mqd = PK(3) | ((uint64_t)PK(4) << 32);
            if (S.cp_cur_me == 1) { S.mec_kiq_sched_ignored++; return rptr; }
            if (cnt == 5 && S.cp_cur_me == 3 && me == 0 && eng == 4) {
                const uint32_t *m = (const uint32_t *)mc_ptr(mqd, 4 * RD_MQD_DWORDS);
                uint32_t i2, dc;
                if (!m || q || pipe || (m[128] & ~0xfffffffcu) || m[128] != (uint32_t)mqd) { S.cp_bad++; break; }
                for (i2 = 0; i2 < sizeof(gfx_mqd_map) / sizeof(gfx_mqd_map[0]); i2++)
                    S.reg[GC_BASE0 + gfx_mqd_map[i2].reg] = m[gfx_mqd_map[i2].mqd];
                dc = S.reg[GCR(regCP_RB_DOORBELL_CONTROL)] & ~CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK;
                S.reg[GCR(regCP_RB_DOORBELL_CONTROL)] = dc | CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK |
                    ((db << CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT) & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK);
                S.reg[GCR(regCP_GFX_HQD_ACTIVE)] = 1;
                S.gfx_mapped = 1;
                S.cp_rptr = 0;
                S.kgq_maps++;
                break;
            }
            if (cnt == 5 && S.cp_cur_me == 3 && me == 2 && eng == 5) {
                if (q || pipe || !hqd_load_mqd(&S.mes_hqd[0], mqd, db)) { S.cp_bad++; break; }
                S.mes_sched.maps++;
                break;
            }
            if (cnt != 5 || S.cp_cur_me != 3 || me != 1 || q > 3) { S.cp_bad++; break; }
            if (!(S.set_resources_mask & (1ull << (pipe * 8 + q)))) { S.mec_bad_queue++; break; }
            if (!hqd_load_mqd(&S.hqd[pipe][q], mqd, db)) { S.cp_bad++; break; }
            S.kiq_maps++;
            break;
        }
        case PACKET3_UNMAP_QUEUES: {
            uint32_t db = (PK(2) >> 2) & 0x3ffffff;
            int found = 0;
            if (S.cp_cur_me == 1) { S.mec_kiq_sched_ignored++; return rptr; }
            if (cnt != 4 || S.cp_cur_me != 3) { S.cp_bad++; break; }
            if (((PK(1) >> 26) & 7) == 4) {
                uint32_t dc = S.reg[GCR(regCP_RB_DOORBELL_CONTROL)];
                if (S.gfx_mapped && ((dc & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == db) {
                    S.gfx_mapped = 0;
                    S.reg[GCR(regCP_GFX_HQD_ACTIVE)] = 0;
                    S.kgq_unmaps++;
                } else {
                    S.cp_bad++;
                }
                break;
            }
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
            uint32_t sel = (PK(2) >> 29) & 7;   /* DATA_SEL 1: 32 bits, 2: 64 bits */
            uint8_t *p = mc_ptr(a, sel == 2 ? 8 : 4);
            if (p && sel == 1) memcpy(p, &PK(5), 4);
            else if (p && sel == 2 && !(a & 7)) { memcpy(p, &PK(5), 4); memcpy(p + 4, &PK(6), 4); }
            else S.cp_bad++;
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

/* the MES scheduler firmware: MESAPI frames (mes_v11_api_def.h) */
static uint64_t mes_api_exec(const uint32_t *buf, uint64_t mask, uint64_t rptr, uint64_t end)
{
    while ((rptr & mask) != (end & mask)) {
        uint32_t f[64], st, op;
        uint64_t addr, val;
        uint8_t *p;
        for (uint32_t i = 0; i < 64; i++)
            f[i] = buf[(rptr + i) & mask];
        rptr += 64;
        S.mes_sched.frames++;
        if (S.mes_sched.hang)
            continue;
        op = (f[0] >> 4) & 0xff;
        if ((f[0] & 0xf) != 1 || ((f[0] >> 12) & 0xff) != 64) { S.mes_sched.bad++; continue; }
        if (op == RD_MES_SCH_API_SET_HW_RSRC) {
            memcpy(S.mes_sched.hw, f, sizeof(f));
            S.mes_sched.hw_rsrc++;
            st = 50;
        } else if (op == RD_MES_SCH_API_QUERY_SCHEDULER_STATUS) {
            if (!S.mes_sched.hw_rsrc)
                S.mes_sched.before_rsrc++;
            S.mes_sched.queries++;
            st = 2;
        } else {
            S.mes_sched.bad++;
            continue;
        }
        addr = f[st] | ((uint64_t)f[st + 1] << 32);
        val = f[st + 2] | ((uint64_t)f[st + 3] << 32);
        p = mc_ptr(addr, 8);
        if (p)
            memcpy(p, &val, 8);
        else
            S.mes_sched.bad++;
    }
    return rptr;
}

/* a mapped MEC queue runs its PQ up to the doorbell value (dwords) */
static void mec_run(uint32_t me, uint32_t pipe, uint32_t q, uint64_t wptr)
{
    struct hqd *h = me == 3 ? &S.mes_hqd[pipe] : &S.hqd[pipe][q];
    uint32_t ctl = HQD(h, regCP_HQD_PQ_CONTROL);
    uint32_t ndw = 2u << (ctl & CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK);
    uint64_t base = ((uint64_t)HQD(h, regCP_HQD_PQ_BASE) << 8) | ((uint64_t)HQD(h, regCP_HQD_PQ_BASE_HI) << 40);
    uint64_t rwb = HQD(h, regCP_HQD_PQ_RPTR_REPORT_ADDR) | ((uint64_t)HQD(h, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI) << 32);
    uint32_t *ring = (uint32_t *)mc_ptr(base, 4ull * ndw);
    uint32_t mec = S.reg[GCR(regCP_MEC_RS64_CNTL)], mes = S.reg[GCR(regCP_MES_CNTL)];
    uint8_t *wb;
    if (!(HQD(h, regCP_HQD_ACTIVE) & 1) || !ring) { S.mec_bad_queue++; return; }
    if (me == 1 && ((mec & CP_MEC_RS64_CNTL__MEC_HALT_MASK) ||
                    !(mec & (CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK << pipe)))) {
        S.cp_while_halted++;
        return;
    }
    if (me == 3 && ((mes & (CP_MES_CNTL__MES_HALT_MASK | (CP_MES_CNTL__MES_PIPE0_RESET_MASK << pipe))) ||
                    !(mes & (CP_MES_CNTL__MES_PIPE0_ACTIVE_MASK << pipe)))) {
        S.cp_while_halted++;
        return;
    }
    if (S.pending_writes)
        S.unflushed_doorbells++;
    S.cp_cur_me = me; S.cp_cur_pipe = pipe; S.cp_cur_queue = q;
    if (me == 3 && pipe == 0)
        h->rptr = mes_api_exec(ring, ndw - 1, h->rptr, wptr);
    else
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
                mec_run(1, p, q, v);
                return;
            }
        }
    for (uint32_t p = 0; p < 2; p++) {
        struct hqd *h = &S.mes_hqd[p];
        uint32_t c = HQD(h, regCP_HQD_PQ_DOORBELL_CONTROL);
        if ((c & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK) &&
            ((c & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == index &&
            (HQD(h, regCP_HQD_ACTIVE) & 1)) {
            mec_run(3, p, 0, v);
            return;
        }
    }
    S.db_unclaimed++;
}

static void cp_run(uint32_t wptr_dw)
{
    int mapped = S.gfx_mapped && S.reg[GCR(regCP_GFX_HQD_ACTIVE)];
    uint32_t cntl = mapped ? S.reg[GCR(regCP_GFX_HQD_CNTL)] : S.reg[GCR(regCP_RB0_CNTL)];
    uint32_t me = S.reg[GCR(regCP_ME_CNTL)];
    uint64_t base = mapped ? ((uint64_t)S.reg[GCR(regCP_GFX_HQD_BASE)] << 8) |
                             ((uint64_t)S.reg[GCR(regCP_GFX_HQD_BASE_HI)] << 40)
                           : ((uint64_t)S.reg[GCR(regCP_RB0_BASE)] << 8) |
                             ((uint64_t)S.reg[GCR(regCP_RB0_BASE_HI)] << 40);
    uint32_t ndw = 2u << ((cntl & CP_RB0_CNTL__RB_BUFSZ_MASK) >> CP_RB0_CNTL__RB_BUFSZ__SHIFT);
    uint32_t *ring = (uint32_t *)mc_ptr(base, 4ull * ndw);
    uint64_t rptr_wb = mapped ? ((uint64_t)S.reg[GCR(regCP_GFX_HQD_RPTR_ADDR_HI)] << 32) |
                                S.reg[GCR(regCP_GFX_HQD_RPTR_ADDR)]
                              : ((uint64_t)S.reg[GCR(regCP_RB0_RPTR_ADDR_HI)] << 32) |
                                S.reg[GCR(regCP_RB0_RPTR_ADDR)];
    uint8_t *wb;
    if ((!mapped && !S.reg[GCR(regCP_RB_ACTIVE)]) || !ring)
        return;
    if (me & (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK)) {
        S.cp_while_halted++;
        return;
    }
    if (!mapped && S.rs64_needs_mqd && !S.reg[GCR(regCP_GFX_MQD_BASE_ADDR)]) {
        /* the card: GFXHUB fault at MC 0 from the CPG, CE/PFP/ME halted */
        S.cp_mqd_faults++;
        S.reg[GCR(regCP_ME_CNTL)] |= CP_ME_CNTL__CE_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK |
                                     CP_ME_CNTL__ME_HALT_MASK;
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
/* SDMA 6.0 is a GC client: an IB with VMID n is fetched, and its addresses
 * resolved, through GFXHUB context n (sdma_v6_0: ring->vm_hub =
 * AMDGPU_GFXHUB(0)); a context the GFXHUB never enabled translates nothing */
static uint8_t *sdma_vm_ptr(uint32_t vmid, uint64_t va, uint64_t len)
{
    uint32_t dist = regGCVM_CONTEXT1_CNTL - regGCVM_CONTEXT0_CNTL;
    if (!(S.reg[GC_BASE0 + regGCVM_CONTEXT0_CNTL + dist * vmid] & GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK))
        return NULL;
    return vm_ptr(vmid, va, len);
}

static void sdma_run_ib(uint32_t vmid, uint64_t va, uint32_t ndw)
{
    const uint32_t *ib = (const uint32_t *)sdma_vm_ptr(vmid, va, 4ull * ndw);
    uint32_t i = 0;
    S.sdma_ibs++;
    if (!ib) {
        S.sdma_ib_fault++;
        return;
    }
    while (i < ndw) {
        uint32_t h = ib[i], op = h & 0xff, sub = (h >> 8) & 0xff, n;
        if (op == SDMA_OP_NOP) {
            n = 1 + ((h >> 16) & 0x3fff);
        } else if (op == SDMA_OP_WRITE && sub == 0) {
            uint64_t a = ib[i + 1] | ((uint64_t)ib[i + 2] << 32);
            uint32_t cnt = (ib[i + 3] & 0xfffff) + 1;
            uint8_t *p = sdma_vm_ptr(vmid, a, 4ull * cnt);
            if (p) memcpy(p, &ib[i + 4], 4ull * cnt); else S.sdma_ib_fault++;
            n = 4 + cnt;
        } else if (op == SDMA_OP_CONST_FILL) {
            uint64_t a = ib[i + 1] | ((uint64_t)ib[i + 2] << 32);
            uint32_t val = ib[i + 3], len = ib[i + 4] + 1, k;
            uint8_t *p = sdma_vm_ptr(vmid, a, len);
            for (k = 0; p && k + 4 <= len; k += 4) memcpy(p + k, &val, 4);
            if (!p) S.sdma_ib_fault++;
            n = 5;
        } else {
            S.sdma_bad++;
            return;
        }
        i += n;
    }
}

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
    if (S.sdma_hang)
        return;
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
            if (ps && pd) {
                memmove(pd, ps, len);
            } else if (ps) {
                /* destination through GART, one page at a time */
                uint32_t k = 0;
                while (k < len) {
                    uint32_t c = 4096 - (uint32_t)((d + k) & 0xfff);
                    uint8_t *q = gart_page(d + k);
                    if (c > len - k) c = len - k;
                    if (!q) { S.sdma_bad++; break; }
                    memcpy(q, ps + k, c);
                    k += c;
                }
            } else {
                S.sdma_bad++;
            }
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
        } else if (op == SDMA_OP_INDIRECT) {
            sdma_run_ib((h >> 16) & 0xf, (RD(1) & 0xffffffe0u) | ((uint64_t)RD(2) << 32), RD(3));
            n = 6;
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
        S.tmr_mc = cmd->cmd.cmd_setup_tmr.buf_phy_addr_lo |
                   ((uint64_t)cmd->cmd.cmd_setup_tmr.buf_phy_addr_hi << 32);
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
        if (t == GFX_FW_TYPE_DMUB) {
            /* the PSP puts the image behind CW0 and opens the window */
            S.dmub.psp_loaded = 1;
            *R(DCN_B2 + regDMCUB_REGION3_CW0_TOP_ADDRESS) |= DMCUB_REGION3_CW0_TOP_ADDRESS__DMCUB_REGION3_CW0_ENABLE_MASK;
        }
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
    if (dw == mmMM_DATA) {
        uint32_t *w = mm_word();
        if (w)
            *w = v;
        return;
    }
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
    /* gfx HQD dequeue (soft reset): drained at once, the KIQ mapping is gone */
    if (dw == GCR(regCP_GFX_HQD_DEQUEUE_REQUEST) && v) {
        *R(GCR(regCP_GFX_HQD_ACTIVE)) = 0;
        S.gfx_mapped = 0;
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
    /* HPD interrupt control: ACK clears the latched status (self-clearing
     * bit), then the level condition is checked again */
    for (uint32_t n = 0; n < RD_AUX_ENGINES; n++)
        if (dw == HPDR(regHPD0_DC_HPD_INT_CONTROL, n)) {
            if (v & HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_ACK_MASK)
                S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, n)] &= ~HPD0_DC_HPD_INT_STATUS__DC_HPD_INT_STATUS_MASK;
            *R(dw) = v & ~HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_ACK_MASK;
            return;
        }
    /* MES: a pipe held in reset loses its queue (mes_v11_0_enable resets
     * both pipes before it starts them) */
    if (dw == GCR(regCP_MES_CNTL) && (v & CP_MES_CNTL__MES_PIPE0_RESET_MASK)) {
        HQD(&S.mes_hqd[0], regCP_HQD_ACTIVE) = 0;
        S.mes_hqd[0].mapped_by_kiq = 0;
    }
    /* DCN power domains, DET segments */
    if (dw >= DCN_B2 + regDOMAIN0_PG_CONFIG && dw < DCN_B2 + regDOMAIN0_PG_CONFIG + 4 * 2 &&
        !((dw - DCN_B2 - regDOMAIN0_PG_CONFIG) & 1)) {
        uint32_t gate = (v & DOMAIN0_PG_CONFIG__DOMAIN_POWER_GATE_MASK) ? 2u : 0u;
        *R(dw) = v;
        if (!(S.reg[DCN_B2 + regDC_IP_REQUEST_CNTL] & DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK))
            S.dcn_pg_no_request++;
        else
            S.reg[dw + 1] = gate << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT;
        return;
    }
    if (dw >= DCN_B2 + regDCHUBBUB_DET0_CTRL && dw < DCN_B2 + regDCHUBBUB_DET0_CTRL + 4) {
        uint32_t sz = v & DCHUBBUB_DET0_CTRL__DET0_SIZE_MASK;
        *R(dw) = (v & ~DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT_MASK) |
                 (sz << DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT__SHIFT);
        return;
    }
    if (S.dcn_pg) {
        for (uint32_t n = 0; n < RD_DCN_MAX_PIPES; n++) {
            uint32_t st = S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2 * n];
            if (!(st & DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK))
                continue;
            for (size_t k = 0; k < sizeof(rd_clone_hubp) / 2 + 1; k++) {
                uint32_t ro = k < sizeof(rd_clone_hubp) / 2 ? rd_clone_hubp[k] : regHUBP0_DCHUBP_CNTL;
                if (dw == DCN_B2 + ro + RD_CLONE_HUBP_STRIDE * n) {
                    S.dcn_gated_drops++;
                    return;
                }
            }
            for (size_t k = 0; k < sizeof(rd_clone_dpp) / 2; k++)
                if (dw == DCN_B2 + rd_clone_dpp[k] + RD_CLONE_DPP_STRIDE * n) {
                    S.dcn_gated_drops++;
                    return;
                }
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
        if (S.smu_wedge_at && S.dmub.nseq >= S.smu_wedge_at) {
            *R(dw) = v;          /* taken, never answered */
            return;
        }
        smu_msg(v);
        return;
    }
    if (dw == RD_DAL_MSG_REG) {
        uint32_t arg = S.reg[RD_DAL_ARG_REG];
        S.dal_msgs++;
        S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;
        if (v == RD_DALSMC_MSG_NumOfDisplays)
            S.dal_displays = arg;
        else if (v == RD_DALSMC_MSG_SetHardMinByFreq && (arg >> 16) < 16) {
            S.dal_hard_min[arg >> 16] = arg & 0xffff;
            S.reg[RD_DAL_ARG_REG] = (arg & 0xffff) * 1000;
        } else if (v == RD_DALSMC_MSG_ReturnHardMinStatus)
            S.reg[RD_DAL_ARG_REG] = 0x3f;
        else
            S.reg[RD_DAL_RESP_REG] = 0xfe;   /* DALSMC_Result_UnknownCmd */
        *R(dw) = v;
        return;
    }
    /* DMCUB */
    if (dw == DMR(regDMCUB_GPINT_DATAIN1)) {
        *R(dw) = v;
        if (S.dmub.running && (v >> 28) && ((v >> 16) & 0xfff) == RD_DMUB_GPINT_STOP_FW) {
            S.dmub.running = 0;
            S.dmub.stops++;
            S.reg[DMR(regDMCUB_SCRATCH7)] = RD_DMUB_GPINT_STOP_FW_RESPONSE;
            S.reg[DMR(regDMCUB_CNTL)] |= DMCUB_CNTL__DMCUB_PWAIT_MODE_STATUS_MASK;
        }
        return;
    }
    if (dw == DMR(regDMCUB_CNTL) || dw == DMR(regDMCUB_CNTL2)) {
        int was = (S.reg[DMR(regDMCUB_CNTL)] & DMCUB_CNTL__DMCUB_ENABLE_MASK) &&
                  !(S.reg[DMR(regDMCUB_CNTL2)] & DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK);
        int now;
        *R(dw) = v;
        now = (S.reg[DMR(regDMCUB_CNTL)] & DMCUB_CNTL__DMCUB_ENABLE_MASK) &&
              !(S.reg[DMR(regDMCUB_CNTL2)] & DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK);
        if (!now) {
            S.dmub.running = 0;
        } else if (!was) {
            S.reg[DMR(regDMCUB_CNTL)] &= ~DMCUB_CNTL__DMCUB_PWAIT_MODE_STATUS_MASK;
            dmub_boot();
        }
        return;
    }
    if (dw == DMR(regDMCUB_INBOX1_WPTR)) {
        *R(dw) = v;
        dmub_inbox(v);
        return;
    }
    /* DP AUX engines (DCN segment 2) */
    if (dw == DCN_B2 + regDC_I2C_DATA) {
        uint32_t idx = (v & DC_I2C_DATA__DC_I2C_INDEX_MASK) >> DC_I2C_DATA__DC_I2C_INDEX__SHIFT;
        if (v & DC_I2C_DATA__DC_I2C_DATA_RW_MASK) {
            if (v & DC_I2C_DATA__DC_I2C_INDEX_WRITE_MASK)
                S.i2c_ri = idx;
        } else {
            if (v & DC_I2C_DATA__DC_I2C_INDEX_WRITE_MASK)
                S.i2c_wi = idx;
            S.i2c_buf[S.i2c_wi++ % sizeof(S.i2c_buf)] =
                (uint8_t)((v & DC_I2C_DATA__DC_I2C_DATA_MASK) >> DC_I2C_DATA__DC_I2C_DATA__SHIFT);
        }
        *R(dw) = v;
        return;
    }
    if (dw == DCN_B2 + regDC_I2C_CONTROL) {
        if (v & DC_I2C_CONTROL__DC_I2C_SW_STATUS_RESET_MASK)
            S.reg[DCN_B2 + regDC_I2C_SW_STATUS] = 0;
        *R(dw) = v & ~(DC_I2C_CONTROL__DC_I2C_GO_MASK | DC_I2C_CONTROL__DC_I2C_SW_STATUS_RESET_MASK |
                       DC_I2C_CONTROL__DC_I2C_SOFT_RESET_MASK);
        if (v & DC_I2C_CONTROL__DC_I2C_GO_MASK)
            i2c_go(v);
        return;
    }
    if (dw == DCN_B2 + regDC_I2C_ARBITRATION) {
        uint32_t st = (S.reg[dw] & DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK) >>
                      DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS__SHIFT;
        if (v & DC_I2C_ARBITRATION__DC_I2C_SW_USE_I2C_REG_REQ_MASK)
            st = 1;
        if (v & DC_I2C_ARBITRATION__DC_I2C_SW_DONE_USING_I2C_REG_MASK)
            st = 0;
        *R(dw) = (v & ~(DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK |
                        DC_I2C_ARBITRATION__DC_I2C_SW_DONE_USING_I2C_REG_MASK)) |
                 (st << DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS__SHIFT);
        return;
    }
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
    if (dw == GCR(regRLC_CGTT_MGCG_OVERRIDE) || dw == GCR(regRLC_CGCG_CGLS_CTRL)) {
        S.cg_writes++;
        if (!S.rlc_safe)
            S.cg_outside_safe++;
        *R(dw) = v;
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
        if (v & GRBM_SOFT_RESET__SOFT_RESET_SDMA0_MASK) {
            /* SDMA0 back to its reset state: halted, queue off, unfrozen */
            S.sdma_resets++;
            if (!(S.reg[GC_BASE0 + regSDMA0_FREEZE] & SDMA0_FREEZE__FREEZE_MASK))
                S.sdma_resets_unfrozen++;
            S.sdma_hang = 0;
            S.sdma_rptr = 0;
            S.reg[GC_BASE0 + regSDMA0_FREEZE] = 0;
            S.reg[0xA000 + regSDMA0_F32_CNTL] = SDMA0_F32_CNTL__HALT_MASK;
            S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_CNTL] &= ~SDMA0_QUEUE0_RB_CNTL__RB_ENABLE_MASK;
        }
        if (v & GRBM_SOFT_RESET__SOFT_RESET_CP_MASK) {
            /* CP/CPF/CPC/CPG reset: queues gone, pointers at 0, VMID reset done */
            S.soft_resets++;
            S.cp_rptr = 0;
            memset(S.hqd, 0, sizeof(S.hqd));
            memset(S.mes_hqd, 0, sizeof(S.mes_hqd));
            S.gfx_mapped = 0;
            *R(GCR(regCP_GFX_HQD_ACTIVE)) = 0;
            *R(GCR(regCP_GFX_MQD_BASE_ADDR)) = 0;
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
    if (dw == mmMM_DATA) {
        uint32_t *w = mm_word();
        return w ? *w : 0;
    }
    if (dw == DCN_B2 + regDC_I2C_DATA) {
        uint32_t b = S.i2c_buf[S.i2c_ri++ % sizeof(S.i2c_buf)];
        return (S.reg[dw] & ~DC_I2C_DATA__DC_I2C_DATA_MASK) | (b << DC_I2C_DATA__DC_I2C_DATA__SHIFT);
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
    if (dw == 0x16A00u + regGOLDEN_TSC_COUNT_UPPER) {
        S.tsc += 4;
        return (uint32_t)(S.tsc >> 32);
    }
    if (dw == 0x16A00u + regGOLDEN_TSC_COUNT_LOWER) {
        S.tsc += 4;
        return (uint32_t)S.tsc;
    }
    if (dw == GCR(regCC_GC_SHADER_ARRAY_CONFIG)) {
        /* per SE/SA: the one GRBM_GFX_INDEX selects */
        uint32_t sel = S.reg[GCR(regGRBM_GFX_INDEX)];
        uint32_t se = (sel & GRBM_GFX_INDEX__SE_INDEX_MASK) >> GRBM_GFX_INDEX__SE_INDEX__SHIFT;
        uint32_t sa = (sel & GRBM_GFX_INDEX__SA_INDEX_MASK) >> GRBM_GFX_INDEX__SA_INDEX__SHIFT;
        if ((sel & (GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK | GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK)) ||
            se >= 8 || sa >= 2)
            return S.reg[dw];
        return (S.reg[dw] & 0xffff) | (S.harvest_wgp[se][sa] << 16);
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
static void dcn_tick(uint32_t ms)
{
    for (uint32_t o = 0; o < RD_DCN_MAX_PIPES; o++) {
        uint32_t *fc = &S.reg[OTG(regOTG0_OTG_STATUS_FRAME_COUNT, o)];
        if (!(S.reg[OTG(regOTG0_OTG_CONTROL, o)] & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK))
            continue;
        *fc = (*fc + (ms + 15) / 16) & OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
        if (!(S.reg[OTG(regOTG0_OTG_CRC_CNTL, o)] & OTG0_OTG_CRC_CNTL__OTG_CRC_EN_MASK))
            continue;
        for (uint32_t p = 0; p < RD_DCN_MAX_PIPES; p++) {
            uint32_t cntl = S.reg[HUBP(regHUBP0_DCHUBP_CNTL, p)];
            uint32_t crc;
            if ((cntl & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK) ||
                ((cntl & HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK) >> HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT) != o)
                continue;
            crc = (S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p)] * 2654435761u) ^
                  S.reg[OTG(regOTG0_OTG_H_TOTAL, o)];
            S.reg[OTG(regOTG0_OTG_CRC0_DATA_RG, o)] = (crc & 0xffff) | ((crc >> 16) << 16);
            S.reg[OTG(regOTG0_OTG_CRC0_DATA_B, o)] = (crc ^ 0xa5a5) & 0xffff;
        }
    }
}
static void sim_msleep(void *c, uint32_t ms)
{
    (void)c;
    S.now_us += 1000ull * ms;
    if (S.dcn_frames)
        dcn_tick(ms);
}
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
    IP(AMD_HWID_ATHUB, 3, 0, 0, 0xC00, 0, 0);
    IP(AMD_HWID_MMHUB, 3, 0, 2, MM_BASE0, 0, 0);
    IP(AMD_HWID_OSSSYS, 6, 0, 0, OSS_BASE0, 0, 0);
    IP(AMD_HWID_SDMA0, 6, 0, 3, GC_BASE0, 0xA000, 0x1C000);
    IP(AMD_HWID_DMU, 3, 2, 1, 0x12, 0xC0, DCN_B2);
    disc->ips[disc->num_ips - 1].num_base = disc->ips[disc->num_ips - 1].num_base_kept = 4;
    disc->ips[disc->num_ips - 1].base[3] = DCN_B3;
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
    /* dmub_fw_meta at the end of the image, in front of a 256-byte PSP footer */
    {
        uint8_t *m = syn_blob[AMD_BLOB_DMCUB] + 256 + 1024 - 256 - 64;
        w32(syn_blob[AMD_BLOB_DMCUB] + 16, 0x07001234u);
        memset(m, 0, 64);
        w32(m, RD_DMUB_FW_META_MAGIC);
        w32(m + 4, 8192);          /* fw_region_size */
        w32(m + 8, 4096 + 16);     /* trace_buffer_size */
        w32(m + 12, 0x07001234u);
        m[16] = 1;
    }
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
    S.i2c_sink = -1;
    S.verbose = getenv("SIM_VERBOSE") != NULL;
    S.vram = calloc(1, BAR_LEN);
    *R(MP0(35)) = 0x80000000u;  /* bootloader ready */
    make_vbios();
    make_sink();
    /* DMCUB fused on, and still running the firmware the VBIOS started */
    *R(DCN_B2 + regCC_DC_PIPE_DIS) = CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK;
    *R(DCN_B2 + regDMCUB_CNTL) = DMCUB_CNTL__DMCUB_ENABLE_MASK;
    *R(DCN_B2 + regDMCUB_SCRATCH0) = RD_DMUB_BOOT_DAL_FW | RD_DMUB_BOOT_MAILBOX_RDY;
    S.dmub.running = 1;
    *smn_slot(SMN_ROM_OFFSET_CTRL, 1) = ROM_STRAP;
    *R(MP0(33)) = 0x80000000u;  /* IFWI done */
    *R(mmRCC_CONFIG_MEMSIZE) = (uint32_t)(VRAM_SIZE >> 20);
    /* the IFWI programs the FB location in MMHUB; the GC copy reads 0 until
     * the gfxhub is set up (as on the RX 7800 XT, 2026-10-09) */
    *R(MM_BASE0 + regMMMC_VM_FB_LOCATION_BASE) = (uint32_t)(FB_START >> 24);
    *R(MM_BASE0 + regMMMC_VM_FB_LOCATION_TOP) = (uint32_t)((FB_START + VRAM_SIZE - 1) >> 24);
    *R(MM_BASE0 + regMMMC_VM_FB_OFFSET) = 0;
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_BASE) = 0;
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_TOP) = 0;

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
    /* the TMR goes top-down out of the CPU-visible window, below the
     * firmware's reservation (amdgpu: NO_CPU_ACCESS kernel BO, TTM top-down) */
    rdna3_vram_alloc_top_init(&dev, VRAM_SIZE - (256ull << 20), VRAM_SIZE - (16ull << 20));
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
    CHECK(psp.tmr >= VRAM_SIZE - (256ull << 20) && psp.tmr + psp.tmr_size <= VRAM_SIZE - (16ull << 20) &&
          (psp.tmr & (PSP_TMR_ALIGNMENT - 1)) == 0 && S.tmr_mc == FB_START + psp.tmr);
    CHECK(dev.alloc_next < (32u << 20));          /* the visible window stays free */
    CHECK(S.nipfw == plan.count);
    CHECK(S.ip_fw_types[0] == GFX_FW_TYPE_SMU);
    CHECK(S.autoload && S.autoload_after_rlc_g);
    CHECK(S.unflushed_doorbells == 0);

    CHECK(smu13_hello(&dev, &smu_ver, &ifv) == RDNA3_OK);
    CHECK(smu_ver == 0x00526100 && ifv == SMU13_0_0_DRIVER_IF_VERSION);
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
        CHECK(vbios_fw_reserved_fb_size(&vb) == (16ull << 20));
        CHECK(boot.gfxclk == 50000 && boot.uclk == 9600 && boot.socclk == 60000 &&
              boot.fclk == 100000 && boot.vclk == 50000 && boot.vddc == 800 && boot.fw_crev == 4 &&
              boot.firmware_revision == 0x02000f && boot.pp_table_id == 0);
        CHECK(vbios_fw_vram_reservation(&vb, &rs, &rsz) == RDNA3_OK &&
              rs == VRAM_SIZE - (48ull << 20) && rsz == (48ull << 20));
        CHECK(smu13_check_fw_status(&dev) == RDNA3_OK);

        /* the default (no Display Core) feature set on a Gen4 x16 link */
        CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_SAFE, 3, 6, 0) == RDNA3_OK);
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
        /* smu_v13_0_gfx_off_control */
        CHECK(smu13_gfx_off(&dev, 1) == RDNA3_OK && S.smu.gfxoff == 1);
        CHECK(smu13_gfx_off(&dev, 0) == RDNA3_OK && S.smu.gfxoff == 0 && S.smu.gfxoff_msgs >= 2);

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
        CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_AMDGPU, 2, 4, 0) == RDNA3_OK);
        CHECK(smu.enabled == (RD_SMU_FEATURES_AMDGPU & ~RD_SMU_FEATURE(FEATURE_DPM_DCN_BIT)));
        CHECK(smu.uclk.count == 4 && smu.uclk.level[3] == 1218 && !smu.uclk.fine_grained);
        CHECK(smu.fclk.count == 2 && smu.fclk.level[1] == 1800);
        CHECK(S.smu.pcie_overrides == 3 && S.smu.last_pcie_arg == ((2u << 16) | (2u << 8) | 4));
        CHECK(smu13_read_metrics(&dev, &smu) == RDNA3_OK && smu13_metric_uclk_mhz(&smu) == 1218);
        CHECK(smu13_pm_fini(&dev, &smu) == RDNA3_OK && S.smu.unload);

        /* SCPM (consumer Navi 32): the PSP runtime database 1 MiB below the
         * top of VRAM says so; the PSP gave the PMFW its PPTable, so neither
         * the PPTable nor the allowed mask may be sent, the PMFW runs its own
         * features and UCLK/FCLK DPM get pinned to the top level */
        {
            uint32_t st = 0;
            uint8_t *db = S.vram_top + sizeof(S.vram_top) - RD_PSP_RUNTIME_DB_OFFSET;
            CHECK(psp13_scpm_enabled(&dev, &st) == 0);              /* no database yet */
            put16(db, RD_PSP_RUNTIME_DB_COOKIE_ID); put16(db + 2, 0x0100);
            put16(db + 4, 2);                                       /* entry_count */
            put32(db + 8, 5); put16(db + 12, 0x100); put16(db + 14, 8);   /* BOOT_CONFIG */
            put32(db + 16, RD_PSP_RUNTIME_ENTRY_SCPM); put16(db + 20, 0x200); put16(db + 22, 4);
            put32(db + 0x200, 1);                                   /* SCPM_ENABLE */
            CHECK(psp13_scpm_enabled(&dev, &st) == 1 && st == 1);
            memset(&S.smu.hard_min, 0, sizeof(S.smu.hard_min));
            S.smu.scpm = 1; S.smu.scpm_violations = 0;
            S.smu.pp_frev = 20;                                     /* PMFW 80.92 on the card */
            S.smu.pptable_written = 1; S.smu.btc = 0;               /* written by the PSP */
            CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_SAFE, 3, 6, 1) == RDNA3_OK);
            CHECK(S.smu.scpm_violations == 0 && S.smu.btc && smu.scpm && !smu.board_table_appended);
            CHECK(smu.pp_frev == 20 && smu.platform_caps == 0x19);
            CHECK(smu.enabled & RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT));
            CHECK(smu.uclk.count == 4 && S.smu.hard_min[PPCLK_UCLK] == 1218);
            CHECK(smu.fclk.count == 2 && S.smu.hard_min[PPCLK_FCLK] == 1800);
            /* amdgpu's own set: nothing pinned */
            memset(&S.smu.hard_min, 0, sizeof(S.smu.hard_min));
            CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_AMDGPU, 3, 6, 1) == RDNA3_OK);
            CHECK(S.smu.hard_min[PPCLK_UCLK] == 0 && S.smu.scpm_violations == 0);
            S.smu.scpm = 0; S.smu.pp_frev = 0;
            memset(db, 0, 0x300);
        }

        /* without a VBIOS the PMFW's placeholder BoardTable goes back as is */
        CHECK(smu13_pm_init(&dev, &smu, NULL, RD_SMU_FEATURES_SAFE, 3, 6, 0) == RDNA3_OK);
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
        CHECK(mmhub302_get_fb_location(&dev) == FB_START && dev.fb_start == FB_START);
        CHECK(mmhub302_get_mc_fb_offset(&dev) == dev.fb_offset);
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

            /* HPD hotplug through the IH (irq_service_dcn32): pin 2 has a
             * monitor, pin 3 none; enabling raises nothing, unplug and plug
             * raise one IV each, and the ack re-arms for the next change */
            {
                uint32_t pin, sense_before = S.hpd_irqs;
                int rx = 0;
                struct sim_seen hs = { 0, 0, 0, 0, 0, 0 };
                ih60_process(&dev, &ih, NULL, NULL);
                hpd_plug(2, 1);
                hpd_plug(3, 0);
                dchpd_irq_set(&dev, 2, 1);
                dchpd_irq_set(&dev, 3, 1);
                CHECK(S.hpd_irqs == sense_before);
                hpd_plug(2, 0);                        /* unplug */
                hpd_plug(2, 0);                        /* still latched: no second IV */
                CHECK(S.hpd_irqs == sense_before + 1);
                CHECK(ih60_process(&dev, &ih, sim_iv_cb, &hs) == 1 && hs.client == RD_IH_CLIENT_DCE &&
                      hs.src == RD_IH_SRC_DC_HPD1_INT);
                CHECK(dchpd_iv_pin(RD_IH_CLIENT_DCE, RD_IH_SRC_DC_HPD1_INT, 2, &rx) == 2 && !rx);
                CHECK(dchpd_iv_pin(RD_IH_CLIENT_DCE, RD_IH_SRC_DC_HPD1_INT, 8, &rx) == 2 && rx);
                CHECK(dchpd_iv_pin(RD_IH_CLIENT_GFX, RD_IH_SRC_DC_HPD1_INT, 2, &rx) == -1);
                CHECK(dchpd_irq_ack(&dev, 2) == 0);     /* disconnected, now waits for a plug */
                CHECK(S.hpd_irqs == sense_before + 1);  /* no storm after the ack */
                hpd_plug(2, 1);
                CHECK(S.hpd_irqs == sense_before + 2 && dchpd_irq_ack(&dev, 2) == 1);
                hpd_plug(3, 1);
                CHECK(S.hpd_irqs == sense_before + 3 && dchpd_irq_ack(&dev, 3) == 1);
                pin = (uint32_t)S.hpd_irqs;
                dchpd_irq_set(&dev, 3, 0);
                hpd_plug(3, 0);
                CHECK(S.hpd_irqs == pin);                /* disabled */
                dchpd_irq_set(&dev, 2, 0);
                ih60_process(&dev, &ih, NULL, NULL);
                S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] = 0;
                S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 3)] = 0;
            }
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
            /* user fence: two FENCE packets, low and high dword */
            sdma60_emit_fence64(&dev, &sd, rdna3_vram_mc(&dev, dst + 4096), 0x1234567800000042ull);
            k = sdma60_emit_fence(&dev, &sd);
            sdma60_commit(&dev, &sd);
            CHECK(k == 42 && sdma60_wait_fence(&dev, &sd, k) == RDNA3_OK);
            rdna3_vram_read(&dev, dst + 4096, out, 8);
            CHECK(out[0] == 0x42 && out[1] == 0x12345678u);
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
            /* a hung engine: the fence never lands until the soft reset,
             * which completes it and brings the queue back on the same ring */
            {
                uint32_t hung, again;
                S.sdma_hang = 1;
                sdma60_emit_copy(&dev, &sd, rdna3_vram_mc(&dev, src), rdna3_vram_mc(&dev, dst), 256);
                hung = sdma60_emit_fence(&dev, &sd);
                sdma60_commit(&dev, &sd);
                CHECK(!sdma60_fence_passed(&dev, &sd, hung));
                CHECK(sdma60_soft_reset(&dev, &sd) == RDNA3_OK && sd.ready);
                CHECK(S.sdma_resets == 1 && S.sdma_resets_unfrozen == 0 && !S.sdma_hang);
                CHECK(sdma60_fence_passed(&dev, &sd, hung) && sd.wptr > 0);
                CHECK(sdma60_copy_sync(&dev, &sd, rdna3_vram_mc(&dev, src), rdna3_vram_mc(&dev, dst),
                                       sizeof(pat)) == RDNA3_OK);
                again = sd.fence_seq;
                CHECK(again == hung + 1 && S.sdma_bad == 0);
            }
            /* a userspace DMA job (rdna3_drminfo --dma-test at rdna3lvl=7): IB
             * and target in one VRAM BO mapped in VMID 6. SDMA 6.0 walks the
             * GFXHUB, so a VM set up on MMHUB alone signals its fence and
             * writes nothing (the "DMA job results WRONG" on the card) until
             * gfx_v11_0_gfxhub_enable has run */
            {
                static rdna3_pt_pool upool;
                static rdna3_vm uvm;
                const uint64_t gva = 0x100000;
                const uint32_t ctx6 = GC_BASE0 + regGCVM_CONTEXT0_CNTL +
                                      6 * (regGCVM_CONTEXT1_CNTL - regGCVM_CONTEXT0_CNTL);
                uint64_t bo = rdna3_vram_alloc(&dev, 64 << 10, 4096, "test-user-bo");
                uint64_t f = gpuvm_pte_flags((1u << 1) | (1u << 2) | (1u << 3), 0);
                uint32_t ib[16], got[3], n = 0, seq, saved = S.reg[ctx6], pass;
                CHECK(gpuvm_pool_init(&dev, &upool, 16) == RDNA3_OK);
                CHECK(gpuvm_vm_init(&dev, &uvm, &upool) == RDNA3_OK);
                CHECK(gpuvm_map_linear(&dev, &uvm, gva, rdna3_vram_pa(&dev, bo), 16, f) == RDNA3_OK);
                ib[n++] = SDMA_OP_WRITE; ib[n++] = (uint32_t)(gva + 0x8000); ib[n++] = 0; ib[n++] = 1;
                ib[n++] = 0xC0FFEE01; ib[n++] = 0xC0FFEE02;
                ib[n++] = SDMA_OP_CONST_FILL; ib[n++] = (uint32_t)(gva + 0xC000); ib[n++] = 0;
                ib[n++] = 0x5A5A5A5A; ib[n++] = 255;
                while (n & 7)
                    ib[n++] = 0;
                rdna3_vram_fill(&dev, bo, 0, 64 << 10);
                rdna3_vram_write(&dev, bo, ib, 4ull * n);
                for (pass = 0; pass < 2; pass++) {
                    uint32_t faults = S.sdma_ib_fault;
                    if (!pass) {
                        S.reg[ctx6] = 0;   /* level 7 before the fix: MMHUB only */
                        mmhub302_setup_vm_pt_regs(&dev, 6, gpuvm_pd_addr(&dev, &uvm));
                        mmhub302_flush_tlb(&dev, 6, 0);
                    } else {
                        S.reg[ctx6] = saved;
                        CHECK(gfx11_gfxhub_enable(&dev, &gmc) == RDNA3_OK && gmc.gfxhub_enabled);
                        CHECK(S.reg[ctx6] & GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK);
                        gfxhub303_setup_vm_pt_regs(&dev, 6, gpuvm_pd_addr(&dev, &uvm));
                        CHECK(gfxhub303_flush_tlb(&dev, 6, 0) == RDNA3_OK);
                    }
                    sdma60_emit_ib(&dev, &sd, 6, gva, n);
                    seq = sdma60_emit_fence(&dev, &sd);
                    sdma60_commit(&dev, &sd);
                    CHECK(sdma60_wait_fence(&dev, &sd, seq) == RDNA3_OK);
                    rdna3_vram_read(&dev, bo + 0x8000, got, 8);
                    rdna3_vram_read(&dev, bo + 0xC000 + 252, &got[2], 4);
                    if (!pass)
                        CHECK(S.sdma_ib_fault == faults + 1 && got[0] == 0 && got[2] == 0);
                    else
                        CHECK(S.sdma_ib_fault == faults && got[0] == 0xC0FFEE01 &&
                              got[1] == 0xC0FFEE02 && got[2] == 0x5A5A5A5A);
                }
                gpuvm_vm_fini(&dev, &uvm);
                CHECK(S.sdma_bad == 0);
            }
            /* VRAM -> scattered system pages through a GART window, as the
             * IOSurface page-off does: 3 pages in a 2-page window = two chunks */
            {
                uint64_t pg[3] = { SYS_PHYS + 0x5000, SYS_PHYS + 0x1000, SYS_PHYS + 0x9000 };
                uint64_t win = gmc.gart_size - 16 * 4096, wpte = 0;
                uint32_t pat2[3 * 1024], f0 = S.mm_tlb_flushes;
                for (j = 0; j < 3 * 1024; j++) pat2[j] = 0xc0de0000u + j;
                uint64_t xsrc = rdna3_vram_alloc(&dev, 3 * 4096, 4096, "xfer-src");
                rdna3_vram_write(&dev, xsrc, pat2, sizeof(pat2));
                memset(S.sysmem, 0, sizeof(S.sysmem));
                CHECK(rdna3_copy_vram_to_pages(&dev, &gmc, &sd, xsrc, pg, 3, 3 * 4096 - 100, win, 2) == RDNA3_OK);
                CHECK(!memcmp(S.sysmem + 0x5000, pat2, 4096) && !memcmp(S.sysmem + 0x1000, pat2 + 1024, 4096) &&
                      !memcmp(S.sysmem + 0x9000, pat2 + 2048, 4096 - 100) && S.sysmem[0x9000 + 4096 - 100] == 0);
                CHECK(S.sdma_bad == 0 && S.gart_unmapped == 0 && S.mm_tlb_flushes >= f0 + 4);
                memcpy(&wpte, S.vram + gmc.gart_table + (win / 4096) * 8, 8);
                CHECK(wpte == 0);                           /* the window is left unmapped */
                CHECK(rdna3_copy_vram_to_pages(&dev, &gmc, &sd, src, pg, 1, 8192, win, 2) == RDNA3_EINVAL);
                pg[1] |= 0x10;
                CHECK(rdna3_copy_vram_to_pages(&dev, &gmc, &sd, src, pg, 3, 8192, win, 2) == RDNA3_EINVAL);
                memcpy(&wpte, S.vram + gmc.gart_table + (win / 4096) * 8, 8);
                CHECK(wpte == 0);                          /* nothing mapped on a refusal */
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
            /* gfx_v11_0_cp_gfx_resume programs RB0_CNTL with BUFSZ/BLKSZ only;
             * KMD_QUEUE belongs to the MQD queues (the card stalled with the
             * CPG reading MC 0 when RB0 had it) */
            CHECK(!(S.reg[GCR(regCP_RB0_CNTL)] & CP_RB0_CNTL__KMD_QUEUE_MASK));
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
                /* user fence (AMDGPU_CHUNK_ID_FENCE): the job's number as
                 * 64 bits, written before the ring fence */
                {
                    uint64_t uf = 0xDEADBEEFDEADBEEFull;
                    rdna3_gfx_job_ext x;
                    memset(&x, 0, sizeof(x));
                    x.uf_addr = rdna3_vram_mc(&dev, dst + 64);
                    rdna3_vram_write(&dev, dst + 64, &uf, 8);
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                    rdna3_vram_read(&dev, dst + 64, &uf, 8);
                    CHECK(uf == seq && S.cp_bad == 0);
                    /* GDS switch (gfx_v11_0_ring_emit_gds_switch): VMID 5
                     * gets GDS bytes 0x100+0x200, GWS 4..11, OA 2..4; hw
                     * init left VMIDs 1-15 without any */
                    CHECK(S.reg[GC_BASE0 + regGDS_VMID0_SIZE + 10] == 0 &&
                          S.reg[GC_BASE0 + regGDS_OA_VMID0 + 5] == 0);
                    memset(&x, 0, sizeof(x));
                    x.gds_switch = 1;
                    x.gds_base = 0x100; x.gds_size = 0x200;
                    x.gws_base = 4; x.gws_size = 8; x.oa_base = 2; x.oa_size = 3;
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                    CHECK(S.reg[GC_BASE0 + regGDS_VMID0_BASE + 10] == 0x100 &&
                          S.reg[GC_BASE0 + regGDS_VMID0_SIZE + 10] == 0x200 &&
                          S.reg[GC_BASE0 + regGDS_GWS_VMID0 + 5] == ((8u << 16) | 4) &&
                          S.reg[GC_BASE0 + regGDS_OA_VMID0 + 5] == 0x1C && S.cp_bad == 0);
                    jobs[0].va = ibva;
                }
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

                /* KIQ, then KCQ0 (MEC1 pipe 0 queue 0) mapped by the KIQ's
                 * MAP_QUEUES; a compute job runs the same IB through the page
                 * tables. A MEC KIQ (pipe 1) passes its ring test but stalls
                 * on SET_RESOURCES (the card); GC 11 runs the KIQ on MES
                 * pipe 1 (mes_v11_0_kiq_hw_init) */
                {
                    uint32_t ibs0 = S.cp_ibs, tlb0 = S.ring_tlb_flushes, sync0 = S.cp_pfp_sync, acq0 = S.cp_acquire;
                    uint32_t fault0 = S.cp_ib_fault;   /* the gfx test faulted one IB on purpose */
                    const uint64_t ucs[2] = { 0x10000, 0x20000 };
                    struct hqd *hk = &S.mes_hqd[1], *hc = &S.hqd[RD_KCQ0_PIPE][0];
                    CHECK(gfx11_mec_ring_init(&dev, &kiq, 1, RD_KIQ_PIPE, 0, RD_DB_KIQ, 0) == RDNA3_OK);
                    CHECK(gfx11_mec_ring_init(&dev, &kcq, 0, RD_KCQ0_PIPE, 0, RD_DB_MEC_RING0, 0) == RDNA3_OK);
                    CHECK(kiq.rb.kind == RD_RING_KIQ && kcq.rb.kind == RD_RING_COMPUTE && kcq.rb.doorbell == 6);
                    CHECK(gfx11_kcq_start(&dev, &kiq, &kcq) == RDNA3_EINVAL);   /* KIQ first */
                    CHECK(gfx11_kiq_start(&dev, &kiq) == RDNA3_OK && kiq.rb.ready);
                    CHECK(S.reg[0xC000 + 0x40] == 0xDEADBEEF);
                    CHECK(gfx11_kcq_start(&dev, &kiq, &kcq) == RDNA3_ETIME && !kcq.mapped);
                    CHECK(S.mec_kiq_sched_ignored >= 1 && S.kiq_maps == 0);
                    gfx11_mec_stop(&dev, &kiq, NULL, NULL);
                    S.hqd[RD_KIQ_PIPE][0].reg[regCP_HQD_ACTIVE - HQD_FIRST] = 0;
                    CHECK(gfx11_mes_kiq_ring_init(&dev, &kiq, ucs) == RDNA3_OK && kiq.me == 3 && kiq.pipe == 1);
                    CHECK(kiq.rb.doorbell == RD_DB_INDEX(RD_DB_MES_RING1));
                    S.reg[0xC000 + 0x40] = 0;
                    CHECK(gfx11_kiq_start(&dev, &kiq) == RDNA3_OK && kiq.rb.ready);
                    CHECK((S.reg[GCR(regRLC_CP_SCHEDULERS)] & 0xff) == (0x80 | (3u << 5) | (1u << 3)));
                    CHECK((S.reg[GCR(regCP_MES_CNTL)] & (CP_MES_CNTL__MES_PIPE0_ACTIVE_MASK | CP_MES_CNTL__MES_PIPE1_ACTIVE_MASK)) ==
                          (CP_MES_CNTL__MES_PIPE0_ACTIVE_MASK | CP_MES_CNTL__MES_PIPE1_ACTIVE_MASK));
                    CHECK(!(S.reg[GCR(regCP_MES_CNTL)] & (CP_MES_CNTL__MES_HALT_MASK | CP_MES_CNTL__MES_PIPE1_RESET_MASK)));
                    CHECK(S.reg[GCR(regCP_MES_PRGRM_CNTR_START)] == 0x20000 >> 2);   /* pipe 1 written last */
                    CHECK(!(S.reg[GCR(regCP_MEC_RS64_CNTL)] & CP_MEC_RS64_CNTL__MEC_HALT_MASK));
                    CHECK(S.reg[GCR(regCP_MEC_RS64_CNTL)] & CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK);
                    CHECK(HQD(hk, regCP_HQD_ACTIVE) == 1 && !hk->mapped_by_kiq);
                    CHECK(HQD(hk, regCP_HQD_PQ_BASE) == (uint32_t)(rdna3_vram_mc(&dev, kiq.rb.ring) >> 8));
                    CHECK(((HQD(hk, regCP_HQD_PQ_DOORBELL_CONTROL) & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == kiq.rb.doorbell);
                    CHECK(HQD(hk, regCP_MQD_BASE_ADDR) == (uint32_t)rdna3_vram_mc(&dev, kiq.mqd));
                    CHECK(HQD(hk, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR_MASK);
                    CHECK(S.reg[GCR(regGRBM_GFX_CNTL)] == 0);   /* selection restored */
                    CHECK(S.reg[0xC000 + 0x40] == 0xDEADBEEF && S.cp_bad == 0);
                    CHECK(gfx11_kcq_start(&dev, &kiq, &kcq) == RDNA3_OK && kcq.rb.ready && kcq.mapped);
                    CHECK(S.kiq_maps == 1 && S.set_resources_mask == 1ull && S.mec_bad_queue == 0);
                    CHECK(HQD(hc, regCP_HQD_ACTIVE) == 1 && hc->mapped_by_kiq);
                    CHECK(HQD(hc, regCP_HQD_PQ_BASE) == (uint32_t)(rdna3_vram_mc(&dev, kcq.rb.ring) >> 8));
                    CHECK(((HQD(hc, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK)) == 9);   /* 1024 dw */
                    CHECK(HQD(hc, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK);
                    CHECK(S.cp_bad == 0 && S.db_unclaimed == 0);
                    /* C1: the MES scheduler pipe, mapped by the KIQ, answers
                     * SET_HW_RESOURCES and the status query through their
                     * fences; a silent firmware is reported, the pipe dequeued */
                    {
                        static rdna3_mes mes;
                        struct hqd *hm = &S.mes_hqd[0];
                        const uint32_t *hw = S.mes_sched.hw;
                        memset(&mes, 0, sizeof(mes));   /* run() goes twice */
                        memset(&S.mes_sched, 0, sizeof(S.mes_sched));
                        S.reg[GCR(regCP_MES_GP3_LO)] = 0x0000004a;
                        CHECK(gfx11_mes_sched_start(&dev, &kiq, &mes) == RDNA3_EINVAL);   /* no ring yet */
                        CHECK(gfx11_mes_sched_init(&dev, &mes) == RDNA3_OK && mes.ring.me == 3 && mes.ring.pipe == 0);
                        CHECK(mes.ring.rb.doorbell == RD_DB_INDEX(RD_DB_MES_RING0));
                        CHECK(gfx11_mes_sched_start(&dev, &kiq, &mes) == RDNA3_OK && mes.ready);
                        CHECK(S.mes_sched.maps == 1 && HQD(hm, regCP_HQD_ACTIVE) == 1 && hm->mapped_by_kiq);
                        CHECK(HQD(hm, regCP_HQD_PQ_BASE) == (uint32_t)(rdna3_vram_mc(&dev, mes.ring.rb.ring) >> 8));
                        CHECK(S.mes_sched.hw_rsrc == 1 && S.mes_sched.queries == 3 && S.mes_sched.before_rsrc == 0 &&
                              S.mes_sched.bad == 0);
                        CHECK(hw[1] == 0xffffff00 && hw[2] == 0xffffff00 && hw[3] == 0x1000 && hw[5] == 0xc &&
                              hw[8] == 0xc && hw[9] == 0 && hw[13] == 0xfffffffe && hw[15] == 0xfc &&
                              hw[17] == RD_MES_DB_START_DW && hw[21] == RD_MES_DB_START_DW + 8);
                        CHECK(hw[26] == GC_BASE0 && hw[27] == 0xA000 && hw[34] == MM_BASE0 && hw[42] == OSS_BASE0);
                        CHECK(hw[54] == 0x447 && hw[55] == 50);
                        CHECK(mes.sched_version == 0x4a && mes.legacy_queue_map && mes.seq == 2 && mes.failed == 0);
                        CHECK(S.cp_bad == 0 && S.db_unclaimed == 0);
                        /* C2: the KIQ restart of a soft reset (mes_v11_0_kiq_hw_init)
                         * resets both MES pipes: the scheduler is gone and has to
                         * be started again, as the kext does after a soft reset */
                        CHECK(gfx11_kiq_start(&dev, &kiq) == RDNA3_OK && HQD(hm, regCP_HQD_ACTIVE) == 0);
                        CHECK(gfx11_mes_sched_start(&dev, &kiq, &mes) == RDNA3_OK && mes.ready &&
                              S.mes_sched.maps == 2 && S.mes_sched.hw_rsrc == 2 && HQD(hm, regCP_HQD_ACTIVE) == 1);
                        /* the firmware stops answering */
                        S.mes_sched.hang = 1;
                        {
                            uint32_t pk[RD_MES_API_DWORDS] = { RD_MES_API_HEADER(RD_MES_SCH_API_QUERY_SCHEDULER_STATUS) };
                            CHECK(gfx11_mes_submit(&dev, &mes, pk, 2) == RDNA3_ETIME && mes.failed == 1);
                        }
                        S.mes_sched.hang = 0;
                        gfx11_mes_sched_stop(&dev, &mes);
                        CHECK(!mes.ready && !mes.ring.mapped && HQD(hm, regCP_HQD_ACTIVE) == 0 &&
                              HQD(hm, regCP_HQD_PQ_DOORBELL_CONTROL) == 0);
                        CHECK(S.reg[GCR(regGRBM_GFX_CNTL)] == 0);
                    }
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
                        /* gfx_v11_0_update_gfx_clock_gating for GC 11.0.3, in RLC safe mode */
                        {
                            uint32_t ov0 = RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGCG_OVERRIDE_MASK |
                                           RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK |
                                           RLC_CGTT_MGCG_OVERRIDE__GRBM_CGTT_SCLK_OVERRIDE_MASK |
                                           RLC_CGTT_MGCG_OVERRIDE__RLC_CGTT_SCLK_OVERRIDE_MASK |
                                           RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK |
                                           RLC_CGTT_MGCG_OVERRIDE__GFXIP_REPEATER_FGCG_OVERRIDE_MASK;
                            S.reg[GCR(regRLC_CNTL)] = 0;
                            CHECK(gfx11_set_clock_gating(&dev, 1, RD_GFX_CG_GC_11_0_3) == RDNA3_ENODEV);
                            S.reg[GCR(regRLC_CNTL)] = RLC_CNTL__RLC_ENABLE_F32_MASK;
                            S.reg[GCR(regRLC_CGTT_MGCG_OVERRIDE)] = ov0 | 0x80000000u;
                            S.reg[GCR(regRLC_CGCG_CGLS_CTRL)] = 0;
                            S.cg_writes = S.cg_outside_safe = 0;
                            CHECK(gfx11_set_clock_gating(&dev, 1, RD_GFX_CG_GC_11_0_3) == RDNA3_OK);
                            CHECK(S.cg_writes > 0 && S.cg_outside_safe == 0 && !S.rlc_safe);
                            CHECK(S.reg[GCR(regRLC_CGTT_MGCG_OVERRIDE)] == 0x80000000u);   /* only the overrides cleared */
                            CHECK((S.reg[GCR(regRLC_CGCG_CGLS_CTRL)] & RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK) &&
                                  (S.reg[GCR(regRLC_CGCG_CGLS_CTRL)] & RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK) &&
                                  ((S.reg[GCR(regRLC_CGCG_CGLS_CTRL)] & RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD_MASK) >>
                                   RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD__SHIFT) == 0x36);
                            CHECK((S.reg[GCR(regCP_RB_WPTR_POLL_CNTL)] & (CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY_MASK |
                                   CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK)) == 0x00900100u);
                            CHECK(S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__GFX_IDLE_INT_ENABLE_MASK);
                            CHECK(gfx11_set_clock_gating(&dev, 0, RD_GFX_CG_GC_11_0_3) == RDNA3_OK);
                            CHECK(S.cg_outside_safe == 0 && !S.rlc_safe &&
                                  (S.reg[GCR(regRLC_CGTT_MGCG_OVERRIDE)] & (ov0 & ~(RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGCG_OVERRIDE_MASK |
                                   RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK))) ==
                                  (ov0 & ~(RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGCG_OVERRIDE_MASK | RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK)) &&
                                  !(S.reg[GCR(regRLC_CGCG_CGLS_CTRL)] & (RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK)) &&
                                  !(S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__GFX_IDLE_INT_ENABLE_MASK));
                        }
                        /* soc21_common/gmc_v11_0 set_clockgating_state for GC 11.0.3:
                         * HDP SRAM shutdown + ATHUB MGCG/LS; the GPU still runs jobs */
                        {
                            const uint32_t hclk = 0xF20 + regHDP_CLK_CNTL, hpwr = 0xF20 + regHDP_MEM_POWER_CTRL,
                                           ath = 0xC00 + regATHUB_MISC_CNTL;
                            S.reg[hclk] = 0x00000101u;
                            S.reg[hpwr] = HDP_MEM_POWER_CTRL__ATOMIC_MEM_POWER_LS_EN_MASK | 0x40000000u;
                            S.reg[ath] = 0x00000001u;
                            CHECK(soc21_clock_gating_state(&dev) == 0);
                            CHECK(soc21_set_clock_gating(&dev, 1) == RDNA3_OK);
                            CHECK(S.reg[hpwr] == (0x40000000u | HDP_MEM_POWER_CTRL__ATOMIC_MEM_POWER_SD_EN_MASK |
                                                  HDP_MEM_POWER_CTRL__RC_MEM_POWER_SD_EN_MASK |
                                                  HDP_MEM_POWER_CTRL__ATOMIC_MEM_POWER_CTRL_EN_MASK |
                                                  HDP_MEM_POWER_CTRL__RC_MEM_POWER_CTRL_EN_MASK));
                            CHECK(S.reg[hclk] == 0x00000101u);   /* override released again */
                            CHECK(S.reg[ath] == (0x1u | ATHUB_MISC_CNTL__CG_ENABLE_MASK | ATHUB_MISC_CNTL__CG_MEM_LS_ENABLE_MASK));
                            CHECK(soc21_clock_gating_state(&dev) ==
                                  (RD_SOC_CG_HDP_SD | RD_SOC_CG_ATHUB_MGCG | RD_SOC_CG_ATHUB_LS));
                            CHECK(soc21_set_clock_gating(&dev, 0) == RDNA3_OK);
                            CHECK(S.reg[hpwr] == 0x40000000u && S.reg[ath] == 0x1u && soc21_clock_gating_state(&dev) == 0);
                            CHECK(soc21_set_clock_gating(&dev, 1) == RDNA3_OK);
                        }
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
            /* the card's RS64 gfx firmware reads the queue's MQD: RB0 set up
             * through registers (amdgpu_async_gfx_ring=0) faults on MC 0 and
             * halts the CP; the KIQ-mapped queue (amdgpu's default) runs,
             * survives a soft reset and is unmapped through the KIQ */
            {
                static rdna3_gfx_ring ga;
                rdna3_rs64_start rs = { 0x1000, 0x2, 0x2000, 0x3, 0x4000, 0x1 };
                uint32_t aseq;
                S.rs64_needs_mqd = 1;
                memset(&ga, 0, sizeof(ga));
                CHECK(gfx11_hw_init(&dev, &gmc, &ga, &rs, 8192) == RDNA3_ETIME && !ga.ready);
                CHECK(S.cp_mqd_faults >= 1 && (S.reg[GCR(regCP_ME_CNTL)] & CP_ME_CNTL__PFP_HALT_MASK));
                memset(&ga, 0, sizeof(ga));
                ga.async = 1;
                CHECK(gfx11_kgq_start(&dev, &kiq, &ga) == RDNA3_EINVAL);   /* no ring yet */
                CHECK(gfx11_hw_init(&dev, &gmc, &ga, &rs, 8192) == RDNA3_OK && ga.async && !ga.ready);
                CHECK(!(S.reg[GCR(regCP_ME_CNTL)] & (CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__ME_HALT_MASK)));
                CHECK(gfx11_kgq_start(&dev, &kiq, &ga) == RDNA3_OK && ga.ready);
                CHECK(S.kgq_maps == 1 && S.gfx_mapped && S.reg[GCR(regCP_GFX_HQD_ACTIVE)] == 1);
                CHECK(S.reg[GCR(regCP_GFX_MQD_BASE_ADDR)] == (uint32_t)rdna3_vram_mc(&dev, ga.mqd));
                CHECK(S.reg[GCR(regCP_GFX_MQD_BASE_ADDR_HI)] == (uint32_t)(rdna3_vram_mc(&dev, ga.mqd) >> 32));
                CHECK(S.reg[GCR(regCP_GFX_HQD_BASE)] == (uint32_t)(rdna3_vram_mc(&dev, ga.ring) >> 8));
                CHECK(S.reg[GCR(regCP_GFX_HQD_RPTR_ADDR)] == (uint32_t)rdna3_vram_mc(&dev, ga.wb + RD_GFX_WB_RPTR));
                CHECK(S.reg[GCR(regCP_RB_WPTR_POLL_ADDR_LO)] == (uint32_t)rdna3_vram_mc(&dev, ga.wb + RD_GFX_WB_WPTR));
                CHECK(((S.reg[GCR(regCP_GFX_HQD_CNTL)] & CP_GFX_HQD_CNTL__RB_BUFSZ_MASK) >>
                       CP_GFX_HQD_CNTL__RB_BUFSZ__SHIFT) == 10);   /* 8 KiB = 2048 dw */
                CHECK(S.reg[GCR(regCP_GFX_MQD_CONTROL)] & CP_GFX_MQD_CONTROL__PRIV_STATE_MASK);
                CHECK(S.reg[0xC000 + 0x40] == 0xDEADBEEF);
                aseq = gfx11_emit_fence(&dev, &ga);
                gfx11_commit(&dev, &ga);
                CHECK(gfx11_wait_fence(&dev, &ga, aseq) == RDNA3_OK);
                CHECK(gfx11_soft_reset(&dev, &ga, &kiq, &kcq) == RDNA3_OK && ga.ready && kcq.rb.ready);
                CHECK(S.kgq_maps == 2 && S.gfx_mapped);
                aseq = gfx11_emit_fence(&dev, &ga);
                gfx11_commit(&dev, &ga);
                CHECK(gfx11_wait_fence(&dev, &ga, aseq) == RDNA3_OK);
                gfx11_kgq_stop(&dev, &kiq, &ga);
                CHECK(S.kgq_unmaps == 1 && !S.gfx_mapped && !ga.ready);
                CHECK(S.reg[GCR(regCP_ME_CNTL)] & CP_ME_CNTL__ME_HALT_MASK);
                CHECK(S.cp_bad == 0);
                S.rs64_needs_mqd = 0;
            }
            gfx11_set_eop_interrupt(&dev, &gr, 0);
            /* UNMAP_QUEUES through the KIQ, then the MEC halts */
            gfx11_mec_stop(&dev, &kiq, &kcq, NULL);
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
        /* the GOP's route: OTG 2 -> DIGC front end -> DIGB back end
         * (UNIPHY B) in HDMI mode; DIGA's back end is lit too but fed by
         * DIGA, which sources OTG 0 */
#define DIGR(reg, n) (DCN_B2 + reg + (regDIG1_DIG_FE_CNTL - regDIG0_DIG_FE_CNTL) * (n))
        S.reg[DIGR(regDIG0_DIG_FE_CNTL, 2)] = 2;
        S.reg[DIGR(regDIG0_DIG_BE_CNTL, 1)] = (1u << 2) << DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT |
                                              3u << DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT;
        S.reg[DIGR(regDIG0_DIG_BE_EN_CNTL, 1)] = DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
        S.reg[DIGR(regDIG0_DIG_FE_CNTL, 0)] = 0;
        S.reg[DIGR(regDIG0_DIG_BE_CNTL, 0)] = 1u << DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT;
        S.reg[DIGR(regDIG0_DIG_BE_EN_CNTL, 0)] = DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;

        CHECK(dcn32_read_state(&dev, &dc) == 1);
        CHECK(dc.head[0].pipe == 1 && dc.head[0].otg == 2);
        CHECK(dc.head[0].dig_be == 1 && dc.head[0].dig_fe == 2 && dc.head[0].dig_mode == 3);
        CHECK(dcn32_otg_dig(&dev, 0, NULL, NULL) == 0 && dcn32_otg_dig(&dev, 3, NULL, NULL) == -1);
        CHECK(dc.head[0].width == 2560 && dc.head[0].height == 1440 && dc.head[0].pitch_px == 2560);
        CHECK(dc.head[0].address == gop && dcn32_head_usable(&dev, &dc.head[0]));
        CHECK(dc.head[0].h_total == 2720 && dc.head[0].h_active == 2560 && dc.head[0].h_front == 48 &&
              dc.head[0].h_sync == 32);
        CHECK(dc.head[0].v_total == 1481 && dc.head[0].v_active == 1440 && dc.head[0].v_front == 3 &&
              dc.head[0].v_sync == 5 && dc.head[0].v_sync_neg && !dc.head[0].h_sync_neg);
        CHECK(dcn32_frame_count(&dev, &dc, 0) == 1234 && dcn32_scanline(&dev, &dc, 0) == 777);

        /* the mode computations: which EDID mode the GOP runs, and the OTG
         * values optc1_program_timing would write for it (they must be the
         * ones already in the OTG); DP bandwidth and the mode filter */
        {
            rdna3_edid e;
            rdna3_otg_timing t;
            rdna3_mode m;
            int k;
            CHECK(edid_parse(S.edid, 256, &e) == 0);
            k = dcn32_match_head_mode(&dc.head[0], 241500, &e);
            CHECK(k >= 0 && e.mode[k].h_active == 2560 && e.mode[k].preferred);
            CHECK(dcn32_match_head_mode(&dc.head[0], 148500, &e) == -1);   /* wrong clock */
            CHECK(dcn32_match_head_mode(&dc.head[0], 0, &e) == k);
            CHECK(dcn32_otg_timing(&e.mode[k], 1, &t) == RDNA3_OK);
            CHECK(t.h_total == S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] &&
                  (t.h_blank_start | (t.h_blank_end << 16)) == S.reg[OTG(regOTG0_OTG_H_BLANK_START_END, 2)] &&
                  (t.h_sync_start | (t.h_sync_end << 16)) == S.reg[OTG(regOTG0_OTG_H_SYNC_A, 2)] &&
                  t.v_total == S.reg[OTG(regOTG0_OTG_V_TOTAL, 2)] &&
                  (t.v_blank_start | (t.v_blank_end << 16)) == S.reg[OTG(regOTG0_OTG_V_BLANK_START_END, 2)] &&
                  (t.v_sync_start | (t.v_sync_end << 16)) == S.reg[OTG(regOTG0_OTG_V_SYNC_A, 2)]);
            CHECK(t.start_point == 1 && t.h_sync_pol == 0 && t.v_sync_pol == 0 && !t.interlace);
            /* front porch workaround: a zero vertical front porch becomes 1 */
            m = e.mode[k];
            m.v_front = 0;
            CHECK(dcn32_otg_timing(&m, 1, &t) == RDNA3_OK && t.v_blank_start == m.v_total - 1);
            /* 241.5 MHz x 24 bpp = 5.796 Gbps */
            CHECK(dcn32_dp_link_kbps(0x1e, 4, 0) == 25920000ull && dcn32_dp_link_kbps(0x0a, 1, 0) == 2160000ull);
            CHECK(dcn32_mode_kbps(&e.mode[k], 24) == 5796000ull);
            CHECK(dcn32_mode_check(&e.mode[k], 0x0a, 1, 0) == RD_MODE_LINK_BW);
            CHECK(dcn32_mode_check(&e.mode[k], 0x14, 2, 1) == RD_MODE_OK);
            CHECK(dcn32_mode_check(&e.mode[k], 0, 0, 0) == RD_MODE_OK);   /* no link known: no bw test */
            m = e.mode[k];
            m.interlaced = 1;
            CHECK(dcn32_mode_check(&m, 0x1e, 4, 1) == RD_MODE_INTERLACED);
            m = e.mode[k];
            m.pixel_khz = RD_DCN32_MAX_PCLK_KHZ + 1;
            CHECK(dcn32_mode_check(&m, 0x1e, 4, 1) == RD_MODE_PIXEL_CLOCK);
            m.pixel_khz = 0;   /* a standard timing: size and rate only */
            CHECK(dcn32_mode_check(&m, 0x1e, 4, 1) == RD_MODE_NO_TIMING &&
                  dcn32_otg_timing(&m, 1, &t) == RDNA3_EINVAL);
        }

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
        /* the report's screen snapshot: what pipe 1 scans (EARLIEST_INUSE);
         * its 2624-pitch surface at +96 MiB is past the 64 MiB BAR model */
        {
            rdna3_scanout_info si;
            uint8_t rgb[320 * 4 * 3], save[64 * 32 * 4];
            volatile uint32_t *px = (volatile uint32_t *)(void *)(S.vram + 0x3000000);
            CHECK(dcn32_scanout_info(&dev, 1, &si) == RDNA3_OK && si.powered && si.lit && si.otg == 2);
            CHECK(si.inuse == FB_START + 0x6000000 && si.pitch_px == 2624 && si.width == 2560 &&
                  si.height == 1440 && !si.flip_pending && si.format == RD_DCN_FMT_ARGB8888);
            CHECK(dcn32_scanout_sample(&dev, &si, 320, 180, 0, 4, rgb) == RDNA3_EINVAL);
            /* a small one inside the BAR: ARGB8888 -> RGB, nearest pixel */
            memcpy(save, (const void *)px, sizeof(save));
            for (uint32_t y = 0; y < 32; y++)
                for (uint32_t x = 0; x < 64; x++)
                    px[y * 64 + x] = 0xff000042u | x << 16 | y << 8;
            si.inuse = FB_START + 0x3000000;
            si.width = si.pitch_px = 64;
            si.height = 32;
            CHECK(dcn32_scanout_sample(&dev, &si, 16, 8, 4, 4, rgb) == RDNA3_OK);
            /* out (3, 5) <- surface (12, 20) */
            CHECK(rgb[(1 * 16 + 3) * 3] == 12 && rgb[(1 * 16 + 3) * 3 + 1] == 20 &&
                  rgb[(1 * 16 + 3) * 3 + 2] == 0x42);
            si.format = RD_DCN_FMT_ABGR8888;
            CHECK(dcn32_scanout_sample(&dev, &si, 16, 8, 4, 4, rgb) == RDNA3_OK &&
                  rgb[(1 * 16 + 3) * 3] == 0x42 && rgb[(1 * 16 + 3) * 3 + 2] == 12);
            CHECK(dcn32_scanout_sample(&dev, &si, 16, 8, 6, 4, rgb) == RDNA3_EINVAL);   /* past out_h */
            si.sw_mode = 1;
            CHECK(dcn32_scanout_sample(&dev, &si, 16, 8, 0, 4, rgb) == RDNA3_EINVAL);   /* tiled */
            memcpy((void *)px, save, sizeof(save));
            CHECK(dcn32_scanout_info(&dev, 0, &si) == RDNA3_OK && !si.lit);   /* blanked */
            CHECK(dcn32_scanout_info(&dev, 4, &si) == RDNA3_EINVAL);
            /* display sleep: hubp2_set_blank_regs on head 0 = pipe 1 */
            {
                uint32_t c0 = S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)];
                S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] |= HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK;
                CHECK(dcn32_set_blank(&dev, &dc, 0, 1) == RDNA3_OK);
                CHECK((S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & (HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK |
                       HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK)) ==
                      (HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK | HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK));
                CHECK(dcn32_scanout_info(&dev, 1, &si) == RDNA3_OK && si.blanked && !si.lit);
                CHECK(dcn32_set_blank(&dev, &dc, 0, 0) == RDNA3_OK);
                CHECK(dcn32_scanout_info(&dev, 1, &si) == RDNA3_OK && !si.blanked && si.lit);
                CHECK((S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK) ==
                      (c0 & HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK));
                CHECK(dcn32_set_blank(&dev, &dc, 3, 1) == RDNA3_EINVAL);
                S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] = c0;
            }
        }
        /* scaled source on head 0 (pipe 1): DPP scaler + HUBP viewport under
         * the OTG lock, bypass state back for the native size */
        {
            static rdna3_dcn_scale sc;
            const uint32_t dpp = regCNVC_CUR1_CURSOR0_CONTROL - regCNVC_CUR0_CURSOR0_CONTROL;
#define DSC(r) S.reg[DCN_B2 + (r) + dpp]
            const uint32_t mode0 = 0x00000100u, vp0 = S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, 1)];
            const uint32_t pitch0 = S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, 1)];
            memset(&sc, 0, sizeof(sc));
            DSC(regDSCL0_SCL_MODE) = mode0;
            DSC(regDSCL0_SCL_TAP_CONTROL) = 0;
            DSC(regDSCL0_MPC_SIZE) = 2560u | 1440u << 16;
            CHECK(dcn32_set_source(&dev, &dc, &sc, 0, FB_START + 0x2000000, 3000, 1440, 3000) == RDNA3_EINVAL);
            CHECK(dcn32_set_source(&dev, &dc, &sc, 0, FB_START + 0x2000000, 600, 1440, 600) == RDNA3_EINVAL);
            CHECK(dcn32_set_source(&dev, &dc, &sc, 0, FB_START + 0x2000010, 1280, 720, 1280) == RDNA3_EINVAL);
            CHECK(dcn32_set_source(&dev, &dc, &sc, 0, FB_START + 0x2000000, 1280, 720, 1280) == RDNA3_OK);
            CHECK((DSC(regDSCL0_SCL_MODE) & 7) == 1 && (DSC(regDSCL0_SCL_MODE) & ~7u) == (mode0 & ~7u));
            CHECK(DSC(regDSCL0_SCL_HORZ_FILTER_SCALE_RATIO) == 0x800000u &&
                  DSC(regDSCL0_SCL_VERT_FILTER_SCALE_RATIO) == 0x800000u);
            CHECK(DSC(regDSCL0_SCL_HORZ_FILTER_INIT) == 0x01C00000u &&      /* (0.5 + 3) / 2 = 1.75 */
                  DSC(regDSCL0_SCL_VERT_FILTER_INIT_BOT) == 0x02400000u);  /* + ratio = 2.25 */
            CHECK(DSC(regDSCL0_SCL_TAP_CONTROL) == 0x1111u && (DSC(regDSCL0_DSCL_2TAP_CONTROL) & 0x10001u) == 0x10001u);
            CHECK(DSC(regDSCL0_RECOUT_SIZE) == (2560u | 1440u << 16) && DSC(regDSCL0_MPC_SIZE) == (2560u | 1440u << 16));
            CHECK(S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, 1)] == (1280u | 720u << 16));
            CHECK((S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, 1)] & 0x3fff) == 1279);
            CHECK(S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 1)] == (uint32_t)(FB_START + 0x2000000));
            CHECK(!(S.reg[OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, 2)] & 1) &&
                  ((S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL2, 2)] >> 25) & 7) == 2);   /* locked OTG 2, released */
            CHECK(dc.head[0].width == 1280 && dc.head[0].height == 720 && dc.head[0].pitch_px == 1280);
            /* flips keep working at the scaled size */
            CHECK(dcn32_flip(&dev, &dc, 0, FB_START + 0x2100000, 1280, 1) == RDNA3_OK);
            /* another scaled size reuses the saved native state */
            CHECK(dcn32_set_source(&dev, &dc, &sc, 0, FB_START + 0x2000000, 1920, 1080, 1920) == RDNA3_OK);
            CHECK(DSC(regDSCL0_SCL_HORZ_FILTER_SCALE_RATIO) == 0xC00000u);   /* 0.75 */
            /* native: everything as the GOP left it */
            CHECK(dcn32_set_source(&dev, &dc, &sc, 0, FB_START + 0x6000000, 2560, 1440, 2624) == RDNA3_OK);
            CHECK(DSC(regDSCL0_SCL_MODE) == mode0 && DSC(regDSCL0_SCL_TAP_CONTROL) == 0 &&
                  DSC(regDSCL0_MPC_SIZE) == (2560u | 1440u << 16));
            CHECK(S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, 1)] == vp0);
            CHECK((S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, 1)] & 0x3fff) == (pitch0 & 0x3fff) && !sc.saved[1]);
            CHECK(dc.head[0].width == 2560 && dc.head[0].height == 1440);
            /* the OTG CRC over the active area, as dc_stream_configure_crc */
            dcn32_crc_enable(&dev, &dc, 0, 1);
            CHECK(S.reg[OTG(regOTG0_OTG_CRC0_WINDOWA_X_CONTROL, 2)] == 2560u << 16 &&
                  S.reg[OTG(regOTG0_OTG_CRC0_WINDOWB_Y_CONTROL, 2)] == 1440u << 16);
            CHECK(S.reg[OTG(regOTG0_OTG_CRC_CNTL, 2)] & OTG0_OTG_CRC_CNTL__OTG_CRC_EN_MASK);
            dcn32_crc_enable(&dev, &dc, 0, 0);
#undef DSC
        }
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
            if (m->h_active == 640 && m->v_active == 480) {
                got480++;   /* established: the DMT timing */
                CHECK(m->pixel_khz == 25175 && m->h_total == 800 && m->v_total == 525 &&
                      m->origin == RD_MODE_FROM_DMT && !m->h_sync_pos);
            }
            if (m->h_active == 800 && m->v_active == 600)
                CHECK(m->pixel_khz == 40000 && m->h_front == 40 && m->h_sync == 128 && m->v_total == 628);
            if (m->h_active == 1280 && m->v_active == 720)
                CHECK(m->origin == RD_MODE_FROM_CTA && m->pixel_khz == 74250 && m->h_total == 1650);
            if (m->h_active == 3840)
                CHECK(m->origin == RD_MODE_FROM_CTA && m->pixel_khz == 594000 && m->v_total == 2250 &&
                      m->refresh_mhz == 60000);
            CHECK(m->pixel_khz != 0);   /* every mode has a timing now */
        }
        CHECK(got1440 == 1 && got1080 == 1 && got480 == 1 && e.nmodes >= 6);
        {
            /* drm_cvt_mode reduced blanking: 1920x1200R and 2560x1440R */
            rdna3_mode c;
            CHECK(edid_cvt_rb(1920, 1200, 60, &c) == 0 && c.pixel_khz == 154000 && c.h_total == 2080 &&
                  c.v_total == 1235 && c.h_front == 48 && c.h_sync == 32 && c.v_front == 3 && c.v_sync == 6 &&
                  c.h_sync_pos && !c.v_sync_pos);
            CHECK(edid_cvt_rb(2560, 1440, 60, &c) == 0 && c.pixel_khz == 241500 && c.v_total == 1481);
            CHECK(edid_dmt(1366, 768, 60, &c) == 0 && c.pixel_khz == 85500 && c.h_total == 1792);
            CHECK(edid_dmt(1366, 768, 75, &c) == -1);
        }
        CHECK(edid_parse(S.edid, 100, &e) == -1);
        S.edid[5] = 0;
        CHECK(edid_parse(S.edid, 128, &e) == -1);
        S.edid[5] = 0xff;
        /* the probe finds the one sink; its pad stays in AUX mode, the
         * others go back to I2C */
        CHECK(dcaux_probe(&dev, conn, RD_AUX_ENGINES) == 1);
        CHECK(S.reg[DCN_B2 + regDC_GPIO_DDC1_MASK] & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK);
        CHECK(!(S.reg[DCN_B2 + regDC_GPIO_DDC3_MASK] & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK));
        CHECK(S.aux_non_aux == 0);
        CHECK(((S.reg[AUXR(regDP_AUX0_AUX_CONTROL, 2)] & DP_AUX0_AUX_CONTROL__AUX_HPD_SEL_MASK) >>
               DP_AUX0_AUX_CONTROL__AUX_HPD_SEL__SHIFT) == 2);
        /* a TMDS sink (the card: HDMI, and a DP->HDMI adaptor on DP-3) on
         * engine 2: AUX says NON_AUX_MODE, the EDID comes over the DDC
         * through the HW I2C engine, its pad back in I2C mode */
        S.i2c_sink = 2;
        S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] = HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK;
        memset(edid, 0, sizeof(edid));
        CHECK(dcddc_read_edid(&dev, 2, edid, sizeof(edid)) == 256 && memcmp(edid, S.edid, 256) == 0);
        CHECK(dcddc_read_edid(&dev, 1, edid, sizeof(edid)) == RD_AUX_ERR_NACK);
        CHECK(dcaux_probe(&dev, conn, RD_AUX_ENGINES) == 2);
        CHECK(conn[1].aux == 2 && conn[1].hpd && !conn[1].dp && conn[1].edid_ddc && conn[1].edid_len == 256);
        CHECK(memcmp(conn[1].edid, S.edid, 256) == 0 && conn[0].dp && !conn[0].edid_ddc);
        CHECK(!(S.reg[DCN_B2 + regDC_GPIO_DDC3_MASK] & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK));
        CHECK(((S.reg[DCN_B2 + regDC_I2C_ARBITRATION] & DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK)
               >> DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS__SHIFT) == 0);   /* released */
        CHECK(S.i2c_gos >= 4);
        S.i2c_sink = -1;
        S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] = 0;
        CHECK(conn[0].aux == 0 && conn[0].hpd && conn[0].dp && conn[0].dpcd_rev == 0x14 &&
              conn[0].max_link_rate == 0x1e && conn[0].max_lane_count == 4 && conn[0].edid_len == 256);
        CHECK(S.aux_bad == 0);
    }

    /* GPU timestamp: a carry between the two reads must not tear it */
    S.tsc = 0xFFFFFFF4ull;
    CHECK(rdna3_gpu_clock_counter(&dev) == 0x100000004ull);
    S.tsc = 0x500000000ull;
    CHECK(rdna3_gpu_clock_counter(&dev) == 0x500000008ull);

    /* chip revision from the NBIO strap, not the PCI revision */
    S.reg[NBIO_B2 + regRCC_STRAP0_RCC_DEV0_EPF0_STRAP0] = 0x02000000u | 0x1234u;
    CHECK(rdna3_nbio_rev_id(&dev) == 2);
    S.reg[NBIO_B2 + regRCC_STRAP0_RCC_DEV0_EPF0_STRAP0] = 0;
    CHECK(rdna3_nbio_rev_id(&dev) == 0);

    /* harvesting: one SA, one RB and one WGP fused off */
    {
        rdna3_gfx_cu_info ci;
        memset(&ci, 0, sizeof(ci));
        ci.max_se = 3; ci.max_sa_per_se = 2; ci.max_cu_per_sa = 10; ci.max_backends_per_se = 4;
        S.reg[GCR(regCC_GC_SA_UNIT_DISABLE)] = (1u << 5) << CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT;  /* SE2 SA1 */
        S.reg[GCR(regCC_RB_BACKEND_DISABLE)] = 1u << CC_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;   /* RB0 */
        S.harvest_wgp[0][1] = 1u << 4;                                                              /* SE0 SA1 WGP4 */
        CHECK(gfx11_get_cu_info(&dev, &ci) == RDNA3_OK);
        CHECK(ci.sa_active == 0x1f);
        CHECK(ci.rb_active == (0x3ffu & ~1u) && ci.num_rbs == 9);
        CHECK(ci.cu_active == 48 && ci.cu_bitmap[0][0] == 0x3ff && ci.cu_bitmap[0][1] == 0xff &&
              ci.cu_bitmap[2][1] == 0 && ci.cu_bitmap[2][0] == 0x3ff);
        CHECK(S.reg[GCR(regGRBM_GFX_INDEX)] == (GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK |
                                               GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK |
                                               GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK));
        ci.max_sa_per_se = 3;
        CHECK(gfx11_get_cu_info(&dev, &ci) == RDNA3_EINVAL);
        S.reg[GCR(regCC_GC_SA_UNIT_DISABLE)] = 0;
        S.reg[GCR(regCC_RB_BACKEND_DISABLE)] = 0;
        S.harvest_wgp[0][1] = 0;
    }

    /* AtomBIOS: the interpreter on the bytecode above, and the display
     * object table (connector -> UNIPHY, DDC line = AUX engine, HPD pin) */
    {
        static rdna3_vbios vb;
        static rdna3_atom at;
        static uint32_t scratch[64];
        rdna3_display_path dp[RD_VBIOS_MAX_PATHS];
        uint32_t ps[8] = { 0 };
        uint8_t fr = 0, cr = 0;
        uint64_t t0;
        CHECK(vbios_parse(&dev, &vb, S.vbios, VBIOS_LEN) == RDNA3_OK);
        {
            rdna3_vbios_strings vs;
            vbios_strings(&vb, &vs);
            CHECK(!strcmp(vs.name, "SIM RX 7800 XT") && !strcmp(vs.pn, "113-SIMPN-001"));
            CHECK(!strcmp(vs.date, "2026/10/09 12:34") && !strncmp(vs.ver_str, "022.001.002.008.000001", 22));
            CHECK(vs.version != 0);   /* atom_firmware_info revision */
        }
        CHECK(atom_init(&at, &dev, &vb, scratch, sizeof(scratch)) == RDNA3_OK);
        CHECK(at.cmd_table == VB_CMD_DIR && at.iio[0x81] == VB_IIO + 6);
        CHECK(atom_cmd_revision(&at, RD_ATOM_CMD_SET_PIXEL_CLOCK, &fr, &cr) && fr == 1 && cr == 1);
        CHECK(!atom_cmd_revision(&at, RD_ATOM_CMD_DIG1_TRANSMITTER_CONTROL, NULL, NULL));
        /* amdgpu_device_need_post -> amdgpu_device_asic_init (passthrough
         * VM: OVMF did not run the option ROM's init) */
        S.reg[SIM_BIOS_SCRATCH + 7] = 0;
        CHECK(atom_need_post(&at) == 1);
        CHECK(atom_asic_init(&at) == RDNA3_OK && S.reg[ATOM_REG_A] == 50000);
        CHECK(atom_need_post(&at) == 0);
        S.vbios[VB_CMD_CODE + 2] = 1;                 /* ASIC_Init v1.1: not atomfirmware's */
        CHECK(atom_asic_init(&at) == RDNA3_EINVAL);
        S.vbios[VB_CMD_CODE + 2] = 2;
        ps[0] = 100;
        ps[2] = 21;
        t0 = S.now_us;
        CHECK(atom_execute(&at, RD_ATOM_CMD_SET_PIXEL_CLOCK, ps, 8) == RDNA3_OK);
        CHECK(S.reg[ATOM_REG_A] == 116);              /* move, add, reg write */
        CHECK(S.reg[ATOM_REG_B] == 0x00012340);       /* mask, shift, xor */
        CHECK(ps[2] == 42);                           /* loop + switch case 5 + nested table */
        CHECK(ps[1] == 50000);                        /* data block + ID operand */
        CHECK(ps[3] == 0xABCD && scratch[5] == 0xABCD);   /* FB window */
        CHECK(S.reg[ATOM_REG_C] == 50000);            /* indirect IO write method */
        CHECK(ps[0] == 116 / 7);                      /* div -> quotient */
        CHECK(S.now_us - t0 >= 10 && at.tables_run == 3);
        CHECK(atom_execute(&at, RD_ATOM_CMD_ENABLE_DISP_POWER_GATING, ps, 0) == RDNA3_EINVAL);
        CHECK(atom_execute(&at, RD_ATOM_CMD_BLANK_CRTC, ps, 0) == RDNA3_EINVAL);
        CHECK(atom_execute(&at, RD_ATOM_CMD_ENABLE_CRTC, ps, 0) == RDNA3_EINVAL && at.depth == 0);
        CHECK(atom_execute(&at, RD_ATOM_CMD_SELECT_CRTC_SOURCE, ps, 0) == RDNA3_OK && at.unknown_ops == 1);
        CHECK(atom_execute(&at, RD_ATOM_CMD_DIG1_TRANSMITTER_CONTROL, ps, 0) == RDNA3_ENOENT);
        CHECK(at.ws_top == 0);
        /* still usable after the aborts */
        ps[0] = 100; ps[2] = 1;
        CHECK(atom_execute(&at, RD_ATOM_CMD_SET_PIXEL_CLOCK, ps, 8) == RDNA3_OK && ps[2] == 2);

        CHECK(vbios_display_paths(&vb, dp, RD_VBIOS_MAX_PATHS) == 2);
        CHECK(dp[0].connector == RD_ATOM_CONN_DISPLAYPORT && dp[0].connector_enum == 1 &&
              dp[0].transmitter == 0 && dp[0].ddc_line == 0 && dp[0].hpd == 0 &&
              dp[0].hpd_active == 1 && dp[0].max_link_mhz == 8100 && dp[0].device_tag == 0x0008 &&
              dp[0].encoder_caps == (RD_ATOM_ENC_CAP_MST | RD_ATOM_ENC_CAP_HBR2 | RD_ATOM_ENC_CAP_HBR3));
        CHECK(dp[1].connector == RD_ATOM_CONN_HDMI_A && dp[1].transmitter == 1 &&
              dp[1].ddc_line == 1 && dp[1].hpd == 1 && dp[1].encoder_caps == 0);
        CHECK(strcmp(vbios_connector_name(dp[1].connector), "HDMI") == 0);
        CHECK(vbios_display_paths(&vb, dp, 1) == 1);
    }

    /* DMCUB: stop the firmware the VBIOS left running, lay out the windows,
     * boot it again on the PSP-loaded image, talk to it through inbox1 */
    {
        static rdna3_dmub dm;
        rdna3_dig_tx_v1_7 tx;
        uint32_t k;
        uint64_t cw;
        CHECK(dmub32_is_supported(&dev) && dmub32_is_hw_init(&dev));
        CHECK(dmub32_sw_init(&dm, &dev, blobs[AMD_BLOB_DMCUB].data, (uint32_t)blobs[AMD_BLOB_DMCUB].len,
                             VBIOS_LEN) == RDNA3_OK);
        CHECK(dm.meta.found && dm.meta.dal_fw == 1);
        if (fwdir)          /* whatever linux-firmware ships today: sane, not pinned */
            CHECK(dm.fw_version != 0 && dm.meta.trace_buffer_size > 0 &&
                  dm.meta.fw_region_size > 0 && dm.inst_const_size > 0);
        else
            CHECK(dm.fw_version == 0x07001234u && dm.inst_const_size == 1024 - 256 - 256 &&
                  dm.meta.fw_region_size == 8192);
        for (k = 0; k < RD_DMUB_WIN_TOTAL; k++)
            CHECK((dm.region[k].base & 255) == 0 && dm.region[k].top >= dm.region[k].base &&
                  (k == 0 || dm.region[k].base >= dm.region[k - 1].top));
        CHECK(dm.region[RD_DMUB_WIN_MAILBOX].top - dm.region[RD_DMUB_WIN_MAILBOX].base == 2 * RD_DMUB_RB_SIZE);
        CHECK(S.dmub.psp_loaded);
        CHECK(dmub32_hw_init(&dm, S.vbios, VBIOS_LEN, 1, 100000) == RDNA3_OK);
        CHECK(dmub32_wait_boot(&dm, 1000) == RDNA3_OK && dm.hw_init);
        dmub32_log_state(&dm, "test");
        CHECK(S.dmub.stops == 1 && S.dmub.boots == 1 && S.dmub.bad_boots == 0 && S.dmub.running);
        CHECK((dm.boot_status & 3) == 3 && dmub32_is_hw_init(&dev));
        cw = dmub_cw_offset(3);
        CHECK(cw == rdna3_vram_mc(&dev, dm.fb_off + dm.region[RD_DMUB_WIN_VBIOS].base));
        CHECK(S.reg[DMR(regDMCUB_REGION3_CW3_BASE_ADDRESS)] == RD_DMUB_CW3_BASE &&
              S.reg[DMR(regDMCUB_OUTBOX0_BASE_ADDRESS)] == RD_DMUB_REGION5_BASE + 16 &&
              S.reg[DMR(regDMCUB_OUTBOX1_BASE_ADDRESS)] == RD_DMUB_CW4_BASE + RD_DMUB_RB_SIZE &&
              S.reg[DMR(regDMCUB_SCRATCH7)] == RD_DMUB_GPINT_STOP_FW_RESPONSE &&
              S.reg[DMR(regDMCUB_GPINT_DATAIN1)] == 0);
        /* the mailbox: feature caps with a reply, then a VBIOS command */
        CHECK(dmub32_query_caps(&dm) == RDNA3_OK && (dm.feature_caps[0] & 0xffff) == 0x0201);
        memset(&tx, 0, sizeof(tx));
        tx.phyid = 0; tx.action = RD_ATOM_TX_ENABLE; tx.digmode = 0; tx.lanenum = 4;
        tx.symclk_10khz = 81000; tx.hpdsel = 1; tx.digfe_sel = 1; tx.connobj_id = RD_ATOM_CONN_DISPLAYPORT;
        CHECK(dmub32_transmitter_control(&dm, &tx) == RDNA3_OK);
        CHECK(S.dmub.tx_cmds == 1 && S.dmub.tx[1] == RD_ATOM_TX_ENABLE && S.dmub.tx[3] == 4 &&
              S.dmub.tx[4] == (81000 & 0xff) && S.dmub.tx[8] == 1 && S.dmub.tx[10] == 0x13);
        /* the ring wraps */
        for (k = 0; k < 2 * RD_DMUB_RB_MAX_ENTRY; k++)
            if (dmub32_query_caps(&dm) != RDNA3_OK)
                break;
        CHECK(k == 2 * RD_DMUB_RB_MAX_ENTRY && S.dmub.bad_cmds == 0 &&
              dm.submitted == dm.reported && dm.submitted == 2 + 2 * RD_DMUB_RB_MAX_ENTRY);
        /* a firmware that stops answering */
        S.dmub.hang = 1;
        CHECK(dmub32_query_caps(&dm) == RDNA3_ETIME);
        S.dmub.hang = 0;
        /* back in business after a re-init, this time with a backdoor load */
        CHECK(dmub32_hw_init(&dm, S.vbios, VBIOS_LEN, 0, 100000) == RDNA3_OK);
        CHECK(S.dmub.stops == 2 && S.dmub.boots == 2);
        cw = dmub_cw_offset(0);
        CHECK(cw == dm.fb_off + dm.region[RD_DMUB_WIN_INST_CONST].base);   /* FB physical, offset 0 */
        CHECK(memcmp(S.vram + dm.fb_off + dm.region[RD_DMUB_WIN_INST_CONST].base, dm.fw_inst_const, 64) == 0);
        CHECK(dmub32_query_caps(&dm) == RDNA3_OK);

        /* second monitor: clone the GOP head (pipe 1 -> OTG 2 -> OPP 2,
         * DIGC -> UNIPHY B, HDMI) onto UNIPHY D. The free pipes are power
         * gated and own no DET; the PHY side goes through the DMCUB. */
        {
            static rdna3_dcn dc2;
            static rdna3_dcn_clone cl;
            rdna3_display_path path;
            uint32_t k2, n;
            memset(&dc2, 0, sizeof(dc2));
            S.dcn_pg = 1;
            for (n = 0; n < RD_DCN_MAX_PIPES; n++) {
                int on = n == 1;
                S.reg[DCN_B2 + regDOMAIN0_PG_CONFIG + 2 * n] = on ? 0 : DOMAIN0_PG_CONFIG__DOMAIN_POWER_GATE_MASK;
                S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2 * n] = on ? 0 : 2u << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT;
                S.reg[DCN_B3 + regMPCC0_MPCC_TOP_SEL + RD_CLONE_MPCC_STRIDE * n] = on ? 1 : 0xf;
                S.reg[DCN_B3 + regMPCC0_MPCC_BOT_SEL + RD_CLONE_MPCC_STRIDE * n] = 0xf;
                S.reg[DCN_B3 + regMPCC0_MPCC_OPP_ID + RD_CLONE_MPCC_STRIDE * n] = on ? 2 : 0xf;
                S.reg[DCN_B3 + regMPC_OUT0_MUX + (regMPC_OUT1_MUX - regMPC_OUT0_MUX) * n] = n == 2 ? 1 : 0xf;
                S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + n] = on ? (8u | 8u << 8) : 0;
            }
            S.reg[DCN_B2 + regODM0_OPTC_DATA_SOURCE_SELECT + RD_CLONE_ODM_STRIDE * 2] =
                2u << ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL__SHIFT;
            /* values to carry over: every block gets a marker */
            for (k2 = 0; k2 < sizeof(rd_clone_hubp) / 2; k2++)
                S.reg[DCN_B2 + rd_clone_hubp[k2] + RD_CLONE_HUBP_STRIDE] = 0x1000 + k2;
            for (k2 = 0; k2 < sizeof(rd_clone_dpp) / 2; k2++)
                S.reg[DCN_B2 + rd_clone_dpp[k2] + RD_CLONE_DPP_STRIDE] = 0x2000 + k2;
            S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL + RD_CLONE_DPP_STRIDE] = 1;   /* LUT RAM in use */
            S.reg[DCN_B2 + regHUBP0_DCHUBP_CNTL + RD_CLONE_HUBP_STRIDE] =
                (2u << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT) | HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK;
            S.reg[DCN_B2 + 0x2094u + RD_CLONE_DIGFE_STRIDE * 2] = 0x11;
            S.reg[DCN_B2 + regDIG0_HDMI_GENERIC_PACKET_CONTROL0 + RD_CLONE_DIGFE_STRIDE * 2] = 0x33;
            S.reg[DCN_B2 + regDIG0_DIG_FE_CNTL + RD_CLONE_DIGFE_STRIDE * 2] =
                2u | DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
            S.reg[DCN_B2 + regDIG0_TMDS_CTL_BITS + RD_CLONE_DIGFE_STRIDE * 1] = 0x44;
            S.reg[DCN_B1 + regDPPCLK0_DTO_PARAM + 1] = 0x00640032;
            S.reg[DCN_B2 + regVTG0_CONTROL + 2] = 0x80050003u;   /* GOP's VTG 2: FP2 3, VCOUNT_INIT 5, on */
            S.reg[DCN_B2 + regVTG0_CONTROL + 1] = 0;
            S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] = 3u << 4;
            S.reg[DCN_B1 + regOTG_PIXEL_RATE_DIV] = 5u << 6;
            S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * 2] = OTG0_PIXEL_RATE_CNTL__DPDTO0_ENABLE_STATUS_MASK;
            /* the GOP's display clocks: VCO 43 x 100 MHz; DISPCLK, DPPCLK and
             * DPREFCLK /6 (DID 0x18), DCFCLK /4 (0x10), DTBCLK /9 (0x24) */
            S.reg[RD_CLK1_CLK_PLL_REQ] = 43;
            S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x18;
            S.reg[RD_CLK1_CLK0_DFS_CNTL + 3] = 0x18;
            S.reg[RD_CLK1_CLK0_DFS_CNTL + 6] = 0x18;
            S.reg[RD_CLK1_CLK0_DFS_CNTL + 9] = 0x10;
            S.reg[RD_CLK1_CLK0_DFS_CNTL + 12] = 0x24;
            S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;   /* the PMFW's idle state */

            /* the ATOM tests above ran the VBIOS CRTC tables: light OTG 2
             * again (DIG route as the DCN test set it) */
            S.reg[OTG(regOTG0_OTG_CONTROL, 2)] = 1;
            CHECK(dcn32_read_state(&dev, &dc2) == 1 && dc2.head[0].dig_mode == 3);
            memset(&path, 0, sizeof(path));
            path.transmitter = 3;           /* UNIPHY D: encoder UNIPHY1 enum 2 */
            path.hpd = 2;
            path.encoder_objid = 0x2120;
            path.connector_objid = 0x310c;  /* HDMI type A */
            /* not lit: no such head, no clock, DP source refused */
            CHECK(dcn32_clone_plan(&dev, &dc2, 1, &path, 148500, &cl) == RDNA3_EINVAL);
            CHECK(dcn32_clone_plan(&dev, &dc2, 0, &path, 0, &cl) == RDNA3_EINVAL);
            dc2.head[0].dig_mode = 0;
            CHECK(dcn32_clone_plan(&dev, &dc2, 0, &path, 148500, &cl) == RDNA3_EINVAL);
            dc2.head[0].dig_mode = 3;
            path.transmitter = 1;           /* the GOP's own PHY */
            CHECK(dcn32_clone_plan(&dev, &dc2, 0, &path, 148500, &cl) == RDNA3_EBUSY);
            path.transmitter = 3;
            /* DISPCLK at /128 (33.6 MHz) cannot carry 148.5 MHz */
            S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x7f;
            CHECK(dcn32_clone_plan(&dev, &dc2, 0, &path, 148500, &cl) == RDNA3_EBUSY);
            S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x18;
            CHECK(dcn32_clone_plan(&dev, &dc2, 0, &path, 148500, &cl) == RDNA3_OK);
            CHECK(cl.clk.vco_khz == 4300000 && cl.clk.dispclk_khz == 716666 && cl.clk.dppclk_khz == 716666 &&
                  cl.clk.dprefclk_khz == 716666 && cl.clk.dcfclk_khz == 1075000 && cl.clk.dtbclk_khz == 477777 &&
                  cl.displays == 2);
            CHECK(cl.src_pipe == 1 && cl.src_otg == 2 && cl.src_opp == 2 && cl.src_mpcc == 1 &&
                  cl.src_fe == 2 && cl.src_be == 1);
            /* OTG 0 is taken: the stale DIGA back end still sources it */
            CHECK(cl.dst_pipe == 0 && cl.dst_otg == 1 && cl.dst_opp == 1 && cl.dst_mpcc == 0 &&
                  cl.dst_fe == 3 && cl.dst_be == 3 && cl.pll == 3);
            CHECK(cl.det_src_new == 4 && cl.det_dst_new == 4);
            /* no DMCUB, no picture */
            CHECK(dcn32_clone_enable(&dev, NULL, &cl) == RDNA3_ENODEV);
            S.dmub.nseq = 0;
            S.dal_displays = 0;
            cl.smu_probe = 1;           /* the PMFW keeps answering: 5 TestMessages */
            cl.pstate_force = 3;        /* arbiter: p-state and USR retraining allowed */
            S.reg[DCN_B2 + RD_DCHUBBUB_ARB_DRAM_STATE_CNTL] = 0x1;
            S.reg[DCN_B2 + RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL] = 0x1;   /* a retraining request pending */
            k2 = S.smu_msgs;
            CHECK(dcn32_clone_enable(&dev, &dm, &cl) == RDNA3_OK);
            CHECK(S.smu_msgs - k2 == 5);
            CHECK(S.reg[DCN_B2 + RD_DCHUBBUB_ARB_DRAM_STATE_CNTL] == (0x1 | RD_ARB_ALLOW_PSTATE_FORCE_VALUE | RD_ARB_ALLOW_PSTATE_FORCE_ENABLE) &&
                  S.reg[DCN_B2 + RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL] == (0x1 | RD_ARB_USR_FORCE_VALUE | RD_ARB_USR_FORCE_ENABLE));
            CHECK(S.dal_displays == 0);   /* no NumOfDisplays: it wedged the PMFW on the card */
            CHECK(S.dcn_gated_drops == 0 && S.dcn_pg_no_request == 0 && S.dmub.bad_cmds == 0);
            CHECK(S.reg[DCN_B2 + regDOMAIN0_PG_STATUS] == 0 &&
                  !(S.reg[DCN_B2 + regDC_IP_REQUEST_CNTL] & DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK));
            CHECK((S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] & 0x1f) == 4 &&
                  (S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL] & 0x1f) == 4);
            /* the copies */
            for (k2 = 0; k2 < sizeof(rd_clone_hubp) / 2; k2++)
                if (S.reg[DCN_B2 + rd_clone_hubp[k2]] != S.reg[DCN_B2 + rd_clone_hubp[k2] + RD_CLONE_HUBP_STRIDE])
                    break;
            CHECK(k2 == sizeof(rd_clone_hubp) / 2);
            for (k2 = 0; k2 < sizeof(rd_clone_otg) / 2; k2++)
                if (S.reg[DCN_B2 + rd_clone_otg[k2] + RD_CLONE_OTG_STRIDE] != S.reg[DCN_B2 + rd_clone_otg[k2] + RD_CLONE_OTG_STRIDE * 2])
                    break;
            CHECK(k2 == sizeof(rd_clone_otg) / 2);
            CHECK(S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL] == 0 &&
                  S.reg[DCN_B2 + rd_clone_dpp[0]] == 0x2000);
            CHECK(S.reg[DCN_B2 + regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS] ==
                  S.reg[DCN_B2 + regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS + RD_CLONE_HUBP_STRIDE]);
            /* routing: OTG 1 <- OPP 1 <- MPCC 0 <- DPP 0, DIGD sources OTG 1,
             * UNIPHY D takes DIGD in HDMI mode */
            CHECK(S.reg[DCN_B2 + regHUBP0_DCHUBP_CNTL] ==
                  (HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK | 1u << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT));
            CHECK((S.reg[OTG(regOTG0_OTG_CONTROL, 1)] & 1) && ((S.reg[OTG(regOTG0_OTG_CONTROL, 1)] >> 8) & 3) == 2);
            CHECK(S.reg[DCN_B2 + regVTG0_CONTROL + 1] == 0x80050003u);   /* VTG programmed and enabled */
            CHECK(((S.reg[DCN_B2 + regODM0_OPTC_DATA_SOURCE_SELECT + RD_CLONE_ODM_STRIDE] >> 16) & 0xf) == 1);
            CHECK(S.reg[DCN_B3 + regMPCC0_MPCC_TOP_SEL] == 0 && S.reg[DCN_B3 + regMPCC0_MPCC_OPP_ID] == 1 &&
                  S.reg[DCN_B3 + regMPCC0_MPCC_BOT_SEL] == 0xf && (S.reg[DCN_B3 + regMPC_OUT1_MUX] & 0xf) == 0);
            CHECK(S.reg[DCN_B2 + regDIG0_DIG_FE_CNTL + RD_CLONE_DIGFE_STRIDE * 3] ==
                  (1u | DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK));
            CHECK(S.reg[DCN_B2 + 0x2094u + RD_CLONE_DIGFE_STRIDE * 3] == 0x11 &&
                  S.reg[DCN_B2 + regDIG0_HDMI_GENERIC_PACKET_CONTROL0 + RD_CLONE_DIGFE_STRIDE * 3] == 0);
            CHECK(S.reg[DCN_B2 + regDIG0_TMDS_CTL_BITS + RD_CLONE_DIGFE_STRIDE * 3] == 0x44);
            {
                uint32_t be = S.reg[DCN_B2 + regDIG0_DIG_BE_CNTL + RD_CLONE_DIGFE_STRIDE * 3];
                CHECK(((be >> 8) & 0x7f) == 1u << 3 && ((be >> 16) & 7) == 3);
            }
            /* DCCG */
            CHECK(S.reg[DCN_B1 + regDPPCLK0_DTO_PARAM] == 0x00640032 && (S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] & 3) == 3);
            CHECK(((S.reg[DCN_B1 + regOTG_PIXEL_RATE_DIV] >> 3) & 7) == 5 &&
                  S.reg[DCN_B1 + regOTG1_PIXEL_RATE_CNTL] == 0);   /* status bits not copied */
            /* the VBIOS commands: pixel clock, stream setup, PHY init, enable */
            CHECK(S.dmub.nseq == 4 && S.dmub.seq[0] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK &&
                  S.dmub.seq[1] == RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL &&
                  S.dmub.seq[2] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[2] == RD_ATOM_TX_INIT &&
                  S.dmub.seq[3] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[3] == RD_ATOM_TX_ENABLE);
            CHECK(S.dmub.pixclk[0] == (1485000 & 0xff) && S.dmub.pixclk[2] == (1485000 >> 16) &&
                  S.dmub.pixclk[4] == RD_ATOM_COMBOPHY_PLL0 + 3 && S.dmub.pixclk[5] == 0x20 &&
                  S.dmub.pixclk[6] == RD_ATOM_ENCODER_MODE_HDMI && S.dmub.pixclk[8] == 1);
            CHECK(S.dmub.enc[0] == 3 && S.dmub.enc[1] == RD_ATOM_ENCODER_CMD_STREAM_SETUP &&
                  S.dmub.enc[2] == RD_ATOM_ENCODER_MODE_HDMI && S.dmub.enc[3] == 4 &&
                  S.dmub.enc[4] == (14850 & 0xff) && S.dmub.enc[5] == (14850 >> 8) && S.dmub.enc[8] == 2);
            CHECK(S.dmub.tx[0] == 3 && S.dmub.tx[2] == 3 && S.dmub.tx[3] == 4 && S.dmub.tx[4] == (14850 & 0xff) &&
                  S.dmub.tx[8] == 3 && S.dmub.tx[9] == 1u << 3 && S.dmub.tx[10] == 0x0c);
            /* the core sees two heads now, the new one on UNIPHY D */
            CHECK(dcn32_read_state(&dev, &dc2) == 2 && dc2.head[0].pipe == 0 && dc2.head[0].otg == 1 &&
                  dc2.head[0].dig_fe == 3 && dc2.head[0].dig_be == 3 && dc2.head[0].dig_mode == 3 &&
                  dc2.head[0].h_total == dc2.head[1].h_total && dc2.head[0].address == dc2.head[1].address);
            /* the check: the clone OTG counts frames and shows the boot
             * head's picture (same CRC); a different surface differs */
            {
                rdna3_clone_check ck;
                uint32_t crc0 = S.reg[OTG(regOTG0_OTG_CRC_CNTL, 2)];
                S.dcn_frames = 1;
                CHECK(dcn32_clone_verify(&dev, &cl, 50, &ck) == RDNA3_OK && ck.running && ck.crc_match &&
                      ck.frames_dst == 4 && ck.frames_src == 4);
                CHECK(S.reg[OTG(regOTG0_OTG_CRC_CNTL, 2)] == crc0);   /* restored */
                S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 0)] ^= 0x100000;
                CHECK(dcn32_clone_verify(&dev, &cl, 50, &ck) == RDNA3_EIO && ck.running && !ck.crc_match);
                S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 0)] ^= 0x100000;
                S.reg[OTG(regOTG0_OTG_CONTROL, 1)] &= ~1u;            /* clone OTG stopped */
                CHECK(dcn32_clone_verify(&dev, &cl, 50, &ck) == RDNA3_EIO && !ck.running);
                S.reg[OTG(regOTG0_OTG_CONTROL, 1)] |= 1u;
                S.dcn_frames = 0;
            }
            /* extend (rdna3mirror=2): the clone head flips to a surface of
             * its own, the boot head keeps the GOP's */
            {
                uint32_t boot_lo = S.reg[DCN_B2 + regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS + RD_CLONE_HUBP_STRIDE];
                CHECK(dcn32_flip(&dev, &dc2, 0, FB_START + 0x7000000, dc2.head[0].pitch_px, 1) == RDNA3_OK);
                CHECK(S.reg[DCN_B2 + regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS] == (uint32_t)(FB_START + 0x7000000) &&
                      S.reg[DCN_B2 + regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS + RD_CLONE_HUBP_STRIDE] == boot_lo);
            }
            /* monitor standby on the clone: blank + TransmitterControl
             * DISABLE, back with ENABLE + unblank; nothing else touched */
            {
                uint32_t ctl = S.reg[DCN_B2 + regOTG0_OTG_CONTROL + RD_CLONE_OTG_STRIDE * cl.dst_otg];
                S.reg[DCN_B2 + regHUBP0_DCHUBP_CNTL + RD_CLONE_HUBP_STRIDE * cl.dst_pipe] |=
                    HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK;
                S.dmub.nseq = 0;
                CHECK(dcn32_clone_link(&dev, &dm, &cl, 0) == RDNA3_OK);
                CHECK(S.dmub.nseq == 1 && S.dmub.seq[0] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL &&
                      S.dmub.seq_tx_action[0] == RD_ATOM_TX_DISABLE);
                CHECK(S.reg[DCN_B2 + regHUBP0_DCHUBP_CNTL + RD_CLONE_HUBP_STRIDE * cl.dst_pipe] &
                      HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK);
                CHECK(dcn32_clone_link(&dev, &dm, &cl, 1) == RDNA3_OK);
                CHECK(S.dmub.nseq == 2 && S.dmub.seq_tx_action[1] == RD_ATOM_TX_ENABLE);
                CHECK(!(S.reg[DCN_B2 + regHUBP0_DCHUBP_CNTL + RD_CLONE_HUBP_STRIDE * cl.dst_pipe] &
                        HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK));
                CHECK(S.reg[DCN_B2 + regOTG0_OTG_CONTROL + RD_CLONE_OTG_STRIDE * cl.dst_otg] == ctl);
                CHECK(dcn32_clone_link(&dev, NULL, &cl, 0) == RDNA3_ENODEV);
            }
            /* the transmitter is lit: a second plan for it is refused */
            CHECK(dcn32_clone_plan(&dev, &dc2, 1, &path, 148500, &cl) == RDNA3_EBUSY);
            /* and off again */
            {
                static rdna3_dcn_clone off;
                off.src_pipe = 1; off.src_otg = 2; off.dst_pipe = 0; off.dst_otg = 1; off.dst_opp = 1;
                off.dst_mpcc = 0; off.dst_fe = 3; off.dst_be = 3; off.pll = 3; off.hpd = 2;
                off.connector_id = 0x0c; off.dig_mode = 3; off.pclk_khz = 148500;
                off.det_src = 8; off.det_dst = 0; off.det_src_new = 4; off.det_dst_new = 4;
                off.displays = 2;
                off.pstate_force = 3; off.arb_dram_state = 0x1; off.arb_usr_retraining = 0x1;
                S.dmub.nseq = 0;
                CHECK(dcn32_clone_disable(&dev, &dm, &off) == RDNA3_OK);
                CHECK(S.dal_displays == 0);
                CHECK(S.reg[DCN_B2 + RD_DCHUBBUB_ARB_DRAM_STATE_CNTL] == 0x1 &&
                      S.reg[DCN_B2 + RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL] == 0x1);   /* back */
            }
            CHECK(S.dmub.nseq == 2 && S.dmub.seq_tx_action[0] == RD_ATOM_TX_DISABLE &&
                  S.dmub.seq[1] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK && S.dmub.pixclk[0] == 0 && S.dmub.pixclk[2] == 0);
            CHECK(!(S.reg[DCN_B2 + regVTG0_CONTROL + 1] & VTG0_CONTROL__VTG0_ENABLE_MASK) &&
                  !(S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] & 3));
            CHECK(dcn32_read_state(&dev, &dc2) == 1 && dc2.head[0].pipe == 1);
            CHECK((S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] & 0x1f) == 8 &&
                  (S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL] & 0x1f) == 0 &&
                  S.reg[DCN_B2 + regDOMAIN0_PG_STATUS] == 2u << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT &&
                  S.reg[DCN_B3 + regMPCC0_MPCC_TOP_SEL] == 0xf);
            CHECK(S.dcn_gated_drops == 0 && S.dcn_pg_no_request == 0);   /* blanked before the gate */
            /* the PMFW stops answering after the third VBIOS command
             * (TransmitterControl INIT): the clone stops there, undone */
            CHECK(dcn32_clone_plan(&dev, &dc2, 0, &path, 148500, &cl) == RDNA3_OK);
            cl.smu_probe = 1;
            S.smu_wedge_at = 3;
            S.dmub.nseq = 0;
            CHECK(dcn32_clone_enable(&dev, &dm, &cl) == RDNA3_EIO);
            CHECK(S.dmub.nseq >= 4 && S.dmub.seq[2] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL &&
                  S.dmub.seq_tx_action[2] == RD_ATOM_TX_INIT && S.dmub.seq_tx_action[3] != RD_ATOM_TX_ENABLE);
            S.smu_wedge_at = 0;
            S.reg[MP1(90)] = 1;      /* the PMFW back (a reset in real life) */
            CHECK(dcn32_read_state(&dev, &dc2) == 1);
            /* dcn32_smu_set_hard_min_by_freq with the status wait */
            {
                uint32_t got = 0;
                CHECK(dcn32_smu_set_hard_min(&dev, RD_PPCLK_DCFCLK, 1200, 0x503900, &got) == RDNA3_OK &&
                      S.dal_hard_min[RD_PPCLK_DCFCLK] == 1200 && got == 1200000);
                CHECK(dcn32_smu_send(&dev, 0x7f, 0, NULL) == RDNA3_EIO);
                CHECK(dcn32_dentist_divider(0x08) == 8 && dcn32_dentist_divider(0x40) == 64 &&
                      dcn32_dentist_divider(0x60) == 128 && dcn32_dentist_divider(0x7f) == 512 &&
                      dcn32_dentist_divider(0) == 8);
            }
            /* a mode set on the lit HDMI head (pipe 1, OTG 2, DIGC -> UNIPHY B):
             * 2560x1440 at 241.5 MHz -> 1920x1080 at 148.5 MHz (DMT) */
            {
                static rdna3_dcn_modeset ms;
                static rdna3_dcn_scale sc;
                rdna3_display_path own;
                rdna3_mode m;
                uint32_t r_new, k3;
                memset(&own, 0, sizeof(own));
                own.transmitter = 1;
                own.hpd = 0;
                own.encoder_objid = 0x211e;
                own.connector_objid = 0x310c;
                /* the GOP's DLG/TTU and global sync for 1440p (DML values) */
                S.reg[HUBP(regHUBPREQ0_REF_FREQ_TO_PIX_FREQ, 1)] = 217093;          /* 100 / 241.5 x 2^19 */
                S.reg[HUBP(regHUBPREQ0_DST_DIMENSIONS, 1)] = 288192;                /* x 2720 x 2^8 */
                S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY, 1)] = 1060;
                S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY_PRE, 1)] = 1060;
                S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL0, 1)] = 0x10000 | (8u << 24);
                S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL1, 1)] = 0x10000;
                S.reg[HUBP(regHUBPREQ0_PREFETCH_SETTINGS, 1)] = (40u << 24) | (1u << 19);   /* 10 lines, vratio 1 */
                S.reg[HUBP(regHUBPREQ0_DCN_GLOBAL_TTU_CNTL, 1)] = 1000;
                S.reg[HUBP(regHUBPREQ0_DST_AFTER_SCALER, 1)] = 100 | (3u << 16);
                S.reg[OTG(regOTG0_OTG_VSTARTUP_PARAM, 2)] = 40;
                S.reg[OTG(regOTG0_OTG_VUPDATE_PARAM, 2)] = 680 | (30u << 16);
                S.reg[OTG(regOTG0_OTG_VREADY_PARAM, 2)] = 100;
                S.reg[DCN_B2 + regOPPBUF0_OPPBUF_CONTROL + RD_CLONE_OPP_STRIDE * 2] = 2560 | (1u << 16);
                CHECK(dcn32_read_state(&dev, &dc2) == 1 && dc2.head[0].h_active == 2560 && dc2.head[0].dig_be == 1);
                CHECK(edid_dmt(1920, 1080, 60, &m) == 0);
                /* refused: no timing, DP head, too fast for the clocks */
                {
                    rdna3_mode none = m;
                    none.pixel_khz = 0;
                    CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 241500, &none, &ms) == RD_MODESET_NO_TIMING);
                    none = m;
                    none.pixel_khz = 900000;
                    CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 241500, &none, &ms) == RD_MODESET_TMDS_CLOCK);
                    S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x7f;   /* DISPCLK 33.6 MHz */
                    CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 241500, &m, &ms) == RD_MODESET_CLOCKS);
                    S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x18;
                    dc2.head[0].dig_mode = 0;
                    CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 241500, &m, &ms) == RD_MODESET_NOT_TMDS);
                    dc2.head[0].dig_mode = 3;
                }
                CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 241500, &m, &ms) == RD_MODESET_OK);
                CHECK(ms.pipe == 1 && ms.otg == 2 && ms.opp == 2 && ms.fe == 2 && ms.be == 1 && ms.dig_mode == 3);
                CHECK(ms.cur.h_total == 2719 && ms.cur.pclk_khz == 241500);
                /* optc1_program_timing */
                CHECK(ms.want.h_total == 2199 && ms.want.v_total == 1124 &&
                      ms.want.h_blank == ((2200u - 88) | (192u << 16)) && ms.want.h_sync == (44u << 16) &&
                      ms.want.v_blank == ((1125u - 4) | (41u << 16)) && ms.want.v_sync == (5u << 16));
                /* DML 3.2: VStartup = vblank - 1, VUpdateOffset = htotal / 4,
                 * the times rescaled: ceil(30 x 148.5 / 241.5), ceil(100 x ...) */
                CHECK(ms.want.vstartup == 44 && ms.want.vupdate == (550u | (19u << 16)) && ms.want.vready == 62);
                /* VTG: VCOUNT_INIT 1121, FP2 = -(41 - 44 + 1) = 2 */
                CHECK((ms.want.vtg & 0x7fff) == 2 && ((ms.want.vtg >> 16) & 0x7fff) == 1121);
                /* DLG: ref/pix at 100 MHz / 148.5 MHz, htotal, h blank end, QoS */
                r_new = ms.want.ref_to_pix & 0x1fffff;
                CHECK(r_new >= 353050 && r_new <= 353060);
                CHECK(ms.want.dst_dim == (uint32_t)(((uint64_t)r_new * 2200) >> 11));
                CHECK((ms.want.blank0 & 0x1fff) == 129 && (ms.want.blank0 >> 16) == 41);
                CHECK((ms.want.qos_wm >> 16) == (uint32_t)((4ull * 2200 * r_new) >> 19));
                CHECK((ms.want.blank1 & 0x3ffff) == 4u * 1122 + 1);
                /* delivery: 1060 x (1920 / 2560) x (241.5 / 148.5) = 1293 */
                CHECK(ms.want.line >= 1291 && ms.want.line <= 1294);
                CHECK((ms.want.prefetch >> 24) == 40 && ms.want.viewport_dim == (1920u | 1080u << 16) &&
                      (ms.want.oppbuf & 0x3fff) == 1920 && (ms.want.oppbuf >> 16) == 1);
                /* apply: frames move, no underflow */
                S.dmub.nseq = 0;
                S.dcn_frames = 1;
                CHECK(dcn32_modeset_apply(&dev, NULL, &dc2, &sc, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) ==
                      RDNA3_ENODEV);
                CHECK(dcn32_modeset_apply(&dev, &dm, &dc2, &sc, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) ==
                      RDNA3_OK);
                CHECK(S.dmub.nseq == 4 && S.dmub.seq[0] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL &&
                      S.dmub.seq_tx_action[0] == RD_ATOM_TX_DISABLE && S.dmub.seq[1] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK &&
                      S.dmub.seq[2] == RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL &&
                      S.dmub.seq[3] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[3] == RD_ATOM_TX_ENABLE);
                CHECK(S.dmub.pixclk[0] == (1485000 & 0xff) && S.dmub.pixclk[2] == (1485000 >> 16) &&
                      S.dmub.pixclk[4] == RD_ATOM_COMBOPHY_PLL0 + 1 && S.dmub.pixclk[8] == 2);
                CHECK(S.dmub.enc[0] == 2 && S.dmub.enc[4] == (14850 & 0xff) && S.dmub.tx[0] == 1);
                CHECK(S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] == 2199 && S.reg[OTG(regOTG0_OTG_V_TOTAL_MIN, 2)] == 1124 &&
                      (S.reg[OTG(regOTG0_OTG_CONTROL, 2)] & 1) && (S.reg[DCN_B2 + regVTG0_CONTROL + 2] & VTG0_CONTROL__VTG0_ENABLE_MASK) &&
                      S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, 1)] == (1920u | 1080u << 16) &&
                      S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 1)] == (uint32_t)(FB_START + 0x8000000) &&
                      (S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_PITCH, 1)] & 0x3fff) == 1919 &&
                      !(S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK));
                CHECK(dc2.head[0].width == 1920 && dc2.head[0].h_total == 2200 && dc2.head[0].v_front == 4 &&
                      sc.native_w[1] == 1920 && !sc.saved[1]);
                CHECK(S.reg[DCN_B2 + regDIG0_HDMI_GENERIC_PACKET_CONTROL0 + RD_CLONE_DIGFE_STRIDE * 2] == 0);
                /* the read-back agrees: the core finds the new timing */
                CHECK(dcn32_read_state(&dev, &dc2) == 1 && dc2.head[0].h_active == 1920 && dc2.head[0].v_total == 1125);
                /* back up to 1440p from the new state, then a mode that does
                 * not run (frames frozen): the old one comes back */
                {
                    rdna3_mode up;
                    memset(&up, 0, sizeof(up));
                    up.h_active = 2560; up.v_active = 1440; up.h_total = 2720; up.v_total = 1481;
                    up.h_front = 48; up.h_sync = 32; up.v_front = 3; up.v_sync = 5; up.pixel_khz = 241500;
                    up.h_sync_pos = up.v_sync_pos = 1;
                    CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 148500, &up, &ms) == RD_MODESET_OK);
                    S.dcn_frames = 0;
                    S.dmub.nseq = 0;
                    k3 = S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)];
                    CHECK(dcn32_modeset_apply(&dev, &dm, &dc2, &sc, &ms, FB_START, 2560, FB_START + 0x8000000, 1920, 50) ==
                          RDNA3_EIO);
                    CHECK(S.dmub.nseq == 8 && S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] == k3 && dc2.head[0].width == 1920 &&
                          S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 1)] == (uint32_t)(FB_START + 0x8000000));
                    S.dcn_frames = 1;
                    CHECK(dcn32_modeset_apply(&dev, &dm, &dc2, &sc, &ms, FB_START, 2560, FB_START + 0x8000000, 1920, 50) ==
                          RDNA3_OK && dc2.head[0].width == 2560 && S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] == 2719);
                    S.dcn_frames = 0;
                }
            }
            S.dcn_pg = 0;
        }
        /* attach (rdna3dmub=3): no reset, no load, the running firmware's
         * own inbox1 found behind its code window; commands go through */
        {
            static rdna3_dmub dm2;
            uint32_t w[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 }, r[4] = { 0 };
            unsigned cmds0 = S.dmub.cmds, boots0 = S.dmub.boots;
            CHECK(dmub32_attach(&dm2, &dev) == RDNA3_OK && dm2.attached && dm2.hw_init);
            CHECK(dm2.inbox_vram == dm.fb_off + dm.region[RD_DMUB_WIN_MAILBOX].base &&
                  dm2.rb_size == RD_DMUB_RB_SIZE && dm2.inbox1_wptr == dm.inbox1_wptr);
            CHECK(dmub32_query_caps(&dm2) == RDNA3_OK && S.dmub.cmds == cmds0 + 1 && S.dmub.boots == boots0);
            /* the inbox beyond the BAR goes through MM_INDEX / MM_DATA */
            rdna3_vram_mm_write(&dev, VRAM_SIZE - 4096, w, 4);
            rdna3_vram_mm_read(&dev, VRAM_SIZE - 4096, r, 4);
            CHECK(memcmp(w, r, sizeof(w)) == 0);
            /* nothing running: refused */
            S.reg[DMR(regDMCUB_CNTL2)] |= DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK;
            CHECK(dmub32_attach(&dm2, &dev) == RDNA3_ENODEV);
            S.reg[DMR(regDMCUB_CNTL2)] &= ~DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK;
        }
        /* unload */
        dmub32_hw_reset(&dm);
        CHECK(!dmub32_is_hw_init(&dev) && !S.dmub.running && S.dmub.stops == 3);
        CHECK(dmub32_query_caps(&dm) == RDNA3_EIO);
        /* fused off */
        *R(DMR(regCC_DC_PIPE_DIS)) = 0;
        CHECK(dmub32_hw_init(&dm, S.vbios, VBIOS_LEN, 1, 1000) == RDNA3_ENODEV);
        *R(DMR(regCC_DC_PIPE_DIS)) = CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK;
    }

    /* GPU reset: mode-1 through the PMFW debug mailbox takes the chip back
     * to the PSP bootloader with VRAM intact; PSP, the firmware and the SMU
     * come up again from there (what the kext redoes from level 4) */
    {
        static rdna3_psp p3;
        uint32_t ipfw0 = S.nipfw;
        CHECK(!psp13_bootloader_stuck(&psp));                  /* sOS alive */
        CHECK(smu13_mode1_reset(&dev) == RDNA3_OK);
        CHECK(S.mode1_resets == 1 && !S.sos_alive && !S.smu_loaded);
        CHECK(psp13_wait_for_bootloader(&psp) == RDNA3_OK);
        CHECK(!psp13_bootloader_stuck(&psp));
        /* passthrough after the host driver: no sOS, bootloader not ready */
        *R(MP0(35)) = 0;
        CHECK(psp13_bootloader_stuck(&psp));
        psp13_log_state(&psp, "test");
        *R(MP0(35)) = 0x80000000u;
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
