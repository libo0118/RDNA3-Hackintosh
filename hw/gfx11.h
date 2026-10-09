/*
 * gfx11.h - GFX 11 (GC 11.0.x) bring-up and the kernel GFX ring, ported
 * from Linux amdgpu gfx_v11_0.c (MIT) for the PSP firmware-load path:
 *
 *   wait_for_rlc_autoload_complete -> config_gfx_rs64 -> gfxhub_enable ->
 *   golden registers -> constants_init -> rlc_resume (CSB + SRM) ->
 *   cp_gfx_resume (RB0 on ME0 pipe 0) -> cp_gfx_start (clear state) ->
 *   ring test (SET_UCONFIG_REG SCRATCH_REG0)
 *
 * Like amdgpu with amdgpu_mes=0 and amdgpu_async_gfx_ring=0: the gfx ring
 * is programmed through CP_RB0_* registers, no MES; compute queues are
 * MEC HQDs mapped by the KIQ (gfx_v11_0_kiq_*, MAP_QUEUES). Ring, rptr
 * writeback, wptr poll slot, fence slot, MQDs and the clear-state buffer
 * live in VRAM (MC addresses); the write pointer goes through the doorbell
 * when the aperture is up (doorbell.h), else CP_RB0_WPTR (gfx only: a MEC
 * queue needs its doorbell).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GFX11_H
#define RDNA3_GFX11_H

#include "rdna3_hw.h"
#include "gmc11.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nvd.h */
#define PM4_TYPE3 3u
#define PACKET3(op, n) ((PM4_TYPE3 << 30) | (((op) & 0xFFu) << 8) | (((n) & 0x3FFFu) << 16))
#define PACKET3_NOP                0x10
#define PACKET3_CLEAR_STATE        0x12
#define PACKET3_CONTEXT_CONTROL    0x28
#define PACKET3_WRITE_DATA         0x37
#define PACKET3_INDIRECT_BUFFER    0x3F
#define PACKET3_RELEASE_MEM        0x49
#define PACKET3_PREAMBLE_CNTL      0x4A
#define PACKET3_WAIT_REG_MEM       0x3C
#define PACKET3_PFP_SYNC_ME        0x42
#define PACKET3_ACQUIRE_MEM        0x58
#define PACKET3_FRAME_CONTROL      0x90
#define PACKET3_SET_CONTEXT_REG    0x69
#define PACKET3_SET_CONTEXT_REG_START 0x0000a000
#define PACKET3_SET_UCONFIG_REG    0x79
#define PACKET3_SET_RESOURCES      0xA0
#define PACKET3_MAP_QUEUES         0xA2
#define PACKET3_UNMAP_QUEUES       0xA3
#define INDIRECT_BUFFER_VALID      (1u << 23)
#define PACKET3_SET_UCONFIG_REG_START 0x0000c000
#define WRITE_DATA_DST_SEL(x)      ((uint32_t)(x) << 8)
#define WR_CONFIRM                 (1u << 20)
#define WRITE_DATA_ENGINE_SEL(x)   ((uint32_t)(x) << 30)
#define WAIT_REG_MEM_FUNCTION(x)   ((uint32_t)(x) << 0)
#define WAIT_REG_MEM_MEM_SPACE(x)  ((uint32_t)(x) << 4)
#define WAIT_REG_MEM_OPERATION(x)  ((uint32_t)(x) << 6)
#define WAIT_REG_MEM_ENGINE(x)     ((uint32_t)(x) << 8)
#define FRAME_TMZ                  (1u << 0)
#define FRAME_CMD(x)               ((uint32_t)(x) << 28)
#define PACKET3_PREAMBLE_BEGIN_CLEAR_STATE (2u << 28)
#define PACKET3_PREAMBLE_END_CLEAR_STATE   (3u << 28)

#define RD_GFX_RING_SIZE (256 * 1024)   /* bytes */

/* RS64 program start addresses from the gfx_firmware_header_v2_0 of the
 * PFP / ME / MEC blobs (gfx_v11_0_config_gfx_rs64) */
typedef struct rdna3_rs64_start {
    uint32_t pfp_lo, pfp_hi, me_lo, me_hi, mec_lo, mec_hi;
} rdna3_rs64_start;

/* which CP engine runs the ring: packet flavours differ (emit_wreg's
 * ENGINE_SEL, PFP sync, HDP flush mask) */
enum { RD_RING_GFX = 0, RD_RING_COMPUTE = 1, RD_RING_KIQ = 2 };

typedef struct rdna3_gfx_ring {
    uint32_t kind, pipe;    /* RD_RING_*; pipe for the HDP flush mask */
    int      use_doorbell;
    uint32_t doorbell;      /* RD_DB_INDEX(...) */
    uint64_t ring;          /* VRAM offsets */
    uint64_t wb;            /* 4 KiB: rptr @0, wptr poll @8, fence @16, test @24 */
    uint64_t csb;           /* clear-state buffer */
    uint32_t csb_dw;
    uint32_t ring_size, buf_mask;   /* bytes, dwords-1 */
    uint64_t wptr;          /* dwords */
    uint32_t fence_seq;
    int      ready;
    uint64_t last_ctx;      /* amdgpu ring->current_ctx: context of the last job */
    uint32_t last_vmid;     /* VMID of the last job (whose waves a recovery kills) */
    int      fence_irq;     /* fences use RELEASE_MEM INT_SEL 2 (EOP interrupt) */
} rdna3_gfx_ring;

