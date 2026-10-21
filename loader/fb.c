/*
 * Framebuffer bring-up for the begonia stage-2 loader.
 *
 * MTK LK leaves the display (MIPI panel) initialized; the display RDMA
 * engine keeps streaming pixels from RAM. If we write pixels at the
 * framebuffer address, they appear live on the screen — no UART needed.
 *
 * The FB base address on this board is TBD, so the first boot runs a
 * PROBE CYCLE: it paints each candidate address with a distinctive
 * solid color + a big label. Whatever color number the user reports
 * seeing (or films) — that candidate is the real framebuffer. Later
 * builds fix BEGONIA_FB_DEFAULT to it and get a real boot menu.
 *
 * All code here runs with the MMU OFF (before mmu_boot_enable).
 */
#include "loader.h"
#include "font4x6.h"

#define FB_W   1080
#define FB_H   2340
#define FB_PX  4 /* assume 32bpp */

#define SCALE  8  /* 4x6 font cell -> 32x48 px */
#define CELL_W (FONT4X6_COLS * SCALE)
#define CELL_H (FONT4X6_ROWS * SCALE)

/* Colors (ARGB, opaque) */
#define C_BG      0xFF1A1A1A
#define C_TEXT    0xFFF2F2F2
#define C_ACCENT  0xFF2D7FF9
#define C_WARN    0xFF050505 /* black label over probe colors */

/* Palette for the probe cycle: candidate #i paints color i. */
static const uint32_t fb_probe_colors[] = {
	0xFFFF0000, /*  1: red     */
	0xFF00CC00, /*  2: green   */
	0xFF0000FF, /*  3: blue    */
	0xFFFFFF00, /*  4: yellow  */
	0xFFFF00FF, /*  5: magenta */
	0xFF00FFFF, /*  6: cyan    */
	0xFFFF6600, /*  7: orange  */
	0xFF8800FF, /*  8: purple  */
	0xFFFFFFFF, /*  9: white   */
	0xFF30C030, /* 10: lime    */
	0xFFFF3399, /* 11: pink    */
	0xFF009999, /* 12: teal    */
	0xFF996633, /* 13: brown   */
	0xFF6666FF, /* 14: violet  */
	0xFFFFFF66, /* 15: cream   */
};

static const uint64_t fb_probe_addrs[] = {
	0x60000000ULL,
	0x62000000ULL,
	0x64000000ULL,
	0x68000000ULL,
	0x70000000ULL,
	0x74000000ULL,
	0x78000000ULL,
	0x7C000000ULL,
	0x80000000ULL,
	0x88000000ULL,
	0x90000000ULL,
	0xA0000000ULL,
	0xA8000000ULL,
	0xB0000000ULL,
	0xB8000000ULL,
};

#define FB_N_CANDIDATES (sizeof(fb_probe_addrs) / sizeof(fb_probe_addrs[0]))

_Static_assert(sizeof(fb_probe_colors) / sizeof(fb_probe_colors[0]) ==
    sizeof(fb_probe_addrs) / sizeof(fb_probe_addrs[0]),
    "probe palette and probe address list must have the same length");

/* ------------------------------------------------------------------ */

static char
uppercase(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static void
fb_fill(uint64_t addr, uint32_t color)
{
	volatile uint32_t *p = (volatile uint32_t *)addr;
	for (uint32_t i = 0; i < (FB_W * FB_H); i++) {
		p[i] = color;
	}
}

static void
fb_rect(uint64_t addr, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
	volatile uint32_t *p = (volatile uint32_t *)addr;
	for (uint32_t row = 0; row < h; row++) {
		uint32_t py = y + row;
		if (py >= FB_H) {
			break;
		}
		for (uint32_t col = 0; col < w; col++) {
			uint32_t px = x + col;
			if (px >= FB_W) {
				break;
			}
			p[py * FB_W + px] = color;
		}
	}
}

static int
glyph_index(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= '0' && c <= '9') {
		return 26 + (c - '0');
	}
	switch (c) {
	case ' ':  return 36;
	case ':':  return 37;
	case '>':  return 38;
	case '<':  return 39;
	case '=':  return 40;
	case '(':  return 41;
	case ')':  return 42;
	case '.':  return 43;
	case '-':  return 44;
	case '/':  return 45;
	case '!':  return 46;
	default:   return 36; /* space */
	}
}

static void
fb_char(uint64_t addr, uint32_t x, uint32_t y, char c, uint32_t color)
{
	int gi = glyph_index(c);
	if (gi < 0) {
		return;
	}
	const unsigned char *g = font4x6_glyphs[gi];
	for (uint32_t row = 0; row < FONT4X6_ROWS; row++) {
		unsigned char nib = (row % 2 == 0) ? (g[row / 2] >> 4) : (g[row / 2] & 0xF);
		for (uint32_t col = 0; col < FONT4X6_COLS; col++) {
			if (!((nib >> (3 - col)) & 1)) {
				continue;
			}
			fb_rect(addr,
			    x + col * SCALE, y + row * SCALE,
			    SCALE, SCALE, color);
		}
	}
}

static void
fb_text(uint64_t addr, uint32_t x, uint32_t y, const char *s, uint32_t color)
{
	while (*s) {
		fb_char(addr, x, y, uppercase(*s), color);
		x += CELL_W + SCALE; /* 1-cell spacing */
		s++;
	}
}

