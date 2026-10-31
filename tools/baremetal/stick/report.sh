#!/bin/sh
# Collect the driver report onto the stick, inside macOS:
#
#   sudo sh /Volumes/OPENCORE/rdna3/report.sh
#
# It writes /Volumes/OPENCORE/rdna3/report-<date>/ (the stick is where Linux reads it; if the
# stick is not under /Volumes, `sudo diskutil mount diskNsM` mounts its EFI partition).
# autoreport.sh --standalone is NOT run: its GPU probes crashed WindowServer on this Mac
# (2026-10-31, layer_blit_byte_ANY). `sudo RDNA3_FULL=1 sh report.sh` runs it anyway.
# Messages to the owner are Polish. RDNA3_REPORT_DIR / RDNA3_SYSROOT: for tests.
[ "$(id -u)" = 0 ] || { echo "uruchom z sudo: sudo sh $0"; exit 1; }
R=${RDNA3_SYSROOT:-}
S=$(cd "$(dirname "$0")" && pwd)
# next to this script when it runs from the stick (<stick>/rdna3, EFI/ beside it), else the usual mount
if [ -d "${S%/rdna3}/EFI" ]; then base=$S; else base=/Volumes/OPENCORE/rdna3; fi
o=${RDNA3_REPORT_DIR:-$base/report-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$o" || { echo "nie moge utworzyc $o (pendrive zamontowany? sudo diskutil mount <dysk>)"; exit 1; }
echo "[1/4] logi jadra"
# the last 3 hours: also the previous (slow) boot, when this one runs with -rdna3off
log show --last 3h --predicate 'eventMessage CONTAINS "RDNA3" OR sender CONTAINS "RDNA3" OR process == "WindowServer"' > "$o/rdna3-3h.log" 2>&1
log show --last 3h --predicate 'process == "kernel"' --style compact > "$o/kernel-3h.log" 2>&1
log show --last boot --style compact > "$o/boot.log" 2>&1
ls -la "$R/Library/Logs/DiagnosticReports" > "$o/diag-list.txt" 2>&1
cp "$R"/Library/Logs/DiagnosticReports/*WindowServer* "$R"/Library/Logs/DiagnosticReports/*.panic "$o/" 2>/dev/null
echo "[2/4] ioreg i kexty"
ioreg -lw0 > "$o/ioreg.txt" 2>&1
kmutil showloaded > "$o/kexts.txt" 2>&1
echo "[3/4] ekrany i ustawienia"
system_profiler SPDisplaysDataType > "$o/displays.txt" 2>&1
sysctl -a 2>/dev/null | grep -i rdna3 > "$o/sysctl.txt" || echo "(no rdna3 sysctls)" > "$o/sysctl.txt"
nvram boot-args > "$o/bootargs.txt" 2>&1
echo "[4/4] raporty awarii"
A="$R/Library/Application Support/RDNA3/autoreport.sh"
if [ "${RDNA3_FULL:-0}" = 1 ]; then
    if [ -f "$A" ]; then sh "$A" --standalone > "$o/autoreport.out" 2>&1; else echo "UWAGA: brak autoreport.sh"; fi
fi
cp -R "$R/Users/Shared/rdna3-reports" "$o/" 2>/dev/null
# DRIVER-PLAN N68: the kernel's frame trace (its IOP events) and the plugin's per-process traces, read together by
#   python3 tools/plugin_trace.py <report>/31-plugin-trace --kernel <report>/trace-stream.bin
# the newest events also into the kernel log (trace_dump) before dmesg below takes it
if sysctl -n debug.rdna3accel_swap_mode >/dev/null 2>&1; then
    sysctl -b debug.rdna3accel_trace_stream > "$o/trace-stream.bin" 2>"$o/trace.err"
    sysctl -b debug.rdna3accel_trace_sec > "$o/trace-sec.bin" 2>>"$o/trace.err"
    [ "$(head -c 4 "$o/trace-stream.bin" 2>/dev/null)" = R3TR ] ||
        echo "trace-stream.bin is NOT a binary trace ($(wc -c < "$o/trace-stream.bin") bytes): sysctl -b failed" >> "$o/trace.err"
    sysctl -w debug.rdna3accel_trace_dump=2000 >/dev/null 2>>"$o/trace.err"
else
    echo "(no debug.rdna3accel_trace_stream: an accelerator without the frame trace, or none)" > "$o/trace.err"
fi
mkdir -p "$o/31-plugin-trace"
cp "$R"/var/tmp/rdna3-nvmtl/*.trace "$o/31-plugin-trace/" 2>/dev/null ||
    echo "(no plugin traces in /var/tmp/rdna3-nvmtl)" > "$o/31-plugin-trace/none.txt"
cp "$R"/Library/Logs/DiagnosticReports/*.ips "$R"/Library/Logs/DiagnosticReports/*.shutdownStall "$o/" 2>/dev/null
# the OpenCore-injected core's IOLog lines do not reach the unified log on bare metal: the
# kernel message buffer of this boot (msgbuf=1048576) is the only copy of them
dmesg > "$o/dmesg.txt" 2>&1
sync
echo "gotowe: $o"
