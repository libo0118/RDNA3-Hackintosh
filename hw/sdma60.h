/*
 * sdma60.h - SDMA 6.0 (Navi 3x) kernel ring, ported from Linux amdgpu
 * sdma_v6_0.c (MIT): engine unhalt, QUEUE0 ring setup, ring test,
 * linear copy / constant fill / fence packets (sdma_v6_0_0_pkt_open.h).
 *
 * Bring-up shape: the ring, its rptr writeback, the wptr poll slot and the
 * fence slot all live in VRAM and are addressed by MC address (VMID0
 * system aperture), like the PSP and IH buffers. The write pointer goes
 * to the poll slot (F32_WPTR_POLL_ENABLE) and then through the doorbell
 * when the aperture is up (sdma_v6_0_ring_set_wptr), else to
 * QUEUE0_RB_WPTR. Firmware comes from the PSP load (SDMA_UCODE_TH0/TH1).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SDMA60_H
#define RDNA3_SDMA60_H

#include "rdna3_hw.h"
#include "amd_mqd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* sdma_v6_0_0_pkt_open.h */
#define SDMA_OP_NOP          0
#define SDMA_OP_COPY         1
#define SDMA_OP_WRITE        2
#define SDMA_OP_INDIRECT     4
#define SDMA_OP_FENCE        5
#define SDMA_OP_TRAP         6
#define SDMA_OP_CONST_FILL   11
#define SDMA_OP_POLL_REGMEM  8
#define SDMA_OP_COND_EXE     9
#define SDMA_OP_PTEPDE       12
#define SDMA_OP_SRBM_WRITE   14
#define SDMA_OP_GCR_REQ      17
#define SDMA_SUBOP_COPY_LINEAR  0
#define SDMA_SUBOP_WRITE_LINEAR 0
#define SDMA_SUBOP_VM_INVALIDATION 4
#define SDMA_PKT_HEADER_OP(x)        ((uint32_t)(x) & 0xff)
#define SDMA_PKT_HEADER_SUB_OP(x)    (((uint32_t)(x) & 0xff) << 8)
#define SDMA_PKT_FENCE_HEADER_MTYPE(x) (((uint32_t)(x) & 0x7) << 16)
#define SDMA_PKT_NOP_HEADER_COUNT(x) (((uint32_t)(x) & 0x3fff) << 16)
#define SDMA_PKT_INDIRECT_HEADER_VMID(x) (((uint32_t)(x) & 0xf) << 16)

#define RD_SDMA_RING_SIZE (256 * 1024)  /* sdma_v6_0_sw_init: 1024 * 256 */

typedef struct rdna3_sdma_ring {
    uint32_t inst;
    uint64_t ring;          /* VRAM offsets */
    uint64_t wb;            /* 4 KiB: rptr wb @0, wptr poll @8, fence @16, test @24 */
    uint32_t ring_size;     /* bytes, power of two */
    uint32_t buf_mask;      /* in dwords */
    uint64_t wptr;          /* dwords */
    uint32_t fence_seq;
    int      ready;
    int      fence_irq;     /* each fence also raises an IH trap */
    int      use_doorbell;  /* wptr goes through the doorbell (d->doorbell_ok at start) */
    uint32_t doorbell;      /* RD_DB_INDEX(RD_DB_SDMA_ENGINEn) */
    /* rdna3amdgpu=1: ring->vm_inv_eng (amdgpu_gmc_allocate_vm_inv_eng, 12
     * for sdma0 on this card), commits padded to the fetch size (align_mask
     * 0xf), and burst NOPs (amdgpu_sdma_init_microcode: firmware feature
     * version >= 20) */
    uint32_t vm_inv_eng;
    int      amdgpu_pad;
    int      burst_nop;
} rdna3_sdma_ring;

#define RD_SDMA_WB_RPTR  0
#define RD_SDMA_WB_WPTR  8
#define RD_SDMA_WB_FENCE 16
#define RD_SDMA_WB_TEST  24
/* ring->cond_exe_gpu_addr (1: run) and the IB test's writeback slot */
#define RD_SDMA_WB_COND_EXE 32
#define RD_SDMA_WB_IB_TEST  40

