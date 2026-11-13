/*
 * cpufreq: the effective clock of the core this runs on, from a chain of dependent adds (1 cycle each on
 * Cortex-A73/A53): N adds in T seconds = N/T Hz. Runs a few rounds; with "bg" at background priority
 * (where kern.sweet_sched sends it: the A53s), else default (UI cores).
 */
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/qos.h>

static double
round_ghz(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t x = 1, n = 400000000;
	uint64_t t0 = mach_absolute_time();
	for (uint64_t i = 0; i < n; i++) {
		__asm__ volatile("add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1" : "+r"(x));
	}
	uint64_t t1 = mach_absolute_time();
	double s = (double)(t1 - t0) * tb.numer / tb.denom / 1e9;
	return (double)n * 4 / s / 1e9;
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "bg") == 0) {
		pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
	} else if (argc > 1 && strcmp(argv[1], "ui") == 0) {
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	}
	for (int r = 0; r < 4; r++) {
		printf("%.2f GHz\n", round_ghz());
	}
	return 0;
}
