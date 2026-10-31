#!/bin/bash
# The app survey (`rdna3vm step apps`, boot-arg rdna3ladder=5): the APPS block of tools/vm/macos/autoreport.sh on the host (with the BISECT and LADDER
# blocks it builds on), stubs for open, pgrep, kill, log, lsof, sips, curl and rdna3_drminfo; under sh, bash and dash when they exist:
#  - the app list is the one tools/triage_apps.py reads, each app opened as the survey promises: the right app, the right content (Safari's three pages,
#    Preview's PDF and JPEG, Settings' three panes, the movie, Mission Control with two windows), the DEFAULT driver (no --env, no sysctl, no conf edit, no
#    WindowServer restart), Terminal as a new instance with the tick script, Chess as a new instance
#  - the content the run makes (HTML pages, RTF, PDF, JPEG, MP4) exists and is what it says it is
#  - per app and stage: a 320-wide and a hi-res look (names scanoutapps-<app>[-<stage>]a|h), a second hi-res look for the Terminal, a log slice
#  - the quit: only what the run started, SIGINT first, a line per instance; the owner's own instance of an app is never quit
#  - a stage that does not open (the survey goes on), an app that dies with and without a crash report, a WindowServer that restarted (with and without the
#    desktop coming back), a plugin that is not loaded, notable log lines: each reaches ladder.txt and the triage rows
#  - tools/triage_apps.py reads what the block wrote (parse_ladder + apps_rows)
#  - the wiring: --apps, rdna3ladder=5, the step line, both step lists, the auto limit, the triage dispatch, the Makefile
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "${T:?}"' EXIT
fail() { echo "test_apps_step: FAIL $*"; exit 1; }
AR=$ROOT/tools/vm/macos/autoreport.sh
export ROOT

for f in tools/vm/macos/autoreport.sh tools/vm/rdna3vm; do bash -n "$ROOT/$f" || fail "bash -n $f"; done
if command -v dash >/dev/null 2>&1; then dash -n "$AR" || fail "dash -n autoreport.sh"; fi
sh -n "$AR" || fail "sh -n autoreport.sh"

sed -n '/^# BISECT-BEGIN/,/^# BISECT-END/p' "$AR" > "$T/bisect.sh"
sed -n '/^# LADDER-BEGIN/,/^# LADDER-END/p' "$AR" > "$T/ladder.sh"
sed -n '/^# APPS-BEGIN/,/^# APPS-END/p' "$AR" > "$T/apps.sh"
[ "$(wc -l < "$T/bisect.sh")" -gt 150 ] && [ "$(wc -l < "$T/ladder.sh")" -gt 60 ] && [ "$(wc -l < "$T/apps.sh")" -gt 150 ] || fail "the BISECT / LADDER / APPS blocks were not found in autoreport.sh"

# ---- the app list = the triage's
RUNGS=$(sed -n 's/^APPS_RUNGS="\(.*\)"$/\1/p' "$T/apps.sh")
PY=$(python3 -I -c "
import sys; sys.path.insert(0, '$ROOT/tools')
import triage_apps as t
print(' '.join(t.APPS_RUNGS))")
[ "$RUNGS" = "$PY" ] && [ "$(echo $RUNGS | wc -w | tr -d ' ')" = 15 ] || fail "apps: autoreport.sh [$RUNGS] vs triage_apps.py [$PY]"

# ---- the stubs
mkdir -p "$T/bin" "$T/D/bin" "$T/crash"
cat > "$T/bin/open" <<'EOF'
#!/bin/sh
# open [-n] -a APP [file] [--args ...] | open URL: a new instance (-n, or none running yet) = a new pid in $STUBDIR/pids-APP (a name with a blank: underscore)
echo "open $*" >> "$STUBLOG"
newi=
[ "$1" = -n ] && { newi=1; shift; }
app=
case "$1" in -a) app=$2 ;; x-apple.systempreferences:*) app="System Settings" ;; esac
[ -n "$app" ] || exit 0
f="$STUBDIR/pids-$(echo "$app" | tr ' ' '_')"
[ "${FAIL_APP:-}" = "$app" ] && exit 1
case "$app" in "Mission Control"|Launchpad|Apps) exit 0 ;; esac   # the Dock shows them: no process to find (macOS 26: no "Apps" process)
if [ -n "$newi" ] || [ ! -s "$f" ]; then
    n=$(cat "$STUBDIR/next" 2>/dev/null || echo 1000); echo $((n + 1)) > "$STUBDIR/next"
    echo $n >> "$f"
    [ "$app" = Safari ] && printf '2001\n2002\n' > "$STUBDIR/pids-WebKit"
    if [ "${DIE_APP:-}" = "$app" ]; then
        echo $n > "$STUBDIR/die"
        [ -n "${DIE_CRASH:-}" ] && touch "$CRASHDIR/$(echo "$app" | tr ' ' '_')-2026-10-26-230000.ips"
    fi
    [ "${NOPLUGIN_APP:-}" = "$app" ] && echo $n >> "$STUBDIR/noplugin"
    [ "${WS_APP:-}" = "$app" ] && { echo 91 > "$STUBDIR/pids-WindowServer"; [ -n "${NOLOGIN:-}" ] && echo loginwindow > "$STUBDIR/user"; touch "$CRASHDIR/WindowServer-2026-10-26-230001.ips"; }
