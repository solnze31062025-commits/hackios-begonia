/*
 * HackiOS stage-2 loader main (begonia / Helio G90T).
 *
 * Flow after MTK LK hands control over (MMU OFF, single core):
 *   1. console up
 *   2. copy XNU Mach-O segments to their physical locations
 *   3. build flattened device tree + boot_args (scratch area)
 *   4. enable MMU: identity map DRAM + KVA map for the kernel link space
 *   5. branch to kernel entry with x0 = &boot_args (KVA)
 */
#include "loader.h"
#include <stddef.h>

/*
 * NOTE: this file deliberately avoids the C library; everything is built
 * freestanding with a minimal API to be reviewable by eye.
 */

extern uint8_t __stack_top[];         /* crt0.S */

/*
 * x0 saved by crt0.S: the physical address of the device tree LK patched for
 * this boot. In .data so the .bss clearing in crt0 cannot wipe it.
 */
uint64_t g_boot_dtb __attribute__((section(".data")));

/* Framebuffer base reported by LK for this boot, 0 when unknown. */
static uint64_t g_fb_live;

void
loader_panic(const char *msg)
{
	uart_puts("\n[LOADER PANIC] ");
	uart_puts(msg);
	uart_puts("\n");
	fb_panic(msg); /* take over the screen too (infinite loop inside) */
}

/* ------------------------------------------------------------------ */
/* Mach-O                                                             */

static int
macho_copy_kernel(uint64_t src, uint64_t dest_phys)
{
	struct mach_header64 *mh = (struct mach_header64 *)src;

	uart_puts("[loader] mach_header magic ");
	uart_puthex(mh->magic);
	if (mh->magic != MH_MAGIC_64) {
		loader_panic("ramdisk slot does not contain a Mach-O64 kernel");
	}
	if (mh->filetype != MH_EXECUTE) {
		uart_puts(" (filetype ");
		uart_puthex(mh->filetype);
		uart_puts(")\n");
		loader_panic("kernel is not an MH_EXECUTE image (is it a prelinked kernelcache?)");
	}

	uint8_t *cursor = (uint8_t *)(src + sizeof(struct mach_header64));
	uint64_t total = 0;
	uint64_t first_vmaddr = 0;

	for (uint32_t i = 0; i < mh->ncmds; i++) {
		struct segment_command64 *seg = (struct segment_command64 *)cursor;
		if (seg->cmd == LC_SEGMENT_64) {
			if (first_vmaddr == 0) {
				first_vmaddr = seg->vmaddr;
			}
			uint64_t dest = dest_phys + (seg->vmaddr - first_vmaddr);
			/* byte copy (fileoff -> phys dest) */
			const uint8_t *s = (const uint8_t *)(src + seg->fileoff);
			uint8_t *d = (uint8_t *)dest;
			for (uint64_t n = 0; n < seg->filesize; n++) {
				d[n] = s[n];
			}
			uart_puts("[loader] seg ");
			uart_puts_n(seg->segname, 16);
			uart_puts(" vm=");
			uart_puthex(seg->vmaddr);
			uart_puts(" -> phys ");
			uart_puthex(dest);
			uart_puts(" size=");
			uart_puthex(seg->filesize);
			uart_puts("\n");
			total += seg->filesize;
		}
		cursor += seg->cmdsize;
	}
	uart_puts("[loader] copied ");
	uart_puthex(total);
	uart_puts(" bytes to phys base ");
	uart_puthex(dest_phys);
	uart_puts("\n");

	return 0;
}

static uint64_t
macho_find_entry(uint64_t src)
{
	struct mach_header64 *mh = (struct mach_header64 *)src;
	uint8_t *cursor = (uint8_t *)(src + sizeof(struct mach_header64));
	uint64_t entry = 0;
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		struct segment_command64 *seg = (struct segment_command64 *)cursor;
		if (seg->cmd == LC_UNIXTHREAD) {
			/*
			 * LC_UNIXTHREAD layout: cmd(4), cmdsize(4), flavor(4),
			 * count(4), then `count` 64-bit register values.
			 * ARM64 thread state = 33 scalars: x0..x30, sp, pc.
			 * The entry point is therefore the 33rd qword:
			 * cmd + 8 + 32 * 8.
			 */
			entry = *(uint64_t *)((uint8_t *)seg + 8 + 32 * 8);
			break;
		}
		cursor += seg->cmdsize;
	}
	return entry;
}

