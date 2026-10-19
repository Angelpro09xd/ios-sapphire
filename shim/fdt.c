/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static int is_memory_node(const char *name)
{
	static const char m[] = "memory";
	for (int i = 0; i < 6; i++)
		if (name[i] != m[i])
			return 0;
	return name[6] == 0 || name[6] == '@';
}

static uint64_t read_cells(uintptr_t a, uint32_t cells)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < cells; i++)
		v = v << 32 | be32(a + 4 * i);
	return v;
}

int fdt_mem_bank_end(uintptr_t fdt, uint64_t addr, uint64_t *end)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return -1;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	/* Root cell sizes come before any child node (properties first). */
	uint32_t ac = 2, sc = 2;
	int depth = 0, in_mem = 0, banks = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_mem = depth == 2 && is_memory_node(name);
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			in_mem = 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *pname = (const char *)(strings + nameoff);
			if (depth == 1 && len == 4) {
				if (streq(pname, "#address-cells"))
					ac = be32(val);
				else if (streq(pname, "#size-cells"))
					sc = be32(val);
			} else if (in_mem && streq(pname, "reg") && ac && ac <= 2 && sc && sc <= 2) {
				uint32_t entry = 4 * (ac + sc);
				for (uint32_t off = 0; off + entry <= len; off += entry) {
					uint64_t base = read_cells(val + off, ac);
					uint64_t size = read_cells(val + off + 4 * ac, sc);
					if (!size)
						continue;
					banks++;
					if (addr >= base && addr - base < size) {
						*end = base + size;
						return 1;
					}
				}
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return banks ? 0 : -1;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}
