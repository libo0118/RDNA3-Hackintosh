#!/bin/bash
# Installs this release without the .pkg: the same files and the same
# preinstall/postinstall scripts as the .pkg (package/make_pkg.sh builds
# both). In the unpacked release directory:
#
#   sudo ./install.sh [--fw <linux-firmware>/amdgpu]
#
# Firmware: kept when already installed, else copied from --fw, else
# downloaded from linux-firmware (firmware.sh). Then restart.
# Remove with: sudo "/Library/Application Support/RDNA3/uninstall.sh"
# (--root DIR installs into DIR instead of /: for tests, nothing is signed
# or registered there.)
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
ROOT=/; FW=""
while [ $# -gt 0 ]; do
    case "$1" in
        --fw) FW=$(cd "$2" && pwd); shift 2 ;;
        --root) ROOT=$2; shift 2 ;;
        *) echo "unknown option $1 (--fw DIR)"; exit 2 ;;
    esac
done
[ "$ROOT" != / ] || [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
if [ ! -d "$H/root/Library" ] || [ ! -f "$H/scripts/preinstall" ]; then
    echo "run it from the unpacked release directory"; exit 1
fi
mkdir -p "$ROOT"
RDNA3_FW_FROM="$FW" sh "$H/scripts/preinstall" "" "" "$ROOT"
cp -R "$H/root/." "$ROOT/"
sh "$H/scripts/postinstall" "" "" "$ROOT"
