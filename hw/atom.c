/*
 * atom.c - see atom.h. Port of Linux amdgpu atom.c (MIT):
 * Copyright 2008 Advanced Micro Devices, Inc. Author: Stanislaw Skowronek.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 *
 * SPDX-License-Identifier: MIT
 */
#include "atom.h"

#define ATOM_ROM_CMD_PTR    0x1E
#define ATOM_ROM_DATA_PTR   0x20
#define ATOM_DATA_FWI_PTR   0xC
#define ATOM_DATA_IIO_PTR   0x32
#define ATOM_FWI_DEFSCLK_PTR 8
#define ATOM_FWI_DEFMCLK_PTR 0xC
#define ATOM_FWI_SCRATCH_PTR 0x18   /* atom_firmware_info_v3_1.bios_scratch_reg_startaddr */
#define ATOM_S7_ASIC_INIT_COMPLETE_MASK 0x00000200u

#define ATOM_CT_SIZE_PTR    0
#define ATOM_CT_WS_PTR      4
#define ATOM_CT_PS_PTR      5
#define ATOM_CT_PS_MASK     0x7F
#define ATOM_CT_CODE_PTR    6

#define ATOM_OP_CNT         127
#define ATOM_OP_EOT         91

#define ATOM_CASE_MAGIC     0x63
#define ATOM_CASE_END       0x5A5A

#define ATOM_ARG_REG 0
#define ATOM_ARG_PS  1
#define ATOM_ARG_WS  2
#define ATOM_ARG_FB  3
#define ATOM_ARG_ID  4
#define ATOM_ARG_IMM 5
#define ATOM_ARG_PLL 6
#define ATOM_ARG_MC  7

#define ATOM_SRC_DWORD  0
#define ATOM_SRC_WORD0  1
#define ATOM_SRC_WORD8  2
#define ATOM_SRC_WORD16 3
#define ATOM_SRC_BYTE0  4
#define ATOM_SRC_BYTE8  5
#define ATOM_SRC_BYTE16 6
#define ATOM_SRC_BYTE24 7

#define ATOM_WS_QUOTIENT   0x40
#define ATOM_WS_REMAINDER  0x41
#define ATOM_WS_DATAPTR    0x42
#define ATOM_WS_SHIFT      0x43
#define ATOM_WS_OR_MASK    0x44
#define ATOM_WS_AND_MASK   0x45
#define ATOM_WS_FB_WINDOW  0x46
#define ATOM_WS_ATTRIBUTES 0x47
#define ATOM_WS_REGPTR     0x48

#define ATOM_IIO_NOP        0
#define ATOM_IIO_START      1
#define ATOM_IIO_READ       2
#define ATOM_IIO_WRITE      3
#define ATOM_IIO_CLEAR      4
#define ATOM_IIO_SET        5
#define ATOM_IIO_MOVE_INDEX 6
#define ATOM_IIO_MOVE_ATTR  7
#define ATOM_IIO_MOVE_DATA  8
#define ATOM_IIO_END        9

#define ATOM_IO_MM    0
#define ATOM_IO_PCI   1
#define ATOM_IO_SYSIO 2
#define ATOM_IO_IIO   0x80

#define ATOM_COND_ABOVE        0
#define ATOM_COND_ABOVEOREQUAL 1
#define ATOM_COND_ALWAYS       2
#define ATOM_COND_BELOW        3
#define ATOM_COND_BELOWOREQUAL 4
#define ATOM_COND_EQUAL        5
#define ATOM_COND_NOTEQUAL     6

#define ATOM_PORT_ATI   0
#define ATOM_PORT_PCI   1
#define ATOM_PORT_SYSIO 2

#define ATOM_UNIT_MICROSEC 0
#define ATOM_UNIT_MILLISEC 1

typedef struct {
    rdna3_atom *ctx;
    uint32_t *ps, *ws;
    int ps_size, ws_size;
    int ps_shift;
    uint16_t start;
    unsigned last_jump;
    uint64_t last_jump_us;
    int abort;
} atom_exec;

static const uint32_t atom_arg_mask[8] = { 0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000,
                                           0xFF, 0xFF00, 0xFF0000, 0xFF000000 };
static const int atom_arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };
static const int atom_dst_to_src[8][4] = {
    /* translate destination alignment field to the source alignment encoding */
    { 0, 0, 0, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 },
    { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 },
};
static const int atom_def_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

/* ---- bounds-checked image access (CU8/CU16/CU32) ---- */
static uint32_t cu8(rdna3_atom *a, uint32_t off)
{
    if (off >= a->bios_size) {
        a->bad_access = 1;
        return 0;
    }
    return a->bios[off];
}
static uint32_t cu16(rdna3_atom *a, uint32_t off)
{
    if ((uint64_t)off + 2 > a->bios_size) {
        a->bad_access = 1;
        return 0;
    }
    return (uint32_t)a->bios[off] | ((uint32_t)a->bios[off + 1] << 8);
}
static uint32_t cu32(rdna3_atom *a, uint32_t off)
{
    if ((uint64_t)off + 4 > a->bios_size) {
        a->bad_access = 1;
        return 0;
    }
    return (uint32_t)a->bios[off] | ((uint32_t)a->bios[off + 1] << 8) |
           ((uint32_t)a->bios[off + 2] << 16) | ((uint32_t)a->bios[off + 3] << 24);
}
#define U8(p)  cu8(ctx->ctx, (uint32_t)(p))
#define U16(p) cu16(ctx->ctx, (uint32_t)(p))
#define U32(p) cu32(ctx->ctx, (uint32_t)(p))

