#!/usr/bin/env python3
"""
triage_reports.py - one summary of a batch of hardware-test reports.

    python3 tools/triage_reports.py ~/rdna3-vm/reports [--all]
    python3 tools/triage_reports.py --picture scanout.ppm   (-> scanout.png)

Reads every rdna3-report-*.tar.gz (tools/macos/collect.sh), the matching
*-tests.txt (DMA/GFX tests), serial-*.log (macOS kernel output of the VM)
and host-*.log (tools/vm/rdna3vm), oldest first, and prints per boot:
boot-args, the bring-up stage reached, every check below as PASS / FAIL /
-- (not reached at that level), the kext's ERROR lines and the tail of
the serial log. The first FAIL is where docs/AFTER-TESTS.md starts.
The pictures of what each display pipe scanned (<report>-scanout-pipe<N>.ppm,
rdna3_drminfo --scanout) are turned into PNGs next to them.
Exit status: 0 all reached checks pass, 1 something failed.
"""
import contextlib
import glob
import io
import os
import re
import struct
import subprocess
import sys
import tarfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import rdna3_trace  # noqa: E402  (tools/rdna3_trace.py: the kernel's per-frame trace)
except ImportError:      # a copy of this file alone (the tests' fake checkouts)
    rdna3_trace = None
try:
    import triage_bisect  # noqa: E402  (tools/triage_bisect.py: the display bisect's table)
except ImportError:
    triage_bisect = None

U64 = 1 << 64


def signed(v):
    return v - U64 if v >= 1 << 63 else v


# (key, minimum rdna3lvl, extra boot-arg or None, check, what is expected)
CHECKS = [
    # RCC_CONFIG_MEMSIZE, the value amdgpu also reports (mem_info_vram_total):
    # 16368 on the RX 7800 XT, 16 MiB below the 16 GiB on the box
    ("VRAMSizeMB", 1, None, lambda v: v == REF_VRAM_MB if REF_VRAM_MB else v in (16368, 16384),
     "Linux's mem_info_vram_total (linux-reference), else 16368"),
    ("C2PMSG33", 1, None, lambda v: v != 0 and v != 0xFFFFFFFF, "IFWI ready, not 0 / all-ones"),
    ("IPDiscovery", 2, None, lambda v: bool(v), "present (IP list)"),
    # heads the UEFI GOP lit; none in the VM, where OVMF shows no picture
    ("DisplayHeads", 2, None, lambda v: v >= 1 or IN_VM, ">= 1 (any in the VM)"),
    ("DisplayPaths", 2, None, lambda v: v >= 1, ">= 1"),
    ("ActiveCUs", 3, None, lambda v: v == 60, "60 (compare Linux if fused)"),
    ("PSPResult", 4, None, lambda v: signed(v) == 0, "0"),
    ("SMUVersion", 5, None, lambda v: v != 0, "!= 0"),
    ("PMResult", 5, None, lambda v: signed(v) == 0, "0"),
    ("PMTempHotspotC", 5, None, lambda v: 15 <= v <= 95, "15..95"),
    ("DMUBResult", 5, "rdna3dmub=", lambda v: signed(v) == 0, "0 (rdna3dmub=1 load, =3 attach)"),
    ("GMCResult", 6, None, lambda v: signed(v) == 0, "0"),
    ("IHResult", 6, None, lambda v: signed(v) == 0, "0"),
    ("MSILive", 6, "rdna3msi=1", lambda v: v == 1, "1"),
    ("SDMAResult", 7, None, lambda v: signed(v) == 0, "0"),
    ("SDMACopyMismatches", 7, None, lambda v: v == 0, "0"),
    ("MSIVerified", 7, "rdna3msi=1", lambda v: v == 1, "1"),
    ("GFXResult", 8, None, lambda v: signed(v) == 0, "0"),
    ("KIQResult", 8, None, lambda v: signed(v) == 0, "0"),
    ("KCQResult", 8, None, lambda v: signed(v) == 0, "0"),
    ("VMFaults", 7, None, lambda v: v == 0, "0"),
    ("GPUResets", 4, None, lambda v: v == 0, "0"),
]

REF_VRAM_MB = 0
IN_VM = False           # set per report: rdna3vm passes rdna3vmauto=1


def load_linux_reference(d):
    """mem_info_vram_total from <reports>/linux-reference/sysfs-pm.txt"""
    global REF_VRAM_MB
    try:
        txt = open(os.path.join(d, "linux-reference", "sysfs-pm.txt"), errors="replace").read()
    except OSError:
        return
    m = re.search(r"== mem_info_vram_total\s+(\d+)", txt)
    if m:
        REF_VRAM_MB = int(m.group(1)) >> 20


PROP = re.compile(r'"RDNA3,([A-Za-z0-9,]+)"\s*=\s*(.*)$')


def parse_props(text):
    props = {}
    for line in text.splitlines():
        m = PROP.search(line)
        if not m:
            continue
        key, val = m.group(1), m.group(2).strip()
        if val.startswith('"'):
            props[key] = val.strip('"')
        elif re.fullmatch(r"\d+", val):
            props[key] = int(val)
        elif re.fullmatch(r"0x[0-9a-fA-F]+", val):
            props[key] = int(val, 16)
        else:
            props[key] = val
    return props


def read_member(tar, suffix):
    for m in tar.getmembers():
        if m.name.endswith(suffix) and m.isfile():
            return tar.extractfile(m).read().decode("utf-8", "replace")
    return ""


def bootarg_level(args):
    m = re.search(r"rdna3lvl=(\d+)", args)
    return int(m.group(1)) if m else 0


# accel/Makefile's ACCEL_SRC_HASH inputs, in its order (tests/test_accel_hash_parity.py keeps them equal)
ACCEL_HASH_FILES = ("accel/rdna3-accel.cpp", "accel/Info.plist", "accel/rdna3k_abi.h", "accel/rdna3accel_stubs.h",
                    "common/rdna3_bootargs.h", "common/rdna3_a3_display.h",
                    "common/rdna3_swap_front.h", "common/rdna3_flip_latch.h", "common/rdna3_swap_copy.h",
                    "common/rdna3_trace.h", "common/rdna3_write_gen.h", "common/rdna3_flip_queue.h",
                    "common/rdna3_vram_park.h", "common/rdna3_surf_pending.h", "common/rdna3_tcall.h",
                    "common/rdna3_gpu_util.h")


# fb/Makefile's FB_SRC_HASH inputs and agdc/Makefile's AGDC_SRC_HASH inputs
FB_HASH_FILES = ("fb/rdna3-fb.cpp", "fb/Info.plist", "accel/rdna3k_abi.h", "agdc/rdna3_agdc_k5.h",
                 "common/rdna3_fb_policy.h", "common/rdna3_bootargs.h", "common/rdna3_vbl_irq.h",
                 "common/rdna3_tcall.h")
AGDC_HASH_FILES = ("agdc/rdna3-agdc.cpp", "agdc/rdna3_agdc_k5.h", "agdc/rdna3_agdc_n48.h",
                   "agdc/AppleGraphicsDeviceControl_re.h", "agdc/Info.plist", "common/rdna3_bootargs.h")


def fb_source_hash(files=FB_HASH_FILES):
    """fb/Makefile's FB_SRC_HASH (accel/Makefile's ACCEL_SRC_HASH with the
    accelerator's files): sha1 of the concatenated sources, 10 hex"""
    import hashlib
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    h = hashlib.sha1()
    try:
        for f in files:
            with open(os.path.join(root, f), "rb") as fh:
                h.update(fh.read())
    except OSError:
        return None
    return h.hexdigest()[:10]


def ppm_to_png(path):
    """binary PPM (P6, maxval 255) -> PNG next to it; returns the PNG path"""
    data = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        raise ValueError("not a P6 PPM: " + path)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    if len(px) < w * h * 3:
        raise ValueError("short PPM: " + path)
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    out = os.path.splitext(path)[0] + ".png"
    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    return out


def count_color(path, rgb, tol=40):
    """pixels of a P6 PPM within 'tol' of colour 'rgb' (each channel); the
    total; None when it is no picture"""
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    hit = 0
    for i in range(0, len(px) - 2, 3):
        if abs(px[i] - rgb[0]) <= tol and abs(px[i + 1] - rgb[1]) <= tol and abs(px[i + 2] - rgb[2]) <= tol:
            hit += 1
    return hit, w * h


# --- STALE PLUGIN / RADV -----------------------------------------------------
# tools/vm/rdna3vm update installs only the kext; the Metal plugin and RADV are
# built inside the VM (tools/vm/macos/build_metal.sh, build_radv.sh), so they
# can silently be older than the kext under test (card run 2026-10-15: a fix
# of eb5e3e8 and the N1/A3 plugin work were never tested). Two checks:
#  1. the build stamps (35-build-stamps.txt, written by the build scripts):
#     the last commit touching metal/ (resp. userspace/mesa) in the source the
#     build used, against the same in the repo at the commit 00-host.txt names.
#     STALE only when the stamp's commit is provably an ancestor of the repo's
#     (git history of this checkout); otherwise an INFO line says why not.
#  2. no stamp (reports before stamps, CI-artifact RADV): the plugin log has
#     the "vk: G14 primitive topology" line (eb5e3e8's neighbour) but not the
#     "vk: VK_EXT_extended_dynamic_state (ENABLED|ABSENT)" line that eb5e3e8
#     added right before it -> a plugin older than that commit.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STALE_SOURCES = {   # kind: (stamp key, 00-host.txt key, paths in the repo)
    "PLUGIN": ("metal", "repo-metal", ("metal",)),
    "RADV": ("mesa", "repo-mesa", ("userspace/mesa", "userspace/rdna3_vktest.c")),
}
HEX_SHA = re.compile(r"[0-9a-f]{7,40}\Z")


# M6 opens Chess, Maps and System Settings on the main display, then looks
# at the desktop: a pipe whose picture hardly changed between any two
# consecutive M6 looks never showed the apps (card run 58: head 0 kept an
# old frame while WindowServer drew into A3 BOs no swap scanned out). A
# clock tick in the menu bar is far below the threshold, a window is far
# above it.
M6_LOOK = re.compile(r"-scanoutm6([A-Za-z]+)-pipe(\d+)\.ppm$")
M6_ORDER = ("Chess", "Maps", "Settings", "desktop")
M6_FROZEN_FRACTION = 0.01   # of the pixels: below this between every two looks = frozen


def ppm_pixels(path):
    """(width, height, rgb bytes) of a binary PPM, or None"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    m = re.match(rb"P6\s+(?:#[^\n]*\s+)*(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    return (w, h, px) if len(px) == w * h * 3 else None


def changed_fraction(a, b):
    """the fraction of pixels that differ between two pictures (1.0 when the sizes differ)"""
    if not a or not b or a[0] != b[0] or a[1] != b[1]:
        return 1.0
    w, h, pa = a
    pb = b[2]
    row, n = w * 3, 0
    for y in range(h):
        ra, rb = pa[y * row:(y + 1) * row], pb[y * row:(y + 1) * row]
        if ra != rb:
            n += sum(1 for x in range(0, row, 3) if ra[x:x + 3] != rb[x:x + 3])
    return n / float(w * h)


# a look whose picture is at most this much not black is a black screen (A5)
BLACK_PCT = 3


def look_pictures(blk):
    """{pipe: (% not black or None, bytes changed in 2 s)} of the LIT pipes of one rdna3_drminfo --scanout
    block; None: lit, but its picture was not readable (tiled, DCC, outside the BAR) or not printed"""
    out = {}
    cur = None
    for line in blk.splitlines():
        m = re.match(r"^pipe (\d+): (\w+)", line)
        if m:
            cur = m.group(1) if m.group(2) == "LIT" else None
            if cur is not None:
                out[cur] = (None, 0)
            continue
        m = re.match(r"^  picture \d+x\d+: (\d+)% not black, mean rgb \d+ \d+ \d+, (\d+) bytes changed", line)
        if m and cur is not None:
            out[cur] = (int(m.group(1)), int(m.group(2)))
    return out


def ab_pictures(txt, main=None):
    """[(row name, text)]: the A/B's two looks side by side (look ab1: mode 1, look ab0: mode 0) - 'FAIL ...'
    when mode 1 left a judged pipe black, WARN for a lit pipe whose picture was not readable, for no pipe
    judged in both looks and for a look missing although the A/B ran (ECC review round 3: those were a
    silent PASS) - and, with main = (pipe, how it was found), the main display's line"""
    looks = {}
    for blk in txt.split("== rdna3_drminfo --scanout")[1:]:
        m = re.match(r"\s*\(look (ab[01])\)", blk)
        if m:
            looks[m.group(1)] = look_pictures(blk.split("\n== ")[0])
    ran = "== swap A/B phase 1" in txt
    missing = [k for k in ("ab1", "ab0") if k not in looks]
    if missing:
        if ran:
            return [("A/B screens", "WARN look %s missing although the A/B ran (rdna3_drminfo not run or its "
                                    "output lost): the panels were NOT JUDGED" % " and ".join(missing))]
        return []
    a, b = looks["ab1"], looks["ab0"]
    pipes = sorted(set(a) | set(b), key=int)
    unread = ["pipe %s picture NOT READABLE in look %s (not judged)" % (p, k)
              for k, lk in (("ab1", a), ("ab0", b)) for p in pipes if p in lk and lk[p][0] is None]
    judged = [p for p in pipes if a.get(p, (None,))[0] is not None and b.get(p, (None,))[0] is not None]
    side = "; ".join("pipe %s: mode 1 %s%%, mode 0 %s%%" % (p, a[p][0], b[p][0]) for p in judged)
    note = ("; " + "; ".join(unread)) if unread else ""
    rows = []
    black1 = [p for p in judged if a[p][0] <= BLACK_PCT]
    black0 = [p for p in judged if b[p][0] <= BLACK_PCT]
    if not judged:
        rows.append(("A/B screens", "WARN NOT JUDGED: no lit pipe with a readable picture in both looks%s" % note))
    elif black1:
        rows.append(("A/B screens", "FAIL mode 1 left pipe(s) %s BLACK (%s)%s%s" %
                     (" ".join(black1), side, "; mode 0 too" if set(black1) & set(black0) else "", note)))
    elif black0:
        rows.append(("A/B screens", "WARN mode 0 left pipe(s) %s black, mode 1 did not (%s)%s" %
                     (" ".join(black0), side, note)))
    elif unread:
        rows.append(("A/B screens", "WARN no black pipe among those judged (%s)%s" % (side, note)))
    else:
        rows.append(("A/B screens", "PASS no black pipe in either mode (%s)" % side))
    if main is not None:
        mp = str(main[0])
        x, y = a.get(mp, (None,))[0], b.get(mp, (None,))[0]
        if x is None or y is None:
            rows.append(("A/B main display", "WARN pipe %s (%s): %s - not judged" % (
                mp, main[1], "not lit in both looks" if mp not in a or mp not in b else "picture NOT READABLE")))
        else:
            lv = "FAIL" if x <= BLACK_PCT else "WARN" if y <= BLACK_PCT else "PASS"
            rows.append(("A/B main display", "%s mode 1 vs mode 0 on the main display (pipe %s, %s): %d%% vs %d%% "
                         "not black" % (lv, mp, main[1], x, y)))
    return rows


def ab_diag_row(txt):
    """('swap A/B', text) from the tests file's '== swap A/B' / '== frame trace' / '== plugin trace' lines
    (ECC review round 3: autoreport's diagnostics never reached the SUMMARY): FAIL for a failed A/B (its
    exit), a setting NOT APPLIED or FAILED, nobody logged in; WARN for no activity, pump read errors,
    uploads that failed, plugin snapshots without a trace or with rejects, rdna3mtl.conf not written,
    fix_tracedir.sh failed; None when the tests file has no A/B at all"""
    if "== swap A/B" not in txt:
        return None
    if "== swap A/B: NOT ASKED" in txt and "== swap A/B phase" not in txt:
        # card run 2026-10-17d: the A/B is opt-in (boot-arg rdna3swapab=1); a normal step does none
        return ("swap A/B", "not asked (opt-in: boot-arg rdna3swapab=1; the default swap mode is 0, the flip)")
    if "== swap A/B: SKIPPED" in txt:
        return ("swap A/B", "not run: " + re.search(r"== swap A/B: SKIPPED - (.*)", txt).group(1)[:120])
    fail, warn = [], []
    ex = re.findall(r"^== swap A/B done: .*\(exit (\d+)\)", txt, re.M)
    if not ex:
        fail.append("no '== swap A/B done' line (the A/B did not finish)")
    elif ex[-1] != "0":
        fail.append("exit %s" % ex[-1])
    fail += [m[:100] for m in re.findall(r"^== swap A/B: ((?:FAILED to set|.*NOT APPLIED).*)$", txt, re.M)]
    if "NOBODY LOGGED IN" in txt:
        fail.append("NOBODY LOGGED IN")
    if "Activity Monitor is NOT running" in txt:
        warn.append("Activity Monitor NOT running")
    if "pump read errors" in txt:
        warn.append("pump read errors")
    up = re.findall(r"^upload of (\S+) FAILED", txt, re.M)
    if up:
        warn.append("%d upload(s) FAILED (%s)" % (len(up), ", ".join(up[:3])))
    for lab, n, rej in re.findall(r"^== plugin trace snapshot (\S+): (\d+) WindowServer trace\(s\); (\d+) rejected", txt, re.M):
        if n == "0" or rej != "0":
            warn.append("plugin snapshot %s: %s trace(s), %s rejected" % (lab, n, rej))
    if "plugin trace ring: could NOT write" in txt:
        warn.append("rdna3mtl.conf NOT written")
    if "fix_tracedir.sh FAILED" in txt:
        warn.append("fix_tracedir.sh FAILED")
    snaps = len(re.findall(r"^== frame trace snapshot ", txt, re.M))
    body = "; ".join(fail + warn) or "ran (%d snapshot(s) sent)" % snaps
    return ("swap A/B", ("FAIL " if fail else "WARN " if warn else "PASS ") + body)


def m6_screens(prefix):
    """{pipe: (look names in order, the largest change between two
    consecutive looks)} of the M6 hi-res pictures"""
    by_pipe = {}
    for ppm in glob.glob(prefix + "-scanoutm6*-pipe*.ppm"):
        m = M6_LOOK.search(ppm)
        if m:
            by_pipe.setdefault(int(m.group(2)), []).append((m.group(1), ppm))
    rank = lambda n: (M6_ORDER.index(n) if n in M6_ORDER else len(M6_ORDER), n)
    out = {}
    for pipe, looks in sorted(by_pipe.items()):
        looks.sort(key=lambda t: rank(t[0]))
        pics = [ppm_pixels(f) for _, f in looks]
        most = max([changed_fraction(pics[i], pics[i + 1]) for i in range(len(pics) - 1)] or [0.0])
        out[pipe] = ([n for n, _ in looks], most)
    return out


def main_pipe(path, txt):
    """(pipe, source) of the main display: the pipe whose size followed the
    mode switch of CG display 0 (rdna3_modetest's "display 0", the main
    display: card run 58 showed it on pipe 1, not head 0's), else head 0's
    OTG (RDNA3,Head0,Route) found in the looks' 'pipe P: ... otg O' lines,
    else pipe 0"""
    for m in re.finditer(r"^== rdna3_drminfo --scanout \(look mode-d0-(\d+)x(\d+)(?:-[\w.]+)?\)\n(.*?)(?=^== |\Z)",
                         txt, re.M | re.S):
        hit = re.findall(r"^pipe (\d+): LIT otg \d+ %sx%s " % (m.group(1), m.group(2)), m.group(3), re.M)
        if len(hit) == 1:
            return int(hit[0]), "display 0's mode switch to %sx%s" % (m.group(1), m.group(2))
    props_txt = ""
    if os.path.exists(path):
        try:
            with tarfile.open(path) as tar:
                props_txt = read_member(tar, "11-ioreg-props.txt") or ""
        except (OSError, tarfile.TarError):
            pass
    m = re.search(r"OTG(\d+)", str(parse_props(props_txt).get("Head0,Route", "")))
    if m:
        for pp, otg in re.findall(r"^pipe (\d+): \w+ otg (\d+)", txt, re.M):
            if otg == m.group(1):
                return int(pp), "head 0's route, OTG%s" % m.group(1)
    return 0, "the default: no mode switch look, no head 0 route"


def m6_cut(txt):
    """M6 began but never reached its end: '== M6 end' (card run 58b), or for
    older reports the copy-forward log that closes it"""
    return "== probe: Metal apps (M6)" in txt and "== M6 end" not in txt and \
        "== copy forward (kernel) during M6" not in txt


def m6_verdict(prefix, txt, main_src):
    """[(level, text, short)] about the M6 pictures: FAIL only for the main display's
    pipe, only when every app ran and at least 3 looks were taken"""
    main, src = main_src
    out = []
    norun = re.findall(r"^M6 (\S+): running NO", txt, re.M) + \
        [a for a, cr in re.findall(r"^M6 (\S+): running .*crash report: (\S+)", txt, re.M) if cr != "none"]
    for pipe, (names, most) in sorted(m6_screens(prefix).items()):
        what = "pipe %d%s" % (pipe, " (main display, by %s)" % src if pipe == main else "")
        if len(names) < 3:
            out.append(("INFO", "%s: %d M6 look(s), not judged" % (what, len(names)), "pipe %d not judged" % pipe))
        elif most >= M6_FROZEN_FRACTION:
            out.append(("PASS", "%s changed between the M6 looks (up to %.0f%% of the pixels)" % (what, most * 100),
                        "pipe %d changed (%.0f%%)" % (pipe, most * 100)))
        elif pipe != main:
            out.append(("INFO", "%s did not change in the M6 looks (%s): not the main display, the apps open "
                                "elsewhere" % (what, ", ".join(names)), "pipe %d unchanged (not main)" % pipe))
        elif norun:
            out.append(("INFO", "%s did not change in the M6 looks, not judged: %s did not run" %
                        (what, ", ".join(sorted(set(norun)))), "pipe %d unchanged, an app did not run" % pipe))
        else:
            out.append(("FAIL", "%s showed the same picture in all %d M6 looks (%s; under %.0f%% of the pixels "
                                "changed): the apps' frames never reached it (card run 58: an old frame on the "
                                "main screen)" % (what, len(names), ", ".join(names), M6_FROZEN_FRACTION * 100),
                        "pipe %d (main) FROZEN in all %d looks" % (pipe, len(names))))
    return out


FAULT = re.compile(r"\[(gfxhub|mmhub)\] page fault \(src_id:\d+ ring:\d+ vmid:(\d+) pasid:\d+\) at (0x[0-9a-f]+)")
FAULT_OWNER = re.compile(r"page fault: VMID (\d+)'s client \(([^,]+), pid (\d+)\)")
FAULT_CID = re.compile(r"L2_PROTECTION_FAULT_STATUS 0x[0-9a-f]+: UTCL2 client (0x[0-9a-f]+), .* RW (read|write)")
# gfxhub_v3_0.c gfxhub_client_ids
GFXHUB_CLIENTS = {0x0: "CB/DB", 0x2: "GE1", 0x3: "GE2", 0x4: "CPF", 0x5: "CPC", 0x6: "CPG", 0x7: "RLC", 0x8: "TCP",
                  0x9: "SQC inst", 0xa: "SQC data", 0xb: "SQG", 0xd: "SDMA0", 0xe: "SDMA1", 0xf: "GCR"}


def vm_fault_groups(log):
    """the GPU page faults of a kernel log grouped by hub, VMID and address:
    [(count, hub, vmid, addr, owner, client)] (card run 58: 9 faults of
    Terminal's VMID 5, each followed by 4 on VMID 0 at address 0)"""
    groups, order, owners = {}, [], {}
    last = None
    for l in log.splitlines():
        m = FAULT.search(l)
        if m:
            last = (m.group(1), int(m.group(2)), m.group(3))
            if last not in groups:
                groups[last] = [0, ""]
                order.append(last)
            groups[last][0] += 1
            continue
        m = FAULT_CID.search(l)
        if m and last and not groups[last][1]:
            cid = int(m.group(1), 16)
            groups[last][1] = "%s %s" % (GFXHUB_CLIENTS.get(cid, "client 0x%x" % cid) if last[0] == "gfxhub"
                                         else "client 0x%x" % cid, m.group(2))
            continue
        m = FAULT_OWNER.search(l)
        if m:
            owners.setdefault(int(m.group(1)), set()).add("%s %s" % (m.group(2), m.group(3)))
    out = []
    for k in order:
        hub, vmid, addr = k
        who = "no client (the kernel's VMID)" if vmid == 0 else ", ".join(sorted(owners.get(vmid, {"?"})))
        out.append((groups[k][0], hub, vmid, addr, who, groups[k][1]))
    return out


def scanout_sizes(log):
    """the (w, h) strings each head scanned out, from the accelerator's log"""
    return set(re.findall(r"LIVE SCANOUT phys 0x[0-9a-f]+ (\d+) x (\d+)", log)) | \
        set(re.findall(r"a3 display: head \d+: .*?\((\d+)x(\d+)\)", log))


N41_SYSCTL = re.compile(r"^debug\.rdna3accel_((?:vm|surface)_\w+): (-?\d+)", re.M)
# every counter an accelerator of these sources exports (ECC review of N41: a missing one is named, never silence)
N41_VM_KEYS = ("vm_sticky", "vm_alloc", "vm_dealloc", "vm_refused", "vm_parked", "vm_unparked", "vm_park_released",
               "vm_park_grow", "vm_reparked", "vm_parked_bytes", "vm_free_live", "vm_released", "vm_released_bytes",
               "vm_release_failed", "vm_restored", "vm_restore_miss", "vm_dirty_moved", "vm_apdesc_moved",
               # security review of N41: only a BO's allocation parked; parks no BO holds released under pressure / cap
               "vm_park_noimport", "vm_park_trim_pressure", "vm_park_trim_cap", "vm_park_trimmed",
               "vm_park_trimmed_bytes", "vm_park_trim_held", "vm_park_trim_unknown", "vm_alloc_retry_ok",
               "vm_alloc_retry_fail", "vm_park_cap_mb")
N41_SW_KEYS = ("surface_wait", "surface_wait_ms", "surface_wait_maxus", "surface_wait_iop_maxus", "surface_wait_busy",
               "surface_wait_pending", "surface_wait_ready", "surface_wait_late", "surface_wait_evicted",
               "surface_wait_saturated", "surface_wait_off", "surface_wait_notws", "surface_wait_notown",
               "surface_wait_waits", "surface_wait_timeouts", "surface_wait_cleared", "surface_wait_us",
               "surface_wait_iop_waits", "surface_wait_iop_timeouts", "surface_wait_iop_cleared", "surface_wait_iop_us",
               "surface_dirty_trybusy",
               # security review / round-2 review of N41: the mark gate, the timeout budget, the forgiven marks
               "surface_wait_notreal", "surface_wait_moot", "surface_wait_skipped", "surface_wait_starved",
               "surface_wait_aged", "surface_wait_ghosts", "surface_wait_ws_pid", "surface_wait_mark_pid",
               "surface_wait_mark_uid",
               # ECC review round 3 of N41: the first display client is sticky while it lives
               "surface_wait_ws_pid_refused",
               # N41 open item 4: the marks of a surface dropped from the full table, forgiven at its next mark
               "surface_wait_evict_kept")
N41_PARKED_BYTES_WARN = 256 << 20   # VRAM parked at the end of a run that says "parks hold VRAM others may need"
N41_UNACCOUNTED_WARN = 64           # pending marks whose ready never came (more than one frame's surfaces in flight)


def n43_buftex_rows(cdlog="", pluginlog=""):
    """DRIVER-PLAN N43 (1): buffer textures the plugin did not alias because their bytesPerRow is not a multiple of
    256 B (RADV's linear custom pitch; before N43 each was a vkCreateImage failure, -1000158000, ~32/s from
    WindowServer). [(level, name, text)]. INFO with the count (the plugin logs the first 8 and every 1024th, and
    the buffer texture census carries the running count: the largest number seen is a lower bound) and the pitches;
    WARN for a plugin older than N43 (its log shows the vkCreateImage failure itself), which is also named by the
    stale-plugin check once the sources changed."""
    text = (cdlog or "") + "\n" + (pluginlog or "")
    rows = []
    seen = [int(n) for n in re.findall(r"buffer texture copy \(pitch\): [^\n]*?\((\d+) so far\)", text)]
    seen += [int(n) for n in re.findall(r"buffer texture \([a-z]+\): [^\n]*?, (\d+) copy \(pitch\)", text)]
    pitches = sorted(set(int(p) for p in re.findall(r"buffer texture copy \(pitch\): bytesPerRow (\d+) is not a multiple of 256", text)))
    seen = [n for n in seen if n]
    if seen:
        rows.append(("INFO", "buffer texture copy (pitch)",
                     "at least %d buffer texture(s) over a bytesPerRow that is not a multiple of 256 B were not aliased (RADV's "
                     "linear custom pitch) and drew from a private copy refreshed per command buffer (bytesPerRow %s); open "
                     "(DRIVER-PLAN N43): park the copy instead of a new image + upload per use" %
                     (max(seen), ", ".join(str(p) for p in pitches[:8]) or "?")))
    old = len(re.findall(r"vk: vkCreateImage\(buffer texture\) -> -1000158000", text))
    if old:
        rows.append(("WARN", "buffer texture copy (pitch)",
                     "%d vkCreateImage(buffer texture) -> -1000158000 line(s): the plugin is older than N43 (it asks RADV for a "
                     "linear alias with a pitch that is not a multiple of 256 B and lets it fail); rebuild the plugin" % old))
    return rows


N43_UMC_LOAD_PCT = 20    # umc activity above this is a memory load the PMFW should answer with a higher UCLK


def _n43_pm_sample(blk):
    """one `rdna3_pmctl info` excerpt: {"levels": [MHz], "now": MHz, "umc": %, "uclk": MHz} (None: a key not found)"""
    m = re.search(r"^pp_dpm_mclk \(now (\d+) MHz\):(.*)$", blk, re.M)
    a = re.search(r"activity gfx (\d+) % umc (\d+) %.*?uclk (\d+) MHz", blk)
    return {"levels": [int(x) for x in re.findall(r"\d+: (\d+)Mhz", m.group(2))] if m else [],
            "now": int(m.group(1)) if m else None, "umc": int(a.group(2)) if a else None,
            "uclk": int(a.group(3)) if a else None}


def n43_vktest_rows(txt, pm2=False):
    """DRIVER-PLAN N43 from the tests file: (1) rdna3_vktest's `LINPITCH:` line (an explicit linear row pitch of 64 B
    refused by RADV with -1000158000, 256 B accepted: the rule behind nvmtl_linear_pitch.h) and (2) the memory load
    (autoreport.sh's `== rdna3 memory load` section: `rdna3_vktest --membw` while `rdna3_pmctl info` is read three
    times, once more after it). [(level, name, text)]. WARN when UCLK stays at its lowest level while the memory
    controller is busy (umc activity above N43_UMC_LOAD_PCT) - UCLK DPM does not follow a load; never silence for
    a report whose boot-args had rdna3pm=2 and which ran the Vulkan test (pm2)."""
    rows = []
    lp = re.search(r"^LINPITCH: (rowPitch 64 -> (-?\d+).*rowPitch 256 -> (-?\d+).*)$", txt, re.M)
    if lp:
        r64, r256 = int(lp.group(2)), int(lp.group(3))
        if r256 != 0:
            rows.append(("FAIL", "linear pitch", "an explicit 256 B linear row pitch is refused by RADV (%d): no buffer texture can "
                         "be aliased; %s" % (r256, lp.group(1)[:120])))
        elif r64 == -1000158000:
            rows.append(("INFO", "linear pitch", "RADV refuses an explicit linear row pitch of 64 B (-1000158000) and takes 256 B: "
                         "the plugin's 256 B rule (nvmtl_linear_pitch.h) is the RADV rule"))
        elif r64 == 0:
            rows.append(("WARN", "linear pitch", "RADV ACCEPTED an explicit linear row pitch of 64 B: the plugin's 256 B rule is "
                         "stricter than this RADV (only costs copies); a different refusal (%d) is none of the known rule" % r64))
        else:
            rows.append(("WARN", "linear pitch", "a 64 B linear row pitch gave %d, not -1000158000: %s" % (r64, lp.group(1)[:120])))
    elif "== rdna3_vktest" in txt:
        why = re.search(r"^LINPITCH: (NOT CHECKED[^\n]*)", txt, re.M)
        rows.append(("INFO" if why else "WARN", "linear pitch",
                     why.group(1)[:140] if why else "no `LINPITCH:` line in the Vulkan test (an rdna3_vktest older than N43, or it died "
                     "before the check)"))
    i = txt.find("\n== rdna3 memory load")
    if i < 0:
        sk = re.search(r"^== memory load: skipped[^\n]*", txt, re.M)
        if sk:
            rows.append(("INFO", "memory load", sk.group(0)[3:160]))
        elif pm2 and "== rdna3_vktest" in txt:
            rows.append(("WARN", "memory load", "no memory load in this report although rdna3pm=2 and the Vulkan test ran "
                         "(autoreport.sh older than N43, or the report was cut before it)"))
        return rows
    sec = txt[i + 1:]
    j = sec.find("\n== ", 1)
    sec = sec[:j] if j >= 0 else sec
    bw = re.search(r"^MEMBW: (\d+) submit\(s\).* = ([\d.]+) GiB/s", sec, re.M)
    bad = re.search(r"^MEMBW: (FAILED|NOT MEASURED)[^\n]*", sec, re.M)
    if bad and bad.group(1) == "FAILED":
        rows.append(("FAIL", "memory load", bad.group(0)[:200]))
        return rows
    if bad or not bw:
        rows.append(("WARN", "memory load", (bad.group(0) if bad else "rdna3_vktest --membw printed no `MEMBW:` result "
                                             "(died or was cut off by the time limit)")[:200]))
        return rows
    load = [_n43_pm_sample(b) for b in re.split(r"^-- pmctl ", sec, flags=re.M)[1:] if b.startswith("under load")]
    idle = [_n43_pm_sample(b) for b in re.split(r"^-- pmctl ", sec, flags=re.M)[1:] if b.startswith("idle after")]
    load = [x for x in load if x["now"] is not None or x["uclk"] is not None]
    if not load:
        rows.append(("WARN", "memory load", "%s GiB/s moved, but no `rdna3_pmctl info` sample under the load could be read "
                     "(no pp_dpm_mclk / activity line): UCLK was not judged" % bw.group(2)))
        return rows
    lv0 = min((x["levels"][0] for x in load if x["levels"]), default=None)
    umcs = [x["umc"] for x in load if x["umc"] is not None]
    uclks = [v for x in load for v in (x["now"], x["uclk"]) if v is not None]
    umc, uclk = max(umcs) if umcs else None, max(uclks)
    ilast = idle[-1] if idle else None
    tail = "; after the load: %s" % ("uclk %s MHz, umc %s %%" % (ilast["now"] if ilast["now"] is not None else ilast["uclk"],
                                                              ilast["umc"]) if ilast else "no idle sample")
    desc = ("memory load %s GiB/s (VRAM traffic): umc activity max %s %% (%d sample(s)), UCLK max %d MHz, level 0 is %s MHz%s" %
            (bw.group(2), umc if umc is not None else "?", len(load), uclk, lv0 if lv0 is not None else "?", tail))
    if lv0 is None:
        rows.append(("WARN", "memory load", desc + " - no pp_dpm_mclk levels read, UCLK not judged"))
    elif umc is None:
        rows.append(("WARN", "memory load", desc + " - no `activity ... umc` line read, the load was not confirmed"))
    elif uclk <= lv0 and umc > N43_UMC_LOAD_PCT:
        rows.append(("WARN", "memory load", desc + " - UCLK STAYED at its lowest level under a busy memory controller: the PMFW's "
                     "UCLK DPM does not follow the load (Linux reaches the upper levels; compare rdna3pm=2 and the power profile)"))
    elif uclk <= lv0:
        rows.append(("INFO", "memory load", desc + " - umc activity stayed at or under %d %%: the load did not register at the "
                     "memory controller, UCLK cannot be judged" % N43_UMC_LOAD_PCT))
    else:
        rows.append(("INFO", "memory load", desc + " - UCLK left level 0 under load"))
    return rows


def n41_plugin(cdlog, pluginlog=""):
    """DRIVER-PLAN N41: what WindowServer's Metal plugin said about its marks. Its log is 24-log-coredisplay.txt
    (WindowServer's process; 26-log-metal-plugin.txt holds every other process) - both are read. {"decision": the
    one-time line's text or None, "old": the decision kept the old order, "gates": the ABSENT / PRESENT / headless /
    NVMTL_SURFACE_WAIT=0 gates named, "forgood": ENOSYS, "backoffs": [errno text], "again": marks taken again,
    "refused": [lines], "stats": the last stats line or None, "early": readies sent before their dirty mark, "ready" /
    "ready_end": readies sent and those sent at the command buffer's end (None: an older plugin's stats line)}"""
    text = (cdlog or "") + "\n" + (pluginlog or "")
    out = {"decision": None, "old": False, "gates": [], "forgood": False, "backoffs": [], "again": 0, "refused": [],
           "stats": None, "early": 0, "ready": None, "ready_end": None}
    d = re.findall(r"surface wait \(N41\): WindowServer's IOSurface VRAM writes are ([^\n]*)", text)
    if d:
        out["decision"] = d[-1].strip()
        out["old"] = "old order" in d[-1]
        g = out["gates"]
        for name in ("libdrm_xnu marks", "try-lock dirty", "dirty"):
            if re.search(r"[(,;] ?%s ABSENT" % re.escape(name), d[-1]):
                g.append("%s ABSENT" % name)
        if "NVK surface dirty PRESENT" in d[-1]:
            g.append("the NVK surface-dirty path (its own order)")
        if re.search(r"headless [1-9]", d[-1]):
            g.append("headless")
        if "NVMTL_SURFACE_WAIT=0" in d[-1]:
            g.append("NVMTL_SURFACE_WAIT=0")
    out["forgood"] = "old order for good" in text
    out["backoffs"] = re.findall(r"surface wait \(N41\): pending mark of surface \d+ refused \(errno (\d+, [^)]*)\) - "
                                 r"old order, asked again in 1 s", text)
    out["again"] = len(re.findall(r"surface wait \(N41\): marks taken again", text))
    out["refused"] = re.findall(r"surface wait \(N41\): ((?:pending|ready) mark of surface \d+ refused: errno [^\n]*)",
                                text)
    st = re.findall(r"surface wait \(N41\): stats \([^)]*\): ([^\n]*)", text)
    out["stats"] = st[-1].strip() if st else None
    m = re.search(r"(\d+) ready mark\(s\) sent before the surface's dirty mark", out["stats"] or "")
    out["early"] = int(m.group(1)) if m else (1 if "a ready mark went out before its surface could be marked" in text
                                              else 0)
    m = re.search(r"(\d+) ready mark\(s\) sent \((\d+) at the command buffer's end\)", out["stats"] or "")
    if m:
        out["ready"], out["ready_end"] = int(m.group(1)), int(m.group(2))
    return out


def _n41_plugin_reason(pl):
    """the plugin's own reason for no marks, from n41_plugin()"""
    if pl["forgood"]:
        return "the plugin's pending marks were refused with ENOSYS (a kext without the marks): old order for good"
    if pl["decision"] is None:
        return ("no 'surface wait (N41)' decision line in 24-log-coredisplay.txt: WindowServer's plugin is older than "
                "N41 or never committed a command buffer")
    if pl["old"]:
        return "the plugin kept the old order: %s" % (", ".join(pl["gates"]) or pl["decision"][:160])
    if pl["backoffs"]:
        return "the plugin's pending marks were refused (errno %s) and it backed off" % "; ".join(sorted(set(pl["backoffs"])))
    if pl["stats"] and re.search(r"\b0 pending mark\(s\) taken", pl["stats"]):
        return "the plugin marked nothing: %s" % pl["stats"][:200]
    return "the plugin is on (%s) but no mark reached the kernel" % pl["decision"][:120]


def n41_rows(sv, log, pluginlog="", cdlog=""):
    """DRIVER-PLAN N41: the VRAM parking (vm_*) and the page-off's wait for WindowServer's pending GPU writes
    (surface_wait*) from the sysctl dump in 29-surface-vram.txt, and WindowServer's plugin lines (cdlog, its
    24-log-coredisplay.txt; pluginlog the other processes'). [(level, name, text)]; missing data is a WARN when the
    accelerator runs these sources (the collector's grep lost them; each missing key named), INFO for an older kext."""
    c = dict((k, int(v)) for k, v in N41_SYSCTL.findall(sv or ""))
    cur = accel_current(log)
    rows = []
    if "vm_sticky" not in c:
        if cur:
            rows.append(("WARN", "VRAM parking", "counters missing: the accelerator runs these sources but the dump "
                         "has no rdna3accel_vm_sticky - the collector's sysctl grep (vm_, surface_)"))
        else:
            rows.append(("INFO", "VRAM parking", "no rdna3accel_vm_sticky: an accelerator before DRIVER-PLAN N41"))
        return rows
    g = c.get
    if cur:
        missing = [k for k in N41_VM_KEYS + N41_SW_KEYS if k not in c]
        if missing:
            rows.append(("WARN", "N41 counters", "missing from the dump although the accelerator runs these sources: "
                         + ", ".join(missing)))
    if not g("vm_sticky"):
        rows.append(("WARN", "VRAM parking", "OFF (vm_sticky 0: rdna3vmsticky=0 or a runtime A/B): every deallocPhysical "
                     "frees, imported surfaces move to new VRAM (%d given back)" % g("vm_released", 0)))
    else:
        rows.append(("INFO", "VRAM parking", "%d parked, %d handed back, %d released (%d grown), %d bytes parked at the "
                     "end, %d given back in all" % (g("vm_parked", 0), g("vm_unparked", 0), g("vm_park_released", 0),
                                                    g("vm_park_grow", 0), g("vm_parked_bytes", 0), g("vm_released", 0))))
        if g("vm_dealloc", 0) and not g("vm_parked", 0):
            rows.append(("WARN", "VRAM parking", "vm_sticky 1 but %d deallocPhysical call(s) and NOTHING parked: the "
                         "parking never engaged (the kernel's 'PARKED' lines; scanouts only?)" % g("vm_dealloc")))
    if g("vm_parked", 0) and not g("vm_unparked", 0):
        rows.append(("WARN", "VRAM parking", "%d park(s) and NONE handed back: every allocPhysical after a park got new "
                     "VRAM or none came (the kernel's 'PARKED VRAM handed back' lines; vm_park_grow %d)" % (
                         g("vm_parked"), g("vm_park_grow", 0))))
    if g("vm_parked_bytes", 0) >= N41_PARKED_BYTES_WARN:
        rows.append(("WARN", "VRAM parking", "%d MiB still parked at the end: parks hold VRAM the family thinks it gave "
                     "back (N41 open item 3)" % (g("vm_parked_bytes") >> 20)))
    # ECC review of N41 (L3): with parking vm_dealloc counts parks too and vm_alloc only new allocations - not a leak
    if "vm_alloc" in c and "vm_dealloc" in c:
        rows.append(("INFO", "VRAM parking", "vm_alloc %d (new allocations) / vm_dealloc %d (deallocPhysical calls, parks "
                     "included) / vm_unparked %d: these do not balance with parking; what was really given back is "
                     "vm_released %d (%d MiB), live or parked now about %d" % (
                         g("vm_alloc"), g("vm_dealloc"), g("vm_unparked", 0), g("vm_released", 0),
                         g("vm_released_bytes", 0) >> 20, g("vm_alloc") - g("vm_released", 0) - g("vm_release_failed", 0))))
    if g("vm_release_failed", 0):
        rows.append(("WARN", "VRAM parking", "%d VRAM release(s) the framebuffer did NOT take back: leaked (the kernel's "
                     "'NOT given back' lines)" % g("vm_release_failed")))
    if g("vm_dirty_moved", 0):
        rows.append(("WARN", "VRAM parking", "%d dirty mark(s) of an allocation other than the surface's BO import: the "
                     "plugin drew elsewhere (the kernel's 'marks allocation #' lines)" % g("vm_dirty_moved")))
    if g("vm_restore_miss", 0):
        rows.append(("WARN", "VRAM parking", "%d dirty mark(s) found the VRAM parked and could NOT restore it (refused: "
                     "the plugin copied out; %d restored)" % (g("vm_restore_miss"), g("vm_restored", 0))))
    elif g("vm_restored", 0):
        rows.append(("INFO", "VRAM parking", "%d dirty mark(s) restored parked VRAM first" % g("vm_restored")))
    if g("vm_reparked", 0):
        rows.append(("WARN", "VRAM parking", "%d park(s) replaced an older one - never expected (the kernel's 'an older "
                     "park' lines)" % g("vm_reparked")))
    if g("vm_refused", 0) and g("vm_parked_bytes", 0):
        rows.append(("WARN", "VRAM parking", "%d VRAM allocation(s) refused while %d bytes were parked: parking may "
                     "cost VRAM (the kernel's 'VRAM refused:' line; A/B vm_sticky 0)" % (g("vm_refused"),
                                                                                         g("vm_parked_bytes"))))
    if g("vm_free_live", 0):
        rows.append(("INFO", "VRAM parking", "%d vidmem object(s) freed with VRAM still live, released in free() (they "
                     "leaked before N41)" % g("vm_free_live")))
    if g("vm_apdesc_moved", 0):
        rows.append(("INFO", "VRAM parking", "%d cached aperture descriptor(s) dropped: the VRAM had moved, changed size "
                     "or was released" % g("vm_apdesc_moved")))
    # security review of N41: only a BO's allocation parked; parks no BO holds go under pressure or past the cap
    if g("vm_park_noimport", 0):
        rows.append(("INFO", "VRAM parking", "%d deallocPhysical(s) freed, not parked: no BO was imported over the "
                     "allocation (the kernel's 'freed, not parked' lines)" % g("vm_park_noimport")))
    if g("vm_park_trim_pressure", 0) or g("vm_park_trim_cap", 0):
        rows.append(("WARN" if g("vm_park_trim_pressure", 0) else "INFO", "VRAM parking",
                     "parks released: %d time(s) VRAM ran out, %d time(s) past the cap (%s MiB): %d park(s), %d MiB "
                     "given back, %d kept (a BO holds them); refused allocations asked again: %d granted, %d refused "
                     "again (the kernel's 'VRAM parks:' lines)" % (
                         g("vm_park_trim_pressure", 0), g("vm_park_trim_cap", 0), g("vm_park_cap_mb", "?"),
                         g("vm_park_trimmed", 0), g("vm_park_trimmed_bytes", 0) >> 20, g("vm_park_trim_held", 0),
                         g("vm_alloc_retry_ok", 0), g("vm_alloc_retry_fail", 0))))
    if g("vm_park_trim_unknown", 0):
        rows.append(("WARN", "VRAM parking", "%d park(s) kept because the core's reference count was unknown (an older "
                     "core kext without rdna3VramRefs? the kernel's 'count unknown' lines): they are never given back "
                     "under pressure" % g("vm_park_trim_unknown")))
    # the page-off's wait
    if "surface_wait" not in c:
        rows.append(("WARN" if cur else "INFO", "surface wait",
                     "no rdna3accel_surface_wait in the dump" + (" - the collector's grep" if cur else
                                                                 ": an older accelerator")))
        return rows
    pend, ready, late = g("surface_wait_pending", 0), g("surface_wait_ready", 0), g("surface_wait_late", 0)
    a3m = re.search(r"^debug\.rdna3accel_a3ws: (-?\d+)", sv or "", re.M)
    a3ws = int(a3m.group(1)) if a3m else None
    pl = n41_plugin(cdlog, pluginlog)
    moot = g("surface_wait_moot", 0)
    if not g("surface_wait"):
        rows.append(("INFO", "surface wait", "OFF (surface_wait 0): page-offs do not wait for WindowServer's GPU"))
    elif not pend:
        if a3ws == 0:
            rows.append(("INFO", "surface wait", "MOOT: rdna3a3ws=0 keeps WindowServer's surfaces out of A3; the page-off "
                         "wait was NOT exercised (N41 open item 1)%s" % (
                             "; %d pending mark(s) refused as MOOT (ENXIO: the plugin backs off)" % moot if moot
                             else "")))
        else:
            rows.append(("WARN", "surface wait", "no pending marks taken%s: %s" % (
                " although rdna3a3ws=1" if a3ws else " (rdna3accel_a3ws not in the dump)", _n41_plugin_reason(pl))))
    else:
        rows.append(("INFO", "surface wait", "%d pending / %d ready / %d late marks; page-offs: %d wait(s) (longest %d us), "
                     "%d timeout(s), %d cleared; IOP flips: %d wait(s) (longest %d us), %d timeout(s), %d cleared; %d "
                     "pending now, %d mark(s) refused while off, %d try-lock dirty mark(s) busy" % (
                         pend, ready, late, g("surface_wait_waits", 0), g("surface_wait_maxus", 0),
                         g("surface_wait_timeouts", 0), g("surface_wait_cleared", 0), g("surface_wait_iop_waits", 0),
                         g("surface_wait_iop_maxus", 0), g("surface_wait_iop_timeouts", 0),
                         g("surface_wait_iop_cleared", 0), g("surface_wait_busy", 0), g("surface_wait_off", 0),
                         g("surface_dirty_trybusy", 0))))
    to = g("surface_wait_timeouts", 0) + g("surface_wait_iop_timeouts", 0)
    if to:
        rows.append(("WARN", "surface wait", "%d page-off(s) and %d IOP flip(s) gave up waiting for WindowServer's GPU "
                     "write (ready marks lost or a hung GPU; the kernel's 'still PENDING' lines)" % (
                         g("surface_wait_timeouts", 0), g("surface_wait_iop_timeouts", 0))))
    cl = g("surface_wait_cleared", 0) + g("surface_wait_iop_cleared", 0)
    if cl:
        rows.append(("WARN", "surface wait", "%d wait(s) cleared under them (another wait's timeout, a full table or the "
                     "switch-off): not finished writes (the kernel's 'was CLEARED' lines)" % cl))
    if pend and not ready:
        rows.append(("FAIL", "surface wait", "%d pending mark(s) and NO ready mark: every page-off of those surfaces ran "
                     "into the bound (the plugin's 'surface wait (N41)' lines)" % pend))
    unacc = pend - ready - late - g("surface_wait_evicted", 0)
    if pend and ready and unacc > N41_UNACCOUNTED_WARN:
        rows.append(("WARN", "surface wait", "%d pending mark(s) without a ready or late one (pending %d - ready %d - late "
                     "%d - evicted %d; %d pending at the dump): ready marks lost" % (
                         unacc, pend, ready, late, g("surface_wait_evicted", 0), g("surface_wait_busy", 0))))
    if g("surface_wait_notws", 0):
        rows.append(("WARN", "surface wait", "%d mark(s) from a process other than WindowServer refused (an app's plugin "
                     "with NVMTL marks? the kernel's 'only WindowServer' lines)" % g("surface_wait_notws")))
    if g("surface_wait_notreal", 0):
        rows.append(("WARN", "surface wait", "%d mark(s) from a process named WindowServer REFUSED: neither uid 88 "
                     "(_windowserver) nor the display client's pid %d (a process posing as WindowServer, or the real "
                     "one under another uid before its first swap; the kernel's 'neither _windowserver' lines)" % (
                         g("surface_wait_notreal"), g("surface_wait_ws_pid", 0))))
    if g("surface_wait_ws_pid_refused", 0):
        rows.append(("WARN", "surface wait", "%d legacy swap(s) / display-pipe transaction(s) of a SECOND process named "
                     "WindowServer while the display client pid %d lived: refused as the display client (a process "
                     "posing as WindowServer, or two WindowServers; the kernel's 'REFUSED as the display client' "
                     "lines)" % (g("surface_wait_ws_pid_refused"), g("surface_wait_ws_pid", 0))))
    if g("surface_wait_mark_uid", -1) >= 0:
        uid = g("surface_wait_mark_uid")
        rows.append(("INFO" if uid == 88 else "WARN", "surface wait",
                     "the first mark taken came from pid %d uid %d (%s; display client pid %d)" % (
                         g("surface_wait_mark_pid", 0), uid, "_windowserver" if uid == 88 else
                         "NOT uid 88: taken as the display client - the uid rule did not hold on this macOS",
                         g("surface_wait_ws_pid", 0))))
    if g("surface_wait_starved", 0) or g("surface_wait_skipped", 0):
        rows.append(("WARN", "surface wait", "the timeout budget ran out %d time(s) (waits in a row timed out with no "
                     "ready mark): %d page-off(s) / IOP flip(s) did NOT wait (ready marks lost; the kernel's 'STOP "
                     "waiting' lines)" % (g("surface_wait_starved", 0), g("surface_wait_skipped", 0))))
    if g("surface_wait_ghosts", 0) or g("surface_wait_aged", 0):
        rows.append(("INFO", "surface wait", "lost ready marks: %d pending mark(s) dropped for good at a second "
                     "timeout (a newer write's ready took the forgiven mark), %d forgiven mark(s) aged out" % (
                         g("surface_wait_ghosts", 0), g("surface_wait_aged", 0))))
    if g("surface_wait_notown", 0):
        rows.append(("WARN", "surface wait", "%d pending mark(s) of a surface WindowServer could not look up refused (the "
                     "kernel's 'not a surface of the calling task' lines)" % g("surface_wait_notown")))
    if g("surface_wait_evicted", 0) or g("surface_wait_saturated", 0):
        rows.append(("WARN", "surface wait", "%d surface(s) dropped from the full table, %d mark(s) refused as saturated"
                     "; %d mark(s) of dropped surfaces forgiven when they were marked again (their late readies "
                     "could not end a newer wait)" % (g("surface_wait_evicted", 0), g("surface_wait_saturated", 0),
                                                       g("surface_wait_evict_kept", 0))))
    # WindowServer's plugin (24-log-coredisplay.txt): every reason it gave
    if g("surface_wait") and pl["decision"] and pl["old"]:
        rows.append(("WARN", "surface wait", "WindowServer's plugin keeps the old order: %s" % (
            ", ".join(pl["gates"]) or pl["decision"][:160])))
    if pl["forgood"]:
        rows.append(("WARN", "surface wait", "WindowServer's plugin: pending marks refused with ENOSYS - old order for good "
                     "(a kext without the marks)"))
    if pl["backoffs"]:
        # the MOOT refusals (security review of N41) are ENXIO back-offs by design: INFO when they explain them all
        moot_only = moot and not pend and all(b.startswith("6,") for b in pl["backoffs"])
        rows.append(("INFO" if moot_only else "WARN", "surface wait",
                     "WindowServer's plugin backed off %d time(s) (errno %s; asked again in 1 s), marks taken again %d "
                     "time(s)%s" % (len(pl["backoffs"]), "; ".join(sorted(set(pl["backoffs"]))), pl["again"],
                                    " - the MOOT refusals" if moot_only else "")))
    if pl["refused"]:
        rows.append(("WARN", "surface wait", "WindowServer's plugin: %d mark(s) refused, e.g. %s" % (
            len(pl["refused"]), pl["refused"][0][:140])))
    if pl["early"]:
        rows.append(("WARN", "surface wait", "WindowServer's plugin sent %d ready mark(s) before the surface could be "
                     "marked newest: a page-off in between copied stale memory" % pl["early"]))
    if pl["stats"]:
        rows.append(("INFO", "surface wait", "WindowServer's plugin: " + pl["stats"][:300]))
    if pl["ready_end"]:
        rows.append(("INFO", "surface wait", "WindowServer's plugin sent %d of its %d ready mark(s) at the command "
                     "buffer's end (completion or error), not after the part that wrote the surface last: those "
                     "page-offs waited for the whole command buffer" % (pl["ready_end"], pl["ready"])))
    return rows


def n41_lines(path, args=""):
    """[(level, name, text)] of n41_rows from the report archive, for the SUMMARY"""
    if not os.path.exists(path):
        return []
    with tarfile.open(path) as tar:
        sv = read_member(tar, "29-surface-vram.txt") or ""
        log = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt") or ""
        cd = read_member(tar, "24-log-coredisplay.txt") or ""
        pl = read_member(tar, "26-log-metal-plugin.txt") or ""
    if "rdna3accel=1" not in args and not sv.strip():
        return []
    return n41_rows(sv, log, pl, cd)


def a3_display_verdict(log, sv):
    """card run 58: WindowServer's display framebuffers and A3 - kept out
    (the refusal lines), the refusal that never fired (display-size BOs in
    WindowServer, no refusal), or let in and never scanned out. Prints the
    verdict; True when it failed."""
    a3lines = {}
    for l in log.splitlines():
        m = re.search(r"a3 display: head (\d+): .*$", l)
        if m:
            a3lines[m.group(1)] = m.group(0)
    kept_mode = "display framebuffers are kept out of A3" in log
    let_in = "display framebuffers MAY become BOs" in log
    if not (kept_mode or let_in or a3lines):
        return False   # a kernel before card run 58
    refused = len(set(re.findall(r"(?:surfgem|surfvram): surface (\d+) plane \d+ refused: WindowServer's display "
                                 r"framebuffer", log + "\n" + sv)))
    c = a3_counters(sv) or {}   # the sysctl dump at the end of 29-surface-vram.txt (log lines are capped)
    refused = max(refused, c.get("a3display_refused", 0))
    # the heads' scanout sizes, and the plugin's display-size BOs in WindowServer
    sizes = scanout_sizes(log)
    cd = sv.split("== 24-log-coredisplay.txt", 1)[1] if "== 24-log-coredisplay.txt" in sv else ""
    ws_bos = set(sid for sid, w, h in re.findall(r"item 7: surface (\d+) (\d+)x(\d+) pitch \d+ -> the family's VRAM", cd)
                 if (w, h) in sizes)
    apart = [l for l in a3lines.values() if "DOES NOT READ" in l]
    # the marker counts only when the rule looked at something (kernels of
    # aa913df printed it with "0 other WindowServer surfaces, 0 unchecked" too)
    never = []
    for l in a3lines.values():
        m = re.search(r"(\d+) other WindowServer surfaces, (\d+) unchecked", l)
        if "the refusal never fired" in l and (not m or int(m.group(1)) + int(m.group(2)) > 0):
            never.append(l)
    # debug.rdna3accel_iop_flip=1 flips the IOSurface's own VRAM: the rule is off by design
    iop_flip = "debug.rdna3accel_iop_flip=1 flips IOSurfaces" in log or \
        bool(re.search(r"^debug\.rdna3accel_iop_flip: [1-9]", sv, re.M))
    failed = False
    if apart:
        print("   FAIL %-20s WindowServer draws display surfaces into A3 BOs no legacy swap scans out "
              "(rdna3a3display=1): the panel keeps an old frame" % "A3 display")
        failed = True
    elif iop_flip and not refused:
        print("   INFO %-20s debug.rdna3accel_iop_flip=1: display surfaces may be BOs (that path flips the "
              "IOSurface's own VRAM)" % "A3 display")
    elif kept_mode and (never or (ws_bos and not refused)):
        print("   FAIL %-20s the refusal never fired: %d WindowServer surface(s) of a scanout size became BOs (%s), "
              "0 refused - the check missed them (the 'surfgem: WindowServer surface ... is not a display "
              "framebuffer' / 'did not run' lines say why)" % ("A3 display", len(ws_bos),
                                                                ", ".join(sorted(ws_bos, key=int)[:6]) or "?"))
        failed = True
    elif kept_mode and refused:
        print("   INFO %-20s WindowServer's display framebuffers kept out of A3: %d surface(s) refused, composited "
              "through copies (card run 58)" % ("A3 display", refused))
    elif kept_mode:
        print("   INFO %-20s kept-out mode, but no display framebuffer import seen (none refused, none shared)" %
              "A3 display")
    else:
        print("   INFO %-20s display surfaces may become BOs (rdna3a3display=1)" % "A3 display")
    ws_kept = len(set(re.findall(r"(?:surfgem|surfvram): surface (\d+) plane \d+ refused: a WindowServer input",
                                 log + "\n" + sv)))
    ws_kept = max(ws_kept, c.get("a3ws_refused", 0))
    if "A3: OFF for every process" in log:
        print("   INFO %-20s A3 off for every process (rdna3a3=0): %d import(s) refused" %
              ("A3 WindowServer", c.get("a3off_refused", 0)))
    elif "WindowServer's other inputs MAY become BOs" in log:
        print("   INFO %-20s WindowServer's inputs may be BOs (rdna3a3ws=1: the 2026-10-15d flicker)" % "A3 WindowServer")
    elif "WindowServer's inputs are kept out" in log:
        # every WindowServer import is refused in this mode: one that went
        # into the family's VRAM all the same, or more non-display imports
        # than refusals, is a leak (an unnamed caller is refused too)
        ws_in = re.findall(r"item 7: surface (\d+) \d+x\d+ pitch \d+ -> the family's VRAM", cd)
        other = c.get("a3display_other", 0)
        unknown = c.get("a3display_task_mismatch", 0) + c.get("a3display_noname", 0)
        if ws_in:
            print("   FAIL %-20s WindowServer input leaked into A3: %d surface(s) in the family's VRAM in WindowServer "
                  "(%s) although its inputs are kept out" % ("A3 WindowServer", len(set(ws_in)),
                                                             ", ".join(sorted(set(ws_in), key=int)[:6])))
            failed = True
        elif c and other > c.get("a3ws_refused", 0):
            print("   FAIL %-20s WindowServer input leaked into A3: %d non-display import(s), %d refused" %
                  ("A3 WindowServer", other, c.get("a3ws_refused", 0)))
            failed = True
        else:
            print("   INFO %-20s WindowServer's inputs kept out of A3: %d refused (card run 58, 2026-10-15d)%s" %
                  ("A3 WindowServer", ws_kept, "; %d import(s) by a process the kernel could not name, refused" %
                   unknown if unknown else ""))
    for h in sorted(a3lines):
        print("     " + a3lines[h][:300])
    for l in sorted(set(re.findall(r"flicker: head \d+: [^\n]*", log)))[-4:]:
        print("     " + l[:220])
    for l in sorted(set(re.findall(r"surfgem: (?:WindowServer surface \d+ .*? is not a display framebuffer|surface \d+ .*?"
                                   r"display check did not run)[^\n]*", log)))[:4]:
        print("     " + l[:200])
    return failed


A3_SYSCTL = re.compile(r"^debug\.rdna3accel_(a3display(?:_\w+)?|a3ws(?:_refused)?|a3off_refused|a3|flicker|"
                       r"swaps(?:_a3|_abab|_ababwide|_hashed|_hashskip|_frontskip|_skipped|_emptyshown|_bufchg|"
                       r"_latchpend|_latchwait|_latchus|_latchmax|_latchtimeout|_latchunknown|_latchoff|_tornchk|"
                       r"_torn|_inplace|_tornskip|_shownfreed|_emptyfilled|_emptystill|_latchimmediate|_latchwaitsoff|_latchsame|"
                       r"_flipfailed|_copy|_copyempty|_copylanded|_copylanded_would|_stalehold|_stalehold_would|_stalehold_miss|"
                       r"_stalehold_changed|_stalenocf|_stalereset|_staleevict|_stalegone|"
                       r"_stalecheck_(?:norecord|changed|cf|sameshown|nopanel|nohead)|_copyblack|_copybytes|_copyus|_copyusmax|_copywait|_copywaitus|"
                       r"_copylatchto|_copyfail|_copydirect|_copyfront|_copyfrontfail|_copynowait|_copybrkflip|_copyfreewait|"
                       r"_genpresent|_genhold|_genhold_would|_genfirst|_gencontent|_genunwritten|_gen_judged|_gen_aged|"
                       r"_gen_evictpresent|_gen_landed|_gennotjudged|_gen_resets|"
                       r"_pq_(?:prog|queued|replaced|joined|busy|refused|latched|latchus|latchmax|next|timeout|unknown|"
                       r"dropped|dropmode|early|wait|waitus|waitmax|waittimeout|inplace|inline|polled|iop|"
                       r"foreigntimeout|giveup|iopdrop|fbwaitto|fbwaitfast|freedinflight|home|"
                       r"irqdone|irqlatus|irqlatmax|irqlive|fbwaitv1))?_h\d|"
                       r"swaps_pq_iopunwritten|swaps_pq_donemax|swap_async|iop_queue|iop_wrote|present_init_failed|"
                       r"perfstats|perf_(?:writes|nocore|nokey|dictfail|setfail|rings|smu|noanswer|util|utilgfx|jobs|flushed|lostns|"
                       r"gpuns|nosubns|nosub|creditfail|stale|merged|lostjobs|inflight|cappedns|ringns_(?:gfx|comp|sdma)|"
                       r"unc_(?:vcn|jpeg|other|kcopy|userq)|sdmaend_(?:irq|wait|take|sub|reset)|lastret|winreset|"
                       r"submitters|uccbad|cmdqueues|creditwait|pendingns|nolast)|"
                       r"swaps_gen_(?:aperture|apcached|cf|cfnostamp|evicted|freed|resets)|swap_gen|flipbuf_present|cf_gennewer_h\d|"
                       r"flipbuf(?:_scanout|_presented|_held|_failed|_busy|_unlocked|_emptyheld)?_h\d|"
                       r"flipbuf_(?:nohead|nores|intoff|badptr|busy|unlocked)|"
                       r"hook_(?:intr_(?:flipbuf|scanout|pickpresent|didsubmitswap|backready|submitupdate)|intr|intoff|pickpresent|"
                       r"didsubmitswap|backready|backready_no|submitupdate)|"
                       r"swaps_copybrktrips|swap_latch|swap_torn|swap_samenoflip|swap_landed|swap_stale|"
                       r"swap_mode|trace_scan|swap_trace_events|swap_trace_nomem|swap_trace_lost|cf_sc(?:src|scanout|stale)_h\d|"
                       r"bufs_new_h\d|cf_(?:shown|src|backlast|nothingshown|foreign|miss|healed|onpanel)_h\d|swaps_skipped_all|"
                       r"swaps_nohead|cf_(?:calls|nohead|shown_stale|refused|same|disabled|pend_blocked|pend_forced|pend_evicted|"
                       r"pend_whole|nohead_miss)|iop_flip): (\d+)", re.M)
# the kernel's refusal reasons that are the A3 policy (rd3AccelA3Why), not a fault
A3_POLICY_REASONS = ("WindowServer's display framebuffer", "a WindowServer input", "A3 is off",
                     "imported for another task", "a process the kernel could not name")
FLICKER_SHARE = 0.25   # of a head's swaps showing the picture of two swaps before: flicker


def gamma_unjudged(txt):
    """(level, text) for 'GAMMA: UNJUDGED' (the screens kept changing): the
    core's RDNA3,Head<n>,Gamma record before and after the test decides -
    every head took the test's tables (its call count grew by 3 or more:
    dark, identity, ColorSync's restore) is WARN (open, not a pass), else
    FAIL"""
    def calls(section):
        m = re.search(r"^== gamma property %s\n(.*?)(?=^== |^\[exit|\Z)" % section, txt, re.M | re.S)
        return dict((h, int(n)) for h, n in
                    re.findall(r'"RDNA3,Head(\d),Gamma" = "[^"]*?(\d+) calls"', m.group(1))) if m else None
    before, after = calls("before"), calls("after")
    if not after:
        return "FAIL", ("the screens kept changing (CRC before not stable) and no RDNA3,Head<n>,Gamma record came "
                        "with the test: nothing shows that the tables reached the pipes")
    grew = dict((h, n - (before or {}).get(h, 0)) for h, n in after.items())
    short = sorted(h for h, d in grew.items() if d < 3)
    if short:
        return "FAIL", ("the screens kept changing, and head(s) %s took fewer than 3 tables during the test (%s)" %
                        (", ".join(short), ", ".join("head %s +%d" % kv for kv in sorted(grew.items()))))
    return "WARN", ("open: the screens kept changing (card run 58: flicker), so the CRCs prove nothing; the core "
                    "took the tables (%s) - judge again on still screens" %
                    ", ".join("head %s +%d" % kv for kv in sorted(grew.items())))


def kernel_text(report, d):
    """the kernel log of a report: the archive's 20-log-rdna3.txt and the
    serial logs next to it"""
    out = ""
    if os.path.exists(report):
        try:
            with tarfile.open(report) as tar:
                out = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt") or ""
        except (OSError, tarfile.TarError):
            pass
    # the newest serial log only: an older boot's "tr" lines and counters
    # are another run's (ECC review round 1)
    serials = sorted(glob.glob(os.path.join(d, "serial-*.log")), key=os.path.getmtime)
    for f in serials[-1:]:
        try:
            out += "\n" + open(f, errors="replace").read()
        except OSError:
            pass
    return out


def a3_counter_dumps(txt):
    """[(name, {counter: value})] of every card run 58 sysctl dump in a tests
    file or 29-surface-vram.txt, in order"""
    out = []
    for m in re.finditer(r"^== (display pipe and AGDC counters|A3 display counters before the probes|"
                         r"A3 display counters after M6|sysctl \(card run 58\))\n"
                         r"(.*?)(?=^== |\Z)", txt, re.M | re.S):
        c = dict((k, int(v)) for k, v in A3_SYSCTL.findall(m.group(2)))
        if c:
            out.append((m.group(1), c))
    if not out and A3_SYSCTL.search(txt):   # counters without a known header: one dump
        out.append(("the counters", dict((k, int(v)) for k, v in A3_SYSCTL.findall(txt))))
    return out


def flicker_verdict(txt_or_counters):
    """[(level, text)] from the A B A B counters: per head the swaps whose
    picture alternated against the swaps hashed (swaps_hashed_h<N>; older
    kernels: all swaps), as the change between the first and the last dump
    when there are two (before the probes and after M6), else since boot.
    One band taking turns is a blinking caret or one status item: a head
    over the line is FAIL only when more than one band alternated in that
    share of its swaps (swaps_ababwide_h<N>), or when two heads or more are
    over the line; else WARN"""
    if isinstance(txt_or_counters, dict):
        dumps = [("the counters", txt_or_counters)]
    else:
        dumps = a3_counter_dumps(txt_or_counters)
    if not dumps:
        return [("INFO", "NOT MEASURED: no counters in this report")]
    last_name, last = dumps[-1]
    # the dump right before the probes (the flicker test turned on for them)
    # when the run has one, else the first
    first_name, first = next((d for d in dumps[:-1] if d[0] == "A3 display counters before the probes"),
                             dumps[0] if len(dumps) > 1 else (None, {}))
    span = "between '%s' and '%s'" % (first_name, last_name) if first_name else "since boot ('%s')" % last_name
    out, over = [], []
    heads = sorted(set(m.group(1) for m in (re.match(r"swaps_(?:hashed_)?h(\d)$", k) for k in last) if m))
    for h in heads:
        if not last.get("swaps_h" + h, 0) and not last.get("swaps_hashed_h" + h, 0):
            continue   # a head that never swapped (no monitor)
        den_key = "swaps_hashed_h" + h if "swaps_hashed_h" + h in last else "swaps_h" + h
        den = last.get(den_key, 0) - first.get(den_key, 0)
        ab = last.get("swaps_abab_h" + h)
        swaps = last.get("swaps_h" + h, 0) - first.get("swaps_h" + h, 0)
        if ab is None:
            out.append(("INFO", "head %s: NOT MEASURED (a kernel without the flicker test)" % h))
            continue
        ab -= first.get("swaps_abab_h" + h, 0)
        if den <= 0:
            if swaps > 0 or last.get("swaps_h" + h, 0):
                out.append(("INFO", "head %s: NOT MEASURED %s: no hashed swaps (debug.rdna3accel_flicker=0, the test's "
                                    "budget spent, or every swap skipped: swaps_hashskip_h%s %d)" %
                            (h, span, h, last.get("swaps_hashskip_h" + h, 0))))
            continue
        share = ab / float(den)
        what = "%d of %d %s swaps %s" % (ab, den, "hashed" if den_key.startswith("swaps_hashed") else "legacy", span)
        if den < 60:
            out.append(("INFO", "head %s: not judged, %s (fewer than 60)" % (h, what)))
        elif share >= FLICKER_SHARE:
            wide = last.get("swaps_ababwide_h" + h)
            if wide is not None:
                wide -= first.get("swaps_ababwide_h" + h, 0)
            over.append((h, what, share, wide, den))
        elif ab:
            out.append(("PASS", "head %s: %s A B A B (%.0f%%, under %.0f%%)" % (h, what, share * 100,
                                                                              FLICKER_SHARE * 100)))
        else:
            out.append(("PASS", "head %s: no A B A B in the sampled pixels (%s)" % (h, what)))
    for h, what, share, wide, den in over:
        text = "head %s: alternating pictures (candidate flicker): %s showed the picture of two swaps before " \
               "(A B A B, %.0f%%)" % (h, what, share * 100)
        if wide is None:
            bands = "this kernel does not count the bands"
        else:
            bands = "%d of them in more than one band" % wide
        if wide is not None and wide / float(den) >= FLICKER_SHARE:
            out.append(("FAIL", "%s, %s" % (text, bands)))
        elif len(over) > 1:
            out.append(("FAIL", "%s, %s; on %d heads" % (text, bands, len(over))))
        else:
            out.append(("WARN", "%s, %s: one band taking turns on one head - a blinking caret or one status item, "
                                "or flicker in a small area" % (text, bands)))
    return out


SERIAL_SWAP_LINE = re.compile(r"(flicker: head \d+: [^\n]*|swap: head \d+: \d+ swaps over[^\n]*|"
                              r"a3 display: head \d+: \d+ legacy swaps \(\d+ A B A B of \d+ hashed\)|"
                              r"copy forward #\d+: (?:head \d+: )?(?:from the buffer it shows|the buffer it showed|"
                              r"the target \(phys 0x[0-9a-f]+\) is the buffer the head showed last|"
                              r"\d+ rect\(s\) the target|the front \(phys[^\n]*holds the last frame|"
                              r"\d+ of \d+ rect\(s\) not copied|no head known|head \d+ has shown nothing|"
                              r"head \d+ shows a buffer that is not|the target \(phys[^\n]*not copyable|refused|"
                              r"the front \(phys[^\n]*not copyable|pixel \(\d+,\d+\)[^\n]*did NOT land)[^\n]*|"
                              r"swap: head \d+: the back \(phys[^\n]*|swap: no front named[^\n]*|"
                              r"swap #\d+: head \d+ shows a new buffer[^\n]*|swap #\d+: head \d+: the back[^\n]*|"
                              r"swap: surface [^\n]*matched no head[^\n]*|"
                              r"swap latch: (?:head \d+(?: \(core head \d+\))?: |a swap returns|OFF: a swap)[^\n]*|"
                              r"swap same: [^\n]*|"
                              r"flip to VRAM [^\n]*still pending after 20 ms[^\n]*|"
                              r"swap #\d+: head \d+: the (?:previous flip|flip to phys|buffer it showed)[^\n]*"
                              r"(?:NOT latched|changed while on the panel)[^\n]*|"
                              r"swap #\d+: head \d+: (?:the flip state could not be read|the torn test skipped|"
                              r"the flip to phys [^\n]*was refused)[^\n]*|"
                              r"copy forward #\d+: head \d+: the target [^\n]*STILL ON THE PANEL[^\n]*|"
                              r"swap: head \d+: the buffer [^\n]*was FREED[^\n]*|"
                              r"swap: head \d+: the empty back [^\n]*read again[^\n]*|"
                              # card run 2026-10-17c: the landed buffer's and the stale back's decisions
                              r"swap #\d+: head \d+: (?:would hold the stale back|would present landed buffer|"
                              r"the stale-back table is full|the stale back )[^\n]*|"
                              r"swap copy: head \d+: (?:the stale-back records forgotten|the back at 0x[0-9a-f]+ was "
                              r"freed)[^\n]*|swap (?:stale|landed): [^\n]*|"
                              r"copy forward #\d+: the front buffer \(vm [^)]*\) was never shown[^\n]*)")
SERIAL_SUMMARY = re.compile(
    r"swap: head (\d+): (\d+) swaps over (\d+) buffer\(s\) now \([^)]*\), (\d+) distinct so far, (\d+) buffer changes; "
    r"front \(framebuffer resource\) [^;]*; (\d+) skipped \(empty back\), (\d+) of them with pixels in the front, (\d+) "
    r"empty backs shown; copy forward: (\d+) from the shown buffer, (\d+) from the front, (\d+) not needed \(one "
    r"back\), (\d+) nothing shown yet, (\d+) another surface's, (\d+) missed, (\d+) healed; all heads: (\d+) no head, "
    r"(\d+) shown buffer not usable, (\d+) refused, (\d+) swaps with no head; flicker (\d+) A B A B of (\d+) hashed, "
    r"(\d+) in more than one band")


def serial_swap_verdict(text):
    """([(level, text)], [lines]) from the kernel's swap, copy forward and
    flicker lines in a serial log (card run 58b, 2026-10-16: the VM was
    stopped before the report, the serial log was all there was).
    Flicker: the kernel prints its counts only now and then ('flicker:' at
    the 8th / 120th / 1200th A B A B, 'a3 display:' and the 'swap: head N'
    summary at the 8th / 120th / 1200th swap), so per head the snapshot with
    the most hashed swaps is judged, labelled with its swap: A B A B in 25 %
    or more of 60+ hashed swaps is FAIL when more than one band alternated
    in 25 % or two heads are over the line, else WARN; under the line only
    INFO (later swaps are not in the log). Copy forward and the swaps: the
    last summary line per head (WARN when copies were impossible or missed,
    a head took turns between buffers with no copy forward, the front had
    pixels the back did not). A kernel before rdna3_swap_front.h that
    skipped copy forward ('front buffer ... was never shown') while a head's
    swaps took turns between two buffers: WARN, the cause of that run's
    flicker."""
    out = []
    snap = {}   # head -> (hashed, abab, wide or None, swap or None, source)

    def note(h, den, ab, wide, swap, src):
        old = snap.get(h)
        if old is None or den > old[0] or (den == old[0] and old[2] is None and wide is not None):
            snap[h] = (den, ab, wide, swap, src)
    for m in re.finditer(r"flicker: head (\d+): swap (\d+) showed alternating pictures \(A B A B in \d+ of \d+ bands"
                         r"[^)]*\), (\d+) of (\d+) hashed swaps so far(?:, (\d+) in more than one band)?", text):
        note(m.group(1), int(m.group(4)), int(m.group(3)), int(m.group(5)) if m.group(5) else None,
             int(m.group(2)), "flicker:")
    for m in re.finditer(r"a3 display: head (\d+): (\d+) legacy swaps \((\d+) A B A B of (\d+) hashed\)", text):
        note(m.group(1), int(m.group(4)), int(m.group(3)), None, int(m.group(2)), "a3 display:")
    summ = {}
    for m in SERIAL_SUMMARY.finditer(text):
        summ[m.group(1)] = [int(x) for x in m.groups()[1:]]
        g = summ[m.group(1)]
        note(m.group(1), g[19], g[18], g[20], g[0], "swap: head summary")
    stopped = dict(re.findall(r"flicker: head (\d+): the test stopped after (\d+) hashed swaps", text))
    over = []
    for h, (den, ab, wide, swap, src) in sorted(snap.items()):
        at = "at swap %s ('%s')" % (swap, src) if swap else "('%s')" % src
        what = "%d of %d hashed swaps A B A B%s %s" % (ab, den, "" if wide is None else
                                                      ", %d in more than one band" % wide, at)
        if den < 60:
            out.append(("INFO", "head %s: not judged, %s (fewer than 60)" % (h, what)))
        elif ab / float(den) >= FLICKER_SHARE:
            over.append((h, what, wide is not None and wide / float(den) >= FLICKER_SHARE))
        else:
            out.append(("INFO", "head %s: %s, under %.0f%% so far (the log has no later count)%s" %
                        (h, what, FLICKER_SHARE * 100, "; the test stopped after %s hashed swaps" % stopped[h]
                         if h in stopped else "")))
    for h, what, wide in over:
        if wide or len(over) > 1:
            out.append(("FAIL", "head %s: alternating pictures (candidate flicker): %s%s" %
                        (h, what, "" if wide else "; on %d heads" % len(over))))
        else:
            out.append(("WARN", "head %s: alternating pictures in one band (a caret, or flicker in a small area): %s" %
                        (h, what)))
    seen = set(re.findall(r"swap(?: #\d+)?: (?:surface \S+ -> )?head (\d+)", text))
    for h in sorted(seen - set(snap)):
        out.append(("INFO", "head %s: flicker not measured (no count in the log)" % h))
    for h, g in sorted(summ.items()):
        (swaps, nb, distinct, chg, skipped, frontpx, emptyshown, shown, front, notneeded, nothing, foreign, missed,
         healed, nohead, stale, refused, swapnohead) = g[:18]
        what = ("head %s at swap %d: %d distinct buffer(s), %d changes; copy forward %d from the shown buffer, %d from "
                "the front, %d not needed, %d nothing shown yet, %d another surface's, %d missed, %d healed; %d skipped "
                "(%d with pixels in the front); all heads %d no head, %d not usable, %d refused, %d swaps with no head" %
                (h, swaps, distinct, chg, shown, front, notneeded, nothing, foreign, missed, healed, skipped, frontpx,
                 nohead, stale, refused, swapnohead))
        why = []
        if nohead + stale + refused:
            why.append("copies impossible or refused")
        if missed > healed:
            why.append("missed copies not healed yet")
        if distinct >= 2 and chg and shown + front + notneeded == 0:
            why.append("buffers take turns with no copy forward")
        if frontpx:
            why.append("the front had pixels the back did not")
        if foreign:
            why.append("the head showed another surface's buffer")
        out.append(("WARN" if why else "INFO", "copy forward / swaps: %s%s" % (what, " - " + ", ".join(why) if why else "")))
    # an older kernel: two backs taking turns while copy forward copied nothing
    moves = {}
    for h, a, b in re.findall(r"swap #\d+: head (\d+) frame phys (0x[0-9a-f]+) -> (0x[0-9a-f]+)", text):
        if a != "0x0":
            moves.setdefault(h, set()).add((a, b))
    # taking turns: some buffer went A -> B and back B -> A
    turns = sorted(h for h, m in moves.items() if any((b, a) in m for a, b in m))
    if turns and "was never shown -- the back buffer already holds the last frame" in text:
        out.append(("WARN", "head(s) %s: two back buffers took turns while copy forward copied nothing ('the front "
                            "buffer ... was never shown'): each back kept what changed two frames before - the A B A B "
                            "flicker of card run 58b; a kernel with rdna3_swap_front.h copies from the shown buffer" %
                    ", ".join(turns)))
    out.extend(serial_latch_verdict(text, seen))
    out.extend(serial_present_verdict(text))
    lines = []
    for l in SERIAL_SWAP_LINE.findall(text):
        if l not in lines:
            lines.append(l)
    # the earliest lines say how it started, the last where it ended
    return out, (lines if len(lines) <= 20 else lines[:6] + ["..."] + lines[-14:])


# a head that skipped this many swaps and showed none is a failure (fewer:
# the first swaps of a boot, before anything is drawn; the kernel shows an
# empty back after RD3_SWAP_EMPTY_RUN skips anyway)
SKIP_FAIL_MIN = 10


def copy_forward_verdict(c):
    """[(level, text)] from the card run 58b swap and copy forward counters,
    [] on a kernel without them. Per head the copy forward outcomes; WARN
    when copies were impossible (no head, the shown buffer not usable,
    refused), missed and not healed, the head showed another surface's
    buffer, or more than 10 calls found nothing shown. Swaps: WARN when a
    head skipped 10 % or more of its swaps or a swap matched no head, FAIL
    when a head skipped swaps and showed none."""
    if not any(k.startswith("cf_") for k in c):
        return []
    out = []
    heads = sorted(set(m.group(1) for m in (re.match(r"(?:cf_\w+|swaps_\w+|bufs_new)_h(\d)$", k) for k in c) if m))
    per, why = [], []
    for h in heads:
        g = lambda k: c.get(k + "_h" + h, 0)
        if not any(g(k) for k in ("cf_shown", "cf_src", "cf_backlast", "cf_nothingshown", "cf_foreign", "cf_miss")):
            continue
        per.append("h%s %d from the shown buffer / %d not needed / %d from the front / %d missed (%d healed) / %d "
                   "nothing shown yet / %d another surface's" % (h, g("cf_shown"), g("cf_backlast"), g("cf_src"),
                                                              g("cf_miss"), g("cf_healed"), g("cf_nothingshown"),
                                                              g("cf_foreign")))
        if g("cf_miss") > g("cf_healed"):
            why.append("head %s missed copies not healed" % h)
        if g("cf_foreign"):
            why.append("head %s showed another surface's buffer" % h)
        if g("cf_nothingshown") > 10:
            why.append("head %s: %d calls with nothing shown" % (h, g("cf_nothingshown")))
    if c.get("cf_nohead_miss", 0):
        why.append("%d call(s) with no head left rects stale" % c["cf_nohead_miss"])
    if c.get("cf_pend_evicted", 0):
        why.append("%d pending-rect list(s) dropped from a full table" % c["cf_pend_evicted"])
    if c.get("cf_pend_forced", 0):
        why.append("%d pending copies forced from a source that missed them too (stale alike)" % c["cf_pend_forced"])
    glob_bad = c.get("cf_nohead", 0) + c.get("cf_shown_stale", 0) + c.get("cf_refused", 0)
    if glob_bad:
        why.append("%d with no head, %d shown buffer not usable, %d refused" % (
            c.get("cf_nohead", 0), c.get("cf_shown_stale", 0), c.get("cf_refused", 0)))
    out.append(("WARN" if why else "INFO", "%s; %d calls%s" % ("; ".join(per) or "no copy forward",
                                                              c.get("cf_calls", 0), (" - " + "; ".join(why)) if why else "")))
    for h in heads:
        swaps, skipped = c.get("swaps_h" + h, 0), c.get("swaps_skipped_h" + h, 0)
        if skipped >= SKIP_FAIL_MIN and not swaps:
            out.append(("FAIL", "head %s: %d swap(s) skipped (empty back) and none shown" % (h, skipped)))
        elif skipped and skipped >= 0.1 * (swaps + skipped):
            out.append(("WARN", "head %s: %d of %d swaps skipped (empty back, %d with pixels in the front)" % (
                h, skipped, swaps + skipped, c.get("swaps_frontskip_h" + h, 0))))
    per_head = sum(c.get("swaps_skipped_h" + h, 0) for h in heads)
    if c.get("swaps_skipped_all", 0) > per_head:
        out.append(("WARN", "%d swap(s) skipped with no head named (nothing drawn and no front: %d in all, %d on "
                            "heads)" % (c["swaps_skipped_all"] - per_head, c["swaps_skipped_all"], per_head)))
    if c.get("swaps_nohead", 0):
        out.append(("WARN", "%d swap(s) matched no head (not shown)" % c.get("swaps_nohead", 0)))
    return out


# card run 2026-10-17 (common/rdna3_flip_latch.h): of the shown buffers the
# kernel hashed again at the head's next swap, this share changed while on
# the panel and was then flipped away from (torn: the panel showed the writes)
TORN_SHARE = 0.05
TORN_MIN_CHECKS = 60
# rewritten in place (one back buffer): the panel reads the buffer
# WindowServer draws into; a WARN from this share on
INPLACE_SHARE = 0.25
# buffers freed while a head scanned them: a few come with every
# WindowServer restart and login (its surfaces go before the new swaps)
SHOWN_FREED_WARN = 4


# card run 2026-10-17's per-head counters (common/rdna3_flip_latch.h), the
# names without their _h<N>
LATCH_KEYS = ("swaps_latchpend", "swaps_latchwait", "swaps_latchus", "swaps_latchmax", "swaps_latchimmediate",
              "swaps_latchtimeout", "swaps_latchunknown", "swaps_latchoff", "swaps_latchwaitsoff", "swaps_flipfailed",
              "swaps_tornchk", "swaps_torn", "swaps_inplace", "swaps_tornskip", "cf_onpanel", "swaps_shownfreed",
              "swaps_emptyfilled", "swaps_emptystill")


def has_latch_counters(c):
    return bool(c) and any(k.startswith("swaps_latch") or k.startswith("swaps_torn") for k in c)


def latch_verdict(c, current=False, report_missing=True):
    """[(level, text)] from the card run 2026-10-17 counters. Per head: the
    swaps whose entry found the previous flip not latched (the panel still
    read the buffer WindowServer had back), how long the waited flips took
    to latch, flips latched at once, timeouts, waits off at the end, unreadable
    states, flips not waited for, refused flips; copy forwards into the
    buffer on the panel; shown frames that changed on the panel (torn:
    FAIL from TORN_SHARE of TORN_MIN_CHECKS+ checks, else WARN; a head with
    60+ swaps and no check is WARN 'not measured'); rewritten in place (one
    back); freed while scanned; sampled empty backs that filled 4 ms after a
    skipped swap (WARN: frames landing after the swap). A key the dump lacks
    is unknown ('?'), never 0. Without any of these counters: [] - or, when
    'current' (the kernel log's build id is these sources) and heads
    swapped, WARN 'counters missing'."""
    heads = sorted(set(m.group(1) for m in (re.match(r"swaps_h(\d)$", k) for k in c) if m))
    if not has_latch_counters(c):
        swapped = [h for h in heads if c.get("swaps_h" + h)]
        if current and swapped:
            return [("WARN", "counters missing: the accelerator runs these sources and head(s) %s swapped, but the "
                             "dump has no swaps_latch* / swaps_torn* counters (the collector's sysctl grep?)" %
                     ", ".join(swapped))]
        return []
    out = []
    latch_on, torn_on = c.get("swap_latch"), c.get("swap_torn")
    same_on = c.get("swap_samenoflip")
    if latch_on == 0:
        out.append(("WARN", "debug.rdna3accel_swap_latch was 0: swaps returned before their flip latched (the A/B run "
                            "without the fix)"))
    for h in heads:
        def g(k):
            return c.get(k + "_h" + h)

        def v(k):
            x = g(k)
            return 0 if x is None else x

        def f(k):
            x = g(k)
            return "?" if x is None else str(x)
        swaps = v("swaps")
        if not swaps and not any(v(k) for k in LATCH_KEYS):
            continue
        missing = [k for k in LATCH_KEYS if g(k) is None]
        if missing and report_missing:
            out.append(("WARN", "head %s: counters missing (unknown, not 0): %s" % (h, ", ".join(missing))))
        pend, waits, us = v("swaps_latchpend"), v("swaps_latchwait"), v("swaps_latchus")
        # card run 2026-10-18b: mode 0 swaps to the buffer already on the panel, not flipped again (no latch wait)
        same_txt = "" if g("swaps_latchsame") is None and same_on is None else (
            "; same buffer, not flipped: %s swap(s) (swap_samenoflip %s)" % (f("swaps_latchsame"),
                                                                          "?" if same_on is None else same_on))
        imm, tout, unk, off = v("swaps_latchimmediate"), v("swaps_latchtimeout"), v("swaps_latchunknown"), v("swaps_latchoff")
        chk, torn, inpl, tskip = v("swaps_tornchk"), v("swaps_torn"), v("swaps_inplace"), v("swaps_tornskip")
        why = []
        if pend:
            why.append("the panel still read the buffer WindowServer had back")
        if tout:
            why.append("%d flip(s) never latched in time" % tout)
        if unk:
            why.append("%d flip state(s) unreadable" % unk)
        if off:
            why.append("%d flip(s) not waited for (waits off after timeouts, or given up)" % off)
        if latch_on != 0 and swaps >= 60 and not waits and g("swaps_latchimmediate") is not None and \
                imm >= 0.9 * swaps:
            why.append("the flip state never read pending - the wait did nothing")
        out.append(("WARN" if why else "INFO",
                    "head %s: %s of %s swaps: the previous flip had not latched at the swap; flips waited for %s "
                    "time(s), avg %s us, max %s us; %s latched at once; %s timeouts, %s unknown, %s not waited "
                    "(swap_latch %s)%s" %
                    (h, f("swaps_latchpend"), f("swaps"), f("swaps_latchwait"),
                     us // waits if waits and g("swaps_latchus") is not None else ("?" if g("swaps_latchus") is None
                                                                                   else 0),
                     f("swaps_latchmax"), f("swaps_latchimmediate"), f("swaps_latchtimeout"),
                     f("swaps_latchunknown"), f("swaps_latchoff"), "?" if latch_on is None else latch_on,
                     " - " + ", ".join(why) if why else "") + same_txt))
        if g("swaps_latchsame") is None and same_on is not None:
            out.append(("WARN", "head %s: counter missing (unknown, not 0): swaps_latchsame (the kernel has "
                                "swap_samenoflip)" % h))
        if v("swaps_latchwaitsoff"):
            out.append(("WARN", "head %s: its swaps were NOT waiting for their flips at the end (timeouts in a row; "
                                "until a flip is seen latched)" % h))
        if v("swaps_flipfailed"):
            out.append(("WARN", "head %s: %d flip(s) refused by the framebuffer (copied onto its scanout or not shown)" %
                        (h, v("swaps_flipfailed"))))
        if v("cf_onpanel"):
            out.append(("WARN", "head %s: %d copy forward(s) wrote into the buffer on the panel (the flip away from it "
                                "had not latched)" % (h, v("cf_onpanel"))))
        if g("swaps_tornchk") is None:
            pass
        elif not chk and swaps > TORN_MIN_CHECKS:
            out.append(("INFO" if torn_on == 0 else "WARN", "head %s: torn test not measured: 0 checks in %d swaps (swap_torn %s, %s skipped)" %
                        (h, swaps, "?" if torn_on is None else torn_on, f("swaps_tornskip"))))
        elif chk or torn or inpl or tskip:
            what = "head %s: %d of %d shown frames changed while on the panel, then flipped away (torn)" % (h, torn, chk)
            if chk >= TORN_MIN_CHECKS and torn >= TORN_SHARE * chk:
                out.append(("FAIL", "%s: %.0f%% - something writes the buffer the panel shows" %
                            (what, 100.0 * torn / chk)))
            elif torn:
                out.append(("WARN", what))
            else:
                out.append(("PASS" if chk >= TORN_MIN_CHECKS else "INFO", what + ("" if chk >= TORN_MIN_CHECKS else
                                                                                 " (fewer than %d checks)" %
                                                                                 TORN_MIN_CHECKS)))
            if inpl:
                share = float(inpl) / chk if chk else 1.0
                out.append(("WARN" if share >= INPLACE_SHARE else "INFO",
                            "head %s: %d of %d shown frames rewritten in place and shown again (one back buffer: the "
                            "panel reads the buffer WindowServer draws into)" % (h, inpl, chk)))
            if tskip:
                out.append(("INFO", "head %s: %d shown buffer(s) the torn test skipped (not hashable at the swap, "
                                    "re-allocated, another surface's)" % (h, tskip)))
        freed = v("swaps_shownfreed")
        if freed:
            out.append(("WARN" if freed > SHOWN_FREED_WARN else "INFO",
                        "head %s: %d buffer(s) freed while the head scanned them" % (h, freed)))
        filled, still = v("swaps_emptyfilled"), v("swaps_emptystill")
        if filled or still:
            out.append(("WARN" if filled else "INFO",
                        "head %s: sampled empty backs of skipped swaps, read again 4 ms later: %d had pixels%s, %d "
                        "still empty" % (h, filled, " (frames landing after the swap: skipped, not shown)" if filled
                                         else "", still)))
    return out


# card run 2026-10-17b's swap as a copy (common/rdna3_swap_copy.h): of a
# head's copies in modes 1/2, this share of black frames (empty backs that
# reached the panel) is a FAIL
BLACK_SHARE = 0.05


def swapcopy_verdict(c):
    """[(level, text)] from the mode 1/2 counters (swaps_copy*_h<N>,
    cf_sc*_h<N>, swap_mode); [] on a kernel without them"""
    if not c or not any(k.startswith(("swaps_copy", "swaps_stalehold")) for k in c):
        return []
    out = []
    # card run 2026-10-17c's counters: absent on an older kext - said so, never printed as zeros
    if "swap_landed" not in c:
        out.append(("WARN", "counter missing: swap_landed (kext older than 2026-10-17c): the landed buffer is not "
                            "counted"))
    mode = c.get("swap_mode")
    out.append(("INFO", "swap mode %s at the dump (0 flip, the default 2026-10-17d..g; 1 copy keeping the picture over "
                        "an empty back, the default again since the write generations; 2 copy all)%s" %
                ("?" if mode is None else mode,
                 ": mode 0 (flip): the copy counters below and H-STALE are not judged" if mode == 0 else "")))
    if c.get("swaps_copybrktrips"):
        out.append(("WARN", "the SDMA breaker of modes 1/2 opened %d time(s): the swaps flipped as in mode 0 while it "
                            "was open (the kernel log's 'SDMA BREAKER OPEN' line says why)" % c["swaps_copybrktrips"]))
    heads = sorted(set(m.group(2) for m in (re.match(r"swaps_(copy|stalehold)_h(\d)$", k) for k in c) if m))
    for h in heads:
        def g(k):
            return c.get(k + "_h" + h, 0)

        def v(k):   # a counter as text: "?" when this kext has none
            return str(c[k + "_h" + h]) if k + "_h" + h in c else "?"
        if "swaps_copy_h" + h not in c:
            out.append(("WARN", "head %s: counter missing: swaps_copy_h%s although swaps_stalehold_h%s is %s - a "
                                "partial dump (the collector's grep), the copies not counted" %
                        (h, h, h, v("swaps_stalehold"))))
            continue
        copies = g("swaps_copy")
        if not copies and not g("swaps_copyempty") and not g("swaps_copyfail") and not g("swaps_stalehold"):
            continue
        missing = [k for k in ("swaps_stalehold", "swaps_copylanded") if k + "_h" + h not in c]
        if missing:
            out.append(("WARN", "head %s: counter missing: %s (kext older than 2026-10-17c): %s not counted" % (
                h, ", ".join(k + "_h" + h for k in missing),
                " and ".join({"swaps_stalehold": "the stale back", "swaps_copylanded": "the landed buffer"}[k]
                             for k in missing))))
        black, held, fail, direct = g("swaps_copyblack"), g("swaps_copyempty"), g("swaps_copyfail"), g("swaps_copydirect")
        landed = g("swaps_copylanded")   # card run 2026-10-17c: the landed buffer presented instead of the back
        would = g("swaps_copylanded_would")   # ... found but not presented (debug.rdna3accel_swap_landed=0)
        stale = g("swaps_stalehold")   # ... backs WindowServer did not redraw since presented, held (H-STALE)
        waits = g("swaps_copywait")
        nowait, brk, freewait = g("swaps_copynowait"), g("swaps_copybrkflip"), g("swaps_copyfreewait")
        line = ("head %s: %d backs copied (%d MiB, avg %d us, max %d us), %s of them the landed buffer instead of an "
                "empty / stale back, %d empty backs kept off a picture, %s stale backs held (not redrawn since "
                "shown), %d black "
                "frames, %d failed, %d direct copies (no second buffer: may tear), %d waits for the last flip (avg %d "
                "us, %d ran out), front copies %d ok / %d not; copy forward from the front %d, from the panel's buffer "
                "%d, nothing to copy from %d" % (h, copies, g("swaps_copybytes") >> 20,
                                                g("swaps_copyus") // copies if copies else 0, g("swaps_copyusmax"),
                                                v("swaps_copylanded"), held, v("swaps_stalehold"), black, fail,
                                                direct, waits,
                                                g("swaps_copywaitus") // waits if waits else 0, g("swaps_copylatchto"),
                                                g("swaps_copyfront"), g("swaps_copyfrontfail"), g("cf_scsrc"),
                                                g("cf_scscanout"), g("cf_scstale")))
        bad = []
        if copies >= 20 and black >= BLACK_SHARE * copies:
            bad.append("FAIL")
        lv = "FAIL" if bad else ("WARN" if fail or direct or g("swaps_copylatchto") or g("swaps_copyfrontfail") or
                                 g("cf_scstale") else "INFO")
        extra = []
        if nowait:
            extra.append("%d copies NOT waited for (waits off after 3 latch timeouts: may tear)" % nowait)
        if brk:
            extra.append("%d swaps FLIPPED with the SDMA breaker open" % brk)
        if freewait:
            extra.append("%d swaps kept an old second buffer (still on the panel / not latched away)" % freewait)
        if extra:
            line += "; " + "; ".join(extra)
            if lv == "INFO" and (nowait or brk):
                lv = "WARN"
        out.append((lv, line))
        if would:
            out.append(("WARN", "head %s: the landed buffer WOULD have been presented %d times (swap_landed %s: the "
                                "empty / stale back's old rule applied - held or copied); rdna3swaplanded=1 presents "
                                "it (watch for half-drawn frames)" % (h, would, c.get("swap_landed", "?"))))
        if landed:
            out.append(("INFO", "head %s: H-B2 handled: landed buffer presented %d times (the named back empty or a "
                                "stale one without an IOSurface, WindowServer's frame in another buffer of the "
                                "surface); %d empty backs still held (no other buffer had a new frame)" %
                        (h, landed, held)))
        elif held and copies and held >= copies:
            out.append(("WARN", "head %s: as many empty backs as frames (%d / %d): WindowServer presents a back it "
                                "never draws (H-B2) - the trace's H-B2 line says whether a frame was held back" %
                        (h, held, copies)))
        if stale:   # card run 2026-10-17c: a back WindowServer did not redraw since presented, held
            out.append(("INFO", "head %s: H-STALE handled: %d stale backs held (WindowServer did not redraw them since "
                                "this head presented them, a newer frame after them: the panel kept it; the trace's "
                                "H-STALE line says whether any was still copied)" % (h, stale)))
        kern = rdna3_trace.stale_counters(c, int(h)) if rdna3_trace is not None else None
        if kern:
            # why backs were not held, and the checks of the rule's assumptions (needs the card)
            sw = c.get("swap_stale")
            lvs = "WARN" if g("swaps_stalehold_changed") or g("swaps_stalehold_would") else "INFO"
            out.append((lvs, "head %s: stale back (swap_stale %s): %s%s%s" % (
                h, "?" if sw is None else sw, kern,
                "; %d held backs CHANGED in the 4 ms re-sample (a frame landed after the hold)" %
                g("swaps_stalehold_changed") if g("swaps_stalehold_changed") else "",
                "; %d would have been held (debug.rdna3accel_swap_stale=0)" % g("swaps_stalehold_would")
                if g("swaps_stalehold_would") else "")))
    return out


GEN_LOG_RE = re.compile(r"write generations:|hook:|flip-present hooks|flipbuf|held:|did not judge")
GEN_LOG_MAX = 12


def gen_log_lines(log):
    """the first GEN_LOG_MAX kernel-log lines of each kind: the write generations ('write generations:', 'held:',
    'did not judge'), the flip-present hooks ('flip-present hooks installed', 'hook:', 'flipbuf') - for --all"""
    out, per = [], {}
    for l in (log or "").splitlines():
        m = GEN_LOG_RE.search(l)
        if not m:
            continue
        k = m.group(0)
        if per.get(k, 0) >= GEN_LOG_MAX:
            continue
        per[k] = per.get(k, 0) + 1
        out.append(l.strip())
    return out or ["(none: a kext without the write generations, or a cut log)"]


def accel_current(log):
    """the kernel log's accelerator build line names these sources (ACCEL_HASH_FILES): the kext has every
    counter these sources define (ECC review round 1: gen_verdict's missing-counter WARNs)"""
    want = fb_source_hash(ACCEL_HASH_FILES)
    ac = re.search(r"RDNA3Accelerator: build (\S+)", log or "")
    return bool(want and ac and ac.group(1) == want)


GEN_COUNTERS = ("swaps_genpresent", "swaps_genhold", "swaps_genhold_would", "swaps_genfirst", "swaps_gencontent",
                "swaps_genunwritten", "cf_gennewer", "flipbuf", "flipbuf_scanout", "flipbuf_presented", "flipbuf_held",
                "flipbuf_failed")
# ECC review round 1 of the write generations: a kext of these sources has them all
GEN_COUNTERS_R1 = ("swaps_gen_judged", "swaps_gen_aged", "swaps_gen_evictpresent", "swaps_gen_landed",
                   "swaps_gennotjudged", "swaps_gen_resets", "flipbuf_busy", "flipbuf_unlocked", "flipbuf_emptyheld")
GEN_GLOBALS = ("swaps_gen_aperture", "swaps_gen_apcached", "swaps_gen_cf", "swaps_gen_cfnostamp", "swaps_gen_evicted",
               "swaps_gen_freed", "swaps_gen_resets", "flipbuf_nohead", "flipbuf_nores", "flipbuf_intoff",
               "flipbuf_badptr", "hook_intr", "hook_intoff")
HOOK_INTR_KINDS = ("flipbuf", "scanout", "pickpresent", "didsubmitswap", "backready", "submitupdate")
# counters read one by one while swaps go on: a sum may be off by a swap or two
GEN_SLACK = 2


def gen_verdict(c, current=False):
    """[(level, text)] from the write generations' and the flip-present hooks' counters (card run 2026-10-17d:
    swap_gen, flipbuf_present, swaps_gen*_h<N>, cf_gennewer_h<N>, flipbuf*_h<N>, hook_*); a kext of these sources
    ('current': the kernel log's accelerator build line is these sources) without them is a WARN, an older kext
    says nothing. ECC review round 1: the rule judging nothing is a FAIL, the judged swaps reconcile with their
    outcomes, the flip-buffer calls with theirs, the hooks' interrupt-level calls and the hooks never firing on
    a current kext are WARNs"""
    if not c:
        return []
    if "swap_gen" not in c:
        if current:
            return [("WARN", "counter missing: swap_gen and the write generations' counters (swaps_genhold_h<N>, "
                             "flipbuf_h<N>) although the kext is of these sources: the collector's grep missed them")]
        return []
    out = [("INFO", "write generations: swap_gen %s, flipbuf_present %s, swap mode %s; generations handed out by "
                    "page-on %s (+%s by its cached descriptor), by copy forward %s (%s targets kept theirs: the copy "
                    "failed); evicted %s, freed %s, head resets %s" % (
                        c.get("swap_gen"), c.get("flipbuf_present", "?"), c.get("swap_mode", "?"),
                        c.get("swaps_gen_aperture", "?"), c.get("swaps_gen_apcached", "?"), c.get("swaps_gen_cf", "?"),
                        c.get("swaps_gen_cfnostamp", "?"), c.get("swaps_gen_evicted", "?"),
                        c.get("swaps_gen_freed", "?"), c.get("swaps_gen_resets", "?")))]
    if current:
        gmiss = [k for k in GEN_GLOBALS if k not in c]
        if gmiss:
            out.append(("WARN", "counter missing: %s although the kext is of these sources (a partial dump: the "
                                "collector's grep)" % ", ".join(gmiss)))
    if c.get("swaps_gen_evicted"):
        out.append(("WARN", "write generations: %d evicted from the full 32-slot table - an evicted back reads 'never "
                            "written'; while the evicted generation is newer than a head's frame such a back presents "
                            "(the eviction guard: %d time(s))" % (
                                c["swaps_gen_evicted"],
                                sum(v for k, v in c.items() if re.match(r"swaps_gen_evictpresent_h\d$", k)))))
    swap_mode = c.get("swap_mode")
    heads = sorted(set(m.group(1) for m in (re.match(r"(?:swaps_gen\w*|flipbuf\w*|cf_gennewer)_h(\d)$", k) for k in c)
                       if m) | set(k[-1] for k, v in c.items() if re.match(r"swaps_h\d$", k) and v))
    for h in heads:
        def g(k):
            return c.get(k + "_h" + h, 0)
        sw = c.get("swaps_h" + h, 0)
        if not any(k + "_h" + h in c for k in GEN_COUNTERS + GEN_COUNTERS_R1):
            if sw:
                out.append(("WARN", "head %s: %d swaps but NONE of the write generations' counters (swaps_gen*_h%s): "
                                    "a partial dump" % (h, sw, h)))
            continue
        missing = [k for k in GEN_COUNTERS if k + "_h" + h not in c]
        if current:
            missing += [k for k in GEN_COUNTERS_R1 if k + "_h" + h not in c]
        if missing:
            out.append(("WARN", "head %s: counter missing: %s - a partial dump (the collector's grep)" % (
                h, ", ".join(k + "_h" + h for k in missing))))
        judged_known = "swaps_gen_judged_h" + h in c
        judged = g("swaps_gen_judged")
        outcomes = g("swaps_genpresent") + g("swaps_genhold") + g("swaps_genhold_would") + g("swaps_gen_landed")
        # the rule judging nothing while it is on and the head swaps (ECC review round 1, H2)
        if c.get("swap_gen") == 1 and swap_mode in (0, 1) and sw > 20 and \
                (judged == 0 if judged_known else outcomes == 0):
            out.append(("FAIL", "head %s: the write generations judged NONE of %d swaps (swap_gen 1, swap mode %d) - "
                                "the rule is inert: %d not judged (swaps_gennotjudged_h%s; the kernel log's 'did not "
                                "judge' lines say why)" % (h, sw, swap_mode, g("swaps_gennotjudged"), h)))
        if not any(g(k) for k in GEN_COUNTERS + GEN_COUNTERS_R1):
            continue
        out.append(("INFO", "head %s: write generations: %s judged: %d presented (%d the first after a reset, %d "
                            "changed with no write seen, %d by the age guard, %d by the eviction guard), %d held (%d "
                            "never written), %d the surface's newer buffer instead, %d would have been held (swap_gen "
                            "0 / mode 2); %d not judged; %d resets; copy forward took a newer front %d times; "
                            "flip-buffer calls %d + %d scanout, %d presented, %d held (%d empty), %d failed, %d busy, "
                            "%d took the lock" % (
                                h, judged if judged_known else "?", g("swaps_genpresent"), g("swaps_genfirst"),
                                g("swaps_gencontent"), g("swaps_gen_aged"), g("swaps_gen_evictpresent"),
                                g("swaps_genhold"), g("swaps_genunwritten"), g("swaps_gen_landed"),
                                g("swaps_genhold_would"), g("swaps_gennotjudged"), g("swaps_gen_resets"),
                                g("cf_gennewer"), g("flipbuf"), g("flipbuf_scanout"), g("flipbuf_presented"),
                                g("flipbuf_held"), g("flipbuf_emptyheld"), g("flipbuf_failed"), g("flipbuf_busy"),
                                g("flipbuf_unlocked"))))
        if judged_known and abs(judged - outcomes) > GEN_SLACK:
            out.append(("WARN", "head %s: the write generations' outcomes do not add up: %d judged, but presented %d + "
                                "held %d + would-hold %d + newer buffer %d = %d - a decision without its counter" % (
                                    h, judged, g("swaps_genpresent"), g("swaps_genhold"), g("swaps_genhold_would"),
                                    g("swaps_gen_landed"), outcomes)))
        if g("swaps_gennotjudged"):
            out.append(("WARN", "head %s: %d swap(s) the write generations did NOT judge (no head known at the swap's "
                                "entry, or no VRAM address): presented as before, unguarded (the kernel log's 'did not "
                                "judge' lines)" % (h, g("swaps_gennotjudged"))))
        if g("swaps_gencontent"):
            out.append(("WARN", "head %s: %d back(s) changed with NO write the accelerator saw (presented by the content "
                                "guard): a writer that is neither the family's page-on nor copy forward" %
                        (h, g("swaps_gencontent"))))
        if g("swaps_gen_aged") or g("swaps_gen_evictpresent"):
            out.append(("WARN", "head %s: the hold guards presented %d back(s) the rule would have held (age guard %d: "
                                "no present for 250 ms and 3 holds in a row; eviction guard %d) - the panel was not "
                                "frozen, but those frames may be old (the trace's H-GEN line)" % (
                                    h, g("swaps_gen_aged") + g("swaps_gen_evictpresent"), g("swaps_gen_aged"),
                                    g("swaps_gen_evictpresent"))))
        if g("swaps_genpresent") >= 10 and g("swaps_genfirst") > 0.3 * g("swaps_genpresent"):
            out.append(("WARN", "head %s: %d of %d generation presents were the first after a reset (> 30%%): the head's "
                                "record keeps restarting (%d resets on the head) - the rule barely judges" % (
                                    h, g("swaps_genfirst"), g("swaps_genpresent"), g("swaps_gen_resets"))))
        if g("swaps_genhold") >= 20 and g("swaps_genhold") > 2 * max(g("swaps_genpresent"), 1):
            out.append(("WARN", "head %s: %d generation holds against %d presents: the panel may look frozen - compare the "
                                "looks" % (h, g("swaps_genhold"), g("swaps_genpresent"))))
        if g("flipbuf_failed"):
            out.append(("WARN", "head %s: %d flip-buffer presents FAILED (the kernel log's 'flipbuf #' lines say why)" %
                        (h, g("flipbuf_failed"))))
        if g("flipbuf_busy"):
            out.append(("WARN", "head %s: flip-buffer presents not done %d time(s): the family's lock held by another "
                                "thread (flipbuf_busy_h%s)" % (h, g("flipbuf_busy"), h)))
    # the flip-buffer calls against their outcomes (debug.rdna3accel_flipbuf_present=1 for the whole run)
    def total(k):
        return sum(v for kk, v in c.items() if re.match(k + r"_h\d$", kk))
    calls = total("flipbuf") + total("flipbuf_scanout")
    if c.get("flipbuf_present") == 1 and calls:
        done = total("flipbuf_presented") + total("flipbuf_held") + total("flipbuf_failed") + total("flipbuf_busy") + \
            c.get("flipbuf_nores", 0)
        if abs(calls - done) > GEN_SLACK:
            out.append(("WARN", "flip-buffer calls do not add up: %d calls on our heads, but presented %d + held %d + "
                                "failed %d + busy %d + no resource %d = %d (unless flipbuf_present was switched during "
                                "the run)" % (calls, total("flipbuf_presented"), total("flipbuf_held"),
                                              total("flipbuf_failed"), total("flipbuf_busy"), c.get("flipbuf_nores", 0),
                                              done)))
    if c.get("flipbuf_nores"):
        out.append(("WARN", "flip-buffer presents not done %d time(s): neither buffer a VRAM resource of ours "
                            "(flipbuf_nores)" % c["flipbuf_nores"]))
    if c.get("flipbuf_badptr"):
        out.append(("WARN", "flip-buffer calls with a resource argument that is no kernel address: %d (flipbuf_badptr) "
                            "- the reconstructed signature may be wrong" % c["flipbuf_badptr"]))
    if c.get("hook_intr"):
        kinds = ", ".join("%s %d" % (k, c["hook_intr_" + k]) for k in HOOK_INTR_KINDS if c.get("hook_intr_" + k))
        out.append(("WARN", "flip-present hooks called at INTERRUPT LEVEL %d time(s)%s: only counted and forwarded, "
                            "those frames were neither traced nor presented" % (
                                c["hook_intr"], " (%s)" % kinds if kinds else "")))
    if c.get("flipbuf_intoff") or c.get("hook_intoff"):
        out.append(("WARN", "flip-present hooks called with INTERRUPTS DISABLED: %d flip-buffer, %d surface call(s): "
                            "only counted and forwarded" % (c.get("flipbuf_intoff", 0), c.get("hook_intoff", 0))))
    hooks = [(k, c[k]) for k in ("hook_pickpresent", "hook_didsubmitswap", "hook_backready", "hook_backready_no",
                                 "hook_submitupdate", "hook_intr", "flipbuf_nohead") if k in c]
    if hooks:
        out.append(("INFO", "flip-present hooks: " + ", ".join("%s %d" % kv for kv in hooks)))
    elif current:
        out.append(("WARN", "counter missing: hook_* (the surface hooks' counters) although swap_gen is there"))
    swaps = sum(v for k, v in c.items() if re.match(r"swaps_h\d$", k))
    fired = sum(v for k, v in hooks) + calls
    if current and hooks and swaps > 50 and not fired:
        out.append(("WARN", "hook wiring unproven: %d swaps and NOT ONE call of any flip-present hook (hook_*, "
                            "flipbuf*): the overrides may not be in the family's vtables - the kernel log's 'flip-present "
                            "hooks installed' line gives their offsets" % swaps))
    return out


def trace_inputs(path):
    """the per-frame trace's pieces of a report: the archive's 36-*.bin (the
    full reads and the stream collect.sh packed, 36-stream-*.bin) and the
    stream files autoreport.sh sent next to it -> [(name, bytes)], plus
    their .size sidecars -> {name: size}"""
    out, sizes = [], {}
    if os.path.exists(path):
        try:
            with tarfile.open(path) as tar:
                for m in tar.getmembers():
                    if not m.isfile():
                        continue
                    if re.search(r"36-(frame-trace|frame-trace-sec|drm-trace|stream-[\w.-]+)\.bin$", m.name):
                        out.append((m.name, tar.extractfile(m).read()))
                    elif re.search(r"36-stream-[\w.-]+\.bin\.size$", m.name):
                        sizes[m.name[:-len(".size")]] = tar.extractfile(m).read().decode("ascii", "replace").strip()
        except (OSError, tarfile.TarError):
            pass
    base = path[:-len(".tar.gz")]
    for f in sorted(glob.glob(base + "-*trace*.bin")):
        try:
            with open(f, "rb") as fh:
                out.append((f, fh.read()))
        except OSError:
            pass
        try:
            with open(f + ".size") as fh:
                sizes[f] = fh.read().strip()
        except OSError:
            pass
    return out, sizes


def trace_size_problems(inputs, sizes):
    """[text]: stream files whose bytes differ from the size autoreport.sh
    wrote next to them (a cut-off upload or copy), and uploaded streams
    shorter than the archive's copy of the same file"""
    out = []
    got = dict((n, len(d)) for n, d in inputs)
    for n, want in sorted(sizes.items()):
        if n in got and want.isdigit() and int(want) != got[n]:
            out.append("%s: %d bytes, %s written (a cut-off upload or copy)" % (os.path.basename(n), got[n], want))
    for n, ln in got.items():
        m = re.search(r"36-stream-([\w.-]+\.bin)$", n)
        if not m:
            continue
        for n2, ln2 in got.items():
            if n2.endswith("-" + m.group(1)) and "36-stream-" not in n2 and ln2 < ln:
                out.append("%s: %d bytes uploaded, the archive's copy has %d" % (os.path.basename(n2), ln2, ln))
    return out


def ab_counter_window(txt):
    """{head: swaps the counters saw over the A/B} from the tests file: the
    last swaps_h<N> dump before '== swap A/B phase 1' against the one after
    mode 0; {} when either is missing"""
    i = txt.find("== swap A/B phase 1")
    j = txt.find("== swap A/B counters after mode 0")
    if i < 0 or j < 0:
        return {}
    before = dict((int(h), int(v)) for h, v in re.findall(r"rdna3accel_swaps_h(\d): (\d+)", txt[:i]))
    seg = txt[j:]
    k = seg.find("\n== ", 5)
    after = dict((int(h), int(v)) for h, v in re.findall(r"rdna3accel_swaps_h(\d): (\d+)",
                                                          seg if k < 0 else seg[:k]))
    return dict((h, after[h] - before.get(h, 0)) for h in after if after[h] - before.get(h, 0) > 0)


def trace_lines(path, txt, klog="", args=""):
    """[(level, text)], timeline lines: the per-frame trace of a report
    (card run 2026-10-17b), else the 'tr' lines of the kernel log; with
    neither on a kernel that has the trace: FAIL 'trace incomplete'. Never
    raises: a decoder failure on a broken file is a FAIL naming it."""
    try:
        return _trace_lines(path, txt, klog, args)
    except Exception as e:   # noqa: BLE001 - the report is untrusted input
        return [("FAIL", "trace incomplete: Q1-Q11 (the decoder failed on this report: %s: %s)" %
                 (type(e).__name__, str(e)[:200]))], []


def _trace_lines(path, txt, klog="", args=""):
    if rdna3_trace is None:
        return [("WARN", "trace not decoded: tools/rdna3_trace.py is missing next to this script")], []
    ftxt = ""
    if os.path.exists(path):
        try:
            with tarfile.open(path) as tar:
                args = args or read_member(tar, "01-boot-args.txt") or ""
                ftxt = read_member(tar, "36-frame-trace.txt") or ""
        except (OSError, tarfile.TarError):
            pass
    tr = rdna3_trace.Trace()
    inputs, sizes = trace_inputs(path)
    size_problems = trace_size_problems(inputs, sizes)
    # an uploaded stream the archive also holds (36-stream-<f>): read once,
    # the longer copy (the streams' checks count every header)
    packed = dict((m.group(1), len(d)) for n, d in inputs for m in [re.search(r"36-stream-([\w.-]+\.bin)$", n)] if m)
    inputs = [(n, d) for n, d in inputs
              if "36-stream-" in n or not any(n.endswith("-" + f) and len(d) <= ln for f, ln in packed.items())]
    inputs = [(n, d) for n, d in inputs
              if not ("36-stream-" in n and any(n2.endswith("-" + n.split("36-stream-")[1]) and "36-stream-" not in n2
                                               for n2, _ in inputs))]
    for name, data in inputs:
        tr.add_blob(data, os.path.basename(name))
    if klog:
        tr.check_dumps(klog)
    if not tr.headers and klog:
        tr.add_lines(klog)
    tr.finish()
    _TRACE_CACHE[path] = tr
    c = a3_counters(txt) if txt else {}
    c = c or {}
    want = fb_source_hash(ACCEL_HASH_FILES)
    ac = re.search(r"RDNA3Accelerator: build (\S+)", klog)
    # a kernel with the trace: the build line names these sources, or (no
    # build line in the log: a cut log, a serial-only run) the report shows
    # the trace's own sysctls (ECC review round 2: a missing build line must
    # not silence every 'trace incomplete')
    current = bool(want and ac and ac.group(1) == want) or \
        (not ac and bool(re.search(r"rdna3accel_swap_mode|rdna3accel_trace", txt + ftxt)))
    accel_on = "rdna3accel=1" in args or "rdna3accel=1" in txt or ("rdna3lvl=" not in args and current)
    out = []
    # collect.sh's own record of what it could read (root only sysctls)
    for oid in re.findall(r"^(\S+): NOT read", ftxt, re.M):
        if current and accel_on:
            out.append(("FAIL", "trace incomplete: Q1-Q11 (%s NOT read by collect.sh: not root, or the sysctl is "
                                "missing)" % oid))
    if current and accel_on and os.path.exists(path) and not ftxt:
        out.append(("FAIL", "trace incomplete: Q1-Q11 (no 36-frame-trace.txt in the archive: a collect.sh before "
                            "card run 2026-10-17b)"))
    if not tr.events and not tr.headers:
        if any(re.match(r"swaps_stalehold_h\d$", k) for k in c):
            out.append(("WARN", "H-STALE not judged: no trace in the report (swaps_stalehold_h<N> counters only): "
                                "whether a stale back was still copied needs the trace stream"))
        if current and accel_on:
            return out + [("FAIL", "trace incomplete: Q1-Q11 (no 36-frame-trace.bin, no -trace-stream.bin and no "
                                   "'tr' lines although the accelerator runs these sources)%s" %
                                   ("; " + "; ".join(t for _, _, t in tr.problems[:3]) if tr.problems else ""))], []
        return out, []
    streams = any("stream" in n for n, _ in inputs)
    # a display bisect run (rdna3bisect=1 / 2 / 4) replaces the other tests, the swap A/B among them
    is_bisect = bisect_run_expected(path, txt, args)
    # card run 2026-10-17d: the A/B is opt-in (boot-arg rdna3swapab=1, or its phases are in the tests file)
    ab_asked = "rdna3swapab=1" in args.split() or "== swap A/B phase" in txt
    ab_expected = bool(current and accel_on and not is_bisect and ab_asked and "swap A/B: SKIPPED" not in txt and
                       (bootarg_level(args) >= 2 or "rdna3lvl=" not in args) and (txt or streams))
    ctx = {"swaps": sum(v for k, v in c.items() if re.match(r"swaps_h\d$", k)) or None,
           "swaps_window": ab_counter_window(txt) if txt else {},
           "no_tests": not txt,
           "cf_calls": c.get("cf_calls"),
           "ab_ran": "== swap A/B phase" in txt,
           "ab_expected": ab_expected,
           "size_problems": size_problems,
           "counters": c,
           "core_expected": any(n.endswith("drm-trace.bin") or "drmtrace" in n for n, _ in inputs) or
                            "== swap A/B" in txt}
    if not txt:
        out.append(("INFO", "no tests file next to this report: the A/B is judged from the trace alone, the "
                            "counters' window not compared"))
    if current and accel_on and not is_bisect and not ab_asked:
        out.append(("INFO", "A/B not asked: swap mode 1 (the copy) is the default again since the write generations; "
                            "the 60 s mode 1 / mode 0 comparison runs with the boot-arg rdna3swapab=1"))
    out += rdna3_trace.summary_lines(tr, ctx)
    tl = []
    heads = sorted(set(e.head for e in tr.events if e.src == rdna3_trace.SRC_ACCEL and e.head != 0xff))
    for h in heads:
        tl.append("head %d, the last 60 events:" % h)
        tl.extend("  " + l for l in rdna3_trace.timeline(tr, h, 60))
    return out, tl


_TRACE_CACHE = {}   # report path -> the kernel trace _trace_lines decoded (the plugin's join reads it)


def read_tar_member(path, suffix):
    """one member of a report archive, "" when there is none"""
    if not os.path.exists(path):
        return ""
    try:
        with tarfile.open(path) as tar:
            return read_member(tar, suffix) or ""
    except (OSError, tarfile.TarError):
        return ""


PLUGIN_TRACE_SRC = "metal/plugin/NVMTLTrace.m"   # the plugin's frame trace (card run 2026-10-17b)


def plugin_stale_reason(path):
    """why the plugin wrote no frame trace when the VM's plugin is provably
    older than the repo's metal/ (the same check as stale_builds: the
    archive's 35-build-stamps.txt against the 00-host.txt next to it), and
    whether the commit it was built from had the plugin's trace at all;
    "" when the plugin is not proven stale (card run 2026-10-17c: built from
    8540be5, the trace came with e168385 - triage printed only the
    collector's generic list of reasons)"""
    st = parse_stamps(read_tar_member(path, "35-build-stamps.txt")).get("plugin")
    if not st:
        return ""
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(path)), "00-host.txt"), errors="replace") as fh:
            host = parse_host(fh.read())
    except OSError:
        host = {}
    verdict, text = stamp_verdict("PLUGIN", st, host)
    if verdict != "stale":
        return ""
    sha = st.get("metal", "")
    added = git_out("log", "--diff-filter=A", "--format=%h", "--", PLUGIN_TRACE_SRC)
    added = added.split()[-1] if added else ""
    if HEX_SHA.match(sha) and git_out("cat-file", "-e", "%s:%s" % (sha, PLUGIN_TRACE_SRC)) is None:
        has = "a plugin without the frame trace (%s came%s after it)" % (
            PLUGIN_TRACE_SRC, " with " + added if added else "")
    else:
        has = "a plugin older than the repo's"
    return ("the VM's plugin is STALE: %s - %s; rebuild it in the VM (tools/vm/macos/build_metal.sh, "
            "docs/TESTING-VM.md) before the next card run" % (text, has))


def plugin_trace_lines(path, cdlog="", txt=""):
    """[(level, text)] of the Metal plugin's trace (tools/plugin_trace.py):
    its verdicts with the time its ring covers against the kernel's A/B, the
    join of each back the kernel's swaps named with what the plugin did to
    that IOSurface, and WARN 'plugin trace MISSING (<why>)' when WindowServer
    loaded the plugin (the tests file's M5 line, the CoreDisplay log) or the
    kernel's backs have IOSurface ids to join, but the report has no
    WindowServer trace (ECC review rounds 2, 3)"""
    ws_loaded = bool(re.search(r"^M5: WindowServer LOADED", txt, re.M) or
                     re.search(r"^processes with the plugin loaded:.*\bWindowSer", txt, re.M) or
                     re.search(r"WindowServer\[\d+:[0-9a-f]+\].*NVMTL", cdlog))
    ktr = _TRACE_CACHE.get(path)
    abw = rdna3_trace.ab_window(ktr) if (ktr is not None and rdna3_trace is not None) else None
    joinable = bool(_join_lines(ktr, None))
    info = read_tar_member(path, "31-plugin-trace/info.txt")
    coll = read_tar_member(path, "37-plugin-trace-collect.txt")
    out = []
    for l in re.findall(r"^REJECT .*$", info, re.M)[:6]:
        out.append(("WARN", "plugin trace collector: " + l[:200]))

    def missing(why_extra):
        stale = plugin_stale_reason(path)
        if stale:   # the reason, not the collector's list of possible ones
            return ("WARN", "plugin trace MISSING%s: %s" % (why_extra, stale))
        why = re.search(r"^== plugin frame traces collected: .*$", info, re.M)
        if why:
            why = why.group(0)[3:]
            hint = re.search(r"^== no traces: (.*)$", info, re.M)
            if hint:
                why += "; " + hint.group(1)
        elif coll:
            ex = re.findall(r"^\[exit (\d+)\]", coll, re.M)
            why = "collect_plugin_trace.sh ran (exit %s) but left no 31-plugin-trace/info.txt" % (ex[-1] if ex else "?")
        elif os.path.exists(path):
            why = "collect_plugin_trace.sh did not run (no 31-plugin-trace/ and no 37-plugin-trace-collect.txt)"
        else:
            why = "no archive (collect.sh did not finish), no snapshot next to it"
        return ("WARN", "plugin trace MISSING (%s)%s" % (why, why_extra))

    def tail(surfaces=None, covers=None, decoded=False):
        res = []
        if not decoded and (ws_loaded or joinable):
            res.append(missing(" although WindowServer loaded the plugin" if ws_loaded else
                               " - the join lines below carry no plugin verdict"))
        return res + _join_lines(ktr, surfaces, covers)

    try:
        import plugin_trace  # noqa: E402
    except ImportError as e:
        out.append(("WARN" if (ws_loaded or joinable) else "INFO",
                    "plugin trace NOT decoded: tools/plugin_trace.py is not next to this script (%s)" % e))
        return out + tail()
    try:
        r = plugin_trace.triage_ex(path, ab_window=abw)
    except Exception as e:   # noqa: BLE001 - the report is untrusted input
        return out + [("FAIL", "plugin trace not decoded: %s: %s" % (type(e).__name__, str(e)[:200]))] + tail()
    for lv, t in r["lines"]:
        if "no plugin traces in the archive" in t and "snapshots" not in t:
            continue   # said below, with the reason
        out.append((str(lv), str(t)))
    ws = bool(r.get("ws"))
    return out + tail(r["surfaces"] if ws else None, r.get("covers_ab"), ws)


def _join_lines(ktr, surfaces, covers=None):
    """the kernel's backs joined with the plugin's surfaces (rdna3_trace.join_lines; the WARNs for backs
    without an id are the frame trace's own lines)"""
    if ktr is None or rdna3_trace is None:
        return []
    return [(lv, t) for lv, t in rdna3_trace.join_lines(ktr, surfaces, covers) if "has NO IOSurface id" not in t]


def coredisplay_cf_lines(cdlog, tr_lines=()):
    """CoreDisplay switched copy forward off ("copy forward failed ...
    disabling"): CRITICAL - with whether the driver's trace saw an error
    answer (else the family failed before calling the driver)"""
    out = []
    for m in re.finditer(r"^(\S+ \S+).*copy_forward_region: copy forward failed \((0x[0-9a-f]+)\)", cdlog, re.M):
        drv = any("CF-ERROR" in t for _, t in tr_lines)
        out.append(("CRITICAL", "CoreDisplay disabled copy forward at %s (%s)%s" % (
            m.group(1), m.group(2), " - the driver answered an error (trace CF-ERROR)" if drv else
            " - the driver's submitCopyForward answers only success (or 0xe00002c7 with rdna3accelcf=0): the family "
            "failed before calling it (a buffer without VRAM: look for VRAM refusals, the trace's VRAM line)")))
    return out


VRAM_HEAP_OWNERS = (("accel", "accel"), ("fb_surface", "fb-surface"), ("fb_cursor", "fb-cursor"),
                    ("swap_d", "swap-D"), ("gem", "gem"), ("gem_hi", "gem-beyond-BAR0"), ("kernel", "kernel"))
# the last key vramReport() prints: a dump without it was cut (the sysctl buffer, a pipe)
VRAM_HEAP_LAST_KEY = "kernel_refused_kib"


def vram_heap_counters(*texts):
    """{key: int} of the newest 'debug.rdna3_vram: key=value ...' line in the
    given texts (the core's per-owner report of the CPU-visible VRAM, card
    run 2026-10-17c), None when none has one. Later texts win ties on the
    refusal count (the archive is read after the run's last dump)."""
    best = None
    for t in texts:
        for l in (t or "").splitlines():
            m = re.match(r"\s*debug\.rdna3_vram: (visible_kib=.*)$", l)
            if m:
                c = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", m.group(1))}
                if best is None or c.get("refused", 0) >= best.get("refused", 0):
                    best = c
    return best


def vram_heap_owners_text(c):
    """'accel 49200 KiB x5 (peak 65000 KiB, refused 3), gem ...' of the owners that held anything"""
    out = []
    for key, name in VRAM_HEAP_OWNERS:
        if c.get(key + "_peak_kib") or c.get(key + "_refused"):
            out.append("%s %d KiB x%d (peak %d KiB%s)" % (
                name, c.get(key + "_kib", 0), c.get(key + "_n", 0), c.get(key + "_peak_kib", 0),
                ", refused %d" % c[key + "_refused"] if c.get(key + "_refused") else ""))
    return ", ".join(out) or "nothing"


def vram_heap_lines(path, txt, log):
    """[(level, text)] of the CPU-visible VRAM (card run 2026-10-17c: RADV's BOs filled
    the 144 MiB visible heap, WindowServer's framebuffers were refused and the main
    display froze). From debug.rdna3_vram (the tests file's dumps, the archive's
    38-vram-heap.txt) and the kernel's 'VRAM REFUSED' lines. FAIL: the accelerator's
    (display) allocations were refused or the heap lost a range; WARN: GEM BOs fell
    back to GTT, a free list ran full, or the data is missing on a kext that has it."""
    arc = ""
    if os.path.exists(path):
        try:
            with tarfile.open(path) as tar:
                arc = read_member(tar, "38-vram-heap.txt") or ""
        except (OSError, tarfile.TarError):
            pass
    c = vram_heap_counters(txt, arc)
    fb_refused = len(re.findall(r"RD3VidMemory::allocPhysical\([^)]*\): \d+ bytes REFUSED by the framebuffer", log))
    nref = max([int(n) for n in re.findall(r"VRAM REFUSED #(\d+):", log)] or [0])
    new_kext = "accelerator's reserve" in log or "VRAM heap:" in log or nref
    out = []
    if c is None:
        if arc.strip():
            # collect.sh ran and left no dump: loud whatever the log says
            why = re.search(r"^\(no debug\.rdna3_vram[^\n]*\)", arc, re.M)
            return [("WARN", "not measured: 38-vram-heap.txt has no debug.rdna3_vram dump%s (%d 'VRAM REFUSED' "
                             "line(s) and %d framebuffer refusal(s) in the log)"
                     % (" - " + why.group(0) if why else "", nref, fb_refused))]
        if new_kext:
            # the kext that has the report left no dump: loud, not silent
            return [("WARN", "not measured: debug.rdna3_vram is in neither the tests file nor 38-vram-heap.txt "
                             "although the kernel log has the heap lines (%d 'VRAM REFUSED' line(s), %d framebuffer "
                             "refusal(s)) - collect.sh / autoreport.sh did not read it" % (nref, fb_refused))]
        if fb_refused:
            return [("WARN", "%d accelerator VRAM allocation(s) refused by the framebuffer, no debug.rdna3_vram to say "
                             "who held the visible heap (a kext before card run 2026-10-17c)" % fb_refused)]
        return []
    refused = c.get("refused", 0)
    ra, rg, rh = c.get("refused_accel", 0), c.get("refused_gem", 0), c.get("refused_hi", 0)
    rk = c.get("kernel_refused", 0)
    owners = vram_heap_owners_text(c)
    if VRAM_HEAP_LAST_KEY not in c:
        out.append(("WARN", "dump truncated: debug.rdna3_vram ends before %s - the counters after the cut are "
                            "missing, read the kernel log's 'VRAM heap:' lines" % VRAM_HEAP_LAST_KEY))
    if c.get("visible_kib", 0) == 0:
        out.append(("WARN", "visible_kib=0: the core reports no CPU-visible heap (BAR0 not mapped, or smaller than "
                            "the kernel's windows): nothing of the accelerator's or the clients' CPU-visible VRAM "
                            "is placed"))
    if nref > refused:
        out.append(("WARN", "log shows %d refusals, newest dump says %d: the dump is older than the log (read "
                            "before the run ended), or the counters were reset" % (nref, refused)))
    if ra or rg:
        lv = "FAIL" if ra else "WARN"
        out.append((lv, "VISIBLE VRAM exhausted: %d refusals (%d of the accelerator's own allocations - the display's "
                        "framebuffers; %d of GEM BOs, which the UAPI then tries to place in GTT: gtt_fail below says "
                        "when that failed too), owners %s; visible heap %d KiB with a %d KiB reserve for the "
                        "accelerator, free now: reserve %d KiB, general %d KiB (largest %d KiB)%s"
                    % (ra + rg, ra, rg, owners, c.get("visible_kib", 0), c.get("reserve_kib", 0),
                       c.get("free_acc_kib", 0), c.get("free_gem_kib", 0), c.get("largest_gem_kib", 0),
                       "; %d GEM request(s) were held off by the reserve" % c["reserve_blocked"]
                       if c.get("reserve_blocked") else "")))
        if ra:
            out.append(("INFO", "the accelerator was refused although its reserve is %d KiB: more than the reserve and "
                                "the general heap together - raise it with rdna3visreserve=<MiB>, or a bigger BAR0 "
                                "(RESIZE_BAR_LOG2 in ~/rdna3-vm/config)" % c.get("reserve_kib", 0)))
    if rk:
        out.append(("FAIL", "%d kernel-internal BO(s) (seq64, user queue objects) refused: no room in the general part "
                            "or the reserve, and they have no GTT fallback" % rk))
    if c.get("gtt_fail"):
        out.append(("WARN", "%d client BO(s), %d KiB, found no VRAM and no GTT either (the allocation returned an "
                            "error; the kernel log's 'no VRAM and no GTT either' line has the GTT usage)"
                    % (c["gtt_fail"], c.get("gtt_fail_kib", 0))))
    if rh or c.get("hi_full"):
        out.append(("WARN", "%d BO(s) without CPU access found no room beyond BAR0 (free %d KiB, largest %d KiB, "
                            "alignment/fragmentation); they never take visible space, so the UAPI tries GTT for them"
                    % (rh, c.get("free_hi_kib", 0), c.get("largest_hi_kib", 0))))
    if c.get("free_fail"):
        out.append(("FAIL", "%d free(s) of %d KiB were refused by the heap (outside it, or a double free): the owner "
                            "still counts the memory, a refcount bug - the kernel log's 'could not give' lines "
                            "name the first ones" % (c["free_fail"], c.get("free_fail_kib", 0))))
    if c.get("lost_kib"):
        out.append(("FAIL", "the heap lost %d KiB: a free list was full when a range came back (nfree acc/gem/hi "
                            "%d/%d/%d) - fragmentation beyond what the allocator can hold"
                    % (c["lost_kib"], c.get("nfree_acc", 0), c.get("nfree_gem", 0), c.get("nfree_hi", 0))))
    if c.get("retired_n"):
        out.append(("WARN", "%d range(s), %d KiB, retired for good: a VRAM clear timed out (the SDMA fill may still "
                            "land, so the range is never reused) - the kernel log's 'range retired' lines"
                    % (c["retired_n"], c.get("retired_kib", 0))))
    skips = c.get("full_skips_acc", 0) + c.get("full_skips_gem", 0) + c.get("full_skips_hi", 0)
    if skips:
        out.append(("WARN", "%d allocation attempt(s) skipped a free range only because the free list was full "
                            "(nfree %d/%d/%d)" % (skips, c.get("nfree_acc", 0), c.get("nfree_gem", 0),
                                                  c.get("nfree_hi", 0))))
    if c.get("hi_clear_fail"):
        out.append(("WARN", "%d BO(s) without CPU access could not be cleared by the SDMA ring and were refused "
                            "(they no longer take visible VRAM for a CPU clear; the UAPI tries GTT)"
                    % c["hi_clear_fail"]))
    if c.get("refused_other"):
        out.append(("WARN", "%d allocation(s) refused before the heaps were asked (no heap yet or any more, no "
                            "memory for the handle): not an exhaustion" % c["refused_other"]))
    if fb_refused and not ra:
        out.append(("WARN", "%d allocation(s) 'REFUSED by the framebuffer' in the log, but the core's accelerator "
                            "refusal counter is %d: another cause (the request, the BAR check) - read the log lines"
                    % (fb_refused, ra)))
    # the kernel's own refusal lines (GEM BOs as well as the accelerator's), first of each kind
    seen = set()
    for l in log.splitlines():
        m = re.search(r"RDNA3: (VRAM REFUSED #\d+: ([^,]*),.*)$", l)
        if m and m.group(2) not in seen and len(seen) < 4:
            seen.add(m.group(2))
            out.append(("INFO", "log: " + m.group(1)[:300]))
    if not any(lv in ("FAIL", "WARN") for lv, _ in out):
        out.insert(0, ("PASS", "no refusals (%d): the accelerator's reserve is %d KiB of the %d KiB visible heap; held at "
                               "the end: %s" % (refused, c.get("reserve_kib", 0), c.get("visible_kib", 0), owners)))
    out.extend(bomap_counter_lines(c))   # DRIVER-PLAN N47 (b): BO mappings, after the heap's own verdict
    return out


def latch_lines(path, txt, args=""):
    """[(level, text)] of a report's flip latch: the counters of its tests
    file, else the archive's 29-surface-vram.txt (collect.sh without
    autoreport: no tests file); 'not measured' when neither has a dump on
    an accelerator run"""
    c = a3_counters(txt) if txt else None
    where = "the tests file"
    sv = log = ioreg = ""
    if os.path.exists(path):
        try:
            with tarfile.open(path) as tar:
                sv = read_member(tar, "29-surface-vram.txt") or ""
                log = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt") or ""
                args = args or read_member(tar, "01-boot-args.txt") or ""
                ioreg = read_member(tar, "13-ioreg-accel.txt") or ""
        except (OSError, tarfile.TarError):
            pass
    if not has_latch_counters(c):
        c2 = a3_counters(sv) if sv else None
        if c2 and (has_latch_counters(c2) or not c):
            c, where = c2, "29-surface-vram.txt"
    want = fb_source_hash(ACCEL_HASH_FILES)
    ac = re.search(r"RDNA3Accelerator: build (\S+)", log)
    current = bool(want and ac and ac.group(1) == want)
    if not c:
        if "rdna3accel=1" in args or "rdna3accel=1" in txt:
            return [("INFO", "not measured: no rdna3accel counters in the tests file or 29-surface-vram.txt")]
        return []
    out = latch_verdict(c, current) + present_verdict(c, current, args, ioreg)
    if out and where != "the tests file":
        out.append(("INFO", "(counters from %s: no tests file dump)" % where))
    return out


# DRIVER-PLAN N38: the present queue's per-head counters (debug.rdna3accel_swaps_pq_<key>_h<N>)
PQ_KEYS = ("prog", "queued", "replaced", "joined", "busy", "refused", "latched", "latchus", "latchmax", "next",
           "timeout", "unknown", "dropped", "dropmode", "early", "wait", "waitus", "waitmax", "waittimeout", "inplace",
           "inline", "polled", "iop", "foreigntimeout", "giveup", "iopdrop", "fbwaitto", "fbwaitfast", "freedinflight",
           "home",
           # DRIVER-PLAN N42: latches ended by the head's pflip interrupt, its latency, live, a v1-only framebuffer
           "irqdone", "irqlatus", "irqlatmax", "irqlive", "fbwaitv1")
# Busy answers per present programmed (at once or at a latch) above which a head reads as stuck behind flips that
# do not latch (another path's, a blanked head): healthy queues see Busy only when an IOP / home flip interleaves
PQ_BUSY_SHARE = 0.25
# common/rdna3_flip_queue.h's RD3_FQ_STUCK_RUN (tests/test_trace_parity.py checks it)
PQ_STUCK_RUN = 3


def present_verdict(c, current=False, args="", ioreg=""):
    """[(level, text)] of the present queue (DRIVER-PLAN N38, common/rdna3_flip_queue.h) from its counters. Per
    head: presents programmed at once, queued (replaced: newest wins; Busy kept), joined; latches seen (avg, max),
    queued presents programmed at a latch, latches taken before the worker; writers (copy forward, page-on) that
    waited for the buffer leaving the panel; writes in place; IOP transactions. FAIL a completion before its latch
    (swaps_pq_early: the panel may still have read the buffer WindowServer had back), a frame lost with no newer
    one (swaps_pq_dropped), Busy answers on a head that never latched (stuck), more done-list entries than the
    kernel holds (swaps_pq_donemax); WARN a writer wait that ran out, flips the framebuffer refused, Busy heavy
    against the presents programmed, another path's flip given up / presents flipped anyway (a head whose flips
    stay pending), a buffer freed with its flip in flight, a queue that never engaged on a head that swapped in mode
    0, counters missing (also: a kernel built from these sources without swap_async at all), the queue OFF
    because it could not start, swap_async 0 that the boot-args did not ask for, perfstats 1 with nothing
    published; INFO the worker polling (an older framebuffer), latch waits answered at once, drops at a mode
    change, IOP transactions completed without their own flip, the IOP copy fallback's flips home, IOP flips copied
    for lack of the plugin's mark, that the latch-wait counters (latchpend / latchwait) do not measure flips under
    swap_async 1. [] for an older kernel without the queue."""
    on = c.get("swap_async")
    if on is None:
        if current and any(c.get(k) for k in c if re.match(r"swaps_h\d$", k)):
            return [("WARN", "present queue counters missing: the accelerator runs these sources but the dump has no "
                             "swap_async / swaps_pq_* (the collector's sysctl grep?) - unknown, not 0")]
        return []
    out = []
    if c.get("present_init_failed"):
        out.append(("WARN", "present queue OFF: it could not be set up at start (no lock or thread call; "
                            "present_init_failed 1) - swap_async and iop_queue were forced to 0"))
    if on == 0:
        asked = re.search(r"(?:^|\s)rdna3swapasync=0(?:\s|$)", args or "")
        if asked or c.get("present_init_failed"):
            out.append(("INFO", "present queue: debug.rdna3accel_swap_async was 0 - mode 0 swaps waited for their "
                                "latch (the old path)"))
        else:
            out.append(("WARN", "present queue: debug.rdna3accel_swap_async was 0 but the boot-args do not say "
                                "rdna3swapasync=0 - switched off at runtime (sysctl) or by an older default; mode 0 "
                                "swaps waited for their latch"))
    else:
        out.append(("INFO", "present queue on (swap_async 1): the latch counters swaps_latchpend / _latchwait do not "
                            "measure mode 0 flips any more - the swaps_pq_* lines below do"))
    mode = c.get("swap_mode")
    heads = sorted(set(m.group(1) for m in (re.match(r"swaps_h(\d)$", k) for k in c) if m))
    for h in heads:
        def g(k):
            return c.get("swaps_pq_%s_h%s" % (k, h))

        def v(k):
            x = g(k)
            return 0 if x is None else x
        engaged = v("prog") + v("queued") + v("joined")
        if not engaged and not any(v(k) for k in PQ_KEYS):
            sw = c.get("swaps_h" + h) or 0
            same = c.get("swaps_latchsame_h" + h) or 0
            if on == 1 and sw > same and mode in (0, None) and any(g(k) is not None for k in PQ_KEYS):
                out.append(("WARN", "head %s: %d swap(s) but the present queue never engaged (programmed + queued + "
                                    "joined 0, %d same-buffer) with swap_async 1 and swap_mode %s - the swaps took "
                                    "another path (a refused present? 'present queue:' lines in the log)" % (
                                        h, sw, same, "?" if mode is None else mode)))
            continue
        missing = [k for k in PQ_KEYS if g(k) is None]
        if missing:
            out.append(("WARN", "head %s: present queue counters missing (unknown, not 0): %s" % (
                h, ", ".join("swaps_pq_" + k for k in missing))))
        lat, waits = v("latched"), v("wait")
        out.append(("INFO", "head %s: present queue: %d present(s): %d programmed at once, %d queued (%d replaced: "
                    "newest wins, %d Busy kept), %d joined; %d latched (avg %d us, max %d us), %d programmed at a "
                    "latch, %d latch(es) taken before the worker; writers waited %d time(s) (avg %d us, max %d us), "
                    "%d write(s) in place; IOP %d (swap_async %s)" % (
                        h, engaged, v("prog"), v("queued"), v("replaced"), v("busy"),
                        v("joined"), lat, v("latchus") // lat if lat else 0, v("latchmax"), v("next"), v("inline"),
                        waits, v("waitus") // waits if waits else 0, v("waitmax"), v("inplace"), v("iop"), on)))
        if v("early"):
            out.append(("FAIL", "head %s: %d present(s) completed BEFORE their flip was seen latched (%d not latched in "
                                "time, %d unreadable) - the panel may still have read the buffer WindowServer had back"
                        % (h, v("early"), v("timeout"), v("unknown"))))
        if v("dropped"):
            out.append(("FAIL", "head %s: %d frame(s) LOST with no newer one (the buffer freed while queued, or "
                                "refused when programmed from the queue)" % (h, v("dropped"))))
        prog = v("prog") + v("next")
        if v("busy") and not lat:
            out.append(("FAIL", "head %s: %d Busy answer(s) and no latch ever seen - the head's flips never latch "
                                "(blanked, DPMS off, a stuck HUBP, another path's flip): %d given up, %d flipped "
                                "anyway" % (h, v("busy"), v("foreigntimeout"), v("giveup"))))
        elif v("busy") > PQ_BUSY_SHARE * max(prog, 1):
            out.append(("WARN", "head %s: Busy heavy: %d Busy answer(s) for %d present(s) programmed (%d%%) - another "
                                "path's flips keep the head's flip pending (IOP / home flips interleaved, a stuck "
                                "head?)" % (h, v("busy"), prog, 100 * v("busy") // max(prog, 1))))
        if v("foreigntimeout") or v("giveup"):
            out.append(("WARN", "head %s: the head's flips stayed pending: %d pending flip(s) given up, %d present(s) "
                                "flipped anyway after %d in a row (blanked, DPMS off, a stuck HUBP)" % (
                                    h, v("foreigntimeout"), v("giveup"), PQ_STUCK_RUN)))
        if v("waittimeout"):
            out.append(("WARN", "head %s: %d writer wait(s) ran out: a copy forward / page-on wrote a buffer the panel "
                                "may still have read" % (h, v("waittimeout"))))
        if v("refused"):
            out.append(("WARN", "head %s: %d flip(s) refused by the framebuffer (at the present: copied onto its "
                                "scanout instead)" % (h, v("refused"))))
        if v("freedinflight"):
            out.append(("WARN", "head %s: %d buffer(s) freed while their flip was in flight - the HUBP may have "
                                "scanned freed VRAM" % (h, v("freedinflight"))))
        if v("polled"):
            out.append(("INFO", "head %s: the present worker polled %d time(s) (an RDNA3Framebuffer without "
                                "rdna3FlipLatchWait: an older build)" % (h, v("polled"))))
        if v("fbwaitfast"):
            out.append(("INFO", "head %s: rdna3FlipLatchWait answered %d time(s) at once without a latch (the "
                                "framebuffer stopping, no latch lock, its state unreadable): the worker slept 1 ms "
                                "instead" % (h, v("fbwaitfast"))))
        # DRIVER-PLAN N42: flip done from the HUBP's flip interrupt (amdgpu's page-flip event semantics)
        if v("fbwaitv1"):
            out.append(("INFO", "head %s: the framebuffer answers only the version 1 latch wait (older than N42): "
                                "no flip-done interrupt information" % h))
        elif g("irqlive") is not None:
            idone = v("irqdone")
            if v("irqlive") and lat and not idone:
                out.append(("WARN", "head %s: the pflip interrupt is live but none of the %d latch(es) was ended by "
                                    "it - the HUBP's flip interrupt does not reach the framebuffer (the core's "
                                    "debug.rdna3_dispirq pflip count says whether it reaches the core)" % (h, lat)))
            elif idone:
                out.append(("INFO", "head %s: flip done from the pflip interrupt: %d of %d latch(es) (avg %d us, max "
                                    "%d us from the interrupt to the wait waking)%s" % (
                                        h, idone, lat, v("irqlatus") // idone, v("irqlatmax"),
                                        "" if v("irqlive") else "; the interrupt was OFF at the last wait")))
            elif not lat:
                # ECC review: with no latch wait the live flag was never sampled: no verdict on the interrupt
                out.append(("INFO", "head %s: no latch wait yet: whether the pflip interrupt ends them is not known "
                                    "(irqlive %d, sampled at a wait)" % (h, v("irqlive"))))
            else:
                out.append(("INFO", "head %s: the pflip interrupt %s: latches seen by the vblank / poll looks" % (
                    h, "live, no latch ended by it yet" if v("irqlive") else "not live at the last wait "
                       "(rdna3vblirq=0, no MSI, a storm, the display not brought back, or an older core)")))
        if v("dropmode"):
            out.append(("INFO", "head %s: %d queued present(s) dropped at a display mode change" % (h, v("dropmode"))))
        if v("iopdrop"):
            out.append(("INFO", "head %s: %d IOP transaction(s) completed without a flip of their own (replaced by a "
                                "newer one, joined, dropped at a flip home)" % (h, v("iopdrop"))))
        if v("home"):
            out.append(("INFO", "head %s: the IOP copy fallback flipped home %d time(s) over the queue's flip in "
                                "flight or queued" % (h, v("home"))))
    if c.get("swaps_pq_donemax"):
        out.append(("FAIL", "%d time(s) more IOP pipes / surface references at once than the present queue holds "
                            "(swaps_pq_donemax): a pipe not signalled then, or a reference leaked" %
                    c["swaps_pq_donemax"]))
    if c.get("swaps_pq_iopunwritten"):
        out.append(("INFO", "%d IOP zero-copy flip(s) copied instead: VRAM marked newest that the plugin never wrote "
                            "(iop_wrote %s)" % (c["swaps_pq_iopunwritten"], c.get("iop_wrote", "?"))))
    ps = c.get("perfstats")
    if ps is not None:
        w = c.get("perf_writes")
        fails = dict((k, c.get("perf_" + k, 0)) for k in ("nocore", "nokey", "dictfail", "setfail"))
        if ps and not w:
            out.append(("WARN", "GPU utilization NOT published into PerformanceStatistics (perf_writes %s; %d without "
                                "the core, %d without its Device Utilization %%, %d allocations failed, %d refused by "
                                "setProperty): Activity Monitor shows the family's own value" % (
                                    "missing" if w is None else w, fails["nocore"], fails["nokey"],
                                    fails["dictfail"], fails["setfail"])))
        elif ps:
            out.append(("INFO", "GPU utilization published into PerformanceStatistics %s time(s) (perfstats 1)" % w))
            if fails["dictfail"] or fails["setfail"]:
                out.append(("WARN", "GPU utilization updates lost: %d allocation(s) failed, %d refused by setProperty"
                            % (fails["dictfail"], fails["setfail"])))
        out += gpu_util_verdict(c, ioreg) if ps else []
    return out


GPUQ_DEVINIT = re.compile(r"(?m)^\S+ \S+ +\S+ +(.+?)\[(\d+):[0-9a-f]+\][^\n]*?NVMTL: -\[NVMTLDevice initWithAcceleratorPort:")
GPUQ_NOSUB = re.compile(r"perfstats: pid (\d+) used the GPU \((\d+) us")
# a device the plugin refused after its initWithAcceleratorPort line: another vendor's accelerator, not allow-listed, no Vulkan
GPUQ_REFUSED = re.compile(r"(?m)^\S+ \S+ +\S+ +.+?\[(\d+):[0-9a-f]+\][^\n]*?NVMTL: +(?:accelerator vendor [0-9a-f]+, but this process "
                          r"runs Metal on [0-9a-f]+: no second device|not allow-listed: no device|NVK did not come up)")
GPUQ_EVENT = re.compile(r"GPU time queue: (IOAccelCommandQueue opened|closed again) for .+? \(pid (\d+)")


def gpu_time_queue_rows(cdlog="", pluginlog="", klog=""):
    """DRIVER-PLAN N49 open item 1: the Metal plugin's one IOAccelCommandQueue per process (the IOAccelSubmitter2 the
    kernel credits the process's GPU time to: Activity Monitor's % GPU). [(level, name, text)] from its 'GPU time
    queue:' lines in WindowServer's (24-log-coredisplay.txt) and the other processes' (26-log-metal-plugin.txt) plugin
    logs, against the kernel's 'perfstats: pid N used the GPU ... has no IOAccelSubmitter2' lines (klog): INFO with
    the processes that opened one; WARN per failure step and IOReturn, for a close that failed too, for a pid the kernel
    could not credit although its plugin made a Metal device and opened no queue, and for plugin device lines with no
    'GPU time queue:' line at all (a plugin older than item 1); INFO for a pid whose time came before its queue opened
    (the start-up gap), for pids with no Metal device of the plugin, and when switched off. A pid whose FAILED first try
    was followed by an opened queue (the retry) is an INFO; a device the plugin refused (vendor, allow-list, Vulkan) is no
    Metal device. Limit: a release build of the plugin does not syslog its initWithAcceleratorPort / refusal lines, so
    there a pid without its queue line falls into the 'no Metal device' INFO (its FAILED line is still WARNed)."""
    text = (cdlog or "") + "\n" + (pluginlog or "")
    rows = []
    opened = re.findall(r"GPU time queue: IOAccelCommandQueue opened for (.+?) \(pid (\d+)", text)
    failed = re.findall(r"GPU time queue: FAILED at (.+?), IOReturn (0x[0-9a-f]+)[^\n]*? for (.+?) \(pid (\d+)", text)
    badclose = re.findall(r"GPU time queue: FAILED at [^\n]*?closing it FAILED too: IOReturn (0x[0-9a-f]+)\)[^\n]*? for (.+?) "
                          r"\(pid (\d+)", text)
    off = re.findall(r"GPU time queue: off \(NVMTL_GPUTIME_QUEUE=0\) for (.+?) \(pid (\d+)", text)
    refused = set(GPUQ_REFUSED.findall(text))
    devinit = dict((pid, name) for name, pid in GPUQ_DEVINIT.findall(text) if pid not in refused)
    last = {}
    for what, pid in GPUQ_EVENT.findall(text):   # in log order: a queue closed again (its device refused) no longer stands
        last[pid] = what
    opened_pids = set(pid for pid, what in last.items() if what != "closed again")
    retried = sorted(set((name, pid) for _, _, name, pid in failed if pid in opened_pids))
    failed = [f for f in failed if f[3] not in opened_pids]
    if opened:
        names = sorted(set(n for n, _ in opened))
        rows.append(("INFO", "GPU time queue", "%d process(es) opened their IOAccelCommandQueue (Activity Monitor can show their %% GPU): %s"
                     % (len(set(opened)), ", ".join(names[:10]) + (" ..." if len(names) > 10 else ""))))
    for (step, ret), procs in sorted(_group_gpuq(failed).items()):
        rows.append(("WARN", "GPU time queue", "%d process(es) without their IOAccelCommandQueue (Activity Monitor shows 0 %% GPU for "
                     "them): FAILED at %s, IOReturn %s (%s)" % (len(procs), step, ret, ", ".join(sorted(procs)[:8]))))
    if retried:
        rows.append(("INFO", "GPU time queue", "%d process(es) needed the retry (FAILED at the first device, opened at the next): %s"
                     % (len(retried), ", ".join("%s (%s)" % r for r in retried[:8]))))
    for ret, name, pid in badclose:   # WARN even after a working retry: the unstarted queue stays until the process exits
        rows.append(("WARN", "GPU time queue", "%s (pid %s): the queue that did not start was NOT closed (IOServiceClose %s): a "
                     "half-made IOAccelCommandQueue may stay under the accelerator until the process exits (perf_creditwait "
                     "grows with it)%s" % (name, pid, ret, " - its retry opened a working one" if pid in opened_pids else "")))
    if off:
        rows.append(("INFO", "GPU time queue", "switched off (NVMTL_GPUTIME_QUEUE=0) in %d process(es): Activity Monitor shows 0 %% "
                     "GPU for them" % len(set(off))))
    if devinit and "GPU time queue:" not in text:
        rows.append(("WARN", "GPU time queue", "%d process(es) made a Metal device of the plugin but no 'GPU time queue:' line was "
                     "logged: a plugin older than DRIVER-PLAN N49 item 1 (rebuild it) - Activity Monitor shows 0 %% GPU for "
                     "them" % len(devinit)))
    nosub = {}
    for pid, us in GPUQ_NOSUB.findall(klog or ""):
        nosub[pid] = nosub.get(pid, 0) + int(us)
    gap = sorted(p for p in nosub if p in opened_pids)
    if gap:
        rows.append(("INFO", "GPU time queue", "pid(s) %s used the GPU before their queue opened: that time stays uncredited "
                     "(perf_nosubns); only the time after the open reaches Activity Monitor" % ", ".join(gap[:10])))
    lost = sorted(p for p in nosub if p not in opened_pids and p in devinit)
    for p in lost[:10]:
        why = [st for st, _, _, fp in failed if fp == p]
        rows.append(("WARN", "GPU time queue", "pid %s (%s): %.1f ms of GPU time with no IOAccelSubmitter2 although its plugin made a "
                     "Metal device: %s" % (p, devinit[p], nosub[p] / 1e3, ("its queue FAILED at " + why[0]) if why else
                                           "it logged no opened queue (off, an older plugin, or the line was lost)")))
    other = sorted(p for p in nosub if p not in opened_pids and p not in devinit)
    if other:
        rows.append(("INFO", "GPU time queue", "pid(s) %s used the GPU with no submitter and no Metal device of the plugin in the "
                     "logs (a DRM / Vulkan-only process, or a process whose device line was not collected)"
                     % ", ".join(other[:10])))
    return rows


def _group_gpuq(failed):
    """{(step, IOReturn): {"name (pid)"}} of the plugin's 'GPU time queue: FAILED' lines"""
    out = {}
    for step, ret, name, pid in failed:
        out.setdefault((step, ret), set()).add("%s (%s)" % (name, pid))
    return out


GPUQ_CREDITWAIT_WARN = 16   # unstarted-submitter misses above this are not the open-to-connect gap any more

UNC_KINDS = (("vcn", "VCN encode jobs"), ("jpeg", "JPEG jobs"), ("other", "jobs on rings other than 0"),
             ("kcopy", "kernel SDMA copies / clears"), ("userq", "user queues (their jobs never reach the kernel)"))


def ioreg_gpu(ioreg):
    """(Device Utilization % or None, [accumulatedGPUTime values]) from 13-ioreg-accel.txt (DRIVER-PLAN N49)"""
    m = re.search(r'"Device Utilization %"=(\d+)', ioreg or "")
    acc = [int(v) for v in re.findall(r'"accumulatedGPUTime" = (\d+)', ioreg or "")]
    return (int(m.group(1)) if m else None), acc


def gpu_util_verdict(c, ioreg=""):
    """[(level, text)] for Activity Monitor's GPU numbers from the rings' busy time (DRIVER-PLAN N49): the
    accelerator's perf_rings / perf_smu / perf_util* and the per-process GPU time it credited (perf_gpuns) or
    could not (perf_nosubns: no IOAccelSubmitter2 for the process), what the busy time does not count
    (perf_unc_*), and the accelerator's ioreg (13-ioreg-accel.txt) against the counters"""
    out = []
    if c.get("perf_rings") is None:
        return out   # an accelerator older than N49
    rings, smu, noans = c.get("perf_rings", 0), c.get("perf_smu", 0), c.get("perf_noanswer", 0)
    if rings:
        out.append(("INFO", "GPU utilization from the rings' busy time: %d update(s), last %s %% (GFX %s %%), %s job(s)"
                    % (rings, c.get("perf_util", "?"), c.get("perf_utilgfx", "?"), c.get("perf_jobs", "?"))))
    if smu or (noans and not rings):
        lr = c.get("perf_lastret")
        out.append(("WARN", "GPU utilization from the SMU's GFX activity (100 %% at idle on this card, P2) in %d "
                            "update(s): the core gave no ring busy time %d time(s), last answer %s (0xe00002c7 an older "
                            "core, 0xe00002d8 no busy accounting / no GPU yet, 0xe00002c2 an ABI version mismatch; the "
                            "kernel's 'perfstats: the core's ring busy time:' lines)" % (
                                smu, noans, "?" if lr is None else "0x%x" % lr)))
    unc = [(c.get("perf_unc_" + k, 0), what) for k, what in UNC_KINDS]
    if any(n for n, _ in unc):
        txt = ", ".join("%d %s" % (n, what) for n, what in unc if n)
        if not c.get("perf_jobs"):
            out.append(("WARN", "GPU work the busy time does not count (%s) and no counted job at all (perf_jobs 0): "
                                "Activity Monitor's GPU numbers miss all of it" % txt))
        else:
            out.append(("INFO", "not in the busy time: %s" % txt))
    gpu, nosub = c.get("perf_gpuns", 0), c.get("perf_nosubns", 0)
    if gpu:
        out.append(("INFO", "%.1f s of GPU time added to the processes' contexts (accumulatedGPUTime: Activity "
                            "Monitor's %% GPU / GPU Time)" % (gpu / 1e9)))
    if nosub:
        out.append(("INFO", "%.1f s of GPU time (%d take(s)) of processes with no IOAccelSubmitter2 under the "
                            "accelerator: Activity Monitor shows 0 %% GPU for them (DRIVER-PLAN N49 open item; the "
                            "kernel's 'has no IOAccelSubmitter2' lines name the pids; the last walk saw %s submitter(s))"
                    % (nosub / 1e9, c.get("perf_nosub", 0), c.get("perf_submitters", "?"))))
    cq = c.get("perf_cmdqueues")
    if cq is not None:
        if cq:
            out.append(("INFO", "%d IOAccelCommandQueue(s) under the accelerator at the last credit walk (the Metal plugin "
                                "opens one per Metal process: DRIVER-PLAN N49 open item 1)" % cq))
        elif nosub:
            # INFO (ECC round 1 of item 1): the walk is the last one, the Metal processes may have exited by then; the
            # 'GPU time queue' rows hold the per-pid verdict (the kernel's no-submitter lines against the plugin's)
            out.append(("INFO", "no IOAccelCommandQueue under the accelerator at the last credit walk, and %.1f s of GPU "
                                "time had no IOAccelSubmitter2 (the 'GPU time queue' rows name the pids)" % (nosub / 1e9)))
    cw, pend = c.get("perf_creditwait", 0), c.get("perf_pendingns", 0)
    if cw:
        out.append(("WARN" if cw > GPUQ_CREDITWAIT_WARN else "INFO",
                    "%d take(s) found a process's only submitter not started yet (%.1f ms not credited: the gap between "
                    "IOServiceOpen and IOConnectAddClient%s)" % (cw, pend / 1e6, "; this many means one that stays "
                    "unstarted - the kernel's 'has no accumulatedGPUTime yet' lines name it" if cw > GPUQ_CREDITWAIT_WARN
                    else ", transient")))
    if c.get("perf_uccbad"):
        out.append(("WARN", "%d IOAccelSubmitter2 entr(y/ies) without a readable IOUserClientCreator pid: their "
                            "processes' time counts as 'no IOAccelSubmitter2' though they have one" % c["perf_uccbad"]))
    if rings and not gpu and not nosub and not c.get("perf_pendingns") and c.get("perf_jobs"):
        out.append(("WARN", "the rings ran %s job(s) but no process got GPU time (perf_gpuns 0, perf_nosubns 0, perf_pendingns 0)"
                    % c.get("perf_jobs")))
    if c.get("perf_creditfail"):
        out.append(("WARN", "%d time(s) GPU time NOT credited: no child iterator, or a submitter whose accumulatedGPUTime "
                            "is not a 64-bit number (lasting; the kernel's perfstats lines name the class)" % c["perf_creditfail"]))
    if c.get("perf_nolast"):
        out.append(("WARN", "%d credited context(s) without a 64-bit lastSubmittedTime: Activity Monitor may not list "
                            "the process as submitting" % c["perf_nolast"]))
    if c.get("perf_stale"):
        out.append(("WARN", "%d job(s) flushed as stale: no fence for 30 s and no reset the core saw (the serial "
                            "log's 'GPU busy: ... flushed' line names the ring)" % c["perf_stale"]))
    if c.get("perf_flushed") or c.get("perf_lostns") or c.get("perf_merged") or c.get("perf_cappedns"):
        out.append(("INFO", "busy accounting: %d job(s) flushed with no time (a ring reset / a fence never seen), %d "
                            "merged into a full queue, %.1f ms with no process slot, %.1f ms dropped above the wall "
                            "time" % (c.get("perf_flushed", 0), c.get("perf_merged", 0), c.get("perf_lostns", 0) / 1e6,
                                      c.get("perf_cappedns", 0) / 1e6)))
    se = [c.get("perf_sdmaend_" + k, 0) for k in ("irq", "wait", "take", "sub", "reset")]
    if sum(se) and se[2] * 4 > sum(se):
        out.append(("INFO", "%d of %d SDMA job(s) ended only at the 250 ms take (no waiter read their fence): the SDMA "
                            "busy time is up to 250 ms long per gap" % (se[2], sum(se))))
    if c.get("perf_winreset", 0) > 1:
        out.append(("INFO", "%d take(s) not published: the core's busy totals went back (a core restart?)"
                    % (c["perf_winreset"] - 1)))
    util, acc = ioreg_gpu(ioreg)
    if acc and gpu and not any(acc):
        # INFO (ECC round 2): the credited processes may have exited before the ioreg ran
        out.append(("INFO", "13-ioreg-accel.txt: every accumulatedGPUTime is 0 although %.1f s were credited (the "
                            "credited processes exited before the ioreg, it ran before the credits, or the family "
                            "replaced its numbers)" % (gpu / 1e9)))
    if util is not None and rings:
        if util == 100 and c.get("perf_util", 100) <= 50:
            out.append(("WARN", "13-ioreg-accel.txt: Device Utilization %% = 100 while the rings' last value is %s %%: "
                                "the family's or the SMU's value overwrote ours (the ioreg and the counters are read "
                                "at different times)" % c.get("perf_util")))
        else:
            out.append(("INFO", "13-ioreg-accel.txt: Device Utilization %% = %d (the rings' last value %s %%, read at "
                                "another time)" % (util, c.get("perf_util", "?"))))
    return out


# the kernel's 'present queue:' / 'perfstats:' lines in a serial log (DRIVER-PLAN N38; a serial log alone, the
# counters not dumped): (level, regex, what) - each judged with its count, the largest '(N such)' when the line
# carries one
SERIAL_PQ = (
    ("FAIL", r"present queue: head \d+: the flip of .*? completed EARLY", "completion(s) BEFORE the latch was seen"),
    ("FAIL", r"present queue: head \d+: the queued .*? DROPPED: its buffer was freed",
     "queued frame(s) LOST: the buffer freed"),
    ("FAIL", r"present queue: head \d+: the queued .*? was REFUSED by the framebuffer",
     "queued frame(s) LOST: refused when programmed from the queue"),
    ("FAIL", r"present queue: more than \d+ (?:IOP pipes|surface references)",
     "done-list overflow(s): a pipe not signalled or a reference leaked"),
    ("FAIL", r"present queue: no lock or thread call -- OFF", "present queue OFF at start (swap_async forced to 0)"),
    ("WARN", r"present queue: head \d+: [^\n]*? REFUSED by the framebuffer \(0x", "present(s) refused (copied instead)"),
    ("WARN", r"present queue: head \d+: [^\n]*? -- NOT latched in time, written while", "writer wait(s) ran out"),
    ("WARN", r"present queue: head \d+: its flips stay PENDING", "present(s) flipped anyway: the head's flips stay "
             "pending (blanked, DPMS off, a stuck HUBP)"),
    ("WARN", r"present queue: head \d+: the flip pending before .*? given up", "pending flip(s) given up"),
    ("WARN", r"present queue: head \d+: the buffer of .*? was FREED while its flip was in flight",
     "buffer(s) freed with their flip in flight"),
    ("WARN", r"perfstats: [^\n]*? -- (?:the accelerator's GPU utilization not updated|GPU utilization \d+ % NOT "
             r"published)", "GPU utilization update(s) skipped or lost"),
    ("WARN", r"perfstats: the core did not give its ring busy time", "GPU utilization from the SMU (100 % at idle, P2):"
             " no ring busy time from the core (N49)"),
    ("WARN", r"perfstats: (?:no child iterator|pid \d+'s \S+ has no accumulatedGPUTime number)",
     "time(s) a process's GPU time was NOT credited (N49)"),
    ("WARN", r"GPU busy: no memory for the ring busy accounting", "no ring busy accounting in the core (N49)"),
    ("INFO", r"perfstats: pid \d+ used the GPU [^\n]*? has no IOAccelSubmitter2",
     "process(es) with GPU time Activity Monitor cannot show: no IOAccelSubmitter2 (N49)"),
    ("WARN", r"GPU busy: \d+ job\(s\) flushed [^\n]*?; (?!0 stale)\d+ stale", "stale flush(es): a ring's jobs never "
             "saw their fence for 30 s and no reset said so (N49)"),
    ("INFO", r"GPU busy: \d+ job\(s\) (?:flushed [^\n]*?; 0 stale|merged|with no process slot)|GPU busy: \d+ us "
             r"of process GPU time dropped", "busy accounting line(s): jobs flushed / merged / with no slot / capped (N49)"),
    ("INFO", r"GPU busy: [^\n]*? is not in the ring busy time", "kind(s) of GPU work the busy time does not count (N49)"),
    ("INFO", r"GPU busy: [^\n]*? ended \d+ GFX / \d+ compute / \d+ SDMA job\(s\) in flight",
     "reset(s) that ended jobs in flight (N49)"),
    ("INFO", r"present queue: head \d+: the queued .*? DROPPED: (?!its buffer was freed)",
     "queued present(s) dropped at a display mode change"),
    ("INFO", r"present queue: head \d+: the framebuffer has no rdna3FlipLatchWait", "an older framebuffer: the "
             "worker polls every 1 ms"),
    ("INFO", r"present queue: head \d+: rdna3FlipLatchWait answered", "latch wait(s) answered at once (1 ms sleeps)"),
    ("INFO", r"present queue: head \d+: the IOP copy fallback flipped home", "flip(s) home over the queue"),
)


def serial_present_verdict(text):
    """[(level, text)] from the kernel's 'present queue:' and 'perfstats:' lines in a serial log (DRIVER-PLAN N38):
    see SERIAL_PQ. Plus, when the log says the queue was on ('swap async: 1 ='), INFO that the latch summary's
    latchpend / latchwait do not measure mode 0 flips any more. [] for a log without these lines."""
    out = []
    for lv, rx, what in SERIAL_PQ:
        hits = list(re.finditer(rx + r"[^\n]*", text))
        if not hits:
            continue
        such = [int(m.group(1)) for m in (re.search(r"[(;] ?(\d+) (?:such|lost|so far)\)", x.group(0)) for x in hits) if m]
        n = max(such + [len(hits)])
        out.append((lv, "present queue (serial): %d %s - e.g. '%s'" % (n, what, hits[0].group(0).strip()[:160])))
    if re.search(r"swap async: 1 = ", text):
        out.append(("INFO", "present queue on (swap async: 1): the 'swap latch' line's latchpend / latchwait do not "
                            "measure mode 0 flips any more - the present queue's lines do"))
    elif re.search(r"swap async: 0 = ", text):
        out.append(("INFO", "present queue off (swap async: 0): mode 0 swaps waited for their latch"))
    return out


SERIAL_LATCH = re.compile(
    r"swap latch: head (\d+): (\d+) swaps; previous flip not latched at entry (\d+); latched while waited (\d+) \(avg "
    r"(\d+) us, max (\d+) us\), (\d+) timeouts, (\d+) unknown, (\d+) not waited; shown buffers hashed again (\d+): "
    r"(\d+) changed on the panel then flipped away \(torn\), (\d+) rewritten in place \(one back\), (\d+) not hashed; "
    r"copy forwards into the buffer on the panel (\d+); buffers freed on the panel (\d+); empty backs read again: "
    r"(\d+) filled, (\d+) still empty \(swap_latch (-?\d+)(?:, card run [^)]*\); latched at once (\d+), flips failed "
    r"(\d+), waits (on|OFF)(?: \(timeouts\))?(?:; same buffer not flipped (\d+) \(swap_samenoflip (-?\d+)\))?)?")


def serial_latch_verdict(text, seen=()):
    """[(level, text)] from the kernel's last 'swap latch: head N' line per
    head in a serial log (card run 2026-10-17), as latch_verdict judges the
    report's counters; a head that swapped without such a line, in a log of
    a kernel that has them, is INFO 'not measured'"""
    last = {}
    for m in SERIAL_LATCH.finditer(text):
        g = list(m.groups()[1:])
        g[-3] = None if g[-3] is None else int(g[-3] == "OFF")
        last[m.group(1)] = [None if x is None else int(x) for x in g]
    out = []
    for h, v in sorted(last.items()):
        (swaps, pend, waits, avg, mx, tout, unk, off, chk, torn, inpl, tskip, onp, freed, filled, still,
         latch_on, imm, failed, waits_off, same, same_on) = v
        c = {"swaps_h" + h: swaps, "swaps_latchpend_h" + h: pend, "swaps_latchwait_h" + h: waits,
             "swaps_latchus_h" + h: avg * waits, "swaps_latchmax_h" + h: mx, "swaps_latchtimeout_h" + h: tout,
             "swaps_latchunknown_h" + h: unk, "swaps_latchoff_h" + h: off, "swaps_tornchk_h" + h: chk,
             "swaps_torn_h" + h: torn, "swaps_inplace_h" + h: inpl, "swaps_tornskip_h" + h: tskip,
             "cf_onpanel_h" + h: onp, "swaps_shownfreed_h" + h: freed, "swaps_emptyfilled_h" + h: filled,
             "swaps_emptystill_h" + h: still, "swap_latch": latch_on}
        if same_on is not None:
            c["swap_samenoflip"] = same_on
        for k, x in (("swaps_latchimmediate", imm), ("swaps_flipfailed", failed), ("swaps_latchwaitsoff", waits_off),
                     ("swaps_latchsame", same)):
            if x is not None:
                c[k + "_h" + h] = x
        for lv, t in latch_verdict(c, report_missing=False):
            # the summary line does not carry swap_torn: a torn test that did
            # not run may have been switched off on purpose (INFO, not WARN)
            if lv == "WARN" and "torn test not measured" in t:
                lv, t = "INFO", t + " - swap_torn is not in the serial line, it may be 0 on purpose"
            out.append((lv, "%s (flip latch, serial at swap %d)" % (t, swaps)))
    if "swap latch:" in text:
        for h in sorted(set(seen) - set(last)):
            out.append(("INFO", "head %s: flip latch not measured (no 'swap latch: head %s' line in the log)" % (h, h)))
    return out


def a3_counters(txt):
    """the last dump of the card run 58 sysctls in the tests file: {name: value},
    None when the kext had none ("no rdna3accel_a3display sysctl")"""
    if "no rdna3accel_a3display sysctl" in txt and not A3_SYSCTL.search(txt):
        return None
    out = {}
    for k, v in A3_SYSCTL.findall(txt):
        out[k] = int(v)
    return out


def git_out(*args):
    """stdout of a git command in this checkout, or None (no git, no repo,
    non-zero exit). The report is untrusted: only hex ids reach `args`."""
    try:
        r = subprocess.run(["git", "-C", REPO_ROOT] + list(args), capture_output=True,
                           text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def parse_stamps(text):
    """35-build-stamps.txt -> {"plugin": {key: value} or None, "radv": ...}"""
    out, cur = {}, None
    for l in text.splitlines():
        m = re.match(r"== (plugin|radv) \(", l)
        if m:
            cur = m.group(1)
            out[cur] = None
        elif cur and re.match(r"[a-z]+=", l):
            out[cur] = out[cur] or {}
            k, v = l.split("=", 1)
            out[cur][k] = v.strip()
    return out


def parse_host(text):
    """00-host.txt (tools/vm/rdna3vm result): "repo <sha> <subject>",
    "repo-metal <sha>", "repo-mesa <sha>" -> {"repo": sha, ...}"""
    out = {}
    for m in re.finditer(r"^(repo(?:-metal|-mesa)?) ([0-9a-f]{7,40})\b", text, re.M):
        out[m.group(1)] = m.group(2)
    return out


def stamp_verdict(kind, stamp, host):
    """("ok" | "stale" | "unknown", text) for one build's stamp"""
    key, hkey, paths = STALE_SOURCES[kind]
    sha = (stamp or {}).get(key, "")
    if not HEX_SHA.match(sha):
        return "unknown", "%s stamp has no revision (%s)" % (kind, sha or "empty")
    ref = host.get("repo", "")
    if not (HEX_SHA.match(ref) and git_out("cat-file", "-e", ref + "^{commit}") is not None):
        ref = "HEAD"        # the host's commit is not in this clone
    target = git_out("log", "-1", "--format=%H", ref, "--", *paths) or host.get(hkey)
    if not target:
        return "unknown", "%s: no git history here and no %s line in 00-host.txt" % (kind, hkey)
    where = "/".join(paths) if len(paths) == 1 else paths[0]
    if target.startswith(sha) or sha.startswith(target):
        return "ok", "%s %s at %s = the repo's" % (kind, where, sha)
    subj = git_out("log", "-1", "--format=%s", target) or ""
    both = git_out("cat-file", "-e", sha + "^{commit}") is not None and \
        git_out("cat-file", "-e", target + "^{commit}") is not None
    if both and git_out("merge-base", "--is-ancestor", sha, target) is not None:
        return "stale", "%s built from %s at %s, the repo's last commit there is %s%s" % (
            kind, where, sha, target[:7], " (%s)" % subj[:70] if subj else "")
    if both and git_out("merge-base", "--is-ancestor", target, sha) is not None:
        return "ok", "%s %s at %s is newer than the repo's %s" % (kind, where, sha, target[:7])
    return "unknown", "%s %s at %s, the repo's is %s: cannot tell which is older (commit not in this clone)" % (
        kind, where, sha, target[:7])


def stale_builds(stamps_txt, host_txt, plugin_log):
    """Prints the stamps / STALE lines; True when a stale build was proven."""
    stamps, host = parse_stamps(stamps_txt), parse_host(host_txt)
    has_plugin = "NVMTL" in plugin_log
    if not stamps_txt.strip() and not has_plugin:
        return False
    reasons = []
    notes = []
    for kind, name in (("PLUGIN", "plugin"), ("RADV", "radv")):
        st = stamps.get(name)
        if not st:
            if name == "plugin" and not has_plugin:
                continue
            notes.append("%s: no stamp (built before stamps existed%s)" % (
                kind, ", or installed from a CI artifact" if kind == "RADV" else ""))
            continue
        verdict, text = stamp_verdict(kind, st, host)
        notes.append("%s %s (built %s)" % (text, "STALE" if verdict == "stale" else "", st.get("built", "?")))
        if verdict == "stale":
            reasons.append(text)
    # the log is the ground truth whatever the stamp says (and the only evidence without one)
    if not any(r.startswith("PLUGIN") for r in reasons) and "vk: G14 primitive topology" in plugin_log and \
            not re.search(r"vk: VK_EXT_extended_dynamic_state (?:ENABLED|ABSENT)", plugin_log):
        reasons.append("PLUGIN log has the 'vk: G14 primitive topology' line but no 'vk: "
                       "VK_EXT_extended_dynamic_state ENABLED/ABSENT' line: a plugin older than eb5e3e8 "
                       "(the Chess crash fix, Mesa patch 0008)")
    print("   INFO %-20s %s" % ("build stamps", "; ".join(n.replace("  ", " ") for n in notes) or "none"))
    if reasons:
        print("   FAIL STALE PLUGIN/RADV: %s; run build_radv.sh and build_metal.sh in the VM "
              "(docs/TESTING-VM.md) - the plugin / RADV results of this run do not test the current code" %
              "; ".join(reasons))
    return bool(reasons)


# The kexts macOS loads from the Auxiliary KC only after the user allowed each new build ('rdna3vm approve'):
# until then macOS keeps running the build approved before. Card runs 2026-10-18b and 2026-10-19b tested the old
# RDNA3Accelerator (e35e78c275) three times in a row: the new build (4fcf2bc0b7) was installed but never allowed,
# and nothing said so. kext, the serial log's build line, the sources that build is hashed from
KEXT_SERIAL_BUILDS = (
    ("RDNA3Accelerator", re.compile(r"RDNA3Accelerator: build ([0-9a-f]{7,})"), ACCEL_HASH_FILES),
    ("RDNA3Framebuffer", re.compile(r"RDNA3-fb: started .*?, build ([0-9a-f]{7,})"), FB_HASH_FILES),
    ("RDNA3AGDC", re.compile(r"RDNA3-agdc: build ([0-9a-f]{7,})"), AGDC_HASH_FILES),
)
# autoreport.sh's approve-*.txt (stale_kexts): "<kext>: running <hash>, installed <hash> (not approved yet)"; the
# tests file's "stale: <kext>: running X, installed Y (not approved yet);..." line holds the same, joined with ';'
APPROVE_MARKER_RE = re.compile(r"^(RDNA3\w+): running (\w+), installed (\w+)", re.M)
STALE_LINE_RE = re.compile(r"(RDNA3\w+): running (\w+), installed (\w+)")
# the action first: result_checklist cuts a long row at the terminal's width
STALE_KEXTS_HEAD = "old driver ran - run 'rdna3vm approve', then the step again"


def approve_command(host_text):
    """the command that gives Allow under the Mac model the run had: 'rdna3vm approve', or 'rdna3vm approve --step N'
    when the run's host log shows an @smbios= token in its boot-args (plain approve runs the config's model, and
    macOS may not keep an Allow across a model change). N is looked up in ladder-steps.txt by the step's tokens"""
    ba = _host_boot_args(host_text)
    m = re.search(r"(?:^|\s)@smbios=(\S+)", ba)
    if not m:
        return "rdna3vm approve"
    toks = set(ba.split())
    host_at = {t for t in toks if t.startswith("@")}
    best, best_n = None, -1
    try:
        lines = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ladder-steps.txt"), errors="replace").read().splitlines()
    except OSError:
        lines = []
    for line in lines:
        parts = line.split()
        if len(parts) < 2 or parts[0].startswith("#"):
            continue
        st = set(parts[1:])
        if st <= toks and {t for t in st if t.startswith("@")} == host_at and len(st) > best_n:
            best, best_n = parts[0], len(st)
    return "rdna3vm approve --step %s" % (best or "<the step that ran with @smbios=%s>" % m.group(1))
# ---- present mode (card run 2026-10-17d): WindowServer presents with the legacy page-flip
# mode ("init_page_flip: page flip mode is on"), never IOPresentment transactions. What
# differs from the NVIDIA setups that get transactions: IOGraphics sets IOFBIntegrated
# when the card's PCI bus number is 0, GPUWrangler then logs "(IG)", and the Mac model
# is a muxed laptop (CoreDisplay: isBDW=YES isMobile=YES). One INFO row each for every
# run (rdna3vm steps play-macpro / play-rootport / play-desktop change them), WARN
# where a value is missing or contradicts what the host asked for.
GPUW_RE = re.compile(r"gpu (0x[0-9a-f]+) flags (0x[0-9a-f]+)(?: \(([^)]*)\))? vid\.did=(\S+) b:d:f=(\d+):(\d+):(\d+)")


def _rdna3_tokens(args):
    """the rdna3 boot-args of a boot-args string, as a set (what a host log and a report share; the host's own
    @tokens, -v, debug= etc. are not compared)"""
    return frozenset(t for t in (args or "").split() if t.lstrip("-").startswith("rdna3"))


def _host_boot_args(text):
    m = re.search(r"^boot-args: (.*)$", text, re.M)
    return m.group(1) if m else ""


def _host_request(text):
    """(model, bus) a host log says rdna3vm asked for: model = "MacPro7,1" / "not asked" / None (no 'SMBIOS model:'
    line), bus = "pcie0" / "rootport" / None (no 'GPU on:' line); the last line of each wins"""
    m = re.findall(r"^\s*SMBIOS model: (not asked|[A-Za-z]+\d+,\d+)", text, re.M)
    b = re.findall(r"^\s*GPU on: (\w+)", text, re.M)
    return (m[-1] if m else None, b[-1] if b else None)


def _read(path):
    try:
        return open(path, errors="replace").read()
    except OSError:
        return ""


def host_log_for(path, args=""):
    """(text, status, note, name): the host-*.log (tools/vm/rdna3vm) of the VM run a report belongs to. Matched by
    CONTENT: the log's 'boot-args:' line against the report's boot-args (01-boot-args.txt, or the tests file's
    'boot-args:' line), compared on their rdna3 tokens. Not by the clock stamps: the host's file name uses the host's
    local time, the report's the guest's (QEMU's RTC is UTC), so they differ by the time zone.
    status: "matched" (one log, or several that ask for the same), "ambiguous" (several logs carry these boot-args and
    ask for different things: the newest is used), "unmatched" (no log has them, or the report has no boot-args: the
    newest log by modification time, as the rest of the triage orders files; a guess), "none" (no host log)"""
    d = os.path.dirname(os.path.abspath(path))
    logs = sorted(glob.glob(os.path.join(d, "host-*.log")), key=lambda f: (os.path.getmtime(f), f))
    if not logs:
        return "", "none", "no host-*.log next to the report", ""
    texts = {l: _read(l) for l in logs}
    rt = _rdna3_tokens(args)
    newest = logs[-1]
    if not rt:
        return texts[newest], "unmatched", "the report has no boot-args to match the host logs by", os.path.basename(newest)
    cands = [l for l in logs if _rdna3_tokens(_host_boot_args(texts[l])) == rt]
    if not cands:
        return texts[newest], "unmatched", "none of the %d host log(s) has this report's rdna3 boot-args (%s)" % (
            len(logs), " ".join(sorted(rt))[:120]), os.path.basename(newest)
    pick = cands[-1]
    if len(cands) > 1 and len({_host_request(texts[l]) for l in cands}) > 1:
        return texts[pick], "ambiguous", "%d host logs carry this report's boot-args and ask for different things (%s)" % (
            len(cands), ", ".join(os.path.basename(l) for l in cands)), os.path.basename(pick)
    return texts[pick], "matched", "", os.path.basename(pick)


def guest_pci_rows(host):
    """[(level, text)]: the card in the guest's PCI bus ('info pci' at 8 and 40 s, host-*.log): its bus and whether the
    BARs are mapped (an address of all ones = not assigned)"""
    snaps = re.findall(r"== guest PCI bus \(QEMU 'info pci', (\d+) s after start\)\n(.*?)(?=\n== |\Z)", host, re.S)
    if not snaps:
        return [("WARN", "the host log has no 'guest PCI bus' (info pci) output: the VM ended before 8 s, or the QEMU "
                         "monitor did not answer; the card's bus and BARs as the guest has them are unknown")]
    out = []
    for secs, blk in snaps:
        parts = re.split(r"(?m)^\s*Bus\s+(\d+), device\s+(\d+), function (\d+):", blk)
        card = [(parts[i], parts[i + 1], parts[i + 2], parts[i + 3]) for i in range(1, len(parts) - 3, 4)
                if re.search(r"1002:747e", parts[i + 3], re.I)]
        if not card:
            out.append(("WARN", "%s s: the card (1002:747e) is not in the guest's PCI list%s" % (
                secs, "" if blk.strip() else " (the list is empty: the monitor did not answer)")))
            continue
        for bus, dev, fn, body in card:
            bars = re.findall(r"BAR([0-5]): .*? at (0x[0-9a-fA-F]+)", body)
            mapped = [n for n, a in bars if int(a, 16) != 0xFFFFFFFFFFFFFFFF]
            unmapped = [n for n, a in bars if int(a, 16) == 0xFFFFFFFFFFFFFFFF]
            out.append(("WARN" if (unmapped or not mapped) else "INFO",
                        "%s s: PCI bus %s, device %s, function %s; %s%s%s" % (
                            secs, bus, dev, fn, "BAR%s mapped" % ",".join(mapped) if mapped else "no BAR mapped",
                            "; NOT mapped: BAR" + ",".join(unmapped) if unmapped else "",
                            "" if bars else " (no BAR lines)")))
    return out


def host_request_rows(host, status, note, name, bare=False):
    """[(level, label, text)] about the host log itself: absent, guessed, ambiguous, or without the lines that say what
    rdna3vm asked for (Mac model, card bus). Never silence: a request that is not known is a WARN"""
    out = []
    if status == "none":
        out.append(("INFO" if bare else "WARN", "request unknown", "%s: %s" % (
            note, "bare metal run, no VM host log expected" if bare else
            "what rdna3vm asked for (Mac model, card bus) and where the card sat in the guest are unknown")))
        return out
    if status in ("unmatched", "ambiguous"):
        out.append(("WARN", "host log not matched", "%s; %s is used as a guess (the newest), so the rows from it "
                    "(asked model, card bus) may belong to another run" % (note, name)))
    model, bus = _host_request(host)
    for val, what, line in ((model, "Mac model", "'SMBIOS model:'"), (bus, "card bus", "'GPU on:'")):
        if val is None:
            out.append(("WARN", "request unknown", "%s has no %s line: the %s rdna3vm asked for is unknown (an rdna3vm "
                        "older than the present-mode A/B, or the run ended before it was written)" % (name, line, what)))
    return out


def present_host_rows(host, status, note, name, bare=False):
    """[(level, label, text)] from the host log alone (a TESTS-ONLY report, or no report at all): the asked model, the
    asked bus and the card as the guest's PCI bus has it"""
    out = host_request_rows(host, status, note, name, bare)
    if status == "none":
        return out
    model, bus = _host_request(host)
    if model:
        out.append(("INFO", "asked model", "rdna3vm asked for no model: the OpenCore config is untouched" if model == "not asked"
                    else "rdna3vm asked for %s" % model))
    if bus:
        out.append(("INFO", "asked bus", "rdna3vm put the card on %s%s" % (bus, " (PCI bus 1)" if bus == "rootport" else " (PCI bus 0)")))
    for lv, t in guest_pci_rows(host):
        out.append((lv, "guest PCI", t))
    return out


def _fb_blocks(fbtxt):
    """[(OpenGLIndex, DependentIndex, Integrated)] of 12-ioreg-fb.txt, one per framebuffer (None = no such key)"""
    out = []
    for blk in re.split(r"(?m)^\+-o ", fbtxt)[1:]:
        g = lambda k: (re.search(r'"%s" = (\w+)' % k, blk) or [None, None])[1]
        out.append((g("IOFramebufferOpenGLIndex"), g("IOFBDependentIndex"), g("IOFBIntegrated")))
    return out


# DRIVER-PLAN N44: CoreDisplay's GetSystemGPUVendors logs, per IOFramebuffer, the vendor bit
# _GetGPUVendorForFramebufferService gave it from the LEAF class name (IOObjectCopyClass):
# "GetSystemGPUVendors_block_invoke: <entry id> <vendor> <class> IOService:/<path>". Vendor 1
# (unknown) on any framebuffer keeps IOPresentment off for the whole WindowServer session.
# FB_CLASS is RDNA3_FB_CLASS of fb/rdna3-fb.cpp (tests/test_fb_class.py keeps them equal);
# FB_CLASS_OLD is the class of every framebuffer before N44 (old reports still parse). The strings
# CoreDisplay looks for (probe on 24H32, in this order): "Intel" 2, "AMD" 4, "AppleParavirt" 0x10, "NVDA" 8.
FB_CLASS = "AMDRDNA3Framebuffer"
FB_CLASS_OLD = "RDNA3Framebuffer"
GPU_VENDOR_RE = re.compile(r"GetSystemGPUVendors_block_invoke: (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+|\d+) (\S+)")
GPU_VENDOR_NAMES = {1: "unknown", 2: "Intel (IG)", 4: "AMD", 8: "NVIDIA (NVDA)", 0x10: "AppleParavirt"}


def _vendor_int(v):
    """a vendor as CoreDisplay logs it: hex with 0x, else decimal (int(v, 0) would crash on "04")"""
    return int(v, 16) if v[:2].lower() == "0x" else int(v)


def vendor_name(v):
    """the name of a vendor bit, or of a combined mask ("AMD + Intel (IG)")"""
    if v in GPU_VENDOR_NAMES:
        return GPU_VENDOR_NAMES[v]
    names = [GPU_VENDOR_NAMES[b] for b in (1, 2, 4, 8, 0x10) if v & b]
    return " + ".join(names) if names else "?"


def gpu_vendors(cd):
    """{entry id: (vendor, class)} of the GetSystemGPUVendors lines of a CoreDisplay log (the last per id)"""
    out = {}
    for eid, v, cls in GPU_VENDOR_RE.findall(cd or ""):
        out[eid.lower()] = (_vendor_int(v), cls)
    return out


# fb/rdna3-fb.cpp start(): "RDNA3-fb: head 0 class <class>, name <name>: CoreDisplay classifies ... -- NOT <expected>: ..."
FB_CLASS_LOG_RE = re.compile(r"RDNA3-fb: head (\d+) class (\S+), name (\S+): .*? -- NOT (\w+):")


def fb_class_rows(klog):
    """[(level, label, text)]: a FAIL per framebuffer head the kext itself logged as not its leaf class (a stale
    or wrong fb/Info.plist: the class CoreDisplay reads is then not AMDRDNA3Framebuffer); nothing when it did not"""
    label = "fb class"
    out = []
    for head, cls, name, want in FB_CLASS_LOG_RE.findall(klog or ""):
        out.append(("FAIL", label, "the kext logged head %s as class %s (service %s), not %s: Info.plist IOClass of "
                    "RDNA3Framebuffer.kext is not the leaf class (stale or wrong bundle), CoreDisplay classes it "
                    "vendor 1 and keeps IOPresentment off (DRIVER-PLAN N44)" % (head, cls, name, want)))
    return out


# common/rdna3_tcall.h (card run 2026-10-25b, DRIVER-PLAN N47; ECC review round 1): a thread call's stop that
# waited long (it never gives up), ran from inside the call's own run, a free refused there, an enter refused during
# the stop, a failed allocation; the names are the kexts' ("fb.vbl", "accel.iop_async", "core.hotplugTick")
TCALL_LOG_RE = re.compile(r"RDNA3-tcall: (\S+): (the stop has waited (\d+) ms for (\d+) run\(s\) still outstanding"
                          r"|drained after (\d+) ms|stopped from inside its own run"
                          r"|FAIL: freed from inside its own run|thread_call_free refused (\d+) times"
                          r"|an enter during its stop was refused|thread call allocation failed \(([^)]*)\))")


def tcall_rows(klog):
    """[(level, label, text)] per thread call the kernel log (archive + serial: kernel_text) names: FAIL for a
    failed allocation, a free refused inside its own run, or a stop still waiting (30 s or more) when the log ends;
    WARN for a slow stop that drained, a stop from inside its own run, thread_call_free refusing; INFO for enters
    refused during a stop and when there is no kernel log at all. The counts are log lines (rate limited), so
    "at least"."""
    label = "thread call stop"
    if not (klog or "").strip():
        return [("INFO", label, "no kernel log (20-log-rdna3.txt, serial log): thread call stops not known")]
    seen = {}
    for m in TCALL_LOG_RE.finditer(klog):
        name, what = m.group(1), m.group(2)
        e = seen.setdefault(name, {"waits": 0, "max_ms": 0, "max_out": 0, "pending": False, "drained": [],
                                   "self": 0, "selffree": 0, "free": 0, "refused": 0, "alloc": []})
        if what.startswith("the stop has waited"):
            e["waits"] += 1
            e["max_ms"] = max(e["max_ms"], int(m.group(3)))
            e["max_out"] = max(e["max_out"], int(m.group(4)))
            e["pending"] = True
        elif what.startswith("drained after"):
            e["drained"].append(int(m.group(5)))
            e["pending"] = False
        elif what.startswith("stopped from inside"):
            e["self"] += 1
        elif what.startswith("FAIL: freed"):
            e["selffree"] += 1
        elif what.startswith("thread_call_free refused"):
            e["free"] = max(e["free"], int(m.group(6)))
        elif what.startswith("an enter during"):
            e["refused"] += 1
        else:
            e["alloc"].append(m.group(7))
    out = []
    for name in sorted(seen):
        e = seen[name]
        for why in e["alloc"]:
            out.append(("FAIL", label, "%s: thread call allocation failed (%s): the work it runs never happens"
                        % (name, why)))
        if e["selffree"]:
            out.append(("FAIL", label, "%s: freed from inside its own run at least %d time(s): refused, left "
                        "allocated (a driver bug in the stop path)" % (name, e["selffree"])))
        if e["waits"] and e["pending"] and e["max_ms"] >= 30000:
            out.append(("FAIL", label, "%s: a stop still waiting %d s for %d run(s) when the log ends (at least %d "
                        "log line(s)): a run of it hangs (its function, or a lock the stopping thread holds)"
                        % (name, e["max_ms"] // 1000, e["max_out"], e["waits"])))
        elif e["waits"]:
            out.append(("WARN", label, "%s: the stop waited at least %.1f s for %d run(s) (at least %d log line(s)), "
                        "drained after %s ms: a slow run of its function at the stop" % (
                            name, e["max_ms"] / 1000.0, e["max_out"], e["waits"],
                            "/".join(str(x) for x in e["drained"]) if e["drained"] else "? (no drained line)")))
        if e["self"]:
            out.append(("WARN", label, "%s: stopped at least %d time(s) from inside its own run (not waited for: "
                        "thread_call_cancel_wait would have panicked there)" % (name, e["self"])))
        if e["free"]:
            out.append(("WARN", label, "%s: thread_call_free refused at least %d times at its free (a queued run "
                        "or a ONCE reschedule kept coming back)" % (name, e["free"])))
        if e["refused"]:
            out.append(("INFO", label, "%s: at least %d enter(s) refused during its stop (a kick racing the stop; "
                        "no run queued)" % (name, e["refused"])))
    return out



# kext/src/RDNA3Drm.cpp, DRIVER-PLAN N47 (b). The kext of 2026-10-25 (before the BO-mapping lifetime fix) logged
# RDNA3BoMemory::doMap / doUnmap if a newer xnu ever called them on its sub-range descriptor: a client's map of a
# VRAM or doorbell BO failed (doMap) or left its range mapped (doUnmap). Since the fix the descriptor maps itself,
# and the kext logs a map or unmap xnu refused, and every map request it refused (common/rdna3_bo_map.h)
BOMEM_LOG_RE = re.compile(r"RDNA3: GEM: RDNA3BoMemory::(doMap|doUnmap) called \((\d+) such\)")
BOMAP_FAIL_RE = re.compile(r"RDNA3: GEM: CPU (map|unmap) of a (\d+) KiB (\w+) BO at (0x[0-9a-f]+) failed in xnu "
                           r"\((0x[0-9a-f]+)\) \(pid (-?\d+), (\d+) such\)")
BOMAP_NODESC_RE = re.compile(r"RDNA3: GEM: CPU map of offset (0x[0-9a-f]+): no descriptor for the (\d+) KiB (\w+) BO "
                             r"at (0x[0-9a-f]+): (.*?) \(physical (0x[0-9a-f]+), segment (\d+) KiB; pid (-?\d+), "
                             r"(\d+) such\)")
BOMAP_LATE_RE = re.compile(r"RDNA3: GEM: a CPU mapping of a (\d+) KiB BO at (0x[0-9a-f]+) went after the DRM "
                           r"teardown.*?\((\d+) such\)")
BOMAP_QUOTA_RE = re.compile(r"RDNA3: GEM: CPU map of offset (0x[0-9a-f]+) refused by the quota for the first time: "
                            r"(.*?) \(uid (\d+), BO (\d+) KiB; pid (-?\d+), (\d+) quota refusals\)")
BOMAP_REFUSED_RE = re.compile(r"RDNA3: GEM: CPU map of offset (0x[0-9a-f]+) refused: (.*?) \((?:BO [^)]*; )?pid "
                              r"(-?\d+), (\d+) such\)")


def bomem_rows(klog):
    """[(level, label, text)] of the kernel log's BO mapping lines: FAIL per doMap / doUnmap call of the old kext
    and per kind of map xnu refused; WARN for the map requests the kext refused (offset 0, which names no BO: an
    unmap from a libdrm_xnu before N47; the rest: a client asking for what amdgpu never maps). The counts are the
    highest the log prints, so "at least"; nothing when there is none."""
    seen = {}
    for m in BOMEM_LOG_RE.finditer(klog or ""):
        seen[m.group(1)] = max(seen.get(m.group(1), 0), int(m.group(2)))
    rows = [("FAIL", "BO map", "RDNA3BoMemory::%s called at least %d time(s): this macOS maps a sub-range "
             "descriptor other than through its parent (xnu-12377 never does); the call answered "
             "kIOReturnUnsupported (DRIVER-PLAN N47 (b))" % (k, seen[k])) for k in sorted(seen)]
    fails = {}
    for m in BOMAP_FAIL_RE.finditer(klog or ""):
        k = (m.group(1), m.group(3))
        n = int(m.group(7))
        if k not in fails or n > fails[k][0]:
            fails[k] = (n, m.group(2), m.group(4), m.group(5), m.group(6))
    for (what, kind), (n, kib, at, err, pid) in sorted(fails.items()):
        rows.append(("FAIL", "BO map", "xnu refused a CPU %s of a %s BO at least %d time(s) (the last logged: %s KiB "
                     "at %s, error %s, pid %s): %s" % (what, kind, n, kib, at, err, pid,
                                                     "the client got no pointer to it" if what == "map" else
                                                     "the range may still be mapped in that task")))
    nobo0, nobo, refused = 0, 0, {}
    for m in BOMAP_REFUSED_RE.finditer(klog or ""):
        n = int(m.group(4))
        if m.group(2).startswith("no BO") and int(m.group(1), 16) == 0:
            nobo0 = max(nobo0, n)
        elif m.group(2).startswith("no BO"):
            nobo = max(nobo, n)
        else:
            refused[m.group(2)] = max(refused.get(m.group(2), 0), n)
    if nobo0:
        rows.append(("WARN", "BO map", "map / unmap requests for offset 0 (at least %d of the 'no BO' counter): a "
                     "libdrm_xnu before DRIVER-PLAN N47 unmaps with memory type 0, so its mappings, and the BOs "
                     "they hold, stay until the process closes the device - install the userspace and the Metal "
                     "plugin of the same build as the kext (CI full: true)" % nobo0))
    if nobo:
        rows.append(("WARN", "BO map", "map / unmap requests for offsets that name no BO of the file (at least %d of the 'no "
                     "BO' counter): an unmap after GEM_CLOSE (amdgpu's order is unmap, then close) or a stale "
                     "offset; such a mapping holds its BO until the process closes the device" % nobo))
    for why in sorted(refused):
        rows.append(("WARN", "BO map", "a CPU map refused: %s (at least %d time(s); the counter covers every "
                     "reason)" % (why, refused[why])))
    for m in BOMAP_QUOTA_RE.finditer(klog or ""):
        rows.append(("WARN", "BO map", "the quota of mapped BOs refused uid %s for the first time: %s (a %s KiB BO, "
                     "pid %s): a client mapping BOs and dropping their handles without unmapping"
                     % (m.group(3), m.group(2), m.group(4), m.group(5))))
    nodesc = {}
    for m in BOMAP_NODESC_RE.finditer(klog or ""):
        n = int(m.group(9))
        if m.group(5) not in nodesc or n > nodesc[m.group(5)][0]:
            nodesc[m.group(5)] = (n, m.group(2), m.group(3), m.group(4), m.group(6), m.group(7))
    for why, (n, kib, kind, at, phys, seg) in sorted(nodesc.items()):
        rows.append(("FAIL", "BO map", "no mapping descriptor for a %s BO, at least %d time(s): %s (the last logged: "
                     "%s KiB at %s, physical %s, segment %s KiB): the client's map failed" % (kind, n, why, kib, at,
                                                                                            phys, seg)))
    late = 0
    for m in BOMAP_LATE_RE.finditer(klog or ""):
        late = max(late, int(m.group(3)))
    if late:
        rows.append(("WARN", "BO map", "at least %d CPU mapping(s) went after the DRM teardown (the kext stopped "
                     "with mappings alive): their BOs were left allocated" % late))
    return rows


# debug.rdna3_vram's bomap_* counters (DRIVER-PLAN N47 (b)): the per-BO mapping descriptors, each holding its BO
BOMAP_KEYS = ("bomap_live", "bomap_live_kib", "bomap_made", "bomap_reused", "bomap_refused", "bomap_quota",
              "bomap_nodesc", "bomap_nobo", "bomap_fail")


def bomap_counter_lines(c):
    """[(level, text)] from the counters of one debug.rdna3_vram dump (vram_heap_counters)"""
    if c is None:
        return []
    if any(k not in c for k in BOMAP_KEYS):
        return [("WARN", "not measured: debug.rdna3_vram has no bomap_* counters (a kext before DRIVER-PLAN N47's "
                         "BO-mapping fix, or the dump was cut): whether CPU mappings keep BOs alive is not known")]
    out = [("INFO", "CPU mappings of VRAM / doorbell BOs: %d descriptor(s) live holding %d KiB of BOs (%d made, "
                    "%d reused by a second map or an unmap)" % (c["bomap_live"], c["bomap_live_kib"],
                                                                c["bomap_made"], c["bomap_reused"]))]
    if c["bomap_fail"]:
        out.append(("FAIL", "%d CPU map / unmap(s) of VRAM or doorbell BOs failed in xnu: the kernel log's 'failed "
                            "in xnu' lines" % c["bomap_fail"]))
    if c["bomap_nobo"]:
        out.append(("WARN", "%d map / unmap request(s) named no BO of the file (an unmap after GEM_CLOSE, or a "
                            "libdrm_xnu before N47 unmapping with memory type 0: those mappings and their BOs stay "
                            "until the device is closed); the kernel log's 'CPU map of offset' lines"
                    % c["bomap_nobo"]))
    if c["bomap_nodesc"]:
        out.append(("FAIL", "%d CPU map(s) of VRAM / doorbell BOs found no descriptor (no physical range in the BAR, "
                            "or no memory): the kernel log's 'no descriptor for the' lines" % c["bomap_nodesc"]))
    if c["bomap_refused"]:
        out.append(("WARN", "%d CPU map(s) refused (userptr, no CPU access, outside the BAR%s): the kernel log's "
                            "'CPU map of offset ... refused' lines"
                    % (c["bomap_refused"], ", and %d by the uid's quota of mapped BOs (a client mapping and closing "
                                           "BOs in a loop, or never unmapping)" % c["bomap_quota"]
                       if c["bomap_quota"] else "")))
    return out

CD_PREFS_SEEN_RE = re.compile(r"^== CoreDisplay prefs \(.*?\): this boot's WindowServer started with: (.*)$", re.M)
CD_PREFS_DID_RE = re.compile(r"^== CoreDisplay prefs: (.*)$", re.M)


def cd_pref(text, key):
    """the value of key in a `defaults read` domain flattened to one line, or None (raw: 1, 0, true, ...)"""
    m = re.search(r'(?:^|[{;\s])"?%s"?\s*=\s*"?(\w+)"?\s*;' % re.escape(key), text or "")
    return m.group(1) if m else None


CD_PREF_BOOL = {"1": "1", "true": "1", "yes": "1", "0": "0", "false": "0", "no": "0"}


def iop_prefs_off(args):
    """rdna3iopprefs=0 among the boot-args (as a whole token)"""
    return re.search(r"(^|\s)rdna3iopprefs=0(\s|$)", args or "") is not None


# autoreport.sh's state of the two stores on one line: "AnyHost: <domain>; ByHost(<uuid>): <domain>"
# ("unknown" for the uuid and "(unknown)" for the domain when ioreg gave no IOPlatformUUID). An older
# autoreport.sh printed the AnyHost domain alone.
CD_PREFS_STORES_RE = re.compile(r"^AnyHost: (.*?); ByHost\(([^)]*)\): (.*)$")


def cd_prefs_stores(st):
    """(AnyHost domain, uuid, ByHost domain) of a state line; (st, None, None) for the older single-store line"""
    m = CD_PREFS_STORES_RE.match(st)
    return (m.group(1), m.group(2), m.group(3)) if m else (st, None, None)


def _cd_switches(domain):
    """(useIOP_onAMD, useIOP) of one flattened `defaults read` domain, "1"/"0"/raw/None (not set)"""
    return tuple(CD_PREF_BOOL.get((cd_pref(domain, k) or "").lower(), cd_pref(domain, k))
                 for k in ("useIOP_onAMD", "useIOP"))


def cd_prefs_rows(txt, args="", prefs_member=""):
    """[(level, label, text)], useIOP_onAMD: CoreDisplay's IOPresentment switches as this boot's
    WindowServer started with them (autoreport.sh's tests-file lines, else 19-coredisplay-prefs.txt;
    DRIVER-PLAN N44); useIOP_onAMD is "1"/"0"/None (not set: CoreDisplay's static 0)/False (unknown:
    no line, the boot's start state could not be kept, or the boot time was unknown). Two stores are
    reported separately: AnyHost (/Library/Preferences/com.apple.CoreDisplay.plist) and ByHost
    (/Library/Preferences/ByHost/com.apple.CoreDisplay.<IOPlatformUUID>.plist, the store
    CFPreferencesCopyValue(AnyUser, CurrentHost) reads: found for CoreDisplay on macOS 26 by
    Navi48-MacOS, Almosst-DEV, MIT; unproven on macOS 15). useIOP_onAMD is "1" when either holds it."""
    label = "CoreDisplay prefs"
    seen = CD_PREFS_SEEN_RE.findall(txt or "")
    did = CD_PREFS_DID_RE.findall(txt or "")
    stale = any("could NOT keep" in d or "could NOT tell" in d for d in did)
    from_member = False
    if not seen and prefs_member:
        m = re.search(r"^== at the start of the boot.*\n(?:\$ .*\n)?boot (\S+)\n(.*)$", prefs_member, re.M)
        if m:
            g = m.group(2).strip()
            # an empty domain leaves no second line: collect.sh's "[exit 0]" follows the "boot <sec>" line
            seen = ["(none)" if not g or g.startswith("[exit") else g]
            from_member = True
            stale = stale or m.group(1) == "?"
    if not seen:
        return [("WARN", label, "no '== CoreDisplay prefs' line in the tests file and no 19-coredisplay-prefs.txt: "
                 "useIOP / useIOP_onAMD of this boot are not known (autoreport.sh older than DRIVER-PLAN N44?)")], False
    st = seen[-1].strip() or "(none)"
    out = []
    if st == "(unknown)" or stale:
        out.append(("WARN", label, "what this boot's WindowServer started with is NOT known (%s): the boot's start state "
                    "could not be kept or told from an earlier boot's; useIOP_onAMD at WindowServer's start is unknown" % st))
    st_any, uuid, st_bh = cd_prefs_stores(st)
    amd_any, iop_any = _cd_switches(st_any)
    bh_known = st_bh is not None and st_bh != "(unknown)"
    amd_bh, iop_bh = _cd_switches(st_bh) if bh_known else (None, None)
    amd = "1" if "1" in (amd_any, amd_bh) else ("0" if "0" in (amd_any, amd_bh) else None)
    off = iop_prefs_off(args)
    if not out:
        def unset(v, static):
            return v if v is not None else "unset (%s)" % static
        if st_bh is None:
            # the older autoreport.sh line (the AnyHost domain alone): the text of that time
            what = "useIOP_onAMD %s, useIOP %s (%s)" % (
                unset(amd_any, "CoreDisplay's static 0"), unset(iop_any, "static 1"), st)
        else:
            what = "AnyHost: useIOP_onAMD %s, useIOP %s; ByHost(%s): %s (%s)" % (
                unset(amd_any, "CoreDisplay's static 0"), unset(iop_any, "static 1"), uuid,
                "useIOP_onAMD %s, useIOP %s" % (unset(amd_bh, "static 0"), unset(iop_bh, "static 1")) if bh_known else
                "not known (ioreg gave no IOPlatformUUID)", st)
        text = "this boot's WindowServer started with " + what
        if amd != "1":
            text += "; " + ("rdna3iopprefs=0: the A/B without them" if off else
                            "the switch counts from the next boot (WindowServer started before autoreport wrote it)")
        out.append(("INFO", label, text))
        if amd_any == "1" and bh_known and amd_bh != "1":
            out.append(("WARN", label, "useIOP_onAMD is in the AnyHost store only, not in ByHost(%s): CoreDisplay may read "
                        "the ByHost store (CFPreferencesCopyValue AnyUser + CurrentHost: Navi48-MacOS evidence on macOS 26, "
                        "unproven on 15), so this boot's IOPresentment switch may have been off (%s)" % (uuid, st)))
        if off and amd == "1":
            # the keys of an earlier boot were still there: rdna3vm's install step returns early when the bundle is
            # current, so rdna3iopprefs=0 reached no install boot; autoreport.sh takes them out only after
            # WindowServer started
            out.append(("WARN", label, "rdna3iopprefs=0 but WindowServer started with useIOP_onAMD 1 (%s): the keys are "
                        "from an earlier boot and no boot took them out in between: this run is NOT the A/B without "
                        "them (autoreport.sh takes them out after WindowServer started: the next boot is)" % st))
    if from_member and st_bh == "(unknown)":
        # the tests file's "could NOT be written (host UUID unknown ...)" line is not there to say it: the ByHost
        # store of this boot is unknown (ioreg gave no IOPlatformUUID), so a missing ByHost write must not be silent
        out.append(("WARN", label, "the ByHost store is NOT known (%s): ioreg gave no IOPlatformUUID, so autoreport.sh "
                    "could not write or read /Library/Preferences/ByHost/com.apple.CoreDisplay.<IOPlatformUUID>" % st))
    for d in did:
        if "could NOT" in d:
            out.append(("WARN", label, "autoreport.sh: " + d))
    return out, (False if (st == "(unknown)" or stale) else amd)


# CoreDisplay's / IOPresentment's own words on the display-pipe capabilities (display_pipe_guard.h and
# tools/pc/agdc-probe.c of Navi48-MacOS, Almosst-DEV, MIT: macOS 26; the strings are CoreDisplay's, not the
# driver's, and are expected to be the same on 15): each failure ends the IOPresentment path before Submit
IOP_LOG_FAILS = (
    ("Invalid end point size", "the capabilities' end point (HorizontalActive / VerticalActive, from AGDC "
     "getLinkConfig) is zero: every IOPTransaction dies before Submit"),
    ("capabilities with no devices", "the capabilities' DeviceArray is empty (AGDC answered 0 devices): "
     "LoadCapabilities returns FALSE"),
    ("device with no planes", "the device's PlaneArray is empty (AGDC answered 0 planes): LoadCapabilities returns FALSE"),
    ("Failed to get capabilities", "IOPresentment capabilities could not be read from the AGDC: LoadCapabilities returns FALSE"),
)
IOP_LOG_OK = "AGDC IB successful"
IOP_LOG_QUEUE = "QueueDepth"


def iop_log_rows(cd):
    """[(level, label, text)]: what CoreDisplay / IOPresentment logged about the display pipe's capabilities in
    the WindowServer log (24-log-coredisplay.txt, DRIVER-PLAN N44): FAIL per failure string, PASS for 'AGDC IB
    successful' (the inline buffer was built), INFO whether 'QueueDepth' appears (a key present only when the
    capabilities came from the display-pipe class, not the base IOPresentmentCapabilities)"""
    label = "IOPresentment log"
    # collect.sh wraps a log in "$ command" / "[exit N]": what is left is the log
    lines = [l for l in (cd or "").splitlines() if l.strip() and not l.startswith(("$ ", "[exit"))]
    if not lines:
        return [("WARN", label, "24-log-coredisplay.txt is missing or empty: whether IOPresentment accepted the display "
                 "pipe's capabilities is not known (DRIVER-PLAN N44)")]
    out = []
    ok = sum(1 for l in lines if IOP_LOG_OK in l)
    qd = sum(1 for l in lines if IOP_LOG_QUEUE in l)
    failed = False
    for needle, meaning in IOP_LOG_FAILS:
        hits = [l for l in lines if needle in l]
        if hits:
            failed = True
            # a failure and later a success: the first attempt failed, the capabilities were read in the end (WARN)
            out.append(("WARN" if ok else "FAIL", label, "'%s' %d time(s) in the CoreDisplay log%s: %s (first: %s)" % (
                needle, len(hits), " (and '%s' too)" % IOP_LOG_OK if ok else "", meaning, hits[0].strip()[:200])))
    if ok:
        out.append(("PASS", label, "'%s' %d time(s): IOPresentment built the inline buffer from the AGDC's answer" % (IOP_LOG_OK, ok)))
    elif not failed:
        out.append(("INFO", label, "no '%s' and no capabilities failure in the %d-line CoreDisplay log: IOPresentment "
                    "did not get as far as the capabilities (or does not log it)" % (IOP_LOG_OK, len(lines))))
    out.append(("INFO", label, "'%s' %s in the CoreDisplay log%s" % (
        IOP_LOG_QUEUE, "appears %d time(s)" % qd if qd else "does not appear",
        ": the capabilities came from the display-pipe class" if qd else
        " (the key exists only when the capabilities came from the display-pipe class; the base class leaves it out, "
        "or the log does not show it)")))
    return out


def gpu_vendor_rows(cd, planes=False, iop_amd=False, prefs_off=False, klog=""):
    """[(level, label, text)]: the GPU vendor CoreDisplay gave each framebuffer (DRIVER-PLAN N44), and the
    kext's own word on its class (klog, 20-log-rdna3.txt). planes: planes arrive in IOPresentment transactions
    (True / False; None = the accelerator's counters are not in the report); iop_amd: useIOP_onAMD as this
    boot's WindowServer started with it (cd_prefs_rows); prefs_off: rdna3iopprefs=0 is set"""
    return fb_class_rows(klog) + _gpu_vendor_cd_rows(cd, planes, iop_amd, prefs_off)


def _gpu_vendor_cd_rows(cd, planes, iop_amd, prefs_off):
    label = "GPU vendor"
    # collect.sh wraps a log in "$ command" / "[exit N]": what is left is the log
    if not [l for l in (cd or "").splitlines() if l.strip() and not l.startswith(("$ ", "[exit"))]:
        return [("WARN", label, "24-log-coredisplay.txt is missing or empty: the framebuffers' GPU vendor "
                 "(CoreDisplay's IOPresentment gate, DRIVER-PLAN N44) is not known")]
    seen = gpu_vendors(cd)
    if not seen:
        # card run 2026-10-25c (macOS 26.4.1): the line is Info level and ages out of the log buffer; planes in
        # IOPresentment transactions (debug.rdna3accel_iop_ok > 0) prove the gate was open, so it is no warning
        if planes is True:
            return [("INFO", label, "no 'GetSystemGPUVendors_block_invoke' line in the CoreDisplay log (Info level, "
                     "aged out of the log buffer?), but planes arrive in IOPresentment transactions "
                     "(debug.rdna3accel_iop_ok > 0): the gate was open")]
        return [("WARN", label, "no 'GetSystemGPUVendors_block_invoke' line in the CoreDisplay log: the framebuffers' "
                 "GPU vendor (CoreDisplay's IOPresentment gate, DRIVER-PLAN N44) is not known")]
    out = []
    for eid, (v, cls) in sorted(seen.items()):
        what = "framebuffer %s class %s: vendor %d (%s)" % (eid, cls, v, vendor_name(v))
        if v == 4:
            out.append(("PASS", label, what + ": CoreDisplay counts it as AMD, IOPresentment allowed"))
        elif v == 1 and cls == FB_CLASS_OLD:
            out.append(("INFO", label, "IOPresentment off: framebuffer classed vendor 1 (unknown): %s, a framebuffer "
                        "older than DRIVER-PLAN N44 (no AMD in its class name)" % what))
        elif v == 1:
            out.append(("FAIL", label, "IOPresentment off: framebuffer classed vendor 1 (unknown): %s; %s" % (
                what, "CoreDisplay's AMD string is not in this class name: change RDNA3_FB_CLASS "
                "(fb/rdna3-fb.cpp, fb/Info.plist) to what the probe names (DRIVER-PLAN N44)" if cls == FB_CLASS else
                "not a class of this driver (%s, %s): another driver's framebuffer keeps IOPresentment off for the "
                "whole WindowServer session" % (FB_CLASS, FB_CLASS_OLD))))
        elif v & 4:
            out.append(("WARN", label, what + ": a combined mask, not AMD alone: %s (DRIVER-PLAN N44)" % (
                "IOPresentment is allowed only if useIOP_onIG is set too (the Intel part of the mask)" if v & 2 else
                "the other vendor's switch decides too")))
        else:
            out.append(("WARN", label, what + ": not AMD (DRIVER-PLAN N44)"))
    if all(v == 4 for v, _ in seen.values()) and planes is not True:
        if planes is None:
            why = ("the accelerator's counters (debug.rdna3accel_iop_ok) are not in the report: whether planes arrive "
                   "is not known")
        else:
            why = "no planes arrive in IOPresentment transactions: " + (
                "useIOP_onAMD was 1 at WindowServer's start although rdna3iopprefs=0 is set (keys of an earlier "
                "boot): this run is not the A/B without them" if iop_amd == "1" and prefs_off else
                "useIOP_onAMD was 1 in /Library/Preferences at WindowServer's start: next lead the "
                "_windowserver user's com.apple.CoreDisplay domain" if iop_amd == "1" else
                "useIOP_onAMD at WindowServer's start is unknown (CoreDisplay prefs row): check it first"
                if iop_amd is False else
                "next lead CoreDisplay's useIOP_onAMD default (not 1 at WindowServer's start)")
        out.append(("INFO", label, "every framebuffer is vendor 4 (AMD) but %s (DRIVER-PLAN N44)" % why))
    return out


# DRIVER-PLAN N46 (1): WindowServer pairs a display with its accelerator at display init through
# IOAccelFindAccelerator(fb), which reads IOAccelTypes (the registry path of an IOAccelerator),
# IOAccelIndex (SInt32) and IOAccelRevision (2) from the IOFramebuffer (the family's
# IOAccelDisplayPipe::init stamps them). collect.sh's pairing_report() prints, per framebuffer
# instance of the class (RDNA3Framebuffer matches the leaf AMDRDNA3Framebuffer and the older class),
# the keys that are set, then the registry path of every RDNA3Accelerator:
#   +-o RDNA3Framebuffer  <class AMDRDNA3Framebuffer, id 0x..., ...>
#   "RDNA3,Connected" = Yes / "IOAccelTypes" = "..." / "IOAccelIndex" = 0 / "IOAccelRevision" = 2
#   accelerator: IOService:/...
PAIRING_MEMBER = "16-ioaccel-pairing.txt"
PAIRING_ACCEL = "RDNA3Accelerator"   # the class (and service name) the pairing has to name
PAIRING_REVISION = "2"


def _pairing_blocks(txt):
    """[{name, class, id, connected, types, index, revision}] of a pairing file (None = key not printed)"""
    out = []
    for blk in re.split(r"(?m)^\+-o ", txt)[1:]:
        head_line = blk.split("\n", 1)[0]
        m = re.match(r"(\S+)\s+<class (\w+), id (0x[0-9a-fA-F]+)", head_line)
        g = lambda k: (re.search(r'^"%s" = (.*?)\s*$' % re.escape(k), blk, re.M) or [None, None])[1]
        out.append({"name": m.group(1) if m else head_line.split()[0] if head_line.split() else "?",
                    "class": m.group(2) if m else "?", "id": m.group(3) if m else "?", "connected": g("RDNA3,Connected"),
                    "types": g("IOAccelTypes"), "index": g("IOAccelIndex"), "revision": g("IOAccelRevision")})
    return out


def _pairing_comps(path):
    p = path.strip().strip('"')
    if p.startswith("IOService:"):
        p = p[len("IOService:"):]
    return [c for c in p.split("/") if c]


def _pairing_same_path(accel, types):
    """the same registry path, tolerating one extra leading component on either side: the kernel's getPath
    (IOAccelTypes) leaves out the platform expert device (named after the Mac model, e.g. MacBookPro16,1) that
    `ioreg -p IOService` shows below the root (card run 2026-10-25c, macOS 26.4.1: IOAccelTypes
    IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/RDNA3Accelerator against
    IOService:/MacBookPro16,1/AppleACPIPlatformExpert/PCI0/...)"""
    a, t = _pairing_comps(accel), _pairing_comps(types)
    if not a or not t:
        return False
    return a == t or (len(a) == len(t) + 1 and a[1:] == t) or (len(t) == len(a) + 1 and t[1:] == a)


def pairing_rows(txt):
    """[(level, label, text)]: does each lit head's framebuffer name our accelerator the way
    IOAccelFindAccelerator reads it? PASS: IOAccelTypes names RDNA3Accelerator, IOAccelRevision 2 and an
    IOAccelIndex is set; WARN: a key missing, another accelerator named, another revision, no framebuffer or
    no lit head to judge; INFO: no 16-ioaccel-pairing.txt in the report (an older collect.sh)"""
    label = "display pairing"
    lines = [l for l in (txt or "").splitlines() if l.strip() and not l.startswith(("$ ", "[exit"))]
    if not lines:
        return [("INFO", label, "no %s in the report (a collect.sh older than DRIVER-PLAN N46): the IOAccel keys "
                 "WindowServer pairs a display with its accelerator by are not known" % PAIRING_MEMBER)]
    body = "\n".join(lines)
    fbs = _pairing_blocks(body)
    if not fbs:
        return [("WARN", label, "%s has no framebuffer instance (ioreg -c RDNA3Framebuffer printed none): no display "
                 "can be paired with %s" % (PAIRING_MEMBER, PAIRING_ACCEL))]
    accels = re.findall(r"^accelerator: (\S+)", body, re.M)
    # a head the core marks disconnected has no display to pair; no RDNA3,Connected key = judge it
    lit = [f for f in fbs if (f["connected"] or "").lower() != "no"]
    if not lit:
        return [("INFO", label, "%d framebuffer(s), none connected (RDNA3,Connected = No): no display to pair" % len(fbs))]
    out = []
    for f in lit:
        who = "framebuffer id %s (%s, class %s%s)" % (f["id"], "IOAccelIndex %s" % f["index"] if f["index"] is not None
                                                      else "no IOAccelIndex", f["class"],
                                                      ", older than DRIVER-PLAN N44" if f["class"] == FB_CLASS_OLD else "")
        have = "IOAccelTypes %s, IOAccelIndex %s, IOAccelRevision %s" % (
            (f["types"] or "not set")[:200], f["index"] if f["index"] is not None else "not set",
            f["revision"] if f["revision"] is not None else "not set")
        why = []
        matched = None
        if f["types"] is None:
            why.append("IOAccelTypes is not set (IOAccelFindAccelerator finds no accelerator for this display)")
        elif accels:
            # IOAccelTypes is the registry path of an accelerator (or, bare, its class name): it has to be one
            # of the accelerators this report registered, not just any string containing the class name
            matched = next((a for a in accels if a in f["types"] or _pairing_same_path(a, f["types"])), None)
            if matched is None and f["types"].strip().strip('"') == PAIRING_ACCEL:
                matched = PAIRING_ACCEL
            if matched is None:
                why.append("IOAccelTypes names %s" % (
                    "a %s that is not a registered one" % PAIRING_ACCEL if PAIRING_ACCEL in f["types"] else
                    "something else than %s" % PAIRING_ACCEL))
        elif PAIRING_ACCEL not in f["types"]:
            why.append("IOAccelTypes names something else than %s" % PAIRING_ACCEL)
        if f["revision"] is None:
            why.append("IOAccelRevision is not set")
        elif f["revision"] != PAIRING_REVISION:
            why.append("IOAccelRevision is %s, not %s" % (f["revision"], PAIRING_REVISION))
        if f["index"] is None:
            why.append("IOAccelIndex is not set")
        if why:
            out.append(("WARN", label, "%s: %s; %s; registered accelerator(s): %s (DRIVER-PLAN N46)" % (
                who, "; ".join(why), have, ", ".join(accels) or "none printed")))
        else:
            out.append(("PASS", label, "%s: %s: WindowServer can pair the display with the accelerator%s" % (
                who, have, " (registered accelerator %s)" % matched if matched else "")))
    return out


# Crash reports of the boot (card run 2026-10-25c: Chess crashed three times on macOS 26.4.1 and nothing was
# collected). collect.sh's crash_reports() lists every *.ips newer than the boot in /Library/Logs/DiagnosticReports
# and /Users/*/Library/Logs/DiagnosticReports (40-app-crashes.txt, the files in 40-crash-ips/), one block each:
#   == crash: <path> / mtime: .. / size: .. / copy: 40-crash-ips/.. / process: Chess / pid: 412 /
#   exception: EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x.. / termination: {..} /
#   crashed thread: 0 (com.apple.main-thread) / frame 0: libobjc.A.dylib  objc_msgSend + 32 ... / crashes: N
CRASH_MEMBER = "40-app-crashes.txt"


def _crash_blocks(txt):
    out = []
    for blk in re.split(r"(?m)^== crash: ", txt)[1:]:
        g = lambda k: (re.search(r"^%s: (.*?)\s*$" % re.escape(k), blk, re.M) or [None, None])[1]
        # the grep fallback prints "key":value pairs instead of the summary
        fb = lambda k: (re.search(r'"%s" *: *"?([^",}\n]*)' % k, blk) or [None, None])[1]
        exc = g("exception")
        if exc is None:
            m = re.search(r'"exception" *: *\{[^}]*"type" *: *"([^"]*)"(?:[^}]*"signal" *: *"([^"]*)")?', blk)
            exc = (m.group(1) + (" (%s)" % m.group(2) if m.group(2) else "")) if m else None
        term = g("termination") or ""
        ind = re.search(r'"indicator" *: *"([^"]*)"', term + blk)
        out.append({"path": blk.split("\n", 1)[0].strip(), "process": g("process") or fb("procName") or fb("app_name") or "?",
                    "pid": g("pid") or fb("pid") or "?", "exception": exc, "indicator": ind.group(1) if ind else None,
                    "frame": g("frame 0"), "thread": g("crashed thread"), "copy": g("copy"),
                    "unreadable": g("unreadable")})
    return out


def crash_rows(txt, nocard=False):
    """[(level, label, text)] "app crashes": WARN per crashed process (exception + top frame of the crashed thread +
    how many reports), INFO when the boot has none. INFO: no 40-app-crashes.txt (a collect.sh older than card run
    2026-10-25c); WARN: the listing did not finish (no 'crashes: N' line) or a report is unreadable.
    nocard: a boot without the card (no_card_boot): every row INFO"""
    label = "app crashes"
    lines = [l for l in (txt or "").splitlines() if l.strip() and not l.startswith(("$ ", "[exit"))]
    if not lines:
        return [("INFO", label, "no %s in the report (a collect.sh older than card run 2026-10-25c): user app crashes "
                 "are not known" % CRASH_MEMBER)]
    lv = (lambda x: "INFO") if nocard else (lambda x: x)
    tail = "" if not nocard else " (no-card boot: not about the driver)"
    done = re.search(r"^crashes: (\d+)\s*$", txt, re.M)
    if not done:
        return [(lv("WARN"), label, "%s has no 'crashes: N' line: the crash listing did not finish (killed at the "
                 "45 s limit?), so crashes of this boot may be missing%s" % (CRASH_MEMBER, tail))]
    blocks = _crash_blocks(txt)
    if not blocks:
        return [("INFO", label, "no crash report (*.ips) newer than the boot in /Library/Logs/DiagnosticReports or "
                 "/Users/*/Library/Logs/DiagnosticReports")]
    out = []
    byproc = {}
    for b in blocks:                         # newest first (collect.sh sorts them)
        if b["unreadable"]:
            out.append((lv("WARN"), label, "crash report %s unreadable: %s%s" % (b["path"], b["unreadable"], tail)))
            continue
        byproc.setdefault(b["process"], []).append(b)
    for proc, bl in byproc.items():
        b = bl[0]
        out.append((lv("WARN"), label, "%s crashed %d time(s) since the boot (newest pid %s): %s%s; top frame of the "
                    "crashed thread: %s; %s%s" % (
                        proc, len(bl), b["pid"], b["exception"] or "no exception in the report",
                        "; " + b["indicator"] if b["indicator"] else "", b["frame"] or "not in the report",
                        "copy " + b["copy"] if b["copy"] else b["path"], tail)))
    return out


def no_card_boot(path):
    """None, or why this report is of a boot WITHOUT the card: 04-pci.txt was dumped (it lists devices) and has no
    AMD 1002:747e device, and the boot-args do not ask for the driver (rdna3...=). A boot that asked for the
    driver and lost the card is a real failure and is not demoted. Card run 2026-10-25c:
    rdna3-report-default-profile-20261025-023935 (the VM's default profile: no card)"""
    pci = read_tar_member(path, "04-pci.txt")
    # only a complete device list counts (collect.sh's AMD section and the ioreg run's exit line): an ioreg
    # killed at the time limit before it reached the card must not demote a real card report
    if "vendor-id" not in pci or "== AMD devices" not in pci:
        return None
    for blk in re.split(r"(?m)^[ |]*\+-o ", pci):
        v = re.search(r'"vendor-id" = <([0-9a-fA-F]{8})>', blk)
        d = re.search(r'"device-id" = <([0-9a-fA-F]{8})>', blk)
        if v and d and v.group(1).lower() == "02100000" and d.group(1).lower() == "7e740000":
            return None
    args = read_tar_member(path, "01-boot-args.txt")
    if re.search(r"\brdna3[a-z0-9_]*=", args):
        return None
    return "no AMD 1002:747e device in 04-pci.txt and the boot-args do not ask for the driver"


NOCARD_LEVEL_RE = re.compile(r"(?m)^(\s*)(FAIL|WARN|CRITICAL)\b")


def demote_nocard(text):
    """a no-card boot's printed rows: every FAIL / WARN / CRITICAL row leads with INFO"""
    return NOCARD_LEVEL_RE.sub(r"\1INFO", text)



def present_mode_lines(path, txt="", args="", host=None):
    """[(level, label, text)]: how the run presents and what could decide it (see above).
    The report archive (12-ioreg-fb.txt, 20-log-rdna3.txt, 24-log-coredisplay.txt,
    00-system.txt, 23-windowserver-crash.txt), the tests file text and the host log"""
    args = args or (re.search(r"^boot-args: (.*)$", txt or "", re.M) or [None, ""])[1]
    if host is None:
        host, hstatus, hnote, hname = host_log_for(path, args)
    else:
        hstatus, hnote, hname = "matched", "", "the host log"
    fbtxt, cd, klog, system, crash = (read_tar_member(path, n) for n in (
        "12-ioreg-fb.txt", "24-log-coredisplay.txt", "20-log-rdna3.txt", "00-system.txt",
        "23-windowserver-crash.txt"))
    out = []
    # a bare metal run has no VM host log (PLATFORM: ..., bare metal, ...)
    bare = bool(re.search(r"^PLATFORM: .*, bare metal, ", txt or "", re.M))
    # collect.sh wraps a log in "$ command" / "[exit N]": what is left is the log
    cd_lines = [l for l in cd.splitlines() if l.strip() and not l.startswith(("$ ", "[exit"))]
    # transactions the accelerator saw
    ok = re.findall(r"debug\.rdna3accel_iop_ok: (\d+)", txt)
    empty = re.findall(r"debug\.rdna3accel_iop_empty: (\d+)", txt)
    planes = bool(ok) and int(ok[-1]) > 0
    # the mode WindowServer's CoreDisplay chose
    m = re.findall(r"init_page_flip: page flip mode is (\w+)", cd)
    sets = len(re.findall(r"MPAccessSurfaceForDisplayDevice: Set up page flip mode", cd))
    if m:
        out.append(("INFO", "page flip mode", "init_page_flip: page flip mode is %s%s" % (
            m[-1], "; 'Set up page flip mode' on a display %d time(s)" % sets if sets else "")))
    elif cd_lines and planes:
        out.append(("INFO", "page flip mode", "no 'init_page_flip' line in the CoreDisplay log, and planes arrive "
                    "in transactions: not the page-flip mode"))
    elif cd_lines:
        out.append(("WARN", "page flip mode", "no 'init_page_flip' line in the %d-line CoreDisplay log "
                    "(24-log-coredisplay.txt): the mode is not known" % len(cd_lines)))
    else:
        out.append(("WARN", "page flip mode", "24-log-coredisplay.txt is missing or empty: the present mode is not known"))
    if ok and empty:
        o, e = int(ok[-1]), int(empty[-1])
        out.append(("INFO", "transactions", "iop_ok %d, iop_empty %d: %s" % (
            o, e, "planes arrive in IOPresentment transactions" if o else
            "plane-less IOPresentment transactions only" if e else
            "no IOPresentment transactions at all")))
    elif "rdna3accel=1" in args + txt:
        out.append(("WARN", "transactions", "debug.rdna3accel_iop_ok / iop_empty are not in the tests file (an accelerator run)"))
    else:
        out.append(("INFO", "transactions", "no accelerator counters (rdna3accel is off in this run)"))
    # DRIVER-PLAN N44: the vendor CoreDisplay gave each framebuffer (its IOPresentment gate) and the
    # useIOP / useIOP_onAMD preferences WindowServer started with
    prefs_rows, iop_amd = cd_prefs_rows(txt, args, read_tar_member(path, "19-coredisplay-prefs.txt"))
    out.extend(gpu_vendor_rows(cd, planes if ok else None, iop_amd, iop_prefs_off(args), klog))
    out.extend(prefs_rows)
    out.extend(iop_log_rows(cd))
    out.extend(pairing_rows(read_tar_member(path, PAIRING_MEMBER)))
    # IOGraphics' integrated flag and the two index properties, per framebuffer
    fbs = _fb_blocks(fbtxt)
    if not fbs:
        out.append(("WARN", "integrated", "12-ioreg-fb.txt has no RDNA3Framebuffer: IOFBIntegrated is not known"))
        out.append(("WARN", "fb index", "12-ioreg-fb.txt has no RDNA3Framebuffer: the index properties are not known"))
    else:
        yes = sum(1 for _, _, i in fbs if i == "Yes")
        out.append(("INFO", "integrated", "IOFBIntegrated = Yes on %d of %d framebuffer(s)%s" % (
            yes, len(fbs), "" if yes else " (not set: IOGraphics sees a discrete card)")))
        ogl, dep = [f[0] for f in fbs], [f[1] for f in fbs]
        text = "IOFramebufferOpenGLIndex %s, IOFBDependentIndex %s" % (
            ",".join(x or "?" for x in ogl), ",".join(x or "?" for x in dep))
        bad = [str(i) for i, (a, b) in enumerate(zip(ogl, dep)) if a is None or b is None or a != b]
        # the state of card run 2026-10-17d (DRIVER-PLAN N39 open item 3) is INFO on every run since: a WARN that
        # never goes away is noise. A different pattern, or the indices agreeing now, is news (a run's data shows
        # the mismatch changed)
        if not bad:
            out.append(("INFO", "fb index", text + ": they agree" + (
                " (card run 2026-10-17d had 0,1 against 1,0: this run differs, note it in DRIVER-PLAN N39 open item 3)"
                if len(fbs) > 1 else "")))
        elif (ogl, dep) == (["0", "1"], ["1", "0"]):
            out.append(("INFO", "fb index", text + ": they disagree on framebuffer(s) %s, as in card run 2026-10-17d "
                        "(unchanged; whether it matters is DRIVER-PLAN N39 open item 3)" % ",".join(bad)))
        else:
            out.append(("WARN", "fb index", text + ": they disagree on framebuffer(s) %s, and not as 0,1 / 1,0 of card run "
                        "2026-10-17d: the mismatch CHANGED (DRIVER-PLAN N39 open item 3)" % ",".join(bad)))
    # GPUWrangler's view of the card
    seen = sorted(set(GPUW_RE.findall(cd + "\n" + klog)))
    bus = None
    seen = [n for n in seen if n]
    if seen:
        _, flags, tags, did, b, dv, fn = seen[-1]
        bus = int(b)
        ig = "IG" in (tags or "").replace(",", " ").split()
        out.append(("INFO", "GPUWrangler", "flags %s (%s) vid.did=%s b:d:f=%s:%s:%s: %s" % (
            flags, tags or "no tags", did, b, dv, fn,
            "an integrated GPU (IG)" if ig else "a discrete GPU")))
    else:
        out.append(("WARN", "GPUWrangler", "no 'GPUWrangler (reg) gpu ... flags ... b:d:f=' line in "
                    "24-log-coredisplay.txt / 20-log-rdna3.txt: the integrated flag is not known"))
    # the host log: absent, guessed or without the request lines = a WARN row of its own
    for lv, label, t in host_request_rows(host, hstatus, hnote, hname, bare=bare):
        out.append((lv, label, t))
    asked_model, asked_bus = _host_request(host)
    guess = " [UNVERIFIED: the host log was not matched to this run]" if hstatus in ("unmatched", "ambiguous") else ""
    if asked_bus and bus is not None and ((asked_bus == "pcie0") != (bus == 0)):
        out.append(("WARN", "card bus", "rdna3vm put the card on %s but macOS sees PCI bus %d (b:d:f above): "
                    "the host log's 'GPU on' and the guest PCI bus lines (host-*.log) say where it really was%s" % (
                        asked_bus, bus, guess)))
    # the Mac model and what CoreDisplay makes of it
    models = []
    for src, rx, text in (("hw.model", r"^hw\.model: (\S+)", system),
                          ("PLATFORM line", r"^PLATFORM: .*?, ([A-Za-z]+\d+,\d+) ", txt),
                          ("WindowServer crash report", r'"modelCode" : "([^"]+)"', crash)):
        m = re.search(rx, text, re.M)
        if m:
            models.append((m.group(1), src))
    flags = {}
    for k in ("isBDW", "isMobile", "isMUX", "isBuiltIn"):
        v = sorted(set(re.findall(r"get_display_device_info: .*?\b%s=(YES|NO)" % k, cd)))
        if v:
            flags[k] = "/".join(v)
    nomux = "mux_initialize: Couldn't find any matches" in cd
    asked = [asked_model] if asked_model and asked_model != "not asked" else []
    lv, parts = "INFO", []
    if models:
        bid = re.search(r"^board-id: (Mac-\S+)", system, re.M)
        parts.append("%s (%s%s)" % (models[0] + (", board-id " + bid.group(1) if bid else "",)))
    else:
        lv = "WARN"
        parts.append("model not in the report (no hw.model in 00-system.txt, no PLATFORM line, no crash report)")
    if flags:
        parts.append("CoreDisplay " + " ".join("%s=%s" % kv for kv in sorted(flags.items())))
    if nomux:
        parts.append("mux_initialize found no match")
    if asked_model == "not asked":
        parts.append("rdna3vm asked for no model (the OpenCore config was left as it is)")
    if asked:
        parts.append("rdna3vm asked for %s" % asked[-1])
        if models and models[0][0] != asked[-1]:
            lv = "WARN"
            parts.append("BUT macOS reports %s: OpenCore did not apply the model%s" % (models[0][0], guess))
    if models and models[0][0].startswith("MacBook") and flags.get("isMobile") == "YES":
        parts.append("laptop model: CoreDisplay takes the mobile paths")
    elif models and not models[0][0].startswith("MacBook") and flags.get("isMobile") == "YES":
        lv = "WARN"
        parts.append("isMobile=YES on a desktop model: something else makes CoreDisplay mobile")
    out.append((lv, "SMBIOS", "; ".join(parts)))
    return out


# ---- DRIVER-PLAN N40 (the driver side of the present-mode A/B): RDNA3Framebuffer's switches
# (RDNA3,Switches), what they did (RDNA3,OpenOrder, RDNA3,IntegratedAB, RDNA3,VBLSource) and its
# counters (RDNA3,FBStats) per framebuffer from 12-ioreg-fb.txt, and RDNA3AGDC's vendor answer
# (debug.rdna3agdc_vendor*) from the tests file. Missing data is a WARN row, never silence.
FB_N40_SWITCHES = ("dispflags", "glorder", "fbintegrated", "cursorasync", "fbwake", "vblonce", "fbdtmap", "fbconsole")
# what is in effect where it can differ from the switch as asked (ECC review): a thread call moves the cursor, the
# hardware cursor exists, the VBL thread call exists (and is ONCE), the wake tap and its gate exist
FB_N40_EFFECTIVE = ("cursorasync_active", "hwcursor", "vbl_call", "vblonce_active", "waketap", "wakegate")
# every counter RDNA3,FBStats carries: an absent key is not a 0
FB_N40_STATS = (
    "cursor_sets", "cursor_kicks", "cursor_coalesced", "cursor_same", "cursor_applies", "cursor_fails", "cursor_sync",
    "cursor_images", "cursor_image_fails", "cursor_convert_fails",
    "wake_sleeps", "wake_family", "wake_owed_max", "wake_plain_on", "wake_didwake_seen", "wake_unanswered",
    "wake_nogate", "wake_off", "wake_tap_events", "wake_slept_pending", "wake_paging_already", "wake_dropped",
    "wake_fails", "wake_retries", "wake_gave_up", "wake_stopping",
    "vbl_delivered", "vbl_polls", "vbl_still", "vbl_overlap", "vbl_call_failed",
    "integ_applied", "integ_reset", "integ_late", "open_ungated", "open_fails",
    "dt_validate", "dt_matched", "dt_refused", "dt_sets", "dt_mapped", "dispflags_asked")
AGDC_VENDORS = {0: "AMD 0x1002", 1: "NVIDIA 0x10de", 2: "Intel 0x8086"}


def _ioreg_dict(blk, key):
    """{name: int} of an ioreg dictionary property, None when the property is missing"""
    m = re.search(r'"%s" = \{([^}]*)\}' % re.escape(key), blk)
    return None if not m else {k: int(v) for k, v in re.findall(r'"(\w+)"=(\d+)', m.group(1))}


def _ioreg_str(blk, key):
    m = re.search(r'"%s" = "([^"]*)"' % re.escape(key), blk)
    return m.group(1) if m else None


def _boot_arg_num(args, name):
    """(raw, int or None) of boot-arg 'name=value' as PE_parse_boot_argn reads a number (decimal or 0x), None when
    the boot-arg is absent; the last one wins"""
    m = re.findall(r"(?:^|\s)%s=(\S+)" % re.escape(name), args or "")
    if not m:
        return None
    raw = m[-1]
    try:
        return raw, int(raw, 16) if raw.lower().startswith("0x") else int(raw)
    except ValueError:
        return raw, None


def _fb_switch_rows(name, sw, args):
    """WARN rows: a switch the boot-args ask for that the framebuffer does not publish, and what is not in effect"""
    out = []
    for k in FB_N40_SWITCHES:
        req = _boot_arg_num(args, "rdna3" + k)
        if req is None or k not in sw:
            continue
        raw, val = req
        if val is None or val != sw[k]:
            out.append(("WARN", "switch asked", "%s: boot-args ask rdna3%s=%s but the framebuffer publishes %s %d: the "
                        "boot-arg did not reach it (not a number, or a build that reads it otherwise)" % (
                            name, k, raw, k, sw[k])))
    missing = [k for k in FB_N40_EFFECTIVE if k not in sw]
    if missing:
        out.append(("WARN", "switch in effect", "%s: RDNA3,Switches has no %s: what is in effect is not known (a build "
                    "older than the ECC review of N40)" % (name, ",".join(missing))))
    if sw.get("cursorasync") and sw.get("hwcursor") and sw.get("cursorasync_active") == 0:
        out.append(("WARN", "switch in effect", "%s: rdna3cursorasync %d but NO cursor thread call: the cursor moves on "
                    "WindowServer's thread (the serial log's 'hardware cursor: no lock or thread call' line)" % (
                        name, sw["cursorasync"])))
    if sw.get("vbl_call") == 0:
        out.append(("WARN", "switch in effect", "%s: NO VBL thread call: IOGraphics gets no VBL at all%s" % (
            name, " (rdna3vblonce %d is not in effect)" % sw["vblonce"] if sw.get("vblonce") else "")))
    if sw.get("fbwake") and sw.get("waketap") == 0:
        out.append(("WARN", "switch in effect", "%s: rdna3fbwake %d but NO notification tap: a system sleep is never "
                    "seen, the family wake work never runs" % (name, sw["fbwake"])))
    if sw.get("fbwake") and sw.get("wakegate") == 0:
        out.append(("WARN", "switch in effect", "%s: rdna3fbwake %d but NO command gate on the controller workloop: "
                    "the family wake work cannot run" % (name, sw["fbwake"])))
    return out


def _fb_integrated_rows(name, blk, sw, st, integ):
    """the IOFBIntegrated A/B: the property the report shows against the switch, and what the kext did"""
    prop = (re.search(r'"IOFBIntegrated" = (\w+)', blk) or [None, None])[1]
    mode = sw.get("fbintegrated", 0)
    shown = "IOFBIntegrated %s" % (prop if prop else "absent")
    if integ is None:
        return [("WARN", "integrated A/B", "%s: no RDNA3,IntegratedAB (a build older than DRIVER-PLAN N40); %s" % (name, shown))]
    if integ.startswith("never opened"):
        return [("WARN", "integrated A/B", "%s: %s - the A/B was never applied; %s" % (name, integ, shown))]
    bad = (mode == 1 and prop is not None) or (mode == 2 and prop != "Yes")
    extra = ""
    if st.get("integ_reset"):
        extra += "; IOGraphics re-set it %d time(s), applied again" % st["integ_reset"]
    if st.get("integ_late"):
        extra += "; applied late %d time(s) (an open not through our open())" % st["integ_late"]
    return [("WARN" if bad else "INFO", "integrated A/B", "%s: %s; %s%s%s" % (
        name, integ, shown, extra, ": NOT what rdna3fbintegrated=%d asks (1 = removed, 2 = Yes)" % mode if bad else ""))]


def _fb_wake_row(name, sw, g):
    tap = sw.get("waketap")
    power_ons = g("wake_plain_on") + g("wake_family")
    why = []
    if g("wake_nogate"):
        why.append("no gate %d time(s)" % g("wake_nogate"))
    if g("wake_fails"):
        why.append("the wake work FAILED %d time(s) (retried %d, given up %d)" % (
            g("wake_fails"), g("wake_retries"), g("wake_gave_up")))
    if sw.get("fbwake"):
        if g("wake_sleeps") > g("wake_family") + g("wake_paging_already"):
            why.append("a system sleep without the family's wake work and without paging on - the serial log's "
                       "'system wake' line says whether WindowServer waited")
        if tap == 0:
            why.append("no notification tap")
        if power_ons >= 2 and g("wake_tap_events") == 0:
            why.append("%d power-ons and the tap saw no WillSleep/DidWake at all (only display sleeps, or a deaf tap: "
                       "the serial log's 'system sleep #' lines say)" % power_ons)
        if g("wake_slept_pending"):
            why.append("a system sleep still recorded when the report was taken (the wake work did not run yet)")
    return ("WARN" if why else "INFO", "wake", "%s: system sleeps %d, family wakes %d (most DidWake(false) owed %d), "
            "plain power-ons %d, paging already on %d (WillSleep(false) dropped %d), DidWake(false) seen %d, "
            "WillSleep(false) unanswered %d, tap events %d (tap %s), sleep pending %d, wake work off at a wake %d "
            "(rdna3fbwake %d)%s" % (
                name, g("wake_sleeps"), g("wake_family"), g("wake_owed_max"), g("wake_plain_on"),
                g("wake_paging_already"), g("wake_dropped"), g("wake_didwake_seen"), g("wake_unanswered"),
                g("wake_tap_events"), "?" if tap is None else "present" if tap else "ABSENT", g("wake_slept_pending"),
                g("wake_off"), sw.get("fbwake", 0), ": " + "; ".join(why) if why else ""))


# DRIVER-PLAN N42: the VBL from the DCN vblank interrupt, the flip done from the pflip interrupt (RDNA3,FBStats virq_*)
FB_N42_WHY = {0: "as asked", 1: "debug.rdna3_vblirq=0", 2: "no MSI handler (rdna3msi=1?)", 3: "cut after a storm",
              4: "the enable bit did not read back", 5: "the head has no OTG / HUBP",
              6: "the display (or this head: still lost after the restore) was not brought back after a power loss / "
                 "mode-1 reset"}
# common/rdna3_vbl_irq.h RD3FB_VIRQ_NOREG_*: why the framebuffer never asked the core to REGISTER
FB_N42_NOREG = {0: "no reason recorded", 1: "rdna3vblirq=0", 2: "no DCN head", 3: "no VBL thread call",
                4: "no symbol / thread call for the interrupt", 5: "the framebuffer was stopping at its first try"}
FB_N42_STATS = ("virq_reg", "virq_reg_ret", "virq_reg_tries", "virq_live", "virq_pflip_live", "virq_why", "virq_irqs",
                "virq_delivered", "virq_dup", "virq_fallback", "virq_fallback_runs", "virq_lat_avg_us",
                "virq_lat_max_us", "virq_pflips", "latch_waits", "latch_timeouts", "latch_irq_woke",
                "pflip_lat_avg_us", "pflip_lat_max_us", "core_vblank", "core_pflip", "core_storms", "virq_noreg",
                "virq_late_unreg")


def _fb_vblirq_rows(name, sw, st, g, dcn):
    """the 'vblank irq' row of one framebuffer (DRIVER-PLAN N42): WARN when the interrupt path is on but not
    registered (or never asked), off, silent or fell back to the poll; INFO with the counts and the latencies
    otherwise, and for a framebuffer with no DCN head (the console timer)"""
    if not dcn:
        return [("INFO", "vblank irq", "%s: no DCN head (the console timer delivers): no vblank interrupt to use" % name)]
    if "virq_live" not in st:
        return [("WARN", "vblank irq", "%s: RDNA3,FBStats has no virq_* counters (a framebuffer older than DRIVER-PLAN "
                 "N42): the VBL came from the OTG poll" % name)]
    miss = [k for k in FB_N42_STATS if k not in st]
    out = []
    if miss:
        out.append(("WARN", "vblank irq", "%s: RDNA3,FBStats MISSING %s (unknown, not 0)" % (name, ",".join(miss))))
    if not sw.get("vblirq", 1):
        out.append(("INFO", "vblank irq", "%s: rdna3vblirq=0: the OTG poll alone (the A/B's off side), %d VBL(s)" % (
            name, g("vbl_delivered"))))
        return out
    pfl = "pflip interrupts %d, latch waits %d (%d ended by the pflip interrupt: avg %d us, max %d us; %d timed " \
          "out)" % (g("virq_pflips"), g("latch_waits"), g("latch_irq_woke"), g("pflip_lat_avg_us"),
                    g("pflip_lat_max_us"), g("latch_timeouts"))
    if not g("virq_reg") and not g("virq_reg_tries"):
        # ECC review: never asked is not "refused"; the framebuffer records why (virq_noreg)
        out.append(("WARN", "vblank irq", "%s: rdna3vblirq=1 but REGISTER was never attempted (%s): the OTG poll "
                    "delivered" % (name, FB_N42_NOREG.get(g("virq_noreg"), "reason %d unknown" % g("virq_noreg")))))
        return out
    if not g("virq_reg"):
        ret = g("virq_reg_ret") & 0xffffffff
        out.append(("WARN", "vblank irq", "%s: rdna3vblirq=1 but NOT registered with the core (rdna3DisplayIrq answered "
                    "0x%x after %d tries: %s): the OTG poll delivered" % (
                        name, ret, g("virq_reg_tries"),
                        "an older core or rdna3vblirq=0 there" if ret == 0xe00002c7 else
                        "no MSI handler or no head yet (rdna3msi=1?)" if ret == 0xe00002d8 else "refused")))
        return out
    if not g("virq_live"):
        out.append(("WARN", "vblank irq", "%s: registered but the vblank interrupt is OFF (why %d: %s): the OTG poll "
                    "delivered; %s" % (name, g("virq_why"), FB_N42_WHY.get(g("virq_why"), "?"), pfl)))
        return out
    if not g("virq_irqs"):
        out.append(("WARN", "vblank irq", "%s: the vblank interrupt is on but SILENT: none reached the framebuffer (the "
                    "core counted %d), the OTG poll delivered %d VBL(s) as the fallback" % (
                        name, g("core_vblank"), g("virq_fallback"))))
        return out
    why = []
    if g("virq_fallback_runs"):
        why.append("fell back to the OTG poll %d time(s), %d VBL(s) by it" % (g("virq_fallback_runs"),
                                                                             g("virq_fallback")))
    if g("core_storms"):
        why.append("%d storm(s) cut" % g("core_storms"))
    if g("virq_pflip_live") and g("latch_waits") > g("latch_timeouts") and not g("latch_irq_woke"):
        why.append("the pflip interrupt is live but no latch wait was ended by it")
    if g("virq_late_unreg"):
        why.append("%d registration(s) found at stop after the unregister (unregistered again)" % g("virq_late_unreg"))
    out.append(("WARN" if why else "INFO", "vblank irq", "%s: VBL from the vblank interrupt: %d interrupt(s), %d VBL(s) "
                "delivered by it (interrupt -> delivery avg %d us, max %d us), %d same-vblank skip(s); %s%s" % (
                    name, g("virq_irqs"), g("virq_delivered"), g("virq_lat_avg_us"), g("virq_lat_max_us"),
                    g("virq_dup"), pfl, ": " + "; ".join(why) if why else "")))
    return out


def fb_n40_lines(path, txt="", args=""):
    """[(level, label, text)]: the N40 switches and counters of every framebuffer and the AGDC vendor answer"""
    fbtxt = read_tar_member(path, "12-ioreg-fb.txt")
    args = args or (re.search(r"^boot-args: (.*)$", txt or "", re.M) or [None, ""])[1]
    out = []
    blocks = re.split(r"(?m)^\+-o ", fbtxt)[1:]
    if not blocks:
        out.append(("WARN", "switches", "12-ioreg-fb.txt has no RDNA3Framebuffer: the N40 switches and counters are not known"))
    for i, blk in enumerate(blocks):
        dep = (re.search(r'"IOFBDependentIndex" = (\d+)', blk) or [None, "?"])[1]
        gl = (re.search(r'"IOFramebufferOpenGLIndex" = (\d+)', blk) or [None, "?"])[1]
        name = "fb %s" % dep
        sw = _ioreg_dict(blk, "RDNA3,Switches")
        if sw is None:
            out.append(("WARN", "switches", "%s has no RDNA3,Switches: a framebuffer older than DRIVER-PLAN N40 "
                        "(or not the installed build): the A/B switches are not known" % name))
            continue
        missing = [k for k in FB_N40_SWITCHES if k not in sw]
        out.append(("WARN" if missing else "INFO", "switches", "%s: %s%s" % (
            name, ", ".join("%s %s" % (k, sw[k]) for k in FB_N40_SWITCHES if k in sw),
            "; boot console here" if sw.get("bootconsole") else "; not the boot console") +
            ("; MISSING %s" % ",".join(missing) if missing else "")))
        out.extend(_fb_switch_rows(name, sw, args))
        st = _ioreg_dict(blk, "RDNA3,FBStats") or {}
        out.append(("INFO", "connection flags", "%s: kConnectionDisplayFlags answered %d (rdna3dispflags %d), asked %s" % (
            name, 1 if sw.get("dispflags") else 0, sw.get("dispflags", 0),
            "%d time(s)" % st["dispflags_asked"] if "dispflags_asked" in st else "? times")))
        order = _ioreg_str(blk, "RDNA3,OpenOrder")
        if order is None:
            out.append(("WARN", "open order", "%s: no RDNA3,OpenOrder: WindowServer never opened it" % name))
        else:
            bad = sw.get("glorder") and gl != "?" and gl != dep
            failed = "open FAILED" in order
            out.append(("WARN" if bad or failed else "INFO", "open order", "%s: %s; IOFramebufferOpenGLIndex %s, IOFBDependentIndex %s%s" % (
                name, order, gl, dep, ": STILL DIFFERENT with rdna3glorder=1 (DRIVER-PLAN N40 item 2)" if bad else "")))
        out.extend(_fb_integrated_rows(name, blk, sw, st, _ioreg_str(blk, "RDNA3,IntegratedAB")))
        if not st:
            out.append(("WARN", "counters", "%s has no RDNA3,FBStats: the N40 counters are not known" % name))
            continue
        smiss = [k for k in FB_N40_STATS if k not in st]
        if smiss:
            out.append(("WARN", "counters", "%s: RDNA3,FBStats MISSING %s: those counters are not known (not 0; a "
                        "build older than the ECC review of N40)" % (name, ",".join(smiss))))
        g = lambda k: st.get(k, 0)
        if g("open_ungated") or g("open_fails"):
            out.append(("WARN", "open", "%s: open() outside IOGraphics' system gate %d time(s), failed opens %d" % (
                name, g("open_ungated"), g("open_fails"))))
        async_on = sw.get("cursorasync") and sw.get("hwcursor", 1)
        unused = async_on and "cursor_sets" in st and "cursor_images" in st and g("cursor_sets") == 0 and g("cursor_images") == 0
        cfail = g("cursor_fails") > 0 or g("cursor_image_fails") > 0 or \
            (sw.get("cursorasync") and g("cursor_sets") > 0 and g("cursor_applies") == 0)
        out.append(("WARN" if cfail or unused else "INFO", "cursor", "%s: %s; sets %d, thread-call runs %d, folded %d, unchanged %d, "
                    "applied %d, FAILED %d, on WindowServer's thread %d, images %d (first image refused %d, not "
                    "converted %d)%s" % (
                        name, "thread call" if sw.get("cursorasync") and sw.get("cursorasync_active", 1) else
                        "synchronous (rdna3cursorasync=0 or no thread call)",
                        g("cursor_sets"), g("cursor_kicks"), g("cursor_coalesced"), g("cursor_same"),
                        g("cursor_applies"), g("cursor_fails"), g("cursor_sync"), g("cursor_images"),
                        g("cursor_image_fails"), g("cursor_convert_fails"),
                        ": WindowServer never set a cursor image or position on the hardware cursor" if unused else "")))
        out.append(_fb_wake_row(name, sw, g))
        src = _ioreg_str(blk, "RDNA3,VBLSource") or "?"
        overlap = g("vbl_overlap")
        dcn = _ioreg_str(blk, "RDNA3,Mode") == "dcn"
        novbl = "vbl_delivered" in st and g("vbl_delivered") == 0 and dcn
        vwhy = []
        if overlap and sw.get("vblonce") and sw.get("vblonce_active", 1):
            vwhy.append("overlapping runs with vblonce on")
        if novbl:
            vwhy.append("NO VBL delivered on a DCN head")
        if g("vbl_call_failed"):
            vwhy.append("NO VBL thread call")
        out.append(("WARN" if vwhy else "INFO", "VBL", "%s: source %s (%s), delivered %d, 250 us re-looks %d, "
                    "delivered with the counter unmoved %d, "
                    "overlapping runs %d (rdna3vblonce %d%s)%s" % (
                        name, src, "no hardware vblank interrupt yet" if "virq_live" not in st else
                        "the vblank interrupt: the 'vblank irq' row", g("vbl_delivered"), g("vbl_polls"),
                        g("vbl_still"), overlap, sw.get("vblonce", 0),
                        "; overlaps with vblonce=0: the switch's case" if overlap and not sw.get("vblonce") else "",
                        ": " + "; ".join(vwhy) if vwhy else "")))
        out.extend(_fb_vblirq_rows(name, sw, st, g, dcn))
        out.append(("INFO", "detailed timings", "%s: validateDetailedTiming %d (%d one of our modes, %d refused), "
                    "setDetailedTimings %d, ids mapped %d (rdna3fbdtmap %d)" % (
                        name, g("dt_validate"), g("dt_matched"), g("dt_refused"), g("dt_sets"), g("dt_mapped"),
                        sw.get("fbdtmap", 0))))
    out.extend(agdc_vendor_lines(path, txt, args))
    return out


# DRIVER-PLAN N42: the core's display interrupts (debug.rdna3_dispirq: the tests file, the archive's
# 18-sysctl-dispirq.txt, or the RDNA3,DisplayIrq property in 11-ioreg-props.txt)
DISPIRQ_SYSCTL_RE = re.compile(r"debug\.rdna3_dispirq: (vblirq=[^\n]*)")
DISPIRQ_PROP_RE = re.compile(r"\"RDNA3,DisplayIrq\" = \"(vblirq=[^\n\"]*)")
DISPIRQ_WHY = {0: "as asked", 1: "debug.rdna3_vblirq=0", 2: "no MSI handler", 3: "cut after a storm",
               4: "the enable bit did not read back", 5: "the head has no OTG / HUBP",
               6: "the display (or this head: still lost after the restore) was not brought back after a power loss / "
                  "mode-1 reset"}
DISPIRQ_KINDS = (("VBLANK", "OTG"), ("PFLIP", "HUBP"), ("VUPDATE", "OTG"))
# the keys the ECC review round added to the core's report (an older string lacks them: unknown, not 0)
DISPIRQ_ECC_KEYS = ("hpdnopin", "unr", "unrinst", "badmask", "ackfails", "offfails", "drained", "drains", "down",
                    "downs", "downivs", "dceother", "dce")
# the keys of the review's round 2 (per-head "up" after a wake / reset, stale sources, failed publishes)
DISPIRQ_ECC2_KEYS = ("downheads", "hdowns", "staleoffs", "publishfails")


def _dispirq_num(v):
    try:
        return int(v, 16) if v.lower().startswith("0x") else int(v)
    except ValueError:
        return None


def _dispirq_triple(v):
    """'a/b/c' (per VBLANK / PFLIP / VUPDATE) -> [a, b, c]; missing or unreadable parts are 0"""
    parts = (v or "").split("/")
    return [(_dispirq_num(parts[i]) or 0) if i < len(parts) else 0 for i in range(3)]


def _dispirq_insts(mask):
    return ",".join(str(i) for i in range(32) if mask >> i & 1) or "?"


def _dispirq_prop_num(props, key):
    m = re.search(r'"%s" = (\d+)' % re.escape(key), props or "")
    return int(m.group(1)) if m else None


def dispirq_lines(path, txt="", args=""):
    """[(level, label, text)] of the core's vblank / pflip / HPD interrupts (DRIVER-PLAN N42): WARN when the path is
    on but has no MSI handler, was switched off, stormed, failed to enable / ack / disable, dropped IVs no
    framebuffer owns or of an instance DCN 3.2 lacks, met DCE IVs no handler claims, HPD IVs of a pin DCN 3.2 does
    not wire, a display not brought back, or a registered head's sources stay silent; INFO with the per-head counts.
    The live sysctl (tests file, 18-sysctl-dispirq.txt) wins over the RDNA3,DisplayIrq property, a copy published
    at the last sync. Missing data is a WARN row."""
    sysctl = (txt or "")
    props = ""
    if path and os.path.exists(path):
        sysctl += "\n" + read_tar_member(path, "18-sysctl-dispirq.txt")
        props = read_tar_member(path, "11-ioreg-props.txt")
    found = DISPIRQ_SYSCTL_RE.findall(sysctl)
    origin = "debug.rdna3_dispirq"
    if not found:
        found = DISPIRQ_PROP_RE.findall(props)
        origin = "the RDNA3,DisplayIrq property (a copy from the last sync / HPD: older than the report)"
    if not found:
        return [("WARN", "display irq", "no debug.rdna3_dispirq / RDNA3,DisplayIrq: a core older than DRIVER-PLAN N42, "
                 "or not collected - whether the vblank / pflip interrupts ran is not known")]
    parts = found[-1].split(" | ")
    g = dict(re.findall(r"(\w+)=(\S+)", parts[0]))
    n = lambda k: _dispirq_num(g.get(k, "0")) or 0
    out = []
    # the HPD counts: RDNA3,HPDIrqs is set at the first 8 HPD IVs, every 1024th and by the 2 s stats tick (the
    # string's copy may be older)
    hpd = max(n("hpd"), _dispirq_prop_num(props, "RDNA3,HPDIrqs") or 0)
    nopin = max(n("hpdnopin"), _dispirq_prop_num(props, "RDNA3,HPDIrqsNoPin") or 0)
    hpdt = "HPD interrupts %d (%s)" % (hpd, "acted on: rdna3hpdirq=1" if n("hpdirq") else
                                       "counted only: rdna3hpdirq=0, the hotplug poll decides")
    src = "" if origin == "debug.rdna3_dispirq" else " [from %s]" % origin
    why = []
    if nopin:
        why.append("%d HPD IV(s) of a pin DCN 3.2 does not wire (HPD6 / HPD6 RX): not acked, dropped" % nopin)
    missing = [k for k in DISPIRQ_ECC_KEYS if k not in g]
    if missing:
        why.append("the core's report lacks %s (older than the ECC review round: unknown, not 0)" % ",".join(missing))
    missing2 = [k for k in DISPIRQ_ECC2_KEYS if k not in g] if not missing else []
    if missing2:
        why.append("the core's report lacks %s (older than the ECC review round 2: unknown, not 0)" %
                   ",".join(missing2))
    if g.get("dceother") and n("dceother"):
        why.append("%d DCE IV(s) no handler claims (src_id:ext:ring %s): dropped" % (n("dceother"), g.get("dce", "?")))
    if not n("vblirq"):
        return [("WARN" if why else "INFO", "display irq", "rdna3vblirq=0: no vblank / pflip source enabled, the "
                 "framebuffers polled the OTG counter (the A/B's off side); " + hpdt + src +
                 (": " + "; ".join(why) if why else ""))]
    if not n("msi"):
        why.append("NO MSI handler (rdna3msi=1?): no display interrupt can arrive, the framebuffers polled")
    if not n("on"):
        why.append("debug.rdna3_vblirq was 0 when the report was taken (switched off at runtime)")
    if n("down"):
        why.append("the display was NOT brought back after a power loss / mode-1 reset: no interrupt register "
                   "written, the sources forgotten as off (%d IV(s) dropped since)" % n("downivs"))
    elif n("downs"):
        why.append("the display was not brought back %d time(s) (back now)" % n("downs"))
    if not n("down") and n("downheads"):
        why.append("head(s) %s still LOST after the restore of a wake / mode-1 reset: their interrupt registers not "
                   "written, their framebuffers poll (until a mode set lights them)" % _dispirq_insts(n("downheads")))
    elif n("hdowns"):
        why.append("a wake / mode-1 reset left head(s) down %d time(s) (back now)" % n("hdowns"))
    if n("staleoffs"):
        why.append("%d source(s) left enabled while their head was down, turned off when it came back" %
                   n("staleoffs"))
    if n("publishfails"):
        why.append("%d RDNA3,DisplayIrq publish(es) failed for want of memory (the property copy is older)" %
                   n("publishfails"))
    if g.get("stormed", "0x0/0x0/0x0") != "0x0/0x0/0x0":
        why.append("sources CUT after a storm (VBLANK/PFLIP/VUPDATE instance masks %s)" % g.get("stormed"))
    fails = []
    if n("regfails"):
        fails.append("%d enable(s) did not read back" % n("regfails"))
    if n("ackfails"):
        fails.append("%d ack(s) failed (the source may raise again at once)" % n("ackfails"))
    if n("offfails"):
        fails.append("%d disable(s) did not take (the source may go on raising)" % n("offfails"))
    if fails:
        why.append(", ".join(fails))
    unr, unrinst, badmask = _dispirq_triple(g.get("unr")), _dispirq_triple(g.get("unrinst")), \
        _dispirq_triple(g.get("badmask"))
    if n("unrouted"):
        per = ["%d %s %s %s" % (unr[k], DISPIRQ_KINDS[k][0], DISPIRQ_KINDS[k][1], _dispirq_insts(unrinst[k]))
               for k in range(3) if unr[k]]
        why.append("%d IV(s) with no framebuffer registered for them (%s): acked, dropped" % (
            n("unrouted"), ", ".join(per) if per else "kind not recorded"))
    if n("badinst"):
        per = ["%s %s %s" % (DISPIRQ_KINDS[k][0], DISPIRQ_KINDS[k][1], _dispirq_insts(badmask[k]))
               for k in range(3) if badmask[k]]
        why.append("%d IV(s) of an instance DCN 3.2 lacks (%s): dropped" % (
            n("badinst"), ", ".join(per) if per else "instance not recorded"))
    drained = sum(_dispirq_triple(g.get("drained")))
    heads = []
    for p in parts[1:]:
        h = dict(re.findall(r"(\w+)=(\S+)", p.split(" ", 1)[1] if " " in p else ""))
        hn = p.split(" ", 1)[0]
        hv = lambda k: _dispirq_num(h.get(k, "0")) or 0
        if hv("stale"):
            why.append("%s: sources 0x%x still enabled from before the head went down (not written while down; "
                       "they may raise as IVs with no framebuffer)" % (hn, hv("stale")))
        if not hv("reg"):
            heads.append("%s not registered (vblank %d, pflip %d)" % (hn, hv("vblank"), hv("pflip")))
            continue
        want, en = hv("want"), hv("en")
        if en != want:
            why.append("%s: wanted 0x%x, enabled 0x%x (%s)" % (hn, want, en, DISPIRQ_WHY.get(hv("why"), "?")))
        if (en & 1) and not hv("vblank"):
            why.append("%s: VBLANK enabled on OTG %s but NO interrupt reached the core (the DCN source or its IH route "
                       "is silent: the framebuffer's poll delivered)" % (hn, h.get("otg")))
        heads.append("%s OTG %s HUBP %s: vblank %d, pflip %d, vupdate %d, storms %d" % (
            hn, h.get("otg"), h.get("pipe"), hv("vblank"), hv("pflip"), hv("vupdate"), hv("storms")))
    if not parts[1:]:
        why.append("no framebuffer registered for the interrupts (an RDNA3Framebuffer older than N42, rdna3vblirq=0 "
                   "in the framebuffer, or none started)")
    out.append(("WARN" if why else "INFO", "display irq", "%s; total %d DCN IV(s), %d with no framebuffer, %d of an "
                "instance DCN 3.2 lacks%s; %s%s%s" % (
                    "; ".join(heads) if heads else "no head", n("total"), n("unrouted"), n("badinst"),
                    "; %d IV(s) drained after %d handler stall(s), kept out of the storm window" % (
                        drained, n("drains")) if drained else "", hpdt, src,
                    ": " + "; ".join(why) if why else "")))
    return out


def agdc_vendor_lines(path, txt="", args=""):
    """[(level, label, text)]: RDNA3AGDC's vendor answer from the tests file (autoreport.sh) or the archive's
    17-sysctl-agdc.txt (collect.sh); a TESTS-ONLY report has the tests file alone"""
    args = args or (re.search(r"^boot-args: (.*)$", txt or "", re.M) or [None, ""])[1]
    txt = (txt or "") + "\n" + read_tar_member(path, "17-sysctl-agdc.txt")
    out = []
    v = re.findall(r"debug\.rdna3agdc_vendor: (-?\d+)", txt)
    asked = re.findall(r"debug\.rdna3agdc_vendor_asked: (\d+)", txt)
    last = re.findall(r"debug\.rdna3agdc_vendor_last: (-?\d+)", txt)
    armed = bool(re.search(r"(?:^|\s)rdna3agdc=[1-9]", args or "") or re.search(r"debug\.rdna3agdc: [1-9]\b", txt))
    req = _boot_arg_num(args, "rdna3agdcvendor")
    if req is not None and not armed:
        out.append(("WARN", "AGDC vendor", "boot-args ask rdna3agdcvendor=%s but RDNA3AGDC is not armed (no rdna3agdc=1..): "
                    "the vendor answer is never given" % req[0]))
    if v:
        n = int(v[-1])
        out.append(("INFO", "AGDC vendor", "vendor info answer %s (debug.rdna3agdc_vendor %d), asked %s time(s)%s" % (
            AGDC_VENDORS.get(n, "AMD 0x1002 (unknown value %d)" % n), n, asked[-1] if asked else "?",
            "; last answered as %s" % AGDC_VENDORS.get(int(last[-1]), last[-1]) if last and last[-1] != "-1" else
            "; never asked" if last else "")))
        if req is not None and armed and req[1] != n:
            out.append(("WARN", "AGDC vendor", "boot-args ask rdna3agdcvendor=%s but debug.rdna3agdc_vendor is %d (the "
                        "boot-arg did not reach RDNA3AGDC, or the sysctl was changed at runtime)" % (req[0], n)))
        if n and asked and int(asked[-1]) == 0:
            out.append(("WARN", "AGDC vendor", "the A/B answer %s is set but IOPresentment never asked the vendor info "
                        "(debug.rdna3agdc_vendor_asked 0): this run says nothing about it" % AGDC_VENDORS.get(n, n)))
    elif armed:
        out.append(("WARN", "AGDC vendor", "AGDC armed but no debug.rdna3agdc_vendor in the tests file: an RDNA3AGDC "
                    "older than DRIVER-PLAN N40, or the counters were not collected"))
    else:
        out.append(("INFO", "AGDC vendor", "no debug.rdna3agdc_vendor (AGDC not armed in this run)"))
    out.extend(agdc_n48_lines(txt, args))
    return out


# DRIVER-PLAN N45 (2), (3) (agdc/rdna3_agdc_n48.h): sysctl name, boot-arg, default, largest value
AGDC_N48_SWITCHES = (("ib", "rdna3agdcib", 0, 3), ("linkflags", "rdna3agdclinkflags", 0, 2),
                     ("scaler", "rdna3agdcscaler", 0, 2))
AGDC_N48_COUNTERS = ("ib_answered", "zero_filled", "scaler_alt", "linkflags_last", "types", "k5_refused")


def agdc_n48_lines(txt, args):
    """[(level, label, text)]: N45's AGDC switches against the boot-args, the per-type 0x711 text and the counters
    (txt: the tests file + 17-sysctl-agdc.txt). WARN: a boot-arg the kext does not show, the inline buffer asked
    but never answered with ib bit 0 set, no zero fill with ib bit 1 set while other types were asked, counters
    missing while K5 is armed, N45 switches set while K5 is off"""
    label = "AGDC N45"

    def last(name, rx=r"(-?\d+)"):
        m = re.findall(r"debug\.rdna3agdc_%s: %s[ \t]*$" % (name, rx), txt or "", re.M)
        return m[-1] if m else None
    k5 = bool(re.search(r"(?:^|\s)rdna3agdc=[34]\b", args or "")) or last("k5") == "1"
    asked = {k: _boot_arg_num(args, b) for k, b, _, _ in AGDC_N48_SWITCHES}
    vals = {k: last(k) for k, _, _, _ in AGDC_N48_SWITCHES}
    if all(v is None for v in vals.values()):
        given = ["%s=%s" % (b, asked[k][0]) for k, b, _, _ in AGDC_N48_SWITCHES if asked[k] is not None]
        if k5 or given:
            return [("WARN", label, "%sno debug.rdna3agdc_ib / _linkflags / _scaler in the report: an RDNA3AGDC older "
                     "than DRIVER-PLAN N45, or the counters were not collected%s" % (
                         "K5 armed but " if k5 else "", "; boot-args ask " + ", ".join(given) if given else ""))]
        return [("INFO", label, "no N45 counters (K5 not armed in this run)")]
    out, why = [], []
    sw = {}
    for k, b, dflt, top in AGDC_N48_SWITCHES:
        v = int(vals[k]) if vals[k] is not None else None
        sw[k] = v
        if v is None:
            why.append("no debug.rdna3agdc_%s" % k)
            continue
        if asked[k] is not None:
            want = asked[k][1] if asked[k][1] is not None and 0 <= asked[k][1] <= top else dflt
            if want != v:
                why.append("boot-args ask %s=%s%s but debug.rdna3agdc_%s is %d (the boot-arg did not reach RDNA3AGDC, "
                           "or the sysctl was changed at runtime)" % (
                               b, asked[k][0], "" if want == asked[k][1] else " (unknown: the default %d)" % dflt, k, v))
    if not k5 and any(sw[k] not in (None, d) for k, _, d, _ in AGDC_N48_SWITCHES):
        why.append("N45 switches off their defaults while K5 is off (no rdna3agdc=3/4, debug.rdna3agdc_k5 not 1): "
                   "they answer nothing in this run")
    cnt = {c: last(c, r"(.*?)" if c == "types" else r"(-?\d+)") for c in AGDC_N48_COUNTERS}
    if k5:
        miss = [c for c in AGDC_N48_COUNTERS if cnt[c] is None]
        if miss:
            why.append("K5 armed but no debug.rdna3agdc_%s" % ", _".join(miss))
    types = {t: (int(a), int(s)) for t, a, s in re.findall(r"(0x[0-9a-f]+|other) (\d+)/(\d+)", cnt["types"] or "")}
    ib = sw.get("ib") or 0
    if k5 and ib & 1 and types.get("0x40", (0, 0))[0] and not types["0x40"][1]:
        why.append("the inline buffer (0x40) was asked %d time(s) and never answered with rdna3agdcib bit 0 set "
                   "(no timing on the head? see debug.rdna3agdc_k5_why)" % types["0x40"][0])
    others = sum(a for t, (a, _) in types.items() if t not in ("0x10", "0x40", "other"))
    if k5 and ib & 2 and others and cnt["zero_filled"] is not None and int(cnt["zero_filled"]) == 0:
        why.append("other capability types were asked %d time(s) but none was zero-filled with rdna3agdcib bit 1 set"
                   % others)
    out.append(("WARN" if why else "INFO", label, "K5 %s; ib %s, linkflags %s, scaler %s; 0x711 types (asked/answered) "
                "%s; inline buffer answered %s, zero-filled %s, scaler with AMD markers %s, last 0x921 flags %s, "
                "K5 refused %s (last why %s)%s" % (
                    "armed" if k5 else "off",
                    *("?" if sw[k] is None else sw[k] for k, _, _, _ in AGDC_N48_SWITCHES),
                    cnt["types"] if cnt["types"] is not None else "?",
                    *(cnt[c] if cnt[c] is not None else "?" for c in AGDC_N48_COUNTERS if c != "types"),
                    last("k5_why") or "?", ": " + "; ".join(why) if why else "")))
    return out


STALE_KEXTS_SHOWN = False   # a STALE KEXTS FAIL was printed (one per run; report() does not repeat it)


def stale_marker_text(kext, running, installed, cmd="rdna3vm approve"):
    return "%s (%s running %s, installed %s)" % (STALE_KEXTS_HEAD.replace("rdna3vm approve", cmd), kext, running, installed)


def stale_kext_lines(*dirs):
    """[(level, text)] of the kexts macOS ran older than the installed build, from a result directory without
    needing a report from macOS: the approve-*.txt marker autoreport.sh uploads (the exact hashes, certain), and the
    newest serial log's build lines against the hashes of the checked-out sources (the checkout must be the tested
    commit). FAIL = stale; INFO = the check could not be done (a marker or log unreadable, a hash not computable,
    no build line in the newest serial log), never silence"""
    seen, found, info = [], {}, []
    for d in dirs:
        d = os.path.abspath(d)
        if d not in seen and os.path.isdir(d):
            seen.append(d)
    # the newest host log says which Mac model the stale run had (--step N when it asked for one)
    hosts = sorted((f for d in seen for f in glob.glob(os.path.join(d, "host-*.log"))), key=os.path.getmtime)
    cmd = approve_command(_read(hosts[-1])) if hosts else "rdna3vm approve"
    for d in seen:
        for f in sorted(glob.glob(os.path.join(d, "approve-*.txt"))):
            try:
                txt = open(f, errors="replace").read()
            except OSError as e:
                info.append("approve marker %s cannot be read (%s): the stale check from it is not done" %
                            (os.path.basename(f), e.strerror or e))
                continue
            for kext, running, installed in APPROVE_MARKER_RE.findall(txt):
                found.setdefault(kext, stale_marker_text(kext, running, installed, cmd))
    # not rdna3vm's headless install boot (serial-install-*): it never loads the accelerator
    serials = sorted((f for d in seen for f in glob.glob(os.path.join(d, "serial-*.log"))
                      if not os.path.basename(f).startswith("serial-install-")), key=os.path.getmtime)
    if serials:
        sname = os.path.basename(serials[-1])
        try:
            slog = open(serials[-1], errors="replace").read()
        except OSError as e:
            slog = ""
            info.append("serial log %s cannot be read (%s): stale check not done" % (sname, e.strerror or e))
        else:
            hits = 0
            for kext, pat, files in KEXT_SERIAL_BUILDS:
                m = pat.search(slog)
                if not m:
                    continue
                hits += 1
                want = fb_source_hash(files)
                if not want:
                    info.append("%s: the hash of its sources cannot be computed here (a source file of %s is "
                                "missing from this checkout): its stale check is not done" % (kext, files[0]))
                elif m.group(1) != want and kext not in found:
                    found[kext] = ("%s (%s running %s per %s, the sources of this checkout build %s; a false "
                                   "alarm if this checkout is not the tested commit)" % (
                                       STALE_KEXTS_HEAD.replace("rdna3vm approve", cmd), kext, m.group(1), sname, want))
            if not hits:
                info.append("newest serial log %s has none of the RDNA3Accelerator / RDNA3-fb / RDNA3-agdc build "
                            "lines (the kexts did not start, or a step without them): stale check not done" % sname)
    order = [b[0] for b in KEXT_SERIAL_BUILDS]
    return [("FAIL", found[k]) for k in order if k in found] + \
        [("FAIL", v) for k, v in sorted(found.items()) if k not in order] + [("INFO", t) for t in info]


def report(path, show_all):
    global STALE_KEXTS_SHOWN
    failed = False
    with tarfile.open(path) as tar:
        args = read_member(tar, "01-boot-args.txt")
        props_txt = read_member(tar, "11-ioreg-props.txt")
        log = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt")
        panics = read_member(tar, "21-log-panics.txt")
        wsmember = read_member(tar, "22-log-windowserver.txt") or ""
        crashmember = read_member(tar, "23-windowserver-crash.txt") or ""
        cdlog = read_member(tar, "24-log-coredisplay.txt") or ""
        kexts = read_member(tar, "03-kexts.txt")
        pci = read_member(tar, "04-pci.txt")
        surfvram = read_member(tar, "29-surface-vram.txt") or ""
        mtlallow = read_member(tar, "34-metal-allow.txt") or ""
        stamps_txt = read_member(tar, "35-build-stamps.txt") or ""
        pluginlog = read_member(tar, "26-log-metal-plugin.txt") or ""
        # DRIVER-PLAN N50: the A/B ladder (rdna3vm step ladder): ladder.txt and each rung's plugin log slice
        ladder_txt = read_member(tar, "41-ladder.txt") or ""
        ladder_logs = {}
        for m_ in tar.getmembers():
            lm_ = re.search(r"41-ladder-(.+)\.log$", m_.name)
            if lm_ and m_.isfile():
                ladder_logs[lm_.group(1)] = tar.extractfile(m_).read().decode("utf-8", "replace")
    args = " ".join(l for l in args.splitlines() if not l.startswith("$") and not l.startswith("[exit"))
    args = args.replace("boot-args", "").strip()
    lvl = bootarg_level(args)
    global IN_VM
    IN_VM = "rdna3vmauto=1" in args
    props = parse_props(props_txt)
    print("=" * 78)
    print(os.path.basename(path))
    print("boot-args:", args or "?")
    if "RDNA3Accel" not in kexts and not props:
        nc = no_card_boot(path)
        if nc:
            # card run 2026-10-25c: the VM's default-profile boot has no card; main() prints the rest of this
            # report's rows as INFO and does not count it as a failure
            print("   INFO no-card boot: %s: this boot ran without the card, so its present-mode, RADV, Metal, "
                  "display sleep/wake and frame-trace rows are INFO, not results about the driver" % nc)
            return False
        print("!! RDNA3Accel not loaded (03-kexts.txt) -> AFTER-TESTS step 0")
        amd = pci.split("\n== AMD devices", 1)[1] if "\n== AMD devices" in pci else ""
        ids = [l.strip(" |") for l in amd.splitlines()
               if re.search(r'\+-o|"(vendor-id|device-id)"', l)]
        print("   AMD PCI devices seen by macOS (04-pci.txt): %s" %
              ("; ".join(ids[:9]) if ids else "none (or an old collect.sh)"))
        return True
    print("level %s (requested %d), Stage2: %s, Rings: %s" % (
        props.get("Level", "?"), lvl, props.get("Stage2", "-"), props.get("Rings", "-")))
    first_fail = None
    for key, minlvl, need, check, want in CHECKS:
        if key == "VRAMSizeMB" and REF_VRAM_MB:
            want = "%d, as Linux" % REF_VRAM_MB
        if lvl < minlvl or (need and need not in args):
            if show_all:
                print("   --   %-20s (not at this level)" % key)
            continue
        if key not in props:
            if key in ("VMFaults", "GPUResets"):
                continue            # published only when one happened
            if first_fail:
                if show_all:
                    print("   --   %-20s missing (after %s failed)" % (key, first_fail))
                continue            # a consequence of the first failure
            print("   FAIL %-20s missing (expected %s)" % (key, want))
            failed = True
            first_fail = key
            continue
        v = props[key]
        try:
            ok = check(v)
        except TypeError:
            ok = False
        shown = signed(v) if isinstance(v, int) and v >= 1 << 63 else v
        if not ok:
            failed = True
            first_fail = first_fail or key
        if not ok or show_all:
            print("   %s %-20s = %s (expected %s)" % ("PASS" if ok else "FAIL", key, shown, want))
    for key in ("SOSVersion", "SMUDriverIF", "PMGfxClkMinMHz", "PMGfxClkMaxMHz", "PMUclkMaxMHz",
                "PMFanRPM", "DMUBVersion", "GBAddrConfig", "ChipRevId", "Head0EdidMode",
                "AtomCommandTables", "SCPM", "PMFeaturesRunning", "PSPStuckAtInit", "InitResetResult", "GFXHUBResult", "GFXAsync", "KIQOnMES", "Mirror", "MirrorResult", "MirrorCheck", "ExtendHead", "DMUB", "DMUBCapsResult", "IdleSleepBlocked", "DCNClocks", "CloneMetricsResult", "DMUBWaitAckResult", "GfxCGResult", "GfxOffResult", "HotplugEvents", "HotplugPins", "MESResult", "MESSchedVersion", "MESKiqVersion", "MESLegacyQueueMap", "NeedPost", "PostResult",
                "Hotplug", "HPDState", "HotplugGeneration", "HotplugPolledEvents", "HotplugGlitches",
                "HotplugLast", "HotplugClonesLit", "HotplugClonesOff", "HotplugBootHead", "HotplugNoEdid",
                "LastFaultAddr", "LastFaultStatus", "DCNClockLimits",
                "Wakes", "WakePowerLost", "WakeResumeResult", "WakeHeadsLost", "SystemSuspends",
                "EvictedMiB", "EvictFailedMiB", "DisplayPictureLost", "HotplugResumes",
                "KernelQueuesByMES", "KernelQueuesByMESFallback", "SleepUserQueuesActive",
                "WakeUserQueuesActive", "DPLinkLosses"):
        if key in props:
            v = props[key]
            print("   info %-20s = %s" % (key, hex(v) if isinstance(v, int) and key in (
                "SOSVersion", "SMUDriverIF", "DMUBVersion", "GBAddrConfig", "LastFaultAddr",
                "LastFaultStatus") else v))
    # ECC review round 1: --all lists the first kernel-log lines of the write generations and the flip-present hooks
    if show_all and log:
        print("   --   write generations / flip-present hooks (the kernel log's first lines):")
        for l in gen_log_lines(log):
            print("        " + l[:240])
    # displays (step 2+): connectors with an EDID, what each head drives
    for key in sorted(k for k in props if re.match(r"(Connector\d+,(Monitor|EDIDSource|Type)|Head\d+,(Route|TX))$", k)):
        print("   info %-20s = %s" % (key, props[key]))
    for l in log.splitlines():
        if "refresh" in l and "display: head" in l:
            print("   info " + l[l.find("display:"):][:110])
    # PM trend: the last activity line and the last RLC/CP status line (why
    # the PMFW counts the GFX as busy while GRBM_STATUS is idle)
    for pat in ("PM: activity", "PM: GRBM_STATUS", "PM: RLC_STAT"):
        last = [l for l in log.splitlines() if pat in l]
        if last:
            print("   info " + last[-1][last[-1].find("PM:"):][:150])
    # rdna3idlecap=1: the GFXCLK cap while nothing is submitted
    caps = [l[l.find("PM: "):] for l in log.splitlines() if "GFXCLK cap" in l or "GFXCLK capped" in l]
    if caps:
        print("   idle cap: %s ... (%d lines)" % (caps[0][:110], len(caps)))
    # rdna3pmexp=1: one line per idle-power variant (10 s each)
    exp = [l[l.find("PM experiment"):] for l in log.splitlines() if "PM experiment" in l and "activity" in l]
    if exp:
        print("   idle-power experiment (activity, GFX clock, watts per variant):")
        for l in exp:
            print("     " + l[:200])
    for l in log.splitlines():
        if "soc21 CG" in l or "SocCG" in l:
            print("   info " + l[l.find("soc21"):][:120] if "soc21" in l else "   info " + l[:120])
            break
    # the Metal plugin / RADV built in the VM against the repo (see above)
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(path)), "00-host.txt"), errors="replace") as fh:
            host_txt = fh.read()
    except OSError:
        host_txt = ""
    failed = stale_builds(stamps_txt, host_txt, pluginlog + "\n" + cdlog) or failed
    # step 9: our framebuffer (rdna3fb=1) took the head and has the EDID
    if "rdna3fb=1" in args:
        fb_started = "RDNA3-fb: started" in log
        # the framebuffer must be the same build as the core: OpenCore cannot
        # inject it (it links IOGraphicsFamily), so the VM loads the copy
        # setup.sh last put in the Auxiliary KC
        # its build id is a hash of its sources (fb/Makefile): compare with
        # this checkout, which should be the tested commit
        want = fb_source_hash()
        fb_c = re.search(r"RDNA3-fb: started .*?, build (\S+)", log)
        same = bool(want and fb_c and fb_c.group(1) == want)
        print("   %s %-20s %s" % ("PASS" if same else "FAIL", "framebuffer build",
                                 "%s = these sources" % want if same else
                                 "framebuffer %s, these sources %s: stale RDNA3Framebuffer.kext - "
                                 "'rdna3vm approve' (or the checkout is not the tested commit)" %
                                 (fb_c.group(1) if fb_c else "older than the build check", want or "?")))
        failed = failed or not same
        fb_edid = re.search(r"RDNA3FB: head \d+ EDID \d+ bytes", log)
        print("   %s %-20s %s" % ("PASS" if fb_started else "FAIL", "framebuffer",
                                 "started" if fb_started else "no 'RDNA3-fb: started' in the log"))
        print("   %s %-20s %s" % ("PASS" if fb_edid else "FAIL", "framebuffer EDID",
                                 fb_edid.group(0) if fb_edid else
                                 "never handed to IOGraphics (see the RDNA3FB lines below)"))
        for l in log.splitlines():
            if "RDNA3FB:" in l and not fb_edid:
                print("     " + l[l.find("RDNA3FB:"):][:140])
        failed = failed or not fb_started or not fb_edid
    # AGDC (rdna3agdc=1/2): IOPresentment's display-topology answers; with
    # them WindowServer should hand display-pipe transactions a plane
    # (rdna3accel_iop_ok / iop_src_surf instead of only iop_empty)
    if re.search(r"rdna3agdc=[1-4]\b", args):
        want = fb_source_hash(AGDC_HASH_FILES)
        ag = re.search(r"RDNA3-agdc: build ([0-9a-f]+)", log)
        same = bool(want and ag and ag.group(1) == want)
        print("   %s %-20s %s" % ("PASS" if same else "FAIL", "AGDC build",
                                 "%s = these sources" % want if same else
                                 "AGDC %s, these sources %s: stale or missing RDNA3AGDC.kext - 'rdna3vm approve'" %
                                 (ag.group(1) if ag else "not loaded", want or "?")))
        up = re.search(r"RDNA3-agdc: UP on .*", log)
        print("   %s %-20s %s" % ("PASS" if up else "FAIL", "AGDC object",
                                 up.group(0)[:140] if up else "never up (see the RDNA3-agdc lines below)"))
        for l in log.splitlines():
            if "RDNA3-agdc:" in l and ("NOT" in l or "failed" in l or "refused" in l or "differs" in l or
                                       "not making" in l or "slot table" in l):
                print("     " + l[l.find("RDNA3-agdc:"):][:140])
        tp = path[:-len(".tar.gz")] + "-tests.txt"
        tests = open(tp, errors="replace").read() if os.path.exists(tp) else ""
        ok = re.findall(r"debug\.rdna3accel_iop_ok: (\d+)", tests)
        ok = ok[-1] if ok else None
        # card run 48: with a Metal device WindowServer composites on our
        # plugin and presents by surface swaps the accelerator flips to
        # (zero-copy); display-pipe transactions then stay 0
        flips = len(re.findall(r"FLIPPED\*\*\* head|\*\*\* FLIPPED \*\*\*", log))
        wsmetal = (bool(re.search(r"MTLIOAccelDevice's initWithAcceleratorPort: -> 0x[0-9a-f]+", cdlog)) and
                   "NVK did not come up in this process" not in cdlog) or \
            bool(re.search(r"WindowServer\[\d+:[0-9a-f]+\] \(RDNA3MTLDriver\) NVMTL: (vk: submit census|newHeapWithDescriptor)", cdlog))
        if ok and int(ok) > 0:
            print("   PASS %-20s rdna3accel_iop_ok %s" % ("planes in transactions", ok))
        elif wsmetal and flips:
            print("   PASS %-20s WindowServer composites on the Metal plugin, %d zero-copy flip(s) logged; "
                  "display-pipe transactions %s" % ("WindowServer on Metal", flips, ok or "0"))
        else:
            unknown = sorted(set(cls for v, cls in gpu_vendors(cdlog).values() if v == 1))
            print("   FAIL %-20s rdna3accel_iop_ok %s%s%s" % ("planes in transactions", ok if ok else "not in the report",
                  "" if wsmetal else "; WindowServer has no Metal device (24-log-coredisplay.txt NVMTL lines)",
                  "; CoreDisplay classed framebuffer(s) %s vendor 1 (unknown): IOPresentment is off for the session "
                  "(DRIVER-PLAN N44)" % ",".join(unknown) if unknown else ""))
        # card run 2026-10-14: an AGDC answer CoreDisplay could not use made
        # WindowServer abort at every start (both screens black)
        # the WindowServer log is in the report archive (card run 45: the
        # tests file holds none, 0 counted while it restarted 17 times)
        wslog = wsmember + tests
        pids = set(re.findall(r"WindowServer\[(\d+):", wslog))
        print("   %s %-20s %s" % ("PASS" if len(pids) <= 2 else "FAIL", "WindowServer starts",
                                 "%d WindowServer process(es) in the report's logs%s" %
                                 (len(pids), "" if len(pids) <= 2 else ": a restart loop (23-windowserver-crash.txt)")))
        # card runs 41/45: the abort is CoreDisplay's "Failed to create
        # MetalDevice" (CoreDisplay::Display::Display) once a display pipe
        # exists; the Metal plugin must give WindowServer a device
        # ('@WindowServer' in rdna3mtl-allow.txt) while AGDC is up
        for m in sorted(set(re.findall(r"Assertion failed:[^\"\]]*|Failed to create MetalDevice[^\n]*", crashmember + cdlog)))[:4]:
            print("     " + m[:160])
        wsdev = [l for l in cdlog.splitlines() if "NVMTL:" in l and ("allow" in l or "no device" in l
                                                                      or "initWithAcceleratorPort" in l)]
        for l in wsdev[:4]:
            print("     " + l[l.find("NVMTL:"):][:150])
        # N1: which processes the Metal plugin served (apps mode or the
        # explicit list) and who it refused, from the plugin's own decisions
        if mtlallow.strip():
            served = sorted(set(re.findall(r"allow mode: [^;]*; ([^:]+): (?:apps mode|allow-listed)", mtlallow)))
            refused = sorted(set(re.findall(r"allow mode: [^;]*; ([^:]+): (?:not allow-listed|denied|'[^']*' (?:is armed-only|only with the AGDC))", mtlallow)))
            modes = sorted(set(re.findall(r"allow mode: (\w+)", mtlallow)))
            print("   INFO %-20s mode %s; device for %s; refused %s" % (
                "Metal apps mode", "/".join(modes) or "?", ", ".join(served) or "none",
                ", ".join(refused[:12]) + (" ..." if len(refused) > 12 else "") if refused else "none"))
        # A3: IOSurface textures in the accelerator's VRAM (no per-process
        # copies); informational until it has run on the card
        sv = surfvram
        if not sv.strip():
            print("   INFO %-20s no 29-surface-vram.txt (a collector before A3)" % "IOSurface VRAM (A3)")
        else:
            live = re.findall(r"item 7: IOSurface textures (live in the surface's family VRAM|private per process)[^\n]*", sv)
            gem = len(re.findall(r"GEM: IOSurface \d+ plane \d+ ->", sv))
            refused = re.findall(r"(?:surfgem|GEM: IOSurface \d+ plane \d+ \w+(?: \w+)?) refused: ([^(\n]*)", sv)
            imp_ref = re.findall(r"family VRAM import refused \(VkResult (-?\d+), kernel errno (\d+)", sv)
            # card run 58: WindowServer's display framebuffers stay out of A3
            # by design (kernel status RD3K_SS_DISPLAY, errno 1 in the plugin)
            disp_ids = set(re.findall(r"(?:surfgem|surfvram): surface (\d+) plane \d+ refused: (?:WindowServer's display "
                                      r"framebuffer|a WindowServer input|A3 is off|imported for another task|"
                                      r"a process the kernel could not name)", sv + "\n" + log))
            # the policy's refusals (card run 58: display framebuffers, then
            # every WindowServer input and every caller that might be it;
            # rdna3a3=0) are no warning
            refused = [r for r in refused if not r.startswith(A3_POLICY_REASONS)]
            # only the plugin's refusals of those very surfaces: any other
            # EPERM stays a warning
            # a plugin EPERM of a display-size surface the kernel's capped log
            # did not name counts as the policy while the kernel's counter
            # covers them all
            disp_bytes = set(int(w) * int(h) * 4 for w, h in scanout_sizes(log))
            plug = re.findall(r"surface (\d+) plane \d+: family VRAM import refused \(VkResult (-?\d+), kernel errno "
                              r"(\d+)(?:[^,\n]*, (\d+) B\))?", sv)
            unnamed = set(sid for sid, _, ke, b in plug if ke == "1" and sid not in disp_ids and b and
                          int(b) in disp_bytes)
            cnt = a3_counters(sv) or {}
            ctr = cnt.get("a3display_refused", 0)
            if unnamed and ctr >= len(unnamed | disp_ids):
                disp_ids |= unnamed
            imp_ref = [(vr, ke) for sid, vr, ke, _ in plug if not (ke == "1" and sid in disp_ids)]
            shared = sum(1 for l in live if l.startswith("live"))
            state = ("%d process(es) shared, %d private" % (shared, len(live) - shared)) if live else "no plugin line"
            warn = (shared and not gem) or refused or imp_ref
            print("   %s %-20s %s; %d kernel BO(s) logged%s" % ("WARN" if warn else "INFO", "IOSurface VRAM (A3)", state, gem,
                  "" if not warn else " -- see 29-surface-vram.txt"))
            for why in sorted(set(r.strip() for r in refused))[:4]:
                print("     kernel refused: " + why[:120])
            for vr, ke in sorted(set(imp_ref))[:4]:
                print("     plugin import refused: VkResult %s, kernel errno %s" % (vr, ke))
            for l in sorted(set(re.findall(r"item 7: kext surface BOs probe[^\n]*", sv)))[:2]:
                print("     " + l[:150])
        # card run 58: WindowServer's display framebuffers and A3
        failed = a3_display_verdict(log, sv) or failed
        failed = failed or not same or not up or len(pids) > 2
    # DRIVER-PLAN N41: the VRAM parking and the page-off's wait for WindowServer's GPU
    if "rdna3accel=1" in args or surfvram.strip():
        for lvl_, name_, text_ in n41_rows(surfvram, log, pluginlog, cdlog):
            print("   %s %-20s %s" % (lvl_, name_, text_))
            failed = failed or lvl_ == "FAIL"
    # DRIVER-PLAN N43 (1): buffer textures without an alias (bytesPerRow not a multiple of 256 B)
    for lvl_, name_, text_ in n43_buftex_rows(cdlog, pluginlog):
        print("   %s %-20s %s" % (lvl_, name_, text_))
    # DRIVER-PLAN N49 open item 1: the plugin's IOAccelCommandQueue per process (Activity Monitor's % GPU)
    for lvl_, name_, text_ in gpu_time_queue_rows(cdlog, pluginlog, log or ""):
        print("   %s %-20s %s" % (lvl_, name_, text_))
    # DRIVER-PLAN N48: the plugin's blit copies, the multisample upload, [[color(n)]] framebuffer fetch (tools/triage_n48.py)
    try:
        from triage_n48 import n48_plugin_rows
        n48_rows_ = n48_plugin_rows(pluginlog)
    except ImportError:
        n48_rows_ = [("WARN", "plugin copies", "tools/triage_n48.py is missing next to triage_reports.py: the plugin's copy / multisample / "
                      "framebuffer-fetch lines were NOT read")]
    for lvl_, name_, text_ in n48_rows_:
        print("   %s %-20s %s" % (lvl_, name_, text_))
    # DRIVER-PLAN N50: the A/B switches and counters for the glyph / tile artefacts (tools/triage_glyphs.py)
    try:
        from triage_glyphs import n50_plugin_rows
        n50_rows_ = n50_plugin_rows((cdlog or "") + "\n" + (pluginlog or ""))
    except ImportError:
        n50_rows_ = [("WARN", "plugin A/B", "tools/triage_glyphs.py is missing next to triage_reports.py: the plugin's RADV profile, load pass layout, "
                      "fence and managed shadow lines were NOT read")]
    except Exception as e_:  # noqa: BLE001  (one bad line of a report must not take the whole triage down)
        n50_rows_ = [("WARN", "plugin A/B", "tools/triage_glyphs.py FAILED (%s: %s): the plugin's A/B rows are missing from this triage; the rest of it ran" % (type(e_).__name__, e_))]
    for lvl_, name_, text_ in n50_rows_:
        print("   %s %-20s %s" % (lvl_, name_, text_))
    if ladder_txt:
        try:
            from triage_glyphs import parse_ladder, ladder_rows
            lv_, lr_, lp_ = parse_ladder(ladder_txt, ladder_logs)
            ladder_rows_ = ladder_rows(lr_, lv_, lp_)
        except ImportError:
            ladder_rows_ = [("WARN", "ladder", "tools/triage_glyphs.py is missing next to triage_reports.py: the ladder's rungs were NOT read")]
        except Exception as e_:  # noqa: BLE001
            ladder_rows_ = [("WARN", "ladder", "tools/triage_glyphs.py FAILED (%s: %s): the ladder's rows are missing from this triage; the rest of it ran" % (type(e_).__name__, e_))]
        for lvl_, name_, text_ in ladder_rows_:
            print("   %s %-20s %s" % (lvl_, name_, text_))
    # step 10: the accelerator also loads from the Auxiliary KC
    if "rdna3accel=1" in args:
        want = fb_source_hash(ACCEL_HASH_FILES)
        ac = re.search(r"RDNA3Accelerator: build (\S+)", log)
        same = bool(want and ac and ac.group(1) == want)
        print("   %s %-20s %s" % ("PASS" if same else "FAIL", "accelerator build",
                                 "%s = these sources" % want if same else
                                 "accelerator %s, these sources %s: stale RDNA3Accelerator.kext - "
                                 "'rdna3vm approve'" % (ac.group(1) if ac else "older than the build id", want or "?")))
        started = re.search(r"IOGraphicsAccelerator2::start -> (\d)", log)
        print("   %s %-20s %s" % ("PASS" if started and started.group(1) == "1" else "FAIL", "accelerator start",
                                 started.group(0) if started else "no 'IOGraphicsAccelerator2::start' in the log"))
        for l in log.splitlines():
            if "RDNA3Accelerator:" in l and ("NOT starting" in l or "never claimed" in l or
                                             "filled: AccelCaps" in l):
                print("     " + l[l.find("RDNA3Accelerator:"):][:140])
        failed = failed or not same or not (started and started.group(1) == "1")
        # the desktop through the accelerator: which head each swap went to,
        # by which strategy, and whether frames reached the panels
        shown = set()
        for l in log.splitlines():
            for key in ("swap: surface", "FLIPPED", "COPIED THE COMPOSITED", "matched no head",
                        "refused", "LIVE WRITER", "iop: K6", "boot-arg rdna3accelfb"):
                if key in l:
                    t = l[l.find("RDNA3Accelerator:"):][:150]
                    k = re.sub(r"0x[0-9a-f]+|#\d+|sample \d+", "", t)
                    if k not in shown:
                        shown.add(k)
                        print("     " + t)
    # system sleep (rdna3resume=1): what the wake brought back
    if "rdna3resume=1" in args and props.get("Wakes", 0):
        checks = [
            ("WakeHeadsLost", props.get("WakeHeadsLost", 0) == 0,
             "every head back after the wake", "head(s) still lost after the wake"),
            ("DisplayPictureLost", props.get("DisplayPictureLost", 0) == 0,
             "the picture in VRAM survived (eviction complete)",
             "the picture was lost: the eviction did not bring everything back (EvictFailedMiB, "
             "the suspend: / resume: VRAM lines); WindowServer was asked to redraw"),
        ]
        if "SleepUserQueuesActive" in props or "WakeUserQueuesActive" in props:
            a, b = props.get("SleepUserQueuesActive", 0), props.get("WakeUserQueuesActive", 0)
            checks.append(("user queues", a == b, "%d mapped before the sleep, %d after" % (a, b),
                           "%d mapped before the sleep, only %d after (amdgpu_userq_resume)" % (a, b)))
        for name, ok, good, bad in checks:
            print("   %s %-20s %s" % ("PASS" if ok else "FAIL", name, good if ok else bad))
            failed = failed or not ok
        # HPD looks after the wake (rdna3hotplug=1): an unplug right after
        # it, with nobody at the cables, means the pins were read too early
        lines = log.splitlines()
        wake_at = next((i for i, l in enumerate(lines) if "wake #" in l), None)
        if wake_at is not None:
            for l in lines[wake_at:]:
                if "hotplug: HPD" in l and " disconnected on " in l:
                    print("   WARN hotplug after the wake: " + l[l.find("hotplug:"):][:120])
    # swap copies onto a scanout by SDMA (rdna3accelsdma=1, A2)
    if "rdna3accelsdma=1" in args:
        done = "swap copy by SDMA (head" in log
        refused = log.count("swap copy by SDMA refused")
        if refused:
            print("   FAIL swap copy by SDMA  %d refused (the copy: lines of the core, the CPU took over after "
                  "three in a row)" % refused)
            failed = True
        elif done:
            print("   PASS swap copy by SDMA  the composited frames reach the scanout through SDMA")
        else:
            print("   INFO swap copy by SDMA  no copy was needed (every swap was a zero-copy flip)")
    if props.get("KernelQueuesByMESFallback", 0):
        print("   WARN kernel queues      the MES scheduler refused the legacy map; the KIQ mapped them "
              "(the MES: lines)")
    errs = [l for l in log.splitlines() if "ERROR" in l or "failed" in l or "timeout" in l]
    if errs:
        print("   kext errors (%d):" % len(errs))
        for l in errs[:25]:
            print("     " + l[-160:])
    # collect.sh prints "== <file>" per panic report (the command line
    # itself also contains "*.panic")
    if re.search(r"^== .*\.panic\s*$", panics, re.M):
        print("   !! panic report(s) present: see 21-log-panics.txt")
        failed = True
    return failed


def tail_file(path, n, pattern=None):
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        return
    if pattern:
        lines = [l for l in lines if re.search(pattern, l)]
    for l in lines[-n:]:
        print("     " + l[-160:])


TX_CHANGE = re.compile(r"display: (head \d+|clone) TX change #(\d+) at (\d+) ms uptime: problems: ([^;]*); (.*)$", re.M)
TX_LINE = re.compile(r"display: (head \d+|clone) TX|display: TX watch")


def tx_changes(log):
    """the core's TX watch changes (what a head sends after its OTG), once
    each even when a line is both in the report's log and the serial log:
    {entity: [(n, ms, problems, registers)]}"""
    seen, out = set(), {}
    for m in TX_CHANGE.finditer(log):
        ent, n = m.group(1), int(m.group(2))
        if (ent, n) in seen:
            continue
        seen.add((ent, n))
        out.setdefault(ent, []).append((n, int(m.group(3)), m.group(4).strip(), m.group(5)))
    for v in out.values():
        v.sort()
    return out


def tx_excerpt(lines):
    """the watch's lines worth reading: every full state (the start, after
    the clone, a new chain) with its register lines, the watch's own
    lines, the first change with problems, the last 20; each line of the
    log once (two dumps with the same registers are both kept)"""
    keep, first_bad, full = set(), None, None
    for i, l in enumerate(lines):
        if re.search(r"TX (at the start|after the clone|new chain|first look)|TX watch:|TX not read|TX readable again|changed since the start", l):
            keep.add(i)
            full = re.search(r"display: (head \d+|clone) TX", l)
            full = full.group(1) if full else None
        elif full and re.search(r"display: %s TX   " % re.escape(full), l):
            keep.add(i)
        else:
            full = None
        m = TX_CHANGE.search(l)
        if m and m.group(4).strip() != "none" and first_bad is None:
            first_bad = i
    if first_bad is not None:
        keep.add(first_bad)
    keep.update(range(max(0, len(lines) - 20), len(lines)))
    return [lines[i][lines[i].find("display:"):] for i in sorted(keep)]


def bisect_run_expected(path, tests_txt, args=""):
    """whether this report was a display bisect run (rdna3bisect=1 / 2 / 4 in its boot-args or in the tests file)"""
    return bool(re.search(r"rdna3bisect=[124]\b", (args or "") + "\n" + tests_txt[:2000]) or "\n== bisect" in "\n" + tests_txt)


def bisect_analysis(path, tests_txt, args=""):
    """(expected, result of tools/triage_bisect.analyse or None, [(level, text)] when there is nothing to table)"""
    expected = bisect_run_expected(path, tests_txt, args)
    base = path[:-len(".tar.gz")] if path.endswith(".tar.gz") else path
    if triage_bisect is None:
        if expected:
            return True, None, [("WARN", "bisect run, but tools/triage_bisect.py is missing next to this script: the step table is not made")]
        return False, None, []
    text = triage_bisect.locate(path)
    if not text:
        if expected:
            return True, None, [("FAIL", "bisect run (rdna3bisect in the boot-args) but no bisect.txt: neither the archive's "
                                          "39-bisect.txt nor %s-bisect.txt came - autoreport.sh's bisect_run never wrote it "
                                          "(an older autoreport.sh in the VM? no debug.rdna3accel_swap_mode?) - the tests file "
                                          "has the reason" % os.path.basename(base))]
        return False, None, []
    # the agdc variant (rdna3vm step bisect-agdc) cuts the kernel log per step: FRONT SURFACE live-writer lines
    logs = bisect_logs(path) if re.search(r"^variant: 4\b", text[:2000], re.M) else None
    return True, triage_bisect.analyse(text, base, logs, triage_bisect.locate_owner(path)), []


def bisect_logs(path):
    """[(name, text)] of the kernel logs a report has (tools/triage_bisect.py picks the one that places most of the steps)"""
    return triage_bisect.bisect_logs(path)


def bisect_print(path, tests_txt, args=""):
    """([lines to print], failed): the display bisect's table and findings"""
    expected, res, alone = bisect_analysis(path, tests_txt, args)
    lines = ["   %s bisect: %s" % (lv, t) for lv, t in alone]
    if res:
        lines.append("   display bisect (tools/triage_bisect.py; a step = 30 s of one set of switches):")
        lines.extend("     " + l for l in res["table"])
        lines.extend("   %s %s" % (lv, t) for lv, t in res["findings"])
    failed = any(l.startswith("   FAIL") for l in lines)
    return lines, failed


def bisect_rows(path, tests_txt, args=""):
    """[(name, text)] for the SUMMARY"""
    expected, res, alone = bisect_analysis(path, tests_txt, args)
    rows = [("bisect", "%s %s" % (lv, t[:300])) for lv, t in alone]
    if res:
        rows.extend(res["rows"])
    return rows


def summary(path, serial=None):
    """one line per area for the newest report: what a round's message to
    the user is made of"""
    log = args = props_txt = ""
    if os.path.exists(path):   # else only the tests file came (collect.sh did not finish)
        with tarfile.open(path) as tar:
            log = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt")
            args = read_member(tar, "01-boot-args.txt")
            props_txt = read_member(tar, "11-ioreg-props.txt") or ""
    tests = path[:-len(".tar.gz")] + "-tests.txt"
    txt = open(tests, errors="replace").read() if os.path.exists(tests) else ""
    if serial:   # the kernel log of the same boot has the whole PM trend
        log = log + "\n" + open(serial, errors="replace").read()
    out = []
    m = re.search(r"rdna3lvl=\S+.*", args)
    out.append(("boot-args", m.group(0)[:150] if m else "?"))
    nocard = no_card_boot(path)
    if nocard:
        out.append(("no-card boot", "INFO %s: every row below is INFO (nothing here is a result about the driver)" % nocard))
    if os.path.exists(path):
        for lv, label, t in present_mode_lines(path, txt, args):
            out.append(("present: " + label, "%s %s" % (lv, t[:300])))
        for lv, label, t in fb_n40_lines(path, txt, args):
            out.append(("fb N40: " + label, "%s %s" % (lv, t[:300])))
        for lv, label, t in dispirq_lines(path, txt, args):
            out.append(("N42: " + label, "%s %s" % (lv, t[:300])))
        # card run 2026-10-25c: user app crashes of the boot (INFO none, WARN per crashed process)
        for lv, label, t in crash_rows(read_tar_member(path, CRASH_MEMBER), bool(nocard)):
            out.append((label, "%s %s" % (lv, t[:300])))
        # DRIVER-PLAN N41 (ECC review): its WARN / FAIL rows, and MOOT when the page-off wait was not exercised
        for lv, label, t in n41_lines(path, args):
            if lv in ("WARN", "FAIL") or t.startswith("MOOT"):
                out.append(("N41: " + label, "%s %s" % (lv, t[:300])))
    else:   # tests only: the AGDC vendor answer still comes from the tests file
        for lv, label, t in agdc_vendor_lines(path, txt, args):
            out.append(("fb N40: " + label, "%s %s" % (lv, t[:300])))
        for lv, label, t in dispirq_lines(path, txt, args):
            out.append(("N42: " + label, "%s %s" % (lv, t[:300])))
    out.append(("DMA/GFX jobs", "%d verified, %d wrong" % (len(re.findall(r"job results verified", txt)),
                                                         len(re.findall(r"job results WRONG", txt)))))
    sec = ""
    if "== rdna3_vktest" in txt:   # the vktest section only: a later FAILED (BACO's) is not its verdict
        sec = txt[txt.index("== rdna3_vktest"):]
        sec = sec[:sec.find("\n== ", 1)] if "\n== " in sec[1:] else sec
    vk = re.search(r"^(ALL PASSED.*|NOTHING TESTED.*|.*FAILED.*)$", sec, re.M) if sec else None
    if not vk and sec:   # no verdict: the test died (a driver assert)
        dead = re.search(r"^(Assertion failed.*|.*Assertion .* failed.*)$", sec, re.M)
        out.append(("Vulkan", "CRASHED after %d PASS: %s" % (len(re.findall(r"^PASS ", sec, re.M)),
                                                             dead.group(0)[:80] if dead else "no verdict")))
    else:
        out.append(("Vulkan", vk.group(1)[:80] if vk else "not run"))
    # DRIVER-PLAN N43: the WARN / FAIL rows of the linear row pitch check and the memory load
    for lv, label, t in n43_vktest_rows(txt, " rdna3pm=2 " in " %s " % args):
        if lv in ("WARN", "FAIL"):
            out.append(("N43: " + label, "%s %s" % (lv, t[:300])))
    mt = re.search(r"^METAL: .*$", txt, re.M)
    if not mt and "== mtlprobe" in txt:   # no verdict: the probe died (a driver assert)
        mt = re.search(r"^(Assertion failed.*|.*Assertion .* failed.*|\[exit [1-9]\d*\])$",
                       txt[txt.index("== mtlprobe"):], re.M)
        out.append(("Metal", "CRASHED: " + mt.group(0)[:100] if mt else "no verdict"))
    else:
        out.append(("Metal", mt.group(0)[:80] if mt else "not run"))
    for blk in txt.split("== rdna3_drminfo --scanout")[1:]:
        look = re.match(r"(?:-hires)?\s*\(look (\S+)\)", blk)
        if "display asleep:" in blk.split("\n== ")[0]:
            out.append(("screens %s" % (look.group(1) if look else "1"),
                        "NOT READ: the display was asleep (system sleep or shutdown)"))
            continue
        if "pipe(s) lit" not in blk.split("\n== ")[0]:
            continue   # a picture-only look (M6 keeps only the pictures)
        lit = re.findall(r"^pipe (\d+): LIT .* in use (0x[0-9a-f]+)", blk, re.M)
        blank = len(re.findall(r"^pipe \d+: BLANKED", blk, re.M))
        uf = len(re.findall(r"UNDERFLOW", blk))
        same = len(lit) > 1 and len(set(a for _, a in lit)) < len(lit)
        own = blk.split("\n== ")[0]
        txbad = [m for m in re.findall(r"^  TX problems: (.*)$", own, re.M) if m.strip() != "none"]
        notx = lit and not re.search(r"^  TX ", own, re.M)
        pics = look_pictures(own)
        black = sorted(p for p, (pct, _) in pics.items() if pct is not None and pct <= BLACK_PCT)
        out.append(("screens %s" % (look.group(1) if look else "1"),
                    "%d lit%s%s%s%s%s%s%s" % (len(lit), ", SAME SURFACE on two pipes" if same else "",
                                              ", %d blanked" % blank if blank else "", ", UNDERFLOW" if uf else "",
                                              ", TX: " + "; ".join(txbad) if txbad else "",
                                              ", NO TX LINES" if notx else "",
                                              "".join(", pipe %s %d%% not black" % (p, pics[p][0]) if pics[p][0] is not None
                                                      else ", pipe %s picture NOT READABLE (not judged)" % p
                                                      for p in sorted(pics)),
                                              ", BLACK pipe(s) %s" % " ".join(black) if black else "")))
    out.extend(ab_pictures(txt, main_pipe(path, txt)))
    out.extend(bisect_rows(path, txt, args))
    abd = ab_diag_row(txt)
    if abd:
        out.append(abd)
    # the core's TX watch (what each head sends after its OTG): changes
    # with their time, problems first, from the kernel log of the same boot
    if "TX watch:" in log or "TX at the start" in log:
        ch = tx_changes(log)
        pr = parse_props(props_txt) if props_txt else {}
        ticks, skips = pr.get("TXWatchTicks"), pr.get("TXWatchBusySkips")
        ticked = "TX watch: first tick" in log or (isinstance(ticks, int) and ticks > 0)
        busy = (isinstance(skips, int) and skips > 0) or "TX watch: skipped, display lock busy" in log
        parts = []
        for ent, v in sorted(ch.items()):
            bad = [c for c in v if c[2] != "none"]
            c = bad[0] if bad else v[0]
            parts.append("%s: %d change(s); first %s at %d ms: problems: %s; %s" %
                         (ent, len(v), "with problems" if bad else "", c[1], c[2], c[3][:160]))
        if "TX watch: NOT started" in log:
            out.append(("TX watch", "NOT STARTED: no memory for its state"))
        elif parts:
            out.append(("TX watch", " | ".join(parts)))
        elif "TX watch: off" in log:
            out.append(("TX watch", "off (rdna3txwatch=0)"))
        elif not ticked and busy:
            out.append(("TX watch", "NO TICK: every tick skipped, the display lock was busy (%s skip(s))" %
                        (skips if isinstance(skips, int) else "some")))
        elif ticked:
            out.append(("TX watch", "no change on any head after the start (%s tick(s))" %
                        (ticks if isinstance(ticks, int) else "some")))
        else:
            out.append(("TX watch", "NEVER TICKED: no change seen proves nothing (the statistics tick did not run)"))
        if "TX watch: could not publish" in log:
            out.append(("TX watch publish", "FAILED: the RDNA3,Head<n>,TX properties are stale (see the log)"))
    # the Vulkan CTS subset (step cts): the total, the groups that failed or
    # hung, the first failing cases
    cts = re.search(r"^CTS: .*$", txt, re.M)
    if cts:
        out.append(("Vulkan CTS", cts.group(0)[5:][:100]))
        bad = re.findall(r"^CTS (dEQP\S+): (pass \d+, fail [1-9]\d*.*|no totals.*)$", txt, re.M)
        for g, v in bad[:8]:
            out.append(("  " + g[:40], v[:70]))
        for c in re.findall(r"^    FAIL '([^']+)'", txt, re.M)[:10]:
            out.append(("  failed case", c[:90]))
    elif "== Vulkan CTS: no deqp-vk bundle" in txt:
        out.append(("Vulkan CTS", "not run: no deqp-vk bundle in the VM (rdna3vm cts)"))
    # the card-only plan items (rdna3probe=1, step big4)
    for pat, name in ((r"^PLATFORM: (.*)$", "R3 platform"), (r"^R4: (.*)$", "R4 macOS"), (r"^WSI: (.*)$", "V3 Vulkan WSI"),
                      (r"^A3: ((?:PASSED|FAILED).*)$", "A3 IOSurface"), (r"^M4: ((?:PASSED|FAILED|NO PRESENT CALLBACK).*)$", "M4 CAMetalLayer"),
                      (r"^M5: (.*)$", "M5 WindowServer"), (r"^M6: (.*)$", "M6 apps")):
        m = re.search(pat, txt, re.M)
        if m:
            out.append((name, m.group(1)[:110]))
    # lsof cut the name to "WindowSer" (reports before card run 52)
    if re.search(r"^processes with the plugin loaded:.*\bWindowSer\w*\(", txt, re.M):
        for i, (k, v) in enumerate(out):
            if k == "M5 WindowServer" and "did not load" in v:
                out[i] = (k, v.replace("did not load", "LOADED"))
    for m in re.finditer(r"^M6 (\S+): (.*)$", txt, re.M):
        out.append(("  " + m.group(1), m.group(2)[:100]))
    m6 = m6_verdict(path[:-len(".tar.gz")], txt, main_pipe(path, txt))
    if m6:
        out.append(("M6 screens", "; ".join(sh for _, _, sh in m6)))
    elif m6_cut(txt):
        out.append(("M6 screens", "NOT FINISHED: the run stopped inside M6 (the VM's time limit?), no pictures"))
    elif "(look m6" in txt:
        out.append(("M6 screens", "no pictures: the M6 looks' PPMs did not arrive"))
    a3c = a3_counters(txt)
    if a3c is None:
        out.append(("A3 display counters", "none (bisect run: the counters are per step in the bisect table)"
                    if bisect_run_expected(path, txt, args) else "none: the kext predates card run 58"))
    elif a3c:
        fl = flicker_verdict(txt)
        for lv, t in fl:
            out.append(("flicker", "%s %s" % (lv, t)))
        # the kernel logged A B A B, yet the share stayed under the line
        if re.search(r"flicker: head \d+: swap \d+ showed alternating pictures", log) and \
                not any(lv in ("FAIL", "WARN") for lv, _ in fl):
            out.append(("flicker", "WARN the kernel logged alternating pictures (flicker: lines) under %.0f%% of "
                                   "the hashed swaps" % (FLICKER_SHARE * 100)))
        for lv, t in copy_forward_verdict(a3c):
            out.append(("copy forward", "%s %s" % (lv, t)))
        out.append(("A3 display counters", "refused %s, imported %s, other %s (kept out %s); swaps %s" % (
            a3c.get("a3display_refused", "?"), a3c.get("a3display_imported", "?"), a3c.get("a3display_other", "?"),
            a3c.get("a3ws_refused", "?"),
            ", ".join("h%s %d (A3 %d)" % (k[-1], v, a3c.get("swaps_a3_h" + k[-1], 0))
                      for k, v in sorted(a3c.items()) if re.match(r"swaps_h\d$", k) and v))))
    for lv, t in vram_heap_lines(path, txt, log):
        if lv != "INFO":
            out.append(("VISIBLE VRAM", "%s %s" % (lv, t[:360])))
    for lv, t in latch_lines(path, txt, args):
        out.append(("flip latch", "%s %s" % (lv, t)))
    for lv, t in swapcopy_verdict(a3_counters(txt) if txt else {}):
        if lv != "INFO" or t.startswith("swap mode") or "H-B2 handled" in t or "H-STALE handled" in t or \
                ": stale back (swap_stale" in t:
            out.append(("swap copy", "%s %s" % (lv, t)))
    for lv, t in gen_verdict(a3_counters(txt) if txt else {}, accel_current(log)):
        if lv != "INFO" or t.startswith(("write generations", "flip-present")) or ": write generations:" in t:
            out.append(("swap gen", "%s %s" % (lv, t[:300])))
    tlv, _ = trace_lines(path, txt, log, args)
    for lv, t in tlv:
        if lv in ("FAIL", "CRITICAL", "WARN") or t.startswith("A/B") or \
                t.startswith(("H-B2", "H-STALE", "A-B-A", "H-GEN", "H-FLIPPATH")):
            out.append(("frame trace", "%s %s" % (lv, t[:300])))
    cdl = read_tar_member(path, "24-log-coredisplay.txt")
    for lv, t in coredisplay_cf_lines(cdl, tlv):
        out.append(("copy forward", "%s %s" % (lv, t[:300])))
    for lv, t in plugin_trace_lines(path, cdl, txt):
        if lv in ("FAIL", "CRITICAL", "WARN") or t.startswith("join:"):
            out.append(("plugin trace", "%s %s" % (lv, t[:300])))
    blanked = sorted(set(re.findall(r"display: head (\d+) blanked \(display sleep\)", log)))
    if "look sleep" in txt and "display asleep:" in txt.split("(look sleep)", 1)[-1][:600]:
        out.append(("display sleep", "the look found the system asleep; heads blanked first (kernel log): %s" %
                    (", ".join(blanked) or "NONE")))
    # the serial log when there is one: the unified log may carry the same lines
    faults = vm_fault_groups(open(serial, errors="replace").read() if serial else log)
    if faults:
        out.append(("VM faults", "%d in %d place(s)" % (sum(f[0] for f in faults), len(faults))))
        for n, hub, vmid, addr, who, client in faults[:6]:
            out.append(("  VMID %d" % vmid, "%dx %s at %s%s: %s" % (n, hub, addr, " (%s)" % client if client else "",
                                                                     who)[:150]))
    if "== rdna3_drminfo --scanout (look m4)" in txt:
        pics = sorted(glob.glob(path[:-len(".tar.gz")] + "-scanoutm4-pipe*.ppm"))
        seen = []
        for ppm in pics:
            c = count_color(ppm, (255, 0, 255))
            if c:
                seen.append("%s %d of %d" % (os.path.basename(ppm).split("-")[-1][:-4], c[0], c[1]))
        best = max([count_color(p_, (255, 0, 255))[0] for p_ in pics if count_color(p_, (255, 0, 255))] or [0])
        # card run 49: the window was on the screen (the user saw it pink)
        # while mtlprobe counted no presented callback: with WindowServer
        # presenting by flips none is delivered; the screen decides
        out.append(("M4 on the screen", "magenta pixels: " + (", ".join(seen) if seen else "no picture") +
                    (" -> PASS: the CAMetalLayer window is on the screen" if best > 1000 else "")))
        # mtlprobe's own verdict waits for presented callbacks, which the
        # flip path never delivers (card runs 49-51: the user saw the window
        # while the line said FAILED): the screen decides
        if best > 1000:
            for i, (k, v) in enumerate(out):
                if k == "M4 CAMetalLayer" and (v.startswith("FAILED") or v.startswith("NO PRESENT")):
                    out[i] = (k, "PASS on the screen (mtlprobe: " + v.split(" ", 1 if v.startswith("FAILED") else 3)[-1][:80] + ")")
    # P2: the PM experiment's variants, the RLC state after each
    lines = log.splitlines()
    for i, l in enumerate(lines):
        m = re.search(r"PM experiment (\d) \(([^)]*)\): activity (\d+)%.*?(\d+) W", l)
        if not m:
            continue
        rlc = next((x for x in lines[i + 1:i + 4] if "PM: RLC_STAT" in x), "")
        g = re.search(r"GPM_STAT (0x[0-9a-f]+) \(pwr (\d) clk (\d) ls (\d)\)", rlc)
        out.append(("P2 variant %s" % m.group(1), "%s: activity %s%%, %s W%s" % (
            m.group(2)[:28], m.group(3), m.group(4),
            ", GPM_STAT %s (pwr %s clk %s ls %s)" % g.groups() if g else "")))
    if re.search(r"^GAMMA: UNJUDGED", txt, re.M):
        out.append(("gamma (open)", "%s %s" % gamma_unjudged(txt)))
    for pat, name in ((r"^MODES: .*$", "scaled modes"), (r"^GAMMA: .*$", "gamma"), (r"^SOAK: .*$", "soak"),
                      (r"^HANG TEST: .*$", "hang test"), (r"^VCN TEST: .*$", "video (VCN)"),
                      (r"^SURFXPROC: .*$", "IOSurface isolation")):
        m = re.search(pat, txt, re.M)
        if m:
            out.append((name, m.group(0)[:100]))
    pm = [l[l.find("PM: activity"):] for l in log.splitlines() if "PM: activity" in l]
    if pm:
        out.append(("power (last)", pm[-1][:110]))
    exp = [l for l in log.splitlines() if "PM experiment" in l and " W," in l]
    if exp:
        best = min(exp, key=lambda l: int(re.search(r"(\d+) W,", l).group(1)))
        out.append(("lowest-power variant", best[best.find("PM experiment"):][:150]))
    caps = [l for l in log.splitlines() if "GFXCLK capped" in l]
    if caps:
        out.append(("idle cap", "%d cap(s), e.g. %s" % (len(caps), caps[0][caps[0].find("PM:"):][:80])))
    if nocard:   # a boot without the card: the Vulkan / Metal / screens / trace rows are not about the driver
        out = [(k, v) if k in ("boot-args", "no-card boot") else
               (k, NOCARD_LEVEL_RE.sub(r"\1INFO", v) if re.match(r"(?:FAIL|WARN|CRITICAL|INFO|PASS)\b", v)
                else "INFO (no-card boot) " + v)
               for k, v in out]
    print("=" * 78)
    print("SUMMARY of the newest report (%s)" % os.path.basename(path))
    for k, v in out:
        print("  %-22s %s" % (k, v))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--picture":
        for p in sys.argv[2:]:
            print(ppm_to_png(p))
        return
    d = d0 = sys.argv[1]
    # a collected 'rdna3vm result' archive unpacks to <dir>/rdna3-result/
    if not glob.glob(os.path.join(d, "rdna3-report-*")) and \
            glob.glob(os.path.join(d, "rdna3-result", "rdna3-report-*")):
        d = os.path.join(d, "rdna3-result")
    show_all = "--all" in sys.argv
    # linux-reference/ sits next to rdna3-result/ (docs/AFTER-TESTS.md intake)
    load_linux_reference(d0)
    if d != d0:
        load_linux_reference(d)
    failed = False
    # first of all: a run that tested the old driver (approve marker / serial build line) says so before anything
    # else, also when macOS never sent a report
    global STALE_KEXTS_SHOWN
    for lv, t in stale_kext_lines(d, d0, os.path.join(d0, "rdna3-result")):
        print("   %s %-20s %s" % (lv, "STALE KEXTS" if lv == "FAIL" else "stale check", t))
        if lv == "FAIL":
            STALE_KEXTS_SHOWN = failed = True
    reports = glob.glob(os.path.join(d, "rdna3-report-*.tar.gz"))
    # a tests file whose archive never came (collect.sh killed): its results
    # still count, under the archive's would-be name
    for t in glob.glob(os.path.join(d, "rdna3-report-*-tests.txt")):
        p = t[:-len("-tests.txt")] + ".tar.gz"
        if p not in reports:
            reports.append(p)
    reports.sort(key=lambda p: os.path.getmtime(p if os.path.exists(p) else p[:-len(".tar.gz")] + "-tests.txt"))
    if not reports:
        print("!! no rdna3-report-*.tar.gz: macOS never sent a report (did not boot far enough,")
        print("   no network to the host, or the VM was stopped before ~2 min) - read the serial log")
        failed = True
        # the host log alone still says what rdna3vm asked for and where the card sat in the guest
        newest = sorted(glob.glob(os.path.join(d, "host-*.log")), key=os.path.getmtime)
        hl = host_log_for(os.path.join(d, "rdna3-report-none.tar.gz"))
        if newest:
            hl = (hl[0], "matched", "", hl[3])    # no report to match by: the newest log is the run
        for lv, label, t in present_host_rows(*hl):
            print("   %s present mode, %s: %s" % (lv, label, t))
    def one_report(p):
        global STALE_KEXTS_SHOWN
        nonlocal failed
        if os.path.exists(p):
            failed |= report(p, show_all)
        else:
            print("=" * 78)
            print(os.path.basename(p)[:-len(".tar.gz")] + ": TESTS ONLY - the archive never came "
                  "(collect.sh did not finish in time); the kernel log is in the serial log")
            failed = True
        tests = p[:-len(".tar.gz")] + "-tests.txt"
        # card run 2026-10-17: the flip latch, from the tests file or the archive
        ttxt = open(tests, errors="replace").read() if os.path.exists(tests) else ""
        # card run 2026-10-17c: who holds the CPU-visible VRAM, and what was refused
        for lv, t in vram_heap_lines(p, ttxt, kernel_text(p, d)):
            print("   %s VISIBLE VRAM: %s" % (lv, t))
            failed = failed or lv == "FAIL"
        for lv, t in latch_lines(p, ttxt):
            print("   %s flip latch: %s" % (lv, t))
            failed = failed or lv == "FAIL"
        # card run 2026-10-17d: page flips instead of transactions; the facts that could decide it
        if os.path.exists(p):
            for lv, label, t in present_mode_lines(p, ttxt, read_tar_member(p, "01-boot-args.txt")):
                print("   %s present mode, %s: %s" % (lv, label, t))
            # DRIVER-PLAN N40: the framebuffer's A/B switches and counters, the AGDC vendor answer
            for lv, label, t in fb_n40_lines(p, ttxt, read_tar_member(p, "01-boot-args.txt")):
                print("   %s fb N40, %s: %s" % (lv, label, t))
            # DRIVER-PLAN N42: the core's vblank / pflip / HPD interrupts
            for lv, label, t in dispirq_lines(p, ttxt):
                print("   %s N42, %s: %s" % (lv, label, t))
            # card run 2026-10-25c: user app crashes (collect.sh's 40-app-crashes.txt)
            for lv, label, t in crash_rows(read_tar_member(p, CRASH_MEMBER), bool(no_card_boot(p))):
                print("   %s %s: %s" % (lv, label, t))
        else:
            # tests only: the archive's CoreDisplay / ioreg logs are missing, the host log still answers
            targs = (re.search(r"^boot-args: (.*)$", ttxt, re.M) or [None, ""])[1]
            bare = bool(re.search(r"^PLATFORM: .*, bare metal, ", ttxt, re.M))
            for lv, label, t in present_host_rows(*host_log_for(p, targs), bare=bare):
                print("   %s present mode, %s: %s" % (lv, label, t))
            # the AGDC vendor answer is in the tests file (autoreport.sh's rdna3agdc grep)
            for lv, label, t in agdc_vendor_lines(p, ttxt, targs):
                print("   %s fb N40, %s: %s" % (lv, label, t))
            # DRIVER-PLAN N42: debug.rdna3_dispirq is in the tests file (autoreport.sh)
            for lv, label, t in dispirq_lines(p, ttxt):
                print("   %s N42, %s: %s" % (lv, label, t))
        # card run 2026-10-17b: the swap as a copy, and the per-frame trace
        for lv, t in swapcopy_verdict(a3_counters(ttxt) if ttxt else {}):
            print("   %s swap copy: %s" % (lv, t))
            failed = failed or lv == "FAIL"
        klog = kernel_text(p, d)
        # common/rdna3_tcall.h: thread call stops (card run 2026-10-25b, DRIVER-PLAN N47)
        for lv, label, t in tcall_rows(klog):
            print("   %s %s: %s" % (lv, label, t))
            failed = failed or lv == "FAIL"
        for lv, label, t in bomem_rows(klog):
            print("   %s %s: %s" % (lv, label, t))
            failed = failed or lv == "FAIL"
        for lv, t in gen_verdict(a3_counters(ttxt) if ttxt else {}, accel_current(klog)):
            print("   %s swap gen: %s" % (lv, t))
            failed = failed or lv == "FAIL"
        tl_lv, tl = trace_lines(p, ttxt, klog)
        for lv, t in tl_lv:
            print("   %s frame trace: %s" % (lv, t))
            failed = failed or lv in ("FAIL", "CRITICAL")
        cd = ""
        if os.path.exists(p):
            try:
                with tarfile.open(p) as tar:
                    cd = read_member(tar, "24-log-coredisplay.txt") or ""
            except (OSError, tarfile.TarError):
                pass
        for lv, t in coredisplay_cf_lines(cd, tl_lv):
            print("   %s copy forward: %s" % (lv, t))
            failed = True
        for lv, t in plugin_trace_lines(p, cd, ttxt):
            print("   %s %s" % (lv, t if t.startswith("plugin trace") else "plugin trace: " + t))
            failed = failed or lv in ("FAIL", "CRITICAL")
        bl, bfail = bisect_print(p, ttxt, read_tar_member(p, "01-boot-args.txt"))
        for l in bl:
            print(l)
        failed = failed or bfail
        if tl:
            print("   frame trace timeline (tools/rdna3_trace.py for all of it):")
            for l in tl:
                print("     " + l[:220])
        if os.path.exists(tests):
            txt = open(tests, errors="replace").read()
            # 'rdna3vm approve --step N' when the run asked for a Mac model (@smbios= in its host log)
            acmd = approve_command(host_log_for(p, (re.search(r"^boot-args: (.*)$", txt, re.M) or [None, ""])[1])[0])
            stale = re.search(r"^stale: (.*)$", txt, re.M)
            skipped = re.findall(r"^== (\S+): SKIPPED - passed on ([0-9-]+ [0-9:]+)", txt, re.M)
            if skipped:
                print("   note: %d test(s) not run again, passed before with the same builds (their saved results "
                      "below): %s" % (len(skipped), ", ".join("%s (%s)" % k for k in skipped)))
            # a non-empty 'stale:' line alone (hashes of the kext macOS ran and of the one installed) proves the old
            # driver ran, whatever kernelmanagerd logged; one FAIL per run
            sk = STALE_LINE_RE.findall(stale.group(1)) if stale else []
            if sk and not STALE_KEXTS_SHOWN:
                print("   FAIL %-20s %s" % ("STALE KEXTS", "; ".join(stale_marker_text(*x, cmd=acmd) for x in sk)))
                STALE_KEXTS_SHOWN = failed = True
            if "not approved to load" in txt and stale and not stale.group(1).strip():
                # the builds running are the ones installed (same source
                # hashes): only a relinked copy of the same sources waits
                # (card run 45: a CI rebuild reinstalled), the results count
                print("   note: kernelmanagerd waits for approval of a relinked copy of the running "
                      "sources only ('stale:' empty); the results count")
            elif "not approved to load" in txt:
                print("   !! a new RDNA3Framebuffer/RDNA3Accelerator waits for approval in macOS: run "
                      "'%s' (System Settings -> Privacy & Security -> Allow)" % acmd)
                # the ones approved earlier ran instead: what this run says
                # about the display, the accelerator and Metal on screen is
                # about those (2026-10-12: an older framebuffer in console
                # mode, every frame copied by the CPU)
                if not STALE_KEXTS_SHOWN:
                    print("   FAIL STALE KEXTS this run used the framebuffer/accelerator approved earlier, not "
                          "this build's: display, accelerator and Metal-on-screen results do not count; "
                          "'%s', then the step again" % acmd)
                failed = True
            if "== rdna3_drminfo --scanout" in txt:
                # what the panels showed: per pipe the HUBP state, how much
                # of the picture is not black, whether it changed in 2 s;
                # one block per look (20 s apart), and whether two lit pipes
                # scan the same surface (a mirrored picture)
                print("   screens (what each display pipe scanned out):")
                for blk in txt.split("== rdna3_drminfo --scanout")[1:]:
                    look = re.match(r"(?:-hires)?\s*\(look (\w+)\)", blk)
                    addrs = []
                    txl = 0
                    print("     look %s:" % (look.group(1) if look else "1"))
                    for l in blk.splitlines()[1:]:
                        if l.startswith("=="):
                            break
                        if l.startswith(("pipe", "  picture", "  UNDERFLOW", "  OTG CRC", "[exit")) and "power-gated off" not in l:
                            print("       " + l[:160])
                        # what the head sends after its OTG (RD_NR_XNU_TXSTATE):
                        # the registers in full, the problems flagged
                        elif l.startswith("  TX "):
                            print("       " + l)
                            txl += 1
                            if l.startswith("  TX problems: ") and l.strip() != "TX problems: none":
                                print("       !! the pixels leave the OTG, but its output is wrong: " + l[15:])
                        m = re.match(r"pipe (\d+): LIT .* in use (0x[0-9a-f]+)", l)
                        if m:
                            addrs.append(m.group(2))
                    if len(addrs) > 1 and len(set(addrs)) < len(addrs):
                        print("       !! two lit pipes scan the same surface: both monitors show one picture")
                    if addrs and not txl and "pipe(s) lit" in blk.split("\n== ")[0]:
                        print("       !! lit pipe(s) but no TX lines: what leaves the OTG was not read "
                              "(a kext or rdna3_drminfo without RD_NR_XNU_TXSTATE?)")
                    # display sleep (rdna3dpms=1): blanked while asleep, lit after the wake
                    if look and look.group(1) == "sleep":
                        nb = len(re.findall(r"^pipe \d+: BLANKED", blk, re.M))
                        if not nb and re.search(r"^display asleep: \d+ pipe\(s\) not read", blk, re.M):
                            # card run 58 (2026-10-15d): the system went on into
                            # sleep after the display; the core's "head N
                            # blanked (display sleep)" lines (serial log) say
                            # whether the heads blanked first
                            klog = kernel_text(p, d)
                            bl = sorted(set(re.findall(r"display: head (\d+) blanked \(display sleep\)", klog)))
                            if bl:
                                print("       INFO display sleep: not judged by the look (the system was asleep); "
                                      "the kernel blanked head(s) %s first" % ", ".join(bl))
                            elif not klog.strip():
                                # no 20-log-rdna3.txt (the run cut before the
                                # archive) and no serial log: nothing says no
                                print("       INFO display sleep: not judged: the look found the system asleep and "
                                      "there is no kernel log to judge whether a head blanked first")
                            else:
                                print("       FAIL display sleep: never blanked a head (no 'display: head N blanked "
                                      "(display sleep)' line in the kernel log), and the look found the system asleep")
                                failed = True
                        else:
                            print("       %s display sleep: %d pipe(s) blanked" % ("PASS" if nb else "FAIL", nb))
                            failed = failed or not nb
                    if look and look.group(1) == "wake":
                        nl, nb = len(addrs), len(re.findall(r"^pipe \d+: BLANKED", blk, re.M))
                        print("       %s display wake: %d pipe(s) lit, %d still blanked" %
                              ("PASS" if nl and not nb else "FAIL", nl, nb))
                        failed = failed or not nl or nb > 0
            if m6_cut(txt):
                print("   WARN M6: NOT FINISHED - the run stopped inside M6 (the VM's time limit?): its pictures and "
                      "counters are missing")
            for lv, t, _ in m6_verdict(p[:-len(".tar.gz")], txt, main_pipe(p, txt)):
                print("   %s M6 screen: %s" % (lv, t))
                failed = failed or lv == "FAIL"
            # card run 58's counters (the dumps of autoreport.sh, the last wins)
            a3c = a3_counters(txt)
            if a3c is None:
                print("   INFO A3 display counters: none (%s)" % (
                    "bisect run: the counters are per step in the bisect table" if bisect_run_expected(p, txt)
                    else "the kext predates card run 58"))
            elif a3c:
                print("   INFO A3 display counters: " + ", ".join("%s %d" % kv for kv in sorted(a3c.items())))
                for lv, t in flicker_verdict(txt):
                    print("   %s flicker: %s" % (lv, t))
                    failed = failed or lv == "FAIL"
                if a3c.get("a3display_imported") and any(v for k, v in a3c.items() if k.startswith("swaps_h")) and \
                        not any(v for k, v in a3c.items() if k.startswith("swaps_a3_h")):
                    print("   FAIL A3 display: %d display surface(s) imported (rdna3a3display=1) and no legacy swap "
                          "ever scanned one out" % a3c["a3display_imported"])
                    failed = True
            for ppm in sorted(glob.glob(p[:-len(".tar.gz")] + "-scanout*-pipe*.ppm")):
                try:
                    print("     picture: " + ppm_to_png(ppm))
                except (OSError, ValueError) as e:
                    print("     picture: " + str(e))
            if "== rdna3_modetest list" in txt:
                # scaled modes: what macOS listed, each switch, the pipes after it
                ml = txt[txt.index("== rdna3_modetest list"):]
                print("   display modes (rdna3modes=1):")
                for l in ml.splitlines():
                    if re.match(r"(display \d|  current|== mode|MODES:)", l) or \
                       ("mode " in l and re.search(r"\d+x\d+ \(pixels", l) and "IO flags" in l and l.startswith("  mode")):
                        print("     " + l[:150])
                    if l.startswith("== rdna3_drminfo --hang-test") or l.startswith("SOAK") or \
                       l.startswith("== rdna3_modetest gamma"):
                        break
                m = re.search(r"^MODES: (\d+) switch\(es\) tried, (\d+) failed", ml, re.M)
                if m and m.group(2) != "0":
                    failed = True
            m = re.search(r"^GAMMA: (\w+).*$", txt, re.M)
            if m:
                # output gamma (on unless rdna3gamma=0): the OTG CRC per pipe after each table
                print("   " + m.group(0)[:150])
                for l in re.findall(r"^== gamma pipe .*$", txt, re.M):
                    print("     " + l[3:170])
                failed = failed or m.group(1) == "FAIL"
                if m.group(1) == "UNJUDGED":
                    lv, t = gamma_unjudged(txt)
                    print("   %s gamma: %s" % (lv, t))
                    failed = failed or lv == "FAIL"
            # A3: another process's IOSurface refused (rdna3_drminfo --surface-xproc)
            m = re.search(r"^SURFXPROC: (\w+).*$", txt, re.M)
            if m:
                print("   " + m.group(0)[:160])
                failed = failed or m.group(1) == "FAIL"
            m = re.search(r"^SOAK: (\d+) rounds, (\d+) failed", txt, re.M)
            if m:
                print("   %s soak: %s rounds of DMA/GFX/Vulkan tests, %s failed" %
                      ("PASS" if m.group(2) == "0" else "FAIL", m.group(1), m.group(2)))
                failed = failed or m.group(2) != "0"
            if "== rdna3_drminfo --hang-test" in txt:
                ht = txt[txt.index("== rdna3_drminfo --hang-test"):]
                print("   hang test (a DMA and a GFX job that never finish, then normal jobs):")
                for l in ht.splitlines()[1:]:
                    if l.startswith("=="):
                        break
                    if "hang job" in l or "verified" in l or "WRONG" in l or "HANG TEST" in l or l.startswith("[exit"):
                        print("     " + l[:150])
                if "HANG TEST: PASSED" not in ht:
                    failed = True
            if "--dma-test" in txt or "--gfx-test" in txt:
                print("   tests:")
                for l in txt.splitlines():
                    if l.startswith("==") or "verified" in l or "[exit" in l or "fail" in l.lower():
                        print("     " + l)
                # the probes' own exits do not decide: V3 records that RADV has
                # no present path on macOS, M4 is decided by the screen in the
                # summary (mtlprobe gets no presented callback on the flip
                # path; card runs 49-53 counted that as a failure)
                cur = ""
                for l in txt.splitlines():
                    if l.startswith("== probe:"):
                        cur = l
                    elif l.startswith("== ") and not l.startswith("== rdna3_drminfo --scanout") and \
                            not l.startswith("== plugin log") and not l.startswith("== using"):
                        cur = ""
                    if re.match(r"\[exit [1-9]", l) and not ("(V3)" in cur or "(M4)" in cur):
                        failed = True
                # M6 exits 0 either way: an app that crashed on the plugin is a
                # failure (card run 56: Chess crashed, the result said passed)
                m6 = re.search(r"^M6: (\d+) of (\d+) apps", txt, re.M)
                if m6 and int(m6.group(1)) < int(m6.group(2)):
                    print("   FAIL M6: " + m6.group(0))
                    for l in re.findall(r"^M6 \S+: .*crash report: /.*$", txt, re.M):
                        print("     " + l[:150])
                    failed = True
            if "== rdna3_vktest" in txt:
                vk = txt[txt.index("== rdna3_vktest"):]
                print("   vulkan (RADV):")
                for l in vk.splitlines()[1:]:
                    if l.startswith("==") and "rdna3_vktest" not in l:
                        break
                    if re.match(r"(PASS|FAIL|device|physical devices|ALL PASSED|FAILED|NOTHING|\[exit)", l):
                        print("     " + l[:150])
                if not re.search(r"^ALL PASSED", vk, re.M):
                    failed = True
            # DRIVER-PLAN N43: the linear row pitch rule (LINPITCH) and the UCLK reading under a memory load
            for lv_, nm_, t_ in n43_vktest_rows(txt, " rdna3pm=2 " in " %s " % (re.search(r"^boot-args: (.*)$", txt, re.M) or [None, ""])[1]):
                print("   %s %-20s %s" % (lv_, nm_, t_))
                failed = failed or lv_ == "FAIL"
            if "== mtlprobe" in txt:
                mt = txt[txt.index("== mtlprobe"):]
                print("   metal:")
                print("     " + mt.splitlines()[0][:150])
                for l in mt.splitlines()[1:]:
                    if l.startswith("== ") and not re.match(r"== (compile|pipeline|dispatch|using|render|plugin log)", l):
                        break
                    if re.match(r"(device |  families|\d+ Metal|== |  \d+ words|  \d+ red|  status|METAL:|\[exit)", l) or \
                       re.search(r"FAIL|refus|not allow|dlopen|error", l, re.I):
                        print("     " + l[:150])
                if "plugin not installed" not in mt and not re.search(r"^METAL: ALL PASSED", mt, re.M):
                    failed = True
    for p in reports:
        if not no_card_boot(p):
            one_report(p)
            continue
        # a boot without the card: its rows are printed as INFO and it does not fail the run
        before, stale_before, buf = failed, STALE_KEXTS_SHOWN, io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                one_report(p)
        finally:
            # the rows printed before an exception are not lost; a no-card report neither fails the run nor
            # uses up the one STALE KEXTS FAIL a later real report must still print
            failed, STALE_KEXTS_SHOWN = before, stale_before
            sys.stdout.write(demote_nocard(buf.getvalue()))
    serials = sorted(glob.glob(os.path.join(d, "serial-*.log")), key=os.path.getmtime)
    hosts = sorted(glob.glob(os.path.join(d, "host-*.log")), key=os.path.getmtime)
    for s in serials:
        print("=" * 78)
        print(os.path.basename(s), "(RDNA3 lines and panics, last 15)")
        tail_file(s, 15, r"RDNA3|panic|Panic|ERROR")
        # what each head sent after its OTG at the start, after the clone
        # and every change since (the core's TX watch, hw/dcn32_txstate.c)
        try:
            tx = [l for l in open(s, errors="replace").read().splitlines() if TX_LINE.search(l)]
        except OSError:
            tx = []
        if tx:
            ex = tx_excerpt(tx)
            print("   TX watch (%d line(s); the full states, the first change with problems, the last 20):" % len(tx))
            for l in ex:
                print("     " + l)
        # the newest serial log only (older boots' logs are history), and
        # only when no report carries the flicker counters: a serial log is
        # a snapshot from the middle of the run, the report's counters the
        # end (review of e2bf78e)
        if s != serials[-1]:
            continue
        try:
            sv = serial_swap_verdict(open(s, errors="replace").read())
        except OSError:
            sv = ([], [])
        # the heads the newest report's counters judged (60+ hashed swaps:
        # PASS / WARN / FAIL in flicker_verdict) - their serial flicker
        # lines are INFO; copy forward and swap warnings stay
        judged = set()
        if reports:
            tp = reports[-1][:-len(".tar.gz")] + "-tests.txt"
            try:
                rtxt = open(tp, errors="replace").read()
            except OSError:
                rtxt = ""
            if "rdna3accel_swaps_hashed_h" in rtxt:
                for lv, t in flicker_verdict(rtxt):
                    m = re.match(r"head (\d+): ", t)
                    if m and lv != "INFO":
                        judged.add(m.group(1))
        if sv[0] or sv[1]:
            print("   swaps and flicker (the kernel's lines; a run without a report has only these%s):" %
                  ("; the report's counters decide head(s) %s, their lines are INFO" % ", ".join(sorted(judged))
                   if judged else ""))
            for lv, t in sv[0]:
                m = re.match(r"head (\d+): ", t)
                if m and m.group(1) in judged and lv != "INFO":
                    t = "(serial said %s) %s" % (lv, t)
                    lv = "INFO"
                print("   %s %-20s %s" % (lv, "swap/flicker", t))
                failed = failed or lv == "FAIL"
            for l in sv[1]:
                print("     " + l[:240])
    for h in hosts:
        print("=" * 78)
        print(os.path.basename(h))
        tail_file(h, 12, r"^==|boot-args|vfio|FAIL|error|Error")
    if reports:
        try:
            summary(reports[-1], serials[-1] if serials else None)
        except (OSError, tarfile.TarError, ValueError) as e:
            print("summary: " + str(e))
    print("=" * 78)
    print("RESULT:", "something failed - start at the first FAIL (docs/AFTER-TESTS.md)"
          if failed else "every reached check passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
