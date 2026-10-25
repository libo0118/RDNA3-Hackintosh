#!/bin/sh
# Boot-time report for the VM test (LaunchDaemon org.rdna3.vmreport, root).
# Waits for the boot to settle, runs tools/macos/collect.sh, adds the
# rdna3_drminfo DMA/GFX tests on the levels that have them, uploads the
# archive to the host (QEMU user network: host = 10.0.2.2) and shuts down
# when the host started the VM with rdna3vmauto=1.
D="/Library/Application Support/RDNA3VM"
HOST=http://10.0.2.2:8000
export HOME=/var/root
# the copies waiting in /var/db for the next boot and the trace files are
# root's alone (ECC review round 2: they were world-readable); not a global
# umask - the kexts, the plugin and the bin copies this script installs must
# stay readable for macOS and WindowServer
pend_cp() { ( umask 077; cp "$1" "$2" ) 2>/dev/null; }
# --standalone: a bare-metal install (package/make_pkg.sh, docs/TESTING-HW.md),
# run by hand with sudo: the same tests and report, but no VM host - no
# wait, no updates, no uploads - the files land in /Users/Shared/rdna3-reports
STANDALONE=
if [ "${1:-}" = --standalone ]; then
    STANDALONE=1; HOST=; D="/Library/Application Support/RDNA3"; shift
    [ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
fi
# the display bisect (card run 2026-10-17d, bisect_run below), by hand in a
# Terminal: sudo sh autoreport.sh --standalone --bisect | --bisect-plugin |
# --bisect-agdc (a tty: y / n / Enter per step). In the VM the boot-arg
# rdna3bisect=1 / 2 / 4 asks for it (rdna3vm step bisect / bisect-plugin /
# bisect-agdc).
BISECT=; BISECT_HAND=
# the A/B ladder (DRIVER-PLAN N50, ladder_run below): sudo sh autoreport.sh --standalone --ladder | --ladder-ws | --ladder-glyphs;
# in the VM the boot-arg rdna3ladder=1 / 2 / 3 (rdna3vm step ladder / ladder-ws / ladder-glyphs)
LADDER=
case "${1:-}" in
    --ladder) LADDER=1; BISECT_HAND=1; shift ;;
    --ladder-ws) LADDER=2; BISECT_HAND=1; shift ;;
    --ladder-glyphs) LADDER=3; BISECT_HAND=1; shift ;;
esac
case "${1:-}" in
    --bisect) BISECT=1; BISECT_HAND=1; shift ;;
    --bisect-plugin) BISECT=2; BISECT_HAND=1; shift ;;
    --bisect-agdc) BISECT=4; BISECT_HAND=1; shift ;;
esac

# with_limit SECONDS command...: never let one stage hold the VM up
with_limit() {
    t=$1; shift
    "$@" & p=$!
    ( sleep "$t"; kill -9 "$p" 2>/dev/null ) & w=$!
    wait "$p"; r=$?
    kill "$w" 2>/dev/null
    return $r
}

# stop the VM when the host asked for it (rdna3vmauto=1): shutdown, and if
# that has not happened within 30 s (a logged-in session can hold it up)
# halt without asking anybody
finish() {
    case " $(nvram boot-args 2>/dev/null | cut -f2-) " in
        *" rdna3vmauto=1 "*)
            shutdown -h now
            sleep 30
            halt -q ;;
    esac
    exit 0
}

# the Metal plugin's RADV + libdrm (build_metal.sh installs the plugin):
# the copies next to it follow the report's bin, so a new bundle reaches
# Metal too. RADV finds libdrm through the rpath added here.
# the Metal plugin's trace directory (/var/tmp/rdna3-nvmtl): created or
# repaired by tools/macos/fix_tracedir.sh (the plugin's own: a planted
# symlink removed, no mkdir -p as root through someone's link); a failure
# is logged, never fatal
fix_tracedir() {
    _ft="$D/tools/macos/fix_tracedir.sh"
    [ -f "$_ft" ] || return 0
    sh "$_ft" || echo "== fix_tracedir.sh FAILED ($?): the plugin's traces may be missing"
}
sync_metal() {
    fix_tracedir
    plugin_trace_conf
    m=/Library/GPUBundles/rdna3mtl
    [ -d "$m" ] && [ -f "$1/libvulkan_radeon.dylib" ] || return 0
    for f in libvulkan_radeon.dylib libdrm.2.dylib libdrm_amdgpu.1.dylib; do
        [ -f "$1/$f" ] && ! cmp -s "$1/$f" "$m/$f" && cp "$1/$f" "$m/$f" && touch "$m/.changed" && metal_synced=1
    done
    if [ -f "$m/.changed" ]; then
        install_name_tool -add_rpath "$m" "$m/libvulkan_radeon.dylib" 2>/dev/null
        for f in "$m"/*.dylib; do codesign -f -s - "$f" >/dev/null 2>&1; done
        rm -f "$m/.changed"
    fi
    printf '{\n    "ICD": {\n        "api_version": "1.4.0",\n        "library_path": "%s/libvulkan_radeon.dylib"\n    },\n    "file_format_version": "1.0.0"\n}\n' \
        "$m" > "$m/radeon_icd.x86_64.json"
}
if [ "${1:-}" = --sync-metal ]; then
    sync_metal "$2"
    exit 0
fi

# the Metal allow list as it was, should a report have died inside its M6
# probe (which widens it for two apps)
[ -f /var/db/rdna3-allow.bak ] && mv /var/db/rdna3-allow.bak /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null
# the same for a bisect-plugin run that died (it edits rdna3mtl.conf and the
# allow list; the allow list's copy is the one above)
if [ -f /var/db/rdna3-bisect-conf.bak ]; then
    mv -f /var/db/rdna3-bisect-conf.bak /Library/GPUBundles/rdna3mtl/rdna3mtl.conf 2>/dev/null &&
        echo "== rdna3mtl.conf put back from a bisect-plugin run that did not finish"
elif [ -f /var/db/rdna3-bisect-conf.bak.absent ]; then
    rm -f /Library/GPUBundles/rdna3mtl/rdna3mtl.conf /var/db/rdna3-bisect-conf.bak.absent
fi
# the system sleep setting as it was, should a report have died inside its
# display sleep look (which sets it to never; the VM's time limit, a kill):
# a bare-metal machine must not keep "never" (card run 58b review)
SLEEP_BAK=/var/db/rdna3-sleep.bak
restore_sleep() {
    [ -f "$SLEEP_BAK" ] || return 0
    _s=$(sed -n '1s/^\([0-9][0-9]*\)$/\1/p' "$SLEEP_BAK")
    if [ -n "$_s" ] && pmset -a sleep "$_s" >/dev/null 2>&1; then
        rm -f "$SLEEP_BAK"
        echo "system sleep restored to $_s min"
    else
        echo "system sleep NOT restored from $SLEEP_BAK (\"$(head -c 40 "$SLEEP_BAK")\"): pmset -a sleep <minutes> by hand"
    fi
}
restore_sleep
[ -n "$STANDALONE" ] && trap 'restore_sleep' EXIT && trap 'restore_sleep; exit 130' INT && trap 'restore_sleep; exit 143' TERM

# which processes the Metal plugin serves (metal/plugin/rdna3mtl_allow_policy.h):
# "apps" = every application (the list has a %apps line), "list" = the names
# in the list only; rdna3mtlapps=0 in the boot-args switches apps mode off
rdna3_allow_mode() {
    _al=/Library/GPUBundles/rdna3mtl-allow.txt
    [ -r "$_al" ] || { echo "no readable allow list"; return; }
    if ! grep -q '^[[:space:]]*%apps[[:space:]]*$' "$_al"; then echo "list (no %apps line)"
    elif sysctl -n kern.bootargs 2>/dev/null | tr ' ' '\n' | grep -qx 'rdna3mtlapps=0'; then
        echo "list (%apps line switched off by boot-arg rdna3mtlapps=0)"
    else echo "apps (every application; denied: $(sed -n 's/^[[:space:]]*-[[:space:]]*//p' "$_al" | tr '\n' ',' | sed 's/,$//'))"; fi
}

# card run 58: WindowServer's display framebuffers and A3 (refused /
# imported, the legacy swaps per head and those that scanned out an A3 BO)
a3_sysctls() {
    _a3=$(sysctl debug 2>/dev/null | grep -E "rdna3accel_(a3display|a3ws|a3off|a3:|flicker:|swaps_|swap_|trace_scan:|cf_|bufs_|iop_flip:|iop_queue|iop_wrote|perf|present_|flipbuf|hook_|vm_|surface_)")
    if [ -n "$_a3" ]; then echo "$_a3"
    else echo "(no rdna3accel_a3display sysctl: kext older than card run 58)"; fi
    # card run 2026-10-17c: who holds the CPU-visible VRAM now (tools/triage_reports.py "VISIBLE VRAM")
    _vh=$(sysctl -n debug.rdna3_vram 2>/dev/null)
    if [ -n "$_vh" ]; then echo "debug.rdna3_vram: $_vh"
    else echo "(no debug.rdna3_vram sysctl: kext older than card run 2026-10-17c)"; fi
    # DRIVER-PLAN N42: the core's vblank / pflip / HPD interrupts (tools/triage_reports.py "display irq")
    _di=$(sysctl -n debug.rdna3_dispirq 2>/dev/null)
    if [ -n "$_di" ]; then echo "debug.rdna3_vblirq: $(sysctl -n debug.rdna3_vblirq 2>/dev/null)"; echo "debug.rdna3_dispirq: $_di"
    else echo "(no debug.rdna3_dispirq sysctl: a core older than DRIVER-PLAN N42)"; fi
}

# card run 2026-10-17b: the kernel's per-frame trace streamed every 3 s
# through the whole run (both rings: the accelerator's and the core's), so
# no event of the tests and the A/B is lost (the stream headers count any
# that were; ECC review round 1: the pump ran only in the A/B before). The
# files go into the archive (collect.sh: 36-stream-*.bin), up to the host
# next to it, and into /var/db/rdna3-pending-* until they are sent. After
# each A/B phase a snapshot (whole blobs: the pump pauses for the copy) and
# WindowServer's plugin trace go up too, so a run cut later keeps the A/B
# (ECC review round 2).
TS="$HOME/rdna3-trace"
TRACE_FILES="trace-stream.bin drmtrace-stream.bin trace-sec.bin"
TRACE_CAP_BYTES=536870912   # 512 MB per stream file: the pump stops appending (logged) beyond it
trace_pump_start() {
    ( umask 077; mkdir -p "$TS" ) && chmod 700 "$TS"
    rm -rf "$TS/snap" "$TS"/plug-*
    rm -f "$TS"/*.bin "$TS"/*.size "$TS"/*.capped "$TS/.pump" "$TS/.hold" "$TS/.held" "$TS/.pumpdone" "$TS/.pumpstarted" "$TS/pump.err" "$TS/stopped"
    if ! sysctl -n debug.rdna3accel_swap_mode >/dev/null 2>&1; then
        echo "== frame trace: no debug.rdna3accel_trace_stream (an accelerator before card run 2026-10-17b, or none): not streamed"
        return 0
    fi
    # the first read: everything of the boot the ring still holds (its 'lost'
    # counts the boot's events overwritten before it); kept as the stream
    # files' first blob (ECC review round 2). It must be binary ("R3TR").
    for p in "debug.rdna3accel_trace_stream trace-stream.bin" "debug.rdna3_drmtrace_stream drmtrace-stream.bin"; do
        oid=${p% *}; f=${p#* }
        sysctl -b "$oid" > "$TS/first.bin" 2>"$TS/first.err"
        m=$(head -c 4 "$TS/first.bin" 2>/dev/null)
        if [ "$m" = R3TR ]; then
            cat "$TS/first.bin" >> "$TS/$f"
            echo "== frame trace: $oid streams ($(wc -c < "$TS/first.bin") bytes of the boot at the start, kept)"
        else
            echo "== frame trace: $oid NOT BINARY: sysctl -b gave $(wc -c < "$TS/first.bin") bytes starting \"$(head -c 40 "$TS/first.bin" | tr -c '[:print:]' '.')\" ($(head -c 120 "$TS/first.err")) -- the stream files of this run cannot be decoded"
        fi
    done
    rm -f "$TS/first.bin" "$TS/first.err"
    touch "$TS/.pump" "$TS/.pumpstarted"
    # the pump: stops with the report ($$ gone: no orphan), pauses while a
    # snapshot copies (.hold -> .held), logs failed reads, caps each file
    ( while [ -f "$TS/.pump" ] && kill -0 $$ 2>/dev/null; do
          if [ -f "$TS/.hold" ]; then touch "$TS/.held"; sleep 1; continue; fi
          rm -f "$TS/.held"
          for p in "debug.rdna3accel_trace_stream trace-stream.bin" "debug.rdna3_drmtrace_stream drmtrace-stream.bin"; do
              oid=${p% *}; f=${p#* }
              sz=$(wc -c < "$TS/$f" 2>/dev/null | tr -d ' '); sz=${sz:-0}
              if [ "$sz" -ge "$TRACE_CAP_BYTES" ]; then
                  [ -f "$TS/$f.capped" ] || { touch "$TS/$f.capped"
                      echo "$(date +%T) $f reached $TRACE_CAP_BYTES bytes: no more reads into it" >> "$TS/pump.err"; }
                  continue
              fi
              sysctl -b "$oid" >> "$TS/$f" 2>>"$TS/pump.err" || echo "$(date +%T) $oid: read failed ($?)" >> "$TS/pump.err"
          done
          sleep 3
      done
      touch "$TS/.pumpdone" ) &
    TRACE_PUMP=$!
    echo "== frame trace: pump started ($(date +%T), every 3 s into $TS)"
}
# the pump paused (whole blobs in the files), at most 15 s
trace_hold() {
    [ -f "$TS/.pump" ] || return 0
    touch "$TS/.hold"
    _w=0
    while [ ! -f "$TS/.held" ] && [ -f "$TS/.pump" ] && [ $_w -lt 15 ]; do sleep 1; _w=$((_w + 1)); done
    [ -f "$TS/.held" ] || echo "== frame trace: the pump did not pause in 15 s (a read hangs?): the snapshot may end in a partial blob"
}
trace_release() { rm -f "$TS/.hold" "$TS/.held"; }
trace_sizes() {   # trace_sizes <dir>: the size next to each file (triage tells a cut-off upload from a short run)
    for f in $TRACE_FILES; do
        [ -f "$1/$f" ] && wc -c < "$1/$f" | tr -d ' ' > "$1/$f.size"
    done
}
trace_pump_stop() {
    [ -d "$TS" ] || return 0
    [ -f "$TS/stopped" ] && return 0
    rm -f "$TS/.pump" "$TS/.hold"   # stops the pump even when this shell did not start it (a subshell)
    _w=0
    # the pump's last read must end before ours (two appends would interleave)
    while [ -f "$TS/.pumpstarted" ] && [ ! -f "$TS/.pumpdone" ] && [ $_w -lt 10 ]; do sleep 1; _w=$((_w + 1)); done
    [ -n "${TRACE_PUMP:-}" ] && wait "$TRACE_PUMP" 2>/dev/null
    TRACE_PUMP=
    touch "$TS/stopped"
    if sysctl -n debug.rdna3accel_swap_mode >/dev/null 2>&1; then
        sysctl -b debug.rdna3accel_trace_stream >> "$TS/trace-stream.bin" 2>>"$TS/pump.err"
        sysctl -b debug.rdna3_drmtrace_stream >> "$TS/drmtrace-stream.bin" 2>>"$TS/pump.err"
        sysctl -b debug.rdna3accel_trace_sec > "$TS/trace-sec.bin" 2>>"$TS/pump.err"
        # the newest events into the kernel log too: a serial log alone has them
        sysctl -w debug.rdna3accel_trace_dump=600 >/dev/null 2>&1
    fi
    trace_sizes "$TS"
    echo "== frame trace: pump stopped ($(date +%T)); files:"
    for f in $TRACE_FILES; do
        if [ -f "$TS/$f" ]; then echo "$f: $(cat "$TS/$f.size" 2>/dev/null) bytes"; else echo "$f: MISSING"; fi
    done
    if [ -s "$TS/pump.err" ]; then
        echo "== frame trace: pump read errors ($(wc -l < "$TS/pump.err" | tr -d ' ') lines, the last 20):"
        tail -20 "$TS/pump.err"
    fi
}
# up to the host: <report>-<file>, kept in /var/db/rdna3-pending-* until
# sent (the next boot sends what is left); at most 60 s per file. The
# standalone run (no host) has them in its archive (36-stream-*.bin).
trace_upload() {   # trace_upload <file> <name>
    [ -s "$1" ] && [ -n "$HOST" ] || return 0
    pend_cp "$1" "/var/db/rdna3-pending-$base-$2"
    if curl -sf --connect-timeout 5 --max-time 60 -T "$1" "$HOST/reports/$base-$2" >/dev/null 2>&1; then
        rm -f "/var/db/rdna3-pending-$base-$2"
    else
        echo "upload of $2 FAILED (kept for the next boot)"
    fi
}
trace_send() {   # trace_send [dir]: the stream files (and their sizes) of $TS or a snapshot
    _d=${1:-$TS}
    for f in $TRACE_FILES; do
        trace_upload "$_d/$f" "$f"
        trace_upload "$_d/$f.size" "$f.size"
    done
}
# WindowServer's live plugin trace, copied by the plugin's own collector
# (its checks: owner, links, magic, this boot), bounded, as root
snap_plugin() {   # snap_plugin <label>
    _c="$D/tools/macos/collect_plugin_trace.sh"
    [ -f "$_c" ] || { echo "== plugin trace snapshot $1: no collect_plugin_trace.sh"; return 0; }
    rm -rf "$TS/plug-$1"; mkdir -p "$TS/plug-$1"
    with_limit 90 sh "$_c" --traces-only --only WindowServer "$TS/plug-$1"
    _n=0
    for t in "$TS/plug-$1"/31-plugin-trace/*.trace; do
        [ -f "$t" ] || continue
        _n=$((_n + 1))
        # every snapshot keeps its own pending copy until sent: an earlier one
        # holds records the ring has overwritten since (triage merges them)
        trace_upload "$t" "plugin-$1-$(basename "$t")"
    done
    echo "== plugin trace snapshot $1: $_n WindowServer trace(s); $(grep -c '^REJECT' "$TS/plug-$1/31-plugin-trace/info.txt" 2>/dev/null || echo 0) rejected"
    grep '^REJECT' "$TS/plug-$1/31-plugin-trace/info.txt" 2>/dev/null | head -4
}
# a snapshot of everything so far (whole blobs), sent at once: the A/B
# survives a later cut (the VM's time limit, a hang in a later test)
trace_snapshot() {   # trace_snapshot <label>
    [ -d "$TS" ] || return 0
    trace_hold
    rm -rf "$TS/snap"; mkdir -p "$TS/snap"
    for f in trace-stream.bin drmtrace-stream.bin; do [ -f "$TS/$f" ] && cp "$TS/$f" "$TS/snap/$f"; done
    trace_release
    sysctl -b debug.rdna3accel_trace_sec > "$TS/snap/trace-sec.bin" 2>/dev/null
    trace_sizes "$TS/snap"
    trace_send "$TS/snap"
    snap_plugin "$1"
    echo "== frame trace snapshot $1 ($(date +%T)): $(cat "$TS/snap/trace-stream.bin.size" 2>/dev/null || echo 0) bytes of stream sent"
}

# the Metal plugin's main ring: 256 K records (32 MB) instead of 16 K, so
# WindowServer's trace still covers the A/B when it is collected (ECC review
# round 2: the ring wrapped minutes before). The plugin reads
# /Library/GPUBundles/rdna3mtl/rdna3mtl.conf when a process loads it (the
# release build drops inherited NVMTL_* variables first, the file's are
# taken), so the knob reaches WindowServer at its next start: the next boot.
PLUGIN_TRACE_RING=262144
# Disk: with that ring every process that loads the plugin preallocates
# about 51 MB (32 MB main ring, 16 MB Vulkan ring, the summary) in
# /var/tmp/rdna3-nvmtl, and nothing cleans it: prune_old_traces deletes the
# traces of earlier boots at the start of each run; a standalone run takes
# the knob out again once its report is collected (the VM keeps it).
prune_old_traces() {
    _td=${RDNA3_TRACE_DIR:-/var/tmp/rdna3-nvmtl}   # the variable only for tests/test_prune_traces.sh
    [ -d "$_td" ] && [ ! -L "$_td" ] || return 0
    case "$(stat -f %u "$_td" 2>/dev/null)" in 0|88) ;; *) echo "== plugin traces: $_td is not root's or _windowserver's: not pruned"; return 0 ;; esac
    _bt=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ *sec = \([0-9]*\).*/\1/p')
    [ -n "$_bt" ] || { echo "== plugin traces: boot time unknown: not pruned"; return 0; }
    _n=0; _kb=0
    for _f in "$_td"/*.trace; do
        [ -f "$_f" ] && [ ! -L "$_f" ] || continue
        set -- $(ls -ldn "$_f" 2>/dev/null)
        [ "${2:-0}" = 1 ] || continue
        case "${3:-x}" in 0|88|"$(id -u)") ;; *) continue ;; esac
        _m=$(stat -f %m "$_f" 2>/dev/null) || continue
        [ "$_m" -lt "$((_bt - 120))" ] || continue
        _kb=$((_kb + ${5:-0} / 1024))
        rm -f "$_f" && _n=$((_n + 1))
    done
    echo "== plugin traces: $_n trace(s) of earlier boots deleted ($((_kb / 1024)) MB)"
}
plugin_trace_conf_off() {   # the standalone run: the ring back to the plugin's default
    _cf=/Library/GPUBundles/rdna3mtl/rdna3mtl.conf
    [ -f "$_cf" ] && [ ! -L "$_cf" ] && grep -q '^NVMTL_TRACE_RING=' "$_cf" || return 0
    grep -v '^NVMTL_TRACE_RING=' "$_cf" > "$_cf.new" && chown root:wheel "$_cf.new" && chmod 644 "$_cf.new" &&
        mv -f "$_cf.new" "$_cf" && echo "== plugin trace ring: NVMTL_TRACE_RING taken out of $_cf again (standalone run)"
}
plugin_trace_conf() {
    _m=/Library/GPUBundles/rdna3mtl
    [ -d "$_m" ] && [ ! -L "$_m" ] || return 0
    _cf="$_m/rdna3mtl.conf"
    [ -L "$_cf" ] && rm -f "$_cf"
    if grep -qx "NVMTL_TRACE_RING=$PLUGIN_TRACE_RING" "$_cf" 2>/dev/null; then
        echo "== plugin trace ring: NVMTL_TRACE_RING=$PLUGIN_TRACE_RING in $_cf"
        return 0
    fi
    { grep -v '^NVMTL_TRACE_RING=' "$_cf" 2>/dev/null; echo "NVMTL_TRACE_RING=$PLUGIN_TRACE_RING"; } > "$_cf.new" &&
        chown root:wheel "$_cf.new" && chmod 644 "$_cf.new" && mv -f "$_cf.new" "$_cf" &&
        echo "== plugin trace ring: NVMTL_TRACE_RING=$PLUGIN_TRACE_RING written to $_cf - WindowServer takes it at its next start (the next boot); this boot's ring is the old size" ||
        echo "== plugin trace ring: could NOT write $_cf"
}

# DRIVER-PLAN N44: CoreDisplay's IOPresentment switches, read by GetBoolCFPreference from
# the com.apple.CoreDisplay domain (probe run on 24H32; CoreDisplay's static values:
# useIOP 1, useIOP_onAMD 0). A framebuffer CoreDisplay classes as AMD (vendor 4) may use
# IOPresentment only with useIOP_onAMD: written on every boot of the VM (the install boot
# included, so the card boot's WindowServer starts with them) into BOTH stores that could be
# the one CoreDisplay reads:
#  - AnyHost:  /Library/Preferences/com.apple.CoreDisplay.plist (AnyUser + AnyHost, what
#    `defaults write /Library/Preferences/com.apple.CoreDisplay` reaches)
#  - ByHost:   /Library/Preferences/ByHost/com.apple.CoreDisplay.<host UUID>.plist (AnyUser +
#    CurrentHost). Fact from Navi48-MacOS (Almosst-DEV, MIT, tools/pc/cdpref.c, macOS 26):
#    CoreDisplay::GetBoolCFPreference calls CFPreferencesCopyValue(key, "com.apple.CoreDisplay",
#    kCFPreferencesAnyUser, kCFPreferencesCurrentHost), the ByHost store. The file is written by
#    its path because `defaults -currentHost write com.apple.CoreDisplay` as root goes to
#    /var/root/Library/Preferences/ByHost, the wrong store (AnyUser + CurrentHost is
#    /Library/Preferences/ByHost); and `defaults -currentHost write <abs path>` is no way either
#    (macOS 26.4.1 appends the host UUID itself: <path>.<IOPlatformUUID>.plist), so the path
#    given is the one WITH the UUID and no -currentHost. Unproven on
#    macOS 15 (DRIVER-PLAN N44 open item). The host UUID in the file name is IOPlatformUUID, the
#    one CFPreferences uses for ByHost files since 10.8; its files in the directory are listed
#    in 19-coredisplay-prefs.txt (collect.sh), so a wrong UUID shows.
# The keys are useIOP and useIOP_onAMD only (Navi48-MacOS also names displaySurfaceUsesIOSurface
# and useIOP_gamma_onAMD, read by _CGXMappedDisplayStart: the next A/B, DRIVER-PLAN N44).
# rdna3iopprefs=0 in the boot-args takes both keys out of both stores again (the A/B).
# WindowServer reads them at its start, before this script: what this boot's WindowServer saw
# is the state at the first call of the boot (kept in CD_PREFS_BOOT, "AnyHost: ...;
# ByHost(<uuid>): ..." on its second line), a change here takes effect from the next boot.
# A standalone run only reports them.
CD_PREFS=${RDNA3_CD_PREFS:-/Library/Preferences/com.apple.CoreDisplay}
CD_BYHOST_DIR=${RDNA3_CD_BYHOST_DIR:-/Library/Preferences/ByHost}
CD_PREFS_BOOT=${RDNA3_CD_PREFS_BOOT:-/var/db/rdna3-coredisplay-prefs-boot.txt}
cd_prefs_read() {   # cd_prefs_read <domain>: the domain on one line, empty when it does not exist
    defaults read "$1" 2>/dev/null | tr -s ' \n\t' '   ' | sed 's/^ //; s/ $//'
}
cd_host_uuid() {   # IOPlatformUUID, empty when ioreg does not give a plausible one (it ends up in a path)
    _u=$(ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F'"' '/IOPlatformUUID/{print $4; exit}')
    case $_u in ''|*[!0-9A-Fa-f-]*) _u= ;; esac
    printf '%s' "$_u"
}
cd_prefs_state() {   # cd_prefs_state <uuid>: "AnyHost: <domain>; ByHost(<uuid>): <domain>" (triage parses it)
    _sa=$(cd_prefs_read "$CD_PREFS")
    if [ -n "$1" ]; then _sb=$(cd_prefs_read "$CD_BYHOST_DIR/com.apple.CoreDisplay.$1"); _sb=${_sb:-(none)}
    else _sb="(unknown)"; fi
    printf 'AnyHost: %s; ByHost(%s): %s' "${_sa:-(none)}" "${1:-unknown}" "$_sb"
}
cd_prefs_on() {   # cd_prefs_on <domain>: both keys true, read back
    defaults write "$1" useIOP -bool true >/dev/null 2>&1
    defaults write "$1" useIOP_onAMD -bool true >/dev/null 2>&1
    [ "$(defaults read "$1" useIOP_onAMD 2>/dev/null)" = 1 ] && [ "$(defaults read "$1" useIOP 2>/dev/null)" = 1 ]
}
cd_prefs_off() {   # cd_prefs_off <domain>: both keys taken out, read back
    defaults delete "$1" useIOP >/dev/null 2>&1
    defaults delete "$1" useIOP_onAMD >/dev/null 2>&1
    [ -z "$(defaults read "$1" useIOP_onAMD 2>/dev/null)" ] && [ -z "$(defaults read "$1" useIOP 2>/dev/null)" ]
}
coredisplay_iop_prefs() {
    _uuid=$(cd_host_uuid)
    _bh=
    [ -n "$_uuid" ] && _bh="$CD_BYHOST_DIR/com.apple.CoreDisplay.$_uuid"
    _bt=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ *sec = \([0-9]*\).*/\1/p')
    _rec=ok
    if [ -z "$_bt" ]; then
        # no boot time to tell this boot's record from an earlier boot's: rewrite it every call, and say so
        # (the state kept may already be the one after this script's own change)
        { echo "boot ?"; cd_prefs_state "$_uuid"; echo; } > "$CD_PREFS_BOOT" 2>/dev/null || _rec=lost
        echo "== CoreDisplay prefs: boot time unknown: could NOT tell this boot's start state, it may be stale"
    elif [ "$(head -n 1 "$CD_PREFS_BOOT" 2>/dev/null)" != "boot $_bt" ]; then
        { echo "boot $_bt"; cd_prefs_state "$_uuid"; echo; } > "$CD_PREFS_BOOT" 2>/dev/null || _rec=lost
    fi
    if [ "$_rec" = lost ]; then
        echo "== CoreDisplay prefs: could NOT keep this boot's state in $CD_PREFS_BOOT"
        _seen="(unknown)"
    else
        _seen=$(sed -n '2p' "$CD_PREFS_BOOT" 2>/dev/null)
    fi
    echo "== CoreDisplay prefs ($CD_PREFS, DRIVER-PLAN N44): this boot's WindowServer started with: ${_seen:-(none)}"
    if [ -n "${STANDALONE:-}" ]; then
        _what="left as they are (standalone run)"
    elif sysctl -n kern.bootargs 2>/dev/null | tr ' ' '\n' | grep -qx 'rdna3iopprefs=0'; then
        if cd_prefs_off "$CD_PREFS"; then _ra="taken out"; else _ra="could NOT be taken out"; fi
        if [ -z "$_bh" ]; then _rb="could NOT be taken out (host UUID unknown: ioreg gave no IOPlatformUUID)"
        elif cd_prefs_off "$_bh"; then _rb="taken out"; else _rb="could NOT be taken out"; fi
        _what="rdna3iopprefs=0: useIOP / useIOP_onAMD: AnyHost $_ra; ByHost(${_uuid:-unknown}) $_rb"
    else
        if cd_prefs_on "$CD_PREFS"; then _ra="written"; else _ra="could NOT be written"; fi
        if [ -z "$_bh" ]; then _rb="could NOT be written (host UUID unknown: ioreg gave no IOPlatformUUID)"
        else
            [ -d "$CD_BYHOST_DIR" ] || mkdir -p "$CD_BYHOST_DIR" 2>/dev/null
            if cd_prefs_on "$_bh"; then _rb="written"; else _rb="could NOT be written"; fi
        fi
        _what="useIOP / useIOP_onAMD true: AnyHost $_ra; ByHost(${_uuid:-unknown}) $_rb (rdna3iopprefs=0 takes them out)"
    fi
    _now=$(cd_prefs_state "$_uuid")
    echo "== CoreDisplay prefs: $_what; now: $_now$([ "$_now" = "$_seen" ] || echo '; WindowServer takes the change at its next start (the next boot)')"
}

