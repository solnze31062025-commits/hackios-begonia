/*
 * Persistent log for the begonia stage-2 loader.
 *
 * The phone has no UART adapter and (right now) no working display path, so
 * the only channel that survives a reboot is DRAM that the *next* boot turns
 * back into a file:
 *
 *   /chosen/ramoops.mem_address = 0x4d010000, mem_size = 0xe0000
 *   /reserved-memory/mblock-13-pstore  = 0x4d010000 .. 0x4d0f0000
 *
 * Linux (Android or TWRP) probes ramoops at boot and exposes the preserved
 * buffers as /sys/fs/pstore/pmsg-ramoops-0, console-ramoops, dmesg-ramoops-*.
 * We cannot know which sub-zone starts where, so the log is written to every
 * 64 KB boundary of the whole 896 KB region: whatever file the kernel
 * produces, its beginning contains our text.
 *
 * Writes are plain DRAM stores with the MMU off - no hardware to initialise,
 * nothing that can block.
 */
#include "loader.h"

#define PLOG_MAX     3072
#define PLOG_SPREAD  0x10000ULL   /* one copy per 64 KB of the pstore region */
#define PLOG_CHUNK   0x4000U      /* at most 16 KB of text per copy */

static char     plog_buf[PLOG_MAX];
static uint32_t plog_len;

static const char plog_header[] = "HACKIOS S2 LOADER LOG\n";

static void
plog_putc(char c)
{
	if (plog_len + 1 < PLOG_MAX) {
		plog_buf[plog_len++] = c;
	}
}

static void
plog_puts(const char *s)
{
	while (*s) {
		plog_putc(*s++);
	}
}

static void
plog_begin(void)
{
	if (plog_len == 0) {
		plog_puts(plog_header);
	}
}

void
plog_flush(void)
{
	uint32_t n = plog_len;
	if (n > PLOG_CHUNK) {
		n = PLOG_CHUNK;
	}

	for (uint64_t off = 0; off + PLOG_CHUNK <= BEGONIA_PSTORE_SIZE;
	     off += PLOG_SPREAD) {
		volatile char *dst = (volatile char *)(BEGONIA_PSTORE_ADDR + off);
		for (uint32_t i = 0; i < n; i++) {
			dst[i] = plog_buf[i];
		}
		dst[n] = '\0';
	}
}

void
plog_reset(void)
{
	plog_len = 0;
	plog_flush();
}

void
plog(const char *msg)
{
	plog_begin();
	plog_puts(msg);
	plog_putc('\n');
	plog_flush();
}

void
plog_hex(const char *tag, uint64_t value)
{
	static const char digits[] = "0123456789abcdef";

	plog_begin();
	plog_puts(tag);
	plog_puts(" = 0x");
	for (int shift = 60; shift >= 0; shift -= 4) {
		plog_putc(digits[(value >> shift) & 0xF]);
	}
	plog_putc('\n');
	plog_flush();
}

/* Same as plog_hex but without the 0x, for small decimal-ish values. */
void
plog_dec(const char *tag, uint64_t value)
{
	static const char digits[] = "0123456789";

	char tmp[24];
	int  n = 0;

	plog_begin();
	plog_puts(tag);
	plog_puts(" = ");
	if (value == 0) {
		plog_putc('0');
	} else {
		while (value && n < (int)sizeof(tmp)) {
			tmp[n++] = digits[value % 10];
			value /= 10;
		}
		while (n--) {
			plog_putc(tmp[n]);
		}
	}
	plog_putc('\n');
	plog_flush();
}
