#!/usr/bin/env python3
"""
vmtool.py - helpers for tools/vm/rdna3vm (Linux host, stdlib only).

  vmtool.py plist CONFIG [--show] [--boot-args S] [--sip-off] [--timeout N]
      edit an OpenCore config.plist: boot-args and csr-active-config under
      NVRAM/Add (and listed under NVRAM/Delete so OpenCore really applies
      them), SecureBootModel Disabled, picker timeout
  vmtool.py romcheck FILE        a PCI option ROM with a UEFI (GOP) image?
  vmtool.py unzip ZIP DIR        extract (keeps the tree)
  vmtool.py serve DIR PORT       HTTP on 127.0.0.1: GET files, PUT into reports/
  vmtool.py powerkey             return once the PC's power button is pressed
  vmtool.py usbhid               USB keyboards / mice: "vendor product name"
"""
import http.server
import os
import plistlib
import select
import struct
import sys
import zipfile

APPLE_NVRAM = "7C436110-AB2A-4BBB-A880-FE41995C9F82"


def cmd_plist(argv):
    path = argv[0]
    with open(path, "rb") as f:
        cfg = plistlib.load(f)
    add = cfg.setdefault("NVRAM", {}).setdefault("Add", {}).setdefault(APPLE_NVRAM, {})
    if "--show" in argv:
        print(add.get("boot-args", ""))
        return 0
    delete = cfg["NVRAM"].setdefault("Delete", {}).setdefault(APPLE_NVRAM, [])
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--boot-args":
            add["boot-args"] = argv[i + 1]
            if "boot-args" not in delete:
                delete.append("boot-args")
            i += 2
        elif a == "--sip-off":
            # 0x0FEF: SIP off, unsigned and unapproved kexts allowed (test VM only)
            add["csr-active-config"] = bytes([0xEF, 0x0F, 0x00, 0x00])
            if "csr-active-config" not in delete:
                delete.append("csr-active-config")
            cfg.setdefault("Misc", {}).setdefault("Security", {})["SecureBootModel"] = "Disabled"
            i += 1
        elif a == "--timeout":
            cfg.setdefault("Misc", {}).setdefault("Boot", {})["Timeout"] = int(argv[i + 1])
            i += 2
        else:
            sys.exit("unknown option " + a)
    with open(path, "wb") as f:
        plistlib.dump(cfg, f)
    print("boot-args:", add.get("boot-args", ""))
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
        os.makedirs("reports", exist_ok=True)
        with open(os.path.join("reports", name), "wb") as f:
            while n > 0:
                chunk = self.rfile.read(min(n, 1 << 20))
                if not chunk:
                    break
                f.write(chunk)
                n -= len(chunk)
        print("received reports/" + name, flush=True)
        self.send_response(201)
        self.end_headers()


def cmd_serve(argv):
    os.chdir(argv[0])
    # the VM reaches the host's loopback as 10.0.2.2 (QEMU user networking)
    http.server.ThreadingHTTPServer(("127.0.0.1", int(argv[1])), Share).serve_forever()


def cmd_powerkey(argv):
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
    if not fds:
        print("powerkey: no power button input device; stop the VM another way", flush=True)
        select.select([], [], [])          # never returns
    fmt = "llHHi"                          # struct input_event on 64-bit Linux
    size = struct.calcsize(fmt)
    while True:
        ready, _, _ = select.select(fds, [], [])
        for fd in ready:
            data = os.read(fd, size * 64)
            for o in range(0, len(data) - size + 1, size):
                _, _, typ, code, val = struct.unpack_from(fmt, data, o)
                if typ == 1 and code == 116 and val == 1:   # EV_KEY KEY_POWER press
                    print("powerkey: pressed", flush=True)
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


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmds = {"plist": cmd_plist, "romcheck": cmd_romcheck, "unzip": cmd_unzip,
            "serve": cmd_serve, "powerkey": cmd_powerkey, "usbhid": cmd_usbhid}
    fn = cmds.get(sys.argv[1])
    if not fn:
        sys.exit(__doc__)
    sys.exit(fn(sys.argv[2:]))


if __name__ == "__main__":
    main()
