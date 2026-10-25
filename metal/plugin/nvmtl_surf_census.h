/*
 * nvmtl_surf_census.h - what touched the IOSurface-backed render targets of this process, and how (DRIVER-PLAN N50).
 *
 * Plain C (no Objective-C, no Vulkan), so tests/test_surf_census.c checks the table and the line exactly as the plugin builds them. The plugin keeps one
 * table per process behind a lock (nvmtl_cen_note in NVMTLTrace.m) and prints it as `census: SUMMARY` (time-based, newest line of a process counts).
 *
 * The question it answers (card run 2026-10-25f, Terminal): the window is one IOSurface that the CPU, the GPU (a render pass, Apple's buffer_image_blit
 * draws, a framebuffer-fetch draw), WindowServer and a page-off all touch. Which of them touched THIS surface, how often and in which order of magnitude
 * is the first thing a "the whole window is right, the incremental update is not" report needs.
 *
 * Counters per surface (letters in the `top=` list):
 *   u surface-in uploads into family VRAM   U surface-in uploads into a private image   s surface-in skipped (VRAM current)   ! surface-in failed
 *   P render passes with the surface as a colour target   c of them cleared at the start (load action Clear)
 *   D draws in those passes   B draws whose pipeline is Apple's buffer_image_blit_*   F draws of a framebuffer-fetch pass
 *   E blit-encoder copies into it   R replaceRegion into it   X compute (storage twin) writes   T presented
 *   O GPU write published as a dirty mark (VRAM newest: the next CPU lock pages it off)   C GPU write copied out into the CPU surface   ? publish failed
 * A page-off itself happens in the kernel; what this process sees of it is `u` (the CPU copy was newer than the VRAM tag when the surface was next used: a CPU
 * lock, a window capture or the display pipe moved the seed) and `O`.
 */
#ifndef NVMTL_SURF_CENSUS_H
#define NVMTL_SURF_CENSUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define NVC_MAX 128
#define NVC_TOP_MAX 8

enum { NVC_SIN_UP_VRAM, NVC_SIN_UP_PRIV, NVC_SIN_SKIP, NVC_SIN_FAIL, NVC_PASS, NVC_PASS_CLEAR, NVC_DRAW, NVC_DRAW_BLIT, NVC_DRAW_FBF,
       NVC_BLITENC, NVC_REPLACE, NVC_COMPUTE, NVC_PRESENT, NVC_OUT_DIRTY, NVC_OUT_COPY, NVC_OUT_FAIL, NVC_SIN_BYTES, NVC_N };

typedef struct { uint32_t id, w, h; uint8_t vram; uint64_t c[NVC_N]; } nvc_surf;
typedef struct { nvc_surf s[NVC_MAX]; unsigned n; uint64_t tot[NVC_N], overflow; } nvc_table;

/* the letter of a counter in the `top=` list (the bytes of the uploads are only a total) */
static const char nvc_letter[NVC_N] = { 'u', 'U', 's', '!', 'P', 'c', 'D', 'B', 'F', 'E', 'R', 'X', 'T', 'O', 'C', '?', 0 };

/* The slot of surface `id` (made at the first sight; its size and placement are refreshed), or NULL for id 0 or a full table (counted in `overflow`). The caller holds the
 * table's lock. A slot never moves and is never freed, so the pointer may be kept (a render encoder keeps it for the draws of its pass) and counted into with nvc_add. */
static inline nvc_surf *nvc_slot(nvc_table *t, uint32_t id, uint32_t w, uint32_t h, int vram)
{
    if (!id) return NULL;
    nvc_surf *f = NULL;
    for (unsigned i = 0; i < t->n; i++) if (t->s[i].id == id) { f = &t->s[i]; break; }
    if (!f) {
        if (t->n >= NVC_MAX) { __atomic_fetch_add(&t->overflow, 1, __ATOMIC_RELAXED); return NULL; }
        f = &t->s[t->n]; memset(f, 0, sizeof *f); f->id = id;
        __atomic_store_n(&t->n, t->n + 1, __ATOMIC_RELEASE);
    }
    f->w = w; f->h = h; f->vram = vram ? 1 : 0;
    return f;
}
/* count `n` of `kind` on a slot (NULL: only the total): lock-free, so a draw does not take the table's lock */
static inline void nvc_add(nvc_table *t, nvc_surf *f, int kind, uint64_t n)
{
    if (kind < 0 || kind >= NVC_N) return;
    __atomic_fetch_add(&t->tot[kind], n, __ATOMIC_RELAXED);
    if (f) __atomic_fetch_add(&f->c[kind], n, __ATOMIC_RELAXED);
}
/* count `n` of `kind` for surface `id` (a full table counts the event in the totals and in `overflow` only) */
static inline void nvc_note(nvc_table *t, uint32_t id, uint32_t w, uint32_t h, int vram, int kind, uint64_t n)
{
    if (kind < 0 || kind >= NVC_N || !id) return;
    nvc_add(t, nvc_slot(t, id, w, h, vram), kind, n);
}

