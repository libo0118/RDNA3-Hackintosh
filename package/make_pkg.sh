#!/bin/bash
# make_pkg.sh - the installer for a bare-metal Hackintosh: an Installer
# package (.pkg) and the same files as a tarball with install.sh, built from
# the CI artifacts or from a local build tree. Firmware is not inside unless
# --fw is given (docs/FIRMWARE.html): the package's preinstall downloads it.
#
#   package/make_pkg.sh                         from build/ (make kext; make -C fb; make -C accel; ...)
#   package/make_pkg.sh --from RDNA3Accel-kext.zip [--from RDNA3MTL.zip] [--from radv-macos-logs.zip]
#
#   --from DIR|ZIP   where to look for the parts (repeatable, first match wins;
#                    default: build/). Parts: RDNA3Accel.kext (required),
#                    RDNA3Framebuffer.kext, RDNA3Accelerator.kext, userspace tools
#                    and libdrm (RDNA3Accel-kext artifact), RDNA3MTLDriver.bundle +
#                    librdna3mtl_translate.dylib (RDNA3MTL), libvulkan_radeon.dylib
#                    (radv-macos-logs)
#   --fw DIR         bundle the firmware from linux-firmware's amdgpu/ (with its
#                    LICENSE.amdgpu, which must be there): for your own offline use
#   --out DIR        output directory (default build/pkg)
#   --version V      package version (default: CFBundleVersion of RDNA3Accel)
#   --no-pkg         only the tarball (automatic where pkgbuild is missing)
#
# The .pkg's installation check accepts macOS 15 and 26 (MACOS_MIN=15.0 up to, not including,
# MACOS_BEFORE=27.0; environment variables): the payload carries the Sequoia accelerator and the
# Tahoe one (kexts-tahoe, which the postinstall picks on Darwin 25).
#
# Output: RDNA3-Hackintosh-<version>-<commit>.pkg and .tar.gz. What gets
# installed where: docs/INSTALL-OPENCORE.md. macOS (pkgbuild, productbuild,
# install_name_tool); elsewhere only the tarball, for tests.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=$ROOT/package
MACOS_MIN=${MACOS_MIN:-15.0}
MACOS_BEFORE=${MACOS_BEFORE:-27.0}
SRCS=(); FW=""; OUT=$ROOT/build/pkg; VER=""; NOPKG=0
while [ $# -gt 0 ]; do
    case "$1" in
        --from) SRCS+=("$2"); shift 2 ;;
        --fw) FW=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --version) VER=$2; shift 2 ;;
        --no-pkg) NOPKG=1; shift ;;
        *) echo "unknown option $1 (see the header of $0)"; exit 2 ;;
    esac
