#!/bin/bash
# The A/B ladder (DRIVER-PLAN N50): the LADDER block of tools/vm/macos/autoreport.sh on the host (with the BISECT block's plugin-conf helpers),
# stubs for open, pgrep, kill, log, killall, curl and rdna3_drminfo:
#  - the rung list is the one tools/triage_glyphs.py reads
#  - the app rungs: Terminal and Chess opened as NEW instances (open -n -a APP --env ...) with exactly the rung's NVMTL_ switches, nothing for the
#    baseline; two looks (320 wide and hi-res) and a log file per rung; only the instances the rung started are quit (the owner's Terminal stays);
#    rdna3mtl.conf is not touched and WindowServer is not restarted
#  - the WindowServer rungs (variant 2): the conf holds the rung's lines (the others kept, a same-named line replaced) when WindowServer is
#    restarted, and is EXACTLY the original afterwards - also with no conf file at all, after an interrupt (SIGTERM in a rung), after a dead run
#  - variant 3 (ladder-glyphs): 13 rungs in the order the triage expects, each with exactly its switches (one knob per rung, each a knob the plugin reads
#    with the value it reads), two hi-res looks per rung (the second identifies the live window), the triage reads it
#  - the quit: SIGINT first, then SIGTERM, then SIGKILL, a line per instance saying how it ended, never System Events (no consent needed)
#  - the text script (one colour for the tick lines, the mixed-glyph line) and the persistence flag of the Terminal open
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
PRUNGS=$(sed -n 's/^LADDER_GLYPHS_RUNGS="\(.*\)"$/\1/p' "$T/ladder.sh")
PY=$(python3 -I -c "
import sys; sys.path.insert(0, '$ROOT/tools')
import triage_glyphs as t
print(' '.join(t.LADDER_RUNGS)); print(' '.join(t.LADDER_WS_RUNGS)); print(' '.join(t.LADDER_GLYPHS_RUNGS))")
[ "$RUNGS" = "$(echo "$PY" | sed -n 1p)" ] && [ "$WSRUNGS" = "$(echo "$PY" | sed -n 2p)" ] || fail "rungs: autoreport.sh [$RUNGS] [$WSRUNGS] vs triage_glyphs.py [$PY]"
[ "$PRUNGS" = "$(echo "$PY" | sed -n 3p)" ] && [ "$(echo $PRUNGS | wc -w | tr -d ' ')" = 13 ] || fail "glyph rungs: autoreport.sh [$PRUNGS] vs triage_glyphs.py [$(echo "$PY" | sed -n 3p)]"

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
[ "$1" = -f ] && [ -n "${OLDTICK:-}" ] && echo 4242
[ "$1" = -x ] && cat "$STUBDIR/pids-$2" 2>/dev/null
exit 0
EOF
cat > "$T/bin/log" <<'EOF'
#!/bin/sh
# log show --start T --info --style compact --predicate '... processID == N OR ...': the plugin's lines of each pid, as the plugin writes them
# (vk: ab switches, the RADV line, the NVMTL_PARANOID profile line with what it set and what it found set, a SUMMARY)
echo "log $*" >> "$STUBLOG"
pred=
while [ $# -gt 0 ]; do [ "$1" = --predicate ] && pred=$2; shift; done
[ -n "${BIGLOG:-}" ] && awk 'BEGIN { for (i = 0; i < 9000; i++) print "2026-10-25 12:00:01.000 Df  Chess[1000:1f] (RDNA3MTLDriver) NVMTL: filler line number " i " xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" }'
PKEYS="NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears NVMTL_ASYNC_COMMIT=0 NVMTL_GSUB_INFLIGHT=1 NVMTL_PRESUBMIT_PROLOGUE=0 NVMTL_NO_POOL_REUSE=1 NVMTL_NO_BUFREUSE=1 NVMTL_NO_BUFTEX_RECYCLE=1 NVMTL_NO_HEAPTEX_RECYCLE=1 NVMTL_NO_HEAPTEX_PLACE=1 NVMTL_NO_DIRTYONLY=1 NVMTL_ENC78_OFF=all NVMTL_G79_OFF=all NVMTL_NO_RES2=1 NVMTL_NO_PURGE_RECLAIM=1 NVMTL_NO_SURFIN_CACHE=1 NVMTL_SURFOUT_DMA=0"
for p in $(echo "$pred" | sed -n 's/processID == \([0-9]*\)/\1/gp' | tr -cs '0-9' ' '); do
    ev=$(tr '\n' ' ' < "$STUBDIR/env-$p" 2>/dev/null)
    sw=; set_by=; kept=; nset=0; radv=unset
    for kv in $ev; do
        sw="$sw ${kv%%=*}=\"${kv#*=}\""
        [ "${kv%%=*}" = NVMTL_RADV_DEBUG ] && [ -n "${kv#*=}" ] && radv="\"${kv#*=}\""
    done
    case " $ev " in *" NVMTL_PARANOID=1 "*)
        for pk in $PKEYS; do
            case " $ev " in
                *" ${pk%%=*}="*) kept="$kept${kept:+ }${pk%%=*}" ;;
                *) set_by="$set_by${set_by:+ }$pk"; nset=$((nset + 1)); sw="$sw ${pk%%=*}=\"${pk#*=}\""
                   [ "${pk%%=*}" = NVMTL_RADV_DEBUG ] && radv="\"${pk#*=}\"" ;;
            esac
        done
        echo "2026-10-25 12:00:00.500 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: conf: NVMTL_PARANOID=1 - the diagnostic profile is ON: $nset switch(es) set by it (${set_by:-none})${kept:+, already set and kept: $kept}; no async commit, no recycling" ;;
    esac
    if [ -n "$ev" ]; then
        echo "2026-10-25 12:00:01.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: RADV_DEBUG=$radv RADV_PERFTEST=unset (NVMTL_RADV_DEBUG=unset, NVMTL_RADV_PERFTEST=unset)"
        echo "2026-10-25 12:00:01.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: ab switches [1]:$sw"
        case " $ev " in *" NVMTL_ATTR_RING_BIG_PAGE=0 "*) echo "2026-10-25 12:00:01.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: RADV_ATTR_RING_BIG_PAGE=\"0\" (NVMTL_ATTR_RING_BIG_PAGE=\"0\") - the attribute ring's BIG_PAGE is OFF: this run does not show the default driver" ;; esac
    else
        echo "2026-10-25 12:00:01.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: ab switches: none set (x)"
    fi
    # the effective state of the plugin as the knobs of the rung change it, and the counters only those knobs move (tools/triage_glyphs.py rung_engaged compares them with the baseline's)
    has() { case " $ev " in *" $1 "*) return 0 ;; esac; return 1; }
    share="live in the surface's family VRAM"; has NVMTL_NO_SURFACE_SHARE=1 && share="private per process"
    po="marked newest; a CPU lock pages it off"; has NVMTL_NO_SURFACE_PAGEOFF=1 && po="copied out eagerly (old)"
    zf="clear their VRAM copy on the GPU"; has NVMTL_MANAGED_EAGER_ZERO=1 && zf="memset their CPU copy at creation and upload it (old)"
    tb=ON; has NVMTL_NO_TEXBUF=1 && tb=OFF
    bl=ON; has NVMTL_NO_BINDLESS=1 && bl=OFF
    se="are NOT submitted"; has NVMTL_SKIP_EMPTY=0 && se="are submitted as before b78"
    dc=0; has NVMTL_DONTCARE_CLEARS=1 && dc=9
    wide=0; wh=0; pg=0; ck=0; sc=0
    has NVMTL_MANAGED_WHOLE=1 && { wh=1; wide=70; }
    has NVMTL_MANAGED_PAGES=1 && { pg=1; wide=50; }
    has NVMTL_MANAGED_CHECK=1 && { ck=1; sc=380; }
    zr=0; has NVMTL_FBFETCH_ZERO=1 && zr=2
    pre="2026-10-25 12:00:02.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL:"
    echo "$pre item 7: IOSurface textures $share (NVK import ABSENT, NVMTL_NO_SURFACE_SHARE=unset)"
    echo "$pre after a GPU write, IOSurface VRAM is $po (NVK dirty export ABSENT)"
    echo "$pre zero-fill: managed buffers $zf (NVMTL_MANAGED_EAGER_ZERO=unset)"
    echo "$pre vk: par3 VK_EXT_mutable_descriptor_type present - texture buffers $tb (mutable texture/storage bands)"
    echo "$pre vk: descriptor indexing present - bindless textures $bl (set 2)"
    echo "$pre commit: empty submission parts $se (NVMTL_SKIP_EMPTY=unset)"
    case "$se" in "are NOT"*) echo "$pre commit: 3 empty submission parts NOT submitted (no encoder, no resources) - each WAS a full submit + wait" ;; esac
    echo "2026-10-25 12:00:11.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: ab: SUMMARY wit_ok=3 wit_early=0 wit_blind=0 wit_worst_us=0 same_image_copies=0 dontcare_seen=9 dontcare_cleared=$dc"
    echo "2026-10-25 12:00:11.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: managed: SUMMARY check=$ck snapshot=0 flushes=40 uploads=80 prologue_uploads=80 sync_uploads=0 checked=0 changed_exact=0 changed_env=0 check_skipped=0 whole=$wh pages=$pg decl_bytes=9000 up_bytes=9000 widened_uploads=$wide ud_scans=$sc ud_idle_scans=0 ud_hits=0 ud_mirrors=3 ud_managed=3"
    echo "2026-10-25 12:00:11.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: vk: fbfetch: SUMMARY served=4 modules=4 read=4 zr_forced=$zr draws=100"
    echo "2026-10-25 12:00:11.000 Df  Chess[$p:1f] (RDNA3MTLDriver) NVMTL: census: SUMMARY seq=1 t_ms=10000 surfaces=1 overflow=0 sin_up_vram=0 sin_up_priv=0 sin_up_mb=0 sin_skip=0 sin_fail=0 pass=10 pass_clear=0 draws=5 draw_blit=2 draw_fbf=3 blitenc=0 replace=0 compute=0 present=0 out_dirty=9 out_copy=0 out_fail=0 top=325:640x353:V:P10,D5"
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
cat > "$T/bin/mktemp" <<'STUBEND'
#!/bin/sh
# mktemp -d /Users/Shared/rdna3-ladder-text.XXXXXX -> a directory in the stub dir
[ "$1" = -d ] && [ "$2" = /Users/Shared/rdna3-ladder-text.XXXXXX ] || { echo "mktemp: unexpected $*" >> "$STUBLOG"; exit 1; }
d=$STUBDIR/textdir.$$; mkdir -p "$d"; echo "mktemp $d" >> "$STUBLOG"; echo "$d"
STUBEND
for c in caffeinate say afplay sleep pkill; do printf '#!/bin/sh\necho "%s $*" >> "$STUBLOG"\nexit 0\n' "$c" > "$T/bin/$c"; done
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
kill() {   # the real kill is not wanted: the fake instances are forgotten, the line is kept; -0 asks whether the instance is still there. IGNORE_INT / IGNORE_TERM / IGNORE_KILL: an
    # instance that survives that signal (a Terminal that shrugs off SIGINT, one that cannot be killed)
    if [ "$1" = -0 ]; then cat "$STUBDIR"/pids-* 2>/dev/null | grep -qx "$2"; return $?; fi
    echo "kill $*" >> "$STUBLOG"
    sig=$1
    case $sig in -INT) [ -n "${IGNORE_INT:-}" ] && return 0 ;; -TERM) [ -n "${IGNORE_TERM:-}" ] && return 0 ;; -KILL) [ -n "${IGNORE_KILL:-}" ] && return 0 ;; esac
    for a in "$@"; do
        case $a in -*) ;; *) for f in "$STUBDIR"/pids-*; do [ -f "$f" ] && { grep -vx "$a" "$f" > "$f.n" || true; mv -f "$f.n" "$f"; }; done ;; esac
    done
    return 0
}
if [ -n "${STOP_AT:-}" ]; then   # an interrupt in the middle of a rung: the harness's own SIGTERM at the Nth hires look
    ladder_look_hires() { n=$(cat "$STUBDIR/looks" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$STUBDIR/looks"
        if [ "$n" = "$STOP_AT" ]; then [ -n "${STRAY:-}" ] && echo 1999 >> "$STUBDIR/pids-Chess"; command kill -${STOP_SIG:-TERM} $$; fi; return 0; }
fi
if [ -n "${FAIL_WS:-}" ]; then bisect_restart_ws() { echo "bisect_restart_ws FAILED (stub)" >> "$STUBLOG"; return 1; }; fi
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
    [ -n "${OWNER_CHESS:-}" ] && echo 600 > "$T/sys/pids-Chess"   # and an owner's Chess that is there already
    echo 90 > "$T/sys/pids-WindowServer"
    env HT="$T" STUBLOG="$T/stub.log" STUBDIR="$T/sys" PATH="$T/bin:$PATH" VARIANT="$v" LADDER_OUT="$T/$name" LADDER_TEXT="$T/text.command" \
        LADDER_LAUNCHWAIT=0 LADDER_SETTLE=0 LADDER_TAIL=0 LADDER_QUITWAIT=0 BISECT_USER=tester BISECT_NOLOGINWAIT=1 \
        BISECT_CONF="$T/conf" BISECT_ALLOW="$T/allow" BISECT_CONFBAK="$T/conf.bak" BISECT_ALLOWBAK="$T/allow.bak" \
        "$@" ${SH:-sh} "$T/h.sh" > "$T/$name.out" 2>&1 < /dev/null || echo "rc=$?" >> "$T/$name.out"
}
cat > "$T/conf.orig" <<'EOF'
# the plugin's knobs
NVMTL_TRACE_RING=262144
  NVMTL_RADV_DEBUG = hang
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
    [ "$(grep -c '^== rung ' "$L")" = 8 ] && [ "$(grep -c '^== end rung ' "$L")" = 8 ] || fail "$SH: not 8 rungs: $(grep '^== ' "$L")"
    [ "$(grep '^== rung ' "$L" | sed 's/^== rung //' | tr '\n' ' ' | sed 's/ $//')" = "$RUNGS" ] || fail "$SH: the rungs ran in a different order: $(grep '^== rung ' "$L")"
    [ "$(grep -c '^open -n -a ' "$T/stub.log")" = 16 ] || fail "$SH: expected 16 open calls: $(grep -c '^open' "$T/stub.log")"
    n=0
    for r in $RUNGS; do
        n=$((n + 1))
        case $r in
            baseline) want="" ;;
            attrring) want="--env NVMTL_ATTR_RING_BIG_PAGE=0" ;;
            radv) want="--env NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears" ;;
            sync) want="--env NVMTL_ASYNC_COMMIT=0" ;;
            managed) want="--env NVMTL_MANAGED_CHECK=1 --env NVMTL_MANAGED_SNAPSHOT=1" ;;
            alignguard) want="--env NVMTL_NO_ALIGN_GUARD=1" ;;
            paranoid) want="--env NVMTL_PARANOID=1" ;;
            loadbarrier) want="--env NVMTL_LOAD_BARRIER=1" ;;
        esac
        got=$(envs_of $n | sed -E 's/^open -n -a (Terminal|Chess) ?//; s# ?[^ ]*text\.command --args -ApplePersistenceIgnoreState YES$##; s/ +$//' | sort -u)
        [ "$(echo "$got" | wc -l | tr -d ' ')" = 1 ] || fail "$SH: rung $r: Terminal and Chess got different switches: $got"
        [ "$got" = "$want" ] || fail "$SH: rung $r opened its apps with [$got], expected [$want]"
        [ -f "$T/apps/$r.log" ] || fail "$SH: rung $r has no log slice"
        grep -q "^== rung $r\$" "$L" && grep -q "^looks: ladder-${r}a ladder-${r}h\$" "$L" || fail "$SH: rung $r: looks line"
        [ -f "$T/home/rdna3-scanout/ladder-${r}a/scanout-pipe0.ppm" ] && [ -f "$T/home/rdna3-scanout/ladder-${r}h/scanout-pipe0.ppm" ] || fail "$SH: rung $r: both looks' pictures"
    done
    grep -q "^open -n -a Terminal .*$T/text.command --args -ApplePersistenceIgnoreState YES\$" "$T/stub.log" || fail "$SH: the Terminal is not opened with the text script and without state restoration"
    # the tick Terminal opens AFTER Chess in every rung: both open in the top left, the newer window is in front (card run 2026-10-26b: Chess on top hid the text)
    [ "$(grep '^open -n -a ' "$T/stub.log" | awk 'NR % 2 == 1 { print $4 }' | sort -u | tr '\n' ' ')" = "Chess " ] && [ "$(grep '^open -n -a ' "$T/stub.log" | awk 'NR % 2 == 0 { print $4 }' | sort -u | tr '\n' ' ')" = "Terminal " ] || fail "$SH: Chess is not opened before the Terminal in every rung"
    [ "$(grep -c '^open -n -a Terminal ' "$T/stub.log")" = "$(grep -c -e '^open -n -a Terminal .*-ApplePersistenceIgnoreState YES$' "$T/stub.log")" ] || fail "$SH: a Terminal was opened that restores the last rung's windows"
    ! grep -q '^open -n -a Chess .*--args' "$T/stub.log" || fail "$SH: Chess was opened with --args (only the Terminal's restored windows are a problem)"
    [ -x "$T/text.command" ] && grep -q 'ls -laG' "$T/text.command" || fail "$SH: the text script"
    # the text script: the tick lines in ONE fixed colour (bright white, no colour from the counter), a line of mixed glyphs now and then, valid sh
    sh -n "$T/text.command" || fail "$SH: the text script does not parse"
    [ "$(grep -c 'rdna3 ladder tick  abcdefghijklmnopqrstuvwxyz' "$T/text.command")" = 1 ] && grep 'rdna3 ladder tick' "$T/text.command" | grep -q '\\033\[97m' || fail "$SH: the tick line is not in one fixed colour"
    ! grep 'rdna3 ladder tick' "$T/text.command" | grep -q 'i % 7' || fail "$SH: the tick line still takes its colour from the counter"
    grep -q 'ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 !#\$%%&' "$T/text.command" || fail "$SH: the mixed-glyph line is missing"
    [ "$(grep -c '^drminfo --scanout-hires ' "$T/stub.log")" = 8 ] && [ "$(grep -c '^drminfo --scanout ' "$T/stub.log")" = 8 ] || fail "$SH: looks: $(grep -c drminfo "$T/stub.log")"
    grep -q '^log show --start ' "$T/stub.log" && grep -q 'processID == 1000' "$T/stub.log" || fail "$SH: the log slice is not by pid"
    # only the pids this run started are quit; the owner's Terminal (500) is not
    ! grep -q '^kill .*500' "$T/stub.log" || fail "$SH: the owner's Terminal was killed"
    # the quit needs no consent: SIGINT first (no System Events), and the evidence file says how every instance ended
    ! grep -q 'osascript\|System Events' "$T/stub.log" || fail "$SH: the quit goes through System Events/osascript"
    [ "$(grep -c '^kill -INT ' "$T/stub.log")" = 8 ] && [ "$(grep -c '^quit: pid [0-9]* ended by SIGINT after ' "$L")" = 16 ] || fail "$SH: the quit by SIGINT: $(grep -c '^kill -INT' "$T/stub.log") kills, $(grep -c '^quit: pid' "$L") lines"
    ! grep -q '^kill -TERM' "$T/stub.log" || fail "$SH: an instance that quit on SIGINT was TERMed too"
    [ "$(grep -n '^kill -INT' "$T/stub.log" | head -1 | cut -d: -f1)" -lt "$(grep -n '^log show' "$T/stub.log" | head -1 | cut -d: -f1)" ] || fail "$SH: the log slice is taken before the apps are asked to quit"
    grep -q '^pkill -f ' "$T/stub.log" || fail "$SH: the text script was not ended before the quit"
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
gl = {n: {"verdict": "broken", "text": "BROKEN: 3 of 20 (test)"} for n in rungs}   # (the looks are 2x2 stubs: the verdicts are given, the run's evidence is what is tested)
rows = t.ladder_rows(rungs, v, probs, gl)
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
    [ "$(grep -c '^== rung ' "$L")" = 10 ] && [ "$(grep '^== rung ' "$L" | tail -2 | sed 's/^== rung //' | tr '\n' ' ')" = "radv-ws paranoid-ws " ] || fail "$SH: variant 2 rungs: $(grep '^== rung ' "$L" | tr '\n' ' ')"
    cmp -s "$T/conf" "$T/conf.orig" || fail "$SH: rdna3mtl.conf is not as it was: $(cat "$T/conf")"
    cmp -s "$T/allow" "$T/allow.orig" || fail "$SH: the allow list is not as it was"
    [ ! -e "$T/conf.bak" ] && [ ! -e "$T/conf.bak.absent" ] && [ ! -e "$T/allow.bak" ] || fail "$SH: backups left behind"
    [ "$(grep -c '^killall -HUP WindowServer' "$T/stub.log")" = 3 ] || fail "$SH: WindowServer restarts: $(grep -c killall "$T/stub.log"), expected 3 (two rungs and the last one back)"
    c1=$T/sys/conf-at-ws-1; c2=$T/sys/conf-at-ws-2; c3=$T/sys/conf-at-ws-3
    grep -q '^NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears$' "$c1" && ! grep -q 'hang' "$c1" || fail "$SH: radv-ws: the conf at the restart: $(cat "$c1")"
    grep -q '^NVMTL_TRACE_RING=262144$' "$c1" || fail "$SH: radv-ws: the conf lost another line: $(cat "$c1")"
    grep -q '^NVMTL_PARANOID=1$' "$c2" && grep -q '^  NVMTL_RADV_DEBUG = hang$' "$c2" && ! grep -q 'nodcc' "$c2" || fail "$SH: paranoid-ws: the conf at the restart: $(cat "$c2")"
    cmp -s "$c3" "$T/conf.orig" || fail "$SH: the last WindowServer restart did not run with the original conf: $(cat "$c3")"
    # the WindowServer rungs' log slice includes WindowServer's own pid
    grep -q 'processID == 90' "$T/stub.log" || fail "$SH: the WindowServer rungs' log slice does not name WindowServer"
    # no conf file at all before: none after
    rm -f "$T/conf" "$T/conf.bak" "$T/conf.bak.absent"; cp "$T/allow.orig" "$T/allow"
    run 2 ws2
    grep -q '^rc=0$' "$T/ws2.out" || fail "$SH: no-conf run rc"
    [ ! -e "$T/conf" ] && [ ! -e "$T/conf.bak.absent" ] || fail "$SH: a conf the run made was left: $(cat "$T/conf" 2>/dev/null)"
    # an interrupt in the middle of a rung: SIGTERM at the 9th hires look = the first WindowServer rung (variant 2), so that the conf is at stake
    fresh
    run 2 intr STOP_AT=9
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

    # review round 2: a failed WindowServer restart still restores the conf and restarts WindowServer at the end
    fresh
    run 2 wsfail FAIL_WS=1
    cmp -s "$T/conf" "$T/conf.orig" && [ ! -e "$T/conf.bak" ] || fail "$SH: after a failed WindowServer restart the conf is not as it was: $(cat "$T/conf")"
    grep -q '^failed: WindowServer did not come back' "$T/wsfail/ladder.txt" && grep -q '^ws_restart: final' "$T/wsfail/ladder.txt" || fail "$SH: a failed restart: no note, or no final restart: $(grep -e failed -e ws_restart "$T/wsfail/ladder.txt")"
    [ "$(grep -c '^bisect_restart_ws FAILED' "$T/stub.log")" = 2 ] || fail "$SH: expected the rung's restart and the final one: $(grep -c FAILED "$T/stub.log")"
    # instances of the owner's that were there before the run are never quit; one the pid capture missed is (sweep), only without a WindowServer restart
    fresh
    OWNER_CHESS=1
    run 1 base STOP_AT=2 STRAY=1
    OWNER_CHESS=
    grep -q '^rc=143$' "$T/base.out" && grep -q '^baseline_pids: Terminal=500 Chess=600 *$' "$T/base/ladder.txt" || fail "$SH: baseline_pids: $(grep baseline "$T/base/ladder.txt"; tail -2 "$T/base.out")"
    grep -q '^kill -TERM 1999' "$T/stub.log" && grep -q '^sweep: Chess instance(s) 1999' "$T/base/ladder.txt" || fail "$SH: a missed instance was not swept"
    ! grep -q '^kill .*\b600\b' "$T/stub.log" && ! grep -q 'unix id is 600' "$T/stub.log" && [ "$(cat "$T/sys/pids-Chess")" = 600 ] || fail "$SH: the owner's Chess was touched: $(cat "$T/sys/pids-Chess")"
    # an instance that shrugs off SIGINT is TERMed, one that shrugs off both is KILLed, one that survives even that is reported; each line says how it ended
    fresh
    run 1 stubborn IGNORE_INT=1
    grep -q '^rc=0$' "$T/stubborn.out" && grep -q '^kill -TERM 1001 1000$' "$T/stub.log" && [ -z "$(cat "$T/sys/pids-Chess" 2>/dev/null)" ] || fail "$SH: an app that ignores SIGINT is not TERMed"
    [ "$(grep -c '^quit: pid [0-9]* ended by SIGTERM after ' "$T/stubborn/ladder.txt")" = 16 ] && ! grep -q 'ended by SIGINT' "$T/stubborn/ladder.txt" || fail "$SH: how the instances ended (TERM): $(grep '^quit' "$T/stubborn/ladder.txt" | sort | uniq -c | head -3)"
    fresh
    run 1 killed IGNORE_INT=1 IGNORE_TERM=1
    [ "$(grep -c '^quit: pid [0-9]* ended by SIGKILL after ' "$T/killed/ladder.txt")" = 16 ] && [ -z "$(cat "$T/sys/pids-Chess" 2>/dev/null)" ] || fail "$SH: an app that ignores INT and TERM is not KILLed: $(grep '^quit' "$T/killed/ladder.txt" | head -3)"
    fresh
    run 1 immortal IGNORE_INT=1 IGNORE_TERM=1 IGNORE_KILL=1
    grep -q '^quit: pid(s) .* SURVIVED SIGKILL$' "$T/immortal/ladder.txt" || fail "$SH: an instance that survives SIGKILL is not reported: $(grep '^quit' "$T/immortal/ladder.txt" | head -3)"
    python3 -I - "$T/immortal/ladder.txt" "$T/killed/ladder.txt" "$T/apps/ladder.txt" "$T/stubborn/ladder.txt" <<'PYEOF' || fail "$SH: the triage does not read how the instances ended"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t
