#!/bin/bash
# DRIVER-PLAN N43 (2): autoreport.sh's membw_load (a memory load in the background, `rdna3_pmctl info` read three
# times during it and once after) run on the host with a stub rdna3_vktest and rdna3_pmctl, and what it prints read by
# tools/triage_reports.py's n43_vktest_rows: UCLK climbing under the load is INFO, UCLK stuck at 96 MHz with a busy
# memory controller is a WARN; a load that dies gives its exit code and still prints what it had. Also: autoreport.sh
# parses (bash -n, dash -n), runs the load only with rdna3pm=2 and prints a skip line otherwise.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_membw_load: FAIL $*"; exit 1; }
AR="$ROOT/tools/vm/macos/autoreport.sh"
bash -n "$AR" || fail "bash -n autoreport.sh"
if command -v dash >/dev/null 2>&1; then dash -n "$AR" || fail "dash -n autoreport.sh"; fi

# the gate: rdna3pm=2 only, one line for every other step
grep -q '^    \*" rdna3pm=2 "\*)' "$AR" || fail "the memory load is not gated on rdna3pm=2"
grep -q '== memory load: skipped (rdna3pm=2 is not set' "$AR" || fail "no skip line for a step without rdna3pm=2"
grep -q 'rdna3 memory load (rdna3_vktest --membw' "$AR" || fail "no header for the memory load section"

{ sed -n '/^with_limit() {/,/^}/p' "$AR"; sed -n '/^membw_load() {/,/^}/p' "$AR"; } > "$T/fn.sh"
[ "$(wc -l < "$T/fn.sh")" -gt 20 ] || fail "with_limit / membw_load not found in autoreport.sh"

mkdir -p "$T/bin"
cat > "$T/bin/rdna3_vktest" <<'STUB'
#!/bin/sh
# the stub load: allocates, prints the first MEMBW: line, "loads" for 3 s, prints the result
[ "$1" = --membw=10 ] || { echo "stub vktest: unexpected arguments: $*"; exit 2; }
echo "MEMBW: 2 x 8 MiB device-local (heap 16128 MiB), queue family 0, 10 s"
[ -n "$MEMBW_DIE" ] && exit 1
sleep 3
echo "MEMBW: 100 submit(s) of 8 fill + copy rounds, 4800 GiB of VRAM traffic in 10.0 s = 480.0 GiB/s"
echo "ALL PASSED: memory load"
STUB
cat > "$T/bin/rdna3_pmctl" <<'STUB'
#!/bin/sh
# the stub pmctl: the first three calls are "under load", the fourth is idle
n=$(cat "$STUBDIR/n" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$STUBDIR/n"
now=96; umc=1
if [ "$n" -le 3 ]; then umc=70; [ "$STUB_UCLK" = stuck ] || now=$((n == 1 ? 96 : n == 2 ? 456 : 772)); fi
echo "gates: read-only"
echo "pp_dpm_sclk (now 2000 MHz): 0: 500Mhz 1: 2000Mhz *"
echo "pp_dpm_mclk (now $now MHz): 0: 96Mhz $([ $now = 96 ] && echo '*') 1: 456Mhz $([ $now = 456 ] && echo '*') 2: 772Mhz $([ $now = 772 ] && echo '*') 3: 1218Mhz"
echo "pp_dpm_fclk (now 601 MHz): 0: 601Mhz *"
echo "  activity gfx 40 % umc $umc % mm 0 %, 80 W, gfxclk 2000 MHz, uclk $now MHz, socclk 1000 MHz"
echo "thermal: edge 50 C"
STUB
chmod +x "$T/bin/rdna3_vktest" "$T/bin/rdna3_pmctl"
: > "$T/bin/libvulkan_radeon.dylib"

run() {   # run <name> <env...>: the function's output, the exit code on the last line
    rm -f "$T/n"
    ( export D="$T" STUBDIR="$T"; export "${@:2}"
      . "$T/fn.sh"
      membw_load && rc=0 || rc=$?; echo "[exit $rc]" ) > "$T/$1.out" 2>&1
}

run climb STUB_UCLK=climb MEMBW_DIE=
grep -q '^-- pmctl under load 1$' "$T/climb.out" || { cat "$T/climb.out"; fail "no first load sample"; }
grep -q '^-- pmctl under load 3$' "$T/climb.out" || fail "no third load sample"
grep -q '^-- pmctl idle after the load$' "$T/climb.out" || fail "no idle sample"
grep -q '^MEMBW: 100 submit(s)' "$T/climb.out" || { cat "$T/climb.out"; fail "the load's result is not in the output"; }
grep -q '^\[exit 0\]$' "$T/climb.out" || { cat "$T/climb.out"; fail "exit code"; }
grep -q '^== ' "$T/climb.out" && fail "a '==' line inside the section would end it for the triage"
grep -q 'thermal' "$T/climb.out" && fail "unfiltered pmctl output in the section"
[ ! -e /tmp/rdna3-membw.txt ] || fail "the load's temp file was left behind"

verdict() {   # verdict <name> <expected level>
    out=$(python3 -I -B - "$T/$1.out" <<'PY'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
import triage_reports as tr
body = open(sys.argv[1]).read()
txt = "== rdna3_vktest\nALL PASSED: 1\n\n== rdna3 memory load (rdna3_vktest --membw, rdna3_pmctl info x3 during it)\n" + body
print(";".join(r[0] for r in tr.n43_vktest_rows(txt, True) if r[1] == "memory load"))
PY
)
    [ "$out" = "$2" ] || { cat "$T/$1.out"; fail "$1: triage said '$out', expected '$2'"; }
}
export ROOT
verdict climb INFO

run stuck STUB_UCLK=stuck MEMBW_DIE=
verdict stuck WARN
run die STUB_UCLK=climb MEMBW_DIE=1
grep -q '^\[exit 1\]$' "$T/die.out" || { cat "$T/die.out"; fail "a load that died: its exit code is not returned"; }
grep -q '^-- pmctl idle after the load$' "$T/die.out" || fail "a load that died: no idle sample"
echo "test_membw_load: ok"
