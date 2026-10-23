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

lim=45
run() {   # run <file> <command...>: output and exit code, never stops the
          # script; a command still running after $lim s (45) is killed. The
          # watchdog takes its own sleep with it (no orphan sleep left behind
          # for minutes after a fast command, ECC review round 2)
    f=$1; shift
    { echo "\$ $*"
      "$@" & p=$!
      ( sleep "$lim" & s=$!; trap 'kill $s 2>/dev/null; exit 0' TERM; wait $s; kill -9 "$p" 2>/dev/null ) & w=$!
      wait "$p"; r=$?
      kill "$w" 2>/dev/null
      echo "[exit $r]"; } > "$dir/$f" 2>&1
}

# the Metal plugin's traces first (tools/macos/collect_plugin_trace.sh, as
# root, at most 120 s): before the slow log shows below, so WindowServer's
# live ring still holds the A/B when it is copied (ECC review round 2); its
# WindowServer log (32-log-nvmtl-windowserver.txt) comes later, bounded
if [ -f "$root/tools/macos/collect_plugin_trace.sh" ]; then
    lim=120
    run 37-plugin-trace-collect.txt sh "$root/tools/macos/collect_plugin_trace.sh" --traces-only "$dir"
    lim=45
fi

run 00-system.txt sw_vers
uname -a >> "$dir/00-system.txt" 2>&1
# the Mac model the firmware (OpenCore in the VM) gives macOS, and its board id: the
# triage's "present: SMBIOS" row (card run 2026-10-17d, present-mode A/B)
echo "hw.model: $(sysctl -n hw.model 2>&1)" >> "$dir/00-system.txt"
echo "board-id: $(ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | sed -n 's/.*"board-id" = <"\(.*\)">.*/\1/p' | head -1)" >> "$dir/00-system.txt"
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
# DRIVER-PLAN N40: RDNA3AGDC's counters and its vendor answer A/B (debug.rdna3agdc_vendor*);
# the framebuffer's switches and counters are properties in 12-ioreg-fb.txt
run 17-sysctl-agdc.txt sh -c "sysctl debug 2>/dev/null | grep rdna3agdc || echo '(no debug.rdna3agdc sysctl: RDNA3AGDC not loaded)'"
run 14-ioreg-displays.txt sh -c "ioreg -l -w0 -r -c IODisplayConnect | head -200"
run 15-system-profiler.txt system_profiler SPDisplaysDataType
# the logs of a whole card run: 45 s cut card run 54's plugin log off
# before Maps (log show over a boot of several apps takes longer)
lim=180
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
# the Metal plugin in every other process (card run 51: Chess's plugin
# log stopped where its lines no longer named RDNA3, before the failure)
run 26-log-metal-plugin.txt sh -c "log show --last boot --info --style compact --predicate 'eventMessage CONTAINS \"NVMTL\" AND process != \"WindowServer\"' 2>/dev/null | grep -v -e 'lazy air' -e sizequery -e 'heap texture'"
lim=45
# card run 54 (stale regions, Chess, Maps): RD3Surface::submitCopyForward and
# CoreDisplay's copy forward in one place, then each app's plugin warnings
grep -h -e 'copy forward' -e 'copy_forward' "$dir/20-log-rdna3.txt" "$dir/24-log-coredisplay.txt" 2>/dev/null \
    | head -400 > "$dir/27-copy-forward.txt"
grep -h -i -e 'refused' -e 'failed' -e 'NOT ' -e 'not a multiple' -e 'swizzle' -e 'clear-only' -e 'surface-in' \
    -e 'surface-out' "$dir/26-log-metal-plugin.txt" "$dir/24-log-coredisplay.txt" 2>/dev/null \
    | sed -E 's/^[0-9-]+ [0-9:.]+ +[A-Za-z]+ +//' | sort | uniq -c | sort -rn | head -300 > "$dir/28-plugin-warnings.txt"
