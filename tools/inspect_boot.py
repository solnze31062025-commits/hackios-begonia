#!/usr/bin/env python3
"""
Inspect an Android boot image, including raw partition dumps produced by
TWRP ("boot.emmc.win"), and the MediaTek payload headers MTK LK requires.

Usage:
    python tools/inspect_boot.py boot.emmc.win
    python tools/inspect_boot.py hackios-begonia-boot.img

It prints exactly what LK reads on this device:
  * page size, header version, kernel/ramdisk/second addresses and sizes
  * the v2 dtb slot (LK patches it while booting, so it must be present)
  * the MTK "KERNEL"/"ROOTFS" wrappers (name, declared size)
  * whether the kernel payload is a valid ARM64 Linux Image (magic at 0x38)
Exits non-zero if the image cannot be booted by MTK LK.
"""
import gzip
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
MTK_MAGIC = 0x58881688
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def align_up(x: int, y: int) -> int:
    return (x + y - 1) // y * y


def find_boot_magic(blob: bytes) -> int:
    for off in (0, 512, 1024, 2048, 4096, 8192):
        if blob[off:off + 8] == BOOT_MAGIC:
            return off
    return blob.find(BOOT_MAGIC, 0, 1 << 20)


def parse_mtk_header(blob: bytes, off: int):
    if off + 512 > len(blob):
        return None
    magic, size, name = struct.unpack_from("<II32s", blob, off)
    if magic != MTK_MAGIC:
        return None
    return size, name.rstrip(b"\x00").decode("ascii", "replace")


def describe_kernel(blob: bytes, off: int, size: int):
    """LK accepts a raw ARM64 Image or a gzip-compressed one (the stock image
    ships gzip, so that is the well tested path)."""
    if blob[off:off + 2] == b"\x1f\x8b":
        try:
            raw = gzip.decompress(blob[off:off + size])
        except Exception as exc:  # noqa: BLE001 - report any gzip problem
            return f"  gzip kernel : corrupt ({exc})", False
        inner, ok = describe_arm64_image(raw, 0)
        return ("  gzip kernel : %d bytes -> %d bytes uncompressed\n" % (size, len(raw))) + inner, ok
    return describe_arm64_image(blob, off)


def describe_arm64_image(blob: bytes, off: int):
    if off + 0x40 > len(blob):
        return "payload too small to hold an ARM64 Image header", False
    code0, code1 = struct.unpack_from("<II", blob, off)
    text_offset, image_size, flags = struct.unpack_from("<QQQ", blob, off + 8)
    magic = blob[off + 0x38:off + 0x3c]
    ok = magic == b"ARM\x64"
    lines = [
        f"  code0        = {code0:#010x}",
        f"  code1        = {code1:#010x}",
        f"  text_offset  = {text_offset:#x}",
        f"  image_size   = {image_size:#x} ({image_size} bytes)",
        f"  flags        = {flags:#x}",
        f"  magic@0x38   = {magic!r} "
        + ("(valid ARM64 Image)" if ok else "(MISSING: LK will refuse this payload)"),
    ]
    return "\n".join(lines), ok


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    blob = open(path, "rb").read()
    print(f"file        : {path}")
    print(f"size        : {len(blob)} bytes ({len(blob) / 1024 / 1024:.1f} MiB)")

    hdr_off = find_boot_magic(blob)
    if hdr_off < 0:
        print("no ANDROID! magic found in the first 1 MiB")
        print("first 64 bytes:", blob[:64].hex(" "))
        return 1
    print(f"header at   : {hdr_off:#x}")

    (magic, ksize, kaddr, rsize, raddr, ssize, saddr, tags,
     page, hdr_version, os_version) = struct.unpack_from("<8s10I", blob, hdr_off)

    header_size = 1632
    dtb_size = 0
    dtb_addr = 0
    if hdr_version >= 1:
        rd_size, rd_off, header_size = struct.unpack_from("<IQI", blob, hdr_off + 1632)
    if hdr_version >= 2:
        dtb_size, dtb_addr = struct.unpack_from("<IQ", blob, hdr_off + 1648)

    cmdline = blob[hdr_off + 64:hdr_off + 64 + 512].split(b"\x00")[0].decode("ascii", "replace")

    print(f"page size   : {page}")
    print(f"hdr version : {hdr_version} (header_size {header_size}, os_version {os_version:#x})")
    print(f"cmdline     : {cmdline!r}")
    print()
    print(f"kernel      : size {ksize} ({ksize / 1024:.1f} KiB) @ {kaddr:#x}")
    print(f"ramdisk     : size {rsize} ({rsize / 1024:.1f} KiB) @ {raddr:#x}")
    print(f"second      : size {ssize} @ {saddr:#x}")
    print(f"tags        : @ {tags:#x}")
    if hdr_version >= 2:
        print(f"dtb         : size {dtb_size} ({dtb_size / 1024:.1f} KiB) @ {dtb_addr:#x}")
    print()

    problems = []
    base = hdr_off + align_up(max(header_size, page), page)
    slots = [("kernel", ksize, kaddr), ("ramdisk", rsize, raddr), ("second", ssize, saddr)]
    if hdr_version >= 2:
        slots.append(("dtb", dtb_size, dtb_addr))

    for name, size, addr in slots:
        if not size:
            continue
        print(f"--- {name} payload at file offset {base:#x}, declared {size} bytes")
        mtk = parse_mtk_header(blob, base)
        payload_off = base
        if mtk:
            mtk_size, mtk_name = mtk
            print(f"  MTK header  : name {mtk_name!r}, declared size {mtk_size}")
            payload_off = base + 512
        else:
            print("  MTK header  : none")
        if name == "kernel":
            desc, ok = describe_kernel(blob, payload_off, size)
            print(desc)
            if not ok:
                problems.append("kernel payload is not a valid ARM64 Image")
        if name == "dtb":
            magic_dtb = blob[payload_off:payload_off + 4]
            print(f"  fdt magic   : {magic_dtb!r} "
                  + ("(valid device tree)" if magic_dtb == FDT_MAGIC
                     else "(MISSING: LK has no dtb to patch)"))
            if magic_dtb != FDT_MAGIC:
                problems.append("v2 dtb slot does not contain a valid FDT")
        base += align_up(size, page)
        print()

    if hdr_version >= 2 and not dtb_size:
        problems.append("header v2 without dtb (stock has 165214 bytes)")

    if problems:
        for p in problems:
            print(f"ERROR: {p}")
        return 1
    print("OK: boot image layout and kernel header look bootable for MTK LK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
