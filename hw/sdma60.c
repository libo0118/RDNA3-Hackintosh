/*
 * sdma60.c - see sdma60.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sdma60.h"
#include "doorbell.h"
#include "gen/gc_11_0_0_sdma.h"
#include "gen/gc_11_0_0_sdma_reset.h"
#include "gen/gc_11_0_0_reset.h"
#include "gen/gc_11_0_0_userq.h"
#include "gmc11.h"
#include "navi3x_regs.h"

#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_SET_FIELD(orig, reg, field, val) \
    (((orig) & ~(uint32_t)REG_FIELD_MASK(reg, field)) | \
     ((uint32_t)REG_FIELD_MASK(reg, field) & ((uint32_t)(val) << REG_FIELD_SHIFT(reg, field))))

/* sdma_v6_0.c */
#define SDMA1_REG_OFFSET         0x600
#define SDMA0_HYP_DEC_REG_START  0x5880
#define SDMA0_HYP_DEC_REG_END    0x589a
#define SDMA1_HYP_DEC_REG_OFFSET 0x20
/* sdma_common.h */
#define CACHE_READ_POLICY_L2__DEFAULT  2   /* NOA */
#define CACHE_WRITE_POLICY_L2__DEFAULT 3   /* BYPASS */

/* sdma_v6_0_get_reg_offset(): HYP_DEC registers sit in GC segment 1 with
 * a 0x20 stride per instance, the rest in segment 0 with 0x600 */
static uint32_t sdma_off(rdna3_dev *d, uint32_t inst, uint32_t reg, uint32_t base_idx)
{
    uint32_t o = rdna3_reg_offset(d, HWIP_GC, 0, base_idx, reg);
    if (reg >= SDMA0_HYP_DEC_REG_START && reg <= SDMA0_HYP_DEC_REG_END)
        return o + SDMA1_HYP_DEC_REG_OFFSET * inst;
    return o + (inst == 1 ? SDMA1_REG_OFFSET : 0);
}
#define RS(d, i, reg)    rdna3_rreg((d), sdma_off((d), (i), reg, reg##_BASE_IDX))
#define WS(d, i, reg, v) rdna3_wreg((d), sdma_off((d), (i), reg, reg##_BASE_IDX), (v))

static uint32_t order_base_2(uint32_t n)
{
    uint32_t r = 0;
    while ((1u << r) < n)
        r++;
    return r;
}

static uint32_t wb_read32(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t off)
{
    uint32_t v;
    rdna3_vram_read(d, r->wb + off, &v, 4);
    return v;
}

void sdma60_write(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t v)
{
    rdna3_vram_write(d, r->ring + (r->wptr & r->buf_mask) * 4, &v, 4);
    r->wptr++;
}

/* sdma_v6_0_ring_set_wptr (no doorbell) + the poll slot the F32 reads */
/* sdma_v6_0_ring_set_wptr: wptr in bytes to the poll slot, then the
 * doorbell or the registers */
void sdma60_commit(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint64_t w;
    if (r->amdgpu_pad) {
        /* amdgpu_ring_commit: pad to match fetch size (align_mask 0xf) */
        uint32_t count = (0x10 - ((uint32_t)r->wptr & 0xf)) & 0xf;
        if (count)
            sdma60_ring_insert_nop(d, r, count);
    }
    w = r->wptr << 2;
    rdna3_vram_write(d, r->wb + RD_SDMA_WB_WPTR, &w, 8);
    rdna3_hdp_flush(d);
    if (r->use_doorbell) {
        rdna3_doorbell_wr64(d, r->doorbell, w);
        return;
    }
    WS(d, r->inst, regSDMA0_QUEUE0_RB_WPTR, (uint32_t)w);
    WS(d, r->inst, regSDMA0_QUEUE0_RB_WPTR_HI, (uint32_t)(w >> 32));
}

/* sdma_v6_0_emit_copy_buffer (no TMZ) */
void sdma60_emit_copy(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t src, uint64_t dst, uint32_t bytes)
{
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR));
    sdma60_write(d, r, bytes - 1);
    sdma60_write(d, r, 0);   /* src/dst endian swap */
    sdma60_write(d, r, (uint32_t)src);
    sdma60_write(d, r, (uint32_t)(src >> 32));
    sdma60_write(d, r, (uint32_t)dst);
    sdma60_write(d, r, (uint32_t)(dst >> 32));
}

/* sdma_v6_0_emit_fill_buffer */
void sdma60_emit_fill(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t dst, uint32_t value, uint32_t bytes)
{
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL));
    sdma60_write(d, r, (uint32_t)dst);
    sdma60_write(d, r, (uint32_t)(dst >> 32));
    sdma60_write(d, r, value);
    sdma60_write(d, r, bytes - 1);
}

void sdma60_emit_ib(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t vmid, uint64_t va, uint32_t ndw)
{
    /* sdma_v6_0_ring_insert_nop (burst NOPs with r->burst_nop) */
    sdma60_ring_insert_nop(d, r, (2 - (uint32_t)r->wptr) & 7);
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT) | SDMA_PKT_INDIRECT_HEADER_VMID(vmid));
    sdma60_write(d, r, (uint32_t)va & 0xffffffe0);   /* 32-byte aligned base */
    sdma60_write(d, r, (uint32_t)(va >> 32));
    sdma60_write(d, r, ndw);
    sdma60_write(d, r, 0);                           /* CSA: none */
    sdma60_write(d, r, 0);
}

