/*
 * rdna3_swap_copy.h - a legacy surface swap as the copy Apple's family asks
 * for (card run 2026-10-17b). Plain C, no kernel headers: the accelerator
 * uses it and tests/test_swap_copy.c checks it on the host.
 *
 * IOAccelLegacySurface::submitSwapCopy(event, front, back) asks for the back
 * to be COPIED into the front - the display pipe's framebuffer resource,
 * which on Apple's drivers is what the head scans out - and the back is
 * WindowServer's again the moment the call returns. Copy forward
 * (submitCopyForward(event, fb, buffer 0 = the front, buffer 1 = the next
 * back, rects)) then refreshes the next back from the front, which holds the
 * last frame because of that copy. Until this run the accelerator flipped
 * the panel to the back instead (zero-copy, mode 0): WindowServer drew the
 * next frame into the buffer on the panel (card run 2026-10-17b: head 0
 * rewrote 85 of 229 shown frames in place), the front never held a frame
 * (so copy forward needed its own "copy from the buffer the head shows"
 * rules, rdna3_swap_front.h), and freed backs were still scanned
 * (swaps_shownfreed: 14 + 10).
 *
 * Mode 1 (debug.rdna3accel_swap_mode / boot-arg rdna3swapmode=1; the default
 * until card run 2026-10-17d, whose bisect found it broke both monitors, and
 * again since the write generations of rdna3_write_gen.h hold the old frames
 * that broke it) copies. The front resource stopped being the scanout on our card: the
 * family maps it into the GPU page table, which the scanout's vidmem cannot
 * be, frees it and gives the resource ordinary VRAM (card run 58b:
 * 0x87040000 / 0x88060000), so the panel needs its own copy. Each head has
 * two scanout buffers of the accelerator: the framebuffer's own ("home",
 * rdna3ScanoutInfo) and one the accelerator allocates (D, the head's mode
 * size). A swap
 *   1. waits until the head's last flip latched (rd3_sc_must_wait: the
 *      buffer the copy goes into may still be scanned until then; bounded by
 *      rdna3_flip_latch.h's cap),
 *   2. copies the back into the buffer NOT on the panel (rd3_sc_target) by
 *      SDMA (rdna3CopyVram, synchronous) and flips the head to it; the HUBP
 *      takes the new address at the next vblank, so no frame shows a half
 *      copy (amdgpu: a flip of a whole new buffer, latched at vblank -
 *      dm_pflip_high_irq - never a write into the buffer being scanned),
 *   3. copies the back into the front too (WindowServer's front, never
 *      scanned in this mode), so copy forward reads the last frame where
 *      Apple's family expects it (rd3_sc_cf_source).
 * The back is free when the call returns (both copies are done). Bounded:
 * one wait of at most the latch cap (2.5 frames, 35-100 ms) and two
 * synchronous copies (~0.65 ms each for 1920x1080 on the card). Without D
 * (no VRAM for it) the copy goes into home while the panel reads it
 * (RD3_SCT_HOME_DIRECT: may tear, counted). The panel never scans a buffer
 * WindowServer owns, so no freed buffer stays on it.
 *
 * WindowServer presents backs it never drew (card run 2026-10-17b: head
 * 0's second back, 0x8b8f0000, read empty at all of its 120 swaps; the
 * family's aperture access - its page-on, the only CPU path into a legacy
 * buffer - named only the first back). Copied, each of them would put a
 * black frame over the picture every other swap. So mode 1 copies a back
 * that reads empty only when there is no picture to lose (the front and the
 * head's last copy are empty too, a black start); otherwise the panel and
 * the front keep the last frame (RD3_SC_HOLD, swaps_copyempty_h<N>, traced,
 * the first ones logged with where WindowServer's frame did land). Mode 2
 * copies everything, black frames and stale backs too (Apple's literal copy,
 * the control for the A/B: neither the empty-back hold nor the stale-back
 * hold below applies). Mode 0 is the flip of card run 2026-10-17.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SWAP_COPY_H
#define RDNA3_SWAP_COPY_H

#include "rdna3_swap_front.h"

/* 0 flip (card run 2026-10-17; the default 2026-10-17d..g), 1 copy keeping
 * the picture over an empty back (the default again since the write
 * generations, rdna3_write_gen.h), 2 copy everything */
