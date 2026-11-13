/*
 * sapphire-inboxcat: the inbox after iOS's volume, read through the kernel (kern.sweet_inbox, patch 0310).
 *
 *   sapphire-inboxcat OFF LEN      LEN bytes from OFF (bytes from the partition's start) to stdout
 *   sapphire-inboxcat start        where the inbox header is: S = the volume's end (statfs of /) rounded up
 *                                  to 1 MiB, + 1 MiB (installer/recovery/inbox.c, IpaInbox.java)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define MIB 1048576ull

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "start") == 0) {
		struct statfs sf;
		if (statfs("/", &sf) != 0) {
			perror("statfs /");
			return 1;
		}
		uint64_t v = (uint64_t)sf.f_blocks * sf.f_bsize;
		printf("%llu\n", (unsigned long long)((v + MIB - 1) / MIB * MIB + MIB));
		return 0;
	}
	if (argc != 3) {
		fprintf(stderr, "usage: sapphire-inboxcat OFF LEN | start\n");
		return 2;
	}
	uint64_t off = strtoull(argv[1], NULL, 0), len = strtoull(argv[2], NULL, 0);
	char *buf = malloc(MIB);
	if (buf == NULL) {
		return 1;
	}
	while (len > 0) {
		size_t n = len < MIB ? (size_t)len : (size_t)MIB;
		size_t got = n;
		if (sysctlbyname("kern.sweet_inbox", buf, &got, &off, sizeof(off)) != 0) {
			fprintf(stderr, "kern.sweet_inbox at %llu: %s\n", (unsigned long long)off, strerror(errno));
			return 1;
		}
		if (got == 0 || fwrite(buf, 1, got, stdout) != got) {
			fprintf(stderr, "short read at %llu\n", (unsigned long long)off);
			return 1;
		}
		off += got;
		len -= got;
	}
	return fflush(stdout) == 0 ? 0 : 1;
}
