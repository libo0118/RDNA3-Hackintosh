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
#include "dchpd.h"
#include "edid.h"
#include "dcn32_modeset.h"
#include "atom.h"
#include "dmub32.h"
#include "dcn32_mode.h"
#include "gfx11_info.h"
#include "xfer.h"
#include "rdna3_drm.h"
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
#include "gen/nbio_4_3_0_ih.h"
#include "gen/dcn_3_2_1.h"
#include "gen/gc_11_0_0_gfx.h"
#include "gen/gc_11_0_0_reset.h"
#include "gen/gc_11_0_0_cg.h"
#include "gen/gc_11_0_0_rs64.h"
#include "gen/gc_11_0_0_mes.h"
#include "gen/gc_11_0_0_irq.h"
#include "gen/dcn_3_2_0_gpio.h"
#include "gen/dcn_3_2_0_i2c.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "dcn32_clone.h"
#include "dcn32_gamma.h"
#include "gen/dcn_3_2_0_gamma.h"
#include "dcn32_state.h"
#include "dcn32_dp.h"
#include "gen/dcn_3_2_0_dp.h"
#include "gen/gc_11_0_3_gmc.h"
#include "gen/mmhub_3_0_0.h"
#include "navi3x_regs.h"
#include "gen/thm_13_0_2_fan.h"
#include "psp_gfx_if.h"
#include "amdgpu_ucode_hdr.h"
#include "vcn40.h"
#include "gen/vcn_4_0_0.h"

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
#define THM_BASE   0x16600u   /* THM 13.0.2 segment 0 */
#define THMR(reg)  (THM_BASE + reg)
#define ROM_STRAP  1u          /* REGS_ROM_OFFSET_CTRL.ROM_OFFSET */
#define SMN_ROM_OFFSET_CTRL ((NBIO_B5 + regREGS_ROM_OFFSET_CTRL) * 4u)
#define VBIOS_LEN  8192u
#define DCN_B2     0x34C0u   /* DCN segment 2 (dcn_3_2_1 DCN_BASE__INST0_SEG2) */
#define DCN_B3     0x9000u   /* DCN segment 3: MPC */
#define DCN_B1     0xC0u     /* DCN segment 1: DCCG */
/* VCN 4.0 register segment 1 of instance i (discovery on the card) */
#define VCN_B1(i)  ((i) ? 0x12000u : 0x7E00u)
#define VCNR(i, reg) (VCN_B1(i) + (reg))
#define VCN_TMR_FW_OFF(i) (0x100000ull * (1 + (i)))
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
    uint32_t ip_fw_types[256];
    unsigned nipfw;
    int tmr_setup, toc_loaded, autoload, autoload_after_rlc_g, smu_loaded;
    int tmr_after_smu;
    int smu_after_tmr;            /* LOAD_IP_FW SMU came after SETUP_TMR */
    /* trusted applications (LOAD_TA / LOAD_ASD / INVOKE_CMD / UNLOAD_TA):
     * live sessions, what was refused, DESTROY_TMR, the register list */
    unsigned ta_loads, asd_loads, ta_unloads, ta_invokes, ta_bad, tmr_destroys, rl_loads, rl_before_tmr;
    unsigned ring_destroys;
    uint32_t ta_live[8], ta_next_session, ta_last_cmd, ta_last_len[8];
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
    /* SET_Q_PREEMPTION_MODE packets the CP ran (dw 1-8 of the last four)
     * and COND_EXEC packets that discarded their dwords */
    unsigned cp_qmode, cp_cond_skips;
    uint32_t cp_qmode_hist[4][8];
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
    /* user queues the MES scheduler took (ADD_QUEUE, mes_v11_api_def.h): the
     * queue's MQD, doorbell and process; the process gets a VMID from the
     * GFXHUB mask of SET_HW_RESOURCES and its page tables are loaded into
     * that context. A doorbell runs the queue's ring through the process VM
     * up to the doorbell's wptr. A RELEASE_MEM that interrupts (INT_SEL 2)
     * makes the firmware write the queue's rptr to the MQD's fence address
     * and raise the end-of-pipe IV with the doorbell in src_data[0]; an SDMA
     * queue does that at a FENCE packet (SDMA_FENCE IV, doorbell << 2). The
     * rptr goes to the queue's rptr address after every run. */
    struct sim_uq {
        int used;
        uint32_t type, doorbell, process_id, vmid, me, pipe, queue;
        uint64_t mqd, wptr_addr, gang_ctx, proc_ctx, pd, fence_addr;
        uint64_t rptr;               /* dwords (bytes for SDMA), never wraps */
        uint32_t queue_size_dw;      /* ADD_QUEUE gds_size */
        unsigned runs, eops;
    } uq[8];
    unsigned uq_adds, uq_removes, uq_bad, uq_faults, uq_runs, uq_eops;
    int cp_cur_uq;                   /* 1 + index of the user queue cp_exec runs, 0 = a kernel ring */
    uint64_t cp_uq_pos;              /* ring position after the queue's current packet */
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
    uint8_t  dpcd_ext[0x20];     /* 0x2200.. extended receiver caps */
    uint8_t  dpcd_lttpr[0x10];   /* 0xf0000.. LTTPR caps (zero: no repeater) */
    /* the DP sink's link layer (dp_sink_eval): the sink is cabled to
     * transmitter dps.be (-1: not modelled, DPCD is plain storage). It
     * locks clock recovery on TPS1 once the source drives voltage swing
     * need_vs (one level more per adjust request), channel EQ on
     * TPS2/3/4 once pre-emphasis reaches need_pe, never CR at LINK_BW
     * fail_rate; the PHY state comes from the DMCUB's TransmitterControl
     * (ENABLE: symbol clock, lanes; SETUP_VSEMPH: the lane set) */
    struct {
        int be;
        unsigned need_vs, need_pe;
        uint32_t fail_rate;
        int phy_on, cr, eq;
        uint32_t symclk_10khz, lanes;
        uint8_t vs, pe;
        unsigned vsemph, adjust_reqs, tps1_sets, lane_mismatch, evals;
        uint8_t eq_pattern;          /* the last EQ pattern the source asked for in DPCD */
        uint32_t trained_rate, trained_lanes;
    } dps;
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
    /* DISPCLK / DPPCLK DPM levels (GetDpmFreqByIndex); SetHardMinByFreq on
     * them moves the CLK1 DFS divider (unless dal_ignore_hard_min) and is
     * logged in order with the DMCUB's VBIOS command count and OTG 2's
     * H_TOTAL at that moment */
    unsigned dal_dpm_n, dal_dpm[8], dal_ignore_hard_min;
    unsigned dal_norder, dal_order[8], dal_order_nseq[8], dal_order_htotal[8];
    /* the PMFW stops answering once the DMCUB ran this many VBIOS commands
     * (0 = never), as it did on the card after the clone */
    unsigned smu_wedge_at;
    unsigned hpd_irqs;
    /* HPD cable model (dchpd.c): pins in hpd_model have a cable level and
     * the toggle filter: DC_HPD_SENSE follows the cable at once,
     * DC_HPD_SENSE_DELAYED once the cable held its level for the
     * DC_HPD_TOGGLE_FILT_CNTL connect / disconnect delay (10 ms units) */
    uint32_t hpd_model;
    int8_t   hpd_cable[6], hpd_delayed[6];
    uint64_t hpd_since[6];
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
        /* PM control: the overdrive settings the PMFW runs with, what it
         * accepted / refused, activity monitor coefficients per workload,
         * feature switches, DF C-state, BACO */
        int od_init;
        OverDriveTable_t od;
        unsigned od_uploads, od_rejects, coeff_writes, coeff_rejects, feature_msgs;
        uint32_t od_last_mask;
        DpmActivityMonitorCoeffIntExternal_t coeff[WORKLOAD_PPLIB_COUNT];
        uint32_t df_cstate, df_msgs;
        int baco;             /* in BACO: only ExitBaco is answered */
        unsigned baco_enters, baco_exits, armd3_msgs, msgs_in_baco;
        uint32_t baco_seq, armd3_seq;
        int sim_throttle;     /* metrics report PPT0 + hotspot throttling */
        uint32_t soft_min[PPCLK_COUNT], soft_max[PPCLK_COUNT];   /* last SetSoftMin/MaxByFreq */
        /* stage 4: OverridePcieParameters after EnableAllSmuFeatures, the
         * PrepareMp1ForUnload argument, DALNotPresent, SetGpoAllow,
         * AllowIHHostInterrupt, ReenableAcDcInterrupt */
        unsigned pcie_after_enable, dal_not_present, gpo_msgs, ih_allow, acdc_reenable;
        uint32_t unload_arg, gpo;
        int all_enabled;      /* EnableAllSmuFeatures done */
    } smu;
    unsigned vm_huge_walks;              /* translations through a PDE_PTE (huge) entry */
    unsigned cp_mem_polls, cp_fault_ivs, cp_nop1;   /* WAIT_REG_MEM on memory, CP fault IVs, 1-dword NOPs */
    int gfx_mutex_fw;                    /* the firmware holds CP_GFX_INDEX_MUTEX */
    uint32_t sh_mem[16][3];              /* per VMID: SH_MEM_CONFIG, SH_MEM_BASES, SPI_GDBG_PER_VMID_CNTL */
    unsigned ih_snooped, ih_ring1_ivs, ih_db_rptr, ih_db_unrouted;   /* IH in system memory (rdna3amdgpu=1) */
    unsigned sdma_pkts, sdma_bad, sdma_while_halted;
    unsigned sdma_ibs, sdma_ib_fault;   /* INDIRECT packets run, ones the GFXHUB could not translate */
    int      sdma_hang;              /* the engine stopped consuming its ring */
    uint32_t harvest_wgp[8][2];
    uint64_t tsc;                    /* GOLDEN_TSC: +4 on every access of either half */
    uint8_t  sysmem[1u << 20];       /* system RAM at SYS_PHYS, reached through GART */
    unsigned gart_unmapped;      /* INACTIVE_WGPS per SE/SA (CC_GC_SHADER_ARRAY_CONFIG) */
    unsigned sdma_resets, sdma_resets_unfrozen;
    /* VCN 4.0 / JPEG 4.0 (see the model above sim_wr) */
    struct {
        int smu_on[2], jpeg_smu_on;
        unsigned vcn_up_msgs, vcn_down_msgs, jpeg_up_msgs, jpeg_down_msgs, regs_while_off;
        uint64_t tmr_fw[2];          /* where LOAD_IP_FW VCN / VCN1 put the image */
        int booted[2];
        unsigned boots, bad_boots, rb_enables, bad_fw_shared, dbs, db_unrouted;
        uint32_t rptr[2];
        unsigned cmds, ibs, bad, fences, traps, ends, reg_writes, reg_waits, ib_faults;
        unsigned enc_init, enc_close, dec_create, dec_destroy, checksum_bad;
        uint32_t last_ib_inst, last_ib_vmid;
        uint32_t jregs[0x200], jext, jrptr, jpeg_ib_vmid, jpeg_vmid;
        unsigned jext_writes, jpeg_cmds, jpeg_ibs, jpeg_fences, jpeg_traps, jpeg_bad, jpeg_ib_faults;
        /* DPG: the SRAM images the PSP loaded (VCN0_RAM / VCN1_RAM), the
         * LMA writes, the pause handshake */
        uint32_t fw_size[2];         /* the image LOAD_IP_FW VCN / VCN1 placed */
        uint32_t sram[2][64][2], sram_n[2];
        unsigned sram_loads[2], sram_bad, lma_writes, lma_bad, static_pg_in_dpg;
        unsigned pause_reqs, unpause_reqs, pause_bad, down_in_dpg;
    } vcn;
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

/* OverDriveLimitsMin / OverDriveLimitsBasicMax of the SKU: GFXCLK / UCLK
 * limits, voltage offset, PPT, fan curve and zero RPM (PP_OD_FEATURE_*) */
#define SIM_OD_FEATURES ((1u << PP_OD_FEATURE_GFX_VF_CURVE_BIT) | (1u << PP_OD_FEATURE_PPT_BIT) | \
                         (1u << PP_OD_FEATURE_FAN_CURVE_BIT) | (1u << PP_OD_FEATURE_GFXCLK_BIT) | \
                         (1u << PP_OD_FEATURE_UCLK_BIT) | (1u << PP_OD_FEATURE_ZERO_FAN_BIT))
static void sim_od_limits(OverDriveLimits_t *lo, OverDriveLimits_t *up)
{
    memset(lo, 0, sizeof(*lo));
    memset(up, 0, sizeof(*up));
    lo->FeatureCtrlMask = up->FeatureCtrlMask = SIM_OD_FEATURES;
    lo->VoltageOffsetPerZoneBoundary = -150; up->VoltageOffsetPerZoneBoundary = 0;
    lo->GfxclkFmin = 500;  up->GfxclkFmin = 2500;
    lo->GfxclkFmax = 500;  up->GfxclkFmax = 3000;
    lo->UclkFmin = 97;     up->UclkFmin = 1250;
    lo->UclkFmax = 97;     up->UclkFmax = 1350;
    lo->Ppt = -10;         up->Ppt = 15;
    lo->FanLinearPwmPoints = 15;  up->FanLinearPwmPoints = 100;
    lo->FanLinearTempPoints = 25; up->FanLinearTempPoints = 100;
    lo->FanMinimumPwm = 15;       up->FanMinimumPwm = 100;
    lo->AcousticTargetRpmThreshold = 500; up->AcousticTargetRpmThreshold = 3300;
    lo->AcousticLimitRpmThreshold = 500;  up->AcousticLimitRpmThreshold = 3300;
    lo->FanTargetTemperature = 25;        up->FanTargetTemperature = 110;
    lo->FanZeroRpmEnable = 0;             up->FanZeroRpmEnable = 1;
    lo->FanZeroRpmStopTemp = 25;          up->FanZeroRpmStopTemp = 100;
}

/* the PMFW's overdrive settings at boot, and per-workload activity
 * monitor coefficients (made up, but distinct per workload) */
static void sim_smu_pm_defaults(void)
{
    static const uint8_t temps[NUM_OD_FAN_MAX_POINTS] = { 25, 45, 65, 80, 95, 100 };
    static const uint8_t pwms[NUM_OD_FAN_MAX_POINTS] = { 20, 30, 45, 60, 80, 100 };
    unsigned i;
    if (S.smu.od_init)
        return;
    S.smu.od_init = 1;
    memset(&S.smu.od, 0, sizeof(S.smu.od));
    S.smu.od.GfxclkFmin = 500;  S.smu.od.GfxclkFmax = 2600;
    S.smu.od.UclkFmin = 97;     S.smu.od.UclkFmax = 1250;
    for (i = 0; i < NUM_OD_FAN_MAX_POINTS; i++) {
        S.smu.od.FanLinearTempPoints[i] = temps[i];
        S.smu.od.FanLinearPwmPoints[i] = pwms[i];
    }
    S.smu.od.FanMinimumPwm = 20;
    S.smu.od.AcousticTargetRpmThreshold = 1450;
    S.smu.od.AcousticLimitRpmThreshold = 2200;
    S.smu.od.FanTargetTemperature = 85;
    S.smu.od.FanZeroRpmEnable = 1;
    S.smu.od.FanZeroRpmStopTemp = 50;
    S.smu.od.FanMode = FAN_MODE_AUTO;
    S.smu.od.MaxOpTemp = 110;
    for (i = 0; i < WORKLOAD_PPLIB_COUNT; i++) {
        DpmActivityMonitorCoeffInt_t *a = &S.smu.coeff[i].DpmActivityMonitorCoeffInt;
        memset(&S.smu.coeff[i], 0, sizeof(S.smu.coeff[i]));
        a->Gfx_FPS = (uint8_t)(10 + i);
        a->Gfx_MinActiveFreqType = 1;
        a->Gfx_MinActiveFreq = (uint16_t)(500 + 100 * i);
        a->Gfx_BoosterFreq = (uint16_t)(1000 + 100 * i);
        a->Gfx_PD_Data_limit_c = 0x10000u * (i + 1);
        a->Fclk_FPS = (uint8_t)(20 + i);
        a->Fclk_MinActiveFreq = (uint16_t)(800 + i);
    }
    S.smu.df_cstate = ~0u;
}

/* what the PMFW checks on an OverDriveTable upload: every group named in
 * FeatureCtrlMask must be a feature the SKU allows and lie within the
 * OverDriveLimitsMin / BasicMax of the PPTable; then only those groups
 * are taken. Returns 0 when refused. */
static int sim_od_accept(const OverDriveTable_t *t)
{
    OverDriveLimits_t lo, up;
    uint32_t m = t->FeatureCtrlMask;
    int i;
    sim_od_limits(&lo, &up);
    if (m & ~up.FeatureCtrlMask)
        return 0;
#define IN(v, f) ((v) >= lo.f && (v) <= up.f)
    if (m & (1u << PP_OD_FEATURE_GFXCLK_BIT))
        if (!IN(t->GfxclkFmin, GfxclkFmin) || !IN(t->GfxclkFmax, GfxclkFmax) || t->GfxclkFmin > t->GfxclkFmax)
            return 0;
    if (m & (1u << PP_OD_FEATURE_UCLK_BIT))
        if (!IN(t->UclkFmin, UclkFmin) || !IN(t->UclkFmax, UclkFmax) || t->UclkFmin > t->UclkFmax)
            return 0;
    if (m & (1u << PP_OD_FEATURE_GFX_VF_CURVE_BIT))
        for (i = 0; i < PP_NUM_OD_VF_CURVE_POINTS; i++)
            if (!IN(t->VoltageOffsetPerZoneBoundary[i], VoltageOffsetPerZoneBoundary))
                return 0;
    if (m & (1u << PP_OD_FEATURE_PPT_BIT))
        if (!IN(t->Ppt, Ppt))
            return 0;
    if (m & (1u << PP_OD_FEATURE_FAN_CURVE_BIT)) {
        if (t->FanMode > FAN_MODE_MANUAL_LINEAR)
            return 0;
        for (i = 0; i < NUM_OD_FAN_MAX_POINTS - 1; i++)
            if (!IN(t->FanLinearTempPoints[i], FanLinearTempPoints) ||
                !IN(t->FanLinearPwmPoints[i], FanLinearPwmPoints))
                return 0;
        if (!IN(t->FanMinimumPwm, FanMinimumPwm) || !IN(t->AcousticLimitRpmThreshold, AcousticLimitRpmThreshold) ||
            !IN(t->AcousticTargetRpmThreshold, AcousticTargetRpmThreshold) ||
            !IN(t->FanTargetTemperature, FanTargetTemperature))
            return 0;
    }
    if (m & (1u << PP_OD_FEATURE_ZERO_FAN_BIT))
        if (!IN(t->FanZeroRpmEnable, FanZeroRpmEnable) || !IN(t->FanZeroRpmStopTemp, FanZeroRpmStopTemp))
            return 0;
#undef IN
    if (m & (1u << PP_OD_FEATURE_GFXCLK_BIT)) {
        S.smu.od.GfxclkFmin = t->GfxclkFmin; S.smu.od.GfxclkFmax = t->GfxclkFmax;
    }
    if (m & (1u << PP_OD_FEATURE_UCLK_BIT)) {
        S.smu.od.UclkFmin = t->UclkFmin; S.smu.od.UclkFmax = t->UclkFmax;
    }
    if (m & (1u << PP_OD_FEATURE_GFX_VF_CURVE_BIT))
        memcpy(S.smu.od.VoltageOffsetPerZoneBoundary, t->VoltageOffsetPerZoneBoundary,
               sizeof(t->VoltageOffsetPerZoneBoundary));
    if (m & (1u << PP_OD_FEATURE_PPT_BIT))
        S.smu.od.Ppt = t->Ppt;
    if (m & (1u << PP_OD_FEATURE_FAN_CURVE_BIT)) {
        memcpy(S.smu.od.FanLinearTempPoints, t->FanLinearTempPoints, sizeof(t->FanLinearTempPoints));
        memcpy(S.smu.od.FanLinearPwmPoints, t->FanLinearPwmPoints, sizeof(t->FanLinearPwmPoints));
        S.smu.od.FanMinimumPwm = t->FanMinimumPwm;
        S.smu.od.AcousticLimitRpmThreshold = t->AcousticLimitRpmThreshold;
        S.smu.od.AcousticTargetRpmThreshold = t->AcousticTargetRpmThreshold;
        S.smu.od.FanTargetTemperature = t->FanTargetTemperature;
        S.smu.od.FanMode = t->FanMode;
    }
    if (m & (1u << PP_OD_FEATURE_ZERO_FAN_BIT)) {
        S.smu.od.FanZeroRpmEnable = t->FanZeroRpmEnable;
        S.smu.od.FanZeroRpmStopTemp = t->FanZeroRpmStopTemp;
    }
    S.smu.od_last_mask = m;
    return 1;
}

/* the fan the PMFW drives: in its own (auto) control the OD fan curve at
 * the hotspot temperature when the user set one, else 30 % / 900 rpm; with
 * fan control stopped the THM FDO static duty or static RPM */
static void sim_fan(uint32_t *pct, uint32_t *rpm)
{
    const uint32_t max_rpm = 3300, hotspot = 60;
    *pct = 30; *rpm = 900;
    if (S.smu.enabled & RD_SMU_FEATURE(FEATURE_FAN_CONTROL_BIT)) {
        if (S.smu.od_init && S.smu.od.FanMode == FAN_MODE_MANUAL_LINEAR) {
            unsigned i;
            for (i = 0; i < NUM_OD_FAN_MAX_POINTS - 1; i++)
                if (S.smu.od.FanLinearTempPoints[i] >= hotspot)
                    break;
            *pct = S.smu.od.FanLinearPwmPoints[i < NUM_OD_FAN_MAX_POINTS - 1 ? i : NUM_OD_FAN_MAX_POINTS - 2];
            *rpm = *pct * max_rpm / 100;
        }
        return;
    }
    switch ((*R(THMR(regCG_FDO_CTRL2)) & CG_FDO_CTRL2__FDO_PWM_MODE_MASK) >> CG_FDO_CTRL2__FDO_PWM_MODE__SHIFT) {
    case RD_FDO_PWM_MODE_STATIC: {
        uint32_t duty100 = *R(THMR(regCG_FDO_CTRL1)) & CG_FDO_CTRL1__FMAX_DUTY100_MASK;
        uint32_t duty = *R(THMR(regCG_FDO_CTRL0)) & CG_FDO_CTRL0__FDO_STATIC_DUTY_MASK;
        *pct = duty100 ? duty * 100 / duty100 : 0;
        *rpm = *pct * max_rpm / 100;
        break;
    }
    case RD_FDO_PWM_MODE_STATIC_RPM: {
        uint32_t period = (*R(THMR(regCG_TACH_CTRL)) & CG_TACH_CTRL__TARGET_PERIOD_MASK) >>
                          CG_TACH_CTRL__TARGET_PERIOD__SHIFT;
        *rpm = period ? 60u * 2500u * 10000u / (8u * period) : 0;
        *pct = *rpm * 100 / max_rpm;
        break;
    }
    default:
        break;
    }
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
    pp->SkuTable.DriverReportedClocks.BaseClockAc = 1295;
    pp->SkuTable.FanAbnormalTempLimitOffset = 10;
    pp->SkuTable.FeaturesToRun[0] = 1u << FEATURE_FAN_CONTROL_BIT;
    sim_od_limits(&pp->SkuTable.OverDriveLimitsMin, &pp->SkuTable.OverDriveLimitsBasicMax);
    /* overdrive_table.max/min[SMU_13_0_0_ODSETTING_POWERPERCENTAGE]: +15 / -10 % */
    put32(out + RD_SMU_PP_OD_MAX_OFF + 4 * RD_SMU_13_0_0_ODSETTING_POWERPERCENTAGE, 15);
    put32(out + RD_SMU_PP_OD_MIN_OFF + 4 * RD_SMU_13_0_0_ODSETTING_POWERPERCENTAGE, 10);
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
    {
        uint32_t pct, rpm;
        sim_fan(&pct, &rpm);
        m->SmuMetrics.AvgFanRpm = (uint16_t)rpm;
        m->SmuMetrics.AvgFanPwm = (uint8_t)pct;
    }
    m->SmuMetrics.AvgVoltage[SVI_PLANE_GFX] = 850;
    m->SmuMetrics.AvgVoltage[SVI_PLANE_SOC] = 750;
    m->SmuMetrics.CurrClock[PPCLK_SOCCLK] = 900;
    m->SmuMetrics.CurrClock[PPCLK_FCLK] = 1000;
    m->SmuMetrics.AvgVoltage[SVI_PLANE_VMEMP] = 1250;
    m->SmuMetrics.AvgTemperature[TEMP_VR_GFX] = 55;
    m->SmuMetrics.AvgTemperature[TEMP_VR_MEM0] = 48;
    m->SmuMetrics.AvgTemperature[TEMP_VR_MEM1] = 52;
    m->SmuMetrics.AverageGfxclkFrequencyPostDs = 40;
    m->SmuMetrics.AverageMemclkFrequencyPostDs = 96;
    m->SmuMetrics.AverageMemclkFrequencyPreDs = 1218;
    m->SmuMetrics.AverageUclkActivity = 3;
    m->SmuMetrics.EnergyAccumulator = 0x12345678u;
    m->SmuMetrics.PcieRate = 4;     /* Gen4 */
    m->SmuMetrics.PcieWidth = 16;
    m->SmuMetrics.PublicSerialNumberLower = 0x5678EF01u;
    m->SmuMetrics.PublicSerialNumberUpper = 0x1234ABCDu;
    if (S.smu.sim_throttle) {
        m->SmuMetrics.ThrottlingPercentage[THROTTLER_PPT0_BIT] = 20;
        m->SmuMetrics.ThrottlingPercentage[THROTTLER_TEMP_HOTSPOT_BIT] = 5;
    }
}

/* DPM levels per PPCLK (MHz); gfxclk is fine grained (two end points) */
static unsigned sim_dpm_levels(uint32_t clk, const uint32_t **lv, int *fine)
{
    static const uint32_t gfx[] = { 500, 2124 }, uclk[] = { 96, 500, 1000, 1218 },
                          soc[] = { 600, 900, 1200 }, fclk[] = { 1000, 1800 },
                          vclk[] = { 500, 1300, 2600 }, dclk[] = { 400, 1100, 2200 };
    *fine = 0;
    switch (clk) {
    case PPCLK_GFXCLK: *lv = gfx; *fine = 1; return 2;
    case PPCLK_UCLK:   *lv = uclk; return 4;
    case PPCLK_SOCCLK: *lv = soc; return 3;
    case PPCLK_FCLK:   *lv = fclk; return 2;
    case PPCLK_VCLK_0: case PPCLK_VCLK_1: *lv = vclk; return 3;
    case PPCLK_DCLK_0: case PPCLK_DCLK_1: *lv = dclk; return 3;
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
    case PPCLK_VCLK_0: case PPCLK_VCLK_1: case PPCLK_DCLK_0: case PPCLK_DCLK_1:
        return (S.smu.enabled & RD_SMU_FEATURE(FEATURE_MM_DPM_BIT)) != 0;
    default: return 0;
    }
}

/* system pages for gpuvm_map: pairs swapped (scattered) / in order */
static uint64_t test_scatter_pa(void *ctx, uint64_t i)
{
    return *(const uint64_t *)ctx + (i ^ 1) * 4096;
}

static uint64_t test_contig_pa(void *ctx, uint64_t i)
{
    return *(const uint64_t *)ctx + i * 4096;
}

/* amdgpu_asic_update_umd_stable_pstate stand-in: counts enter / exit */
static void test_umd_pstate(void *ctx, int enter)
{
    *(int *)ctx += enter ? 1 : -1;
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

    sim_smu_pm_defaults();
    /* BACO: the chip is off, the PMFW only takes the way out */
    if (S.smu.baco && v != PPSMC_MSG_ExitBaco) {
        S.smu.msgs_in_baco++;
        *R(MP1(90)) = PPSMC_Result_CmdRejectedPrereq;
        return;
    }
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
        case TABLE_OVERDRIVE: {
            OverDriveTableExternal_t o;
            memset(&o, 0, sizeof(o));
            o.OverDriveTable = S.smu.od;
            o.OverDriveTable.FeatureCtrlMask = 0;
            memcpy(tbl, &o, sizeof(o));
            break;
        }
        case TABLE_ACTIVITY_MONITOR_COEFF:
            if ((arg >> 16) >= WORKLOAD_PPLIB_COUNT) { resp = PPSMC_Result_Failed; break; }
            memcpy(tbl, &S.smu.coeff[arg >> 16], sizeof(S.smu.coeff[0]));
            break;
        default: resp = PPSMC_Result_Failed;
        }
        break;
    case PPSMC_MSG_TransferTableDram2Smu:
        S.smu.xfers++;
        if (!tbl) { S.smu.bad_addr++; resp = PPSMC_Result_Failed; break; }
        if ((arg & 0xffff) == TABLE_OVERDRIVE) {
            OverDriveTableExternal_t o;
            if (S.pending_writes)
                S.smu.unflushed++;
            memcpy(&o, tbl, sizeof(o));
            if (sim_od_accept(&o.OverDriveTable)) {
                S.smu.od_uploads++;
            } else {
                S.smu.od_rejects++;
                resp = PPSMC_Result_Failed;
            }
            break;
        }
        if ((arg & 0xffff) == TABLE_ACTIVITY_MONITOR_COEFF) {
            /* only the CUSTOM workload's coefficients can be changed */
            if (S.pending_writes)
                S.smu.unflushed++;
            if ((arg >> 16) != WORKLOAD_PPLIB_CUSTOM_BIT) {
                S.smu.coeff_rejects++;
                resp = PPSMC_Result_Failed;
                break;
            }
            memcpy(&S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT], tbl, sizeof(S.smu.coeff[0]));
            S.smu.coeff_writes++;
            break;
        }
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
        /* fan control: the PMFW programs the FDO's 100 % duty */
        if (S.smu.enabled & RD_SMU_FEATURE(FEATURE_FAN_CONTROL_BIT))
            *R(THMR(regCG_FDO_CTRL1)) = (*R(THMR(regCG_FDO_CTRL1)) & ~CG_FDO_CTRL1__FMAX_DUTY100_MASK) | 200u;
        S.smu.all_enabled = 1;
        break;
    case PPSMC_MSG_EnableSmuFeaturesLow:
    case PPSMC_MSG_EnableSmuFeaturesHigh:
    case PPSMC_MSG_DisableSmuFeaturesLow:
    case PPSMC_MSG_DisableSmuFeaturesHigh: {
        int high = v == PPSMC_MSG_EnableSmuFeaturesHigh || v == PPSMC_MSG_DisableSmuFeaturesHigh;
        uint64_t m = (uint64_t)arg << (high ? 32 : 0);
        S.smu.feature_msgs++;
        if (v == PPSMC_MSG_EnableSmuFeaturesLow || v == PPSMC_MSG_EnableSmuFeaturesHigh) {
            if (m & ~SIM_SMU_SUPPORTED) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
            S.smu.enabled |= m;
        } else {
            S.smu.enabled &= ~m;
        }
        break;
    }
    case PPSMC_MSG_SetExternalClientDfCstateAllow:
        if (arg > 1) { resp = PPSMC_Result_Failed; break; }
        S.smu.df_cstate = arg; S.smu.df_msgs++;
        break;
    case PPSMC_MSG_EnterBaco:
        if (arg != RD_BACO_SEQ_BACO && arg != RD_BACO_SEQ_BAMACO) { resp = PPSMC_Result_Failed; break; }
        if (!(S.smu.enabled & RD_SMU_FEATURE(FEATURE_BACO_BIT))) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        S.smu.baco = 1; S.smu.baco_seq = arg; S.smu.baco_enters++;
        break;
    case PPSMC_MSG_ExitBaco:
        if (!S.smu.baco) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        S.smu.baco = 0; S.smu.baco_exits++;
        break;
    case PPSMC_MSG_ArmD3:
        S.smu.armd3_seq = arg; S.smu.armd3_msgs++;
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
        if (S.smu.all_enabled)
            S.smu.pcie_after_enable++;
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
        if ((arg >> 16) < PPCLK_COUNT) {
            if (v == PPSMC_MSG_SetSoftMinByFreq)
                S.smu.soft_min[arg >> 16] = arg & 0xffff;
            else
                S.smu.soft_max[arg >> 16] = arg & 0xffff;
        }
        break;
    case PPSMC_MSG_GetPptLimit: *R(MP1(82)) = S.smu.ppt_limit ? S.smu.ppt_limit : 263; break;
    case PPSMC_MSG_SetPptLimit:
        /* the PMFW takes nothing above the PPTable's message limit */
        if (arg > 280) { resp = PPSMC_Result_CmdRejectedPrereq; break; }
        S.smu.ppt_limit = arg;
        break;
    case PPSMC_MSG_SetWorkloadMask: S.smu.workload = arg; break;
    case PPSMC_MSG_AllowGfxOff:     S.smu.gfxoff = 1; S.smu.gfxoff_msgs++; break;
    case PPSMC_MSG_DisallowGfxOff:  S.smu.gfxoff = 0; S.smu.gfxoff_msgs++; break;
    case PPSMC_MSG_NotifyPowerSource: break;
    case PPSMC_MSG_PrepareMp1ForUnload: S.smu.unload = 1; S.smu.unload_arg = arg; break;
    case PPSMC_MSG_DALNotPresent:   S.smu.dal_not_present++; break;
    case PPSMC_MSG_SetGpoAllow:     S.smu.gpo = arg; S.smu.gpo_msgs++; break;
    case PPSMC_MSG_AllowIHHostInterrupt: S.smu.ih_allow++; break;
    case PPSMC_MSG_ReenableAcDcInterrupt: S.smu.acdc_reenable++; break;
    case PPSMC_MSG_PowerUpVcn:
    case PPSMC_MSG_PowerDownVcn:
        if ((arg >> 16) > 1 || (arg & 0xffff)) { resp = PPSMC_Result_Failed; break; }
        S.vcn.smu_on[arg >> 16] = v == PPSMC_MSG_PowerUpVcn;
        if (v == PPSMC_MSG_PowerUpVcn) {
            S.vcn.vcn_up_msgs++;
        } else {
            /* the instance loses its power: the VCPU and its state go; in
             * DPG mode (or paused) the driver must have left it first */
            uint32_t vi = arg >> 16;
            if ((S.reg[VCNR(vi, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_PG_MODE_MASK) ||
                (S.reg[VCNR(vi, regUVD_DPG_PAUSE)] & UVD_DPG_PAUSE__NJ_PAUSE_DPG_REQ_MASK))
                S.vcn.down_in_dpg++;
            S.vcn.vcn_down_msgs++;
            S.vcn.booted[vi] = 0;
            S.reg[VCNR(vi, regUVD_STATUS)] = 0;
        }
        break;
    case PPSMC_MSG_PowerUpJpeg:
    case PPSMC_MSG_PowerDownJpeg:
        S.vcn.jpeg_smu_on = v == PPSMC_MSG_PowerUpJpeg;
        if (v == PPSMC_MSG_PowerUpJpeg) S.vcn.jpeg_up_msgs++; else S.vcn.jpeg_down_msgs++;
        break;
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
    memset(S.dpcd_ext, 0, sizeof(S.dpcd_ext));
    memset(S.dpcd_lttpr, 0, sizeof(S.dpcd_lttpr));
    memset(&S.dps, 0, sizeof(S.dps));
    S.dps.be = -1;
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
static void hpd_filter_eval(uint32_t n)
{
    uint32_t f = S.reg[HPDR(regHPD0_DC_HPD_TOGGLE_FILT_CNTL, n)];
    uint32_t *st = &S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, n)];
    uint64_t delay = S.hpd_cable[n]
        ? 10000ull * ((f & HPD0_DC_HPD_TOGGLE_FILT_CNTL__DC_HPD_CONNECT_INT_DELAY_MASK) >>
                      HPD0_DC_HPD_TOGGLE_FILT_CNTL__DC_HPD_CONNECT_INT_DELAY__SHIFT)
        : 10000ull * ((f & HPD0_DC_HPD_TOGGLE_FILT_CNTL__DC_HPD_DISCONNECT_INT_DELAY_MASK) >>
                      HPD0_DC_HPD_TOGGLE_FILT_CNTL__DC_HPD_DISCONNECT_INT_DELAY__SHIFT);
    if (S.hpd_delayed[n] != S.hpd_cable[n] && S.now_us - S.hpd_since[n] >= delay)
        S.hpd_delayed[n] = S.hpd_cable[n];
    *st &= ~(HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK | HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK);
    *st |= (S.hpd_cable[n] ? HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK : 0) |
           (S.hpd_delayed[n] ? HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK : 0);
}
static void hpd_cable(uint32_t n, int on)
{
    if (!(S.hpd_model & (1u << n))) {
        S.hpd_model |= 1u << n;
        S.hpd_delayed[n] = (S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, n)] &
                            HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK) ? 1 : 0;
        S.hpd_cable[n] = S.hpd_delayed[n];
    }
    if (S.hpd_cable[n] != (on ? 1 : 0)) {
        S.hpd_cable[n] = on ? 1 : 0;
        S.hpd_since[n] = S.now_us;
    }
    hpd_filter_eval(n);
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
                /* the DP sink's view of its transmitter */
                if (S.dps.be >= 0 && c[4] == (uint8_t)S.dps.be) {
                    if (c[5] == RD_ATOM_TX_ENABLE) {
                        S.dps.phy_on = 1;
                        S.dps.lanes = c[7];
                        S.dps.symclk_10khz = (uint32_t)c[8] | (uint32_t)c[9] << 8 | (uint32_t)c[10] << 16 |
                                             (uint32_t)c[11] << 24;
                        S.dps.cr = S.dps.eq = 0;
                    } else if (c[5] == RD_ATOM_TX_DISABLE) {
                        S.dps.phy_on = 0;
                        S.dps.cr = S.dps.eq = 0;
                    } else if (c[5] == RD_ATOM_TX_SETUP_VSEMPH) {
                        S.dps.vs = c[6] & 3;
                        S.dps.pe = (c[6] >> 3) & 3;
                        S.dps.vsemph++;
                    }
                }
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

/* the sparse DPCD address space the sink answers */
static uint8_t *dpcd_at(uint32_t a)
{
    if (a < sizeof(S.dpcd))
        return &S.dpcd[a];
    if (a >= 0x2200 && a < 0x2200 + sizeof(S.dpcd_ext))
        return &S.dpcd_ext[a - 0x2200];
    if (a >= 0xf0000 && a < 0xf0000 + sizeof(S.dpcd_lttpr))
        return &S.dpcd_lttpr[a - 0xf0000];
    return NULL;
}

#define DPR(reg, n) (DCN_B2 + reg + (regDP1_DP_LINK_CNTL - regDP0_DP_LINK_CNTL) * (n))
#define DIGN(reg, n) (DCN_B2 + reg + RD_CLONE_DIGFE_STRIDE * (n))

/* the sink's lane status and adjust requests (DPCD 0x202..0x207) for what
 * the source drives now */
static void dp_sink_eval(void)
{
    uint8_t *d = S.dpcd, adj, nib;
    uint32_t be, lanes = d[0x101] & 0x1f, rate = d[0x100], pat = d[0x102] & 0xf, sel, i;
    int alive, done;
    if (S.dps.be < 0)
        return;
    be = (uint32_t)S.dps.be;
    S.dps.evals++;
    sel = S.reg[DPR(regDP0_DP_DPHY_TRAINING_PATTERN_SEL, be)] & 3;
    done = (S.reg[DPR(regDP0_DP_LINK_CNTL, be)] & DP0_DP_LINK_CNTL__DP_LINK_TRAINING_COMPLETE_MASK) != 0;
    alive = S.dps.phy_on && lanes && S.dps.lanes >= lanes && S.dps.symclk_10khz == rate * 2700 && d[0x600] == 1 &&
            (S.reg[DIGN(regDIG0_DIG_BE_EN_CNTL, be)] & DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK) &&
            !(S.reg[DIGN(regDIG0_DIG_BE_CNTL, be)] & DIG0_DIG_BE_CNTL__DIG_MODE_MASK);
    adj = (uint8_t)(S.dps.vs | S.dps.pe << 2);
    if (pat && (RD_DP_LANE_VS(d[0x103]) != S.dps.vs || RD_DP_LANE_PE(d[0x103]) != S.dps.pe))
        S.dps.lane_mismatch++;   /* DPCD lane set and PHY disagree */
    if (!alive) {
        S.dps.cr = S.dps.eq = 0;
    } else if (pat == RD_DPCD_TRAINING_PATTERN_1) {
        S.dps.eq = 0;
        S.dps.cr = sel == 0 && !done && rate != S.dps.fail_rate && S.dps.vs >= S.dps.need_vs;
        if (!S.dps.cr)
            adj = (uint8_t)((S.dps.vs < 3 ? S.dps.vs + 1 : 3) | S.dps.pe << 2);
    } else if (pat == RD_DPCD_TRAINING_PATTERN_2 || pat == RD_DPCD_TRAINING_PATTERN_3 ||
               pat == RD_DPCD_TRAINING_PATTERN_4) {
        uint32_t want = pat == RD_DPCD_TRAINING_PATTERN_2 ? 1 : pat == RD_DPCD_TRAINING_PATTERN_3 ? 2 : 3;
        S.dps.eq = S.dps.cr && sel == want && !done && S.dps.pe >= S.dps.need_pe;
        if (S.dps.cr && !S.dps.eq)
            adj = (uint8_t)(S.dps.vs | (S.dps.pe < S.dps.need_pe ? S.dps.pe + 1 : S.dps.pe) << 2);
    }
    /* pattern 0 (idle / video): the lock holds while the link stays up */
    if (adj != (uint8_t)(S.dps.vs | S.dps.pe << 2))
        S.dps.adjust_reqs++;
    nib = (uint8_t)((S.dps.cr ? RD_DP_LANE_CR_DONE : 0) |
                    (S.dps.cr && S.dps.eq ? RD_DP_LANE_CHANNEL_EQ_DONE | RD_DP_LANE_SYMBOL_LOCKED : 0));
    d[0x202] = d[0x203] = 0;
    for (i = 0; i < 4; i++)
        if (i < lanes)
            d[0x202 + i / 2] |= (uint8_t)(nib << (4 * (i % 2)));
    d[0x204] = S.dps.cr && S.dps.eq ? RD_DP_INTERLANE_ALIGN_DONE : 0;
    d[0x206] = d[0x207] = (uint8_t)(adj | adj << 4);
    if (S.dps.cr && S.dps.eq && pat) {
        S.dps.trained_rate = rate;
        S.dps.trained_lanes = lanes;
    }
}

/* DPCD writes the link layer reacts to */
static void dp_sink_written(uint32_t addr, uint32_t len)
{
    if (S.dps.be < 0)
        return;
    if (addr <= 0x101 && addr + len > 0x100)
        S.dps.cr = S.dps.eq = 0;            /* new rate / lane count: lock lost */
    if (addr <= 0x600 && addr + len > 0x600 && S.dpcd[0x600] != 1)
        S.dps.cr = S.dps.eq = 0;            /* D3 */
    if (addr <= 0x102 && addr + len > 0x102) {
        uint8_t p = S.dpcd[0x102] & 0xf;
        if (p == RD_DPCD_TRAINING_PATTERN_1)
            S.dps.tps1_sets++;
        else if (p)
            S.dps.eq_pattern = p;
    }
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
        if (addr <= 0x207 && addr + len > 0x200)
            dp_sink_eval();
        for (i = 0; i < len && (addr + i < sizeof(S.dpcd) || S.dps.be >= 0); i++) {
            uint8_t *b = dpcd_at(addr + i);
            S.aux_rx[1 + i] = b ? *b : 0;
        }
        S.aux_nrx = 1 + i;
        break;
    case RD_AUX_DP_WRITE:
        if (wr != 4 + len) { S.aux_bad++; }
        for (i = 0; i < len && (addr + i < sizeof(S.dpcd) || S.dps.be >= 0); i++) {
            uint8_t *b = dpcd_at(addr + i);
            if (b)
                *b = S.aux_tx[4 + i];
        }
        S.aux_nrx = 1;
        dp_sink_written(addr, len);
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
    memset(S.uq, 0, sizeof(S.uq));   /* the MES forgets its queues */
    memset(&S.smu, 0, sizeof(S.smu));
    S.nsmn = 0;
    *smn_slot(SMN_ROM_OFFSET_CTRL, 1) = ROM_STRAP;
    S.sos_alive = 0; S.sos_boot_at = 0; S.rptr_dw = 0; S.ring_mc = 0;
    S.smu_loaded = 0; S.tmr_setup = 0; S.toc_loaded = 0; S.autoload = 0;
    S.smu_after_tmr = 0;
    memset(S.ta_live, 0, sizeof(S.ta_live));   /* the sOS and its TAs are gone */
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
    { 131, regCP_HQD_VMID }, { 132, regCP_HQD_PERSISTENT_STATE }, { 133, regCP_HQD_PIPE_PRIORITY },
    { 134, regCP_HQD_QUEUE_PRIORITY }, { 136, regCP_HQD_PQ_BASE },
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
/* the IH writes through MMHUB VMID 0: VRAM by MC address, or a GART page
 * (system memory) */
static uint8_t *ih_mem(uint64_t mc, uint64_t len)
{
    uint8_t *p = mc_ptr(mc, len);
    if (!p && (mc & 0xfff) + len <= 4096)
        p = gart_page(mc);
    return p;
}

/* ring 1 registers (osssys_6_0_0_offset.h) */
#define SIM_IH_RB_CNTL_RING1   0x008c
#define SIM_IH_RB_BASE_RING1   0x008d
#define SIM_IH_RB_BASE_HI_RING1 0x008e
#define SIM_IH_RB_RPTR_RING1   0x008f
#define SIM_IH_RB_WPTR_RING1   0x0090
#define SIM_IH_DOORBELL_RPTR_RING1 0x0093

/* one IV into ring 0 or 1; returns the new wptr, ~0 when dropped */
static uint32_t ih_ring_put(int ring, const uint32_t iv[8])
{
    uint32_t rcntl = ring ? SIM_IH_RB_CNTL_RING1 : regIH_RB_CNTL;
    uint32_t cntl = S.reg[OSS_BASE0 + rcntl];
    uint64_t base = ((uint64_t)S.reg[OSS_BASE0 + (ring ? SIM_IH_RB_BASE_RING1 : regIH_RB_BASE)] << 8) |
                    ((uint64_t)S.reg[OSS_BASE0 + (ring ? SIM_IH_RB_BASE_HI_RING1 : regIH_RB_BASE_HI)] << 40);
    uint64_t wb = S.reg[OSS_BASE0 + regIH_RB_WPTR_ADDR_LO] |
                  ((uint64_t)S.reg[OSS_BASE0 + regIH_RB_WPTR_ADDR_HI] << 32);
    uint32_t size = 4u << ((cntl & IH_RB_CNTL__RB_SIZE_MASK) >> IH_RB_CNTL__RB_SIZE__SHIFT);
    uint32_t rw = ring ? SIM_IH_RB_WPTR_RING1 : regIH_RB_WPTR;
    uint32_t wreg = S.reg[OSS_BASE0 + rw], wptr = wreg & (size - 1);
    uint32_t rptr = S.reg[OSS_BASE0 + (ring ? SIM_IH_RB_RPTR_RING1 : regIH_RB_RPTR)] & (size - 1);
    uint8_t *p = (cntl & IH_RB_CNTL__RB_ENABLE_MASK) ? ih_mem(base + wptr, 32) : NULL;
    if (!p) {
        S.ih_dropped++;
        return ~0u;
    }
    if (cntl & IH_RB_CNTL__MC_SNOOP_MASK)
        S.ih_snooped++;
    memcpy(p, iv, 32);
    wptr = (wptr + 32) & (size - 1);
    wreg = wptr | (wreg & IH_RB_WPTR__RB_OVERFLOW_MASK) | (wptr == rptr ? IH_RB_WPTR__RB_OVERFLOW_MASK : 0);
    S.reg[OSS_BASE0 + rw] = wreg;
    if (!ring && (cntl & IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK) && ih_mem(wb, 4))
        memcpy(ih_mem(wb, 4), &wreg, 4);
    S.ih_ivs++;
    if (!ring && (cntl & IH_RB_CNTL__ENABLE_INTR_MASK))
        S.ih_msi++;
    return wptr;
}

static void ih_push_iv(uint32_t client, uint32_t src, uint32_t ring_id, uint32_t vmid,
                       uint32_t pasid, uint32_t sd0, uint32_t sd1)
{
    uint32_t iv[8] = { client | (src << 8) | (ring_id << 16) | (vmid << 24), 0, 0, pasid, sd0, sd1, 0, 0 };
    uint32_t cfg = S.reg[OSS_BASE0 + regIH_RING1_CLIENT_CFG_DATA];
    /* IH_RING1_CLIENT_CFG: this client / source goes to ring 1, whose wptr
     * the IH then reports with a self IV on ring 0 */
    if ((S.reg[OSS_BASE0 + SIM_IH_RB_CNTL_RING1] & IH_RB_CNTL__RB_ENABLE_MASK) &&
        (cfg & IH_RING1_CLIENT_CFG_DATA__SOURCE_ID_MATCH_ENABLE_MASK) &&
        (cfg & IH_RING1_CLIENT_CFG_DATA__CLIENT_ID_MASK) == client &&
        ((cfg & IH_RING1_CLIENT_CFG_DATA__SOURCE_ID_MASK) >> IH_RING1_CLIENT_CFG_DATA__SOURCE_ID__SHIFT) == src) {
        uint32_t w1 = ih_ring_put(1, iv);
        if (w1 != ~0u) {
            uint32_t self[8] = { RD_IH_CLIENT_IH | (0u << 8) | (1u << 16), 0, 0, 0, w1, 0, 0, 0 };
            S.ih_ring1_ivs++;
            ih_ring_put(0, self);
        }
        return;
    }
    ih_ring_put(0, iv);
}

/* CP (ME/PFP) model for the kernel gfx ring: SET_UCONFIG_REG, the clear
 * state packets, RELEASE_MEM fence writes, WRITE_DATA, WAIT_REG_MEM, the
 * job framing packets and INDIRECT_BUFFER through the VMID's page tables */
static void sim_wr(void *ctx, uint64_t off, uint32_t v);
static uint32_t vcn_dpg_sram_load(uint32_t inst, uint64_t mc, uint32_t size);
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
        /* a valid PDE with PDE_PTE (bit 54) maps a 1 GiB / 2 MiB page */
        if (l < 3 && (e & 1) && (e & (1ull << 54))) {
            uint64_t size = 1ull << shift[l];
            uint64_t pa = (e & 0x0000FFFFFFFFF000ull & ~(size - 1)) | (va & (size - 1));
            S.vm_huge_walks++;
            if (pa + len > BAR_LEN)
                return NULL;
            return S.vram + pa;
        }
    }
    if (!(e & 1) || (e & 0x0000FFFFFFFFF000ull) + 4096 > BAR_LEN)
        return NULL;
    return S.vram + (e & 0x0000FFFFFFFFF000ull) + (va & 4095);
}

/* memory a CP packet names: MC addresses on the kernel rings, VAs of the
 * process VM on a user queue (the MES runs it in the process's VMID) */
static uint8_t *cp_mem(uint64_t a, uint64_t len)
{
    if (S.cp_cur_uq)
        return vm_ptr(S.uq[S.cp_cur_uq - 1].vmid, a, len);
    return mc_ptr(a, len);
}

static void uq_eop(void);

/* run packets [rptr, end) of buffer 'buf' (index mask 'mask'); returns new rptr */
static uint64_t cp_exec(const uint32_t *buf, uint64_t mask, uint64_t rptr, uint64_t end, int in_ib)
{
#define PK(i) buf[(rptr + (i)) & mask]
    while ((rptr & mask) != (end & mask)) {
        uint32_t h = PK(0), type = h >> 30, op = (h >> 8) & 0xff, cnt = (h >> 16) & 0x3fff;
        S.cp_pkts++;
        if (S.cp_cur_uq && !in_ib)
            S.cp_uq_pos = rptr + (type == 3 ? 2 + cnt : 1);
        if (type != 3) { S.cp_bad++; rptr++; continue; }
        if (op == PACKET3_NOP && cnt == 0x3FFF) {   /* ring->funcs->nop: one dword */
            S.cp_nop1++;
            rptr++;
            continue;
        }
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
        case PACKET3_COND_EXEC: {
            /* ADDR_LO, ADDR_HI, 0, EXEC_COUNT: the next EXEC_COUNT dwords
             * are discarded while the dword at ADDR reads 0 */
            uint8_t *p = cnt == 3 && !(PK(1) & 3) ? mc_ptr(PK(1) | ((uint64_t)PK(2) << 32), 4) : NULL;
            uint32_t v, skip = PK(4);
            if (!p || in_ib) { S.cp_bad++; break; }
            memcpy(&v, p, 4);
            if (!v) {
                S.cp_cond_skips++;
                rptr += skip;
            }
            break;
        }
        case PACKET3_SET_Q_PREEMPTION_MODE:
            /* shadow VA, GDS VA, CSA VA (lo/hi each), IB_VMID, INIT_SHADOW_MEM */
            if (cnt != 7 || in_ib) { S.cp_bad++; break; }
            for (uint32_t i = 0; i < 8; i++)
                S.cp_qmode_hist[S.cp_qmode & 3][i] = PK(1 + i);
            S.cp_qmode++;
            break;
        case PACKET3_FRAME_CONTROL:
            if (PK(1) >> 28) S.cp_frame_end++; else S.cp_frame_begin++;
            break;
        case PACKET3_WRITE_DATA: {
            uint32_t dst = (PK(1) >> 8) & 0xf;
            for (uint32_t i = 0; i + 3 < cnt + 1; i++) {
                if (dst == 0) {
                    sim_wr(NULL, 4ull * (PK(2) + i), PK(4 + i));
                } else if (dst == 5) {
                    uint8_t *p = cp_mem((PK(2) | ((uint64_t)PK(3) << 32)) + 4ull * i, 4);
                    if (p) memcpy(p, &PK(4 + i), 4); else S.cp_bad++;
                } else {
                    S.cp_bad++;
                }
            }
            break;
        }
        case PACKET3_WAIT_REG_MEM: {
            uint32_t ctl = PK(1), ref = PK(4), mask_ = PK(5), v;
            if (ctl & (1u << 4)) {                        /* poll memory (equal) */
                uint8_t *mp = (ctl & (1u << 6)) ? NULL : cp_mem(PK(2) | ((uint64_t)PK(3) << 32), 4);
                if (!mp) { S.cp_bad++; break; }
                memcpy(&v, mp, 4);
                S.cp_mem_polls++;
                if ((v & mask_) != (ref & mask_))
                    S.cp_wait_stuck++;
                break;
            }
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
            if (S.cp_cur_uq)
                vmid = S.uq[S.cp_cur_uq - 1].vmid;   /* a user queue's IBs run in its VMID */
            static uint32_t ib[4096];
            if (in_ib || ndw > 4096) { S.cp_bad++; break; }
            for (i = 0; i < ndw; i++) {
                /* VMID 0 (kernel IBs, amdgpu_ib_get): MC addresses */
                uint8_t *p = (vmid || S.cp_cur_uq) ? vm_ptr(vmid, va + 4ull * i, 4) : mc_ptr(va + 4ull * i, 4);
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
            uint8_t *p = cp_mem(a, sel == 2 ? 8 : 4);
            if (p && sel == 1) memcpy(p, &PK(5), 4);
            else if (p && sel == 2 && !(a & 7)) { memcpy(p, &PK(5), 4); memcpy(p + 4, &PK(6), 4); }
            else S.cp_bad++;
            if (((PK(2) >> 24) & 7) == 2 && S.cp_cur_uq) {
                uq_eop();
            } else if (((PK(2) >> 24) & 7) == 2) {
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
        default: {
            /* an opcode the CP does not know: CP_BAD_OPCODE_ERROR when the
             * engine's OPCODE_ERROR_INT_ENABLE is set */
            uint32_t en = S.cp_cur_me == 0
                ? S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK
                : S.cp_cur_me == 1
                ? S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + S.cp_cur_pipe] & CP_ME1_PIPE0_INT_CNTL__OPCODE_ERROR_INT_ENABLE_MASK
                : 0;
            S.cp_bad++;
            if (en && !S.cp_cur_uq) {
                S.cp_fault_ivs++;
                ih_push(RD_IH_CLIENT_GRBM_CP, RD_IH_SRC_CP_BAD_OPCODE,
                        (S.cp_cur_queue << 4) | (S.cp_cur_me << 2) | S.cp_cur_pipe);
            }
            break;
        }
        }
        rptr += 2 + cnt;
    }
#undef PK
    return rptr;
}

/* ---- MES user queues ---- */

static uint64_t q64(const uint32_t *f, unsigned i) { return f[i] | ((uint64_t)f[i + 1] << 32); }

/* ADD_QUEUE: the MQD has to describe the queue the frame names (its own
 * address, the doorbell), the process gets a VMID of the GFXHUB mask and
 * the page tables of the frame; a rejected frame gets no status fence */
static int uq_add(const uint32_t *f)
{
    uint32_t type = f[27], db = f[18], pid = f[1], vmid = 0, taken = 0, slot = 8, ngfx = 0, i;
    uint64_t mqd = q64(f, 19), gang = q64(f, 14), pd = q64(f, 2);
    uint32_t dist = regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
    const uint32_t *m = (const uint32_t *)mc_ptr(mqd, type == 2 ? 512 : 2048);
    struct sim_uq *u;
    if (!S.mes_sched.hw_rsrc || type > 2 || !pid || !gang || (gang & 4095) || !q64(f, 10) || !m || !(pd & 1))
        return 0;
    if (type == 1 && (m[0] != 0xC0310800u || m[128] != (uint32_t)mqd ||
                      !(m[143] & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK) ||
                      ((m[143] & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) != db))
        return 0;
    if (type == 0 && (m[128] != (uint32_t)mqd || m[130] != 1 ||
                      !(m[143] & CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK) ||
                      ((m[143] & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) != db))
        return 0;
    if (type == 2 && (!(m[17] & SDMA0_QUEUE0_DOORBELL__ENABLE_MASK) ||
                      ((m[19] & SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK) >> 2) != db))
        return 0;
    for (i = 0; i < 8; i++) {
        if (!S.uq[i].used) {
            if (slot == 8)
                slot = i;
            continue;
        }
        if (S.uq[i].doorbell == db)
            return 0;                               /* that doorbell is mapped already */
        if (S.uq[i].process_id == pid)
            vmid = S.uq[i].vmid;
        else
            taken |= 1u << S.uq[i].vmid;
        ngfx += S.uq[i].type == type;
    }
    for (i = 1; !vmid && i < 16; i++)
        if ((S.mes_sched.hw[2] & (1u << i)) && !(taken & (1u << i)))
            vmid = i;
    if (!vmid || slot == 8)
        return 0;
    u = &S.uq[slot];
    memset(u, 0, sizeof(*u));
    u->used = 1;
    u->type = type;
    u->doorbell = db;
    u->process_id = pid;
    u->vmid = vmid;
    u->mqd = mqd;
    u->pd = pd;
    u->gang_ctx = gang;
    u->proc_ctx = q64(f, 10);
    u->wptr_addr = q64(f, 21);
    u->queue_size_dw = f[29];
    if (type == 0) {           /* the gfx pipe's queues past the kernel's (gfx_hqd_mask) */
        u->me = 0; u->pipe = 0; u->queue = 1 + ngfx;
        u->fence_addr = q64(m, 510);
    } else if (type == 1) {    /* queues 2-3 of the MEC1 pipes (compute_hqd_mask 0xc) */
        u->me = 1; u->pipe = slot % 4; u->queue = 2 + (slot / 4) % 2;
        u->fence_addr = q64(m, 446);
    } else {                   /* SDMA0 queues past the kernel's (sdma_hqd_mask 0xfc) */
        u->me = 0; u->pipe = 0; u->queue = 2 + ngfx;
        u->fence_addr = q64(m, 43);
    }
    S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + dist * vmid] = (uint32_t)pd;
    S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 + dist * vmid] = (uint32_t)(pd >> 32);
    S.uq_adds++;
    return 1;
}

/* REMOVE_QUEUE: by doorbell, with the gang context it was added with */
static int uq_remove(const uint32_t *f)
{
    for (uint32_t i = 0; i < 8; i++)
        if (S.uq[i].used && S.uq[i].doorbell == f[1] && S.uq[i].gang_ctx == q64(f, 2)) {
            memset(&S.uq[i], 0, sizeof(S.uq[i]));
            S.uq_removes++;
            return 1;
        }
    return 0;
}

/* a gfx / compute user queue interrupts: rptr to the fence address, EOP IV */
static void uq_eop(void)
{
    struct sim_uq *u = &S.uq[S.cp_cur_uq - 1];
    uint8_t *fa = mc_ptr(u->fence_addr, 8);
    uint32_t en = u->me == 0
        ? S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK
        : S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + u->pipe] & CP_ME1_PIPE0_INT_CNTL__TIME_STAMP_INT_ENABLE_MASK;
    u->eops++;
    S.uq_eops++;
    if (fa)
        memcpy(fa, &S.cp_uq_pos, 8);
    else
        S.uq_faults++;
    if (en)
        ih_push_iv(RD_IH_CLIENT_GRBM_CP, RD_IH_SRC_CP_EOP, (u->queue << 4) | (u->me << 2) | u->pipe,
                   u->vmid, u->process_id, u->doorbell, 0);
}

static int uq_rd(const struct sim_uq *u, uint64_t va, uint32_t *v)
{
    uint8_t *p = vm_ptr(u->vmid, va, 4);
    if (!p)
        return 0;
    memcpy(v, p, 4);
    return 1;
}

static int uq_wr(const struct sim_uq *u, uint64_t va, const void *v, uint32_t len)
{
    uint8_t *p = vm_ptr(u->vmid, va, len);
    if (!p)
        return 0;
    memcpy(p, v, len);
    return 1;
}

/* an SDMA user queue: NOP, WRITE_LINEAR, FENCE (+ the user fence), TRAP */
static void uq_run_sdma(struct sim_uq *u, const uint32_t *m, uint64_t wptr)
{
    uint64_t base = q64(m, 1) << 8, rwb = ((uint64_t)m[7] << 32) | m[8];
    uint32_t size = 4u << ((m[0] & SDMA0_QUEUE0_RB_CNTL__RB_SIZE_MASK) >> SDMA0_QUEUE0_RB_CNTL__RB_SIZE__SHIFT);
#define URD(k, v) uq_rd(u, base + ((u->rptr + 4ull * (k)) & (size - 1)), (v))
    while ((u->rptr & (size - 1)) != (wptr & (size - 1))) {
        uint32_t h, op, n, a0, a1, cnt, v, k;
        if (!URD(0, &h)) { S.uq_faults++; break; }
        op = h & 0xff;
        if (op == SDMA_OP_NOP) {
            n = 1 + ((h >> 16) & 0x3fff);
        } else if (op == SDMA_OP_WRITE && ((h >> 8) & 0xff) == 0) {
            if (!URD(1, &a0) || !URD(2, &a1) || !URD(3, &cnt)) { S.uq_faults++; break; }
            cnt = (cnt & 0xfffff) + 1;
            for (k = 0; k < cnt; k++)
                if (!URD(4 + k, &v) || !uq_wr(u, (a0 | ((uint64_t)a1 << 32)) + 4ull * k, &v, 4))
                    S.uq_faults++;
            n = 4 + cnt;
        } else if (op == SDMA_OP_FENCE) {
            uint64_t pos = u->rptr + 16;
            uint8_t *fa = mc_ptr(u->fence_addr, 8);
            if (!URD(1, &a0) || !URD(2, &a1) || !URD(3, &v) || !uq_wr(u, a0 | ((uint64_t)a1 << 32), &v, 4))
                S.uq_faults++;
            if (fa)
                memcpy(fa, &pos, 8);
            else
                S.uq_faults++;
            u->eops++;
            S.uq_eops++;
            if (S.reg[GC_BASE0 + regSDMA0_CNTL] & SDMA0_CNTL__TRAP_ENABLE_MASK)
                ih_push_iv(RD_IH_CLIENT_GFX, RD_IH_SRC_SDMA_FENCE, u->queue, u->vmid, u->process_id,
                           u->doorbell << 2, 0);
            n = 4;
        } else if (op == SDMA_OP_TRAP) {
            if (S.reg[GC_BASE0 + regSDMA0_CNTL] & SDMA0_CNTL__TRAP_ENABLE_MASK)
                ih_push(RD_IH_CLIENT_GFX, RD_IH_SRC_SDMA_TRAP, u->queue);
            n = 2;
        } else {
            S.uq_faults++;
            break;
        }
        u->rptr += 4ull * n;
    }
#undef URD
    if (!uq_wr(u, rwb, &u->rptr, 8))
        S.uq_faults++;
}

/* a doorbell of a MES-mapped user queue: run it up to 'wptr' */
static void uq_run(uint32_t i, uint64_t wptr)
{
    static uint32_t ring[65536];
    struct sim_uq *u = &S.uq[i];
    const uint32_t *m = (const uint32_t *)mc_ptr(u->mqd, u->type == 2 ? 512 : 2048);
    uint64_t base, rwb;
    uint32_t ndw, k;
    u->runs++;
    S.uq_runs++;
    if (!m) { S.uq_faults++; return; }
    if (u->type == 2) {
        uq_run_sdma(u, m, wptr);
        return;
    }
    ndw = u->type == 0 ? 2u << ((m[145] & CP_GFX_HQD_CNTL__RB_BUFSZ_MASK) >> CP_GFX_HQD_CNTL__RB_BUFSZ__SHIFT)
                       : 2u << ((m[145] & CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK) >> CP_HQD_PQ_CONTROL__QUEUE_SIZE__SHIFT);
    base = q64(m, 136) << 8;
    rwb = m[139] | ((uint64_t)(m[140] & 0xffff) << 32);
    if (ndw > 65536) { S.uq_faults++; return; }
    for (k = 0; k < ndw; k++)          /* the ring, through the process VM */
        if (!uq_rd(u, base + 4ull * k, &ring[k])) { S.uq_faults++; return; }
    S.cp_cur_uq = (int)i + 1;
    S.cp_cur_me = u->me; S.cp_cur_pipe = u->pipe; S.cp_cur_queue = u->queue;
    u->rptr = cp_exec(ring, ndw - 1, u->rptr, wptr, 0);
    S.cp_cur_uq = 0;
    S.cp_cur_me = 0; S.cp_cur_pipe = 0; S.cp_cur_queue = 0;
    if (!uq_wr(u, rwb & ~7ull, &u->rptr, 8))
        S.uq_faults++;
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
        } else if (op == RD_MES_SCH_API_ADD_QUEUE) {
            if (!uq_add(f)) { S.uq_bad++; continue; }
            st = RD_MES_ADD_QUEUE_STATUS_DW;
        } else if (op == RD_MES_SCH_API_REMOVE_QUEUE) {
            if (!uq_remove(f)) { S.uq_bad++; continue; }
            st = RD_MES_REMOVE_QUEUE_STATUS_DW;
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
    for (uint32_t i = 0; i < 8; i++)
        if (S.uq[i].used && S.uq[i].doorbell == index) {
            uq_run(i, v);
            return;
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
    case GFX_CMD_ID_LOAD_TA:
    case GFX_CMD_ID_LOAD_ASD: {
        /* the image comes through fw_pri (4 KiB aligned); a TA has a 16 KiB
         * shared buffer, ASD none; both need the TMR */
        const struct psp_gfx_cmd_load_ta *t = &cmd->cmd.cmd_load_ta;
        uint64_t app = t->app_phy_addr_lo | ((uint64_t)t->app_phy_addr_hi << 32);
        uint64_t sh = t->cmd_buf_phy_addr_lo | ((uint64_t)t->cmd_buf_phy_addr_hi << 32);
        int asd = cmd->cmd_id == GFX_CMD_ID_LOAD_ASD;
        unsigned k;
        if (!S.tmr_setup || !mc_ptr(app, t->app_len) || (app & 4095) || !t->app_len ||
            (asd ? (t->cmd_buf_len != 0) : (t->cmd_buf_len != 0x4000 || (sh & 4095) || !mc_ptr(sh, 0x4000)))) {
            S.ta_bad++;
            cmd->resp.status = 0xFFFF0003;
            break;
        }
        for (k = 0; k < 8 && S.ta_live[k]; k++)
            ;
        if (k == 8) { S.ta_bad++; cmd->resp.status = 0xFFFF0004; break; }
        S.ta_live[k] = 0x100 + ++S.ta_next_session;
        S.ta_last_len[k] = t->app_len;
        cmd->resp.session_id = S.ta_live[k];
        if (asd) S.asd_loads++; else S.ta_loads++;
        break;
    }
    case GFX_CMD_ID_UNLOAD_TA:
    case GFX_CMD_ID_INVOKE_CMD: {
        uint32_t sid = cmd->cmd_id == GFX_CMD_ID_UNLOAD_TA ? cmd->cmd.cmd_unload_ta.session_id
                                                           : cmd->cmd.cmd_invoke_cmd.session_id;
        unsigned k;
        for (k = 0; k < 8 && (!sid || S.ta_live[k] != sid); k++)
            ;
        if (k == 8) { S.ta_bad++; cmd->resp.status = 0xFFFF0005; break; }
        if (cmd->cmd_id == GFX_CMD_ID_UNLOAD_TA) {
            S.ta_live[k] = 0;
            S.ta_unloads++;
        } else {
            S.ta_invokes++;
            S.ta_last_cmd = cmd->cmd.cmd_invoke_cmd.ta_cmd_id;
        }
        break;
    }
    case GFX_CMD_ID_DESTROY_TMR:
        if (!S.tmr_setup) { S.ta_bad++; cmd->resp.status = 0xFFFF0006; break; }
        S.tmr_setup = 0;
        S.tmr_destroys++;
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
        if (S.nipfw < 256)
            S.ip_fw_types[S.nipfw++] = t;
        if (t == GFX_FW_TYPE_VCN0_RAM || t == GFX_FW_TYPE_VCN1_RAM)
            cmd->resp.status = vcn_dpg_sram_load(t == GFX_FW_TYPE_VCN1_RAM, mc, cmd->cmd.cmd_load_ip_fw.fw_size);
        if (t == GFX_FW_TYPE_VCN || t == GFX_FW_TYPE_VCN1) {
            /* psp: the image now lives in the TMR, answered in fw_addr */
            uint32_t vi = t == GFX_FW_TYPE_VCN1;
            S.vcn.fw_size[vi] = cmd->cmd.cmd_load_ip_fw.fw_size;
            S.vcn.tmr_fw[vi] = S.tmr_mc + VCN_TMR_FW_OFF(vi);
            cmd->resp.fw_addr_lo = (uint32_t)S.vcn.tmr_fw[vi];
            cmd->resp.fw_addr_hi = (uint32_t)(S.vcn.tmr_fw[vi] >> 32);
        }
        if (t == GFX_FW_TYPE_DMUB) {
            /* the PSP puts the image behind CW0 and opens the window */
            S.dmub.psp_loaded = 1;
            *R(DCN_B2 + regDMCUB_REGION3_CW0_TOP_ADDRESS) |= DMCUB_REGION3_CW0_TOP_ADDRESS__DMCUB_REGION3_CW0_ENABLE_MASK;
        }
        if (t == GFX_FW_TYPE_REG_LIST) {
            S.rl_loads++;
            S.rl_before_tmr += !S.tmr_setup;
        }
        if (t == GFX_FW_TYPE_SMU) {
            uint32_t *fl = smn_slot(RD_SMN_MP1_FIRMWARE_FLAGS, 1);
            S.smu_after_tmr = S.tmr_setup;
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

/* MPCC output gamma (OGAM): the two LUT RAMs behind MPCC_OGAM_LUT_INDEX /
 * _DATA (auto-increment, WRITE_COLOR_MASK 4 R / 2 G / 1 B, HOST_SEL RAM A
 * or B), MPCC_OGAM_MODE / _SELECT latching into _CURRENT when the OTG
 * master update lock is released, and the OGAM memory power state: LUT
 * writes while it is not powered (MEM_PWR_STATE != 0) are lost. */
#define OGAM_R(reg, n) (DCN_B3 + reg + (regMPCC_OGAM1_MPCC_OGAM_CONTROL - regMPCC_OGAM0_MPCC_OGAM_CONTROL) * (n))
#define MPCC_R(reg, n) (DCN_B3 + reg + RD_CLONE_MPCC_STRIDE * (n))
static struct {
    uint32_t ram[RD_DCN_MAX_PIPES][2][3][512];   /* [mpcc][A/B][R/G/B] */
    uint32_t index[RD_DCN_MAX_PIPES];
    unsigned writes, dropped, latches;
    int      pwr_stuck;                          /* the memory never powers up */
} ogam;

static void ogam_latch(void)
{
    for (uint32_t n = 0; n < RD_DCN_MAX_PIPES; n++) {
        uint32_t *c = &S.reg[OGAM_R(regMPCC_OGAM0_MPCC_OGAM_CONTROL, n)];
        uint32_t mode = (*c & MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_MASK) >>
                        MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE__SHIFT;
        uint32_t sel = (*c & MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT_MASK) >>
                       MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT__SHIFT;
        *c = (*c & ~(MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_CURRENT_MASK |
                     MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT_CURRENT_MASK)) |
             mode << MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_CURRENT__SHIFT |
             sel << MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT_CURRENT__SHIFT;
    }
    ogam.latches++;
}

/* 1 when the write was an OGAM register the model handled */
static int ogam_wr(uint32_t dw, uint32_t v)
{
    const uint32_t cur = MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_CURRENT_MASK |
                         MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT_CURRENT_MASK;
    for (uint32_t o = 0; o < RD_DCN_MAX_PIPES; o++)
        if (dw == OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, o) && !(v & 1)) {
            *R(dw) = v;
            ogam_latch();
            return 1;
        }
    for (uint32_t n = 0; n < RD_DCN_MAX_PIPES; n++) {
        uint32_t lc = S.reg[OGAM_R(regMPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, n)];
        if (dw == OGAM_R(regMPCC_OGAM0_MPCC_OGAM_CONTROL, n)) {
            *R(dw) = (v & ~cur) | (S.reg[dw] & cur);       /* _CURRENT: read only */
            return 1;
        }
        if (dw == OGAM_R(regMPCC_OGAM0_MPCC_OGAM_LUT_INDEX, n)) {
            ogam.index[n] = v & MPCC_OGAM0_MPCC_OGAM_LUT_INDEX__MPCC_OGAM_LUT_INDEX_MASK;
            *R(dw) = ogam.index[n];
            return 1;
        }
        if (dw == OGAM_R(regMPCC_OGAM0_MPCC_OGAM_LUT_DATA, n)) {
            uint32_t mask = lc & MPCC_OGAM0_MPCC_OGAM_LUT_CONTROL__MPCC_OGAM_LUT_WRITE_COLOR_MASK_MASK;
            uint32_t ram = (lc & MPCC_OGAM0_MPCC_OGAM_LUT_CONTROL__MPCC_OGAM_LUT_HOST_SEL_MASK) ? 1 : 0;
            ogam.writes++;
            if (S.reg[MPCC_R(regMPCC0_MPCC_MEM_PWR_CTRL, n)] & MPCC0_MPCC_MEM_PWR_CTRL__MPCC_OGAM_MEM_PWR_STATE_MASK) {
                ogam.dropped++;
            } else {
                for (uint32_t c = 0; c < 3; c++)
                    if (mask & (4u >> c))
                        ogam.ram[n][ram][c][ogam.index[n] & 511] = v & MPCC_OGAM0_MPCC_OGAM_LUT_DATA__MPCC_OGAM_LUT_DATA_MASK;
            }
            ogam.index[n] = (ogam.index[n] + 1) & 511;
            return 1;
        }
        if (dw == MPCC_R(regMPCC0_MPCC_MEM_PWR_CTRL, n)) {
            int on = (v & MPCC0_MPCC_MEM_PWR_CTRL__MPCC_OGAM_MEM_PWR_DIS_MASK) && !ogam.pwr_stuck;
            *R(dw) = (v & ~MPCC0_MPCC_MEM_PWR_CTRL__MPCC_OGAM_MEM_PWR_STATE_MASK) |
                     (on ? 0u : 1u << MPCC0_MPCC_MEM_PWR_CTRL__MPCC_OGAM_MEM_PWR_STATE__SHIFT);
            return 1;
        }
    }
    return 0;
}

/* what the OGAM of MPCC n does to the picture, for the OTG CRC: 0 in bypass */
static uint32_t ogam_crc(uint32_t n)
{
    uint32_t c = S.reg[OGAM_R(regMPCC_OGAM0_MPCC_OGAM_CONTROL, n)], h = 0;
    uint32_t ram = (c & MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT_CURRENT_MASK) ? 1 : 0;
    if (((c & MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_CURRENT_MASK) >>
         MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_CURRENT__SHIFT) != 2)
        return 0;
    for (uint32_t k = 0; k < 3; k++)
        for (uint32_t i = 0; i < RD_GAMMA_HW_POINTS; i++)
            h = (h ^ ogam.ram[n][ram][k][i]) * 16777619u;
    return h;
}

/* ---- VCN 4.0 (two instances) and JPEG 4.0 ----
 * Registers at the card's discovery bases (segment 1: 0x7E00 / 0x12000).
 * The PMFW powers an instance up and down (PowerUpVcn / PowerDownVcn,
 * PowerUpJpeg / PowerDownJpeg); writes to an instance's VCPU / ring
 * registers while it is powered down are counted as errors. UVD_PGFSM_CONFIG
 * moves the tiles' UVD_PGFSM_STATUS (1: on -> 0, 2: off -> 2). The VCPU
 * boots (UVD_STATUS bit 1) when BLK_RST is released with the clock on and
 * cache window 0 on the image the PSP placed in the TMR. Enabling RB1 checks
 * the fw_shared page behind the non-cached window (unified queue flag,
 * sq.is_enabled, the ring-reset handshake). The unified ring runs on the
 * doorbell (VCN_RB1_DB_CTRL) or UVD_RB_WPTR: IB (in VMID 0 by MC address,
 * other VMIDs through the MMHUB page tables), FENCE (32-bit write), TRAP
 * (IV client VCN / VCN1, src 119), END, NOP, REG_WRITE / REG_WAIT. IBs are
 * unified-queue packages: the single-queue checksum is verified, engine
 * info names encode or decode; encode session init / close and decode
 * create / destroy messages (through the decode buffer's message address)
 * are counted. The JRBC runs PACKETJ pairs: internal register writes
 * (GPCOM_CMD 8 writes GPCOM_DATA0 to the RB_MEM_WR address, the pitch
 * register reaches UVD_JPEG_PITCH), TYPE2 runs the IB (in the IB VMID),
 * TYPE7 raises the JPEG decode IV (src 153). */

static uint8_t *mm_vm_ptr(uint32_t vmid, uint64_t va, uint64_t len)
{
    static const uint32_t shift[4] = { 39, 30, 21, 12 };
    uint32_t dist = regMMVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
    uint64_t e;
    if (!vmid)
        return mc_ptr(va, len);
    e = S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + dist * vmid] |
        ((uint64_t)S.reg[MM_BASE0 + regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 + dist * vmid] << 32);
    va &= 0x0000FFFFFFFFFFFFull;
    if ((va & 4095) + len > 4096)
        return NULL;
    for (int l = 0; l < 4; l++) {
        uint64_t t = e & 0x0000FFFFFFFFF000ull;
        if (!(e & 1) || t + 4096 > BAR_LEN)
            return NULL;
        memcpy(&e, S.vram + t + 8 * ((va >> shift[l]) & 511), 8);
        /* a valid PDE with PDE_PTE (bit 54) maps a 1 GiB / 2 MiB page */
        if (l < 3 && (e & 1) && (e & (1ull << 54))) {
            uint64_t size = 1ull << shift[l];
            uint64_t pa = (e & 0x0000FFFFFFFFF000ull & ~(size - 1)) | (va & (size - 1));
            S.vm_huge_walks++;
            if (pa + len > BAR_LEN)
                return NULL;
            return S.vram + pa;
        }
    }
    if (!(e & 1) || (e & 0x0000FFFFFFFFF000ull) + 4096 > BAR_LEN)
        return NULL;
    return S.vram + (e & 0x0000FFFFFFFFF000ull) + (va & 4095);
}

static int vcn_rd32(uint32_t vmid, uint64_t va, uint32_t *v)
{
    uint8_t *p = mm_vm_ptr(vmid, va, 4);
    if (!p)
        return -1;
    memcpy(v, p, 4);
    return 0;
}

/* the instance of a VCN register, -1 for none */
static int vcn_inst_of(uint32_t dw, uint32_t *reg)
{
    for (int i = 0; i < 2; i++)
        if (dw >= VCN_B1(i) && dw < VCN_B1(i) + 0x800) {
            *reg = dw - VCN_B1(i);
            return i;
        }
    return -1;
}

static void vcn_run_ib(uint32_t inst, uint32_t vmid, uint64_t va, uint32_t ndw)
{
    uint32_t i = 0, sz, id, engine = 0, w[8];
    S.vcn.ibs++;
    while (i + 1 < ndw) {
        if (vcn_rd32(vmid, va + 4ull * i, &sz) || vcn_rd32(vmid, va + 4ull * (i + 1), &id)) {
            S.vcn.ib_faults++;
            return;
        }
        if (sz < 8 || (sz & 3))
            break;
        if (id == 0x30000002) {   /* single queue checksum over the packages that follow */
            uint32_t sum = 0, want, n, k, x;
            if (vcn_rd32(vmid, va + 4ull * (i + 2), &want) || vcn_rd32(vmid, va + 4ull * (i + 3), &n)) {
                S.vcn.ib_faults++;
                return;
            }
            for (k = 0; k < n; k++) {
                if (vcn_rd32(vmid, va + 4ull * (i + 4 + k), &x)) {
                    S.vcn.ib_faults++;
                    return;
                }
                sum += x;
            }
            if (sum != want)
                S.vcn.checksum_bad++;
        } else if (id == 0x30000001) {   /* engine info */
            if (vcn_rd32(vmid, va + 4ull * (i + 2), &engine)) {
                S.vcn.ib_faults++;
                return;
            }
        }
        if (engine == 2 && sz == 8) {   /* an encode op package */
            if (id == 0x08000001) S.vcn.enc_init++;
            if (id == 0x08000002) S.vcn.enc_close++;
        }
        if (engine == 3 && id == 0x00000001) {   /* the decode buffer: a message */
            uint32_t flag, hi, lo, k;
            uint64_t msg;
            if (vcn_rd32(vmid, va + 4ull * (i + 2), &flag) || vcn_rd32(vmid, va + 4ull * (i + 3), &hi) ||
                vcn_rd32(vmid, va + 4ull * (i + 4), &lo)) {
                S.vcn.ib_faults++;
                return;
            }
            msg = ((uint64_t)hi << 32) | lo;
            if (flag & 1) {
                for (k = 0; k < 8; k++)
                    if (vcn_rd32(vmid, msg + 4ull * k, &w[k])) {
                        S.vcn.ib_faults++;
                        return;
                    }
                if (w[0] == 0x28 && w[3] == 0) S.vcn.dec_create++;
                if (w[0] == 0x28 && w[3] == 2) S.vcn.dec_destroy++;
            }
        }
        i += sz / 4;
    }
    S.vcn.last_ib_inst = inst;
    S.vcn.last_ib_vmid = vmid;
}

static void vcn_run(uint32_t inst, uint32_t wptr)
{
    uint64_t base = S.reg[VCNR(inst, regUVD_RB_BASE_LO)] | ((uint64_t)S.reg[VCNR(inst, regUVD_RB_BASE_HI)] << 32);
    uint32_t size = S.reg[VCNR(inst, regUVD_RB_SIZE)], mask, *ring, guard = 0;
    if (!S.vcn.booted[inst] || !(S.reg[VCNR(inst, regVCN_RB_ENABLE)] & VCN_RB_ENABLE__RB1_EN_MASK) ||
        !size || (size & (size - 1)) || !(ring = (uint32_t *)mc_ptr(base, 4ull * size))) {
        S.vcn.bad++;
        return;
    }
    mask = size - 1;
    wptr &= mask;
#define VPK(k) ring[(S.vcn.rptr[inst] + (k)) & mask]
    while ((S.vcn.rptr[inst] & mask) != wptr && guard++ < 100000) {
        uint32_t cmd = VPK(0), n = 1;
        S.vcn.cmds++;
        switch (cmd) {
        case VCN_ENC_CMD_NO_OP: break;
        case VCN_ENC_CMD_END:   S.vcn.ends++; break;
        case VCN_ENC_CMD_IB:
            vcn_run_ib(inst, VPK(1), VPK(2) | ((uint64_t)VPK(3) << 32), VPK(4));
            n = 5;
            break;
        case VCN_ENC_CMD_FENCE: {
            uint8_t *p = mc_ptr(VPK(1) | ((uint64_t)VPK(2) << 32), 4);
            uint32_t seq = VPK(3);
            if (p) memcpy(p, &seq, 4); else S.vcn.bad++;
            S.vcn.fences++;
            n = 4;
            break;
        }
        case VCN_ENC_CMD_TRAP:
            S.vcn.traps++;
            ih_push(inst ? RD_IH_CLIENTID_VCN1 : RD_IH_CLIENTID_VCN, VCN_4_0__SRCID__UVD_ENC_GENERAL_PURPOSE, 0);
            break;
        case VCN_ENC_CMD_REG_WRITE: S.vcn.reg_writes++; n = 3; break;
        case VCN_ENC_CMD_REG_WAIT:  S.vcn.reg_waits++; n = 4; break;
        default: S.vcn.bad++; break;
        }
        S.vcn.rptr[inst] = (S.vcn.rptr[inst] + n) & mask;
    }
#undef VPK
    S.reg[VCNR(inst, regUVD_RB_RPTR)] = S.vcn.rptr[inst];
}

static void jpeg_exec(const uint32_t *pk, uint32_t ndw, uint32_t vmid_ib_unused, int in_ib);

static void jpeg_ib(void)
{
    uint64_t va = S.vcn.jregs[mmUVD_LMI_JRBC_IB_64BIT_BAR_LOW_INTERNAL_OFFSET - 0x4000] |
                  ((uint64_t)S.vcn.jregs[mmUVD_LMI_JRBC_IB_64BIT_BAR_HIGH_INTERNAL_OFFSET - 0x4000] << 32);
    uint32_t n = S.vcn.jregs[mmUVD_JRBC_IB_SIZE_INTERNAL_OFFSET - 0x4000];
    uint32_t vmid = S.vcn.jregs[mmUVD_LMI_JRBC_IB_VMID_INTERNAL_OFFSET - 0x4000] & 0xf, buf[512], k;
    S.vcn.jpeg_ibs++;
    S.vcn.jpeg_ib_vmid = vmid;
    S.vcn.jpeg_vmid = S.vcn.jregs[mmUVD_LMI_JPEG_VMID_INTERNAL_OFFSET - 0x4000] & 0xf;
    if (n > 512) {
        S.vcn.jpeg_bad++;
        return;
    }
    for (k = 0; k < n; k++)
        if (vcn_rd32(vmid, va + 4ull * k, &buf[k])) {
            S.vcn.jpeg_ib_faults++;
            return;
        }
    jpeg_exec(buf, n, vmid, 1);
}

static void jpeg_exec(const uint32_t *pk, uint32_t ndw, uint32_t vmid_ib_unused, int in_ib)
{
    (void)vmid_ib_unused;
    for (uint32_t i = 0; i + 1 < ndw; i += 2) {
        uint32_t h = pk[i], v = pk[i + 1];
        uint32_t reg = h & 0x3FFFF, type = (h >> 28) & 0xF;
        S.vcn.jpeg_cmds++;
        switch (type) {
        case PACKETJ_TYPE0:
            if (reg == mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET) {
                S.vcn.jext = v;
            } else if (reg == JRBC_DEC_EXTERNAL_REG_WRITE_ADDR) {
                S.vcn.jext_writes++;   /* 0x68e04 framing, 0x3fbc: the IH trigger */
            } else if (reg >= 0x4000 && reg < 0x4200) {
                S.vcn.jregs[reg - 0x4000] = v;
                if (reg == mmUVD_JPEG_PITCH_INTERNAL_OFFSET)
                    S.reg[VCNR(0, regUVD_JPEG_PITCH)] = v;
                if (reg == mmUVD_JPEG_GPCOM_CMD_INTERNAL_OFFSET && v == 0x8) {
                    uint64_t a = S.vcn.jregs[mmUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_LOW_INTERNAL_OFFSET - 0x4000] |
                                 ((uint64_t)S.vcn.jregs[mmUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_HIGH_INTERNAL_OFFSET - 0x4000] << 32);
                    uint8_t *p = mc_ptr(a, 4);
                    uint32_t seq = S.vcn.jregs[mmUVD_JPEG_GPCOM_DATA0_INTERNAL_OFFSET - 0x4000];
                    if (p) memcpy(p, &seq, 4); else S.vcn.jpeg_bad++;
                    S.vcn.jpeg_fences++;
                }
            } else {
                S.vcn.jpeg_bad++;
            }
            break;
        case PACKETJ_TYPE2:
            if (in_ib) S.vcn.jpeg_bad++;
            else jpeg_ib();
            break;
        case PACKETJ_TYPE3: case PACKETJ_TYPE4: case PACKETJ_TYPE6:
            break;
        case PACKETJ_TYPE7:
            S.vcn.jpeg_traps++;
            ih_push(RD_IH_CLIENTID_VCN, VCN_4_0__SRCID__JPEG_DECODE, 0);
            break;
        default:
            S.vcn.jpeg_bad++;
        }
    }
}

static void jpeg_run(uint32_t wptr)
{
    uint64_t base = S.reg[VCNR(0, regUVD_LMI_JRBC_RB_64BIT_BAR_LOW)] |
                    ((uint64_t)S.reg[VCNR(0, regUVD_LMI_JRBC_RB_64BIT_BAR_HIGH)] << 32);
    uint32_t size = S.reg[VCNR(0, regUVD_JRBC_RB_SIZE)], mask, *ring;
    if (!S.vcn.jpeg_smu_on || !size || (size & (size - 1)) || !(ring = (uint32_t *)mc_ptr(base, 4ull * size)) ||
        (S.reg[VCNR(0, regUVD_JRBC_RB_CNTL)] & 1) /* NO_FETCH */) {
        S.vcn.jpeg_bad++;
        return;
    }
    mask = size - 1;
    wptr &= mask;
    while ((S.vcn.jrptr & mask) != wptr) {
        uint32_t two[2] = { ring[S.vcn.jrptr & mask], ring[(S.vcn.jrptr + 1) & mask] };
        jpeg_exec(two, 2, 0, 0);
        S.vcn.jrptr = (S.vcn.jrptr + 2) & mask;
    }
    S.reg[VCNR(0, regUVD_JRBC_RB_RPTR)] = S.vcn.jrptr;
}

/* PGFSM: per two-bit tile field, 1 powers on (status 0), 2 off (status 2) */
static void vcn_pgfsm(uint32_t inst, uint32_t cfg)
{
    uint32_t st = S.reg[VCNR(inst, regUVD_PGFSM_STATUS)];
    for (uint32_t sh = 0; sh < 30; sh += 2) {
        uint32_t f = (cfg >> sh) & 3;
        if (f == 1) st &= ~(3u << sh);
        if (f == 2) st = (st & ~(3u << sh)) | (2u << sh);
    }
    S.reg[VCNR(inst, regUVD_PGFSM_STATUS)] = st;
}

/* a DPG register write (SRAM image or LMA): 'off' is the internal offset
 * SOC15_DPG_MODE_OFFSET made (the AON window from 0x30000 bytes, the VID
 * window from 0), back to instance 'inst''s register. With 'psp' the
 * windows the PSP owns, written as 0 by the driver, get the image it put
 * in the TMR (cache window 0 and its size) and the stack it keeps after it
 * (window 1). 1 = an offset outside the instance. */
#define VCN_TMR_STACK_OFF 0x80000ull
static int vcn_dpg_apply(uint32_t inst, uint32_t off, uint32_t val, int psp)
{
    uint32_t reg = off >= (VCN_AON_IP_ADDRESS_2_0 >> 2) ? off - (VCN_AON_IP_ADDRESS_2_0 >> 2)
                                                         : off + ((VCN_VID_SOC_ADDRESS_2_0 - VCN_AON_SOC_ADDRESS_2_0) >> 2);
    if (reg >= 0x800)
        return 1;
    if (psp && !val) {
        uint64_t stack = S.vcn.tmr_fw[inst] + VCN_TMR_STACK_OFF;
        if (reg == regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW) val = (uint32_t)S.vcn.tmr_fw[inst];
        if (reg == regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH) val = (uint32_t)(S.vcn.tmr_fw[inst] >> 32);
        if (reg == regUVD_VCPU_CACHE_SIZE0) val = (S.vcn.fw_size[inst] + 8 + 4095u) & ~4095u;
        if (reg == regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW) val = (uint32_t)stack;
        if (reg == regUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH) val = (uint32_t)(stack >> 32);
    }
    sim_wr(NULL, 4ull * VCNR(inst, reg), val);
    return 0;
}

/* LOAD_IP_FW GFX_FW_TYPE_VCN0_RAM / VCN1_RAM: the DPG SRAM image of an
 * instance, (offset, value) pairs the tiles take as they power up in DPG
 * mode; the model applies them at once. Returns the PSP status. */
static uint32_t vcn_dpg_sram_load(uint32_t inst, uint64_t mc, uint32_t size)
{
    const uint32_t *p = (const uint32_t *)mc_ptr(mc, size ? size : 1);
    uint32_t k;
    S.vcn.sram_loads[inst]++;
    if (!p || !size || (size & 7) || size > sizeof(S.vcn.sram[0]) || !S.vcn.smu_on[inst] ||
        !(S.reg[VCNR(inst, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_PG_MODE_MASK)) {
        S.vcn.sram_bad++;
        return 0xFFFF0003;
    }
    S.vcn.sram_n[inst] = size / 8;
    for (k = 0; k < size / 8; k++) {
        S.vcn.sram[inst][k][0] = p[2 * k];
        S.vcn.sram[inst][k][1] = p[2 * k + 1];
        if (vcn_dpg_apply(inst, p[2 * k], p[2 * k + 1], 1))
            S.vcn.sram_bad++;
    }
    return 0;
}

/* a register write the VCN model takes; 0 = plain register */
static int vcn_wr(uint32_t dw, uint32_t v)
{
    uint32_t reg;
    int i = vcn_inst_of(dw, &reg);
    if (i < 0)
        return 0;
    if (!S.vcn.smu_on[i] && (reg == regUVD_VCPU_CNTL || reg == regUVD_RB_BASE_LO || reg == regUVD_RB_SIZE ||
                             reg == regVCN_RB_ENABLE || reg == regUVD_MASTINT_EN))
        S.vcn.regs_while_off++;
    if (i == 0 && !S.vcn.jpeg_smu_on && (reg == regUVD_JRBC_RB_CNTL || reg == regUVD_LMI_JRBC_RB_64BIT_BAR_LOW ||
                                         reg == regUVD_JRBC_RB_SIZE || reg == regJPEG_SYS_INT_EN))
        S.vcn.regs_while_off++;
    switch (reg) {
    case regUVD_PGFSM_CONFIG:
        /* DPG: the tiles are the hardware's; only JPEG's UVDJ is static */
        if ((S.reg[VCNR(i, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_PG_MODE_MASK) &&
            (v & ~(uint32_t)UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG_MASK))
            S.vcn.static_pg_in_dpg++;
        S.reg[dw] = v;
        vcn_pgfsm((uint32_t)i, v);
        return 1;
    case regUVD_DPG_LMA_CTL: {
        /* a DPG register write through the LMA (WREG32_SOC15_DPG_MODE,
         * indirect off): LMA_DATA to the internal offset */
        uint32_t addr = (v & UVD_DPG_LMA_CTL__READ_WRITE_ADDR_MASK) >> UVD_DPG_LMA_CTL__READ_WRITE_ADDR__SHIFT;
        S.reg[dw] = v;
        if (!(v & UVD_DPG_LMA_CTL__READ_WRITE_MASK))
            return 1;
        S.vcn.lma_writes++;
        if (!(S.reg[VCNR(i, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_PG_MODE_MASK) ||
            vcn_dpg_apply((uint32_t)i, addr, S.reg[VCNR(i, regUVD_DPG_LMA_DATA)], 0))
            S.vcn.lma_bad++;
        return 1;
    }
    case regUVD_DPG_PAUSE: {
        /* vcn_v4_0_pause_dpg_mode's handshake: REQ is acknowledged, the
         * tiles held on (POWER_STATUS 2) until the request goes; idle in
         * DPG they read as gated (1) */
        uint32_t ps = VCNR(i, regUVD_POWER_STATUS);
        S.reg[dw] = v & ~(uint32_t)UVD_DPG_PAUSE__NJ_PAUSE_DPG_ACK_MASK;
        if (!(S.reg[ps] & UVD_POWER_STATUS__UVD_PG_MODE_MASK) || !S.vcn.smu_on[i]) {
            S.vcn.pause_bad++;
            return 1;
        }
        if (v & UVD_DPG_PAUSE__NJ_PAUSE_DPG_REQ_MASK) {
            if ((S.reg[ps] & UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) != 1)
                S.vcn.pause_bad++;   /* amdgpu waits for 1 before asking */
            S.reg[dw] |= UVD_DPG_PAUSE__NJ_PAUSE_DPG_ACK_MASK;
            S.reg[ps] = (S.reg[ps] & ~(uint32_t)UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) | 2;
            S.vcn.pause_reqs++;
        } else {
            S.reg[ps] = (S.reg[ps] & ~(uint32_t)UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) | 1;
            S.vcn.unpause_reqs++;
        }
        return 1;
    }
    case regUVD_VCPU_CNTL: {
        uint64_t win = S.reg[VCNR(i, regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW)] |
                       ((uint64_t)S.reg[VCNR(i, regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH)] << 32);
        S.reg[dw] = v;
        if (v & UVD_VCPU_CNTL__BLK_RST_MASK) {
            S.vcn.booted[i] = 0;
        } else if ((v & UVD_VCPU_CNTL__CLK_EN_MASK) && !S.vcn.booted[i]) {
            if (win != S.vcn.tmr_fw[i] || !S.vcn.tmr_fw[i] || !S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE0)] ||
                !S.vcn.smu_on[i]) {
                S.vcn.bad_boots++;
            } else {
                S.vcn.booted[i] = 1;
                S.vcn.boots++;
                S.reg[VCNR(i, regUVD_STATUS)] |= 2;   /* VCPU_REPORT: up */
            }
        }
        return 1;
    }
    case regVCN_RB_ENABLE: {
        uint32_t was = S.reg[dw];
        S.reg[dw] = v;
        if ((v & VCN_RB_ENABLE__RB1_EN_MASK) && !(was & VCN_RB_ENABLE__RB1_EN_MASK)) {
            uint64_t nc = S.reg[VCNR(i, regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW)] |
                          ((uint64_t)S.reg[VCNR(i, regUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH)] << 32);
            uint8_t *fs = mc_ptr(nc, RD_VCN4_FWS_SIZE);
            uint32_t flags = 0;
            if (fs) memcpy(&flags, fs, 4);
            if (!fs || !(flags & RD_FW_SHARED_FLAG_0_UNIFIED_QUEUE) || fs[RD_VCN4_FWS_SQ_IS_ENABLED] != 1 ||
                !(fs[RD_VCN4_FWS_SQ_QUEUE_MODE] & RD_FW_QUEUE_RING_RESET))
                S.vcn.bad_fw_shared++;
            S.vcn.rb_enables++;
            S.vcn.rptr[i] = S.reg[VCNR(i, regUVD_RB_RPTR)];
        }
        return 1;
    }
    case regUVD_RB_RPTR:
        S.reg[dw] = v;
        S.vcn.rptr[i] = v;
        return 1;
    case regUVD_RB_WPTR:
        S.reg[dw] = v;
        if (S.vcn.booted[i] && (S.reg[VCNR(i, regVCN_RB_ENABLE)] & VCN_RB_ENABLE__RB1_EN_MASK))
            vcn_run((uint32_t)i, v);
        return 1;
    default:
        break;
    }
    if (i == 0) {
        switch (reg) {
        case regUVD_JRBC_RB_RPTR:
            S.reg[dw] = v;
            S.vcn.jrptr = v;
            return 1;
        case regUVD_JRBC_RB_WPTR:
            S.reg[dw] = v;
            if (S.vcn.jpeg_smu_on && !(S.reg[VCNR(0, regUVD_JRBC_RB_CNTL)] & 1))
                jpeg_run(v);
            return 1;
        default:
            break;
        }
    }
    return 0;
}

/* a register read the VCN model answers; 0 = plain register */
static int vcn_rd(uint32_t dw, uint32_t *out)
{
    uint32_t reg;
    int i = vcn_inst_of(dw, &reg);
    if (i < 0)
        return 0;
    if (reg == regUVD_LMI_STATUS) {   /* the memory interface is always clean */
        *out = ~0u;
        return 1;
    }
    if (i == 0 && reg == regUVD_JRBC_STATUS) {
        *out = (S.reg[dw] & ~(uint32_t)UVD_JRBC_STATUS__RB_JOB_DONE_MASK) | UVD_JRBC_STATUS__RB_JOB_DONE_MASK;
        return 1;
    }
    return 0;
}

/* 32-bit doorbells: the VCN unified rings and the JPEG ring, inside the
 * S2A range NBIO routes to VCN (entries 4 / 5) */
static int vcn_db_routed(uint32_t index, uint32_t inst)
{
    uint32_t e = smn_val(inst ? regS2A_DOORBELL_ENTRY_5_CTRL : regS2A_DOORBELL_ENTRY_4_CTRL);
    uint32_t off = (e & S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_OFFSET_MASK) >>
                   S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_OFFSET__SHIFT;
    uint32_t sz = (e & S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_SIZE_MASK) >>
                  S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_SIZE__SHIFT;
    return (e & S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_ENABLE_MASK) && index >= off && index < off + sz;
}

static void sim_doorbell32(void *ctx, uint32_t index, uint32_t v)
{
    (void)ctx;
    S.db_writes++;
    S.db_last_index = index;
    if (S.pending_writes)
        S.unflushed_doorbells++;
    /* IH rptr doorbells (IH_DOORBELL_RPTR / _RING1), routed by S2A entry 1 */
    for (uint32_t ring = 0; ring < 2; ring++) {
        uint32_t c = S.reg[OSS_BASE0 + (ring ? SIM_IH_DOORBELL_RPTR_RING1 : regIH_DOORBELL_RPTR)];
        if ((c & IH_DOORBELL_RPTR__ENABLE_MASK) && (c & IH_DOORBELL_RPTR__OFFSET_MASK) == index) {
            uint32_t e = smn_val(regS2A_DOORBELL_ENTRY_1_CTRL);
            uint32_t off = (e & S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_OFFSET_MASK) >>
                           S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_OFFSET__SHIFT;
            uint32_t sz = (e & S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_SIZE_MASK) >>
                          S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_SIZE__SHIFT;
            /* ring 1's doorbell (the next 64-bit slot) is outside the
             * two-dword range nbio_v4_3_ih_doorbell_range opens; the
             * model does not require it to be routed */
            if (!ring && (!(e & S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_ENABLE_MASK) || index < off ||
                          index >= off + sz)) {
                S.ih_db_unrouted++;
                return;
            }
            S.reg[OSS_BASE0 + (ring ? SIM_IH_RB_RPTR_RING1 : regIH_RB_RPTR)] = v;
            S.ih_db_rptr++;
            return;
        }
    }
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t c = S.reg[VCNR(i, regVCN_RB1_DB_CTRL)];
        if ((c & VCN_RB1_DB_CTRL__EN_MASK) &&
            ((c & VCN_RB1_DB_CTRL__OFFSET_MASK) >> VCN_RB1_DB_CTRL__OFFSET__SHIFT) == index) {
            if (!vcn_db_routed(index, i))
                S.vcn.db_unrouted++;
            if (S.vcn.booted[i] && (S.reg[VCNR(i, regVCN_RB_ENABLE)] & VCN_RB_ENABLE__RB1_EN_MASK))
                vcn_run(i, v);
            S.vcn.dbs++;
            return;
        }
    }
    {
        uint32_t c = S.reg[VCNR(0, regVCN_JPEG_DB_CTRL)];
        if ((c & VCN_JPEG_DB_CTRL__EN_MASK) &&
            ((c & VCN_JPEG_DB_CTRL__OFFSET_MASK) >> VCN_JPEG_DB_CTRL__OFFSET__SHIFT) == index) {
            if (!vcn_db_routed(index, 0))
                S.vcn.db_unrouted++;
            if (S.vcn.jpeg_smu_on && !(S.reg[VCNR(0, regUVD_JRBC_RB_CNTL)] & 1))
                jpeg_run(v);
            S.vcn.dbs++;
            return;
        }
    }
    S.db_unclaimed++;
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
    if (vcn_wr(dw, v))
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
    /* SH_MEM_CONFIG / SH_MEM_BASES / SPI_GDBG_PER_VMID_CNTL: one copy per
     * VMID, the one GRBM_GFX_CNTL.VMID selects */
    if (dw == GCR(regSH_MEM_CONFIG) || dw == GCR(regSH_MEM_BASES) || dw == GC_BASE0 + 0x1f72) {
        uint32_t vmid = (S.reg[GCR(regGRBM_GFX_CNTL)] & GRBM_GFX_CNTL__VMID_MASK) >> GRBM_GFX_CNTL__VMID__SHIFT;
        S.sh_mem[vmid][dw == GCR(regSH_MEM_CONFIG) ? 0 : dw == GCR(regSH_MEM_BASES) ? 1 : 2] = v;
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
            S.ring_mc = 0;
            S.rptr_dw = 0;
            S.ring_destroys++;
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
    if (ogam_wr(dw, v))
        return;
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
            uint32_t clk = arg >> 16;
            S.dal_hard_min[clk] = arg & 0xffff;
            S.reg[RD_DAL_ARG_REG] = (arg & 0xffff) * 1000;
            if (S.dal_norder < 8) {
                S.dal_order[S.dal_norder] = clk;
                S.dal_order_nseq[S.dal_norder] = S.dmub.nseq;
                S.dal_order_htotal[S.dal_norder++] = S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)];
            }
            /* the PMFW's DFS: the slowest divider that reaches the minimum */
            if ((clk == RD_PPCLK_DISPCLK || clk == RD_PPCLK_DPPCLK) && !S.dal_ignore_hard_min) {
                uint32_t vco = (S.reg[RD_CLK1_CLK_PLL_REQ] & 0x1ff) * 100000u, did;
                if (!vco)
                    vco = RD_DCN32_VCO_DEFAULT_KHZ;
                for (did = 0x7f; did >= 0x08; did--)
                    if (4ull * vco / dcn32_dentist_divider(did) >= (arg & 0xffff) * 1000ull)
                        break;
                S.reg[RD_CLK1_CLK0_DFS_CNTL + RD_CLK1_DFS_STRIDE * (clk == RD_PPCLK_DPPCLK)] = did;
            }
        } else if (v == RD_DALSMC_MSG_GetSmuVersion) {
            S.reg[RD_DAL_ARG_REG] = 0x00505c00;   /* 80.92 */
        } else if (v == RD_DALSMC_MSG_GetDpmFreqByIndex && ((arg >> 16) == RD_PPCLK_DISPCLK ||
                                                             (arg >> 16) == RD_PPCLK_DPPCLK)) {
            S.reg[RD_DAL_ARG_REG] = (arg & 0xff) == 0xff ? S.dal_dpm_n
                                  : (arg & 0xff) < S.dal_dpm_n ? S.dal_dpm[arg & 0xff] : 0;
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
    /* DP stream encoder / DIG FIFO status bits follow their controls
     * (only while the DP sink is modelled) */
    if (S.dps.be >= 0) {
        for (uint32_t n = 0; n < 5; n++) {
            if (dw == DPR(regDP0_DP_VID_STREAM_CNTL, n)) {
                *R(dw) = (v & ~DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK) |
                         ((v & DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK)
                              ? DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK : 0);
                return;
            }
            if (dw == DIGN(regDIG0_DIG_FIFO_CTRL0, n)) {
                *R(dw) = (v & ~DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE_MASK) |
                         ((v & DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_MASK) ? DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE_MASK : 0);
                return;
            }
        }
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

/* the PM4 packets a gfx ring holds between two write pointers, as emitted
 * (COND_EXEC is not followed): opcodes into ops[], ring indexes into at[] */
static unsigned ring_ops(rdna3_dev *d, const rdna3_gfx_ring *r, uint64_t from, uint64_t to,
                         uint32_t *ops, uint32_t *at, unsigned max)
{
    unsigned n = 0;
    while (from < to && n < max) {
        uint32_t h;
        rdna3_vram_read(d, r->ring + 4ull * (from & r->buf_mask), &h, 4);
        if (h >> 30 != 3)
            return 0;
        ops[n] = (h >> 8) & 0xff;
        at[n++] = (uint32_t)(from & r->buf_mask);
        from += 2 + ((h >> 16) & 0x3fff);
    }
    return from == to ? n : 0;
}

static uint32_t ring_dw(rdna3_dev *d, const rdna3_gfx_ring *r, uint32_t idx)
{
    uint32_t v;
    rdna3_vram_read(d, r->ring + 4ull * (idx & r->buf_mask), &v, 4);
    return v;
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
    /* CP_GFX_INDEX_MUTEX held by the firmware (client 2) */
    if (dw == GCR(regCP_GFX_INDEX_MUTEX) && S.gfx_mutex_fw)
        return 0x5;
    if (dw == NBIO_B1 + regBIF_BX_PF0_RSMU_DATA) {
        uint32_t *p = smn_slot(S.smn_index, 0);
        return p ? *p : 0;
    }
    if (dw == mmMM_DATA) {
        uint32_t *w = mm_word();
        return w ? *w : 0;
    }
    {
        uint32_t vv;
        if (vcn_rd(dw, &vv))
            return vv;
    }
    if (dw == DCN_B2 + regDC_I2C_DATA) {
        uint32_t b = S.i2c_buf[S.i2c_ri++ % sizeof(S.i2c_buf)];
        return (S.reg[dw] & ~DC_I2C_DATA__DC_I2C_DATA_MASK) | (b << DC_I2C_DATA__DC_I2C_DATA__SHIFT);
    }
    for (uint32_t n = 0; n < RD_AUX_ENGINES; n++)
        if ((S.hpd_model & (1u << n)) && dw == HPDR(regHPD0_DC_HPD_INT_STATUS, n))
            hpd_filter_eval(n);
    /* DIG_SYMCLK_FE_ON: an enabled back end takes the front end's symbols */
    if (S.dps.be >= 0)
        for (uint32_t n = 0; n < 5; n++)
            if (dw == DIGN(regDIG0_DIG_FE_CNTL, n)) {
                uint32_t on = 0;
                for (uint32_t b = 0; b < 5; b++)
                    if ((S.reg[DIGN(regDIG0_DIG_BE_EN_CNTL, b)] & DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK) &&
                        (S.reg[DIGN(regDIG0_DIG_BE_CNTL, b)] & (1u << (n + DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT))))
                        on = DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
                return (S.reg[dw] & ~DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK) | on;
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
            crc ^= ogam_crc(p);    /* MPCC p: the pipe's own */
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
    IP(AMD_HWID_MP0, 13, 0, 10, MP0_BASE, 0, 0);
    IP(AMD_HWID_MP1, 13, 0, 10, MP1_BASE, 0, 0);
    IP(AMD_HWID_THM, 13, 0, 2, THM_BASE, 0, 0);
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
    /* VCN 4.0.0: two instances (the card's bases; instance 1 reports 4.0.1) */
    IP(AMD_HWID_VCN, 4, 0, 0, 0x7800, VCN_B1(0), 0);
    disc->ips[disc->num_ips - 1].num_base = disc->ips[disc->num_ips - 1].num_base_kept = 2;
    IP(AMD_HWID_VCN, 4, 0, 1, 0x7B00, VCN_B1(1), 0);
    disc->ips[disc->num_ips - 1].num_base = disc->ips[disc->num_ips - 1].num_base_kept = 2;
    disc->ips[disc->num_ips - 1].instance = 1;
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
static uint8_t syn_sos[16384], syn_blob[AMD_BLOB_COUNT][8192], syn_ta[256 + 4096];

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void synth(amd_fw_blob_ref blobs[AMD_BLOB_COUNT], const uint8_t **sos, size_t *sos_len)
{
    unsigned i;
    /* SYS_DRV, SOS, KDB, TOC and the register list (PSP_FW_TYPE_PSP_RL) */
    uint32_t types[5][3] = { { 2, 0, 1024 }, { 1, 1024, 2048 }, { 3, 3072, 512 }, { 4, 3584, 256 },
                             { 6, 3840, 128 } };
    /* the TA file: ASD, HDCP, DTM and RAS (ta_firmware_header_v2_0) */
    uint32_t tas[4][3] = { { 1, 0, 1024 }, { 4, 1024, 512 }, { 5, 2048, 512 }, { 3, 3072, 256 } };
    memset(syn_ta, 0x5A, sizeof(syn_ta));
    memset(syn_ta, 0, 256);
    w32(syn_ta + 0, 256 + 4096); w32(syn_ta + 4, 256);
    syn_ta[8] = 2;
    w32(syn_ta + 24, 256);
    w32(syn_ta + 32, 4);
    for (i = 0; i < 4; i++) {
        w32(syn_ta + 36 + 16 * i, tas[i][0]);
        w32(syn_ta + 40 + 16 * i, 0x21000000u + i);
        w32(syn_ta + 44 + 16 * i, tas[i][1]);
        w32(syn_ta + 48 + 16 * i, tas[i][2]);
    }
    memset(syn_sos, 0xA5, sizeof(syn_sos));
    w32(syn_sos + 0, 256 + 4096); w32(syn_sos + 4, 256);
    syn_sos[8] = 2; syn_sos[9] = 0; syn_sos[10] = 0; syn_sos[11] = 0;
    w32(syn_sos + 24, 256);
    w32(syn_sos + 32, 5);
    for (i = 0; i < 5; i++) {
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

/* D4: display state save / restore (hw/dcn32_state.c). A head's registers
 * as the power loss of a reset or S3 leaves them (dm_suspend / dm_resume
 * model): its lists zeroed, OTG off, HUBP blanked, DIG off, the pipe power
 * gated, DCCG / DET / routing / the system aperture gone. */
static uint32_t st_snap[MMIO_LEN / 4];

static void st_wipe(uint32_t pipe, uint32_t otg, uint32_t opp, uint32_t mpcc, uint32_t fe, uint32_t be)
{
    static const uint16_t tmds[] = { regDIG0_TMDS_CNTL, regDIG0_TMDS_CONTROL_CHAR, regDIG0_TMDS_CONTROL0_FEEDBACK,
        regDIG0_TMDS_SYNC_CHAR_PATTERN_0_1, regDIG0_TMDS_SYNC_CHAR_PATTERN_2_3, regDIG0_TMDS_CTL_BITS,
        regDIG0_TMDS_DCBALANCER_CONTROL, regDIG0_TMDS_SYNC_DCBALANCE_CHAR, regDIG0_TMDS_CTL0_1_GEN_CNTL,
        regDIG0_TMDS_CTL2_3_GEN_CNTL };
    size_t k;
    for (k = 0; k < sizeof(rd_clone_hubp) / 2; k++)
        S.reg[DCN_B2 + rd_clone_hubp[k] + RD_CLONE_HUBP_STRIDE * pipe] = 0;
    for (k = 0; k < sizeof(rd_clone_dpp) / 2; k++)
        S.reg[DCN_B2 + rd_clone_dpp[k] + RD_CLONE_DPP_STRIDE * pipe] = 0;
    for (k = 0; k < sizeof(rd_clone_mpcc) / 2; k++)
        S.reg[DCN_B3 + rd_clone_mpcc[k] + RD_CLONE_MPCC_STRIDE * mpcc] = 0;
    for (k = 0; k < sizeof(rd_clone_opp) / 2; k++)
        S.reg[DCN_B2 + rd_clone_opp[k] + RD_CLONE_OPP_STRIDE * opp] = 0;
    for (k = 0; k < sizeof(rd_clone_odm) / 2; k++)
        S.reg[DCN_B2 + rd_clone_odm[k] + RD_CLONE_ODM_STRIDE * otg] = 0;
    for (k = 0; k < sizeof(rd_clone_otg) / 2; k++)
        S.reg[DCN_B2 + rd_clone_otg[k] + RD_CLONE_OTG_STRIDE * otg] = 0;
    for (k = 0; k < sizeof(rd_clone_digfe) / 2; k++)
        S.reg[DCN_B2 + rd_clone_digfe[k] + RD_CLONE_DIGFE_STRIDE * fe] = 0;
    for (k = 0; k < sizeof(tmds) / 2; k++)
        S.reg[DCN_B2 + tmds[k] + RD_CLONE_DIGFE_STRIDE * be] = 0;
    for (k = regCURSOR0_0_CURSOR_CONTROL; k <= regCURSOR0_0_CURSOR_DST_OFFSET; k++)
        S.reg[HUBP(k, pipe)] = 0;
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, pipe)] = HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
    S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, pipe)] = 0;
    S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, pipe)] = 0;
    S.reg[OTG(regOTG0_OTG_CONTROL, otg)] = 0;
    S.reg[DCN_B2 + regVTG0_CONTROL + otg] = 0;
    S.reg[DCN_B2 + regODM0_OPTC_DATA_SOURCE_SELECT + RD_CLONE_ODM_STRIDE * otg] = 0;
    S.reg[DCN_B3 + regMPCC0_MPCC_TOP_SEL + RD_CLONE_MPCC_STRIDE * mpcc] = 0xf;
    S.reg[DCN_B3 + regMPCC0_MPCC_BOT_SEL + RD_CLONE_MPCC_STRIDE * mpcc] = 0xf;
    S.reg[DCN_B3 + regMPCC0_MPCC_OPP_ID + RD_CLONE_MPCC_STRIDE * mpcc] = 0xf;
    S.reg[DCN_B3 + regMPC_OUT0_MUX + (regMPC_OUT1_MUX - regMPC_OUT0_MUX) * opp] = 0xf;
    S.reg[DCN_B3 + regMPCC_OGAM0_MPCC_OGAM_CONTROL + 0x5e * mpcc] = 0;
    S.reg[DCN_B2 + regDIG0_DIG_FE_CNTL + RD_CLONE_DIGFE_STRIDE * fe] = 0;
    S.reg[DCN_B2 + regDIG0_DIG_BE_CNTL + RD_CLONE_DIGFE_STRIDE * be] = 0;
    S.reg[DCN_B2 + regDIG0_DIG_BE_EN_CNTL + RD_CLONE_DIGFE_STRIDE * be] = 0;
    S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * otg] = 0;
    S.reg[DCN_B1 + regDP_DTO0_PHASE + 4 * otg] = 0;
    S.reg[DCN_B1 + regDP_DTO0_MODULO + 4 * otg] = 0;
    S.reg[DCN_B1 + regOTG_PIXEL_RATE_DIV] &= ~(7u << (3 * otg));
    S.reg[DCN_B1 + regDPPCLK0_DTO_PARAM + pipe] = 0;
    S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] &= ~(3u << (4 * pipe));
    S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + pipe] = 0;
    S.reg[DCN_B2 + regDOMAIN0_PG_CONFIG + 2 * pipe] = DOMAIN0_PG_CONFIG__DOMAIN_POWER_GATE_MASK;
    S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2 * pipe] = 2u << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT;
}

/* registers of the DCN windows that differ from the snapshot, minus the
 * DC_IP request the power steps leave behind; the first ones printed */
static unsigned st_diff(const char *when)
{
    static const struct { uint32_t base, len; } win[] = {
        { DCN_B1, 0x100 }, { DCN_B2 + 0x400, 0x2200 }, { DCN_B3, 0x200 } };
    unsigned n = 0, w, i;
    for (w = 0; w < 3; w++)
        for (i = win[w].base; i < win[w].base + win[w].len; i++) {
            uint32_t ign = 0, p;
            for (p = 0; p < RD_DCN_MAX_PIPES; p++)
                if (i == HUBP(regHUBP0_DCHUBP_CNTL, p))   /* trigger and status bits */
                    ign = HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_CLEAR_MASK | HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK;
                else if (i == DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * p)
                    ign = OTG0_PIXEL_RATE_CNTL__DPDTO0_ENABLE_STATUS_MASK;
            if (((S.reg[i] ^ st_snap[i]) & ~ign) == 0 || i == DCN_B2 + regDC_IP_REQUEST_CNTL)
                continue;
            if (n++ < 8)
                fprintf(stderr, "dcn32_state %s: reg 0x%x (window +0x%x) 0x%x, was 0x%x\n", when, i,
                        i - win[w].base, S.reg[i], st_snap[i]);
        }
    return n;
}

static void test_dcn_state(rdna3_dev *dev, rdna3_dmub *dm)
{
    static rdna3_dcn dc, dp;
    static rdna3_dcn_state st, stdp;
    static rdna3_dcn_clone cl;
    rdna3_display_path own, other;
    rdna3_dcn_state_check k;
    uint32_t ctl, i, wm;
    uint8_t bring_up[4], bring_up_tx[4];

    memset(&st, 0, sizeof(st));   /* run() goes twice */
    memset(&stdp, 0, sizeof(stdp));
    memset(&own, 0, sizeof(own));
    own.transmitter = 1; own.hpd = 0; own.encoder_objid = 0x211e; own.connector_objid = 0x310c;
    memset(&other, 0, sizeof(other));
    other.transmitter = 3; other.hpd = 2; other.encoder_objid = 0x2120; other.connector_objid = 0x310c;
    /* the boot head (pipe 1, OTG 2, DIGC -> UNIPHY B, 1440p) and a clone
     * of it on UNIPHY D: the clone's DMCUB commands are the bring-up a
     * restore after a power loss must repeat */
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].pipe == 1);
    CHECK(dcn32_clone_plan(dev, &dc, 0, &other, 241500, &cl) == RDNA3_OK);
    S.dmub.nseq = 0;
    CHECK(dcn32_clone_enable(dev, dm, &cl) == RDNA3_OK && S.dmub.nseq == 4);
    for (i = 0; i < 4; i++) {
        bring_up[i] = S.dmub.seq[i];
        bring_up_tx[i] = S.dmub.seq_tx_action[i];
    }
    /* global state the bring-up (hubbub3_init_dchub_sys_ctx, dccg2_init) left */
    S.reg[DCN_B2 + 0x0475] = 0x80;      /* DCN_VM_FB_LOCATION_BASE */
    S.reg[DCN_B2 + 0x0476] = 0x83;      /* DCN_VM_FB_LOCATION_TOP */
    S.reg[DCN_B1 + 0x007b] = 0x120464;  /* MICROSECOND_TIME_BASE_DIV */
    S.reg[DCN_B3 + regMPCC_OGAM0_MPCC_OGAM_CONTROL + 0x5e * 1] = 2;   /* boot head: OGAM RAM A */
    CHECK(dcn32_read_state(dev, &dc) == 2 && dc.head[0].pipe == 0 && dc.head[1].pipe == 1);
    CHECK(dcn32_state_save(dev, &dc, 0, &other, 241500, &st) == RDNA3_OK && st.head[0].tmds);
    CHECK(dcn32_state_save(dev, &dc, 1, &own, 0, &st) == RDNA3_OK && st.head[1].tmds);
    CHECK(st.head[1].pclk_khz >= 241400 && st.head[1].pclk_khz <= 241600);   /* from REF_FREQ_TO_PIX_FREQ */
    CHECK(st.head[0].otg == 1 && st.head[0].fe == 3 && st.head[0].be == 3 && st.head[1].otg == 2 &&
          st.head[1].opp == 2 && st.head[1].fe == 2 && st.head[1].be == 1 && st.saves == 2);
    CHECK(dcn32_state_check(dev, &st, 0, 0, &k) == 0 && k.otg_on && k.dig_on && k.ndiff == 0);
    CHECK(dcn32_state_check(dev, &st, 2, 0, &k) == RDNA3_EINVAL);

    /* 1. monitor standby on the boot head (rdna3dpms=2): blank +
     * TransmitterControl DISABLE on UNIPHY B, back with ENABLE + unblank;
     * the OTG keeps running */
    ctl = S.reg[OTG(regOTG0_OTG_CONTROL, 2)];
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] |= HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK;
    S.dmub.nseq = 0;
    CHECK(dcn32_head_link(dev, dm, &dc, &st, 1, 0) == RDNA3_OK);
    CHECK(S.dmub.nseq == 1 && S.dmub.seq[0] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL &&
          S.dmub.seq_tx_action[0] == RD_ATOM_TX_DISABLE && S.dmub.tx[0] == 1 && S.dmub.tx[2] == 3 &&
          S.dmub.tx[8] == 1 && S.dmub.tx[9] == 1u << 2 && S.dmub.tx[10] == 0x0c);
    CHECK((S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK) &&
          !(S.reg[DCN_B2 + regDIG0_DIG_BE_EN_CNTL + RD_CLONE_DIGFE_STRIDE] & DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK));
    CHECK(dcn32_head_link(dev, dm, &dc, &st, 1, 0) == RDNA3_OK && S.dmub.nseq == 1);   /* DIG off: no command */
    /* a snapshot taken in standby keeps the DIG ids of the last one */
    dp = dc;
    dp.head[1].dig_fe = dp.head[1].dig_be = -1;
    CHECK(dcn32_state_save(dev, &dp, 1, &own, 241500, &st) == RDNA3_OK && st.head[1].tmds &&
          st.head[1].fe == 2 && st.head[1].be == 1);
    CHECK(dcn32_head_link(dev, dm, &dc, &st, 1, 1) == RDNA3_OK);
    CHECK(S.dmub.nseq == 2 && S.dmub.seq_tx_action[1] == RD_ATOM_TX_ENABLE && S.dmub.tx[4] == (24150 & 0xff));
    CHECK(!(S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK) &&
          (S.reg[DCN_B2 + regDIG0_DIG_BE_EN_CNTL + RD_CLONE_DIGFE_STRIDE] & DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK) &&
          S.reg[OTG(regOTG0_OTG_CONTROL, 2)] == ctl);
    CHECK(dcn32_head_link(dev, NULL, &dc, &st, 1, 0) == RDNA3_ENODEV);
    /* a DP head: refused (no link training), the caller keeps it blank */
    dp = dc;
    dp.head[1].dig_mode = 0;
    CHECK(dcn32_state_save(dev, &dp, 1, &own, 241500, &stdp) == RDNA3_OK && !stdp.head[1].tmds);
    CHECK(dcn32_head_link(dev, dm, &dp, &stdp, 1, 0) == RDNA3_EINVAL);
    CHECK(dcn32_state_restore(dev, dm, &dp, &stdp, 1, 0, 0) == RDNA3_EINVAL);
    CHECK(dcn32_state_save(dev, &dc, 1, &own, 241500, &st) == RDNA3_OK);
    memcpy(st_snap, S.reg, sizeof(st_snap));

    /* 2. system sleep without a power loss: both streams down
     * (amdgpu_dm_commit_zero_streams), then the snapshot back */
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 0)] |= HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK;
    wm = S.reg[DCN_B2 + 0x04fe];
    S.reg[DCN_B2 + 0x04fe] = 0x1234;   /* a watermark set after the snapshot: the sleep keeps it */
    memcpy(st_snap, S.reg, sizeof(st_snap));
    S.dmub.nseq = 0;
    CHECK(dcn32_state_head_off(dev, dm, &dc, &st, 0) == RDNA3_OK && dcn32_state_head_off(dev, dm, &dc, &st, 1) == RDNA3_OK);
    CHECK(S.dmub.nseq == 4 && S.dmub.seq_tx_action[0] == RD_ATOM_TX_DISABLE &&
          S.dmub.seq[1] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK && S.dmub.seq_tx_action[2] == RD_ATOM_TX_DISABLE &&
          S.dmub.seq[3] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK && S.dmub.pixclk[0] == 0);
    CHECK(dcn32_read_state(dev, &dp) == 0);
    CHECK(dcn32_state_check(dev, &st, 1, 0, &k) == 1 && !k.otg_on && k.blanked && !k.dig_on && k.powered);
    S.dmub.nseq = 0;
    CHECK(dcn32_state_restore(dev, dm, &dc, &st, 0, 0, 0) == RDNA3_OK &&
          dcn32_state_restore(dev, dm, &dc, &st, 1, 0, 0) == RDNA3_OK);
    /* per head: pixel clock, stream setup, enable (the PHY kept its setup) */
    CHECK(S.dmub.nseq == 6);
    for (i = 0; i < 6; i += 3)
        CHECK(S.dmub.seq[i] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK && S.dmub.seq[i + 1] == RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL &&
              S.dmub.seq[i + 2] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[i + 2] == RD_ATOM_TX_ENABLE);
    CHECK(S.dmub.pixclk[0] == (2415000 & 0xff) && S.dmub.pixclk[4] == RD_ATOM_COMBOPHY_PLL0 + 1 && S.dmub.pixclk[8] == 2);
    CHECK(st_diff("after sleep") == 0 && st.restores == 2);
    S.dcn_frames = 1;
    CHECK(dcn32_state_check(dev, &st, 1, 50, &k) == 0 && k.frames > 0 && !k.blanked && k.dig_on);
    S.dcn_frames = 0;
    S.reg[DCN_B2 + 0x04fe] = wm;

    /* 3. a power loss (mode-1 reset, S3): both heads and the global state
     * wiped; the restore repeats the clone's bring-up per head */
    memcpy(st_snap, S.reg, sizeof(st_snap));
    st_wipe(0, 1, 1, 0, 3, 3);
    st_wipe(1, 2, 2, 1, 2, 1);
    S.reg[DCN_B2 + 0x0475] = S.reg[DCN_B2 + 0x0476] = 0;
    S.reg[DCN_B1 + 0x007b] = 0;
    CHECK(dcn32_read_state(dev, &dp) == 0);
    CHECK(dcn32_state_check(dev, &st, 0, 0, &k) == 1 && !k.powered && !k.sys_ok && !k.routed && k.ndiff > 100);
    /* the clocks can no longer carry the head: refused (no clock manager) */
    S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x7f;
    CHECK(dcn32_state_restore(dev, dm, &dc, &st, 0, 0, 0) == RDNA3_EBUSY);
    S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x18;
    CHECK(dcn32_state_restore(dev, NULL, &dc, &st, 0, 0, 0) == RDNA3_ENODEV);
    i = S.dcn_gated_drops;
    S.dmub.nseq = 0;
    CHECK(dcn32_state_restore(dev, dm, &dc, &st, 0, 0, 0) == RDNA3_OK &&
          dcn32_state_restore(dev, dm, &dc, &st, 1, 0, 0) == RDNA3_OK);
    CHECK(S.dcn_gated_drops == i && S.dcn_pg_no_request == 0);   /* powered before the writes */
    CHECK(S.dmub.nseq == 8);
    for (i = 0; i < 8; i++)
        CHECK(S.dmub.seq[i] == bring_up[i % 4] &&
              (bring_up[i % 4] != RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL || S.dmub.seq_tx_action[i] == bring_up_tx[i % 4]));
    /* the gamma LUT RAMs did not survive: output gamma of the boot head
     * and the degamma the GOP left on its DPP go to bypass */
    CHECK(S.reg[DCN_B3 + regMPCC_OGAM0_MPCC_OGAM_CONTROL + 0x5e * 1] == 0 &&
          S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL + RD_CLONE_DPP_STRIDE] == 0);
    S.reg[DCN_B3 + regMPCC_OGAM0_MPCC_OGAM_CONTROL + 0x5e * 1] = 2;
    S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL + RD_CLONE_DPP_STRIDE] = 1;
    CHECK(st_diff("after power loss") == 0);
    CHECK(dcn32_read_state(dev, &dp) == 2 && dp.head[1].h_active == 2560 && dp.head[1].dig_be == 1 &&
          dp.head[0].dig_be == 3 && dp.head[0].address == dp.head[1].address);
    S.dcn_frames = 1;
    CHECK(dcn32_state_check(dev, &st, 0, 50, &k) == 0 && dcn32_state_check(dev, &st, 1, 50, &k) == 0);
    S.dcn_frames = 0;
    /* restore into display sleep with the monitor in standby: no ENABLE */
    st_wipe(1, 2, 2, 1, 2, 1);
    S.dmub.nseq = 0;
    CHECK(dcn32_state_restore(dev, dm, &dc, &st, 1, 1, 1) == RDNA3_OK && S.dmub.nseq == 3 &&
          S.dmub.seq_tx_action[2] == RD_ATOM_TX_INIT);
    CHECK((S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK) &&
          (S.reg[OTG(regOTG0_OTG_CONTROL, 2)] & 1));
    CHECK(dcn32_head_link(dev, dm, &dc, &st, 1, 1) == RDNA3_OK && S.dmub.nseq == 4 &&
          S.dmub.seq_tx_action[3] == RD_ATOM_TX_ENABLE);

    /* leave the GOP's single head, as the tests after this expect */
    CHECK(dcn32_clone_disable(dev, dm, &cl) == RDNA3_OK);
    dcn32_state_drop(&st, 0);
    CHECK(dcn32_state_check(dev, &st, 0, 0, &k) == RDNA3_EINVAL);
    S.reg[DCN_B2 + 0x0475] = S.reg[DCN_B2 + 0x0476] = 0;
    S.reg[DCN_B1 + 0x007b] = 0;
    S.reg[DCN_B3 + regMPCC_OGAM0_MPCC_OGAM_CONTROL + 0x5e * 1] = 0;
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].pipe == 1);
}

/* ---- user queues through the MES (mes_userqueue.c with the gfx_v11_0 /
 * sdma_v6_0 MQDs and mes_v11_0 ADD_QUEUE / REMOVE_QUEUE) ---- */

struct uq_iv_seen { unsigned n; uint32_t db[8], client[8], src[8], ring[8], vmid[8], pasid[8]; };
static void uq_iv_cb(void *ctx, const rdna3_iv *iv)
{
    struct uq_iv_seen *s = (struct uq_iv_seen *)ctx;
    if (s->n < 8) {
        s->db[s->n] = ih60_iv_userq_doorbell(iv);
        s->client[s->n] = iv->client_id;
        s->src[s->n] = iv->src_id;
        s->ring[s->n] = iv->ring_id;
        s->vmid[s->n] = iv->vmid;
        s->pasid[s->n] = iv->pasid;
    }
    s->n++;
}

/* PM4 for a user queue: WRITE_DATA of 'v' to VA 'a', then a RELEASE_MEM
 * that writes 'fv' to VA 'fa' and interrupts (INT_SEL 2); 13 dwords */
static uint32_t uq_pm4(uint32_t *q, uint64_t a, uint32_t v, uint64_t fa, uint32_t fv)
{
    uint32_t n = 0;
    q[n++] = PACKET3(PACKET3_WRITE_DATA, 3);
    q[n++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
    q[n++] = (uint32_t)a;
    q[n++] = (uint32_t)(a >> 32);
    q[n++] = v;
    q[n++] = PACKET3(PACKET3_RELEASE_MEM, 6);
    q[n++] = 0x14;                          /* CACHE_FLUSH_AND_INV_TS_EVENT */
    q[n++] = (1u << 29) | (2u << 24);       /* DATA_SEL 1 (32 bit), INT_SEL 2 */
    q[n++] = (uint32_t)fa;
    q[n++] = (uint32_t)(fa >> 32);
    q[n++] = fv;
    q[n++] = 0;
    q[n++] = 0;
    return n;
}

/* rdna3amdgpu=1 GFX 11 ports: gfx_v11_0_sw_init's queue topology, the
 * padded commits, pipeline sync, KCQ priority, the soft reset's GFX index
 * mutex, the CP fault interrupts and the IB tests */
static void test_gfx_amdgpu(rdna3_dev *dev, rdna3_gfx_ring *gr, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq,
                            uint64_t ibva, uint32_t ndw, uint64_t pd)
{
    static rdna3_gfx_topo t;
    static const char *const comp[8] = { "comp_1.0.0", "comp_1.1.0", "comp_1.2.0", "comp_1.3.0",
                                         "comp_1.0.1", "comp_1.1.1", "comp_1.2.1", "comp_1.3.1" };
    struct hqd *hc = &S.hqd[RD_KCQ0_PIPE][0];
    rdna3_gfx_ib job = { ibva, ndw, 0 };
    rdna3_iv iv;
    uint64_t ibt = rdna3_vram_alloc(dev, 4096, 4096, "ib-test");
    uint32_t k, seq, polls0, ibs0, faults0, nop0;
    int a, b, c, which;

    /* gfx_v11_0_early_init + sw_init for GC 11.0.3: the rings of the host's dmesg */
    CHECK(gfx11_sw_init_topo(&t, 11, 0, 3, -1, 1, -1) == RDNA3_OK);
    CHECK(t.num_gfx_rings == 1 && t.num_compute_rings == 8 && t.num_mec == 2 && t.mec_pipes == 4 &&
          t.mec_queues == 4 && t.me_bitmap[0] == 1 && t.mec_bitmap[0] == 0x3333 && t.mec_bitmap[1] == 0);
    CHECK(!strcmp(t.gfx[0].name, "gfx_0.0.0") && t.gfx[0].doorbell_index == RD_DB_INDEX(RD_DB_GFX_RING0) &&
          t.gfx[0].irq_type == RD_CP_IRQ_GFX_ME0_PIPE0_EOP && t.gfx[0].hw_prio == RD_GFX_PIPE_PRIO_NORMAL);
    for (k = 0; k < 8; k++)
        CHECK(!strcmp(t.compute[k].name, comp[k]) &&
              t.compute[k].doorbell_index == RD_DB_INDEX(RD_DB_MEC_RING0 + k) &&
              t.compute[k].eop_offset == k * RD_GFX11_MEC_HPD_SIZE && t.compute[k].me == 1 &&
              t.compute[k].irq_type == RD_CP_IRQ_COMPUTE_MEC1_PIPE0_EOP + t.compute[k].pipe &&
              t.compute[k].hw_prio == (k ? RD_GFX_PIPE_PRIO_NORMAL : RD_GFX_PIPE_PRIO_HIGH));
    CHECK(gfx11_is_high_priority_compute_queue(&t, 0) && !gfx11_is_high_priority_compute_queue(&t, 1) &&
          !gfx11_is_high_priority_graphics_queue(&t, 0));
    CHECK(t.mec_hpd_size == 8 * RD_GFX11_MEC_HPD_SIZE && t.max_hw_contexts == 8 &&
          t.sc_prim_fifo_size_frontend == 0x20 && t.sc_prim_fifo_size_backend == 0x100 &&
          t.sc_hiz_tile_fifo_size == 0 && t.sc_earlyz_tile_fifo_size == 0x4C0);
    CHECK(gfx11_mec_queue_to_bit(&t, 1, 2, 3) == 27);
    gfx11_queue_mask_bit_to_mec_queue(&t, 27, &a, &b, &c);
    CHECK(a == 1 && b == 2 && c == 3);
    CHECK(gfx11_is_mec_queue_enabled(&t, 0, 3, 1) && !gfx11_is_mec_queue_enabled(&t, 0, 3, 2) &&
          !gfx11_is_mec_queue_enabled(&t, 1, 0, 0));
    gfx11_bit_to_me_queue(&t, 0, &a, &b, &c);
    CHECK(a == 0 && b == 0 && c == 0 && gfx11_me_queue_to_bit(&t, 0, 0, 0) == 0 &&
          gfx11_is_me_queue_enabled(&t, 0, 0, 0));
    CHECK(gfx11_get_num_kcq(-1) == 8 && gfx11_get_num_kcq(9) == 8 && gfx11_get_num_kcq(-2) == 8 &&
          gfx11_get_num_kcq(3) == 3);
    /* amdgpu.num_kcq=1: one normal-priority ring; compute_multipipe=0: whole pipes */
    CHECK(gfx11_sw_init_topo(&t, 11, 0, 3, 1, 1, -1) == RDNA3_OK && t.num_compute_rings == 1 &&
          t.compute[0].hw_prio == RD_GFX_PIPE_PRIO_NORMAL && t.mec_bitmap[0] == 1);
    CHECK(gfx11_sw_init_topo(&t, 11, 0, 3, -1, 1, 0) == RDNA3_OK && t.mec_bitmap[0] == 0xff &&
          !strcmp(t.compute[1].name, "comp_1.1.0") && !strcmp(t.compute[2].name, "comp_1.0.1") &&
          !strcmp(t.compute[7].name, "comp_1.1.3"));
    CHECK(gfx11_sw_init_topo(&t, 11, 0, 1, -1, 1, -1) == RDNA3_OK && t.num_mec == 1 &&
          t.sc_hiz_tile_fifo_size == 0x80 && t.sc_earlyz_tile_fifo_size == 0x300);
    CHECK(gfx11_sw_init_topo(&t, 12, 0, 0, -1, 1, -1) == RDNA3_EINVAL && t.mec_queues == 8);

    /* gfx_v11_0_select_me_pipe_q */
    gfx11_select_me_pipe_q(dev, 1, 2, 3, 4);
    CHECK(S.reg[GCR(regGRBM_GFX_CNTL)] == ((2u << GRBM_GFX_CNTL__PIPEID__SHIFT) | (1u << GRBM_GFX_CNTL__MEID__SHIFT) |
                                           (4u << GRBM_GFX_CNTL__VMID__SHIFT) | (3u << GRBM_GFX_CNTL__QUEUEID__SHIFT)));
    gfx11_select_me_pipe_q(dev, 0, 0, 0, 0);

    dev->amdgpu_exact = 1;
    /* amdgpu_ring_commit: every commit padded to 256 dwords with
     * gfx_v11_ring_insert_nop; gfx_v11_0_ring_get_wptr_* */
    gr->amdgpu_pad = kcq->rb.amdgpu_pad = 1;
    nop0 = S.cp_nop1;
    polls0 = S.cp_mem_polls;
    seq = gfx11_submit_job(dev, gr, 0x77, 5, pd, &job, 1);
    CHECK(seq && gfx11_wait_fence(dev, gr, seq) == RDNA3_OK && (gr->wptr & 0xff) == 0);
    CHECK(gfx11_ring_get_wptr(dev, gr) == gr->wptr);
    seq = gfx11_submit_compute_job(dev, kcq, 0x78, 5, pd, &job, 1);
    CHECK(seq && gfx11_wait_fence(dev, &kcq->rb, seq) == RDNA3_OK && (kcq->rb.wptr & 0xff) == 0);
    CHECK(gfx11_ring_get_wptr(dev, &kcq->rb) == kcq->rb.wptr);
    /* amdgpu_vm_need_pipeline_sync: both jobs flushed VMID 5 -> one memory
     * poll each, on a fence that had landed */
    CHECK(S.cp_mem_polls == polls0 + 2 && S.cp_nop1 > nop0 && S.cp_bad == 0 && S.cp_wait_stuck == 0);
    seq = gfx11_submit_compute_job(dev, kcq, 0x78, 0, 0, &job, 1);   /* VMID 0: no sync */
    CHECK(seq && gfx11_wait_fence(dev, &kcq->rb, seq) == RDNA3_OK && S.cp_mem_polls == polls0 + 2);
    gfx11_ring_insert_nop(dev, &kcq->rb, 1);
    gfx11_ring_insert_nop(dev, &kcq->rb, 5);
    gfx11_commit(dev, &kcq->rb);
    CHECK((kcq->rb.wptr & 0xff) == 0 && S.cp_bad == 0);

    /* gfx_v11_0_ring_test_ib on both rings (IB in VMID 0) */
    ibs0 = S.cp_ibs;
    CHECK(gfx11_ring_test_ib(dev, gr, ibt) == RDNA3_OK && gfx11_ring_test_ib(dev, &kcq->rb, ibt) == RDNA3_OK);
    CHECK(S.cp_ibs == ibs0 + 2 && S.cp_bad == 0);
    CHECK(gfx11_ring_test_ib(dev, gr, UINT64_MAX) == RDNA3_EINVAL);

    /* gfx_v11_0_late_init: the CP fault sources on ME0 pipe 0 and MEC1 pipes 0-3 */
    S.reg[GCR(regCP_INT_CNTL_RING1)] = 0;
    gfx11_late_init(dev);
    CHECK((S.reg[GCR(regCP_INT_CNTL_RING0)] & 0x01C00000u) == 0x01C00000u && S.reg[GCR(regCP_INT_CNTL_RING1)] == 0);
    for (k = 0; k < 4; k++)
        CHECK((S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + k] & 0x01C00000u) == 0x01800000u);
    /* a bad opcode on the KCQ raises CP_BAD_OPCODE_ERROR naming me 1 pipe 0 queue 0 */
    faults0 = S.cp_fault_ivs;
    gfx11_write(dev, &kcq->rb, PACKET3(0xFE, 0));
    gfx11_write(dev, &kcq->rb, 0);
    gfx11_commit(dev, &kcq->rb);
    CHECK(S.cp_fault_ivs == faults0 + 1 && S.cp_bad == 1);
    S.cp_bad = 0;
    memset(&iv, 0, sizeof(iv));
    iv.client_id = RD_IH_CLIENT_GRBM_CP;
    iv.src_id = RD_IH_SRC_CP_BAD_OPCODE;
    iv.ring_id = (0u << 4) | (1u << 2) | 0u;
    CHECK(gfx11_handle_priv_fault(&iv, gr, kcq, &which) == 1 && which == 1);
    iv.src_id = RD_IH_SRC_CP_PRIV_REG;
    iv.ring_id = 0;
    CHECK(gfx11_handle_priv_fault(&iv, gr, kcq, &which) == 1 && which == 0);
    iv.src_id = RD_IH_SRC_CP_PRIV_INSTR;
    iv.ring_id = (1u << 2) | 2u;   /* me 1 pipe 2: not ours */
    CHECK(gfx11_handle_priv_fault(&iv, gr, kcq, &which) == 1 && which == -1);
    iv.src_id = RD_IH_SRC_CP_EOP;
    CHECK(gfx11_handle_priv_fault(&iv, gr, kcq, &which) == 0);
    /* gfx_v11_0_hw_fini puts them */
    gfx11_set_priv_reg_fault_state(dev, 0);
    gfx11_set_priv_inst_fault_state(dev, 0);
    gfx11_set_bad_op_fault_state(dev, 0);
    CHECK(!(S.reg[GCR(regCP_INT_CNTL_RING0)] & 0x01C00000u) && !(S.reg[GCR(regCP_ME1_PIPE3_INT_CNTL)] & 0x01C00000u));

    /* gfx_v11_0_update_spm_vmid, gfx_v11_0_get_tcc_info, wait_for_idle */
    S.reg[GCR(regRLC_SPM_MC_CNTL)] = 0x30;
    gfx11_update_spm_vmid(dev, 0xf);
    CHECK(S.reg[GCR(regRLC_SPM_MC_CNTL)] == 0x3f);
    S.reg[GCR(regCGTS_TCC_DISABLE)] = 0x00030100u;
    S.reg[GCR(regCGTS_USER_TCC_DISABLE)] = 0x00040000u;
    CHECK(gfx11_get_tcc_info(dev) == 0x00010007u);
    S.reg[GCR(regCGTS_TCC_DISABLE)] = S.reg[GCR(regCGTS_USER_TCC_DISABLE)] = 0;
    CHECK(gfx11_wait_for_idle(dev) == RDNA3_OK);

    /* gfx_v11_0_request_gfx_index_mutex: granted, released; a firmware
     * holding it makes the request time out */
    CHECK(gfx11_request_gfx_index_mutex(dev, 1) == RDNA3_OK && S.reg[GCR(regCP_GFX_INDEX_MUTEX)] == 0x9);
    CHECK(gfx11_request_gfx_index_mutex(dev, 0) == RDNA3_OK && S.reg[GCR(regCP_GFX_INDEX_MUTEX)] == 0x8);
    S.gfx_mutex_fw = 1;
    CHECK(gfx11_request_gfx_index_mutex(dev, 1) == RDNA3_EINVAL);
    S.gfx_mutex_fw = 0;

    /* gfx_v11_0_soft_reset with the mutex, then the KCQ comes back as
     * amdgpu's high priority comp_1.0.0 (pipe HIGH, queue 15, tunneling) */
    kcq->high_prio = 1;
    CHECK(gfx11_soft_reset(dev, gr, kiq, kcq) == RDNA3_OK && gr->ready && kcq->rb.ready);
    CHECK(S.reg[GCR(regCP_GFX_INDEX_MUTEX)] == 0x8 && S.reg[GCR(regGRBM_GFX_CNTL)] == 0);
    CHECK(HQD(hc, regCP_HQD_PIPE_PRIORITY) == RD_GFX_PIPE_PRIO_HIGH &&
          HQD(hc, regCP_HQD_QUEUE_PRIORITY) == RD_GFX_QUEUE_PRIORITY_MAXIMUM &&
          (HQD(hc, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH_MASK));
    seq = gfx11_submit_compute_job(dev, kcq, 0x79, 5, pd, &job, 1);
    CHECK(seq && gfx11_wait_fence(dev, &kcq->rb, seq) == RDNA3_OK && S.cp_bad == 0 && S.cp_wait_stuck == 0);
    S.gfx_mutex_fw = 1;
    CHECK(gfx11_soft_reset(dev, gr, kiq, kcq) == RDNA3_EINVAL && !S.rlc_safe);
    S.gfx_mutex_fw = 0;
    CHECK(gfx11_soft_reset(dev, gr, kiq, kcq) == RDNA3_OK && gr->ready && kcq->rb.ready);
    kcq->high_prio = 0;
    gr->amdgpu_pad = kcq->rb.amdgpu_pad = 0;
    dev->amdgpu_exact = 0;
}

static void test_userq_hw(rdna3_dev *dev, rdna3_mes *mes, rdna3_gmc *gmc)
{
    static rdna3_pt_pool pool;
    static rdna3_vm vm;
    static rdna3_ih ih;
    static uint32_t mqd[512];
    const uint64_t va = 0x0000600000000000ull;
    /* pages of one VRAM chunk; 0-7 are also mapped at 'va' in the process VM */
    enum { P_RING_C, P_RING_G, P_RING_S, P_WP_C, P_WP_G, P_WP_S, P_DST, P_EOP,
           P_FENCE, P_MQD_C, P_MQD_G, P_MQD_S, P_CTX_C, P_CTX_G = P_CTX_C + 2, P_CTX_S = P_CTX_G + 2,
           P_END = P_CTX_S + 2 };
    uint64_t chunk = rdna3_vram_alloc(dev, P_END * 4096ull, 4096, "uq-test");
    const uint32_t dbc = 0x1810, dbg = 0x1812, dbs = 0x1820;   /* dword indices in BAR2 */
    uint32_t sav[6], q[64], n, v, ver0 = mes->sched_version, failed_at_start = mes->failed, i;
    uint64_t pd, w, got;
    struct uq_iv_seen seen;
    amd_mqd_prop pr;
    amd_mes_add_queue in;
    amd_mes_remove_queue rm;
#define PG(i_) (chunk + (uint64_t)(i_) * 4096)
#define MC(i_) rdna3_vram_mc(dev, PG(i_))
#define VA(i_) (va + (uint64_t)(i_) * 4096)
    CHECK(chunk != UINT64_MAX);
    rdna3_vram_fill(dev, chunk, 0, P_END * 4096ull);
    memset(&pool, 0, sizeof(pool));
    memset(&vm, 0, sizeof(vm));
    CHECK(gpuvm_pool_init(dev, &pool, 16) == RDNA3_OK && gpuvm_vm_init(dev, &vm, &pool) == RDNA3_OK);
    CHECK(gpuvm_map_linear(dev, &vm, va, rdna3_vram_pa(dev, chunk), 8,
                           gpuvm_pte_flags((1u << 1) | (1u << 2) | (1u << 3), 0)) == RDNA3_OK);
    pd = gpuvm_pd_addr(dev, &vm);
    CHECK(ih60_init(dev, &ih, 4096, gmc->dummy_page_pa, 0) == RDNA3_OK);
    memset(&S.uq, 0, sizeof(S.uq));
    S.uq_adds = S.uq_removes = S.uq_bad = S.uq_faults = S.uq_runs = S.uq_eops = 0;
    mes->sched_version = 0x0000207a;          /* API version 2, firmware 122 */

    /* the gates of gfx_v11_0_gpu_early_init / sdma_v6_0_sw_init */
    CHECK(gfx11_userq_supported(2390, 2530, 2600, 120) && !gfx11_userq_supported(2389, 2530, 2600, 120) &&
          !gfx11_userq_supported(2390, 2530, 2600, 119) && !gfx11_userq_supported(2390, 2529, 2600, 120) &&
          !gfx11_userq_supported(2390, 2530, 2599, 120));
    CHECK(sdma60_userq_supported(25) && !sdma60_userq_supported(24));

    /* the EOP interrupts of every pipe a user queue may land on, the SDMA traps */
    sav[0] = S.reg[GCR(regCP_INT_CNTL_RING0)];
    for (i = 0; i < 4; i++)
        sav[1 + i] = S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + i];
    sav[5] = S.reg[GC_BASE0 + regSDMA0_CNTL];
    gfx11_set_userq_eop_interrupts(dev, 3, 1);
    sdma60_set_userq_trap_interrupts(dev, 2, 1);
    CHECK(S.reg[GCR(regCP_INT_CNTL_RING0)] & CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK);
    for (i = 0; i < 4; i++)
        CHECK(S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + i] & CP_ME1_PIPE0_INT_CNTL__TIME_STAMP_INT_ENABLE_MASK);
    CHECK(S.reg[GC_BASE0 + regSDMA0_CNTL] & SDMA0_CNTL__TRAP_ENABLE_MASK);
    CHECK(S.reg[GC_BASE0 + regSDMA0_CNTL + 0x600] & SDMA0_CNTL__TRAP_ENABLE_MASK);

    /* compute: gfx_v11_0_compute_mqd_init from mes_userq_mqd_create's props */
    memset(&pr, 0, sizeof(pr));
    pr.mqd_gpu_addr = MC(P_MQD_C);
    pr.hqd_base_gpu_addr = VA(P_RING_C);
    pr.rptr_gpu_addr = VA(P_WP_C) + 8;
    pr.wptr_gpu_addr = VA(P_WP_C);
    pr.queue_size = 4096;
    pr.use_doorbell = 1;
    pr.doorbell_index = dbc;
    pr.eop_gpu_addr = VA(P_EOP);
    pr.hqd_pipe_priority = AMD_GFX_PIPE_PRIO_NORMAL;
    pr.hqd_queue_priority = AMD_GFX_QUEUE_PRIORITY_MINIMUM;
    pr.fence_address = MC(P_FENCE);
    memset(mqd, 0, sizeof(mqd));
    CHECK(gfx11_compute_mqd_init(dev, mqd, &pr) == RDNA3_OK);
    CHECK(mqd[0] == 0xC0310800u && mqd[128] == (uint32_t)MC(P_MQD_C) && mqd[129] == (uint32_t)(MC(P_MQD_C) >> 32));
    CHECK(mqd[136] == (uint32_t)(VA(P_RING_C) >> 8) && mqd[137] == (uint32_t)(VA(P_RING_C) >> 40));
    CHECK((mqd[145] & CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK) == 9 && (mqd[145] & CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK) &&
          (mqd[145] & CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK) && !(mqd[145] & CP_HQD_PQ_CONTROL__TMZ_MASK));
    CHECK(((mqd[143] & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == dbc &&
          (mqd[143] & CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK));
    CHECK(mqd[165] == (uint32_t)(VA(P_EOP) >> 8) && mqd[130] == 0 && mqd[133] == 1 && mqd[134] == 0);
    CHECK(mqd[139] == (uint32_t)(VA(P_WP_C) + 8) && mqd[141] == (uint32_t)VA(P_WP_C) && mqd[140] == 0x6000);
    CHECK(mqd[446] == (uint32_t)MC(P_FENCE) && mqd[447] == (uint32_t)(MC(P_FENCE) >> 32));
    rdna3_vram_write(dev, PG(P_MQD_C), mqd, 2048);

    /* ADD_QUEUE: the process gets VMID 8, the first of the GFXHUB mask */
    memset(&in, 0, sizeof(in));
    in.process_id = 42;
    in.page_table_base_addr = pd;
    in.process_va_end = (1ull << 36) - 1;
    in.process_quantum = 100000;
    in.gang_quantum = 10000;
    in.process_context_addr = MC(P_CTX_C);
    in.gang_context_addr = MC(P_CTX_C) + 4096;
    in.inprocess_gang_priority = AMD_MES_PRIORITY_LEVEL_NORMAL;
    in.gang_global_priority_level = AMD_MES_PRIORITY_LEVEL_NORMAL;
    in.doorbell_offset = dbc;
    in.mqd_addr = MC(P_MQD_C);
    in.wptr_addr = VA(P_WP_C);
    in.wptr_mc_addr = MC(P_WP_C);
    in.queue_type = 1;
    in.queue_size = 1024;
    CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_OK);
    CHECK(S.uq_adds == 1 && S.uq[0].used && S.uq[0].vmid == 8 && S.uq[0].type == 1 && S.uq[0].me == 1);
    CHECK(S.uq[0].wptr_addr == MC(P_WP_C) && S.uq[0].queue_size_dw == 1024 && S.uq[0].pd == pd);
    CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 +
                8 * (regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32)] ==
          (uint32_t)pd);
    /* the client writes its ring and wptr, rings the doorbell; the work
     * lands through its VM, the fence slot gets the rptr, the EOP IV names
     * the doorbell */
    n = uq_pm4(q, VA(P_DST), 0xC0DE0001, VA(P_DST) + 4, 0x77);
    rdna3_vram_write(dev, PG(P_RING_C), q, 4ull * n);
    w = n;
    rdna3_vram_write(dev, PG(P_WP_C), &w, 8);
    rdna3_hdp_flush(dev);
    rdna3_doorbell_wr64(dev, dbc, w);
    rdna3_vram_read(dev, PG(P_DST), &v, 4);
    CHECK(v == 0xC0DE0001 && S.uq_runs == 1 && S.uq_eops == 1 && S.uq_faults == 0);
    rdna3_vram_read(dev, PG(P_DST) + 4, &v, 4);
    CHECK(v == 0x77);
    rdna3_vram_read(dev, PG(P_FENCE), &got, 8);
    CHECK(got == n);
    rdna3_vram_read(dev, PG(P_WP_C) + 8, &got, 8);
    CHECK(got == n);
    memset(&seen, 0, sizeof(seen));
    CHECK(ih60_process(dev, &ih, uq_iv_cb, &seen) == 1);
    CHECK(seen.n == 1 && seen.db[0] == dbc && seen.client[0] == RD_IH_CLIENT_GRBM_CP &&
          seen.src[0] == RD_IH_SRC_CP_EOP && seen.vmid[0] == 8 && seen.pasid[0] == 42);
    CHECK(((seen.ring[0] >> 2) & 3) == 1);   /* MEC1 */
    /* more work on the same queue: the rptr runs on */
    n = uq_pm4(q, VA(P_DST) + 8, 0xC0DE0002, VA(P_DST) + 12, 0x78);
    rdna3_vram_write(dev, PG(P_RING_C) + 4ull * w, q, 4ull * n);
    w += n;
    rdna3_vram_write(dev, PG(P_WP_C), &w, 8);
    rdna3_hdp_flush(dev);
    rdna3_doorbell_wr64(dev, dbc, w);
    rdna3_vram_read(dev, PG(P_FENCE), &got, 8);
    CHECK(got == w && w == 26);
    rdna3_vram_read(dev, PG(P_DST) + 8, &v, 4);
    CHECK(v == 0xC0DE0002);

    /* gfx: gfx_v11_0_gfx_mqd_init with the shadow / CSA / fence the user
     * queue adds; the same process keeps its VMID */
    memset(&pr, 0, sizeof(pr));
    pr.mqd_gpu_addr = MC(P_MQD_G);
    pr.hqd_base_gpu_addr = VA(P_RING_G);
    pr.rptr_gpu_addr = VA(P_WP_G) + 8;
    pr.wptr_gpu_addr = VA(P_WP_G);
    pr.queue_size = 4096;
    pr.use_doorbell = 1;
    pr.doorbell_index = dbg;
    pr.shadow_addr = 0x0000700000000000ull;
    pr.csa_addr = 0x0000700000100000ull;
    pr.fence_address = MC(P_FENCE) + 8;
    memset(mqd, 0, sizeof(mqd));
    CHECK(gfx11_gfx_mqd_init(dev, mqd, &pr) == RDNA3_OK);
    CHECK(mqd[0] == 0 && mqd[1] == 0x7000 && mqd[4] == 0x00100000 && mqd[5] == 0x7000);
    CHECK(mqd[510] == (uint32_t)(MC(P_FENCE) + 8) && mqd[128] == (uint32_t)MC(P_MQD_G) && mqd[130] == 1);
    CHECK(((mqd[143] & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> 2) == dbg &&
          (mqd[143] & CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK));
    CHECK((mqd[145] & CP_GFX_HQD_CNTL__RB_BUFSZ_MASK) == 9 &&
          ((mqd[145] & CP_GFX_HQD_CNTL__RB_BLKSZ_MASK) >> CP_GFX_HQD_CNTL__RB_BLKSZ__SHIFT) == 7 &&
          !(mqd[145] & CP_GFX_HQD_CNTL__TMZ_MATCH_MASK));
    CHECK(mqd[134] == 0 && (mqd[135] & CP_GFX_HQD_QUANTUM__QUANTUM_EN_MASK) &&
          (mqd[162] & CP_GFX_MQD_CONTROL__PRIV_STATE_MASK) && mqd[136] == (uint32_t)(VA(P_RING_G) >> 8));
    rdna3_vram_write(dev, PG(P_MQD_G), mqd, 2048);
    {   /* a secure high-priority queue: TMZ_MATCH and priority level 1 */
        static uint32_t m2[512];
        amd_mqd_prop p2 = pr;
        p2.tmz_queue = 1;
        p2.hqd_pipe_priority = AMD_GFX_PIPE_PRIO_HIGH;
        memset(m2, 0, sizeof(m2));
        CHECK(gfx11_gfx_mqd_init(dev, m2, &p2) == RDNA3_OK);
        CHECK((m2[145] & CP_GFX_HQD_CNTL__TMZ_MATCH_MASK) && (m2[134] & CP_GFX_HQD_QUEUE_PRIORITY__PRIORITY_LEVEL_MASK));
    }
    in.doorbell_offset = dbg;
    in.mqd_addr = MC(P_MQD_G);
    in.wptr_addr = VA(P_WP_G);
    in.wptr_mc_addr = MC(P_WP_G);
    in.process_context_addr = MC(P_CTX_G);
    in.gang_context_addr = MC(P_CTX_G) + 4096;
    in.queue_type = 0;
    CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_OK);
    CHECK(S.uq_adds == 2 && S.uq[1].vmid == 8 && S.uq[1].me == 0 && S.uq[1].queue == 1);
    n = uq_pm4(q, VA(P_DST) + 16, 0xC0DE0003, VA(P_DST) + 20, 0x79);
    rdna3_vram_write(dev, PG(P_RING_G), q, 4ull * n);
    w = n;
    rdna3_vram_write(dev, PG(P_WP_G), &w, 8);
    rdna3_hdp_flush(dev);
    rdna3_doorbell_wr64(dev, dbg, w);
    rdna3_vram_read(dev, PG(P_DST) + 16, &v, 4);
    CHECK(v == 0xC0DE0003 && S.uq_faults == 0);
    rdna3_vram_read(dev, PG(P_FENCE) + 8, &got, 8);
    CHECK(got == n);
    memset(&seen, 0, sizeof(seen));
    CHECK(ih60_process(dev, &ih, uq_iv_cb, &seen) == 2);   /* the second compute run, then gfx */
    CHECK(seen.db[0] == dbc && seen.db[1] == dbg && seen.ring[1] == ((1u << 4) | 0));

    /* SDMA: sdma_v6_0_mqd_init; another process, VMID 9 */
    memset(&pr, 0, sizeof(pr));
    pr.mqd_gpu_addr = MC(P_MQD_S);
    pr.hqd_base_gpu_addr = VA(P_RING_S);
    pr.rptr_gpu_addr = VA(P_WP_S) + 8;
    pr.wptr_gpu_addr = VA(P_WP_S);
    pr.queue_size = 4096;
    pr.use_doorbell = 1;
    pr.doorbell_index = dbs;
    pr.csa_addr = 0x0000700000200000ull;
    pr.fence_address = MC(P_FENCE) + 16;
    S.reg[GC_BASE0 + regSDMA0_QUEUE0_IB_CNTL] = 0x00000101;
    memset(mqd, 0, sizeof(mqd));
    CHECK(sdma60_mqd_init(dev, mqd, &pr) == RDNA3_OK);
    CHECK(((mqd[0] & SDMA0_QUEUE0_RB_CNTL__RB_SIZE_MASK) >> SDMA0_QUEUE0_RB_CNTL__RB_SIZE__SHIFT) == 10 &&
          (mqd[0] & SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK) &&
          (mqd[0] & SDMA0_QUEUE0_RB_CNTL__F32_WPTR_POLL_ENABLE_MASK));
    CHECK(mqd[1] == (uint32_t)(VA(P_RING_S) >> 8) && mqd[2] == (uint32_t)(VA(P_RING_S) >> 40));
    CHECK(mqd[8] == (uint32_t)(VA(P_WP_S) + 8) && mqd[7] == 0x6000 && mqd[27] == (uint32_t)VA(P_WP_S));
    CHECK(mqd[9] == 0x101 && mqd[19] == dbs << 2 && (mqd[17] & SDMA0_QUEUE0_DOORBELL__ENABLE_MASK));
    CHECK(mqd[20] == 0x00200000 && mqd[21] == 0x7000 && mqd[28] == 0x4000 && mqd[25] == 0xf);
    CHECK(mqd[43] == (uint32_t)(MC(P_FENCE) + 16) && mqd[44] == (uint32_t)((MC(P_FENCE) + 16) >> 32));
    rdna3_vram_write(dev, PG(P_MQD_S), mqd, 512);
    in.process_id = 43;
    in.doorbell_offset = dbs;
    in.mqd_addr = MC(P_MQD_S);
    in.wptr_addr = VA(P_WP_S);
    in.wptr_mc_addr = MC(P_WP_S);
    in.process_context_addr = MC(P_CTX_S);
    in.gang_context_addr = MC(P_CTX_S) + 4096;
    in.queue_type = 2;
    CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_OK);
    CHECK(S.uq_adds == 3 && S.uq[2].vmid == 9 && S.uq[2].type == 2);
    n = 0;
    q[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_WRITE);
    q[n++] = (uint32_t)(VA(P_DST) + 24);
    q[n++] = (uint32_t)((VA(P_DST) + 24) >> 32);
    q[n++] = 0;                               /* one dword */
    q[n++] = 0xC0DE0004;
    q[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    q[n++] = (uint32_t)(VA(P_DST) + 28);
    q[n++] = (uint32_t)((VA(P_DST) + 28) >> 32);
    q[n++] = 0x7a;
    rdna3_vram_write(dev, PG(P_RING_S), q, 4ull * n);
    w = 4ull * n;                             /* SDMA counts bytes */
    rdna3_vram_write(dev, PG(P_WP_S), &w, 8);
    rdna3_hdp_flush(dev);
    rdna3_doorbell_wr64(dev, dbs, w);
    rdna3_vram_read(dev, PG(P_DST) + 24, &v, 4);
    CHECK(v == 0xC0DE0004 && S.uq_faults == 0);
    rdna3_vram_read(dev, PG(P_DST) + 28, &v, 4);
    CHECK(v == 0x7a);
    rdna3_vram_read(dev, PG(P_FENCE) + 16, &got, 8);
    CHECK(got == w);
    rdna3_vram_read(dev, PG(P_WP_S) + 8, &got, 8);
    CHECK(got == w);
    memset(&seen, 0, sizeof(seen));
    CHECK(ih60_process(dev, &ih, uq_iv_cb, &seen) == 1);
    CHECK(seen.db[0] == dbs && seen.client[0] == RD_IH_CLIENT_GFX && seen.src[0] == RD_IH_SRC_SDMA_FENCE &&
          seen.vmid[0] == 9 && seen.pasid[0] == 43);

    /* what the scheduler refuses: no status fence, the submit times out */
    {
        uint32_t failed0 = mes->failed, bad0 = S.uq_bad;
        in.queue_type = 3;
        CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_EINVAL);   /* no frame */
        in.queue_type = 2;
        CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_ETIME);    /* doorbell taken */
        in.doorbell_offset = dbs + 2;                                                    /* not the MQD's */
        CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_ETIME);
        rm.doorbell_offset = dbs;
        rm.gang_context_addr = MC(P_CTX_C) + 4096;                                       /* not its gang */
        CHECK(gfx11_mes_remove_hw_queue(dev, mes, &rm) == RDNA3_ETIME);
        CHECK(mes->failed == failed0 + 3 && S.uq_bad == bad0 + 3 && S.uq_adds == 3 && S.uq_removes == 0);
    }
    /* REMOVE_QUEUE: the doorbells reach nothing any more */
    rm.doorbell_offset = dbc;
    rm.gang_context_addr = MC(P_CTX_C) + 4096;
    CHECK(gfx11_mes_remove_hw_queue(dev, mes, &rm) == RDNA3_OK);
    rm.doorbell_offset = dbg;
    rm.gang_context_addr = MC(P_CTX_G) + 4096;
    CHECK(gfx11_mes_remove_hw_queue(dev, mes, &rm) == RDNA3_OK);
    rm.doorbell_offset = dbs;
    rm.gang_context_addr = MC(P_CTX_S) + 4096;
    CHECK(gfx11_mes_remove_hw_queue(dev, mes, &rm) == RDNA3_OK);
    CHECK(S.uq_removes == 3 && !S.uq[0].used && !S.uq[1].used && !S.uq[2].used);
    {
        uint32_t un0 = S.db_unclaimed;
        rdna3_doorbell_wr64(dev, dbc, 40);
        CHECK(S.db_unclaimed == un0 + 1);
    }
    /* before MES API version 2 the frame carries the wptr's VA */
    mes->sched_version = 0x0000007a;
    in.process_id = 42;
    in.doorbell_offset = dbc;
    in.mqd_addr = MC(P_MQD_C);
    in.wptr_addr = VA(P_WP_C);
    in.wptr_mc_addr = MC(P_WP_C);
    in.process_context_addr = MC(P_CTX_C);
    in.gang_context_addr = MC(P_CTX_C) + 4096;
    in.queue_type = 1;
    CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_OK);
    CHECK(S.uq[0].used && S.uq[0].wptr_addr == VA(P_WP_C));
    rm.doorbell_offset = dbc;
    rm.gang_context_addr = MC(P_CTX_C) + 4096;
    CHECK(gfx11_mes_remove_hw_queue(dev, mes, &rm) == RDNA3_OK);
    /* a scheduler that is not up takes nothing */
    mes->ready = 0;
    CHECK(gfx11_mes_add_hw_queue(dev, mes, &in, gmc->vm_cntx_cntl) == RDNA3_EINVAL &&
          gfx11_mes_remove_hw_queue(dev, mes, &rm) == RDNA3_EINVAL);
    mes->ready = 1;
    CHECK(S.uq_faults == 0 && S.cp_bad == 0);

    mes->sched_version = ver0;
    mes->failed = failed_at_start;   /* the refusals above were on purpose */
    S.reg[GCR(regCP_INT_CNTL_RING0)] = sav[0];
    for (i = 0; i < 4; i++)
        S.reg[GCR(regCP_ME1_PIPE0_INT_CNTL) + i] = sav[1 + i];
    S.reg[GC_BASE0 + regSDMA0_CNTL] = sav[5];
    ih60_disable(dev, &ih);
    gpuvm_vm_fini(dev, &vm);
#undef PG
#undef MC
#undef VA
}

/* ---- user queues end to end: uapi/rdna3_drm.c (amdgpu_userq*) on the
 * simulated MES, with the OS hooks the kext provides ---- */

static struct {
    rdna3_dev *dev;
    rdna3_mes *mes;
    rdna3_gmc *gmc;
    uint64_t heap, heap_next, heap_end;    /* VRAM BOs: bump allocated */
    rdna3_pt_pool pool;
    struct uq_vm { int used; rdna3_vm vm; uint32_t pasid; } vms[4];
    unsigned waits, adds, removes;
} UQ;

static void *uqo_zalloc(void *c, size_t n) { (void)c; return calloc(1, n); }
static void uqo_free(void *c, void *p, size_t n) { (void)c; (void)n; free(p); }
static int uqo_copyin(void *c, void *d, uint64_t u, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)u, n); return 0; }
static int uqo_copyout(void *c, uint64_t u, const void *src, size_t n) { (void)c; memcpy((void *)(uintptr_t)u, src, n); return 0; }
static int uqo_bo_alloc(void *c, rdna3_bo *bo)
{
    uint64_t a = bo->alignment > 4096 ? bo->alignment : 4096, off = (UQ.heap_next + a - 1) & ~(a - 1);
    (void)c;
    if (off + bo->size > UQ.heap_end)
        return -RD_ENOMEM;
    bo->placement = AMDGPU_GEM_DOMAIN_VRAM;   /* everything in VRAM here */
    bo->vram_offset = off;
    UQ.heap_next = off + bo->size;
    rdna3_vram_fill(UQ.dev, off, 0, bo->size);
    return 0;
}
static void uqo_bo_free(void *c, rdna3_bo *bo) { (void)c; (void)bo; }
static void *uqo_vm_create(void *c)
{
    (void)c;
    for (uint32_t i = 0; i < 4; i++)
        if (!UQ.vms[i].used) {
            memset(&UQ.vms[i].vm, 0, sizeof(UQ.vms[i].vm));
            if (gpuvm_vm_init(UQ.dev, &UQ.vms[i].vm, &UQ.pool) != RDNA3_OK)
                return NULL;
            UQ.vms[i].used = 1;
            UQ.vms[i].pasid = 100 + i;
            return &UQ.vms[i];
        }
    return NULL;
}
static void uqo_vm_destroy(void *c, void *vm)
{
    (void)c;
    gpuvm_vm_fini(UQ.dev, &((struct uq_vm *)vm)->vm);
    ((struct uq_vm *)vm)->used = 0;
}
static int uqo_vm_map(void *c, void *vm, rdna3_bo *bo, uint64_t va, uint64_t off, uint64_t size, uint32_t flags)
{
    (void)c;
    return gpuvm_map_linear(UQ.dev, &((struct uq_vm *)vm)->vm, va, rdna3_vram_pa(UQ.dev, bo->vram_offset + off),
                            size / 4096, gpuvm_pte_flags(flags, 0)) == RDNA3_OK ? 0 : -RD_ENOMEM;
}
static void uqo_vm_unmap(void *c, void *vm, uint64_t va, uint64_t size)
{
    (void)c;
    gpuvm_unmap(UQ.dev, &((struct uq_vm *)vm)->vm, va, size / 4096);
}
static void *uqo_bo_kmap(void *c, rdna3_bo *bo)
{
    (void)c;
    return bo->placement == AMDGPU_GEM_DOMAIN_VRAM ? (void *)(S.vram + bo->vram_offset) : NULL;
}
static int uqo_bo_gpu_addr(void *c, rdna3_bo *bo, uint64_t *mc)
{
    (void)c;
    if (bo->placement != AMDGPU_GEM_DOMAIN_VRAM)
        return -RD_EINVAL;
    *mc = rdna3_vram_mc(UQ.dev, bo->vram_offset);
    return 0;
}
static int uqo_mqd_init(void *c, uint32_t ip, void *mqd, const amd_mqd_prop *p)
{
    (void)c;
    if (ip == AMDGPU_HW_IP_GFX)
        return gfx11_gfx_mqd_init(UQ.dev, (uint32_t *)mqd, p) ? -RD_EINVAL : 0;
    if (ip == AMDGPU_HW_IP_COMPUTE)
        return gfx11_compute_mqd_init(UQ.dev, (uint32_t *)mqd, p) ? -RD_EINVAL : 0;
    return sdma60_mqd_init(UQ.dev, (uint32_t *)mqd, p) ? -RD_EINVAL : 0;
}
static int uqo_add(void *c, void *vm, amd_mes_add_queue *in)
{
    int r;
    (void)c;
    in->process_id = ((struct uq_vm *)vm)->pasid;
    in->page_table_base_addr = gpuvm_pd_addr(UQ.dev, &((struct uq_vm *)vm)->vm);
    r = gfx11_mes_add_hw_queue(UQ.dev, UQ.mes, in, UQ.gmc->vm_cntx_cntl);
    UQ.adds += !r;
    return r == RDNA3_OK ? 0 : r == RDNA3_ETIME ? -RD_ETIME : -RD_EINVAL;
}
static int uqo_remove(void *c, const amd_mes_remove_queue *in)
{
    int r = gfx11_mes_remove_hw_queue(UQ.dev, UQ.mes, in);
    (void)c;
    UQ.removes += !r;
    return r == RDNA3_OK ? 0 : r == RDNA3_ETIME ? -RD_ETIME : -RD_EINVAL;
}
/* the GPU here runs at the doorbell: whatever is not done now never is */
static int uqo_wait(void *c, int (*done)(void *), void *arg, uint64_t abs_ns)
{
    (void)c; (void)abs_ns;
    UQ.waits++;
    return done(arg) ? 0 : -RD_ETIME;
}
static uint64_t uqo_now(void *c) { (void)c; return S.now_us * 1000; }
static int uqo_privileged(void *c) { (void)c; return 0; }

#define UQ_IOCTL(f_, nr_, a_) rdna3_drm_ioctl((f_), (nr_), &(a_), sizeof(a_))

static uint32_t uq_gem(rdna3_drm_file *f, uint64_t size, uint32_t dom)
{
    union drm_amdgpu_gem_create gc;
    memset(&gc, 0, sizeof(gc));
    gc.in.bo_size = size;
    gc.in.alignment = 4096;
    gc.in.domains = dom;
    return UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) ? 0 : gc.out.handle;
}

static int uq_va(rdna3_drm_file *f, uint32_t h, uint64_t va, uint64_t size)
{
    struct drm_amdgpu_gem_va a;
    memset(&a, 0, sizeof(a));
    a.handle = h;
    a.operation = AMDGPU_VA_OP_MAP;
    a.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
    a.va_address = va;
    a.map_size = size;
    return UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), a);
}

static int uq_create(rdna3_drm_file *f, uint32_t ip, uint32_t db, uint32_t dboff, uint64_t qva, uint64_t qsize,
                     uint64_t rptr, uint64_t wptr, const void *mqd, uint64_t mqd_size, uint32_t flags, uint32_t *qid)
{
    union drm_amdgpu_userq u;
    int r;
    memset(&u, 0, sizeof(u));
    u.in.op = AMDGPU_USERQ_OP_CREATE;
    u.in.ip_type = ip;
    u.in.doorbell_handle = db;
    u.in.doorbell_offset = dboff;
    u.in.queue_va = qva;
    u.in.queue_size = qsize;
    u.in.rptr_va = rptr;
    u.in.wptr_va = wptr;
    u.in.mqd = (uint64_t)(uintptr_t)mqd;
    u.in.mqd_size = mqd_size;
    u.in.flags = flags;
    r = UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u);
    if (!r && qid)
        *qid = u.out.queue_id;
    return r;
}

static int uq_free(rdna3_drm_file *f, uint32_t qid)
{
    union drm_amdgpu_userq u;
    memset(&u, 0, sizeof(u));
    u.in.op = AMDGPU_USERQ_OP_FREE;
    u.in.queue_id = qid;
    return UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u);
}

static int uq_signal(rdna3_drm_file *f, uint32_t qid, const uint32_t *sobjs, uint16_t n)
{
    struct drm_amdgpu_userq_signal sg;
    memset(&sg, 0, sizeof(sg));
    sg.queue_id = qid;
    sg.syncobj_handles = (uint64_t)(uintptr_t)sobjs;
    sg.num_syncobj_handles = n;
    return UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL), sg);
}

static uint32_t uq_sobj(rdna3_drm_file *f)
{
    struct drm_syncobj_create c;
    memset(&c, 0, sizeof(c));
    return UQ_IOCTL(f, RD_NR_SYNCOBJ_CREATE, c) ? 0 : c.handle;
}

static int uq_sobj_wait(rdna3_drm_file *f, uint32_t h, int64_t timeout)
{
    struct drm_syncobj_wait w;
    memset(&w, 0, sizeof(w));
    w.handles = (uint64_t)(uintptr_t)&h;
    w.count_handles = 1;
    w.timeout_nsec = timeout;
    return UQ_IOCTL(f, RD_NR_SYNCOBJ_WAIT, w);
}

/* the client's side: packets at the ring's tail, the wptr, the doorbell */
static void uq_submit(rdna3_drm_file *f, uint32_t ring, uint32_t wp, const uint32_t *pk, uint32_t n,
                      uint64_t *wptr, uint32_t unit, uint32_t doorbell, int ring_it)
{
    rdna3_bo *r = rdna3_drm_bo_lookup(f, ring), *w = rdna3_drm_bo_lookup(f, wp);
    rdna3_vram_write(UQ.dev, r->vram_offset + *wptr * unit, pk, 4ull * n);
    *wptr += unit == 4 ? n : 4ull * n;
    rdna3_vram_write(UQ.dev, w->vram_offset, wptr, 8);
    rdna3_hdp_flush(UQ.dev);
    if (ring_it)
        rdna3_doorbell_wr64(UQ.dev, doorbell, *wptr);
}

static void test_userq_uapi(rdna3_dev *dev, rdna3_mes *mes, rdna3_gmc *gmc)
{
    static rdna3_drm_device dd;
    static rdna3_drm_file f1, f2;
    rdna3_drm_ops ops;
    amd_discovery disc;
    const uint64_t va = 0x0000500000000000ull;
    uint32_t hdb, hring, hwp, hrp, heop, hdst, hring2, hwp2, hrp2, hshadow, hcsa, hring3, hwp3, hrp3, hbig;
    uint32_t qc = 0, qg = 0, qs = 0, s1, s2, s3, pk[32], n, dbbase, v, adds0, removes0, ver0 = mes->sched_version;
    uint64_t wc = 0, wg = 0, ws = 0, got;
    struct drm_amdgpu_userq_mqd_compute_gfx11 mc;
    struct drm_amdgpu_userq_mqd_gfx11 mg;
    struct drm_amdgpu_userq_mqd_sdma_gfx11 ms;
    uint32_t i;

    memset(&UQ, 0, sizeof(UQ));
    UQ.dev = dev;
    UQ.mes = mes;
    UQ.gmc = gmc;
    UQ.heap = rdna3_vram_alloc(dev, 2u << 20, 65536, "uq-uapi-heap");
    UQ.heap_next = UQ.heap;
    UQ.heap_end = UQ.heap + (2u << 20);
    CHECK(UQ.heap != UINT64_MAX && gpuvm_pool_init(dev, &UQ.pool, 64) == RDNA3_OK);
    memset(&S.uq, 0, sizeof(S.uq));
    S.uq_bad = 0;
    mes->sched_version = 0x0000207a;
    memset(&ops, 0, sizeof(ops));
    ops.zalloc = uqo_zalloc; ops.free = uqo_free; ops.bo_alloc = uqo_bo_alloc; ops.bo_free = uqo_bo_free;
    ops.copyin = uqo_copyin; ops.copyout = uqo_copyout;
    ops.vm_create = uqo_vm_create; ops.vm_destroy = uqo_vm_destroy; ops.vm_map = uqo_vm_map; ops.vm_unmap = uqo_vm_unmap;
    ops.privileged = uqo_privileged;
    ops.bo_kmap = uqo_bo_kmap; ops.bo_gpu_addr = uqo_bo_gpu_addr; ops.userq_mqd_init = uqo_mqd_init;
    ops.userq_add_hw_queue = uqo_add; ops.userq_remove_hw_queue = uqo_remove; ops.userq_wait = uqo_wait;
    ops.now_ns = uqo_now;
    make_discovery(&disc);
    rdna3_drm_device_init(&dd, &ops, &disc, 0x747E, 0xC8, 0, 16ull << 30, 256ull << 20, 8ull << 30);
    CHECK(rdna3_drm_file_open(&dd, &f1) == 0);

    /* no user queues until the OS says so: no doorbell BOs, no queues */
    CHECK(uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_DOORBELL) == 0);
    {
        union drm_amdgpu_gem_create gc;
        memset(&gc, 0, sizeof(gc));
        gc.in.bo_size = 4096;
        gc.in.domains = AMDGPU_GEM_DOMAIN_DOORBELL;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == -RD_EOPNOTSUPP);
    }
    rdna3_drm_device_set_userq(&dd, (1u << AMDGPU_HW_IP_GFX) | (1u << AMDGPU_HW_IP_COMPUTE) | (1u << AMDGPU_HW_IP_DMA),
                               2u << 20);
    {   /* DEV_INFO and AMDGPU_INFO_UQ_FW_AREAS */
        struct drm_amdgpu_info in;
        struct drm_amdgpu_info_device di;
        struct drm_amdgpu_info_uq_metadata meta;
        memset(&in, 0, sizeof(in));
        in.return_pointer = (uint64_t)(uintptr_t)&di;
        in.return_size = sizeof(di);
        in.query = AMDGPU_INFO_DEV_INFO;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && di.userq_ip_mask == 7);
        memset(&meta, 0xff, sizeof(meta));
        in.return_pointer = (uint64_t)(uintptr_t)&meta;
        in.return_size = sizeof(meta);
        in.query = AMDGPU_INFO_UQ_FW_AREAS;
        in.query_hw_ip.type = AMDGPU_HW_IP_GFX;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0);
        CHECK(meta.gfx.shadow_size == 73728 && meta.gfx.shadow_alignment == 256 && meta.gfx.csa_size == 484 &&
              meta.gfx.csa_alignment == 256);
        in.query_hw_ip.type = AMDGPU_HW_IP_COMPUTE;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
    }

    /* the client's objects: a doorbell page, ring / wptr / rptr / EOP / dst */
    hdb = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_DOORBELL);
    CHECK(hdb && rdna3_drm_bo_lookup(&f1, hdb)->vram_offset == RD_DOORBELL_KERNEL_SIZE);
    dbbase = (uint32_t)(rdna3_drm_bo_lookup(&f1, hdb)->vram_offset / 4);
    {
        union drm_amdgpu_gem_mmap mm;
        memset(&mm, 0, sizeof(mm));
        mm.in.handle = hdb;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), mm) == 0 &&
              rdna3_drm_bo_by_mmap_offset(&f1, mm.out.addr_ptr) == rdna3_drm_bo_lookup(&f1, hdb));
        CHECK(uq_va(&f1, hdb, va + 0x900000, 4096) == 0);   /* recorded only */
    }
    hring = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hwp = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hrp = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    heop = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hdst = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hbig = uq_gem(&f1, 8192, AMDGPU_GEM_DOMAIN_VRAM);
    CHECK(hring && hwp && hrp && heop && hdst && hbig);
    CHECK(uq_va(&f1, hring, va, 4096) == 0 && uq_va(&f1, hwp, va + 0x1000, 4096) == 0 &&
          uq_va(&f1, hrp, va + 0x2000, 4096) == 0 && uq_va(&f1, heop, va + 0x3000, 4096) == 0 &&
          uq_va(&f1, hdst, va + 0x4000, 4096) == 0 && uq_va(&f1, hbig, va + 0x10000, 8192) == 0);
    memset(&mc, 0, sizeof(mc));
    mc.eop_va = va + 0x3000;

    /* what amdgpu_userq_ioctl refuses */
    adds0 = UQ.adds;
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    1u << 3, NULL) == -RD_EINVAL);                                  /* unknown flag */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_VCN_ENC, hdb, 0, va, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    0, NULL) == -RD_EINVAL);
    CHECK(uq_create(&f1, AMDGPU_HW_IP_DMA, hdb, 0, va, 4096, va + 0x2000, va + 0x1000, &ms, sizeof(ms),
                    AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE, NULL) == -RD_EINVAL);   /* no TMZ for SDMA */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_HIGH, NULL) == -RD_EACCES);
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, 0, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    0, NULL) == -RD_EINVAL);
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va + 0x800000, 4096, va + 0x2000, va + 0x1000, &mc,
                    sizeof(mc), 0, NULL) == -RD_EINVAL);                            /* queue not mapped */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va, 8192, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    0, NULL) == -RD_EINVAL);                                        /* past its mapping */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hring, 0, va, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    0, NULL) == -RD_EINVAL);                                        /* not a doorbell BO */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 512, va, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    0, NULL) == -RD_EINVAL);                                        /* outside the doorbell BO */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va, 4096, va + 0x2000, va + 0x1000, &mc, 4,
                    0, NULL) == -RD_EINVAL);                                        /* MQD size */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va, 4096, va + 0x2000, va + 0x10000, &mc, sizeof(mc),
                    0, NULL) == -RD_EINVAL);                                        /* wptr BO > 1 page */
    CHECK(UQ.adds == adds0 && rdna3_drm_bo_lookup(&f1, hdb)->refcnt == 2 && rdna3_drm_bo_lookup(&f1, hbig)->refcnt == 2);
    for (i = 0; i < RD_MAX_FENCE_DRV; i++)
        CHECK(!dd.fence_drv || !dd.fence_drv[i].refcnt);

    /* a compute queue: MQD, ADD_QUEUE in the client's process, seq64 in its VM */
    CHECK(uq_create(&f1, AMDGPU_HW_IP_COMPUTE, hdb, 0, va, 4096, va + 0x2000, va + 0x1000, &mc, sizeof(mc),
                    AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_NORMAL_HIGH, &qc) == 0 && qc == 1);
    CHECK(UQ.adds == adds0 + 1 && S.uq[0].used && S.uq[0].type == 1 && S.uq[0].doorbell == dbbase);
    CHECK(S.uq[0].process_id == 100 && S.uq[0].vmid == 8 && S.uq[0].queue_size_dw == 1024);
    CHECK(S.uq[0].wptr_addr == rdna3_vram_mc(dev, rdna3_drm_bo_lookup(&f1, hwp)->vram_offset));
    CHECK(dd.seq64_bo && S.uq[0].fence_addr >= dd.seq64_gpu_addr && S.uq[0].fence_addr < dd.seq64_gpu_addr + RD_SEQ64_SIZE);
    CHECK(f1.seq64_mapped && rdna3_drm_bo_lookup(&f1, hdb)->refcnt == 3 && rdna3_drm_bo_lookup(&f1, hwp)->refcnt == 3);

    /* a signal before the work ran: the fence waits for the queue's rptr */
    s1 = uq_sobj(&f1);
    n = uq_pm4(pk, va + 0x4000, 0xFEED0001, va + 0x4004, 0x11);
    uq_submit(&f1, hring, hwp, pk, n, &wc, 4, dbbase, 0);
    CHECK(uq_signal(&f1, qc, &s1, 1) == 0);
    {
        struct rd_xnu_syncobj_fence x;
        memset(&x, 0, sizeof(x));
        x.handle = s1;
        CHECK(UQ_IOCTL(&f1, RD_NR_XNU_SYNCOBJ_FENCE, x) == 0 && x.ip == RD_USERQ_FENCE_IP && x.seq == 13 &&
              !x.signaled);
    }
    CHECK(uq_sobj_wait(&f1, s1, 0) == -RD_ETIME);
    CHECK(uq_sobj_wait(&f1, s1, 1000000) == -RD_ETIME && UQ.waits == 1);
    /* the doorbell: the queue runs, the firmware writes the rptr to the
     * seq64 slot, the syncobj signals */
    rdna3_doorbell_wr64(dev, dbbase, wc);
    CHECK(S.uq_faults == 0);
    rdna3_vram_read(dev, rdna3_drm_bo_lookup(&f1, hdst)->vram_offset, &v, 4);
    CHECK(v == 0xFEED0001);
    CHECK(uq_sobj_wait(&f1, s1, 1000000) == 0);
    /* the slot reads the same through the client's VM (seq64 mapped there) */
    {
        uint8_t *p = vm_ptr(8, RD_SEQ64_VA + (S.uq[0].fence_addr - dd.seq64_gpu_addr), 8);
        CHECK(p && (memcpy(&got, p, 8), got == 13));
    }

    /* a gfx queue of the same client on the next doorbell */
    hring2 = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hwp2 = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hrp2 = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hshadow = uq_gem(&f1, 73728, AMDGPU_GEM_DOMAIN_VRAM);
    hcsa = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    CHECK(uq_va(&f1, hring2, va + 0x20000, 4096) == 0 && uq_va(&f1, hwp2, va + 0x21000, 4096) == 0 &&
          uq_va(&f1, hrp2, va + 0x22000, 4096) == 0 && uq_va(&f1, hshadow, va + 0x40000, 73728) == 0 &&
          uq_va(&f1, hcsa, va + 0x60000, 4096) == 0);
    memset(&mg, 0, sizeof(mg));
    mg.shadow_va = va + 0x40000;
    mg.csa_va = va + 0x60000;
    CHECK(uq_create(&f1, AMDGPU_HW_IP_GFX, hdb, 1, va + 0x20000, 4096, va + 0x22000, va + 0x21000, &mg, sizeof(mg),
                    0, &qg) == 0 && qg == 2);
    CHECK(S.uq[1].used && S.uq[1].type == 0 && S.uq[1].doorbell == dbbase + 2 && S.uq[1].vmid == 8);
    {
        const uint32_t *m = (const uint32_t *)mc_ptr(S.uq[1].mqd, 2048);
        CHECK(m && m[0] == (uint32_t)(va + 0x40000) && m[4] == (uint32_t)(va + 0x60000) &&
              q64(m, 510) == S.uq[1].fence_addr);
    }

    /* USERQ_WAIT: compute work the gfx queue has to wait for comes back as
     * (seq64 VA, value); the gfx queue holds the compute fence driver */
    s2 = uq_sobj(&f1);
    n = uq_pm4(pk, va + 0x4008, 0xFEED0002, va + 0x400c, 0x12);
    uq_submit(&f1, hring, hwp, pk, n, &wc, 4, dbbase, 0);
    CHECK(uq_signal(&f1, qc, &s2, 1) == 0 && wc == 26);
    {
        struct drm_amdgpu_userq_wait w;
        struct drm_amdgpu_userq_fence_info fi[4];
        rdna3_userq_fence_drv *fc = NULL;
        uint32_t hs[2] = { s2, s1 };
        memset(&w, 0, sizeof(w));
        w.waitq_id = qg;
        w.syncobj_handles = (uint64_t)(uintptr_t)hs;
        w.num_syncobj_handles = 2;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == 0 && w.num_fences == 2);
        w.num_fences = 1;
        w.out_fences = (uint64_t)(uintptr_t)fi;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == -RD_EINVAL);   /* too small */
        w.num_fences = 4;
        memset(fi, 0, sizeof(fi));
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == 0);
        /* s1 signaled: one pair, the compute queue's slot at 26 */
        CHECK(w.num_fences == 1 && fi[0].value == 26 && fi[0].va >= RD_SEQ64_VA &&
              fi[0].va - RD_SEQ64_VA == S.uq[0].fence_addr - dd.seq64_gpu_addr);
        for (i = 0; i < RD_MAX_FENCE_DRV; i++)
            if (dd.fence_drv[i].refcnt && dd.fence_drv[i].va == fi[0].va)
                fc = &dd.fence_drv[i];
        CHECK(fc && fc->refcnt == 2 && f1.userqs[qg - 1].nxa == 1);
        w.waitq_id = 9;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == -RD_EINVAL);
        /* the gfx queue signals: the reference moves onto its fence and
         * goes once that fence signals */
        s3 = uq_sobj(&f1);
        n = uq_pm4(pk, va + 0x4010, 0xFEED0003, va + 0x4014, 0x13);
        uq_submit(&f1, hring2, hwp2, pk, n, &wg, 4, dbbase + 2, 0);
        CHECK(uq_signal(&f1, qg, &s3, 1) == 0 && f1.userqs[qg - 1].nxa == 0 && fc->refcnt == 2);
        rdna3_doorbell_wr64(dev, dbbase, wc);        /* compute first */
        CHECK(uq_sobj_wait(&f1, s2, 1000000) == 0);
        rdna3_doorbell_wr64(dev, dbbase + 2, wg);    /* then gfx */
        rdna3_vram_read(dev, rdna3_drm_bo_lookup(&f1, hdst)->vram_offset + 0x10, &v, 4);
        CHECK(v == 0xFEED0003 && S.uq_faults == 0);
        CHECK(uq_sobj_wait(&f1, s3, 1000000) == 0 && fc->refcnt == 1);
    }

    /* an SDMA queue in a second client: its own process and VMID */
    CHECK(rdna3_drm_file_open(&dd, &f2) == 0);
    {
        uint32_t hdb2 = uq_gem(&f2, 4096, AMDGPU_GEM_DOMAIN_DOORBELL);
        uint32_t hd2 = uq_gem(&f2, 4096, AMDGPU_GEM_DOMAIN_VRAM), s4;
        hring3 = uq_gem(&f2, 4096, AMDGPU_GEM_DOMAIN_VRAM);
        hwp3 = uq_gem(&f2, 4096, AMDGPU_GEM_DOMAIN_VRAM);
        hrp3 = uq_gem(&f2, 4096, AMDGPU_GEM_DOMAIN_VRAM);
        CHECK(hdb2 && rdna3_drm_bo_lookup(&f2, hdb2)->vram_offset == RD_DOORBELL_KERNEL_SIZE + 4096);
        CHECK(uq_va(&f2, hring3, va, 4096) == 0 && uq_va(&f2, hwp3, va + 0x1000, 4096) == 0 &&
              uq_va(&f2, hrp3, va + 0x2000, 4096) == 0 && uq_va(&f2, hd2, va + 0x4000, 4096) == 0);
        memset(&ms, 0, sizeof(ms));
        ms.csa_va = va + 0x8000;
        CHECK(uq_create(&f2, AMDGPU_HW_IP_DMA, hdb2, 3, va, 4096, va + 0x2000, va + 0x1000, &ms, sizeof(ms), 0,
                        &qs) == 0 && qs == 1);
        CHECK(S.uq[2].used && S.uq[2].type == 2 && S.uq[2].vmid == 9 && S.uq[2].process_id == 101 &&
              S.uq[2].doorbell == dbbase + 1024 + 6);
        s4 = uq_sobj(&f2);
        n = 0;
        pk[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_WRITE);
        pk[n++] = (uint32_t)(va + 0x4000);
        pk[n++] = (uint32_t)((va + 0x4000) >> 32);
        pk[n++] = 0;
        pk[n++] = 0xFEED0004;
        pk[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
        pk[n++] = (uint32_t)(va + 0x4004);
        pk[n++] = (uint32_t)((va + 0x4004) >> 32);
        pk[n++] = 0x14;
        uq_submit(&f2, hring3, hwp3, pk, n, &ws, 1, dbbase + 1024 + 6, 0);
        CHECK(uq_signal(&f2, qs, &s4, 1) == 0 && uq_sobj_wait(&f2, s4, 1000000) == -RD_ETIME);
        rdna3_doorbell_wr64(dev, dbbase + 1024 + 6, ws);
        rdna3_vram_read(dev, rdna3_drm_bo_lookup(&f2, hd2)->vram_offset, &v, 4);
        CHECK(v == 0xFEED0004 && S.uq_faults == 0 && uq_sobj_wait(&f2, s4, 1000000) == 0);
        /* the other client's queue ids are not this one's */
        CHECK(uq_signal(&f2, qg, &s4, 1) == -RD_ENOENT);
    }

    /* USERQ_FREE: REMOVE_QUEUE, its doorbell reaches nothing any more;
     * a signal on it fails, the fences it made count as signaled */
    removes0 = UQ.removes;
    {
        union drm_amdgpu_userq u;
        memset(&u, 0, sizeof(u));
        u.in.op = AMDGPU_USERQ_OP_FREE;
        u.in.queue_id = qc;
        u.in.ip_type = 1;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u) == -RD_EINVAL);   /* FREE takes only the id */
    }
    n = uq_pm4(pk, va + 0x4020, 0xFEED0005, va + 0x4024, 0x15);
    uq_submit(&f1, hring, hwp, pk, n, &wc, 4, dbbase, 0);
    CHECK(uq_signal(&f1, qc, &s1, 1) == 0 && uq_sobj_wait(&f1, s1, 0) == -RD_ETIME);
    CHECK(uq_free(&f1, qc) == 0 && UQ.removes == removes0 + 1 && !S.uq[0].used);
    CHECK(uq_free(&f1, qc) == -RD_EINVAL && uq_signal(&f1, qc, &s1, 1) == -RD_ENOENT);
    CHECK(uq_sobj_wait(&f1, s1, 0) == 0);   /* its driver is gone: cancelled */
    {
        uint32_t un0 = S.db_unclaimed;
        rdna3_doorbell_wr64(dev, dbbase, wc);
        CHECK(S.db_unclaimed == un0 + 1);
    }
    CHECK(rdna3_drm_bo_lookup(&f1, hdb)->refcnt == 3 && rdna3_drm_bo_lookup(&f1, hwp)->refcnt == 2);

    /* a closing client takes its queues with it */
    rdna3_drm_file_close(&f1);
    CHECK(UQ.removes == removes0 + 2 && !S.uq[1].used && S.uq[2].used);
    rdna3_drm_file_close(&f2);
    CHECK(UQ.removes == removes0 + 3 && !S.uq[2].used);
    for (i = 0; i < RD_MAX_FENCE_DRV; i++)
        CHECK(!dd.fence_drv[i].refcnt);
    for (i = 0; i < RD_SEQ64_SLOTS / 8; i++)
        CHECK(!dd.seq64_used[i]);
    for (i = 0; i < RD_MAX_DOORBELL_PAGES / 8; i++)
        CHECK(!dd.doorbell_used[i]);
    CHECK(!UQ.vms[0].used && !UQ.vms[1].used);
    rdna3_drm_device_fini(&dd);
    CHECK(!dd.seq64_bo); CHECK(S.uq_bad == 0); CHECK(S.cp_bad == 0);
    mes->sched_version = ver0;
}

/* ---------------------------- test ---------------------------- */

/* output gamma (hw/dcn32_gamma.c) on the two heads the clone test leaves:
 * dc->head[1] is the boot head (pipe 1, OTG 2 -> OPP 2 -> MPCC 1),
 * dc->head[0] the clone (pipe 0, OTG 1 -> its OPP -> MPCC 0). Expected
 * values by hand: custom float 6e12m = exponent (bias 31) << 12 | mantissa,
 * e.g. 2^-10 -> 21 << 12, 0.5 -> 30 << 12, 0.75 -> 30 << 12 | 0x800, 1 -> 31 << 12;
 * 2 as 6e10m -> 32 << 10. Point 136 is x = 0.5, 144 x = 0.75, 152 x = 1. */
static void test_gamma(rdna3_dev *dev, const rdna3_dcn *dc)
{
    static uint16_t ident[256];
    static rdna3_gamma_pwl pw, pc, pd;
    /* R = identity, G = x / 3, B = 1 - x (per channel, two entries each) */
    static const uint16_t chan[6] = { 0, 65535, 0, 21845, 65535, 0 };
    static const uint16_t dark[2] = { 0, 16384 };
    const uint32_t rb = regMPCC_OGAM0_MPCC_OGAM_RAMB_START_CNTL_B - regMPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_B;
    rdna3_gamma_state st;
    uint32_t mpcc[2], i, crc_a, crc_b;
#define OG(r_) S.reg[OGAM_R(r_, 1)]
#define OGAM_CRC() (dev->os.msleep(dev->os.ctx, 32), S.reg[OTG(regOTG0_OTG_CRC0_DATA_RG, 2)])

    memset(&ogam, 0, sizeof(ogam));
    for (i = 0; i < RD_DCN_MAX_PIPES; i++)     /* the GOP leaves the memory in light sleep */
        S.reg[MPCC_R(regMPCC0_MPCC_MEM_PWR_CTRL, i)] = 1u << MPCC0_MPCC_MEM_PWR_CTRL__MPCC_OGAM_MEM_PWR_STATE__SHIFT;
    for (i = 0; i < 256; i++)
        ident[i] = (uint16_t)(i * 257);

    /* the curve */
    CHECK(dcn32_gamma_build(ident, 2, 256, &pw) == RDNA3_EINVAL);
    CHECK(dcn32_gamma_build(ident, 1, 1, &pw) == RDNA3_EINVAL);
    CHECK(dcn32_gamma_build(ident, 1, RD_GAMMA_MAX_ENTRIES + 1, &pw) == RDNA3_EINVAL);
    CHECK(dcn32_gamma_build(ident, 1, 256, &pw) == RDNA3_OK && pw.num == RD_GAMMA_HW_POINTS &&
          pw.linear && pw.rgb_equal);
    CHECK(pw.start_x[0] == 0x15000 && pw.start_x[2] == 0x15000 && pw.end_x[1] == 0x8000 && pw.end_slope[0] == 0);
    CHECK(pw.start_slope[0] == 0x1F000 && pw.end_base[2] == 0x1F000);          /* y = x: slope 1, y(2) = 1 */
    CHECK(pw.lut[0][0] == 0x15000 && pw.lut[1][136] == 0x1E000 && pw.lut[2][144] == 0x1E800 &&
          pw.lut[0][152] == 0x1F000 && pw.lut[0][153] == 0x1F000 && pw.lut[0][154] == 0x1F000);
    /* arr_curve_points: offsets 0, 8, 24, ... 152, 154; segments 3, 4 x 9, 1 */
    CHECK(pw.region[0] == 0x40083000u && pw.region[1] == 0x40284018u && pw.region[4] == 0x40884078u &&
          pw.region[5] == 0x009A1098u && pw.region[6] == 0 && pw.region[16] == 0);
    /* macOS's identity as the framebuffer scales it: 12-bit entries to 16
     * bits (card run 30) - still the identity, the bypass */
    {
        static uint16_t id12[256];
        for (i = 0; i < 256; i++)
            id12[i] = (uint16_t)(((i * 4095u + 127) / 255 * 65535u + 2047) / 4095);
        CHECK(dcn32_gamma_build(id12, 1, 256, &pc) == RDNA3_OK && pc.linear);
        id12[128] = (uint16_t)(id12[128] + 40);   /* a real curve is not */
        CHECK(dcn32_gamma_build(id12, 1, 256, &pc) == RDNA3_OK && !pc.linear);
    }
    CHECK(dcn32_gamma_build(chan, 3, 2, &pc) == RDNA3_OK && !pc.linear && !pc.rgb_equal);
    /* slopes 1, 1/3 (1/3 = 2^-2 * 1.333: 29 << 12 | 0x555), 1023 (= 2^9 * 1.998: 40 << 12 | 0xff8) */
    CHECK(pc.start_slope[0] == 0x1F000 && pc.start_slope[1] == 0x1D555 && pc.start_slope[2] == 0x28FF8);
    CHECK(pc.end_base[0] == 0x1F000 && pc.end_base[1] == 0x1D555 && pc.end_base[2] == 0);
    CHECK(pc.lut[1][0] == 0x13555 && pc.lut[2][0] == 0x1EFF8);   /* 2^-10 / 3, 1 - 2^-10 */
    CHECK(dcn32_gamma_build(dark, 1, 2, &pd) == RDNA3_OK && pd.rgb_equal && !pd.linear);

    /* the boot head's MPCC; OTG 4 does not exist */
    CHECK(dcn32_gamma_otg_mpccs(dev, dc->head[1].otg, mpcc) == 1 && mpcc[0] == 1);
    CHECK(dcn32_gamma_otg_mpccs(dev, 4, mpcc) == RDNA3_EINVAL);
    CHECK(dcn32_gamma_set_head(dev, dc, 2, &pw) == RDNA3_EINVAL);

    /* bypass -> RAM A: memory forced on, one pass for all colours, the
     * new mode latched at the OTG unlock */
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.current == RD_GAMMA_LUT_BYPASS && st.mem_pwr_state == 1);
    CHECK(dcn32_gamma_set_head(dev, dc, 1, &pw) == RDNA3_OK);
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.mode == 2 && st.select == 0 &&
          st.current == RD_GAMMA_LUT_RAM_A && st.mem_pwr_state == 0);
    {   /* the DPP's degamma, read for the report (GOP: none) */
        uint32_t *gc = &S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL + RD_CLONE_DPP_STRIDE], gc0 = *gc;
        CHECK(st.dpp_degamma == ((gc0 & CM0_CM_GAMCOR_CONTROL__CM_GAMCOR_MODE_CURRENT_MASK) >>
                                 CM0_CM_GAMCOR_CONTROL__CM_GAMCOR_MODE_CURRENT__SHIFT));
        *gc = 2u << CM0_CM_GAMCOR_CONTROL__CM_GAMCOR_MODE_CURRENT__SHIFT;
        CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.dpp_degamma == 2);
        *gc = gc0;
    }
    CHECK(ogam.writes == RD_GAMMA_HW_POINTS && !ogam.dropped && ogam.latches == 1 &&
          !(S.reg[OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, 2)] & 1));
    CHECK(OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_B) == 0x15000 && OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_R) == 0x15000 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_G) == 0x1F000 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL1_R) == 0x1F000 && OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL2_B) == 0x8000 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1) == 0x40083000u &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1 + 5) == 0x009A1098u &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_REGION_32_33) == 0 && OG(regMPCC_OGAM0_MPCC_OGAM_RAMB_START_CNTL_B) == 0);
    CHECK(ogam.ram[1][0][0][0] == 0x15000 && ogam.ram[1][0][1][136] == 0x1E000 && ogam.ram[1][0][2][144] == 0x1E800 &&
          ogam.ram[1][0][2][154] == 0x1F000 && ogam.ram[1][0][0][155] == 0);
    CHECK(ogam.ram[0][0][0][0] == 0 && ogam.ram[1][1][0][0] == 0);   /* the clone's MPCC, RAM B untouched */

    /* a table per channel -> RAM B, red / green / blue in three passes */
    CHECK(dcn32_gamma_set_head(dev, dc, 1, &pc) == RDNA3_OK);
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.select == 1 && st.current == RD_GAMMA_LUT_RAM_B);
    CHECK(ogam.writes == 4 * RD_GAMMA_HW_POINTS && !ogam.dropped);
    CHECK(ogam.ram[1][1][0][136] == 0x1E000 && ogam.ram[1][1][1][136] == 0x1C555 && ogam.ram[1][1][2][136] == 0x1E000);
    CHECK(ogam.ram[1][1][1][0] == 0x13555 && ogam.ram[1][1][2][0] == 0x1EFF8 && ogam.ram[1][1][2][154] == 0 &&
          ogam.ram[1][1][0][154] == 0x1F000);
    CHECK(OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_B + rb) == 0x28FF8 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_G + rb) == 0x1D555 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_R + rb) == 0x1F000 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL1_B + rb) == 0 && OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL1_G + rb) == 0x1D555 &&
          OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1 + rb) == 0x40083000u);
    CHECK(OG(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_B) == 0x1F000 && ogam.ram[1][0][2][136] == 0x1E000);   /* RAM A kept */
    /* ping-pong back to RAM A */
    CHECK(dcn32_gamma_set_head(dev, dc, 1, &pw) == RDNA3_OK);
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.select == 0 && st.current == RD_GAMMA_LUT_RAM_A);

    /* what the card test checks: the OTG CRC moves with a dark table and
     * comes back with the bypass */
    dcn32_otg_crc_config(dev, dc->head[1].otg, 1);
    S.dcn_frames = 1;
    CHECK(dcn32_gamma_set_head(dev, dc, 1, NULL) == RDNA3_OK);
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.mode == 0 && st.current == RD_GAMMA_LUT_BYPASS);
    crc_a = OGAM_CRC();
    CHECK(dcn32_gamma_set_head(dev, dc, 1, &pd) == RDNA3_OK);
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.current == RD_GAMMA_LUT_RAM_A);   /* from bypass: A */
    crc_b = OGAM_CRC();
    CHECK(crc_b != crc_a);
    CHECK(dcn32_gamma_set_head(dev, dc, 1, NULL) == RDNA3_OK && OGAM_CRC() == crc_a);
    S.dcn_frames = 0;
    dcn32_otg_crc_config(dev, dc->head[1].otg, 0);

    /* the clone head has an MPCC of its own */
    CHECK(dcn32_gamma_otg_mpccs(dev, dc->head[0].otg, mpcc) == 1 && mpcc[0] == 0);
    CHECK(dcn32_gamma_set_head(dev, dc, 0, &pc) == RDNA3_OK);
    CHECK(dcn32_gamma_state(dev, 0, &st) == RDNA3_OK && st.current == RD_GAMMA_LUT_RAM_A &&
          ogam.ram[0][0][1][136] == 0x1C555);
    CHECK(dcn32_gamma_set_head(dev, dc, 0, NULL) == RDNA3_OK);

    /* the memory does not power up: refused, the mode stays as it was */
    ogam.pwr_stuck = 1;
    S.reg[MPCC_R(regMPCC0_MPCC_MEM_PWR_CTRL, 1)] = 1u << MPCC0_MPCC_MEM_PWR_CTRL__MPCC_OGAM_MEM_PWR_STATE__SHIFT;
    CHECK(dcn32_gamma_set_head(dev, dc, 1, &pc) == RDNA3_ETIME);
    CHECK(dcn32_gamma_state(dev, 1, &st) == RDNA3_OK && st.mode == 0 && st.current == RD_GAMMA_LUT_BYPASS &&
          !(S.reg[OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, 2)] & 1));
    ogam.pwr_stuck = 0;
#undef OG
#undef OGAM_CRC
}

/* DP SST on the boot head: the HDMI head of the DCN tests turned into a
 * DP one (DIG_MODE 0, the synthetic sink of make_sink cabled to UNIPHY B,
 * the GOP's link at HBR2 x4). Sink capabilities, link settings,
 * dp_verify_link_cap with a sink that fails clock recovery at HBR3, the mode
 * set through the DP path (DP DTO, training, MSA, M/N, unblank), a mode
 * whose training fails (the old one comes back, trained again), and
 * perform_link_training_with_retries with and without fallback. */
static void test_dp_link(rdna3_dev *dev, rdna3_dmub *dm)
{
    static rdna3_dcn dc;
    static rdna3_dcn_modeset ms;
    static rdna3_dp_link dp, dp2;
    static uint8_t dpcd_save[0x800];
    rdna3_display_path own;
    rdna3_dp_link_settings st, mx, cur;
    rdna3_mode m, up, big;
    const uint32_t be_cntl = S.reg[DIGN(regDIG0_DIG_BE_CNTL, 1)];
    uint32_t otg, dto[3], m_vid, runs, i;

    memcpy(dpcd_save, S.dpcd, sizeof(S.dpcd));
    S.reg[DIGN(regDIG0_DIG_BE_CNTL, 1)] = be_cntl & ~DIG0_DIG_BE_CNTL__DIG_MODE_MASK;
    memset(&S.dps, 0, sizeof(S.dps));
    S.dps.be = 1;
    S.dps.need_vs = 2;                      /* two swing requests */
    S.dps.need_pe = 1;                      /* one pre-emphasis request */
    S.dps.fail_rate = RD_DP_LINK_RATE_HIGH3;
    /* the GOP's link: HBR2 x4, trained, video on */
    S.dps.phy_on = 1;
    S.dps.lanes = 4;
    S.dps.symclk_10khz = RD_DP_LINK_RATE_HIGH2 * 2700;
    S.dps.cr = S.dps.eq = 1;
    S.dpcd[0x100] = RD_DP_LINK_RATE_HIGH2;
    S.dpcd[0x101] = 0x84;
    S.dpcd[0x002] = 0xc4;                   /* 4 lanes, TPS3, enhanced framing */
    S.dpcd[0x003] = 0x81;                   /* TPS4, 0.5 % downspread */
    S.dpcd[0x00e] = 0x00;                   /* 100 us CR / 400 us EQ */
    S.dpcd[0x200] = 0x01;                   /* one sink */
    S.dpcd[0x400] = 0x00; S.dpcd[0x401] = 0x12; S.dpcd[0x402] = 0x34;   /* synthetic OUI */
    S.reg[DPR(regDP0_DP_VID_STREAM_CNTL, 2)] = DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK |
                                               DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK;
    S.reg[DIGN(regDIG0_DIG_FIFO_CTRL0, 2)] = DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE_MASK;
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].dig_mode == 0 && dc.head[0].dig_be == 1 &&
          dc.head[0].dig_fe == 2 && dc.head[0].h_active == 2560);
    otg = dc.head[0].otg;
    dto[0] = S.reg[DCN_B1 + regDP_DTO0_PHASE + 4 * otg];
    dto[1] = S.reg[DCN_B1 + regDP_DTO0_MODULO + 4 * otg];
    dto[2] = S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * otg];
    memset(&own, 0, sizeof(own));
    own.transmitter = 1; own.hpd = 0; own.ddc_line = 0;
    own.encoder_objid = 0x211e; own.connector_objid = 0x3113;   /* DP connector */
    CHECK(edid_dmt(1920, 1080, 60, &m) == 0);
    /* without a link the DP head stays refused */
    CHECK(dcn32_modeset_plan_dp(dev, &dc, 0, &own, NULL, 241500, &m, NULL, &ms) == RD_MODESET_NOT_TMDS);

    /* dc_bandwidth_in_kbps_from_timing, dp_link_bandwidth_kbps */
    CHECK(dcn32_dp_stream_kbps(148500, 8) == 3564000);
    st.lane_count = 4; st.link_rate = RD_DP_LINK_RATE_HIGH2; st.link_spread = 0;
    CHECK(dcn32_dp_link_kbps_settings(&st) == 17280000);
    st.lane_count = 1; st.link_rate = RD_DP_LINK_RATE_LOW;
    CHECK(dcn32_dp_link_kbps_settings(&st) == 1296000);

    /* retrieve_link_cap + dp_get_max_link_cap */
    dcn32_dp_link_init(&dp, dev, dm, 0, 1, 2, otg, 0, 0x13, 0x1e, 0, 0);
    CHECK(dcn32_modeset_plan_dp(dev, &dc, 0, &own, &dp, 241500, &m, NULL, &ms) == RD_MODESET_DP_LINK);
    CHECK(dcn32_dp_read_caps(&dp) == RDNA3_OK && dp.caps_valid && dp.dpcd_caps.dpcd_rev == 0x14 &&
          dp.dpcd_caps.sink_count == 1 && dp.dpcd_caps.sink_dev_id == 0x1234);
    CHECK(dp.reported_link_cap.link_rate == RD_DP_LINK_RATE_HIGH3 && dp.reported_link_cap.lane_count == 4 &&
          dp.reported_link_cap.link_spread == RD_DP_LINK_SPREAD_05_DOWNSPREAD_30KHZ);
    CHECK(dp.max_link_cap.link_rate == RD_DP_LINK_RATE_HIGH3 && dp.max_link_cap.lane_count == 4);
    /* the VBIOS encoder caps: HBR2 without the HBR3 bit */
    dcn32_dp_link_init(&dp2, dev, dm, 0, 1, 2, otg, 0, 0x13, 0x1e, RD_ATOM_ENC_CAP_HBR2, 0);
    CHECK(dcn32_dp_read_caps(&dp2) == RDNA3_OK && dp2.max_link_cap.link_rate == RD_DP_LINK_RATE_HIGH2);
    /* an LTTPR (1 repeater, HBR x2): transparent mode, the count echoed, its limits */
    S.dpcd_lttpr[0] = 0x14; S.dpcd_lttpr[1] = RD_DP_LINK_RATE_HIGH; S.dpcd_lttpr[2] = 0x80; S.dpcd_lttpr[4] = 2;
    dcn32_dp_link_init(&dp2, dev, dm, 0, 1, 2, otg, 0, 0x13, 0x1e, 0, 0);
    CHECK(dcn32_dp_read_caps(&dp2) == RDNA3_OK && dp2.max_link_cap.link_rate == RD_DP_LINK_RATE_HIGH &&
          dp2.max_link_cap.lane_count == 2 && S.dpcd_lttpr[3] == RD_DP_PHY_REPEATER_MODE_TRANSPARENT &&
          S.dpcd_lttpr[0xa] == 0x80);
    memset(S.dpcd_lttpr, 0, sizeof(S.dpcd_lttpr));
    /* the sink in D3: wake_up_aux_channel retries, then the caps */
    S.dpcd[0x600] = RD_DP_SET_POWER_D3;
    S.dpcd_lttpr[0] = 0;
    dcn32_dp_link_init(&dp2, dev, dm, 0, 1, 2, otg, 0, 0x13, 0x1e, 0, 0);
    dp2.lttpr_aware = 0;
    CHECK(dcn32_dp_read_caps(&dp2) == RDNA3_OK);
    S.dpcd[0x600] = RD_DP_SET_POWER_D0;

    /* link_decide_link_settings: the smallest setting that carries the mode */
    CHECK(dcn32_dp_decide_link_settings(&dp, 3564000, &st) == RDNA3_OK && st.link_rate == RD_DP_LINK_RATE_LOW &&
          st.lane_count == 4);
    CHECK(dcn32_dp_decide_link_settings(&dp, 5796000, &st) == RDNA3_OK && st.link_rate == RD_DP_LINK_RATE_HIGH &&
          st.lane_count == 4);
    CHECK(dcn32_dp_decide_link_settings(&dp, 1000000, &st) == RDNA3_OK && st.link_rate == RD_DP_LINK_RATE_LOW &&
          st.lane_count == 1);
    CHECK(dcn32_dp_decide_link_settings(&dp, 30000000, &st) == RDNA3_EINVAL);
    /* decide_fallback_link_setting */
    mx.lane_count = 4; mx.link_rate = RD_DP_LINK_RATE_HIGH3; mx.link_spread = 0;
    cur = mx;
    CHECK(dcn32_dp_fallback_link_setting(&dp, &mx, &cur, RD_LT_CR_FAIL_LANE0) == 1 &&
          cur.link_rate == RD_DP_LINK_RATE_HIGH2 && cur.lane_count == 4);
    CHECK(dcn32_dp_fallback_link_setting(&dp, &mx, &cur, RD_LT_EQ_FAIL_EQ) == 1 &&
          cur.link_rate == RD_DP_LINK_RATE_HIGH2 && cur.lane_count == 2);
    cur.lane_count = 4; cur.link_rate = RD_DP_LINK_RATE_LOW;
    CHECK(dcn32_dp_fallback_link_setting(&dp, &mx, &cur, RD_LT_CR_FAIL_LANE1) == 1 && cur.lane_count == 1 &&
          cur.link_rate == RD_DP_LINK_RATE_HIGH3);
    cur.lane_count = 1; cur.link_rate = RD_DP_LINK_RATE_LOW;
    CHECK(dcn32_dp_fallback_link_setting(&dp, &mx, &cur, RD_LT_CR_FAIL_LANE0) == 0);
    cur.lane_count = 2; cur.link_rate = RD_DP_LINK_RATE_HIGH;
    CHECK(dcn32_dp_fallback_link_setting(&dp, &mx, &cur, RD_LT_EQ_FAIL_CR) == 1 &&
          cur.link_rate == RD_DP_LINK_RATE_LOW && mx.link_rate == RD_DP_LINK_RATE_LOW && cur.lane_count == 4);

    /* dp_verify_link_cap_with_retries: HBR3 x4 does not lock (the sink
     * asks for more swing until the max), the fallback HBR2 x4 trains
     * after two swing requests and one pre-emphasis request, with TPS4.
     * The failure at HBR3 counts (fail_count), so both verify attempts run
     * and the result is "not clean", with HBR2 x4 as the verified cap,
     * as in DC */
    S.dmub.nseq = 0;
    CHECK(dcn32_dp_verify_link_cap(&dp, RD_DP_LINK_TRAINING_MAX_VERIFY_RETRY) == 0 && dp.verified &&
          dp.verified_link_cap.link_rate == RD_DP_LINK_RATE_HIGH2 && dp.verified_link_cap.lane_count == 4);
    CHECK(dp.lt_fallbacks == 2 && dp.lt_runs == 4 && S.dps.trained_rate == RD_DP_LINK_RATE_HIGH2 &&
          S.dps.trained_lanes == 4);
    /* per attempt: HBR3 VS 0->1->2->3; HBR2 VS 0->1->2, PE 0->1 */
    CHECK(S.dps.vs == 2 && S.dps.pe == 1 && S.dps.lane_mismatch == 0 && S.dps.eq_pattern == RD_DPCD_TRAINING_PATTERN_4 &&
          S.dps.tps1_sets == 4 && S.dps.adjust_reqs == 2 * (3 + 3));
    CHECK(S.dpcd[0x107] == 0x10 && S.dpcd[0x108] == 1 && S.dpcd[0x102] == 0 && S.dpcd[0x101] == 0x84);
    /* dp_disable_link_phy: the sink in D3, the transmitter off */
    CHECK(!S.dps.phy_on && S.dpcd[0x600] == RD_DP_SET_POWER_D3 &&
          !(S.reg[DIGN(regDIG0_DIG_BE_EN_CNTL, 1)] & DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK));
    CHECK(S.dmub.seq[0] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[0] == RD_ATOM_TX_ENABLE &&
          S.dmub.seq_tx_action[1] == RD_ATOM_TX_SETUP_VSEMPH);

    /* the mode set: 2560x1440 -> 1920x1080 on the DP head */
    big = m;
    big.pixel_khz = 800000;                 /* 19.2 Gbps: beyond the verified HBR2 x4 */
    CHECK(dcn32_modeset_plan_dp(dev, &dc, 0, &own, &dp, 241500, &big, NULL, &ms) == RD_MODESET_LINK_BW);
    CHECK(dcn32_modeset_plan_dp(dev, &dc, 0, &own, &dp, 241500, &m, NULL, &ms) == RD_MODESET_OK && ms.dp == &dp &&
          ms.dp_cur.h_active == 2560 && ms.dp_cur.pixel_khz == 241500 && ms.dp_want.h_total == 2200);
    if (!dp.dprefclk_khz)
        dp.dprefclk_khz = 600000;
    S.dcn_frames = 1;
    S.dmub.nseq = 0;
    runs = dp.lt_runs;
    CHECK(dcn32_modeset_apply(dev, dm, &dc, NULL, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) == RDNA3_OK);
    /* dcn31_program_pix_clk: the DP DTO at 148.5 MHz from DPREFCLK, source 1 (8b/10b) */
    CHECK(S.reg[DCN_B1 + regDP_DTO0_PHASE + 4 * otg] == 148500000u &&
          S.reg[DCN_B1 + regDP_DTO0_MODULO + 4 * otg] == dp.dprefclk_khz * 1000u &&
          (S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * otg] & OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK) &&
          ((S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * otg] & OTG0_PIXEL_RATE_CNTL__PIPE0_DTO_SRC_SEL_MASK) >>
           OTG0_PIXEL_RATE_CNTL__PIPE0_DTO_SRC_SEL__SHIFT) == 1);
    /* no VBIOS pixel clock or encoder command on DP: transmitter only */
    for (i = 0; i < S.dmub.nseq && i < 16; i++)
        CHECK(S.dmub.seq[i] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL);
    /* trained once at the smallest setting the verified cap gives: RBR x4 */
    CHECK(dp.lt_runs == runs + 1 && S.dpcd[0x100] == RD_DP_LINK_RATE_LOW && S.dpcd[0x101] == 0x84 && S.dps.cr &&
          S.dps.eq && S.dps.trained_rate == RD_DP_LINK_RATE_LOW && S.dpcd[0x600] == RD_DP_SET_POWER_D0 &&
          S.dps.symclk_10khz == RD_DP_LINK_RATE_LOW * 2700 && S.dps.lanes == 4);
    CHECK((S.reg[DPR(regDP0_DP_CONFIG, 1)] & DP0_DP_CONFIG__DP_UDI_LANES_MASK) == 3 &&
          (S.reg[DPR(regDP0_DP_LINK_CNTL, 1)] & DP0_DP_LINK_CNTL__DP_LINK_TRAINING_COMPLETE_MASK));
    /* the MSA of 1080p (CEA/DMT: htotal 2200, sync 44, back porch 148) */
    CHECK(S.reg[DPR(regDP0_DP_MSA_TIMING_PARAM1, 2)] == (2200u << 16 | 1125u) &&
          S.reg[DPR(regDP0_DP_MSA_TIMING_PARAM2, 2)] == ((44u + 148u) << 16 | 41u) &&
          S.reg[DPR(regDP0_DP_MSA_TIMING_PARAM3, 2)] == (44u << 16 | 5u) &&
          S.reg[DPR(regDP0_DP_MSA_TIMING_PARAM4, 2)] == (1920u << 16 | 1080u));
    CHECK((S.reg[DPR(regDP0_DP_PIXEL_FORMAT, 2)] & DP0_DP_PIXEL_FORMAT__DP_COMPONENT_DEPTH_MASK) ==
          1u << DP0_DP_PIXEL_FORMAT__DP_COMPONENT_DEPTH__SHIFT);
    /* M / N = 148.5 MHz / 162 MHz */
    m_vid = (uint32_t)(0x8000ull * 148500 / (RD_DP_LINK_RATE_LOW * 27000));
    CHECK(S.reg[DPR(regDP0_DP_VID_M, 2)] == m_vid && S.reg[DPR(regDP0_DP_VID_N, 2)] == 0x8000 &&
          (S.reg[DPR(regDP0_DP_VID_TIMING, 2)] & DP0_DP_VID_TIMING__DP_VID_M_N_GEN_EN_MASK) &&
          (S.reg[DPR(regDP0_DP_VID_STREAM_CNTL, 2)] & DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK) &&
          !(S.reg[DPR(regDP0_DP_STEER_FIFO, 2)] & DP0_DP_STEER_FIFO__DP_STEER_FIFO_RESET_MASK) &&
          (S.reg[DIGN(regDIG0_DIG_FIFO_CTRL0, 2)] & DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE_MASK));
    CHECK((S.reg[DIGN(regDIG0_DIG_BE_CNTL, 1)] & (1u << (2 + DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT))) &&
          !(S.reg[DIGN(regDIG0_DIG_BE_CNTL, 1)] & DIG0_DIG_BE_CNTL__DIG_MODE_MASK) &&
          (S.reg[DIGN(regDIG0_DIG_BE_EN_CNTL, 1)] & DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK));
    CHECK(dc.head[0].width == 1920 && S.reg[OTG(regOTG0_OTG_H_TOTAL, otg)] == 2199 &&
          !(S.reg[HUBP(regHUBP0_DCHUBP_CNTL, dc.head[0].pipe)] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK));
    CHECK(dcn32_dp_link_ok(&dp));

    /* back to 1440p (HBR x4) with a sink that never locks at HBR: four
     * attempts fail, the 1080p timing comes back, trained again at RBR */
    memset(&up, 0, sizeof(up));
    up.h_active = 2560; up.v_active = 1440; up.h_total = 2720; up.v_total = 1481;
    up.h_front = 48; up.h_sync = 32; up.v_front = 3; up.v_sync = 5; up.pixel_khz = 241500;
    up.h_sync_pos = 1;
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].h_active == 1920);
    CHECK(dcn32_modeset_plan_dp(dev, &dc, 0, &own, &dp, 148500, &up, NULL, &ms) == RD_MODESET_OK);
    S.dps.fail_rate = RD_DP_LINK_RATE_HIGH;
    runs = dp.lt_runs;
    CHECK(dcn32_modeset_apply(dev, dm, &dc, NULL, &ms, FB_START, 2560, FB_START + 0x8000000, 1920, 50) == RDNA3_EIO);
    CHECK(dp.lt_runs == runs + RD_DP_LINK_TRAINING_ATTEMPTS + 1 && dc.head[0].width == 1920 &&
          S.dps.trained_rate == RD_DP_LINK_RATE_LOW && S.dps.cr && S.dps.eq &&
          S.reg[OTG(regOTG0_OTG_H_TOTAL, otg)] == 2199 && S.reg[DCN_B1 + regDP_DTO0_PHASE + 4 * otg] == 148500000u);

    /* perform_link_training_with_retries with fallback: HBR3 x4 -> HBR2 x4 */
    S.dps.fail_rate = RD_DP_LINK_RATE_HIGH3;
    st.lane_count = 4; st.link_rate = RD_DP_LINK_RATE_HIGH3; st.link_spread = RD_DP_LINK_SPREAD_05_DOWNSPREAD_30KHZ;
    runs = dp.lt_fallbacks;
    CHECK(dcn32_dp_link_training_with_retries(&dp, &st, 1, RD_DP_LINK_TRAINING_ATTEMPTS, 1, 3564000) == 1 &&
          dp.cur_link_settings.link_rate == RD_DP_LINK_RATE_HIGH2 && dp.lt_fallbacks == runs + 1 &&
          S.dps.trained_rate == RD_DP_LINK_RATE_HIGH2);
    /* without fallback: every attempt at HBR3 */
    runs = dp.lt_runs;
    CHECK(dcn32_dp_link_training_with_retries(&dp, &st, 1, RD_DP_LINK_TRAINING_ATTEMPTS, 0, 3564000) == 0 &&
          dp.lt_runs == runs + RD_DP_LINK_TRAINING_ATTEMPTS && dp.last_result == RD_LT_CR_FAIL_LANE0);
    /* a fallback below what the stream needs stops at once */
    runs = dp.lt_runs;
    CHECK(dcn32_dp_link_training_with_retries(&dp, &st, 1, RD_DP_LINK_TRAINING_ATTEMPTS, 1, 20000000) == 0 &&
          dp.lt_runs == runs + 1);

    /* and back to the boot timing over HBR x4 (the link up again first:
     * the calls above left the transmitter off: no DIG on the head) */
    S.dps.fail_rate = 0;
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].dig_be < 0);
    st.lane_count = 4; st.link_rate = RD_DP_LINK_RATE_LOW;
    dcn32_dp_enable_link_phy(&dp, &st);
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].dig_mode == 0);
    CHECK(dcn32_modeset_plan_dp(dev, &dc, 0, &own, &dp, 148500, &up, NULL, &ms) == RD_MODESET_OK);
    CHECK(dcn32_modeset_apply(dev, dm, &dc, NULL, &ms, FB_START, 2560, FB_START + 0x8000000, 1920, 50) == RDNA3_OK &&
          dc.head[0].width == 2560 && S.dps.trained_rate == RD_DP_LINK_RATE_HIGH && S.dpcd[0x101] == 0x84);
    S.dcn_frames = 0;

    /* the HDMI head again for the tests that follow */
    S.reg[DIGN(regDIG0_DIG_BE_CNTL, 1)] = be_cntl;
    S.reg[DCN_B1 + regDP_DTO0_PHASE + 4 * otg] = dto[0];
    S.reg[DCN_B1 + regDP_DTO0_MODULO + 4 * otg] = dto[1];
    S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + 4 * otg] = dto[2];
    memcpy(S.dpcd, dpcd_save, sizeof(S.dpcd));
    S.dps.be = -1;
    CHECK(dcn32_read_state(dev, &dc) == 1 && dc.head[0].dig_mode == 3 && dc.head[0].h_active == 2560);
}

/* PM control (stage 3 F): fan, power limit, overdrive, clocks / profiles,
 * DF C-states, BACO and the userspace dispatcher with its gates, against
 * the PMFW model (overdrive validation, activity monitor table, FDO
 * registers, BACO state) */
static void pmctl_req(rdna3_pmctl_req *rq, uint32_t op, uint32_t n, const int32_t *a)
{
    uint32_t i;
    memset(rq, 0, sizeof(*rq));
    rq->abi = RDNA3_PMCTL_ABI;
    rq->op = op;
    rq->nargs = n;
    for (i = 0; i < n; i++)
        rq->a[i] = a[i];
}

static void test_pm_control(rdna3_dev *dev, const rdna3_vbios *vb)
{
    static rdna3_smu smu;
    static rdna3_pmctl_reply rp;
    rdna3_pmctl_req rq;
    rdna3_temp_range tr;
    rdna3_gpu_metrics gm;
    rdna3_pmctl_coeff co;
    long in[16];
    uint32_t v = 0, n0;
    const uint32_t all = RD_PMCTL_GATE_WRITE | RD_PMCTL_GATE_OD | RD_PMCTL_GATE_ADMIN;
    const uint64_t fan_bit = RD_SMU_FEATURE(FEATURE_FAN_CONTROL_BIT);

    /* a PMFW straight after boot */
    S.smu.od_init = 0; S.smu.ppt_limit = 0; S.smu.od_uploads = S.smu.od_rejects = 0;
    S.smu.coeff_writes = S.smu.coeff_rejects = S.smu.feature_msgs = 0;
    S.smu.scpm = 0; S.smu.pptable_written = 0; S.smu.btc = 0;
    CHECK(smu13_pm_init(dev, &smu, vb, RD_SMU_FEATURES_SAFE, 3, 6, 0) == RDNA3_OK);
    CHECK(smu.od_ppt_pct_max == 15 && smu.od_ppt_pct_min == 10);
    CHECK(smu.supported == smu.enabled && smu.baco_state == RD_SMU_BACO_STATE_NONE);

    /* late-init queries: no overdrive table, no feature switch, no limit set */
    CHECK(smu13_pm_query(dev, &smu, vb, 0) == RDNA3_OK);
    CHECK(smu.baco_platform && smu.maco_support && !smu.dc_controlled_by_gpio && !smu.no_fan);
    CHECK(!smu.od_enabled && !smu.od_valid && smu.bios_scratch_reg == SIM_BIOS_SCRATCH);
    CHECK(smu.current_power_limit == 263 && smu.power_limit_default == 263);
    CHECK(smu.power_limit_max == 280 && smu.power_limit_min == 263 * 90 / 100);
    CHECK(smu.unique_id == 0x1234ABCD5678EF01ull);
    CHECK(smu.pstate_gfx.min == 500 && smu.pstate_gfx.peak == 2124 && smu.pstate_gfx.standard == 1295);
    CHECK(smu.pstate_uclk.peak == 96 && smu.pstate_uclk.standard == 96 && smu.pstate_soc.standard == 600);
    CHECK(S.smu.od_uploads == 0 && S.smu.feature_msgs == 0 && S.smu.ppt_limit == 0);
    /* amdgpu.ppfeaturemask with PP_OVERDRIVE_MASK: the PPT may go 15 % over the message limit */
    CHECK(smu13_pm_query(dev, &smu, vb, 1) == RDNA3_OK && smu.od_enabled);
    CHECK(smu.power_limit_max == 280 * 115 / 100);

    /* smu_v13_0_0_get_thermal_temperature_range */
    CHECK(smu13_get_thermal_temperature_range(&smu, &tr) == RDNA3_OK);
    CHECK(tr.min == -273150 && tr.max == 100000 && tr.edge_emergency_max == 105000);
    CHECK(tr.hotspot_crit_max == 110000 && tr.hotspot_emergency_max == 115000);
    CHECK(tr.mem_crit_max == 105000 && tr.mem_emergency_max == 110000);
    CHECK(tr.software_shutdown_temp == 118 && tr.software_shutdown_temp_offset == 10);

    /* smu_v13_0_0_get_gpu_metrics: idle (activity <= 15 %) reports the post-DS clocks */
    CHECK(smu13_get_gpu_metrics(dev, &smu, &gm) == RDNA3_OK);
    CHECK(gm.average_gfxclk_frequency == 40 && gm.current_gfxclk == 40 && gm.average_uclk_frequency == 96);
    CHECK(gm.temperature_hotspot == 60 && gm.temperature_vrgfx == 55 && gm.temperature_vrmem == 52);
    CHECK(gm.pcie_link_speed == 160 && gm.pcie_link_width == 16 && gm.voltage_soc == 750 &&
          gm.voltage_mem == 1250 && gm.energy_accumulator == 0x12345678u);
    CHECK(gm.current_fan_speed == 900 && gm.throttle_status == 0 && gm.indep_throttle_status == 0);
    /* smu_v13_0_0_get_current_clk_freq_by_table */
    CHECK(smu13_get_current_clk_freq_by_table(dev, &smu, PPCLK_GFXCLK, &v) == RDNA3_OK && v == 40);
    CHECK(smu13_get_current_clk_freq_by_table(dev, &smu, PPCLK_UCLK, &v) == RDNA3_OK && v == 96);
    CHECK(smu13_get_current_clk_freq_by_table(dev, &smu, PPCLK_SOCCLK, &v) == RDNA3_OK && v == 900);
    CHECK(smu13_get_current_clk_freq_by_table(dev, &smu, PPCLK_COUNT, &v) == RDNA3_EINVAL);
    S.smu.sim_throttle = 1;
    CHECK(smu13_get_gpu_metrics(dev, &smu, &gm) == RDNA3_OK);
    CHECK(gm.throttle_status == ((1u << THROTTLER_PPT0_BIT) | (1u << THROTTLER_TEMP_HOTSPOT_BIT)));
    CHECK(gm.indep_throttle_status == ((1ull << 0) | (1ull << 36)));
    S.smu.sim_throttle = 0;

    /* fan: PMFW control, then manual through the THM FDO */
    CHECK(smu13_get_fan_control_mode(dev, &smu) == RD_FAN_CTRL_AUTO);
    CHECK(smu13_get_fan_speed_pwm(dev, &smu, &v) == RDNA3_OK && v == 30 * 255 / 100);
    CHECK(smu13_get_fan_speed_rpm(dev, &smu, &v) == RDNA3_OK && v == 900);
    CHECK(smu13_set_fan_control_mode(dev, &smu, RD_FAN_CTRL_MANUAL) == RDNA3_OK);
    CHECK(!(S.smu.enabled & fan_bit) && S.smu.feature_msgs == 2);
    CHECK(smu13_get_fan_control_mode(dev, &smu) == RD_FAN_CTRL_MANUAL);
    CHECK(smu13_set_fan_speed_pwm(dev, &smu, 128) == RDNA3_OK);
    CHECK((S.reg[THMR(regCG_FDO_CTRL0)] & CG_FDO_CTRL0__FDO_STATIC_DUTY_MASK) == 128 * 200 / 255);
    CHECK(((S.reg[THMR(regCG_FDO_CTRL2)] & CG_FDO_CTRL2__FDO_PWM_MODE_MASK) >>
           CG_FDO_CTRL2__FDO_PWM_MODE__SHIFT) == RD_FDO_PWM_MODE_STATIC);
    CHECK((S.reg[THMR(regCG_FDO_CTRL2)] & CG_FDO_CTRL2__TMIN_MASK) == 0);
    CHECK(smu13_get_fan_speed_pwm(dev, &smu, &v) == RDNA3_OK && v == 50 * 255 / 100);
    CHECK(smu13_get_fan_speed_rpm(dev, &smu, &v) == RDNA3_OK && v == 1650);
    CHECK(smu13_set_fan_speed_rpm(dev, &smu, 0) == RDNA3_EINVAL);
    CHECK(smu13_set_fan_speed_rpm(dev, &smu, 2000) == RDNA3_OK);
    CHECK(((S.reg[THMR(regCG_TACH_CTRL)] & CG_TACH_CTRL__TARGET_PERIOD_MASK) >>
           CG_TACH_CTRL__TARGET_PERIOD__SHIFT) == 60u * 2500u * 10000u / (8u * 2000u));
    CHECK(smu13_get_fan_speed_rpm(dev, &smu, &v) == RDNA3_OK && v == 2000);
    CHECK(smu13_set_fan_control_mode(dev, &smu, RD_FAN_CTRL_NONE) == RDNA3_OK);   /* full speed */
    CHECK(smu13_get_fan_speed_pwm(dev, &smu, &v) == RDNA3_OK && v == 255);
    dev->ipver[HWIP_THM].present = 0;
    CHECK(smu13_set_fan_speed_pwm(dev, &smu, 100) == RDNA3_ENODEV);
    dev->ipver[HWIP_THM].present = 1;
    CHECK(smu13_set_fan_control_mode(dev, &smu, RD_FAN_CTRL_AUTO) == RDNA3_OK && (S.smu.enabled & fan_bit));
    CHECK(smu13_get_fan_speed_rpm(dev, &smu, &v) == RDNA3_OK && v == 900);

    /* power limit within the PPTable's limits (no OD) */
    CHECK(smu13_pm_query(dev, &smu, vb, 0) == RDNA3_OK);
    CHECK(smu13_set_ppt_limit(dev, &smu, 281) == RDNA3_EINVAL);
    CHECK(smu13_set_ppt_limit(dev, &smu, 200) == RDNA3_EINVAL);    /* under 90 % */
    CHECK(smu13_set_ppt_limit(dev, &smu, 250) == RDNA3_OK && S.smu.ppt_limit == 250 &&
          smu.current_power_limit == 250);
    /* with OD: above 280 W through the OD table's Ppt percentage */
    CHECK(smu13_pm_query(dev, &smu, vb, 1) == RDNA3_OK && smu.current_power_limit == 250);
    CHECK(smu13_set_default_od_settings(dev, &smu, 0) == RDNA3_OK && smu.od_valid && !smu.user_od);
    CHECK(smu.od_feature_mask == 0x3fff && smu.od_boot.OverDriveTable.GfxclkFmax == 2600 &&
          smu.od.OverDriveTable.FanZeroRpmStopTemp == 50);
    CHECK(smu13_set_ppt_limit(dev, &smu, 300) == RDNA3_OK);
    CHECK(S.smu.ppt_limit == 280 && S.smu.od.Ppt == 300 * 100 / 280 - 100 && smu.current_power_limit == 300);
    CHECK(S.smu.od_uploads == 1 && (S.smu.od_last_mask & (1u << PP_OD_FEATURE_PPT_BIT)));
    CHECK(smu13_set_ppt_limit(dev, &smu, 250) == RDNA3_OK);   /* back under: Ppt 0 first */
    CHECK(S.smu.od.Ppt == 0 && S.smu.ppt_limit == 250 && S.smu.od_uploads == 2);
    CHECK(S.smu.unflushed == 0);

    /* overdrive: GFXCLK / UCLK limits, voltage offset, commit */
    in[0] = 0; in[1] = 600; in[2] = 1; in[3] = 2800;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_SCLK_VDDC_TABLE, in, 4) == RDNA3_OK);
    CHECK(smu.od.OverDriveTable.GfxclkFmin == 600 && smu.od.OverDriveTable.GfxclkFmax == 2800);
    in[0] = 0; in[1] = 100; in[2] = 1; in[3] = 1300;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_MCLK_VDDC_TABLE, in, 4) == RDNA3_OK);
    in[0] = -50;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_VDDGFX_OFFSET, in, 1) == RDNA3_OK);
    n0 = S.smu.od_uploads;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_COMMIT_DPM_TABLE, NULL, 0) == RDNA3_OK);
    CHECK(S.smu.od_uploads == n0 + 1 && S.smu.od.GfxclkFmin == 600 && S.smu.od.GfxclkFmax == 2800);
    CHECK(S.smu.od.UclkFmin == 100 && S.smu.od.UclkFmax == 1300 &&
          S.smu.od.VoltageOffsetPerZoneBoundary[5] == -50);
    /* as in amdgpu the PPT bit of the power limit uploads is still set */
    CHECK(S.smu.od_last_mask == ((1u << PP_OD_FEATURE_GFXCLK_BIT) | (1u << PP_OD_FEATURE_UCLK_BIT) |
                                 (1u << PP_OD_FEATURE_GFX_VF_CURVE_BIT) | (1u << PP_OD_FEATURE_PPT_BIT)));
    CHECK(smu.od.OverDriveTable.FeatureCtrlMask == 0 && smu.user_od);
    /* nothing changed since: no upload */
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_COMMIT_DPM_TABLE, NULL, 0) == RDNA3_OK &&
          S.smu.od_uploads == n0 + 1);
    /* the driver's own range checks */
    in[0] = 1; in[1] = 3100;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_SCLK_VDDC_TABLE, in, 2) == RDNA3_EINVAL);
    in[0] = 0; in[1] = 2500; in[2] = 1; in[3] = 2000;    /* min above max */
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_SCLK_VDDC_TABLE, in, 4) == RDNA3_EINVAL);
    in[0] = 0; in[1] = 600; in[2] = 1;                   /* odd count */
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_SCLK_VDDC_TABLE, in, 3) == RDNA3_EINVAL);
    in[0] = 0; in[1] = 600; in[2] = 1; in[3] = 2800;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_SCLK_VDDC_TABLE, in, 4) == RDNA3_OK);
    in[0] = -200;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_VDDGFX_OFFSET, in, 1) == RDNA3_EINVAL);
    CHECK(smu13_od_edit_dpm_table(dev, &smu, 99, in, 1) == RDNA3_EOPNOTSUPP);
    /* the PMFW's own check: a value past the PPTable limits is refused */
    smu.od.OverDriveTable.GfxclkFmax = 3500;
    smu.od.OverDriveTable.FeatureCtrlMask = 1u << PP_OD_FEATURE_GFXCLK_BIT;
    CHECK(smu13_upload_overdrive_table(dev, &smu, &smu.od) != RDNA3_OK && S.smu.od_rejects == 1 &&
          S.smu.od.GfxclkFmax == 2800);
    smu.od.OverDriveTable.GfxclkFmax = 2800;
    smu.od.OverDriveTable.FeatureCtrlMask = 0;

    /* fan curve (the PMFW leaves its auto curve), acoustic, target, zero RPM */
    in[0] = 2; in[1] = 60; in[2] = 70;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_CURVE, in, 3) == RDNA3_OK);
    CHECK(smu.od.OverDriveTable.FanMode == FAN_MODE_MANUAL_LINEAR);
    in[0] = 5;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_CURVE, in, 3) == RDNA3_EINVAL);
    in[0] = 1; in[1] = 120;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_CURVE, in, 3) == RDNA3_EINVAL);
    in[0] = 2500;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_ACOUSTIC_LIMIT, in, 1) == RDNA3_OK);
    in[0] = 90;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_TARGET_TEMPERATURE, in, 1) == RDNA3_OK);
    CHECK(smu.od.OverDriveTable.FanMode == FAN_MODE_AUTO);   /* as amdgpu: these go back to auto */
    in[0] = 2; in[1] = 60; in[2] = 70;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_CURVE, in, 3) == RDNA3_OK);
    in[0] = 0;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE, in, 1) == RDNA3_OK);
    in[0] = 60;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP, in, 1) == RDNA3_OK);
    in[0] = 2;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE, in, 1) == RDNA3_EINVAL);
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_COMMIT_DPM_TABLE, NULL, 0) == RDNA3_OK);
    CHECK(S.smu.od.FanMode == FAN_MODE_MANUAL_LINEAR && S.smu.od.FanLinearPwmPoints[2] == 70 &&
          S.smu.od.AcousticLimitRpmThreshold == 2500 && S.smu.od.FanTargetTemperature == 90);
    CHECK(S.smu.od.FanZeroRpmEnable == 0 && S.smu.od.FanZeroRpmStopTemp == 60);
    CHECK(smu13_get_fan_speed_rpm(dev, &smu, &v) == RDNA3_OK && v == 70 * 3300 / 100);
    /* restore one setting: the fan curve back to the boot one, auto again */
    in[0] = RD_PP_OD_EDIT_FAN_CURVE;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_RESTORE_DEFAULT_TABLE, in, 1) == RDNA3_OK);
    CHECK(S.smu.od.FanMode == FAN_MODE_AUTO && S.smu.od.FanLinearPwmPoints[2] == 45);
    CHECK(smu13_get_fan_speed_rpm(dev, &smu, &v) == RDNA3_OK && v == 900);

    /* resume / re-init: the PMFW lost the OD settings, the user's come back */
    S.smu.od_init = 0;
    CHECK(smu13_set_default_od_settings(dev, &smu, 1) == RDNA3_OK && smu.user_od);
    CHECK(smu.od.OverDriveTable.GfxclkFmax == 2600 && smu.od_user.OverDriveTable.GfxclkFmax == 2800);
    CHECK(smu13_restore_user_od_settings(dev, &smu) == RDNA3_OK);
    CHECK(S.smu.od.GfxclkFmax == 2800 && S.smu.od.UclkFmax == 1300 &&
          S.smu.od.VoltageOffsetPerZoneBoundary[0] == -50 && S.smu.od.AcousticLimitRpmThreshold == 2500);
    CHECK(smu.od_user.OverDriveTable.FeatureCtrlMask == 0 && smu.od.OverDriveTable.GfxclkFmax == 2800);
    /* restore everything: the driver's tables are the boot ones again. As
     * in amdgpu the upload carries the FeatureCtrlMask of the edits since
     * the last commit (none here), so the PMFW keeps what it runs */
    n0 = S.smu.od_uploads;
    CHECK(smu13_od_edit_dpm_table(dev, &smu, RD_PP_OD_RESTORE_DEFAULT_TABLE, NULL, 0) == RDNA3_OK);
    CHECK(S.smu.od_uploads == n0 + 1 && S.smu.od_last_mask == 0);
    CHECK(smu.od.OverDriveTable.GfxclkFmax == 2600 && !smu.user_od);

    /* clocks: pp_dpm_* masks */
    CHECK(smu13_force_clk_levels(dev, &smu, PPCLK_SOCCLK, 0x6) == RDNA3_OK &&
          S.smu.soft_min[PPCLK_SOCCLK] == 900 && S.smu.soft_max[PPCLK_SOCCLK] == 1200);
    CHECK(smu13_force_clk_levels(dev, &smu, PPCLK_GFXCLK, 0x4) == RDNA3_OK &&   /* fine grained: 2 -> 1 */
          S.smu.soft_min[PPCLK_GFXCLK] == 2124 && S.smu.soft_max[PPCLK_GFXCLK] == 2124);
    CHECK(smu13_force_clk_levels(dev, &smu, PPCLK_SOCCLK, 0x8) == RDNA3_EINVAL);
    n0 = S.smu.soft_limits;
    CHECK(smu13_force_clk_levels(dev, &smu, PPCLK_UCLK, 0x1) == RDNA3_OK && S.smu.soft_limits == n0);
    CHECK(smu13_get_dpm_ultimate_freq(&smu, PPCLK_SOCCLK, &v, NULL) == RDNA3_OK && v == 600);
    CHECK(smu13_get_dpm_ultimate_freq(&smu, PPCLK_VCLK_0, &v, NULL) == RDNA3_EINVAL);

    /* profiles: 13.0.10 PMFW >= 80.69 adds POWER_SAVING to the mask */
    in[0] = RD_PP_SMC_POWER_PROFILE_COMPUTE;
    CHECK(smu13_set_power_profile_mode(dev, &smu, in, 0) == RDNA3_OK);
    CHECK(S.smu.workload == ((1u << WORKLOAD_PPLIB_COMPUTE_BIT) | (1u << WORKLOAD_PPLIB_POWER_SAVING_BIT)));
    CHECK(smu.workload_mask == (1u << WORKLOAD_PPLIB_COMPUTE_BIT) &&
          smu.power_profile_mode == RD_PP_SMC_POWER_PROFILE_COMPUTE);
    in[0] = 0; in[1] = 60; in[2] = 1; in[3] = 1500; in[4] = 0; in[5] = 2000; in[6] = 100; in[7] = 200;
    in[8] = 300; in[9] = RD_PP_SMC_POWER_PROFILE_CUSTOM;
    CHECK(smu13_set_power_profile_mode(dev, &smu, in, 9) == RDNA3_OK && S.smu.coeff_writes == 1);
    CHECK(S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT].DpmActivityMonitorCoeffInt.Gfx_FPS == 60 &&
          S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT].DpmActivityMonitorCoeffInt.Gfx_MinActiveFreq == 1500 &&
          S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT].DpmActivityMonitorCoeffInt.Gfx_PD_Data_error_rate_coeff == 300);
    CHECK(S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT].DpmActivityMonitorCoeffInt.Fclk_FPS == 26);   /* untouched */
    CHECK(S.smu.workload == ((1u << WORKLOAD_PPLIB_CUSTOM_BIT) | (1u << WORKLOAD_PPLIB_POWER_SAVING_BIT)));
    in[3] = RD_PP_SMC_POWER_PROFILE_CUSTOM;
    CHECK(smu13_set_power_profile_mode(dev, &smu, in, 3) == RDNA3_EINVAL);   /* custom needs 9 */
    in[0] = RD_PP_SMC_POWER_PROFILE_CAPPED;
    CHECK(smu13_set_power_profile_mode(dev, &smu, in, 0) == RDNA3_EINVAL);   /* no workload on 13.0.0 */
    in[0] = 12;
    CHECK(smu13_set_power_profile_mode(dev, &smu, in, 0) == RDNA3_EINVAL);
    CHECK(smu13_get_power_profile_coeff(dev, &smu, WORKLOAD_PPLIB_VIDEO_BIT, &co) == RDNA3_OK &&
          co.gfx[0] == 13 && co.gfx[2] == 800 && co.fclk[2] == 803);
    CHECK(smu13_get_power_profile_coeff(dev, &smu, 9, &co) == RDNA3_EINVAL);

    /* DF C-states */
    CHECK(smu13_set_df_cstate(dev, &smu, 0) == RDNA3_OK && S.smu.df_cstate == 0 && smu.df_cstate == 0);
    CHECK(smu13_set_df_cstate(dev, &smu, 1) == RDNA3_OK && S.smu.df_cstate == 1);

    /* the userspace path and its gates */
    pmctl_req(&rq, RD_PMCTL_INFO, 0, NULL);
    CHECK(smu13_pmctl(dev, &smu, &rq, &rp, 0) == RDNA3_OK && rp.err == 0 && rp.info.gates == 0);
    CHECK(rp.info.fan_mode == RD_FAN_CTRL_AUTO && rp.info.fan_rpm == 900 && rp.info.fan_max_rpm == 3300);
    CHECK(rp.info.power_current == 250 && rp.info.power_max == 322 && rp.info.od_valid &&
          rp.info.od_feature_ctrl == SIM_OD_FEATURES && rp.info.od_lim_max[RD_OD_SET_GFXCLK_FMAX] == 3000);
    CHECK(rp.info.gfxclk_max == 2124 && rp.info.socclk_max == 1200 && rp.info.gfxclk_standard == 1295);
    CHECK(rp.info.socclk_levels == 3 && rp.info.socclk_lv[1] == 900 && rp.info.socclk_cur == 900 &&
          rp.info.fclk_cur == 1000 && rp.info.gfxclk_lv[1] == 2124);
    CHECK(rp.info.thermal.hotspot_crit_max == 110000 && rp.info.unique_id == 0x1234ABCD5678EF01ull);
    CHECK(rp.info.baco_support == (RD_BACO_SUPPORT | RD_MACO_SUPPORT) && rp.info.df_cstate == 1);
    CHECK(rp.info.metrics.temperature_edge == 45 && rp.info.workload_mask == (1u << WORKLOAD_PPLIB_CUSTOM_BIT));
    {
        int32_t a[10] = { RD_FAN_CTRL_MANUAL };
        n0 = S.smu.feature_msgs;
        pmctl_req(&rq, RD_PMCTL_FAN_MODE, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, 0) == RDNA3_EOPNOTSUPP);         /* no rdna3pmctl */
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, RD_PMCTL_GATE_WRITE) == RDNA3_EPERM);   /* no admin */
        CHECK(S.smu.feature_msgs == n0 && (S.smu.enabled & fan_bit));
        a[0] = 255;
        pmctl_req(&rq, RD_PMCTL_FAN_PWM, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_EINVAL);   /* manual mode first */
        a[0] = RD_FAN_CTRL_MANUAL;
        pmctl_req(&rq, RD_PMCTL_FAN_MODE, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.info.fan_mode == RD_FAN_CTRL_MANUAL);
        a[0] = 255;
        pmctl_req(&rq, RD_PMCTL_FAN_PWM, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.info.fan_pwm == 255);
        a[0] = 1800;
        pmctl_req(&rq, RD_PMCTL_FAN_RPM, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.info.fan_rpm == 1800);
        a[0] = RD_FAN_CTRL_AUTO;
        pmctl_req(&rq, RD_PMCTL_FAN_MODE, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.info.fan_rpm == 900);
        a[0] = 260;
        pmctl_req(&rq, RD_PMCTL_POWER_LIMIT, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.info.power_current == 260);
        /* overdrive needs rdna3pmctl=2 */
        a[0] = RD_PP_OD_EDIT_FAN_CURVE; a[1] = 1; a[2] = 50; a[3] = 55;
        pmctl_req(&rq, RD_PMCTL_OD_EDIT, 4, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, RD_PMCTL_GATE_WRITE | RD_PMCTL_GATE_ADMIN) ==
              RDNA3_EOPNOTSUPP);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK);
        pmctl_req(&rq, RD_PMCTL_OD_EDIT, 2, a);                         /* too few inputs */
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_EINVAL);
        a[0] = RD_PP_OD_COMMIT_DPM_TABLE;
        pmctl_req(&rq, RD_PMCTL_OD_EDIT, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK);
        CHECK(rp.info.od_fan_curve_pwm[1] == 55 && rp.info.od_fan_curve_temp[1] == 50 &&
              rp.info.od_fan_mode == FAN_MODE_MANUAL_LINEAR && rp.info.user_od && S.smu.od.FanLinearPwmPoints[1] == 55);
        a[0] = RD_PP_SMC_POWER_PROFILE_FULLSCREEN3D;
        pmctl_req(&rq, RD_PMCTL_PROFILE, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK &&
              rp.info.power_profile_mode == RD_PP_SMC_POWER_PROFILE_FULLSCREEN3D &&
              S.smu.workload == ((1u << WORKLOAD_PPLIB_FULL_SCREEN_3D_BIT) | (1u << WORKLOAD_PPLIB_POWER_SAVING_BIT)));
        a[0] = RD_PP_SMC_POWER_PROFILE_CUSTOM; a[1] = 1; a[2] = 30; a[3] = 0; a[4] = 1000; a[5] = 0;
        a[6] = 1500; a[7] = 1; a[8] = 2; a[9] = 3;
        pmctl_req(&rq, RD_PMCTL_PROFILE, 10, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK &&
              S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT].DpmActivityMonitorCoeffInt.Fclk_FPS == 30 &&
              S.smu.coeff[WORKLOAD_PPLIB_CUSTOM_BIT].DpmActivityMonitorCoeffInt.Fclk_BoosterFreq == 1500);
        a[0] = WORKLOAD_PPLIB_CUSTOM_BIT;
        pmctl_req(&rq, RD_PMCTL_PROFILE_COEFF, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, 0) == RDNA3_EOPNOTSUPP);   /* a new table: gated */
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.coeff.fclk[0] == 30 &&
              rp.coeff.gfx[0] == 60);
        a[0] = RD_PP_SOCCLK; a[1] = 0x1;
        pmctl_req(&rq, RD_PMCTL_FORCE_LEVELS, 2, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && S.smu.soft_max[PPCLK_SOCCLK] == 600);
        a[0] = 7;
        pmctl_req(&rq, RD_PMCTL_FORCE_LEVELS, 2, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_EINVAL);
        a[0] = 2;
        pmctl_req(&rq, RD_PMCTL_DF_CSTATE, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_EINVAL);
        a[0] = 0;
        pmctl_req(&rq, RD_PMCTL_DF_CSTATE, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && rp.info.df_cstate == 0);
        /* power_dpm_force_performance_level */
        a[0] = (int32_t)RD_DPM_FORCED_LEVEL_LOW;
        pmctl_req(&rq, RD_PMCTL_PERF_LEVEL, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, 0) == RDNA3_EOPNOTSUPP);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && smu.dpm_level == RD_DPM_FORCED_LEVEL_LOW &&
              S.smu.soft_max[PPCLK_GFXCLK] == 500 && S.smu.soft_min[PPCLK_GFXCLK] == 500);
        a[0] = (int32_t)RD_DPM_FORCED_LEVEL_AUTO;
        pmctl_req(&rq, RD_PMCTL_PERF_LEVEL, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_OK && S.smu.soft_max[PPCLK_GFXCLK] == 2124);
        a[0] = 0;
        pmctl_req(&rq, RD_PMCTL_PERF_LEVEL, 1, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_EINVAL);
        pmctl_req(&rq, RD_PMCTL_BACO_CYCLE, 0, a);
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all | RD_PMCTL_GATE_BACO) == RDNA3_EOPNOTSUPP);
        pmctl_req(&rq, RD_PMCTL_INFO, 0, a);
        rq.abi = 7;
        CHECK(smu13_pmctl(dev, &smu, &rq, &rp, all) == RDNA3_EINVAL);
        CHECK(S.smu.unflushed == 0 && S.smu.bad_addr == 0);
    }

    /* BACO: never on a suspend path, only reached through the boot-arg */
    CHECK(smu13_get_bamaco_support(dev, &smu) == (int)(RD_BACO_SUPPORT | RD_MACO_SUPPORT));
    S.reg[SIM_BIOS_SCRATCH + 6] = 0x55; S.reg[SIM_BIOS_SCRATCH + 7] = 0x77;
    CHECK(smu13_baco_enter(dev, &smu, 0, RD_BACO_SEQ_BACO) == RDNA3_OK);
    CHECK(S.smu.baco && S.smu.baco_seq == RD_BACO_SEQ_BACO && smu13_baco_get_state(&smu) == RD_SMU_BACO_STATE_ENTER);
    n0 = S.smu.msgs_in_baco;
    CHECK(smu13_get_bamaco_support(dev, &smu) == (int)(RD_BACO_SUPPORT | RD_MACO_SUPPORT) &&
          S.smu.msgs_in_baco == n0);    /* already in BACO: no message */
    CHECK(smu13_send_msg(dev, PPSMC_MSG_TestMessage, 1, NULL) != RDNA3_OK && S.smu.msgs_in_baco == n0 + 1);
    CHECK(smu13_baco_enter(dev, &smu, 0, RD_BACO_SEQ_BACO) == RDNA3_OK && S.smu.baco_enters == 1);
    CHECK(smu13_baco_exit(dev, &smu, 0) == RDNA3_OK && !S.smu.baco && S.smu.baco_exits == 1);
    CHECK(smu13_baco_get_state(&smu) == RD_SMU_BACO_STATE_EXIT);
    CHECK(S.reg[SIM_BIOS_SCRATCH + 6] == 0 && S.reg[SIM_BIOS_SCRATCH + 7] == 0);
    CHECK(smu13_baco_exit(dev, &smu, 0) == RDNA3_OK && S.smu.baco_exits == 1);   /* same state */
    /* runtime PM with audio: the ArmD3 sequence instead */
    CHECK(smu13_baco_enter(dev, &smu, 1, RD_BACO_SEQ_BAMACO) == RDNA3_OK && S.smu.armd3_seq == RD_BACO_SEQ_BAMACO &&
          S.smu.baco_enters == 1 && smu.baco_state == RD_SMU_BACO_STATE_ENTER);
    CHECK(smu13_baco_exit(dev, &smu, 1) == RDNA3_OK && S.smu.armd3_seq == RD_BACO_SEQ_ULPS &&
          smu.baco_state == RD_SMU_BACO_STATE_EXIT && S.smu.armd3_msgs == 2);
    /* the BACO feature stopped: no BACO, and the PMFW refuses to enter */
    S.smu.enabled &= ~RD_SMU_FEATURE(FEATURE_BACO_BIT);
    CHECK(smu13_get_bamaco_support(dev, &smu) == 0);
    CHECK(smu13_baco_enter(dev, &smu, 0, RD_BACO_SEQ_BACO) != RDNA3_OK && !S.smu.baco &&
          smu.baco_state == RD_SMU_BACO_STATE_EXIT);
    S.smu.enabled |= RD_SMU_FEATURE(FEATURE_BACO_BIT);
    smu.baco_platform = 0;
    CHECK(smu13_get_bamaco_support(dev, &smu) == 0);
}

/* ---- VCN 4.0 / JPEG 4.0 (vcn40.c) against the model above sim_wr ---- */

struct vcn_iv_seen { unsigned vcn[2], jpeg, other; };
static rdna3_vcn *g_vcn;
static void vcn_iv_cb(void *ctx, const rdna3_iv *iv)
{
    struct vcn_iv_seen *s = (struct vcn_iv_seen *)ctx;
    uint32_t inst = 9;
    if (vcn40_process_interrupt(g_vcn, iv->client_id, iv->src_id, &inst) && inst < 2)
        s->vcn[inst]++;
    else if (jpeg40_process_interrupt(g_vcn, iv->client_id, iv->src_id))
        s->jpeg++;
    else
        s->other++;
}

static void test_vcn(rdna3_dev *dev, rdna3_psp *psp, const amd_ucode_plan *plan0, const amd_fw_blob_ref *blobs,
                     rdna3_gmc *gmc, rdna3_vcn *v)
{
    static amd_ucode_plan plan;
    static rdna3_ih ih;
    const amd_ucode *u0, *u1;
    uint64_t stage, tmr[2], buf, vcpu_mc;
    uint32_t i, seq = 0, flags = 0, k, dbu0 = S.db_unclaimed;
    uint8_t fs[RD_VCN4_FWS_SIZE];
    struct vcn_iv_seen seen;
    rdna3_vcn_ib ib;

    /* amdgpu_vcn_setup_ucode: VCN1 (same image) joins the plan for rdna3vcn=1 */
    memcpy(&plan, plan0, sizeof(plan));
    u0 = amd_ucode_plan_find(&plan, AMD_UCODE_VCN);
    CHECK(u0 && u0->psp_type == GFX_FW_TYPE_VCN && !amd_ucode_plan_find(&plan, AMD_UCODE_VCN1));
    CHECK(amd_vcn_setup_ucode(blobs, 1, &plan) == AMD_FW_OK && plan.count == plan0->count);
    CHECK(amd_vcn_setup_ucode(blobs, 2, &plan) == AMD_FW_OK && plan.count == plan0->count + 1);
    CHECK(amd_vcn_setup_ucode(blobs, 2, &plan) == AMD_FW_OK && plan.count == plan0->count + 1);
    u0 = amd_ucode_plan_find(&plan, AMD_UCODE_VCN);
    u1 = amd_ucode_plan_find(&plan, AMD_UCODE_VCN1);
    CHECK(u0 && u1 && u1 == u0 + 1 && u1->psp_type == GFX_FW_TYPE_VCN1 && u1->blob == AMD_BLOB_VCN &&
          u1->offset == u0->offset && u1->size == u0->size);
    CHECK(plan.total_aligned == plan0->total_aligned + ((u0->size + 4095u) & ~4095u));
    for (i = 1; i < plan.count; i++)
        CHECK(plan.ucode[i - 1].ucode_id < plan.ucode[i].ucode_id);
    /* the PSP loads VCN1 too and answers where both images are in the TMR */
    stage = rdna3_vram_alloc(dev, (u1->size + 4095u) & ~4095u, 4096, "vcn1-stage");
    rdna3_vram_write(dev, stage, blobs[AMD_BLOB_VCN].data + u1->offset, u1->size);
    rdna3_hdp_flush(dev);
    CHECK(psp13_load_ip_fw(psp, u1, rdna3_vram_mc(dev, stage)) == RDNA3_OK);
    tmr[0] = psp->ucode_tmr_mc[AMD_UCODE_VCN];
    tmr[1] = psp->ucode_tmr_mc[AMD_UCODE_VCN1];
    CHECK(tmr[0] && tmr[1] && tmr[0] != tmr[1] && tmr[0] == S.vcn.tmr_fw[0] && tmr[1] == S.vcn.tmr_fw[1]);
    CHECK(psp->ucode_tmr_mc[AMD_UCODE_SMC] == 0);   /* only the VCN images are placed by the model */

    CHECK(ih60_init(dev, &ih, 4096, gmc->dummy_page_pa, 0) == RDNA3_OK);
    memset(&seen, 0, sizeof(seen));
    g_vcn = v;

    /* early / sw init: two instances from discovery, the header, vcpu_bo, fw_shared */
    memset(v, 0, sizeof(*v));
    CHECK(vcn40_early_init(dev, v, NULL, 0, 0x545, 1, RD_VCN_PG_STATIC) == RDNA3_ENOENT);
    CHECK(vcn40_early_init(dev, v, blobs[AMD_BLOB_VCN].data, blobs[AMD_BLOB_VCN].len, 0x545, 1, RD_VCN_PG_STATIC) == RDNA3_OK);
    CHECK(v->num_vcn_inst == 2 && v->harvest_config == 0 && v->num_enc_rings == 1);
    CHECK(v->fw_version == ((const struct common_firmware_header *)blobs[AMD_BLOB_VCN].data)->ucode_version);
    if (v->fw_version == 0x09118022)   /* vcn_4_0_0.bin of the card: ENC 1.24 DEC 9 VEP 0 Revision 34 */
        CHECK(v->enc_major == 1 && v->enc_minor == 24 && v->dec_ver == 9 && v->vep == 0 && v->fw_rev == 34);
    CHECK(v->cg_flags == (RD_CG_VCN_MGCG | RD_CG_JPEG_MGCG) && v->pg_flags == (RD_PG_VCN | RD_PG_JPEG));
    CHECK(v->cur_state == RD_PG_STATE_GATE && v->vcn_gated && v->jpeg_gated);
    CHECK(jpeg40_early_init(dev, v) == RDNA3_OK && v->jpeg_present);
    CHECK(vcn40_sw_init(dev, v, tmr) == RDNA3_OK && jpeg40_sw_init(dev, v) == RDNA3_OK);
    for (i = 0; i < 2; i++) {
        CHECK(v->fw_shared[i] == v->vcpu_bo[i] + RD_VCN_STACK_SIZE + RD_VCN_CONTEXT_SIZE);
        rdna3_vram_read(dev, v->fw_shared[i], fs, sizeof(fs));
        memcpy(&flags, fs, 4);
        CHECK(flags == (RD_FW_SHARED_FLAG_0_UNIFIED_QUEUE | RD_VCN_SMU_DPM_INTERFACE_FLAG) &&
              fs[RD_VCN4_FWS_SQ_IS_ENABLED] == 1 && fs[RD_VCN4_FWS_SMU_DPM_INTERFACE] == RD_VCN_SMU_DPM_INTERFACE_DGPU);
        CHECK(v->ring_enc[i].doorbell_index == (RD_DB_VCN0_1 << 1) + 2 + 8 * i && v->ring_enc[i].use_doorbell);
    }
    CHECK(v->jpeg.doorbell_index == (RD_DB_VCN0_1 << 1) + 1);
    CHECK(S.vcn.vcn_up_msgs == 0 && !S.vcn.smu_on[0] && !S.vcn.smu_on[1]);   /* still gated */

    /* hw init: doorbell ranges, the ring tests power the block up */
    for (i = 0; i < 2; i++) {
        S.reg[VCNR(i, regUVD_PGFSM_STATUS)] = 0x2AAAAAAAu;   /* every tile off at reset */
        S.reg[VCNR(i, regUVD_VCPU_CNTL)] = UVD_VCPU_CNTL__BLK_RST_MASK;   /* the VCPU held in reset */
    }
    CHECK(vcn40_hw_init(dev, v) == RDNA3_OK);
    CHECK(v->ring_enc[0].ready && v->ring_enc[1].ready && v->cur_state == RD_PG_STATE_UNGATE && !v->vcn_gated);
    CHECK(S.vcn.vcn_up_msgs == 2 && S.vcn.smu_on[0] && S.vcn.smu_on[1] && S.vcn.regs_while_off == 0);
    CHECK(S.vcn.boots == 2 && S.vcn.bad_boots == 0 && S.vcn.rb_enables == 2 && S.vcn.bad_fw_shared == 0);
    vcpu_mc = rdna3_vram_mc(dev, v->vcpu_bo[1]);
    for (i = 0; i < 2; i++) {
        uint32_t s2a = smn_val(i ? regS2A_DOORBELL_ENTRY_5_CTRL : regS2A_DOORBELL_ENTRY_4_CTRL);
        CHECK((S.reg[VCNR(i, regUVD_PGFSM_STATUS)] & 0x3F3FFFFF) == RD_UVD_PGFSM_STATUS__UVDM_UVDU_UVDLM_PWR_ON_3_0);
        CHECK((S.reg[VCNR(i, regUVD_POWER_STATUS)] & 0x103) == (UVD_POWER_STATUS__UVD_PG_EN_MASK | 2));
        CHECK(S.reg[VCNR(i, regUVD_STATUS)] == RD_UVD_STATUS__IDLE);   /* booted, busy bit cleared */
        CHECK(S.reg[VCNR(i, regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW)] == (uint32_t)tmr[i] &&
              S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE0)] == ((((const struct common_firmware_header *)
                                                              blobs[AMD_BLOB_VCN].data)->ucode_size_bytes + 8 + 4095u) & ~4095u));
        CHECK(S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE1)] == RD_VCN_STACK_SIZE &&
              S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE2)] == RD_VCN_CONTEXT_SIZE &&
              S.reg[VCNR(i, regUVD_VCPU_NONCACHE_SIZE0)] == 4096);
        CHECK(S.reg[VCNR(i, regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW)] == (uint32_t)rdna3_vram_mc(dev, v->fw_shared[i]));
        CHECK(S.reg[VCNR(i, regUVD_GFX10_ADDR_CONFIG)] == 0x545);
        CHECK(S.reg[VCNR(i, regUVD_MASTINT_EN)] & UVD_MASTINT_EN__VCPU_EN_MASK);
        CHECK(S.reg[VCNR(i, regVCN_RB1_DB_CTRL)] == (v->ring_enc[i].doorbell_index << 2 | VCN_RB1_DB_CTRL__EN_MASK));
        CHECK(S.reg[VCNR(i, regUVD_RB_BASE_LO)] == (uint32_t)v->ring_enc[i].gpu_addr &&
              S.reg[VCNR(i, regUVD_RB_SIZE)] == RD_VCN_RING_SIZE / 4);
        CHECK(s2a & S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_ENABLE_MASK);
        CHECK(((s2a & S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_OFFSET_MASK) >>
               S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_OFFSET__SHIFT) == (RD_DB_VCN0_1 << 1) + 8 * i);
        CHECK(((s2a & S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_AWID_MASK) >>
               S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_AWID__SHIFT) == (i ? 7u : 4u));
        rdna3_vram_read(dev, v->fw_shared[i], fs, sizeof(fs));
        CHECK(fs[RD_VCN4_FWS_SQ_QUEUE_MODE] == 0);   /* the ring-reset handshake is over */
    }
    CHECK(S.reg[VCNR(1, regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW)] == (uint32_t)vcpu_mc &&
          S.reg[VCNR(1, regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW)] == (uint32_t)(vcpu_mc + RD_VCN_STACK_SIZE));
    CHECK(S.vcn.dbs >= 2 && S.vcn.db_unrouted == 0 && S.db_unclaimed == dbu0 && S.vcn.ends == 2 && S.vcn.bad == 0);

    /* the IB tests: encode session create / destroy, decode create / destroy
     * messages behind the unified-queue checksum and engine info */
    for (i = 0; i < 2; i++)
        CHECK(vcn40_unified_ring_test_ib(dev, v, &v->ring_enc[i]) == RDNA3_OK);
    CHECK(S.vcn.enc_init == 2 && S.vcn.enc_close == 2 && S.vcn.dec_create == 2 && S.vcn.dec_destroy == 2);
    CHECK(S.vcn.checksum_bad == 0 && S.vcn.ib_faults == 0 && S.vcn.bad == 0 && S.vcn.ibs == 8);
    CHECK(S.vcn.fences == 8 && S.vcn.traps == 8 && S.unflushed_doorbells == 0);
    ih60_process(dev, &ih, vcn_iv_cb, &seen);
    CHECK(seen.vcn[0] == 4 && seen.vcn[1] == 4 && seen.jpeg == 0 && seen.other == 0 && v->irqs == 8);

    /* JPEG: VCN_JPEG_DB_CTRL, ring test through the pitch register, IB test
     * from the checked-copy pool (VMID 0), the decode trap */
    S.reg[VCNR(0, regUVD_PGFSM_STATUS)] |= 2u << UVD_PGFSM_STATUS__UVDJ_PWR_STATUS__SHIFT;
    CHECK(jpeg40_hw_init(dev, v) == RDNA3_OK && v->jpeg.ready && v->jpeg_cur_state == RD_PG_STATE_UNGATE);
    CHECK(S.vcn.jpeg_up_msgs == 1 && S.vcn.jpeg_smu_on && S.vcn.regs_while_off == 0);
    CHECK(S.reg[VCNR(0, regVCN_JPEG_DB_CTRL)] == (v->jpeg.doorbell_index << 2 | VCN_JPEG_DB_CTRL__EN_MASK));
    CHECK((S.reg[VCNR(0, regUVD_PGFSM_STATUS)] & UVD_PGFSM_STATUS__UVDJ_PWR_STATUS_MASK) == 0);
    CHECK(S.reg[VCNR(0, regUVD_JRBC_RB_CNTL)] == 2 && S.reg[VCNR(0, regUVD_JRBC_RB_SIZE)] == RD_JPEG_RING_SIZE / 4);
    CHECK(S.reg[VCNR(0, regJPEG_SYS_INT_EN)] & JPEG_SYS_INT_EN__DJRBC_MASK);
    CHECK(S.reg[VCNR(0, regJPEG_DEC_GFX10_ADDR_CONFIG)] == 0x545);
    CHECK(S.reg[VCNR(0, regUVD_JPEG_PITCH)] == 0xABADCAFE && S.vcn.jpeg_bad == 0);
    CHECK(jpeg40_dec_ring_test_ib(dev, v) == RDNA3_OK && S.reg[VCNR(0, regUVD_JPEG_PITCH)] == 0xDEADBEEF);
    CHECK(S.vcn.jpeg_ibs == 1 && S.vcn.jpeg_ib_vmid == 0 && S.vcn.jpeg_fences == 1 && S.vcn.jpeg_traps == 1 &&
          S.vcn.jpeg_bad == 0 && S.vcn.jpeg_ib_faults == 0);
    ih60_process(dev, &ih, vcn_iv_cb, &seen);
    CHECK(seen.jpeg == 1 && v->jpeg_irqs == 1 && seen.other == 0);

    /* jobs in VMID 0 across the ring wrap (64-dword fetch padding), a user
     * fence, the vm-flush / register packets */
    buf = rdna3_vram_alloc(dev, 8192, 4096, "vcn-job-test");
    {
        uint32_t pk[4] = { 8, 0x08000001, 0, 0 };   /* an encode op package outside a session */
        rdna3_vram_write(dev, buf, pk, sizeof(pk));
        rdna3_hdp_flush(dev);
    }
    ib.va = rdna3_vram_mc(dev, buf);
    ib.length_dw = 4;
    ib.kdata = NULL;
    for (k = 0; k < 40; k++) {
        seq = vcn40_submit(dev, v, 1, 0, &ib, 1, k == 39 ? rdna3_vram_mc(dev, buf + 4096) : 0);
        CHECK(seq && vcn40_wait_fence(dev, &v->ring_enc[1], seq) == RDNA3_OK);
    }
    CHECK(seq == v->ring_enc[1].fence_seq && S.vcn.bad == 0 && S.vcn.last_ib_inst == 1);
    {
        uint32_t uf = 0;
        rdna3_vram_read(dev, buf + 4096, &uf, 4);
        CHECK(uf == seq);
    }
    CHECK(vcn40_submit(dev, v, 2, 0, &ib, 1, 0) == 0);   /* no third instance */
    {
        rdna3_vm_flush_regs fr = { 0x100, 0x101, 0x102, 0x103, 1u << 5 };
        unsigned w0 = S.vcn.reg_writes, r0 = S.vcn.reg_waits;
        vcn40_ring_begin_use(dev, v, &v->ring_enc[0]);
        CHECK(vcn40_ring_alloc(dev, &v->ring_enc[0], 16) == RDNA3_OK);
        vcn40_enc_ring_emit_vm_flush(dev, &v->ring_enc[0], &fr, 0x123456001ull);
        vcn40_ring_commit(dev, &v->ring_enc[0]);
        vcn40_ring_end_use(dev, v, &v->ring_enc[0]);
        CHECK(S.vcn.reg_writes == w0 + 3 && S.vcn.reg_waits == r0 + 2 && S.vcn.bad == 0);
    }

    /* clock gating: VCN_MGCG leaves UVD_CGC_* to the firmware; JPEG's MGCG
     * toggles JPEG_DEC_MODE */
    {
        uint32_t cgc = S.reg[VCNR(0, regUVD_CGC_CTRL)];
        CHECK(vcn40_set_clockgating_state(dev, v, 1) == RDNA3_OK && S.reg[VCNR(0, regUVD_CGC_CTRL)] == cgc);
        CHECK(jpeg40_set_clockgating_state(dev, v, 0) == RDNA3_OK &&
              !(S.reg[VCNR(0, regJPEG_CGC_CTRL)] & JPEG_CGC_CTRL__JPEG_DEC_MODE_MASK) &&
              (S.reg[VCNR(0, regJPEG_CGC_CTRL)] & JPEG_CGC_CTRL__DYN_CLOCK_MODE_MASK));
        CHECK(vcn40_is_idle(dev, v) && jpeg40_is_idle(dev, v) && vcn40_wait_for_idle(dev, v) == RDNA3_OK &&
              jpeg40_wait_for_idle(dev, v) == RDNA3_OK);
    }

    /* idle work: nothing pending -> both blocks gated, the PMFW told */
    vcn40_ring_begin_use(dev, v, &v->ring_enc[0]);
    CHECK(vcn40_idle_work_handler(dev, v) == 0);   /* a submission in flight */
    vcn40_ring_end_use(dev, v, &v->ring_enc[0]);
    CHECK(vcn40_idle_work_handler(dev, v) == 1 && v->cur_state == RD_PG_STATE_GATE && v->vcn_gated);
    CHECK(S.vcn.vcn_down_msgs == 2 && !S.vcn.smu_on[0] && !S.vcn.smu_on[1]);
    for (i = 0; i < 2; i++)
        CHECK((S.reg[VCNR(i, regUVD_PGFSM_STATUS)] & 0x3F3FFFFF) == 0x2A2AAAAA &&
              S.reg[VCNR(i, regUVD_STATUS)] == 0 && (S.reg[VCNR(i, regUVD_VCPU_CNTL)] & UVD_VCPU_CNTL__BLK_RST_MASK));
    CHECK(jpeg40_idle_work_handler(dev, v) == 1 && v->jpeg_cur_state == RD_PG_STATE_GATE && !S.vcn.jpeg_smu_on &&
          S.vcn.jpeg_down_msgs == 1 &&
          (S.reg[VCNR(0, regUVD_PGFSM_STATUS)] & UVD_PGFSM_STATUS__UVDJ_PWR_STATUS_MASK) ==
              (2u << UVD_PGFSM_STATUS__UVDJ_PWR_STATUS__SHIFT));
    CHECK(S.vcn.regs_while_off == 0);
    /* the next job powers everything up again; the ring continues */
    seq = vcn40_submit(dev, v, 0, 0, &ib, 1, 0);
    CHECK(seq && vcn40_wait_fence(dev, &v->ring_enc[0], seq) == RDNA3_OK);
    CHECK(S.vcn.boots == 4 && S.vcn.vcn_up_msgs == 4 && v->cur_state == RD_PG_STATE_UNGATE && S.vcn.bad == 0);
    ih60_process(dev, &ih, vcn_iv_cb, &seen);
    CHECK(seen.other == 0 && v->bad_irqs == 0);
    /* a VCPU that does not boot (window 0 not on the TMR image): start gives up */
    CHECK(vcn40_idle_work_handler(dev, v) == 1 && v->cur_state == RD_PG_STATE_GATE);
    {
        uint64_t keep = v->tmr_mc[1];
        v->tmr_mc[1] = 0x1000;
        CHECK(vcn40_set_powergating_state(dev, v, RD_PG_STATE_UNGATE) == RDNA3_ETIME);
        CHECK(v->cur_state == RD_PG_STATE_GATE && S.vcn.bad_boots > 0);
        v->tmr_mc[1] = keep;
        S.vcn.booted[0] = 0;   /* the model's VCPU 0 restarts with the next start */
        CHECK(vcn40_set_powergating_state(dev, v, RD_PG_STATE_UNGATE) == RDNA3_OK);
        CHECK(vcn40_hw_fini(dev, v) == RDNA3_OK && v->cur_state == RD_PG_STATE_GATE && !v->ring_enc[0].ready);
        CHECK(jpeg40_hw_fini(dev, v) == RDNA3_OK && !v->jpeg.ready);
        /* back up for the UAPI test */
        CHECK(vcn40_hw_init(dev, v) == RDNA3_OK && jpeg40_hw_init(dev, v) == RDNA3_OK);
    }
    ih60_process(dev, &ih, vcn_iv_cb, &seen);
    ih60_disable(dev, &ih);
}

/* ---- the video IPs through uapi/rdna3_drm.c (HW_IP_INFO / COUNT,
 * VIDEO_CAPS, CS with amdgpu_cs_patch_ibs' checks) on the model ---- */

static rdna3_vcn *VV;
static uint32_t vcno_last_ip, vcno_last_ring, vcno_submits;
static int vcno_submit(void *c, void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                       const rdna3_cs_job *job, uint64_t *seq)
{
    rdna3_vcn_ib vib[8];
    uint32_t i, s = 0;
    const uint32_t vmid = 5;
    (void)c; (void)job;
    if (n > 8)
        return -RD_EINVAL;
    /* the kext: the file's VMID on both hubs; VCN walks the MMHUB */
    mmhub302_setup_vm_pt_regs(UQ.dev, vmid, gpuvm_pd_addr(UQ.dev, &((struct uq_vm *)vm)->vm));
    for (i = 0; i < n; i++) {
        vib[i].va = ibs[i].va;
        vib[i].length_dw = ibs[i].length_dw;
        vib[i].kdata = ibs[i].kdata;
    }
    vcno_last_ip = ip;
    vcno_last_ring = ring;
    vcno_submits++;
    if (ip == AMDGPU_HW_IP_VCN_ENC)
        s = vcn40_submit(UQ.dev, VV, ring, vmid, vib, n, 0);
    else if (ip == AMDGPU_HW_IP_VCN_JPEG)
        s = jpeg40_submit(UQ.dev, VV, vmid, vib, n, 0);
    if (!s)
        return -RD_EIO;
    *seq = s;
    return 0;
}
static int vcno_signaled(void *c, uint32_t ip, uint32_t ring, uint64_t seq)
{
    (void)c;
    if (ip == AMDGPU_HW_IP_VCN_ENC && ring < 2)
        return vcn40_fence_passed(UQ.dev, &VV->ring_enc[ring], (uint32_t)seq);
    if (ip == AMDGPU_HW_IP_VCN_JPEG)
        return vcn40_fence_passed(UQ.dev, &VV->jpeg, (uint32_t)seq);
    return 1;
}
static int vcno_wait(void *c, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t abs_ns)
{
    (void)abs_ns;
    return vcno_signaled(c, ip, ring, seq) ? 0 : -RD_ETIME;
}

/* an encode IB the way radeonsi lays it out: checksum, engine info,
 * session info (message buffer), session init (the standard), op */
static uint32_t vcn_enc_ib(uint32_t *ib, uint64_t msg_va, uint32_t standard, uint32_t op)
{
    uint32_t n = 0, slot, i, sum = 0, pack;
    ib[n++] = 0x10; ib[n++] = 0x30000002; slot = n++; ib[n++] = 0;
    ib[n++] = 0x10; ib[n++] = 0x30000001; ib[n++] = 2; ib[n++] = 0;
    ib[n++] = 0x18; ib[n++] = 0x1; ib[n++] = 7; ib[n++] = (uint32_t)(msg_va >> 32); ib[n++] = (uint32_t)msg_va; ib[n++] = 0;
    ib[n++] = 0x10; ib[n++] = 0x3; ib[n++] = standard; ib[n++] = 0;
    ib[n++] = 0x8; ib[n++] = op;
    pack = n - 4;
    ib[slot + 1] = pack;
    ib[7] = pack * 4;
    for (i = 0; i < pack; i++)
        sum += ib[4 + i];
    ib[slot] = sum;
    return n;
}

/* a decode IB: checksum, engine info, the decode buffer naming 'msg_va' */
static uint32_t vcn_dec_ib(uint32_t *ib, uint64_t msg_va)
{
    uint32_t n = 0, slot, i, sum = 0, pack = RD_VCN_DECODE_BUFFER_DW + 4 + 2;
    ib[n++] = 0x10; ib[n++] = 0x30000002; slot = n++; ib[n++] = pack;
    ib[n++] = 0x10; ib[n++] = 0x30000001; ib[n++] = 3; ib[n++] = pack * 4;
    ib[n++] = RD_VCN_DECODE_BUFFER_DW * 4 + 8; ib[n++] = RD_VCN_IB_FLAG_DECODE_BUFFER;
    for (i = 0; i < RD_VCN_DECODE_BUFFER_DW; i++)
        ib[n + i] = 0;
    ib[n] = RD_VCN_CMD_FLAG_MSG_BUFFER;
    ib[n + 1] = (uint32_t)(msg_va >> 32);
    ib[n + 2] = (uint32_t)msg_va;
    n += RD_VCN_DECODE_BUFFER_DW;
    for (i = 0; i < pack; i++)
        sum += ib[4 + i];
    ib[slot] = sum;
    return n;
}

/* a decode create message (amdgpu_vcn_dec_get_create_msg's layout) for 'codec' */
static void vcn_dec_create_msg(uint32_t *msg, uint32_t codec, uint32_t len)
{
    memset(msg, 0, 64);
    msg[0] = 0x28; msg[1] = len; msg[2] = 1; msg[3] = 0; msg[4] = 1; msg[5] = 0;
    msg[6] = 1; msg[7] = 0x28; msg[8] = 0x10; msg[9] = 0;
    msg[10] = codec; msg[12] = 0x780; msg[13] = 0x440;
}

static int vcn_cs(rdna3_drm_file *f, uint32_t ctx, uint32_t ip, uint32_t ring, uint64_t va, uint32_t ndw,
                  uint64_t *handle)
{
    struct drm_amdgpu_cs_chunk_ib cib;
    struct drm_amdgpu_cs_chunk ch;
    uint64_t chp;
    union drm_amdgpu_cs cs;
    int r;
    memset(&cib, 0, sizeof(cib));
    cib.ip_type = ip;
    cib.ring = ring;
    cib.va_start = va;
    cib.ib_bytes = ndw * 4;
    ch.chunk_id = AMDGPU_CHUNK_ID_IB;
    ch.length_dw = sizeof(cib) / 4;
    ch.chunk_data = (uint64_t)(uintptr_t)&cib;
    chp = (uint64_t)(uintptr_t)&ch;
    memset(&cs, 0, sizeof(cs));
    cs.in.ctx_id = ctx;
    cs.in.num_chunks = 1;
    cs.in.chunks = (uint64_t)(uintptr_t)&chp;
    r = UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CS), cs);
    if (!r && handle)
        *handle = cs.out.handle;
    return r;
}

static int vcn_info(rdna3_drm_file *f, uint32_t query, uint32_t type, void *out, uint32_t size)
{
    struct drm_amdgpu_info in;
    memset(&in, 0, sizeof(in));
    in.return_pointer = (uint64_t)(uintptr_t)out;
    in.return_size = size;
    in.query = query;
    if (query == AMDGPU_INFO_VIDEO_CAPS)
        in.video_cap.type = type;
    else
        in.query_hw_ip.type = type;
    return UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in);
}

static int vcn_wait_cs(rdna3_drm_file *f, uint32_t ctx, uint32_t ip, uint64_t h, uint64_t *status)
{
    union drm_amdgpu_wait_cs w;
    int r;
    memset(&w, 0, sizeof(w));
    w.in.handle = h;
    w.in.ip_type = ip;
    w.in.ctx_id = ctx;
    w.in.timeout = ~0ull;
    r = UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w);
    *status = w.out.status;
    return r;
}

static uint32_t vcn_ctx(rdna3_drm_file *f)
{
    union drm_amdgpu_ctx c;
    memset(&c, 0, sizeof(c));
    c.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    return UQ_IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) ? 0 : c.out.alloc.ctx_id;
}

static void test_vcn_uapi(rdna3_dev *dev, rdna3_gmc *gmc, rdna3_vcn *v)
{
    static rdna3_drm_device dd;
    static rdna3_drm_file f1;
    rdna3_drm_ops ops;
    amd_discovery disc;
    const uint64_t va = 0x0000400000000000ull;
    struct drm_amdgpu_info_hw_ip ip;
    struct drm_amdgpu_info_video_caps caps;
    uint32_t n, hib, hmsg, hj, ib[96], c1, c2, c3, c4, t;
    uint8_t *ibp, *msgp, *jp;
    uint64_t h, st;
    unsigned init0, dec0;

    memset(&UQ, 0, sizeof(UQ));
    UQ.dev = dev;
    UQ.gmc = gmc;
    UQ.heap = rdna3_vram_alloc(dev, 1u << 20, 65536, "vcn-uapi-heap");
    UQ.heap_next = UQ.heap;
    UQ.heap_end = UQ.heap + (1u << 20);
    CHECK(UQ.heap != UINT64_MAX && gpuvm_pool_init(dev, &UQ.pool, 64) == RDNA3_OK);
    VV = v;
    memset(&ops, 0, sizeof(ops));
    ops.zalloc = uqo_zalloc; ops.free = uqo_free; ops.bo_alloc = uqo_bo_alloc; ops.bo_free = uqo_bo_free;
    ops.copyin = uqo_copyin; ops.copyout = uqo_copyout;
    ops.vm_create = uqo_vm_create; ops.vm_destroy = uqo_vm_destroy; ops.vm_map = uqo_vm_map; ops.vm_unmap = uqo_vm_unmap;
    ops.bo_kmap = uqo_bo_kmap;
    ops.submit = vcno_submit; ops.fence_signaled = vcno_signaled; ops.fence_wait = vcno_wait;
    make_discovery(&disc);
    rdna3_drm_device_init(&dd, &ops, &disc, 0x747E, 0xC8, 0, 16ull << 30, 256ull << 20, 8ull << 30);
    CHECK(rdna3_drm_file_open(&dd, &f1) == 0);

    /* without the video block (rdna3vcn=0) nothing changes */
    for (t = AMDGPU_HW_IP_UVD; t <= AMDGPU_HW_IP_VCN_JPEG; t++) {
        memset(&ip, 0xff, sizeof(ip));
        CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_INFO, t, &ip, sizeof(ip)) == 0);
        CHECK(ip.available_rings == 0 && ip.hw_ip_version_major == 0 && ip.ip_discovery_version == 0 &&
              ip.ib_start_alignment == 32 && ip.ib_size_alignment == 32);
        n = 7;
        CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_COUNT, t, &n, 4) == 0 && n == 0);
    }
    memset(&caps, 0xff, sizeof(caps));
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, AMDGPU_INFO_VIDEO_CAPS_DECODE, &caps, sizeof(caps)) == 0);
    for (t = 0; t < AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_COUNT; t++)
        CHECK(caps.codec_info[t].valid == 0 && caps.codec_info[t].max_width == 0);
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, 7, &caps, sizeof(caps)) == 0);
    CHECK(vcn_cs(&f1, 0, AMDGPU_HW_IP_VCN_ENC, 0, va, 16, NULL) == -RD_EINVAL);

    /* the block is up: amdgpu_kms.c's answers */
    rdna3_drm_device_set_vcn(&dd, AMD_IP_VERSION(4, 0, 0), 2, 0, 3, 1);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_VCN_DEC, &ip, sizeof(ip)) == 0);
    CHECK(ip.hw_ip_version_major == 4 && ip.hw_ip_version_minor == 0 && ip.ip_discovery_version == 0x040000 &&
          ip.available_rings == 0 && ip.ib_start_alignment == 256 && ip.ib_size_alignment == 64);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_VCN_ENC, &ip, sizeof(ip)) == 0);
    CHECK(ip.hw_ip_version_major == 4 && ip.ip_discovery_version == 0x040000 && ip.available_rings == 1 &&
          ip.ib_start_alignment == 256 && ip.ib_size_alignment == 4);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_VCN_JPEG, &ip, sizeof(ip)) == 0);
    CHECK(ip.hw_ip_version_major == 4 && ip.ip_discovery_version == 0x040000 && ip.available_rings == 1 &&
          ip.ib_start_alignment == 256 && ip.ib_size_alignment == 64);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_UVD, &ip, sizeof(ip)) == 0 && ip.available_rings == 0 &&
          ip.hw_ip_version_major == 0);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_COUNT, AMDGPU_HW_IP_VCN_DEC, &n, 4) == 0 && n == 2);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_COUNT, AMDGPU_HW_IP_VCN_ENC, &n, 4) == 0 && n == 2);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_COUNT, AMDGPU_HW_IP_VCN_JPEG, &n, 4) == 0 && n == 1);
    CHECK(vcn_info(&f1, AMDGPU_INFO_HW_IP_COUNT, AMDGPU_HW_IP_UVD, &n, 4) == 0 && n == 0);
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, AMDGPU_INFO_VIDEO_CAPS_DECODE, &caps, sizeof(caps)) == 0);
    CHECK(caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC].valid &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC].max_width == 4096 &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC].max_level == 52);
    CHECK(caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC].max_pixels_per_frame == 8192u * 4352u &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC].max_level == 186);
    CHECK(caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_JPEG].max_width == 16384 &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VP9].valid &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1].valid &&
          !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG2].valid &&
          !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VC1].valid);
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, AMDGPU_INFO_VIDEO_CAPS_ENCODE, &caps, sizeof(caps)) == 0);
    CHECK(caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC].valid &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC].valid &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1].valid &&
          !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_JPEG].valid &&
          !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VP9].valid &&
          caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC].max_level == 0);
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, 7, &caps, sizeof(caps)) == -RD_EINVAL);
    /* VCN0 harvested: VCN1's tables (no AV1 encode or decode) */
    dd.vcn_harvest = 1;
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, AMDGPU_INFO_VIDEO_CAPS_DECODE, &caps, sizeof(caps)) == 0 &&
          !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1].valid);
    dd.vcn_harvest = 3;
    CHECK(vcn_info(&f1, AMDGPU_INFO_VIDEO_CAPS, AMDGPU_INFO_VIDEO_CAPS_DECODE, &caps, sizeof(caps)) == -RD_EINVAL);
    dd.vcn_harvest = 0;

    /* the client's IB and message BOs */
    hib = uq_gem(&f1, 8192, AMDGPU_GEM_DOMAIN_VRAM);
    hmsg = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    hj = uq_gem(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM);
    CHECK(hib && hmsg && hj && uq_va(&f1, hib, va, 8192) == 0 && uq_va(&f1, hmsg, va + 0x10000, 4096) == 0 &&
          uq_va(&f1, hj, va + 0x20000, 4096) == 0);
    ibp = S.vram + rdna3_drm_bo_lookup(&f1, hib)->vram_offset;
    msgp = S.vram + rdna3_drm_bo_lookup(&f1, hmsg)->vram_offset;
    jp = S.vram + rdna3_drm_bo_lookup(&f1, hj)->vram_offset;

    /* a context's first job: the entity goes to VCN1 (VCN0 starts with a
     * score of one), an H264 session stays there, fences by context */
    c1 = vcn_ctx(&f1);
    init0 = S.vcn.enc_init;
    n = vcn_enc_ib(ib, va + 0x10000, 1, 0x08000001);
    memcpy(ibp, ib, 4ull * n);
    CHECK(vcn_cs(&f1, c1, AMDGPU_HW_IP_VCN_ENC, 1, va, n, NULL) == -RD_EINVAL);   /* one ring only */
    CHECK(vcn_cs(&f1, c1, AMDGPU_HW_IP_VCN_ENC, 0, va, n, &h) == 0 && vcno_last_ring == 1 && h);
    CHECK(S.vcn.enc_init == init0 + 1 && S.vcn.last_ib_inst == 1 && S.vcn.last_ib_vmid == 5 &&
          S.vcn.checksum_bad == 0 && S.vcn.ib_faults == 0);
    CHECK(vcn_wait_cs(&f1, c1, AMDGPU_HW_IP_VCN_ENC, h, &st) == 0 && st == 0);
    CHECK(f1.vcn_entity[c1] == 2 && f1.vcn_entity_jobs[c1] == 1);
    /* AV1 encode in a context's first job: limit_sched moves it to VCN0 */
    c2 = vcn_ctx(&f1);
    n = vcn_enc_ib(ib, va + 0x10000, 2, 0x08000001);
    memcpy(ibp, ib, 4ull * n);
    CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_ENC, 0, va, n, &h) == 0 && vcno_last_ring == 0 && f1.vcn_entity[c2] == 1);
    CHECK(S.vcn.last_ib_inst == 0 && vcn_wait_cs(&f1, c2, AMDGPU_HW_IP_VCN_ENC, h, &st) == 0 && st == 0);
    CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_ENC, 0, va, n, &h) == 0 && vcno_last_ring == 0);   /* VCN0 takes anything */
    /* ... but not once the VCN1 entity ran a job */
    n = vcn_enc_ib(ib, va + 0x10000, 2, 0x08000001);
    memcpy(ibp, ib, 4ull * n);
    CHECK(vcn_cs(&f1, c1, AMDGPU_HW_IP_VCN_ENC, 0, va, n, NULL) == -RD_EINVAL && f1.vcn_entity[c1] == 2);
    /* decode: H264 stays on VCN1, AV1 (0x13) goes to VCN0 */
    c3 = vcn_ctx(&f1);
    dec0 = S.vcn.dec_create;
    vcn_dec_create_msg((uint32_t *)msgp, 0x7, 0x38);
    n = vcn_dec_ib(ib, va + 0x10000);
    memcpy(ibp, ib, 4ull * n);
    CHECK(vcn_cs(&f1, c3, AMDGPU_HW_IP_VCN_ENC, 0, va, n, &h) == 0 && vcno_last_ring == 1 &&
          S.vcn.dec_create == dec0 + 1);
    c4 = vcn_ctx(&f1);
    vcn_dec_create_msg((uint32_t *)msgp, 0x13, 0x38);
    CHECK(vcn_cs(&f1, c4, AMDGPU_HW_IP_VCN_ENC, 0, va, n, &h) == 0 && vcno_last_ring == 0 &&
          S.vcn.dec_create == dec0 + 2 && f1.vcn_entity[c4] == 1);
    /* vcn_v4_0_dec_msg's checks (on a new VCN1 entity) */
    {
        uint32_t c5 = vcn_ctx(&f1), c6 = vcn_ctx(&f1), c7 = vcn_ctx(&f1);
        vcn_dec_create_msg((uint32_t *)msgp, 0x13, 0x2000);   /* longer than its BO */
        CHECK(vcn_cs(&f1, c5, AMDGPU_HW_IP_VCN_ENC, 0, va, n, NULL) == -RD_EINVAL);
        vcn_dec_create_msg((uint32_t *)msgp, 0x13, 0x38);
        n = vcn_dec_ib(ib, va + 0x10004);                      /* not 8-byte aligned */
        memcpy(ibp, ib, 4ull * n);
        CHECK(vcn_cs(&f1, c6, AMDGPU_HW_IP_VCN_ENC, 0, va, n, NULL) == -RD_EINVAL);
        n = vcn_dec_ib(ib, va + 0x50000);                      /* not mapped */
        memcpy(ibp, ib, 4ull * n);
        CHECK(vcn_cs(&f1, c7, AMDGPU_HW_IP_VCN_ENC, 0, va, n, NULL) == -RD_EINVAL);
    }
    /* amdgpu_cs_patch_ibs: the IB inside one mapping */
    CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_ENC, 0, va + 0x40000, 16, NULL) == -RD_EINVAL);
    CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_ENC, 0, va + 8192 - 16, 8, NULL) == -RD_EINVAL);
    /* a freed context's entity is gone with it */
    {
        union drm_amdgpu_ctx c;
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_FREE_CTX;
        c.in.ctx_id = c1;
        CHECK(UQ_IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 && f1.vcn_entity[c1] == 0 &&
              f1.vcn_entity_jobs[c1] == 0);
    }

    /* JPEG: the checked copy runs (pitch register), bad packets are refused */
    {
        uint32_t jib[16], i, before = S.vcn.jpeg_ibs;
        jib[0] = PACKETJ(regUVD_JPEG_PITCH_INTERNAL_OFFSET, 0, 0, PACKETJ_TYPE0);
        jib[1] = 0x1234ABCD;
        for (i = 2; i < 16; i += 2) {
            jib[i] = PACKETJ(0, 0, 0, PACKETJ_TYPE6);
            jib[i + 1] = 0;
        }
        memcpy(jp, jib, sizeof(jib));
        CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_JPEG, 0, va + 0x20000, 16, &h) == 0 &&
              vcno_last_ip == AMDGPU_HW_IP_VCN_JPEG);
        CHECK(vcn_wait_cs(&f1, c2, AMDGPU_HW_IP_VCN_JPEG, h, &st) == 0 && st == 0);
        CHECK(S.reg[VCNR(0, regUVD_JPEG_PITCH)] == 0x1234ABCD && S.vcn.jpeg_ibs == before + 1 &&
              S.vcn.jpeg_ib_vmid == 0 && S.vcn.jpeg_vmid == 5 && S.vcn.jpeg_bad == 0);
        jib[2] = PACKETJ(0, 0, 0, PACKETJ_TYPE7);              /* a trap from userspace */
        memcpy(jp, jib, sizeof(jib));
        CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_JPEG, 0, va + 0x20000, 16, NULL) == -RD_EINVAL);
        jib[2] = PACKETJ(0x1000, 0, 0, PACKETJ_TYPE0);         /* outside the JPEG registers */
        memcpy(jp, jib, sizeof(jib));
        CHECK(vcn_cs(&f1, c2, AMDGPU_HW_IP_VCN_JPEG, 0, va + 0x20000, 16, NULL) == -RD_EINVAL);
        CHECK(S.vcn.jpeg_ibs == before + 1);
    }
    rdna3_drm_file_close(&f1);
    rdna3_drm_device_fini(&dd);
}

/* ---- VCN power loss, DPG (rdna3vcn=2/3), suspend / resume ---- */

/* the card loses its power (an S3 that cuts the slot, a mode-1 reset): the
 * video block's registers back to their reset values, the PMFW's power
 * state gone; with 'vram' the contents of its VRAM blocks too */
static void sim_vcn_power_loss(const rdna3_vcn *v, int vram)
{
    static const uint32_t regs[] = {
        regUVD_POWER_STATUS, regUVD_DPG_PAUSE, regUVD_STATUS, regVCN_RB_ENABLE, regUVD_RB_BASE_LO,
        regUVD_RB_BASE_HI, regUVD_RB_SIZE, regUVD_RB_RPTR, regUVD_RB_WPTR, regUVD_MASTINT_EN, regVCN_RB1_DB_CTRL,
        regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, regUVD_VCPU_CACHE_SIZE0,
        regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW, regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW, regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW,
        regUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH, regUVD_GFX10_ADDR_CONFIG, regUVD_LMI_CTRL, regUVD_LMI_CTRL2,
        regUVD_DPG_LMA_CTL, regUVD_DPG_LMA_DATA, regUVD_JRBC_RB_CNTL, regUVD_JRBC_RB_SIZE, regUVD_JRBC_RB_RPTR,
        regUVD_JRBC_RB_WPTR, regJPEG_SYS_INT_EN, regVCN_JPEG_DB_CTRL, regUVD_JPEG_PITCH,
    };
    uint32_t i, k;
    for (i = 0; i < 2; i++) {
        for (k = 0; k < sizeof(regs) / sizeof(regs[0]); k++)
            S.reg[VCNR(i, regs[k])] = 0;
        S.reg[VCNR(i, regUVD_PGFSM_STATUS)] = 0x2AAAAAAAu;            /* every tile off */
        S.reg[VCNR(i, regUVD_VCPU_CNTL)] = UVD_VCPU_CNTL__BLK_RST_MASK;
        S.vcn.booted[i] = 0;
        S.vcn.smu_on[i] = 0;
        S.vcn.rptr[i] = 0;
    }
    S.vcn.jpeg_smu_on = 0;
    S.vcn.jrptr = 0;
    if (!vram)
        return;
    for (i = 0; i < 2; i++) {
        memset(S.vram + v->vcpu_bo[i], 0xA5, vcn40_vcpu_bo_size());
        memset(S.vram + v->ring_enc[i].ring, 0xA5, v->ring_enc[i].ring_size);
        memset(S.vram + v->ring_enc[i].wb, 0xA5, 4096);
        if (v->dpg_sram[i])
            memset(S.vram + v->dpg_sram[i], 0xA5, RD_VCN_DPG_SRAM_SIZE);
    }
    memset(S.vram + v->jpeg.ring, 0xA5, v->jpeg.ring_size);
    memset(S.vram + v->jpeg.wb, 0xA5, 4096);
}

/* a system sleep that cuts the card's power, as the kext runs it
 * (setPowerState(0) -> vcnSuspend: amdgpu_device_ip_suspend's reverse IP
 * order, JPEG then VCN; the wake -> hwInit -> vcnStart: early init, then
 * vcn_v4_0_resume and jpeg_v4_0_resume): the VCPU BO comes back from the
 * copy, the rings run on, their fences continue */
static void vcn_s3_cycle(rdna3_dev *dev, rdna3_vcn *v, const amd_fw_blob_ref *blobs)
{
    static uint8_t save[2][RD_VCN_STACK_SIZE + RD_VCN_CONTEXT_SIZE + RD_VCN_FW_SHARED_SIZE];
    uint32_t i, seq0[2], jseq0, mark, flags, sus0 = v->suspends, res0 = v->resumes, rb0 = v->restored_bos;
    unsigned bb0 = S.vcn.bad_boots;
    uint8_t fs[RD_VCN4_FWS_SIZE];
    const uint32_t fws_off = RD_VCN_STACK_SIZE + RD_VCN_CONTEXT_SIZE;

    CHECK(vcn40_vcpu_bo_size() == sizeof(save[0]));
    for (i = 0; i < 2; i++) {
        CHECK(vcn40_unified_ring_test_ib(dev, v, &v->ring_enc[i]) == RDNA3_OK);
        seq0[i] = v->ring_enc[i].fence_seq;
        /* what the firmware keeps in its stack and context */
        mark = 0x5A0C0000u | i;
        rdna3_vram_write(dev, v->vcpu_bo[i] + 256, &mark, 4);
        rdna3_vram_write(dev, v->vcpu_bo[i] + RD_VCN_STACK_SIZE + 512, &mark, 4);
    }
    CHECK(jpeg40_dec_ring_test_ib(dev, v) == RDNA3_OK);
    jseq0 = v->jpeg.fence_seq;

    /* suspend: JPEG, then VCN; without the OS's buffers amdgpu_vcn_suspend fails */
    CHECK(jpeg40_suspend(dev, v) == RDNA3_OK && !v->jpeg.ready && v->jpeg_cur_state == RD_PG_STATE_GATE);
    CHECK(vcn40_suspend(dev, v) == RDNA3_ENOMEM && v->cur_state == RD_PG_STATE_GATE);
    v->saved_bo[0] = save[0];
    v->saved_bo[1] = save[1];
    CHECK(vcn40_suspend(dev, v) == RDNA3_OK && v->suspends == sus0 + 1);
    CHECK(v->saved_bo_valid[0] && v->saved_bo_valid[1] && !v->ring_enc[0].ready && !v->ring_enc[1].ready);
    CHECK(!S.vcn.smu_on[0] && !S.vcn.smu_on[1] && !S.vcn.jpeg_smu_on && S.vcn.down_in_dpg == 0);
    for (i = 0; i < 2; i++) {
        memcpy(&flags, save[i] + fws_off, 4);
        memcpy(&mark, save[i] + 256, 4);
        CHECK(flags == (RD_FW_SHARED_FLAG_0_UNIFIED_QUEUE | RD_VCN_SMU_DPM_INTERFACE_FLAG) &&
              save[i][fws_off + RD_VCN4_FWS_SQ_IS_ENABLED] == 1 && mark == (0x5A0C0000u | i));
        CHECK(save[i][fws_off + RD_VCN4_FWS_SQ_QUEUE_MODE] & RD_FW_QUEUE_DPG_HOLD_OFF);   /* vcn_v4_0_stop */
    }

    /* the power goes: registers, PMFW state and VRAM contents */
    sim_vcn_power_loss(v, 1);

    /* the wake: early init (the sw state kept), VCN, then JPEG */
    CHECK(vcn40_early_init(dev, v, blobs[AMD_BLOB_VCN].data, blobs[AMD_BLOB_VCN].len, 0x545, 1, v->pg_mode) ==
          RDNA3_OK);
    CHECK(v->sw_ready && v->cur_state == RD_PG_STATE_GATE && v->vcn_gated);
    CHECK(vcn40_resume(dev, v) == RDNA3_OK && v->resumes == res0 + 1 && v->restored_bos == rb0 + 2);
    CHECK(!v->saved_bo_valid[0] && !v->saved_bo_valid[1] && v->ring_enc[0].ready && v->ring_enc[1].ready);
    for (i = 0; i < 2; i++) {
        rdna3_vram_read(dev, v->fw_shared[i], fs, sizeof(fs));
        memcpy(&flags, fs, 4);
        CHECK(flags == (RD_FW_SHARED_FLAG_0_UNIFIED_QUEUE | RD_VCN_SMU_DPM_INTERFACE_FLAG) &&
              fs[RD_VCN4_FWS_SQ_IS_ENABLED] == 1 && fs[RD_VCN4_FWS_SQ_QUEUE_MODE] == 0);
        rdna3_vram_read(dev, v->vcpu_bo[i] + RD_VCN_STACK_SIZE + 512, &mark, 4);
        CHECK(mark == (0x5A0C0000u | i));
        CHECK(vcn40_fence_passed(dev, &v->ring_enc[i], seq0[i]));
    }
    CHECK(S.vcn.bad_fw_shared == 0 && S.vcn.regs_while_off == 0);
    CHECK(jpeg40_resume(dev, v) == RDNA3_OK && v->jpeg.ready && vcn40_fence_passed(dev, &v->jpeg, jseq0));
    for (i = 0; i < 2; i++) {
        CHECK(vcn40_unified_ring_test_ib(dev, v, &v->ring_enc[i]) == RDNA3_OK);
        CHECK(v->ring_enc[i].fence_seq > seq0[i]);
    }
    CHECK(jpeg40_dec_ring_test_ib(dev, v) == RDNA3_OK && v->jpeg.fence_seq == jseq0 + 1);
    CHECK(S.vcn.bad == 0 && S.vcn.jpeg_bad == 0 && S.vcn.bad_boots == bb0 && S.vcn.static_pg_in_dpg == 0);

    /* a GPU reset whose suspend never ran: no copy, the BO cleared and
     * fw_shared set up again, the rings run */
    CHECK(jpeg40_hw_fini(dev, v) == RDNA3_OK && vcn40_hw_fini(dev, v) == RDNA3_OK);
    sim_vcn_power_loss(v, 1);
    CHECK(vcn40_early_init(dev, v, blobs[AMD_BLOB_VCN].data, blobs[AMD_BLOB_VCN].len, 0x545, 1, v->pg_mode) ==
          RDNA3_OK);
    CHECK(vcn40_resume(dev, v) == RDNA3_OK && v->restored_bos == rb0 + 2);
    rdna3_vram_read(dev, v->vcpu_bo[1] + RD_VCN_STACK_SIZE + 512, &mark, 4);
    CHECK(mark == 0);
    CHECK(jpeg40_resume(dev, v) == RDNA3_OK);
    for (i = 0; i < 2; i++)
        CHECK(vcn40_unified_ring_test_ib(dev, v, &v->ring_enc[i]) == RDNA3_OK);
    CHECK(S.vcn.bad_fw_shared == 0 && S.vcn.bad == 0);
    v->saved_bo[0] = v->saved_bo[1] = NULL;
}

/* rdna3vcn=2 / =3: the driver reloaded with dynamic power gating as amdgpu
 * runs it on this card (soc21: VCN | VCN_DPG | JPEG), the DPG pause
 * handshake (VCN 3's from begin_use / the idle work; the VCN 4 firmware
 * pauses itself), the LMA path without the PSP, suspend / resume */
static void test_vcn_dpg(rdna3_dev *dev, rdna3_psp *psp, const amd_fw_blob_ref *blobs, rdna3_vcn *v)
{
    uint64_t tmr[2] = { v->tmr_mc[0], v->tmr_mc[1] }, fws0 = v->fw_shared[0], buf, vcpu_mc;
    uint32_t i, flags = 1, seq, pk[4] = { 8, 0x08000001, 0, 0 };
    uint32_t fw_size0 = (((const struct common_firmware_header *)blobs[AMD_BLOB_VCN].data)->ucode_size_bytes + 8 +
                         4095u) & ~4095u;
    unsigned boots0, loads0, lma0, pauses0, bb0 = S.vcn.bad_boots;
    uint8_t en = 1;
    rdna3_vcn_ib ib;

    /* driver stop (amdgpu_device_fini: hw_fini, then sw_fini, JPEG first) */
    CHECK(jpeg40_hw_fini(dev, v) == RDNA3_OK && vcn40_hw_fini(dev, v) == RDNA3_OK);
    CHECK(v->cur_state == RD_PG_STATE_GATE && v->jpeg_cur_state == RD_PG_STATE_GATE);
    CHECK(jpeg40_sw_fini(dev, v) == RDNA3_OK && vcn40_sw_fini(dev, v) == RDNA3_OK && !v->sw_ready);
    rdna3_vram_read(dev, fws0, &flags, 4);
    rdna3_vram_read(dev, fws0 + RD_VCN4_FWS_SQ_IS_ENABLED, &en, 1);
    CHECK(flags == 0 && en == 0);   /* present_flag_0 and sq.is_enabled cleared */
    sim_vcn_power_loss(v, 0);

    /* rdna3vcn=2: the flags as soc21 sets them, indirect SRAM through the PSP */
    CHECK(vcn40_early_init(dev, v, blobs[AMD_BLOB_VCN].data, blobs[AMD_BLOB_VCN].len, 0x545, 1, RD_VCN_PG_DPG) ==
          RDNA3_OK);
    CHECK(v->pg_flags == (RD_PG_VCN | RD_PG_VCN_DPG | RD_PG_JPEG) && v->using_unified_queue && !v->ras_supported);
    v->psp = psp;
    CHECK(jpeg40_early_init(dev, v) == RDNA3_OK);
    CHECK(vcn40_sw_init(dev, v, tmr) == RDNA3_OK && jpeg40_sw_init(dev, v) == RDNA3_OK);
    CHECK(v->indirect_sram && v->dpg_sram[0] && v->dpg_sram[1] && !(v->dpg_sram[0] & 4095) &&
          !(v->dpg_sram[1] & 4095));
    boots0 = S.vcn.boots;
    CHECK(vcn40_hw_init(dev, v) == RDNA3_OK);
    CHECK(v->ring_enc[0].ready && v->ring_enc[1].ready && v->cur_state == RD_PG_STATE_UNGATE);
    CHECK(S.vcn.sram_loads[0] == 1 && S.vcn.sram_loads[1] == 1 && S.vcn.sram_bad == 0 && S.vcn.lma_writes == 0);
    CHECK(v->sram_uploads == 2 && v->sram_upload_errs == 0 && v->dpg_starts == 2 && v->last_err == 0);
    CHECK(S.vcn.boots == boots0 + 2 && S.vcn.bad_boots == bb0 && S.vcn.bad_fw_shared == 0 &&
          S.vcn.regs_while_off == 0 && S.vcn.static_pg_in_dpg == 0);
    vcpu_mc = rdna3_vram_mc(dev, v->vcpu_bo[1]);
    for (i = 0; i < 2; i++) {
        uint8_t fs[RD_VCN4_FWS_SIZE];
        /* the image: 7 + mc_resume's 17 + 3 writes, VCPU_CNTL (reset) first */
        CHECK(S.vcn.sram_n[i] == 27 && v->dpg_sram_curr[i] == 54);
        CHECK(S.vcn.sram[i][0][0] == regUVD_VCPU_CNTL - 0xC0 &&
              S.vcn.sram[i][0][1] == (0xFFu << UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT | UVD_VCPU_CNTL__CLK_EN_MASK |
                                      UVD_VCPU_CNTL__BLK_RST_MASK));
        CHECK(S.reg[VCNR(i, regUVD_VCPU_CNTL)] == (0xFFu << UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT |
                                                    UVD_VCPU_CNTL__CLK_EN_MASK));
        CHECK(S.reg[VCNR(i, regUVD_LMI_CTRL)] ==
              (UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK | UVD_LMI_CTRL__REQ_MODE_MASK | UVD_LMI_CTRL__CRC_RESET_MASK |
               UVD_LMI_CTRL__MASK_MC_URGENT_MASK | UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
               UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK | (8u << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT) | 0x00100000u));
        CHECK(S.reg[VCNR(i, regUVD_LMI_CTRL2)] == 0x1fu << UVD_LMI_CTRL2__RE_OFLD_MIF_WR_REQ_NUM__SHIFT);
        /* windows 0/1 filled in by the PSP, 2 and the non-cached one as written */
        CHECK(S.reg[VCNR(i, regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW)] == (uint32_t)tmr[i] &&
              S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE0)] == fw_size0);
        CHECK(S.reg[VCNR(i, regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW)] == (uint32_t)(tmr[i] + VCN_TMR_STACK_OFF) &&
              S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE1)] == RD_VCN_STACK_SIZE);
        CHECK(S.reg[VCNR(i, regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW)] == (uint32_t)rdna3_vram_mc(dev, v->fw_shared[i]) &&
              S.reg[VCNR(i, regUVD_VCPU_NONCACHE_SIZE0)] == 4096 && S.reg[VCNR(i, regUVD_VCPU_CACHE_SIZE2)] ==
              RD_VCN_CONTEXT_SIZE);
        CHECK(S.reg[VCNR(i, regUVD_GFX10_ADDR_CONFIG)] == 0x545 &&
              (S.reg[VCNR(i, regUVD_MASTINT_EN)] & UVD_MASTINT_EN__VCPU_EN_MASK));
        CHECK((S.reg[VCNR(i, regUVD_POWER_STATUS)] & (UVD_POWER_STATUS__UVD_PG_MODE_MASK |
                                                      UVD_POWER_STATUS__UVD_PG_EN_MASK |
                                                      UVD_POWER_STATUS__UVD_POWER_STATUS_MASK)) ==
              (UVD_POWER_STATUS__UVD_PG_MODE_MASK | UVD_POWER_STATUS__UVD_PG_EN_MASK | 1));
        CHECK(S.reg[VCNR(i, regVCN_RB1_DB_CTRL)] == (v->ring_enc[i].doorbell_index << 2 | VCN_RB1_DB_CTRL__EN_MASK) &&
              S.reg[VCNR(i, regUVD_RB_BASE_LO)] == (uint32_t)v->ring_enc[i].gpu_addr);
        CHECK((S.reg[VCNR(i, regUVD_PGFSM_STATUS)] & 0x3F3FFFFF) == 0x2A2AAAAA);   /* no static PG */
        rdna3_vram_read(dev, v->fw_shared[i], fs, sizeof(fs));
        CHECK(fs[RD_VCN4_FWS_SQ_QUEUE_MODE] == 0);
    }
    CHECK(S.reg[VCNR(1, regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW)] == (uint32_t)(vcpu_mc + RD_VCN_STACK_SIZE));

    /* the IB tests under DPG; JPEG (static UVDJ, jpeg_v4_0_start unchanged) beside it */
    for (i = 0; i < 2; i++)
        CHECK(vcn40_unified_ring_test_ib(dev, v, &v->ring_enc[i]) == RDNA3_OK);
    CHECK(jpeg40_hw_init(dev, v) == RDNA3_OK && jpeg40_dec_ring_test_ib(dev, v) == RDNA3_OK);
    CHECK(S.vcn.static_pg_in_dpg == 0 && S.vcn.bad == 0 && S.vcn.jpeg_bad == 0 && S.vcn.pause_reqs == 0);

    /* idle: vcn_v4_0_stop_dpg_mode on both (no pause request on VCN 4),
     * PG_MODE off before the PMFW powers them down; JPEG runs on */
    CHECK(vcn40_idle_work_handler(dev, v) == 1 && v->cur_state == RD_PG_STATE_GATE && v->dpg_stops == 2);
    CHECK(S.vcn.down_in_dpg == 0 && !S.vcn.smu_on[0] && !S.vcn.smu_on[1] && S.vcn.pause_reqs == 0);
    for (i = 0; i < 2; i++)
        CHECK(!(S.reg[VCNR(i, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_PG_MODE_MASK) &&
              (S.reg[VCNR(i, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) == 1);
    CHECK(jpeg40_dec_ring_test_ib(dev, v) == RDNA3_OK && v->cur_state == RD_PG_STATE_GATE);

    /* the next job: DPG start again, one more SRAM upload per instance */
    buf = rdna3_vram_alloc(dev, 4096, 4096, "vcn-dpg-job");
    rdna3_vram_write(dev, buf, pk, sizeof(pk));
    rdna3_hdp_flush(dev);
    ib.va = rdna3_vram_mc(dev, buf);
    ib.length_dw = 4;
    ib.kdata = NULL;
    seq = vcn40_submit(dev, v, 1, 0, &ib, 1, 0);
    CHECK(seq && vcn40_wait_fence(dev, &v->ring_enc[1], seq) == RDNA3_OK && v->cur_state == RD_PG_STATE_UNGATE);
    CHECK(S.vcn.sram_loads[0] == 2 && S.vcn.sram_loads[1] == 2 && S.vcn.sram_bad == 0 && S.vcn.boots == boots0 + 4);

    /* VCN 3's DPG pause from begin_use and the idle work (amdgpu skips it
     * for the unified queue): request, ACK, tiles held (2); unpause */
    pauses0 = S.vcn.pause_reqs;
    v->using_unified_queue = 0;
    vcn40_ring_begin_use(dev, v, &v->ring_enc[0]);
    CHECK(v->pause_state[0].fw_based == RD_VCN_DPG_STATE_PAUSE && v->dpg_enc_submission_cnt[0] == 1);
    CHECK(S.vcn.pause_reqs == pauses0 + 1 && S.vcn.pause_bad == 0 &&
          (S.reg[VCNR(0, regUVD_DPG_PAUSE)] & UVD_DPG_PAUSE__NJ_PAUSE_DPG_ACK_MASK) &&
          (S.reg[VCNR(0, regUVD_POWER_STATUS)] & UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) == 2);
    vcn40_ring_end_use(dev, v, &v->ring_enc[0]);
    CHECK(v->dpg_enc_submission_cnt[0] == 0 && v->dpg_pauses == 1);
    CHECK(vcn40_idle_work_handler(dev, v) == 1 && v->cur_state == RD_PG_STATE_GATE);
    CHECK(v->pause_state[0].fw_based == RD_VCN_DPG_STATE_UNPAUSE && S.vcn.unpause_reqs == 1 && v->dpg_unpauses == 1 &&
          !(S.reg[VCNR(0, regUVD_DPG_PAUSE)] & UVD_DPG_PAUSE__NJ_PAUSE_DPG_REQ_MASK) && S.vcn.down_in_dpg == 0);
    /* gated while still paused: vcn_v4_0_stop_dpg_mode unpauses first */
    vcn40_ring_begin_use(dev, v, &v->ring_enc[1]);
    vcn40_ring_end_use(dev, v, &v->ring_enc[1]);
    CHECK(v->pause_state[1].fw_based == RD_VCN_DPG_STATE_PAUSE && S.vcn.pause_reqs == pauses0 + 2);
    CHECK(vcn40_set_powergating_state(dev, v, RD_PG_STATE_GATE) == RDNA3_OK);
    CHECK(v->pause_state[1].fw_based == RD_VCN_DPG_STATE_UNPAUSE && S.vcn.unpause_reqs == 2 &&
          S.vcn.down_in_dpg == 0 && S.vcn.pause_bad == 0);
    v->using_unified_queue = 1;

    /* rdna3vcn=3 (indirect_sram off): the same writes through the LMA, no PSP */
    loads0 = S.vcn.sram_loads[0] + S.vcn.sram_loads[1];
    lma0 = S.vcn.lma_writes;
    v->indirect_sram = 0;
    CHECK(vcn40_set_powergating_state(dev, v, RD_PG_STATE_UNGATE) == RDNA3_OK);
    CHECK(S.vcn.lma_writes == lma0 + 2 * 27 && S.vcn.lma_bad == 0 && S.vcn.sram_loads[0] + S.vcn.sram_loads[1] == loads0);
    CHECK(S.vcn.boots == boots0 + 8 && S.vcn.bad_boots == bb0);   /* + the start for the pause test */
    CHECK(S.reg[VCNR(1, regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW)] == (uint32_t)tmr[1] &&
          S.reg[VCNR(1, regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW)] == (uint32_t)vcpu_mc &&
          S.reg[VCNR(1, regUVD_VCPU_CACHE_SIZE0)] == fw_size0);
    for (i = 0; i < 2; i++)
        CHECK(vcn40_enc_ring_test_ring(dev, v, &v->ring_enc[i]) == RDNA3_OK);
    CHECK(vcn40_set_powergating_state(dev, v, RD_PG_STATE_GATE) == RDNA3_OK && S.vcn.down_in_dpg == 0);
    v->indirect_sram = 1;

    /* a sleep that loses the power, in DPG */
    vcn_s3_cycle(dev, v, blobs);
    CHECK(S.vcn.sram_bad == 0 && S.vcn.pause_bad == 0 && S.vcn.down_in_dpg == 0 && v->sram_upload_errs == 0);
    CHECK(vcn40_idle_work_handler(dev, v) == 1 && jpeg40_idle_work_handler(dev, v) == 1);
}

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
                     sim_vram_written, sim_doorbell, sim_doorbell32 };
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
        CHECK(smu13_pm_fini(&dev, &smu) == RDNA3_OK && S.smu.unload && S.smu.unload_arg == 0);

        /* rdna3amdgpu=1: smu_smc_hw_setup's own order, the allowed mask of
         * smu_v13_0_0_get_allowed_feature_mask, the pcie_table sent whole,
         * the thermal / MP1 interrupts, the performance levels, MP1 state */
        {
            uint64_t am = smu13_get_allowed_feature_mask(0x00526100u, RD_PP_FEATURE_MASK_DEFAULT, 0);
            uint32_t thm, hot = 0, *fl;
            int umd = 0;
            CHECK(am == ~RD_SMU_FEATURE(FEATURE_ATHUB_MMHUB_PG_BIT));
            CHECK(!(smu13_get_allowed_feature_mask(0x004e3900u, RD_PP_FEATURE_MASK_DEFAULT, 0) &
                    RD_SMU_FEATURE(FEATURE_GFXOFF_BIT)));
            CHECK(smu13_get_allowed_feature_mask(0x00526100u, RD_PP_FEATURE_MASK_DEFAULT & ~RD_PP_PCIE_DPM_MASK, 1) ==
                  ~(RD_SMU_FEATURE(FEATURE_DPM_LINK_BIT) | RD_SMU_FEATURE(FEATURE_DS_LCLK_BIT)));
            CHECK(smu13_is_mode1_reset_supported(&smu));
            dev.amdgpu_exact = 1;
            dev.amdgpu_dc = 0;
            S.smu.all_enabled = 0;
            S.smu.pcie_overrides = S.smu.pcie_after_enable = S.smu.dal_not_present = 0;
            S.smu.unload = 0;
            *R(THMR(0x000b)) = 0x07000000u;     /* masked, trigger mask set */
            *R(MP1_BASE + 0x02c3) = 1;          /* MP1_SMN_IH_SW_INT_CTRL.INT_MASK */
            /* Gen4 x16: the verified profile sends nothing, amdgpu all three levels */
            CHECK(smu13_pm_init(&dev, &smu, &vb, RD_SMU_FEATURES_AMDGPU, 3, 6, 0) == RDNA3_OK);
            CHECK(smu.allowed == am && (smu.enabled & RD_SMU_FEATURE(FEATURE_GFXOFF_BIT)));
            CHECK(S.smu.pcie_overrides == 3 && S.smu.pcie_after_enable == 3 &&
                  S.smu.last_pcie_arg == ((2u << 16) | (3u << 8) | 6));
            CHECK(smu.pcie_levels == 3 && smu.pcie_gen[0] == 2 && smu.pcie_lane[2] == 6);
            CHECK(smu.vdclk_valid && smu.vclk.count == 3 && smu.vclk.level[2] == 2600 &&
                  smu.dclk.level[0] == 400);
            thm = *R(THMR(0x000b));
            CHECK(smu.irq_enabled && (thm & 0xff) == 118 && ((thm >> 8) & 0xff) == 0 &&
                  !(thm & 0x07000000u) && (thm & 0x10000000u) && (thm >> 29) == 5);
            CHECK(*R(THMR(0x000a)) == 0x38);
            CHECK((*R(MP1_BASE + 0x02c2) & 0x1ff) == 0xFE && !(*R(MP1_BASE + 0x02c3) & 1));
            CHECK(S.smu.dal_not_present == 1 && S.smu.ih_allow == 0 && !smu.dc_controlled_by_gpio);

            /* the IVs smu_v13_0_register_irq_handler adds */
            CHECK(smu13_iv_is_smu(RD_IH_CLIENT_THM, RD_THM_SRCID_DIG_THERM_L2H) &&
                  smu13_iv_is_smu(RD_IH_CLIENT_ROM_SMUIO, RD_SMUIO_SRCID_GPIO19) &&
                  smu13_iv_is_smu(RD_IH_CLIENT_MP1, RD_SMU_IH_INTERRUPT_ID_TO_DRIVER) &&
                  !smu13_iv_is_smu(RD_IH_CLIENT_MP1, 0) && !smu13_iv_is_smu(RD_IH_CLIENT_THM, 2));
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_THM, RD_THM_SRCID_DIG_THERM_L2H, 0) ==
                  RD_SMU_IRQ_SWCTF);
            CHECK(smu13_swctf_check(&dev, &smu, &hot) == 0 && hot == 60);   /* 60 C < 118 C */
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_THM, RD_THM_SRCID_DIG_THERM_H2L, 0) ==
                  RD_SMU_IRQ_UNDER);
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_ROM_SMUIO, RD_SMUIO_SRCID_GPIO19, 0) ==
                  RD_SMU_IRQ_HWCTF);
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_MP1, RD_SMU_IH_INTERRUPT_ID_TO_DRIVER,
                                    RD_SMU_IH_CTX_FAN_ABNORMAL) == RD_SMU_IRQ_NONE);
            CHECK((*R(THMR(0x000b)) & 0xff) == 128 && (*R(MP1_BASE + 0x02c3) & 0x100));
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_MP1, RD_SMU_IH_INTERRUPT_ID_TO_DRIVER,
                                    RD_SMU_IH_CTX_FAN_RECOVERY) == RD_SMU_IRQ_NONE);
            CHECK((*R(THMR(0x000b)) & 0xff) == 118);
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_MP1, RD_SMU_IH_INTERRUPT_ID_TO_DRIVER,
                                    RD_SMU_IH_CTX_AC) == RD_SMU_IRQ_NONE &&
                  S.smu.acdc_reenable == 1 && smu.ac_power == 1);
            CHECK(smu13_irq_process(&dev, &smu, RD_IH_CLIENT_MP1, RD_SMU_IH_INTERRUPT_ID_TO_DRIVER,
                                    RD_SMU_IH_CTX_THERMAL_THROTTLING) == RD_SMU_IRQ_THROTTLE &&
                  smu.throttle_int_counter == 1);

            /* power_dpm_force_performance_level */
            S.smu.gpo_msgs = 0;
            CHECK(smu13_force_performance_level(&dev, &smu, RD_DPM_FORCED_LEVEL_HIGH, NULL, NULL) == RDNA3_OK);
            CHECK(smu.dpm_level == RD_DPM_FORCED_LEVEL_HIGH && S.smu.gpo_msgs == 0);
            CHECK(S.smu.soft_min[PPCLK_GFXCLK] == smu.gfx.level[smu.gfx.count - 1] &&
                  S.smu.soft_max[PPCLK_GFXCLK] == smu.gfx.level[smu.gfx.count - 1]);
            CHECK(S.smu.soft_min[PPCLK_UCLK] == 1218 && S.smu.soft_min[PPCLK_FCLK] == 1800 &&
                  S.smu.soft_min[PPCLK_SOCCLK] == 1200);
            CHECK(S.smu.soft_min[PPCLK_VCLK_0] == 2600 && S.smu.soft_max[PPCLK_VCLK_1] == 2600 &&
                  S.smu.soft_min[PPCLK_DCLK_1] == 2200);
            CHECK(smu13_force_performance_level(&dev, &smu, RD_DPM_FORCED_LEVEL_PROFILE_PEAK,
                                                test_umd_pstate, &umd) == RDNA3_OK);
            CHECK(umd == 1 && S.smu.gpo_msgs == 1 && S.smu.gpo == 0 &&
                  smu.saved_dpm_level == RD_DPM_FORCED_LEVEL_HIGH);
            CHECK(!(smu.enabled & RD_SMU_FEATURE(FEATURE_DS_GFXCLK_BIT)) &&
                  !(smu.enabled & RD_SMU_FEATURE(FEATURE_GFX_ULV_BIT)));
            CHECK(S.smu.soft_min[PPCLK_GFXCLK] == smu.pstate_gfx.peak && smu.pstate_gfx.peak == 2124);
            CHECK(smu13_force_performance_level(&dev, &smu, RD_DPM_FORCED_LEVEL_PROFILE_EXIT,
                                                test_umd_pstate, &umd) == RDNA3_OK);
            CHECK(umd == 0 && smu.dpm_level == RD_DPM_FORCED_LEVEL_HIGH && S.smu.gpo == 1 &&
                  (smu.enabled & RD_SMU_FEATURE(FEATURE_DS_GFXCLK_BIT)) &&
                  (smu.enabled & RD_SMU_FEATURE(FEATURE_GFX_ULV_BIT)));
            CHECK(smu13_force_performance_level(&dev, &smu, RD_DPM_FORCED_LEVEL_AUTO, NULL, NULL) == RDNA3_OK);
            CHECK(S.smu.soft_min[PPCLK_GFXCLK] == 500 && S.smu.soft_min[PPCLK_UCLK] == 96 &&
                  S.smu.soft_max[PPCLK_UCLK] == 1218 && S.smu.soft_min[PPCLK_DCLK_0] == 400);
            CHECK(smu13_set_performance_level(&dev, &smu, 0x400) == RDNA3_EINVAL);

            /* smu_hw_fini: alert off, no UNLOAD on a card with display
             * hardware; PrepareMp1ForUnload(0x55) for the shutdown */
            CHECK(smu13_pm_fini(&dev, &smu) == RDNA3_OK && !S.smu.unload && !smu.irq_enabled);
            CHECK((*R(THMR(0x000b)) & 0x03000000u) == 0x03000000u && *R(THMR(0x000a)) == 0 &&
                  (*R(MP1_BASE + 0x02c3) & 1));
            CHECK(smu13_set_mp1_state(&dev, &smu, RD_PP_MP1_STATE_SHUTDOWN) == RDNA3_OK && !S.smu.unload);
            CHECK(smu13_set_mp1_state(&dev, &smu, RD_PP_MP1_STATE_UNLOAD) == RDNA3_OK &&
                  S.smu.unload && S.smu.unload_arg == 0x55);
            CHECK(smu13_check_fw_status(&dev) == RDNA3_OK);
            smu.baco_state = RD_SMU_BACO_STATE_EXIT;   /* after a BACO exit the PMFW state goes too */
            CHECK(smu13_set_mp1_state(&dev, &smu, RD_PP_MP1_STATE_UNLOAD) == RDNA3_OK);
            CHECK(smu13_check_fw_status(&dev) != RDNA3_OK);
            fl = smn_slot(RD_SMN_MP1_FIRMWARE_FLAGS, 1);
            if (fl)
                *fl = MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK;
            S.smu.unload = 0;
            dev.amdgpu_exact = 0;
        }

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

        test_pm_control(&dev, &vb);
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
            /* gfx_v11_0_constants_init (verified default): every VMID 1..15
             * gets the graphics apertures */
            CHECK(S.sh_mem[3][1] == 0x20001000u && S.sh_mem[12][1] == 0x20001000u && S.sh_mem[0][1] == 0);
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

            /* rdna3amdgpu=1: amdgpu_vm_pt_clear values, amdgpu_vm_ptes_update
             * with fragments and huge PDEs, unmapping with EXECUTABLE */
            {
                const uint64_t hva = 0x0000200000000000ull;   /* 1 GiB aligned */
                uint64_t hoff = rdna3_vram_alloc(&dev, 4ull << 20, 2ull << 20, "test-huge-pages");
                uint64_t hpa = rdna3_vram_pa(&dev, hoff), pdb1, pdb0, ptb, sys = 0x7000000000ull;
                uint32_t dist = regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 -
                                regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
                unsigned walks = S.vm_huge_walks, t0;
                CHECK(hoff != UINT64_MAX && hoff + (4ull << 20) <= BAR_LEN && !(hpa & ((2ull << 20) - 1)));
                dev.amdgpu_exact = 1;
                CHECK(gpuvm_vm_init(&dev, &vm, &pool) == RDNA3_OK && vm.tables == 1);
                rdna3_vram_read(&dev, vm.root + 8 * 511, &e, 8);
                CHECK(e == RD_PDE_PTE);                    /* a PD's cleared entry */
                /* 4 MiB, 4 MiB aligned in VA: fragment 10, two 2 MiB PDEs in the PDB0 */
                CHECK(gpuvm_map_linear(&dev, &vm, hva, hpa, 1024, vpte) == RDNA3_OK);
                CHECK(vm.tables == 3);                      /* root, PDB1, PDB0: no PTB */
                rdna3_vram_read(&dev, vm.root + 8ull * ((hva >> 39) & 511), &e, 8);
                pdb1 = (e & 0x0000FFFFFFFFF000ull) - rdna3_vram_pa(&dev, 0);
                rdna3_vram_read(&dev, pdb1 + 8ull * ((hva >> 30) & 511), &e, 8);
                pdb0 = (e & 0x0000FFFFFFFFF000ull) - rdna3_vram_pa(&dev, 0);
                rdna3_vram_read(&dev, pdb0 + 8ull * (((hva >> 21) & 511) + 1), &e, 8);
                CHECK((e & RD_PTE_VALID) && (e & RD_PDE_PTE) && (e & RD_PTE_FRAG(31)) == RD_PTE_FRAG(10) &&
                      (e & 0x0000FFFFFFFFF000ull) == hpa + (2ull << 20) && (e >> 48 & 7) == RD_MTYPE_UC);
                rdna3_vram_read(&dev, pdb0 + 8ull * 2, &e, 8);
                CHECK(e == RD_PDE_PTE);
                CHECK(gpuvm_lookup(&dev, &vm, hva + 0x200123) == hpa + 0x200123);
                /* the walker of the GFXHUB through the huge entry (VMID 9) */
                S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + dist * 9] =
                    (uint32_t)gpuvm_pd_addr(&dev, &vm);
                S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 + dist * 9] =
                    (uint32_t)(gpuvm_pd_addr(&dev, &vm) >> 32);
                CHECK(vm_ptr(9, hva + 0x300010, 4) == S.vram + hpa + 0x300010 && S.vm_huge_walks == walks + 1);
                /* one page right after: a PTB, its neighbours cleared to EXECUTABLE */
                CHECK(gpuvm_map_linear(&dev, &vm, hva + (4ull << 20) + 4096, hpa, 1, vpte) == RDNA3_OK);
                CHECK(vm.tables == 4);
                rdna3_vram_read(&dev, pdb0 + 8ull * 2, &e, 8);
                ptb = (e & 0x0000FFFFFFFFF000ull) - rdna3_vram_pa(&dev, 0);
                CHECK((e & RD_PTE_VALID) && !(e & RD_PDE_PTE));
                rdna3_vram_read(&dev, ptb + 8, &e, 8);
                CHECK((e & RD_PTE_VALID) && !(e & RD_PTE_FRAG(31)) && (e & 0x0000FFFFFFFFF000ull) == hpa);
                rdna3_vram_read(&dev, ptb, &e, 8);
                CHECK(e == RD_PTE_EXECUTABLE);
                /* 64 KiB aligned 16 pages: fragment 4 in every PTE */
                CHECK(gpuvm_map_linear(&dev, &vm, hva + (4ull << 20) + 0x10000, hpa + 0x10000, 16, vpte) == RDNA3_OK);
                rdna3_vram_read(&dev, ptb + 8 * 16 + 8 * 15, &e, 8);
                CHECK((e & RD_PTE_FRAG(31)) == RD_PTE_FRAG(4) && (e & 0x0000FFFFFFFFF000ull) == hpa + 0x10000 + 15 * 4096);
                /* system pages: scattered ones get 4 KiB PTEs, a contiguous run
                 * is mapped like VRAM (fragment) */
                CHECK(gpuvm_map(&dev, &vm, hva + (4ull << 20) + 0x20000, 4, test_scatter_pa, &sys,
                                gpuvm_pte_flags(6, 1)) == RDNA3_OK);
                rdna3_vram_read(&dev, ptb + 8 * 32, &e, 8);
                CHECK((e & 0x0000FFFFFFFFF000ull) == sys + 4096 && !(e & RD_PTE_FRAG(31)) && (e & RD_PTE_SYSTEM));
                CHECK(gpuvm_map(&dev, &vm, hva + (4ull << 20) + 0x30000, 16, test_contig_pa, &sys,
                                gpuvm_pte_flags(6, 1)) == RDNA3_OK);
                rdna3_vram_read(&dev, ptb + 8 * 48 + 8 * 3, &e, 8);
                CHECK((e & 0x0000FFFFFFFFF000ull) == sys + 3 * 4096 && (e & RD_PTE_FRAG(31)) == RD_PTE_FRAG(4));
                /* unmapping the 4 MiB: invalid huge entries (PDE_PTE | FRAG) */
                gpuvm_unmap(&dev, &vm, hva, 1024);
                rdna3_vram_read(&dev, pdb0 + 8ull * (hva >> 21 & 511), &e, 8);
                CHECK(!(e & RD_PTE_VALID) && (e & RD_PDE_PTE) && (e & RD_PTE_FRAG(31)) == RD_PTE_FRAG(10));
                CHECK(gpuvm_lookup(&dev, &vm, hva + 0x1000) == 0 && vm_ptr(9, hva, 4) == NULL);
                /* unmapping one page: EXECUTABLE */
                gpuvm_unmap(&dev, &vm, hva + (4ull << 20) + 4096, 1);
                rdna3_vram_read(&dev, ptb + 8, &e, 8);
                CHECK(e == RD_PTE_EXECUTABLE);
                /* a 2 MiB huge mapping over the PTB frees it */
                t0 = vm.tables;
                CHECK(gpuvm_map_linear(&dev, &vm, hva + (4ull << 20), hpa, 512, vpte) == RDNA3_OK);
                CHECK(vm.tables == t0 - 1);
                rdna3_vram_read(&dev, pdb0 + 8ull * 2, &e, 8);
                CHECK((e & RD_PTE_VALID) && (e & RD_PDE_PTE) && (e & RD_PTE_FRAG(31)) == RD_PTE_FRAG(9));
                CHECK(gpuvm_lookup(&dev, &vm, hva + (4ull << 20) + 0x1234) == hpa + 0x1234);
                gpuvm_vm_fini(&dev, &vm);
                CHECK(vm.tables == 0 && pool.nfree == 64);
                S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + dist * 9] = 0;
                S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 + dist * 9] = 0;
                dev.amdgpu_exact = 0;
            }

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
                /* AMDGPU_CHUNK_ID_CP_GFX_SHADOW on the gfx ring
                 * (gfx_v11_0_ring_emit_gfx_shadow from amdgpu_ib_schedule) */
                {
                    const uint64_t shva = 0x0000400000100000ull, csva = 0x0000400000180000ull,
                                   gdva = 0x00004000001c0000ull;
                    static const uint32_t first[] = {
                        PACKET3_ACQUIRE_MEM, PACKET3_NOP, PACKET3_SET_Q_PREEMPTION_MODE, PACKET3_COND_EXEC,
                        PACKET3_WAIT_REG_MEM, PACKET3_CONTEXT_CONTROL, PACKET3_FRAME_CONTROL,
                        PACKET3_INDIRECT_BUFFER, PACKET3_FRAME_CONTROL, PACKET3_NOP, PACKET3_COND_EXEC,
                        PACKET3_WRITE_DATA, PACKET3_SET_Q_PREEMPTION_MODE, PACKET3_COND_EXEC,
                        PACKET3_RELEASE_MEM };
                    static const uint32_t next[] = {
                        PACKET3_ACQUIRE_MEM, PACKET3_NOP, PACKET3_COND_EXEC, PACKET3_SET_Q_PREEMPTION_MODE,
                        PACKET3_COND_EXEC, PACKET3_WAIT_REG_MEM, PACKET3_CONTEXT_CONTROL,
                        PACKET3_FRAME_CONTROL, PACKET3_INDIRECT_BUFFER, PACKET3_FRAME_CONTROL,
                        PACKET3_NOP, PACKET3_COND_EXEC, PACKET3_WRITE_DATA, PACKET3_SET_Q_PREEMPTION_MODE,
                        PACKET3_COND_EXEC, PACKET3_RELEASE_MEM };
                    rdna3_gfx_job_ext x;
                    uint32_t ops[32], at[32], k, nops, qm, ibs0, skips, cond, gate_q;
                    uint64_t w0, ring_mc = rdna3_vram_mc(&dev, gr.ring), cond_mc;
                    const uint32_t *h;
                    uint32_t me_cntl;

                    memset(&x, 0, sizeof(x));
                    x.shadow_va = shva; x.csa_va = csva; x.gds_va = gdva; x.init_shadow = 1;
                    /* adev->gfx.cp_gfx_shadow off (bare metal): the chunk
                     * changes nothing, no COND_EXEC either */
                    qm = S.cp_qmode;
                    w0 = gr.wptr;
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK && S.cp_qmode == qm);
                    nops = ring_ops(&dev, &gr, w0, gr.wptr, ops, at, 32);
                    CHECK(nops == 7);
                    for (k = 0; k < nops; k++)
                        CHECK(ops[k] != PACKET3_SET_Q_PREEMPTION_MODE && ops[k] != PACKET3_COND_EXEC &&
                              ops[k] != PACKET3_NOP);
                    /* cp_gfx_shadow on (what amdgpu does for a VF): first job,
                     * no earlier gate - prefix, outer COND_EXEC, ..., postfix,
                     * second COND_EXEC, fence */
                    rdna3_vram_read(&dev, gr.wb + RD_GFX_WB_COND_EXE, &k, 4);
                    CHECK(k == 1 && gr.set_q_mode_offs == 0);
                    cond_mc = rdna3_vram_mc(&dev, gr.wb + RD_GFX_WB_COND_EXE);
                    gr.cp_gfx_shadow = 1;
                    ibs0 = S.cp_ibs; skips = S.cp_cond_skips;
                    w0 = gr.wptr;
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                    nops = ring_ops(&dev, &gr, w0, gr.wptr, ops, at, 32);
                    CHECK(nops == sizeof(first) / 4 && !memcmp(ops, first, sizeof(first)));
                    if (nops == sizeof(first) / 4) {
                        /* prefix: gate 1, SET_Q with the VAs, VMID 5, INIT_SHADOW_MEM */
                        CHECK(ring_dw(&dev, &gr, at[1] + 1) == 1 && ring_dw(&dev, &gr, at[1] + 2) == 0);
                        CHECK(ring_dw(&dev, &gr, at[2] + 1) == (uint32_t)shva &&
                              ring_dw(&dev, &gr, at[2] + 2) == (uint32_t)(shva >> 32) &&
                              ring_dw(&dev, &gr, at[2] + 3) == (uint32_t)gdva &&
                              ring_dw(&dev, &gr, at[2] + 4) == (uint32_t)(gdva >> 32) &&
                              ring_dw(&dev, &gr, at[2] + 5) == (uint32_t)csva &&
                              ring_dw(&dev, &gr, at[2] + 6) == (uint32_t)(csva >> 32) &&
                              ring_dw(&dev, &gr, at[2] + 7) == 5 && ring_dw(&dev, &gr, at[2] + 8) == 1);
                        /* outer COND_EXEC on ring->cond_exe_gpu_addr, covering
                         * everything up to the end of the fence */
                        CHECK(ring_dw(&dev, &gr, at[3] + 1) == (uint32_t)cond_mc &&
                              ring_dw(&dev, &gr, at[3] + 2) == (uint32_t)(cond_mc >> 32) &&
                              ring_dw(&dev, &gr, at[3] + 3) == 0 &&
                              ring_dw(&dev, &gr, at[3] + 4) == ((uint32_t)(gr.wptr - 1 - (at[3] + 4)) & gr.buf_mask));
                        /* postfix: gate 0 at first, COND_EXEC on the prefix's
                         * gate over WRITE_DATA + SET_Q (5 + 9 dw), the
                         * WRITE_DATA re-arms its own gate, SET_Q all zero */
                        gate_q = at[9] + 1;
                        cond = at[10];
                        CHECK(ring_dw(&dev, &gr, cond + 1) == (uint32_t)(ring_mc + 4ull * (at[1] + 1)) &&
                              ring_dw(&dev, &gr, cond + 4) == 14);
                        CHECK(ring_dw(&dev, &gr, at[11] + 1) == (WRITE_DATA_DST_SEL(5) | WR_CONFIRM) &&
                              ring_dw(&dev, &gr, at[11] + 2) == (uint32_t)(ring_mc + 4ull * gate_q) &&
                              ring_dw(&dev, &gr, at[11] + 4) == 1);
                        for (k = 1; k <= 8; k++)
                            CHECK(ring_dw(&dev, &gr, at[12] + k) == 0);
                        /* the second COND_EXEC keeps a count of 0 (amdgpu
                         * patches only the first) */
                        CHECK(ring_dw(&dev, &gr, at[13] + 1) == (uint32_t)cond_mc &&
                              ring_dw(&dev, &gr, at[13] + 4) == 0);
                        /* the CP ran both SET_Qs, the postfix armed its gate */
                        CHECK(S.cp_qmode == qm + 2 && ring_dw(&dev, &gr, gate_q) == 1);
                        h = S.cp_qmode_hist[qm & 3];
                        CHECK(h[0] == (uint32_t)shva && h[2] == (uint32_t)gdva && h[4] == (uint32_t)csva &&
                              h[6] == 5 && h[7] == 1);
                        h = S.cp_qmode_hist[(qm + 1) & 3];
                        CHECK(!h[0] && !h[1] && !h[4] && !h[6] && !h[7]);
                        CHECK(gr.set_q_mode_offs == gate_q && gr.set_q_mode_ptr_valid &&
                              gr.set_q_mode_ptr == at[1] + 1);
                    }
                    CHECK(S.cp_ibs == ibs0 + 1 && S.cp_cond_skips == skips && S.cp_bad == 0 &&
                          S.cp_wait_stuck == 0);
                    /* the next job chains its prefix to the last postfix's gate */
                    x.init_shadow = 0;
                    gate_q = gr.set_q_mode_offs;
                    w0 = gr.wptr;
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                    nops = ring_ops(&dev, &gr, w0, gr.wptr, ops, at, 32);
                    CHECK(nops == sizeof(next) / 4 && !memcmp(ops, next, sizeof(next)));
                    if (nops == sizeof(next) / 4)
                        CHECK(ring_dw(&dev, &gr, at[2] + 1) == (uint32_t)(ring_mc + 4ull * gate_q) &&
                              ring_dw(&dev, &gr, at[2] + 4) == 9 &&      /* over SET_Q */
                              ring_dw(&dev, &gr, at[3] + 8) == 0);       /* no INIT_SHADOW_MEM */
                    CHECK(S.cp_qmode == qm + 4 && S.cp_cond_skips == skips && S.cp_ibs == ibs0 + 2);
                    /* the CP behind by two jobs of the same configuration:
                     * the CPU clears the gate of the older postfix, so that
                     * postfix and the next prefix are skipped and the state
                     * stays shadowed - one prefix and one postfix run */
                    me_cntl = S.reg[GCR(regCP_ME_CNTL)];
                    S.reg[GCR(regCP_ME_CNTL)] = me_cntl | CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK;
                    CHECK(gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x) != 0);
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, &x);
                    CHECK(seq != 0 && S.cp_qmode == qm + 4);
                    S.reg[GCR(regCP_ME_CNTL)] = me_cntl;
                    cp_run((uint32_t)gr.wptr);
                    CHECK(gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                    CHECK(S.cp_qmode == qm + 6 && S.cp_cond_skips == skips + 2 && S.cp_ibs == ibs0 + 4);
                    h = S.cp_qmode_hist[(qm + 4) & 3];
                    CHECK(h[0] == (uint32_t)shva && h[6] == 5);
                    h = S.cp_qmode_hist[(qm + 5) & 3];
                    CHECK(!h[0] && !h[6]);
                    /* a job without the chunk on a shadowing ring: its prefix
                     * turns shadowing off (VAs and VMID 0) and re-arms */
                    seq = gfx11_submit_job_ex(&dev, &gr, 0xC0FFEE, 5, 0, jobs, 1, NULL);
                    CHECK(seq != 0 && gfx11_wait_fence(&dev, &gr, seq) == RDNA3_OK);
                    CHECK(S.cp_qmode == qm + 8);
                    h = S.cp_qmode_hist[(qm + 6) & 3];
                    CHECK(!h[0] && !h[2] && !h[4] && !h[6] && !h[7]);
                    CHECK(S.cp_bad == 0 && S.cp_wait_stuck == 0);
                    gr.cp_gfx_shadow = 0;
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
                        /* D: user queues through the scheduler */
                        test_userq_hw(&dev, &mes, &gmc);
                        test_userq_uapi(&dev, &mes, &gmc);
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
                        CHECK(HQD(hc, regCP_HQD_PIPE_PRIORITY) == 0 && HQD(hc, regCP_HQD_QUEUE_PRIORITY) == 0 &&
                              !(HQD(hc, regCP_HQD_PQ_CONTROL) & CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH_MASK));
                        test_gfx_amdgpu(&dev, &gr, &kiq, &kcq, ibva, n, gpuvm_pd_addr(&dev, &vm));
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
                /* rdna3amdgpu=1: gfx_v11_0_init_compute_vmid for VMIDs 8..15 */
                memset(S.sh_mem, 0, sizeof(S.sh_mem));
                dev.amdgpu_exact = 1;
                CHECK(gfx11_hw_init(&dev, &gmc, &ga, &rs, 8192) == RDNA3_OK && ga.async && !ga.ready);
                dev.amdgpu_exact = 0;
                CHECK(S.sh_mem[7][1] == 0x20001000u && !(S.sh_mem[7][2] & 0x8));
                CHECK(S.sh_mem[8][1] == 0x00010002u && (S.sh_mem[8][2] & 0x8) && S.sh_mem[15][1] == 0x00010002u &&
                      (S.sh_mem[15][2] & 0x8) && S.sh_mem[15][0] == S.sh_mem[1][0]);
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

        /* VCN 4.0 / JPEG 4.0 (rdna3vcn=1): bring-up, rings, power gating, UAPI */
        {
            static rdna3_vcn vcn;
            test_vcn(&dev, &psp, &plan, blobs, &gmc, &vcn);
            test_vcn_uapi(&dev, &gmc, &vcn);
            /* rdna3vcn=1 through a sleep that cuts the power, then =2 / =3 */
            vcn_s3_cycle(&dev, &vcn, blobs);
            test_vcn_dpg(&dev, &psp, blobs, &vcn);
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
            /* rdna3scaledlg: the scaled plane also gets the line buffer and
             * the scale-dependent DLG/TTU (dml32), the GOP's come back with
             * the native size; without it (above) they stay untouched */
            {
                static rdna3_dcn_scale sc2;
                rdna3_scanout_info si;
                const uint32_t dfs1 = S.reg[RD_CLK1_CLK0_DFS_CNTL + 3], pll = S.reg[RD_CLK1_CLK_PLL_REQ];
                const uint32_t dtoc = S.reg[DCN_B1 + regDPPCLK_DTO_CTRL];
                CHECK(DSC(regDSCL0_LB_MEMORY_CTRL) == 0 && DSC(regDSCL0_LB_DATA_FORMAT) == 0);
                memset(&sc2, 0, sizeof(sc2));
                sc2.dlg = 1;
                /* the GOP's 1:1 plane: LB as after reset, DLG of DML for 1440p */
                DSC(regDSCL0_LB_DATA_FORMAT) = 0x10;
                DSC(regDSCL0_LB_MEMORY_CTRL) = 0x3f00;
                S.reg[HUBP(regHUBPREQ0_DST_AFTER_SCALER, 1)] = 1110;
                S.reg[HUBP(regHUBPREQ0_DST_DIMENSIONS, 1)] = 1125u << 8;   /* refcyc_per_htotal 1125 */
                S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY, 1)] = 1060;
                S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY_PRE, 1)] = 1000;
                S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL0, 1)] = 0x10000 | (8u << 24);
                S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL1, 1)] = 0x8000;
                /* DPPCLK 165.4 MHz (VCO 4.3 GHz / 26), no DTO on pipe 1 */
                S.reg[RD_CLK1_CLK_PLL_REQ] = 43;
                S.reg[RD_CLK1_CLK0_DFS_CNTL + 3] = 0x54;
                S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] = 0;
                CHECK(dcn32_set_source(&dev, &dc, &sc2, 0, FB_START + 0x2000000, 1280, 720, 1280) == RDNA3_OK);
                /* LB: 970 / ceil(1280 / 6) = 4 lines >= 2 taps -> config 1, 31 partitions */
                CHECK(DSC(regDSCL0_LB_MEMORY_CTRL) == (1u | 31u << 8) && DSC(regDSCL0_LB_DATA_FORMAT) == 0);
                /* 1 / HRatio = 2: twice the time per 256-byte request */
                CHECK(S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL0, 1)] == (0x20000u | (8u << 24)) &&
                      S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL1, 1)] == 0x10000);
                /* line delivery: 1280 / 0.5 = 2560 pixel times, as before */
                CHECK(S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY, 1)] == 1060 &&
                      S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY_PRE, 1)] == 1000);
                /* + ceil(34 x 100 MHz / 165.4 MHz) = 21: 1131 = line 1 + 6 */
                CHECK(S.reg[HUBP(regHUBPREQ0_DST_AFTER_SCALER, 1)] == (6u | 1u << 16));
                CHECK(dcn32_scanout_info(&dev, 1, &si) == RDNA3_OK && si.dscl_mode == 1 &&
                      si.lb_memory_ctrl == (1u | 31u << 8) && si.dlg_req_delivery == 0x20000 &&
                      si.dlg_line_delivery == 1060 && si.dppclk_khz == 165384 && si.dlg_after_scaler == (6u | 1u << 16));
                /* 1366 wide: the swath rounds up to 1408 pixels */
                CHECK(dcn32_set_source(&dev, &dc, &sc2, 0, FB_START + 0x2000000, 1366, 768, 1408) == RDNA3_OK);
                CHECK((S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL0, 1)] & 0x7fffff) == 122820 &&
                      S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL1, 1)] == 61410 &&
                      S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY, 1)] == 1093 &&
                      S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY_PRE, 1)] == 1031);
                /* native: the GOP's values again */
                CHECK(dcn32_set_source(&dev, &dc, &sc2, 0, FB_START + 0x6000000, 2560, 1440, 2624) == RDNA3_OK);
                CHECK(DSC(regDSCL0_LB_MEMORY_CTRL) == 0x3f00 && DSC(regDSCL0_LB_DATA_FORMAT) == 0x10 &&
                      S.reg[HUBP(regHUBPREQ0_DST_AFTER_SCALER, 1)] == 1110 &&
                      S.reg[HUBP(regHUBPREQ0_DCN_SURF0_TTU_CNTL0, 1)] == (0x10000u | (8u << 24)) &&
                      S.reg[HUBP(regHUBPREQ0_PER_LINE_DELIVERY, 1)] == 1060 && !sc2.saved[1] && !sc2.bw_saved[1]);
                /* the report clears the sticky underflow to see if it comes back */
                CHECK(dcn32_clear_underflow(&dev, 1) == RDNA3_OK &&
                      (S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_CLEAR_MASK));
                S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] &= ~HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_CLEAR_MASK;
                CHECK(dcn32_clear_underflow(&dev, 4) == RDNA3_EINVAL);
                S.reg[RD_CLK1_CLK0_DFS_CNTL + 3] = dfs1;
                S.reg[RD_CLK1_CLK_PLL_REQ] = pll;
                S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] = dtoc;
            }
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

    /* hotplug after boot (dchpd.c, rdna3hotplug=1): program_hpd_filter on
     * the toggle filter, the debounced DC_HPD_SENSE_DELAYED poll, the sink
     * read again with dm_helpers_read_local_edid's tries, and what a change
     * on each pin means for the display side */
    {
        static rdna3_hpd_track t;
        static rdna3_connector c;
        rdna3_display_path paths[2];
        rdna3_dcn_head h;
        rdna3_hp_view v;
        uint32_t on_ms = 0, off_ms = 0, tf = 0, ft = 0, raw = 0, gen;
        uint64_t t0;
        /* TMDS 500/100 ms, DP 80/0 ms in 10 ms units; eDP not programmed */
        S.reg[HPDR(regHPD0_DC_HPD_CONTROL, 2)] = 0;
        S.reg[HPDR(regHPD0_DC_HPD_FAST_TRAIN_CNTL, 2)] = 0x10000a05u;
        CHECK(dchpd_program_filter(&dev, 2, RD_ATOM_CONN_HDMI_A, 1) == RDNA3_OK);
        CHECK(S.reg[HPDR(regHPD0_DC_HPD_CONTROL, 2)] & HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK);
        dchpd_filter_info(&dev, 2, &on_ms, &off_ms, &tf, &ft);
        CHECK(on_ms == 500 && off_ms == 100 && tf == (50u | 10u << 20) && ft == 0x10000a05u);
        CHECK(dchpd_program_filter(&dev, 0, RD_ATOM_CONN_DISPLAYPORT, 1) == RDNA3_OK);
        CHECK(S.reg[HPDR(regHPD0_DC_HPD_TOGGLE_FILT_CNTL, 0)] == 8);
        CHECK(dchpd_program_filter(&dev, 3, RD_ATOM_CONN_EDP, 1) == RDNA3_EINVAL &&
              S.reg[HPDR(regHPD0_DC_HPD_TOGGLE_FILT_CNTL, 3)] == 0);
        CHECK(dchpd_program_filter(&dev, 5, RD_ATOM_CONN_HDMI_A, 1) == RDNA3_EINVAL);
        CHECK(dchpd_program_filter(&dev, 1, RD_ATOM_CONN_DVI_D, 0) == RDNA3_OK &&
              S.reg[HPDR(regHPD0_DC_HPD_TOGGLE_FILT_CNTL, 1)] == 0);
        CHECK(S.reg[HPDR(regHPD0_DC_HPD_FAST_TRAIN_CNTL, 2)] == 0x10000a05u);   /* DC leaves it */
        CHECK(dchpd_program_filter(&dev, 1, RD_ATOM_CONN_DVI_D, 1) == RDNA3_OK);
        /* pin 0 has its DP sink, pin 2 a cable not plugged yet */
        hpd_cable(0, 1);
        hpd_cable(2, 0);
        S.now_us += 1000000;
        dchpd_track_init(&t, &dev, RD_AUX_ENGINES, RD_HPD_DEBOUNCE);
        CHECK(t.state[0] == 1 && t.state[2] == 0 && t.state[1] == 0 && t.generation == 0);
        /* plug: the raw sense follows at once, the filtered one after
         * 500 ms, the poll after two equal looks */
        hpd_cable(2, 1);
        sim_msleep(NULL, 100);
        CHECK(dchpd_track_poll(&t, &dev, &raw) == 0 && raw == 0);
        CHECK((S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] &
               (HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK | HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK)) ==
              HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK);
        sim_msleep(NULL, RD_HPD_POLL_MS);
        CHECK(dchpd_track_poll(&t, &dev, &raw) == 0 && raw == 1u << 2 && t.state[2] == 0);
        sim_msleep(NULL, RD_HPD_POLL_MS);
        CHECK(dchpd_track_poll(&t, &dev, &raw) == 1u << 2 && raw == 0);
        CHECK(t.state[2] == 1 && t.generation == 1 && t.events == 1 && t.glitches == 0);
        /* a bounce shorter than the 100 ms disconnect filter: never seen */
        hpd_cable(2, 0);
        sim_msleep(NULL, 50);
        hpd_cable(2, 1);
        sim_msleep(NULL, RD_HPD_POLL_MS);
        CHECK(dchpd_track_poll(&t, &dev, &raw) == 0 && raw == 0);
        /* longer than the filter but back before the second look: a glitch */
        hpd_cable(2, 0);
        sim_msleep(NULL, 150);
        CHECK(dchpd_track_poll(&t, &dev, &raw) == 0 && raw == 1u << 2);
        hpd_cable(2, 1);
        sim_msleep(NULL, 600);
        CHECK(dchpd_track_poll(&t, &dev, &raw) == 0 && t.state[2] == 1 && t.glitches == 1);
        /* the device gone (all-ones): no sample, no event */
        gen = t.generation;
        S.hpd_model &= ~(1u << 2);
        S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] = ~0u;
        sim_msleep(NULL, RD_HPD_POLL_MS);
        CHECK(dchpd_sense_delayed(&dev, 2) == -1);
        CHECK(dchpd_track_poll(&t, &dev, NULL) == 0 && dchpd_track_poll(&t, &dev, NULL) == 0 &&
              t.generation == gen);
        S.hpd_model |= 1u << 2;
        /* the sink read again: a TMDS sink on DDC3 answers with its EDID */
        S.i2c_sink = 2;
        CHECK(dchpd_detect(&dev, 2, &c, RD_HPD_EDID_TRIES) == RD_HPD_SINK);
        CHECK(c.aux == 2 && c.hpd && !c.dp && c.edid_ddc && c.edid_len == 256 && memcmp(c.edid, S.edid, 256) == 0);
        /* HPD high but no EDID yet: three tries 100 ms apart */
        S.i2c_sink = -1;
        t0 = S.now_us;
        CHECK(dchpd_detect(&dev, 2, &c, RD_HPD_EDID_TRIES) == RD_HPD_SINK_NO_EDID && c.edid_len == 0);
        CHECK(S.now_us - t0 >= 2000ull * RD_HPD_EDID_RETRY_MS);
        /* unplug: two looks after the 100 ms filter */
        hpd_cable(2, 0);
        sim_msleep(NULL, RD_HPD_POLL_MS);
        CHECK(dchpd_track_poll(&t, &dev, NULL) == 0);
        sim_msleep(NULL, RD_HPD_POLL_MS);
        CHECK(dchpd_track_poll(&t, &dev, NULL) == 1u << 2 && t.state[2] == 0 && t.generation == gen + 1);
        t0 = S.now_us;
        CHECK(dchpd_detect(&dev, 2, &c, RD_HPD_EDID_TRIES) == RD_HPD_NO_SINK &&
              S.now_us - t0 < 1000ull * RD_HPD_EDID_RETRY_MS);
        /* routing: HDMI on UNIPHY B / DDC2 / HPD2, DP on UNIPHY D / DDC3 / HPD3 */
        memset(paths, 0, sizeof(paths));
        paths[0].connector = RD_ATOM_CONN_HDMI_A; paths[0].transmitter = 1; paths[0].ddc_line = 1; paths[0].hpd = 1;
        paths[1].connector = RD_ATOM_CONN_DISPLAYPORT; paths[1].transmitter = 3; paths[1].ddc_line = 2;
        paths[1].hpd = 2;
        memset(&h, 0, sizeof(h));
        h.dig_be = 1;
        CHECK(dchpd_head_pin(&h, paths, 2) == 1 && dchpd_pin_aux(2, paths, 2) == 2 && dchpd_pin_aux(4, paths, 2) == 4);
        CHECK(dchpd_pin_path(2, paths, 2) == &paths[1] && dchpd_pin_path(0, paths, 2) == NULL);
        h.dig_be = -1;
        CHECK(dchpd_head_pin(&h, paths, 2) == -1);
        /* decisions: boot head reported only, a TMDS sink lights the clone
         * once, the clone's pin unplugged takes it down, DP sinks wait */
        memset(&c, 0, sizeof(c));
        c.aux = 2; c.hpd = 1; c.edid_len = 256; c.edid_ddc = 1;
        v.boot_pin = 1; v.clone_pin = -1; v.mirror = 2; v.can_clone = 1;
        CHECK(dchpd_decide(&v, 1, 0, NULL) == RD_HP_BOOT && dchpd_decide(&v, 1, 1, &c) == RD_HP_BOOT);
        CHECK(dchpd_decide(&v, 2, 1, &c) == RD_HP_CLONE_ON);
        CHECK(dchpd_decide(&v, 2, 0, NULL) == RD_HP_NONE);
        v.mirror = 0;
        CHECK(dchpd_decide(&v, 2, 1, &c) == RD_HP_NONE);
        v.mirror = 2; v.can_clone = 0;
        CHECK(dchpd_decide(&v, 2, 1, &c) == RD_HP_NONE);
        v.can_clone = 1;
        c.dp = 1;
        CHECK(dchpd_decide(&v, 2, 1, &c) == RD_HP_NONE);
        c.dp = 0; c.edid_len = 0;
        CHECK(dchpd_decide(&v, 2, 1, &c) == RD_HP_NONE);   /* detect_link_and_local_sink: no EDID, no sink */
        c.edid_len = 256;
        v.clone_pin = 2;
        CHECK(dchpd_decide(&v, 2, 1, &c) == RD_HP_NONE && dchpd_decide(&v, 2, 0, NULL) == RD_HP_CLONE_OFF);
        CHECK(dchpd_decide(&v, 3, 0, NULL) == RD_HP_NONE);
        CHECK(strcmp(dchpd_action_name(RD_HP_CLONE_OFF), "take the second monitor's pipe down") == 0);
        S.hpd_model = 0;
        S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] = 0;
        S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 0)] = HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK |
                                                    HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK;
        for (uint32_t n = 0; n < 4; n++)
            S.reg[HPDR(regHPD0_DC_HPD_TOGGLE_FILT_CNTL, n)] = 0;
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
            /* output gamma on both heads (hw/dcn32_gamma.c) */
            test_gamma(&dev, &dc2);
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
            /* hotplug drives the clone (rdna3hotplug=1 with rdna3mirror=2):
             * the HDMI cable on HPD3 plugged after boot lights it, pulled
             * takes it down; the boot head's own cable only gets reported */
            {
                static rdna3_hpd_track ht;
                static rdna3_connector hc;
                rdna3_display_path hp[2];
                rdna3_hp_view hv;
                uint32_t ch = 0, k3;
                int act;
                memset(hp, 0, sizeof(hp));
                hp[0].connector = RD_ATOM_CONN_HDMI_A; hp[0].transmitter = 1; hp[0].ddc_line = 0; hp[0].hpd = 0;
                hp[1] = path;
                hp[1].connector = RD_ATOM_CONN_HDMI_A; hp[1].ddc_line = 2;
                for (k3 = 0; k3 < 2; k3++)
                    CHECK(dchpd_program_filter(&dev, (uint32_t)hp[k3].hpd, hp[k3].connector, 1) == RDNA3_OK);
                hpd_cable(0, 1);
                hpd_cable(2, 0);
                S.now_us += 1000000;
                dchpd_track_init(&ht, &dev, RD_AUX_ENGINES, RD_HPD_DEBOUNCE);
                hv.boot_pin = dchpd_head_pin(&dc2.head[0], hp, 2);
                hv.clone_pin = -1; hv.mirror = 2; hv.can_clone = 1;
                CHECK(hv.boot_pin == 0);
                hpd_cable(2, 1);
                for (k3 = 0; k3 < 4 && !ch; k3++) {
                    sim_msleep(NULL, RD_HPD_POLL_MS);
                    ch = dchpd_track_poll(&ht, &dev, NULL);
                }
                CHECK(ch == 1u << 2 && k3 == 2);
                S.i2c_sink = 2;
                CHECK(dchpd_detect(&dev, (uint32_t)dchpd_pin_aux(2, hp, 2), &hc, RD_HPD_EDID_TRIES) == RD_HPD_SINK);
                S.i2c_sink = -1;
                act = dchpd_decide(&hv, 2, ht.state[2], &hc);
                CHECK(act == RD_HP_CLONE_ON);
                CHECK(dcn32_clone_plan(&dev, &dc2, 0, dchpd_pin_path(2, hp, 2), 148500, &cl) == RDNA3_OK);
                cl.smu_probe = 0;
                S.dmub.nseq = 0;
                CHECK(dcn32_clone_enable(&dev, &dm, &cl) == RDNA3_OK && cl.hpd == 2);
                CHECK(dcn32_read_state(&dev, &dc2) == 2);
                hv.clone_pin = cl.hpd;
                /* replugged within the filter: nothing */
                hpd_cable(2, 0);
                sim_msleep(NULL, 50);
                hpd_cable(2, 1);
                sim_msleep(NULL, RD_HPD_POLL_MS);
                CHECK(dchpd_track_poll(&ht, &dev, NULL) == 0);
                /* the boot head's cable: reported, its pipe untouched */
                hpd_cable(0, 0);
                for (ch = 0, k3 = 0; k3 < 4 && !ch; k3++) {
                    sim_msleep(NULL, RD_HPD_POLL_MS);
                    ch = dchpd_track_poll(&ht, &dev, NULL);
                }
                CHECK(ch == 1u << 0 && dchpd_decide(&hv, 0, ht.state[0], NULL) == RD_HP_BOOT);
                hpd_cable(0, 1);
                /* the clone's cable pulled: its pipe goes */
                hpd_cable(2, 0);
                for (ch = 0, k3 = 0; k3 < 6 && !(ch & (1u << 2)); k3++) {
                    sim_msleep(NULL, RD_HPD_POLL_MS);
                    ch |= dchpd_track_poll(&ht, &dev, NULL);
                }
                CHECK(ch & (1u << 2));
                CHECK(dchpd_detect(&dev, 2, &hc, RD_HPD_EDID_TRIES) == RD_HPD_NO_SINK);
                act = dchpd_decide(&hv, 2, ht.state[2], &hc);
                CHECK(act == RD_HP_CLONE_OFF);
                S.dmub.nseq = 0;
                CHECK(dcn32_clone_disable(&dev, &dm, &cl) == RDNA3_OK);
                CHECK(dcn32_read_state(&dev, &dc2) == 1 && dc2.head[0].pipe == 1);
                CHECK(ht.state[0] == 1 && ht.events == 4 && ht.generation == 4);
                S.hpd_model = 0;
                S.reg[HPDR(regHPD0_DC_HPD_INT_STATUS, 2)] = 0;
                for (k3 = 0; k3 < 4; k3++)
                    S.reg[HPDR(regHPD0_DC_HPD_TOGGLE_FILT_CNTL, k3)] = 0;
            }
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
                /* rdna3dispclk: 1920x1080 at 72 Hz (208.65 MHz) over DISPCLK
                 * and DPPCLK at 165.4 MHz (pipe 1's DTO at half of it) */
                {
                    rdna3_dcn_clk_limits lim, low;
                    rdna3_mode fast;
                    const uint32_t dfs0 = S.reg[RD_CLK1_CLK0_DFS_CNTL], dfs1 = S.reg[RD_CLK1_CLK0_DFS_CNTL + 3];
                    const uint32_t dto = S.reg[DCN_B1 + regDPPCLK0_DTO_PARAM + 1], dtoc = S.reg[DCN_B1 + regDPPCLK_DTO_CTRL];
                    memset(&fast, 0, sizeof(fast));
                    fast.h_active = 1920; fast.v_active = 1080; fast.h_total = 2592; fast.v_total = 1118;
                    fast.h_front = 136; fast.h_sync = 208; fast.v_front = 3; fast.v_sync = 5;
                    fast.pixel_khz = 208650; fast.refresh_mhz = 72000; fast.v_sync_pos = 1;
                    S.reg[RD_CLK1_CLK0_DFS_CNTL] = 0x54;
                    S.reg[RD_CLK1_CLK0_DFS_CNTL + 3] = 0x54;
                    CHECK((S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] & 0x10) && (dto & 0xff) == 0x32 && (dto >> 16) == 0x64);
                    /* without the boot-arg: refused as before */
                    CHECK(dcn32_modeset_plan(&dev, &dc2, 0, &own, 241500, &fast, &ms) == RD_MODESET_CLOCKS);
                    CHECK(dcn32_modeset_plan_clk(&dev, &dc2, 0, &own, 241500, &fast, NULL, &ms) == RD_MODESET_CLOCKS);
                    /* the PMFW's DPM levels through the DAL mailbox (dcn32_init_clocks) */
                    S.dal_dpm_n = 4;
                    S.dal_dpm[0] = 358; S.dal_dpm[1] = 716; S.dal_dpm[2] = 1075; S.dal_dpm[3] = 1433;
                    CHECK(dcn32_clk_limits_read(&dev, &lim) == RDNA3_OK && lim.pmfw == 0x00505c00 &&
                          lim.dispclk_levels == 4 && lim.dispclk_max_khz == 1433000 && lim.dppclk_max_khz == 1433000);
                    /* DML 3.2: x 1.005 x 1.01 -> 17.2 GHz / 81, x 1.005 -> / 82 */
                    CHECK(dcn32_required_dispclk(208650, 4300000, lim.dispclk_max_khz) == 212345 &&
                          dcn32_required_dppclk(208650, 4300000) == 209756);
                    /* the ramping margin goes first when the limit is short */
                    CHECK(dcn32_required_dispclk(208650, 4300000, 211000) == 4300000u * 4 / 82 &&
                          dcn32_required_dispclk(208650, 4300000, 200000) == 0);
                    low = lim;
                    low.dispclk_max_khz = 200000;
                    CHECK(dcn32_modeset_plan_clk(&dev, &dc2, 0, &own, 241500, &fast, &low, &ms) == RD_MODESET_CLOCKS);
                    CHECK(dcn32_modeset_plan_clk(&dev, &dc2, 0, &own, 241500, &fast, &lim, &ms) == RD_MODESET_OK);
                    CHECK(ms.raise_dispclk_khz == 212345 && ms.raise_dppclk_khz == 209756 && ms.pmfw == 0x00505c00 &&
                          ms.want.h_total == 2591);
                    /* the PMFW does not move the DFS: refused, the stream untouched */
                    S.dal_ignore_hard_min = 1;
                    S.dal_norder = 0;
                    S.dmub.nseq = 0;
                    CHECK(dcn32_modeset_apply(&dev, &dm, &dc2, &sc, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) ==
                          RDNA3_EBUSY);
                    CHECK(S.dmub.nseq == 0 && S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] == 2719 && dc2.head[0].width == 2560);
                    S.dal_ignore_hard_min = 0;
                    /* DPPCLK's hard min, then DISPCLK's, both before the first
                     * VBIOS command and the timing; the DTO of pipe 1 after */
                    S.dal_norder = 0;
                    S.dmub.nseq = 0;
                    S.dcn_frames = 1;
                    CHECK(dcn32_modeset_apply(&dev, &dm, &dc2, &sc, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) ==
                          RDNA3_OK);
                    CHECK(S.dal_norder == 2 && S.dal_order[0] == RD_PPCLK_DPPCLK && S.dal_order[1] == RD_PPCLK_DISPCLK &&
                          S.dal_order_nseq[0] == 0 && S.dal_order_nseq[1] == 0 &&
                          S.dal_order_htotal[0] == 2719 && S.dal_order_htotal[1] == 2719);
                    CHECK(S.dal_hard_min[RD_PPCLK_DPPCLK] == 209 && S.dal_hard_min[RD_PPCLK_DISPCLK] == 212);
                    /* the DFS: 17.2 GHz / 80 = 215 MHz, / 82 = 209.8 MHz */
                    CHECK(S.reg[RD_CLK1_CLK0_DFS_CNTL] == 0x48 && S.reg[RD_CLK1_CLK0_DFS_CNTL + 3] == 0x49);
                    /* dccg2_update_dpp_dto: ceil(255 x 209756 / 209756) */
                    CHECK(S.reg[DCN_B1 + regDPPCLK0_DTO_PARAM + 1] == (255u | 255u << 16) &&
                          (S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] & 0x10));
                    CHECK(S.dmub.nseq == 4 && S.reg[OTG(regOTG0_OTG_H_TOTAL, 2)] == 2591 && dc2.head[0].width == 1920);
                    /* back to 1440p at the GOP's clocks: nothing to raise */
                    S.reg[RD_CLK1_CLK0_DFS_CNTL] = dfs0;
                    S.reg[RD_CLK1_CLK0_DFS_CNTL + 3] = dfs1;
                    S.reg[DCN_B1 + regDPPCLK0_DTO_PARAM + 1] = dto;
                    S.reg[DCN_B1 + regDPPCLK_DTO_CTRL] = dtoc;
                    {
                        rdna3_mode up;
                        memset(&up, 0, sizeof(up));
                        up.h_active = 2560; up.v_active = 1440; up.h_total = 2720; up.v_total = 1481;
                        up.h_front = 48; up.h_sync = 32; up.v_front = 3; up.v_sync = 5; up.pixel_khz = 241500;
                        up.h_sync_pos = up.v_sync_pos = 1;
                        S.dal_norder = 0;
                        CHECK(dcn32_modeset_plan_clk(&dev, &dc2, 0, &own, 208650, &up, &lim, &ms) == RD_MODESET_OK &&
                              !ms.raise_dispclk_khz && !ms.raise_dppclk_khz);
                        CHECK(dcn32_modeset_apply(&dev, &dm, &dc2, &sc, &ms, FB_START, 2560, FB_START + 0x8000000, 1920,
                                                  50) == RDNA3_OK && S.dal_norder == 0 && dc2.head[0].width == 2560);
                    }
                    S.dcn_frames = 0;
                }
            }
            test_dcn_state(&dev, &dm);
            test_dp_link(&dev, &dm);
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

    /* rdna3amdgpu=1: psp_load_fw as amdgpu runs it on MP0 13.0.10 -
     * psp_early_init's flags, SETUP_TMR before the PMFW (no centralized DF
     * C-state management), then ASD, the register list and the HDCP / DTM
     * TAs; psp_hw_fini takes them down again */
    {
        static rdna3_psp p4;
        const uint8_t *ta = syn_ta;
        size_t ta_len = sizeof(syn_ta);
        uint8_t *ta_file = NULL;
        uint32_t ipfw0;
        if (fwdir) {
            ta_file = slurp(fwdir, amd_navi32_ta_name, &ta_len);
            ta = ta_file;
        }
        sim_mode1_reset();
        CHECK(psp13_wait_for_bootloader(&psp) == RDNA3_OK);
        rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);
        dev.amdgpu_exact = 1;
        CHECK(psp13_init(&p4, &dev, sos, sos_len) == RDNA3_OK);
        CHECK(p4.autoload_supported && !p4.boot_time_tmr && !p4.pmfw_centralized_cstate_management);
        CHECK(p4.sos.bin[AMD_PSP_BIN_RL].size != 0);   /* the 13.0.10 sOS carries a register list */
        if (ta) {
            CHECK(psp13_set_ta(&p4, ta, ta_len) == RDNA3_OK);
            CHECK(p4.asd.bin_size && p4.hdcp.bin_size && p4.dtm.bin_size);
        }
        ipfw0 = S.nipfw;
        CHECK(psp13_hw_init(&p4, &plan, blobs) == RDNA3_OK);
        CHECK(S.smu_loaded && S.smu_after_tmr && !S.tmr_after_smu);
        CHECK(S.ip_fw_types[ipfw0] == GFX_FW_TYPE_SMU);   /* first of psp_load_non_psp_fw */
        CHECK(S.autoload && S.rl_loads == 1 && !S.rl_before_tmr && p4.rl_loaded);
        CHECK(S.nipfw == ipfw0 + plan.count + 1);
        CHECK(S.ip_fw_types[S.nipfw - 1] == GFX_FW_TYPE_REG_LIST);
        if (ta) {
            CHECK(S.asd_loads == 1 && p4.asd.initialized && S.ta_loads == 2 && S.ta_bad == 0);
            CHECK(p4.hdcp.initialized && p4.dtm.initialized && p4.hdcp.session_id != p4.dtm.session_id);
            CHECK(p4.hdcp.shared != UINT64_MAX && p4.hdcp.shared_mem_size == PSP_HDCP_SHARED_MEM_SIZE);
            CHECK(psp13_hdcp_invoke(&p4, 7) == RDNA3_OK && S.ta_invokes == 1 && S.ta_last_cmd == 7);
            CHECK(psp13_dtm_invoke(&p4, 3) == RDNA3_OK && S.ta_invokes == 2 && S.ta_last_cmd == 3);
        }
        CHECK(smu13_check_fw_status(&dev) == RDNA3_OK);
        CHECK(smu13_hello(&dev, &smu_ver, &ifv) == RDNA3_OK);
        CHECK(psp13_hw_fini(&p4) == RDNA3_OK);
        CHECK(S.tmr_destroys == 1 && !S.tmr_setup && !p4.ring_created && S.ring_destroys >= 1);
        CHECK(S.ta_unloads == (ta ? 3u : 0u) && S.ta_bad == 0 && !p4.asd.initialized);
        CHECK(psp13_hdcp_invoke(&p4, 1) == RDNA3_EINVAL);
        dev.amdgpu_exact = 0;
        free(ta_file);
    }

    /* rdna3amdgpu=1: GMC as gmc_v11_0 - GART placed HIGH (host log
     * 0x7FFF00000000), the engines amdgpu_gmc_allocate_vm_inv_eng gives the
     * card's rings (host log), gfxhub / mmhub gart_disable at hw_fini */
    {
        static rdna3_gmc g2;
        static const uint32_t hubs[15] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0 };
        static const uint32_t want[15] = { 0, 1, 4, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1, 4, 14 };
        uint32_t eng[15];
        rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);
        dev.amdgpu_exact = 1;
        CHECK(gmc11_init(&dev, &g2, 0) == RDNA3_OK && g2.gart_start == 0x00007FFF00000000ull &&
              g2.gart_end == 0x00007FFF1FFFFFFFull);
        CHECK(gmc11_allocate_vm_inv_eng(hubs, NULL, 15, 1, eng) == RDNA3_OK &&
              !memcmp(eng, want, sizeof(want)));
        CHECK(gmc11_allocate_vm_inv_eng(hubs, NULL, 15, 0, eng) == RDNA3_OK && eng[3] == 5);
        CHECK(gfxhub303_get_mc_fb_offset(&dev) == ((uint64_t)S.reg[GC_BASE0 + regGCMC_VM_FB_OFFSET] << 24));
        S.reg[GC_BASE0 + regGCVM_CONTEXT1_CNTL] = 1;
        S.reg[GC_BASE0 + regGCVM_L2_CNTL] |= 1;
        S.reg[MM_BASE0 + regMMVM_L2_CNTL] |= MMVM_L2_CNTL__ENABLE_L2_CACHE_MASK;
        gfxhub303_gart_disable(&dev);
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT1_CNTL] == 0 && !(S.reg[GC_BASE0 + regGCVM_L2_CNTL] & 1) &&
              !(S.reg[GC_BASE0 + regGCMC_VM_MX_L1_TLB_CNTL] & 0x41) && S.reg[GC_BASE0 + regGCVM_L2_CNTL3] == 0);
        /* IH as ih_v6_0 runs it: rings 0 and 1 in system memory through
         * the GART, MC_SNOOP, rptr doorbells, page faults on ring 1 */
        {
            static rdna3_ih ih0, ih1;
            const uint64_t pf = RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE | RD_PTE_WRITEABLE;
            struct sim_seen seen = { 0, 0, 0, 0, 0, 0 };
            uint32_t k3, cntl, e1, bif;
            int db_saved = dev.doorbell_ok;
            CHECK(gmc11_gart_enable(&dev, &g2) == RDNA3_OK);
            for (k3 = 0; k3 < 4; k3++)
                CHECK(gmc11_gart_map(&dev, &g2, k3 * 4096ull, SYS_PHYS + 0x80000 + k3 * 4096ull, 1, pf) == RDNA3_OK);
            dev.doorbell_ok = 1;
            CHECK(ih60_init_amdgpu(&dev, &ih0, &ih1, S.sysmem + 0x80000, g2.gart_start, S.sysmem + 0x82000,
                                   g2.gart_start + 0x2000, 4096, g2.dummy_page_pa, 1) == RDNA3_OK);
            cntl = S.reg[OSS_BASE0 + regIH_RB_CNTL];
            CHECK((cntl & IH_RB_CNTL__MC_SNOOP_MASK) && (cntl & IH_RB_CNTL__RPTR_REARM_MASK) &&
                  ((cntl & IH_RB_CNTL__MC_SPACE_MASK) >> IH_RB_CNTL__MC_SPACE__SHIFT) == 4 &&
                  (cntl & IH_RB_CNTL__ENABLE_INTR_MASK));
            cntl = S.reg[OSS_BASE0 + SIM_IH_RB_CNTL_RING1];
            CHECK((cntl & IH_RB_CNTL__RB_ENABLE_MASK) && (cntl & IH_RB_CNTL__RB_FULL_DRAIN_ENABLE_MASK) &&
                  !(cntl & IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK) && !(cntl & IH_RB_CNTL__ENABLE_INTR_MASK));
            bif = rdna3_rreg(&dev, rdna3_reg_offset(&dev, HWIP_NBIO, 0, regBIF_BX0_INTERRUPT_CNTL_BASE_IDX,
                                                     regBIF_BX0_INTERRUPT_CNTL));
            CHECK(!(bif & BIF_BX0_INTERRUPT_CNTL__IH_REQ_NONSNOOP_EN_MASK));
            CHECK(S.reg[OSS_BASE0 + regIH_DOORBELL_RPTR] == (RD_DB_INDEX(RD_DB_IH) | IH_DOORBELL_RPTR__ENABLE_MASK));
            CHECK(S.reg[OSS_BASE0 + SIM_IH_DOORBELL_RPTR_RING1] ==
                  (RD_DB_INDEX(RD_DB_IH + 1) | IH_DOORBELL_RPTR__ENABLE_MASK));
            e1 = smn_val(regS2A_DOORBELL_ENTRY_1_CTRL);
            CHECK((e1 & S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_ENABLE_MASK) &&
                  ((e1 & S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_OFFSET_MASK) >>
                   S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_OFFSET__SHIFT) == RD_DB_INDEX(RD_DB_IH) &&
                  ((e1 & S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_SIZE_MASK) >>
                   S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_SIZE__SHIFT) == 2);
            CHECK(S.reg[OSS_BASE0 + regIH_RING1_CLIENT_CFG_DATA] ==
                  (0xau | IH_RING1_CLIENT_CFG_DATA__SOURCE_ID_MATCH_ENABLE_MASK));
            /* an end-of-pipe on ring 0, a GFX page fault redirected to ring 1 */
            S.ih_snooped = S.ih_ring1_ivs = S.ih_db_rptr = S.ih_db_unrouted = 0;
            ih_push(RD_IH_CLIENT_GRBM_CP, RD_IH_SRC_CP_EOP, 0);
            ih_push_iv(RD_IH_CLIENT_GFX, RD_IH_SRC_VM_FAULT, 0, 3, 7, 0x11, 0x22);
            CHECK(S.ih_snooped == 3 && S.ih_ring1_ivs == 1);
            CHECK(ih60_process(&dev, &ih0, sim_iv_cb, &seen) == 3);
            CHECK(seen.n == 2 && ih0.self_irqs == 1 && seen.client == RD_IH_CLIENT_GFX && seen.vmid == 3 &&
                  seen.pasid == 7);
            CHECK(S.reg[OSS_BASE0 + regIH_RB_RPTR] == 64 && S.reg[OSS_BASE0 + SIM_IH_RB_RPTR_RING1] == 32 &&
                  S.ih_db_rptr == 2 && S.ih_db_unrouted == 0);
            CHECK(ih60_process(&dev, &ih0, sim_iv_cb, &seen) == 0);
            ih60_disable(&dev, &ih0);
            CHECK(!ih0.enabled && !ih1.enabled && !(S.reg[OSS_BASE0 + regIH_RB_CNTL] & IH_RB_CNTL__RB_ENABLE_MASK) &&
                  !(S.reg[OSS_BASE0 + SIM_IH_RB_CNTL_RING1] & IH_RB_CNTL__RB_ENABLE_MASK));
            dev.doorbell_ok = db_saved;
        }
        /* sdma_v6_0_hw_fini: context-empty interrupts off, both instances
         * stopped and halted */
        for (uint32_t si = 0; si < 2; si++) {
            S.reg[GC_BASE0 + regSDMA0_CNTL + si * 0x600] |= SDMA0_CNTL__CTXEMPTY_INT_ENABLE_MASK;
            S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_CNTL + si * 0x600] |= SDMA0_QUEUE0_RB_CNTL__RB_ENABLE_MASK;
            S.reg[GCR(regSDMA0_F32_CNTL) + si * 0x20] &= ~SDMA0_F32_CNTL__HALT_MASK;
        }
        sdma60_hw_fini(&dev, 2);
        for (uint32_t si = 0; si < 2; si++) {
            CHECK(!(S.reg[GC_BASE0 + regSDMA0_CNTL + si * 0x600] & SDMA0_CNTL__CTXEMPTY_INT_ENABLE_MASK));
            CHECK(!(S.reg[GC_BASE0 + regSDMA0_QUEUE0_RB_CNTL + si * 0x600] & SDMA0_QUEUE0_RB_CNTL__RB_ENABLE_MASK));
            CHECK(S.reg[GCR(regSDMA0_F32_CNTL) + si * 0x20] & SDMA0_F32_CNTL__HALT_MASK);
        }
        gmc11_hw_fini(&dev);
        CHECK(!(S.reg[MM_BASE0 + regMMVM_L2_CNTL] & MMVM_L2_CNTL__ENABLE_L2_CACHE_MASK));
        dev.amdgpu_exact = 0;
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
