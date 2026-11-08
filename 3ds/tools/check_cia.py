#!/usr/bin/env python3
"""Sanity-checks a built wa3ds.cia the way the 3DS loader and kernel read it.

Each check guards a problem that made the .cia crash on hardware:
  - kernel flags must carry the APPLICATION memory region (Luma's loader panics otherwise)
  - DSP memory 0x1FF00000-0x1FF7FFFF must be mapped read-write (ndspInit writes to it)
  - VRAM 0x1F000000-0x1F5FFFFF must be mapped (the keyboard's screen capture reads it)
  - the code must be stored uncompressed (EnableCompress: false in cia.rsf)
Usage: check_cia.py wa3ds.cia
"""
import struct
import sys

def main(path):
    d = open(path, "rb").read()
    magic = d.find(b"NCCH")
    if magic < 0x100:
        sys.exit("no NCCH partition found")
    exh = magic - 0x100 + 0x200            # extended header follows the 0x200-byte NCCH header
    errors = []

    flags = d[exh + 0xD]                    # SCI code-set flags: bit0 compressed code, bit1 SD app
    print("code-set flags: 0x%02x" % flags)
    if flags & 1:
        errors.append("code is compressed")

    caps = struct.unpack_from("<28I", d, exh + 0x370)   # ACI kernel capability descriptors
    region = 0
    dsp_rw = None
    vram = False
    for i, c in enumerate(caps):
        if c >> 23 == 0x1FE:                # kernel flags descriptor
            region = c & 0xF00
        if c >> 21 == 0x7FC:                # memory mapping (prefix 0b11111111100)
            page = c & 0xFFFFF
            read_only = bool(c & (1 << 20))
            print("mapping descriptor 0x%08x: page 0x%05x %s" % (c, page, "RO" if read_only else "RW"))
            if page == 0x1FF00:             # start of the DSP memory range
                dsp_rw = not read_only
            if page == 0x1F000:             # start of VRAM
                vram = True
    print("memory region: 0x%03x" % region)
    print("DSP mapping: %s" % ("missing" if dsp_rw is None else "read-write" if dsp_rw else "read-only"))
    if region != 0x100:
        errors.append("memory region is not APPLICATION (0x100)")
    if dsp_rw is not True:
        errors.append("DSP memory 0x1FF00000 is not mapped read-write")
    print("VRAM mapping: %s" % ("yes" if vram else "missing"))
    if not vram:
        errors.append("VRAM 0x1F000000 is not mapped")

    for e in errors:
        print("::error::" + e)
    sys.exit(1 if errors else 0)

if __name__ == "__main__":
    main(sys.argv[1])
