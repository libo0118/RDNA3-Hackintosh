#!/bin/sh
# Boot-time report for the VM test (LaunchDaemon org.rdna3.vmreport, root).
# Waits for the boot to settle, runs tools/macos/collect.sh, adds the
# rdna3_drminfo DMA/GFX tests on the levels that have them, uploads the
# archive to the host (QEMU user network: host = 10.0.2.2) and shuts down
# when the host started the VM with rdna3vmauto=1.
D="/Library/Application Support/RDNA3VM"
HOST=http://10.0.2.2:8000
export HOME=/var/root

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
        [ -f "$1/$f" ] && ! cmp -s "$1/$f" "$m/$f" && cp "$1/$f" "$m/$f" && touch "$m/.changed"
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

if [ "${1:-}" != --updated ]; then
    sleep 90
    # newest report scripts from the host (rdna3vm start_share); a new
    # autoreport.sh takes over right away
    for f in collect.sh ioreg_discovery.sh autoreport.sh; do
        case $f in autoreport.sh) dst="$D/$f" ;; *) dst="$D/tools/macos/$f" ;; esac
        curl -sf --max-time 10 "$HOST/tools/$f" -o "$dst.new" && [ -s "$dst.new" ] &&
            { cmp -s "$dst.new" "$dst" || { mv "$dst.new" "$dst"; [ $f = autoreport.sh ] && exec sh "$dst" --updated; }; }
        rm -f "$dst.new"
    done
fi

# a new build on the host (rdna3vm prepare): install its kexts like
# setup.sh does. OpenCore injects RDNA3Accel itself, but the framebuffer
# and the accelerator link IOGraphicsFamily, so they load from the
# Auxiliary KC, which kernelmanagerd rebuilds from /Library/Extensions:
# the new copies run from the next boot on.
updated=
new=$(curl -sf --max-time 10 "$HOST/rdna3-bundle.sha256" | cut -c1-64)
if [ -n "$new" ] && [ "$new" != "$(cat "$D/bundle.sha256" 2>/dev/null)" ]; then
    t=$(mktemp -d /tmp/rdna3b.XXXXXX)
    if curl -sf --max-time 120 "$HOST/rdna3-bundle.tar.gz" | tar -xz -C "$t" && [ -d "$t/kexts" ]; then
        for k in "$t"/kexts/*.kext; do
            n=$(basename "$k")
            rm -rf "/Library/Extensions/$n" "$D/kexts/$n"
            mkdir -p "$D/kexts"
            cp -R "$k" "$D/kexts/"
            cp -R "$k" /Library/Extensions/
            chown -R root:wheel "/Library/Extensions/$n"
            chmod -R go-w "/Library/Extensions/$n"
            codesign --force --deep --sign - "/Library/Extensions/$n" >/dev/null 2>&1
            kmutil load -p "/Library/Extensions/$n" >/dev/null 2>&1
        done
        mkdir -p "$D/bin"
        cp "$t"/bin/* "$D/bin/" 2>/dev/null && chmod +x "$D/bin/"*
        echo "$new" > "$D/bundle.sha256"
        updated="kexts updated to the host's new bundle: the framebuffer from it runs from the next boot"
    fi
    rm -rf "$t"
    # an interactive boot (rdna3vm approve / window, no rdna3vmauto): the new
    # framebuffer/accelerator wait for "Allow" in Privacy & Security, which
    # only shows up now - open it for whoever is logged in and say so
    case " $(nvram boot-args 2>/dev/null | cut -f2-) " in
        *" rdna3vmauto=1 "*) ;;
        *)
            if [ -n "$updated" ]; then
                u=$(stat -f %Su /dev/console 2>/dev/null)
                uid=$(id -u "$u" 2>/dev/null)
                if [ -n "$uid" ] && [ "$u" != root ]; then
                    launchctl asuser "$uid" sudo -u "$u" open "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension" 2>/dev/null
                    launchctl asuser "$uid" sudo -u "$u" osascript -e 'display dialog "RDNA3: new kexts installed. In Privacy & Security scroll down and click Allow (password), choose Not Now for the restart, then shut down." buttons {"OK"} with title "RDNA3VM approve"' >/dev/null 2>&1 &
                fi
            fi ;;
    esac
fi

args=$(nvram boot-args 2>/dev/null | cut -f2-)
label=$(echo "$args" | tr ' ' '\n' | grep -E '^rdna3(lvl|dmub|msi|fb|accel)=' | tr '=' '-' | tr '\n' '_' | sed 's/_$//')
[ -n "$label" ] || label=no-rdna3
lvl=$(echo "$args" | sed -n 's/.*rdna3lvl=\([0-9]*\).*/\1/p')

