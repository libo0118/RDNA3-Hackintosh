#!/usr/bin/env python3
"""tools/baremetal/build_efi.py: the OpenCore config for the owner's B550 / Ryzen 7 5800X PC.
Offline: a minimal Sample.plist built here stands in for OpenCore's, fake kexts for the downloads.
When ~/rdna3-baremetal (a real fetch.sh run) exists, the real Sample.plist is built too and both
configs go through OpenCore's ocvalidate. Run: python3 -I tests/test_baremetal_efi.py"""
import copy
import importlib.util
import json
import os
import plistlib
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("build_efi", os.path.join(ROOT, "tools", "baremetal", "build_efi.py"))
be = importlib.util.module_from_spec(spec)
spec.loader.exec_module(be)
vm = be.load_vmtool()

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)
        print("FAIL", what)


def minimal_sample():
    """the keys build_config touches, in the shape of OpenCore 1.0.8's Sample.plist"""
    entry = {"Arguments": "", "Auxiliary": False, "Comment": "Not signed", "Enabled": False, "Flavour": "Auto",
             "Name": "CustomOS", "Path": "PciRoot(0x0)", "TextMode": False}
    return {
        "ACPI": {"Add": [], "Delete": [], "Patch": [], "Quirks": {}},
        "Booter": {"MmioWhitelist": [], "Patch": [], "Quirks": {"ResizeAppleGpuBars": -1, "SetupVirtualMap": False}},
        "DeviceProperties": {"Add": {}, "Delete": {}},
        "Kernel": {"Add": [{"BundlePath": "Sample.kext"}], "Block": [], "Force": [], "Patch": [{"Comment": "sample"}],
                   "Emulate": {"Cpuid1Data": b"\x01", "Cpuid1Mask": b"\x01", "DummyPowerManagement": False,
                               "MaxKernel": "", "MinKernel": ""},
                   "Quirks": {"ProvideCurrentCpuInfo": False, "XhciPortLimit": True}, "Scheme": {}},
        "Misc": {"Boot": {"ShowPicker": False, "Timeout": 5}, "Debug": {}, "Entries": [entry],
                 "Security": {"ScanPolicy": 17760515, "SecureBootModel": "Default"}, "Tools": [{"Name": "Shell"}]},
        "NVRAM": {"Add": {be.GUID_APPLE_BOOT: {"boot-args": "-v", "csr-active-config": b"\0\0\0\0",
                                                "ForceDisplayRotationInEFI": 0, "SystemAudioVolume": b"F"},
                          be.GUID_APPLE_UI: {}},
                  "Delete": {be.GUID_APPLE_BOOT: ["boot-args", "x"]}, "WriteFlash": False},
        "PlatformInfo": {"Automatic": False, "Generic": {"SystemProductName": "iMac19,1", "MLB": "M0000000000000001",
                                                          "SystemSerialNumber": "W00000000001", "ROM": b"\x11" * 6,
                                                          "SystemUUID": "00000000-0000-0000-0000-000000000000"}},
        "UEFI": {"Drivers": [{"Path": "Everything.efi"}], "Output": {}, "Quirks": {}, "APFS": {}, "ConnectDrivers": False},
    }


IDENT = {"serial": "F5KKMLZDP7QM", "mlb": "F5K3178024NK3F78C", "uuid": "11111111-2222-3333-4444-555555555555",
         "rom": "0a1b2c3d4e5f"}


def fake_kext(root, name, exe=None, bundle_id=None):
    exe = exe or name
    d = os.path.join(root, name + ".kext", "Contents")
    os.makedirs(os.path.join(d, "MacOS"))
    with open(os.path.join(d, "Info.plist"), "wb") as f:
        plistlib.dump({"CFBundleIdentifier": bundle_id or "test." + name, "CFBundleVersion": "1.0",
                       "CFBundleExecutable": exe}, f)
    with open(os.path.join(d, "MacOS", exe), "wb") as f:
        f.write(b"\xcf\xfa\xed\xfe")
    return os.path.join(root, name + ".kext")


def kext_list(tmp):
    kexts = []
    for _, bundle, comment in be.KEXTS:
        kexts.append(be.kext_entry(bundle, fake_kext(tmp, bundle), comment))
    kexts.append(be.kext_entry(be.CORE_KEXT, fake_kext(tmp, be.CORE_KEXT), "core", enabled=False,
                               min_kernel=be.CORE_KERNEL_MIN, max_kernel=be.CORE_KERNEL_MAX))
    return kexts


