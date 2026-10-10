#!/bin/sh
# kext_check.sh - syntax/type check of the kernel extensions on a Linux host
# with clang (-fsyntax-only, the flags of kext/Makefile) against a macOS
# SDK's Kernel.framework headers. Catches what CI would reject at compile
# time without a macOS runner; it does not link.
#
#   MACOS_SDK=/path/to/MacOSX11.3.sdk tools/kext_check.sh [files...]
#
# Default files: kext/src/*.cpp, fb/*.cpp and the hw/common C sources the
# kext builds. The SDK can come from github.com/phracker/MacOSX-SDKs.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${MACOS_SDK:?set MACOS_SDK to a MacOSX*.sdk directory}
KH=$SDK/System/Library/Frameworks/Kernel.framework/Headers
RES=$(clang -print-resource-dir)
COMMON="-target x86_64-apple-macos11.0 -fsyntax-only -isysroot $SDK -mkernel -nostdinc
 -I$KH -isystem $RES/include -I$ROOT/common -I$ROOT/hw -I$ROOT/uapi -I$ROOT/third_party/amd
 -I$ROOT/accel -I$ROOT/accel/iofam -I$ROOT/accel/re -I$ROOT/kext/src -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
 -DKEXT_VERSION=\"check\" -DKEXT_COMMIT=\"check\" -O2 -Wall -Wextra -Werror
 -Wno-unused-parameter -Wno-macro-redefined"
if [ $# -eq 0 ]; then
    set -- "$ROOT"/kext/src/*.cpp "$ROOT"/kext/src/*.c "$ROOT"/fb/*.cpp "$ROOT"/accel/*.cpp
fi
rc=0
for f in "$@"; do
    case "$f" in
    */accel/*.cpp)   # accel/Makefile: -Wall only, imported NullMoth code
        clang++ $(echo "$COMMON" | sed 's/-Wextra -Werror//') -std=gnu++17 -fapple-kext -fno-rtti \
            -fno-exceptions -fno-builtin -fno-common -Wno-inconsistent-missing-override \
            -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations "$f" 2>&1 |
            grep -E "error" && rc=1 ;;
    *.cpp) clang++ $COMMON -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions \
               -Wno-inconsistent-missing-override -Wno-deprecated-declarations "$f" || rc=1 ;;
    *.c)   clang $COMMON -std=gnu11 "$f" || rc=1 ;;
    esac
done
[ $rc = 0 ] && echo "kext_check: $# files ok"
exit $rc
