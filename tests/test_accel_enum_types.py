#!/usr/bin/env python3
"""Compile the production family enum declarations with only platform includes stubbed."""
import os
import pathlib
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
types = ("IOAccelStereoMode", "eDoSwap", "eIOAccelSurfaceShapeBits", "eLockType")
cpp_source = '#include <type_traits>\n#include "accel/iofam/iofam_types.h"\n'
for name in types:
    cpp_source += f'''
static_assert(std::is_enum_v<{name}>, "{name} must remain an enum");
static_assert(std::is_same_v<std::underlying_type_t<{name}>, unsigned int>,
              "{name} must keep its unsigned int underlying type");
static_assert(sizeof({name}) == sizeof(unsigned int), "{name} size changed");
static_assert(alignof({name}) == alignof(unsigned int), "{name} alignment changed");
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="rdna3-accel-enum-types-") as directory:
        path = pathlib.Path(directory)
        for header in ("libkern/c++/OSObject.h", "IOKit/IOService.h"):
            stub = path / header
            stub.parent.mkdir(parents=True, exist_ok=True)
            stub.write_text("// Platform include unused by iofam_types.h.\n", encoding="utf-8")
        cpp = path / "test.cpp"
        cpp.write_text(cpp_source, encoding="utf-8")
        subprocess.run(shlex.split(os.environ.get("CXX", "clang++")) +
                       ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                        "-I" + str(path), "-I" + str(root), str(cpp)], check=True)
    print("test_accel_enum_types: ok")