def cfg(stage, sample, kexts, cores=8, **kw):
    return be.build_config(sample, stage, kexts=[dict(k) for k in kexts], cores=cores, smbios="MacPro7,1",
                           identity=IDENT, alcid=13, vmtool=vm, **kw)


tmp = tempfile.mkdtemp()
sample = minimal_sample()
before = copy.deepcopy(sample)
kexts = kext_list(os.path.join(tmp, "k"))

# ---- patches: the VM's AMD_Vanilla file, the core count byte in exactly the four core patches
patches = be.amd_kernel_patches(8, vm)
check(len(patches) == len(vm.amd_patches()) and len(patches) >= 20, "AMD_Vanilla patch count %d" % len(patches))
cp = [p for p in patches if vm.AMD_CORES_COMMENT in p["Comment"]]
check(len(cp) == 4, "four cpuid_cores_per_package patches, got %d" % len(cp))
check(all(p["Replace"][1] == 8 and p["Replace"][0] in (0xB8, 0xBA) for p in cp), "core count 8 in the Replace byte")
check(all(p["Comment"].startswith("AMD_Vanilla: ") and p["Identifier"] == "kernel" for p in patches),
      "patch comments / identifiers")
# AMD_Vanilla ships two optional MTRR patches disabled; they stay as the file has them
check([p["Enabled"] for p in patches] == [bool(p.get("Enabled")) for p in vm.amd_patches()]
      and sum(1 for p in patches if p["Enabled"]) >= 20, "patch Enabled flags as in the file")
check(all(p["Replace"][1] == 16 for p in be.amd_kernel_patches(16, vm) if vm.AMD_CORES_COMMENT in p["Comment"]),
      "core count 16")
for bad in (0, 256, -1):
    try:
        be.amd_kernel_patches(bad, vm)
        check(False, "core count %d accepted" % bad)
    except be.BuildError:
        pass

# ---- the installer config
c = cfg("installer", sample, kexts)
check(sample == before, "build_config left the Sample untouched")
core = [k for k in c["Kernel"]["Add"] if k["BundlePath"] == "RDNA3Accel.kext"]
check(len(core) == 1 and core[0]["Enabled"] is False, "installer: RDNA3Accel present but disabled")
check(c["Kernel"]["Add"][0]["BundlePath"] == "Lilu.kext" and c["Kernel"]["Add"][-1]["BundlePath"] == "RDNA3Accel.kext",
      "Lilu first, core kext last")
check(core[0]["MinKernel"] == "25.0.0" and core[0]["MaxKernel"] == "25.99.99", "core kext Darwin 25 range")
args = c["NVRAM"]["Add"][be.GUID_APPLE_BOOT]["boot-args"]
check("rdna3" not in args and "amfi" not in args and "alcid=13" in args and "-v" in args.split(), "installer boot-args: " + args)
check(c["NVRAM"]["Add"][be.GUID_APPLE_BOOT]["csr-active-config"] == bytes.fromhex("EF0F0000"), "csr-active-config")
check("boot-args" in c["NVRAM"]["Delete"][be.GUID_APPLE_BOOT] and "csr-active-config" in c["NVRAM"]["Delete"][be.GUID_APPLE_BOOT],
      "NVRAM Delete lists boot-args and csr-active-config")
q = c["Kernel"]["Quirks"]
check(q["ProvideCurrentCpuInfo"] and q["DisableIoMapper"] and not q["XhciPortLimit"] and q["PanicNoKextDump"], "kernel quirks")
e = c["Kernel"]["Emulate"]
check(e["Cpuid1Data"] == b"" and e["Cpuid1Mask"] == b"" and e["DummyPowerManagement"], "AMD: no CPUID spoof, dummy PM")
b = c["Booter"]["Quirks"]
check(b["RebuildAppleMemoryMap"] and b["SyncRuntimePermissions"] and b["SetupVirtualMap"] and not b["EnableWriteUnprotector"]
      and b["ResizeAppleGpuBars"] == 10, "booter quirks (Ryzen)")
check(cfg("installer", sample, kexts, resize_bars=-1)["Booter"]["Quirks"]["ResizeAppleGpuBars"] == -1, "resize_bars=-1")
s = c["Misc"]["Security"]
check(s["SecureBootModel"] == "Disabled" and s["ScanPolicy"] == 0 and s["AllowSetDefault"], "security")
check(c["Misc"]["Boot"]["ShowPicker"] and c["Misc"]["Boot"]["Timeout"] == 0 and c["Misc"]["Tools"] == [], "picker visible, no timeout")
g = c["PlatformInfo"]["Generic"]
check(g["SystemProductName"] == "MacPro7,1" and g["SystemSerialNumber"] == IDENT["serial"] and g["MLB"] == IDENT["mlb"]
      and g["ROM"] == bytes.fromhex(IDENT["rom"]) and c["PlatformInfo"]["Automatic"], "SMBIOS")