# card run 2026-10-17b: one run decides between the swap models. 60 s with
# debug.rdna3accel_swap_mode=1 (the copy), 60 s with 0 (the flip), back to
# what it was. Opt-in since card run 2026-10-17d: only with the boot-arg
# rdna3swapab=1 (one of the two modes may break the picture for a minute; the
# flip was the default 2026-10-17d..g, the copy again since the write
# generations); per phase a look at the panels, the counters and a snapshot
# of the traces. Activity Monitor redraws its graphs every few seconds: both
# phases see the same kind of activity (whether it runs is logged; a head
# whose display shows nothing that redraws has no swaps and is not judged,
# triage says so). The phases are labelled in the trace
# (debug.rdna3accel_swap_ab) and here ("== swap A/B phase N"); triage
# prints the two side by side. Should the report die inside it, the trap
# puts the mode back (ECC review round 1). Returns non-zero when a setting
# did not take or nothing redrew (ECC review round 2).
ab_restore() {
    [ -n "${ab_old:-}" ] && sysctl -w debug.rdna3accel_swap_mode="$ab_old" >/dev/null 2>&1
    sysctl -w debug.rdna3accel_swap_ab=0 >/dev/null 2>&1
    rm -f "$TS/.pump" "$TS/.hold"
    restore_sleep
}
ab_set() {   # ab_set <oid> <value>: set and read back, loud when it did not take
    if ! e=$(sysctl -w "$1=$2" 2>&1); then
        echo "== swap A/B: FAILED to set $1=$2: $e"
        return 1
    fi
    v=$(sysctl -n "$1" 2>/dev/null)
    [ "$v" = "$2" ] || { echo "== swap A/B: $1 is $v after setting $2 (NOT APPLIED)"; return 1; }
}
swap_ab() {
    if ! sysctl -n debug.rdna3accel_swap_mode >/dev/null 2>&1; then
        echo "== swap A/B: SKIPPED - no debug.rdna3accel_swap_mode (an accelerator before card run 2026-10-17b, or none)"
        return 0
    fi
    ab_rc=0
    ab_old=$(sysctl -n debug.rdna3accel_swap_mode)
    trap 'ab_restore' EXIT
    trap 'ab_restore; exit 130' INT
    trap 'ab_restore; exit 143' TERM
    # preconditions: somebody logged in (a desktop that redraws), the
    # displays awake; both logged
    _w=0; _u=
    while [ $_w -lt 120 ]; do
        _u=$(stat -f %Su /dev/console 2>/dev/null)
        case "$_u" in ""|root|loginwindow) sleep 5; _w=$((_w + 5)) ;; *) break ;; esac
    done
    echo "== swap A/B: console user ${_u:-none} (waited ${_w} s)"
    case "$_u" in ""|root|loginwindow) echo "== swap A/B: NOBODY LOGGED IN - nothing redraws, the A/B is inconclusive"; ab_rc=1 ;; esac
    caffeinate -u -t 5 >/dev/null 2>&1 &
    echo "== swap A/B: power: $(pmset -g powerstate IODisplayWrangler 2>/dev/null | tail -1 | tr -s ' ')"
    pmset -g assertions 2>/dev/null | grep -iE 'UserIsActive|PreventUserIdleDisplaySleep' | head -3
    in_session open -g -a "Activity Monitor" 2>&1
    sleep 3
    am=$(pgrep -x "Activity Monitor" | head -1)
    if [ -n "$am" ]; then echo "== swap A/B: activity: Activity Monitor runs (pid $am; it redraws on the main display only: a head with no swaps is not judged)"
    else echo "== swap A/B: activity: Activity Monitor is NOT running - the phases may see no swaps (inconclusive)"; ab_rc=1; fi
    for ph in "1 1" "2 0" "3 $ab_old"; do
        phase=${ph% *}; mode=${ph#* }
        ab_set debug.rdna3accel_swap_mode "$mode" || ab_rc=1
        ab_set debug.rdna3accel_swap_ab "$phase" || ab_rc=1
        echo "== swap A/B phase $phase: swap mode $(sysctl -n debug.rdna3accel_swap_mode 2>/dev/null) ($(date +%T))"
        [ "$phase" = 3 ] && break
        sleep 60
        mkdir -p "$HOME/rdna3-scanout/ab$mode"
        echo "== rdna3_drminfo --scanout (look ab$mode)"
        [ -x "$D/bin/rdna3_drminfo" ] && with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/ab$mode" 2>&1
        echo "== swap A/B counters after mode $mode"
        a3_sysctls
        trace_snapshot "ab$phase"
    done
    ab_set debug.rdna3accel_swap_ab 0 || ab_rc=1
    pkill -x "Activity Monitor" 2>/dev/null
    trap - EXIT INT TERM
    [ -n "$STANDALONE" ] && trap 'restore_sleep' EXIT && trap 'restore_sleep; exit 130' INT && trap 'restore_sleep; exit 143' TERM
    # the A/B's newest events into the kernel log (a serial log alone has
    # them), and everything up once more
    sysctl -w debug.rdna3accel_trace_dump=600 >/dev/null 2>&1
    trace_snapshot ab-end
    echo "== swap A/B done: swap mode $(sysctl -n debug.rdna3accel_swap_mode 2>/dev/null) (exit $ab_rc)"
    return $ab_rc
}

# BISECT-BEGIN (tests/test_bisect_step.sh runs this block on the host)
# Display bisect (card run 2026-10-17d): the owner's windows do not appear or
# flash, the screens go black or flicker, since about card run 58. Every new
# feature that has a runtime switch is turned off, then back on one at a time
# (oldest first), and each state is held for BISECT_HOLD (30) s while the
# owner looks at both monitors. `rdna3vm step bisect` (boot-arg rdna3bisect=1:
# the kernel's sysctls) or `rdna3vm step bisect-plugin` (rdna3bisect=2: the
# Metal plugin's conf, WindowServer restarted), or by hand in a Terminal
# (a tty: y / n / Enter per step):
#   sudo sh autoreport.sh --standalone --bisect      (or --bisect-plugin, --bisect-agdc)
# `rdna3vm step bisect-agdc` (rdna3bisect=4, card run 2026-10-17f) is the same
# bisect on the agdc profile (WindowServer on our Metal plugin, rdna3agdc=3
# rdna3acceliop=1 rdna3accelfb=4): the plain `bisect` boots the default
# profile, where the AGDC is idle and WindowServer does no GPU work, so its
# result (swap mode 0 fine, mode 1 broken) says nothing about the real
# desktop. Its steps are NOT cumulative: each sets its whole combination
# (plan 4 below); it adds the iop, iop_flip, flipbuf_present and swap_gen switches and the per-step trace
# position (trace_events_start / stream_bytes_start, and a one-event
# trace_dump line in the kernel log as a marker: the serial log has no clock).
# The owner knows which step runs even with a broken picture: a notification,
# `say "step N"` and N short beeps at every step. Evidence per step goes into
# $HOME/rdna3-bisect/bisect.txt (collect.sh: 39-bisect.txt; uploaded as
# <report>-bisect.txt after every step): the switches set and read back, the
# owner's answer, two scanout looks (bisect<N>a about 8 s in, bisect<N> about
# 20 s in; the PPMs go up as <report>-scanoutbisect<N>[a]-pipe<P>.ppm), the
# counters, the trace stream's size. tools/triage_reports.py (tools/triage_bisect.py)
# prints the table. Nothing here changes the swap_ab label: with it set the
# trace sampling (trace_scan=1) would scan every swap, which is not the default.
BISECT_KNOBS_BASE="a3 flicker swap_latch swap_torn swap_mode trace_scan swap_stale swap_landed a3ws"
BISECT_KNOBS=$BISECT_KNOBS_BASE   # bisect_run adds iop, iop_flip, flipbuf_present and swap_gen for the agdc variant (4)
BISECT_CONF=${BISECT_CONF:-/Library/GPUBundles/rdna3mtl/rdna3mtl.conf}
BISECT_ALLOW=${BISECT_ALLOW:-/Library/GPUBundles/rdna3mtl-allow.txt}
BISECT_CONFBAK=${BISECT_CONFBAK:-/var/db/rdna3-bisect-conf.bak}
BISECT_ALLOWBAK=${BISECT_ALLOWBAK:-/var/db/rdna3-allow.bak}

# the steps of `bisect`: N|kind|name|a3 flicker swap_latch swap_torn swap_mode trace_scan swap_stale swap_landed a3ws|reopen
# @ = the value the boot left (the defaults: swap_mode 1 = the copy, again since the write generations of card run
# 2026-10-17d; it was 0 = the flip from 2026-10-17d to 2026-10-17g; trace_scan 0),
# - = set nothing, a number = that value;
# reopen 1 = Activity Monitor is opened again (a3 / a3ws only reach surfaces created after the switch)
bisect_plan_1() {
    cat <<'PLAN'
0|ref|reference: the defaults as the boot left them|- - - - - - - - -|1
1|off|every new feature off (closest to before the bug)|0 0 0 0 0 0 0 0 @|1
2|on|A3: applications' IOSurfaces in the card's VRAM (a3)|@ 0 0 0 0 0 0 0 @|1
3|on|flicker test of the swaps (flicker)|@ @ 0 0 0 0 0 0 @|0
4|on|a swap waits for its flip to latch (swap_latch)|@ @ @ 0 0 0 0 0 @|0
5|on|torn-frame test (swap_torn)|@ @ @ @ 0 0 0 0 @|0
6|on|stale-back hold (swap_stale) and swap mode 1 (the copy, the default again): the defaults again|@ @ @ @ @ @ @ @ @|0
7|extra|CONTROL, off by default: swap mode 0, the flip (the default 2026-10-17d..g; on the agdc profile it shows the buffer WindowServer writes)|@ @ @ @ 0 @ @ @ @|0
8|extra|CONTROL, off by default: trace sampling of the swap's other buffers (trace_scan=1)|@ @ @ @ @ 1 @ @ @|0
9|extra|CONTROL, off by default: mode 1, the landed buffer is presented (swap_mode=1, swap_landed=1)|@ @ @ @ 1 @ @ 1 @|0
10|extra|CONTROL, off by default: mode 2, every back copied (swap_mode=2)|@ @ @ @ 2 @ @ @ @|0
11|extra|CONTROL, off by default: WindowServer's inputs in A3 (a3ws=1, the card run 58b flicker)|@ @ @ @ @ @ @ @ 1|1
12|final|the defaults again|@ @ @ @ @ @ @ @ @|1
PLAN
}
# the steps of `bisect-plugin`: N|kind|name|action|reopen|what is switched
bisect_plan_2() {
    cat <<'PLAN'
0|ref|reference: the Metal plugin as it is|-|1|nothing (Activity Monitor re-opened)
1|off|every plugin feature off|off|1|NVMTL_NO_SURFACE_SHARE=1, NVMTL_NO_SURFACE_PAGEOFF=1 and NVMTL_TRACE=0 in rdna3mtl.conf, the %apps line out of the allow list, WindowServer restarted
2|on|A3's plugin half: IOSurface sharing and dirty marks|share|1|both NVMTL_NO_SURFACE_* lines out of rdna3mtl.conf, WindowServer restarted (apps mode and the trace still off)
3|on|apps mode: every application gets the Metal device|apps|1|the allow list as it was (the %apps line), Activity Monitor re-opened; WindowServer is not restarted
4|final|the plugin's frame trace (NVMTL_TRACE): the defaults again|trace|1|rdna3mtl.conf as it was, WindowServer restarted
PLAN
}

