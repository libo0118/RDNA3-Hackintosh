#!/bin/bash
# The A/B ladder (DRIVER-PLAN N50): the LADDER block of tools/vm/macos/autoreport.sh on the host (with the BISECT block's plugin-conf helpers),
# stubs for open, pgrep, kill, log, killall, curl and rdna3_drminfo:
#  - the rung list is the one tools/triage_glyphs.py reads
#  - the app rungs: Terminal and Chess opened as NEW instances (open -n -a APP --env ...) with exactly the rung's NVMTL_ switches, nothing for the
#    baseline; two looks (320 and 1280 wide) and a log file per rung; only the instances the rung started are quit (the owner's Terminal stays);
#    rdna3mtl.conf is not touched and WindowServer is not restarted
#  - the WindowServer rungs (variant 2): the conf holds the rung's lines (the others kept, a same-named line replaced) when WindowServer is
#    restarted, and is EXACTLY the original afterwards - also with no conf file at all, after an interrupt (SIGTERM in a rung), after a dead run
#  - nobody logged in: aborts with a line, touches nothing
#  - an app that does not start is reported; tty mode: the owner's y / n per rung reaches ladder.txt
#  - tools/triage_glyphs.py reads what the block wrote (parse_ladder + ladder_rows)
#  - the wiring: flags, boot-args, step lists, auto limit, collect.sh, triage_reports.py; the default `play` step is unchanged
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "${T:?}"' EXIT
fail() { echo "test_ladder_step: FAIL $*"; exit 1; }
AR=$ROOT/tools/vm/macos/autoreport.sh
export ROOT

for f in tools/vm/macos/autoreport.sh tools/macos/collect.sh tools/vm/rdna3vm; do bash -n "$ROOT/$f" || fail "bash -n $f"; done
if command -v dash >/dev/null 2>&1; then dash -n "$AR" || fail "dash -n autoreport.sh"; fi

sed -n '/^# BISECT-BEGIN/,/^# BISECT-END/p' "$AR" > "$T/bisect.sh"
sed -n '/^# LADDER-BEGIN/,/^# LADDER-END/p' "$AR" > "$T/ladder.sh"
[ "$(wc -l < "$T/bisect.sh")" -gt 150 ] && [ "$(wc -l < "$T/ladder.sh")" -gt 60 ] || fail "the BISECT / LADDER blocks were not found in autoreport.sh"

