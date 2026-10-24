#!/bin/bash
# tools/triage_reports.py's "present mode" rows (card run 2026-10-17d: WindowServer
# presents with page flips, never IOPresentment transactions; the VM has a laptop
# SMBIOS model and the card on PCI bus 0) on synthetic reports:
#  - a run like 2026-10-17d: page flip mode on, iop_ok 0, IOFBIntegrated Yes, GPUWrangler
#    (IG) b:d:f=0:16:0, MacBookPro16,1 with isMobile=YES and no mux, the framebuffers'
#    two index properties disagreeing (WARN)
#  - a desktop run (play-desktop): MacPro7,1, isMobile=NO, bus 1, no integrated flag,
#    iop_ok > 0, no init_page_flip line (INFO, not a WARN), the indices agree
#  - what the host asked for against what macOS sees: card bus, model (WARN)
#  - isMobile=YES on a desktop model (WARN)
#  - nothing in the report: every row says so (WARN), never silence
#  - the host log of the VM run is found by CONTENT (its boot-args line against the report's
#    boot-args), not by the clock stamps (host local time against the guest's UTC) and not by
#    "the newest": a newer log of another run is not read; a log that cannot be matched, two
#    matching logs that ask for different things, no log, a log without the 'SMBIOS model:' /
#    'GPU on:' lines: each a WARN row (ECC review of 501eae9), never silence
#  - a TESTS-ONLY report and a result directory without a report: the rows come from the host
#    log alone (asked model, asked bus, the card in the guest's `info pci`: bus and BARs)
#  - 'rdna3vm approve --step N' in the STALE KEXTS action when the run asked for a Mac model
#  - the fb index row is INFO for the state of card run 2026-10-17d (unchanged), WARN when it changed
#  - the GPU vendor CoreDisplay gave each framebuffer (DRIVER-PLAN N44): vendor 1 of an old class INFO,
#    vendor 4 of the AMD class PASS, no CoreDisplay log WARN
#  - SUMMARY rows and the checklist's levels (WARN stays WARN, INFO INFO)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_present: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt" | head -60; exit 1; }

python3 -I - "$T" "$ROOT" <<'PY'
import io, os, sys, tarfile

T = sys.argv[1]
ROOT = sys.argv[2]

FB_BLOCK = """+-o RDNA3Framebuffer  <class RDNA3Framebuffer, id 0x1000002d1, registered, matched, active, busy 0 (2 ms), retain 22>
  | {
  |   "IOFBDependentIndex" = %(dep)s
  |   "IOFramebufferOpenGLIndex" = %(ogl)s
%(integrated)s  |   "IOFBConfig" = {"dims"=<80070000>}
  | }

"""


def fb(pairs, integrated):
    out = "$ ioreg -l -w0 -r -c RDNA3Framebuffer\n"
    for ogl, dep in pairs:
        out += FB_BLOCK % {"ogl": ogl, "dep": dep,
                           "integrated": '  |   "IOFBIntegrated" = Yes\n' if integrated else ""}
    return out + "[exit 0]\n"


def display_line(mobile, bdw="YES"):
    return ("2026-10-17 19:23:44.090 Df WindowServer[158:28c] [com.apple.CoreDisplay:default] [WARN] - "
            "get_display_device_info: Display 0x0b437d44 force8BPC=NO isBDW=%s isSKL=NO isCNL=NO isAcc=YES "
            "isMUX=NO isBuiltIn=NO isMobile=%s isSkylakeMacBook=NO\n" % (bdw, mobile))


GPUW = ("2026-10-17 19:23:43.342 Df WindowServer[158:28c] [com.apple.GPUWrangler:default] (reg) gpu 0xfbd5 "
        "flags %s vid.did=1002.747e b:d:f=%s\n")
PAGEFLIP = ("2026-10-17 19:23:43.336 I  WindowServer[158:28c] [com.apple.CoreDisplay:default] [INFO] - "
            "init_page_flip: page flip mode is on\n")
