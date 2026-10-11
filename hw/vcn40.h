/*
 * vcn40.h - VCN 4.0 (video decode / encode, unified queue) and JPEG 4.0,
 * ported from Linux amdgpu vcn_v4_0.c, amdgpu_vcn.c, vcn_v2_0.c (the
 * unified ring's packet emitters), jpeg_v4_0.c, jpeg_v2_0.c (the JRBC
 * packet emitters and IB check) and amdgpu_jpeg.c (MIT).
 *
 * Navi 32 (GC 11.0.3) has two VCN 4.0.0 instances and one JPEG 4.0.0
 * decoder that lives in VCN instance 0's register space. Each VCN
 * instance has one "unified" ring (decode and encode share it, the IBs
 * name the engine) at doorbell (vcn_ring0_1 << 1) + 2 + 8 * inst; the
 * JPEG ring sits at (vcn_ring0_1 << 1) + 1.
 *
 * Bring-up shape:
 *  - the firmware is the PSP's (LOAD_IP_FW GFX_FW_TYPE_VCN / VCN1): cache
 *    window 0 points into the TMR at the address the PSP answered with,
 *    windows 1/2 (stack, context) and the non-cached fw_shared page live
 *    in one VRAM block per instance (amdgpu's vcpu_bo);
 *  - rings, write-pointer shadow and fence slots in VRAM by MC address
 *    (VMID 0, MMHUB), like the SDMA ring;
 *  - the non-DPG start path (vcn_v4_0_start, static power gating through
 *    UVD_PGFSM_CONFIG). soc21 sets AMD_PG_SUPPORT_VCN_DPG for GC 11.0.3;
 *    dynamic power gating (the DPG start / stop, indirect SRAM through
 *    the PSP) is not ported yet, so the flag is masked like amdgpu's
 *    pg_mask module parameter would;
 *  - VCN and JPEG are powered on demand (begin_use ungates, the idle
 *    handler gates again) and the PMFW is told through PowerUpVcn /
 *    PowerDownVcn / PowerUpJpeg / PowerDownJpeg (smu_dpm_set_vcn_enable).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_VCN40_H
#define RDNA3_VCN40_H

#include "rdna3_hw.h"
#include "gmc11.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_VCN_MAX_INST          2
#define RD_VCN_STACK_SIZE        (128 * 1024)       /* AMDGPU_VCN_STACK_SIZE */
#define RD_VCN_CONTEXT_SIZE      (512 * 1024)       /* AMDGPU_VCN_CONTEXT_SIZE */
#define RD_VCN_FW_SHARED_SIZE    4096               /* GPU_PAGE_ALIGN(sizeof(amdgpu_vcn4_fw_shared)) */
/* amdgpu_ring_init(ring, 512 dw): 512 * 4 * amdgpu_sched_hw_submission (2) */
#define RD_VCN_RING_SIZE         4096
#define RD_JPEG_RING_SIZE        4096
#define RD_VCN_IDLE_TIMEOUT_MS   1000               /* VCN_IDLE_TIMEOUT / JPEG_IDLE_TIMEOUT */

/* amdgpu_vcn.h: the unified (encode) ring's commands */
#define VCN_ENC_CMD_NO_OP        0x00000000
#define VCN_ENC_CMD_END          0x00000001
#define VCN_ENC_CMD_IB           0x00000002
#define VCN_ENC_CMD_FENCE        0x00000003
#define VCN_ENC_CMD_TRAP         0x00000004
#define VCN_ENC_CMD_REG_WRITE    0x0000000b
#define VCN_ENC_CMD_REG_WAIT     0x0000000c

