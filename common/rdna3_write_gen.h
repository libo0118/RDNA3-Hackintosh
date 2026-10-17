/*
 * rdna3_write_gen.h - whether the back of a legacy swap holds a frame
 * WindowServer wrote after the one the head shows (card runs 2026-10-17d,
 * f, g). Plain C, no kernel headers: the accelerator uses it and
 * tests/test_write_gen.c checks it on the host.
 *
 * Evidence-driven, no amdgpu analogue: this is macOS legacy surface
 * semantics (IOAccelLegacySurface, IOAccelLegacyDisplayPipe), which Linux
 * has no counterpart of. On the agdc profile WindowServer composites on the
 * CPU, through the family's aperture (its page-on: RD3Resource::
 * addToAperture / getApertureMemoryDescriptor), into buf[1] of each display
 * surface, every frame. Card run 2026-10-17d: 0x88850000 had 441 page-ons,
 * its partner 0x89040000 none. Each frame is
 *   page-on b1 (~13 ms after the last swap), copy forward b0 -> b1 (~1.9 ms
 *   before the swap), submitSwapCopy(b0, b1), ~0.36 ms later
 *   submitSwapCopy(b0, b2)
 * and WindowServer never draws b2. In bursts copy forward rotates the
 * surface's three buffers 0 -> 1 -> 2 -> 0 at ~60 Hz with no swap at all
 * (17d: 688.51-688.72 s, 803.1-806.6 s; buffer 0 of the array rotates):
 * the family presents those frames by a path the driver did not see
 * (IOAccelLegacyDisplayPipe::submitFlipBuffer, traced since this change).
 * A burst leaves an old frame in b2. Mode 0 then flipped the panel to it
 * (h0 at 309-360 s of 17d: 0x89040000, last written at the burst, flipped
 * to about once a second) and copy forward's COPY_SHOWN rule copied it back
 * over WindowServer's new b1; mode 1 copied it over the panel (the stale-back
 * hold of 2026-10-17c requires "no copy forward into it", which the burst
 * broke: 0x8b7f0000 on h1 at 61-262 s, 0x8ac10000's 4 stale copies on h0).
 * The pixel and hash rules (empty back, stale back, landed buffer) guess
 * at what one signal says directly: a write.
 *
 * So every write into a buffer - the family's CPU page-on and a copy
 * forward that targets it - stamps the buffer's VRAM address with the next
 * value of one global counter, its generation (rd3_wg_bump). Each head
 * remembers the generation of the frame it presented last. A swap presents
 * its back only when the back's generation is newer than that
 * (rd3_wg_decide: RD3_WGW_NEWER): WindowServer wrote it after the frame the
 * panel shows. Otherwise the swap holds: no flip, no copy, no front copy, and
 * the panel keeps its frame. Copy forward takes the front WindowServer named
 * as its source whenever that front was written after the frame the head
 * shows (rd3_wg_src_newer: the burst's chain), so its source is never a
 * buffer older than the panel's frame.
 *
 * Over card run 2026-10-17d's trace this rule holds 1056 of 2080 swaps,
 * none of them while the back's band bytes differed from those at its last
 * presentation on the head (a frame never held back), and it holds every
 * stale b2 the run copied or flipped to (64 copies in mode 1, 83 flips in
 * mode 0).
 *
 * Guards against a write this does not see (both counted, the second
 * logged as a WARN by triage: a writer the accelerator does not know):
 *  - the first swap after the head was reset (boot, display mode change,
 *    swap mode change, a panel move by someone else) presents
 *    (RD3_WGW_FIRST);
 *  - a back whose 64 samples differ from those it had when this head
 *    presented it last presents (RD3_WGW_CONTENT).
 * A back with no write recorded that this head never presented is held
 * (RD3_WGW_UNWRITTEN: the b2 WindowServer never draws).
 *
 * A hold never freezes the panel (ECC review round 1): when the head has not
 * presented for RD3_WG_AGE_US and this is its RD3_WG_AGE_HOLDS-th hold in a
 * row, the back presents (RD3_WGW_AGED; 17c and 17d's traces have no such
 * case, their longest hold runs were 5 swaps inside 250 ms), and a back with
 * no generation presents while a generation newer than the head's frame was
 * evicted from the full table (RD3_WGW_EVICTED: it may be that one). The
 * caller looks for a newer buffer of the surface before it holds
 * (rd3_wg_landed_pick: a frame WindowServer wrote into a buffer the swap did
 * not name presents instead).
 *
 * Fixed size, no allocation: RD3_WG_SLOTS buffer addresses, the oldest
 * generation evicted from a full table (that buffer then reads 0, "never
 * written", until its next write); forgotten per address when its VRAM is
 * freed (rd3_wg_forget, rd3_wg_head_forget). The generations are 64-bit
 * and only grow.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_WRITE_GEN_H
#define RDNA3_WRITE_GEN_H

#define RD3_WG_SLOTS     32u   /* buffer addresses with a generation (3 per display surface, other surfaces) */
#define RD3_WG_HEAD_SIGS 4u    /* buffers whose samples a head remembers from their presentation */
#define RD3_WG_AGE_US    250000ull   /* no present for this long ... */
#define RD3_WG_AGE_HOLDS 3u          /* ... and this many holds in a row: the back presents (RD3_WGW_AGED) */

