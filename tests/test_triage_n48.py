"""tools/triage_n48.py on synthetic plugin logs (the line formats are the plugin's: metal/plugin/nvmtl_vk.c, NVMTLObjects.m):
refused copies (WARN, with the process and the reason), the multisample upload (INFO; its failures WARN), [[color(n)]]
framebuffer fetch (WARN while a read is left, INFO when folded away, WARN for a scan failure), an open copy bracket (WARN: the
process died at or after its last line), a clean log (nothing), and the hook in triage_reports.py.
Run: python3 -I -B tests/test_triage_n48.py"""
import os
import re
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
check(list(r) == ["multisample upload"] and r["multisample upload"][0] == "INFO" and "Chess 4-sample vk format 50: 1 line(s)" in r["multisample upload"][2], "msupload INFO: %r" % r)
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

# multisample texture -> buffer (N52; line formats of nvmtl_vk.c: nvmtl_vk_cmd_copy_msimage_to_buffer, nvmtl_msd_fail / nvmtl_msd_refuse; the SUMMARY lines of
# nvmtl_msstat.h, whose text the C host test pins)
dl = L("Chess", 3005, "3b4f", "msdl: STENCIL 4-sample image 971x680 -> buffer (vk format 130, aspect 4 via view 130, mode 0): region (0,0) 971x680 to buffer offset 0 row 3884 B, 660280 word(s) in 10317x1 group(s) [stencil #3]")
rr = rows_of(dl + L("Chess", 3005, "3b4f", "msdl: pass built for the unsigned-integer (stencil, R8_UINT ...) module") + L("Chess", 3005, "3b4f", "msdl: descriptor pool 1 opened (256 sets each)"))
check("multisample download" in rr and rr["multisample download"][0] == "INFO" and "Chess 4-sample vk format 130 aspect 4: 1 line(s)" in rr["multisample download"][2], "msdl INFO (no SUMMARY): %r" % rr)
for line in ("msdl: FAILED (pipeline) - vkCreateComputePipelines (VkResult -3) [1 of this kind]",
             "msdl: FAILED (descriptor set) - descriptor set allocation (8 of 8 pools open with 256 sets each, 64 set(s) already in this command buffer) (VkResult -1000069000) [2 of this kind]",
             "msdl: FAILED (image view) - no sampled view of format 130 aspect 4 over the format-130 image (the image_view_create line before names the cause) [1 of this kind]"):
    rr = [x for x in t48.n48_plugin_rows(dl + L("Chess", 3005, "3b4f", line)) if x[0] == "WARN" and x[1] == "multisample download"]
    check(len(rr) == 1, "msdl failure is a WARN: %s -> %r" % (line, rr))
for line in ("msdl: REFUSED (a render pass is open) - the copy was recorded inside an open render pass [1 of this kind]",
             "msdl: REFUSED (no buffer device address) - the buffer has no device address (bufferDeviceAddress is not enabled) [1 of this kind]",
             "copy_image_to_buffer: REFUSED - a 4-sample image: the 24-bit depth of this format has no byte layout in a Metal blit (vk format 129, type 4, aspect option 2, region (0,0) 8x8, row 32 B, buffer offset 0 of 256) [this reason 1 time(s), 1 plan refusals in all]"):
    rr = rows_of(L("Chess", 3005, "3b4f", line))
    check(list(rr) == ["plugin copy refused"], "msdl refusal: %s -> %r" % (line[:50], rr))
# the reason keeps its own parentheses and loses the detail group and the counters (ECC round 1, F6)
rr = rows_of(L("Chess", 3005, "3b4f", "msupload: REFUSED (pass unavailable) - the pass for this (view format, samples) pair failed to build earlier [3 of this kind]") +
             L("Chess", 3005, "3b4f", "copy_buffer_to_image: REFUSED - a 4-sample image: this colour format is not carried by the multisample upload pass (8-bit RGBA/BGRA/R/RG UNORM or sRGB, R8/RG8/RGBA8/R16/R32 UINT only) (vk format 97, type 4, region (0,0) 8x8, row 64 B, buffer offset 0 of 4096) [this reason 2 time(s), 5 plan refusals in all]"))
txt = rr["plugin copy refused"][2]
check("(view format, samples) pair failed to build earlier (1)" in txt and "UINT only) (1)" in txt and "[" not in txt.split("blit copies):")[1], "reason keys: %r" % txt)
# more than 4 reasons are shown
many = "".join(L("Chess", 3005, "3b4f", "copy_image_to_buffer: REFUSED - reason number %d (vk format 1) [x]" % i) for i in range(7))
check(rows_of(many)["plugin copy refused"][2].count("reason number") == 7, "up to 8 reasons")