fi
exit 0
EOF
cat > "$T/bin/pgrep" <<'EOF'
#!/bin/sh
case "$1" in
    -x) cat "$STUBDIR/pids-$(echo "$2" | tr ' ' '_')" 2>/dev/null ;;
    -f) case "$2" in *WebKit*) cat "$STUBDIR/pids-WebKit" 2>/dev/null ;; esac ;;
esac
exit 0
EOF
cat > "$T/bin/log" <<'EOF'
#!/bin/sh
# log show --start T --info --style compact --predicate '...': the NVMTL lines of each pid in the predicate, WindowServer's and the kernel's when asked for
echo "log $*" >> "$STUBLOG"
pred=
while [ $# -gt 0 ]; do [ "$1" = --predicate ] && pred=$2; shift; done
echo "$pred" > "$STUBDIR/last-predicate"
for p in $(echo "$pred" | grep -o 'processID == [0-9]*' | sed 's/.* //'); do
    echo "2026-10-26 23:00:01.000 Df  App[$p:1f] (RDNA3MTLDriver) NVMTL: vk: ab switches: none set (x)"
    echo "2026-10-26 23:00:11.000 Df  App[$p:1f] (RDNA3MTLDriver) NVMTL: census: SUMMARY seq=1 t_ms=10000 surfaces=1 overflow=0 pass=10 draws=5 present=3 draw_blit=2"
    echo "2026-10-26 23:00:12.000 Df  App[$p:1f] (Metal) some harmless Metal line"
    [ -n "${NOTABLE:-}" ] && echo "2026-10-26 23:00:13.000 E   App[$p:1f] (Metal) MTLDevice error: invalid pixel format"
done
case "$pred" in *WindowServer*) echo "2026-10-26 23:00:02.000 Df  WindowServer[90:1] (RDNA3MTLDriver) NVMTL: ws line" ;; esac
case "$pred" in *kernel*) echo "2026-10-26 23:00:03.000 Df  kernel[0:0] RDNA3Accel: fine line"
    [ -n "${NOTABLE:-}" ] && echo "2026-10-26 23:00:04.000 E   kernel[0:0] RDNA3Accel: VM fault at 0x1000 (gfx ring)" ;; esac
exit 0
EOF
cat > "$T/bin/lsof" <<'EOF'
#!/bin/sh
# lsof +c 0 -n -p P1,P2: the plugin line of each pid that is not in $STUBDIR/noplugin
echo "lsof $*" >> "$STUBLOG"
for p in $(echo "$5" | tr ',' ' '); do
    grep -qx "$p" "$STUBDIR/noplugin" 2>/dev/null && continue
    echo "App $p root txt REG 1,5 100 1 /Library/GPUBundles/RDNA3MTLDriver.bundle/Contents/MacOS/RDNA3MTLDriver"
done
exit 0
EOF
cat > "$T/bin/sips" <<'EOF'
#!/bin/sh
echo "sips $*" >> "$STUBLOG"
out=; while [ $# -gt 0 ]; do [ "$1" = --out ] && out=$2; shift; done
[ -n "$out" ] && [ -z "${NO_SIPS:-}" ] && printf '\377\330JPEG' > "$out"
exit 0
EOF
cat > "$T/bin/mktemp" <<'STUBEND'
#!/bin/sh
[ "$1" = -d ] && [ "$2" = /Users/Shared/rdna3-ladder-text.XXXXXX ] || { echo "mktemp: unexpected $*" >> "$STUBLOG"; exit 1; }
d=$STUBDIR/textdir.$$; mkdir -p "$d"; echo "$d"
STUBEND
cat > "$T/bin/kill" <<'EOF'
#!/bin/sh
# a real /bin/kill would be found by the shell's own: the fake instances are forgotten, the line is kept
echo "kill $*" >> "$STUBLOG"
exit 0
EOF
for c in caffeinate say afplay sleep pkill; do printf '#!/bin/sh\necho "%s $*" >> "$STUBLOG"\nexit 0\n' "$c" > "$T/bin/$c"; done
printf '#!/bin/sh\nexit 0\n' > "$T/bin/curl"
# sysctl: only the N59 stale wait exists (the frames stage's A/B); every write is logged
cat > "$T/bin/sysctl" <<'EOF'
#!/bin/sh
f=$STUBDIR/iop_stale_wait_ms; [ -f "$f" ] || echo 12 > "$f"
case "$*" in
    "-n debug.rdna3accel_iop_stale_wait_ms") cat "$f" ;;
    "-w debug.rdna3accel_iop_stale_wait_ms="*) echo "sysctl $*" >> "$STUBLOG"; echo "${2#*=}" > "$f" ;;
    debug) echo "debug.rdna3accel_iop_stale: 0" ;;
    *) exit 1 ;;
