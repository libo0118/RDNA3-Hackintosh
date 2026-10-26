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
import lookimg  # noqa: E402

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

# ---- the second look: only the LIVE window counts (the text script prints a line a second; a restored / frozen window does not change)
def look(path, windows, seed=3):
    open(path, "wb").write(render(windows, seed=seed))


a1, a2, b1, b2 = (os.path.join(tmp, n) for n in ("a1-pipe0.ppm", "a2-pipe0.ppm", "b1-pipe0.ppm", "b2-pipe0.ppm"))
static_clean = Window(300, 200, 80, 16, tick_lines(14, seed=2, first=7))
# a live BROKEN window with a static CLEAN one beside it (the 25f picture): the verdict is the live one's, the static one is named and ignored
look(a1, [Window(1100, 330, 80, 24, tick_lines(22, broken_from=37, seed=5)), static_clean])
look(a2, [Window(1100, 330, 80, 24, tick_lines(22, broken_from=37, seed=6, first=3)), static_clean])
v = gv.verdict_files([a1], later=[a2])
check(v["verdict"] == "broken" and len(v["stacks"]) == 1 and len(v["static"]) == 1 and "STATIC" in v["text"] and "in the live window" in v["text"],
      "live broken + static clean: %r" % (v["text"],))
check(v["static"][0]["verdict"] == "clean" and v["stacks"][0]["live"] and v["stacks"][0]["changed"] >= gv.LIVE_MIN, "the static one is the clean one, the live one changed a lot")
# a live CLEAN window with a static BROKEN one: clean (the restored text of another rung does not decide)
look(a1, [Window(1100, 330, 80, 24, tick_lines(22, seed=5)), Window(300, 200, 80, 16, tick_lines(14, broken_from=37, seed=2, first=7))])
look(a2, [Window(1100, 330, 80, 24, tick_lines(22, seed=6, first=3)), Window(300, 200, 80, 16, tick_lines(14, broken_from=37, seed=2, first=7))])
v = gv.verdict_files([a1], later=[a2])
check(v["verdict"] == "clean" and len(v["static"]) == 1, "live clean + static broken: %r" % (v["text"],))
# the window's lines grow downwards between the looks (the window is not full yet): still live
look(a1, [Window(1100, 330, 80, 24, tick_lines(10, seed=5))])
look(a2, [Window(1100, 330, 80, 24, tick_lines(13, seed=5))])
v = gv.verdict_files([a1], later=[a2])
check(v["verdict"] == "clean" and not v["static"], "a stack that grew downwards is live: %r" % (v["text"],))
# nothing changes (a frozen screen, only a restored window): none, with the reason
look(a1, [static_clean]); look(a2, [static_clean])
v = gv.verdict_files([a1], later=[a2])
check(v["verdict"] == "none" and "no LIVE tick stack" in v["text"] and len(v["static"]) == 1, "only static text: %r" % (v["text"],))
look(a1, [Window(1100, 330, 80, 24, tick_lines(22, seed=5)), static_clean]); look(a2, [Window(1100, 330, 80, 24, tick_lines(22, seed=6, first=3)), static_clean])
v = gv.verdict_files([a1], later=[a2])
check("pipes judged: 0" in v["text"] and v["pipes"] == ["0"], "the pipes judged are listed: %r" % (v["text"],))
# something that changes BESIDE a static stack (a clock, another window in its rows) does not make it live: only its own columns count
look(a1, [static_clean, Window(1100, 200, 40, 6, ragged_lines(5, seed=3))]); look(a2, [static_clean, Window(1100, 200, 40, 6, ragged_lines(5, seed=4))])
v = gv.verdict_files([a1], later=[a2])
check(v["verdict"] == "none" and len(v["static"]) == 1, "a change beside the stack does not make it live: %r" % (v["text"],))
# no tick stack at all, and a missing / differently sized second look
look(a1, [Window(1100, 330, 80, 24, ragged_lines(22))]); look(a2, [Window(1100, 330, 80, 24, ragged_lines(22, seed=12))])
v = gv.verdict_files([a1], later=[a2])
check(v["verdict"] == "none" and "no stack of >= 8" in v["text"], "no tick lines: %r" % (v["text"],))
v = gv.verdict_files([a1], later=[])
check(v["verdict"] == "none" and "no second look of this pipe" in v["text"], "no second look: %r" % (v["text"],))
open(b1, "wb").write(render([static_clean], w=1280, h=720)); open(b2, "wb").write(render([static_clean], w=640, h=360))
v = gv.verdict_files([b1], later=[b2])
check(v["verdict"] == "none" and "differ in size" in v["text"], "looks of two sizes: %r" % (v["text"],))
# two pipes: each is paired with its own second look
p0a, p0b, p1a, p1b = (os.path.join(tmp, n) for n in ("p0a-pipe0.ppm", "p0b-pipe0.ppm", "p1a-pipe1.ppm", "p1b-pipe1.ppm"))
look(p0a, [static_clean]); look(p0b, [static_clean])
look(p1a, [Window(1100, 330, 80, 24, tick_lines(22, seed=5))]); look(p1b, [Window(1100, 330, 80, 24, tick_lines(22, seed=6, first=3))])
v = gv.verdict_files([p0a, p1a], later=[p1b, p0b])
check(v["verdict"] == "clean" and len(v["stacks"]) == 1 and len(v["static"]) == 1, "two pipes: %r" % (v["text"],))