/* sdma_v6_0_start() for one instance (unhalt + gfx_resume_instance), then
 * sdma_v6_0_ring_test_ring(). */
int  sdma60_start(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t inst, uint32_t ring_size);
void sdma60_stop(rdna3_dev *d, rdna3_sdma_ring *r);
/* sdma_v6_0_ctxempty_int_enable / sdma_v6_0_hw_fini (and _suspend): context
 * empty interrupts off, every instance's queue stopped and halted
 * (rdna3amdgpu=1: at unload and before a GPU reset) */
void sdma60_ctxempty_int_enable(rdna3_dev *d, uint32_t num_instances, int enable);
void sdma60_hw_fini(rdna3_dev *d, uint32_t num_instances);
/* sdma_v6_0_soft_reset (FREEZE, HALT + TH1_RESET, GRBM_SOFT_RESET.SDMAn),
 * then the queue again on the same ring: pending fences read as done,
 * numbering continues. The hang recovery for the DMA ring (no MES). */
int  sdma60_soft_reset(rdna3_dev *d, rdna3_sdma_ring *r);
int  sdma60_test_ring(rdna3_dev *d, rdna3_sdma_ring *r);

/* ring building */
void sdma60_write(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t v);
void sdma60_commit(rdna3_dev *d, rdna3_sdma_ring *r);
void sdma60_emit_copy(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t src_mc, uint64_t dst_mc,
                      uint32_t bytes);
int  sdma60_fill_sync(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t dst_mc, uint32_t value, uint64_t len);
void sdma60_emit_fill(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t dst_mc, uint32_t value,
                      uint32_t bytes);
/* sdma_v6_0_ring_emit_ib: NOP-pad so the 6-dword INDIRECT packet ends on
 * an 8-dword boundary, then run 'ndw' dwords at GPU VA 'va' in 'vmid' */
void sdma60_emit_ib(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t vmid, uint64_t va, uint32_t ndw);
/* SDMA0_CNTL.TRAP_ENABLE (sdma_v6_0_set_trap_irq_state); also makes
 * every following fence emit a TRAP packet */
void sdma60_set_trap_interrupt(rdna3_dev *d, rdna3_sdma_ring *r, int enable);
/* fence 'seq' has been written back */
int  sdma60_fence_passed(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t seq);
/* emit a fence write of ++fence_seq; returns that sequence number */
uint32_t sdma60_emit_fence(rdna3_dev *d, rdna3_sdma_ring *r);
/* a user fence (AMDGPU_CHUNK_ID_FENCE): sdma_v6_0_ring_emit_fence with
 * AMDGPU_FENCE_FLAG_64BIT and no interrupt - two FENCE packets, low then
 * high dword of 'seq' at MC address 'addr' (8-byte aligned) */
void sdma60_emit_fence64(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t addr, uint64_t seq);
int  sdma60_wait_fence(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t seq);
/* sdma60_wait_fence with its own budget in microseconds */
int  sdma60_wait_fence_us(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t seq, uint32_t usec);

/* copy 'bytes' (<= 1 GiB) between MC addresses and wait for it */
int  sdma60_copy_sync(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t src_mc, uint64_t dst_mc,
                      uint32_t bytes);

/* a 'rows' x 'row_bytes' rectangle between MC addresses with their own
 * pitches (amdgpu_copy_buffer per row; one span when the pitches match),
 * waited for. RDNA3_EINVAL for a row wider than a pitch */
int  sdma60_copy_rect_sync(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t src_mc, uint32_t src_pitch, uint64_t dst_mc,
                           uint32_t dst_pitch, uint32_t row_bytes, uint32_t rows);

/* ---- sdma_v6_0 ring functions amdgpu_ib_schedule uses (rdna3amdgpu=1) ---- */
void sdma60_ring_insert_nop(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t count);
void sdma60_emit_wreg(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t reg, uint32_t val);
void sdma60_emit_reg_wait(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t reg, uint32_t val, uint32_t mask);
void sdma60_emit_reg_write_reg_wait(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t reg0, uint32_t reg1,
                                    uint32_t ref, uint32_t mask);
