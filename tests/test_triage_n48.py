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

# the failure lines of every shape the plugin prints (new "msupload: FAILED - ..." and the older "msupload: vkCreate... -> N")
for line in ("msupload: FAILED - vkCreateShaderModule (the device lacks Int64 / PhysicalStorageBuffer64?) (-3) [1 failed, 0 refused, 0 done so far]",
             "msupload: vkCreateShaderModule -> -3 (the device lacks Int64 / PhysicalStorageBuffer64?)",
             "msupload: FAILED - vkCreateFramebuffer (-4) [2 failed, 0 refused, 5 done so far]",
             "msupload: FAILED - no attachment view of format 44 over the format-50 image (0) [1 failed, 0 refused, 0 done so far]",
             "msupload: FAILED - more (format, samples) pairs than the pass table holds (16) [1 failed, 0 refused, 0 done so far]",
             "msupload: FAILED - framebuffer retention allocation (0) [1 failed, 0 refused, 0 done so far]"):
    rr = rows_of(L("Chess", 3005, "3b4f", line))
    check(list(rr) == ["multisample upload"] and rr["multisample upload"][0] == "WARN", "failure line not a WARN: %s -> %r" % (line, rr))
# the pass-built and upload lines are not failures
check(rows_of(L("Chess", 3005, "3b4f", "msupload: pass built for view format 44 at 4 samples (1 in this process)")) == {}, "pass built: nothing")
# REFUSED lines of the pass count as refused copies, with the running counts stripped from the reason
rr = rows_of(L("Chess", 3005, "3b4f", "msupload: REFUSED - a render pass is open [1 refused, 0 failed, 0 done so far]"))
check(list(rr) == ["plugin copy refused"], "msupload REFUSED: %r" % rr)

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

# framebuffer fetch, the implemented path (line formats of nvmtl_vk.c: nvmtl_fbfetch_pipeline_note, nvmtl_fbf_bind, nvmtl_fbf_prepare_pass)
FB2 = ("vk: fbfetch: fragment module #%d declares [[color(n)]] subpass inputs 0x7 (binding 192+n, 3 decoration(s) moved to set 3) and READS them; pipeline has 1 colour "
       "attachment(s), 1 sample(s); served 0x%x, zeroed 0x%x: %s [%d served / %d read / 0 declared-only / %d zeroed / %d zeroing failed so far]")
impl = L("Terminal", 904, "1e1e", FB2 % (1, 1, 6, "IMPLEMENTED for the attachments the pipeline has (input attachments of the render pass, barrier before each draw); the other reads zeroed", 1, 1, 1, 0))
impl += L("Terminal", 904, "1e1e", "vk: fbfetch: reads of attachments the pipeline lacks (the pipeline has no colour attachment at that index (normal: the translator reads every colour input it declares)): reads of colour attachment(s) 0x6 are zeroed, 0x1 are served [1 such pipeline(s) for this reason so far]")
impl += L("Terminal", 904, "1e1e", "vk: fbfetch: draw 4 reads [[color(n)]] 0x1: barrier + set 3 bound (pass inputs 0x1) [2 set(s), 0 skipped, 1 restart(s)]")
r = rows_of(impl)
check(list(r) == ["framebuffer fetch"] and r["framebuffer fetch"][0] == "INFO" and "IMPLEMENTED" in r["framebuffer fetch"][2]
      and "Terminal 4 draw(s), 2 pass set(s), 0 skipped, 1 pass restart(s)" in r["framebuffer fetch"][2], "implemented: INFO only, %r" % r)
