#!/bin/sh
# Extract the IP discovery blob the kext published (rdna3lvl>=2) and decode it.
#   tools/macos/ioreg_discovery.sh [out.bin]
set -eu
out=${1:-navi32-discovery-macos.bin}
ioreg -a -r -c RDNA3Accel -k RDNA3,DiscoveryBinary > /tmp/rdna3-ioreg.plist
# python3 is only there with the Xcode tools; plutil always is
if ! /usr/bin/xcode-select -p >/dev/null 2>&1 || ! command -v python3 >/dev/null; then
    plutil -extract "0.RDNA3,DiscoveryBinary" raw -o - /tmp/rdna3-ioreg.plist 2>/dev/null |
        base64 -D > "$out" && [ -s "$out" ] && echo "wrote $(wc -c < "$out") bytes to $out" ||
        { echo "RDNA3Accel has no RDNA3,DiscoveryBinary (boot with rdna3lvl=2?)"; exit 1; }
else
python3 - "$out" <<'PY'
import plistlib, sys
with open('/tmp/rdna3-ioreg.plist', 'rb') as f:
    nodes = plistlib.load(f)
if not nodes:
    sys.exit("RDNA3Accel has no RDNA3,DiscoveryBinary (boot with rdna3lvl=2?)")
blob = nodes[0]['RDNA3,DiscoveryBinary']
open(sys.argv[1], 'wb').write(blob)
print(f"wrote {len(blob)} bytes to {sys.argv[1]}")
PY
fi
dump="$(dirname "$0")/../../build/host/amd_discovery_dump"
[ -x "$dump" ] && "$dump" "$out" || echo "run 'make tools' to decode it"
