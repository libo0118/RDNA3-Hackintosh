/*
 * nvmtl_surfin_cache.h - the decisions of the plugin's "surface-in" cache (DRIVER-PLAN N51).
 *
 * Card run 2026-10-25e, WindowServer: the surface-in checks of the render encoders were 38 % of the encode time (104 us per call).
 * What the calls really were (tools/plugin_trace.py, join of ENC_SPLIT with SURF_IN by command buffer): 466 surface UPLOADs
 * (855 us each, 25 % of the time), a cheap seed check (1.6 to 5.5 us per call, measured 0.003 us for IOSurfaceGetSeed and 0.8 us
 * for the tag query on macOS 26.4.1) and the rest ~75 % in calls of 30 to 300 us that leave no trace record: the BUFFER TEXTURES
 * the GPU cannot alias (112 of the first 128, bytesPerRow not a multiple of 256) are private images that are re-uploaded from the
 * buffer's CPU bytes once per command buffer, each upload a synchronous submit + wait (~80 us for a few KB).
 *
 * Plain C, no Vulkan and no Objective-C, so tests/test_surfin_cache.c checks it on the host exactly as NVMTLObjects.m uses it:
 *   1. nvmtl_sin_probe: the lock-free "nothing to upload" test of a surface texture (the same decision as the locked path of
 *      -nvmtlSurfaceIn, taken without the pending-write scan, the string key and the monitor);
 *   2. nvmtl_bshadow_*: a buffer texture's private image is only re-uploaded when the bytes it would upload changed. The shadow
 *      is the exact copy that was uploaded (the upload reads the shadow, not the app's memory), so it can never claim contents
 *      the image does not hold: it is valid only after a SUCCESSFUL upload and dropped by anything that writes the image;
 *   3. nvmtl_cpool_*: a small LRU pool of such private images keyed by their shape (the churn: 32768 images in 13 minutes).
 */
#ifndef NVMTL_SURFIN_CACHE_H
#define NVMTL_SURFIN_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- 1. surface textures: the lock-free probe ---------------------------------------------------------------------------------- */

enum { NVMTL_SIN_FRESH = 0, NVMTL_SIN_NOT_SYNCED = 1, NVMTL_SIN_SEED = 2, NVMTL_SIN_GEN = 3 };

/* NVMTL_SIN_FRESH: the GPU image holds what the surface holds, no upload is needed. Otherwise why not (the cache's miss reasons,
 * counted): never synced, the surface's seed moved since the last copy either way, or (private image only, a texture on shared VRAM
 * has no generation) another process wrote the family's VRAM and bumped the tag generation. `genNow` is read by the caller only when
 * it matters (!vramOn and the seed matches). The verdict is only ever "no upload" when every condition the locked path would test
 * first holds; whatever is not FRESH takes the locked path, which decides again under the monitor. */
static inline int nvmtl_sin_probe(int synced, uint32_t seedHave, uint32_t seedNow, int vramOn, uint32_t genHave, uint32_t genNow)
{
    if (!synced) return NVMTL_SIN_NOT_SYNCED;
    if (seedHave != seedNow) return NVMTL_SIN_SEED;
    if (!vramOn && genHave != genNow) return NVMTL_SIN_GEN;
    return NVMTL_SIN_FRESH;
}

/* ---- 2. buffer textures: the shadow of the last upload ------------------------------------------------------------------------- */

#define NVMTL_BSHADOW_MAX_BYTES  (1u << 20)    /* an image whose tight rows are larger is uploaded every time, as before */
#define NVMTL_BSHADOW_BUDGET     (32u << 20)   /* all shadows of a process together */

typedef struct { uint8_t *buf; size_t len; int valid; } nvmtl_bshadow;

enum { NVMTL_BS_NOSHADOW = 0,   /* no shadow (too big, no memory, off): the caller uploads from the source as before */
       NVMTL_BS_SKIP = 1,       /* the bytes equal what the image holds: no upload                                    */
       NVMTL_BS_UPLOAD = 2 };   /* the shadow now holds the bytes to upload: upload FROM sh->buf with row pitch `tight`, then _done() */

/* 1: a shadow of `bytes` may be allocated (one image <= NVMTL_BSHADOW_MAX_BYTES, the process <= NVMTL_BSHADOW_BUDGET with `used` taken) */
static inline int nvmtl_bshadow_admit(uint64_t bytes, uint64_t used)
{
    return bytes && bytes <= NVMTL_BSHADOW_MAX_BYTES && used + bytes <= NVMTL_BSHADOW_BUDGET;
}

/* One refresh of an image of `rows` rows of `tight` bytes from the source `src` (rows `pitch` apart, pitch >= tight; the bytes between
 * tight and pitch are padding that no upload reads, so a change there is no change). NOSHADOW when sh has no buffer of exactly
 * tight * rows bytes. With a valid shadow the rows are compared and an equal image is SKIP; otherwise the rows from the first
 * difference (all of them when the shadow is not valid) are copied into the shadow ONCE and the shadow is marked not valid until the
 * caller has uploaded those very bytes and says so with nvmtl_bshadow_done. The upload therefore always uploads what the shadow holds,
 * whatever the app writes into its memory meanwhile, and a later compare sees that write. */
