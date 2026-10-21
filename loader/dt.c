/*
 * Apple-format flattened device tree writer (byte-accurate).
 *
 * Format (pexpert/pexpert/device_tree.h):
 *   node    : { uint32 nProperties; uint32 nChildren; }   (little-endian)
 *             followed by nProperties <property> entries,
 *             then nChildren child nodes (each full node recursively).
 *   property: { char name[32]; uint32 length; <value, padded to 4>; }
 *
 * The kernel parser (SecureDT*) reads this directly; there is no separate
 * header/magic — boot_args.deviceTreeP points at the root node.
 */
#include "loader.h"

#define DT_MAX_DEPTH  4
#define DT_MAX_NODES  16

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

static int
dt_can_write(struct dt_writer *w, size_t n)
{
	return (w->pos + n) <= w->size;
}

static void
dt_put(struct dt_writer *w, const void *p, size_t n)
{
	const uint8_t *s = p;
	for (size_t i = 0; i < n; i++) {
		if (dt_can_write(w, 1)) {
			w->buf[w->pos + i] = s[i];
		}
	}
	w->pos += n;
}

static void
dt_put_le32(struct dt_writer *w, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	dt_put(w, b, 4);
}

void
dt_init(struct dt_writer *w, void *buf, size_t size)
{
	w->buf = buf;
	w->size = size;
	w->pos = 0;
	w->depth = 0;
}

void
dt_node_open(struct dt_writer *w, const char *name)
{
	if (w->depth >= DT_MAX_DEPTH) {
		return;
	}
	/* emit header: nProperties = 0, nChildren = 0, patched on close */
	w->nodes[w->depth].nprops_off = w->pos;
	dt_put_le32(w, 0);
	w->nodes[w->depth].nchild_off = w->pos;
	dt_put_le32(w, 0);
	w->nodes[w->depth].nprops = 0;
	w->nodes[w->depth].nchildren = 0;
	w->depth++;

	if (w->depth > 1) {
		/* parent child count bump */
		if (dt_can_write(w, 0)) {
			uint32_t *parent = (uint32_t *)(void *)&w->buf[w->nodes[w->depth - 2].nchild_off];
			*parent = w->nodes[w->depth - 2].nchildren = w->nodes[w->depth - 2].nchildren + 1;
		}
	}

	/* node "name" property is mandatory: pexpert looks it up first */
	dt_prop_str(w, "name", name);
}

void
dt_node_close(struct dt_writer *w)
{
	if (w->depth == 0) {
		return;
	}
	w->depth--;
	/* patch counts */
	uint32_t *nprops = (uint32_t *)(void *)&w->buf[w->nodes[w->depth].nprops_off];
	*nprops = w->nodes[w->depth].nprops;
	uint32_t *nchild = (uint32_t *)(void *)&w->buf[w->nodes[w->depth].nchild_off];
	*nchild = w->nodes[w->depth].nchildren;
}

static uint32_t
dt_strlen(const char *s)
{
	uint32_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	if (w->depth == 0) {
		return;
	}
	char pname[32];
	for (int i = 0; i < 32; i++) {
		pname[i] = 0;
	}
	for (int i = 0; name[i] && i < 31; i++) {
		pname[i] = name[i];
	}
	uint32_t vlen = dt_strlen(value) + 1;

	dt_put(w, pname, 32);
	dt_put_le32(w, vlen);
	dt_put(w, value, dt_strlen(value));   /* value without terminator */
	uint8_t z = 0;
	dt_put(w, &z, 1);                     /* add terminator */
	for (uint32_t i = 0; i < ((vlen + 3) & ~3U) - vlen; i++) {
		dt_put(w, &z, 1);             /* pad to 4 */
	}
	w->nodes[w->depth - 1].nprops++;
}

void
dt_prop_bytes(struct dt_writer *w, const char *name, const uint8_t *value, uint32_t len)
{
	if (w->depth == 0) {
		return;
	}
	char pname[32];
	for (int i = 0; i < 32; i++) {
		pname[i] = 0;
	}
	for (int i = 0; name[i] && i < 31; i++) {
		pname[i] = name[i];
	}
	dt_put(w, pname, 32);
	dt_put_le32(w, len);
	dt_put(w, value, len);
	uint8_t z = 0;
	for (uint32_t i = 0; i < ((len + 3) & ~3U) - len; i++) {
		dt_put(w, &z, 1);
	}
	w->nodes[w->depth - 1].nprops++;
}

void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	uint8_t b[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };
	dt_prop_bytes(w, name, b, 4);
}

void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)(value >> (56 - 8 * i));
	}
	dt_prop_bytes(w, name, b, 8);
}

/*
 *  reg = { base (BE u64), size (BE u64) }  — byte strings, not ints.
 *  Apple DT uses two BE u64 for arm64 reg entries; this matches what
 *  pe_serial.c/pl011 setup/pe_identify_machine.c read.
 */
void
dt_prop_reg(struct dt_writer *w, uint64_t base, uint64_t size)
{
	uint8_t b[16];
	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)(base >> (56 - 8 * i));
		b[8 + i] = (uint8_t)(size >> (56 - 8 * i));
	}
	dt_prop_bytes(w, "reg", b, 16);
}

void
dt_prop_stringlist(struct dt_writer *w, const char *name, const char **strings, int count)
{
	if (w->depth == 0) {
		return;
	}
	uint32_t total = 1;
	for (int i = 0; i < count; i++) {
		total += dt_strlen(strings[i]) + 1;
	}
	uint8_t tmp[64];
	uint32_t p = 0;
	for (int i = 0; i < count; i++) {
		for (uint32_t j = 0; j < dt_strlen(strings[i]) + 1; j++) {
			tmp[p + j] = strings[i][j];
		}
		p += dt_strlen(strings[i]) + 1;
	}
	tmp[p] = 0;
	dt_prop_bytes(w, name, tmp, total);
}
