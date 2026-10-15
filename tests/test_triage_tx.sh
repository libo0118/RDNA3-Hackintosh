#!/bin/bash
# tools/triage_reports.py's TX lines (what a head sends after its OTG, the
# core's RD_NR_XNU_TXSTATE and TX watch) on synthetic reports: a clean
# look, a look with a problem, a lit look without TX lines, the watch's
# lines from the serial log (deduplicated against the report's log, the
# problems first) and a watch that never ticked. Card run of 2026-10-15:
# the HDMI monitor said "no signal" while its pipe scanned the desktop.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_tx: FAIL $*"; exit 1; }

CHANGE1='RDNA3: display: head 0 TX change #1 at 61234 ms uptime: problems: DIG back end off, DIO FIFO error; 1 register(s): DIG_BE_EN_CNTL 0x101 -> 0x0'
mk() {   # mk <dir> <ticks property or -> <report log>
    mkdir -p "$T/$1"
    python3 -I - "$T/$1/rdna3-report-tx" "$2" "$3" <<'PY'
import io, sys, tarfile
path, ticks, log = sys.argv[1], sys.argv[2], sys.argv[3]
props = '  |   "RDNA3,Head0,TX" = "DIG back end off, DIO FIFO error; 1 change(s) since the start"\n'
if ticks != "-":
    props += '  |   "RDNA3,TXWatchTicks" = %s\n' % ticks
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": props,
    "20-log-rdna3.txt": log.replace("\\n", "\n") + "\n",
    "24-log-coredisplay.txt": "",
}
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
look = """== rdna3_drminfo --scanout (look %s)
pipe 0: LIT otg 0 1920x1080 pitch 1920 fmt 8 sw 0 dcc 0 address 0x800b100000 in use 0x800b100000
  OTG CRC 1a00/a4cd/4776 (what leaves the pipe)
%s  picture 320x180: 100%% not black, mean rgb 84 90 104, 0 bytes changed in 2 s
1 pipe(s) lit
[exit 0]
"""
tx = """  TX head 0: OTG0 -> DIGD -> UNIPHYD
  TX problems: %s
  TX DIG BE / UNIPHYD: DIG_BE_CNTL 0x30803 DIG_BE_EN_CNTL %s
  TX watch: 5 tick(s), %s change(s), 0 busy skip(s)
"""
open(path + "-tests.txt", "w").write(look % ("1", tx % ("none", "0x101", "0")) +
                                     look % ("2", tx % ("DIG back end off, DIO FIFO error", "0x0", "1")) +
                                     look % ("3", ""))
PY
}
mk r - "$CHANGE1"
cat > "$T/r/serial-1.log" <<EOF
RDNA3: display: TX watch: on (each head's output after its OTG: in full now and after the clone, then every 2 s from the statistics tick only what changed; reads only)
RDNA3: display: head 0 TX at the start (as the firmware left it) (5000 ms uptime), OTG0 -> DIGD -> UNIPHYD: problems: none
RDNA3: display: head 0 TX   DIG BE / UNIPHYD: DIG_BE_CNTL 0x30803 DIG_BE_EN_CNTL 0x101
RDNA3: display: head 0 TX after the clone (9000 ms uptime), OTG0 -> DIGD -> UNIPHYD: problems: none
RDNA3: display: head 0 TX   OTG0: OTG_CONTROL 0x10201
RDNA3: display: TX watch: first tick at 20000 ms uptime
$CHANGE1
EOF

o=$(python3 "$ROOT/tools/triage_reports.py" "$T/r" 2>&1 || true)
! grep -q Traceback <<<"$o" || fail "a traceback: $o"
grep -q 'info Head0,TX *= DIG back end off, DIO FIFO error; 1 change(s) since the start' <<<"$o" || fail "the property: $o"
grep -q '  TX DIG BE / UNIPHYD: DIG_BE_CNTL 0x30803 DIG_BE_EN_CNTL 0x101' <<<"$o" || fail "look 1's TX line: $o"
[ "$(grep -c '!! the pixels leave the OTG, but its output is wrong: DIG back end off, DIO FIFO error$' <<<"$o")" = 1 ] ||
    fail "one warning, for look 2 only, problems uncut: $o"
[ "$(grep -c '!! lit pipe(s) but no TX lines' <<<"$o")" = 1 ] || fail "look 3 has no TX lines: $o"
grep -q 'TX watch (7 line(s); the full states' <<<"$o" || fail "the watch's lines: $o"
grep -q '^     display: head 0 TX   OTG0: OTG_CONTROL 0x10201$' <<<"$o" || fail "the after-clone state in full: $o"
grep -q '^     display: head 0 TX change #1 at 61234 ms uptime: problems: DIG back end off' <<<"$o" || fail "the change line: $o"
grep -q 'screens 1 *1 lit$' <<<"$o" || fail "summary look 1: $o"
grep -q 'screens 2 *1 lit, TX: DIG back end off, DIO FIFO error$' <<<"$o" || fail "summary look 2: $o"
grep -q 'screens 3 *1 lit, NO TX LINES$' <<<"$o" || fail "summary look 3: $o"
# the change is in the report's log and in the serial log: counted once
grep -q 'TX watch *head 0: 1 change(s); first with problems at 61234 ms: problems: DIG back end off, DIO FIFO error; 1 register(s): DIG_BE_EN_CNTL 0x101 -> 0x0' <<<"$o" ||
    fail "summary watch: $o"

# a watch that logged its start but never ticked proves nothing
mk n - "RDNA3: display: TX watch: on (each head ...)\\nRDNA3: display: head 0 TX at the start (x) (5000 ms uptime), OTG0 -> DIGD -> UNIPHYD: problems: none"
o=$(python3 "$ROOT/tools/triage_reports.py" "$T/n" 2>&1 || true)
grep -q 'TX watch *NEVER TICKED' <<<"$o" || fail "never ticked: $o"
# ticks in the registry, no change: a real "no change"
mk k 40 "RDNA3: display: TX watch: on (each head ...)"
o=$(python3 "$ROOT/tools/triage_reports.py" "$T/k" 2>&1 || true)
grep -q 'TX watch *no change on any head after the start (40 tick(s))' <<<"$o" || fail "ticked, no change: $o"
echo "test_triage_tx: all checks passed"