static inline int nvmtl_bshadow_refresh(nvmtl_bshadow *sh, const uint8_t *src, size_t pitch, size_t tight, uint32_t rows)
{
    if (!sh || !sh->buf || !src || !tight || !rows || pitch < tight || sh->len != tight * (size_t)rows) return NVMTL_BS_NOSHADOW;
    uint32_t from = 0;
    if (sh->valid) {
        for (; from < rows; from++)
            if (memcmp(src + (size_t)from * pitch, sh->buf + (size_t)from * tight, tight)) break;
        if (from == rows) return NVMTL_BS_SKIP;
    }
    sh->valid = 0;
    for (uint32_t y = from; y < rows; y++) memcpy(sh->buf + (size_t)y * tight, src + (size_t)y * pitch, tight);
    return NVMTL_BS_UPLOAD;
}

/* VkImageLayout of an image the refresh may SKIP: only SHADER_READ_ONLY_OPTIMAL (5), the layout every refresh upload leaves the image in and the one the render descriptors
 * declare. A copy-source use, a copy to a buffer or a getBytes leaves TRANSFER_SRC (6), a blit TRANSFER_DST (7), a compute write GENERAL (1): the render encoders do not
 * transition a sampled image, so for those the refresh must upload again (which also puts the layout right). */
#define NVMTL_BS_LAYOUT_SRO 5u
static inline int nvmtl_bshadow_skip_allowed(uint32_t layout) { return layout == NVMTL_BS_LAYOUT_SRO; }

/* the upload of the shadow's bytes finished: ok = it succeeded. A failed upload leaves the image in an unknown state, so the next refresh uploads again. */
static inline void nvmtl_bshadow_done(nvmtl_bshadow *sh, int ok) { if (sh) sh->valid = ok ? 1 : 0; }

/* something other than a refresh writes the image (a blit, a replaceRegion, mipmap generation, a resolve): the image no longer equals the shadow */
static inline void nvmtl_bshadow_forget(nvmtl_bshadow *sh) { if (sh) sh->valid = 0; }

/* ---- 3. the pool of private copy images ---------------------------------------------------------------------------------------- */

#define NVMTL_CPOOL_SLOTS      16
#define NVMTL_CPOOL_KEY_WORDS  8
#define NVMTL_CPOOL_MAX_BYTES  (4ull << 20)   /* all parked images together */
#define NVMTL_CPOOL_MAX_ONE    (1ull << 20)   /* one image */

typedef struct { uint32_t key[NVMTL_CPOOL_KEY_WORDS]; uint64_t bytes; uint64_t stamp; int used; } nvmtl_cpool_ent;
typedef struct { nvmtl_cpool_ent e[NVMTL_CPOOL_SLOTS]; uint64_t clock; uint64_t total; } nvmtl_cpool;

/* the slot of the most recently parked image with this key, which is removed from the pool (the caller moves the payload out); -1: none */
static inline int nvmtl_cpool_take(nvmtl_cpool *p, const uint32_t *key)
{
    int best = -1;
    for (int i = 0; i < NVMTL_CPOOL_SLOTS; i++)
        if (p->e[i].used && !memcmp(p->e[i].key, key, sizeof p->e[i].key) && (best < 0 || p->e[i].stamp > p->e[best].stamp)) best = i;
    if (best >= 0) { p->e[best].used = 0; p->total -= p->e[best].bytes; }
    return best;
}

/* 1: an image of `bytes` may be parked at all */
static inline int nvmtl_cpool_admit(uint64_t bytes) { return bytes && bytes <= NVMTL_CPOOL_MAX_ONE; }

/* the slot to evict before an image of `bytes` can be parked (the oldest), -1 when there is room; the caller moves the payload out and
 * calls this again until it answers -1 */
static inline int nvmtl_cpool_victim(const nvmtl_cpool *p, uint64_t bytes)
{
    int free_slot = 0, oldest = -1;
    for (int i = 0; i < NVMTL_CPOOL_SLOTS; i++) {
        if (!p->e[i].used) { free_slot = 1; continue; }
        if (oldest < 0 || p->e[i].stamp < p->e[oldest].stamp) oldest = i;
    }
    if (free_slot && p->total + bytes <= NVMTL_CPOOL_MAX_BYTES) return -1;
    return oldest;   /* -1 only when nothing is parked, and then the pool is empty and has room */
}

/* the evicted entry is gone: free its slot */
static inline void nvmtl_cpool_drop(nvmtl_cpool *p, int slot)
{
    if (slot < 0 || slot >= NVMTL_CPOOL_SLOTS || !p->e[slot].used) return;
    p->e[slot].used = 0; p->total -= p->e[slot].bytes;
}

/* park: the slot to store the payload in, -1 when there is no room (the caller destroys the image). Call after nvmtl_cpool_victim answered -1. */
static inline int nvmtl_cpool_add(nvmtl_cpool *p, const uint32_t *key, uint64_t bytes)
{
    if (!nvmtl_cpool_admit(bytes) || p->total + bytes > NVMTL_CPOOL_MAX_BYTES) return -1;
    for (int i = 0; i < NVMTL_CPOOL_SLOTS; i++) {
        if (p->e[i].used) continue;
        memcpy(p->e[i].key, key, sizeof p->e[i].key); p->e[i].bytes = bytes; p->e[i].stamp = ++p->clock; p->e[i].used = 1; p->total += bytes;
        return i;
    }
    return -1;
}

#endif
