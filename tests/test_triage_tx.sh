#!/bin/bash
# tools/triage_reports.py's TX lines (what a head sends after its OTG, the
# core's RD_NR_XNU_TXSTATE and TX watch) on a synthetic report: a clean
# look, a look with a problem, the watch's lines from the serial log and
# the summary line. Card run of 2026-10-15: the HDMI monitor said "no
# signal" while its pipe scanned the desktop.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_tx: FAIL $*"; exit 1; }

mkdir -p "$T/r"
python3 -I - "$T/r/rdna3-report-tx" <<'PY'
import io, sys, tarfile
path = sys.argv[1]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": '    | |   "RDNA3,Head0,TX" = "DIG back end off; 1 change(s) since the start"\n',
    "20-log-rdna3.txt": "RDNA3: hello\n",
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
  TX head 0
  TX OTG0: OTG_CONTROL 0x10201 V_TOTAL 0x464
  TX DIG BE / UNIPHYD: DIG_BE_CNTL 0x30803 DIG_BE_EN_CNTL %s
  TX problems: %s; %s change(s) seen by the driver's watch
  picture 320x180: 100%% not black, mean rgb 84 90 104, 0 bytes changed in 2 s
1 pipe(s) lit
[exit 0]
"""
open(path + "-tests.txt", "w").write(look % ("1", "0x101", "none", "0") +
                                     look % ("2", "0x0", "DIG back end off", "1"))
PY
cat > "$T/r/serial-1.log" <<'EOF'
RDNA3: display: head 0 TX at the start (as the firmware left it) (5000 ms after the start): problems: none
RDNA3: display: head 0 TX   DIG BE / UNIPHYD: DIG_BE_CNTL 0x30803 DIG_BE_EN_CNTL 0x101
RDNA3: display: head 0 TX change #1 at 61234 ms after the start: 1 register(s): DIG_BE_EN_CNTL 0x101 -> 0x0; problems: DIG back end off
EOF

o=$(python3 "$ROOT/tools/triage_reports.py" "$T/r" 2>&1 || true)
! grep -q Traceback <<<"$o" || fail "a traceback: $o"
grep -q 'info Head0,TX *= DIG back end off; 1 change(s) since the start' <<<"$o" || fail "the property: $o"
grep -q '  TX DIG BE / UNIPHYD: DIG_BE_CNTL 0x30803 DIG_BE_EN_CNTL 0x101' <<<"$o" || fail "look 1's TX line: $o"
[ "$(grep -c '!! the pixels leave the OTG, but its output is wrong: DIG back end off' <<<"$o")" = 1 ] ||
    fail "one warning, for look 2 only: $o"
grep -q 'TX watch (3 line(s); the first 60):' <<<"$o" || fail "the watch's lines: $o"
grep -q 'display: head 0 TX change #1 at 61234 ms' <<<"$o" || fail "the change line: $o"
grep -q 'screens 1 *1 lit$' <<<"$o" || fail "summary look 1: $o"
grep -q 'screens 2 *1 lit, TX: DIG back end off' <<<"$o" || fail "summary look 2: $o"
grep -q 'TX watch *head 0: 1 change(s), first at 61234 ms: 1 register(s): DIG_BE_EN_CNTL 0x101 -> 0x0' <<<"$o" ||
    fail "summary watch: $o"
echo "test_triage_tx: all checks passed"