# A3: IOSurfaces in the accelerator's VRAM for RADV, kernel and plugin
# halves each with their own budget (kernel: "GEM: IOSurface", "surfgem:";
# plugin: "item 7", the dirty marks); an empty source says so
{
    for f in 20-log-rdna3.txt 26-log-metal-plugin.txt 24-log-coredisplay.txt; do
        echo "== $f"
        if [ ! -s "$dir/$f" ]; then echo "(no such log in this report)"; continue; fi
        grep -h -e 'GEM: IOSurface' -e 'surfgem' -e 'surfvram' -e 'item 7' -e 'IOSurface VRAM is' \
            -e 'dirty refused' -e 'surface-out -> dirty' -e 'memory types' -e 'identity' "$dir/$f" \
            | head -250 || true
        # card run 58 with a budget of its own (the 250 above fill up first):
        # the display rule at start, the refusals, the misses, the verdicts per head
        grep -h -e 'a3 display' -e 'A3: WindowServer' -e "WindowServer's display framebuffer" \
            -e 'is not a display framebuffer' -e 'display check did not run' "$dir/$f" | tail -60 || true
        # DRIVER-PLAN N41 with a budget of its own: the VRAM parking and the
        # page-off's wait for WindowServer's GPU (kernel and plugin lines).
        # The boot lines and the plugin's decision get their own (ECC review
        # of N41: a busy run's 80 last lines cut them): first and last 4
        grep -h -e 'vm sticky: [01] =' -e 'surface wait: [01] =' \
            -e "surface wait (N41): WindowServer's IOSurface VRAM writes are" "$dir/$f" | head -4 || true
        grep -h -e 'vm sticky: [01] =' -e 'surface wait: [01] =' \
            -e "surface wait (N41): WindowServer's IOSurface VRAM writes are" "$dir/$f" | tail -4 || true
        grep -h -e 'vm sticky:' -e 'surface wait' -e 'PARKED' -e 'parked VRAM' -e 'VRAM refused:' \
            -e 'marks allocation #' -e 'its VRAM was parked' -e 'still LIVE' -e 'an older park' \
            -e 'NOT given back' -e 'aperture descriptor' -e 'getApertureMemoryDescriptor: the cached' "$dir/$f" \
            | grep -v -e 'vm sticky: [01] =' -e 'surface wait: [01] =' | tail -80 || true
    done
    echo "== sysctl (card run 58)"
    _a3=$(sysctl debug 2>/dev/null | grep -E "rdna3accel_(a3display|a3ws|a3off|a3:|flicker:|swaps_|swap_|trace_scan:|cf_|bufs_|iop_flip:|iop_queue|iop_wrote|perf|present_|flipbuf|hook_|vm_|surface_)")
    if [ -n "$_a3" ]; then echo "$_a3"; else echo "(no rdna3accel_a3display sysctl: kext older than card run 58)"; fi
} > "$dir/29-surface-vram.txt" 2>/dev/null
# card run 2026-10-17b: the kernel's per-frame trace (common/rdna3_trace.h),
# root only: the accelerator's ring, its per-second table, the core's ring
# (command submissions, fences, faults); decoded on the host by
# tools/rdna3_trace.py (tools/triage_reports.py)
{
    for st in "debug.rdna3accel_trace 36-frame-trace.bin" "debug.rdna3accel_trace_sec 36-frame-trace-sec.bin" \
              "debug.rdna3_drmtrace 36-drm-trace.bin"; do
        oid=${st%% *}; f=${st#* }
        sysctl -b "$oid" > "$dir/$f" 2>/dev/null
        [ -s "$dir/$f" ] || sudo -n sysctl -b "$oid" > "$dir/$f" 2>/dev/null
        if [ -s "$dir/$f" ]; then echo "$oid: $(wc -c < "$dir/$f") bytes in $f"
        else rm -f "$dir/$f"; echo "$oid: NOT read (not root, or a kext before card run 2026-10-17b)"; fi
    done
    echo "swap mode: $(sysctl -n debug.rdna3accel_swap_mode 2>/dev/null || echo none)"
    # the stream autoreport.sh pumped through the whole run (and its sizes):
    # in the archive too, so a lost upload does not lose it
    # (RDNA3_TRACE_DIR: autoreport's play step points it at a whole-blob snapshot while the pump still runs)
    for f in "${RDNA3_TRACE_DIR:-$HOME/rdna3-trace}"/*.bin "${RDNA3_TRACE_DIR:-$HOME/rdna3-trace}"/*.size; do
        [ -f "$f" ] || continue
        cp "$f" "$dir/36-stream-$(basename "$f")" && echo "stream: $(wc -c < "$f") bytes in 36-stream-$(basename "$f")"
    done
} > "$dir/36-frame-trace.txt" 2>&1
# card run 2026-10-17c: who holds the CPU-visible VRAM (the heap WindowServer's
# framebuffers and the GEM BOs share): the core's per-owner report and the
# kernel's refusal / summary lines; tools/triage_reports.py "VISIBLE VRAM"
{
    echo "== sysctl debug.rdna3_vram (the end of the run)"
    _vh=$(sysctl -n debug.rdna3_vram 2>/dev/null)
    if [ -n "$_vh" ]; then echo "debug.rdna3_vram: $_vh"
    else echo "(no debug.rdna3_vram sysctl: kext older than card run 2026-10-17c, or not loaded)"; fi
    # the refusal / summary lines of the kernel log; both log files (the sudo
    # one is written later), the head and the tail of each with a marker
    # between, so neither the first refusal nor the newest summary is cut
    _found=0
    for _lf in 20-log-rdna3.txt 20-log-rdna3-sudo.txt; do
        [ -s "$dir/$_lf" ] || continue
        _found=1
        echo "== kernel log ($_lf)"
        grep -h -e 'VRAM REFUSED' -e 'VRAM heap:' -e 'DRM/amdgpu UAPI ready' -e 'REFUSED by the framebuffer' \
            -e 'could not give' -e 'range retired' -e 'VRAM: a ' -e 'VRAM: debug.rdna3_vram' \
            -e 'GEM: .* VRAM BO' -e 'no VRAM and no GTT either' "$dir/$_lf" > "$dir/.38.tmp" 2>/dev/null || true
        _n=$(wc -l < "$dir/.38.tmp" | tr -d ' ')
        if [ "$_n" -le 200 ]; then cat "$dir/.38.tmp"
        else
            head -80 "$dir/.38.tmp"
            echo "== ... $((_n - 200)) line(s) left out between the first 80 and the last 120 =="
            tail -120 "$dir/.38.tmp"
        fi
        rm -f "$dir/.38.tmp"
    done
    [ "$_found" = 1 ] || echo "(no kernel log in this report)"
} > "$dir/38-vram-heap.txt" 2>/dev/null
# card run 2026-10-17d: the display bisect (rdna3vm step bisect, autoreport.sh
# bisect_run): per step the switches set and read back, the owner's answer, the
# looks' pipe lines and the counters; tools/triage_reports.py prints the table.
# autoreport.sh removes the folder at the start of every other run, so an old
# one is never packed
if [ -s "$HOME/rdna3-bisect/bisect.txt" ]; then
    cp "$HOME/rdna3-bisect/bisect.txt" "$dir/39-bisect.txt" 2>/dev/null
fi
# N1: which processes the Metal plugin served and why. The list, the
# boot-args (rdna3mtlapps=0 ends apps mode) and the plugin's decision line of
# every process that loaded it: "allow mode: <mode>; <process>: <reason>"
{
    echo "== /Library/GPUBundles/rdna3mtl-allow.txt"
    cat /Library/GPUBundles/rdna3mtl-allow.txt 2>&1 || echo "(not readable)"
    echo "== boot-args"; nvram boot-args 2>&1
    echo "== the plugin's decision per process (24-log-coredisplay.txt, 26-log-metal-plugin.txt)"
    grep -h 'allow mode' "$dir/24-log-coredisplay.txt" "$dir/26-log-metal-plugin.txt" 2>/dev/null \
        | sed -E 's/^[0-9-]+ [0-9:.]+ +[A-Za-z]+ +//' | sort | uniq -c | sort -rn | head -200
} > "$dir/34-metal-allow.txt" 2>&1
# which source the Metal plugin and RADV in this machine were built from
# (build_metal.sh / build_radv.sh write the stamps; tools/triage_reports.py
# compares them with the checkout: STALE PLUGIN/RADV). The kext was current
# but the plugin and RADV were not on card run 2026-10-15.
{
    for st in "plugin /Library/GPUBundles/rdna3mtl/metal.stamp" "radv /Library/Application Support/RDNA3VM/bin/radv.stamp"; do
        name=${st%% *}; file=${st#* }
        echo "== $name ($file)"
        if [ -r "$file" ]; then cat "$file"
        else echo "no stamp (built before stamps existed, or installed from a CI artifact)"; fi
    done
} > "$dir/35-build-stamps.txt" 2>&1
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

# WindowServer's NVMTL lines of this boot, whole (the plugin trace's
# companion; collect_plugin_trace.sh --traces-only above left it out)
if [ -f "$root/tools/macos/collect_plugin_trace.sh" ]; then
    lim=240
    run 32-log-nvmtl-windowserver.txt log show --last boot --info --style compact --predicate 'process == "WindowServer" AND eventMessage CONTAINS "NVMTL"'
    lim=45
fi

tar -C "$(dirname "$dir")" -czf "$out" "$(basename "$dir")" && rm -rf "$dir"
echo "report: $out"
echo "most useful first: 11-ioreg-props.txt and 20-log-rdna3.txt"
