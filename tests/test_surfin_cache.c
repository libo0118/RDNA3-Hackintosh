/* The surface-in cache (metal/plugin/nvmtl_surfin_cache.h, DRIVER-PLAN N51): the lock-free probe of a surface texture, the shadow of
 * a buffer texture's last upload, and the pool of private copy images. The shadow tests simulate the image as a byte array that an
 * "upload" overwrites from the shadow, and check after every step that image == what the app's memory held when the shadow was packed
 * (never a stale skip), including the races the plugin has: the app writes between compare and pack, an upload fails, a blit writes
 * the image. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include "nvmtl_surfin_cache.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define W 5u        /* tight bytes per row */
#define H 4u
#define PITCH 8u    /* the app's rows are padded to 8 */

static uint8_t app[PITCH * H];     /* the buffer's CPU memory */
static uint8_t image[W * H];       /* the GPU image, as an array */
static int uploads, fail_next;

/* what the plugin does around nvmtl_bshadow_refresh; returns the verdict */
static int refresh(nvmtl_bshadow *sh)
{
    const int v = nvmtl_bshadow_refresh(sh, app, PITCH, W, H);
    if (v == NVMTL_BS_NOSHADOW) {   /* uploaded from the app's memory */
        for (unsigned y = 0; y < H; y++) memcpy(image + y * W, app + y * PITCH, W);
        uploads++;
    } else if (v == NVMTL_BS_UPLOAD) {
        const int ok = !fail_next;
        if (ok) memcpy(image, sh->buf, W * H);
        else memset(image, 0xEE, sizeof image);   /* an unknown state */
        fail_next = 0; uploads++;
        nvmtl_bshadow_done(sh, ok);
    }
    return v;
}
static int image_equals_app(void)
{
    for (unsigned y = 0; y < H; y++) if (memcmp(image + y * W, app + y * PITCH, W)) return 0;
    return 1;
}

static void test_probe(void)
{
    CHECK(nvmtl_sin_probe(1, 7, 7, 0, 3, 3) == NVMTL_SIN_FRESH);
    CHECK(nvmtl_sin_probe(1, 7, 7, 1, 3, 99) == NVMTL_SIN_FRESH);        /* shared VRAM: no generation to compare */
    CHECK(nvmtl_sin_probe(0, 7, 7, 0, 3, 3) == NVMTL_SIN_NOT_SYNCED);     /* never synced, even with equal numbers */
    CHECK(nvmtl_sin_probe(0, 0, 0, 1, 0, 0) == NVMTL_SIN_NOT_SYNCED);
    CHECK(nvmtl_sin_probe(1, 7, 8, 0, 3, 3) == NVMTL_SIN_SEED);
    CHECK(nvmtl_sin_probe(1, 7, 8, 1, 3, 3) == NVMTL_SIN_SEED);
    CHECK(nvmtl_sin_probe(1, 7, 7, 0, 3, 4) == NVMTL_SIN_GEN);            /* another process wrote the family's VRAM */
    CHECK(nvmtl_sin_probe(1, 7, 8, 0, 3, 4) == NVMTL_SIN_SEED);           /* seed is reported first */
    /* a wrapped seed is just a different number */
    CHECK(nvmtl_sin_probe(1, 0xffffffffu, 0, 0, 0, 0) == NVMTL_SIN_SEED);
}

