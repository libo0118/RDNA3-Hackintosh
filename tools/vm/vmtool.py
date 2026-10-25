#!/usr/bin/env python3
"""
vmtool.py - helpers for tools/vm/rdna3vm (Linux host, stdlib only).

  vmtool.py plist CONFIG [--show] [--show-smbios] [--boot-args S] [--sip-off] [--timeout N]
                  [--kext NAME[:EXEC[:MIN:MAX]]] [--remove-kext NAME] [--resolution WxH|Max]
                  [--smbios MODEL] [--amd-cpu CORES | --no-amd-cpu] [--show-cpu]
      edit an OpenCore config.plist: boot-args and csr-active-config under
      NVRAM/Add (and listed under NVRAM/Delete so OpenCore really applies
      them), SecureBootModel Disabled, picker timeout, a Kernel/Add entry
      for EFI/OC/Kexts/NAME.kext, UEFI/Output/Resolution (the GOP mode
      OpenCore sets before macOS: the boot framebuffer's size);
      --remove-kext deletes the Kernel/Add entries of EFI/OC/Kexts/NAME.kext
      (only those: other entries stay); --smbios sets the model OpenCore gives
      the VM (PlatformInfo/Generic/SystemProductName; the serial, MLB, UUID
      and ROM of that section stay as they are), --show-smbios prints it;
      --amd-cpu CORES puts the AMD_Vanilla kernel patches (tools/vm/data) into
      Kernel/Patch (their Comment starts with "rdna3vm-amd:"), the core count
      CORES into the four cpuid_cores_per_package ones, and sets
      Kernel/Quirks/ProvideCurrentCpuInfo, Kernel/Emulate/DummyPowerManagement
      true and the Cpuid1 spoof empty (what it replaced is kept in a "#" key of
      Kernel); --no-amd-cpu takes exactly those back; --show-cpu prints the state
  vmtool.py amdpatches           the vendored AMD_Vanilla patch file: count, or why it is unusable
  vmtool.py romcheck FILE        a PCI option ROM with a UEFI (GOP) image?
  vmtool.py unzip ZIP DIR        extract (keeps the tree)
  vmtool.py serve DIR PORT [UP]  HTTP on 127.0.0.1: GET files, PUT into UP (DIR/reports)
  vmtool.py stopkeys [KBD...]    return on the power button, or Esc x3 on a keyboard
  vmtool.py evdev                keyboards / mice as evdev nodes ("kbd PATH")
  vmtool.py usbhid               USB keyboards / mice: "vendor product name"
  vmtool.py monitor SOCK CMD     run one QEMU human-monitor command, print its output
"""
import base64
import hashlib
import http.server
import json
import os
import plistlib
import re
import select
import struct
import sys
import zipfile

APPLE_NVRAM = "7C436110-AB2A-4BBB-A880-FE41995C9F82"


class PlistEditError(Exception):
    pass


def _element_end(text, pos, tag):
    """index just past the </tag> closing the <tag> that starts at pos"""
    depth, i = 0, pos
    pat = re.compile(r"<(/?)%s\b[^>]*?(/?)>" % tag)
    while True:
        m = pat.search(text, i)
        if not m:
            raise PlistEditError("unterminated <%s>" % tag)
        if m.group(1):
            depth -= 1
        elif not m.group(2):
            depth += 1
        i = m.end()
        if depth == 0:
            return i


def _value_after_key(text, key, start=0, stop=None):
    """(key_start, value_start, value_end) of <key>key</key><value> in text[start:stop]"""
    stop = len(text) if stop is None else stop
    m = re.compile(r"<key>%s</key>\s*" % re.escape(key)).search(text, start, stop)
    if not m:
        return None
    v = re.compile(r"<(dict|array|string|data|integer|true|false|real|date)\b[^>]*?(/?)>").match(text, m.end())
    if not v:
        raise PlistEditError("key %s has no value" % key)
    if v.group(2) or v.group(1) in ("true", "false"):
        return m.start(), v.start(), v.end()
    if v.group(1) in ("dict", "array"):
        return m.start(), v.start(), _element_end(text, v.start(), v.group(1))
    close = text.index("</%s>" % v.group(1), v.end())
    return m.start(), v.start(), close + len("</%s>" % v.group(1))


