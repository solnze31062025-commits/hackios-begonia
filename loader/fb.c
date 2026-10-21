/*
 * Framebuffer bring-up for the begonia stage-2 loader.
 *
 * Facts measured on the device (TWRP 21.10.2026, DT + /proc/iomem):
 *
 *   /chosen/atag,videolfb-fb_base_l = 0x7e605000   <- LK framebuffer base
 *   /chosen/atag,videolfb-fb_base_h = 0
 *   /chosen/atag,videolfb-vramSize  = 0x017e8000
 *   /reserved-memory/mblock-15-framebuffer (mediatek,framebuffer)
 *        reg = 0x7d9b0000 .. 0x7fc00000
 *   (right after it, mblock-7-tee-secmem at 0x7fc00000..0x7fe00000 is secure
 *    memory and must never be touched from the non-secure world)
 *   /sys/class/graphics/fb0: virtual_size=1088x4680 stride=4352 bpp=32
 *          (1080x2340 panel, 1088 px stride, double buffered)
 *
 * Because the live buffer may be any of the frame buffers inside the
 * reserved framebuffer area, the loader paints the WHOLE reserved window
 * and draws the same label at three candidate offsets (A/B/C). The letter
 * that becomes visible tells us which buffer the display engine is really
 * reading, without any serial console.
 *
 * All code here runs with the MMU OFF (before mmu_boot_enable), so the
 * addresses above are plain physical addresses.
 */
#include "loader.h"
#include "font4x6.h"

#define FB_W        BEGONIA_FB_WIDTH
#define FB_H        BEGONIA_FB_HEIGHT
#define FB_STRIDE   BEGONIA_FB_STRIDE_PX

#define SCALE  8  /* 4x6 font cell -> 32x48 px */
#define CELL_W (FONT4X6_COLS * SCALE)
#define CELL_H (FONT4X6_ROWS * SCALE)

/* Colors (ARGB, opaque) */
#define C_BG      0xFF1A1A1A
#define C_TEXT    0xFFF2F2F2
#define C_ACCENT  0xFF2D7FF9
#define C_OK      0xFF0A7A0A
#define C_PANIC   0xFFAA0000
#define C_WARN    0xFF050505 /* black label over probe colors */

/* Candidate live buffers inside the reserved framebuffer window. */
static const uint64_t fb_candidate_bases[] = {
	BEGONIA_FB_WINDOW_BASE,                       /* A: window start      */
	BEGONIA_FB_DEFAULT,                           /* B: atag,videolfb     */
	BEGONIA_FB_DEFAULT + BEGONIA_FB_BUFFER_SIZE,  /* C: second buffer     */
};

#define FB_N_CANDIDATES (sizeof(fb_candidate_bases) / sizeof(fb_candidate_bases[0]))

/* ------------------------------------------------------------------ */

static char
uppercase(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

/* Fill the whole reserved framebuffer window: guarantees that whatever
 * buffer the display engine is scanning out gets our color. */
static void
fb_fill_window(uint32_t color)
{
	volatile uint32_t *p = (volatile uint32_t *)BEGONIA_FB_WINDOW_BASE;
	uint64_t n = BEGONIA_FB_WINDOW_SIZE / 4;
	for (uint64_t i = 0; i < n; i++) {
		p[i] = color;
	}
}

/* Fill one screen-sized buffer (with the real stride). */
static void
fb_clear(uint64_t addr, uint32_t color)
{
	volatile uint32_t *p = (volatile uint32_t *)addr;
	for (uint32_t y = 0; y < FB_H; y++) {
		for (uint32_t x = 0; x < FB_STRIDE; x++) {
			p[(uint64_t)y * FB_STRIDE + x] = color;
		}
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
			p[(uint64_t)py * FB_STRIDE + px] = color;
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
 * First thing the loader does: paint the whole framebuffer window blue and
 * label the three candidate buffer offsets. No UART, no other dependencies.
 * Blue screen = our code executes and the window address is right.
 * Letter A/B/C tells which offset the display engine actually scans out.
 */
void
fb_instant_paint(void)
{
	fb_fill_window(C_ACCENT);

	fb_text(fb_candidate_bases[0], 60, 300, "A", 0xFFFFFFFF);
	fb_text(fb_candidate_bases[1], 60, 300, "B", 0xFFFFFFFF);
	fb_text(fb_candidate_bases[2], 60, 300, "C", 0xFFFFFFFF);
}

/*
 * Boot menu drawn at the (assumed) framebuffer base.
 * Buttons are VISUAL for now: there is no input yet on this platform;
 * default action ("RUN XNU") auto-starts after the countdown.
 */
void
fb_draw_menu(int countdown_secs)
{
	uint64_t fb = BEGONIA_FB_DEFAULT;
	char buf[4];

	fb_clear(fb, C_BG);

	fb_text(fb, 40, 60, "HACKIOS", C_ACCENT);
	fb_text(fb, 40 + 8 * (CELL_W + SCALE), 60, "BEGONIA", C_TEXT);
	fb_text(fb, 40, 60 + CELL_H + SCALE, "BEGONIA BRING-UP MENU V0.4", C_TEXT);

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
 * Terminal screen: paint the window, print the same two lines at all three
 * candidate buffer offsets (prefixed A/B/C), then park the CPU.
 */
void
fb_halt_screen(uint32_t bg, const char *line1, const char *line2)
{
	static const char *labels[] = { "A", "B", "C" };

	fb_fill_window(bg);

	for (uint32_t i = 0; i < FB_N_CANDIDATES; i++) {
		uint64_t fb = fb_candidate_bases[i];
		fb_text(fb, 60, 200, labels[i], 0xFFFFFFFF);
		fb_text(fb, 60, 300, line1, 0xFFFFFFFF);
		fb_text(fb, 60, 300 + CELL_H + SCALE, line2, 0xFFFFFFFF);
	}

	for (;;) {
		__asm__ volatile("wfe");
	}
}

void
fb_panic(const char *msg)
{
	fb_halt_screen(C_PANIC, "LOADER PANIC:", msg);
}

void
fb_wait_ms(uint32_t ms)
{
	wait_ms(ms);
}

/* Kept for the "unknown FB" bring-up path; not used once the window is known. */
void
fb_probe_cycle(void)
{
	fb_fill_window(C_WARN);
	fb_text(BEGONIA_FB_DEFAULT, 60, 300, "PROBE MODE", 0xFFFFFFFF);
	wait_ms(400);
}
