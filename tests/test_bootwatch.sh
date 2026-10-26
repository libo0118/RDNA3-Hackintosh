#!/bin/bash
# The boot watchdog (tools/vm/macos/autoreport.sh, BOOTWATCH block) on the host, with stubs for ioreg, curl, sample, spindump, log, ps,
# nvram and the rest of macOS, run under bash and (when present) dash (macOS's /bin/sh is bash 3.2 in POSIX mode):
#  - WindowServer never gets a GPU client: a snapshot at the trigger and again after BW_REPEAT, uploaded to the host under bootwatch-<stamp>-<n>.txt
#    with the header, ps, ioreg, plugin conf, the sample and the log show; the END line says gave-up after BW_MAXN snapshots; nothing is left pending
#  - WindowServer gets its client later: the END says gpu (also: a user logs in -> login); before the trigger nothing is written at all
#  - no accelerator in the ioreg (a boot below its level): no snapshot; the watch ends at BW_MAXSEC
#  - the host does not answer: the files stay in the pending directory (the next boot uploads them)
#  - no sample: spindump; neither: a line says so; no log / kextstat: lines say so, the file is complete (BOOTWATCH-DONE)
#  - rdna3bootwatch=0 starts nothing, a second start does not start a second watchdog, the start returns at once (never blocks the boot path)
#  - the wiring in autoreport.sh (before the 90 s wait, not in install / approve / standalone boots); tools/triage_bootwatch.py reads what the block wrote
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "${T:?}"' EXIT
fail() { echo "test_bootwatch: FAIL $*"; exit 1; }
AR=$ROOT/tools/vm/macos/autoreport.sh

bash -n "$AR" || fail "bash -n autoreport.sh"
if command -v dash >/dev/null 2>&1; then dash -n "$AR" || fail "dash -n autoreport.sh"; fi
sed -n '/^# BOOTWATCH-BEGIN/,/^# BOOTWATCH-END/p' "$AR" > "$T/block.sh"
[ "$(wc -l < "$T/block.sh")" -gt 80 ] || fail "the BOOTWATCH block was not found in autoreport.sh"

# ---- the stubs
mkdir -p "$T/bin"
cat > "$T/bin/ioreg" <<'EOF2'
#!/bin/sh
# -r -c CLASS [-d 1] [-l -w0]: the accelerator exists unless $S/noacc; WindowServer's user client appears when $S/wsgpu exists
echo "ioreg $*" >> "$S/calls"
[ -e "$S/stuckioreg" ] && exec sleep 600   # a hung boot: ioreg never answers (card run 2026-10-26, -cpu host)
cls=; while [ $# -gt 0 ]; do [ "$1" = -c ] && cls=$2; shift; done
[ -e "$S/noacc" ] && exit 0
case "$cls" in
    RDNA3Accelerator|RDNA3Accel)
        echo "+-o $cls  <class $cls, id 0x1000, registered>"
        echo "    | \"IOGLBundleName\" = \"rdna3mtl\""
        echo "    | \"RDNA3,Something\" = 1"
        [ -e "$S/wsgpu" ] && echo "    +-o IOAccelContext  <class X>  | \"IOUserClientCreator\" = \"pid 412, WindowServer\""
        echo "    +-o IOAccelContext  <class X>  | \"IOUserClientCreator\" = \"pid 99, mtlprobe\"" ;;
