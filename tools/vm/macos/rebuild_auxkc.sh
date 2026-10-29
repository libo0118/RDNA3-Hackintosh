#!/bin/sh
# rebuild_auxkc.sh - rebuild macOS's Auxiliary Kernel Collection from an explicit
# list of kexts, in the card VM (sudo; from the host share:
#   curl -s http://10.0.2.2:8000/tools/rebuild_auxkc.sh | sudo sh
# ).
#
# Why: 2026-10-29 kernelmanagerd's own rebuild failed on every boot with
# "Could not find: Extension with identifier 'com.apple.driver.AppleKIS'" - the
# Xcode device frameworks register /Library/Extensions/AppleKIS.kext and
# Astris-IOUSBHostFamily.kext as extra kext paths, and those bundles are not on
# the disk. The collection then stayed at the old build, macOS ran the old
# framebuffer/accelerator and never asked for "Allow". `kmutil install
# --update-all` also rebuilds the boot/system collections and stops without a
# KDK. This builds only the auxiliary collection, explicitly (-x), from the kexts
# the current one was built from (its kcgen instructions) plus ours, skipping
# any path that is not there; the old collection is kept next to it.
set -u
[ "$(id -u)" = 0 ] || { echo "rebuild_auxkc: run it with sudo"; exit 1; }
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
INS=/private/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/com.apple.kcgen.instructions.plist
list=""
for k in $(plutil -p "$INS" 2>/dev/null | sed -n 's/.*"bundlePathMainOS" => "\(.*\)"/\1/p') \
         /Library/Extensions/RDNA3Accelerator.kext /Library/Extensions/RDNA3Framebuffer.kext \
         /Library/Extensions/RDNA3AGDC.kext; do
    [ -d "$k" ] || { echo "rebuild_auxkc: skipped, not on the disk: $k"; continue; }
    case " $list " in *" $k "*) continue ;; esac
    list="$list $k"
done
[ -n "$list" ] || { echo "rebuild_auxkc: no kext to build from"; exit 1; }
args=""
for k in $list; do args="$args -p $k"; done
[ -f "$AUX" ] && cp -p "$AUX" "$AUX.rdna3-old" && echo "rebuild_auxkc: the old collection kept as $AUX.rdna3-old"
echo "rebuild_auxkc: building the auxiliary collection from:$list"
# shellcheck disable=SC2086
kmutil create -n aux -x \
    -B /System/Library/KernelCollections/BootKernelExtensions.kc \
    -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
    -A "$AUX" $args
rc=$?
ls -la "$AUX"
if [ "$rc" = 0 ]; then
    echo "rebuild_auxkc: done - if System Settings > Privacy & Security asks to Allow, allow it; then shut down and run the step"
else
    echo "rebuild_auxkc: kmutil create failed (exit $rc): send this output"
fi
exit "$rc"