def _section(text, *path):
    """(start, end) of the <dict> reached by following the keys in path"""
    start, end = 0, len(text)
    for key in path:
        found = _value_after_key(text, key, start, end)
        if not found or not text.startswith("<dict", found[1]):
            raise PlistEditError("no %s dict in config.plist" % "/".join(path))
        start, end = found[1], found[2]
    return start, end


def _set(text, sec, key, value_xml):
    """replace (or add) key's value inside the dict text[sec[0]:sec[1]]"""
    found = _value_after_key(text, key, sec[0], sec[1])
    if found:
        return text[:found[1]] + value_xml + text[found[2]:]
    close = text.rindex("</dict>", sec[0], sec[1])
    return text[:close] + "\t<key>%s</key>\n\t%s\n" % (key, value_xml) + text[close:]


def _unxml(s):
    return (s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
            .replace("&apos;", "'").replace("&amp;", "&"))


def _xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


KEXT_ENTRY = """<dict>
				<key>Arch</key>
				<string>x86_64</string>
				<key>BundlePath</key>
				<string>%(name)s.kext</string>
				<key>Comment</key>
				<string>RDNA3-Hackintosh (rdna3vm)</string>
				<key>Enabled</key>
				<true/>
				<key>ExecutablePath</key>
				<string>Contents/MacOS/%(exe)s</string>
				<key>MaxKernel</key>
				<string>%(maxk)s</string>
				<key>MinKernel</key>
				<string>%(mink)s</string>
				<key>PlistPath</key>
				<string>Contents/Info.plist</string>
			</dict>"""


def add_kext(text, spec):
    """Kernel/Add entry injecting EFI/OC/Kexts/NAME.kext; spec is
    NAME[:EXEC[:MIN:MAX]] (the executable when it is not NAME, the kernel
    range: one build per macOS). An entry already there keeps its place and
    gets the executable and the range"""
    parts = spec.split(":")
    name = parts[0]
    v = {"name": name, "exe": parts[1] if len(parts) > 1 and parts[1] else name,
         "mink": parts[2] if len(parts) > 2 else "", "maxk": parts[3] if len(parts) > 3 else ""}
    sec = _section(text, "Kernel")
    found = _value_after_key(text, "Add", *sec)
    if not found or not text.startswith("<array", found[1]):
        raise PlistEditError("no Kernel/Add array in config.plist")
    arr = text[found[1]:found[2]]
    at = arr.find("<string>%s.kext</string>" % name)
    if at >= 0:
        start = found[1] + arr.rfind("<dict>", 0, at)
        end = found[1] + arr.find("</dict>", at) + len("</dict>")
        entry = text[start:end]
        for key, val in (("ExecutablePath", "Contents/MacOS/" + v["exe"]), ("MinKernel", v["mink"]),
                         ("MaxKernel", v["maxk"])):
            entry = re.sub(r"(<key>%s</key>\s*)<string>[^<]*</string>" % key,
                           lambda m: m.group(1) + "<string>%s</string>" % val, entry)
        return text[:start] + entry + text[end:]
    if arr.endswith("/>"):                 # <array/>
        entry = "<array>\n\t\t\t%s\n\t\t</array>" % (KEXT_ENTRY % v)
        return text[:found[1]] + entry + text[found[2]:]
    close = found[1] + arr.rindex("</array>")
    return text[:close] + "\t" + KEXT_ENTRY % v + "\n\t\t" + text[close:]