def SUMD(sten=0, colour=0, depth=0, r8u=0, **kw):
    v = dict(done=sten + colour + depth, d_colour=colour, d_depth=depth, d_stencil=sten, refused=0, r_plan=0, r_inrp=0, r_nodev=0, r_noaddr=0, r_nousage=0, r_pipe=0,
             failed=0, f_setlayout=0, f_pipelayout=0, f_module=0, f_pipeline=0, f_view=0, f_retire=0, f_set=0, resolve_r8u=r8u, resolve_other=3)
    v.update(kw)
    return "msdl: SUMMARY " + " ".join("%s=%d" % kv for kv in v.items())

def SUMU(f13=0, flat13=0, wide=0, **kw):
    v = dict(done=f13 + flat13, done_int=f13 + flat13, inter=f13, flat=flat13, wide=wide, refused=0, r_plan=0, r_inrp=0, r_nodev=0, r_noaddr=0, r_pipe=0, failed=0,
             f_pipelayout=0, f_renderpass=0, f_module=0, f_pipeline=0, f_table=0, f_view=0, f_framebuffer=0, f_retire=0)
    v.update(kw)
    out = "msupload: SUMMARY " + " ".join("%s=%d" % kv for kv in v.items())
    if f13: out += " up_inter_13=%d" % f13
    if flat13: out += " up_flat_13=%d" % flat13
    return out

CEN = "G14 census: copy texture->buffer 971x680 iosurface=0, vkfmt 130 samples 4 type 4 layers 1 mips 1 aspect 4 bpp 1: multisample, format/aspect carried by the plugin's download pass (view format 130, mode 0; the region is decided at the copy), metal pixel format 260, region 971x680 slice 0 level 0 -> buffer offset 0 row 3884 (first time in this process)"
ok_chain = L("Chess", 3005, "3b4f", CEN) + L("Chess", 3005, "3b4f", SUMD(sten=14, r8u=14)) + L("Chess", 3005, "3b4f", SUMU(f13=14))
r = rows_of(ok_chain)
check(r["multisample chain"][0] == "INFO" and "stencil downloads 14, R8_UINT uploads 14, resolves into R8_UINT 14" in r["multisample chain"][2], "chain ok: %r" % (r.get("multisample chain"),))
check(r["multisample upload"][0] == "INFO" and "14 upload(s)" in r["multisample upload"][2] and "inter_13: 14" in r["multisample upload"][2], "upload INFO from the SUMMARY: %r" % (r["multisample upload"],))
check(all(x[0] == "INFO" for x in t48.n48_plugin_rows(ok_chain)), "a complete chain has no WARN: %r" % t48.n48_plugin_rows(ok_chain))
def chain(log): return [x for x in t48.n48_plugin_rows(log) if x[1] == "multisample chain"]
# the census shows the multisample stencil copy, the plugin predates N52 (no SUMMARY): WARN
w = chain(L("Chess", 3005, "3b4f", "G14 census: copy texture->buffer 971x680 iosurface=0, vkfmt 130 samples 4 type 4 layers 1 mips 1: REFUSED (the image is multisampled (Vulkan copies need 1 sample; a Metal blit cannot copy a multisample texture)), metal pixel format 260, region 971x680 slice 0 level 0 -> buffer offset 0 row 3884 (first time in this process)"))
check(len(w) == 1 and w[0][0] == "WARN" and "no msdl / msupload SUMMARY" in w[0][2], "census without SUMMARY: %r" % w)
# a refusal of the stencil download / the R8_UINT upload (old plugin lines) shows the chain too
w = chain(L("Chess", 3005, "3b4f", "copy_image_to_buffer: REFUSED - the image is multisampled (Vulkan copies need 1 sample; a Metal blit cannot copy a multisample texture) (vk format 130, 4 sample(s), type 4, aspect option 4, buffer offset 0, region 971x680)"))
check(len(w) == 1 and w[0][0] == "WARN", "refused stencil download: %r" % w)
w = chain(L("Chess", 3005, "3b4f", "copy_buffer_to_image: REFUSED - a 4-sample image: this colour format is not carried by the multisample upload pass (8-bit RGBA/BGRA/R/RG UNORM or sRGB only) (vk format 13, type 4, region (0,0) 971x680, row 3884 B, buffer offset 0 of 4194304) [1 refused, 0 failed so far]"))
check(len(w) == 1 and w[0][0] == "WARN", "refused R8_UINT upload: %r" % w)
# a colour-only process and a D24 depth refusal are not the chain
check(chain(L("Chess", 3005, "3b4f", SUMD(colour=5)) + L("Chess", 3005, "3b4f", SUMU())) == [] or all(x[0] == "INFO" for x in chain(L("Chess", 3005, "3b4f", SUMD(colour=5)))), "colour only: no chain row")
check(chain(L("Chess", 3005, "3b4f", "copy_image_to_buffer: REFUSED - a 4-sample image: the 24-bit depth of this format has no byte layout in a Metal blit (vk format 129, type 4, aspect option 2, region (0,0) 8x8, row 32 B, buffer offset 0 of 256) [this reason 1 time(s), 1 plan refusals in all]")) == [], "D24 depth refusal is not the chain")
# each missing link, and a large difference
for log, what in ((SUMD(sten=0, r8u=14) + "\n" + SUMU(f13=14), "stencil downloads 0"), (SUMD(sten=14, r8u=14) + "\n" + SUMU(f13=0, colour=1) if False else SUMD(sten=14, r8u=14) + "\n" + SUMU(), "R8_UINT uploads 0"),
                  (SUMD(sten=14, r8u=0) + "\n" + SUMU(f13=14), "no resolve into a 1-sample R8_UINT"), (SUMD(sten=14, r8u=14) + "\n" + SUMU(f13=7), "differ by more than 25%"),
                  (SUMD(sten=14, r8u=3) + "\n" + SUMU(f13=14), "differ by more than 25%")):
    lg = "".join(L("Chess", 3005, "3b4f", ln) for ln in log.split("\n"))
    w = chain(L("Chess", 3005, "3b4f", CEN) + lg)
    check(len(w) == 1 and w[0][0] == "WARN" and what in w[0][2], "chain link %r: %r" % (what, w))