/* ------------------------------------------------------------------ */
/* Device tree for the kernel (the contract in pexpert, see bringup.md) */

static void
build_device_tree(void)
{
	static struct dt_writer w;
	dt_init(&w, (void *)LOADER_DT_ADDR, LOADER_DT_MAX_SIZE);

	dt_node_open(&w, ""); /* root */

	/* /arm-io: name + device_type + ranges (first entry = SoC MMIO base) */
	dt_node_open(&w, "arm-io");
	{
		static const char dt_armio[] = "arm-io";
		dt_prop_str(&w, "device_type", dt_armio);
		/* ranges: child-addr(2 cells) parent(2)-size, all BE, 4 cells each:
		 * firstdoux.u64 pair after (offset 8) is read as ranges_prop[1]
		 * (big-endian base). Give SoC base = 0 so per-node reg offsets
		 * carry absolute addresses (see pe_identify_machine.c). */
		static const uint8_t ranges[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		};
		dt_prop_bytes(&w, "ranges", ranges, sizeof(ranges));
	}

	/* /arm-io/uart0: our console */
	dt_node_open(&w, "uart0");
	{
		static const char *compat0[] = { "mediatek,mt6785-uart" };
		dt_prop_stringlist(&w, "compatible", compat0, 1);
		dt_prop_str(&w, "device_type", "serial");
		dt_prop_reg(&w, BEGONIA_UART0_BASE, 0x100);
		dt_prop_u32(&w, "AAPL,phandle", 1);
	}

	/* /arm-io/gic: interrupt controller (master) */
	dt_node_open(&w, "gic");
	{
		static const char master[] = "master";
		dt_prop_str(&w, "interrupt-controller", master);
		/* reg: GICD base, size, GICR base, size — four BE u64 = 32 bytes.
		 * The GICv3 FIQ path needs the whole 32-byte layout. */
		static const uint8_t reg[] = {
			0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* GICD  0x0c000000 */
			0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* GICD size 1M */
			0x00, 0x0c, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,   /* GICR  0x0c200000 TBD */
			0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* GICR size 2M TBD */
		};
		dt_prop_bytes(&w, "reg", reg, sizeof(reg));
	}

	/* /arm-io/timer */
	dt_node_open(&w, "timer");
	{
		dt_prop_str(&w, "device_type", "timer");
		dt_prop_reg(&w, 0x00100000, 0x1000); /* TBD; unused by arm64 path */
	}

	dt_node_close(&w); /* gic, timer etc are inside arm-io */
	dt_node_close(&w); /* arm-io */

	/* /cpus with 8 state=running entries */
	dt_node_open(&w, "cpus");
	for (uint32_t cpu = 0; cpu < 8; cpu++) {
		char name[10];
		name[0] = 'c'; name[1] = 'p'; name[2] = 'u';
		name[3] = '0' + (char)(cpu / 10);
		name[4] = '0' + (char)(cpu % 10);
		name[5] = 0;
		dt_node_open(&w, name);
		dt_prop_str(&w, "state", "running");
		/* timebase 13 MHz (ARM generic timer, helio g90t). BE u32. */
		dt_prop_u32(&w, "timebase-frequency", 13000000);
		dt_prop_u32(&w, "bus-frequency", 66000000);  /* TBD */
		dt_prop_u32(&w, "clock-frequency", cpu < 2 ? 2050000000 : 2000000000); /* convenience hack: fits 32 bits */
		dt_node_close(&w);
	}
	dt_node_close(&w); /* cpus */

	/* /defaults points the kernel at the console UART */
	dt_node_open(&w, "defaults");
	dt_prop_u32(&w, "serial-device", 1); /* phandle 1 = uart0 */
	dt_node_close(&w);

	/* /chosen */
	dt_node_open(&w, "chosen");
	dt_prop_str(&w, "bootargs", "serial-device-name=uart0 debug=0x14e");
	dt_node_close(&w);

	dt_node_close(&w); /* root */
}

