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
                    "common/rdna3_swap_front.h", "common/rdna3_flip_latch.h")


def fb_source_hash(files=("fb/rdna3-fb.cpp", "fb/Info.plist", "accel/rdna3k_abi.h", "agdc/rdna3_agdc_k5.h")):
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
                       r"_torn|_inplace|_tornskip|_shownfreed|_emptyfilled|_emptystill)?_h\d|swap_latch|swap_torn|"
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
    for f in glob.glob(os.path.join(d, "serial-*.log")):
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
                              r"swap: surface [^\n]*matched no head[^\n]*|swap latch: head \d+: [^\n]*|"
                              r"swap #\d+: head \d+: the (?:previous flip|flip to phys|buffer it showed)[^\n]*"
                              r"(?:NOT latched|changed while on the panel)[^\n]*|"
                              r"copy forward #\d+: head \d+: the target [^\n]*STILL ON THE PANEL[^\n]*|"
                              r"swap: head \d+: the buffer [^\n]*was FREED[^\n]*|"
                              r"swap: head \d+: the empty back [^\n]*read again[^\n]*|"
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
    out.extend(serial_latch_verdict(text))
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


def latch_verdict(c):
    """[(level, text)] from the card run 2026-10-17 counters, [] on a kernel
    without them. Per head: the swaps whose entry found the previous flip not
    latched (the panel still read the buffer WindowServer had back), how long
    flips took to latch, timeouts and unreadable flip states, copy forwards
    into the buffer on the panel, shown frames that changed on the panel
    (torn: FAIL from TORN_SHARE of TORN_MIN_CHECKS+ checks, else WARN),
    rewritten in place (one back), freed while scanned, empty backs that
    filled 4 ms after a skipped swap."""
    if not any(k.startswith("swaps_latch") or k.startswith("swaps_torn") for k in c):
        return []
    latch_on = c.get("swap_latch")
    out = []
    heads = sorted(set(m.group(1) for m in (re.match(r"swaps_h(\d)$", k) for k in c) if m))
    for h in heads:
        g = lambda k: c.get(k + "_h" + h, 0)
        swaps = g("swaps")
        if not swaps and not g("swaps_latchwait") and not g("swaps_tornchk"):
            continue
        pend, waits, us, mx = g("swaps_latchpend"), g("swaps_latchwait"), g("swaps_latchus"), g("swaps_latchmax")
        tout, unk, off = g("swaps_latchtimeout"), g("swaps_latchunknown"), g("swaps_latchoff")
        chk, torn, inpl, tskip = g("swaps_tornchk"), g("swaps_torn"), g("swaps_inplace"), g("swaps_tornskip")
        onp, freed, filled, still = g("cf_onpanel"), g("swaps_shownfreed"), g("swaps_emptyfilled"), g("swaps_emptystill")
        lat = "flips latched %d time(s) seen, avg %d us, max %d us" % (waits, us // waits if waits else 0, mx)
        why = []
        if pend:
            why.append("the panel still read the buffer WindowServer had back")
        if tout:
            why.append("%d flip(s) never latched in 2 frames" % tout)
        if unk:
            why.append("%d flip state(s) unreadable" % unk)
        out.append(("WARN" if why else "INFO",
                    "head %s: %d of %d swaps: the previous flip had not latched at the swap; %s; %d timeouts, %d "
                    "unknown, %d not waited (swap_latch %s)%s" % (h, pend, swaps, lat, tout, unk, off,
                                                                   "?" if latch_on is None else latch_on,
                                                                   " - " + ", ".join(why) if why else "")))
        if onp:
            out.append(("WARN", "head %s: %d copy forward(s) wrote into the buffer on the panel (the flip away from it "
                                "had not latched)" % (h, onp)))
        if chk or torn or inpl or tskip:
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
                out.append(("INFO", "head %s: %d shown buffer(s) could not be hashed again (re-allocated, another "
                                    "surface's)" % (h, tskip)))
        if freed:
            out.append(("WARN" if freed > SHOWN_FREED_WARN else "INFO",
                        "head %s: %d buffer(s) freed while the head scanned them" % (h, freed)))
        if filled or still:
            out.append(("INFO", "head %s: empty backs of skipped swaps read again 4 ms later: %d had pixels (the frame "
                                "landed after the swap), %d still empty" % (h, filled, still)))
    return out


SERIAL_LATCH = re.compile(
    r"swap latch: head (\d+): (\d+) swaps; previous flip not latched at entry (\d+); latched while waited (\d+) \(avg "
    r"(\d+) us, max (\d+) us\), (\d+) timeouts, (\d+) unknown, (\d+) not waited; shown buffers hashed again (\d+): "
    r"(\d+) changed on the panel then flipped away \(torn\), (\d+) rewritten in place \(one back\), (\d+) not hashed; "
    r"copy forwards into the buffer on the panel (\d+); buffers freed on the panel (\d+); empty backs read again: "
    r"(\d+) filled, (\d+) still empty \(swap_latch (-?\d+)")


def serial_latch_verdict(text):
    """[(level, text)] from the kernel's last 'swap latch: head N' line per
    head in a serial log (card run 2026-10-17), as latch_verdict judges the
    report's counters"""
    last = {}
    for m in SERIAL_LATCH.finditer(text):
        last[m.group(1)] = [int(x) for x in m.groups()[1:]]
    out = []
    for h, v in sorted(last.items()):
        (swaps, pend, waits, avg, mx, tout, unk, off, chk, torn, inpl, tskip, onp, freed, filled, still,
         latch_on) = v
        c = {"swaps_h" + h: swaps, "swaps_latchpend_h" + h: pend, "swaps_latchwait_h" + h: waits,
             "swaps_latchus_h" + h: avg * waits, "swaps_latchmax_h" + h: mx, "swaps_latchtimeout_h" + h: tout,
             "swaps_latchunknown_h" + h: unk, "swaps_latchoff_h" + h: off, "swaps_tornchk_h" + h: chk,
             "swaps_torn_h" + h: torn, "swaps_inplace_h" + h: inpl, "swaps_tornskip_h" + h: tskip,
             "cf_onpanel_h" + h: onp, "swaps_shownfreed_h" + h: freed, "swaps_emptyfilled_h" + h: filled,
             "swaps_emptystill_h" + h: still, "swap_latch": latch_on}
        for lv, t in latch_verdict(c):
            out.append((lv, "%s (flip latch, serial at swap %d)" % (t, swaps)))
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


def report(path, show_all):
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
        want = fb_source_hash(("agdc/rdna3-agdc.cpp", "agdc/rdna3_agdc_k5.h", "agdc/AppleGraphicsDeviceControl_re.h",
                               "agdc/Info.plist"))
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
        out.append(("screens %s" % (look.group(1) if look else "1"),
                    "%d lit%s%s%s%s%s" % (len(lit), ", SAME SURFACE on two pipes" if same else "",
                                          ", %d blanked" % blank if blank else "", ", UNDERFLOW" if uf else "",
                                          ", TX: " + "; ".join(txbad) if txbad else "",
                                          ", NO TX LINES" if notx else "")))
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
        out.append(("A3 display counters", "none: the kext predates card run 58"))
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
        for lv, t in latch_verdict(a3c):
            out.append(("flip latch", "%s %s" % (lv, t)))
        out.append(("A3 display counters", "refused %s, imported %s, other %s (kept out %s); swaps %s" % (
            a3c.get("a3display_refused", "?"), a3c.get("a3display_imported", "?"), a3c.get("a3display_other", "?"),
            a3c.get("a3ws_refused", "?"),
            ", ".join("h%s %d (A3 %d)" % (k[-1], v, a3c.get("swaps_a3_h" + k[-1], 0))
                      for k, v in sorted(a3c.items()) if re.match(r"swaps_h\d$", k) and v))))
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
        if os.path.exists(tests):
            txt = open(tests, errors="replace").read()
            stale = re.search(r"^stale: (.*)$", txt, re.M)
            skipped = re.findall(r"^== (\S+): SKIPPED - passed on ([0-9-]+ [0-9:]+)", txt, re.M)
            if skipped:
                print("   note: %d test(s) not run again, passed before with the same builds (their saved results "
                      "below): %s" % (len(skipped), ", ".join("%s (%s)" % k for k in skipped)))
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
                print("   INFO A3 display counters: none (the kext predates card run 58)")
            elif a3c:
                print("   INFO A3 display counters: " + ", ".join("%s %d" % kv for kv in sorted(a3c.items())))
                for lv, t in flicker_verdict(txt):
                    print("   %s flicker: %s" % (lv, t))
                    failed = failed or lv == "FAIL"
                for lv, t in latch_verdict(a3c):
                    print("   %s flip latch: %s" % (lv, t))
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
