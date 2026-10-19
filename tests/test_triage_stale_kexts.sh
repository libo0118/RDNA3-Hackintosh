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
[ "$(head -n1 "$T/out.txt")" = "   FAIL STALE KEXTS          macOS ran RDNA3Accelerator e35e78c275, the build installed is $ACC (not allowed yet) - this run tested the old driver; run 'rdna3vm approve', then the step again" ] ||
    fail "a: the first line is not the STALE KEXTS FAIL: $(head -n1 "$T/out.txt")"
l1=$(grep -n 'STALE KEXTS' "$T/out.txt" | head -1 | cut -d: -f1)
l2=$(grep -n '!! no rdna3-report' "$T/out.txt" | head -1 | cut -d: -f1)
[ -n "$l1" ] && [ -n "$l2" ] && [ "$l1" -lt "$l2" ] || fail "a: STALE KEXTS ($l1) is not ahead of the 'no report' complaint ($l2)"
[ "$(grep -c 'STALE KEXTS' "$T/out.txt")" = 1 ] || fail "a: the marker and the serial log both reported the accelerator"
grep -q 'RESULT: something failed' "$T/out.txt" || fail "a: RESULT does not say failed"
python3 -I "$ROOT/tools/result_checklist.py" --no-color "$T/out.txt" > "$T/ck.txt"
[ "$(grep '^FAIL ' "$T/ck.txt" | head -1 | cut -c1-40)" = "FAIL    triage: STALE KEXTS  macOS ran R" ] ||
    fail "a: the checklist does not start with STALE KEXTS: $(cat "$T/ck.txt")"

# ---- no report, no marker: the serial log's builds against the sources
mkdir -p "$T/b"
serial "$T/b/serial-1.log" e35e78c275 "$FB" "$AG"
run "$T/b"
grep -q "   FAIL STALE KEXTS          macOS ran RDNA3Accelerator e35e78c275 (serial-1.log), the sources of this checkout build $ACC" "$T/out.txt" || fail "b: accelerator"
grep -q "run 'rdna3vm approve', then the step again" "$T/out.txt" || fail "b: no action"
! grep -q 'STALE KEXTS.*RDNA3Framebuffer\|STALE KEXTS.*RDNA3AGDC' "$T/out.txt" || fail "b: framebuffer / AGDC reported although current"
[ "$(sed -n 1p "$T/out.txt" | cut -c1-40)" = "   FAIL STALE KEXTS          macOS ran R" ] || fail "b: not the first line"

mkdir -p "$T/c"
serial "$T/c/serial-1.log" "$ACC" 0000000001 0000000002
run "$T/c"
grep -q "macOS ran RDNA3Framebuffer 0000000001 (serial-1.log), the sources of this checkout build $FB" "$T/out.txt" || fail "c: framebuffer"
grep -q "macOS ran RDNA3AGDC 0000000002 (serial-1.log), the sources of this checkout build $AG" "$T/out.txt" || fail "c: AGDC"
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
! grep -q 'STALE KEXTS' "$T/out.txt" || fail "e: no build lines reported stale"
# the newest serial log decides (an older boot's builds are another run's)
mkdir -p "$T/f"
serial "$T/f/serial-1.log" e35e78c275 "$FB" "$AG"; touch -d '2 hours ago' "$T/f/serial-1.log"
serial "$T/f/serial-2.log" "$ACC" "$FB" "$AG"
run "$T/f"
! grep -q 'STALE KEXTS' "$T/out.txt" || fail "f: an older serial log decided"

# ---- the unpacked result one level down
mkdir -p "$T/g/rdna3-result"
echo "RDNA3Accelerator: running e35e78c275, installed $ACC (not approved yet)" > "$T/g/rdna3-result/approve-1.txt"
run "$T/g"
grep -q "FAIL STALE KEXTS          macOS ran RDNA3Accelerator e35e78c275, the build installed is $ACC" "$T/out.txt" || fail "g: nested result not found"

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
[ "$(head -n1 "$T/out.txt" | cut -c1-40)" = "   FAIL STALE KEXTS          macOS ran R" ] || fail "h: not first: $(head -n1 "$T/out.txt")"
[ "$(grep -c 'FAIL STALE KEXTS' "$T/out.txt")" = 1 ] || fail "h: the STALE KEXTS FAIL is printed more than once"
grep -q 'a new RDNA3Framebuffer/RDNA3Accelerator waits for approval' "$T/out.txt" || fail "h: the older approval hint is gone"

echo "test_triage_stale_kexts: ok"
