/*
 * rdna3_strip.h - the frame-order strip (DRIVER-PLAN N59): a frame counter a web page draws as a row of
 * black / white blocks, read back from the scanout many times a second (rdna3_drminfo --scanout-strip) and
 * decoded on the host (tools/frame_order.py). Plain C: rdna3_drminfo, the loopback's test picture and
 * tests/test_strip.c share it; tools/frame_order.py mirrors the layout (keep both in step).
 *
 * Layout, left to right, RD3_STRIP_H rows tall, at the page's 1:1 scale:
 *   RD3_STRIP_MARK_W px of the layer's marker colour: css (0,255,0), canvas (0,255,255), webgl (255,255,0)
 *   RD3_STRIP_BITS blocks of RD3_STRIP_BIT_W px: the counter, least significant bit first, white 1 black 0
 *   RD3_STRIP_BITS blocks: the same bits inverted (a sample that is not their exact complement is torn)
 *   RD3_STRIP_MARK_W px of magenta (255,0,255)
 * The counter runs modulo RD3_STRIP_MOD (canvas, webgl) or RD3_STRIP_CSS_MOD (the CSS animation's steps).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_STRIP_H
#define RDNA3_STRIP_H

#include <stdint.h>
#include <string.h>

#define RD3_STRIP_BITS     12u
#define RD3_STRIP_MARK_W   24u
#define RD3_STRIP_BIT_W    16u
#define RD3_STRIP_H        16u
#define RD3_STRIP_W        (2u * RD3_STRIP_MARK_W + 2u * RD3_STRIP_BITS * RD3_STRIP_BIT_W)
#define RD3_STRIP_MOD      4096u
#define RD3_STRIP_CSS_MOD  240u
#define RD3_STRIP_LAYERS   3u
#define RD3_STRIP_MAX      8u      /* strips one look may lock on to */

enum { RD3_SL_CSS = 0, RD3_SL_CANVAS = 1, RD3_SL_WEBGL = 2 };
static const char *const rd3_strip_layer_name[RD3_STRIP_LAYERS] = { "css", "canvas", "webgl" };
static const uint8_t rd3_strip_marker[RD3_STRIP_LAYERS][3] = { {0, 255, 0}, {0, 255, 255}, {255, 255, 0} };

/* a pixel's class: 'G' 'C' 'Y' (the layers' markers), 'M' magenta, 'W' white, 'K' black, '?' anything else */
static inline char rd3_strip_class(const uint8_t *px)
{
    const int r = px[0] >= 180 ? 1 : px[0] <= 75 ? 0 : -1;
    const int g = px[1] >= 180 ? 1 : px[1] <= 75 ? 0 : -1;
    const int b = px[2] >= 180 ? 1 : px[2] <= 75 ? 0 : -1;
    if (r < 0 || g < 0 || b < 0) return '?';
    switch (r << 2 | g << 1 | b) {
    case 2: return 'G';
    case 3: return 'C';
    case 6: return 'Y';
    case 5: return 'M';
    case 7: return 'W';
    case 0: return 'K';
    default: return '?';
    }
}
static inline int rd3_strip_layer_of(char c) { return c == 'G' ? RD3_SL_CSS : c == 'C' ? RD3_SL_CANVAS : c == 'Y' ? RD3_SL_WEBGL : -1; }

typedef struct { int layer; uint32_t x; } rd3_strip_hit;

/* the strips in one row of `w` RGB pixels: a marker run at least RD3_STRIP_MARK_W - 4 long whose magenta end
 * marker starts where the layout puts it (+-2 px). Up to `max` hits; returns how many. */
static inline unsigned rd3_strip_find(const uint8_t *rgb, uint32_t w, rd3_strip_hit *hit, unsigned max)
{
    unsigned n = 0;
    for (uint32_t x = 0; x + RD3_STRIP_W <= w + 2 && n < max;) {
        const int layer = rd3_strip_layer_of(rd3_strip_class(rgb + 3 * x));
        if (layer < 0) { x++; continue; }
        uint32_t e = x;
        while (e < w && rd3_strip_class(rgb + 3 * e) == rd3_strip_class(rgb + 3 * x)) e++;
        const uint32_t run = e - x;
        if (run + 4 >= RD3_STRIP_MARK_W && run <= RD3_STRIP_MARK_W + 4) {
            const uint32_t start = e > RD3_STRIP_MARK_W ? e - RD3_STRIP_MARK_W : 0;   /* the marker's left edge from its right one */
            const uint32_t m = e + 2u * RD3_STRIP_BITS * RD3_STRIP_BIT_W;
            int ok = 0;
            for (uint32_t d = m >= 2 ? m - 2 : 0; d <= m + 2 && d + 4 <= w; d++)
                if (rd3_strip_class(rgb + 3 * d) == 'M' && rd3_strip_class(rgb + 3 * (d + 3)) == 'M') { ok = 1; break; }
            if (ok) { hit[n].layer = layer; hit[n].x = start; n++; x = m; continue; }
        }
        x = e > x ? e : x + 1;
    }
    return n;
}

/* the bits of a strip whose marker starts at rgb[0]: out[] gets 2 * RD3_STRIP_BITS chars '1' / '0' / '?'
 * (white, black, neither: a blend or a moved strip) and a terminating 0; 'x' when the row is too short */
static inline void rd3_strip_bits(const uint8_t *rgb, uint32_t avail, char *out)
{
    for (uint32_t b = 0; b < 2 * RD3_STRIP_BITS; b++) {
        const uint32_t cx = RD3_STRIP_MARK_W + b * RD3_STRIP_BIT_W + RD3_STRIP_BIT_W / 2;
        if (cx >= avail) { out[b] = 'x'; continue; }
        const char c = rd3_strip_class(rgb + 3 * cx);
        out[b] = c == 'W' ? '1' : c == 'K' ? '0' : '?';
    }
    out[2 * RD3_STRIP_BITS] = 0;
}

/* the counter in a bit string, or -1 when it is not a valid sample (a '?', or the halves not complements) */
static inline int rd3_strip_value(const char *bits)
{
    int v = 0;
    for (uint32_t b = 0; b < RD3_STRIP_BITS; b++) {
        const char d = bits[b], i = bits[b + RD3_STRIP_BITS];
        if ((d != '0' && d != '1') || (i != '0' && i != '1') || d == i) return -1;
        if (d == '1') v |= 1 << b;
    }
    return v;
}

/* the strip of `layer` showing `value` into one row of RGB pixels starting at rgb[0] (the loopback's picture) */
static inline void rd3_strip_paint(uint8_t *rgb, int layer, uint32_t value)
{
    for (uint32_t x = 0; x < RD3_STRIP_W; x++) {
        uint8_t *p = rgb + 3 * x;
        if (x < RD3_STRIP_MARK_W) { memcpy(p, rd3_strip_marker[layer], 3); continue; }
        if (x >= RD3_STRIP_W - RD3_STRIP_MARK_W) { p[0] = 255; p[1] = 0; p[2] = 255; continue; }
        const uint32_t b = (x - RD3_STRIP_MARK_W) / RD3_STRIP_BIT_W;
        const uint32_t bit = b < RD3_STRIP_BITS ? (value >> b) & 1u : 1u - ((value >> (b - RD3_STRIP_BITS)) & 1u);
        p[0] = p[1] = p[2] = bit ? 255 : 0;
    }
}

#endif
