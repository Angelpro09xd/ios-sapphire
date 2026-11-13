/*
 * hidtouch: synthetic one-finger touches and button presses, for testing animations without a
 * hand on the phone.
 *
 *   hidtouch swipe X0 Y0 X1 Y1 [MS]    a drag from (X0, Y0) to (X1, Y1) in MS milliseconds (300)
 *   hidtouch tap X Y
 *   hidtouch press KEY                 0 power, 1 volume up, 2 volume down, 3 home
 *
 * Coordinates are fractions of the panel (0..1, top left 0 0). They go to the kernel's sysctl
 * kern.sweet_inject (xnu 0237), which hands them to the touch driver's own path (SweetHID.cpp):
 * to backboardd they are the panel's touches, edge bit and all. (Dispatching made-up events to
 * backboardd's event system from here did nothing.) Root only.
 */
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#define PANEL_W 1080
#define PANEL_H 2400

static int
inject(const char *what)
{
	if (sysctlbyname("kern.sweet_inject", NULL, NULL, (void *)what, strlen(what) + 1) != 0) {
		perror("hidtouch: kern.sweet_inject");
		return -1;
	}
	return 0;
}

static int
contact(int down, double x, double y)
{
	char what[64];

	snprintf(what, sizeof(what), "t 0 %d %d %d", down, (int)(x * (PANEL_W - 1)), (int)(y * (PANEL_H - 1)));
	return inject(what);
}

/*
 * A sample every 4 ms on an absolute grid, as the touch driver reads the IC with a finger down (250 a
 * second): the drag takes MS, whatever each injection costs. (It was 8 ms of sleep after each sample, which
 * with the sysctl's own time made 13.6 ms a sample, 74 a second: a drag measured at most 64 frames a second
 * because no more touches came.)
 */
#define STEP_US 4000

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

static void
sleep_until(uint64_t t)
{
	uint64_t now = now_us();
	if (t > now) {
		usleep((useconds_t)(t - now));
	}
}

static int
drag(double x0, double y0, double x1, double y1, unsigned ms)
{
	unsigned steps = ms * 1000 / STEP_US ? ms * 1000 / STEP_US : 1;
	uint64_t t0 = now_us();

	if (contact(1, x0, y0) != 0) {
		return -1;
	}
	for (unsigned i = 1; i <= steps; i++) {
		sleep_until(t0 + (uint64_t)i * STEP_US);
		contact(1, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
	}
	sleep_until(t0 + (uint64_t)(steps + 1) * STEP_US);
	return contact(0, x1, y1);
}

int
main(int argc, char **argv)
{
	char what[64];

	if (argc >= 6 && strcmp(argv[1], "swipe") == 0) {
		return drag(atof(argv[2]), atof(argv[3]), atof(argv[4]), atof(argv[5]),
		    argc > 6 ? (unsigned)atoi(argv[6]) : 300) != 0;
	}
	if (argc == 4 && strcmp(argv[1], "tap") == 0) {
		return drag(atof(argv[2]), atof(argv[3]), atof(argv[2]), atof(argv[3]), 60) != 0;
	}
	if (argc == 3 && strcmp(argv[1], "press") == 0) {
		snprintf(what, sizeof(what), "k %d 1", atoi(argv[2]));
		if (inject(what) != 0) {
			return 1;
		}
		usleep(100000);
		snprintf(what, sizeof(what), "k %d 0", atoi(argv[2]));
		return inject(what) != 0;
	}
	fprintf(stderr, "usage: hidtouch swipe X0 Y0 X1 Y1 [MS] | tap X Y | press KEY   (fractions of the panel;\n"
	    "       KEY 0 power, 1 volume up, 2 volume down, 3 home)\n");
	return 2;
}
