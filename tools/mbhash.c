/*
 * Print a 64-bit FNV-1a hash of every 1 MiB block of a file or block device,
 * in one sequential pass ("INDEX HASH" per line). Used to find which parts
 * of userdata differ from an image without copying either.
 *
 *   aarch64-linux-gnu-gcc -O2 -static -o mbhash-arm64 tools/mbhash.c   (phone)
 *   gcc -O2 -o mbhash tools/mbhash.c                                   (host)
 *   mbhash /dev/block/sda18 16384 > phone.txt     # optional block count
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BLK (1u << 20)

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE [BLOCKS]\n", argv[0]);
		return 2;
	}
	long max = argc > 2 ? atol(argv[2]) : -1;
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	uint8_t *buf = malloc(BLK);
	if (buf == NULL) {
		return 1;
	}
	for (long i = 0; max < 0 || i < max; i++) {
		size_t got = 0;
		while (got < BLK) {
			ssize_t n = read(fd, buf + got, BLK - got);
			if (n <= 0) {
				break;
			}
			got += (size_t)n;
		}
		if (got == 0) {
			break;
		}
		uint64_t h = 0xcbf29ce484222325ull;
		const uint64_t *w = (const uint64_t *)buf;
		for (size_t k = 0; k < got / 8; k++) {   /* 8 bytes per step: FNV over words */
			h ^= w[k];
			h *= 0x100000001b3ull;
		}
		printf("%ld %016llx\n", i, (unsigned long long)h);
		if (got < BLK) {
			break;
		}
	}
	return 0;
}
