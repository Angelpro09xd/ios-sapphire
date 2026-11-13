/*
 * hfsshrink: the root HFS+ volume shrunk, mounted, to a size in GiB (to make room for Android's userdata
 * in the same partition, which is then split in two). fsctl HFSIOC_RESIZE_VOLUME -> hfs_truncatefs, which
 * moves whatever lies past the new end inside the volume first.
 *
 *   hfsshrink GIB        what it would do
 *   hfsshrink GIB -y     do it
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#define HFSIOC_RESIZE_VOLUME _IOW('h', 2, u_int64_t)    /* bsd/sweet_hfs/core/hfs_fsctl.h */
extern int fsctl(const char *, unsigned long, void *, unsigned int);

int
main(int argc, char **argv)
{
	struct statfs st;

	if (argc < 2 || atoi(argv[1]) <= 0) {
		fprintf(stderr, "usage: hfsshrink GIB [-y]\n");
		return 2;
	}
	if (statfs("/", &st) != 0) {
		perror("statfs /");
		return 1;
	}
	uint64_t now = (uint64_t)st.f_blocks * st.f_bsize, used = (uint64_t)(st.f_blocks - st.f_bfree) * st.f_bsize;
	uint64_t want = ((uint64_t)atoi(argv[1]) << 30) / st.f_bsize * st.f_bsize;
	printf("volume %llu bytes (%.1f GiB), used %.1f GiB, allocation block %u\n", (unsigned long long)now,
	    now / 1073741824.0, used / 1073741824.0, (unsigned)st.f_bsize);
	printf("new volume size %llu bytes (%.1f GiB)\n", (unsigned long long)want, want / 1073741824.0);
	if (want >= now) {
		printf("nothing to shrink\n");
		return 0;
	}
	if (want < used + (4ull << 30)) {
		printf("refused: less than 4 GiB would stay free\n");
		return 1;
	}
	if (argc < 3 || strcmp(argv[2], "-y") != 0) {
		printf("(dry run: hfsshrink %s -y to do it)\n", argv[1]);
		return 0;
	}
	sync();
	if (fsctl("/", HFSIOC_RESIZE_VOLUME, &want, 0) != 0) {
		printf("resize: %s\n", strerror(errno));
		return 1;
	}
	sync();
	statfs("/", &st);
	printf("done: volume now %llu bytes (%.1f GiB), %.1f GiB free\n", (unsigned long long)st.f_blocks * st.f_bsize,
	    (double)st.f_blocks * st.f_bsize / 1073741824.0, (double)st.f_bavail * st.f_bsize / 1073741824.0);
	return 0;
}