/* amdgpu_vcn.h: fw_shared flags and the decode-buffer IB package */
#define RD_FW_SHARED_FLAG_0_UNIFIED_QUEUE   (1u << 2)
#define RD_VCN_SMU_DPM_INTERFACE_FLAG       (1u << 11)
#define RD_VCN_SMU_DPM_INTERFACE_DGPU       0
#define RD_FW_QUEUE_RING_RESET              1u
#define RD_FW_QUEUE_DPG_HOLD_OFF            2u
#define RD_VCN_IB_FLAG_DECODE_BUFFER        0x00000001
#define RD_VCN_CMD_FLAG_MSG_BUFFER          0x00000001
#define RD_VCN_DECODE_BUFFER_DW             33       /* struct amdgpu_vcn_decode_buffer */
/* struct amdgpu_vcn4_fw_shared byte offsets */
#define RD_VCN4_FWS_PRESENT_FLAG_0          0
#define RD_VCN4_FWS_SQ_IS_ENABLED           16
#define RD_VCN4_FWS_SQ_QUEUE_MODE           17
#define RD_VCN4_FWS_SMU_DPM_INTERFACE       120
#define RD_VCN4_FWS_SIZE                    148

/* amdgpu_vcn.h engine_status_constants */
#define RD_UVD_PGFSM_STATUS__UVDM_UVDU_UVDLM_PWR_ON_3_0 0x2A2A8AA0u
#define RD_UVD_PGFSM_CONFIG__UVDM_UVDU_PWR_ON           0x00000002u
#define RD_UVD_STATUS__UVD_BUSY                         0x00000004u
#define RD_UVD_STATUS__IDLE                             0x2u
#define RD_UVD_POWER_STATUS__UVD_POWER_STATUS_TILES_OFF 0x1u
#define RD_UVD_PGFSM_STATUS_UVDJ_PWR_ON                 0u

/* soc15d.h: JPEG ring packets */
#define PACKETJ_CONDITION_CHECK0 0
#define PACKETJ_CONDITION_CHECK3 3
#define PACKETJ_TYPE0            0
#define PACKETJ_TYPE2            2
#define PACKETJ_TYPE3            3
#define PACKETJ_TYPE4            4
#define PACKETJ_TYPE6            6
#define PACKETJ_TYPE7            7
#define PACKETJ(reg, r, cond, type) (((uint32_t)(reg) & 0x3FFFF) | (((uint32_t)(r) & 0x3F) << 18) | \
                                     (((uint32_t)(cond) & 0xF) << 24) | (((uint32_t)(type) & 0xF) << 28))
#define CP_PACKETJ_NOP           0x60000000u
#define CP_PACKETJ_GET_REG(x)    ((x) & 0x3FFFF)
#define CP_PACKETJ_GET_RES(x)    (((x) >> 18) & 0x3F)
#define CP_PACKETJ_GET_COND(x)   (((x) >> 24) & 0xF)
#define CP_PACKETJ_GET_TYPE(x)   (((x) >> 28) & 0xF)
#define RD_PACKET0(reg, n)       (((uint32_t)(reg) & 0xFFFF) | (((uint32_t)(n) & 0x3FFF) << 16))