/* ------------------------------------------------------------------ */
/* mmu bootstrap                                                      */

void
mmu_boot_enable(void);

/* ------------------------------------------------------------------ */

static void
fill_boot_args(uint64_t kernel_phys_base, uint64_t kernel_link_base)
{
	struct boot_args *ba = (struct boot_args *)LOADER_BOOTARGS_ADDR;
	/* Struct is little-endian ARM64: plain field stores. */
	ba->Revision = BOOT_ARGS_REVISION;
	ba->Version  = BOOT_ARGS_VERSION;
	ba->virtBase = kernel_link_base;         /* XNU link VMA (unslid) */
	ba->physBase = kernel_phys_base;         /* kernel load address */
	ba->memSize  = BEGONIA_RAM_SIZE;
	ba->topOfKernelData = LOADER_TOP_KERNEL_DATA;
	/* Video: none — kernel falls back to serial console */
	ba->Video.v_baseAddr = 0;
	ba->Video.v_display  = 0;
	ba->Video.v_rowBytes = 0;
	ba->Video.v_width    = 0;
	ba->Video.v_height   = 0;
	ba->Video.v_depth    = 0;
	ba->machineType = 0;
	ba->deviceTreeP = (uint64_t)LOADER_DT_ADDR;
	ba->deviceTreeLength = (uint32_t)LOADER_DT_MAX_SIZE;
	for (size_t i = 0; i < sizeof(ba->CommandLine); i++) {
		ba->CommandLine[i] = 0;
	}
	const char cmd[] = "serial-device-name=uart0";
	for (size_t i = 0; cmd[i]; i++) {
		ba->CommandLine[i] = cmd[i];
	}
	ba->bootFlags = 0;
	ba->memSizeActual = BEGONIA_RAM_SIZE;
}

/* ------------------------------------------------------------------ */
/* PSCI (device tree says compatible "arm,psci-1.0", method "smc")    */

/* PSCI 0.2/1.0 function IDs. */
#define PSCI_SYSTEM_OFF    0x84000008ULL
#define PSCI_SYSTEM_RESET  0x84000009ULL

/*
 * SMC calls are handled by ARM Trusted Firmware at EL3. If ATF does not
 * implement the call it returns an error code instead of powering off, so
 * every caller keeps a "freeze" fallback.
 */
static int
psci_call(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	__asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
	return (int)x0;
}

static void __attribute__((noreturn))
psci_system_off(void)
{
	int rc = psci_call(PSCI_SYSTEM_OFF);
	plog_hex("psci: SYSTEM_OFF rc", (uint64_t)(uint32_t)rc);
	/* ATF refused or the call returned: park the CPU instead of resetting. */
	for (;;) {
		__asm__ volatile("wfe");
	}
}

/*
 * Exception entry (crt0.S installs vectors_el1 with this as the target).
 *
 * Deliberately does NOT touch UART or the framebuffer: whatever faulted may
 * fault again, and a recursive fault would look exactly like a bootloop.
 * The log write is the only risky part, so a nested fault just spins.
 */
void __attribute__((noreturn))
loader_fault(uint64_t esr, uint64_t sp)
{
	static volatile int nested;

	if (nested) {
		for (;;) {
			__asm__ volatile("wfe");
		}
	}
	nested = 1;

	plog("FAULT: exception taken");
	plog_hex("FAULT: ESR_EL1", esr);
	plog_hex("FAULT: SP", sp);

	psci_system_off();
}

/*
 * Leave the device in a state that LK's own failure path cannot produce:
 * power off, else reset, else freeze. Any of the three is fast, while the
 * LK crash loop on this device takes about seven seconds.
 */
static void __attribute__((noreturn))
psci_off_or_reset(void)
{
	int rc = psci_call(PSCI_SYSTEM_OFF);
	plog_hex("psci: SYSTEM_OFF rc", (uint64_t)(uint32_t)rc);

	rc = psci_call(PSCI_SYSTEM_RESET);
	plog_hex("psci: SYSTEM_RESET rc", (uint64_t)(uint32_t)rc);

	for (;;) {
		__asm__ volatile("wfe");
	}
}

static void
report(const char *msg)
{
	uart_puts("[loader] ");
	uart_puts(msg);
	uart_puts("\n");
}