/* one IB of a job (VA in the job's VMID) */
typedef struct rdna3_gfx_ib {
    uint64_t va;
    uint32_t ndw;
    uint32_t flags;         /* AMDGPU_IB_FLAG_* (only TMZ is looked at) */
} rdna3_gfx_ib;

#define RD_GFX_IB_FLAG_TMZ (1u << 4)   /* AMDGPU_IB_FLAGS_SECURE */
#define RD_GFX_VM_INV_ENG  0           /* first free GFXHUB engine (eng 17 = MMIO/GART) */
#define RD_GFX_MAX_IBS     16

#define RD_GFX_WB_RPTR  0
#define RD_GFX_WB_WPTR  8
#define RD_GFX_WB_FENCE 16

/* parse the RS64 start addresses (returns RDNA3_EINVAL for a non-v2 header) */
int  gfx11_rs64_from_fw(const void *pfp, uint32_t pfp_len, const void *me, uint32_t me_len,
                        const void *mec, uint32_t mec_len, rdna3_rs64_start *out);

/* gfx_v11_0_hw_init up to and including the gfx ring test */
int  gfx11_hw_init(rdna3_dev *d, rdna3_gmc *g, rdna3_gfx_ring *r, const rdna3_rs64_start *rs64,
                   uint32_t ring_size);
void gfx11_cp_gfx_enable(rdna3_dev *d, int enable);
int  gfx11_ring_test(rdna3_dev *d, rdna3_gfx_ring *r);

/* ring building */
void gfx11_write(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t v);
void gfx11_commit(rdna3_dev *d, rdna3_gfx_ring *r);
/* gfx_v11_0_ring_emit_ib_gfx: INDIRECT_BUFFER in 'vmid' */
void gfx11_emit_ib(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t vmid, uint64_t va, uint32_t ndw);
/* gfx_v11_0_ring_emit_fence: RELEASE_MEM with cache flush, ++fence_seq */
uint32_t gfx11_emit_fence(rdna3_dev *d, rdna3_gfx_ring *r);
/* building blocks of a job (gfx_v11_0_ring_emit_*) */
void gfx11_emit_wreg(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t reg, uint32_t val);
void gfx11_emit_wait_reg_mem(rdna3_dev *d, rdna3_gfx_ring *r, int eng_sel, int mem_space, int opt,
                             uint32_t addr0, uint32_t addr1, uint32_t ref, uint32_t mask,
                             uint32_t inv);
void gfx11_emit_vm_flush(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t vmid, uint64_t pd_addr);
void gfx11_emit_hdp_flush(rdna3_dev *d, rdna3_gfx_ring *r);
void gfx11_emit_mem_sync(rdna3_dev *d, rdna3_gfx_ring *r);
void gfx11_emit_cntxcntl(rdna3_dev *d, rdna3_gfx_ring *r, int ctx_switch);
void gfx11_emit_frame_cntl(rdna3_dev *d, rdna3_gfx_ring *r, int start, int secure);

/* free ring space in dwords (from the rptr writeback) */
uint32_t gfx11_ring_space(rdna3_dev *d, rdna3_gfx_ring *r);

/* amdgpu_ib_schedule() for the gfx ring, in amdgpu's order:
 *   [VM flush + PFP_SYNC_ME] -> ACQUIRE_MEM -> HDP flush -> CONTEXT_CONTROL ->
 *   FRAME_CONTROL begin -> IBs -> FRAME_CONTROL end -> RELEASE_MEM fence
 * 'ctx' identifies the submitter (a context switch reloads state);
 * 'pd_addr' != 0 reprograms VMID 'vmid' and invalidates its TLBs from the
 * ring. Waits for ring space; commits. Returns the fence seq, 0 on error. */
uint32_t gfx11_submit_job(rdna3_dev *d, rdna3_gfx_ring *r, uint64_t ctx, uint32_t vmid,
                          uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n);

/* CP_INT_CNTL_RING0 TIME_STAMP/GENERIC0 (gfx_v11_0_set_gfx_eop_interrupt_state
 * me 0 pipe 0); also switches the fences to interrupt-raising */
void gfx11_set_eop_interrupt(rdna3_dev *d, rdna3_gfx_ring *r, int enable);
int  gfx11_fence_passed(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq);
int  gfx11_wait_fence(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq);

/* ---- hang detection and recovery ----
 * amdgpu_job_timedout: dump the engine state, try to kill the hanging
 * job's waves (soft recovery), else reset. gfx11_soft_reset is
 * gfx_v11_0_soft_reset + cp_resume for the rings given; the heavier way out
 * is smu13_mode1_reset and a full re-init from level 4. */