# ---- the rung list = the triage's
RUNGS=$(sed -n 's/^LADDER_RUNGS="\(.*\)"$/\1/p' "$T/ladder.sh")
WSRUNGS=$(sed -n 's/^LADDER_WS_RUNGS="\(.*\)"$/\1/p' "$T/ladder.sh")
PY=$(python3 -I -c "
import sys; sys.path.insert(0, '$ROOT/tools')
import triage_glyphs as t
print(' '.join(t.LADDER_RUNGS)); print(' '.join(t.LADDER_WS_RUNGS))")
[ "$RUNGS" = "$(echo "$PY" | sed -n 1p)" ] && [ "$WSRUNGS" = "$(echo "$PY" | sed -n 2p)" ] || fail "rungs: autoreport.sh [$RUNGS] [$WSRUNGS] vs triage_glyphs.py [$PY]"

# ---- the stubs
mkdir -p "$T/bin" "$T/sys" "$T/D/bin" "$T/home"
cat > "$T/bin/open" <<'EOF'
#!/bin/sh
# open -n -a APP [--env K=V]... [file]: a new instance = a new pid in $STUBDIR/pids-APP, its env in $STUBDIR/env-PID
echo "open $*" >> "$STUBLOG"
[ "$1" = -n ] && [ "$2" = -a ] || { echo "open: not -n -a" >> "$STUBLOG"; exit 1; }
app=$3; shift 3
[ -n "${FAIL_APP:-}" ] && [ "$FAIL_APP" = "$app" ] && exit 1
n=$(cat "$STUBDIR/next" 2>/dev/null || echo 1000); echo $((n + 1)) > "$STUBDIR/next"
echo $n >> "$STUBDIR/pids-$app"
: > "$STUBDIR/env-$n"
while [ $# -gt 0 ]; do
    if [ "$1" = --env ]; then echo "$2" >> "$STUBDIR/env-$n"; shift 2; else shift; fi
done
exit 0
EOF
cat > "$T/bin/pgrep" <<'EOF'
#!/bin/sh
[ "$1" = -x ] && cat "$STUBDIR/pids-$2" 2>/dev/null
exit 0
EOF
cat > "$T/bin/log" <<'EOF'
#!/bin/sh
# log show --start T --info --style compact --predicate '... processID == N OR ...': the plugin's switch line of each pid
echo "log $*" >> "$STUBLOG"
pred=
while [ $# -gt 0 ]; do [ "$1" = --predicate ] && pred=$2; shift; done
for p in $(echo "$pred" | sed -n 's/processID == \([0-9]*\)/\1/gp' | tr -cs '0-9' ' '); do
    ev=$(tr '\n' ' ' < "$STUBDIR/env-$p" 2>/dev/null)
    if [ -n "$ev" ]; then
        sw=
        for kv in $ev; do sw="$sw ${kv%%=*}=\"${kv#*=}\""; done
        echo "2026-10-25 12:00:01.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: ab switches [1]:$sw"
    else
        echo "2026-10-25 12:00:01.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: ab switches: none set (x)"
    fi
done
exit 0
EOF
cat > "$T/bin/killall" <<'EOF'
#!/bin/sh
echo "killall $*" >> "$STUBLOG"
n=$(cat "$STUBDIR/ws" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$STUBDIR/ws"
cp "$BISECT_CONF" "$STUBDIR/conf-at-ws-$n" 2>/dev/null || echo "(absent)" > "$STUBDIR/conf-at-ws-$n"
exit 0
EOF
for c in caffeinate say afplay osascript sleep pkill; do printf '#!/bin/sh\necho "%s $*" >> "$STUBLOG"\nexit 0\n' "$c" > "$T/bin/$c"; done
printf '#!/bin/sh\nexit 0\n' > "$T/bin/curl"
cat > "$T/D/bin/rdna3_drminfo" <<'EOF'
#!/bin/sh
echo "drminfo $*" >> "$STUBLOG"
case "$1" in --scanout|--scanout-hires) ;; *) exit 2 ;; esac
dir=$2; mkdir -p "$dir"
echo "pipe 0: LIT otg 0 1920x1080 pitch 1920 fmt 0 sw 0 dcc 0 address 0x1000 in use 0x1000"
echo "  picture 320x180: 45% not black, mean rgb 90 90 90, 5000 bytes changed in 2 s"
echo "1 pipe(s) lit"
printf 'P6\n2 2\n255\n\200\200\200\200\200\200\200\200\200\200\200\200' > "$dir/scanout-pipe0.ppm"
exit 0
EOF
chmod +x "$T/bin/"* "$T/D/bin/rdna3_drminfo"

cat > "$T/h.sh" <<'EOF'
. "$HT/bisect.sh"
. "$HT/ladder.sh"
prog() { echo "$*" >> "$HT/prog.log"; }
notify() { echo "$*" >> "$HT/notify.log"; }
hdr() { echo "== $*"; }
with_limit() { shift; "$@"; }
restore_sleep() { :; }
as_user() { "$@"; }
kill() {   # the real kill is not wanted: the fake instances are forgotten, the line is kept
    echo "kill $*" >> "$STUBLOG"
    for a in "$@"; do
        case $a in -*) ;; *) for f in "$STUBDIR"/pids-*; do [ -f "$f" ] && { grep -vx "$a" "$f" > "$f.n" || true; mv -f "$f.n" "$f"; }; done ;; esac
    done
    return 0
}
if [ -n "${STOP_AT:-}" ]; then   # an interrupt in the middle of a rung: the harness's own SIGTERM at the Nth hires look
    ladder_look_hires() { n=$(cat "$STUBDIR/looks" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$STUBDIR/looks"; [ "$n" = "$STOP_AT" ] && command kill -TERM $$; return 0; }
fi
STANDALONE=1; HOST=; base=rdna3-report-test; D=$HT/D; TS=$HT/ts; mkdir -p "$TS"
HOME=$HT/home; export HOME
ladder_run "$VARIANT"
echo "rc=$?"
EOF

run() {   # run <variant> <name> [env...]: the block under $SH with the stubs; output in $T/<name>.out, evidence in $T/<name>/ladder.txt
    local v=$1 name=$2; shift 2
    rm -rf "${T:?}/${name:?}" "${T:?}/home" "${T:?}/sys"; mkdir -p "$T/$name" "$T/home" "$T/sys"
    : > "$T/stub.log"; rm -f "$T/notify.log" "$T/prog.log"
    echo 500 > "$T/sys/pids-Terminal"   # the owner's own Terminal, which started this
    echo 90 > "$T/sys/pids-WindowServer"
    env HT="$T" STUBLOG="$T/stub.log" STUBDIR="$T/sys" PATH="$T/bin:$PATH" VARIANT="$v" LADDER_OUT="$T/$name" LADDER_TEXT="$T/text.command" \
        LADDER_LAUNCHWAIT=0 LADDER_SETTLE=0 LADDER_TAIL=0 LADDER_QUITWAIT=0 BISECT_USER=tester BISECT_NOLOGINWAIT=1 \
        BISECT_CONF="$T/conf" BISECT_ALLOW="$T/allow" BISECT_CONFBAK="$T/conf.bak" BISECT_ALLOWBAK="$T/allow.bak" \
        "$@" ${SH:-sh} "$T/h.sh" > "$T/$name.out" 2>&1 < /dev/null || echo "rc=$?" >> "$T/$name.out"
}
cat > "$T/conf.orig" <<'EOF'
# the plugin's knobs
NVMTL_TRACE_RING=262144
NVMTL_RADV_DEBUG=hang
EOF
printf 'mtlprobe\n@WindowServer\n%%apps\n' > "$T/allow.orig"
fresh() { cp "$T/conf.orig" "$T/conf"; cp "$T/allow.orig" "$T/allow"; rm -f "$T/conf.bak" "$T/conf.bak.absent" "$T/allow.bak"; }
envs_of() {   # the two open calls (Terminal, Chess) of rung N
    grep -E "^open -n -a (Terminal|Chess)( |$)" "$T/stub.log" | sed -n "$(($1 * 2 - 1)),$(($1 * 2))p"
}

for SH in sh bash dash; do
    command -v "$SH" >/dev/null 2>&1 || continue
    export SH
    # ------------------------------------------------------------ variant 1: the app rungs
    fresh
    run 1 apps
    L=$T/apps/ladder.txt
    grep -q '^rc=0$' "$T/apps.out" || fail "$SH: variant 1 rc: $(tail -5 "$T/apps.out")"
    [ "$(grep -c '^== rung ' "$L")" = 7 ] && [ "$(grep -c '^== end rung ' "$L")" = 7 ] || fail "$SH: not 7 rungs: $(grep '^== ' "$L")"
    [ "$(grep '^== rung ' "$L" | sed 's/^== rung //' | tr '\n' ' ' | sed 's/ $//')" = "$RUNGS" ] || fail "$SH: the rungs ran in a different order: $(grep '^== rung ' "$L")"
    [ "$(grep -c '^open -n -a ' "$T/stub.log")" = 14 ] || fail "$SH: expected 14 open calls: $(grep -c '^open' "$T/stub.log")"
    n=0
    for r in $RUNGS; do
        n=$((n + 1))
        case $r in
            baseline) want="" ;;
            radv) want="--env NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears" ;;
            sync) want="--env NVMTL_ASYNC_COMMIT=0" ;;
            managed) want="--env NVMTL_MANAGED_CHECK=1 --env NVMTL_MANAGED_SNAPSHOT=1" ;;
            alignguard) want="--env NVMTL_NO_ALIGN_GUARD=1" ;;
            paranoid) want="--env NVMTL_PARANOID=1" ;;
            loadbarrier) want="--env NVMTL_LOAD_BARRIER=1" ;;
        esac
        got=$(envs_of $n | sed -E 's/^open -n -a (Terminal|Chess) ?//; s#(^| )[^ ]*text\.command$##; s/ +$//' | sort -u)
        [ "$(echo "$got" | wc -l | tr -d ' ')" = 1 ] || fail "$SH: rung $r: Terminal and Chess got different switches: $got"
        [ "$got" = "$want" ] || fail "$SH: rung $r opened its apps with [$got], expected [$want]"
        [ -f "$T/apps/$r.log" ] || fail "$SH: rung $r has no log slice"
        grep -q "^== rung $r\$" "$L" && grep -q "^looks: ladder-${r}a ladder-${r}h\$" "$L" || fail "$SH: rung $r: looks line"
        [ -f "$T/home/rdna3-scanout/ladder-${r}a/scanout-pipe0.ppm" ] && [ -f "$T/home/rdna3-scanout/ladder-${r}h/scanout-pipe0.ppm" ] || fail "$SH: rung $r: both looks' pictures"
    done
    grep -q "^open -n -a Terminal .*$T/text.command\$" "$T/stub.log" || fail "$SH: the Terminal is not opened with the text script"
    [ -x "$T/text.command" ] && grep -q 'ls -laG' "$T/text.command" || fail "$SH: the text script"
    [ "$(grep -c '^drminfo --scanout-hires ' "$T/stub.log")" = 7 ] && [ "$(grep -c '^drminfo --scanout ' "$T/stub.log")" = 7 ] || fail "$SH: looks: $(grep -c drminfo "$T/stub.log")"
    grep -q '^log show --start ' "$T/stub.log" && grep -q 'processID == 1000' "$T/stub.log" || fail "$SH: the log slice is not by pid"
    # only the pids this run started are quit; the owner's Terminal (500) is not
    ! grep -q '^kill .*500' "$T/stub.log" || fail "$SH: the owner's Terminal was killed"
    [ "$(cat "$T/sys/pids-Terminal")" = 500 ] || fail "$SH: instances left running: $(cat "$T/sys/pids-Terminal" "$T/sys/pids-Chess" 2>/dev/null | tr '\n' ' ')"
    [ -z "$(cat "$T/sys/pids-Chess" 2>/dev/null)" ] || fail "$SH: Chess instances left running"
    # the app rungs touch neither the conf nor WindowServer
    cmp -s "$T/conf" "$T/conf.orig" && [ ! -e "$T/conf.bak" ] || fail "$SH: variant 1 changed rdna3mtl.conf"
    ! grep -q '^killall' "$T/stub.log" || fail "$SH: variant 1 restarted WindowServer"
    grep -q '^conf_before: ' "$L" && grep -q '^conf_after: ' "$L" && grep -q '^== end$' "$L" || fail "$SH: conf_before / conf_after / end lines"
    # the triage reads it
    python3 -I - "$L" "$T/apps" <<'PYEOF' || fail "$SH: tools/triage_glyphs.py does not read the ladder"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t
