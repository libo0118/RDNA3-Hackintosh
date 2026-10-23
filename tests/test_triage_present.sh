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
#  - the host log of the VM run (the newest one started before the report) is the one read
#  - SUMMARY rows and the checklist's levels (WARN stays WARN, INFO INFO)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_present: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt" | head -60; exit 1; }

python3 -I - "$T" <<'PY'
import io, os, sys, tarfile

T = sys.argv[1]

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


def host(d, stamp, model, bus):
    open(os.path.join(T, d, "host-%s.log" % stamp), "w").write(
        "boot-args: -v rdna3play=1 @smbios=%s\n  SMBIOS model: %s\n  GPU on: %s\n" % (model, model, bus))


ARGS = "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3 rdna3play=1\n"
BASE = {"01-boot-args.txt": ARGS, "03-kexts.txt": "RDNA3Accel\n", "11-ioreg-props.txt": "",
        "20-log-rdna3.txt": ""}


def counters(ok, empty):
    return "debug.rdna3accel_iop_ok: %d\ndebug.rdna3accel_iop_empty: %d\n" % (ok, empty)


# a: like card run 2026-10-17d (the model from the tests file's PLATFORM line, an older collector)
m = dict(BASE)
m.update({"12-ioreg-fb.txt": fb([(0, 1), (1, 0)], True),
          "24-log-coredisplay.txt": PAGEFLIP + OTHER + NOMUX + display_line("YES") + SETUP + SETUP + GPUW % ("0x10 (IG)", "0:16:0")})
report("a", "play", "20261017-195000", m,
       "PLATFORM: macOS 15.8.1 (24H32), Darwin 24.6.0, virtual machine, MacBookPro16,1 Intel Core Processor (Skylake)\n" + counters(0, 0))
host("a", "20261017-190000", "MacBookPro16,1", "pcie0 (root bus 00:10.0: PCI bus 0)")
host("a", "20261017-200000", "MacPro7,1", "rootport (behind a PCIe root port: PCI bus 1, not bus 0)")   # later: not this run's

# b: play-desktop
m = dict(BASE)
m.update({"00-system.txt": system("MacPro7,1"), "12-ioreg-fb.txt": fb([(0, 0), (1, 1)], False),
          "24-log-coredisplay.txt": OTHER + display_line("NO", "NO") + GPUW % ("0x0", "1:0:0")})
report("b", "play", "20261017-195000", m, counters(12, 3))
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
PY

run() { python3 -I "$ROOT/tools/triage_reports.py" "$T/$1" > "$T/out.txt" 2>&1 || true; }
row() { grep -F -- "   $1 present mode, $2:" "$T/out.txt" || fail "$3: no '$1 present mode, $2' row"; }
has() { grep -qF -- "$1" "$T/out.txt" || fail "$2: '$1' not in the output"; }
hasnt() { ! grep -qF -- "$1" "$T/out.txt" || fail "$2: '$1' unexpected"; }

# ---- a
run a
row INFO "page flip mode" a | grep -qF "init_page_flip: page flip mode is on; 'Set up page flip mode' on a display 2 time(s)" || fail "a: page flip row"
row INFO transactions a | grep -qF "iop_ok 0, iop_empty 0: no IOPresentment transactions at all" || fail "a: transactions row"
row INFO integrated a | grep -qF "IOFBIntegrated = Yes on 2 of 2 framebuffer(s)" || fail "a: integrated row"
row WARN "fb index" a | grep -qF "IOFramebufferOpenGLIndex 0,1, IOFBDependentIndex 1,0: they disagree on framebuffer(s) 0,1" || fail "a: fb index row"
row INFO GPUWrangler a | grep -qF "flags 0x10 (IG) vid.did=1002.747e b:d:f=0:16:0: an integrated GPU (IG)" || fail "a: GPUWrangler row"
row INFO SMBIOS a | grep -qF "MacBookPro16,1 (PLATFORM line); CoreDisplay isBDW=YES isBuiltIn=NO isMUX=NO isMobile=YES; mux_initialize found no match; rdna3vm asked for MacBookPro16,1; laptop model: CoreDisplay takes the mobile paths" || fail "a: SMBIOS row: $(grep 'SMBIOS:' "$T/out.txt")"
hasnt "card bus" "a: the host log asked for pcie0 and the card is on bus 0"
hasnt "rdna3vm asked for MacPro7,1" "a: read the host log of a LATER run"
# SUMMARY rows
has "  present: page flip mode INFO init_page_flip: page flip mode is on" "a: SUMMARY page flip"
has "  present: fb index      WARN IOFramebufferOpenGLIndex 0,1" "a: SUMMARY fb index"
python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 400 "$T/out.txt" > "$T/ck.txt"
grep -q '^WARN    present: fb index ' "$T/ck.txt" || fail "a: the checklist does not show the fb index row as WARN: $(grep present "$T/ck.txt")"
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
hasnt "WARN present mode" "b: a clean desktop run has no WARN"
hasnt "card bus" "b: asked rootport, bus 1"

# ---- c: the host asked for something macOS does not show
run c
row WARN "card bus" c | grep -qF "rdna3vm put the card on rootport but macOS sees PCI bus 0" || fail "c: card bus row: $(grep 'card bus' "$T/out.txt")"
row WARN SMBIOS c | grep -qF "rdna3vm asked for MacPro7,1; BUT macOS reports MacBookPro16,1: OpenCore did not apply the model" || fail "c: SMBIOS row"
grep -q '^WARN    present: card bus' <(python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 400 "$T/out.txt") || fail "c: checklist card bus"

# ---- d: a desktop model with the mobile paths
run d
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

# ---- f: an empty CoreDisplay log, a run without the accelerator
run f
row WARN "page flip mode" f | grep -qF "is missing or empty" || fail "f: page flip row"
row INFO transactions f | grep -qF "no accelerator counters (rdna3accel is off in this run)" || fail "f: transactions row"
row INFO SMBIOS f | grep -qF "MacBookPro16,1 (hw.model, board-id Mac-27AD2F918AE68F61)" || fail "f: SMBIOS row"

# ---- the collector writes what the triage reads
grep -q 'hw.model: $(sysctl -n hw.model' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not write hw.model into 00-system.txt"
grep -q '"board-id: ' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not write board-id"
sh -n "$ROOT/tools/macos/collect.sh" || fail "sh -n collect.sh"

echo "test_triage_present: ok"
