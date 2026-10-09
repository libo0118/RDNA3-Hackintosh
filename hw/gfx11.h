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
 * is programmed through CP_RB0_* registers, no MES, no KIQ (compute queues
 * need KIQ/MES and come later). Ring, rptr writeback, wptr poll slot,
 * fence slot and the clear-state buffer live in VRAM (MC addresses); the
 * write pointer goes through CP_RB0_WPTR (no doorbell yet).
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
#define PACKET3_SET_CONTEXT_REG    0x69
#define PACKET3_SET_CONTEXT_REG_START 0x0000a000
#define PACKET3_SET_UCONFIG_REG    0x79
#define PACKET3_SET_UCONFIG_REG_START 0x0000c000
#define PACKET3_PREAMBLE_BEGIN_CLEAR_STATE (2u << 28)
#define PACKET3_PREAMBLE_END_CLEAR_STATE   (3u << 28)

#define RD_GFX_RING_SIZE (256 * 1024)   /* bytes */

/* RS64 program start addresses from the gfx_firmware_header_v2_0 of the
 * PFP / ME / MEC blobs (gfx_v11_0_config_gfx_rs64) */
typedef struct rdna3_rs64_start {
    uint32_t pfp_lo, pfp_hi, me_lo, me_hi, mec_lo, mec_hi;
} rdna3_rs64_start;

typedef struct rdna3_gfx_ring {
    uint64_t ring;          /* VRAM offsets */
    uint64_t wb;            /* 4 KiB: rptr @0, wptr poll @8, fence @16, test @24 */
    uint64_t csb;           /* clear-state buffer */
    uint32_t csb_dw;
    uint32_t ring_size, buf_mask;   /* bytes, dwords-1 */
    uint64_t wptr;          /* dwords */
    uint32_t fence_seq;
    int      ready;
} rdna3_gfx_ring;

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
int  gfx11_fence_passed(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq);
int  gfx11_wait_fence(rdna3_dev *d, rdna3_gfx_ring *r, uint32_t seq);

/* size of the clear-state buffer in dwords (gfx_v11_0_get_csb_size) and
 * its contents (gfx_v11_0_get_csb_buffer); exposed for tests */
uint32_t gfx11_csb_size(void);
uint32_t gfx11_csb_fill(uint32_t *buf, uint32_t max_dw);

#ifdef __cplusplus
}
#endif
#endif
