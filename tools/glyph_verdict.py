"""Automatic glyph verdict for the A/B ladder's scanout looks (DRIVER-PLAN N50, docs/AFTER-TESTS.md "The whole ladder in one card run").

The ladder's Terminal prints one tick line a second; every tick line ends in the same text (`... abcdefghijklmnopqrstuvwxyz`, the time stamp is
at the left). Drawn right, two tick lines are the same picture of that text. Drawn wrong (card run 2026-10-25f: glyph quads or atlas cells that
turn into random strokes) every line is different, because the garbage is different every time. So the verdict does not need to know what
the glyphs should look like, only that identical text is drawn identically:

  1. in the look (a PPM, 1280 wide, nearest-neighbour sampled from the scanout: tools/triage_reports.py `--picture` makes PNGs of the same files) it
     finds stacks of text lines: runs of >= 8 bands of rows that have many horizontal edges, at an even pitch, with a gap between them;
  2. in each stack it keeps the lines whose right edge is the same (the tick lines: a log window or a directory listing has ragged edges, so a
     window of changing text is never taken for one);
  3. it cuts the right half of each kept line (the part after the time stamp) as a black-and-white strip and looks for a TWIN: another line whose
     strip differs in at most TWIN_TOL of its ink. The sampling drops one source row in three, so the lines fall into at most three classes of rows
     seen; lines of one class are pixel-identical when the text is right (a 12 % tolerance covers the antialiasing that depends on the colour of
     the line), and a class has several lines in 15 or more; a line with random strokes has no twin;
  4. clean when >= 80 % of the lines have a twin, broken when <= 50 %, unclear in between; no stack of 8 lines = `none`.

Pure Python (no PIL, no numpy): the report host's triage must run anywhere. Checked on the card run 2026-10-25f looks (every live window broken, 9 to
30 % of the lines with a twin; the windows Terminal restored from its saved state, static text drawn right, 95 to 100 %) and on synthetic looks
(tests/test_glyph_verdict.py). `verdict_files()` takes the looks of one rung (both pipes) and answers the worst stack: a restored window never hides a
broken live one. Limits: a text window that is not on a screen is `none` (the rung proves nothing); text that is wrong in the SAME way on every line
(a constant wrong glyph) would pass, the eye and the owner's y / n are the check for that."""
import itertools
from collections import Counter

MIN_BANDS = 8        # a stack of fewer lines proves nothing
TWIN_TOL = 0.12      # a twin differs in at most this share of its ink
CLEAN_AT = 0.80      # share of the lines with a twin: at least this = clean
BROKEN_AT = 0.50     # at most this = broken
EDGE = 40            # luminance step that is an edge
WIN, STEP = 96, 48   # the windows the stacks are looked for in (columns)
STRIP_FROM = 0.5     # the strip starts here between the left and the right edge of the text (after the time stamp)
CLUSTER_GAP = 40     # columns of blank that end a line's text
INK_DIFF = 90        # distance (sum of the three channels) from the window's background that is ink


def parse_ppm(data):
    """(width, height, pixels) of a binary PPM (P6, 8 bit), or ValueError"""
    if data[:2] != b"P6":
        raise ValueError("not a binary PPM (P6)")
    toks, i, n = [], 2, len(data)
    while len(toks) < 3 and i < n:
        while i < n and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < n and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < n and not data[j:j + 1].isspace():
            j += 1
        toks.append(data[i:j]); i = j
    if len(toks) < 3 or i >= n:
        raise ValueError("truncated PPM header")
    try:
        w, h, mx = int(toks[0]), int(toks[1]), int(toks[2])
    except ValueError:
        raise ValueError("bad PPM header") from None
    if mx != 255 or w < 64 or h < 64 or w > 8192 or h > 8192:
        raise ValueError("unsupported PPM: %sx%s max %s" % (w, h, mx))
    pix = data[i + 1:i + 1 + w * h * 3]
    if len(pix) != w * h * 3:
        raise ValueError("truncated PPM pixels")
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


def _chain(bands):
    """the longest run of bands that look like lines of text: 4..14 rows tall, 1..6 rows apart, an even pitch of 7..26 rows"""
    best, chain = [], []
    for b in bands:
        if not 4 <= b[1] - b[0] <= 14:
            if len(chain) > len(best):
                best = chain
            chain = []
            continue
        if chain:
            gap, pitch = b[0] - chain[-1][1], b[0] - chain[-1][0]
            ppitch = chain[-1][0] - chain[-2][0] if len(chain) > 1 else pitch
            if 1 <= gap <= 6 and abs(pitch - ppitch) <= 2 and 7 <= pitch <= 26:
                chain.append(b)
                continue
            if len(chain) > len(best):
                best = chain
        chain = [b]
    return chain if len(chain) > len(best) else best