check(chain(L("Chess", 3005, "3b4f", CEN) + L("Chess", 3005, "3b4f", SUMD(sten=14, r8u=13)) + L("Chess", 3005, "3b4f", SUMU(f13=15)))[0][0] == "INFO", "small differences are not a WARN")
# flat layout, wide rows, refusals and failures in the totals are WARN rows of their own
w = [x for x in t48.n48_plugin_rows(ok_chain + L("Chess", 3005, "3b4f", SUMU(f13=14, flat13=2, wide=1)) +
                                      L("Chess", 3005, "3b4f", "msupload: FLAT layout fallback (the row is not a multiple of samples * bytes) - buffer -> 4-sample image 100x80 (vk format 44 via view 44, mode 1): region (0,0) 10x10 from buffer offset 0 row 3884 B, every sample of a pixel gets the one element, float output")) if x[0] == "WARN"]
check(any("FLAT layout fallback" in x[2] and "2 upload(s)" in x[2] and "not a multiple of samples * bytes" in x[2] for x in w) and any("WIDE interleaved row" in x[2] for x in w), "flat / wide WARN rows: %r" % w)
w = [x for x in t48.n48_plugin_rows(ok_chain + L("Chess", 3005, "3b4f", SUMD(sten=14, r8u=14, r_noaddr=2, f_set=1, failed=1)) + L("Chess", 3005, "3b4f", SUMU(f13=14, r_pipe=4)))
     if x[0] == "WARN"]
check(any("r_noaddr=2, f_set=1" in x[2] and x[1] == "multisample download" for x in w) and any("r_pipe=4" in x[2] and x[1] == "multisample upload" for x in w), "totals problems: %r" % w)
# the newest SUMMARY of a process wins, a second process is judged on its own
two = (L("Chess", 3005, "3b4f", CEN) + L("Chess", 3005, "3b4f", SUMD(sten=2, r8u=2)) + L("Chess", 3005, "3b4f", SUMU(f13=2)) +
       L("Chess", 3005, "3b4f", SUMD(sten=14, r8u=14)) + L("Chess", 3005, "3b4f", SUMU(f13=14)) +
       L("Chess", 4000, "1", CEN) + L("Chess", 4000, "1", SUMD(sten=5, r8u=5)))
w = chain(two)
check(len(w) == 2 and w[0][0] == "WARN" and "Chess[4000]" in w[0][2] and "R8_UINT uploads 0" in w[0][2] and "Chess[3005]" not in w[0][2] and w[1][0] == "INFO" and "Chess[3005]" in w[1][2], "per-process chain: %r" % w)

# framebuffer fetch
FB = ("vk: fbfetch: fragment module #%d declares [[color(n)]] subpass inputs 0x1 (binding 192+n) and %s; pipeline has 1 colour attachment(s), 1 sample(s); "
      "the set layout has no such binding and no render pass an input attachment: %s [%d read / %d declared-only / %d zeroed / %d zeroing failed so far]")