done
[ ${#SRCS[@]} -gt 0 ] || SRCS=("$ROOT/build")
render_distribution() {   # render_distribution FILE VERSION: the installer script with the macOS range filled in
    sed -e "s/@VERSION@/$2/g" -e "s/@MACOS_MIN@/$MACOS_MIN/g" -e "s/@MACOS_BEFORE@/$MACOS_BEFORE/g" "$1"
}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
command -v pkgbuild >/dev/null 2>&1 && command -v productbuild >/dev/null 2>&1 || NOPKG=1
say() { echo "make_pkg: $*"; }

W=$(mktemp -d "${TMPDIR:-/tmp}/rdna3pkg.XXXXXX")
trap 'rm -rf "$W"' EXIT

# the sources as directories (CI artifacts are zips)
DIRS=()
n=0
for s in "${SRCS[@]}"; do
    if [ -d "$s" ]; then DIRS+=("$(cd "$s" && pwd)")
    elif [ -f "$s" ]; then
        n=$((n + 1)); mkdir -p "$W/src$n"
        if command -v ditto >/dev/null 2>&1; then ditto -x -k "$s" "$W/src$n"
        else python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$s" "$W/src$n"; fi
        DIRS+=("$W/src$n")
    else
        echo "no $s"; exit 1
    fi
done

# find_part TYPE NAME...: the shallowest match of any NAME in the sources,
# in source order (type d or f; never symlinks, never our own output)
find_part() {
    local t=$1 d hit; shift
    local expr=()
    for nm in "$@"; do expr+=(${expr[@]+-o} -name "$nm"); done
    for d in "${DIRS[@]}"; do
        hit=$(find "$d" \( -path "$OUT" -o -path '*/kext-obj' -o -path '*/accel-obj' -o -path '*/fb-obj' \
                           -o -path '*.dSYM' \) -prune -o -type "$t" \( "${expr[@]}" \) -print 2>/dev/null |
              awk '{ n = gsub("/", "/"); print n "\t" $0 }' | sort -n | head -1 | cut -f2-)
        [ -n "$hit" ] && { echo "$hit"; return 0; }
    done
    return 1
}

R=$W/root
A="$R/Library/Application Support/RDNA3"
GB=$R/Library/GPUBundles
M=$GB/rdna3mtl
mkdir -p "$R/Library/Extensions" "$A/bin" "$A/tools/macos" "$A/tools/vm" "$A/package" "$A/OpenCore"
parts=()

# --- kexts (never with firmware inside: a build tree after 'make -C kext
# firmware' has it in Resources; --fw puts it where the kext reads it)
for k in RDNA3Accel RDNA3Framebuffer RDNA3Accelerator RDNA3AGDC; do
    src=$(find_part d "$k.kext") || { [ $k = RDNA3Accel ] && { say "no RDNA3Accel.kext in ${SRCS[*]}"; exit 1; }
                                      [ $k = RDNA3AGDC ] && continue   # optional: only rdna3agdc=1 uses it
                                      say "WARNING: no $k.kext: the default profile needs it"; continue; }
    if [ ! -f "$src/Contents/MacOS/$k" ] || [ ! -f "$src/Contents/Info.plist" ]; then say "$src is incomplete"; exit 1; fi
    cp -R "$src" "$R/Library/Extensions/"
    rm -rf "$R/Library/Extensions/$k.kext/Contents/_CodeSignature"
    find "$R/Library/Extensions/$k.kext/Contents" -name '*.bin' -delete -o -name 'LICENSE.amdgpu' -delete
    parts+=("$k.kext")
done
# the accelerator's macOS 26 build (CI: RDNA3AcceleratorTahoe.kext): kept
# aside, the postinstall puts it in place of the other on macOS 26
if src=$(find_part d RDNA3AcceleratorTahoe.kext); then
    mkdir -p "$A/kexts-tahoe"
    cp -R "$src" "$A/kexts-tahoe/RDNA3Accelerator.kext"
    rm -rf "$A/kexts-tahoe/RDNA3Accelerator.kext/Contents/_CodeSignature"
    parts+=("RDNA3AcceleratorTahoe.kext")
fi
[ -n "$VER" ] || VER=$(python3 -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb")).get("CFBundleVersion", "0.1.0"))' \
                     "$R/Library/Extensions/RDNA3Accel.kext/Contents/Info.plist" 2>/dev/null || echo 0.1.0)
SHA=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
NAME=RDNA3-Hackintosh-$VER-$SHA

# --- the core's copy for OpenCore (code + Info.plist, as oc_install.sh copies)
mkdir -p "$A/OpenCore/RDNA3Accel.kext/Contents/MacOS"
cp "$R/Library/Extensions/RDNA3Accel.kext/Contents/Info.plist" "$A/OpenCore/RDNA3Accel.kext/Contents/"
cp "$R/Library/Extensions/RDNA3Accel.kext/Contents/MacOS/RDNA3Accel" "$A/OpenCore/RDNA3Accel.kext/Contents/MacOS/"
python3 "$P/oc_snippet.py" --kexts "$R/Library/Extensions" --all --plist > "$A/OpenCore/Kernel-Add.plist" 2>/dev/null

# --- userspace: tools in RDNA3/bin, libraries where libdrm_amdgpu and the
# Metal plugin look (/Library/GPUBundles/rdna3mtl), linked into bin for the
# tools' @executable_path rpath
mkdir -p "$M"
for t in rdna3_drminfo rdna3_modetest rdna3_pmctl rdna3_vktest mtlprobe; do
    f=$(find_part f "$t") && { cp "$f" "$A/bin/$t"; parts+=("$t"); }
done
lib() {   # lib INSTALLED-NAME FOUND-NAME...
    local to=$1 f; shift
    f=$(find_part f "$@") || return 1
    cp "$f" "$M/$to"
    ln -s "/Library/GPUBundles/rdna3mtl/$to" "$A/bin/$to"
    parts+=("$to")
}
lib libdrm.2.dylib libdrm.2.dylib libdrm.dylib || say "WARNING: no libdrm: the DMA/GFX tests and Vulkan cannot run"
lib libdrm_amdgpu.1.dylib libdrm_amdgpu.1.dylib libdrm_amdgpu.dylib || true
cp "$ROOT/third_party/libdrm/data/amdgpu.ids" "$M/"
if lib libvulkan_radeon.dylib libvulkan_radeon.dylib; then
    # RADV finds libdrm through this rpath (autoreport.sh sync_metal does the same)
    if command -v install_name_tool >/dev/null 2>&1; then
        install_name_tool -add_rpath /Library/GPUBundles/rdna3mtl "$M/libvulkan_radeon.dylib" 2>/dev/null || true
    else
        say "WARNING: no install_name_tool: RADV without the rdna3mtl rpath (build the package on macOS)"
    fi
    printf '{\n    "ICD": {\n        "api_version": "1.4.0",\n        "library_path": "%s"\n    },\n    "file_format_version": "1.0.0"\n}\n' \
        /Library/GPUBundles/rdna3mtl/libvulkan_radeon.dylib > "$M/radeon_icd.x86_64.json"
fi

# --- Metal plugin (only with the RDNA3MTL artifact or build/metal)
if b=$(find_part d RDNA3MTLDriver.bundle); then
    cp -R "$b" "$GB/"
    cp "$ROOT/metal/rdna3mtl-allow.txt" "$GB/"
    parts+=(RDNA3MTLDriver.bundle)
    if f=$(find_part f librdna3mtl_translate.dylib); then
        cp "$f" "$M/"
        cp "$ROOT/metal/translator/LICENSE" "$A/LICENSE.translator" 2>/dev/null || true
        parts+=(librdna3mtl_translate.dylib)
    fi
fi

# --- tools: report, firmware, uninstall, OpenCore helper
cp "$ROOT/tools/macos/collect.sh" "$ROOT/tools/macos/ioreg_discovery.sh" "$ROOT/tools/macos/collect_plugin_trace.sh" "$ROOT/tools/macos/fix_tracedir.sh" "$A/tools/macos/"
cp "$ROOT/tools/vm/macos/autoreport.sh" "$P/firmware.sh" "$P/uninstall.sh" "$A/"
cp "$P/oc_install.sh" "$A/package/"
cp "$ROOT/tools/vm/vmtool.py" "$A/tools/vm/"
cp "$ROOT/tools/ladder-steps.txt" "$A/tools/"
cat > "$A/oc_install.sh" <<'EOF'
#!/bin/sh
# package/oc_install.sh for this install: injects the RDNA3Accel.kext in
# OpenCore/ (the same build as /Library/Extensions) through EFI/OC/Kexts and
# Kernel/Add, or sets a ladder step's boot-args (--step N) or -rdna3off (--off),
# or takes our part out of OpenCore again (--remove).
#   sudo "/Library/Application Support/RDNA3/oc_install.sh" --efi /Volumes/EFI [--step N | --off | --remove]
# Run it again after every update. The firmware is already in place.
A=$(cd "$(dirname "$0")" && pwd)
case " $* " in *" --fw "*) echo "the firmware is installed already (firmware.sh)"; exit 2 ;; esac
RDNA3_BUILD="$A/OpenCore" exec bash "$A/package/oc_install.sh" "$@"
EOF
cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$ROOT/metal/LICENSE.PolyForm-Noncommercial-1.0.0" "$A/"

# --- firmware, only when asked (the license allows it with LICENSE.amdgpu)
FW_BUNDLED=0
if [ -n "$FW" ]; then
    sh "$P/firmware.sh" --dest "$A/firmware" --from "$FW"
    [ -f "$A/firmware/LICENSE.amdgpu" ] || { say "no LICENSE.amdgpu next to $FW: it must ship with the files"; exit 1; }
    FW_BUNDLED=1
    say "NOTE: the firmware is inside: keep this package for your own use (docs/FIRMWARE.html)"
fi

{
    echo "RDNA3-Hackintosh $VER ($SHA, built $(date -u +%Y-%m-%d))"
    echo "parts: ${parts[*]}"
    echo "firmware: $([ $FW_BUNDLED = 1 ] && echo bundled || echo 'downloaded at install (firmware.sh)')"
} > "$A/VERSION"

# --- scripts (the .pkg's, run by install.sh too)
S=$W/scripts
mkdir -p "$S"
cp "$P/pkg/preinstall" "$P/pkg/postinstall" "$P/firmware.sh" "$S/"
chmod 755 "$S"/* "$A"/*.sh "$A/package/oc_install.sh"
echo "FW_BUNDLED=$FW_BUNDLED" > "$S/manifest"
# what preinstall clears before the payload lands: everything the payload has
{
    for p in "$R"/Library/Extensions/* "$GB"/* "$A"/*; do
        [ "$p" = "$A/firmware" ] && [ $FW_BUNDLED = 0 ] && continue
        echo "${p#"$R"/}"
    done
} > "$S/remove.txt"

# --- tarball: install.sh + the same root and scripts
mkdir -p "$OUT"
T=$W/$NAME
mkdir -p "$T"
cp -R "$R" "$S" "$T/"
cp "$P/pkg/install.sh" "$P/uninstall.sh" "$P/firmware.sh" "$T/"
chmod 755 "$T"/*.sh
cat > "$T/README.txt" <<EOF
RDNA3-Hackintosh $VER ($SHA): kexts and tools for the AMD RX 7800 XT on macOS 15.
  sudo ./install.sh [--fw <linux-firmware>/amdgpu]     install (the same as the .pkg)
  sudo ./uninstall.sh [--efi /Volumes/EFI]             remove, OpenCore's copy and entries too
                                                       (mount the EFI first; --keep-opencore skips it)
Before the first boot read docs/INSTALL-OPENCORE.md in the repository
(SIP, amfi=0x80, OpenCore injection, the way back).
Parts: ${parts[*]}
EOF
COPYFILE_DISABLE=1 tar -czf "$OUT/$NAME.tar.gz" -C "$W" "$NAME"
say "$OUT/$NAME.tar.gz"

# --- the .pkg
if [ $NOPKG = 1 ]; then
    say "no pkgbuild here: tarball only"
    exit 0
fi
pkgbuild --analyze --root "$R" "$W/component.plist" >/dev/null
# bundles stay where they are installed (Installer would otherwise "relocate"
# them to any copy with the same bundle id it finds on the disk)
i=0
while plutil -extract "$i" xml1 -o /dev/null "$W/component.plist" 2>/dev/null; do
    plutil -replace "$i.BundleIsRelocatable" -bool NO "$W/component.plist"
    i=$((i + 1))
done
mkdir -p "$W/pkgs"
pkgbuild --root "$R" --component-plist "$W/component.plist" --scripts "$S" \
    --identifier io.github.voten641.rdna3 --version "$VER" --install-location / \
    --ownership recommended "$W/pkgs/rdna3.pkg" >/dev/null
mkdir -p "$W/res"
cp "$P/pkg/conclusion.html" "$W/res/"
{ cat "$ROOT/LICENSE"; echo; cat "$ROOT/NOTICE"; echo
  echo "The kexts' accelerator and framebuffer and the Metal plugin derive from NullMoth and are"
  echo "licensed under PolyForm Noncommercial 1.0.0 (installed next to this text in"
  echo "/Library/Application Support/RDNA3). AMD firmware: not part of this package unless built"
  echo "with --fw; downloaded from linux-firmware under LICENSE.amdgpu (docs/FIRMWARE.html)."; } > "$W/res/license.txt"
render_distribution "$P/pkg/distribution.xml" "$VER" > "$W/distribution.xml"
productbuild --distribution "$W/distribution.xml" --resources "$W/res" --package-path "$W/pkgs" \
    "$OUT/$NAME.pkg" >/dev/null
say "$OUT/$NAME.pkg"
