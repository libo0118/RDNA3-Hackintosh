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
        failed = failed or not same or not up or len(pids) > 2
    # step 10: the accelerator also loads from the Auxiliary KC
    if "rdna3accel=1" in args:
        want = fb_source_hash(("accel/rdna3-accel.cpp", "accel/Info.plist", "accel/rdna3k_abi.h",
                               "accel/rdna3accel_stubs.h"))
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


TX_CHANGE = re.compile(r"display: (head \d+|clone) TX change #(\d+) at (\d+) ms uptime: problems: ([^;]*); (.*)$")
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
    lines, the first change with problems, the last 20; once each"""
    keep, first_bad, full = set(), None, None
    for i, l in enumerate(lines):
        if re.search(r"TX (at the start|after the clone|new chain|first look)|TX watch:|TX not read|changed since the start", l):
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
    out, seen = [], set()
    for i in sorted(keep):
        t = lines[i][lines[i].find("display:"):]
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


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
        ticks = parse_props(props_txt).get("TXWatchTicks") if props_txt else None
        ticked = "TX watch: first tick" in log or (isinstance(ticks, int) and ticks > 0)
        parts = []
        for ent, v in sorted(ch.items()):
            bad = [c for c in v if c[2] != "none"]
            c = bad[0] if bad else v[0]
            parts.append("%s: %d change(s); first %s at %d ms: problems: %s; %s" %
                         (ent, len(v), "with problems" if bad else "", c[1], c[2], c[3][:160]))
        if parts:
            out.append(("TX watch", " | ".join(parts)))
        elif "TX watch: off" in log:
            out.append(("TX watch", "off (rdna3txwatch=0)"))
        elif ticked:
            out.append(("TX watch", "no change on any head after the start (%s tick(s))" %
                        (ticks if isinstance(ticks, int) else "some")))
        else:
            out.append(("TX watch", "NEVER TICKED: no change seen proves nothing (the statistics tick did not run)"))
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
                        print("       %s display sleep: %d pipe(s) blanked" % ("PASS" if nb else "FAIL", nb))
                        failed = failed or not nb
                    if look and look.group(1) == "wake":
                        nl, nb = len(addrs), len(re.findall(r"^pipe \d+: BLANKED", blk, re.M))
                        print("       %s display wake: %d pipe(s) lit, %d still blanked" %
                              ("PASS" if nl and not nb else "FAIL", nl, nb))
                        failed = failed or not nl or nb > 0
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
