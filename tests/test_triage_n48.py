"""tools/triage_n48.py on synthetic plugin logs (the line formats are the plugin's: metal/plugin/nvmtl_vk.c, NVMTLObjects.m):
refused copies (WARN, with the process and the reason), the multisample upload (INFO; its failures WARN), [[color(n)]]
framebuffer fetch (WARN while a read is left, INFO when folded away, WARN for a scan failure), an open copy bracket (WARN: the
process died at or after its last line), a clean log (nothing), and the hook in triage_reports.py.
Run: python3 -I -B tests/test_triage_n48.py"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_n48 as t48  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def L(proc, pid, tid, msg, ts="04:37:55.1"):
    return "2026-10-25 %s Df %s[%d:%s] (RDNA3MTLDriver) NVMTL: %s\n" % (ts, proc, pid, tid, msg)


def rows_of(log):
    return {r[1]: r for r in t48.n48_plugin_rows(log)}


check(t48.n48_plugin_rows("") == [], "empty log: nothing")
check(t48.n48_plugin_rows(L("Chess", 1, "a", "newTextureWithDescriptor: 971x680 -> 0x1")) == [], "unrelated line: nothing")

# refused copies
log = (L("Chess", 3005, "3b4f", "copy_image_to_buffer: REFUSED - the image is multisampled (Vulkan copies need 1 sample; a Metal blit cannot copy a multisample texture) (vk format 44, 4 sample(s))") +
       L("Chess", 3005, "3b4f", "copy_buffer_to_image: REFUSED - a 4-sample image: this colour format is not carried by the multisample upload pass (8-bit RGBA/BGRA/R/RG UNORM or sRGB only) (vk format 97, type 4)") +
       L("Chess", 3005, "3b4f", "copy_buffer_to_image: REFUSED - a 4-sample image: this colour format is not carried by the multisample upload pass (8-bit RGBA/BGRA/R/RG UNORM or sRGB only) (vk format 97, type 4)") +
       L("Chess", 3005, "3b4f", "blit copyFromTexture:toBuffer: REFUSED - source is a X, destination a Y (this plugin's texture and buffer are needed)"))
r = rows_of(log)
check(list(r) == ["plugin copy refused"] and r["plugin copy refused"][0] == "WARN", "refused: one WARN, %r" % r)
check("4 logged refusal line(s)" in r["plugin copy refused"][2] and "Chess copy_buffer_to_image" in r["plugin copy refused"][2]
      and "DROPPED" in r["plugin copy refused"][2], "refused text: %r" % r["plugin copy refused"][2])

# multisample uploads
ok = L("Chess", 3005, "3b4f", "msupload: buffer -> 4-sample image 971x680 (vk format 50 via view 44, mode 1): region (0,0) 971x680 from buffer offset 0 row 3904 B [3 so far]")
r = rows_of(ok + L("Chess", 3005, "3b4f", "msupload: pass built for view format 44 at 4 samples (1 in this process)"))
check(list(r) == ["multisample upload"] and r["multisample upload"][0] == "INFO" and "Chess 4-sample vk format 50: 3" in r["multisample upload"][2], "msupload INFO: %r" % r)
r = rows_of(ok + L("Chess", 3005, "3b4f", "msupload: vkCreateGraphicsPipelines(format 44, 4 samples) -> -3"))
check(r["multisample upload"][0] == "WARN" and "vkCreateGraphicsPipelines" in r["multisample upload"][2] or
      any(x[0] == "WARN" for x in t48.n48_plugin_rows(ok + L("Chess", 3005, "3b4f", "msupload: vkCreateGraphicsPipelines(format 44, 4 samples) -> -3"))), "msupload failure: WARN")
check([x[0] for x in t48.n48_plugin_rows(ok + L("Chess", 3005, "3b4f", "msupload: vkCreateFramebuffer -> -4"))].count("WARN") == 1, "framebuffer failure: WARN")

# framebuffer fetch
FB = ("vk: fbfetch: fragment module #%d declares [[color(n)]] subpass inputs 0x1 (binding 192+n) and %s; pipeline has 1 colour attachment(s), 1 sample(s); "
      "the set layout has no such binding and no render pass an input attachment: %s [%d read / %d declared-only / %d zeroed / %d zeroing failed so far]")
rd = L("WindowServer", 208, "88e", FB % (1, "READS them", "the read hits an undefined descriptor (NVMTL_FBFETCH_ZERO=1 makes it a defined zero)", 1, 0, 0, 0))
rd += L("Terminal", 797, "1a41", FB % (2, "READS them", "the read hits an undefined descriptor (NVMTL_FBFETCH_ZERO=1 makes it a defined zero)", 2, 0, 0, 0))
r = rows_of(rd)
check(r["framebuffer fetch"][0] == "WARN" and "WindowServer (up to module #1)" in r["framebuffer fetch"][2] and "Terminal" in r["framebuffer fetch"][2]
      and "reads left as translated" in r["framebuffer fetch"][2], "fbfetch read: %r" % r)
z = L("Terminal", 797, "1a41", FB % (1, "READS them", "each read replaced by a zero vector (NVMTL_FBFETCH_ZERO=1)", 1, 0, 1, 0))
check("1 read(s) replaced by zero" in rows_of(z)["framebuffer fetch"][2], "fbfetch zeroed")
gone = L("Terminal", 797, "1a41", FB % (1, "no read is left (folded away)", "harmless", 0, 1, 0, 0))
r = rows_of(gone)
check(r["framebuffer fetch"][0] == "INFO" and "folded away" in r["framebuffer fetch"][2], "fbfetch folded: %r" % r)
check(rows_of(gone + rd)["framebuffer fetch"][0] == "WARN", "a read anywhere wins over a folded one")
sf = L("Terminal", 797, "1a41", "vk: fbfetch: the scan of a fragment module (4096 words) FAILED - malformed module, an id bound over 1048576, or out of memory; a framebuffer fetch in it would go unnoticed [1 so far]")
check(rows_of(sf)["framebuffer fetch"][0] == "WARN", "scan failure: WARN")

# the copy bracket
B = "copy_image_to_buffer: recording vk format 130 971x680 (level 0 of 1, layer 0 of 1) 1 sample(s) type 2 aspect 2 element 4 B region (0,0) 971x680 -> row 3884 B offset 0 of 2639120, image layout 1"
closed = (L("Chess", 3265, "3f06", B) + L("Chess", 3265, "3f06", "copy_image_to_buffer: layout transition recorded") +
          L("Chess", 3265, "3f06", "copy_image_to_buffer: vkCmdCopyImageToBuffer recorded"))
check(t48.n48_plugin_rows(closed) == [], "a closed bracket: nothing")
r = rows_of(L("Chess", 3265, "3f06", B) + L("Chess", 3265, "3f06", "copy_image_to_buffer: layout transition recorded"))
check(r["plugin copy bracket"][0] == "WARN" and "Chess[3265:3f06] copy_image_to_buffer: layout transition recorded" in r["plugin copy bracket"][2], "open after the transition: %r" % r)
r = rows_of(L("Chess", 3265, "3f06", B))
check("recording" in r["plugin copy bracket"][2], "open at recording")
# another thread's closed bracket does not close this thread's; the special lines count
other = L("Chess", 3265, "3f07", B) + L("Chess", 3265, "3f07", "copy_image_to_buffer: layout transition recorded") + L("Chess", 3265, "3f07", "copy_image_to_buffer: vkCmdCopyImageToBuffer recorded")
check("3f06" in rows_of(L("Chess", 3265, "3f06", B) + other)["plugin copy bracket"][2], "per thread")
sp = L("Chess", 3265, "3f06", "copy_image_to_buffer: recording (depth/stencil, multisample or non-colour copy) vk format 130 971x680")
check("recording" in rows_of(sp)["plugin copy bracket"][2], "special recording line")
check(t48.n48_plugin_rows(sp + L("Chess", 3265, "3f06", "copy_image_to_buffer: (special copy) layout transition recorded") +
                          L("Chess", 3265, "3f06", "copy_image_to_buffer: (special copy) vkCmdCopyImageToBuffer recorded")) == [], "special closed")
b2i = L("Chess", 3265, "3f06", "copy_buffer_to_image: recording vk format 44 100x100")
check("copy_buffer_to_image: recording" in rows_of(b2i)["plugin copy bracket"][2], "buffer_to_image open")

# the hook in triage_reports.py
src = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
check("from triage_n48 import n48_plugin_rows" in src and "n48_plugin_rows(pluginlog)" in src and "triage_n48.py is missing" in src, "triage_reports.py calls n48_plugin_rows and says so when the module is missing")

if fails:
    print("test_triage_n48: FAIL")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("test_triage_n48: ok")
