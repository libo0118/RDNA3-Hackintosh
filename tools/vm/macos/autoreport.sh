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

args=$(nvram boot-args 2>/dev/null | cut -f2-)
label=$(echo "$args" | tr ' ' '\n' | grep -E '^rdna3(lvl|dmub|msi|fb|accel)=' | tr '=' '-' | tr '\n' '_' | sed 's/_$//')
[ -n "$label" ] || label=no-rdna3
lvl=$(echo "$args" | sed -n 's/.*rdna3lvl=\([0-9]*\).*/\1/p')

extra="$HOME/rdna3-extra.txt"
{
    echo "boot-args: $args"
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
