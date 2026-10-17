#!/bin/bash
# tools/triage_reports.py's flip latch lines (card run 2026-10-17: windows
# and menus flicker; the swap returned before its flip latched) on synthetic
# reports: frequent torn frames (a FAIL), a clean head, the previous flip
# pending and copy forwards into the buffer on the panel (WARNs), one back
# buffer rewritten in place, waits off, a wait that never saw a pending
# flip, the torn test not measured, keys missing, the archive's
# 29-surface-vram.txt without a tests file, a kernel without the counters
# (no lines, or 'counters missing' when it runs these sources), and the
# kernel's 'swap latch: head N' line in a serial log.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_latch: FAIL $*"; exit 1; }
HASH=$(cd "$ROOT" && python3 -I -c 'import sys; sys.path.insert(0, "tools"); import triage_reports as t; print(t.fb_source_hash(t.ACCEL_HASH_FILES))')

# every per-head counter of the kernel at 0 for heads 0 and 1, then the
# given 'key value' lines (without debug.rdna3accel_) on top
ZERO=""
for h in 0 1; do
    for k in swaps_latchpend swaps_latchwait swaps_latchus swaps_latchmax swaps_latchimmediate swaps_latchtimeout \
             swaps_latchunknown swaps_latchoff swaps_latchwaitsoff swaps_flipfailed swaps_tornchk swaps_torn \
             swaps_inplace swaps_tornskip cf_onpanel swaps_shownfreed swaps_emptyfilled swaps_emptystill; do
        ZERO+="${k}_h$h 0"$'\n'
    done
done

mk() {   # mk <dir/name> <counters> [where: tests | sv | none] [log]
    mkdir -p "$T/$(dirname "$1")"
    python3 -I - "$T/$1" "$2" "${3:-tests}" "${4:-RDNA3: hello}" <<'PY'
import io, sys, tarfile
path, counters, where, log = sys.argv[1:5]
vals = {}
for l in counters.strip().splitlines():
    if l.strip():
        k, v = l.split()
        vals[k] = v
dump = "".join("debug.rdna3accel_%s: %s\n" % kv for kv in vals.items())
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": log + "\n",
    "24-log-coredisplay.txt": "",
}
if where == "sv":
    members["29-surface-vram.txt"] = "== sysctl (card run 58)\n" + dump
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
if where == "tests":
    open(path + "-tests.txt", "w").write("== A3 display counters after M6\n" + dump)
PY
}
run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }
has() { grep -qF -- "$2" <<<"$1" || fail "$3: '$2' not in: $1"; }
hasnt() { ! grep -qF -- "$2" <<<"$1" || fail "$3: '$2' unexpected in: $1"; }

# head 1 torn in 15 % of 200 checks, the previous flip pending at 40 swaps,
# 3 copy forwards into the buffer on the panel, sampled empty backs that
# filled; head 0 clean
TORN="${ZERO}swap_latch 0
swap_torn 1
swaps_h0 300
swaps_h1 300
swaps_latchwait_h0 250
swaps_latchus_h0 2500000
swaps_latchmax_h0 16900
swaps_latchimmediate_h0 50
swaps_tornchk_h0 200
swaps_latchpend_h1 40
swaps_latchwait_h1 40
swaps_latchus_h1 200000
swaps_latchmax_h1 9000
swaps_latchimmediate_h1 260
swaps_tornchk_h1 200
swaps_torn_h1 30
cf_onpanel_h1 3
swaps_shownfreed_h1 2
swaps_emptyfilled_h1 5
swaps_emptystill_h1 19
swaps_flipfailed_h1 4"
mk torn/rdna3-report-torn "$TORN"
o=$(run torn)
hasnt "$o" Traceback torn
has "$o" 'FAIL flip latch: head 1: 30 of 200 shown frames changed while on the panel, then flipped away (torn): 15%' torn
has "$o" 'WARN flip latch: head 1: 40 of 300 swaps: the previous flip had not latched at the swap; flips waited for 40 time(s), avg 5000 us, max 9000 us; 260 latched at once; 0 timeouts, 0 unknown, 0 not waited (swap_latch 0) - the panel still read the buffer WindowServer had back' torn
has "$o" 'WARN flip latch: debug.rdna3accel_swap_latch was 0' torn
has "$o" 'WARN flip latch: head 1: 3 copy forward(s) wrote into the buffer on the panel' torn
has "$o" 'WARN flip latch: head 1: 4 flip(s) refused by the framebuffer' torn
has "$o" 'INFO flip latch: head 1: 2 buffer(s) freed while the head scanned them' torn
has "$o" 'WARN flip latch: head 1: sampled empty backs of skipped swaps, read again 4 ms later: 5 had pixels (frames landing after the swap' torn
has "$o" 'PASS flip latch: head 0: 0 of 200 shown frames changed while on the panel' torn
has "$o" 'INFO flip latch: head 0: 0 of 300 swaps: the previous flip had not latched at the swap; flips waited for 250 time(s), avg 10000 us, max 16900 us' torn
hasnt "$o" 'counters missing' torn
grep -q 'flip latch  *FAIL head 1: 30 of 200' <<<"$o" || fail "torn: the summary line: $o"
grep -q '^RESULT: something failed' <<<"$o" || fail "torn: the result: $o"

