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
        with_limit 60 kmutil load -p "$k" 2>&1 | tail -6
    done
    echo "-- kernelmanagerd / kmutil errors this boot"
    with_limit 60 log show --last boot --style compact \
        --predicate '(process == "kernelmanagerd" OR process == "kmutil" OR process == "syspolicyd") AND (messageType == error OR eventMessage CONTAINS[c] "invalid" OR eventMessage CONTAINS[c] "collection")' \
        2>/dev/null | tail -40
    echo "-- a test build of the Auxiliary KC into /tmp (nothing installed)"
    with_limit 120 kmutil create -n aux -a x86_64 -V release \
        --boot-path /System/Library/KernelCollections/BootKernelExtensions.kc \
        --system-path /System/Library/KernelCollections/SystemKernelExtensions.kc \
        --aux-path /tmp/rdna3-aux-test.kc -r /Library/Extensions 2>&1 | tail -25
    rm -f /tmp/rdna3-aux-test.kc
    echo "-- kmutil create / install options"
    kmutil create --help 2>&1 | head -60
    kmutil install --help 2>&1 | head -40
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
} > "$extra" 2>&1

# collect.sh packs everything from ~/rdna3-*; put the extra file next to its logs
with_limit 180 sh "$D/tools/macos/collect.sh" "$label"
f=$(ls -t "$HOME"/rdna3-report-*.tar.gz 2>/dev/null | head -1)
if [ -n "$f" ]; then
    curl -sf --max-time 60 -T "$f" "$HOST/reports/$(basename "$f")" && echo "uploaded $f"
    curl -sf --max-time 30 -T "$extra" "$HOST/reports/$(basename "$f" .tar.gz)-tests.txt" || true
fi
finish
