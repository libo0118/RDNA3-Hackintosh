#!/bin/bash
# build_metal.sh - build the Metal plugin (RDNA3MTLDriver.bundle) and the
# AIR->SPIR-V translator inside the macOS VM (rdna3vm window: no card
# needed) and install them with RADV into /Library/GPUBundles, where Metal
# loads the plugin from once the accelerator publishes MetalPluginName.
# Run build_radv.sh first (RADV in the report's bin). In the VM's Terminal:
#
#   bash <(curl -s http://10.0.2.2:8000/tools/build_metal.sh)
#
# First run installs Rust through rustup (no Homebrew). Takes 5-10 minutes.
# Remove with: sudo rm -rf /Library/GPUBundles/RDNA3MTLDriver.bundle \
#   /Library/GPUBundles/rdna3mtl /Library/GPUBundles/rdna3mtl-allow.txt
set -euo pipefail
HOST=http://10.0.2.2:8000
D="/Library/Application Support/RDNA3VM"
W=$HOME/rdna3-metal
GB=/Library/GPUBundles
DIR=$GB/rdna3mtl
say() { printf '\n== %s\n' "$*"; }

if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
    say "Xcode Command Line Tools are missing: a dialog opens - click Install, wait, then run this again"
    xcode-select --install || true
    exit 1
fi
[ -f "$D/bin/libvulkan_radeon.dylib" ] ||
    { echo "no RADV in $D/bin: run build_radv.sh first"; exit 1; }
[ -f "$D/bin/libdrm.2.dylib" ] && [ -f "$D/bin/libdrm_amdgpu.1.dylib" ] ||
    { echo "no libdrm in $D/bin: boot once with the current bundle (rdna3vm window), then run this again"; exit 1; }

say "sources from the host (rdna3vm window serves the checkout)"
# the translator's cargo build survives a re-run (minutes saved)
[ -d "$W/src/build/metal/xlate-target" ] && rm -rf "$W/xlate-target" && mv "$W/src/build/metal/xlate-target" "$W/"
rm -rf "$W/src" && mkdir -p "$W/src"
curl -sf "$HOST/rdna3-src.tar.gz" | tar -xz -C "$W/src"
[ -d "$W/xlate-target" ] && mkdir -p "$W/src/build/metal" && mv "$W/xlate-target" "$W/src/build/metal/"
[ -f "$W/src/metal/Makefile" ] || { echo "no sources at $HOST/rdna3-src.tar.gz (update rdna3vm: git pull, then rdna3vm window)"; exit 1; }

if ! command -v cargo >/dev/null && [ ! -x "$HOME/.cargo/bin/cargo" ]; then
    say "Rust through rustup (minimal profile, the release pinned in tools/ci/install_tools.sh)"
    sh "$W/src/tools/ci/install_tools.sh" rustup
fi
export PATH="$HOME/.cargo/bin:$PATH"
cargo --version

say "plugin + translator"
make -C "$W/src/metal" plugin translator > "$W/build.log" 2>&1 ||
    { tail -40 "$W/build.log"; echo "build failed: $W/build.log"; exit 1; }
B=$W/src/build/metal
[ -d "$B/RDNA3MTLDriver.bundle" ] && [ -f "$B/librdna3mtl_translate.dylib" ] ||
    { tail -40 "$W/build.log"; echo "no plugin/translator in $B"; exit 1; }

say "installing into $GB (password)"
sudo mkdir -p "$DIR"
sudo rm -rf "$GB/RDNA3MTLDriver.bundle"
sudo cp -R "$B/RDNA3MTLDriver.bundle" "$GB/"
sudo cp "$B/librdna3mtl_translate.dylib" "$DIR/"
sudo cp "$W/src/third_party/libdrm/data/amdgpu.ids" "$W/src/metal/rdna3mtl-allow.txt" "$DIR/"
sudo mv "$DIR/rdna3mtl-allow.txt" "$GB/"
# RADV + libdrm: the report refreshes these from its bin on every boot
sudo sh "$W/src/tools/vm/macos/autoreport.sh" --sync-metal "$D/bin"
sudo chown -R root:wheel "$DIR" "$GB/RDNA3MTLDriver.bundle" "$GB/rdna3mtl-allow.txt"
ls -la "$GB" "$DIR"
echo
echo "Done. Shut the VM down (sudo shutdown -h now); the report runs mtlprobe on the card"
echo "(a step with rdna3accel=1 and rdna3accelfb>=1 publishes MetalPluginName)."