/* jpeg_v2_0.h: the JRBC's internal register offsets */
#define mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET               0x1bfff
#define mmUVD_JPEG_GPCOM_CMD_INTERNAL_OFFSET                  0x4029
#define mmUVD_JPEG_GPCOM_DATA0_INTERNAL_OFFSET                0x402a
#define mmUVD_JPEG_GPCOM_DATA1_INTERNAL_OFFSET                0x402b
#define mmUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_LOW_INTERNAL_OFFSET  0x40ea
#define mmUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_HIGH_INTERNAL_OFFSET 0x40eb
#define mmUVD_LMI_JRBC_IB_VMID_INTERNAL_OFFSET                0x40cf
#define mmUVD_LMI_JPEG_VMID_INTERNAL_OFFSET                   0x40d1
#define mmUVD_LMI_JRBC_IB_64BIT_BAR_LOW_INTERNAL_OFFSET       0x40e8
#define mmUVD_LMI_JRBC_IB_64BIT_BAR_HIGH_INTERNAL_OFFSET      0x40e9
#define mmUVD_JRBC_IB_SIZE_INTERNAL_OFFSET                    0x4082
#define mmUVD_LMI_JRBC_RB_MEM_RD_64BIT_BAR_LOW_INTERNAL_OFFSET  0x40ec
#define mmUVD_LMI_JRBC_RB_MEM_RD_64BIT_BAR_HIGH_INTERNAL_OFFSET 0x40ed
#define mmUVD_JRBC_RB_COND_RD_TIMER_INTERNAL_OFFSET           0x4085
#define mmUVD_JRBC_RB_REF_DATA_INTERNAL_OFFSET                0x4084
#define mmUVD_JRBC_STATUS_INTERNAL_OFFSET                     0x4089
#define mmUVD_JPEG_PITCH_INTERNAL_OFFSET                      0x401f
#define mmUVD_JPEG_IH_CTRL_INTERNAL_OFFSET                    0x4149
#define JRBC_DEC_EXTERNAL_REG_WRITE_ADDR                      0x18000
#define JPEG_REG_RANGE_START                                  0x4000
#define JPEG_REG_RANGE_END                                    0x41c2
/* jpeg_v4_0.c */
#define regUVD_JPEG_PITCH_INTERNAL_OFFSET                     0x401f

/* soc15_ih_clientid.h / irqsrcs_vcn_4_0.h */
#define RD_IH_CLIENTID_VCN                 0x10
#define RD_IH_CLIENTID_VCN1                0x0e
#define VCN_4_0__SRCID__UVD_ENC_GENERAL_PURPOSE 119
#define VCN_4_0__SRCID__JPEG_DECODE        153

/* amdgpu_doorbell.h AMDGPU_NAVI10_DOORBELL64_VCN0_1 (soc21: vcn.vcn_ring0_1) */
#define RD_DB_VCN0_1                       0x188

/* cg / pg flags (amd_shared.h AMD_CG_SUPPORT_* / AMD_PG_SUPPORT_*, the VCN
 * and JPEG ones soc21_common_early_init sets for GC 11.0.3) */
#define RD_CG_VCN_MGCG   (1u << 0)
#define RD_CG_JPEG_MGCG  (1u << 1)
#define RD_PG_VCN        (1u << 0)
#define RD_PG_VCN_DPG    (1u << 1)
#define RD_PG_JPEG       (1u << 2)

/* amd_powergating_state */
enum { RD_PG_STATE_GATE = 0, RD_PG_STATE_UNGATE = 1 };

/* a VCN unified ring or the JPEG decode ring (struct amdgpu_ring) */
typedef struct rdna3_vcn_ring {
    uint32_t me;            /* VCN instance (JPEG: 0) */
    int      jpeg;          /* 1: the JRBC ring */
    uint64_t ring;          /* VRAM offset */
    uint64_t wb;            /* VRAM, 4 KiB: wptr shadow @0, fence @8, test @16 */
    uint32_t ring_size;     /* bytes */
    uint32_t buf_mask;      /* dwords */
    uint32_t align_mask;    /* funcs->align_mask */
    uint64_t wptr;          /* dwords */
    uint64_t gpu_addr;      /* MC address of the ring */
    uint32_t fence_seq;     /* last emitted */
    int      ready;         /* ring->sched.ready */
    int      use_doorbell;
    uint32_t doorbell_index;
    uint64_t emitted;       /* fences emitted, for the idle handler */
} rdna3_vcn_ring;

#define RD_VCN_WB_WPTR   0
#define RD_VCN_WB_FENCE  8
#define RD_VCN_WB_TEST   16

/* JPEG IBs run from a kernel copy (parse_cs rings: amdgpu_cs_patch_ibs
 * copies the checked IB into its own pool, the JRBC fetches it in VMID 0) */
#define RD_JPEG_IB_SLOTS     16
#define RD_JPEG_IB_SLOT_SIZE (16 * 1024)

