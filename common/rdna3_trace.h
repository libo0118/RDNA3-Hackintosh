/*
 * rdna3_trace.h - the accelerator's per-frame trace (card run 2026-10-17b).
 * Plain C, no kernel headers: the accelerator records into it,
 * tests/test_trace.c checks it on the host, tools/rdna3_trace.py decodes
 * what the report carries (keep the three in step: RD3_TRACE_VERSION).
 *
 * About 12 card runs of "everything flickers" were each read from counters
 * and capped log lines, so every run answered one guess. The trace keeps
 * every legacy swap, copy forward, surface shape, VRAM allocation and free,
 * aperture (page-on) access, page-off, IOSurface import answer, mode change
 * and flip of the last RD3_TRACE_CAP events in a fixed ring, with the VRAM
 * address and a cheap content signature of each buffer, so one report shows
 * frame by frame which buffer WindowServer drew, which one the panel got and
 * what happened to it in between.
 *
 * Read with `sysctl -b debug.rdna3accel_trace` (root only): one
 * struct rd3_trace_hdr, then hdr.count events, the oldest first. The kernel
 * log gets the first RD3_TRACE_BOOT_DUMP events of a boot and, on request
 * (sysctl debug.rdna3accel_trace_dump=<n>), the newest n, as "tr" lines
 * (RD3_TRACE_LINE_FMT), so a serial log alone has them too.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_TRACE_H
#define RDNA3_TRACE_H

#include <stdint.h>

#define RD3_TRACE_MAGIC     0x52543352u   /* "R3TR" in the file (little endian) */
#define RD3_TRACE_VERSION   1u
/* events kept (80 bytes each: 2.5 MiB). A swap records about 4-6 events
 * (SWAP, PANEL, SCAN of the surface's other buffers, APERTURE, SYNC, CF):
 * two heads at 60 Hz fill it in about 25 s; autoreport.sh reads the stream
 * sysctl every few seconds, and the per-second table (struct
 * rd3_trace_sec) never overflows */
#define RD3_TRACE_CAP       32768u
#define RD3_TRACE_BOOT_DUMP 300u          /* events of a boot logged once */
#define RD3_TRACE_DUMP_MAX  2000u         /* the most one dump request logs */

enum rd3_trace_type {
    RD3_TR_NONE = 0,
    RD3_TR_SWAP = 1,      /* a legacy surface swap (submitSwapCopy) */
    RD3_TR_CF = 2,        /* copy forward (submitCopyForward) */
    RD3_TR_SHAPE = 3,     /* shapeSurface */
    RD3_TR_VMALLOC = 4,   /* RD3VidMemory::allocPhysical */
    RD3_TR_VMFREE = 5,    /* RD3VidMemory::deallocPhysical */
    RD3_TR_APERTURE = 6,  /* addToAperture / getApertureMemoryDescriptor: the family's CPU access (page-on) */
    RD3_TR_PAGEOFF = 7,   /* RD3Resource::pageoff (VRAM -> the IOSurface's system memory) */
    RD3_TR_SURFGEM = 8,   /* an IOSurface import answered (A3) */
    RD3_TR_MODE = 9,      /* display mode will / did change, initFramebufferResource */
    RD3_TR_FLIP = 10,     /* a flip asked of the framebuffer */
    RD3_TR_LATCH = 11,    /* a flip seen latched, or not in time */
    RD3_TR_SYNC = 12,     /* setSyncCommand / getDirtyBufferBitsFromPrivateModeBits */
    RD3_TR_GONE = 13,     /* a swapping surface or a head's pipe resource freed */
    RD3_TR_MODESET = 14,  /* debug.rdna3accel_swap_mode or the A/B phase changed */
    RD3_TR_SCAN = 15,     /* one buffer of a swapping surface sampled (where did WindowServer's frame land?) */
    /* the core kext's ring (RDNA3Accel: RADV's command submissions) */
    RD3_TR_CS = 16,       /* a command submission (DRM_AMDGPU_CS) */
    RD3_TR_FENCE = 17,    /* a fence first seen signalled */
    RD3_TR_GPUWAIT = 18,  /* a fence wait (DRM_AMDGPU_WAIT_CS / syncobj) ended */
    RD3_TR_RESET = 19,    /* a hang recovery / GPU reset */
    RD3_TR_VMFAULT = 20,  /* a GPU page fault */
    /* the accelerator's again */
    RD3_TR_SURFRES = 21,  /* an IOSurface id seen with a resource and its VRAM (joins plugin traces to buffers) */
    RD3_TR_PANEL = 22,    /* what a head's HUBP scanned at a swap (SURFACE_EARLIEST_INUSE) */
    RD3_TR_TYPES
};

