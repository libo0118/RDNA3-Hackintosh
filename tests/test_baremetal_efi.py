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
        "Kernel": {"Add": [{"BundlePath": "Sample.kext"}],
                   "Block": [{"Arch": "Any", "Comment": "", "Enabled": False, "Identifier": "com.apple.driver.AppleTyMCEDriver",
                              "MaxKernel": "", "MinKernel": "", "Strategy": "Disable"}], "Force": [], "Patch": [{"Comment": "sample"}],
                   "Emulate": {"Cpuid1Data": b"\x01", "Cpuid1Mask": b"\x01", "DummyPowerManagement": False,
                               "MaxKernel": "", "MinKernel": ""},
                   "Quirks": {"ProvideCurrentCpuInfo": False, "XhciPortLimit": True}, "Scheme": {}},
        "Misc": {"Boot": {"ShowPicker": False, "Timeout": 5, "PollAppleHotKeys": False, "HideAuxiliary": True},
                 "Debug": {"AppleDebug": False, "ApplePanic": False, "Target": 3}, "BlessOverride": [], "Entries": [entry],
                 "Security": {"ScanPolicy": 17760515, "SecureBootModel": "Default", "DmgLoading": "Signed"}, "Tools": [{"Name": "Shell"}]},
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
        if bundle == "Lilu":
            kexts.append(be.mce_entry("MacPro7,1"))
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
check(len(patches) == len(vm.amd_patches(iopci=True)) and len(patches) >= 20, "AMD_Vanilla patch count %d" % len(patches))
# the VM keeps the kernel-only list; the bare-metal EFI adds IOPCIFamily's (ROOT CAUSE of the B550
# chipset failure 2026-10-29..31: macOS enabled 10-bit tags and USB / LAN / NVMe / SATA behind it died)
check(not any(p["Identifier"] != "kernel" for p in vm.amd_patches()), "vmtool.amd_patches() default is kernel patches only (the VM)")
pci = [p for p in patches if p["Identifier"] == "com.apple.iokit.IOPCIFamily"]
check(len(pci) == 3 and patches[-3:] == pci, "three IOPCIFamily patches, after the kernel ones: %d" % len(pci))
tags = [p for p in pci if "Disable 10 bit tags" in p["Comment"]]
check(len(tags) == 2 and all(p["Enabled"] and p["Base"] == "__ZN11IOPCIBridge13probeBusGatedEP14probeBusParams" for p in tags),
      "both 10-bit tags patches (12.0-15.x and 26.0+) enabled, on probeBusGated")
check(sorted((p["MinKernel"], p["MaxKernel"]) for p in tags) == [("21.0.0", "24.99.99"), ("25.0.0", "25.99.99")],
      "10-bit tags patches cover Darwin 21..24 and 25: %r" % [(p["MinKernel"], p["MaxKernel"]) for p in tags])
hot = [p for p in pci if "IOPCIIsHotplugPort" in p["Comment"]]
check(len(hot) == 1 and hot[0]["Enabled"] is False, "the AM5 IOPCIIsHotplugPort patch is disabled")
check(all(p["Arch"] == "x86_64" and p["Count"] == 1 and p["Find"] and p["Replace"] for p in pci), "IOPCIFamily patches carry their bytes")
cp = [p for p in patches if vm.AMD_CORES_COMMENT in p["Comment"]]
check(len(cp) == 4, "four cpuid_cores_per_package patches, got %d" % len(cp))
check(all(p["Replace"][1] == 8 and p["Replace"][0] in (0xB8, 0xBA) for p in cp), "core count 8 in the Replace byte")
check(all(p["Comment"].startswith("AMD_Vanilla: ") and p["Identifier"] in ("kernel", "com.apple.iokit.IOPCIFamily")
          for p in patches),
      "patch comments / identifiers")
