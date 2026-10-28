/*
 * hfsgrow: the root HFS+ volume grown, mounted, to the size of its partition.
 *
 * The iOS root went onto userdata (sda18, 115 GB) as a 16 GiB image, so iOS saw 16 GiB (Settings: "17 GB")
 * and the rest of the partition lay unused. XNU's HFS grows a mounted volume (fsctl HFSIOC_RESIZE_VOLUME
 * -> hfs_extendfs); the partition's last blocks stay outside it: the kernel's persistent console log
 * (sweet_ufs.c, SWEET_LOG_BLOCKS = 1025 x 4 KiB at the end of the partition), and a margin.
 *
 *   hfsgrow          what it would do
 *   hfsgrow -y       do it
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#define HFSIOC_RESIZE_VOLUME _IOW('h', 2, u_int64_t)    /* bsd/sweet_hfs/core/hfs_fsctl.h */
#define DKIOCGETBLOCKSIZE    _IOR('d', 24, uint32_t)    /* sys/disk.h (not in the iOS SDK) */
#define DKIOCGETBLOCKCOUNT   _IOR('d', 25, uint64_t)
extern int fsctl(const char *, unsigned long, void *, unsigned int);

#define LOG_BYTES   (1025ull * 4096)                    /* the kernel's console ring at the partition's end */
#define MARGIN      (64ull << 20)

int
main(int argc, char **argv)
{
	struct statfs st;
	uint64_t count = 0;
	uint32_t bsize = 0;

	if (statfs("/", &st) != 0) {
		perror("statfs /");
		return 1;
	}
	int fd = open("/dev/rufsroot0", O_RDONLY);
	if (fd < 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) != 0) {
		perror("/dev/rufsroot0");
		return 1;
	}
	close(fd);
	uint64_t part = count * bsize, now = (uint64_t)st.f_blocks * st.f_bsize;
	uint64_t want = (part - LOG_BYTES - MARGIN) / st.f_bsize * st.f_bsize;
	printf("partition %llu bytes (%.1f GiB), volume %llu bytes (%.1f GiB), allocation block %u\n",
	    (unsigned long long)part, part / 1073741824.0, (unsigned long long)now, now / 1073741824.0, (unsigned)st.f_bsize);
	printf("new volume size %llu bytes (%.1f GiB)\n", (unsigned long long)want, want / 1073741824.0);
	if (want <= now) {
		printf("nothing to grow\n");
		return 0;
	}
	if (argc < 2 || strcmp(argv[1], "-y") != 0) {
		printf("(dry run: hfsgrow -y to do it)\n");
		return 0;
	}
	sync();
	if (fsctl("/", HFSIOC_RESIZE_VOLUME, &want, 0) != 0) {
		printf("resize: %s\n", strerror(errno));
		return 1;
	}
	statfs("/", &st);
	printf("done: volume now %.1f GiB, %.1f GiB free\n", (double)st.f_blocks * st.f_bsize / 1073741824.0,
	    (double)st.f_bavail * st.f_bsize / 1073741824.0);
	return 0;
}