# the steps of `bisect-agdc` (rdna3bisect=4): N|kind|name|a3 flicker swap_latch swap_torn swap_mode trace_scan swap_stale swap_landed a3ws iop iop_flip flipbuf_present swap_gen|reopen
# NOT cumulative: every step sets its whole combination; @ = the value the boot left, ! = the other one of 0 / 1
# (iop_flip toggled), a number = that value. The values every step names (swap_mode, trace_scan, swap_stale, iop,
# flipbuf_present, swap_gen) are the defaults of the agdc profile (swap_mode 1 = the copy, trace_scan 0, swap_stale 1,
# iop 1, flipbuf_present 0, swap_gen 1: card run 2026-10-17d's write generations), so the reference is those
# defaults even if a boot-arg moved one of them. All of these switches can change at runtime (accel/rdna3-accel.cpp:
# swap_mode, trace_scan, swap_stale, swap_latch, a3, iop_flip, swap_gen, flipbuf_present are read at every swap,
# flip-buffer call or import; iop is a SYSCTL_PROC that publishes / removes IOAccelDisplayPipeCapabilities and makes
# the transaction hooks answer as the base class), none was dropped. Only surfaces created after the switch see a3
# (reopen 1 = Activity Monitor opened again); WindowServer may have read the pipe capabilities at its start (iop 0
# then only changes what the hooks answer: the card says). Steps 1-3 tell the copy from the flip with and without
# the write generations, steps 4-5 the family's flip-buffer frames presented (flipbuf_present 1) in either mode.
bisect_plan_4() {
    cat <<'PLAN'
0|ref|reference: the defaults of the agdc profile (swap_mode 1 the copy, swap_gen 1, swap_stale 1, trace_scan 0, iop 1, flipbuf_present 0)|@ @ @ @ 1 0 1 @ @ 1 @ 0 1|1
1|var|swap_mode 0 (the flip), swap_gen 1|@ @ @ @ 0 0 1 @ @ 1 @ 0 1|0
2|var|swap_mode 1, swap_gen 0 (the write generations only counted: the copy of card run 2026-10-17d)|@ @ @ @ 1 0 1 @ @ 1 @ 0 0|0
3|var|swap_mode 0, swap_gen 0 (the flip of card runs 2026-10-17f / g)|@ @ @ @ 0 0 1 @ @ 1 @ 0 0|0
4|var|swap_mode 1, flipbuf_present 1 (the family's flip-buffer frames copied to the panel too)|@ @ @ @ 1 0 1 @ @ 1 @ 1 1|0
5|var|swap_mode 0, flipbuf_present 1 (the family's flip-buffer frames flipped to)|@ @ @ @ 0 0 1 @ @ 1 @ 1 1|0
6|var|swap_mode 2 (every back copied: the control, nothing held)|@ @ @ @ 2 0 1 @ @ 1 @ 0 1|0
7|var|swap_mode 1, iop 0 (no display-pipe transactions)|@ @ @ @ 1 0 1 @ @ 0 @ 0 1|0
8|var|swap_mode 1, a3 0 (IOSurfaces not in the card's VRAM)|0 @ @ @ 1 0 1 @ @ 1 @ 0 1|1
9|var|swap_mode 1, swap_latch 0 (a copy does not wait for the last flip to latch)|@ @ 0 @ 1 0 1 @ @ 1 @ 0 1|1
10|var|swap_mode 1, swap_stale 0 (stale backs left to the write generations alone)|@ @ @ @ 1 0 0 @ @ 1 @ 0 1|0
11|var|swap_mode 0, iop_flip toggled from its boot value|@ @ @ @ 0 0 1 @ @ 1 ! 0 1|0
12|final|the defaults again|@ @ @ @ 1 0 1 @ @ 1 @ 0 1|1
PLAN
}

bisect_w() { printf '%s\n' "$*" >> "$BF"; }
bisect_say() {   # the results, the progress window and, from a Terminal, the Terminal
    echo "$*"
    prog "$*"
    if [ -n "${_btty:-}" ] && [ -z "${BISECT_KEYFILE:-}" ]; then echo "$*" > /dev/tty 2>/dev/null; fi
    return 0
}
bisect_mmss() { printf '%02d:%02d' $(($1 / 60)) $(($1 % 60)); }
bisect_clock() { date -r "$1" +%H:%M:%S 2>/dev/null || date -d "@$1" +%H:%M:%S 2>/dev/null || echo "?"; }
bisect_knob() { echo "$BISECT_KNOBS" | cut -d' ' -f"$1"; }
bisect_orig() { eval "echo \"\${BORIG_$1:--}\""; }

bisect_read_originals() {   # the value of every switch now (- = this kext has no such sysctl)
    _i=0
    for _k in $BISECT_KNOBS; do
        _i=$((_i + 1))
        _v=$(sysctl -n "debug.rdna3accel_$_k" 2>/dev/null)
        [ -n "$_v" ] || _v=-
        eval "BORIG_$_i=\$_v"
    done
    [ "$(bisect_orig 5)" != - ]   # swap_mode is the oldest of them: without it nothing here can run
}
bisect_values_now() {   # the nine values, in knob order
    _o=; _i=0
    for _k in $BISECT_KNOBS; do
        _i=$((_i + 1))
        _v=$(sysctl -n "debug.rdna3accel_$_k" 2>/dev/null)
        _o="$_o ${_v:--}"
    done
    echo $_o
}
bisect_resolve() {   # bisect_resolve "<one token per switch>": @ -> the value the boot left, ! -> the other one of 0 / 1
    _o=; _i=0
    for _t in $1; do
        _i=$((_i + 1))
        case $_t in
            @) _t=$(bisect_orig "$_i") ;;
            !) case $(bisect_orig "$_i") in 0) _t=1 ;; -) _t=- ;; *) _t=0 ;; esac ;;
        esac
        _o="$_o $_t"
    done
    echo $_o
}
bisect_at() { _a=; for _k in $BISECT_KNOBS; do _a="$_a @"; done; echo $_a; }   # one @ per switch
bisect_changed() {   # bisect_changed "<before>" "<after>": what differs, by switch name
    _o=; _i=0
    for _k in $BISECT_KNOBS; do
        _i=$((_i + 1))
        _p=$(echo "$1" | cut -d' ' -f"$_i"); _c=$(echo "$2" | cut -d' ' -f"$_i")
        if [ "$_c" != - ] && [ "$_c" != "$_p" ]; then _o="$_o$_k $_p->$_c, "; fi
    done
    _o=${_o%, }
    echo "${_o:-nothing}"
}
bisect_make_plan() {   # bisect_make_plan <1|2|4>: $BD/plan.txt = N|kind|name|values or action|reopen|what is switched
    : > "$BD/plan.txt"
    bisect_plan_"$1" | {
        _prev=$(bisect_resolve "$(bisect_at)")   # what the boot left
        while IFS='|' read -r _pn _pk _pname _pasg _preo _pchg; do
            [ -n "$_pn" ] || continue
            if [ "$1" != 2 ]; then
                _res=$(bisect_resolve "$_pasg")
                _pchg=$(bisect_changed "$_prev" "$_res")
                # plan 1 is cumulative (a step that sets nothing keeps the base); plan 4 compares every step with the boot
                if [ "$1" = 1 ]; then [ -z "$(echo "$_res" | tr -d ' -')" ] || _prev=$_res; fi
                _pasg=$_res
            fi
            printf '%s|%s|%s|%s|%s|%s\n' "$_pn" "$_pk" "$_pname" "$_pasg" "$_preo" "$_pchg" >> "$BD/plan.txt"
        done
    }
}
bisect_timetable() {   # the timetable on the screen, in the results and (plan: lines) in bisect.txt
    bisect_say "BISECT timetable: step, mm:ss after the start at $(bisect_clock "$BSTART"), what is switched (each step holds $HOLD s)"
    while IFS='|' read -r _pn _pk _pname _pasg _preo _pchg <&3; do
        _line=$(printf 'step %2d  %s  %s: %s' "$_pn" "$(bisect_mmss $((_pn * HOLD)))" "$_pname" "$_pchg")
        bisect_say "  $_line"
        bisect_w "plan: $_pn|$(bisect_mmss $((_pn * HOLD)))|$_pk|$_pname|$_pchg"
    done 3< "$BD/plan.txt"
    [ "$_bv" = 2 ] && bisect_say "  (the times do not count the WindowServer restarts: log in when the login window shows)"
    return 0
}

bisect_beeps() {   # N short beeps: afplay of a system sound, else the terminal bell
    _bn=$1; _bi=0
    _snd=${BISECT_SOUND:-/System/Library/Sounds/Tink.aiff}
    while [ "$_bi" -lt "$_bn" ]; do
        if command -v afplay >/dev/null 2>&1 && [ -f "$_snd" ]; then in_session afplay "$_snd" >/dev/null 2>&1
        else { printf '\a' > /dev/tty; } 2>/dev/null || printf '\a' >&2; sleep "${BISECT_BEEPGAP:-0.4}"; fi
        _bi=$((_bi + 1))
    done
}
bisect_announce() {   # bisect_announce N LAST NAME CHANGED
    bisect_say "BISECT step $1 of $2: $3 - switched: $4"
    notify "step $1 of $2: $3"
    ( command -v say >/dev/null 2>&1 && in_session say "step $1" >/dev/null 2>&1
      bisect_beeps "$1" ) >/dev/null 2>&1 &
    _bpid=$!
    if [ -n "${_btty:-}" ] && [ -z "${BISECT_KEYFILE:-}" ]; then
        echo "    y = looks right, n = broken, Enter = next step ($HOLD s)" > /dev/tty 2>/dev/null
    fi
    return 0
}

bisect_key() {   # bisect_key SECONDS: prints y, n or e (Enter), nothing on a timeout; a key file stands in for the tty in the host test
    if [ -n "${BISECT_KEYFILE:-}" ]; then
        _kl=$(head -n 1 "$BISECT_KEYFILE" 2>/dev/null)
        if [ -s "$BISECT_KEYFILE" ]; then tail -n +2 "$BISECT_KEYFILE" > "$BISECT_KEYFILE.t"; mv -f "$BISECT_KEYFILE.t" "$BISECT_KEYFILE"; fi
        case $_kl in y|Y) echo y ;; n|N) echo n ;; "") echo e ;; esac   # any other line (-): no key
        return 0
    fi
    _ks=$(stty -g < /dev/tty 2>/dev/null) || { sleep "$1"; return 0; }
    stty -icanon -echo min 0 time $(($1 * 10)) < /dev/tty 2>/dev/null
    _kh=$(dd bs=1 count=1 < /dev/tty 2>/dev/null | od -An -tx1 | tr -d ' \n')
    stty "$_ks" < /dev/tty 2>/dev/null
    case $_kh in 79|59) echo y ;; 6e|4e) echo n ;; 0a|0d) echo e ;; esac
}
bisect_takekey() {   # bisect_takekey SECONDS: wait up to that long for y / n / Enter and note it
    if [ -n "${BISECT_KEYFILE:-}" ]; then   # the host test: one line of the file per step, then only the clock
        if [ -z "${_bkread:-}" ]; then _bkread=1; _kk=$(bisect_key "$1"); else _kk=; sleep "$1"; fi
    else
        _kk=$(bisect_key "$1")
    fi
    case $_kk in
        y) _bans=y; _bgo=1 ;;
        n) _bans=n; _bgo=1 ;;
        e) [ -n "$_bans" ] || _bans=enter; _bgo=1 ;;
    esac
    return 0
}
bisect_wait() {   # bisect_wait EPOCH: to that second; with a tty a key ends the step once its evidence is in
    while [ "$(date +%s)" -lt "$1" ]; do
        if [ -n "${_btty:-}" ]; then
            bisect_takekey 1
            [ "${_bev:-}" = 1 ] && [ "${_bgo:-}" = 1 ] && return 0
        else
            sleep 1
        fi
    done
    # a key typed while the looks ran is still waiting in the terminal
    if [ -n "${_btty:-}" ] && [ "${_bev:-}" = 1 ] && [ -z "${_bgo:-}" ]; then bisect_takekey 0; fi
    return 0
}

bisect_look() {   # bisect_look NAME LIMIT: rdna3_drminfo --scanout into rdna3-scanout/NAME; its pipe lines into bisect.txt
    bisect_w "-- look $1 ($(date +%T))"
    if [ ! -x "$D/bin/rdna3_drminfo" ]; then bisect_w "NOT TAKEN: no $D/bin/rdna3_drminfo"; return 1; fi
    mkdir -p "$HOME/rdna3-scanout/$1"
    with_limit "$2" "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$1" > "$BD/look.out" 2>&1
    _lr=$?
    grep -E '^(pipe [0-9]+:|  picture|  UNDERFLOW|display asleep|[0-9]+ pipe\(s\) lit)' "$BD/look.out" | cut -c1-200 >> "$BF"
    bisect_w "exit: $_lr"
    return $_lr
}
bisect_counter_lines() {   # the swap counters (a3_sysctls), trace_scan, and the display-pipe switch and its counters
    { a3_sysctls; sysctl debug.rdna3accel_trace_scan 2>/dev/null
      sysctl debug 2>/dev/null | grep -E '^debug\.rdna3accel_(iop|iop_ok|iop_fail|iop_flips|iop_flip_home|iop_flip_lock|iop_flip_novram|iop_flip_stale|iop_flip_refused|iop_async_flips|iop_async_refused|iop_async_drop|iop_copy_skip):'
    } | grep -v '^debug.rdna3_vram' | cut -c1-200
}
bisect_counters() {
    bisect_w "-- counters"
    bisect_counter_lines >> "$BF"
}
bisect_before() {   # the start of a step: the counters it is measured from, and where the trace is (its slice of the stream)
    bisect_w "-- before"
    bisect_counter_lines >> "$BF"
    _sb=$(wc -c < "$TS/trace-stream.bin" 2>/dev/null | tr -d ' ')
    bisect_w "stream_bytes_start: ${_sb:-none}"
    bisect_w "trace_events_start: $(sysctl -n debug.rdna3accel_swap_trace_events 2>/dev/null || echo none)"
    # one event into the kernel log ("trace: events W..W (asked by sysctl ...)"): the serial log has no clock, this line
    # puts the step's start in it (W >= trace_events_start, a few events later); tools/triage_bisect.py cuts the log by it
    sysctl -w debug.rdna3accel_trace_dump=1 >/dev/null 2>&1
}
bisect_trace() {   # the trace stream is pumped all through the run: its size and the event count place this step in it
    _sb=$(wc -c < "$TS/trace-stream.bin" 2>/dev/null | tr -d ' ')
    bisect_w "stream_bytes: ${_sb:-none}"
    bisect_w "trace_events: $(sysctl -n debug.rdna3accel_swap_trace_events 2>/dev/null || echo none)"
}
bisect_reopen_am() {   # Activity Monitor redraws every few seconds on the main display; a new window = new surfaces
    pkill -x "Activity Monitor" 2>/dev/null
    sleep "${BISECT_AMWAIT:-1}"
    in_session open -g -a "Activity Monitor" >/dev/null 2>&1
    sleep "${BISECT_AMWAIT:-1}"
}

bisect_apply_kext() {   # bisect_apply_kext "<nine values>": set, read back, say what did not take
    _bset=; _bfail=
    _i=0
    for _v in $1; do
        _i=$((_i + 1)); _k=$(bisect_knob "$_i")
        [ "$_v" = - ] && continue
        [ "$(bisect_orig "$_i")" = - ] && continue   # this kext has no such switch (the header's absent: line says so)
        # iop publishes / removes the pipe capabilities (a log line, a property change): never written to the value it has
        [ "$_k" = iop ] && [ "$(sysctl -n debug.rdna3accel_iop 2>/dev/null)" = "$_v" ] && continue
        _bset="$_bset debug.rdna3accel_$_k=$_v"
        if ! _e=$(sysctl -w "debug.rdna3accel_$_k=$_v" 2>&1); then
            _bfail="$_bfail debug.rdna3accel_$_k:set-failed($(echo "$_e" | head -c 60 | tr -d '\n'))"
            continue
        fi
        _r=$(sysctl -n "debug.rdna3accel_$_k" 2>/dev/null)
        [ "$_r" = "$_v" ] || _bfail="$_bfail debug.rdna3accel_$_k:reads-${_r:-nothing}-after-setting-$_v"
    done
    bisect_w "set:${_bset:- (nothing)}"
    _rb=; _i=0
    for _v in $(bisect_values_now); do
        _i=$((_i + 1)); _rb="$_rb debug.rdna3accel_$(bisect_knob "$_i")=$_v"
    done
    bisect_w "readback:$_rb"
    [ -z "$_bfail" ] || bisect_w "failed:$_bfail"
    [ -z "$_bfail" ]
}

# --- the plugin variant: rdna3mtl.conf and the allow list, WindowServer restarted
bisect_plugin_save() {   # the originals, once (a dead run's are put back at the next start)
    [ -f "$BISECT_CONFBAK" ] || [ -f "$BISECT_CONFBAK.absent" ] || {
        if [ -f "$BISECT_CONF" ]; then cp "$BISECT_CONF" "$BISECT_CONFBAK"; else : > "$BISECT_CONFBAK.absent"; fi; }
    [ -f "$BISECT_ALLOWBAK" ] || { [ -f "$BISECT_ALLOW" ] && cp "$BISECT_ALLOW" "$BISECT_ALLOWBAK"; }
    return 0
}
bisect_own() { chown root:wheel "$1" 2>/dev/null; chmod 644 "$1" 2>/dev/null; }
bisect_plugin_restore() {
    if [ -f "$BISECT_CONFBAK" ]; then mv -f "$BISECT_CONFBAK" "$BISECT_CONF" && bisect_own "$BISECT_CONF"
    elif [ -f "$BISECT_CONFBAK.absent" ]; then rm -f "$BISECT_CONF" "$BISECT_CONFBAK.absent"; fi
    if [ -f "$BISECT_ALLOWBAK" ]; then mv -f "$BISECT_ALLOWBAK" "$BISECT_ALLOW" && bisect_own "$BISECT_ALLOW"; fi
    return 0
}
bisect_conf_set() {   # bisect_conf_set "<lines>": the NVMTL_NO_SURFACE_* and NVMTL_TRACE lines replaced by these (the others stay)
    { grep -v -e '^NVMTL_NO_SURFACE_SHARE=' -e '^NVMTL_NO_SURFACE_PAGEOFF=' -e '^NVMTL_TRACE=' "$BISECT_CONF" 2>/dev/null
      for _cl in $1; do echo "$_cl"; done; true; } > "$BISECT_CONF.new" &&
        bisect_own "$BISECT_CONF.new" && mv -f "$BISECT_CONF.new" "$BISECT_CONF"
}
bisect_conf_original() {   # rdna3mtl.conf exactly as the run found it (the backup stays until the end)
    if [ -f "$BISECT_CONFBAK" ]; then cp "$BISECT_CONFBAK" "$BISECT_CONF" && bisect_own "$BISECT_CONF"
    elif [ -f "$BISECT_CONFBAK.absent" ]; then rm -f "$BISECT_CONF"; fi
}
bisect_allow_apps() {   # bisect_allow_apps off|on: the %apps line out of the list, or the list as it was
    if [ "$1" = off ]; then
        grep -v '^[[:space:]]*%apps[[:space:]]*$' "$BISECT_ALLOW" > "$BISECT_ALLOW.new" &&
            bisect_own "$BISECT_ALLOW.new" && mv -f "$BISECT_ALLOW.new" "$BISECT_ALLOW"
    else
        [ -f "$BISECT_ALLOWBAK" ] && cp "$BISECT_ALLOWBAK" "$BISECT_ALLOW" && bisect_own "$BISECT_ALLOW"
    fi
}
bisect_console_user() { if [ -n "${BISECT_USER:-}" ]; then echo "$BISECT_USER"; else stat -f %Su /dev/console 2>/dev/null; fi; }
bisect_wait_login() {   # after the restart: the old session ends, the login window shows, somebody logs in (300 s)
    [ -n "${BISECT_NOLOGINWAIT:-}" ] && return 0
    sleep 5
    _w=0
    while [ "$_w" -lt 20 ]; do
        case $(bisect_console_user) in ""|root|loginwindow) break ;; esac
        sleep 1; _w=$((_w + 1))
    done
    _w=0
    while [ "$_w" -lt 300 ]; do
        case $(bisect_console_user) in
            ""|root|loginwindow) ;;
            *) sleep 15; return 0 ;;
        esac
        if [ $((_w % 30)) = 0 ]; then
            echo "$(date +%T) bisect: waiting for the login ($_w s): log in at the login window"
            ( command -v say >/dev/null 2>&1 && say "log in now" ) >/dev/null 2>&1 &
        fi
        sleep 2; _w=$((_w + 2))
    done
    return 1
}
bisect_readback_plugin() {
    _c=$(grep -e '^NVMTL_NO_SURFACE' -e '^NVMTL_TRACE=' "$BISECT_CONF" 2>/dev/null | tr '\n' ' ' | sed 's/ $//')
    _a=$(grep -c '^[[:space:]]*%apps[[:space:]]*$' "$BISECT_ALLOW" 2>/dev/null)
    bisect_w "readback: conf ${_c:-(no NVMTL_NO_SURFACE_* or NVMTL_TRACE line)} allow-list %apps lines ${_a:-0}"
}
bisect_apply_plugin() {   # bisect_apply_plugin ACTION
    bisect_plugin_save
    case $1 in
        -) bisect_w "set: (nothing)" ;;
        off)
            bisect_w "set: rdna3mtl.conf NVMTL_NO_SURFACE_SHARE=1 NVMTL_NO_SURFACE_PAGEOFF=1 NVMTL_TRACE=0; allow list without %apps; WindowServer restart"
            bisect_conf_set "NVMTL_NO_SURFACE_SHARE=1 NVMTL_NO_SURFACE_PAGEOFF=1 NVMTL_TRACE=0" || bisect_w "failed: rdna3mtl.conf not written"
            bisect_allow_apps off || bisect_w "failed: allow list not written"
            bisect_restart_ws || return 1 ;;
        share)
            bisect_w "set: rdna3mtl.conf without the NVMTL_NO_SURFACE_* lines (NVMTL_TRACE=0 stays); WindowServer restart"
            bisect_conf_set "NVMTL_TRACE=0" || bisect_w "failed: rdna3mtl.conf not written"
            bisect_restart_ws || return 1 ;;
        apps)
            bisect_w "set: the allow list as it was (%apps back)"
            bisect_allow_apps on || bisect_w "failed: allow list not restored" ;;
        trace)
            bisect_w "set: rdna3mtl.conf as it was (NVMTL_TRACE back); WindowServer restart"
            bisect_conf_original || bisect_w "failed: rdna3mtl.conf not restored"
            bisect_restart_ws || return 1 ;;
    esac
    bisect_readback_plugin
    _kv=$(bisect_values_now)
    bisect_w "kext: $_kv"
    return 0
}
bisect_restart_ws() {   # the plugin conf reaches WindowServer only at its start: the session ends, the login window shows
    bisect_w "ws_restart: $(date +%T)"
    killall -HUP WindowServer 2>/dev/null || pkill -HUP -x WindowServer 2>/dev/null
    if bisect_wait_login; then bisect_w "login: ok ($(date +%T))"; return 0; fi
    bisect_w "ABORT: nobody logged in within 300 s after the WindowServer restart: the remaining steps were not run"
    bisect_say "BISECT: nobody logged in within 300 s - the plugin steps stop here"
    return 1
}

bisect_restore() {   # every switch back to what the boot left, the plugin's files as they were
    _i=0
    for _k in $BISECT_KNOBS; do
        _i=$((_i + 1)); _o=$(bisect_orig "$_i")
        [ "$_o" = - ] && continue
        [ "$_k" = iop ] && [ "$(sysctl -n debug.rdna3accel_iop 2>/dev/null)" = "$_o" ] && continue
        sysctl -w "debug.rdna3accel_$_k=$_o" >/dev/null 2>&1
    done
    bisect_plugin_restore
    # a run stopped while it waited for a key must not leave the Terminal without echo
    if [ -n "${_btty:-}" ] && [ -z "${BISECT_KEYFILE:-}" ]; then stty sane < /dev/tty 2>/dev/null; fi
    return 0
}

bisect_step() {   # bisect_step N KIND NAME VALUES REOPEN CHANGED LAST
    _n=$1; _kind=$2; _name=$3; _vals=$4; _reo=$5; _chg=$6; _last=$7
    _bans=; _bgo=; _bev=; _bkread=
    bisect_w "== step $_n"
    bisect_w "kind: $_kind"
    bisect_w "name: $_name"
    bisect_w "changed: $_chg"
    hdr "bisect step $_n of $_last: $_name"
    bisect_before
    _rc=0
    if [ "$_bv" = 2 ]; then bisect_apply_plugin "$_vals" || return 1; else bisect_apply_kext "$_vals" || _rc=1; fi
    [ "$_rc" = 0 ] || bisect_say "BISECT step $_n: a switch did NOT apply (see bisect.txt 'failed:')"
    [ "$_reo" = 1 ] && bisect_reopen_am
    _t0=$(date +%s)
    bisect_w "at: $(date +%T)"
    bisect_w "offset: $(bisect_mmss $((_t0 - BSTART)))"
    bisect_announce "$_n" "$_last" "$_name" "$_chg"
    bisect_wait $((_t0 + BEARLY))
    bisect_look "bisect${_n}a" 12
    bisect_wait $((_t0 + BMAIN))
    bisect_look "bisect${_n}" 20
    bisect_w "looks: bisect${_n}a bisect${_n}"
    bisect_counters
    bisect_trace
    # opt-in (BISECT_PLUGIN_SNAP=1): WindowServer's plugin trace after every step, in the background (one at a time).
    # Off by default: about 50 MB uploaded per step, and the copy + upload load the guest while the owner looks
    if [ "${BISECT_PLUGIN_SNAP:-0}" = 1 ]; then
        [ -n "${_bsnap:-}" ] && wait "$_bsnap" 2>/dev/null
        ( snap_plugin "bisect$_n" ) >> "$BD/plugin-snap.log" 2>&1 &
        _bsnap=$!
        bisect_w "plugin_snapshot: bisect$_n"
    fi
    _bev=1
    bisect_wait $((_t0 + HOLD))
    [ -n "${_bpid:-}" ] && wait "$_bpid" 2>/dev/null
    if [ -n "${_btty:-}" ]; then bisect_w "answer: ${_bans:-none}"; else bisect_w "answer: -"; fi
    bisect_w "== end step $_n"
    bisect_push
    return $_rc
}
bisect_push() {   # up to the host after every step: a run cut short keeps what it did
    [ -n "$HOST" ] || return 0
    curl -sf --max-time 10 -T "$BF" "$HOST/reports/$base-bisect.txt" >/dev/null 2>&1
    for _pp in "$HOME"/rdna3-scanout/bisect*/scanout-pipe*.ppm; do
        [ -f "$_pp" ] || continue
        [ -f "$BD/sent.$(basename "$(dirname "$_pp")").$(basename "$_pp")" ] && continue
        curl -sf --max-time 20 -T "$_pp" "$HOST/reports/$base-scanout$(basename "$(dirname "$_pp")")-$(basename "$_pp" | sed 's/^scanout-//')" >/dev/null 2>&1 &&
            : > "$BD/sent.$(basename "$(dirname "$_pp")").$(basename "$_pp")"
    done
    return 0
}

