#!/usr/bin/env python3
"""
triage_reports.py - one summary of a batch of hardware-test reports.

    python3 tools/triage_reports.py ~/rdna3-vm/reports [--all]

Reads every rdna3-report-*.tar.gz (tools/macos/collect.sh), the matching
*-tests.txt (DMA/GFX tests), serial-*.log (macOS kernel output of the VM)
and host-*.log (tools/vm/rdna3vm), oldest first, and prints per boot:
boot-args, the bring-up stage reached, every check below as PASS / FAIL /
-- (not reached at that level), the kext's ERROR lines and the tail of
the serial log. The first FAIL is where docs/AFTER-TESTS.md starts.
Exit status: 0 all reached checks pass, 1 something failed.
"""
import glob
import io
import os
import re
import sys
import tarfile

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


def report(path, show_all):
    failed = False
    with tarfile.open(path) as tar:
        args = read_member(tar, "01-boot-args.txt")
        props_txt = read_member(tar, "11-ioreg-props.txt")
        log = read_member(tar, "20-log-rdna3.txt") or read_member(tar, "20-log-rdna3-sudo.txt")
        panics = read_member(tar, "21-log-panics.txt")
        kexts = read_member(tar, "03-kexts.txt")
        pci = read_member(tar, "04-pci.txt")
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
                "AtomCommandTables", "SCPM", "PMFeaturesRunning", "PSPStuckAtInit", "InitResetResult", "GFXHUBResult", "GFXAsync", "KIQOnMES", "Mirror", "MirrorResult", "MirrorCheck", "ExtendHead", "DMUB", "DMUBCapsResult", "IdleSleepBlocked", "DCNClocks", "CloneMetricsResult", "DMUBWaitAckResult", "HotplugEvents", "HotplugPins", "MESResult", "MESSchedVersion", "MESKiqVersion", "MESLegacyQueueMap", "NeedPost", "PostResult",
                "LastFaultAddr", "LastFaultStatus"):
        if key in props:
            v = props[key]
            print("   info %-20s = %s" % (key, hex(v) if isinstance(v, int) and key in (
                "SOSVersion", "SMUDriverIF", "DMUBVersion", "GBAddrConfig", "LastFaultAddr",
                "LastFaultStatus") else v))
    # displays (step 2+): connectors with an EDID, what each head drives
    for key in sorted(k for k in props if re.match(r"(Connector\d+,(Monitor|EDIDSource|Type)|Head\d+,Route)$", k)):
        print("   info %-20s = %s" % (key, props[key]))
    for l in log.splitlines():
        if "refresh" in l and "display: head" in l:
            print("   info " + l[l.find("display:"):][:110])
    # step 9: our framebuffer (rdna3fb=1) took the head and has the EDID
    if "rdna3fb=1" in args:
        fb_started = "RDNA3-fb: started" in log
        # the framebuffer must be the same build as the core: OpenCore cannot
        # inject it (it links IOGraphicsFamily), so the VM loads the copy
        # setup.sh last put in the Auxiliary KC
        core_c = props.get("Commit")
        fb_c = re.search(r"RDNA3-fb: started .*?, build (\S+)", log)
        same = bool(core_c and fb_c and fb_c.group(1) == core_c)
        print("   %s %-20s %s" % ("PASS" if same else "FAIL", "framebuffer build",
                                 "%s = core" % core_c if same else
                                 "framebuffer %s, core %s: stale RDNA3Framebuffer.kext - run setup.sh "
                                 "in the VM (rdna3vm window) and boot twice" %
                                 (fb_c.group(1) if fb_c else "older than the build check", core_c or "?")))
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


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    show_all = "--all" in sys.argv
    load_linux_reference(d)
    failed = False
    reports = sorted(glob.glob(os.path.join(d, "rdna3-report-*.tar.gz")), key=os.path.getmtime)
    if not reports:
        print("!! no rdna3-report-*.tar.gz: macOS never sent a report (did not boot far enough,")
        print("   no network to the host, or the VM was stopped before ~2 min) - read the serial log")
        failed = True
    for p in reports:
        failed |= report(p, show_all)
        tests = p[:-len(".tar.gz")] + "-tests.txt"
        if os.path.exists(tests):
            txt = open(tests, errors="replace").read()
            if "not approved to load" in txt:
                print("   !! a new RDNA3Framebuffer/RDNA3Accelerator waits for approval in macOS: run "
                      "'rdna3vm approve' (System Settings -> Privacy & Security -> Allow)")
            if "--dma-test" in txt or "--gfx-test" in txt:
                print("   tests:")
                for l in txt.splitlines():
                    if l.startswith("==") or "verified" in l or "[exit" in l or "fail" in l.lower():
                        print("     " + l)
                if re.search(r"\[exit [1-9]", txt):
                    failed = True
    serials = sorted(glob.glob(os.path.join(d, "serial-*.log")), key=os.path.getmtime)
    hosts = sorted(glob.glob(os.path.join(d, "host-*.log")), key=os.path.getmtime)
    for s in serials:
        print("=" * 78)
        print(os.path.basename(s), "(RDNA3 lines and panics, last 15)")
        tail_file(s, 15, r"RDNA3|panic|Panic|ERROR")
    for h in hosts:
        print("=" * 78)
        print(os.path.basename(h))
        tail_file(h, 12, r"^==|boot-args|vfio|FAIL|error|Error")
    print("=" * 78)
    print("RESULT:", "something failed - start at the first FAIL (docs/AFTER-TESTS.md)"
          if failed else "every reached check passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
