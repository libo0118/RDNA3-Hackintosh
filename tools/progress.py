#!/usr/bin/env python3
"""
progress.py - the porting status shown in README.md (docs/status/).

    python3 tools/progress.py            # regenerate docs/status/
    python3 tools/progress.py --check    # exit 1 when docs/status/ is stale

Two measures, each item in one of three states:
  works    - ported, and the ladder steps that exercise it passed on the
             card (docs/status/hw-verified.txt)
  ported   - in the code, checked only in the simulator / CI
  todo     - not ported yet

* amdgpu kernel functions: every function of the amdgpu source files the
  project ports from (tools/progress/sources.txt, index in
  tools/progress/amdgpu-index.tsv). A function is ported when the driver
  sources name it (ports carry the amdgpu function name in a comment).
* amdgpu UAPI: amdgpu ioctls, DRM core ioctls libdrm uses, INFO queries,
  firmware / sensor sub-queries and CS chunk ids (third_party/amd/
  amdgpu_drm.h, uapi/rdna3_drm.h). An item is implemented when
  uapi/rdna3_drm.c (libdrm_xnu for fds, the kext's ops.sensor for
  sensors) handles it instead of rejecting it; a firmware sub-query counts
  once the kext fills its version.

Items that do not apply - not on the RX 7800 XT, or of no use on macOS -
are left out of the totals and listed with their reason
(tools/progress/not-applicable.txt); one the driver ports still counts.
"""
import json
import re
import sys
import tempfile
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "status"
INDEX = ROOT / "tools" / "progress" / "amdgpu-index.tsv"
VERIFIED = OUT / "hw-verified.txt"
NOT_APPLICABLE = ROOT / "tools" / "progress" / "not-applicable.txt"
DRIVER_DIRS = ["hw", "kext", "uapi", "common", "fb", "accel", "userspace/libdrm_xnu"]
DRIVER_EXT = {".c", ".h", ".cpp", ".hpp", ".m", ".mm"}

