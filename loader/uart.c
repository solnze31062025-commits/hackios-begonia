/* MediaTek 8250-compatible UART console for the stage-2 loader. */
#include "loader.h"

#define UART_RBR 0x00
#define UART_THR 0x00
#define UART_LSR 0x14
#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20

struct uart_ctx {
	uint64_t base;
	uint64_t stride;
	uint8_t  enabled;
};

static struct uart_ctx g_uart = { BEGONIA_UART0_BASE, 4, 0 };

static inline uint32_t
uart_read(uint32_t reg)
{
	return *(volatile uint32_t *)(g_uart.base + reg * g_uart.stride);
}

static inline void
uart_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(g_uart.base + reg * g_uart.stride) = value;
}

void
uart_early_init(void)
{
	/*
	 * Trust the bootloader: MTK LK leaves the debug UART running.
	 * Touching FCR/LCR/IER here can break an ongoing line, so this
	 * driver deliberately only sets its view of the hardware.
	 */
	if (g_uart.base != 0) {
		g_uart.enabled = 1;
	}
}

void
uart_putc(char c)
{
	if (!g_uart.enabled) {
		return;
	}
	/*
	 * Bounded wait: if LK left the UART clock gated, THRE never comes up and
	 * an unbounded spin would freeze the CPU (indistinguishable from a hang
	 * we are trying to debug). Give up once and disable the console.
	 */
	uint32_t spins = 0;
	while (!(uart_read(UART_LSR) & UART_LSR_THRE)) {
		if (++spins > 200000U) {
			g_uart.enabled = 0;
			return;
		}
	}
	uart_write(UART_THR, (uint8_t)c);
}

void
uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n') {
			uart_putc('\r');
		}
		uart_putc(*s++);
	}
}

void
uart_puts_n(const char *s, uint32_t n) /* bounded: segname etc. */
{
	for (uint32_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == 0) {
			break;
		}
		if (c == '\n') {
			uart_putc('\r');
		}
		uart_putc(c);
	}
}

static const char hexdigits[] = "0123456789abcdef";

void
uart_puthex(uint64_t v)
{
	uart_puts("0x");
	for (int i = 0; i < 16; i++) {
		uart_putc(hexdigits[(v >> ((15 - i) * 4)) & 0xF]);
	}
}
