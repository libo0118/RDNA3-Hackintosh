#!/usr/bin/env python3
"""Test four opaque enum declarations in a disposable source copy, never the driver.

patch SOURCE LOGS: require a copy under RUNNER_TEMP and preserve the public patch.
check BASELINE_SOURCE CANDIDATE_SOURCE LOGS: compare captured native nm/kmutil output.
Only symbol names/providers are tested; no ABI/vtable, KC, loading or hardware claim.
"""
import argparse
import difflib
import json
import os
from pathlib import Path
import sys

import kmutil_kpi_guard as kpi


ROOT = Path(__file__).resolve().parents[2]
HEADER = Path("accel/iofam/iofam_types.h")
CONTENTS = Path("build/RDNA3Accelerator.kext/Contents")
TYPES = ("IOAccelStereoMode", "eDoSwap", "eIOAccelSurfaceShapeBits", "eLockType")
# Hypotheses, not provider evidence: check() must remeasure both builds on this Mac.
PAIRS = {
    "__ZN14IOAccelSurface20surface_lock_optionsEjjP25IOAccelSurfaceInformationy":
        "__ZN14IOAccelSurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy",
    "__ZN14IOAccelSurface22surface_unlock_optionsEjj":
        "__ZN14IOAccelSurface22surface_unlock_optionsE9eLockTypej",
    "__ZN14IOAccelSurface28set_shape_backing_length_extEjjyjyP19IOAccelDeviceRegiony":
        "__ZN14IOAccelSurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony",
    "__ZN14IOAccelSurface9set_shapeEjjP19IOAccelDeviceRegiony":
        "__ZN14IOAccelSurface9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony",
    "__ZN17IOAccelGLContext211processSwapEj":
        "__ZN17IOAccelGLContext211processSwapE7eDoSwap",
    "__ZN24IOAccelLegacyDisplayPipe9setStereoEj":
        "__ZN24IOAccelLegacyDisplayPipe9setStereoE17IOAccelStereoMode",
    "__ZN27IOAccelLegacyDisplayMachine9setStereoEjj":
        "__ZN27IOAccelLegacyDisplayMachine9setStereoEj17IOAccelStereoMode",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def patch_types(original):
    result = original
    for name in TYPES:
        old = ("typedef unsigned int %s;" % name).encode("ascii")
        require(original.splitlines().count(old) == 1 and original.count(old) == 1,
                "expected exactly one unchanged declaration: " + old.decode("ascii"))
        result = result.replace(old, ("enum %s : unsigned int;" % name).encode("ascii"))
    return result


def patch(source, logs):
    source = source.resolve(strict=True)
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    require(source != temporary and source.is_relative_to(temporary)
            and source != ROOT and not ROOT.is_relative_to(source),
            "candidate must be a separate source copy strictly under RUNNER_TEMP")
    header = source / HEADER
    require(header.resolve(strict=True) == header, "candidate header must not be a symlink")
    original = header.read_bytes()
    require(original == (ROOT / HEADER).read_bytes(), "candidate header differs from the checkout")
    result = patch_types(original)
    diff = difflib.unified_diff(original.decode("utf-8").splitlines(keepends=True),
                                result.decode("utf-8").splitlines(keepends=True),
                                fromfile="a/" + HEADER.as_posix(), tofile="b/" + HEADER.as_posix())
    (logs / "candidate-header.patch").write_text("".join(diff), encoding="utf-8", newline="\n")
    header.write_bytes(result)
    print("Patched only the disposable copy: four opaque enums, no enumerators or vtable edits.")


def compare(before_nm, before_km, after_nm, after_km):
    before, after = kpi.parse_nm_undef(before_nm), kpi.parse_nm_undef(after_nm)
    before_providers, _ = kpi.parse_kmutil(before_km)
    after_providers, _ = kpi.parse_kmutil(after_km)
    require(before - set(before_providers) == set(PAIRS),
            "current normal build does not have exactly the seven expected unresolved imports")
    require(before - after == set(PAIRS), "removed imports are not exactly the original seven")
    require(after - before == set(PAIRS.values()), "added imports are not exactly the seven enum hypotheses")
    return {
        "scope": "symbol-names-and-current-providers-only; NOT ABI/vtable/KC/loading/hardware",
        "baseline_imports": len(before),
        "candidate_imports": len(after),
        "baseline_unresolved": sorted(before - set(before_providers)),
        "candidate_unresolved": sorted(after - set(after_providers)),
        "replacements": [{"baseline_symbol": old, "baseline_providers": before_providers.get(old, []),
                          "candidate_symbol": new, "candidate_providers": after_providers.get(new, [])}
                         for old, new in PAIRS.items()],
    }


def check(baseline, candidate, logs):
    before_nm, before_km, after_nm, after_km = [
        (logs / name).read_text(encoding="utf-8") for name in
        ("baseline-nm.log", "baseline-kmutil.log", "candidate-nm.log", "candidate-kmutil.log")]
    # Only the measured, exact seven may be unresolved in the normal control build.
    baseline_rc = kpi.check("normal-control-NOT-VERIFIED", before_km, before_nm,
                            baseline / CONTENTS / "Info.plist", warn_unresolved=True)
    candidate_rc = kpi.check("opaque-enum-candidate-NAMES-ONLY", after_km, after_nm,
                             candidate / CONTENTS / "Info.plist")
    report = compare(before_nm, before_km, after_nm, after_km)
    report.update(baseline_other_kpi_guard_exit=baseline_rc, candidate_strict_guard_exit=candidate_rc,
                  result="names-match-only" if baseline_rc == candidate_rc == 0 else "NEGATIVE-name-level-result")
    (logs / "result.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                       encoding="utf-8", newline="\n")
    for replacement in report["replacements"]:
        print("Candidate: %s -> %s" % (replacement["candidate_symbol"],
              ", ".join(replacement["candidate_providers"]) or "UNRESOLVED"))
    require(baseline_rc == 0, "normal control has KPI failures beyond the exact seven unresolved imports")
    require(candidate_rc == 0, "NEGATIVE: candidate strict KPI guard rejected; no driver validation")
    print("NAME MATCH ONLY: exactly seven replacements resolve to declared current providers.")
    print("No ABI/vtable, KC creation, installation, loading or hardware validation was performed.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    patch_cmd = sub.add_parser("patch")
    patch_cmd.add_argument("source", type=Path)
    patch_cmd.add_argument("logs", type=Path)
    check_cmd = sub.add_parser("check")
    check_cmd.add_argument("baseline", type=Path)
    check_cmd.add_argument("candidate", type=Path)
    check_cmd.add_argument("logs", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "patch":
            patch(args.source, args.logs)
        else:
            check(args.baseline, args.candidate, args.logs)
    except (OSError, ValueError, KeyError, kpi.GuardError) as error:
        print("macos15_accel_symbol_probe: FAIL: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
