/*
 * nvmtl_ab_switch.h - the decisions behind the plugin's A/B switches for the glyph / tile artefacts (DRIVER-PLAN N50).
 *
 * Plain C, no Vulkan and no Objective-C, so tests/test_ab_switch.c checks them on the host exactly as nvmtl_vk.c and
 * NVMTLObjects.m use them:
 *   - which NVMTL_RADV_DEBUG / NVMTL_RADV_PERFTEST values reach RADV's environment (the value lands in the environment of
 *     WindowServer: only RADV's own flag names, at most 200 characters), how they join a value that is there, how the value
 *     is shown in the log (quoted and escaped, so a rejected value with a space is readable);
 *   - the layout slot a LOAD pass is counted under, whether it needs a barrier to make its declared initial layout true, and
 *     whether a depth / stencil attachment is LOADed at all (the render pass declares the layout from `dl || sl`);
 *   - which managed upload ranges the snapshot covers;
 *   - the list of switches a process logs once at start (every knob that is set, in chunks that fit the log line).
 */
#ifndef NVMTL_AB_SWITCH_H
#define NVMTL_AB_SWITCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define NVMTL_RADV_FLAGS_MAX 200u
#define NVMTL_SNAP_MAX_BYTES (1u << 20)

/* RADV's flag names (Mesa 26.2.5's radv_debug_options / radv_perftest_options; the plugin's Mesa d381c361 may know more: a name that is not
 * here is refused with a log line instead of being typed into the environment of WindowServer, add it here when the card's Mesa has it) */
static const char *const nvmtl_radv_debug_names[] = {
    "nofastclears", "nodcc", "nocachecompat", "nocache", "shaderstats", "nohiz", "nocompute", "allbos", "noibchaining", "spirv", "zerovram",
    "syncshaders", "preoptir", "startup", "checkir", "nobinning", "nongg", "metashaders", "llvm", "forcecompress", "hang", "noumr", "nodisplaydcc",
    "notccompatcmask", "novrsflatshading", "noatocdithering", "nonggc", "prologs", "nodma", "epilogs", "nofmask", "shadowregs", "extra_md", "nogpl",
    "nort", "nomeshshader", "noeso", "psocachestats", "nirdebuginfo", "dump_trap_handler", "tcs", "nir", "pso_history", "bvh4", "novideo",
    "validatevas", "dumpibs", "nosmemmitigation", "fullsync", "notmz", "noheap", "info", "img", "asm", "vs", "ps", "cs", "gs", "ts", "ms", "rt", NULL };
static const char *const nvmtl_radv_perftest_names[] = {
    "video_decode", "transfer_queue", "video_encode", "hic", "bfloat16", "msrtss", "localbos", "dccmsaa", "cswave32", "pswave32", "gewave32",
    "nosam", "sam", "rtwave64", "dmashaders", "nircache", "nogttspill", "rtcps", "lowlatencydec", "lowlatencyenc", "nggc", "gpl", "emulate_rt", NULL };

/* 0: v is a list RADV_DEBUG (perftest 0) / RADV_PERFTEST (perftest 1) may be given: comma-separated known names, at most NVMTL_RADV_FLAGS_MAX
 * characters. Otherwise -1 and why says what is wrong (the first offending character as 0xNN, or the unknown name). */
static inline int nvmtl_radv_flag_check(const char *v, int perftest, char *why, size_t cap)
{
    const char *const *names = perftest ? nvmtl_radv_perftest_names : nvmtl_radv_debug_names;
    if (cap) why[0] = 0;
    if (!v || !*v) { if (cap) snprintf(why, cap, "empty value"); return -1; }
    if (strlen(v) > NVMTL_RADV_FLAGS_MAX) { if (cap) snprintf(why, cap, "longer than %u characters", NVMTL_RADV_FLAGS_MAX); return -1; }
    for (const char *c = v; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == ',')) {
            if (cap) snprintf(why, cap, "character 0x%02x at position %u", (unsigned)(unsigned char)*c, (unsigned)(c - v));
            return -1;
        }
    const char *p = v;
    while (*p) {
        const char *e = strchr(p, ','); const size_t n = e ? (size_t)(e - p) : strlen(p);
        int known = 0;
        for (size_t i = 0; names[i] && n; i++) if (strlen(names[i]) == n && !memcmp(names[i], p, n)) { known = 1; break; }
        if (!known) { if (cap) snprintf(why, cap, n ? "unknown %s flag '%.*s'" : "empty flag name", perftest ? "RADV_PERFTEST" : "RADV_DEBUG", (int)n, p); return -1; }
        if (!e) break;
        p = e + 1;
        if (!*p) { if (cap) snprintf(why, cap, "trailing comma"); return -1; }
    }
    return 0;
}

