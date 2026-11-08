ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x8000;
	. = ALIGN(16);
	_stack_top = .;
	. = ALIGN(4096);
	_sec_stacks = .;
	. += 8 * 0x1000;
	/* the boot screen's picture (splash_data.S): after the stacks, so start.S's adr still reach them */
	. = ALIGN(16);
	.splash : { *(.splash*) }
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
