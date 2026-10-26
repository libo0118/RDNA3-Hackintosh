#!/usr/bin/env python3
"""Read the Metal plugin's draw captures (NVMTL_CAPTURE_DRAW, metal/plugin/nvmtl_capture.h, DRIVER-PLAN N55).

    tools/capture_decode.py <capture-*.bin | report dir | report .tar.gz> [--out DIR]
    tools/capture_decode.py --check <sample.bin>      (make test: the container written by tests/test_capture.c)

For every capture it prints the draw (meta.txt) and, for every bound buffer, whether the three snapshots agree:
  cpu_draw  the CPU copy when the draw was encoded,
  cpu_done  the CPU copy when the command buffer completed,
  gpu_done  the GPU's own copy (a managed buffer's VRAM) read back after completion.
cpu_draw != gpu_done over bytes the draw reads is the verdict "the GPU drew from bytes the app did not give it" (upload / coherence);
cpu_draw != cpu_done is the app (or the plugin) rewriting the bytes while the GPU still needed them.
For Core Animation's buffer_image_blit draws (glyphs: rects at vertex buffer 1, A8 coverage at fragment buffer 9) it decodes every
rect and says which glyphs read differing bytes, and with --out writes the glyph coverage drawn from the CPU bytes and from the GPU
bytes as PGM images (target-sized), plus every record as a file.
"""
import io
import os
import re
import struct
import sys
import tarfile

MAGIC = b"NVMTLCP1"


def parse(blob):
    if blob[:8] != MAGIC:
        raise ValueError("not a capture (magic %r)" % blob[:8])
    ver, _ = struct.unpack_from("<II", blob, 8)
    if ver != 1:
        raise ValueError("capture version %d, this reader knows 1" % ver)
    recs, at = [], 16
    while at < len(blob):
        if at + 16 > len(blob):
            raise ValueError("truncated record header at %d" % at)
        tag = blob[at:at + 4].decode("latin-1")
        nl, dl = struct.unpack_from("<IQ", blob, at + 4)
        at += 16
        if at + nl + dl > len(blob):
            raise ValueError("truncated record %r at %d" % (tag, at))
        name = blob[at:at + nl].decode("utf-8", "replace")
        data = blob[at + nl:at + nl + dl]
        at += nl + dl
        recs.append((tag, name, data))
    return recs


def buffers(recs):
    out = {}
    for tag, name, data in recs:
        if tag == "BUF ":
            slot, kind = name.rsplit(".", 1)
            lo = struct.unpack_from("<Q", data, 0)[0]
            out.setdefault(slot, {})[kind] = (lo, data[8:])
    return out


def meta_of(recs):
    return "".join(d.decode("utf-8", "replace") for t, n, d in recs if t == "META")


def diff_ranges(a, b, limit=8):
    n = min(len(a), len(b))
    out, i = [], 0
    while i < n and len(out) < limit:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    total = sum(1 for k in range(n) if a[k] != b[k])
    return total, out