static void test_shadow(void)
{
    nvmtl_bshadow sh = { NULL, 0, 0 };
    memset(app, 0xAA, sizeof app); memset(image, 0, sizeof image);
    /* no buffer: the caller uploads from the source */
    CHECK(refresh(&sh) == NVMTL_BS_NOSHADOW && image_equals_app() && uploads == 1);
    sh.buf = calloc(1, W * H + 1); sh.len = W * H + 1;                    /* wrong size */
    CHECK(refresh(&sh) == NVMTL_BS_NOSHADOW);
    free(sh.buf); sh.buf = calloc(1, W * H); sh.len = W * H; sh.valid = 0; uploads = 0;

    /* first refresh uploads, the second skips */
    CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && image_equals_app() && sh.valid && uploads == 1);
    CHECK(refresh(&sh) == NVMTL_BS_SKIP && uploads == 1);
    /* the padding bytes (W..PITCH-1) are never uploaded: a change there is no change */
    app[W] = 1; app[PITCH + 7] = 2; app[3 * PITCH + 6] = 3;
    CHECK(refresh(&sh) == NVMTL_BS_SKIP && image_equals_app() && uploads == 1);
    /* a change in every row position is seen: first byte, last byte of the last row, one in the middle */
    for (unsigned i = 0; i < W * H; i++) {
        const unsigned y = i / W, x = i % W;
        app[y * PITCH + x] ^= 0x5A;
        CHECK(refresh(&sh) == NVMTL_BS_UPLOAD);
        CHECK(image_equals_app());
        CHECK(refresh(&sh) == NVMTL_BS_SKIP);
    }
    CHECK(uploads == 1 + (int)(W * H));

    /* a change in a middle row: rows before it are kept, rows after are repacked, the image equals the app */
    app[2 * PITCH + 1] = 0x11; app[3 * PITCH] = 0x22;
    CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && image_equals_app());
    CHECK(refresh(&sh) == NVMTL_BS_SKIP);

    /* an upload that fails: not valid, so the next refresh uploads again even though the app did not change */
    app[0] ^= 1; fail_next = 1; uploads = 0;
    CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && !sh.valid);
    CHECK(!image_equals_app());
    CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && sh.valid && image_equals_app());
    CHECK(refresh(&sh) == NVMTL_BS_SKIP);

    /* a blit / replaceRegion wrote the image: forget, and the next refresh restores it from the buffer */
    memset(image, 0x77, sizeof image);
    CHECK(refresh(&sh) == NVMTL_BS_SKIP && !image_equals_app());          /* without forget the cache would be stale: that is why every writer calls forget */
    nvmtl_bshadow_forget(&sh);
    CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && image_equals_app());

    /* the app writes between the compare and the pack of the same refresh (here: between two refreshes, the pack reads the app once):
     * the image holds what the shadow holds, and the next refresh sees the later write */
    {
        const int v = nvmtl_bshadow_refresh(&sh, app, PITCH, W, H);
        CHECK(v == NVMTL_BS_SKIP);
        app[PITCH + 2] = 0x99;                                            /* after the compare, before the next refresh */
        CHECK(nvmtl_bshadow_refresh(&sh, app, PITCH, W, H) == NVMTL_BS_UPLOAD);
        app[PITCH + 2] = 0x98;                                            /* after the pack, before the upload */
        memcpy(image, sh.buf, W * H); nvmtl_bshadow_done(&sh, 1);
        CHECK(image[W + 2] == 0x99);                                      /* the image got the packed bytes, not the later write */
        CHECK(nvmtl_bshadow_refresh(&sh, app, PITCH, W, H) == NVMTL_BS_UPLOAD);   /* and the later write is not lost */
        memcpy(image, sh.buf, W * H); nvmtl_bshadow_done(&sh, 1);
        CHECK(image_equals_app());
    }

    /* replaceRegion (ECC round 1): the plugin does SurfaceIn (a refresh, which re-validates the shadow) BEFORE it patches the image. forget must come after the patch:
     * replace after a refresh, then a refresh with EQUAL bytes must upload, and the image must equal the app again (the slice variant never writes the buffer) */
    {
        CHECK(refresh(&sh) != NVMTL_BS_NOSHADOW);
        CHECK(refresh(&sh) == NVMTL_BS_SKIP && image_equals_app());
        nvmtl_bshadow_forget(&sh);                                        /* the early forget (before SurfaceIn) ... */
        CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && sh.valid);               /* ... is undone by the refresh SurfaceIn does */
        memset(image, 0x31, 3);                                           /* the patch of the image */
        CHECK(refresh(&sh) == NVMTL_BS_SKIP && !image_equals_app());      /* without a forget AFTER the patch the cache keeps the patch: the bug */
        nvmtl_bshadow_forget(&sh);                                        /* forget after the patch */
        CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && image_equals_app());     /* equal bytes, uploaded anyway */
    }
    /* the layout (ECC round 1): a skip is only allowed for SHADER_READ_ONLY_OPTIMAL (5); TRANSFER_SRC 6, TRANSFER_DST 7, GENERAL 1, UNDEFINED 0 are not */
    CHECK(nvmtl_bshadow_skip_allowed(5));
    CHECK(!nvmtl_bshadow_skip_allowed(0) && !nvmtl_bshadow_skip_allowed(1) && !nvmtl_bshadow_skip_allowed(6) && !nvmtl_bshadow_skip_allowed(7));
    {   /* the plugin's rule: a valid shadow with a layout that does not allow a skip is forgotten first, so the refresh uploads */
        uint32_t layout = 6;
        CHECK(refresh(&sh) == NVMTL_BS_SKIP);
        if (sh.valid && !nvmtl_bshadow_skip_allowed(layout)) nvmtl_bshadow_forget(&sh);
        CHECK(refresh(&sh) == NVMTL_BS_UPLOAD && image_equals_app());
        layout = 5;
        if (sh.valid && !nvmtl_bshadow_skip_allowed(layout)) nvmtl_bshadow_forget(&sh);
        CHECK(refresh(&sh) == NVMTL_BS_SKIP);
    }

    /* bad arguments never skip */
    CHECK(nvmtl_bshadow_refresh(&sh, app, W - 1, W, H) == NVMTL_BS_NOSHADOW);
    CHECK(nvmtl_bshadow_refresh(&sh, NULL, PITCH, W, H) == NVMTL_BS_NOSHADOW);
    CHECK(nvmtl_bshadow_refresh(&sh, app, PITCH, W, 0) == NVMTL_BS_NOSHADOW);
    CHECK(nvmtl_bshadow_refresh(NULL, app, PITCH, W, H) == NVMTL_BS_NOSHADOW);
    /* tight pitch (no padding) */
    {
        uint8_t src[W * H], buf[W * H]; nvmtl_bshadow t = { buf, W * H, 0 };
        memset(src, 3, sizeof src);
        CHECK(nvmtl_bshadow_refresh(&t, src, W, W, H) == NVMTL_BS_UPLOAD); nvmtl_bshadow_done(&t, 1);
        CHECK(nvmtl_bshadow_refresh(&t, src, W, W, H) == NVMTL_BS_SKIP);
        src[W * H - 1] = 4;
        CHECK(nvmtl_bshadow_refresh(&t, src, W, W, H) == NVMTL_BS_UPLOAD);
    }
    free(sh.buf);

    /* admission: one image up to 1 MiB, all shadows up to 32 MiB */
    CHECK(nvmtl_bshadow_admit(1, 0));
    CHECK(!nvmtl_bshadow_admit(0, 0));
    CHECK(nvmtl_bshadow_admit(NVMTL_BSHADOW_MAX_BYTES, 0));
    CHECK(!nvmtl_bshadow_admit(NVMTL_BSHADOW_MAX_BYTES + 1, 0));
    CHECK(nvmtl_bshadow_admit(4096, NVMTL_BSHADOW_BUDGET - 4096));
    CHECK(!nvmtl_bshadow_admit(4097, NVMTL_BSHADOW_BUDGET - 4096));
}