int sdma60_fence_passed(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t seq)
{
    return (int32_t)(wb_read32(d, r, RD_SDMA_WB_FENCE) - seq) >= 0;
}

/* sdma_v6_0_ring_emit_fence (32-bit, no interrupt) */
uint32_t sdma60_emit_fence(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint64_t addr = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_FENCE);
    uint32_t seq = ++r->fence_seq;
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE) | SDMA_PKT_FENCE_HEADER_MTYPE(0x3));
    sdma60_write(d, r, (uint32_t)addr);
    sdma60_write(d, r, (uint32_t)(addr >> 32));
    sdma60_write(d, r, seq);
    if (r->fence_irq) {   /* sdma_v6_0_ring_emit_fence, AMDGPU_FENCE_FLAG_INT */
        sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_TRAP));
        sdma60_write(d, r, 0);   /* INT_CONTEXT 0 */
    }
    return seq;
}

void sdma60_emit_fence64(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t addr, uint64_t seq)
{
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE) | SDMA_PKT_FENCE_HEADER_MTYPE(0x3));
    sdma60_write(d, r, (uint32_t)addr);
    sdma60_write(d, r, (uint32_t)(addr >> 32));
    sdma60_write(d, r, (uint32_t)seq);
    addr += 4;
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE) | SDMA_PKT_FENCE_HEADER_MTYPE(0x3));
    sdma60_write(d, r, (uint32_t)addr);
    sdma60_write(d, r, (uint32_t)(addr >> 32));
    sdma60_write(d, r, (uint32_t)(seq >> 32));
}

void sdma60_set_trap_interrupt(rdna3_dev *d, rdna3_sdma_ring *r, int enable)
{
    uint32_t tmp = RS(d, r->inst, regSDMA0_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_CNTL, TRAP_ENABLE, enable ? 1 : 0);
    WS(d, r->inst, regSDMA0_CNTL, tmp);
    r->fence_irq = enable;
}

int sdma60_wait_fence(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t seq)
{
    uint32_t i, v = 0;
    for (i = 0; i < d->usec_timeout; i++) {
        v = wb_read32(d, r, RD_SDMA_WB_FENCE);
        if ((int32_t)(v - seq) >= 0)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "SDMA%u: fence %u timeout (last %u, rptr 0x%x, status 0x%08x)", r->inst, seq, v,
         wb_read32(d, r, RD_SDMA_WB_RPTR), RS(d, r->inst, regSDMA0_STATUS_REG));
    return RDNA3_ETIME;
}

int sdma60_test_ring(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint64_t gpu = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_TEST);
    uint32_t v = 0xCAFEDEAD, i;

    rdna3_vram_write(d, r->wb + RD_SDMA_WB_TEST, &v, 4);
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR));
    sdma60_write(d, r, (uint32_t)gpu);
    sdma60_write(d, r, (uint32_t)(gpu >> 32));
    sdma60_write(d, r, 0);   /* WRITE_UNTILED_DW_3_COUNT(0): one dword */
    sdma60_write(d, r, 0xDEADBEEF);
    sdma60_commit(d, r);
    for (i = 0; i < d->usec_timeout; i++) {
        v = wb_read32(d, r, RD_SDMA_WB_TEST);
        if (v == 0xDEADBEEF)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "SDMA%u: ring test timeout (0x%08x, status 0x%08x)", r->inst, v,
         RS(d, r->inst, regSDMA0_STATUS_REG));
    return RDNA3_ETIME;
}

static int sdma_program(rdna3_dev *d, rdna3_sdma_ring *r);

int sdma60_start(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t inst, uint32_t ring_size)
{
    uint8_t zero[64] = { 0 };

    if (!d->ipver[HWIP_SDMA0].present || d->ipver[HWIP_SDMA0].major != 6) {
        RERR(d, "SDMA: discovery has no SDMA 6.x");
        return RDNA3_ENODEV;
    }
    if (inst > 1)
        return RDNA3_EINVAL;
    if (!ring_size)
        ring_size = RD_SDMA_RING_SIZE;
    if (ring_size & (ring_size - 1))
        return RDNA3_EINVAL;

    r->inst = inst;
    r->ring_size = ring_size;
    r->buf_mask = ring_size / 4 - 1;
    r->wptr = 0;
    r->fence_seq = 0;
    r->ready = 0;
    r->ring = rdna3_vram_alloc(d, ring_size, 4096, "sdma-ring");
    r->wb = rdna3_vram_alloc(d, 4096, 4096, "sdma-wb");
    if (r->ring == UINT64_MAX || r->wb == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, r->ring, 0, ring_size);
    rdna3_vram_write(d, r->wb, zero, sizeof(zero));
    {
        /* amdgpu_ring_init: "always set cond_exec_polling to CONTINUE" */
        uint32_t one = 1;
        rdna3_vram_write(d, r->wb + RD_SDMA_WB_COND_EXE, &one, 4);
    }
    rdna3_hdp_flush(d);
    /* sdma_v6_0_hw_init / sdma_v6_0_resume = sdma_v6_0_start */
    return sdma_program(d, r);
}

/* sdma_v6_0_start for one instance on an allocated ring: unhalt, the
 * queue registers (gfx_resume_instance), ring test */