/* RD3_TR_SWAP's arg: what the swap did */
enum rd3_trace_swap {
    RD3_TRS_SKIP = 0,        /* mode 0: the back read empty, nothing shown */
    RD3_TRS_FLIP = 1,        /* mode 0: the panel flipped to the picked buffer */
    RD3_TRS_FLIP_COPIED = 2, /* mode 0: the flip refused, copied onto the framebuffer's scanout */
    RD3_TRS_FLIP_FAILED = 3, /* mode 0: neither flipped nor copied */
    RD3_TRS_COPY = 4,        /* modes 1/2: the back copied into a scanout buffer of ours and flipped to */
    RD3_TRS_HOLD = 5,        /* mode 1: an empty back over a picture, not copied (the panel keeps the last frame) */
    RD3_TRS_COPY_FAILED = 6, /* modes 1/2: nothing reached the panel (v4: the step, rd3_trace_fail) */
    RD3_TRS_NOHEAD = 7       /* matched no head */
};
/* RD3_TR_SWAP's flags */
#define RD3_TRF_A1_FRONT   0x01u   /* a1 is a head's framebuffer resource */
#define RD3_TRF_A2_FRONT   0x02u
#define RD3_TRF_A1_PIXELS  0x04u   /* a1's 64 samples were not all zero */
#define RD3_TRF_A2_PIXELS  0x08u
#define RD3_TRF_BACK_EMPTY 0x10u   /* the buffer shown / copied read empty */
#define RD3_TRF_FRONT_OK   0x20u   /* modes 1/2: the back was copied into the front too */
#define RD3_TRF_DOUBLE     0x40u   /* modes 1/2: copied into the buffer NOT on the panel, then flipped (tear-free) */
#define RD3_TRF_WAITED     0x80u   /* a flip was waited for (mode 0: after it; modes 1/2: before the copy) */
/* where a mode 1/2 swap failed (RD3_TRS_COPY_FAILED's v4) */
enum rd3_trace_fail { RD3_TRX_NONE = 0, RD3_TRX_SRC = 1, RD3_TRX_SCANOUT = 2, RD3_TRX_SDMA = 3, RD3_TRX_FLIP = 4 };

