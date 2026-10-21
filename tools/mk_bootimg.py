#!/usr/bin/env python3
"""
Pack payloads into an Android boot image (header v0) as MTK LK expects it
on begonia (MT6785):

  - kernel slot  : stage-2 loader (raw aarch64)
  - ramdisk slot : XNU kernel Mach-O (loader resolves + relocates it)

Optionally wraps each payload in the MediaTek 512-byte header ("KERNEL" /
"ROOTFS") that MTK LK requires to accept the image on this platform.
"""
import argparse
import struct

BOOT_MAGIC = b"ANDROID!"
MTK_MAGIC  = 0x58881688

def align_up(x: int, y: int) -> int:
    return (x + y - 1) // y * y

def mtk_header(name: str, size: int) -> bytes:
    """512-byte MediaTek payload header: magic(4) + size(4) + name(32) + pad."""
    raw = struct.pack("<II32s", MTK_MAGIC, size, name.encode())
    return raw + b"\x00" * (512 - len(raw))

def pad_to(b: bytes, page: int) -> bytes:
    return b + b"\x00" * (align_up(len(b), page) - len(b))

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--kernel", required=True, help="stage-2 loader binary")
    p.add_argument("--ramdisk", default=None, help="XNU kernel Mach-O (optional)")
    p.add_argument("--cmdline", default="debug=0x14e")
    p.add_argument("--pagesize", type=int, default=2048)
    p.add_argument("--base", default="0x40000000")
    p.add_argument("--kernel-offset", default="0x00080000")
    p.add_argument("--ramdisk-offset", default="0x07c80000")
    p.add_argument("--second-offset", default="0x00000000")
    p.add_argument("--tags-offset", default="0x0bc80000")
    p.add_argument("--mtk-header", action="store_true",
                   help="prepend MediaTek KERNEL/ROOTFS headers (the stock "
                        "begonia boot.img has none; enable only if LK asks)")
    p.add_argument("-o", "--output", required=True)
    args = p.parse_args()

    kernel = open(args.kernel, "rb").read()
    ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b""

    if args.mtk_header:
        kernel  = mtk_header("KERNEL", len(kernel)) + kernel
        if ramdisk:
            ramdisk = mtk_header("ROOTFS", len(ramdisk)) + ramdisk

    base = int(args.base, 0)
    kernel_addr   = base + int(args.kernel_offset, 0)
    ramdisk_addr  = base + int(args.ramdisk_offset, 0)
    second_addr   = base + int(args.second_offset, 0)
    tags_addr     = base + int(args.tags_offset, 0)

    cmdline = args.cmdline.encode()
    if len(cmdline) > 512:
        raise SystemExit("cmdline too long")

    # boot_img_hdr_v0 (AOSP): magic 8s, 10 x u32, name 16s, cmdline 512s,
    # id 8 x u32, extra_cmdline 1024s  = 1632 bytes total.
    hdr = struct.pack(
        "<8s10I16s512s32s1024s",
        BOOT_MAGIC,
        len(kernel), kernel_addr,
        len(ramdisk), ramdisk_addr,
        0, second_addr,              # second size + addr
        tags_addr,                   # tags addr
        args.pagesize,               # page_size
        0,                           # header_version
        0,                           # os_version
        b"\x00" * 16,                # product name
        cmdline + b"\x00" * (512 - len(cmdline)),
        b"\x00" * 32,                # id hash
        b"\x00" * 1024,              # extra cmdline
    )
    assert len(hdr) == 1632

    blob = bytearray(hdr)
    # The header occupies one full page; payloads start at page boundaries.
    # Without this padding LK reads garbage/zeros as the kernel and resets the
    # SoC in a loop (logo -> black -> reboot) instead of jumping into it.
    blob += b"\x00" * (align_up(len(blob), args.pagesize) - len(blob))
    blob += pad_to(kernel, args.pagesize)
    if ramdisk:
        blob += pad_to(ramdisk, args.pagesize)

    with open(args.output, "wb") as f:
        f.write(blob)

    print(f"boot.img written: {args.output} ({len(blob)} bytes)")
    print(f"  kernel slot : {len(kernel)} bytes (addr {kernel_addr:#x})")
    print(f"  ramdisk slot: {len(ramdisk)} bytes (addr {ramdisk_addr:#x})")
    print("NOTE: ramdisk_addr here is the SLOT inside the image; running")
    print("RAM addresses live in loader/loader.h (BEGONIA_RAMDISK_ADDR).")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
