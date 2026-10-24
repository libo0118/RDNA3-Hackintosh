#!/bin/bash
# tools/triage_reports.py's "STALE KEXTS" line on synthetic results (card runs
# 2026-10-18b and 2026-10-19b: macOS ran the old RDNA3Accelerator because the
# new build was never allowed; the result only said "no report"):
#  - no report, an approve marker + a serial log whose accelerator build is not
#    the checkout's -> FAIL STALE KEXTS with both hashes is the FIRST line, ahead
#    of the generic "no rdna3-report" complaint; the checklist lists it first
#  - no report, a serial log alone (no marker) -> the same FAIL from the hashes
#    of the checked-out sources (accelerator, framebuffer, AGDC)
#  - serial builds equal to the sources, no marker -> no STALE KEXTS
#  - the result unpacked one level down (<dir>/rdna3-result/) is found too
#  - with a report: the line is printed once, ahead of the report, also when the
#    tests file says "not approved to load" (its older FAIL is not repeated)
#  - the action comes first in the line and survives result_checklist at width 80
#  - a report's non-empty "stale: <kext>: running X, installed Y" line alone gives
#    the FAIL with the exact hashes (no "not approved to load" needed), once per run
#  - a check that cannot be done is an INFO line, not silence: no build line in the
#    newest serial log, a source hash that cannot be computed, an unreadable marker
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_stale_kexts: FAIL $*"; [ -f "$T/out.txt" ] && cat "$T/out.txt"; exit 1; }
TR=$ROOT/tools/triage_reports.py

