/*
 * rdna3_swap_front.h - which buffer of a WindowServer legacy surface swap
 * goes to the panel, and where copy forward takes the last frame from.
 * Plain C, no kernel headers: the accelerator uses it and
 * tests/test_swap_front.c checks it on the host.
 *
 * IOAccelLegacySurface's model (probe runs 7-10, card runs 54-58): buffer 0
 * of a display surface is the front, the display pipe's framebuffer
 * resource (RD3DisplayPipe::initFramebufferResource; after the boot its own
 * VRAM, 0x87040000 / 0x88060000 in card run 58b). Buffers 1 and 2 (shape
 * mode 0x6) are the backs WindowServer draws into. submitSwapCopy(event,
 * front, back) asks for the back to be copied into the front, which the
 * panel shows; copy_forward then copies the regions the last frame changed
 * and this one does not redraw from the front (holding the last frame) into
 * the next back. The accelerator flips the panel to the back itself
 * instead (zero-copy, rd3AccelFlipTo), so the front never holds a frame:
 *
 * - the last frame is in the buffer the head showed last. With one back
 *   (the login window) that is the back copy forward writes to, and nothing
 *   needs copying (card run 56). With two backs taking turns (the user's
 *   WindowServer after login, 2026-10-15b..16) it is the other back: card
 *   run 58b skipped those copies ("the front buffer was never shown"), so
 *   each back kept what changed two frames before, and the panel showed
 *   the two in turns - A B A B on both screens (flicker: head 1, 120 of 204
 *   hashed swaps, 6 of 8 bands). rd3_cf_source copies from the shown buffer.
 * - the source of a swap is the back. The front was shown only when the
 *   back read empty (rd3AccelResHasContent's samples) and the front did
 *   not: a frame the panel should never get in this scheme. A back that
 *   keeps reading empty is a black or clear frame (a fade, the lock
 *   screen): the third swap of it in a row goes to the panel.
 * - copy forward copies only between the calling surface's own buffers
 *   (ECC review of 60526e4): the buffer a head shows may be another
 *   surface's, another process's frame - never a source for this one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SWAP_FRONT_H
#define RDNA3_SWAP_FRONT_H

/* the swap's buffer that goes to the panel */
enum rd3_swap_pick { RD3_SWAP_SKIP = 0, RD3_SWAP_A1 = 1, RD3_SWAP_A2 = 2 };

/* the swaps of a head skipped in a row after which an empty back goes to
 * the panel */
#define RD3_SWAP_EMPTY_RUN 3u

/* a1IsFront / a2IsFront: that argument is a head's framebuffer resource
 * (the front); a1Content / a2Content: rd3AccelResContent found pixels;
 * emptyRun: the head's swaps skipped in a row before this one, whichever
 * back they named (two empty backs taking turns must reach the run too,
 * review of e2bf78e). With the front known the other argument is the back
 * and the only candidate: an empty back is skipped (nothing drawn yet)
 * until RD3_SWAP_EMPTY_RUN swaps came in a row (a black or clear frame),
 * never replaced by the front. Without (or with two fronts) the older rule:
 * the argument that has content, a2 first (probe runs: a2 held the
 * composited frame). */
static inline enum rd3_swap_pick rd3_swap_source(int a1IsFront, int a2IsFront, int a1Content, int a2Content,
                                                 unsigned emptyRun)
{
    if (a1IsFront && !a2IsFront)
        return a2Content || emptyRun + 1u >= RD3_SWAP_EMPTY_RUN ? RD3_SWAP_A2 : RD3_SWAP_SKIP;
    if (a2IsFront && !a1IsFront)
        return a1Content || emptyRun + 1u >= RD3_SWAP_EMPTY_RUN ? RD3_SWAP_A1 : RD3_SWAP_SKIP;
    if (a2Content)
        return RD3_SWAP_A2;
    if (a1Content)
        return RD3_SWAP_A1;
    return RD3_SWAP_SKIP;
}

