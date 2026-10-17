#!/bin/bash
# Step bisect (card run 2026-10-17d): the bisect_run block of
# tools/vm/macos/autoreport.sh on the host, with stubs for sysctl, say,
# afplay, killall, rdna3_drminfo and the report's helper functions:
#  - timer mode: 13 steps, each switch set and read back as the plan says
#    (step 1 every new feature off, then a3, flicker, swap_latch, swap_torn,
#    swap_mode, trace_scan, swap_stale back on, oldest first; step 8 and the
#    final step 12 = step 0), the three controls, the originals restored, the
#    timetable, `say "step N"` and N beeps per step, two looks and the
#    counters per step, no answers
#  - tty mode (a key file stands in for the keyboard): y / n / Enter / nothing
#    per step, the step ends early after its evidence
#  - a switch that does not take is a loud `failed:` line, the run goes on and
#    puts everything back; a kernel without a sysctl skips that switch; a
#    kernel without swap_mode aborts
#  - the plugin variant: rdna3mtl.conf and the allow list edited, WindowServer
#    restarted twice, both files exactly as they were afterwards (also when
#    there was no conf), a dead run's files put back
#  - the same table from tools/triage_bisect.py
# plus bash -n / dash -n of the scripts the step touches.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_bisect_step: FAIL $*"; exit 1; }

for f in tools/vm/macos/autoreport.sh tools/macos/collect.sh; do bash -n "$ROOT/$f" || fail "bash -n $f"; done
bash -n "$ROOT/tools/vm/rdna3vm" || fail "bash -n rdna3vm"
if command -v dash >/dev/null 2>&1; then
    for f in tools/vm/macos/autoreport.sh tools/macos/collect.sh; do dash -n "$ROOT/$f" || fail "dash -n $f"; done
fi

# the block, cut out of the script that runs on the card
sed -n '/^# BISECT-BEGIN/,/^# BISECT-END/p' "$ROOT/tools/vm/macos/autoreport.sh" > "$T/block.sh"
[ "$(wc -l < "$T/block.sh")" -gt 150 ] || fail "the BISECT-BEGIN/END block was not found in autoreport.sh"

# --- the stubs
mkdir -p "$T/bin" "$T/sys" "$T/D/bin" "$T/home"
cat > "$T/bin/sysctl" <<'EOF'
#!/bin/sh
# a file per oid in $SYSDIR; refuse.<oid> makes -w fail; `sysctl debug` lists them and counts swaps up
d=$SYSDIR
case "$1" in
    -n) [ -f "$d/$2" ] && { cat "$d/$2"; exit 0; }; echo "sysctl: unknown oid '$2'" >&2; exit 1 ;;
    -w) oid=${2%%=*}; val=${2#*=}
        [ -f "$d/$oid" ] || { echo "sysctl: unknown oid '$oid'" >&2; exit 1; }
        [ -f "$d/refuse.$oid" ] && { echo "sysctl: $oid: Operation not permitted" >&2; exit 1; }
        old=$(cat "$d/$oid"); echo "$val" > "$d/$oid"; echo "$oid: $old -> $val"; exit 0 ;;
    debug)
        n=$(cat "$d/.calls" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$d/.calls"
        echo $((n * 10)) > "$d/debug.rdna3accel_swaps_h0"; echo $((n * 7)) > "$d/debug.rdna3accel_swaps_h1"
        for f in "$d"/debug.*; do case $f in */refuse.*) continue ;; esac; echo "$(basename "$f"): $(cat "$f")"; done; exit 0 ;;
    debug.*) [ -f "$d/$1" ] && { echo "$1: $(cat "$d/$1")"; exit 0; }; exit 1 ;;
esac
exit 0
EOF
for c in afplay say killall caffeinate pkill; do
    printf '#!/bin/sh\necho "%s $*" >> "$STUBLOG"\nexit 0\n' "$c" > "$T/bin/$c"