static uint32_t mask_bits(uint32_t n)   /* 0xFFFFFFFF >> (32 - n), n = 0 safe */
{
    return n == 0 ? 0 : (n >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu >> (32 - n)));
}

static void reg_write(rdna3_atom *a, uint32_t reg, uint32_t v) { rdna3_wreg(a->d, reg, v); }
static uint32_t reg_read(rdna3_atom *a, uint32_t reg) { return rdna3_rreg(a->d, reg); }

static uint32_t atom_iio_execute(rdna3_atom *a, uint32_t base, uint32_t index, uint32_t data)
{
    uint32_t temp = 0xCDCDCDCD, start = base;

    /* IIO opcodes read up to base+3; keep within the BIOS image */
    while (base + 3 < a->bios_size)
        switch (cu8(a, base)) {
        case ATOM_IIO_NOP:
            base++;
            break;
        case ATOM_IIO_READ:
            temp = reg_read(a, cu16(a, base + 1));
            base += 3;
            break;
        case ATOM_IIO_WRITE:
            reg_write(a, cu16(a, base + 1), temp);
            base += 3;
            break;
        case ATOM_IIO_CLEAR:
            temp &= ~(mask_bits(cu8(a, base + 1)) << cu8(a, base + 2));
            base += 3;
            break;
        case ATOM_IIO_SET:
            temp |= mask_bits(cu8(a, base + 1)) << cu8(a, base + 2);
            base += 3;
            break;
        case ATOM_IIO_MOVE_INDEX:
            temp &= ~(mask_bits(cu8(a, base + 1)) << cu8(a, base + 3));
            temp |= ((index >> cu8(a, base + 2)) & mask_bits(cu8(a, base + 1))) << cu8(a, base + 3);
            base += 4;
            break;
        case ATOM_IIO_MOVE_DATA:
            temp &= ~(mask_bits(cu8(a, base + 1)) << cu8(a, base + 3));
            temp |= ((data >> cu8(a, base + 2)) & mask_bits(cu8(a, base + 1))) << cu8(a, base + 3);
            base += 4;
            break;
        case ATOM_IIO_MOVE_ATTR:
            temp &= ~(mask_bits(cu8(a, base + 1)) << cu8(a, base + 3));
            temp |= (((uint32_t)a->io_attr >> cu8(a, base + 2)) & mask_bits(cu8(a, base + 1)))
                    << cu8(a, base + 3);
            base += 4;
            break;
        case ATOM_IIO_END:
            return temp;
        default:
            RERR(a->d, "ATOM: unknown IIO opcode 0x%02x at 0x%04x", cu8(a, base), base);
            return 0;
        }
    RERR(a->d, "ATOM: IIO method at 0x%04x runs past the image", start);
    return 0;
}

static uint32_t atom_get_src_int(atom_exec *ctx, uint8_t attr, int *ptr, uint32_t *saved)
{
    uint32_t idx, val = 0xCDCDCDCD, align, arg;
    rdna3_atom *g = ctx->ctx;
    arg = attr & 7;
    align = (attr >> 3) & 7;
    switch (arg) {
    case ATOM_ARG_REG:
        idx = U16(*ptr);
        (*ptr) += 2;
        idx += g->reg_block;
        switch (g->io_mode) {
        case ATOM_IO_MM:
            val = reg_read(g, idx);
            break;
        case ATOM_IO_PCI:
        case ATOM_IO_SYSIO:
            g->unimplemented_io++;
            RERR(g->d, "ATOM: PCI/SYSIO register read not implemented");
            return 0;
        default:
            if (!(g->io_mode & 0x80) || !g->iio[g->io_mode & 0x7F]) {
                RERR(g->d, "ATOM: undefined indirect IO read method %d", g->io_mode & 0x7F);
                return 0;
            }
            val = atom_iio_execute(g, g->iio[g->io_mode & 0x7F], idx, 0);
        }
        break;
    case ATOM_ARG_PS:
        idx = U8(*ptr);
        (*ptr)++;
        if ((int)idx < ctx->ps_size)
            val = ctx->ps[idx];
        else
            RERR(g->d, "ATOM: PS index out of range: %u > %d", idx, ctx->ps_size);
        break;
    case ATOM_ARG_WS:
        idx = U8(*ptr);
        (*ptr)++;
        switch (idx) {
        case ATOM_WS_QUOTIENT:   val = g->divmul[0]; break;
        case ATOM_WS_REMAINDER:  val = g->divmul[1]; break;
        case ATOM_WS_DATAPTR:    val = g->data_block; break;
        case ATOM_WS_SHIFT:      val = g->shift; break;
        case ATOM_WS_OR_MASK:    val = 1u << (g->shift & 31); break;
        case ATOM_WS_AND_MASK:   val = ~(1u << (g->shift & 31)); break;
        case ATOM_WS_FB_WINDOW:  val = g->fb_base; break;
        case ATOM_WS_ATTRIBUTES: val = g->io_attr; break;
        case ATOM_WS_REGPTR:     val = g->reg_block; break;
        default:
            if ((int)idx < ctx->ws_size)
                val = ctx->ws[idx];
            else
                RERR(g->d, "ATOM: WS index out of range: %u > %d", idx, ctx->ws_size);
        }
        break;
    case ATOM_ARG_ID:
        idx = U16(*ptr);
        (*ptr) += 2;
        val = U32(idx + g->data_block);
        break;
    case ATOM_ARG_FB:
        idx = U8(*ptr);
        (*ptr)++;
        if (!g->scratch || (uint64_t)g->fb_base + idx * 4 + 4 > g->scratch_bytes) {
            RERR(g->d, "ATOM: fb read beyond scratch region: %u vs. %u", g->fb_base + idx * 4,
                 g->scratch_bytes);
            val = 0;
        } else {
            val = g->scratch[(g->fb_base / 4) + idx];
        }
        break;
    case ATOM_ARG_IMM:
        switch (align) {
        case ATOM_SRC_DWORD:
            val = U32(*ptr);
            (*ptr) += 4;
            break;
        case ATOM_SRC_WORD0:
        case ATOM_SRC_WORD8:
        case ATOM_SRC_WORD16:
            val = U16(*ptr);
            (*ptr) += 2;
            break;
        default:
            val = U8(*ptr);
            (*ptr)++;
            break;
        }
        return val;
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
        /* no PLL / MC register spaces on SOC21 (amdgpu: "not supported") */
        (*ptr)++;
        g->unimplemented_io++;
        RERR(g->d, "ATOM: %s read not supported", arg == ATOM_ARG_PLL ? "PLL" : "MC");
        val = 0;
        break;
    }
    if (saved)
        *saved = val;
    val &= atom_arg_mask[align];
    val >>= atom_arg_shift[align];
    return val;
}

