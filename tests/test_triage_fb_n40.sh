#!/bin/bash
# tools/triage_reports.py's "fb N40" rows (DRIVER-PLAN N40, the driver side of the
# present-mode A/B) on synthetic reports:
#  - a framebuffer pair with every N40 property (RDNA3,Switches, RDNA3,OpenOrder,
#    RDNA3,IntegratedAB, RDNA3,VBLSource, RDNA3,FBStats) and the GL index following the
#    dependent index, the AGDC vendor answer in 17-sysctl-agdc.txt: INFO rows only
#  - rdna3glorder=1 with the indices still crossed, a cursor that failed, a system sleep
#    without the family wake work, VBL runs overlapping with vblonce=1, AGDC armed without
#    the vendor sysctl: WARN rows
#  - framebuffers older than N40 (no RDNA3,Switches) and a report without framebuffers: WARN
#  - the ECC review round: a boot-arg the framebuffer does not publish, a switch not in effect
#    (cursor thread call, VBL thread call, wake tap/gate), the IOFBIntegrated A/B against the
#    property, a never-opened head, a failed open, wake counters (power-ons with a deaf tap, a
#    sleep still pending, failed wake work), FBStats keys missing, no VBL on a DCN head, the
#    cursor unused or refused, the AGDC vendor (set but never asked, asked without AGDC armed,
#    not what the boot-arg asked) and a TESTS-ONLY report's AGDC rows: WARN rows
#  - the SUMMARY rows ("fb N40: ...") and the checklist's levels
#  - the collectors: collect.sh writes 17-sysctl-agdc.txt, autoreport.sh greps rdna3agdc
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_fb_n40: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt" | grep -E "fb N40|FAIL|Traceback|Error" | head -60; exit 1; }

python3 -I - "$T" <<'PY'
import io, os, sys, tarfile

T = sys.argv[1]

FB = """+-o RDNA3Framebuffer  <class RDNA3Framebuffer, id 0x1000002d1, registered, matched, active, busy 0 (2 ms), retain 22>
  | {
  |   "IOFBDependentIndex" = %(dep)d
  |   "IOFramebufferOpenGLIndex" = %(gl)d
%(extra)s  | }

"""


def fb(dep, gl, n40=True, glorder=1, vblonce=1, stats=None, console=0, sw=None, drop=(), integ="Yes",
       integ_ab=None, order=None):
    extra = ""
    if n40:
        st = {"cursor_sets": 900, "cursor_kicks": 300, "cursor_coalesced": 550, "cursor_same": 50,
              "cursor_applies": 300, "cursor_fails": 0, "cursor_sync": 0, "cursor_images": 3,
              "cursor_image_fails": 0, "cursor_convert_fails": 0,
              "wake_sleeps": 0, "wake_family": 0, "wake_owed_max": 0, "wake_plain_on": 1,
              "wake_didwake_seen": 0, "wake_unanswered": 0, "wake_nogate": 0, "wake_off": 0,
              "wake_tap_events": 0, "wake_slept_pending": 0, "wake_paging_already": 0, "wake_dropped": 0,
              "wake_fails": 0, "wake_retries": 0, "wake_gave_up": 0, "wake_stopping": 0,
              "vbl_delivered": 3600, "vbl_polls": 7000, "vbl_still": 2, "vbl_overlap": 0, "vbl_call_failed": 0,
              "integ_applied": 1, "integ_reset": 0, "integ_late": 0, "open_ungated": 0, "open_fails": 0,
              "dt_validate": 61, "dt_matched": 4, "dt_refused": 61, "dt_sets": 0, "dt_mapped": 0,
              "dispflags_asked": 2}
        st.update(stats or {})
        for k in drop:
            st.pop(k, None)
        sws = {"dispflags": 0, "glorder": glorder, "fbintegrated": 0, "cursorasync": 1, "cursorasync_active": 1,
               "hwcursor": 1, "fbwake": 1, "waketap": 1, "wakegate": 1, "vblonce": vblonce, "vbl_call": 1,
               "vblonce_active": vblonce, "fbdtmap": 0, "fbconsole": 1, "bootconsole": console}
        sws.update(sw or {})
        d = lambda x: "{" + ",".join('"%s"=%d' % kv for kv in x.items()) + "}"
        extra += '  |   "RDNA3,Switches" = %s\n' % d(sws)
        extra += '  |   "RDNA3,FBStats" = %s\n' % d(st)
        extra += '  |   "RDNA3,OpenOrder" = "%s"\n' % (order or ("head 0 opened first" if dep else "head 0"))
        extra += '  |   "RDNA3,IntegratedAB" = "%s"\n' % (
            integ_ab or "IOGraphics Yes, kept (rdna3fbintegrated=%d), PCI bus 0" % sws["fbintegrated"])
        extra += '  |   "RDNA3,VBLSource" = "otg-poll"\n'
        extra += '  |   "RDNA3,Mode" = "dcn"\n'
    if integ:
        extra += '  |   "IOFBIntegrated" = %s\n' % integ
    return FB % {"dep": dep, "gl": gl, "extra": extra}