done
chmod +x "$T/bin/"*
cat > "$T/D/bin/rdna3_drminfo" <<'EOF'
#!/bin/sh
# --scanout DIR: two lit pipes; a picture and a count of changed bytes that differ per call
[ "$1" = --scanout ] || exit 2
dir=$2; mkdir -p "$dir"
n=$(cat "$STUBDIR/looks" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$STUBDIR/looks"
echo "pipe 0: LIT otg 0 1920x1080 pitch 1920 fmt 0 sw 0 dcc 0 address 0x1000 in use 0x1000"
echo "  picture 320x180: 45% not black, mean rgb 90 90 90, $((n % 2 * 5000)) bytes changed in 2 s"
echo "  -> $dir/scanout-pipe0.ppm"
echo "pipe 1: LIT otg 1 1920x1080 pitch 1920 fmt 0 sw 0 dcc 0 address 0x2000 in use 0x2000"
echo "  picture 320x180: 30% not black, mean rgb 60 60 60, 0 bytes changed in 2 s"
echo "2 pipe(s) lit"
printf 'P6\n2 2\n255\n\200\200\200\200\200\200\200\200\200\200\200\200' > "$dir/scanout-pipe0.ppm"
exit 0
EOF
chmod +x "$T/D/bin/rdna3_drminfo"
: > "$T/sound.aiff"

mkoids() {   # mkoids <dir> [omit oid ...]: the accelerator's switches at their defaults
    local d=$1; shift
    rm -rf "$d"; mkdir -p "$d"
    for kv in a3=1 flicker=1 swap_latch=1 swap_torn=1 swap_mode=1 trace_scan=1 swap_stale=1 swap_landed=0 a3ws=0 \
              swaps_h0=0 swaps_h1=0 swap_trace_events=5 swaps_stalehold_h0=0 swaps_stalehold_would_h0=0 swaps_copyempty_h0=0; do
        echo "${kv#*=}" > "$d/debug.rdna3accel_${kv%%=*}"
    done
    for o in "$@"; do rm -f "$d/debug.rdna3accel_$o"; done
}

# the harness: what the rest of autoreport.sh gives the block
cat > "$T/h.sh" <<'EOF'
. "$HT/block.sh"
prog() { echo "$*" >> "$HT/prog.log"; }
notify() { echo "$*" >> "$HT/notify.log"; }
in_session() { "$@"; }
with_limit() { shift; "$@"; }
hdr() { echo "== $*"; }
a3_sysctls() { sysctl debug 2>/dev/null | grep -E "rdna3accel_(a3|flicker|swaps_|swap_)"; }
trace_snapshot() { echo "snapshot $1" >> "$HT/prog.log"; }
restore_sleep() { :; }
STANDALONE=1; HOST=; base=rdna3-report-test; TS=$HT/ts; D=$HT/D; mkdir -p "$TS"; echo 12345 > "$TS/trace-stream.bin"
HOME=$HT/home; export HOME
bisect_run "$VARIANT"
echo "rc=$?"
EOF

run() {   # run <variant> <name> [env...]: the block under sh (or $SH) with the stubs, output in $T/<name>.out
    local v=$1 name=$2; shift 2
    rm -rf "$T/$name" "$T/home"; mkdir -p "$T/$name/home" "$T/home"
    : > "$T/stub.log"; rm -f "$T/notify.log" "$T/prog.log" "$T/sys/looks"
    env HT="$T" STUBLOG="$T/stub.log" STUBDIR="$T/sys" SYSDIR="$T/sys" PATH="$T/bin:$PATH" VARIANT="$v" \
        BISECT_HOLD=${HOLDV:-0} BISECT_LEAD=0 BISECT_AMWAIT=0 BISECT_BEEPGAP=0 BISECT_SOUND="$T/sound.aiff" BISECT_USER=tester \
        BISECT_OUT="$T/$name" "$@" ${SH:-sh} "$T/h.sh" > "$T/$name.out" 2>&1 < /dev/null || true
}
val() {   # val <file> <step> <key>: a header line of a step block
    awk -v s="== step $2" -v k="$3:" '$0 == s { on = 1; next } /^== (end )?step/ { on = 0 } on && index($0, k) == 1 { sub(k " ?", ""); print; exit }' "$1"
}
sw() {   # sw <file> <step> <knob>: the value read back after the step
    val "$1" "$2" readback | tr ' ' '\n' | sed -n "s/^debug.rdna3accel_$3=//p"
}

# ----------------------------------------------------------------- timer mode
for SH in sh bash; do
    command -v "$SH" >/dev/null 2>&1 || continue
    export SH
    # one second per step under sh (the timetable is checked), none under bash (the steps only)
    if [ "$SH" = sh ]; then HOLDV=1; else HOLDV=0; fi
    mkoids "$T/sys"
    run 1 timer
    B=$T/timer/bisect.txt
    [ -s "$B" ] || fail "$SH: no bisect.txt: $(cat "$T/timer.out")"
    grep -q '^rc=0$' "$T/timer.out" || fail "$SH: bisect_run did not return 0: $(tail -5 "$T/timer.out")"
    [ "$(grep -c '^== step ' "$B")" = 13 ] || fail "$SH: not 13 steps: $(grep '^== step ' "$B" | tr '\n' ' ')"
    [ "$(grep -c '^== end step ' "$B")" = 13 ] || fail "$SH: a step has no end line"
    grep -q '^== end$' "$B" || fail "$SH: no end line"
    grep -q '^mode: timer$' "$B" || fail "$SH: mode is not timer"
    ! grep -q '^failed:' "$B" || fail "$SH: a switch failed: $(grep '^failed:' "$B")"
    # the plan: 13 timetable lines, 1 s per step here
    [ "$(grep -c '^plan: ' "$B")" = 13 ] || fail "$SH: no 13-line timetable"
    grep -q "^plan: 12|00:$(printf '%02d' $((12 * HOLDV)))|final|" "$B" || fail "$SH: the timetable's last line: $(grep '^plan: 12' "$B")"
    grep -q 'BISECT timetable' "$T/timer.out" || fail "$SH: the timetable is not printed"
    # step 0 sets nothing; step 1 everything off; 2.. back on one at a time, oldest first
    [ "$(val "$B" 0 set)" = "(nothing)" ] || fail "$SH: step 0 set something: $(val "$B" 0 set)"
    want() {   # want <step> a3 flicker latch torn mode scan stale landed a3ws
        local s=$1 got exp; shift
        got=$(for k in a3 flicker swap_latch swap_torn swap_mode trace_scan swap_stale swap_landed a3ws; do sw "$B" "$s" $k; done | tr '\n' ' ')
        exp="$* "
        [ "$got" = "$exp" ] || fail "$SH: step $s reads back [$got], expected [$exp]"
    }
    want 0  1 1 1 1 1 1 1 0 0
    want 1  0 0 0 0 0 0 0 0 0
    want 2  1 0 0 0 0 0 0 0 0
    want 3  1 1 0 0 0 0 0 0 0
    want 4  1 1 1 0 0 0 0 0 0
    want 5  1 1 1 1 0 0 0 0 0
    want 6  1 1 1 1 1 0 0 0 0
    want 7  1 1 1 1 1 1 0 0 0
    want 8  1 1 1 1 1 1 1 0 0
    want 9  1 1 1 1 1 1 1 1 0
    want 10 1 1 1 1 2 1 1 0 0
    want 11 1 1 1 1 1 1 1 0 1
    want 12 1 1 1 1 1 1 1 0 0
    [ "$(val "$B" 2 changed)" = "a3 0->1" ] || fail "$SH: step 2 'changed' is [$(val "$B" 2 changed)]"
    [ "$(val "$B" 8 changed)" = "swap_stale 0->1" ] || fail "$SH: step 8 'changed' is [$(val "$B" 8 changed)]"
    [ "$(val "$B" 9 changed)" = "swap_landed 0->1" ] || fail "$SH: step 9 'changed' is [$(val "$B" 9 changed)]"
    [ "$(val "$B" 10 changed)" = "swap_mode 1->2, swap_landed 1->0" ] || fail "$SH: step 10 'changed' is [$(val "$B" 10 changed)]"
    [ "$(val "$B" 12 changed)" = "a3ws 1->0" ] || fail "$SH: step 12 'changed' is [$(val "$B" 12 changed)]"
    [ "$(val "$B" 1 set | wc -w)" = 9 ] || fail "$SH: step 1 did not set the 9 switches: $(val "$B" 1 set)"
    # the defaults come back at the end
    for k in a3 flicker swap_latch swap_torn swap_mode trace_scan swap_stale swap_landed a3ws; do
        v=$(cat "$T/sys/debug.rdna3accel_$k"); e=1; case $k in swap_landed|a3ws) e=0 ;; esac
        [ "$v" = "$e" ] || fail "$SH: $k is $v after the run, expected $e"
    done
    grep -q '^restored: 1 1 1 1 1 1 1 0 0$' "$B" || fail "$SH: restored line: $(grep '^restored:' "$B")"
    # the owner knows where he is: a notification, "step N" spoken and N beeps per step
    [ "$(grep -c '^say step ' "$T/stub.log")" = 13 ] || fail "$SH: say called $(grep -c '^say step ' "$T/stub.log") times"
    for n in 0 1 5 12; do grep -q "^say step $n\$" "$T/stub.log" || fail "$SH: no 'say step $n'"; done
    [ "$(grep -c '^afplay ' "$T/stub.log")" = 78 ] || fail "$SH: afplay $(grep -c '^afplay ' "$T/stub.log") times, expected 0+1+...+12 = 78"
    [ "$(grep -c 'step [0-9]* of 12' "$T/notify.log")" = 13 ] || fail "$SH: notifications: $(wc -l < "$T/notify.log")"
    grep -q 'bisect starts at' "$T/notify.log" || fail "$SH: no start notification"
    # evidence: two looks, the counters, the trace per step
    for n in 0 7 12; do
        [ -f "$T/home/rdna3-scanout/bisect$n/scanout-pipe0.ppm" ] && [ -f "$T/home/rdna3-scanout/bisect${n}a/scanout-pipe0.ppm" ] ||
            fail "$SH: no look pictures of step $n"
    done
    [ "$(grep -c '^-- look bisect' "$B")" = 26 ] || fail "$SH: looks: $(grep -c '^-- look bisect' "$B")"
    [ "$(grep -c '^-- counters' "$B")" = 13 ] || fail "$SH: counters blocks"
    [ "$(grep -c '^stream_bytes: 6$' "$B")" = 13 ] || fail "$SH: stream_bytes lines: $(grep '^stream_bytes' "$B" | sort | uniq -c)"
    grep -q '^debug.rdna3accel_trace_scan: ' "$B" || fail "$SH: trace_scan not in the counters"
    [ "$(grep -c '^answer: -$' "$B")" = 13 ] || fail "$SH: timer mode should have no answers"
    grep -q 'snapshot bisect-end' "$T/prog.log" || fail "$SH: no trace snapshot at the end"
    # nothing of the swap A/B label (it would make trace_scan=1 scan every swap)
    ! grep -q 'swap_ab' "$T/stub.log" "$B" || fail "$SH: the swap A/B label was touched"
    # Activity Monitor re-opened where a3 / a3ws changed
    [ "$(grep -c '^pkill -x Activity Monitor' "$T/stub.log")" = 5 ] || fail "$SH: Activity Monitor re-opens: $(grep -c '^pkill' "$T/stub.log"), expected 5 (steps 0 1 2 11 12)"
    echo "test_bisect_step: ok timer mode under $SH"
