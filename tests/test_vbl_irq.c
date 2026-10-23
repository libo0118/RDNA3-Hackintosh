/*
 * test_vbl_irq.c - common/rdna3_vbl_irq.h on the host (DRIVER-PLAN N42:
 * IOGraphics' VBL from the DCN vblank interrupt, the OTG poll as fallback).
 *
 * The model: a head at 60 Hz; the vblank interrupt reaches the framebuffer
 * a little after each frame start while the path works; the framebuffer's
 * poll thread call looks every frame when it delivers itself, otherwise
 * only at rd3_virq_watch_at. Checked: while the interrupt works every frame
 * is delivered once and the poll delivers nothing; when it falls silent the
 * poll delivers from at most RD3_VIRQ_FALLBACK_FRAMES frames later on; when
 * it comes back the poll stops; a storm is cut at the limit, once per window.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_vbl_irq.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { PERIOD = 16667, IRQ_LAT = 40, FRAMES = 600, SILENT_FROM = 200, SILENT_TO = 300 };

struct fb {
    int live;
    uint64_t since;        /* last interrupt delivery, or when the path went live */
    uint64_t next_poll;
    uint32_t irq_deliv, poll_deliv, fallbacks;
    int64_t last_frame;    /* the last frame delivered */
    uint32_t dup, gap_max;
};

static void deliver(struct fb *f, int64_t frame, int from_irq)
{
    if (frame == f->last_frame) {
        f->dup++;
        return;
    }
    if (f->last_frame >= 0 && (uint32_t)(frame - f->last_frame) > f->gap_max)
        f->gap_max = (uint32_t)(frame - f->last_frame);
    f->last_frame = frame;
    if (from_irq)
        f->irq_deliv++;
    else
        f->poll_deliv++;
}

static void run(int path_live, struct fb *f)
{
    memset(f, 0, sizeof(*f));
    f->last_frame = -1;
    f->live = path_live;
    f->since = path_live ? 1 : 0;
    f->next_poll = PERIOD / 2;
    for (uint64_t t = 1; t < (uint64_t)FRAMES * PERIOD; t++) {
        const int64_t frame = (int64_t)(t / PERIOD);
        /* the interrupt: IRQ_LAT after each frame start, unless silent */
        if (f->live && t % PERIOD == IRQ_LAT && !(frame >= SILENT_FROM && frame < SILENT_TO)) {
            deliver(f, frame, 1);
            f->since = t;
        }
        if (t == f->next_poll) {
            if (rd3_virq_poll_due(f->live, f->since, t, PERIOD, RD3_VIRQ_FALLBACK_FRAMES)) {
                if (f->live)
                    f->fallbacks++;
                deliver(f, frame, 0);
                f->next_poll = t + PERIOD;
            } else {
                f->next_poll = rd3_virq_watch_at(f->since, PERIOD, RD3_VIRQ_FALLBACK_FRAMES);
                if (f->next_poll <= t)
                    f->next_poll = t + 1;
            }
        }
    }
}

int main(void)
{
    struct fb f;

    /* no interrupt path: the poll delivers every frame */
    run(0, &f);
    CHECK(f.irq_deliv == 0 && f.poll_deliv >= FRAMES - 2 && f.dup == 0 && f.gap_max == 1);

    /* the interrupt path, silent for 100 frames in the middle */
    run(1, &f);
    CHECK(f.dup == 0);
    CHECK(f.irq_deliv == FRAMES - (SILENT_TO - SILENT_FROM));
    /* the silence: the poll from RD3_VIRQ_FALLBACK_FRAMES frames on */
    CHECK(f.poll_deliv >= SILENT_TO - SILENT_FROM - RD3_VIRQ_FALLBACK_FRAMES - 1 &&
          f.poll_deliv <= SILENT_TO - SILENT_FROM);
    CHECK(f.fallbacks == f.poll_deliv);
    CHECK(f.gap_max <= RD3_VIRQ_FALLBACK_FRAMES + 1);
    printf("test_vbl_irq: %u by interrupt, %u by the poll (fallback), longest gap %u frames\n", f.irq_deliv,
           f.poll_deliv, f.gap_max);

    /* the decision itself */
    CHECK(rd3_virq_poll_due(0, 100, 100, PERIOD, 3) == 1);
    CHECK(rd3_virq_poll_due(1, 0, 100, PERIOD, 3) == 1);              /* never live */
    CHECK(rd3_virq_poll_due(1, 1000, 1000 + 3 * PERIOD - 1, PERIOD, 3) == 0);
    CHECK(rd3_virq_poll_due(1, 1000, 1000 + 3 * PERIOD, PERIOD, 3) == 1);
    CHECK(rd3_virq_poll_due(1, 5000, 4000, PERIOD, 3) == 0);          /* an interrupt after the look began */
    CHECK(rd3_virq_watch_at(1000, PERIOD, 3) == 1000 + 3 * PERIOD + PERIOD / 4);

    /* storm guard: the (MAX+1)th interrupt in a window says storm, once */
    {
        struct rd3_virq_storm s;
        uint32_t i, hits = 0;
        memset(&s, 0, sizeof(s));
        for (i = 0; i < 1000; i++)
            hits += (uint32_t)rd3_virq_storm_note(&s, 1000000ull + i * 1000ull);   /* 1 MHz for 1 ms */
        CHECK(hits == 1 && s.storms == 1);
        /* a normal 240 Hz source never trips it */
        memset(&s, 0, sizeof(s));
        for (i = 0; i < 24000; i++)
            hits += (uint32_t)rd3_virq_storm_note(&s, 5000000ull + (uint64_t)i * 4166667ull);
        CHECK(hits == 1 && s.storms == 0);
        /* the next window starts counting again */
        memset(&s, 0, sizeof(s));
        for (i = 0; i <= RD3_VIRQ_STORM_MAX; i++)
            rd3_virq_storm_note(&s, 10ull + i);
        CHECK(s.storms == 1);
        CHECK(rd3_virq_storm_note(&s, 10ull + RD3_VIRQ_STORM_WINDOW_NS) == 0 && s.in_win == 1);
    }

    /* latency */
    {
        struct rd3_lat l;
        memset(&l, 0, sizeof(l));
        CHECK(rd3_lat_avg(&l) == 0);
        rd3_lat_add(&l, 10);
        rd3_lat_add(&l, 30);
        CHECK(l.n == 2 && l.max_us == 30 && rd3_lat_avg(&l) == 20);
    }
    CHECK(rd3_virq_pflip_seen(5, 5) == 0 && rd3_virq_pflip_seen(5, 6) == 1);

    if (fails) {
        printf("test_vbl_irq: %d FAILED\n", fails);
        return 1;
    }
    printf("test_vbl_irq: ok\n");
    return 0;
}