check([a["Path"] for a in c["ACPI"]["Add"]] == list(be.SSDTS) and all(a["Enabled"] for a in c["ACPI"]["Add"]), "SSDTs")
check([d["Path"] for d in c["UEFI"]["Drivers"]] == list(be.DRIVERS), "drivers")
check(c["UEFI"]["Quirks"]["ReleaseUsbOwnership"], "ReleaseUsbOwnership")

# ---- the driver config: only the kext switch, the boot-args and the timeout differ
d = cfg("driver", sample, kexts)
dargs = d["NVRAM"]["Add"][be.GUID_APPLE_BOOT]["boot-args"]
check(d["Kernel"]["Add"][-1]["Enabled"] is True, "driver: RDNA3Accel enabled")
for tok in ("amfi=0x80", "rdna3agdc=3", "alcid=13", "keepsyms=1"):
    check(tok in dargs.split(), "driver boot-args has %s: %s" % (tok, dargs))
for tok in ("serial=3", "gemini_check=SUCCESS", "-rdna3off", "rdna3probe=1"):
    check(tok not in dargs.split(), "driver boot-args has no VM/test token %s" % tok)
d2 = copy.deepcopy(d)
c2 = copy.deepcopy(c)
for x in (d2, c2):
    x["Kernel"]["Add"][-1]["Enabled"] = None
    x["NVRAM"]["Add"][be.GUID_APPLE_BOOT]["boot-args"] = None
    x["Misc"]["Boot"]["Timeout"] = None
check(d2 == c2, "installer and driver configs differ only in the kext switch, boot-args and timeout")
try:
    cfg("bogus", sample, kexts)
    check(False, "bad stage accepted")
except be.BuildError:
    pass

# ---- picker entries (efibootmgr)
EFIBOOTMGR = (
    "BootCurrent: 0001\nBootOrder: 0001,0000\n"
    "Boot0000* Windows Boot Manager\tHD(1,GPT,a46e195a-7c06-4ae7-a77b-7e2092cc3306,0x800,0x200000)/\\EFI\\MICROSOFT\\BOOT\\BOOTMGFW.EFI57494e444f5753\n"
    "      dp: 04 01 2a 00\n"
    "Boot0001* GRUB\tHD(1,GPT,a46e195a-7c06-4ae7-a77b-7e2092cc3306,0x800,0x200000)/\\EFI\\GRUB\\GRUBX64.EFI\n"
    "Boot0003* UEFI: SanDisk\tPciRoot(0x0)/Pci(0x8,0x1)/USB(2,0)/HD(1,MBR,0x0,0x800,0x100)/\\EFI\\BOOT\\BOOTx64.efi\n")
found = be.parse_efibootmgr(EFIBOOTMGR)
check(found == [("GRUB", "HD(1,GPT,a46e195a-7c06-4ae7-a77b-7e2092cc3306,0x800,0x200000)/\\EFI\\GRUB\\GRUBX64.EFI")],
      "efibootmgr parse: %r" % (found,))
ce = cfg("installer", sample, kexts, entries=found)
check(len(ce["Misc"]["Entries"]) == 1 and ce["Misc"]["Entries"][0]["Name"] == "GRUB" and ce["Misc"]["Entries"][0]["Enabled"],
      "Misc/Entries from efibootmgr")
check(c["Misc"]["Entries"] == [], "no entries by default")

# ---- identity: macserial's output, reused on the second run, private file
fake_ms = os.path.join(tmp, "macserial")
with open(fake_ms, "w") as f:
    f.write("#!/bin/sh\necho 'Warning: arc4random is not available!'\necho 'F5KKMLZDP7QM | F5K3178024NK3F78C'\n")
os.chmod(fake_ms, 0o755)
ip = os.path.join(tmp, "identity.json")
i1 = be.make_identity(ip, fake_ms, "MacPro7,1")
check(i1["serial"] == "F5KKMLZDP7QM" and i1["mlb"] == "F5K3178024NK3F78C" and len(i1["rom"]) == 12
      and int(i1["rom"][:2], 16) & 1 == 0, "identity from macserial: %r" % i1)
