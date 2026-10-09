/*
 * sdma60.c - see sdma60.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sdma60.h"
#include "gen/gc_11_0_0_sdma.h"

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
void sdma60_commit(rdna3_dev *d, rdna3_sdma_ring *r)
{
    uint64_t w = r->wptr << 2;
    rdna3_vram_write(d, r->wb + RD_SDMA_WB_WPTR, &w, 8);
    rdna3_hdp_flush(d);
    WS(d, r->inst, regSDMA0_QUEUE0_RB_WPTR, (uint32_t)w);
    WS(d, r->inst, regSDMA0_QUEUE0_RB_WPTR_HI, (uint32_t)(w >> 32));
}

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
    uint32_t pad = (2 - (uint32_t)r->wptr) & 7, i;
    for (i = 0; i < pad; i++)
        sdma60_write(d, r, SDMA_PKT_HEADER_OP(SDMA_OP_NOP));
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

int sdma60_start(rdna3_dev *d, rdna3_sdma_ring *r, uint32_t inst, uint32_t ring_size)
{
    uint32_t rb_cntl, ib_cntl, tmp;
    uint64_t ring_mc, rptr_mc, wptr_mc;
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
    rdna3_hdp_flush(d);
    ring_mc = rdna3_vram_mc(d, r->ring);
    rptr_mc = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_RPTR);
    wptr_mc = rdna3_vram_mc(d, r->wb + RD_SDMA_WB_WPTR);

    /* sdma_v6_0_start: unhalt, ctxempty interrupt */
    tmp = RS(d, inst, regSDMA0_F32_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, HALT, 0);
    WS(d, inst, regSDMA0_F32_CNTL, tmp);
    tmp = RS(d, inst, regSDMA0_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_CNTL, CTXEMPTY_INT_ENABLE, 1);
    WS(d, inst, regSDMA0_CNTL, tmp);

    /* sdma_v6_0_gfx_resume_instance */
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

    tmp = RS(d, inst, regSDMA0_QUEUE0_DOORBELL);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_DOORBELL, ENABLE, 0);
    WS(d, inst, regSDMA0_QUEUE0_DOORBELL, tmp);
    WS(d, inst, regSDMA0_QUEUE0_DOORBELL_OFFSET, RS(d, inst, regSDMA0_QUEUE0_DOORBELL_OFFSET));
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

/* sdma_v6_0_gfx_stop + halt */
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