esac
exit 0
EOF
cat > "$T/D/bin/rdna3_drminfo" <<'EOF'
#!/bin/sh
echo "drminfo $*" >> "$STUBLOG"
if [ "$1" = --scanout-strip ]; then   # N59: one strip, two frames
    printf 'L canvas 0 206 300\nS 0 canvas 0 100000000000011111111111 3\nS 50 canvas 0 010000000000101111111111 2\nE 5 100 0\n' > "$2"
    echo "scanout strip: 1 strip(s) found"; exit 0
fi
case "$1" in --scanout|--scanout-hires) ;; *) exit 2 ;; esac
dir=$2; mkdir -p "$dir"
echo "pipe 0: LIT otg 0 1920x1080 pitch 1920 fmt 0 sw 0 dcc 0 address 0x1000 in use 0x1000"
echo "  picture 320x180: 45% not black, mean rgb 90 90 90, 5000 bytes changed in 2 s"
echo "1 pipe(s) lit"
printf 'P6\n2 2\n255\n\200\200\200\200\200\200\200\200\200\200\200\200' > "$dir/scanout-pipe0.ppm"
# an app that dies: it is gone by the time of its first look
if [ -f "$STUBDIR/die" ]; then
    for p in $(cat "$STUBDIR/die"); do for f in "$STUBDIR"/pids-*; do [ -f "$f" ] && { grep -vx "$p" "$f" > "$f.n" || true; mv -f "$f.n" "$f"; }; done; done
    rm -f "$STUBDIR/die"
fi
exit 0
EOF
chmod +x "$T/bin/"* "$T/D/bin/rdna3_drminfo"

cat > "$T/h.sh" <<'EOF'
. "$HT/bisect.sh"
. "$HT/ladder.sh"
. "$HT/apps.sh"
prog() { echo "$*" >> "$HT/prog.log"; }
notify() { echo "$*" >> "$HT/notify.log"; }
hdr() { echo "== $*"; }
with_limit() { shift; "$@"; }
restore_sleep() { :; }
as_user() { "$@"; }
kill() {   # -0 asks whether the instance is still there; IGNORE_INT / IGNORE_TERM / IGNORE_KILL: an instance that survives that signal
    if [ "$1" = -0 ]; then cat "$STUBDIR"/pids-* 2>/dev/null | grep -qx "$2"; return $?; fi
    echo "kill $*" >> "$STUBLOG"
    case $1 in -INT) [ -n "${IGNORE_INT:-}" ] && return 0 ;; -TERM) [ -n "${IGNORE_TERM:-}" ] && return 0 ;; -KILL) [ -n "${IGNORE_KILL:-}" ] && return 0 ;; esac
    for a in "$@"; do
        case $a in -*) ;; *) for f in "$STUBDIR"/pids-*; do [ -f "$f" ] && { grep -vx "$a" "$f" > "$f.n" || true; mv -f "$f.n" "$f"; }; done ;; esac
    done
    return 0
}
bisect_console_user() { cat "$STUBDIR/user"; }
STANDALONE=1; HOST=; base=rdna3-report-test; D=$HT/D; TS=$HT/ts; mkdir -p "$TS"
HOME=$HT/home; export HOME
ladder_run 5
echo "rc=$?"
EOF