/* DRIVER-PLAN N57: NVMTL_ATTR_RING_BIG_PAGE -> RADV_ATTR_RING_BIG_PAGE (userspace/mesa/patches/0010: "0" turns SPI_ATTRIBUTE_RING_SIZE.BIG_PAGE
 * off, the GFX11 attribute ring is translated in 4 KiB pages). Returns the value RADV gets: "0" for knob "0"; NULL for an unset knob or "1"
 * (upstream, nothing is set; why empty) and for anything else (refused, why says so). */
static inline const char *nvmtl_attr_ring_value(const char *knob, char *why, size_t cap)
{
    if (cap) why[0] = 0;
    if (!knob || !strcmp(knob, "1")) return NULL;
    if (!strcmp(knob, "0")) return "0";
    if (cap) snprintf(why, cap, "only 0 (BIG_PAGE off) or 1 (upstream) is taken");
    return NULL;
}

/* 1: every flag of `add` is already a flag of `old` (both comma lists) */
static inline int nvmtl_radv_flag_contains(const char *old, const char *add)
{
    if (!old || !*old || !add || !*add) return 0;
    const char *p = add;
    while (*p) {
        const char *e = strchr(p, ','); const size_t n = e ? (size_t)(e - p) : strlen(p);
        int found = 0;
        for (const char *q = old; *q && !found; ) {
            const char *f = strchr(q, ','); const size_t m = f ? (size_t)(f - q) : strlen(q);
            if (m == n && !memcmp(p, q, n)) found = 1;
            q = f ? f + 1 : q + m;
        }
        if (!found) return 0;
        if (!e) break;
        p = e + 1;
    }
    return 1;
}

/* out = old,add (or add alone when old is unset or empty, or old when it already has every flag of add); 0 when it fits in cap bytes, -1 otherwise (out is then empty) */
static inline int nvmtl_radv_flag_join(char *out, size_t cap, const char *old, const char *add)
{
    const int has = old && *old;
    const int n = nvmtl_radv_flag_contains(old, add) ? snprintf(out, cap, "%s", old)
                                                     : snprintf(out, cap, "%s%s%s", has ? old : "", has ? "," : "", add ? add : "");
    if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return -1; }
    return 0;
}

/* The value as the log shows it: unset (NULL) -> unset, otherwise "..." with \" \\ and everything outside 0x20..0x7e written as \xNN, cut at cap */
static inline void nvmtl_ab_quote(char *out, size_t cap, const char *v)
{
    if (!cap) return;
    if (!v) { snprintf(out, cap, "unset"); return; }
    size_t n = 0; out[n++] = '"';
    for (const unsigned char *c = (const unsigned char *)v; *c && n + 6 < cap; c++) {
        if (*c == '"' || *c == '\\') { out[n++] = '\\'; out[n++] = (char)*c; }
        else if (*c < 0x20 || *c > 0x7e) n += (size_t)snprintf(out + n, cap - n, "\\x%02x", *c);
        else out[n++] = (char)*c;
    }
    if (n + 2 <= cap) { out[n++] = '"'; }
    out[n < cap ? n : cap - 1] = 0;
}

/* The slot a LOAD pass is counted under, by the layout the image is tracked in: VkImageLayout 0..8 as they are (UNDEFINED, GENERAL,
 * COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT, DEPTH_STENCIL_READ_ONLY, SHADER_READ_ONLY, TRANSFER_SRC, TRANSFER_DST, PREINITIALIZED), 9 = anything else */
