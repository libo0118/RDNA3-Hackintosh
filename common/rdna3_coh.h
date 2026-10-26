/*
 * rdna3_coh.h - DRIVER-PLAN N54: the kernel's GPU-memory coherence switches
 * and the decisions behind them. Plain C99, no kernel headers: the core kext
 * (RDNA3Bringup) and hw/ use it, tests/test_coh.c checks it on the host.
 *
 * Card runs 2026-10-25e..26b: Terminal's incrementally drawn text and Chess's
 * pieces stay broken under every plugin-side switch (PARANOID included), and
 * the plugin's completion witness saw a fence return before the GPU's last
 * write was visible to the CPU (2026-10-25f: WindowServer 5 of 91994, worst
 * 867 us). Each switch below turns one kernel-side ordering or cache rule up
 * to the strongest form, at runtime (sysctl debug.rdna3_coh_<name>), so one
 * card run can tell which one, if any, cures the picture. Default: the
 * behaviour before N54 (0), except where amdgpu does it (those are not
 * switches, see docs/DRIVER-PLAN.md N54).
 *
 * The amdgpu functions each switch strengthens:
 *   fence_sys  amdgpu_fence_driver_start_ring: the ring's fence lives in
 *              adev->wb, a GTT (system memory) BO; ours lived in a VRAM wb
 *              slot read through the BAR. The fence is now ALSO written to
 *              a system page (RELEASE_MEM, the same GCR as
 *              gfx_v11_0_ring_emit_fence, before the VRAM one); this switch
 *              makes that copy the CPU's answer, as in amdgpu. Measured
 *              either way: a VRAM fence seen before the system one is the
 *              ordering window the witness hit (skew counters).
 *   fence_inv  gfx_v11_0_ring_emit_fence writes GL2 back (GL2_WB, GLM_WB/INV);
 *              this adds GL2_INV, GL1_INV and GLV_INV: no line of the job
 *              survives in any GPU cache.
 *   pipe_sync  gfx_v11_0_ring_emit_pipeline_sync (amdgpu emits it for a
 *              job with an explicit / same-ring implicit dependency or a VM
 *              flush): here at every job, so a job never overlaps the one
 *              before it (two processes sharing a surface have no implicit
 *              sync in this driver).
 *   wait_idle  the CS ioctl returns only after its own fence passed (CPU
 *              wait, bounded): no two jobs of anybody in flight at once.
 *   uc         gmc_v11_0_get_vm_pte's MTYPE_UC override (amdgpu: COHERENT /
 *              UNCACHED BOs) for every new user mapping: the GL2 never holds
 *              a user line. Takes effect for mappings made after the switch:
 *              restart the app.
 *   hdp_end    amdgpu_device_flush_hdp before the IBs (amdgpu_ib_schedule);
 *              this adds one after them (HDP 6.0 has no read cache to
 *              invalidate: amdgpu_device_invalidate_hdp is empty for it).
 *   vm_flush   amdgpu_vm_flush when vm_needs_flush; here at every job.
 *   sdma_gcr   sdma_v6_0_ring_emit_mem_sync (GCR_REQ: GL2 wb+inv) around
 *              every kernel SDMA copy / fill (page-off, copy forward,
 *              clears), and the HDP flush before it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_COH_H
#define RDNA3_COH_H

#include <stdint.h>

#define RD3_COH_FENCE_SYS  (1u << 0)
#define RD3_COH_FENCE_INV  (1u << 1)
#define RD3_COH_PIPE_SYNC  (1u << 2)
#define RD3_COH_WAIT_IDLE  (1u << 3)
#define RD3_COH_UC         (1u << 4)
#define RD3_COH_HDP_END    (1u << 5)
#define RD3_COH_VM_FLUSH   (1u << 6)
#define RD3_COH_SDMA_GCR   (1u << 7)
#define RD3_COH_NBITS      8u
#define RD3_COH_ALL        ((1u << RD3_COH_NBITS) - 1u)
#define RD3_COH_DEFAULT    0u

/* ---- the work each switch did (ECC round 1 of N54: a rung counts as
 * engaged only from these, never from the jobs that merely ran while the
 * bit was set). Index = the switch's bit: fence_sys the CPU answers taken
 * from the system copy, fence_inv the fences emitted with the
 * invalidations, pipe_sync the pipeline syncs emitted, wait_idle the CS
 * ioctls that really waited, uc the mappings made UC by the switch, hdp_end
 * the HDP flushes emitted after IBs, vm_flush the forced VM flushes of
 * jobs that were submitted, sdma_gcr the kernel copies wrapped in GCR. Then
 * the kernel's own waits (gfx11_wait_fence, sdma60_wait_fence_us) the
 * system copy held back with fence_sys, the waits that gave up on the copy
 * and took the VRAM fence, and the forced fence completions (resets). The
 * OS layer owns the array (rdna3_dev.coh_count, NULL = not counted). */
