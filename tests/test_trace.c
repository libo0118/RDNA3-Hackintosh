/*
 * test_trace.c - common/rdna3_trace.h on the host (card run 2026-10-17b:
 * the per-frame trace). The record and header layouts tools/rdna3_trace.py
 * decodes, the ring (overwrite, count, lost, oldest-first order, finding an
 * event by number), the band bytes, the salted object names, the bounding
 * box of copy forward's rects, the stream reader's window and the
 * per-second table. tests/test_triage_trace.sh feeds synthetic traces of
 * each hypothesis through tools/rdna3_trace.py.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "rdna3_trace.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void layout(void)
{
    CHECK(sizeof(struct rd3_trace_ev) == 80);
    CHECK(sizeof(struct rd3_trace_hdr) == 96);
    CHECK(sizeof(struct rd3_trace_sec) == 32);
    /* tools/rdna3_trace.py's EV_FMT "<QIBBBBIIIIIIQQQ8s8s" */
    CHECK(offsetof(struct rd3_trace_ev, seq) == 8);
    CHECK(offsetof(struct rd3_trace_ev, type) == 12);
    CHECK(offsetof(struct rd3_trace_ev, flags) == 15);
    CHECK(offsetof(struct rd3_trace_ev, obj) == 16);
    CHECK(offsetof(struct rd3_trace_ev, v0) == 20);
    CHECK(offsetof(struct rd3_trace_ev, v4) == 36);
    CHECK(offsetof(struct rd3_trace_ev, pa) == 40);
    CHECK(offsetof(struct rd3_trace_ev, pc) == 56);
    CHECK(offsetof(struct rd3_trace_ev, ba) == 64);
    CHECK(offsetof(struct rd3_trace_ev, bb) == 72);
    /* "<IIIIIIQQQQIIIIIIQII" */
    CHECK(offsetof(struct rd3_trace_hdr, count) == 16);
    CHECK(offsetof(struct rd3_trace_hdr, written) == 24);
    CHECK(offsetof(struct rd3_trace_hdr, now) == 40);
    CHECK(offsetof(struct rd3_trace_hdr, nomem) == 48);
    CHECK(offsetof(struct rd3_trace_hdr, boot_id) == 56);
    CHECK(offsetof(struct rd3_trace_hdr, tb_numer) == 64);
    CHECK(offsetof(struct rd3_trace_hdr, ab) == 76);
    CHECK(offsetof(struct rd3_trace_hdr, wall_us) == 80);
    CHECK(offsetof(struct rd3_trace_hdr, flags) == 88);
    CHECK(offsetof(struct rd3_trace_hdr, hdr_size) == 92);
    CHECK(RD3_TRACE_VERSION == 2);
    /* "<IBBBBHHHHHHHHHHI" */
    CHECK(offsetof(struct rd3_trace_sec, swaps) == 8);
    CHECK(offsetof(struct rd3_trace_sec, freedshown) == 26);
    CHECK(offsetof(struct rd3_trace_sec, events) == 28);
    CHECK(RD3_TRACE_MAGIC == 0x52543352u);
    CHECK(RD3_TR_TYPES == 26 && RD3_TR_PRESENT == 25 && RD3_TR_SCAN == 15 && RD3_TR_CS == 16 && RD3_TR_PANEL == 22);
    CHECK(RD3_TR_FLIPBUF == 23 && RD3_TR_HOOK == 24);
}

static struct rd3_trace_ev store[8];

static void ring(void)
{
    struct rd3_trace_ring r;
    uint32_t i;
    memset(&r, 0, sizeof r);
    CHECK(!rd3_trace_next(&r) && !rd3_trace_next(0));   /* no storage: nothing */
    CHECK(rd3_trace_count(&r) == 0 && rd3_trace_lost(&r) == 0);
    r.ev = store;
    r.cap = 8;
    for (i = 1; i <= 5; i++) {
        struct rd3_trace_ev *e = rd3_trace_next(&r);
        CHECK(e && e->seq == i && e->type == 0 && e->pa == 0);
        e->type = RD3_TR_SWAP;
        e->v0 = i;
    }
    CHECK(rd3_trace_count(&r) == 5 && rd3_trace_lost(&r) == 0);
    CHECK(rd3_trace_index(&r, 0) == 0 && rd3_trace_index(&r, 4) == 4);
    CHECK(rd3_trace_find(&r, 3) == 2 && rd3_trace_find(&r, 6) == -1 && rd3_trace_find(&r, 0) == -1);
    for (i = 6; i <= 19; i++) {
        struct rd3_trace_ev *e = rd3_trace_next(&r);
        e->v0 = i;
    }
    /* 19 written, 8 kept (12..19), 11 lost; the oldest first */
    CHECK(rd3_trace_count(&r) == 8 && rd3_trace_lost(&r) == 11);
    for (i = 0; i < 8; i++)
        CHECK(store[rd3_trace_index(&r, i)].seq == 12 + i && store[rd3_trace_index(&r, i)].v0 == 12 + i);
    CHECK(rd3_trace_find(&r, 11) == -1 && rd3_trace_find(&r, 12) >= 0 && rd3_trace_find(&r, 19) >= 0);
    CHECK(store[rd3_trace_find(&r, 15)].seq == 15);
    /* copies: the oldest still kept first, at most max */
    {
        static struct rd3_trace_ev out[8];
        CHECK(rd3_trace_copy(&r, 1, out, 8) == 8 && out[0].seq == 12 && out[7].seq == 19);
        CHECK(rd3_trace_copy(&r, 17, out, 8) == 3 && out[0].seq == 17);
        CHECK(rd3_trace_copy(&r, 12, out, 2) == 2 && out[1].seq == 13);
        CHECK(rd3_trace_copy(&r, 20, out, 8) == 0);
    }
    /* a slot is zeroed before it is handed out */
    {
        struct rd3_trace_ev *e = rd3_trace_next(&r);
        CHECK(e->v0 == 0 && e->seq == 20);
    }
}

