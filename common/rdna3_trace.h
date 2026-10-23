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
#include <stddef.h>

#define RD3_TRACE_MAGIC     0x52543352u   /* "R3TR" in the file (little endian) */
#define RD3_TRACE_VERSION   2u            /* 2: the 96-byte header (wall clock, flags, its own size) */
/* events kept (80 bytes each: 2.5 MiB). A swap records about 4-6 events
 * (SWAP, PANEL, SCAN of the surface's other buffers, APERTURE, SYNC, CF):
 * two heads at 60 Hz fill it in about 25 s; autoreport.sh reads the stream
 * sysctl every few seconds. The per-second table (struct rd3_trace_sec)
 * keeps counts the ring may have lost, for the last RD3_TRACE_SECS seconds
 * (older rows are reused) with 16-bit counts that stop at 65535 */
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
    RD3_TR_FLIPBUF = 23,  /* IOAccelLegacyDisplayPipe::submitFlipBuffer / submitScanoutFlipBuffer (the family's
                           * other present path, card run 2026-10-17d's copy forward bursts) */
    RD3_TR_HOOK = 24,     /* IOAccelLegacySurface::pickPresentType / didSubmitSwap / isBackBufferReady /
                           * submitUpdate: traced and forwarded */
    RD3_TR_PRESENT = 25,  /* the present queue (DRIVER-PLAN N38, rdna3_flip_queue.h): a present queued,
                           * programmed, latched, dropped; a writer waiting for the buffer leaving the panel */
    RD3_TR_TYPES
};