def remove_kext(text, name):
    """(text without the Kernel/Add entries whose BundlePath is NAME.kext,
    how many were removed); the entries are flat <dict>s, other entries and
    a missing Kernel/Add array stay untouched"""
    found = _value_after_key(text, "Add", *_section(text, "Kernel"))
    if not found or not text.startswith("<array", found[1]):
        return text, 0
    entry = re.compile(r"[ \t]*<dict>(?:(?!</dict>).)*?<key>BundlePath</key>\s*<string>%s\.kext</string>"
                       r".*?</dict>[ \t]*\n?" % re.escape(_xml(name)), re.S)
    arr, n = entry.subn("", text[found[1]:found[2]])
    return text[:found[1]] + arr + text[found[2]:], n


SMBIOS_RE = re.compile(r"^[A-Za-z]+[0-9]+,[0-9]+$")


def get_smbios(text):
    """PlatformInfo/Generic/SystemProductName, or '' when the config has none"""
    sec = _section(text, "PlatformInfo", "Generic")
    found = _value_after_key(text, "SystemProductName", *sec)
    m = re.match(r"<string>(.*?)</string>", text[found[1]:found[2]], re.S) if found else None
    return _unxml(m.group(1)) if m else ""


def set_smbios(text, model):
    """text with PlatformInfo/Generic/SystemProductName = model. OpenCore uses
    that section only with PlatformInfo/Automatic true: with false the model
    would silently stay the one of PlatformInfo/SMBIOS, so that is an error"""
    if not SMBIOS_RE.match(model):
        raise PlistEditError("SMBIOS model %r is not like MacPro7,1" % model)
    auto = _value_after_key(text, "Automatic", *_section(text, "PlatformInfo"))
    if auto and text.startswith("<false", auto[1]):
        raise PlistEditError("PlatformInfo/Automatic is false in config.plist: OpenCore would ignore "
                             "PlatformInfo/Generic/SystemProductName")
    return _set(text, _section(text, "PlatformInfo", "Generic"), "SystemProductName",
                "<string>%s</string>" % _xml(model))


# ---------------------------------------------------------------- AMD CPU (-cpu host)
# The card VM can run with QEMU's `-cpu host` on an AMD host (CPU_MODEL=host in
# ~/rdna3-vm/config). macOS then needs the AMD_Vanilla kernel patches in OpenCore,
# exactly as on a bare-metal Ryzen hackintosh. rdna3vm calls `plist --amd-cpu N`
# before every boot with CPU_MODEL=host and `--no-amd-cpu` otherwise, so switching
# CPU_MODEL back to the Skylake model needs no manual edit of the OpenCore image.
AMD_PATCH_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "amd_vanilla-a174cca.plist")
AMD_PATCH_SHA256 = "dc147342ba068f8221783b43ca5d233587167e4907ae523d83146e5451bf3298"
AMD_MARK = "rdna3vm-amd:"                 # Comment prefix of every entry this tool adds
AMD_RESTORE_KEY = "#rdna3vm-amd-restore"  # OpenCore ignores keys that start with #
AMD_CORES_COMMENT = "Force cpuid_cores_per_package"
# what the AMD mode needs and what it replaces (xml of the value, as OpenCore reads it)
AMD_EMULATE = (("Cpuid1Data", "<data></data>"), ("Cpuid1Mask", "<data></data>"),
               ("DummyPowerManagement", "<true/>"))
AMD_QUIRKS = (("ProvideCurrentCpuInfo", "<true/>"),)
PATCH_FIELDS = ("Arch", "Base", "Comment", "Count", "Enabled", "Find", "Identifier", "Limit", "Mask",
                "MaxKernel", "MinKernel", "Replace", "ReplaceMask", "Skip")