typedef struct rdna3_gfx_status {
    uint32_t grbm_status, grbm_status2, grbm_status_se0;
    uint32_t cp_stat, cp_stalled_stat1, cp_cpc_status, cp_cpc_stalled_stat1, cp_cpf_status;
    uint32_t cp_me_cntl, rb0_rptr, rb0_wptr, rb0_rptr_wb;
    uint32_t cp_int_cntl, rlc_safe_mode;
    uint32_t sdma0_status;
} rdna3_gfx_status;
void gfx11_read_status(rdna3_dev *d, const rdna3_gfx_ring *r, rdna3_gfx_status *st);
void gfx11_log_status(rdna3_dev *d, const rdna3_gfx_status *st, const char *why);
/* GRBM_STATUS.GUI_ACTIVE == 0 (gfx_v11_0_is_idle) */
int  gfx11_is_idle(rdna3_dev *d);
/* gfx_v10_0_ring_soft_recovery: SQ_CMD KILL broadcast to the waves of 'vmid'
 * inside RLC safe mode; the job's fence may then land */
void gfx11_kill_waves(rdna3_dev *d, uint32_t vmid);
/* amdgpu_fence_driver_force_completion: the last emitted seq lands in the
 * fence slot so every waiter returns (used right before a reset) */
void gfx11_force_fence_completion(rdna3_dev *d, rdna3_gfx_ring *r);
/* gfx_v11_0_soft_reset (dequeue every HQD, CP_VMID_RESET, GRBM soft reset of
 * CP/GFX/CPF/CPC/CPG) followed by gfx_v11_0_cp_resume: the gfx ring, and the
 * KIQ + KCQ when given (NULL = not in use), come back with a ring test. */
struct rdna3_mec_ring;
int  gfx11_soft_reset(rdna3_dev *d, rdna3_gfx_ring *r, struct rdna3_mec_ring *kiq,
                      struct rdna3_mec_ring *kcq);

/* size of the clear-state buffer in dwords (gfx_v11_0_get_csb_size) and
 * its contents (gfx_v11_0_get_csb_buffer); exposed for tests */
uint32_t gfx11_csb_size(void);
uint32_t gfx11_csb_fill(uint32_t *buf, uint32_t max_dw);

/* ---- MEC queues: the KIQ and kernel compute queues (KCQ) ----
 * A MEC queue is an HQD (me 1, pipe, queue) described by a v11_compute_mqd
 * in VRAM; the KIQ is set up through its registers (gfx_v11_0_kiq_init_
 * register), every other queue by the KIQ's MAP_QUEUES packet. Both need
 * doorbells (gfx_v11_0_ring_set_wptr_compute). */
#define RD_MQD_DWORDS   512          /* sizeof(struct v11_compute_mqd) / 4 */
#define RD_MEC_HPD_SIZE 2048         /* GFX11_MEC_HPD_SIZE: EOP buffer per queue */

typedef struct rdna3_mec_ring {
    rdna3_gfx_ring rb;      /* kind COMPUTE or KIQ */
    uint32_t me, pipe, queue;
    uint64_t mqd, eop;      /* VRAM offsets */
    int      mapped;        /* HQD active (KIQ) / mapped by the KIQ (KCQ) */
} rdna3_mec_ring;

/* amdgpu_gfx_kiq_acquire / amdgpu_gfx_compute_queue_acquire for one MEC
 * with 4 pipes x 4 queues: the KIQ is pipe 1 queue 0, KCQ0 pipe 0 queue 0 */
#define RD_KIQ_PIPE  1
#define RD_KCQ0_PIPE 0

/* allocate ring, writeback, MQD and EOP; 'kiq' selects RD_RING_KIQ */
int  gfx11_mec_ring_init(rdna3_dev *d, rdna3_mec_ring *r, int kiq, uint32_t pipe, uint32_t queue,
                         uint32_t doorbell_assignment, uint32_t ring_size);
/* gfx_v11_0_cp_compute_enable + kiq_resume: KIQ HQD programmed, ring test */
int  gfx11_kiq_start(rdna3_dev *d, rdna3_mec_ring *kiq);
/* gfx_v11_0_kcq_resume + amdgpu_gfx_enable_kcq: MQD, SET_RESOURCES and
 * MAP_QUEUES on the KIQ, then the KCQ ring test */
int  gfx11_kcq_start(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq);
/* amdgpu_gfx_disable_kcq (UNMAP_QUEUES) + cp_compute_enable(false) */
void gfx11_mec_stop(rdna3_dev *d, rdna3_mec_ring *kiq, rdna3_mec_ring *kcq);
/* compute job: VM flush, ACQUIRE_MEM, HDP flush, IBs, fence (no PFP, no
 * frame control); returns the fence seq, 0 on error */
uint32_t gfx11_submit_compute_job(rdna3_dev *d, rdna3_mec_ring *kcq, uint64_t ctx, uint32_t vmid,
                                  uint64_t pd_addr, const rdna3_gfx_ib *ibs, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif
