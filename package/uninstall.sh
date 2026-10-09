#!/bin/bash
# Removes everything package/install.sh installed. Reboot afterwards.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
rm -rf /Library/Extensions/RDNA3Accel.kext /Library/Extensions/RDNA3Accelerator.kext /Library/Extensions/RDNA3Framebuffer.kext /Library/GPUBundles/RDNA3MTLDriver.bundle \
       /Library/GPUBundles/rdna3mtl /Library/GPUBundles/rdna3mtl-allow.txt
kmutil install --update-all --check-rebuild || true
echo "Removed. Reboot."