def amd_patches(path=None):
    """the Kernel/Patch entries of the vendored AMD_Vanilla file that patch the kernel
    itself (Identifier "kernel"; the IOPCIFamily ones are for Zen 4 boards)"""
    path = path or AMD_PATCH_FILE
    with open(path, "rb") as f:
        raw = f.read()
    if path == AMD_PATCH_FILE and hashlib.sha256(raw).hexdigest() != AMD_PATCH_SHA256:
        raise PlistEditError("%s was edited (sha256 differs from tools/vm/data/amd_vanilla.SOURCE)" % path)
    patches = plistlib.loads(raw)["Kernel"]["Patch"]
    return [p for p in patches if p.get("Identifier") == "kernel"]


def _patch_xml(p, cores):
    """one Kernel/Patch dict as the text OpenCore's own plists use (tabs, three deep)"""
    p = dict(p)
    p["Comment"] = "%s %s" % (AMD_MARK, p.get("Comment", ""))
    if AMD_CORES_COMMENT in p["Comment"]:
        rep = bytearray(p["Replace"])
        # mov eax|edx, imm32: the byte after the opcode is the core count (README of
        # AMD_Vanilla: B8|BA <core count> 0000 0000 ...)
        if len(rep) < 2 or rep[0] not in (0xB8, 0xBA) or rep[1] != 0:
            raise PlistEditError("AMD_Vanilla core count patch has an unexpected Replace %s" % bytes(rep).hex())
        rep[1] = cores
        p["Replace"] = bytes(rep)
    out = ["\t\t\t<dict>"]
    for k in PATCH_FIELDS:
        v = p.get(k)
        out.append("\t\t\t\t<key>%s</key>" % k)
        if k in ("Count", "Limit", "Skip"):
            out.append("\t\t\t\t<integer>%d</integer>" % int(v or 0))
        elif k == "Enabled":
            out.append("\t\t\t\t<%s/>" % ("true" if v else "false"))
        elif k in ("Find", "Mask", "Replace", "ReplaceMask"):
            out.append("\t\t\t\t<data>%s</data>" % base64.b64encode(v or b"").decode())
        else:
            out.append("\t\t\t\t<string>%s</string>" % _xml(str(v or "")))
    out.append("\t\t\t</dict>")
    return "\n".join(out)


_AMD_ENTRY_RE = re.compile(r"[ \t]*<dict>(?:(?!</dict>).)*?<key>Comment</key>\s*<string>%s[^<]*</string>"
                           r".*?</dict>[ \t]*\n?" % re.escape(AMD_MARK), re.S)


def _patch_array(text):
    found = _value_after_key(text, "Patch", *_section(text, "Kernel"))
    if not found or not text.startswith("<array", found[1]):
        raise PlistEditError("no Kernel/Patch array in config.plist")
    return found


def _remove_amd_patches(text):
    """(text without the entries this tool added, how many)"""
    found = _patch_array(text)
    arr, n = _AMD_ENTRY_RE.subn("", text[found[1]:found[2]])
    return text[:found[1]] + arr + text[found[2]:], n


def _add_amd_patches(text, cores):
    """(text with the AMD_Vanilla kernel patches appended to Kernel/Patch, how many)"""
    patches = amd_patches()
    block = "\n".join(_patch_xml(p, cores) for p in patches)
    found = _patch_array(text)
    arr = text[found[1]:found[2]]
    if arr.endswith("/>"):                  # <array/>
        return text[:found[1]] + "<array>\n%s\n\t\t</array>" % block + text[found[2]:], len(patches)
    line = close = found[1] + arr.rindex("</array>")
    while line > 0 and text[line - 1] in " \t":
        line -= 1
    return text[:line] + block + "\n" + text[line:], len(patches)


def _norm(xml):
    """a value's xml in one comparable form: whitespace-only data is empty data"""
    m = re.match(r"<data>\s*(.*?)\s*</data>$", xml, re.S)
    if m:
        return "<data>%s</data>" % re.sub(r"\s+", "", m.group(1))
    return xml


def _get(text, sec, key):
    found = _value_after_key(text, key, *sec)
    return _norm(text[found[1]:found[2]]) if found else None


