#!/bin/sh
# Install the Metal plugin + RADV (the card then draws the desktop) and switch the stick's
# OpenCore config to WindowServer on the plugin (rdna3agdc=3), as the card VM runs it
# (tools/vm/macos/build_metal.sh does the same in the VM). Inside macOS, after install.sh and
# the restart with config-driver.plist:
#
#   sudo sh /Volumes/OPENCORE/rdna3/metal.sh
#
# Needs next to this script, in metal/ (the CI artifacts RDNA3MTL and radv-macos-logs, unpacked):
#   RDNA3MTLDriver.bundle  librdna3mtl_translate.dylib  libvulkan_radeon.dylib  rdna3mtl-allow.txt
# The way back: in Linux, EFI/OC/config-before-metal.plist as config.plist (made on the first
# run only; a second run does not overwrite it). Messages to the owner are Polish.
# RDNA3_SYSROOT: install under that directory instead of / (tests); RDNA3_OC: the config.plist.
[ "$(id -u)" = 0 ] || { echo "uruchom z sudo: sudo sh $0"; exit 1; }
S=$(cd "$(dirname "$0")" && pwd)
R=${RDNA3_SYSROOT:-}
GB=$R/Library/GPUBundles
DIR=$GB/rdna3mtl
A="$R/Library/Application Support/RDNA3"
STICK=${S%/rdna3}
OC=${RDNA3_OC:-$STICK/EFI/OC/config.plist}
[ -f "$OC" ] || OC=/Volumes/OPENCORE/EFI/OC/config.plist
[ -d "$DIR" ] && [ -f "$A/autoreport.sh" ] || { echo "najpierw sudo sh $S/install.sh"; exit 1; }
[ -f "$OC" ] || { echo "nie widze EFI/OC/config.plist pendrive'a (sudo diskutil mount <dysk> i jeszcze raz)"; exit 1; }
for f in RDNA3MTLDriver.bundle librdna3mtl_translate.dylib libvulkan_radeon.dylib rdna3mtl-allow.txt; do
    [ -e "$S/metal/$f" ] || { echo "brak $S/metal/$f (skopiuj artefakty RDNA3MTL i radv z Linuksa)"; exit 1; }
done

echo "[1/4] plugin Metal"
rm -rf "$GB/RDNA3MTLDriver.bundle"
cp -R "$S/metal/RDNA3MTLDriver.bundle" "$GB/" || { echo "kopiowanie plugina nie powiodlo sie"; exit 1; }
cp "$S/metal/librdna3mtl_translate.dylib" "$DIR/"
cp "$S/metal/rdna3mtl-allow.txt" "$GB/"

echo "[2/4] RADV (Vulkan)"
T=${RDNA3_STICK_TMP:-/private/tmp}/rdna3-radv
rm -rf "$T"; mkdir -p "$T"
cp "$S/metal/libvulkan_radeon.dylib" "$T/"
sh "$A/autoreport.sh" --sync-metal "$T" || echo "UWAGA: --sync-metal zwrocil blad"
ls -la "$DIR"

echo "[3/4] uprawnienia"
chown -R root:wheel "$DIR" "$GB/RDNA3MTLDriver.bundle" "$GB/rdna3mtl-allow.txt"
# WindowServer (_windowserver) must read the plugin and the allow list
chmod -R go-w,a+rX "$GB/RDNA3MTLDriver.bundle" "$DIR"
chmod 644 "$GB/rdna3mtl-allow.txt"
sh "$A/tools/macos/fix_tracedir.sh" ${R:+"$R"} || echo "UWAGA: katalog sladow klatek nie ustawiony"

echo "[4/4] OpenCore: rdna3agdc=3 (WindowServer na pluginie)"
K=NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args
cur=$(plutil -extract "$K" raw -o - "$OC") || { echo "nie moge odczytac boot-args z $OC"; exit 1; }
BAK=${OC%.plist}-before-metal.plist
# the first run's copy is the way back; a second run must not replace it with the changed file
if [ -e "$BAK" ]; then
    echo "kopia $(basename "$BAK") juz jest, zostaje"
else
    cp "$OC" "$BAK" || { echo "nie moge zrobic kopii $BAK"; exit 1; }
fi
case " $cur " in
*" rdna3agdc="*) new=$(echo "$cur" | sed 's/rdna3agdc=[0-9]*/rdna3agdc=3/') ;;
*) new="$cur rdna3agdc=3" ;;
esac
plutil -replace "$K" -string "$new" "$OC" || { echo "nie udalo sie zmienic $OC"; exit 1; }
echo "boot-args: $new"
sync
echo "gotowe. Uruchom ponownie komputer (z pendrive'a, wpis macOS)."
echo "Gdyby ekran zgasl: w Linuksie przywroc EFI/OC/config-before-metal.plist jako config.plist."
