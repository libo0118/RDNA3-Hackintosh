/*
 * rdna3_modetest - list and switch display modes through CoreGraphics, so
 * the unattended report can try the scaled modes RDNA3Framebuffer offers
 * (rdna3modes=1) and look at the pipes after each switch.
 *
 *   rdna3_modetest list              every display, its modes, the current one
 *   rdna3_modetest set D W H         display D (index in the list) to a W x H mode
 *   rdna3_modetest native            every display back to its largest mode
 *   rdna3_modetest cycle DRMINFO DIR every display first to the fastest mode of
 *                                    its own size (if one beats the running
 *                                    refresh; DIR/mode-d<D>-<W>x<H>-<Hz>hz) and
 *                                    back, then through up to two smaller
 *                                    modes; after each switch DRMINFO --scanout
 *                                    DIR/mode-d<D>-<W>x<H> looks at the pipes;
 *                                    then all back to native
 *   rdna3_modetest gamma DRMINFO DIR the output gamma (rdna3gamma=1): every display
 *                                    gets a dark table, an identity table, then
 *                                    its ColorSync settings back; DRMINFO
 *                                    --scanout DIR/gamma-<step> reads each pipe's
 *                                    OTG CRC (taken after the gamma, unlike the
 *                                    picture, which is VRAM). PASS when the CRC
 *                                    moves with the dark table and comes back
 *                                    after the restore
 *
 * Exit 0 when the switch was accepted (CGDisplaySetDisplayMode == 0 and the
 * display reports the new size), 1 otherwise.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreGraphics/CoreGraphics.h>

#define MAX_DISPLAYS 8

static CFArrayRef all_modes(CGDirectDisplayID d)
{
    const void *k[] = { kCGDisplayShowDuplicateLowResolutionModes };
    const void *v[] = { kCFBooleanTrue };
    CFDictionaryRef opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    CFArrayRef m = CGDisplayCopyAllDisplayModes(d, opt);
    if (opt)
        CFRelease(opt);
    return m;
}

static void print_mode(const char *tag, CGDisplayModeRef m)
{
    printf("  %s %zux%zu (pixels %zux%zu) %.2f Hz, IO flags 0x%08x%s\n", tag, CGDisplayModeGetWidth(m),
           CGDisplayModeGetHeight(m), CGDisplayModeGetPixelWidth(m), CGDisplayModeGetPixelHeight(m),
           CGDisplayModeGetRefreshRate(m), CGDisplayModeGetIOFlags(m),
           CGDisplayModeIsUsableForDesktopGUI(m) ? "" : " (not for the desktop)");
}

static uint32_t displays(CGDirectDisplayID *ids)
{
    /* card run 29: an empty list from the report's daemon while both
     * framebuffers were open - ask again for up to 30 s and say why */
    for (int t = 0; t < 10; t++) {
        uint32_t n = 0;
        CGError e = CGGetOnlineDisplayList(MAX_DISPLAYS, ids, &n);
        if (e == kCGErrorSuccess && n)
            return n;
        if (t == 0 || t == 9)
            fprintf(stderr, "rdna3_modetest: CGGetOnlineDisplayList -> error %d, %u display(s)%s\n", (int)e, n,
                    t == 9 ? ", giving up" : ", asking again");
        sleep(3);
    }
    return 0;
}

static int list(void)
{
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = displays(ids);
    printf("%u display(s)\n", n);
    for (uint32_t i = 0; i < n; i++) {
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(ids[i]);
        CFArrayRef modes = all_modes(ids[i]);
        printf("display %u: id 0x%x%s, vendor 0x%x model 0x%x, %zux%zu\n", i, ids[i],
               CGDisplayIsMain(ids[i]) ? " (main)" : "", CGDisplayVendorNumber(ids[i]),
               CGDisplayModelNumber(ids[i]), CGDisplayPixelsWide(ids[i]), CGDisplayPixelsHigh(ids[i]));
        if (cur) {
            print_mode("current", cur);
            CGDisplayModeRelease(cur);
        }
        for (CFIndex k = 0; modes && k < CFArrayGetCount(modes); k++)
            print_mode("mode   ", (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, k));
        if (modes)
            CFRelease(modes);
    }
    return n ? 0 : 1;
}

