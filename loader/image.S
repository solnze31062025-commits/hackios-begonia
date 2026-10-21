/*
 * ARM64 Linux "Image" header trampoline.
 *
 * MediaTek LK refuses to jump into a payload it does not recognize as an
 * aarch64 kernel: boot_linux() validates the 64-bit Image magic "ARM\x64"
 * at offset 0x38 and, when it is missing, resets the SoC instead of
 * branching into the payload (observed as "logo -> black -> reboot" loop).
 *
 * So the loader binary starts with a synthetic Image header. The first
 * instruction is a branch to the real entry point, which is what the
 * Linux arm64 boot protocol does as well (code0 = "b stext").
 *
 * Layout (ARM64 Image header, 64 bytes):
 *   0x00 code0        branch to _start
 *   0x04 code1        0
 *   0x08 text_offset  0      -> LK jumps to kernel_addr + 0
 *   0x10 image_size   loader size
 *   0x18 flags        0      (little-endian, 4K pages)
 *   0x20 res2         0
 *   0x28 res3         0
 *   0x30 res4         0
 *   0x38 magic        0x644d5241 ("ARM\x64")
 *   0x3c res5         0
 */
.section .image, "ax"
.balign 8
.global _image_start
_image_start:
	b	_start
	.word	0
	.quad	0			/* text_offset */
	.quad	__payload_end - _image_start	/* image_size */
	.quad	0			/* flags */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.byte	0x41, 0x52, 0x4d, 0x64	/* "ARM\x64" magic at 0x38 */
	.word	0			/* res5 */
.global _image_end
_image_end:
