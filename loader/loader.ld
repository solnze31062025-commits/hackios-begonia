/* Linker script — stage-2 loader for begonia (loaded at the kernel slot). */
ENTRY(_start)

KERNEL_SLOT_ADDR = 0x40080000;

SECTIONS
{
	/* MTK LK drops the boot.img kernel payload (plus its 0x200-byte MTK
	 * header, which we strip in code? no: header counted in image only)
	 * at this address; the loader must be linked at the same address. */
	. = KERNEL_SLOT_ADDR;

	__load_start = .;

	/* ARM64 Linux Image header + branch to the real entry (see image.S). */
	.image :
	{
		*(.image)
	}

	.text :
	{
		*(.text.start)
		*(.text*)
	}

	.data :
	{
		*(.rodata*)
		*(.data*)
	}

	__payload_end = .;

	.bss (NOLOAD) :
	{
		__bss_start = .;
		*(.bss*)
		*(COMMON)
		__bss_end = .;
	}

	__load_end = .;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) }
}