def _delete_key(text, sec, key):
    found = _value_after_key(text, key, *sec)
    if not found:
        return text
    start = found[0]
    while start > 0 and text[start - 1] in " \t":
        start -= 1
    end = found[2]
    if text[end:end + 1] == "\n":
        end += 1
    return text[:start] + text[end:]


def _put(text, sec, key, xml):
    """key = xml in the dict text[sec[0]:sec[1]]: replaced, or added on lines of its own with
    the dict's indentation (so that _delete_key takes back exactly the lines added)"""
    found = _value_after_key(text, key, *sec)
    if found:
        return text[:found[1]] + xml + text[found[2]:]
    close = text.rindex("</dict>", sec[0], sec[1])
    line = close
    while line > 0 and text[line - 1] in " \t":
        line -= 1
    ind = text[line:close] + "\t"
    return text[:line] + "%s<key>%s</key>\n%s%s\n" % (ind, key, ind, xml) + text[line:]


def _set_value(text, path, key, xml):
    """key = xml in the dict at path; the text stays byte for byte when it already says that"""
    sec = _section(text, *path)
    if _get(text, sec, key) == _norm(xml):
        return text
    return _put(text, sec, key, xml)


def amd_cpu_state(text):
    """(core count of the AMD patches in the config or None, how many of them)"""
    found = _value_after_key(text, "Patch", *_section(text, "Kernel"))
    if not found or not text.startswith("<array", found[1]):
        return None, 0
    arr = text[found[1]:found[2]]
    n = len(_AMD_ENTRY_RE.findall(arr))
    cores = None
    for m in _AMD_ENTRY_RE.finditer(arr):
        e = m.group(0)
        if AMD_CORES_COMMENT in e:
            rep = re.search(r"<key>Replace</key>\s*<data>([^<]*)</data>", e)
            if rep:
                b = base64.b64decode(rep.group(1))
                cores = b[1] if len(b) > 1 else None
                break
    return cores, n


def set_amd_cpu(text, cores):
    """AMD mode: the patches (the core count in the cpuid_cores_per_package ones), the quirk,
    the Cpuid1 spoof off; what was there before goes into the restore key once"""
    if not isinstance(cores, int) or not 1 <= cores <= 255:
        raise PlistEditError("AMD core count %r is not 1..255" % (cores,))
    kernel = ("Kernel",)
    if AMD_RESTORE_KEY not in text:
        saved = {}
        for path, items in ((("Kernel", "Emulate"), AMD_EMULATE), (("Kernel", "Quirks"), AMD_QUIRKS)):
            sec = _section(text, *path)
            for key, _ in items:
                saved[key] = _get(text, sec, key)
        text = _put(text, _section(text, *kernel), AMD_RESTORE_KEY,
                    "<string>%s</string>" % _xml(json.dumps(saved, sort_keys=True)))
    for path, items in ((("Kernel", "Emulate"), AMD_EMULATE), (("Kernel", "Quirks"), AMD_QUIRKS)):
        for key, xml in items:
            text = _set_value(text, path, key, xml)
    text, _ = _remove_amd_patches(text)
    return _add_amd_patches(text, cores)


def unset_amd_cpu(text):
    """the other CPU modes: the patches this tool added go, the values it replaced come back
    (nothing else changes); (text, patches removed, values restored)"""
    if AMD_MARK not in text and AMD_RESTORE_KEY not in text:
        return text, 0, 0                   # never in AMD mode: nothing to take back (any config)
    text, removed = _remove_amd_patches(text)
    restored = 0
    found = _value_after_key(text, AMD_RESTORE_KEY, *_section(text, "Kernel"))
    if found:
        m = re.match(r"<string>(.*?)</string>", text[found[1]:found[2]], re.S)
        try:
            saved = json.loads(_unxml(m.group(1))) if m else {}
        except ValueError as e:
            raise PlistEditError("%s in config.plist is not valid: %s" % (AMD_RESTORE_KEY, e))
        for path, items in ((("Kernel", "Emulate"), AMD_EMULATE), (("Kernel", "Quirks"), AMD_QUIRKS)):
            for key, _ in items:
                if key not in saved:
                    continue
                if saved[key] is None:
                    text = _delete_key(text, _section(text, *path), key)
                else:
                    text = _set_value(text, path, key, saved[key])
                restored += 1
        text = _delete_key(text, _section(text, "Kernel"), AMD_RESTORE_KEY)
    return text, removed, restored


