/*
 * sapphire-part: the split of LUN 0 (sda) between iOS and Android, as the dual boot has it.
 *
 * The phone's own layout ends in userdata. The dual boot makes that space two partitions, back to back:
 *   iosroot   the old userdata entry renamed (its GUIDs kept), from where userdata started
 *   userdata  a new entry (same type GUID, a new unique GUID) right after, to the old end
 * The iOS kernel finds its root by GPT name (boot-arg ufsroot=iosroot); Android finds userdata by name.
 *
 *   sapphire-part DEV info            iosroot_bytes= userdata_bytes= sector= (KEY=value lines)
 *   sapphire-part DEV split BYTES     iosroot made BYTES (MiB-aligned), userdata the rest; both GPT copies
 *                                     rewritten (backup first). Android's data is lost: userdata is
 *                                     formatted after, once the recovery has restarted.
 * Exit 0 (info); 1 on an error (nothing written); 3: written (the recovery must restart to see it).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sweet_slot.h"

#define MIN_IOS     (16ull << 30)
#define MIN_ANDROID (12ull << 30)

static int fd;
static uint32_t ss;                     /* logical sector size */

static uint64_t le64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void
fail(const char *what)
{
	printf("ERROR %s\n", what);
	exit(1);
}

static void
rd(uint64_t lba, void *buf, size_t n)
{
	if (pread(fd, buf, n, (off_t)(lba * ss)) != (ssize_t)n) {
		fail("cannot read the partition table");
	}
}

static void
wr(uint64_t lba, const void *buf, size_t n)
{
	if (pwrite(fd, buf, n, (off_t)(lba * ss)) != (ssize_t)n) {
		int e = errno, ro = -1;
		ioctl(fd, BLKROGET, &ro);
		printf("write failed: BLKROGET=%d\n", ro);
		fflush(stdout);
		system("for f in /sys/block/sda/ro /sys/block/sda/size /sys/block/sda/device/model; do echo \"$f=$(cat $f 2>&1)\"; done; "
		       "dmesg 2>/dev/null | grep -iE 'write.?protect|read-only|ro=|sda' | tail -8");
		printf("ERROR write at LBA %llu: %s\n", (unsigned long long)lba, strerror(e));
		exit(2);
	}
}

static int
named(const uint8_t *e, const char *name)
{
	for (int i = 0; i < 36; i++) {
		uint16_t c = (uint16_t)(e[56 + 2 * i] | e[57 + 2 * i] << 8);
		if (c != (uint8_t)name[i]) {
			return 0;
		}
		if (!c) {
			return 1;
		}
	}
	return 1;
}

static void
set_name(uint8_t *e, const char *name)
{
	memset(e + 56, 0, 72);
	for (int i = 0; name[i] && i < 36; i++) {
		e[56 + 2 * i] = (uint8_t)name[i];
	}
}