static void atom_skip_src_int(atom_exec *ctx, uint8_t attr, int *ptr)
{
    uint32_t align = (attr >> 3) & 7, arg = attr & 7;
    (void)ctx;
    switch (arg) {
    case ATOM_ARG_REG:
    case ATOM_ARG_ID:
        (*ptr) += 2;
        break;
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
    case ATOM_ARG_PS:
    case ATOM_ARG_WS:
    case ATOM_ARG_FB:
        (*ptr)++;
        break;
    case ATOM_ARG_IMM:
        switch (align) {
        case ATOM_SRC_DWORD:
            (*ptr) += 4;
            return;
        case ATOM_SRC_WORD0:
        case ATOM_SRC_WORD8:
        case ATOM_SRC_WORD16:
            (*ptr) += 2;
            return;
        default:
            (*ptr)++;
            return;
        }
    }
}

static uint32_t atom_get_src(atom_exec *ctx, uint8_t attr, int *ptr)
{
    return atom_get_src_int(ctx, attr, ptr, NULL);
}

static uint32_t atom_get_src_direct(atom_exec *ctx, uint8_t align, int *ptr)
{
    uint32_t val = 0xCDCDCDCD;
    switch (align) {
    case ATOM_SRC_DWORD:
        val = U32(*ptr);
        (*ptr) += 4;
        break;
    case ATOM_SRC_WORD0:
    case ATOM_SRC_WORD8:
    case ATOM_SRC_WORD16:
        val = U16(*ptr);
        (*ptr) += 2;
        break;
    default:
        val = U8(*ptr);
        (*ptr)++;
        break;
    }
    return val;
}

static uint32_t atom_get_dst(atom_exec *ctx, int arg, uint8_t attr, int *ptr, uint32_t *saved)
{
    return atom_get_src_int(ctx, (uint8_t)(arg | atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3),
                            ptr, saved);
}

static void atom_skip_dst(atom_exec *ctx, int arg, uint8_t attr, int *ptr)
{
    atom_skip_src_int(ctx, (uint8_t)(arg | atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3),
                      ptr);
}

