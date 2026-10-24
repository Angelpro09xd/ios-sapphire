/* cpuspeed: a chain of dependent adds (one a cycle on a Cortex-A73): the core's clock */
#include <stdio.h>
#include <mach/mach_time.h>

int
main(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	for (int r = 0; r < 5; r++) {
		uint64_t x = 1, n = 400000000;
		uint64_t t0 = mach_absolute_time();
		for (uint64_t i = 0; i < n; i += 8) {
			__asm__ volatile("add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			                 "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1" : "+r"(x));
		}
		double s = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
		printf("%.0f MHz\n", n / s / 1e6);
	}
	return 0;
}
