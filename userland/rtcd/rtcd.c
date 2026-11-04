/*
 * rtcd: the wall clock across reboots. The kernel starts at 1970: nothing reads the PMIC's RTC.
 * The PM6125 RTC (SID 0, peripheral 0x6000, RTC_READ 0x6048-0x604b) counts seconds whether the
 * phone is on or off, but not from 1970 (it is not set: Android keeps an offset in time_daemon).
 * rtcd does the same: at start, a clock before 2020 is set to RTC + the saved offset; then every
 * 5 minutes a clock that looks right (set by timed from the network, or by hand) refreshes the
 * offset in /var/root/rtc_offset. The RTC goes through kern.sweet_spmi (op 1 read).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#define OFFSET_PATH "/var/root/rtc_offset"
#define Y2020       1577836800L

static int
pmic_read(unsigned sid, unsigned addr, uint8_t *out)
{
	uint64_t v = (1ull << 56) | ((uint64_t)sid << 48) | ((uint64_t)addr << 32);
	if (sysctlbyname("kern.sweet_spmi", NULL, NULL, &v, sizeof v) != 0) return -1;
	uint64_t r = 0;
	size_t n = sizeof r;
	if (sysctlbyname("kern.sweet_spmi", &r, &n, NULL, 0) != 0) return -1;
	*out = (uint8_t)r;
	return 0;
}

/* RTC seconds, read twice until two reads agree (the low byte may roll over between bytes) */
static int
rtc_seconds(uint32_t *out)
{
	for (int t = 0; t < 5; t++) {
		uint8_t a[4], b[4];
		for (int i = 0; i < 4; i++) if (pmic_read(0, 0x6048 + i, &a[i])) return -1;
		for (int i = 0; i < 4; i++) if (pmic_read(0, 0x6048 + i, &b[i])) return -1;
		if (memcmp(a, b, 4) == 0) {
			*out = (uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24;
			return 0;
		}
	}
	return -1;
}

static int
load_offset(long *off)
{
	FILE *f = fopen(OFFSET_PATH, "r");
	if (!f) return -1;
	int ok = fscanf(f, "%ld", off) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static void
save_offset(long off)
{
	FILE *f = fopen(OFFSET_PATH ".new", "w");
	if (!f) return;
	fprintf(f, "%ld\n", off);
	fclose(f);
	rename(OFFSET_PATH ".new", OFFSET_PATH);
}

int
main(int argc, char **argv)
{
	uint32_t rtc;
	long off;
	time_t now = time(NULL);
	if (rtc_seconds(&rtc) != 0) {
		fprintf(stderr, "rtcd: no RTC through kern.sweet_spmi\n");
		return 1;
	}
	if (now < Y2020 && load_offset(&off) == 0 && (long)rtc + off > Y2020) {
		struct timeval tv = { (time_t)((long)rtc + off), 0 };
		if (settimeofday(&tv, NULL) == 0) {
			fprintf(stderr, "rtcd: clock set from the RTC: %ld\n", (long)tv.tv_sec);
		}
	}
	if (argc > 1 && strcmp(argv[1], "--once") == 0) {
		now = time(NULL);
		if (now > Y2020 && rtc_seconds(&rtc) == 0) save_offset((long)now - (long)rtc);
		printf("rtc %u, clock %ld, offset %ld\n", rtc, (long)now, (long)now - (long)rtc);
		return 0;
	}
	long saved = load_offset(&off) == 0 ? off : 0;
	for (;;) {
		now = time(NULL);
		if (now > Y2020 && rtc_seconds(&rtc) == 0) {
			long o = (long)now - (long)rtc;
			if (labs(o - saved) > 2) {
				save_offset(o);
				saved = o;
			}
		}
		sleep(300);
	}
}
