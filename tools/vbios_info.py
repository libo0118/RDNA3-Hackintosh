#!/usr/bin/env python3
"""
vbios_info.py - what an AMD ATOM VBIOS image contains (host side, stdlib only).

  python3 tools/vbios_info.py VBIOS.rom

Prints the ATOM ROM header, every non-empty entry of the command table list
(index, offset, format/content revision, size) and of the data table list,
with the amdgpu names (atomfirmware.h, atom_master_list_of_*_v2_1) for the
indices the driver uses. Same layout as hw/vbios.c and hw/atom.c read it.
"""
import struct
import sys

CMD_NAMES = {
    0: "asic_init", 4: "digxencodercontrol", 10: "setengineclock",
    11: "setmemoryclock", 12: "setpixelclock", 13: "enabledisppowergating",
    34: "blankcrtc", 35: "enablecrtc", 41: "getsmuclockinfo",
    42: "selectcrtc_source", 46: "setdceclock", 49: "setcrtc_usingdtdtiming",
    50: "externalencodercontrol", 54: "processi2cchanneltransaction",
    76: "dig1transmittercontrol", 78: "processauxchanneltransaction",
}
DATA_NAMES = {
    0: "utilitypipeline", 1: "multimedia_info", 2: "smc_dpm_info", 4: "firmwareinfo",
    6: "lcd_info", 8: "smu_info", 11: "vram_usagebyfirmware", 12: "gpio_pin_lut",
    14: "gfx_info", 15: "powerplayinfo", 22: "displayobjectinfo", 23: "indirectioaccess",
    24: "umc_info", 27: "dce_info", 28: "vram_info", 30: "integratedsysteminfo",
    31: "asic_profiling_info", 32: "voltageobject_info",
}


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0] if o + 2 <= len(b) else 0


def table_list(b, base, names, label):
    if not base or base + 4 > len(b):
        print("%s: no table list (offset 0x%x)" % (label, base))
        return
    size = u16(b, base)
    n = max(0, (size - 4) // 2)
    print("%s list at 0x%x: header size %d, frev %d crev %d, %d entries" %
          (label, base, size, b[base + 2], b[base + 3], n))
    used = 0
    for i in range(n):
        off = u16(b, base + 4 + 2 * i)
        if not off:
            continue
        used += 1
        tsize = u16(b, off)
        fr, cr = (b[off + 2], b[off + 3]) if off + 4 <= len(b) else (0, 0)
        print("  %3d 0x%05x v%d.%d size %5d %s" % (i, off, fr, cr, tsize, names.get(i, "")))
    print("  (%d of %d entries used)" % (used, n))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    b = open(sys.argv[1], "rb").read()
    if b[:2] != b"\x55\xaa":
        sys.exit("not a PCI option ROM (no 55 AA)")
    hdr = u16(b, 0x48)
    if hdr + 0x22 > len(b) or b[hdr + 4:hdr + 8] != b"ATOM":
        sys.exit("no ATOM ROM header at 0x%x" % hdr)
    print("image %d bytes, ATOM ROM header at 0x%x: frev %d crev %d" %
          (len(b), hdr, b[hdr + 2], b[hdr + 3]))
    msg = u16(b, hdr + 0x10)
    if msg:
        end = b.find(b"\0", msg)
        print("boot message:", b[msg:end if end > 0 else msg + 80].decode("latin-1").strip())
    table_list(b, u16(b, hdr + 0x1E), CMD_NAMES, "command table")
    table_list(b, u16(b, hdr + 0x20), DATA_NAMES, "data table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
