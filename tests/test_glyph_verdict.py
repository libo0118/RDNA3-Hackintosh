"""tools/glyph_verdict.py (DRIVER-PLAN N50, the ladder's automatic glyph verdict) on synthetic looks.

The looks are made the way the card's look is: a 1920x1080 scanout sampled nearest-neighbour down to 1280x720 (source pixel = floor(1.5 * output
pixel): every third source row and column is dropped, so tick lines whose top rows differ by a multiple of 3 are sampled identically and the others
are not - three classes), a noisy "wallpaper", a Terminal-like window with tick lines of 14 source rows that end in the same text (the time stamp
at the left is different on each line) in a colour per line, and other windows. Cases: drawn right = clean, random strokes in the right half of every
line = broken (also when only some cells are hit), a window of changing text and a tiny stack = none (never mistaken for tick lines), a clean
restored window next to a broken live one = broken (the worst stack decides), PPM parsing errors, and the real 2026-10-25f looks when the report is
on this machine (baseline, paranoid: broken; the restored windows in them: clean).
Run: python3 -I -B tests/test_glyph_verdict.py"""
import glob
import os
import random
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import glyph_verdict as gv  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


BG = (28, 28, 28)
COLOURS = [(217, 63, 46), (40, 190, 60), (200, 190, 40), (90, 70, 240), (210, 60, 200), (16, 189, 201), (240, 240, 240)]
CELL_W, CELL_H, PITCH = 7, 10, 14   # source pixels: a glyph bitmap is 6 x 10 in a cell 7 wide, a line is 14 rows
_rng = random.Random(7)
_FONT = {}


def glyph(ch):
    if ch not in _FONT:
        r = random.Random(ord(ch) * 7919)
        _FONT[ch] = [[1 if r.random() < 0.45 else 0 for _ in range(6)] for _ in range(CELL_H)] if ch != " " else [[0] * 6 for _ in range(CELL_H)]
    return _FONT[ch]


