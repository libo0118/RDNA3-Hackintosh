"""Triage rows of DRIVER-PLAN N48 / N52 (Metal plugin: blit copies, buffer -> multisample upload, multisample -> buffer download, [[color(n)]] framebuffer fetch).
Read from 26-log-metal-plugin.txt (every process that loaded the plugin; lines `... Name[pid:tid] (RDNA3MTLDriver) NVMTL: msg`).
Kept in a module of its own so that tools/triage_reports.py only gains one call. The lines are the plugin's
(metal/plugin/nvmtl_vk.c, NVMTLObjects.m); docs/AFTER-TESTS.md "Metal plugin: blit copies ..." explains them."""
import collections
import re

_LINE = re.compile(r"\b([^\s\[\]]+)\[(\d+):([0-9a-f]+)\] \(RDNA3MTLDriver\) NVMTL: (.*)$")
_REFUSED = re.compile(r"^(copy_image_to_buffer|copy_buffer_to_image|blit copyFromTexture:toBuffer|blit copyFromBuffer:toTexture|msupload|msdl): REFUSED(?: \([^)]*\))? - (.*)$")
_BRACKET = re.compile(r"^copy_image_to_buffer: (?:\(special copy\) )?(recording|layout transition recorded|vkCmdCopyImageToBuffer recorded)")
_BRACKET_B2I = re.compile(r"^copy_buffer_to_image: (?:\(special copy\) )?(recording|vkCmdCopyBufferToImage recorded)")
# the lines of one upload (the first 8 of each kind have a literal of their own; the totals are in `msupload: SUMMARY`): interleaved / flat / wide
_MSU = re.compile(r"^msupload: buffer -> (\d+)-sample image (\d+)x(\d+) \(vk format (\d+) via view (\d+), mode (\d+)\)(.*)$")
_MSU_FLAT = re.compile(r"^msupload: FLAT layout fallback \((.*?)\) - buffer -> (\d+)-sample image (\d+)x(\d+) \(vk format (\d+) via view")
_MSU_WIDE = re.compile(r"^msupload: WIDE interleaved row - (\d+)-sample image (\d+)x(\d+) \(vk format (\d+) via view")
_SUMMARY = re.compile(r"^(msdl|msupload): SUMMARY (.*)$")
_CENSUS = re.compile(r"^G14 census: copy texture->buffer .*?vkfmt (\d+) samples (\d+) ")
_REF_FMT = re.compile(r"\(vk format (\d+)")
_FBF = re.compile(r"^vk: fbfetch: fragment module #(\d+) declares .*? and (READS them|no read is left \(folded away\));.*?: (.*?) "
                  r"\[(?:(\d+) served / )?(\d+) read / (\d+) declared-only / (\d+) zeroed / (\d+) zeroing failed so far\]")
# multisample texture -> buffer (N52): the first 8 of each aspect "msdl: STENCIL|DEPTH|COLOUR N-sample image WxH -> buffer (vk format F, aspect A via view V, mode M): ..."
_MSDL = re.compile(r"^msdl: (?:STENCIL|DEPTH|COLOUR) (\d+)-sample image (\d+)x(\d+) -> buffer \(vk format (\d+), aspect (\d+) via view (\d+), mode (\d+)\)")
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


def _reason(text):
    """The reason of a refusal line without its detail group and counters: `<reason> (vk format F, ...) [this reason N time(s) ...]` -> `<reason>`.
    Reasons contain parentheses themselves ("(8-bit RGBA ...)", "(view format, samples) pair"), so only a trailing detail group that starts like
    the plugin's (`(vk format`, `(format`, `(level`) is cut."""
    t = re.sub(r"\s*\[[^\]]*\]\s*$", "", text)
    for m in ("(vk format ", "(format ", "(level ", "(region ", "(usage "):
        i = t.rfind(" " + m)
        if i > 0:
            t = t[:i]
            break
    return t[:160]


def _kv(text):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


# the SUMMARY keys that are problems by themselves (plan refusals have their own row `plugin copy refused`)
_MS_PROBLEM_KEYS = ("r_inrp", "r_nodev", "r_noaddr", "r_nousage", "r_pipe", "f_setlayout", "f_pipelayout", "f_renderpass", "f_module", "f_pipeline", "f_table",
                    "f_view", "f_retire", "f_set", "f_framebuffer")


def _ms_problems(v):
    return ", ".join("%s=%d" % (k, v[k]) for k in _MS_PROBLEM_KEYS if v.get(k, 0) > 0)


