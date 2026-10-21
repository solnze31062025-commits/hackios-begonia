/*
 * HackiOS stage-2 loader for Xiaomi Redmi Note 8 Pro (begonia / Helio G90T).
 *
 * Runs after MTK LK jumps to the payload in the "kernel" slot of boot.img.
 * Responsibilities (mirrors what Apple's iBoot does for XNU):
 *   1. Early console on the MediaTek 8250-compatible debug UART.
 *   2. Locate the XNU kernel Mach-O (LK has loaded it in the "ramdisk"
 *      position), copy each LC_SEGMENT_64 to its final physical location
 *      (physBase + vmaddr - firstSegment.vmaddr), exactly like Apple's
 *      copy_by_vm_offset flow.
 *   3. Synthesize an Apple-format flattened device tree for the kernel and
 *      build the boot_args structure (Revision/Version 2 ABI).
 *   4. Enable the MMU with:
 *         - V=P identity map of DRAM (1GB blocks, 4K granule)
 *         - KVA map at VM_KERNEL_LINK_ADDRESS (1GB) -> DRAM
 *   5. Jump to the kernel entry point with x0 = &boot_args.
 *
 * herrre lines marked TBD must be validated against the actual board
 * bring-up logs; they carry the addresses that LK uses on this board.
 */

#ifndef LOADER_LOADER_H
#define LOADER_LOADER_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t addr64_t;

/*
 * Board configuration for begonia. All addresses TBD-until-verified are
 * checked during serial bring-up (the loader prints everything it assumes).
 */
#define BEGONIA_UART0_BASE       0x11002000ULL  /* mt-uart #0 */
#define BEGONIA_UART_BASE_STRIDE 4              /* reg-shift = 2 */
#define BEGONIA_UART_CLOCK       26000000ULL    /* baud clock assumed by LK config */
#define BEGONIA_UART_BAUD        115200

#define BEGONIA_RAM_BASE         0x40000000ULL  /* first DRAM bank */

/* Where LK drops the boot.img payloads (verified against the stock boot.img
 * header of the device: kernel_addr 0x40080000, ramdisk_addr 0x47c80000). */
#define BEGONIA_KERNEL_SLOT_ADDR 0x40080000ULL  /* payload in "kernel" slot (loader) */
#define BEGONIA_RAMDISK_ADDR     0x47C80000ULL  /* payload in "ramdisk" slot (XNU Mach-O) */
#define BEGONIA_TAGS_ADDR        0x4BC80000ULL  /* boot.img tags_addr from the stock header */

/* XNU arm64 unslid link address (VM_KERNEL_LINK_ADDRESS). */
#define XNU_LINK_ADDRESS         0xFFFFFE0007004000ULL
/* 1GB-aligned base used for the KVA block mapping. */
#define XNU_KVA_BLOCK            (XNU_LINK_ADDRESS & ~0x3FFFFFFFULL)

/*
 * Memory layout seen by the kernel:
 *   physBase                = 0x43004000 (unslid kernel)
 *   DT blob                 = 0x44000000
 *   boot_args               = 0x44200000
 *   topOfKernelData         = 0x44400000
 *   mappable DRAM for XNU   = [0x43004000, 0x43004000 + BEGONIA_RAM_SIZE)
 *   (bank0 ends at 0xC0000000; keep margin for LK/reserved carve-outs)
 */
#define LOADER_KERNEL_DEST       0x43004000ULL
#define LOADER_DT_ADDR           0x44000000ULL
#define LOADER_BOOTARGS_ADDR     0x44200000ULL
#define LOADER_TOP_KERNEL_DATA   0x44400000ULL
#define BEGONIA_RAM_SIZE         0x7B000000ULL
#define LOADER_DT_MAX_SIZE       0x20000ULL /* 128 KB, lives at LOADER_DT_ADDR */

/*
 * Framebuffer, measured from the device's device tree (TWRP):
 *   /chosen/atag,videolfb-fb_base_l = 0x7e605000
 *   /chosen/atag,videolfb-vramSize  = 0x017e8000
 *   /reserved-memory/mblock-15-framebuffer (mediatek,framebuffer)
 *        reg = 0x7d9b0000 .. 0x7fc00000
 *   /sys/class/graphics/fb0: 1088x4680, stride 4352, 32 bpp
 *        -> panel 1080x2340, stride 1088 px, double buffered
 *
 * We paint the whole reserved window so that the live buffer is covered no
 * matter which of the frame buffers the display engine scans out.
 */