bisect_run() {   # bisect_run <1|2|4>: the whole sequence; 0 when every step ran and every switch took
    _bv=$1
    BD=${BISECT_OUT:-$HOME/rdna3-bisect}; BF=$BD/bisect.txt
    HOLD=${BISECT_HOLD:-30}
    BEARLY=$((HOLD * 8 / 30)); BMAIN=$((HOLD * 20 / 30))
    rm -rf "$BD"; mkdir -p "$BD" || { echo "== bisect: cannot make $BD"; return 1; }
    _btty=
    if [ -n "${BISECT_KEYFILE:-}" ]; then _btty=1
    elif [ -t 0 ] && ( : < /dev/tty ) 2>/dev/null; then _btty=1; fi
    case $_bv in 4) BISECT_KNOBS="$BISECT_KNOBS_BASE iop iop_flip flipbuf_present swap_gen" ;; *) BISECT_KNOBS=$BISECT_KNOBS_BASE ;; esac
    bisect_w "# rdna3 bisect v1"
    case $_bv in 2) _bvn=bisect-plugin ;; 4) _bvn=bisect-agdc ;; *) _bvn=bisect ;; esac
    bisect_w "variant: $_bv ($_bvn)"
    bisect_w "hold_s: $HOLD"
    bisect_w "mode: $([ -n "$_btty" ] && echo tty || echo timer)"
    bisect_w "started: $(date +%T)"
    bisect_w "console_user: $(bisect_console_user)"
    bisect_w "autologin: $(defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2>/dev/null || echo none)"
    if ! bisect_read_originals; then
        bisect_w "ABORT: no debug.rdna3accel_swap_mode sysctl: the accelerator is not running, or is older than card run 2026-10-17b"
        bisect_say "BISECT: NOT RUN - no debug.rdna3accel_swap_mode sysctl (accelerator down or too old)"
        return 1
    fi
    _o=; _i=0
    for _k in $BISECT_KNOBS; do _i=$((_i + 1)); _o="$_o $_k=$(bisect_orig "$_i")"; done
    bisect_w "originals:$_o"
    _o=; _i=0
    for _k in $BISECT_KNOBS; do _i=$((_i + 1)); [ "$(bisect_orig "$_i")" = - ] && _o="$_o $_k"; done
    [ -z "$_o" ] || bisect_w "absent:$_o"
    # a switch this kext does not have is no step's business; the table says so
    bisect_make_plan "$_bv"
    _last=$(tail -n 1 "$BD/plan.txt" | cut -d'|' -f1)
    # somebody must be logged in (a desktop that redraws), as in the swap A/B
    _w=0; _u=
    while [ "$_w" -lt 120 ]; do
        _u=$(bisect_console_user)
        case "$_u" in ""|root|loginwindow) sleep 5; _w=$((_w + 5)) ;; *) break ;; esac
    done
    case "$_u" in ""|root|loginwindow)
        bisect_w "WARN: nobody logged in after $_w s: nothing redraws, the looks prove little"
        bisect_say "BISECT: NOBODY LOGGED IN - log in; the steps start in $((${BISECT_LEAD:-20})) s anyway" ;; esac
    trap 'bisect_restore' EXIT
    trap 'bisect_restore; exit 130' INT
    trap 'bisect_restore; exit 143' TERM
    # the plugin variant restarts WindowServer, which closes a Terminal it was
    # started from: ignore the hangup so the steps and the restore still run
    [ "$_bv" = 2 ] && trap '' HUP
    caffeinate -u -t 5 >/dev/null 2>&1 &
    BSTART=$(( $(date +%s) + ${BISECT_LEAD:-20} ))
    bisect_timetable
    notify "bisect starts at $(bisect_clock "$BSTART"): $((_last + 1)) steps of $HOLD s - look at both monitors"
    bisect_wait "$BSTART"
    _fail=0
    # the plan is read on fd 3: a command inside the loop (curl, launchctl, osascript) must not eat it from stdin
    while IFS='|' read -r _pn _pk _pname _pasg _preo _pchg <&3; do
        bisect_step "$_pn" "$_pk" "$_pname" "$_pasg" "$_preo" "$_pchg" "$_last" || { _fail=1; [ "$_bv" = 2 ] && break; }
    done 3< "$BD/plan.txt"
    [ -n "${_bsnap:-}" ] && wait "$_bsnap" 2>/dev/null
    [ -s "$BD/plugin-snap.log" ] && cat "$BD/plugin-snap.log"
    bisect_restore
    bisect_w "restored: $(bisect_values_now)"
    sysctl -w debug.rdna3accel_trace_dump=600 >/dev/null 2>&1
    trace_snapshot bisect-end
    bisect_w "finished: $(date +%T)"
    bisect_w "== end"
    bisect_push
    trap - EXIT INT TERM
    [ -n "$STANDALONE" ] && trap 'restore_sleep' EXIT && trap 'restore_sleep; exit 130' INT && trap 'restore_sleep; exit 143' TERM
    echo "BISECT: $((_last + 1)) step(s) of $HOLD s, mode $([ -n "$_btty" ] && echo tty || echo timer), $([ "$_fail" = 0 ] && echo "every switch applied" || echo "A SWITCH DID NOT APPLY OR THE RUN STOPPED"); bisect.txt in the report"
    bisect_say "BISECT done: the report has the table (39-bisect.txt)"
    return $_fail
}
# BISECT-END

# LADDER-BEGIN (tests/test_ladder_step.sh runs this block, with the BISECT block's helpers, on the host)
# The A/B ladder of DRIVER-PLAN N50 in ONE card run (docs/AFTER-TESTS.md "Rendering artefacts: the A/B ladder"): the rungs that used to be a
# conf line, a restart and a report each. `rdna3vm step ladder` (boot-arg rdna3ladder=1: the play step's boot-args, no desktop to drive) or by
# hand: sudo sh autoreport.sh --standalone --ladder. Per rung: Terminal gets text (a colored listing, then a line a second so that it draws
# all the time) and Chess a game, both as NEW instances (`open -n -a APP --env NVMTL_X=1`: the plugin reads its switches from the environment
# of the process as well as from rdna3mtl.conf, the environment wins), then a 320-wide and a 1280-wide scanout look and the plugin log of
# exactly those processes (log show --start, by pid), labelled by rung. Nothing is written to rdna3mtl.conf and WindowServer is not restarted
# for the app rungs. `rdna3vm step ladder-ws` (rdna3ladder=2, standalone --ladder-ws) adds rungs whose switch must reach WindowServer
# (radv-ws, paranoid-ws): rdna3mtl.conf edited, WindowServer restarted through the bisect-plugin machinery (bisect_restart_ws: the login
# window shows, log in), the conf put back exactly (bisect_plugin_save / bisect_plugin_restore, also on an interrupt, and by the top of this
# script after a dead run). Evidence: $HOME/rdna3-ladder/ladder.txt (collect.sh: 41-ladder.txt) and <rung>.log (41-ladder-<rung>.log);
# tools/triage_reports.py (tools/triage_glyphs.py ladder_rows) prints one row per rung.
LADDER_RUNGS="baseline radv sync managed alignguard paranoid loadbarrier"
LADDER_WS_RUNGS="radv-ws paranoid-ws"
# variant 3 (`rdna3vm step ladder-glyphs`, rdna3ladder=3): the glyph hunt of DRIVER-PLAN N50 after card run 2026-10-25f, where NVMTL_PARANOID did not cure Terminal's text. Strongest
# first (a run cut short keeps them): `baseline`; the managed-buffer rungs (hypothesis A: the app writes bytes outside its didModifyRange, Apple's drivers copy whole pages):
# `managed-whole` (every flush uploads the whole buffer), `managed-check` (the undeclared-write detector, no behaviour change); the IOSurface coherency rungs (hypothesis C):
# `no-surface-share` (the window is a private image, not the family's VRAM), `no-surface-pageoff` (a GPU write is copied out eagerly, no VRAM-newest mark); then `fbfetch-zero`
# (framebuffer fetch returns zero: if the text then has no half-blended strokes, the fetch path is where it breaks), `managed-pages` (ranges widened to 4 KiB pages),
# `managed-eager-zero` (=1: the OLD behaviour, a new managed buffer is memset and uploaded; the default clears the VRAM copy on the GPU), `no-texbuf` (no texel-buffer views),
# `no-bindless`, `dontcare-clears` (a don't-care load action is a clear), `skip-empty-0` (empty encoders are not skipped), and `paranoid`, the NEGATIVE control (card run
# 2026-10-25f: it does not clean the text; if it does here, the run is not comparable). Each rung's knob is the code's own (checked against getenv in metal/plugin): the value is the one the code reads
# (NO_SURFACE_SHARE / NO_SURFACE_PAGEOFF / MANAGED_EAGER_ZERO test the first character for '1', NO_BINDLESS likewise, NO_TEXBUF is set-or-not, SKIP_EMPTY is on unless it starts with '0').
# This variant takes a SECOND hi-res look a few seconds after the first (`h2`): the verdict (tools/glyph_verdict.py) only counts the tick-line window that CHANGED between the two,
# the rung's own live text, never a window that stands still (a restored one, another rung's, a frozen screen).
LADDER_GLYPHS_RUNGS="baseline managed-whole managed-check no-surface-share no-surface-pageoff fbfetch-zero managed-pages managed-eager-zero no-texbuf no-bindless dontcare-clears skip-empty-0 paranoid"
ladder_rung_env() {   # the NAME=value switches of a rung, blank separated
    case $1 in
        baseline) ;;
        radv|radv-ws) echo "NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears" ;;
        sync) echo "NVMTL_ASYNC_COMMIT=0" ;;
        managed) echo "NVMTL_MANAGED_CHECK=1 NVMTL_MANAGED_SNAPSHOT=1" ;;
        alignguard) echo "NVMTL_NO_ALIGN_GUARD=1" ;;
        paranoid|paranoid-ws) echo "NVMTL_PARANOID=1" ;;
        loadbarrier) echo "NVMTL_LOAD_BARRIER=1" ;;
        managed-whole) echo "NVMTL_MANAGED_WHOLE=1" ;;
        managed-check) echo "NVMTL_MANAGED_CHECK=1" ;;
        no-surface-share) echo "NVMTL_NO_SURFACE_SHARE=1" ;;
        no-surface-pageoff) echo "NVMTL_NO_SURFACE_PAGEOFF=1" ;;
        fbfetch-zero) echo "NVMTL_FBFETCH_ZERO=1" ;;
        managed-pages) echo "NVMTL_MANAGED_PAGES=1" ;;
        managed-eager-zero) echo "NVMTL_MANAGED_EAGER_ZERO=1" ;;
        no-texbuf) echo "NVMTL_NO_TEXBUF=1" ;;
        no-bindless) echo "NVMTL_NO_BINDLESS=1" ;;
        dontcare-clears) echo "NVMTL_DONTCARE_CLEARS=1" ;;
        skip-empty-0) echo "NVMTL_SKIP_EMPTY=0" ;;
        *) return 1 ;;
    esac
}
ladder_env_args() {   # "A=1 B=2" -> "--env A=1 --env B=2"
    for _le in $1; do printf '%s %s ' --env "$_le"; done
}
ladder_w() { printf '%s\n' "$*" >> "$LF"; }
ladder_text_script() {   # the Terminal's text: a colored listing and fox lines, then a tick line a second in ONE fixed colour (bright white) and, every 4th second, a line of mixed glyphs
    # made once per run, as root, in a directory of its own that mktemp creates (a fixed name in the sticky /Users/Shared could be a planted link);
    # ladder_restore removes it. LADDER_TEXT stands in for it in the host test.
    # The tick lines are the glyph check's evidence (tools/glyph_verdict.py): the text after the time stamp is the same on every line, and in one colour
    # every line is drawn the same when the glyphs are drawn right, so a line that has no twin is a broken line. The mixed line (digits, upper case, punctuation)
    # is for the eye; it ends differently from the tick lines and the verdict leaves it out.
    if [ -n "${LADDER_TEXT:-}" ]; then _lt=$LADDER_TEXT
    else
        if [ -z "${_LADDER_TEXTDIR:-}" ]; then
            _LADDER_TEXTDIR=$(mktemp -d /Users/Shared/rdna3-ladder-text.XXXXXX) || { echo "ladder: cannot make a directory for the text script" >&2; return 1; }
            chmod 755 "$_LADDER_TEXTDIR"
        fi
        _lt=$_LADDER_TEXTDIR/text.command
    fi
    [ -L "$_lt" ] && rm -f "$_lt"
    cat > "$_lt" <<'TXT'
#!/bin/sh
clear
echo "RDNA3 ladder: text for the glyph check (do not close, this window is closed by the run)"
CLICOLOR_FORCE=1 ls -laG /System/Library /usr/bin 2>/dev/null | head -n 70
i=0
while [ $i -lt 8 ]; do printf '\033[%sm The quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ \033[0m\n' $((31 + i)); i=$((i + 1)); done
i=0
while :; do
    i=$((i + 1))
    printf '\033[97m %s  rdna3 ladder tick  abcdefghijklmnopqrstuvwxyz\033[0m\n' "$(date +%T)"
    [ $((i % 4)) -eq 0 ] && printf '\033[97m          ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 !#$%%&()*+,-./:;<=>?@[]^_{|}~\033[0m\n'
    sleep 1
done
TXT
    chmod 755 "$_lt" 2>/dev/null
    _LADDER_TEXT=$_lt   # (a global: this is not called inside $( ), where _LADDER_TEXTDIR would be lost)
    return 0
}
ladder_wait_new() {   # ladder_wait_new NAME "<pids before>": the new pids of NAME, polled up to LADDER_LAUNCHWAIT s (the window in which a pid could be missed is the poll)
    _wn_i=0
    while :; do
        _wn=$(ladder_new_pids "$1" "$2")
        if [ -n "$_wn" ] || [ "$_wn_i" -ge "${LADDER_LAUNCHWAIT:-8}" ]; then break; fi
        sleep 1; _wn_i=$((_wn_i + 1))
    done
    echo "$_wn"
}
ladder_sweep() {   # Terminal / Chess instances that are not in the run's starting set: started by the ladder and missed by its pid capture
    for _sa in Terminal Chess; do
        eval "_sb=\${_LADDER_BASE_$_sa:-}"
        _sn=$(ladder_new_pids "$_sa" "$_sb")
        [ -n "$_sn" ] || continue
        ladder_w "sweep: $_sa instance(s) $_sn started after the run began and not captured: quit"
        ladder_quit "$_sn"
    done
}
ladder_new_pids() {   # ladder_new_pids NAME "<pids before>": the pids of NAME that are new
    for _lp in $(pgrep -x "$1" 2>/dev/null); do
        case " $2 " in *" $_lp "*) ;; *) printf '%s ' "$_lp" ;; esac
    done
}
ladder_look_hires() {   # ladder_look_hires NAME: the 1280-wide look (small text-cell rectangles and broken glyphs are too small at 320)
    ladder_w "-- look $1 (hires, $(date +%T))"
    if [ ! -x "$D/bin/rdna3_drminfo" ]; then ladder_w "NOT TAKEN: no $D/bin/rdna3_drminfo"; return 1; fi
    mkdir -p "$HOME/rdna3-scanout/$1"
    with_limit 90 "$D/bin/rdna3_drminfo" --scanout-hires "$HOME/rdna3-scanout/$1" > "$LD/look.out" 2>&1
    _lr=$?
    grep -E '^pipe [0-9]+: |pipe\(s\) lit|asleep' "$LD/look.out" | cut -c1-140 >> "$LF"
    ladder_w "exit: $_lr"
}
ladder_log_slice() {   # ladder_log_slice RUNG START_EPOCH "<pids>": the plugin's log lines of those processes since the rung began (the LAST 600000 bytes: a flood keeps the end, where the SUMMARY lines are)
    _ls_start=$(date -r "$2" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date -d "@$2" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
    _ls_pred='(eventMessage CONTAINS "NVMTL" OR eventMessage CONTAINS "NullMoth:") AND ('
    _ls_or=
    for _lp in $3; do _ls_pred="$_ls_pred${_ls_or}processID == $_lp"; _ls_or=" OR "; done
    _ls_pred="$_ls_pred)"
    : > "$LD/$1.full"
    with_limit 90 sh -c "log show --start \"$_ls_start\" --info --style compact --predicate '$_ls_pred' 2>/dev/null | grep -v -e 'lazy air' -e sizequery > \"$LD/$1.full\"" 2>/dev/null
    _ls_rc=$?
    _ls_sz=$(wc -c < "$LD/$1.full" | tr -d ' ')
    _ls_note=
    # a log show killed by the 90 s limit ends mid-stream: its end is not the rung's end (the last SUMMARY lines may be missing)
    [ "$_ls_rc" = 0 ] || _ls_note=", log show did not finish in 90 s (exit $_ls_rc): the slice ends early"
    if [ "${_ls_sz:-0}" -gt 600000 ]; then
        tail -c 600000 "$LD/$1.full" > "$LD/$1.log"
        _ls_note="$_ls_note, slice truncated: kept the last 600000 of $_ls_sz bytes"
    else
        cp "$LD/$1.full" "$LD/$1.log"
    fi
    rm -f "$LD/$1.full"
    ladder_w "log: $1.log ($(wc -l < "$LD/$1.log" | tr -d ' ') lines, pids $3, since $_ls_start$_ls_note)"
}
ladder_ws_conf() {   # ladder_ws_conf "<NAME=value ...>": rdna3mtl.conf = the run's original without these keys, then these lines (the original stays in the backup)
    bisect_plugin_save
    {
        if [ -f "$BISECT_CONFBAK" ]; then
            # the original without any line of these keys, as the plugin's parser reads them: blanks before the key and around the '=' are ignored
            _lk=; for _le in $1; do _lk="$_lk ${_le%%=*}"; done
            awk -v keys="$_lk" 'BEGIN { n = split(keys, K, " ") }
                { l = $0; sub(/^[ \t]+/, "", l); if (l ~ /=/) { k = l; sub(/[ \t]*=.*/, "", k); for (i = 1; i <= n; i++) if (k == K[i]) next } print }' "$BISECT_CONFBAK" 2>/dev/null
        fi
        for _le in $1; do echo "$_le"; done
        true
    } > "$BISECT_CONF.new" && bisect_own "$BISECT_CONF.new" && mv -f "$BISECT_CONF.new" "$BISECT_CONF"
}
ladder_quit_w() { [ -n "${LF:-}" ] && ladder_w "$*"; return 0; }   # (the restore path of a run that died before its evidence file existed has nowhere to write)
ladder_quit() {   # ladder_quit "<pids>" [graceful]: the instances this rung started, and only those
    # SIGINT, then SIGTERM, then SIGKILL, each followed by a wait, and a line per instance saying how it ended. No System Events: that needs the user's consent (Automation), which
    # card run 2026-10-25f never got (`quit: still running after 8s` in every rung). A signal ends a GUI process without its atexit, so the plugin's exit SUMMARY is lost; its 10 s
    # time-based lines are the evidence (the rung waits LADDER_TAIL s before it quits). The restore path of an interrupted run skips the polite INT.
    _qleft=$1
    [ -n "$_qleft" ] || return 0
    # end the text script first: Terminal asks "terminate running processes?" on a quit while it runs
    if [ "${2:-}" = graceful ] && [ -n "${_LADDER_TEXT:-}" ]; then pkill -f "$_LADDER_TEXT" 2>/dev/null; sleep 1; fi
    for _qsig in INT TERM KILL; do
        [ -n "$_qleft" ] || break
        if [ "$_qsig" = INT ] && [ "${2:-}" != graceful ]; then continue; fi
        case $_qsig in INT) _qlim=${LADDER_QUITGRACE:-4} ;; TERM) _qlim=${LADDER_QUITTERM:-3} ;; *) _qlim=${LADDER_QUITWAIT:-2} ;; esac
        kill -"$_qsig" $_qleft 2>/dev/null
        _qw=0
        while :; do
            _qn=
            for _qp in $_qleft; do
                if kill -0 "$_qp" 2>/dev/null; then _qn="$_qn $_qp"; else ladder_quit_w "quit: pid $_qp ended by SIG$_qsig after ${_qw}s"; fi
            done
            _qleft=${_qn# }
            if [ -z "$_qleft" ] || [ "$_qw" -ge "$_qlim" ]; then break; fi
            sleep 1; _qw=$((_qw + 1))
        done
    done
    [ -z "$_qleft" ] || ladder_quit_w "quit: pid(s) $_qleft SURVIVED SIGKILL"
    return 0
}
ladder_rung() {   # ladder_rung NAME SCOPE: one rung's evidence in $LF
    _rn=$1; _rs=$2; _LADDER_CUR=$_rn
    _renv=$(ladder_rung_env "$_rn") || { ladder_w "== rung $_rn: unknown"; return 1; }
    ladder_w "== rung $_rn"
    ladder_w "scope: $_rs"
    ladder_w "conf: ${_renv:-(nothing)}"
    hdr "ladder rung $_rn ($_rs): ${_renv:-the plugin as it is}"
    _rargs=
    if [ "$_rs" = windowserver ]; then
        ladder_w "set: rdna3mtl.conf $_renv; WindowServer restart"
        ladder_ws_conf "$_renv" || ladder_w "failed: rdna3mtl.conf not written"
        _LADDER_WS=1   # (before the restart: if it fails, the conf is still put back and WindowServer restarted once more at the end)
        bisect_restart_ws || { ladder_w "failed: WindowServer did not come back with a login; the conf is put back at the end and WindowServer restarted once more"; return 1; }
    else
        _rargs=$(ladder_env_args "$_renv")
        ladder_w "env: ${_rargs:-(nothing)}"
    fi
    _rt0=$(date +%s)
    ladder_w "started: $(date +%T)"
    # each app's instances are listed right before its own open and polled for right after it: the window in which another instance could be taken for ours
    _tb=$(pgrep -x Terminal 2>/dev/null | tr '\n' ' ')
    # shellcheck disable=SC2086
    # -ApplePersistenceIgnoreState: a new Terminal instance restores the windows the previous one had saved (card run 2026-10-25f: the text of the rung before,
    # static and drawn clean, next to the live window, which looked like a clean picture in one rung); without it the only text on screen is this rung's
    as_user open -n -a Terminal $_rargs "$_LADDER_TEXT" --args -ApplePersistenceIgnoreState YES >/dev/null 2>&1
    _tp=$(ladder_wait_new Terminal "$_tb")
    _cb=$(pgrep -x Chess 2>/dev/null | tr '\n' ' ')
    # shellcheck disable=SC2086
    as_user open -n -a Chess $_rargs >/dev/null 2>&1
    _cp=$(ladder_wait_new Chess "$_cb")
    _LADDER_PIDS="$_tp$_cp"
    ladder_w "pids: Terminal=${_tp:-none}Chess=${_cp:-none}"
    [ -n "$_tp" ] && [ -n "$_cp" ] || ladder_w "WARN: an app did not start (Terminal: ${_tp:-no} Chess: ${_cp:-no}): the rung proves little"
    sleep "${LADDER_SETTLE:-25}"
    bisect_look "ladder-${_rn}a" 12
    ladder_look_hires "ladder-${_rn}h"
    _rlooks="ladder-${_rn}a ladder-${_rn}h"
    if [ "${_LADDER_V:-1}" = 3 ]; then
        # the glyph hunt: a second hi-res look a few seconds later. The text script prints a line a second, so the rung's own window CHANGES between the two looks and
        # a window that does not (restored, another rung's, a frozen screen) is not the rung's text (tools/glyph_verdict.py verdict_files(later=...))
        sleep "${LADDER_LOOK2GAP:-3}"
        ladder_look_hires "ladder-${_rn}h2"
        _rlooks="$_rlooks ladder-${_rn}h2"
    fi
    ladder_w "looks: $_rlooks"
    _rwait=
    if [ -n "${_btty:-}" ]; then
        bisect_say "LADDER rung $_rn: is the picture right? y / n / Enter (20 s)"
        _rk=$(bisect_key 20); _rwait=${_rk:-}
    fi
    case $_rwait in y) _rans=y ;; n) _rans=n ;; *) _rans=- ;; esac
    ladder_w "answer: $_rans"
    sleep "${LADDER_TAIL:-12}"   # the plugin's counters go out every 10 s while something moved (`vk: ab: SUMMARY`, `managed: SUMMARY`)
    _lwp=$([ "$_rs" = windowserver ] && pgrep -x WindowServer 2>/dev/null | tr '\n' ' ')
    ladder_quit "$_tp$_cp" graceful   # SIGINT, TERM, KILL (no consent needed); the slice below holds the last 10 s SUMMARY of each process (a signal skips the exit one)
    _LADDER_PIDS=
    sleep "${LADDER_AFTERQUIT:-2}"
    ladder_log_slice "$_rn" "$_rt0" "$_tp$_cp$_lwp"
    ladder_w "== end rung $_rn"
    ladder_push
    return 0
}
ladder_push() {   # up to the host after every rung: a run cut short keeps what it did. A file that did not go up is counted in ladder.txt (the triage then cannot judge that rung's glyphs)
    [ -n "$HOST" ] || return 0
    _pu_miss=0
    for _pp in "$HOME"/rdna3-scanout/ladder-*/scanout-pipe*.ppm; do
        [ -f "$_pp" ] || continue
        [ -f "$LD/sent.$(basename "$(dirname "$_pp")").$(basename "$_pp")" ] && continue
        if curl -sf --max-time 20 -T "$_pp" "$HOST/reports/$base-scanout$(basename "$(dirname "$_pp")")-$(basename "$_pp" | sed 's/^scanout-//')" >/dev/null 2>&1; then
            : > "$LD/sent.$(basename "$(dirname "$_pp")").$(basename "$_pp")"
        else
            _pu_miss=$((_pu_miss + 1))
        fi
    done
    [ "$_pu_miss" = 0 ] || ladder_w "push: $_pu_miss ppm file(s) not sent to the host (curl failed): their rungs have no glyph verdict in the report"
    curl -sf --max-time 10 -T "$LF" "$HOST/reports/$base-ladder.txt" >/dev/null 2>&1
    return 0
}
ladder_restore() {   # the plugin's conf and allow list exactly as the run found them; the instances still running are quit
    [ -n "${_LADDER_PIDS:-}" ] && ladder_quit "$_LADDER_PIDS"
    _LADDER_PIDS=
    [ -n "${_LADDER_BASE_SET:-}" ] && [ -z "${_LADDER_WS:-}" ] && ladder_sweep   # (not after a WindowServer restart: the session's apps are all new then)
    bisect_plugin_restore
    if [ -n "${_LADDER_TEXTDIR:-}" ]; then rm -rf "${_LADDER_TEXTDIR:?}"; _LADDER_TEXTDIR=; fi
    [ -n "${_LADDER_WS:-}" ] && [ -n "${LF:-}" ] && ladder_w "restored: rdna3mtl.conf as the run found it (a WindowServer started during a rung keeps that rung's conf until it restarts)"
    if [ -n "${_btty:-}" ] && [ -z "${BISECT_KEYFILE:-}" ]; then stty sane < /dev/tty 2>/dev/null; fi
    return 0
}
ladder_abort() {   # ladder_abort SIGNAME EXITCODE: the run is ending because of a signal
    trap '' INT TERM HUP   # a second signal must not run the restore and the push again
    ladder_w "ABORTED by SIG$1 at rung ${_LADDER_CUR:-none} ($(date +%T)): the rungs after it did not run"
    ladder_restore
    ladder_push
    trap - EXIT
    exit "$2"
}
ladder_run() {   # ladder_run <1|2|3>: 1 = the app rungs, 2 = and the WindowServer rungs, 3 = the glyph hunt (ladder-glyphs); 0 when every rung ran
    _lv=$1; _LADDER_V=$1
    _lrungs=$LADDER_RUNGS; [ "$_lv" = 3 ] && _lrungs=$LADDER_GLYPHS_RUNGS
    LD=${LADDER_OUT:-$HOME/rdna3-ladder}; LF=$LD/ladder.txt; BD=$LD; BF=$LF
    rm -rf "$LD"; mkdir -p "$LD" || { echo "== ladder: cannot make $LD"; return 1; }
    _btty=
    if [ -n "${BISECT_KEYFILE:-}" ]; then _btty=1
    elif [ -t 0 ] && ( : < /dev/tty ) 2>/dev/null; then _btty=1; fi
    _LADDER_WS=; _LADDER_PIDS=; _LADDER_TEXTDIR=; _LADDER_BASE_SET=; _LADDER_CUR=
    ladder_w "# rdna3 ladder v1"
    ladder_w "variant: $_lv ($(case $_lv in 2) echo ladder-ws ;; 3) echo ladder-glyphs ;; *) echo ladder ;; esac))"
    ladder_w "started: $(date +%T)"
    ladder_w "mode: $([ -n "$_btty" ] && echo tty || echo timer)"
    ladder_w "console_user: $(bisect_console_user)"
    ladder_w "conf_before: $(tr '\n' ';' < "$BISECT_CONF" 2>/dev/null | cut -c1-300)"
    # somebody must be logged in: the apps open in the console user's session
    _w=0; _u=
    while [ "$_w" -lt 120 ]; do
        _u=$(bisect_console_user)
        case "$_u" in ""|root|loginwindow) sleep 5; _w=$((_w + 5)) ;; *) break ;; esac
    done
    case "$_u" in ""|root|loginwindow)
        ladder_w "ABORT: nobody logged in after $_w s: no app can be opened"
        bisect_say "LADDER: NOBODY LOGGED IN - nothing ran"
        return 1 ;; esac
    # a signal ends the run with a line that says so, the evidence pushed and the conf put back. HUP too (the Terminal this was started from was closed), except in the WindowServer
    # variant, where the restart of WindowServer closes that Terminal on purpose and the run must go on
    trap 'ladder_restore' EXIT
    trap 'ladder_abort INT 130' INT
    trap 'ladder_abort TERM 143' TERM
    if [ "$_lv" = 2 ]; then trap '' HUP; else trap 'ladder_abort HUP 129' HUP; fi
    # the instances that were there before the run: never ours to quit
    _LADDER_BASE_Terminal=$(pgrep -x Terminal 2>/dev/null | tr '\n' ' '); _LADDER_BASE_Chess=$(pgrep -x Chess 2>/dev/null | tr '\n' ' '); _LADDER_BASE_SET=1
    ladder_w "baseline_pids: Terminal=${_LADDER_BASE_Terminal:-none}Chess=${_LADDER_BASE_Chess:-none}"
    # tick windows that were already there: the text script of an earlier run still printing its lines in some Terminal. They are live too, so the glyph verdict could judge them as well as ours
    _old_ticks=$(pgrep -f 'rdna3-ladder-text\.' 2>/dev/null | tr '\n' ' ')
    [ -z "$_old_ticks" ] || ladder_w "WARN: the text script of an earlier run is still running (pids $_old_ticks): its Terminal window shows tick lines too and is as live as this run's; close it and run again if a verdict says STATIC/live windows"
    ladder_text_script || { ladder_w "ABORT: no text script"; ladder_restore; trap - EXIT INT TERM HUP; return 1; }
    caffeinate -u -t 5 >/dev/null 2>&1 &
    notify "ladder: $(echo $_lrungs | wc -w | tr -d ' ') rungs, Terminal and Chess open and close by themselves - look at the screen"
    _lfail=0
    for _lr in $_lrungs; do ladder_rung "$_lr" apps || _lfail=1; done
    if [ "$_lv" = 2 ]; then
        for _lr in $LADDER_WS_RUNGS; do ladder_rung "$_lr" windowserver || { _lfail=1; break; }; done
    fi
    ladder_restore
    # the plugin's files are back; a WindowServer that ran with a rung's conf is restarted once more so that the desktop is the default again
    [ -n "$_LADDER_WS" ] && [ -z "${LADDER_NO_FINAL_RESTART:-}" ] && { ladder_w "ws_restart: final ($(date +%T))"; bisect_restart_ws; }
    ladder_w "conf_after: $(tr '\n' ';' < "$BISECT_CONF" 2>/dev/null | cut -c1-300)"
    ladder_w "finished: $(date +%T)"
    ladder_w "== end"
    ladder_push
    trap - EXIT INT TERM HUP
    [ -n "$STANDALONE" ] && trap 'restore_sleep' EXIT && trap 'restore_sleep; exit 130' INT && trap 'restore_sleep; exit 143' TERM
    echo "LADDER: $_lv, $([ "$_lfail" = 0 ] && echo "every rung ran" || echo "A RUNG DID NOT RUN"); ladder.txt in the report"
    bisect_say "LADDER done: the report has the table (41-ladder.txt)"
    return $_lfail
}
# LADDER-END

