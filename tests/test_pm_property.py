#!/usr/bin/env python3
"""Execute the production power-property publisher with an in-memory owner."""
import os
import pathlib
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "kext/src/RDNA3Bringup.cpp").read_text(encoding="utf-8")
start = source.index("void RDNA3Bringup::publishPowerLimit(")
end = source.index("void RDNA3Bringup::publishString(", start)
body = source[start:end]
harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <map>
#include <string>
struct Owner {
    std::map<std::string, uint64_t> values;
    void removeProperty(const char *key) { values.erase(key); }
};
struct RDNA3Bringup {
    Owner *fOwner;
    void publish(const char *key, uint64_t value) { fOwner->values[key] = value; }
    void publishPowerLimit(const char *key, uint32_t value);
};
'''
main = r'''
int main() {
    Owner owner;
    RDNA3Bringup bringup{&owner};
    const char *key = "RDNA3,PMPowerLimitW";
    const char *available = "RDNA3,PMPowerLimitWAvailable";
    bringup.publishPowerLimit(key, 200);
    assert(owner.values.at(key) == 200 && owner.values.at(available) == 1);
    bringup.publishPowerLimit(key, UINT32_MAX);
    assert(owner.values.count(key) == 0 && owner.values.at(available) == 0);
    bringup.publishPowerLimit(key, UINT32_MAX);
    assert(owner.values.count(key) == 0 && owner.values.at(available) == 0);
    bringup.publishPowerLimit(key, 0);
    assert(owner.values.at(key) == 0 && owner.values.at(available) == 1);
    bringup.publishPowerLimit(key, 150);
    assert(owner.values.at(key) == 150 && owner.values.at(available) == 1);
}
'''
with tempfile.TemporaryDirectory(prefix="rdna3-pm-property-") as directory:
    path = pathlib.Path(directory)
    cpp, binary = path / "test.cpp", path / "test"
    cpp.write_text(harness + body + main, encoding="utf-8")
    subprocess.run(shlex.split(os.environ.get("CXX", "clang++")) +
                   ["-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("test_pm_property: ok")