static void atom_put_dst(atom_exec *ctx, int arg, uint8_t attr, int *ptr, uint32_t val,
                         uint32_t saved)
{
    uint32_t align = (uint32_t)atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3], idx;
    rdna3_atom *g = ctx->ctx;
    val <<= atom_arg_shift[align];
    val &= atom_arg_mask[align];
    saved &= ~atom_arg_mask[align];
    val |= saved;
    switch (arg) {
    case ATOM_ARG_REG:
        idx = U16(*ptr);
        (*ptr) += 2;
        idx += g->reg_block;
        switch (g->io_mode) {
        case ATOM_IO_MM:
            if (idx == 0)
                reg_write(g, idx, val << 2);
            else
                reg_write(g, idx, val);
            break;
        case ATOM_IO_PCI:
        case ATOM_IO_SYSIO:
            g->unimplemented_io++;
            RERR(g->d, "ATOM: PCI/SYSIO register write not implemented");
            return;
        default:
            /* amdgpu indexes with io_mode & 0xFF here; iio[] has 256 slots */
            if (!(g->io_mode & 0x80) || !g->iio[g->io_mode & 0xFF]) {
                RERR(g->d, "ATOM: undefined indirect IO write method %d", g->io_mode & 0x7F);
                return;
            }
            atom_iio_execute(g, g->iio[g->io_mode & 0xFF], idx, val);
        }
        break;
    case ATOM_ARG_PS:
        idx = U8(*ptr);
        (*ptr)++;
        if ((int)idx >= ctx->ps_size) {
            RERR(g->d, "ATOM: PS index out of range: %u > %d", idx, ctx->ps_size);
            return;
        }
        ctx->ps[idx] = val;
        break;
    case ATOM_ARG_WS:
        idx = U8(*ptr);
        (*ptr)++;
        switch (idx) {
        case ATOM_WS_QUOTIENT:   g->divmul[0] = val; break;
        case ATOM_WS_REMAINDER:  g->divmul[1] = val; break;
        case ATOM_WS_DATAPTR:    g->data_block = (uint16_t)val; break;
        case ATOM_WS_SHIFT:      g->shift = (uint8_t)val; break;
        case ATOM_WS_OR_MASK:
        case ATOM_WS_AND_MASK:   break;
        case ATOM_WS_FB_WINDOW:  g->fb_base = val; break;
        case ATOM_WS_ATTRIBUTES: g->io_attr = (uint16_t)val; break;
        case ATOM_WS_REGPTR:     g->reg_block = (uint16_t)val; break;
        default:
            if ((int)idx >= ctx->ws_size) {
                RERR(g->d, "ATOM: WS index out of range: %u > %d", idx, ctx->ws_size);
                return;
            }
            ctx->ws[idx] = val;
        }
        break;
    case ATOM_ARG_FB:
        idx = U8(*ptr);
        (*ptr)++;
        if (!g->scratch || (uint64_t)g->fb_base + idx * 4 + 4 > g->scratch_bytes)
            RERR(g->d, "ATOM: fb write beyond scratch region: %u vs. %u", g->fb_base + idx * 4,
                 g->scratch_bytes);
        else
            g->scratch[(g->fb_base / 4) + idx] = val;
        break;
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
        (*ptr)++;
        g->unimplemented_io++;
        RERR(g->d, "ATOM: %s write not supported", arg == ATOM_ARG_PLL ? "PLL" : "MC");
        return;
    }
}

/* ---- opcodes ---- */
#define BINOP(name, expr)                                                    \
    static void atom_op_##name(atom_exec *ctx, int *ptr, int arg)            \
    {                                                                        \
        uint8_t attr = (uint8_t)U8((*ptr)++);                                \
        uint32_t dst, src, saved = 0;                                        \
        int dptr = *ptr;                                                     \
        dst = atom_get_dst(ctx, arg, attr, ptr, &saved);                     \
        src = atom_get_src(ctx, attr, ptr);                                  \
        expr;                                                                \
        atom_put_dst(ctx, arg, attr, &dptr, dst, saved);                     \
    }
BINOP(add, dst += src)
BINOP(and, dst &= src)
BINOP(or,  dst |= src)
BINOP(sub, dst -= src)
BINOP(xor, dst ^= src)

static void atom_op_beep(atom_exec *ctx, int *ptr, int arg)
{
    (void)ptr; (void)arg;
    RLOG(ctx->ctx->d, "ATOM BIOS beeped!");
}

static int execute_locked(rdna3_atom *a, unsigned index, uint32_t *params, int params_size);

static void atom_op_calltable(atom_exec *ctx, int *ptr, int arg)
{
    unsigned idx = U8((*ptr)++);
    int r = 0;
    (void)arg;
    if (U16(ctx->ctx->cmd_table + 4 + 2 * idx))
        r = execute_locked(ctx->ctx, idx, ctx->ps + ctx->ps_shift, ctx->ps_size - ctx->ps_shift);
    if (r)
        ctx->abort = 1;
}

static void atom_op_clear(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t saved = 0;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= (uint8_t)(atom_def_dst[attr >> 3] << 6);
    atom_get_dst(ctx, arg, attr, ptr, &saved);
    atom_put_dst(ctx, arg, attr, &dptr, 0, saved);
}

static void atom_op_compare(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL);
    src = atom_get_src(ctx, attr, ptr);
    ctx->ctx->cs_equal = (dst == src);
    ctx->ctx->cs_above = (dst > src);
}

static void atom_op_delay(atom_exec *ctx, int *ptr, int arg)
{
    unsigned count = U8((*ptr)++);
    rdna3_dev *d = ctx->ctx->d;
    if (arg == ATOM_UNIT_MICROSEC) {
        d->os.udelay(d->os.ctx, count);
        ctx->ctx->delay_us += count;
    } else {
        if (d->os.msleep)
            d->os.msleep(d->os.ctx, count);
        else
            d->os.udelay(d->os.ctx, count * 1000u);
        ctx->ctx->delay_us += (uint64_t)count * 1000u;
    }
}

static void atom_op_div(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL);
    src = atom_get_src(ctx, attr, ptr);
    if (src != 0) {
        ctx->ctx->divmul[0] = dst / src;
        ctx->ctx->divmul[1] = dst % src;
    } else {
        ctx->ctx->divmul[0] = 0;
        ctx->ctx->divmul[1] = 0;
    }
}