/* how busy a surface is: every touch but a skip */
static inline uint64_t nvc_activity(const nvc_surf *s)
{
    uint64_t a = 0;
    for (int k = 0; k < NVC_SIN_BYTES; k++) if (k != NVC_SIN_SKIP && k != NVC_PASS_CLEAR && k != NVC_DRAW_BLIT && k != NVC_DRAW_FBF) a += s->c[k];
    return a;
}

/* "ID:WxH:V|P:u12,s3,P40,c2,D90,B40": the surface's non-zero counters */
static inline size_t nvc_format_surf(const nvc_surf *s, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    n += (size_t)snprintf(out + n, cap - n, "%u:%ux%u:%c:", s->id, s->w, s->h, s->vram ? 'V' : 'P');
    int any = 0;
    for (int k = 0; k < NVC_SIN_BYTES && n < cap; k++) {
        if (!s->c[k]) continue;
        n += (size_t)snprintf(out + n, cap - n, "%s%c%llu", any ? "," : "", nvc_letter[k], (unsigned long long)s->c[k]);
        any = 1;
    }
    if (!any && n < cap) n += (size_t)snprintf(out + n, cap - n, "-");
    return n < cap ? n : cap - 1;
}

/* the body of the SUMMARY line: totals, then the `top` busiest surfaces (at most NVC_TOP_MAX). 0 when the table is empty (nothing to say). */
static inline int nvc_format(const nvc_table *t, char *out, size_t cap, unsigned top)
{
    if (!cap) return 0;
    out[0] = 0;
    if (!t->n && !t->overflow) return 0;
    if (top > NVC_TOP_MAX) top = NVC_TOP_MAX;
    const uint64_t *c = t->tot;
    size_t n = (size_t)snprintf(out, cap, "surfaces=%u overflow=%llu sin_up_vram=%llu sin_up_priv=%llu sin_up_mb=%llu sin_skip=%llu sin_fail=%llu pass=%llu pass_clear=%llu draws=%llu draw_blit=%llu draw_fbf=%llu "
                                "blitenc=%llu replace=%llu compute=%llu present=%llu out_dirty=%llu out_copy=%llu out_fail=%llu top=",
                                t->n, (unsigned long long)t->overflow, (unsigned long long)c[NVC_SIN_UP_VRAM], (unsigned long long)c[NVC_SIN_UP_PRIV],
                                (unsigned long long)(c[NVC_SIN_BYTES] >> 20), (unsigned long long)c[NVC_SIN_SKIP], (unsigned long long)c[NVC_SIN_FAIL],
                                (unsigned long long)c[NVC_PASS], (unsigned long long)c[NVC_PASS_CLEAR], (unsigned long long)c[NVC_DRAW], (unsigned long long)c[NVC_DRAW_BLIT],
                                (unsigned long long)c[NVC_DRAW_FBF], (unsigned long long)c[NVC_BLITENC], (unsigned long long)c[NVC_REPLACE], (unsigned long long)c[NVC_COMPUTE],
                                (unsigned long long)c[NVC_PRESENT], (unsigned long long)c[NVC_OUT_DIRTY], (unsigned long long)c[NVC_OUT_COPY], (unsigned long long)c[NVC_OUT_FAIL]);
    if (n >= cap) return (int)(cap - 1);
    unsigned used[NVC_MAX] = { 0 }; unsigned shown = 0;
    while (shown < top) {
        int best = -1; uint64_t ba = 0;
        for (unsigned i = 0; i < t->n; i++) { const uint64_t a = nvc_activity(&t->s[i]); if (!used[i] && (best < 0 || a > ba)) { best = (int)i; ba = a; } }
        if (best < 0) break;
        used[best] = 1;
        if (shown) { if (n + 1 < cap) out[n++] = ';'; }
        n += nvc_format_surf(&t->s[best], out + n, cap - n);
        shown++;
        if (n + 1 >= cap) break;
    }
    if (!shown && n + 1 < cap) { out[n++] = '-'; out[n] = 0; }
    return (int)(n < cap ? n : cap - 1);
}

/* the kind of a draw's pipeline: 1 = Apple's buffer_image_blit_* (vertex or fragment function name), 0 = any other */
static inline int nvc_is_blit_pipeline(const char *vname, const char *fname)
{
    static const char key[] = "buffer_image_blit";
    return (vname && strstr(vname, key)) || (fname && strstr(fname, key));
}

#endif