run() {   # run <name> [env...]: the survey under $SH with the stubs; output in $T/<name>.out, evidence in $T/<name>/ladder.txt
    local name=$1; shift
    rm -rf "${T:?}/${name:?}" "${T:?}/home" "${T:?}/sys" "${T:?}/appsdir" "${T:?}/crash"; mkdir -p "$T/$name" "$T/home" "$T/sys" "$T/crash"
    : > "$T/stub.log"; rm -f "$T/notify.log" "$T/prog.log"
    echo 500 > "$T/sys/pids-Terminal"   # the owner's own Terminal, which started this
    [ -n "${OWNER_MAPS:-}" ] && echo 600 > "$T/sys/pids-Maps"   # and an owner's Maps that is running already
    echo 90 > "$T/sys/pids-WindowServer"; echo tester > "$T/sys/user"
    env HT="$T" STUBLOG="$T/stub.log" STUBDIR="$T/sys" CRASHDIR="$T/crash" PATH="$T/bin:$PATH" LADDER_OUT="$T/$name" LADDER_TEXT="$T/text.command" \
        APPS_DIR="$T/appsdir" APPS_CRASHDIRS="$T/crash" APPS_LAUNCHPAD=Launchpad APPS_SETTLE=0 APPS_TAIL=0 APPS_LAUNCHWAIT=0 APPS_WSWAIT=0 APPS_LOGINGAP=0 \
        LADDER_LAUNCHWAIT=0 LADDER_SETTLE=0 LADDER_TAIL=0 LADDER_QUITWAIT=0 LADDER_AFTERQUIT=0 LADDER_LOOK2GAP=0 BISECT_USER=tester \
        BISECT_CONF="$T/conf" BISECT_ALLOW="$T/allow" BISECT_CONFBAK="$T/conf.bak" BISECT_ALLOWBAK="$T/allow.bak" \
        "$@" ${SH:-sh} "$T/h.sh" > "$T/$name.out" 2>&1 < /dev/null || echo "rc=$?" >> "$T/$name.out"
}
printf 'NVMTL_TRACE_RING=262144\n' > "$T/conf.orig"
printf 'mtlprobe\n%%apps\n' > "$T/allow.orig"
fresh() { cp "$T/conf.orig" "$T/conf"; cp "$T/allow.orig" "$T/allow"; rm -f "$T/conf.bak" "$T/allow.bak"; }
# stages per app, the looks per stage: the names the owner reads
STAGES="chess:main terminal:main textedit:main calculator:main notes:main safari:text safari:canvas safari:webgl safari:frames preview:pdf preview:image maps:main settings:appearance settings:wallpaper settings:displays appstore:main photos:main imagecapture:main quicktime:play quicktime:later mission-control:windows mission-control:mc mission-control:closed launchpad:apps launchpad:closed"

