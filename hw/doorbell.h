/*
 * doorbell.h - doorbell aperture (NBIO 4.3) and the SOC21 doorbell index
 * layout, ported from Linux amdgpu nbio_v4_3.c, soc21.c and
 * amdgpu_doorbell.h (MIT).
 *
 * A doorbell is a 64-bit write into BAR2 at dword index 'ring->doorbell';
 * the index is the SOC21 assignment shifted left by one (64-bit slots).
 * The S2A range registers sit outside the MMIO BAR and go through the
 * RSMU index/data pair (rdna3_wreg does that on its own).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DOORBELL_H
#define RDNA3_DOORBELL_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AMDGPU_NAVI10_DOORBELL_ASSIGNMENT (soc21_init_doorbell_index) */
#define RD_DB_KIQ                 0x000
#define RD_DB_MEC_RING0           0x003
#define RD_DB_USERQUEUE_END       0x08A
#define RD_DB_GFX_RING0           0x08B
#define RD_DB_GFX_USERQUEUE_END   0x0FF
#define RD_DB_SDMA_ENGINE0        0x100
#define RD_DB_SDMA_ENGINE1        0x10A
#define RD_DB_IH                  0x178
#define RD_DB_MAX_ASSIGNMENT      0x190
#define RD_DB_SDMA_DOORBELL_RANGE 20
#define RD_DB_INDEX(assignment)   ((uint32_t)(assignment) << 1)   /* ring->doorbell_index */

/* soc21_common_hw_init: enable_doorbell_aperture + selfring aperture.
 * Needs d->doorbell_bus / doorbell_len (and doorbell_cpu or os.doorbell_wr);
 * sets d->doorbell_ok. Without a doorbell BAR it returns RDNA3_ENODEV and
 * the rings keep writing their wptr registers. */
int  nbio43_doorbell_init(rdna3_dev *d);
/* nbio_v4_3_gc_doorbell_init (gfx_v11_0_hw_init) */
void nbio43_gc_doorbell_init(rdna3_dev *d);
/* nbio_v4_3_sdma_doorbell_range (instance 0 only on this NBIO) */
void nbio43_sdma_doorbell_range(rdna3_dev *d, uint32_t inst, int use, uint32_t index,
                                uint32_t size);

#ifdef __cplusplus
}
#endif
#endif
