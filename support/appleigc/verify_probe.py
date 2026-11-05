#!/usr/bin/env python3
"""Check AppleIGC native baseline parity or its member in a built KC.

Native parity is a name-level comparison, not successful symbol resolution or
evidence that a KC links.  The kc check inspects a KC already produced by kmutil;
neither command loads a kext or writes to the system.
"""
import argparse
from pathlib import Path
import plistlib
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools" / "ci"), str(ROOT / "tools" / "probe")]
import kmutil_kpi_guard as kpi
from macos15_native_contract import macho
from kc_func import LC_FILESET_ENTRY, LC_SEGMENT_64, load_commands

BUNDLE_ID = "com.sxx.AppleIGC"
MARKER = b"i226-intx-probe-8802cba\0"
BUILD_FIELDS = {"BuildMachineOSBuild", "DTPlatformBuild", "DTPlatformVersion", "DTSDKBuild",
                "DTSDKName", "DTXcode", "DTXcodeBuild"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_bundle(path):
    path = Path(path).resolve()
    plist = path / "Contents" / "Info.plist"
    info, declared = kpi.declared_libs(str(plist))
    require(isinstance(info, dict) and info.get("CFBundleIdentifier") == BUNDLE_ID
            and info.get("CFBundleExecutable") == "AppleIGC", str(path) + ": wrong bundle identity")
    require(isinstance(info.get("OSBundleLibraries"), dict) and info["OSBundleLibraries"]
            and all(isinstance(k, str) and isinstance(v, str) and k and v
                    for k, v in info["OSBundleLibraries"].items()), str(path) + ": invalid libraries")
    require(isinstance(info.get("IOKitPersonalities"), dict) and info["IOKitPersonalities"]
            and isinstance(info.get("OSBundleRequired"), str) and info["OSBundleRequired"],
            str(path) + ": missing boot metadata")
    binary = path / "Contents" / "MacOS" / "AppleIGC"
    target, imports = macho(binary.read_bytes())
    require(target["filetype"] == 11 and target["kmod_info"]["name"] == BUNDLE_ID,
            str(path) + ": wrong Mach-O kext identity")
    return {"path": str(path), "binary": str(binary), "info": info, "declared": declared,
            "target": target, "imports": imports}


def compare_bundles(official, probe):
    keys = ("cpu_type", "cpu_subtype", "filetype", "kmod_info")
    require(all(official["target"][k] == probe["target"][k] for k in keys),
            "Mach-O CPU/filetype/kmod identity differs from official")
    infos = [{k: v for k, v in item["info"].items() if k not in BUILD_FIELDS}
             for item in (official, probe)]
    require(plistlib.dumps(infos[0], sort_keys=True) == plistlib.dumps(infos[1], sort_keys=True),
            "non-build Info.plist metadata differs from official")
    added = probe["imports"] - official["imports"]
    removed = official["imports"] - probe["imports"]
    require(not added and not removed, "Mach-O imports differ from official; added=%s removed=%s"
            % (sorted(added), sorted(removed)))


def evaluate_kpi(bundle, kmutil_text, nm_text, xnu):
    syms, exts = kpi.parse_kmutil(kmutil_text)
    undef = kpi.parse_nm_undef(nm_text)
    require(exts == [BUNDLE_ID] and syms and undef, "missing, extra or empty kmutil/nm listing")
    require(undef == bundle["imports"], "nm -u and Mach-O import sets differ")
    require(set(syms) <= undef, "kmutil listed a symbol not present in nm -u")
    bad = kpi.verdicts(syms, undef, bundle["declared"])
    policy_bad = [(kind, symbol, libs) for kind, symbol, libs in bad if kind != "unresolved"]
    require(not policy_bad, "native KPI policy rejects: " + repr(policy_bad))
    tag, private, unsupported = xnu
    offline_bad = kpi.xnu_verdicts(undef, bundle["declared"] - {kpi.UNSUPPORTED}, private, unsupported)
    require(not offline_bad, "%s offline KPI policy rejects: %r" % (tag, offline_bad))
    return {symbol for kind, symbol, _ in bad if kind == "unresolved"}, len(syms)


def native_parity(official, probe, official_output, probe_output, xnu):
    compare_bundles(official, probe)
    stock_bad, stock_resolved = evaluate_kpi(official, *official_output, xnu)
    probe_bad, probe_resolved = evaluate_kpi(probe, *probe_output, xnu)
    require(stock_bad == probe_bad, "unresolved symbols differ from official; added=%s removed=%s"
            % (sorted(probe_bad - stock_bad), sorted(stock_bad - probe_bad)))
    return stock_bad, stock_resolved, probe_resolved


def native(official_path, probe_path):
    official, probe = read_bundle(official_path), read_bundle(probe_path)
    outputs = []
    for name, bundle in (("official", official), ("probe", probe)):
        texts = []
        for cmd in (["kmutil", "libraries", "-p", bundle["path"], "--undef-symbols"],
                    ["nm", "-u", bundle["binary"]]):
            print("=== %s: %r ===" % (name, cmd), flush=True)
            output = kpi.run(cmd)
            print(output, end="" if output.endswith("\n") else "\n", flush=True)
            texts.append(output)
        outputs.append(texts)
    xnu = kpi.load_xnu_lists()
    unresolved, stock_resolved, probe_resolved = native_parity(official, probe, outputs[0], outputs[1], xnu)
    print("Native baseline parity: %d identical imports; kmutil listed official=%d probe=%d; "
          "%d identical unresolved: %s" % (len(official["imports"]), stock_resolved, probe_resolved,
                                             len(unresolved), " ".join(sorted(unresolved))))
    print("Offline policy %s: no private-only or unsupported-only imports." % xnu[0])
    print("Parity is NOT full symbol resolution or KC link validation; a real KC link is still required.")


def checked_commands(buf, off, filetype=None):
    require(0 <= off <= len(buf) - 32, "truncated Mach-O header")
    magic, cpu, _, kind, ncmds, cmdbytes, _, _ = struct.unpack_from("<8I", buf, off)
    require(magic == 0xFEEDFACF and cpu == 0x01000007
            and (filetype is None or kind == filetype), "wrong Mach-O magic, CPU or filetype")
    end = off + 32 + cmdbytes
    require(ncmds > 0 and ncmds <= cmdbytes // 8 and end <= len(buf), "invalid Mach-O command bounds")
    iterator = load_commands(buf, off)
    commands, pos = [], off + 32
    for _ in range(ncmds):
        require(pos + 8 <= end, "truncated load command")
        cmd, actual, size = next(iterator)
        require(actual == pos and size >= 8 and size % 8 == 0 and pos + size <= end,
                "invalid load command size")
        commands.append((cmd, pos, size))
        pos += size
    require(pos == end, "load command sizes do not match header")
    return commands, end


def checked_segment(buf, pos, size):
    require(size >= 72, "truncated LC_SEGMENT_64")
    segname, vm, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from("<16s4Q4I", buf, pos + 8)
    require(size == 72 + 80 * nsects and vm + vmsize <= 1 << 64
            and fileoff + filesize <= len(buf), "invalid LC_SEGMENT_64 extent")
    return segname, vm, vmsize, fileoff, filesize, nsects


def segments_and_cstrings(buf, commands):
    cstrings = []
    for cmd, pos, size in commands:
        if cmd != LC_SEGMENT_64:
            continue
        segname, vm, vmsize, fileoff, filesize, nsects = checked_segment(buf, pos, size)
        for i in range(nsects):
            section = pos + 72 + i * 80
            name, secseg, addr, length, offset, _, reloff, nreloc, flags = struct.unpack_from(
                "<16s16sQQ5I", buf, section)
            require(secseg == segname and vm <= addr and addr + length <= vm + vmsize,
                    "section outside its segment")
            require(reloff + nreloc * 8 <= len(buf), "section relocations outside file")
            zero_fill = flags & 0xff in (1, 0xc, 0x12)
            if zero_fill:
                require(offset == 0, "zero-fill section has a file offset")
            elif length or offset:
                mapped = fileoff + addr - vm
                require(addr + length <= vm + min(vmsize, filesize) and offset == mapped
                        and mapped + length <= fileoff + filesize, "section outside file-backed segment")
            if name.rstrip(b"\0") == b"__cstring":
                require(length and flags & 0xff == 2, "empty or non-cstring __cstring")
                cstrings.append((mapped, mapped + length))
    require(cstrings, "member has no file-backed __cstring")
    ordered = sorted(cstrings)
    require(all(left[1] <= right[0] for left, right in zip(ordered, ordered[1:])), "overlapping __cstring sections")
    return cstrings


def check_kc(buf, mode):
    require(mode in ("official", "probe"), "invalid KC mode")
    commands, outer_end = checked_commands(buf, 0, 12)  # MH_FILESET
    entries = []
    for cmd, pos, size in commands:
        if cmd != LC_FILESET_ENTRY:
            continue
        require(size >= 32, "truncated LC_FILESET_ENTRY")
        _, off, nameoff, _ = struct.unpack_from("<QQII", buf, pos + 8)
        require(32 <= nameoff < size, "fileset entry id outside command")
        start, end = pos + nameoff, pos + size
        nul = buf.find(b"\0", start, end)
        require(nul > start and off >= outer_end and off <= len(buf) - 32,
                "invalid fileset entry id or member offset")
        entries.append((buf[start:nul], off))
    selected = [off for name, off in entries if name == BUNDLE_ID.encode()]
    require(len(selected) == 1 and sum(off == selected[0] for _, off in entries) == 1,
            "missing, duplicate or aliased AppleIGC fileset entry")
    off = selected[0]
    member, member_end = checked_commands(buf, off, 11)  # MH_KEXT_BUNDLE
    cstrings = segments_and_cstrings(buf, member)
    require(all(start >= outer_end and (end <= off or start >= member_end) for start, end in cstrings),
            "__cstring overlaps Mach-O metadata")
    for _, other_off in entries:
        if other_off == off:
            continue
        other, other_end = checked_commands(buf, other_off)
        ranges = [(other_off, other_end)]
        for cmd, pos, size in other:
            if cmd == LC_SEGMENT_64:
                _, _, _, fileoff, filesize, _ = checked_segment(buf, pos, size)
                if filesize:
                    ranges.append((fileoff, fileoff + filesize))
        require(all(end <= lo or start >= hi for start, end in cstrings for lo, hi in ranges),
                "AppleIGC __cstring overlaps another fileset member")
    has_marker = False
    has_exact_marker = False
    for start, end in cstrings:
        content = buf[start:end]
        pos = content.find(MARKER)
        while pos != -1:
            has_marker = True
            has_exact_marker |= pos == 0 or content[pos - 1] == 0
            pos = content.find(MARKER, pos + 1)
    require(not has_marker if mode == "official" else has_exact_marker,
            "marker does not match KC mode in AppleIGC __cstring")
    print("KC %s: unique x86_64 AppleIGC kext member at 0x%x; marker %s in its __cstring"
          % (mode, off, "present" if has_marker else "absent"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    n = sub.add_parser("native")
    n.add_argument("official_kext")
    n.add_argument("probe_kext")
    k = sub.add_parser("kc")
    k.add_argument("path")
    k.add_argument("mode", choices=("official", "probe"))
    args = parser.parse_args()
    try:
        if args.command == "native":
            native(args.official_kext, args.probe_kext)
        else:
            check_kc(Path(args.path).read_bytes(), args.mode)
    except (OSError, ValueError, KeyError, TypeError, UnicodeError, struct.error, kpi.GuardError) as error:
        print("verify_probe: FAIL: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