typedef struct rdna3_vcn {
    /* amdgpu_vcn_early_init / vcn_v4_0_early_init */
    uint32_t num_vcn_inst;            /* adev->vcn.num_vcn_inst */
    uint32_t harvest_config;          /* AMDGPU_VCN_HARVEST_VCNn */
    uint32_t num_enc_rings;           /* 1: the enc ring is the unified ring */
    uint32_t fw_version;              /* adev->vcn.fw_version (AMDGPU_INFO_FW_VCN) */
    uint32_t fw_ucode_size;           /* common header ucode_size_bytes */
    uint32_t enc_major, enc_minor, dec_ver, vep, fw_rev;   /* the new naming convention */
    uint32_t cg_flags, pg_flags;      /* VCN / JPEG part of adev->cg_flags / pg_flags */
    uint32_t gb_addr_config;          /* adev->gfx.config.gb_addr_config */
    int      dpm_enabled;             /* adev->pm.dpm_enabled: tell the PMFW */
    /* firmware.ucode[AMDGPU_UCODE_ID_VCN + i].tmr_mc_addr (LOAD_IP_FW answer) */
    uint64_t tmr_mc[RD_VCN_MAX_INST];
    /* amdgpu_vcn_sw_init: vcpu_bo (stack, context, fw_shared) per instance */
    uint64_t vcpu_bo[RD_VCN_MAX_INST];
    uint64_t fw_shared[RD_VCN_MAX_INST];
    int      sw_ready;
    rdna3_vcn_ring ring_enc[RD_VCN_MAX_INST];
    int      cur_state;               /* adev->vcn.cur_state */
    int      vcn_gated;               /* smu power_gate.vcn_gated */
    uint32_t total_submission_cnt;
    /* JPEG 4.0 */
    rdna3_vcn_ring jpeg;
    int      jpeg_present;
    int      jpeg_cur_state;
    int      jpeg_gated;
    uint32_t jpeg_total_submission_cnt;
    uint64_t jpeg_ib_pool;            /* VRAM, RD_JPEG_IB_SLOTS slots */
    uint32_t jpeg_ib_seq[RD_JPEG_IB_SLOTS];
    uint32_t jpeg_ib_next;
    uint64_t ib_test_buf;             /* VRAM for the unified ring's IB tests */
    /* counters */
    uint32_t starts, stops, jpeg_starts, jpeg_stops, irqs, jpeg_irqs, bad_irqs;
    int      last_err;
} rdna3_vcn;

/* ---- VCN 4.0 ---- */
/* vcn_v4_0_early_init + amdgpu_vcn_early_init: instances from discovery,
 * the firmware header (vcn_4_0_0.bin, the PSP loaded the image) */
int  vcn40_early_init(rdna3_dev *d, rdna3_vcn *v, const uint8_t *fw, size_t fw_len,
                      uint32_t gb_addr_config, int dpm_enabled);
/* vcn_v4_0_sw_init: amdgpu_vcn_sw_init (vcpu_bo), amdgpu_vcn_resume, the
 * unified rings (amdgpu_ring_init), vcn_v4_0_fw_shared_init. tmr_mc: the
 * firmware's TMR address per instance (psp13 LOAD_IP_FW response) */
int  vcn40_sw_init(rdna3_dev *d, rdna3_vcn *v, const uint64_t tmr_mc[RD_VCN_MAX_INST]);
/* vcn_v4_0_hw_init: doorbell ranges and the ring test of every instance
 * (which powers the block up through begin_use) */