# AMD_Vanilla ships two optional MTRR patches disabled; they stay as the file has them
check([p["Enabled"] for p in patches] == [bool(p.get("Enabled")) for p in vm.amd_patches()] + [True, True, False]
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
check(c["Kernel"]["Add"][1]["BundlePath"] == "RDNA3MCEReporterDisabler.kext" and c["Kernel"]["Add"][1]["Enabled"]
      and c["Kernel"]["Add"][1]["ExecutablePath"] == "" and c["Kernel"]["Add"][1]["PlistPath"] == "Contents/Info.plist",
      "codeless MCE kext right after Lilu, no executable")
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
check(b["RebuildAppleMemoryMap"] and b["SyncRuntimePermissions"] and b["SetupVirtualMap"] is False
      and not b["EnableWriteUnprotector"], "booter quirks (Ryzen, B550 2020+ firmware: SetupVirtualMap off)")
check(b["ResizeAppleGpuBars"] == -1, "ResizeAppleGpuBars -1 by default (the BIOS's 256 MiB BAR)")
check(cfg("installer", sample, kexts, resize_bars=10)["Booter"]["Quirks"]["ResizeAppleGpuBars"] == 10, "resize_bars=10")
blk = c["Kernel"]["Block"]
check(len(blk) == 2 and blk[1]["Identifier"] == "com.apple.driver.AppleAHCIPort" and blk[1]["Enabled"]
      and blk[1]["Strategy"] == "Disable" and blk[1]["MinKernel"] == "20.0.0", "AppleAHCIPort blocked: %r" % (blk,))
check(len(cfg("installer", sample, kexts, block_ahci=False)["Kernel"]["Block"]) == 1, "block_ahci=False leaves the Sample's blocks")
s = c["Misc"]["Security"]
check(s["SecureBootModel"] == "Disabled" and s["ScanPolicy"] == 0 and s["AllowSetDefault"], "security")
check(s["DmgLoading"] == "Any", "DmgLoading Any (the recovery dmg)")
check(c["Misc"]["Boot"]["ShowPicker"] and c["Misc"]["Boot"]["Timeout"] == 0 and c["Misc"]["Tools"] == [], "picker visible, no timeout")
check(c["Misc"]["Boot"]["PollAppleHotKeys"] is True and c["Misc"]["Boot"]["HideAuxiliary"] is False,
      "PollAppleHotKeys on, auxiliary entries (the installer dmg) shown")
check(c["Misc"]["Debug"] == {"AppleDebug": False, "ApplePanic": False, "Target": 3}, "debug off by default")
dbg = cfg("installer", sample, kexts, debug=True)["Misc"]["Debug"]
check(dbg["Target"] == 67 and dbg["AppleDebug"] is True and dbg["ApplePanic"] is True, "debug=True: Target 67, AppleDebug, ApplePanic")
check("msgbuf=1048576" in c["NVRAM"]["Add"][be.GUID_APPLE_BOOT]["boot-args"].split(), "boot-args keep msgbuf=1048576")
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
for tok in ("amfi=0x80", "alcid=13", "keepsyms=1"):
    check(tok in dargs.split(), "driver boot-args has %s: %s" % (tok, dargs))
# rdna3agdc=3 only once the Metal plugin is installed: tools/baremetal/stick/metal.sh adds it
for tok in ("rdna3agdc=3", "serial=3", "gemini_check=SUCCESS", "-rdna3off", "rdna3probe=1"):
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
      "Misc/Entries from --entry")
check(c["Misc"]["Entries"] == [] and c["Misc"]["BlessOverride"] == [], "no entries, no BlessOverride by default")
check(be.bless_path(found[0][1]) == "\\EFI\\GRUB\\GRUBX64.EFI", "bless_path: %r" % be.bless_path(found[0][1]))
check(be.bless_path("PciRoot(0x0)") == "", "bless_path of a path without a loader")
cb = cfg("installer", sample, kexts, bless=["\\EFI\\GRUB\\GRUBX64.EFI"])
check(cb["Misc"]["BlessOverride"] == ["\\EFI\\GRUB\\GRUBX64.EFI"] and cb["Misc"]["Entries"] == [], "BlessOverride, no Misc/Entries")

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
rc = be.main(["--out", out, "--kext-zip", zp, "--cores", "8", "--no-validate", "--bless", "\\EFI\\GRUB\\GRUBX64.EFI",
              "--entry", "Linux=PciRoot(0x0)/Pci(0x1,0x1)/HD(1,GPT,00000000-0000-0000-0000-000000000000,0x800,0x200000)/\\EFI\\X\\X.EFI"])
