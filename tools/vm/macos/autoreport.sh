#!/bin/sh
# Boot-time report for the VM test (LaunchDaemon org.rdna3.vmreport, root).
# Waits for the boot to settle, runs tools/macos/collect.sh, adds the
# rdna3_drminfo DMA/GFX tests on the levels that have them, uploads the
# archive to the host (QEMU user network: host = 10.0.2.2) and shuts down
# when the host started the VM with rdna3vmauto=1.
D="/Library/Application Support/RDNA3VM"
HOST=http://10.0.2.2:8000
export HOME=/var/root
sleep 90

args=$(nvram boot-args 2>/dev/null | cut -f2-)
label=$(echo "$args" | tr ' ' '\n' | grep -E '^rdna3(lvl|dmub|msi|fb|accel)=' | tr '=' '-' | tr '\n' '_' | sed 's/_$//')
[ -n "$label" ] || label=no-rdna3
lvl=$(echo "$args" | sed -n 's/.*rdna3lvl=\([0-9]*\).*/\1/p')

extra="$HOME/rdna3-extra.txt"
{
    echo "boot-args: $args"
    if [ "${lvl:-0}" -ge 7 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        echo "== rdna3_drminfo --dma-test"
        "$D/bin/rdna3_drminfo" --dma-test 2>&1
        echo "[exit $?]"
    fi
    if [ "${lvl:-0}" -ge 8 ] && [ -x "$D/bin/rdna3_drminfo" ]; then
        echo "== rdna3_drminfo --gfx-test"
        "$D/bin/rdna3_drminfo" --gfx-test 2>&1
        echo "[exit $?]"
    fi
} > "$extra" 2>&1

# collect.sh packs everything from ~/rdna3-*; put the extra file next to its logs
sh "$D/tools/macos/collect.sh" "$label"
f=$(ls -t "$HOME"/rdna3-report-*.tar.gz 2>/dev/null | head -1)
if [ -n "$f" ]; then
    curl -sf -T "$f" "$HOST/reports/$(basename "$f")" && echo "uploaded $f"
    curl -sf -T "$extra" "$HOST/reports/$(basename "$f" .tar.gz)-tests.txt" || true
fi
case " $args " in
    *" rdna3vmauto=1 "*) shutdown -h now ;;
esac