for SH in sh bash dash; do
    command -v "$SH" >/dev/null 2>&1 || continue
    export SH
    # ------------------------------------------------------------ the survey, nothing goes wrong
    fresh
    run ok
    L=$T/ok/ladder.txt
    grep -q '^rc=0$' "$T/ok.out" || fail "$SH: rc: $(tail -5 "$T/ok.out")"
    grep -q '^variant: 5 (apps)$' "$L" || fail "$SH: variant header: $(grep '^variant' "$L")"
    [ "$(grep '^== rung ' "$L" | sed 's/^== rung //' | tr '\n' ' ' | sed 's/ $//')" = "$RUNGS" ] || fail "$SH: the apps ran in a different order: $(grep '^== rung ' "$L" | tr '\n' ' ')"
    [ "$(grep -c '^== end rung ' "$L")" = 15 ] && [ "$(grep -c '^result: ran$' "$L")" = 15 ] || fail "$SH: not 15 apps that ran: $(grep '^result: ' "$L" | sort | uniq -c)"
    grep -q '^== end$' "$L" || fail "$SH: no end line"
    # the default driver: nothing switched on, nothing restarted
    ! grep -v -e '^sysctl -w debug.rdna3accel_iop_stale_wait_ms=' "$T/stub.log" | grep -q -e '--env' -e 'sysctl' || fail "$SH: a switch was set: $(grep -e '--env' -e sysctl "$T/stub.log" | head -3)"
    # N59: the frames stage's two bursts, the kernel's stale wait off for the second and put back
    [ "$(grep -c '^drminfo --scanout-strip ' "$T/stub.log")" = 2 ] && grep -qx 'sysctl -w debug.rdna3accel_iop_stale_wait_ms=0' "$T/stub.log" \
        && grep -qx 'sysctl -w debug.rdna3accel_iop_stale_wait_ms=12' "$T/stub.log" && [ "$(cat "$T/sys/iop_stale_wait_ms")" = 12 ] || fail "$SH: the frame-order bursts: $(grep -e strip -e sysctl "$T/stub.log")"
    grep -q '^# frameorder frames .* iop_stale_wait_ms=12$' "$T/ok/frameorder-frames.log" && grep -q '^# frameorder frames-nowait .* iop_stale_wait_ms=0$' "$T/ok/frameorder-frames-nowait.log" \
        && grep -q '^L canvas 0 206 300$' "$T/ok/frameorder-frames.log" && grep -q '^frameorder: frames: 1 strip(s), 2 change(s), exit 0' "$L" || fail "$SH: the burst files: $(head -3 "$T/ok/frameorder-frames.log")"
    python3 -I -B tools/frame_order.py "$T/ok/frameorder-frames.log" | grep -q '^PASS frame order: frameorder-frames (kernel stale wait 12 ms): 0 regressions in 5 samples (layer: canvas, pipe 0)' || fail "$SH: the burst does not decode"
    grep -qF "open -a Safari $T/appsdir/frameorder.html" "$T/stub.log" && grep -q "steps(240,end)" "$T/appsdir/frameorder.html" || fail "$SH: the frame-order page"
    ! grep -q '^killall' "$T/stub.log" || fail "$SH: WindowServer was restarted"
    cmp -s "$T/conf" "$T/conf.orig" && [ ! -e "$T/conf.bak" ] || fail "$SH: the survey touched rdna3mtl.conf"
    ! grep -q 'osascript\|System Events' "$T/stub.log" || fail "$SH: the survey scripts an app's UI (System Events needs the owner's consent)"
    # what each app is asked to open
    A=$T/appsdir
    for want in "open -n -a Chess" "open -n -a Terminal $T/text.command --args -ApplePersistenceIgnoreState YES" "open -a TextEdit $A/survey.rtf" "open -a Calculator" "open -a Notes" \
        "open -a Safari $A/text.html" "open -a Safari $A/canvas.html" "open -a Safari $A/webgl.html" "open -a Preview $A/survey.pdf" "open -a Preview $A/survey.jpg" "open -a Maps" \
        "open x-apple.systempreferences:com.apple.Appearance-Settings.extension" "open x-apple.systempreferences:com.apple.Wallpaper-Settings.extension" \
        "open x-apple.systempreferences:com.apple.Displays-Settings.extension" 'open -a App Store' "open -a Photos" 'open -a Image Capture' "open -a QuickTime Player $A/movie.mp4" \
        "open -a Mission Control" "open -a Launchpad"; do
        grep -qxF "$want" "$T/stub.log" || fail "$SH: not opened: [$want]"
    done
    [ "$(grep -cxF 'open -a Mission Control' "$T/stub.log")" = 2 ] && [ "$(grep -cxF 'open -a Launchpad' "$T/stub.log")" = 2 ] || fail "$SH: Mission Control / Launchpad are not toggled open and closed"
    [ "$(grep -n 'open -a Calculator' "$T/stub.log" | tail -1 | cut -d: -f1)" -lt "$(grep -nxF 'open -a Mission Control' "$T/stub.log" | head -1 | cut -d: -f1)" ] || fail "$SH: Mission Control opens before its two windows"
    # the content the survey made
    head -c 4 "$A/survey.pdf" | grep -q '%PDF' && grep -q '%%EOF' "$A/survey.pdf" || fail "$SH: survey.pdf is not a PDF"
    [ "$(python3 -I -c "import sys; d=open(sys.argv[1],'rb').read(); print(d[4:8])" "$A/movie.mp4")" = "b'ftyp'" ] || fail "$SH: movie.mp4 is not an MP4"
    head -c 6 "$A/survey.rtf" | grep -q '{\\rtf1' || fail "$SH: survey.rtf is not RTF"
    for h in text canvas webgl; do grep -q '<!doctype html>' "$A/$h.html" || fail "$SH: $h.html"; done
    grep -q 'requestAnimationFrame' "$A/canvas.html" && grep -q "getContext('webgl" "$A/webgl.html" && grep -q 'rotate(360deg)' "$A/text.html" || fail "$SH: the pages do not animate / draw"
    [ -s "$A/survey.jpg" ] || fail "$SH: no JPEG for Preview"
    [ -x "$T/text.command" ] && grep -q 'rdna3 ladder tick' "$T/text.command" || fail "$SH: the Terminal's tick script"
    # looks: a short and a hi-res one per stage, the Terminal's second hi-res look, names the owner can read
    [ "$(grep -c '^drminfo --scanout ' "$T/stub.log")" = 25 ] && [ "$(grep -c '^drminfo --scanout-hires ' "$T/stub.log")" = 26 ] || fail "$SH: looks: $(grep -c '^drminfo --scanout ' "$T/stub.log") short, $(grep -c '^drminfo --scanout-hires ' "$T/stub.log") hi-res"
    for st in $STAGES; do
        app=${st%%:*}; stage=${st#*:}
        if [ "$app" = chess ] || [ "$stage" = main ]; then n=apps-$app; else n=apps-$app-$stage; fi
        [ -f "$T/home/rdna3-scanout/${n}a/scanout-pipe0.ppm" ] && [ -f "$T/home/rdna3-scanout/${n}h/scanout-pipe0.ppm" ] || fail "$SH: no looks $n(a|h)"
        grep -q "^looks:.* ${n}h\( \|\$\)" "$L" || fail "$SH: $n is not in a looks line"
    done
    [ -f "$T/home/rdna3-scanout/apps-terminalh2/scanout-pipe0.ppm" ] && grep -q '^looks: apps-terminala apps-terminalh apps-terminalh2$' "$L" || fail "$SH: the Terminal has no second hi-res look: $(grep -A12 '^== rung terminal' "$L" | grep looks)"
    ! grep -q 'apps-safarih2\|apps-chessh2' "$L" || fail "$SH: a second hi-res look outside the Terminal"
    # a log slice per app: the plugin's lines of its pids only, the sys slice with WindowServer's and the kernel's
    for r in $RUNGS; do
        [ -f "$T/ok/$r.log" ] && [ -f "$T/ok/$r-sys.log" ] || fail "$SH: $r: no log slices"
    done
    grep -q 'NVMTL: census' "$T/ok/chess.log" && ! grep -q 'WindowServer\|kernel' "$T/ok/chess.log" || fail "$SH: chess.log is not the app's plugin lines only"
    grep -q 'WindowServer\[90:1\] .*NVMTL' "$T/ok/chess-sys.log" && grep -q 'kernel\[0:0\] RDNA3Accel' "$T/ok/chess-sys.log" || fail "$SH: chess-sys.log lacks WindowServer's or the kernel's lines"
    grep -q 'processID == 2001' "$T/stub.log" && grep -q 'App\[2002:' "$T/ok/safari.log" || fail "$SH: Safari's helper processes (WebKit) are not in its slice"
    grep -q '^helpers: 2001 2002$' "$L" || fail "$SH: helpers line: $(grep '^helpers' "$L" | sort -u | head -3)"
    grep -q '^pids: Safari=' "$L" && grep -q '^pids: System_Settings=' "$L" || fail "$SH: pids lines: $(grep '^pids' "$L" | head -8 | tr '\n' ';')"
    grep -q '^pids: none= Calculator=[0-9]* TextEdit=[0-9]*$' "$L" || fail "$SH: Mission Control has no process of its own; its two windows are tracked: $(grep -A8 '^== rung mission-control' "$L" | grep pids)"
    grep -q '^plugin: App(' "$L" || fail "$SH: plugin line: $(grep '^plugin' "$L" | head -2)"
    # the quit: SIGINT first, only the pids the survey started; the owner's Terminal (500) stays
    ! grep -q '^kill .*\b500\b' "$T/stub.log" || fail "$SH: the owner's Terminal was killed"
    # 15: Apps / Launchpad has no process of its own (the Dock draws it), like Mission Control
    [ "$(grep -c "^quit: pid [0-9]* ended by SIGINT after " "$L")" = 15 ] || fail "$SH: the quit lines: $(grep -c '^quit: pid' "$L")"
    grep -A8 '^== rung launchpad' "$L" | grep -q '^pids: none=' || fail "$SH: Apps / Launchpad is not a process of its own: $(grep -A8 '^== rung launchpad' "$L" | grep pids)"
    ! grep -q '^kill -TERM\|^kill -KILL' "$T/stub.log" || fail "$SH: an instance that quit on SIGINT was TERMed too"
    [ "$(cat "$T/sys/pids-Terminal")" = 500 ] || fail "$SH: instances left running: $(cat "$T/sys"/pids-* 2>/dev/null | tr '\n' ' ')"
    for f in "$T/sys"/pids-*; do case $f in *pids-Terminal|*pids-WindowServer|*pids-WebKit) ;; *) [ ! -s "$f" ] || fail "$SH: $f still lists $(cat "$f")" ;; esac; done
    [ "$(grep -c '^crash: none$' "$L")" = 15 ] || fail "$SH: crash lines"
    # the content directory and the text script of the run are removed when it is over only when the run made them (the test names its own)
    [ -d "$A" ] || fail "$SH: the test's own content directory was removed"
    # the triage reads it
    python3 -I - "$L" "$T/ok" <<'PYEOF' || fail "$SH: tools/triage_apps.py does not read the survey"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t, triage_apps as a