def cmd_plist(argv):
    """Edits only the values it changes, as text: OpenCore accepts plists
    that Python's plistlib refuses (and a rewrite would reformat the file)."""
    path = argv[0]
    text = open(path, encoding="utf-8").read()
    add = lambda: _section(text, "NVRAM", "Add", APPLE_NVRAM)
    if "--show" in argv:
        found = _value_after_key(text, "boot-args", *add())
        m = re.match(r"<string>(.*?)</string>", text[found[1]:found[2]], re.S) if found else None
        # the file holds XML text: a value with & comes back as the user typed it,
        # so writing it again (which escapes) round-trips byte for byte
        print(_unxml(m.group(1)) if m else "")
        return 0
    if "--show-smbios" in argv:
        print(get_smbios(text))
        return 0
    if "--show-cpu" in argv:
        cores, n = amd_cpu_state(text)
        print("amd-cpu: %s" % ("off" if not n else "on, %d patches, core count %s" % (n, cores)))
        return 0

    def delete_list(name):
        nonlocal text
        sec = _section(text, "NVRAM", "Delete")
        found = _value_after_key(text, APPLE_NVRAM, *sec)
        if not found:
            text = _set(text, sec, APPLE_NVRAM, "<array>\n\t</array>")
            found = _value_after_key(text, APPLE_NVRAM, *_section(text, "NVRAM", "Delete"))
        arr = text[found[1]:found[2]]
        if "<string>%s</string>" % name not in arr:
            close = found[1] + arr.rindex("</array>")
            text = text[:close] + "\t<string>%s</string>\n\t\t\t" % name + text[close:]

    amd_note = ""
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--boot-args":
            text = _set(text, add(), "boot-args", "<string>%s</string>" % _xml(argv[i + 1]))
            delete_list("boot-args")
            i += 2
        elif a == "--sip-off":
            # 0x0FEF: SIP off, unsigned and unapproved kexts allowed (test VM only)
            text = _set(text, add(), "csr-active-config", "<data>7w8AAA==</data>")
            delete_list("csr-active-config")
            text = _set(text, _section(text, "Misc", "Security"), "SecureBootModel",
                        "<string>Disabled</string>")
            i += 1
        elif a == "--timeout":
            text = _set(text, _section(text, "Misc", "Boot"), "Timeout",
                        "<integer>%d</integer>" % int(argv[i + 1]))
            i += 2
        elif a == "--kext":
            text = add_kext(text, argv[i + 1])
            i += 2
        elif a == "--remove-kext":
            text, n = remove_kext(text, argv[i + 1])
            print("Kernel/Add: %s.kext %s" % (argv[i + 1], "removed" if n else "not listed"))
            i += 2
        elif a == "--resolution":
            text = _set(text, _section(text, "UEFI", "Output"), "Resolution",
                        "<string>%s</string>" % _xml(argv[i + 1]))
            i += 2
        elif a == "--smbios":
            text = set_smbios(text, argv[i + 1])
            i += 2
        elif a == "--amd-cpu":
            try:
                n = int(argv[i + 1])
            except (IndexError, ValueError):
                raise PlistEditError("--amd-cpu wants the core count (1..255)")
            text, count = set_amd_cpu(text, n)
            amd_note = "amd-cpu: on, %d AMD_Vanilla kernel patches, core count %d (0x%02X)" % (count, n, n)
            i += 2
        elif a == "--no-amd-cpu":
            text, removed, restored = unset_amd_cpu(text)
            amd_note = "amd-cpu: off (%d AMD_Vanilla patches removed, %d values restored)" % (removed, restored)
            i += 1
        else:
            sys.exit("unknown option " + a)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    found = _value_after_key(text, "boot-args", *add())
    print("boot-args:", _unxml(re.sub(r"</?string>", "", text[found[1]:found[2]])) if found else "")
    if "--smbios" in argv:
        print("smbios:", get_smbios(text))
    if amd_note:
        print(amd_note)
    return 0


