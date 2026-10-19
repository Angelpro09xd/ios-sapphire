#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Find the DDR bank (a reg entry of a top-level memory node, as ABL fills
 * them in) that contains addr. Returns 1 with the bank end in *end, 0 if no
 * bank holds addr, -1 if the FDT has no usable memory reg at all.
 */
int fdt_mem_bank_end(uintptr_t fdt, uint64_t addr, uint64_t *end);
