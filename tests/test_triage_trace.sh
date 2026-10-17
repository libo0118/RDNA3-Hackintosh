#!/bin/bash
# The per-frame trace (card run 2026-10-17b, common/rdna3_trace.h) through
# tools/rdna3_trace.py and tools/triage_reports.py: a synthetic trace per
# hypothesis (tests/trace_fixture.py) must be named as that hypothesis -
# H-B2 (a back that never receives a frame), H-COPY (the buffer on the
# panel rewritten in place), H-FREE (VRAM freed while scanned), a copy
# forward error (CRITICAL) - the A/B side by side must judge mode 1, and the
# coverage self-check must FAIL "trace incomplete" for a missing HUBP
# address (Q6), events lost in the A/B window (Q8) and a report of a kernel
# with the trace but no trace in it; the mode 1 counters' verdict and
# CoreDisplay's "copy forward failed" (CRITICAL) too.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_trace: FAIL $*"; exit 1; }
has() { grep -qF -- "$2" <<<"$1" || fail "$3: '$2' not in: $1"; }
hasnt() { ! grep -qF -- "$2" <<<"$1" || fail "$3: '$2' unexpected in: $1"; }
dec() { python3 -I "$ROOT/tests/trace_fixture.py" "$1" "$T/$1.bin" && python3 "$ROOT/tools/rdna3_trace.py" "$T/$1.bin"; }

out=$(dec b2)
has "$out" "FAIL     H-B2: head 0: the back 0x8b8f0000 was named by 40 swaps and read EMPTY at 40" b2
has "$out" "WindowServer's frames land in 0x8b100000" b2
has "$out" "PASS     Q3: 40 empty backs, 40 delayed re-samples" b2
hasnt "$out" "H-FREE" b2
hasnt "$out" "CF-ERROR" b2

out=$(dec copy)
has "$out" "FAIL     H-COPY: mode 0: 39 of 39 flipped buffers were rewritten" copy
hasnt "$out" "H-B2" copy

out=$(dec free)
has "$out" "WARN     H-FREE: 1 buffer(s) freed while a head scanned them" free
hasnt "$out" "FAIL     H-COPY" free

out=$(dec cferr)
has "$out" "CRITICAL CF-ERROR: 1 copy forward(s) answered an error" cferr
hasnt "$out" "H-COPY" cferr

out=$(dec ab)
has "$out" "PASS     A/B verdict: the copy (mode 1) showed no black frame" ab
has "$out" "mode 1 (A: mode 1)" ab
has "$out" "mode 0 (B: mode 0)" ab
has "$out" "PASS     Q9: 1 command submissions" ab
has "$out" "PASS     Q8: 2 stream reads, 0 events lost" ab

out=$(dec clean)
hasnt "$out" "FAIL" clean
hasnt "$out" "WARN" clean

out=$(dec nopanel)
has "$out" "FAIL     trace incomplete: Q6" nopanel
out=$(dec lost)
has "$out" "FAIL     trace incomplete: Q8 (2 stream reads, 25 events lost" lost

# a serial log alone: the kernel's "tr" lines (RD3_TRACE_LINE_FMT) decode to the same events
out=$(python3 -I - "$ROOT/tools" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import rdna3_trace as r
line = ("RDNA3Accelerator: tr 0 7 123456789 1 0 1 d 1a 3 5543 0 0 100 87040000 8b100000 8b100000 "
        "0 1716151413121110\n")
t = r.Trace()
print(t.add_lines("noise\n" + line + line))   # the same event twice: once
t.finish()
e = t.events[0]
print(len(t.events), e.name, e.seq, e.t, e.head, e.arg, hex(e.flags), hex(e.pb), e.bb.hex(), (e.v1 >> 4) & 0xf)
PY
)
has "$out" "1 SWAP 7 123456789 0 1 0xd 0x8b100000 1011121314151617 4" tr-lines

# the same through triage: a report whose kernel runs these sources
HASH=$(cd "$ROOT" && python3 -I -c 'import sys; sys.path.insert(0, "tools"); import triage_reports as t; print(t.fb_source_hash(t.ACCEL_HASH_FILES))')
mk() {   # mk <name> <trace kind or -> <tests counters> [coredisplay line]
    mkdir -p "$T/$1"
    [ "$2" = - ] || python3 -I "$ROOT/tests/trace_fixture.py" "$2" "$T/$1/trace.bin"
    python3 -I - "$T/$1" "$2" "$HASH" "$3" "${4:-}" <<'PY'
import io, os, sys, tarfile
d, kind, h, counters, cd = sys.argv[1:6]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": "RDNA3Accelerator: build %s\nRDNA3Accelerator: IOGraphicsAccelerator2::start -> 1\n" % h,
    "24-log-coredisplay.txt": cd + "\n",
}
base = os.path.join(d, "rdna3-report-x-20261017-120000")
with tarfile.open(base + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
    if kind != "-":
        data = open(os.path.join(d, "trace.bin"), "rb").read()
        ti = tarfile.TarInfo("rdna3-report/36-frame-trace.bin")
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
dump = "".join("debug.rdna3accel_%s: %s\n" % tuple(l.split()) for l in counters.strip().splitlines() if l.strip())
open(base + "-tests.txt", "w").write("== A3 display counters after M6\n" + dump)
PY
}
run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }

mk b2 b2 "swaps_h0 80
cf_calls 1
swap_mode 0"
out=$(run b2)
has "$out" "FAIL frame trace: H-B2: head 0: the back 0x8b8f0000" triage-b2
has "$out" "frame trace timeline" triage-b2

mk none - "swaps_h0 80
swap_mode 1"
out=$(run none)
has "$out" "FAIL frame trace: trace incomplete: Q1-Q11" triage-none

mk cd clean "swaps_h0 40
swap_mode 1
swaps_copy_h0 40
swaps_copyempty_h0 3
swaps_copyblack_h0 10
swaps_copyfail_h0 0" "2026-10-17 13:20:15.056 E  WindowServer[158:29f] [com.apple.CoreDisplay:default] [ERROR] - copy_forward_region: copy forward failed (0xe00002bc) ... disabling."
out=$(run cd)
has "$out" "CRITICAL copy forward: CoreDisplay disabled copy forward at 2026-10-17 13:20:15.056 (0xe00002bc)" triage-cd
has "$out" "the family failed before calling it" triage-cd
has "$out" "FAIL swap copy: head 0: 40 backs copied" triage-cd
has "$out" "10 black frames" triage-cd

echo "test_triage_trace: all checks passed"