void
loader_main(void)
{
	uint64_t current_el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));

	plog("main: C code reached");
	plog_hex("main: CurrentEL", current_el);
	plog_hex("main: x0 from LK (dtb)", g_boot_dtb);

	/* What did LK actually hand over for this boot? */
	if (fdt_valid(g_boot_dtb)) {
		uint32_t lo   = fdt_chosen_u32(g_boot_dtb, "atag,videolfb-fb_base_l");
		uint32_t hi   = fdt_chosen_u32(g_boot_dtb, "atag,videolfb-fb_base_h");
		uint32_t vram = fdt_chosen_u32(g_boot_dtb, "atag,videolfb-vramSize");
		uint32_t inited = fdt_chosen_u32(g_boot_dtb, "atag,videolfb-islcm_inited");
		const char *lcm = fdt_chosen_str(g_boot_dtb, "atag,videolfb-lcmname");

		g_fb_live = ((uint64_t)hi << 32) | (uint64_t)lo;

		plog("dtb: valid FDT from LK");
		plog_hex("dtb: atag,videolfb-fb_base", g_fb_live);
		plog_hex("dtb: atag,videolfb-vramSize", vram);
		plog(inited ? "dtb: lcm inited by LK" : "dtb: lcm NOT inited by LK");
		if (lcm && lcm[0]) {
			plog("dtb: lcm name present");
		}
	} else {
		plog("dtb: x0 from LK is not a valid FDT");
	}

#if LOADER_EXEC_PROOF == 2
	/*
	 * Fast proof. No framebuffer, no delays: if the phone powers off or
	 * reboots within a fraction of a second instead of after the usual
	 * ~7 s LK failure, the payload really executed.
	 */
	plog("proof2: leaving immediately");
	psci_off_or_reset();
#endif

	/* We are executing; now try to show it. */
	fb_instant_paint();
	plog("fb: whole window painted");

	uart_early_init();
	report("HackiOS stage-2 loader (begonia) v0.6 (pstore logging)");

#if LOADER_EXEC_PROOF
	/*
	 * Isolation test, no kernel involved:
	 *   blue window  -> code executes
	 *   green status -> framebuffer offsets
	 *   power off    -> ATF/heartbeat still alive, i.e. we own the SoC
	 */
	fb_wait_ms(1500);
	fb_status_screen(0xFF0A7A0A, "LOADER RAN OK", "FB WINDOW A/B/C");
	plog("fb: status screen drawn");
	fb_wait_ms(2500);

	plog("psci: calling SYSTEM_OFF");
	psci_system_off();
#endif

#if LOADER_VISUAL_ONLY
	/* Keep the blue paint on screen briefly, then show the diagnostic
	 * screen: green window + A/B/C labels at the three candidate
	 * buffer offsets (which letter is visible tells us the live base). */
	fb_wait_ms(1200);
	fb_halt_screen(0xFF0A7A0A, "LOADER RAN OK", "FB WINDOW A/B/C");
#endif

	if (BEGONIA_FB_PROBE_ENABLE) {
		fb_probe_cycle();
	}
	fb_draw_menu(3);

	report("EL/MPIDR state:");
	uint64_t mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	uart_puts("  mpidr_el1 = ");
	uart_puthex(mpidr);
	uart_puts("\n");

	const uint64_t phys_base = LOADER_KERNEL_DEST;

	macho_copy_kernel(BEGONIA_RAMDISK_ADDR, phys_base);
	const uint64_t entry = macho_find_entry(BEGONIA_RAMDISK_ADDR);
	uart_puts("[loader] kernel entrypoint ");
	uart_puthex(entry); /* Should be a KVA (link address space). */
	uart_puts("\n");

	build_device_tree();
	report("device tree built");

	fill_boot_args(phys_base, XNU_LINK_ADDRESS);

	report("enabling MMU (identity DRAM + KVA)");
	mmu_boot_enable();

	/* x0 = boot_args (phys, identity-mapped), x1..x3 must be dead. */
	__asm__ volatile(
	    "mov x0, %0\n"
	    "br %1\n"
	    :: "r"(LOADER_BOOTARGS_ADDR), "r"(entry));

	loader_panic("kernel returned");
}