SETUP = ("2026-10-17 19:23:44.580 I  WindowServer[158:28c] [com.apple.CoreDisplay:default] [INFO] - "
         "MPAccessSurfaceForDisplayDevice: Set up page flip mode on display 0x0b437d44 isBackBuffered: 1\n")
NOMUX = ("2026-10-17 19:23:43.994 I  WindowServer[158:28c] [com.apple.CoreDisplay:default] [INFO] - "
         "mux_initialize: Couldn't find any matches\n")
OTHER = "2026-10-17 19:23:43.435 I  WindowServer[158:28c] (IOPresentment) Info: AGDC Plane Scaler successful\n"


def vendors(v, klass):
    """CoreDisplay's GetSystemGPUVendors lines for both heads (DRIVER-PLAN N44)"""
    out = ""
    for eid, path in (("0x1000002f2", "RDNA3Framebuffer"), ("0x1000002d1", "RDNA3Accel/RDNA3DisplayHead/RDNA3Framebuffer")):
        out += ("2026-10-17 19:23:43.429 I  WindowServer[158:28c] [com.apple.CoreDisplay:default] [INFO] - "
                "GetSystemGPUVendors_block_invoke: %s %d %s IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/%s\n"
                % (eid, v, klass, path))
    return out


def system(model):
    s = "$ sw_vers\nProductName:\t\tmacOS\n[exit 0]\n"
    if model:
        s += "hw.model: %s\nboard-id: Mac-27AD2F918AE68F61\n" % model
    return s