check(stat.S_IMODE(os.stat(ip).st_mode) == 0o600, "identity.json is private (0600)")
check(be.make_identity(ip, "/nonexistent/macserial", "MacPro7,1") == i1, "identity reused, macserial not run again")
with open(fake_ms, "w") as f:
    f.write("#!/bin/sh\necho nothing\n")
try:
    be.make_identity(os.path.join(tmp, "other.json"), fake_ms, "MacPro7,1")
    check(False, "macserial without output accepted")
except be.BuildError:
    pass

# ---- the core kext out of the CI artifact zip
zp = os.path.join(tmp, "art.zip")
with zipfile.ZipFile(zp, "w") as z:
    z.writestr("RDNA3Accel.kext/Contents/Info.plist", plistlib.dumps({"CFBundleExecutable": "RDNA3Accel"}))
    z.writestr("RDNA3Accel.kext/Contents/MacOS/RDNA3Accel", b"\xcf\xfa\xed\xfe")
    z.writestr("RDNA3Framebuffer.kext/Contents/Info.plist", b"x")
dst = os.path.join(tmp, "x1")
os.makedirs(dst)
p = be.extract_core_kext(zp, dst)
check(os.path.isfile(os.path.join(p, "Contents", "MacOS", "RDNA3Accel")) and not os.path.exists(os.path.join(dst, "RDNA3Framebuffer.kext")),
      "only RDNA3Accel.kext extracted")
check(os.access(os.path.join(p, "Contents", "MacOS", "RDNA3Accel"), os.X_OK), "executable bit on the kext binary")
zb = os.path.join(tmp, "bad.zip")
with zipfile.ZipFile(zb, "w") as z:
    z.writestr("RDNA3Accel.kext/../../evil", b"x")
try:
    be.extract_core_kext(zb, os.path.join(tmp, "x2"))
    check(False, "path traversal member accepted")
except be.BuildError:
    pass
zn = os.path.join(tmp, "none.zip")
with zipfile.ZipFile(zn, "w") as z:
    z.writestr("other.kext/Contents/Info.plist", b"x")
try:
    be.extract_core_kext(zn, os.path.join(tmp, "x3"))
    check(False, "zip without RDNA3Accel.kext accepted")
except be.BuildError:
    pass

# ---- the whole build() on a fake fetch.sh tree: every file the configs name exists
out = os.path.join(tmp, "out")
oc_src = os.path.join(out, "src", "OpenCorePkg")
for sub in ("X64/EFI/BOOT", "X64/EFI/OC/Drivers", "Docs", "Utilities/macserial", "Utilities/ocvalidate"):
    os.makedirs(os.path.join(oc_src, sub))
for fn in ("X64/EFI/BOOT/BOOTx64.efi", "X64/EFI/OC/OpenCore.efi"):
    open(os.path.join(oc_src, fn), "wb").write(b"MZ")
for dname in be.DRIVERS:
    open(os.path.join(oc_src, "X64/EFI/OC/Drivers", dname), "wb").write(b"MZ")
with open(os.path.join(oc_src, "Docs/Sample.plist"), "wb") as f:
    plistlib.dump(minimal_sample(), f)
os.symlink(fake_ms, os.path.join(oc_src, "Utilities/macserial/macserial.linux"))
with open(fake_ms, "w") as f:
    f.write("#!/bin/sh\necho 'F5KKMLZDP7QM | F5K3178024NK3F78C'\n")
os.makedirs(os.path.join(out, "dl"))
for a in be.SSDTS:
    open(os.path.join(out, "dl", a), "wb").write(b"SSDT")
for src_dir, bundle, _ in be.KEXTS:
    base = os.path.join(out, "src", src_dir, "Release" if bundle == "RealtekRTL8111" else "Kexts")
    os.makedirs(base)
    fake_kext(base, bundle)
    os.makedirs(os.path.join(out, "src", src_dir, "junk.dSYM"), exist_ok=True)
    os.makedirs(os.path.join(base, bundle + ".kext", "Contents", "Resources"), exist_ok=True)
    open(os.path.join(base, bundle + ".kext", ".DS_Store"), "w").write("x")
rc = be.main(["--out", out, "--kext-zip", zp, "--cores", "8", "--no-validate", "--entry",
              "Linux=HD(1,GPT,00000000-0000-0000-0000-000000000000,0x800,0x200000)/\\EFI\\GRUB\\GRUBX64.EFI"])