int  vcn40_hw_init(rdna3_dev *d, rdna3_vcn *v);
/* vcn_v4_0_hw_fini: gate the block */
int  vcn40_hw_fini(rdna3_dev *d, rdna3_vcn *v);
int  vcn40_set_powergating_state(rdna3_dev *d, rdna3_vcn *v, int state);
int  vcn40_set_clockgating_state(rdna3_dev *d, rdna3_vcn *v, int gate);
int  vcn40_is_idle(rdna3_dev *d, rdna3_vcn *v);
int  vcn40_wait_for_idle(rdna3_dev *d, rdna3_vcn *v);
/* amdgpu_vcn_ring_begin_use / end_use around every submission, and the
 * idle work: gate when nothing is left (call RD_VCN_IDLE_TIMEOUT_MS after
 * the last end_use; returns 1 when it gated, 0 when work is still pending) */
void vcn40_ring_begin_use(rdna3_dev *d, rdna3_vcn *v);
void vcn40_ring_end_use(rdna3_dev *d, rdna3_vcn *v);
int  vcn40_idle_work_handler(rdna3_dev *d, rdna3_vcn *v);

/* the unified ring (vcn_v4_0_unified_ring_vm_funcs) */
uint64_t vcn40_unified_ring_get_rptr(rdna3_dev *d, rdna3_vcn_ring *r);
uint64_t vcn40_unified_ring_get_wptr(rdna3_dev *d, rdna3_vcn_ring *r);
void vcn40_unified_ring_set_wptr(rdna3_dev *d, rdna3_vcn_ring *r);
/* amdgpu_ring_alloc (room for 'ndw', waits for the engine), write, commit
 * (pads to align_mask with the ring's NOP, then set_wptr) */
int  vcn40_ring_alloc(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t ndw);
void vcn40_ring_write(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t v);
void vcn40_ring_commit(rdna3_dev *d, rdna3_vcn_ring *r);
/* vcn_v2_0_enc_ring_emit_* (shared by VCN 2.0 .. 4.0) */
void vcn40_enc_ring_emit_ib(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t vmid, uint64_t va, uint32_t ndw);
void vcn40_enc_ring_emit_fence(rdna3_dev *d, rdna3_vcn_ring *r, uint64_t addr, uint32_t seq);
void vcn40_enc_ring_insert_end(rdna3_dev *d, rdna3_vcn_ring *r);
void vcn40_enc_ring_emit_reg_wait(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val, uint32_t mask);
void vcn40_enc_ring_emit_wreg(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val);
/* page-table switch + invalidation on the ring (amdgpu_gmc_emit_flush_gpu_tlb
 * through emit_wreg / emit_reg_write_reg_wait, then the PTB wait) */
void vcn40_enc_ring_emit_vm_flush(rdna3_dev *d, rdna3_vcn_ring *r, const rdna3_vm_flush_regs *f,
                                  uint64_t pd_addr);
/* ring tests: amdgpu_vcn_enc_ring_test_ring, amdgpu_vcn_unified_ring_test_ib */
int  vcn40_enc_ring_test_ring(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r);
int  vcn40_unified_ring_test_ib(rdna3_dev *d, rdna3_vcn *v, rdna3_vcn_ring *r);

/* fences of a ring (VCN or JPEG) */
int  vcn40_fence_passed(rdna3_dev *d, const rdna3_vcn_ring *r, uint32_t seq);
int  vcn40_wait_fence(rdna3_dev *d, const rdna3_vcn_ring *r, uint32_t seq);
/* a user job (amdgpu_ib_schedule on the unified ring): begin_use, the IBs in
 * 'vmid', an optional 32-bit user fence at MC 'uf_mc', the ring fence with
 * its trap, END, commit, end_use. Returns the fence sequence (0: failed). */
typedef struct rdna3_vcn_ib {
    uint64_t va;              /* GPU VA in 'vmid' (JPEG: ignored when kdata is set) */
    uint32_t length_dw;
    const uint32_t *kdata;    /* JPEG: the checked copy (parse_cs) the JRBC runs */
} rdna3_vcn_ib;
uint32_t vcn40_submit(rdna3_dev *d, rdna3_vcn *v, uint32_t inst, uint32_t vmid, const rdna3_vcn_ib *ibs,
                      uint32_t n, uint64_t uf_mc);