text = open(sys.argv[1]).read()
logs = {}
for f in os.listdir(sys.argv[2]):
    if f.endswith(".log"):
        logs[f[:-4]] = open(os.path.join(sys.argv[2], f)).read()
v, rungs, probs = t.parse_ladder(text, logs)
assert v == 5 and not probs and list(rungs) == list(a.APPS_RUNGS), (v, probs, list(rungs))
assert rungs["terminal"]["terminal_pid"] and rungs["safari"]["looks"][:2] == ["apps-safari-texta", "apps-safari-texth"], rungs["safari"]["looks"]
gl = {"terminal": {"verdict": "clean", "text": "CLEAN: 20 of 20 tick lines have a twin (100 %)"}}
rows = a.apps_rows(text, rungs, probs, gl, logs)
names = [r[1] for r in rows]
assert names == ["app " + n for n in a.APPS_RUNGS] + ["apps survey"], names
bad = [r for r in rows if r[0] != "INFO"]
assert not bad, bad
d = {r[1]: r[2] for r in rows}
assert d["app safari"].startswith("Safari: ran; stages text canvas webgl frames; pids Safari=") and "plugin census: " in d["app safari"], d["app safari"]
assert "glyphs (automatic" in d["app terminal"] and "CLEAN" in d["app terminal"], d["app terminal"]
assert "15 ran, 0 crashed" in d["apps survey"], d["apps survey"]
PYEOF
    echo "test_apps_step: ok survey ($SH)"

    # ------------------------------------------------------------ what can go wrong, app by app
    fresh
    OWNER_MAPS=1 run bad FAIL_APP=Notes DIE_APP=Preview DIE_CRASH=1 NOPLUGIN_APP=Calculator NOTABLE=1
    L=$T/bad/ladder.txt
    grep -q '^rc=1$' "$T/bad.out" && fail "$SH: the survey stopped" || true
    [ "$(grep -c '^== end rung ' "$L")" = 15 ] || fail "$SH: an app that failed stopped the survey: $(grep -c '^== end rung' "$L") of 15"
    blk() { awk -v r="$1" '$0 == "== rung " r { p = 1 } p { print } $0 == "== end rung " r { p = 0 }' "$L"; }
    blk notes | grep -q '^result: did not open$' && blk notes | grep -q '^WARN: Notes did not start within' || fail "$SH: Notes: $(blk notes | grep -e result -e WARN)"
    [ "$(blk notes | grep -c '^-- look ')" = 2 ] || fail "$SH: an app that did not open still gets one look for the owner: $(blk notes | grep -c '^-- look ')"
    blk preview | grep -q '^died: pid [0-9]* was gone after stage pdf' && blk preview | grep -q '^crash: Preview-2026-10-26-230000.ips (this app)$' && blk preview | grep -q '^result: crashed (report: Preview-2026-10-26-230000.ips)$' || fail "$SH: Preview: $(blk preview | grep -e result -e crash -e died)"
    [ -f "$T/bad/preview-crash.log" ] || fail "$SH: the crash report was not summarised into preview-crash.log"
    blk maps | grep -q "^result: was already running (the owner's instance)$" && blk maps | grep -q 'WARN: Maps was already running (pid 600)' || fail "$SH: Maps: $(blk maps | grep -e result -e WARN)"
    ! grep -q '^kill .* 600\b' "$T/stub.log" || fail "$SH: the owner's Maps was quit"
    [ "$(cat "$T/sys/pids-Maps")" = 600 ] || fail "$SH: the owner's Maps is gone"
    blk calculator | grep -q '^plugin: NOT loaded in any process of the app' || fail "$SH: Calculator without the plugin: $(blk calculator | grep plugin)"
    python3 -I - "$L" "$T/bad" <<'PYEOF' || fail "$SH: the triage rows of a bad survey"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t, triage_apps as a
