#!/usr/bin/env python3
"""DRIVER-PLAN N54: the kernel's GPU-memory coherence switches and counters in a report.

Reads 42-coherence.txt (tools/macos/collect.sh: `sysctl -n debug.rdna3_coh`, the switches, the
kernel's 'coherence:' lines) and the kernel log. coh_rows(cohtxt, log) -> [(level, name, text)]:

  coherence switches  which debug.rdna3_coh_<name> were on (WARN: not the default driver), and the
                      jobs that ran under each (a switch that was on but ran under no job proves nothing)
  fence copy          the self-test of the system-memory copy of each ring's fence (FAIL: it did not land)
  fence skew          VRAM fence values seen before their system copy (written BEFORE them): the window of
                      the plugin's 'fence early'; FAIL when the copy went stale
  wait idle / uc      the CPU waits of wait_idle (WARN on timeouts), the MTYPE_UC mappings
"""
import re

COH_BITS = ("fence_sys", "fence_inv", "pipe_sync", "wait_idle", "uc", "hdp_end", "vm_flush", "sdma_gcr")


def coh_counters(*texts):
    """{key: str|int} of the newest 'debug.rdna3_coh: flags=...' line"""
    found = None
    for t in texts:
        for l in (t or "").splitlines():
            m = re.match(r"\s*debug\.rdna3_coh: (flags=.*)$", l)
            if m:
                found = m.group(1)
    if not found:
        return {}
    out = {}
    for kv in found.split():
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        try:
            out[k] = int(v, 0)
        except ValueError:
            out[k] = v
    return out


def _selftest(c):
    st = {}
    m = re.match(r"gfx:(-?\d+),comp:(-?\d+),sdma:(-?\d+)$", str(c.get("selftest", "")))
    if m:
        st = {"gfx": int(m.group(1)), "comp": int(m.group(2)), "sdma": int(m.group(3))}
    return st