/* the first mode of that size (pixel size = point size: no HiDPI variant) */
static int set(uint32_t di, size_t w, size_t h)
{
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = displays(ids);
    CGDisplayModeRef pick = NULL;
    CFArrayRef modes;
    int rc = 1;
    if (di >= n) {
        printf("no display %u (%u online)\n", di, n);
        return 1;
    }
    modes = all_modes(ids[di]);
    for (CFIndex k = 0; modes && k < CFArrayGetCount(modes) && !pick; k++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, k);
        if (CGDisplayModeGetWidth(m) == w && CGDisplayModeGetHeight(m) == h &&
            CGDisplayModeGetPixelWidth(m) == w)
            pick = m;
    }
    if (!pick) {
        printf("display %u: no %zux%zu mode\n", di, w, h);
    } else {
        CGDisplayConfigRef cfg;
        CGError e = CGBeginDisplayConfiguration(&cfg);
        if (e == kCGErrorSuccess) {
            e = CGConfigureDisplayWithDisplayMode(cfg, ids[di], pick, NULL);
            e = e == kCGErrorSuccess ? CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession)
                                     : (CGCancelDisplayConfiguration(cfg), e);
        }
        sleep(2);
        printf("display %u: set %zux%zu -> CGError %d, now %zux%zu\n", di, w, h, (int)e,
               CGDisplayPixelsWide(ids[di]), CGDisplayPixelsHigh(ids[di]));
        rc = e == kCGErrorSuccess && CGDisplayPixelsWide(ids[di]) == w ? 0 : 1;
    }
    if (modes)
        CFRelease(modes);
    return rc;
}

/* display 'di' to mode 'm' itself (same size, another refresh rate) */
static int set_ref(uint32_t di, CGDirectDisplayID id, CGDisplayModeRef m)
{
    CGDisplayConfigRef cfg;
    CGDisplayModeRef now;
    double want = CGDisplayModeGetRefreshRate(m), got = 0;
    CGError e = CGBeginDisplayConfiguration(&cfg);
    if (e == kCGErrorSuccess) {
        e = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
        e = e == kCGErrorSuccess ? CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession)
                                 : (CGCancelDisplayConfiguration(cfg), e);
    }
    sleep(2);
    now = CGDisplayCopyDisplayMode(id);
    if (now) {
        got = CGDisplayModeGetRefreshRate(now);
        CGDisplayModeRelease(now);
    }
    printf("display %u: set %zux%zu %.2f Hz -> CGError %d, now %zux%zu %.2f Hz\n", di, CGDisplayModeGetWidth(m),
           CGDisplayModeGetHeight(m), want, (int)e, CGDisplayPixelsWide(id), CGDisplayPixelsHigh(id), got);
    return e == kCGErrorSuccess && got - want < 0.5 && want - got < 0.5 ? 0 : 1;
}

/* the fastest mode of the display's own size above its refresh rate (a
 * real timing that may need more DISPCLK, rdna3dispclk=1), a look at the
 * pipes, then the old mode again; 0 tried when there is none */
