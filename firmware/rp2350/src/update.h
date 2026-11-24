#ifndef _UPDATE_H_
#define _UPDATE_H_

/*
 * Updates of the mod chip's own firmware over the network: POST /update with
 * a file made by tools/sign_update.py. It is signed (ECDSA P-256) with the
 * key whose public half was built in, for this board, and not older than the
 * last one accepted. It goes into the slot that isn't running (the RP2350's
 * A/B partitions); the boot ROM then starts it once. Only when it has proved
 * itself does update_second() keep it: else, or if it crashes or hangs before
 * that, the next start is the old one's.
 *
 * Boards flashed without the partition table don't have slots, and a build
 * without a key takes no updates. update.c is the portable part, the slots
 * are update_flash.c's.
 *
 * The file: a header of 128 + a signature of 64 bytes, then the image.
 * Numbers are little endian.
 *	0  "MCUPD1\0\0"		8  board, 56 version: text, NUL padded
 *	88 sequence number	92 image length		96 image's SHA-256
 *	128 signature of the first 128 bytes, r and s
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_HDR	192
#define UPDATE_SECTOR	4096

/* The same states and calls as upload.h's */
bool update_begin(uint32_t content_length);
size_t update_feed(const uint8_t *p, size_t n);
bool update_finish(void);
int update_state(void);
const char *update_error(void);
uint32_t update_body_left(void);
void update_abort(void);

bool update_ready(void);		/* staged: update_commit() starts it */
void update_commit(void);
void update_boot(void);			/* after store_load() */
void update_second(bool healthy);
/* Keep the running one now, without waiting */
bool update_keep(void);
size_t update_status(char *buf, size_t max);
size_t update_json(char *buf, size_t max);

/* ---- update_flash.c, or the test ---- */

/* What this build is and trusts */
extern const char *const update_board, *const update_version;
extern const uint8_t update_key[65];
extern const bool update_has_key;

struct update_slots {
	bool ab;			/* the flash has two slots */
	uint8_t running;		/* 0: A, 1: B */
	bool trial;			/* started once, not kept yet */
	uint32_t other_size;
};
void update_slots(struct update_slots *s);
uint32_t update_own_seq(void);		/* the running image's number, from its IMAGE_DEF */
/* In the other slot; off and n multiples of a sector */
bool update_flash_write(uint32_t off, const uint8_t *p);	/* one sector, erased first */
bool update_flash_erase(uint32_t off);
void update_flash_read(uint32_t off, uint8_t *p, uint32_t n);
void update_flash_start(void);		/* does not return */
bool update_flash_keep(void);

#endif