/* vcn_v4_0_process_interrupt: 1 when the IV is a VCN fence trap (*inst set) */
int  vcn40_process_interrupt(rdna3_vcn *v, uint32_t client_id, uint32_t src_id, uint32_t *inst);

/* ---- JPEG 4.0 ---- */
/* jpeg_v4_0_early_init / sw_init (amdgpu_jpeg_sw_init, the ring) / hw_init
 * (doorbell, ring test) */
int  jpeg40_early_init(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_sw_init(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_hw_init(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_hw_fini(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_set_powergating_state(rdna3_dev *d, rdna3_vcn *v, int state);
int  jpeg40_set_clockgating_state(rdna3_dev *d, rdna3_vcn *v, int gate);
int  jpeg40_is_idle(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_wait_for_idle(rdna3_dev *d, rdna3_vcn *v);
void jpeg40_ring_begin_use(rdna3_dev *d, rdna3_vcn *v);
void jpeg40_ring_end_use(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_idle_work_handler(rdna3_dev *d, rdna3_vcn *v);
uint64_t jpeg40_dec_ring_get_rptr(rdna3_dev *d, rdna3_vcn_ring *r);
uint64_t jpeg40_dec_ring_get_wptr(rdna3_dev *d, rdna3_vcn_ring *r);
void jpeg40_dec_ring_set_wptr(rdna3_dev *d, rdna3_vcn_ring *r);
/* jpeg_v2_0_dec_ring_* (shared by JPEG 2.0 .. 4.0) */
void jpeg40_dec_ring_insert_start(rdna3_dev *d, rdna3_vcn_ring *r);
void jpeg40_dec_ring_insert_end(rdna3_dev *d, rdna3_vcn_ring *r);
void jpeg40_dec_ring_emit_fence(rdna3_dev *d, rdna3_vcn_ring *r, uint64_t addr, uint32_t seq);
/* ib_vmid: the VMID the JRBC fetches the IB in (0 for the kernel copy of a
 * parse_cs ring), vmid: the job's VMID for the JPEG engine's own accesses */
void jpeg40_dec_ring_emit_ib(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t vmid, uint32_t ib_vmid,
                             uint64_t addr, uint32_t ndw);
void jpeg40_dec_ring_emit_reg_wait(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val, uint32_t mask);
void jpeg40_dec_ring_emit_wreg(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t reg, uint32_t val);
void jpeg40_dec_ring_emit_vm_flush(rdna3_dev *d, rdna3_vcn_ring *r, const rdna3_vm_flush_regs *f,
                                   uint64_t pd_addr);
void jpeg40_dec_ring_nop(rdna3_dev *d, rdna3_vcn_ring *r, uint32_t count);
int  jpeg40_dec_ring_test_ring(rdna3_dev *d, rdna3_vcn *v);
int  jpeg40_dec_ring_test_ib(rdna3_dev *d, rdna3_vcn *v);
uint32_t jpeg40_submit(rdna3_dev *d, rdna3_vcn *v, uint32_t vmid, const rdna3_vcn_ib *ibs, uint32_t n,
                       uint64_t uf_mc);
/* jpeg_v4_0_process_interrupt: 1 when the IV is the JPEG decode trap */
int  jpeg40_process_interrupt(rdna3_vcn *v, uint32_t client_id, uint32_t src_id);

/* PMFW messages of the VCN / JPEG power gating, in smu13.c:
 * smu_v13_0_set_vcn_enable (PowerUpVcn / PowerDownVcn, param inst << 16,
 * every instance not in harvest_config) and smu_v13_0_set_jpeg_enable */
int smu13_set_vcn_enable(rdna3_dev *d, uint32_t num_inst, uint32_t harvest_config, int enable);
int smu13_set_jpeg_enable(rdna3_dev *d, int enable);

#ifdef __cplusplus
}
#endif
#endif