# group -> ladder steps (docs/TESTING-VM.md) that must pass on the card for
# the group to count as working; a step no ladder has yet keeps it unverified
AMDGPU_STEPS = {
    "PSP 13": ["4"], "SMU 13 power": ["5"], "GMC / GPUVM": ["6", "7"],
    "IH interrupts": ["6", "6b"], "SoC / NBIO / HDP": ["1", "4"], "SDMA 6": ["7"],
    "GFX 11": ["8"], "Rings / fences": ["7", "8"], "MES queues": ["mes"],
    "User queues": ["userq"],
    "Discovery / VBIOS": ["2"], "DMCUB": ["5b"], "Display: OTG / AUX": ["2", "9"],
    "Display: modeset": ["modeset"],
    "Display: DP link": ["dplink"],
    "Display: hotplug": ["hotplug"],
    "Display: color": ["gamma"],
    "VCN / JPEG video": ["vcn"],
}
UAPI_STEPS = {
    "amdgpu ioctls": ["7", "8"], "DRM core ioctls": ["7", "8"], "INFO queries": ["7"],
    "INFO firmware / sensors": ["5", "7"], "CS chunks": ["7", "8"],
}
# items added to a group after its steps passed: they need their own step
# (regex over the item name -> steps) before they count as working
ITEM_STEPS = [
    (re.compile(r"DRM_AMDGPU_USERQ(_SIGNAL|_WAIT)?|AMDGPU_INFO_UQ_FW_AREAS"), ["userq"]),
    # the video block's parts of verified groups (SMU power messages, the
    # NBIO doorbell range, the codec tables, the generic ring helpers the
    # VCN / JPEG rings use)
    (re.compile(r"smu_v13_0_set_(vcn|jpeg)_enable|nbio_v4_3_vcn_doorbell_range|soc21_query_video_codecs"
                r"|amdgpu_fence_count_emitted|amdgpu_ib_ring_tests|amdgpu_ring_commit"
                r"|amdgpu_ring_emit_reg_write_reg_wait_helper|amdgpu_ring_insert_nop"), ["vcn"]),
    # amdgpu enables CP gfx shadowing only on SR-IOV VFs: no step can run it
    (re.compile(r"AMDGPU_CHUNK_ID_CP_GFX_SHADOW|gfx_v11_0_ring_emit_(gfx_shadow|init_cond_exec)"
                r"|amdgpu_ring_patch_cond_exec|gfx_v11_0_get_gfx_shadow_info"), ["gfxshadow"]),
    (re.compile(r"gfx_v11_0_gfx_mqd_set_priority|sdma_v6_0_mqd_init"
                r"|gfx_v11_0_get_gfx_shadow_info_nocheck"), ["userq"]),
    # the DP side of files the TMDS modeset step verified
    (re.compile(r"dcn10_link_encoder_(dp_set_lane_settings|dp_set_phy_pattern|enable_dp_output"
                r"|get_max_link_cap|set_dp_phy_pattern_training_pattern)"
                r"|dcn32_(disable_link_output|unblank_stream)|(enable|disable)_link_dp|disable_prbs_mode"
                r"|enable_phy_bypass_mode|enc1_configure_encoder|enc32_(dp_set_odm_combine|enable_fifo"
                r"|reset_fifo|set_dig_input_mode|stream_encoder_dp_unblank)|link_encoder_disable"
                r"|link_transmitter_control|set_dp_phy_pattern_passthrough_mode"
                r"|set_link_training_complete|setup_panel_mode"), ["dplink"]),
    # SMU 13 PM control (rdna3_pmctl) joined the verified step 5 group
    (re.compile(r"smu_v13_0_baco_\w+|smu_v13_0_get_bamaco_support"), ["baco"]),
    (re.compile(r"smu_v13_0_(get|set)_fan_control_mode|smu_v13_0_auto_fan_control"
                r"|smu_v13_0_set_fan_(static_mode|speed_pwm|speed_rpm)"
                r"|smu_v13_0_0_(get_fan_speed_pwm|get_power_limit|set_power_limit"
                r"|get_overdrive_table|upload_overdrive_table|set_default_od_settings"
                r"|get_od_setting_limits|is_od_feature_supported|od_edit_dpm_table"
                r"|od_restore_table_single|restore_user_od_settings|set_supported_od_feature_mask"
                r"|force_clk_levels|get_dpm_ultimate_freq|populate_umd_state_clk"
                r"|get_power_profile_mode|get_thermal_temperature_range|get_unique_id"
                r"|get_current_clk_freq_by_table"
                r"|set_df_cstate)"), ["pmctl"]),
]

GREEN, YELLOW, GRAY = "#2ea043", "#d29922", "#6e7681"
BG, TEXT = "#0d1117", "#ffffff"
PANEL_W, GAP, MAP_H, HEAD = 495, 10, 300, 52


# ------------------------------------------------------------------ data

def verified_steps():
    steps = set()
    if VERIFIED.exists():
        for line in VERIFIED.read_text().splitlines():
            line = line.split("#", 1)[0].split()
            if line:
                steps.add(line[0])
    return steps


def driver_words():
    words = set()
    for d in DRIVER_DIRS:
        for p in (ROOT / d).rglob("*"):
            if p.suffix in DRIVER_EXT and p.is_file():
                words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", p.read_text(errors="ignore")))
    return words


def not_applicable():
    """[(compiled regex, reason)] from tools/progress/not-applicable.txt"""
    rules = []
    if NOT_APPLICABLE.exists():
        for line in NOT_APPLICABLE.read_text().splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            pat, _, why = re.split(r"(\s{2,})", line.strip(), maxsplit=1)
            rules.append((re.compile(pat), why.strip()))
    return rules


NA_RULES = not_applicable()


def na_reason(item):
    for rx, why in NA_RULES:
        if rx.fullmatch(item):
            return why
    return None