static int sdma_program(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint32_t rb_cntl, ib_cntl, tmp, inst = r->inst, ring_size = r->ring_size;
    uint64_t ring_mc, rptr_mc, wptr_mc;

    ring_mc = rdna3_vram_mc(d, r->ring);
    rptr_mc = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_RPTR);
    wptr_mc = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_WPTR);

    /* sdma_v6_0_start: unhalt (sdma_v6_0_enable(true)), ctxempty interrupt */
    tmp = RS(d, inst, regSDMA0_F32_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, HALT, 0);
    WS(d, inst, regSDMA0_F32_CNTL, tmp);
    tmp = RS(d, inst, regSDMA0_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_CNTL, CTXEMPTY_INT_ENABLE, 1);
    WS(d, inst, regSDMA0_CNTL, tmp);

    /* sdma_v6_0_gfx_resume (gfx_resume_instance in later kernels) */
    WS(d, inst, regSDMA0_SEM_WAIT_FAIL_TIMER_CNTL, 0);
    rb_cntl = RS(d, inst, regSDMA0_QUEUE0_RB_CNTL);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_SIZE, order_base_2(ring_size / 4));
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_PRIV, 1);
    WS(d, inst, regSDMA0_QUEUE0_RB_CNTL, rb_cntl);

    WS(d, inst, regSDMA0_QUEUE0_RB_RPTR, 0);
    WS(d, inst, regSDMA0_QUEUE0_RB_RPTR_HI, 0);
    WS(d, inst, regSDMA0_QUEUE0_RB_WPTR, 0);
    WS(d, inst, regSDMA0_QUEUE0_RB_WPTR_HI, 0);

    WS(d, inst, regSDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO, (uint32_t)wptr_mc);
    WS(d, inst, regSDMA0_QUEUE0_RB_WPTR_POLL_ADDR_HI, (uint32_t)(wptr_mc >> 32));
    WS(d, inst, regSDMA0_QUEUE0_RB_RPTR_ADDR_HI, (uint32_t)(rptr_mc >> 32));
    WS(d, inst, regSDMA0_QUEUE0_RB_RPTR_ADDR_LO, (uint32_t)rptr_mc & 0xFFFFFFFC);

    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, WPTR_POLL_ENABLE, 0);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, F32_WPTR_POLL_ENABLE, 1);

    WS(d, inst, regSDMA0_QUEUE0_RB_BASE, (uint32_t)(ring_mc >> 8));
    WS(d, inst, regSDMA0_QUEUE0_RB_BASE_HI, (uint32_t)(ring_mc >> 40));

    WS(d, inst, regSDMA0_QUEUE0_MINOR_PTR_UPDATE, 1);
    WS(d, inst, regSDMA0_QUEUE0_RB_WPTR, 0);
    WS(d, inst, regSDMA0_QUEUE0_RB_WPTR_HI, 0);

    /* doorbell when the aperture is up (sdma_v6_0_gfx_resume_instance) */
    r->use_doorbell = d->doorbell_ok;
    r->doorbell = RD_DB_INDEX(inst ? RD_DB_SDMA_ENGINE1 : RD_DB_SDMA_ENGINE0);
    tmp = RS(d, inst, regSDMA0_QUEUE0_DOORBELL);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_DOORBELL, ENABLE, r->use_doorbell ? 1 : 0);
    WS(d, inst, regSDMA0_QUEUE0_DOORBELL, tmp);
    tmp = RS(d, inst, regSDMA0_QUEUE0_DOORBELL_OFFSET);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_DOORBELL_OFFSET, OFFSET, r->use_doorbell ? r->doorbell : 0);
    WS(d, inst, regSDMA0_QUEUE0_DOORBELL_OFFSET, tmp);
    nbio43_sdma_doorbell_range(d, inst, r->use_doorbell, r->doorbell, RD_DB_SDMA_DOORBELL_RANGE * 2);
    /* nbio sdma_doorbell_range(use_doorbell = false): S2A_DOORBELL_ENTRY_2_CTRL
     * is outside the MMIO BAR, left at reset like IH's entry */

    WS(d, inst, regSDMA0_QUEUE0_MINOR_PTR_UPDATE, 0);

    tmp = RS(d, inst, regSDMA0_WATCHDOG_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_WATCHDOG_CNTL, QUEUE_HANG_COUNT,
                        d->usec_timeout / 100000 > 1 ? d->usec_timeout / 100000 : 1);
    WS(d, inst, regSDMA0_WATCHDOG_CNTL, tmp);

    tmp = RS(d, inst, regSDMA0_UTCL1_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_UTCL1_CNTL, RESP_MODE, 3);
    tmp = REG_SET_FIELD(tmp, SDMA0_UTCL1_CNTL, REDO_DELAY, 9);
    WS(d, inst, regSDMA0_UTCL1_CNTL, tmp);

    tmp = RS(d, inst, regSDMA0_UTCL1_PAGE);
    tmp &= 0xFF0FFF;
    tmp |= (CACHE_READ_POLICY_L2__DEFAULT << 12) | (CACHE_WRITE_POLICY_L2__DEFAULT << 14) |
           SDMA0_UTCL1_PAGE__LLC_NOALLOC_MASK;
    WS(d, inst, regSDMA0_UTCL1_PAGE, tmp);

    tmp = RS(d, inst, regSDMA0_F32_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, HALT, 0);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, TH1_RESET, 0);
    WS(d, inst, regSDMA0_F32_CNTL, tmp);

    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 1);
    WS(d, inst, regSDMA0_QUEUE0_RB_CNTL, rb_cntl);
    ib_cntl = RS(d, inst, regSDMA0_QUEUE0_IB_CNTL);
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_QUEUE0_IB_CNTL, IB_ENABLE, 1);
    WS(d, inst, regSDMA0_QUEUE0_IB_CNTL, ib_cntl);

    if (sdma60_test_ring(d, r) != RDNA3_OK)
        return RDNA3_ETIME;
    r->ready = 1;
    RLOG(d, "SDMA%u: ring %u KiB at MC 0x%llx, ring test passed", inst, ring_size / 1024,
         (unsigned long long)ring_mc);
    return RDNA3_OK;
}

