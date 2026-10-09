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
 *  - no MSI yet: ih60_process() is polled; the OS layer will call it from
 *    its interrupt handler once IOKit interrupt registration is in place.
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
    uint32_t overflows;
    uint64_t processed;
} rdna3_ih;

/* amdgpu_ih_ring_init + ih_v6_0_irq_init (ring 0) */
int  ih60_init(rdna3_dev *d, rdna3_ih *ih, uint32_t ring_size, uint64_t dummy_page_pa);
/* ih_v6_0_irq_disable */
void ih60_disable(rdna3_dev *d, rdna3_ih *ih);
/* amdgpu_ih_process: decode every IV between rptr and wptr, call 'cb' for
 * each, write the new rptr. Returns the number of IVs handled. */
typedef void (*rdna3_iv_cb)(void *ctx, const rdna3_iv *iv);
uint32_t ih60_process(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx);

/* decode one 8-dword IV (pure; exposed for tests) */
void ih60_decode_iv(const uint32_t dw[8], rdna3_iv *iv);

#ifdef __cplusplus
}
#endif
#endif