# an M6 hi-res look keeps only its picture; the pipes' state stays in the
# results (a pipe not lit or asleep must not look like a frozen picture)
m6_look_state() {
    grep -E '^pipe [0-9]+: |pipe\(s\) lit|asleep' | cut -c1-140
}

# the progress window as soon as somebody is logged in, with the time the
# tests start: the 90 s pause after boot (the desktop and the driver
# settle first) looked like nothing was happening
early_window() {
    w=0
    while [ $w -lt 60 ]; do
        u=$(stat -f %Su /dev/console 2>/dev/null)
        [ -n "$u" ] && [ "$u" != root ] && [ "$u" != loginwindow ] && break
        sleep 2; w=$((w + 2))
    done
    uid=$(id -u "$u" 2>/dev/null)
    [ -n "$uid" ] && [ "$u" != root ] && [ "$u" != loginwindow ] || return 0
    P=/Users/Shared/rdna3-progress.log
    echo "$(date +%H:%M:%S)  macOS is up; the tests start at $(date -v+$((90 - w))S +%H:%M:%S) (the desktop settles first)" > "$P"
    chmod 644 "$P"
    printf '#!/bin/sh\nclear\necho "RDNA3 test progress - this window can be closed"\necho\nexec tail -n +1 -F %s\n' \
        "$P" > /Users/Shared/rdna3-progress.command
    chmod 755 /Users/Shared/rdna3-progress.command
    launchctl asuser "$uid" sudo -u "$u" open -a Terminal /Users/Shared/rdna3-progress.command >/dev/null 2>&1 &&
        touch /tmp/rdna3-progress.win
}

# rdna3vminstall=1: the host booted macOS without the card only to install
# its new bundle (rdna3vm _install): no tests, no desktop to wait for -
# install as soon as the host answers, then shut down
INSTALL_ONLY=
case " $(nvram boot-args 2>/dev/null | cut -f2-) " in *" rdna3vminstall=1 "*) INSTALL_ONLY=1 ;; esac
# rdna3vmapprove=1: 'rdna3vm approve' in a window without the card: open
# Privacy & Security for "Allow" even when the bundle was installed before
# (by the install boot), then stop - no tests without the card
APPROVE=
case " $(nvram boot-args 2>/dev/null | cut -f2-) " in *" rdna3vmapprove=1 "*) APPROVE=1 ;; esac

if [ "${1:-}" != --updated ] && [ -z "$STANDALONE" ] && [ -z "$BISECT_HAND" ]; then
    rm -f /tmp/rdna3-progress.win
    # the plugin's trace ring knob on every boot of the VM (the install boot
# included: the card boot's WindowServer then starts with the big ring)
[ -z "$STANDALONE" ] && plugin_trace_conf
# DRIVER-PLAN N44: on every boot, the install boot included (its output is not kept: the
# tests file of the card boot prints the state again)
[ -z "$STANDALONE" ] && coredisplay_iop_prefs >/dev/null 2>&1
if [ -n "$INSTALL_ONLY" ]; then
        w=0
        until curl -sf --max-time 5 -o /dev/null "$HOST/rdna3-bundle.sha256" || [ $w -ge 120 ]; do
            sleep 5; w=$((w + 5))
        done
    else
        early_window
        sleep $((90 - ${w:-0}))
    fi
    # newest report scripts from the host (rdna3vm start_share); a new
    # autoreport.sh takes over right away
    for f in collect.sh ioreg_discovery.sh collect_plugin_trace.sh fix_tracedir.sh autoreport.sh; do
        case $f in autoreport.sh) dst="$D/$f" ;; *) dst="$D/tools/macos/$f" ;; esac
        curl -sf --max-time 10 "$HOST/tools/$f" -o "$dst.new" && [ -s "$dst.new" ] &&
            { cmp -s "$dst.new" "$dst" || { mv "$dst.new" "$dst"; [ $f = autoreport.sh ] && exec sh "$dst" --updated; }; }
        rm -f "$dst.new"
    done
fi

