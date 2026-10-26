"""The scanout looks (<report>-scanout<look>-pipe<N>.ppm / .png) for the host's triage: read either format, write PNG. Pure Python (zlib, struct, int
arithmetic on bytes): no PIL, no numpy, so `python3 -I` on any machine reads them. Used by glyph_verdict.py, triage_glyphs.py, triage_bisect.py,
triage_reports.py and `rdna3vm result`.

The card writes binary PPMs (P6, 8 bit): 6.2 MB for a native 1920x1080 hi-res look (2.7 MB for the 1280x720 ones of kexts before 2026-10-26), 0.17 MB
for a 320-wide one. `rdna3vm result` keeps them raw when it can pack with xz (the looks of one run are the same desktop, xz finds that: card run 2026-10-26c's
60 looks, 109 MB, go to 2.7 MB; the same at native size, 241 MB, to about 5.7 MB in 7 s) and turns them into PNGs (lossless, about half the size, a decode round trip is checked before
the PPM is dropped) when it can only gzip. Old reports keep their PPMs, so every reader takes both; `read_look(path)` also finds the other extension.

    python3 -I tools/lookimg.py --to-png DIR     every DIR/*.ppm -> DIR/*.png (verified), the PPM removed; one summary line, WARN lines for what stayed
    python3 -I tools/lookimg.py --info FILE...   size and format of looks

decode_png: 8-bit grey / grey+alpha / RGB / RGBA, not interlaced, all five filter types (the alpha channel is dropped; no palette: no tool of ours writes one).
Filters None / Up / Sub are vectorised, Average and Paeth run a Python loop per byte (about 2 s for 1280x720, 2.25 times that for 1920x1080: only foreign
encoders use them). Sizes up to 16384 each way."""
import glob
import os
import re
import struct
import sys
import zlib
from itertools import accumulate

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
MAX_DIM = 16384


# ---- bytewise arithmetic on equal-length byte strings as big integers (8 bits per lane, no carries between lanes)
_MASKS = {}


def _masks(n):
    m = _MASKS.get(n)
    if m is None:
        h = int.from_bytes(b"\x80" * n, "big")
        full = int.from_bytes(b"\xff" * n, "big")
        if len(_MASKS) > 64:
            _MASKS.clear()
        m = _MASKS[n] = (h, full ^ h, full)
    return m


def _add_bytes(a, b):
    """(a + b) mod 256 in every byte"""
    h, nh, _ = _masks(len(a))
    x, y = int.from_bytes(a, "big"), int.from_bytes(b, "big")
    return (((x & nh) + (y & nh)) ^ ((x ^ y) & h)).to_bytes(len(a), "big")


def _sub_bytes(a, b):
    """(a - b) mod 256 in every byte"""
    h, nh, full = _masks(len(a))
    x, y = int.from_bytes(a, "big"), int.from_bytes(b, "big")
    return ((((x | h) - (y & nh)) ^ ((x ^ (full ^ y)) & h)) & full).to_bytes(len(a), "big")


# ---- PPM
def decode_ppm(data):
    """(width, height, rgb bytes) of a binary PPM (P6, maxval 255; '#' comments allowed); ValueError otherwise. Trailing bytes are ignored."""
    m = re.match(rb"P6(?:\s|#[^\n]*\n)+(\d+)(?:\s|#[^\n]*\n)+(\d+)(?:\s|#[^\n]*\n)+(\d+)\s", data)
    if not m:
        raise ValueError("not a binary PPM (P6)")
    w, h, mx = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if mx != 255 or w < 1 or h < 1 or w > MAX_DIM or h > MAX_DIM:
        raise ValueError("unsupported PPM: %dx%d max %d" % (w, h, mx))
    px = data[m.end():m.end() + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError("truncated PPM pixels")
    return w, h, px


def encode_ppm(w, h, rgb):
    return b"P6\n%d %d\n255\n" % (w, h) + bytes(rgb)


# ---- PNG
def _chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)


def _unfilter_avg(row, prev, bpp):
    cur = bytearray(len(row))
    for i in range(min(bpp, len(row))):
        cur[i] = (row[i] + (prev[i] >> 1)) & 255
    for i in range(bpp, len(row)):
        cur[i] = (row[i] + ((cur[i - bpp] + prev[i]) >> 1)) & 255
    return cur


