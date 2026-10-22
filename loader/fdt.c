/*
 * Minimal read-only device tree (FDT) reader.
 *
 * MTK LK jumps to the payload with x0 = physical address of the dtb it just
 * patched (bootargs, atag,videolfb-*, ...). For a stock kernel that dtb is the
 * authoritative description of the current boot, including the framebuffer
 * base LK actually programmed this time. The loader only needs a handful of
 * properties from /chosen, so a full libfdt is not necessary.
 */
#include "loader.h"

#define FDT_MAGIC        0xd00dfeedU
#define FDT_BEGIN_NODE   1U
#define FDT_END_NODE     2U
#define FDT_PROP         3U
#define FDT_NOP          4U
#define FDT_END          9U

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static uint32_t
str_len(const char *s)
{
	uint32_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

int
fdt_valid(uint64_t addr)
{
	const uint8_t *h = (const uint8_t *)addr;
	if (addr == 0 || (addr & 7) != 0) {
		return 0;
	}
	return be32(h) == FDT_MAGIC;
}

/*
 * Find a property inside /chosen. Returns the property data pointer and
 * stores its length, or NULL when the tree or the property is missing.
 */
const uint8_t *
fdt_chosen_prop(uint64_t addr, const char *want, uint32_t *len_out)
{
	const uint8_t *h = (const uint8_t *)addr;

	if (!fdt_valid(addr)) {
		return 0;
	}

	const uint8_t *structure = h + be32(h + 8);
	const uint8_t *strings   = h + be32(h + 12);
	uint32_t       size      = be32(h + 36);

	const uint8_t *p   = structure;
	const uint8_t *end = structure + size;

	int depth = 0;
	int in_chosen = 0;

	while (p + 4 <= end) {
		uint32_t token = be32(p);
		p += 4;

		switch (token) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			p += (str_len(name) + 1 + 3) & ~3U;
			depth++;
			if (depth == 1 && str_eq(name, "chosen")) {
				in_chosen = 1;
			}
			break;
		}
		case FDT_END_NODE:
			if (depth == 1) {
				in_chosen = 0;
			}
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len     = be32(p);
			uint32_t nameoff = be32(p + 4);
			p += 8;
			if (in_chosen && str_eq((const char *)(strings + nameoff), want)) {
				if (len_out) {
					*len_out = len;
				}
				return p;
			}
			p += (len + 3) & ~3U;
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}

	return 0;
}

/* Convenience: big-endian u32 property, 0 when absent. */
uint32_t
fdt_chosen_u32(uint64_t addr, const char *name)
{
	uint32_t len = 0;
	const uint8_t *data = fdt_chosen_prop(addr, name, &len);
	if (!data || len < 4) {
		return 0;
	}
	return be32(data);
}

const char *
fdt_chosen_str(uint64_t addr, const char *name)
{
	uint32_t len = 0;
	const uint8_t *data = fdt_chosen_prop(addr, name, &len);
	if (!data || len == 0) {
		return 0;
	}
	return (const char *)data;
}