uns = L("Chess", 3000, "1", "vk: fbfetch: UNSUPPORTED (a multisample target (per-sample fetch is not carried)): reads of colour attachment(s) 0x1 are zeroed, 0x0 are served [1 such pipeline(s) for this reason so far]")
rr = [x for x in t48.n48_plugin_rows(impl + uns) if x[0] == "WARN"]
check(len(rr) == 1 and "multisample target" in rr[0][2] and "Chess" in rr[0][2], "unsupported reason: WARN, %r" % rr)
for bad in ("vk: fbfetch: a draw that reads [[color(n)]] 0x1 is SKIPPED: an attachment it reads cannot be an input attachment (pass inputs 0x1, refused 0x1) [1 so far]",
            "vk: fbfetch: colour attachment 0 of this pass cannot be an input attachment: a layered render pass (format 44, 1 sample(s), 4 layer(s), textureType 3) - pipelines that read it are SKIPPED [1 so far]",
            "vk: fbfetch: input-attachment descriptor set allocation FAILED (-1000069000, 16 pool(s)) - the draws that read it are SKIPPED [1 so far]",
            "fbfetch: the render pass could not be RESTARTED with input attachments 0x1 - the rest of this encoder is not drawn"):
    rr = [x for x in t48.n48_plugin_rows(impl + L("Terminal", 904, "1e1e", bad)) if x[0] == "WARN"]
    check(len(rr) == 1 and "SKIPPED" in rr[0][2] and "Terminal" in rr[0][2], "refusal line is a WARN: %s -> %r" % (bad[:60], rr))
failed = L("Terminal", 904, "1e1e", FB2 % (2, 0, 1, "UNSUPPORTED and the zeroing FAILED - the read hits an undefined descriptor", 0, 1, 0, 1))
check(rows_of(failed)["framebuffer fetch"][0] == "WARN" and "reads left as translated" in rows_of(failed)["framebuffer fetch"][2], "zeroing failed: WARN")

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

# the hook in triage_reports.py: the real ImportError path (the module hidden), not a grep. A report is run through
# triage_reports.report() with triage_n48 unimportable: it must print the loud WARN row and not crash.
import io
import subprocess
import tarfile
import tempfile
src = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
check("from triage_n48 import n48_plugin_rows" in src and "n48_plugin_rows(pluginlog)" in src, "triage_reports.py calls n48_plugin_rows")
with tempfile.TemporaryDirectory() as d:
    tools = os.path.join(d, "tools")
    os.makedirs(tools)
    for name in os.listdir(os.path.join(ROOT, "tools")):
        if name.endswith(".py") and name != "triage_n48.py":      # everything but the module
            with open(os.path.join(ROOT, "tools", name), "rb") as f, open(os.path.join(tools, name), "wb") as o:
                o.write(f.read())
    rep = os.path.join(d, "reports"); os.makedirs(rep)
    def member(tar, name, text):
        data = text.encode(); ti = tarfile.TarInfo("rdna3-report/" + name); ti.size = len(data); tar.addfile(ti, io.BytesIO(data))
    with tarfile.open(os.path.join(rep, "rdna3-report-x.tar.gz"), "w:gz") as tar:
        member(tar, "01-boot-args.txt", "boot-args: rdna3lvl=10 rdna3accel=1\n")
        member(tar, "03-kexts.txt", "io.github.voten641.RDNA3Accel 1\n")
        member(tar, "11-ioreg-props.txt", '"Level" = 10\n')
        member(tar, "26-log-metal-plugin.txt", L("Chess", 1, "a", "copy_image_to_buffer: REFUSED - x"))
    for have, want_row, want_not in ((True, "plugin copy refused", "triage_n48.py is missing"), (False, "triage_n48.py is missing", "plugin copy refused")):
        if have:
            with open(os.path.join(ROOT, "tools", "triage_n48.py"), "rb") as f, open(os.path.join(tools, "triage_n48.py"), "wb") as o:
                o.write(f.read())
        out = subprocess.run([sys.executable, "-I", os.path.join(tools, "triage_reports.py"), rep], capture_output=True, text=True)
        check(want_row in out.stdout and want_not not in out.stdout and "Traceback" not in out.stderr,
              "module %s: wanted %r, got stdout %r stderr %r" % ("present" if have else "hidden", want_row, out.stdout[-300:], out.stderr[-300:]))
        if have:
            os.remove(os.path.join(tools, "triage_n48.py"))

if fails:
    print("test_triage_n48: FAIL")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("test_triage_n48: ok")