def item_works(item, works_ok, passed):
    for rx, steps in ITEM_STEPS:
        if rx.fullmatch(item):
            return all(s in passed for s in steps)
    return works_ok


def group_entry(name, steps, works_ok, items, passed=frozenset()):
    """items: [(item, ported_bool)]; an item that does not apply and is not
    ported is left out of the counts (listed under "na")"""
    state, na = {}, {}
    for item, ported in items:
        why = None if ported else na_reason(item)
        if why:
            na[item] = why
            continue
        works = item_works(item, works_ok, passed)
        state[item] = ("works" if works else "ported") if ported else "todo"
    count = {k: sum(1 for v in state.values() if v == k) for k in ("works", "ported", "todo")}
    return {"name": name, "steps": steps, **count, "items": dict(sorted(state.items())),
            "na": dict(sorted(na.items()))}


def summarize(groups):
    works = sum(g["works"] for g in groups)
    ported = sum(g["ported"] for g in groups)
    total = works + ported + sum(g["todo"] for g in groups)
    pct = lambda n: round(100.0 * n / total, 1) if total else 0.0
    return {"works": works, "ported": works + ported, "total": total,
            "not_applicable": sum(len(g["na"]) for g in groups),
            "works_pct": pct(works), "ported_pct": pct(works + ported), "groups": groups}


def collect_amdgpu(words, passed):
    groups = {}
    for line in INDEX.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        group, _path, fn = line.split("\t")
        groups.setdefault(group, set()).add(fn)
    out = []
    for name, fns in groups.items():
        steps = AMDGPU_STEPS.get(name, ["?"])
        out.append(group_entry(name, steps, all(s in passed for s in steps),
                               [(f, f in words) for f in fns], passed))
    return summarize(out)


def uapi_handled(src):
    """case labels -> handled? A label (or run of labels) whose first
    statement rejects (return -RD_E..., break, default) is not handled."""
    handled = {}
    pending = []
    for m in re.finditer(r"case\s+([^:]+?)\s*:|default\s*:|([^\s][^\n]*)", src):
        if m.group(1):
            pending.append(m.group(1).strip())
            continue
        if not pending:
            continue
        stmt = m.group(0).strip()
        if stmt.startswith("/*") or stmt.startswith("//"):
            continue
        reject = stmt.startswith("default") or re.match(r"(return\s+-RD_E|break\s*;)", stmt)
        for lab in pending:
            handled[lab] = handled.get(lab, False) or not reject
        pending = []
    return handled