/* sdma_v6_0_soft_reset for one instance, then the queue comes back on the
 * same ring buffer with the fence numbering kept: whatever was in flight is
 * dropped and its fences are completed (amdgpu force-completes them), so
 * every waiter returns. */
int sdma60_soft_reset(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint32_t tmp, seq = r->fence_seq;
    uint8_t zero[16] = { 0 };

    if (r->ring == 0 || r->ring == UINT64_MAX || !r->ring_size)
        return RDNA3_EINVAL;
    sdma60_stop(d, r);
    tmp = RS(d, r->inst, regSDMA0_FREEZE);
    tmp |= SDMA0_FREEZE__FREEZE_MASK;
    WS(d, r->inst, regSDMA0_FREEZE, tmp);
    tmp = RS(d, r->inst, regSDMA0_F32_CNTL);
    tmp |= SDMA0_F32_CNTL__HALT_MASK | SDMA0_F32_CNTL__TH1_RESET_MASK;
    WS(d, r->inst, regSDMA0_F32_CNTL, tmp);
    WS(d, r->inst, regSDMA0_QUEUE0_PREEMPT, 0);
    d->os.udelay(d->os.ctx, 100);

    tmp = GRBM_SOFT_RESET__SOFT_RESET_SDMA0_MASK << r->inst;
    WREG32_SOC15(d, GC, 0, regGRBM_SOFT_RESET, tmp);
    (void)RREG32_SOC15(d, GC, 0, regGRBM_SOFT_RESET);
    d->os.udelay(d->os.ctx, 100);
    WREG32_SOC15(d, GC, 0, regGRBM_SOFT_RESET, 0);
    (void)RREG32_SOC15(d, GC, 0, regGRBM_SOFT_RESET);
    d->os.udelay(d->os.ctx, 100);

    /* the ring starts empty at 0; the fence slot says "all done" */
    r->wptr = 0;
    rdna3_vram_fill(d, r->ring, 0, r->ring_size);
    rdna3_vram_write(d, r->wb + RD_SDMA_WB_RPTR, zero, sizeof(zero));
    rdna3_vram_write(d, r->wb + RD_SDMA_WB_FENCE, &seq, 4);
    rdna3_hdp_flush(d);
    tmp = sdma_program(d, r);
    RLOG(d, "SDMA%u: soft reset %s, fences completed up to %u", r->inst,
         tmp == RDNA3_OK ? "done" : "failed", seq);
    return (int)tmp;
}

/* sdma_v6_0_enable(false) for one instance: sdma_v6_0_gfx_stop + halt
 * (sdma_v6_0_rlc_stop is empty) */