static void key_of(uint32_t *k, uint32_t a) { memset(k, 0, NVMTL_CPOOL_KEY_WORDS * sizeof *k); k[0] = a; k[1] = 7; }

static void test_pool(void)
{
    static nvmtl_cpool p;
    uint32_t k1[NVMTL_CPOOL_KEY_WORDS], k2[NVMTL_CPOOL_KEY_WORDS];
    key_of(k1, 1); key_of(k2, 2);
    CHECK(nvmtl_cpool_take(&p, k1) < 0);
    CHECK(!nvmtl_cpool_admit(0) && nvmtl_cpool_admit(NVMTL_CPOOL_MAX_ONE) && !nvmtl_cpool_admit(NVMTL_CPOOL_MAX_ONE + 1));
    CHECK(nvmtl_cpool_victim(&p, 4096) < 0);
    const int a = nvmtl_cpool_add(&p, k1, 4096), b = nvmtl_cpool_add(&p, k1, 4096), c = nvmtl_cpool_add(&p, k2, 8192);
    CHECK(a >= 0 && b >= 0 && c >= 0 && a != b && b != c && p.total == 4096 + 4096 + 8192);
    /* the most recently parked image of a key comes back first */
    CHECK(nvmtl_cpool_take(&p, k1) == b);
    CHECK(nvmtl_cpool_take(&p, k1) == a);
    CHECK(nvmtl_cpool_take(&p, k1) < 0);
    CHECK(nvmtl_cpool_take(&p, k2) == c && p.total == 0);
    /* too big for the pool */
    CHECK(nvmtl_cpool_add(&p, k1, NVMTL_CPOOL_MAX_ONE + 1) < 0);
    /* full by slots: the oldest is the victim, then there is room */
    for (int i = 0; i < NVMTL_CPOOL_SLOTS; i++) { uint32_t k[NVMTL_CPOOL_KEY_WORDS]; key_of(k, 100 + (uint32_t)i); CHECK(nvmtl_cpool_add(&p, k, 1024) >= 0); }
    CHECK(nvmtl_cpool_add(&p, k1, 1024) < 0);
    int v = nvmtl_cpool_victim(&p, 1024);
    CHECK(v >= 0 && p.e[v].key[0] == 100);
    nvmtl_cpool_drop(&p, v); nvmtl_cpool_drop(&p, v);                    /* a second drop of the same slot changes nothing */
    CHECK(p.total == 1024ull * (NVMTL_CPOOL_SLOTS - 1));
    CHECK(nvmtl_cpool_victim(&p, 1024) < 0);
    CHECK(nvmtl_cpool_add(&p, k1, 1024) == v);
    /* full by bytes: images of 1 MiB, the cap is 4 MiB: evict until the new one fits */
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 4; i++) { uint32_t k[NVMTL_CPOOL_KEY_WORDS]; key_of(k, 200 + (uint32_t)i); CHECK(nvmtl_cpool_add(&p, k, NVMTL_CPOOL_MAX_ONE) >= 0); }
    CHECK(nvmtl_cpool_add(&p, k1, NVMTL_CPOOL_MAX_ONE) < 0);
    v = nvmtl_cpool_victim(&p, NVMTL_CPOOL_MAX_ONE);
    CHECK(v >= 0 && p.e[v].key[0] == 200);
    nvmtl_cpool_drop(&p, v);
    CHECK(nvmtl_cpool_victim(&p, NVMTL_CPOOL_MAX_ONE) < 0 && nvmtl_cpool_add(&p, k1, NVMTL_CPOOL_MAX_ONE) >= 0);
    /* a take refreshes nothing, a re-added key is newer than the others */
    CHECK(p.total == NVMTL_CPOOL_MAX_BYTES);
}

int main(void)
{
    test_probe();
    test_shadow();
    test_pool();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_surfin_cache: ok\n");
    return 0;
}
