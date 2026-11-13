/*
 * test_slot.c: sweet_slot_switch() against what the phone's own bootloader wrote. Inputs (8 sectors of the
 * head and 8 of the tail of sde, from backup-persist/gpt): BEFORE with slot b active, ABL as the ABL left it
 * after `fastboot set_active a`. Switching BEFORE to a must give ABL's entries, and ABL to b BEFORE's, but
 * for the boot_a/boot_b retry and successful bits (the phone booted in between and counted them).
 *   cc -O1 -o test_slot test_slot.c sweet_slot.c && ./test_slot DIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_slot.h"

#define S 4096
#define TAIL0 524280ull                 /* first LBA of the tail file */

struct img {
	uint8_t head[8 * S], tail[8 * S];
};

static void
load(struct img *m, const char *dir, const char *which)
{
	char p[512];
	FILE *f;
	snprintf(p, sizeof(p), "%s/sde-head-%s.bin", dir, which);
	if (!(f = fopen(p, "rb")) || fread(m->head, 1, sizeof(m->head), f) != sizeof(m->head)) {
		perror(p), exit(2);
	}
	fclose(f);
	snprintf(p, sizeof(p), "%s/sde-tail-%s.bin", dir, which);
	if (!(f = fopen(p, "rb")) || fread(m->tail, 1, sizeof(m->tail), f) != sizeof(m->tail)) {
		perror(p), exit(2);
	}
	fclose(f);
}

/* entry name ASCII */
static void
nm(const uint8_t *e, char *o)
{
	int i;
	for (i = 0; i < 36 && e[56 + 2 * i]; i++) {
		o[i] = (char)e[56 + 2 * i];
	}
	o[i] = 0;
}

static int
run(struct img *from, struct img *want, char slot)
{
	struct sweet_slot_gpt g;
	if (sweet_slot_check_header(from->head + S, 1, &g)) {
		printf("bad header\n");
		return 1;
	}
	uint64_t bl = g.alt_lba;
	uint8_t *ents = from->head + g.entries_lba * S, *bhdr = from->tail + (bl - TAIL0) * S;
	struct sweet_slot_gpt bg;
	sweet_slot_check_header(bhdr, bl, &bg);
	uint8_t *bents = from->tail + (bg.entries_lba - TAIL0) * S;
	int r = sweet_slot_switch(from->head + S, ents, bhdr, bents, bl, slot, false);
	if (r) {
		printf("switch to %c: error %d\n", slot, r);
		return 1;
	}
	struct sweet_slot_gpt c;
	if (sweet_slot_check_header(from->head + S, 1, &c) || sweet_slot_check_header(bhdr, bl, &c) ||
	    sweet_slot_crc32(ents, g.count * 128) != c.entries_crc || memcmp(ents, bents, g.count * 128)) {
		printf("switch to %c: result fails its own checks\n", slot);
		return 1;
	}
	const uint8_t *w = want->head + g.entries_lba * S;
	int bad = 0, soft = 0;
	for (uint32_t i = 0; i < g.count; i++) {
		if (!memcmp(ents + i * 128, w + i * 128, 128)) {
			continue;
		}
		char n[37];
		nm(ents + i * 128, n);
		uint64_t am, aw;
		memcpy(&am, ents + i * 128 + 48, 8);
		memcpy(&aw, w + i * 128 + 48, 8);
		bool only_attr = !memcmp(ents + i * 128, w + i * 128, 48) && !memcmp(ents + i * 128 + 56, w + i * 128 + 56, 72);
		/* allowed: boot_* differing only in priority (48-49), retry (51-53) and successful (54) */
		uint64_t mask = (7ull << 51) | (1ull << 54) | (3ull << 48);   /* + priority */
		if (only_attr && !strncmp(n, "boot_", 5) && (am & ~mask) == (aw & ~mask)) {
			soft++;
			printf("  %-12s boot bits only: mine %016llx bootloader %016llx\n", n, (unsigned long long)am, (unsigned long long)aw);
		} else {
			bad++;
			printf("  %-12s DIFFERS (%s): mine %016llx bootloader %016llx\n", n, only_attr ? "attributes" : "GUID/LBA/name",
			    (unsigned long long)am, (unsigned long long)aw);
		}
	}
	printf("switch to %c: %d entries differ, %d only in boot priority/retry/successful bits -> %s\n", slot, bad + soft, soft,
	    bad ? "FAIL" : "PASS");
	return bad != 0;
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	static struct img before, abl, before2, abl2;
	load(&before, dir, "before-dualboot");
	load(&abl, dir, "abl-active-a");
	before2 = before;
	abl2 = abl;
	int f = run(&before, &abl2, 'a');
	f |= run(&abl, &before2, 'b');
	/* switching twice must come back to the start, CRCs included, but for the boot bits */
	struct img rt = before2, abl3 = abl2;
	f |= run(&rt, &abl3, 'a');
	f |= run(&rt, &before2, 'b');

	/* sweet_slot_mark_good on the bootloader's GPT (slot a active): only boot_a's attributes may change */
	struct img m = abl2;
	struct sweet_slot_gpt g, bg;
	sweet_slot_check_header(m.head + S, 1, &g);
	uint8_t *ents = m.head + g.entries_lba * S, *bhdr = m.tail + (g.alt_lba - TAIL0) * S;
	sweet_slot_check_header(bhdr, g.alt_lba, &bg);
	uint8_t *bents = m.tail + (bg.entries_lba - TAIL0) * S;
	int wrong = sweet_slot_mark_good(m.head + S, ents, bhdr, bents, g.alt_lba, 'b');
	int r = sweet_slot_mark_good(m.head + S, ents, bhdr, bents, g.alt_lba, 'a');
	int changed = 0, other = 0;
	for (uint32_t i = 0; i < g.count; i++) {
		if (memcmp(ents + i * 128, abl2.head + g.entries_lba * S + i * 128, 128)) {
			char n[37];
			nm(ents + i * 128, n);
			changed++;
			other += strcmp(n, "boot_a") != 0 || memcmp(ents + i * 128, abl2.head + g.entries_lba * S + i * 128, 48) != 0;
			uint64_t at;
			memcpy(&at, ents + i * 128 + 48, 8);
			printf("  mark a: %s now %016llx\n", n, (unsigned long long)at);
		}
	}
	struct sweet_slot_gpt c;
	int ok = wrong == SWEET_SLOT_EUNCLEAR && (r == 0 || r == SWEET_SLOT_ALREADY) && other == 0 &&
	    !sweet_slot_check_header(m.head + S, 1, &c) && sweet_slot_crc32(ents, g.count * 128) == c.entries_crc &&
	    !memcmp(ents, bents, g.count * 128);
	printf("mark good: inactive slot refused (%d), active slot %d, %d entries changed, %d unexpected -> %s\n",
	    wrong, r, changed, other, ok ? "PASS" : "FAIL");
	f |= !ok;
	return f;
}