/* RD3_TR_SWAP's arg: what the swap did */
enum rd3_trace_swap {
    RD3_TRS_SKIP = 0,        /* mode 0: the back read empty, nothing shown */
    RD3_TRS_FLIP = 1,        /* mode 0: the panel flipped to the picked buffer */
    RD3_TRS_FLIP_COPIED = 2, /* mode 0: the flip refused, copied onto the framebuffer's scanout */
    RD3_TRS_FLIP_FAILED = 3, /* mode 0: neither flipped nor copied */
    RD3_TRS_COPY = 4,        /* modes 1/2: the back copied into a scanout buffer of ours and flipped to */
    RD3_TRS_HOLD = 5,        /* mode 1: an empty back over a picture, not copied (the panel keeps the last frame);
                              * with RD3_TRV1_STALE: a back not redrawn since shown, held (mode 1) */
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
/* RD3_TR_SWAP's v1: modes 1/2 copied the buffer WindowServer's frame landed
 * in, not the (empty / stale) back the swap named (card run 2026-10-17c) */
#define RD3_TRV1_LANDED    (1u << 21)
/* ... found but not presented (debug.rdna3accel_swap_landed=0): the swap did
 * what arg says, v4 >> 24 names the buffer it would have presented */
#define RD3_TRV1_LANDED_WOULD (1u << 22)
#define RD3_TRV4_LANDED_A1 0xfeu   /* the landed buffer's index << 24: a1 (not in the surface's array) */
/* DRIVER-PLAN N41: VMALLOC / VMFREE flags beyond 1 (a scanout) .. 8 - the
 * parked allocation handed back (VMALLOC) / parked, not freed (VMFREE); a
 * parked allocation really released; a live one released by free() - and
 * PAGEOFF's flags: waited for WindowServer's pending GPU write, timed out */
#define RD3_TRVM_PARKED    16u
#define RD3_TRVM_PARK_GONE 32u
#define RD3_TRVM_FREE_LIVE 64u
#define RD3_TRPO_WAITED    1u
#define RD3_TRPO_TIMEOUT   2u
/* RD3_TRS_HOLD with this bit: mode 1 held a back WindowServer did not
 * redraw since the head presented it at swap v3 (no copy forward into it,
 * its samples unchanged, another buffer presented after it: the stale
 * back, rdna3_swap_copy.h, card run 2026-10-17c) */
#define RD3_TRV1_STALE     (1u << 23)
/* mode 1 would have held a stale back (the same rules) but
 * debug.rdna3accel_swap_stale=0: the swap copied it (arg is what it did) */
#define RD3_TRV1_STALE_WOULD (1u << 24)
/* the write generations (rdna3_write_gen.h, card run 2026-10-17d): held by
 * the rule (arg RD3_TRS_HOLD; v3 is then the head's generation, the low 32
 * bits); the rule would have held it but debug.rdna3accel_swap_gen=0 or
 * mode 2 (arg is what the swap did); the rule's reason (enum rd3_wg_why,
 * 0 not judged) in v1 >> RD3_TRV1_GEN_WHY_SHIFT & RD3_TRV1_GEN_WHY_MASK; a
 * flip-buffer present (debug.rdna3accel_flipbuf_present=1:
 * IOAccelLegacyDisplayPipe::submitFlipBuffer presented like a swap), not a
 * submitSwapCopy. v2 >> 16 is the back's generation (its low 16 bits) */
#define RD3_TRV1_GEN_HOLD  (1u << 25)
#define RD3_TRV1_GEN_WOULD (1u << 26)
#define RD3_TRV1_GEN_WHY_SHIFT 28u
#define RD3_TRV1_GEN_WHY_MASK  15u
#define RD3_TRV1_FLIPBUF   (1u << 27)
/* SWAP's target field (v1 >> 16 & 0xf) of a mode 0 swap (arg RD3_TRS_FLIP)
 * whose back was the buffer the head already scanned, its flip latched: not
 * flipped again, no latch wait (card run 2026-10-18b, rd3_latch_same_shown) */
#define RD3_TRV1_TARGET_SAME 4u
/* ... and 5: a mode 0 swap queued behind the head's flip in flight (the
 * present queue, DRIVER-PLAN N38: programmed at that flip's latch, or
 * replaced by a newer present; the PRESENT events tell) */
#define RD3_TRV1_TARGET_QUEUED 5u
/* a mode 1 swap whose back the generations would hold but that presented
 * the surface's buffer written after the head's frame instead
 * (rd3_wg_landed_pick) carries RD3_TRV1_LANDED (v4 >> 24 that buffer's
 * index) with the hold's reason in the why field and no RD3_TRV1_GEN_HOLD */
/* RD3_TR_FLIPBUF's v4: what the flip-buffer present did */
enum rd3_trace_flipbuf {
    RD3_TRFB_TRACED = 0,    /* only traced (debug.rdna3accel_flipbuf_present 0) */
    RD3_TRFB_PRESENTED = 1, /* presented as the swap mode does (copy into D / home, or flip) */
    RD3_TRFB_HELD = 2,      /* held by the write generations: not written since the head's last frame */
    RD3_TRFB_FAILED = 3,    /* the copy or flip failed (the SWAP event says where) */
    RD3_TRFB_NOHEAD = 4,    /* the pipe drives no framebuffer of ours */
    RD3_TRFB_NORES = 5,     /* neither resource has VRAM of ours */
    RD3_TRFB_BUSY = 6       /* the family's lock is held by another thread: not presented */
};
/* RD3_TR_PRESENT's arg (DRIVER-PLAN N38) */
enum rd3_trace_present {
    RD3_TRP_PROGRAM = 0,   /* programmed at the present (nothing was in flight) */
    RD3_TRP_QUEUED = 1,    /* queued behind the in-flight flip */
    RD3_TRP_REPLACED = 2,  /* queued, replacing an older queued present (v2 its token): that one never shown */
    RD3_TRP_JOINED = 3,    /* the in-flight flip's own address: completes with it */
    RD3_TRP_BUSY = 4,      /* the framebuffer answered Busy (another path's flip pending): kept, queued */
    RD3_TRP_REFUSED = 5,   /* the framebuffer refused it (v4 the IOReturn, v3 flipResult): at the present (v2 0)
                            * the caller copies it onto the scanout; from the queue (v2 its token) it is lost */
    RD3_TRP_LATCHED = 6,   /* the in-flight flip seen latched (v1 us from its programming): completed */
    RD3_TRP_NEXT = 7,      /* the queued present programmed at that latch */
    RD3_TRP_TIMEOUT = 8,   /* not latched in time (v1 us): completed EARLY, the panel may still read the old buffer */
    RD3_TRP_UNKNOWN = 9,   /* the flip state unreadable: completed EARLY */
    RD3_TRP_DROPPED = 10,  /* a queued present lost with no newer one (its buffer freed; flags | 0x80: a display
                            * mode change, the picture it was for gone) */
    RD3_TRP_WAIT = 11,     /* a writer waited for the buffer leaving the panel (v1 us; flags 1 copy forward,
                            * 2 page-on, 4 timed out) */
    RD3_TRP_FOREIGNTO = 12, /* the flip pending before the queued present (another path's, or one of ours given
                             * up) not latched in time (v1 us): given up, v2 the run (rd3_fq_verdict) */
    RD3_TRP_GIVEUP = 13,   /* the queued present programmed WITHOUT waiting for the pending flip (v2 the run: the
                            * head's flips stay pending - blanked, DPMS off, a stuck HUBP) */
    RD3_TRP_HOME = 14      /* the IOP copy fallback flipped home: v0 the in-flight present completed (0: none), v2
                            * the queued one dropped (0: none) */
};
/* RD3_TR_PRESENT's flags (args 0-10, 12-14): the present's kind */
enum rd3_trace_pq_kind { RD3_TRPK_SWAP = 0, RD3_TRPK_FLIPBUF = 1, RD3_TRPK_IOP = 2 };
/* where a mode 1/2 swap failed (RD3_TRS_COPY_FAILED's v4) */
enum rd3_trace_fail { RD3_TRX_NONE = 0, RD3_TRX_SRC = 1, RD3_TRX_SCANOUT = 2, RD3_TRX_SDMA = 3, RD3_TRX_FLIP = 4 };

/* One event: 80 bytes, little endian, no padding. The fields by type:
 *  SWAP    head, arg rd3_trace_swap, flags RD3_TRF_*, obj the surface; v0 the
 *          swap number, v1 = rc(a1) | rc(a2) << 4 | route << 8 | mode << 12 |
 *          target << 16 (0 none, 1 home, 2 the driver's buffer, 3 home
 *          directly, 4 mode 0: the buffer already on the panel, not
 *          flipped again: RD3_TRV1_TARGET_SAME)
 *          | 1 << 20 when modes 1/2 flipped because the SDMA breaker is
 *          open (mode then reads 0) | 1 << 21 when modes 1/2
 *          presented the landed buffer instead of the named back
 *          (RD3_TRV1_LANDED, rdna3_swap_copy.h) | 1 << 22 when it found
 *          one but debug.rdna3accel_swap_landed=0 (RD3_TRV1_LANDED_WOULD:
 *          arg is what the swap did) | 1 << 23 when mode 1 held a stale
 *          back (RD3_TRV1_STALE, arg RD3_TRS_HOLD: v3 is then the number of
 *          the swap that last presented it) | 1 << 24 when it would have
 *          held one but debug.rdna3accel_swap_stale=0 (RD3_TRV1_STALE_WOULD),
 *          v2 the copy's us (low 16 bits; the back's generation << 16), v3 the wait's us, v4 = the index
 *          of a1 in the surface's buffer array | a2's << 8 (0xff: not in
 *          it) | rd3_trace_fail << 16 | the landed buffer's index << 24
 *          (0xfe: a1 itself; its address is that swap's SCAN of the same
 *          index, or pa);
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
 *  VMALLOC arg 1 granted / 0 refused, flags 1 a scanout, 8 a swap copy's
 *          second scanout buffer D (head: its head; the stale-back records
 *          of that head are forgotten), 16 the parked allocation handed back
 *          (N41: no new VRAM, same address); obj the vidmem; v0
 *          KiB, v1 MiB handed out, v4 the IOReturn; pa its address
 *  VMFREE  head the head scanning it (0xff none), flags 1 a scanout, 2 shown
 *          by mode 0's record, 4 the flip away from it not latched, 16
 *          parked, NOT freed (N41: still allocated, the tables keep it), 32
 *          a parked allocation really released (grown, the object freed),
 *          64 a live allocation released by free() (no deallocPhysical);
 *          obj the vidmem; v0 KiB; pa its address
 *  APERTURE arg 1 addToAperture, 2 getApertureMemoryDescriptor; flags 1 ok;
 *          obj the resource; v0 KiB, v1 the IOSurface id, v2 the path
 *          (0 new, 1 the gate closed, 2 the cached descriptor, 3 no BAR
 *          VRAM, 4 IODeviceMemory::withRange failed), v3 the cached path's
 *          count (traced 4 times per resource, then every 256th); pa its address
 *  PAGEOFF arg 1 copied; obj the resource; v0 KiB, v1 us; pa its address;
 *          flags 1 waited for WindowServer's pending GPU write, 2 that wait
 *          timed out (N41), v3 the wait's us
 *  SURFGEM arg 1 imported / 0 refused; v0 the IOSurface id, v1 the status,
 *          v2 the reason, v3 w | h << 16; pa the VRAM when imported
 *  MODE    arg 0 will change, 1 did change, 2 initFramebufferResource (head,
 *          pa the scanout, v0 w | h << 16, v1 pitch)
 *  FLIP    head, arg 0 mode 0's flip of a back, 1 to the driver's buffer, 2
 *          home, 3 an IOP surface, 4 home when leaving modes 1/2, 5 a flip-buffer
 *          present, 6 the present queue's (N38); flags 1 done; v0 flipResult, v4 the
 *          IOReturn; pa the target
 *  LATCH   head, arg 0 waited, 1 at once, 2 timed out, 3 unreadable, 4 modes
 *          1/2: latched before the next copy, 5 not in time, 6 not waited
 *          for (the head's waits are off), 7 the waits go off (3 timeouts in
 *          a row), 8 they come back (a latch seen); v0 us; pa the flip
 *  SYNC    arg 0 setSyncCommand (v0 / v1 its two counts), 1
 *          getDirtyBufferBitsFromPrivateModeBits (v0 / v1 the bits); obj the surface
 *  GONE    arg 0 a surface (head its head), 1 a head's pipe resource
 *  MODESET arg 0 debug.rdna3accel_swap_mode changed, arg 1 autoreport's A/B
 *          phase changed (debug.rdna3accel_swap_ab: 0 none, 1 mode 1, 2
 *          mode 0, 3 back); v0 the old value, v1 the new; arg 2 the SDMA
 *          breaker of modes 1/2 (v0 old, v1 new: 0 closed, 1 open, 2 a
 *          probe; v2 the copy's status, v3 its trips, v4 the IOReturn)
 *  SCAN    head, arg the buffer's index in the surface's array (0xff: not
 *          in it), flags 1 pixels, 2 changed since this head last sampled
 *          it, 4 the swap's back, 8 the swap's front; obj the surface; v0
 *          the swap number; pa its address; ba its band bytes, bb the band
 *          bytes it had when last sampled; flags 16 a delayed re-sample
 *          (v1 its microseconds after the swap), 32 a copy forward's target
 *          after the copy, 64 a scanout buffer of the accelerator after the
 *          swap's copy (arg 0xfe), 128 the front after it (arg 0xfd); v3 the
 *          buffer's IOSurface id (0 none or unknown), v4 its resource type byte
 *  CS      arg the IP (AMDGPU_HW_IP_*), flags the ring, head 0xff; v0 the
 *          submitter's pid, v1 its VMID, v2 the fence seq (low 32 bits), v3
 *          IBs | BOs << 16, v4 the result (0 or -errno); pa the first IB's VA;
 *          arg 0xff: the AMDGPU_CS ioctl refused anywhere (v0 the pid, v4
 *          the error, v1 that error's count, v2 those not traced since the
 *          last: 64 per error, then every 64th)
 *  FENCE   arg the IP, flags the ring; v2 the seq, v0 the pid of its submitter
 *          when known; one per newly seen seq (an upper bound of the signal)
 *  GPUWAIT arg the IP, flags the ring; v0 the pid, v1 the wait's us, v2 the
 *          seq, v3 1 when the 10 s cap ran out (hang recovery), v4 the result
 *  RESET   arg the IP, flags the ring; v0 the result, v2 the seq waited for
 *  VMFAULT arg the hub (0 GFX, 1 MM), flags 1 write; v0 the VMID, v1 the
 *          UTCL2 client id, v2 the owning pid (0 unknown), v3 the status; pa
 *          the faulting address
 *  SURFRES obj the resource; v0 the IOSurface id, v1 the plane, v2 how
 *          (1 aperture, 2 page-off, 3 swap a1/a2: v1 1 a1, 2 a2, also with
 *          id 0, 4 import, 5 dirty); pa its VRAM, v3 its KiB, v4 the
 *          resource's type byte (+0x14)
 *  PANEL   head; flags 1 a flip pending; v0 the OTG frame, v1 the scanline,
 *          v2 in vblank; pa the address scanned (EARLIEST_INUSE), pb the one
 *          requested, both as CPU (BAR) addresses like every pa; pc the
 *          address the accelerator expects there (0 unknown); v3 why pa is
 *          missing (0 there, 1 no core, 2 the core does not know the head, 3
 *          the core's rdna3DisplayScanAddr failed, 4 no VRAM base yet, 5 the
 *          address below the framebuffer aperture: 4 and 5 carry MC addresses
 *          with flag 2), v4 the IOReturn
 *  FLIPBUF head (0xff none); arg 0 submitFlipBuffer, 1 submitScanoutFlipBuffer;
 *          flags 1 debug.rdna3accel_flipbuf_present was on, 2 the family's
 *          lock was not held (taken for the present); obj the display pipe;
 *          v0 the unsigned argument, v1 this kind's calls on the head, v2 /
 *          v3 the two resources' object names, v4 rd3_trace_flipbuf; pa / pb
 *          the two resources' VRAM, pc what the panel shows after it
 *  HOOK    head (0xff unknown); arg 0 pickPresentType, 1 didSubmitSwap, 2
 *          isBackBufferReady, 3 submitUpdate; obj the surface; v0 the first
 *          unsigned argument, v1 the second (didSubmitSwap) or the third
 *          (submitUpdate's count), v2 the base's return value (rax, the low
 *          32 bits; isBackBufferReady: its low byte), v3 this kind's calls so
 *          far (the first 2048 of a kind traced, then every 8th)
 *  PRESENT head; arg rd3_trace_present; flags rd3_trace_pq_kind (args 0-10) or the
 *          writer (arg 11: 1 copy forward, 2 page-on, | 4 timed out); v0 the
 *          present's token (the swap number; IOP: the transaction count), v1
 *          microseconds (latched / timeout: since the flip was programmed;
 *          wait: the writer's wait), v2 the replaced or dropped token, v3
 *          flipResult, v4 the IOReturn; pa the present's address, pb the
 *          address in flight after it, pc the queued one
 *  SWAP's write generations: v1 bits 25, 26, 27 and 28-31 (RD3_TRV1_GEN_*,
 *          RD3_TRV1_FLIPBUF); v2 = the copy's us (16 bits, saturated at 65535) | the
 *          back's generation << 16 (its low 16 bits); v3 the head's
 *          generation on a generation hold
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
/* RD3_TRACE_HDR_* flags */
#define RD3_TRACE_HDR_HDP_NOINV 0x1u   /* the content samples were read through the BAR with no HDP invalidate:
                                        * amdgpu_device_invalidate_hdp is a no-op on HDP 6.0 (hdp_v6_0_funcs has
                                        * only flush_hdp, soc21 no asic invalidate_hdp), so none is needed */
#define RD3_TRACE_HDR_CHUNKED   0x2u   /* copied in chunks with the lock dropped between: events overwritten
                                        * meanwhile are counted in 'lost' */
struct rd3_trace_hdr {
    uint32_t magic, version;
    uint32_t ev_size, cap;
    uint32_t count;         /* events that follow, the oldest first */
    uint32_t swap_mode;     /* debug.rdna3accel_swap_mode at the read */
    uint64_t written;       /* events recorded since the boot */
    uint64_t lost;          /* read all: overwritten before this read; stream: since the last stream read */
    uint64_t now;           /* the read's mach_absolute_time */
    uint64_t nomem;         /* events not recorded: the ring could not be allocated */
    uint32_t boot_id;       /* a per-boot random number (not derived from the object key): reads of one boot agree */
    uint32_t source;        /* RD3_TRACE_SRC_* */
    uint32_t tb_numer, tb_denom;   /* mach timebase: ns = t * numer / denom */
    uint32_t read_kind;     /* RD3_TRACE_READ_* */
    uint32_t ab;            /* autoreport's A/B phase at the read (debug.rdna3accel_swap_ab) */
    uint64_t wall_us;       /* the read's wall clock (microtime: us since 1970), taken next to 'now': Q11's
                             * anchor between mach_absolute_time and the logs' dates */
    uint32_t flags;         /* RD3_TRACE_HDR_* */
    uint32_t hdr_size;      /* sizeof(struct rd3_trace_hdr): the events start here */
};
typedef char rd3_trace_hdr_is_96_bytes[sizeof(struct rd3_trace_hdr) == 96 ? 1 : -1];
/* a read's chunk: events copied under the lock at a time (the lock is
 * dropped between chunks, ECC review round 1: a 32768-event copy held a
 * spinlock, interrupts off on the core's ring, for milliseconds) */
#define RD3_TRACE_CHUNK 256u

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

/* the first event still in the ring (written + 1 when empty) */
static inline uint64_t rd3_trace_oldest(const struct rd3_trace_ring *r)
{
    return r->written - (r->written < r->cap ? r->written : r->cap) + 1;
}

/* one chunk of a read: the events numbered *next..to still in the ring, at
 * most max of them. *next moves past those copied; those overwritten since
 * the read began are skipped and added to *lost. The caller holds the lock
 * around each call and may drop it between calls. */
static inline uint32_t rd3_trace_copy_chunk(const struct rd3_trace_ring *r, uint64_t *next, uint64_t to,
                                            struct rd3_trace_ev *out, uint32_t max, uint64_t *lost)
{
    uint64_t oldest;
    uint32_t n = 0;
    if (!r || !r->ev || !r->cap || !out || !*next)
        return 0;
    if (to > r->written)
        to = r->written;
    oldest = rd3_trace_oldest(r);
    if (*next < oldest) {
        const uint64_t stop = to + 1 < oldest ? to + 1 : oldest;
        if (stop > *next)
            *lost += stop - *next;
        *next = stop > *next ? stop : *next;
    }
    while (*next <= to && n < max) {
        out[n++] = r->ev[(*next - 1) % r->cap];
        (*next)++;
    }
    return n;
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
    uint64_t next = from ? from : 1, lost = 0;
    if (!r || !r->ev || !out)
        return 0;
    /* from the oldest kept, never a loop over every number since the boot */
    return rd3_trace_copy_chunk(r, &next, r->written, out, max, &lost);
}

/* little-endian 64-bit value of 8 bytes */
static inline uint64_t rd3_trace_le64(const uint8_t *b)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)b[i] << (8 * i);
    return v;
}