def _find_stacks(L, w, h):
    """[(nbands, y0, y1, x0, x1)] per window of WIN columns that holds a chain of >= MIN_BANDS lines, merged into stacks (same rows, touching windows)"""
    pref = []
    for row in L:
        p = [0]
        p.extend(itertools.accumulate(1 if abs(a - b) > EDGE else 0 for a, b in zip(row, row[1:])))
        pref.append(p)
    hits = []
    for x0 in range(0, w - WIN + 1, STEP):
        seq = [pref[y][x0 + WIN - 1] - pref[y][x0] >= 3 for y in range(h)]
        ch = _chain(_runs(seq))
        if len(ch) >= MIN_BANDS:
            hits.append([len(ch), ch[0][0], ch[-1][1], x0, x0 + WIN])
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


def _analyze_stack(pix, w, h, st, tol):
    n0, y0, y1, cx0, cx1 = st
    xlo, xhi, ya, yb = max(cx0 - 60, 0), min(cx1 + 60, w), max(y0 - 2, 0), min(y1 + 2, h)
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
    bands = [b for b in _runs([c >= max(2, 0.12 * mx) for c in rc]) if b[1] - b[0] >= 4]
    lines = []   # (a, b, left, right) of each line's own text, in rect coordinates
    for a, b in bands:
        cols = [x for x in range(xhi - xlo) if any(ink[y][x] for y in range(a, b))]
        clusters, cur = [], []
        for x in cols:
            if cur and x - cur[-1] > CLUSTER_GAP:
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
        k = [ln for ln in lines if abs(ln[3] - v) <= 2]
        if len(k) > len(best_k):
            best_v, best_k = v, k
    if len(best_k) < MIN_BANDS or len(best_k) < 0.6 * len(lines):
        return None
    xa, xb = _median([ln[2] for ln in best_k]), _median([ln[3] for ln in best_k])
    xs = xa + int(STRIP_FROM * (xb - xa))
    if xb - xs < 30:
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
    """the tick stacks of one look: [{"lines", "twinned", "share", "verdict", "rect", ...}], biggest first; [] when there is none. ValueError: not a PPM"""
    w, h, pix = parse_ppm(data)
    L = []
    for y in range(h):
        r = pix[y * w * 3:(y + 1) * w * 3]
        L.append([(a + b + c) // 3 for a, b, c in zip(r[0::3], r[1::3], r[2::3])])
    out = []
    for st in _find_stacks(L, w, h):
        r = _analyze_stack(pix, w, h, st, tol)
        if r:
            out.append(r)
    out.sort(key=lambda r: -r["lines"])
    return out


_ORDER = {"broken": 0, "unclear": 1, "clean": 2}


def verdict_files(paths, tol=TWIN_TOL):
    """the look files of one rung (its pipes): {"verdict": clean|broken|unclear|none, "text": one line for the triage, "stacks": [...]}. The worst stack
    decides: broken text anywhere on the screen is broken text (a window Terminal restored from its saved state shows static text, drawn clean, next
    to the live one). `none` also when no file could be read; the text says which."""
    stacks, errs = [], []
    for p in paths:
        try:
            with open(p, "rb") as f:
                for r in analyze_ppm(f.read(), tol):
                    r["file"] = p
                    stacks.append(r)
        except (OSError, ValueError) as e:
            errs.append("%s: %s" % (p.replace("\\", "/").rsplit("/", 1)[-1], e))
    if not stacks:
        why = "no look could be read (%s)" % "; ".join(errs)[:200] if errs and len(errs) == len(paths) else \
              "no stack of >= %d tick lines in the look%s" % (MIN_BANDS, "" if len(paths) == 1 else "s")
        return {"verdict": "none", "text": why, "stacks": []}
    worst = min(stacks, key=lambda r: (_ORDER[r["verdict"]], r["share"]))
    text = "%s: %d of %d tick lines have a twin (%.0f %%)" % (worst["verdict"].upper(), worst["twinned"], worst["lines"], 100.0 * worst["share"])
    if len(stacks) > 1:
        text += "; %d tick stack(s) on the screens: %s" % (len(stacks), ", ".join("%d/%d %s" % (r["twinned"], r["lines"], r["verdict"]) for r in stacks[:4]))
    if errs:
        text += "; unreadable: " + "; ".join(errs)[:120]
    return {"verdict": worst["verdict"], "text": text, "stacks": stacks}
