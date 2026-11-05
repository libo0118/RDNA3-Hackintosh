#!/usr/bin/env python3
"""Host-only tests: no kmutil, KC creation, kext loading or hardware."""
import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import struct
import sys
import unittest
from unittest import mock

import verify_probe as verify


def entry(name, off):
    raw = name.encode() + b"\0"
    size = (32 + len(raw) + 7) & ~7
    return struct.pack("<IIQQII", verify.LC_FILESET_ENTRY, size, 0x100000, off, 32, 0) + raw.ljust(size - 32, b"\0")


def header(kind, commands, cpu=0x01000007):
    return struct.pack("<8I", 0xFEEDFACF, cpu, 3, kind, len(commands), sum(map(len, commands)), 0, 0) + b"".join(commands)


def segment(off, section_name=b"__cstring"):
    vm, data_off = 0x100000 + off, off + 0x200
    name = b"__TEXT".ljust(16, b"\0")
    sec = struct.pack("<16s16sQQIIIIIIII", section_name.ljust(16, b"\0"), name, vm + 0x200,
                      0x80, data_off, 0, 0, 0, 2, 0, 0, 0)
    cmd = struct.pack("<II16s4Q4I", verify.LC_SEGMENT_64, 72 + len(sec), name,
                      vm, 0x300, off, 0x300, 7, 5, 1, 0) + sec
    return cmd


def fixture(names=(verify.BUNDLE_ID,), target_marker=True, other_marker=False, section_name=b"__cstring"):
    data = bytearray(0xc00)
    entries = [entry(name, 0x400 + i * 0x400) for i, name in enumerate(names)]
    top = header(12, entries)
    data[:len(top)] = top
    for i, name in enumerate(names):
        off = 0x400 + i * 0x400
        contents = header(11, [segment(off, section_name=section_name if i == 0 else b"__cstring")])
        data[off:off + len(contents)] = contents
        if (i == 0 and target_marker) or (i == 1 and other_marker):
            start = off + 0x200
            data[start:start + len(verify.MARKER)] = verify.MARKER
    return data


EMPTY_LIST = (set(), [], set())
XNU = ("test-xnu", EMPTY_LIST, EMPTY_LIST)


def bundle():
    return {"target": {"cpu_type": 0x01000007, "cpu_subtype": 3, "filetype": 11,
                       "kmod_info": {"name": verify.BUNDLE_ID, "version": "1"}},
            "info": {"CFBundleIdentifier": verify.BUNDLE_ID, "CFBundleExecutable": "AppleIGC",
                     "IOKitPersonalities": {"i226": {"IOPCIMatch": "0x125b8086"}},
                     "OSBundleRequired": "Root", "OSBundleLibraries": {"com.apple.kpi.iokit": "8.10.0"}},
            "declared": {"com.apple.kpi.iokit"}, "imports": {"_public", "_vtable"}}


def listing(provider="com.apple.kpi.iokit", also_resolve_vtable=False):
    text = ("For arch x86_64:\n\tFor extension %s:\n"
            "\t\t_public in /System/Kernel.kc: %s (1.0)\n") % (verify.BUNDLE_ID, provider)
    if also_resolve_vtable:
        text += "\t\t_vtable in /System/Kernel.kc: com.apple.kpi.iokit (1.0)\n"
    return text, "_public\n_vtable\n"


