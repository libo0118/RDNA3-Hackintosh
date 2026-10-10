#!/bin/bash
# build_radv.sh - build RADV + rdna3_vktest inside the macOS VM (rdna3vm
# window: no card needed) instead of on a CI runner, and put them where the
# report runs them from step 8 on. In the VM's Terminal (as the normal user):
#
#   bash <(curl -s http://10.0.2.2:8000/tools/build_radv.sh)
#
# (not "curl | bash": sudo needs the keyboard)
#
# First run: installs the Xcode Command Line Tools (a macOS dialog: click
# Install, then run the command again), Python 3.12 from python.org if
# there is no Python >= 3.10 (asks for your password), the build tools from
# PyPI and glslang from source (no Homebrew: it dropped Intel Macs). Mesa
# itself takes 10-20 minutes.
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
# no Homebrew: it no longer installs on Intel Macs (the VM is x86_64).
# Python 3.12 from python.org, the build tools from PyPI into a venv,
# glslangValidator built from source.
T=$W/tools
mkdir -p "$T"
PY=""
for p in python3.13 python3.12 python3.11 python3.10 /Library/Frameworks/Python.framework/Versions/3.12/bin/python3.12; do
    command -v "$p" >/dev/null && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null && PY=$(command -v "$p") && break
done
if [ -z "$PY" ]; then
    say "installing Python 3.12 from python.org (asks for your password)"
    curl -fL -o "$T/python.pkg" https://www.python.org/ftp/python/3.12.10/python-3.12.10-macos11.pkg
    sudo installer -pkg "$T/python.pkg" -target /
    PY=/Library/Frameworks/Python.framework/Versions/3.12/bin/python3.12
fi
say "build tools (meson, ninja, mako, pkgconf, cmake) into $T/venv"
[ -x "$T/venv/bin/python3" ] || "$PY" -m venv "$T/venv"
"$T/venv/bin/python3" -m pip install --quiet --upgrade pip
"$T/venv/bin/python3" -m pip install --quiet meson ninja mako pyyaml packaging pkgconf cmake
export PATH="$T/venv/bin:$T/bin:$PATH"
export PKG_CONFIG="$T/venv/bin/pkgconf"
if ! command -v glslangValidator >/dev/null; then
    say "glslangValidator from source (2-3 minutes)"
    rm -rf "$T/glslang"
    git -c advice.detachedHead=false clone -q --depth 1 --branch 15.1.0 https://github.com/KhronosGroup/glslang.git "$T/glslang"
    cmake -S "$T/glslang" -B "$T/glslang/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF \
        -DGLSLANG_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$T" >/dev/null
    ninja -C "$T/glslang/build" install >/dev/null
fi
glslangValidator --version | head -1

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