static int faster(uint32_t di, CGDirectDisplayID id, const char *drminfo, const char *dir, int *tried)
{
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id), pick = NULL;
    CFArrayRef modes = all_modes(id);
    size_t nw = CGDisplayPixelsWide(id), nh = CGDisplayPixelsHigh(id);
    double best = cur ? CGDisplayModeGetRefreshRate(cur) + 0.5 : 1e9;
    int bad = 0;
    char cmd[1024];
    for (CFIndex k = 0; modes && k < CFArrayGetCount(modes); k++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, k);
        if (CGDisplayModeGetPixelWidth(m) == nw && CGDisplayModeGetPixelHeight(m) == nh &&
            CGDisplayModeGetWidth(m) == nw && CGDisplayModeGetRefreshRate(m) > best) {
            pick = m;
            best = CGDisplayModeGetRefreshRate(m);
        }
    }
    if (pick) {
        (*tried)++;
        printf("== mode: display %u -> %zux%zu at %.2f Hz (faster)\n", di, nw, nh, best);
        fflush(stdout);
        if (set_ref(di, id, pick)) {
            bad++;
        } else {
            sleep(4);
            printf("== rdna3_drminfo --scanout (look mode-d%u-%zux%zu-%.0fhz)\n", di, nw, nh, best);
            fflush(stdout);
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s/mode-d%u-%zux%zu-%.0fhz' && '%s' --scanout '%s/mode-d%u-%zux%zu-%.0fhz' "
                     "2>&1; echo \"[exit $?]\"", dir, di, nw, nh, best, drminfo, dir, di, nw, nh, best);
            if (system(cmd) != 0)
                bad++;
        }
        if (cur)
            bad += set_ref(di, id, cur);
    }
    if (modes)
        CFRelease(modes);
    if (cur)
        CGDisplayModeRelease(cur);
    return bad;
}

static int native(void)
{
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = displays(ids);
    int rc = 0;
    for (uint32_t i = 0; i < n; i++) {
        CFArrayRef modes = all_modes(ids[i]);
        size_t bw = 0, bh = 0;
        for (CFIndex k = 0; modes && k < CFArrayGetCount(modes); k++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, k);
            if (CGDisplayModeGetPixelWidth(m) * CGDisplayModeGetPixelHeight(m) > bw * bh) {
                bw = CGDisplayModeGetPixelWidth(m);
                bh = CGDisplayModeGetPixelHeight(m);
            }
        }
        if (modes)
            CFRelease(modes);
        if (bw && CGDisplayPixelsWide(ids[i]) != bw)
            rc |= set(i, bw, bh);
    }
    return rc;
}

static int cycle(const char *drminfo, const char *dir)
{
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = displays(ids);
    int bad = 0, tried = 0;
    for (uint32_t i = 0; i < n; i++)
        bad += faster(i, ids[i], drminfo, dir, &tried);
    for (uint32_t i = 0; i < n; i++) {
        CFArrayRef modes = all_modes(ids[i]);
        size_t done_w[2] = { 0, 0 }, nw = CGDisplayPixelsWide(ids[i]);
        uint32_t nd = 0;
        /* the largest smaller mode and the smallest one: the gentlest and
         * the hardest scale */
        size_t pick_w[2] = { 0, 0 }, pick_h[2] = { 0, 0 };
        for (CFIndex k = 0; modes && k < CFArrayGetCount(modes); k++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, k);
            size_t w = CGDisplayModeGetPixelWidth(m), h = CGDisplayModeGetPixelHeight(m);
            if (w >= nw || w != CGDisplayModeGetWidth(m))
                continue;
            if (w > pick_w[0]) { pick_w[0] = w; pick_h[0] = h; }
            if (!pick_w[1] || w < pick_w[1]) { pick_w[1] = w; pick_h[1] = h; }
        }
        if (modes)
            CFRelease(modes);
        for (uint32_t k = 0; k < 2; k++) {
            char cmd[1024];
            if (!pick_w[k] || (nd && done_w[0] == pick_w[k]))
                continue;
            done_w[nd++] = pick_w[k];
            tried++;
            printf("== mode: display %u -> %zux%zu\n", i, pick_w[k], pick_h[k]);
            fflush(stdout);
            if (set(i, pick_w[k], pick_h[k])) {
                bad++;
                continue;
            }
            sleep(4);
            printf("== rdna3_drminfo --scanout (look mode-d%u-%zux%zu)\n", i, pick_w[k], pick_h[k]);
            fflush(stdout);
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s/mode-d%u-%zux%zu' && '%s' --scanout '%s/mode-d%u-%zux%zu' 2>&1; echo \"[exit $?]\"",
                     dir, i, pick_w[k], pick_h[k], drminfo, dir, i, pick_w[k], pick_h[k]);
            if (system(cmd) != 0)
                bad++;
        }
    }
    printf("== mode: back to native\n");
    bad += native() != 0;
    printf("MODES: %d switch(es) tried, %d failed\n", tried, bad);
    return bad ? 1 : 0;
}

