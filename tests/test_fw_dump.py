#!/usr/bin/env python3
"""Exercise the real dump CLI's profile and optional-TA handling, without hardware."""
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "build/host/amd_fw_dump"


def header(size, major):
    data = bytearray(size)
    struct.pack_into("<IIHHHHIIII", data, 0, size, 256, major, 0, 0, 0, 1, size - 256, 256, 0)
    return data


def run(directory, *args):
    return subprocess.run([str(TOOL), str(directory), *args], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    directory = pathlib.Path(tmp)
    sos = header(320, 2)
    struct.pack_into("<I", sos, 32, 2)
    struct.pack_into("<IIII", sos, 36, 2, 1, 0, 32)  # SYS_DRV
    struct.pack_into("<IIII", sos, 52, 1, 1, 32, 32)  # SOS
    for args, version in (((), "13_0_10"), (("navi32",), "13_0_10"), (("navi33",), "13_0_7")):
        (directory / ("psp_" + version + "_sos.bin")).write_bytes(sos)
        ta_path = directory / ("psp_" + version + "_ta.bin")
        ta_path.write_bytes(header(256, 2))
        result = run(directory, *args)
        assert f"{ta_path.name}: ok" in result.stdout, result.stdout + result.stderr
        assert "LOAD_IP_FW plan: required firmware missing" in result.stdout, result.stdout
        ta_path.write_bytes(header(256, 3))
        result = run(directory, *args)
        assert result.returncode == 1, result
        assert f"{ta_path.name}: unsupported header version" in result.stdout, result.stdout
        assert "LOAD_IP_FW plan:" not in result.stdout, result.stdout
        damaged = header(256, 2)
        struct.pack_into("<I", damaged, 0, 512)
        ta_path.write_bytes(damaged)
        result = run(directory, *args)
        assert result.returncode == 1 and f"{ta_path.name}: truncated / out of bounds" in result.stdout, result
        ta_path.unlink()
        result = run(directory, *args)
        assert f"optional TA not found: {ta_path.name}" in result.stdout, result.stdout
    assert run(directory, "unknown").returncode == 2

print("test_fw_dump: ok")