static void
header_fix(uint8_t *h, uint32_t ents_crc)
{
	put32(h + 88, ents_crc);
	put32(h + 16, 0);
	put32(h + 16, sweet_slot_crc32(h, le32(h + 12)));
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: sapphire-part DEV info | DEV split BYTES\n");
		return 2;
	}
	int split = !strcmp(argv[2], "split");
	if (split && argc < 4) {
		fail("split needs BYTES");
	}
	fd = open(argv[1], split ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		fail("cannot open the disk");
	}
	if (ioctl(fd, BLKSSZGET, &ss) || (ss != 512 && ss != 4096)) {
		ss = 4096;
	}
	uint8_t hdr[4096], bhdr[4096];
	struct sweet_slot_gpt g, bg;
	rd(1, hdr, ss);
	if (sweet_slot_check_header(hdr, 1, &g)) {
		fail("the primary GPT header is not valid");
	}
	size_t eb = (size_t)g.count * 128;
	size_t esec = (eb + ss - 1) / ss * ss;
	uint8_t *ents = calloc(1, esec), *bents = calloc(1, esec);
	rd(g.entries_lba, ents, esec);
	if (sweet_slot_crc32(ents, eb) != g.entries_crc) {
		fail("the primary GPT entries are damaged");
	}
	rd(g.alt_lba, bhdr, ss);
	if (sweet_slot_check_header(bhdr, g.alt_lba, &bg) || bg.count != g.count) {
		fail("the backup GPT header is not valid");
	}
	rd(bg.entries_lba, bents, esec);
	if (sweet_slot_crc32(bents, eb) != bg.entries_crc || memcmp(ents, bents, eb)) {
		fail("the two GPT copies differ");
	}
	int ios = -1, ud = -1, free_slot = -1;
	for (uint32_t i = 0; i < g.count; i++) {
		uint8_t *e = ents + i * 128;
		int empty = 1;
		for (int k = 0; k < 16; k++) {
			empty &= e[k] == 0;
		}
		if (empty) {
			if (free_slot < 0) {
				free_slot = (int)i;
			}
		} else if (named(e, "iosroot")) {
			ios = (int)i;
		} else if (named(e, "userdata")) {
			ud = (int)i;
		}
	}
	if (ud < 0) {
		fail("no userdata partition");
	}
	uint8_t *U = ents + ud * 128, *I = ios >= 0 ? ents + ios * 128 : NULL;
	uint64_t us = le64(U + 32), ue = le64(U + 40);
	uint64_t is = I ? le64(I + 32) : 0, ie = I ? le64(I + 40) : 0;
	if (I && ie + 1 != us) {
		fail("iosroot and userdata are not back to back");
	}
	if (!split) {
		printf("sector=%u\n", ss);
		printf("iosroot_bytes=%llu\n", I ? (unsigned long long)((ie - is + 1) * ss) : 0ull);
		printf("iosroot_index=%d\n", ios);
		printf("userdata_bytes=%llu\n", (unsigned long long)((ue - us + 1) * ss));
		printf("userdata_index=%d\n", ud);
		return 0;
	}

	uint64_t want = strtoull(argv[3], NULL, 10);
	uint64_t start = I ? is : us, end = ue;           /* the space shared */
	uint64_t mib = (1u << 20) / ss;
	uint64_t n = (want / ss + mib - 1) / mib * mib;
	uint64_t space = end - start + 1;
	if (n * ss < MIN_IOS || n >= space || (space - n) * ss < MIN_ANDROID) {
		fail("that split leaves too little room for iOS or Android");
	}
	if (!I) {
		if (free_slot < 0) {
			fail("no free GPT entry for the new userdata");
		}
		/* the old userdata entry becomes iosroot; a new userdata entry after it */
		uint8_t *N = ents + free_slot * 128;
		memcpy(N, U, 128);
		int r = open("/dev/urandom", O_RDONLY);
		if (r < 0 || read(r, N + 16, 16) != 16) {
			fail("no random GUID");
		}
		close(r);
		N[16 + 7] = (uint8_t)((N[16 + 7] & 0x0f) | 0x40);  /* version 4 (mixed-endian field 3) */
		N[16 + 8] = (uint8_t)((N[16 + 8] & 0x3f) | 0x80);
		set_name(U, "iosroot");
		I = U;
		U = N;
		ios = ud;
		ud = free_slot;
	}
	put64(I + 32, start);
	put64(I + 40, start + n - 1);
	put64(U + 32, start + n);
	put64(U + 40, end);

	uint32_t crc = sweet_slot_crc32(ents, eb);
	header_fix(hdr, crc);
	header_fix(bhdr, crc);
	/*
	 * The recovery leaves the whole disk read-only (BLKROGET 1: writes fail with EPERM, partitions are
	 * writable): made writable for these writes, as `blockdev --setrw` does, and put back after.
	 */
	int ro = 0, rw = 0;
	ioctl(fd, BLKROGET, &ro);
	printf("sapphire-part 2: disk read-only flag %d\n", ro);
	if (ro) {
		if (ioctl(fd, BLKROSET, &rw) != 0) {
			printf("ERROR the disk is read-only and cannot be made writable (%s)\n", strerror(errno));
			return 1;
		}
		printf("disk was read-only: made writable for the partition table\n");
	}
	/* the backup copy first: a cut in the middle leaves one good copy the bootloader repairs from */
	wr(bg.entries_lba, ents, esec);
	wr(g.alt_lba, bhdr, ss);
	fsync(fd);
	wr(g.entries_lba, ents, esec);
	wr(1, hdr, ss);
	fsync(fd);
	if (ro) {
		ioctl(fd, BLKROSET, &ro);
	}
	printf("STEP iOS %llu MB, Android %llu MB\n", (unsigned long long)(n * ss >> 20), (unsigned long long)((end - start + 1 - n) * ss >> 20));

	/* the kernel keeps the old extents, and the by-name links come from ueventd: the recovery restarts */
	printf("REBOOT\n");
	return 3;
}