/* SipHash-2-4 (Aumasson, Bernstein 2012; the reference siphash.c), the
 * 64-bit output: a keyed PRF, so without the key a name says nothing about
 * the pointer behind it (ECC review round 2: the salted multiply-xorshift
 * of round 1 could be inverted, giving the kernel's slide away) */
#define RD3_SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define RD3_SIP_ROUND(v0, v1, v2, v3) do { \
        v0 += v1; v1 = RD3_SIP_ROTL(v1, 13); v1 ^= v0; v0 = RD3_SIP_ROTL(v0, 32); \
        v2 += v3; v3 = RD3_SIP_ROTL(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = RD3_SIP_ROTL(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = RD3_SIP_ROTL(v1, 17); v1 ^= v2; v2 = RD3_SIP_ROTL(v2, 32); } while (0)
static inline uint64_t rd3_siphash24(const uint8_t key[16], const uint8_t *m, size_t len)
{
    const uint64_t k0 = rd3_trace_le64(key), k1 = rd3_trace_le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    uint64_t b = (uint64_t)len << 56, mi;
    size_t i = 0, j;
    for (; i + 8 <= len; i += 8) {
        mi = rd3_trace_le64(m + i);
        v3 ^= mi;
        RD3_SIP_ROUND(v0, v1, v2, v3);
        RD3_SIP_ROUND(v0, v1, v2, v3);
        v0 ^= mi;
    }
    for (j = 0; i + j < len; j++)
        b |= (uint64_t)m[i + j] << (8 * j);
    v3 ^= b;
    RD3_SIP_ROUND(v0, v1, v2, v3);
    RD3_SIP_ROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    for (j = 0; j < 4; j++)
        RD3_SIP_ROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

/* a pointer as a 32-bit name: SipHash-2-4 under a per-boot random key that
 * never leaves the kernel (no kernel address in the report, and none to be
 * recovered from it), never 0 for a pointer */
static inline uint32_t rd3_trace_obj(uint64_t p, const uint8_t key[16])
{
    uint8_t m[8];
    uint64_t x;
    unsigned i;
    if (!p)
        return 0;
    for (i = 0; i < 8; i++)
        m[i] = (uint8_t)(p >> (8 * i));
    x = rd3_siphash24(key, m, 8);
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

/* whether a swap samples the surface's other buffers (SCAN events, Q2):
 * 64 uncached BAR reads per buffer, so bounded (ECC review round 1).
 * debug.rdna3accel_trace_scan: 0 only when the back read empty ('landing'),
 * 2 every swap, 1 (the default) every swap for a head's first
 * RD3_TRACE_SCAN_FIRST swaps and while autoreport's A/B runs (ab != 0),
 * otherwise every RD3_TRACE_SCAN_EVERY-th swap; an empty back always */
#define RD3_TRACE_SCAN_FIRST 600u
#define RD3_TRACE_SCAN_EVERY 8u
static inline int rd3_trace_scan_due(int setting, int landing, uint64_t headSwaps, unsigned ab)
{
    if (landing || setting >= 2)
        return 1;
    if (setting <= 0)
        return 0;
    return headSwaps < RD3_TRACE_SCAN_FIRST || ab != 0 || headSwaps % RD3_TRACE_SCAN_EVERY == 0;
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

#endif