def report(d, label, stamp, members, tests):
    os.makedirs(os.path.join(T, d), exist_ok=True)
    base = os.path.join(T, d, "rdna3-report-%s-%s" % (label, stamp))
    with tarfile.open(base + ".tar.gz", "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    if tests is not None:
        open(base + "-tests.txt", "w").write(tests)


HOSTARGS = "-v keepsyms=1 rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3 rdna3play=1 @smbios=%s rdna3vmauto=1"


def pci(bus, mapped=True, secs=(8, 40)):
    out = ""
    for sec in secs:
        out += "== guest PCI bus (QEMU 'info pci', %d s after start)\n" % sec
        out += "  Bus  0, device   0, function 0:\n    Host bridge: PCI device 8086:29c0\n      id \"\"\n"
        out += "  Bus  %d, device  %d, function 0:\n    VGA controller: PCI device 1002:747e\n" % (bus, 16 if bus == 0 else 0)
        out += "      PCI subsystem 1002:0e3a\n      IRQ 0, pin A\n"
        a0 = "0x800000000 [0x80fffffff]" if mapped else "0xffffffffffffffff [0x0ffffffe]"
        out += "      BAR0: 64 bit prefetchable memory at %s.\n      BAR5: 32 bit memory at %s.\n" % (
            a0, "0xc0000000 [0xc007ffff]" if mapped else "0xffffffffffffffff [0x0007fffe]")
        out += "      BAR6: 32 bit memory at 0xffffffffffffffff [0x0001fffe].\n      id \"\"\n"
    return out


def host(d, stamp, model, bus, args=None, lines=True, extra="", mtime=None):
    """a host-*.log; model None = "not asked"; lines False = no SMBIOS model / GPU on lines"""
    os.makedirs(os.path.join(T, d), exist_ok=True)
    f = os.path.join(T, d, "host-%s.log" % stamp)
    body = "boot-args: %s\n" % (args or (HOSTARGS % model if model else HOSTARGS.replace(" @smbios=%s", "")))
    if lines:
        body += ("  SMBIOS model: %s\n" % model if model else
                 "  SMBIOS model: not asked (the OpenCore config is untouched: the image's own model, MacBookPro16,1 unless edited by hand)\n")
        body += "  GPU on: %s\n" % bus
    open(f, "w").write(body + extra)
    if mtime:
        os.utime(f, (mtime, mtime))


ARGS = "boot-args\t-v keepsyms=1 rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3 rdna3play=1 rdna3vmauto=1\n"
BASE = {"01-boot-args.txt": ARGS, "03-kexts.txt": "RDNA3Accel\n", "11-ioreg-props.txt": "",
        "20-log-rdna3.txt": ""}


def counters(ok, empty):
    return "debug.rdna3accel_iop_ok: %d\ndebug.rdna3accel_iop_empty: %d\n" % (ok, empty)


# a: like card run 2026-10-17d (the model from the tests file's PLATFORM line, an older collector)
m = dict(BASE)
m.update({"12-ioreg-fb.txt": fb([(0, 1), (1, 0)], True),
          "24-log-coredisplay.txt": PAGEFLIP + OTHER + NOMUX + display_line("YES") + SETUP + SETUP + GPUW % ("0x10 (IG)", "0:16:0")
          + vendors(1, "RDNA3Framebuffer")})
report("a", "play", "20261017-195000", m,
       "PLATFORM: macOS 15.8.1 (24H32), Darwin 24.6.0, virtual machine, MacBookPro16,1 Intel Core Processor (Skylake)\n" + counters(0, 0))
# the host's stamps are local time, the report's the guest's UTC: nothing here lines up by name, and the log of
# ANOTHER run (other boot-args) is the newest by mtime
host("a", "20261017-190000", "MacBookPro16,1", "pcie0 (root bus 00:10.0: PCI bus 0)", mtime=1_000_000)
host("a", "20261017-200000", "MacPro7,1", "rootport (behind a PCIe root port: PCI bus 1, not bus 0)",
     args="-v keepsyms=1 rdna3lvl=3 rdna3play=1 @smbios=MacPro7,1 rdna3vmauto=1", mtime=2_000_000)   # newer: not this run's

# b: play-desktop
m = dict(BASE)
m.update({"00-system.txt": system("MacPro7,1"), "12-ioreg-fb.txt": fb([(0, 0), (1, 1)], False),
          "24-log-coredisplay.txt": OTHER + display_line("NO", "NO") + GPUW % ("0x0", "1:0:0") + vendors(4, "AMDRDNA3Framebuffer")})
report("b", "play", "20261017-195000", m, counters(12, 3) +
       "== CoreDisplay prefs (/Library/Preferences/com.apple.CoreDisplay, DRIVER-PLAN N44): this boot's WindowServer "
       "started with: { useIOP = 1; \"useIOP_onAMD\" = 1; }\n"
       "== CoreDisplay prefs: useIOP / useIOP_onAMD true (rdna3iopprefs=0 takes them out); now: { useIOP = 1; "
       "\"useIOP_onAMD\" = 1; }\n")
host("b", "20261017-190000", "MacPro7,1", "rootport (behind a PCIe root port: PCI bus 1, not bus 0)")

# c: the host asked for a root port and MacPro7,1; macOS sees bus 0 and a MacBook
m = dict(BASE)
m.update({"00-system.txt": system("MacBookPro16,1"), "12-ioreg-fb.txt": fb([(0, 0)], True),
          "24-log-coredisplay.txt": PAGEFLIP + display_line("YES") + GPUW % ("0x10 (IG)", "0:16:0")})
report("c", "play", "20261017-195000", m, counters(0, 5))
host("c", "20261017-190000", "MacPro7,1", "rootport (behind a PCIe root port: PCI bus 1, not bus 0)")

# d: a desktop model, but CoreDisplay still says isMobile=YES
m = dict(BASE)
m.update({"00-system.txt": system("iMacPro1,1"), "12-ioreg-fb.txt": fb([(0, 0)], False),
          "24-log-coredisplay.txt": PAGEFLIP + display_line("YES") + GPUW % ("0x0", "1:0:0")})
report("d", "play", "20261017-195000", m, counters(0, 5))

# e: nothing but the boot-args (collect.sh died early); an accelerator run without counters
report("e", "play", "20261017-195000", {"01-boot-args.txt": ARGS, "03-kexts.txt": "RDNA3Accel\n", "11-ioreg-props.txt": "",
                                         "20-log-rdna3.txt": ""}, "")

# f: an empty CoreDisplay log and a non-accelerator run: the counters are not asked for
report("f", "play", "20261017-195000", {"01-boot-args.txt": "boot-args rdna3lvl=3\n", "03-kexts.txt": "RDNA3Accel\n",
                                         "11-ioreg-props.txt": "", "20-log-rdna3.txt": "",
                                         "12-ioreg-fb.txt": fb([(0, 0)], False), "00-system.txt": system("MacBookPro16,1"),
                                         "24-log-coredisplay.txt": "$ x\n[exit 0]\n"}, "")

# g: host log without the 'SMBIOS model:' / 'GPU on:' lines (an rdna3vm older than the A/B)
m = dict(BASE)
m.update({"00-system.txt": system("MacBookPro16,1"), "12-ioreg-fb.txt": fb([(0, 1), (1, 0)], True),
          "24-log-coredisplay.txt": PAGEFLIP + display_line("YES") + GPUW % ("0x10 (IG)", "0:16:0")})
report("g", "play", "20261017-195000", m, counters(0, 0))
host("g", "20261017-190000", "MacBookPro16,1", "pcie0", lines=False)

# h: the only host log carries other boot-args (a different run): not matched, said
report("h", "play", "20261017-195000", m, counters(0, 0))
host("h", "20261017-190000", "MacPro7,1", "rootport", args="-v rdna3lvl=3 rdna3play=1 @smbios=MacPro7,1 rdna3vmauto=1")

# i: the same step twice, with different requests (config changed between): ambiguous, said
report("i", "play", "20261017-195000", m, counters(0, 0))
host("i", "20261017-190000", "MacBookPro16,1", "pcie0", mtime=1_000_000)
host("i", "20261017-200000", "MacPro7,1", "rootport", mtime=2_000_000)

# j: the same step twice with the same request: sure, no warning
report("j", "play", "20261017-195000", m, counters(0, 0))
host("j", "20261017-190000", "MacBookPro16,1", "pcie0", mtime=1_000_000)
host("j", "20261017-200000", "MacBookPro16,1", "pcie0", mtime=2_000_000)

# k: nothing asked (the OpenCore config untouched)
report("k", "play", "20261017-195000", m, counters(0, 0))
host("k", "20261017-190000", None, "pcie0 (root bus 00:10.0: PCI bus 0)")

# l: the index mismatch changed (not the 0,1 / 1,0 of card run 2026-10-17d)
m2 = dict(m)
m2["12-ioreg-fb.txt"] = fb([(0, 0), (1, 2)], True)
report("l", "play", "20261017-195000", m2, counters(0, 0))
host("l", "20261017-190000", "MacBookPro16,1", "pcie0")

# m: a TESTS-ONLY report (the archive never came): the host log alone, the card on a root port with its BARs mapped
os.makedirs(os.path.join(T, "m"), exist_ok=True)
open(os.path.join(T, "m", "rdna3-report-play-20261017-195000-tests.txt"), "w").write(
    "boot-args: -v keepsyms=1 rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3 rdna3play=1 rdna3vmauto=1\n" + counters(0, 0))
host("m", "20261017-190000", "MacPro7,1", "rootport (behind a PCIe root port: PCI bus 1, not bus 0)", extra=pci(1))

# n: tests only, no host log at all
os.makedirs(os.path.join(T, "n"), exist_ok=True)
open(os.path.join(T, "n", "rdna3-report-play-20261017-195000-tests.txt"), "w").write(
    "boot-args: -v rdna3lvl=8 rdna3play=1\n" + counters(0, 0))

# o: no report at all, a host log with the card behind a root port, BARs NOT mapped at 40 s
os.makedirs(os.path.join(T, "o"), exist_ok=True)
host("o", "20261017-190000", "MacPro7,1", "rootport (behind a PCIe root port: PCI bus 1, not bus 0)",
     extra=pci(1, True, (8,)) + pci(1, False, (40,)))

# p: no report and no host log
os.makedirs(os.path.join(T, "p"), exist_ok=True)

# q: no report, a host log whose QEMU monitor never answered (no 'guest PCI bus' blocks at all)
host("q", "20261017-190000", "MacBookPro16,1", "pcie0")

# r: a stale-kext marker, the run asked for MacPro7,1 + a root port (the play-desktop step)
os.makedirs(os.path.join(T, "r"), exist_ok=True)
open(os.path.join(T, "r", "approve-1.txt"), "w").write(
    "RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7 (not approved yet)\n")
steps = {l.split()[0]: " ".join(l.split()[1:]) for l in open(os.path.join(ROOT, "tools", "ladder-steps.txt")).read().splitlines()
         if l.strip() and not l.startswith("#")}
host("r", "20261017-190000", "MacPro7,1", "rootport", args="-v keepsyms=1 " + steps["play-desktop"] + " rdna3vmauto=1")
os.makedirs(os.path.join(T, "s"), exist_ok=True)     # the same without a model: plain 'rdna3vm approve'
open(os.path.join(T, "s", "approve-1.txt"), "w").write(
    "RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7 (not approved yet)\n")
host("s", "20261017-190000", None, "pcie0", args="-v keepsyms=1 " + steps["play"] + " rdna3vmauto=1")
PY

run() { python3 -I "$ROOT/tools/triage_reports.py" "$T/$1" > "$T/out.txt" 2>&1 || true; }
row() { grep -F -- "   $1 present mode, $2:" "$T/out.txt" || fail "$3: no '$1 present mode, $2' row"; }
has() { grep -qF -- "$1" "$T/out.txt" || fail "$2: '$1' not in the output"; }
hasnt() { ! grep -qF -- "$1" "$T/out.txt" || fail "$2: '$1' unexpected"; }

# ---- a
run a
row INFO "page flip mode" a | grep -qF "init_page_flip: page flip mode is on; 'Set up page flip mode' on a display 2 time(s)" || fail "a: page flip row"
row INFO "GPU vendor" a | grep -qF "IOPresentment off: framebuffer classed vendor 1 (unknown): framebuffer 0x1000002f2 class RDNA3Framebuffer: vendor 1 (unknown), a framebuffer older than DRIVER-PLAN N44" || fail "a: GPU vendor row (an old report)"
row INFO transactions a | grep -qF "iop_ok 0, iop_empty 0: no IOPresentment transactions at all" || fail "a: transactions row"
row INFO integrated a | grep -qF "IOFBIntegrated = Yes on 2 of 2 framebuffer(s)" || fail "a: integrated row"
row INFO "fb index" a | grep -qF "IOFramebufferOpenGLIndex 0,1, IOFBDependentIndex 1,0: they disagree on framebuffer(s) 0,1, as in card run 2026-10-17d (unchanged; whether it matters is DRIVER-PLAN N39 open item 3)" || fail "a: fb index row (INFO: the known state is not a standing WARN)"
row INFO GPUWrangler a | grep -qF "flags 0x10 (IG) vid.did=1002.747e b:d:f=0:16:0: an integrated GPU (IG)" || fail "a: GPUWrangler row"
row INFO SMBIOS a | grep -qF "MacBookPro16,1 (PLATFORM line); CoreDisplay isBDW=YES isBuiltIn=NO isMUX=NO isMobile=YES; mux_initialize found no match; rdna3vm asked for MacBookPro16,1; laptop model: CoreDisplay takes the mobile paths" || fail "a: SMBIOS row: $(grep 'SMBIOS:' "$T/out.txt")"
hasnt "card bus" "a: the host log asked for pcie0 and the card is on bus 0"
hasnt "rdna3vm asked for MacPro7,1" "a: read the host log of a LATER run (the newest by mtime, other boot-args)"
hasnt "host log not matched" "a: the log was matched by its boot-args"
hasnt "request unknown" "a: the request is known"
# SUMMARY rows
has "  present: page flip mode INFO init_page_flip: page flip mode is on" "a: SUMMARY page flip"
has "  present: fb index      INFO IOFramebufferOpenGLIndex 0,1" "a: SUMMARY fb index"
python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 400 "$T/out.txt" > "$T/ck.txt"
grep -q '^INFO    present: fb index ' "$T/ck.txt" || fail "a: the checklist does not show the fb index row as INFO: $(grep present "$T/ck.txt")"
! grep -q '^WARN    present: fb index' "$T/ck.txt" || fail "a: a permanent WARN for the known fb index state"
grep -q '^INFO    present: GPUWrangler ' "$T/ck.txt" || fail "a: the checklist does not show the GPUWrangler row as INFO"
grep -q '^INFO    present: integrated ' "$T/ck.txt" || fail "a: integrated row"

# ---- b: the desktop variant
run b
row INFO "page flip mode" b | grep -qF "no 'init_page_flip' line in the CoreDisplay log, and planes arrive in transactions: not the page-flip mode" || fail "b: page flip row"
row INFO transactions b | grep -qF "iop_ok 12, iop_empty 3: planes arrive in IOPresentment transactions" || fail "b: transactions row"
row INFO integrated b | grep -qF "IOFBIntegrated = Yes on 0 of 2 framebuffer(s) (not set: IOGraphics sees a discrete card)" || fail "b: integrated row"
row INFO "fb index" b | grep -qF "IOFramebufferOpenGLIndex 0,1, IOFBDependentIndex 0,1: they agree" || fail "b: fb index row"
row INFO GPUWrangler b | grep -qF "flags 0x0 (no tags) vid.did=1002.747e b:d:f=1:0:0: a discrete GPU" || fail "b: GPUWrangler row"
row INFO SMBIOS b | grep -qF "MacPro7,1 (hw.model, board-id Mac-27AD2F918AE68F61); CoreDisplay isBDW=NO isBuiltIn=NO isMUX=NO isMobile=NO; rdna3vm asked for MacPro7,1" || fail "b: SMBIOS row: $(grep 'SMBIOS:' "$T/out.txt")"
row PASS "GPU vendor" b | grep -qF "framebuffer 0x1000002d1 class AMDRDNA3Framebuffer: vendor 4 (AMD): CoreDisplay counts it as AMD" || fail "b: GPU vendor row"
hasnt "next lead" "b: planes arrive: no useIOP_onAMD lead"
row INFO "CoreDisplay prefs" b | grep -qF "this boot's WindowServer started with useIOP_onAMD 1, useIOP 1" || fail "b: CoreDisplay prefs row"
hasnt "WARN present mode" "b: a clean desktop run has no WARN"
hasnt "card bus" "b: asked rootport, bus 1"

# ---- c: the host asked for something macOS does not show
run c
row WARN "card bus" c | grep -qF "rdna3vm put the card on rootport but macOS sees PCI bus 0" || fail "c: card bus row: $(grep 'card bus' "$T/out.txt")"
row WARN SMBIOS c | grep -qF "rdna3vm asked for MacPro7,1; BUT macOS reports MacBookPro16,1: OpenCore did not apply the model" || fail "c: SMBIOS row"
grep -q '^WARN    present: card bus' <(python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 400 "$T/out.txt") || fail "c: checklist card bus"

# ---- d: a desktop model with the mobile paths
run d
row WARN "request unknown" d | grep -qF "no host-*.log next to the report: what rdna3vm asked for (Mac model, card bus) and where the card sat in the guest are unknown" || fail "d: no host log must be a WARN"
row WARN SMBIOS d | grep -qF "iMacPro1,1 (hw.model, board-id Mac-27AD2F918AE68F61); CoreDisplay isBDW=YES isBuiltIn=NO isMUX=NO isMobile=YES; isMobile=YES on a desktop model: something else makes CoreDisplay mobile" || fail "d: SMBIOS row: $(grep 'SMBIOS:' "$T/out.txt")"
row INFO "page flip mode" d | grep -qF "page flip mode is on" || fail "d: page flip row"

# ---- e: nothing in the report: every row says so
run e
row WARN "page flip mode" e | grep -qF "24-log-coredisplay.txt is missing or empty" || fail "e: page flip row"
row WARN transactions e | grep -qF "iop_ok / iop_empty are not in the tests file" || fail "e: transactions row"
row WARN integrated e | grep -qF "12-ioreg-fb.txt has no RDNA3Framebuffer" || fail "e: integrated row"
row WARN "fb index" e | grep -qF "the index properties are not known" || fail "e: fb index row"
row WARN GPUWrangler e | grep -qF "no 'GPUWrangler (reg) gpu ... flags ... b:d:f=' line" || fail "e: GPUWrangler row"
row WARN SMBIOS e | grep -qF "model not in the report" || fail "e: SMBIOS row"
row WARN "GPU vendor" e | grep -qF "24-log-coredisplay.txt is missing or empty: the framebuffers' GPU vendor" || fail "e: GPU vendor row"
row WARN "CoreDisplay prefs" e | grep -qF "useIOP / useIOP_onAMD of this boot are not known" || fail "e: CoreDisplay prefs row"
row WARN "request unknown" e | grep -qF "no host-*.log next to the report" || fail "e: no host log"

# ---- f: an empty CoreDisplay log, a run without the accelerator
run f
row WARN "page flip mode" f | grep -qF "is missing or empty" || fail "f: page flip row"
row WARN "GPU vendor" f | grep -qF "is missing or empty" || fail "f: GPU vendor row"
row INFO transactions f | grep -qF "no accelerator counters (rdna3accel is off in this run)" || fail "f: transactions row"
row INFO SMBIOS f | grep -qF "MacBookPro16,1 (hw.model, board-id Mac-27AD2F918AE68F61)" || fail "f: SMBIOS row"

# ---- g: a host log without the request lines
run g
[ "$(grep -c 'WARN present mode, request unknown:' "$T/out.txt")" = 2 ] || fail "g: two 'request unknown' rows expected: $(grep 'request unknown' "$T/out.txt")"
row WARN "request unknown" g | grep -qF "host-20261017-190000.log has no 'SMBIOS model:' line" || fail "g: model row"
grep -F "   WARN present mode, request unknown:" "$T/out.txt" | grep -qF "has no 'GPU on:' line" || fail "g: bus row"

# ---- h: a host log that is not this run's
run h
row WARN "host log not matched" h | grep -qF "none of the 1 host log(s) has this report's rdna3 boot-args" || fail "h: not matched row: $(grep 'host log' "$T/out.txt")"
row WARN "host log not matched" h | grep -qF "host-20261017-190000.log is used as a guess" || fail "h: the guess is named"
row WARN SMBIOS h | grep -qF "[UNVERIFIED: the host log was not matched to this run]" || fail "h: a mismatch against a guessed log must say so"

# ---- i / j: two logs with the same boot-args
run i
row WARN "host log not matched" i | grep -qF "2 host logs carry this report's boot-args and ask for different things (host-20261017-190000.log, host-20261017-200000.log)" || fail "i: ambiguous row: $(grep 'host log' "$T/out.txt")"
run j
hasnt "host log not matched" "j: two identical requests are sure"
row INFO SMBIOS j | grep -qF "rdna3vm asked for MacBookPro16,1" || fail "j: SMBIOS row"

# ---- k: nothing asked
run k
row INFO SMBIOS k | grep -qF "rdna3vm asked for no model (the OpenCore config was left as it is)" || fail "k: SMBIOS row: $(grep 'SMBIOS:' "$T/out.txt")"
hasnt "request unknown" "k: 'not asked' is a known request"

# ---- l: the mismatch changed
run l
row WARN "fb index" l | grep -qF "they disagree on framebuffer(s) 1, and not as 0,1 / 1,0 of card run 2026-10-17d: the mismatch CHANGED (DRIVER-PLAN N39 open item 3)" || fail "l: fb index row: $(grep 'fb index' "$T/out.txt")"
run b
row INFO "fb index" b | grep -qF "they agree (card run 2026-10-17d had 0,1 against 1,0: this run differs, note it in DRIVER-PLAN N39 open item 3)" || fail "b: agreeing indices after 17d's mismatch are news"

# ---- m: tests only, the host log alone
run m
has "TESTS ONLY" "m"
row INFO "asked model" m | grep -qF "rdna3vm asked for MacPro7,1" || fail "m: asked model"
row INFO "asked bus" m | grep -qF "rdna3vm put the card on rootport (PCI bus 1)" || fail "m: asked bus"
[ "$(grep -c 'INFO present mode, guest PCI: .* s: PCI bus 1, device 0, function 0; BAR0,5 mapped$' "$T/out.txt")" = 2 ] || fail "m: guest PCI rows: $(grep 'guest PCI' "$T/out.txt")"
hasnt "host log not matched" "m: matched by the tests file's boot-args"
hasnt "page flip mode" "m: no archive, no CoreDisplay rows"

# ---- n: tests only, no host log
run n
row WARN "request unknown" n | grep -qF "no host-*.log next to the report" || fail "n: tests only without a host log is a WARN"

# ---- o: no report at all, a host log: the card's BARs lost at 40 s
run o
has "no rdna3-report-*.tar.gz: macOS never sent a report" "o"
row INFO "asked model" o | grep -qF "rdna3vm asked for MacPro7,1" || fail "o: asked model"
grep -F "INFO present mode, guest PCI: 8 s: PCI bus 1" "$T/out.txt" | grep -qF "BAR0,5 mapped" || fail "o: 8 s row"
grep -F "WARN present mode, guest PCI: 40 s: PCI bus 1" "$T/out.txt" | grep -qF "no BAR mapped; NOT mapped: BAR0,5" || fail "o: 40 s row must be a WARN: $(grep 'guest PCI' "$T/out.txt")"

# ---- p / q: no report and no host log / no 'info pci' output
run p
row WARN "request unknown" p | grep -qF "no host-*.log next to the report" || fail "p: no report and no host log"
run q
row INFO "asked bus" q | grep -qF "rdna3vm put the card on pcie0 (PCI bus 0)" || fail "q: asked bus"
row WARN "guest PCI" q | grep -qF "the host log has no 'guest PCI bus' (info pci) output" || fail "q: no info pci output is a WARN"

# ---- r / s: the STALE KEXTS action names the step when it asked for a Mac model
run r
grep -qF "FAIL STALE KEXTS          old driver ran - run 'rdna3vm approve --step play-desktop', then the step again (RDNA3Accelerator running e35e78c275" "$T/out.txt" || fail "r: STALE KEXTS action: $(grep 'STALE' "$T/out.txt" | head -3)"
run s
grep -qF "FAIL STALE KEXTS          old driver ran - run 'rdna3vm approve', then the step again" "$T/out.txt" || fail "s: STALE KEXTS action without a model: $(grep 'STALE' "$T/out.txt" | head -3)"

# ---- the collector writes what the triage reads
grep -q 'hw.model: $(sysctl -n hw.model' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not write hw.model into 00-system.txt"
grep -q '"board-id: ' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not write board-id"
sh -n "$ROOT/tools/macos/collect.sh" || fail "sh -n collect.sh"

echo "test_triage_present: ok"
