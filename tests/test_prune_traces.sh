#!/bin/bash
# autoreport.sh's prune_old_traces (it deletes the plugin traces of earlier boots from /var/tmp/rdna3-nvmtl at the
# start of a run) on the host, with a stub `sysctl` (kern.boottime in macOS's real format, usec != 0) and a stub
# `stat` (BSD `-f %u dir` / `-f %m file`: GNU stat's -f is the file system), the directory taken from
# RDNA3_TRACE_DIR:
#  - a trace whose mtime is before boot - 120 s is deleted; one at the boot, 10 s after it, 100 s or 120 s before
#    it is kept (the clock set after the boot must not make this boot's traces "old")
#  - the microseconds are not the boot time (a greedy parse took them: nothing was ever deleted); usec 0 and
#    999999 give the same result
#  - kern.boottime unparseable ("garbage", empty, a line without "sec = " at its start): "boot time unknown: not
#    pruned" and nothing deleted
#  - a symlink and a trace with two hard links are kept, whatever their mtime; a directory that is a symlink or
#    belongs to somebody else is left alone
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_prune_traces: FAIL $*"; [ -f "$T/out" ] && sed 's/^/  | /' "$T/out"; exit 1; }
AR="$ROOT/tools/vm/macos/autoreport.sh"

sed -n '/^prune_old_traces() {/,/^}/p' "$AR" > "$T/fn.sh"
[ "$(wc -l < "$T/fn.sh")" -gt 12 ] || fail "prune_old_traces not found in autoreport.sh"
grep -q '^    prune_old_traces$' "$AR" || fail "prune_old_traces is not called at the start of a run"
grep -q 'RDNA3_TRACE_DIR:-/var/tmp/rdna3-nvmtl' "$AR" || fail "the trace directory is not overridable (default /var/tmp/rdna3-nvmtl)"

mkdir -p "$T/bin"
cat > "$T/bin/sysctl" <<'STUB'
#!/bin/sh
[ "$2" = kern.boottime ] || exit 1
case "$BT_STUB" in
real) echo "{ sec = $BOOT, usec = $USEC } Sat Oct 24 10:00:00 2026" ;;
*) echo "$BT_STUB" ;;
esac
STUB
cat > "$T/bin/stat" <<'STUB'
#!/bin/sh
# BSD stat as the function calls it: stat -f %u <dir>, stat -f %m <file> (the mtimes from $MTIMES: "<name> <sec>")
[ "$1" = -f ] || exit 1
case "$2" in
%u) echo "${DIR_UID:-0}" ;;
%m) v=$(sed -n "s/^$(basename "$3") //p" "$MTIMES"); [ -n "$v" ] || exit 1; echo "$v" ;;
*) exit 1 ;;
esac
STUB
chmod +x "$T/bin/sysctl" "$T/bin/stat"

BOOT=1792310400
# prune <dir> <bt_stub> <usec> [dir uid]: the function's output into $T/out
prune() {
    ( export PATH="$T/bin:$PATH" RDNA3_TRACE_DIR="$1" BT_STUB="$2" USEC="$3" BOOT=$BOOT MTIMES="$T/mtimes" DIR_UID="${4:-0}"
      set +e; . "$T/fn.sh"; prune_old_traces; echo "[exit $?]" ) > "$T/out" 2>&1
}
mktraces() {   # mktraces <dir>: the same set every time; the mtimes in $T/mtimes
    rm -rf "$1"; mkdir -p "$1"
    : > "$T/mtimes"
    local n m
    for spec in "ancient:-5000" "old121:-121" "edge120:-120" "recent100:-100" "boot0:0" "after10:10"; do
        n=${spec%%:*}; m=${spec#*:}
        head -c 5000 /dev/zero > "$1/$n.trace"
        echo "$n.trace $((BOOT + m))" >> "$T/mtimes"
    done
    # a symlink and a hard-linked trace are never touched, however old
    head -c 5000 /dev/zero > "$1/target.tmp"
    ln -s "$1/target.tmp" "$1/symlink.trace"
    echo "symlink.trace $((BOOT - 9000))" >> "$T/mtimes"
    head -c 5000 /dev/zero > "$1/hard-a.trace"; ln "$1/hard-a.trace" "$1/hard-b.trace"
    echo "hard-a.trace $((BOOT - 9000))" >> "$T/mtimes"; echo "hard-b.trace $((BOOT - 9000))" >> "$T/mtimes"
}
exists() { [ -e "$1/$2" ] || [ -L "$1/$2" ]; }
check_pruned() {   # check_pruned <dir> <label>
    for n in ancient old121; do exists "$1" "$n.trace" && fail "$2: $n.trace (older than the boot - 120 s) was kept"; done
    for n in edge120 recent100 boot0 after10 symlink hard-a hard-b; do
        exists "$1" "$n.trace" || fail "$2: $n.trace was deleted"
    done
    exists "$1" target.tmp || fail "$2: the symlink's target was deleted"
}

# the real format, usec 123456 (the case a greedy parse got wrong), 0 and 999999: the same result
for usec in 123456 0 999999; do
    mktraces "$T/d$usec"
    prune "$T/d$usec" real "$usec"
    grep -qF "== plugin traces: 2 trace(s) of earlier boots deleted (0 MB)" "$T/out" || fail "usec $usec: summary line"
    grep -qx '\[exit 0\]' "$T/out" || fail "usec $usec: did not return 0"
    check_pruned "$T/d$usec" "usec $usec"
done

# boot time not parseable: said, nothing deleted
for g in "garbage" "" "{ usec = 5 }" "{ sec = , usec = 5 }" "x { sec = 100, usec = 5 }"; do
    mktraces "$T/g"
    prune "$T/g" "$g" 0
    grep -qF "== plugin traces: boot time unknown: not pruned" "$T/out" || fail "kern.boottime '$g': not said"
    for n in ancient old121 edge120 recent100 boot0 after10; do exists "$T/g" "$n.trace" || fail "kern.boottime '$g': $n.trace deleted"; done
done

# a directory of somebody else's, a symlinked directory, a missing one: left alone
mktraces "$T/o"
prune "$T/o" real 5 1234
grep -qF "is not root's or _windowserver's: not pruned" "$T/out" || fail "a foreign directory: not said"
exists "$T/o" ancient.trace || fail "a foreign directory was pruned"
ln -s "$T/o" "$T/link"
prune "$T/link" real 5
exists "$T/o" ancient.trace || fail "a symlinked directory was pruned"
grep -qx '\[exit 0\]' "$T/out" || fail "a symlinked directory: did not return 0"
prune "$T/missing" real 5
grep -qx '\[exit 0\]' "$T/out" || fail "a missing directory: did not return 0"

# a trace whose mtime cannot be read (stat fails) is kept
mktraces "$T/s"
grep -v '^ancient' "$T/mtimes" > "$T/mtimes.new"; mv "$T/mtimes.new" "$T/mtimes"
prune "$T/s" real 5
exists "$T/s" ancient.trace || fail "a trace whose mtime cannot be read was deleted"
echo "test_prune_traces: ok"