# the framebuffer / accelerator macOS runs vs the ones installed in
# /Library/Extensions (RDNA3SourceHash, written by fb/ and accel/Makefile):
# a line per kext whose new build waits for the user's approval
stale_kexts() {
    for n in RDNA3Framebuffer:FBSource RDNA3Accelerator:AccelSource RDNA3AGDC:AGDCSource; do
        k=${n%%:*}; key=${n#*:}
        h=$(/usr/libexec/PlistBuddy -c "Print :RDNA3SourceHash" "/Library/Extensions/$k.kext/Contents/Info.plist" 2>/dev/null) ||
            continue
        r=$(ioreg -l -w0 -r -c "$k" -d 1 2>/dev/null | sed -n "s/.*\"RDNA3,$key\" = \"\([0-9a-f]*\)\".*/\1/p" | head -n 1)
        [ -n "$r" ] && [ -n "$h" ] && [ "$r" != "$h" ] && echo "$k: running $r, installed $h (not approved yet)"
    done
}

# a new build on the host (rdna3vm prepare): install its kexts like
# setup.sh does. OpenCore injects RDNA3Accel itself, but the framebuffer
# and the accelerator link IOGraphicsFamily, so they load from the
# Auxiliary KC, which kernelmanagerd rebuilds from /Library/Extensions:
# the new copies run from the next boot on.
updated=
changed=
new=$([ -n "$HOST" ] && curl -sf --max-time 10 "$HOST/rdna3-bundle.sha256" | cut -c1-64)
if [ -n "$new" ] && [ "$new" != "$(cat "$D/bundle.sha256" 2>/dev/null)" ]; then
    t=$(mktemp -d /tmp/rdna3b.XXXXXX)
    if curl -sf --max-time 120 "$HOST/rdna3-bundle.tar.gz" | tar -xz -C "$t" && [ -d "$t/kexts" ]; then
        # one accelerator per macOS: the Tahoe build (RDNA3AcceleratorTahoe)
        # under its own name's place on Darwin 25+, the other below
        tahoe=$([ "$(uname -r | cut -d. -f1)" -ge 25 ] && echo 1 || echo 0)
        if [ -d "$t/kexts/RDNA3AcceleratorTahoe.kext" ]; then
            if [ "$tahoe" = 1 ]; then
                rm -rf "$t/kexts/RDNA3Accelerator.kext"
                mv "$t/kexts/RDNA3AcceleratorTahoe.kext" "$t/kexts/RDNA3Accelerator.kext"
            else
                rm -rf "$t/kexts/RDNA3AcceleratorTahoe.kext"
            fi
        fi
        changed=
        for k in "$t"/kexts/*.kext; do
            n=$(basename "$k")
            # the same build as the one installed (the bundle's hash changes
            # with every 'rdna3vm prepare', its kexts not): left alone, so
            # macOS does not ask to allow it again (card run 44: a boot that
            # already ran the approved build reinstalled it and stopped).
            # Same build = the same RDNA3SourceHash (fb/, accel/, agdc/
            # Makefile): a CI run of the same sources links a binary that
            # differs (card run 45); kexts without it: the same files
            nh=$(/usr/libexec/PlistBuddy -c "Print :RDNA3SourceHash" "$k/Contents/Info.plist" 2>/dev/null)
            oh=$(/usr/libexec/PlistBuddy -c "Print :RDNA3SourceHash" "/Library/Extensions/$n/Contents/Info.plist" 2>/dev/null)
            if [ -d "/Library/Extensions/$n" ]; then
                if [ -n "$nh" ]; then
                    [ "$nh" = "$oh" ] && continue
                elif diff -rq "$k" "$D/kexts/$n" >/dev/null 2>&1; then
                    continue
                fi
            fi
            changed="$changed ${n%.kext}"
            rm -rf "/Library/Extensions/$n" "$D/kexts/$n"
            mkdir -p "$D/kexts"
            cp -R "$k" "$D/kexts/"
            cp -R "$k" /Library/Extensions/
            chown -R root:wheel "/Library/Extensions/$n"
            chmod -R go-w "/Library/Extensions/$n"
            codesign --force --deep --sign - "/Library/Extensions/$n" >/dev/null 2>&1
            # the new build loads from the next boot (Auxiliary KC); a load
            # now of a kext that runs terminated the running instance while
            # WindowServer closed its surfaces at the shutdown that follows
            # (card run 50: kernel panic in IOAccelLegacySurface::clientClose)
            bid=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$k/Contents/Info.plist" 2>/dev/null)
            if [ -n "$bid" ] && kmutil showloaded --list-only 2>/dev/null | grep -q "$bid" ||
               { [ -n "$bid" ] && kextstat -l 2>/dev/null | grep -q "$bid"; }; then
                :
            else
                kmutil load -p "/Library/Extensions/$n" >/dev/null 2>&1
            fi
        done
        mkdir -p "$D/bin"
        cp "$t"/bin/* "$D/bin/" 2>/dev/null && chmod +x "$D/bin/"*
        # a RADV from the host's bundle (a CI artifact) replaced the one
        # build_radv.sh stamped: its stamp no longer describes it
        [ -f "$t/bin/libvulkan_radeon.dylib" ] && rm -f "$D/bin/radv.stamp"
        echo "$new" > "$D/bundle.sha256"
        [ -n "$changed" ] &&
            updated="kexts updated to the host's new bundle ($changed ): the framebuffer from it runs from the next boot"
        # the Metal plugin's libdrm / RADV now, not only in the tests below:
        # WindowServer loads them when it starts (card run 47: its DRM client
        # path is in libdrm), so a change needs the next boot like a kext
        metal_synced=
        sync_metal "$D/bin"
        [ -n "$metal_synced" ] && [ -z "$updated" ] &&
            updated="the Metal plugin's libdrm/RADV updated: WindowServer loads them from the next boot"
    fi
    rm -rf "$t"
fi
# the bundle installed now, for the host (rdna3vm _install skips its
# install boot when the VM has the current one)
[ -n "$HOST" ] && [ -s "$D/bundle.sha256" ] &&
    curl -sf --max-time 10 -T "$D/bundle.sha256" "$HOST/reports/bundle-$(date +%Y%m%d-%H%M%S).txt" >/dev/null 2>&1
# the install boot without the card is done: nothing to test here. The new
# framebuffer/accelerator load from the Auxiliary KC on the next boot (the
# card step the host starts now), so no rerun marker.
if [ -n "$INSTALL_ONLY" ]; then
    # a new framebuffer / accelerator / AGDC build waits for "Allow" before
    # macOS loads it: the host opens 'rdna3vm approve' first instead of a
    # card step that would run the old build
    allow=$(printf '%s\n' $changed | grep -x -E 'RDNA3(Framebuffer|Accelerator|AGDC)' | tr '\n' ' ')
    [ -n "$allow" ] && [ -n "$HOST" ] && echo "$allow" > /tmp/rdna3-allow.txt &&
        curl -sf --max-time 10 -T /tmp/rdna3-allow.txt "$HOST/reports/allow-$(date +%Y%m%d-%H%M%S).txt" >/dev/null 2>&1
    finish
fi
if [ -n "$updated" ] || [ -n "$APPROVE" ]; then
    # an interactive boot (rdna3vm approve / window, no rdna3vmauto): the new
    # framebuffer/accelerator wait for "Allow" in Privacy & Security, which
    # only shows up now - open it for whoever is logged in and say so
    case " $(nvram boot-args 2>/dev/null | cut -f2-) " in
        *" rdna3vmauto=1 "*)
            # the new framebuffer/accelerator load from the Auxiliary KC only
            # from the next boot: a step tested now would test the old ones
            # (card run 27: gamma ran on the stale framebuffer). Tell the
            # host to run this step again and stop here.
            if [ -n "$updated" ] && [ -n "$HOST" ]; then
                echo "new kexts installed; the host runs this step again" > /tmp/rdna3-rerun.txt
                curl -sf --max-time 10 -T /tmp/rdna3-rerun.txt "$HOST/reports/rerun-$(date +%Y%m%d-%H%M%S).txt" >/dev/null 2>&1 &&
                    finish
            fi
            ;;
        *)
            if [ -n "$updated" ] || [ -n "$APPROVE" ]; then
                u=$(stat -f %Su /dev/console 2>/dev/null)
                uid=$(id -u "$u" 2>/dev/null)
                if [ -n "$uid" ] && [ "$u" != root ]; then
                    launchctl asuser "$uid" sudo -u "$u" open "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension" 2>/dev/null
                    launchctl asuser "$uid" sudo -u "$u" osascript -e 'display dialog "RDNA3: new kexts installed. In Privacy & Security scroll down and click Allow (password), choose Not Now for the restart, then shut down." buttons {"OK"} with title "RDNA3VM approve"' >/dev/null 2>&1 &
                fi
            fi ;;
    esac
fi
[ -n "$APPROVE" ] && exit 0

# installed earlier (this block runs whether or not the host offered a new
# bundle - inside the install branch it never ran once the bundle was in
# place, card run 40), but macOS still runs a build approved before (card
# run 37: a framebuffer older than the core's display ABI, console mode):
# on an automatic boot tell the host to stop for 'rdna3vm approve'
case " $(nvram boot-args 2>/dev/null | cut -f2-) " in
    *" rdna3vmauto=1 "*)
        if [ -z "$updated" ] && [ -n "$HOST" ]; then
            stale=$(stale_kexts)
            if [ -n "$stale" ]; then
                echo "$stale" > /tmp/rdna3-approve.txt
                curl -sf --max-time 10 -T /tmp/rdna3-approve.txt "$HOST/reports/approve-$(date +%Y%m%d-%H%M%S).txt" >/dev/null 2>&1 &&
                    finish
            fi
        fi ;;
esac

args=$(nvram boot-args 2>/dev/null | cut -f2-)
label=$(echo "$args" | tr ' ' '\n' | grep -E '^rdna3(lvl|dmub|msi|fb|accel)=' | tr '=' '-' | tr '\n' '_' | sed 's/_$//')
[ -n "$label" ] || label=default-profile
# rdna3bisect=1 / 2 / 4 (rdna3vm step bisect / bisect-plugin / bisect-agdc): only the display bisect runs
case " $args " in
    *" rdna3bisect=1 "*) [ -n "$BISECT" ] || BISECT=1 ;;
    *" rdna3bisect=2 "*) [ -n "$BISECT" ] || BISECT=2 ;;
    *" rdna3bisect=4 "*) [ -n "$BISECT" ] || BISECT=4 ;;   # rdna3vm step bisect-agdc: the kernel's switches on the agdc profile
esac
[ -n "$BISECT" ] && label="bisect-$BISECT"
# rdna3ladder=1 / 2 / 3 (rdna3vm step ladder / ladder-ws / ladder-glyphs): only the A/B ladder runs (a standalone run takes the flag)
case " $args " in
    *" rdna3ladder=1 "*) [ -n "$LADDER" ] || LADDER=1 ;;
    *" rdna3ladder=2 "*) [ -n "$LADDER" ] || LADDER=2 ;;
    *" rdna3ladder=3 "*) [ -n "$LADDER" ] || LADDER=3 ;;
esac
[ -n "$LADDER" ] && [ -z "$BISECT" ] && label="ladder-$LADDER"
# rdna3bisect=3 (rdna3vm args "rdna3bisect=3" --stay): nothing runs at boot, so the bisect can be started by hand in
# a Terminal of the guest, with a keyboard to answer on: sudo sh <this script> --bisect | --bisect-plugin | --bisect-agdc
case " $args " in
    *" rdna3bisect=3 "*) [ -n "$BISECT_HAND" ] || { echo "rdna3bisect=3: no tests this boot; start the bisect by hand (sudo sh $0 --bisect; --bisect-agdc needs the agdc profile in the boot-args)"; finish; } ;;
esac
# rdna3play=1 (rdna3vm step play): the desktop is the owner's. No test runs; the full report
# (looks, counters, PM lines, the trace, collect.sh) is taken at minute 5 and again at minute 10, the VM
# stays up. Not in a standalone run.
PLAY=
case " $args " in *" rdna3play=1 "*) [ -n "$STANDALONE" ] || { PLAY=1; label=play; } ;; esac
PLAY_FIRST=${PLAY_FIRST:-300}; PLAY_SECOND=${PLAY_SECOND:-600}
# no rdna3lvl: the kexts run their default profile (common/rdna3_bootargs.h);
# the tests below gate on the same settings
case " $args " in
    *" rdna3lvl="*|*" rdna3profile=0 "*) ;;
    *) args="$args rdna3lvl=8 rdna3msi=1 rdna3dmub=1 rdna3fb=1 rdna3mirror=2 rdna3accel=1 rdna3accelfb=4 rdna3acceliop=1 rdna3pm=2 rdna3idlecap=1 rdna3dpms=1 rdna3modes=1" ;;
esac
lvl=$(echo "$args" | sed -n 's/.*rdna3lvl=\([0-9]*\).*/\1/p')
# no idle system sleep while the tests run: with rdna3resume=1 QEMU lets
# the guest suspend, and macOS (a MacBookPro SMBIOS) slept on its own a
# minute after the display sleep, twice, so the report never got past the
# display wake (card run 36). -i blocks idle sleep only: the display sleep
# and the system sleep test's pmset sleepnow still work
caffeinate -i -w $$ >/dev/null 2>&1 &

extra="$HOME/rdna3-extra.txt"
# the report's name, fixed now: after every test the results so far go up
# under it, so a test that freezes the GPU (and the VM) still leaves them
stamp=$(date +%Y%m%d-%H%M%S)
base=rdna3-report-$label-$stamp
in_session() {   # in_session command...: as root, in the console user's GUI session (CoreGraphics)
    u=$(stat -f %Su /dev/console 2>/dev/null)
    uid=$(id -u "$u" 2>/dev/null)
    if [ -n "$uid" ] && [ "$u" != root ] && [ "$u" != loginwindow ]; then
        launchctl asuser "$uid" "$@"
    else
        "$@"
    fi
}
partial() {
    [ -n "$HOST" ] || return 0
    curl -sf --max-time 10 -T "$extra" "$HOST/reports/$base-tests.txt" >/dev/null 2>&1
}
# the last boot's results that never reached the host (no network after a
# system sleep): up now, under their own names
for pf in /var/db/rdna3-pending-*; do
    [ -f "$pf" ] && [ -n "$HOST" ] || continue
    curl -sf --max-time 60 -T "$pf" "$HOST/reports/${pf#/var/db/rdna3-pending-}" >/dev/null 2>&1 && rm -f "$pf"
done
# a restart only to send them (the VM's network does not come back from S3,
# card runs 28, 29): sent, so stop here
if [ -f /var/db/rdna3-upload-only ]; then
    rm -f /var/db/rdna3-upload-only
    echo "upload-only boot: the results of the boot before are sent"
    finish
fi

# progress on the desktop: a Terminal window in the logged-in user's
# session follows /Users/Shared/rdna3-progress.log (one line per test and
# its result), notifications at the start and the end. Nothing here is
# needed for the report; without a logged-in user it stays silent.
P=/Users/Shared/rdna3-progress.log
win=$([ -f /tmp/rdna3-progress.win ] && echo 1)
as_user() {   # as_user command...: run in the console user's GUI session
    u=$(stat -f %Su /dev/console 2>/dev/null)
    uid=$(id -u "$u" 2>/dev/null)
    [ -n "$uid" ] && [ "$u" != root ] && [ "$u" != loginwindow ] || return 1
    launchctl asuser "$uid" sudo -u "$u" "$@" >/dev/null 2>&1
}
notify() {
    as_user osascript -e "display notification \"$1\" with title \"RDNA3 test\"" &
}
prog() {
    echo "$(date +%H:%M:%S)  $*" >> "$P"
    if [ -z "$win" ]; then
        printf '#!/bin/sh\nclear\necho "RDNA3 test progress (%s) - this window can be closed"\necho\nexec tail -n +1 -F %s\n' \
            "$label" "$P" > /Users/Shared/rdna3-progress.command
        chmod 755 /Users/Shared/rdna3-progress.command
        as_user open -a Terminal /Users/Shared/rdna3-progress.command && win=1 &&
            notify "Tests started: $(echo "$args" | tr ' ' '\n' | grep -c '^rdna3') rdna3 boot-args"
    fi
}
hl=0
# the CTS subset (step cts): every "<seconds> <pattern>" line of
# $D/cts/patterns.txt in its own deqp-vk run; prints per group and in total
# what passed, failed and was not supported, the failing (or, when a run
# was killed, the hanging) cases. 0 when nothing failed or timed out.
cts_run() {
    icd=/tmp/rdna3-radeon_icd.json
    printf '{\n    "file_format_version": "1.0.0",\n    "ICD": {\n        "library_path": "%s/libvulkan_radeon.dylib",\n        "api_version": "1.4.0"\n    }\n}\n' \
        "$D/bin" > "$icd"
    cp=0; cf=0; cn=0; ct=0; cto=0; i=0
    while read -r t p; do
        case "$t" in ''|\#*) continue ;; esac
        i=$((i + 1))
        o=/tmp/rdna3-cts-$i.txt
        ( cd "$D/cts" && DYLD_LIBRARY_PATH="$D/cts:$D/bin" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd" \
            with_limit "$t" ./deqp-vk --deqp-vk-library-path="$D/cts/libvulkan.1.dylib" --deqp-case="$p" \
                --deqp-log-filename="/tmp/rdna3-cts-$i.qpa" --deqp-log-images=disable \
                --deqp-log-shader-sources=disable --deqp-shadercache=disable ) > "$o" 2>&1
        r=$?
        tot=$(sed -n 's/^ *Passed: *\([0-9]*\)\/\([0-9]*\).*/\1 \2/p' "$o" | tail -1)
        f=$(sed -n 's/^ *Failed: *\([0-9]*\)\/.*/\1/p' "$o" | tail -1)
        n=$(sed -n 's/^ *Not supported: *\([0-9]*\)\/.*/\1/p' "$o" | tail -1)
        if [ -n "$tot" ]; then
            set -- $tot
            echo "CTS $p: pass $1, fail ${f:-0}, not supported ${n:-0} (of $2)"
            cp=$((cp + $1)); cf=$((cf + ${f:-0})); cn=$((cn + ${n:-0})); ct=$((ct + $2))
        else
            cto=$((cto + 1))
            echo "CTS $p: no totals (exit $r: killed at ${t} s or crashed) - last case started:"
            grep "^Test case '" "$o" | tail -1 | sed 's/^/    /'
            tail -5 "$o" | sed 's/^/    | /'
        fi
        awk '/^Test case / { c = $3 } /^  (Fail|Crash|InternalError|ResourceError|Timeout)/ { print "    FAIL " c " " $0 }' \
            "$o" | head -20
    done < "$D/cts/patterns.txt"
    echo "CTS: pass $cp, fail $cf, not supported $cn of $ct cases; $cto group(s) without totals"
    [ "$cf" -eq 0 ] && [ "$cto" -eq 0 ]
}
hdr() {   # a test starts: its header in the results, a line in the window
    echo "== $*"
    hl=$(wc -l < "$extra" 2>/dev/null || echo 0)
    prog "$(echo "$*" | sed 's/^rdna3_drminfo //') ..."
    partial   # up before it runs: a test that freezes the VM still shows
}
done_() {   # done_ EXIT [UNJUDGED]: the test's exit code and summary line
    # UNJUDGED (the gamma test's exit 3, the screens kept changing): neither
    # passed nor failed, and the line says so ("[exit 3: ...]" is no failure
    # to triage's "[exit N]" checks)
    if [ "${2:-}" = UNJUDGED ]; then echo "[exit $1: UNJUDGED, not a failure]"; else echo "[exit $1]"; fi
    partial
    sum=$(tail -n +"$((hl + 1))" "$extra" 2>/dev/null |
          grep -E '^(ALL PASSED|FAILED|NOTHING TESTED|METAL:|SOAK:|HANG TEST:|MODES:|GAMMA:|PMCTL:|BACO:|CTS:|PLATFORM:|WSI:|A3:|M4:|M5:|M6:|SOME CHECKS|Stage 4 UAPI|MEMBW:|LINPITCH:)' | tail -1)
    if [ "${2:-}" = UNJUDGED ]; then prog "    UNJUDGED (exit $1)  ${sum}"
    elif [ "$1" = 0 ]; then prog "    OK  ${sum}"; else prog "    FAIL (exit $1)  ${sum}"; fi
}
# Tests that passed before are not run again while nothing they test
# changed: a fingerprint of the builds a test uses (source hashes of the
# kexts, the test tool, the Metal plugin, RADV / libdrm) and the step's
# rdna3 boot-args. A skipped test's saved output goes into the results with
# a SKIPPED line, so the triage reads it as before. rdna3nocache=1 runs all.
CACHE_DIR="$D/passed"
cache_part() {
    case $1 in
        core)  # the source hash (kext/Makefile); older builds: the commit
               c=$(ioreg -l -w0 -r -c RDNA3Accel -d 1 2>/dev/null | sed -n 's/.*"RDNA3,CoreSource" = "\([^"]*\)".*/\1/p' | head -1)
               [ -n "$c" ] && [ "$c" != unknown ] || c=commit-$(ioreg -l -w0 -r -c RDNA3Accel -d 1 2>/dev/null | sed -n 's/.*"RDNA3,Commit" = "\([^"]*\)".*/\1/p' | head -1)
               echo "$c" ;;
        fb)    ioreg -l -w0 -r -c RDNA3Framebuffer -d 1 2>/dev/null | sed -n 's/.*"RDNA3,FBSource" = "\([^"]*\)".*/\1/p' | head -1 ;;
        accel) ioreg -l -w0 -r -c RDNA3Accelerator -d 1 2>/dev/null | sed -n 's/.*"RDNA3,AccelSource" = "\([^"]*\)".*/\1/p' | head -1 ;;
        agdc)  ioreg -l -w0 -r -c RDNA3AGDC -d 1 2>/dev/null | sed -n 's/.*"RDNA3,AGDCSource" = "\([^"]*\)".*/\1/p' | head -1 ;;
        metal) cat /Library/GPUBundles/RDNA3MTLDriver.bundle/Contents/MacOS/RDNA3MTLDriver \
                   /Library/GPUBundles/rdna3mtl/*.dylib /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null | shasum | cut -c1-16 ;;
        radv)  cat "$D/bin/libvulkan_radeon.dylib" "$D/bin/libdrm.2.dylib" "$D/bin/libdrm_amdgpu.1.dylib" 2>/dev/null | shasum | cut -c1-16 ;;
        cts)   cat "$D/cts.sha256" 2>/dev/null ;;
        bin:*) shasum "$D/bin/${1#bin:}" 2>/dev/null | cut -c1-16 ;;
    esac
}
cache_skip() {   # cache_skip NAME PART...: 0 = passed before with the same builds, its output replayed
    CNAME=$1; shift
    CFP=
    [ -z "$STANDALONE" ] || return 1
    case " $args " in *" rdna3nocache=1 "*) return 1 ;; esac
    CFP=$({ for c in "$@"; do echo "$c=$(cache_part "$c")"; done
            echo "args=$(printf '%s\n' $args | grep '^rdna3' | grep -v -e '^rdna3vmauto=' -e '^rdna3nocache=' | sort | tr '\n' ' ')"
          } | shasum | cut -c1-16)
    if [ -f "$CACHE_DIR/$CNAME.out" ] && [ "$(cat "$CACHE_DIR/$CNAME.fp" 2>/dev/null)" = "$CFP" ]; then
        echo "== $CNAME: SKIPPED - passed on $(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$CACHE_DIR/$CNAME.fp") with the same builds of what it tests ($*); rdna3nocache=1 runs it. Its output then:"
        cat "$CACHE_DIR/$CNAME.out"
        prog "    skipped: $CNAME passed before with the same builds"
        return 0
    fi
    return 1
}
ips_summary() {   # ips_summary FILE: a crash report's reason and crashed thread (JXA parses the JSON)
    osascript -l JavaScript -e '
function run(a) {
    var t = $.NSString.stringWithContentsOfFileEncodingError(a[0], 4, null).js;
    var j = JSON.parse(t.slice(t.indexOf("\n") + 1)), o = [];
    ["exception", "termination", "asi"].forEach(function (k) {
        if (j[k]) o.push(k + ": " + JSON.stringify(j[k]));
    });
    var th = j.threads ? j.threads[j.faultingThread || 0] : null, im = j.usedImages || [];
    if (th) {
        o.push("crashed thread " + (j.faultingThread || 0) + (th.queue ? " (" + th.queue + ")" : "") + ":");
        th.frames.slice(0, 30).forEach(function (f) {
            var m = im[f.imageIndex];
            o.push("  " + (m && m.name ? m.name : "?") + "  " + (f.symbol || "0x" + (f.imageOffset || 0).toString(16)) +
                   (f.symbolLocation ? " + " + f.symbolLocation : ""));
        });
    }
    return o.join("\n");
}' "$1"
}

cache_keep() {   # cache_keep EXIT: after done_, the test's output kept when it passed
    [ "$1" = 0 ] && [ -n "${CFP:-}" ] || return 0
    mkdir -p "$CACHE_DIR"
    tail -n +"$((hl + 1))" "$extra" > "$CACHE_DIR/$CNAME.out" 2>/dev/null && echo "$CFP" > "$CACHE_DIR/$CNAME.fp"
    CFP=
}
# ---- step play (rdna3play=1) -------------------------------------------------
# DRIVER-PLAN N43 (2): does UCLK leave its idle floor (96 MHz) under a memory load? rdna3_vktest --membw
# (vkCmdFillBuffer + vkCmdCopyBuffer between two device-local buffers, 10 s) runs in the background while
# rdna3_pmctl info is read three times, and once more 4 s after it ended. Only the lines the triage reads
# are kept (pp_dpm_mclk, the activity line). The inner markers start with "--", not "==": one test section.
# Time: about 25 s, every step under its own limit. Returns the load's exit code.
membw_load() {
    _mo=/tmp/rdna3-membw.txt; _mp=/tmp/rdna3-membw-pm.txt
    rm -f "$_mo" "$_mp"
    DYLD_LIBRARY_PATH="$D/bin" RADV_DEBUG=startup "$D/bin/rdna3_vktest" --membw=10 "$D/bin/libvulkan_radeon.dylib" > "$_mo" 2>&1 &
    _mb=$!
    ( sleep 40; kill -9 "$_mb" 2>/dev/null ) & _mw=$!
    # the buffers are allocated and the load starts when it has printed its first MEMBW: line
    _mi=0
    while [ "$_mi" -lt 40 ] && kill -0 "$_mb" 2>/dev/null && ! grep -q '^MEMBW: ' "$_mo" 2>/dev/null; do sleep 0.5; _mi=$((_mi + 1)); done
    sleep 1
    for _mk in 1 2 3; do
        echo "-- pmctl under load $_mk"
        with_limit 15 "$D/bin/rdna3_pmctl" info > "$_mp" 2>&1
        grep -E '^(pp_dpm_mclk|pp_dpm_sclk|pp_dpm_fclk|  activity gfx)' "$_mp"
        sleep 1
    done
    wait "$_mb"; _mr=$?
    kill "$_mw" 2>/dev/null
    cat "$_mo"
    sleep 4
    echo "-- pmctl idle after the load"
    with_limit 15 "$D/bin/rdna3_pmctl" info > "$_mp" 2>&1
    grep -E '^(pp_dpm_mclk|pp_dpm_sclk|pp_dpm_fclk|  activity gfx)' "$_mp"
    rm -f "$_mo" "$_mp"
    return $_mr
}
play_sleep_until() {   # play_sleep_until <seconds after PLAY_T0>
    _pt=$((PLAY_T0 + $1))
    while [ "$(date +%s)" -lt "$_pt" ]; do sleep 5; done
}
play_looks() {   # play_looks <minute>: three scanout looks 20 s apart, the counters and the PM lines (all read-only)
    for _pk in a b c; do
        mkdir -p "$HOME/rdna3-scanout/play$1$_pk"
        hdr "rdna3_drminfo --scanout (look play$1$_pk)"
        if [ -x "$D/bin/rdna3_drminfo" ]; then with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/play$1$_pk" 2>&1
        else echo "play: no rdna3_drminfo in $D/bin: no scanout look"; fi
        done_ $?
        [ "$_pk" = c ] || sleep 20
    done
    # one 1280-wide look per report (the three above stay 320 wide, the
    # triage's checks read them): small text-cell rectangles and broken
    # glyphs are too small to see at 320. The pictures go up as PPMs; the
    # host's triage makes the PNGs (tools/triage_reports.py ppm_to_png)
    mkdir -p "$HOME/rdna3-scanout/play${1}h"
    hdr "rdna3_drminfo --scanout-hires (look play${1}h)"
    if [ -x "$D/bin/rdna3_drminfo" ]; then with_limit 90 "$D/bin/rdna3_drminfo" --scanout-hires "$HOME/rdna3-scanout/play${1}h" 2>&1
    else echo "play: no rdna3_drminfo in $D/bin: no hires look"; fi
    done_ $?
    echo "== display pipe and AGDC counters (play, minute $1)"
    sysctl debug 2>/dev/null | grep -E "rdna3accel_(iop_(ok|fail|empty|src_res|src_surf|blank|copy_skip|flips)|heads_)|rdna3agdc" || true
    a3_sysctls
    if [ "${lvl:-0}" -ge 5 ] && [ -x "$D/bin/rdna3_pmctl" ]; then
        hdr "rdna3_pmctl info (play, minute $1)"
        with_limit 30 "$D/bin/rdna3_pmctl" info 2>&1
        done_ $?
    fi
}
play_first() {   # inside the results block: wait for minute 5, then the looks
    PLAY_T0=$(date +%s)
    notify "RDNA3 play: use the desktop; the report is taken at minute $((PLAY_FIRST / 60)) and again at minute $((PLAY_SECOND / 60))"
    prog "play: use the desktop; reports at minute $((PLAY_FIRST / 60)) and $((PLAY_SECOND / 60)); the VM stays up after that"
    play_sleep_until "$PLAY_FIRST"
    play_looks "$((PLAY_FIRST / 60))"
}
play_second() {   # after the first report went up: wait for minute 10, then a second full report under its own name
    prog "play: first report done; the second at minute $((PLAY_SECOND / 60))"
    play_sleep_until "$PLAY_SECOND"
    stamp=$(date +%Y%m%d-%H%M%S)
    base=rdna3-report-$label-$stamp
    extra="$HOME/rdna3-extra-2.txt"
    : > "$extra"; hl=0
    {
        echo "boot-args: $args"
        echo "== play: second collection at minute $((PLAY_SECOND / 60)) (the pump still runs)"
        play_looks "$((PLAY_SECOND / 60))"
        trace_snapshot play10
    } >> "$extra" 2>&1
    prog "play: sending the second report (up to 9 min)"
    pend_cp "$extra" "/var/db/rdna3-pending-$base-tests.txt"
    curl -sf --max-time 30 -T "$extra" "$HOST/reports/$base-tests.txt" && echo "uploaded $base-tests.txt" &&
        rm -f "/var/db/rdna3-pending-$base-tests.txt"
    for p in "$HOME"/rdna3-scanout/play$((PLAY_SECOND / 60))*/scanout-pipe*.ppm; do
        [ -f "$p" ] && curl -sf --max-time 30 -T "$p" \
            "$HOST/reports/$base-scanout$(basename "$(dirname "$p")")-$(basename "$p" | sed 's/^scanout-//')"
    done
    RDNA3_STAMP=$stamp with_limit 540 sh "$D/tools/macos/collect.sh" "$label"
    f="$HOME/$base.tar.gz"
    [ -f "$f" ] && pend_cp "$f" "/var/db/rdna3-pending-$base.tar.gz"
    [ -f "$f" ] && curl -sf --max-time 60 -T "$f" "$HOST/reports/$base.tar.gz" && echo "uploaded $f" &&
        rm -f "/var/db/rdna3-pending-$base.tar.gz"
    if [ -f "$f" ]; then prog "play: second report sent - the VM stays up, shut macOS down when you are done"; notify "RDNA3 play: both reports sent; shut macOS down when you are done"
    else prog "play: the second full report did not finish"; notify "RDNA3 play: the second report did not finish"; fi
}
[ -n "$win" ] || { rm -f "$P"; touch "$P"; chmod 644 "$P"; }
[ -n "$win" ] && notify "Tests started: $(echo "$args" | tr ' ' '\n' | grep -c '^rdna3') rdna3 boot-args"
prog "boot-args: $(echo "$args" | tr ' ' '\n' | grep '^rdna3' | tr '\n' ' ')"
{
    echo "boot-args: $args"
    [ -n "$updated" ] && echo "== $updated"
    # the Metal plugin's trace directory, safe from a planted link (before
    # anything that makes WindowServer or an app load the plugin)
    fix_tracedir
    prune_old_traces
    plugin_trace_conf
    # DRIVER-PLAN N44: CoreDisplay's useIOP / useIOP_onAMD, this boot's and from the next boot on
    coredisplay_iop_prefs
    # the kernel's frame trace for the whole run (card run 2026-10-17b)
    trace_pump_start
    # why the Auxiliary KC keeps an old RDNA3Framebuffer (first card runs:
    # kernelmanagerd "invalid codeful kernel extension in the restricted
    # lookup table" for every /Library/Extensions copy)
    # the builds that run against the ones installed (stale_kexts: an
    # automatic boot stops on a difference; card run 39 went on with the
    # framebuffer of the build before)
    echo "== running vs installed framebuffer / accelerator / AGDC"
    for n in RDNA3Framebuffer:FBSource RDNA3Accelerator:AccelSource RDNA3AGDC:AGDCSource; do
        k=${n%%:*}; key=${n#*:}
        echo "$k: running $(ioreg -l -w0 -r -c "$k" -d 1 2>/dev/null | sed -n "s/.*\"RDNA3,$key\" = \"\([0-9a-f]*\)\".*/\1/p" | head -n 1), installed $(/usr/libexec/PlistBuddy -c "Print :RDNA3SourceHash" "/Library/Extensions/$k.kext/Contents/Info.plist" 2>/dev/null)"
    done
    echo "stale: $(stale_kexts | tr '\n' ';')"
    echo "== Auxiliary KC diagnostics"
    ls -l /Library/KernelCollections/ 2>&1
    kmutil showloaded --list-only 2>/dev/null | grep -i rdna3
    for n in RDNA3Framebuffer RDNA3Accelerator RDNA3AGDC; do
        k=/Library/Extensions/$n.kext
        [ -d "$k" ] || continue
        echo "-- $n"
        codesign --verify --deep --strict -vv "$k" 2>&1 | tail -4
        codesign -dv "$k" 2>&1 | grep -E "Identifier|Format|Signature|CDHash"
        with_limit 20 kmutil load -p "$k" 2>&1 | tail -6
    done
    echo "-- kernelmanagerd / kmutil errors this boot"
    with_limit 30 log show --last boot --style compact \
        --predicate '(process == "kernelmanagerd" OR process == "kmutil" OR process == "syspolicyd") AND (messageType == error OR eventMessage CONTAINS[c] "invalid" OR eventMessage CONTAINS[c] "collection")' \
        2>/dev/null | tail -40
    # what each display pipe really scans out (the panels, without anybody
    # looking): HUBP state + a 320-wide picture per lit pipe, uploaded next
    # to the report; the host's triage turns them into PNGs
    # three looks 20 s apart: card run 17's second monitor switched
    # between its own picture and the first one's
    rm -rf "$HOME/rdna3-scanout" "$HOME/rdna3-bisect" "$HOME/rdna3-ladder"
    if [ -n "$LADDER" ] && [ -z "$BISECT" ]; then
        # the A/B ladder replaces the other tests: they would draw on the screens the apps are drawn on
        # (collect.sh packs ladder.txt and the rungs' plugin logs)
        hdr "ladder (DRIVER-PLAN N50: Terminal + Chess per rung, the plugin's switches one by one)"
        ladder_run "$LADDER"
        echo "[exit $?]"
    fi
    if [ -n "$BISECT" ]; then
        # the display bisect replaces the other tests: they would draw on the
        # screens the owner is looking at (collect.sh packs bisect.txt)
        # (no case inside $( ): macOS's bash 3.2 cannot parse its patterns there)
        case $BISECT in
            2) _bwhat="bisect-plugin: the Metal plugin's switches" ;;
            4) _bwhat="bisect-agdc: the kernel's switches, WindowServer on the GPU" ;;
            *) _bwhat="the kernel's switches" ;;
        esac
        hdr "bisect ($_bwhat)"
        bisect_run "$BISECT"
        echo "[exit $?]"
    fi
    if [ -n "$PLAY" ]; then
        hdr "play: the desktop is yours (reports at minute $((PLAY_FIRST / 60)) and $((PLAY_SECOND / 60)))"
        play_first
    fi
    # (not re-indented: every other test, from here to the trace pump's stop)
    if [ -z "$BISECT" ] && [ -z "$PLAY" ] && [ -z "$LADDER" ]; then
    if [ "${lvl:-0}" -ge 2 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        for k in 1 2 3; do
            mkdir -p "$HOME/rdna3-scanout/$k"
            hdr "rdna3_drminfo --scanout (look $k)"
            with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$k" 2>&1
            done_ $?
            [ $k = 3 ] || sleep 20
        done
        # one 1280-wide look (look 3h) beside the three 320-wide ones: small
        # text-cell rectangles and broken glyphs are too small at 320
        mkdir -p "$HOME/rdna3-scanout/3h"
        hdr "rdna3_drminfo --scanout-hires (look 3h)"
        with_limit 90 "$D/bin/rdna3_drminfo" --scanout-hires "$HOME/rdna3-scanout/3h" 2>&1
        done_ $?
        # A3: another process's IOSurface must stay out of reach (ENOENT)
        hdr "rdna3_drminfo --surface-xproc"
        with_limit 30 "$D/bin/rdna3_drminfo" --surface-xproc 2>&1
        done_ $?
        # IOPresentment: does WindowServer hand the display pipes a plane
        # (iop_ok / iop_src_surf), and what did AGDC answer (rdna3agdc)
        echo "== display pipe and AGDC counters"
        sysctl debug 2>/dev/null | grep -E "rdna3accel_(iop_(ok|fail|empty|src_res|src_surf|blank|copy_skip|flips)|heads_)|rdna3agdc" || true
        a3_sysctls
        # card run 2026-10-17b: the swap models side by side, the trace streamed.
        # Opt-in since card run 2026-10-17d (boot-arg rdna3swapab=1): the flip
        # (mode 0) is the default again and mode 1 broke both monitors in the
        # bisect, so a normal step must not run 60 s of it
        case " $args " in
            *" rdna3swapab=1 "*)
                hdr "swap A/B (mode 1 copy / mode 0 flip, 60 s each)"
                swap_ab
                done_ $?
                ;;
            *) echo "== swap A/B: NOT ASKED (boot-arg rdna3swapab=1 runs it; it breaks the picture for 60 s with mode 1)" ;;
        esac
        ioreg -r -c RDNA3AGDC -w0 -d1 2>/dev/null | grep -E '"RDNA3,AGDC' | sed 's/^ *//'
        # with AGDC up WindowServer needs the Metal plugin's device (card runs 41/45)
        echo "Metal allow list: $(grep -v '^#' /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null | tr '\n' ' ') ($(stat -f %Lp /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null))"
        echo "Metal allow mode: $(rdna3_allow_mode)"
        # display sleep (rdna3dpms=1): the heads must blank, then come back
        case " $args " in
            *" rdna3dpms=1 "*|*" rdna3dpms=2 "*)
                hdr "display sleep: pmset displaysleepnow"
                # card run 58 (2026-10-15d): the VM went on into system sleep
                # 8 s after the display slept, so the look read nothing
                # ("display asleep"); an idle-sleep assertion keeps the
                # system up while the display sleeps
                # caffeinate -i -w (the report's own) did not stop it: the
                # power settings, the assertions and the power log before
                # and after say what put the system to sleep; system sleep
                # is set to never for the look and restored after it
                # (only when the old value was read, and saved first:
                # restore_sleep puts it back on the next run, or at exit
                # in standalone mode, should this one die in the look)
                old_sleep=$(pmset -g 2>/dev/null | awk '$1 == "sleep" { print $2; exit }')
                case "$old_sleep" in *[!0-9]*) old_sleep= ;; esac
                echo "== power before: system sleep ${old_sleep:-?} min"
                pmset -g assertions 2>/dev/null | grep -iE 'PreventUserIdleSystemSleep|PreventSystemSleep|caffeinate' | head -8
                if [ -z "$old_sleep" ]; then
                    echo "system sleep left as it is: its value was not read (the caffeinate assertion alone keeps the system up)"
                elif [ "$old_sleep" != 0 ]; then
                    if echo "$old_sleep" > "$SLEEP_BAK" && pmset -a sleep 0 2>&1; then :
                    else echo "system sleep left as it is: could not save it to $SLEEP_BAK or set it"; rm -f "$SLEEP_BAK"; fi
                fi
                caffeinate -s -i -t 120 &
                cf_pid=$!
                pmset displaysleepnow 2>&1
                sleep 8
                echo "== power after displaysleepnow"
                pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Display is turned)' | tail -6
                for k in sleep wake; do
                    mkdir -p "$HOME/rdna3-scanout/$k"
                    hdr "rdna3_drminfo --scanout (look $k)"
                    with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$k" 2>&1
                    done_ $?
                    if [ $k = sleep ]; then
                        hdr "display wake: caffeinate -u"
                        caffeinate -u -t 3 2>&1
                        sleep 8
                    fi
                done
                echo "== power after the looks"
                pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Display is turned)' | tail -6
                kill "$cf_pid" 2>/dev/null
                restore_sleep ;;
        esac
        # monitor standby on every TMDS head (rdna3dpms=2, step sleep): the
        # kext's check of each head after the link went down and came back
        # (RDNA3,Head<n>,State: power, OTG, DIG, frames counted in 50 ms)
        case " $args " in
            *" rdna3dpms=2 "*)
                hdr "monitor standby: the heads' state (RDNA3Accel)"
                ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
                    grep -E '"RDNA3,(Head[0-9],State|DisplayStateSaves|DisplayStateRestores)"'
                done_ $? ;;
        esac
        # scaled modes (rdna3modes=1): each display through two smaller
        # modes (first to the fastest timing of its own size, if any: the
        # DISPCLK raise), a look at the pipes after each switch with underflow,
        # DET and clocks per pipe, then native again
        case " $args " in
            *" rdna3modes=1 "*)
                if [ -x "$D/bin/rdna3_modetest" ]; then
                    hdr "rdna3_modetest list"
                    with_limit 60 in_session "$D/bin/rdna3_modetest" list 2>&1
                    done_ $?
                    if ! cache_skip modes core fb accel agdc metal bin:rdna3_modetest bin:rdna3_drminfo; then
                        hdr "rdna3_modetest cycle"
                        with_limit 240 in_session "$D/bin/rdna3_modetest" cycle "$D/bin/rdna3_drminfo" "$HOME/rdna3-scanout" 2>&1
                        r=$?; done_ $r; cache_keep $r
                    fi
                fi ;;
        esac
        # hotplug (rdna3hotplug=1): a marked window in which the user pulls
        # the second monitor's cable and puts it back; every connector's HPD
        # state (RDNA3,HPDState) whenever it changes, the kext's hotplug
        # lines, then a look at what the pipes show
        case " $args " in
            *" rdna3hotplug=1 "*|*" rdna3hotplug=2 "*)
                hpstate() {
                    ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
                        grep -E '"RDNA3,(HPDState|HotplugGeneration|HotplugLast|HotplugClonesLit|HotplugClonesOff|HotplugBootHead|HotplugRelights|HotplugRelightFails|Mirror|ExtendHead)"' |
                        sed 's/^[ |]*//' | tr '\n' ' '
                }
                hdr "hotplug window (120 s): unplug and replug the second monitor now"
                notify "Unplug and replug the second monitor now (2 minutes)"
                prog "    >>> unplug and replug the second monitor now: cable out ~10 s, then back in <<<"
                hplast=; hpt0=$(date +%s)
                while [ $(( $(date +%s) - hpt0 )) -lt 120 ]; do
                    hpcur=$(hpstate)
                    if [ "$hpcur" != "$hplast" ]; then
                        echo "$(date +%H:%M:%S) $hpcur"
                        prog "    $(echo "$hpcur" | sed -n 's/.*"RDNA3,HPDState" = "\([^"]*\)".*/\1/p')"
                        hplast=$hpcur
                    fi
                    sleep 2
                done
                echo "== kext and framebuffer hotplug lines"
                with_limit 30 log show --last 5m --style compact \
                    --predicate 'eventMessage CONTAINS "hotplug" OR eventMessage CONTAINS "connect change" OR eventMessage CONTAINS "mirror:" OR eventMessage CONTAINS "extend:"' \
                    2>/dev/null | grep -E 'RDNA3' | tail -80
                hpgen=$(hpstate | sed -n 's/.*"RDNA3,HotplugGeneration" = \([0-9]*\).*/\1/p')
                echo "HOTPLUG: ${hpgen:-0} HPD change(s) seen (2 expected: out and back in)"
                prog "    HOTPLUG: ${hpgen:-0} HPD change(s) seen (2 expected: out and back in)"
                partial
                mkdir -p "$HOME/rdna3-scanout/hotplug"
                hdr "rdna3_drminfo --scanout (look hotplug)"
                with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/hotplug" 2>&1
                done_ $?
                ;;
        esac
        # output gamma (on unless rdna3gamma=0): a dark table, the identity,
        # the ColorSync settings back; each pipe's OTG CRC must follow. The
        # test is long, so a step selects it with an explicit rdna3gamma=1.
        case " $args " in
            *" rdna3gamma=1 "*)
                if [ -x "$D/bin/rdna3_modetest" ]; then
                    # card runs 26-28: no setGammaTable reached the framebuffer.
                    # Which way does the table go: the accelerator's display
                    # pipe transactions (empty = no plane, gamma/options only)?
                    echo "== display pipe counters before"; sysctl debug 2>/dev/null | grep -E "rdna3accel_iop" || true
                    if ! cache_skip gamma core fb accel agdc metal bin:rdna3_modetest bin:rdna3_drminfo; then
                        hdr "rdna3_modetest gamma"
                        echo "== gamma property before"
                        ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null | grep -E '"RDNA3,Head[0-9],Gamma"' | sed 's/^ *//'
                        with_limit 180 in_session "$D/bin/rdna3_modetest" gamma "$D/bin/rdna3_drminfo" "$HOME/rdna3-scanout" 2>&1
                        r=$?
                        # the core's own record of the tables: what triage
                        # judges an UNJUDGED test (exit 3: the screen kept
                        # changing) by; such a run is not kept as passed
                        echo "== gamma property after"
                        ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null | grep -E '"RDNA3,Head[0-9],Gamma"' | sed 's/^ *//'
                        if [ "$r" = 3 ]; then done_ 3 UNJUDGED; else done_ $r; cache_keep $r; fi
                    fi
                    echo "== display pipe counters after"; sysctl debug 2>/dev/null | grep -E "rdna3accel_iop" || true
                    echo "== IOAcceleratorFamily2: display pipe transaction and gamma symbols"
                    for k in /System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2 \
                             /System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily \
                             /System/Library/Extensions/IOGraphicsFamily.kext/Contents/MacOS/IOGraphicsFamily; do
                        [ -f "$k" ] || continue
                        echo "-- $k"
                        nm -g "$k" 2>/dev/null | grep -E -i "DisplayPipeTransaction|gamma|DisplayPipe.*Cap" | head -80
                    done
                    echo "== kernel log: gamma and empty transactions"
                    with_limit 30 log show --last 5m --style compact --predicate 'eventMessage CONTAINS[c] "gamma" OR eventMessage CONTAINS "iop: empty"' 2>/dev/null | tail -40
                fi ;;
        esac
    fi
    if [ "${lvl:-0}" -ge 7 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        hdr "rdna3_drminfo --dma-test"
        with_limit 60 "$D/bin/rdna3_drminfo" --dma-test 2>&1
        done_ $?
    fi
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        hdr "rdna3_drminfo --gfx-test"
        with_limit 60 "$D/bin/rdna3_drminfo" --gfx-test 2>&1
        done_ $?
    fi
    # user queues (step userq): a compute queue the MES runs from the
    # client's own doorbell
    case " $args " in
    *" rdna3userq=1 "*)
        if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
            hdr "rdna3_drminfo --userq-test"
            with_limit 60 "$D/bin/rdna3_drminfo" --userq-test 2>&1
            done_ $?
        fi ;;
    esac
    # power management (rdna3_pmctl): what the SMU reports on every level
    # with PM; step pmctl (rdna3pmctl=1/2) sets each knob once and back;
    # BACO (rdna3baco=1) runs near the end, after the system sleep
    if [ "${lvl:-0}" -ge 5 ] && [ -x "$D/bin/rdna3_pmctl" ]; then
        hdr "rdna3_pmctl info"
        with_limit 30 "$D/bin/rdna3_pmctl" info 2>&1
        done_ $?
        case " $args " in
        *" rdna3pmctl=1 "*|*" rdna3pmctl=2 "*)
            if ! cache_skip pmctl-selftest core bin:rdna3_pmctl; then
                hdr "rdna3_pmctl selftest"
                with_limit 120 "$D/bin/rdna3_pmctl" selftest 2>&1
                r=$?; done_ $r; cache_keep $r
            fi
            hdr "rdna3_pmctl profiles"
            with_limit 30 "$D/bin/rdna3_pmctl" profiles 2>&1
            done_ $? ;;
        esac
    fi
    # video (steps vcn, vcn2, vcnsleep): HW_IP_INFO / VIDEO_CAPS and a job
    # on each video ring; the kext's power-gating path and DPG counters
    case " $args " in
    *" rdna3vcn=1 "*|*" rdna3vcn=2 "*|*" rdna3vcn=3 "*)
        ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
            grep -E '"RDNA3,(VCN|VCNPowerGating|VCNResult|VCNDPGStarts|VCNSRAMUploads|VCNSRAMUploadErrors|VCNStarts|VCNGated)"'
        if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
            hdr "rdna3_drminfo --vcn-test"
            with_limit 60 "$D/bin/rdna3_drminfo" --vcn-test 2>&1
            done_ $?
        fi ;;
    esac
    # Vulkan: RADV (prepare --radv) straight through its ICD entry points,
    # no loader; libdrm_xnu talks to RDNA3Accel's user client
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_vktest" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
        if ! cache_skip vktest core radv bin:rdna3_vktest; then
            hdr "rdna3_vktest"
            DYLD_LIBRARY_PATH="$D/bin" RADV_DEBUG=startup with_limit 90 "$D/bin/rdna3_vktest" \
                "$D/bin/libvulkan_radeon.dylib" 2>&1
            r=$?; done_ $r; cache_keep $r
        fi
    fi
    # DRIVER-PLAN N43 (2): a memory-bandwidth load with rdna3_pmctl read during it. Only where UCLK DPM is free
    # (rdna3pm=2: with it off UCLK is pinned and there is nothing to see) and the Vulkan test and the PM tool are
    # there; every other step prints one line saying why not (the triage reads it).
    case " $args " in
    *" rdna3pm=2 "*)
        if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_vktest" ] && [ -x "$D/bin/rdna3_pmctl" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
            hdr "rdna3 memory load (rdna3_vktest --membw, rdna3_pmctl info x3 during it)"
            membw_load
            done_ $?
        else
            echo "== memory load: skipped (needs rdna3lvl>=8, rdna3_vktest, rdna3_pmctl and RADV in the report's bin)"
        fi ;;
    *) echo "== memory load: skipped (rdna3pm=2 is not set: UCLK is not free, nothing to see)" ;;
    esac
    # Vulkan CTS subset (rdna3cts=1, V2): deqp-vk from `rdna3vm cts` through
    # the Khronos loader and RADV's ICD, one run per line of its
    # patterns.txt (a time limit each); totals per group, the failing cases
    case " $args " in
    *" rdna3cts=1 "*)
        ctsum=$([ -n "$HOST" ] && curl -sf --max-time 10 "$HOST/rdna3-cts.sha256" | cut -c1-64)
        if [ -n "$ctsum" ] && [ "$ctsum" != "$(cat "$D/cts.sha256" 2>/dev/null)" ]; then
            rm -rf "$D/cts"
            curl -sf --max-time 900 "$HOST/rdna3-cts.tar.gz" | tar -xz -C "$D" && echo "$ctsum" > "$D/cts.sha256"
        fi
        if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/cts/deqp-vk" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
            if ! cache_skip cts core radv cts; then
                hdr "Vulkan CTS subset ($(cut -d, -f1 "$D/cts/VERSION" 2>/dev/null))"
                cts_run
                r=$?; done_ $r; cache_keep $r
            fi
        else
            echo "== Vulkan CTS: no deqp-vk bundle (rdna3vm cts) or no RADV in the report's bin"
        fi ;;
    esac
    # Metal (build_metal.sh installed the plugin): devices and a compute
    # kernel through plugin -> translator -> RADV; the plugin's log after it
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/mtlprobe" ] && [ -d /Library/GPUBundles/RDNA3MTLDriver.bundle ]; then
        sync_metal "$D/bin"
        if ! cache_skip mtlprobe core accel metal bin:mtlprobe; then
            hdr "mtlprobe (MetalPluginName: $(ioreg -r -c RDNA3Accelerator -w0 | grep -c MetalPluginName))"
            rm -f /tmp/rdna3-nvmtl.log
            NVMTL_LOG_PATH=/tmp/rdna3-nvmtl.log with_limit 90 "$D/bin/mtlprobe" 2>&1
            r=$?; done_ $r; cache_keep $r
            echo "== plugin log (last 60 lines)"
            tail -60 /tmp/rdna3-nvmtl.log 2>/dev/null
        fi
    elif [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/mtlprobe" ]; then
        hdr "mtlprobe: plugin not installed (tools/build_metal.sh in the VM)"
        with_limit 30 "$D/bin/mtlprobe" --list 2>&1
        done_ $?
    fi
    # the plan items only the card can answer (rdna3probe=1, step big4): what
    # the next round of R3 / R4, V3, A3, M4, M5 and M6 starts from (P2 is
    # the PM experiment, rdna3pmexp=1, in the kernel log)
    case " $args " in
    *" rdna3probe=1 "*)
        # the flicker test's baseline right before the probes and M6, with
        # the test on for every swap (its 3000-swap budget is spent by now)
        # until M6 ends; triage takes the change from this dump
        echo "== A3 display counters before the probes"
        flick_old=$(sysctl -n debug.rdna3accel_flicker 2>/dev/null)
        [ -n "$flick_old" ] && sysctl -w debug.rdna3accel_flicker=2 >/dev/null 2>&1 &&
            echo "debug.rdna3accel_flicker set to 2 for the probes (was $flick_old)"
        a3_sysctls
        hdr "probe: platform (R3 bare metal, R4 macOS 26)"
        vm=$([ "$(sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] && echo "virtual machine" || echo "bare metal")
        echo "PLATFORM: $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion)), Darwin $(uname -r), $vm, $(sysctl -n hw.model) $(sysctl -n machdep.cpu.brand_string)"
        ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
            grep -E '"RDNA3,(NeedPost|PostResult|PSPStuckAtInit|PSPResult|SOSVersion|VBIOSResult|Commit)"' | sed 's/^ *//'
        echo "kexts: $(kextstat 2>/dev/null | grep -o 'org\.[a-z0-9]*\.RDNA3[A-Za-z]*' | sort -u | tr '\n' ' ')"
        if ioreg -r -c RDNA3Accelerator -w0 -d1 2>/dev/null | grep -q RDNA3Accelerator; then
            echo "R4: the accelerator runs on Darwin $(uname -r | cut -d. -f1) ($(sw_vers -productVersion))"
        else
            echo "R4: the accelerator is not running on Darwin $(uname -r | cut -d. -f1) ($(sw_vers -productVersion)): its object layout is checked for one Darwin per build"
        fi
        done_ 0
        if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_vktest" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
            hdr "probe: Vulkan WSI (V3)"
            DYLD_LIBRARY_PATH="$D/bin" RADV_DEBUG=startup with_limit 60 "$D/bin/rdna3_vktest" --wsi "$D/bin/libvulkan_radeon.dylib" > /tmp/rdna3-v3.txt 2>&1
            r=$?
            cat /tmp/rdna3-v3.txt
            # RADV has no present path on macOS (no headless/Metal surface,
            # no swapchain): what the probe records, not a failure
            grep -q '^WSI: no present path to try' /tmp/rdna3-v3.txt && r=0
            done_ $r
        fi
        if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/mtlprobe" ] && [ -d /Library/GPUBundles/RDNA3MTLDriver.bundle ]; then
            sync_metal "$D/bin"
            hdr "probe: IOSurface as a Metal texture (A3)"
            rm -f /tmp/rdna3-nvmtl-a3.log
            NVMTL_LOG_PATH=/tmp/rdna3-nvmtl-a3.log with_limit 60 "$D/bin/mtlprobe" --iosurface 2>&1
            r=$?
            echo "== plugin log (last 30 lines)"
            tail -30 /tmp/rdna3-nvmtl-a3.log 2>/dev/null
            done_ $r
            # M4: the window in the user's session, a picture of the
            # scanout while it shows (look m4: triage counts its magenta)
            hdr "probe: CAMetalLayer on the screen (M4)"
            rm -f /tmp/rdna3-nvmtl-m4.log /tmp/rdna3-m4.txt
            ( NVMTL_LOG_PATH=/tmp/rdna3-nvmtl-m4.log with_limit 45 in_session "$D/bin/mtlprobe" --layer 15 \
                > /tmp/rdna3-m4.txt 2>&1; echo "[mtlprobe exit $?]" >> /tmp/rdna3-m4.txt ) &
            mp=$!
            sleep 8
            mkdir -p "$HOME/rdna3-scanout/m4"
            echo "== rdna3_drminfo --scanout (look m4)"
            with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/m4" 2>&1
            wait $mp
            # mtlprobe waits for presented callbacks, which WindowServer never
            # sends on the flip path (card runs 49-53: the window was on the
            # screen, magenta, while mtlprobe said FAILED): with the drawing
            # right, the picture of the scanout above decides (the host's
            # triage counts its magenta)
            sed 's/^M4: FAILED \(.*rendering right, presents NEVER shown.*\)$/M4: NO PRESENT CALLBACK \1 - the m4 scanout look decides (flip path)/' \
                /tmp/rdna3-m4.txt
            echo "== plugin log (last 30 lines)"
            tail -30 /tmp/rdna3-nvmtl-m4.log 2>/dev/null
            grep -q -E '^M4: PASSED|^M4: FAILED .*rendering right, presents NEVER shown' /tmp/rdna3-m4.txt
            done_ $?
            # M5: who loaded the plugin, what macOS thinks of Metal on the
            # card, what WindowServer says about its GPU
            hdr "probe: WindowServer and the Metal plugin (M5)"
            ioreg -r -c RDNA3Accelerator -w0 -d1 2>/dev/null | grep -E '"(MetalPluginName|MetalPluginClassName|IOGLBundleName)"' | sed 's/^ *//'
            echo "allow list: $(grep -v '^#' /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null | tr '\n' ' ')"
            echo "allow mode: $(rdna3_allow_mode)"
            # +c 0: whole command names (lsof cuts them to 9 characters,
            # card run 51 read "WindowSer" as no WindowServer)
            ld=$(lsof +c 0 -n 2>/dev/null | awk '/RDNA3MTLDriver/ { print $1 "(" $2 ")" }' | sort -u | tr '\n' ' ')
            echo "processes with the plugin loaded: ${ld:-none}"
            with_limit 60 system_profiler SPDisplaysDataType 2>/dev/null | grep -iE 'Chipset|Metal|VRAM|Vendor|Device ID|Resolution|Display Type|Main Display' | sed 's/^ *//'
            echo "== WindowServer about Metal / the GPU (last 15 min)"
            with_limit 90 log show --last 15m --style compact \
                --predicate 'process == "WindowServer" AND (eventMessage CONTAINS[c] "metal" OR eventMessage CONTAINS[c] "MTL" OR eventMessage CONTAINS[c] "GPU" OR eventMessage CONTAINS[c] "accelerator")' \
                2>/dev/null | tail -25
            ws=$(echo "$ld" | grep -c WindowSer)
            echo "M5: WindowServer $([ "$ws" -gt 0 ] && echo "LOADED" || echo "did not load") the plugin; $(grep -qxE '[@!]?WindowServer' /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null && echo "it is" || echo "not") on the allow list"
            done_ 0
            # M6: two system apps that draw with Metal, allowed for this run
            # only (the allow list comes back afterwards, or at the next
            # report if this one dies)
            if ! cache_skip m6 core fb accel agdc metal; then
            hdr "probe: Metal apps (M6)"
            al=/Library/GPUBundles/rdna3mtl-allow.txt
            [ -f "$al" ] && [ ! -f /var/db/rdna3-allow.bak ] && cp "$al" /var/db/rdna3-allow.bak
            printf 'Chess\nMaps\n' >> "$al"
            m6ok=0
            for app in Chess Maps; do
                touch "/tmp/rdna3-m6-$app"
                in_session open -a "$app" 2>&1
                sleep 15
                run=$(pgrep -x "$app" >/dev/null && echo yes || echo NO)
                pl=$(lsof -n -c "$app" 2>/dev/null | grep -c RDNA3MTLDriver)
                # card run 56: lsof counted the plugin in Maps, yet Maps logged
                # no NVMTL line at all: which process has it, and what the app
                # itself says about Metal and the GPU
                echo "== M6 $app: processes with the plugin mapped"
                lsof -n -c "$app" 2>/dev/null | grep -E 'RDNA3MTLDriver|libvulkan_radeon' | awk '{print $1, $2, $NF}' | sort -u | head -8
                echo "== M6 $app: its own log about Metal / the GPU (last minute)"
                with_limit 40 log show --last 1m --style compact --predicate "process == \"$app\" AND (eventMessage CONTAINS[c] \"metal\" OR eventMessage CONTAINS[c] \"gpu\" OR eventMessage CONTAINS \"NVMTL\" OR eventMessage CONTAINS[c] \"MTL\")" \
                    2>/dev/null | tail -25
                mkdir -p "$HOME/rdna3-scanout/m6$app"
                # 1280 wide (card run 54: Chess's and Maps' faults were too
                # small to see at 320)
                echo "== rdna3_drminfo --scanout-hires (look m6$app)"
                with_limit 60 "$D/bin/rdna3_drminfo" --scanout-hires "$HOME/rdna3-scanout/m6$app" 2>&1 | m6_look_state
                pkill -x "$app" 2>/dev/null
                sleep 2
                cr=$(find /Library/Logs/DiagnosticReports /Users/*/Library/Logs/DiagnosticReports -name "$app*" \
                     -newer "/tmp/rdna3-m6-$app" 2>/dev/null | head -1)
                echo "M6 $app: running $run, plugin $([ "$pl" -gt 0 ] && echo loaded || echo "NOT loaded"), crash report: ${cr:-none}"
                # the reason and the crashed thread's frames (card run 50:
                # the first 40 lines of Chess's .ips were only its header)
                [ -n "$cr" ] && { ips_summary "$cr" 2>/dev/null || head -40 "$cr"; }
                [ "$run" = yes ] && [ "$pl" -gt 0 ] && [ -z "$cr" ] && m6ok=$((m6ok + 1))
            done
            [ -f /var/db/rdna3-allow.bak ] && mv /var/db/rdna3-allow.bak "$al"
            echo "M6: $m6ok of 2 apps ran on the plugin without a crash"
            # card run 54: System Settings' sidebar stayed stale while it
            # scrolled (WindowServer's copy forward, RD3Surface::
            # submitCopyForward): its window and the desktop afterwards, 1280
            # wide, and what the copy forward did meanwhile
            in_session open -b com.apple.systempreferences 2>&1
            sleep 8
            # in apps mode System Settings is an application like the others
            # (card run 54: its sidebar stayed stale on Apple's software path)
            echo "System Settings: plugin $([ "$(lsof +c 0 -n -c 'System Settings' 2>/dev/null | grep -c RDNA3MTLDriver)" -gt 0 ] && echo loaded || echo "NOT loaded") ($(rdna3_allow_mode))"
            mkdir -p "$HOME/rdna3-scanout/m6Settings"
            echo "== rdna3_drminfo --scanout-hires (look m6Settings)"
            with_limit 60 "$D/bin/rdna3_drminfo" --scanout-hires "$HOME/rdna3-scanout/m6Settings" 2>&1 | m6_look_state
            pkill -x "System Settings" 2>/dev/null
            sleep 3
            mkdir -p "$HOME/rdna3-scanout/m6desktop"
            echo "== rdna3_drminfo --scanout-hires (look m6desktop)"
            with_limit 60 "$D/bin/rdna3_drminfo" --scanout-hires "$HOME/rdna3-scanout/m6desktop" 2>&1 | m6_look_state
            echo "== A3 display counters after M6"
            a3_sysctls
            # the run's limit stops a slow M6 (card run 58b): triage keys "NOT FINISHED" on this line
            echo "== M6 end"
            echo "== copy forward (kernel) during M6"
            with_limit 60 log show --last 3m --style compact --predicate 'eventMessage CONTAINS "copy forward"' 2>/dev/null | tail -30
            done_ 0
            [ "$m6ok" = 2 ] && cache_keep 0
            CFP=
            fi
        else
            echo "== probe: A3 / M4 / M5 / M6 need the Metal plugin (tools/build_metal.sh in the VM) and level 8"
        fi
        # the flicker test back to its setting (a run killed before this
        # leaves 2 until the next boot: sysctls do not persist)
        [ -n "${flick_old:-}" ] && sysctl -w debug.rdna3accel_flicker="$flick_old" >/dev/null 2>&1 ;;
    esac
    # soak (rdna3soak=<minutes>): DMA + GFX jobs and the Vulkan test in a
    # loop; the PM trend in the serial log runs alongside
    soak=$(printf '%s\n' $args | sed -n 's/^rdna3soak=\([0-9]*\)$/\1/p' | head -1)
    if [ -n "$soak" ] && [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        if ! cache_skip soak core radv bin:rdna3_drminfo bin:rdna3_vktest; then
        hdr "soak for $soak min"
        end=$(( $(date +%s) + soak * 60 )); n=0; bad=0
        while [ "$(date +%s)" -lt "$end" ]; do
            n=$((n + 1))
            [ $((n % 5)) = 1 ] && prog "    soak round $n, $bad failed so far"
            with_limit 30 "$D/bin/rdna3_drminfo" --dma-test --gfx-test >/dev/null 2>&1 || bad=$((bad + 1))
            if [ -x "$D/bin/rdna3_vktest" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
                DYLD_LIBRARY_PATH="$D/bin" with_limit 60 "$D/bin/rdna3_vktest" \
                    "$D/bin/libvulkan_radeon.dylib" >/dev/null 2>&1 || bad=$((bad + 1))
            fi
        done
        echo "SOAK: $n rounds, $bad failed"
        if [ "$bad" = 0 ]; then prog "    OK  SOAK: $n rounds"; else prog "    FAIL  SOAK: $n rounds, $bad failed"; fi
        partial
        cache_keep "$bad"
        fi
    fi
    # system sleep (rdna3resume=1, step sleep): a short S3 with an RTC wake
    # (rdna3vm wakes the VM after 90 s otherwise), the heads before and
    # after (picture, OTG CRC), the kext's verdict (RDNA3,Wake: did the
    # card keep its power, which heads were restored), then the rings
    case " $args " in
        *" rdna3resume=1 "*)
            if [ "${lvl:-0}" -ge 2 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
                for k in sysbefore syswake; do
                    if [ $k = syswake ]; then
                        hdr "system sleep: pmset relative wake 30; pmset sleepnow"
                        s0=$(sysctl -n kern.sleeptime 2>/dev/null)
                        pmset relative wake 30 2>&1
                        pmset sleepnow 2>&1
                        sleep 60
                        caffeinate -u -t 2 2>&1
                        sleep 15
                        # card run 28: the VM's vmxnet3 did not come back from
                        # the S3 and nothing after it reached the host; the
                        # interface down and up again, DHCP again
                        if ! curl -sf --max-time 5 -o /dev/null "$HOST/rdna3-bundle.sha256"; then
                            for i in $(ifconfig -l 2>/dev/null); do
                                case $i in en*) ifconfig "$i" down; sleep 1; ifconfig "$i" up; ipconfig set "$i" DHCP 2>/dev/null ;; esac
                            done
                            sleep 10
                            curl -sf --max-time 5 -o /dev/null "$HOST/rdna3-bundle.sha256" &&
                                echo "network: back after the interface was restarted" ||
                                echo "network: still no host after the wake (the results go up at the next boot)"
                        fi
                        s1=$(sysctl -n kern.sleeptime 2>/dev/null)
                        echo "kern.sleeptime before: $s0"
                        echo "kern.sleeptime after:  $s1, waketime $(sysctl -n kern.waketime 2>/dev/null)"
                        pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Failure) ' | tail -6
                        ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
                            grep -E '"RDNA3,(Sleeps|Wakes|Wake|WakePowerLost|WakeHeadsLost|WakeResumeResult|Resumes|ResumeResult|VRAMLost|Head[0-9],State|DisplayStateRestores)"'
                        lost=$(ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
                               sed -n 's/.*"RDNA3,WakeHeadsLost" = \([0-9]*\).*/\1/p' | head -1)
                        if [ "$s0" = "$s1" ]; then echo "SYSTEM SLEEP: the system did not sleep"; r=1
                        elif [ "${lost:-1}" != 0 ]; then echo "SYSTEM SLEEP: slept, ${lost:-?} head(s) not back"; r=1
                        else echo "SYSTEM SLEEP: slept and woke, every head back"; r=0; fi
                        done_ $r
                    fi
                    mkdir -p "$HOME/rdna3-scanout/$k"
                    hdr "rdna3_drminfo --scanout (look $k)"
                    with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$k" 2>&1
                    done_ $?
                done
                if [ "${lvl:-0}" -ge 8 ]; then
                    hdr "rdna3_drminfo --dma-test --gfx-test (after the wake)"
                    with_limit 60 "$D/bin/rdna3_drminfo" --dma-test --gfx-test 2>&1
                    done_ $?
                fi
                # the video block after the wake (step vcnsleep): suspended
                # before the sleep, resumed after it (vcn_v4_0_resume)
                case " $args " in
                *" rdna3vcn="[123]" "*)
                    if [ "${lvl:-0}" -ge 8 ]; then
                        ioreg -r -c RDNA3Accel -w0 -d1 2>/dev/null |
                            grep -E '"RDNA3,(VCN|VCNSuspends|VCNSuspendResult|VCNResumes|VCNResult|VCN0IBTest|VCN1IBTest|JPEGResult|VCNSRAMUploads)"'
                        hdr "rdna3_drminfo --vcn-test (after the wake)"
                        with_limit 60 "$D/bin/rdna3_drminfo" --vcn-test 2>&1
                        done_ $?
                    fi ;;
                esac
            fi ;;
    esac
    # BACO (step baco, big4): the chip off and on again through the PMFW,
    # then the DMA/GFX tests; it resets the whole chip, so it runs after
    # every other test but the hang test (the results so far are already up)
    if [ "${lvl:-0}" -ge 5 ] && [ -x "$D/bin/rdna3_pmctl" ]; then
        case " $args " in
        *" rdna3baco=1 "*)
            hdr "rdna3_pmctl baco-cycle"
            with_limit 120 "$D/bin/rdna3_pmctl" baco-cycle 2>&1
            done_ $?
            if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
                hdr "rdna3_drminfo --dma-test --gfx-test (after BACO)"
                with_limit 60 "$D/bin/rdna3_drminfo" --dma-test --gfx-test 2>&1
                done_ $?
            fi ;;
        esac
    fi
    # last, it resets engines: a DMA job that never finishes, the kernel
    # must recover (rdna3hangtest=1); =2 also a GFX one, which froze the
    # whole host in the VM once (card run 19) - a step of its own
    case " $args " in
        *" rdna3hangtest=1 "*|*" rdna3hangtest=2 "*)
            if [ "${lvl:-0}" -ge 7 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
                hdr "rdna3_drminfo --hang-test"
                with_limit 120 "$D/bin/rdna3_drminfo" --hang-test 2>&1
                done_ $?
            fi ;;
    esac
    case " $args " in
        *" rdna3hangtest=2 "*)
            if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
                hdr "rdna3_drminfo --hang-test-gfx"
                with_limit 120 "$D/bin/rdna3_drminfo" --hang-test-gfx 2>&1
                done_ $?
            fi ;;
    esac
    fi   # [ -z "$BISECT" ] && [ -z "$PLAY" ] && [ -z "$LADDER" ]
    [ -n "$PLAY" ] || trace_pump_stop
} > "$extra" 2>&1
if [ -n "$PLAY" ]; then
    # the pump keeps running: a whole-blob snapshot goes up and into the archive instead of its stop
    trace_snapshot play5 >> "$extra" 2>&1
    export RDNA3_TRACE_DIR="$TS/snap"