def coh_rows(cohtxt, log):
    rows = []
    log = log or ""
    cohtxt = cohtxt or ""
    c = coh_counters(cohtxt, log)
    kernel_has = "coherence: switches" in log or "debug.rdna3_coh" in cohtxt
    if not c:
        if "(no debug.rdna3_coh sysctl" in cohtxt or not kernel_has:
            rows.append(("INFO", "coherence", "no debug.rdna3_coh in this report (a core older than DRIVER-PLAN N54, "
                         "or 42-coherence.txt missing): the fence copy and the coherence switches were not measured"))
        else:
            rows.append(("WARN", "coherence", "the core has DRIVER-PLAN N54 (its 'coherence:' lines are in the log) "
                         "but no debug.rdna3_coh dump was packed: the counters are NOT measured"))
        for l in log.splitlines():
            if "coherence:" in l and ("did NOT land" in l or "STALE" in l):
                rows.append(("FAIL", "fence copy", l[l.find("coherence:"):][:300]))
        return rows

    flags = c.get("flags", 0) if isinstance(c.get("flags"), int) else 0
    on = [b for i, b in enumerate(COH_BITS) if flags & (1 << i)]
    jobs = c.get("jobs", 0)
    if on:
        eng = ", ".join("%s (%s jobs)" % (b, c.get("jobs_" + b, "?")) for b in on)
        rows.append(("WARN", "coherence switches", "ON at the end of the run: %s - this run does not show the default "
                     "driver; a picture that is right with a switch and wrong without it names the kernel rule "
                     "(docs/AFTER-TESTS.md 'Kernel coherence switches')" % eng))
    else:
        ran = [b for b in COH_BITS if isinstance(c.get("jobs_" + b), int) and c.get("jobs_" + b) > 0]
        if ran:
            rows.append(("INFO", "coherence switches", "all off at the end; during the run: %s (%d jobs under any)" % (
                ", ".join("%s %d jobs" % (b, c["jobs_" + b]) for b in ran), jobs)))
        else:
            rows.append(("INFO", "coherence switches", "all off (the default): no job ran under a coherence switch"))
    if c.get("changes", 0):
        changes = [l[l.find("coherence:"):][:200] for l in log.splitlines() + cohtxt.splitlines()
                   if "coherence:" in l and "switches now" in l]
        seen = []
        for ch in changes:
            if ch not in seen:
                seen.append(ch)
        rows.append(("INFO", "coherence changes", "%d change(s): %s" % (c["changes"], "; ".join(seen[-6:]) or "(lines not in the log)")))

    st = _selftest(c)
    if not st:
        rows.append(("WARN", "fence copy", "no self-test result in debug.rdna3_coh (%s)" % c.get("selftest", "missing")))
    else:
        bad = [r for r, v in st.items() if v == -1]
        nog = [r for r, v in st.items() if v == -2]
        ok = [r for r, v in st.items() if v == 1]
        if bad:
            rows.append(("FAIL", "fence copy", "the system-memory copy of the %s fence did NOT land in its self-test: "
                         "those rings keep the VRAM fence only (kernel log 'coherence: ... did NOT land')" % "/".join(bad)))
        elif nog:
            rows.append(("WARN", "fence copy", "no system page / GART for the fence copy (%s): nothing measured" % "/".join(nog)))
        else:
            rows.append(("PASS", "fence copy", "the fences of %s also land in system memory (self-test ok); the CPU "
                         "trusted the %s copy" % ("/".join(ok) or "no ring",
                                                  "SYSTEM" if flags & 1 else "VRAM")))

    checks = c.get("checks", 0)
    skews = c.get("skews", 0)
    if c.get("stale", 0):
        rows.append(("FAIL", "fence skew", "the system copy went STALE %d time(s) (50 ms behind its VRAM fence): the "
                     "page or its GART slot is broken; the VRAM fence answers on that ring" % c["stale"]))
    if isinstance(skews, int) and skews > 0:
        closed = c.get("skews_closed", 0) or 0
        mean = (c.get("skew_sum_us", 0) or 0) / closed if closed else 0
        rows.append(("WARN", "fence skew", "%d time(s) a VRAM fence was visible BEFORE its system copy, which the ring "
                     "writes first (%d closed, mean %.1f us, max %s us, %s checks): the window in which a CPU that trusts "
                     "the VRAM fence reads system memory the job wrote (the plugin's 'fence early'). amdgpu keeps the "
                     "fence in system memory: run with debug.rdna3_coh_fence_sys=1%s" % (
                         skews, closed, mean, c.get("skew_max_us", "?"), checks,
                         " (it WAS on: %d waits held back, %d spins)" % (c.get("sys_held", 0), c.get("skew_spins", 0))
                         if flags & 1 else "")))
    elif checks:
        rows.append(("PASS", "fence skew", "no VRAM fence ahead of its system copy in %d checks" % checks))
    else:
        rows.append(("INFO", "fence skew", "no fence check read the copy (no job, or no copy)"))

    w = c.get("wait_waits", 0)
    if w:
        to = c.get("wait_timeouts", 0)
        rows.append(("WARN" if to else "INFO", "wait idle", "%d CS ioctl(s) waited for their fence (%d us total, max "
                     "%s us), %d timed out at 200 ms" % (w, c.get("wait_us", 0), c.get("wait_max_us", "?"), to)))
    if c.get("uc_bo", 0) or c.get("uc_switch", 0):
        rows.append(("INFO", "uncached maps", "%d mapping(s) MTYPE_UC for a COHERENT/UNCACHED BO (gmc_v11_0_get_vm_pte), "
                     "%d by debug.rdna3_coh_uc" % (c.get("uc_bo", 0), c.get("uc_switch", 0))))
    if c.get("kcopy_gcr", 0):
        rows.append(("INFO", "sdma gcr", "%d of %d kernel SDMA copies between GL2 write-back+invalidate" % (
            c["kcopy_gcr"], c.get("kcopy", 0))))
    if c.get("vmflush_forced", 0):
        rows.append(("INFO", "vm flush", "%d job(s) with a forced VM flush (debug.rdna3_coh_vm_flush)" % c["vmflush_forced"]))
    return rows


# variant 4 of the A/B ladder (autoreport.sh LADDER_COH_RUNGS, `rdna3vm step ladder-coh` / --ladder-coh): one kernel switch per rung
LADDER_COH_RUNGS = ("baseline", "coh-all", "fence-sys", "pipe-sync", "wait-idle", "uc", "fence-inv", "sdma-gcr", "vm-flush", "hdp-end")
LADDER_COH_SWITCH = {"coh-all": "debug.rdna3_coh_all", "fence-sys": "debug.rdna3_coh_fence_sys", "pipe-sync": "debug.rdna3_coh_pipe_sync",
                     "wait-idle": "debug.rdna3_coh_wait_idle", "uc": "debug.rdna3_coh_uc", "fence-inv": "debug.rdna3_coh_fence_inv",
                     "sdma-gcr": "debug.rdna3_coh_sdma_gcr", "vm-flush": "debug.rdna3_coh_vm_flush", "hdp-end": "debug.rdna3_coh_hdp_end"}


