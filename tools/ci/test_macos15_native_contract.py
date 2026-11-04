#!/usr/bin/env python3
"""Small synthetic fixtures for the supplemental cross-build's fail-closed guard."""
import io
from contextlib import redirect_stdout
import json
from pathlib import Path
import plistlib
import stat
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macos15_native_contract as c


def fixture(imports=("_public",), name="RDNA3Accel", version="0.1.0", kind=11, sdk=None):
    strings = bytearray(b"\0")
    symbols = bytearray()
    for symbol, typ, value in [(s, 1, 0) for s in imports] + [
            ("_stab", 0xE0, 0), ("_common", 1, 8), ("_defined", 0x0F, 0x1000),
            ("_kmod_info", 0x0F, 0x1000)]:
        symbols += struct.pack("<IBBHQ", len(strings), typ, 0 if typ & 0x0E == 0 else 1, 0, value)
        strings += symbol.encode() + b"\0"
    kmod = struct.pack("<Qii64s64s", 0, 1, 0,
                       ("io.github.voten641." + name).encode(), version.encode())
    version_cmd = b"" if sdk is None else struct.pack("<6I", 0x32, 24, 1, 0x0F0000, sdk, 0)
    dataoff = 32 + 72 + 24 + len(version_cmd)
    segment = struct.pack("<II16s4Q4I", 0x19, 72, b"__DATA", 0x1000, len(kmod), dataoff, len(kmod), 3, 3, 0, 0)
    symoff = dataoff + len(kmod)
    table = struct.pack("<6I", 2, 24, symoff, len(symbols) // 16, symoff + len(symbols), len(strings))
    return (struct.pack("<8I", 0xFEEDFACF, 0x01000007, 3, kind, 2 + bool(version_cmd), 96 + len(version_cmd), 0, 0)
            + segment + table + version_cmd + kmod + symbols + strings)


def info(name="RDNA3Accel"):
    result = {"CFBundleIdentifier": "io.github.voten641." + name,
              "CFBundleExecutable": name, "CFBundleVersion": "0.1.0", "CFBundlePackageType": "KEXT",
              "OSBundleLibraries": {"com.apple.kpi.libkern": "19.0"}, "OSBundleRequired": "Root",
              "IOKitPersonalities": {"test": {"IOProviderClass": "IOPCIDevice", "IOClass": name,
                                              "IOPCIPrimaryMatch": "0x747e1002"}},
              "OtherMetadata": {"Enabled": True}}
    if name != "RDNA3Accel":
        result["RDNA3SourceHash"] = "0123456789"
    return result


def expected(name="RDNA3Accel", imports=("_public",)):
    target, symbols = c.macho(fixture(imports, name))
    return {"target": target, "info_plist": info(name), "undefined_symbols": sorted(symbols),
            "native_guard": {"scope": "native-name-level-guard", "imports": len(symbols),
                             "resolved": len(symbols), "unresolved_symbols": []}}


class MachOTests(unittest.TestCase):
    def test_nm_undef_semantics_and_actual_kmod(self):
        target, symbols = c.macho(fixture())
        self.assertEqual(symbols, {"_public"})
        self.assertEqual(target["filetype"], 11)
        self.assertEqual(target["version_commands"], [])
        self.assertEqual(target["kmod_info"], {"info_version": 1,
                         "name": "io.github.voten641.RDNA3Accel", "version": "0.1.0"})

    def test_truncated_inputs(self):
        data = fixture()
        for size in (0, 4, 31, 100, len(data) - 1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                c.macho(data[:size])

    def test_bad_load_commands(self):
        for offset, value in ((36, 0), (36, 7), (36, 0xFFFFFFF8), (16, 100), (20, 97)):
            data = bytearray(fixture())
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                c.macho(data)

    def test_symbol_and_string_bounds(self):
        data = fixture()
        for offset, value in ((112, len(data) + 8), (120, len(data) + 8), (272, 0xFFFFFFFF)):
            changed = bytearray(data)
            struct.pack_into("<I", changed, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                c.macho(changed)
        with self.assertRaisesRegex(ValueError, "unterminated"):
            c.macho(data[:-1] + b"x")

    def test_wrong_architecture_and_kmod_mapping(self):
        for offset, value in ((4, 0x0100000C), (8, 8), (72, 0xFFFFFFFF)):
            data = bytearray(fixture())
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                c.macho(data)


class ContractTests(unittest.TestCase):
    def compare(self, name="RDNA3Accel", baseline=None, current=None, binary=None):
        return c.compare_bundle(name, baseline or expected(name),
                                plistlib.dumps(info(name) if current is None else current),
                                fixture(name=name) if binary is None else binary)

    def test_subset_passes(self):
        symbols, report = self.compare(baseline=expected(imports=("_public", "_old")))
        self.assertEqual(symbols, {"_public"})
        self.assertIn("1/2 imports", report)
        self.assertIn("fixed native name-level guard contract not expanded", report)

    def test_new_import_is_not_an_allowlist_update(self):
        with self.assertRaisesRegex(ValueError, "new imports need real native validation: _new"):
            self.compare(binary=fixture(("_public", "_new")))

    def test_full_info_metadata_changes_fail(self):
        changes = [
            ("CFBundleIdentifier", "another.bundle"), ("CFBundleExecutable", "AnotherExecutable"),
            ("CFBundleVersion", "0.1.1"), ("CFBundlePackageType", "BNDL"),
            ("OSBundleRequired", "Local-Root"), ("OSBundleLibraries", {"com.apple.kpi.libkern": "20.0"}),
            ("IOKitPersonalities", {"test": {"IOPCIPrimaryMatch": "0x74801002"}}),
            ("OtherMetadata", {"Enabled": 1}), ("NewMetadata", "not allowed")]
        for key, value in changes:
            changed = info()
            changed[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "Info.plist metadata changed"):
                self.compare(current=changed)

    def test_only_three_generated_source_hashes_are_exempt_and_reported(self):
        for name in c.BUNDLES[1:]:
            changed = info(name)
            changed["RDNA3SourceHash"] = "abcdef0123"
            _, report = self.compare(name, current=changed)
            self.assertIn("baseline=0123456789 current=abcdef0123", report)
            changed["OSBundleLibraries"]["com.apple.kpi.libkern"] = "20.0"
            with self.assertRaisesRegex(ValueError, "Info.plist metadata changed"):
                self.compare(name, current=changed)
        changed = info()
        changed["RDNA3SourceHash"] = "abcdef0123"
        with self.assertRaisesRegex(ValueError, "Info.plist metadata changed"):
            self.compare(current=changed)

    def test_binary_kmod_identity_and_filetype_cannot_hide_behind_plist(self):
        for binary in (fixture(name="AnotherDriver"), fixture(version="0.2.0")):
            with self.assertRaisesRegex(ValueError, "target/kmod_info changed"):
                self.compare(binary=binary)
        changed = bytearray(fixture())
        struct.pack_into("<I", changed, 12, 2)
        with self.assertRaisesRegex(ValueError, "target/kmod_info changed"):
            self.compare(binary=changed)

    def test_all_bundles_require_strict_native_name_level_scope(self):
        for name in c.BUNDLES:
            _, report = self.compare(name)
            self.assertIn("fixed native name-level guard contract not expanded", report)
            for key, value in (("scope", "compile-and-import-contract-only"), ("resolved", 0),
                               ("unresolved_symbols", ["_public"])):
                baseline = expected(name)
                baseline["native_guard"][key] = value
                with self.subTest(name=name, key=key), self.assertRaisesRegex(ValueError, "invalid baseline"):
                    self.compare(name, baseline=baseline)

    def test_current_xnu_export_policy_is_rechecked(self):
        empty = (set(), [], set())
        with patch.object(c.kpi, "load_xnu_lists", return_value=("new-xnu", ({"_public"}, [], set()), empty)):
            with self.assertRaisesRegex(ValueError, "current new-xnu KPI policy rejects"):
                self.compare()

    def test_current_nm_disagreement_fails(self):
        with tempfile.TemporaryDirectory(prefix="native-contract-test-") as temp:
            contents = Path(temp) / "RDNA3Accel.kext" / "Contents"
            (contents / "MacOS").mkdir(parents=True)
            (contents / "Info.plist").write_bytes(plistlib.dumps(info()))
            (contents / "MacOS" / "RDNA3Accel").write_bytes(fixture())
            manifest = {"schema": 1, "baseline": {**c.BASELINE, "artifacts": c.ARTIFACTS},
                        "bundles": {name: expected(name) for name in c.BUNDLES}}
            with patch.object(c.subprocess, "run") as run:
                run.return_value.stdout = "_different\n"
                with self.assertRaisesRegex(ValueError, "nm -u and parser disagree"):
                    c.check(temp, manifest, use_nm=True)

    def test_userspace_sdk_target_and_non_macos_command(self):
        with tempfile.TemporaryDirectory(prefix="native-contract-test-") as temp, redirect_stdout(io.StringIO()):
            root = Path(temp)
            for name in c.BUNDLES:
                contents = root / (name + ".kext") / "Contents"
                (contents / "MacOS").mkdir(parents=True)
                (contents / "Info.plist").write_bytes(plistlib.dumps(info(name)))
                (contents / "MacOS" / name).write_bytes(fixture(name=name))
            (root / "userspace").mkdir()
            for name in c.USERSPACE:
                (root / "userspace" / name).write_bytes(fixture(kind=6 if name.endswith(".dylib") else 2, sdk=0x0F0500))
            manifest = {"schema": 1, "baseline": {**c.BASELINE, "artifacts": c.ARTIFACTS},
                        "bundles": {name: expected(name) for name in c.BUNDLES}}
            c.check(root, manifest)
            (root / "userspace" / "libdrm.dylib").write_bytes(fixture(kind=6, sdk=0x100000))
            with self.assertRaisesRegex(ValueError, "macOS15.0 target/SDK15.5"):
                c.check(root, manifest)
        changed = bytearray(fixture())
        struct.pack_into("<I", changed, 32, 0x25)
        with self.assertRaisesRegex(ValueError, "non-macOS target version"):
            c.macho(changed)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.run = {"id": c.BASELINE["run_id"], "head_sha": c.BASELINE["commit"],
                    "repository": {"full_name": c.BASELINE["repository"]},
                    "status": "completed", "conclusion": "success"}
        self.job = {"id": c.BASELINE["job_id"], "run_id": c.BASELINE["run_id"],
                    "head_sha": c.BASELINE["commit"], "labels": ["macos-15-intel"],
                    "status": "completed", "conclusion": "success", "html_url": "https://example.invalid/job",
                    "steps": [{"name": name, "status": "completed", "conclusion": "success"} for name in (
                        "Require macOS 15 Intel and the macOS 15 SDK",
                        "Strict KPI guards (core, framebuffer and AGDC)",
                        "Strict accelerator KPI guard (names and providers only)")]}
        self.artifacts = {"artifacts": [{**entry, "workflow_run": {
            "id": c.BASELINE["run_id"], "head_sha": c.BASELINE["commit"]}} for entry in c.ARTIFACTS.values()]}
        self.logs = {"toolchain.log": b"ProductVersion: 15.7.9\nBuildVersion: 24G830\nXcode 16.4\n"
                     b"Build version 16F6\n/SDKs/MacOSX15.5.sdk\nTarget: x86_64-apple-darwin24.6.0\n",
                     "kpi-strict.log": b"", "kpi-accelerator-report.log": b"",
                     "kext.log": b"undefined symbols (must all come from the OSBundleLibraries in Info.plist):\n_public\n"}
        self.files = {}
        for name in c.BUNDLES:
            prefix = name + ".kext/Contents/"
            self.files[prefix + "Info.plist"] = plistlib.dumps(info(name))
            self.files[prefix + "MacOS/" + name] = fixture(name=name)
            log = "kpi-accelerator-report.log" if name == "RDNA3Accelerator" else "kpi-strict.log"
            self.logs[log] += ("kmutil_kpi_guard: ok %s.kext: 1 imports, 1 resolved by kmutil, "
                               "checked against fixture-xnu too, libraries declared: com.apple.kpi.libkern\n" % name).encode()

    def capture(self):
        with tempfile.TemporaryDirectory(prefix="native-capture-test-") as temp:
            directory = Path(temp)
            for name, value in (("run.json", self.run), ("job.json", self.job), ("artifacts.json", self.artifacts)):
                (directory / name).write_text(json.dumps(value), encoding="utf-8")
            (directory / "native-job.log").write_bytes(b"".join(self.logs.values()))
            binaries = {"rdna3-experimental-macos15-x86_64-NOT-HARDWARE-VERIFIED.tar.gz": b"fixture tar"}
            with patch.object(c, "read_zip", side_effect=[self.logs, binaries]), \
                    patch.object(c, "read_tar", return_value=self.files):
                return c.capture(directory)

    def test_capture_requires_all_imports_resolved_without_warnings(self):
        result = self.capture()
        for name in c.BUNDLES:
            guard = result["bundles"][name]["native_guard"]
            self.assertEqual((guard["imports"], guard["resolved"], guard["unresolved_symbols"]), (1, 1, []))
            self.assertEqual(guard["scope"], "native-name-level-guard")
        logname = "kpi-accelerator-report.log"
        original = self.logs[logname]
        for text, message in ((original.replace(b"1 resolved", b"0 resolved"), "did not all resolve"),
                              (original.replace(b"1 imports", b"2 imports"), "count or Info libraries disagree"),
                              (b"kmutil_kpi_guard: WARN RDNA3Accelerator.kext: _public is exported by no kext\n"
                               + original, "baseline KPI guard")):
            with self.subTest(message=message), patch.dict(self.logs, {logname: text}), \
                    self.assertRaisesRegex(ValueError, message):
                self.capture()

    def test_capture_rejects_wrong_provenance(self):
        for mapping, key, value, message in (
                (self.run, "id", 0, "wrong baseline run"),
                (self.run, "head_sha", "0" * 40, "wrong baseline run"),
                (self.job, "labels", ["macos-15"], "wrong/non-Intel baseline job"),
                (self.artifacts["artifacts"][0], "digest", "sha256:" + "0" * 64, "wrong baseline artifact"),
                (self.artifacts["artifacts"][0]["workflow_run"], "head_sha", "0" * 40, "wrong baseline artifact")):
            with self.subTest(key=key, message=message), patch.dict(mapping, {key: value}), \
                    self.assertRaisesRegex(ValueError, message):
                self.capture()

    def test_capture_rejects_missing_or_unsuccessful_strict_steps(self):
        for step in self.job["steps"]:
            for key, value in (("name", "old/non-strict step"), ("status", "in_progress"), ("conclusion", "failure")):
                with self.subTest(step=step["name"], key=key), patch.dict(step, {key: value}), \
                        self.assertRaisesRegex(ValueError, "baseline step not successful"):
                    self.capture()


class ArchiveTests(unittest.TestCase):
    def archive(self, name="evidence.log", symlink=False):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
            entry = zipfile.ZipInfo(name)
            if symlink:
                entry.create_system = 3
                entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(entry, b"payload")
        # ZipInfo normalizes Windows separators; put the unsafe name back into both headers.
        return buffer.getvalue().replace(name.replace("\\", "/").encode(), name.encode())

    def read(self, data, digest=None):
        with tempfile.TemporaryDirectory(prefix="native-contract-test-") as temp:
            path = Path(temp) / "artifact.zip"
            path.write_bytes(data)
            return c.read_zip(path, {"digest": digest or "sha256:" + c.sha256(data)})

    def test_digest_and_crc(self):
        data = self.archive()
        self.assertEqual(self.read(data), {"evidence.log": b"payload"})
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            self.read(data, "sha256:" + "0" * 64)
        with self.assertRaises(zipfile.BadZipFile):
            self.read(data.replace(b"payload", b"PAYLOAD"))

    def test_unsafe_paths_and_symlinks(self):
        with self.assertRaisesRegex(ValueError, "unsafe archive path"):
            c.safe_member("a\0b")
        for name in ("../outside", "/absolute", "C:/escape", "a\\escape", "a/./b"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsafe archive path"):
                self.read(self.archive(name))
        with self.assertRaisesRegex(ValueError, "invalid/duplicate ZIP member"):
            self.read(self.archive(symlink=True))

    def test_committed_manifest_scope_and_counts(self):
        manifest = json.loads(c.MANIFEST.read_bytes())
        self.assertEqual(manifest["baseline"]["commit"], c.BASELINE["commit"])
        for name in c.BUNDLES:
            entry = manifest["bundles"][name]
            guard = entry["native_guard"]
            self.assertEqual(guard["imports"], len(entry["undefined_symbols"]))
            self.assertEqual(guard["imports"], guard["resolved"])
            self.assertEqual(guard["unresolved_symbols"], [])
            self.assertEqual(guard["scope"], "native-name-level-guard")
        accelerator = manifest["bundles"]["RDNA3Accelerator"]
        self.assertEqual(accelerator["target"]["kmod_info"]["version"], "0.1")
        self.assertEqual(accelerator["info_plist"]["CFBundleVersion"], "0.1.0")


if __name__ == "__main__":
    unittest.main(verbosity=2)