def cmd_amdpatches(argv):
    """amdpatches: how many AMD_Vanilla kernel patches the vendored file gives (or why not)"""
    try:
        n = len(amd_patches())
    except (OSError, ValueError, KeyError, PlistEditError) as e:
        print("%s: %s" % (AMD_PATCH_FILE, e))
        return 1
    print("%d AMD_Vanilla kernel patches (%s, sha256 ok)" % (n, os.path.basename(AMD_PATCH_FILE)))
    return 0


def cmd_romcheck(argv):
    data = open(argv[0], "rb").read()
    off, efi = 0, False
    while off + 0x1A <= len(data) and data[off:off + 2] == b"\x55\xaa":
        pcir = off + struct.unpack_from("<H", data, off + 0x18)[0]
        if data[pcir:pcir + 4] != b"PCIR":
            break
        length = struct.unpack_from("<H", data, pcir + 0x10)[0] * 512
        if data[pcir + 0x14] == 3:
            efi = True
        if data[pcir + 0x15] & 0x80 or not length:
            break
        off += length
    if off == 0 and data[:2] != b"\x55\xaa":
        print("not a PCI option ROM")
        return 1
    print("option ROM, %d bytes, UEFI image: %s" % (len(data), "yes" if efi else "no"))
    return 0 if efi else 1


def cmd_unzip(argv):
    with zipfile.ZipFile(argv[0]) as z:
        z.extractall(argv[1])
    return 0


class Share(http.server.SimpleHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path.split("?")[0])
        if not self.path.startswith("/reports/") or not name or name.startswith("."):
            self.send_error(403)
            return
        n = int(self.headers.get("Content-Length", "0"))
        os.makedirs(UPLOADS, exist_ok=True)
        with open(os.path.join(UPLOADS, name), "wb") as f:
            while n > 0:
                chunk = self.rfile.read(min(n, 1 << 20))
                if not chunk:
                    break
                f.write(chunk)
                n -= len(chunk)
        print("received " + os.path.join(UPLOADS, name), flush=True)
        self.send_response(201)
        self.end_headers()


UPLOADS = "reports"


def cmd_serve(argv):
    """serve DIR PORT [UPLOAD_DIR]: GET from DIR, PUT /reports/<name> into
    UPLOAD_DIR (default DIR/reports)"""
    global UPLOADS
    if len(argv) > 2:
        UPLOADS = os.path.abspath(argv[2])
    os.chdir(argv[0])
    # the VM reaches the host's loopback as 10.0.2.2 (QEMU user networking)
    http.server.ThreadingHTTPServer(("127.0.0.1", int(argv[1])), Share).serve_forever()


def power_button_fds():
    fds = []
    try:
        blocks = open("/proc/bus/input/devices").read().split("\n\n")
    except OSError:
        blocks = []
    for b in blocks:
        if 'Name="Power Button"' not in b:
            continue
        for line in b.splitlines():
            if line.startswith("H: Handlers="):
                for h in line.split("=", 1)[1].split():
                    if h.startswith("event"):
                        try:
                            fds.append(os.open("/dev/input/" + h, os.O_RDONLY))
                        except OSError:
                            pass
    return fds


