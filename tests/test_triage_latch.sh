#!/bin/bash
# tools/triage_reports.py's flip latch lines (card run 2026-10-17: windows
# and menus flicker; the swap returned before its flip latched) on synthetic
# reports: frequent torn frames (a FAIL), a clean head, the previous flip
# pending and copy forwards into the buffer on the panel (WARNs), one back
# buffer rewritten in place, a kernel without the counters (no lines), and
# the kernel's 'swap latch: head N' line in a serial log.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_latch: FAIL $*"; exit 1; }

mk() {   # mk <dir/name> <counters (one 'key value' per line, without debug.rdna3accel_)>
    mkdir -p "$T/$(dirname "$1")"
    python3 -I - "$T/$1" "$2" <<'PY'
import io, sys, tarfile
path, counters = sys.argv[1], sys.argv[2]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": "RDNA3: hello\n",
    "24-log-coredisplay.txt": "",
}
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
dump = "".join("debug.rdna3accel_%s: %s\n" % tuple(l.split()) for l in counters.strip().splitlines() if l.strip())
open(path + "-tests.txt", "w").write("== A3 display counters after M6\n" + dump)
PY
}
run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }

# head 1 torn in 15 % of 200 checks, the previous flip pending at 40 swaps,
# 3 copy forwards into the buffer on the panel; head 0 clean
mk torn/rdna3-report-torn "
swap_latch 0
swap_torn 1
swaps_h0 300
swaps_h1 300
swaps_latchpend_h0 0
swaps_latchwait_h0 250
swaps_latchus_h0 2500000
swaps_latchmax_h0 16900
swaps_tornchk_h0 200
swaps_torn_h0 0
swaps_latchpend_h1 40
swaps_latchwait_h1 40
swaps_latchus_h1 200000
swaps_latchmax_h1 9000
swaps_tornchk_h1 200
swaps_torn_h1 30
cf_onpanel_h1 3
swaps_shownfreed_h1 2
swaps_emptyfilled_h1 5
swaps_emptystill_h1 19
"
o=$(run torn)
! grep -q Traceback <<<"$o" || fail "torn: a traceback: $o"
grep -q 'FAIL flip latch: head 1: 30 of 200 shown frames changed while on the panel, then flipped away (torn): 15%' <<<"$o" ||
    fail "torn: the FAIL: $o"
grep -q 'WARN flip latch: head 1: 40 of 300 swaps: the previous flip had not latched at the swap; flips latched 40 time(s) seen, avg 5000 us, max 9000 us; 0 timeouts, 0 unknown, 0 not waited (swap_latch 0) - the panel still read the buffer WindowServer had back' <<<"$o" ||
    fail "torn: the pending flips: $o"
grep -q 'WARN flip latch: head 1: 3 copy forward(s) wrote into the buffer on the panel' <<<"$o" || fail "torn: on panel: $o"
grep -q 'INFO flip latch: head 1: 2 buffer(s) freed while the head scanned them' <<<"$o" || fail "torn: freed: $o"
grep -q 'INFO flip latch: head 1: empty backs of skipped swaps read again 4 ms later: 5 had pixels' <<<"$o" || fail "torn: empty: $o"
grep -q 'PASS flip latch: head 0: 0 of 200 shown frames changed while on the panel' <<<"$o" || fail "torn: head 0: $o"
grep -q 'INFO flip latch: head 0: 0 of 300 swaps: the previous flip had not latched at the swap; flips latched 250 time(s) seen, avg 10000 us, max 16900 us' <<<"$o" ||
    fail "torn: head 0 latch: $o"
grep -q 'flip latch  *FAIL head 1: 30 of 200' <<<"$o" || fail "torn: the summary line: $o"
grep -q '^RESULT: something failed' <<<"$o" || fail "torn: the result: $o"