def coh_ladder_switches(ladder_txt):
    """{rung: {"set": [(name, value, readback)], "failed": [line], "coh_end": counters}} from ladder.txt's sysctl lines"""
    out, cur = {}, None
    for line in (ladder_txt or "").splitlines():
        m = re.match(r"^== rung (\S+?)(?::.*)?$", line)
        if m:
            cur = out.setdefault(m.group(1), {"set": [], "failed": [], "coh_start": {}, "coh_end": {}})
            continue
        if line.startswith("== end rung"):
            cur = None
            continue
        if cur is None:
            continue
        m = re.match(r"^sysctl: (\S+)=(\S+) \(was (\S*), read back (\S*)\)", line)
        if m:
            cur["set"].append((m.group(1), m.group(2), m.group(4)))
            continue
        if (line.startswith("failed: sysctl") or line.startswith("WARN: sysctl")) and "put back" not in line:
            cur["failed"].append(line[:200])
            continue
        if line.startswith("coh_start: "):
            cur["coh_start"] = coh_counters("debug.rdna3_coh: " + line[len("coh_start: "):])
        if line.startswith("coh_end: "):
            cur["coh_end"] = coh_counters("debug.rdna3_coh: " + line[len("coh_end: "):])
    return out


def coh_ladder_rows(ladder_txt, rungs, problems=(), glyphs=None):
    """the rows of a variant-4 ladder: per rung whether its switch was on (set, read back, jobs ran under it), the automatic glyph verdict and the
    owner's answer; then the conclusion (the first single switch whose text is clean while the baseline's is broken)."""
    rows = []
    glyphs = glyphs or {}
    sw = coh_ladder_switches(ladder_txt)
    for p in problems:
        rows.append(("WARN", "ladder-coh", p))
    verdict = {}
    for name in LADDER_COH_RUNGS:
        r = rungs.get(name)
        if not r:
            rows.append(("WARN", "ladder-coh " + name, "did not run (cut short, or an older autoreport.sh)"))
            continue
        s = sw.get(name, {"set": [], "failed": [], "coh_end": {}})
        want = LADDER_COH_SWITCH.get(name)
        engaged, why = True, ""
        if want:
            hit = [x for x in s["set"] if x[0] == want]
            if not hit or hit[0][2] != hit[0][1]:
                engaged, why = False, "the switch was NOT on (%s)" % ("; ".join(s["failed"]) or "no sysctl line for " + want)
            else:
                bit = want[len("debug.rdna3_coh_"):]
                key = "jobs_" + bit if bit != "all" else "jobs"
                e_, b_ = s["coh_end"].get(key), (s.get("coh_start") or {}).get(key, 0)
                jobs = e_ - b_ if isinstance(e_, int) and isinstance(b_, int) else None   # the counters run for the whole boot: this rung's share
                why = "%s=1, %s job(s) ran under it" % (want, jobs if jobs is not None else "?")
                if jobs == 0:
                    engaged, why = False, why + ": no job, the rung proves nothing"
        g = glyphs.get(name) or {}
        gv = g.get("verdict", "none")
        verdict[name] = (gv, engaged)
        ans = {"y": "the owner said RIGHT", "n": "the owner said WRONG"}.get(r.get("verdict"), "no answer")
        lv = "WARN" if (not engaged or r.get("cut")) else "INFO"
        rows.append((lv, "ladder-coh " + name, "%s; glyphs: %s%s; %s%s" % (
            why or "nothing set (the default)", gv, (" (" + g.get("text", "")[:160] + ")") if g.get("text") else "", ans,
            "; CUT SHORT" if r.get("cut") else "")))
    base = verdict.get("baseline", ("none", True))[0]
    cured = [n for n in LADDER_COH_RUNGS[2:] if verdict.get(n, ("none", False)) == ("clean", True)]
    allv = verdict.get("coh-all", ("none", False))
    if base == "broken" and cured:
        rows.append(("FAIL", "ladder-coh verdict", "the baseline's text is broken and CLEAN with %s: that kernel rule is the cause "
                     "(docs/DRIVER-PLAN.md N54: make it the default after a confirming run)" % ", ".join(cured)))
    elif base == "broken" and allv == ("clean", True):
        rows.append(("FAIL", "ladder-coh verdict", "clean only with EVERY switch on (coh-all), with none alone: two rules together; "
                     "bisect the mask (debug.rdna3_coh_mask)"))
    elif base == "broken" and allv[0] == "broken" and allv[1]:
        rows.append(("WARN", "ladder-coh verdict", "broken even with every kernel coherence switch on (coh-all): the kernel's "
                     "ordering and caches are NOT the cause (look at the translator / the plugin)"))
    else:
        rows.append(("INFO", "ladder-coh verdict", "no automatic conclusion (baseline %s, coh-all %s): read the rows and the looks" % (base, allv[0])))
    return rows
