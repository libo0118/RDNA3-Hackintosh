#!/bin/sh
# Add the Metal plugin's frame traces to a report directory. Run it by hand on the card machine, as root (the trace
# files are 0640 root/_windowserver/<user>:admin; an ordinary admin sees only some), or from tools/macos/collect.sh
# before that packs the archive - collect.sh does NOT call it until a line is added there (and autoreport.sh must
# fetch this file next to collect.sh):
#   sudo sh tools/macos/collect_plugin_trace.sh <report dir>
# Run through sudo, the files it adds are handed back to the invoking user (SUDO_UID) so that they can pack the report.
# Adds
#   <dir>/31-plugin-trace/<process>-<pid>.trace   every process that loaded the plugin wrote one
#                                                (metal/plugin/nvmtl_trace.h); 31-plugin-trace/info.txt lists them
#   <dir>/32-log-nvmtl-windowserver.txt           WindowServer's NVMTL os_log lines of this boot, unfiltered
# Decode on the host with tools/plugin_trace.py <report>.tar.gz (docs/AFTER-TESTS.md "Plugin frame trace").
# Read-only except for the two outputs; never fails the report (always exits 0).
set -u
dir=${1:-}
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
    echo "usage: collect_plugin_trace.sh <report dir>" >&2
    exit 0
fi
out="$dir/31-plugin-trace"
mkdir -p "$out" 2>/dev/null || { echo "collect_plugin_trace: cannot create $out" >&2; exit 0; }
info="$out/info.txt"
: > "$info"

