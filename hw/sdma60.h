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
#define SDMA_SUBOP_COPY_LINEAR  0
#define SDMA_SUBOP_WRITE_LINEAR 0
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
} rdna3_sdma_ring;

#define RD_SDMA_WB_RPTR  0
#define RD_SDMA_WB_WPTR  8
#define RD_SDMA_WB_FENCE 16
#define RD_SDMA_WB_TEST  24

/* sdma_v6_0_start() for one instance (unhalt + gfx_resume_instance), then
 * sdma_v6_0_ring_test_ring(). */
int  sdma60_start(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t inst, uint32_t ring_size);
void sdma60_stop(rdna3_dev *d, rdna3_sdma_ring *r);
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
int  sdma60_wait_fence(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t seq);

/* copy 'bytes' (<= 1 GiB) between MC addresses and wait for it */
int  sdma60_copy_sync(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t src_mc, uint64_t dst_mc,
                      uint32_t bytes);

#ifdef __cplusplus
}
#endif
#endif