def collect_uapi(passed):
    hdr = (ROOT / "third_party/amd/amdgpu_drm.h").read_text()
    rdh = (ROOT / "uapi/rdna3_drm.h").read_text()
    src = (ROOT / "uapi/rdna3_drm.c").read_text()
    xnu = (ROOT / "userspace/libdrm_xnu/drm_xnu.c").read_text()
    defs = re.findall(r"^\s*#define\s+([A-Z][A-Z0-9_]+)\s+(?:0x[0-9a-fA-F]+|\d+)\b", hdr, re.M)
    handled = uapi_handled(src)
    # AMDGPU_INFO_SENSOR answers come from the kext's ops.sensor
    for lab, ok in uapi_handled((ROOT / "kext/src/RDNA3Bringup.cpp").read_text()).items():
        if lab.startswith("AMDGPU_INFO_SENSOR_"):
            handled[lab] = handled.get(lab, False) or ok
    srcwords = set(re.findall(r"[A-Z][A-Z0-9_]+", src))

    def amdgpu_ioctl(n):
        return handled.get(f"RD_NR_AMDGPU({n})", False)

    kext = "".join(p.read_text(errors="ignore") for p in (ROOT / "kext/src").glob("*.cpp"))

    fwc = set(re.findall(r"AMDGPU_INFO_FW_[A-Z0-9_]+", (ROOT / "common/amd_fw.c").read_text()))

    def plain(n):
        if n.startswith("AMDGPU_INFO_FW_") and n != "AMDGPU_INFO_FW_VERSION":     # answered from dev->fw_version[], set by the kext
            if n in fwc and "amd_fw_versions(" in kext:   # from the firmware headers (amd_fw_versions)
                return True
            return re.search(r"(?:fw_version|fw_feature|ver|feat)\[\s*" + n + r"\s*\]", src + kext) is not None
        if n in handled:
            return handled[n]
        return n in srcwords

    core = [n for n in re.findall(r"#define\s+(RD_NR_[A-Z0-9_]+)", rdh)
            if n not in ("RD_NR_AMDGPU", "RD_NR_XNU_SYNCOBJ_FENCE", "RD_NR_XNU_FILE_ID")]   # libdrm_xnu helpers
    fd_in_xnu = {"RD_NR_SYNCOBJ_HANDLE_TO_FD": "XNU_NR_SYNCOBJ_HANDLE_TO_FD",
                 "RD_NR_SYNCOBJ_FD_TO_HANDLE": "XNU_NR_SYNCOBJ_FD_TO_HANDLE"}

    def core_ok(n):
        if n in fd_in_xnu:
            return fd_in_xnu[n] in xnu
        return handled.get(n, False)

    sets = {
        "amdgpu ioctls": [(n, amdgpu_ioctl(n)) for n in defs if n.startswith("DRM_AMDGPU_")],
        "DRM core ioctls": [(n.replace("RD_NR_", "DRM_IOCTL_"), core_ok(n)) for n in core],
        "INFO queries": [(n, plain(n)) for n in defs if n.startswith("AMDGPU_INFO_")
                         and (n == "AMDGPU_INFO_FW_VERSION"
                              or not re.match(r"AMDGPU_INFO_(FW_|SENSOR_|VIDEO_CAPS_|MMR_)", n))],
        "INFO firmware / sensors": [(n, plain(n)) for n in defs if n != "AMDGPU_INFO_FW_VERSION"
                                    and re.match(r"AMDGPU_INFO_(FW_|SENSOR_)", n)],
        "CS chunks": [(n, plain(n)) for n in defs if n.startswith("AMDGPU_CHUNK_ID_")],
    }
    out = []
    for name, items in sets.items():
        steps = UAPI_STEPS[name]
        out.append(group_entry(name, steps, all(s in passed for s in steps), items, passed))
    return summarize(out)


# ---------------------------------------------------------------- render

def squarify(values, x, y, w, h):
    """treemap rectangles, same order as values (sorted descending)"""
    rects, rest = [], list(values)
    total = float(sum(rest))
    scale = w * h / total if total else 0
    areas = [v * scale for v in rest]

    def worst(row, side):
        s = sum(row)
        return max(max(side * side * r / (s * s), s * s / (side * side * r)) for r in row)

    row = []
    while areas:
        side = min(w, h)
        if not row or worst(row + [areas[0]], side) <= worst(row, side):
            row.append(areas.pop(0))
            continue
        x, y, w, h = lay(row, x, y, w, h, rects)
        row = []
    if row:
        lay(row, x, y, w, h, rects)
    return rects


def lay(row, x, y, w, h, rects):
    s = sum(row)
    if w >= h:
        thick = s / h
        off = 0.0
        for a in row:
            rects.append((x, y + off, thick, a / thick))
            off += a / thick
        return x + thick, y, w - thick, h
    thick = s / w
    off = 0.0
    for a in row:
        rects.append((x + off, y, a / thick, thick))
        off += a / thick
    return x, y + thick, w, h - thick


def grid(n, x, y, w, h):
    if n <= 0:
        return []
    rows = max(1, min(n, round((n * h / w) ** 0.5)))
    cells = []
    for r in range(rows):
        k = n // rows + (1 if r < n % rows else 0)
        cw = w / k
        cells += [(x + c * cw, y + r * h / rows, cw, h / rows) for c in range(k)]
    return cells


def rect(x, y, w, h, fill, stroke=BG, sw=0.5):
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"/>')