else
    trace_pump_stop >> "$extra" 2>&1   # a shell that ran the block in a subshell: the pump stops here
    trace_send >> "$extra" 2>&1
    snap_plugin end >> "$extra" 2>&1
fi

# the standalone run's report folder as root's own directory: /Users/Shared
# is world-writable (and sticky), so nothing found at the name is trusted or
# moved: mkdir (it follows no link and fails when anything is there) makes
# it, and only a real directory of root's is used; anything else gets a
# fresh mktemp directory next to it (ECC review round 3: moving a planted
# link, or install -d through one, let a user make root write anywhere)
safe_outdir() {   # safe_outdir <dir>: prints the directory to use
    _o=$1
    mkdir -m 0755 "$_o" 2>/dev/null
    if [ -d "$_o" ] && [ ! -L "$_o" ] && [ "$(stat -f %u "$_o" 2>/dev/null)" = 0 ]; then
        echo "$_o"
        return 0
    fi
    _t=$(mktemp -d "$_o.XXXXXX") || return 1
    chmod 755 "$_t"
    echo "!! $_o is not root's own directory (a link, a file or somebody else's): the report goes to $_t" >&2
    echo "$_t"
}
if [ -n "$STANDALONE" ]; then
    # the names the VM's uploads get (docs/AFTER-TESTS.md reads them)
    out=/Users/Shared/rdna3-reports
    out=$(safe_outdir "$out") || { echo "!! cannot make a report directory in /Users/Shared: the report stays in $HOME"; out=$HOME; }
    cp "$extra" "$out/$base-tests.txt"
    for p in "$HOME"/rdna3-scanout/*/scanout-pipe*.ppm; do
        [ -f "$p" ] && cp "$p" "$out/$base-scanout$(basename "$(dirname "$p")")-$(basename "$p" | sed 's/^scanout-//')"
    done
    [ -s "$HOME/rdna3-bisect/bisect.txt" ] && cp "$HOME/rdna3-bisect/bisect.txt" "$out/$base-bisect.txt"
    [ -s "$HOME/rdna3-ladder/ladder.txt" ] && cp "$HOME/rdna3-ladder/ladder.txt" "$out/$base-ladder.txt"
    prog "tests done - the full report (up to 9 min)"
    RDNA3_STAMP=$stamp with_limit 540 sh "$D/tools/macos/collect.sh" "$label"
    [ -f "$HOME/$base.tar.gz" ] && mv "$HOME/$base.tar.gz" "$out/"
    plugin_trace_conf_off
    chmod -R a+rX "$out"
    ls -l "$out"/"$base"*
    prog "all done: $out/$base*"
    notify "All done: the report is in $out"
    exit 0
fi

# the test results and pictures go up first: they matter most, and
# collect.sh (log show, system_profiler on a GPU the hang test just reset)
# can run out of time. The archive gets the same name.
prog "tests done - sending the results, then the full report (up to 9 min)"
# what could not go up now (no network after a sleep) goes up at the next boot
pend_cp "$extra" "/var/db/rdna3-pending-$base-tests.txt"
curl -sf --max-time 30 -T "$extra" "$HOST/reports/$base-tests.txt" && echo "uploaded $base-tests.txt" &&
    rm -f "/var/db/rdna3-pending-$base-tests.txt"
for p in "$HOME"/rdna3-scanout/*/scanout-pipe*.ppm; do
    [ -f "$p" ] && curl -sf --max-time 30 -T "$p" \
        "$HOST/reports/$base-scanout$(basename "$(dirname "$p")")-$(basename "$p" | sed 's/^scanout-//')"
