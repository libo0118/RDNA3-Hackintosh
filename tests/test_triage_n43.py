"""tools/triage_reports.py's DRIVER-PLAN N43 rows on synthetic data:
 (1) n43_buftex_rows: the plugin's `buffer texture copy (pitch)` lines and the census counter give an INFO with the
     count (a lower bound: the plugin logs the first 8 and every 1024th), a plugin older than N43 (its log shows the
     vkCreateImage -> -1000158000 failure) gives a WARN, a clean plugin log gives nothing;
 (2) n43_vktest_rows: rdna3_vktest's LINPITCH line (64 B refused with -1000158000 and 256 B accepted: INFO; 256 B
     refused: FAIL; 64 B accepted or another code: WARN) and the memory load section (UCLK left level 0: INFO; stayed at
     the lowest level while umc activity was above 20 %: WARN; the load not registering: INFO; no pmctl sample, no
     result, NOT MEASURED: WARN; FAILED: FAIL; skipped by autoreport.sh: INFO with its reason; absent although
     rdna3pm=2 and the Vulkan test ran: WARN - never silence).
Run: python3 -I -B tests/test_triage_n43.py"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def lv(rows, name=None):
    return [r[0] for r in rows if name is None or r[1] == name]


def text(rows, name):
    return " | ".join(r[2] for r in rows if r[1] == name)


# ---- (1) the plugin's log ----
PITCH = ("buffer texture copy (pitch): bytesPerRow %d is not a multiple of 256 B (RADV linear custom pitch) - 40x16 VkFormat 37, "
         "a private copy instead of an alias (%d so far)\n")
log = "".join(PITCH % (p, k) for k, p in enumerate((320, 320, 576, 960), 1))
rows = tr.n43_buftex_rows(log, "")
check(lv(rows) == ["INFO"], "pitch lines: one INFO, got %r" % rows)
check("at least 4 buffer texture" in text(rows, "buffer texture copy (pitch)"), "count 4: %r" % rows)
check("320, 576, 960" in text(rows, "buffer texture copy (pitch)"), "pitches named: %r" % rows)
# the census line carries the running count too; the larger number wins
cen = "buffer texture (new): 5 new, 3 recycled, 0 parked, 0 evicted, 2049 copy (pitch)\n"
rows = tr.n43_buftex_rows("", log + cen)
check("at least 2049 " in text(rows, "buffer texture copy (pitch)"), "census count: %r" % rows)
# a census with 0 copies and no pitch line: nothing to say
rows = tr.n43_buftex_rows("buffer texture (new): 1 new, 0 recycled, 0 parked, 0 evicted, 0 copy (pitch)\n", "")
check(rows == [], "0 copies: silence is right, got %r" % rows)
check(tr.n43_buftex_rows("", "") == [], "no log: nothing")
# the plugin's real format strings (metal/plugin/nvmtl_vk.c, NVMTLObjects.m) are what the regexes read
import re  # noqa: E402
src = open(os.path.join(ROOT, "metal", "plugin", "nvmtl_vk.c")).read()
call = re.search(r'nvlog\("buffer texture copy \(pitch\)(.*?)\);\n', src, re.S)
check(call is not None, "nvmtl_vk.c: no nvlog(\"buffer texture copy (pitch)...\") call")
if call:
    head = call.group(0)[:call.group(0).index(", row_pitch")]
    line = re.sub(r"%(zu|u|lu)", "7", "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', head)))
    rows = tr.n43_buftex_rows(line, "")
    check(lv(rows) == ["INFO"] and "at least 7 " in rows[0][2], "the plugin's own log line is not read: %r -> %r" % (line, rows))
check("nvmtl_linear_pitch_ok(row_pitch, bpp)" in src and "% 32" not in src.split("alias_lifetime_body(uint32_t w")[-1].split("VkFormatProperties fp")[0],
      "the alias still checks the 32 B unit, or not the 256 B predicate")
body = src.split("nvmtl_vk_image_create_buffer_alias_lifetime_body(uint32_t w")[-1]
check(body.index("nvmtl_linear_pitch_ok") < body.index("nvmtl_opaque_CreateImage"), "the pitch is checked after vkCreateImage")
census = open(os.path.join(ROOT, "metal", "plugin", "NVMTLObjects.m")).read()
check("%lu copy (pitch)" in census, "the buffer texture census lost its `copy (pitch)` count")
# the plugin from before N43
old = "vk: vkCreateImage(buffer texture) -> -1000158000\n" * 3
rows = tr.n43_buftex_rows(old, "")
check(lv(rows) == ["WARN"] and "older than N43" in rows[0][2] and rows[0][2].startswith("3 "), "old plugin: %r" % rows)

# ---- (2) the tests file ----
LP_OK = "LINPITCH: rowPitch 64 -> -1000158000 (INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT), rowPitch 256 -> 0 (VK_SUCCESS)\n"
VK = "== rdna3_vktest\nPASS x\n" + LP_OK + "ALL PASSED: 13 submit(s) tested\n[exit 0]\n"
rows = tr.n43_vktest_rows(VK)
check(lv(rows, "linear pitch") == ["INFO"] and "is the RADV rule" in text(rows, "linear pitch"), "LINPITCH ok: %r" % rows)
rows = tr.n43_vktest_rows("== rdna3_vktest\n" + LP_OK.replace("rowPitch 256 -> 0 (VK_SUCCESS)", "rowPitch 256 -> -1000158000 (X)") +
                          "LINPITCH: FAILED a 256 B row pitch is refused\n")
check(lv(rows, "linear pitch") == ["FAIL"], "256 refused is a FAIL: %r" % rows)
rows = tr.n43_vktest_rows("== rdna3_vktest\nLINPITCH: rowPitch 64 -> 0 (VK_SUCCESS), rowPitch 256 -> 0 (VK_SUCCESS)\n")
check(lv(rows, "linear pitch") == ["WARN"] and "ACCEPTED" in text(rows, "linear pitch"), "64 accepted: %r" % rows)
rows = tr.n43_vktest_rows("== rdna3_vktest\nLINPITCH: rowPitch 64 -> -3 (INITIALIZATION_FAILED), rowPitch 256 -> 0 (VK_SUCCESS)\n")
check(lv(rows, "linear pitch") == ["WARN"] and "-3" in text(rows, "linear pitch"), "other code: %r" % rows)
rows = tr.n43_vktest_rows("== rdna3_vktest\nALL PASSED: 12 submit(s) tested\n")
check(lv(rows, "linear pitch") == ["WARN"], "no LINPITCH line after a Vulkan test: WARN, %r" % rows)
rows = tr.n43_vktest_rows("== rdna3_vktest\nLINPITCH: NOT CHECKED, VK_EXT_image_drm_format_modifier is not offered (x)\n")
check(lv(rows, "linear pitch") == ["INFO"], "not offered is INFO: %r" % rows)
check(tr.n43_vktest_rows("nothing\n") == [], "no Vulkan test, no rows")


def samp(now, umc, uclk, lv0=96):
    return ("pp_dpm_mclk (now %d MHz): 0: %dMhz %s1: 456Mhz %s2: 772Mhz 3: 1218Mhz\n"
            "  activity gfx 40 %% umc %d %% mm 0 %%, 80 W, gfxclk 2000 MHz, uclk %d MHz, socclk 1000 MHz\n" %
            (now, lv0, "* " if now == lv0 else "", "* " if now == 456 else "", umc, uclk))


def section(load, idle=samp(96, 1, 96), bw="MEMBW: 2 x 1024 MiB device-local (heap 16128 MiB), queue family 0, 10 s\n"
            "MEMBW: 1200 submit(s) of 8 fill + copy rounds, 5800 GiB of VRAM traffic in 10.0 s = 580.0 GiB/s\n"):
    s = "\n== rdna3 memory load (rdna3_vktest --membw, rdna3_pmctl info x3 during it)\n"
    for i, l in enumerate(load, 1):
        s += "-- pmctl under load %d\n%s" % (i, l)
    s += bw + "-- pmctl idle after the load\n" + idle + "ALL PASSED: memory load\n[exit 0]\n"
    return s


T = "x\n" + VK
good = T + section([samp(96, 3, 96), samp(456, 70, 450), samp(772, 90, 700)])
rows = tr.n43_vktest_rows(good, True)
check(lv(rows, "memory load") == ["INFO"] and "left level 0" in text(rows, "memory load"), "uclk left level 0: %r" % rows)
check("580.0 GiB/s" in text(rows, "memory load") and "umc activity max 90 %" in text(rows, "memory load") and
      "UCLK max 772 MHz" in text(rows, "memory load"), "numbers: %r" % rows)
check("after the load: uclk 96 MHz" in text(rows, "memory load"), "idle reading: %r" % rows)
stuck = T + section([samp(96, 60, 96), samp(96, 85, 96), samp(96, 70, 96)])
rows = tr.n43_vktest_rows(stuck, True)
check(lv(rows, "memory load") == ["WARN"] and "STAYED at its lowest level" in text(rows, "memory load"), "stuck at 96: %r" % rows)
light = T + section([samp(96, 1, 96), samp(96, 2, 96), samp(96, 0, 96)])
rows = tr.n43_vktest_rows(light, True)
check(lv(rows, "memory load") == ["INFO"] and "did not register" in text(rows, "memory load"), "load too light: %r" % rows)
# exactly 20 % is not above the line
edge = T + section([samp(96, 20, 96)] * 3)
check(lv(tr.n43_vktest_rows(edge, True), "memory load") == ["INFO"], "umc 20 % is not > 20 %")
# the activity line's uclk alone shows it too (pp_dpm_mclk says 96, the average says 450)
avg = T + section([samp(96, 50, 450)] * 3)
check(lv(tr.n43_vktest_rows(avg, True), "memory load") == ["INFO"] and "left level 0" in text(tr.n43_vktest_rows(avg, True), "memory load"),
      "average uclk above level 0")
nosamp = T + section(["no output\n"] * 3)
rows = tr.n43_vktest_rows(nosamp, True)
check(lv(rows, "memory load") == ["WARN"] and "no `rdna3_pmctl info` sample" in text(rows, "memory load"), "no samples: %r" % rows)
rows = tr.n43_vktest_rows(T + section([samp(96, 60, 96)] * 3, bw="MEMBW: NOT MEASURED, no 2 x 16 MiB of device-local memory: -2 (OUT_OF_DEVICE_MEMORY)\n"), True)
check(lv(rows, "memory load") == ["WARN"] and "NOT MEASURED" in text(rows, "memory load"), "not measured: %r" % rows)
rows = tr.n43_vktest_rows(T + section([samp(96, 60, 96)] * 3, bw="MEMBW: FAILED fence #3 not signalled in 20 s -> 2 (TIMEOUT)\n"), True)
check(lv(rows, "memory load") == ["FAIL"], "failed: %r" % rows)
rows = tr.n43_vktest_rows(T + section([samp(96, 60, 96)] * 3, bw=""), True)
check(lv(rows, "memory load") == ["WARN"] and "no `MEMBW:` result" in text(rows, "memory load"), "no result: %r" % rows)
rows = tr.n43_vktest_rows(T + "== memory load: skipped (rdna3pm=2 is not set: UCLK is not free, nothing to see)\n", False)
check(lv(rows, "memory load") == ["INFO"] and "rdna3pm=2 is not set" in text(rows, "memory load"), "skipped: %r" % rows)
rows = tr.n43_vktest_rows(T, True)
check(lv(rows, "memory load") == ["WARN"], "absent although rdna3pm=2: WARN, %r" % rows)
rows = tr.n43_vktest_rows(T, False)
check(lv(rows, "memory load") == [], "absent without rdna3pm=2: nothing to say")
# the section is read up to the next test header only
rows = tr.n43_vktest_rows(good + "== rdna3_pmctl baco-cycle\n" + samp(96, 90, 96), True)
check(lv(rows, "memory load") == ["INFO"], "text after the section is not read: %r" % rows)

if fails:
    print("test_triage_n43: FAIL")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("test_triage_n43: ok")