check(rc == 0, "build() returned %r" % rc)
ocd = os.path.join(out, "efi", "EFI", "OC")
for stage_file in ("config.plist", "config-installer.plist", "config-driver.plist"):
    with open(os.path.join(ocd, stage_file), "rb") as f:
        built = plistlib.load(f)
    for k in built["Kernel"]["Add"]:
        kp = os.path.join(ocd, "Kexts", k["BundlePath"])
        check(os.path.isfile(os.path.join(kp, k["PlistPath"])) and os.path.isfile(os.path.join(kp, k["ExecutablePath"])),
              "%s: Kernel/Add %s exists" % (stage_file, k["BundlePath"]))
    check(all(os.path.isfile(os.path.join(ocd, "Drivers", x["Path"])) for x in built["UEFI"]["Drivers"]), stage_file + " drivers exist")
    check(all(os.path.isfile(os.path.join(ocd, "ACPI", x["Path"])) for x in built["ACPI"]["Add"]), stage_file + " ACPI exist")
    check(len(built["Kernel"]["Patch"]) == len(vm.amd_patches()), stage_file + " patch count")
    check(built["Misc"]["Entries"] and built["Misc"]["Entries"][0]["Name"] == "Linux", stage_file + " --entry")
check(open(os.path.join(ocd, "config.plist"), "rb").read() == open(os.path.join(ocd, "config-installer.plist"), "rb").read(),
      "config.plist is the installer config")
check(not os.path.exists(os.path.join(ocd, "Kexts", "Lilu.kext", ".DS_Store")), "no .DS_Store copied")
check(os.path.isfile(os.path.join(out, "efi", "EFI", "BOOT", "BOOTx64.efi")), "BOOTx64.efi")
check(json.load(open(os.path.join(out, "identity.json")))["serial"] == "F5KKMLZDP7QM", "identity.json written")
# a second build keeps the identity
ident_before = json.load(open(os.path.join(out, "identity.json")))
rc2 = be.main(["--out", out, "--kext-zip", zp, "--cores", "8", "--no-validate"])
check(rc2 == 0 and json.load(open(os.path.join(out, "identity.json"))) == ident_before, "second build keeps identity.json")
# nothing generated may be in git: the repo has no identity file and no serial of this run
r = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True, text=True)
check("identity.json" not in r.stdout, "identity.json is not tracked")
# missing inputs are loud
rc3 = be.main(["--out", os.path.join(tmp, "nothing"), "--kext-zip", zp])
check(rc3 == 2, "missing fetch.sh output returns 2, got %r" % rc3)
rc4 = be.main(["--out", out, "--kext-zip", os.path.join(tmp, "nonexistent.zip"), "--cores", "8", "--no-validate"])
check(rc4 == 2, "missing kext zip returns 2, got %r" % rc4)

# ---- ocvalidate's output: the one accepted note
check(be.ocvalidate_issues("NOTE: x\n\nBooter->Quirks->ResizeAppleGpuBars is set to 10, bad!\nCheckBooter returns 1 error!\n\n"
                           "Completed validating c in 2 ms. Found 1 issue requiring attention.\n")
      == ["Booter->Quirks->ResizeAppleGpuBars is set to 10, bad!"], "ocvalidate_issues")

# ---- the real thing, when fetch.sh has been run on this machine
real = os.path.expanduser("~/rdna3-baremetal")
tool = os.path.join(real, "src", "OpenCorePkg", "Utilities", "ocvalidate", "ocvalidate.linux")
if os.path.isfile(tool) and os.path.isfile(os.path.join(real, "src", "OpenCorePkg", "Docs", "Sample.plist")):
    with open(os.path.join(real, "src", "OpenCorePkg", "Docs", "Sample.plist"), "rb") as f:
        rs = plistlib.load(f)
    for stage in ("installer", "driver"):
        x = cfg(stage, rs, kexts)
        p = os.path.join(tmp, "real-%s.plist" % stage)
        with open(p, "wb") as f:
            plistlib.dump(x, f)
        ok, text = be.run_ocvalidate(tool, p, allow_bar_note=True)
        check(ok, "ocvalidate %s on the real Sample.plist: %s" % (stage, text))
        # ... and with the quirk off (Resizable BAR off in the BIOS) it needs no exception at all
        with open(p, "wb") as f:
            plistlib.dump(cfg(stage, rs, kexts, resize_bars=-1), f)
        ok, text = be.run_ocvalidate(tool, p)
        check(ok, "ocvalidate %s resize_bars=-1: %s" % (stage, text))
    print("test_baremetal_efi: real Sample.plist + ocvalidate checked")
else:
    print("test_baremetal_efi: no ~/rdna3-baremetal, ocvalidate part skipped")

if fails:
    print("test_baremetal_efi: %d FAIL" % len(fails))
    sys.exit(1)
print("test_baremetal_efi: ok")
