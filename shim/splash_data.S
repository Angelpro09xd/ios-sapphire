/* The boot screen's picture (gen_splash.py), in its own section behind the stacks (shim.ld). */
	.section .splash, "a"
	.balign 16
	.globl splash_bin
splash_bin:
	.incbin "out/splash.bin"