/* Convert a small number to a decimal string ("FB PROBE 5"). */
static void
put_num(char *dst, uint32_t v)
{
	char tmp[12];
	int n = 0;
	if (v == 0) {
		dst[0] = '0';
		dst[1] = 0;
		return;
	}
	while (v > 0) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	for (int i = 0; i < n; i++) {
		dst[i] = tmp[n - 1 - i];
	}
	dst[n] = 0;
}

/*
 * Delay. Prefer the generic timer (CNTPCT); LK usually leaves it ticking,
 * but CNTFRQ may be 0 if only EL2 configured the timer, so fall back to a
 * plain busy loop. Exact timing does not matter for bring-up: only that a
 * color stays on screen long enough for a human/video to see it.
 */
static void
wait_ms(uint32_t ms)
{
	uint64_t frq = 0;
	__asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(frq));

	if (frq >= 1000000ULL && frq <= 200000000ULL) {
		uint64_t start;
		__asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
		uint64_t end = start + (frq / 1000ULL) * (uint64_t)ms;
		for (;;) {
			uint64_t now;
			__asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
			if (now >= end) {
				break;
			}
		}
		return;
	}

	/* Conservative fallback: ~100k iterations per "ms" at ~1.5 GHz. */
	volatile uint64_t n = (uint64_t)ms * 100000ULL;
	while (n-- > 0) {
	}
}

/* ------------------------------------------------------------------ */

/*
 * First thing the loader does: paint the whole screen. No UART, no waits,
 * no other dependencies — if the FB address is right the user sees a blue
 * screen with a label immediately, which proves our code executed.
 */
void
fb_instant_paint(void)
{
	uint64_t fb = BEGONIA_FB_DEFAULT;

	fb_fill(fb, C_ACCENT);
	fb_text(fb, 40, 40, "HACKIOS LOADER", 0xFFFFFFFF);
	fb_text(fb, 40, 40 + CELL_H + 2 * SCALE, "V0.3 BEGONIA", 0xFFFFFFFF);
}

void
fb_probe_cycle(void)
{
	char label[20];

	for (uint32_t i = 0; i < FB_N_CANDIDATES; i++) {
		fb_fill(fb_probe_addrs[i], fb_probe_colors[i]);
		/* Big black label on top of the solid color. */
		fb_text(fb_probe_addrs[i], 40, 40, "FB#", C_WARN);
		fb_text(fb_probe_addrs[i], 40 + 5 * (CELL_W + SCALE), 40, "PROBE ", C_WARN);
		char *p = label;
		*p++ = '#';
		put_num(p, i + 1);
		fb_text(fb_probe_addrs[i], 40, 40 + CELL_H + 3 * SCALE, label, C_WARN);
		wait_ms(400);
	}
}

/*
 * Boot menu drawn at the (assumed) framebuffer address.
 * Buttons are VISUAL for now: there is no input yet on this platform;
 * default action ("RUN XNU") auto-starts after the countdown.
 */
void
fb_draw_menu(int countdown_secs)
{
	uint64_t fb = BEGONIA_FB_DEFAULT;
	char buf[4];

	fb_fill(fb, C_BG);

	fb_text(fb, 40, 60, "HACKIOS", C_ACCENT);
	fb_text(fb, 40 + 8 * (CELL_W + SCALE), 60, "BEGONIA", C_TEXT);
	fb_text(fb, 40, 60 + CELL_H + SCALE, "BEGONIA BRING-UP MENU V0.2", C_TEXT);

	/* Button 1 (selected) */
	fb_rect(fb, 40, 700, 700, 120, C_ACCENT);
	fb_text(fb, 60, 730, "RUN XNU (KERNEL)", 0xFFFFFFFF);
	/* Button 2 (placeholder, inactive) */
	fb_rect(fb, 40, 850, 700, 120, 0xFF2A2A2A);
	fb_text(fb, 60, 880, "UART LOG", C_TEXT);

	for (int n = countdown_secs; n > 0; n--) {
		buf[0] = (char)('0' + n);
		buf[1] = 0;
		fb_text(fb, 40, 60 + 2 * (CELL_H + SCALE), "AUTO IN", C_TEXT);
		fb_text(fb, 40 + 9 * (CELL_W + SCALE), 60 + 2 * (CELL_H + SCALE), buf, C_ACCENT);
		fb_text(fb, 40, 60 + 3 * (CELL_H + SCALE), "NO INPUT YET: VOLUME KEYS TBD", C_TEXT);
		wait_ms(1000);
	}
}

/*
 * Terminal screen: fill with a color, print two lines, then park the CPU.
 * Used for both panic (red) and "visual stage completed" (green).
 */
void
fb_halt_screen(uint32_t bg, const char *line1, const char *line2)
{
	uint64_t fb = BEGONIA_FB_DEFAULT;

	fb_fill(fb, bg);
	fb_text(fb, 40, 400, line1, 0xFFFFFFFF);
	fb_text(fb, 40, 400 + CELL_H + SCALE, line2, 0xFFFFFFFF);
	for (;;) {
		__asm__ volatile("wfe");
	}
}

void
fb_panic(const char *msg)
{
	fb_halt_screen(0xFFAA0000, "LOADER PANIC:", msg);
}