# ---- PPM parsing
for bad, why in ((b"P5\n1 1\n255\n\0", "not P6"), (b"P6\n1280 720\n255\n\0\0", "truncated"), (b"P6\n1280 720\n65535\n" + bytes(10), "16 bit"), (b"", "empty")):
    try:
        gv.parse_ppm(bad)
        check(False, "parse_ppm accepted %s" % why)
    except ValueError:
        pass
w, h, pix = gv.parse_ppm(b"P6\n# a comment\n64 64\n255\n" + bytes(64 * 64 * 3))
check((w, h, len(pix)) == (64, 64, 64 * 64 * 3), "parse_ppm with a comment")

# ---- the same looks as PNG (`rdna3vm result` without xz packs them so): the same verdicts, through the .png names and through the names the card wrote
def png_of(ppm_bytes):
    return lookimg.encode_png(*lookimg.decode_ppm(ppm_bytes))


def put(path, windows, seed=3, png=False):
    data = render(windows, seed=seed)
    open(path, "wb").write(png_of(data) if png else data)


live1 = [Window(1100, 330, 80, 24, tick_lines(22, broken_from=37, seed=5)), static_clean]
live2 = [Window(1100, 330, 80, 24, tick_lines(22, broken_from=37, seed=6, first=3)), static_clean]
raw = render(live1)
check(gv.analyze_ppm(png_of(raw)) == gv.analyze_ppm(raw), "analyze_ppm: a PNG gives the PPM's stacks")
check(gv.parse_ppm(png_of(raw)) == gv.parse_ppm(raw), "parse_ppm: a PNG gives the PPM's pixels")
for bad, why in ((png_of(raw)[:-30], "a truncated PNG"), (png_of(render([], w=32, h=32)), "a 32x32 PNG"), (b"GIF89a" + bytes(100), "a GIF")):
    try:
        gv.parse_ppm(bad)
        check(False, "parse_ppm accepted %s" % why)
    except ValueError:
        pass
q1, q2 = os.path.join(tmp, "q1-pipe0.png"), os.path.join(tmp, "q2-pipe0.png")
put(q1, live1, png=True); put(q2, live2, png=True)
v = gv.verdict_files([q1], later=[q2])
check(v["verdict"] == "broken" and len(v["stacks"]) == 1 and len(v["static"]) == 1 and v["pipes"] == ["0"], "PNG looks, live broken + static clean: %r" % (v["text"],))
put(q1, [Window(1100, 330, 80, 24, tick_lines(22, seed=5))], png=True)
v = gv.verdict_files([q1])
check(v["verdict"] == "clean", "one PNG look, clean: %r" % (v["text"],))
# one pipe a PPM, the other a PNG, and a look whose PPM is gone: the name the card wrote is read as the PNG
put(p0a, [static_clean]); put(p0b, [static_clean])
put(p1a[:-4] + ".png", [Window(1100, 330, 80, 24, tick_lines(22, seed=5))], png=True); put(p1b[:-4] + ".png", [Window(1100, 330, 80, 24, tick_lines(22, seed=6, first=3))], png=True)
v = gv.verdict_files([p0a, p1a], later=[p1b, p0b])
check(v["verdict"] == "clean" and len(v["stacks"]) == 1 and len(v["static"]) == 1, "pipe 0 PPM + pipe 1 PNG: %r" % (v["text"],))
v = gv.verdict_files([os.path.join(tmp, "z-pipe0.png")])
check(v["verdict"] == "none" and "no look could be read" in v["text"], "a PNG that is not there: %r" % (v["text"],))
open(q1, "wb").write(png_of(render([static_clean]))[:100])
v = gv.verdict_files([q1])
check(v["verdict"] == "none" and "unreadable" in v["text"] or "no look could be read" in v["text"], "a cut PNG is named, not an exception: %r" % (v["text"],))

