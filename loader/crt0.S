/*
 * Stage-2 loader entry point after MTK LK jumps to the "kernel" slot.
 *
 * State we reasonably assume: MMU off, single core, UART clocked by LK.
 * CurrentEL is EL1 or EL2; we normalize to EL1.
 *
 * IMPORTANT: all address materialization below uses adrp+add (PC-relative)
 * instead of ldr =symbol (link-time absolute). MTK LK may load the payload
 * shifted by the 0x200-byte MTK header, and the whole image must keep
 * working under that offset.
 */
.text
.section .text.start, "ax"
.balign 8
.global _start
_start:
	/* Stack first: C calls below need it (PC-relative address). */
	adrp	x28, __stack_top
	add	x28, x28, :lo12:__stack_top
	mov	sp, x28

	/* Zero .bss — byte-stable loop, PC-relative bounds. */
	adrp	x29, __bss_start
	add	x29, x29, :lo12:__bss_start
	adrp	x30, __bss_end
	add	x30, x30, :lo12:__bss_end
1:	cmp	x29, x30
	b.hs	2f
	strb	wzr, [x29], #1
	b	1b
2:

	/* CurrentEL: bits[3:2]: 0b0100=EL1, 0b1000=EL2, 0b1100=EL3. */
	mrs	x27, CurrentEL
	cmp	x27, #0xC
	b.eq	el3_panic
	cmp	x27, #0x8
	b.eq	enter_from_el2
	b	el1_entry

el3_panic:
	adrp	x0, panic_el3
	add	x0, x0, :lo12:panic_el3
	bl	uart_puts
	b	.

enter_from_el2:
	adrp	x0, el1_entry
	add	x0, x0, :lo12:el1_entry
	msr	ELR_EL2, x0
	mov	x0, #0x3C5		/* EL1h, DAIF masked */
	msr	SPSR_EL2, x0
	mrs	x0, CPTR_EL2
	bic	x0, x0, #0x400		/* clear TFP */
	msr	CPTR_EL2, x0
	isb
	eret

el1_entry:
	adrp	x16, loader_main
	add	x16, x16, :lo12:loader_main
	blr	x16

loop:
	b	loop

.section .rodata
panic_el3:
	.asciz "\n[loader] running at EL3, unsupported\n"

.section .bss.stack
.balign 16
	.skip 0x100000 /* 1 MB stack, grows down from __stack_top */
.global __stack_top
__stack_top:
