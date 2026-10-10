#!/bin/bash
# build_radv.sh - build RADV + rdna3_vktest inside the macOS VM (rdna3vm
# window: no card needed) instead of on a CI runner, and put them where the
# report runs them from step 8 on. In the VM's Terminal (as the normal user):
#
#   bash <(curl -s http://10.0.2.2:8000/tools/build_radv.sh)
#
# (not "curl | bash": Homebrew and sudo need the keyboard)
#
# First run: installs the Xcode Command Line Tools (a macOS dialog: click
# Install, then run the command again) and Homebrew (asks for your
# password). Mesa itself takes 10-20 minutes.
set -euo pipefail
HOST=http://10.0.2.2:8000
D="/Library/Application Support/RDNA3VM"
W=$HOME/rdna3-radv
say() { printf '\n== %s\n' "$*"; }

if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
    say "Xcode Command Line Tools are missing: a dialog opens - click Install, wait, then run this again"
    xcode-select --install || true
    exit 1
fi
if ! command -v brew >/dev/null; then
    for b in /usr/local/bin/brew /opt/homebrew/bin/brew; do [ -x "$b" ] && eval "$("$b" shellenv)" || true; done
fi
if ! command -v brew >/dev/null; then
    say "installing Homebrew (asks for your password)"
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    eval "$(/usr/local/bin/brew shellenv)"
fi
say "build tools"
brew install meson ninja glslang pkgconf python@3.12 >/dev/null
PY=$(brew --prefix python@3.12)/bin/python3.12
"$PY" -m pip install --quiet --break-system-packages mako pyyaml packaging
# meson runs Mesa's generators with the first python3 on PATH: this one has mako
export PATH="$(brew --prefix python@3.12)/libexec/bin:$(dirname "$PY"):$PATH"

say "sources from the host (rdna3vm window serves the checkout)"
rm -rf "$W/src" && mkdir -p "$W/src"
curl -sf "$HOST/rdna3-src.tar.gz" | tar -xz -C "$W/src"
[ -f "$W/src/userspace/mesa/build_radv.sh" ] || { echo "no sources at $HOST/rdna3-src.tar.gz (update rdna3vm: git pull, then rdna3vm window)"; exit 1; }

say "RADV (10-20 minutes)"
bash "$W/src/userspace/mesa/build_radv.sh" "$W/mesa" > "$W/build.log" 2>&1 ||
    { tail -40 "$W/build.log"; echo "build failed: $W/build.log"; exit 1; }
LIB=$W/mesa/install/lib/libvulkan_radeon.dylib
[ -f "$LIB" ] || { tail -40 "$W/build.log"; echo "no $LIB"; exit 1; }
grep -A50 "MISSING" "$W/build.log" | head -20
xcrun clang -O1 -g -arch x86_64 -mmacosx-version-min=15.0 -std=gnu11 -Wall -Wextra \
    -I "$W/mesa/src/include" -o "$W/rdna3_vktest" "$W/src/userspace/rdna3_vktest.c"

say "quick check on the loopback (no card): RADV must enumerate and pass the transfer queue"
make -C "$W/src/userspace" loopback >/dev/null
mkdir -p "$W/lb" && cp "$W/src/build/userspace/libdrm_loopback.dylib" "$W/lb/libdrm.2.dylib"
cp "$W/src/build/userspace/prefix/lib/libdrm_amdgpu.1.dylib" "$W/lb/"
DYLD_LIBRARY_PATH="$W/lb" "$W/rdna3_vktest" --loopback "$LIB" | tail -12 || true

say "installing into $D/bin (password)"
sudo mkdir -p "$D/bin"
sudo cp "$LIB" "$W/rdna3_vktest" "$D/bin/"
sudo cp "$W/src/build/userspace/prefix/lib/libdrm_amdgpu.1.dylib" "$D/bin/"
sudo cp "$W/src/build/userspace/prefix/lib/libdrm.2.dylib" "$D/bin/"
sudo chmod +x "$D/bin/rdna3_vktest"
echo
echo "Done. Shut the VM down (sudo shutdown -h now); from step 8 on the report runs rdna3_vktest on the card."