# the same counters only in the archive's 29-surface-vram.txt (collect.sh
# without autoreport: no tests file)
mk sv/rdna3-report-sv "$TORN" sv
o=$(run sv)
hasnt "$o" Traceback sv
has "$o" 'FAIL flip latch: head 1: 30 of 200 shown frames changed while on the panel' sv
has "$o" 'INFO flip latch: (counters from 29-surface-vram.txt: no tests file dump)' sv
# neither: said
mk none/rdna3-report-none "" none
o=$(run none)
has "$o" 'INFO flip latch: not measured: no rdna3accel counters in the tests file or 29-surface-vram.txt' none

# one back buffer rewritten in place, a timeout run that turned the waits
# off, one torn frame (a WARN)
mk inplace/rdna3-report-inplace "${ZERO}swap_latch 1
swaps_h0 100
swaps_latchwait_h0 90
swaps_latchus_h0 900000
swaps_latchmax_h0 41000
swaps_latchtimeout_h0 3
swaps_latchoff_h0 7
swaps_latchwaitsoff_h0 1
swaps_tornchk_h0 100
swaps_torn_h0 1
swaps_inplace_h0 80
swaps_h1 0"
o=$(run inplace)
hasnt "$o" Traceback inplace
has "$o" 'WARN flip latch: head 0: 80 of 100 shown frames rewritten in place and shown again (one back buffer' inplace
grep -q 'WARN flip latch: head 0: 1 of 100 shown frames changed while on the panel, then flipped away (torn)$' <<<"$o" ||
    fail "inplace: one torn frame is a WARN: $o"
has "$o" '3 timeouts, 0 unknown, 7 not waited (swap_latch 1) - 3 flip(s) never latched in time, 7 flip(s) not waited for (waits off after timeouts, or given up)' inplace
has "$o" 'WARN flip latch: head 0: its swaps were NOT waiting for their flips at the end' inplace
hasnt "$o" 'FAIL flip latch' inplace

# the wait that did nothing (every flip latched at once), the torn test
# not measured, keys missing on head 1
mk idle/rdna3-report-idle "${ZERO}swap_latch 1
swap_torn 0
swaps_h0 500
swaps_latchimmediate_h0 500
swaps_h1 100
swaps_latchpend_h1 0"
o=$(python3 - "$T/idle/rdna3-report-idle-tests.txt" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r"debug\.rdna3accel_swaps_(?:latchmax|tornchk)_h1: 0\n", "", s)
open(p, "w").write(s)
PY
run idle)
hasnt "$o" Traceback idle
has "$o" 'the flip state never read pending - the wait did nothing' idle
# swap_torn=0 set deliberately: not measured is INFO; with the test on, WARN
has "$o" 'INFO flip latch: head 0: torn test not measured: 0 checks in 500 swaps (swap_torn 0, 0 skipped)' idle
has "$o" 'WARN flip latch: head 1: counters missing (unknown, not 0): swaps_latchmax, swaps_tornchk' idle
has "$o" 'max ? us' idle
mk idle2/rdna3-report-idle2 "${ZERO}swap_latch 1
swap_torn 1
swaps_h0 500
swaps_latchwait_h0 400
swaps_h1 0"
o=$(run idle2)
has "$o" 'WARN flip latch: head 0: torn test not measured: 0 checks in 500 swaps (swap_torn 1, 0 skipped)' idle2

# a kernel before the latch counters: no flip latch line - unless the
# kernel log says it runs these sources ('counters missing')
OLD="swaps_h0 100
swaps_hashed_h0 100
swaps_abab_h0 0
cf_shown_h0 10"
mk old/rdna3-report-old "$OLD"
o=$(run old)
hasnt "$o" Traceback old
hasnt "$o" 'flip latch' old
mk cur/rdna3-report-cur "$OLD" tests "RDNA3Accelerator: build $HASH"
o=$(run cur)
has "$o" 'WARN flip latch: counters missing: the accelerator runs these sources and head(s) 0 swapped' cur