text = open(sys.argv[1]).read()
logs = {r: open(os.path.join(sys.argv[2], r + ".log")).read() for r in t.LADDER_RUNGS}
v, rungs, probs = t.parse_ladder(text, logs)
assert v == 1 and not probs and list(rungs) == list(t.LADDER_RUNGS), (v, probs, list(rungs))
rows = t.ladder_rows(rungs, v, probs)
bad = [r for r in rows if r[0] != "INFO"]
assert not bad, bad
assert any(r[1] == "ladder radv" and "NVMTL_RADV_DEBUG" in r[2] for r in rows), rows
PYEOF
    echo "test_ladder_step: ok app rungs ($SH)"

    # ------------------------------------------------------------ variant 2: the WindowServer rungs
    fresh
    run 2 ws
    L=$T/ws/ladder.txt
    grep -q '^rc=0$' "$T/ws.out" || fail "$SH: variant 2 rc: $(tail -5 "$T/ws.out")"
    [ "$(grep -c '^== rung ' "$L")" = 9 ] && [ "$(grep '^== rung ' "$L" | tail -2 | sed 's/^== rung //' | tr '\n' ' ')" = "radv-ws paranoid-ws " ] || fail "$SH: variant 2 rungs: $(grep '^== rung ' "$L" | tr '\n' ' ')"
    cmp -s "$T/conf" "$T/conf.orig" || fail "$SH: rdna3mtl.conf is not as it was: $(cat "$T/conf")"
    cmp -s "$T/allow" "$T/allow.orig" || fail "$SH: the allow list is not as it was"
    [ ! -e "$T/conf.bak" ] && [ ! -e "$T/conf.bak.absent" ] && [ ! -e "$T/allow.bak" ] || fail "$SH: backups left behind"
    [ "$(grep -c '^killall -HUP WindowServer' "$T/stub.log")" = 3 ] || fail "$SH: WindowServer restarts: $(grep -c killall "$T/stub.log"), expected 3 (two rungs and the last one back)"
    c1=$T/sys/conf-at-ws-1; c2=$T/sys/conf-at-ws-2; c3=$T/sys/conf-at-ws-3
    grep -q '^NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears$' "$c1" && ! grep -q '^NVMTL_RADV_DEBUG=hang$' "$c1" || fail "$SH: radv-ws: the conf at the restart: $(cat "$c1")"
    grep -q '^NVMTL_TRACE_RING=262144$' "$c1" || fail "$SH: radv-ws: the conf lost another line: $(cat "$c1")"
    grep -q '^NVMTL_PARANOID=1$' "$c2" && grep -q '^NVMTL_RADV_DEBUG=hang$' "$c2" && ! grep -q 'nodcc' "$c2" || fail "$SH: paranoid-ws: the conf at the restart: $(cat "$c2")"
    cmp -s "$c3" "$T/conf.orig" || fail "$SH: the last WindowServer restart did not run with the original conf: $(cat "$c3")"
    # the WindowServer rungs' log slice includes WindowServer's own pid
    grep -q 'processID == 90' "$T/stub.log" || fail "$SH: the WindowServer rungs' log slice does not name WindowServer"
    # no conf file at all before: none after
    rm -f "$T/conf" "$T/conf.bak" "$T/conf.bak.absent"; cp "$T/allow.orig" "$T/allow"
    run 2 ws2
    grep -q '^rc=0$' "$T/ws2.out" || fail "$SH: no-conf run rc"
    [ ! -e "$T/conf" ] && [ ! -e "$T/conf.bak.absent" ] || fail "$SH: a conf the run made was left: $(cat "$T/conf" 2>/dev/null)"
    # an interrupt in the middle of a rung: SIGTERM at the 8th hires look = the first WindowServer rung (variant 2), so that the conf is at stake
    fresh
    run 2 intr STOP_AT=8
    grep -q '^rc=143$' "$T/intr.out" || fail "$SH: an interrupted run did not exit 143: $(tail -3 "$T/intr.out")"
    cmp -s "$T/conf" "$T/conf.orig" || fail "$SH: after an interrupt rdna3mtl.conf is not as it was: $(cat "$T/conf")"
    cmp -s "$T/allow" "$T/allow.orig" && [ ! -e "$T/conf.bak" ] || fail "$SH: after an interrupt: allow list / backups"
    grep -q '^killall -HUP WindowServer' "$T/stub.log" || fail "$SH: the interrupt test never reached a WindowServer rung"
    grep -q '^restored: ' "$T/intr/ladder.txt" || fail "$SH: the interrupted run left no 'restored:' line"
    [ -z "$(cat "$T/sys/pids-Chess" 2>/dev/null)" ] && [ "$(cat "$T/sys/pids-Terminal")" = 500 ] || fail "$SH: the interrupted rung's apps were left running"
    # an interrupt in an app rung (variant 1) leaves the conf alone and quits the apps
    fresh
    run 1 intr1 STOP_AT=2
    grep -q '^rc=143$' "$T/intr1.out" && cmp -s "$T/conf" "$T/conf.orig" && [ -z "$(cat "$T/sys/pids-Chess" 2>/dev/null)" ] || fail "$SH: an interrupt in an app rung: $(tail -3 "$T/intr1.out")"
    # a dead run's files: the restore puts the originals back (the top of autoreport.sh does it at the next start)
    cp "$T/conf.orig" "$T/conf.bak"; echo 'NVMTL_PARANOID=1' > "$T/conf"
    ( set +e; . "$T/bisect.sh"; BISECT_CONF="$T/conf" BISECT_ALLOW="$T/allow" BISECT_CONFBAK="$T/conf.bak" BISECT_ALLOWBAK="$T/allow.bak"; bisect_plugin_restore )
    cmp -s "$T/conf" "$T/conf.orig" || fail "$SH: bisect_plugin_restore after a dead ladder run"
    echo "test_ladder_step: ok WindowServer rungs, restore ($SH)"

    # ------------------------------------------------------------ nobody logged in; an app that does not start; tty answers
    fresh
    run 1 nobody BISECT_USER=root
    grep -q '^rc=1$' "$T/nobody.out" && grep -q '^ABORT: nobody logged in' "$T/nobody/ladder.txt" || fail "$SH: nobody logged in: $(tail -3 "$T/nobody.out")"
    ! grep -q '^open' "$T/stub.log" && cmp -s "$T/conf" "$T/conf.orig" || fail "$SH: nobody logged in, yet something ran"
    fresh
    run 1 noapp FAIL_APP=Chess
    grep -q '^WARN: an app did not start' "$T/noapp/ladder.txt" && [ "$(grep -c '^== end rung ' "$T/noapp/ladder.txt")" = 7 ] || fail "$SH: an app that did not start is not reported: $(grep -c WARN "$T/noapp/ladder.txt")"
    printf 'n\ny\ny\n-\n\nn\ny\n' > "$T/keys"
    fresh
    run 1 tty BISECT_KEYFILE="$T/keys"
    grep -q '^mode: tty$' "$T/tty/ladder.txt" || fail "$SH: tty mode"
    [ "$(grep '^answer: ' "$T/tty/ladder.txt" | sed 's/^answer: //' | tr '\n' ' ')" = "n y y - - n y " ] || fail "$SH: the owner's answers: $(grep '^answer: ' "$T/tty/ladder.txt" | tr '\n' ' ')"
    python3 -I - "$T/tty/ladder.txt" <<'PYEOF' || fail "$SH: ladder verdicts"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t