rd = L("WindowServer", 208, "88e", FB % (1, "READS them", "the read hits an undefined descriptor (NVMTL_FBFETCH_ZERO=1 makes it a defined zero)", 1, 0, 0, 0))
rd += L("Terminal", 797, "1a41", FB % (2, "READS them", "the read hits an undefined descriptor (NVMTL_FBFETCH_ZERO=1 makes it a defined zero)", 2, 0, 0, 0))
r = rows_of(rd)
check(r["framebuffer fetch"][0] == "WARN" and "WindowServer (up to module #1)" in r["framebuffer fetch"][2] and "Terminal" in r["framebuffer fetch"][2]
      and "reads left as translated" in r["framebuffer fetch"][2], "fbfetch read: %r" % r)
z = L("Terminal", 797, "1a41", FB % (1, "READS them", "each read replaced by a zero vector (NVMTL_FBFETCH_ZERO=1)", 1, 0, 1, 0))
check("1 module(s) with zeroed reads" in rows_of(z)["framebuffer fetch"][2], "fbfetch zeroed")
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
impl += L("Terminal", 904, "1e1e", "vk: fbfetch: draw 4 reads [[color(n)]] 0x1: barrier + set 3 bound (pass inputs 0x1)")
SUM = ("vk: fbfetch: SUMMARY served=%d modules=5 read=5 zmod=5 zero_failed=%d scan_failed=0 move_failed=0 zr_noatt=5 zr_ms=%d zr_int=0 zr_tess=0 zr_nolayout=0 zr_forced=0 "
       "sets=%d draws=%d skipped=%d skip_nopass=0 skip_noinput=0 skip_att=%d skip_noset=0 restarts=%d att_refused=%d alloc_failed=0")
impl += L("Terminal", 904, "1e1e", SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0))
r = rows_of(impl)
check(list(r) == ["framebuffer fetch"] and r["framebuffer fetch"][0] == "INFO" and "IMPLEMENTED" in r["framebuffer fetch"][2]
      and "Terminal 5 pipeline(s) served, 9000 draw(s), 40 pass set(s), 3 restart(s)" in r["framebuffer fetch"][2], "implemented: INFO only, %r" % r)
# N64: the barrier kinds of a newer plugin are in the row; a reading draw without an image to name is a WARN from the totals
n64 = L("Terminal", 904, "1e1e", SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0) + " bar_img=8990 bar_mem=0 bar_noimg=0")
r = rows_of(impl.replace(SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0), "x") + n64)
check(r["framebuffer fetch"][0] == "INFO" and "3 restart(s), barriers: image 8990, old memory 0 (NVMTL_FBF_MEMBAR), 0 without an image" in r["framebuffer fetch"][2],
      "N64 barrier kinds: %r" % r)
rr = [x for x in t48.n48_plugin_rows(L("Terminal", 904, "1e1e", SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0) + " bar_img=1 bar_mem=0 bar_noimg=7")) if x[0] == "WARN"]
check(len(rr) == 1 and "7 reading draw(s) that got the OLD global barrier" in rr[0][2], "N64 bar_noimg: WARN, %r" % rr)
uns = L("Chess", 3000, "1", "vk: fbfetch: UNSUPPORTED (a multisample target (per-sample fetch is not carried)): reads of colour attachment(s) 0x1 are zeroed, 0x0 are served [1 such pipeline(s) for this reason so far]")
rr = [x for x in t48.n48_plugin_rows(impl + uns) if x[0] == "WARN"]
check(len(rr) == 1 and "multisample target" in rr[0][2] and "Chess" in rr[0][2], "unsupported reason: WARN, %r" % rr)
for bad in ("vk: fbfetch: a draw that reads [[color(n)]] 0x1 is SKIPPED: an attachment it reads (refused 0x1 of the pass's 0x1) cannot be an input attachment - see the 'cannot be an input attachment' lines",
            "vk: fbfetch: colour attachment 0 of this pass cannot be an input attachment: a layered render pass (format 44, 1 sample(s), 4 layer(s), textureType 3) - pipelines that read it are SKIPPED [1 so far]",
            "vk: fbfetch: input-attachment descriptor set allocation FAILED (-1000069000, 16 pool(s)) - the draws that read it are SKIPPED [1 so far]",
            "fbfetch: the render pass could not be RESTARTED with input attachments 0x1 - the rest of this encoder is not drawn",
            "vk: fbfetch: ZEROING FAILED for a fragment module (812 words, zero mask 0x6): the reads of colour attachment(s) 0x6 hit an UNDEFINED descriptor (corrupt colours are to be expected from this pipeline)",
            "vk: fbfetch: BUG: a draw that reads [[color(n)]] 0x1 was reached in a render pass with input attachments 0x2 only - the encoder's restart did not happen, SKIPPED",
            "vk: fbfetch: moving the subpass inputs to descriptor set 3 FAILED - every read of this module is zeroed instead (served mask 0x1 NOT served)",
            "vk: fbfetch: this device allows only 4 input attachments per stage / 4 per set (need 8) - [[color(n)]] reads will be zero vectors"):
    rr = [x for x in t48.n48_plugin_rows(impl + L("Terminal", 904, "1e1e", bad)) if x[0] == "WARN"]
    check(len(rr) == 1 and ("SKIPPED" in rr[0][2] or "FAILED" in rr[0][2] or "BUG" in rr[0][2]) and "Terminal" in rr[0][2], "refusal line is a WARN: %s -> %r" % (bad[:60], rr))