def quit_row(path):
    v, rungs, probs = t.parse_ladder(open(path).read(), {})
    return [r for r in t.ladder_rows(rungs, v, probs) if r[1] == "ladder quit"]
imm, kil, ok, stub = (quit_row(a) for a in sys.argv[1:5])
assert imm and imm[0][0] == "WARN" and "STILL ALIVE after SIGKILL" in imm[0][2], imm
assert kil and kil[0][0] == "INFO" and "ended by SIGKILL" in kil[0][2], kil
assert ok and ok[0][0] == "INFO" and "16 instance(s) ended by SIGINT" in ok[0][2], ok
assert stub and "ended by SIGTERM" in stub[0][2], stub
PYEOF
    # a log slice over 600000 bytes keeps its END (the SUMMARY lines) and says it was truncated
    fresh
    run 1 big BIGLOG=1
    grep -q '^rc=0$' "$T/big.out" && grep -q 'slice truncated: kept the last 600000 of [0-9]* bytes' "$T/big/ladder.txt" && grep -q 'census: SUMMARY' "$T/big/baseline.log" && [ "$(wc -c < "$T/big/baseline.log" | tr -d ' ')" -le 600000 ] || fail "$SH: a large log slice: $(grep '^log: baseline' "$T/big/ladder.txt")"
    # a tick window of an earlier run is said before the first rung
    fresh
    run 1 oldtick OLDTICK=1
    grep -q '^WARN: the text script of an earlier run is still running (pids 4242 )' "$T/oldtick/ladder.txt" || fail "$SH: an earlier run's text script is not warned about: $(grep '^WARN' "$T/oldtick/ladder.txt")"
    # a signal ends the run with a line that names it and the rung, and the triage shows it
    grep -q '^ABORTED by SIGTERM at rung ' "$T/intr1/ladder.txt" && grep -q '^ABORTED by SIGTERM at rung ' "$T/intr/ladder.txt" || fail "$SH: an interrupted run left no ABORTED line: $(tail -3 "$T/intr1/ladder.txt")"
    python3 -I - "$T/intr1/ladder.txt" <<'PYEOF' || fail "$SH: the triage does not show the abort"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t