def report(d, members, tests):
    os.makedirs(os.path.join(T, d), exist_ok=True)
    base = os.path.join(T, d, "rdna3-report-play-20261023-120000")
    if members is None:   # TESTS ONLY: the archive never came
        open(base + "-tests.txt", "w").write(tests)
        return
    with tarfile.open(base + ".tar.gz", "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    open(base + "-tests.txt", "w").write(tests)


ARGS = "boot-args\t-v keepsyms=1 rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3 rdna3play=1 rdna3vmauto=1\n"
BASE = {"01-boot-args.txt": ARGS, "03-kexts.txt": "RDNA3Accel\n", "11-ioreg-props.txt": "", "20-log-rdna3.txt": ""}
AGDC = ("$ sh -c sysctl debug | grep rdna3agdc\ndebug.rdna3agdc: 1\ndebug.rdna3agdc_vendor: 1\n"
        "debug.rdna3agdc_vendor_asked: 3\ndebug.rdna3agdc_vendor_last: 1\n[exit 0]\n")

# a: the change works: both framebuffers carry N40, GL = dependent index, AGDC answers NVIDIA (the A/B)
m = dict(BASE)
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(1, 1) + fb(0, 0, console=1) + "[exit 0]\n"
m["17-sysctl-agdc.txt"] = AGDC
report("a", m, "")

# b: glorder on but the indices still crossed, a failed cursor, a sleep without wake work, overlapping VBL runs,
#    AGDC armed without the vendor sysctl
m = dict(BASE)
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(1, 0, stats={"cursor_fails": 4, "wake_sleeps": 1, "vbl_overlap": 3}) + \
    fb(0, 1, console=1) + "[exit 0]\n"
report("b", m, "debug.rdna3agdc: 1\n")

# c: framebuffers older than N40
m = dict(BASE)
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(1, 0, n40=False) + fb(0, 1, n40=False) + "[exit 0]\n"
report("c", m, "")

# d: no framebuffer at all, AGDC not armed
m = dict(BASE)
m["01-boot-args.txt"] = "boot-args\trdna3lvl=8 rdna3fb=1\n"
report("d", m, "")

# e: vblonce=0 with overlaps: INFO (the switch's own case), not a WARN
m = dict(BASE)
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(0, 0, vblonce=0, stats={"vbl_overlap": 7}, console=1) + "[exit 0]\n"
m["17-sysctl-agdc.txt"] = AGDC.replace("vendor: 1", "vendor: 0").replace("vendor_last: 1", "vendor_last: 0")
report("e", m, "")

# f: the boot-args ask what the framebuffer does not publish; switches not in effect
m = dict(BASE)
m["01-boot-args.txt"] = ARGS.replace("rdna3fb=1", "rdna3fb=1 rdna3fbwake=0 rdna3glorder=1 rdna3vblonce=yes")
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(0, 0, console=1, sw={"cursorasync_active": 0, "vbl_call": 0, "vblonce_active": 0,
                                                             "waketap": 0, "wakegate": 0}) + "[exit 0]\n"
m["17-sysctl-agdc.txt"] = AGDC
report("f", m, "")

# g: the IOFBIntegrated A/B against the property; a never-opened head; a failed open
m = dict(BASE)
m["12-ioreg-fb.txt"] = "$ ioreg\n" + \
    fb(1, 1, sw={"fbintegrated": 1}, order="head 0 opened first; open FAILED 0xe00002c0 (1 failure(s))") + \
    fb(0, 0, console=1, sw={"fbintegrated": 2}, integ=None, stats={"integ_reset": 2}) + \
    fb(2, 2, integ=None, integ_ab="never opened (rdna3fbintegrated=0)") + "[exit 0]\n"
m["17-sysctl-agdc.txt"] = AGDC
report("g", m, "")

# h: wake: power-ons with a deaf tap, a sleep pending, failed wake work; a sleep covered by paging already on;
#    old FBStats (keys missing); no VBL on a DCN head; the cursor never used; the first cursor image refused
m = dict(BASE)
m["12-ioreg-fb.txt"] = "$ ioreg\n" + \
    fb(1, 1, stats={"wake_plain_on": 3, "wake_slept_pending": 1, "wake_fails": 6, "wake_retries": 5, "wake_gave_up": 1,
                    "vbl_delivered": 0, "cursor_sets": 0, "cursor_images": 0}) + \
    fb(0, 0, console=1, stats={"wake_sleeps": 1, "wake_paging_already": 1, "wake_tap_events": 2, "wake_plain_on": 2,
                               "cursor_image_fails": 1}, drop=("open_fails", "wake_dropped")) + "[exit 0]\n"
m["17-sysctl-agdc.txt"] = AGDC
report("h", m, "")

# i: AGDC: the A/B answer set but never asked, the boot-arg not what the sysctl says
m = dict(BASE)
m["01-boot-args.txt"] = ARGS.replace("rdna3agdc=3", "rdna3agdc=3 rdna3agdcvendor=2")
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(0, 0, console=1) + "[exit 0]\n"
m["17-sysctl-agdc.txt"] = AGDC.replace("asked: 3", "asked: 0")
report("i", m, "")

# j: rdna3agdcvendor without AGDC armed
m = dict(BASE)
m["01-boot-args.txt"] = "boot-args\trdna3lvl=8 rdna3fb=1 rdna3agdcvendor=1\n"
m["12-ioreg-fb.txt"] = "$ ioreg\n" + fb(0, 0, console=1) + "[exit 0]\n"
report("j", m, "")

# k: TESTS ONLY: the AGDC rows from the tests file
report("k", None, "boot-args: rdna3lvl=8 rdna3fb=1 rdna3agdc=3 rdna3agdcvendor=1\n" + AGDC.replace("$ sh -c", "#"))
PY

run() { python3 -I "$ROOT/tools/triage_reports.py" "$T/$1" > "$T/out.txt" 2>&1 || true; }
row() { grep -F -- "   $1 fb N40, $2:" "$T/out.txt" || fail "$3: no '$1 fb N40, $2' row"; }
hasnt() { ! grep -qF -- "$1" "$T/out.txt" || fail "$2: '$1' unexpected"; }

# ---- a: INFO only
run a
row INFO switches a | grep -qF "fb 1: dispflags 0, glorder 1, fbintegrated 0, cursorasync 1, fbwake 1, vblonce 1, fbdtmap 0, fbconsole 1; not the boot console" || fail "a: switches fb 1"
row INFO switches a | grep -qF "fb 0: dispflags 0, glorder 1" || fail "a: switches fb 0"
row INFO switches a | grep -qF "fb 0: dispflags 0, glorder 1, fbintegrated 0, cursorasync 1, fbwake 1, vblonce 1, fbdtmap 0, fbconsole 1; boot console here" || fail "a: boot console on fb 0"
row INFO "connection flags" a | grep -qF "fb 1: kConnectionDisplayFlags answered 0 (rdna3dispflags 0), asked 2 time(s)" || fail "a: connection flags"
row INFO "open order" a | grep -qF "fb 1: head 0 opened first; IOFramebufferOpenGLIndex 1, IOFBDependentIndex 1" || fail "a: open order"
row INFO "integrated A/B" a | grep -qF "IOGraphics Yes, kept (rdna3fbintegrated=0), PCI bus 0" || fail "a: integrated"
row INFO cursor a | grep -qF "fb 1: thread call; sets 900, thread-call runs 300, folded 550, unchanged 50, applied 300, FAILED 0" || fail "a: cursor"
row INFO wake a | grep -qF "system sleeps 0, family wakes 0" || fail "a: wake"
row INFO VBL a | grep -qF "source otg-poll (no hardware vblank interrupt yet), delivered 3600, 250 us re-looks 7000" || fail "a: VBL"
row INFO "detailed timings" a | grep -qF "validateDetailedTiming 61 (4 one of our modes, 61 refused)" || fail "a: timings"
row INFO "AGDC vendor" a | grep -qF "vendor info answer NVIDIA 0x10de (debug.rdna3agdc_vendor 1), asked 3 time(s); last answered as NVIDIA 0x10de" || fail "a: AGDC vendor"
hasnt "WARN fb N40" "a: a clean N40 run has no WARN"
grep -qF "  fb N40: switches" "$T/out.txt" || fail "a: SUMMARY fb N40 rows"
python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 400 "$T/out.txt" > "$T/ck.txt"
grep -q '^INFO    fb N40: AGDC vendor ' "$T/ck.txt" || fail "a: the checklist shows the AGDC vendor row as INFO: $(grep 'fb N40' "$T/ck.txt" | head)"

# ---- b: WARN rows
run b
row WARN "open order" b | grep -qF "fb 1: head 0 opened first; IOFramebufferOpenGLIndex 0, IOFBDependentIndex 1: STILL DIFFERENT with rdna3glorder=1" || fail "b: open order"
row WARN cursor b | grep -qF "FAILED 4" || fail "b: cursor"
row WARN wake b | grep -qF "system sleeps 1, family wakes 0" || fail "b: wake"
row WARN VBL b | grep -qF "overlapping runs 3 (rdna3vblonce 1)" || fail "b: VBL"
row WARN "AGDC vendor" b | grep -qF "AGDC armed but no debug.rdna3agdc_vendor" || fail "b: AGDC vendor"
python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 400 "$T/out.txt" > "$T/ck.txt"
grep -q '^WARN    fb N40: ' "$T/ck.txt" || fail "b: the checklist keeps the WARN rows"

# ---- c: older framebuffers
run c
[ "$(grep -c 'WARN fb N40, switches: fb [01] has no RDNA3,Switches' "$T/out.txt")" = 2 ] || fail "c: two old framebuffers"

# ---- d: nothing
run d
row WARN switches d | grep -qF "12-ioreg-fb.txt has no RDNA3Framebuffer" || fail "d: no framebuffer"
row INFO "AGDC vendor" d | grep -qF "AGDC not armed" || fail "d: AGDC not armed"

# ---- e: overlaps with the switch off are its case
run e
row INFO VBL e | grep -qF "overlapping runs 7 (rdna3vblonce 0; overlaps with vblonce=0: the switch's case)" || fail "e: VBL"
row INFO "AGDC vendor" e | grep -qF "AMD 0x1002 (debug.rdna3agdc_vendor 0)" || fail "e: AMD"

# ---- f: what the boot-args ask against what the framebuffer publishes, and what is in effect
run f
row WARN "switch asked" f | grep -qF "fb 0: boot-args ask rdna3fbwake=0 but the framebuffer publishes fbwake 1" || fail "f: fbwake asked"
row WARN "switch asked" f | grep -qF "boot-args ask rdna3vblonce=yes but the framebuffer publishes vblonce 1" || fail "f: vblonce not a number"
hasnt "rdna3glorder=1 but" "f: a boot-arg the framebuffer publishes as asked"
row WARN "switch in effect" f | grep -qF "rdna3cursorasync 1 but NO cursor thread call" || fail "f: cursor thread call"
row WARN "switch in effect" f | grep -qF "NO VBL thread call: IOGraphics gets no VBL at all (rdna3vblonce 1 is not in effect)" || fail "f: VBL call"
row WARN "switch in effect" f | grep -qF "rdna3fbwake 1 but NO notification tap" || fail "f: tap"
row WARN "switch in effect" f | grep -qF "NO command gate on the controller workloop" || fail "f: gate"
row WARN wake f | grep -qF "(tap ABSENT)" || fail "f: the wake row names the tap"
run c
hasnt "switch in effect" "c: no RDNA3,Switches at all is the older WARN only"

# ---- g: the IOFBIntegrated A/B
run g
row WARN "integrated A/B" g | grep -qF "fb 1: IOGraphics Yes, kept (rdna3fbintegrated=1), PCI bus 0; IOFBIntegrated Yes: NOT what rdna3fbintegrated=1 asks" || fail "g: removed but present"
row WARN "integrated A/B" g | grep -qF "fb 0: IOGraphics Yes, kept (rdna3fbintegrated=2), PCI bus 0; IOFBIntegrated absent; IOGraphics re-set it 2 time(s), applied again: NOT what rdna3fbintegrated=2 asks" || fail "g: set but absent"
row WARN "integrated A/B" g | grep -qF "fb 2: never opened (rdna3fbintegrated=0) - the A/B was never applied" || fail "g: never opened"
row WARN "open order" g | grep -qF "fb 1: head 0 opened first; open FAILED 0xe00002c0" || fail "g: failed open"

# ---- h: wake, FBStats, VBL, cursor
run h
row WARN wake h | grep -qF "fb 1: system sleeps 0" || fail "h: wake fb 1"
row WARN wake h | grep -qF "the wake work FAILED 6 time(s) (retried 5, given up 1)" || fail "h: wake failures"
row WARN wake h | grep -qF "3 power-ons and the tap saw no WillSleep/DidWake at all" || fail "h: deaf tap"
row WARN wake h | grep -qF "a system sleep still recorded when the report was taken" || fail "h: pending sleep"
row INFO wake h | grep -qF "fb 0: system sleeps 1, family wakes 0 (most DidWake(false) owed 0), plain power-ons 2, paging already on 1" || fail "h: a sleep with paging already on is no WARN"
row WARN counters h | grep -qF "fb 0: RDNA3,FBStats MISSING wake_dropped,open_fails" || fail "h: missing FBStats keys"
row WARN VBL h | grep -qF "NO VBL delivered on a DCN head" || fail "h: no VBL on DCN"
row WARN cursor h | grep -qF "WindowServer never set a cursor image or position on the hardware cursor" || fail "h: cursor unused"
row WARN cursor h | grep -qF "(first image refused 1, not converted 0)" || fail "h: first image refused"

# ---- i, j: the AGDC vendor
run i
row WARN "AGDC vendor" i | grep -qF "boot-args ask rdna3agdcvendor=2 but debug.rdna3agdc_vendor is 1" || fail "i: vendor asked"
row WARN "AGDC vendor" i | grep -qF "the A/B answer NVIDIA 0x10de is set but IOPresentment never asked" || fail "i: never asked"
run j
row WARN "AGDC vendor" j | grep -qF "boot-args ask rdna3agdcvendor=1 but RDNA3AGDC is not armed" || fail "j: vendor without AGDC"

# ---- k: a TESTS-ONLY report still has the AGDC rows
run k
grep -qF "TESTS ONLY" "$T/out.txt" || fail "k: not a tests-only report"
row INFO "AGDC vendor" k | grep -qF "vendor info answer NVIDIA 0x10de (debug.rdna3agdc_vendor 1), asked 3 time(s)" || fail "k: AGDC rows"
grep -qF "  fb N40: AGDC vendor" "$T/out.txt" || fail "k: SUMMARY AGDC row"

# ---- the collectors
grep -q 'run 17-sysctl-agdc.txt' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not write 17-sysctl-agdc.txt"
grep -q 'grep rdna3agdc' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not grep the rdna3agdc sysctls"
grep -q 'rdna3agdc" || true' "$ROOT/tools/vm/macos/autoreport.sh" || fail "autoreport.sh no longer greps rdna3agdc into the tests file"
grep -q 'run 12-ioreg-fb.txt ioreg -l -w0 -r -c RDNA3Framebuffer' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not dump the framebuffers' properties"
sh -n "$ROOT/tools/macos/collect.sh" || fail "sh -n collect.sh"

echo "test_triage_fb_n40: ok"
