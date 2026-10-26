#!/usr/bin/env python3
"""The two binary files the app survey embeds in tools/vm/macos/autoreport.sh (apps_assets: survey.pdf for Preview, movie.mp4 for QuickTime Player),
as base64 blocks ready to paste between the `apps_b64 ... <<'APPSEND'` lines.

    python3 tools/apps_assets.py pdf    # a two-page PDF: text in two fonts, an axial and a radial shading, colour boxes (pure Python, no libraries)
    python3 tools/apps_assets.py mp4    # a 12 s H.264 baseline movie, 400x224, three boxes moving (needs ffmpeg with libx264)

Regenerate only to change what the survey shows; tests/test_apps_step.sh checks that the embedded files are a PDF and an MP4.
"""
import base64
import subprocess
import sys
import tempfile
import zlib


def pdf():
    objs = []

    def add(s):
        objs.append(s)
        return len(objs)

    cat, pgs = add(None), add(None)
    font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    fontb = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Italic >>")
    sh = add(b"<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [60 0 552 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> /Extend [true true] >>")
    sh2 = add(b"<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [306 300 0 306 300 150] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 1 0] /C1 [0 0.4 0] /N 1 >> /Extend [false true] >>")
    kids = []
    for n in (1, 2):
        s = b"BT /F1 24 Tf 60 730 Td (RDNA3 app survey - Preview PDF, page %d) Tj ET\n" % n
        s += b"q 60 640 492 60 re W n /Sh1 sh Q\n"
        s += b"q 100 150 412 150 re W n /Sh2 sh Q\n" if n == 1 else b"0.9 0.2 0.2 rg 60 200 100 80 re f 0.2 0.5 0.9 rg 200 200 100 80 re f 0.2 0.8 0.3 rg 340 200 100 80 re f\n"
        y = 600
        for t, size in ((b"The quick brown fox jumps over the lazy dog 0123456789", 14), (b"ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz", 12),
                        (b"Small text 8pt: sphinx of black quartz, judge my vow", 8)):
            s += b"BT /F1 %d Tf 60 %d Td (%s) Tj ET\n" % (size, y, t)
            y -= 24
        s += b"BT /F2 18 Tf 60 %d Td (Italic serif line for glyph checks) Tj ET\n" % y
        for i in range(8):
            s += b"BT /F1 10 Tf 60 %d Td (Line %d: lorem ipsum dolor sit amet, consectetur adipiscing elit) Tj ET\n" % (y - 40 - i * 16, i + 1)
        z = zlib.compress(s, 9)
        c = add(b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(z) + z + b"\nendstream")
        kids.append(add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> /Shading << /Sh1 %d 0 R /Sh2 %d 0 R >> >> >>"
                        % (pgs, c, font, fontb, sh, sh2)))
    objs[cat - 1] = b"<< /Type /Catalog /Pages %d 0 R >>" % pgs
    objs[pgs - 1] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (b" ".join(b"%d 0 R" % k for k in kids), len(kids))
    out, offs = bytearray(b"%PDF-1.4\n"), []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
    x = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
    return bytes(out)


def mp4():
    src = ("color=c=0x204080:size=400x224:rate=10:duration=12,drawbox=x='mod(t*80,350)':y=90:w=50:h=50:color=yellow:t=fill,"
           "drawbox=x=25:y='25+mod(t*30,120)':w=70:h=35:color=red:t=fill,drawbox=x='370-mod(t*50,330)':y=180:w=30:h=30:color=white:t=fill")
    with tempfile.NamedTemporaryFile(suffix=".mp4") as f:
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", src, "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0", "-pix_fmt", "yuv420p",
                        "-crf", "34", "-preset", "veryslow", "-g", "20", "-an", "-movflags", "+faststart", f.name], check=True)
        return open(f.name, "rb").read()


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what not in ("pdf", "mp4"):
        sys.exit(__doc__)
    b = base64.b64encode(pdf() if what == "pdf" else mp4()).decode()
    print("\n".join(b[i:i + 76] for i in range(0, len(b), 76)))