void sdma60_stop(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint32_t tmp = RS(d, r->inst, regSDMA0_QUEUE0_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    WS(d, r->inst, regSDMA0_QUEUE0_RB_CNTL, tmp);
    tmp = RS(d, r->inst, regSDMA0_QUEUE0_IB_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_IB_CNTL, IB_ENABLE, 0);
    WS(d, r->inst, regSDMA0_QUEUE0_IB_CNTL, tmp);
    tmp = RS(d, r->inst, regSDMA0_F32_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, HALT, 1);
    WS(d, r->inst, regSDMA0_F32_CNTL, tmp);
    r->ready = 0;
}

int sdma60_copy_sync(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t src, uint64_t dst, uint32_t bytes)
{
    uint32_t seq;
    if (!r->ready || !bytes || bytes > (1u << 30))
        return RDNA3_EINVAL;
    sdma60_emit_copy(d, r, src, dst, bytes);
    seq = sdma60_emit_fence(d, r);
    sdma60_commit(d, r);
    return sdma60_wait_fence(d, r, seq);
}

/* --------------------------- user queues --------------------------- */

/* struct v11_sdma_mqd dword offsets (v11_structs.h) */
#define SMQD_RB_CNTL               0
#define SMQD_RB_BASE               1
#define SMQD_RB_BASE_HI            2
#define SMQD_RB_RPTR_ADDR_HI       7
#define SMQD_RB_RPTR_ADDR_LO       8
#define SMQD_IB_CNTL               9
#define SMQD_SKIP_CNTL             15
#define SMQD_CONTEXT_STATUS        16
#define SMQD_DOORBELL              17
#define SMQD_DOORBELL_LOG          18
#define SMQD_DOORBELL_OFFSET       19
#define SMQD_CSA_ADDR_LO           20
#define SMQD_CSA_ADDR_HI           21
#define SMQD_DUMMY_REG             25
#define SMQD_RB_WPTR_POLL_ADDR_HI  26
#define SMQD_RB_WPTR_POLL_ADDR_LO  27
#define SMQD_RB_AQL_CNTL           28
#define SMQD_F32_DBG0              43
#define SMQD_F32_DBG1              44

/* Ported from sdma_v6_0_mqd_init */
int sdma60_mqd_init(rdna3_dev *d, uint32_t *m, const amd_mqd_prop *p)
{
    uint64_t wb_gpu_addr;

    m[SMQD_RB_CNTL] = order_base_2(p->queue_size / 4) << SDMA0_QUEUE0_RB_CNTL__RB_SIZE__SHIFT |
                      1u << SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT |
                      4u << SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_TIMER__SHIFT |
                      1u << SDMA0_QUEUE0_RB_CNTL__F32_WPTR_POLL_ENABLE__SHIFT;
    m[SMQD_RB_BASE] = (uint32_t)(p->hqd_base_gpu_addr >> 8);
    m[SMQD_RB_BASE_HI] = (uint32_t)(p->hqd_base_gpu_addr >> 40);
    wb_gpu_addr = p->wptr_gpu_addr;
    m[SMQD_RB_WPTR_POLL_ADDR_LO] = (uint32_t)wb_gpu_addr;
    m[SMQD_RB_WPTR_POLL_ADDR_HI] = (uint32_t)(wb_gpu_addr >> 32);
    wb_gpu_addr = p->rptr_gpu_addr;
    m[SMQD_RB_RPTR_ADDR_LO] = (uint32_t)wb_gpu_addr;
    m[SMQD_RB_RPTR_ADDR_HI] = (uint32_t)(wb_gpu_addr >> 32);
    m[SMQD_IB_CNTL] = RS(d, 0, regSDMA0_QUEUE0_IB_CNTL);
    m[SMQD_DOORBELL_OFFSET] = p->doorbell_index << SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET__SHIFT;
    m[SMQD_DOORBELL] = REG_SET_FIELD(0u, SDMA0_QUEUE0_DOORBELL, ENABLE, 1);
    m[SMQD_SKIP_CNTL] = 0;
    m[SMQD_CONTEXT_STATUS] = 0;
    m[SMQD_DOORBELL_LOG] = 0;
    m[SMQD_RB_AQL_CNTL] = regSDMA0_QUEUE0_RB_AQL_CNTL_DEFAULT;
    m[SMQD_DUMMY_REG] = regSDMA0_QUEUE0_DUMMY_REG_DEFAULT;
    m[SMQD_CSA_ADDR_LO] = (uint32_t)p->csa_addr;
    m[SMQD_CSA_ADDR_HI] = (uint32_t)(p->csa_addr >> 32);
    m[SMQD_F32_DBG0] = (uint32_t)p->fence_address;
    m[SMQD_F32_DBG1] = (uint32_t)(p->fence_address >> 32);
    return RDNA3_OK;
}

/* the user queue gate of sdma_v6_0_sw_init (IP_VERSION(6, 0, 3): fw_version >= 25) */
int sdma60_userq_supported(uint32_t fw_version)
{
    return fw_version >= 25;
}

/* Ported from sdma_v6_0_set_userq_trap_interrupts: the trap interrupt of
 * every instance (sdma_v6_0_set_trap_irq_state), as amdgpu_irq_get does */
void sdma60_set_userq_trap_interrupts(rdna3_dev *d, uint32_t num_instances, int enable)
{
    uint32_t i, tmp;
    for (i = 0; i < num_instances; i++) {
        tmp = RS(d, i, regSDMA0_CNTL);
        tmp = REG_SET_FIELD(tmp, SDMA0_CNTL, TRAP_ENABLE, enable ? 1 : 0);
        WS(d, i, regSDMA0_CNTL, tmp);
    }
}

/* Ported from sdma_v6_0_ctxempty_int_enable */
void sdma60_ctxempty_int_enable(rdna3_dev *d, uint32_t num_instances, int enable)
{
    uint32_t i, f32_cntl;
    for (i = 0; i < num_instances; i++) {
        f32_cntl = RS(d, i, regSDMA0_CNTL);
        f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL, CTXEMPTY_INT_ENABLE, enable ? 1 : 0);
        WS(d, i, regSDMA0_CNTL, f32_cntl);
    }
}

/* Ported from sdma_v6_0_gfx_stop */
static void sdma_v6_0_gfx_stop(rdna3_dev *d, uint32_t num_instances)
{
    uint32_t i, rb_cntl, ib_cntl;
    for (i = 0; i < num_instances; i++) {
        rb_cntl = RS(d, i, regSDMA0_QUEUE0_RB_CNTL);
        rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 0);
        WS(d, i, regSDMA0_QUEUE0_RB_CNTL, rb_cntl);
        ib_cntl = RS(d, i, regSDMA0_QUEUE0_IB_CNTL);
        ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_QUEUE0_IB_CNTL, IB_ENABLE, 0);
        WS(d, i, regSDMA0_QUEUE0_IB_CNTL, ib_cntl);
    }
}

/* Ported from sdma_v6_0_enable */
static void sdma_v6_0_enable(rdna3_dev *d, uint32_t num_instances, int enable)
{
    uint32_t i, f32_cntl;
    if (!enable)
        sdma_v6_0_gfx_stop(d, num_instances);   /* sdma_v6_0_rlc_stop: nothing */
    for (i = 0; i < num_instances; i++) {
        f32_cntl = RS(d, i, regSDMA0_F32_CNTL);
        f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, enable ? 0 : 1);
        WS(d, i, regSDMA0_F32_CNTL, f32_cntl);
    }
}

/* Ported from sdma_v6_0_hw_fini (= sdma_v6_0_suspend) */
void sdma60_hw_fini(rdna3_dev *d, uint32_t num_instances)
{
    sdma60_ctxempty_int_enable(d, num_instances, 0);
    sdma_v6_0_enable(d, num_instances, 0);
}

