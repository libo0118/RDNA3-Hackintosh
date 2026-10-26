"""Automatic glyph verdict for the A/B ladder's scanout looks (DRIVER-PLAN N50, docs/AFTER-TESTS.md "The whole ladder in one card run").

The ladder's Terminal prints one tick line a second; every tick line ends in the same text (`... abcdefghijklmnopqrstuvwxyz`, the time stamp is
at the left). Drawn right, two tick lines are the same picture of that text. Drawn wrong (card run 2026-10-25f: glyph quads or atlas cells that
turn into random strokes) every line is different, because the garbage is different every time. So the verdict does not need to know what
the glyphs should look like, only that identical text is drawn identically:

  1. in the look (a PPM or a PNG - tools/lookimg.py reads both -; `--picture` of tools/triage_reports.py makes PNGs) it finds stacks of text lines: runs of
     >= 8 bands of rows that have many horizontal edges, at an even pitch, with a gap between them. Looks come at the panel's native size (1920x1080 since
     2026-10-26, every source pixel) or, from older kexts, 1280 wide (nearest-neighbour: one source row and column in three dropped). The geometry below
     (line heights, pitches, gaps, margins, windows) is in pixels of a 1280-wide look and is scaled by the look's width / 1280 (`Geom`), so both kinds
     are judged alike;
  2. in each stack it keeps the lines whose right edge is the same (the tick lines: a log window or a directory listing has ragged edges, so a
     window of changing text is never taken for one);
  3. it cuts the right half of each kept line (the part after the time stamp) as a black-and-white strip and looks for a TWIN: another line whose
     strip differs in at most TWIN_TOL of its ink. In a native look every tick line is the same pixels when the text is right; in a 1280-wide look the
     sampling drops one source row in three, so the lines fall into at most three classes of rows seen, and lines of one class are pixel-identical (a
     class has several lines in 15 or more). A 12 % tolerance covers the antialiasing that depends on the colour of the line; a line with random
     strokes has no twin;
  4. clean when >= 80 % of the lines have a twin, broken when <= 50 %, unclear in between; no stack of 8 lines = `none`.

Pure Python (no PIL, no numpy): the report host's triage must run anywhere. Checked on the card run 2026-10-25f looks (every live window broken, 9 to
30 % of the lines with a twin; the windows Terminal restored from its saved state, static text drawn right, 95 to 100 %) and on synthetic looks
(tests/test_glyph_verdict.py). `verdict_files()` takes the looks of one rung (both pipes) and answers the worst stack: a restored window never hides a
broken live one. Limits: a text window that is not on a screen is `none` (the rung proves nothing); text that is wrong in the SAME way on every line
(a constant wrong glyph) would pass, the eye and the owner's y / n are the check for that."""
import itertools
import os
import re
import sys
from collections import Counter

try:
    import lookimg
except ImportError:   # run from another directory without tools/ on the path
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lookimg

REF_W = 1280         # the geometry constants below are in pixels of a look this wide (a 1920 panel sampled to 1280); Geom scales them
MIN_BANDS = 8        # a stack of fewer lines proves nothing
TWIN_TOL = 0.12      # a twin differs in at most this share of its ink
CLEAN_AT = 0.80      # share of the lines with a twin: at least this = clean
BROKEN_AT = 0.50     # at most this = broken
EDGE = 40            # luminance step that is an edge
WIN, STEP = 96, 48   # the windows the stacks are looked for in (columns)
STRIP_FROM = 0.5     # the strip starts here between the left and the right edge of the text (after the time stamp)
CLUSTER_GAP = 40     # columns of blank that end a line's text
INK_DIFF = 90        # distance (sum of the three channels) from the window's background that is ink


class Geom:
    """the pixel geometry of the heuristics for a look `w` wide: the constants above (in pixels of a REF_W-wide look) times w / REF_W, at least 1 (a
    1920-wide native look of a 1920 panel: 1.5, the text is 1.5 times as many pixels as in the 1280-wide sample)"""

    def __init__(self, w):
        self.scale = k = max(1.0, w / float(REF_W))
        r = lambda v: int(round(v * k))
        self.win, self.step = r(WIN), r(STEP)
        self.band_min, self.band_max = r(4), r(14)           # a line of text: rows tall
        self.gap_min, self.gap_max = 1, r(6)                 # rows between two lines
        self.pitch_min, self.pitch_max, self.pitch_tol = r(7), r(26), max(2, r(2))
        self.margin_x, self.margin_y = r(60), max(2, r(2))   # around a stack: the window's background is sampled there
        self.edge_tol = max(2, r(2))                         # the tick lines' common right edge
        self.min_strip = r(30)                               # the strip after the time stamp
        self.cluster_gap = r(CLUSTER_GAP)
        self.min_pitch = r(8)                                # changed_pixels: the line pitch at least
        self.live_min = int(round(LIVE_MIN * k * k))         # changed pixels: an area


