#!/bin/sh
# firmware.sh - the AMD microcode RDNA3Accel loads (docs/FIRMWARE.html). The
# project does not ship it: this script downloads it from linux-firmware
# (a pinned tag, SHA-256 checked) or copies it from a local linux-firmware
# checkout, into /Library/Application Support/RDNA3/firmware, where the kext
# reads it from disk (kFwDirs in kext/src/RDNA3Bringup.cpp). LICENSE.amdgpu
# goes next to the blobs. The .pkg's preinstall and install.sh run it.
#
#   firmware.sh [--dest DIR] --check       all files there and sane? (exit 1 if not)
#   firmware.sh [--dest DIR] --from DIR    copy from linux-firmware's amdgpu/ (plain .bin)
#   firmware.sh [--dest DIR] --fetch       download (RDNA3_FW_REF=<tag|main> to change the tag)
#   firmware.sh [--dest DIR]               --check, else --fetch
#
# Sane = the size in the common firmware header (first 32-bit word) equals
# the file size, so an HTML error page or a truncated download never lands.
set -u
FILES="psp_13_0_10_sos.bin psp_13_0_10_ta.bin smu_13_0_10.bin sdma_6_0_3.bin gc_11_0_3_imu.bin
gc_11_0_3_pfp.bin gc_11_0_3_me.bin gc_11_0_3_mec.bin gc_11_0_3_rlc.bin
gc_11_0_3_mes_2.bin gc_11_0_3_mes1.bin dcn_3_2_0_dmcub.bin vcn_4_0_0.bin"
PIN=20260916
REF=${RDNA3_FW_REF:-$PIN}
URL=${RDNA3_FW_URL:-https://gitlab.com/kernel-firmware/linux-firmware/-/raw}
# SHA-256 of the files at linux-firmware tag $PIN (checked only for that tag)
SUMS="psp_13_0_10_sos.bin 0bcaaad9cd8578d3841ae69155a6bd4fc3ceae8f4fb5a6ba4f576e7ace94d1d9
psp_13_0_10_ta.bin 4a8c32aad88e028b70fcc220a859a1603684949782ac2cd09e8fa1cc5e46ff7a
smu_13_0_10.bin d1c59c86d7d2b021218fa6a5c24a64057d39a86e115c8be4b99938499baae28e
sdma_6_0_3.bin d47ac4db523aa35d77b27d57c35d4c04f431229dec6c0d667c36d98b985a6933
gc_11_0_3_imu.bin beaf704d5acdf4623456b0d0cbcea8b8e428058340cd922a259a9045f5c457a3
gc_11_0_3_pfp.bin 362db904fa16c1fea2af7ad1295532434df7f85662b4a69332f51ae6c7290b61
gc_11_0_3_me.bin d02c25070e5bdf0ec0146f5c9d6d2f8b86de43bd2a318a0b67eb5201963bafdc
gc_11_0_3_mec.bin a37bc1a4e245300a5c3e26da34ea213842447d7df6c5c81e9fc78887a2fde26f
gc_11_0_3_rlc.bin 890d8e0123efb40c0179dd8ac3e9af073a0b87cbbccfec1db54e5ed2315a8d39
gc_11_0_3_mes_2.bin d3e08b7f6b54210c288bb2ce35f0a6f5bca00cbef74a2d121aa0a309292635f2
gc_11_0_3_mes1.bin 26441f08db0babb4660f1278e92796754f1e8375a6fb1db13a132bd79932f550
dcn_3_2_0_dmcub.bin 858ff09f406688c7cfd9ec749de37fb26d6659b078617a628c259aa86c91b0bc
vcn_4_0_0.bin 0312642f73717256299524f9ace75320ceb098b43609c07ce90ba24b47e7c6a5"

DEST="/Library/Application Support/RDNA3/firmware"
MODE=auto; SRC=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dest) DEST=$2; shift 2 ;;
        --check) MODE=check; shift ;;
        --from) MODE=copy; SRC=$2; shift 2 ;;
        --fetch) MODE=fetch; shift ;;
        --list) for f in $FILES; do echo "$f"; done; exit 0 ;;
        *) echo "firmware.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