static void helpers(void)
{
    uint32_t z[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }, a[8] = { 0, 0, 0, 0xff000000u, 0, 0, 0, 0 };
    uint32_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, c[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };
    uint8_t w[8];
    struct rd3_trace_box box;
    CHECK(rd3_trace_band(z, 8) == 0);
    CHECK(rd3_trace_band(a, 8) == 0);   /* alpha only: an empty (black, transparent) band */
    CHECK(rd3_trace_band(b, 8) != 0 && rd3_trace_band(b, 8) != rd3_trace_band(c, 8));
    CHECK(rd3_trace_band(b, 8) == rd3_trace_band(b, 8));
    {
        /* SipHash-2-4's reference vectors (key 00..0f, message 00..len-1) */
        uint8_t key[16], key2[16], msg[16];
        unsigned i;
        for (i = 0; i < 16; i++) {
            key[i] = (uint8_t)i;
            key2[i] = (uint8_t)(i + 1);
            msg[i] = (uint8_t)i;
        }
        CHECK(rd3_siphash24(key, msg, 0) == 0x726fdb47dd0e0e31ull);
        CHECK(rd3_siphash24(key, msg, 8) == 0x93f5f5799a932462ull);
        CHECK(rd3_siphash24(key, msg, 15) == 0xa129ca6149be45e5ull);
        CHECK(rd3_trace_obj(0, key) == 0);
        CHECK(rd3_trace_obj(0xffffff8012345678ull, key) != 0);
        CHECK(rd3_trace_obj(0xffffff8012345678ull, key) != rd3_trace_obj(0xffffff8012345678ull, key2));
        CHECK(rd3_trace_obj(0xffffff8012345678ull, key) != rd3_trace_obj(0xffffff8012345679ull, key));
        CHECK(rd3_trace_obj(0xffffff8012345678ull, key) != (uint32_t)0x12345678u);
    }
    memset(w, 0, sizeof w);
    rd3_trace_put16(w, 1, 0x1234);
    rd3_trace_put16(w, 3, 0xffff);
    CHECK(w[2] == 0x34 && w[3] == 0x12 && w[6] == 0xff && w[7] == 0xff && w[0] == 0);
    CHECK(rd3_trace_le64(w) == 0xffff000012340000ull);
    memset(&box, 0, sizeof box);
    rd3_trace_box_add(&box, 10, 20, 0, 5);   /* no size: left out */
    CHECK(box.n == 0);
    rd3_trace_box_add(&box, 100, 200, 50, 10);
    rd3_trace_box_add(&box, 20, 300, 10, 10);
    CHECK(box.n == 2 && box.x0 == 20 && box.y0 == 200 && box.x1 == 150 && box.y1 == 310);
    CHECK(rd3_trace_u16(-5) == 0 && rd3_trace_u16(70000) == 0xffff && rd3_trace_u16(1080) == 1080);
}