def parse_ppm(data):
    """(width, height, pixels) of a binary PPM (P6, 8 bit) or a PNG (8 bit RGB, RGBA, grey), 64..8192 each way, or ValueError. (The name is from the PPM-only days.)"""
    if data[:2] != b"P6" and data[:8] != lookimg.PNG_MAGIC:
        raise ValueError("not a binary PPM (P6) or a PNG")
    w, h, pix = lookimg.decode(data)
    if w < 64 or h < 64 or w > 8192 or h > 8192:
        raise ValueError("unsupported look: %sx%s" % (w, h))
    return w, h, pix


def _runs(flags):
    out, i, n = [], 0, len(flags)
    while i < n:
        if flags[i]:
            s = i
            while i < n and flags[i]:
                i += 1
            out.append((s, i))
        else:
            i += 1
    return out


def _chain(bands, g=None):
    """the longest run of bands that look like lines of text: 4..14 rows tall, 1..6 rows apart, an even pitch of 7..26 rows (in a 1280-wide look; Geom scales them)"""
    g = g or Geom(REF_W)
    best, chain = [], []
    for b in bands:
        if not g.band_min <= b[1] - b[0] <= g.band_max:
            if len(chain) > len(best):
                best = chain
            chain = []
            continue
        if chain:
            gap, pitch = b[0] - chain[-1][1], b[0] - chain[-1][0]
            ppitch = chain[-1][0] - chain[-2][0] if len(chain) > 1 else pitch
            if g.gap_min <= gap <= g.gap_max and abs(pitch - ppitch) <= g.pitch_tol and g.pitch_min <= pitch <= g.pitch_max:
                chain.append(b)
                continue
            if len(chain) > len(best):
                best = chain
        chain = [b]
    return chain if len(chain) > len(best) else best


def _find_stacks(L, w, h, g=None):
    """[(nbands, y0, y1, x0, x1)] per window of WIN columns (scaled) that holds a chain of >= MIN_BANDS lines, merged into stacks (same rows, touching windows)"""
    g = g or Geom(w)
    pref = []
    for row in L:
        p = [0]
        p.extend(itertools.accumulate(1 if abs(a - b) > EDGE else 0 for a, b in zip(row, row[1:])))
        pref.append(p)
    hits = []
    for x0 in range(0, w - g.win + 1, g.step):
        seq = [pref[y][x0 + g.win - 1] - pref[y][x0] >= 3 for y in range(h)]
        ch = _chain(_runs(seq), g)
        if len(ch) >= MIN_BANDS:
            hits.append([len(ch), ch[0][0], ch[-1][1], x0, x0 + g.win])
    # windows that hold the same rows are one stack: their columns are joined
    hits.sort(key=lambda t: (t[1], t[3]))
    merged = []
    for hit in hits:
        for m in merged:
            ov = min(m[2], hit[2]) - max(m[1], hit[1])
            if ov >= 0.6 * min(m[2] - m[1], hit[2] - hit[1]) and hit[3] <= m[4]:
                m[0] = max(m[0], hit[0]); m[1] = min(m[1], hit[1]); m[2] = max(m[2], hit[2]); m[4] = max(m[4], hit[4])
                break
        else:
            merged.append(hit)
    return merged