def slot_info(meta):
    info = {}
    for m in re.finditer(r"^buffer (\S+): off=(\d+) len=(\d+)", meta, re.M):
        info[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return info


def byte_at(snap, pos):
    lo, d = snap
    k = pos - lo
    return d[k] if 0 <= k < len(d) else None


def blit_analysis(meta, bufs, out, tag):
    si = slot_info(meta)
    if "v1" not in si or "f9" not in si or "v1" not in bufs or "f9" not in bufs:
        print("  blit: rects (v1) or coverage (f9) not captured - no glyph analysis")
        return
    inst = int(re.search(r"instances=(\d+)", meta).group(1))
    voff, foff = si["v1"][0], si["f9"][0]
    vb, fb = bufs["v1"], bufs["f9"]
    mvp = None
    if "v2" in bufs and "v2" in si:
        lo, d = bufs["v2"].get("cpu_draw", (0, b""))
        k = si["v2"][0] - lo
        if 0 <= k and k + 64 <= len(d):
            mvp = struct.unpack_from("<16f", d, k)
    vp = [float(x) for x in re.search(r"^viewport set=\d+ (.*)$", meta, re.M).group(1).split()]
    tgt = re.search(r"^target (\d+)x(\d+)", meta, re.M)
    W, H = int(tgt.group(1)), int(tgt.group(2))
    imgs = {"cpu_draw": bytearray(W * H), "gpu_done": bytearray(W * H)}
    bad = []
    for i in range(inst):
        rect = {}
        for kind in ("cpu_draw", "gpu_done"):
            if kind not in vb:
                continue
            lo, d = vb[kind]
            k = voff + 24 * i - lo
            if 0 <= k and k + 24 <= len(d):
                rect[kind] = struct.unpack_from("<6f", d, k)
        r = rect.get("cpu_draw") or rect.get("gpu_done")
        if not r:
            continue
        mx, my, sw, sh, off, bpr = r
        w, h, bpr_i, off_i = int(sw), int(sh), int(bpr), int(off)
        glyph_diff = 0
        for y in range(h):
            for x in range(w):
                p = foff + off_i + y * bpr_i + x
                a = byte_at(fb["cpu_draw"], p) if "cpu_draw" in fb else None
                g = byte_at(fb["gpu_done"], p) if "gpu_done" in fb else None
                if a is not None and g is not None and a != g:
                    glyph_diff += 1
                if mvp:
                    X, Y = mx + x + 0.5, my - y - 0.5
                    nx = mvp[0] * X + mvp[4] * Y + mvp[12]
                    ny = mvp[1] * X + mvp[5] * Y + mvp[13]
                    px, py = int(vp[0] + (nx + 1) * vp[2] / 2), int(vp[1] + (1 - ny) * vp[3] / 2)
                    if 0 <= px < W and 0 <= py < H:
                        if a is not None:
                            imgs["cpu_draw"][py * W + px] = a
                        if g is not None:
                            imgs["gpu_done"][py * W + px] = g
        rect_diff = "cpu_draw" in rect and "gpu_done" in rect and rect["cpu_draw"] != rect["gpu_done"]
        if glyph_diff or rect_diff:
            bad.append((i, r, glyph_diff, rect_diff))
    print("  blit: %d rect(s); %d read bytes the GPU had DIFFERENT from the CPU's%s" % (inst, len(bad), "" if mvp else " (no mvp: no images)"))
    for i, r, gd, rd in bad[:24]:
        print("    rect %d min=(%.1f,%.1f) size=%gx%g offset=%d bpr=%d: %d coverage byte(s) differ%s" % (i, r[0], r[1], r[2], r[3], int(r[4]), int(r[5]), gd,
              ", the RECT itself differs" if rd else ""))
    target_check(meta, bufs, imgs["cpu_draw"] if mvp else None, W, H, out, tag)
    if out and mvp:
        for kind, img in imgs.items():
            with open(os.path.join(out, "%s-%s.pgm" % (tag, kind)), "wb") as f:
                f.write(b"P5\n%d %d\n255\n" % (W, H) + bytes(img))


def target_check(meta, bufs, cov, W, H, out, tag):
    """N56: the target rows the GPU left (target.gpu_done, read at completion) against the CPU's coverage of this draw.
    A pixel the CPU covers (>= 128) must differ from the region's background, a pixel with no coverage within 1 px must not."""
    snap = bufs.get("target", {}).get("gpu_done")
    if not snap:
        m = re.search(r"^target_readback (.*)$", meta, re.M)
        print("  target: no read-back (%s)" % (m.group(1) if m else "a capture without N56"))
        return
    lo, d = snap
    pitch = int(re.search(r"bytes_per_row=(\d+)", meta).group(1))
    sc = [int(x) for x in re.search(r"^scissor set=\d+ (.*)$", meta, re.M).group(1).split()]
    x0, y0, x1, y1 = sc[0], sc[1], min(W, sc[0] + sc[2]), min(H, sc[1] + sc[3])
    px = lambda x, y: d[y * pitch - lo + 4 * x: y * pitch - lo + 4 * x + 4]
    counts = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            k = bytes(px(x, y))
            counts[k] = counts.get(k, 0) + 1
    bg = max(counts, key=counts.get)
    far = lambda p: max(abs(p[i] - bg[i]) for i in range(3)) > 24
    if cov is None:
        print("  target: read back, but no CPU coverage to compare (no mvp)")
        return
    miss = extra = text = 0
    cols = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = cov[y * W + x]
            near = max(cov[yy * W + xx] for yy in range(max(0, y - 1), min(H, y + 2)) for xx in range(max(0, x - 1), min(W, x + 2)))
            p = px(x, y)
            if c >= 128:
                text += 1
                if not far(p):
                    miss += 1; cols[x // 8] = cols.get(x // 8, 0) + 1
            elif near == 0 and far(p):
                extra += 1; cols[x // 8] = cols.get(x // 8, 0) + 1
    bad = sorted(k * 8 for k, v in cols.items() if v >= 2)
    print("  target (the GPU's rows %d..%d at completion, background bgra %s): %d of %d covered pixels show NO text, %d pixels show ink the CPU did not draw%s"
          % (y0, y1, tuple(bg), miss, text, extra, (" - columns %d..%d" % (bad[0], bad[-1] + 7)) if bad else " - the draw output MATCHES"))
    if out:
        with open(os.path.join(out, "%s-target.ppm" % tag), "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (x1 - x0, y1 - y0))
            for y in range(y0, y1):
                for x in range(x0, x1):
                    p = px(x, y)
                    f.write(bytes([p[2], p[1], p[0]]))


def report(name, blob, out):
    recs = parse(blob)
    meta = meta_of(recs)
    tag = os.path.basename(name).replace(".bin", "")
    print("== %s (%d records)" % (name, len(recs)))
    for line in meta.splitlines():
        if not line.startswith("buffer "):
            print("  " + line)
    for t, n, d in recs:
        if t == "DROP":
            print("  DROPPED %s (%d bytes over the cap)" % (n, struct.unpack_from("<Q", d)[0]))
        if t == "ERR ":
            print("  ERROR %s: %s" % (n, d.decode("utf-8", "replace")))
    bufs = buffers(recs)
    print("  NOTE: cpu_done / gpu_done are read when the command buffer COMPLETED, not at the draw: an app that reuses a pool slice or a\n"
          "  sub-range as soon as it may (Core Animation's buffer pool) can have rewritten it by then - such changes are the app's, not a\n"
          "  driver bug; trust a cpu_draw vs gpu_done difference only over bytes this draw read and cpu_done did not change.\n"
          "  The GPU read-back is a synchronous copy without the pre-submit hook (no managed upload is flushed by it), but it is still an\n"
          "  extra submit at completion time: a captured run is not timed like an uncaptured one.")
    for m in re.finditer(r"^buffer (\S+): (.*)$", meta, re.M):
        slot = m.group(1)
        snaps = bufs.get(slot, {})
        verdict = []
        if "cpu_draw" in snaps and "gpu_done" in snaps:
            n, rr = diff_ranges(snaps["cpu_draw"][1], snaps["gpu_done"][1])
            verdict.append("cpu_draw vs gpu_done: %s" % ("SAME" if not n else "%d bytes DIFFER, first ranges %s" % (n, rr)))
        if "cpu_draw" in snaps and "cpu_done" in snaps:
            n, rr = diff_ranges(snaps["cpu_draw"][1], snaps["cpu_done"][1])
            verdict.append("cpu_draw vs cpu_done: %s" % ("SAME" if not n else "%d bytes changed, first ranges %s" % (n, rr)))
        print("  buffer %s: %s | %s" % (slot, m.group(2)[:160], "; ".join(verdict) or "no snapshots"))
    if out:
        d = os.path.join(out, tag)
        os.makedirs(d, exist_ok=True)
        for i, (t, n, data) in enumerate(recs):
            fn = re.sub(r"[^A-Za-z0-9._-]", "_", n) or "rec%d" % i
            with open(os.path.join(d, fn), "wb") as f:
                f.write(data)
    if "buffer_image_blit" in meta:
        blit_analysis(meta, bufs, out, tag)


def inputs(path):
    if os.path.isdir(path):
        for root, _, files in os.walk(path):
            for f in sorted(files):
                if f.startswith("capture-") and f.endswith(".bin"):
                    p = os.path.join(root, f)
                    with open(p, "rb") as fh:
                        yield p, fh.read()
    elif tarfile.is_tarfile(path):
        with tarfile.open(path) as t:
            for m in t.getmembers():
                b = os.path.basename(m.name)
                if m.isfile() and b.startswith("capture-") and b.endswith(".bin"):
                    yield m.name, t.extractfile(m).read()
    else:
        with open(path, "rb") as fh:
            yield path, fh.read()


def main(argv):
    if len(argv) == 3 and argv[1] == "--check":
        recs = parse(open(argv[2], "rb").read())
        bufs = buffers(recs)
        ok = (meta_of(recs) == "process=test\n" and bufs["v1"]["cpu_draw"][0] == 1234 and len(bufs["v1"]["cpu_draw"][1]) == 1000
              and bufs["v1"]["cpu_draw"][1][:3] == bytes([0, 7, 14])
              and any(t == "DROP" and n == "f9.cpu_draw" and struct.unpack_from("<Q", d)[0] == 3008 for t, n, d in recs))
        print("capture_decode --check: %s" % ("ok" if ok else "FAILED"))
        return 0 if ok else 1
    args = [a for a in argv[1:] if not a.startswith("--")]
    out = None
    if "--out" in argv:
        out = argv[argv.index("--out") + 1]
        args.remove(out)
        os.makedirs(out, exist_ok=True)
    if not args:
        print(__doc__)
        return 2
    n = 0
    for p in args:
        for name, blob in inputs(p):
            n += 1
            try:
                report(name, blob, out)
            except ValueError as e:
                print("== %s: UNREADABLE (%s)" % (name, e))
    if not n:
        print("no capture-*.bin found (was NVMTL_CAPTURE_DRAW set, and did collect_plugin_trace.sh run?)")
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