done
unset SH HOLDV

# the same file through the triage's table
python3 -I "$ROOT/tools/triage_bisect.py" "$T/timer/bisect.txt" "$T/nothing" > "$T/table.out" || true
[ "$(grep -cE '^ *[0-9]+ (ref|off|on|extra|final) ' "$T/table.out")" = 13 ] || fail "triage_bisect table: $(cat "$T/table.out")"

# ------------------------------------------------------------------- tty mode
mkoids "$T/sys"
printf 'y\nn\n\n-\ny\ny\ny\ny\nn\n-\n-\n-\n-\n' > "$T/keys"
run 1 tty BISECT_KEYFILE="$T/keys"
B=$T/tty/bisect.txt
grep -q '^mode: tty$' "$B" || fail "tty: mode is not tty"
[ "$(val "$B" 0 answer)" = y ] || fail "tty: step 0 answer [$(val "$B" 0 answer)]"
[ "$(val "$B" 1 answer)" = n ] || fail "tty: step 1 answer [$(val "$B" 1 answer)]"
[ "$(val "$B" 2 answer)" = enter ] || fail "tty: step 2 answer [$(val "$B" 2 answer)]"
[ "$(val "$B" 3 answer)" = none ] || fail "tty: step 3 answer [$(val "$B" 3 answer)]"
[ "$(val "$B" 8 answer)" = n ] || fail "tty: step 8 answer [$(val "$B" 8 answer)]"
echo "test_bisect_step: ok tty answers"