def label(x, y, s, size, fill=TEXT):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="Verdana,DejaVu Sans,sans-serif" '
            f'font-size="{size}" fill="{fill}" stroke="{BG}" stroke-width="3" '
            f'paint-order="stroke">{escape(s)}</text>')


def panel(title, data, left):
    out = [label(left + 4, 20, title, 15),
           label(left + 4, 42, f'works {data["works_pct"]}%   ported {data["ported_pct"]}%   '
                 f'({data["works"]} / {data["ported"]} / {data["total"]})', 12)]
    groups = sorted((g for g in data["groups"] if g["works"] + g["ported"] + g["todo"]),
                    key=lambda g: -(g["works"] + g["ported"] + g["todo"]))
    sizes = [g["works"] + g["ported"] + g["todo"] for g in groups]
    for g, (x, y, w, h) in zip(groups, squarify(sizes, left, HEAD, PANEL_W, MAP_H)):
        n = g["works"] + g["ported"] + g["todo"]
        pct_w = 100 * g["works"] / n
        pct_p = 100 * (g["works"] + g["ported"]) / n
        out.append(f'<g><title>{escape(g["name"])}: works {pct_w:.0f}%, ported {pct_p:.0f}% '
                   f'({g["works"]}+{g["ported"]} of {n}); verified by step(s) '
                   f'{escape(", ".join(g["steps"]))}</title>')
        for i, c in enumerate(grid(n, x + 1, y + 1, w - 2, h - 2)):
            out.append(rect(*c, GREEN if i < g["works"] else YELLOW if i < g["works"] + g["ported"] else GRAY))
        out.append(rect(x, y, w, h, "none", BG, 2))
        name = g["name"] if len(g["name"]) * 7 < w - 8 else g["name"][:max(0, int((w - 14) / 7))] + "…"
        if h > 34 and w > 40:
            out.append(label(x + 5, y + 15, name, 11))
            out.append(label(x + 5, y + 29, f"{pct_w:.0f}% / {pct_p:.0f}%", 10))
        out.append("</g>")
    return out


def legend(y):
    x, out = 4, []
    for color, text in ((GREEN, "works (verified on the card)"),
                        (YELLOW, "ported, not verified on the card yet"), (GRAY, "not ported")):
        out.append(rect(x, y, 12, 12, color, BG, 0))
        out.append(label(x + 17, y + 11, text, 11))
        x += 30 + 7 * len(text)
    return out


def render(amdgpu, uapi):
    width = 2 * PANEL_W + GAP
    height = HEAD + MAP_H + 24
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
           f'width="{width}" height="{height}">', rect(0, 0, width, height, BG, BG, 0)]
    out += panel("amdgpu kernel functions*", amdgpu, 0)
    out += panel("amdgpu UAPI (ioctls, INFO, CS chunks)", uapi, PANEL_W + GAP)
    out += legend(HEAD + MAP_H + 6)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def badge(name, data):
    """three-part badge: name | works X% | ported Y%"""
    w_txt, p_txt = f'works {data["works_pct"]}%', f'ported {data["ported_pct"]}%'
    a, b, c = (10 + 7 * len(s) for s in (name, w_txt, p_txt))
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{a + b + c}" height="20" role="img" '
            f'aria-label="{name}: {w_txt}, {p_txt}">'
            f'<rect width="{a}" height="20" fill="#555"/>'
            f'<rect x="{a}" width="{b}" height="20" fill="{GREEN}"/>'
            f'<rect x="{a + b}" width="{c}" height="20" fill="{YELLOW}"/>'
            f'<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle">'
            f'<text x="{a / 2}" y="14">{name}</text><text x="{a + b / 2}" y="14">{w_txt}</text>'
            f'<text x="{a + b + c / 2}" y="14">{p_txt}</text></g></svg>\n')