struct rd3_wg_rec {
    unsigned long long addr;   /* the buffer's VRAM address; 0: a free slot */
    unsigned long long gen;    /* the generation of its last write */
};
struct rd3_wg {
    struct rd3_wg_rec r[RD3_WG_SLOTS];
    unsigned long long seq;    /* the last generation handed out */
    unsigned long long evictedMax;   /* the newest generation evicted from a full table (0: none) */
};

static inline struct rd3_wg_rec *rd3_wg_find(struct rd3_wg *w, unsigned long long addr)
{
    for (unsigned i = 0; w && addr && i < RD3_WG_SLOTS; i++)
        if (w->r[i].addr == addr)
            return &w->r[i];
    return 0;
}
/* the generation of the last write into 'addr'; 0: none recorded */
static inline unsigned long long rd3_wg_gen(const struct rd3_wg *w, unsigned long long addr)
{
    for (unsigned i = 0; w && addr && i < RD3_WG_SLOTS; i++)
        if (w->r[i].addr == addr)
            return w->r[i].gen;
    return 0;
}
/* a write into 'addr': its new generation (0: no address). A full table
 * gives up the slot with the oldest generation; *evicted (may be NULL) is
 * that slot's address, 0 when nothing was evicted */
static inline unsigned long long rd3_wg_bump(struct rd3_wg *w, unsigned long long addr, unsigned long long *evicted)
{
    struct rd3_wg_rec *r;
    if (evicted)
        *evicted = 0;
    if (!w || !addr)
        return 0;
    r = rd3_wg_find(w, addr);
    for (unsigned i = 0; !r && i < RD3_WG_SLOTS; i++)
        if (!w->r[i].addr)
            r = &w->r[i];
    if (!r) {
        r = &w->r[0];
        for (unsigned i = 1; i < RD3_WG_SLOTS; i++)
            if (w->r[i].gen < r->gen)
                r = &w->r[i];
        if (evicted)
            *evicted = r->addr;
        if (r->gen > w->evictedMax)
            w->evictedMax = r->gen;
    }
    r->addr = addr;
    r->gen = ++w->seq;
    return r->gen;
}
/* the VRAM at 'addr' was freed: a new buffer there is never taken for one
 * WindowServer wrote; returns 1 when the table had it */
static inline int rd3_wg_forget(struct rd3_wg *w, unsigned long long addr)
{
    struct rd3_wg_rec *r = rd3_wg_find(w, addr);
    if (!r)
        return 0;
    r->addr = 0;
    r->gen = 0;
    return 1;
}

/* a head: the frame it presented last and the samples of the buffers it
 * presented (the content guard) */
struct rd3_wg_sig {
    unsigned long long addr, sig;   /* addr 0: a free slot */
};
struct rd3_wg_head {
    unsigned long long gen;    /* the newest generation it presented (0: none written) */
    unsigned long long addr;   /* the buffer of its last presentation */
    int presented;             /* it presented since its last reset */
    unsigned long long presentedUs;   /* when it presented last (any monotonic microseconds) */
    unsigned holds;            /* swaps held in a row since */
    unsigned next;             /* the sample slot a new buffer takes when none is free */
    struct rd3_wg_sig s[RD3_WG_HEAD_SIGS];
};