check(rc == 0, "build() returned %r" % rc)
ocd = os.path.join(out, "efi", "EFI", "OC")
for stage_file in ("config.plist", "config-installer.plist", "config-driver.plist"):
    with open(os.path.join(ocd, stage_file), "rb") as f:
        built = plistlib.load(f)
    for k in built["Kernel"]["Add"]:
        kp = os.path.join(ocd, "Kexts", k["BundlePath"])
        check(os.path.isfile(os.path.join(kp, k["PlistPath"])) and (os.path.isfile(os.path.join(kp, k["ExecutablePath"]))
                                                                    or (k["ExecutablePath"] == "" and "MCE" in k["BundlePath"])),
              "%s: Kernel/Add %s exists" % (stage_file, k["BundlePath"]))
    check(all(os.path.isfile(os.path.join(ocd, "Drivers", x["Path"])) for x in built["UEFI"]["Drivers"]), stage_file + " drivers exist")
    check(all(os.path.isfile(os.path.join(ocd, "ACPI", x["Path"])) for x in built["ACPI"]["Add"]), stage_file + " ACPI exist")
    check(len(built["Kernel"]["Patch"]) == len(vm.amd_patches(iopci=True)), stage_file + " patch count")
    check(built["Misc"]["Entries"] and built["Misc"]["Entries"][0]["Name"] == "Linux", stage_file + " --entry")
    check(built["Misc"]["BlessOverride"] == ["\\EFI\\GRUB\\GRUBX64.EFI"], stage_file + " --bless")
    adds = [k["BundlePath"] for k in built["Kernel"]["Add"]]
    check(adds[:2] == ["Lilu.kext", "RDNA3MCEReporterDisabler.kext"] and adds[-1] == "RDNA3Accel.kext",
          "%s: MCE kext right after Lilu: %r" % (stage_file, adds))
    check(built["Booter"]["Quirks"]["SetupVirtualMap"] is False and built["Booter"]["Quirks"]["ResizeAppleGpuBars"] == -1,
          stage_file + " booter quirks")
    check(any(p["Identifier"] == "com.apple.iokit.IOPCIFamily" and p["Enabled"] for p in built["Kernel"]["Patch"]),
          stage_file + " has an enabled IOPCIFamily patch")
check(open(os.path.join(ocd, "config.plist"), "rb").read() == open(os.path.join(ocd, "config-installer.plist"), "rb").read(),
      "config.plist is the installer config")
check(not os.path.exists(os.path.join(ocd, "Kexts", "Lilu.kext", ".DS_Store")), "no .DS_Store copied")
mce = os.path.join(ocd, "Kexts", "RDNA3MCEReporterDisabler.kext")
check(sorted(os.listdir(mce)) == ["Contents"] and os.listdir(os.path.join(mce, "Contents")) == ["Info.plist"],
      "the MCE kext is only Contents/Info.plist")
with open(os.path.join(mce, "Contents", "Info.plist"), "rb") as f:
    mi = plistlib.load(f)
check(mi["CFBundleIdentifier"] == "org.rdna3.MCEReporterDisabler" and mi["OSBundleRequired"] == "Root"
      and "CFBundleExecutable" not in mi and mi["CFBundlePackageType"] == "KEXT", "MCE Info.plist header")
pers = mi["IOKitPersonalities"]
check(sorted(pers) == ["DisableMCEInterruptController", "DisableMCEReporter"], "two MCE personalities: %r" % sorted(pers))
check(all(x["CFBundleIdentifier"] == "com.apple.driver.AppleIntelMCEReporter" and x["IOClass"] == "IOService"
          and x["IOProbeScore"] == 5000 for x in pers.values()), "MCE personalities impersonate AppleIntelMCEReporter at score 5000")
a, b = pers["DisableMCEInterruptController"], pers["DisableMCEReporter"]
check(a["IOProviderClass"] == "IOPlatformExpertDevice" and a["IOMatchCategory"] == "AppleIntelMCEInterruptController"
      and a["IOPropertyMatch"] == [{"board-id": "Mac-27AD2F918AE68F61"}], "MCE interrupt controller personality")
check(b["IOProviderClass"] == "AppleIntelMCEInterruptNub" and b["IOMatchCategory"] == "AppleIntelMCEReporter"
      and "IOPropertyMatch" not in b, "MCE reporter personality")
try:
    be.mce_board_id("iMac19,1")
    check(False, "MCE board-id for an unknown SMBIOS accepted")
except be.BuildError:
    pass
check(be.main(["--out", out, "--kext-zip", zp, "--cores", "8", "--no-validate", "--bless", "EFI/GRUB/x.efi"]) == 2,
      "a bad --bless is refused")
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
        # the default (ResizeAppleGpuBars -1, MCE kext, BlessOverride, AHCI block) needs no exception at all
        ok, text = be.run_ocvalidate(tool, p)
        check(ok, "ocvalidate %s on the real Sample.plist: %s" % (stage, text))
        # ... with the quirk on (Re-Size BAR on in the BIOS) only its own note is accepted
        with open(p, "wb") as f:
            plistlib.dump(cfg(stage, rs, kexts, resize_bars=10, bless=["\\EFI\\GRUB\\GRUBX64.EFI"], debug=True), f)
        ok, text = be.run_ocvalidate(tool, p, allow_bar_note=True)
        check(ok, "ocvalidate %s resize_bars=10 + debug: %s" % (stage, text))
    print("test_baremetal_efi: real Sample.plist + ocvalidate checked")
else:
    print("test_baremetal_efi: no ~/rdna3-baremetal, ocvalidate part skipped")

if fails:
    print("test_baremetal_efi: %d FAIL" % len(fails))
    sys.exit(1)
print("test_baremetal_efi: ok")