#define BEGONIA_FB_WINDOW_BASE   0x7D9B0000ULL
#define BEGONIA_FB_WINDOW_SIZE   0x02250000ULL  /* exactly mblock-15: 0x7d9b0000..0x7fc00000 */
#define BEGONIA_FB_DEFAULT       0x7E605000ULL  /* atag,videolfb-fb_base */
#define BEGONIA_FB_STRIDE_PX     1088
#define BEGONIA_FB_WIDTH         1080
#define BEGONIA_FB_HEIGHT        2340
#define BEGONIA_FB_BUFFER_SIZE   0x9B6400ULL    /* 1088 * 2340 * 4 */

/* Set to 1 only while the FB address is still unknown: paints candidate
 * addresses in a fast cycle. Keep it off for normal operation. */
#define BEGONIA_FB_PROBE_ENABLE  0

/*
 * Bring-up isolation switch.
 *   1 = visual stage only: paint, probe, menu, then park on a status screen.
 *       No Mach-O parsing, no kernel copy, no MMU jump. Used while the
 *       framebuffer address is still unknown, so a wrong guess can never be
 *       confused with a broken kernel path.
 *   0 = full boot flow (kernel copy + DT + boot_args + MMU + jump).
 */
#define LOADER_VISUAL_ONLY       1

/*
 * Proof-of-execution mode: paint the screen, wait a few seconds and then
 * power the SoC off through PSCI (SMC). If the phone stops rebooting (goes
 * off or freezes) instead of looping, our code definitely executed - even
 * with a completely dark screen.
 */
#define LOADER_EXEC_PROOF        1

/* CPU topology (2x A76 + 6x A55, temporary assumption of 2 clusters). */
#define BEGONIA_CPU_CLUSTER_0    0
#define BEGONIA_CPU_CLUSTER_1    1

/* boot_args ABI (pexpert/pexpert/arm64/boot.h). */
struct boot_args {
	uint16_t Revision;
	uint16_t Version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct {
		uint64_t v_baseAddr;
		uint64_t v_display;
		uint64_t v_rowBytes;
		uint64_t v_width;
		uint64_t v_height;
		uint64_t v_depth;
	} Video;
	uint32_t machineType;
	uint32_t _pad0;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	uint32_t _pad1;
	char     CommandLine[1024];
	uint64_t bootFlags;
	uint64_t memSizeActual;
} __attribute__((packed));

#define BOOT_ARGS_VERSION  2
#define BOOT_ARGS_REVISION 2

struct mach_header64 {
	uint32_t magic;
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t filetype;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	uint32_t flags;
	uint32_t reserved;
};

#define MH_MAGIC_64      0xFEEDFACF
#define MH_EXECUTE       0x2
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD   0x5

struct segment_command64 {
	uint32_t cmd;
	uint32_t cmdsize;
	char     segname[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	uint32_t maxprot;
	uint32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};

struct thread_state {
	uint64_t pc;
	uint64_t sp;
};

/* dt.c — Apple-format flattened device tree writer */
#define DT_MAX_DEPTH  4

struct dt_writer {
	uint8_t *buf;
	size_t   size;
	size_t   pos;

	/* current node bookkeeping */
	struct {
		size_t nprops_off; /* offset of nProperties word */
		size_t nchild_off; /* offset of nChildren word */
		uint32_t nprops;
		uint32_t nchildren;
	} nodes[DT_MAX_DEPTH];
	int depth;
};

void dt_init(struct dt_writer *w, void *buf, size_t size);
void dt_node_open(struct dt_writer *writer, const char *name);
void dt_node_close(struct dt_writer *writer);
void dt_prop_str(struct dt_writer *writer, const char *name, const char *value);
void dt_prop_bytes(struct dt_writer *writer, const char *name, const uint8_t *value, uint32_t len);
void dt_prop_u32(struct dt_writer *writer, const char *name, uint32_t value);
void dt_prop_u64(struct dt_writer *writer, const char *name, uint64_t value);
void dt_prop_reg(struct dt_writer *writer, uint64_t base, uint64_t size);
void dt_prop_stringlist(struct dt_writer *writer, const char *name, const char **strings, int count);

/* uart.c */
void uart_early_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puts_n(const char *s, uint32_t n);
void uart_puthex(uint64_t v);

/* fb.c */
void fb_instant_paint(void);
void fb_probe_cycle(void);
void fb_draw_menu(int countdown_secs);
void fb_status_screen(uint32_t bg, const char *line1, const char *line2);
void fb_halt_screen(uint32_t bg, const char *line1, const char *line2)
    __attribute__((noreturn));
void fb_panic(const char *msg) __attribute__((noreturn));
void fb_wait_ms(uint32_t ms);

/* main.c */
void loader_panic(const char *msg);
void loader_fault(uint64_t esr, uint64_t sp) __attribute__((noreturn));

#endif /* LOADER_LOADER_H */