/* what the rule says about a back, and why */
enum rd3_wg_why {
    RD3_WGW_OFF = 0,        /* not judged: no head, no address */
    RD3_WGW_NEWER = 1,      /* written after the head's last frame: presented */
    RD3_WGW_FIRST = 2,      /* the head presented nothing since its reset: presented */
    RD3_WGW_CONTENT = 3,    /* no write seen since, but its samples changed since this head presented it: presented */
    RD3_WGW_SAME = 4,       /* the buffer the panel shows, not written since: held */
    RD3_WGW_OLDER = 5,      /* written before the head's last frame: held */
    RD3_WGW_UNWRITTEN = 6,  /* no write recorded (never, evicted or freed) and not presented here: held */
    RD3_WGW_AGED = 7,       /* would hold, but the head has not presented for RD3_WG_AGE_US and RD3_WG_AGE_HOLDS
                             * holds in a row: presented (a hold never freezes the panel) */
    RD3_WGW_EVICTED = 8     /* no generation, and one newer than the head's frame was evicted: presented */
};
#define RD3_WGW_MAX RD3_WGW_EVICTED

static inline int rd3_wg_presents(enum rd3_wg_why w)
{
    return w == RD3_WGW_NEWER || w == RD3_WGW_FIRST || w == RD3_WGW_CONTENT || w == RD3_WGW_AGED ||
           w == RD3_WGW_EVICTED;
}
static inline int rd3_wg_holds(enum rd3_wg_why w)
{
    return w == RD3_WGW_SAME || w == RD3_WGW_OLDER || w == RD3_WGW_UNWRITTEN;
}

static inline const struct rd3_wg_sig *rd3_wg_head_sig(const struct rd3_wg_head *h, unsigned long long addr)
{
    for (unsigned i = 0; h && addr && i < RD3_WG_HEAD_SIGS; i++)
        if (h->s[i].addr == addr)
            return &h->s[i];
    return 0;
}

/* the back at 'addr' with generation 'gen' (rd3_wg_gen); 'sampled': its
 * 64 samples were read at this swap and hash to 'sig'; 'evictedMax' the
 * table's (rd3_wg.evictedMax), 'nowUs' the time in the unit of
 * rd3_wg_presented's */
static inline enum rd3_wg_why rd3_wg_decide(const struct rd3_wg_head *h, unsigned long long addr,
                                            unsigned long long gen, int sampled, unsigned long long sig,
                                            unsigned long long evictedMax, unsigned long long nowUs)
{
    const struct rd3_wg_sig *s;
    if (!h || !addr)
        return RD3_WGW_OFF;
    if (!h->presented)
        return RD3_WGW_FIRST;
    if (gen > h->gen)
        return RD3_WGW_NEWER;
    s = rd3_wg_head_sig(h, addr);
    if (sampled && s && s->sig != sig)
        return RD3_WGW_CONTENT;
    if (!gen && evictedMax > h->gen)
        return RD3_WGW_EVICTED;
    if (h->holds + 1u >= RD3_WG_AGE_HOLDS && nowUs >= h->presentedUs && nowUs - h->presentedUs >= RD3_WG_AGE_US)
        return RD3_WGW_AGED;
    if (addr == h->addr)
        return RD3_WGW_SAME;
    return gen ? RD3_WGW_OLDER : (s ? RD3_WGW_OLDER : RD3_WGW_UNWRITTEN);
}
/* the swap was held (the run RD3_WGW_AGED counts) */
static inline void rd3_wg_held(struct rd3_wg_head *h)
{
    if (h)
        h->holds++;
}
/* before a hold: the buffer of the surface written after the head's frame,
 * the newest - WindowServer's frame in a buffer the swap did not name.
 * gens[i] the generation of candidate i (0: none / not a candidate: the
 * back, the head's framebuffer resource, no VRAM); the index, -1 none */
static inline int rd3_wg_landed_pick(const unsigned long long *gens, unsigned n, unsigned long long headGen)
{
    int best = -1;
    for (unsigned i = 0; gens && i < n; i++)
        if (gens[i] > headGen && (best < 0 || gens[i] > gens[best]))
            best = (int)i;
    return best;
}