# ----------------------------------------------- a switch that does not take
mkoids "$T/sys"; touch "$T/sys/refuse.debug.rdna3accel_swap_latch"
run 1 refuse
B=$T/refuse/bisect.txt
grep -q '^failed: .*debug.rdna3accel_swap_latch:set-failed' "$B" || fail "refusal not reported: $(grep '^failed' "$B" | head -3)"
[ "$(grep -c '^== step ' "$B")" = 13 ] || fail "a refused switch stopped the run"
grep -q '^rc=1$' "$T/refuse.out" || fail "a refused switch should make bisect_run return 1"
grep -q 'did NOT apply' "$T/refuse.out" || fail "a refused switch is not said on the screen"
[ "$(cat "$T/sys/debug.rdna3accel_a3")" = 1 ] || fail "switches not put back after a refusal"

# ------------------------------------ an older kernel: no swap_landed, no a3ws
mkoids "$T/sys" swap_landed a3ws
run 1 absent
B=$T/absent/bisect.txt
grep -q '^absent: swap_landed a3ws$' "$B" || fail "absent line: $(grep '^absent' "$B")"
! grep -q '^failed:' "$B" || fail "an absent switch is not a failure: $(grep '^failed' "$B")"
grep -q '^rc=0$' "$T/absent.out" || fail "absent switches: rc"
[ -z "$(ls "$T/sys" | grep 'swap_landed')" ] || fail "the stub grew a sysctl"
# no swap_mode at all: nothing runs
mkoids "$T/sys" swap_mode
run 1 nomode
grep -q '^ABORT: no debug.rdna3accel_swap_mode' "$T/nomode/bisect.txt" || fail "no ABORT without swap_mode"
grep -q '^rc=1$' "$T/nomode.out" || fail "no swap_mode: rc"
python3 -I "$ROOT/tools/triage_bisect.py" "$T/nomode/bisect.txt" > "$T/nomode.table" 2>&1 || true
grep -q 'FAIL bisect.txt has no step block' "$T/nomode.table" || fail "triage_bisect on an aborted run: $(cat "$T/nomode.table")"

