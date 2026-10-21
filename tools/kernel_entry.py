#!/usr/bin/env python3
"""Print the LC_UNIXTHREAD entry point of an (unslid, little-endian) arm64
XNU kernel Mach-O. Flow/debug helper for the stage-2 loader.

ARM64 LC_UNIXTHREAD layout: cmd(4), cmdsize(4), flavor(4), count(4),
then `count` 64-bit registers: x0..x30, sp, pc  -> pc at offset
cmd + 8 + 8 + 32*8 = cmd + 264.
"""
import struct
import sys

MH_MAGIC_64 = 0xFeedFace  # little-endian read: 0xfeedfacf
MH_EXECUTE  = 2
LC_UNIXTHREAD = 0x5

def main() -> int:
    data = open(sys.argv[1], "rb").read(4096)
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = \
        struct.unpack_from("<IIIIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit(f"not MH_MAGIC_64 (magic={magic:#x})")
    if ftype != MH_EXECUTE:
        raise SystemExit(f"not an MH_EXECUTE image (filetype={ftype})")
    pos = 32
    entry = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, pos)
        if cmd == LC_UNIXTHREAD:
            entry = struct.unpack_from("<Q", data, pos + 8 + 8 + 32 * 8)[0]
            break
        pos += cmdsize
    if entry is None:
        raise SystemExit("no LC_UNIXTHREAD found")
    print(f"entry = {entry:#x}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
