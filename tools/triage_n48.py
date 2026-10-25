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
# the implemented path (N48 follow-up): per-pass sets, draws, restarts, and every way it refuses. The plugin's log keeps only the first 8
# lines of each format literal, so the TOTALS are in the SUMMARY line (key=value; the newest per process wins) and every kind of event has a literal
# of its own (metal/plugin/nvmtl_log_cap.h, nvmtl_fbfetch_plan.h).
_FBF_SUM = re.compile(r"^vk: fbfetch: SUMMARY (.*)$")
_FBF_RESTART = re.compile(r"^vk: fbfetch: a pipeline that reads .* RESTARTED")
_FBF_REASON = re.compile(r"^vk: fbfetch: (UNSUPPORTED|reads of attachments the pipeline lacks) \((.*?)\): reads of colour attachment\(s\) 0x([0-9a-f]+) are zeroed")
# lines that are failures by themselves (the start of the text after "vk: fbfetch: " / "fbfetch: ")
_FBF_BAD_PREFIX = ("a draw that reads", "BUG: ", "colour attachment ", "input-attachment descriptor set allocation FAILED", "descriptor layouts FAILED",
                   "this device allows only", "moving the subpass inputs", "ZEROING FAILED")


def _fbf_bad(msg):
    if msg.startswith("fbfetch: the render pass could not be RESTARTED"):
        return "the render pass could not be RESTARTED (the rest of that encoder is not drawn)"
    if msg.startswith("vk: fbfetch: "):
        t = msg[len("vk: fbfetch: "):]
        if t.startswith(_FBF_BAD_PREFIX):
            return re.sub(r"\s*[(\[].*$", "", t)[:110]
    return None


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
    fbf_sum = {}                             # (process, pid) -> {key: int} of the newest SUMMARY line
    fbf_restart_lines = collections.Counter()  # process -> RESTARTED lines (a lower bound; the summary has the total)
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
        m = _FBF_SUM.match(msg)
        if m:
            fbf_sum[(proc, pid)] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", m.group(1))}
            continue
        if _FBF_RESTART.match(msg):
            fbf_restart_lines[proc] += 1
            continue
        m = _FBF_REASON.match(msg)
        if m:
            if m.group(1) == "UNSUPPORTED":
                fbf_unsup[(m.group(2)[:80], proc)] += 1
            continue
        bad = _fbf_bad(msg)
        if bad:
            fbf_bad[(proc, bad)] += 1
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
                     "fragment module(s) READ [[color(n)]] (binding 192+n) from an UNDEFINED descriptor in %s (%s): the zeroing failed (see the ZEROING FAILED line), "
                     "or this log is from before the N48 implementation (no set layout / input attachment); random colours / corrupt glyphs are the suspects" %
                     (", ".join("%s (up to module #%d)" % (p, n) for p, n in sorted(fbf_read.items())[:4]),
                      ("%d module(s) with zeroed reads" % fbf_zeroed) if fbf_zeroed else "reads left as translated")))
    elif fbf_gone and not (fbf_impl or fbf_zmod):
        rows.append(("INFO", "framebuffer fetch", "[[color(n)]] declared in %s but no read is left in the module (folded away)" %
                     ", ".join(sorted(fbf_gone)[:4])))
    if fbf_impl or fbf_sum:
        tot = ["%s %d pipeline(s) served, %d draw(s), %d pass set(s), %d restart(s)" % (p, v.get("served", 0), v.get("draws", 0), v.get("sets", 0), v.get("restarts", 0))
               for (p, _), v in sorted(fbf_sum.items())[:4]]
        rows.append(("INFO", "framebuffer fetch",
                     "IMPLEMENTED (render-pass input attachments, DRIVER-PLAN N48): modules served in %s; %s" %
                     (", ".join("%s (up to module #%d)" % (p, n) for p, n in sorted(fbf_impl.items())[:4]) or "-",
                      "; ".join(tot) or "no SUMMARY line (the process ended before the first one, or the plugin predates it)")))
    # problems seen in the totals (they are not capped, the individual lines are)
    probs = []
    for (p, pid), v in sorted(fbf_sum.items()):
        for key, what in (("skipped", "draw(s) SKIPPED (nothing drawn from an undefined input)"), ("zero_failed", "zeroing failure(s): reads of an undefined descriptor"),
                          ("scan_failed", "module scan failure(s)"), ("move_failed", "failure(s) moving the inputs to their set"),
                          ("att_refused", "attachment(s) that cannot be input attachments"), ("alloc_failed", "descriptor-set allocation failure(s)"),
                          ("zr_ms", "pipeline(s) with a multisample read zeroed"), ("zr_int", "pipeline(s) with an integer-format read zeroed"),
                          ("zr_tess", "tessellation pipeline(s) with a read zeroed"), ("zr_nolayout", "pipeline(s) zeroed for lack of a descriptor layout"),
                          ("zr_forced", "pipeline(s) zeroed by NVMTL_FBFETCH_ZERO=1")):
            if v.get(key, 0) > 0:
                probs.append("%s[%d]: %d %s" % (p, pid, v[key], what))
        if v.get("restarts", 0) > 100:
            probs.append("%s[%d]: %d pass restart(s) (> 100: each costs a render-pass end/begin; a pipeline mix that reads after a non-reading one)" % (p, pid, v["restarts"]))
    for p, n in fbf_restart_lines.items():
        if n > 100:
            probs.append("%s: %d RESTARTED lines (> 100)" % (p, n))
    if probs:
        rows.append(("WARN", "framebuffer fetch", "totals: " + "; ".join(probs[:6])))
    if fbf_unsup:
        rows.append(("WARN", "framebuffer fetch",
                     "reads the implementation does not carry were replaced by a zero vector (a black / wrong blend for them): " +
                     "; ".join("%s in %s (%d)" % (r, p, n) for (r, p), n in sorted(fbf_unsup.items(), key=lambda kv: -kv[1])[:4])))
    if fbf_bad:
        rows.append(("WARN", "framebuffer fetch",
                     "the implemented path refused or failed (a draw that reads is SKIPPED, nothing is drawn from an undefined descriptor; BUG lines are logic errors): " +
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