class NativeParityTests(unittest.TestCase):
    def setUp(self):
        self.official = bundle()
        self.probe = copy.deepcopy(self.official)
        self.output = listing()

    def parity(self, official_output=None, probe_output=None, xnu=XNU):
        return verify.native_parity(self.official, self.probe, official_output or self.output,
                                    probe_output or self.output, xnu)

    def test_matching_unresolved_is_parity_not_a_kc_check(self):
        self.assertEqual(self.parity(), ({"_vtable"}, 1, 1))

    def test_private_and_undeclared_fail_even_when_stock_matches(self):
        for provider in ("com.apple.kpi.private", "com.apple.kpi.other"):
            with self.subTest(provider=provider), self.assertRaisesRegex(ValueError, "native KPI policy"):
                self.parity(official_output=listing(provider), probe_output=listing(provider))

    def test_new_unresolved_fails(self):
        with self.assertRaisesRegex(ValueError, "unresolved symbols differ"):
            self.parity(official_output=listing(also_resolve_vtable=True))

    def test_different_import_fails_in_either_direction(self):
        for symbols in ({"_public", "_vtable", "_new"}, {"_public"}):
            self.probe["imports"] = symbols
            with self.subTest(symbols=symbols), self.assertRaisesRegex(ValueError, "imports differ"):
                self.parity()

    def test_offline_private_and_unsupported_fail(self):
        for private, unsupported in (({"_vtable"}, set()), (set(), {"_vtable"})):
            xnu = ("test-xnu", (private, [], set()), (unsupported, [], set()))
            with self.subTest(private=private), self.assertRaisesRegex(ValueError, "offline KPI policy"):
                self.parity(xnu=xnu)

    def test_unsupported_fails_even_if_declared(self):
        for b in (self.official, self.probe):
            b["declared"].add(verify.kpi.UNSUPPORTED)
            b["info"]["OSBundleLibraries"][verify.kpi.UNSUPPORTED] = "1.0"
        xnu = ("test-xnu", EMPTY_LIST, ({"_vtable"}, [], set()))
        with self.assertRaisesRegex(ValueError, "offline KPI policy"):
            self.parity(xnu=xnu)

    def test_metadata_compares_values_and_types(self):
        for key, value in (("OSBundleLibraries", {"com.apple.kpi.iokit": "9.0"}),
                           ("IOKitPersonalities", {"i226": {"IOPCIMatch": "changed"}}),
                           ("OSBundleRequired", "Safe Boot"), ("DTPlatformName", "iphoneos"), ("SomeFlag", True)):
            changed = copy.deepcopy(self.probe)
            changed["info"][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "Info.plist"):
                verify.compare_bundles(self.official, changed)
        self.probe["info"]["BuildMachineOSBuild"] = "new compiler host"
        verify.compare_bundles(self.official, self.probe)

    def test_nm_and_kmutil_listing_must_belong_to_this_binary(self):
        for out in ((self.output[0], "_public\n"),
                    (self.output[0].replace(verify.BUNDLE_ID, "other.id"), self.output[1]),
                    (self.output[0] + "\t\t_extra in /System/Kernel.kc: com.apple.kpi.iokit (1.0)\n",
                     self.output[1])):
            with self.subTest(out=out), self.assertRaises(ValueError):
                self.parity(probe_output=out)

    def test_native_calls_both_tools_for_both_kexts_and_prints_raw_output(self):
        self.official.update(path="official.kext", binary="official.kext/AppleIGC")
        self.probe.update(path="probe.kext", binary="probe.kext/AppleIGC")
        stdout = io.StringIO()
        with mock.patch.object(verify, "read_bundle", side_effect=[self.official, self.probe]), \
             mock.patch.object(verify.kpi, "run", side_effect=list(self.output) * 2) as run, \
             mock.patch.object(verify.kpi, "load_xnu_lists", return_value=XNU), redirect_stdout(stdout):
            verify.native("official.kext", "probe.kext")
        self.assertEqual(run.call_args_list, [
            mock.call(["kmutil", "libraries", "-p", "official.kext", "--undef-symbols"]),
            mock.call(["nm", "-u", "official.kext/AppleIGC"]),
            mock.call(["kmutil", "libraries", "-p", "probe.kext", "--undef-symbols"]),
            mock.call(["nm", "-u", "probe.kext/AppleIGC"])])
        self.assertEqual(stdout.getvalue().count(self.output[0]), 2)
        self.assertIn("NOT full symbol resolution", stdout.getvalue())