/* ------------------- rdna3amdgpu=1: amdgpu's SDMA ring functions ------------------- */

#define SDMA_POLL_HDP_FLUSH   (1u << 26)
#define SDMA_POLL_FUNC(x)     ((uint32_t)(x) << 28)
#define SDMA_POLL_MEM         (1u << 31)
#define SDMA_POLL_DW5(retry, interval) (((uint32_t)(retry) << 16) | (uint32_t)(interval))

/* Ported from sdma_v6_0_ring_insert_nop */
void sdma60_ring_insert_nop(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        if (r->burst_nop && i == 0)
            sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_NOP) | SDMA_PKT_NOP_HEADER_COUNT(count - 1));
        else
            sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_NOP));
}

/* Ported from sdma_v6_0_ring_emit_wreg (SRBM_WRITE, all bytes) */
void sdma60_emit_wreg(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t reg, uint32_t val)
{
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE) | (0xfu << 28));
    sdma60_write(d, r, reg);
    sdma60_write(d, r, val);
}

/* Ported from sdma_v6_0_ring_emit_reg_wait */
void sdma60_emit_reg_wait(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t reg, uint32_t val, uint32_t mask)
{
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) | SDMA_POLL_FUNC(3));   /* equal */
    sdma60_write(d, r, reg << 2);
    sdma60_write(d, r, 0);
    sdma60_write(d, r, val);    /* reference */
    sdma60_write(d, r, mask);   /* mask */
    sdma60_write(d, r, SDMA_POLL_DW5(0xfff, 10));
}

/* Ported from sdma_v6_0_ring_emit_reg_write_reg_wait */
void sdma60_emit_reg_write_reg_wait(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t reg0, uint32_t reg1,
                                    uint32_t ref, uint32_t mask)
{
    sdma60_emit_wreg(d, r, reg0, ref);
    /* wait for a cycle to reset vm_inv_eng*_ack */
    sdma60_emit_reg_wait(d, r, reg0, 0, 0);
    sdma60_emit_reg_wait(d, r, reg1, mask, mask);
}

/* Ported from sdma_v6_0_ring_emit_vm_flush (the ring's hub is the GFXHUB) */
void sdma60_emit_vm_flush(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t vmid, uint64_t pd_addr)
{
    rdna3_vm_flush_regs f;
    gfxhub303_flush_regs(d, vmid, r->vm_inv_eng, &f);
    /* Update the PD address for this VMID. */
    sdma60_emit_wreg(d, r, f.pt_lo, (uint32_t)pd_addr);
    sdma60_emit_wreg(d, r, f.pt_hi, (uint32_t)(pd_addr >> 32));
    /* Trigger invalidation. */
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_VM_INVALIDATION) |
                       ((r->vm_inv_eng & 0x1f) << 16) /* GFX_ENG_ID */ | (0x1fu << 24) /* MM_ENG_ID */);
    sdma60_write(d, r, f.req_val);
    sdma60_write(d, r, 0xFFFFFFFF);
    sdma60_write(d, r, ((1u << vmid) & 0xffff) /* INVALIDATEACK */ | (0x1fu << 16) /* ADDRESSRANGEHI */);
}

/* Ported from sdma_v6_0_ring_emit_pipeline_sync */
void sdma60_emit_pipeline_sync(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint32_t seq = r->fence_seq;   /* ring->fence_drv.sync_seq */
    uint64_t addr = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_FENCE);
    /* wait for idle */
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) | SDMA_POLL_FUNC(3) | SDMA_POLL_MEM);
    sdma60_write(d, r, (uint32_t)addr & 0xfffffffc);
    sdma60_write(d, r, (uint32_t)(addr >> 32));
    sdma60_write(d, r, seq);           /* reference */
    sdma60_write(d, r, 0xffffffff);    /* mask */
    sdma60_write(d, r, SDMA_POLL_DW5(0xfff, 4));
}

/* Ported from sdma_v6_0_ring_emit_hdp_flush (nbio_v4_3 flush registers,
 * ref_and_mask_sdma0 << ring->me) */
void sdma60_emit_hdp_flush(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint32_t ref_and_mask = BIF_BX_PF_GPU_HDP_FLUSH_DONE__SDMA0_MASK << r->inst;
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) | SDMA_POLL_HDP_FLUSH | SDMA_POLL_FUNC(3));
    sdma60_write(d, r, SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_GPU_HDP_FLUSH_DONE) << 2);
    sdma60_write(d, r, SOC15_OFF(d, NBIO, 0, regBIF_BX_PF0_GPU_HDP_FLUSH_REQ) << 2);
    sdma60_write(d, r, ref_and_mask);   /* reference */
    sdma60_write(d, r, ref_and_mask);   /* mask */
    sdma60_write(d, r, SDMA_POLL_DW5(0xfff, 10));
}