#define GAMMA_N 256

/* what DRMINFO --scanout DIR/gamma-<name> says about each pipe: lit, the
 * OTG CRC (after the gamma) and the mean colour of the picture (VRAM,
 * before the gamma). Its output is passed through. */
struct pipe_look { int lit, valid, pic; unsigned r, g, b, mean; };
static int look(const char *drminfo, const char *dir, const char *name, struct pipe_look pl[4])
{
    char cmd[1024], line[512];
    int pipe = -1;
    FILE *f;
    memset(pl, 0, sizeof(*pl) * 4);
    printf("== rdna3_drminfo --scanout (look gamma-%s)\n", name);
    fflush(stdout);
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/gamma-%s' && '%s' --scanout '%s/gamma-%s' 2>&1", dir, name, drminfo,
             dir, name);
    f = popen(cmd, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned p, a, b, c, w, h, nb;
        fputs(line, stdout);
        if (sscanf(line, "pipe %u:", &p) == 1 && p < 4) {
            pipe = (int)p;
            pl[p].lit = strstr(line, " LIT ") != NULL;
        } else if (pipe >= 0 && sscanf(line, "  OTG CRC %x/%x/%x", &a, &b, &c) == 3) {
            pl[pipe].valid = 1;
            pl[pipe].r = a;
            pl[pipe].g = b;
            pl[pipe].b = c;
        } else if (pipe >= 0 && sscanf(line, "  picture %ux%u: %u%% not black, mean rgb %u %u %u", &w, &h, &nb, &a, &b,
                                       &c) == 6) {
            pl[pipe].pic = 1;
            pl[pipe].mean = a + b + c;
        }
    }
    fflush(stdout);
    return pclose(f) == 0 ? 0 : -1;
}

static int crc_eq(const struct pipe_look *a, const struct pipe_look *b)
{
    return a->valid && b->valid && a->r == b->r && a->g == b->g && a->b == b->b;
}

static void set_all(const CGDirectDisplayID *ids, uint32_t n, const CGGammaValue *t, int *bad)
{
    for (uint32_t i = 0; i < n; i++)
        if (CGSetDisplayTransferByTable(ids[i], GAMMA_N, t, t, t) != kCGErrorSuccess) {
            printf("display %u: CGSetDisplayTransferByTable refused the table\n", i);
            *bad = 1;
        }
    sleep(2);   /* WindowServer hands it to IOFramebuffer::setGammaTable */
}

/* every display through a dark table (a quarter of the range), the
 * identity, and back to its ColorSync settings, with the pipes' CRCs after
 * each. The screen may change on its own (a blinking text cursor): two
 * looks before, and up to three after the restore, each compared with both. */
