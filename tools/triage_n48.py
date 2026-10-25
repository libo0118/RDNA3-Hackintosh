"""Triage rows of DRIVER-PLAN N48 (Metal plugin: blit copies, buffer -> multisample upload, [[color(n)]] framebuffer fetch).
Read from 26-log-metal-plugin.txt (every process that loaded the plugin; lines `... Name[pid:tid] (RDNA3MTLDriver) NVMTL: msg`).
Kept in a module of its own so that tools/triage_reports.py only gains one call. The lines are the plugin's
(metal/plugin/nvmtl_vk.c, NVMTLObjects.m); docs/AFTER-TESTS.md "Metal plugin: blit copies ..." explains them."""
import collections
import re

_LINE = re.compile(r"\b([^\s\[\]]+)\[(\d+):([0-9a-f]+)\] \(RDNA3MTLDriver\) NVMTL: (.*)$")
_REFUSED = re.compile(r"^(copy_image_to_buffer|copy_buffer_to_image|blit copyFromTexture:toBuffer|blit copyFromBuffer:toTexture|msupload): REFUSED - (.*)$")
_BRACKET = re.compile(r"^copy_image_to_buffer: (?:\(special copy\) )?(recording|layout transition recorded|vkCmdCopyImageToBuffer recorded)")
_BRACKET_B2I = re.compile(r"^copy_buffer_to_image: (?:\(special copy\) )?(recording|vkCmdCopyBufferToImage recorded)")
_MSU = re.compile(r"^msupload: buffer -> (\d+)-sample image (\d+)x(\d+) \(vk format (\d+) via view (\d+), mode (\d+)\).*?\[(\d+) so far\]")
_FBF = re.compile(r"^vk: fbfetch: fragment module #(\d+) declares .*? and (READS them|no read is left \(folded away\));.*?: (.*?) "
                  r"\[(?:(\d+) served / )?(\d+) read / (\d+) declared-only / (\d+) zeroed / (\d+) zeroing failed so far\]")
# the implemented path (N48 follow-up): per-pass sets, draws, restarts, and every way it refuses
_FBF_DRAW = re.compile(r"^vk: fbfetch: draw (\d+) reads .*?\[(\d+) set\(s\), (\d+) skipped, (\d+) restart\(s\)\]")
_FBF_REASON = re.compile(r"^vk: fbfetch: (UNSUPPORTED|reads of attachments the pipeline lacks) \((.*?)\): reads of colour attachment\(s\) 0x([0-9a-f]+) are zeroed")
_FBF_BAD = re.compile(r"^(?:vk: fbfetch: (a draw that reads .*? is SKIPPED: .*?|colour attachment \d+ of this pass cannot be an input attachment: .*?|"
                      r"input-attachment descriptor set allocation FAILED.*?|descriptor layouts FAILED.*?|moving the subpass inputs.*? FAILED.*?)|"
                      r"fbfetch: the render pass could not be RESTARTED.*?)(?: \(|\s*\[|$)")


def _lines(pluginlog):
    for raw in (pluginlog or "").splitlines():
        m = _LINE.search(raw)
        if m:
            yield m.group(1), int(m.group(2)), m.group(3), m.group(4)