def markdown(amdgpu, uapi, passed):
    def table(title, col, data):
        rows = [f"## {title}", "",
                f'{data["not_applicable"]} items that do not apply (not on the RX 7800 XT, or of no use on macOS) '
                "are left out of the totals; each group lists them with the reason "
                "(`tools/progress/not-applicable.txt`).", "",
                f"| {col} | Works | Ported | Total | Works % | Ported % | Verified by step |",
                "|---|---:|---:|---:|---:|---:|---|"]
        for g in sorted(data["groups"], key=lambda g: g["name"].lower()):
            n = g["works"] + g["ported"] + g["todo"]
            rows.append(f'| {g["name"]} | {g["works"]} | {g["works"] + g["ported"]} | {n} | '
                        f'{100 * g["works"] / n:.0f}% | {100 * (g["works"] + g["ported"]) / n:.0f}% | '
                        f'{", ".join(g["steps"])} |')
        rows.append(f'| **Total** | {data["works"]} | {data["ported"]} | {data["total"]} | '
                    f'{data["works_pct"]}% | {data["ported_pct"]}% | |')
        rows.append("")
        for g in sorted(data["groups"], key=lambda g: g["name"].lower()):
            todo = [k for k, v in g["items"].items() if v == "todo"]
            done = [k for k, v in g["items"].items() if v != "todo"]
            na_txt = f', {len(g["na"])} do not apply' if g["na"] else ""
            rows.append(f'<details><summary>{g["name"]}: {len(done)} ported, {len(todo)} to do{na_txt}</summary>\n')
            if done:
                rows.append("Ported: " + ", ".join(f"`{k}`" for k in done) + "\n")
            if todo:
                rows.append("To do: " + ", ".join(f"`{k}`" for k in todo) + "\n")
            if g["na"]:
                by = {}
                for k, why in g["na"].items():
                    by.setdefault(why, []).append(k)
                rows.append("Not counted (does not apply):\n")
                for why, ks in by.items():
                    rows.append(f"- {why}: " + ", ".join(f"`{k}`" for k in ks))
                rows.append("")
            rows.append("</details>\n")
        return rows

    lines = ["# Porting status", "",
             "Generated by `tools/progress.py` (do not edit). Steps passed on the card: "
             + (", ".join(sorted(passed)) if passed else "none yet") + " (`docs/status/hw-verified.txt`).", "",
             "![progress map](progress.svg)", ""]
    lines += table("amdgpu kernel functions", "Block", amdgpu)
    lines += table("amdgpu UAPI", "Group", uapi)
    return "\n".join(lines) + "\n"


def generate(out):
    passed = verified_steps()
    words = driver_words()
    amdgpu, uapi = collect_amdgpu(words, passed), collect_uapi(passed)
    out.mkdir(parents=True, exist_ok=True)
    (out / "progress.json").write_text(json.dumps({"amdgpu": amdgpu, "uapi": uapi,
                                                   "verified_steps": sorted(passed)}, indent=1) + "\n")
    (out / "progress.svg").write_text(render(amdgpu, uapi))
    (out / "badge-amdgpu.svg").write_text(badge("amdgpu port*", amdgpu))
    (out / "badge-uapi.svg").write_text(badge("amdgpu UAPI", uapi))
    (out / "STATUS.md").write_text(markdown(amdgpu, uapi, passed))
    return amdgpu, uapi


def main():
    if "--check" in sys.argv:
        with tempfile.TemporaryDirectory() as t:
            generate(Path(t))
            stale = [f.name for f in Path(t).iterdir()
                     if not (OUT / f.name).exists() or (OUT / f.name).read_text() != f.read_text()]
        if stale:
            print("docs/status is stale (run python3 tools/progress.py):", ", ".join(sorted(stale)))
            sys.exit(1)
        print("docs/status is up to date")
        return
    amdgpu, uapi = generate(OUT)
    for n, d in (("amdgpu", amdgpu), ("uapi", uapi)):
        print(f'{n}: works {d["works"]} ({d["works_pct"]}%), ported {d["ported"]} '
              f'({d["ported_pct"]}%) of {d["total"]}')


if __name__ == "__main__":
    main()