v, rungs, probs = t.parse_ladder(open(sys.argv[1]).read(), {})
assert any(p.startswith("ABORTED by SIGTERM") for p in probs), probs
assert any(r[1] == "ladder" and r[2].startswith("ABORTED") for r in t.ladder_rows(rungs, v, probs, {})), probs
PYEOF
    fresh
    run 1 hup1 STOP_AT=2 STOP_SIG=HUP
    grep -q '^rc=129$' "$T/hup1.out" && grep -q '^ABORTED by SIGHUP at rung ' "$T/hup1/ladder.txt" && cmp -s "$T/conf" "$T/conf.orig" && [ -z "$(cat "$T/sys/pids-Chess" 2>/dev/null)" ] || fail "$SH: SIGHUP in an app rung: $(tail -3 "$T/hup1.out")"
    fresh
    run 3 hup3 STOP_AT=3 STOP_SIG=HUP
    grep -q '^rc=129$' "$T/hup3.out" && grep -q '^ABORTED by SIGHUP at rung ' "$T/hup3/ladder.txt" || fail "$SH: SIGHUP in the glyph hunt: $(tail -3 "$T/hup3.out")"
    fresh
    run 2 hup2 STOP_AT=9 STOP_SIG=HUP
    grep -q '^rc=0$' "$T/hup2.out" && ! grep -q '^ABORTED' "$T/hup2/ladder.txt" || fail "$SH: the WindowServer variant must go on after SIGHUP (the restart closes its Terminal): $(tail -3 "$T/hup2.out")"
    # the text script is made in a directory of mktemp's and removed at the end (no fixed name in /Users/Shared)
    fresh
    run 1 mk LADDER_TEXT=
    grep -q '^rc=0$' "$T/mk.out" && grep -q '^mktemp ' "$T/stub.log" && grep -q "^open -n -a Terminal .*/textdir\.[0-9]*/text.command --args -ApplePersistenceIgnoreState YES\$" "$T/stub.log" || fail "$SH: the text script's directory: $(grep -e mktemp -e 'open -n -a Terminal' "$T/stub.log" | head -3)"
    [ -z "$(ls -d "$T"/sys/textdir.* 2>/dev/null)" ] || fail "$SH: the text script's directory was left behind"
    ! grep -q 'Users/Shared' "$T/stub.log" || fail "$SH: a fixed /Users/Shared name is used"

    # ------------------------------------------------------------ variant 3: the glyph hunt, one knob per rung, two hi-res looks per rung
    fresh
    run 3 par
    L=$T/par/ladder.txt
    grep -q '^rc=0$' "$T/par.out" || fail "$SH: variant 3 rc: $(tail -5 "$T/par.out")"
    grep -q '^variant: 3 (ladder-glyphs)$' "$L" || fail "$SH: variant 3 header: $(grep '^variant' "$L")"
    [ "$(grep -c '^== rung ' "$L")" = 13 ] && [ "$(grep -c '^== end rung ' "$L")" = 13 ] || fail "$SH: not 13 rungs: $(grep -c '^== rung ' "$L")"
    [ "$(grep '^== rung ' "$L" | sed 's/^== rung //' | tr '\n' ' ' | sed 's/ $//')" = "$PRUNGS" ] || fail "$SH: variant 3 rungs ran in a different order: $(grep '^== rung ' "$L" | tr '\n' ' ')"
    [ "$(grep -c '^open -n -a ' "$T/stub.log")" = 26 ] || fail "$SH: variant 3: expected 26 open calls: $(grep -c '^open' "$T/stub.log")"
    [ "$(grep -c '^drminfo --scanout-hires ' "$T/stub.log")" = 26 ] && [ "$(grep -c '^drminfo --scanout ' "$T/stub.log")" = 13 ] || fail "$SH: variant 3 looks: $(grep -c '^drminfo --scanout-hires' "$T/stub.log") hires (two per rung), $(grep -c '^drminfo --scanout ' "$T/stub.log") small"
    n=0
    for r in $PRUNGS; do
        n=$((n + 1))
        case $r in
            baseline) want="" ;;
            managed-whole) want="--env NVMTL_MANAGED_WHOLE=1" ;;
            managed-check) want="--env NVMTL_MANAGED_CHECK=1" ;;
            no-surface-share) want="--env NVMTL_NO_SURFACE_SHARE=1" ;;
            no-surface-pageoff) want="--env NVMTL_NO_SURFACE_PAGEOFF=1" ;;
            fbfetch-zero) want="--env NVMTL_FBFETCH_ZERO=1" ;;
            managed-pages) want="--env NVMTL_MANAGED_PAGES=1" ;;
            managed-eager-zero) want="--env NVMTL_MANAGED_EAGER_ZERO=1" ;;
            no-texbuf) want="--env NVMTL_NO_TEXBUF=1" ;;
            no-bindless) want="--env NVMTL_NO_BINDLESS=1" ;;
            dontcare-clears) want="--env NVMTL_DONTCARE_CLEARS=1" ;;
            skip-empty-0) want="--env NVMTL_SKIP_EMPTY=0" ;;
            paranoid) want="--env NVMTL_PARANOID=1" ;;
            *) fail "$SH: the test does not know rung $r" ;;
        esac
        got=$(envs_of $n | sed -E 's/^open -n -a (Terminal|Chess) ?//; s# ?[^ ]*text\.command --args -ApplePersistenceIgnoreState YES$##; s/ +$//' | sort -u)
        [ "$(echo "$got" | wc -l | tr -d ' ')" = 1 ] && [ "$got" = "$want" ] || fail "$SH: variant 3 rung $r opened its apps with [$got], expected [$want]"
        [ -f "$T/par/$r.log" ] && grep -q "^looks: ladder-${r}a ladder-${r}h ladder-${r}h2\$" "$L" || fail "$SH: variant 3 rung $r: log slice or looks: $(grep '^looks' "$L" | sed -n ${n}p)"
        [ -f "$T/home/rdna3-scanout/ladder-${r}h/scanout-pipe0.ppm" ] && [ -f "$T/home/rdna3-scanout/ladder-${r}h2/scanout-pipe0.ppm" ] || fail "$SH: variant 3 rung $r: the two hires pictures"
    done
    cmp -s "$T/conf" "$T/conf.orig" && [ ! -e "$T/conf.bak" ] && ! grep -q '^killall' "$T/stub.log" || fail "$SH: variant 3 touched rdna3mtl.conf or WindowServer"
    # the default ladder takes ONE hires look per rung (unchanged), only the glyph hunt takes the second
    [ "$(grep -c 'h2$' "$T/apps/ladder.txt")" = 0 ] || fail "$SH: variant 1 took a second hires look"
    # every knob of a rung is one the plugin reads, with the value it reads (the plugin's source is the authority), and is on the list `vk: ab switches` shows
    python3 -I - "$L" "$ROOT" <<'PYEOF' || fail "$SH: variant 3: the knobs against the plugin's source"