def _unfilter_paeth(row, prev, bpp):
    cur = bytearray(len(row))
    for i in range(min(bpp, len(row))):
        cur[i] = (row[i] + prev[i]) & 255          # a = c = 0: the predictor is b
    for i in range(bpp, len(row)):
        a, b, c = cur[i - bpp], prev[i], prev[i - bpp]
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        cur[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    return cur


def _unfilter_sub(row, bpp):
    cur = bytearray(len(row))
    for c in range(bpp):
        cur[c::bpp] = bytes(map((255).__and__, accumulate(row[c::bpp])))
    return cur


def decode_png(data):
    """(width, height, rgb bytes) of a PNG (8 bit grey / grey+alpha / RGB / RGBA, not interlaced); ValueError otherwise"""
    if data[:8] != PNG_MAGIC:
        raise ValueError("not a PNG")
    pos, ihdr, idat = 8, None, []
    while pos + 8 <= len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if len(body) != n or pos + 12 + n > len(data):
            raise ValueError("truncated PNG chunk %r" % tag)
        if zlib.crc32(tag + body) & 0xffffffff != struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0]:
            raise ValueError("bad CRC in PNG chunk %r" % tag)
        if tag == b"IHDR":
            if n != 13:
                raise ValueError("bad PNG IHDR length %d" % n)
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
        pos += 12 + n
    if ihdr is None or not idat:
        raise ValueError("PNG without IHDR or IDAT")
    w, h, depth, ctype, _comp, _filt, interlace = ihdr
    chans = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or chans is None or interlace != 0:
        raise ValueError("unsupported PNG (depth %d, colour type %d, interlace %d)" % (depth, ctype, interlace))
    if w < 1 or h < 1 or w > MAX_DIM or h > MAX_DIM:
        raise ValueError("unsupported PNG size %dx%d" % (w, h))
    stride = w * chans
    want = h * (stride + 1)
    try:
        # capped: a crafted IDAT cannot inflate past what the header says the pixels need
        dec = zlib.decompressobj()
        raw = dec.decompress(b"".join(idat), want + 1)
        if len(raw) > want or dec.unconsumed_tail:
            raise ValueError("PNG pixel data is longer than %dx%d needs" % (w, h))
    except zlib.error as e:
        raise ValueError("bad PNG data: %s" % e) from None
    if len(raw) < h * (stride + 1):
        raise ValueError("truncated PNG pixels")
    out = bytearray(h * stride)
    prev = bytes(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        row = raw[pos + 1:pos + 1 + stride]
        pos += 1 + stride
        if ft == 0:
            cur = row
        elif ft == 1:
            cur = _unfilter_sub(row, chans)
        elif ft == 2:
            cur = _add_bytes(row, prev)
        elif ft == 3:
            cur = _unfilter_avg(row, prev, chans)
        elif ft == 4:
            cur = _unfilter_paeth(row, prev, chans)
        else:
            raise ValueError("bad PNG filter type %d in row %d" % (ft, y))
        out[y * stride:(y + 1) * stride] = cur
        prev = cur
    if chans == 3:
        return w, h, bytes(out)
    rgb = bytearray(w * h * 3)
    for c in range(3):
        rgb[c::3] = out[(c if chans == 4 else 0)::chans]
    return w, h, bytes(rgb)


def encode_png(w, h, rgb, level=9):
    """PNG bytes (8 bit RGB) of w*h*3 bytes. The filter (none / sub / up for the whole picture) is the one that compresses best on a quick trial"""
    stride = w * 3
    if len(rgb) != stride * h:
        raise ValueError("encode_png: %d bytes for %dx%d" % (len(rgb), w, h))
    rows = [bytes(rgb[y * stride:(y + 1) * stride]) for y in range(h)]
    zero = bytes(stride)
    cands = {
        0: [b"\0" + r for r in rows],
        1: [b"\1" + r[:3] + _sub_bytes(r[3:], r[:-3]) if w > 1 else b"\1" + r for r in rows],
        2: [b"\2" + _sub_bytes(r, p) for r, p in zip(rows, [zero] + rows[:-1])],
    }
    best = min(cands, key=lambda k: len(zlib.compress(b"".join(cands[k]), 1)))
    return (PNG_MAGIC + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            _chunk(b"IDAT", zlib.compress(b"".join(cands[best]), level)) + _chunk(b"IEND", b""))


# ---- both formats
def decode(data):
    """(width, height, rgb bytes) of a PPM or a PNG (told by the first bytes); ValueError otherwise"""
    if data[:8] == PNG_MAGIC:
        return decode_png(data)
    return decode_ppm(data)


_ALT = {".ppm": ".png", ".png": ".ppm"}


def read_look(path):
    """(width, height, rgb bytes) of a look. A path that does not exist is tried with the other extension (.ppm <-> .png): a report whose looks were packed as
    PNG is read through the names the card wrote. OSError when neither exists, ValueError when it is no picture"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        base, ext = os.path.splitext(path)
        if ext.lower() not in _ALT:
            raise
        with open(base + _ALT[ext.lower()], "rb") as f:
            data = f.read()
    return decode(data)


def look_files(pattern):
    """the look files for `pattern` = a glob WITHOUT the extension (e.g. prefix + "-scanoutladder-baselineh-pipe*"): .ppm and .png, one per stem, sorted;
    the .ppm wins when both exist (the PNG next to a PPM is the triage's own copy of it). Returns paths with the extension they have"""
    found = {}
    for ext in (".png", ".ppm"):     # .ppm last: it overwrites
        for p in glob.glob(pattern + ext):
            found[p[:-len(ext)]] = p
    return [found[k] for k in sorted(found)]


def ppm_to_png(path, remove=False):
    """PPM -> PNG next to it (same name, .png); returns the PNG path. remove=True: the PPM is deleted after the PNG has been decoded again and is byte for byte
    the PPM's pixels; ValueError (and nothing deleted) when it is not"""
    with open(path, "rb") as f:
        w, h, px = decode_ppm(f.read())
    png = encode_png(w, h, px)
    out = os.path.splitext(path)[0] + ".png"
    tmp = out + ".part"
    try:
        with open(tmp, "wb") as f:
            f.write(png)
    except BaseException:
        try:
            os.unlink(tmp)   # a full disk or a Ctrl-C must not leave a .part in the result
        except OSError:
            pass
        raise
    if remove:
        w2, h2, px2 = decode_png(png)
        if (w2, h2) != (w, h) or px2 != px:
            os.unlink(tmp)
            raise ValueError("PNG round trip of %s differs from the PPM: kept the PPM" % os.path.basename(path))
    os.replace(tmp, out)
    if remove:
        os.unlink(path)
    return out


def _convert_one(p):
    """a worker of to_png_dir: ("ok", bytes before, bytes after) or ("err", message)"""
    try:
        b = os.path.getsize(p)
        out = ppm_to_png(p, remove=True)
        return "ok", b, os.path.getsize(out)
    except (OSError, ValueError) as e:
        return "err", "%s: %s" % (os.path.basename(p), e)


def _jobs():
    """RDNA3_TRIAGE_JOBS, else the CPUs this process may use (1 = serial)"""
    try:
        n = int(os.environ.get("RDNA3_TRIAGE_JOBS", "") or 0)
    except ValueError:
        n = 0
    if n >= 1:
        return n
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except (AttributeError, OSError):
        return max(1, os.cpu_count() or 1)


def to_png_dir(d):
    """every d/*.ppm -> .png with the PPM removed; (converted, kept, bytes before, bytes after, [problems]).
    One process per CPU (a 1280x720 look takes 0.5 s of pure Python, a 1920x1080 one about 1.1 s; 60 looks were 30 s): each file is
    independent, the results are put together in file order, so the answer is the same as one by one"""
    n = kept = before = after = 0
    problems = []
    files = sorted(glob.glob(os.path.join(glob.escape(d), "*.ppm")))
    results = None
    workers = min(_jobs(), len(files))
    if workers > 1:
        try:
            import multiprocessing
            pool = multiprocessing.get_context("fork").Pool(workers)
            try:
                results = pool.map(_convert_one, files, chunksize=1)
            except BaseException:
                pool.terminate()
                pool.join()
                raise
            pool.close()
            pool.join()
        except (OSError, ValueError, ImportError, EOFError):
            results = None   # no pool here: one by one (a file the workers already finished is a PNG now, its PPM gone: skipped)
            files = [p for p in files if os.path.exists(p)]
    if results is None:
        results = [_convert_one(p) for p in files]
    for r in results:
        if r[0] == "ok":
            n += 1
            before += r[1]
            after += r[2]
        else:
            kept += 1
            problems.append(r[1])
    return n, kept, before, after, problems


def main(argv):
    if len(argv) >= 3 and argv[1] == "--to-png":
        n, kept, before, after, problems = to_png_dir(argv[2])
        print("looks: %d PPM -> PNG (%.1f MB -> %.1f MB)%s" % (n, before / 1e6, after / 1e6, ", %d kept as PPM" % kept if kept else ""))
        for p in problems:
            print("WARN look not converted: " + p)
        return 0
    if len(argv) >= 3 and argv[1] == "--info":
        for p in argv[2:]:
            try:
                w, h, _ = read_look(p)
                print("%s: %dx%d" % (p, w, h))
            except (OSError, ValueError) as e:
                print("%s: %s" % (p, e))
        return 0
    sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