def _chain_row(chain_seen, dl_sum, up_sum):
    """The row `multisample chain`: Apple's GL resolves the stencil of a 4-sample D32_S8 framebuffer with a stencil download, an R8_UINT upload, a resolve
    into a 1-sample R8_UINT texture, and two single-sample copies (DRIVER-PLAN N52). WARN when a process shows the first of them (census / refusal /
    counters) but a link of the chain is missing or the links differ by a large margin."""
    probs, ok = [], []
    for key in sorted(chain_seen):
        proc, pid = key
        dl, up = dl_sum.get(key), up_sum.get(key)
        who = "%s[%d]" % (proc, pid)
        if dl is None and up is None:
            probs.append("%s: a multisample stencil copy is in the census/refusals but there is no msdl / msupload SUMMARY line (the plugin predates N52, or the process ended before the first one)" % who)
            continue
        st = (dl or {}).get("d_stencil", 0)
        u13 = (up or {}).get("up_inter_13", 0) + (up or {}).get("up_flat_13", 0)
        r8 = (dl or {}).get("resolve_r8u", 0)
        bad = []
        if st == 0:
            bad.append("stencil downloads 0 (the first step of the chain never ran)")
        if u13 == 0:
            bad.append("R8_UINT uploads 0 (the second step never ran)")
        if r8 == 0:
            bad.append("no resolve into a 1-sample R8_UINT texture counted (resolve_r8u=0: the empty resolve pass never ran or was refused)")
        for a, b, what in ((st, u13, "stencil downloads %d vs R8_UINT uploads %d" % (st, u13)), (st, r8, "stencil downloads %d vs resolves %d" % (st, r8))):
            if a and b and abs(a - b) > max(2, max(a, b) // 4):
                bad.append("%s differ by more than 25%%" % what)
        if bad:
            probs.append("%s: %s" % (who, "; ".join(bad)))
        else:
            ok.append("%s: stencil downloads %d, R8_UINT uploads %d, resolves into R8_UINT %d" % (who, st, u13, r8))
    rows = []
    if probs:
        rows.append(("WARN", "multisample chain", "Chess's stencil resolve chain is not complete (stencil -> buffer -> 4-sample R8_UINT -> resolve -> 1-sample stencil; "
                     "a missing link leaves the 1-sample stencil with uninitialised bytes = streaks on the pieces): " + " | ".join(probs[:6])))
    if ok:
        rows.append(("INFO", "multisample chain", "the stencil resolve chain ran in every process that showed it: " + " | ".join(ok[:6])))
    return rows


def n48_plugin_rows(pluginlog=""):
    """[(level, name, text)]. WARN for a copy the plugin dropped, a multisample upload that failed, a framebuffer fetch read that
    is still in a pipeline, a scan that failed and a copy bracket that stays open (the process died at or after its last
    line); INFO for the multisample uploads that ran and for fetches the specialiser removed."""
    refused = collections.Counter()         # (process, call, reason) -> lines
    msu = {}                                 # (process, samples, fmt) -> highest running count
    msu_failed = []
    msu_flat = collections.Counter()         # (process, reason) -> FLAT-layout lines
    msu_wide = collections.Counter()         # process -> WIDE lines
    msdl = {}                                # (process, samples, fmt, aspect) -> lines (the plugin logs the first 8 of each aspect; the totals are in SUMMARY)
    msdl_failed = []
    sums = {"msdl": {}, "msupload": {}}      # kind -> (process, pid) -> {key: int} of the newest SUMMARY line
    chain_seen = set()                       # (process, pid) that shows a multisample stencil copy / the R8_UINT upload (census, refusal, counters)
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
        m = _CENSUS.match(msg)
        if m:
            asp = re.search(r" aspect (\d+) ", msg)
            # D32_S8, multisample, the stencil aspect (an older plugin's census has no aspect: counted): the stencil resolve chain of
            # Apple's GL; a depth or colour download of the same texture is not the chain
            if int(m.group(1)) == 130 and int(m.group(2)) > 1 and (not asp or asp.group(1) == "4"):
                chain_seen.add((proc, pid))
            continue
        m = _SUMMARY.match(msg)
        if m:
            sums[m.group(1)][(proc, pid)] = _kv(m.group(2))
            continue
        m = _REFUSED.match(msg)
        if m:
            reason = _reason(m.group(2))
            refused[(proc, m.group(1), reason)] += 1
            fm = _REF_FMT.search(m.group(2))
            if fm and (("copy_image_to_buffer" == m.group(1) and 127 <= int(fm.group(1)) <= 130 and "aspect option 4" in m.group(2)) or
                       ("copy_buffer_to_image" == m.group(1) and int(fm.group(1)) == 13)) and ("multisample" in m.group(2) or "-sample image" in m.group(2)):
                chain_seen.add((proc, pid))
            continue
        m = _BRACKET.match(msg)
        if m:
            brackets[(pid, tid, "i2b")] = (proc, "copy_image_to_buffer", m.group(1))
            continue
        m = _BRACKET_B2I.match(msg)
        if m:
            brackets[(pid, tid, "b2i")] = (proc, "copy_buffer_to_image", m.group(1))
            continue
        m = _MSU_FLAT.match(msg)
        if m:
            msu_flat[(proc, m.group(1))] += 1
            continue
        m = _MSU_WIDE.match(msg)
        if m:
            msu_wide[proc] += 1
            continue
        m = _MSU.match(msg)
        if m:
            k = (proc, int(m.group(1)), int(m.group(4)))
            msu[k] = msu.get(k, 0) + 1
            continue
        m = _MSDL.match(msg)
        if m:
            k = (proc, int(m.group(1)), int(m.group(4)), int(m.group(5)))
            msdl[k] = msdl.get(k, 0) + 1
            continue
        if msg.startswith("msdl: FAILED"):
            msdl_failed.append("%s: %s" % (proc, msg[:140]))
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
                     (sum(refused.values()), "; ".join("%s (%d)" % (k, n) for k, n in list(by.items())[:8]))))
    # the totals (SUMMARY, not capped) win over the first lines of each kind (capped at 8 per literal)
    for (proc, pid), v in sorted(sums["msupload"].items()):
        pf = ", ".join("%s: %s" % (k[len("up_"):], v[k]) for k in sorted(v) if k.startswith(("up_inter_", "up_flat_")))
        rows.append(("INFO", "multisample upload", "%s[%d]: %d upload(s) through the plugin's own pass (%d integer-output), %d sample-interleaved / %d flat (one element per pixel)%s" %
                     (proc, pid, v.get("done", 0), v.get("done_int", 0), v.get("inter", 0), v.get("flat", 0), (": " + pf) if pf else "")))
    flat_tot = [(key, v["flat"]) for key, v in sorted(sums["msupload"].items()) if v.get("flat", 0) > 0]
    if flat_tot or msu_flat:
        why = "; ".join("%s: %s (%d line(s))" % (p, r, n) for (p, r), n in sorted(msu_flat.items(), key=lambda kv: -kv[1])[:4])
        rows.append(("WARN", "multisample upload", "FLAT layout fallback (the row did not match Apple's sample-interleaved layout: every sample of a pixel got the one element, "
                     "which is wrong if Apple meant interleaved): %s%s" % (", ".join("%s[%d] %d upload(s)" % (k[0], k[1], n) for k, n in flat_tot[:4]), (" - " + why) if why else "")))
    wide_tot = [(key, v["wide"]) for key, v in sorted(sums["msupload"].items()) if v.get("wide", 0) > 0]
    if wide_tot or msu_wide:
        rows.append(("WARN", "multisample upload", "WIDE interleaved row (more than 255 B beyond one row of the region: it may be the pitch of a flat buffer under a sub-region, "
                     "i.e. the wrong layout): %s" % (", ".join("%s[%d] %d upload(s)" % (k[0], k[1], n) for k, n in wide_tot[:4]) or ", ".join("%s %d line(s)" % (p, n) for p, n in sorted(msu_wide.items())[:4]))))
    if msu and not sums["msupload"]:
        rows.append(("INFO", "multisample upload",
                     "buffer -> multisample texture uploads ran through the plugin's own pass (no SUMMARY line: the plugin predates it; counts are the logged lines): " +
                     "; ".join("%s %d-sample vk format %d: %d line(s)" % (p, s_, f, n) for (p, s_, f), n in sorted(msu.items())[:6])))
    if msu_failed:
        rows.append(("WARN", "multisample upload", "%d failure line(s), e.g. %s" % (len(msu_failed), msu_failed[0])))
    probs_u = ["%s[%d]: %s" % (k[0], k[1], _ms_problems(v)) for k, v in sorted(sums["msupload"].items()) if _ms_problems(v)]
    if probs_u:
        rows.append(("WARN", "multisample upload", "failures / refusals in the totals: " + "; ".join(probs_u[:6])))
    for (proc, pid), v in sorted(sums["msdl"].items()):
        if v.get("done", 0) or v.get("d_colour", 0) or v.get("d_depth", 0) or v.get("d_stencil", 0):
            rows.append(("INFO", "multisample download",
                         "%s[%d]: %d download(s) through the plugin's own compute pass (colour %d, depth %d, stencil %d; Vulkan has no such copy, before N52 it was dropped and "
                         "Chess's stencil resolve read uninitialised memory)" % (proc, pid, v.get("done", 0), v.get("d_colour", 0), v.get("d_depth", 0), v.get("d_stencil", 0))))
    if msdl and not sums["msdl"]:
        rows.append(("INFO", "multisample download",
                     "multisample texture -> buffer copies ran through the plugin's own compute pass (no SUMMARY line: the plugin predates it; aspect 1 colour / 2 depth / 4 stencil): " +
                     "; ".join("%s %d-sample vk format %d aspect %d: %d line(s)" % (p, s_, f, a, n) for (p, s_, f, a), n in sorted(msdl.items())[:6])))
    if msdl_failed:
        rows.append(("WARN", "multisample download", "%d failure line(s), e.g. %s" % (len(msdl_failed), msdl_failed[0])))
    probs_d = ["%s[%d]: %s" % (k[0], k[1], _ms_problems(v)) for k, v in sorted(sums["msdl"].items()) if _ms_problems(v)]
    if probs_d:
        rows.append(("WARN", "multisample download", "failures / refusals in the totals: " + "; ".join(probs_d[:6])))
    for key, v in list(sums["msdl"].items()) + list(sums["msupload"].items()):
        # a resolve into R8_UINT alone (an object-ID pass of any MSAA app) is not the chain: only a stencil download or an R8_UINT upload starts it
        if v.get("d_stencil", 0) or v.get("up_inter_13", 0) or v.get("up_flat_13", 0):
            chain_seen.add(key)
    rows.extend(_chain_row(chain_seen, sums["msdl"], sums["msupload"]))
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