def noise_ppm(w=1280, h=720, seed=3):
    r = random.Random(seed)
    base = bytes(r.randrange(40, 200) for _ in range(w * 3))
    # smooth-ish: every row is the base row shifted and dimmed a little, so there are edges but no regular line structure
    out = bytearray()
    for y in range(h):
        k = (y * 37) % w
        row = base[k * 3:] + base[:k * 3]
        out += bytes((c * (180 + (y % 7)) // 255) for c in row)
    return out


class Window:
    """a text window in source coordinates; lines: [(text, colour, broken_cells)] drawn at PITCH rows"""
    def __init__(self, x0, y0, cols, rows, lines):
        self.x0, self.y0, self.w, self.h = x0, y0, cols * CELL_W + 12, rows * PITCH + 12
        self.lines = lines

    def pixel(self, sx, sy):
        lx, ly = sx - self.x0, sy - self.y0
        if not (0 <= lx < self.w and 0 <= ly < self.h):
            return None
        k, ry = divmod(ly - 6, PITCH)
        if ly < 6 or k >= len(self.lines) or ry >= CELL_H:
            return BG
        text, colour, hit = self.lines[k]
        c, rx = divmod(lx - 6, CELL_W)
        if lx < 6 or c >= len(text) or rx >= 6:
            return BG
        bits = hit.get(c)
        if bits is not None:
            return colour if bits[ry][rx] else BG
        return colour if glyph(text[c])[ry][rx] else BG


def render(windows, w=1280, h=720, seed=3):
    pix = noise_ppm(w, h, seed)
    for win in windows:
        for yo in range(h):
            sy = int(yo * 1.5)
            if not (win.y0 <= sy < win.y0 + win.h):
                continue
            for xo in range(w):
                sx = int(xo * 1.5)
                if win.x0 <= sx < win.x0 + win.w:
                    p = win.pixel(sx, sy)
                    if p is not None:
                        o = (yo * w + xo) * 3
                        pix[o:o + 3] = bytes(p)
    return b"P6\n%d %d\n255\n" % (w, h) + bytes(pix)


TAIL = "  rdna3 ladder tick  abcdefghijklmnopqrstuvwxyz"


def tick_lines(n, broken_from=None, hit_prob=0.8, seed=5, tail=TAIL, colour=None, first=0):
    """n tick lines; broken_from = the first cell (in the line) whose glyph is replaced by random strokes (different on each line)"""
    r = random.Random(seed)
    out = []
    for k in range(n):
        text = " %02d:%02d:%02d%s" % (13, 58, 42 + first + k, tail)
        hit = {}
        if broken_from is not None:
            for c in range(broken_from, len(text)):
                if r.random() < hit_prob:
                    hit[c] = [[1 if r.random() < 0.4 else 0 for _ in range(6)] for _ in range(CELL_H)]
        out.append((text, colour or COLOURS[k % len(COLOURS)], hit))
    return out


def ragged_lines(n, seed=11):
    r = random.Random(seed)
    out = []
    for k in range(n):
        text = "".join(r.choice("abcdefghijklmnopqrstuvwxyz0123456789 ") for _ in range(r.randrange(20, 58)))
        out.append((text, COLOURS[k % len(COLOURS)], {}))
    return out


def analyze(windows, **kw):
    return gv.analyze_ppm(render(windows, **kw))


# ---- drawn right: clean (the lines are in three row classes of the sampler, several in each)
st = analyze([Window(1100, 330, 80, 24, tick_lines(22))])
check(len(st) == 1 and st[0]["verdict"] == "clean" and st[0]["lines"] >= 20 and st[0]["share"] >= 0.9, "clean tick lines: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))
# the same in one colour (the new text script)
st = analyze([Window(1100, 330, 80, 24, tick_lines(22, colour=(250, 250, 250)))])
check(len(st) == 1 and st[0]["verdict"] == "clean", "clean, one colour: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))
# the window somewhere else, a different line count, a light background is not assumed
st = analyze([Window(200, 100, 80, 16, tick_lines(14, seed=9))], seed=8)
check(len(st) == 1 and st[0]["verdict"] == "clean" and st[0]["lines"] >= 12, "another place and size: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))

# ---- broken: random strokes in the cells after the 8th letter of the alphabet's start (every line different)
st = analyze([Window(1100, 330, 80, 24, tick_lines(22, broken_from=37))])
check(len(st) == 1 and st[0]["verdict"] == "broken" and st[0]["share"] <= 0.3, "broken tick lines: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))
st = analyze([Window(1100, 330, 80, 24, tick_lines(22, broken_from=37, hit_prob=0.35))])
check(len(st) == 1 and st[0]["verdict"] == "broken", "broken with a third of the cells hit: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))

# ---- not tick lines: a window of changing text, a short stack
st = analyze([Window(1100, 330, 80, 24, ragged_lines(22))])
check(st == [], "a window of changing text is not a tick stack: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))
st = analyze([Window(1100, 330, 80, 6, tick_lines(5))])
check(st == [], "five lines are not a stack")
check(gv.verdict_files([]) ["verdict"] == "none", "no files: none")

# ---- a clean restored window beside a broken live one: broken; both clean: clean
tmp = tempfile.mkdtemp(prefix="glyphv")
pa, pb = os.path.join(tmp, "a.ppm"), os.path.join(tmp, "b.ppm")
open(pa, "wb").write(render([Window(1100, 330, 80, 24, tick_lines(22, broken_from=37))]))
open(pb, "wb").write(render([Window(300, 200, 80, 20, tick_lines(18, seed=2, first=3))], seed=4))
v = gv.verdict_files([pa, pb])
check(v["verdict"] == "broken" and len(v["stacks"]) == 2 and "clean" in v["text"], "worst of two stacks: %r" % (v["text"],))
open(pa, "wb").write(render([Window(1100, 330, 80, 24, tick_lines(22))]))
v = gv.verdict_files([pa, pb])
check(v["verdict"] == "clean", "both clean: %r" % (v["text"],))
v = gv.verdict_files([pa, os.path.join(tmp, "nope.ppm")])
check(v["verdict"] == "clean" and "unreadable" in v["text"], "one unreadable look is named, the other still counts: %r" % (v["text"],))
v = gv.verdict_files([os.path.join(tmp, "nope.ppm")])
check(v["verdict"] == "none" and "no look could be read" in v["text"], "nothing readable: %r" % (v["text"],))

# ---- PPM parsing
for bad, why in ((b"P5\n1 1\n255\n\0", "not P6"), (b"P6\n1280 720\n255\n\0\0", "truncated"), (b"P6\n1280 720\n65535\n" + bytes(10), "16 bit"), (b"", "empty")):
    try:
        gv.parse_ppm(bad)
        check(False, "parse_ppm accepted %s" % why)
    except ValueError:
        pass
w, h, pix = gv.parse_ppm(b"P6\n# a comment\n64 64\n255\n" + bytes(64 * 64 * 3))
check((w, h, len(pix)) == (64, 64, 64 * 64 * 3), "parse_ppm with a comment")

# ---- the card run 2026-10-25f's looks, when the report is here (it is not committed): live windows broken, the restored ones clean
REP = os.environ.get("RDNA3_REPORT_25F") or os.path.join(ROOT, "reports", "2026-10-25f", "rdna3-result")
looks = {r: sorted(glob.glob(os.path.join(REP, "*-scanoutladder-%sh-pipe*.ppm" % r))) for r in ("baseline", "radv", "sync", "managed", "alignguard", "paranoid", "loadbarrier")}
if all(looks.values()):
    for r, files in looks.items():
        v = gv.verdict_files(files)
        check(v["verdict"] == "broken", "2026-10-25f %s: %s" % (r, v["text"]))
    v = gv.verdict_files(looks["paranoid"])
    check(any(s["verdict"] == "clean" for s in v["stacks"]) and any(s["verdict"] == "broken" for s in v["stacks"]),
          "2026-10-25f paranoid: the live window is broken, the window Terminal restored from the previous rung is clean: %r" % (v["text"],))
    print("test_glyph_verdict: the real 2026-10-25f looks checked")
else:
    print("test_glyph_verdict: (the 2026-10-25f looks are not on this machine: the real-data case is skipped)")

if fails:
    print("test_glyph_verdict: FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("test_glyph_verdict: ok")