v, rungs, probs = t.parse_ladder(open(sys.argv[1]).read(), {})
assert rungs["baseline"]["verdict"] == "n" and rungs["radv"]["verdict"] == "y" and rungs["managed"]["verdict"] is None, rungs
rows = t.ladder_rows(rungs, v, probs)
assert any(r[1] == "ladder" and "RIGHT with: radv, sync, loadbarrier" in r[2] for r in rows), rows
PYEOF
    echo "test_ladder_step: ok abort / no app / tty ($SH)"
done

# ---- the wiring
grep -q -- '--ladder) LADDER=1' "$AR" && grep -q -- '--ladder-ws) LADDER=2' "$AR" || fail "autoreport.sh: --ladder / --ladder-ws"
grep -q 'rdna3ladder=1 "\*) \[ -n "\$LADDER" \] || LADDER=1' "$AR" && grep -q 'rdna3ladder=2 "\*) \[ -n "\$LADDER" \] || LADDER=2' "$AR" || fail "autoreport.sh: boot-args rdna3ladder=1 / 2"
grep -q 'ladder_run "\$LADDER"' "$AR" || fail "autoreport.sh: ladder_run is not called"
grep -q '\[ -z "\$BISECT" \] && \[ -z "\$PLAY" \] && \[ -z "\$LADDER" \]' "$AR" || fail "autoreport.sh: the other tests still run in a ladder run"
grep -q 'rdna3-ladder/ladder.txt' "$AR" || fail "autoreport.sh: ladder.txt is not sent"
grep -q '41-ladder' "$ROOT/tools/macos/collect.sh" || fail "collect.sh: 41-ladder"
grep -q '^ladder .* rdna3ladder=1$' "$ROOT/tools/ladder-steps.txt" && grep -q '^ladder-ws .* rdna3ladder=2$' "$ROOT/tools/ladder-steps.txt" || fail "tools/ladder-steps.txt: ladder / ladder-ws"
play=$(awk '$1 == "play" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
lad=$(awk '$1 == "ladder" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ "$lad" = "${play% rdna3play=1} rdna3ladder=1" ] || fail "ladder-steps.txt: ladder is [$lad], expected the play step's boot-args + rdna3ladder=1"
[ "$(grep -c 'play-agdcscaler ladder ladder-ws' "$ROOT/tools/vm/rdna3vm")" = 2 ] || fail "rdna3vm: ladder is not in both step lists"
sed -n '/^auto_limit() {/,/^}/p' "$ROOT/tools/vm/rdna3vm" > "$T/limit.sh"
m1=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$lad")
m2=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "${lad%1}2")
mp=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$play")
[ "$m1" -ge 20 ] && [ "$m1" -lt 40 ] && [ "$m2" -ge 40 ] && [ "$m2" -lt 60 ] && [ "$mp" = 30 ] || fail "rdna3vm: auto_limit ladder $m1, ladder-ws $m2, play $mp (expected 20..39, 40..59, 30)"
grep -q '41-ladder.txt' "$ROOT/tools/triage_reports.py" && grep -q 'parse_ladder' "$ROOT/tools/triage_reports.py" || fail "triage_reports.py: the ladder is not read"
echo "test_ladder_step: ok"
