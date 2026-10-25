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

# DRIVER-PLAN N46 (1): WindowServer pairs a display with its accelerator through
# IOAccelFindAccelerator(fb), which reads IOAccelTypes (the registry path of an
# IOAccelerator), IOAccelIndex (SInt32) and IOAccelRevision (2) from the
# IOFramebuffer; the family's IOAccelDisplayPipe::init stamps them. One block per
# framebuffer instance (-c RDNA3Framebuffer matches AMDRDNA3Framebuffer, the leaf
# class since N44, and the older class of earlier reports), then the registry path
# of every RDNA3Accelerator to compare IOAccelTypes with (the path is built from the
# indentation of `ioreg -p IOService`: two columns per level; the first component below
# the registry root (the platform expert device, named after the Mac model, e.g.
# MacBookPro16,1) is left out, as the kernel's getPath leaves it out of IOAccelTypes:
# card run 2026-10-25c, macOS 26.4.1: IOAccelTypes
# IOService:/AppleACPIPlatformExpert/PCI0/.../RDNA3Accelerator). Triage row
# "display pairing" (tools/triage_reports.py); a key that is not printed is not set.
pairing_report() {
    echo "== framebuffers (ioreg -c RDNA3Framebuffer; IOAccel keys, RDNA3,Connected)"
    ioreg -l -w0 -r -c RDNA3Framebuffer -d 1 2>&1 \
        | grep -E '^\+-o |"(IOAccelTypes|IOAccelIndex|IOAccelRevision|RDNA3,Connected)" =' \
        | sed 's/^[ |]*//'
    echo "== accelerators (registry path of each RDNA3Accelerator)"
    ioreg -p IOService -w0 2>&1 | awk -v cls="${RDNA3_PAIRING_CLASS:-RDNA3Accelerator}" '
        { p = index($0, "+-o "); if (!p) next
          d = int((p - 1) / 2); n = substr($0, p + 4); sub(/  <.*/, "", n); comp[d] = n
          if (index($0, "<class " cls ",") || index($0, "<class " cls ">")) {
              path = "IOService:"; for (i = 2; i <= d; i++) path = path "/" comp[i]
              print "accelerator: " path; found++ } }
        END { if (!found) print "(no " cls " in the IOService plane)" }'
}

