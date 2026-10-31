#!/bin/sh
# Install the RDNA3 driver on this Mac from the release tarball that sits next to this script
# on the stick (RDNA3-Hackintosh-*.tar.gz, the CI artifact RDNA3-pkg). Run inside macOS:
#
#   sudo sh /Volumes/OPENCORE/rdna3/install.sh [path/to/RDNA3-Hackintosh-*.tar.gz]
#
# Why the tarball and not the .pkg: the tarball runs the same preinstall / postinstall (firmware
# download, ad-hoc signatures, kernelmanagerd registration, the accelerator's Tahoe build on
# macOS 26) and needs no Installer.app. Messages to the owner are Polish.
# RDNA3_STICK_TMP: where the tarball is unpacked (default /private/tmp; for tests).
[ "$(id -u)" = 0 ] || { echo "uruchom z sudo: sudo sh $0"; exit 1; }
S=$(cd "$(dirname "$0")" && pwd)
T=${RDNA3_STICK_TMP:-/private/tmp}/rdna3-install
TGZ=${1:-}
if [ -z "$TGZ" ]; then
    # the newest one when there are several
    TGZ=$(ls -t "$S"/RDNA3-Hackintosh-*.tar.gz 2>/dev/null | head -1)
fi
[ -f "$TGZ" ] || { echo "brak RDNA3-Hackintosh-*.tar.gz w $S (skopiuj go z Linuksa)"; exit 1; }
rm -rf "$T"; mkdir -p "$T"
echo "[1/3] rozpakowanie $(basename "$TGZ")"
tar xzf "$TGZ" -C "$T" || { echo "nie da sie rozpakowac archiwum"; exit 1; }
REL=$(ls -d "$T"/RDNA3-Hackintosh-*/ 2>/dev/null | head -1)
[ -f "${REL}install.sh" ] || { echo "w archiwum nie ma RDNA3-Hackintosh-*/install.sh"; exit 1; }
echo "[2/3] instalacja (pobiera firmware AMD, potrzebny internet)"
bash "${REL}install.sh" || { echo "instalacja nie powiodla sie"; exit 1; }
echo "[3/3] sprawdzenie"
ls -d /Library/Extensions/RDNA3*.kext
ls "/Library/Application Support/RDNA3/firmware" 2>/dev/null | head -3 || echo "brak firmware"
echo "gotowe. Ustawienia systemowe -> Prywatnosc i ochrona -> Zezwol (jesli prosi), restart,"
echo "potem wylacz komputer i wroc do Linuksa (config-driver.plist jako config.plist)."