static void atom_op_div32(atom_exec *ctx, int *ptr, int arg)
{
    uint64_t val64;
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL);
    src = atom_get_src(ctx, attr, ptr);
    if (src != 0) {
        val64 = dst;
        val64 |= ((uint64_t)ctx->ctx->divmul[1]) << 32;
        val64 /= src;
        ctx->ctx->divmul[0] = (uint32_t)val64;
        ctx->ctx->divmul[1] = (uint32_t)(val64 >> 32);
    } else {
        ctx->ctx->divmul[0] = 0;
        ctx->ctx->divmul[1] = 0;
    }
}

static void atom_op_eot(atom_exec *ctx, int *ptr, int arg)
{
    (void)ctx; (void)ptr; (void)arg;   /* functionally a nop */
}

static void atom_op_jump(atom_exec *ctx, int *ptr, int arg)
{
    int execute = 0;
    unsigned target = U16(*ptr);
    rdna3_atom *g = ctx->ctx;

    (*ptr) += 2;
    switch (arg) {
    case ATOM_COND_ABOVE:        execute = g->cs_above; break;
    case ATOM_COND_ABOVEOREQUAL: execute = g->cs_above || g->cs_equal; break;
    case ATOM_COND_ALWAYS:       execute = 1; break;
    case ATOM_COND_BELOW:        execute = !(g->cs_above || g->cs_equal); break;
    case ATOM_COND_BELOWOREQUAL: execute = !g->cs_above; break;
    case ATOM_COND_EQUAL:        execute = g->cs_equal; break;
    case ATOM_COND_NOTEQUAL:     execute = !g->cs_equal; break;
    }
    if (execute) {
        /* "atombios stuck in loop": the same backward target keeps being
         * taken for longer than ATOM_CMD_TIMEOUT_SEC of requested delays */
        if (ctx->last_jump == ctx->start + target) {
            if (g->delay_us - ctx->last_jump_us > RD_ATOM_TIMEOUT_US) {
                RERR(g->d, "ATOM: stuck in loop for more than %u s, aborting",
                     RD_ATOM_TIMEOUT_US / 1000000u);
                ctx->abort = 1;
            }
        } else {
            ctx->last_jump = ctx->start + target;
            ctx->last_jump_us = g->delay_us;
        }
        *ptr = ctx->start + (int)target;
    }
}

static void atom_op_mask(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, mask, src, saved = 0;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved);
    mask = atom_get_src_direct(ctx, ((attr >> 3) & 7), ptr);
    src = atom_get_src(ctx, attr, ptr);
    dst &= mask;
    dst |= src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_move(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t src, saved = 0;
    int dptr = *ptr;
    if (((attr >> 3) & 7) != ATOM_SRC_DWORD) {
        atom_get_dst(ctx, arg, attr, ptr, &saved);
    } else {
        atom_skip_dst(ctx, arg, attr, ptr);
        saved = 0xCDCDCDCD;
    }
    src = atom_get_src(ctx, attr, ptr);
    atom_put_dst(ctx, arg, attr, &dptr, src, saved);
}

static void atom_op_mul(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL);
    src = atom_get_src(ctx, attr, ptr);
    ctx->ctx->divmul[0] = dst * src;
}

static void atom_op_mul32(atom_exec *ctx, int *ptr, int arg)
{
    uint64_t val64;
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL);
    src = atom_get_src(ctx, attr, ptr);
    val64 = (uint64_t)dst * (uint64_t)src;
    ctx->ctx->divmul[0] = (uint32_t)val64;
    ctx->ctx->divmul[1] = (uint32_t)(val64 >> 32);
}

static void atom_op_nop(atom_exec *ctx, int *ptr, int arg)
{
    (void)ctx; (void)ptr; (void)arg;
}

static void atom_op_postcard(atom_exec *ctx, int *ptr, int arg)
{
    (void)arg;
    (*ptr)++;
    (void)ctx;
}

/* atom_op_repeat, atom_op_savereg, atom_op_restorereg */
static void atom_op_unimplemented(atom_exec *ctx, int *ptr, int arg)
{
    /* repeat / savereg / restorereg: amdgpu prints "unimplemented!" and
     * carries on without consuming operands; so do we */
    (void)ptr; (void)arg;
    ctx->ctx->unknown_ops++;
    RERR(ctx->ctx->d, "ATOM: unimplemented opcode");
}

static void atom_op_setdatablock(atom_exec *ctx, int *ptr, int arg)
{
    unsigned idx = U8(*ptr);
    (void)arg;
    (*ptr)++;
    if (!idx)
        ctx->ctx->data_block = 0;
    else if (idx == 255)
        ctx->ctx->data_block = ctx->start;
    else
        ctx->ctx->data_block = (uint16_t)U16(ctx->ctx->data_table + 4 + 2 * idx);
}

static void atom_op_setfbbase(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    (void)arg;
    ctx->ctx->fb_base = atom_get_src(ctx, attr, ptr);
}