# the hashes of the checked-out sources (what a current build prints)
read -r ACC FB AG < <(cd "$ROOT" && python3 -I -c '
import sys; sys.path.insert(0, "tools")
import triage_reports as t
print(t.fb_source_hash(t.ACCEL_HASH_FILES), t.fb_source_hash(t.FB_HASH_FILES), t.fb_source_hash(t.AGDC_HASH_FILES))')
[ -n "$ACC" ] && [ -n "$FB" ] && [ -n "$AG" ] || fail "no source hashes: [$ACC] [$FB] [$AG]"

serial() {   # serial <file> <accel build> <fb build> <agdc build>
    cat > "$1" <<EOF
RDNA3: start, safety level 8, build abcdef0 (0.1.0)
RDNA3-agdc: build $4: rdna3agdc=3: up when the first framebuffer publishes
RDNA3-fb: started (head 0, VBL locked to the OTG frame counter, hardware cursor), build $3
RDNA3Accelerator: build $2
EOF
}
run() { python3 -I "$TR" "$1" > "$T/out.txt" 2>&1 || true; }
first_fail() { grep -n '' "$T/out.txt" | grep -E 'FAIL|^[0-9]+:!!' | head -1; }

# ---- no report: marker + a serial log with the old accelerator
mkdir -p "$T/a"
echo "RDNA3Accelerator: running e35e78c275, installed $ACC (not approved yet)" > "$T/a/approve-20261019-213355.txt"
serial "$T/a/serial-20261019-213152.log" e35e78c275 "$FB" "$AG"
run "$T/a"
[ "$(head -n1 "$T/out.txt")" = "   FAIL STALE KEXTS          old driver ran - run 'rdna3vm approve', then the step again (RDNA3Accelerator running e35e78c275, installed $ACC)" ] ||
    fail "a: the first line is not the STALE KEXTS FAIL: $(head -n1 "$T/out.txt")"
l1=$(grep -n 'STALE KEXTS' "$T/out.txt" | head -1 | cut -d: -f1)
l2=$(grep -n '!! no rdna3-report' "$T/out.txt" | head -1 | cut -d: -f1)
[ -n "$l1" ] && [ -n "$l2" ] && [ "$l1" -lt "$l2" ] || fail "a: STALE KEXTS ($l1) is not ahead of the 'no report' complaint ($l2)"
[ "$(grep -c 'STALE KEXTS' "$T/out.txt")" = 1 ] || fail "a: the marker and the serial log both reported the accelerator"
grep -q 'RESULT: something failed' "$T/out.txt" || fail "a: RESULT does not say failed"
python3 -I "$ROOT/tools/result_checklist.py" --no-color "$T/out.txt" > "$T/ck.txt"
[ "$(grep '^FAIL ' "$T/ck.txt" | head -1 | cut -c1-49)" = "FAIL    triage: STALE KEXTS  old driver ran - run" ] ||
    fail "a: the checklist does not start with STALE KEXTS: $(cat "$T/ck.txt")"
# a narrow terminal: the action and the hashes of the row survive (card run 2026-10-19b cut them)
python3 -I "$ROOT/tools/result_checklist.py" --no-color --width 80 "$T/out.txt" > "$T/ck80.txt"
grep -qF "old driver ran - run 'rdna3vm approve', then the step again (RDNA3Accelerator running e35e78c275, installed $ACC)" "$T/ck80.txt" ||
    fail "a: the STALE KEXTS row is cut at width 80: $(cat "$T/ck80.txt")"

# ---- no report, no marker: the serial log's builds against the sources
mkdir -p "$T/b"
serial "$T/b/serial-1.log" e35e78c275 "$FB" "$AG"
run "$T/b"
grep -q "   FAIL STALE KEXTS          old driver ran - run 'rdna3vm approve', then the step again (RDNA3Accelerator running e35e78c275 per serial-1.log, the sources of this checkout build $ACC" "$T/out.txt" || fail "b: accelerator"
grep -q "run 'rdna3vm approve', then the step again" "$T/out.txt" || fail "b: no action"
! grep -q 'STALE KEXTS.*RDNA3Framebuffer\|STALE KEXTS.*RDNA3AGDC' "$T/out.txt" || fail "b: framebuffer / AGDC reported although current"
sed -n 1p "$T/out.txt" | grep -q "^   FAIL STALE KEXTS  *old driver ran - run 'rdna3vm approve'" || fail "b: not the first line"

mkdir -p "$T/c"
serial "$T/c/serial-1.log" "$ACC" 0000000001 0000000002
run "$T/c"
grep -q "RDNA3Framebuffer running 0000000001 per serial-1.log, the sources of this checkout build $FB" "$T/out.txt" || fail "c: framebuffer"
grep -q "RDNA3AGDC running 0000000002 per serial-1.log, the sources of this checkout build $AG" "$T/out.txt" || fail "c: AGDC"
! grep -q 'STALE KEXTS.*RDNA3Accelerator' "$T/out.txt" || fail "c: accelerator reported although current"

# ---- everything current: no STALE KEXTS
mkdir -p "$T/d"
serial "$T/d/serial-1.log" "$ACC" "$FB" "$AG"
run "$T/d"
! grep -q 'STALE KEXTS' "$T/out.txt" || fail "d: current builds reported stale"
# a serial log without the build lines (no accelerator in this step) is not a complaint
mkdir -p "$T/e"
echo "RDNA3: start, safety level 8, build abcdef0 (0.1.0)" > "$T/e/serial-1.log"
run "$T/e"
! grep -q 'FAIL STALE KEXTS' "$T/out.txt" || fail "e: no build lines reported stale"
grep -q 'INFO stale check .*serial-1.log has none of the RDNA3Accelerator / RDNA3-fb / RDNA3-agdc build lines.*stale check not done' "$T/out.txt" ||
    fail "e: no INFO that the stale check was not done"
# the newest serial log decides (an older boot's builds are another run's)
mkdir -p "$T/f"
serial "$T/f/serial-1.log" e35e78c275 "$FB" "$AG"
# 2 hours older (python: BSD touch on the macOS runner has no -d '2 hours ago')
python3 -c 'import os, sys, time; t = time.time() - 7200; os.utime(sys.argv[1], (t, t))' "$T/f/serial-1.log"
serial "$T/f/serial-2.log" "$ACC" "$FB" "$AG"
run "$T/f"
! grep -q 'STALE KEXTS' "$T/out.txt" || fail "f: an older serial log decided"

# ---- the unpacked result one level down
mkdir -p "$T/g/rdna3-result"
echo "RDNA3Accelerator: running e35e78c275, installed $ACC (not approved yet)" > "$T/g/rdna3-result/approve-1.txt"
run "$T/g"
grep -q "FAIL STALE KEXTS          old driver ran - run 'rdna3vm approve', then the step again (RDNA3Accelerator running e35e78c275, installed $ACC)" "$T/out.txt" || fail "g: nested result not found"

# ---- with a report: once, ahead of the report, the older FAIL not repeated
mkdir -p "$T/h"
echo "RDNA3Accelerator: running e35e78c275, installed $ACC (not approved yet)" > "$T/h/approve-1.txt"
python3 -I - "$T/h/rdna3-report-x.tar.gz" <<'PY'
import io, sys, tarfile
members = {"01-boot-args.txt": "boot-args rdna3lvl=2\n", "03-kexts.txt": "RDNA3Accel\n",
           "11-ioreg-props.txt": "", "20-log-rdna3.txt": "RDNA3: hello\n"}
with tarfile.open(sys.argv[1], "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
PY
printf 'kernelmanagerd: not approved to load\nstale: RDNA3Accelerator\n' > "$T/h/rdna3-report-x-tests.txt"
run "$T/h"
head -n1 "$T/out.txt" | grep -q "^   FAIL STALE KEXTS  *old driver ran - run 'rdna3vm approve'" || fail "h: not first: $(head -n1 "$T/out.txt")"
[ "$(grep -c 'FAIL STALE KEXTS' "$T/out.txt")" = 1 ] || fail "h: the STALE KEXTS FAIL is printed more than once"
grep -q 'a new RDNA3Framebuffer/RDNA3Accelerator waits for approval' "$T/out.txt" || fail "h: the older approval hint is gone"

# ---- a report whose tests file has a non-empty "stale:" line: the FAIL with the exact hashes by itself,
# without "not approved to load" and without a marker or serial log
mkSTALE() {   # mkSTALE <dir> <tests file body>
    mkdir -p "$1"
    cp "$T/h/rdna3-report-x.tar.gz" "$1/"
    printf '%s\n' "$2" > "$1/rdna3-report-x-tests.txt"
}
mkSTALE "$T/i" "stale: RDNA3Accelerator: running e35e78c275, installed $ACC (not approved yet);RDNA3Framebuffer: running 1111111111, installed $FB (not approved yet);"
run "$T/i"
[ "$(grep -c 'FAIL STALE KEXTS' "$T/out.txt")" = 1 ] || fail "i: expected exactly one STALE KEXTS FAIL: $(grep -c 'FAIL STALE KEXTS' "$T/out.txt")"
grep -qF "(RDNA3Accelerator running e35e78c275, installed $ACC); old driver ran - run 'rdna3vm approve', then the step again (RDNA3Framebuffer running 1111111111, installed $FB)" "$T/out.txt" ||
    fail "i: the hashes of the stale line are not in the FAIL: $(grep 'STALE KEXTS' "$T/out.txt")"
grep -q 'RESULT: something failed' "$T/out.txt" || fail "i: RESULT does not say failed"
# empty "stale:" line = nothing stale
mkSTALE "$T/j" "stale: "
run "$T/j"
! grep -q 'STALE KEXTS' "$T/out.txt" || fail "j: an empty stale: line reported stale"
# marker + stale line + 'not approved to load': still one FAIL
mkSTALE "$T/k" "$(printf 'kernelmanagerd: not approved to load\nstale: RDNA3Accelerator: running e35e78c275, installed %s (not approved yet);' "$ACC")"
echo "RDNA3Accelerator: running e35e78c275, installed $ACC (not approved yet)" > "$T/k/approve-1.txt"
run "$T/k"
[ "$(grep -c 'FAIL STALE KEXTS' "$T/out.txt")" = 1 ] || fail "k: more than one STALE KEXTS FAIL"
# the generic FAIL stays for a stale line without hashes (the older approval hint, no marker)
mkSTALE "$T/m" "$(printf 'kernelmanagerd: not approved to load\nstale: RDNA3Accelerator')"
run "$T/m"
[ "$(grep -c 'FAIL STALE KEXTS' "$T/out.txt")" = 1 ] && grep -q 'FAIL STALE KEXTS *this run used the framebuffer/accelerator approved earlier' "$T/out.txt" ||
    fail "m: the generic STALE KEXTS FAIL is gone"

# ---- a check that cannot be done says so (INFO), never silence
python3 -I - "$ROOT" "$T" <<'PY' || fail "l: $(cat "$T/out.txt" 2>/dev/null)"
import os, sys
root, tmp = sys.argv[1:3]
sys.path.insert(0, os.path.join(root, "tools"))
import triage_reports as t
# a source hash that cannot be computed
d = os.path.join(tmp, "l1"); os.makedirs(d)
open(os.path.join(d, "serial-1.log"), "w").write("RDNA3Accelerator: build e35e78c275\n")
real = t.fb_source_hash
t.fb_source_hash = lambda files=None: None
out = t.stale_kext_lines(d)
t.fb_source_hash = real
assert [lv for lv, _ in out] == ["INFO"], out
assert "RDNA3Accelerator" in out[0][1] and "cannot be computed" in out[0][1], out
# a marker that cannot be read (a directory by that name)
d = os.path.join(tmp, "l2"); os.makedirs(os.path.join(d, "approve-1.txt"))
out = t.stale_kext_lines(d)
assert len(out) == 1 and out[0][0] == "INFO" and "approve-1.txt cannot be read" in out[0][1], out
# an unreadable newest serial log
d = os.path.join(tmp, "l3"); os.makedirs(os.path.join(d, "serial-1.log"))
out = t.stale_kext_lines(d)
assert len(out) == 1 and out[0][0] == "INFO" and "stale check not done" in out[0][1], out
print("l: ok")
PY

echo "test_triage_stale_kexts: ok"