def n48_plugin_rows(pluginlog=""):
    """[(level, name, text)]. WARN for a copy the plugin dropped, a multisample upload that failed, a framebuffer fetch read that
    is still in a pipeline, a scan that failed and a copy bracket that stays open (the process died at or after its last
    line); INFO for the multisample uploads that ran and for fetches the specialiser removed."""
    refused = collections.Counter()         # (process, call, reason) -> lines
    msu = {}                                 # (process, samples, fmt) -> highest running count
    msu_failed = []
    fbf_read, fbf_gone = {}, {}              # process -> highest module number
    fbf_impl, fbf_zmod = {}, {}              # process -> highest module number: implemented / zeroed (the module line)
    fbf_unsup = collections.Counter()        # reason -> process -> lines (real limits; "pipeline lacks the attachment" is normal)
    fbf_bad = collections.Counter()          # process + message -> lines (draw skipped, attachment refused, allocation failed, restart failed)
    fbf_draw = {}                            # process -> (draws, sets, skipped, restarts) at the highest draw line
    fbf_zeroed = 0
    scan_failed = 0
    brackets = {}                            # (pid, tid) -> (process, call, last state)
    for proc, pid, tid, msg in _lines(pluginlog):
        m = _REFUSED.match(msg)
        if m:
            reason = re.sub(r"\s*\(.*$", "", m.group(2))[:110]
            refused[(proc, m.group(1), reason)] += 1
            continue
        m = _BRACKET.match(msg)
        if m:
            brackets[(pid, tid, "i2b")] = (proc, "copy_image_to_buffer", m.group(1))
            continue
        m = _BRACKET_B2I.match(msg)
        if m:
            brackets[(pid, tid, "b2i")] = (proc, "copy_buffer_to_image", m.group(1))
            continue
        m = _MSU.match(msg)
        if m:
            k = (proc, int(m.group(1)), int(m.group(4)))
            msu[k] = max(msu.get(k, 0), int(m.group(7)))
            continue
        if msg.startswith("msupload: ") and not msg.startswith(("msupload: buffer ->", "msupload: pass built")) and re.search(
                r"^msupload: (FAILED\b|vkCreate\w*|no attachment view|more than)|->\s*-?\d+", msg):
            msu_failed.append("%s: %s" % (proc, msg[:140]))
            continue
        m = _FBF.match(msg)
        if m:
            verdict = m.group(3)
            if not m.group(2).startswith("READS"):
                tab = fbf_gone
            elif "undefined descriptor" in verdict or verdict.startswith("each read replaced"):
                tab = fbf_read
            elif verdict.startswith("IMPLEMENTED"):
                tab = fbf_impl
            else:
                tab = fbf_zmod
            tab[proc] = max(tab.get(proc, 0), int(m.group(1)))
            fbf_zeroed = max(fbf_zeroed, int(m.group(7)))
            continue
        m = _FBF_DRAW.match(msg)
        if m:
            cur = fbf_draw.get(proc, (0, 0, 0, 0))
            if int(m.group(1)) >= cur[0]:
                fbf_draw[proc] = tuple(int(m.group(i)) for i in (1, 2, 3, 4))
            continue
        m = _FBF_REASON.match(msg)
        if m:
            if m.group(1) == "UNSUPPORTED":
                fbf_unsup[(m.group(2)[:80], proc)] += 1
            continue
        m = _FBF_BAD.match(msg)
        if m:
            fbf_bad[(proc, (m.group(1) or "the render pass could not be RESTARTED")[:120])] += 1
            continue
        if msg.startswith("vk: fbfetch: the scan of a fragment module"):
            scan_failed += 1
    rows = []
    if refused:
        by = collections.OrderedDict()
        for (proc, call, reason), n in sorted(refused.items(), key=lambda kv: -kv[1]):
            by.setdefault("%s %s: %s" % (proc, call, reason), n)
        rows.append(("WARN", "plugin copy refused",
                     "%d logged refusal line(s) (a lower bound: the plugin logs the first few of each kind): the copy was DROPPED, the destination keeps "
                     "its old bytes (see docs/AFTER-TESTS.md, Metal plugin: blit copies): %s" %
                     (sum(refused.values()), "; ".join("%s (%d)" % (k, n) for k, n in list(by.items())[:4]))))
    if msu:
        rows.append(("INFO", "multisample upload",
                     "buffer -> multisample texture uploads ran through the plugin's own pass (Vulkan has no such copy; Chess aborted in RADV before): " +
                     "; ".join("%s %d-sample vk format %d: %d" % (p, s, f, n) for (p, s, f), n in sorted(msu.items())[:4])))
    if msu_failed:
        rows.append(("WARN", "multisample upload", "%d failure line(s), e.g. %s" % (len(msu_failed), msu_failed[0])))
    if fbf_read:
        rows.append(("WARN", "framebuffer fetch",
                     "fragment module(s) still READ [[color(n)]] (binding 192+n) in %s: the plugin's set layout has no such binding and no render pass "
                     "an input attachment, so RADV reads an undefined descriptor (random colours / corrupt glyphs are the suspects); %s; open: "
                     "DRIVER-PLAN N48 (real input attachment); NVMTL_FBFETCH_ZERO=1 makes the read a defined zero for an A/B" %
                     (", ".join("%s (up to module #%d)" % (p, n) for p, n in sorted(fbf_read.items())[:4]),
                      ("%d read(s) replaced by zero" % fbf_zeroed) if fbf_zeroed else "reads left as translated")))
    elif fbf_gone and not (fbf_impl or fbf_zmod):
        rows.append(("INFO", "framebuffer fetch", "[[color(n)]] declared in %s but no read is left in the module (folded away)" %
                     ", ".join(sorted(fbf_gone)[:4])))
    if fbf_impl or fbf_draw:
        rows.append(("INFO", "framebuffer fetch",
                     "IMPLEMENTED (render-pass input attachments, DRIVER-PLAN N48): modules served in %s; draws that fetched: %s" %
                     (", ".join("%s (up to module #%d)" % (p, n) for p, n in sorted(fbf_impl.items())[:4]) or "-",
                      "; ".join("%s %d draw(s), %d pass set(s), %d skipped, %d pass restart(s)" % ((p,) + v) for p, v in sorted(fbf_draw.items())[:4]) or "none logged")))
    if fbf_unsup:
        rows.append(("WARN", "framebuffer fetch",
                     "reads the implementation does not carry were replaced by a zero vector (a black / wrong blend for them): " +
                     "; ".join("%s in %s (%d)" % (r, p, n) for (r, p), n in sorted(fbf_unsup.items(), key=lambda kv: -kv[1])[:4])))
    if fbf_bad:
        rows.append(("WARN", "framebuffer fetch",
                     "the implemented path refused or failed (the draws that read are SKIPPED, nothing is drawn from an undefined descriptor): " +
                     "; ".join("%s: %s (%d)" % (p, msg, n) for (p, msg), n in sorted(fbf_bad.items(), key=lambda kv: -kv[1])[:4])))
    if scan_failed:
        rows.append(("WARN", "framebuffer fetch", "%d scan failure line(s): the plugin could not scan a fragment module for [[color(n)]] reads" % scan_failed))
    open_ = [(proc, call, state, pid, tid) for (pid, tid, _), (proc, call, state) in sorted(brackets.items())
             if state in ("recording", "layout transition recorded")]
    if open_:
        rows.append(("WARN", "plugin copy bracket",
                     "the last line of %d thread(s)' copy bracket is not 'recorded': %s - the process died at or after that point (or the plugin log "
                     "was cut); the bracket is 'recording' -> 'layout transition recorded' -> 'vkCmdCopy... recorded'" %
                     (len(open_), "; ".join("%s[%d:%s] %s: %s" % (proc, pid, tid, call, state) for proc, call, state, pid, tid in open_[:4]))))
    return rows