def cmd_stopkeys(argv):
    """Return when the VM must stop: the PC's power button, or Esc pressed
    3 times within 3 s on one of the keyboards in argv. QEMU's input-linux
    grabs those keyboards while the guest has them, so the host sees keys
    only after the user released the grab (both Ctrl keys together)."""
    power = power_button_fds()
    kbds = []
    for p in argv:
        try:
            kbds.append(os.open(p, os.O_RDONLY))
        except OSError:
            pass
    if not power:
        print("stopkeys: no power button input device", flush=True)
    if not power and not kbds:
        select.select([], [], [])          # nothing to watch: never returns
    fmt = "llHHi"                          # struct input_event on 64-bit Linux
    size = struct.calcsize(fmt)
    esc = []
    while True:
        ready, _, _ = select.select(power + kbds, [], [])
        for fd in ready:
            data = os.read(fd, size * 64)
            for o in range(0, len(data) - size + 1, size):
                sec, usec, typ, code, val = struct.unpack_from(fmt, data, o)
                if typ != 1 or val != 1:   # EV_KEY presses only
                    continue
                if fd in power and code == 116:          # KEY_POWER
                    print("stopkeys: power button", flush=True)
                    return 0
                if fd in kbds and code == 1:             # KEY_ESC
                    t = sec + usec / 1e6
                    esc = [x for x in esc if t - x < 3.0] + [t]
                    if len(esc) >= 3:
                        print("stopkeys: Esc x3", flush=True)
                        return 0


def cmd_evdev(argv):
    """keyboards and mice as /dev/input/by-id event nodes: 'kbd PATH' / 'mouse PATH'"""
    for d in ("/dev/input/by-id", "/dev/input/by-path"):
        if not os.path.isdir(d):
            continue
        found = False
        for n in sorted(os.listdir(d)):
            kind = "kbd" if n.endswith("-event-kbd") else "mouse" if n.endswith("-event-mouse") else ""
            if kind:
                print(kind, os.path.join(d, n))
                found = True
        if found:
            return 0
    return 0


def cmd_usbhid(argv):
    base = "/sys/bus/usb/devices"
    seen = set()
    if not os.path.isdir(base):
        return 0
    for intf in sorted(os.listdir(base)):
        if ":" not in intf:
            continue
        p = os.path.join(base, intf)
        try:
            cls = open(p + "/bInterfaceClass").read().strip()
            proto = open(p + "/bInterfaceProtocol").read().strip()
        except OSError:
            continue
        if cls != "03" or proto not in ("01", "02"):
            continue
        dev = os.path.join(base, intf.split(":")[0])
        try:
            vid = open(dev + "/idVendor").read().strip()
            pid = open(dev + "/idProduct").read().strip()
        except OSError:
            continue
        name = ""
        try:
            name = open(dev + "/product").read().strip()
        except OSError:
            pass
        if (vid, pid) not in seen:
            seen.add((vid, pid))
            print(vid, pid, name or "?")
    return 0


def cmd_monitor(argv):
    import socket
    import time
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(argv[0])
    out = b""

    def drain(until):
        nonlocal out
        end = time.time() + 5
        while until not in out and time.time() < end:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk

    drain(b"(qemu) ")                      # banner
    out = b""
    s.sendall((" ".join(argv[1:]) + "\n").encode())
    drain(b"(qemu) ")
    text = out.decode("utf-8", "replace").replace("\r", "")
    lines = text.split("\n")[1:]           # the echoed command
    print("\n".join(l for l in lines if not l.startswith("(qemu)")).rstrip())
    return 0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmds = {"plist": cmd_plist, "amdpatches": cmd_amdpatches, "romcheck": cmd_romcheck, "unzip": cmd_unzip,
            "serve": cmd_serve, "stopkeys": cmd_stopkeys, "evdev": cmd_evdev,
            "usbhid": cmd_usbhid, "monitor": cmd_monitor}
    fn = cmds.get(sys.argv[1])
    if not fn:
        sys.exit(__doc__)
    try:
        sys.exit(fn(sys.argv[2:]))
    except PlistEditError as e:
        sys.exit("vmtool: %s" % e)


if __name__ == "__main__":
    main()
