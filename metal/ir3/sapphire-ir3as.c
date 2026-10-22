/*
 * sapphire-ir3as: assemble ir3 (Adreno shader ISA) for the a610 without a GPU, using Mesa's own
 * assembler and encoder (src/freedreno/ir3, isa). Writes the binary and prints the disassembly
 * and the variant's register use, which the 3D state of a draw has to be told about.
 *
 *   sapphire-ir3as IN.asm OUT.bin
 *
 * The a610's identity is the one the kernel reads off the GPU: gpu_id 610, chip id 0x06010001.
 * ir3_compiler_create() keeps the fd_device but never uses it, so NULL is fine here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/freedreno_dev_info.h"
#include "ir3/ir3_assembler.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s IN.asm OUT.bin\n", argv[0]);
		return 2;
	}
	static const struct fd_dev_id id = { .gpu_id = 610, .chip_id = 0x06010001 };
	const struct fd_dev_info *info = fd_dev_info_raw(&id);
	if (info == NULL) {
		fprintf(stderr, "no device info for a610\n");
		return 1;
	}
	struct ir3_compiler_options opts = { 0 };
	struct ir3_compiler *c = ir3_compiler_create(NULL, &id, info, &opts);
	struct ir3_kernel_info ki = { 0 };
	FILE *in = fopen(argv[1], "r");
	if (in == NULL) {
		perror(argv[1]);
		return 1;
	}
	struct ir3_shader *sh = ir3_parse_asm(c, &ki, in);
	if (sh == NULL) {
		fprintf(stderr, "%s: assembly failed\n", argv[1]);
		return 1;
	}
	struct ir3_shader_variant *v = sh->variants;
	FILE *out = fopen(argv[2], "wb");
	fwrite(v->bin, 1, v->info.size, out);
	fclose(out);
	printf("# %s: %u bytes, %u instructions, max_reg %d, max_half_reg %d, max_const %d, instrlen %u\n",
	    argv[1], v->info.size, v->info.instrs_count, v->info.max_reg, v->info.max_half_reg,
	    v->info.max_const, v->instrlen);
	printf("# for the SP registers: constlen %u (vec4), mergedregs %d, double_threadsize %d, branchstack %u, "
	    "localid regid %u, instr_cache_size %u, has_lpac %d\n", v->constlen, v->mergedregs, v->info.double_threadsize,
	    ir3_shader_branchstack_hw(v), ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID),
	    info->props.instr_cache_size, info->props.has_lpac);
	ir3_shader_disasm(v, (uint32_t *)v->bin, stdout);
	return 0;
}