import re, sys, glob
L, root = sys.argv[1], sys.argv[2]
src = "".join(open(f).read() for f in glob.glob(root + "/metal/plugin/*.[mch]"))
conf, cur = {}, None
for line in open(L):
    m = re.match(r"^== rung (\S+)", line)
    if m: cur = m.group(1)
    m = re.match(r"^conf: (.*)$", line)
    if m and cur: conf[cur] = dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv)
assert len(conf) == 13 and conf["baseline"] == {}, conf
assert all(len(c) == 1 for r, c in conf.items() if r != "baseline"), "one knob per rung"
for r, c in conf.items():
    for k, v in c.items():
        assert 'getenv("%s")' % k in src, "%s: the plugin never reads %s" % (r, k)
        assert '"%s"' % k in src[src.index("nvmtl_ab_knobs[]"):][:3000], "%s: %s is not on the list `vk: ab switches` shows" % (r, k)
# the values mean what the rung says: =1 for the knobs that test the first character / presence, SKIP_EMPTY=0 turns the default off
assert conf["skip-empty-0"] == {"NVMTL_SKIP_EMPTY": "0"} and all(v == "1" for r, c in conf.items() if r != "skip-empty-0" for v in c.values())
assert 'on = !(e && e[0] == \'0\')' in src[src.index('getenv("NVMTL_SKIP_EMPTY")') - 60:src.index('getenv("NVMTL_SKIP_EMPTY")') + 120], "SKIP_EMPTY is on unless it starts with 0"
assert 'on = !(e && e[0] == \'1\')' in src[src.index('getenv("NVMTL_MANAGED_EAGER_ZERO")') - 60:src.index('getenv("NVMTL_MANAGED_EAGER_ZERO")') + 120], "MANAGED_EAGER_ZERO=1 is the old behaviour"
PYEOF
    # the triage reads it: no WARN, the rungs in order, a rung that did not reach the plugin is caught
    python3 -I - "$L" "$T/par" <<'PYEOF' || fail "$SH: variant 3: tools/triage_glyphs.py does not read the ladder"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t
