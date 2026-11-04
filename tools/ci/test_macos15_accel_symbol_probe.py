#!/usr/bin/env python3
"""Synthetic nm/kmutil fixtures test probe policy, not actual Apple providers."""
import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macos15_accel_symbol_probe as probe


PROVIDER = "com.apple.iokit.IOAcceleratorFamily2"
BID = "test.symbol-probe"
STABLE = "_stable_fixture_import"


def nm(symbols):
    return "".join("                 U %s\n" % symbol for symbol in sorted(symbols))


def km(symbols, private=False):
    return "For arch x86_64:\n\tFor extension %s:\n" % BID + "".join(
        "\t\t%s in /System/Library/KernelCollections/BootKernelExtensions.kc: %s (1.0)\n" %
        (symbol, probe.kpi.PRIVATE if private and symbol == STABLE else PROVIDER)
        for symbol in sorted(symbols))


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.before = set(probe.PAIRS) | {STABLE}
        self.after = set(probe.PAIRS.values()) | {STABLE}

    def test_exact_four_declarations_without_enumerators(self):
        original = (probe.ROOT / probe.HEADER).read_bytes()
        result = probe.patch_types(original)
        self.assertEqual(len(probe.TYPES), 4)
        for name in probe.TYPES:
            self.assertIn(("enum %s : unsigned int;" % name).encode(), result.splitlines())
            self.assertNotIn(("typedef unsigned int %s;" % name).encode(), result)
        for name in probe.TYPES:
            old = ("typedef unsigned int %s;" % name).encode()
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "exactly one"):
                probe.patch_types(original.replace(old, b""))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "exactly one"):
                probe.patch_types(original + old + b"\n")

    def test_patch_is_confined_to_a_separate_temporary_source_copy(self):
        original = (probe.ROOT / probe.HEADER).read_bytes()
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"RUNNER_TEMP": directory}):
            temp = Path(directory)
            source, logs = temp / "source", temp / "logs"
            logs.mkdir()
            header = source / probe.HEADER
            header.parent.mkdir(parents=True)
            header.write_bytes(original)
            with contextlib.redirect_stdout(io.StringIO()):
                probe.patch(source, logs)
            self.assertEqual(header.read_bytes(), probe.patch_types(original))
            self.assertEqual((probe.ROOT / probe.HEADER).read_bytes(), original)
            diff = (logs / "candidate-header.patch").read_text()
            self.assertTrue(diff.startswith("--- a/accel/iofam/iofam_types.h\n+++ b/accel/iofam/iofam_types.h\n"))
            self.assertEqual(sum(line.startswith("+enum ") for line in diff.splitlines()), 4)
            for forbidden in (probe.ROOT, temp):
                with self.subTest(source=forbidden), self.assertRaisesRegex(ValueError, "separate source copy"):
                    probe.patch(forbidden, logs)
            header.write_bytes(original + b"// drift\n")
            with self.assertRaisesRegex(ValueError, "differs from the checkout"):
                probe.patch(source, logs)

    def test_exact_seven_changes_report_actual_provider_measurements(self):
        report = probe.compare(nm(self.before), km({STABLE}), nm(self.after), km(self.after))
        self.assertEqual(report["baseline_imports"], report["candidate_imports"])
        self.assertEqual(len(report["replacements"]), 7)
        self.assertEqual(report["candidate_unresolved"], [])
        for replacement in report["replacements"]:
            self.assertEqual(replacement["baseline_providers"], [])
            self.assertEqual(replacement["candidate_providers"], [PROVIDER])

    def test_current_baseline_and_both_sides_of_difference_are_required(self):
        new = next(iter(probe.PAIRS.values()))
        old = next(iter(probe.PAIRS))
        for before, providers, after in (
            (self.before - {old}, {STABLE}, self.after),
            (self.before, {STABLE, old}, self.after),
            (self.before, set(), self.after),
            (self.before, {STABLE}, self.after - {new}),
            (self.before, {STABLE}, self.after | {"_unexpected_import"}),
            (self.before, {STABLE}, self.after - {STABLE}),
        ):
            with self.subTest(before=before, providers=providers, after=after), self.assertRaises(ValueError):
                probe.compare(nm(before), km(providers), nm(after), km(after))

    def test_strict_guard_and_structured_positive_or_negative_result(self):
        missing = next(iter(probe.PAIRS.values()))
        for private, unresolved, expected in ((False, False, 0), (False, True, 1), (True, False, 1)):
            with self.subTest(private=private, unresolved=unresolved), tempfile.TemporaryDirectory() as directory:
                temp = Path(directory)
                baseline, candidate, logs = temp / "baseline", temp / "candidate", temp / "logs"
                logs.mkdir()
                for source in (baseline, candidate):
                    contents = source / probe.CONTENTS
                    contents.mkdir(parents=True)
                    (contents / "Info.plist").write_bytes(plistlib.dumps(
                        {"CFBundleIdentifier": BID, "OSBundleLibraries": {PROVIDER: "1.0"}}))
                for name, text in {
                    "baseline-nm.log": nm(self.before), "baseline-kmutil.log": km({STABLE}, private),
                    "candidate-nm.log": nm(self.after),
                    "candidate-kmutil.log": km(self.after - {missing} if unresolved else self.after),
                }.items():
                    (logs / name).write_text(text, encoding="utf-8")
                output = io.StringIO()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                    rc = probe.main(["check", str(baseline), str(candidate), str(logs)])
                self.assertEqual(rc, expected, output.getvalue())
                report = json.loads((logs / "result.json").read_bytes())
                self.assertEqual(report["candidate_strict_guard_exit"], int(unresolved))
                self.assertEqual(report["baseline_other_kpi_guard_exit"], int(private))
                self.assertEqual(report["candidate_unresolved"], [missing] if unresolved else [])
                self.assertEqual(report["result"], "NEGATIVE-name-level-result" if expected else "names-match-only")
                self.assertIn("FAIL" if expected else "NAME MATCH ONLY", output.getvalue())


if __name__ == "__main__":
    unittest.main()
