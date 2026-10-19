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
                    "common/rdna3_trace.h", "common/rdna3_write_gen.h")


# fb/Makefile's FB_SRC_HASH inputs and agdc/Makefile's AGDC_SRC_HASH inputs
FB_HASH_FILES = ("fb/rdna3-fb.cpp", "fb/Info.plist", "accel/rdna3k_abi.h", "agdc/rdna3_agdc_k5.h")
AGDC_HASH_FILES = ("agdc/rdna3-agdc.cpp", "agdc/rdna3_agdc_k5.h", "agdc/AppleGraphicsDeviceControl_re.h",
                   "agdc/Info.plist")


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
                       r"_gen_evictpresent|_gen_landed|_gennotjudged|_gen_resets)?_h\d|"
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
    return out


def latch_lines(path, txt, args=""):
    """[(level, text)] of a report's flip latch: the counters of its tests
    file, else the archive's 29-surface-vram.txt (collect.sh without
    autoreport: no tests file); 'not measured' when neither has a dump on
    an accelerator run"""
    c = a3_counters(txt) if txt else None
    where = "the tests file"
    sv = log = ""
    if os.path.exists(path):
        try:
            with tarfile.open(path) as tar:
                sv = read_member(tar, "29-surface-vram.txt") or ""
                log = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt") or ""
                args = args or read_member(tar, "01-boot-args.txt") or ""
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
    out = latch_verdict(c, current)
    if out and where != "the tests file":
        out.append(("INFO", "(counters from %s: no tests file dump)" % where))
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
STALE_KEXTS_SHOWN = False   # a STALE KEXTS FAIL was printed (one per run; report() does not repeat it)


def stale_marker_text(kext, running, installed):
    return "%s (%s running %s, installed %s)" % (STALE_KEXTS_HEAD, kext, running, installed)


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
    for d in seen:
        for f in sorted(glob.glob(os.path.join(d, "approve-*.txt"))):
            try:
                txt = open(f, errors="replace").read()
            except OSError as e:
                info.append("approve marker %s cannot be read (%s): the stale check from it is not done" %
                            (os.path.basename(f), e.strerror or e))
                continue
            for kext, running, installed in APPROVE_MARKER_RE.findall(txt):
                found.setdefault(kext, stale_marker_text(kext, running, installed))
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
                                       STALE_KEXTS_HEAD, kext, m.group(1), sname, want))
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
            print("   FAIL %-20s rdna3accel_iop_ok %s%s" % ("planes in transactions", ok if ok else "not in the report",
                  "" if wsmetal else "; WindowServer has no Metal device (24-log-coredisplay.txt NVMTL lines)"))
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
    return True, triage_bisect.analyse(text, base, logs), []


def bisect_logs(path):
    """[(name, text)] of the kernel logs a report has: the archive's 20-log-rdna3.txt and the newest serial-*.log next to it
    (tools/triage_bisect.py picks the one that places most of the bisect's steps)"""
    out = []
    t = read_tar_member(path, "20-log-rdna3.txt") or read_tar_member(path, "20-log-rdna3-sudo.txt")
    if t:
        out.append(("20-log-rdna3.txt", t))
    d = os.path.dirname(os.path.abspath(path))
    for f in sorted(glob.glob(os.path.join(d, "serial-*.log")), key=os.path.getmtime)[-1:]:
        try:
            out.append((os.path.basename(f), open(f, errors="replace").read()))
        except OSError:
            pass
    return out


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
    for p in reports:
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
        # card run 2026-10-17b: the swap as a copy, and the per-frame trace
        for lv, t in swapcopy_verdict(a3_counters(ttxt) if ttxt else {}):
            print("   %s swap copy: %s" % (lv, t))
            failed = failed or lv == "FAIL"
        klog = kernel_text(p, d)
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
            stale = re.search(r"^stale: (.*)$", txt, re.M)
            skipped = re.findall(r"^== (\S+): SKIPPED - passed on ([0-9-]+ [0-9:]+)", txt, re.M)
            if skipped:
                print("   note: %d test(s) not run again, passed before with the same builds (their saved results "
                      "below): %s" % (len(skipped), ", ".join("%s (%s)" % k for k in skipped)))
            # a non-empty 'stale:' line alone (hashes of the kext macOS ran and of the one installed) proves the old
            # driver ran, whatever kernelmanagerd logged; one FAIL per run
            sk = STALE_LINE_RE.findall(stale.group(1)) if stale else []
            if sk and not STALE_KEXTS_SHOWN:
                print("   FAIL %-20s %s" % ("STALE KEXTS", "; ".join(stale_marker_text(*x) for x in sk)))
                STALE_KEXTS_SHOWN = failed = True
            if "not approved to load" in txt and stale and not stale.group(1).strip():
                # the builds running are the ones installed (same source
                # hashes): only a relinked copy of the same sources waits
                # (card run 45: a CI rebuild reinstalled), the results count
                print("   note: kernelmanagerd waits for approval of a relinked copy of the running "
                      "sources only ('stale:' empty); the results count")
            elif "not approved to load" in txt:
                print("   !! a new RDNA3Framebuffer/RDNA3Accelerator waits for approval in macOS: run "
                      "'rdna3vm approve' (System Settings -> Privacy & Security -> Allow)")
                # the ones approved earlier ran instead: what this run says
                # about the display, the accelerator and Metal on screen is
                # about those (2026-10-12: an older framebuffer in console
                # mode, every frame copied by the CPU)
                if not STALE_KEXTS_SHOWN:
                    print("   FAIL STALE KEXTS this run used the framebuffer/accelerator approved earlier, not "
                          "this build's: display, accelerator and Metal-on-screen results do not count; "
                          "'rdna3vm approve', then the step again")
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