esac
exit 0
EOF2
cat > "$T/bin/curl" <<'EOF2'
#!/bin/sh
# -T file URL: stored under $S/host/NAME unless $S/nohost
echo "curl $*" >> "$S/calls"
[ -e "$S/nohost" ] && exit 22
f=; url=; while [ $# -gt 0 ]; do case "$1" in -T) f=$2; shift ;; http*) url=$1 ;; esac; shift; done
mkdir -p "$S/host"; cp "$f" "$S/host/$(basename "$url")"
exit 0
EOF2
cat > "$T/bin/sample" <<'EOF2'
#!/bin/sh
# sample PID 5 -file F
echo "sample $*" >> "$S/calls"
[ -e "$S/nosample" ] && exit 1
f=; while [ $# -gt 0 ]; do [ "$1" = -file ] && f=$2; shift; done
cat > "$f" <<'EOF3'
Sampling process 412 for 5 seconds with 1 millisecond of run time between samples
Call graph:
    4980 Thread_1001   DispatchQueue_1: com.apple.main-thread  (serial)
    + 4980 start  (in dyld) + 6400  [0x7ff8100a141f]
    +   4980 IOServiceOpen  (in IOKit) + 187  [0x7ff80e333333]
    +     4980 mach_msg2_trap  (in libsystem_kernel.dylib) + 10  [0x7ff8100b4f2a]

EOF3
EOF2
cat > "$T/bin/spindump" <<'EOF2'
#!/bin/sh
echo "spindump $*" >> "$S/calls"
f=; while [ $# -gt 0 ]; do [ "$1" = -o ] && f=$2; shift; done
printf 'Heaviest stack for the main thread of the target process:\n  4 start + 1 (dyld + 5) [0x1]\n  4 mach_msg2_trap + 10 (libsystem_kernel.dylib + 1) [0x4]\n\n' > "$f"
EOF2
cat > "$T/bin/log" <<'EOF2'
#!/bin/sh
echo "log $*" >> "$S/calls"
echo "2026-10-26 12:00:01.000 Df WindowServer[412:1a] [com.apple.SkyLight:default] waiting for the accelerator"
EOF2
cat > "$T/bin/ps" <<'EOF2'
#!/bin/sh
echo "  PID  PPID STAT  %CPU     ELAPSED WCHAN COMMAND"
echo "  412     1 Ss      0.0       01:20 -     /System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer"
echo "  300     1 Ss      0.0       01:50 -     /System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow console"
EOF2
cat > "$T/bin/pgrep" <<'EOF2'
#!/bin/sh
[ -e "$S/nows" ] && exit 1
echo 412
EOF2
cat > "$T/bin/nvram" <<'EOF2'
#!/bin/sh
printf 'boot-args\t%s\n' "$(cat "$S/bootargs" 2>/dev/null || echo "rdna3accel=1")"
EOF2
cat > "$T/bin/sysctl" <<'EOF2'
#!/bin/sh
echo "{ sec = $(( $(date +%s) - 100 )), usec = 0 } Mon Oct 26 12:00:00 2026"
EOF2
printf '#!/bin/sh\necho "com.apple.WindowServer 412"\n' > "$T/bin/launchctl"
printf '#!/bin/sh\necho "RDNA3: accelerator up"\n' > "$T/bin/dmesg"
printf '#!/bin/sh\necho "  120    0 0xffffff7f9 io.github.voten641.RDNA3Accel (1.0)"\n' > "$T/bin/kextstat"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/sync"
chmod +x "$T"/bin/*
# the same without spindump, kextstat and log (a Mac that lacks them)
mkdir -p "$T/bin-bare"
for f in "$T"/bin/*; do case "$(basename "$f")" in spindump|kextstat|log) ;; *) cp "$f" "$T/bin-bare/" ;; esac; done
# and a system PATH without them: macOS has all three in /usr/bin and /usr/sbin (CI run 37)
mkdir -p "$T/sys-bare"
for d in /bin /usr/bin /sbin /usr/sbin; do
    for f in "$d"/*; do
        b=$(basename "$f")
        case "$b" in spindump|kextstat|log) continue ;; esac
        [ -e "$T/sys-bare/$b" ] || [ -L "$T/sys-bare/$b" ] || ln -s "$f" "$T/sys-bare/$b"
    done
done

# ---- the scenarios: one script, run by every shell
cat > "$T/scen.sh" <<'EOF2'
S=$1; BIN=$2; BLK=$3
export S PATH="$BIN:${SCEN_SYS:-$PATH}"
. "$BLK"
HOST=http://10.0.2.2:8000
BW_PEND=$S/pend/rdna3-pending-
BW_PIDFILE=$S/bw.pid
BW_LOG=$S/bw.log
mkdir -p "$S/pend"
# the console user: the file $S/user, root when there is none
bw_console_user() { cat "$S/user" 2>/dev/null || echo root; }
BW_POLL=1; BW_REPEAT=1; BW_MAXN=2; BW_MAXSEC=40; BW_N=1
eval "$SCEN"
EOF2

# timeout(1) is GNU coreutils: macOS has none, perl's alarm does the same there
tmo() { if command -v timeout >/dev/null 2>&1; then timeout "$@"; else perl -e 'alarm shift; exec @ARGV' "$@"; fi; }

run_scen() {   # run_scen <shell> <name> <scenario body> [bin dir] [system PATH]: a fresh state directory
    local sh=$1 name=$2 body=$3 bin=${4:-$T/bin}
    S=$T/$sh-$name; mkdir -p "$S"; : > "$S/calls"
    SCEN=$body SCEN_SYS=${5:-$PATH} tmo 120 "$sh" "$T/scen.sh" "$S" "$bin" "$T/block.sh" > "$S/out.txt" 2>&1 ||
        fail "$sh $name: the scenario failed: $(tail -5 "$S/out.txt")"
}

for sh in bash $(command -v dash >/dev/null 2>&1 && echo dash); do
    # 1. never gets a GPU client: two snapshots and the END (gave-up), uploaded, nothing pending
    run_scen $sh never 'bootwatch_run'
    S=$T/$sh-never; H=$S/host
    for n in 1 2; do
        f=$(ls "$H"/bootwatch-*-$n.txt 2>/dev/null | head -n1) || true
        [ -n "$f" ] || fail "$sh never: no bootwatch-<stamp>-$n.txt on the host: $(ls "$H" 2>&1)"
        grep -q "^BOOTWATCH: n=$n trigger_s=1 after_s=[0-9]* uptime_s=[0-9]* console_user=root stamp=" "$f" || fail "$sh never: header of $f: $(head -2 "$f")"
        grep -q "^== ps" "$f" && grep -q 'SkyLight.framework/Resources/WindowServer' "$f" && grep -q 'loginwindow' "$f" || fail "$sh never: ps in $f"
        grep -q 'IOUserClientCreator.*mtlprobe' "$f" && grep -q '^-- RDNA3Accel$' "$f" || fail "$sh never: ioreg essentials in $f"
        grep -q '^== plugin conf' "$f" && grep -q '^-- allow list' "$f" && grep -q '^== kextstat' "$f" && grep -q '^== dmesg' "$f" ||
            fail "$sh never: conf / kextstat / dmesg sections in $f"
        grep -q '^== sample WindowServer' "$f" && grep -q 'mach_msg2_trap' "$f" || fail "$sh never: the sample in $f"
        grep -q '^== log show --last 3m' "$f" && grep -q 'waiting for the accelerator' "$f" && grep -q "^BOOTWATCH-DONE: n=$n" "$f" ||
            fail "$sh never: the log show / DONE in $f"
    done
    end=$(ls "$H"/bootwatch-*-3.txt 2>/dev/null | head -n1) || true
    [ -n "$end" ] && grep -q '^BOOTWATCH-END: state=gave-up after_s=[0-9]* .*snapshots=2 trigger_s=1' "$end" ||
        fail "$sh never: no gave-up END: $(cat "${end:-/dev/null}" 2>&1)"
    [ -z "$(ls "$S/pend")" ] || fail "$sh never: files left pending after a good upload: $(ls "$S/pend")"
    grep -q 'sample 412 5 -file' "$S/calls" || fail "$sh never: sample not run on WindowServer's pid"
    grep -q 'log show --last 3m --style compact --predicate process == "WindowServer" OR process == "loginwindow"' "$S/calls" ||
        fail "$sh never: log show predicate: $(grep '^log' "$S/calls" | head -1)"
    grep -qE 'NVMTL' "$S/calls" || fail "$sh never: the log predicate lacks NVMTL"
    # the triage reads exactly these files
    (cd "$ROOT" && python3 -I -B -c '
import sys
sys.path.insert(0, "tools")
import triage_bootwatch as tb
r = tb.bootwatch_rows(sys.argv[1])
assert len(r) == 1 and r[0][0] == "FAIL" and "did not reach the GPU within 1 s" in r[0][2] and "2 snapshot(s)" in r[0][2] \
    and "mach_msg2_trap" in r[0][2], r
' "$H") || fail "$sh never: triage_bootwatch.py on the uploaded files"

    # 2. WindowServer gets its client after the first snapshot: END state=gpu
    run_scen $sh late 'BW_MAXN=9; ( sleep 3; touch "$S/wsgpu" ) & bootwatch_run; wait'
    S=$T/$sh-late; H=$S/host
    ls "$H"/bootwatch-*-1.txt >/dev/null 2>&1 || fail "$sh late: no snapshot"
    grep -l '^BOOTWATCH-END: state=gpu' "$H"/bootwatch-*.txt >/dev/null 2>&1 || fail "$sh late: no END state=gpu: $(ls "$H")"
    # 3. a user logs in: END state=login
    run_scen $sh login 'BW_MAXN=9; ( sleep 3; echo shrek > "$S/user" ) & bootwatch_run; wait'
    S=$T/$sh-login; H=$S/host
    grep -q '^BOOTWATCH-END: state=login' "$H"/bootwatch-*.txt || fail "$sh login: no END state=login: $(ls "$H")"
    # 4. WindowServer is up before the trigger: nothing is written at all, nothing uploaded
    run_scen $sh early 'BW_N=30; touch "$S/wsgpu"; bootwatch_run'
    S=$T/$sh-early
    [ ! -e "$S/host" ] && [ -z "$(ls "$S/pend")" ] && ! grep -q '^sample' "$S/calls" || fail "$sh early: wrote or uploaded something"
    # 5. no accelerator: no snapshot, the watch ends at BW_MAXSEC
    run_scen $sh noacc 'touch "$S/noacc"; BW_MAXSEC=3; bootwatch_run'
    S=$T/$sh-noacc
    [ ! -e "$S/host" ] && [ -z "$(ls "$S/pend")" ] && ! grep -q '^sample' "$S/calls" || fail "$sh noacc: collected without an accelerator"
    # 6. the host does not answer: the files stay pending under their names, and the run still ends
    run_scen $sh nohost 'touch "$S/nohost"; BW_MAXN=1; bootwatch_run'
    S=$T/$sh-nohost
    ls "$S/pend"/rdna3-pending-bootwatch-*-1.txt >/dev/null 2>&1 || fail "$sh nohost: no pending file: $(ls "$S/pend")"
    grep -q 'upload of bootwatch-.*-1.txt failed: kept for the next boot' "$S/out.txt" || fail "$sh nohost: no log line about the failed upload: $(cat "$S/out.txt")"
    # 7. no sample: spindump; neither spindump, kextstat nor log: lines say so, the file is complete
    run_scen $sh nosample 'touch "$S/nosample"; BW_MAXN=1; bootwatch_run'
    S=$T/$sh-nosample
    grep -q '^== spindump WindowServer' "$S"/host/bootwatch-*-1.txt && grep -q 'mach_msg2_trap' "$S"/host/bootwatch-*-1.txt || fail "$sh nosample: no spindump fallback"
    run_scen $sh bare 'touch "$S/nosample"; BW_MAXN=1; bootwatch_run' "$T/bin-bare" "$T/sys-bare"
    S=$T/$sh-bare
    f=$(ls "$S"/host/bootwatch-*-1.txt | head -n1)
    grep -q 'no sample output and no spindump' "$f" && grep -q '^kextstat: not installed' "$f" && grep -q '^log: not installed' "$f" && grep -q '^BOOTWATCH-DONE' "$f" ||
        fail "$sh bare: missing tools were not reported: $(head -40 "$f")"
    # ioreg hangs (the IOKit of a hung boot): the probe gives up, the watch still collects, spindump first, no more ioreg
    run_scen $sh stuck 'touch "$S/stuckioreg"; BW_PROBE_T=1; BW_N=2; BW_MAXN=1; bootwatch_run'
    S=$T/$sh-stuck; H=$S/host
    f=$(ls "$H"/bootwatch-*-1.txt 2>/dev/null | head -n1)
    [ -n "$f" ] || fail "$sh stuck: no snapshot although ioreg hung: $(ls "$H" 2>&1) $(cat "$S/out.txt")"
    grep -q '^probe: an ioreg of this watch hung' "$f" && grep -q 'skipped (a probe hung)' "$f" && grep -q '^BOOTWATCH-DONE' "$f" ||
        fail "$sh stuck: the hung probe is not reported: $(head -30 "$f")"
    ls "$H"/bootwatch-*-1.spindump >/dev/null 2>&1 || fail "$sh stuck: no spindump on the host: $(ls "$H")"
    [ "$(grep -c '^ioreg' "$S/calls")" -le 2 ] || fail "$sh stuck: ioreg started again after it hung: $(grep -c '^ioreg' "$S/calls") calls"
    # no WindowServer process at all: said so, no sample is attempted
    run_scen $sh nows 'touch "$S/nows"; BW_MAXN=1; bootwatch_run'
    S=$T/$sh-nows
    grep -q '^WindowServer: NOT RUNNING' "$S"/host/bootwatch-*-1.txt && ! grep -q '^sample' "$S/calls" || fail "$sh nows: a missing WindowServer was not reported"
    # 8. the start: off with rdna3bootwatch=0; returns at once with the watchdog behind it; a second start starts none
    run_scen $sh off 'echo "rdna3accel=1 rdna3bootwatch=0" > "$S/bootargs"; bootwatch_start; [ -e "$BW_PIDFILE" ] && echo started || echo not-started'
    grep -q '^not-started$' "$T/$sh-off/out.txt" || fail "$sh off: rdna3bootwatch=0 still started a watchdog: $(cat "$T/$sh-off/out.txt")"
    run_scen $sh start 'echo "rdna3accel=1 rdna3bootwatch=7" > "$S/bootargs"; touch "$S/wsgpu"
        t0=$(date +%s); bootwatch_start; t1=$(date +%s); echo "took $((t1 - t0))"; echo "n=$BW_N"
        p1=$(cat "$BW_PIDFILE"); bootwatch_start; echo "same=$([ "$p1" = "$(cat "$BW_PIDFILE")" ] && echo yes)"; sleep 2'
    S=$T/$sh-start
    grep -q '^took [01]$' "$S/out.txt" && grep -q '^n=7$' "$S/out.txt" && grep -q '^same=yes$' "$S/out.txt" || fail "$sh start: $(cat "$S/out.txt")"
    grep -q 'bootwatch .*: waiting for WindowServer to open the GPU (trigger 7 s)' "$S/bw.log" || fail "$sh start: the watchdog did not run / log: $(cat "$S/bw.log" 2>&1)"
done

# ---- the wiring in autoreport.sh
grep -q '^\[ -z "\$STANDALONE" \] && \[ -z "\$BISECT_HAND" \] && \[ -z "\$INSTALL_ONLY" \] && \[ -z "\$APPROVE" \] && \[ -n "\$HOST" \] && bootwatch_start$' "$AR" ||
    fail "the bootwatch_start call (not standalone / by hand / install / approve boots) is missing"
a=$(grep -n '&& bootwatch_start$' "$AR" | cut -d: -f1); b=$(grep -n 'sleep \$((90 - \${w:-0}))' "$AR" | head -n1 | cut -d: -f1)
[ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] || fail "bootwatch_start must come before the 90 s wait (line $a vs $b)"
echo "test_bootwatch: ok"