#define RD3_COH_CNT_KWAIT_SYS      (RD3_COH_NBITS + 0u)
#define RD3_COH_CNT_KWAIT_FALLBACK (RD3_COH_NBITS + 1u)
#define RD3_COH_CNT_FORCED         (RD3_COH_NBITS + 2u)
#define RD3_COH_NCNT               (RD3_COH_NBITS + 3u)

static inline void rd3_coh_count(uint64_t *c, uint32_t i)
{
    if (c && i < RD3_COH_NCNT)
        c[i]++;
}

/* the sysctl leaf of each bit: debug.rdna3_coh_<name> */
static inline const char *rd3_coh_name(uint32_t bit)
{
    static const char *const n[RD3_COH_NBITS] = { "fence_sys", "fence_inv", "pipe_sync", "wait_idle",
                                                  "uc", "hdp_end", "vm_flush", "sdma_gcr" };
    return bit < RD3_COH_NBITS ? n[bit] : "?";
}

/* "fence_sys,pipe_sync" (or "none") into buf; returns buf */
static inline char *rd3_coh_flags_str(uint32_t flags, char *buf, uint32_t cap)
{
    uint32_t i, o = 0;
    if (!cap)
        return buf;
    buf[0] = 0;
    for (i = 0; i < RD3_COH_NBITS; i++) {
        const char *s = rd3_coh_name(i);
        if (!(flags & (1u << i)))
            continue;
        if (o && o + 1 < cap)
            buf[o++] = ',';
        while (*s && o + 1 < cap)
            buf[o++] = *s++;
    }
    if (!o) {
        const char *s = "none";
        while (*s && o + 1 < cap)
            buf[o++] = *s++;
    }
    buf[o] = 0;
    return buf;
}

/* ---- the system-memory fence copy (fence_sys) ----
 *
 * The ring writes its fence twice: first the system page (no interrupt),
 * then the VRAM slot (the EOP interrupt, as before). A VRAM value at or past
 * the seq while the system one is not means the VRAM write became visible to
 * the CPU first: exactly the window in which a CPU that trusts the VRAM fence
 * reads system memory the GPU wrote before it (the witness's "fence early").
 * Each seq of a ring opens at most one such skew; it closes when the system
 * value gets there (its length is measured) and is declared STALE after
 * RD3_COH_SYS_STALE_NS: the system copy then no longer answers (the page or
 * its GART mapping is broken; the VRAM fence answers as before N54). */
#define RD3_COH_SYS_STALE_NS (50ull * 1000 * 1000)

typedef struct rd3_coh_ring_state {
    uint32_t skew_seq;      /* the seq whose skew is open */
    int      skew_open;
    uint64_t skew_t0;       /* when it opened */
    int      stale;         /* the system copy stopped answering */
} rd3_coh_ring_state;

typedef struct rd3_coh_fence_out {
    int      passed;        /* the answer */
    int      from_sys;      /* the answer came from the system copy */
    int      skew_new;      /* a skew opened with this check (count it) */
    int      skew_closed;   /* a skew closed with this check ... */
    uint64_t skew_ns;       /* ... after this long */
    int      went_stale;    /* the system copy just went stale (log it once) */
} rd3_coh_fence_out;