/* Ported from sdma_v6_0_ring_emit_mem_sync: flush entire cache L0/L1/L2 */
void sdma60_emit_mem_sync(rdna3_dev *d, rdna3_sdma_ring *r)
{
    const uint32_t gcr_cntl = (1u << 14) /* GL2_INV */ | (1u << 15) /* GL2_WB */ | (1u << 5) /* GLM_INV */ |
                              (1u << 9) /* GL1_INV */ | (1u << 8) /* GLV_INV */ | (1u << 7) /* GLK_INV */ |
                              (1u << 0) /* GLI_INV(1) */;
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_GCR_REQ));
    sdma60_write(d, r, 0);                                   /* BASE_VA_31_7 */
    sdma60_write(d, r, ((gcr_cntl & 0xffff) << 16) | 0);    /* GCR_CONTROL_15_0, BASE_VA_47_32 */
    sdma60_write(d, r, (0u << 7) | (gcr_cntl >> 16));        /* LIMIT_VA_31_7, GCR_CONTROL_18_16 */
    sdma60_write(d, r, 0 | (0u << 24));                      /* LIMIT_VA_47_32, VMID */
}

/* Ported from sdma_v6_0_ring_init_cond_exec */
uint32_t sdma60_ring_init_cond_exec(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t addr)
{
    uint32_t ret;
    sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_COND_EXE));
    sdma60_write(d, r, (uint32_t)addr);
    sdma60_write(d, r, (uint32_t)(addr >> 32));
    sdma60_write(d, r, 1);
    /* this is the offset we need patch later */
    ret = (uint32_t)r->wptr & r->buf_mask;
    /* insert dummy here and patch it later */
    sdma60_write(d, r, 0);
    return ret;
}

/* amdgpu_ring_patch_cond_exec */
void sdma60_ring_patch_cond_exec(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t offset)
{
    uint32_t cur = ((uint32_t)r->wptr - 1) & r->buf_mask, v;
    if (cur < offset)
        cur += r->ring_size >> 2;
    v = cur - offset;
    rdna3_vram_write(d, r->ring + 4ull * (offset & r->buf_mask), &v, 4);
}

/* Ported from sdma_v6_0_ring_pad_ib */
void sdma60_ring_pad_ib(const rdna3_sdma_ring *r, uint32_t *ib, uint32_t *length_dw)
{
    uint32_t pad_count = (0u - *length_dw) & 0x7, i;
    for (i = 0; i < pad_count; i++)
        if (r && r->burst_nop && i == 0)
            ib[(*length_dw)++] = SDMA_PKT_HEADER_OP(SDMA_OP_NOP) | SDMA_PKT_NOP_HEADER_COUNT(pad_count - 1);
        else
            ib[(*length_dw)++] = SDMA_PKT_HEADER_OP(SDMA_OP_NOP);
}

/* Ported from sdma_v6_0_vm_copy_pte */
void sdma60_vm_copy_pte(uint32_t *ib, uint32_t *length_dw, uint64_t pe, uint64_t src, uint32_t count)
{
    uint32_t bytes = count * 8;
    ib[(*length_dw)++] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR);
    ib[(*length_dw)++] = bytes - 1;
    ib[(*length_dw)++] = 0; /* src/dst endian swap */
    ib[(*length_dw)++] = (uint32_t)src;
    ib[(*length_dw)++] = (uint32_t)(src >> 32);
    ib[(*length_dw)++] = (uint32_t)pe;
    ib[(*length_dw)++] = (uint32_t)(pe >> 32);
}

/* Ported from sdma_v6_0_vm_write_pte */
void sdma60_vm_write_pte(uint32_t *ib, uint32_t *length_dw, uint64_t pe, uint64_t value, uint32_t count,
                         uint32_t incr)
{
    uint32_t ndw = count * 2;
    ib[(*length_dw)++] = SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR);
    ib[(*length_dw)++] = (uint32_t)pe;
    ib[(*length_dw)++] = (uint32_t)(pe >> 32);
    ib[(*length_dw)++] = ndw - 1;
    for (; ndw > 0; ndw -= 2) {
        ib[(*length_dw)++] = (uint32_t)value;
        ib[(*length_dw)++] = (uint32_t)(value >> 32);
        value += incr;
    }
}

/* Ported from sdma_v6_0_vm_set_pte_pde: for physically contiguous pages (vram) */
void sdma60_vm_set_pte_pde(uint32_t *ib, uint32_t *length_dw, uint64_t pe, uint64_t addr, uint32_t count,
                           uint32_t incr, uint64_t flags)
{
    ib[(*length_dw)++] = SDMA_PKT_HEADER_OP(SDMA_OP_PTEPDE);
    ib[(*length_dw)++] = (uint32_t)pe;              /* dst addr */
    ib[(*length_dw)++] = (uint32_t)(pe >> 32);
    ib[(*length_dw)++] = (uint32_t)flags;           /* mask */
    ib[(*length_dw)++] = (uint32_t)(flags >> 32);
    ib[(*length_dw)++] = (uint32_t)addr;            /* value */
    ib[(*length_dw)++] = (uint32_t)(addr >> 32);
    ib[(*length_dw)++] = incr;                      /* increment size */
    ib[(*length_dw)++] = 0;
    ib[(*length_dw)++] = count - 1;                 /* number of entries */
}

/* Ported from sdma_v6_0_ring_get_rptr: the rptr writeback is in bytes */
uint64_t sdma60_ring_get_rptr(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint64_t rptr = 0;
    rdna3_vram_read(d, r->wb + RD_SDMA_WB_RPTR, &rptr, 8);
    return rptr >> 2;
}

/* Ported from sdma_v6_0_ring_get_wptr: the poll slot (bytes) with a doorbell */
uint64_t sdma60_ring_get_wptr(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint64_t wptr = 0;
    if (r->use_doorbell)
        rdna3_vram_read(d, r->wb + RD_SDMA_WB_WPTR, &wptr, 8);
    return wptr >> 2;
}