/* One event: 80 bytes, little endian, no padding. The fields by type:
 *  SWAP    head, arg rd3_trace_swap, flags RD3_TRF_*, obj the surface; v0 the
 *          swap number, v1 = rc(a1) | rc(a2) << 4 | route << 8 | mode << 12 |
 *          target << 16 (0 none, 1 home, 2 the driver's buffer, 3 home
 *          directly), v2 the copy's us, v3 the wait's us, v4 = the index
 *          of a1 in the surface's buffer array | a2's << 8 (0xff: not in
 *          it) | rd3_trace_fail << 16;
 *          pa a1, pb a2, pc what the panel shows after it; ba / bb the band
 *          bytes of a1 / a2 at the swap's entry
 *  CF      head, arg the source's pick (mode 0: rd3_cf_pick; modes 1/2: 16 +
 *          rd3_sc_cf), flags 1 modes 1/2, 2 the front is a framebuffer
 *          resource, 4 rects missed; obj the surface; v0 rects, v1 rects
 *          copied, v2 KiB, v3 us, v4 the IOReturn answered; pa the front WindowServer
 *          named, pb the target, pc the source used; ba the bounding box of the
 *          rects (4 x u16: x0 y0 x1 y1), bb[0] pending rects taken, bb[1] failed
 *  SHAPE   obj the surface; v0 the buffer mask, v1 w | h << 16, v2 the
 *          private mode bits, v3 present | shaped << 8; pa..pc buffers 0..2
 *  VMALLOC arg 1 granted / 0 refused, flags 1 a scanout; obj the vidmem; v0
 *          KiB, v1 MiB handed out, v4 the IOReturn; pa its address
 *  VMFREE  head the head scanning it (0xff none), flags 1 a scanout, 2 shown
 *          by mode 0's record, 4 the flip away from it not latched; obj the
 *          vidmem; v0 KiB; pa its address
 *  APERTURE arg 1 addToAperture, 2 getApertureMemoryDescriptor; flags 1 ok;
 *          obj the resource; v0 KiB; pa its address
 *  PAGEOFF arg 1 copied; obj the resource; v0 KiB, v1 us; pa its address
 *  SURFGEM arg 1 imported / 0 refused; v0 the IOSurface id, v1 the status,
 *          v2 the reason, v3 w | h << 16; pa the VRAM when imported
 *  MODE    arg 0 will change, 1 did change, 2 initFramebufferResource (head,
 *          pa the scanout, v0 w | h << 16, v1 pitch)
 *  FLIP    head, arg 0 mode 0's flip of a back, 1 to the driver's buffer, 2
 *          home, 3 an IOP surface; flags 1 done; v0 flipResult, v4 the
 *          IOReturn; pa the target
 *  LATCH   head, arg 0 waited, 1 at once, 2 timed out, 3 unreadable, 4 modes
 *          1/2: latched before the next copy, 5 not in time; v0 us; pa the flip
 *  SYNC    arg 0 setSyncCommand (v0 / v1 its two counts), 1
 *          getDirtyBufferBitsFromPrivateModeBits (v0 / v1 the bits); obj the surface
 *  GONE    arg 0 a surface (head its head), 1 a head's pipe resource
 *  MODESET arg 0 debug.rdna3accel_swap_mode changed, arg 1 autoreport's A/B
 *          phase changed (debug.rdna3accel_swap_ab: 0 none, 1 mode 1, 2
 *          mode 0, 3 back); v0 the old value, v1 the new
 *  SCAN    head, arg the buffer's index in the surface's array (0xff: not
 *          in it), flags 1 pixels, 2 changed since this head last sampled
 *          it, 4 the swap's back, 8 the swap's front; obj the surface; v0
 *          the swap number; pa its address; ba its band bytes, bb the band
 *          bytes it had when last sampled; flags 16 a delayed re-sample
 *          (v1 its microseconds after the swap), 32 a copy forward's target
 *          after the copy, 64 a scanout buffer of the accelerator after the
 *          swap's copy (arg 0xfe), 128 the front after it (arg 0xfd)
 *  CS      arg the IP (AMDGPU_HW_IP_*), flags the ring, head 0xff; v0 the
 *          submitter's pid, v1 its VMID, v2 the fence seq (low 32 bits), v3
 *          IBs | BOs << 16, v4 the result (0 or -errno); pa the first IB's VA
 *  FENCE   arg the IP, flags the ring; v2 the seq, v0 the pid of its submitter
 *          when known; one per newly seen seq (an upper bound of the signal)
 *  GPUWAIT arg the IP, flags the ring; v0 the pid, v1 the wait's us, v2 the
 *          seq, v3 1 when the 10 s cap ran out (hang recovery), v4 the result
 *  RESET   arg the IP, flags the ring; v0 the result, v2 the seq waited for
 *  VMFAULT arg the hub (0 GFX, 1 MM), flags 1 write; v0 the VMID, v1 the
 *          UTCL2 client id, v2 the owning pid (0 unknown), v3 the status; pa
 *          the faulting address
 *  SURFRES obj the resource; v0 the IOSurface id, v1 the plane, v2 how
 *          (1 aperture, 2 page-off, 3 swap a1/a2, 4 import, 5 dirty); pa its
 *          VRAM, v3 its KiB
 *  PANEL   head; flags 1 a flip pending; v0 the OTG frame, v1 the scanline,
 *          v2 in vblank; pa the address scanned (EARLIEST_INUSE), pb the one
 *          requested, both as CPU (BAR) addresses like every pa; pc the
 *          address the accelerator expects there (0 unknown)
 *  Time: t is mach_absolute_time (the Metal plugin's traces use it too),
 *  the header carries the timebase. */
struct rd3_trace_ev {
    uint64_t t;             /* mach_absolute_time */
    uint32_t seq;           /* the event's number, from 1 */
    uint8_t  type, head, arg, flags;
    uint32_t obj;           /* rd3_trace_obj of the surface / resource / vidmem, 0 none */
    uint32_t v0, v1, v2, v3, v4;
    uint64_t pa, pb, pc;    /* VRAM addresses */
    uint8_t  ba[8], bb[8];
};
typedef char rd3_trace_ev_is_80_bytes[sizeof(struct rd3_trace_ev) == 80 ? 1 : -1];

/* what `sysctl -b debug.rdna3accel_trace` (and the stream, and the core's
 * debug.rdna3_drmtrace) starts with; a stream read is one header and the
 * events since the last stream read */
