#!/usr/bin/env python3
"""
Pack payloads into an Android boot image for begonia (MT6785), matching what
the stock boot.img of this device looks like, because MTK LK parses it:

    stock boot.emmc.win:
        header_version = 2, page_size = 2048, header_size = 1660
        kernel  @ 0x40080000
        ramdisk @ 0x47c80000
        dtb     @ 0x4bc80000  (165214 bytes, magic d00dfeed)
        tags    @ 0x4bc80000
        no MTK "KERNEL"/"ROOTFS" wrappers

LK loads the dtb and patches it (bootargs, atag,videolfb, ...). A v0 image
without a dtb makes it crash while preparing the boot (observed as a
logo -> black -> reboot loop with androidboot.bootreason=lk_crash).

kernel slot  : stage-2 loader (raw aarch64, valid ARM64 Image header)
ramdisk slot : XNU kernel Mach-O (loader resolves + relocates it)
dtb slot     : stock begonia device tree from the backup
"""
import argparse
import struct

BOOT_MAGIC = b"ANDROID!"
MTK_MAGIC = 0x58881688

V0_FMT = "<8s10I16s512s32s1024s"
V0_SIZE = 1632

STOCK_CMDLINE = ("bootopt=64S3,32N2,64N2 androidboot.super_partition=system "
                 "androidboot.init_fatal_reboot_target=recovery "
                 "androidboot.serialconsole=0 androidboot.super_partition=system")


def align_up(x: int, y: int) -> int:
    return (x + y - 1) // y * y


def mtk_header(name: str, size: int) -> bytes:
    """512-byte MediaTek payload header: magic(4) + size(4) + name(32) + pad."""
    raw = struct.pack("<II32s", MTK_MAGIC, size, name.encode())
    return raw + b"\x00" * (512 - len(raw))


def pad_to(b: bytes, page: int) -> bytes:
    return b + b"\x00" * (align_up(len(b), page) - len(b))


def build_header(args, ksize, kaddr, rsize, raddr, ssize, saddr, dsize, daddr):
    cmdline = args.cmdline.encode()
    if len(cmdline) > 512:
        raise SystemExit("cmdline too long")
    v0 = struct.pack(
        V0_FMT,
        BOOT_MAGIC,
        ksize, kaddr,
        rsize, raddr,
        ssize, saddr,
        saddr,                    # tags_addr == dtb_addr on this device
        args.pagesize,
        args.header_version,
        0,                        # os_version
        b"\x00" * 16,             # product name
        cmdline + b"\x00" * (512 - len(cmdline)),
        b"\x00" * 32,             # id hash
        b"\x00" * 1024,           # extra cmdline
    )
    assert len(v0) == V0_SIZE

    if args.header_version == 0:
        return v0

    # v1: recovery_dtbo_size, recovery_dtbo_offset, header_size
    header_size = V0_SIZE + 4 + 8 + 4 + (4 + 8 if args.header_version >= 2 else 0)
    v1 = struct.pack("<IQI", 0, 0, header_size)
    if args.header_version == 1:
        return v0 + v1

    # v2: dtb_size, dtb_addr
    return v0 + v1 + struct.pack("<IQ", dsize, daddr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--kernel", required=True, help="stage-2 loader binary")
    p.add_argument("--ramdisk", default=None, help="XNU kernel Mach-O (optional)")
    p.add_argument("--dtb", default=None, help="device tree blob for header v2")
    p.add_argument("--cmdline", default=STOCK_CMDLINE)
    p.add_argument("--pagesize", type=int, default=2048)
    p.add_argument("--header-version", type=int, default=2, choices=(0, 1, 2))
    p.add_argument("--base", default="0x40000000")
    p.add_argument("--kernel-offset", default="0x00080000")
    p.add_argument("--ramdisk-offset", default="0x07c80000")
    p.add_argument("--second-offset", default="0x00000000")
    p.add_argument("--tags-offset", default="0x0bc80000")
    p.add_argument("--device-tree-offset", default="0x0bc80000")
    p.add_argument("--mtk-header", action="store_true",
                   help="prepend MediaTek KERNEL/ROOTFS headers (the stock "
                        "begonia boot.img has none; enable only if LK asks)")
    p.add_argument("-o", "--output", required=True)
    args = p.parse_args()

    kernel = open(args.kernel, "rb").read()
    ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b""
    dtb = open(args.dtb, "rb").read() if args.dtb else b""

    if args.mtk_header:
        kernel = mtk_header("KERNEL", len(kernel)) + kernel
        if ramdisk:
            ramdisk = mtk_header("ROOTFS", len(ramdisk)) + ramdisk

    base = int(args.base, 0)
    kernel_addr = base + int(args.kernel_offset, 0)
    ramdisk_addr = base + int(args.ramdisk_offset, 0)
    second_addr = base + int(args.second_offset, 0)
    tags_addr = base + int(args.tags_offset, 0)
    dtb_addr = base + int(args.device_tree_offset, 0)

    if args.header_version >= 2 and not dtb:
        raise SystemExit("header v2 needs --dtb (LK patches it while booting)")

    hdr = build_header(args, len(kernel), kernel_addr, len(ramdisk),
                       ramdisk_addr, 0, second_addr, len(dtb), dtb_addr)

    blob = bytearray(hdr)
    # The header occupies whole pages; payloads start at page boundaries.
    blob += b"\x00" * (align_up(len(blob), args.pagesize) - len(blob))
    blob += pad_to(kernel, args.pagesize)
    if ramdisk:
        blob += pad_to(ramdisk, args.pagesize)
    if dtb:
        blob += pad_to(dtb, args.pagesize)

    with open(args.output, "wb") as f:
        f.write(blob)

    print(f"boot.img written: {args.output} ({len(blob)} bytes)")
    print(f"  header      : v{args.header_version}, page {args.pagesize}")
    print(f"  kernel slot : {len(kernel)} bytes @ {kernel_addr:#x}")
    print(f"  ramdisk slot: {len(ramdisk)} bytes @ {ramdisk_addr:#x}")
    print(f"  dtb         : {len(dtb)} bytes @ {dtb_addr:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