static int gamma_test(const char *drminfo, const char *dir)
{
    CGDirectDisplayID ids[MAX_DISPLAYS];
    static CGGammaValue save[3][GAMMA_N], dark[GAMMA_N], ident[GAMMA_N];
    uint32_t saved = 0, n = displays(ids), i, k;
    struct pipe_look base[2][4], drk[4], idn[4], rest[4];
    int bad = 0, pipes = 0, stable = 1, moved = 0, back = 0, soft = 0, ok[4] = { 0 };
    char name[32];

    for (i = 0; i < GAMMA_N; i++) {
        ident[i] = (CGGammaValue)i / (GAMMA_N - 1);
        dark[i] = ident[i] / 4;
    }
    printf("%u display(s)\n", n);
    for (i = 0; i < n; i++) {
        float worst = 0;
        if (CGGetDisplayTransferByTable(ids[i], GAMMA_N, save[0], save[1], save[2], &saved) != kCGErrorSuccess)
            saved = 0;
        for (k = 0; saved > 1 && k < 3 * saved; k++) {
            float d = save[k / saved][k % saved] - (float)(k % saved) / (float)(saved - 1);
            if (d < 0)
                d = -d;
            if (d > worst)
                worst = d;
        }
        printf("display %u: id 0x%x, ColorSync table of %u entries, %s (largest step from the identity %.4f)\n", i,
               ids[i], saved, saved ? (worst < 0.002f ? "identity" : "a curve") : "not readable", (double)worst);
    }
    if (!n || look(drminfo, dir, "before", base[0]) || look(drminfo, dir, "before2", base[1])) {
        printf("GAMMA: FAIL - no display, or rdna3_drminfo --scanout failed\n");
        return 1;
    }
    set_all(ids, n, dark, &bad);
    look(drminfo, dir, "dark", drk);
    set_all(ids, n, ident, &bad);
    look(drminfo, dir, "identity", idn);
    CGDisplayRestoreColorSyncSettings();
    sleep(2);
    for (k = 0; k < 3; k++) {
        int all = 1;
        snprintf(name, sizeof(name), k ? "restored%u" : "restored", k + 1);
        look(drminfo, dir, name, rest);
        for (i = 0; i < 4; i++) {
            ok[i] |= crc_eq(&rest[i], &base[0][i]) || crc_eq(&rest[i], &base[1][i]);
            all &= ok[i] || !base[0][i].lit || !base[0][i].valid;
        }
        if (all)
            break;
    }

    for (i = 0; i < 4; i++) {
        const struct pipe_look *b = &base[0][i];
        if (!b->lit || !b->valid)
            continue;
        pipes++;
        stable &= crc_eq(b, &base[1][i]);
        moved += !crc_eq(&drk[i], b) && !crc_eq(&drk[i], &base[1][i]);
        back += ok[i];
        /* the table in the compositor instead of the OGAM: the picture in
         * VRAM darkens too */
        soft += b->pic && drk[i].pic && drk[i].mean * 10 < b->mean * 7;
        printf("== gamma pipe %u: CRC %04x/%04x/%04x, dark %04x/%04x/%04x, identity %04x/%04x/%04x, "
               "restored %04x/%04x/%04x (%s); picture mean %u -> %u with the dark table\n", i, b->r, b->g, b->b,
               drk[i].r, drk[i].g, drk[i].b, idn[i].r, idn[i].g, idn[i].b, rest[i].r, rest[i].g, rest[i].b,
               ok[i] ? "back" : "NOT back", b->mean, drk[i].mean);
    }
    printf("CRC before %s\n", stable ? "stable" : "NOT stable (the screen changed between the two looks)");
    if (bad || !pipes) {
        printf("GAMMA: FAIL - %s\n", bad ? "CGSetDisplayTransferByTable refused a table" : "no lit pipe with an OTG CRC");
        return 1;
    }
    if (soft) {
        printf("GAMMA: FAIL - the picture in VRAM darkened: the compositor applied the table, not the OGAM\n");
        return 1;
    }
    if (moved == pipes && back == pipes) {
        printf("GAMMA: PASS - %d pipe(s): the CRC moved with the dark table and came back after the restore\n", pipes);
        return 0;
    }
    printf("GAMMA: FAIL - %d of %d pipe(s) moved with the dark table, %d came back (%s)\n", moved, pipes, back,
           moved < pipes ? "no gamma reaches the pipe: rdna3gamma=1? fb/core log 'gamma'"
                         : "the restore did not reach the pipe");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "list"))
        return list();
    if (argc >= 5 && !strcmp(argv[1], "set"))
        return set((uint32_t)atoi(argv[2]), (size_t)atol(argv[3]), (size_t)atol(argv[4]));
    if (argc >= 2 && !strcmp(argv[1], "native"))
        return native();
    if (argc >= 4 && !strcmp(argv[1], "cycle"))
        return cycle(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "gamma"))
        return gamma_test(argv[2], argv[3]);
    fprintf(stderr, "usage: rdna3_modetest list | set DISPLAY W H | native | cycle DRMINFO DIR | gamma DRMINFO DIR\n");
    return 2;
}