enum rd3_swap_mode { RD3_SWAPMODE_FLIP = 0, RD3_SWAPMODE_COPY = 1, RD3_SWAPMODE_COPY_ALL = 2 };
/* Card run 2026-10-17d's bisect ran on the default profile (WindowServer on
 * the CPU, the AGDC idle) and found mode 1 broke both monitors, so mode 0
 * became the default. On the agdc profile (17f, 17g) mode 0 is broken
 * itself: it flips the panel to buf[1], which WindowServer writes the next
 * frame into ("rewritten in place", "A LIVE WRITER IS PAINTING THE
 * SCANNED-OUT SURFACE"). Mode 1 is the only mode in which the panel never
 * shows a buffer WindowServer writes. What broke it in 17d is in that run's
 * trace: copy forward bursts put old frames into buf[2], which mode 1 copied
 * over the panel (the stale-back hold needs "no copy forward into it");
 * the write generations hold those (tests/test_write_gen.c: the model of
 * 17d's sequence shows every frame on the panel in order and no old one
 * in mode 1 with them, 8 old frames shown with the old rules). So the copy
 * is the default again; rdna3swapmode=0 / debug.rdna3accel_swap_mode=0 is
 * the flip. */
#define RD3_SWAPMODE_DEFAULT RD3_SWAPMODE_COPY
#define RD3_SWAPMODE_MAX     RD3_SWAPMODE_COPY_ALL

static inline int rd3_swapmode_valid(long v)
{
    return v >= RD3_SWAPMODE_FLIP && v <= RD3_SWAPMODE_MAX;
}

/* which argument is the back (the copy's source) and which the front (its
 * destination): 1 = a1, 2 = a2. The one that is a head's framebuffer
 * resource is the front; with none (a surface no head shows) or both,
 * submitSwapCopy(event, front, back): a1 the front, a2 the back */
struct rd3_sc_roles { int back, front, frontIsFb; };
static inline struct rd3_sc_roles rd3_sc_roles(int a1IsFb, int a2IsFb)
{
    struct rd3_sc_roles r;
    r.back = 2;
    r.front = 1;
    r.frontIsFb = a1IsFb;
    if (a2IsFb && !a1IsFb) {
        r.back = 1;
        r.front = 2;
        r.frontIsFb = 1;
    }
    return r;
}

/* what a swap does with its back */
enum rd3_sc_act {
    RD3_SC_COPY = 0,        /* the back has pixels (or could not be sampled): copied */
    RD3_SC_COPY_EMPTY = 1,  /* the back read empty: copied anyway (mode 2, or nothing to lose) */
    RD3_SC_HOLD = 2,        /* mode 1: an empty back over a picture: not copied, the last frame stays */
    RD3_SC_COPY_LANDED = 3, /* modes 1/2: the back is suspect, another buffer of the surface holds the new
                             * frame (rd3_sc_landed_pick): that one copied */
    RD3_SC_HOLD_STALE = 4   /* mode 1: a back WindowServer did not redraw since the head presented it, a newer
                             * frame after it (rd3_sc_stale_check / _confirm): not copied, the panel keeps that frame */
};
/* sampled: the back's 64 samples were read (a mapping and a geometry);
 * pixels: one was not 0; picture: the front, or the head's last copy, has
 * pixels (what an empty back would overwrite) */
static inline enum rd3_sc_act rd3_sc_action(int mode, int sampled, int pixels, int picture)
{
    if (!sampled || pixels)
        return RD3_SC_COPY;
    if (mode == RD3_SWAPMODE_COPY && picture)
        return RD3_SC_HOLD;
    return RD3_SC_COPY_EMPTY;
}