static inline unsigned nvmtl_load_layout_slot(uint32_t layout) { return layout < 9u ? layout : 9u; }

/* 1: a LOAD pass whose image is tracked in `have` while the render pass declares `want` needs a barrier from one to the other to make
 * the declaration true (UNDEFINED included: the image was never written, which also initialises its compression metadata) */
static inline int nvmtl_load_needs_barrier(uint32_t have, uint32_t want, int enabled) { return enabled && have != want; }

/* 1: the render pass declares DEPTH_STENCIL_ATTACHMENT_OPTIMAL as the initial layout of the depth attachment: nvmtl_make_render_pass's
 * `dl || sl` (depth LOAD, or stencil LOAD when the format has stencil; without stencil the stencil bit means nothing). `load` is the pass's
 * load bits (NVMTL_LOAD_DEPTH 2, NVMTL_LOAD_STENCIL 4) */
static inline int nvmtl_load_depth_declared(uint32_t load, int has_stencil)
{
    const int dl = (load & 2u) != 0, sl = has_stencil ? (load & 4u) != 0 : dl;
    return dl || sl;
}

/* 1: a managed upload range of n bytes is read from a private snapshot taken at the flush (0 < n <= 1 MiB, the knob on) */
static inline int nvmtl_snap_wanted(size_t n, int enabled) { return enabled && n && n <= NVMTL_SNAP_MAX_BYTES; }

/* DRIVER-PLAN N50, hypothesis A of the glyph artefacts: Apple's drivers copy a managed buffer's modified range in whole pages (or the whole buffer),
 * ours is byte-exact, so bytes an app wrote but did not declare reach the GPU on Apple's and stay stale in VRAM on ours.
 * NVMTL_MANAGED_WHOLE=1 uploads the whole buffer at every flush, NVMTL_MANAGED_PAGES=1 widens the range to whole 4 KiB pages (WHOLE wins). */
#define NVMTL_PAGE_BYTES 4096u
static inline void nvmtl_managed_widen(size_t size, size_t lo, size_t hi, int whole, int pages, size_t *nlo, size_t *nhi)
{
    if (hi > size) hi = size;
    if (lo > hi) lo = hi;
    if (whole) { *nlo = 0; *nhi = size; return; }
    if (pages) {
        lo &= ~(size_t)(NVMTL_PAGE_BYTES - 1);
        hi = (hi + NVMTL_PAGE_BYTES - 1) & ~(size_t)(NVMTL_PAGE_BYTES - 1);
        if (hi > size) hi = size;
    }
    *nlo = lo; *nhi = hi;
}

/* The undeclared-write detector (NVMTL_MANAGED_CHECK): `cur` is the shadow now, `prev` the shadow at the previous flush; the bytes that changed OUTSIDE the
 * range [lo, hi) declared since (the range a flush uploads) were written by the app without a didModifyRange. Fills `o`: how many bytes, the first and last
 * offset, the distance of the nearest one to the range and whether one lies in a page the range touches (what a page-granular copy would have carried). 0 = none. */
typedef struct { size_t bytes, first, last, nearest; int samepage; } nvmtl_undecl;
static inline int nvmtl_undeclared_scan(const uint8_t *cur, const uint8_t *prev, size_t size, size_t lo, size_t hi, nvmtl_undecl *o)
{
    memset(o, 0, sizeof *o);
    if (hi > size) hi = size;
    if (lo > hi) lo = hi;
    const size_t plo = lo & ~(size_t)(NVMTL_PAGE_BYTES - 1), phi = (hi + NVMTL_PAGE_BYTES - 1) & ~(size_t)(NVMTL_PAGE_BYTES - 1);
    int any = 0;
    for (int part = 0; part < 2; part++) {
        size_t a = part ? hi : 0, b = part ? size : lo;
        for (size_t i = a; i < b; ) {
            if (!(i & 7) && i + 8 <= b && !memcmp(cur + i, prev + i, 8)) { i += 8; continue; }
            if (cur[i] != prev[i]) {
                const size_t dist = i < lo ? lo - i : i - hi + 1;
                if (!any || i < o->first) o->first = i;
                if (!any || i > o->last) o->last = i;
                if (!any || dist < o->nearest) o->nearest = dist;
                if (i >= plo && i < phi) o->samepage = 1;
                o->bytes++; any = 1;
            }
            i++;
        }
    }
    return any;
}