static void atom_op_setport(atom_exec *ctx, int *ptr, int arg)
{
    unsigned port;
    switch (arg) {
    case ATOM_PORT_ATI:
        port = U16(*ptr);
        if (!port)
            ctx->ctx->io_mode = ATOM_IO_MM;
        else
            ctx->ctx->io_mode = (int)(ATOM_IO_IIO | port);
        (*ptr) += 2;
        break;
    case ATOM_PORT_PCI:
        ctx->ctx->io_mode = ATOM_IO_PCI;
        (*ptr)++;
        break;
    case ATOM_PORT_SYSIO:
        ctx->ctx->io_mode = ATOM_IO_SYSIO;
        (*ptr)++;
        break;
    }
}

static void atom_op_setregblock(atom_exec *ctx, int *ptr, int arg)
{
    (void)arg;
    ctx->ctx->reg_block = (uint16_t)U16(*ptr);
    (*ptr) += 2;
}

static void atom_op_shift_left(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++), shift;
    uint32_t saved = 0, dst;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= (uint8_t)(atom_def_dst[attr >> 3] << 6);
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved);
    shift = (uint8_t)atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr);
    dst = shift >= 32 ? 0 : dst << shift;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shift_right(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++), shift;
    uint32_t saved = 0, dst;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= (uint8_t)(atom_def_dst[attr >> 3] << 6);
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved);
    shift = (uint8_t)atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr);
    dst = shift >= 32 ? 0 : dst >> shift;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shl(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++), shift;
    uint32_t saved = 0, dst;
    int dptr = *ptr;
    uint32_t dst_align = (uint32_t)atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    atom_get_dst(ctx, arg, attr, ptr, &saved);
    dst = saved;   /* the op needs the full dst value */
    shift = (uint8_t)atom_get_src(ctx, attr, ptr);
    dst = shift >= 32 ? 0 : dst << shift;
    dst &= atom_arg_mask[dst_align];
    dst >>= atom_arg_shift[dst_align];
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shr(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++), shift;
    uint32_t saved = 0, dst;
    int dptr = *ptr;
    uint32_t dst_align = (uint32_t)atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    atom_get_dst(ctx, arg, attr, ptr, &saved);
    dst = saved;
    shift = (uint8_t)atom_get_src(ctx, attr, ptr);
    dst = shift >= 32 ? 0 : dst >> shift;
    dst &= atom_arg_mask[dst_align];
    dst >>= atom_arg_shift[dst_align];
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_switch(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t src, val, target;
    (void)arg;
    src = atom_get_src(ctx, attr, ptr);
    while (U16(*ptr) != ATOM_CASE_END) {
        if (ctx->ctx->bad_access)
            return;
        if (U8(*ptr) == ATOM_CASE_MAGIC) {
            (*ptr)++;
            val = atom_get_src(ctx, (uint8_t)((attr & 0x38) | ATOM_ARG_IMM), ptr);
            target = U16(*ptr);
            if (val == src) {
                *ptr = ctx->start + (int)target;
                return;
            }
            (*ptr) += 2;
        } else {
            RERR(ctx->ctx->d, "ATOM: bad case at 0x%04x", *ptr);
            ctx->abort = 1;
            return;
        }
    }
    (*ptr) += 2;
}

static void atom_op_test(atom_exec *ctx, int *ptr, int arg)
{
    uint8_t attr = (uint8_t)U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL);
    src = atom_get_src(ctx, attr, ptr);
    ctx->ctx->cs_equal = ((dst & src) == 0);
}

static void atom_op_debug(atom_exec *ctx, int *ptr, int arg)
{
    (void)ctx; (void)arg;
    (*ptr)++;
}

static void atom_op_processds(atom_exec *ctx, int *ptr, int arg)
{
    uint32_t val = U16(*ptr);
    (void)arg;
    (*ptr) += (int)val + 2;
}

