#!/bin/sh
# Collect everything needed to diagnose one test boot of RDNA3-Hackintosh
# into a single archive (run on the hackintosh, after the boot under test):
#   tools/macos/collect.sh [label]        -> ~/rdna3-report-<label>-<date>.tar.gz
# Read-only: it changes nothing on the system. sudo only for the kernel log
# of the last boot when the current user cannot read it.
set -u
label=${1:-test}
stamp=${RDNA3_STAMP:-$(date +%Y%m%d-%H%M%S)}   # autoreport.sh names it before
dir=$(mktemp -d "/tmp/rdna3-report-XXXXXX")
out="$HOME/rdna3-report-$label-$stamp.tar.gz"
root=$(cd "$(dirname "$0")/../.." && pwd)

run() {   # run <file> <command...>: output and exit code, never stops the
          # script; a command still running after 45 s is killed
    f=$1; shift
    { echo "\$ $*"
      "$@" & p=$!
      ( sleep 45; kill -9 "$p" 2>/dev/null ) & w=$!
      wait "$p"; r=$?
      kill "$w" 2>/dev/null
      echo "[exit $r]"; } > "$dir/$f" 2>&1
}

run 00-system.txt sw_vers
uname -a >> "$dir/00-system.txt" 2>&1
run 01-boot-args.txt nvram boot-args
run 02-csr.txt csrutil status
run 03-kexts.txt sh -c "kmutil showloaded 2>/dev/null | grep -i -E 'rdna3|amd|radeon|IONDRV' || kextstat | grep -i -E 'rdna3|amd|radeon'"
# every PCI device with its ids (the card is 0x1002 / 0x747e), then the full
# entry of the AMD ones; "display" alone can be an IOServiceCompatibility stub
run 04-pci.txt sh -c "ioreg -l -w0 -r -c IOPCIDevice | grep -E '^[ |]*\+-o|\"(vendor-id|device-id|class-code|subsystem-vendor-id|assigned-addresses|IOPCIResourced|IOPCIConfigured|IOPCIMSIMode)\" ='; echo; echo '== AMD devices'; ioreg -l -w0 -r -c IOPCIDevice | awk '/^[ |]*\+-o/{if(b~/<02100000>/)print b; b=\"\"} {b=b\"\\n\"\$0} END{if(b~/<02100000>/)print b}' | head -300"
run 05-pci-profiler.txt system_profiler SPPCIDataType
run 06-kexts-all.txt sh -c "kmutil showloaded 2>/dev/null | grep -v com.apple; echo; echo '== RDNA3 in the boot/aux KC log'; log show --last boot --style compact --predicate 'eventMessage CONTAINS \"voten641\"' 2>/dev/null | head -40"
run 10-ioreg-core.txt ioreg -l -w0 -r -c RDNA3Accel
run 11-ioreg-props.txt sh -c "ioreg -l -w0 -r -c RDNA3Accel | grep -E '\"RDNA3,' | grep -v -E 'DiscoveryBinary|,EDID\"'"
run 12-ioreg-fb.txt ioreg -l -w0 -r -c RDNA3Framebuffer
run 13-ioreg-accel.txt ioreg -l -w0 -r -c RDNA3Accelerator
run 14-ioreg-displays.txt sh -c "ioreg -l -w0 -r -c IODisplayConnect | head -200"
run 15-system-profiler.txt system_profiler SPDisplaysDataType
run 20-log-rdna3.txt log show --last boot --style compact --predicate 'eventMessage CONTAINS "RDNA3" OR eventMessage CONTAINS "rdna3" OR eventMessage CONTAINS "RD3" OR eventMessage CONTAINS "RDNA3MTL"'
run 21-log-panics.txt sh -c "ls -lt /Library/Logs/DiagnosticReports/ 2>/dev/null | head -20; for f in \$(ls -t /Library/Logs/DiagnosticReports/*.panic 2>/dev/null | head -2); do echo \"== \$f\"; head -120 \"\$f\"; done"
# WindowServer crash reports (step 10: it restarts in a loop): the newest
# two, the crashed thread and the exception are in the first ~150 lines;
# the assertion text ("asi") is near the END of the .ips, so it is printed
# on its own first (card runs 41/45: cut off at 24 KB, it was lost)
run 23-windowserver-crash.txt sh -c "for f in \$(ls -t /Library/Logs/DiagnosticReports/WindowServer*.ips /Library/Logs/DiagnosticReports/Retired/WindowServer*.ips 2>/dev/null | head -2); do echo \"== \$f\"; grep -o '\"asi\" *: *{[^}]*}' \"\$f\" | head -3; head -c 24000 \"\$f\"; echo; done"
run 22-log-windowserver.txt log show --last boot --style compact --predicate 'process == "WindowServer" AND (eventMessage CONTAINS "GPU" OR eventMessage CONTAINS "Metal" OR eventMessage CONTAINS "accel")'
# CoreDisplay / IOPresentment / GPUWrangler / the Metal plugin in
# WindowServer, unfiltered: why a display pipe or Metal device failed
run 24-log-coredisplay.txt sh -c "log show --last boot --info --style compact --predicate 'process == \"WindowServer\" AND (subsystem BEGINSWITH \"com.apple.CoreDisplay\" OR subsystem BEGINSWITH \"com.apple.IOPresentment\" OR subsystem BEGINSWITH \"com.apple.GPUWrangler\" OR eventMessage CONTAINS \"NVMTL\" OR eventMessage CONTAINS[c] \"agdc\" OR eventMessage CONTAINS \"MetalDevice\")' 2>/dev/null | grep -v -e 'lazy air' -e sizequery -e 'heap texture'"
# whole (card run 48 lost the plugin's start to a tail, run 49 the middle
# where WindowServer restarted six times)
# every WindowServer crash of this boot, its termination reason and the
# assertion text (run 49: one "COREANIMATION ... Vulkan is not up" kept,
# the other restarts unexplained)
run 25-windowserver-exits.txt sh -c "for f in \$(ls -t /Library/Logs/DiagnosticReports/WindowServer*.ips /Library/Logs/DiagnosticReports/Retired/WindowServer*.ips 2>/dev/null | head -12); do echo \"== \$f\"; tr -d '\\n' < \"\$f\" | grep -o -E '\"(pid|procLaunch|termination|asi|exception)\" *: *(\\{[^}]*\\}|\"[^\"]*\"|[0-9]+)' | head -8; done; echo '== launchd about WindowServer'; log show --last boot --style compact --predicate 'process == \"launchd\" AND eventMessage CONTAINS \"WindowServer\"' 2>/dev/null | grep -i -E 'exit|crash|signal|spawn' | tail -40"
[ -s "$dir/20-log-rdna3.txt" ] && grep -q "RDNA3" "$dir/20-log-rdna3.txt" || \
    run 20-log-rdna3-sudo.txt sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "RDNA3"'

# the discovery blob, decoded when the host tools are built
if [ -x "$root/tools/macos/ioreg_discovery.sh" ]; then
    (cd "$dir" && sh "$root/tools/macos/ioreg_discovery.sh" discovery.bin > 30-discovery.txt 2>&1)
fi
# userspace view, when the tools are built (rdna3lvl>=2)
info="$root/build/userspace/rdna3_drminfo"
[ -x "$info" ] && run 31-drminfo.txt "$info"
pm="$root/build/userspace/rdna3_pmctl"
[ -x "$pm" ] && run 33-pmctl.txt "$pm" info
vki="$root/build/mesa/vulkaninfo.log"
[ -f "$vki" ] && cp "$vki" "$dir/32-vulkaninfo-ci.log"

tar -C "$(dirname "$dir")" -czf "$out" "$(basename "$dir")" && rm -rf "$dir"
echo "report: $out"
echo "most useful first: 11-ioreg-props.txt and 20-log-rdna3.txt"
