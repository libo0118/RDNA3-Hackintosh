/*
 * ih60.h - IH 6.0 (OSSSYS 6.0.x) interrupt ring, ported from Linux amdgpu
 * ih_v6_0.c, amdgpu_ih.c and nbio_v4_3.c ih_control (MIT).
 *
 * Bring-up shape, deliberately smaller than amdgpu:
 *  - ring 0 only (amdgpu adds ring 1 for page-fault redirection on dGPUs
 *    and a software ring; both come later),
 *  - ring and write-pointer writeback live in VRAM and are addressed by
 *    their MC address (MC_SPACE 4, VMID0), like the PSP buffers, so no
 *    bus mastering is needed,
 *  - no doorbell: the read pointer goes through IH_RB_RPTR,
 *  - with 'msi' the ring re-arms the interrupt on every rptr write
 *    (IH_RB_CNTL.RPTR_REARM, what amdgpu does with msi_enabled) and the OS
 *    layer calls ih60_process() from its MSI handler; without it the ring
 *    is polled.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_IH60_H
#define RDNA3_IH60_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_IH_RING_SIZE (256 * 1024)   /* ih_v6_0.c IH_RING_SIZE */
#define RD_IH_IV_SIZE   32             /* bytes per interrupt vector */

/* amdgpu_iv_entry (amdgpu_ih_decode_iv_helper) */
typedef struct rdna3_iv {
    uint32_t client_id, src_id, ring_id, vmid, vmid_src;
    uint64_t timestamp;
    uint32_t timestamp_src, pasid, node_id;
    uint32_t src_data[4];
} rdna3_iv;

typedef struct rdna3_ih {
    uint64_t ring;          /* VRAM offset of the ring */
    uint64_t wptr_wb;       /* VRAM offset of the wptr writeback dword */
    uint32_t ring_size;     /* bytes, power of two */
    uint32_t ptr_mask;
    uint32_t rptr;          /* bytes */
    int      enabled;
    int      msi;
    uint32_t overflows;
    uint64_t processed;
    /* rdna3amdgpu=1 (ih60_init_amdgpu): the ring in system memory through
     * the GART like amdgpu's GTT ring (MC_SNOOP), its wptr / rptr shadows
     * right after it, the rptr written by doorbell */
    volatile uint32_t *sys;       /* CPU view of the ring (NULL: the ring is in VRAM) */
    uint64_t gpu_addr;            /* ring MC (GART) address; wptr at + ring_size, rptr + 4 */
    uint32_t ring_id;             /* 0, or 1 for amdgpu's ih1 */
    int      use_doorbell;
    uint32_t doorbell_index;
    struct rdna3_ih *ih1;         /* ring 0: the page-fault ring 1 (dGPU) */
    uint64_t self_irqs;           /* ih_v6_0_self_irq IVs that moved ring 1's wptr */
} rdna3_ih;

/* amdgpu_ih_ring_init + ih_v6_0_irq_init (ring 0); 'msi' = an MSI handler
 * will drain the ring (RPTR_REARM), otherwise it is polled */
int  ih60_init(rdna3_dev *d, rdna3_ih *ih, uint32_t ring_size, uint64_t dummy_page_pa, int msi);
/* With rdna3amdgpu=1, ih_v6_0_sw_init + hw_init as amdgpu runs them on a
 * dGPU: ring 0 and ring 1 in system memory ('sys0' / 'sys1', ring_size + 8
 * bytes each, mapped at MC 'mc0' / 'mc1' through the GART), MC_SNOOP and
 * IH_REQ_NONSNOOP_EN 0, rptr doorbells (AMDGPU_NAVI10_DOORBELL_IH << 1 and
 * the next one, nbio_v4_3_ih_doorbell_range) when d->doorbell_ok, retry
 * page faults (client 0xa, source 0) redirected to ring 1, whose wptr the
 * IH reports through a self IV on ring 0 (ih_v6_0_self_irq). ih60_process
 * drains ring 1 too. */
int  ih60_init_amdgpu(rdna3_dev *d, rdna3_ih *ih, rdna3_ih *ih1, volatile void *sys0, uint64_t mc0,
                      volatile void *sys1, uint64_t mc1, uint32_t ring_size, uint64_t dummy_page_pa,
                      int msi);
/* ih_v6_0_irq_disable (both rings) */
void ih60_disable(rdna3_dev *d, rdna3_ih *ih);
/* amdgpu_ih_process: decode every IV between rptr and wptr, call 'cb' for
 * each, write the new rptr. Returns the number of IVs handled. */
typedef void (*rdna3_iv_cb)(void *ctx, const rdna3_iv *iv);
uint32_t ih60_process(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx);

/* fence interrupts the kernel rings raise (soc15_ih_clientid.h,
 * irqsrcs_gfx_11_0_0.h): CP end-of-pipe for gfx ME0 pipe 0 (RELEASE_MEM
 * INT_SEL 2), SDMA0 queue 0 trap (SDMA TRAP packet) */
#define RD_IH_CLIENT_GFX          0x0a
#define RD_IH_CLIENT_GRBM_CP      0x14
#define RD_IH_SRC_CP_EOP          181
#define RD_IH_SRC_SDMA_TRAP       49
#define RD_IH_CLIENT_VMC          0x12   /* MMHUB page faults (VMC_1_0__SRCID__VM_FAULT) */
#define RD_IH_SRC_VM_FAULT        0      /* also UTCL2_1_0__SRCID__FAULT on client GFX */
#define RD_IH_CLIENT_IH           0x00   /* SOC21_IH_CLIENTID_IH: self IVs */
#define RD_IH_FENCE_GFX           1u
#define RD_IH_FENCE_SDMA0         2u
#define RD_IH_FENCE_COMPUTE0      3u   /* MEC1 pipe 0 queue 0 (the first KCQ) */
/* which kernel ring's fence an IV reports (RD_IH_FENCE_*), 0 if none */
uint32_t ih60_iv_fence(const rdna3_iv *iv);
/* user queue fences (MES-scheduled queues): a CP end-of-pipe or an SDMA
 * user fence (GFX_11_0_0__SRCID__SDMA_FENCE) whose src_data[0] names the
 * queue's doorbell; returns that doorbell index (the key of adev->userq_xa),
 * 0 if the IV is no user queue fence */
#define RD_IH_SRC_SDMA_FENCE      67
uint32_t ih60_iv_userq_doorbell(const rdna3_iv *iv);

/* decode one 8-dword IV (pure; exposed for tests) */
void ih60_decode_iv(const uint32_t dw[8], rdna3_iv *iv);

#ifdef __cplusplus
}
#endif
#endif