typedef void (*atom_op_fn)(atom_exec *, int *, int);
static const struct {
    atom_op_fn func;
    int arg;
} opcode_table[ATOM_OP_CNT] = {
#define SIX(f) { f, ATOM_ARG_REG }, { f, ATOM_ARG_PS }, { f, ATOM_ARG_WS }, \
               { f, ATOM_ARG_FB }, { f, ATOM_ARG_PLL }, { f, ATOM_ARG_MC }
    { NULL, 0 },
    SIX(atom_op_move),          /* 1-6 */
    SIX(atom_op_and),           /* 7-12 */
    SIX(atom_op_or),            /* 13-18 */
    SIX(atom_op_shift_left),    /* 19-24 */
    SIX(atom_op_shift_right),   /* 25-30 */
    SIX(atom_op_mul),           /* 31-36 */
    SIX(atom_op_div),           /* 37-42 */
    SIX(atom_op_add),           /* 43-48 */
    SIX(atom_op_sub),           /* 49-54 */
    { atom_op_setport, ATOM_PORT_ATI },     /* 55 */
    { atom_op_setport, ATOM_PORT_PCI },
    { atom_op_setport, ATOM_PORT_SYSIO },
    { atom_op_setregblock, 0 },             /* 58 */
    { atom_op_setfbbase, 0 },               /* 59 */
    SIX(atom_op_compare),       /* 60-65 */
    { atom_op_switch, 0 },                  /* 66 */
    { atom_op_jump, ATOM_COND_ALWAYS },     /* 67 */
    { atom_op_jump, ATOM_COND_EQUAL },
    { atom_op_jump, ATOM_COND_BELOW },
    { atom_op_jump, ATOM_COND_ABOVE },
    { atom_op_jump, ATOM_COND_BELOWOREQUAL },
    { atom_op_jump, ATOM_COND_ABOVEOREQUAL },
    { atom_op_jump, ATOM_COND_NOTEQUAL },   /* 73 */
    SIX(atom_op_test),          /* 74-79 */
    { atom_op_delay, ATOM_UNIT_MILLISEC },  /* 80 */
    { atom_op_delay, ATOM_UNIT_MICROSEC },  /* 81 */
    { atom_op_calltable, 0 },               /* 82 */
    { atom_op_unimplemented, 0 },           /* 83 repeat */
    SIX(atom_op_clear),         /* 84-89 */
    { atom_op_nop, 0 },                     /* 90 */
    { atom_op_eot, 0 },                     /* 91 */
    SIX(atom_op_mask),          /* 92-97 */
    { atom_op_postcard, 0 },                /* 98 */
    { atom_op_beep, 0 },                    /* 99 */
    { atom_op_unimplemented, 0 },           /* 100 savereg */
    { atom_op_unimplemented, 0 },           /* 101 restorereg */
    { atom_op_setdatablock, 0 },            /* 102 */
    SIX(atom_op_xor),           /* 103-108 */
    SIX(atom_op_shl),           /* 109-114 */
    SIX(atom_op_shr),           /* 115-120 */
    { atom_op_debug, 0 },                   /* 121 */
    { atom_op_processds, 0 },               /* 122 */
    { atom_op_mul32, ATOM_ARG_PS },         /* 123 */
    { atom_op_mul32, ATOM_ARG_WS },
    { atom_op_div32, ATOM_ARG_PS },
    { atom_op_div32, ATOM_ARG_WS },         /* 126 */
#undef SIX
};

/* amdgpu_atom_execute_table_locked */
static int execute_locked(rdna3_atom *a, unsigned index, uint32_t *params, int params_size)
{
    uint32_t base;
    int ws, ps, ptr, ret = 0, i;
    unsigned op;
    atom_exec ectx;

    if (index >= 0x100)
        return RDNA3_ENOENT;
    base = cu16(a, a->cmd_table + 4 + 2 * index);
    if (!base || a->bad_access)
        return RDNA3_ENOENT;
    if (a->depth >= RD_ATOM_MAX_DEPTH) {
        RERR(a->d, "ATOM: command table nesting exceeded limit (%u)", RD_ATOM_MAX_DEPTH);
        return RDNA3_EINVAL;
    }

    ws = (int)cu8(a, base + ATOM_CT_WS_PTR);
    ps = (int)(cu8(a, base + ATOM_CT_PS_PTR) & ATOM_CT_PS_MASK);
    ptr = (int)base + ATOM_CT_CODE_PTR;
    if (a->ws_top + (uint32_t)ws > RD_ATOM_WS_POOL)
        return RDNA3_ENOMEM;

    a->depth++;
    a->tables_run++;
    ectx.ctx = a;
    ectx.ps_shift = ps / 4;
    ectx.start = (uint16_t)base;
    ectx.ps = params;
    ectx.ps_size = params_size;
    ectx.abort = 0;
    ectx.last_jump = 0;
    ectx.last_jump_us = 0;
    ectx.ws = ws ? &a->ws_pool[a->ws_top] : NULL;
    ectx.ws_size = ws;
    for (i = 0; i < ws; i++)
        ectx.ws[i] = 0;
    a->ws_top += (uint32_t)ws;

    if (a->trace)
        RLOG(a->d, "ATOM: >> execute %u @0x%04x (len %u, WS %d, PS %d)", index, base,
             cu16(a, base + ATOM_CT_SIZE_PTR), ws, ps);

    for (;;) {
        op = cu8(a, (uint32_t)ptr++);
        if (ectx.abort || a->bad_access) {
            RERR(a->d, "ATOM: %s executing table %u @0x%04x (WS %d, PS %d) at 0x%04x",
                 a->bad_access ? "out-of-image access" : "stuck", index, base, ws, ps, ptr - 1);
            ret = RDNA3_EINVAL;
            break;
        }
        if (++a->ops > RD_ATOM_OP_BUDGET) {
            RERR(a->d, "ATOM: opcode budget exhausted in table %u", index);
            ret = RDNA3_EINVAL;
            break;
        }
        if (a->trace)
            RLOG(a->d, "ATOM:   op %u @0x%04x", op, ptr - 1);
        if (op < ATOM_OP_CNT && op > 0) {
            opcode_table[op].func(&ectx, &ptr, opcode_table[op].arg);
        } else {
            /* amdgpu stops quietly on 0 or an unknown opcode */
            if (op)
                a->unknown_ops++;
            break;
        }
        if (op == ATOM_OP_EOT)
            break;
    }
    if (!ret && (ectx.abort || a->bad_access))
        ret = RDNA3_EINVAL;

    a->ws_top -= (uint32_t)ws;
    a->depth--;
    return ret;
}