# the serial log alone
mkdir -p "$T/ser"
cat > "$T/ser/serial-1.log" <<'EOF'
RDNA3Accelerator: swap latch: a swap returns only once its flip latched
RDNA3Accelerator: swap latch: head 1 (core head 1): refresh 60000 mHz, frame 16666 us -> a swap waits at most 41665 us after its flip
RDNA3-fb: flip to VRAM 0x8b100000: the previous flip still pending after 20 ms -- flipped anyway (1 such)
RDNA3Accelerator: swap: surface <ptr> -> head 0 (only head piped; a1 phys 0x1 a2 phys 0x2, front a1, src a2)
RDNA3Accelerator: swap latch: head 1: 8 swaps; previous flip not latched at entry 0; latched while waited 8 (avg 9000 us, max 16000 us), 0 timeouts, 0 unknown, 0 not waited; shown buffers hashed again 7: 0 changed on the panel then flipped away (torn), 0 rewritten in place (one back), 0 not hashed; copy forwards into the buffer on the panel 0; buffers freed on the panel 0; empty backs read again: 0 filled, 0 still empty (swap_latch 1, card run 2026-10-17); latched at once 0, flips failed 0, waits on
RDNA3Accelerator: swap #90: head 1: the buffer it showed (phys 0x8b100000) changed while on the panel: bands 0x7e of 8 (bit 0 the top 32 rows); its flip latched 8000 us after it, 12 ms ago -- WRITTEN WHILE SHOWN, now flipped away (torn: the panel showed the writes) (1 such, 60 checked)
RDNA3Accelerator: swap #91: head 1: the flip state could not be read after the flip: the core refused rdna3DisplayStatus (0xe00002c2) -- not waited for (1 such)
RDNA3Accelerator: swap latch: head 1: 120 swaps; previous flip not latched at entry 2; latched while waited 118 (avg 8000 us, max 16500 us), 0 timeouts, 1 unknown, 0 not waited; shown buffers hashed again 119: 50 changed on the panel then flipped away (torn), 0 rewritten in place (one back), 1 not hashed; copy forwards into the buffer on the panel 0; buffers freed on the panel 0; empty backs read again: 1 filled, 3 still empty (swap_latch 1, card run 2026-10-17); latched at once 0, flips failed 0, waits on
EOF
echo 'RDNA3Accelerator: swap latch: head 2: 120 swaps; previous flip not latched at entry 0; latched while waited 0 (avg 0 us, max 0 us), 3 timeouts, 0 unknown, 117 not waited; shown buffers hashed again 0: 0 changed on the panel then flipped away (torn), 0 rewritten in place (one back), 0 not hashed; copy forwards into the buffer on the panel 0; buffers freed on the panel 0; empty backs read again: 0 filled, 0 still empty (swap_latch 1, card run 2026-10-17); latched at once 0, flips failed 0, waits OFF (timeouts)' >> "$T/ser/serial-1.log"
o=$(python3 "$ROOT/tools/triage_reports.py" "$T/ser" 2>&1 || true)
hasnt "$o" Traceback serial
grep -q 'FAIL swap/flicker  *head 1: 50 of 119 shown frames changed while on the panel, then flipped away (torn): 42% - something writes the buffer the panel shows (flip latch, serial at swap 120)' <<<"$o" ||
    fail "serial: the FAIL from the last line: $o"
grep -q 'WARN swap/flicker  *head 1: 2 of 120 swaps: the previous flip had not latched at the swap' <<<"$o" || fail "serial: pending: $o"
has "$o" 'swap #90: head 1: the buffer it showed (phys 0x8b100000) changed while on the panel' serial
has "$o" 'swap #91: head 1: the flip state could not be read after the flip' serial
has "$o" "INFO swap/flicker         head 0: flip latch not measured (no 'swap latch: head 0' line in the log)" serial
hasnt "$o" 'counters missing' serial
grep -q 'WARN swap/flicker  *head 2: its swaps were NOT waiting for their flips at the end' <<<"$o" || fail "serial: waits off: $o"
has "$o" 'swap latch: head 1 (core head 1): refresh 60000 mHz' serial
has "$o" 'swap latch: a swap returns only once its flip latched' serial
has "$o" 'flip to VRAM 0x8b100000: the previous flip still pending after 20 ms' serial
echo "test_triage_latch: all checks passed"
