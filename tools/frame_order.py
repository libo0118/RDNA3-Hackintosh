#!/usr/bin/env python3
"""Decode the app survey's frame-order bursts (DRIVER-PLAN N59) and report every frame counter that steps BACK.

The survey's Safari page (tools/vm/macos/autoreport.sh, apps_assets: frameorder.html) draws three strips, each a
frame counter as black / white blocks (common/rdna3_strip.h: a marker, 12 bits least significant first, the same
12 bits inverted, a magenta end): `css` slides a pre-drawn column of codes with a CSS steps() animation (Core
Animation composites it), `canvas` redraws a 2D canvas per requestAnimationFrame, `webgl` clears scissor boxes per
frame. `rdna3_drminfo --scanout-strip FILE SECONDS` reads each strip's row from the scanout as fast as it can and
writes a line per change; the survey keeps one file per burst, 41-ladder-frameorder-<burst>.log in the report:

    # frameorder <burst> <time> iop_stale_wait_ms=<v>     the burst and the kernel's N59 switch during it
    B/A debug.rdna3accel_iop_stale...: <n>                the kernel's counters before / after the burst
    L <layer> <pipe> <row> <x>                            a strip found
    S <us> <layer> <pipe> <bits> <n>                      from <us> the strip showed <bits> for <n> samples
    E <samples> <us> <errors>

A step back: the next valid counter is behind the one before (modulo the layer's period: 4096, the CSS
animation's 240 steps). A torn sample (bits that are not a value and its complement) is counted apart.

    frame_order.py FILE...           a row per burst and layer, then the steps back (time, from -> to)
    frame_order.py --check FILE...   exit 1 when a step back was seen
"""
import re
import sys

BITS = 12
MOD = {"css": 240, "canvas": 4096, "webgl": 4096}
LAYERS = ("css", "canvas", "webgl")


def value(bits):
    """common/rdna3_strip.h rd3_strip_value: the counter, or None for a torn / moved sample"""
    if len(bits) != 2 * BITS:
        return None
    v = 0
    for b in range(BITS):
        d, i = bits[b], bits[b + BITS]
        if d not in "01" or i not in "01" or d == i:
            return None
        if d == "1":
            v |= 1 << b
    return v


def parse(text):
    """{'strips': [(layer, pipe, row, x)], 'samples': {(layer, pipe): [(us, bits, n)]}, 'end': (samples, us, errors) or None,
    'wait_ms': str or None, 'before': {name: int}, 'after': {name: int}}"""
    out = {"strips": [], "samples": {}, "end": None, "wait_ms": None, "before": {}, "after": {}}
    for line in text.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "#":
            m = re.search(r"iop_stale_wait_ms=(\S+)", line)
            if m:
                out["wait_ms"] = m.group(1)
        elif f[0] in ("B", "A") and len(f) == 3 and f[1].endswith(":") and f[2].lstrip("-").isdigit():
            out["before" if f[0] == "B" else "after"][f[1][:-1]] = int(f[2])
        elif f[0] == "L" and len(f) == 5 and f[1] in MOD:
            out["strips"].append((f[1], int(f[2]), int(f[3]), int(f[4])))
        elif f[0] == "S" and len(f) == 6 and f[2] in MOD and f[1].isdigit() and f[5].isdigit():
            out["samples"].setdefault((f[2], int(f[3])), []).append((int(f[1]), f[4], int(f[5])))
        elif f[0] == "E" and len(f) == 4 and all(x.isdigit() for x in f[1:]):
            out["end"] = tuple(int(x) for x in f[1:])
    return out


def analyse(layer, runs):
    """one strip's runs -> dict: samples, torn, frames (distinct valid values in order), back [(us, from, to, by)], skipped"""
    mod = MOD[layer]
    res = {"samples": 0, "torn": 0, "frames": 0, "back": [], "skipped": 0}
    prev = None
    for us, bits, n in sorted(runs):
        res["samples"] += n
        v = value(bits)
        if v is None:
            res["torn"] += n
            continue
        if prev is None or v != prev:
            res["frames"] += 1
        if prev is not None and v != prev:
            d = (v - prev) % mod
            if d >= mod // 2:
                res["back"].append((us, prev, v, mod - d))
            else:
                res["skipped"] += d - 1
        prev = v
    return res


def kernel_delta(p):
    """the kernel's N59 counters during the burst, 'name +n' for those that moved"""
    names = ("iop_stale_checked", "iop_stale", "iop_stale_moved", "iop_stale_timeout", "iop_stale_nowait", "iop_stale_torn", "iop_ok")
    parts = []
    for n in names:
        k = "debug.rdna3accel_" + n
        if k in p["before"] and k in p["after"]:
            parts.append("%s +%d" % (n, p["after"][k] - p["before"][k]))
    return ", ".join(parts)


def rows(name, text):
    """triage rows (LEVEL, 'frame order', text) of one burst file"""
    p = parse(text)
    wait = " (kernel stale wait %s ms)" % p["wait_ms"] if p["wait_ms"] is not None else ""
    kd = kernel_delta(p)
    kd = "; kernel during it: " + kd if kd else ""
    if not p["strips"]:
        return [("WARN", "frame order", "%s%s: NO strip found on any lit pipe (the page did not show, Safari covered it, or the "
                 "scanout look failed: %s)%s" % (name, wait, "no E line - the burst did not finish" if not p["end"] else "%d samples" % p["end"][0], kd))]
    out = []
    found = sorted({s[0] for s in p["strips"]}, key=LAYERS.index)
    missing = [l for l in LAYERS if l not in found]
    for (layer, pipe), runs in sorted(p["samples"].items(), key=lambda kv: (LAYERS.index(kv[0][0]), kv[0][1])):
        a = analyse(layer, runs)
        n = len(a["back"])
        ex = ", ".join("%.3f s: %d -> %d (%d back)" % (us / 1e6, f, t, by) for us, f, t, by in a["back"][:4])
        out.append(("FAIL" if n else "PASS", "frame order",
                    "%s%s: %d regressions in %d samples (layer: %s, pipe %d): %d frames seen, %d skipped, %d torn sample(s)%s%s" % (
                        name, wait, n, a["samples"], layer, pipe, a["frames"], a["skipped"], a["torn"],
                        "; first: " + ex if ex else "", kd)))
    if missing:
        out.append(("INFO", "frame order", "%s: layer(s) %s not found (WebGL: Safari may have no WebGL without the plugin in its GPU process)" % (
            name, "/".join(missing))))
    return out