/* the GFXHUB page directory of 'vmid' and its invalidation on r->vm_inv_eng */
void sdma60_emit_vm_flush(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t vmid, uint64_t pd_addr);
void sdma60_emit_pipeline_sync(rdna3_dev *d, rdna3_sdma_ring *r);
void sdma60_emit_hdp_flush(rdna3_dev *d, rdna3_sdma_ring *r);
void sdma60_emit_mem_sync(rdna3_dev *d, rdna3_sdma_ring *r);
uint32_t sdma60_ring_init_cond_exec(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t addr);
/* amdgpu_ring_patch_cond_exec on an SDMA ring */
void sdma60_ring_patch_cond_exec(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t offset);
/* sdma_v6_0_ring_pad_ib: NOPs to an 8-dword multiple */
void sdma60_ring_pad_ib(const rdna3_sdma_ring *r, uint32_t *ib, uint32_t *length_dw);
/* the page-table packets of amdgpu_vm_sdma (into an IB being built) */
void sdma60_vm_copy_pte(uint32_t *ib, uint32_t *length_dw, uint64_t pe, uint64_t src, uint32_t count);
void sdma60_vm_write_pte(uint32_t *ib, uint32_t *length_dw, uint64_t pe, uint64_t value, uint32_t count,
                         uint32_t incr);
void sdma60_vm_set_pte_pde(uint32_t *ib, uint32_t *length_dw, uint64_t pe, uint64_t addr, uint32_t count,
                           uint32_t incr, uint64_t flags);
/* sdma_v6_0_ring_get_rptr / _get_wptr (dwords) */
uint64_t sdma60_ring_get_rptr(rdna3_dev *d, rdna3_sdma_ring *r);
uint64_t sdma60_ring_get_wptr(rdna3_dev *d, rdna3_sdma_ring *r);
/* sdma_v6_0_is_idle / sdma_v6_0_wait_for_idle over 'num_instances' */
int  sdma60_is_idle(rdna3_dev *d, uint32_t num_instances);
int  sdma60_wait_for_idle(rdna3_dev *d);
/* a DMA job in amdgpu_ib_schedule's order: memory sync (EMIT_MEM_SYNC),
 * amdgpu_vm_flush under COND_EXE (pipeline sync, VM flush, PASID mapping,
 * the flush's fence), the job's COND_EXE, HDP flush, IBs, user fence,
 * fence. pd_addr 0: no flush; pasid 0: no mapping. Returns the fence */
uint32_t sdma60_submit_job(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t vmid, uint64_t pd_addr,
                           uint32_t pasid, uint32_t pasid_reg, const uint64_t *ib_va,
                           const uint32_t *ib_ndw, const uint32_t *ib_flags, uint32_t n,
                           uint64_t uf_addr, uint64_t uf_seq_base);
/* sdma_v6_0_ring_test_ib: WRITE_LINEAR + 3 NOPs at VRAM offset 'ib' (VMID 0) */
int  sdma60_ring_test_ib(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t ib);

/* ---- user queues (amdgpu_userq through the MES) ----
 * sdma_v6_0_mqd_init (adev->mqds[AMDGPU_HW_IP_DMA].init_mqd): a zeroed
 * struct v11_sdma_mqd (CPU view) for the MES to map on an SDMA queue.
 * Reads SDMA0_QUEUE0_IB_CNTL, so the GFX must be powered. 0 */
int  sdma60_mqd_init(rdna3_dev *d, uint32_t *mqd, const amd_mqd_prop *p);
/* sdma_v6_0_sw_init for SDMA 6.0.3: user queues from firmware 25 on */
int  sdma60_userq_supported(uint32_t fw_version);
/* sdma_v6_0_set_userq_trap_interrupts: TRAP_ENABLE on every instance */
void sdma60_set_userq_trap_interrupts(rdna3_dev *d, uint32_t num_instances, int enable);

#ifdef __cplusplus
}
#endif
#endif