failed = L("Terminal", 904, "1e1e", FB2 % (2, 0, 1, "UNSUPPORTED and the zeroing FAILED - the read hits an undefined descriptor", 0, 1, 0, 1))
check(rows_of(failed)["framebuffer fetch"][0] == "WARN" and "reads left as translated" in rows_of(failed)["framebuffer fetch"][2], "zeroing failed: WARN")

# the plugin log keeps the first 8 lines of each format literal: eight benign lines, then a real limit, then the totals
benign = "".join(L("Terminal", 904, "1e1e", "vk: fbfetch: reads of attachments the pipeline lacks (normal, the translator reads every colour input it declares): reads of colour attachment(s) 0x6 are zeroed, 0x1 are served [%d such pipeline(s) so far]" % k) for k in range(1, 9))
ms = L("Chess", 3000, "1", "vk: fbfetch: UNSUPPORTED (a multisample target, per-sample fetch is not carried): reads of colour attachment(s) 0x1 are zeroed, 0x0 are served [1 such pipeline(s) so far]")
rr = [x for x in t48.n48_plugin_rows(benign + ms) if x[0] == "WARN"]
check(len(rr) == 1 and "multisample target" in rr[0][2], "a real limit after a flood of benign lines: WARN, %r" % rr)
check(not [x for x in t48.n48_plugin_rows(benign) if x[0] == "WARN"], "only the benign reason: no WARN")
# the totals alone raise the WARN (the individual lines may have been capped away)
for key, val in (("skipped", 3), ("zero_failed", 1), ("att_refused", 2), ("alloc_failed", 1), ("zr_int", 4), ("restarts", 101)):
    line = SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0)
    line = line.replace(" %s=" % key, " %s=" % key + str(val) + "#", 1)
    line = re.sub(r"(%s=)\d+#\d+" % key, r"\g<1>%d" % val, line) if key != "restarts" else line.replace("restarts=3", "restarts=101")
    w = [x for x in t48.n48_plugin_rows(L("Terminal", 904, "1e1e", line)) if x[0] == "WARN"]
    check(len(w) == 1 and "totals:" in w[0][2], "summary alone: %s -> %r (%s)" % (key, w, line[-90:]))
check(not [x for x in t48.n48_plugin_rows(L("Terminal", 904, "1e1e", SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0))) if x[0] == "WARN"], "a clean summary: no WARN")
# the newest summary of a process wins
old_new = L("Terminal", 904, "1e1e", SUM % (5, 0, 0, 1, 10, 7, 7, 0, 0)) + L("Terminal", 904, "1e1e", SUM % (5, 0, 0, 40, 9000, 0, 0, 3, 0))
check(not [x for x in t48.n48_plugin_rows(old_new) if x[0] == "WARN"], "newest summary wins")
# every UNSUPPORTED literal of the plugin (the NVMTL_FBF_LOG_REASON macro) is read by the triage
import re as _re
hdr = open(os.path.join(ROOT, "metal", "plugin", "nvmtl_fbfetch_plan.h")).read()
lits = _re.findall(r'LOG\("(vk: fbfetch: UNSUPPORTED \([^"]*)"', hdr)
check(len(lits) >= 6, "found the UNSUPPORTED literals in the plan header: %d" % len(lits))
for lit in lits:
    text = _re.sub(r"%[ux]", "1", lit)
    ww = [x for x in t48.n48_plugin_rows(L("Terminal", 904, "1e1e", text)) if x[0] == "WARN"]
    check(len(ww) == 1, "triage reads the plugin literal: %s" % lit[:70])

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