int atom_execute(rdna3_atom *a, unsigned index, uint32_t *params, unsigned params_dwords)
{
    /* amdgpu_atom_execute_table: reset data block, reg block, fb window,
     * io mode and divmul */
    a->data_block = 0;
    a->reg_block = 0;
    a->fb_base = 0;
    a->io_mode = ATOM_IO_MM;
    a->divmul[0] = a->divmul[1] = 0;
    a->ops = 0;
    a->delay_us = 0;
    a->bad_access = 0;
    a->ws_top = 0;
    return execute_locked(a, index, params, (int)params_dwords);
}

int atom_cmd_revision(const rdna3_atom *a, unsigned index, uint8_t *frev, uint8_t *crev)
{
    uint32_t off = (uint32_t)a->cmd_table + 4 + 2 * index, idx;
    if (index >= 0x100 || off + 2 > a->bios_size)
        return 0;
    idx = (uint32_t)a->bios[off] | ((uint32_t)a->bios[off + 1] << 8);
    if (!idx || idx + 4 > a->bios_size)
        return 0;
    if (frev)
        *frev = a->bios[idx + 2];
    if (crev)
        *crev = a->bios[idx + 3];
    return 1;
}

static const int atom_iio_len[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };

static void atom_index_iio(rdna3_atom *a, uint32_t base)
{
    while (base + 1 < a->bios_size && a->bios[base] == ATOM_IIO_START) {
        uint8_t index = a->bios[base + 1];
        uint32_t start = base + 2;
        base += 2;
        while (base < a->bios_size && a->bios[base] != ATOM_IIO_END) {
            uint8_t op = a->bios[base];
            /* unknown opcode: its length is unknown, cannot resync */
            if (op >= sizeof(atom_iio_len) / sizeof(atom_iio_len[0]))
                return;
            base += (uint32_t)atom_iio_len[op];
        }
        if (base >= a->bios_size)
            return;
        /* only index well-formed methods, others stay 0 */
        a->iio[index] = (uint16_t)start;
        base += 3;
    }
}

int atom_init(rdna3_atom *a, rdna3_dev *d, const rdna3_vbios *v, uint32_t *scratch,
              uint32_t scratch_bytes)
{
    uint32_t i;
    for (i = 0; i < sizeof(*a); i++)
        ((uint8_t *)a)[i] = 0;
    if (!v || !v->data || !v->rom_header || v->len < 0x4a)
        return RDNA3_EINVAL;
    a->d = d;
    a->bios = v->data;
    a->bios_size = v->len;
    a->scratch = scratch;
    a->scratch_bytes = scratch ? scratch_bytes : 0;
    a->cmd_table = (uint16_t)cu16(a, v->rom_header + ATOM_ROM_CMD_PTR);
    a->data_table = (uint16_t)cu16(a, v->rom_header + ATOM_ROM_DATA_PTR);
    if (a->bad_access || !a->cmd_table || (uint32_t)a->cmd_table + 4 > a->bios_size) {
        RERR(d, "ATOM: no command table directory");
        return RDNA3_ENODEV;
    }
    {
        uint32_t iio = cu16(a, a->data_table + ATOM_DATA_IIO_PTR);
        a->bad_access = 0;
        if (iio)
            atom_index_iio(a, iio + 4);
    }
    return RDNA3_OK;
}

/* amdgpu_atomfirmware_asic_init(adev, fb_reset = true): asic_init_ps_allocation_v2_1
 * with the firmware_info boot clocks, engineflag b3NORMAL_ENGINE_INIT (0),
 * memflag 0 */
int atom_asic_init(rdna3_atom *a)
{
    uint32_t ps[16], hwi;
    uint8_t frev = 0, crev = 0;
    unsigned i;
    for (i = 0; i < 16; i++)
        ps[i] = 0;
    a->bad_access = 0;
    hwi = cu16(a, a->data_table + ATOM_DATA_FWI_PTR);
    ps[0] = cu32(a, hwi + ATOM_FWI_DEFSCLK_PTR) & 0xFFFFFFu;
    ps[1] = cu32(a, hwi + ATOM_FWI_DEFMCLK_PTR) & 0xFFFFFFu;
    if (a->bad_access || !hwi || !ps[0] || !ps[1])
        return RDNA3_EINVAL;
    if (!atom_cmd_revision(a, RD_ATOM_CMD_ASIC_INIT, &frev, &crev))
        return RDNA3_ENOENT;
    if (frev != 2 || crev < 1)
        return RDNA3_EINVAL;
    return atom_execute(a, RD_ATOM_CMD_ASIC_INIT, ps, 16);
}

/* amdgpu_atomfirmware_scratch_regs_init + amdgpu_atombios_scratch_need_asic_init:
 * BIOS_SCRATCH_7.ASIC_INIT_COMPLETE clear -> the VBIOS never posted this GPU
 * (a passthrough VM whose firmware did not run the option ROM's init). */
int atom_need_post(rdna3_atom *a)
{
    uint32_t hwi, scratch;
    a->bad_access = 0;
    hwi = cu16(a, a->data_table + ATOM_DATA_FWI_PTR);
    scratch = cu32(a, hwi + ATOM_FWI_SCRATCH_PTR);
    if (a->bad_access || !hwi || !scratch)
        return RDNA3_EINVAL;
    return (rdna3_rreg(a->d, scratch + 7) & ATOM_S7_ASIC_INIT_COMPLETE_MASK) ? 0 : 1;
}
