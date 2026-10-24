/* membw: memcpy bandwidth of 64 MB buffers (the DDR's clock shows in it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>

int
main(int argc, char **argv)
{
	size_t n = (argc > 1 ? (size_t)atoi(argv[1]) : 65536) << 10;   /* KB */
	char *a = malloc(n), *b = malloc(n);
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	memset(a, 1, n);
	memset(b, 2, n);
	for (int r = 0; r < 3; r++) {
		uint64_t t0 = mach_absolute_time();
		int reps = (int)((512ull << 20) / n);
		for (int i = 0; i < reps; i++) {
			memcpy(i & 1 ? a : b, i & 1 ? b : a, n);
			__asm__ volatile("" : : "r"(a), "r"(b) : "memory");
		}
		double s = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
		printf("memcpy %zu KB: %.2f GB/s\n", n >> 10, (double)reps * n / s / 1e9);
	}
	return 0;
}