/* ---- the landed buffer (card run 2026-10-17c) ----
 * Evidence-driven, no amdgpu analogue: this is macOS legacy surface
 * semantics (IOAccelLegacySurface's buffer array, offset 0x11C0), which
 * Linux has no counterpart of. In card run 2026-10-17c head 0's swaps named
 * two backs in turns: 0x8b100000 (IOSurface 37, resource type 0xc0) and
 * 0x8b8f0000 (type 0x40, no IOSurface). WindowServer's frame was in
 * 0x8b100000 at 159 of 159 swaps (the trace's SCAN of the surface's other
 * buffers); 0x8b8f0000 read EMPTY at all 159 swaps that named it (16
 * aperture writes into it, 272 into 0x8b100000). Mode 1 held the panel at
 * each of those (437 "empty backs kept off a picture" on head 0), mode 0
 * flipped to them or skipped them.
 *
 * So at a swap whose named back reads empty - or is not IOSurface-backed
 * (type 0x40) and has not changed since the head last sampled it - modes
 * 1/2 look at the surface's other buffers (the array, and a1 when it is not
 * the head's framebuffer resource), sampled at THIS swap (never a record of
 * where frames landed before), and present the one that changed since the
 * head last sampled it, has pixels and is not the picture the panel already
 * shows; an IOSurface-backed one first (rd3_sc_landed_pick). Without one
 * the old rule stands (rd3_sc_action: hold over a picture, or copy). The
 * caller samples at most RD3_SC_LANDED_MAX buffers (64 BAR reads each), and
 * only at those swaps.
 *
 * The limit: a buffer WindowServer is drawing the NEXT frame into also
 * "changed"; presenting it would show a half-drawn frame. In run
 * 2026-10-17c the empty back's swap came 0.2-0.3 ms after the swap of the
 * drawn one, which had not changed again by then (2 changes of 0x8b100000
 * between two swaps in the whole run, both copy forward's writes) - so the
 * rule would not have fired there, and the hold lost no frame. It fires when
 * a frame really is in a buffer the swap did not name. So the accelerator
 * presents it only with debug.rdna3accel_swap_landed=1 (boot-arg
 * rdna3swaplanded); by default (0) it counts and traces what it WOULD have
 * presented (swaps_copylanded_would_h<N>) and keeps the old rule. */
#define RD3_SC_LANDED_MAX 6u
struct rd3_sc_cand {
    int sampled;    /* its 64 samples were read (a mapping and a geometry) */
    int pixels;     /* one of them was not 0 */
    int changed;    /* they differ from the head's last look at this buffer (never seen before: 0) */
    int shown;      /* they equal the samples of the frame the head's panel shows (its last copy) */
    int iosurface;  /* IOSurface-backed (resource type 0xc0) */
};
/* the named back is suspect: modes 1/2 look for the landed buffer.
 * backChanged: the back's samples differ from the head's last look, or it
 * was never seen (unknown: trusted) */
static inline int rd3_sc_landed_wanted(int mode, int backSampled, int backPixels, int backChanged,
                                       int backIOSurface)
{
    if (mode != RD3_SWAPMODE_COPY && mode != RD3_SWAPMODE_COPY_ALL)
        return 0;
    if (!backSampled)
        return 0;   /* nothing known of the back: copied as before */
    if (!backPixels)
        return 1;
    return !backIOSurface && !backChanged;
}
/* the buffer to present instead of the back: the index into c[], -1 none */
static inline int rd3_sc_landed_pick(const struct rd3_sc_cand *c, unsigned n)
{
    int best = -1;
    for (unsigned i = 0; c && i < n && i < RD3_SC_LANDED_MAX; i++) {
        if (!c[i].sampled || !c[i].pixels || !c[i].changed || c[i].shown)
            continue;
        if (best < 0 || (c[i].iosurface && !c[best].iosurface))
            best = (int)i;
    }
    return best;
}