text = open(sys.argv[1]).read()
logs = {f[:-4]: open(os.path.join(sys.argv[2], f)).read() for f in os.listdir(sys.argv[2]) if f.endswith(".log")}
v, rungs, probs = t.parse_ladder(text, logs)
rows = a.apps_rows(text, rungs, probs, {}, logs)
d = {r[1]: (r[0], r[2]) for r in rows}
assert d["app notes"][0] == "WARN" and "did not open" in d["app notes"][1], d["app notes"]
assert d["app preview"][0] == "FAIL" and "crashed" in d["app preview"][1] and "Preview-2026-10-26-230000.ips" in d["app preview"][1], d["app preview"]
assert d["app maps"][0] == "WARN" and "already running" in d["app maps"][1], d["app maps"]
assert d["app calculator"][0] == "WARN" and "NOT loaded" in d["app calculator"][1], d["app calculator"]
assert d["app chess"][0] == "WARN" and "Metal/NVMTL error: 1 line(s)" in d["app chess"][1] and "VM fault: 1 line(s)" in d["app chess"][1], d["app chess"]
assert "1 crashed" in d["apps survey"][1] and "1 did not open" in d["apps survey"][1], d["apps survey"]
PYEOF
    echo "test_apps_step: ok failures ($SH)"

    # ------------------------------------------------------------ WindowServer restarts under an app
    fresh
    run ws WS_APP=Maps
    L=$T/ws/ladder.txt
    grep -q '^WARN: WindowServer restarted during this app (pid 90 -> 91)' "$L" && grep -q '^result: ran; WindowServer / Dock crashed (report: WindowServer-2026-10-26-230001.ips); WindowServer restarted$' "$L" || fail "$SH: WindowServer restart: $(grep -e 'WARN: Window' -e '^result' "$L" | sort | uniq -c | head)"
    grep -q '^login: tester is back after 0s$' "$L" && [ "$(grep -c '^== end rung ' "$L")" = 15 ] || fail "$SH: the survey did not go on after the desktop came back: $(grep '^login\|^ABORT' "$L")"
    grep -q '^crash: WindowServer-2026-10-26-230001.ips (WindowServer / Dock)$' "$L" || fail "$SH: WindowServer's report: $(grep '^crash' "$L" | head -3)"
    python3 -I - "$L" "$T/ws" <<'PYEOF' || fail "$SH: the triage rows of a WindowServer restart"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t, triage_apps as a