extra="$HOME/rdna3-extra.txt"
{
    echo "boot-args: $args"
    [ -n "$updated" ] && echo "== $updated"
    # why the Auxiliary KC keeps an old RDNA3Framebuffer (first card runs:
    # kernelmanagerd "invalid codeful kernel extension in the restricted
    # lookup table" for every /Library/Extensions copy)
    echo "== Auxiliary KC diagnostics"
    ls -l /Library/KernelCollections/ 2>&1
    kmutil showloaded --list-only 2>/dev/null | grep -i rdna3
    for n in RDNA3Framebuffer RDNA3Accelerator; do
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
            echo "== rdna3_drminfo --scanout (look $k)"
            with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$k" 2>&1
            echo "[exit $?]"
            [ $k = 3 ] || sleep 20
        done
        # display sleep (rdna3dpms=1): the heads must blank, then come back
        case " $args " in
            *" rdna3dpms=1 "*|*" rdna3dpms=2 "*)
                echo "== display sleep: pmset displaysleepnow"
                pmset displaysleepnow 2>&1
                sleep 8
                for k in sleep wake; do
                    mkdir -p "$HOME/rdna3-scanout/$k"
                    echo "== rdna3_drminfo --scanout (look $k)"
                    with_limit 30 "$D/bin/rdna3_drminfo" --scanout "$HOME/rdna3-scanout/$k" 2>&1
                    echo "[exit $?]"
                    if [ $k = sleep ]; then
                        echo "== display wake: caffeinate -u"
                        caffeinate -u -t 3 2>&1
                        sleep 8
                    fi
                done ;;
        esac
        # scaled modes (rdna3modes=1): each display through two smaller
        # modes, a look at the pipes after each switch, then native again
        case " $args " in
            *" rdna3modes=1 "*)
                if [ -x "$D/bin/rdna3_modetest" ]; then
                    echo "== rdna3_modetest list"
                    with_limit 30 "$D/bin/rdna3_modetest" list 2>&1
                    echo "[exit $?]"
                    echo "== rdna3_modetest cycle"
                    with_limit 150 "$D/bin/rdna3_modetest" cycle "$D/bin/rdna3_drminfo" "$HOME/rdna3-scanout" 2>&1
                    echo "[exit $?]"
                fi ;;
        esac
    fi
    if [ "${lvl:-0}" -ge 7 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        echo "== rdna3_drminfo --dma-test"
        with_limit 60 "$D/bin/rdna3_drminfo" --dma-test 2>&1
        echo "[exit $?]"
    fi
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        echo "== rdna3_drminfo --gfx-test"
        with_limit 60 "$D/bin/rdna3_drminfo" --gfx-test 2>&1
        echo "[exit $?]"
    fi
    # Vulkan: RADV (prepare --radv) straight through its ICD entry points,
    # no loader; libdrm_xnu talks to RDNA3Accel's user client
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_vktest" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
        echo "== rdna3_vktest"
        DYLD_LIBRARY_PATH="$D/bin" RADV_DEBUG=startup with_limit 90 "$D/bin/rdna3_vktest" \
            "$D/bin/libvulkan_radeon.dylib" 2>&1
        echo "[exit $?]"
    fi
    # Metal (build_metal.sh installed the plugin): devices and a compute
    # kernel through plugin -> translator -> RADV; the plugin's log after it
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/mtlprobe" ] && [ -d /Library/GPUBundles/RDNA3MTLDriver.bundle ]; then
        sync_metal "$D/bin"
        echo "== mtlprobe (MetalPluginName: $(ioreg -r -c RDNA3Accelerator -w0 | grep -c MetalPluginName))"
        rm -f /tmp/rdna3-nvmtl.log
        NVMTL_LOG_PATH=/tmp/rdna3-nvmtl.log with_limit 90 "$D/bin/mtlprobe" 2>&1
        echo "[exit $?]"
        echo "== plugin log (last 60 lines)"
        tail -60 /tmp/rdna3-nvmtl.log 2>/dev/null
    elif [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/mtlprobe" ]; then
        echo "== mtlprobe: plugin not installed (tools/build_metal.sh in the VM)"
        with_limit 30 "$D/bin/mtlprobe" --list 2>&1
    fi
    # soak (rdna3soak=<minutes>): DMA + GFX jobs and the Vulkan test in a
    # loop; the PM trend in the serial log runs alongside
    soak=$(printf '%s\n' $args | sed -n 's/^rdna3soak=\([0-9]*\)$/\1/p' | head -1)
    if [ -n "$soak" ] && [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        echo "== soak for $soak min"
        end=$(( $(date +%s) + soak * 60 )); n=0; bad=0
        while [ "$(date +%s)" -lt "$end" ]; do
            n=$((n + 1))
            with_limit 30 "$D/bin/rdna3_drminfo" --dma-test --gfx-test >/dev/null 2>&1 || bad=$((bad + 1))
            if [ -x "$D/bin/rdna3_vktest" ] && [ -f "$D/bin/libvulkan_radeon.dylib" ]; then
                DYLD_LIBRARY_PATH="$D/bin" with_limit 60 "$D/bin/rdna3_vktest" \
                    "$D/bin/libvulkan_radeon.dylib" >/dev/null 2>&1 || bad=$((bad + 1))
            fi
        done
        echo "SOAK: $n rounds, $bad failed"
    fi
    # last, it resets engines: a DMA and a GFX job that never finish, the
    # kernel must recover both (only when the step asks: rdna3hangtest=1)
    case " $args " in
        *" rdna3hangtest=1 "*)
            if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
                echo "== rdna3_drminfo --hang-test"
                with_limit 120 "$D/bin/rdna3_drminfo" --hang-test 2>&1
                echo "[exit $?]"
            fi ;;
    esac
} > "$extra" 2>&1

# the test results and pictures go up first: they matter most, and
# collect.sh (log show, system_profiler on a GPU the hang test just reset)
# can run out of time. The archive gets the same name.
stamp=$(date +%Y%m%d-%H%M%S)
base=rdna3-report-$label-$stamp
curl -sf --max-time 30 -T "$extra" "$HOST/reports/$base-tests.txt" && echo "uploaded $base-tests.txt"
for p in "$HOME"/rdna3-scanout/*/scanout-pipe*.ppm; do
    [ -f "$p" ] && curl -sf --max-time 30 -T "$p" \
        "$HOST/reports/$base-scanout$(basename "$(dirname "$p")")-$(basename "$p" | sed 's/^scanout-//')"
done
# collect.sh packs everything from ~/rdna3-*
RDNA3_STAMP=$stamp with_limit 360 sh "$D/tools/macos/collect.sh" "$label"
f="$HOME/$base.tar.gz"
[ -f "$f" ] && curl -sf --max-time 60 -T "$f" "$HOST/reports/$base.tar.gz" && echo "uploaded $f"
finish