text = open(sys.argv[1]).read()
logs = {r: open(os.path.join(sys.argv[2], r + ".log")).read() for r in t.LADDER_GLYPHS_RUNGS}
v, rungs, probs = t.parse_ladder(text, logs)
assert v == 3 and not probs and list(rungs) == list(t.LADDER_GLYPHS_RUNGS), (v, probs, list(rungs))
assert all(r["looks"] == ["ladder-%sa" % n, "ladder-%sh" % n, "ladder-%sh2" % n] for n, r in rungs.items()), [r["looks"] for r in rungs.values()][:2]
gl = {n: {"verdict": "broken", "text": "BROKEN: 3 of 20 (test)"} for n in rungs}
rows = t.ladder_rows(rungs, v, probs, gl)
bad = [r for r in rows if r[0] != "INFO"]
assert not bad, bad
assert [r[1] for r in rows if r[1].startswith("ladder ")][:3] == ["ladder baseline", "ladder managed-whole", "ladder managed-check"], rows[:3]
assert all("knob engaged" in [r for r in rows if r[1] == "ladder " + n][0][2] for n in rungs if n != "baseline"), "every rung's knob engaged in the stub run"
m = [r for r in rows if r[1] == "ladder no-texbuf"][0]
assert "NVMTL_NO_TEXBUF" in m[2], m
# a log in which the knob never reached the plugin is a WARN
rows2 = t.ladder_rows(dict(rungs, **{"managed-pages": dict(rungs["managed-pages"], log=logs["baseline"])}), v, probs, gl)
m2 = [r for r in rows2 if r[1] == "ladder managed-pages"][0]
assert m2[0] == "WARN" and "did not reach the plugin" in m2[2] and "Terminal pid" in m2[2] and "engagement could not be shown" in m2[2], m2
PYEOF
    echo "test_ladder_step: ok glyph hunt ($SH)"

    # ------------------------------------------------------------ nobody logged in; an app that does not start; tty answers
    fresh
    run 1 nobody BISECT_USER=root
    grep -q '^rc=1$' "$T/nobody.out" && grep -q '^ABORT: nobody logged in' "$T/nobody/ladder.txt" || fail "$SH: nobody logged in: $(tail -3 "$T/nobody.out")"
    ! grep -q '^open' "$T/stub.log" && cmp -s "$T/conf" "$T/conf.orig" || fail "$SH: nobody logged in, yet something ran"
    fresh
    run 1 noapp FAIL_APP=Chess
    grep -q '^WARN: an app did not start' "$T/noapp/ladder.txt" && [ "$(grep -c '^== end rung ' "$T/noapp/ladder.txt")" = 8 ] || fail "$SH: an app that did not start is not reported: $(grep -c WARN "$T/noapp/ladder.txt")"
    printf 'n\nn\ny\ny\n-\n\nn\ny\n' > "$T/keys"
    fresh
    run 1 tty BISECT_KEYFILE="$T/keys"
    grep -q '^mode: tty$' "$T/tty/ladder.txt" || fail "$SH: tty mode"
    [ "$(grep '^answer: ' "$T/tty/ladder.txt" | sed 's/^answer: //' | tr '\n' ' ')" = "n n y y - - n y " ] || fail "$SH: the owner's answers: $(grep '^answer: ' "$T/tty/ladder.txt" | tr '\n' ' ')"
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
grep -q -- '--ladder) LADDER=1' "$AR" && grep -q -- '--ladder-ws) LADDER=2' "$AR" && grep -q -- '--ladder-glyphs) LADDER=3' "$AR" || fail "autoreport.sh: --ladder / --ladder-ws / --ladder-glyphs"
grep -q 'rdna3ladder=1 "\*) \[ -n "\$LADDER" \] || LADDER=1' "$AR" && grep -q 'rdna3ladder=2 "\*) \[ -n "\$LADDER" \] || LADDER=2' "$AR" && grep -q 'rdna3ladder=3 "\*) \[ -n "\$LADDER" \] || LADDER=3' "$AR" || fail "autoreport.sh: boot-args rdna3ladder=1 / 2 / 3"
grep -q 'ladder_run "\$LADDER"' "$AR" || fail "autoreport.sh: ladder_run is not called"
grep -q '\[ -z "\$BISECT" \] && \[ -z "\$PLAY" \] && \[ -z "\$LADDER" \]' "$AR" || fail "autoreport.sh: the other tests still run in a ladder run"
grep -q 'rdna3-ladder/ladder.txt' "$AR" || fail "autoreport.sh: ladder.txt is not sent"
grep -q '41-ladder' "$ROOT/tools/macos/collect.sh" || fail "collect.sh: 41-ladder"
grep -q '^ladder .* rdna3ladder=1$' "$ROOT/tools/ladder-steps.txt" && grep -q '^ladder-ws .* rdna3ladder=2$' "$ROOT/tools/ladder-steps.txt" || fail "tools/ladder-steps.txt: ladder / ladder-ws"
play=$(awk '$1 == "play" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
lad=$(awk '$1 == "ladder" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ "$lad" = "${play% rdna3play=1} rdna3ladder=1" ] || fail "ladder-steps.txt: ladder is [$lad], expected the play step's boot-args + rdna3ladder=1"
[ "$(grep -c 'play-agdcscaler ladder ladder-ws ladder-glyphs' "$ROOT/tools/vm/rdna3vm")" = 2 ] || fail "rdna3vm: ladder / ladder-ws / ladder-glyphs are not in both step lists"
par=$(awk '$1 == "ladder-glyphs" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ "$par" = "${play% rdna3play=1} rdna3ladder=3" ] || fail "ladder-steps.txt: ladder-glyphs is [$par], expected the play step's boot-args + rdna3ladder=3"
sed -n '/^auto_limit() {/,/^}/p' "$ROOT/tools/vm/rdna3vm" > "$T/limit.sh"
m1=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$lad")
m2=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "${lad%1}2")
m3=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$par")
mp=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$play")
[ "$m1" -ge 20 ] && [ "$m1" -lt 40 ] && [ "$m2" -ge 40 ] && [ "$m2" -lt 60 ] && [ "$mp" = 30 ] || fail "rdna3vm: auto_limit ladder $m1, ladder-ws $m2, play $mp (expected 20..39, 40..59, 30)"
[ "$m3" -ge 35 ] && [ "$m3" -lt 60 ] || fail "rdna3vm: auto_limit ladder-glyphs $m3 (13 rungs of ~95 s need 21 min of rungs: expected 35..59)"
grep -q '41-ladder.txt' "$ROOT/tools/triage_reports.py" && grep -q 'parse_ladder' "$ROOT/tools/triage_reports.py" || fail "triage_reports.py: the ladder is not read"
echo "test_ladder_step: ok"