done
# the display bisect's table once more (bisect_push sent it after every step)
[ -s "$HOME/rdna3-bisect/bisect.txt" ] && curl -sf --max-time 30 -T "$HOME/rdna3-bisect/bisect.txt" "$HOST/reports/$base-bisect.txt" >/dev/null 2>&1
# the A/B ladder's table once more (ladder_push sent it after every rung)
[ -s "$HOME/rdna3-ladder/ladder.txt" ] && curl -sf --max-time 30 -T "$HOME/rdna3-ladder/ladder.txt" "$HOST/reports/$base-ladder.txt" >/dev/null 2>&1
# collect.sh packs everything from ~/rdna3-*
RDNA3_STAMP=$stamp with_limit 540 sh "$D/tools/macos/collect.sh" "$label"
f="$HOME/$base.tar.gz"
[ -f "$f" ] && pend_cp "$f" "/var/db/rdna3-pending-$base.tar.gz"
[ -f "$f" ] && curl -sf --max-time 60 -T "$f" "$HOST/reports/$base.tar.gz" && echo "uploaded $f" &&
    rm -f "/var/db/rdna3-pending-$base.tar.gz"
if [ -f "$f" ]; then prog "report sent - all done"; notify "All done, report sent"
else prog "the full report did not finish (the test results were sent)"; notify "Done, without the full report"; fi
# nothing reached the host (no network since the system sleep): restart
# once, the next boot sends what is waiting and stops
if ls /var/db/rdna3-pending-* >/dev/null 2>&1; then
    case " $args " in
        *" rdna3resume=1 "*" rdna3vmauto=1 "*|*" rdna3vmauto=1 "*" rdna3resume=1 "*)
            prog "no network since the system sleep: restarting to send the results"
            touch /var/db/rdna3-upload-only
            shutdown -r now
            sleep 60 ;;
    esac
fi
# step play: the second report, then the VM stays up (no shutdown)
if [ -n "$PLAY" ]; then play_second; exit 0; fi
case " $args " in *" rdna3vmauto=1 "*) prog "shutting down in 10 s"; sleep 10 ;; esac
finish