/* where copy forward copies from */
enum rd3_cf_pick {
    RD3_CF_SKIP_SAME = 0,       /* source and target are one buffer */
    RD3_CF_COPY_SRC,            /* the front WindowServer named holds the last frame (it was shown) */
    RD3_CF_COPY_SHOWN,          /* the surface's own buffer the head showed last holds it: copy from there */
    RD3_CF_SKIP_BACK_IS_LAST,   /* the target is the buffer the head showed last: nothing to bring forward */
    RD3_CF_SKIP_NO_HEAD,        /* no head known and the front never shown: nothing to copy from */
    RD3_CF_SKIP_NOTHING_SHOWN,  /* the head is known but shows nothing recorded (before its first swap, after a
                                   free or a failed flip): the front is not the last frame either */
    RD3_CF_SKIP_FOREIGN         /* the head shows a buffer that is not this surface's: never a source */
};

struct rd3_cf_in {
    int srcIsDst;     /* the front and the back are one buffer */
    int headKnown;    /* the surface's head is known */
    int headShown;    /* that head has a record of the buffer it shows */
    int shownIsDst;   /* that buffer is the target */
    int shownIsSrc;   /* that buffer is the front WindowServer named */
    int shownOwned;   /* that buffer is one of the calling surface's own */
    int srcShown;     /* the front is recorded as shown on some head (used when no head is known) */
    int srcNewer;     /* the front was written after the frame the head presented last (rdna3_write_gen.h
                         rd3_wg_src_newer: a copy forward burst's chain, card run 2026-10-17d): the source */
};

static inline enum rd3_cf_pick rd3_cf_source(const struct rd3_cf_in *in)
{
    if (!in || in->srcIsDst)
        return RD3_CF_SKIP_SAME;
    if (in->headKnown) {
        if (in->srcNewer)
            return RD3_CF_COPY_SRC;
        if (!in->headShown)
            return RD3_CF_SKIP_NOTHING_SHOWN;
        if (in->shownIsDst)
            return RD3_CF_SKIP_BACK_IS_LAST;
        if (in->shownIsSrc)
            return RD3_CF_COPY_SRC;
        return in->shownOwned ? RD3_CF_COPY_SHOWN : RD3_CF_SKIP_FOREIGN;
    }
    return in->srcShown ? RD3_CF_COPY_SRC : RD3_CF_SKIP_NO_HEAD;
}

/* Rects a copy forward left stale in its target (a copy that failed, or a
 * call that copied nothing): copy forward moves only the regions the last
 * frame changed, so a missed region stays stale in that buffer until it is
 * redrawn and shows in turns with the other back (the A B A B again).
 * Each buffer keeps its own missed rects (review of e2bf78e: one mark per
 * head lost the first back's when the second missed too, and a whole copy
 * from a source that had missed rects itself spread them). The next copy
 * forward into that buffer copies them too, from the shown buffer - only
 * those that do not touch a rect the source missed itself (the source is
 * stale there; such rects stay pending). A rect the last frame or this one
 * changed was drawn by WindowServer into the source, so the source holds
 * it. Too many rects: the whole picture is pending, copied in one piece
 * from a source with nothing pending, and only with the copy engine on
 * (the CPU takes 256 KiB at most); until then only the rects. A buffer's
 * rects go when it is freed. The miss is not reported to CoreDisplay: on
 * kIOReturnUnsupported it switches copy forward off for good ("copy
 * forward failed ... disabling", card run 54) and keeps drawing only the
 * damage, which would leave every later frame stale; it is counted and
 * logged instead (debug.rdna3accel_cf_miss_h<N>, cf_healed_h<N>). */
struct rd3_rect { int x, y, w, h; };
#define RD3_CF_PEND_RECTS 8u
#define RD3_CF_PEND_BUFS  4u
/* calls into a buffer its source blocked (the source missed the same
 * region, or the whole picture) after which it copies anyway: two backs
 * each waiting for the other to be clean never heal (review of 7b2c4c0);
 * copied anyway, both hold the same picture there - stale, but no A B A B */