text = open(sys.argv[1]).read()
v, rungs, probs = t.parse_ladder(text, {})
d = {r[1]: (r[0], r[2]) for r in a.apps_rows(text, rungs, probs, {}, {})}
assert d["app maps"][0] == "FAIL" and "WindowServer restarted" in d["app maps"][1] and "WindowServer pid 90 -> 91" in d["app maps"][1], d["app maps"]
PYEOF
    # nobody logs in after it: the survey stops there and says so
    fresh
    run nologin WS_APP=Maps NOLOGIN=1
    L=$T/nologin/ladder.txt
    grep -q '^login: NOBODY logged in 0s after the WindowServer restart' "$L" && grep -q '^ABORT: the survey stops at maps' "$L" || fail "$SH: no login: $(grep '^login\|^ABORT' "$L")"
    [ "$(grep -c '^== rung ' "$L")" = 8 ] || fail "$SH: the survey went on without a desktop: $(grep -c '^== rung ' "$L") rungs"
    grep -q '^rc=1$' "$T/nologin.out" || fail "$SH: rc after no login: $(tail -3 "$T/nologin.out")"
    python3 -I - "$L" <<'PYEOF' || fail "$SH: the triage rows of a stopped survey"
import sys, os
sys.path.insert(0, os.environ["ROOT"] + "/tools")
import triage_glyphs as t, triage_apps as a
text = open(sys.argv[1]).read()
v, rungs, probs = t.parse_ladder(text, {})
rows = a.apps_rows(text, rungs, probs, {}, {})
d = {r[1]: (r[0], r[2]) for r in rows}
assert d["app settings"][0] == "WARN" and "did not run" in d["app settings"][1], d["app settings"]
assert any(r[1] == "apps" and r[2].startswith("ABORT: the survey stops at maps") for r in rows), rows
assert any(r[1] == "apps" and "NOBODY" in r[2] for r in rows), rows
assert "7 missing" in d["apps survey"][1], d["apps survey"]
PYEOF
    echo "test_apps_step: ok WindowServer ($SH)"

    # ------------------------------------------------------------ an app that ignores SIGINT is TERMed, then KILLed; a sips that makes no JPEG is said
    fresh
    run stubborn IGNORE_INT=1 IGNORE_TERM=1 NO_SIPS=1
    L=$T/stubborn/ladder.txt
    grep -q '^quit: pid [0-9]* ended by SIGKILL after ' "$L" && ! grep -q 'SURVIVED' "$L" || fail "$SH: SIGKILL: $(grep '^quit' "$L" | sort | uniq -c | head -4)"
    grep -q '^WARN: apps: sips made no JPEG from the PDF' "$L" || fail "$SH: no sips warning"
    echo "test_apps_step: ok quit / assets ($SH)"
done

# ---- the wiring
grep -q -- '--apps) LADDER=5' "$AR" && grep -q 'rdna3ladder=5 "\*) \[ -n "\$LADDER" \] || LADDER=5' "$AR" || fail "autoreport.sh: --apps / rdna3ladder=5"
grep -q 'ladder_run "\$LADDER"' "$AR" && grep -q 'apps_rung "\$_lr"' "$AR" && grep -q 'apps_assets' "$AR" || fail "autoreport.sh: the survey is not started by ladder_run"
grep -q 'apps-\*/scanout-pipe' "$AR" || fail "autoreport.sh: the survey's looks are not pushed to the host"
st=$(awk '$1 == "apps" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
play=$(awk '$1 == "play" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ "$st" = "${play% rdna3play=1} rdna3ladder=5" ] || fail "ladder-steps.txt: apps is [$st], expected the play step's boot-args + rdna3ladder=5"
[ "$(grep -c 'ladder-coh apps' "$ROOT/tools/vm/rdna3vm")" = 2 ] || fail "rdna3vm: apps is not in both step lists"
sed -n '/^auto_limit() {/,/^}/p' "$ROOT/tools/vm/rdna3vm" > "$T/limit.sh"
ma=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$st")
[ "$ma" -ge 40 ] && [ "$ma" -lt 70 ] || fail "rdna3vm: auto_limit apps $ma (15 apps of 50-130 s need ~25 min and the report: expected 40..69)"
grep -q 'apps_rows' "$ROOT/tools/triage_reports.py" && grep -q 'lv_ == 5' "$ROOT/tools/triage_reports.py" || fail "triage_reports.py: the app survey is not read"
grep -qw 'test_apps_step' "$ROOT/Makefile" && grep -qw 'test_triage_apps' "$ROOT/Makefile" || fail "Makefile: the survey's tests are not in make test"
echo "test_apps_step: ok"
