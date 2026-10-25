#!/bin/bash
# Step play (card run 2026-10-17d, `rdna3vm step play`): the desktop is the
# owner's, autoreport.sh runs no test and takes its reports at minute 5 and 10.
#  - tools/ladder-steps.txt: play = the agdc step's boot-args + rdna3play=1
#  - tools/vm/rdna3vm: the step is listed, its auto limit is 30 min (auto_limit)
#  - autoreport.sh's play functions on the host, with stubs: the notification
#    text, three looks and the PM lines at the first moment, the second report
#    (own name, tests file + archive uploaded, the pump left running, no
#    shutdown), and the normal tests / A/B skipped under rdna3play=1
#  - the swap A/B is opt-in (rdna3swapab=1), not part of a normal step
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_play_step: FAIL $*"; exit 1; }
AR=$ROOT/tools/vm/macos/autoreport.sh

# ---- the ladder and the host script
agdc=$(awk '$1 == "agdc" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
play=$(awk '$1 == "play" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ -n "$agdc" ] && [ "$play" = "$agdc rdna3play=1" ] || fail "ladder-steps.txt: play is [$play], agdc [$agdc]"
[ "$(grep -c 'cursormall bisect bisect-plugin bisect-agdc play' "$ROOT/tools/vm/rdna3vm")" = 2 ] || fail "rdna3vm: play is not in both step lists"
# DRIVER-PLAN N45's A/B steps: play plus exactly one AGDC switch, in both step lists
for kv in play-agdcib:rdna3agdcib=3 play-agdclink:rdna3agdclinkflags=1 play-agdcscaler:rdna3agdcscaler=1; do
    n=${kv%%:*}
    s=$(awk -v n="$n" '$1 == n { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
    [ "$s" = "$play ${kv#*:}" ] || fail "ladder-steps.txt: $n is [$s], expected the play step + ${kv#*:}"
    [ "$(grep -c "play-desktop play-agdcib play-agdclink play-agdcscaler" "$ROOT/tools/vm/rdna3vm")" = 2 ] || fail "rdna3vm: $n is not in both step lists"
done
case " $play " in *" rdna3agdcib="*|*" rdna3agdclinkflags="*|*" rdna3agdcscaler="*) fail "the play step sets an N45 switch: [$play]" ;; esac
sed -n '/^auto_limit() {/,/^}/p' "$ROOT/tools/vm/rdna3vm" > "$T/limit.sh"
[ -s "$T/limit.sh" ] || fail "rdna3vm: auto_limit not found"
m=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$play")
[ "$m" = 30 ] || fail "rdna3vm: auto_limit of play is [$m], expected 30"
m=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "rdna3lvl=3")
[ "$m" != 30 ] || fail "rdna3vm: auto_limit of a plain step is also 30 (the play case leaks)"

# ---- autoreport.sh: the switches
grep -q '\*" rdna3play=1 "\*)' "$AR" || fail "autoreport.sh: no rdna3play=1 case"
grep -q '\*" rdna3swapab=1 "\*)' "$AR" || fail "autoreport.sh: the swap A/B is not opt-in (rdna3swapab=1)"
grep -q '== swap A/B: NOT ASKED' "$AR" || fail "autoreport.sh: no 'swap A/B: NOT ASKED' line"
grep -q 'if \[ -z "\$BISECT" \] && \[ -z "\$PLAY" \]; then' "$AR" || fail "autoreport.sh: the tests are not skipped under play"
grep -q 'if \[ -n "\$PLAY" \]; then play_second; exit 0; fi' "$AR" || fail "autoreport.sh: play does not stay up after the second report"

# ---- the play functions, on the host
sed -n '/^# ---- step play (rdna3play=1)/,/^\[ -n "\$win" \] || /p' "$AR" | sed '$d' > "$T/fn.sh"
[ "$(grep -c '^play_[a-z_]*() {' "$T/fn.sh")" = 4 ] || fail "autoreport.sh: play functions not found: $(grep '^play_' "$T/fn.sh")"
mkdir -p "$T/bin" "$T/home" "$T/D/bin" "$T/D/tools/macos"
cat > "$T/bin/sysctl" <<'SC'
#!/bin/sh
[ "$1" = debug ] && { echo "debug.rdna3accel_iop_ok: 5"; echo "debug.rdna3agdc_x: 1"; }
exit 0
SC
cat > "$T/bin/curl" <<'SC'
#!/bin/sh
while [ $# -gt 0 ]; do [ "$1" = -T ] && { echo "$2 -> $3" >> "$STUBLOG"; }; shift; done
exit 0
SC
chmod +x "$T/bin/sysctl" "$T/bin/curl"
# --scanout-hires DIR also leaves a picture, the way the real one does
printf '#!/bin/sh\necho "pipe 0: LIT"\n[ "$1" = --scanout-hires ] && printf "P6\\n1 1\\n255\\n000" > "$2/scanout-pipe0.ppm"\nexit 0\n' > "$T/D/bin/rdna3_drminfo"
printf '#!/bin/sh\necho "SMU: ok"\n' > "$T/D/bin/rdna3_pmctl"
# collect.sh stub: makes the archive the way the real one names it
printf '#!/bin/sh\necho "collect label=$1 stamp=$RDNA3_STAMP" >> "$STUBLOG"\ntouch "$HOME/rdna3-report-$1-$RDNA3_STAMP.tar.gz"\n' > "$T/D/tools/macos/collect.sh"
chmod +x "$T/D/bin/"*
cat > "$T/h.sh" <<'EOF'
. "$HT/fn.sh"
notify() { echo "notify: $*" >> "$STUBLOG"; }
prog() { echo "prog: $*" >> "$STUBLOG"; }
with_limit() { shift; "$@"; }
pend_cp() { cp "$1" "$2" 2>/dev/null || true; }
a3_sysctls() { echo "a3 counters"; }
trace_snapshot() { echo "snapshot $1" >> "$STUBLOG"; echo "== frame trace snapshot $1"; }
hdr() { echo "== $*"; hl=$(wc -l < "$extra" 2>/dev/null || echo 0); }
done_() { echo "[exit $1]"; }
HOME=$HT/home; D=$HT/D; HOST=http://stub; label=play; args="rdna3play=1"; lvl=8
extra=$HOME/rdna3-extra.txt; : > "$extra"; hl=0
PLAY_FIRST=60; PLAY_SECOND=120
sleep() { command sleep 0.01; }
# the clock stub: every call of date +%s moves 61 s on, so both waits end at once
# (a file, not a variable: $(date +%s) runs in a subshell)
echo 1000000000 > "$HT/clock"
date() { if [ "$1" = +%s ]; then t=$(( $(cat "$HT/clock") + 61 )); echo $t > "$HT/clock"; echo $t; else command date "$@"; fi; }
PLAY_T0=$(date +%s)
{ play_first; } >> "$extra" 2>&1
play_second
echo "stamp=$stamp base=$base extra=$extra"
EOF
: > "$T/stub.log"
env HT="$T" STUBLOG="$T/stub.log" PATH="$T/bin:$PATH" bash "$T/h.sh" > "$T/out.txt" 2>&1 || fail "the play functions failed: $(tail -5 "$T/out.txt")"
grep -q '^notify: RDNA3 play: use the desktop; the report is taken at minute 1 and again at minute 2' "$T/stub.log" || fail "no start notification: $(cat "$T/stub.log")"
[ "$(grep -c 'rdna3_drminfo --scanout (look play1' "$T/home/rdna3-extra.txt")" = 3 ] || fail "first moment: not three looks: $(grep 'look' "$T/home/rdna3-extra.txt")"
# one 1280-wide look per report (--scanout-hires, look play<N>h), beside the three 320-wide ones
[ "$(grep -c 'rdna3_drminfo --scanout-hires (look play1h)' "$T/home/rdna3-extra.txt")" = 1 ] || fail "first moment: no hires look: $(grep 'look' "$T/home/rdna3-extra.txt")"
[ "$(grep -c 'rdna3_drminfo --scanout-hires (look play2h)' "$T/home/rdna3-extra-2.txt")" = 1 ] || fail "second report: no hires look: $(grep 'look' "$T/home/rdna3-extra-2.txt")"
[ -f "$T/home/rdna3-scanout/play2h/scanout-pipe0.ppm" ] || fail "the stub's hires picture is missing"
grep -q -- '-scanoutplay2h-pipe0\.ppm$' "$T/stub.log" || fail "the hires picture of the second report was not uploaded: $(cat "$T/stub.log")"
[ -d "$T/home/rdna3-scanout/play1h" ] && [ -d "$T/home/rdna3-scanout/play2h" ] || fail "the hires look directories were not made"
grep -q 'rdna3_pmctl info (play' "$T/home/rdna3-extra.txt" && grep -q 'SMU: ok' "$T/home/rdna3-extra.txt" || fail "no PM lines at the first moment"
grep -q '^snapshot play10$' "$T/stub.log" || fail "no snapshot for the second report"
grep -q '^collect label=play stamp=[0-9]*-[0-9]*$' "$T/stub.log" || fail "collect.sh not run for the second report: $(grep collect "$T/stub.log")"
grep -q 'rdna3-report-play-.*-tests.txt$' "$T/stub.log" || fail "the second tests file was not uploaded: $(cat "$T/stub.log")"
grep -q 'rdna3-report-play-.*\.tar\.gz$' "$T/stub.log" || fail "the second archive was not uploaded"
grep -q 'second report sent' "$T/stub.log" || fail "second report not announced"
[ -f "$T/home/rdna3-extra-2.txt" ] || fail "no second tests file"
! grep -q 'swap A/B\|shutdown\|halt' "$T/out.txt" "$T/stub.log" || fail "play touched the A/B or shut down"
bash -n "$AR" || fail "bash -n"
echo "test_play_step: ok"