static void stream_and_seconds(void)
{
    struct rd3_trace_ring r;
    static struct rd3_trace_sec tab[RD3_TRACE_SECS * RD3_TRACE_SEC_ROWS];
    struct rd3_trace_sec *row;
    uint64_t lost = 0;
    unsigned i;
    memset(&r, 0, sizeof r);
    r.ev = store;
    r.cap = 8;
    for (i = 0; i < 5; i++)
        rd3_trace_next(&r);
    /* a reader at 0 gets 1..5; at 3 gets 4..5 */
    CHECK(rd3_trace_stream_from(&r, 0, &lost) == 1 && lost == 0);
    CHECK(rd3_trace_stream_from(&r, 3, &lost) == 4 && lost == 0);
    CHECK(rd3_trace_stream_from(&r, 5, &lost) == 6 && lost == 0);   /* nothing new */
    for (i = 0; i < 10; i++)
        rd3_trace_next(&r);
    /* 15 written, 8..15 kept: a reader at 3 lost 4..7 */
    CHECK(rd3_trace_stream_from(&r, 3, &lost) == 8 && lost == 4);
    CHECK(rd3_trace_stream_from(&r, 7, &lost) == 8 && lost == 0);
    /* the per-second table: a row per (second, head), reused a table later */
    memset(tab, 0, sizeof tab);
    row = rd3_trace_sec_get(tab, 10, 1);
    CHECK(row && row->sec == 11 && row->head == 1 && row->swaps == 0);
    rd3_trace_inc16(&row->swaps);
    CHECK(rd3_trace_sec_get(tab, 10, 1)->swaps == 1);
    CHECK(rd3_trace_sec_get(tab, 10, 0) != row);
    CHECK(rd3_trace_sec_get(tab, 10, 9)->head == 4);   /* no head: the last row */
    row = rd3_trace_sec_get(tab, 10 + RD3_TRACE_SECS, 1);   /* the same row, a newer second: emptied */
    CHECK(row->sec == 11 + RD3_TRACE_SECS && row->swaps == 0);
    row->swaps = 0xffff;
    rd3_trace_inc16(&row->swaps);
    CHECK(row->swaps == 0xffff);   /* saturates */
    CHECK(!rd3_trace_sec_get(0, 1, 1));
}

/* ECC review round 1: reads copy a fixed window in chunks with the lock
 * dropped between; what the writers overwrite meanwhile is counted lost,
 * never copied twice or out of order, and a full read starts at the oldest
 * event (not at number 1: a loop over every event since the boot) */
static void chunks(void)
{
    static struct rd3_trace_ev big[64], out[64];
    struct rd3_trace_ring r;
    uint64_t next, lost, to, expect;
    uint32_t n = 0, got, i;
    memset(&r, 0, sizeof r);
    r.ev = big;
    r.cap = 64;
    for (i = 0; i < 1000; i++)
        rd3_trace_next(&r);
    CHECK(rd3_trace_oldest(&r) == 937);
    /* a full read: 937..1000 */
    CHECK(rd3_trace_copy(&r, 1, out, 64) == 64 && out[0].seq == 937 && out[63].seq == 1000);
    CHECK(rd3_trace_copy(&r, 990, out, 64) == 11 && out[0].seq == 990);
    /* a chunked read of [937, 1000] with 40 events written after the first
     * chunk of 16: 937..976 overwritten meanwhile -> 16 copied, then 977.. */
    to = r.written;
    next = rd3_trace_oldest(&r);
    lost = 0;
    got = rd3_trace_copy_chunk(&r, &next, to, out, 16, &lost);
    CHECK(got == 16 && next == 953 && lost == 0);
    n = got;
    for (i = 0; i < 40; i++)
        rd3_trace_next(&r);   /* 1040 written: 977..1040 kept */
    while (next <= to && n < 64) {
        got = rd3_trace_copy_chunk(&r, &next, to, out + n, 16, &lost);
        n += got;
        if (!got)
            break;
    }
    CHECK(lost == 977 - 953);       /* 953..976 */
    CHECK(n == 16 + (1000 - 977 + 1));
    expect = 937;
    for (i = 0; i < n; i++) {
        if (i == 16)
            expect = 977;
        CHECK(out[i].seq == (uint32_t)expect);
        expect++;
    }
    CHECK(next == 1001);            /* the window ends where the read began, 1001..1040 are the next read's */
    /* a window wholly overwritten: everything lost, nothing copied */
    next = 900;
    lost = 0;
    CHECK(rd3_trace_copy_chunk(&r, &next, 950, out, 64, &lost) == 0 && lost == 51 && next == 951);
    next = 0;
    CHECK(rd3_trace_copy_chunk(&r, &next, 950, out, 64, &lost) == 0);   /* 0: no window */
}

/* the cost bound of sampling the surface's other buffers */
static void scan_due(void)
{
    CHECK(rd3_trace_scan_due(0, 1, 5000, 0));          /* an empty back: always */
    CHECK(!rd3_trace_scan_due(0, 0, 0, 1));
    CHECK(rd3_trace_scan_due(2, 0, 5001, 0));
    CHECK(rd3_trace_scan_due(1, 0, 10, 0));            /* a head's first swaps */
    CHECK(rd3_trace_scan_due(1, 0, 5001, 2));          /* the A/B */
    CHECK(!rd3_trace_scan_due(1, 0, 5001, 0));
    CHECK(rd3_trace_scan_due(1, 0, 5000, 0));          /* every 8th */
}

int main(void)
{
    layout();
    ring();
    helpers();
    stream_and_seconds();
    chunks();
    scan_due();
    if (fails) {
        printf("test_trace: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_trace: all checks passed\n");
    return 0;
}
