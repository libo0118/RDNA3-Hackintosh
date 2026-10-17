#!/bin/sh
# Add the Metal plugin's frame traces to a report directory. Run it by hand on the card machine, or from
# tools/macos/collect.sh before that packs the archive - collect.sh does NOT call it until a line is added there
# (and autoreport.sh must fetch this file next to collect.sh):
#   tools/macos/collect_plugin_trace.sh <report dir>
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

# the plugin tries /var/tmp first, /private/tmp when that fails; NVMTL_TRACE_DIR is the plugin's own override (tests)
n=0
for src in ${NVMTL_TRACE_DIR:-} /var/tmp/rdna3-nvmtl /private/tmp/rdna3-nvmtl; do
    [ -d "$src" ] || continue
    for f in "$src"/*.trace; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        [ -e "$out/$b" ] && continue
        if cp "$f" "$out/$b" 2>/dev/null; then n=$((n + 1)); fi
    done
done

{
    echo "== plugin frame traces collected: $n"
    date -u '+collected %Y-%m-%d %H:%M:%S UTC'
    echo "== directories"
    ls -ld /var/tmp/rdna3-nvmtl /private/tmp/rdna3-nvmtl 2>&1 || true
    echo "== files (size, modification time)"
    ls -l "$out" 2>&1 | grep '\.trace' || echo "(none)"
    echo "== processes with the plugin loaded now (a trace of a live process keeps growing)"
    ps -axo pid,user,comm 2>/dev/null | grep -E 'WindowServer|Safari|Finder|Dock' | head -20 || true
    if [ "$n" = 0 ]; then
        echo "== no traces: the plugin is older than the frame trace, NVMTL_TRACE=0 is set (environment or"
        echo "   /Library/GPUBundles/rdna3mtl/rdna3mtl.conf), or no process used the plugin since the directory was last cleaned."
        echo "   The plugin's own line says which: grep 'trace:' in 26-log-metal-plugin.txt / 32-log-nvmtl-windowserver.txt"
    fi
} > "$out/info.txt" 2>&1

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
exit 0
