#!/bin/sh
# tests/test_scanout.sh BUILD_DIR - the loopback's scanout end to end (was a run of recipe lines in the Makefile's
# `test`): rdna3_drminfo_host writes the test picture, the triage turns it into a PNG. The pictures go to a private
# directory, so this runs beside the other tests; its steps depend on each other and run in this order.
set -eu
B=${1:-build/host}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
set -x
"$B/rdna3_drminfo_host" --scanout "$W" | grep -q '1 pipe(s) lit'
"$B/rdna3_drminfo_host" --scanout "$W" | grep -q '^  TX problems: none$'
"$B/rdna3_drminfo_host" --scanout-hires "$W" | grep -q '1 pipe(s) lit'
# the hires look is the surface's native 1920x1080 (every pixel, rows cut in segments of rgb[]: the picture is
# put together right across the cuts at 1280) and the host triage turns it into a PNG of that size
test "$(sed -n 2p "$W/scanout-pipe0.ppm")" = '1920 1080'
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); d=d[d.index(b'255\n')+4:]; px=lambda x,y: tuple(d[(y*1920+x)*3:(y*1920+x)*3+3]); box=lambda x,y: 140<=x*320//1920<180 and 70<=y*180//1080<110; sys.exit(0 if len(d)==1920*1080*3 and all(px(x,y)==((255,255,255) if box(x,y) else (0,y*255//1080,x*255//1920)) for x in (0,1,1279,1280,1281,1919,900,1000) for y in (0,1,539,540,1079)) else 1)" "$W/scanout-pipe0.ppm"
"$B/rdna3_drminfo_host" --scanout-hires "$W" | grep -q 'picture 1920x1080: .* (native)$'
sh tests/test_frame_order.sh "$B"
python3 tools/triage_reports.py --picture "$W/scanout-pipe0.ppm" | grep -q 'png'
python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(); sys.exit(0 if struct.unpack('>II', d[16:24]) == (1920, 1080) else 1)" "$W/scanout-pipe0.png"
# a kext from before the native look (RDNA3_LB_SCANOUT_V1): its 1280-wide sample, and the output says so
RDNA3_LB_SCANOUT_V1=1 "$B/rdna3_drminfo_host" --scanout-hires "$W" | grep -q 'native look: NOT available, the kext predates it (abi 0): this is its 1280-wide sample'
test "$(sed -n 2p "$W/scanout-pipe0.ppm")" = '1280 720'