/* ---- the stale back (card run 2026-10-17c) ----
 * Evidence-driven, no amdgpu analogue (macOS legacy surface semantics, as
 * the landed buffer above). WindowServer issues two legacy swaps per frame
 * on a head: a1 = B1, the frame (copy forward refreshed it from the front
 * just before), then a2 = B2, which it did not draw this frame. Head 1's
 * B2 (0x8d0c0000) got pixels once, from a window animation that rotated
 * three buffers, and was never drawn again; from then on every frame the
 * mode 1 copy put B1's new frame on the panel and, 5-14 ms later, B2's old
 * one (and into the front, from where the next copy forward spread it into
 * the next frame): windows appeared and disappeared on the side monitor.
 * 259 such copies on head 1 (0x8d0c0000, later 0x8d8b0000, ...) of its
 * 709, none on head 0; no WindowServer GPU work was pending at those swaps.
 *
 * So mode 1 remembers, per head and per buffer address, what each back
 * looked like when the head last presented it - the hashes of two grids of
 * 64 samples (rd3_sc_sig64: grid 0 the trace's, grid 1 shifted by half a
 * cell) - and whether a copy forward targeted it since. A back with pixels
 * is a hold candidate (rd3_sc_stale_check) when
 *   (a) its grid 0 samples equal those of its last presentation here,
 *   (b) no copy forward targeted it since,
 *   (c) a different buffer was presented on the head after it, and
 *   (d) the panel holds a frame this mode copied (after a mode set or
 *       another flip it holds none);
 * it is HELD (no copy to the panel or the front: the panel keeps the newer
 * frame, as for an empty back) when grid 1, read again then, matches too
 * (rd3_sc_stale_confirm: a small redraw between grid 0's points is not
 * held). Never presented here (no record): copied. Mode 2 stays the
 * literal "copy everything" control and never holds; mode 0 never decides.
 * debug.rdna3accel_swap_stale / boot-arg rdna3swapstale = 0 only counts
 * what would have been held.
 *
 * The records are forgotten when the surface or the head's framebuffer
 * resource goes, at a display or swap mode change and D's reallocation
 * (rd3_sc_stale_reset), and per address when its VRAM is freed
 * (rd3_sc_stale_forget_addr). Fixed size, no allocation: a buffer evicted
 * from a full table is copied at its next swap (counted). The alpha byte
 * counts in these hashes; the trace's band bytes are RGB only, so the
 * decoder can take an alpha-only change for "unchanged" (it may count a
 * stale copy the kernel rightly made). */