# Crash reports of this boot (card run 2026-10-25c: Chess crashed three times and nothing
# was collected, the only crash list was the one in /Library/Logs/DiagnosticReports).
# crash_reports() lists every *.ips in /Library/Logs/DiagnosticReports and in each
# /Users/*/Library/Logs/DiagnosticReports newer than the boot (kern.boottime), the
# newest 12, copies each (the first 300000 bytes: 40-crash-ips/) and prints a short
# summary: process, pid, exception, termination, the crashed thread and its top frames.
# The .ips is a JSON header line, then a JSON body; macOS has osascript (JXA) on every
# install, a grep fallback prints the header fields when it fails. Triage row "app
# crashes" (tools/triage_reports.py reads the "== crash:" blocks and "crashes: N").
# RDNA3_CRASH_DIRS replaces the directories (tests/test_collect_crashes.sh).
IPS_JXA='
function run(a) {
    var t = $.NSString.stringWithContentsOfFileEncodingError(a[0], 4, null).js;
    var nl = t.indexOf("\n"), h = {}, j, o = [];
    try { h = JSON.parse(t.slice(0, nl)); } catch (e) {}
    j = JSON.parse(t.slice(nl + 1));
    o.push("process: " + (j.procName || h.app_name || "?"));
    o.push("pid: " + (j.pid === undefined ? "?" : j.pid));
    var x = j.exception;
    if (x) o.push("exception: " + (x.type || "?") + (x.signal ? " (" + x.signal + ")" : "") + (x.subtype ? " " + x.subtype : ""));
    var tm = j.termination;
    if (tm) o.push("termination: " + JSON.stringify(tm).slice(0, 300));
    if (j.asi) o.push("asi: " + JSON.stringify(j.asi).slice(0, 300));
    var ths = j.threads || [], ix = -1, i;
    for (i = 0; i < ths.length; i++) if (ths[i].triggered) { ix = i; break; }
    if (ix < 0 && j.faultingThread !== undefined) ix = j.faultingThread;
    var th = ths[ix], im = j.usedImages || [];
    if (th) {
        o.push("crashed thread: " + ix + (th.queue ? " (" + th.queue + ")" : th.name ? " (" + th.name + ")" : ""));
        (th.frames || []).slice(0, 8).forEach(function (f, n) {
            var m = im[f.imageIndex];
            o.push("frame " + n + ": " + (m && m.name ? m.name : "?") + "  " +
                   (f.symbol || "0x" + (f.imageOffset || 0).toString(16)) + (f.symbolLocation ? " + " + f.symbolLocation : ""));
        });
    }
    return o.join("\n");
}'
mtime_of() {   # mtime_of FILE: epoch seconds (BSD stat; GNU stat as the fallback), empty when neither works
    _mt=$(stat -f %m "$1" 2>/dev/null)
    case $_mt in ''|*[!0-9]*) _mt=$(stat -c %Y "$1" 2>/dev/null) ;; esac
    case $_mt in ''|*[!0-9]*) _mt= ;; esac
    echo "$_mt"
}
ips_brief() {   # ips_brief FILE: the summary lines of one .ips
    _ib=
    if command -v osascript >/dev/null 2>&1; then
        _ib=$(osascript -l JavaScript -e "$IPS_JXA" "$1" 2>/dev/null)
    fi
    if [ -n "$_ib" ]; then printf '%s\n' "$_ib"; return 0; fi
    echo "(no JSON summary: osascript missing or failed; header fields by grep)"
    tr -d '\n' < "$1" | grep -o -E '"(app_name|procName|pid|bug_type)" *: *("[^"]*"|[0-9]+)' | head -6
    tr -d '\n' < "$1" | grep -o -E '"(exception|termination|asi)" *: *\{[^}]*\}' | cut -c1-300 | head -3
}
crash_reports() {
    _cbt=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ *sec = \([0-9]*\).*/\1/p')
    echo "boot: ${_cbt:-unknown}"
    [ -n "$_cbt" ] || echo "(boot time unknown: every crash report found is listed, newest 12)"
    mkdir -p "$dir/40-crash-ips"
    _cl="$dir/.40.list"; : > "$_cl"
    for _cd in ${RDNA3_CRASH_DIRS:-/Library/Logs/DiagnosticReports /Users/*/Library/Logs/DiagnosticReports}; do
        for _cf in "$_cd"/*.ips; do
            [ -f "$_cf" ] || continue
            _cm=$(mtime_of "$_cf")
            if [ -n "$_cbt" ] && [ -n "$_cm" ] && [ "$_cm" -lt "$_cbt" ]; then continue; fi
            printf '%s\t%s\n' "${_cm:-0}" "$_cf" >> "$_cl"
        done
    done
    _cn=0; _tab=$(printf '\t')
    sort -rn "$_cl" | head -12 > "$_cl.s"
    while IFS= read -r _cline; do
        _cm=${_cline%%"$_tab"*}; _cf=${_cline#*"$_tab"}
        _cn=$((_cn + 1))
        echo "== crash: $_cf"
        echo "mtime: $_cm${_cbt:+ ($((_cm - _cbt)) s after the boot)}"
        if [ ! -r "$_cf" ]; then echo "unreadable: no permission (run collect.sh as the admin user, or with sudo)"; continue; fi
        _csz=$(wc -c < "$_cf" | tr -d ' ')
        _cname="$_cn-$(printf '%s' "$(basename "$_cf")" | tr -c 'A-Za-z0-9._-' '_')"
        head -c 300000 "$_cf" > "$dir/40-crash-ips/$_cname" 2>/dev/null
        if [ "${_csz:-0}" -gt 300000 ]; then echo "size: $_csz bytes (copy cut at 300000)"; else echo "size: $_csz bytes"; fi
        echo "copy: 40-crash-ips/$_cname"
        ips_brief "$_cf"
    done < "$_cl.s"
    rm -f "$_cl" "$_cl.s"
    echo "crashes: $_cn"
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
# DRIVER-PLAN N46 (1): the keys WindowServer pairs a display with its accelerator by (pairing_report above)
run 16-ioaccel-pairing.txt pairing_report
# DRIVER-PLAN N40: RDNA3AGDC's counters and its vendor answer A/B (debug.rdna3agdc_vendor*);
# the framebuffer's switches and counters are properties in 12-ioreg-fb.txt
run 17-sysctl-agdc.txt sh -c "sysctl debug 2>/dev/null | grep rdna3agdc || echo '(no debug.rdna3agdc sysctl: RDNA3AGDC not loaded)'"
# DRIVER-PLAN N42: the core's vblank / pflip / HPD interrupts (tools/triage_reports.py "display irq"); the
# framebuffers' side is RDNA3,FBStats virq_* in 12-ioreg-fb.txt
run 18-sysctl-dispirq.txt sh -c "sysctl debug.rdna3_vblirq 2>/dev/null || echo '(no debug.rdna3_vblirq sysctl: a core older than DRIVER-PLAN N42)'; sysctl debug.rdna3_dispirq 2>/dev/null || echo '(no debug.rdna3_dispirq sysctl)'"
# DRIVER-PLAN N44: CoreDisplay's IOPresentment switches now, and as this boot's WindowServer
# started with them (kept by autoreport.sh; "boot <sec>" then "AnyHost: ...; ByHost(<uuid>): ..." on one
# line). Two stores: AnyHost (/Library/Preferences/com.apple.CoreDisplay.plist) and ByHost
# (/Library/Preferences/ByHost/com.apple.CoreDisplay.<IOPlatformUUID>.plist, the one Navi48-MacOS,
# Almosst-DEV, MIT, found CoreDisplay reading on macOS 26); the ByHost files that exist are listed
# so a wrong UUID in the name shows
run 19-coredisplay-prefs.txt sh -c 'defaults read /Library/Preferences/com.apple.CoreDisplay; u=$(ioreg -rd1 -c IOPlatformExpertDevice | grep IOPlatformUUID | head -n 1 | cut -d\" -f4); echo "== ByHost store (IOPlatformUUID $u):"; defaults read /Library/Preferences/ByHost/com.apple.CoreDisplay.$u; echo "== ByHost files:"; ls -l /Library/Preferences/ByHost/ | grep CoreDisplay; echo "== at the start of the boot (autoreport.sh):"; cat /var/db/rdna3-coredisplay-prefs-boot.txt'
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
# crash reports of this boot, user apps and system (crash_reports above; triage row "app crashes")
run 40-app-crashes.txt crash_reports
run 22-log-windowserver.txt log show --last boot --style compact --predicate 'process == "WindowServer" AND (eventMessage CONTAINS "GPU" OR eventMessage CONTAINS "Metal" OR eventMessage CONTAINS "accel")'
# CoreDisplay / IOPresentment / GPUWrangler / the Metal plugin in
# WindowServer, unfiltered: why a display pipe or Metal device failed
run 24-log-coredisplay.txt sh -c "log show --last boot --info --style compact --predicate 'process == \"WindowServer\" AND (subsystem BEGINSWITH \"com.apple.CoreDisplay\" OR subsystem BEGINSWITH \"com.apple.IOPresentment\" OR subsystem BEGINSWITH \"com.apple.GPUWrangler\" OR eventMessage CONTAINS \"NVMTL\" OR eventMessage CONTAINS \"NullMoth:\" OR eventMessage CONTAINS[c] \"agdc\" OR eventMessage CONTAINS \"MetalDevice\" OR eventMessage CONTAINS \"Invalid end point size\" OR eventMessage CONTAINS \"capabilities with no devices\" OR eventMessage CONTAINS \"device with no planes\" OR eventMessage CONTAINS \"Failed to get capabilities\")' 2>/dev/null | grep -v -e 'lazy air' -e sizequery -e 'heap texture: '"
# whole (card run 48 lost the plugin's start to a tail, run 49 the middle
# where WindowServer restarted six times)
# every WindowServer crash of this boot, its termination reason and the
# assertion text (run 49: one "COREANIMATION ... Vulkan is not up" kept,
# the other restarts unexplained)
run 25-windowserver-exits.txt sh -c "for f in \$(ls -t /Library/Logs/DiagnosticReports/WindowServer*.ips /Library/Logs/DiagnosticReports/Retired/WindowServer*.ips 2>/dev/null | head -12); do echo \"== \$f\"; tr -d '\\n' < \"\$f\" | grep -o -E '\"(pid|procLaunch|termination|asi|exception)\" *: *(\\{[^}]*\\}|\"[^\"]*\"|[0-9]+)' | head -8; done; echo '== launchd about WindowServer'; log show --last boot --style compact --predicate 'process == \"launchd\" AND eventMessage CONTAINS \"WindowServer\"' 2>/dev/null | grep -i -E 'exit|crash|signal|spawn' | tail -40"
# the Metal plugin in every other process (card run 51: Chess's plugin
# log stopped where its lines no longer named RDNA3, before the failure)
run 26-log-metal-plugin.txt sh -c "log show --last boot --info --style compact --predicate '(eventMessage CONTAINS \"NVMTL\" OR eventMessage CONTAINS \"NullMoth:\") AND process != \"WindowServer\"' 2>/dev/null | grep -v -e 'lazy air' -e sizequery -e 'heap texture: '"
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
# DRIVER-PLAN N50: the A/B ladder (rdna3vm step ladder, autoreport.sh ladder_run): ladder.txt (one block per rung: the switches it ran with,
# the looks, the owner's answer, the rung's log file) and each rung's plugin log slice; tools/triage_reports.py prints one row per rung
if [ -s "$HOME/rdna3-ladder/ladder.txt" ]; then
    cp "$HOME/rdna3-ladder/ladder.txt" "$dir/41-ladder.txt" 2>/dev/null
    for _lf in "$HOME"/rdna3-ladder/*.log; do
        [ -f "$_lf" ] && cp "$_lf" "$dir/41-ladder-$(basename "$_lf")" 2>/dev/null
    done
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
# N50: the plugin's conf file as the plugin reads it (the A/B profile of a run is in it: NVMTL_RADV_DEBUG, NVMTL_PARANOID, ...); the plugin also logs
# `conf: set ...` and `vk: ab switches` per process, this is the file itself
{
    echo "== /Library/GPUBundles/rdna3mtl/rdna3mtl.conf"
    cat /Library/GPUBundles/rdna3mtl/rdna3mtl.conf 2>&1 || echo "(no conf file: the defaults)"
} > "$dir/34-metal-conf.txt" 2>&1
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