/* The knobs a report must show. A process logs the ones that are SET once, at the start (`vk: ab switches`); triage names them. Every
 * optimisation NVMTL_PARANOID turns off, every switch of the ladder and the older ones the ladder uses. */
static const char *const nvmtl_ab_knobs[] = {
    "NVMTL_PARANOID", "NVMTL_RADV_DEBUG", "NVMTL_RADV_PERFTEST", "NVMTL_ASYNC_COMMIT", "NVMTL_GSUB_INFLIGHT", "NVMTL_PRESUBMIT_PROLOGUE",
    "NVMTL_NO_POOL_REUSE", "NVMTL_POOL_REUSE", "NVMTL_NO_BUFREUSE", "NVMTL_NO_BUFTEX_RECYCLE", "NVMTL_NO_HEAPTEX_RECYCLE", "NVMTL_NO_HEAPTEX_PLACE",
    "NVMTL_NO_DIRTYONLY", "NVMTL_ENC78_OFF", "NVMTL_G79_OFF", "NVMTL_NO_RES2", "NVMTL_NO_PURGE_RECLAIM", "NVMTL_MANAGED_CHECK", "NVMTL_MANAGED_SNAPSHOT",
    "NVMTL_LOAD_BARRIER", "NVMTL_NO_ALIGN_GUARD", "NVMTL_FBFETCH_ZERO", "NVMTL_SKIP_EMPTY", "NVMTL_EVFOLD", "NVMTL_DONTCARE_CLEARS", "NVMTL_NO_TEXBUF",
    "NVMTL_NO_SAMPVAR", "NVMTL_MANAGED_WHOLE", "NVMTL_MANAGED_PAGES", "NVMTL_NO_SURFIN_CACHE", "NVMTL_SURFIN_VERIFY", "NVMTL_NO_SURFACE_SHARE", "NVMTL_NO_SURFACE_PAGEOFF", "NVMTL_SHARED_VRAM", "NVMTL_ZERO_FILL", "NVMTL_MANAGED_EAGER_ZERO", "NVMTL_NO_BINDLESS",
    "NVMTL_ATTR_RING_BIG_PAGE", "NVMTL_SCHED_AFTER_PUBLISH", "NVMTL_SURFOUT_DMA", "NVMTL_FBF_MEMBAR", "NVMTL_DRAWABLE_DEFER", "NVMTL_GSUB_LAST_EMPTY", "NVMTL_SURFACE_SHARE",
    "NVMTL_SURFTEX_KEEP", "NVMTL_DRAWABLE_TRACE", "NVMTL_WS_SHARE",
    NULL };

/* Fill `out` (cap bytes) with `KEY="value"` tokens of the knobs that are set, starting at knob index *next, as many as fit; *next moves past them.
 * Returns the number of tokens written (0: nothing more is set). get(name) is getenv or a test's table. */
static inline unsigned nvmtl_ab_switch_chunk(char *out, size_t cap, size_t *next, const char *(*get)(const char *))
{
    unsigned n = 0; size_t used = 0; if (cap) out[0] = 0;
    for (; nvmtl_ab_knobs[*next]; (*next)++) {
        const char *v = get(nvmtl_ab_knobs[*next]);
        if (!v) continue;
        char q[160]; nvmtl_ab_quote(q, sizeof q, v);
        const int w = snprintf(out + used, cap - used, "%s%s=%s", used ? " " : "", nvmtl_ab_knobs[*next], q);
        if (w < 0 || (size_t)w >= cap - used) { out[used] = 0; if (used) break; else continue; }   /* it does not fit: the next chunk starts with it (alone in a chunk, it is dropped) */
        used += (size_t)w; n++;
    }
    return n;
}

#endif