/* Ported from sdma_v6_0_is_idle */
int sdma60_is_idle(rdna3_dev *d, uint32_t num_instances)
{
    uint32_t i;
    for (i = 0; i < num_instances; i++)
        if (!(RS(d, i, regSDMA0_STATUS_REG) & SDMA0_STATUS_REG__IDLE_MASK))
            return 0;
    return 1;
}

/* Ported from sdma_v6_0_wait_for_idle (both instances) */
int sdma60_wait_for_idle(rdna3_dev *d)
{
    uint32_t i;
    for (i = 0; i < d->usec_timeout; i++) {
        uint32_t sdma0 = RS(d, 0, regSDMA0_STATUS_REG), sdma1 = RS(d, 1, regSDMA0_STATUS_REG);
        if (sdma0 & sdma1 & SDMA0_STATUS_REG__IDLE_MASK)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    return RDNA3_ETIME;
}

/* free dwords in the ring (one slot stays empty) */
static uint32_t ring_space(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint32_t rptr = (uint32_t)(sdma60_ring_get_rptr(d, r)) & r->buf_mask;
    return r->buf_mask - (((uint32_t)r->wptr - rptr) & r->buf_mask);
}

/* amdgpu_ib_schedule on an SDMA ring (see sdma60.h) */
uint32_t sdma60_submit_job(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t vmid, uint64_t pd_addr,
                           uint32_t pasid, uint32_t pasid_reg, const uint64_t *ib_va,
                           const uint32_t *ib_ndw, const uint32_t *ib_flags, uint32_t n,
                           uint64_t uf_addr)
{
    uint64_t cond_addr = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_COND_EXE);
    uint32_t i, seq, patch, cond_exec, ndw = 6 + 5 + 6 + 6 + 10 + 3 + 6 + 5 + 6 + 14 * n + 16 + 16;
    int need_pipe = vmid && pd_addr;   /* amdgpu_vm_need_pipeline_sync */
    if (!r->ready || !n)
        return 0;
    ndw = (ndw + 0xf) & ~0xfu;   /* amdgpu_ring_alloc */
    for (i = 0; i < d->usec_timeout && ring_space(d, r) < ndw; i++)
        d->os.udelay(d->os.ctx, 1);
    if (ring_space(d, r) < ndw) {
        RERR(d, "SDMA%u: ring full", r->inst);
        return 0;
    }
    if (ib_flags[0] & (1u << 6))   /* AMDGPU_IB_FLAG_EMIT_MEM_SYNC */
        sdma60_emit_mem_sync(d, r);
    /* amdgpu_vm_flush (no GDS on the DMA ring) */
    if (pd_addr || need_pipe) {
        patch = sdma60_ring_init_cond_exec(d, r, cond_addr);
        if (need_pipe)
            sdma60_emit_pipeline_sync(d, r);
        if (pd_addr)
            sdma60_emit_vm_flush(d, r, vmid, pd_addr);
        if (pasid)
            sdma60_emit_wreg(d, r, pasid_reg, pasid);   /* gmc_v11_0_emit_pasid_mapping */
        if (pd_addr || pasid)
            (void)sdma60_emit_fence(d, r);
        sdma60_ring_patch_cond_exec(d, r, patch);
    }
    cond_exec = sdma60_ring_init_cond_exec(d, r, cond_addr);
    sdma60_emit_hdp_flush(d, r);   /* amdgpu_device_flush_hdp */
    for (i = 0; i < n; i++)
        sdma60_emit_ib(d, r, vmid, ib_va[i], ib_ndw[i]);
    if (uf_addr)
        sdma60_emit_fence64(d, r, uf_addr, (uint32_t)(r->fence_seq + 1));
    seq = sdma60_emit_fence(d, r);
    sdma60_ring_patch_cond_exec(d, r, cond_exec);
    sdma60_commit(d, r);
    return seq;
}

/* Ported from sdma_v6_0_ring_test_ib */
int sdma60_ring_test_ib(rdna3_dev *d, rdna3_sdma_ring *r, uint64_t ib)
{
    uint64_t gpu_addr = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_IB_TEST), va;
    uint32_t tmp = 0xCAFEDEAD, p[8], ndw = 8, flags = 0, seq;
    int ret;
    if (!r->ready || ib == UINT64_MAX)
        return RDNA3_EINVAL;
    rdna3_vram_write(d, r->wb + RD_SDMA_WB_IB_TEST, &tmp, 4);
    p[0] = SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR);
    p[1] = (uint32_t)gpu_addr;
    p[2] = (uint32_t)(gpu_addr >> 32);
    p[3] = 0;   /* SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(0) */
    p[4] = 0xDEADBEEF;
    p[5] = p[6] = p[7] = SDMA_PKT_HEADER_OP(SDMA_OP_NOP);
    rdna3_vram_write(d, ib, p, sizeof(p));
    rdna3_hdp_flush(d);
    va = rdna3_vram_mc(d, ib);
    seq = sdma60_submit_job(d, r, 0, 0, 0, 0, &va, &ndw, &flags, 1, 0);
    if (!seq)
        return RDNA3_EINVAL;
    ret = sdma60_wait_fence(d, r, seq);
    if (ret)
        return ret;
    rdna3_vram_read(d, r->wb + RD_SDMA_WB_IB_TEST, &tmp, 4);
    return tmp == 0xDEADBEEF ? RDNA3_OK : RDNA3_EINVAL;
}