#define RD3_SC_STALE_SLOTS 8u
#define RD3_SC_SIG_INIT 14695981039346656037ull
#define RD3_SC_GRID_N 64u
/* 64-bit FNV-1a over a 32-bit sample, every byte (alpha too) */
static inline unsigned long long rd3_sc_sig_add(unsigned long long h, unsigned v)
{
    for (unsigned i = 0; i < 4; i++) {
        h ^= (v >> (8u * i)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}
/* the hash of a grid's 64 samples, in grid order */
static inline unsigned long long rd3_sc_sig64(const unsigned *px)
{
    unsigned long long h = RD3_SC_SIG_INIT;
    for (unsigned k = 0; px && k < RD3_SC_GRID_N; k++)
        h = rd3_sc_sig_add(h, px[k]);
    return h;
}
/* sample k (0..63, row-major 8 x 8) of a w x h buffer of 'pitch' bytes per
 * row and 'bytes' in all: its index in 32-bit words, 0 when outside it (the
 * sample then reads 0). Grid 0 is rd3AccelPxScan's (x = w * i / 9, i 1..8),
 * grid 1 the same shifted by half a cell (x = w * (2i + 1) / 18) */
static inline int rd3_sc_grid_index(unsigned grid, unsigned w, unsigned h, unsigned pitch, unsigned long long bytes,
                                    unsigned k, unsigned long *idx)
{
    const unsigned ix = k % 8u + 1u, iy = k / 8u + 1u;
    const unsigned x = grid ? (unsigned)(((unsigned long long)w * (2u * ix + 1u)) / 18u)
                            : (unsigned)(((unsigned long long)w * ix) / 9u);
    const unsigned y = grid ? (unsigned)(((unsigned long long)h * (2u * iy + 1u)) / 18u)
                            : (unsigned)(((unsigned long long)h * iy) / 9u);
    *idx = 0;
    if (k >= RD3_SC_GRID_N || x >= w || y >= h || (unsigned long long)y * pitch + x * 4ull + 4u > bytes)
        return 0;
    *idx = (unsigned long)y * (pitch / 4u) + x;
    return 1;
}

struct rd3_sc_stale_rec {
    unsigned long long addr;   /* the buffer's VRAM address; 0: a free slot */
    unsigned long long sig;    /* grid 0's hash at the head's last presentation of it */
    unsigned long long sig2;   /* grid 1's */
    unsigned shownAt;          /* that swap's number */
    int cfSince;               /* a copy forward targeted it since */
};
struct rd3_sc_stale {
    struct rd3_sc_stale_rec r[RD3_SC_STALE_SLOTS];
    unsigned long long lastShown;   /* the buffer the head presented last; 0: none (or freed) */
    unsigned next;                  /* the slot a new buffer takes when none is free */
};
/* why a back is or is not a hold candidate */
enum rd3_sc_stale_why {
    RD3_SCST_HOLD = 0,        /* a candidate: (a)-(d) hold; grid 1 decides (rd3_sc_stale_confirm) */
    RD3_SCST_OFF = 1,         /* not mode 1, no pixels or no address: not judged, not counted */
    RD3_SCST_NORECORD = 2,    /* never presented on this head (or forgotten, evicted) */
    RD3_SCST_CHANGED = 3,     /* (a) its samples differ from its last presentation */
    RD3_SCST_CF = 4,          /* (b) unchanged, but a copy forward targeted it since */
    RD3_SCST_SAMESHOWN = 5,   /* (c) nothing else presented after it (or that one freed) */
    RD3_SCST_NOPANEL = 6      /* (d) the panel holds no frame this mode copied */
};
static inline struct rd3_sc_stale_rec *rd3_sc_stale_find(struct rd3_sc_stale *s, unsigned long long addr)
{
    for (unsigned i = 0; s && addr && i < RD3_SC_STALE_SLOTS; i++)
        if (s->r[i].addr == addr)
            return &s->r[i];
    return 0;
}
/* forgets every record; returns how many there were (lastShown counts) */
static inline unsigned rd3_sc_stale_reset(struct rd3_sc_stale *s)
{
    unsigned had = s->lastShown != 0;
    for (unsigned i = 0; i < RD3_SC_STALE_SLOTS; i++) {
        had += s->r[i].addr != 0;
        s->r[i].addr = 0;
        s->r[i].sig = 0;
        s->r[i].sig2 = 0;
        s->r[i].shownAt = 0;
        s->r[i].cfSince = 0;
    }
    s->lastShown = 0;
    s->next = 0;
    return had;
}
/* the back at 'addr' whose grid 0 hashes to 'sig': the rules (a)-(d) in
 * the order (a) (b) (c) (d); *shownAt the swap that last presented it;
 * *nocf: it changed although no copy forward targeted it (WindowServer
 * redrew it some other way: the assumption the rule rests on, counted) */
static inline enum rd3_sc_stale_why rd3_sc_stale_check(struct rd3_sc_stale *s, int mode, int pixels, int panelFrame,
                                                       unsigned long long addr, unsigned long long sig,
                                                       unsigned *shownAt, int *nocf)
{
    const struct rd3_sc_stale_rec *r;
    if (nocf)
        *nocf = 0;
    if (mode != RD3_SWAPMODE_COPY || !pixels || !addr)
        return RD3_SCST_OFF;
    r = rd3_sc_stale_find(s, addr);
    if (!r)
        return RD3_SCST_NORECORD;
    if (shownAt)
        *shownAt = r->shownAt;
    if (r->sig != sig) {
        if (nocf)
            *nocf = !r->cfSince;
        return RD3_SCST_CHANGED;
    }
    if (r->cfSince)
        return RD3_SCST_CF;
    if (!s->lastShown || s->lastShown == addr)
        return RD3_SCST_SAMESHOWN;
    if (!panelFrame)
        return RD3_SCST_NOPANEL;
    return RD3_SCST_HOLD;
}
/* a candidate's grid 1, read again at the swap: held only when it matches
 * the presentation's (else a redraw between grid 0's points: copied) */
static inline int rd3_sc_stale_confirm(struct rd3_sc_stale *s, unsigned long long addr, unsigned long long sig2)
{
    const struct rd3_sc_stale_rec *r = rd3_sc_stale_find(s, addr);
    return r && r->sig2 == sig2;
}
/* swap 'n' presented the buffer at 'addr' (grid hashes 'sig', 'sig2');
 * returns the address it evicted from a full table (0 none) */
static inline unsigned long long rd3_sc_stale_shown(struct rd3_sc_stale *s, unsigned long long addr,
                                                    unsigned long long sig, unsigned long long sig2, unsigned n)
{
    struct rd3_sc_stale_rec *r = rd3_sc_stale_find(s, addr);
    unsigned long long evicted = 0;
    if (!addr)
        return 0;
    for (unsigned i = 0; !r && i < RD3_SC_STALE_SLOTS; i++)
        if (!s->r[i].addr)
            r = &s->r[i];
    if (!r) {
        r = &s->r[s->next % RD3_SC_STALE_SLOTS];
        s->next = (s->next + 1u) % RD3_SC_STALE_SLOTS;
        evicted = r->addr;
    }
    r->addr = addr;
    r->sig = sig;
    r->sig2 = sig2;
    r->shownAt = n;
    r->cfSince = 0;
    s->lastShown = addr;
    return evicted;
}
/* a copy forward targeted the buffer at 'addr' (WindowServer refreshes it
 * for a new frame): not held until it is presented again. Marked in every
 * head's table: a VRAM address is one buffer, and a copy forward's head
 * may not be the one its swaps are recorded under; a mark can only turn a
 * hold into a copy. Returns how many tables had it. */
static inline unsigned rd3_sc_stale_cf(struct rd3_sc_stale *heads, unsigned nheads, unsigned long long addr)
{
    unsigned k = 0;
    for (unsigned h = 0; heads && h < nheads; h++) {
        struct rd3_sc_stale_rec *r = rd3_sc_stale_find(&heads[h], addr);
        if (r) {
            r->cfSince = 1;
            k++;
        }
    }
    return k;
}
/* the buffer at 'addr' was freed: its record leaves this head's table (a
 * new buffer there is never taken for it), and when it was the buffer
 * presented last, "something else was presented after" is no longer known
 * (lastShown 0: rule (c) then copies). Returns 1 when the table had it. */
static inline int rd3_sc_stale_forget_addr(struct rd3_sc_stale *s, unsigned long long addr)
{
    struct rd3_sc_stale_rec *r = rd3_sc_stale_find(s, addr);
    int had = 0;
    if (!addr)
        return 0;
    if (s->lastShown == addr) {
        s->lastShown = 0;
        had = 1;
    }
    if (r) {
        r->addr = 0;
        r->cfSince = 0;
        had = 1;
    }
    return had;
}

/* what the head's panel shows as far as the accelerator knows */
enum rd3_sc_shows {
    RD3_SCS_OTHER = 0,   /* not the accelerator's D: home, a WindowServer buffer (mode 0), an IOP surface */
    RD3_SCS_HOME = 1,    /* home, flipped to by a mode 1/2 swap or a mode change */
    RD3_SCS_D = 2        /* the accelerator's buffer */
};
/* where the copy goes */
enum rd3_sc_target {
    RD3_SCT_NONE = 0,
    RD3_SCT_HOME = 1,        /* home, not on the panel: copied, then flipped to */
    RD3_SCT_D = 2,           /* D, not on the panel: copied, then flipped to */
    RD3_SCT_HOME_DIRECT = 3  /* no D: home while the panel may read it (may tear), then flipped to */
};
static inline enum rd3_sc_target rd3_sc_target(enum rd3_sc_shows shows, int haveD)
{
    if (!haveD)
        return RD3_SCT_HOME_DIRECT;
    return shows == RD3_SCS_D ? RD3_SCT_HOME : RD3_SCT_D;
}
/* the copy is tear-free: it goes into a buffer the panel does not read */
static inline int rd3_sc_tear_free(enum rd3_sc_target t)
{
    return t == RD3_SCT_HOME || t == RD3_SCT_D;
}
/* wait for the head's last flip before copying: it may still leave the
 * buffer the copy goes into (two buffers take turns); not seen latched yet */
static inline int rd3_sc_must_wait(enum rd3_sc_target t, int flipped, int latched)
{
    return rd3_sc_tear_free(t) && flipped && !latched;
}
/* what the panel shows after the swap's flip (done or refused) */
static inline enum rd3_sc_shows rd3_sc_after_flip(enum rd3_sc_shows before, enum rd3_sc_target t, int flipDone)
{
    if (!flipDone)
        return before;
    return t == RD3_SCT_D ? RD3_SCS_D : RD3_SCS_HOME;
}

/* D's geometry for a head showing w x h: shapeSurface's pitch rule (256
 * byte rows), 4-byte pixels */
static inline unsigned rd3_sc_pitch(unsigned w)
{
    return (w * 4u + 255u) & ~255u;
}
/* D must be (re)allocated: none yet, or another mode's size */
static inline int rd3_sc_d_stale(int haveD, unsigned dW, unsigned dH, unsigned w, unsigned h)
{
    return !haveD || dW != w || dH != h;
}
/* D may be freed now: the panel does not show it and the head's last flip
 * (away from D, or anyone's: a mode 0 or IOP flip, posted to the swaps)
 * was SEEN latched - until then the HUBP may still read D (ECC review
 * round 1: a freed D is VRAM the next allocation scribbles on while it is
 * on the panel). 'seen' is an observation of the latch, never a wait that
 * gave up (round 2). */
static inline int rd3_sc_d_freeable(enum rd3_sc_shows shows, int flipped, int seen)
{
    return shows != RD3_SCS_D && (!flipped || seen);
}

/* A panel move posted from outside the family's lock (an IOP flip, a
 * display mode change) applies only when it is newer than the head's last
 * flip of its own: a post that was in flight while a swap flipped would
 * otherwise undo that swap's record (ECC review round 2). Times in any
 * monotonic unit (mach_absolute_time). */
static inline int rd3_sc_post_applies(unsigned long long postAt, unsigned long long lastOwnFlipAt)
{
    return postAt > lastOwnFlipAt;
}

/* the wait before a copy: none, the bounded wait for the latch, or (the
 * head's waits are off: rdna3_flip_latch.h's RD3_LATCH_TIMEOUT_RUN timeouts
 * in a row) one look without waiting - a latch seen there turns the waits
 * back on (rd3_latch_seen) */
enum rd3_sc_wait { RD3_SCW_NONE = 0, RD3_SCW_WAIT = 1, RD3_SCW_LOOK = 2 };
static inline enum rd3_sc_wait rd3_sc_wait_plan(enum rd3_sc_target t, int flipped, int latched, int waitsOff)
{
    if (!rd3_sc_must_wait(t, flipped, latched))
        return RD3_SCW_NONE;
    return waitsOff ? RD3_SCW_LOOK : RD3_SCW_WAIT;
}

/* the front the head's last copy reached (copy forward's source): forgotten
 * when that allocation is freed, so a new buffer at the same address (and
 * the same VRAM offset) is never taken for one holding the last frame (ECC
 * review round 1) */
struct rd3_sc_front { const void *vm; unsigned long long cookie; };
static inline void rd3_sc_front_set(struct rd3_sc_front *f, const void *vm, unsigned long long cookie)
{
    f->vm = vm;
    f->cookie = cookie;
}
static inline int rd3_sc_front_forget(struct rd3_sc_front *f, const void *gone)
{
    if (!gone || f->vm != gone)
        return 0;
    f->vm = 0;
    f->cookie = 0;
    return 1;
}
static inline int rd3_sc_front_current(const struct rd3_sc_front *f, const void *vm, unsigned long long cookie)
{
    return vm && f->vm == vm && f->cookie == cookie;
}

/* The SDMA breaker of modes 1/2 (ECC review round 1). Every swap copies by
 * SDMA, synchronously: a ring that times out costs its timeout (~100 ms)
 * at every swap. RD3_SC_BRK_RUN failures in a row, or one timeout, open the
 * breaker: the swaps flip as in mode 0 (the panel keeps getting frames, the
 * flicker of card run 2026-10-17 comes back, nothing waits on the engine)
 * until RD3_SC_BRK_RETRY_US later (by the clock, not by swaps: a quiet
 * desktop swaps rarely, round 2) one swap tries SDMA again; its success
 * closes the breaker, its failure opens it for another RD3_SC_BRK_RETRY_US.
 * Every transition is an event the caller logs and traces. 'now' is in
 * microseconds. */
#define RD3_SC_BRK_RUN      3u
#define RD3_SC_BRK_RETRY_US 10000000ull   /* 10 s */
struct rd3_sc_brk {
    unsigned run;            /* failures in a row */
    int off;                 /* open: the swaps flip */
    int probing;             /* the one swap trying SDMA again */
    unsigned long long offAt;/* when it opened (us) */
    unsigned trips;          /* times it opened */
};
enum rd3_sc_brk_ev {
    RD3_SC_BRK_NONE = 0, RD3_SC_BRK_OPENED = 1, RD3_SC_BRK_PROBE = 2, RD3_SC_BRK_CLOSED = 3,
    RD3_SC_BRK_REOPENED = 4   /* the probe failed: open for another RD3_SC_BRK_RETRY_US */
};
/* whether a swap at 'now' (us) copies (else it flips); *ev: a probe begins */
static inline int rd3_sc_brk_allow(struct rd3_sc_brk *b, unsigned long long now, enum rd3_sc_brk_ev *ev)
{
    *ev = RD3_SC_BRK_NONE;
    if (!b->off)
        return 1;
    if (!b->probing && now - b->offAt >= RD3_SC_BRK_RETRY_US) {
        b->probing = 1;
        *ev = RD3_SC_BRK_PROBE;
        return 1;
    }
    return b->probing;
}
/* a copy failed ('timedOut': the engine's fence did not come) */
static inline enum rd3_sc_brk_ev rd3_sc_brk_fail(struct rd3_sc_brk *b, unsigned long long now, int timedOut)
{
    b->run++;
    if (b->probing || (!b->off && (timedOut || b->run >= RD3_SC_BRK_RUN))) {
        const int was = b->off;
        b->off = 1;
        b->probing = 0;
        b->offAt = now;
        if (!was) {
            b->trips++;
            return RD3_SC_BRK_OPENED;
        }
        return RD3_SC_BRK_REOPENED;
    }
    return RD3_SC_BRK_NONE;
}
/* a copy went through */
static inline enum rd3_sc_brk_ev rd3_sc_brk_ok(struct rd3_sc_brk *b)
{
    const int was = b->off;
    b->run = 0;
    b->off = 0;
    b->probing = 0;
    return was ? RD3_SC_BRK_CLOSED : RD3_SC_BRK_NONE;
}

/* the part of a copy both sides hold: the narrower width and the lower
 * height, rows that end inside the destination's allocation; 0: nothing */
static inline int rd3_sc_extent(unsigned sw, unsigned sh, unsigned spitch, unsigned long long sbytes, unsigned dw,
                                unsigned dh, unsigned dpitch, unsigned long long dbytes, unsigned *rowBytes,
                                unsigned *rows)
{
    const unsigned w = sw < dw ? sw : dw;
    unsigned h = sh < dh ? sh : dh;
    const unsigned rb = w * 4u;
    *rowBytes = 0;
    *rows = 0;
    if (!w || !h || rb > spitch || rb > dpitch || rb > sbytes || rb > dbytes)
        return 0;
    if ((unsigned long long)(h - 1u) * spitch + rb > sbytes)
        h = (unsigned)((sbytes - rb) / spitch) + 1u;
    if ((unsigned long long)(h - 1u) * dpitch + rb > dbytes)
        h = (unsigned)((dbytes - rb) / dpitch) + 1u;
    *rowBytes = rb;
    *rows = h;
    return 1;
}

/* copy forward's source in modes 1 and 2 */
enum rd3_sc_cf {
    RD3_SCCF_SAME = 0,     /* the front and the target are one buffer: nothing to do */
    RD3_SCCF_SRC = 1,      /* the front WindowServer named holds the last frame (the swap copied it there) */
    RD3_SCCF_SCANOUT = 2,  /* the head's front missed that copy: from the scanout buffer the panel shows */
    RD3_SCCF_STALE = 3     /* neither holds it: nothing copied (counted, the rects stay pending) */
};
struct rd3_sc_cf_in {
    int srcIsDst;        /* the front and the target are one buffer */
    int srcIsHeadFront;  /* the front is a head's framebuffer resource (else: a surface no head shows) */
    int frontCurrent;    /* that head's last swap copied into this front (same allocation) */
    int scanoutKnown;    /* that head shows a scanout buffer of the accelerator, its last frame */
    int srcNewer;        /* the front was written after the frame the head presented last (rdna3_write_gen.h
                          * rd3_wg_src_newer, card run 2026-10-17d): a newer frame than the panel's, the source */
};
static inline enum rd3_sc_cf rd3_sc_cf_source(const struct rd3_sc_cf_in *in)
{
    if (!in || in->srcIsDst)
        return RD3_SCCF_SAME;
    if (!in->srcIsHeadFront || in->frontCurrent || in->srcNewer)
        return RD3_SCCF_SRC;
    return in->scanoutKnown ? RD3_SCCF_SCANOUT : RD3_SCCF_STALE;
}

#endif