class KCTests(unittest.TestCase):
    def rejected(self, data, mode="probe"):
        with self.assertRaises((ValueError, struct.error)):
            verify.check_kc(bytes(data), mode)

    def test_valid_probe_and_official(self):
        verify.check_kc(bytes(fixture()), "probe")
        verify.check_kc(bytes(fixture(target_marker=False)), "official")
        self.rejected(fixture(), "official")

    def test_missing_or_duplicate_id(self):
        self.rejected(fixture(names=("com.example.other",)))
        self.rejected(fixture(names=(verify.BUNDLE_ID, verify.BUNDLE_ID)))

    def test_marker_in_other_member_or_metadata_does_not_count(self):
        data = fixture(names=(verify.BUNDLE_ID, "com.example.other"), target_marker=False, other_marker=True)
        self.rejected(data)
        # A forged target segment must not borrow the other member's bytes.
        struct.pack_into("<Q", data, 0x400 + 32 + 40, 0x800)
        struct.pack_into("<I", data, 0x400 + 32 + 72 + 48, 0xa00)
        self.rejected(data)
        data = fixture(target_marker=False)
        data[0x300:0x300 + len(verify.MARKER)] = verify.MARKER
        self.rejected(data)
        self.rejected(fixture(section_name=b"__text"))

    def test_probe_marker_must_be_a_complete_cstring(self):
        data = fixture(target_marker=False)
        data[0x600:0x600 + len(verify.MARKER) + 1] = b"x" + verify.MARKER
        self.rejected(data)
        self.rejected(data, "official")

    def test_fake_header_without_commands(self):
        data = fixture()
        struct.pack_into("<II", data, 0x400 + 16, 0, 0)
        self.rejected(data)

    def test_bad_command_bounds_and_entry_offsets(self):
        for where, value in ((16, 0xffffffff), (20, 0xffffffff), (32 + 4, 0x1000),
                             (32 + 24, 0xffffff), (32 + 16, len(fixture()) - 4),
                             (0x400 + 32 + 4, 0), (0x400 + 32 + 4, 0x1000)):
            data = fixture()
            struct.pack_into("<I", data, where, value)
            with self.subTest(where=where):
                self.rejected(data)

    def test_segment_section_bounds_and_identity(self):
        seg = 0x400 + 32
        section = seg + 72
        for where, fmt, value in ((seg + 40, "<Q", len(fixture()) - 1),  # fileoff
                                  (seg + 48, "<Q", 0x100000),  # filesize
                                  (section + 32, "<Q", 0xfffffffffffffff0),  # address
                                  (section + 40, "<Q", 0x400),  # size
                                  (section + 48, "<I", 0x10),  # section file offset
                                  (section + 56, "<I", len(fixture()) - 1),  # relocations
                                  (section + 64, "<I", 0),  # not S_CSTRING_LITERALS
                                  (section + 16, "<8s", b"wrong\0\0\0")):
            data = fixture()
            if where == section + 56:
                struct.pack_into("<I", data, section + 60, 1)
            struct.pack_into(fmt, data, where, value)
            with self.subTest(where=where):
                self.rejected(data)

    def test_non_x86_or_wrong_type_rejected(self):
        for where, value in ((4, 0x0100000c), (12, 2), (0x400 + 4, 0x0100000c), (0x400 + 12, 2)):
            data = fixture()
            struct.pack_into("<I", data, where, value)
            with self.subTest(where=where):
                self.rejected(data)

    def test_cli_returns_failure_for_malformed_kc(self):
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", ["verify_probe.py", "kc", "bad.kc", "probe"]), \
             mock.patch.object(verify.Path, "read_bytes", return_value=b"broken"), redirect_stderr(stderr):
            self.assertEqual(verify.main(), 1)
        self.assertIn("verify_probe: FAIL: truncated Mach-O header", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