say() { echo "firmware: $*"; }
NF=$(echo "$FILES" | wc -w | tr -d ' ')

# sane FILE: the header's size_bytes is the file size
sane() {
    [ -f "$1" ] || return 1
    n=$(wc -c < "$1" | tr -d ' ')
    h=$(od -An -tu4 -N4 "$1" 2>/dev/null | tr -d ' \n')
    [ "$n" -ge 4096 ] && [ "$h" = "$n" ]
}

sha256() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else sha256sum "$1" | cut -d' ' -f1; fi
}

# check DIR: every file there and sane; prints what is missing
check() {
    bad=0
    for f in $FILES; do
        sane "$1/$f" || { say "missing or damaged: $1/$f"; bad=1; }
    done
    return $bad
}

# place TMPDIR: verified files from TMPDIR into DEST (only when all are good)
place() {
    check "$1" || return 1
    mkdir -p "$DEST" || return 1
    for f in $FILES LICENSE.amdgpu; do
        [ -f "$1/$f" ] && { cp "$1/$f" "$DEST/$f.new" && mv -f "$DEST/$f.new" "$DEST/$f" || return 1; }
    done
    chmod 755 "$DEST"; chmod 644 "$DEST"/* 2>/dev/null
    if [ "$(id -u)" = 0 ] && [ "$(uname -s)" = Darwin ]; then chown -R root:wheel "$DEST"; fi
    [ -f "$DEST/LICENSE.amdgpu" ] || say "NOTE: no LICENSE.amdgpu next to the files (its terms: docs/FIRMWARE.html)"
    say "$NF files in $DEST"
}

tmp=""
cleanup() { [ -n "$tmp" ] && rm -rf "$tmp"; }
trap cleanup EXIT
mktmp() { tmp=$(mktemp -d "${TMPDIR:-/tmp}/rdna3fw.XXXXXX") || exit 1; }

case $MODE in
    check)
        check "$DEST" && say "all $NF files in $DEST" ;;
    copy)
        [ -d "$SRC" ] || { say "no directory $SRC"; exit 1; }
        mktmp
        for f in $FILES; do
            if [ -f "$SRC/$f" ]; then cp "$SRC/$f" "$tmp/$f"
            else say "not in $SRC: $f (compressed .bin.zst/.xz copies need unpacking first)"; fi
        done
        for l in "$SRC/LICENSE.amdgpu" "$SRC/../LICENSE.amdgpu" "$SRC/../LICENSES/LICENSE.amdgpu"; do
            [ -f "$l" ] && { cp "$l" "$tmp/LICENSE.amdgpu"; break; }
        done
        place "$tmp" ;;
    fetch|auto)
        if [ $MODE = auto ] && check "$DEST" >/dev/null; then
            say "already in $DEST"; exit 0
        fi
        command -v curl >/dev/null 2>&1 || { say "no curl"; exit 1; }
        mktmp
        say "downloading linux-firmware $REF (amdgpu/, $NF files)"
        for f in $FILES; do
            curl -sSfL --retry 2 --max-time 120 "$URL/$REF/amdgpu/$f" -o "$tmp/$f" ||
                { say "download failed: $URL/$REF/amdgpu/$f"; exit 1; }
            if [ "$REF" = "$PIN" ]; then
                want=$(echo "$SUMS" | awk -v f="$f" '$1 == f { print $2 }')
                [ "$(sha256 "$tmp/$f")" = "$want" ] || { say "SHA-256 mismatch: $f"; exit 1; }
            fi
        done
        curl -sSfL --max-time 60 "$URL/$REF/LICENSES/LICENSE.amdgpu" -o "$tmp/LICENSE.amdgpu" 2>/dev/null ||
            curl -sSfL --max-time 60 "$URL/$REF/LICENSE.amdgpu" -o "$tmp/LICENSE.amdgpu" 2>/dev/null ||
            rm -f "$tmp/LICENSE.amdgpu"
        place "$tmp" ;;
esac