def _median(v):
    s = sorted(v)
    return s[len(s) // 2]


def _analyze_stack(pix, w, h, st, tol, g=None):
    g = g or Geom(w)
    n0, y0, y1, cx0, cx1 = st
    xlo, xhi, ya, yb = max(cx0 - g.margin_x, 0), min(cx1 + g.margin_x, w), max(y0 - g.margin_y, 0), min(y1 + g.margin_y, h)
    cnt = Counter()
    for y in range(ya, yb, 2):
        row = pix[(y * w + cx0) * 3:(y * w + cx1) * 3]
        cnt.update(zip(row[0::3], row[1::3], row[2::3]))
    bg = cnt.most_common(1)[0][0]
    D = []
    for y in range(ya, yb):
        row = pix[(y * w + xlo) * 3:(y * w + xhi) * 3]
        D.append([abs(a - bg[0]) + abs(b - bg[1]) + abs(c - bg[2]) for a, b, c in zip(row[0::3], row[1::3], row[2::3])])
    ink = [[1 if v > INK_DIFF else 0 for v in r] for r in D]
    c0, c1 = cx0 - xlo, cx1 - xlo
    rc = [sum(r[c0:c1]) for r in ink]
    mx = max(rc) if rc else 0
    bands = [b for b in _runs([c >= max(2, 0.12 * mx) for c in rc]) if b[1] - b[0] >= g.band_min]
    lines = []   # (a, b, left, right) of each line's own text, in rect coordinates
    for a, b in bands:
        cols = [x for x in range(xhi - xlo) if any(ink[y][x] for y in range(a, b))]
        clusters, cur = [], []
        for x in cols:
            if cur and x - cur[-1] > g.cluster_gap:
                clusters.append(cur); cur = []
            cur.append(x)
        if cur:
            clusters.append(cur)
        mine = [c for c in clusters if c[0] <= c1 and c[-1] >= c0]
        if mine:
            c = max(mine, key=len)
            lines.append((a, b, c[0], c[-1]))
    if len(lines) < MIN_BANDS:
        return None
    # the tick lines: the same right edge (a window of changing text is ragged there)
    best_v, best_k = None, []
    for v in sorted({ln[3] for ln in lines}):
        k = [ln for ln in lines if abs(ln[3] - v) <= g.edge_tol]
        if len(k) > len(best_k):
            best_v, best_k = v, k
    if len(best_k) < MIN_BANDS or len(best_k) < 0.6 * len(lines):
        return None
    xa, xb = _median([ln[2] for ln in best_k]), _median([ln[3] for ln in best_k])
    xs = xa + int(STRIP_FROM * (xb - xa))
    if xb - xs < g.min_strip:
        return None
    # each line's strip as one integer of bits (rows top-aligned, padded to the tallest): a twin test is an xor and a popcount
    subs = []
    for a, b, _l, _r in best_k:
        sub = [D[y][xs:xb + 1] for y in range(a, b)]
        thr = 0.45 * max(max(r) for r in sub)
        subs.append([[1 if v > thr else 0 for v in r] for r in sub])
    H, Wd = max(len(s) for s in subs), xb + 1 - xs
    bits, ones = [], []
    for sub in subs:
        v = 0
        for y in range(H):
            for p in (sub[y] if y < len(sub) else [0] * Wd):
                v = (v << 1) | p
        bits.append(v); ones.append(bin(v).count("1"))
    twinned, ratios = 0, []
    for i in range(len(bits)):
        best = None
        for j in range(len(bits)):
            if i == j:
                continue
            d = bin(bits[i] ^ bits[j]).count("1") / max(ones[i], ones[j], 1)
            if best is None or d < best:
                best = d
        ratios.append(best if best is not None else 1.0)
        if best is not None and best <= tol:
            twinned += 1
    share = twinned / len(bits)
    return {"lines": len(bits), "twinned": twinned, "share": share, "ratios": ratios,
            "rect": (xlo + xa, ya + best_k[0][0], xlo + xb, ya + best_k[-1][1]), "strip_x": xlo + xs,
            "verdict": "clean" if share >= CLEAN_AT else "broken" if share <= BROKEN_AT else "unclear"}


def analyze_ppm(data, tol=TWIN_TOL):
    """the tick stacks of one look (the bytes of a PPM or a PNG): [{"lines", "twinned", "share", "verdict", "rect", ...}], biggest first; [] when there is none. ValueError: not a picture"""
    w, h, pix = parse_ppm(data)
    L = []
    for y in range(h):
        r = pix[y * w * 3:(y + 1) * w * 3]
        L.append([(a + b + c) // 3 for a, b, c in zip(r[0::3], r[1::3], r[2::3])])
    out, g = [], Geom(w)
    for st in _find_stacks(L, w, h, g):
        r = _analyze_stack(pix, w, h, st, tol, g)
        if r:
            r["scale"] = g.scale
            out.append(r)
    out.sort(key=lambda r: -r["lines"])
    return out


_ORDER = {"broken": 0, "unclear": 1, "clean": 2}

LIVE_STEP = 60       # luminance step between two looks that counts as a changed pixel
LIVE_MIN = 150       # changed pixels in a stack's rectangle (grown to where the next lines appear) that make it LIVE (in a 1280-wide look: Geom.live_min scales it by the area)


def _lum(pix, w, y, x0, x1):
    r = pix[(y * w + x0) * 3:(y * w + x1) * 3]
    return [(a + b + c) // 3 for a, b, c in zip(r[0::3], r[1::3], r[2::3])]


def changed_pixels(data_a, data_b, rect, pitch=None):
    """pixels of `rect` (x0, y0, x1, y1) whose luminance differs by more than LIVE_STEP between two looks of the same screen, in the stack's OWN columns (nothing to the sides:
    a clock or another window there is not the text) and grown downwards by four line pitches only (the next tick lines appear right below). ValueError when the two looks are not
    the same size"""
    wa, ha, pa = parse_ppm(data_a)
    wb, hb, pb = parse_ppm(data_b)
    if (wa, ha) != (wb, hb):
        raise ValueError("the two looks differ in size (%dx%d against %dx%d)" % (wa, ha, wb, hb))
    x0, y0, x1, y1 = rect
    pitch = pitch or max((y1 - y0) // 20, Geom(wa).min_pitch)
    x0, x1, y0, y1 = max(x0, 0), min(x1 + 1, wa), max(y0 - 2, 0), min(y1 + 4 * pitch, ha)
    n = 0
    for y in range(y0, y1):
        la, lb = _lum(pa, wa, y, x0, x1), _lum(pb, wb, y, x0, x1)
        n += sum(1 for a, b in zip(la, lb) if abs(a - b) > LIVE_STEP)
    return n


def _pipe_of(path):
    m = re.search(r"-pipe(\d+)\.(?:ppm|png)$", path.replace("\\", "/"))
    return m.group(1) if m else None


def verdict_files(paths, tol=TWIN_TOL, later=None):
    """the look files of one rung (its pipes): {"verdict": clean|broken|unclear|none, "text": one line for the triage, "stacks": [...]}.

    Without `later` the worst stack decides: broken text anywhere on the screen is broken text (a window Terminal restored from its saved state shows static
    text, drawn clean, next to the live one).
    With `later` (the second look of each pipe, taken a few seconds after the first, same file names with another look name) only a LIVE stack counts: the text
    script prints a line a second, so the window that holds the rung's own text CHANGES between the two looks, and a window that does not (a restored one, another
    rung's, a frozen screen) is listed as static and left out of the verdict. No live stack = `none`. `none` also when no file could be read; the text says which."""
    stacks, errs, static, pipes = [], [], [], set()
    partner = {}
    if later is not None:
        for q in later:
            partner[_pipe_of(q)] = q
    for p in paths:
        try:
            with open(p, "rb") as f:
                data = f.read()
            found = analyze_ppm(data, tol)
            other = None
            if later is not None:
                q = partner.get(_pipe_of(p))
                if q is None:
                    errs.append("%s: no second look of this pipe" % p.replace("\\", "/").rsplit("/", 1)[-1])
                    continue
                with open(q, "rb") as f:
                    other = f.read()
            pipes.add(_pipe_of(p) or "?")
            for r in found:
                r["file"] = p
                if other is not None:
                    g = Geom(parse_ppm(data)[0])
                    r["changed"] = changed_pixels(data, other, r["rect"], max((r["rect"][3] - r["rect"][1]) // max(r["lines"], 1), g.min_pitch))
                    r["live"] = r["changed"] >= g.live_min
                    if not r["live"]:
                        static.append(r)
                        continue
                stacks.append(r)
        except (OSError, ValueError) as e:
            errs.append("%s: %s" % (p.replace("\\", "/").rsplit("/", 1)[-1], e))
    live_note = ""
    if later is not None and static:
        live_note = "; %d STATIC tick stack(s) ignored (unchanged between the two looks: %s)" % (
            len(static), ", ".join("%d/%d %s" % (r["twinned"], r["lines"], r["verdict"]) for r in static[:4]))
    if not stacks:
        if errs and len(errs) == len(paths):
            why = "no look could be read (%s)" % "; ".join(errs)[:200]
        elif later is not None and static:
            why = "no LIVE tick stack: the %d tick stack(s) on the screens did not change between the two looks (restored or frozen text, not the rung's own)" % len(static)
        else:
            why = ("no stack of >= %d tick lines in the look%s (is the tick Terminal on the screen, not behind Chess? open the pictures)") % (MIN_BANDS, "" if len(paths) == 1 else "s")
        return {"verdict": "none", "text": why + ("; pipes judged: " + ", ".join(sorted(pipes)) if pipes else ""), "stacks": [], "static": static, "pipes": sorted(pipes)}
    worst = min(stacks, key=lambda r: (_ORDER[r["verdict"]], r["share"]))
    text = "%s: %d of %d tick lines have a twin (%.0f %%)" % (worst["verdict"].upper(), worst["twinned"], worst["lines"], 100.0 * worst["share"])
    if later is not None:
        text += " in the live window"
    if len(stacks) > 1:
        text += "; %d tick stack(s) on the screens: %s" % (len(stacks), ", ".join("%d/%d %s" % (r["twinned"], r["lines"], r["verdict"]) for r in stacks[:4]))
    text += live_note
    text += "; pipes judged: " + ", ".join(sorted(pipes))
    if errs:
        text += "; unreadable: " + "; ".join(errs)[:120]
    return {"verdict": worst["verdict"], "text": text, "stacks": stacks, "static": static, "pipes": sorted(pipes)}