/* the head presented the buffer at 'addr' (generation 'gen'; its samples
 * 'sig' when 'sampled', else its old record goes: unknown is never
 * "unchanged"). The head's generation never goes back: a buffer presented
 * by the content guard keeps the order of the writes that were seen. */
static inline void rd3_wg_presented(struct rd3_wg_head *h, unsigned long long addr, unsigned long long gen,
                                    int sampled, unsigned long long sig, unsigned long long nowUs)
{
    struct rd3_wg_sig *s = 0;
    if (!h || !addr)
        return;
    h->presented = 1;
    h->presentedUs = nowUs;
    h->holds = 0;
    if (gen > h->gen)
        h->gen = gen;
    h->addr = addr;
    for (unsigned i = 0; i < RD3_WG_HEAD_SIGS && !s; i++)
        if (h->s[i].addr == addr)
            s = &h->s[i];
    if (!sampled) {
        if (s) {
            s->addr = 0;
            s->sig = 0;
        }
        return;
    }
    for (unsigned i = 0; i < RD3_WG_HEAD_SIGS && !s; i++)
        if (!h->s[i].addr)
            s = &h->s[i];
    if (!s) {
        s = &h->s[h->next % RD3_WG_HEAD_SIGS];
        h->next = (h->next + 1u) % RD3_WG_HEAD_SIGS;
    }
    s->addr = addr;
    s->sig = sig;
}

/* the head's panel moved without a swap (a display mode change, a swap mode
 * change, another flip): its next swap presents. Returns 1 when it had
 * presented something. */
static inline int rd3_wg_head_reset(struct rd3_wg_head *h)
{
    int had;
    if (!h)
        return 0;
    had = h->presented;
    h->presented = 0;
    h->holds = 0;
    h->addr = 0;
    h->next = 0;
    for (unsigned i = 0; i < RD3_WG_HEAD_SIGS; i++) {
        h->s[i].addr = 0;
        h->s[i].sig = 0;
    }
    return had;
}
/* the VRAM at 'addr' was freed: its samples leave the head's record (the
 * head's generation stays: the panel still shows that frame) */
static inline int rd3_wg_head_forget(struct rd3_wg_head *h, unsigned long long addr)
{
    int had = 0;
    for (unsigned i = 0; h && addr && i < RD3_WG_HEAD_SIGS; i++)
        if (h->s[i].addr == addr) {
            h->s[i].addr = 0;
            h->s[i].sig = 0;
            had = 1;
        }
    if (h && addr && h->addr == addr) {
        h->addr = 0;
        had = 1;
    }
    return had;
}

/* copy forward's source: the front WindowServer named was written after the
 * frame the head presented last (headGen). Such a front is the newest frame
 * of WindowServer's chain - a burst's rotation, presented by the flip-buffer
 * path - and the source, whatever the panel shows; else the old rules stand
 * (mode 0: the buffer the head presented, modes 1/2: the front our swap
 * copied into, or the panel's buffer). Compared with the head's presented
 * generation, never with the generation the target or the panel's buffer
 * has now: a page-on stamps a buffer when WindowServer STARTS drawing into
 * it, before copy forward fills its undamaged regions (17d: page-on, then
 * copy forward ~13 ms later), so the target is always "newer" than the
 * front by then without holding a newer picture. */
static inline int rd3_wg_src_newer(unsigned long long srcGen, unsigned long long headGen)
{
    return srcGen && srcGen > headGen;
}

/* which of a flip-buffer present's two resources is the buffer it
 * presents: 1 or 2. The one that is not the head's framebuffer resource;
 * with neither (or both) the one written last, the second on a tie (as
 * submitSwapCopy's back) */
static inline int rd3_wg_flip_pick(int r1IsFb, int r2IsFb, unsigned long long gen1, unsigned long long gen2)
{
    if (r1IsFb && !r2IsFb)
        return 2;
    if (r2IsFb && !r1IsFb)
        return 1;
    return gen1 > gen2 ? 1 : 2;
}

/* the trace's 16-bit view of a generation (SWAP v2 >> 16) */
static inline unsigned rd3_wg_gen16(unsigned long long gen)
{
    return (unsigned)(gen & 0xffffu);
}

#endif
