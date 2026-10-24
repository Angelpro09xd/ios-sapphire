/* membw: memcpy bandwidth of 64 MB buffers (the DDR's clock shows in it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>

int
main(void)
{
	size_t n = 64u << 20;
	char *a = malloc(n), *b = malloc(n);
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	memset(a, 1, n);
	memset(b, 2, n);
	for (int r = 0; r < 3; r++) {
		uint64_t t0 = mach_absolute_time();
		for (int i = 0; i < 8; i++) {
			memcpy(i & 1 ? a : b, i & 1 ? b : a, n);
			__asm__ volatile("" : : "r"(a), "r"(b) : "memory");
		}
		double s = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
		printf("memcpy: %.2f GB/s (read + write %.2f GB/s)\n", 8.0 * n / s / 1e9, 16.0 * n / s / 1e9);
	}
	return 0;
}