#define RD3_TRACE_SRC_ACCEL 0u   /* the accelerator (RDNA3Accelerator) */
#define RD3_TRACE_SRC_CORE  1u   /* the core (RDNA3Accel): command submissions, fences, faults */
#define RD3_TRACE_READ_ALL    0u
#define RD3_TRACE_READ_STREAM 1u
struct rd3_trace_hdr {
    uint32_t magic, version;
    uint32_t ev_size, cap;
    uint32_t count;         /* events that follow, the oldest first */
    uint32_t swap_mode;     /* debug.rdna3accel_swap_mode at the read */
    uint64_t written;       /* events recorded since the boot */
    uint64_t lost;          /* read all: overwritten before this read; stream: since the last stream read */
    uint64_t now;           /* the read's mach_absolute_time */
    uint64_t nomem;         /* events not recorded: the ring could not be allocated */
    uint32_t salt_check;    /* rd3_trace_obj(0x1000, salt): two reads of one boot agree */
    uint32_t source;        /* RD3_TRACE_SRC_* */
    uint32_t tb_numer, tb_denom;   /* mach timebase: ns = t * numer / denom */
    uint32_t read_kind;     /* RD3_TRACE_READ_* */
    uint32_t ab;            /* autoreport's A/B phase at the read (debug.rdna3accel_swap_ab) */
};
typedef char rd3_trace_hdr_is_80_bytes[sizeof(struct rd3_trace_hdr) == 80 ? 1 : -1];

/* The per-second table: one row per second of uptime and head (head 4: the
 * events no head owns), RD3_TRACE_SECS seconds kept, so a whole run's
 * shape survives however many events the ring lost */
#define RD3_TRACE_SECS 4096u
#define RD3_TRACE_SEC_ROWS 5u
struct rd3_trace_sec {
    uint32_t sec;           /* uptime second + 1 (0: an empty row) */
    uint8_t  head, mode, ab, pad;
    uint16_t swaps;         /* legacy swaps routed to the head */
    uint16_t shown;         /* that put a frame on the panel (flip or copy) */
    uint16_t held;          /* skipped (mode 0) or held (mode 1): the panel kept its frame */
    uint16_t empty;         /* whose back read empty */
    uint16_t black;         /* empty backs that reached the panel */
    uint16_t cf;            /* copy forwards */
    uint16_t cferr;         /* copy forwards answered with an error */
    uint16_t aperture;      /* the family's aperture accesses (page-on) */
    uint16_t vmrefused;     /* VRAM allocations refused */
    uint16_t freedshown;    /* buffers freed while scanned */
    uint32_t events;        /* trace events recorded this second */
};
typedef char rd3_trace_sec_is_32_bytes[sizeof(struct rd3_trace_sec) == 32 ? 1 : -1];
/* the row of (second, head) */
static inline uint32_t rd3_trace_sec_row(uint64_t sec, unsigned head)
{
    return (uint32_t)((sec % RD3_TRACE_SECS) * RD3_TRACE_SEC_ROWS + (head < RD3_TRACE_SEC_ROWS ? head : 4u));
}
/* the row for (sec, head), emptied when it held an older second */
static inline struct rd3_trace_sec *rd3_trace_sec_get(struct rd3_trace_sec *tab, uint64_t sec, unsigned head)
{
    struct rd3_trace_sec *r;
    unsigned i;
    if (!tab)
        return 0;
    r = &tab[rd3_trace_sec_row(sec, head)];
    if (r->sec != (uint32_t)(sec + 1)) {
        for (i = 0; i < sizeof *r; i++)
            ((volatile unsigned char *)r)[i] = 0;
        r->sec = (uint32_t)(sec + 1);
        r->head = (uint8_t)(head < RD3_TRACE_SEC_ROWS ? head : 4u);
    }
    return r;
}
static inline void rd3_trace_inc16(uint16_t *v)
{
    if (*v != 0xffffu)
        (*v)++;
}

/* the ring; the caller holds its lock around every call */
struct rd3_trace_ring {
    struct rd3_trace_ev *ev;
    uint32_t cap;
    uint64_t written;
};

/* the next slot (overwriting the oldest once full), its seq set, the rest
 * zeroed; NULL without storage */
static inline struct rd3_trace_ev *rd3_trace_next(struct rd3_trace_ring *r)
{
    struct rd3_trace_ev *e;
    unsigned i;
    if (!r || !r->ev || !r->cap)
        return 0;
    e = &r->ev[r->written % r->cap];
    for (i = 0; i < sizeof *e; i++)
        ((volatile unsigned char *)e)[i] = 0;
    r->written++;
    e->seq = (uint32_t)r->written;
    return e;
}

/* the stream: the events after 'cursor' still in the ring (from = the
 * first event number to read) and how many after it were overwritten */
static inline uint64_t rd3_trace_stream_from(const struct rd3_trace_ring *r, uint64_t cursor, uint64_t *lost)
{
    const uint64_t oldest = r->written - (r->written < r->cap ? r->written : r->cap) + 1;
    *lost = cursor + 1 < oldest ? oldest - (cursor + 1) : 0;
    return cursor + 1 < oldest ? oldest : cursor + 1;
}