# one back buffer: rewritten in place on most swaps (a WARN), a timeout run
mk inplace/rdna3-report-inplace "
swap_latch 1
swaps_h0 100
swaps_latchwait_h0 90
swaps_latchus_h0 900000
swaps_latchmax_h0 35000
swaps_latchtimeout_h0 3
swaps_latchoff_h0 7
swaps_tornchk_h0 100
swaps_torn_h0 1
swaps_inplace_h0 80
"
o=$(run inplace)
! grep -q Traceback <<<"$o" || fail "inplace: a traceback: $o"
grep -q 'WARN flip latch: head 0: 80 of 100 shown frames rewritten in place and shown again (one back buffer' <<<"$o" ||
    fail "inplace: $o"
grep -q 'WARN flip latch: head 0: 1 of 100 shown frames changed while on the panel, then flipped away (torn)$' <<<"$o" ||
    fail "inplace: one torn frame is a WARN: $o"
grep -q 'WARN flip latch: head 0: 0 of 100 swaps: .*3 timeouts, 0 unknown, 7 not waited (swap_latch 1) - 3 flip(s) never latched in 2 frames' <<<"$o" ||
    fail "inplace: the timeouts: $o"
! grep -q 'FAIL flip latch' <<<"$o" || fail "inplace: no FAIL: $o"

# a kernel before the latch counters: no flip latch line at all
mk old/rdna3-report-old "
swaps_h0 100
swaps_hashed_h0 100
swaps_abab_h0 0
cf_shown_h0 10
"
o=$(run old)
! grep -q Traceback <<<"$o" || fail "old: a traceback: $o"
! grep -q 'flip latch' <<<"$o" || fail "old: no flip latch lines: $o"

# the serial log alone
mkdir -p "$T/ser"
cat > "$T/ser/serial-1.log" <<'EOF'
RDNA3Accelerator: swap latch: head 1: 8 swaps; previous flip not latched at entry 0; latched while waited 8 (avg 9000 us, max 16000 us), 0 timeouts, 0 unknown, 0 not waited; shown buffers hashed again 7: 0 changed on the panel then flipped away (torn), 0 rewritten in place (one back), 0 not hashed; copy forwards into the buffer on the panel 0; buffers freed on the panel 0; empty backs read again: 0 filled, 0 still empty (swap_latch 1, card run 2026-10-17)
RDNA3Accelerator: swap #90: head 1: the buffer it showed (phys 0x8b100000) changed while on the panel: bands 0x7e of 8 (bit 0 the top 32 rows); its flip latched 8000 us after it, 12 ms ago -- WRITTEN WHILE SHOWN, now flipped away (torn: the panel showed the writes) (1 such, 60 checked)
RDNA3Accelerator: swap latch: head 1: 120 swaps; previous flip not latched at entry 2; latched while waited 118 (avg 8000 us, max 16500 us), 0 timeouts, 0 unknown, 0 not waited; shown buffers hashed again 119: 50 changed on the panel then flipped away (torn), 0 rewritten in place (one back), 1 not hashed; copy forwards into the buffer on the panel 0; buffers freed on the panel 0; empty backs read again: 1 filled, 3 still empty (swap_latch 1, card run 2026-10-17)
EOF
o=$(python3 "$ROOT/tools/triage_reports.py" "$T/ser" 2>&1 || true)
! grep -q Traceback <<<"$o" || fail "serial: a traceback: $o"
grep -q 'FAIL swap/flicker  *head 1: 50 of 119 shown frames changed while on the panel, then flipped away (torn): 42% - something writes the buffer the panel shows (flip latch, serial at swap 120)' <<<"$o" ||
    fail "serial: the FAIL from the last line: $o"
grep -q 'WARN swap/flicker  *head 1: 2 of 120 swaps: the previous flip had not latched at the swap' <<<"$o" || fail "serial: pending: $o"
grep -q 'swap #90: head 1: the buffer it showed (phys 0x8b100000) changed while on the panel' <<<"$o" || fail "serial: the kernel line: $o"
echo "test_triage_latch: all checks passed"
