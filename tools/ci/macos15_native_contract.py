#!/usr/bin/env python3
"""Capture one immutable Intel build; reject expansion of its name-level contract.

capture EVIDENCE_DIR OUTPUT: local GitHub API JSON, job log and digest-pinned ZIPs.
check BUILD_DIR [--nm]: compare four kexts and userspace targets, never run kmutil.
This does not create a KC, verify a vtable/ABI, load a kext or validate hardware.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import struct
import subprocess
import sys
import tarfile
import zipfile

import kmutil_kpi_guard as kpi


BASELINE = {
    "repository": "libo0118/RDNA3-Hackintosh",
    "run_id": 41896280472,
    "job_id": 126806539895,
    "commit": "fce68dbc2cb932a75cf5fd989c22cac4900910ff",
}
ARTIFACTS = {
    "binaries.zip": {
        "id": 13356645862,
        "name": "experimental-macos15-x86_64-NOT-HARDWARE-VERIFIED",
        "digest": "sha256:683cea32997d54fd3aced5cab577def0e91c97039cc64d7d4f0b41bd99060d36",
    },
    "compile-logs.zip": {
        "id": 13354609881,
        "name": "experimental-macos15-compile-logs",
        "digest": "sha256:5ffd537ee5e6c5edb48902b5226f1bb31f17e13c6860b6393a0ea09bfde5376d",
    },
}
BUNDLES = ("RDNA3Accel", "RDNA3Accelerator", "RDNA3Framebuffer", "RDNA3AGDC")
USERSPACE = ("libdrm.dylib", "libdrm_amdgpu.dylib", "rdna3_drminfo", "rdna3_modetest", "rdna3_pmctl")
MANIFEST = Path(__file__).with_name("data") / "macos15-native-contract.json"
SUMMARY = re.compile(
    r"^kmutil_kpi_guard: ok (\w+)\.kext: (\d+) imports, (\d+) resolved by kmutil, "
    r"checked against (\S+) too, libraries declared: (.+)$", re.M)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def macho(data):
    """Thin little-endian Mach-O64, N_UNDF/non-STAB/value==0 as in nm -u."""
    def unpack(fmt, offset, end=None):
        size = struct.calcsize(fmt)
        require(0 <= offset <= (len(data) if end is None else end) - size,
                "truncated Mach-O structure")
        return struct.unpack_from(fmt, data, offset)

    magic, cpu, subtype, kind, ncmds, cmdbytes, flags, reserved = unpack("<8I", 0)
    require(magic == 0xFEEDFACF and cpu == 0x01000007 and subtype == 3,
            "expected thin x86_64 Mach-O64 (not ARM/universal/x86_64h)")
    end = 32 + cmdbytes
    require(end <= len(data) and ncmds <= cmdbytes // 8, "truncated Mach-O load commands")
    offset, symtab, versions, segments = 32, None, [], []
    for _ in range(ncmds):
        cmd, size = unpack("<II", offset, end)
        require(size >= 8 and size % 8 == 0 and offset + size <= end, "bad Mach-O load command size")
        if cmd == 2:  # LC_SYMTAB
            require(size == 24 and symtab is None, "invalid/duplicate LC_SYMTAB")
            symtab = unpack("<4I", offset + 8, offset + size)
        elif cmd == 0x19:  # LC_SEGMENT_64, used to locate the actual _kmod_info bytes
            values = unpack("<16s4Q4I", offset + 8, offset + size)
            _, vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = values
            require(size == 72 + 80 * nsects and fileoff + filesize <= len(data),
                    "invalid Mach-O segment extent")
            segments.append((vmaddr, vmsize, fileoff, filesize))
        elif cmd == 0x24:  # LC_VERSION_MIN_MACOSX
            require(size == 16, "invalid LC_VERSION_MIN_MACOSX")
            minimum, sdk = unpack("<II", offset + 8, offset + size)
            versions.append({"command": "LC_VERSION_MIN_MACOSX", "minimum": minimum, "sdk": sdk})
        elif cmd == 0x32:  # LC_BUILD_VERSION
            platform, minimum, sdk, tools = unpack("<4I", offset + 8, offset + size)
            require(size == 24 + 8 * tools, "invalid LC_BUILD_VERSION")
            versions.append({"command": "LC_BUILD_VERSION", "platform": platform,
                             "minimum": minimum, "sdk": sdk})
        elif cmd in (0x25, 0x2F, 0x30):
            raise ValueError("non-macOS target version command")
        offset += size
    require(offset == end and symtab is not None, "incomplete load commands or missing LC_SYMTAB")
    symoff, count, stroff, strsize = symtab
    require(symoff >= end and symoff + count * 16 <= len(data)
            and stroff >= symoff + count * 16 and stroff + strsize <= len(data),
            "invalid Mach-O symbol/string table bounds")
    strings = data[stroff:stroff + strsize]
    imports, kmod_addresses = set(), []
    for index in range(count):
        string, typ, section, desc, value = unpack("<IBBHQ", symoff + index * 16)
        require(string < strsize, "Mach-O symbol name outside string table")
        nul = strings.find(b"\0", string)
        require(nul != -1, "unterminated Mach-O symbol name")
        if typ & 0xE0:  # N_STAB
            continue
        name = strings[string:nul].decode("utf-8")
        if typ & 0x0E == 0 and value == 0:  # N_UNDF, excluding common symbols
            require(name and not any(c.isspace() for c in name), "invalid undefined symbol name")
            imports.add(name)
        if typ & 0x0E == 0x0E and name == "_kmod_info":
            kmod_addresses.append(value)
    target = {"cpu_type": cpu, "cpu_subtype": subtype, "filetype": kind,
              "version_commands": versions}
    if kind == 11:  # MH_KEXT_BUNDLE
        require(len(kmod_addresses) == 1, "missing/ambiguous _kmod_info definition")
        address = kmod_addresses[0]
        locations = [fileoff + address - vmaddr for vmaddr, vmsize, fileoff, filesize in segments
                     if vmaddr <= address and address + 144 <= vmaddr + min(vmsize, filesize)]
        require(len(locations) == 1, "_kmod_info outside file-backed segment")
        _, info_version, ident, name, version = unpack("<Qii64s64s", locations[0])
        require(b"\0" in name and b"\0" in version, "unterminated _kmod_info identity")
        target["kmod_info"] = {"info_version": info_version,
                               "name": name.split(b"\0", 1)[0].decode("ascii"),
                               "version": version.split(b"\0", 1)[0].decode("ascii")}
    return target, imports


def info_contract(info, name):
    require(isinstance(info, dict), "Info.plist root must be a dictionary")
    result = info.copy()
    if name != "RDNA3Accel":
        # Only these three Makefiles insert this non-binding, source-derived buildstamp.
        stamp = result.pop("RDNA3SourceHash")
        require(isinstance(stamp, str) and re.fullmatch(r"[0-9a-f]{10}", stamp),
                "invalid generated RDNA3SourceHash")
    return plistlib.dumps(result, fmt=plistlib.FMT_XML, sort_keys=True)


def safe_member(name):
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and "\\" not in name and ":" not in name and "\0" not in name
            and ".." not in path.parts and str(path) == name, "unsafe archive path: " + name)


def read_zip(path, expected):
    data = path.read_bytes()
    require("sha256:" + sha256(data) == expected["digest"], "artifact SHA256 mismatch: " + path.name)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        files = {}
        for item in archive.infolist():
            safe_member(item.orig_filename)
            safe_member(item.filename)
            require(not item.is_dir() and not stat.S_ISLNK(item.external_attr >> 16)
                    and item.file_size <= 64 * 1024 * 1024 and item.filename not in files,
                    "invalid/duplicate ZIP member")
            files[item.filename] = archive.read(item)  # ZipFile checks CRC while reading every member.
        return files


def read_tar(data):
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for item in archive:
            safe_member(item.name)
            require(item.isfile() and item.size <= 64 * 1024 * 1024 and item.name not in files,
                    "invalid/duplicate TAR member")
            files[item.name] = archive.extractfile(item).read()
    return files


def capture(directory):
    directory = Path(directory)
    run, job, artifacts = [json.loads((directory / name).read_bytes())
                           for name in ("run.json", "job.json", "artifacts.json")]
    require(run["id"] == BASELINE["run_id"] and run["head_sha"] == BASELINE["commit"]
            and run["repository"]["full_name"] == BASELINE["repository"]
            and run["status"] == "completed" and run["conclusion"] == "success", "wrong baseline run")
    require(job["id"] == BASELINE["job_id"] and job["run_id"] == BASELINE["run_id"]
            and job["head_sha"] == BASELINE["commit"] and job["labels"] == ["macos-15-intel"]
            and job["status"] == "completed" and job["conclusion"] == "success", "wrong/non-Intel baseline job")
    for step in ("Require macOS 15 Intel and the macOS 15 SDK", "Strict KPI guards (core, framebuffer and AGDC)",
                 "Strict accelerator KPI guard (names and providers only)"):
        require(any(s["name"] == step and s["status"] == "completed" and s["conclusion"] == "success"
                    for s in job["steps"]), "baseline step not successful: " + step)
    for expected in ARTIFACTS.values():
        matches = [a for a in artifacts["artifacts"] if a["id"] == expected["id"]]
        require(len(matches) == 1 and all(matches[0][k] == v for k, v in expected.items())
                and matches[0]["workflow_run"]["id"] == BASELINE["run_id"]
                and matches[0]["workflow_run"]["head_sha"] == BASELINE["commit"], "wrong baseline artifact metadata")
    logs = read_zip(directory / "compile-logs.zip", ARTIFACTS["compile-logs.zip"])
    binaries = read_zip(directory / "binaries.zip", ARTIFACTS["binaries.zip"])
    tarname = "rdna3-experimental-macos15-x86_64-NOT-HARDWARE-VERIFIED.tar.gz"
    require(set(binaries) == {tarname}, "unexpected binary archive members")
    files = read_tar(binaries[tarname])
    toolchain = logs["toolchain.log"].decode()
    os_version = re.search(r"^ProductVersion:\s+(15\.\S+)$", toolchain, re.M)
    os_build = re.search(r"^BuildVersion:\s+(\S+)$", toolchain, re.M)
    require(os_version and os_build and "Xcode 16.4\nBuild version 16F6\n" in toolchain
            and "/SDKs/MacOSX15.5.sdk\n" in toolchain and "Target: x86_64-" in toolchain,
            "baseline toolchain is not macOS15/Intel/Xcode16.4/SDK15.5")
    joblog_bytes = (directory / "native-job.log").read_bytes()
    joblog = joblog_bytes.decode("utf-8")
    summaries = {}
    for logname in ("kpi-strict.log", "kpi-accelerator-report.log"):
        text = logs[logname].decode()
        require(not any("kmutil_kpi_guard: " + level in text for level in ("FAIL", "ERROR", "WARN")),
                "baseline KPI guard failed or warned")
        for line in text.splitlines():
            require(line in joblog, "artifact guard log not found in native job log")
        for name, count, resolved, tag, libs in SUMMARY.findall(text):
            require(name not in summaries, "duplicate baseline guard summary")
            summaries[name] = {"imports": int(count), "resolved": int(resolved), "xnu_tag": tag,
                               "declared_libraries": libs.split(", ")}
    require(set(summaries) == set(BUNDLES), "incomplete native guard summaries")
    result = {"schema": 1, "baseline": {**BASELINE, "job_url": job["html_url"], "os_version": os_version[1],
              "os_build": os_build[1], "xcode": "16.4", "xcode_build": "16F6", "sdk": "15.5",
              "artifacts": ARTIFACTS, "job_log_sha256": sha256(joblog_bytes),
              "tar_sha256": sha256(binaries[tarname])}, "bundles": {}}
    for name in BUNDLES:
        prefix = name + ".kext/Contents/"
        raw_info, binary = files[prefix + "Info.plist"], files[prefix + "MacOS/" + name]
        info = plistlib.loads(raw_info)
        info_contract(info, name)
        target, imports = macho(binary)
        summary = summaries[name]
        require(target["filetype"] == 11 and summary["imports"] == len(imports)
                and summary["declared_libraries"] == sorted(info["OSBundleLibraries"]),
                "parser/native guard count or Info libraries disagree: " + name)
        require(target["kmod_info"]["name"] == info["CFBundleIdentifier"], "baseline kmod_info/Info name mismatch")
        require(summary["imports"] == summary["resolved"], "strict baseline imports did not all resolve: " + name)
        result["bundles"][name] = {"binary_sha256": sha256(binary), "info_sha256": sha256(raw_info),
            "target": target, "info_plist": info, "undefined_symbols": sorted(imports),
            "native_guard": {**summary, "scope": "native-name-level-guard", "unresolved_symbols": []}}
    nm_lines = logs["kext.log"].decode().split("undefined symbols (must all come from the OSBundleLibraries in Info.plist):\n", 1)[1]
    nm_symbols = set(re.findall(r"^(_\S+)$", nm_lines, re.M))
    require(nm_symbols == set(result["bundles"]["RDNA3Accel"]["undefined_symbols"]),
            "Mach-O parser disagrees with the original native nm -u listing")
    return result


def compare_bundle(name, expected, raw_info, binary):
    info = plistlib.loads(raw_info)
    target, imports = macho(binary)
    require(target == expected["target"], name + ": Mach-O target/kmod_info changed")
    require(info_contract(info, name) == info_contract(expected["info_plist"], name),
            name + ": Info.plist metadata changed (only generated RDNA3SourceHash may differ)")
    added = imports - set(expected["undefined_symbols"])
    require(not added, name + ": new imports need real native validation: " + " ".join(sorted(added)))
    tag, private, unsupported = kpi.load_xnu_lists()
    bad = kpi.xnu_verdicts(imports, set(info["OSBundleLibraries"]), private, unsupported)
    require(not bad, name + ": current " + tag + " KPI policy rejects: " + repr(bad))
    guard = expected["native_guard"]
    require(guard["scope"] == "native-name-level-guard"
            and guard["imports"] == guard["resolved"] == len(expected["undefined_symbols"])
            and guard["unresolved_symbols"] == [], name + ": invalid baseline validation scope/counts")
    stamp = ""
    if name != "RDNA3Accel":
        stamp = "; generated RDNA3SourceHash baseline=%s current=%s" % (
            expected["info_plist"]["RDNA3SourceHash"], info["RDNA3SourceHash"])
    return imports, "%s: %d/%d imports; fixed native name-level guard contract not expanded; current %s policy checked%s" % (
        name, len(imports), len(expected["undefined_symbols"]), tag, stamp)


def check(directory, manifest, use_nm=False):
    require(manifest["schema"] == 1 and all(manifest["baseline"][k] == v for k, v in BASELINE.items())
            and manifest["baseline"]["artifacts"] == ARTIFACTS and set(manifest["bundles"]) == set(BUNDLES),
            "unexpected baseline manifest/provenance")
    directory = Path(directory)
    for name in BUNDLES:
        contents = directory / (name + ".kext") / "Contents"
        binary = contents / "MacOS" / name
        imports, report = compare_bundle(name, manifest["bundles"][name],
                                         (contents / "Info.plist").read_bytes(), binary.read_bytes())
        if use_nm:
            output = subprocess.run(["xcrun", "nm", "-arch", "x86_64", "-u", str(binary)],
                                    check=True, capture_output=True, text=True).stdout
            require(kpi.parse_nm_undef(output) == imports, name + ": current nm -u and parser disagree")
        print(report)
    for name in USERSPACE:
        target, _ = macho((directory / "userspace" / name).read_bytes())
        require(target["filetype"] == (6 if name.endswith(".dylib") else 2), name + ": incorrect filetype")
        versions = target["version_commands"]
        require(len(versions) == 1 and versions[0]["minimum"] == 0x0F0000
                and versions[0]["sdk"] == 0x0F0500
                and versions[0].get("platform", 1) == 1, name + ": expected macOS15.0 target/SDK15.5")
        print(name + ": x86_64 macOS15.0 target/SDK15.5 (compile only)")
    print("No current x86_64 kmutil/KC binding, ABI/vtable, loading or hardware validation was performed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    cap = sub.add_parser("capture")
    cap.add_argument("evidence")
    cap.add_argument("output", type=Path)
    chk = sub.add_parser("check")
    chk.add_argument("build")
    chk.add_argument("--manifest", type=Path, default=MANIFEST)
    chk.add_argument("--nm", action="store_true", help="also require the current macOS nm -u to match")
    args = parser.parse_args()
    try:
        if args.command == "capture":
            args.output.write_text(json.dumps(capture(args.evidence), indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8", newline="\n")
            print("Captured immutable Intel baseline; all four bundles have strict native name-level guards.")
        else:
            check(args.build, json.loads(args.manifest.read_bytes()), args.nm)
    except (OSError, ValueError, KeyError, IndexError, struct.error, zipfile.BadZipFile,
            tarfile.TarError, subprocess.CalledProcessError, kpi.GuardError) as error:
        print("macos15_native_contract: FAIL: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
