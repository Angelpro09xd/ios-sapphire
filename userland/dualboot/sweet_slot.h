/* sweet_slot.h: Qualcomm A/B slot switch on a GPT held in memory (see sweet_slot.c). */
#ifndef SWEET_SLOT_H
#define SWEET_SLOT_H

#ifdef KERNEL
#include <sys/types.h>
#include <stdbool.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define SWEET_SLOT_MAX_ENTRIES  256

enum {
	SWEET_SLOT_EINVAL = 1,          /* slot is not 'a' or 'b' */
	SWEET_SLOT_EBADHDR,             /* not a GPT header, or one with odd fields */
	SWEET_SLOT_EHDRCRC,             /* header CRC wrong */
	SWEET_SLOT_EENTCRC,             /* entry array CRC wrong */
	SWEET_SLOT_EMISMATCH,           /* the two copies differ */
	SWEET_SLOT_EUNCLEAR,            /* boot_a/boot_b do not say which slot is active */
	SWEET_SLOT_EPAIRS,              /* too few X_a/X_b pairs: not the A/B LUN */
	SWEET_SLOT_ALREADY,             /* sweet_slot_mark_good: nothing to do */
};

/*
 * boot_SLOT marked successful (7 retries, not unbootable) when SLOT is the active one -- what Android's
 * update_verifier/boot HAL markBootSuccessful does for Android; the bootloader then stops counting the
 * slot's retries down. 0, SWEET_SLOT_ALREADY, or an error (nothing changed).
 */
int sweet_slot_mark_good(uint8_t *hdr, uint8_t *ents, uint8_t *bhdr, uint8_t *bents, uint64_t backup_lba, char slot);

struct sweet_slot_gpt {
	uint64_t alt_lba, entries_lba;
	uint32_t count, size, entries_crc;
};

uint32_t sweet_slot_crc32(const uint8_t *p, size_t n);
int sweet_slot_check_header(const uint8_t *hdr, uint64_t my_lba, struct sweet_slot_gpt *g);
/* 'a', 'b', or -1 */
int sweet_slot_active(const uint8_t *ents, uint32_t n);
/*
 * HDR/BHDR: the primary (LBA 1) and backup (BACKUP_LBA) headers, a sector each; ENTS/BENTS: the two entry
 * arrays (count * 128 bytes). Checked, then changed in place to make SLOT active; SUCCESSFUL also marks
 * boot_SLOT successful (its retries are then not counted down). 0 or SWEET_SLOT_E*; nothing is changed on
 * an error. Already active: only boot_SLOT's attributes are refreshed.
 */
int sweet_slot_switch(uint8_t *hdr, uint8_t *ents, uint8_t *bhdr, uint8_t *bents, uint64_t backup_lba,
    char slot, bool successful);

#endif