def sysctls(text):
    """debug.rdna3accel_iop_stale* / iop_ok counters of a sysctl dump (29-surface-vram.txt), {short name: int}"""
    out = {}
    for m in re.finditer(r"^debug\.rdna3accel_(iop_stale\w*|iop_ok): (-?\d+)\s*$", text or "", re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def kernel_rows(surfvram, iop_on):
    """the kernel's N59 row: IOP copies of a buffer nothing wrote since the head last showed it"""
    c = sysctls(surfvram)
    if "iop_stale" not in c:
        if iop_on:
            return [("WARN", "iop stale", "no debug.rdna3accel_iop_stale* counters in 29-surface-vram.txt: a kernel before DRIVER-PLAN N59 "
                     "(or a collector that does not grep them): whether old frames reached the panel is UNKNOWN")]
        return []
    g = c.get
    old = g("iop_stale", 0) - g("iop_stale_moved", 0)
    text = ("%d IOP copies, %d of a buffer this head showed before: %d with a new frame, %d STALE (seeds unchanged: nothing wrote it "
            "since); of those %d waited and got the late write (the step back avoided), %d timed out, %d not waited (wait %s ms); "
            "%d torn (written during the copy); waits %.1f ms in all, longest %.1f ms" % (
                g("iop_ok", 0), g("iop_stale_checked", 0), g("iop_stale_fresh", 0), g("iop_stale", 0), g("iop_stale_moved", 0),
                g("iop_stale_timeout", 0), g("iop_stale_nowait", 0), g("iop_stale_wait_ms", "?"), g("iop_stale_torn", 0),
                g("iop_stale_waitus", 0) / 1000.0, g("iop_stale_wait_maxus", 0) / 1000.0))
    if g("iop_stale_moved", 0):
        text += ": PROVEN - WindowServer presented buffers before the plugin's copy-out of their frame (DRIVER-PLAN N59)"
    return [("WARN" if old > 0 else "INFO", "iop stale", text)]


def handoff_rows(plugin_text):
    """the Metal plugin's `vk: handoff: SUMMARY` (metal/plugin/nvmtl_handoff.h), the newest line per process"""
    last = {}
    for m in re.finditer(r"([A-Za-z][\w .-]*?)\[(\d+):[0-9a-f]+\].*?NVMTL: vk: handoff: SUMMARY (.*)$", plugin_text or "", re.M):
        name = re.sub(r"^(Df|Db|Dc|I|In|E|Er|Ef|F|Fa|A|De|Default|Info|Debug|Error|Fault)\s+", "", m.group(1).strip())   # `log show --style compact`'s level
        last["%s[%s]" % (name, m.group(2))] = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    out = []
    for proc, d in sorted(last.items()):
        early, cbs = int(d.get("early", "0") or 0), int(d.get("cbs", "0") or 0)
        text = ("%s: %d of %d command buffer(s) writing an IOSurface were seen scheduled BEFORE the write was in the surface "
                "(handler %s, waitUntilScheduled %s; gap avg %s us, max %s us); NVMTL_SCHED_AFTER_PUBLISH %s, held %s (avg %s us)" % (
                    proc, early, cbs, d.get("by_handler", "?"), d.get("by_wait", "?"), d.get("gap_us_avg", "?"), d.get("gap_us_max", "?"),
                    "ON" if d.get("hold") == "1" else "off", d.get("held", "?"), d.get("held_us_avg", "?")))
        out.append(("WARN" if early else "INFO", "handoff", text))
    return out


def report_rows(ladder_logs, surfvram, iop_on, plugin_text=""):
    """triage_reports.py: the kernel's row, the plugin's hand-off rows, then a row per burst and layer (41-ladder-frameorder-<burst>.log)"""
    out = kernel_rows(surfvram, iop_on) + handoff_rows(plugin_text)
    for name in sorted(k for k in (ladder_logs or {}) if k.startswith("frameorder-")):
        out.extend(rows(name, ladder_logs[name]))
    return out


def main(argv):
    check = "--check" in argv
    files = [a for a in argv[1:] if a != "--check"]
    if not files:
        sys.exit(__doc__)
    bad = False
    for fn in files:
        with open(fn, encoding="utf-8", errors="replace") as f:
            text = f.read()
        name = re.sub(r"^.*?(frameorder-[^/]*?)(\.log)?$", r"\1", fn)
        for lvl, _, t in rows(name, text):
            print("%-4s frame order: %s" % (lvl, t))
            bad = bad or lvl == "FAIL"
        p = parse(text)
        for (layer, pipe), runs in sorted(p["samples"].items()):
            for us, f_, t_, by in analyse(layer, runs)["back"]:
                print("     %s pipe %d %.6f s: %d -> %d (%d frame(s) back)" % (layer, pipe, us / 1e6, f_, t_, by))
    return 1 if (check and bad) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
