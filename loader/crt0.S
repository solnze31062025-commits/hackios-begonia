/*
 * Stage-2 loader entry point after MTK LK jumps to the "kernel" slot.
 *
 * Assumed state: MMU off, single core running, UART already clocked by LK.
 * CurrentEL is either EL1 or EL2 (MTK LK drops to EL1 itself or jumps at
 * EL2 with a trampoline). We normalize to EL1 ourselves.
 */
.text
.section .text.start, "ax"
.balign 8
.global _start
_start:
	/* Stack first: all C calls below need it. */
	ldr	x28, =__stack_top
	mov	sp, x28

	/* Zero .bss (LK does not do this for us) — byte-stable loop.
	 * adrp+add instead of adr: the BSS is > 1MB, out of adr range. */
	adrp	x29, __bss_start
	add	x29, x29, :lo12:__bss_start
	adrp	x30, __bss_end
	add	x30, x30, :lo12:__bss_end
1:	cmp	x29, x30
	b.hs	2f
	strb	wzr, [x29], #1
	b	1b
2:

	/* Early console before anything can go wrong. */
	bl	uart_early_init
	ldr	x0, =banner
	bl	uart_puts

	/* CurrentEL: bits[3:2]: 0b0100=EL1, 0b1000=EL2, 0b1100=EL3. */
	mrs	x27, CurrentEL
	cmp	x27, #0xC
	b.eq	el3_panic

	cmp	x27, #0x8
	b.eq	enter_from_el2

	/* EL1: run directly. */
el1_go:
	b	el1_entry

el3_panic:
	ldr	x0, =panic_el3
	bl	uart_puts
	b	.

enter_from_el2:
	/*
	 * Drop to EL1: SPSR_EL2 = EL1h + DAIF masked (0b1111<<6 | 5),
	 * ELR_EL2 = our EL1 entry. Also clear any FP/SIMD traps
	 * (CPTR_EL2.TFP = bit 10) so nothing traps after the drop.
	 */
	ldr	x0, =dropping_to_el1
	bl	uart_puts

	ldr	x0, =el1_entry
	msr	ELR_EL2, x0
	mov	x0, #0x3C5		/* EL1h, DAIF masked */
	msr	SPSR_EL2, x0
	mrs	x0, CPTR_EL2
	bic	x0, x0, #0x400		/* clear TFP: allow FP/SIMD access */
	msr	CPTR_EL2, x0
	isb
	eret

el1_entry:
	ldr	x16, =loader_main
	blr	x16

	/* never returns */
loop:
	b	loop

.section .rodata
banner:
	.asciz "\n\n[loader] HackiOS begonia stage-2, jumped from LK\n"
panic_el3:
	.asciz "\n[loader] running at EL3, unsupported\n"
dropping_to_el1:
	.asciz "[loader] dropping EL2 -> EL1\n"

.section .bss.stack
.balign 16
.global __stack_top
__stack_top:
	.skip 0x100000 /* 1 MB stack */
