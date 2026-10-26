"""tools/lookimg.py: the scanout looks as PPM and PNG (pure Python, no PIL): encode / decode round trips on odd sizes, a PNG decoder against a reference filter
implementation written here (all five filter types, grey / grey+alpha / RGB / RGBA), broken files, the .ppm <-> .png name fallback, the verified
PPM -> PNG conversion `rdna3vm result` uses, and a 1280x720 speed bound. With PIL on the machine the PNGs are also cross-checked against it (PIL is optional).
Run: python3 -I -B tests/test_lookimg.py"""
import glob
import os
import random
import struct
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import lookimg as L  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def noise(w, h, seed):
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(w * h * 3))


def smooth(w, h):
    """a picture with long runs and gradients, the way a desktop is"""
    return bytes((x * 3 + y * 5 + c * 40) & 255 if (x // 7 + y // 5) % 3 else 17 * c for y in range(h) for x in range(w) for c in range(3))


# ---- the reference PNG writer: forward filters straight from the PNG specification, one per-byte loop each
def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def ref_filter(ft, row, prev, bpp):
    if ft > 4:      # not a filter type: the byte only has to be wrong, the row stays as it is
        return bytes(row)
    out = bytearray()
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[ft]
        out.append((x - pred) & 255)
    return bytes(out)


def make_png(w, h, pixels, ctype, filters, interlace=0, depth=8, crc_bad=False):
    chans = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    stride = w * chans
    raw, prev = b"", bytes(stride)
    for y in range(h):
        row = pixels[y * stride:(y + 1) * stride]
        ft = filters[y % len(filters)]
        raw += bytes([ft]) + ref_filter(ft, row, prev, chans)
        prev = row

    def chunk(t, b):
        crc = zlib.crc32(t + b) & 0xffffffff
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", crc ^ (1 if crc_bad and t == b"IDAT" else 0))
    return (L.PNG_MAGIC + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def raises(fn, *a):
    try:
        fn(*a)
    except ValueError:
        return True
    return False


# ---- bytewise arithmetic against the plain loops
r = random.Random(4)
for n in (1, 2, 3, 5, 100, 3840):
    a, b = bytes(r.randrange(256) for _ in range(n)), bytes(r.randrange(256) for _ in range(n))
    check(L._sub_bytes(a, b) == bytes((x - y) & 255 for x, y in zip(a, b)), "_sub_bytes %d" % n)
    check(L._add_bytes(a, b) == bytes((x + y) & 255 for x, y in zip(a, b)), "_add_bytes %d" % n)
edge = bytes([0, 255, 128, 127, 1, 0, 255]), bytes([255, 0, 128, 129, 255, 0, 255])
check(L._sub_bytes(*edge) == bytes((x - y) & 255 for x, y in zip(*edge)), "_sub_bytes edge values")
check(L._add_bytes(*edge) == bytes((x + y) & 255 for x, y in zip(*edge)), "_add_bytes edge values")

# ---- PPM
w, h, px = L.decode_ppm(b"P6\n# a comment\n4 2\n255\n" + bytes(range(24)) + b"trailing")
check((w, h, px) == (4, 2, bytes(range(24))), "decode_ppm with a comment and trailing bytes")
for bad, why in ((b"P5\n1 1\n255\n\0", "P5"), (b"P6\n4 2\n255\n\0\0", "truncated"), (b"P6\n4 2\n65535\n" + bytes(48), "16 bit"), (b"", "empty"), (b"P6\n0 2\n255\n", "zero width")):
    check(raises(L.decode_ppm, bad), "decode_ppm accepted %s" % why)
check(L.decode(L.encode_ppm(3, 2, bytes(range(18)))) == (3, 2, bytes(range(18))), "decode() takes a PPM")

# ---- encode -> decode, odd sizes (a row of one pixel, a single row, widths that are not a multiple of anything)
for (w, h) in ((1, 1), (1, 7), (2, 1), (3, 3), (5, 4), (7, 9), (65, 3), (127, 33), (321, 17)):
    for name, pix in (("noise", noise(w, h, w * 31 + h)), ("smooth", smooth(w, h)), ("black", bytes(w * h * 3))):
        png = L.encode_png(w, h, pix)
        check(L.decode_png(png) == (w, h, pix) and L.decode(png) == (w, h, pix), "round trip %dx%d %s" % (w, h, name))
check(raises(L.encode_png, 4, 4, bytes(10)), "encode_png: wrong byte count")
check(len(L.encode_png(256, 64, smooth(256, 64))) < 256 * 64 * 3 // 2, "smooth pictures compress")

# ---- the decoder against the reference writer: every filter type, alone and mixed, every colour type, odd widths
for ctype, chans in ((2, 3), (6, 4), (0, 1), (4, 2)):
    for (w, h) in ((1, 5), (3, 6), (7, 11), (13, 5), (64, 8)):
        pixels = bytes(random.Random(w * 100 + h + ctype).randrange(256) for _ in range(w * h * chans))
        want = {2: pixels, 6: b"".join(pixels[i:i + 3] for i in range(0, len(pixels), 4)),
                0: b"".join(bytes([v]) * 3 for v in pixels),
                4: b"".join(bytes([pixels[i]]) * 3 for i in range(0, len(pixels), 2))}[ctype]
        for filters in ([0], [1], [2], [3], [4], [0, 1, 2, 3, 4], [4, 3, 2, 1, 0, 4]):
            got = L.decode_png(make_png(w, h, pixels, ctype, filters))
            check(got == (w, h, want), "decode_png colour type %d %dx%d filters %r" % (ctype, w, h, filters))

# ---- broken files: an error, never garbage and never an exception of another kind
good = make_png(8, 8, noise(8, 8, 1), 2, [0, 1, 2, 3, 4])
check(raises(L.decode_png, good[:-20]), "truncated PNG")
check(raises(L.decode_png, good[:40]), "PNG cut inside IDAT")
check(raises(L.decode_png, make_png(8, 8, noise(8, 8, 1), 2, [0], crc_bad=True)), "bad CRC")
check(raises(L.decode_png, make_png(8, 8, bytes(8 * 8 * 3), 2, [0], interlace=1)), "interlaced PNG is refused")
check(raises(L.decode_png, make_png(8, 8, bytes(8 * 8 * 3), 2, [0], depth=16)), "16 bit PNG is refused")
check(raises(L.decode_png, b"\x89PNG\r\n\x1a\n"), "PNG without chunks")
check(raises(L.decode_png, L.PNG_MAGIC + struct.pack(">I4s", 9, b"IHDR")), "PNG with a chunk longer than the file")
check(raises(L.decode, b"GIF89a"), "decode() of another format")
check(raises(L.decode_png, make_png(8, 8, bytes(8 * 8 * 3), 2, [7])), "bad filter type")

# ---- the names: .ppm <-> .png, one file per look, the PPM wins
tmp = tempfile.mkdtemp(prefix="lookimg")
pix = smooth(16, 8)
base = os.path.join(tmp, "rep-scanoutladder-baselineh-pipe0")
open(base + ".png", "wb").write(L.encode_png(16, 8, pix))
check(L.read_look(base + ".ppm") == (16, 8, pix), "read_look(.ppm) finds the .png")
check(L.read_look(base + ".png") == (16, 8, pix), "read_look(.png)")
try:
    L.read_look(os.path.join(tmp, "nope.ppm"))
    check(False, "read_look of nothing")
except OSError:
    pass
open(os.path.join(tmp, "rep-scanoutladder-baselineh-pipe1.ppm"), "wb").write(L.encode_ppm(16, 8, pix))
open(os.path.join(tmp, "rep-scanoutladder-baselineh-pipe1.png"), "wb").write(b"not looked at")
open(os.path.join(tmp, "rep-scanoutladder-baselineh2-pipe0.png"), "wb").write(b"x")
files = [os.path.basename(f) for f in L.look_files(os.path.join(tmp, "rep-scanoutladder-baselineh-pipe*"))]
check(files == ["rep-scanoutladder-baselineh-pipe0.png", "rep-scanoutladder-baselineh-pipe1.ppm"], "look_files: one per look, the PPM wins, h2 is not h: %r" % (files,))

# ---- the verified conversion of `rdna3vm result` (no xz): every PPM becomes a PNG with the same pixels, the PPM goes; a bad PPM stays and is named
d = os.path.join(tmp, "conv")
os.makedirs(d)
pics = {"a-pipe0": noise(40, 30, 1), "a-pipe1": smooth(40, 30)}
for k, v in pics.items():
    open(os.path.join(d, k + ".ppm"), "wb").write(L.encode_ppm(40, 30, v))
open(os.path.join(d, "broken-pipe0.ppm"), "wb").write(b"P6\n40 30\n255\nshort")
open(os.path.join(d, "notes.txt"), "w").write("keep")
n, kept, before, after, problems = L.to_png_dir(d)
check((n, kept) == (2, 1) and len(problems) == 1 and "broken-pipe0.ppm" in problems[0], "to_png_dir counts: %r" % ((n, kept, problems),))
check(sorted(os.listdir(d)) == ["a-pipe0.png", "a-pipe1.png", "broken-pipe0.ppm", "notes.txt"], "to_png_dir leaves: %r" % (sorted(os.listdir(d)),))
check(all(L.read_look(os.path.join(d, k + ".ppm")) == (40, 30, v) for k, v in pics.items()), "converted looks read back through their .ppm names")
check(not [f for f in os.listdir(d) if f.endswith(".part")], "no .part files left")
check(L.main(["lookimg.py", "--to-png", d]) == 0, "main --to-png")
# a PNG that does not decode to the PPM's pixels is not accepted: the PPM stays (the check that the conversion cannot lose a look)
real_encode = L.encode_png
L.encode_png = lambda w, h, rgb, level=9: real_encode(w, h, bytes(rgb[:-1]) + bytes([rgb[-1] ^ 1]), level)
p = os.path.join(tmp, "x-pipe0.ppm")
open(p, "wb").write(L.encode_ppm(8, 8, smooth(8, 8)))
check(raises(L.ppm_to_png, p, True) and os.path.exists(p) and not os.path.exists(p[:-4] + ".png"), "a conversion that changes a pixel keeps the PPM")
L.encode_png = real_encode

# ---- speed: 1280x720, the worst filters (a foreign encoder's Paeth and Average rows)
pixels = smooth(1280, 720)
for filters, bound in (([0], 3), ([2], 3), ([1], 3), ([4], 25), ([3], 25)):
    png = make_png(1280, 720, pixels, 2, filters) if filters in ([0], [2], [1]) else None
    if png is None:
        # the reference writer is slow for a 1280x720 Paeth: 60 rows of the image are enough to time the per-byte loops
        png = make_png(1280, 60, pixels[:1280 * 60 * 3], 2, filters)
        t = time.time()
        got = L.decode_png(png)
        dt = (time.time() - t) * 12
        check(got == (1280, 60, pixels[:1280 * 60 * 3]), "paeth/average decode of 60 rows %r" % filters)
    else:
        t = time.time()
        got = L.decode_png(png)
        dt = time.time() - t
        check(got == (1280, 720, pixels), "decode 1280x720 filters %r" % filters)
    check(dt < bound, "1280x720 decode with filters %r takes %.1f s (bound %d)" % (filters, dt, bound))
t = time.time()
png = L.encode_png(1280, 720, pixels)
check(time.time() - t < 6, "encode 1280x720 takes %.1f s" % (time.time() - t))

# ---- PIL, when it is there (not needed anywhere else)
try:
    import io
    from PIL import Image
except ImportError:
    Image = None
if Image is not None:
    pix = noise(33, 21, 9)
    check(Image.open(io.BytesIO(L.encode_png(33, 21, pix))).tobytes() == pix, "PIL reads our PNG")
    b = io.BytesIO()
    Image.frombytes("RGB", (33, 21), pix).save(b, "PNG")
    check(L.decode_png(b.getvalue()) == (33, 21, pix), "we read PIL's PNG")
    b = io.BytesIO()
    Image.frombytes("RGB", (1280, 720), pixels).save(b, "PNG", optimize=True)
    t = time.time()
    check(L.decode_png(b.getvalue()) == (1280, 720, pixels), "we read PIL's optimized 1280x720 PNG")
    check(time.time() - t < 5, "PIL's 1280x720 PNG takes %.1f s" % (time.time() - t))

# ---- a real look (the card run reports are not committed)
for rep in ("2026-10-26b", "2026-10-26"):
    ppms = sorted(glob.glob(os.path.join(os.environ.get("RDNA3_REPORTS") or os.path.join(ROOT, "reports"), rep, "rdna3-result", "*scanoutladder-baselineh-pipe0.ppm")))
    if ppms:
        w, h, px = L.read_look(ppms[0])
        png = L.encode_png(w, h, px)
        check((w, h) == (1280, 720) and L.decode_png(png) == (w, h, px) and len(png) < len(px) * 0.7, "real look %s" % rep)
        print("test_lookimg: the real %s look: %d -> %d bytes" % (rep, len(px), len(png)))
        break
else:
    print("test_lookimg: (no real look on this machine: skipped)")

if fails:
    print("test_lookimg: FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("test_lookimg: ok")