static inline int rd3_coh_seq_passed(uint32_t value, uint32_t seq)
{
    return (int32_t)(value - seq) >= 0;
}

/* one check of 'seq' on a ring: the VRAM wb value, and the system copy's
 * when the ring has one (have_sys). 'flags' the switches; 'now_ns' a
 * monotonic clock. */
static inline void rd3_coh_fence_check(rd3_coh_ring_state *s, uint32_t flags, int have_sys, uint32_t seq,
                                       uint32_t vram_value, uint32_t sys_value, uint64_t now_ns,
                                       rd3_coh_fence_out *o)
{
    const int vram = rd3_coh_seq_passed(vram_value, seq);
    const int sys = have_sys && rd3_coh_seq_passed(sys_value, seq);
    o->passed = vram;
    o->from_sys = 0;
    o->skew_new = o->skew_closed = o->went_stale = 0;
    o->skew_ns = 0;
    if (!have_sys)
        return;
    if (s->skew_open && (rd3_coh_seq_passed(sys_value, s->skew_seq) ||
                         !rd3_coh_seq_passed(vram_value, s->skew_seq))) {
        /* the system copy caught up (or the ring was reset under the skew) */
        o->skew_closed = rd3_coh_seq_passed(sys_value, s->skew_seq);
        o->skew_ns = now_ns > s->skew_t0 ? now_ns - s->skew_t0 : 0;
        s->skew_open = 0;
    }
    if (vram && !sys) {
        if (!s->skew_open) {
            o->skew_new = 1;
            s->skew_open = 1;
            s->skew_seq = seq;
            s->skew_t0 = now_ns;
        } else if (s->skew_seq != seq && rd3_coh_seq_passed(seq, s->skew_seq)) {
            /* a later seq while the system copy is still behind the earlier
             * one: the same skew goes on (its clock is not restarted, so a
             * copy that never lands goes stale however many jobs follow) */
            s->skew_seq = seq;
        }
        if (!s->stale && now_ns >= s->skew_t0 && now_ns - s->skew_t0 >= RD3_COH_SYS_STALE_NS) {
            s->stale = 1;
            o->went_stale = 1;
        }
    }
    if ((flags & RD3_COH_FENCE_SYS) && !s->stale) {
        o->passed = sys;
        o->from_sys = 1;
    }
}

/* ---- the MTYPE of a user mapping ----
 * gmc_v11_0_get_vm_pte: a BO created AMDGPU_GEM_CREATE_COHERENT,
 * _EXT_COHERENT or _UNCACHED is mapped MTYPE_UC whatever the VA flags say
 * (a port: always on); the uc switch does it for every mapping. 'pte' the
 * flags gpuvm_pte_flags made; returns the flags to map with, *why 0 (as
 * made), 1 (the BO's flags, amdgpu) or 2 (the switch). */
#define RD3_COH_PTE_MTYPE_MASK (7ull << 48)            /* AMDGPU_PTE_MTYPE_NV10_MASK */
#define RD3_COH_PTE_MTYPE_UC   (3ull << 48)            /* MTYPE_UC */
#define RD3_COH_GEM_UC_FLAGS   ((1ull << 13) | (1ull << 14) | (1ull << 15))   /* COHERENT | UNCACHED | EXT_COHERENT */

static inline uint64_t rd3_coh_pte(uint32_t flags, uint64_t pte, uint64_t bo_create_flags, int *why)
{
    int w = (bo_create_flags & RD3_COH_GEM_UC_FLAGS) ? 1 : (flags & RD3_COH_UC) ? 2 : 0;
    if (why)
        *why = w;
    if (!w)
        return pte;
    return (pte & ~RD3_COH_PTE_MTYPE_MASK) | RD3_COH_PTE_MTYPE_UC;
}

/* ---- the CPU wait of wait_idle ---- the longest a CS ioctl waits for
 * its own fence before it returns anyway (counted as a timeout) */
#define RD3_COH_WAIT_IDLE_MAX_US 200000u

#endif /* RDNA3_COH_H */