#define RD3_CF_PEND_BLOCKED_MAX 3u
struct rd3_cf_pend { const void *buf; unsigned n; int whole; unsigned blocked; struct rd3_rect r[RD3_CF_PEND_RECTS]; };
struct rd3_cf_heal { struct rd3_cf_pend b[RD3_CF_PEND_BUFS]; };

/* what rd3_cf_pend_add did beyond adding: dropped another buffer's list
 * (the table was full), turned the list into the whole picture (too many
 * rects) */
enum { RD3_PEND_EVICTED = 1, RD3_PEND_OVERFLOW = 2 };
/* what rd3_cf_pend_take did: the source blocked some rects (they stay),
 * the blocked limit was reached and they were copied anyway */
struct rd3_cf_take { unsigned blocked, forced; };

static inline int rd3_rect_meet(const struct rd3_rect *a, const struct rd3_rect *b)
{
    return a->w > 0 && a->h > 0 && b->w > 0 && b->h > 0 && a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/* the part of 'r' inside a w x h picture; 0: none (the picture got smaller) */
static inline int rd3_rect_clip(struct rd3_rect *r, int w, int h)
{
    int x1 = r->x + r->w, y1 = r->y + r->h;
    if (r->x < 0) r->x = 0;
    if (r->y < 0) r->y = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    r->w = x1 - r->x;
    r->h = y1 - r->y;
    return r->w > 0 && r->h > 0;
}

static inline struct rd3_cf_pend *rd3_cf_pend_get(struct rd3_cf_heal *s, const void *buf)
{
    unsigned i;
    for (i = 0; s && buf && i < RD3_CF_PEND_BUFS; i++)
        if (s->b[i].buf == buf)
            return &s->b[i];
    return 0;
}

/* anything pending for 'buf' */
static inline int rd3_cf_pending(struct rd3_cf_heal *s, const void *buf)
{
    const struct rd3_cf_pend *p = rd3_cf_pend_get(s, buf);
    return p && (p->n || p->whole);
}

/* the entry leaves the table; the later ones move up, so the table stays
 * in the order the buffers first missed (the first is the oldest) */
static inline void rd3_cf_pend_drop(struct rd3_cf_heal *s, struct rd3_cf_pend *p)
{
    unsigned i = (unsigned)(p - s->b);
    for (; i + 1 < RD3_CF_PEND_BUFS; i++)
        s->b[i] = s->b[i + 1];
    s->b[RD3_CF_PEND_BUFS - 1].buf = 0;
    s->b[RD3_CF_PEND_BUFS - 1].n = 0;
    s->b[RD3_CF_PEND_BUFS - 1].whole = 0;
    s->b[RD3_CF_PEND_BUFS - 1].blocked = 0;
}

/* 'buf' missed 'n' rects ('r' NULL or n of ~0u: the whole picture; rects
 * of no size are left out, and nothing at all claims no entry). A full
 * table drops the oldest list that is neither buf's nor 'keep''s (the
 * source of the copy at hand: its list must not look clean). Returns
 * RD3_PEND_* bits. */
static inline unsigned rd3_cf_pend_add(struct rd3_cf_heal *s, const void *buf, const struct rd3_rect *r, unsigned n,
                                       const void *keep)
{
    struct rd3_cf_pend *p;
    unsigned i, valid = 0, flags = 0;
    const int whole = !r || n == ~0u;
    if (!s || !buf)
        return 0;
    for (i = 0; !whole && i < n; i++)
        valid += r[i].w > 0 && r[i].h > 0;
    if (!whole && !valid)
        return 0;
    p = rd3_cf_pend_get(s, buf);
    if (!p) {
        for (i = 0; i < RD3_CF_PEND_BUFS && !p; i++)
            if (!s->b[i].buf)
                p = &s->b[i];
        if (!p) {
            for (i = 0; i < RD3_CF_PEND_BUFS && s->b[i].buf == keep; i++)
                ;
            if (i == RD3_CF_PEND_BUFS)
                return 0;   /* cannot happen with one buffer kept and four entries */
            rd3_cf_pend_drop(s, &s->b[i]);
            flags |= RD3_PEND_EVICTED;
            p = &s->b[RD3_CF_PEND_BUFS - 1];
        }
        p->buf = buf; p->n = 0; p->whole = 0; p->blocked = 0;
    }
    p->blocked = 0;   /* new rects: the waiting starts again */
    if (whole) { p->whole = 1; p->n = 0; return flags; }
    for (i = 0; i < n && !p->whole; i++) {
        if (r[i].w <= 0 || r[i].h <= 0)
            continue;
        if (p->n >= RD3_CF_PEND_RECTS) { p->whole = 1; p->n = 0; flags |= RD3_PEND_OVERFLOW; break; }
        p->r[p->n++] = r[i];
    }
    return flags;
}

/* the buffer is freed */
static inline void rd3_cf_pend_forget(struct rd3_cf_heal *s, const void *buf)
{
    struct rd3_cf_pend *p = rd3_cf_pend_get(s, buf);
    if (p)
        rd3_cf_pend_drop(s, p);
}

/* The rects to copy into 'dst' (a w x h picture now) from 'src' on top of
 * this call's: dst's pending rects that touch none src missed itself,
 * clipped to the picture (a rect outside it is dropped); those leave dst's
 * list (a copy of them that fails goes back with rd3_cf_pend_add). A whole
 * picture pending in dst: one rect of w x h, when src has nothing pending
 * and 'engine' (the copy engine is on), else nothing (it stays pending).
 * After RD3_CF_PEND_BLOCKED_MAX calls blocked by src's own pending rects,
 * they are copied anyway (info->forced). Returns how many rects went to
 * 'out' (at most 'max'). */
static inline unsigned rd3_cf_pend_take(struct rd3_cf_heal *s, const void *dst, const void *src, int w, int h,
                                        int engine, struct rd3_rect *out, unsigned max, struct rd3_cf_take *info)
{
    struct rd3_cf_pend *d = rd3_cf_pend_get(s, dst), *sp = rd3_cf_pend_get(s, src);
    unsigned i, j, k = 0, keep = 0, blockedAny = 0;
    int srcPending = sp && sp != d && (sp->n || sp->whole);
    if (info) { info->blocked = 0; info->forced = 0; }
    if (!d || !out || !max)
        return 0;
    const int forcing = srcPending && d->blocked >= RD3_CF_PEND_BLOCKED_MAX;
    if (forcing)
        srcPending = 0;
    if (d->whole) {
        if (!engine) {
            d->blocked = 0;   /* waiting for the copy engine, not for the source */
            return 0;
        }
        if (srcPending) {
            d->blocked++;
            if (info) info->blocked = 1;
            return 0;
        }
        out[0].x = 0; out[0].y = 0; out[0].w = w; out[0].h = h;
        rd3_cf_pend_drop(s, d);
        if (info && forcing && w > 0 && h > 0) info->forced = 1;
        return w > 0 && h > 0;
    }
    for (i = 0; i < d->n; i++) {
        int clash = srcPending && sp->whole;
        for (j = 0; srcPending && !clash && j < sp->n; j++)
            clash = rd3_rect_meet(&d->r[i], &sp->r[j]);
        blockedAny |= (unsigned)clash;
        if (clash || k == max) {
            d->r[keep++] = d->r[i];
        } else {
            out[k] = d->r[i];
            k += (unsigned)rd3_rect_clip(&out[k], w, h);
        }
    }
    d->n = keep;
    d->blocked = blockedAny ? d->blocked + 1 : 0;
    if (info) { info->blocked = blockedAny; info->forced = forcing && k > 0; }
    if (!d->n)
        rd3_cf_pend_drop(s, d);
    return k;
}

#endif