# ---- the card run 2026-10-26b text: ONE fixed colour (bright white) and a line of mixed glyphs after every 4th tick line. The mixed line ends differently, so
# it is not a tick line; the ticks around it are still one stack (nothing in the picture changes the verdict). The window can also start with a mixed line.
MIXED = "          ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 !#$%&()*+,-./:;<=>?@[]^_{|}~"
WHITE = (250, 250, 250)


def with_mixed(n, **kw):
    out = []
    for k, line in enumerate(tick_lines(n, colour=WHITE, **kw)):
        out.append(line)
        if (k + 1) % 4 == 0:
            out.append((MIXED, WHITE, {}))
    return out


for n, skip in ((14, 0), (18, 1), (22, 3)):
    st = analyze([Window(100, 100, 80, 30, with_mixed(n)[skip:])], seed=4)
    check(len(st) == 1 and st[0]["verdict"] == "clean" and st[0]["lines"] >= n - 4 - 1, "fixed colour + mixed lines (%d ticks, skip %d): %r" % (n, skip, [(s["lines"], s["twinned"], s["verdict"]) for s in st]))
st = analyze([Window(100, 100, 80, 30, with_mixed(22, broken_from=37))], seed=4)
check(len(st) == 1 and st[0]["verdict"] == "broken", "fixed colour + mixed lines, broken: %r" % ([(s["lines"], s["twinned"], s["verdict"]) for s in st],))

# ---- the card run 2026-10-25f's looks, when the report is here (it is not committed): live windows broken, the restored ones clean
REP = os.environ.get("RDNA3_REPORT_25F") or os.path.join(os.environ.get("RDNA3_REPORTS") or os.path.join(ROOT, "reports"), "2026-10-25f", "rdna3-result")
looks = {r: sorted(glob.glob(os.path.join(REP, "*-scanoutladder-%sh-pipe*.ppm" % r))) for r in ("baseline", "radv", "sync", "managed", "alignguard", "paranoid", "loadbarrier")}
if all(looks.values()):
    for r, files in looks.items():
        v = gv.verdict_files(files)
        check(v["verdict"] == "broken", "2026-10-25f %s: %s" % (r, v["text"]))
    v = gv.verdict_files(looks["paranoid"])
    check(any(s["verdict"] == "clean" for s in v["stacks"]) and any(s["verdict"] == "broken" for s in v["stacks"]),
          "2026-10-25f paranoid: the live window is broken, the window Terminal restored from the previous rung is clean: %r" % (v["text"],))
    # the same look as PNG (what `rdna3vm result` sends without xz): the same stacks, the same verdict
    ppm = open(looks["baseline"][0], "rb").read()
    check(gv.analyze_ppm(png_of(ppm)) == gv.analyze_ppm(ppm), "2026-10-25f baseline: the PNG gives the PPM's stacks")
    print("test_glyph_verdict: the real 2026-10-25f looks checked")
else:
    print("test_glyph_verdict: (the 2026-10-25f looks are not on this machine: the real-data case is skipped)")

# ---- the card run 2026-10-26b, when the report is here: every rung's looks have NO tick Terminal on them (Chess opened on top of it; the one Terminal that is
# visible is the progress window, a log). The verdict is `none` with the hint to look at the pictures, never clean or broken; its PNG copies say the same.
REP26B = os.path.join(os.environ.get("RDNA3_REPORTS") or os.path.join(ROOT, "reports"), "2026-10-26b", "rdna3-result")
base26 = sorted(glob.glob(os.path.join(REP26B, "*scanoutladder-baselineh-pipe*.ppm")))
later26 = sorted(glob.glob(os.path.join(REP26B, "*scanoutladder-baselineh2-pipe*.ppm")))
if base26 and later26:
    v = gv.verdict_files(base26, later=later26)
    check(v["verdict"] == "none" and "no stack of >= 8" in v["text"] and "behind Chess" in v["text"], "2026-10-26b baseline: no Terminal on the screen: %r" % (v["text"],))
    bindless = sorted(glob.glob(os.path.join(REP26B, "*scanoutladder-no-bindlessh-pipe*.ppm")))
    check(bindless and gv.verdict_files(bindless)["verdict"] == "none", "2026-10-26b no-bindless: the progress window is a log, not tick lines")
    pngs = []
    for f in base26:
        w_, h_, px_ = lookimg.read_look(f)
        pngs.append(os.path.join(tmp, os.path.basename(f)[:-4] + ".png"))
        open(pngs[-1], "wb").write(lookimg.encode_png(w_, h_, px_))
    check(gv.verdict_files(pngs)["text"] == gv.verdict_files(base26)["text"], "2026-10-26b baseline: the PNG copies give the same text")
    print("test_glyph_verdict: the real 2026-10-26b looks checked")
if fails:
    print("test_glyph_verdict: FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("test_glyph_verdict: ok")