# ------------------------------------------------------------ plugin variant
mkoids "$T/sys"
cat > "$T/conf.orig" <<'EOF'
# the plugin's knobs
NVMTL_TRACE_RING=262144
EOF
cat > "$T/allow.orig" <<'EOF'
mtlprobe
@WindowServer
-Safari
%apps
EOF
rm -f "$T/baks".*
pl() {   # pl <name> [env...]
    local name=$1; shift
    run 2 "$name" BISECT_CONF="$T/conf" BISECT_ALLOW="$T/allow" BISECT_CONFBAK="$T/conf.bak" BISECT_ALLOWBAK="$T/allow.bak" \
        BISECT_NOLOGINWAIT=1 "$@"
}
cp "$T/conf.orig" "$T/conf"; cp "$T/allow.orig" "$T/allow"
pl plugin
B=$T/plugin/bisect.txt
grep -q '^rc=0$' "$T/plugin.out" || fail "plugin variant rc: $(tail -4 "$T/plugin.out")"
[ "$(grep -c '^== step ' "$B")" = 5 ] || fail "plugin: not 5 steps"
cmp -s "$T/conf" "$T/conf.orig" || fail "rdna3mtl.conf is not as it was: $(cat "$T/conf")"
cmp -s "$T/allow" "$T/allow.orig" || fail "the allow list is not as it was: $(cat "$T/allow")"
[ ! -e "$T/conf.bak" ] && [ ! -e "$T/allow.bak" ] || fail "the backups were left behind"
rb() { val "$B" "$1" readback; }
grep -q 'NVMTL_NO_SURFACE_SHARE=1 NVMTL_NO_SURFACE_PAGEOFF=1 NVMTL_TRACE=0 .*%apps lines 0' <<<"$(rb 1)" || fail "plugin step 1 readback: $(rb 1)"
grep -q 'conf NVMTL_TRACE=0 allow-list %apps lines 0' <<<"$(rb 2)" || fail "plugin step 2 readback: $(rb 2)"
grep -q 'conf NVMTL_TRACE=0 allow-list %apps lines 1' <<<"$(rb 3)" || fail "plugin step 3 readback: $(rb 3)"
grep -q '(no NVMTL_NO_SURFACE_\* or NVMTL_TRACE line) allow-list %apps lines 1' <<<"$(rb 4)" || fail "plugin step 4 readback: $(rb 4)"
grep -q '(no NVMTL_NO_SURFACE_\* or NVMTL_TRACE line) allow-list %apps lines 1' <<<"$(rb 0)" || fail "plugin step 0 readback: $(rb 0)"
# the other lines of the conf stay while the three are edited
grep -q 'NVMTL_TRACE_RING=262144' "$T/conf" || fail "the conf lost its NVMTL_TRACE_RING line"
[ "$(grep -c '^killall -HUP WindowServer' "$T/stub.log")" = 3 ] || fail "WindowServer restarts: $(grep -c killall "$T/stub.log"), expected 3 (steps 1, 2, 4)"
[ "$(grep -c '^ws_restart:' "$B")" = 3 ] || fail "ws_restart lines"
grep -q '^autologin: ' "$B" || fail "no autologin line"
# no conf file at all before: none after
rm -f "$T/conf"; cp "$T/allow.orig" "$T/allow"
pl plugin2
[ ! -e "$T/conf" ] || fail "a conf the run made was left: $(cat "$T/conf")"
cmp -s "$T/allow" "$T/allow.orig" || fail "allow list (no conf case)"
# a dead run's files: the restore puts the originals back
cp "$T/conf.orig" "$T/conf.bak"; echo 'NVMTL_NO_SURFACE_SHARE=1' > "$T/conf"
cp "$T/allow.orig" "$T/allow.bak"; grep -v '%apps' "$T/allow.orig" > "$T/allow"
( set +e; . "$T/block.sh"; BISECT_CONF="$T/conf" BISECT_ALLOW="$T/allow" BISECT_CONFBAK="$T/conf.bak" BISECT_ALLOWBAK="$T/allow.bak"; bisect_plugin_restore )
cmp -s "$T/conf" "$T/conf.orig" && cmp -s "$T/allow" "$T/allow.orig" || fail "bisect_plugin_restore after a dead run"
echo "test_bisect_step: ok plugin variant"

# the top of autoreport.sh puts a dead bisect-plugin run's conf back, and the flags / boot-args reach the group
grep -q 'rdna3-bisect-conf.bak' "$ROOT/tools/vm/macos/autoreport.sh" || fail "autoreport.sh does not restore a dead run's conf"
grep -q -- '--bisect-plugin) BISECT=2' "$ROOT/tools/vm/macos/autoreport.sh" || fail "autoreport.sh: --bisect-plugin"
grep -q 'rdna3bisect=2 "\*) \[ -n "\$BISECT" \] || BISECT=2' "$ROOT/tools/vm/macos/autoreport.sh" || fail "autoreport.sh: boot-arg rdna3bisect=2"
grep -q '^bisect rdna3bisect=1$' "$ROOT/tools/ladder-steps.txt" && grep -q '^bisect-plugin rdna3bisect=2$' "$ROOT/tools/ladder-steps.txt" ||
    fail "tools/ladder-steps.txt: bisect / bisect-plugin"
grep -q '39-bisect.txt' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not pack the bisect"
echo "test_bisect_step: ok"
