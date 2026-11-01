#!/usr/bin/env python3
"""
oc_snippet.py - OpenCore Kernel/Add entries for the built RDNA3 kexts, read
from their own Info.plist (bundle id, executable, version, OSBundleLibraries).

  package/oc_snippet.py [--kexts DIR] [--all] [--min 24.0.0] [--max 24.99.99] [--plist]

Prints <dict> entries to paste into config.plist -> Kernel -> Add (after
Lilu and friends, order as printed). By default only the kexts OpenCore
can link: one that links IOGraphicsFamily / IOAcceleratorFamily2 / IOSurface
(RDNA3Framebuffer, RDNA3Accelerator) exists only against Apple's System
kernel collection, so macOS loads it from /Library/Extensions (the
Auxiliary KC) instead; --all lists those too (disabled), the way the VM
harness injects everything. --plist wraps the entries in a whole plist
(Kernel/Add only), which plistlib and ProperTree can open.
Default MinKernel/MaxKernel: macOS 15 Sequoia (Darwin 24), the only
release the kexts are built and tested for (docs/INSTALL-OPENCORE.html).
"""
import os
import plistlib
import sys
from xml.sax.saxutils import escape

# what the core must come before: the framebuffer and the accelerator wait
# for the RDNA3Accel service (waitForMatchingService), no OSBundleLibraries link
ORDER = ["RDNA3Accel", "RDNA3Framebuffer", "RDNA3Accelerator", "RDNA3AGDC"]
SYSTEM_KC_ONLY = ("com.apple.iokit.IOGraphicsFamily", "com.apple.iokit.IOAcceleratorFamily2",
                  "com.apple.iokit.IOSurface", "com.apple.AppleGraphicsDeviceControl")


def load(kdir):
    kexts = []
    for name in sorted(os.listdir(kdir)):
        if not (name.startswith("RDNA3") and name.endswith(".kext")):
            continue
        p = os.path.join(kdir, name, "Contents", "Info.plist")
        if not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            info = plistlib.load(f)
        kexts.append({
            "name": name[:-5],
            "id": info.get("CFBundleIdentifier", "?"),
            "exe": info.get("CFBundleExecutable", name[:-5]),
            "version": info.get("CFBundleVersion", "?"),
            "libs": sorted(info.get("OSBundleLibraries", {})),
        })
    ids = {k["id"] for k in kexts}
    rank = {n: i for i, n in enumerate(ORDER)}
    # a kext linking another of ours goes after it; otherwise ORDER
    kexts.sort(key=lambda k: (sum(1 for l in k["libs"] if l in ids), rank.get(k["name"], 99), k["name"]))
    for k in kexts:
        k["system_kc"] = [l for l in k["libs"] if l in SYSTEM_KC_ONLY]
        k["third_party"] = [l for l in k["libs"] if not l.startswith("com.apple.") and l not in ids]
    return kexts


ENTRY = """\t\t\t<dict>
\t\t\t\t<key>Arch</key>
\t\t\t\t<string>x86_64</string>
\t\t\t\t<key>BundlePath</key>
\t\t\t\t<string>{name}.kext</string>
\t\t\t\t<key>Comment</key>
\t\t\t\t<string>{comment}</string>
\t\t\t\t<key>Enabled</key>
\t\t\t\t<{enabled}/>
\t\t\t\t<key>ExecutablePath</key>
\t\t\t\t<string>Contents/MacOS/{exe}</string>
\t\t\t\t<key>MaxKernel</key>
\t\t\t\t<string>{max}</string>
\t\t\t\t<key>MinKernel</key>
\t\t\t\t<string>{min}</string>
\t\t\t\t<key>PlistPath</key>
\t\t\t\t<string>Contents/Info.plist</string>
\t\t\t</dict>
"""

HEAD = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>Kernel</key>
\t<dict>
\t\t<key>Add</key>
\t\t<array>
"""
TAIL = """\t\t</array>
\t</dict>
</dict>
</plist>
"""


def main(argv):
    kdir, all_, kmin, kmax, whole = "build", False, "24.0.0", "24.99.99", False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--kexts":
            kdir = argv[i + 1]; i += 2
        elif a == "--all":
            all_ = True; i += 1
        elif a == "--min":
            kmin = argv[i + 1]; i += 2
        elif a == "--max":
            kmax = argv[i + 1]; i += 2
        elif a == "--plist":
            whole = True; i += 1
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    kexts = load(kdir)
    if not kexts:
        print("no RDNA3*.kext with a Contents/Info.plist in %s" % kdir, file=sys.stderr)
        return 1
    out = []
    for k in kexts:
        injectable = not k["system_kc"]
        if not injectable and not all_:
            print("skipped %s: links %s, loads from /Library/Extensions (Auxiliary KC)"
                  % (k["name"], ", ".join(k["system_kc"])), file=sys.stderr)
            continue
        if k["third_party"]:
            print("NOTE: %s needs %s before it in Kernel/Add" % (k["name"], ", ".join(k["third_party"])),
                  file=sys.stderr)
        comment = "RDNA3-Hackintosh %s %s" % (k["id"], k["version"])
        if not injectable:
            comment += " (links the System KC: loads from /Library/Extensions)"
        out.append(ENTRY.format(name=escape(k["name"]), exe=escape(k["exe"]), comment=escape(comment),
                                enabled="true" if injectable else "false", min=escape(kmin), max=escape(kmax)))
    sys.stdout.write((HEAD if whole else "") + "".join(out) + (TAIL if whole else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