/* the events numbered from 'from' on still in the ring, at most 'max', the
 * oldest first, into 'out'; returns how many (the caller holds the lock) */
static inline uint32_t rd3_trace_copy(const struct rd3_trace_ring *r, uint64_t from, struct rd3_trace_ev *out,
                                      uint32_t max);

/* events in the ring, and those overwritten */
static inline uint32_t rd3_trace_count(const struct rd3_trace_ring *r)
{
    return !r || !r->cap ? 0u : r->written < r->cap ? (uint32_t)r->written : r->cap;
}
static inline uint64_t rd3_trace_lost(const struct rd3_trace_ring *r)
{
    return !r || r->written <= r->cap ? 0u : r->written - r->cap;
}

/* the slot of the i-th oldest event in the ring (i < rd3_trace_count) */
static inline uint32_t rd3_trace_index(const struct rd3_trace_ring *r, uint32_t i)
{
    const uint64_t first = r->written - rd3_trace_count(r);
    return (uint32_t)((first + i) % r->cap);
}

/* the slot holding event number 'seq', or -1 (overwritten, not yet written) */
static inline long rd3_trace_find(const struct rd3_trace_ring *r, uint64_t seq)
{
    if (!r || !r->cap || !seq || seq > r->written || seq + r->cap <= r->written)
        return -1;
    return (long)((seq - 1) % r->cap);
}

static inline uint32_t rd3_trace_copy(const struct rd3_trace_ring *r, uint64_t from, struct rd3_trace_ev *out,
                                      uint32_t max)
{
    uint32_t n = 0;
    uint64_t seq;
    if (!r || !r->ev || !out)
        return 0;
    for (seq = from ? from : 1; seq <= r->written && n < max; seq++) {
        const long i = rd3_trace_find(r, seq);
        if (i >= 0)
            out[n++] = r->ev[i];
    }
    return n;
}

/* a pointer as a 32-bit name: salted per boot (no kernel address in the
 * report), never 0 for a pointer */
static inline uint32_t rd3_trace_obj(uint64_t p, uint32_t salt)
{
    uint64_t x;
    if (!p)
        return 0;
    x = (p ^ ((uint64_t)salt << 17) ^ salt) * 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 32;
    return (uint32_t)x ? (uint32_t)x : 1u;
}

/* one band byte from a row of samples (24-bit colours): FNV-1a folded; a
 * band of zeros is 0, anything else never 0 */
static inline uint8_t rd3_trace_band(const uint32_t *px, unsigned n)
{
    uint32_t h = 2166136261u;
    unsigned i, any = 0;
    uint8_t b;
    for (i = 0; i < n; i++) {
        const uint32_t v = px[i] & 0xffffffu;
        any |= v != 0;
        h = (h ^ v) * 16777619u;
    }
    if (!any)
        return 0;
    b = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    return b ? b : 1u;
}

/* little-endian 16-bit field i of an 8-byte array */
static inline void rd3_trace_put16(uint8_t *b, unsigned i, unsigned v)
{
    b[2 * i] = (uint8_t)(v & 0xffu);
    b[2 * i + 1] = (uint8_t)((v >> 8) & 0xffu);
}

/* the rects' bounding box (n: rects in it so far; a rect of no size is
 * left out) */
struct rd3_trace_box { int x0, y0, x1, y1, n; };
static inline void rd3_trace_box_add(struct rd3_trace_box *b, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (!b->n || x < b->x0) b->x0 = x;
    if (!b->n || y < b->y0) b->y0 = y;
    if (!b->n || x + w > b->x1) b->x1 = x + w;
    if (!b->n || y + h > b->y1) b->y1 = y + h;
    b->n++;
}
static inline unsigned rd3_trace_u16(int v)
{
    return v < 0 ? 0u : v > 0xffff ? 0xffffu : (unsigned)v;
}

/* one event as a kernel log line ("tr" after the kext's prefix): source seq
 * t type head arg flags obj v0 v1 v2 v3 v4 pa pb pc ba bb, the numbers
 * after t in hex, ba / bb as little-endian 64-bit words
 * (tools/rdna3_trace.py) */
#define RD3_TRACE_LINE_FMT "tr %u %u %llu %u %u %u %x %x %x %x %x %x %x %llx %llx %llx %llx %llx"
static inline uint64_t rd3_trace_le64(const uint8_t *b)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)b[i] << (8 * i);
    return v;
}

#endif
