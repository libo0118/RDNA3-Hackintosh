#!/bin/sh
# Boot-time report for the VM test (LaunchDaemon org.rdna3.vmreport, root).
# Waits for the boot to settle, runs tools/macos/collect.sh, adds the
# rdna3_drminfo DMA/GFX tests on the levels that have them, uploads the
# archive to the host (QEMU user network: host = 10.0.2.2) and shuts down
# when the host started the VM with rdna3vmauto=1.
D="/Library/Application Support/RDNA3VM"
HOST=http://10.0.2.2:8000
export HOME=/var/root
# --standalone: a bare-metal install (package/make_pkg.sh, docs/TESTING-HW.md),
# run by hand with sudo: the same tests and report, but no VM host - no
# wait, no updates, no uploads - the files land in /Users/Shared/rdna3-reports
STANDALONE=
if [ "${1:-}" = --standalone ]; then
    STANDALONE=1; HOST=; D="/Library/Application Support/RDNA3"; shift
    [ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
fi

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
sync_metal() {
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
    _a3=$(sysctl debug 2>/dev/null | grep -E "rdna3accel_(a3display|swaps_)")
    if [ -n "$_a3" ]; then echo "$_a3"
    else echo "(no rdna3accel_a3display sysctl: kext older than card run 58)"; fi
}

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

if [ "${1:-}" != --updated ] && [ -z "$STANDALONE" ]; then
    rm -f /tmp/rdna3-progress.win
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
    for f in collect.sh ioreg_discovery.sh autoreport.sh; do
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
done_() {   # done_ EXIT: the test's exit code and summary line
    echo "[exit $1]"
    partial
    sum=$(tail -n +"$((hl + 1))" "$extra" 2>/dev/null |
          grep -E '^(ALL PASSED|FAILED|NOTHING TESTED|METAL:|SOAK:|HANG TEST:|MODES:|GAMMA:|PMCTL:|BACO:|CTS:|PLATFORM:|WSI:|A3:|M4:|M5:|M6:|SOME CHECKS|Stage 4 UAPI)' | tail -1)
    if [ "$1" = 0 ]; then prog "    OK  ${sum}"; else prog "    FAIL (exit $1)  ${sum}"; fi
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
[ -n "$win" ] || { rm -f "$P"; touch "$P"; chmod 644 "$P"; }
[ -n "$win" ] && notify "Tests started: $(echo "$args" | tr ' ' '\n' | grep -c '^rdna3') rdna3 boot-args"
prog "boot-args: $(echo "$args" | tr ' ' '\n' | grep '^rdna3' | tr '\n' ' ')"
{
    echo "boot-args: $args"
    [ -n "$updated" ] && echo "== $updated"
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
    rm -rf "$HOME/rdna3-scanout"
    if [ "${lvl:-0}" -ge 2 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        for k in 1 2 3; do
            mkdir -p "$HOME/rdna3-scanout/$k"
            hdr "rdna3_drminfo --scanout (look $k)"
            with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$k" 2>&1
            done_ $?
            [ $k = 3 ] || sleep 20
        done
        # A3: another process's IOSurface must stay out of reach (ENOENT)
        hdr "rdna3_drminfo --surface-xproc"
        with_limit 30 "$D/bin/rdna3_drminfo" --surface-xproc 2>&1
        done_ $?
        # IOPresentment: does WindowServer hand the display pipes a plane
        # (iop_ok / iop_src_surf), and what did AGDC answer (rdna3agdc)
        echo "== display pipe and AGDC counters"
        sysctl debug 2>/dev/null | grep -E "rdna3accel_(iop_(ok|fail|empty|src_res|src_surf|blank|copy_skip|flips)|heads_)|rdna3agdc" || true
        a3_sysctls
        ioreg -r -c RDNA3AGDC -w0 -d1 2>/dev/null | grep -E '"RDNA3,AGDC' | sed 's/^ *//'
        # with AGDC up WindowServer needs the Metal plugin's device (card runs 41/45)
        echo "Metal allow list: $(grep -v '^#' /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null | tr '\n' ' ') ($(stat -f %Lp /Library/GPUBundles/rdna3mtl-allow.txt 2>/dev/null))"
        echo "Metal allow mode: $(rdna3_allow_mode)"
        # display sleep (rdna3dpms=1): the heads must blank, then come back
        case " $args " in
            *" rdna3dpms=1 "*|*" rdna3dpms=2 "*)
                hdr "display sleep: pmset displaysleepnow"
                pmset displaysleepnow 2>&1
                sleep 8
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
                done ;;
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
                        with_limit 180 in_session "$D/bin/rdna3_modetest" gamma "$D/bin/rdna3_drminfo" "$HOME/rdna3-scanout" 2>&1
                        r=$?; done_ $r; cache_keep $r
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
            DYLD_LIBRARY_PATH="$D/bin" with_limit 60 "$D/bin/rdna3_vktest" --wsi "$D/bin/libvulkan_radeon.dylib" > /tmp/rdna3-v3.txt 2>&1
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
            echo "== copy forward (kernel) during M6"
            with_limit 60 log show --last 3m --style compact --predicate 'eventMessage CONTAINS "copy forward"' 2>/dev/null | tail -30
            done_ 0
            [ "$m6ok" = 2 ] && cache_keep 0
            CFP=
            fi
        else
            echo "== probe: A3 / M4 / M5 / M6 need the Metal plugin (tools/build_metal.sh in the VM) and level 8"
        fi ;;
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
} > "$extra" 2>&1

if [ -n "$STANDALONE" ]; then
    # the names the VM's uploads get (docs/AFTER-TESTS.md reads them)
    out=/Users/Shared/rdna3-reports
    mkdir -p "$out"
    cp "$extra" "$out/$base-tests.txt"
    for p in "$HOME"/rdna3-scanout/*/scanout-pipe*.ppm; do
        [ -f "$p" ] && cp "$p" "$out/$base-scanout$(basename "$(dirname "$p")")-$(basename "$p" | sed 's/^scanout-//')"
    done
    prog "tests done - the full report (up to 6 min)"
    RDNA3_STAMP=$stamp with_limit 360 sh "$D/tools/macos/collect.sh" "$label"
    [ -f "$HOME/$base.tar.gz" ] && mv "$HOME/$base.tar.gz" "$out/"
    chmod -R a+rX "$out"
    ls -l "$out"/"$base"*
    prog "all done: $out/$base*"
    notify "All done: the report is in $out"
    exit 0
fi

# the test results and pictures go up first: they matter most, and
# collect.sh (log show, system_profiler on a GPU the hang test just reset)
# can run out of time. The archive gets the same name.
prog "tests done - sending the results, then the full report (up to 6 min)"
# what could not go up now (no network after a sleep) goes up at the next boot
cp "$extra" "/var/db/rdna3-pending-$base-tests.txt" 2>/dev/null
curl -sf --max-time 30 -T "$extra" "$HOST/reports/$base-tests.txt" && echo "uploaded $base-tests.txt" &&
    rm -f "/var/db/rdna3-pending-$base-tests.txt"
for p in "$HOME"/rdna3-scanout/*/scanout-pipe*.ppm; do
    [ -f "$p" ] && curl -sf --max-time 30 -T "$p" \
        "$HOST/reports/$base-scanout$(basename "$(dirname "$p")")-$(basename "$p" | sed 's/^scanout-//')"
done
# collect.sh packs everything from ~/rdna3-*
RDNA3_STAMP=$stamp with_limit 360 sh "$D/tools/macos/collect.sh" "$label"
f="$HOME/$base.tar.gz"
[ -f "$f" ] && cp "$f" "/var/db/rdna3-pending-$base.tar.gz" 2>/dev/null
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
case " $args " in *" rdna3vmauto=1 "*) prog "shutting down in 10 s"; sleep 10 ;; esac
finish