# The trace directory is world writable (like /tmp): any local user can plant a file in it. Only take what is a trace
# of a process we can vouch for, and never anything that could be somebody else's data (the report is shared):
#   - the directory is not a symlink and belongs to root, _windowserver (88) or us;
#   - the file is a regular file (no symlink), with ONE link (no hard link to a root-only file), 4 KiB .. 256 MB,
#     starts with the magic NVMTLTR1;
#   - its owner is root or _windowserver, or the uid its own header names (an app's own trace); a file named
#     WindowServer* must belong to root or _windowserver, so a forged one cannot shadow the real one;
#   - it is checked again after the copy (the copy must still start with the magic), and a name already taken
#     gets a prefix instead of replacing the first.
cap=${NVMTL_COLLECT_MAXBYTES:-268435456}
me=$(id -u)
note() { echo "$*" >> "$info"; }
# The checks above and this copy are two opens of the name, and the directory is world writable: the copy is
# bounded (never more than the cap, whatever the name has become by now), exclusive (noclobber) and watched (a FIFO
# swapped in would block the open; the watchdog ends it), and checked again afterwards.
bounded_copy() {
    ( set -C; exec head -c "$cap" < "$1" > "$2" ) 2>/dev/null &
    cp_pid=$!
    ( sleep 60; kill -9 "$cp_pid" 2>/dev/null ) >/dev/null 2>&1 &
    cp_wd=$!
    wait "$cp_pid"; cp_rc=$?
    kill "$cp_wd" 2>/dev/null
    return "$cp_rc"
}
n=0; rejected=0
for src in ${NVMTL_TRACE_DIR:-} /var/tmp/rdna3-nvmtl; do
    [ -n "$src" ] || continue
    if [ -L "$src" ] || [ ! -d "$src" ]; then
        [ -e "$src" ] || [ -L "$src" ] && note "REJECT dir $src: a symlink or not a directory"
        continue
    fi
    set -- $(ls -ldn "$src" 2>/dev/null)
    downer=${3:-x}
    case "$downer" in 0|88|"$me") ;; *) note "REJECT dir $src: owned by uid $downer"; continue ;; esac
    for f in "$src"/*.trace; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        b=$(basename "$f")
        set -- $(ls -ldn "$f" 2>/dev/null)
        mode=${1:-?}; links=${2:-0}; owner=${3:-x}; size=${5:-0}
        line="$mode $links uid $owner size $size $b"
        why=""
        if [ -L "$f" ]; then why="a symlink"
        elif [ ! -f "$f" ]; then why="not a regular file"
        elif [ "$links" != 1 ]; then why="$links hard links"
        elif [ "$size" -gt "$cap" ] 2>/dev/null; then why="over $cap bytes"
        elif [ "$size" -lt 4096 ] 2>/dev/null; then why="shorter than a trace header"
        else
            magic=""; hu=""
            [ -r "$f" ] && { magic=$(head -c 8 "$f" 2>/dev/null); hu=$(od -An -tu4 -j104 -N4 "$f" 2>/dev/null | tr -d ' '); }
            if [ ! -r "$f" ]; then why="unreadable: run with sudo"
            elif [ "$magic" != NVMTLTR1 ]; then why="no NVMTLTR1 magic"
            else
                case "$b" in
                WindowServer*) case "$owner" in 0|88) ;; *) why="named WindowServer but owned by uid $owner" ;; esac ;;
                *) case "$owner" in 0|88) ;; *) [ "$owner" = "$hu" ] || why="owned by uid $owner, its header says uid ${hu:-?}" ;; esac ;;
                esac
            fi
        fi
        if [ -n "$why" ]; then note "REJECT $line: $why"; rejected=$((rejected + 1)); continue; fi
        dest="$b"; k=0
        while [ -e "$out/$dest" ]; do k=$((k + 1)); dest="dup$k-$b"; done
        if bounded_copy "$f" "$out/$dest" && [ "$(head -c 8 "$out/$dest" 2>/dev/null)" = NVMTLTR1 ]; then
            chmod 644 "$out/$dest" 2>/dev/null
            note "ACCEPT $line -> $dest"; n=$((n + 1))
        else
            rm -f "$out/$dest"
            note "REJECT $line: unreadable, or not what it was when checked"; rejected=$((rejected + 1))
        fi
    done
done

{
    echo "== plugin frame traces collected: $n (rejected: $rejected)"
    date -u '+collected %Y-%m-%d %H:%M:%S UTC'
    echo "== directory (numeric owner and group)"
    ls -ldn /var/tmp/rdna3-nvmtl 2>&1 || true
    echo "== copied files"
    ls -ln "$out" 2>&1 | grep '\.trace' || echo "(none)"
    echo "== processes with the plugin loaded now (a trace of a live process keeps growing)"
    ps -axo pid,user,comm 2>/dev/null | grep -E 'WindowServer|Safari|Finder|Dock' | head -20 || true
    if [ "$n" = 0 ]; then
        echo "== no traces: the plugin is older than the frame trace, NVMTL_TRACE=0 is set (environment or"
        echo "   /Library/GPUBundles/rdna3mtl/rdna3mtl.conf), or no process used the plugin since the directory was last cleaned."
        echo "   The plugin's own line says which: grep 'trace:' in 26-log-metal-plugin.txt / 32-log-nvmtl-windowserver.txt."
        echo "   Files listed REJECT ... unreadable: run with sudo (they are 0640, group admin; root reads them all)"
    fi
} >> "$info" 2>&1

# WindowServer's NVMTL lines whole: the text log is rate limited, the traces above are the record, but the log
# carries the plugin's own explanations (which route each surface took, the kernel's refusals)
lim=240
{
    echo "\$ log show --last boot --info --style compact --predicate 'process == \"WindowServer\" AND eventMessage CONTAINS \"NVMTL\"'"
    log show --last boot --info --style compact --predicate 'process == "WindowServer" AND eventMessage CONTAINS "NVMTL"' &
    p=$!
    ( sleep "$lim"; kill -9 "$p" 2>/dev/null ) &
    w=$!
    wait "$p"; r=$?
    kill "$w" 2>/dev/null
    echo "[exit $r]"
} > "$dir/32-log-nvmtl-windowserver.txt" 2>&1
# run through sudo: the files belong to root now; give them to the user who will pack the report
if [ "$(id -u)" = 0 ] && [ -n "${SUDO_UID:-}" ]; then
    chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$out" "$dir/32-log-nvmtl-windowserver.txt" 2>/dev/null
fi
exit 0
